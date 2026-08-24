param(
    [string]$SourceRoot,
    [string]$BuildRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# Match the resolved leaf name rather than one exact spelling.  Debug
# configurations and older SDKs append a `d` suffix, while authoring graphs
# may expose the same import libraries through a `*lib` target name.
$ForbiddenDependencyPatterns = @(
    '^d3d8(?:d|lib)?$',
    '^d3dx8[a-z0-9_]*$',
    '^(?:miles|milesd|milesstub|milesstubd)$',
    '^(?:bink|binkd|binkstub|binkstubd)$',
    '^rts_d3d8libd?$'
)

function Normalize-DependencyToken {
    param([AllowNull()][string]$Token)

    if ([string]::IsNullOrWhiteSpace($Token)) {
        return ''
    }

    $value = $Token.Trim().Trim('"').Trim("'").Trim().TrimEnd(';', ',')
    if ([string]::IsNullOrWhiteSpace($value)) {
        return ''
    }

    # Generated Ninja and MSBuild commands contain a mixture of slash styles,
    # quoted absolute paths, and optional linker prefixes.  Only compare the
    # final library name, so rts_d3d8_headers remains distinct from the
    # forbidden D3D8 import/library spellings.
    $value = $value.Replace('\', '/')
    # A library-search directory is not a dependency.  Do not compare its
    # final directory component (for example, an SDK path named `d3d8`).
    if ($value -match '^(?:/|-)(?:LIBPATH:)') {
        return ''
    }
    $value = $value -replace '^/(?:WHOLEARCHIVE:|DEFAULTLIB:|LIBPATH:)', ''
    $leaf = ($value -split '/')[-1]
    $leaf = $leaf -replace '\.(?:lib|a|dll|obj|exe)$', ''
    return $leaf.ToLowerInvariant()
}

function Get-CommandDependencyTokens {
    param([AllowNull()][string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return @()
    }

    $tokens = @()
    # Keep quoted paths together, while treating whitespace and semicolons as
    # command/list delimiters.  The normalized leaf comparison is exact.
    $matches = [regex]::Matches($Text, '"[^"]*"|''[^'']*''|[^\s;]+')
    foreach ($match in $matches) {
        $normalized = Normalize-DependencyToken $match.Value
        if (-not [string]::IsNullOrWhiteSpace($normalized)) {
            $tokens += $normalized
        }
    }
    return @($tokens)
}

function Read-ClosureContent {
    param([Parameter(Mandatory = $true)][string]$Content)

    $target = ''
    $dependencies = @()
    foreach ($line in ($Content -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        if ($line -match '^target=(?<target>[^\r\n]+)$') {
            if (-not [string]::IsNullOrWhiteSpace($target)) {
                throw 'The runtime regression closure contains multiple target records.'
            }
            $target = $Matches.target.Trim()
            continue
        }

        if ($line -match '^dependency=(?<dependency>[^\r\n]+)$') {
            $dependencies += $Matches.dependency.Trim()
            continue
        }

        throw "The runtime regression closure contains an invalid record: '$line'."
    }

    if ([string]::IsNullOrWhiteSpace($target)) {
        throw 'The runtime regression closure has no target record.'
    }

    return [pscustomobject]@{
        Target = $target
        Dependencies = @($dependencies)
    }
}

function Assert-NoForbiddenDependencies {
    param(
        [Parameter(Mandatory = $true)][string[]]$Dependencies,
        [Parameter(Mandatory = $true)][string]$Context
    )

    foreach ($dependency in $Dependencies) {
        $normalized = Normalize-DependencyToken $dependency
        if (@($ForbiddenDependencyPatterns | Where-Object {
                    $normalized -match $_
                }).Count -gt 0) {
            throw "$Context retains forbidden dependency '$normalized'."
        }
    }
}

function Assert-RequiredDependencies {
    param(
        [Parameter(Mandatory = $true)][string[]]$Dependencies,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $normalizedDependencies = @($Dependencies | ForEach-Object {
            Normalize-DependencyToken $_
        })
    foreach ($required in @(
            'core_debug',
            'core_profile_legacy',
            'rts_legacy_product_runtime',
            'z_gameengine',
            'z_gameenginedevice',
            'zi_always')) {
        if ($normalizedDependencies -notcontains $required) {
            throw "$Context is missing required dependency '$required'."
        }
    }
}

function Get-CurrentBuildFiles {
    param(
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][string]$Filter
    )

    $resolvedRoot = (Resolve-Path -LiteralPath $BuildRoot).Path.TrimEnd('\')
    $files = @(Get-ChildItem -LiteralPath $BuildRoot -Filter $Filter -Recurse -File -ErrorAction SilentlyContinue)
    foreach ($file in $files) {
        $directory = $file.Directory
        $isNestedBuild = $false
        while ($null -ne $directory) {
            $directoryPath = $directory.FullName.TrimEnd('\')
            if ($directoryPath.Length -le $resolvedRoot.Length) {
                break
            }

            if (Test-Path -LiteralPath (Join-Path $directoryPath 'CMakeCache.txt')) {
                $isNestedBuild = $true
                break
            }
            $directory = $directory.Parent
        }

        if (-not $isNestedBuild) {
            $file
        }
    }
}

function Test-NativeX64CacheText {
    param([Parameter(Mandatory = $true)][string]$CacheText)

    $cacheLines = @($CacheText -split "`r?`n")
    return @($cacheLines | Where-Object {
            $_ -match '^\s*CMAKE_SIZEOF_VOID_P(?::[^=]+)?=8\s*$'
        }).Count -gt 0
}

function Get-GeneratedLinkCommandTexts {
    param([Parameter(Mandatory = $true)][string]$BuildRoot)

    $texts = @()

    # Ninja writes the resolved libraries in the target's linker build block.
    # Search every generated configuration graph because Ninja Multi-Config
    # may place the active command in build-<Config>.ninja or an impl file.
    $ninjaFiles = @(Get-CurrentBuildFiles $BuildRoot '*.ninja')
    foreach ($ninjaFile in $ninjaFiles) {
        $lines = @(Get-Content -LiteralPath $ninjaFile.FullName)
        for ($index = 0; $index -lt $lines.Count; $index++) {
            $line = $lines[$index]
            if ($line -notmatch '(?i)^\s*build\s+.*z_runtime_regression_tests\.exe\s*:\s*CXX_EXECUTABLE_LINKER') {
                continue
            }

            $block = @($line)
            for ($next = $index + 1; $next -lt $lines.Count; $next++) {
                if ($lines[$next] -match '^\s') {
                    $block += $lines[$next]
                    continue
                }
                break
            }

            $blockText = $block -join "`n"
            if ($blockText -match '(?im)^\s*LINK_LIBRARIES\s*=') {
                $texts += $blockText
            }
        }
    }

    # Single-config Ninja/Makefile generators can leave a target-specific
    # link.txt instead of a Ninja variable block.
    $linkFiles = @(Get-CurrentBuildFiles $BuildRoot 'link.txt' |
            Where-Object { $_.FullName -match '(?i)z_runtime_regression_tests' })
    foreach ($linkFile in $linkFiles) {
        $texts += (Get-Content -LiteralPath $linkFile.FullName -Raw)
    }

    # Visual Studio generators resolve the equivalent data in vcxproj
    # AdditionalDependencies entries.  Read all configurations so a Debug
    # or Release graph cannot hide a forbidden transitive dependency.
    $projectFiles = @(Get-CurrentBuildFiles $BuildRoot 'z_runtime_regression_tests.vcxproj')
    foreach ($projectFile in $projectFiles) {
        [xml]$project = Get-Content -LiteralPath $projectFile.FullName -Raw
        $dependencyNodes = $project.SelectNodes("//*[local-name()='AdditionalDependencies']")
        foreach ($dependencyNode in $dependencyNodes) {
            if (-not [string]::IsNullOrWhiteSpace($dependencyNode.InnerText)) {
                $texts += $dependencyNode.InnerText
            }
        }
    }

    return @($texts)
}

function Remove-CMakeLineComment {
    param([AllowNull()][string]$Line)

    if ([string]::IsNullOrEmpty($Line)) {
        return ''
    }

    # The contracts audited here do not place a '#' inside a quoted command
    # argument.  Removing comments before parsing keeps an unrelated comment
    # from satisfying a source-contract assertion.
    $commentIndex = $Line.IndexOf('#')
    if ($commentIndex -ge 0) {
        return $Line.Substring(0, $commentIndex)
    }
    return $Line
}

function Get-CMakeControlCommands {
    param([Parameter(Mandatory = $true)][string]$Content)

    $lines = @($Content -split "`r?`n")
    $commands = @()
    $lineIndex = 0
    while ($lineIndex -lt $lines.Count) {
        $code = Remove-CMakeLineComment $lines[$lineIndex]
        if ($code.Trim() -notmatch '^(?<name>if|elseif|else|endif)\s*\(') {
            $lineIndex++
            continue
        }

        $name = $Matches.name.ToLowerInvariant()
        $startLine = $lineIndex
        $endLine = $lineIndex
        $text = ''
        $parenthesisDepth = 0
        for ($cursor = $lineIndex; $cursor -lt $lines.Count; $cursor++) {
            $fragment = Remove-CMakeLineComment $lines[$cursor]
            $text += ' ' + $fragment.Trim()
            $parenthesisDepth += ([regex]::Matches($fragment, '\(')).Count
            $parenthesisDepth -= ([regex]::Matches($fragment, '\)')).Count
            $endLine = $cursor
            if ($parenthesisDepth -le 0) {
                break
            }
        }

        if ($parenthesisDepth -ne 0) {
            throw "Unbalanced CMake control command beginning at line $($startLine + 1)."
        }

        $commands += [pscustomobject]@{
            Name = $name
            Text = $text.Trim()
            StartLine = $startLine
            EndLine = $endLine
        }
        $lineIndex = $endLine + 1
    }

    return @($commands)
}

function Get-CMakeIfBlock {
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$ConditionPattern,
        [Parameter(Mandatory = $true)][string]$Context,
        [AllowEmptyString()][string]$RequiredBodyPattern = ''
    )

    $lines = @($Content -split "`r?`n")
    $commands = @(Get-CMakeControlCommands $Content)
    for ($index = 0; $index -lt $commands.Count; $index++) {
        $start = $commands[$index]
        if ($start.Name -ne 'if' -or $start.Text -notmatch $ConditionPattern) {
            continue
        }

        $depth = 1
        $elseCommand = $null
        $endCommand = $null
        for ($cursor = $index + 1; $cursor -lt $commands.Count; $cursor++) {
            $command = $commands[$cursor]
            if ($command.Name -eq 'if') {
                $depth++
                continue
            }
            if ($command.Name -eq 'endif') {
                $depth--
                if ($depth -eq 0) {
                    $endCommand = $command
                    break
                }
                continue
            }
            if ($depth -eq 1 -and ($command.Name -eq 'else' -or $command.Name -eq 'elseif') -and $null -eq $elseCommand) {
                $elseCommand = $command
            }
        }

        if ($null -eq $endCommand) {
            throw "CMake block '$Context' has no matching endif()."
        }

        $bodyStart = $start.EndLine + 1
        $bodyEnd = if ($null -ne $elseCommand) { $elseCommand.StartLine - 1 } else { $endCommand.StartLine - 1 }
        $bodyLines = if ($bodyStart -le $bodyEnd) { @($lines[$bodyStart..$bodyEnd]) } else { @() }

        $elseLines = @()
        if ($null -ne $elseCommand) {
            $elseStart = $elseCommand.EndLine + 1
            $elseEnd = $endCommand.StartLine - 1
            if ($elseStart -le $elseEnd) {
                $elseLines = @($lines[$elseStart..$elseEnd])
            }
        }

        $bodyText = ($bodyLines -join "`n")
        if (-not [string]::IsNullOrWhiteSpace($RequiredBodyPattern) -and
                $bodyText -notmatch $RequiredBodyPattern) {
            # A file may contain more than one condition with the same
            # pointer-size expression.  Continue searching until the block
            # containing the contract's target command is found.
            continue
        }

        return [pscustomobject]@{
            StartCommand = $start
            ElseCommand = $elseCommand
            EndCommand = $endCommand
            BodyLines = @($bodyLines)
            BodyText = $bodyText
            ElseBodyLines = @($elseLines)
            ElseBodyText = ($elseLines -join "`n")
        }
    }

    throw "CMake block '$Context' was not found with the required exact condition."
}

function Get-CMakeTargetLinkCommandTexts {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines
    )

    $commands = @()
    $lineIndex = 0
    while ($lineIndex -lt $Lines.Count) {
        $code = Remove-CMakeLineComment $Lines[$lineIndex]
        if ($code.Trim() -notmatch '^target_link_libraries\s*\(') {
            $lineIndex++
            continue
        }

        $text = ''
        $parenthesisDepth = 0
        $endLine = $lineIndex
        for ($cursor = $lineIndex; $cursor -lt $Lines.Count; $cursor++) {
            $fragment = Remove-CMakeLineComment $Lines[$cursor]
            $text += ' ' + $fragment.Trim()
            $parenthesisDepth += ([regex]::Matches($fragment, '\(')).Count
            $parenthesisDepth -= ([regex]::Matches($fragment, '\)')).Count
            $endLine = $cursor
            if ($parenthesisDepth -le 0) {
                break
            }
        }

        if ($parenthesisDepth -ne 0) {
            throw "Unbalanced target_link_libraries() command beginning at line $($lineIndex + 1)."
        }
        $commandText = $text.Trim()
        $commandText = $commandText -replace '^target_link_libraries\s*\(\s*', ''
        $commandText = $commandText -replace '\s*\)\s*$', ''
        $commands += $commandText.Trim()
        $lineIndex = $endLine + 1
    }
    return @($commands)
}

function Assert-ExactElseBranch {
    param(
        [Parameter(Mandatory = $true)]$Block,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($null -eq $Block.ElseCommand -or
            $Block.ElseCommand.Name -ne 'else' -or
            $Block.ElseCommand.Text -notmatch '^else\s*\(\s*\)$') {
        throw "$Context must use an exact else() native branch."
    }
}

function Assert-RegistryDiscoveryContract {
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][string]$RegistryPrefix
    )

    $conditionPattern = '^if\(\s*"\$\{CMAKE_HOST_SYSTEM\}"\s+MATCHES\s+"Windows"\s+AND\s+"\$\{CMAKE_SYSTEM\}"\s+MATCHES\s+"Windows"\s+AND\s+CMAKE_SIZEOF_VOID_P\s+EQUAL\s+4\s*\)$'
    $registryBodyPattern = '(?is)include\s*\(\s*\$\{CMAKE_SOURCE_DIR\}/cmake/registry\.cmake\s*\).*?fetch_registry_value\s*\([^)]*\b' +
        [regex]::Escape($RegistryPrefix) + '\b'
    $registryBlock = Get-CMakeIfBlock $Content $conditionPattern $Context $registryBodyPattern

    $contentLines = @($Content -split "`r?`n")
    $guardedFetchCount = 0
    for ($lineIndex = 0; $lineIndex -lt $contentLines.Count; $lineIndex++) {
        $code = (Remove-CMakeLineComment $contentLines[$lineIndex]).Trim()
        if ($code -notmatch '^fetch_registry_value\s*\(') {
            continue
        }

        if ($lineIndex -lt $registryBlock.StartCommand.StartLine -or
                $lineIndex -gt $registryBlock.EndCommand.StartLine) {
            throw "Registry discovery block '$Context' has an unguarded fetch_registry_value call at line $($lineIndex + 1)."
        }
        $guardedFetchCount++
    }

    if ($guardedFetchCount -eq 0) {
        throw "Registry discovery block '$Context' contains no registry fetches."
    }

    if ($registryBlock.BodyText -match '(?is)fetch_registry_value\s*\([^)]*\bRTS_INSTALL_PREFIX_(?:GENERALS|ZEROHOUR)\b') {
        throw "Registry discovery block '$Context' writes directly to a user install prefix."
    }
}

function Assert-InstallPrefixContract {
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [Parameter(Mandatory = $true)][string]$EffectivePrefix,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][string]$MainTarget,
        [Parameter(Mandatory = $true)][string]$LauncherTarget,
        [switch]$RequireRuntimeRegressionInstall
    )

    $prefixPattern = '^if\(\s*' + [regex]::Escape($EffectivePrefix) + '\s*\)$'
    $mainInstallPattern = '(?im)^\s*install\s*\(\s*TARGETS\s+' + [regex]::Escape($MainTarget) + '\b'
    $prefixBlock = Get-CMakeIfBlock $Content $prefixPattern "$Context install prefix" $mainInstallPattern

    $launcherConditionPattern = '^if\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+8\s+AND\s+TARGET\s+' +
        [regex]::Escape($LauncherTarget) + '\s*\)$'
    $launcherInstallPattern = '(?im)^\s*install\s*\(\s*TARGETS\s+' + [regex]::Escape($LauncherTarget) + '\b'
    $launcherBlock = Get-CMakeIfBlock $prefixBlock.BodyText $launcherConditionPattern `
        "$Context native launcher install" $launcherInstallPattern
    if ($launcherBlock.BodyText -notmatch ('(?im)^\s*install\s*\(\s*FILES\s+\$<TARGET_PDB_FILE:' +
                [regex]::Escape($LauncherTarget) + '>')) {
        throw "'$Context' does not install the $LauncherTarget PDB in its x64 target guard."
    }

    if ($RequireRuntimeRegressionInstall) {
        $runtimeInstallCondition = '^if\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+8\s+AND\s+TARGET\s+z_runtime_regression_tests\s*\)$'
        $runtimeInstallBlock = Get-CMakeIfBlock $prefixBlock.BodyText $runtimeInstallCondition "$Context runtime regression install"
        $runtimeInstallBody = $runtimeInstallBlock.BodyText
        if ($runtimeInstallBody -notmatch '(?im)^\s*install\s*\(\s*TARGETS\s+z_runtime_regression_tests\b') {
            throw "'$Context' does not install z_runtime_regression_tests inside its target/architecture guard."
        }
        if ($runtimeInstallBody -notmatch '(?im)^\s*install\s*\(\s*FILES\s+\$<TARGET_PDB_FILE:z_runtime_regression_tests>') {
            throw "'$Context' does not install the z_runtime_regression_tests PDB."
        }
    }
}

function Assert-RuntimeRegressionSourceContract {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    $cmakePath = Join-Path $SourceRoot 'GeneralsMD/Code/Tools/RuntimeRegressionTests/CMakeLists.txt'
    $runtimeCmake = Get-Content -LiteralPath $cmakePath -Raw
    $win32Condition = '^if\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+4\s*\)$'
    $win32Block = Get-CMakeIfBlock $runtimeCmake $win32Condition 'runtime regression Win32 lane'

    $win32LinkCommands = @(Get-CMakeTargetLinkCommandTexts $win32Block.BodyLines)
    $win32Dependencies = @($win32LinkCommands | ForEach-Object {
            Get-CommandDependencyTokens $_
        })
    foreach ($required in @('d3d8', 'd3dx8', 'milesstub')) {
        if ($win32Dependencies -notcontains $required) {
            throw "The legacy Win32 runtime regression lane is missing '$required'."
        }
    }

    $binkCondition = '^if\(\s*NOT\s+RTS_BUILD_OPTION_FFMPEG\s*\)$'
    $binkBlock = Get-CMakeIfBlock $win32Block.BodyText $binkCondition 'runtime regression Win32 Bink lane'
    $binkDependencies = @((Get-CMakeTargetLinkCommandTexts $binkBlock.BodyLines) | ForEach-Object {
            Get-CommandDependencyTokens $_
        })
    if ($binkDependencies -notcontains 'binkstub') {
        throw 'The legacy Win32 Bink compatibility condition does not link binkstub.'
    }

    Assert-ExactElseBranch $win32Block 'The runtime regression target'
    $x64LinkCommands = @(Get-CMakeTargetLinkCommandTexts $win32Block.ElseBodyLines)
    $x64Dependencies = @($x64LinkCommands | ForEach-Object {
            Get-CommandDependencyTokens $_
        })
    foreach ($required in @('rts_legacy_product_runtime', 'z_gameengine', 'z_gameenginedevice')) {
        if ($x64Dependencies -notcontains $required) {
            throw "The native x64 runtime regression lane is missing '$required'."
        }
    }
    Assert-NoForbiddenDependencies $x64Dependencies 'Native x64 runtime regression source link lane'
    if ($win32Block.ElseBodyText -notmatch '(?is)rts_d3d8_headers.*?header-only compatibility boundary') {
        throw 'The temporary x64 D3D8 header-only compatibility boundary is undocumented.'
    }
}

function Assert-RegistryMacroContract {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    $registryPath = Join-Path $SourceRoot 'cmake/registry.cmake'
    $registryMacro = Get-Content -LiteralPath $registryPath -Raw
    if ($registryMacro -match '(?i)\bRTS_[A-Z0-9_]+_REGISTRY_VALUE\b|unset\s*\(\s*RTS_INSTALL_PREFIX_') {
        throw 'The registry helper still contains superseded install-prefix marker or clear machinery.'
    }
    if ($registryMacro -notmatch '(?im)^\s*if\(\s*NOT\s+DEFINED\s+\$\{output_var\}\s+OR\s+"\$\{\$\{output_var\}\}"\s+STREQUAL\s+""\s*\)') {
        throw 'The registry helper does not preserve an existing explicit output value.'
    }
    if ($registryMacro -notmatch '(?im)^\s*set\(\s*\$\{output_var\}\s+"\$\{_variable\}"\s+CACHE\s+PATH\b') {
        throw 'The registry helper does not write the requested registry-only cache output.'
    }
}

function Assert-NativeLauncherBuildContract {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$TitleDirectory,
        [Parameter(Mandatory = $true)][string]$TitleOption,
        [Parameter(Mandatory = $true)][string]$LauncherTarget
    )

    $codePath = Join-Path $SourceRoot "$TitleDirectory/Code/CMakeLists.txt"
    $codeContent = Get-Content -LiteralPath $codePath -Raw
    $codeCondition = '^if\s*\(\s*RTS_BUILD_' + [regex]::Escape($TitleOption) +
        '_TOOLS\s+OR\s+RTS_BUILD_' + [regex]::Escape($TitleOption) +
        '_EXTRAS\s+OR\s+\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+8\s+AND\s+RTS_BUILD_PRODUCT\s*\)\s*\)$'
    Get-CMakeIfBlock $codeContent $codeCondition "$TitleDirectory native tools graph" '(?im)^\s*add_subdirectory\s*\(\s*Tools\s*\)' | Out-Null

    $toolsPath = Join-Path $SourceRoot "$TitleDirectory/Code/Tools/CMakeLists.txt"
    $toolsContent = Get-Content -LiteralPath $toolsPath -Raw
    $toolsCondition = '^if\s*\(\s*RTS_BUILD_' + [regex]::Escape($TitleOption) +
        '_EXTRAS\s+OR\s+\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+8\s+AND\s+RTS_BUILD_PRODUCT\s*\)\s*\)$'
    Get-CMakeIfBlock $toolsContent $toolsCondition "$TitleDirectory native launcher graph" `
        ('(?im)^\s*add_subdirectory\s*\(\s*Launcher\s*\)') | Out-Null
}

function Assert-SourceContracts {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    Assert-RuntimeRegressionSourceContract $SourceRoot
    Assert-RegistryMacroContract $SourceRoot

    $toolsCmake = Get-Content -LiteralPath (Join-Path $SourceRoot 'GeneralsMD/Code/Tools/CMakeLists.txt') -Raw
    if ($toolsCmake -notmatch '(?ms)^if\(RTS_BUILD_ZEROHOUR_EXTRAS\s+OR\s+\(CMAKE_SIZEOF_VOID_P\s+EQUAL\s+8\s+AND\s+RTS_BUILD_PRODUCT\)\)\s*add_subdirectory\(RuntimeRegressionTests\)\s*endif\(\)') {
        throw 'The Zero Hour runtime regression utility is not enabled for the native x64 product graph.'
    }
    Assert-NativeLauncherBuildContract $SourceRoot 'Generals' 'GENERALS' 'g_launcher'
    Assert-NativeLauncherBuildContract $SourceRoot 'GeneralsMD' 'ZEROHOUR' 'z_launcher'

    foreach ($installContract in @(
            @{ Path = 'Generals/CMakeLists.txt'; Prefix = 'RTS_INSTALL_PREFIX_GENERALS'; EffectivePrefix = '_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS'; RegistryPrefix = 'RTS_REGISTRY_INSTALL_PREFIX_GENERALS'; MainTarget = 'g_generals'; LauncherTarget = 'g_launcher' },
            @{ Path = 'GeneralsMD/CMakeLists.txt'; Prefix = 'RTS_INSTALL_PREFIX_ZEROHOUR'; EffectivePrefix = '_RTS_EFFECTIVE_INSTALL_PREFIX_ZEROHOUR'; RegistryPrefix = 'RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR'; MainTarget = 'z_generals'; LauncherTarget = 'z_launcher'; RequireRuntimeRegressionInstall = $true })) {
        $installCmake = Get-Content -LiteralPath (Join-Path $SourceRoot $installContract.Path) -Raw
        Assert-RegistryDiscoveryContract $installCmake $installContract.Path $installContract.RegistryPrefix
        $requireRuntime = [bool]$installContract.RequireRuntimeRegressionInstall
        if ($requireRuntime) {
            Assert-InstallPrefixContract $installCmake $installContract.EffectivePrefix $installContract.Path $installContract.MainTarget $installContract.LauncherTarget -RequireRuntimeRegressionInstall
        } else {
            Assert-InstallPrefixContract $installCmake $installContract.EffectivePrefix $installContract.Path $installContract.MainTarget $installContract.LauncherTarget
        }
    }
}

function Invoke-SelfTest {
    # Exercise first-position/transitive linker inputs, debug legacy-library
    # spellings, and structural source guards.  The source fixtures are
    # intentionally malformed in ways that the previous whole-file regexes
    # accepted.
    $allowedClosure = Read-ClosureContent @"
target=z_runtime_regression_tests
dependency=core_debug
dependency=rts_d3d8_headers
"@
    Assert-NoForbiddenDependencies $allowedClosure.Dependencies 'Self-test allowed closure'

    $firstPositionCaught = $false
    try {
        $firstForbidden = Read-ClosureContent @"
target=z_runtime_regression_tests
dependency=d3d8
dependency=core_debug
"@
        Assert-NoForbiddenDependencies $firstForbidden.Dependencies 'Self-test first-position fixture'
    }
    catch {
        $firstPositionCaught = $true
    }
    if (-not $firstPositionCaught) {
        throw 'Self-test failed to reject a first-position d3d8 dependency.'
    }

    $transitiveCaught = $false
    try {
        $transitiveForbidden = Read-ClosureContent @"
target=z_runtime_regression_tests
dependency=core_debug
dependency=legacy_audio_interface
dependency=milesstub
"@
        Assert-NoForbiddenDependencies $transitiveForbidden.Dependencies 'Self-test transitive fixture'
    }
    catch {
        $transitiveCaught = $true
    }
    if (-not $transitiveCaught) {
        throw 'Self-test failed to reject a transitive milesstub dependency.'
    }

    $actualFirstCaught = $false
    try {
        Assert-NoForbiddenDependencies (Get-CommandDependencyTokens 'LINK_LIBRARIES = d3d8.lib core_debug.lib') 'Self-test actual first-position fixture'
    }
    catch {
        $actualFirstCaught = $true
    }
    if (-not $actualFirstCaught) {
        throw 'Self-test failed to reject d3d8 in an actual linker command.'
    }

    $actualTransitiveCaught = $false
    try {
        Assert-NoForbiddenDependencies (Get-CommandDependencyTokens 'LINK_LIBRARIES = core_debug.lib C:/legacy/transitive/milesstub.lib') 'Self-test actual transitive fixture'
    }
    catch {
        $actualTransitiveCaught = $true
    }
    if (-not $actualTransitiveCaught) {
        throw 'Self-test failed to reject transitive milesstub in an actual linker command.'
    }

    Assert-NoForbiddenDependencies (Get-CommandDependencyTokens 'LINK_LIBRARIES = rts_d3d8_headers.lib core_debug.lib') 'Self-test allowed linker boundary'
    Assert-NoForbiddenDependencies (Get-CommandDependencyTokens 'LINK_FLAGS = /LIBPATH:C:/sdk/d3d8 LINK_LIBRARIES = core_debug.lib') 'Self-test allowed library search path'

    if (Test-NativeX64CacheText "UNRELATED_CMAKE_SIZEOF_VOID_P=8`nCMAKE_CXX_SIZEOF_DATA_PTR=4") {
        throw 'Self-test accepted an unrelated cache key as native x64 evidence.'
    }
    if (-not (Test-NativeX64CacheText 'CMAKE_SIZEOF_VOID_P:INTERNAL=8')) {
        throw 'Self-test rejected an exact native x64 cache entry.'
    }

    foreach ($variant in @('d3d8d', 'd3d8lib', 'd3dx8d', 'd3dx8math', 'milesstubd', 'binkstubd', 'rts_d3d8libd')) {
        $variantCaught = $false
        try {
            Assert-NoForbiddenDependencies @($variant) "Self-test forbidden variant '$variant'"
        }
        catch {
            $variantCaught = $true
        }
        if (-not $variantCaught) {
            throw "Self-test failed to reject forbidden legacy dependency variant '$variant'."
        }
    }

    $nestedArtifactRoot = Join-Path ([IO.Path]::GetTempPath()) ('rts-runtime-graph-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $nestedArtifactRoot -Force | Out-Null
        $nestedArtifact = Join-Path $nestedArtifactRoot 'Stage3FFmpegGraphAudit'
        New-Item -ItemType Directory -Path $nestedArtifact -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $nestedArtifact 'CMakeCache.txt') -Value '# nested build fixture'
        Set-Content -LiteralPath (Join-Path $nestedArtifactRoot 'z_runtime_regression_tests.vcxproj') -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup><Link><AdditionalDependencies>core_debug.lib</AdditionalDependencies></Link></ItemDefinitionGroup></Project>
'@
        Set-Content -LiteralPath (Join-Path $nestedArtifact 'z_runtime_regression_tests.vcxproj') -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup><Link><AdditionalDependencies>d3d8.lib;milesstub.lib</AdditionalDependencies></Link></ItemDefinitionGroup></Project>
'@
        $nestedArtifactTexts = @(Get-GeneratedLinkCommandTexts $nestedArtifactRoot)
        if ($nestedArtifactTexts.Count -ne 1) {
            throw 'Self-test did not exclude a nested CMake build artifact.'
        }
        Assert-NoForbiddenDependencies (Get-CommandDependencyTokens $nestedArtifactTexts[0]) 'Self-test current-build artifact scope'
    }
    finally {
        if (Test-Path -LiteralPath $nestedArtifactRoot) {
            Remove-Item -LiteralPath $nestedArtifactRoot -Recurse -Force
        }
    }

    $registryFixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('rts-registry-cache-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $registryFixtureRoot -Force | Out-Null
        $registryFixturePath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../../../cmake/registry.cmake')).Path.Replace('\', '/')
        $registryFixture = @"
cmake_minimum_required(VERSION 3.25)
set(RTS_INSTALL_PREFIX_FIXTURE "C:/same" CACHE PATH "explicit prefix")
function(cmake_host_system_information)
    set(_variable "C:/same" PARENT_SCOPE)
endfunction()
include("$registryFixturePath")
fetch_registry_value("fixture" "InstallPath" RTS_REGISTRY_INSTALL_PREFIX_FIXTURE "fixture")
if(NOT "`${RTS_INSTALL_PREFIX_FIXTURE}" STREQUAL "C:/same")
    message(FATAL_ERROR "The explicit prefix was changed by registry discovery.")
endif()
if(NOT "`${RTS_REGISTRY_INSTALL_PREFIX_FIXTURE}" STREQUAL "C:/same")
    message(FATAL_ERROR "The registry-only cache output was not populated.")
endif()
set(CMAKE_SIZEOF_VOID_P 8)
set(_effective_prefix "`${RTS_INSTALL_PREFIX_FIXTURE}")
if(NOT "`${_effective_prefix}" STREQUAL "C:/same")
    message(FATAL_ERROR "The same-valued explicit x64 prefix did not survive.")
endif()
unset(RTS_INSTALL_PREFIX_FIXTURE CACHE)
set(CMAKE_SIZEOF_VOID_P 4)
set(_effective_prefix "")
if(NOT RTS_INSTALL_PREFIX_FIXTURE)
    set(_effective_prefix "`${RTS_REGISTRY_INSTALL_PREFIX_FIXTURE}")
endif()
if(NOT "`${_effective_prefix}" STREQUAL "C:/same")
    message(FATAL_ERROR "The Win32 registry fallback was not selected.")
endif()
"@
        $registryFixturePathOnDisk = Join-Path $registryFixtureRoot 'registry_cache_fixture.cmake'
        Set-Content -LiteralPath $registryFixturePathOnDisk -Value $registryFixture -Encoding UTF8
        & cmake -P $registryFixturePathOnDisk 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw 'Self-test registry cache fixture failed.'
        }
    }
    finally {
        if (Test-Path -LiteralPath $registryFixtureRoot) {
            Remove-Item -LiteralPath $registryFixtureRoot -Recurse -Force
        }
    }

    $malformedRegistry = @'
