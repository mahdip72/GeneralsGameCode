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
if ($implementation -notmatch '#include "MilesAudioDevice/MilesAudioManager\.h"') {
    throw 'Win32 factory implementation does not retain the legacy Miles fallback include.'
}
if ($implementation -notmatch 'AudioManager \*Win32GameEngine::createAudioManager\(Bool dummy\)') {
    throw 'Win32 audio factory implementation is missing.'
}

Write-Output 'Native audio factory audit passed.'
