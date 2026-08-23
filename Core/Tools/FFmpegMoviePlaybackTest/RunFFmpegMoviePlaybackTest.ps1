param(
    [Parameter(Mandatory = $true)]
    [string]$FFmpegExecutable,

    [Parameter(Mandatory = $true)]
    [string]$TestExecutable,

    [Parameter(Mandatory = $true)]
    [string]$AudioFixturePath,

    [Parameter(Mandatory = $true)]
    [string]$VideoFixturePath,

    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'

try {
    if (-not [string]::IsNullOrWhiteSpace($RuntimeDirectory)) {
        $env:PATH = $RuntimeDirectory + [IO.Path]::PathSeparator + $env:PATH
    }

    foreach ($fixture in @($AudioFixturePath, $VideoFixturePath)) {
        if (Test-Path -LiteralPath $fixture) {
            Remove-Item -LiteralPath $fixture -Force
        }
    }

    $commonVideoInput = @(
        '-hide_banner', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=30000/1001:duration=0.4'
    )
    $commonVideoOutput = @(
        '-frames:v', '12', '-c:v', 'mpeg4', '-bf', '2', '-g', '12',
        '-sc_threshold', '0', '-q:v', '2', '-pix_fmt', 'yuv420p', '-threads', '1'
    )
    $audioCommand = $commonVideoInput + @(
        '-f', 'lavfi', '-i', 'aevalsrc=0.8*sin(2*PI*440*t)|-0.8*sin(2*PI*440*t):s=44100:d=0.4'
    ) + $commonVideoOutput + @(
        '-map', '0:v:0', '-map', '1:a:0', '-c:a', 'pcm_s16le', '-ar', '44100', '-ac', '2',
        '-y', $AudioFixturePath
    )
    $videoCommand = $commonVideoInput + $commonVideoOutput + @('-an', '-y', $VideoFixturePath)

    Write-Output ("fixture-audio-command: " + $FFmpegExecutable + ' ' + ($audioCommand -join ' '))
    & $FFmpegExecutable @audioCommand
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $AudioFixturePath)) {
        throw 'Failed to generate the deterministic FFmpeg audio/video fixture.'
    }

    Write-Output ("fixture-video-command: " + $FFmpegExecutable + ' ' + ($videoCommand -join ' '))
    & $FFmpegExecutable @videoCommand
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $VideoFixturePath)) {
        throw 'Failed to generate the deterministic FFmpeg video-only fixture.'
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    foreach ($fixture in @($AudioFixturePath, $VideoFixturePath)) {
        $bytes = [System.IO.File]::ReadAllBytes($fixture)
        $hash = ([BitConverter]::ToString($sha256.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
        Write-Output ("fixture: {0} bytes={1} sha256={2}" -f $fixture, (Get-Item -LiteralPath $fixture).Length, $hash)
    }
    $sha256.Dispose()

    $ffprobeExecutable = Join-Path (Split-Path -Parent $FFmpegExecutable) 'ffprobe.exe'
    if (-not (Test-Path -LiteralPath $ffprobeExecutable)) {
        throw "The selected FFmpeg SDK is missing ffprobe.exe: $ffprobeExecutable"
    }
    $probeCommand = "$ffprobeExecutable -v error -count_frames -select_streams {stream}:0 -show_entries stream=nb_read_frames -of default=nw=1:nk=1 {fixture}"
    Write-Output ("fixture-count-command: " + $probeCommand.Replace('{stream}', 'v').Replace('{fixture}', $AudioFixturePath))
    $audioVideoFrames = & $ffprobeExecutable -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $AudioFixturePath
    $audioVideoSamples = & $ffprobeExecutable -v error -count_frames -select_streams a:0 -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $AudioFixturePath
    $videoOnlyFrames = & $ffprobeExecutable -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $VideoFixturePath
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to inspect deterministic FFmpeg fixture stream counts.'
    }
    Write-Output ("fixture-count audio-video video_frames={0} audio_frames={1}" -f ($audioVideoFrames -join ''), ($audioVideoSamples -join ''))
    Write-Output ("fixture-count video-only video_frames={0}" -f ($videoOnlyFrames -join ''))

    & $TestExecutable $AudioFixturePath $VideoFixturePath
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg movie playback test failed with exit code $LASTEXITCODE."
    }
}
finally {
    foreach ($fixture in @($AudioFixturePath, $VideoFixturePath)) {
        if (Test-Path -LiteralPath $fixture) {
            Remove-Item -LiteralPath $fixture -Force
        }
    }
}
