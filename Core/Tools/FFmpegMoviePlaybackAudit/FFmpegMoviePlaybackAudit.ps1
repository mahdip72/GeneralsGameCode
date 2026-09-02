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
if ($source -notmatch 'DRAIN_NO_PROGRESS_TIMEOUT_US' -or
    $source -notmatch '(?s)getPlayedSample\(.*?drainWatchdogStartUs' -or
    $source -notmatch '(?s)drainWatchdogExpired\(\).*?setFailed\(') {
    throw 'FFmpeg playback core has no bounded monotonic no-progress drain watchdog.'
}
if ($source -notmatch '(?s)!m_audioSink->canAccept\(2\).*?serviceSink\(\).*?beginDrainWatchdog\(\).*?drainWatchdogExpired\(\).*?setFailed\(') {
    throw 'FFmpeg playback core has no bounded monotonic watchdog for active audio backpressure.'
}
if ($source -notmatch '(?s)!m_audioSink->canAccept\(2\).*?serviceSink\(\).*?if\s*\(m_audioSink->canAccept\(2\)\).*?progressed\s*=\s*true;.*?continue;') {
    throw 'FFmpeg playback does not report a successful reset/service admission transition as bounded progress.'
}
$fileHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/VideoDevice/FFmpeg/FFmpegFile.h') -Raw
$fileSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp') -Raw
$decodeStepBlock = [regex]::Match($fileSource,
    '(?s)FFmpegDecodeStepResult FFmpegFile::decodeStep\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\nBool FFmpegFile::decodePacket').Groups[1].Value
if ($fileHeader -notmatch 'FFmpegDecodeStepResult decodeStep\(\)' -or
    [string]::IsNullOrWhiteSpace($decodeStepBlock) -or $decodeStepBlock -match '\b(?:for|while)\s*\(') {
    throw 'FFmpeg decodeStep is missing or can process an unbounded number of state transitions.'
}
if ($source -notmatch 'm_file\.decodeStep\(\)' -or $source -match 'm_file\.decodePacket\(\)') {
    throw 'FFmpeg movie playback does not use the bounded decoder-step contract.'
}
$flushBlock = [regex]::Match($source,
    '(?s)bool flush\(\)\s*\{(.*?)\n\t\}\s*\n\s*void reset').Groups[1].Value
if ($source -notmatch '(?s)canAccept\(2\).*?serviceSink\(\).*?return true;.*?m_file\.decodeStep\(\)' -or
    $flushBlock -notmatch '(?s)m_sink\.submit\(std::move\(m_pending\)\).*?result != AudioPcmSubmitResult::ACCEPTED\)\s*\{\s*return false;' -or
    $source -notmatch '(?s)if \(!m_gainSink->flush\(\)\)\s*\{\s*setFailed\(\)' -or
    $source -notmatch '(?s)if \(!m_audioDecoder->drain\(\*m_audioSink\) \|\| !m_gainSink->flush\(\)\)\s*\{\s*return setFailed\(\)') {
    throw 'FFmpeg movie playback can consume input without two-slot admission or continue after a defensive PCM drop.'
}

$pcmTypes = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/AudioDevice/AudioPcmTypes.h') -Raw
$voiceHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2PcmVoice.h') -Raw
$serviceHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2AudioService.h') -Raw
$movieSinkSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2MoviePcmSink.cpp') -Raw
if ($pcmTypes -notmatch 'canAccept\(std::size_t submissions\)' -or
    $pcmTypes -notmatch 'setOutputGain\(double gain\)' -or
    $voiceHeader -notmatch 'canAccept\(std::size_t submissions\).*?override' -or
    $serviceHeader -notmatch 'canVoiceAccept\(' -or
    $movieSinkSource -notmatch 'canVoiceAccept\(m_handle, submissions\)' -or
    $movieSinkSource -notmatch 'setVoiceVolume\(m_handle') {
    throw 'The native movie sink does not expose bounded admission and queued-output gain.'
}

$testCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/Tools/FFmpegMoviePlaybackTest/CMakeLists.txt') -Raw
if ($testCMake -match 'Source/VideoDevice/FFmpeg/(FFmpegFile|FFmpegMoviePlayback)\.cpp') {
    throw 'Integrated FFmpeg movie playback test compiles a shadow production source.'
}
if ($testCMake -notmatch '(?ms)target_link_libraries\(core_ffmpeg_movie_playback_test PRIVATE\s+rts_ffmpeg_movie_playback\s*\)') {
    throw 'Integrated FFmpeg movie playback test does not link the production playback target.'
}

