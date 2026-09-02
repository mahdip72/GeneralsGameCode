param(
    [string] $SourceRoot,
    [string] $BuildRoot,
    [string] $Configuration,
    [string[]] $Targets = @(
        'g_ww3d2',
        'z_ww3d2',
        'g_gameenginedevice',
        'z_gameenginedevice'
    ),
    [string[]] $MigrationTargets = @(
        'g_ww3d2',
        'z_ww3d2',
        'g_gameenginedevice',
        'z_gameenginedevice'
    ),
    [string[]] $AuthorityTargets = @(
        'core_renderer',
        'core_ww3d2_native'
    ),
    [string[]] $ProductTargets = @(
        'g_generals',
        'z_generals'
    ),
    [switch] $AuthorityOnly,
    [switch] $StrictFinal,
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'

# CTest expands a CMake list into separate command-line tokens, but PowerShell
# binds only the first token to a string-array parameter before the next named
# parameter.  Accept comma/semicolon-delimited values so the generated test
# cannot silently audit only the first target.
$Targets = @($Targets | ForEach-Object { $_ -split '[,;]' } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$MigrationTargets = @($MigrationTargets | ForEach-Object { $_ -split '[,;]' } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$AuthorityTargets = @($AuthorityTargets | ForEach-Object { $_ -split '[,;]' } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$ProductTargets = @($ProductTargets | ForEach-Object { $_ -split '[,;]' } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

$legacySourcePattern = '(?i)(?:^|[\\/])(?:dx8[^\\/]*|d3d11legacybridge|legacytexturecompat)\.(?:cpp|cxx|cc|h)$'
$legacyGraphPattern = '(?i)(?:BUILD_WITH_D3D8|rts_d3d8_headers|rts_d3d8lib|d3d8(?:\.lib|to9)|dx8wrapper|dx8webbrowser|d3d11legacybridge|(?:^|[\\/])dx8[^\\/]*\.(?:cpp|cxx|cc|obj))'
$legacyIncludePattern = '(?i)(?:[\\/]_deps[\\/]dx8-src(?:[\\/]|$|[\s"])|min-dx8-sdk|(?:^|[\\/])d3d8(?:types|caps)?\.h(?:$|[\s"<>])|(?:^|[\\/])dx8(?:wrapper|webbrowser)\.h(?:$|[\s"<>]))'

# Browser ownership is deliberately explicit in the inventory.  The neutral
# facade and native implementation must remain in the product tree, while
# the typed compatibility adapter is allowed only in the external x86 lane.
$nativeBrowserInventory = @(
    [pscustomobject]@{
        Path = 'Core/Libraries/Include/Renderer/GameWebBrowser.h'
        Role = 'neutral browser facade header'
    },
    [pscustomobject]@{
        Path = 'Core/Libraries/Source/WWVegas/WW3D2/gamewebbrowser.cpp'
        Role = 'native browser implementation'
    }
)
$legacyBrowserInventory = @(
    [pscustomobject]@{
        Path = 'Core/LegacyRenderer/WWVegas/WW3D2/dx8webbrowser.h'
        Role = 'x86 browser compatibility header'
    },
    [pscustomobject]@{
        Path = 'Core/LegacyRenderer/WWVegas/WW3D2/dx8webbrowser.cpp'
        Role = 'x86 browser compatibility adapter'
    }
)

# GameEngineDevice has the same paired source-selection contract.  Native x64
# targets compile the product implementations, while the external Win32 lane
# supplies the historical adapters.  Keep both sides in the audit inventory
# so a source list cannot silently regress to the wrong ABI.
$nativeGameEngineDeviceInventory = @(
    [pscustomobject]@{
        Path = 'Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DProfilerFrameCapture.cpp'
        Role = 'native profiler frame-capture implementation'
    },
    [pscustomobject]@{
        Path = 'Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DSnow.cpp'
        Role = 'native snow implementation'
    }
)
$legacyGameEngineDeviceInventory = @(
    [pscustomobject]@{
        Path = 'Core/LegacyRenderer/GameEngineDevice/Source/W3DDevice/GameClient/W3DProfilerFrameCaptureLegacy.cpp'
        Role = 'x86 profiler frame-capture compatibility adapter'
    },
    [pscustomobject]@{
        Path = 'Core/LegacyRenderer/GameEngineDevice/Source/W3DDevice/GameClient/W3DSnowLegacy.cpp'
        Role = 'x86 snow compatibility adapter'
    }
)

# The title WW3D2/GameEngineDevice graph still contains the compatibility
# facade while it is being ported.  AuthorityOnly therefore proves the
# native renderer/device ownership and final PE/link closure, while
# StrictFinal remains the hard migration gate over the complete title graph.
$legacySourceEdgePattern = '(?im)^\s*#\s*include\s*[<"](?:[^">]*[\\/]\s*)?(?:dx8wrapper|dx8webbrowser|d3d8(?:types|caps)?)\.h[>"]|\b(?:DX8Wrapper|IDirect3[A-Za-z]*8|D3D(?:FVF|TS|RS|TSS|LOCK|POOL|USAGE|PRIMITIVE|FORMAT|CAPS)8?)_[A-Za-z0-9_]*|\bD3D[A-Z0-9_]*8\b'

function Add-Finding {
    param(
        [System.Collections.Generic.List[string]] $Findings,
        [string] $Priority,
        [string] $Message
    )

    [void] $Findings.Add("[$Priority] $Message")
}

function Get-ConfiguredNativeTitleTargets {
    param(
        [string[]] $MigrationTargets,
        [string] $TargetSuffix
    )

    if ([string]::IsNullOrWhiteSpace($TargetSuffix)) {
        return @()
    }

    $targetPattern = '(?i)^(?:g|z)_' + [regex]::Escape($TargetSuffix) + '$'
    return @($MigrationTargets | Where-Object {
        $_ -match $targetPattern
    } | Select-Object -Unique)
}

function Test-EntryConfiguration {
    param(
        [psobject] $Entry,
        [string] $WantedConfiguration
    )

    if ([string]::IsNullOrWhiteSpace($WantedConfiguration)) {
        return $true
    }

    $text = "{0}`n{1}" -f [string] $Entry.file, [string] $Entry.command
    $configurationPattern = [regex]::Escape($WantedConfiguration)
    if ($text -match "(?i)CMAKE_INTDIR.{0,12}$configurationPattern") {
        return $true
    }
    if ($text -match "(?i)[\\/]$configurationPattern[\\/]") {
        return $true
    }

    # Single-configuration generators do not emit a configuration marker.
    # The caller detects a configured multi-config graph by the presence of
    # at least one marker and only uses this fallback when none are present.
    return $false
}

function Get-TargetEntries {
    param(
        [object[]] $Entries,
        [string] $Target,
        [string] $WantedConfiguration
    )

    $targetPattern = "(?i)CMakeFiles[\\/]" + [regex]::Escape("$Target.dir")
    $targetEntries = @($Entries | Where-Object {
        $text = "{0}`n{1}" -f [string] $_.file, [string] $_.command
        $text -match "(?i)$targetPattern"
    })
    if ($targetEntries.Count -eq 0) {
        return @()
    }

    if ([string]::IsNullOrWhiteSpace($WantedConfiguration)) {
        return $targetEntries
    }

    $configuredEntries = @($targetEntries | Where-Object {
        Test-EntryConfiguration -Entry $_ -WantedConfiguration $WantedConfiguration
    })
    if ($configuredEntries.Count -gt 0) {
        return $configuredEntries
    }

    # A single-configuration generator has no CMAKE_INTDIR/path marker.  Do
    # not silently select a different configuration when a multi-config graph
    # did emit markers: the caller's requested configuration must be real.
    $hasAnyConfigurationMarker = $targetEntries | Where-Object {
        $text = "{0}`n{1}" -f [string] $_.file, [string] $_.command
        $text -match '(?i)CMAKE_INTDIR|[\\/](?:Debug|Release|RelWithDebInfo|MinSizeRel)[\\/]'
    }
    if ($null -eq $hasAnyConfigurationMarker) {
        return $targetEntries
    }

    return @()
}

function Get-PchPaths {
    param([object[]] $Entries)

    $paths = [System.Collections.Generic.List[string]]::new()
    foreach ($entry in $Entries) {
        $command = [string] $entry.command
        $matches = [regex]::Matches($command, '(?i)(?:^|[\s"/])(?:/Y[uf]|/FI)?((?:[A-Za-z]:)?[^\s"]*cmake_pch\.hxx)')
        foreach ($match in $matches) {
            $path = $match.Groups[1].Value
            if (-not [string]::IsNullOrWhiteSpace($path) -and -not $paths.Contains($path)) {
                [void] $paths.Add($path)
            }
        }
    }
    return @($paths)
}

function Get-SourcePath {
    param(
        [string] $SourceRoot,
        [string] $Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    if ([IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $SourceRoot $Path
}

function Get-GraphFindings {
    param(
        [object[]] $Entries,
        [string[]] $TargetNames,
        [string] $SourceRoot,
        [string[]] $PchContents = @(),
        [string[]] $NinjaContents = @(),
        [switch] $SkipSourceRead,
        [switch] $SkipDirectSourceEdge
    )

    $findings = [System.Collections.Generic.List[string]]::new()
    foreach ($target in $TargetNames) {
        $targetEntries = @($Entries | Where-Object { $_.Target -eq $target })
        if ($targetEntries.Count -eq 0) {
            Add-Finding $findings 'P0' "generated compile graph has no entries for $target"
            continue
        }

        $sourceEntries = @($targetEntries | Where-Object {
            ([IO.Path]::GetFileName([string] $_.file)) -notmatch '(?i)^cmake_pch\.c(?:xx|pp)$'
        })
        $buildDefinitionEntries = @($targetEntries | Where-Object {
            ([string] $_.command) -match '(?i)BUILD_WITH_D3D8'
        })
        if ($buildDefinitionEntries.Count -gt 0) {
            Add-Finding $findings 'P0' "$target compile commands inherit BUILD_WITH_D3D8 (entries=$($buildDefinitionEntries.Count); first=$([string] $buildDefinitionEntries[0].file))"
        }
        $includeEntries = @($targetEntries | Where-Object {
            ([string] $_.command) -match $legacyIncludePattern
        })
        if ($includeEntries.Count -gt 0) {
            Add-Finding $findings 'P0' "$target compile commands expose a min-DX8 include/dependency edge (entries=$($includeEntries.Count); first=$([string] $includeEntries[0].file))"
        }

        $legacyObjects = @($sourceEntries | Where-Object {
            ([string] $_.file) -match $legacySourcePattern
        } | ForEach-Object { [string] $_.file } | Select-Object -Unique)
        foreach ($file in $legacyObjects) {
            Add-Finding $findings 'P0' "$target compiles a legacy renderer object: $file"
        }

        $directEdges = [System.Collections.Generic.List[string]]::new()
        foreach ($entry in $sourceEntries) {
            $file = [string] $entry.file
            if (-not $SkipSourceRead) {
                $sourcePath = Get-SourcePath -SourceRoot $SourceRoot -Path $file
                if (-not (Test-Path -LiteralPath $sourcePath)) {
                    Add-Finding $findings 'P1' "$target compile entry references a missing source file: $file"
                    continue
                }
                $sourceText = Get-Content -LiteralPath $sourcePath -Raw
                if (-not $SkipDirectSourceEdge -and
                        $sourceText -match $legacySourceEdgePattern -and
                        -not $directEdges.Contains($file)) {
                    [void] $directEdges.Add($file)
                }
            }
        }
        foreach ($file in $directEdges) {
            Add-Finding $findings 'P1' "$target source retains a direct D3D8/dx8wrapper edge: $file"
        }
    }

    $legacyPchCount = @($PchContents | Where-Object {
        $_ -match '(?i)dx8wrapper|d3d8(?:types|caps)?\.h'
    }).Count
    if ($legacyPchCount -gt 0) {
        Add-Finding $findings 'P0' "x64 renderer target PCH contains dx8wrapper or a D3D8 header (files=$legacyPchCount)"
    }

    $targetPattern = '(?i)CMakeFiles[\\/](?:' +
        (($TargetNames | ForEach-Object { [regex]::Escape($_ + '.dir') }) -join '|') + ')'
    $ninjaEvidence = [System.Collections.Generic.List[string]]::new()
    foreach ($ninja in $NinjaContents) {
        foreach ($line in ($ninja -split '\r?\n')) {
            if ($line -match $targetPattern -and $line -match $legacyGraphPattern) {
                if (-not $ninjaEvidence.Contains($line)) {
                    [void] $ninjaEvidence.Add($line)
                }
            }
        }
    }
    if ($ninjaEvidence.Count -gt 0) {
        Add-Finding $findings 'P0' "generated Ninja renderer targets contain legacy object/dependency edges (lines=$($ninjaEvidence.Count); first=$($ninjaEvidence[0]))"
    }

    return @($findings | Select-Object -Unique)
}

function Get-ProductLinkBlocks {
    param(
        [string[]] $NinjaContents,
        [string] $ProductTarget
    )

    $artifact = switch ($ProductTarget) {
        'g_generals' { 'generalsv.exe' }
        'z_generals' { 'generalszh.exe' }
        default { $null }
    }
    if ([string]::IsNullOrWhiteSpace($artifact)) {
        return @()
    }

    $blocks = [System.Collections.Generic.List[string]]::new()
    $artifactPattern = [regex]::Escape($artifact)
    foreach ($ninja in $NinjaContents) {
        $lines = @($ninja -split '\r?\n')
        for ($index = 0; $index -lt $lines.Count; $index++) {
            if ($lines[$index] -notmatch "(?i)^build\s+\S*${artifactPattern}:\s+CXX_EXECUTABLE_LINKER") {
                continue
            }
            $blockLines = [System.Collections.Generic.List[string]]::new()
            for ($end = $index; $end -lt $lines.Count; $end++) {
                [void] $blockLines.Add($lines[$end])
                # Ninja Multi-Config emits LINK_PATH for some CMake versions
                # and RSP_FILE as the final variable for others.  Stop at the
                # first terminal linker variable so a later test target's
                # legacy libraries cannot contaminate this product block.
                if ($lines[$end] -match '(?i)^\s*(?:LINK_PATH|RSP_FILE)\s*=') {
                    break
                }
            }
            [void] $blocks.Add(($blockLines -join "`n"))
        }
    }
    return @($blocks)
}

function Get-DumpbinPath {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return [string] $command.Source
    }

    $visualStudioRoots = @()
    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $visualStudioRoots += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio'
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $visualStudioRoots += Join-Path $env:ProgramFiles 'Microsoft Visual Studio'
    }
    foreach ($visualStudioRoot in $visualStudioRoots | Select-Object -Unique) {
        if (-not (Test-Path -LiteralPath $visualStudioRoot)) {
            continue
        }
        $candidate = Get-ChildItem -LiteralPath $visualStudioRoot -Filter 'dumpbin.exe' -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '(?i)[\\/]Hostx64[\\/]x64[\\/]dumpbin\.exe$' } |
            Sort-Object FullName |
            Select-Object -First 1
        if ($null -ne $candidate) {
            return [string] $candidate.FullName
        }
    }
    return $null
}

function Get-NativeAuthorityFindings {
    param(
        [string] $SourceRoot,
        [string] $BuildRoot,
        [string[]] $MigrationTargets,
        [string[]] $ProductTargets,
        [string[]] $NinjaContents,
        [object[]] $NativeEntries,
        [string] $Configuration
    )

    $findings = [System.Collections.Generic.List[string]]::new()
    $authorityForbiddenPattern = '(?i)(?:\bd3d8(?:\.lib|\.dll)?\b|\bd3dx8(?:\.lib|\.dll)?\b|[\\/]_deps[\\/]dx8-src|rts_d3d8lib|d3d11legacybridge|dx8webbrowser|(?:^|\s)dx8[^\s]*\.obj\b)'

    $expectedNativeSources = @{
        'core_renderer' = @(
            'RendererDevice.cpp',
            'D3D11RenderDevice.cpp',
            'NativeW3DRenderer.cpp',
            'NativeW3DResources.cpp',
            'NativeW3DRenderState.cpp'
        )
        'core_ww3d2_native' = @(
            'nativew3d2.cpp',
            'nativew3dbufferowner.cpp',
            'nativew3dsampledtexture.cpp',
            'nativew3dtextureowner.cpp'
        )
    }
    foreach ($nativeTarget in $expectedNativeSources.Keys) {
        $targetEntries = @($NativeEntries | Where-Object { $_.Target -eq $nativeTarget })
        if ($targetEntries.Count -eq 0) {
            Add-Finding $findings 'P0' "native authority compile graph has no entries for $nativeTarget"
            continue
        }
        $sourceNames = @($targetEntries | ForEach-Object {
            [IO.Path]::GetFileName([string] $_.file)
        })
        foreach ($expectedSource in $expectedNativeSources[$nativeTarget]) {
            if ($sourceNames -notcontains $expectedSource) {
                Add-Finding $findings 'P0' "native authority source inventory is incomplete for ${nativeTarget}: $expectedSource"
            }
        }
    }

    foreach ($inventoryEntry in @(
            $nativeBrowserInventory + $legacyBrowserInventory +
            $nativeGameEngineDeviceInventory + $legacyGameEngineDeviceInventory)) {
        $inventoryPath = Join-Path $SourceRoot $inventoryEntry.Path
        if (-not (Test-Path -LiteralPath $inventoryPath)) {
            Add-Finding $findings 'P0' "native/legacy renderer source inventory is incomplete: $($inventoryEntry.Role) ($($inventoryEntry.Path))"
        }
    }

    # The native facade is part of both title WW3D2 targets. Check the
    # generated graph explicitly so a product cannot silently fall back to
    # the external adapter merely because the neutral header remains present.
    $nativeWw3d2Targets = @(Get-ConfiguredNativeTitleTargets `
        -MigrationTargets $MigrationTargets `
        -TargetSuffix 'ww3d2')
    foreach ($titleTarget in $nativeWw3d2Targets) {
        $nativeBrowserPattern = '(?i)CMakeFiles[\\/]' + [regex]::Escape($titleTarget) +
            '\.dir[\\/].*gamewebbrowser\.cpp\.obj\b'
        $nativeBrowserEntries = @($NinjaContents | ForEach-Object {
            $_ -split '\r?\n'
        } | Where-Object {
            $_ -match $nativeBrowserPattern
        })
        if ($nativeBrowserEntries.Count -eq 0) {
            Add-Finding $findings 'P0' "native browser implementation is absent from the generated $titleTarget graph"
        }
    }

    # These paired GameEngineDevice sources must follow the architecture split
    # in both native title graphs.  The external adapter paths are checked by
    # inventory above; only native product objects may appear in x64 Ninja.
    $nativeGameEngineDeviceTargets = @(Get-ConfiguredNativeTitleTargets `
        -MigrationTargets $MigrationTargets `
        -TargetSuffix 'gameenginedevice')
    foreach ($titleTarget in $nativeGameEngineDeviceTargets) {
        foreach ($nativeDeviceSource in @('W3DProfilerFrameCapture.cpp', 'W3DSnow.cpp')) {
            $nativeDevicePattern = '(?i)CMakeFiles[\\/]' + [regex]::Escape($titleTarget) +
                '\.dir[\\/].*' + [regex]::Escape($nativeDeviceSource) + '\.obj\b'
            $nativeDeviceEntries = @($NinjaContents | ForEach-Object {
                $_ -split '\r?\n'
            } | Where-Object {
                $_ -match $nativeDevicePattern
            })
            if ($nativeDeviceEntries.Count -eq 0) {
                Add-Finding $findings 'P0' "native $nativeDeviceSource is absent from the generated $titleTarget graph"
            }
        }
    }

    $closurePaths = @(
        (Join-Path $BuildRoot 'native_product_runtime_link_closure.txt'),
        (Join-Path $BuildRoot 'product_runtime_selection.txt')
    )
    foreach ($closurePath in $closurePaths) {
        if (-not (Test-Path -LiteralPath $closurePath)) {
            Add-Finding $findings 'P1' "native runtime authority evidence is missing: $closurePath"
            continue
        }
        $closureText = Get-Content -LiteralPath $closurePath -Raw
        $linkLines = @($closureText -split '\r?\n' | Where-Object { $_ -match '^links=' })
        if ($linkLines.Count -eq 0) {
            Add-Finding $findings 'P1' "native runtime authority evidence has no links record: $closurePath"
            continue
        }
        foreach ($linkLine in $linkLines) {
            if ($linkLine -match $authorityForbiddenPattern) {
                Add-Finding $findings 'P0' "native runtime authority links a legacy D3D8 dependency: $closurePath ($linkLine)"
            }
        }
        if ($closurePath -eq $closurePaths[0]) {
            if (-not ($linkLines -match '(?i)\bd3d11(?:\.lib)?\b') -or
                    -not ($linkLines -match '(?i)\bdxgi(?:\.lib)?\b')) {
                Add-Finding $findings 'P0' "native runtime authority closure does not reach both d3d11 and dxgi: $closurePath"
            }
        }
        else {
            if (-not ($linkLines -match '(?i)\brts_native_product_runtime\b')) {
                Add-Finding $findings 'P0' "product runtime selection does not dispatch through rts_native_product_runtime: $closurePath"
            }
        }
    }

    $nativeClosurePath = Join-Path $BuildRoot 'native_product_runtime_link_closure.txt'
    if (Test-Path -LiteralPath $nativeClosurePath) {
        $nativeClosureText = Get-Content -LiteralPath $nativeClosurePath -Raw
        if ($nativeClosureText -notmatch '(?im)^requires_legacy_d3d8=OFF\s*$') {
            Add-Finding $findings 'P0' 'native product runtime does not publish requires_legacy_d3d8=OFF.'
        }
        if ($nativeClosureText -notmatch '(?im)^resource_closure_complete=ON\s*$') {
            Add-Finding $findings 'P0' 'native product runtime does not publish a complete resource closure.'
        }
    }

    foreach ($productTarget in $ProductTargets) {
        $blocks = @(Get-ProductLinkBlocks -NinjaContents $NinjaContents -ProductTarget $productTarget)
        if ($blocks.Count -eq 0) {
            Add-Finding $findings 'P1' "generated native product link command is missing: $productTarget"
            continue
        }
        foreach ($block in $blocks) {
            if ($block -match $authorityForbiddenPattern) {
                Add-Finding $findings 'P0' "native product link command retains a legacy D3D8 import/dependency: $productTarget"
            }
            if ($block -notmatch '(?i)core_renderer(?:\.lib)?') {
                Add-Finding $findings 'P0' "native product link command does not reach core_renderer: $productTarget"
            }
            if ($block -notmatch '(?i)\bd3d11(?:\.lib)?\b' -or
                    $block -notmatch '(?i)\bdxgi(?:\.lib)?\b') {
                Add-Finding $findings 'P0' "native product link command does not reach both d3d11 and dxgi: $productTarget"
            }
        }
    }

    $dumpbinPath = Get-DumpbinPath
    if ([string]::IsNullOrWhiteSpace($dumpbinPath)) {
        Add-Finding $findings 'P1' 'native product PE import audit cannot locate dumpbin.exe.'
    }
    else {
        $artifactConfiguration = if ([string]::IsNullOrWhiteSpace($Configuration)) {
            'Release'
        }
        else {
            $Configuration
        }
        foreach ($productTarget in $ProductTargets) {
            $relativeArtifact = switch ($productTarget) {
                'g_generals' { "Generals/${artifactConfiguration}/generalsv.exe" }
                'z_generals' { "GeneralsMD/${artifactConfiguration}/generalszh.exe" }
                default { $null }
            }
            if ([string]::IsNullOrWhiteSpace($relativeArtifact)) {
                continue
            }
            $artifactPath = Join-Path $BuildRoot $relativeArtifact
            if (-not (Test-Path -LiteralPath $artifactPath)) {
                Add-Finding $findings 'P1' "native product PE artifact is missing: $artifactPath"
                continue
            }
            $imports = (& $dumpbinPath /nologo /dependents $artifactPath 2>$null | Out-String)
            if ($LASTEXITCODE -ne 0) {
                Add-Finding $findings 'P1' "native product PE import audit failed: $artifactPath"
                continue
            }
            if ($imports -match $authorityForbiddenPattern) {
                Add-Finding $findings 'P0' "native product PE imports retain a legacy D3D8 dependency: $artifactPath"
            }
            if ($imports -notmatch '(?im)^\s*d3d11\.dll\s*$' -or
                    $imports -notmatch '(?im)^\s*dxgi\.dll\s*$') {
                Add-Finding $findings 'P0' "native product PE imports do not prove both d3d11.dll and dxgi.dll: $artifactPath"
            }
        }
    }

    return @($findings | Select-Object -Unique)
}

function Invoke-SelfTest {
    $cleanEntries = @(
        [pscustomobject]@{
            Target = 'native_renderer'
            file = 'Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.cpp'
            command = 'cl.exe /DWIN32 /ICore/Libraries/Include -c nativew3d2.cpp'
        }
    )
    $cleanFindings = @(Get-GraphFindings -Entries $cleanEntries -TargetNames @('native_renderer') -SkipSourceRead)
    if ($cleanFindings.Count -ne 0) {
        throw "clean native graph fixture unexpectedly failed: $($cleanFindings -join '; ')"
    }

    $badEntries = @(
        [pscustomobject]@{
            Target = 'native_renderer'
            file = 'Core/Libraries/Source/WWVegas/WW3D2/dx8renderer.cpp'
            command = 'cl.exe /DBUILD_WITH_D3D8 /IH:/build/_deps/dx8-src -c dx8renderer.cpp'
        }
    )
    $badFindings = @(Get-GraphFindings `
        -Entries $badEntries `
        -TargetNames @('native_renderer') `
        -PchContents @('#include "dx8wrapper.h"') `
        -NinjaContents @('build CMakeFiles/g_ww3d2.dir/dx8renderer.cpp.obj: CXX_COMPILER') `
        -SkipSourceRead)
    if ($badFindings.Count -lt 4) {
        Write-Output ($badFindings -join '; ')
        throw "legacy graph fixture did not expose all strict edges (findings=$($badFindings.Count))"
    }

    $browserCleanEntries = @(
        [pscustomobject]@{
            Target = 'g_ww3d2'
            file = 'Core/Libraries/Source/WWVegas/WW3D2/gamewebbrowser.cpp'
            command = 'cl.exe /ICore/Libraries/Include -c gamewebbrowser.cpp'
        }
    )
    $browserCleanFindings = @(Get-GraphFindings -Entries $browserCleanEntries -TargetNames @('g_ww3d2') -SkipSourceRead)
    if ($browserCleanFindings.Count -ne 0) {
        throw "neutral browser graph fixture unexpectedly failed: $($browserCleanFindings -join '; ')"
    }

    $browserLegacyEntries = @(
        [pscustomobject]@{
            Target = 'g_ww3d2'
            file = 'Core/LegacyRenderer/WWVegas/WW3D2/dx8webbrowser.cpp'
            command = 'cl.exe /ICore/LegacyRenderer/WWVegas /ICore/LegacyRenderer/WWVegas/WW3D2/dx8webbrowser.h -c dx8webbrowser.cpp'
        }
    )
    $browserLegacyFindings = @(Get-GraphFindings -Entries $browserLegacyEntries -TargetNames @('g_ww3d2') -NinjaContents @('build CMakeFiles/g_ww3d2.dir/dx8webbrowser.obj: CXX_COMPILER') -SkipSourceRead)
    if ($browserLegacyFindings.Count -lt 3) {
        throw "browser legacy graph fixture did not expose adapter/include/object edges (findings=$($browserLegacyFindings.Count))"
    }

    $singleTitleMigrationTargets = @('g_ww3d2', 'g_gameenginedevice')
    $singleTitleWw3d2Targets = @(Get-ConfiguredNativeTitleTargets `
        -MigrationTargets $singleTitleMigrationTargets `
        -TargetSuffix 'ww3d2')
    $singleTitleGameEngineDeviceTargets = @(Get-ConfiguredNativeTitleTargets `
        -MigrationTargets $singleTitleMigrationTargets `
        -TargetSuffix 'gameenginedevice')
    if ($singleTitleWw3d2Targets.Count -ne 1 -or
            $singleTitleWw3d2Targets[0] -ne 'g_ww3d2' -or
            $singleTitleGameEngineDeviceTargets.Count -ne 1 -or
            $singleTitleGameEngineDeviceTargets[0] -ne 'g_gameenginedevice') {
        throw "single-title migration target fixture selected the wrong native title graph"
    }

    # StrictFinal must continue to evaluate migration targets even when a
    # caller's authority target selection contains only native targets.
    $migrationFixture = @(
        [pscustomobject]@{
            Target = 'g_ww3d2'
            file = 'Core/Libraries/Source/WWVegas/WW3D2/dx8renderer.cpp'
            command = 'cl.exe /DBUILD_WITH_D3D8 /IH:/build/_deps/dx8-src -c dx8renderer.cpp'
        }
    )
    $strictMigrationFindings = @(Get-GraphFindings `
        -Entries $migrationFixture `
        -TargetNames @('g_ww3d2') `
        -SkipSourceRead)
    if ($strictMigrationFindings.Count -lt 3) {
        throw "StrictFinal migration fixture did not expose the complete target graph (findings=$($strictMigrationFindings.Count))"
    }

    $linkFixture = @(
        "build Generals\\Release\\generalsv.exe: CXX_EXECUTABLE_LINKER__g_generals_Release" +
            "`n  LINK_LIBRARIES = core_renderer.lib d3d11.lib dxgi.lib" +
            "`n  RSP_FILE = CMakeFiles/g_generals.Release.rsp"
    )
    $linkBlocks = @(Get-ProductLinkBlocks -NinjaContents $linkFixture -ProductTarget 'g_generals')
    if ($linkBlocks.Count -ne 1 -or
            $linkBlocks[0] -notmatch '(?i)core_renderer\.lib' -or
            $linkBlocks[0] -notmatch '(?i)\bd3d11\.lib\b' -or
            $linkBlocks[0] -notmatch '(?i)\bdxgi\.lib\b') {
        throw "product link authority fixture was not extracted completely (blocks=$($linkBlocks.Count))"
    }

    Write-Output 'Native renderer graph audit self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot) -or [string]::IsNullOrWhiteSpace($BuildRoot)) {
    throw 'SourceRoot and BuildRoot are required unless -SelfTest is specified.'
}
if (-not (Test-Path -LiteralPath $SourceRoot)) {
    throw "source root does not exist: $SourceRoot"
}
if (-not (Test-Path -LiteralPath $BuildRoot)) {
    throw "build root does not exist: $BuildRoot"
}

$compileCommandsPath = Join-Path $BuildRoot 'compile_commands.json'
if (-not (Test-Path -LiteralPath $compileCommandsPath)) {
    throw "generated compile graph is missing: $compileCommandsPath (configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON)"
}
$compileEntries = Get-Content -LiteralPath $compileCommandsPath -Raw | ConvertFrom-Json
if ($null -eq $compileEntries) {
    throw "generated compile graph is empty: $compileCommandsPath"
}
Write-Output "compile entries loaded: $($compileEntries.Count)"

$allTargetEntries = [System.Collections.Generic.List[object]]::new()
$configuration = $Configuration
foreach ($target in $Targets) {
    $targetEntries = @(Get-TargetEntries -Entries $compileEntries -Target $target -WantedConfiguration $configuration)
    if ($AuthorityOnly -and $targetEntries.Count -eq 0) {
        # CMake's multi-config compile_commands.json records one configuration
        # (normally Debug), while the Ninja link graph records every
        # configuration.  Authority checks may use representative entries
        # for an individual native target after an exact requested-config
        # lookup returns no entries; strict final checks never use this.
        $targetEntries = @(Get-TargetEntries -Entries $compileEntries -Target $target -WantedConfiguration '')
        if ($targetEntries.Count -gt 0) {
            Write-Output "authority compile graph fallback selected representative entries for ${target}: $($targetEntries.Count)"
        }
    }
    foreach ($entry in $targetEntries) {
        [void] $allTargetEntries.Add([pscustomobject]@{
            Target = $target
            file = [string] $entry.file
            command = [string] $entry.command
            directory = [string] $entry.directory
        })
    }
}
Write-Output "target entries selected: $($allTargetEntries.Count)"

$migrationTargetEntries = [System.Collections.Generic.List[object]]::new()
foreach ($target in $MigrationTargets) {
    $targetEntries = @(Get-TargetEntries -Entries $compileEntries -Target $target -WantedConfiguration $configuration)
    if ($AuthorityOnly -and $targetEntries.Count -eq 0) {
        $targetEntries = @(Get-TargetEntries -Entries $compileEntries -Target $target -WantedConfiguration '')
    }
    if ($AuthorityOnly) {
        Write-Output "migration target ${target} entries: $($targetEntries.Count)"
    }
    foreach ($entry in $targetEntries) {
        [void] $migrationTargetEntries.Add([pscustomobject]@{
            Target = $target
            file = [string] $entry.file
            command = [string] $entry.command
            directory = [string] $entry.directory
        })
    }
}
Write-Output "migration target entries selected: $($migrationTargetEntries.Count)"

$authorityTargetEntries = [System.Collections.Generic.List[object]]::new()
foreach ($target in $AuthorityTargets) {
    $targetEntries = @(Get-TargetEntries -Entries $compileEntries -Target $target -WantedConfiguration $configuration)
    if ($AuthorityOnly -and $targetEntries.Count -eq 0) {
        $targetEntries = @(Get-TargetEntries -Entries $compileEntries -Target $target -WantedConfiguration '')
        if ($targetEntries.Count -gt 0) {
            Write-Output "authority compile graph fallback selected representative entries for ${target}: $($targetEntries.Count)"
        }
    }
    foreach ($entry in $targetEntries) {
        [void] $authorityTargetEntries.Add([pscustomobject]@{
            Target = $target
            file = [string] $entry.file
            command = [string] $entry.command
            directory = [string] $entry.directory
        })
    }
}
Write-Output "authority target entries selected: $($authorityTargetEntries.Count)"

$cachePath = Join-Path $BuildRoot 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "CMake cache is missing: $cachePath"
}
$cache = Get-Content -LiteralPath $cachePath -Raw
$pointerSizeConfigured = $cache -match '(?i)CMAKE_SIZEOF_VOID_P(?::[^=]+)?=8'
if (-not $pointerSizeConfigured) {
    $compilerFiles = Get-ChildItem -LiteralPath (Join-Path $BuildRoot 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -Recurse -File -ErrorAction SilentlyContinue
    foreach ($compilerFile in $compilerFiles) {
        if ((Get-Content -LiteralPath $compilerFile.FullName -Raw) -match '(?i)CMAKE_(?:CXX|C)_SIZEOF_DATA_PTR\s+"8"') {
            $pointerSizeConfigured = $true
            break
        }
    }
}
Write-Output "pointer-size check complete: $pointerSizeConfigured"
if (-not $pointerSizeConfigured) {
    throw 'native renderer graph audit requires an 8-byte configured target graph.'
}

$pchContents = [System.Collections.Generic.List[string]]::new()
foreach ($pchPath in @(Get-PchPaths -Entries $allTargetEntries)) {
    if (Test-Path -LiteralPath $pchPath) {
        [void] $pchContents.Add((Get-Content -LiteralPath $pchPath -Raw))
    }
}
Write-Output "PCH scan complete: $($pchContents.Count)"

$migrationPchContents = [System.Collections.Generic.List[string]]::new()
foreach ($pchPath in @(Get-PchPaths -Entries $migrationTargetEntries)) {
    if (Test-Path -LiteralPath $pchPath) {
        [void] $migrationPchContents.Add((Get-Content -LiteralPath $pchPath -Raw))
    }
}
Write-Output "migration PCH scan complete: $($migrationPchContents.Count)"

$authorityPchContents = [System.Collections.Generic.List[string]]::new()
foreach ($pchPath in @(Get-PchPaths -Entries $authorityTargetEntries)) {
    if (Test-Path -LiteralPath $pchPath) {
        [void] $authorityPchContents.Add((Get-Content -LiteralPath $pchPath -Raw))
    }
}
Write-Output "authority PCH scan complete: $($authorityPchContents.Count)"

$ninjaContents = [System.Collections.Generic.List[string]]::new()
$ninjaCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($configuration)) {
    $ninjaCandidates += Join-Path $BuildRoot "CMakeFiles/impl-$configuration.ninja"
}
$ninjaCandidates += Join-Path $BuildRoot 'build.ninja'
if ($AuthorityOnly) {
    foreach ($configurationGraph in @('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')) {
        $ninjaCandidates += Join-Path $BuildRoot "CMakeFiles/impl-$configurationGraph.ninja"
    }
}
foreach ($ninjaPath in $ninjaCandidates | Select-Object -Unique) {
    if (Test-Path -LiteralPath $ninjaPath) {
        [void] $ninjaContents.Add((Get-Content -LiteralPath $ninjaPath -Raw))
    }
}
Write-Output "Ninja graph loaded: $($ninjaContents.Count) file(s)"

if ($AuthorityOnly -and -not $StrictFinal) {
    $nativeFindings = @(Get-GraphFindings `
        -Entries @($authorityTargetEntries) `
        -TargetNames $AuthorityTargets `
        -SourceRoot $SourceRoot `
        -PchContents @($authorityPchContents) `
        -NinjaContents @($ninjaContents) `
        -SkipDirectSourceEdge)
    $authorityFindings = @(Get-NativeAuthorityFindings `
        -SourceRoot $SourceRoot `
        -BuildRoot $BuildRoot `
        -MigrationTargets $MigrationTargets `
        -ProductTargets $ProductTargets `
        -NinjaContents @($ninjaContents) `
        -NativeEntries @($authorityTargetEntries) `
        -Configuration $configuration)
    $findings = @($nativeFindings + $authorityFindings | Select-Object -Unique)

    $migrationFindings = @(Get-GraphFindings `
        -Entries @($migrationTargetEntries) `
        -TargetNames $MigrationTargets `
        -SourceRoot $SourceRoot `
        -PchContents @($migrationPchContents) `
        -NinjaContents @($ninjaContents))
    Write-Output "migration debt inventory: findings=$($migrationFindings.Count) (reported, not used as authority failure)"
    foreach ($finding in @($migrationFindings | Select-Object -First 24)) {
        Write-Output "  migration $finding"
    }
    if ($migrationFindings.Count -gt 24) {
        Write-Output "  migration ... $($migrationFindings.Count - 24) additional findings; use -StrictFinal for the complete list"
    }
}
elseif ($StrictFinal) {
    # StrictFinal is intentionally independent of the caller's convenience
    # -Targets selection.  It always evaluates every configured migration
    # target and also proves the native authority/link closure.
    $migrationFindings = @(Get-GraphFindings `
        -Entries @($migrationTargetEntries) `
        -TargetNames $MigrationTargets `
        -SourceRoot $SourceRoot `
        -PchContents @($migrationPchContents) `
        -NinjaContents @($ninjaContents))
    $authorityFindings = @(Get-NativeAuthorityFindings `
        -SourceRoot $SourceRoot `
        -BuildRoot $BuildRoot `
        -MigrationTargets $MigrationTargets `
        -ProductTargets $ProductTargets `
        -NinjaContents @($ninjaContents) `
        -NativeEntries @($authorityTargetEntries) `
        -Configuration $configuration)
    $findings = @($migrationFindings + $authorityFindings | Select-Object -Unique)
}
else {
    $findings = @(Get-GraphFindings `
        -Entries @($allTargetEntries) `
        -TargetNames $Targets `
        -SourceRoot $SourceRoot `
        -PchContents @($pchContents) `
        -NinjaContents @($ninjaContents))
}
Write-Output "findings complete: $($findings.Count)"

Write-Output ("Native x64 renderer graph: targets={0}, configuration={1}, compile-entries={2}, pch-files={3}" -f `
    ($Targets -join ','),
    ($(if ([string]::IsNullOrWhiteSpace($configuration)) { 'unspecified' } else { $configuration })),
    $allTargetEntries.Count,
    $pchContents.Count)

if ($findings.Count -gt 0) {
    if ($AuthorityOnly -and -not $StrictFinal) {
        Write-Output 'Native x64 renderer authority audit FAILED.'
    }
    else {
        Write-Output 'Native x64 renderer graph audit FAILED.'
    }
    foreach ($finding in $findings) {
        Write-Output "  $finding"
    }
    throw "native renderer audit contains $($findings.Count) forbidden generated-graph edge(s)."
}

if ($AuthorityOnly -and -not $StrictFinal) {
    Write-Output 'Native x64 renderer authority audit passed.'
}
else {
    Write-Output 'Native x64 renderer graph audit passed.'
}
