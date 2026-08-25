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
    Assert-Contains $CIWorkflow "(?m)^              - 'vcpkg-lock\.json'\r?$" `
        'vcpkg lockfile changes bypass native CI'
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
    Assert-Contains $BuildWorkflow "hashFiles\([^\r\n]*'cmake/patches/\*\.patch'" `
        'native D3D8 patch contents do not invalidate the CMake dependency cache'

    $testStep = Get-StepBody -Workflow $BuildWorkflow -StepNamePrefix 'Run Core extras tests'
    Assert-Contains $testStep '(?m)^        if: inputs\.extras\r?$' `
        'the CTest step is not bound to the extras-enabled job predicate'
    Assert-Contains $testStep "-like 'win32\*' -or '[^']*' -like 'x64\*'" `
        'the CTest step is not x64 multi-config aware'
    Assert-Contains $testStep "'--no-tests=error'" `
        'CTest can succeed without registering tests'
    Assert-Contains $testStep '& ctest @arguments' `
        'the CTest step does not invoke CTest'
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
    Assert-Contains $artifactStep 'Copy-Item -Path \(Join-Path \$installedRuntime ''\*''\)' `
        'the native x64 artifact copy is not bound to the installed runtime'

    $uploadStep = Get-StepBody -Workflow $BuildWorkflow -StepNamePrefix 'Upload '
    Assert-Contains $uploadStep '(?m)^          path: build\\\$\{\{ inputs\.preset \}\}\\\$\{\{ inputs\.game \}\}\\artifacts\r?$' `
        'the uploaded artifact is not bound to the collected artifact directory'
    Assert-Contains $uploadStep '(?m)^          if-no-files-found: error\r?$' `
        'the artifact upload can silently accept an empty native runtime'
}

function Test-NativeX64CMakeContract {
    param(
        [string]$Presets,
        [string]$CoreToolsCMake,
        [string]$FindFFmpegCMake,
        [string]$CompressionCMake,
        [string]$GeneralsCMake,
        [string]$ZeroHourCMake,
        [string]$NativeProductRuntimeAudit
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
    Assert-Contains $CoreToolsCMake '\$\{CMAKE_SOURCE_DIR\}\|\$\{CMAKE_BINARY_DIR\}' `
        'concurrent build trees share the nested native product audit root'
    Assert-Contains $CoreToolsCMake '\$\{_native_product_runtime_audit_configuration\}' `
        'concurrent configurations share the nested native product audit root'
    Assert-Contains $CoreToolsCMake '_native_product_runtime_audit_source_hash\}/\$\{_native_product_runtime_audit_configuration\}' `
        'the nested native product audit root is not partitioned by configuration'
    Assert-Contains $CoreToolsCMake '-Configuration "\$\{_native_product_runtime_audit_configuration\}"' `
        'the nested native product audit does not validate the selected test configuration'
    Assert-Contains $CoreToolsCMake 'string\(LENGTH "\$\{_native_product_runtime_audit_budget_root\}"' `
        'the nested native product audit has no Windows path-length budget check'
    Assert-Contains $CoreToolsCMake '(?m)^\s*if\([A-Za-z0-9_]+ GREATER 80\)\r?$' `
        'the nested native product audit does not enforce the 80-character path budget'
    Assert-Contains $CoreToolsCMake '-FFmpegRoot "\$\{FFMPEG_SDK_ROOT\}"' `
        'the nested native product audit does not receive the resolved FFmpeg SDK root'
    Assert-Contains $CoreToolsCMake '-FFmpegRuntimeDir "\$\{FFMPEG_RUNTIME_DIR\}"' `
        'the nested native product audit does not receive a custom FFmpeg runtime directory'
    Assert-Contains $CoreToolsCMake '-ToolchainFile "\$\{CMAKE_TOOLCHAIN_FILE\}"' `
        'the nested native product audit does not inherit the parent dependency toolchain'
    Assert-Contains $FindFFmpegCMake 'set\(FFMPEG_SDK_ROOT ' `
        'FFmpeg discovery does not publish its resolved SDK root'
    Assert-Contains $FindFFmpegCMake 'RTS_FFMPEG_ROOT_HINT_SIGNATURE' `
        'FFmpeg discovery does not invalidate cached SDK paths when root hints change'
    Assert-Contains $FindFFmpegCMake 'cmake_path\(NORMAL_PATH _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR\)' `
        'FFmpeg discovery dereferences a stale automatic runtime path when SDK roots change'
    Assert-Contains $FindFFmpegCMake 'ResolveWindowsRuntimeClosure\.cmake' `
        'FFmpeg packaging does not resolve app-local transitive runtime dependencies'
    Assert-Contains $FindFFmpegCMake '(?ms)set\(RTS_FFMPEG_RUNTIME_DLLS .*?CACHE INTERNAL.*?FORCE\)' `
        'FFmpeg runtime closure can remain stale after an SDK change'
    Assert-Contains $CompressionCMake 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE' `
        'vcpkg zlib runtime discovery is missing'
    Assert-Contains $GeneralsCMake 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE' `
        'Generals does not install the resolved vcpkg zlib runtime'
    Assert-Contains $ZeroHourCMake 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE' `
        'Zero Hour does not install the resolved vcpkg zlib runtime'
    Assert-Contains $NativeProductRuntimeAudit 'RTS_NATIVE_ZLIB_RUNTIME_DLLS_' `
        'the installed product audit does not validate the resolved zlib runtime'
    Assert-Contains $NativeProductRuntimeAudit '-DFFMPEG_RUNTIME_DIR=\$FFmpegRuntimeDir' `
        'the installed product audit does not preserve a custom FFmpeg runtime directory'
}

