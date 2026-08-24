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

$windowVideoPath = Join-Path $SourceRoot 'Core/GameEngine/Source/GameClient/GUI/WindowVideoManager.cpp'
$windowVideo = Get-Content -LiteralPath $windowVideoPath -Raw
$updateBlock = [regex]::Match($windowVideo,
    '(?s)void WindowVideoManager::update\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\nvoid WindowVideoManager::handleFinishedMovie').Groups[1].Value
$terminalBlock = [regex]::Match($windowVideo,
    '(?s)void WindowVideoManager::handleFinishedMovie\(.*?\)\s*\{(.*?)\r?\n\}\r?\n\r?\nvoid WindowVideoManager::playMovie').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($updateBlock) -or [string]::IsNullOrWhiteSpace($terminalBlock)) {
    throw 'Window video playback is missing the anchored terminal-policy helper.'
}
if (([regex]::Matches($updateBlock, 'handleFinishedMovie\(winVid, win, videoStream\);').Count -ne 3) -or
    ($updateBlock -match 'isFinished\(\)\s*\)\s*\{\s*stopMovie\(win\)')) {
    throw 'Window video update does not route every terminal path through the shared policy.'
}
if ($updateBlock -notmatch 'frameIndex\(\)\s*==\s*0[\s\S]*?WINDOW_PLAY_MOVIE_LOOP[\s\S]*?isFinished\(\)[\s\S]*?handleFinishedMovie\(winVid, win, videoStream\)') {
    throw 'Window video update does not restart a terminal one-frame loop.'
}
$requiredTerminalPolicy = @(
    'WINDOW_PLAY_MOVIE_ONCE[\s\S]*?stopMovie\(win\)',
    'WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME[\s\S]*?pauseMovie\(win\)',
    'WINDOW_PLAY_MOVIE_LOOP[\s\S]*?if\s*\(\s*!videoStream->frameGoto\(0\)\s*\)[\s\S]*?stopMovie\(win\)'
)
foreach ($pattern in $requiredTerminalPolicy) {
    if ($terminalBlock -notmatch $pattern) {
        throw 'Window video terminal policy does not preserve once, show-last-frame, and loop behavior.'
    }
}

Write-Output 'FFmpeg movie playback device-free audit passed.'
