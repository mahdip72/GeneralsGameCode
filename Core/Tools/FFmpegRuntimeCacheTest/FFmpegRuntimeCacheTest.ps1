param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$ScratchRoot,
    [Parameter(Mandatory = $true)]
    [string]$FFmpegRoot,
    [Parameter(Mandatory = $true)]
    [string]$FFmpegRuntimeDir
)

$ErrorActionPreference = 'Stop'

function Invoke-FixtureConfigure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [bool]$ExpectSuccess = $true,
        [string]$ExpectedFailure = ''
    )

    $buildRoot = Join-Path $script:RunRoot $Name
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & cmake -Werror=dev -S $script:FixtureSource -B $buildRoot @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    $text = ($output | Out-String)
    if ($ExpectSuccess -and $exitCode -ne 0) {
        throw "FFmpeg runtime cache fixture '$Name' failed:`n$text"
    }
    if (-not $ExpectSuccess) {
        if ($exitCode -eq 0) {
            throw "FFmpeg runtime cache fixture '$Name' unexpectedly succeeded."
        }
        if (-not $text.Contains($ExpectedFailure)) {
            throw "FFmpeg runtime cache fixture '$Name' failed for the wrong reason:`n$text"
        }
    }
}

$resolvedSourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$resolvedScratchRoot = [IO.Path]::GetFullPath($ScratchRoot)
$resolvedFFmpegRoot = [IO.Path]::GetFullPath($FFmpegRoot)
$resolvedRuntimeDir = [IO.Path]::GetFullPath($FFmpegRuntimeDir)
if (-not (Test-Path -LiteralPath $resolvedSourceRoot -PathType Container)) {
    throw "Source root does not exist: $resolvedSourceRoot"
}
if (-not (Test-Path -LiteralPath $resolvedFFmpegRoot -PathType Container)) {
    throw "FFmpeg SDK root does not exist: $resolvedFFmpegRoot"
}
if (-not (Test-Path -LiteralPath $resolvedRuntimeDir -PathType Container)) {
    throw "FFmpeg runtime directory does not exist: $resolvedRuntimeDir"
}

New-Item -ItemType Directory -Path $resolvedScratchRoot -Force | Out-Null
$script:RunRoot = Join-Path $resolvedScratchRoot ([guid]::NewGuid().ToString('N'))
$script:FixtureSource = Join-Path $script:RunRoot 'source'
New-Item -ItemType Directory -Path $script:FixtureSource -Force | Out-Null

try {
    $fixture = @'
cmake_minimum_required(VERSION 3.25)
project(FFmpegRuntimeCacheFixture NONE)
set(CMAKE_SIZEOF_VOID_P 8)
list(PREPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
find_package(FFMPEG REQUIRED)
file(REAL_PATH "${EXPECTED_RUNTIME}" _expected_runtime)
file(REAL_PATH "${FFMPEG_RUNTIME_DIR}" _actual_runtime)
string(TOLOWER "${_expected_runtime}" _expected_runtime)
string(TOLOWER "${_actual_runtime}" _actual_runtime)
if(NOT _actual_runtime STREQUAL _expected_runtime)
    message(FATAL_ERROR "Unexpected FFmpeg runtime: ${FFMPEG_RUNTIME_DIR}")
endif()
if(EXPECT_AUTO_RUNTIME)
    if(NOT DEFINED RTS_FFMPEG_AUTO_RUNTIME_DIR)
        message(FATAL_ERROR "Automatic FFmpeg runtime provenance was not retained")
    endif()
elseif(DEFINED RTS_FFMPEG_AUTO_RUNTIME_DIR)
    message(FATAL_ERROR "Custom FFmpeg runtime retained automatic provenance")
endif()
'@
    Set-Content -LiteralPath (Join-Path $script:FixtureSource 'CMakeLists.txt') `
        -Value $fixture -Encoding UTF8

    $staleRuntime = Join-Path $script:RunRoot 'removed-sdk\bin'
    $common = @(
        "-DREPO_ROOT=$resolvedSourceRoot",
        "-DFFMPEG_ROOT=$resolvedFFmpegRoot",
        '-DRTS_FFMPEG_ROOT_HINT_SIGNATURE=stale-fixture-signature'
    )
    Invoke-FixtureConfigure -Name 'automatic-switch' -Arguments ($common + @(
        "-DFFMPEG_RUNTIME_DIR=$staleRuntime",
        "-DRTS_FFMPEG_AUTO_RUNTIME_DIR:INTERNAL=$staleRuntime",
        "-DEXPECTED_RUNTIME=$(Join-Path $resolvedFFmpegRoot 'bin')",
        '-DEXPECT_AUTO_RUNTIME=ON'
    ))

    $customRuntime = Join-Path $script:RunRoot 'custom-runtime'
    New-Item -ItemType Directory -Path $customRuntime -Force | Out-Null
    $runtimeFiles = @(Get-ChildItem -LiteralPath $resolvedRuntimeDir -Filter '*.dll' -File)
    if ($runtimeFiles.Count -eq 0) {
        throw "No FFmpeg runtime DLLs were found in $resolvedRuntimeDir"
    }
    foreach ($runtimeFile in $runtimeFiles) {
        $destination = Join-Path $customRuntime $runtimeFile.Name
        try {
            New-Item -ItemType HardLink -Path $destination `
                -Target $runtimeFile.FullName -ErrorAction Stop | Out-Null
        } catch {
            Copy-Item -LiteralPath $runtimeFile.FullName -Destination $destination
        }
    }
    Invoke-FixtureConfigure -Name 'custom-preserved' -Arguments ($common + @(
        "-DFFMPEG_RUNTIME_DIR=$customRuntime",
        "-DRTS_FFMPEG_AUTO_RUNTIME_DIR:INTERNAL=$staleRuntime",
        "-DEXPECTED_RUNTIME=$customRuntime",
        '-DEXPECT_AUTO_RUNTIME=OFF'
    ))

    $missingCustomRuntime = Join-Path $script:RunRoot 'missing-custom-runtime'
    Invoke-FixtureConfigure -Name 'missing-custom-rejected' -ExpectSuccess $false `
        -ExpectedFailure 'FFmpeg runtime directory does not exist' `
        -Arguments ($common + @(
            "-DFFMPEG_RUNTIME_DIR=$missingCustomRuntime",
            "-DRTS_FFMPEG_AUTO_RUNTIME_DIR:INTERNAL=$staleRuntime",
            "-DEXPECTED_RUNTIME=$missingCustomRuntime",
            '-DEXPECT_AUTO_RUNTIME=OFF'
        ))

    Write-Host 'FFmpeg runtime cache tests passed.'
} finally {
    $resolvedRunRoot = [IO.Path]::GetFullPath($script:RunRoot)
    $scratchPrefix = $resolvedScratchRoot.TrimEnd('\') + '\'
    if ($resolvedRunRoot.StartsWith($scratchPrefix,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedRunRoot).Length -eq 32 -and
        (Test-Path -LiteralPath $resolvedRunRoot)) {
        Remove-Item -LiteralPath $resolvedRunRoot -Recurse -Force
    }
}
