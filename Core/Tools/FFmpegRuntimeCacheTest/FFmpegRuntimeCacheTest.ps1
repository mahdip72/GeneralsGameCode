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

function Copy-SdkTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,
        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $sourceFull = [IO.Path]::GetFullPath($Source).TrimEnd('\')
    $sourcePrefix = $sourceFull + '\'
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($sourceFile in @(Get-ChildItem -LiteralPath $Source -Recurse -File)) {
        if (-not $sourceFile.FullName.StartsWith($sourcePrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "SDK file escaped the source root: $($sourceFile.FullName)"
        }
        $relative = $sourceFile.FullName.Substring($sourcePrefix.Length)
        $destinationFile = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destinationFile) `
            -Force | Out-Null
        try {
            New-Item -ItemType HardLink -Path $destinationFile `
                -Target $sourceFile.FullName -ErrorAction Stop | Out-Null
        } catch {
            Copy-Item -LiteralPath $sourceFile.FullName -Destination $destinationFile
        }
    }
}

function Get-FixtureCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Key
    )

    $cachePath = Join-Path (Join-Path $script:RunRoot $Name) 'CMakeCache.txt'
    $prefix = $Key + ':'
    $entry = @(Get-Content -LiteralPath $cachePath | Where-Object {
        $_.StartsWith($prefix, [StringComparison]::Ordinal)
    })
    if ($entry.Count -ne 1) {
        throw "Expected one '$Key' cache entry in fixture '$Name'."
    }
    return $entry[0].Substring($entry[0].IndexOf('=') + 1)
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
if(EXPECT_HINT_PRECEDENCE)
    set(_expected_hints "${RTS_FFMPEG_ROOT};$ENV{RTS_FFMPEG_ROOT}")
    list(REMOVE_DUPLICATES _expected_hints)
    string(SHA256 _expected_hint_signature "${_expected_hints}")
endif()
find_package(FFMPEG REQUIRED)
if(EXPECT_HINT_PRECEDENCE)
    if(NOT RTS_FFMPEG_ROOT_HINT_SIGNATURE STREQUAL _expected_hint_signature)
        message(FATAL_ERROR "FFmpeg hint signature lost explicit-root precedence")
    endif()
endif()
if(DEFINED EXPECTED_SDK)
    file(REAL_PATH "${EXPECTED_SDK}" _expected_sdk)
    file(REAL_PATH "${FFMPEG_SDK_ROOT}" _actual_sdk)
    string(TOLOWER "${_expected_sdk}" _expected_sdk)
    string(TOLOWER "${_actual_sdk}" _actual_sdk)
    if(NOT _actual_sdk STREQUAL _expected_sdk)
        message(FATAL_ERROR "Unexpected FFmpeg SDK: ${FFMPEG_SDK_ROOT}")
    endif()
endif()
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

    $environmentRoot = Join-Path $script:RunRoot 'a-environment-root'
    $explicitRoot = Join-Path $script:RunRoot 'z-explicit-root'
    $replacementExplicitRoot = Join-Path $script:RunRoot 'y-replacement-explicit-root'
    Copy-SdkTree -Source $resolvedFFmpegRoot -Destination $environmentRoot
    Copy-SdkTree -Source $resolvedFFmpegRoot -Destination $explicitRoot
    Copy-SdkTree -Source $resolvedFFmpegRoot -Destination $replacementExplicitRoot
    $previousFFmpegEnvironmentRoot = $env:FFMPEG_ROOT
    $previousRtsEnvironmentRoot = $env:RTS_FFMPEG_ROOT
    try {
        Remove-Item Env:FFMPEG_ROOT -ErrorAction SilentlyContinue
        $env:RTS_FFMPEG_ROOT = $environmentRoot.Replace('\', '/')
        Invoke-FixtureConfigure -Name 'explicit-root-precedence' -Arguments @(
            "-DREPO_ROOT=$resolvedSourceRoot",
            "-DRTS_FFMPEG_ROOT:PATH=$explicitRoot",
            "-DEXPECTED_SDK=$explicitRoot",
            "-DEXPECTED_RUNTIME=$(Join-Path $explicitRoot 'bin')",
            '-DEXPECT_HINT_PRECEDENCE=ON',
            '-DEXPECT_AUTO_RUNTIME=ON'
        )
        $firstSignature = Get-FixtureCacheValue -Name 'explicit-root-precedence' `
            -Key 'RTS_FFMPEG_ROOT_HINT_SIGNATURE'

        $replacementArguments = @(
            "-DREPO_ROOT=$resolvedSourceRoot",
            "-DRTS_FFMPEG_ROOT:PATH=$replacementExplicitRoot",
            "-DEXPECTED_SDK=$replacementExplicitRoot",
            "-DEXPECTED_RUNTIME=$(Join-Path $replacementExplicitRoot 'bin')",
            '-DEXPECT_HINT_PRECEDENCE=ON',
            '-DEXPECT_AUTO_RUNTIME=ON'
        )
        Invoke-FixtureConfigure -Name 'explicit-root-precedence' `
            -Arguments $replacementArguments
        $replacementSignature = Get-FixtureCacheValue -Name 'explicit-root-precedence' `
            -Key 'RTS_FFMPEG_ROOT_HINT_SIGNATURE'
        if ($replacementSignature -eq $firstSignature) {
            throw 'FFmpeg root change did not invalidate the cached hint signature.'
        }

        Invoke-FixtureConfigure -Name 'explicit-root-precedence' `
            -Arguments $replacementArguments
        $stableSignature = Get-FixtureCacheValue -Name 'explicit-root-precedence' `
            -Key 'RTS_FFMPEG_ROOT_HINT_SIGNATURE'
        if ($stableSignature -ne $replacementSignature) {
            throw 'Identical FFmpeg root hints did not retain a stable signature.'
        }
    } finally {
        if ($null -eq $previousFFmpegEnvironmentRoot) {
            Remove-Item Env:FFMPEG_ROOT -ErrorAction SilentlyContinue
        } else {
            $env:FFMPEG_ROOT = $previousFFmpegEnvironmentRoot
        }
        if ($null -eq $previousRtsEnvironmentRoot) {
            Remove-Item Env:RTS_FFMPEG_ROOT -ErrorAction SilentlyContinue
        } else {
            $env:RTS_FFMPEG_ROOT = $previousRtsEnvironmentRoot
        }
    }

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
