#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AcceptanceTemplateRoot,
    [Parameter(Mandatory = $true)][string]$AcceptanceTemplatePath,
    [Parameter(Mandatory = $true)][string]$GeneralsEvidenceRoot,
    [Parameter(Mandatory = $true)][string]$ZeroHourEvidenceRoot,
    [Parameter(Mandatory = $true)][string]$CombinedEvidenceRoot,
    [Parameter(Mandatory = $true)][string]$LockstepEvidenceRoot,
    [Parameter(Mandatory = $true)][string]$PerformanceEvidenceRoot,
    [string]$InstalledKernelEvidencePath = '',
    [switch]$ExternalQualificationExempt,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
    [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
    [Parameter(Mandatory = $true)][string]$ExpectedPhaseBaselineProfileSha256
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

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
    $base = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    $candidate = [IO.Path]::GetFullPath($Path)
    Assert-Stage5FinalAcceptancePathContained $base $candidate $Context
    $relative = $candidate.Substring($base.Length).TrimStart([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($relative) -and
        -not [IO.Path]::IsPathRooted($relative) -and $relative -notmatch ':') `
        "$Context did not produce a safe relative path."
    return $relative
}

function Resolve-RelativeFile {
    param([string]$BaseDirectory, [string]$RelativePath, [string]$Context)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($RelativePath) -and
        -not [IO.Path]::IsPathRooted($RelativePath) -and
        $RelativePath -notmatch '^[A-Za-z]:' -and $RelativePath -notmatch ':' -and
        $RelativePath -notmatch '(^|[\\/])\.\.([\\/]|$)' -and
        $RelativePath -notmatch '(^|[\\/])\.([\\/]|$)') `
        "$Context path is not a canonical relative path."
    $candidate = [IO.Path]::GetFullPath((Join-Path $BaseDirectory $RelativePath))
    Assert-Stage5FinalAcceptanceNoReparsePath $BaseDirectory $candidate $Context
    Assert-Condition (Test-Path -LiteralPath $candidate -PathType Leaf) `
        "$Context file is missing: $candidate"
    return $candidate
}

function Get-BoundedRegularFiles {
    param([string]$Root, [string]$Context)
    $rootFull = [IO.Path]::GetFullPath($Root)
    $rootItem = Get-Item -LiteralPath $rootFull -Force -ErrorAction Stop
    Assert-Condition ($rootItem.PSIsContainer -and
        ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context must be a regular directory."
    $directories = New-Object 'Collections.Generic.Queue[string]'
    $directories.Enqueue($rootFull)
    $files = New-Object 'Collections.Generic.List[string]'
    $directoryCount = 0
    [Int64]$totalBytes = 0
    while ($directories.Count -gt 0) {
        $directory = $directories.Dequeue()
        ++$directoryCount
        Assert-Condition ($directoryCount -le 4096) `
            "$Context exceeds the 4096-directory inspection bound."
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)) {
            Assert-Condition (($item.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -eq 0) `
                "$Context contains a reparse-point alias '$($item.FullName)'."
            if ($item.PSIsContainer) {
                $directories.Enqueue([IO.Path]::GetFullPath($item.FullName))
            }
            else {
                $files.Add([IO.Path]::GetFullPath($item.FullName)) | Out-Null
                $totalBytes += [Int64]$item.Length
                Assert-Condition ($files.Count -le 16384 -and
                    $totalBytes -le [Int64]34359738368) `
                    "$Context exceeds its bounded file-count or 32-GiB byte closure."
            }
        }
    }
    return $files.ToArray()
}