$streamPath = Join-Path $SourceRoot 'Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp'
$streamSource = Get-Content -LiteralPath $streamPath -Raw
$destructorBlock = [regex]::Match($streamSource,
    '(?s)FFmpegVideoStream::~FFmpegVideoStream\(\)\s*\{(.*?)\r?\n\}').Groups[1].Value
$updateBlock = [regex]::Match($streamSource,
    '(?s)void FFmpegVideoStream::update\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\nBool FFmpegVideoStream::isFinished').Groups[1].Value
$frameNextBlock = [regex]::Match($streamSource,
    '(?s)void FFmpegVideoStream::frameNext\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\nBool FFmpegVideoStream::finishPlayback').Groups[1].Value
$finishBlock = [regex]::Match($streamSource,
    '(?s)Bool FFmpegVideoStream::finishPlayback\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\n//={20,}\r?\n// FFmpegVideoStream::frameIndex').Groups[1].Value
$frameGotoBlock = [regex]::Match($streamSource,
    '(?s)Bool FFmpegVideoStream::frameGoto\(\s*Int index\s*\)\s*\{(.*?)\r?\n\}\r?\n\r?\n//={20,}\r?\n// VideoStream::height').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($destructorBlock) -or [string]::IsNullOrWhiteSpace($updateBlock) -or
    [string]::IsNullOrWhiteSpace($frameNextBlock) -or [string]::IsNullOrWhiteSpace($finishBlock)) {
    throw 'FFmpeg stream lifecycle methods are missing from the anchored audit.'
}
if ($destructorBlock -match 'finish\s*\(') {
    throw 'Closing an FFmpeg stream still drains the remaining movie synchronously.'
}
if ($updateBlock -notmatch '(?s)serviceSink\(\).*?if\s*\(m_gotFrame\)\s*\{\s*return;') {
    throw 'Direct FFmpeg stream updates do not service movie audio before retaining a display-owned frame.'
}
if ($updateBlock -notmatch '(?s)FFmpegMoviePlaybackState::DRAINING.*?pump\(1\).*?if\s*\(m_gotFrame\)') {
    throw 'A retained FFmpeg final frame can prevent callback-driven audio drain from reaching terminal state.'
}
if ($frameNextBlock -notmatch '(?s)attempts\s*<\s*32.*?pump\(1\).*?serviceSink\(\).*?m_gotFrame\s*=\s*true.*?break;') {
    throw 'FFmpeg frame advancement does not decode and publish exactly one serviced frame.'
}
if ($frameNextBlock -match 'isFrameReady\(\)' -or $frameNextBlock -match 'pump\((?:[2-9]|[1-9][0-9]+)\)') {
    throw 'FFmpeg frame advancement can still discard multiple overdue frames without a continuous audio cursor.'
}
if ($finishBlock -notmatch '(?s)serviceSink\(\).*?pump\(1\).*?isTerminal\(\)\s*\?\s*TRUE\s*:\s*FALSE') {
    throw 'Normal FFmpeg completion lacks a nonblocking owner-side tail-audio drain step.'
}
if ($streamSource -notmatch '(?s)Bool FFmpegVideoStream::isPlaybackFailed\(\).*?m_playback->state\(\) == FFmpegMoviePlaybackState::FAILED' -or
    $streamSource -notmatch '(?s)markPlaybackFailed\(\).*?failPlayback\(\).*?m_audioSink->close\(\)') {
    throw 'FFmpeg stream failure status does not remain explicit through safe audio teardown.'
}
if ([string]::IsNullOrWhiteSpace($frameGotoBlock) -or $frameGotoBlock -notmatch 'serviceSink\(\)') {
    throw 'FFmpeg seek advancement does not service its bounded movie-audio sink.'
}
if ($streamSource -notmatch '(?s)getMovieSpeechGain\(\).*?isOn\(AudioAffect_Speech\).*?getVolume\(AudioAffect_Speech\).*?0\.8' -or
    $streamSource -notmatch 'options\.gain = initialGain' -or
    ([regex]::Matches($streamSource, 'syncSpeechGain\(\);').Count -lt 4)) {
    throw 'FFmpeg movie streams do not apply initial and live legacy speech gain before every pump owner.'
}

$videoInterfacePath = Join-Path $SourceRoot 'Core/GameEngine/Include/GameClient/VideoPlayer.h'
$videoInterface = Get-Content -LiteralPath $videoInterfacePath -Raw
if ($videoInterface -notmatch 'virtual Bool\s+isPlaybackFailed\(\) const \{\s*return FALSE;\s*\}') {
    throw 'VideoStreamInterface lacks the VC6-compatible legacy default for explicit playback failure.'
}