if("${CMAKE_HOST_SYSTEM}" MATCHES "Windows")
    include(${CMAKE_SOURCE_DIR}/cmake/registry.cmake)
    fetch_registry_value("retail" "InstallPath" RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR "retail")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(STATUS "unrelated pointer-size condition")
endif()
'@
    $registryGuardCaught = $false
    try {
        Assert-RegistryDiscoveryContract $malformedRegistry 'Self-test malformed registry guard' 'RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR'
    }
    catch {
        $registryGuardCaught = $true
    }
    if (-not $registryGuardCaught) {
        throw 'Self-test accepted an unguarded registry block followed by an unrelated x86 condition.'
    }

    $extraRegistryFetch = @'
if("${CMAKE_HOST_SYSTEM}" MATCHES "Windows" AND "${CMAKE_SYSTEM}" MATCHES "Windows" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    include(${CMAKE_SOURCE_DIR}/cmake/registry.cmake)
    fetch_registry_value("retail" "InstallPath" RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR "retail")
endif()
fetch_registry_value("unrelated" "InstallPath" RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR "unrelated")
'@
    $extraRegistryCaught = $false
    try {
        Assert-RegistryDiscoveryContract $extraRegistryFetch 'Self-test extra registry fetch' 'RTS_REGISTRY_INSTALL_PREFIX_ZEROHOUR'
    }
    catch {
        $extraRegistryCaught = $true
    }
    if (-not $extraRegistryCaught) {
        throw 'Self-test accepted an extra unguarded registry fetch.'
    }

    $malformedRuntime = @'
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(z_runtime_regression_tests PRIVATE core_debug)
endif()
if(NOT RTS_BUILD_OPTION_FFMPEG)
    target_link_libraries(z_runtime_regression_tests PRIVATE binkstub)
endif()
else()
    target_link_libraries(z_runtime_regression_tests PRIVATE rts_legacy_product_runtime z_gameengine z_gameenginedevice)
endif()
'@
    $runtimeBranchCaught = $false
    try {
        $malformedRuntimeBlock = Get-CMakeIfBlock $malformedRuntime '^if\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+4\s*\)$' 'Self-test malformed runtime branch'
        if ($null -eq $malformedRuntimeBlock.ElseCommand) {
            throw 'missing native branch'
        }
    }
    catch {
        $runtimeBranchCaught = $true
    }
    if (-not $runtimeBranchCaught) {
        throw 'Self-test accepted an unrelated Bink block as the runtime target native branch.'
    }

    $elseifRuntime = @'
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(z_runtime_regression_tests PRIVATE core_debug)
elseif(RTS_BUILD_OPTION_FFMPEG)
    target_link_libraries(z_runtime_regression_tests PRIVATE rts_legacy_product_runtime z_gameengine z_gameenginedevice)
endif()
'@
    $elseifBranchCaught = $false
    try {
        $elseifBlock = Get-CMakeIfBlock $elseifRuntime '^if\(\s*CMAKE_SIZEOF_VOID_P\s+EQUAL\s+4\s*\)$' 'Self-test elseif runtime branch'
        Assert-ExactElseBranch $elseifBlock 'Self-test elseif runtime branch'
    }
    catch {
        $elseifBranchCaught = $true
    }
    if (-not $elseifBranchCaught) {
        throw 'Self-test accepted elseif(...) as the native x64 runtime branch.'
    }

    $decoyInstall = @'
if(_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS)
    message(STATUS "decoy install block")
endif()
if(_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS)
    install(TARGETS g_generals RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND TARGET g_launcher)
        install(TARGETS g_launcher RUNTIME DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}")
        install(FILES $<TARGET_PDB_FILE:g_launcher> DESTINATION "${_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS}" OPTIONAL)
    endif()
endif()
'@
    $selectedInstall = Get-CMakeIfBlock $decoyInstall '^if\(\s*_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS\s*\)$' `
        'Self-test anchored install block' '(?im)^\s*install\s*\(\s*TARGETS\s+g_generals\b'
    if ($selectedInstall.BodyText -notmatch '(?im)TARGETS\s+g_launcher\b') {
        throw 'Self-test selected a decoy install block instead of the target install block.'
    }

    $decoyOnlyCaught = $false
    try {
        Get-CMakeIfBlock @'
if(_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS)
    message(STATUS "decoy only")
endif()
'@ '^if\(\s*_RTS_EFFECTIVE_INSTALL_PREFIX_GENERALS\s*\)$' `
            'Self-test decoy-only install block' '(?im)^\s*install\s*\(\s*TARGETS\s+g_generals\b' | Out-Null
    }
    catch {
        $decoyOnlyCaught = $true
    }
    if (-not $decoyOnlyCaught) {
        throw 'Self-test accepted a decoy install block without the required target command.'
    }

    Write-Output 'Runtime regression graph audit self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot) -or [string]::IsNullOrWhiteSpace($BuildRoot)) {
    throw 'SourceRoot and BuildRoot are required unless -SelfTest is specified.'
}

