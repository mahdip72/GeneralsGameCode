[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EvidenceManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedGeneralsExecutableSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedZeroHourExecutableSha256
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

$outputFull = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetFileName($outputFull) -cne 'MultiplayerSimulationReleaseProof.generated.h') {
    throw 'The release-proof output must be named MultiplayerSimulationReleaseProof.generated.h.'
}
if (Test-Path -LiteralPath $outputFull) {
    throw "Release-proof output already exists; refusing to overwrite evidence-bound configuration: $outputFull"
}

# Validate before creating the destination directory or file. Invalid, absent,
# partial, stale, or self-asserted evidence therefore cannot leave a header that
# advertises a nonzero proof mask. Runtime consumers must define their default
# proven mask as zero when this generated header is absent.
$proof = Read-Stage5Net3LoopbackEvidence -Path $EvidenceManifestPath `
    -ExpectedSourceCommit $ExpectedSourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ExpectedGeneralsExecutableSha256 $ExpectedGeneralsExecutableSha256 `
    -ExpectedZeroHourExecutableSha256 $ExpectedZeroHourExecutableSha256

$content = @"
#pragma once

// Generated only from a fully validated installed NET3 loopback evidence set.
// Do not edit or define these values through a free-form build option.
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_SCHEMA 1
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_SOURCE_REVISION "$($proof.sourceCommit)"
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_ARTIFACT_SET_SHA256 "$($proof.artifactSetSha256)"
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_EVIDENCE_MANIFEST_SHA256 "$($proof.evidenceManifestSha256)"
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_GENERALS_EXE_SHA256 "$($proof.generalsExecutableSha256)"
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_ZERO_HOUR_EXE_SHA256 "$($proof.zeroHourExecutableSha256)"
#define RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_PROVEN_KERNEL_MASK 0x0000003Fu
"@
$outputDirectory = Split-Path -Parent $outputFull
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$utf8WithoutBom = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText($outputFull, $content, $utf8WithoutBom)
Write-Output "Generated evidence-bound multiplayer release proof for commit $($proof.sourceCommit)."
