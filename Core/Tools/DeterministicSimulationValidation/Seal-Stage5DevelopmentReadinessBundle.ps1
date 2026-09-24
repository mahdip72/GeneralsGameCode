#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AcceptanceManifestPath,
    [Parameter(Mandatory = $true)][string]$ReadinessReportPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
    [switch]$ExternalQualificationExempt
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
Add-Type -AssemblyName System.IO.Compression

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Test-CanonicalUuid {
    param([string]$Value)
    return $Value -cmatch
        '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$'
}

function Get-ContainedRelativePath {
    param([string]$BaseDirectory, [string]$Path, [string]$Context)
    $base = [IO.Path]::GetFullPath($BaseDirectory)
    if ($base.Length -gt [IO.Path]::GetPathRoot($base).Length) {
        $base = $base.TrimEnd([char[]]@([IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar))
    }
    $candidate = [IO.Path]::GetFullPath($Path)
    Assert-Stage5FinalAcceptancePathContained $base $candidate $Context
    $relative = $candidate.Substring($base.Length).TrimStart([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($relative) -and
        -not [IO.Path]::IsPathRooted($relative) -and $relative -notmatch ':' -and
        $relative -notmatch '(^|[\\/])\.\.([\\/]|$)' -and
        $relative -notmatch '(^|[\\/])\.([\\/]|$)') `
        "$Context did not produce a canonical contained path."
    return $relative.Replace('\', '/')
}

function Get-StreamSha256 {
    param([IO.Stream]$Stream)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($Stream) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Read-JsonSnapshot {
    param([string]$Path, [string]$Context)
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $Path $Context
    return [pscustomobject]@{
        snapshot = $snapshot
        document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot $Context
    }
}

function Assert-ReportEquivalent {
    param([object]$Report, [object]$Regenerated, [string]$Context)
    [DateTimeOffset]$generated = [DateTimeOffset]::MinValue
    $generatedText = [string]$Report.generatedUtc
    Assert-Condition ($generatedText -cmatch
            '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        [DateTimeOffset]::TryParseExact($generatedText,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$generated) -and $generated -ge $script:CohortCreated -and
        $generated -le [DateTimeOffset]::UtcNow.AddMinutes(5)) `
        "$Context generatedUtc is stale or invalid."
    $reportClone = $Report | ConvertTo-Json -Depth 64 | ConvertFrom-Json
    $regeneratedClone = $Regenerated | ConvertTo-Json -Depth 64 | ConvertFrom-Json
    $reportClone.generatedUtc = [string]$regeneratedClone.generatedUtc
    $reportCanonical = $reportClone | ConvertTo-Json -Depth 64 -Compress
    $regeneratedCanonical = $regeneratedClone | ConvertTo-Json -Depth 64 -Compress
    Assert-Condition ($reportCanonical -ceq $regeneratedCanonical) `
        "$Context does not exactly match freshly regenerated development readiness."
}

$ExpectedSourceCommit = $ExpectedSourceCommit.ToLowerInvariant()
Assert-Condition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$') `
    'ExpectedSourceCommit must be lowercase 40-hex.'
Assert-Condition (Test-CanonicalUuid $ExpectedCohortNonce) `
    'ExpectedCohortNonce must be a canonical UUID.'
[DateTimeOffset]$script:CohortCreated = [DateTimeOffset]::MinValue
Assert-Condition ($ExpectedCohortCreatedUtc -cmatch
        '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
    [DateTimeOffset]::TryParseExact($ExpectedCohortCreatedUtc,
        'yyyy-MM-ddTHH:mm:ss.fffffffZ',
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal,
        [ref]$script:CohortCreated) -and
    $script:CohortCreated -le [DateTimeOffset]::UtcNow.AddMinutes(5)) `
    'ExpectedCohortCreatedUtc must be a canonical current UTC timestamp.'

$manifestFull = [IO.Path]::GetFullPath($AcceptanceManifestPath)
$bundleRoot = Split-Path -Parent $manifestFull
$reportFull = [IO.Path]::GetFullPath($ReadinessReportPath)
$outputFull = [IO.Path]::GetFullPath($OutputPath)
Assert-Condition ([IO.Path]::GetFileName($manifestFull) -ceq
        'FinalAcceptanceManifest.json' -and
    [IO.Path]::GetFileName($reportFull) -ceq 'Stage5DevelopmentReadiness.json' -and
    [IO.Path]::GetFileName($outputFull) -ceq
        'Stage5DevelopmentReadinessBundle.zip' -and
    [IO.Path]::GetFullPath((Split-Path -Parent $reportFull)) -ceq $bundleRoot -and
    [IO.Path]::GetFullPath((Split-Path -Parent $outputFull)) -ceq $bundleRoot) `
    'The manifest, readiness report, and sealed archive must use their exact names in one bundle root.'
Assert-Stage5FinalAcceptanceNoReparsePath $bundleRoot $manifestFull `
    'Stage 5 acceptance manifest seal input'
Assert-Stage5FinalAcceptanceNoReparsePath $bundleRoot $reportFull `
    'Stage 5 readiness report seal input'
Assert-Condition (-not (Test-Path -LiteralPath $outputFull)) `
    "Sealed readiness archive already exists: $outputFull"

$manifestEvidence = Read-JsonSnapshot $manifestFull 'Stage 5 acceptance manifest seal input'
$manifest = $manifestEvidence.document
Assert-Stage5JsonShape $manifest @('schemaVersion', 'gateName', 'sourceCommit',
    'cohortNonce', 'cohortCreatedUtc', 'artifactSet', 'evidence') `
    'Stage 5 acceptance manifest seal input'
Assert-Condition ((Test-Stage5JsonInteger $manifest.schemaVersion) -and
    $manifest.schemaVersion -eq 1 -and
    [string]$manifest.gateName -ceq 'final-stage5-acceptance' -and
    [string]$manifest.sourceCommit -ceq $ExpectedSourceCommit -and
    [string]$manifest.cohortNonce -ceq $ExpectedCohortNonce -and
    [string]$manifest.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc) `
    'Stage 5 acceptance manifest seal identity is stale or substituted.'
$artifactReference = Get-Stage5JsonValue $manifest 'artifactSet' `
    'Stage 5 acceptance manifest seal input'
Assert-Stage5JsonShape $artifactReference @('path', 'sha256') `
    'Stage 5 acceptance manifest artifact-set reference'
$artifactRelative = [string]$artifactReference.path
Assert-Condition ($artifactRelative -ceq 'Stage5ArtifactSet.json') `
    'Sealed readiness requires the exact canonical Stage5ArtifactSet.json path.'
$artifactSetPath = [IO.Path]::GetFullPath((Join-Path $bundleRoot $artifactRelative))
Assert-Stage5FinalAcceptanceNoReparsePath $bundleRoot $artifactSetPath `
    'Stage 5 sealed artifact set'
$artifactEvidence = Read-JsonSnapshot $artifactSetPath 'Stage 5 sealed artifact set'
$artifactHash = Assert-Stage5FinalAcceptanceSnapshotSha256 `
    $artifactEvidence.snapshot ([string]$artifactReference.sha256) `
    'Stage 5 sealed artifact set'
$runtime = Get-Stage5RuntimeClosureBinding $artifactEvidence.document $bundleRoot `
    $ExpectedSourceCommit 'Stage 5 sealed runtime closure'

$reportEvidence = Read-JsonSnapshot $reportFull 'Stage 5 readiness report seal input'
$report = $reportEvidence.document
$regenerated = Invoke-Stage5FinalAcceptanceAggregation $manifestFull `
    -DevelopmentReadiness `
    -ExternalQualificationExempt:$ExternalQualificationExempt
$validatedClosure = @(Get-Stage5FinalAcceptanceValidatedClosure)
Assert-Condition ($validatedClosure.Count -gt 0) `
    'Final acceptance returned an empty validated evidence closure.'
Assert-ReportEquivalent $report $regenerated 'Stage 5 readiness report seal input'
Assert-Condition ([string]$regenerated.sourceCommit -ceq $ExpectedSourceCommit -and
    [string]$regenerated.cohortNonce -ceq $ExpectedCohortNonce -and
    [string]$regenerated.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
    [string]$regenerated.artifactSet.sha256 -ceq $artifactHash) `
    'Freshly regenerated readiness is detached from the requested candidate.'

$sourceByRelative = New-Object 'Collections.Generic.Dictionary[string,object]' `
    ([StringComparer]::OrdinalIgnoreCase)
function Add-BundleSource {
    param([string]$Path, [string]$ExpectedSha256 = '')
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5FinalAcceptanceNoReparsePath $bundleRoot $full `
        'Stage 5 sealed payload source'
    Assert-Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Stage 5 sealed payload source is missing: $full"
    $relative = Get-ContainedRelativePath $bundleRoot $full `
        'Stage 5 sealed payload source'
    Assert-Condition ($relative -cne 'BundleManifest.json' -and
        $relative -cne 'Stage5DevelopmentReadinessBundle.zip' -and
        -not $sourceByRelative.ContainsKey($relative)) `
        "Stage 5 sealed payload source aliases or reserves '$relative'."
    $sourceByRelative.Add($relative, [pscustomobject]@{
        path = $full
        relative = $relative
        expectedSha256 = if ([string]::IsNullOrWhiteSpace($ExpectedSha256)) {
            ''
        }
        else { $ExpectedSha256.ToUpperInvariant() }
    })
}

Add-BundleSource $manifestFull ([string]$manifestEvidence.snapshot.sha256)
Add-BundleSource $reportFull ([string]$reportEvidence.snapshot.sha256)
Add-BundleSource $artifactSetPath $artifactHash
$runtimeManifestPath = [IO.Path]::GetFullPath((Join-Path $bundleRoot `
    ([string]$runtime.dependencyManifestPath)))
Add-BundleSource $runtimeManifestPath ([string]$runtime.dependencyManifestSha256)
foreach ($file in @($runtime.files)) {
    Add-BundleSource ([string]$file.fullPath) ([string]$file.sha256)
}
foreach ($validated in $validatedClosure) {
    Assert-Condition ($null -ne $validated -and
        [string]$validated.sha256 -cmatch '^[0-9A-F]{64}$' -and
        [Int64]$validated.length -ge 0) `
        'Final acceptance returned a malformed validated closure entry.'
    $file = [IO.Path]::GetFullPath([string]$validated.path)
    $relative = Get-ContainedRelativePath $bundleRoot $file `
        'Stage 5 validated closure file'
    if (-not $sourceByRelative.ContainsKey($relative)) {
        Add-BundleSource $file ([string]$validated.sha256)
    }
    else {
        $prior = $sourceByRelative[$relative]
        Assert-Condition ([string]$prior.path -ceq $file -and
            ([string]::IsNullOrWhiteSpace([string]$prior.expectedSha256) -or
                [string]$prior.expectedSha256 -ceq
                    [string]$validated.sha256)) `
            "Validated closure entry '$relative' differs from its fixed bundle source."
    }
}
Assert-Condition ($sourceByRelative.Count -le 32768) `
    'Stage 5 sealed payload exceeds the 32768-entry bound.'

$orderedSources = @($sourceByRelative.Values | Sort-Object relative)
$sealedSourceBytes = [Int64]0
foreach ($sourceRecord in $orderedSources) {
    $sealedSourceBytes += [Int64](Get-Item -LiteralPath `
        ([string]$sourceRecord.path) -Force -ErrorAction Stop).Length
}
Assert-Condition ($sealedSourceBytes -le [Int64]68719476736) `
    'Stage 5 validated seal closure exceeds the 64-GiB bound.'
$entryRecords = New-Object 'Collections.Generic.List[object]'
$archiveStream = [IO.FileStream]::new($outputFull, [IO.FileMode]::CreateNew,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 1048576,
    [IO.FileOptions]::WriteThrough)
try {
    Assert-Stage5FinalAcceptanceFileHandlePath $archiveStream $outputFull `
        'Stage 5 sealed readiness archive' | Out-Null
    $archive = [IO.Compression.ZipArchive]::new($archiveStream,
        [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($sourceRecord in $orderedSources) {
            $source = [IO.FileStream]::new([string]$sourceRecord.path,
                [IO.FileMode]::Open, [IO.FileAccess]::Read,
                [IO.FileShare]::Read, 1048576, [IO.FileOptions]::SequentialScan)
            try {
                Assert-Stage5FinalAcceptanceFileHandlePath $source `
                    ([string]$sourceRecord.path) `
                    "Stage 5 sealed entry '$($sourceRecord.relative)'" | Out-Null
                $length = [Int64]$source.Length
                $sha256 = Get-StreamSha256 $source
                Assert-Condition ([string]::IsNullOrWhiteSpace(
                        [string]$sourceRecord.expectedSha256) -or
                    $sha256 -ceq [string]$sourceRecord.expectedSha256) `
                    "Stage 5 sealed entry '$($sourceRecord.relative)' changed after validation."
                $source.Position = 0
                $entry = $archive.CreateEntry([string]$sourceRecord.relative,
                    [IO.Compression.CompressionLevel]::Fastest)
                $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0,
                    [TimeSpan]::Zero)
                $entryStream = $entry.Open()
                try { $source.CopyTo($entryStream, 1048576) }
                finally { $entryStream.Dispose() }
                $entryRecords.Add([ordered]@{
                    path = [string]$sourceRecord.relative
                    length = $length
                    sha256 = $sha256
                }) | Out-Null
            }
            finally { $source.Dispose() }
        }
        $bundleManifest = [ordered]@{
            schemaVersion = 1
            evidenceKind = 'stage5-development-readiness-bundle'
            sourceCommit = $ExpectedSourceCommit
            artifactSetSha256 = $artifactHash
            cohortNonce = $ExpectedCohortNonce
            cohortCreatedUtc = $ExpectedCohortCreatedUtc
            runtimeClosure = [ordered]@{
                dependencyManifestSha256 = [string]$runtime.dependencyManifestSha256
                closureSha256 = [string]$runtime.closureSha256
            }
            entryCount = $entryRecords.Count
            entries = $entryRecords.ToArray()
        }
        $manifestBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
            ($bundleManifest | ConvertTo-Json -Depth 16))
        $manifestEntry = $archive.CreateEntry('BundleManifest.json',
            [IO.Compression.CompressionLevel]::Fastest)
        $manifestEntry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0,
            [TimeSpan]::Zero)
        $manifestEntryStream = $manifestEntry.Open()
        try { $manifestEntryStream.Write($manifestBytes, 0, $manifestBytes.Length) }
        finally { $manifestEntryStream.Dispose() }
    }
    finally { $archive.Dispose() }
    $archiveStream.Flush($true)
}
finally { $archiveStream.Dispose() }

