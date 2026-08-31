param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [string]$Baseline = '754cdae0170472a242095acded0cf253ced89512',
    [switch]$StrictFinal,
    [switch]$StrictD3D8Boundary
)

$ErrorActionPreference = 'Stop'
$sourceRootPath = (Resolve-Path -LiteralPath $SourceRoot).Path
$extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inl', '.cmake')
$strictBuildFileNames = @('CMakeLists.txt')
$rawD3D8RuleName = 'raw-d3d8-surface-area'
$rawD3D8BoundaryPaths = @(
    # The product's temporary D3D8 compatibility ABI is declared once here;
    # it remains deliberately outside the final-cutover boundary list below.
    'cmake/dx8.cmake',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h',
    'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.h',
    'Core/Libraries/Source/WWVegas/WW3D2/surfaceblit.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/surfaceblit.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8fvf.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8fvf.h',
    'Core/Libraries/Source/WWVegas/WW3D2/texturemipgenerator.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/texturemipgenerator.h',
    'Core/Libraries/Source/WWVegas/WW3D2/legacytexturecompat.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/legacytexturecompat.h',
    'Core/Tools/FVFStrideTest/FVFStrideTest.cpp',
    'Core/Tools/SurfaceBlitTest/SurfaceBlitTest.cpp',
    'Core/Tools/TextureMipGeneratorTest/TextureMipGeneratorTest.cpp',
    'Core/Tools/LegacyTextureCreationTest/LegacyTextureCreationTest.cpp',
    'Core/Tools/RendererContractTest/RendererContractTest.cpp',
    'Core/Tools/RendererContractTest/LegacyResetResourceTest.cpp',
    # These exact device-free renderer fixtures intentionally mirror the
    # legacy ABI while exercising production bridge fragments. Keep the
    # exemption narrow; StrictFinal still scans only product-runtime paths.
    'Core/Tools/RendererContractTest/CMakeLists.txt',
    'Core/Tools/RendererContractTest/LegacyAsyncBridgeCompletionTest.cpp',
    'Core/Tools/NativeD3D8CompatibilityTest/NativeD3D8CompatibilityTest.cpp'
)

# These are the only product-runtime files allowed to mention the legacy
# D3D8 ABI after the final renderer cutover.  Keep this list explicit: a
# wildcard here would allow an entire subsystem to silently remain on D3D8.
# The strict audit is intentionally not enabled by the intermediate-stage
# CTest; it is the final-cutover gate while the D3D8 backend still exists as a
# differential oracle.
$strictD3D8BoundaryPaths = @(
    'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8caps.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8caps.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8fvf.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8fvf.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8indexbuffer.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8indexbuffer.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8polygonrenderer.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8polygonrenderer.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8renderer.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8renderer.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8rendererdebugger.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8rendererdebugger.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8texman.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8texman.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8vertexbuffer.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8vertexbuffer.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8webbrowser.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8webbrowser.h',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h',
    'Core/Libraries/Source/WWVegas/WW3D2/legacytexturecompat.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/legacytexturecompat.h',
    'Core/Libraries/Source/WWVegas/WW3D2/surfaceblit.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/surfaceblit.h',
    'Core/Libraries/Source/WWVegas/WW3D2/texturemipgenerator.cpp',
    'Core/Libraries/Source/WWVegas/WW3D2/texturemipgenerator.h'
)

# Product code is deliberately narrower than the repository source set.  In
# particular, W3DView, WorldBuilder, GUI editors, and Core/Tools are
# authoring/test programs and may retain an old SDK while the game runtime is
# migrated.  Do not add a broad "not Tools" rule: the prefixes below make the
# audited product surface reviewable and deterministic.
$productRuntimePrefixes = @(
    'Core/GameEngine/',
    'Core/GameEngineDevice/',
    'Core/Libraries/',
    'Generals/Code/GameEngine/',
    'Generals/Code/GameEngineDevice/',
    'Generals/Code/Libraries/',
    'Generals/Code/Main/',
    'GeneralsMD/Code/GameEngine/',
    'GeneralsMD/Code/GameEngineDevice/',
    'GeneralsMD/Code/Libraries/',
    'GeneralsMD/Code/Main/'
)

