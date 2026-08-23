param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

$headers = @(
    'Generals/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h',
    'GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameClient.h'
)

foreach ($relativePath in $headers) {
    $content = Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw
    if ($content -notmatch '#ifdef RTS_HAS_FFMPEG[\s\S]*FFmpegVideoPlayer[\s\S]*#else[\s\S]*BinkVideoPlayer[\s\S]*#endif') {
        throw "FFmpeg/Bink video backend selection contract is missing in $relativePath"
    }
}

Write-Output 'Video backend selection audit passed.'
