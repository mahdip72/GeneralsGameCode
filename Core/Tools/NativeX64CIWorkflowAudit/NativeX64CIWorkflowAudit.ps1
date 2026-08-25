param(
    [string]$SourceRoot,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Description
    )

    if ($Text -notmatch $Pattern) {
        throw "Native x64 CI workflow audit failed: $Description"
    }
}

function Get-JobBody {
    param(
        [string]$Workflow,
        [string]$JobName
    )

    $escapedName = [Regex]::Escape($JobName)
    $match = [Regex]::Match(
        $Workflow,
        "(?ms)^  ${escapedName}:\r?\n(?<body>.*?)(?=^  [A-Za-z0-9_-]+:\r?\n|\z)"
    )
    if (-not $match.Success) {
        throw "Native x64 CI workflow audit failed: missing '$JobName' job"
    }

    return $match.Groups['body'].Value
}

function Test-NativeX64CIWorkflow {
    param(
        [string]$CIWorkflow,
        [string]$BuildWorkflow
    )

    foreach ($contract in @(
        @{ Name = 'build-generals-x64'; Game = 'Generals' },
        @{ Name = 'build-generalsmd-x64'; Game = 'GeneralsMD' }
    )) {
        $body = Get-JobBody -Workflow $CIWorkflow -JobName $contract.Name
        Assert-Contains $body "(?m)^      game: `"$([Regex]::Escape($contract.Game))`"\r?$" `
            "$($contract.Name) does not select $($contract.Game)"
        Assert-Contains $body '(?m)^      preset: "x64-vcpkg"\r?$' `
            "$($contract.Name) does not select the dependency-complete native x64 preset"
        Assert-Contains $body '(?m)^      extras: true\r?$' `
            "$($contract.Name) does not enable the native test graph"
    }

    Assert-Contains $CIWorkflow '(?m)^  pull_request:\r?$' `
        'stacked pull requests are excluded from CI'
    $pullRequestBlock = [Regex]::Match(
        $CIWorkflow,
        '(?ms)^  pull_request:\r?\n(?<body>(?: {4}.*\r?\n)*)'
    )
    if ($pullRequestBlock.Groups['body'].Value -match '(?m)^    branches(?:-ignore)?:') {
        throw 'Native x64 CI workflow audit failed: stacked pull requests are filtered out by base branch'
    }

    Assert-Contains $BuildWorkflow `
        "if: startsWith\(inputs\.preset, 'win32'\) \|\| startsWith\(inputs\.preset, 'x64'\)" `
        'the MSVC environment is not enabled for x64 presets'
    Assert-Contains $BuildWorkflow `
        "arch: \$\{\{ startsWith\(inputs\.preset, 'x64'\) && 'x64' \|\| 'x86' \}\}" `
        'the MSVC environment does not select the x64 host/target architecture'

    $multiConfigMatches = [Regex]::Matches(
        $BuildWorkflow,
        "-like 'win32\*' -or '[^']*' -like 'x64\*'"
    )
    if ($multiConfigMatches.Count -lt 2) {
        throw 'Native x64 CI workflow audit failed: x64 CTest and artifact paths are not both multi-config aware'
    }
}

function Test-NativeX64CMakeContract {
    param(
        [string]$Presets,
        [string]$CoreToolsCMake,
        [string]$FindFFmpegCMake
    )

    $parsedPresets = $Presets | ConvertFrom-Json
    $x64VcpkgPreset = @($parsedPresets.configurePresets | Where-Object {
        $_.name -eq 'x64-vcpkg'
    })
    if ($x64VcpkgPreset.Count -ne 1) {
        throw 'Native x64 CI workflow audit failed: x64-vcpkg configure preset is missing or ambiguous'
    }
    $inheritedPresets = @($x64VcpkgPreset[0].inherits)
    if ($inheritedPresets -notcontains 'x64' -or $inheritedPresets -notcontains 'default-vcpkg') {
        throw 'Native x64 CI workflow audit failed: x64-vcpkg does not inherit the product and dependency contracts'
    }

    Assert-Contains $CoreToolsCMake '\$ENV\{RUNNER_TEMP\}' `
        'the nested native product audit is not redirected to runner-owned scratch storage'
    Assert-Contains $CoreToolsCMake '-FFmpegRoot "\$\{FFMPEG_SDK_ROOT\}"' `
        'the nested native product audit does not receive the resolved FFmpeg SDK root'
    Assert-Contains $FindFFmpegCMake 'set\(FFMPEG_SDK_ROOT ' `
        'FFmpeg discovery does not publish its resolved SDK root'
}