$strictD3D8Rules = @(
    [pscustomobject]@{
        Name = 'd3d8-interface'
        Pattern = '\b(?:LPDIRECT3D[A-Z0-9_]*8|IDirect3D[A-Za-z0-9_]*8)\b'
    },
    [pscustomobject]@{
        Name = 'd3d8-fvf'
        Pattern = '\bD3DFVF_[A-Za-z0-9_]+\b'
    },
    [pscustomobject]@{
        Name = 'd3d8-format'
        Pattern = '\bD3DFMT_[A-Za-z0-9_]+\b'
    },
    [pscustomobject]@{
        Name = 'd3d8-header'
        Pattern = '(?i)#\s*include\s*[<"]\s*d3d8(?:types)?\.h\s*[>"]'
    },
    [pscustomobject]@{
        Name = 'd3dx8-header-or-symbol'
        Pattern = '(?i)(?:#\s*include\s*[<"]\s*d3dx8[A-Za-z0-9_.-]*\s*[>"]|\bD3DX[A-Za-z0-9_]+\b)'
    },
    [pscustomobject]@{
        Name = 'd3d8-build-dependency'
        # This rule is applied only to CMake/build files below. Keeping the
        # scope explicit prevents an include such as d3dx8.h from being
        # reported a second time as a linker dependency.
        Pattern = '(?i)(?<![A-Za-z0-9_])d3dx?8(?:lib|\.lib)?(?![A-Za-z0-9_])'
        Scope = 'build'
    },
    [pscustomobject]@{
        Name = 'd3d8-legacy-descriptor-or-constant'
        Pattern = '(?i)\b(?:D3D[A-Z0-9_]*8|D3D(?:ADAPTER|BLEND|BLENDOP|CAPS|CLEAR|CLIP|COLORWRITEENABLE|CREATE|CULL|DEVCAPS|ERR_|FOG|LIGHT|LOCK|MATERIAL|MCS_|MULTISAMPLE|PRESENT|PRASTERCAPS|PRIMITIVE|PTEXTURE|PTFILTER|RENDERSTATE|RS_|RESOURCE|SHADE|STENCIL|SWAPEFFECT|TA_|TADDRESS|TEXOP|TEXTURE|TOP_|TRANSFORM|TS_|TSS_|USAGE|VIEWPORT|WRAP_)[A-Z0-9_]*)\b'
    },
    [pscustomobject]@{
        Name = 'd3d8-active-device-call'
        # Do not classify a generic D3D11 variable named "device" as a
        # D3D8 call. Raw interface declarations/constants are independently
        # reported; these names cover the legacy pointer aliases used by the
        # old backend without relying on file-wide context heuristics.
        Pattern = '(?i)(?:\bDX8Wrapper::(?:_Get_D3D_Device8|Get_D3D_Device8)\s*\(\s*\)\s*->\s*[A-Za-z_][A-Za-z0-9_]*\s*\(|\bDX8CALL(?:_HRES)?\s*\(|\b(?:D3DDevice|m_pDev|pDev|d3dDevice|d3d_device)\s*->\s*(?:Set|Get|Create|Delete|Draw|Present|Clear|Reset|Lock|Unlock|Test|Process|Resource|Stretch|Begin|End|Light|Color|Validate)[A-Za-z0-9_]*\s*\()'
    }
)

function Test-BaselineAncestry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Commit
    )

    & git -C $Root cat-file -e "$Commit`^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Baseline '$Commit' is not a commit in the source repository."
    }
    & git -C $Root merge-base --is-ancestor $Commit HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Baseline '$Commit' must be an ancestor of HEAD."
    }
}

function Get-SourceFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $tracked = @(& git -C $Root ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) {
        throw 'git ls-files failed'
    }
    return @($tracked | Where-Object {
        $extensions -contains [IO.Path]::GetExtension($_).ToLowerInvariant() -or
            $strictBuildFileNames -contains [IO.Path]::GetFileName($_)
    } | Sort-Object -Unique)
}

