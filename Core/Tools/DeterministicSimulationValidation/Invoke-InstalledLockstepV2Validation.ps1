[CmdletBinding()]
param(
    [string]$GeneralsExecutable,
    [string]$ZeroHourExecutable,
    [string]$ArtifactSetManifestPath,
    [string]$SourceCommit,
    [string]$OutputDirectory,
    [string]$MapName,
    [uint32]$GeneralsMapCrc,
    [uint32]$ZeroHourMapCrc,
    [ValidateRange(2, 8)][int]$PeerCount = 2,
    [ValidateRange(1, 2147483646)][int]$Seed = 23063,
    [ValidateRange(1024, 65000)][int]$BasePort = 41000,
    [ValidateRange(30, 1800)][int]$PeerTimeoutSeconds = 300,
    # The workflow mints one cohort before any title/component run and passes
    # the same identity into this producer.  Direct invocations may mint a
    # cohort only when they are not participating in a final acceptance run;
    # all emitted evidence still carries a canonical cohort and closure.
    [string]$ExecutionCohortNonce = '',
    [string]$ExecutionCohortCreatedUtc = '',
    [string]$RuntimeClosureDependencyManifestSha256 = '',
    [string]$RuntimeClosureSha256 = '',
    [string]$QualificationDataManifestPath = '',
    [string]$QualificationDataManifestSha256 = '',
    [string]$QualificationDataClosureSha256 = '',
    [switch]$AllowHeadlessDirectExecution,
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Stage5ValidationProfileCapability.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Stage5RegistryRecovery.psm1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'Stage5InstalledLockstepV2Session.psm1') -ErrorAction Stop

$CommonStopFrame = 4096
$LockstepSchema = 2
$LockstepProtocolEpoch = 2
$LockstepAuthorityMask = 63
$LockstepNetworkPeerCount = 2
$LockstepNetworkRosterMask = 0x3
$LockstepSimulationRosterMask = 0x3f
$LockstepAIRosterMask = 0x3c
$LockstepAIPlayerCount = 4
$LockstepCheckpointCount = 129
$LockstepMode = 'installed-lockstep-v2-production'
$LockstepProducer = 'installed-lockstep-v2'
$LockstepTitleSessionDisposition = 'removed-after-peer-exit-before-evidence-persist'
$LockstepEvidenceClosureLeaf = 'Stage5LockstepV2EvidenceClosure.json'
$LockstepQualificationDataEvidenceLeaf = 'QualificationData.json'
$LockstepGeneralsDataArchiveSha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
$LockstepZeroHourDataArchiveSha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
$LockstepMagic = 'RTS_LOCKSTEP_V2_RECEIPT'
$LockstepNegativeProbeMagic = 'RTS_LOCKSTEP_V2_NEGATIVE_PROBE'
$PostKillWaitMilliseconds = 5000
$script:LockstepHostSelfTestScratchRoot = $null




















