$playbackTestPath = Join-Path $SourceRoot 'Core/Tools/FFmpegMoviePlaybackTest/FFmpegMoviePlaybackTest.cpp'
$playbackTest = Get-Content -LiteralPath $playbackTestPath -Raw
if ($playbackTest -notmatch '(?s)testNoCallbackDrainFailsBoundedly.*?holdDrain\s*=\s*true.*?state\(\) == FFmpegMoviePlaybackState::FAILED') {
    throw 'FFmpeg movie playback tests lack fake no-callback failure-status coverage.'
}
if ($playbackTest -notmatch '(?s)testNoCallbackBackpressureFailsBoundedly.*?availableSubmissions\s*=\s*0.*?state\(\) == FFmpegMoviePlaybackState::FAILED') {
    throw 'FFmpeg movie playback tests lack fake no-callback active-backpressure coverage.'
}
if ($playbackTest -notmatch '(?s)testResetPendingServiceReopensAdmission.*?serviceReopensAdmission\s*=\s*true.*?resetPending.*?pump\(1\).*?serviceCalls\s*!=\s*1.*?submitCalls\s*!=\s*0' -or
    $playbackTest -notmatch '(?s)reset-pending service reopens admission.*?testResetPendingServiceReopensAdmission') {
    throw 'FFmpeg movie playback tests lack reset-pending admission recovery coverage.'
}

$videoBufferPath = Join-Path $SourceRoot 'Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DVideoBuffer.cpp'
$videoBufferSource = Get-Content -LiteralPath $videoBufferPath -Raw
$selectionBlock = [regex]::Match($videoBufferSource,
    '(?s)Bool W3DVideoBuffer::UsesNativeD3D11PublicationPath\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\n//={20,}\r?\n// W3DVideoBuffer::SetBuffer').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($selectionBlock) -or
    $selectionBlock -notmatch '(?s)#if\s+defined\(_WIN64\).*?RequestedRenderBackend\(\).*?IsRenderBackendSupported\(backend\).*?IsNativeD3D11PublicationActive\(\).*?#else.*?return\s+FALSE;') {
    throw 'W3D video native publication selection is not x64-only and active-owner bound.'
}
$allocateBlock = [regex]::Match($videoBufferSource,
    '(?s)Bool W3DVideoBuffer::allocate\(\s*UnsignedInt width, UnsignedInt height\s*\)\s*\{(.*?)\r?\n\}\r?\n\r?\n//={20,}\r?\n// W3DVideoBuffer::~W3DVideoBuffer').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($allocateBlock) -or
    $allocateBlock -notmatch '(?s)m_nativePublicationPath\s*=\s*UsesNativeD3D11PublicationPath\(\).*?if\s*\(\s*m_nativePublicationPath\s*\).*?max_texture_dimension.*?max_texture_bytes.*?else\s*\{.*?Validate_Texture_Size') {
    throw 'D3D11 movie textures do not use the active native selection with a legacy fallback.'
}
$unlockBlock = [regex]::Match($videoBufferSource,
    '(?s)void\s+W3DVideoBuffer::unlock\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\n//={20,}\r?\n// W3DVideoBuffer::valid').Groups[1].Value
if ($videoBufferSource -match 'DX8Wrapper::Is_D3D11_Backend_Active|Publish_Render_Texture_BGRA8_Change|PublishTextureBGRA8Change|publishLockedFrame|m_framePublished|frame_published') {
    throw 'The W3D video buffer still contains a retired direct or duplicate publication path.'
}
if ([string]::IsNullOrWhiteSpace($unlockBlock) -or
    $unlockBlock -notmatch '(?s)m_surface->Unlock\(\).*?m_nativePublicationPath.*?NotifyTextureChanged') {
    throw 'The W3D video buffer no longer preserves the legacy fallback notification after unlock.'
}

$surfacePath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/surfaceclass.cpp'
$surfaceSource = Get-Content -LiteralPath $surfacePath -Raw
$surfaceUnlockBlock = [regex]::Match($surfaceSource,
    '(?s)void SurfaceClass::Unlock\(\)\s*\{(.*?)\r?\n\}\r?\n\r?\n#if\s+defined\(_WIN64\)').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($surfaceUnlockBlock) -or
    $surfaceUnlockBlock -notmatch '(?s)#if\s+defined\(_WIN64\).*?NativeSurface->locked\s*=\s*false;.*?Publish_Native_Surface\(NativeSurface\)') {
    throw 'SurfaceClass::Unlock is not the sole native video publication boundary.'
}
if (([regex]::Matches($surfaceUnlockBlock,
    '\bPublish_Native_Surface\(\s*NativeSurface\b')).Count -ne 1) {
    throw 'SurfaceClass::Unlock has more than one native video publication call.'
}

