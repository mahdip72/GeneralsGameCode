param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

function Assert-TokenCount {
    param(
        [string]$Content,
        [string]$Pattern,
        [int]$ExpectedCount,
        [string]$FailureMessage
    )

    if ([regex]::Matches($Content, $Pattern).Count -ne $ExpectedCount) {
        throw $FailureMessage
    }
}

$headers = @(
    'Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h',
    'GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h'
)

foreach ($relativePath in $headers) {
    $content = Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw
    if ($content -notmatch '(?ms)^\s*#ifdef RTS_HAS_FFMPEG\s*^\s*#include "VideoDevice/FFmpeg/FFmpegVideoPlayer\.h"\s*^\s*#else\s*^\s*#include "VideoDevice/Bink/BinkVideoPlayer\.h"\s*^\s*#endif') {
        throw "FFmpeg/Bink video header selection contract is missing in $relativePath"
    }
    Assert-TokenCount $content 'VideoDevice/FFmpeg/FFmpegVideoPlayer\.h' 1 "FFmpeg header ownership is ambiguous in $relativePath"
    Assert-TokenCount $content 'VideoDevice/Bink/BinkVideoPlayer\.h' 1 "Bink header ownership is ambiguous in $relativePath"
    if ($content -notmatch '(?ms)^\s*#ifdef RTS_HAS_FFMPEG\s*^\s*virtual VideoPlayerInterface \*createVideoPlayer\(\) override \{ return NEW FFmpegVideoPlayer; \}\s*^\s*#else\s*^\s*virtual VideoPlayerInterface \*createVideoPlayer\(\) override \{ return NEW BinkVideoPlayer; \}\s*^\s*#endif') {
        throw "FFmpeg/Bink video backend selection contract is missing in $relativePath"
    }
}

$deviceCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/CMakeLists.txt') -Raw
if ($deviceCMake -notmatch '(?ms)^if\(NOT RTS_BUILD_OPTION_FFMPEG\)\s*^    list\(APPEND GAMEENGINEDEVICE_SRC\s*^        Include/VideoDevice/Bink/BinkVideoPlayer\.h\s*^        Source/VideoDevice/Bink/BinkVideoPlayer\.cpp\s*^    \)\s*^endif\(\)') {
    throw 'Bink source ownership is not conditional on the FFmpeg backend option.'
}
Assert-TokenCount $deviceCMake 'BinkVideoPlayer\.h' 1 'Bink header source ownership is ambiguous.'
Assert-TokenCount $deviceCMake 'BinkVideoPlayer\.cpp' 1 'Bink implementation source ownership is ambiguous.'

$runtimeCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'cmake/legacy-product-runtime.cmake') -Raw
if ($runtimeCMake -notmatch '(?ms)^    if\(NOT RTS_BUILD_OPTION_FFMPEG\)\s*^        target_link_libraries\(rts_legacy_product_runtime INTERFACE binkstub\)\s*^    endif\(\)') {
    throw 'Bink link ownership is not conditional on the FFmpeg backend option.'
}
Assert-TokenCount $runtimeCMake 'binkstub' 1 'Bink runtime link ownership is ambiguous.'

$rootCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt') -Raw
$configIndex = $rootCMake.IndexOf('include(cmake/config.cmake)', [System.StringComparison]::Ordinal)
$binkIndex = $rootCMake.IndexOf('include(cmake/bink.cmake)', [System.StringComparison]::Ordinal)
if ($configIndex -lt 0 -or $binkIndex -lt 0 -or $configIndex -ge $binkIndex) {
    throw 'Build configuration must be available before selecting video backend dependencies.'
}
if ($rootCMake -notmatch '(?ms)^    if\(NOT RTS_BUILD_OPTION_FFMPEG\)\s*^        include\(cmake/bink\.cmake\)\s*^    endif\(\)') {
    throw 'Bink dependency fetch is not conditional on the FFmpeg backend option.'
}
Assert-TokenCount $rootCMake 'include\(cmake/bink\.cmake\)' 1 'Bink dependency fetch ownership is ambiguous.'

$runtimeTestsCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'GeneralsMD/Code/Tools/RuntimeRegressionTests/CMakeLists.txt') -Raw
if ($runtimeTestsCMake -notmatch '(?ms)^if\(NOT RTS_BUILD_OPTION_FFMPEG\)\s*^\s*target_link_libraries\(z_runtime_regression_tests PRIVATE binkstub\)\s*^endif\(\)') {
    throw 'Zero Hour runtime regression tests link Bink outside the fallback backend.'
}
Assert-TokenCount $runtimeTestsCMake 'binkstub' 1 'Zero Hour runtime regression test link ownership is ambiguous.'

$audioContractFiles = @(
    'Core/GameEngine/Include/Common/GameAudio.h',
    'Core/GameEngineDevice/Include/MilesAudioDevice/MilesAudioManager.h',
    'Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp',
    'Core/GameEngineDevice/Include/VideoDevice/Bink/BinkVideoPlayer.h',
    'Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp',
    'Core/GameEngineDevice/Include/VideoDevice/FFmpeg/FFmpegVideoPlayer.h',
    'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp'
)

foreach ($relativePath in $audioContractFiles) {
    $content = Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw
    if ($content -match 'getHandleForBink|releaseHandleForBink|initializeBinkWithMiles') {
        throw "Legacy Bink-specific audio contract remains in $relativePath"
    }
}

$gameAudio = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngine/Include/Common/GameAudio.h') -Raw
if ($gameAudio -notmatch 'class LegacyVideoAudioInterface') {
    throw 'AudioManager is missing the explicit legacy video audio capability interface.'
}
if ($gameAudio -match 'getHandleForVideo') {
    throw 'AudioManager retains a raw video audio acquisition contract.'
}
if ($gameAudio -match 'releaseHandleForVideo') {
    throw 'AudioManager retains a raw video audio release contract.'
}

$milesAudio = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/MilesAudioDevice/MilesAudioManager.h') -Raw
if ($milesAudio -notmatch 'virtual void \*getLegacyVideoDirectSoundHandle\(\)' -or $milesAudio -notmatch 'virtual void releaseLegacyVideoAudioHandle\(\)') {
    throw 'Miles fallback does not own the legacy video audio handle contract.'
}
if ($milesAudio -notmatch 'void\* getLegacyVideoDirectSoundHandle\(\) override' -or $milesAudio -notmatch 'void releaseLegacyVideoAudioHandle\(\) override') {
    throw 'Miles headless audio manager does not override the legacy video audio contract.'
}

$binkPlayer = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp') -Raw
if ($binkPlayer -notmatch 'dynamic_cast<LegacyVideoAudioInterface \*>') {
    throw 'Bink player does not use the checked legacy video audio capability.'
}

$ffmpegPlayer = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp') -Raw
if ($ffmpegPlayer -match 'RTS_HAS_OPENAL|RTS_USE_OPENAL|OpenALAudioStream|getHandleForVideo|releaseHandleForVideo') {
    throw 'FFmpeg player retains an unowned legacy or OpenAL audio path.'
}

Write-Output 'Video backend selection audit passed.'
