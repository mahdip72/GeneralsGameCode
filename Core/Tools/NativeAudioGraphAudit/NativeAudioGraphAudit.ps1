param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

$common = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp') -Raw
if ($common -match '(?i)miles|mss|MilesAudioManager|MilesAudioDevice') {
    throw 'Common Win32 game-engine source still names a legacy audio backend.'
}

$cmake = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/GameEngineDevice/CMakeLists.txt') -Raw
if ($cmake -notmatch 'Source/XAudio2AudioDevice/AudioManagerFactory\.cpp') {
    throw 'The x64 device graph does not select the native audio factory implementation.'
}
if ($cmake -notmatch 'Source/AudioDevice/AudioChannelPolicy\.cpp') {
    throw 'The neutral audio channel policy is not in the device graph.'
}

$tools = Get-Content -LiteralPath (Join-Path $SourceRoot 'Core/Tools/CMakeLists.txt') -Raw
$x64Extras = [regex]::Match($tools, '(?s)if\(CMAKE_SIZEOF_VOID_P EQUAL 8.*?add_subdirectory\(JobSystemTest\)').Value
if ($x64Extras -match 'MilesAudioCompletionTest') {
    throw 'A Miles-only tool remains reachable from the x64 extras graph.'
}
if ($tools -notmatch '(?s)if\(CMAKE_SIZEOF_VOID_P EQUAL 4\).*?add_subdirectory\(MilesAudioCompletionTest\)') {
    throw 'The Miles-only completion tool is not explicitly retained in the x86 lane.'
}

Write-Output 'Native x64 audio graph audit passed.'
