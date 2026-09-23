[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AcceptanceManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [ValidateSet('development', 'development-readiness', 'pre-manual')]
    [string]$ReadinessMode = 'development-readiness',
    [switch]$DevelopmentReadiness,
    [switch]$ExternalQualificationExempt
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
Assert-Stage5FinalAcceptanceNoReparsePath `
    ([IO.Path]::GetPathRoot($outputDirectory)) $outputDirectory `
    'Final acceptance output'

# The module validates and independently rehashes the artifact set, all
# development evidence manifests, and every evidence attachment before
# returning a pre-manual readiness report. External premium review and user
# approval are never represented as local JSON authority.
$report = Invoke-Stage5FinalAcceptanceAggregation `
    -AcceptanceManifestPath $AcceptanceManifestPath `
    -ReadinessMode $ReadinessMode `
    -DevelopmentReadiness:$DevelopmentReadiness `
    -ExternalQualificationExempt:$ExternalQualificationExempt
$reportJson = $report | ConvertTo-Json -Depth 10
$reportBytes = ([Text.UTF8Encoding]::new($false)).GetBytes([string]$reportJson)
[void](Write-Stage5FinalAcceptanceFileAtomically -Path $outputFull `
    -Bytes $reportBytes -Context 'Final acceptance output')
Write-Output "Stage 5 development readiness passed for commit $($report.sourceCommit); final user manual approval remains required."
