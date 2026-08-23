param(
    [Parameter(Mandatory = $true)]
    [string] $SourceRoot
)

$ErrorActionPreference = 'Stop'

function Read-Source([string] $RelativePath) {
    $path = Join-Path $SourceRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        throw "missing source file: $RelativePath"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Require([string] $Text, [string] $Pattern, [string] $Message) {
    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

$gameAudio = Read-Source 'Core/GameEngine/Source/Common/Audio/GameAudio.cpp'
$nullHeader = Read-Source 'Core/GameEngineDevice/Include/AudioDevice/NullAudioManager.h'
$nullSource = Read-Source 'Core/GameEngineDevice/Source/AudioDevice/NullAudioManager.cpp'
$xaudioSource = Read-Source 'Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2AudioManager.cpp'
$assetSource = Read-Source 'Core/GameEngineDevice/Include/AudioDevice/AudioAssetSource.h'

$iniPaths = @(
    'Data\\INI\\AudioSettings',
    'Data\\INI\\Default\\Music',
    'Data\\INI\\Music',
    'Data\\INI\\Default\\SoundEffects',
    'Data\\INI\\SoundEffects',
    'Data\\INI\\Default\\Speech',
    'Data\\INI\\Speech',
    'Data\\INI\\Default\\Voice',
    'Data\\INI\\Voice',
    'Data\\INI\\MiscAudio'
)
$positions = @()
foreach ($path in $iniPaths) {
    $position = $gameAudio.IndexOf($path)
    if ($position -lt 0) { throw "missing base audio INI path: $path" }
    $positions += $position
}
for ($i = 1; $i -lt $positions.Count; ++$i) {
    if ($positions[$i] -le $positions[$i - 1]) { throw 'base audio INI order is not stable' }
}

Require $gameAudio 'm_allAudioEventInfo\[newEvent->m_audioName\]\s*=\s*newEvent' 'event pointer ownership changed'
Require $gameAudio 'getFileLengthMS\(tmpEvent\.getAttackFilename\(\)\)' 'attack duration lookup changed'
Require $gameAudio 'getFileLengthMS\(tmpEvent\.getFilename\(\)\)' 'main duration lookup changed'
Require $gameAudio 'getFileLengthMS\(tmpEvent\.getDecayFilename\(\)\)' 'decay duration lookup changed'
Require $nullHeader 'class NullAudioManager final : public AudioManager' 'Null manager is not an independent AudioManager'
Require $nullSource '#include "AudioDevice/AudioAssetSource\.h"' 'Null manager does not use the neutral asset source'
Require $nullSource 'm_assetSource\s*!=\s*nullptr\s*&&\s*m_assetSource->getDurationMS' 'Null duration lookup is not injected'
Require $nullSource 'removeAllAudioRequests\(\)' 'Null reset does not clear requests'
Require $nullSource 'removeLevelSpecificAudioEventInfos\(\)' 'Null reset does not clear level metadata'
Require $nullSource '\+\+m_lifecycleGeneration' 'Null reset does not advance generation'
Require $nullSource 'TheTacticalView\s*!=\s*nullptr' 'Null headless update guard missing'
Require $xaudioSource 'TheTacticalView\s*!=\s*nullptr' 'XAudio headless update guard missing'
Require $assetSource 'getEventDurationMS' 'attack/main/decay duration helper missing'

Write-Output 'native audio lifecycle audit passed'