function Get-UntrackedSourceFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $untracked = @(& git -C $Root ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) {
        throw 'git ls-files for untracked sources failed'
    }
    return @($untracked | Where-Object {
        $extensions -contains [IO.Path]::GetExtension($_).ToLowerInvariant() -or
            $strictBuildFileNames -contains [IO.Path]::GetFileName($_)
    } | Sort-Object -Unique)
}

function Get-RegexMatchCount {
    param(
        [AllowEmptyString()]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Pattern
    )

    if ([string]::IsNullOrEmpty($Text)) {
        return 0
    }
    return ([regex]::Matches(
        $Text, $Pattern,
        [Text.RegularExpressions.RegexOptions]::Multiline)).Count
}

function Get-BaselineRegexMatchCounts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Commit,
        [Parameter(Mandatory = $true)]
        [string]$Pattern
    )

    $counts = @{}
    $pathspecs = @('*.c', '*.cc', '*.cpp', '*.cxx', '*.h', '*.hpp', '*.inl')
    $matches = @(& git -C $Root grep --no-color -o -E -e $Pattern $Commit -- $pathspecs)
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1) {
        throw "git grep against $Commit failed"
    }
    foreach ($match in $matches) {
        $firstColon = $match.IndexOf(':')
        $secondColon = $match.IndexOf(':', $firstColon + 1)
        if ($firstColon -lt 0 -or $secondColon -lt 0) {
            continue
        }
        $path = $match.Substring(
            $firstColon + 1, $secondColon - $firstColon - 1).Replace('\', '/')
        if (-not $counts.ContainsKey($path)) {
            $counts[$path] = 0
        }
        ++$counts[$path]
    }
    return $counts
}

function Get-ProductRuntimeSourceFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Files
    )

    $selected = New-Object 'System.Collections.Generic.List[string]'
    foreach ($file in $Files) {
        $relativePath = $file.Replace('\', '/')
        $segments = $relativePath.Split('/')
        if ([IO.Path]::IsPathRooted($relativePath) -or $segments -contains '..') {
            throw "Unsafe repository-relative source path '$relativePath'."
        }
        $isProductRuntime = $false
        foreach ($prefix in $productRuntimePrefixes) {
            if ($relativePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $isProductRuntime = $true
                break
            }
        }
        if ($isProductRuntime) {
            $selected.Add($relativePath)
        }
    }
    return @($selected | Sort-Object -Unique)
}

