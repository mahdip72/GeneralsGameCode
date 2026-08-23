param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$serviceHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2AudioService.h') -Raw
$serviceSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2AudioService.cpp') -Raw
$nativeHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2NativeAudioEngine.h') -Raw
$nativeSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2NativeAudioEngine.cpp') -Raw
$deviceCMake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/CMakeLists.txt') -Raw

foreach ($text in @($serviceHeader, $serviceSource, $nativeHeader)) {
    if ($text -match 'IXAudio2\s*\*|IXAudio2MasteringVoice\s*\*|IXAudio2SourceVoice\s*\*') {
        throw 'Raw XAudio2 engine, mastering, or source-voice pointers escaped the native implementation boundary.'
    }
}

foreach ($required in @(
        'IXAudio2AudioEngineBackend.h',
        'XAudio2NativeAudioEngine.h',
        'rts_xaudio2_audio_service',
        'StartEngine',
        'StopEngine',
        'UnregisterForCallbacks',
        'disableAndWait',
        'm_inFlight')) {
    if ($nativeSource -notmatch [regex]::Escape($required) -and $deviceCMake -notmatch [regex]::Escape($required)) {
        throw "Native audio lifecycle boundary is missing required token: $required"
    }
}

if ($serviceHeader -match '#include\s*<xaudio2\.h>' -or $serviceSource -match '#include\s*<xaudio2\.h>') {
    throw 'The service directly includes the native XAudio2 header.'
}

Write-Output 'XAudio2 audio service boundary audit passed'
