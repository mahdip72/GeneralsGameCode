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
$existingOutputParent = $outputDirectory
while (-not (Test-Path -LiteralPath $existingOutputParent -PathType Container)) {
    $parent = Split-Path -Parent $existingOutputParent
    if ([string]::IsNullOrWhiteSpace($parent) -or
        [String]::Equals($parent, $existingOutputParent,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Final acceptance output has no existing directory ancestor: $outputDirectory"
    }
    $existingOutputParent = $parent
}
$outputRoot = [IO.Path]::GetPathRoot($existingOutputParent)
if ([String]::Equals($existingOutputParent, $outputRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    $outputRootItem = Get-Item -LiteralPath $outputRoot -Force -ErrorAction Stop
    if (($outputRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Final acceptance output volume root is a reparse point: $outputRoot"
    }
}
else {
    Assert-Stage5FinalAcceptanceNoReparsePath `
        $outputRoot $existingOutputParent 'Final acceptance output existing parent'
}
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