function Test-StrictBoundaryPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    foreach ($boundaryPath in $strictD3D8BoundaryPaths) {
        if ($boundaryPath.Equals($RelativePath, [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Test-StrictBuildFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    return $strictBuildFileNames -contains [IO.Path]::GetFileName($RelativePath) -or
        [IO.Path]::GetExtension($RelativePath).Equals('.cmake', [StringComparison]::OrdinalIgnoreCase)
}

function Get-StrictD3D8Matches {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string[]]$Files
    )

    $records = New-Object 'System.Collections.Generic.List[object]'
    $ignoreCase = [Text.RegularExpressions.RegexOptions]::IgnoreCase

    foreach ($relativePath in (Get-ProductRuntimeSourceFiles $Files)) {
        $path = Join-Path $Root ($relativePath -replace '/', '\')
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            continue
        }
        $isBoundary = Test-StrictBoundaryPath $relativePath
        $isBuildFile = Test-StrictBuildFile $relativePath
        $lines = [IO.File]::ReadAllLines($path)
        for ($lineIndex = 0; $lineIndex -lt $lines.Length; ++$lineIndex) {
            $line = $lines[$lineIndex]
            foreach ($rule in $strictD3D8Rules) {
                $scope = $rule.PSObject.Properties['Scope']
                if ($null -ne $scope -and $scope.Value -eq 'build' -and -not $isBuildFile) {
                    continue
                }
                foreach ($match in [regex]::Matches($line, $rule.Pattern, $ignoreCase)) {
                    $records.Add([pscustomobject]@{
                        Path = $relativePath
                        Category = $rule.Name
                        Line = $lineIndex + 1
                        Match = $match.Value
                        Text = $line.Trim()
                        Boundary = $isBoundary
                    })
                }
            }
        }
    }
    return @($records | Sort-Object Path, Line, Category, Match)
}

function Write-StrictD3D8Report {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Matches
    )

    $outside = @($Matches | Where-Object { -not $_.Boundary })
    $boundary = @($Matches | Where-Object { $_.Boundary })
    Write-Output ('raw-d3d8-boundary: product-files={0} boundary-files={1}' -f
        ((@($outside.Path) + @($boundary.Path) | Sort-Object -Unique).Count),
        (@($boundary.Path | Sort-Object -Unique).Count))
    foreach ($rule in $strictD3D8Rules) {
        $outsideCategory = @($outside | Where-Object {
            $_.Category -eq $rule.Name
        })
        $boundaryCategory = @($boundary | Where-Object {
            $_.Category -eq $rule.Name
        })
        Write-Output ('raw-d3d8-boundary category={0} outside-files={1} outside-matches={2} boundary-files={3} boundary-matches={4}' -f
            $rule.Name,
            (@($outsideCategory.Path | Sort-Object -Unique).Count),
            $outsideCategory.Count,
            (@($boundaryCategory.Path | Sort-Object -Unique).Count),
            $boundaryCategory.Count)
    }
    foreach ($match in $outside | Sort-Object Path, Line, Category, Match) {
        Write-Output ('raw-d3d8-boundary violation: {0}:{1}: {2}: {3}' -f
            $match.Path, $match.Line, $match.Category, $match.Text)
    }
}

$rules = @(
    [pscustomobject]@{
        Name = 'pointer-to-32-bit-cast'
        Pattern = '(reinterpret_cast\s*<\s*(Int|UnsignedInt|int|unsigned|DWORD|LONG)\s*>|\(\s*(Int|UnsignedInt|int|unsigned|DWORD|LONG)\s*\)\s*([A-Za-z_][A-Za-z0-9_]*(Ptr|Pointer|Address)|this)\b)'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'x86-inline-assembly-or-context'
        Pattern = '(^|[^A-Za-z0-9_])(__asm|_asm|Eip|Esp|Ebp)([^A-Za-z0-9_]|$)'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'pointer-sized-serialization'
        Pattern = '(sizeof\s*\([^\)]*\*\)|xfer[^\r\n]*(void\s*\*|uintptr_t|intptr_t))'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'pointer-bearing-window-message'
        Pattern = '(WindowMsgData|WM_[A-Z0-9_]+|LPARAM|WPARAM)[^\r\n]*(void\s*\*|reinterpret_cast|\(Int\)|\(UnsignedInt\))'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'window-message-implicit-narrowing'
        # WindowMsgData is pointer-sized on x64.  Scalar payloads must pass
        # through an explicit width-preserving helper before being assigned to
        # the legacy 8/16/32-bit gameplay types.
        Pattern = '(?<![A-Za-z0-9_])(?:UnsignedByte|Byte|char|WideChar|Int|int|UnsignedInt|unsigned|Bool|Color|UnsignedShort|short|long|float|double|NameKeyType)\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*mData[123]\b'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = 'window-message-raw-pointer-cast'
        # A C-style cast hides the ABI boundary and makes accidental pointer
        # truncation easy to reintroduce.  Use WindowMsgDataToPointer instead.
        Pattern = '\(\s*(?:const\s+)?[A-Za-z_][A-Za-z0-9_:<>]*\s*\*+\s*\)\s*mData[123]\b'
        RejectAddedLine = $true
    },
    [pscustomobject]@{
        Name = $rawD3D8RuleName
        # The wrapper is the temporary migration facade; calling it is not a
        # raw D3D8 dependency. Ratchet only legacy API types/constants that
        # escape the explicit wrapper/bridge boundary.
        # Require at least one type-name character between D3D and the legacy
        # generation suffix. This intentionally matches D3DLIGHT8 and
        # IDirect3DTexture8, but not neutral helper names containing the plain
        # label "D3D8" (for example Checked_D3D8_Primitive_Index_Count).
        Pattern = '(IDirect3D[A-Za-z0-9_]*8|D3D[A-Z0-9_]+8)'
        RejectAddedLine = $false
    }
)