function Find-ExactlyOneFile {
    param([string]$Root, [string]$LeafName, [string]$Context)
    $matches = @(Get-BoundedRegularFiles $Root $Context | Where-Object {
        [IO.Path]::GetFileName([string]$_) -ceq $LeafName
    })
    Assert-Condition ($matches.Count -eq 1) `
        "$Context requires exactly one '$LeafName', found $($matches.Count)."
    return [string]$matches[0]
}

function Copy-ExactFile {
    param(
        [string]$SourceRoot,
        [string]$SourcePath,
        [string]$DestinationPath,
        [string]$Context
    )
    $sourceFull = [IO.Path]::GetFullPath($SourcePath)
    $destinationFull = [IO.Path]::GetFullPath($DestinationPath)
    Assert-Stage5FinalAcceptanceNoReparsePath $SourceRoot $sourceFull $Context
    Assert-Condition (-not (Test-Path -LiteralPath $destinationFull)) `
        "$Context destination already exists: $destinationFull"
    $destinationDirectory = Split-Path -Parent $destinationFull
    [IO.Directory]::CreateDirectory($destinationDirectory) | Out-Null
    Assert-Stage5FinalAcceptanceNoReparsePath $script:ReadinessRoot `
        $destinationDirectory "$Context destination directory"
    $source = [IO.FileStream]::new($sourceFull, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576,
        [IO.FileOptions]::SequentialScan)
    $sha = [Security.Cryptography.SHA256]::Create()
    $destinationHash = $null
    try {
        Assert-Stage5FinalAcceptanceFileHandlePath $source $sourceFull `
            "$Context source" | Out-Null
        $sourceHash = (($sha.ComputeHash($source) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
        $source.Position = 0
        $destination = [IO.FileStream]::new($destinationFull, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 1048576,
            [IO.FileOptions]::WriteThrough)
        try {
            Assert-Stage5FinalAcceptanceFileHandlePath $destination $destinationFull `
                "$Context destination" | Out-Null
            $source.CopyTo($destination, 1048576)
            $destination.Flush($true)
            Assert-Condition ($destination.Length -eq $source.Length) `
                "$Context copied length differs from its locked source stream."
            $destination.Position = 0
            $destinationSha = [Security.Cryptography.SHA256]::Create()
            try {
                $destinationHash = (($destinationSha.ComputeHash($destination) |
                    ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
            }
            finally { $destinationSha.Dispose() }
        }
        finally { $destination.Dispose() }
    }
    finally {
        $sha.Dispose()
        $source.Dispose()
    }
    Assert-Condition ($destinationHash -ceq $sourceHash) `
        "$Context copied bytes do not match while both file handles are locked."
    return $destinationHash
}

function Copy-EvidenceTree {
    param([string]$SourceRoot, [string]$DestinationRoot, [string]$Context)
    $separators = [char[]]@([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $sourceFull = [IO.Path]::GetFullPath($SourceRoot).TrimEnd($separators)
    $destinationFull = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd($separators)
    $sourcePrefix = $sourceFull + [IO.Path]::DirectorySeparatorChar
    $destinationPrefix = $destinationFull + [IO.Path]::DirectorySeparatorChar
    Assert-Condition (-not $destinationFull.Equals($sourceFull,
            [StringComparison]::OrdinalIgnoreCase) -and
        -not $destinationFull.StartsWith($sourcePrefix,
            [StringComparison]::OrdinalIgnoreCase) -and
        -not $sourceFull.StartsWith($destinationPrefix,
            [StringComparison]::OrdinalIgnoreCase)) `
        "$Context source and destination trees must not overlap."
    Assert-Condition (-not (Test-Path -LiteralPath $destinationFull)) `
        "$Context destination must be fresh: $destinationFull"
    [IO.Directory]::CreateDirectory($destinationFull) | Out-Null
    Assert-Stage5FinalAcceptanceNoReparsePath $script:ReadinessRoot `
        $destinationFull "$Context destination root"
    foreach ($source in @(Get-BoundedRegularFiles $sourceFull $Context)) {
        $relative = Get-ContainedRelativePath $sourceFull $source "$Context source"
        [void](Copy-ExactFile $sourceFull $source (Join-Path $destinationFull $relative) `
            "$Context '$relative'")
    }
}

function Read-JsonEvidence {
    param([string]$Path, [string]$Context)
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $Path $Context
    return [pscustomobject]@{
        snapshot = $snapshot
        document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot $Context
    }
}

function Assert-CurrentReceipt {
    param([object]$Evidence, [string]$Role, [string]$Title, [string]$Context)
    $document = $Evidence.document
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind', 'status',
        'role', 'trustDomain', 'producer', 'producerVersion', 'runNonce',
        'sourceCommit', 'title', 'architecture', 'artifactSetSha256',
        'cohortNonce', 'runtimeClosure', 'executableSha256', 'recordedUtc',
        'rawLogs', 'provenance', 'details') $Context
    $schemaVersion = Get-Stage5JsonValue $document 'schemaVersion' $Context
    Assert-Condition ((Test-Stage5JsonInteger $schemaVersion) -and
        $schemaVersion -eq 1 -and
        (Get-Stage5JsonValue $document 'evidenceKind' $Context) -ceq
            'stage5-host-runner-receipt' -and
        (Get-Stage5JsonValue $document 'status' $Context) -ceq 'passed' -and
        (Get-Stage5JsonValue $document 'role' $Context) -ceq $Role -and
        (Get-Stage5JsonValue $document 'trustDomain' $Context) -ceq 'host-runner' -and
        (Get-Stage5JsonValue $document 'producer' $Context) -ceq
            "installed-runtime-$Role-v2" -and
        (Get-Stage5JsonValue $document 'producerVersion' $Context) -ceq '2' -and
        (Get-Stage5JsonValue $document 'sourceCommit' $Context) -ceq
            $ExpectedSourceCommit -and
        (Get-Stage5JsonValue $document 'title' $Context) -ceq $Title -and
        (Get-Stage5JsonValue $document 'architecture' $Context) -ceq 'x64' -and
        ([string](Get-Stage5JsonValue $document 'artifactSetSha256' $Context)).ToUpperInvariant() -ceq
            $script:ArtifactSetSha256 -and
        (Get-Stage5JsonValue $document 'cohortNonce' $Context) -ceq
            $ExpectedCohortNonce) `
        "$Context is stale, substituted, or outside the current execution cohort."
    $closure = Get-Stage5JsonValue $document 'runtimeClosure' $Context
    Assert-Stage5JsonShape $closure @('dependencyManifestSha256', 'closureSha256') `
        "$Context runtime closure"
    Assert-Condition (([string]$closure.dependencyManifestSha256).ToUpperInvariant() -ceq
            $script:RuntimeClosure.dependencyManifestSha256 -and
        ([string]$closure.closureSha256).ToUpperInvariant() -ceq
            $script:RuntimeClosure.closureSha256) `
        "$Context runtime closure is stale or substituted."
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    Assert-Condition ([DateTimeOffset]::TryParse(
            [string](Get-Stage5JsonValue $document 'recordedUtc' $Context),
            [ref]$recorded) -and $recorded -ge $script:CohortCreated) `
        "$Context predates the current execution cohort."
    return Get-Stage5JsonValue $document 'details' $Context
}

function Get-BoundRawEvidence {
    param([string]$ReceiptPath, [object]$Receipt, [string]$LeafName,
        [string]$Context)
    $rawLogs = Get-Stage5JsonValue $Receipt 'rawLogs' $Context
    $matches = @($rawLogs | Where-Object {
        $_ -is [Collections.IDictionary] -and
        [IO.Path]::GetFileName([string]$_.path) -ceq $LeafName
    })
    Assert-Condition ($matches.Count -eq 1) `
        "$Context requires exactly one byte-bound '$LeafName' raw document."
    $binding = $matches[0]
    Assert-Stage5JsonShape $binding @('name', 'path', 'sha256') "$Context binding"
    $path = Resolve-RelativeFile (Split-Path -Parent $ReceiptPath) `
        ([string]$binding.path) "$Context '$LeafName'"
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path "$Context '$LeafName'"
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot ([string]$binding.sha256) `
        "$Context '$LeafName'" | Out-Null
    return [pscustomobject]@{
        path = $path
        snapshot = $snapshot
        binding = $binding
        document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot `
            "$Context '$LeafName'"
    }
}

function Get-BoundRawJson {
    param([string]$ReceiptPath, [object]$Receipt, [string]$LeafName,
        [string]$Context)
    return (Get-BoundRawEvidence $ReceiptPath $Receipt $LeafName $Context).document
}

function Get-BoundRawEvidenceSet {
    param([string]$ReceiptPath, [object]$Receipt, [string]$Context)
    $rawLogs = Get-Stage5JsonValue $Receipt 'rawLogs' $Context
    Assert-Condition ($rawLogs.Count -gt 0) "$Context has no retained raw logs."
    $seenNames = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    $seenPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $records = New-Object 'Collections.Generic.List[object]'
    foreach ($rawLog in $rawLogs) {
        Assert-Stage5JsonShape $rawLog @('name', 'path', 'sha256') `
            "$Context raw log"
        $name = [string](Get-Stage5JsonValue $rawLog 'name' "$Context raw log")
        $path = Resolve-RelativeFile (Split-Path -Parent $ReceiptPath) `
            ([string](Get-Stage5JsonValue $rawLog 'path' "$Context raw log")) `
            "$Context raw log '$name'"
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($name) -and
            $seenNames.Add($name) -and $seenPaths.Add($path)) `
            "$Context raw-log names and paths must be unique."
        $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path `
            "$Context raw log '$name'" -EvidenceKind RawLog
        $sha256 = Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
            ([string](Get-Stage5JsonValue $rawLog 'sha256' "$Context raw log")) `
            "$Context raw log '$name'"
        $records.Add([pscustomobject]@{
            name = $name; path = $path; sha256 = $sha256; snapshot = $snapshot
        }) | Out-Null
    }
    return ,$records.ToArray()
}

function Get-ResultTreeSha256 {
    param([object[]]$Results, [ValidateSet('replay', 'ai')][string]$Kind)
    $lines = New-Object 'Collections.Generic.List[string]'
    $orderedResults = @($Results | Where-Object {
                [string]$_.kind -ceq $Kind
            } | Sort-Object -Property @{
                Expression = { [Int64]$_.sequence }
                Ascending = $true
            })
    foreach ($result in $orderedResults) {
        if ($Kind -ceq 'replay') {
            $lines.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f
                $result.sequence, $result.determinismKey, $result.matrixRepeat,
                $result.repeat, $result.replayResult.finalFrame,
                $result.replayResult.finalCRC)) | Out-Null
        }
        else {
            $lines.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f
                $result.sequence, $result.scenario, $result.seed,
                $result.configuration, $result.repeat,
                $result.aiEvidence.finalDigest)) | Out-Null
        }
    }
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
        (($lines.ToArray() -join "`n") + "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function New-Attachment {
    param([string]$Role, [string]$Title, [string]$Path, [string]$TrustDomain)
    return [ordered]@{
        role = $Role
        title = $Title
        path = Get-ContainedRelativePath $script:ReadinessRoot $Path `
            "Final acceptance attachment '$Role|$Title'"
        sha256 = Get-Stage5FileSha256 $Path
        trustDomain = $TrustDomain
    }
}

function Write-EvidenceEnvelope {
    param([string]$Kind, [string]$Title, [object[]]$Attachments,
        [Collections.IDictionary]$Details)
    $path = Join-Path $script:ReadinessRoot "$Kind.json"
    $document = [ordered]@{
        schemaVersion = 1
        evidenceKind = $Kind
        status = 'passed'
        sourceCommit = $ExpectedSourceCommit
        title = $Title
        architecture = 'x64'
        artifactSetSha256 = $script:ArtifactSetSha256
        recordedUtc = $script:AssemblyRecordedUtc
        cohortNonce = $ExpectedCohortNonce
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = $script:RuntimeClosure.dependencyManifestSha256
            closureSha256 = $script:RuntimeClosure.closureSha256
        }
        attachments = @($Attachments)
        details = $Details
    }
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
        ($document | ConvertTo-Json -Depth 64))
    $snapshot = Write-Stage5FinalAcceptanceFileAtomically -Path $path -Bytes $bytes `
        -Context "Stage 5 '$Kind' development-readiness envelope"
    return [pscustomobject]@{ path = $path; sha256 = [string]$snapshot.sha256 }
}

function New-Stage5DeterministicRuntimeEnvelope {
    param(
        [string]$ValidationPlanReceiptPath,
        [string]$ValidationResultsReceiptPath,
        [string]$InstalledKernelAttachmentPath,
        [string[]]$RequiredWorkers,
        [string]$ReplayEvidenceSha256,
        [string]$FreshAiEvidenceSha256,
        [string]$PerformanceScalingEvidenceSha256
    )
    $runtimeAttachments = New-Object 'Collections.Generic.List[object]'
    $runtimeAttachments.Add((New-Attachment -Role 'validation-plan' `
            -Title 'ZeroHour' -Path $ValidationPlanReceiptPath `
            -TrustDomain 'host-runner')) | Out-Null
    $runtimeAttachments.Add((New-Attachment -Role 'validation-results' `
            -Title 'ZeroHour' -Path $ValidationResultsReceiptPath `
            -TrustDomain 'host-runner')) | Out-Null
    if (-not [string]::IsNullOrWhiteSpace($InstalledKernelAttachmentPath)) {
        $runtimeAttachments.Add((New-Attachment `
                -Role 'installed-kernel-execution' -Title 'Both' `
                -Path $InstalledKernelAttachmentPath `
                -TrustDomain 'installed-runtime')) | Out-Null
    }

    return Write-EvidenceEnvelope 'deterministic-runtime' 'ZeroHour' `
        $runtimeAttachments.ToArray() ([ordered]@{
        gateName = 'deterministic-runtime'
        isolatedPipelineMode = 'serial'
        simulationModes = @('serial', 'parallel', 'shadow')
        workerConfigurations = $RequiredWorkers
        isolatedMatrixPassed = $true
        finalAcceptanceClaim = $false
        replayEvidenceSha256 = $ReplayEvidenceSha256
        freshAiEvidenceSha256 = $FreshAiEvidenceSha256
        performanceEvidenceSha256 = $PerformanceScalingEvidenceSha256
        installedKernelExecution = $script:InstalledKernelExecutionStatus
    })
}

function Get-TemplateAttachment {
    param([string]$Kind, [string]$Role, [string]$Title,
        [string]$ExpectedTrustDomain)
    $entry = @($script:TemplateManifest.evidence | Where-Object {
        [string]$_.kind -ceq $Kind
    })
    Assert-Condition ($entry.Count -eq 1) `
        "Acceptance template requires exactly one '$Kind' evidence entry."
    $outerPath = Resolve-RelativeFile $script:TemplateDirectory `
        ([string]$entry[0].path) "Acceptance template '$Kind' evidence"
    $outerEvidence = Read-JsonEvidence $outerPath "Acceptance template '$Kind' evidence"
    Assert-Stage5FinalAcceptanceSnapshotSha256 $outerEvidence.snapshot `
        ([string]$entry[0].sha256) "Acceptance template '$Kind' evidence" | Out-Null
    $outer = $outerEvidence.document
    Assert-Condition ((Get-Stage5JsonValue $outer 'evidenceKind' `
            "Acceptance template '$Kind'") -ceq $Kind -and
        (Get-Stage5JsonValue $outer 'sourceCommit' `
            "Acceptance template '$Kind'") -ceq $ExpectedSourceCommit -and
        ([string](Get-Stage5JsonValue $outer 'artifactSetSha256' `
            "Acceptance template '$Kind'")).ToUpperInvariant() -ceq
            $script:ArtifactSetSha256) `
        "Acceptance template '$Kind' is stale or bound to another candidate."
    $templateAttachments = Get-Stage5JsonValue $outer 'attachments' `
        "Acceptance template '$Kind'"
    $attachment = @($templateAttachments | Where-Object {
        [string]$_.role -ceq $Role -and [string]$_.title -ceq $Title
    })
    Assert-Condition ($attachment.Count -eq 1 -and
        [string]$attachment[0].trustDomain -ceq $ExpectedTrustDomain) `
        "Acceptance template '$Kind' requires exactly one trusted '$Role|$Title' attachment."
    Assert-Stage5JsonShape $attachment[0] @('role', 'title', 'path', 'sha256',
        'trustDomain') "Acceptance template '$Role|$Title' attachment"
    $attachmentPath = Resolve-RelativeFile (Split-Path -Parent $outerPath) `
        ([string]$attachment[0].path) "Acceptance template '$Role|$Title' attachment"
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $attachmentPath `
        "Acceptance template '$Role|$Title' attachment"
    $hash = Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
        ([string]$attachment[0].sha256) `
        "Acceptance template '$Role|$Title' attachment"
    return [pscustomobject]@{ path = $attachmentPath; sha256 = $hash; document =
        ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot `
            "Acceptance template '$Role|$Title' attachment" }
}

function Copy-ReviewedTemplateFile {
    param([string]$SourcePath, [string]$Context)
    $sourceFull = [IO.Path]::GetFullPath($SourcePath)
    $key = $sourceFull.ToLowerInvariant()
    if ($script:ReviewedCopies.ContainsKey($key)) {
        return [string]$script:ReviewedCopies[$key]
    }
    $relative = Get-ContainedRelativePath $script:TemplateDirectory $sourceFull $Context
    $destination = Join-Path $script:ReviewedStageRoot $relative
    [void](Copy-ExactFile $script:TemplateDirectory $sourceFull $destination $Context)
    $script:ReviewedCopies[$key] = $destination
    return $destination
}

function Copy-ReviewedFixtureAuthority {
    param([object]$Receipt, [ValidateSet('Generals', 'ZeroHour')][string]$Title)
    $context = "Reviewed $Title replay-fixture authority"
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $Receipt.path `
        "$context receipt"
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $Receipt.sha256 `
        "$context receipt" | Out-Null
    $read = Read-Stage5FinalAcceptanceImmutableReceipt `
        -Path $Receipt.path -Kind 'replay-determinism' `
        -Role 'replay-fixture-manifest' -EvidenceTitle $Title `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -ExpectedArtifactSetSha256 $script:ArtifactSetSha256 `
        -ArtifactHashes $artifactHashes -ArtifactPaths $artifactPaths `
        -ExpectedEvidenceSha256 $Receipt.sha256 -EvidenceSnapshot $snapshot `
        -ExpectedRuntimeClosure $script:RuntimeClosure
    Assert-Condition ([string]$read.trustDomain -ceq 'reviewed-fixture' -and
        [string]$read.producer -ceq 'reviewed-replay-fixture-manifest-v2' -and
        $null -ne $read.reviewedFixtureManifest -and
        $null -ne $read.reviewedFixtureManifestSnapshot) `
        "$context is not the protected reviewed-fixture authority."
    $receiptDestination = Copy-ReviewedTemplateFile $Receipt.path "$context receipt"
    $protection = Get-Stage5JsonValue $read.document 'protection' "$context receipt"
    $manifestReference = Get-Stage5JsonValue $read.provenance 'fixtureManifest' `
        "$context receipt"
    $receiptDirectory = Split-Path -Parent $Receipt.path
    $attestationPath = Resolve-RelativeFile $receiptDirectory `
        ([string]$protection.path) "$context attestation"
    [void](Copy-ReviewedTemplateFile $attestationPath "$context attestation")
    $manifestPath = Resolve-RelativeFile $receiptDirectory `
        ([string]$manifestReference.path) "$context manifest"
    $manifestDestination = Copy-ReviewedTemplateFile $manifestPath "$context manifest"
    foreach ($fixture in (Get-Stage5JsonValue $read.reviewedFixtureManifest `
            'fixtures' "$context manifest")) {
        $fixturePath = Resolve-RelativeFile (Split-Path -Parent $manifestPath) `
            ([string]$fixture.source) "$context replay"
        [void](Copy-ReviewedTemplateFile $fixturePath "$context replay")
        if (@($fixture.Keys | Where-Object {
                [string]$_ -ceq 'maps'
            }).Count -eq 1) {
            foreach ($map in @($fixture.maps)) {
                $mapPath = Resolve-RelativeFile (Split-Path -Parent $manifestPath) `
                    ([string]$map.source) "$context map"
                [void](Copy-ReviewedTemplateFile $mapPath "$context map")
            }
        }
    }
    Assert-Condition ((Get-Stage5FileSha256 $receiptDestination) -ceq
            [string]$Receipt.sha256 -and
        (Get-Stage5FileSha256 $manifestDestination) -ceq
            [string]$read.reviewedFixtureManifestSnapshot.sha256) `
        "$context copied closure changed while staging."
    return [pscustomobject]@{
        source = $Receipt
        destination = $receiptDestination
        read = $read
        manifestReference = $manifestReference
        manifestPath = $manifestPath
        manifestDestination = $manifestDestination
        manifest = $read.reviewedFixtureManifest
    }
}

function Get-TitleQualificationData {
    param([object]$ReceiptDocument, [string]$Title, [string]$EvidenceRoot,
        [string]$Context)
    $details = Get-Stage5JsonValue $ReceiptDocument 'details' "$Context receipt"
    $binding = Get-Stage5JsonValue $details 'qualificationData' `
        "$Context receipt details"
    Assert-Stage5JsonShape $binding @('path', 'title', 'manifestSha256',
        'closureSha256', 'fileCount') "$Context qualificationData"
    Assert-Condition ([string]$binding.path -ceq 'QualificationData.json' -and
        [string]$binding.title -ceq $Title -and
        [string]$binding.manifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        [string]$binding.closureSha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $binding.fileCount) -and
        [int]$binding.fileCount -eq 6) `
        "$Context qualificationData is malformed, title-swapped, or incomplete."
    $manifestPath = Resolve-RelativeFile $EvidenceRoot ([string]$binding.path) `
        "$Context qualificationData manifest"
    $proof = Read-Stage5SimulationQualificationDataEvidence `
        -Path $manifestPath -Binding $binding `
        -ExpectedSourceCommit $ExpectedSourceCommit -ExpectedTitle $Title
    Assert-Condition ([string]$proof.manifestSha256 -ceq
            [string]$binding.manifestSha256 -and
        [string]$proof.closureSha256 -ceq [string]$binding.closureSha256 -and
        [int]$proof.fileCount -eq 6) `
        "$Context qualificationData manifest is stale or detached."
    return [ordered]@{
        path = 'QualificationData.json'
        title = $Title
        manifestSha256 = [string]$binding.manifestSha256
        closureSha256 = [string]$binding.closureSha256
        fileCount = 6
    }
}

function Assert-QualificationDataEqual {
    param([Collections.IDictionary]$Expected, [object]$Actual, [string]$Context)
    Assert-Stage5JsonShape $Actual @('path', 'title', 'manifestSha256',
        'closureSha256', 'fileCount') $Context
    foreach ($field in @('path', 'title', 'manifestSha256', 'closureSha256',
            'fileCount')) {
        Assert-Condition ([string](Get-Stage5JsonValue $Actual $field $Context) -ceq
            [string]$Expected[$field]) `
            "$Context field '$field' differs from the current title manifest."
    }
}

function Resolve-InstalledKernelQualificationInput {
    param([string]$Path, [switch]$Exempt)
    $hasPath = -not [string]::IsNullOrWhiteSpace($Path)
    Assert-Condition ($hasPath -xor [bool]$Exempt) `
        'Exactly one of InstalledKernelEvidencePath or ExternalQualificationExempt is required.'
    if ($Exempt) {
        return [pscustomobject]@{
            mode = 'external-qualification-exempt'
            path = $null
            status = [ordered]@{
                status = 'skipped'
                claim = $false
                reason = 'external-qualification-exempt-and-reviewed-native-fixture-unavailable'
                sha256 = $null
            }
        }
    }
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Condition ([IO.Path]::GetFileName($full) -ceq
            'Stage5InstalledKernelExecution.json' -and
        (Test-Path -LiteralPath $full -PathType Leaf)) `
        "InstalledKernelEvidencePath must name an existing Stage5InstalledKernelExecution.json: $full"
    Assert-Stage5FinalAcceptanceNoReparsePath (Split-Path -Parent $full) $full `
        'Installed-kernel evidence input'
    return [pscustomobject]@{
        mode = 'installed-kernel-evidence'
        path = $full
        status = $null
    }
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
        [ref]$script:CohortCreated)) `
    'ExpectedCohortCreatedUtc must be a canonical UTC timestamp.'
$script:AssemblyRecordedUtc = [DateTime]::UtcNow.ToString('o',
    [Globalization.CultureInfo]::InvariantCulture)
Assert-Condition ([DateTimeOffset]::UtcNow.AddMinutes(5) -ge $script:CohortCreated) `
    'The requested execution cohort is in the future.'
$installedKernelInput = Resolve-InstalledKernelQualificationInput `
    -Path $InstalledKernelEvidencePath -Exempt:$ExternalQualificationExempt

$outputFull = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
Assert-Condition (Test-Path -LiteralPath $outputFull -PathType Container) `
    "OutputRoot is missing: $outputFull"
$artifactSetPath = Join-Path $outputFull 'Stage5ArtifactSet.json'
Assert-Condition (Test-Path -LiteralPath $artifactSetPath -PathType Leaf) `
    'OutputRoot must contain the exact Stage5ArtifactSet.json qualification manifest.'
$artifactEvidence = Read-JsonEvidence $artifactSetPath 'Stage 5 artifact set'
$artifactSet = $artifactEvidence.document
$script:ArtifactSetSha256 = [string]$artifactEvidence.snapshot.sha256
$runtimeResult = Get-Stage5RuntimeClosureBinding $artifactSet $outputFull `
    $ExpectedSourceCommit 'Stage 5 development-readiness artifact set'
$script:RuntimeClosure = [ordered]@{
    dependencyManifestSha256 = ([string]$runtimeResult.dependencyManifestSha256).ToUpperInvariant()
    closureSha256 = ([string]$runtimeResult.closureSha256).ToUpperInvariant()
}
$artifactHashes = @{}
$artifactPaths = @{}
foreach ($artifact in (Get-Stage5JsonValue $artifactSet 'artifacts' `
        'Stage 5 artifact set')) {
    $role = [string](Get-Stage5JsonValue $artifact 'role' 'Stage 5 artifact')
    $artifactHashes[$role] = ([string](Get-Stage5JsonValue $artifact 'sha256' `
        'Stage 5 artifact')).ToUpperInvariant()
    $artifactPaths[$role] = Resolve-RelativeFile $outputFull `
        ([string](Get-Stage5JsonValue $artifact 'path' 'Stage 5 artifact')) `
        "Stage 5 artifact '$role'"
}

$templateRootFull = [IO.Path]::GetFullPath($AcceptanceTemplateRoot)
if ($templateRootFull.Length -gt [IO.Path]::GetPathRoot($templateRootFull).Length) {
    $templateRootFull = $templateRootFull.TrimEnd([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
}
Assert-Condition (Test-Path -LiteralPath $templateRootFull -PathType Container) `
    "AcceptanceTemplateRoot is missing: $templateRootFull"
$templateVolumeRoot = [IO.Path]::GetPathRoot($templateRootFull)
Assert-Condition (-not [string]::IsNullOrWhiteSpace($templateVolumeRoot) -and
    -not [String]::Equals($templateRootFull, $templateVolumeRoot,
        [StringComparison]::OrdinalIgnoreCase)) `
    'AcceptanceTemplateRoot must be a bounded directory below its volume root.'
Assert-Stage5FinalAcceptanceNoReparsePath $templateVolumeRoot $templateRootFull `
    'Stage 5 trusted acceptance-template root'
Assert-Condition (-not [string]::IsNullOrWhiteSpace($AcceptanceTemplatePath) -and
    -not [IO.Path]::IsPathRooted($AcceptanceTemplatePath) -and
    $AcceptanceTemplatePath -notmatch ':' -and
    $AcceptanceTemplatePath -notmatch '(^|[\\/])\.\.([\\/]|$)' -and
    $AcceptanceTemplatePath -notmatch '(^|[\\/])\.([\\/]|$)') `
    'AcceptanceTemplatePath must be a canonical path relative to AcceptanceTemplateRoot.'
$templateFull = [IO.Path]::GetFullPath((Join-Path $templateRootFull `
    $AcceptanceTemplatePath))
Assert-Stage5FinalAcceptanceNoReparsePath $templateRootFull $templateFull `
    'Stage 5 acceptance template'
Assert-Condition (Test-Path -LiteralPath $templateFull -PathType Leaf) `
    "AcceptanceTemplatePath is missing: $templateFull"
$script:TemplateDirectory = Split-Path -Parent $templateFull
$templateEvidence = Read-JsonEvidence $templateFull 'Stage 5 acceptance template'
$script:TemplateManifest = $templateEvidence.document
Assert-Stage5JsonShape $script:TemplateManifest @('schemaVersion', 'gateName',
    'sourceCommit', 'cohortNonce', 'cohortCreatedUtc', 'artifactSet', 'evidence') `
    'Stage 5 acceptance template'
$templateArtifact = Get-Stage5JsonValue $script:TemplateManifest 'artifactSet' `
    'Stage 5 acceptance template'
Assert-Condition ((Get-Stage5JsonValue $script:TemplateManifest 'sourceCommit' `
        'Stage 5 acceptance template') -ceq $ExpectedSourceCommit -and
    ([string](Get-Stage5JsonValue $templateArtifact 'sha256' `
        'Stage 5 acceptance template artifact set')).ToUpperInvariant() -ceq
        $script:ArtifactSetSha256) `
    'Acceptance template is bound to a different source commit or artifact set.'

$lockstepSourcePath = Find-ExactlyOneFile $LockstepEvidenceRoot `
    'LockstepV2LoopbackEvidence.json' 'Lockstep-v2 qualification root'
$lockstepSourceTree = Split-Path -Parent $lockstepSourcePath
$readinessCandidate = Join-Path $outputFull 'DevelopmentReadiness'
Assert-Condition (-not (Test-Path -LiteralPath $readinessCandidate)) `
    "Development-readiness output must be fresh: $readinessCandidate"
$script:ReadinessRoot = $readinessCandidate
[IO.Directory]::CreateDirectory($script:ReadinessRoot) | Out-Null
$sourcesRoot = Join-Path $script:ReadinessRoot 'Sources'
[IO.Directory]::CreateDirectory($sourcesRoot) | Out-Null

$generalsStage = Join-Path $sourcesRoot 'Generals'
$zeroHourStage = Join-Path $sourcesRoot 'ZeroHour'
$combinedStage = Join-Path $sourcesRoot 'Combined'
$lockstepStage = Join-Path $sourcesRoot 'Lockstep'
$performanceStage = Join-Path $sourcesRoot 'Performance'
$script:ReviewedStageRoot = Join-Path $sourcesRoot 'ReviewedTemplate'
Copy-EvidenceTree $GeneralsEvidenceRoot $generalsStage 'Generals title evidence'
Copy-EvidenceTree $ZeroHourEvidenceRoot $zeroHourStage 'Zero Hour title evidence'
Copy-EvidenceTree $CombinedEvidenceRoot $combinedStage 'Combined title evidence'
Copy-EvidenceTree $lockstepSourceTree $lockstepStage 'Lockstep-v2 evidence closure'
Copy-EvidenceTree $PerformanceEvidenceRoot $performanceStage `
    'External performance evidence closure'
$installedKernelAttachmentPath = $null
if ($installedKernelInput.mode -ceq 'installed-kernel-evidence') {
    $installedKernelStage = Join-Path $sourcesRoot 'InstalledKernel'
    Copy-EvidenceTree (Split-Path -Parent $installedKernelInput.path) `
        $installedKernelStage 'Installed-kernel execution evidence closure'
    $installedKernelAttachmentPath = Find-ExactlyOneFile $installedKernelStage `
        'Stage5InstalledKernelExecution.json' `
        'Staged installed-kernel execution evidence'
    $installedKernelSha256 = Get-Stage5FileSha256 $installedKernelAttachmentPath
    $installedKernelModule = Join-Path $PSScriptRoot `
        'Stage5InstalledKernelExecutionEvidence.psm1'
    Assert-Condition (Test-Path -LiteralPath $installedKernelModule -PathType Leaf) `
        "Installed-kernel evidence module is missing: $installedKernelModule"
    Import-Module $installedKernelModule -Force
    $installedKernelProof = Read-Stage5InstalledKernelExecutionEvidence `
        -Path $installedKernelAttachmentPath `
        -ExpectedSha256 $installedKernelSha256 `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -ExpectedArtifactSetSha256 $script:ArtifactSetSha256 `
        -ExpectedCohortNonce $ExpectedCohortNonce `
        -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
        -ExpectedDependencyManifestSha256 `
            $script:RuntimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $script:RuntimeClosure.closureSha256 `
        -GeneralsExecutableSha256 $artifactHashes['generals-executable'] `
        -ZeroHourExecutableSha256 $artifactHashes['zerohour-executable']
    Assert-Condition ([string]$installedKernelProof.sha256 -ceq
            $installedKernelSha256 -and
        -not [bool]$installedKernelProof.finalAcceptanceClaim -and
        -not [bool]$installedKernelProof.performanceScalingClaim) `
        'Installed-kernel execution evidence is detached or overclaims final/scaling acceptance.'
    $script:InstalledKernelExecutionStatus = [ordered]@{
        status = 'passed'
        claim = $true
        reason = $null
        sha256 = $installedKernelSha256
    }
}
else {
    $script:InstalledKernelExecutionStatus = $installedKernelInput.status
}
[IO.Directory]::CreateDirectory($script:ReviewedStageRoot) | Out-Null
$script:ReviewedCopies = @{}

$receiptSpecs = @(
    [pscustomobject]@{ key = 'generalsPlan'; root = $generalsStage
        leaf = 'validation-plan-receipt.json'; role = 'validation-plan'; title = 'Generals' },
    [pscustomobject]@{ key = 'generalsValidation'; root = $generalsStage
        leaf = 'validation-results-receipt.json'; role = 'validation-results'; title = 'Generals' },
    [pscustomobject]@{ key = 'generalsReplay'; root = $generalsStage
        leaf = 'replay-results-receipt.json'; role = 'replay-results'; title = 'Generals' },
    [pscustomobject]@{ key = 'generalsAi'; root = $generalsStage
        leaf = 'ai-results-receipt.json'; role = 'ai-results'; title = 'Generals' },
    [pscustomobject]@{ key = 'plan'; root = $zeroHourStage
        leaf = 'validation-plan-receipt.json'; role = 'validation-plan'; title = 'ZeroHour' },
    [pscustomobject]@{ key = 'validation'; root = $zeroHourStage
        leaf = 'validation-results-receipt.json'; role = 'validation-results'; title = 'ZeroHour' },
    [pscustomobject]@{ key = 'replay'; root = $zeroHourStage
        leaf = 'replay-results-receipt.json'; role = 'replay-results'; title = 'ZeroHour' },
    [pscustomobject]@{ key = 'ai'; root = $zeroHourStage
        leaf = 'ai-results-receipt.json'; role = 'ai-results'; title = 'ZeroHour' },
    [pscustomobject]@{ key = 'combined'; root = $combinedStage
        leaf = 'combined-results-receipt.json'; role = 'combined-results'; title = 'Both' }
)
$receipts = @{}
foreach ($spec in $receiptSpecs) {
    $path = Find-ExactlyOneFile $spec.root $spec.leaf `
        "Stage 5 $($spec.key) receipt"
    $evidence = Read-JsonEvidence $path "Stage 5 $($spec.key) receipt"
    $receipts[$spec.key] = [pscustomobject]@{
        path = $path; evidence = $evidence; spec = $spec
    }
}

$titleCorpora = [ordered]@{
    Generals = [pscustomobject]@{
        root = $generalsStage; validationKey = 'generalsValidation'
        receiptKeys = @('generalsPlan', 'generalsValidation', 'generalsReplay',
            'generalsAi')
        executableRole = 'generals-executable'
    }
    ZeroHour = [pscustomobject]@{
        root = $zeroHourStage; validationKey = 'validation'
        receiptKeys = @('plan', 'validation', 'replay', 'ai')
        executableRole = 'zerohour-executable'
    }
}
$qualificationDataByTitle = @{}
foreach ($title in @('Generals', 'ZeroHour')) {
    $corpus = $titleCorpora[$title]
    $qualificationDataByTitle[$title] = Get-TitleQualificationData `
        -ReceiptDocument $receipts[$corpus.validationKey].evidence.document `
        -Title $title -EvidenceRoot $corpus.root `
        -Context "Stage 5 $title validation-results receipt"
    foreach ($key in @($corpus.receiptKeys)) {
        $details = Get-Stage5JsonValue $receipts[$key].evidence.document 'details' `
            "Stage 5 $key receipt"
        $receiptQualificationData = Get-Stage5JsonValue $details `
            'qualificationData' "Stage 5 $key receipt details"
        Assert-QualificationDataEqual $qualificationDataByTitle[$title] `
            $receiptQualificationData "Stage 5 $key receipt qualificationData"
    }
}

$seenReceiptRunNonces = @{}
foreach ($spec in $receiptSpecs) {
    $record = $receipts[$spec.key]
    $kind = if ($spec.role -ceq 'replay-results') { 'replay-determinism' }
        elseif ($spec.role -ceq 'ai-results') { 'fresh-ai' }
        elseif ($spec.role -ceq 'combined-results') {
            'combined-stage4-stage5-installed-runtime'
        }
        else { 'deterministic-runtime' }
    $arguments = [ordered]@{
        Path = $record.path
        Kind = $kind
        Role = $spec.role
        EvidenceTitle = $spec.title
        ExpectedSourceCommit = $ExpectedSourceCommit
        ExpectedArtifactSetSha256 = $script:ArtifactSetSha256
        ArtifactHashes = $artifactHashes
        ArtifactPaths = $artifactPaths
        SeenRunNonces = $seenReceiptRunNonces
        ExpectedEvidenceSha256 = [string]$record.evidence.snapshot.sha256
        EvidenceSnapshot = $record.evidence.snapshot
        ExpectedCohortNonce = $ExpectedCohortNonce
        ExpectedCohortCreatedUtc = $ExpectedCohortCreatedUtc
        ExpectedRuntimeClosure = $script:RuntimeClosure
        ExpectedEvidenceDirectory = $spec.root
    }
    if ($spec.title -cne 'Both') {
        $arguments['ExpectedQualificationData'] =
            $qualificationDataByTitle[$spec.title]
    }
    if ($spec.role -in @('validation-results', 'replay-results', 'ai-results')) {
        $relocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
            -Path $record.path -EvidenceDirectory $spec.root
        if ($spec.role -ceq 'validation-results') {
            $arguments['NativeRelocationBindings'] = @($relocation.children)
        }
        else {
            $arguments['NativeRawBindings'] = $relocation.nativeRawBindings
            $arguments['NativeReceiptSourcePath'] =
                $relocation.nativeReceiptSourcePath
        }
    }
    $read = Read-Stage5FinalAcceptanceImmutableReceipt @arguments
    $record | Add-Member -NotePropertyName read -NotePropertyValue $read
    $record | Add-Member -NotePropertyName details -NotePropertyValue $read.details
    if ($spec.title -cne 'Both') {
        Assert-QualificationDataEqual $qualificationDataByTitle[$spec.title] `
            $read.qualificationData "Stage 5 $($spec.key) validated qualificationData"
    }
}

$combinedSources = @($receipts.combined.read.combinedSourceBindings.sourceCorpora)
Assert-Condition ($combinedSources.Count -eq 2 -and
    [string]$combinedSources[0].title -ceq 'Generals' -and
    [string]$combinedSources[1].title -ceq 'ZeroHour') `
    'Combined receipt must contain the exact ordered Generals/ZeroHour sourceCorpora.'
foreach ($title in @('Generals', 'ZeroHour')) {
    $combinedSource = @($combinedSources | Where-Object {
        [string]$_.title -ceq $title
    })
    Assert-Condition ($combinedSource.Count -eq 1 -and
        [int]$combinedSource[0].sourceChildCount -eq 253) `
        "Combined receipt does not bind the complete current $title corpus."
    Assert-QualificationDataEqual $qualificationDataByTitle[$title] `
        $combinedSource[0].qualificationData `
        "Combined $title sourceCorpora qualificationData"
    foreach ($key in @($titleCorpora[$title].receiptKeys)) {
        $role = [string]$receipts[$key].spec.role
        $combinedReceipt = $combinedSource[0].receipts[$role]
        Assert-Condition ($null -ne $combinedReceipt -and
            [string]$combinedReceipt.sha256 -ceq
                [string]$receipts[$key].read.sha256 -and
            [string]$combinedReceipt.runNonce -ceq
                [string]$receipts[$key].read.runNonce) `
            "Combined receipt is not byte- and nonce-bound to the current $title $role receipt."
    }
}

$planEvidence = Get-BoundRawEvidence $receipts.plan.path `
    $receipts.plan.evidence.document 'validation-plan.json' `
    'Current validation plan receipt'
$planDocument = $planEvidence.document
$planEntries = Get-Stage5JsonValue $planDocument 'entries' `
    'Current validation plan'
Assert-Condition ($planEntries.Count -gt 0) 'Current validation plan is empty.'
Assert-Condition ([int]$receipts.plan.details.entryCount -eq $planEntries.Count) `
    'Current validation-plan receipt entry count does not match its byte-bound plan.'
$resultsEvidence = Get-BoundRawEvidence $receipts.validation.path `
    $receipts.validation.evidence.document 'validation-results.json' `
    'Current validation-results receipt'
$resultEntries = @($resultsEvidence.document)
Assert-Condition ($resultEntries.Count -eq $planEntries.Count -and
    [int]$receipts.validation.details.resultCount -eq $planEntries.Count -and
    ([string]$receipts.validation.details.resultsSha256).ToUpperInvariant() -ceq
        ([string]$resultsEvidence.snapshot.sha256).ToUpperInvariant()) `
    'Current validation results are not the complete byte-bound execution of the validation plan.'
foreach ($receiptKey in @('replay', 'ai')) {
    $roleResults = Get-BoundRawEvidence $receipts[$receiptKey].path `
        $receipts[$receiptKey].evidence.document 'validation-results.json' `
        "Current $receiptKey receipt"
    Assert-Condition (([string]$roleResults.snapshot.sha256).ToUpperInvariant() -ceq
            ([string]$resultsEvidence.snapshot.sha256).ToUpperInvariant()) `
        "Current $receiptKey receipt is detached from the complete validation result set."
    $receipts[$receiptKey] | Add-Member -NotePropertyName resultsEvidence `
        -NotePropertyValue $roleResults
}
$receipts.validation | Add-Member -NotePropertyName resultsEvidence `
    -NotePropertyValue $resultsEvidence
$resultsBySequence = @{}
foreach ($result in $resultEntries) {
    $sequenceValue = Get-Stage5JsonValue $result 'sequence' `
        'Current validation result'
    Assert-Condition (Test-Stage5JsonInteger $sequenceValue) `
        'Current validation result sequence must be an integer.'
    $sequence = [int]$sequenceValue
    Assert-Condition ($sequence -ge 1 -and $sequence -le $planEntries.Count -and
        -not $resultsBySequence.ContainsKey($sequence)) `
        'Current validation results contain a missing, duplicate, or out-of-range sequence.'
    $exitCodeValue = Get-Stage5JsonValue $result 'exitCode' `
        'Current validation result'
    Assert-Condition (Test-Stage5JsonInteger $exitCodeValue) `
        "Current validation result sequence $sequence exitCode must be an integer."
    $timedOutValue = Get-Stage5JsonValue $result 'timedOut' `
        'Current validation result'
    Assert-Condition ($timedOutValue -is [bool]) `
        "Current validation result sequence $sequence timedOut must be Boolean."
    Assert-Condition ([int]$exitCodeValue -eq 0 -and
        -not [bool]$timedOutValue) `
        "Current validation result sequence $sequence did not complete successfully."
    $resultsBySequence[$sequence] = $result
}
$executionIdentityFields = @('kind', 'caseId', 'determinismKey', 'configuration',
    'simulationMode', 'requestedWorkers', 'workerPolicy', 'repeat', 'matrixRepeat',
    'replayArgument', 'seed', 'scenario', 'fixtureSha256', 'stress')
foreach ($planEntry in $planEntries) {
    $sequenceValue = Get-Stage5JsonValue $planEntry 'sequence' `
        'Current validation plan entry'
    Assert-Condition (Test-Stage5JsonInteger $sequenceValue) `
        'Current validation plan sequence must be an integer.'
    $sequence = [int]$sequenceValue
    Assert-Condition ($resultsBySequence.ContainsKey($sequence)) `
        "Current validation plan sequence $sequence has no completed result."
    $result = $resultsBySequence[$sequence]
    foreach ($field in $executionIdentityFields) {
        $planned = Get-Stage5JsonValue $planEntry $field `
            "Current validation plan sequence $sequence"
        $observed = Get-Stage5JsonValue $result $field `
            "Current validation result sequence $sequence"
        Assert-Condition ([string]$planned -ceq [string]$observed) `
            "Current validation result sequence $sequence changed planned field '$field'."
    }
}
$simulationModes = @($planEntries | ForEach-Object {
    [string](Get-Stage5JsonValue $_ 'simulationMode' 'Current validation plan entry')
} | Sort-Object -CaseSensitive -Unique)
Assert-Condition (($simulationModes -join '|') -ceq 'parallel|serial|shadow') `
    'Current validation plan does not contain the exact serial, parallel, and shadow modes.'
foreach ($entry in $planEntries) {
    $arguments = Get-Stage5JsonValue $entry 'arguments' `
        'Current validation plan entry'
    $pipelineIndex = [Array]::IndexOf([object[]]$arguments, '-pipelineMode')
    $simulationIndex = [Array]::IndexOf([object[]]$arguments, '-simulationMode')
    $workerPolicyIndex = [Array]::IndexOf([object[]]$arguments, '-workerPolicy')
    Assert-Condition ($pipelineIndex -ge 0 -and
        $pipelineIndex + 1 -lt $arguments.Count -and
        [string]$arguments[$pipelineIndex + 1] -ceq 'serial' -and
        $simulationIndex -ge 0 -and
        $simulationIndex + 1 -lt $arguments.Count -and
        [string]$arguments[$simulationIndex + 1] -ceq
            [string](Get-Stage5JsonValue $entry 'simulationMode' `
                'Current validation plan entry') -and
        $workerPolicyIndex -ge 0 -and
        $workerPolicyIndex + 1 -lt $arguments.Count -and
        [string]$arguments[$workerPolicyIndex + 1] -ceq 'auto' -and
        [Array]::LastIndexOf([object[]]$arguments, '-pipelineMode') -eq
            $pipelineIndex -and
        [Array]::LastIndexOf([object[]]$arguments, '-simulationMode') -eq
            $simulationIndex -and
        [Array]::LastIndexOf([object[]]$arguments, '-workerPolicy') -eq
            $workerPolicyIndex) `
        'Current validation plan contains an execution outside the isolated serial pipeline or auto worker policy.'
}
$requiredWorkers = @('serial-1', 'parallel-1', 'parallel-2', 'parallel-4',
    'parallel-8', 'parallel-16', 'parallel-auto')
$workerContracts = @{
    'serial-1' = [pscustomobject]@{ mode = 'serial'; workers = '1' }
    'parallel-1' = [pscustomobject]@{ mode = 'parallel'; workers = '1' }
    'parallel-2' = [pscustomobject]@{ mode = 'parallel'; workers = '2' }
    'parallel-4' = [pscustomobject]@{ mode = 'parallel'; workers = '4' }
    'parallel-8' = [pscustomobject]@{ mode = 'parallel'; workers = '8' }
    'parallel-16' = [pscustomobject]@{ mode = 'parallel'; workers = '16' }
    'parallel-auto' = [pscustomobject]@{ mode = 'parallel'; workers = 'auto' }
    'shadow-16' = [pscustomobject]@{ mode = 'shadow'; workers = '16' }
}
foreach ($entry in $planEntries) {
    $configuration = [string](Get-Stage5JsonValue $entry 'configuration' `
        'Current validation plan entry')
    Assert-Condition ($workerContracts.ContainsKey($configuration)) `
        "Current validation plan uses noncanonical configuration '$configuration'."
    $workerContract = $workerContracts[$configuration]
    $arguments = @($entry.arguments)
    $workerIndex = [Array]::IndexOf([object[]]$arguments, '-workerCount')
    $workerBindingValid = if ([string]$workerContract.workers -ceq 'auto') {
        $workerIndex -lt 0
    }
    else {
        $workerIndex -ge 0 -and $workerIndex + 1 -lt $arguments.Count -and
            [string]$arguments[$workerIndex + 1] -ceq
                [string]$workerContract.workers -and
            [Array]::LastIndexOf([object[]]$arguments, '-workerCount') -eq
                $workerIndex
    }
    Assert-Condition ([string]$entry.simulationMode -ceq
            [string]$workerContract.mode -and
        [string]$entry.requestedWorkers -ceq [string]$workerContract.workers -and
        $workerBindingValid) `
        "Current validation plan configuration '$configuration' has a substituted mode or worker count."
}
$replayEntries = @($planEntries | Where-Object { [string]$_.kind -ceq 'replay' })
$replayWorkers = @($replayEntries | ForEach-Object { [string]$_.configuration } |
    Sort-Object -CaseSensitive -Unique)
