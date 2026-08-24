param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

$header = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/Win32Device/Common/Win32GameEngine.h') -Raw
if ($header -match 'MilesAudioDevice/MilesAudioManager\.h') {
    throw 'Win32GameEngine public header still exposes the Miles audio implementation.'
}
if ($header -match 'inline AudioManager \*Win32GameEngine::createAudioManager') {
    throw 'Win32GameEngine audio factory is still coupled to its header implementation.'
}

$implementation = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp') -Raw
if ($implementation -notmatch '#include "AudioDevice/AudioManagerFactory\.h"') {
    throw 'Win32 factory implementation does not delegate to the neutral audio factory.'
}
if ($implementation -notmatch 'AudioManager \*Win32GameEngine::createAudioManager\(Bool dummy\)') {
    throw 'Win32 audio factory implementation is missing.'
}
if ($implementation -match '(?i)miles|mss|MilesAudioManager|MilesAudioDevice') {
    throw 'Common Win32 factory implementation still names a legacy audio backend.'
}

$deviceCmake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/CMakeLists.txt') -Raw
if ($deviceCmake -notmatch 'Source/MilesAudioDevice/AudioManagerFactory\.cpp' -or
    $deviceCmake -notmatch 'Source/XAudio2AudioDevice/AudioManagerFactory\.cpp') {
    throw 'Architecture-specific audio factory implementations are incomplete.'
}

$managerHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Include/XAudio2AudioDevice/XAudio2AudioManager.h') -Raw
$managerSource = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/XAudio2AudioDevice/XAudio2AudioManager.cpp') -Raw
if ($managerHeader -notmatch 'std::unique_ptr<AudioAssetSource>\s+m_ownedAssetSource') {
    throw 'The native manager has no owned neutral production asset source for the default factory path.'
}
if ($managerSource -notmatch 'm_ownedAssetSource\s*=\s*std::make_unique<FileAudioAssetSource>') {
    throw 'The native manager default path does not construct the filesystem/container asset source.'
}
if ($managerSource -notmatch 'm_assetSource\s*=\s*m_ownedAssetSource\.get\(\)') {
    throw 'The native manager default path does not wire the owned asset source into playback.'
}
$assetImplementation = Join-Path $SourceRoot 'Core/GameEngineDevice/Source/AudioDevice/AudioAssetSource.cpp'
if (-not (Test-Path -LiteralPath $assetImplementation)) {
    throw 'The production filesystem/container asset source implementation is missing.'
}

Write-Output 'Native audio factory audit passed.'
