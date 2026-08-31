param(
    [string] $ScratchRoot
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-File([string] $Path, [string] $Description) {
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "$Description is missing: $Path"
}

function Get-Sha256([string] $Path) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            return [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha256.Dispose()
    }
}

$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$fixtureRoot = Join-Path $PSScriptRoot 'Fixtures'
$deploymentAudit = Join-Path $PSScriptRoot 'AsanRuntimeDeploymentAudit.ps1'
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
} elseif ([string]::IsNullOrWhiteSpace($env:RTS_TEST_SCRATCH_ROOT)) {
    [IO.Path]::GetTempPath()
} else {
    [IO.Path]::GetFullPath($env:RTS_TEST_SCRATCH_ROOT)
}

$testRoot = Join-Path $scratchParent ("asan-runtime-packaging-" + [Guid]::NewGuid().ToString('N'))
$compilerRoot = Join-Path $testRoot 'compiler-bin'
$fakeCompiler = Join-Path $compilerRoot 'cl.exe'
$runtimeName = 'clang_rt.asan_dynamic-x86_64.dll'
$fakeRuntime = Join-Path $compilerRoot $runtimeName
$negativeBuildRoot = Join-Path $testRoot 'negative-build'
$disabledBuildRoot = Join-Path $testRoot 'disabled-build'
$positiveBuildRoot = Join-Path $testRoot 'positive-build'
$singleConfigBuildRoot = Join-Path $testRoot 'single-config-build'
$installRoot = Join-Path $testRoot 'install'
$singleConfigInstallRoot = Join-Path $testRoot 'single-config-install'