$frameRenderBlock = [regex]::Match($streamSource,
    '(?s)void FFmpegVideoStream::frameRender\(\s*VideoBuffer \*buffer\s*\)\s*\{(.*?)\r?\n\}\r?\n\r?\n//={20,}\r?\n// FFmpegVideoStream::frameNext').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($frameRenderBlock) -or
    $frameRenderBlock -notmatch '(?s)TYPE_X8R8G8B8.*?AV_PIX_FMT_BGRA.*?sws_scale.*?buffer->unlock\(\)') {
    throw 'FFmpeg does not scale opaque BGRA8 pixels before the sole SurfaceClass unlock publication.'
}
if ($frameRenderBlock -match 'Publish_Render_Texture_BGRA8_Change|PublishTextureBGRA8Change') {
    throw 'FFmpeg frame rendering reintroduced a second direct native texture upload.'
}
if ($frameRenderBlock -match 'AV_PIX_FMT_BGR0') {
    throw 'The video conversion path can expose a zero alpha channel to DRAW_IMAGE_ALPHA.'
}

$bridgePath = Join-Path $SourceRoot 'Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp'
$bridgeSource = Get-Content -LiteralPath $bridgePath -Raw
$bridgePublishBlock = [regex]::Match($bridgeSource,
    '(?s)bool D3D11LegacyBridge::Publish_Texture_BGRA8_Change\(.*?\)\s*\{(.*?)\r?\n\}\r?\n\r?\nvoid D3D11LegacyBridge::Invalidate_Texture').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($bridgePublishBlock) -or
    $bridgePublishBlock -notmatch 'device->createTexture' -or
    $bridgePublishBlock -notmatch 'device->refreshTexture' -or
    $bridgePublishBlock -match '!m_impl->frame_open') {
    throw 'The D3D11 bridge lacks the pre-frame create/refresh path for locked BGRA8 video pixels.'
}

$loadScreenPath = Join-Path $SourceRoot 'Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp'
$loadScreenSource = Get-Content -LiteralPath $loadScreenPath -Raw
$singleInit = [regex]::Match($loadScreenSource,
    '(?s)void SinglePlayerLoadScreen::init\(.*?\)\s*\{(.*?)\r?\n\}\r?\n\r?\nvoid ChallengeLoadScreen::').Groups[1].Value
$challengeInit = [regex]::Match($loadScreenSource,
    '(?s)void ChallengeLoadScreen::init\(.*?\)\s*\{(.*?)\r?\n\}\r?\n\r?\nvoid ShellGameLoadScreen::').Groups[1].Value
foreach ($entry in @(@('single-player', $singleInit), @('challenge', $challengeInit))) {
    $name = $entry[0]
    $block = $entry[1]
    if ([string]::IsNullOrWhiteSpace($block)) {
        throw "The $name loading-screen initializer is missing from the anchored audit."
    }
    if ([regex]::Matches($block, 'if \(progressUpdateCount < 1\)').Count -ne 1) {
        throw "The $name loading movie does not have exactly one short-movie progress-divisor guard."
    }
    if ([regex]::Matches($block, 'm_videoStream->finishPlayback\(\)').Count -ne 1 -or
        $block -notmatch '(?s)(?:!movieAborted.*?finalFrameReached|!movieAborted.*?frameIndex\(\).*?frameCount\(\)\s*-\s*1).*?MAX_FINISH_PLAYBACK_ATTEMPTS\s*=\s*10000.*?for\s*\(.*?finishPlayback\(\).*?isMovieAbortRequested') {
        throw "The $name loading movie does not present and boundedly drain natural completion."
    }
    if ([regex]::Matches($block, 'MAX_FINAL_FRAME_READY_ATTEMPTS\s*=\s*10000').Count -ne 1 -or
        $block -notmatch '(?s)Bool finalFrameReady = FALSE;.*?finalFrameReady = m_videoStream->isFrameReady\(\).*?MAX_FINAL_FRAME_READY_ATTEMPTS.*?m_videoStream->update\(\).*?finalFrameReady = m_videoStream->isFrameReady\(\).*?!movieAborted.*?finalFrameReady.*?m_videoStream->frameDecompress\(\)') {
        throw "The $name loading movie can decode a final frame before its readiness gate."
    }
    if ($block -notmatch '(?s)!finalFrameReady\s*&&\s*m_videoStream->isFinished\(\).*?continue;') {
        throw "The $name loading movie treats a terminal-index Bink stream as ready without another bounded readiness poll."
    }
    if ($block -notmatch '(?s)const Bool hasKnownFrameCount = movieFrameCount > 0;.*?while\s*\(\(!hasKnownFrameCount\s*\|\|\s*m_videoStream->frameIndex\(\)\s*<\s*movieFrameCount\s*-\s*1\).*?const Bool finalFrameReached = hasKnownFrameCount.*?:\s*m_videoStream->isFinished\(\);') {
        throw "The $name loading movie does not decode unknown-frame-count streams until they finish."
    }
    if ($block -notmatch '(?s)while\s*\(\(!hasKnownFrameCount.*?!m_videoStream->isPlaybackFailed\(\)\)' -or
        $block -notmatch '(?s)m_videoStream->update\(\);\s*if\s*\(m_videoStream->isPlaybackFailed\(\)\)' -or
        $block -notmatch '(?s)!movieAborted\s*&&\s*!m_videoStream->isPlaybackFailed\(\)\s*&&\s*finalFrameReached') {
        throw "The $name loading movie does not exit promptly on explicit playback failure."
    }
    if ($block -match '(?s)!movieAborted\s*&&\s*!m_videoStream->isFinished\(\).*?frameIndex\(\)') {
        throw "The $name retained-final-frame fallback is unreachable for a terminal legacy backend."
    }
    if ($block -notmatch '(?s)m_videoStream->close\(\);\s*m_videoStream\s*=\s*nullptr;') {
        throw "The $name loading movie has no prompt close fallback."
    }
}
if ($challengeInit -notmatch '(?s)TheVideoPlayer->open.*?if \(m_videoStream == nullptr\).*?return;.*?m_videoStream->width\(\)') {
    throw 'Challenge loading movie open failure can reach a null stream dereference.'
}

