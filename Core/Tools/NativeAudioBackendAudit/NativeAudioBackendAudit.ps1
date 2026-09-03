param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

$modulePath = Join-Path $SourceRoot 'cmake/xaudio2.cmake'
if (-not (Test-Path -LiteralPath $modulePath)) {
    throw "Native XAudio2 CMake module is missing: $modulePath"
}

$module = Get-Content -LiteralPath $modulePath -Raw
if ($module -notmatch 'add_library\(rts_xaudio2 INTERFACE\)') {
    throw 'Native XAudio2 module does not declare the rts_xaudio2 boundary target.'
}
if ($module -notmatch 'CMAKE_SIZEOF_VOID_P EQUAL 8') {
    throw 'Native XAudio2 module is not limited to the x64 product graph.'
}
if ($module -notmatch 'xaudio2\.lib') {
    throw 'Native XAudio2 module does not resolve the SDK import library.'
}
if ($module -notmatch '_WIN32_WINNT=0x0A00' -or $module -notmatch 'WINVER=0x0A00' -or
    $module -notmatch 'NTDDI_VERSION=NTDDI_WIN10_19H1') {
    throw 'Native XAudio2 module does not declare the Windows 10 API contract.'
}

$runtimeModule = Get-Content -LiteralPath (Join-Path $SourceRoot 'cmake/native-product-runtime.cmake') -Raw
if ($runtimeModule -notmatch 'rts_xaudio2') {
    throw 'The native product runtime boundary does not retain the XAudio2 contract.'
}

Write-Output 'Native audio backend audit passed.'
