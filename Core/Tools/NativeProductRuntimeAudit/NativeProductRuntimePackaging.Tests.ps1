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
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
Assert-True (Test-Path -LiteralPath $vswhere -PathType Leaf) 'Visual Studio discovery is unavailable.'
$vsInstallation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
Assert-True (-not [string]::IsNullOrWhiteSpace($vsInstallation)) 'Visual Studio C++ tools are unavailable.'
$vsRedistParent = Join-Path $vsInstallation 'VC/Redist/MSVC'
$vsRedistRoot = (Get-ChildItem -LiteralPath $vsRedistParent -Directory |
    Where-Object { $_.Name -match '^\d+\.' } | Sort-Object Name -Descending |
    Select-Object -First 1).FullName
$windowsKitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$windowsKitsBin = Join-Path $windowsKitsRoot 'bin'
$windowsKitsVersion = (Get-ChildItem -LiteralPath $windowsKitsBin -Directory |
    Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1).Name
Assert-True (Test-Path -LiteralPath $vsRedistRoot -PathType Container) 'The VS x64 redist fixture root is unavailable.'
Assert-True (Test-Path -LiteralPath $windowsKitsRoot -PathType Container) 'The Windows Kits fixture root is unavailable.'
Assert-True (-not [string]::IsNullOrWhiteSpace($windowsKitsVersion)) 'A Windows Kits version is unavailable.'

$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
} elseif ([string]::IsNullOrWhiteSpace($env:RTS_TEST_SCRATCH_ROOT)) {
    [IO.Path]::GetTempPath()
} else {
    [IO.Path]::GetFullPath($env:RTS_TEST_SCRATCH_ROOT)
}
$testRoot = Join-Path $scratchParent ("native-product-runtime-packaging-" + [Guid]::NewGuid().ToString('N'))
$buildRoot = Join-Path $testRoot 'build'
$releaseInstallRoot = Join-Path $testRoot 'release-install'
$debugInstallRoot = Join-Path $testRoot 'debug-install'

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

    & $cmake -S $fixtureRoot -B $buildRoot -G Ninja `
        "-DRTS_PACKAGING_MODULE=$sourceRoot/cmake/native-product-packaging.cmake" `
        '-DRTS_BUILD_OUTPUT_SUFFIX=-stage3' `
        "-DRTS_TEST_MSVC_REDIST_DIR=$vsRedistRoot" `
        "-DRTS_TEST_WINDOWS_KITS_DIR=$windowsKitsRoot" `
        "-DRTS_TEST_WINDOWS_KITS_VERSION=$windowsKitsVersion"
    Assert-True ($LASTEXITCODE -eq 0) 'Packaging fixture configuration failed.'

    & $cmake --install $buildRoot --config Release --prefix $releaseInstallRoot
    Assert-True ($LASTEXITCODE -eq 0) 'Release packaging fixture installation failed.'

    $releaseProductRoot = Join-Path $releaseInstallRoot 'Generals'
    $releaseZeroHourRoot = Join-Path $releaseInstallRoot 'ZeroHour'
    $releaseExecutable = Join-Path $releaseProductRoot 'generalsv-stage3.exe'
    $releaseLauncher = Join-Path $releaseProductRoot 'launcher.lcf'
    Assert-File $releaseExecutable 'The suffixed release executable'
    Assert-File $releaseLauncher 'The generated release launcher configuration'
    Assert-True ((Get-Content -LiteralPath $releaseLauncher -Raw).Trim() -ceq 'RUN = . generalsv-stage3.exe') `
        'The release launcher does not name the target output file.'
    Assert-File (Join-Path $releaseZeroHourRoot 'generalszh-stage3.exe') 'The suffixed Zero Hour release executable'
    Assert-True ((Get-Content -LiteralPath (Join-Path $releaseZeroHourRoot 'launcher.lcf') -Raw).Trim() -ceq 'RUN = . generalszh-stage3.exe') `
        'The Zero Hour launcher does not name the target output file.'
    Assert-File (Join-Path $releaseProductRoot 'msvcp140.dll') 'The release MSVC C++ runtime'
    Assert-File (Join-Path $releaseProductRoot 'vcruntime140.dll') 'The release MSVC runtime'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $releaseProductRoot 'msvcp140d.dll'))) `
        'The release install incorrectly contains the debug MSVC C++ runtime.'

    $releaseHash = Get-Sha256 (Join-Path $releaseProductRoot 'msvcp140.dll')

    & $cmake --install $buildRoot --config Debug --prefix $debugInstallRoot
    Assert-True ($LASTEXITCODE -eq 0) 'Debug packaging fixture installation failed.'

    $debugProductRoot = Join-Path $debugInstallRoot 'Generals'
    $debugZeroHourRoot = Join-Path $debugInstallRoot 'ZeroHour'
    Assert-File (Join-Path $debugProductRoot 'generalsv-stage3.exe') 'The suffixed debug executable'
    Assert-True ((Get-Content -LiteralPath (Join-Path $debugProductRoot 'launcher.lcf') -Raw).Trim() -ceq 'RUN = . generalsv-stage3.exe') `
        'The debug launcher does not name the target output file.'
    Assert-File (Join-Path $debugZeroHourRoot 'generalszh-stage3.exe') 'The suffixed Zero Hour debug executable'
    Assert-True ((Get-Content -LiteralPath (Join-Path $debugZeroHourRoot 'launcher.lcf') -Raw).Trim() -ceq 'RUN = . generalszh-stage3.exe') `
        'The Zero Hour debug launcher does not name the target output file.'
    Assert-File (Join-Path $debugProductRoot 'msvcp140d.dll') 'The debug MSVC C++ runtime'
    Assert-File (Join-Path $debugProductRoot 'vcruntime140d.dll') 'The debug MSVC runtime'
    Assert-File (Join-Path $debugProductRoot 'ucrtbased.dll') 'The debug Universal CRT runtime'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $debugProductRoot 'msvcp140.dll'))) `
        'The debug install incorrectly contains the release MSVC C++ runtime.'

    & $cmake --install $buildRoot --config Debug --prefix $releaseInstallRoot
    Assert-True ($LASTEXITCODE -eq 0) 'Debug packaging fixture same-prefix installation failed.'
    Assert-File (Join-Path $releaseProductRoot 'msvcp140d.dll') `
        'The same-prefix Debug install MSVC C++ runtime'
    Assert-True ((Get-Sha256 (Join-Path $releaseProductRoot 'msvcp140.dll')) -ceq $releaseHash) `
        'The same-prefix Debug installation changed the previously installed release runtime.'

    Write-Output 'Native product runtime packaging configure/install fixture passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
