[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GeneralsReceiptPath,
    [Parameter(Mandatory = $true)][string]$ZeroHourReceiptPath,
    [Parameter(Mandatory = $true)][string]$GeneralsReviewedFixtureReceiptPath,
    [Parameter(Mandatory = $true)][string]$GeneralsReviewedFixtureReceiptSha256,
    [Parameter(Mandatory = $true)][string]$ZeroHourReviewedFixtureReceiptPath,
    [Parameter(Mandatory = $true)][string]$ZeroHourReviewedFixtureReceiptSha256,
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

$combinedPhaseStopwatch = [Diagnostics.Stopwatch]::StartNew()
Write-Verbose 'STAGE5_COMBINED_PHASE phase=module-import state=start context= index=0 total=0 elapsedMs=0'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
if ($PSBoundParameters.ContainsKey('Verbose')) {
    $stage5EvidenceModule = Get-Module DeterministicSimulationEvidence
    & $stage5EvidenceModule { $VerbosePreference = 'Continue' }
}

function Write-CombinedPhaseTiming {
    param(
        [string]$Phase,
        [ValidateSet('start', 'progress', 'complete')][string]$State,
        [string]$Context = '',
        [int]$Index = 0,
        [int]$Total = 0
    )
    Write-Verbose (('STAGE5_COMBINED_PHASE phase={0} state={1} context={2} ' +
        'index={3} total={4} elapsedMs={5}') -f $Phase, $State, $Context,
        $Index, $Total, $combinedPhaseStopwatch.ElapsedMilliseconds)
}
Write-CombinedPhaseTiming 'module-import' 'complete'

function Assert-CombinedCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-CombinedRelativePath {
    param([string]$Path, [string]$Context)
    Assert-CombinedCondition (-not [string]::IsNullOrWhiteSpace($Path) -and
        -not [IO.Path]::IsPathRooted($Path) -and
        $Path -notmatch '^[A-Za-z]:' -and
        $Path -notmatch '(^|[\\/])\.\.([\\/]|$)' -and
        $Path -notmatch ':') `
        "$Context must be a non-rooted path without parent traversal, drive-relative syntax, or ADS."
}

function Assert-CombinedNativeRawPathText {
    param([string]$Path, [string]$Context)
    Assert-CombinedCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is empty."
    Assert-CombinedCondition ($Path -notmatch '(^|[\\/])\.\.([\\/]|$)') `
        "$Context path contains parent traversal."
    if ([IO.Path]::IsPathRooted($Path)) {
        # A rooted Windows path may contain the one colon that separates its
        # drive.  Any later colon is an alternate data stream separator.
        $afterDrive = if ($Path -match '^[A-Za-z]:') { $Path.Substring(2) } else { $Path }
        Assert-CombinedCondition ($Path -notmatch '^[A-Za-z]:[^\\/]' -and
            $afterDrive -notmatch ':') `
            "$Context path is drive-relative or names an alternate data stream."
    }
    else {
        Assert-CombinedRelativePath $Path $Context
    }
}

function Get-CombinedPathParts {
    param([string]$Path, [string]$Context)
    try { $full = [IO.Path]::GetFullPath($Path) }
    catch { throw "$Context path could not be canonicalized: $($_.Exception.Message)" }
    $root = [IO.Path]::GetPathRoot($full)
    Assert-CombinedCondition ($root -is [string] -and -not [string]::IsNullOrWhiteSpace($root)) `
        "$Context path does not have a valid volume root: $full"
    return @($full.Substring($root.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Get-CombinedRelativePathFromBase {
    param([string]$BaseDirectory, [string]$CandidatePath, [string]$Context)
    $base = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd([char[]]@('\', '/'))
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-CombinedCondition ($baseRoot -is [string] -and $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."
    $baseParts = @(Get-CombinedPathParts $base $Context)
    $candidateParts = @(Get-CombinedPathParts $candidate $Context)
    Assert-CombinedCondition ($candidateParts.Count -gt $baseParts.Count) `
        "$Context path must identify a file below its source directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-CombinedCondition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its source directory."
    }
    $relative = $candidate.Substring($base.Length).TrimStart([char[]]@('\', '/'))
    Assert-CombinedRelativePath $relative $Context
    return $relative.Replace('/', '\')
}

function Get-CombinedRegularFilesNoReparse {
    param([string]$RootDirectory, [string]$Context)
    $root = [IO.Path]::GetFullPath($RootDirectory)
    Assert-CombinedNoReparsePath $root $Context
    $pending = New-Object 'Collections.Generic.Stack[string]'
    $pending.Push($root)
    [int]$visitedEntryCount = 0
    [int]$emittedFileCount = 0
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)) {
            ++$visitedEntryCount
            Assert-CombinedCondition ($visitedEntryCount -le 16384) `
                "$Context exceeds the bounded 16384-entry source search."
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                continue
            }
            if ($item -is [IO.DirectoryInfo]) {
                Assert-CombinedCondition ($pending.Count -lt 4096) `
                    "$Context exceeds the bounded 4096-directory source search."
                $pending.Push([IO.Path]::GetFullPath($item.FullName))
            }
            elseif ($item -is [IO.FileInfo]) {
                ++$emittedFileCount
                Assert-CombinedCondition ($emittedFileCount -le 8192) `
                    "$Context exceeds the bounded 8192-file source search."
                Write-Output ([IO.Path]::GetFullPath($item.FullName))
            }
        }
    }
}

function Resolve-CombinedNativeRawSourcePath {
    param(
        [string]$SourceBase,
        [string]$NativeReceiptDirectory,
        [string]$RawPath,
        [string]$ExpectedSha256,
        [string]$Context
    )
    Assert-CombinedNativeRawPathText $RawPath $Context
    Assert-CombinedCondition ($ExpectedSha256 -match '^[0-9A-Fa-f]{64}$') `
        "$Context expected SHA-256 is not canonical."
    $sourceRoot = [IO.Path]::GetFullPath($SourceBase)
    $candidatePaths = New-Object 'Collections.Generic.List[string]'
    if ([IO.Path]::IsPathRooted($RawPath)) {
        try { $candidate = [IO.Path]::GetFullPath($RawPath) }
        catch { throw "$Context path could not be canonicalized: $($_.Exception.Message)" }
        try {
            $relative = Get-CombinedRelativePathFromBase $sourceRoot $candidate $Context
            [void]$candidatePaths.Add($candidate)
        }
        catch {
            # The native executable recorded its original absolute output
            # root.  That root is intentionally not trusted on the combined
            # runner; a suffix rebinding below finds the immutable artifact
            # downloaded under the current source root instead.
        }
    }
    else {
        foreach ($base in @($NativeReceiptDirectory, $sourceRoot)) {
            try {
                $candidate = [IO.Path]::GetFullPath((Join-Path $base $RawPath))
                [void](Get-CombinedRelativePathFromBase $sourceRoot $candidate $Context)
                [void]$candidatePaths.Add($candidate)
            }
            catch { }
        }
    }
    foreach ($candidate in @($candidatePaths | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            Assert-CombinedNoReparsePath $candidate $Context
            return $candidate
        }
    }

    $rawParts = @(Get-CombinedPathParts $RawPath $Context)
    $files = @(Get-CombinedRegularFilesNoReparse $sourceRoot "$Context source root")
    $matches = New-Object 'Collections.Generic.List[string]'
    $maxSuffix = $rawParts.Count
    foreach ($file in $files) {
        $relative = Get-CombinedRelativePathFromBase $sourceRoot $file $Context
        $fileParts = @($relative -split '[\\/]')
        $limit = [Math]::Min($maxSuffix, $fileParts.Count)
        for ($length = $limit; $length -ge 1; --$length) {
            $equal = $true
            for ($offset = 1; $offset -le $length; ++$offset) {
                if (-not $fileParts[$fileParts.Count - $offset].Equals(
                    $rawParts[$rawParts.Count - $offset],
                    [StringComparison]::OrdinalIgnoreCase)) {
                    $equal = $false
                    break
                }
            }
            if ($equal) {
                [void]$matches.Add($file)
                break
            }
        }
    }
    $uniqueMatches = @($matches | Select-Object -Unique)
    Assert-CombinedCondition ($uniqueMatches.Count -eq 1) `
        "$Context absolute path could not be rebound uniquely beneath the downloaded source artifact."
    $resolved = [IO.Path]::GetFullPath([string]$uniqueMatches[0])
    Assert-CombinedNoReparsePath $resolved $Context
    Assert-CombinedCondition ((Get-Stage5FileSha256 $resolved) -ceq $ExpectedSha256.ToUpperInvariant()) `
        "$Context rebound source SHA-256 does not match the native receipt."
    return $resolved
}

function Get-CombinedNativeClosureInfo {
    param(
        [string]$SourceReceiptPath,
        [string]$SourceBase,
        [string]$Title,
        [string]$Context
    )
    $relocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
        -Path $SourceReceiptPath -EvidenceDirectory $SourceBase
    $children = @($relocation.children)
    Assert-CombinedCondition ($children.Count -eq 253) `
        "$Context must bind the complete 253-child native execution closure."
    $seenSequences = New-Object 'Collections.Generic.HashSet[int]'
    $seenNonces = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    foreach ($child in $children) {
        Assert-Stage5JsonShape $child @('sequence', 'runNonce', 'evidenceDirectory',
            'nativeRawBindings', 'nativeReceiptSourcePath') `
            "$Context native relocation child"
        $sequence = [int]$child.sequence
        Assert-CombinedCondition ($sequence -ge 1 -and $sequence -le 253 -and
            $seenSequences.Add($sequence) -and
            $seenNonces.Add([string]$child.runNonce) -and
            @($child.nativeRawBindings).Count -eq 2) `
            "$Context native relocation closure is incomplete, duplicated, or out of order."
    }
    Assert-CombinedCondition ($seenSequences.Count -eq 253 -and
        @((1..253 | Where-Object {
            -not $seenSequences.Contains($_)
        })).Count -eq 0) `
        "$Context native relocation closure does not cover every sequence from 1 through 253."
    return $relocation
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
    param(
        [string]$Path,
        [string]$Context,
        [ValidateSet('JsonReceipt', 'Replay', 'Trace', 'RawLog', 'RuntimeBinary')]
        [string]$EvidenceKind = 'JsonReceipt'
    )
    return Get-Stage5FinalAcceptanceFileSnapshot $Path $Context `
        -EvidenceKind $EvidenceKind
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
        [object]$SourceSnapshot = $null,
        [ValidateSet('JsonReceipt', 'Replay', 'RawLog')]
        [string]$EvidenceKind = 'RawLog'
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
        $SourceSnapshot = Get-CombinedFileSnapshot $sourceFull $Context `
            -EvidenceKind $EvidenceKind
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
    $destinationBytes = [byte[]]$SourceSnapshot.bytes
    $destinationSnapshot = Write-Stage5FinalAcceptanceFileAtomically `
        $destinationFull $destinationBytes "$Context destination" `
        -EvidenceKind $EvidenceKind
    Assert-CombinedNoReparsePath $destinationFull "$Context destination"
    $destinationItem = Get-Item -LiteralPath $destinationFull -Force
    Assert-CombinedCondition (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context destination became a reparse point: $destinationFull"
    Assert-CombinedCondition ([string]$destinationSnapshot.sha256 -ceq
        $ExpectedSha256.ToUpperInvariant()) `
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
    Assert-CombinedCondition ($children.Count -eq 253) `
        "$Context must contain the complete 253-child deterministic-runtime corpus."
    $selected = @($children | Where-Object {
        [int]$_.sequence -eq 1 -and [string]$_.title -ceq $Title
    })
    Assert-CombinedCondition ($selected.Count -eq 1) `
        "$Context must contain exactly one canonical sequence-1 child for $Title."
    $child = $selected[0]
    Assert-Stage5JsonShape $child @('sequence', 'role', 'title', 'runNonce', 'processId',
        'processCreationUtc', 'executablePath', 'executableSha256',
        'commandLine', 'arguments', 'exitCode', 'qualificationData', 'stdout',
        'stderr', 'nativeReceipt') `
        "$Context selected child"
    $native = Get-Stage5JsonValue $child 'nativeReceipt' "$Context child"
    Assert-Stage5JsonShape $native @('path', 'sha256', 'producer', 'runNonce',
        'cohortNonce') "$Context child native receipt"
    Assert-CombinedCondition ([int]$child.sequence -eq 1 -and
        [string]$child.title -ceq $Title -and
        [string]$child.role -ceq 'validation-results' -and
        [string]$child.runNonce -cne [string]$ReadResult.runNonce -and
        [string]$native.runNonce -ceq [string]$child.runNonce) `
        "$Context must keep the wrapper nonce distinct while binding the retained validation-results child to its native receipt for $Title."
    return $child
}

function Get-CombinedSourceRelocation {
    param([object]$Closure, [object]$Child, [string]$Context)
    $matches = @($Closure.children | Where-Object {
        [int]$_.sequence -eq [int]$Child.sequence -and
        [string]$_.runNonce -ceq [string]$Child.runNonce
    })
    Assert-CombinedCondition ($matches.Count -eq 1) `
        "$Context must have one exact native relocation for the selected source child."
    return $matches[0]
}

function Get-CombinedProjectionPolicy {
    param([object]$Child, [string]$Context)
    $arguments = Get-Stage5JsonValue $Child 'arguments' "$Context selected child"
    Assert-CombinedCondition ($arguments -is [Array] -and $arguments.Count -gt 0 -and
        @($arguments | Where-Object { $_ -isnot [string] }).Count -eq 0) `
        "$Context selected child arguments are not a non-empty string array."
    $values = @{}
    foreach ($option in @('-pipelineMode', '-simulationMode', '-workerPolicy')) {
        $indices = @(for ($index = 0; $index -lt $arguments.Count; ++$index) {
            if ([string]$arguments[$index] -ceq $option) { $index }
        })
        Assert-CombinedCondition ($indices.Count -eq 1 -and
            $indices[0] + 1 -lt $arguments.Count -and
            $arguments[$indices[0] + 1] -is [string] -and
            -not [string]::IsNullOrWhiteSpace([string]$arguments[$indices[0] + 1])) `
            "$Context selected child must contain one exact '$option' value."
        $values[$option] = [string]$arguments[$indices[0] + 1]
    }
    $workerIndices = @(for ($index = 0; $index -lt $arguments.Count; ++$index) {
        if ([string]$arguments[$index] -ceq '-workerCount') { $index }
    })
    Assert-CombinedCondition ($workerIndices.Count -eq 1 -and
        $workerIndices[0] + 1 -lt $arguments.Count -and
        [string]$arguments[$workerIndices[0] + 1] -cmatch '^[1-9][0-9]*$') `
        "$Context selected child must contain one exact positive '-workerCount' value."
    $requestedWorkers = [string]$arguments[$workerIndices[0] + 1]
    Assert-CombinedCondition ($values['-pipelineMode'] -ceq 'serial' -and
        $values['-simulationMode'] -ceq 'serial' -and
        $values['-workerPolicy'] -ceq 'auto' -and
        $requestedWorkers -ceq '1') `
        "$Context sequence-1 lineage is not the expected serial/serial/auto/one-worker source execution."
    return [pscustomobject]@{
        pipelineMode = $values['-pipelineMode']
        simulationMode = $values['-simulationMode']
        requestedWorkers = $requestedWorkers
        workerPolicy = $values['-workerPolicy']
    }
}

function Resolve-CombinedSiblingReceipt {
    param([string]$ValidationReceiptPath, [string]$Leaf, [string]$Context)
    $validationFull = [IO.Path]::GetFullPath($ValidationReceiptPath)
    Assert-CombinedCondition ([IO.Path]::GetFileName($validationFull) -ceq
        'validation-results-receipt.json') `
        "$Context validation-results input must retain its canonical leaf name."
    $candidate = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $validationFull) $Leaf))
    Assert-CombinedCondition (Test-Path -LiteralPath $candidate -PathType Leaf) `
        "$Context required sibling receipt '$Leaf' is missing."
    Assert-CombinedNoReparsePath $candidate "$Context sibling receipt '$Leaf'"
    return $candidate
}

function Read-CombinedSupportingReceipt {
    param(
        [string]$Path,
        [ValidateSet('validation-plan', 'replay-results', 'ai-results')]
        [string]$Role,
        [string]$Title,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [Collections.IDictionary]$SeenRunNonces,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc,
        [object]$RuntimeClosure,
        [string]$Context
    )
    $snapshot = Get-CombinedFileSnapshot $Path "$Context $Role receipt"
    $kind = if ($Role -ceq 'replay-results') {
        'replay-determinism'
    }
    elseif ($Role -ceq 'ai-results') { 'fresh-ai' }
    else { 'deterministic-runtime' }
    $arguments = [ordered]@{
        Path = $Path; Kind = $kind; Role = $Role
        EvidenceTitle = $Title; ExpectedSourceCommit = $SourceCommit
        ExpectedArtifactSetSha256 = $ArtifactSetSha256
        ArtifactHashes = $ArtifactHashes; SeenRunNonces = $SeenRunNonces
        ExpectedEvidenceSha256 = [string]$snapshot.sha256
        EvidenceSnapshot = $snapshot; ExpectedCohortNonce = $CohortNonce
        ExpectedCohortCreatedUtc = $CohortCreatedUtc
        ExpectedRuntimeClosure = $RuntimeClosure
        ExpectedEvidenceDirectory = Split-Path -Parent $Path
    }
    $relocation = $null
    if ($Role -ne 'validation-plan') {
        $relocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
            -Path $Path -EvidenceDirectory (Split-Path -Parent $Path)
        Assert-CombinedCondition (@($relocation.children).Count -eq 1) `
            "$Context $Role receipt must retain exactly one native lineage child."
        $arguments['NativeRawBindings'] = $relocation.nativeRawBindings
        $arguments['NativeReceiptSourcePath'] = $relocation.nativeReceiptSourcePath
    }
    $read = Read-Stage5FinalAcceptanceImmutableReceipt @arguments
    Assert-CombinedCondition ([string]$read.trustDomain -ceq 'host-runner' -and
        [string]$read.producer -ceq "installed-runtime-$Role-v2") `
        "$Context $Role receipt is not the allowlisted host-runner v2 producer."
    return [pscustomobject]@{
        role = $Role; path = $Path; snapshot = $snapshot
        sha256 = [string]$snapshot.sha256; read = $read; relocation = $relocation
    }
}

function Read-CombinedReviewedFixtureReceipt {
    param(
        [string]$Path,
        [string]$ExpectedSha256,
        [string]$Title,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [object]$RuntimeClosure,
        [string]$Context
    )
    Assert-CombinedCondition ($ExpectedSha256 -cmatch '^[0-9A-F]{64}$') `
        "$Context reviewed receipt SHA-256 must be independently supplied as uppercase 64-hex."
    $full = [IO.Path]::GetFullPath($Path)
    Assert-CombinedNoReparsePath $full "$Context reviewed receipt"
    $snapshot = Get-CombinedFileSnapshot $full "$Context reviewed receipt"
    Assert-CombinedCondition ([string]$snapshot.sha256 -ceq $ExpectedSha256) `
        "$Context reviewed receipt does not match its independently supplied SHA-256."
    $read = Read-Stage5FinalAcceptanceImmutableReceipt `
        -Path $full -Kind 'replay-determinism' `
        -Role 'replay-fixture-manifest' -EvidenceTitle $Title `
        -ExpectedSourceCommit $SourceCommit `
        -ExpectedArtifactSetSha256 $ArtifactSetSha256 `
        -ArtifactHashes $ArtifactHashes `
        -ExpectedEvidenceSha256 $ExpectedSha256 -EvidenceSnapshot $snapshot `
        -ExpectedRuntimeClosure $RuntimeClosure
    Assert-CombinedCondition ([string]$read.trustDomain -ceq 'reviewed-fixture' -and
        [string]$read.producer -ceq 'reviewed-replay-fixture-manifest-v2' -and
        $null -ne $read.reviewedFixtureManifest -and
        $null -ne $read.reviewedFixtureManifestSnapshot) `
        "$Context is not the exact protected reviewed-fixture receipt contract."
    return [pscustomobject]@{
        path = $full
        snapshot = $snapshot
        sha256 = $ExpectedSha256
        read = $read
    }
}

function Get-CombinedQualificationDataBinding {
    param(
        [object]$Receipt,
        [string]$Title,
        [string]$Context
    )
    Assert-CombinedCondition ($null -ne $Receipt -and
        $Receipt.PSObject.Properties.Name -contains 'document') `
        "$Context receipt must be an immutable-read wrapper with a document."
    $receiptDocument = $Receipt.document
    Assert-CombinedCondition ($receiptDocument -is [Collections.IDictionary]) `
        "$Context receipt document must be a JSON object."
    $details = Get-Stage5JsonValue $receiptDocument 'details' "$Context receipt"
    $binding = Get-Stage5JsonValue $details 'qualificationData' `
        "$Context receipt details"
    Assert-Stage5JsonShape $binding @('path', 'title', 'manifestSha256',
        'closureSha256', 'fileCount') "$Context qualificationData"
    Assert-CombinedCondition (
        [string]$binding.path -ceq 'QualificationData.json' -and
        [string]$binding.title -ceq $Title -and
        [string]$binding.manifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        [string]$binding.closureSha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $binding.fileCount) -and
        [int64]$binding.fileCount -eq 6) `
        "$Context qualificationData is malformed, title-swapped, or incomplete."
    return [ordered]@{
        path = 'QualificationData.json'
        title = $Title
        manifestSha256 = [string]$binding.manifestSha256
        closureSha256 = [string]$binding.closureSha256
        fileCount = 6
    }
}

function Assert-CombinedQualificationDataBindingEqual {
    param(
        [Collections.IDictionary]$Expected,
        [Collections.IDictionary]$Actual,
        [string]$Context
    )
    foreach ($field in @('path', 'title', 'manifestSha256', 'closureSha256',
            'fileCount')) {
        Assert-CombinedCondition ([string]$Actual[$field] -ceq
            [string]$Expected[$field]) `
            "$Context qualificationData field '$field' differs from the title corpus."
    }
}

