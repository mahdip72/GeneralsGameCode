[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AcceptanceManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

$outputFull = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputFull) {
    throw "Final acceptance output already exists; refusing to overwrite evidence: $outputFull"
}
$outputDirectory = Split-Path -Parent $outputFull
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

# The module validates and independently rehashes the artifact set, all eight
# evidence manifests, and every evidence attachment before returning a passed
# report. Any absent, stale, mismatched, or non-passing input throws before the
# report can be written.
$report = Invoke-Stage5FinalAcceptanceAggregation `
    -AcceptanceManifestPath $AcceptanceManifestPath
[IO.File]::WriteAllText($outputFull, ($report | ConvertTo-Json -Depth 10))
Write-Output "Stage 5 final acceptance passed for commit $($report.sourceCommit)."