if ($SelfTest) {
    $goodCI = @'
on:
  pull_request:
jobs:
  build-generals-x64:
    with:
      game: "Generals"
      preset: "x64-vcpkg"
      extras: true
  build-generalsmd-x64:
    with:
      game: "GeneralsMD"
      preset: "x64-vcpkg"
      extras: true
'@
    $goodBuild = @'
if: startsWith(inputs.preset, 'win32') || startsWith(inputs.preset, 'x64')
arch: ${{ startsWith(inputs.preset, 'x64') && 'x64' || 'x86' }}
if ('preset' -like 'win32*' -or 'preset' -like 'x64*') {}
if ('preset' -like 'win32*' -or 'preset' -like 'x64*') {}
'@
    $goodPresets = @'
{"configurePresets":[{"name":"x64-vcpkg","inherits":["x64","default-vcpkg"]}]}
'@
    $goodCoreToolsCMake = @'
set(audit_root "$ENV{RUNNER_TEMP}/GeneralsGameCode")
-FFmpegRoot "${FFMPEG_SDK_ROOT}"
'@
    $goodFindFFmpegCMake = 'set(FFMPEG_SDK_ROOT "resolved" CACHE PATH "root")'

    Test-NativeX64CIWorkflow -CIWorkflow $goodCI -BuildWorkflow $goodBuild
    Test-NativeX64CMakeContract `
        -Presets $goodPresets `
        -CoreToolsCMake $goodCoreToolsCMake `
        -FindFFmpegCMake $goodFindFFmpegCMake

    $caughtMissingTitle = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow ($goodCI -replace '  build-generalsmd-x64:', '  build-generalsmd-win32:') `
            -BuildWorkflow $goodBuild
    } catch {
        $caughtMissingTitle = $true
    }
    if (-not $caughtMissingTitle) {
        throw 'Native x64 CI workflow audit self-test failed to reject missing Zero Hour coverage'
    }

    $caughtBranchFilter = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow ($goodCI -replace "  pull_request:\r?\n", "  pull_request:`n    branches:`n      - main`n") `
            -BuildWorkflow $goodBuild
    } catch {
        $caughtBranchFilter = $true
    }
    if (-not $caughtBranchFilter) {
        throw 'Native x64 CI workflow audit self-test failed to reject base-branch filtering'
    }

    $caughtWin32Only = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow $goodCI `
            -BuildWorkflow ($goodBuild -replace " -or 'preset' -like 'x64\*'", '')
    } catch {
        $caughtWin32Only = $true
    }
    if (-not $caughtWin32Only) {
        throw 'Native x64 CI workflow audit self-test failed to reject Win32-only handling'
    }

    $caughtCheckoutAuditRoot = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake ($goodCoreToolsCMake -replace '\$ENV\{RUNNER_TEMP\}', '${CMAKE_BINARY_DIR}') `
            -FindFFmpegCMake $goodFindFFmpegCMake
    } catch {
        $caughtCheckoutAuditRoot = $true
    }
    if (-not $caughtCheckoutAuditRoot) {
        throw 'Native x64 CI workflow audit self-test failed to reject checkout-local audit storage'
    }

    Write-Host 'Native x64 CI workflow audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless -SelfTest is used.'
}

$ciPath = Join-Path $SourceRoot '.github/workflows/ci.yml'
$buildPath = Join-Path $SourceRoot '.github/workflows/build-toolchain.yml'
$presetsPath = Join-Path $SourceRoot 'CMakePresets.json'
$coreToolsCMakePath = Join-Path $SourceRoot 'Core/Tools/CMakeLists.txt'
$findFFmpegCMakePath = Join-Path $SourceRoot 'cmake/FindFFMPEG.cmake'
Test-NativeX64CIWorkflow `
    -CIWorkflow (Get-Content -Raw -LiteralPath $ciPath) `
    -BuildWorkflow (Get-Content -Raw -LiteralPath $buildPath)
Test-NativeX64CMakeContract `
    -Presets (Get-Content -Raw -LiteralPath $presetsPath) `
    -CoreToolsCMake (Get-Content -Raw -LiteralPath $coreToolsCMakePath) `
    -FindFFmpegCMake (Get-Content -Raw -LiteralPath $findFFmpegCMakePath)

Write-Host 'Native x64 CI workflow audit passed.'
