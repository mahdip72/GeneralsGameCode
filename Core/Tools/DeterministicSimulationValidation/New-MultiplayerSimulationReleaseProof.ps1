[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EvidenceManifestPath,
    [Parameter(Mandatory = $true)][string]$RawEvidenceIndexPath,
    [Parameter(Mandatory = $true)][string]$ArtifactSetManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedGeneralsExecutableSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedZeroHourExecutableSha256,
    [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')][string]$Title,
    [Parameter(Mandatory = $true)][UInt32]$ExpectedBuildCompatibilityCrc,
    [Parameter(Mandatory = $true)][UInt32]$ExpectedContentCrc
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

$outputFull = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetFileName($outputFull) -cne 'MultiplayerSimulationRuntimeProof.txt') {
    throw 'The external release-proof output must be named MultiplayerSimulationRuntimeProof.txt.'
}
if (Test-Path -LiteralPath $outputFull) {
    throw "Release-proof output already exists; refusing to overwrite evidence-bound configuration: $outputFull"
}
$outputDirectory = Split-Path -Parent $outputFull
$evidenceFull = [IO.Path]::GetFullPath($EvidenceManifestPath)
$rawIndexFull = [IO.Path]::GetFullPath($RawEvidenceIndexPath)
$artifactManifestFull = [IO.Path]::GetFullPath($ArtifactSetManifestPath)
if ([IO.Path]::GetFileName($evidenceFull) -cne 'Net3LoopbackEvidence.json' -or
    [IO.Path]::GetFileName($rawIndexFull) -cne 'MultiplayerSimulationRawEvidence.index' -or
    [IO.Path]::GetFileName($artifactManifestFull) -cne 'Stage5ArtifactSet.json' -or
    (Split-Path -Parent $evidenceFull) -cne $outputDirectory -or
    (Split-Path -Parent $rawIndexFull) -cne $outputDirectory -or
    (Split-Path -Parent $artifactManifestFull) -cne $outputDirectory) {
    throw 'Proof, evidence manifest, raw index, and artifact-set manifest must use fixed names in one external bundle directory.'
}
foreach ($required in @($evidenceFull, $rawIndexFull, $artifactManifestFull)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required external proof input was not found: $required"
    }
}
if ((Get-Stage5FileSha256 $artifactManifestFull) -cne
    $ExpectedArtifactSetSha256) {
    throw 'External proof artifact-set manifest does not match independent provenance.'
}
$rawLines = @(Get-Content -LiteralPath $rawIndexFull)
$rawIndexEntries = @()
if ($rawLines.Count -ne 42 -or
    $rawLines[0] -cne 'RTS_MULTIPLAYER_SIMULATION_RAW_EVIDENCE_V1' -or
    $rawLines[41] -cne 'END') {
    throw 'Raw evidence index must contain the exact 40-peer canonical record set.'
}
for ($index = 0; $index -lt 40; ++$index) {
    $match = [regex]::Match($rawLines[$index + 1],
        '^(?<ordinal>[0-9]{2})\|(?<path>[^|]+)\|(?<sha>[0-9A-F]{64})$')
    if (-not $match.Success -or [int]$match.Groups['ordinal'].Value -ne $index) {
        throw "Raw evidence index entry $index is not canonical."
    }
    $relative = $match.Groups['path'].Value
    if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or
        $relative.Contains(':') -or
        -not $relative.StartsWith('Net3Raw\', [StringComparison]::Ordinal)) {
        throw "Raw evidence index entry $index has an unsafe path."
    }
    $rawFile = [IO.Path]::GetFullPath((Join-Path $outputDirectory $relative))
    $prefix = $outputDirectory.TrimEnd('\') + '\'
    if (-not $rawFile.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $rawFile -PathType Leaf) -or
        (Get-Stage5FileSha256 $rawFile) -cne
            $match.Groups['sha'].Value) {
        throw "Raw evidence index entry $index does not match its peer output."
    }
    $rawIndexEntries += [pscustomobject]@{
        path = $relative
        sha256 = $match.Groups['sha'].Value
    }
}
$rawIndexSha256 = Get-Stage5FileSha256 $rawIndexFull

# Validate before creating the destination directory or file. This command
# consumes the installed runner's raw 16-match/40-peer receipts and never writes
# into a source/build include tree. The exact prebuilt executable is therefore
# unchanged by proof creation and re-hashed by the runtime before activation.
$expectedGeneralsBuildCrc = if ($Title -ceq 'Generals') { $ExpectedBuildCompatibilityCrc } else { 0 }
$expectedZeroHourBuildCrc = if ($Title -ceq 'ZeroHour') { $ExpectedBuildCompatibilityCrc } else { 0 }
$expectedGeneralsContentCrc = if ($Title -ceq 'Generals') { $ExpectedContentCrc } else { 0 }
$expectedZeroHourContentCrc = if ($Title -ceq 'ZeroHour') { $ExpectedContentCrc } else { 0 }
$proof = Read-Stage5Net3LoopbackEvidence -Path $EvidenceManifestPath `
    -ExpectedSourceCommit $ExpectedSourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ExpectedGeneralsExecutableSha256 $ExpectedGeneralsExecutableSha256 `
    -ExpectedZeroHourExecutableSha256 $ExpectedZeroHourExecutableSha256 `
    -ExpectedGeneralsBuildCompatibilityCrc $expectedGeneralsBuildCrc `
    -ExpectedZeroHourBuildCompatibilityCrc $expectedZeroHourBuildCrc `
    -ExpectedGeneralsContentCrc $expectedGeneralsContentCrc `
    -ExpectedZeroHourContentCrc $expectedZeroHourContentCrc
if ($proof.rawEvidenceEntries.Count -ne 40) {
    throw 'Strict NET3 evidence parser did not return the exact 40 raw peer records.'
}
for ($index = 0; $index -lt 40; ++$index) {
    if ($rawIndexEntries[$index].path -cne $proof.rawEvidenceEntries[$index].path -or
        $rawIndexEntries[$index].sha256 -cne $proof.rawEvidenceEntries[$index].sha256) {
        throw "Raw evidence index entry $index does not match the canonical evidence peer order."
    }
}

$executableSha256 = if ($Title -ceq 'Generals') {
    $proof.generalsExecutableSha256
} else { $proof.zeroHourExecutableSha256 }
$buildCrc = if ($Title -ceq 'Generals') {
    $proof.generalsBuildCompatibilityCrc
} else { $proof.zeroHourBuildCompatibilityCrc }
$contentCrc = if ($Title -ceq 'Generals') {
    $proof.generalsContentCrc
} else { $proof.zeroHourContentCrc }

$content = @"
RTS_MULTIPLAYER_SIMULATION_RUNTIME_PROOF_V1
schema=1
title=$Title
source_revision=$($proof.sourceCommit)
executable_sha256=$executableSha256
artifact_set_sha256=$($proof.artifactSetSha256)
evidence_manifest_sha256=$($proof.evidenceManifestSha256)
raw_evidence_index_sha256=$rawIndexSha256
policy_schema=1
engine_epoch=1
determinism_epoch=1
build_compatibility_crc=$buildCrc
content_crc=$contentCrc
proven_kernel_mask=$($proof.provenKernelMask)
match_count=$($proof.matchCount)
peer_process_count=$($proof.peerRecordCount)
producer=installed-runtime-runner-v1
validation_mode=scoped-net3-loopback-release-proof
END
"@
$utf8WithoutBom = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText($outputFull, $content, $utf8WithoutBom)
Write-Output "Generated external $Title multiplayer runtime evidence for executable $executableSha256; it cannot exceed embedded build authority."
