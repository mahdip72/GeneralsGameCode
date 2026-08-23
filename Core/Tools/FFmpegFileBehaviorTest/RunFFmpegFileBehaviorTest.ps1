param(
    [Parameter(Mandatory = $true)]
    [string]$FFmpegExecutable,

    [Parameter(Mandatory = $true)]
    [string]$TestExecutable,

    [Parameter(Mandatory = $true)]
    [string]$FixturePath,

    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'

try {
    if (Test-Path -LiteralPath $FixturePath) {
        Remove-Item -LiteralPath $FixturePath -Force
    }

    if (-not [string]::IsNullOrWhiteSpace($RuntimeDirectory)) {
        $env:PATH = $RuntimeDirectory + [IO.Path]::PathSeparator + $env:PATH
    }

    & $FFmpegExecutable -hide_banner -loglevel error `
        -f lavfi -i 'testsrc2=size=256x256:rate=24:duration=0.5' `
        -frames:v 12 -c:v mpeg4 -bf 2 -g 12 -sc_threshold 0 -q:v 2 -pix_fmt yuv420p -threads 1 -an -y $FixturePath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $FixturePath)) {
        throw 'Failed to generate the deterministic FFmpegFile behavior fixture.'
    }

    & $TestExecutable $FixturePath
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpegFile behavior test failed with exit code $LASTEXITCODE."
    }
} finally {
    if (Test-Path -LiteralPath $FixturePath) {
        Remove-Item -LiteralPath $FixturePath -Force
    }
}
