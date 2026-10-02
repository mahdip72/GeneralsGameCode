[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GeneralsHostPath,
    [Parameter(Mandatory = $true)][string]$GeneralsHostSha256,
    [Parameter(Mandatory = $true)][string]$ZeroHourHostPath,
    [Parameter(Mandatory = $true)][string]$ZeroHourHostSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
    [Parameter(Mandatory = $true)][string]$ExpectedDependencyManifestSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedRuntimeClosureSha256,
    [Parameter(Mandatory = $true)][string]$GeneralsExecutableSha256,
    [Parameter(Mandatory = $true)][string]$ZeroHourExecutableSha256,
    [Parameter(Mandatory = $true)][string]$OutputRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$modulePath = Join-Path $PSScriptRoot `
    'Stage5InstalledKernelExecutionEvidence.psm1'
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "The installed-kernel evidence module is absent: $modulePath"
}
Import-Module $modulePath -Force

New-Stage5InstalledKernelExecutionEvidence @PSBoundParameters