try {
    New-Item -ItemType Directory -Path $compilerRoot -Force | Out-Null
    [IO.File]::WriteAllText($fakeCompiler, 'compiler fixture')

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $negativeOutput = & $cmake -S $fixtureRoot -B $negativeBuildRoot -G 'Ninja Multi-Config' `
            "-DRTS_ASAN_MODULE=$sourceRoot/cmake/asan.cmake" `
            "-DRTS_TEST_COMPILER=$fakeCompiler" 2>&1
        $negativeExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    Assert-True ($negativeExitCode -ne 0) 'ASan configure unexpectedly passed without its runtime DLL.'
    Assert-True (($negativeOutput -join "`n") -match 'clang_rt\.asan_dynamic-x86_64\.dll') `
        'The negative ASan fixture did not report the missing runtime DLL.'

    & $cmake -S $fixtureRoot -B $disabledBuildRoot -G 'Ninja Multi-Config' `
        "-DRTS_ASAN_MODULE=$sourceRoot/cmake/asan.cmake" `
        "-DRTS_TEST_COMPILER=$fakeCompiler" `
        '-DRTS_BUILD_OPTION_ASAN=OFF'
    Assert-True ($LASTEXITCODE -eq 0) `
        'A non-ASan product graph could not call the no-op ASan install boundary.'

    [IO.File]::WriteAllText($fakeRuntime, 'ASan runtime fixture')
    & $cmake -S $fixtureRoot -B $positiveBuildRoot -G 'Ninja Multi-Config' `
        "-DRTS_ASAN_MODULE=$sourceRoot/cmake/asan.cmake" `
        "-DRTS_TEST_COMPILER=$fakeCompiler"
    Assert-True ($LASTEXITCODE -eq 0) 'ASan configure failed with its runtime DLL present.'

    $stagedDebugRuntime = Join-Path $positiveBuildRoot "Core/Debug/$runtimeName"
    $stagedReleaseRuntime = Join-Path $positiveBuildRoot "Core/Release/$runtimeName"
    $stagedExecutable = Join-Path $positiveBuildRoot 'Core/Debug/core_job_system_tests.exe'
    $stagedReleaseExecutable = Join-Path $positiveBuildRoot 'Core/Release/core_job_system_tests.exe'
    Assert-File $stagedDebugRuntime 'The staged Debug ASan runtime'
    Assert-File $stagedReleaseRuntime 'The staged Release ASan runtime'
    Assert-File $stagedExecutable 'The fixture ASan executable'
    Assert-File $stagedReleaseExecutable 'The fixture Release ASan executable'
    $sourceHash = Get-Sha256 $fakeRuntime
    Assert-True ((Get-Sha256 $stagedDebugRuntime) -ceq $sourceHash) `
        'The staged Debug ASan runtime differs from the resolved compiler runtime.'
    Assert-True ((Get-Sha256 $stagedReleaseRuntime) -ceq $sourceHash) `
        'The staged Release ASan runtime differs from the resolved compiler runtime.'

    & $cmake -S $fixtureRoot -B $singleConfigBuildRoot -G Ninja `
        '-DCMAKE_BUILD_TYPE=Debug' `
        "-DRTS_ASAN_MODULE=$sourceRoot/cmake/asan.cmake" `
        "-DRTS_TEST_COMPILER=$fakeCompiler"
    Assert-True ($LASTEXITCODE -eq 0) 'Single-config ASan configure failed.'
    $singleConfigRuntime = Join-Path $singleConfigBuildRoot "Core/$runtimeName"
    $singleConfigExecutable = Join-Path $singleConfigBuildRoot 'Core/core_job_system_tests.exe'
    Assert-File $singleConfigRuntime 'The staged single-config ASan runtime'
    Assert-File $singleConfigExecutable 'The single-config ASan executable fixture'
    Assert-True ((Get-Sha256 $singleConfigRuntime) -ceq $sourceHash) `
        'The staged single-config ASan runtime differs from the resolved compiler runtime.'
    & $cmake --install $singleConfigBuildRoot --config Debug --prefix $singleConfigInstallRoot
    Assert-True ($LASTEXITCODE -eq 0) 'Single-config ASan install fixture failed.'
    Assert-File (Join-Path $singleConfigInstallRoot "NativeProduct/$runtimeName") `
        'The installed single-config ASan runtime'
    Assert-File (Join-Path $singleConfigInstallRoot 'NativeProduct/core_job_system_tests.exe') `
        'The installed single-config ASan executable'

    Remove-Item -LiteralPath $stagedDebugRuntime -Force
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $missingRuntimeOutput = & powershell -NoProfile -ExecutionPolicy Bypass `
            -File $deploymentAudit -BuildRoot $positiveBuildRoot -RuntimeDll $fakeRuntime 2>&1
        $missingRuntimeExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    Assert-True ($missingRuntimeExitCode -ne 0) `
        'The deployment audit skipped an executable directory with a missing ASan runtime.'
    Assert-True (($missingRuntimeOutput -join "`n") -match 'ASan runtime is not staged beside') `
        'The deployment audit did not identify the executable missing its ASan runtime.'
    [IO.File]::Copy($fakeRuntime, $stagedDebugRuntime, $true)

    & powershell -NoProfile -ExecutionPolicy Bypass -File $deploymentAudit `
        -BuildRoot $positiveBuildRoot -RuntimeDll $fakeRuntime
    Assert-True ($LASTEXITCODE -eq 0) 'The deployment audit rejected complete ASan runtime staging.'

    & $cmake --install $positiveBuildRoot --config Debug --prefix $installRoot
    Assert-True ($LASTEXITCODE -eq 0) 'ASan install fixture failed.'
    $installedRoot = Join-Path $installRoot 'NativeProduct'
    Assert-File (Join-Path $installedRoot $runtimeName) 'The installed ASan runtime'
    Assert-File (Join-Path $installedRoot 'core_job_system_tests.exe') 'The installed ASan executable'
    Assert-True ((Get-Sha256 (Join-Path $installedRoot $runtimeName)) -ceq $sourceHash) `
        'The installed ASan runtime differs from the resolved compiler runtime.'

    Write-Output 'ASan runtime negative/positive, multi-config, and single-config packaging fixtures passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
