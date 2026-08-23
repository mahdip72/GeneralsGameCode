param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRoot
)

$ErrorActionPreference = 'Stop'

function Read-Source([string] $RelativePath) {
    $path = Join-Path $SourceRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) { throw "missing source file: $RelativePath" }
    return Get-Content -LiteralPath $path -Raw
}

function Require([string] $Text, [string] $Pattern, [string] $Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

$managerHeader = Read-Source 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2AudioManager.h'
$managerSource = Read-Source 'Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2AudioManager.cpp'
$serviceHeader = Read-Source 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2AudioService.h'
$voiceHeader = Read-Source 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2PcmVoice.h'

Require $managerHeader 'class XAudio2AudioManager final : public AudioManager' 'XAudio manager is not a native AudioManager'
if ($managerHeader -match 'LegacyVideoAudioInterface|IXAudio2|IXAudio2SourceVoice') {
    throw 'XAudio manager exposes a legacy/raw native interface'
}
Require $managerHeader 'XAudio2AudioManager\(XAudio2AudioService \*service,\s*AudioAssetSource \*assetSource\)' 'injected service/asset constructor missing'
Require $managerSource 'AudioManager::init\(\)' 'base audio initialization missing'
Require $managerSource 'm_service->serviceVoices\(\)' 'owner update does not service voices first'
Require $managerSource 'drainCompletions\(\)' 'owner completion drain missing'
Require $managerSource 'processRequestList\(\)' 'owner request processing missing'
Require $managerSource 'm_service->shutdown\(\)' 'shutdown does not quiesce service callbacks'
Require $managerSource 'completion\.generation != m_lifecycleGeneration' 'stale completion rejection missing'
Require $managerSource 'removeLevelSpecificAudioEventInfos\(\)' 'reset metadata cleanup missing'
Require $managerSource 'prepareAudioEventForPlayback' 'native manager bypasses base admission semantics'
Require $managerSource 'resetVoice\(playing\.voice,\s*playing\.generation\)' 'new voices are not reset to manager generation'
Require $managerSource 'm_assetSource->decodePcmAt' 'native manager has no deterministic chunk continuation path'
Require $managerSource 'getFileIdentity' 'native manager does not track asset identity'
Require $managerSource 'isCurrentlyPlaying\(AudioHandle handle\)' 'generation-aware playback query missing'
Require $managerSource 'm_fadeAudioFrames' 'configured fade frame count is ignored'
Require $managerSource 'm_3DSoundRangeVolumeFadeExponent' 'configured 3D attenuation exponent is ignored'
Require $serviceHeader 'struct XAudio2AudioCompletion' 'bounded completion record missing'
Require $serviceHeader 'tryPopCompletion' 'owner completion observation API missing'
Require $serviceHeader 'setVoiceVolume|pauseVoice|resumeVoice|stopVoice' 'typed voice controls missing'
Require $voiceHeader 'COMPLETION_COUNT = 32' 'voice completion queue is not bounded'
Require $voiceHeader 'setVolume|pause\(\)|resume\(\)|stop\(\)' 'typed voice control boundary missing'

Write-Output 'native XAudio manager audit passed'
