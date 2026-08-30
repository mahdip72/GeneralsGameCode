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

    $audioOnlyFixturePath = [IO.Path]::ChangeExtension($AudioFixturePath, '.wav')
    foreach ($fixture in @($AudioFixturePath, $VideoFixturePath, $audioOnlyFixturePath)) {
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
    $audioCommand = @(
        '-hide_banner', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'aevalsrc=0.8*sin(2*PI*440*t)|-0.8*sin(2*PI*440*t):s=44100:d=0.55',
        '-c:a', 'pcm_s16le', '-ar', '44100', '-ac', '2', '-t', '0.55',
        '-y', $audioOnlyFixturePath
    )
    $videoCommand = $commonVideoInput + $commonVideoOutput + @('-an', '-y', $VideoFixturePath)
    $muxCommand = @(
        '-hide_banner', '-loglevel', 'error',
        '-i', $VideoFixturePath, '-i', $audioOnlyFixturePath,
        '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'pcm_s16le',
        '-ar', '44100', '-ac', '2', '-output_ts_offset', '5', '-y', $AudioFixturePath
    )

    Write-Output ("fixture-audio-command: " + $FFmpegExecutable + ' ' + ($audioCommand -join ' '))
    & $FFmpegExecutable @audioCommand
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $audioOnlyFixturePath)) {
        throw 'Failed to generate the deterministic FFmpeg audio fixture.'
    }

    Write-Output ("fixture-video-command: " + $FFmpegExecutable + ' ' + ($videoCommand -join ' '))
    & $FFmpegExecutable @videoCommand
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $VideoFixturePath)) {
        throw 'Failed to generate the deterministic FFmpeg video-only fixture.'
    }

    Write-Output ("fixture-mux-command: " + $FFmpegExecutable + ' ' + ($muxCommand -join ' '))
    & $FFmpegExecutable @muxCommand
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $AudioFixturePath)) {
        throw 'Failed to mux the deterministic audio/video fixture.'
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    foreach ($fixture in @($AudioFixturePath, $VideoFixturePath, $audioOnlyFixturePath)) {
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
    if ($LASTEXITCODE -ne 0 -or ($audioVideoFrames -join '') -ne '12') {
        throw 'The deterministic audio/video fixture does not contain exactly 12 readable video frames.'
    }
    $firstVideoPts = & $ffprobeExecutable -v error -read_intervals '%+#1' -select_streams v:0 -show_entries packet=pts_time -of default=nw=1:nk=1 $AudioFixturePath
    $firstVideoPtsValue = 0.0
    if ($LASTEXITCODE -ne 0 -or -not [double]::TryParse(($firstVideoPts -join ''),
            [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture,
            [ref]$firstVideoPtsValue) -or $firstVideoPtsValue -lt 4.9) {
        throw 'The deterministic audio/video fixture does not exercise a nonzero initial presentation timestamp.'
    }
    $audioVideoSamples = & $ffprobeExecutable -v error -select_streams a:0 -show_entries stream=sample_rate,duration_ts -of default=nw=1:nk=1 $AudioFixturePath
    if ($LASTEXITCODE -ne 0 -or $audioVideoSamples.Count -ne 2 -or
        ($audioVideoSamples -join ',') -ne '44100,24255') {
        throw 'The deterministic fixture does not contain exactly 24,255 samples at 44.1 kHz (26,400 at the 48 kHz output rate).'
    }
    $videoOnlyFrames = & $ffprobeExecutable -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $VideoFixturePath
    if ($LASTEXITCODE -ne 0 -or ($videoOnlyFrames -join '') -ne '12') {
        throw 'The deterministic video-only fixture does not contain exactly 12 readable video frames.'
    }
    Write-Output ("fixture-count audio-video video_frames={0} audio_contract={1}" -f ($audioVideoFrames -join ''), ($audioVideoSamples -join ','))
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