$archiveRead = [IO.FileStream]::new($outputFull, [IO.FileMode]::Open,
    [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576,
    [IO.FileOptions]::RandomAccess)
try {
    Assert-Stage5FinalAcceptanceFileHandlePath $archiveRead $outputFull `
        'Stage 5 sealed readiness archive verification' | Out-Null
    $archiveSha256 = Get-StreamSha256 $archiveRead
    $archiveRead.Position = 0
    $archive = [IO.Compression.ZipArchive]::new($archiveRead,
        [IO.Compression.ZipArchiveMode]::Read, $true)
    try {
        $entries = @($archive.Entries)
        $entryNames = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $entries) {
            Assert-Condition ($entryNames.Add([string]$entry.FullName) -and
                -not [IO.Path]::IsPathRooted([string]$entry.FullName) -and
                [string]$entry.FullName -notmatch ':' -and
                [string]$entry.FullName -notmatch '(^|/)\.\.(/|$)' -and
                [string]$entry.FullName -notmatch '(^|/)\.(/|$)') `
                "Sealed archive contains an unsafe or duplicate entry '$($entry.FullName)'."
        }
        $manifestEntries = @($entries | Where-Object {
            [string]$_.FullName -ceq 'BundleManifest.json'
        })
        Assert-Condition ($manifestEntries.Count -eq 1 -and
            $manifestEntries[0].Length -gt 0 -and
            $manifestEntries[0].Length -le 16777216) `
            'Sealed archive must contain one bounded BundleManifest.json.'
        $manifestStream = $manifestEntries[0].Open()
        try {
            $reader = [IO.StreamReader]::new($manifestStream,
                (New-Object Text.UTF8Encoding($false, $true)), $true, 4096, $true)
            try { $manifestText = $reader.ReadToEnd() }
            finally { $reader.Dispose() }
        }
        finally { $manifestStream.Dispose() }
        $sealedManifest = $manifestText | ConvertFrom-Json
        Assert-Condition ((Test-Stage5JsonInteger $sealedManifest.schemaVersion) -and
            $sealedManifest.schemaVersion -eq 1 -and
            [string]$sealedManifest.evidenceKind -ceq
                'stage5-development-readiness-bundle' -and
            [string]$sealedManifest.sourceCommit -ceq $ExpectedSourceCommit -and
            [string]$sealedManifest.artifactSetSha256 -ceq $artifactHash -and
            [string]$sealedManifest.cohortNonce -ceq $ExpectedCohortNonce -and
            [string]$sealedManifest.cohortCreatedUtc -ceq
                $ExpectedCohortCreatedUtc -and
            [int]$sealedManifest.entryCount -eq $entryRecords.Count -and
            @($sealedManifest.entries).Count -eq $entryRecords.Count -and
            $entries.Count -eq $entryRecords.Count + 1) `
            'Sealed archive manifest identity or entry count is invalid.'
        foreach ($record in @($sealedManifest.entries)) {
            $matching = @($entries | Where-Object {
                [string]$_.FullName -ceq [string]$record.path
            })
            Assert-Condition ($matching.Count -eq 1 -and
                [Int64]$matching[0].Length -eq [Int64]$record.length -and
                [string]$record.sha256 -cmatch '^[0-9A-F]{64}$') `
                "Sealed archive entry '$($record.path)' is missing or has the wrong length."
            $entryStream = $matching[0].Open()
            try { $entryHash = Get-StreamSha256 $entryStream }
            finally { $entryStream.Dispose() }
            Assert-Condition ($entryHash -ceq [string]$record.sha256) `
                "Sealed archive entry '$($record.path)' failed SHA-256 self-validation."
        }
    }
    finally { $archive.Dispose() }
}
finally { $archiveRead.Dispose() }

