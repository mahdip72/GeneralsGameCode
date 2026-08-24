param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot
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

$closurePath = Join-Path $BuildRoot 'native_audio_link_closure.txt'
if (-not (Test-Path -LiteralPath $closurePath)) {
    throw 'The configured native-device link-closure artifact is missing.'
}
$closure = Get-Content -LiteralPath $closurePath -Raw
$cache = Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt') -Raw
$pointerSizeConfigured = $cache -match 'CMAKE_SIZEOF_VOID_P(?::[^=]+)?=8'
if (-not $pointerSizeConfigured) {
    $compilerFiles = Get-ChildItem -LiteralPath (Join-Path $BuildRoot 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -Recurse -File -ErrorAction SilentlyContinue
    foreach ($compilerFile in $compilerFiles) {
        if ((Get-Content -LiteralPath $compilerFile.FullName -Raw) -match 'CMAKE_CXX_SIZEOF_DATA_PTR\s+"8"') {
            $pointerSizeConfigured = $true
            break
        }
    }
}
if (-not $pointerSizeConfigured) {
    throw 'The native x64 graph audit was not run against an 8-byte configured target graph.'
}
if ($closure -notmatch 'XAudio2AudioDevice[/\\]AudioManagerFactory\.cpp') {
    throw 'The configured x64 device closure does not contain the native factory source.'
}
if ($closure -notmatch 'links=.*rts_native_audio_asset_source' -or
    $closure -notmatch 'links=.*rts_xaudio2_pcm_voice' -or
    $closure -notmatch 'links=.*rts_xaudio2_audio_service') {
    throw 'The configured x64 target closure does not retain the native asset, voice, and service targets.'
}
if ($closure -match '(?i)mss|MilesAudioDevice|MilesAudioManager|milesstub') {
    throw 'The configured x64 native-device closure contains a legacy Miles source or link.'
}

Write-Output 'Native x64 audio graph audit passed.'