function Read-AndValidateQualificationData {
    param(
        [string]$Manifest,
        [string]$ExpectedManifestSha256,
        [string]$ExpectedClosureSha256,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedMapName,
        [object]$ExpectedMapCrcs,
        [object[]]$RuntimeFiles
    )
    $full = [IO.Path]::GetFullPath($Manifest)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf) -or
        -not (Test-CanonicalHex $ExpectedManifestSha256 64) -or
        -not (Test-CanonicalHex $ExpectedClosureSha256 64)) {
        throw 'Qualification data manifest and independently recorded hashes are required.'
    }
    $root = [IO.Path]::GetFullPath((Split-Path -Parent $full)).TrimEnd('\')
    Assert-LockstepNoReparse $full 'qualification data manifest' $root
    $manifestSha256 = Get-UpperSha256 $full
    if ($manifestSha256 -cne $ExpectedManifestSha256.ToUpperInvariant()) {
        throw 'Qualification data manifest SHA-256 is stale or substituted.'
    }
    $document = ConvertFrom-Stage5JsonDictionary $full
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind',
        'producer', 'sourceCommit', 'productSet', 'mapName', 'mapCrcs',
        'archiveSources', 'files', 'closureSha256') `
        'Qualification data manifest'
    $productSet = @($document['productSet'])
    $expectedCrcs = Assert-LockstepMapCrcs $ExpectedMapCrcs `
        'Expected qualification data map CRCs'
    $manifestCrcs = Assert-LockstepMapCrcs $document['mapCrcs'] `
        'Qualification data map CRCs'
    if (-not (Test-Stage5JsonInteger $document['schemaVersion']) -or
        $document['schemaVersion'] -ne 2 -or
        [string]$document['evidenceKind'] -cne 'lockstep-v2-qualification-data' -or
        [string]$document['producer'] -cne 'genci-r2-trimmed-data' -or
        [string]$document['sourceCommit'] -cne $ExpectedSourceCommit -or
        $productSet.Count -ne 2 -or [string]$productSet[0] -cne 'Generals' -or
        [string]$productSet[1] -cne 'ZeroHour' -or
        [string]$document['mapName'] -cne $ExpectedMapName -or
        $manifestCrcs.Generals -ne $expectedCrcs.Generals -or
        $manifestCrcs.ZeroHour -ne $expectedCrcs.ZeroHour -or
        [string]$document['closureSha256'] -cnotmatch '^[0-9A-F]{64}$' -or
        [string]$document['closureSha256'] -cne
            $ExpectedClosureSha256.ToUpperInvariant()) {
        throw 'Qualification data identity, map binding, or closure is stale or substituted.'
    }

    $expectedArchives = @(
        [pscustomobject]@{
            title = 'Generals'
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = $LockstepGeneralsDataArchiveSha256
        },
        [pscustomobject]@{
            title = 'ZeroHour'
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = $LockstepZeroHourDataArchiveSha256
        }
    )
    $archiveSources = @($document['archiveSources'])
    if ($archiveSources.Count -ne $expectedArchives.Count) {
        throw 'Qualification data must bind exactly the two reviewed trimmed archives.'
    }
    for ($archiveIndex = 0; $archiveIndex -lt $archiveSources.Count;
        ++$archiveIndex) {
        $archive = $archiveSources[$archiveIndex]
        $expectedArchive = $expectedArchives[$archiveIndex]
        Assert-Stage5JsonShape $archive @('title', 'object', 'sha256') `
            'Qualification data archive source'
        if ([string]$archive['title'] -cne $expectedArchive.title -or
            [string]$archive['object'] -cne $expectedArchive.object -or
            [string]$archive['sha256'] -cne $expectedArchive.sha256) {
            throw 'Qualification data archive source is unreviewed, reordered, or substituted.'
        }
    }

    $requiredByTitle = [ordered]@{
        Generals = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
        ZeroHour = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    $productFiles = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($runtimeFile in @($RuntimeFiles)) {
        [void]$productFiles.Add([IO.Path]::GetFullPath(
            [string]$runtimeFile.fullPath))
    }
    $declaredDataFiles = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $seenTitlePaths = @{
        Generals = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        ZeroHour = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
    }
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $previousIdentity = $null
    $entries = @($document['files'])
    if ($document['files'] -isnot [Array] -or $entries.Count -lt 12) {
        throw 'Qualification data manifest does not contain the complete staged data set.'
    }
    foreach ($entry in $entries) {
        Assert-Stage5JsonShape $entry @('title', 'path', 'sha256') `
            'Qualification data file'
        $title = [string]$entry['title']
        $relative = [string]$entry['path']
        $declaredHash = [string]$entry['sha256']
        $runtimePrefix = if ($title -ceq 'Generals') {
            'GeneralsRuntime/'
        }
        elseif ($title -ceq 'ZeroHour') { 'ZeroHourRuntime/' }
        else { '' }
        $titleRelative = if ($relative.StartsWith($runtimePrefix,
                [StringComparison]::Ordinal)) {
            $relative.Substring($runtimePrefix.Length)
        }
        else { '' }
        $isRootBig = $titleRelative.IndexOf('/') -lt 0 -and
            $titleRelative.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $titleRelative.StartsWith('Data/',
            [StringComparison]::OrdinalIgnoreCase)
        $identity = "$title|$relative"
        if ([string]::IsNullOrWhiteSpace($runtimePrefix) -or
            [string]::IsNullOrWhiteSpace($titleRelative) -or
            $relative -cnotmatch '^[^\\/:]+(?:/[^\\/:]+)*$' -or
            $relative -match '(^|/)\.\.?(/|$)' -or
            (-not $isRootBig -and -not $isDataFile) -or
            $declaredHash -cnotmatch '^[0-9A-F]{64}$' -or
            ($null -ne $previousIdentity -and
                [StringComparer]::Ordinal.Compare($previousIdentity,
                    $identity) -ge 0)) {
            throw "Qualification data file is unsafe, unsorted, or unreviewed: $relative"
        }
        $path = Resolve-BoundedArtifactPath $full $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            $productFiles.Contains($path) -or
            -not $declaredDataFiles.Add($path) -or
            -not $seenTitlePaths[$title].Add($titleRelative) -or
            (Get-UpperSha256 $path) -cne $declaredHash) {
            throw "Qualification data file is missing, aliases product data, or has changed: $relative"
        }
        $canonicalLines.Add(('{0}|{1}|{2}' -f $title, $relative,
            $declaredHash)) | Out-Null
        $previousIdentity = $identity
    }
    foreach ($title in $requiredByTitle.Keys) {
        foreach ($requiredPath in $requiredByTitle[$title]) {
            if (-not $seenTitlePaths[$title].Contains($requiredPath)) {
                throw "Qualification data for $title omits required file $requiredPath."
            }
        }
    }
    $computedClosureSha256 = Get-LockstepTextSha256 `
        (($canonicalLines.ToArray() -join "`n") + "`n")
    if ($computedClosureSha256 -cne [string]$document['closureSha256']) {
        throw 'Qualification data file closure SHA-256 is stale or substituted.'
    }

    $actualRuntimeItems = @(@('GeneralsRuntime', 'ZeroHourRuntime') |
        ForEach-Object {
            Get-ChildItem -LiteralPath (Join-Path $root $_) -Recurse -Force
        })
    foreach ($runtimeItem in $actualRuntimeItems) {
        Assert-LockstepNoReparse $runtimeItem.FullName `
            'qualification runtime item' $root
    }
    $actualExtraFiles = @($actualRuntimeItems | Where-Object {
        -not $_.PSIsContainer -and
        -not $productFiles.Contains([IO.Path]::GetFullPath($_.FullName))
    })
    if ($actualExtraFiles.Count -ne $declaredDataFiles.Count) {
        throw 'Qualification runtime contains missing or undeclared staged data files.'
    }
    foreach ($extraFile in $actualExtraFiles) {
        if (-not $declaredDataFiles.Contains(
                [IO.Path]::GetFullPath($extraFile.FullName))) {
            throw "Qualification runtime contains undeclared staged data: $($extraFile.FullName)"
        }
    }
    return [pscustomobject]@{
        path = $full
        manifestSha256 = $manifestSha256
        closureSha256 = $computedClosureSha256
        fileCount = $entries.Count
        mapName = [string]$document['mapName']
        mapCrcs = $manifestCrcs
    }
}



function Get-LockstepTextSha256 {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($Text))) -replace '-', '')
    }
    finally { $sha.Dispose() }
}

function New-LockstepV2EvidenceClosure {
    param(
        [string]$EvidenceRoot,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$CohortNonce
    )
    if (-not (Test-LowerHex40 $SourceCommit) -or
        -not (Test-CanonicalHex $ArtifactSetSha256 64)) {
        throw 'Lockstep-v2 evidence closure identity is malformed.'
    }
    Assert-LockstepCanonicalUuid $CohortNonce `
        'Lockstep-v2 evidence closure cohortNonce' | Out-Null

    $rootFull = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) {
        throw "Lockstep-v2 evidence closure root was not found: $rootFull"
    }
    Assert-LockstepNoReparse $rootFull 'lockstep-v2 evidence closure root' $rootFull
    $manifestPath = Join-Path $rootFull $LockstepEvidenceClosureLeaf
    if ($null -ne (Get-LockstepItemIfPresent $manifestPath)) {
        throw "Lockstep-v2 evidence closure output already exists: $manifestPath"
    }

    $filesByPath = @{}
    foreach ($item in @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force)) {
        Assert-LockstepNoReparse $item.FullName `
            'lockstep-v2 evidence closure member' $rootFull
        if ($item.PSIsContainer) { continue }
        $full = [IO.Path]::GetFullPath($item.FullName)
        $relative = $full.Substring($rootFull.Length).TrimStart('\', '/')
        $normalized = $relative.Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($normalized) -or
            $normalized -ceq $LockstepEvidenceClosureLeaf -or
            [IO.Path]::IsPathRooted($normalized) -or
            $normalized -match '(^|/)\.\.?(/|$)|:') {
            throw "Lockstep-v2 evidence closure contains an unsafe member: $full"
        }
        if ($filesByPath.ContainsKey($normalized)) {
            throw "Lockstep-v2 evidence closure aliases member '$normalized'."
        }
        $filesByPath[$normalized] = Get-UpperSha256 $full
    }
    if ($filesByPath.Count -lt 2 -or
        -not $filesByPath.ContainsKey('LockstepV2LoopbackEvidence.json') -or
        -not $filesByPath.ContainsKey('mixed-worker-multiplayer.json')) {
        throw 'Lockstep-v2 evidence closure is missing its native evidence or final-acceptance envelope.'
    }

    [string[]]$paths = @($filesByPath.Keys | ForEach-Object { [string]$_ })
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $entries = @($paths | ForEach-Object {
        [ordered]@{ path = $_; sha256 = [string]$filesByPath[$_] }
    })
    $canonicalLines = @($entries | ForEach-Object {
        '{0}|{1}' -f $_.path, $_.sha256
    })
    $closureSha256 = Get-LockstepTextSha256 `
        (($canonicalLines -join "`n") + "`n")
    $document = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'lockstep-v2-evidence-closure'
        producer = $LockstepProducer
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256.ToUpperInvariant()
        cohortNonce = $CohortNonce
        canonicalEvidenceRoot = $rootFull
        fileCount = $entries.Count
        files = $entries
        closureSha256 = $closureSha256
    }
    Write-AtomicText $manifestPath ($document | ConvertTo-Json -Depth 8)
    Assert-LockstepNoReparse $manifestPath `
        'lockstep-v2 evidence closure manifest' $rootFull
    return [pscustomobject]@{
        path = $manifestPath
        sha256 = Get-UpperSha256 $manifestPath
        closureSha256 = $closureSha256
        document = $document
    }
}

function Write-LockstepHostSelfTestText {
    param([string]$Path, [string]$Text)
    $root = [string]$script:LockstepHostSelfTestScratchRoot
    Assert-LockstepNoReparse $Path 'self-test output' $root
    [IO.File]::WriteAllText($Path, $Text,
        (New-Object Text.UTF8Encoding($false)))
    Assert-LockstepNoReparse $Path 'self-test output' $root
}





















function New-SyntheticReceiptText {
    param([int]$LocalSlot = 0, [int]$PeerCount = 2,
        [string]$RunNonce = '0123456789ABCDEF0123456789ABCDEF',
        [string]$SessionNonce = 'ABCDEF0123456789ABCDEF0123456789',
        [string]$ExecutableSha256 = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',
        [string]$SourceCommit = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
        [int]$PhysicalWorkerCount = 2)
    $lines = New-Object Collections.Generic.List[string]
    [void]$lines.Add($LockstepMagic)
    [void]$lines.Add("producer=$LockstepProducer")
    [void]$lines.Add("mode=$LockstepMode")
    foreach ($line in @(
        'schema=2', 'protocol_epoch=2', "local_slot=$LocalSlot", "peer_count=$PeerCount",
        "roster_mask=$LockstepNetworkRosterMask",
        "simulation_roster_mask=$LockstepSimulationRosterMask",
        "ai_roster_mask=$LockstepAIRosterMask", 'build_compatibility_crc=1',
        'content_crc=1', 'map_crc=1', "common_stop_frame=$CommonStopFrame",
        "proven_kernel_mask=$LockstepAuthorityMask", 'packet_router_slot=0',
        'origin_mode=2', "run_nonce=$RunNonce", "session_nonce=$SessionNonce",
        "executable_sha256=$ExecutableSha256", "source_revision=$SourceCommit",
        'network_session_token=1', "final_frame=$CommonStopFrame",
        "frame_count=$CommonStopFrame", "contributed_peer_mask=$((1 -shl $PeerCount) - 1)",
        "checkpoint_count=$LockstepCheckpointCount", "validation_authority_mask=$LockstepAuthorityMask",
        'executable_origin=1', 'worker_telemetry_executable_origin=1',
        'transport_path_used=1', 'handshake_validated=1', 'clean_shutdown=1',
        'ai_planning_captured_snapshots=4', 'ai_planning_captured_candidates=16',
        'ai_planning_requested_batches=2', 'ai_planning_submitted_jobs=8',
        'ai_planning_completed_jobs=8', 'ai_planning_serial_fallbacks=0',
        'ai_planning_shadow_matches=8', 'ai_planning_shadow_mismatches=0',
        'ai_planning_validation_failures=0',
        'ai_planning_canonical_validation_invocations=2',
        'ai_planning_committed_batches=2',
        'ai_planning_parallel_authoritative_commits=2',
        'ai_planning_rejected_commits=0',
        'ai_planning_physical_worker_executions=8',
        'ai_planning_owner_helped_executions=0',
        "ai_planning_observed_physical_worker_mask=$((1 -shl $PhysicalWorkerCount) - 1)",
        "ai_planning_maximum_distinct_physical_workers=$PhysicalWorkerCount",
        "ai_planning_maximum_concurrent_physical_workers=$PhysicalWorkerCount",
        'ai_planning_digest=1')) {
        [void]$lines.Add($line)
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $count = if ($slot -lt $PeerCount) { $slot + 1 } else { 0 }
        $first = if ($slot -lt $PeerCount) { 8 } else { 0 }
        $last = if ($slot -lt $PeerCount) { 8 + $slot } else { 0 }
        $has = if ($slot -lt $PeerCount) { 1 } else { 0 }
        $id = if ($slot -lt $PeerCount) { $slot + 1 } else { 0 }
        $digest = if ($slot -lt $PeerCount) { $slot + 1 } else { 0 }
        [void]$lines.Add("peer_${slot}_command_count=$count")
        [void]$lines.Add("peer_${slot}_first_command_frame=$first")
        [void]$lines.Add("peer_${slot}_last_command_frame=$last")
        [void]$lines.Add("peer_${slot}_last_command_id=$id")
        [void]$lines.Add("peer_${slot}_has_last_command_id=$has")
        [void]$lines.Add("peer_${slot}_last_command_digest=$digest")
        [void]$lines.Add("peer_${slot}_command_digest=$digest")
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        [void]$lines.Add("kernel_${kernel}_physical_worker_mask=$((1 -shl $PhysicalWorkerCount) - 1)")
        [void]$lines.Add("kernel_${kernel}_physical_worker_jobs=64")
        [void]$lines.Add("kernel_${kernel}_distinct_physical_workers=$PhysicalWorkerCount")
        [void]$lines.Add("kernel_${kernel}_peak_concurrent_physical_workers=$PhysicalWorkerCount")
        [void]$lines.Add("kernel_${kernel}_physical_worker_mask_complete=1")
    }
    for ($checkpoint = 0; $checkpoint -lt $LockstepCheckpointCount; ++$checkpoint) {
        $frame = if ($checkpoint -eq 0) { 1 } else { $checkpoint * 32 }
        [void]$lines.Add("checkpoint_${checkpoint}_frame=$frame")
        [void]$lines.Add("checkpoint_${checkpoint}_crc=1")
        [void]$lines.Add("checkpoint_${checkpoint}_command_digest=1")
    }
    [void]$lines.Add('END')
    return (($lines.ToArray() -join "`n") + "`n")
}

function New-LockstepV2FinalAcceptanceEnvelope {
    param(
        [string]$NativeEvidencePath,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$RecordedUtc,
        [string]$MapName,
        [object]$MapCrcs,
        [int]$Seed,
        [int]$PeerCount,
        [object[]]$Sessions,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc,
        [object]$RuntimeClosure
    )
    $nativeFull = [IO.Path]::GetFullPath($NativeEvidencePath)
    if (-not (Test-Path -LiteralPath $nativeFull -PathType Leaf)) {
        throw "Lockstep-v2 native evidence was not written before its final-acceptance envelope: $nativeFull"
    }
    if ($SourceCommit -notmatch '^[0-9a-f]{40}$' -or
        $ArtifactSetSha256 -notmatch '^[0-9A-F]{64}$' -or
        [string]::IsNullOrWhiteSpace($RecordedUtc)) {
        throw 'Lockstep-v2 final-acceptance envelope identity is incomplete.'
    }
    Assert-LockstepCanonicalUuid $CohortNonce 'Lockstep-v2 final-acceptance envelope cohortNonce' | Out-Null
    $validatedRuntimeClosure = Assert-LockstepRuntimeClosure $RuntimeClosure `
        'Lockstep-v2 final-acceptance envelope'
    $validatedMapCrcs = Assert-LockstepMapCrcs $MapCrcs `
        'Lockstep-v2 final-acceptance envelope'
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse($RecordedUtc, [ref]$recorded) -or
        -not [DateTimeOffset]::TryParse($CohortCreatedUtc, [ref]$cohortCreated) -or
        $recorded -lt $cohortCreated) {
        throw 'Lockstep-v2 final-acceptance envelope timestamp predates its execution cohort or is invalid.'
    }
    if ($PeerCount -ne $LockstepNetworkPeerCount -or
        -not (Test-SafeMapName $MapName) -or
        $Sessions.Count -ne 2) {
        throw 'Lockstep-v2 final-acceptance envelope has an invalid two-session topology.'
    }
    $nativeSha256 = Get-UpperSha256 $nativeFull
    $expectedTitles = @('Generals', 'ZeroHour')
    $nativeDocument = $null
    try {
        $nativeDocument = [IO.File]::ReadAllText($nativeFull) | ConvertFrom-Json
    }
    catch {
        throw "Lockstep-v2 native evidence is not parseable JSON: $nativeFull"
    }
    if ($null -eq $nativeDocument -or $null -eq $nativeDocument.negativeProbes) {
        throw 'Lockstep-v2 native evidence has no observed negative-probe collection.'
    }
    $nativeMapCrcs = Assert-LockstepMapCrcs $nativeDocument.mapCrcs `
        'Lockstep-v2 native evidence'
    if ([string]$nativeDocument.cohortNonce -cne $CohortNonce -or
        [string]$nativeDocument.mapName -cne $MapName -or
        $nativeMapCrcs.Generals -ne $validatedMapCrcs.Generals -or
        $nativeMapCrcs.ZeroHour -ne $validatedMapCrcs.ZeroHour -or
        $null -eq $nativeDocument.runtimeClosure -or
        [string]$nativeDocument.runtimeClosure.dependencyManifestSha256 -cne
            $validatedRuntimeClosure.dependencyManifestSha256 -or
        [string]$nativeDocument.runtimeClosure.closureSha256 -cne
            $validatedRuntimeClosure.closureSha256) {
        throw 'Lockstep-v2 native evidence is detached from the execution cohort/runtime closure.'
    }
    $negativeProbeProperties = @($nativeDocument.negativeProbes.PSObject.Properties |
        ForEach-Object { $_.Name })
    if ($negativeProbeProperties.Count -ne 2 -or
        $negativeProbeProperties[0] -cne 'crossEpoch' -or
        $negativeProbeProperties[1] -cne 'contentMismatch') {
        throw 'Lockstep-v2 native negative-probe collection has an unexpected shape.'
    }
    $crossEpochEntries = @($nativeDocument.negativeProbes.crossEpoch)
    $contentMismatchEntries = @($nativeDocument.negativeProbes.contentMismatch)
    if ($crossEpochEntries.Count -ne $Sessions.Count -or
        $contentMismatchEntries.Count -ne $Sessions.Count) {
        throw 'Lockstep-v2 native evidence does not contain one observed negative proof per title.'
    }
    $validatedCrossEpoch = @()
    $validatedContentMismatch = @()
    $sessionRecords = @()
    for ($sessionIndex = 0; $sessionIndex -lt $Sessions.Count; ++$sessionIndex) {
        $session = $Sessions[$sessionIndex]
        $peerRecords = @($session.peers)
        $sessionMapCrc = [uint32]$validatedMapCrcs[$expectedTitles[$sessionIndex]]
        foreach ($field in @('peerCount', 'networkRosterMask',
            'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount')) {
            if (-not (Test-Stage5JsonInteger $session.$field)) {
                throw "Lockstep-v2 final-acceptance envelope session field '$field' must be a JSON integer."
            }
            if ($session.$field -lt 0 -or
                $session.$field -gt [UInt64][Int32]::MaxValue) {
                throw "Lockstep-v2 final-acceptance envelope session field '$field' is outside the Int32 range."
            }
        }
        if (-not (Test-Stage5JsonInteger $session.mapCrc)) {
            throw 'Lockstep-v2 final-acceptance envelope session field mapCrc must be a JSON integer.'
        }
        if ($session.mapCrc -lt 0 -or
            $session.mapCrc -gt [UInt64][UInt32]::MaxValue) {
            throw 'Lockstep-v2 final-acceptance envelope session mapCrc is outside the UInt32 range.'
        }
        if ($sessionIndex -lt 0 -or $sessionIndex -ge $expectedTitles.Count -or
            [string]$session.title -cne $expectedTitles[$sessionIndex] -or
            [uint32]$session.mapCrc -ne $sessionMapCrc -or
            [int]$session.peerCount -ne $PeerCount -or
            $peerRecords.Count -ne $PeerCount -or
            @($session.workerProfiles).Count -ne $PeerCount -or
            @($session.effectiveWorkerCounts).Count -ne $PeerCount -or
            [string]$session.sessionNonce -notmatch '^[0-9A-F]{32}$' -or
            -not [bool]$session.mixedWorkerProof -or
            -not [bool]$session.profileReadOnlyVerified -or
            [string]$session.comparableProjectionSha256 -notmatch '^[0-9A-F]{64}$') {
            throw 'Lockstep-v2 final-acceptance envelope session evidence is incomplete or substituted.'
        }
        foreach ($peer in $peerRecords) {
            foreach ($field in @('peerCount', 'networkRosterMask',
                'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount')) {
                if (-not (Test-Stage5JsonInteger $peer.$field)) {
                    throw "Lockstep-v2 final-acceptance envelope peer field '$field' must be a JSON integer."
                }
                if ($peer.$field -lt 0 -or
                    $peer.$field -gt [UInt64][Int32]::MaxValue) {
                    throw "Lockstep-v2 final-acceptance envelope peer field '$field' is outside the Int32 range."
                }
            }
            if ([int]$peer.peerCount -ne $PeerCount -or
                [int]$peer.networkRosterMask -ne $LockstepNetworkRosterMask -or
                [int]$peer.simulationRosterMask -ne $LockstepSimulationRosterMask -or
                [int]$peer.aiRosterMask -ne $LockstepAIRosterMask -or
                [int]$peer.aiPlayerCount -ne $LockstepAIPlayerCount) {
                throw 'Lockstep-v2 final-acceptance envelope peer roster evidence is incomplete or substituted.'
            }
        }
        $expectedExecutableSha256 = [string]$peerRecords[0].executableSha256
        if ($expectedExecutableSha256 -notmatch '^[0-9A-F]{64}$') {
            throw "Lockstep-v2 $($session.title) peer evidence has no executable identity for negative proofs."
        }
        $crossEntry = @($crossEpochEntries | Where-Object {
            [string]$_.title -ceq [string]$session.title
        })
        $contentEntry = @($contentMismatchEntries | Where-Object {
            [string]$_.title -ceq [string]$session.title
        })
        if ($crossEntry.Count -ne 1 -or $contentEntry.Count -ne 1) {
            throw "Lockstep-v2 $($session.title) is missing a unique observed negative proof pair."
        }
        $expectedExecutablePath = $null
        if ($peerRecords[0].PSObject.Properties.Name -contains 'launcherEquivalence' -and
            $null -ne $peerRecords[0].launcherEquivalence) {
            $expectedExecutablePath = [string]$peerRecords[0].launcherEquivalence.directExecutable
        }
        $validatedCrossEpoch += Assert-LockstepNegativeProbeEvidence `
            ([pscustomobject]$crossEntry[0]) `
            (Split-Path -Parent $nativeFull) $session.title `
            'negative-cross-epoch' $SourceCommit $expectedExecutableSha256 $sessionMapCrc $Seed `
            $expectedExecutablePath
        $validatedContentMismatch += Assert-LockstepNegativeProbeEvidence `
            ([pscustomobject]$contentEntry[0]) `
            (Split-Path -Parent $nativeFull) $session.title `
            'negative-content-mismatch' $SourceCommit $expectedExecutableSha256 $sessionMapCrc $Seed `
            $expectedExecutablePath
        foreach ($effectiveWorkerCount in @($session.effectiveWorkerCounts)) {
            if (-not (Test-Stage5JsonInteger $effectiveWorkerCount)) {
                throw 'Lockstep-v2 final-acceptance envelope effective worker counts must contain only JSON integers.'
            }
            if ($effectiveWorkerCount -lt 0 -or
                $effectiveWorkerCount -gt [UInt64][Int32]::MaxValue) {
                throw 'Lockstep-v2 final-acceptance envelope effective worker count is outside the Int32 range.'
            }
        }
        $effectiveCounts = @($session.effectiveWorkerCounts | ForEach-Object {
            [int]$_
        })
        if (@($effectiveCounts | Select-Object -Unique).Count -lt 2 -or
            @($effectiveCounts | Where-Object { $_ -lt 2 }).Count -gt 0) {
            throw 'Lockstep-v2 final-acceptance envelope did not preserve mixed effective worker evidence.'
        }
        $sessionRecords += [ordered]@{
            title = [string]$session.title
            mapCrc = $sessionMapCrc
            sessionNonce = [string]$session.sessionNonce
            peerCount = [int]$session.peerCount
            peerRecordCount = $peerRecords.Count
            networkRosterMask = $LockstepNetworkRosterMask
            simulationRosterMask = $LockstepSimulationRosterMask
            aiRosterMask = $LockstepAIRosterMask
            aiPlayerCount = $LockstepAIPlayerCount
            workerProfiles = @($session.workerProfiles | ForEach-Object {
                [string]$_.requestedWorkers
            })
            effectiveWorkerCounts = $effectiveCounts
            mixedWorkerProof = [bool]$session.mixedWorkerProof
            comparableProjectionSha256 = [string]$session.comparableProjectionSha256
            profileReadOnlyVerified = [bool]$session.profileReadOnlyVerified
        }
    }
    $peerRecordCount = [int](@($sessionRecords | ForEach-Object {
        [int]$_.peerRecordCount
    } | Measure-Object -Sum).Sum)
    $effectiveWorkerCounts = @($sessionRecords | ForEach-Object {
        @($_.effectiveWorkerCounts)
    } | ForEach-Object { [int]$_ })
    $envelope = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'mixed-worker-multiplayer'
        status = 'passed'
        sourceCommit = $SourceCommit
        title = 'Both'
        architecture = 'x64'
        artifactSetSha256 = $ArtifactSetSha256.ToUpperInvariant()
        recordedUtc = $RecordedUtc
        cohortNonce = $CohortNonce
        runtimeClosure = $validatedRuntimeClosure
        attachments = @([ordered]@{
            role = 'multiplayer-results'
            path = [IO.Path]::GetFileName($nativeFull)
            sha256 = $nativeSha256
            trustDomain = 'host-runner'
        })
        details = [ordered]@{
            nativeEvidenceKind = 'lockstep-v2-multiplayer'
            producer = $LockstepProducer
            nativeEvidenceSha256 = $nativeSha256
            networkRosterMask = $LockstepNetworkRosterMask
            simulationRosterMask = $LockstepSimulationRosterMask
            aiRosterMask = $LockstepAIRosterMask
            aiPlayerCount = $LockstepAIPlayerCount
            title = 'Both'
            sessionCount = $Sessions.Count
            peerCount = $PeerCount
            commonStopFrame = $CommonStopFrame
            allMatchesCompleted = $true
            stateTracesIdentical = $true
            crossEpochRejected = ($validatedCrossEpoch.Count -eq $Sessions.Count)
            contentMismatchRejected = ($validatedContentMismatch.Count -eq $Sessions.Count)
        }
    }
    return [pscustomobject]@{
        document = $envelope
        nativeEvidenceSha256 = $nativeSha256
    }
}