Assert-Condition (($replayWorkers -join '|') -ceq
    ((@($requiredWorkers | Sort-Object -CaseSensitive)) -join '|')) `
    'Current replay plan does not contain the exact canonical worker matrix.'
Assert-Condition ($replayEntries.Count -eq 168 -and
    [int]$receipts.replay.details.uniqueReplayCount -eq 10 -and
    [int]$receipts.replay.details.executionCount -eq 168) `
    'Current replay receipt and plan do not contain the exact 168-execution canonical matrix.'
$matrixPasses = @($replayEntries | ForEach-Object { [int]$_.matrixRepeat } |
    Sort-Object -Unique)
Assert-Condition (($matrixPasses -join '|') -ceq '1|2') `
    'Current replay plan does not contain exactly two matrix passes.'
$stressCounts = @()
foreach ($configuration in $requiredWorkers) {
    $configurationEntries = @($replayEntries | Where-Object {
        [string]$_.configuration -ceq $configuration
    })
    Assert-Condition ($configurationEntries.Count -eq 24) `
        "Current replay plan does not retain 24 executions for '$configuration'."
    foreach ($matrixPass in @(1, 2)) {
        $passEntries = @($configurationEntries | Where-Object {
            [int]$_.matrixRepeat -eq $matrixPass
        })
        Assert-Condition ($passEntries.Count -eq 12 -and
            @($passEntries | Where-Object { [bool]$_.stress }).Count -eq 3 -and
            @($passEntries | Where-Object { -not [bool]$_.stress }).Count -eq 9) `
            "Current replay plan matrix pass $matrixPass for '$configuration' is incomplete."
    }
    $stressCounts += @($configurationEntries | Where-Object {
        [bool]$_.stress
    }).Count
}
$requiredAiScenarios = @('4v3', '4v2')
$requiredAiEntries = @($planEntries | Where-Object {
    [string]$_.kind -ceq 'ai' -and
    $requiredAiScenarios -ccontains [string]$_.scenario -and
    $requiredWorkers -ccontains [string]$_.configuration
})
$observedAiScenarios = @($requiredAiEntries | ForEach-Object {
    [string]$_.scenario
} | Sort-Object -CaseSensitive -Unique)
Assert-Condition (($observedAiScenarios -join '|') -ceq
    ((@($requiredAiScenarios | Sort-Object -CaseSensitive)) -join '|')) `
    'Current AI plan does not contain both required 4v3 and 4v2 scenarios.'
$aiSeeds = @($requiredAiEntries | ForEach-Object { [int]$_.seed } |
    Sort-Object -Unique)
$aiRepeats = @($requiredAiEntries | ForEach-Object { [int]$_.repeat } |
    Sort-Object -Unique)
Assert-Condition ($requiredAiEntries.Count -eq 84 -and
    $aiSeeds.Count -eq 3 -and ($aiRepeats -join '|') -ceq '1|2' -and
    [int]$receipts.ai.details.scenarioCount -eq 2 -and
    [int]$receipts.ai.details.distinctSeedCount -eq 3 -and
    [int]$receipts.ai.details.repeatCount -eq 2) `
    'Current AI receipt and plan are below the exact scenario/seed/repeat cross-product.'
$shadowAiEntries = @($planEntries | Where-Object {
    [string]$_.kind -ceq 'ai' -and [string]$_.configuration -ceq 'shadow-16'
})
Assert-Condition ($shadowAiEntries.Count -eq 1 -and
    [string]$shadowAiEntries[0].scenario -ceq '4v2' -and
    [int]$shadowAiEntries[0].repeat -eq 1 -and
    [string]$shadowAiEntries[0].simulationMode -ceq 'shadow' -and
    $planEntries.Count -eq 253) `
    'Current validation plan does not contain the exact bounded shadow execution.'

$reviewedAuthorities = [ordered]@{}
foreach ($title in @('Generals', 'ZeroHour')) {
    $templateAttachment = Get-TemplateAttachment `
        -Kind 'replay-determinism' -Role 'replay-fixture-manifest' `
        -Title $title -ExpectedTrustDomain 'reviewed-fixture'
    $authority = Copy-ReviewedFixtureAuthority -Receipt $templateAttachment `
        -Title $title
    $combinedSource = @($combinedSources | Where-Object {
        [string]$_.title -ceq $title
    })
    Assert-Condition ($combinedSource.Count -eq 1 -and
        [string]$combinedSource[0].reviewedReceiptSha256 -ceq
            [string]$authority.read.sha256) `
        "Combined $title sourceCorpora is detached from its protected reviewed-fixture authority."
    $reviewedAuthorities[$title] = $authority
}
$reviewedReceipt = $reviewedAuthorities['ZeroHour'].source
$reviewedReceiptDestination = $reviewedAuthorities['ZeroHour'].destination
$reviewedDocument = $reviewedAuthorities['ZeroHour'].read.document
$reviewedManifestReference = $reviewedAuthorities['ZeroHour'].manifestReference
$reviewedManifestPath = $reviewedAuthorities['ZeroHour'].manifestPath
$reviewedManifestDestination = $reviewedAuthorities['ZeroHour'].manifestDestination
$reviewedManifest = $reviewedAuthorities['ZeroHour'].manifest
$reviewedFixtures = Get-Stage5JsonValue $reviewedManifest 'fixtures' `
    'Reviewed replay-fixture manifest'
Assert-Condition ($reviewedFixtures.Count -eq 10) `
    'Reviewed replay-fixture manifest must contain exactly ten fixtures.'
$reviewedFixturesById = @{}
$reviewedFixturePaths = New-Object 'Collections.Generic.HashSet[string]' `
    ([StringComparer]::OrdinalIgnoreCase)
$reviewedFixtureHashes = New-Object 'Collections.Generic.HashSet[string]' `
    ([StringComparer]::OrdinalIgnoreCase)
$reviewedStressIds = New-Object 'Collections.Generic.List[string]'
foreach ($fixture in $reviewedFixtures) {
    $fixtureId = [string](Get-Stage5JsonValue $fixture 'id' `
        'Reviewed replay fixture')
    $fixtureSha256 = [string](Get-Stage5JsonValue $fixture 'sha256' `
        "Reviewed replay fixture '$fixtureId'")
    $fixtureStress = Get-Stage5JsonValue $fixture 'stress' `
        "Reviewed replay fixture '$fixtureId'"
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($fixtureId) -and
        -not $reviewedFixturesById.ContainsKey($fixtureId) -and
        $fixtureSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $reviewedFixtureHashes.Add($fixtureSha256) -and
        $fixtureStress -is [bool]) `
        'Reviewed replay-fixture identities are malformed or duplicated.'
    $fixturePath = Resolve-RelativeFile (Split-Path -Parent $reviewedManifestPath) `
        ([string]$fixture.source) 'Reviewed replay fixture'
    Assert-Condition ($reviewedFixturePaths.Add(
            [IO.Path]::GetFullPath($fixturePath))) `
        "Reviewed replay fixture '$fixtureId' aliases another fixture path."
    [void](Copy-ReviewedTemplateFile $fixturePath 'Reviewed replay fixture')
    $reviewedFixturesById[$fixtureId] = [pscustomobject]@{
        id = $fixtureId
        sha256 = $fixtureSha256.ToUpperInvariant()
        stress = [bool]$fixtureStress
    }
    if ([bool]$fixtureStress) { $reviewedStressIds.Add($fixtureId) | Out-Null }
    if (@($fixture.Keys | Where-Object { [string]$_ -ceq 'maps' }).Count -eq 1) {
        foreach ($map in @($fixture.maps)) {
            $mapPath = Resolve-RelativeFile (Split-Path -Parent $reviewedManifestPath) `
                ([string]$map.source) 'Reviewed replay map'
            [void](Copy-ReviewedTemplateFile $mapPath 'Reviewed replay map')
        }
    }
}
Assert-Condition ($reviewedFixturesById.Count -eq 10 -and
    $reviewedStressIds.Count -eq 1) `
    'Reviewed replay-fixture manifest must contain ten unique IDs and one stress fixture.'
foreach ($configuration in $requiredWorkers) {
    foreach ($matrixPass in @(1, 2)) {
        foreach ($fixtureId in @($reviewedFixturesById.Keys)) {
            $reviewedFixture = $reviewedFixturesById[$fixtureId]
            $planned = @($replayEntries | Where-Object {
                [string]$_.configuration -ceq $configuration -and
                [int]$_.matrixRepeat -eq $matrixPass -and
                [string]$_.determinismKey -ceq $fixtureId
            })
            $expectedCount = if ([bool]$reviewedFixture.stress) { 3 } else { 1 }
            Assert-Condition ($planned.Count -eq $expectedCount) `
                "Replay plan does not execute reviewed fixture '$fixtureId' exactly $expectedCount time(s) for '$configuration' pass $matrixPass."
            $expectedReplayArgument = "Stage5Validation\$fixtureId.rep"
            $expectedRepeats = 1..$expectedCount
            $actualRepeats = @($planned | ForEach-Object { [int]$_.repeat } |
                Sort-Object -Unique)
            Assert-Condition (($actualRepeats -join '|') -ceq
                ($expectedRepeats -join '|') -and
                @($planned | Where-Object {
                    ([string]$_.fixtureSha256).ToUpperInvariant() -cne
                        $reviewedFixture.sha256 -or
                    [bool]$_.stress -ne [bool]$reviewedFixture.stress -or
                    [string]$_.replayArgument -cne $expectedReplayArgument -or
                    [string]$_.caseId -cne "$fixtureId-p$matrixPass"
                }).Count -eq 0) `
                "Replay plan is detached from reviewed fixture '$fixtureId'."
            foreach ($plannedEntry in $planned) {
                $arguments = @($plannedEntry.arguments)
                $replayIndex = [Array]::IndexOf([object[]]$arguments, '-replay')
                Assert-Condition ($replayIndex -ge 0 -and
                    $replayIndex + 1 -lt $arguments.Count -and
                    [string]$arguments[$replayIndex + 1] -ceq
                        $expectedReplayArgument -and
                    [Array]::LastIndexOf([object[]]$arguments, '-replay') -eq
                        $replayIndex -and
                    [Array]::IndexOf([object[]]$arguments,
                        '-runSkirmishAITest') -lt 0 -and
                    [Array]::IndexOf([object[]]$arguments,
                        '-runSkirmishAITest4v2') -lt 0) `
                    "Replay plan arguments are detached from reviewed fixture '$fixtureId'."
            }
        }
    }
}
$reviewedAi = Get-Stage5JsonValue $reviewedManifest 'ai' `
    'Reviewed replay-fixture manifest'
$reviewedAiSeeds = Get-Stage5JsonValue $reviewedAi 'seeds' `
    'Reviewed AI configuration'
$reviewedAiScenarios = Get-Stage5JsonValue $reviewedAi 'scenarios' `
    'Reviewed AI configuration'
$reviewedAiRepeats = [int](Get-Stage5JsonValue $reviewedAi 'repeats' `
    'Reviewed AI configuration')
Assert-Condition ($reviewedAiSeeds.Count -eq 3 -and
    @($reviewedAiSeeds | Sort-Object -Unique).Count -eq 3 -and
    (@($reviewedAiScenarios | Sort-Object -CaseSensitive) -join '|') -ceq
        '4v2|4v3' -and $reviewedAiRepeats -eq 2) `
    'Reviewed AI configuration is not the exact required scenario/seed/repeat set.'
foreach ($configuration in $requiredWorkers) {
    foreach ($scenario in $reviewedAiScenarios) {
        foreach ($seed in $reviewedAiSeeds) {
            foreach ($repeat in 1..$reviewedAiRepeats) {
                $planned = @($requiredAiEntries | Where-Object {
                    [string]$_.configuration -ceq $configuration -and
                    [string]$_.scenario -ceq [string]$scenario -and
                    [int]$_.seed -eq [int]$seed -and
                    [int]$_.repeat -eq $repeat
                })
                Assert-Condition ($planned.Count -eq 1) `
                    "AI plan is missing or duplicates '$scenario' seed $seed repeat $repeat for '$configuration'."
                $runnerFlag = if ([string]$scenario -ceq '4v2') {
                    '-runSkirmishAITest4v2'
                }
                else { '-runSkirmishAITest' }
                $incompatibleRunnerFlag = if ([string]$scenario -ceq '4v2') {
                    '-runSkirmishAITest'
                }
                else { '-runSkirmishAITest4v2' }
                $arguments = @($planned[0].arguments)
                $runnerIndex = [Array]::IndexOf([object[]]$arguments, $runnerFlag)
                Assert-Condition ($runnerIndex -ge 0 -and
                    $runnerIndex + 1 -lt $arguments.Count -and
                    [int]$arguments[$runnerIndex + 1] -eq [int]$seed -and
                    [Array]::LastIndexOf([object[]]$arguments, $runnerFlag) -eq
                        $runnerIndex -and
                    [Array]::IndexOf([object[]]$arguments,
                        $incompatibleRunnerFlag) -lt 0 -and
                    [Array]::IndexOf([object[]]$arguments, '-replay') -lt 0) `
                    "AI plan arguments are detached from '$scenario' seed $seed."
            }
        }
    }
}
Assert-Condition ([int]$shadowAiEntries[0].seed -eq [int]$reviewedAiSeeds[0]) `
    'AI shadow execution is detached from the first reviewed seed.'
$expectedAiDeterminismKeys = @(
    foreach ($scenario in $reviewedAiScenarios) {
        foreach ($seed in $reviewedAiSeeds) { "$scenario-seed-$seed" }
    }
)
$executionProofByTitle = @{}
foreach ($title in @('Generals', 'ZeroHour')) {
    $keys = if ($title -ceq 'Generals') {
        [pscustomobject]@{ plan = 'generalsPlan'; validation = 'generalsValidation'
            replay = 'generalsReplay'; ai = 'generalsAi' }
    }
    else {
        [pscustomobject]@{ plan = 'plan'; validation = 'validation'
            replay = 'replay'; ai = 'ai' }
    }
    $titlePlanEvidence = Get-BoundRawEvidence $receipts[$keys.plan].path `
        $receipts[$keys.plan].evidence.document 'validation-plan.json' `
        "$title validation-plan receipt"
    $titleResultsEvidence = Get-BoundRawEvidence $receipts[$keys.validation].path `
        $receipts[$keys.validation].evidence.document 'validation-results.json' `
        "$title validation-results receipt"
    $titleReplayEvidence = Get-BoundRawEvidence $receipts[$keys.replay].path `
        $receipts[$keys.replay].evidence.document 'validation-results.json' `
        "$title replay-results receipt"
    $titleAiEvidence = Get-BoundRawEvidence $receipts[$keys.ai].path `
        $receipts[$keys.ai].evidence.document 'validation-results.json' `
        "$title AI-results receipt"
    Assert-Condition ([string]$titleResultsEvidence.snapshot.sha256 -ceq
            [string]$titleReplayEvidence.snapshot.sha256 -and
        [string]$titleResultsEvidence.snapshot.sha256 -ceq
            [string]$titleAiEvidence.snapshot.sha256) `
        "$title source corpus receipts do not bind one byte-identical result set."
    $titlePlan = $titlePlanEvidence.document
    $titleResults = @($titleResultsEvidence.document)
    $proof = Assert-Stage5DevelopmentReadinessExecutionEvidence `
        -ValidationPlan $titlePlan -Results $titleResults `
        -ReviewedFixtureManifest $reviewedAuthorities[$title].manifest `
        -PlanDetails $receipts[$keys.plan].details `
        -ValidationDetails $receipts[$keys.validation].details `
        -ReplayDetails $receipts[$keys.replay].details `
        -AiDetails $receipts[$keys.ai].details `
        -ValidatedRawLogs @($receipts[$keys.validation].read.rawLogs) `
        -ValidatedChildren @($receipts[$keys.validation].read.validatedChildren) `
        -ExpectedPlanSha256 ([string]$titlePlanEvidence.snapshot.sha256) `
        -ValidationResultsSha256 ([string]$titleResultsEvidence.snapshot.sha256) `
        -ReplayResultsSha256 ([string]$titleReplayEvidence.snapshot.sha256) `
        -AiResultsSha256 ([string]$titleAiEvidence.snapshot.sha256) `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -ExpectedArtifactSetSha256 $script:ArtifactSetSha256 `
        -ExpectedCohortNonce $ExpectedCohortNonce `
        -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
        -ExpectedRuntimeClosure $script:RuntimeClosure `
        -ExpectedQualificationData $qualificationDataByTitle[$title] `
        -ExpectedCurrentExecutablePath `
            $artifactPaths[$titleCorpora[$title].executableRole] `
        -RequireCurrentArtifactRelocation -ExpectedTitle $title
    $executionProofByTitle[$title] = $proof
}
Assert-Condition ((Get-Stage5FileSha256 $reviewedManifestDestination) -ceq
    ([string]$reviewedManifestReference.sha256).ToUpperInvariant()) `
    'Copied reviewed replay-fixture manifest hash is detached from its receipt.'

$stage3Baseline = Get-TemplateAttachment -Kind 'performance-scaling' `
    -Role 'stage3-baseline' -Title 'ZeroHour' `
    -ExpectedTrustDomain 'reviewed-fixture'
$stage3BaselineDestination = Copy-ReviewedTemplateFile $stage3Baseline.path `
    'Reviewed Stage 3 performance baseline'
$phaseBaselineProfilePath = Find-ExactlyOneFile $performanceStage `
    'Stage5PerformancePhaseBaselineProfile.json' `
    'External performance phase-baseline profile'
$phaseBaselineProfileSha256 = Get-Stage5FileSha256 $phaseBaselineProfilePath
Assert-Condition ($ExpectedPhaseBaselineProfileSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
    $phaseBaselineProfileSha256 -ceq
        $ExpectedPhaseBaselineProfileSha256.ToUpperInvariant()) `
    'External performance phase-baseline profile does not match the independently pinned reviewed hash.'
$performancePath = Find-ExactlyOneFile $performanceStage `
    'Stage5PerformanceScaling.json' 'External performance evidence'
$performanceEvidence = Read-JsonEvidence $performancePath `
    'External Stage 5 performance evidence'
$performanceDocument = $performanceEvidence.document
Assert-Condition (([string](Get-Stage5JsonValue $performanceDocument `
        'stage3BaselineSha256' 'External Stage 5 performance evidence')).ToUpperInvariant() -ceq
        ([string]$stage3Baseline.sha256).ToUpperInvariant()) `
    'External performance evidence is not bound to the reviewed Stage 3 baseline.'
$performanceProof = Read-Stage5PerformanceScalingEvidence -Path $performancePath `
    -ExpectedSourceCommit $ExpectedSourceCommit `
    -ExpectedArtifactSetSha256 $script:ArtifactSetSha256 `
    -ExpectedExecutableSha256 $artifactHashes['zerohour-executable'] `
    -ExpectedStage3BaselineSha256 $stage3Baseline.sha256 `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
    -ExpectedRuntimeClosure $script:RuntimeClosure `
    -ExpectedPhaseBaselineProfileSha256 $phaseBaselineProfileSha256
foreach ($freshnessName in @('cohortNonce', 'cohortCreatedUtc', 'recordedUtc',
        'runtimeClosure')) {
    Get-Stage5JsonValue $performanceDocument $freshnessName `
        'External Stage 5 performance evidence' | Out-Null
}
Assert-Condition ((Get-Stage5JsonValue $performanceDocument 'cohortNonce' `
        'External Stage 5 performance evidence') -ceq $ExpectedCohortNonce -and
    (Get-Stage5JsonValue $performanceDocument 'cohortCreatedUtc' `
        'External Stage 5 performance evidence') -ceq $ExpectedCohortCreatedUtc) `
    'External performance evidence is detached from the current execution cohort.'
[DateTimeOffset]$performanceRecorded = [DateTimeOffset]::MinValue
Assert-Condition ([DateTimeOffset]::TryParse([string](Get-Stage5JsonValue `
        $performanceDocument 'recordedUtc' 'External Stage 5 performance evidence'),
        [ref]$performanceRecorded) -and $performanceRecorded -ge $script:CohortCreated) `
    'External performance evidence predates the current execution cohort.'
$performanceClosure = Get-Stage5JsonValue $performanceDocument 'runtimeClosure' `
    'External Stage 5 performance evidence'
Assert-Stage5JsonShape $performanceClosure @('dependencyManifestSha256',
    'closureSha256') 'External Stage 5 performance runtime closure'
Assert-Condition (([string]$performanceClosure.dependencyManifestSha256).ToUpperInvariant() -ceq
        $script:RuntimeClosure.dependencyManifestSha256 -and
    ([string]$performanceClosure.closureSha256).ToUpperInvariant() -ceq
        $script:RuntimeClosure.closureSha256) `
    'External performance evidence runtime closure is stale or substituted.'

$lockstepPath = Find-ExactlyOneFile $lockstepStage 'LockstepV2LoopbackEvidence.json' `
    'Staged lockstep-v2 evidence'
$lockstepHash = Get-Stage5FileSha256 $lockstepPath
$lockstepProof = Read-Stage5LockstepV2Evidence -Path $lockstepPath `
    -ExpectedSourceCommit $ExpectedSourceCommit `
    -ExpectedArtifactSetSha256 $script:ArtifactSetSha256 `
    -ArtifactHashes $artifactHashes -ArtifactPaths $artifactPaths `
    -ArtifactRootDirectory $outputFull -ExpectedEvidenceSha256 $lockstepHash `
    -ExpectedCohortNonce $ExpectedCohortNonce `
    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
    -ExpectedRuntimeClosure $script:RuntimeClosure
$lockstepDocument = (Read-JsonEvidence $lockstepPath `
    'Staged lockstep-v2 evidence').document

$replayEnvelope = Write-EvidenceEnvelope 'replay-determinism' 'Both' @(
    (New-Attachment -Role 'replay-results' -Title 'ZeroHour' `
        -Path $receipts.replay.path -TrustDomain 'host-runner'),
    (New-Attachment -Role 'replay-fixture-manifest' -Title 'Generals' `
        -Path $reviewedAuthorities['Generals'].destination `
        -TrustDomain 'reviewed-fixture'),
    (New-Attachment -Role 'replay-fixture-manifest' -Title 'ZeroHour' `
        -Path $reviewedAuthorities['ZeroHour'].destination `
        -TrustDomain 'reviewed-fixture')) ([ordered]@{
    uniqueReplayCount = [int]$receipts.replay.details.uniqueReplayCount
    executionCount = [int]$receipts.replay.details.executionCount
    matrixPasses = $matrixPasses.Count
    stressExecutionsPerConfiguration = [int]$stressCounts[0]
    workerConfigurations = $requiredWorkers
    allExecutionsPassed = $true
    deterministicAcrossWorkers = $true
})
$aiEnvelope = Write-EvidenceEnvelope 'fresh-ai' 'ZeroHour' @(
    (New-Attachment -Role 'ai-results' -Title 'ZeroHour' `
        -Path $receipts.ai.path -TrustDomain 'host-runner')) ([ordered]@{
    scenarios = $requiredAiScenarios
    distinctSeeds = $aiSeeds.Count
    repeats = $aiRepeats.Count
    workerConfigurations = $requiredWorkers
    freshGames = $true
    allGamesCompleted = $true
    deterministicAcrossWorkers = $true
})
$regressionRatio = [double]$performanceProof.maximumOneWorkerRegressionRatio
$eightWorkerSpeedup = [double]$performanceProof.minimumEightWorkerSpeedup
$physicalCoreCount = [int]$performanceProof.physicalCoreCount
$sixteenStatus = if ($physicalCoreCount -ge 16) { 'passed' } else {
    'unsupported-host-topology'
}
$eightToSixteen = if ($physicalCoreCount -ge 16) {
    [double]$performanceProof.minimumEightToSixteenSpeedup
}
else { $null }
$performanceEnvelope = Write-EvidenceEnvelope 'performance-scaling' 'ZeroHour' @(
    (New-Attachment -Role 'performance-report' -Title 'ZeroHour' `
        -Path $performancePath -TrustDomain 'host-runner'),
    (New-Attachment -Role 'stage3-baseline' -Title 'ZeroHour' `
        -Path $stage3BaselineDestination -TrustDomain 'reviewed-fixture'),
    (New-Attachment -Role 'phase-baseline-profile' -Title 'ZeroHour' `
        -Path $phaseBaselineProfilePath -TrustDomain 'reviewed-fixture')) `
    ([ordered]@{
    physicalCoreCount = $physicalCoreCount
    oneWorkerRegressionRatio = $regressionRatio
    eightWorkerSpeedup = $eightWorkerSpeedup
    sixteenWorkerStatus = $sixteenStatus
    eightToSixteenSpeedup = $eightToSixteen
})
$mixedEnvelope = Write-EvidenceEnvelope 'mixed-worker-multiplayer' 'Both' @(
    (New-Attachment -Role 'multiplayer-results' -Title 'Both' `
        -Path $lockstepPath -TrustDomain 'host-runner')) ([ordered]@{
    nativeEvidenceKind = 'lockstep-v2-multiplayer'
    producer = 'installed-lockstep-v2'
    nativeEvidenceSha256 = $lockstepHash
    networkRosterMask = [int]$lockstepDocument.networkRosterMask
    simulationRosterMask = [int]$lockstepDocument.simulationRosterMask
    aiRosterMask = [int]$lockstepDocument.aiRosterMask
    aiPlayerCount = [int]$lockstepDocument.aiPlayerCount
    title = 'Both'
    sessionCount = @($lockstepProof.sessions).Count
    peerCount = [int]$lockstepProof.peerCount
    commonStopFrame = [int]$lockstepProof.commonStopFrame
    allMatchesCompleted = $true
    stateTracesIdentical = $true
    crossEpochRejected = [bool]$lockstepProof.crossEpochRejected
    contentMismatchRejected = [bool]$lockstepProof.contentMismatchRejected
})
$combinedEnvelope = Write-EvidenceEnvelope `
    'combined-stage4-stage5-installed-runtime' 'Both' @(
    (New-Attachment -Role 'combined-results' -Title 'Both' `
        -Path $receipts.combined.path -TrustDomain 'host-runner')) `
    ([ordered]@{
        installedRuntime = $true
        pipelineMode = [string]$receipts.combined.details.pipelineMode
        simulationMode = [string]$receipts.combined.details.simulationMode
        requestedWorkers = [string]$receipts.combined.details.requestedWorkers
        workerPolicy = [string]$receipts.combined.details.workerPolicy
        projectionSequence = [int]$receipts.combined.details.projectionSequence
        projectionSemantics = [string]$receipts.combined.details.projectionSemantics
        sourceChildCount = [int]$receipts.combined.details.sourceChildCount
        bothTitlesPassed = [bool]$receipts.combined.details.bothTitlesPassed
    })
$runtimeEnvelope = New-Stage5DeterministicRuntimeEnvelope `
    -ValidationPlanReceiptPath $receipts.plan.path `
    -ValidationResultsReceiptPath $receipts.validation.path `
    -InstalledKernelAttachmentPath $installedKernelAttachmentPath `
    -RequiredWorkers $requiredWorkers `
    -ReplayEvidenceSha256 $replayEnvelope.sha256 `
    -FreshAiEvidenceSha256 $aiEnvelope.sha256 `
    -PerformanceScalingEvidenceSha256 $performanceEnvelope.sha256

$evidenceRecords = [ordered]@{
    'deterministic-runtime' = $runtimeEnvelope
    'replay-determinism' = $replayEnvelope
    'fresh-ai' = $aiEnvelope
    'performance-scaling' = $performanceEnvelope
    'mixed-worker-multiplayer' = $mixedEnvelope
    'combined-stage4-stage5-installed-runtime' = $combinedEnvelope
}
$manifestPath = Join-Path $outputFull 'FinalAcceptanceManifest.json'
$manifest = [ordered]@{
    schemaVersion = 1
    gateName = 'final-stage5-acceptance'
    sourceCommit = $ExpectedSourceCommit
    cohortNonce = $ExpectedCohortNonce
    cohortCreatedUtc = $ExpectedCohortCreatedUtc
    artifactSet = [ordered]@{
        path = 'Stage5ArtifactSet.json'
        sha256 = $script:ArtifactSetSha256
    }
    evidence = @($evidenceRecords.Keys | ForEach-Object {
        $kind = [string]$_
        [ordered]@{
            kind = $kind
            path = Get-ContainedRelativePath $outputFull `
                $evidenceRecords[$kind].path "Final acceptance '$kind' envelope"
            sha256 = $evidenceRecords[$kind].sha256
        }
    })
}
$manifestBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
    ($manifest | ConvertTo-Json -Depth 64))
[void](Write-Stage5FinalAcceptanceFileAtomically -Path $manifestPath `
    -Bytes $manifestBytes -Context 'Stage 5 final acceptance manifest')

$readiness = Invoke-Stage5FinalAcceptanceAggregation $manifestPath `
    -DevelopmentReadiness `
    -ExternalQualificationExempt:$ExternalQualificationExempt
Assert-Condition ($readiness.gateName -ceq 'stage5-development-readiness' -and
    $readiness.status -ceq 'ready-for-manual-approval' -and
    $readiness.sourceCommit -ceq $ExpectedSourceCommit -and
    $readiness.cohortNonce -ceq $ExpectedCohortNonce -and
    -not [bool]$readiness.finalAcceptanceClaim) `
    'Assembled manifest did not pass the exact pre-manual development-readiness gate.'

Write-Output $manifestPath