$cachePath = Join-Path $BuildRoot 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw 'Runtime regression graph audit has no CMake cache.'
}
$cache = Get-Content -LiteralPath $cachePath -Raw

$pointerSizeConfigured = Test-NativeX64CacheText $cache
if (-not $pointerSizeConfigured) {
    $compilerFiles = @(
        Get-CurrentBuildFiles $BuildRoot 'CMakeCCompiler.cmake'
        Get-CurrentBuildFiles $BuildRoot 'CMakeCXXCompiler.cmake'
    )
    foreach ($compilerFile in $compilerFiles) {
        $compilerText = Get-Content -LiteralPath $compilerFile.FullName -Raw
        if ($compilerText -match '(?m)^\s*set\(CMAKE_(?:C|CXX)_SIZEOF_DATA_PTR\s+"8"\)\s*$') {
            $pointerSizeConfigured = $true
            break
        }
    }
}
if (-not $pointerSizeConfigured) {
    throw 'Runtime regression graph audit must run against a native x64 configure.'
}

$closurePath = Join-Path $BuildRoot 'z_runtime_regression_link_closure.txt'
if (-not (Test-Path -LiteralPath $closurePath)) {
    throw 'The configured runtime regression link closure is missing.'
}
$parsedClosure = Read-ClosureContent (Get-Content -LiteralPath $closurePath -Raw)
if ($parsedClosure.Target -ne 'z_runtime_regression_tests') {
    throw "The runtime regression closure names unexpected target '$($parsedClosure.Target)'."
}
Assert-RequiredDependencies $parsedClosure.Dependencies 'Native x64 runtime regression target'
Assert-NoForbiddenDependencies $parsedClosure.Dependencies 'Native x64 runtime regression target'

$linkCommands = @(Get-GeneratedLinkCommandTexts $BuildRoot)
if ($linkCommands.Count -eq 0) {
    throw 'The generated native x64 runtime regression linker command is missing; build z_runtime_regression_tests before running this audit.'
}
foreach ($linkCommand in $linkCommands) {
    Assert-NoForbiddenDependencies (Get-CommandDependencyTokens $linkCommand) 'Generated native x64 runtime regression linker command'
}

Assert-SourceContracts $SourceRoot
Write-Output 'Native x64 Zero Hour runtime regression graph and linker audits passed.'
