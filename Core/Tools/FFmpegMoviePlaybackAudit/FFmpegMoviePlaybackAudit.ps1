param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

$coreFiles = @(
    'Core/GameEngineDevice/Include/VideoDevice/FFmpeg/FFmpegMoviePlayback.h',
    'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegMoviePlayback.cpp'
)
$forbidden = @(
    'TheAudio',
    'TheGlobalData',
    'TheFileSystem',
    'XAudio2',
    'VideoBuffer',
    'DirectSound',
    'IDirectSound'
)

foreach ($relativePath in $coreFiles) {
    $path = Join-Path $SourceRoot $relativePath
    if (-not (Test-Path -LiteralPath $path)) {
        throw "FFmpeg playback core file is missing: $relativePath"
    }
    $content = Get-Content -LiteralPath $path -Raw
    foreach ($token in $forbidden) {
        if ($content -match [regex]::Escape($token)) {
            throw "Device-free FFmpeg playback core contains forbidden token '$token': $relativePath"
        }
    }
}

$header = Get-Content -LiteralPath (Join-Path $SourceRoot $coreFiles[0]) -Raw
$source = Get-Content -LiteralPath (Join-Path $SourceRoot $coreFiles[1]) -Raw
if (($header -notmatch 'AudioPcmSink') -or ($header -notmatch 'FFmpegAudioDecoder') -or ($source -notmatch 'av_frame_clone') -or ($source -notmatch 'm_audioDecoder->drain')) {
    throw 'FFmpeg playback core is missing the typed sink, decoder, callback clone, or EOF drain contract.'
}

$testCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/Tools/FFmpegMoviePlaybackTest/CMakeLists.txt') -Raw
if ($testCMake -match 'Source/VideoDevice/FFmpeg/(FFmpegFile|FFmpegMoviePlayback)\.cpp') {
    throw 'Integrated FFmpeg movie playback test compiles a shadow production source.'
}
if ($testCMake -notmatch '(?ms)target_link_libraries\(core_ffmpeg_movie_playback_test PRIVATE\s+rts_ffmpeg_movie_playback\s*\)') {
    throw 'Integrated FFmpeg movie playback test does not link the production playback target.'
}

Write-Output 'FFmpeg movie playback device-free audit passed.'
