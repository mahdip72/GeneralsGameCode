[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GeneralsReceiptPath,
    [Parameter(Mandatory = $true)][string]$ZeroHourReceiptPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedGeneralsExecutableSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedZeroHourExecutableSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

function Assert-CombinedCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-CombinedRelativePath {
    param([string]$Path, [string]$Context)
    Assert-CombinedCondition (-not [string]::IsNullOrWhiteSpace($Path) -and
        -not [IO.Path]::IsPathRooted($Path) -and
        $Path -notmatch '(^|[\\/])\.\.([\\/]|$)') `
        "$Context must be a non-rooted path without parent traversal."
}

function Assert-CombinedNoReparsePath {
    param([string]$Path, [string]$Context)
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    Assert-CombinedCondition ($root -is [string] -and -not [string]::IsNullOrWhiteSpace($root)) `
        "$Context path does not have a valid volume root: $full"
    $current = $root
    foreach ($segment in @($full.Substring($root.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $current = Join-Path $current $segment
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-CombinedCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$segment' is a reparse point: $current"
    }
}

function Get-CombinedFileSnapshot {
    param([string]$Path, [string]$Context)
    $full = [IO.Path]::GetFullPath($Path)
    Assert-CombinedNoReparsePath $full $Context
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-CombinedCondition (($item -is [IO.FileInfo]) -and
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context file is a reparse point: $full"
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $beforeLength = [Int64]$item.Length
        Assert-CombinedCondition ($stream.Length -eq $beforeLength) `
            "$Context file changed before its immutable copy began: $full"
        $memory = New-Object IO.MemoryStream
        try {
            $stream.CopyTo($memory)
            $bytes = $memory.ToArray()
        }
        finally { $memory.Dispose() }
    }
    finally { $stream.Dispose() }
    Assert-CombinedNoReparsePath $full $Context
    $after = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-CombinedCondition (($after -is [IO.FileInfo]) -and
        ($after.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and
        [Int64]$after.Length -eq $beforeLength -and
        [Int64]$bytes.LongLength -eq $beforeLength -and
        [DateTime]$after.CreationTimeUtc -eq [DateTime]$item.CreationTimeUtc -and
        [DateTime]$after.LastWriteTimeUtc -eq [DateTime]$item.LastWriteTimeUtc) `
        "$Context file changed or was replaced while it was snapshotted: $full"
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
    return [pscustomobject]@{
        path = $full; bytes = $bytes; sha256 = $hash
        length = $beforeLength
        creationTimeUtc = ([DateTime]$item.CreationTimeUtc).ToString('o')
        lastWriteTimeUtc = ([DateTime]$item.LastWriteTimeUtc).ToString('o')
        identity = '{0}|{1}|{2}' -f [Int64]$item.Length,
            ([DateTime]$item.CreationTimeUtc).Ticks,
            ([DateTime]$item.LastWriteTimeUtc).Ticks
    }
}

function ConvertFrom-CombinedJsonSnapshot {
    param([object]$Snapshot, [string]$Context)
    Assert-CombinedCondition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'bytes') `
        "$Context does not contain an immutable byte snapshot."
    $text = [Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes)
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        return $text | ConvertFrom-Json -AsHashtable
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = 10485760
    return $serializer.DeserializeObject($text)
}

function Copy-CombinedSourceFile {
    param(
        [string]$SourceBase,
        [string]$RelativePath,
        [string]$DestinationBase,
        [string]$DestinationRelativePath,
        [string]$ExpectedSha256,
        [string]$Context,
        [hashtable]$Copied,
        [object]$SourceSnapshot = $null
    )
    Assert-CombinedRelativePath $RelativePath "$Context source path"
    Assert-CombinedRelativePath $DestinationRelativePath "$Context destination path"
    Assert-CombinedCondition ($ExpectedSha256 -match '^[0-9A-Fa-f]{64}$') `
        "$Context expected SHA-256 is not canonical."
    $sourceFull = [IO.Path]::GetFullPath((Join-Path $SourceBase $RelativePath))
    $destinationFull = [IO.Path]::GetFullPath((Join-Path $DestinationBase $DestinationRelativePath))
    Assert-CombinedCondition (Test-Path -LiteralPath $sourceFull -PathType Leaf) `
        "$Context source file was not found: $sourceFull"
    $sourceItem = Get-Item -LiteralPath $sourceFull -Force
    Assert-CombinedCondition (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context source file is a reparse point: $sourceFull"
    $key = $DestinationRelativePath.Replace('/', '\').ToLowerInvariant()
    if ($Copied.ContainsKey($key)) {
        Assert-CombinedCondition ([string]$Copied[$key] -ceq $ExpectedSha256.ToUpperInvariant()) `
            "$Context aliases a staged path with a different SHA-256."
        return $DestinationRelativePath.Replace('/', '\')
    }
    Assert-CombinedCondition (-not (Test-Path -LiteralPath $destinationFull)) `
        "$Context destination already exists: $destinationFull"
    if ($null -eq $SourceSnapshot) {
        $SourceSnapshot = Get-CombinedFileSnapshot $sourceFull $Context
    }
    else {
        Assert-CombinedCondition ($SourceSnapshot.PSObject.Properties.Name -contains 'path' -and
            [String]::Equals([IO.Path]::GetFullPath([string]$SourceSnapshot.path),
                $sourceFull, [StringComparison]::OrdinalIgnoreCase) -and
            $SourceSnapshot.PSObject.Properties.Name -contains 'bytes' -and
            $SourceSnapshot.PSObject.Properties.Name -contains 'sha256') `
            "$Context source snapshot is bound to a different path."
    }
    $sourceHash = [string]$SourceSnapshot.sha256
    Assert-CombinedCondition ($sourceHash -ceq $ExpectedSha256.ToUpperInvariant()) `
        "$Context source SHA-256 changed during staging."
    $destinationDirectory = Split-Path -Parent $destinationFull
    Assert-CombinedNoReparsePath $destinationDirectory "$Context destination directory"
    if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Assert-CombinedNoReparsePath $destinationDirectory "$Context destination directory"
    # CreateNew makes a same-path race fail instead of following a file
    # reparse point installed after the preflight.  The full component walk is
    # repeated after the copy as an independent destination-identity check.
    $destinationStream = New-Object IO.FileStream($destinationFull,
        [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $destinationBytes = [byte[]]$SourceSnapshot.bytes
        $destinationStream.Write($destinationBytes, 0, $destinationBytes.Length)
        $destinationStream.Flush()
    }
    finally { $destinationStream.Dispose() }
    Assert-CombinedNoReparsePath $destinationFull "$Context destination"
    $destinationItem = Get-Item -LiteralPath $destinationFull -Force
    Assert-CombinedCondition (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context destination became a reparse point: $destinationFull"
    Assert-CombinedCondition ((Get-Stage5FileSha256 $destinationFull) -ceq $ExpectedSha256.ToUpperInvariant()) `
        "$Context staged destination SHA-256 does not match the consumed receipt."
    $Copied[$key] = $ExpectedSha256.ToUpperInvariant()
    return $DestinationRelativePath.Replace('/', '\')
}

function Get-CombinedSourceChild {
    param([object]$ReadResult, [string]$Title, [string]$Context)
    $provenance = $ReadResult.provenance
    Assert-Stage5JsonShape $provenance @('kind', 'runner', 'runnerVersion',
        'childProvenance', 'children') "$Context provenance"
    Assert-CombinedCondition ([string]$provenance.kind -ceq 'host-runner-observation' -and
        [string]$provenance.childProvenance -ceq 'bound') `
        "$Context is missing retained child provenance."
    $children = @($provenance.children)
    Assert-CombinedCondition ($children.Count -eq 1) `
        "$Context must contain exactly one concrete title child."
    $child = $children[0]
    Assert-Stage5JsonShape $child @('role', 'title', 'runNonce', 'processId',
        'processCreationUtc', 'executablePath', 'executableSha256',
        'commandLine', 'exitCode', 'stdout', 'stderr', 'nativeReceipt') "$Context child"
    Assert-CombinedCondition ([string]$child.title -ceq $Title -and
        [string]$child.role -ceq 'validation-results' -and
        [string]$child.runNonce -ceq [string]$ReadResult.runNonce) `
        "$Context child is not the retained validation-results process for $Title."
    return $child
}

function Get-CombinedChildStream {
    param([object]$Child, [string]$Name, [string]$Context)
    $stream = Get-Stage5JsonValue $Child $Name "$Context child"
    Assert-Stage5JsonShape $stream @('path', 'sha256') "$Context child $Name"
    return $stream
}

$sourceCommit = $ExpectedSourceCommit.ToLowerInvariant()
Assert-CombinedCondition ($sourceCommit -cmatch '^[0-9a-f]{40}$') `
    'ExpectedSourceCommit must be a lowercase 40-hex commit.'
Assert-CombinedCondition ($ExpectedArtifactSetSha256 -match '^[0-9A-Fa-f]{64}$') `
    'ExpectedArtifactSetSha256 must contain exactly 64 hexadecimal characters.'
Assert-CombinedCondition ($ExpectedGeneralsExecutableSha256 -match '^[0-9A-Fa-f]{64}$' -and
    $ExpectedZeroHourExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
    'Both expected executable SHA-256 values must be canonical.'
Assert-CombinedCondition ($ExpectedCohortNonce -match
    '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
    'ExpectedCohortNonce must be a canonical UUID minted before the title runs.'
[DateTimeOffset]$expectedCohortCreated = [DateTimeOffset]::MinValue
Assert-CombinedCondition ([DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
    [ref]$expectedCohortCreated)) `
    'ExpectedCohortCreatedUtc must be a valid execution-cohort timestamp.'

$generalsFull = [IO.Path]::GetFullPath($GeneralsReceiptPath)
$zeroHourFull = [IO.Path]::GetFullPath($ZeroHourReceiptPath)
$outputFull = [IO.Path]::GetFullPath($OutputPath)
Assert-CombinedCondition (-not [String]::Equals($generalsFull, $zeroHourFull,
    [StringComparison]::OrdinalIgnoreCase)) `
    'Generals and Zero Hour source receipts must be different files.'
Assert-CombinedCondition (-not [String]::Equals($outputFull, $generalsFull,
    [StringComparison]::OrdinalIgnoreCase) -and
    -not [String]::Equals($outputFull, $zeroHourFull,
        [StringComparison]::OrdinalIgnoreCase)) `
    'Combined output must not overwrite a source receipt.'
Assert-CombinedCondition (-not (Test-Path -LiteralPath $outputFull)) `
    "Combined output already exists; refusing to overwrite evidence: $outputFull"

$artifactHashes = @{
    'generals-executable' = $ExpectedGeneralsExecutableSha256.ToUpperInvariant()
    'zerohour-executable' = $ExpectedZeroHourExecutableSha256.ToUpperInvariant()
}
$sourceSeenRunNonces = @{}
$generalsSnapshot = Get-CombinedFileSnapshot $generalsFull 'Generals source receipt'
$zeroHourSnapshot = Get-CombinedFileSnapshot $zeroHourFull 'Zero Hour source receipt'
$generalsHash = [string]$generalsSnapshot.sha256
$zeroHourHash = [string]$zeroHourSnapshot.sha256
$generalsRead = Read-Stage5FinalAcceptanceImmutableReceipt `
    -Path $generalsFull -Kind 'deterministic-runtime' -Role 'validation-results' `
    -EvidenceTitle 'Generals' -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -SeenRunNonces $sourceSeenRunNonces `
    -ExpectedEvidenceSha256 $generalsHash -EvidenceSnapshot $generalsSnapshot `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc
$sourceCohortNonce = [string]$generalsRead.cohortNonce
$sourceRuntimeClosure = $generalsRead.runtimeClosure
$zeroHourRead = Read-Stage5FinalAcceptanceImmutableReceipt `
    -Path $zeroHourFull -Kind 'deterministic-runtime' -Role 'validation-results' `
    -EvidenceTitle 'ZeroHour' -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -SeenRunNonces $sourceSeenRunNonces `
    -ExpectedEvidenceSha256 $zeroHourHash -EvidenceSnapshot $zeroHourSnapshot `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
    -ExpectedRuntimeClosure $sourceRuntimeClosure
foreach ($source in @(
    [pscustomobject]@{ title = 'Generals'; read = $generalsRead; hash = $generalsHash },
    [pscustomobject]@{ title = 'ZeroHour'; read = $zeroHourRead; hash = $zeroHourHash }
)) {
    Assert-CombinedCondition ([string]$source.read.trustDomain -ceq 'host-runner' -and
        [string]$source.read.producer -ceq 'installed-runtime-validation-results-v2') `
        "$($source.title) source receipt is not the allowlisted host-runner v2 producer."
    Assert-CombinedCondition ([string]$source.read.runNonce -match
        '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
        "$($source.title) source receipt nonce is not canonical."
}
Assert-CombinedCondition ([string]$generalsRead.runNonce -cne [string]$zeroHourRead.runNonce) `
    'Generals and Zero Hour source receipts must have distinct run nonces.'
Assert-CombinedCondition ([string]$zeroHourRead.cohortNonce -ceq $sourceCohortNonce) `
    'Generals and Zero Hour source receipts must share the same execution cohort.'
Assert-CombinedCondition ([string]$zeroHourRead.runtimeClosure.dependencyManifestSha256 -ceq
    [string]$sourceRuntimeClosure.dependencyManifestSha256 -and
    [string]$zeroHourRead.runtimeClosure.closureSha256 -ceq
    [string]$sourceRuntimeClosure.closureSha256) `
    'Generals and Zero Hour source receipts must share the same runtime closure.'
$generalsChild = Get-CombinedSourceChild $generalsRead 'Generals' 'Generals source receipt'
$zeroHourChild = Get-CombinedSourceChild $zeroHourRead 'ZeroHour' 'Zero Hour source receipt'
Assert-CombinedCondition (-not ([string]$generalsChild.processId -eq [string]$zeroHourChild.processId -and
    [string]$generalsChild.processCreationUtc -ceq [string]$zeroHourChild.processCreationUtc)) `
    'Generals and Zero Hour source receipts must identify distinct child processes.'

$outputDirectory = Split-Path -Parent $outputFull
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$outputDirectoryItem = Get-Item -LiteralPath $outputDirectory -Force
Assert-CombinedCondition (($outputDirectoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
    'Combined output directory must not be a reparse point.'
$sourceStageRoot = Join-Path $outputDirectory 'sources'
Assert-CombinedCondition (-not (Test-Path -LiteralPath $sourceStageRoot)) `
    "Combined source staging directory already exists: $sourceStageRoot"
New-Item -ItemType Directory -Path $sourceStageRoot | Out-Null
$generalsStageRoot = Join-Path $sourceStageRoot 'Generals'
$zeroHourStageRoot = Join-Path $sourceStageRoot 'ZeroHour'
New-Item -ItemType Directory -Path $generalsStageRoot, $zeroHourStageRoot | Out-Null

$generalsSourceBase = Split-Path -Parent $generalsFull
$zeroHourSourceBase = Split-Path -Parent $zeroHourFull
$generalsCopied = @{}
$zeroHourCopied = @{}
$generalsPathMap = @{}
$zeroHourPathMap = @{}
foreach ($source in @(
    [pscustomobject]@{ title = 'Generals'; read = $generalsRead; child = $generalsChild
        sourceBase = $generalsSourceBase; stageRoot = $generalsStageRoot
        copied = $generalsCopied; map = $generalsPathMap },
    [pscustomobject]@{ title = 'ZeroHour'; read = $zeroHourRead; child = $zeroHourChild
        sourceBase = $zeroHourSourceBase; stageRoot = $zeroHourStageRoot
        copied = $zeroHourCopied; map = $zeroHourPathMap }
)) {
    foreach ($raw in @($source.read.rawLogs)) {
        $rawPath = [string]$raw.path
        $rawHash = [string]$raw.sha256
        $destinationRelative = Join-Path ('sources\' + $source.title) $rawPath
        $rawSnapshot = $null
        if (@($raw.PSObject.Properties.Name | Where-Object { [string]$_ -ceq 'snapshot' }).Count -gt 0) {
            $rawSnapshot = $raw.snapshot
        }
        $source.map[$rawPath.Replace('/', '\').ToLowerInvariant()] =
            (Copy-CombinedSourceFile $source.sourceBase $rawPath $outputDirectory `
                $destinationRelative $rawHash "$($source.title) source raw log" $source.copied `
                -SourceSnapshot $rawSnapshot)
    }
    foreach ($streamName in @('stdout', 'stderr')) {
        $stream = Get-CombinedChildStream $source.child $streamName "$($source.title) source"
        $streamPath = [string]$stream.path
        $destinationRelative = Join-Path ('sources\' + $source.title) $streamPath
        $streamSnapshot = $null
        foreach ($raw in @($source.read.rawLogs)) {
            if ([string]$raw.path -ceq $streamPath -and
                @($raw.PSObject.Properties.Name | Where-Object { [string]$_ -ceq 'snapshot' }).Count -gt 0) {
                $streamSnapshot = $raw.snapshot
                break
            }
        }
        $source.map[$streamPath.Replace('/', '\').ToLowerInvariant()] =
            (Copy-CombinedSourceFile $source.sourceBase $streamPath $outputDirectory `
                $destinationRelative ([string]$stream.sha256) "$($source.title) child $streamName" $source.copied `
                -SourceSnapshot $streamSnapshot)
    }
    if (@($source.child.Keys | Where-Object { [string]$_ -ceq 'nativeReceipt' }).Count -gt 0) {
        $native = Get-Stage5JsonValue $source.child 'nativeReceipt' "$($source.title) source child"
        Assert-Stage5JsonShape $native @('path', 'sha256', 'producer', 'runNonce',
            'cohortNonce') `
            "$($source.title) source child native receipt"
        $nativePath = [string]$native.path
        $nativeSourceFull = [IO.Path]::GetFullPath((Join-Path $source.sourceBase $nativePath))
        $nativeSnapshot = Get-CombinedFileSnapshot $nativeSourceFull `
            "$($source.title) source child native receipt"
        $nativeDocument = ConvertFrom-CombinedJsonSnapshot $nativeSnapshot `
            "$($source.title) source child native receipt"
        Assert-CombinedCondition ($nativeDocument -is [Collections.IDictionary]) `
            "$($source.title) source child native receipt is not a JSON object."
        $nativeRawLogs = $nativeDocument['rawLogs']
        Assert-CombinedCondition ($nativeRawLogs -is [Array] -and
            $nativeRawLogs.Count -eq 2) `
            "$($source.title) source child native receipt must carry raw-log and timing evidence."
        $nativeSourceDirectory = Split-Path -Parent $nativeSourceFull
        foreach ($nativeRaw in $nativeRawLogs) {
            Assert-Stage5JsonShape $nativeRaw @('name', 'path', 'sha256') `
                "$($source.title) source child native raw evidence"
            $nativeRawPath = [string](Get-Stage5JsonValue $nativeRaw 'path' `
                "$($source.title) source child native raw evidence")
            $nativeRawHash = [string](Get-Stage5JsonValue $nativeRaw 'sha256' `
                "$($source.title) source child native raw evidence")
            # The native receipt is copied byte-for-byte, so its relative raw
            # paths must remain valid beside the staged native receipt.  An
            # absolute native path cannot be safely remapped without changing
            # the hash-bound native receipt and is therefore fail-closed.
            Assert-CombinedRelativePath $nativeRawPath `
                "$($source.title) source child native raw evidence"
            $nativeRawDestination = Join-Path ('sources\' + $source.title) $nativeRawPath
            Copy-CombinedSourceFile $nativeSourceDirectory $nativeRawPath `
                $outputDirectory $nativeRawDestination $nativeRawHash `
                "$($source.title) source child native raw evidence" $source.copied | Out-Null
        }
        $destinationRelative = Join-Path ('sources\' + $source.title) $nativePath
        $source.map[$nativePath.Replace('/', '\').ToLowerInvariant()] =
            (Copy-CombinedSourceFile $source.sourceBase $nativePath $outputDirectory `
                $destinationRelative ([string]$native.sha256) "$($source.title) child native receipt" $source.copied)
    }
}

$generalsReceiptRelative = 'sources\Generals\source-validation-results.json'
$zeroHourReceiptRelative = 'sources\ZeroHour\source-validation-results.json'
Copy-CombinedSourceFile $generalsSourceBase ([IO.Path]::GetFileName($generalsFull)) `
    $outputDirectory $generalsReceiptRelative $generalsHash `
    'Generals source receipt' $generalsCopied -SourceSnapshot $generalsSnapshot | Out-Null
Copy-CombinedSourceFile $zeroHourSourceBase ([IO.Path]::GetFileName($zeroHourFull)) `
    $outputDirectory $zeroHourReceiptRelative $zeroHourHash `
    'Zero Hour source receipt' $zeroHourCopied -SourceSnapshot $zeroHourSnapshot | Out-Null
Assert-CombinedCondition ((Get-Stage5FileSha256 (Join-Path $outputDirectory $generalsReceiptRelative)) -ceq
    $generalsHash) 'Staged Generals source receipt hash changed.'
Assert-CombinedCondition ((Get-Stage5FileSha256 (Join-Path $outputDirectory $zeroHourReceiptRelative)) -ceq
    $zeroHourHash) 'Staged Zero Hour source receipt hash changed.'

function New-CombinedChild {
    param(
        [object]$Child,
        [string]$Title,
        [string]$SourceBase,
        [string]$OutputDirectory,
        [hashtable]$PathMap,
        [hashtable]$Copied
    )
    $combined = [ordered]@{
        role = 'combined-results'
        title = $Title
        runNonce = [string]$Child.runNonce
        processId = [int]$Child.processId
        processCreationUtc = [string]$Child.processCreationUtc
        executablePath = [string]$Child.executablePath
        executableSha256 = [string]$Child.executableSha256
        commandLine = [string]$Child.commandLine
        exitCode = [int]$Child.exitCode
    }
    foreach ($streamName in @('stdout', 'stderr')) {
        $stream = Get-CombinedChildStream $Child $streamName "$Title source"
        $streamPath = [string]$stream.path
        $key = $streamPath.Replace('/', '\').ToLowerInvariant()
        Assert-CombinedCondition $PathMap.ContainsKey($key) `
            "$Title child $streamName was not staged from its source receipt."
        $combined[$streamName] = [ordered]@{
            path = [string]$PathMap[$key]
            sha256 = [string]$stream.sha256
        }
    }
    if (@($Child.Keys | Where-Object { [string]$_ -ceq 'nativeReceipt' }).Count -gt 0) {
        $native = Get-Stage5JsonValue $Child 'nativeReceipt' "$Title source child"
        $nativePath = [string]$native.path
        $key = $nativePath.Replace('/', '\').ToLowerInvariant()
        Assert-CombinedCondition $PathMap.ContainsKey($key) `
            "$Title native receipt was not staged from its source receipt."
        $combined.nativeReceipt = [ordered]@{
            path = [string]$PathMap[$key]
            sha256 = [string]$native.sha256
            producer = [string]$native.producer
            runNonce = [string]$native.runNonce
            cohortNonce = [string]$native.cohortNonce
        }
    }
    return $combined
}

$combinedGeneralsChild = New-CombinedChild $generalsChild 'Generals' `
    $generalsSourceBase $outputDirectory $generalsPathMap $generalsCopied
$combinedZeroHourChild = New-CombinedChild $zeroHourChild 'ZeroHour' `
    $zeroHourSourceBase $outputDirectory $zeroHourPathMap $zeroHourCopied
$combinedRunNonce = [Guid]::NewGuid().ToString()
while ($combinedRunNonce -ceq [string]$generalsRead.runNonce -or
    $combinedRunNonce -ceq [string]$zeroHourRead.runNonce) {
    $combinedRunNonce = [Guid]::NewGuid().ToString()
}

$combinedRawRelative = 'combined-results.sources.raw.log'
$combinedRawFull = Join-Path $outputDirectory $combinedRawRelative
Assert-CombinedCondition (-not (Test-Path -LiteralPath $combinedRawFull)) `
    "Combined raw binding log already exists: $combinedRawFull"
$combinedRaw = @(
    'STAGE5_COMBINED_HOST_RUNNER_V2'
    "sourceCommit=$sourceCommit"
    "artifactSetSha256=$($ExpectedArtifactSetSha256.ToUpperInvariant())"
    "GeneralsReceiptSha256=$generalsHash"
    "GeneralsRunNonce=$($generalsRead.runNonce)"
    "ZeroHourReceiptSha256=$zeroHourHash"
    "ZeroHourRunNonce=$($zeroHourRead.runNonce)"
) -join "`n"
Assert-CombinedNoReparsePath $outputDirectory 'Combined output directory before raw binding'
[IO.File]::WriteAllText($combinedRawFull, $combinedRaw + "`n", (New-Object Text.UTF8Encoding($false)))
Assert-CombinedNoReparsePath $combinedRawFull 'Combined raw binding log after creation'
$combinedRawHash = Get-Stage5FileSha256 $combinedRawFull

$combinedDocument = [ordered]@{
    schemaVersion = 1
    evidenceKind = 'stage5-host-runner-receipt'
    status = 'passed'
    role = 'combined-results'
    trustDomain = 'host-runner'
    producer = 'installed-runtime-combined-results-v2'
    producerVersion = '2'
    runNonce = $combinedRunNonce
    sourceCommit = $sourceCommit
    title = 'Both'
    architecture = 'x64'
    artifactSetSha256 = $ExpectedArtifactSetSha256.ToUpperInvariant()
    cohortNonce = $sourceCohortNonce
    runtimeClosure = [ordered]@{
        dependencyManifestSha256 = [string]$sourceRuntimeClosure.dependencyManifestSha256
        closureSha256 = [string]$sourceRuntimeClosure.closureSha256
    }
    executableSha256 = [ordered]@{
        Generals = $ExpectedGeneralsExecutableSha256.ToUpperInvariant()
        ZeroHour = $ExpectedZeroHourExecutableSha256.ToUpperInvariant()
    }
    recordedUtc = [DateTime]::UtcNow.ToString('o')
    rawLogs = @(
        [ordered]@{ name = 'Generals-source-receipt'; path = $generalsReceiptRelative; sha256 = $generalsHash }
        [ordered]@{ name = 'ZeroHour-source-receipt'; path = $zeroHourReceiptRelative; sha256 = $zeroHourHash }
        [ordered]@{ name = 'combined-source-bindings'; path = $combinedRawRelative; sha256 = $combinedRawHash }
    )
    provenance = [ordered]@{
        kind = 'host-runner-observation'
        runner = 'New-Stage5CombinedHostRunnerReceipt.ps1'
        runnerVersion = '1'
        childProvenance = 'bound'
        children = @($combinedGeneralsChild, $combinedZeroHourChild)
    }
    details = [ordered]@{
        pipelineMode = 'parallel'
        simulationMode = 'parallel'
        workerPolicy = 'auto'
        renderer = 'd3d11'
        renderThread = 'dedicated'
        bothTitlesPassed = $true
        sourceReceipts = @(
            [ordered]@{ title = 'Generals'; path = $generalsReceiptRelative
                sha256 = $generalsHash; runNonce = [string]$generalsRead.runNonce
                cohortNonce = [string]$generalsRead.cohortNonce }
            [ordered]@{ title = 'ZeroHour'; path = $zeroHourReceiptRelative
                sha256 = $zeroHourHash; runNonce = [string]$zeroHourRead.runNonce
                cohortNonce = [string]$zeroHourRead.cohortNonce }
        )
    }
}
Assert-CombinedNoReparsePath $outputDirectory 'Combined output directory before receipt creation'
[IO.File]::WriteAllText($outputFull, ($combinedDocument | ConvertTo-Json -Depth 16),
    (New-Object Text.UTF8Encoding($false)))
Assert-CombinedNoReparsePath $outputFull 'Combined receipt after creation'
$combinedHash = Get-Stage5FileSha256 $outputFull
$combinedSeenRunNonces = @{}
$combinedRead = Read-Stage5FinalAcceptanceImmutableReceipt `
    -Path $outputFull -Kind 'combined-stage4-stage5-installed-runtime' `
    -Role 'combined-results' -EvidenceTitle 'Both' `
    -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -SeenRunNonces $combinedSeenRunNonces `
    -ExpectedEvidenceSha256 $combinedHash `
    -ExpectedCohortNonce $sourceCohortNonce `
    -ExpectedRuntimeClosure $sourceRuntimeClosure
Assert-CombinedCondition ([string]$combinedRead.trustDomain -ceq 'host-runner' -and
    [string]$combinedRead.producer -ceq 'installed-runtime-combined-results-v2') `
    'Combined producer self-validation did not return the allowlisted host-runner v2 receipt.'
Write-Output "Generated combined-results v2 host receipt for $sourceCommit with distinct Generals/ZeroHour source nonces."
