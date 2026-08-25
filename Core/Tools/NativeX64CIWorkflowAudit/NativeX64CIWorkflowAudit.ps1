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

function Get-StepBody {
    param(
        [string]$Workflow,
        [string]$StepNamePrefix
    )

    $escapedPrefix = [Regex]::Escape($StepNamePrefix)
    $match = [Regex]::Match(
        $Workflow,
        "(?ms)^      - name: ${escapedPrefix}[^\r\n]*\r?\n(?<body>.*?)(?=^      - name: |\z)"
    )
    if (-not $match.Success) {
        throw "Native x64 CI workflow audit failed: missing '$StepNamePrefix' step"
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
        $titleChangeOutput = if ($contract.Game -eq 'GeneralsMD') { 'generalsmd' } else { 'generals' }
        Assert-Contains $body "(?m)^      game: `"$([Regex]::Escape($contract.Game))`"\r?$" `
            "$($contract.Name) does not select $($contract.Game)"
        Assert-Contains $body '(?m)^      preset: "x64-vcpkg"\r?$' `
            "$($contract.Name) does not select the dependency-complete native x64 preset"
        Assert-Contains $body '(?m)^      extras: true\r?$' `
            "$($contract.Name) does not enable the native test graph"
        Assert-Contains $body '(?m)^    needs: (?:detect-changes|\[detect-changes, build-generals-x64\])\r?$' `
            "$($contract.Name) does not depend on change detection"
        Assert-Contains $body '(?m)^    uses: \./\.github/workflows/build-toolchain\.yml\r?$' `
            "$($contract.Name) does not invoke the reusable build workflow"
        Assert-Contains $body `
            "needs\.detect-changes\.outputs\.${titleChangeOutput} == 'true'" `
            "$($contract.Name) can skip its title changes"
        Assert-Contains $body "needs\.detect-changes\.outputs\.shared == 'true'" `
            "$($contract.Name) can skip shared changes"
        if ($contract.Game -eq 'Generals') {
            Assert-Contains $body "needs\.detect-changes\.outputs\.generalsmd == 'true'" `
                'the x64 cache producer can be skipped for a Zero Hour-only change'
        } else {
            Assert-Contains $body '(?m)^    needs: \[detect-changes, build-generals-x64\]\r?$' `
                'Zero Hour x64 can race the dependency-cache producer'
        }
    }

    Assert-Contains $CIWorkflow '(?m)^  pull_request:\r?$' `
        'stacked pull requests are excluded from CI'
    Assert-Contains $CIWorkflow "(?m)^              - 'vcpkg\.json'\r?$" `
        'vcpkg manifest changes bypass native CI'
    Assert-Contains $CIWorkflow "(?m)^              - 'triplets/\*\*'\r?$" `
        'vcpkg triplet changes bypass native CI'
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
    Assert-Contains $BuildWorkflow `
        "inputs\.game == 'Generals' && inputs\.preset == 'x64-vcpkg'" `
        'no active native x64 job can save the vcpkg binary cache'
    Assert-Contains $BuildWorkflow `
        "timeout-minutes: \$\{\{ startsWith\(inputs\.preset, 'x64'\) && 90 \|\| 30 \}\}" `
        'native dependency builds retain the legacy 30-minute timeout'

    $testStep = Get-StepBody -Workflow $BuildWorkflow -StepNamePrefix 'Run Core extras tests'
    Assert-Contains $testStep "-like 'win32\*' -or '[^']*' -like 'x64\*'" `
        'the CTest step is not x64 multi-config aware'
    Assert-Contains $testStep "'--no-tests=error'" `
        'CTest can succeed without registering tests'
    Assert-Contains $testStep 'exit \$LASTEXITCODE' `
        'CTest failures are not propagated to the workflow result'

    $installStep = Get-StepBody -Workflow $BuildWorkflow -StepNamePrefix 'Install native runtime'
    Assert-Contains $installStep "if: startsWith\(inputs\.preset, 'x64'\)" `
        'the installed-runtime step is not limited to native x64 builds'
    Assert-Contains $installStep 'cmake --install ' `
        'the native CI artifact is not produced through CMake install rules'

    $artifactStep = Get-StepBody -Workflow $BuildWorkflow -StepNamePrefix 'Collect '
    Assert-Contains $artifactStep "-like 'x64\*'" `
        'the artifact collector has no native x64 installed-runtime branch'
    Assert-Contains $artifactStep '\$installedRuntime' `
        'the native x64 artifact is collected from build outputs instead of the installed runtime'
}

function Test-NativeX64CMakeContract {
    param(
        [string]$Presets,
        [string]$CoreToolsCMake,
        [string]$FindFFmpegCMake,
        [string]$CompressionCMake,
        [string]$GeneralsCMake,
        [string]$ZeroHourCMake
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
    Assert-Contains $CoreToolsCMake '_native_product_runtime_audit_fallback_root' `
        'local preset audits have no external fallback scratch root'
    Assert-Contains $CoreToolsCMake '-FFmpegRoot "\$\{FFMPEG_SDK_ROOT\}"' `
        'the nested native product audit does not receive the resolved FFmpeg SDK root'
    Assert-Contains $FindFFmpegCMake 'set\(FFMPEG_SDK_ROOT ' `
        'FFmpeg discovery does not publish its resolved SDK root'
    Assert-Contains $CompressionCMake 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE' `
        'vcpkg zlib runtime discovery is missing'
    Assert-Contains $GeneralsCMake 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE' `
        'Generals does not install the resolved vcpkg zlib runtime'
    Assert-Contains $ZeroHourCMake 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE' `
        'Zero Hour does not install the resolved vcpkg zlib runtime'
}

if ($SelfTest) {
    $goodCI = @'
on:
  pull_request:
jobs:
              - 'vcpkg.json'
              - 'triplets/**'
  build-generals-x64:
    needs: detect-changes
    if: needs.detect-changes.outputs.generals == 'true' || needs.detect-changes.outputs.generalsmd == 'true' || needs.detect-changes.outputs.shared == 'true'
    uses: ./.github/workflows/build-toolchain.yml
    with:
      game: "Generals"
      preset: "x64-vcpkg"
      extras: true
  build-generalsmd-x64:
    needs: [detect-changes, build-generals-x64]
    if: needs.detect-changes.outputs.generalsmd == 'true' || needs.detect-changes.outputs.shared == 'true'
    uses: ./.github/workflows/build-toolchain.yml
    with:
      game: "GeneralsMD"
      preset: "x64-vcpkg"
      extras: true
'@
    $goodBuild = @'
if: startsWith(inputs.preset, 'win32') || startsWith(inputs.preset, 'x64')
arch: ${{ startsWith(inputs.preset, 'x64') && 'x64' || 'x86' }}
timeout-minutes: ${{ startsWith(inputs.preset, 'x64') && 90 || 30 }}
if: inputs.game == 'Generals' && inputs.preset == 'x64-vcpkg'
      - name: Run Core extras tests for game
        run: |
          $arguments = @('--no-tests=error')
          if ('preset' -like 'win32*' -or 'preset' -like 'x64*') {}
          exit $LASTEXITCODE
      - name: Install native runtime for game
        if: startsWith(inputs.preset, 'x64')
        run: |
          cmake --install build
      - name: Collect game Artifact
        run: |
          if ('preset' -like 'x64*') { $installedRuntime = 'installed' }
'@
    $goodPresets = @'
{"configurePresets":[{"name":"x64-vcpkg","inherits":["x64","default-vcpkg"]}]}
'@
    $goodCoreToolsCMake = @'
set(audit_root "$ENV{RUNNER_TEMP}/GeneralsGameCode")
set(_native_product_runtime_audit_fallback_root "sibling")
-FFmpegRoot "${FFMPEG_SDK_ROOT}"
'@
    $goodFindFFmpegCMake = 'set(FFMPEG_SDK_ROOT "resolved" CACHE PATH "root")'
    $goodCompressionCMake = 'set(RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE "zlib1.dll")'
    $goodGeneralsCMake = 'install(FILES ${RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE})'
    $goodZeroHourCMake = 'install(FILES ${RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE})'

    Test-NativeX64CIWorkflow -CIWorkflow $goodCI -BuildWorkflow $goodBuild
    Test-NativeX64CMakeContract `
        -Presets $goodPresets `
        -CoreToolsCMake $goodCoreToolsCMake `
        -FindFFmpegCMake $goodFindFFmpegCMake `
        -CompressionCMake $goodCompressionCMake `
        -GeneralsCMake $goodGeneralsCMake `
        -ZeroHourCMake $goodZeroHourCMake

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

    $caughtWrongReusableWorkflow = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow ($goodCI -replace 'uses: \./\.github/workflows/build-toolchain\.yml', 'uses: ./wrong.yml') `
            -BuildWorkflow $goodBuild
    } catch {
        $caughtWrongReusableWorkflow = $true
    }
    if (-not $caughtWrongReusableWorkflow) {
        throw 'Native x64 CI workflow audit self-test failed to reject the wrong reusable workflow'
    }

    $caughtMissingNoTestsGuard = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow $goodCI `
            -BuildWorkflow ($goodBuild -replace "'--no-tests=error'", "'--output-on-failure'")
    } catch {
        $caughtMissingNoTestsGuard = $true
    }
    if (-not $caughtMissingNoTestsGuard) {
        throw 'Native x64 CI workflow audit self-test failed to reject zero-test success'
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
            -CoreToolsCMake (($goodCoreToolsCMake -replace '\$ENV\{RUNNER_TEMP\}', '${CMAKE_BINARY_DIR}') `
                -replace '_native_product_runtime_audit_fallback_root', 'missing_fallback') `
            -FindFFmpegCMake $goodFindFFmpegCMake `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake
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
$compressionCMakePath = Join-Path $SourceRoot 'Core/Libraries/Source/Compression/CMakeLists.txt'
$generalsCMakePath = Join-Path $SourceRoot 'Generals/CMakeLists.txt'
$zeroHourCMakePath = Join-Path $SourceRoot 'GeneralsMD/CMakeLists.txt'
Test-NativeX64CIWorkflow `
    -CIWorkflow (Get-Content -Raw -LiteralPath $ciPath) `
    -BuildWorkflow (Get-Content -Raw -LiteralPath $buildPath)
Test-NativeX64CMakeContract `
    -Presets (Get-Content -Raw -LiteralPath $presetsPath) `
    -CoreToolsCMake (Get-Content -Raw -LiteralPath $coreToolsCMakePath) `
    -FindFFmpegCMake (Get-Content -Raw -LiteralPath $findFFmpegCMakePath) `
    -CompressionCMake (Get-Content -Raw -LiteralPath $compressionCMakePath) `
    -GeneralsCMake (Get-Content -Raw -LiteralPath $generalsCMakePath) `
    -ZeroHourCMake (Get-Content -Raw -LiteralPath $zeroHourCMakePath)

Write-Host 'Native x64 CI workflow audit passed.'
