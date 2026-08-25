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
        Assert-Contains $body '(?m)^      preset: "x64"\r?$' `
            "$($contract.Name) does not select the native x64 preset"
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

if ($SelfTest) {
    $goodCI = @'
on:
  pull_request:
jobs:
  build-generals-x64:
    with:
      game: "Generals"
      preset: "x64"
      extras: true
  build-generalsmd-x64:
    with:
      game: "GeneralsMD"
      preset: "x64"
      extras: true
'@
    $goodBuild = @'
if: startsWith(inputs.preset, 'win32') || startsWith(inputs.preset, 'x64')
arch: ${{ startsWith(inputs.preset, 'x64') && 'x64' || 'x86' }}
if ('preset' -like 'win32*' -or 'preset' -like 'x64*') {}
if ('preset' -like 'win32*' -or 'preset' -like 'x64*') {}
'@

    Test-NativeX64CIWorkflow -CIWorkflow $goodCI -BuildWorkflow $goodBuild

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

    Write-Host 'Native x64 CI workflow audit self-test passed.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless -SelfTest is used.'
}

$ciPath = Join-Path $SourceRoot '.github/workflows/ci.yml'
$buildPath = Join-Path $SourceRoot '.github/workflows/build-toolchain.yml'
Test-NativeX64CIWorkflow `
    -CIWorkflow (Get-Content -Raw -LiteralPath $ciPath) `
    -BuildWorkflow (Get-Content -Raw -LiteralPath $buildPath)

Write-Host 'Native x64 CI workflow audit passed.'