$stackDumpAddressContracts = @(
    [pscustomobject]@{
        Path = 'Generals/Code/GameEngine/Include/Common/StackDump.h'
        ExpectedCount = 2
    },
    [pscustomobject]@{
        Path = 'GeneralsMD/Code/GameEngine/Include/Common/StackDump.h'
        ExpectedCount = 2
    },
    [pscustomobject]@{
        Path = 'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp'
        ExpectedCount = 2
    },
    [pscustomobject]@{
        Path = 'GeneralsMD/Code/GameEngine/Source/Common/System/StackDump.cpp'
        ExpectedCount = 2
    }
)
$stackDumpAddressPattern = '(?m)^\s*(?:__inline\s+)?void\s+GetFunctionDetails\([^\r\n]*\b(?:std::)?uintptr_t\s*\*\s*address\s*\)'

Test-BaselineAncestry $sourceRootPath $Baseline
$files = Get-SourceFiles $sourceRootPath
$untrackedFiles = Get-UntrackedSourceFiles $sourceRootPath

$violations = @()
foreach ($contract in $stackDumpAddressContracts) {
    $path = Join-Path $sourceRootPath ($contract.Path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        continue
    }
    $count = Get-RegexMatchCount ([IO.File]::ReadAllText($path)) $stackDumpAddressPattern
    if ($count -ne $contract.ExpectedCount) {
        $violations += ('{0}: stackdump-address-width expected={1} current={2}' -f
            $contract.Path, $contract.ExpectedCount, $count)
    }
}
$rawD3D8Rule = $rules | Where-Object { $_.Name -eq $rawD3D8RuleName }
$baselineRawCounts = Get-BaselineRegexMatchCounts $sourceRootPath $Baseline $rawD3D8Rule.Pattern
$currentRawTotal = 0
foreach ($relativePath in $files) {
    $path = Join-Path $sourceRootPath ($relativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        continue
    }
    $count = Get-RegexMatchCount ([IO.File]::ReadAllText($path)) $rawD3D8Rule.Pattern
    $currentRawTotal += $count
    if ($rawD3D8BoundaryPaths -contains $relativePath) {
        continue
    }
    $baselineCount = if ($baselineRawCounts.ContainsKey($relativePath)) {
        $baselineRawCounts[$relativePath]
    } else {
        0
    }
    if ($count -gt $baselineCount) {
        $violations += "${relativePath}: $rawD3D8RuleName baseline=$baselineCount current=$count"
    }
}

foreach ($rule in $rules) {
    if ($rule.Name -eq $rawD3D8RuleName) {
        Write-Output ('{0}: {1}' -f $rule.Name, $currentRawTotal)
        continue
    }
    $count = 0
    foreach ($relativePath in $files) {
        $path = Join-Path $sourceRootPath ($relativePath -replace '/', '\')
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $count += Get-RegexMatchCount (
                [IO.File]::ReadAllText($path)) $rule.Pattern
        }
    }
    Write-Output ('{0}: {1}' -f $rule.Name, $count)
}

foreach ($relativePath in $untrackedFiles) {
    $path = Join-Path $sourceRootPath ($relativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        continue
    }
    $content = [IO.File]::ReadAllText($path)
    foreach ($rule in $rules | Where-Object { $_.RejectAddedLine }) {
        if ($content -match $rule.Pattern) {
            $violations += "${relativePath}: $($rule.Name)"
        }
    }
}

$diff = & git -C $sourceRootPath diff --unified=0 $Baseline -- @(
    '*.c', '*.cc', '*.cpp', '*.cxx', '*.h', '*.hpp', '*.inl'
)
if ($LASTEXITCODE -ne 0) {
    throw "git diff against $Baseline failed"
}