function New-SyntheticNegativeProbeEvidence {
    param(
        [string]$Root,
        [string]$Title,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$Mode,
        [int]$ProcessId,
        [uint32]$MapCrc,
        [int]$Seed
    )
    $titleRoot = Join-Path $Root $Title
    Ensure-LockstepHostSelfTestDirectory $titleRoot `
        'lockstep host self-test title directory'
    $negativeRoot = Join-Path $titleRoot 'NegativeProbes'
    Ensure-LockstepHostSelfTestDirectory $negativeRoot `
        'lockstep host self-test negative-probe directory'
    $isCrossEpoch = $Mode -ceq 'negative-cross-epoch'
    $expectedError = if ($isCrossEpoch) {
        'UnsupportedEngineEpoch'
    }
    else { 'ContentHashMismatch' }
    $mutation = if ($isCrossEpoch) { 'engine-epoch' } else { 'content-hash' }
    $receiptName = if ($isCrossEpoch) { 'cross-epoch.proof' } else { 'content-mismatch.proof' }
    $proofPath = Join-Path $negativeRoot $receiptName
    $stdoutPath = Join-Path $negativeRoot ($Mode + '.stdout.log')
    $stderrPath = Join-Path $negativeRoot ($Mode + '.stderr.log')
    $runNonce = [string]::new('A', 24) + ('{0:X8}' -f $ProcessId)
    $sessionNonce = [string]::new('B', 24) + ('{0:X8}' -f ($ProcessId + 10))
    $baselineInputSha256 = if ($isCrossEpoch) {
        [string]::new('1', 64)
    }
    else { [string]::new('3', 64) }
    $inputSha256 = if ($isCrossEpoch) {
        [string]::new('2', 64)
    }
    else { [string]::new('4', 64) }
    $proofText = @(
        $LockstepNegativeProbeMagic
        "producer=$LockstepProducer"
        "mode=$Mode"
        'schema=2'
        'protocol_epoch=2'
        "run_nonce=$runNonce"
        "session_nonce=$sessionNonce"
        "executable_sha256=$ExecutableSha256"
        "source_revision=$SourceCommit"
        "probe_build_compatibility_crc=$MapCrc"
        "probe_content_crc=$Seed"
        "mutation=$mutation"
        "baseline_input_sha256=$baselineInputSha256"
        "input_sha256=$inputSha256"
        'baseline_accepted=1'
        'mutated_accepted=0'
        "expected_error=$expectedError"
        "observed_error=$expectedError"
        "process_id=$ProcessId"
        'END'
    ) -join "`n"
    Write-LockstepHostSelfTestText $proofPath ($proofText + "`n")
    Write-LockstepHostSelfTestText $stdoutPath ("LOCKSTEP_V2_NEGATIVE_PROBE_PASS mode=$Mode pid=$ProcessId rejection=$expectedError`n")
    Write-LockstepHostSelfTestText $stderrPath ''
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $entry = [ordered]@{
        title = $Title
        mode = $Mode
        producer = $LockstepProducer
        processId = $ProcessId
        processCreationUtc = '2026-09-02T00:00:00.0000000Z'
        executablePath = [IO.Path]::GetFullPath((Join-Path $Root ("$Title.exe")))
        runNonce = $runNonce
        sessionNonce = $sessionNonce
        executableSha256 = $ExecutableSha256
        sourceCommit = $SourceCommit
        proofPath = ([IO.Path]::GetFullPath($proofPath).Substring($rootFull.Length).Replace('\', '/'))
        proofSha256 = Get-UpperSha256 $proofPath
        stdoutPath = ([IO.Path]::GetFullPath($stdoutPath).Substring($rootFull.Length).Replace('\', '/'))
        stdoutSha256 = Get-UpperSha256 $stdoutPath
        stderrPath = ([IO.Path]::GetFullPath($stderrPath).Substring($rootFull.Length).Replace('\', '/'))
        stderrSha256 = Get-UpperSha256 $stderrPath
        inputSha256 = $inputSha256
        baselineAccepted = $true
        mutatedAccepted = $false
        mutation = $mutation
        expectedError = $expectedError
        observedError = $expectedError
        exitCode = 0
        commandLine = "generalsv.exe -installedLockstepV2Validation mode=$Mode"
        arguments = @('-installedLockstepV2Validation', "mode=$Mode")
        probeBuildCrc = [uint32]$MapCrc
        probeContentCrc = [uint32]$Seed
    }
    return [pscustomobject]$entry
}

function Invoke-SelfTest {
    if (-not (Test-CanonicalHex ('A' * 32) 32) -or
        (Test-CanonicalHex ('A' * 31) 32) -or
        -not (Test-LowerHex40 ('a' * 40)) -or
        (Test-LowerHex40 ('A' * 40))) {
        throw 'Lockstep-v2 host nonce/source lexical self-test failed.'
    }
    $canonicalMapName = 'Maps\Twilight Flame\Twilight Flame.map'
    $canonicalMapCrcs = [ordered]@{
        Generals = [uint32]739101722
        ZeroHour = [uint32]4042777579
    }
    if (-not (Test-SafeMapName $canonicalMapName)) {
        throw 'Lockstep-v2 host map-name guard rejected the canonical spaced 2v4 map.'
    }
    foreach ($unsafeMapName in @(
        "Maps\Twilight`tFlame\Twilight Flame.map",
        "Maps\Twilight`nFlame\Twilight Flame.map",
        'Maps\Twilight;Flame\Twilight Flame.map',
        'Maps\Twilight"Flame\Twilight Flame.map',
        'Maps\..\Twilight Flame.map',
        '\Maps\Twilight Flame\Twilight Flame.map')) {
        if (Test-SafeMapName $unsafeMapName) {
            throw "Lockstep-v2 host map-name guard accepted an unsafe map: $unsafeMapName"
        }
    }
    $canonicalConfiguration = Build-LockstepConfiguration 0 2 `
        ([UInt64[]]@(41000, 41001)) ('1' * 32) ('2' * 32) ('A' * 64) `
        ('a' * 40) $canonicalMapName $canonicalMapCrcs.Generals 23063 `
        'H:\Stage5WeeklyPromotionQualification\Evidence\Generals' `
        'lockstep-v2-Generals-peer-0.receipt'
    if (-not $canonicalConfiguration.Contains(
            "map=$canonicalMapName;map_crc=$($canonicalMapCrcs.Generals);")) {
        throw 'Lockstep-v2 host configuration did not preserve the canonical spaced map and title CRC.'
    }
    # The production contract deliberately keeps installed qualification on
    # task-owned H:. The host self-test itself must also run on hosted Windows
    # runners where H: is not mounted, so its disposable fixture uses the OS
    # temp volume. Test-SafeHDirectory admits only this exact, bounded root
    # while -SelfTest is active; all production paths remain H:-only.
    $root = Join-Path ([IO.Path]::GetTempPath()) `
        ('GGC-LockstepV2HostSelfTest-' + [guid]::NewGuid().ToString('N'))
    $script:LockstepHostSelfTestScratchRoot = [IO.Path]::GetFullPath($root).TrimEnd('\')
    Set-Stage5LockstepHostSelfTestScratchRoot $script:LockstepHostSelfTestScratchRoot
    try {
        Ensure-LockstepHostSelfTestDirectory $root `
            'lockstep host self-test root' -Fresh
        $runtime = Join-Path $root 'runtime'
        Ensure-LockstepHostSelfTestDirectory $runtime `
            'lockstep host self-test runtime' -Fresh
        $fixtureExecutable = Join-Path $runtime 'generalsv.exe'
        $fixtureLauncher = Join-Path $runtime 'launcher.exe'
        $fixtureConfig = Join-Path $runtime 'launcher.lcf'
        Write-LockstepHostSelfTestText $fixtureExecutable 'fixture executable'
        Write-LockstepHostSelfTestText $fixtureLauncher 'fixture launcher'
        Write-LockstepHostSelfTestText $fixtureConfig `
            'RUN = . generalsv.exe -simulationMode parallel -workerPolicy auto'
        $launcher = Get-LauncherRunContract $fixtureConfig $fixtureLauncher $runtime `
            $fixtureExecutable ('A' * 64) ('B' * 64)
        if ($launcher.directory -cne '.' -or
            $launcher.executable -cne 'generalsv.exe' -or
            (@($launcher.launcherArguments) -join '|') -cne
                '-simulationMode|parallel|-workerPolicy|auto') {
            throw 'Lockstep-v2 launcher-equivalence self-test did not retain Stage 5 defaults.'
        }
        $x64Rejected = $false
        try { Assert-X64PeExecutable $fixtureExecutable }
        catch { $x64Rejected = $true }
        if (-not $x64Rejected) {
            throw 'Lockstep-v2 host self-test accepted a non-PE installed executable fixture.'
        }
        $missingSwitchRejected = $false
        try { Assert-HeadlessDirectExecutionOptIn $false }
        catch { $missingSwitchRejected = $true }
        if (-not $missingSwitchRejected) {
            throw 'Lockstep-v2 host self-test accepted direct execution without its opt-in switch.'
        }
        $workerProfiles = @(Get-LockstepWorkerProfiles 2)
        Assert-MixedLockstepWorkerProfiles $workerProfiles
        if ((@($workerProfiles[0].overrideArguments) -join '|') -cne
                '-workerCount|2|-workerPolicy|all' -or
            (@($workerProfiles[1].overrideArguments) -join '|') -cne
                '-workerPolicy|auto') {
            throw 'Lockstep-v2 worker self-test did not bind distinct reviewed overrides.'
        }
        $homogeneousRejected = $false
        try { Assert-MixedLockstepWorkerProfiles @($workerProfiles[0], $workerProfiles[0]) }
        catch { $homogeneousRejected = $true }
        if (-not $homogeneousRejected) {
            throw 'Lockstep-v2 host self-test accepted homogeneous worker profiles.'
        }
        $adapterNativePath = Join-Path $root 'LockstepV2LoopbackEvidence.json'
        $syntheticSourceCommit = [string]::new('a', 40)
        $qualificationDataRoot = Join-Path $root 'QualificationDataFixture'
        Ensure-LockstepHostSelfTestDirectory $qualificationDataRoot `
            'qualification data self-test root' -Fresh
        foreach ($runtimeLeaf in @('GeneralsRuntime', 'ZeroHourRuntime')) {
            $fixtureRuntimeRoot = Join-Path $qualificationDataRoot $runtimeLeaf
            Ensure-LockstepHostSelfTestDirectory $fixtureRuntimeRoot `
                'qualification data self-test runtime' -Fresh
            $fixtureDataRoot = Join-Path $fixtureRuntimeRoot 'Data'
            Ensure-LockstepHostSelfTestDirectory $fixtureDataRoot `
                'qualification data self-test Data root' -Fresh
            Ensure-LockstepHostSelfTestDirectory `
                (Join-Path $fixtureDataRoot 'Scripts') `
                'qualification data self-test Scripts root' -Fresh
        }
        $qualificationDataFiles = [ordered]@{
            Generals = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
                'Data/Scripts/MultiplayerScripts.scb',
                'Data/Scripts/SkirmishScripts.scb')
            ZeroHour = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
                'Data/Scripts/MultiplayerScripts.scb',
                'Data/Scripts/Scripts.ini',
                'Data/Scripts/SkirmishScripts.scb')
        }
        $qualificationDataEntriesByIdentity = @{}
        foreach ($title in $qualificationDataFiles.Keys) {
            $runtimeLeaf = if ($title -ceq 'Generals') {
                'GeneralsRuntime'
            }
            else { 'ZeroHourRuntime' }
            foreach ($relativeDataPath in $qualificationDataFiles[$title]) {
                $manifestRelative = "$runtimeLeaf/$relativeDataPath"
                $fixtureDataPath = Join-Path $qualificationDataRoot `
                    $manifestRelative
                Write-LockstepHostSelfTestText $fixtureDataPath `
                    "reviewed fixture $title $relativeDataPath"
                $identity = "$title|$manifestRelative"
                $qualificationDataEntriesByIdentity[$identity] = [ordered]@{
                    title = $title
                    path = $manifestRelative
                    sha256 = Get-UpperSha256 $fixtureDataPath
                }
            }
        }
        [string[]]$qualificationDataIdentities = @(
            $qualificationDataEntriesByIdentity.Keys)
        [Array]::Sort($qualificationDataIdentities, [StringComparer]::Ordinal)
        $qualificationDataEntries = @($qualificationDataIdentities |
            ForEach-Object { $qualificationDataEntriesByIdentity[$_] })
        $qualificationDataCanonicalLines = @($qualificationDataEntries |
            ForEach-Object {
                '{0}|{1}|{2}' -f $_.title, $_.path, $_.sha256
            })
        $qualificationDataClosureSha256 = Get-LockstepTextSha256 `
            (($qualificationDataCanonicalLines -join "`n") + "`n")
        $qualificationDataManifestPath = Join-Path $qualificationDataRoot `
            'Stage5QualificationData.json'
        $qualificationDataDocument = [ordered]@{
            schemaVersion = 2
            evidenceKind = 'lockstep-v2-qualification-data'
            producer = 'genci-r2-trimmed-data'
            sourceCommit = $syntheticSourceCommit
            productSet = @('Generals', 'ZeroHour')
            mapName = $canonicalMapName
            mapCrcs = $canonicalMapCrcs
            archiveSources = @(
                [ordered]@{
                    title = 'Generals'
                    object = 's3://github-ci/generals108_gamedata_trimmed.7z'
                    sha256 = $LockstepGeneralsDataArchiveSha256
                },
                [ordered]@{
                    title = 'ZeroHour'
                    object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
                    sha256 = $LockstepZeroHourDataArchiveSha256
                })
            files = $qualificationDataEntries
            closureSha256 = $qualificationDataClosureSha256
        }
        Write-LockstepHostSelfTestText $qualificationDataManifestPath `
            ($qualificationDataDocument | ConvertTo-Json -Depth 8)
        $qualificationDataManifestSha256 = Get-UpperSha256 `
            $qualificationDataManifestPath
        $qualificationDataBinding = Read-AndValidateQualificationData `
            -Manifest $qualificationDataManifestPath `
            -ExpectedManifestSha256 $qualificationDataManifestSha256 `
            -ExpectedClosureSha256 $qualificationDataClosureSha256 `
            -ExpectedSourceCommit $syntheticSourceCommit `
            -ExpectedMapName $canonicalMapName `
            -ExpectedMapCrcs $canonicalMapCrcs `
            -RuntimeFiles @()
        if ($qualificationDataBinding.manifestSha256 -cne
                $qualificationDataManifestSha256 -or
            $qualificationDataBinding.closureSha256 -cne
                $qualificationDataClosureSha256 -or
            $qualificationDataBinding.fileCount -ne 12) {
            throw 'Qualification data self-test did not validate the exact reviewed closure.'
        }
        $qualificationDataOriginalText = [IO.File]::ReadAllText(
            $qualificationDataManifestPath)
        $qualificationDataWrongTitleCrc = $qualificationDataOriginalText |
            ConvertFrom-Json
        $qualificationDataWrongTitleCrc.mapCrcs.ZeroHour =
            $canonicalMapCrcs.Generals
        Write-LockstepHostSelfTestText $qualificationDataManifestPath `
            ($qualificationDataWrongTitleCrc | ConvertTo-Json -Depth 8)
        $asymmetricMapCrcRejected = $false
        try {
            Read-AndValidateQualificationData `
                -Manifest $qualificationDataManifestPath `
                -ExpectedManifestSha256 (Get-UpperSha256 $qualificationDataManifestPath) `
                -ExpectedClosureSha256 $qualificationDataClosureSha256 `
                -ExpectedSourceCommit $syntheticSourceCommit `
                -ExpectedMapName $canonicalMapName `
                -ExpectedMapCrcs $canonicalMapCrcs `
                -RuntimeFiles @() | Out-Null
        }
        catch { $asymmetricMapCrcRejected = $true }
        Write-LockstepHostSelfTestText $qualificationDataManifestPath `
            $qualificationDataOriginalText
        if (-not $asymmetricMapCrcRejected) {
            throw 'Qualification data self-test accepted a Zero Hour CRC substituted with the Generals CRC.'
        }
        $missingQualificationDataHashRejected = $false
        try {
            Read-AndValidateQualificationData `
                -Manifest $qualificationDataManifestPath `
                -ExpectedManifestSha256 '' `
                -ExpectedClosureSha256 $qualificationDataClosureSha256 `
                -ExpectedSourceCommit $syntheticSourceCommit `
                -ExpectedMapName $canonicalMapName `
                -ExpectedMapCrcs $canonicalMapCrcs `
                -RuntimeFiles @() | Out-Null
        }
        catch { $missingQualificationDataHashRejected = $true }
        if (-not $missingQualificationDataHashRejected) {
            throw 'Qualification data self-test accepted a missing independent manifest hash.'
        }
        $tamperedQualificationDataPath = Join-Path $qualificationDataRoot `
            'GeneralsRuntime/Maps.big'
        Write-LockstepHostSelfTestText $tamperedQualificationDataPath `
            'tampered reviewed fixture'
        $tamperedQualificationDataRejected = $false
        try {
            Read-AndValidateQualificationData `
                -Manifest $qualificationDataManifestPath `
                -ExpectedManifestSha256 $qualificationDataManifestSha256 `
                -ExpectedClosureSha256 $qualificationDataClosureSha256 `
                -ExpectedSourceCommit $syntheticSourceCommit `
                -ExpectedMapName $canonicalMapName `
                -ExpectedMapCrcs $canonicalMapCrcs `
                -RuntimeFiles @() | Out-Null
        }
        catch { $tamperedQualificationDataRejected = $true }
        if (-not $tamperedQualificationDataRejected) {
            throw 'Qualification data self-test accepted a changed staged data file.'
        }
        $syntheticCrossGenerals = New-SyntheticNegativeProbeEvidence $root `
            'Generals' ([string]::new('A', 64)) $syntheticSourceCommit `
            'negative-cross-epoch' 1001 $canonicalMapCrcs.Generals 23063
        $syntheticContentGenerals = New-SyntheticNegativeProbeEvidence $root `
            'Generals' ([string]::new('A', 64)) $syntheticSourceCommit `
            'negative-content-mismatch' 1002 $canonicalMapCrcs.Generals 23063
        $syntheticCrossZeroHour = New-SyntheticNegativeProbeEvidence $root `
            'ZeroHour' ([string]::new('B', 64)) $syntheticSourceCommit `
            'negative-cross-epoch' 1003 $canonicalMapCrcs.ZeroHour 23063
        $syntheticContentZeroHour = New-SyntheticNegativeProbeEvidence $root `
            'ZeroHour' ([string]::new('B', 64)) $syntheticSourceCommit `
            'negative-content-mismatch' 1004 $canonicalMapCrcs.ZeroHour 23063
        $selfTestCohortNonce = [Guid]::NewGuid().ToString()
        $selfTestCohortCreatedUtc = '2026-09-02T00:00:00.0000000Z'
        $selfTestRuntimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = [string]::new('D', 64)
            closureSha256 = [string]::new('E', 64)
        }
        $adapterSessions = @(
            [pscustomobject]@{
                title = 'Generals'; mapCrc = $canonicalMapCrcs.Generals
                peerCount = 2; sessionNonce = [string]::new('1', 32)
                peers = @(
                    [pscustomobject]@{ peerCount = 2; networkRosterMask = 3
                        simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
                        executableSha256 = [string]::new('A', 64)
                        sourceCommit = $syntheticSourceCommit },
                    [pscustomobject]@{ peerCount = 2; networkRosterMask = 3
                        simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
                        executableSha256 = [string]::new('A', 64)
                        sourceCommit = $syntheticSourceCommit })
                workerProfiles = $workerProfiles
                effectiveWorkerCounts = @(2, 4); mixedWorkerProof = $true
                comparableProjectionSha256 = [string]::new('C', 64)
                profileReadOnlyVerified = $true
            },
            [pscustomobject]@{
                title = 'ZeroHour'; mapCrc = $canonicalMapCrcs.ZeroHour
                peerCount = 2; sessionNonce = [string]::new('2', 32)
                peers = @(
                    [pscustomobject]@{ peerCount = 2; networkRosterMask = 3
                        simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
                        executableSha256 = [string]::new('B', 64)
                        sourceCommit = $syntheticSourceCommit },
                    [pscustomobject]@{ peerCount = 2; networkRosterMask = 3
                        simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
                        executableSha256 = [string]::new('B', 64)
                        sourceCommit = $syntheticSourceCommit })
                workerProfiles = $workerProfiles
                effectiveWorkerCounts = @(2, 4); mixedWorkerProof = $true
                comparableProjectionSha256 = [string]::new('D', 64)
                profileReadOnlyVerified = $true
            })
        $adapterNativeDocument = [ordered]@{
            schemaVersion = 2
            evidenceKind = 'lockstep-v2-multiplayer'
            producer = $LockstepProducer
            cohortNonce = $selfTestCohortNonce
            runtimeClosure = $selfTestRuntimeClosure
            titleSessionDisposition = $LockstepTitleSessionDisposition
            mapName = $canonicalMapName
            mapCrcs = $canonicalMapCrcs
            negativeProbes = [ordered]@{
                crossEpoch = @($syntheticCrossGenerals, $syntheticCrossZeroHour)
                contentMismatch = @($syntheticContentGenerals, $syntheticContentZeroHour)
            }
        }
        Write-LockstepHostSelfTestText $adapterNativePath `
            ($adapterNativeDocument | ConvertTo-Json -Depth 12)
        $adapter = New-LockstepV2FinalAcceptanceEnvelope `
            -NativeEvidencePath $adapterNativePath `
            -SourceCommit ('a' * 40) `
            -ArtifactSetSha256 ('B' * 64) `
            -RecordedUtc '2026-09-02T00:00:00Z' `
            -MapName $canonicalMapName -MapCrcs $canonicalMapCrcs -Seed 23063 `
            -PeerCount 2 -Sessions $adapterSessions `
            -CohortNonce $selfTestCohortNonce `
            -CohortCreatedUtc $selfTestCohortCreatedUtc `
            -RuntimeClosure $selfTestRuntimeClosure
        $adapterAttachment = $adapter.document.attachments[0]
        if (-not (Test-Stage5JsonInteger $adapter.document.schemaVersion) -or
            $adapter.document.schemaVersion -ne 1 -or
            $adapter.document.evidenceKind -cne 'mixed-worker-multiplayer' -or
            $adapter.document.title -cne 'Both' -or
            $adapter.document.details.producer -cne 'installed-lockstep-v2' -or
            $adapter.document.details.nativeEvidenceKind -cne 'lockstep-v2-multiplayer' -or
            $adapter.document.details.networkRosterMask -ne 3 -or
            $adapter.document.details.simulationRosterMask -ne 63 -or
            $adapter.document.details.aiRosterMask -ne 60 -or
            $adapter.document.details.aiPlayerCount -ne 4 -or
            $adapter.document.details.sessionCount -ne 2 -or
            $adapter.document.details.title -cne 'Both' -or
            $adapter.document.details.peerCount -ne 2 -or
            $adapter.document.details.commonStopFrame -ne 4096 -or
            -not $adapter.document.details.allMatchesCompleted -or
            -not $adapter.document.details.stateTracesIdentical -or
            -not $adapter.document.details.crossEpochRejected -or
            -not $adapter.document.details.contentMismatchRejected -or
            $adapterAttachment.role -cne 'multiplayer-results' -or
            $adapterAttachment.path -cne 'LockstepV2LoopbackEvidence.json' -or
            $adapterAttachment.trustDomain -cne 'host-runner' -or
            $adapterAttachment.sha256 -cne (Get-UpperSha256 $adapterNativePath) -or
            $adapterNativeDocument.titleSessionDisposition -cne
                'removed-after-peer-exit-before-evidence-persist') {
            throw 'Lockstep-v2 host self-test did not bind the v2 child to the final-acceptance host envelope.'
        }
        $closureFixtureRoot = Join-Path $root 'EvidenceClosureFixture'
        Ensure-LockstepHostSelfTestDirectory $closureFixtureRoot `
            'lockstep-v2 evidence closure self-test root' -Fresh
        $closureFixtureTitleRoot = Join-Path $closureFixtureRoot 'Generals'
        Ensure-LockstepHostSelfTestDirectory $closureFixtureTitleRoot `
            'lockstep-v2 evidence closure self-test title root' -Fresh
        Write-LockstepHostSelfTestText `
            (Join-Path $closureFixtureRoot 'LockstepV2LoopbackEvidence.json') `
            'native evidence'
        Write-LockstepHostSelfTestText `
            (Join-Path $closureFixtureRoot 'mixed-worker-multiplayer.json') `
            'final acceptance envelope'
        Write-LockstepHostSelfTestText `
            (Join-Path $closureFixtureRoot $LockstepQualificationDataEvidenceLeaf) `
            'qualification data closure'
        $closureFixtureRawPath = Join-Path $closureFixtureTitleRoot 'peer-0.raw.json'
        Write-LockstepHostSelfTestText $closureFixtureRawPath 'raw peer evidence'
        $closureResult = New-LockstepV2EvidenceClosure `
            -EvidenceRoot $closureFixtureRoot `
            -SourceCommit $syntheticSourceCommit `
            -ArtifactSetSha256 ([string]::new('B', 64)) `
            -CohortNonce $selfTestCohortNonce
        $closureDocument = [IO.File]::ReadAllText($closureResult.path) |
            ConvertFrom-Json
        $closurePaths = @($closureDocument.files | ForEach-Object {
            [string]$_.path
        })
        if (-not (Test-Stage5JsonInteger $closureDocument.schemaVersion) -or
            $closureDocument.schemaVersion -ne 1 -or
            $closureDocument.evidenceKind -cne 'lockstep-v2-evidence-closure' -or
            $closureDocument.producer -cne $LockstepProducer -or
            $closureDocument.sourceCommit -cne $syntheticSourceCommit -or
            $closureDocument.artifactSetSha256 -cne ([string]::new('B', 64)) -or
            $closureDocument.cohortNonce -cne $selfTestCohortNonce -or
            $closureDocument.canonicalEvidenceRoot -cne
                [IO.Path]::GetFullPath($closureFixtureRoot).TrimEnd('\') -or
            $closureDocument.fileCount -ne 4 -or
            ($closurePaths -join '|') -cne
                'Generals/peer-0.raw.json|LockstepV2LoopbackEvidence.json|QualificationData.json|mixed-worker-multiplayer.json' -or
            $closurePaths -ccontains $LockstepEvidenceClosureLeaf -or
            $closureDocument.closureSha256 -cne $closureResult.closureSha256 -or
            $closureResult.sha256 -cne (Get-UpperSha256 $closureResult.path)) {
            throw 'Lockstep-v2 host self-test did not emit the exact deterministic evidence closure.'
        }
        $rawBinding = @($closureDocument.files | Where-Object {
            [string]$_.path -ceq 'Generals/peer-0.raw.json'
        })
        if ($rawBinding.Count -ne 1 -or
            [string]$rawBinding[0].sha256 -cne (Get-UpperSha256 $closureFixtureRawPath)) {
            throw 'Lockstep-v2 host self-test did not hash-bind its raw evidence member.'
        }
        $duplicateClosureRejected = $false
        try {
            New-LockstepV2EvidenceClosure `
                -EvidenceRoot $closureFixtureRoot `
                -SourceCommit $syntheticSourceCommit `
                -ArtifactSetSha256 ([string]::new('B', 64)) `
                -CohortNonce $selfTestCohortNonce | Out-Null
        }
        catch { $duplicateClosureRejected = $true }
        if (-not $duplicateClosureRejected) {
            throw 'Lockstep-v2 host self-test allowed its closure sidecar to enter its own closure.'
        }
        $tamperedProofPath = Join-Path $root 'Generals\NegativeProbes\cross-epoch.proof'
        $originalTamperedProof = [IO.File]::ReadAllText($tamperedProofPath)
        $tamperRejected = $false
        try {
            Write-LockstepHostSelfTestText $tamperedProofPath `
                ($originalTamperedProof.Replace(
                    'observed_error=UnsupportedEngineEpoch',
                    'observed_error=ContentHashMismatch'))
            try {
                New-LockstepV2FinalAcceptanceEnvelope `
                    -NativeEvidencePath $adapterNativePath `
                    -SourceCommit $syntheticSourceCommit `
                    -ArtifactSetSha256 ([string]::new('B', 64)) `
                    -RecordedUtc '2026-09-02T00:00:00Z' `
                    -MapName $canonicalMapName -MapCrcs $canonicalMapCrcs -Seed 23063 `
                    -PeerCount 2 -Sessions $adapterSessions `
                    -CohortNonce $selfTestCohortNonce `
                    -CohortCreatedUtc $selfTestCohortCreatedUtc `
                    -RuntimeClosure $selfTestRuntimeClosure | Out-Null
            }
            catch { $tamperRejected = $true }
        }
        finally {
            Write-LockstepHostSelfTestText $tamperedProofPath $originalTamperedProof
        }
        if (-not $tamperRejected) {
            throw 'Lockstep-v2 host self-test accepted a tampered negative raw proof.'
        }
        $adapterTopologyRejected = $false
        try {
            New-LockstepV2FinalAcceptanceEnvelope `
                -NativeEvidencePath $adapterNativePath -SourceCommit ('a' * 40) `
                -ArtifactSetSha256 ('B' * 64) -RecordedUtc '2026-09-02T00:00:00Z' `
                -MapName $canonicalMapName -MapCrcs $canonicalMapCrcs -Seed 23063 `
                -PeerCount 3 -Sessions $adapterSessions `
                -CohortNonce $selfTestCohortNonce `
                -CohortCreatedUtc $selfTestCohortCreatedUtc `
                -RuntimeClosure $selfTestRuntimeClosure | Out-Null
        }
        catch { $adapterTopologyRejected = $true }
        if (-not $adapterTopologyRejected) {
            throw 'Lockstep-v2 host self-test accepted a non-two-human adapter topology.'
        }
        $adapterRosterRejected = $false
        $adapterSessions[0].peers[0].aiRosterMask = 0
        try {
            New-LockstepV2FinalAcceptanceEnvelope `
                -NativeEvidencePath $adapterNativePath -SourceCommit ('a' * 40) `
                -ArtifactSetSha256 ('B' * 64) -RecordedUtc '2026-09-02T00:00:00Z' `
                -MapName $canonicalMapName -MapCrcs $canonicalMapCrcs -Seed 23063 `
                -PeerCount 2 -Sessions $adapterSessions `
                -CohortNonce $selfTestCohortNonce `
                -CohortCreatedUtc $selfTestCohortCreatedUtc `
                -RuntimeClosure $selfTestRuntimeClosure | Out-Null
        }
        catch { $adapterRosterRejected = $true }
        if (-not $adapterRosterRejected) {
            throw 'Lockstep-v2 host self-test accepted a substituted AI/network roster mask.'
        }
        $titleSession = New-LockstepTitleSessionContract 'Generals' `
            (Join-Path $root 'TitleSessionContract') `
            (Join-Path $root 'RuntimeContract')
        $peerEnvironment0 = Get-LockstepPeerEnvironment $titleSession 0
        $peerEnvironment1 = Get-LockstepPeerEnvironment $titleSession 1
        $selfTestDriveRoot = [IO.Path]::GetPathRoot($root)
        if ([IO.Path]::GetPathRoot($titleSession.profileRoot) -cne $selfTestDriveRoot -or
            @($titleSession.registryValues).Count -ne 1 -or
            $peerEnvironment0.values['TEMP'] -ceq $peerEnvironment1.values['TEMP'] -or
            [IO.Path]::GetPathRoot($peerEnvironment0.values['TEMP']) -cne $selfTestDriveRoot -or
            $peerEnvironment0.values['HOMEDRIVE'] -cne $selfTestDriveRoot.TrimEnd([char]92) -or
            @($titleSession.environmentVariableNames) -notcontains 'TEMP') {
            throw 'Lockstep-v2 host self-test did not isolate profile/environment paths to its bounded scratch drive.'
        }
        $scriptText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot `
            'Stage5InstalledLockstepV2Session.psm1'))
        if ($scriptText -notmatch 'Restore-LockstepRegistrySnapshots' -or
            $scriptText -notmatch 'Set-LockstepProcessEnvironment' -or
            $scriptText -notmatch 'Assert-Stage5ProcessLocalProfileCapability' -or
            $scriptText -notmatch 'Assert-LockstepProfileReadOnly' -or
            $scriptText -notmatch 'Remove-LockstepTitleSessionDirectories') {
            throw 'Lockstep-v2 host self-test did not retain registry/environment cleanup guards.'
        }
        $escapeCleanupRejected = $false
        try { Remove-LockstepTitleSessionDirectories $titleSession $root }
        catch { $escapeCleanupRejected = $true }
        if (-not $escapeCleanupRejected) {
            throw 'Lockstep-v2 host self-test accepted a title-session cleanup path outside the output root.'
        }
        $cleanupRoot = Join-Path $root 'TitleSession'
        $cleanupContract = New-LockstepTitleSessionContract 'Generals' $cleanupRoot $runtime
        Initialize-LockstepTitleSessionDirectories $cleanupContract
        Write-LockstepHostSelfTestText `
            (Join-Path $cleanupContract.profileRoot 'transient.bin') 'transient'
        $cleanupRan = $false
        try {
            try { throw 'synthetic qualification failure' }
            finally {
                Remove-LockstepTitleSessionDirectories $cleanupContract $root
                $cleanupRan = $true
            }
        }
        catch {
            if (-not $cleanupRan) {
                throw "Lockstep-v2 host self-test could not clean a thrown qualification path: $($_.Exception.Message)"
            }
        }
        if (Test-Path -LiteralPath $cleanupRoot) {
            throw 'Lockstep-v2 host self-test left disposable title-session directories behind.'
        }
        # Exercise cleanup's no-reparse guard when junction creation is available.
        $redirectTarget = Join-Path ([IO.Path]::GetTempPath()) `
            ('GGC-LockstepV2RedirectTarget-' + [guid]::NewGuid().ToString('N'))
        $redirectSentinel = Join-Path $redirectTarget 'sentinel.txt'
        $cleanupRedirectParent = Join-Path $root 'Adversarial'
        $cleanupRedirectRoot = Join-Path $cleanupRedirectParent 'TitleSession'
        $cleanupRedirect = Join-Path $cleanupRedirectRoot 'Redirected'
        try {
            Assert-LockstepNoReparse (Split-Path -Parent $redirectTarget) `
                'lockstep junction target parent'
            New-Item -Path $redirectTarget -ItemType Directory `
                -ErrorAction Stop | Out-Null
            Assert-LockstepNoReparse $redirectTarget 'lockstep junction target'
            [IO.File]::WriteAllText($redirectSentinel, 'must-survive-cleanup',
                (New-Object Text.UTF8Encoding($false)))
            Assert-LockstepNoReparse $redirectSentinel 'lockstep junction target sentinel'
            Ensure-LockstepHostSelfTestDirectory $cleanupRedirectParent 'adversarial cleanup parent'
            Ensure-LockstepHostSelfTestDirectory $cleanupRedirectRoot 'adversarial cleanup root'
            $junctionCreated = Try-NewLockstepDirectoryJunction `
                $cleanupRedirect $redirectTarget
            if ($junctionCreated) {
                $cleanupRejected = $false
                try {
                    Remove-LockstepTitleSessionDirectories `
                        ([pscustomobject]@{ sessionRoot = $cleanupRedirectRoot }) $root
                }
                catch { $cleanupRejected = $true }
                if (-not $cleanupRejected) {
                    throw 'Lockstep-v2 host cleanup followed a reparse child.'
                }
                if (-not (Test-Path -LiteralPath $redirectSentinel -PathType Leaf) -or
                    [IO.File]::ReadAllText($redirectSentinel) -cne
                        'must-survive-cleanup') {
                    throw 'Lockstep-v2 host cleanup touched a redirected target.'
                }
                Remove-LockstepDirectoryJunction $cleanupRedirect
            }
            Remove-LockstepTitleSessionDirectories `
                ([pscustomobject]@{ sessionRoot = $cleanupRedirectRoot }) $root
            if ($null -ne (Get-LockstepItemIfPresent $cleanupRedirectRoot)) {
                throw 'Lockstep-v2 host self-test left the adversarial cleanup root.'
            }
        }
        finally {
            $linkItem = Get-LockstepItemIfPresent $cleanupRedirect
            if ($null -ne $linkItem -and
                ($linkItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Remove-LockstepDirectoryJunction $cleanupRedirect
            }
            foreach ($cleanupPath in @($cleanupRedirectParent, $redirectTarget)) {
                if ($null -ne (Get-LockstepItemIfPresent $cleanupPath)) {
                    if ($cleanupPath -eq $redirectTarget) {
                        Remove-LockstepHostSelfTestTree $cleanupPath $null
                    }
                    else {
                        Remove-LockstepHostSelfTestTree $cleanupPath $root
                    }
                }
            }
        }
        $stdoutProof = Get-LockstepStdoutProof `
            "LOCKSTEP_V2_VALIDATION_ACTIVE peer=0 frame_limit=4096`nLOCKSTEP_V2_VALIDATION_PASS peer=0 pid=1 frame=4096 crc=00000001`n" 0
        if (-not $stdoutProof.executableOrigin -or $stdoutProof.pid -ne 1 -or
            $stdoutProof.frameLimit -ne 4096) {
            throw 'Lockstep-v2 host self-test did not bind executable-origin stdout markers.'
        }
        $negativeLauncherCases = @(
            'RUN = . other.exe -simulationMode parallel -workerPolicy auto',
            'RUN = child generalsv.exe -simulationMode parallel -workerPolicy auto',
            'RUN = . generalsv.exe -unsupported value')
        foreach ($badLine in $negativeLauncherCases) {
            Write-LockstepHostSelfTestText $fixtureConfig $badLine
            $rejected = $false
            try {
                [void](Get-LauncherRunContract $fixtureConfig $fixtureLauncher $runtime `
                    $fixtureExecutable ('A' * 64) ('B' * 64))
            }
            catch { $rejected = $true }
            if (-not $rejected) {
                throw "Lockstep-v2 launcher parser accepted a negative fixture: $badLine"
            }
        }
        $receiptPath = Join-Path $root 'synthetic.receipt'
        $text = New-SyntheticReceiptText
        Write-LockstepHostSelfTestText $receiptPath $text
        $parsed = Get-ReceiptPairs $receiptPath
        $pairs = $parsed.pairs
        if ($pairs['schema'] -cne '2' -or
            $pairs['common_stop_frame'] -cne '4096' -or
            $pairs['final_frame'] -cne '4096' -or
            $pairs['frame_count'] -cne '4096' -or
            $pairs['checkpoint_count'] -cne '129' -or
            $pairs['proven_kernel_mask'] -cne '63' -or
            $pairs['validation_authority_mask'] -cne '63') {
            throw 'Lockstep-v2 host parser did not retain schema/checkpoint fields.'
        }
        $digest = Get-ReceiptCommandDigest $parsed
        $text = $text.Replace('checkpoint_128_command_digest=1',
            "checkpoint_128_command_digest=$digest")
        Write-LockstepHostSelfTestText $receiptPath $text
        $parsed = Get-ReceiptPairs $receiptPath
        $aiDigest = Get-ReceiptAIPlanningDigest $parsed
        $text = $text.Replace('ai_planning_digest=1',
            "ai_planning_digest=$aiDigest")
        Write-LockstepHostSelfTestText $receiptPath $text
        $parsed = Get-ReceiptPairs $receiptPath
        [void](Assert-LockstepV2Receipt $parsed 0 2 1 `
            '0123456789ABCDEF0123456789ABCDEF' `
            'ABCDEF0123456789ABCDEF0123456789' `
            ('A' * 64) ('a' * 40))
        $explicitTelemetry = Get-LockstepReceiptWorkerTelemetry $parsed $workerProfiles[0]
        if ($explicitTelemetry.effectiveWorkers -ne 2) {
            throw 'Lockstep-v2 host self-test did not retain forced-two worker telemetry.'
        }
        $claimedMismatchRejected = $false
        try { [void](Get-LockstepReceiptWorkerTelemetry $parsed $workerProfiles[1]) }
        catch { $claimedMismatchRejected = $true }
        if (-not $claimedMismatchRejected) {
            throw 'Lockstep-v2 host self-test accepted an auto profile with forced-two telemetry.'
        }
        $peerReceiptPath = Join-Path $root 'synthetic-peer.receipt'
        $peerText = New-SyntheticReceiptText -LocalSlot 1 -PhysicalWorkerCount 4 `
            -RunNonce 'FEDCBA9876543210FEDCBA9876543210'
        Write-LockstepHostSelfTestText $peerReceiptPath $peerText
        $peerParsed = Get-ReceiptPairs $peerReceiptPath
        $peerDigest = Get-ReceiptCommandDigest $peerParsed
        $peerText = $peerText.Replace('checkpoint_128_command_digest=1',
            "checkpoint_128_command_digest=$peerDigest")
        Write-LockstepHostSelfTestText $peerReceiptPath $peerText
        $peerParsed = Get-ReceiptPairs $peerReceiptPath
        $peerAIDigest = Get-ReceiptAIPlanningDigest $peerParsed
        $peerText = $peerText.Replace('ai_planning_digest=1',
            "ai_planning_digest=$peerAIDigest")
        Write-LockstepHostSelfTestText $peerReceiptPath $peerText
        $peerParsed = Get-ReceiptPairs $peerReceiptPath
        [void](Assert-LockstepV2Receipt $peerParsed 1 2 1 `
            'FEDCBA9876543210FEDCBA9876543210' `
            'ABCDEF0123456789ABCDEF0123456789' `
            ('A' * 64) ('a' * 40))
        $automaticTelemetry = Get-LockstepReceiptWorkerTelemetry $peerParsed $workerProfiles[1]
        if ($automaticTelemetry.effectiveWorkers -ne 4 -or
            (Get-ComparableReceiptHash $parsed) -cne (Get-ComparableReceiptHash $peerParsed)) {
            throw 'Lockstep-v2 host self-test did not preserve mixed-worker receipt/projection semantics.'
        }
        $explicitMismatchRejected = $false
        try { [void](Get-LockstepReceiptWorkerTelemetry $peerParsed $workerProfiles[0]) }
        catch { $explicitMismatchRejected = $true }
        if (-not $explicitMismatchRejected) {
            throw 'Lockstep-v2 host self-test accepted an explicit-two profile with four-worker telemetry.'
        }
        $mutated = $text.Replace($LockstepMagic, 'RTS_NET3_RECEIPT')
        Write-LockstepHostSelfTestText $receiptPath $mutated
        $rejected = $false
        try { [void](Get-ReceiptPairs $receiptPath) } catch { $rejected = $true }
        if (-not $rejected) { throw 'Host parser accepted a v1 receipt magic.' }
        $sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
        $cmake = [IO.File]::ReadAllText((Join-Path $sourceRoot 'Core\Tools\DeterministicSimulationValidation\CMakeLists.txt'))
        if ($cmake -notmatch 'core_installed_lockstep_v2_host_self_test' -or
            $cmake -notmatch 'NOT IS_VS6_BUILD') {
            throw 'Lockstep-v2 host self-test is not x64/non-VC6 registered.'
        }
        Write-Output 'LOCKSTEP_V2_HOST_SELF_TEST_PASS'
    }
    finally {
        try {
            if ($null -ne (Get-LockstepItemIfPresent $root)) {
                Remove-LockstepHostSelfTestTree $root $root
            }
        }
        finally {
            $script:LockstepHostSelfTestScratchRoot = $null
            Set-Stage5LockstepHostSelfTestScratchRoot $null
        }
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

Assert-HeadlessDirectExecutionOptIn ([bool]$AllowHeadlessDirectExecution)
if (-not (Test-LowerHex40 $SourceCommit)) {
    throw 'SourceCommit must be the exact lowercase 40-hex revision.'
}
if ([string]::IsNullOrWhiteSpace($MapName) -or -not (Test-SafeMapName $MapName) -or
    $GeneralsMapCrc -eq 0 -or $ZeroHourMapCrc -eq 0 -or
    $PeerCount -ne $LockstepNetworkPeerCount) {
    throw 'MapName/per-title map CRCs/PeerCount do not form the bounded two-human/four-local-AI installed lockstep-v2 contract.'
}
$mapCrcs = Assert-LockstepMapCrcs ([ordered]@{
    Generals = $GeneralsMapCrc
    ZeroHour = $ZeroHourMapCrc
}) 'Requested lockstep-v2 map CRCs'
if (-not (Test-SafeHDirectory $OutputDirectory) -or (Test-Path -LiteralPath $OutputDirectory)) {
    throw 'OutputDirectory must be a fresh task-owned directory on H:.'
}
$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
$artifactSet = Read-AndValidateArtifactSet $ArtifactSetManifestPath $SourceCommit
$executionCohortNonce = if ([string]::IsNullOrWhiteSpace($ExecutionCohortNonce)) {
    [Guid]::NewGuid().ToString()
}
else { $ExecutionCohortNonce }
Assert-LockstepCanonicalUuid $executionCohortNonce 'ExecutionCohortNonce' | Out-Null
$executionCohortCreatedUtc = if ([string]::IsNullOrWhiteSpace($ExecutionCohortCreatedUtc)) {
    [DateTimeOffset]::UtcNow.ToString('o')
}
else { $ExecutionCohortCreatedUtc }
[DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
if (-not [DateTimeOffset]::TryParse($executionCohortCreatedUtc,
        [ref]$cohortCreated)) {
    throw 'ExecutionCohortCreatedUtc must be a valid timestamp.'
}
$runtimeClosure = Assert-LockstepRuntimeClosure $artifactSet.runtimeClosure `
    'Artifact-set runtime closure'
if (-not [string]::IsNullOrWhiteSpace($RuntimeClosureDependencyManifestSha256) -and
    $runtimeClosure.dependencyManifestSha256 -cne
        $RuntimeClosureDependencyManifestSha256.ToUpperInvariant()) {
    throw 'Requested runtime dependency-manifest closure hash does not match the artifact set.'
}
if (-not [string]::IsNullOrWhiteSpace($RuntimeClosureSha256) -and
    $runtimeClosure.closureSha256 -cne $RuntimeClosureSha256.ToUpperInvariant()) {
    throw 'Requested runtime closure hash does not match the artifact set.'
}
$qualificationData = Read-AndValidateQualificationData `
    -Manifest $QualificationDataManifestPath `
    -ExpectedManifestSha256 $QualificationDataManifestSha256 `
    -ExpectedClosureSha256 $QualificationDataClosureSha256 `
    -ExpectedSourceCommit $SourceCommit `
    -ExpectedMapName $MapName `
    -ExpectedMapCrcs $mapCrcs `
    -RuntimeFiles $artifactSet.runtimeFiles
$generalsFull = [IO.Path]::GetFullPath($GeneralsExecutable)
$zeroHourFull = [IO.Path]::GetFullPath($ZeroHourExecutable)
if ($generalsFull -cne $artifactSet.artifacts['generals-executable'].path -or
    $zeroHourFull -cne $artifactSet.artifacts['zerohour-executable'].path) {
    throw 'Requested executables must be the exact installed x64 artifact-set executables.'
}
Assert-X64PeExecutable $generalsFull
Assert-X64PeExecutable $zeroHourFull
Assert-Stage5ProcessLocalProfileCapability $generalsFull `
    -Context 'Generals installed executable' | Out-Null
Assert-Stage5ProcessLocalProfileCapability $zeroHourFull `
    -Context 'Zero Hour installed executable' | Out-Null
if (-not (Test-LockstepNoActiveTitleProcesses @($generalsFull, $zeroHourFull))) {
    throw 'An installed Generals or Zero Hour title process is already running; refusing registry/profile setup.'
}
$executables = [ordered]@{
    Generals = $artifactSet.artifacts['generals-executable'].sha256
    ZeroHour = $artifactSet.artifacts['zerohour-executable'].sha256
}
$launcherContracts = [ordered]@{
    Generals = Get-LauncherRunContract `
        $artifactSet.artifacts['generals-launcher-config'].path `
        $artifactSet.artifacts['generals-launcher'].path `
        (Split-Path -Parent $generalsFull) $generalsFull `
        $artifactSet.artifacts['generals-launcher-config'].sha256 `
        $artifactSet.artifacts['generals-launcher'].sha256
    ZeroHour = Get-LauncherRunContract `
        $artifactSet.artifacts['zerohour-launcher-config'].path `
        $artifactSet.artifacts['zerohour-launcher'].path `
        (Split-Path -Parent $zeroHourFull) $zeroHourFull `
        $artifactSet.artifacts['zerohour-launcher-config'].sha256 `
        $artifactSet.artifacts['zerohour-launcher'].sha256
}
if ($BasePort + (2 * $PeerCount) + 8 - 1 -gt 65535) {
    throw 'BasePort and PeerCount would exceed the 16-bit UDP port range.'
}
$sessionRun = Invoke-Stage5InstalledLockstepV2SessionSet `
    -GeneralsExecutable $generalsFull `
    -ZeroHourExecutable $zeroHourFull `
    -ArtifactSetManifestPath $ArtifactSetManifestPath `
    -SourceCommit $SourceCommit `
    -OutputDirectory $OutputDirectory `
    -MapName $MapName `
    -MapCrcs $mapCrcs `
    -PeerCount $PeerCount `
    -BasePort $BasePort `
    -Seed $Seed `
    -PeerTimeoutSeconds $PeerTimeoutSeconds `
    -ExecutionCohortNonce $executionCohortNonce `
    -ExecutionCohortCreatedUtc $executionCohortCreatedUtc `
    -ExpectedRuntimeClosure $runtimeClosure `
    -ValidationDataBinding $qualificationData `
    -AllowHeadlessDirectExecution ([bool]$AllowHeadlessDirectExecution) `
    -RunnerScriptPath $PSCommandPath
$sessionResults = @($sessionRun.sessionResults)
$negativeProbeResults = @($sessionRun.negativeProbeResults)
$recordedUtc = [string]$sessionRun.recordedUtc
$evidence = [ordered]@{
    schemaVersion = 2
    evidenceKind = 'lockstep-v2-multiplayer'
    status = 'passed'
    producer = $LockstepProducer
    validationMode = $LockstepMode
    architecture = 'x64'
    sourceCommit = $SourceCommit
    artifactSetSha256 = $artifactSet.sha256
    recordedUtc = $recordedUtc
    cohortNonce = $executionCohortNonce
    runtimeClosure = $runtimeClosure
    qualificationData = [ordered]@{
        manifestSha256 = $qualificationData.manifestSha256
        closureSha256 = $qualificationData.closureSha256
        fileCount = $qualificationData.fileCount
    }
    allowHeadlessDirectExecution = [bool]$AllowHeadlessDirectExecution
    launcherEquivalence = $launcherContracts
    commonStopFrame = $CommonStopFrame
    peerCount = $PeerCount
    networkRosterMask = $LockstepNetworkRosterMask
    simulationRosterMask = $LockstepSimulationRosterMask
    aiRosterMask = $LockstepAIRosterMask
    aiPlayerCount = $LockstepAIPlayerCount
    mapName = $MapName
    mapCrcs = $mapCrcs
    seed = $Seed
    negativeProbes = [ordered]@{
        crossEpoch = @($negativeProbeResults | Where-Object {
            $_.mode -ceq 'negative-cross-epoch'
        })
        contentMismatch = @($negativeProbeResults | Where-Object {
            $_.mode -ceq 'negative-content-mismatch'
        })
    }
    v1Accepted = $false
    profileStrategy = 'process-local-validation-profile-root'
    registryViews = @('Registry32', 'Registry64')
    environmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA',
        'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
        'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
        'RTS_STAGE5_VALIDATION_CACHE_ROOT',
        'RTS_STAGE5_VALIDATION_LOG_ROOT',
        'RTS_STAGE5_VALIDATION_DUMP_ROOT')
    profileConcurrency = 'shared-title-profile-read-only'
    titleSessionDisposition = $LockstepTitleSessionDisposition
    sessions = $sessionResults
}
$finalQualificationData = Read-AndValidateQualificationData `
    -Manifest $QualificationDataManifestPath `
    -ExpectedManifestSha256 $QualificationDataManifestSha256 `
    -ExpectedClosureSha256 $QualificationDataClosureSha256 `
    -ExpectedSourceCommit $SourceCommit `
    -ExpectedMapName $MapName `
    -ExpectedMapCrcs $mapCrcs `
    -RuntimeFiles $artifactSet.runtimeFiles
if ($finalQualificationData.manifestSha256 -cne
        $qualificationData.manifestSha256 -or
    $finalQualificationData.closureSha256 -cne
        $qualificationData.closureSha256 -or
    $finalQualificationData.fileCount -ne $qualificationData.fileCount) {
    throw 'Qualification data binding changed while the installed matrix was running.'
}
$evidencePath = Join-Path $outputFull 'LockstepV2LoopbackEvidence.json'
Write-AtomicText $evidencePath ($evidence | ConvertTo-Json -Depth 12)
$finalAcceptanceEnvelope = New-LockstepV2FinalAcceptanceEnvelope `
    -NativeEvidencePath $evidencePath `
    -SourceCommit $SourceCommit `
    -ArtifactSetSha256 $artifactSet.sha256 `
    -RecordedUtc $recordedUtc `
    -MapName $MapName `
    -MapCrcs $mapCrcs `
    -Seed $Seed `
    -PeerCount $PeerCount `
    -Sessions $sessionResults `
    -CohortNonce $executionCohortNonce `
    -CohortCreatedUtc $executionCohortCreatedUtc `
    -RuntimeClosure $runtimeClosure
$finalAcceptancePath = Join-Path $outputFull 'mixed-worker-multiplayer.json'
Write-AtomicText $finalAcceptancePath `
    ($finalAcceptanceEnvelope.document | ConvertTo-Json -Depth 12)
$qualificationDataEvidencePath = Join-Path $outputFull `
    $LockstepQualificationDataEvidenceLeaf
if ($null -ne (Get-LockstepItemIfPresent $qualificationDataEvidencePath)) {
    throw "Qualification data evidence output already exists: $qualificationDataEvidencePath"
}
[IO.File]::Copy($qualificationData.path, $qualificationDataEvidencePath)
Assert-LockstepNoReparse $qualificationDataEvidencePath `
    'qualification data evidence copy' $outputFull
if ((Get-UpperSha256 $qualificationDataEvidencePath) -cne
    $qualificationData.manifestSha256) {
    throw 'Qualification data evidence copy is stale or substituted.'
}
$evidenceClosure = New-LockstepV2EvidenceClosure `
    -EvidenceRoot $outputFull `
    -SourceCommit $SourceCommit `
    -ArtifactSetSha256 $artifactSet.sha256 `
    -CohortNonce $executionCohortNonce
Write-Output ("LOCKSTEP_V2_HOST_PASS sessions={0} peers={1} frame={2}" -f `
    $sessionResults.Count, $PeerCount, $CommonStopFrame)
Write-Output ("LOCKSTEP_V2_FINAL_ACCEPTANCE evidence={0} nativeEvidence={1}" -f `
    $finalAcceptancePath, $evidencePath)
Write-Output ("LOCKSTEP_V2_EVIDENCE_CLOSURE path={0} sha256={1} closure={2}" -f `
    $evidenceClosure.path, $evidenceClosure.sha256, $evidenceClosure.closureSha256)