if ($SelfTest) {
    $goodCI = @'
on:
  pull_request:
jobs:
              - 'vcpkg.json'
              - 'vcpkg-lock.json'
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
key: cmake-deps-${{ hashFiles('cmake/patches/*.patch') }}
if: inputs.game == 'Generals' && inputs.preset == 'x64-vcpkg'
      - name: Run Core extras tests for game
        if: inputs.extras
        run: |
          $arguments = @('--no-tests=error')
          if ('preset' -like 'win32*' -or 'preset' -like 'x64*') {}
          & ctest @arguments
          exit $LASTEXITCODE
      - name: Install native runtime for game
        if: startsWith(inputs.preset, 'x64')
        run: |
          cmake --install build
      - name: Collect game Artifact
        run: |
          if ('preset' -like 'x64*') {
            $installedRuntime = 'installed'
            Copy-Item -Path (Join-Path $installedRuntime '*') -Destination artifacts
          }
      - name: Upload game Artifact
        uses: actions/upload-artifact@sha
        with:
          path: build\${{ inputs.preset }}\${{ inputs.game }}\artifacts
          if-no-files-found: error
'@
    $goodPresets = @'
{"configurePresets":[{"name":"x64-vcpkg","inherits":["x64","default-vcpkg"]}]}
'@
    $goodCoreToolsCMake = @'
set(audit_root "$ENV{RUNNER_TEMP}/GeneralsGameCode")
set(_native_product_runtime_audit_fallback_root "sibling")
string(SHA256 _native_product_runtime_audit_source_hash "${CMAKE_SOURCE_DIR}|${CMAKE_BINARY_DIR}")
set(audit_root "${_native_product_runtime_audit_source_hash}/${_native_product_runtime_audit_configuration}")
string(LENGTH "${_native_product_runtime_audit_budget_root}" audit_root_length)
if(audit_root_length GREATER 80)
endif()
-FFmpegRoot "${FFMPEG_SDK_ROOT}"
-FFmpegRuntimeDir "${FFMPEG_RUNTIME_DIR}"
-Configuration "${_native_product_runtime_audit_configuration}"
-ToolchainFile "${CMAKE_TOOLCHAIN_FILE}"
'@
    $goodFindFFmpegCMake = @'
set(FFMPEG_SDK_ROOT "resolved" CACHE PATH "root")
set(RTS_FFMPEG_ROOT_HINT_SIGNATURE "signature" CACHE INTERNAL "signature" FORCE)
cmake_path(NORMAL_PATH _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
-P "ResolveWindowsRuntimeClosure.cmake"
set(RTS_FFMPEG_RUNTIME_DLLS "closure" CACHE INTERNAL "closure" FORCE)
'@
    $goodCompressionCMake = 'set(RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE "zlib1.dll")'
    $goodGeneralsCMake = 'install(FILES ${RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE})'
    $goodZeroHourCMake = 'install(FILES ${RTS_NATIVE_ZLIB_RUNTIME_DLLS_RELEASE})'
    $goodNativeProductRuntimeAudit = @'
$cacheName = "RTS_NATIVE_ZLIB_RUNTIME_DLLS_$Configuration"
$arguments += "-DFFMPEG_RUNTIME_DIR=$FFmpegRuntimeDir"
'@

    Test-NativeX64CIWorkflow -CIWorkflow $goodCI -BuildWorkflow $goodBuild
    Test-NativeX64CMakeContract `
        -Presets $goodPresets `
        -CoreToolsCMake $goodCoreToolsCMake `
        -FindFFmpegCMake $goodFindFFmpegCMake `
        -CompressionCMake $goodCompressionCMake `
        -GeneralsCMake $goodGeneralsCMake `
        -ZeroHourCMake $goodZeroHourCMake `
        -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit

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

    $caughtMissingPatchHash = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow $goodCI `
            -BuildWorkflow ($goodBuild -replace "'cmake/patches/\*\.patch'", "'cmake/patches/ignored.txt'")
    } catch {
        $caughtMissingPatchHash = $true
    }
    if (-not $caughtMissingPatchHash) {
        throw 'Native x64 CI workflow audit self-test failed to reject a stale D3D8 patch cache key'
    }

    $caughtSkippedCTest = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow $goodCI `
            -BuildWorkflow ($goodBuild -replace '        if: inputs\.extras\r?\n', '')
    } catch {
        $caughtSkippedCTest = $true
    }
    if (-not $caughtSkippedCTest) {
        throw 'Native x64 CI workflow audit self-test failed to reject an unbound CTest predicate'
    }

    $caughtRawBuildArtifact = $false
    try {
        Test-NativeX64CIWorkflow `
            -CIWorkflow $goodCI `
            -BuildWorkflow ($goodBuild -replace '\(Join-Path \$installedRuntime ''\*''\)', "'build\\raw\\*'")
    } catch {
        $caughtRawBuildArtifact = $true
    }
    if (-not $caughtRawBuildArtifact) {
        throw 'Native x64 CI workflow audit self-test failed to reject raw build artifact collection'
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
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtCheckoutAuditRoot = $true
    }
    if (-not $caughtCheckoutAuditRoot) {
        throw 'Native x64 CI workflow audit self-test failed to reject checkout-local audit storage'
    }

    $caughtSharedConfigurationRoot = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake ($goodCoreToolsCMake -replace '_native_product_runtime_audit_source_hash\}/\$\{_native_product_runtime_audit_configuration\}', '_native_product_runtime_audit_source_hash}/shared') `
            -FindFFmpegCMake $goodFindFFmpegCMake `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtSharedConfigurationRoot = $true
    }
    if (-not $caughtSharedConfigurationRoot) {
        throw 'Native x64 CI workflow audit self-test failed to reject a shared configuration audit root'
    }

    $caughtMissingConfiguration = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake ($goodCoreToolsCMake -replace '-Configuration "\$\{_native_product_runtime_audit_configuration\}"', '') `
            -FindFFmpegCMake $goodFindFFmpegCMake `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtMissingConfiguration = $true
    }
    if (-not $caughtMissingConfiguration) {
        throw 'Native x64 CI workflow audit self-test failed to reject missing configuration propagation'
    }

    $caughtMissingRuntimeOverride = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake ($goodCoreToolsCMake -replace '-FFmpegRuntimeDir "\$\{FFMPEG_RUNTIME_DIR\}"', '') `
            -FindFFmpegCMake $goodFindFFmpegCMake `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtMissingRuntimeOverride = $true
    }
    if (-not $caughtMissingRuntimeOverride) {
        throw 'Native x64 CI workflow audit self-test failed to reject a dropped FFmpeg runtime override'
    }

    $caughtMissingRuntimeClosure = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake $goodCoreToolsCMake `
            -FindFFmpegCMake ($goodFindFFmpegCMake -replace 'ResolveWindowsRuntimeClosure\.cmake', 'MissingClosure.cmake') `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtMissingRuntimeClosure = $true
    }
    if (-not $caughtMissingRuntimeClosure) {
        throw 'Native x64 CI workflow audit self-test failed to reject a missing FFmpeg runtime closure'
    }

    $caughtStaleRuntimeDereference = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake $goodCoreToolsCMake `
            -FindFFmpegCMake ($goodFindFFmpegCMake -replace 'cmake_path\(NORMAL_PATH _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR\)', 'file(REAL_PATH ${RTS_FFMPEG_AUTO_RUNTIME_DIR} _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)') `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtStaleRuntimeDereference = $true
    }
    if (-not $caughtStaleRuntimeDereference) {
        throw 'Native x64 CI workflow audit self-test failed to reject stale FFmpeg runtime dereferencing'
    }

    $caughtMissingPathBudget = $false
    try {
        Test-NativeX64CMakeContract `
            -Presets $goodPresets `
            -CoreToolsCMake ($goodCoreToolsCMake -replace 'if\(audit_root_length GREATER 80\)', 'if(audit_root_length GREATER 800)') `
            -FindFFmpegCMake $goodFindFFmpegCMake `
            -CompressionCMake $goodCompressionCMake `
            -GeneralsCMake $goodGeneralsCMake `
            -ZeroHourCMake $goodZeroHourCMake `
            -NativeProductRuntimeAudit $goodNativeProductRuntimeAudit
    } catch {
        $caughtMissingPathBudget = $true
    }
    if (-not $caughtMissingPathBudget) {
        throw 'Native x64 CI workflow audit self-test failed to reject a missing path-length budget'
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
$nativeProductRuntimeAuditPath = Join-Path $SourceRoot 'Core/Tools/NativeProductRuntimeAudit/NativeProductRuntimeAudit.ps1'
Test-NativeX64CIWorkflow `
    -CIWorkflow (Get-Content -Raw -LiteralPath $ciPath) `
    -BuildWorkflow (Get-Content -Raw -LiteralPath $buildPath)
Test-NativeX64CMakeContract `
    -Presets (Get-Content -Raw -LiteralPath $presetsPath) `
    -CoreToolsCMake (Get-Content -Raw -LiteralPath $coreToolsCMakePath) `
    -FindFFmpegCMake (Get-Content -Raw -LiteralPath $findFFmpegCMakePath) `
    -CompressionCMake (Get-Content -Raw -LiteralPath $compressionCMakePath) `
    -GeneralsCMake (Get-Content -Raw -LiteralPath $generalsCMakePath) `
    -ZeroHourCMake (Get-Content -Raw -LiteralPath $zeroHourCMakePath) `
    -NativeProductRuntimeAudit (Get-Content -Raw -LiteralPath $nativeProductRuntimeAuditPath)

Write-Host 'Native x64 CI workflow audit passed.'