$currentFile = ''
$lineNumber = 0
foreach ($line in $diff) {
    if ($line -match '^\+\+\+ b/(.+)$') {
        $currentFile = $Matches[1]
        continue
    }
    if ($line -match '^@@ -[^ ]+ \+(\d+)') {
        $lineNumber = [int]$Matches[1]
        continue
    }
    if ($line.StartsWith('+') -and -not $line.StartsWith('+++')) {
        $content = $line.Substring(1)
        foreach ($rule in $rules) {
            $allowedPath = $rule.PSObject.Properties['AllowedPath']
            $isAllowed = $null -ne $allowedPath -and
                $currentFile -match $allowedPath.Value
            # A 32-bit compatibility branch must still name the legacy
            # CONTEXT Eip member.  Allow only this explicitly annotated,
            # pointer-width conversion in the two crash-path adapters; all
            # assembly and every other context-register addition remain
            # fail-closed.
            $isApprovedX86Context = $rule.Name -eq 'x86-inline-assembly-or-context' -and
                $currentFile -in @(
                    'Core/Libraries/Source/debug/debug_except.cpp',
                    'Core/Libraries/Source/WWVegas/WWLib/Except.cpp',
                    'Generals/Code/GameEngine/Source/Common/System/StackDump.cpp',
                    'GeneralsMD/Code/GameEngine/Source/Common/System/StackDump.cpp'
                ) -and
                ($content -match '^\s*return static_cast<uintptr_t>\((ctx|context)\.Eip\);\s*// portability-audit: x86-context\s*$' -or
                 $content -match '^\s*MakeStackTrace\(eip,esp,ebp, 0, callback\);\s*// portability-audit: x86-context\s*$' -or
                 $content -match '^\s*const (?:std::)?uintptr_t instructionPointer = static_cast<(?:std::)?uintptr_t>\(context->Eip\);\s*// portability-audit: x86-context\s*$')
            # VC6 has no intrinsic/header for obtaining the caller return
            # address. Permit only the explicitly annotated instruction in
            # the legacy compatibility branch; every other added assembly
            # line, including an unannotated line in this file, still fails.
            $isApprovedVC6CallerAddress =
                $rule.Name -eq 'x86-inline-assembly-or-context' -and
                $currentFile -eq 'Core/Libraries/Source/debug/debug_debug.cpp' -and
                ($content -match '^\s*_asm\s*// portability-audit: vc6-caller-address\s*$' -or
                 $content -match '^\s*mov eax,\[ebp\+4\]\s*// portability-audit: vc6-caller-address\s*$')
            $isApprovedWindowMessageBoundary =
                $rule.Name -eq 'pointer-bearing-window-message' -and
                $currentFile -eq 'Core/GameEngine/Include/GameClient/GameWindow.h' -and
                $content -match '^\s*inline\s+WindowMsgData\s+WindowMsgDataFromPointer\(const\s+void\s*\*\s*value\)\s*$'
            if ($rule.RejectAddedLine -and -not $isAllowed -and
                -not $isApprovedX86Context -and
                -not $isApprovedVC6CallerAddress -and
                -not $isApprovedWindowMessageBoundary -and
                $content -match $rule.Pattern) {
                $violations += "${currentFile}:${lineNumber}: $($rule.Name)"
            }
        }
        ++$lineNumber
    } elseif (-not $line.StartsWith('-')) {
        ++$lineNumber
    }
}

if ($StrictFinal -or $StrictD3D8Boundary) {
    $strictMatches = Get-StrictD3D8Matches $sourceRootPath $files
    Write-StrictD3D8Report $strictMatches
    $strictOutsideCount = @($strictMatches |
        Where-Object { -not $_.Boundary }).Count
    if ($strictOutsideCount -ne 0) {
        $violations +=
            "strict-final raw-d3d8-boundary outside-occurrences=$strictOutsideCount"
    }
}

if ($violations.Count -ne 0) {
    foreach ($violation in $violations) {
        Write-Output $violation
    }
    exit 1
}

Write-Output "Portability audit found no new high-confidence violations relative to $Baseline."