function Get-CombinedUtf8Sha256 {
    param([string]$Text)
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($Text)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '')
    }
    finally { $hasher.Dispose() }
}

function Copy-CombinedReviewedFixtureClosure {
    param(
        [string]$Title,
        [object]$Reviewed,
        [string]$OutputDirectory,
        [hashtable]$Copied
    )
    $context = "$Title protected reviewed-fixture closure"
    $sourceBase = Split-Path -Parent ([string]$Reviewed.path)
    $destinationBase = "sources\$Title\reviewed"
    $rows = New-Object 'Collections.Generic.List[string]'
    $paths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)

    function Add-ReviewedClosureRow {
        param([string]$RelativePath, [string]$Sha256)
        $normalized = $RelativePath.Replace('\', '/')
        Assert-CombinedRelativePath $normalized "$context closure row"
        Assert-CombinedCondition ($Sha256 -cmatch '^[0-9A-F]{64}$' -and
            $paths.Add($normalized)) `
            "$context contains a duplicate path or noncanonical hash."
        $rows.Add("$normalized|$Sha256") | Out-Null
    }

    $receiptDestination = Join-Path $destinationBase 'receipt.json'
    Copy-CombinedSourceFile $sourceBase `
        ([IO.Path]::GetFileName([string]$Reviewed.path)) $OutputDirectory `
        $receiptDestination ([string]$Reviewed.sha256) "$context receipt" $Copied `
        -SourceSnapshot $Reviewed.snapshot | Out-Null
    Add-ReviewedClosureRow 'receipt.json' ([string]$Reviewed.sha256)

    $protection = Get-Stage5JsonValue $Reviewed.read.document 'protection' $context
    Assert-Stage5JsonShape $protection @('kind', 'path', 'sha256') `
        "$context protected attestation"
    $protectionRelative = [string]$protection.path
    Copy-CombinedSourceFile $sourceBase $protectionRelative $OutputDirectory `
        (Join-Path $destinationBase $protectionRelative) `
        ([string]$protection.sha256).ToUpperInvariant() `
        "$context protected attestation" $Copied | Out-Null
    Add-ReviewedClosureRow $protectionRelative `
        ([string]$protection.sha256).ToUpperInvariant()

    $manifestReference = Get-Stage5JsonValue $Reviewed.read.provenance `
        'fixtureManifest' $context
    Assert-Stage5JsonShape $manifestReference @('path', 'sha256') `
        "$context fixture manifest"
    $manifestRelative = [string]$manifestReference.path
    Copy-CombinedSourceFile $sourceBase $manifestRelative $OutputDirectory `
        (Join-Path $destinationBase $manifestRelative) `
        ([string]$manifestReference.sha256).ToUpperInvariant() `
        "$context fixture manifest" $Copied `
        -SourceSnapshot $Reviewed.read.reviewedFixtureManifestSnapshot | Out-Null
    Add-ReviewedClosureRow $manifestRelative `
        ([string]$manifestReference.sha256).ToUpperInvariant()

    $manifestDirectory = Split-Path -Parent `
        ([string]$Reviewed.read.reviewedFixtureManifestSnapshot.path)
    $manifestDestinationDirectory = Split-Path -Parent `
        (Join-Path $destinationBase $manifestRelative)
    $fixtures = Get-Stage5JsonValue $Reviewed.read.reviewedFixtureManifest `
        'fixtures' "$context fixture manifest"
    Assert-CombinedCondition ($fixtures.Count -eq 10) `
        "$context must contain exactly ten reviewed replay fixtures."
    foreach ($fixture in $fixtures) {
        Assert-Stage5JsonShape $fixture @('id', 'source', 'sha256', 'stress') `
            "$context replay fixture"
        if (@($fixture.Keys | Where-Object { [string]$_ -ceq 'maps' }).Count -gt 0) {
            Assert-CombinedCondition (@($fixture.maps).Count -eq 0) `
                "$context may not depend on undeclared map bytes."
        }
        $fixtureRelative = [string]$fixture.source
        $fixtureHash = ([string]$fixture.sha256).ToUpperInvariant()
        $fixtureDestination = Join-Path $manifestDestinationDirectory $fixtureRelative
        Copy-CombinedSourceFile $manifestDirectory $fixtureRelative $OutputDirectory `
            $fixtureDestination $fixtureHash `
            "$context replay fixture '$([string]$fixture.id)'" $Copied `
            -EvidenceKind Replay | Out-Null
        $closureRelative = Get-CombinedRelativePathFromBase `
            (Join-Path $OutputDirectory $destinationBase) `
            (Join-Path $OutputDirectory $fixtureDestination) `
            "$context replay fixture '$([string]$fixture.id)'"
        Add-ReviewedClosureRow $closureRelative $fixtureHash
    }
    Assert-CombinedCondition ($rows.Count -eq 13 -and $paths.Count -eq 13) `
        "$context must contain exactly receipt, attestation, manifest, and ten replay bytes."
    $sortedRows = $rows.ToArray()
    [Array]::Sort($sortedRows, [StringComparer]::Ordinal)
    $closureText = ($sortedRows -join "`n") + "`n"
    return [ordered]@{
        receipt = [ordered]@{
            path = $receiptDestination.Replace('/', '\')
            sha256 = [string]$Reviewed.sha256
        }
        protection = [ordered]@{
            path = (Join-Path $destinationBase $protectionRelative).Replace('/', '\')
            sha256 = ([string]$protection.sha256).ToUpperInvariant()
        }
        manifest = [ordered]@{
            path = (Join-Path $destinationBase $manifestRelative).Replace('/', '\')
            sha256 = ([string]$manifestReference.sha256).ToUpperInvariant()
        }
        fileCount = 13
        closureSha256 = Get-CombinedUtf8Sha256 $closureText
    }
}

function Get-CombinedChildStream {
    param([object]$Child, [string]$Name, [string]$Context)
    $stream = Get-Stage5JsonValue $Child $Name "$Context child"
    Assert-Stage5JsonShape $stream @('path', 'sha256') "$Context child $Name"
    return $stream
}

function Get-CombinedSourceEnvelopePreflight {
    param(
        [Parameter(Mandatory = $true)][object]$Snapshot,
        [Parameter(Mandatory = $true)][string]$ExpectedTitle,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedExecutableSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $context = "$Context source receipt preflight"
    $document = ConvertFrom-CombinedJsonSnapshot $Snapshot $context
    Assert-Stage5JsonShape $document @(
        'schemaVersion', 'evidenceKind', 'status', 'role', 'trustDomain',
        'producer', 'producerVersion', 'sourceCommit', 'title', 'architecture',
        'artifactSetSha256', 'recordedUtc', 'cohortNonce', 'runtimeClosure', 'runNonce',
        'executableSha256', 'rawLogs', 'provenance', 'details') $context

    $producer = [string](Get-Stage5JsonValue $document 'producer' $context)
    Assert-CombinedCondition ($producer -ceq 'installed-runtime-validation-results-v2') `
        "$Context source receipt is not the allowlisted host-runner v2 producer."
    Assert-CombinedCondition (
        [string](Get-Stage5JsonValue $document 'title' $context) -ceq $ExpectedTitle) `
        "$Context title scope is substituted; expected '$ExpectedTitle'."
    Assert-CombinedCondition (
        [string](Get-Stage5JsonValue $document 'sourceCommit' $context) -ceq
            $ExpectedSourceCommit) `
        "$Context source receipt is stale or does not match the final acceptance commit."
    $artifactSetSha256 = [string](Get-Stage5JsonValue $document `
        'artifactSetSha256' $context)
    Assert-CombinedCondition ($artifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $artifactSetSha256.ToUpperInvariant() -ceq
            $ExpectedArtifactSetSha256.ToUpperInvariant()) `
        "$Context artifact-set SHA-256 binding differs from the final acceptance artifact."
    $executableSha256 = [string](Get-Stage5JsonValue $document `
        'executableSha256' $context)
    Assert-CombinedCondition ($executableSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $executableSha256.ToUpperInvariant() -ceq
            $ExpectedExecutableSha256.ToUpperInvariant()) `
        "$Context executable SHA-256 binding differs from the expected artifact."
    Assert-CombinedCondition (
        [string](Get-Stage5JsonValue $document 'cohortNonce' $context) -ceq
            $ExpectedCohortNonce) `
        "$Context source receipt cohort is stale or detached from the execution cohort."
    $runNonce = [string](Get-Stage5JsonValue $document 'runNonce' $context)
    Assert-CombinedCondition ($runNonce -match
        '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
        "$Context source receipt nonce is not canonical."
    return [pscustomobject]@{
        document = $document
        runNonce = $runNonce
    }
}

$sourceCommit = $ExpectedSourceCommit.ToLowerInvariant()
Assert-CombinedCondition ($sourceCommit -cmatch '^[0-9a-f]{40}$') `
    'ExpectedSourceCommit must be a lowercase 40-hex commit.'
Assert-CombinedCondition ($ExpectedArtifactSetSha256 -match '^[0-9A-Fa-f]{64}$') `
    'ExpectedArtifactSetSha256 must contain exactly 64 hexadecimal characters.'
Assert-CombinedCondition ($ExpectedGeneralsExecutableSha256 -match '^[0-9A-Fa-f]{64}$' -and
    $ExpectedZeroHourExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
    'Both expected executable SHA-256 values must be canonical.'
Assert-CombinedCondition ($GeneralsReviewedFixtureReceiptSha256 -cmatch '^[0-9A-F]{64}$' -and
    $ZeroHourReviewedFixtureReceiptSha256 -cmatch '^[0-9A-F]{64}$') `
    'Both protected reviewed-fixture receipt SHA-256 values must be independently supplied as uppercase 64-hex.'
Assert-CombinedCondition ($ExpectedCohortNonce -cmatch
    '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') `
    'ExpectedCohortNonce must be a canonical lowercase RFC 4122 version-4 UUID minted before the title runs.'
[DateTimeOffset]$expectedCohortCreated = [DateTimeOffset]::MinValue
Assert-CombinedCondition ($ExpectedCohortCreatedUtc -cmatch
        '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
    [DateTimeOffset]::TryParseExact($ExpectedCohortCreatedUtc, 'o',
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind,
        [ref]$expectedCohortCreated)) `
    'ExpectedCohortCreatedUtc must be a canonical UTC execution-cohort timestamp.'

$generalsFull = [IO.Path]::GetFullPath($GeneralsReceiptPath)
$zeroHourFull = [IO.Path]::GetFullPath($ZeroHourReceiptPath)
$generalsReviewedFull = [IO.Path]::GetFullPath($GeneralsReviewedFixtureReceiptPath)
$zeroHourReviewedFull = [IO.Path]::GetFullPath($ZeroHourReviewedFixtureReceiptPath)
$outputFull = [IO.Path]::GetFullPath($OutputPath)
Assert-CombinedCondition (-not [String]::Equals($generalsFull, $zeroHourFull,
    [StringComparison]::OrdinalIgnoreCase)) `
    'Generals and Zero Hour source receipts must be different files.'
Assert-CombinedCondition (-not [String]::Equals($generalsReviewedFull,
        $zeroHourReviewedFull, [StringComparison]::OrdinalIgnoreCase) -and
    $GeneralsReviewedFixtureReceiptSha256 -cne
        $ZeroHourReviewedFixtureReceiptSha256) `
    'Generals and Zero Hour protected reviewed-fixture receipts must be distinct files and byte identities.'
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
Write-CombinedPhaseTiming 'source-envelope-preflight' 'start'
$generalsSnapshot = Get-CombinedFileSnapshot $generalsFull 'Generals source receipt'
$zeroHourSnapshot = Get-CombinedFileSnapshot $zeroHourFull 'Zero Hour source receipt'
$generalsHash = [string]$generalsSnapshot.sha256
$zeroHourHash = [string]$zeroHourSnapshot.sha256
$generalsPreflight = Get-CombinedSourceEnvelopePreflight `
    -Snapshot $generalsSnapshot -ExpectedTitle 'Generals' `
    -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ExpectedExecutableSha256 $ExpectedGeneralsExecutableSha256 `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -Context 'Generals'
$zeroHourPreflight = Get-CombinedSourceEnvelopePreflight `
    -Snapshot $zeroHourSnapshot -ExpectedTitle 'ZeroHour' `
    -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ExpectedExecutableSha256 $ExpectedZeroHourExecutableSha256 `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -Context 'Zero Hour'
Assert-CombinedCondition ([string]$generalsPreflight.runNonce -cne
        [string]$zeroHourPreflight.runNonce) `
    'Generals and Zero Hour source receipts must have distinct run nonces.'
$generalsSourceBase = Split-Path -Parent $generalsFull
$zeroHourSourceBase = Split-Path -Parent $zeroHourFull
Write-CombinedPhaseTiming 'source-envelope-preflight' 'complete'
Write-CombinedPhaseTiming 'generals-native-closure' 'start'
$generalsNativeClosureInfo = Get-CombinedNativeClosureInfo $generalsFull `
    $generalsSourceBase 'Generals' 'Generals source receipt'
Write-CombinedPhaseTiming 'generals-native-closure' 'complete'
Write-CombinedPhaseTiming 'zerohour-native-closure' 'start'
$zeroHourNativeClosureInfo = Get-CombinedNativeClosureInfo $zeroHourFull `
    $zeroHourSourceBase 'ZeroHour' 'Zero Hour source receipt'
Write-CombinedPhaseTiming 'zerohour-native-closure' 'complete'
Assert-CombinedCondition ((Get-Stage5FileSha256 $generalsFull) -ceq $generalsHash -and
    (Get-Stage5FileSha256 $zeroHourFull) -ceq $zeroHourHash) `
    'A source receipt changed while its complete native closure was being bound.'
Write-CombinedPhaseTiming 'generals-source-read' 'start'
$generalsRead = Read-Stage5FinalAcceptanceImmutableReceipt `
    -Path $generalsFull -Kind 'deterministic-runtime' -Role 'validation-results' `
    -EvidenceTitle 'Generals' -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -SeenRunNonces $sourceSeenRunNonces `
    -ExpectedEvidenceSha256 $generalsHash -EvidenceSnapshot $generalsSnapshot `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
    -ExpectedEvidenceDirectory $generalsSourceBase `
    -NativeRelocationBindings @($generalsNativeClosureInfo.children)
Write-CombinedPhaseTiming 'generals-source-read' 'complete'
$sourceCohortNonce = [string]$generalsRead.cohortNonce
$sourceRuntimeClosure = $generalsRead.runtimeClosure
Write-CombinedPhaseTiming 'zerohour-source-read' 'start'
$zeroHourRead = Read-Stage5FinalAcceptanceImmutableReceipt `
    -Path $zeroHourFull -Kind 'deterministic-runtime' -Role 'validation-results' `
    -EvidenceTitle 'ZeroHour' -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -SeenRunNonces $sourceSeenRunNonces `
    -ExpectedEvidenceSha256 $zeroHourHash -EvidenceSnapshot $zeroHourSnapshot `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
    -ExpectedRuntimeClosure $sourceRuntimeClosure `
    -ExpectedEvidenceDirectory $zeroHourSourceBase `
    -NativeRelocationBindings @($zeroHourNativeClosureInfo.children)
Write-CombinedPhaseTiming 'zerohour-source-read' 'complete'
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
$generalsReviewed = Read-CombinedReviewedFixtureReceipt `
    -Path $generalsReviewedFull `
    -ExpectedSha256 $GeneralsReviewedFixtureReceiptSha256 -Title 'Generals' `
    -SourceCommit $sourceCommit -ArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -RuntimeClosure $sourceRuntimeClosure `
    -Context 'Generals source corpus'
$zeroHourReviewed = Read-CombinedReviewedFixtureReceipt `
    -Path $zeroHourReviewedFull `
    -ExpectedSha256 $ZeroHourReviewedFixtureReceiptSha256 -Title 'ZeroHour' `
    -SourceCommit $sourceCommit -ArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -RuntimeClosure $sourceRuntimeClosure `
    -Context 'Zero Hour source corpus'
$sourceCorpora = @{}
Write-CombinedPhaseTiming 'source-corpora-validation' 'start'
foreach ($corpus in @(
    [pscustomobject]@{ title = 'Generals'; resultsPath = $generalsFull
        resultsSnapshot = $generalsSnapshot; resultsHash = $generalsHash
        resultsRead = $generalsRead; nativeClosure = $generalsNativeClosureInfo
        reviewed = $generalsReviewed },
    [pscustomobject]@{ title = 'ZeroHour'; resultsPath = $zeroHourFull
        resultsSnapshot = $zeroHourSnapshot; resultsHash = $zeroHourHash
        resultsRead = $zeroHourRead; nativeClosure = $zeroHourNativeClosureInfo
        reviewed = $zeroHourReviewed }
)) {
    $corpusContext = "$($corpus.title) source corpus"
    $fixtureSnapshot = $corpus.reviewed.read.reviewedFixtureManifestSnapshot
    $fixtureDocument = $corpus.reviewed.read.reviewedFixtureManifest
    Assert-CombinedCondition ($fixtureDocument -is [Collections.IDictionary]) `
        "$corpusContext fixture manifest is not a JSON object."
    $records = [ordered]@{}
    $records['validation-results'] = [pscustomobject]@{
        role = 'validation-results'; path = $corpus.resultsPath
        snapshot = $corpus.resultsSnapshot; sha256 = $corpus.resultsHash
        read = $corpus.resultsRead; relocation = $corpus.nativeClosure
    }
    foreach ($support in @(
        [pscustomobject]@{ role = 'validation-plan'; leaf = 'validation-plan-receipt.json' },
        [pscustomobject]@{ role = 'replay-results'; leaf = 'replay-results-receipt.json' },
        [pscustomobject]@{ role = 'ai-results'; leaf = 'ai-results-receipt.json' }
    )) {
        $supportPhase = "support-$($corpus.title)-$($support.role)"
        Write-CombinedPhaseTiming $supportPhase 'start'
        $supportPath = Resolve-CombinedSiblingReceipt $corpus.resultsPath `
            $support.leaf $corpusContext
        $records[$support.role] = Read-CombinedSupportingReceipt `
            -Path $supportPath -Role $support.role -Title $corpus.title `
            -SourceCommit $sourceCommit -ArtifactSetSha256 $ExpectedArtifactSetSha256 `
            -ArtifactHashes $artifactHashes -SeenRunNonces $sourceSeenRunNonces `
            -CohortNonce $sourceCohortNonce `
            -CohortCreatedUtc $ExpectedCohortCreatedUtc `
            -RuntimeClosure $sourceRuntimeClosure -Context $corpusContext
        Write-CombinedPhaseTiming $supportPhase 'complete'
    }
    $qualificationData = Get-CombinedQualificationDataBinding `
        -Receipt $records['validation-plan'].read -Title $corpus.title `
        -Context "$corpusContext validation-plan"
    foreach ($role in @('validation-results', 'replay-results', 'ai-results')) {
        $roleQualificationData = Get-CombinedQualificationDataBinding `
            -Receipt $records[$role].read -Title $corpus.title `
            -Context "$corpusContext $role"
        Assert-CombinedQualificationDataBindingEqual `
            -Expected $qualificationData -Actual $roleQualificationData `
            -Context "$corpusContext $role"
    }
    $qualificationDataPath = [IO.Path]::GetFullPath((Join-Path `
        (Split-Path -Parent $corpus.resultsPath) `
        ([string]$qualificationData.path)))
    [void](Get-CombinedRelativePathFromBase `
        (Split-Path -Parent $corpus.resultsPath) $qualificationDataPath `
        "$corpusContext qualification-data manifest")
    Assert-CombinedNoReparsePath $qualificationDataPath `
        "$corpusContext qualification-data manifest"
    $qualificationDataSnapshot = Get-CombinedFileSnapshot `
        $qualificationDataPath "$corpusContext qualification-data manifest"
    Assert-CombinedCondition ([string]$qualificationDataSnapshot.sha256 -ceq
        [string]$qualificationData.manifestSha256) `
        "$corpusContext qualification-data manifest SHA-256 changed."
    $qualificationDataEvidence = Read-Stage5SimulationQualificationDataEvidence `
        -Path $qualificationDataPath -Binding $qualificationData `
        -ExpectedSourceCommit $sourceCommit -ExpectedTitle $corpus.title
    Assert-CombinedCondition (
        [string]$qualificationDataEvidence.manifestSha256 -ceq
            [string]$qualificationData.manifestSha256 -and
        [string]$qualificationDataEvidence.closureSha256 -ceq
            [string]$qualificationData.closureSha256 -and
        [int]$qualificationDataEvidence.fileCount -eq 6) `
        "$corpusContext qualification-data evidence is stale or incomplete."
    $planRaw = Get-Stage5DevelopmentReadinessRawLog `
        $records['validation-plan'].read.rawLogs 'validation-plan.json' `
        "$corpusContext validation plan"
    $resultsRaw = Get-Stage5DevelopmentReadinessRawLog `
        $records['validation-results'].read.rawLogs 'validation-results.json' `
        "$corpusContext validation results"
    $replayRaw = Get-Stage5DevelopmentReadinessRawLog `
        $records['replay-results'].read.rawLogs 'validation-results.json' `
        "$corpusContext replay results"
    $aiRaw = Get-Stage5DevelopmentReadinessRawLog `
        $records['ai-results'].read.rawLogs 'validation-results.json' `
        "$corpusContext AI results"
    $planDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $planRaw.snapshot `
        "$corpusContext validation plan"
    $resultsDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $resultsRaw.snapshot `
        "$corpusContext validation results"
    Assert-CombinedCondition ($resultsDocument -is [Array]) `
        "$corpusContext validation results must be a JSON array."
    $readinessPhase = "development-readiness-$($corpus.title)"
    Write-CombinedPhaseTiming $readinessPhase 'start'
    [void](Assert-Stage5DevelopmentReadinessExecutionEvidence `
        -ValidationPlan $planDocument -Results @($resultsDocument) `
        -ReviewedFixtureManifest $fixtureDocument `
        -PlanDetails $records['validation-plan'].read.details `
        -ValidationDetails $records['validation-results'].read.details `
        -ReplayDetails $records['replay-results'].read.details `
        -AiDetails $records['ai-results'].read.details `
        -ValidatedRawLogs $records['validation-results'].read.rawLogs `
        -ValidatedChildren $records['validation-results'].read.validatedChildren `
        -ExpectedPlanSha256 ([string]$planRaw.sha256) `
        -ValidationResultsSha256 ([string]$resultsRaw.sha256) `
        -ReplayResultsSha256 ([string]$replayRaw.sha256) `
        -AiResultsSha256 ([string]$aiRaw.sha256) `
        -ExpectedSourceCommit $sourceCommit `
        -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
        -ExpectedCohortNonce $sourceCohortNonce `
        -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
        -ExpectedRuntimeClosure $sourceRuntimeClosure `
        -ExpectedQualificationData $qualificationData `
        -ExpectedTitle $corpus.title)
    Write-CombinedPhaseTiming $readinessPhase 'complete'
    $sourceCorpora[$corpus.title] = [pscustomobject]@{
        title = $corpus.title; sourceBase = Split-Path -Parent $corpus.resultsPath
        records = $records; reviewed = $corpus.reviewed
        fixtureSnapshot = $fixtureSnapshot; fixtureSha256 = [string]$fixtureSnapshot.sha256
        qualificationData = $qualificationData
        qualificationDataPath = $qualificationDataPath
        qualificationDataSnapshot = $qualificationDataSnapshot
    }
}
Write-CombinedPhaseTiming 'source-corpora-validation' 'complete'
$generalsChild = Get-CombinedSourceChild $generalsRead 'Generals' 'Generals source receipt'
$zeroHourChild = Get-CombinedSourceChild $zeroHourRead 'ZeroHour' 'Zero Hour source receipt'
foreach ($projection in @(
    [pscustomobject]@{ title = 'Generals'; child = $generalsChild },
    [pscustomobject]@{ title = 'ZeroHour'; child = $zeroHourChild }
)) {
    $childQualificationData = Get-Stage5JsonValue $projection.child `
        'qualificationData' "$($projection.title) sequence-1 source child"
    Assert-Stage5JsonShape $childQualificationData @('path', 'title',
        'manifestSha256', 'closureSha256', 'fileCount') `
        "$($projection.title) sequence-1 source child qualificationData"
    Assert-CombinedQualificationDataBindingEqual `
        -Expected $sourceCorpora[$projection.title].qualificationData `
        -Actual $childQualificationData `
        -Context "$($projection.title) sequence-1 source child"
}
$generalsSelectedRelocation = Get-CombinedSourceRelocation `
    $generalsNativeClosureInfo $generalsChild 'Generals source receipt'
$zeroHourSelectedRelocation = Get-CombinedSourceRelocation `
    $zeroHourNativeClosureInfo $zeroHourChild 'Zero Hour source receipt'
$generalsProjectionPolicy = Get-CombinedProjectionPolicy $generalsChild `
    'Generals source receipt'
$zeroHourProjectionPolicy = Get-CombinedProjectionPolicy $zeroHourChild `
    'Zero Hour source receipt'
foreach ($field in @('pipelineMode', 'simulationMode', 'requestedWorkers',
        'workerPolicy')) {
    Assert-CombinedCondition ([string]$generalsProjectionPolicy.$field -ceq
        [string]$zeroHourProjectionPolicy.$field) `
        "Generals and Zero Hour sequence-1 projection policy '$field' differs."
}
Assert-CombinedCondition (-not ([string]$generalsChild.processId -eq [string]$zeroHourChild.processId -and
    [string]$generalsChild.processCreationUtc -ceq [string]$zeroHourChild.processCreationUtc)) `
    'Generals and Zero Hour source receipts must identify distinct child processes.'

Write-CombinedPhaseTiming 'source-directory-setup' 'start'
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
Write-CombinedPhaseTiming 'source-directory-setup' 'complete'

Write-CombinedPhaseTiming 'source-copy-and-stage' 'start'
$generalsCopied = @{}
$zeroHourCopied = @{}
$generalsPathMap = @{}
$zeroHourPathMap = @{}
foreach ($source in @(
    [pscustomobject]@{ title = 'Generals'; read = $generalsRead; child = $generalsChild
        sourceBase = $generalsSourceBase; stageRoot = $generalsStageRoot
        copied = $generalsCopied; map = $generalsPathMap
        nativeRelocations = @($generalsNativeClosureInfo.children)
        selectedRelocation = $generalsSelectedRelocation
        corpus = $sourceCorpora['Generals'] },
    [pscustomobject]@{ title = 'ZeroHour'; read = $zeroHourRead; child = $zeroHourChild
        sourceBase = $zeroHourSourceBase; stageRoot = $zeroHourStageRoot
        copied = $zeroHourCopied; map = $zeroHourPathMap
        nativeRelocations = @($zeroHourNativeClosureInfo.children)
        selectedRelocation = $zeroHourSelectedRelocation
        corpus = $sourceCorpora['ZeroHour'] }
)) {
    $sourceCopyPhase = "source-copy-$($source.title)"
    $sourceCopyChildIndex = 0
    $sourceCopyRawIndex = 0
    Write-CombinedPhaseTiming $sourceCopyPhase 'start' $source.title `
        0 $source.read.rawLogs.Count
    foreach ($raw in @($source.read.rawLogs)) {
        ++$sourceCopyRawIndex
        if ($sourceCopyRawIndex -eq 1 -or ($sourceCopyRawIndex % 128) -eq 0 -or
            $sourceCopyRawIndex -eq $source.read.rawLogs.Count) {
            Write-CombinedPhaseTiming "$sourceCopyPhase-raw-logs" 'progress' `
                $source.title $sourceCopyRawIndex $source.read.rawLogs.Count
        }
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
    foreach ($supportRole in @('validation-plan', 'replay-results', 'ai-results')) {
        $supportRecord = $source.corpus.records[$supportRole]
        foreach ($raw in @($supportRecord.read.rawLogs)) {
            $rawPath = [string]$raw.path
            $rawHash = [string]$raw.sha256
            $destinationRelative = Join-Path ('sources\' + $source.title) $rawPath
            $rawSnapshot = $null
            if (@($raw.PSObject.Properties.Name | Where-Object {
                    [string]$_ -ceq 'snapshot'
                }).Count -gt 0) {
                $rawSnapshot = $raw.snapshot
            }
            Copy-CombinedSourceFile $source.sourceBase $rawPath $outputDirectory `
                $destinationRelative $rawHash `
                "$($source.title) $supportRole source raw log" $source.copied `
                -SourceSnapshot $rawSnapshot | Out-Null
        }
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
    $relocationsBySequence = @{}
    foreach ($relocation in @($source.nativeRelocations)) {
        $relocationsBySequence[[int]$relocation.sequence] = $relocation
    }
    Assert-CombinedCondition ($relocationsBySequence.Count -eq 253) `
        "$($source.title) source native relocation map is incomplete."
    foreach ($sourceChild in @($source.read.provenance.children)) {
        ++$sourceCopyChildIndex
        if ($sourceCopyChildIndex -eq 1 -or ($sourceCopyChildIndex % 32) -eq 0 -or
            $sourceCopyChildIndex -eq 169 -or $sourceCopyChildIndex -eq 170 -or
            $sourceCopyChildIndex -eq $source.read.provenance.children.Count) {
            Write-CombinedPhaseTiming "$sourceCopyPhase-native-children" 'progress' `
                $source.title $sourceCopyChildIndex $source.read.provenance.children.Count
        }
        $sequence = [int](Get-Stage5JsonValue $sourceChild 'sequence' `
            "$($source.title) source child")
        Assert-CombinedCondition ($relocationsBySequence.ContainsKey($sequence)) `
            "$($source.title) source child $sequence lacks a native relocation."
        $relocation = $relocationsBySequence[$sequence]
        Assert-CombinedCondition ([string]$relocation.runNonce -ceq
            [string]$sourceChild.runNonce) `
            "$($source.title) source child $sequence relocation nonce is detached."
        foreach ($nativeBinding in @($relocation.nativeRawBindings)) {
            Assert-Stage5JsonShape $nativeBinding @('name', 'sourcePath', 'path',
                'sha256') "$($source.title) source child $sequence native raw binding"
            $nativeRawSourceRelative = [string]$nativeBinding.path
            $nativeRawDestination = Join-Path ('sources\' + $source.title) `
                $nativeRawSourceRelative
            Copy-CombinedSourceFile $source.sourceBase $nativeRawSourceRelative `
                $outputDirectory $nativeRawDestination ([string]$nativeBinding.sha256) `
                "$($source.title) source child $sequence native raw evidence '$($nativeBinding.name)'" `
                $source.copied | Out-Null
        }
        $native = Get-Stage5JsonValue $sourceChild 'nativeReceipt' `
            "$($source.title) source child $sequence"
        Assert-Stage5JsonShape $native @('path', 'sha256', 'producer', 'runNonce',
            'cohortNonce') `
            "$($source.title) source child $sequence native receipt"
        $nativePath = [string]$native.path
        $destinationRelative = Join-Path ('sources\' + $source.title) $nativePath
        $source.map[$nativePath.Replace('/', '\').ToLowerInvariant()] =
            (Copy-CombinedSourceFile $source.sourceBase $nativePath $outputDirectory `
                $destinationRelative ([string]$native.sha256) `
                "$($source.title) source child $sequence native receipt" $source.copied)
    }
    Write-CombinedPhaseTiming $sourceCopyPhase 'complete' $source.title `
        $sourceCopyChildIndex $source.read.provenance.children.Count
}

$stagedCorpora = [ordered]@{}
foreach ($source in @(
    [pscustomobject]@{ title = 'Generals'; corpus = $sourceCorpora['Generals']
        copied = $generalsCopied },
    [pscustomobject]@{ title = 'ZeroHour'; corpus = $sourceCorpora['ZeroHour']
        copied = $zeroHourCopied }
)) {
    $receiptReferences = New-Object 'Collections.Generic.List[object]'
    foreach ($role in @('validation-plan', 'validation-results', 'replay-results',
            'ai-results')) {
        $record = $source.corpus.records[$role]
        $leaf = [IO.Path]::GetFileName([string]$record.path)
        $expectedLeaf = "$role-receipt.json"
        Assert-CombinedCondition ($leaf -ceq $expectedLeaf) `
            "$($source.title) $role receipt lost its canonical leaf name."
        $destinationRelative = Join-Path ('sources\' + $source.title) $leaf
        Copy-CombinedSourceFile $source.corpus.sourceBase $leaf $outputDirectory `
            $destinationRelative ([string]$record.sha256) `
            "$($source.title) $role source receipt" $source.copied `
            -SourceSnapshot $record.snapshot | Out-Null
        Assert-CombinedCondition ((Get-Stage5FileSha256 `
                (Join-Path $outputDirectory $destinationRelative)) -ceq
            [string]$record.sha256) `
            "Staged $($source.title) $role source receipt hash changed."
        [void]$receiptReferences.Add([ordered]@{
            role = $role; path = $destinationRelative.Replace('/', '\')
            sha256 = ([string]$record.sha256).ToUpperInvariant()
            runNonce = [string]$record.read.runNonce
            cohortNonce = [string]$record.read.cohortNonce
        })
    }
    $reviewedFixture = Copy-CombinedReviewedFixtureClosure `
        -Title $source.title -Reviewed $source.corpus.reviewed `
        -OutputDirectory $outputDirectory -Copied $source.copied
    $qualificationDataDestination = Join-Path ('sources\' + $source.title) `
        'QualificationData.json'
    Copy-CombinedSourceFile $source.corpus.sourceBase `
        ([string]$source.corpus.qualificationData.path) $outputDirectory `
        $qualificationDataDestination `
        ([string]$source.corpus.qualificationData.manifestSha256) `
        "$($source.title) qualification-data manifest" $source.copied `
        -SourceSnapshot $source.corpus.qualificationDataSnapshot `
        -EvidenceKind JsonReceipt | Out-Null
    $stagedCorpora[$source.title] = [pscustomobject]@{
        title = $source.title; sourceChildCount = 253
        reviewedFixture = $reviewedFixture
        qualificationData = [ordered]@{
            path = 'QualificationData.json'
            title = $source.title
            manifestSha256 = [string]$source.corpus.qualificationData.manifestSha256
            closureSha256 = [string]$source.corpus.qualificationData.closureSha256
            fileCount = 6
        }
        receipts = @($receiptReferences.ToArray())
    }
}
Write-CombinedPhaseTiming 'source-copy-and-stage' 'complete'

function New-CombinedChild {
    param(
        [object]$Child,
        [string]$Title,
        [string]$SourceBase,
        [string]$OutputDirectory,
        [hashtable]$PathMap,
        [hashtable]$Copied,
        [object]$NativeRawBindings,
        [string]$NativeReceiptSourcePath,
        [Collections.IDictionary]$QualificationData
    )
    $combined = [ordered]@{
        role = 'combined-results'
        sourceSequence = [int]$Child.sequence
        title = $Title
        runNonce = [string]$Child.runNonce
        processId = [int]$Child.processId
        processCreationUtc = [string]$Child.processCreationUtc
        executablePath = [string]$Child.executablePath
        executableSha256 = [string]$Child.executableSha256
        commandLine = [string]$Child.commandLine
        exitCode = [int]$Child.exitCode
        qualificationData = [ordered]@{
            path = 'QualificationData.json'
            title = $Title
            manifestSha256 = [string]$QualificationData.manifestSha256
            closureSha256 = [string]$QualificationData.closureSha256
            fileCount = 6
        }
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
        Assert-CombinedCondition (@($NativeRawBindings).Count -eq 2) `
            "$Title native raw binding metadata is incomplete."
        $combined.nativeRawBindings = @(
            foreach ($binding in @($NativeRawBindings)) {
                Assert-Stage5JsonShape $binding @('name', 'sourcePath', 'path', 'sha256') `
                    "$Title combined native raw binding"
                [ordered]@{
                    name = [string]$binding.name
                    sourcePath = [string]$binding.sourcePath
                    path = (Join-Path ('sources\' + $Title) `
                        ([string]$binding.path)).Replace('/', '\')
                    sha256 = [string]$binding.sha256
                }
            }
        )
        Assert-CombinedNativeRawPathText $NativeReceiptSourcePath `
            "$Title native receipt source path"
        $combined.nativeReceiptSourcePath = $NativeReceiptSourcePath
    }
    return $combined
}

$combinedGeneralsChild = New-CombinedChild $generalsChild 'Generals' `
    $generalsSourceBase $outputDirectory $generalsPathMap $generalsCopied `
    $generalsSelectedRelocation.nativeRawBindings `
    $generalsSelectedRelocation.nativeReceiptSourcePath `
    $sourceCorpora['Generals'].qualificationData
$combinedZeroHourChild = New-CombinedChild $zeroHourChild 'ZeroHour' `
    $zeroHourSourceBase $outputDirectory $zeroHourPathMap $zeroHourCopied `
    $zeroHourSelectedRelocation.nativeRawBindings `
    $zeroHourSelectedRelocation.nativeReceiptSourcePath `
    $sourceCorpora['ZeroHour'].qualificationData
$combinedRunNonce = [Guid]::NewGuid().ToString()
while ($combinedRunNonce -ceq [string]$generalsRead.runNonce -or
    $combinedRunNonce -ceq [string]$zeroHourRead.runNonce) {
    $combinedRunNonce = [Guid]::NewGuid().ToString()
}

$combinedRawRelative = 'combined-results.sources.raw.log'
$combinedRawFull = Join-Path $outputDirectory $combinedRawRelative
Assert-CombinedCondition (-not (Test-Path -LiteralPath $combinedRawFull)) `
    "Combined raw binding log already exists: $combinedRawFull"
$combinedRawLines = New-Object 'Collections.Generic.List[string]'
foreach ($line in @(
    'STAGE5_COMBINED_HOST_RUNNER_V2'
    "sourceCommit=$sourceCommit"
    "artifactSetSha256=$($ExpectedArtifactSetSha256.ToUpperInvariant())"
)) { [void]$combinedRawLines.Add($line) }
$combinedSourceRawLogs = New-Object 'Collections.Generic.List[object]'
foreach ($title in @('Generals', 'ZeroHour')) {
    $stagedCorpus = $stagedCorpora[$title]
    foreach ($receipt in @($stagedCorpus.receipts)) {
        [void]$combinedRawLines.Add(
            "$title-$($receipt.role)-sha256=$($receipt.sha256)")
        [void]$combinedRawLines.Add(
            "$title-$($receipt.role)-runNonce=$($receipt.runNonce)")
        [void]$combinedSourceRawLogs.Add([ordered]@{
            name = "$title-$($receipt.role)-receipt"
            path = [string]$receipt.path; sha256 = [string]$receipt.sha256
        })
    }
    [void]$combinedRawLines.Add(
        "$title-reviewed-fixture-closure-sha256=$($stagedCorpus.reviewedFixture.closureSha256)")
    [void]$combinedRawLines.Add(
        "$title-qualification-data-manifest-sha256=$($stagedCorpus.qualificationData.manifestSha256)")
    [void]$combinedRawLines.Add(
        "$title-qualification-data-closure-sha256=$($stagedCorpus.qualificationData.closureSha256)")
    [void]$combinedSourceRawLogs.Add([ordered]@{
        name = "$title-qualification-data-manifest"
        path = (Join-Path ('sources\' + $title) `
            ([string]$stagedCorpus.qualificationData.path)).Replace('/', '\')
        sha256 = [string]$stagedCorpus.qualificationData.manifestSha256
    })
    foreach ($reviewedRole in @('receipt', 'protection', 'manifest')) {
        $reviewedReference = $stagedCorpus.reviewedFixture[$reviewedRole]
        [void]$combinedSourceRawLogs.Add([ordered]@{
            name = "$title-reviewed-fixture-$reviewedRole"
            path = [string]$reviewedReference.path
            sha256 = [string]$reviewedReference.sha256
        })
    }
}
$combinedRaw = $combinedRawLines.ToArray() -join "`n"
Assert-CombinedNoReparsePath $outputDirectory 'Combined output directory before raw binding'
$combinedRawBytes = (New-Object Text.UTF8Encoding($false)).GetBytes($combinedRaw + "`n")
$combinedRawSnapshot = Write-Stage5FinalAcceptanceFileAtomically `
    $combinedRawFull $combinedRawBytes 'Combined raw binding log' -EvidenceKind RawLog
$combinedRawHash = [string]$combinedRawSnapshot.sha256

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
    rawLogs = @($combinedSourceRawLogs.ToArray()) + @([ordered]@{
        name = 'combined-source-bindings'; path = $combinedRawRelative
        sha256 = $combinedRawHash
    })
    provenance = [ordered]@{
        kind = 'host-runner-observation'
        runner = 'New-Stage5CombinedHostRunnerReceipt.ps1'
        runnerVersion = '1'
        childProvenance = 'bound'
        children = @($combinedGeneralsChild, $combinedZeroHourChild)
    }
    details = [ordered]@{
        pipelineMode = [string]$generalsProjectionPolicy.pipelineMode
        simulationMode = [string]$generalsProjectionPolicy.simulationMode
        requestedWorkers = [string]$generalsProjectionPolicy.requestedWorkers
        workerPolicy = [string]$generalsProjectionPolicy.workerPolicy
        projectionSequence = 1
        projectionSemantics = 'deterministic-lineage-pointer'
        sourceChildCount = 253
        bothTitlesPassed = $true
        sourceCorpora = @($stagedCorpora['Generals'], $stagedCorpora['ZeroHour'])
    }
}
Assert-CombinedNoReparsePath $outputDirectory 'Combined output directory before receipt creation'
$combinedReceiptBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
    ($combinedDocument | ConvertTo-Json -Depth 16))
$combinedReceiptSnapshot = Write-Stage5FinalAcceptanceFileAtomically `
    $outputFull $combinedReceiptBytes 'Combined receipt'
$combinedHash = [string]$combinedReceiptSnapshot.sha256
$combinedSeenRunNonces = @{}
$combinedRead = Read-Stage5FinalAcceptanceImmutableReceipt `
    -Path $outputFull -Kind 'combined-stage4-stage5-installed-runtime' `
    -Role 'combined-results' -EvidenceTitle 'Both' `
    -ExpectedSourceCommit $sourceCommit `
    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -SeenRunNonces $combinedSeenRunNonces `
    -ExpectedEvidenceSha256 $combinedHash `
    -ExpectedCohortNonce $sourceCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
    -ExpectedRuntimeClosure $sourceRuntimeClosure
Assert-CombinedCondition ([string]$combinedRead.trustDomain -ceq 'host-runner' -and
    [string]$combinedRead.producer -ceq 'installed-runtime-combined-results-v2') `
    'Combined producer self-validation did not return the allowlisted host-runner v2 receipt.'
Write-Output "Generated combined-results v2 host receipt for $sourceCommit with distinct Generals/ZeroHour source nonces."