foreach ($scorePath in @(
    'Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/ScoreScreen.cpp',
    'GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/ScoreScreen.cpp')) {
    $scoreSource = Get-Content -LiteralPath (Join-Path $SourceRoot $scorePath) -Raw
    if ($scoreSource -notmatch '(?s)Bool movieAborted = FALSE;.*?!movieAborted.*?MAX_FINISH_PLAYBACK_ATTEMPTS\s*=\s*10000.*?for\s*\(.*?finishPlayback\(\).*?KEY_ESC.*?videoStream->close\(\)') {
        throw "Blocking score movie does not boundedly drain with abort and close fallback: $scorePath"
    }
    if ([regex]::Matches($scoreSource, 'MAX_FINAL_FRAME_READY_ATTEMPTS\s*=\s*10000').Count -ne 1 -or
        $scoreSource -notmatch '(?s)Bool finalFrameReady = FALSE;.*?finalFrameReady = videoStream->isFrameReady\(\).*?MAX_FINAL_FRAME_READY_ATTEMPTS.*?videoStream->update\(\).*?finalFrameReady = videoStream->isFrameReady\(\).*?!movieAborted.*?finalFrameReady.*?videoStream->frameDecompress\(\)') {
        throw "Blocking score movie can decode a final frame before its readiness gate: $scorePath"
    }
    if ($scoreSource -notmatch '(?s)!finalFrameReady\s*&&\s*videoStream->isFinished\(\).*?continue;') {
        throw "Blocking score movie treats a terminal-index Bink stream as ready without another bounded readiness poll: $scorePath"
    }
    if ($scoreSource -match '(?s)!movieAborted\s*&&\s*!videoStream->isFinished\(\).*?frameIndex\(\)') {
        throw "Blocking score movie cannot present a terminal legacy final frame: $scorePath"
    }
    if ($scoreSource -notmatch '(?s)const Int movieFrameCount = videoStream->frameCount\(\);.*?const Bool hasKnownFrameCount = movieFrameCount > 0;.*?while\s*\(\(!hasKnownFrameCount\s*\|\|\s*videoStream->frameIndex\(\)\s*<\s*movieFrameCount\s*-\s*1\).*?const Bool finalFrameReached = hasKnownFrameCount.*?:\s*videoStream->isFinished\(\);') {
        throw "Blocking score movie does not decode unknown-frame-count streams until they finish: $scorePath"
    }
    if ($scoreSource -notmatch '(?s)while\s*\(\(!hasKnownFrameCount.*?!videoStream->isPlaybackFailed\(\)\)' -or
        $scoreSource -notmatch '(?s)videoStream->update\(\);\s*if\s*\(videoStream->isPlaybackFailed\(\)\)' -or
        $scoreSource -notmatch '(?s)!movieAborted\s*&&\s*!videoStream->isPlaybackFailed\(\)\s*&&\s*finalFrameReached') {
        throw "Blocking score movie does not exit promptly on explicit playback failure: $scorePath"
    }
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