# Revalidate after archive creation, then prove every source path still matches
# the exact bytes sealed above. This closes ordinary workflow mutation windows;
# defending against a hostile same-user process replacing the closed archive is
# outside the trusted runner threat model.
$postSealReport = Invoke-Stage5FinalAcceptanceAggregation $manifestFull `
    -DevelopmentReadiness `
    -ExternalQualificationExempt:$ExternalQualificationExempt
$reportAfterSeal = (Read-JsonSnapshot $reportFull `
    'Stage 5 readiness report post-seal verification').document
Assert-ReportEquivalent $reportAfterSeal $postSealReport `
    'Stage 5 readiness report post-seal verification'
$sealedRecordsByPath = @{}
foreach ($record in $entryRecords) { $sealedRecordsByPath[[string]$record.path] = $record }
foreach ($sourceRecord in $orderedSources) {
    $actualHash = Get-Stage5FileSha256 ([string]$sourceRecord.path)
    Assert-Condition ($actualHash -ceq
        [string]$sealedRecordsByPath[[string]$sourceRecord.relative].sha256) `
        "Stage 5 source '$($sourceRecord.relative)' changed during sealing."
}

Write-Output ([pscustomobject]@{
    path = $outputFull
    sha256 = $archiveSha256
    entryCount = $entryRecords.Count
})
