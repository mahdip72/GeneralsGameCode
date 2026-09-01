Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Stage5Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function ConvertFrom-Stage5JsonDictionary {
    param([string]$Path)
    $json = Get-Content -LiteralPath $Path -Raw
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $convertFromJson = Get-Command ConvertFrom-Json
        if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
            return $json | ConvertFrom-Json -AsHashtable -DateKind String
        }
        return $json | ConvertFrom-Json -AsHashtable
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = 10485760
    return $serializer.DeserializeObject($json)
}

function Get-Stage5JsonValue {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-Stage5Condition ($Object -is [Collections.IDictionary]) "$Context must be a JSON object."
    $keys = @($Object.Keys | Where-Object { [string]$_ -ceq $Name })
    Assert-Stage5Condition ($keys.Count -eq 1) "$Context is missing property '$Name'."
    $value = $Object[$keys[0]]
    if ($value -is [Array]) { return ,$value }
    return $value
}

function Assert-Stage5JsonShape {
    param([object]$Object, [string[]]$Names, [string]$Context)
    Assert-Stage5Condition ($Object -is [Collections.IDictionary]) "$Context must be a JSON object."
    foreach ($name in $Names) { Get-Stage5JsonValue $Object $name $Context | Out-Null }
    foreach ($key in $Object.Keys) {
        Assert-Stage5Condition ($Names -ccontains [string]$key) `
            "$Context contains unsupported property '$key'."
    }
}

function Test-Stage5JsonInteger {
    param([object]$Value)
    return $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or
        $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
}

function Test-Stage5JsonNumber {
    param([object]$Value)
    if ((Test-Stage5JsonInteger $Value) -or $Value -is [decimal]) { return $true }
    if ($Value -is [single]) {
        return -not [single]::IsNaN($Value) -and -not [single]::IsInfinity($Value)
    }
    if ($Value -is [double]) {
        return -not [double]::IsNaN($Value) -and -not [double]::IsInfinity($Value)
    }
    return $false
}

function ConvertFrom-Stage5MetricLine {
    param([string]$Line, [string]$Prefix, [string]$Context)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($Line)) "$Context is missing."
    Assert-Stage5Condition ($Line.StartsWith($Prefix + ' ', [StringComparison]::Ordinal)) `
        "$Context does not start with '$Prefix'."
    $fields = @{}
    $matches = [regex]::Matches($Line.Substring($Prefix.Length + 1),
        '(?<name>[A-Za-z_][A-Za-z0-9_]*)=(?:"(?<quoted>[^"]*)"|(?<plain>[^\s]+))')
    foreach ($match in $matches) {
        $name = $match.Groups['name'].Value
        Assert-Stage5Condition (-not $fields.ContainsKey($name)) "$Context repeats field '$name'."
        $fields[$name] = if ($match.Groups['quoted'].Success) {
            $match.Groups['quoted'].Value
        }
        else {
            $match.Groups['plain'].Value
        }
    }
    return $fields
}

function Get-Stage5RequiredField {
    param([hashtable]$Fields, [string]$Name, [string]$Context)
    Assert-Stage5Condition ($Fields.ContainsKey($Name)) "$Context is missing field '$Name'."
    return [string]$Fields[$Name]
}

function Get-Stage5UInt64Field {
    param([hashtable]$Fields, [string]$Name, [string]$Context)
    $text = Get-Stage5RequiredField $Fields $Name $Context
    [UInt64]$value = 0
    Assert-Stage5Condition ([UInt64]::TryParse($text, [ref]$value)) `
        "$Context field '$Name' is not an unsigned integer."
    return $value
}

function Get-Stage5SingleLine {
    param([string]$Output, [string]$Prefix, [string]$Context)
    $lines = @($Output -split "`r?`n" | Where-Object { $_.StartsWith($Prefix + ' ', [StringComparison]::Ordinal) })
    Assert-Stage5Condition ($lines.Count -eq 1) "$Context requires exactly one $Prefix line."
    return $lines[0]
}

function Get-Stage5UInt64BitCount {
    param([UInt64]$Value)
    [UInt64]$remaining = $Value
    [UInt64]$count = 0
    while ($remaining -ne 0) {
        $remaining = [UInt64]($remaining -band ($remaining - [UInt64]1))
        ++$count
    }
    return $count
}

function Get-Stage5ImmutableSpatialFieldNames {
    param([string]$Prefix = '')
    $names = @()
    $names += $Prefix + 'captured_arenas'
    $names += $Prefix + 'capture_failures'
    foreach ($collectionSuffix in @('successful_collections',
        'successful_collection_queries', 'successful_collection_ranges',
        'multi_range_collections', 'collection_submitted_jobs',
        'collection_completed_jobs', 'collection_physical_worker_jobs',
        'collection_owner_helped_jobs', 'collection_physical_worker_mask',
        'maximum_collection_queries', 'maximum_collection_ranges',
        'maximum_collection_distinct_physical_workers')) {
        $names += $Prefix + $collectionSuffix
    }
    foreach ($consumer in @('healing', 'pdl')) {
        foreach ($suffix in @('eligible_queries', 'authoritative_queries',
            'authoritative_candidates', 'shadow_queries', 'shadow_matches',
            'shadow_mismatches', 'submitted_jobs', 'completed_jobs',
            'physical_worker_jobs', 'owner_helped_jobs', 'expected_fallbacks',
            'unexpected_fallbacks', 'stale_rejections', 'validation_failures',
            'circuit_breaker_trips')) {
            $names += $Prefix + $consumer + '_' + $suffix
        }
    }
    return $names
}

function ConvertFrom-Stage5ImmutableSpatialFields {
    param([hashtable]$Fields, [string]$Prefix, [object]$Entry,
        [string]$Context)
    $fieldNames = @(Get-Stage5ImmutableSpatialFieldNames $Prefix)
    $isStress = $false
    if ($null -ne $Entry.PSObject.Properties['stress']) {
        $isStress = [bool]$Entry.stress
    }
    $qualifyingCollectionStress = $isStress -and
        ($Entry.simulationMode -ceq 'parallel' -or
            $Entry.simulationMode -ceq 'shadow') -and
        $Entry.configuration -match '^(?:parallel-(?:2|4|8|16|auto)|shadow-16)$'
    foreach ($numeric in $fieldNames) {
        Get-Stage5UInt64Field $Fields $numeric "$Context immutable-spatial evidence" | Out-Null
    }
    $capturedArenas = Get-Stage5UInt64Field $Fields ($Prefix + 'captured_arenas') $Context
    if ($qualifyingCollectionStress) {
        Assert-Stage5Condition ($capturedArenas -gt 0) `
            "$Context has no captured immutable-spatial arena."
    }
    elseif ($Entry.simulationMode -ceq 'serial') {
        Assert-Stage5Condition ($capturedArenas -eq 0) `
            "$Context serial simulation reports captured immutable-spatial arenas."
    }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $Fields `
        ($Prefix + 'capture_failures') $Context) -eq 0) `
        "$Context reports immutable-spatial arena capture failures."

    $successfulCollections = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'successful_collections') $Context
    $successfulCollectionQueries = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'successful_collection_queries') $Context
    $successfulCollectionRanges = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'successful_collection_ranges') $Context
    $multiRangeCollections = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'multi_range_collections') $Context
    $collectionSubmitted = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_submitted_jobs') $Context
    $collectionCompleted = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_completed_jobs') $Context
    $collectionPhysical = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_physical_worker_jobs') $Context
    $collectionOwnerHelped = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_owner_helped_jobs') $Context
    $collectionPhysicalWorkerMask = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_physical_worker_mask') $Context
    $maximumCollectionQueries = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'maximum_collection_queries') $Context
    $maximumCollectionRanges = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'maximum_collection_ranges') $Context
    $maximumCollectionDistinctPhysicalWorkers = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'maximum_collection_distinct_physical_workers') $Context
    $aggregateCollectionDistinctPhysicalWorkers =
        Get-Stage5UInt64BitCount $collectionPhysicalWorkerMask
    Assert-Stage5Condition ($successfulCollections -eq $multiRangeCollections) `
        "$Context immutable-spatial collection/multi-range counts are inconsistent."
    Assert-Stage5Condition ($collectionSubmitted -eq $collectionCompleted -and
        $collectionCompleted -eq $collectionPhysical) `
        "$Context immutable-spatial collection jobs are not balanced physical-worker work."
    Assert-Stage5Condition ($collectionOwnerHelped -eq 0) `
        "$Context immutable-spatial collection reports owner help."
    if ($successfulCollections -gt 0) {
        Assert-Stage5Condition ($capturedArenas -gt 0) `
            "$Context has no captured immutable-spatial arena for successful collection work."
        Assert-Stage5Condition ($successfulCollectionQueries -ge
            (2 * $successfulCollections) -and $successfulCollectionRanges -ge
            (2 * $successfulCollections) -and $collectionSubmitted -eq
            (2 * $successfulCollectionRanges) -and
            $maximumCollectionQueries -ge 2 -and
            $maximumCollectionRanges -ge 2 -and
            $collectionPhysicalWorkerMask -gt 0 -and
            $maximumCollectionDistinctPhysicalWorkers -gt 0 -and
            $maximumCollectionDistinctPhysicalWorkers -le
                $maximumCollectionRanges -and
            $maximumCollectionDistinctPhysicalWorkers -le
                $aggregateCollectionDistinctPhysicalWorkers) `
            "$Context immutable-spatial collection evidence does not prove multi-query, multi-range two-pass worker execution."
    }
    else {
        Assert-Stage5Condition ($successfulCollectionQueries -eq 0 -and
            $successfulCollectionRanges -eq 0 -and $collectionSubmitted -eq 0 -and
            $collectionCompleted -eq 0 -and $collectionPhysical -eq 0 -and
            $collectionPhysicalWorkerMask -eq 0 -and
            $maximumCollectionQueries -eq 0 -and $maximumCollectionRanges -eq 0 -and
            $maximumCollectionDistinctPhysicalWorkers -eq 0) `
            "$Context reports immutable-spatial collection work without a successful collection."
    }
    if ($qualifyingCollectionStress) {
        Assert-Stage5Condition ($successfulCollections -gt 0 -and
            $successfulCollectionQueries -gt $successfulCollections -and
            $successfulCollectionRanges -gt $successfulCollections -and
            $collectionSubmitted -gt 0 -and $collectionPhysical -gt 0 -and
            $maximumCollectionQueries -ge 2 -and $maximumCollectionRanges -ge 2) `
            "$Context qualifying stress has no positive multi-query, multi-range immutable-spatial collection evidence."
        if ($Entry.configuration -match '^(?:parallel|shadow)-(2|4|8|16)$') {
            $explicitCollectionWorkers = [UInt64]$Matches[1]
            $expectedMaximumCollectionRanges = [Math]::Min(
                $explicitCollectionWorkers, $maximumCollectionQueries)
            Assert-Stage5Condition ($maximumCollectionRanges -eq
                $expectedMaximumCollectionRanges) `
                "$Context immutable-spatial maximum collection ranges do not match min(explicit workers, maximum queueable queries)."
            Assert-Stage5Condition (($collectionPhysicalWorkerMask -shr
                $explicitCollectionWorkers) -eq 0) `
                "$Context immutable-spatial physical-worker mask exceeds the explicit worker lane."
            if ($expectedMaximumCollectionRanges -ge 4) {
                Assert-Stage5Condition (
                    $maximumCollectionDistinctPhysicalWorkers -gt 1) `
                    "$Context sufficiently large immutable-spatial collection did not use more than one distinct physical worker."
            }
        }
    }
    if ($Entry.simulationMode -ceq 'serial' -or
        $Entry.configuration -ceq 'serial-1' -or
        $Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($successfulCollections -eq 0 -and
            $multiRangeCollections -eq 0 -and $collectionSubmitted -eq 0 -and
            $collectionCompleted -eq 0 -and $collectionPhysical -eq 0) `
            "$Context nonqualifying serial/one-worker lane reports immutable-spatial collection worker authority."
    }

    $consumerEvidence = @{}
    foreach ($consumer in @('healing', 'pdl')) {
        $consumerPrefix = $Prefix + $consumer + '_'
        $eligible = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'eligible_queries') $Context
        $authoritative = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'authoritative_queries') $Context
        $candidates = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'authoritative_candidates') $Context
        $shadow = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'shadow_queries') $Context
        $shadowMatches = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'shadow_matches') $Context
        $shadowMismatches = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'shadow_mismatches') $Context
        $submitted = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'submitted_jobs') $Context
        $completed = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'completed_jobs') $Context
        $physical = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'physical_worker_jobs') $Context
        $ownerHelped = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'owner_helped_jobs') $Context
        $expectedFallbacks = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'expected_fallbacks') $Context
		$unexpectedFallbacks = Get-Stage5UInt64Field $Fields `
			($consumerPrefix + 'unexpected_fallbacks') $Context
		$staleRejections = Get-Stage5UInt64Field $Fields `
			($consumerPrefix + 'stale_rejections') $Context
        Assert-Stage5Condition ($submitted -eq $completed -and
            $completed -eq $physical) `
            "$Context $consumer immutable-spatial jobs are not balanced physical-worker work."
        Assert-Stage5Condition ($ownerHelped -eq 0) `
            "$Context $consumer immutable-spatial work reports owner help."
        Assert-Stage5Condition ($shadow -eq ($shadowMatches + $shadowMismatches)) `
            "$Context $consumer immutable-spatial shadow counters are inconsistent."
        foreach ($zeroInvariant in @('shadow_mismatches', 'unexpected_fallbacks',
            'stale_rejections', 'validation_failures', 'circuit_breaker_trips')) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $Fields `
                ($consumerPrefix + $zeroInvariant) $Context) -eq 0) `
                "$Context reports forbidden $consumer immutable-spatial evidence in '$zeroInvariant'."
        }
        if ($authoritative -gt 0) {
            Assert-Stage5Condition ($eligible -ge $authoritative -and
                $submitted -gt 0 -and $physical -gt 0) `
                "$Context reports $consumer immutable-spatial authority without eligible physical-worker queries."
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($authoritative -eq 0 -and $candidates -eq 0) `
                "$Context reports $consumer immutable-spatial authority outside parallel simulation."
        }
        if ($Entry.simulationMode -cne 'shadow') {
            Assert-Stage5Condition ($shadow -eq 0 -and $shadowMatches -eq 0 -and
                $shadowMismatches -eq 0) `
                "$Context reports $consumer immutable-spatial shadow work outside shadow simulation."
        }
        if ($Entry.configuration -ceq 'serial-1' -or
            $Entry.configuration -ceq 'parallel-1') {
            Assert-Stage5Condition ($authoritative -eq 0 -and $candidates -eq 0 -and
                $shadow -eq 0 -and $submitted -eq 0 -and $completed -eq 0 -and
                $physical -eq 0) `
                "$Context nonqualifying serial/one-worker lane reports $consumer immutable-spatial worker authority."
        }
        $qualifyingParallelStress = $isStress -and
            $Entry.simulationMode -ceq 'parallel' -and
            $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
        if ($qualifyingParallelStress) {
            Assert-Stage5Condition ($authoritative -gt 0 -and $candidates -gt 0 -and
                $submitted -gt 0 -and $physical -gt 0 -and $expectedFallbacks -eq 0) `
                "$Context qualifying stress has no positive $consumer immutable-spatial authority, candidates, and balanced worker evidence."
        }
        if ($Entry.simulationMode -ceq 'shadow') {
            Assert-Stage5Condition ($shadow -gt 0 -and $shadowMatches -eq $shadow -and
                $submitted -gt 0 -and $physical -gt 0 -and $authoritative -eq 0 -and
                $candidates -eq 0 -and $expectedFallbacks -eq 0) `
                "$Context shadow stress has no positive matching $consumer immutable-spatial worker comparison."
        }
        $consumerEvidence[$consumer] = [pscustomobject]@{
            eligibleQueries = $eligible
            authoritativeQueries = $authoritative
            authoritativeCandidates = $candidates
            shadowQueries = $shadow
            shadowMatches = $shadowMatches
            submittedJobs = $submitted
            completedJobs = $completed
            physicalWorkerJobs = $physical
            expectedFallbacks = $expectedFallbacks
			unexpectedFallbacks = $unexpectedFallbacks
			staleRejections = $staleRejections
        }
    }
    return [pscustomobject]@{
        capturedArenas = $capturedArenas
        successfulCollections = $successfulCollections
        successfulCollectionQueries = $successfulCollectionQueries
        successfulCollectionRanges = $successfulCollectionRanges
        collectionSubmittedJobs = $collectionSubmitted
        collectionCompletedJobs = $collectionCompleted
        collectionPhysicalWorkerJobs = $collectionPhysical
        collectionPhysicalWorkerMask = $collectionPhysicalWorkerMask
        maximumCollectionDistinctPhysicalWorkers =
            $maximumCollectionDistinctPhysicalWorkers
        healing = $consumerEvidence['healing']
        pdl = $consumerEvidence['pdl']
        fields = $Fields
    }
}

function ConvertFrom-Stage5AiCompletion {
    param([string]$Output, [object]$Entry, [string]$ExecutableHash,
        [bool]$RequireAuthoritativeWorkEvidence = $true)
    $context = "AI validation entry $($Entry.sequence)"
    $line = Get-Stage5SingleLine $Output 'SKIRMISH_AI_TEST_COMPLETE' $context
    $fields = ConvertFrom-Stage5MetricLine $line 'SKIRMISH_AI_TEST_COMPLETE' "$context completion manifest"
    foreach ($required in @('seed', 'loaded_seed', 'scenario', 'actual_ai', 'actual_teams', 'winner_team', 'end_frame', 'executable_sha256',
        'simulation_mode', 'requested_pipeline', 'effective_pipeline', 'requested_simulation',
        'effective_simulation', 'requested_workers', 'effective_workers', 'worker_policy',
        'final_digest', 'wall_ms', 'job_submitted', 'job_executed', 'job_steals',
        'job_owner_help', 'job_waits', 'job_worker_wait_reject', 'job_failed',
        'job_cancelled', 'job_fallback', 'job_queue_latency_ns',
        'job_max_queue_latency_ns', 'job_sleeps', 'job_wakes', 'job_affinity_failures',
        'job_queue_high_water', 'job_peak_active_workers', 'available_cpus',
        'reserved_owner_cpus', 'selected_worker_cpus')) {
        Get-Stage5RequiredField $fields $required "$context completion manifest" | Out-Null
    }
    $spatialWorkFieldNames = @(Get-Stage5ImmutableSpatialFieldNames 'spatial_')
    $workFieldNames = @('authoritative_commits', 'shadow_executions', 'owner_fallbacks',
        'ai_captured_snapshots', 'ai_captured_candidates', 'ai_requested_batches',
        'ai_submitted_jobs', 'ai_completed_jobs', 'ai_serial_fallbacks',
        'ai_shadow_matches', 'ai_shadow_mismatches', 'ai_validation_failures',
        'ai_committed_batches', 'ai_parallel_authoritative_commits',
        'ai_rejected_commits',
		'direct_eligible', 'direct_submitted', 'direct_executed',
		'direct_worker_executed', 'direct_owner_helped',
		'direct_authoritative_commits',
		'direct_authoritative_multiworker_commits', 'direct_stale_rejections',
		'direct_validation_failures', 'direct_serial_fallbacks',
		'direct_unsupported_authority', 'direct_shadow_authority',
		'direct_stale_acceptance', 'direct_malformed_acceptance',
		'direct_shadow_only', 'direct_timeouts', 'direct_late_drains',
		'direct_peak_active_workers',
		'direct_callback_min', 'direct_callback_max',
        'collision_authoritative_commits', 'collision_shadow_executions',
        'collision_shadow_compared_candidates',
        'collision_shadow_mismatches', 'collision_owner_fallbacks',
        'collision_unexpected_fallbacks', 'collision_ineligible_slices',
        'collision_stale_rejections', 'collision_committed_candidates',
        'collision_prepared_pairs', 'collision_unique_candidates',
        'collision_submitted_jobs', 'collision_completed_jobs',
        'physics_authoritative_batches', 'physics_committed_prefixes',
        'physics_ranges', 'physics_submitted_jobs', 'physics_completed_jobs',
        'physics_allocated_bytes', 'physics_capture_ns', 'physics_prepare_ns',
        'physics_wait_ns', 'physics_commit_ns', 'physics_storage_bytes',
        'physics_storage_capacity_bytes', 'physics_storage_allocations',
        'physics_shadow_executions', 'physics_shadow_prefixes',
        'physics_shadow_ranges', 'physics_shadow_submitted_jobs',
        'physics_shadow_completed_jobs', 'physics_shadow_matches',
        'physics_shadow_mismatches', 'physics_owner_fallbacks',
        'physics_ineligible_slices', 'physics_unexpected_fallbacks',
        'physics_stale_rejections', 'physics_circuit_breaker_trips') +
        $spatialWorkFieldNames
    $presentWorkFieldCount = @($workFieldNames | Where-Object { $fields.ContainsKey($_) }).Count
    Assert-Stage5Condition ($presentWorkFieldCount -eq 0 -or
        $presentWorkFieldCount -eq $workFieldNames.Count) `
        "$context completion manifest contains an incomplete authoritative-work schema."
    $hasAuthoritativeWorkEvidence = $presentWorkFieldCount -eq $workFieldNames.Count
    Assert-Stage5Condition (-not $RequireAuthoritativeWorkEvidence -or $hasAuthoritativeWorkEvidence) `
        "$context completion manifest is missing required authoritative Stage 5 work evidence."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'executable_sha256' $context) -ceq $ExecutableHash) `
        "$context executable hash does not match the validated candidate."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'simulation_mode' $context) -ceq $Entry.simulationMode) `
        "$context simulation mode does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_pipeline' $context) -ceq 'serial') `
        "$context did not honestly request the serial pipeline."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'effective_pipeline' $context) -ceq 'serial') `
        "$context did not run the serial pipeline."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_simulation' $context) -ceq
        $Entry.simulationMode) "$context live requested simulation policy does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'effective_simulation' $context) -ceq
        $Entry.simulationMode) "$context live effective simulation policy does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_workers' $context) -ceq [string]$Entry.requestedWorkers) `
        "$context requested worker count does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'worker_policy' $context) -ceq 'auto') `
        "$context worker policy does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'seed' $context) -ceq [string]$Entry.seed) `
        "$context seed does not match the plan."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'loaded_seed' $context) -eq [UInt64]$Entry.seed) `
        "$context loaded_seed does not match the planned live seed."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'scenario' $context) -ceq $Entry.scenario) `
        "$context scenario does not match the plan."
    $expectedAiCount = if ($Entry.scenario -ceq '4v2') { 6 } else { 7 }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'actual_ai' $context) -eq $expectedAiCount) `
        "$context actual_ai does not match the planned scenario."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'actual_teams' $context) -ceq $Entry.scenario) `
        "$context actual_teams does not match the planned scenario."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'final_digest' $context) -match '^[0-9A-Fa-f]{8}$') `
        "$context final digest is invalid."

    foreach ($numeric in @('loaded_seed', 'actual_ai', 'winner_team', 'end_frame', 'effective_workers', 'wall_ms',
        'job_submitted', 'job_executed', 'job_steals', 'job_owner_help', 'job_waits',
        'job_worker_wait_reject', 'job_failed', 'job_cancelled', 'job_fallback',
        'job_queue_latency_ns', 'job_max_queue_latency_ns', 'job_sleeps', 'job_wakes',
        'job_affinity_failures', 'job_queue_high_water', 'job_peak_active_workers',
        'available_cpus', 'reserved_owner_cpus', 'selected_worker_cpus')) {
        Get-Stage5UInt64Field $fields $numeric "$context completion manifest" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'job_failed' $context) -eq 0) `
        "$context reports failed jobs."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'job_cancelled' $context) -eq 0) `
        "$context reports cancelled jobs."

    [UInt64]$authoritativeCommits = 0
    [UInt64]$shadowExecutions = 0
    [UInt64]$ownerFallbacks = 0
    [UInt64]$aiSubmittedJobs = 0
    [UInt64]$aiCompletedJobs = 0
    [UInt64]$aiCommittedBatches = 0
    [UInt64]$aiParallelAuthoritativeCommits = 0
	[UInt64]$pathWorkerExecuted = 0
	[UInt64]$pathAuthoritativeCommits = 0
	[UInt64]$pathAuthoritativeMultiWorkerCommits = 0
	[UInt64]$pathOwnerHelped = 0
	[UInt64]$pathPeakWorkers = 0
    [UInt64]$collisionAuthoritativeCommits = 0
    [UInt64]$collisionShadowExecutions = 0
    [UInt64]$collisionShadowComparedCandidates = 0
    [UInt64]$collisionOwnerFallbacks = 0
    [UInt64]$collisionCommittedCandidates = 0
    [UInt64]$collisionPreparedPairs = 0
    [UInt64]$collisionUniqueCandidates = 0
    [UInt64]$collisionSubmittedJobs = 0
    [UInt64]$collisionCompletedJobs = 0
    [UInt64]$physicsAuthoritativeBatches = 0
    [UInt64]$physicsCommittedPrefixes = 0
    [UInt64]$physicsRanges = 0
    [UInt64]$physicsSubmittedJobs = 0
    [UInt64]$physicsCompletedJobs = 0
    [UInt64]$physicsShadowExecutions = 0
    [UInt64]$physicsShadowPrefixes = 0
    [UInt64]$physicsShadowRanges = 0
    [UInt64]$physicsShadowSubmittedJobs = 0
    [UInt64]$physicsShadowCompletedJobs = 0
    $spatialEvidence = $null
    if ($hasAuthoritativeWorkEvidence) {
        foreach ($numeric in $workFieldNames) {
            Get-Stage5UInt64Field $fields $numeric "$context authoritative-work evidence" | Out-Null
        }
        $authoritativeCommits = Get-Stage5UInt64Field $fields 'authoritative_commits' $context
        $shadowExecutions = Get-Stage5UInt64Field $fields 'shadow_executions' $context
        $ownerFallbacks = Get-Stage5UInt64Field $fields 'owner_fallbacks' $context
        $aiSubmittedJobs = Get-Stage5UInt64Field $fields 'ai_submitted_jobs' $context
        $aiCompletedJobs = Get-Stage5UInt64Field $fields 'ai_completed_jobs' $context
        $aiSerialFallbacks = Get-Stage5UInt64Field $fields 'ai_serial_fallbacks' $context
        $aiShadowMatches = Get-Stage5UInt64Field $fields 'ai_shadow_matches' $context
        $aiShadowMismatches = Get-Stage5UInt64Field $fields 'ai_shadow_mismatches' $context
        $aiCommittedBatches = Get-Stage5UInt64Field $fields 'ai_committed_batches' $context
        $aiParallelAuthoritativeCommits = Get-Stage5UInt64Field $fields `
            'ai_parallel_authoritative_commits' $context
        Assert-Stage5Condition ($authoritativeCommits -eq $aiParallelAuthoritativeCommits) `
            "$context authoritative_commits does not match the mode-specific AI parallel-authority counter."
        Assert-Stage5Condition ($aiParallelAuthoritativeCommits -le $aiCommittedBatches) `
            "$context reports more mode-specific AI parallel-authority commits than generic owner commits."
        Assert-Stage5Condition ($shadowExecutions -eq ($aiShadowMatches + $aiShadowMismatches)) `
            "$context shadow_executions does not match the AI shadow counters."
        Assert-Stage5Condition ($ownerFallbacks -eq $aiSerialFallbacks) `
            "$context owner_fallbacks does not match the AI serial-fallback counter."
        Assert-Stage5Condition ($aiCompletedJobs -le $aiSubmittedJobs) `
            "$context reports more completed AI jobs than submitted AI jobs."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'ai_validation_failures' $context) -eq 0) `
            "$context reports AI planning validation failures."
        Assert-Stage5Condition ($aiShadowMismatches -eq 0) `
            "$context reports AI planning shadow mismatches."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'ai_rejected_commits' $context) -eq 0) `
            "$context reports rejected AI owner commits."
        if ($authoritativeCommits -gt 0) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'ai_captured_snapshots' $context) -gt 0 -and
                (Get-Stage5UInt64Field $fields 'ai_requested_batches' $context) -gt 0) `
                "$context reports authoritative commits without captured/requested AI planning work."
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($authoritativeCommits -eq 0 -and
                $aiParallelAuthoritativeCommits -eq 0) `
                "$context reports AI owner authority outside parallel simulation."
        }
        if ($Entry.simulationMode -cne 'shadow') {
            Assert-Stage5Condition ($shadowExecutions -eq 0 -and
                $aiShadowMatches -eq 0 -and $aiShadowMismatches -eq 0) `
                "$context reports AI shadow work outside shadow simulation."
        }
        if ($Entry.simulationMode -ceq 'serial') {
            Assert-Stage5Condition ($aiSubmittedJobs -eq 0 -and $aiCompletedJobs -eq 0 -and
                $ownerFallbacks -eq 0) `
                "$context serial simulation reports AI lane jobs or owner fallbacks."
        }

		$pathEligible = Get-Stage5UInt64Field $fields 'direct_eligible' $context
		$pathSubmitted = Get-Stage5UInt64Field $fields 'direct_submitted' $context
		$pathExecuted = Get-Stage5UInt64Field $fields 'direct_executed' $context
		$pathWorkerExecuted = Get-Stage5UInt64Field $fields 'direct_worker_executed' $context
		$pathOwnerHelped = Get-Stage5UInt64Field $fields 'direct_owner_helped' $context
		$pathTimeouts = Get-Stage5UInt64Field $fields 'direct_timeouts' $context
		$pathAuthoritativeCommits = Get-Stage5UInt64Field $fields `
			'direct_authoritative_commits' $context
		$pathAuthoritativeMultiWorkerCommits = Get-Stage5UInt64Field $fields `
			'direct_authoritative_multiworker_commits' $context
		$pathPeakWorkers = Get-Stage5UInt64Field $fields 'direct_peak_active_workers' $context
		$pathEffectiveWorkers = Get-Stage5UInt64Field $fields 'effective_workers' $context
		$pathCallbackMin = Get-Stage5UInt64Field $fields 'direct_callback_min' $context
		$pathCallbackMax = Get-Stage5UInt64Field $fields 'direct_callback_max' $context
		Assert-Stage5Condition ($pathSubmitted -le $pathEligible) `
			"$context reports more submitted direct-path jobs than eligible requests."
		Assert-Stage5Condition ($pathExecuted -eq $pathSubmitted) `
			"$context direct-path submitted/executed job counts do not match."
		Assert-Stage5Condition (($pathWorkerExecuted + $pathOwnerHelped) -eq $pathExecuted) `
			"$context reports inconsistent direct-path execution identities."
		Assert-Stage5Condition ($pathOwnerHelped -eq 0) `
			"$context reports owner-helped direct-path jobs; the bounded batch lane is physical-worker-only."
		Assert-Stage5Condition ($pathTimeouts -eq 0) `
			"$context reports synchronous direct-path watchdog timeouts."
		Assert-Stage5Condition ($pathAuthoritativeCommits -le $pathWorkerExecuted) `
			"$context reports direct-path authority not backed by physical-worker execution."
		Assert-Stage5Condition ($pathAuthoritativeCommits -eq 0 -or
			($pathSubmitted -ge 2 -and $pathWorkerExecuted -ge 2)) `
			"$context reports direct-path authority from an impossible single-request batch."
		Assert-Stage5Condition ($pathAuthoritativeMultiWorkerCommits -le
			$pathAuthoritativeCommits) `
			"$context reports more multi-worker direct-path commits than authoritative commits."
		Assert-Stage5Condition ($pathAuthoritativeMultiWorkerCommits -eq 0 -or
			$pathPeakWorkers -gt 1) `
			"$context reports multi-worker direct-path authority without a multi-worker peak."
		$pathWorkerBound = [Math]::Min([UInt64]16, $pathEffectiveWorkers)
		Assert-Stage5Condition ($pathPeakWorkers -le $pathWorkerBound -and
			$pathPeakWorkers -le $pathWorkerExecuted -and
			(($pathWorkerExecuted -eq 0 -and $pathPeakWorkers -eq 0) -or
			 ($pathWorkerExecuted -gt 0 -and $pathPeakWorkers -gt 0))) `
			"$context reports an impossible direct-path active-worker count."
		if ($pathEligible -eq 0) {
			Assert-Stage5Condition ($pathCallbackMin -eq 0 -and $pathCallbackMax -eq 0) `
				"$context reports a direct-path callback range without eligible requests."
		}
		else {
			Assert-Stage5Condition ($pathCallbackMin -gt 0 -and
				$pathCallbackMax -ge $pathCallbackMin) `
				"$context reports an invalid direct-path callback range."
		}
		foreach ($zeroInvariant in @('direct_unsupported_authority',
			'direct_shadow_authority', 'direct_stale_acceptance',
			'direct_malformed_acceptance', 'direct_shadow_only',
			'direct_validation_failures')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $fields $zeroInvariant $context) -eq 0) `
				"$context reports forbidden direct-path acceptance evidence in '$zeroInvariant'."
		}
		$isQualifyingPathStress = $Entry.scenario -ceq '4v2' -and
			$Entry.simulationMode -ceq 'parallel' -and
			$Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
		if ($isQualifyingPathStress) {
			Assert-Stage5Condition ($pathEligible -ge 2 -and $pathSubmitted -ge 2 -and
				$pathWorkerExecuted -gt 1 -and $pathPeakWorkers -gt 1 -and
				$pathAuthoritativeCommits -gt 0 -and
				$pathAuthoritativeMultiWorkerCommits -gt 0) `
				"$context qualifying parallel stress has no multi-request direct-path batch backed by more than one physical path worker and an authoritative commit."
		}
		if ($Entry.configuration -ceq 'serial-1' -or
			$Entry.configuration -ceq 'parallel-1' -or
			$Entry.simulationMode -cne 'parallel') {
			Assert-Stage5Condition ($pathEligible -eq 0 -and $pathSubmitted -eq 0 -and
				$pathExecuted -eq 0 -and $pathWorkerExecuted -eq 0 -and
				$pathOwnerHelped -eq 0 -and $pathAuthoritativeCommits -eq 0 -and
				$pathAuthoritativeMultiWorkerCommits -eq 0 -and
				$pathPeakWorkers -eq 0) `
				"$context nonqualifying serial, one-worker, or non-parallel lane reports direct-path batch work or authority."
		}

        $collisionAuthoritativeCommits = Get-Stage5UInt64Field $fields `
            'collision_authoritative_commits' $context
        $collisionShadowExecutions = Get-Stage5UInt64Field $fields `
            'collision_shadow_executions' $context
        $collisionShadowComparedCandidates = Get-Stage5UInt64Field $fields `
            'collision_shadow_compared_candidates' $context
        $collisionOwnerFallbacks = Get-Stage5UInt64Field $fields `
            'collision_owner_fallbacks' $context
        $collisionCommittedCandidates = Get-Stage5UInt64Field $fields `
            'collision_committed_candidates' $context
        $collisionPreparedPairs = Get-Stage5UInt64Field $fields `
            'collision_prepared_pairs' $context
        $collisionUniqueCandidates = Get-Stage5UInt64Field $fields `
            'collision_unique_candidates' $context
        $collisionSubmittedJobs = Get-Stage5UInt64Field $fields `
            'collision_submitted_jobs' $context
        $collisionCompletedJobs = Get-Stage5UInt64Field $fields `
            'collision_completed_jobs' $context
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
            'collision_shadow_mismatches' $context) -eq 0) `
            "$context reports collision shadow mismatches."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
            'collision_unexpected_fallbacks' $context) -eq 0) `
            "$context reports unexpected collision owner fallbacks."
        Assert-Stage5Condition ($collisionCompletedJobs -eq $collisionSubmittedJobs) `
            "$context collision submitted/completed job counts do not match."
        Assert-Stage5Condition ($collisionUniqueCandidates -le $collisionPreparedPairs) `
            "$context reports more unique collision candidates than prepared pairs."
        Assert-Stage5Condition ($collisionCommittedCandidates -le $collisionUniqueCandidates) `
            "$context reports more committed collision contacts than unique candidates."
        Assert-Stage5Condition ($collisionShadowComparedCandidates -le $collisionUniqueCandidates) `
            "$context reports more shadow-compared collision insertions than unique candidates."
        if ($collisionAuthoritativeCommits -gt 0) {
            Assert-Stage5Condition ($collisionPreparedPairs -gt 0 -and
                $collisionSubmittedJobs -gt 0 -and $collisionCompletedJobs -gt 0) `
                "$context reports authoritative collision commits without collision-specific parallel work."
        }
        if ($Entry.simulationMode -ceq 'shadow') {
            Assert-Stage5Condition ($collisionShadowExecutions -gt 0 -and
                $collisionShadowComparedCandidates -gt 0 -and
                $collisionPreparedPairs -gt 0 -and $collisionUniqueCandidates -gt 0 -and
                $collisionSubmittedJobs -gt 0 -and $collisionCompletedJobs -gt 0) `
                "$context shadow stress did not compare positive successful legacy collision insertions with collision-specific work and jobs."
            Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
                $collisionCommittedCandidates -eq 0) `
                "$context shadow collision evidence incorrectly reports authoritative publication."
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
                $collisionCommittedCandidates -eq 0) `
                "$context reports collision authority outside parallel simulation."
        }
        if ($Entry.simulationMode -cne 'shadow') {
            Assert-Stage5Condition ($collisionShadowExecutions -eq 0 -and
                $collisionShadowComparedCandidates -eq 0) `
                "$context reports collision shadow work outside shadow simulation."
        }
        if ($Entry.simulationMode -ceq 'serial') {
            foreach ($serialCollisionField in @('collision_authoritative_commits',
                'collision_shadow_executions', 'collision_shadow_compared_candidates',
                'collision_shadow_mismatches', 'collision_owner_fallbacks',
                'collision_unexpected_fallbacks', 'collision_ineligible_slices',
                'collision_stale_rejections', 'collision_committed_candidates',
                'collision_prepared_pairs', 'collision_unique_candidates',
                'collision_submitted_jobs', 'collision_completed_jobs')) {
                Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
                    $serialCollisionField $context) -eq 0) `
                    "$context serial simulation reports collision lane work in '$serialCollisionField'."
            }
        }
        elseif ($Entry.configuration -ceq 'parallel-1') {
            Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
                $collisionShadowExecutions -eq 0 -and
                $collisionShadowComparedCandidates -eq 0 -and
                $collisionCommittedCandidates -eq 0 -and
                $collisionPreparedPairs -eq 0 -and
                $collisionUniqueCandidates -eq 0 -and
                $collisionSubmittedJobs -eq 0 -and $collisionCompletedJobs -eq 0 -and
                (Get-Stage5UInt64Field $fields 'collision_stale_rejections' $context) -eq 0) `
                "$context one-worker ineligible simulation reports collision prepared/publication work."
        }

        $physicsAuthoritativeBatches = Get-Stage5UInt64Field $fields `
            'physics_authoritative_batches' $context
        $physicsCommittedPrefixes = Get-Stage5UInt64Field $fields `
            'physics_committed_prefixes' $context
        $physicsRanges = Get-Stage5UInt64Field $fields 'physics_ranges' $context
        $physicsSubmittedJobs = Get-Stage5UInt64Field $fields `
            'physics_submitted_jobs' $context
        $physicsCompletedJobs = Get-Stage5UInt64Field $fields `
            'physics_completed_jobs' $context
        $physicsShadowExecutions = Get-Stage5UInt64Field $fields `
            'physics_shadow_executions' $context
        $physicsShadowPrefixes = Get-Stage5UInt64Field $fields `
            'physics_shadow_prefixes' $context
        $physicsShadowRanges = Get-Stage5UInt64Field $fields `
            'physics_shadow_ranges' $context
        $physicsShadowSubmittedJobs = Get-Stage5UInt64Field $fields `
            'physics_shadow_submitted_jobs' $context
        $physicsShadowCompletedJobs = Get-Stage5UInt64Field $fields `
            'physics_shadow_completed_jobs' $context
        $physicsShadowMatches = Get-Stage5UInt64Field $fields `
            'physics_shadow_matches' $context
        $physicsShadowMismatches = Get-Stage5UInt64Field $fields `
            'physics_shadow_mismatches' $context
        Assert-Stage5Condition ($physicsSubmittedJobs -eq $physicsCompletedJobs -and
            $physicsRanges -le $physicsSubmittedJobs) `
            "$context reports inconsistent physics ranges or submitted/completed jobs."
        Assert-Stage5Condition ($physicsShadowExecutions -eq
            ($physicsShadowMatches + $physicsShadowMismatches)) `
            "$context reports inconsistent physics shadow counters."
        foreach ($zeroPhysicsInvariant in @('physics_shadow_mismatches',
            'physics_owner_fallbacks', 'physics_unexpected_fallbacks',
            'physics_stale_rejections', 'physics_circuit_breaker_trips')) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
                $zeroPhysicsInvariant $context) -eq 0) `
                "$context reports forbidden physics evidence in '$zeroPhysicsInvariant'."
        }
        if ($physicsAuthoritativeBatches -gt 0) {
            Assert-Stage5Condition ($physicsCommittedPrefixes -gt 0 -and
                $physicsRanges -gt 0 -and $physicsSubmittedJobs -gt 0 -and
                $physicsCompletedJobs -gt 0) `
                "$context reports authoritative physics batches without physics-specific committed work and jobs."
        }
        $isQualifyingPhysicsStress = $Entry.scenario -ceq '4v2' -and
            $Entry.simulationMode -ceq 'parallel' -and
            $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
        if ($isQualifyingPhysicsStress) {
            Assert-Stage5Condition ($physicsAuthoritativeBatches -gt 0 -and
                $physicsCommittedPrefixes -gt 0 -and $physicsRanges -gt 0 -and
                $physicsSubmittedJobs -gt 0 -and $physicsCompletedJobs -gt 0) `
                "$context qualifying parallel stress has no positive authoritative physics batch, prefix, range, and job evidence."
        }
        if ($Entry.simulationMode -ceq 'shadow') {
            Assert-Stage5Condition ($physicsShadowExecutions -gt 0 -and
                $physicsShadowMatches -eq $physicsShadowExecutions -and
                $physicsShadowPrefixes -gt 0 -and $physicsShadowRanges -gt 0 -and
                $physicsShadowSubmittedJobs -gt 0 -and
                $physicsShadowCompletedJobs -gt 0 -and
                $physicsShadowSubmittedJobs -eq $physicsShadowCompletedJobs -and
                $physicsShadowRanges -le $physicsShadowSubmittedJobs) `
                "$context shadow stress has no positive matching physics comparison backed by prefix, range, and job work."
            Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
                $physicsCommittedPrefixes -eq 0) `
                "$context shadow physics evidence incorrectly reports authoritative publication."
        }
        else {
            Assert-Stage5Condition ($physicsShadowExecutions -eq 0 -and
                $physicsShadowPrefixes -eq 0 -and $physicsShadowRanges -eq 0 -and
                $physicsShadowSubmittedJobs -eq 0 -and
                $physicsShadowCompletedJobs -eq 0 -and
                $physicsShadowMatches -eq 0 -and $physicsShadowMismatches -eq 0) `
                "$context reports physics shadow work outside shadow simulation."
        }
        if ($Entry.configuration -ceq 'serial-1' -or
            $Entry.configuration -ceq 'parallel-1') {
            Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
                $physicsCommittedPrefixes -eq 0 -and $physicsRanges -eq 0 -and
                $physicsSubmittedJobs -eq 0 -and $physicsCompletedJobs -eq 0) `
                "$context nonqualifying serial/one-worker lane reports physics authority or prepared jobs."
			foreach ($physicsPreparationField in @('physics_allocated_bytes',
				'physics_capture_ns', 'physics_prepare_ns', 'physics_wait_ns',
				'physics_commit_ns', 'physics_storage_bytes',
				'physics_storage_capacity_bytes', 'physics_storage_allocations')) {
				Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
					$physicsPreparationField $context) -eq 0) `
					"$context nonqualifying serial/one-worker lane reports physics pre-scan, capture, or storage work in '$physicsPreparationField'."
			}
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
                $physicsCommittedPrefixes -eq 0) `
                "$context reports physics authority outside parallel simulation."
        }
        $spatialEvidence = ConvertFrom-Stage5ImmutableSpatialFields $fields `
            'spatial_' $Entry $context
    }

    $effectiveWorkers = Get-Stage5UInt64Field $fields 'effective_workers' $context
    $submittedJobs = Get-Stage5UInt64Field $fields 'job_submitted' $context
    $executedJobs = Get-Stage5UInt64Field $fields 'job_executed' $context
    $fallbackJobs = Get-Stage5UInt64Field $fields 'job_fallback' $context
    $peakWorkers = Get-Stage5UInt64Field $fields 'job_peak_active_workers' $context
    $isExpectedOneWorkerFallback = $Entry.configuration -ceq 'parallel-1' -and
        ($fallbackJobs -gt 0 -or $submittedJobs -eq 0)
    if ($Entry.configuration -ceq 'serial-1') {
        Assert-Stage5Condition ($effectiveWorkers -eq 0 -and $submittedJobs -eq 0 -and
            $executedJobs -eq 0) "$context serial configuration reports active workers or jobs."
    }
    elseif ($Entry.configuration -match '^parallel-(1|2|4|8|16)$') {
        $expectedWorkers = [UInt64]$Matches[1]
        Assert-Stage5Condition ($effectiveWorkers -eq $expectedWorkers) `
            "$context effective worker count does not match explicit configuration '$($Entry.configuration)'."
        if (-not $isExpectedOneWorkerFallback) {
            Assert-Stage5Condition ($fallbackJobs -eq 0) `
                "$context reports an unexpected serial fallback."
            Assert-Stage5Condition ($submittedJobs -gt 0 -and $executedJobs -gt 0) `
                "$context did not submit and execute jobs."
            Assert-Stage5Condition ($peakWorkers -gt 0) "$context did not activate any worker."
        }
    }
    elseif ($Entry.configuration -ceq 'parallel-auto') {
        Assert-Stage5Condition ($effectiveWorkers -gt 0) `
            "$context automatic configuration did not report an effective worker."
        Assert-Stage5Condition ($fallbackJobs -eq 0 -and $submittedJobs -gt 0 -and
            $executedJobs -gt 0 -and $peakWorkers -gt 0) `
            "$context automatic configuration did not execute parallel jobs without fallback."
    }
    elseif ($Entry.configuration -ceq 'shadow-16') {
        Assert-Stage5Condition ($effectiveWorkers -eq 16) `
            "$context shadow stress did not run with 16 effective workers."
        Assert-Stage5Condition ($fallbackJobs -eq 0 -and $submittedJobs -gt 0 -and
            $executedJobs -gt 0 -and $peakWorkers -gt 0) `
            "$context shadow stress did not execute worker jobs without fallback."
    }
    else {
        throw "$context has unsupported worker configuration '$($Entry.configuration)'."
    }

    return [pscustomobject]@{
        line = $line
        finalDigest = (Get-Stage5RequiredField $fields 'final_digest' $context).ToUpperInvariant()
        endFrame = Get-Stage5UInt64Field $fields 'end_frame' $context
        winnerTeam = Get-Stage5UInt64Field $fields 'winner_team' $context
        wallMilliseconds = Get-Stage5UInt64Field $fields 'wall_ms' $context
        expectedOneWorkerFallback = $isExpectedOneWorkerFallback
        authoritativeWorkStatus = $(if ($hasAuthoritativeWorkEvidence) { 'validated' }
            else { 'unavailable-non-acceptance' })
        authoritativeCommits = $authoritativeCommits
        shadowExecutions = $shadowExecutions
        ownerFallbacks = $ownerFallbacks
        aiSubmittedJobs = $aiSubmittedJobs
        aiCompletedJobs = $aiCompletedJobs
        aiCommittedBatches = $aiCommittedBatches
        aiParallelAuthoritativeCommits = $aiParallelAuthoritativeCommits
		pathWorkerExecuted = $pathWorkerExecuted
		pathOwnerHelped = $pathOwnerHelped
		pathAuthoritativeCommits = $pathAuthoritativeCommits
		pathAuthoritativeMultiWorkerCommits =
			$pathAuthoritativeMultiWorkerCommits
		pathPeakActiveWorkers = $pathPeakWorkers
        collisionAuthoritativeCommits = $collisionAuthoritativeCommits
        collisionShadowExecutions = $collisionShadowExecutions
        collisionShadowComparedCandidates = $collisionShadowComparedCandidates
        collisionOwnerFallbacks = $collisionOwnerFallbacks
        collisionCommittedCandidates = $collisionCommittedCandidates
        collisionPreparedPairs = $collisionPreparedPairs
        collisionUniqueCandidates = $collisionUniqueCandidates
        collisionSubmittedJobs = $collisionSubmittedJobs
        collisionCompletedJobs = $collisionCompletedJobs
        physicsAuthoritativeBatches = $physicsAuthoritativeBatches
        physicsCommittedPrefixes = $physicsCommittedPrefixes
        physicsRanges = $physicsRanges
        physicsSubmittedJobs = $physicsSubmittedJobs
        physicsCompletedJobs = $physicsCompletedJobs
        physicsShadowExecutions = $physicsShadowExecutions
        physicsShadowPrefixes = $physicsShadowPrefixes
        physicsShadowRanges = $physicsShadowRanges
        physicsShadowSubmittedJobs = $physicsShadowSubmittedJobs
        physicsShadowCompletedJobs = $physicsShadowCompletedJobs
        spatialEvidence = $spatialEvidence
        fields = $fields
    }
}

function ConvertFrom-Stage5ReplayMetrics {
    param([string]$Output, [object]$Entry)
    $context = "Replay validation entry $($Entry.sequence)"
    $line = Get-Stage5SingleLine $Output 'SIMULATION_JOB_METRICS' $context
    $fields = ConvertFrom-Stage5MetricLine $line 'SIMULATION_JOB_METRICS' "$context metrics"
    foreach ($required in @('replay', 'requested_mode', 'effective_mode', 'requested_pipeline',
        'effective_pipeline', 'scheduler_started', 'workers', 'submitted', 'executed',
        'steals', 'owner_help', 'waits', 'worker_wait_rejections', 'failures', 'cancelled',
        'fallback', 'queue_latency_ns', 'max_queue_latency_ns', 'sleeps', 'wakes',
        'affinity_failures', 'queue_high_water', 'peak_active_workers', 'available_cpus',
        'reserved_owner_cpus', 'selected_worker_cpus')) {
        Get-Stage5RequiredField $fields $required "$context metrics" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'replay' $context) -ceq $Entry.replayArgument) `
        "$context replay path does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_mode' $context) -ceq $Entry.simulationMode) `
        "$context requested simulation mode does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_pipeline' $context) -ceq 'serial') `
        "$context did not honestly request the serial replay pipeline."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'effective_pipeline' $context) -ceq 'serial') `
        "$context did not run the serial replay pipeline."
    foreach ($numeric in @('scheduler_started', 'workers', 'submitted', 'executed', 'steals',
        'owner_help', 'waits', 'worker_wait_rejections', 'failures', 'cancelled', 'fallback',
        'queue_latency_ns', 'max_queue_latency_ns', 'sleeps', 'wakes', 'affinity_failures',
        'queue_high_water', 'peak_active_workers', 'available_cpus', 'reserved_owner_cpus',
        'selected_worker_cpus')) {
        Get-Stage5UInt64Field $fields $numeric "$context metrics" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'failures' $context) -eq 0) `
        "$context reports failed jobs."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'cancelled' $context) -eq 0) `
        "$context reports cancelled jobs."

    $collisionLine = Get-Stage5SingleLine $Output 'COLLISION_CANDIDATE_MANIFEST' $context
    $collisionFields = ConvertFrom-Stage5MetricLine $collisionLine `
        'COLLISION_CANDIDATE_MANIFEST' "$context collision manifest"
    $collisionFieldNames = @('authoritative_commits', 'shadow_executions',
        'shadow_compared_candidates',
        'shadow_mismatches', 'owner_fallbacks', 'unexpected_fallbacks',
        'ineligible_slices', 'stale_rejections', 'committed_candidates',
        'prepared_pairs', 'unique_candidates', 'submitted_jobs', 'completed_jobs')
    foreach ($numeric in $collisionFieldNames) {
        Get-Stage5UInt64Field $collisionFields $numeric `
            "$context collision manifest" | Out-Null
    }
    $collisionAuthoritativeCommits = Get-Stage5UInt64Field $collisionFields `
        'authoritative_commits' $context
    $collisionShadowExecutions = Get-Stage5UInt64Field $collisionFields `
        'shadow_executions' $context
    $collisionShadowComparedCandidates = Get-Stage5UInt64Field $collisionFields `
        'shadow_compared_candidates' $context
    $collisionOwnerFallbacks = Get-Stage5UInt64Field $collisionFields `
        'owner_fallbacks' $context
    $collisionCommittedCandidates = Get-Stage5UInt64Field $collisionFields `
        'committed_candidates' $context
    $collisionPreparedPairs = Get-Stage5UInt64Field $collisionFields `
        'prepared_pairs' $context
    $collisionUniqueCandidates = Get-Stage5UInt64Field $collisionFields `
        'unique_candidates' $context
    $collisionSubmittedJobs = Get-Stage5UInt64Field $collisionFields `
        'submitted_jobs' $context
    $collisionCompletedJobs = Get-Stage5UInt64Field $collisionFields `
        'completed_jobs' $context
    Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
        'shadow_mismatches' $context) -eq 0) `
        "$context reports collision shadow mismatches."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
        'unexpected_fallbacks' $context) -eq 0) `
        "$context reports unexpected collision owner fallbacks."
    Assert-Stage5Condition ($collisionCompletedJobs -eq $collisionSubmittedJobs) `
        "$context collision submitted/completed job counts do not match."
    Assert-Stage5Condition ($collisionUniqueCandidates -le $collisionPreparedPairs) `
        "$context reports more unique collision candidates than prepared pairs."
    Assert-Stage5Condition ($collisionCommittedCandidates -le $collisionUniqueCandidates) `
        "$context reports more committed collision contacts than unique candidates."
    Assert-Stage5Condition ($collisionShadowComparedCandidates -le $collisionUniqueCandidates) `
        "$context reports more shadow-compared collision insertions than unique candidates."
    if ($collisionAuthoritativeCommits -gt 0) {
        Assert-Stage5Condition ($collisionPreparedPairs -gt 0 -and
            $collisionSubmittedJobs -gt 0 -and $collisionCompletedJobs -gt 0) `
            "$context reports authoritative collision commits without collision-specific parallel work."
    }
    if ($Entry.simulationMode -cne 'parallel') {
        Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
            $collisionCommittedCandidates -eq 0) `
            "$context reports collision authority outside parallel simulation."
    }
    if ($Entry.simulationMode -cne 'shadow') {
        Assert-Stage5Condition ($collisionShadowExecutions -eq 0 -and
            $collisionShadowComparedCandidates -eq 0) `
            "$context reports collision shadow work outside shadow simulation."
    }
    if ($Entry.simulationMode -ceq 'serial') {
        foreach ($serialCollisionField in $collisionFieldNames) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
                $serialCollisionField $context) -eq 0) `
                "$context serial replay reports collision lane work in '$serialCollisionField'."
        }
    }
    elseif ($Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
            $collisionShadowExecutions -eq 0 -and
            $collisionShadowComparedCandidates -eq 0 -and
            $collisionCommittedCandidates -eq 0 -and
            $collisionPreparedPairs -eq 0 -and $collisionUniqueCandidates -eq 0 -and
            $collisionSubmittedJobs -eq 0 -and $collisionCompletedJobs -eq 0 -and
            (Get-Stage5UInt64Field $collisionFields 'stale_rejections' $context) -eq 0) `
            "$context one-worker ineligible replay reports collision prepared/publication work."
    }

    $physicsLine = Get-Stage5SingleLine $Output 'PHYSICS_INTEGRATION_MANIFEST' $context
    $physicsFields = ConvertFrom-Stage5MetricLine $physicsLine `
        'PHYSICS_INTEGRATION_MANIFEST' "$context physics manifest"
    $physicsFieldNames = @('authoritative_batches', 'committed_prefixes', 'ranges',
        'submitted_jobs', 'completed_jobs', 'allocated_bytes', 'capture_ns',
        'prepare_ns', 'wait_ns', 'commit_ns', 'storage_bytes',
        'storage_capacity_bytes', 'storage_allocations', 'shadow_executions',
        'shadow_prefixes', 'shadow_ranges', 'shadow_submitted_jobs',
        'shadow_completed_jobs', 'shadow_matches', 'shadow_mismatches', 'owner_fallbacks',
        'ineligible_slices', 'unexpected_fallbacks', 'stale_rejections',
        'circuit_breaker_trips')
    foreach ($numeric in $physicsFieldNames) {
        Get-Stage5UInt64Field $physicsFields $numeric "$context physics manifest" | Out-Null
    }
    $physicsAuthoritativeBatches = Get-Stage5UInt64Field $physicsFields `
        'authoritative_batches' $context
    $physicsCommittedPrefixes = Get-Stage5UInt64Field $physicsFields `
        'committed_prefixes' $context
    $physicsRanges = Get-Stage5UInt64Field $physicsFields 'ranges' $context
    $physicsSubmittedJobs = Get-Stage5UInt64Field $physicsFields 'submitted_jobs' $context
    $physicsCompletedJobs = Get-Stage5UInt64Field $physicsFields 'completed_jobs' $context
    $physicsShadowExecutions = Get-Stage5UInt64Field $physicsFields `
        'shadow_executions' $context
    $physicsShadowPrefixes = Get-Stage5UInt64Field $physicsFields `
        'shadow_prefixes' $context
    $physicsShadowRanges = Get-Stage5UInt64Field $physicsFields `
        'shadow_ranges' $context
    $physicsShadowSubmittedJobs = Get-Stage5UInt64Field $physicsFields `
        'shadow_submitted_jobs' $context
    $physicsShadowCompletedJobs = Get-Stage5UInt64Field $physicsFields `
        'shadow_completed_jobs' $context
    $physicsShadowMatches = Get-Stage5UInt64Field $physicsFields 'shadow_matches' $context
    $physicsShadowMismatches = Get-Stage5UInt64Field $physicsFields `
        'shadow_mismatches' $context
    Assert-Stage5Condition ($physicsSubmittedJobs -eq $physicsCompletedJobs -and
        $physicsRanges -le $physicsSubmittedJobs) `
        "$context reports inconsistent physics ranges or submitted/completed jobs."
    Assert-Stage5Condition ($physicsShadowExecutions -eq
        ($physicsShadowMatches + $physicsShadowMismatches)) `
        "$context reports inconsistent physics shadow counters."
    foreach ($zeroPhysicsInvariant in @('shadow_mismatches', 'owner_fallbacks',
        'unexpected_fallbacks', 'stale_rejections', 'circuit_breaker_trips')) {
        Assert-Stage5Condition ((Get-Stage5UInt64Field $physicsFields `
            $zeroPhysicsInvariant $context) -eq 0) `
            "$context reports forbidden physics evidence in '$zeroPhysicsInvariant'."
    }
    if ($physicsAuthoritativeBatches -gt 0) {
        Assert-Stage5Condition ($physicsCommittedPrefixes -gt 0 -and
            $physicsRanges -gt 0 -and $physicsSubmittedJobs -gt 0 -and
            $physicsCompletedJobs -gt 0) `
            "$context reports authoritative physics batches without physics-specific committed work and jobs."
    }
    Assert-Stage5Condition ($physicsShadowExecutions -eq 0 -and
        $physicsShadowPrefixes -eq 0 -and $physicsShadowRanges -eq 0 -and
        $physicsShadowSubmittedJobs -eq 0 -and
        $physicsShadowCompletedJobs -eq 0 -and
        $physicsShadowMatches -eq 0 -and $physicsShadowMismatches -eq 0) `
        "$context reports physics shadow work outside shadow simulation."
    $isQualifyingPhysicsStress = $Entry.stress -and
        $Entry.simulationMode -ceq 'parallel' -and
        $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
    if ($isQualifyingPhysicsStress) {
        Assert-Stage5Condition ($physicsAuthoritativeBatches -gt 0 -and
            $physicsCommittedPrefixes -gt 0 -and $physicsRanges -gt 0 -and
            $physicsSubmittedJobs -gt 0 -and $physicsCompletedJobs -gt 0) `
            "$context qualifying stress replay has no positive authoritative physics batch, prefix, range, and job evidence."
    }
    if ($Entry.configuration -ceq 'serial-1' -or
        $Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
            $physicsCommittedPrefixes -eq 0 -and $physicsRanges -eq 0 -and
            $physicsSubmittedJobs -eq 0 -and $physicsCompletedJobs -eq 0) `
            "$context nonqualifying serial/one-worker replay reports physics authority or prepared jobs."
		foreach ($physicsPreparationField in @('allocated_bytes', 'capture_ns',
			'prepare_ns', 'wait_ns', 'commit_ns', 'storage_bytes',
			'storage_capacity_bytes', 'storage_allocations')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $physicsFields `
				$physicsPreparationField $context) -eq 0) `
				"$context nonqualifying serial/one-worker replay reports physics pre-scan, capture, or storage work in '$physicsPreparationField'."
		}
    }

    $spatialLine = Get-Stage5SingleLine $Output 'IMMUTABLE_SPATIAL_MANIFEST' $context
    $spatialFields = ConvertFrom-Stage5MetricLine $spatialLine `
        'IMMUTABLE_SPATIAL_MANIFEST' "$context immutable-spatial manifest"
    $spatialEvidence = ConvertFrom-Stage5ImmutableSpatialFields $spatialFields `
        '' $Entry $context

    $effectiveMode = Get-Stage5RequiredField $fields 'effective_mode' $context
    Assert-Stage5Condition ($effectiveMode -ceq 'serial' -or $effectiveMode -ceq 'parallel') `
        "$context effective_mode is not a supported serial/parallel value."
    $schedulerStarted = Get-Stage5UInt64Field $fields 'scheduler_started' $context
    $workers = Get-Stage5UInt64Field $fields 'workers' $context
    $submitted = Get-Stage5UInt64Field $fields 'submitted' $context
    $executed = Get-Stage5UInt64Field $fields 'executed' $context
    $fallback = Get-Stage5UInt64Field $fields 'fallback' $context
    $isExpectedOneWorkerFallback = $Entry.configuration -ceq 'parallel-1' -and
        ($fallback -gt 0 -or $submitted -eq 0)
    if ($Entry.configuration -ceq 'serial-1') {
        Assert-Stage5Condition ($effectiveMode -ceq 'serial') "$context did not remain serial."
        Assert-Stage5Condition ($schedulerStarted -eq 0 -and $workers -eq 0 -and
            $submitted -eq 0 -and $executed -eq 0) `
            "$context serial configuration reports an active scheduler, workers, or jobs."
    }
    elseif ($Entry.configuration -match '^parallel-(1|2|4|8|16)$') {
        $expectedWorkers = [UInt64]$Matches[1]
        Assert-Stage5Condition ($effectiveMode -ceq 'parallel') `
            "$context explicit parallel scheduler unexpectedly fell back to serial."
        Assert-Stage5Condition ($schedulerStarted -eq 1 -and $workers -eq $expectedWorkers) `
            "$context scheduler/worker count does not match explicit configuration '$($Entry.configuration)'."
        if (-not $isExpectedOneWorkerFallback) {
            Assert-Stage5Condition ($submitted -gt 0 -and $executed -gt 0 -and $fallback -eq 0) `
                "$context did not execute parallel jobs without fallback."
        }
    }
    elseif ($Entry.configuration -ceq 'parallel-auto') {
        Assert-Stage5Condition ($effectiveMode -ceq 'parallel' -and $schedulerStarted -eq 1 -and
            $workers -gt 0) "$context automatic configuration did not start workers."
        Assert-Stage5Condition ($submitted -gt 0 -and $executed -gt 0 -and $fallback -eq 0) `
            "$context automatic configuration did not execute parallel jobs without fallback."
    }
    else {
        throw "$context has unsupported worker configuration '$($Entry.configuration)'."
    }

    return [pscustomobject]@{
        line = $line
        effectiveMode = $effectiveMode
        workers = Get-Stage5UInt64Field $fields 'workers' $context
        availableCpus = Get-Stage5UInt64Field $fields 'available_cpus' $context
        selectedWorkerCpus = Get-Stage5UInt64Field $fields 'selected_worker_cpus' $context
        expectedOneWorkerFallback = $isExpectedOneWorkerFallback
        collisionAuthoritativeCommits = $collisionAuthoritativeCommits
        collisionShadowExecutions = $collisionShadowExecutions
        collisionShadowComparedCandidates = $collisionShadowComparedCandidates
        collisionOwnerFallbacks = $collisionOwnerFallbacks
        collisionCommittedCandidates = $collisionCommittedCandidates
        collisionSubmittedJobs = $collisionSubmittedJobs
        collisionCompletedJobs = $collisionCompletedJobs
        physicsAuthoritativeBatches = $physicsAuthoritativeBatches
        physicsCommittedPrefixes = $physicsCommittedPrefixes
        physicsRanges = $physicsRanges
        physicsSubmittedJobs = $physicsSubmittedJobs
        physicsCompletedJobs = $physicsCompletedJobs
        physicsShadowExecutions = $physicsShadowExecutions
        physicsShadowPrefixes = $physicsShadowPrefixes
        physicsShadowRanges = $physicsShadowRanges
        physicsShadowSubmittedJobs = $physicsShadowSubmittedJobs
        physicsShadowCompletedJobs = $physicsShadowCompletedJobs
        spatialEvidence = $spatialEvidence
        collisionFields = $collisionFields
        physicsFields = $physicsFields
        spatialFields = $spatialFields
        fields = $fields
    }
}

function Get-Stage5FileSha256 {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function ConvertFrom-Stage5ReplayResult {
    param([string]$Output, [object]$Entry)
    $context = "Replay validation entry $($Entry.sequence)"
    $line = Get-Stage5SingleLine $Output 'SIMULATION_REPLAY_RESULT' $context
    $fields = ConvertFrom-Stage5MetricLine $line 'SIMULATION_REPLAY_RESULT' "$context result"
    foreach ($required in @('replay', 'final_frame', 'final_crc')) {
        Get-Stage5RequiredField $fields $required "$context result" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'replay' $context) -ceq $Entry.replayArgument) `
        "$context result replay path does not match the plan."
    $finalFrame = Get-Stage5UInt64Field $fields 'final_frame' "$context result"
    Assert-Stage5Condition ($finalFrame -gt 0) "$context result final_frame must be positive."
    $finalCRC = Get-Stage5RequiredField $fields 'final_crc' "$context result"
    Assert-Stage5Condition ($finalCRC -match '^[0-9A-Fa-f]{8}$') `
        "$context result final_crc must contain exactly eight hexadecimal characters."
    return [pscustomobject]@{
        line = $line
        finalFrame = $finalFrame
        finalCRC = $finalCRC.ToUpperInvariant()
        fields = $fields
    }
}

function Get-Stage5TimingEvidence {
    param([string]$TimingDirectory, [string]$Context)
    Assert-Stage5Condition (Test-Path -LiteralPath $TimingDirectory -PathType Container) `
        "$Context timing directory is missing."
    $files = @(Get-ChildItem -LiteralPath $TimingDirectory -Filter 'frame-timing-*.csv' -File)
    Assert-Stage5Condition ($files.Count -eq 1) `
        "$Context requires exactly one frame-timing CSV file."
    $expectedHeader = 'session,mode,frame_begin,frame_end,logic_frames,wall_ms,phase,samples,total_ms,avg_ms,p95_upper_ms,p99_upper_ms,max_ms,over_33ms,over_100ms'
    $rawLines = @(Get-Content -LiteralPath $files[0].FullName)
    Assert-Stage5Condition ($rawLines.Count -gt 0) "$Context frame-timing CSV is empty."
    $header = [string]$rawLines[0]
    Assert-Stage5Condition ($header -ceq $expectedHeader) "$Context frame-timing CSV header is invalid."
    foreach ($rawRow in @($rawLines | Select-Object -Skip 1)) {
        Assert-Stage5Condition ($rawRow.Split(',').Count -eq 15) `
            "$Context frame-timing CSV row has an invalid column count."
    }
    $rows = @(Import-Csv -LiteralPath $files[0].FullName)
    Assert-Stage5Condition ($rows.Count -gt 0) "$Context frame-timing CSV contains no data rows."
    $phases = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $phaseSummaries = @{}
    [UInt64]$maximumFrameEnd = 0
    foreach ($row in $rows) {
        [UInt64]$frameBegin = 0
        [UInt64]$frameEnd = 0
        Assert-Stage5Condition ([UInt64]::TryParse([string]$row.frame_begin, [ref]$frameBegin) -and
            [UInt64]::TryParse([string]$row.frame_end, [ref]$frameEnd) -and $frameEnd -ge $frameBegin) `
            "$Context frame-timing row has an invalid frame range."
        foreach ($integerName in @('session', 'frame_begin', 'frame_end', 'logic_frames', 'samples',
            'over_33ms', 'over_100ms')) {
            [UInt64]$integerValue = 0
            Assert-Stage5Condition ([UInt64]::TryParse([string]$row.$integerName, [ref]$integerValue)) `
                "$Context frame-timing row has invalid $integerName."
            if ($integerName -ceq 'frame_end' -and $integerValue -gt $maximumFrameEnd) {
                $maximumFrameEnd = $integerValue
            }
            if ($integerName -ceq 'samples') {
                Assert-Stage5Condition ($integerValue -gt 0) `
                    "$Context frame-timing row samples must be positive."
            }
        }
        foreach ($decimalName in @('wall_ms', 'total_ms', 'avg_ms', 'p95_upper_ms',
            'p99_upper_ms', 'max_ms')) {
            [double]$decimalValue = 0
            Assert-Stage5Condition ([double]::TryParse([string]$row.$decimalName,
                [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture,
                [ref]$decimalValue) -and -not [double]::IsNaN($decimalValue) -and
                -not [double]::IsInfinity($decimalValue) -and $decimalValue -ge 0) `
                "$Context frame-timing row has invalid $decimalName."
        }
        Assert-Stage5Condition ([string]$row.mode -ceq 'headless') `
            "$Context frame-timing row must identify the headless validation mode."
        Assert-Stage5Condition ([string]$row.phase -match '^[a-z][a-z0-9_]*$') `
            "$Context frame-timing row phase is invalid."
        $phaseName = [string]$row.phase
        $phases.Add($phaseName) | Out-Null
        [UInt64]$rowSamples = 0
        [double]$rowTotalMilliseconds = 0
        [void][UInt64]::TryParse([string]$row.samples, [ref]$rowSamples)
        [void][double]::TryParse([string]$row.total_ms,
            [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture,
            [ref]$rowTotalMilliseconds)
        if (-not $phaseSummaries.ContainsKey($phaseName)) {
            $phaseSummaries[$phaseName] = [pscustomobject]@{
                phase = $phaseName; samples = [UInt64]0; totalMilliseconds = [double]0
            }
        }
        $phaseSummaries[$phaseName].samples = [UInt64](
            $phaseSummaries[$phaseName].samples + $rowSamples)
        $phaseSummaries[$phaseName].totalMilliseconds = [double](
            $phaseSummaries[$phaseName].totalMilliseconds + $rowTotalMilliseconds)
    }
    Assert-Stage5Condition ($maximumFrameEnd -gt 0) `
        "$Context frame-timing CSV has no positive final frame."
    Assert-Stage5Condition ($phases.Contains('frame') -and $phases.Contains('logic')) `
        "$Context frame-timing CSV must contain frame and logic phases."
    return [pscustomobject]@{
        file = $files[0].FullName
        sha256 = Get-Stage5FileSha256 $files[0].FullName
        header = $header
        rows = $rows.Count
        maximumFrameEnd = $maximumFrameEnd
        phases = @($phases | Sort-Object)
        phaseSummaries = @($phaseSummaries.Values | Sort-Object phase)
    }
}

function Assert-Stage5AiDeterminism {
    param([object[]]$Results, [string[]]$ExpectedConfigurations, [int]$ExpectedRepeats,
        [string]$ShadowConfiguration = '', [string[]]$ExpectedDeterminismKeys = @())
    $aiResults = @($Results | Where-Object { $_.kind -ceq 'ai' })
    $regularResults = @($aiResults | Where-Object {
        $ExpectedConfigurations -ccontains $_.configuration
    })
    $knownConfigurations = @($ExpectedConfigurations)
    if (-not [string]::IsNullOrEmpty($ShadowConfiguration)) {
        $knownConfigurations += $ShadowConfiguration
    }
    Assert-Stage5Condition (@($aiResults | Where-Object {
        $knownConfigurations -cnotcontains $_.configuration
    }).Count -eq 0) 'AI results contain an unsupported worker configuration.'
    if ($ExpectedDeterminismKeys.Count -gt 0) {
        $expectedRegularCount = $ExpectedDeterminismKeys.Count *
            $ExpectedConfigurations.Count * $ExpectedRepeats
        Assert-Stage5Condition ($regularResults.Count -eq $expectedRegularCount) `
            "AI matrix has $($regularResults.Count) regular results; expected the complete $expectedRegularCount-result scenario/seed/configuration/repeat cross-product."
        $actualDeterminismKeys = @($regularResults | ForEach-Object { $_.determinismKey } |
            Sort-Object -Unique)
        Assert-Stage5Condition ($actualDeterminismKeys.Count -eq $ExpectedDeterminismKeys.Count -and
            @($actualDeterminismKeys | Where-Object { $ExpectedDeterminismKeys -cnotcontains $_ }).Count -eq 0) `
            'AI matrix determinism cases do not match the required scenario/seed cross-product.'
        $semanticRunKeys = @($regularResults | ForEach-Object {
            "$($_.determinismKey)|$($_.configuration)|$($_.repeat)"
        })
        Assert-Stage5Condition (@($semanticRunKeys | Sort-Object -Unique).Count -eq
            $semanticRunKeys.Count) `
            'AI matrix contains a duplicate scenario/seed/configuration/repeat result.'
    }
    foreach ($group in @($regularResults | Group-Object determinismKey)) {
        $expectedCount = $ExpectedConfigurations.Count * $ExpectedRepeats
        Assert-Stage5Condition ($group.Count -eq $expectedCount) `
            "AI case '$($group.Name)' has $($group.Count) results; expected $expectedCount."
        foreach ($configuration in $ExpectedConfigurations) {
            Assert-Stage5Condition (@($group.Group | Where-Object { $_.configuration -ceq $configuration }).Count -eq $ExpectedRepeats) `
                "AI case '$($group.Name)' is missing repeats for configuration '$configuration'."
        }
        $reference = $group.Group[0].aiEvidence
        foreach ($result in $group.Group) {
            Assert-Stage5Condition ($result.aiEvidence.finalDigest -ceq $reference.finalDigest) `
                "AI case '$($group.Name)' final_digest differs across repeats or worker configurations."
            Assert-Stage5Condition ($result.aiEvidence.endFrame -eq $reference.endFrame) `
                "AI case '$($group.Name)' end_frame differs across repeats or worker configurations."
            Assert-Stage5Condition ($result.aiEvidence.winnerTeam -eq $reference.winnerTeam) `
                "AI case '$($group.Name)' winner_team differs across repeats or worker configurations."
        }
    }
    if (-not [string]::IsNullOrEmpty($ShadowConfiguration)) {
        $shadowResults = @($aiResults | Where-Object {
            $_.configuration -ceq $ShadowConfiguration
        })
        Assert-Stage5Condition ($shadowResults.Count -eq 1) `
            "AI validation requires exactly one '$ShadowConfiguration' collision stress result."
        foreach ($shadowResult in $shadowResults) {
            $references = @($regularResults | Where-Object {
                $_.determinismKey -ceq $shadowResult.determinismKey
            })
            Assert-Stage5Condition ($references.Count -gt 0) `
                "AI shadow case '$($shadowResult.determinismKey)' has no regular matrix reference."
            $reference = $references[0].aiEvidence
            Assert-Stage5Condition ($shadowResult.aiEvidence.finalDigest -ceq $reference.finalDigest -and
                $shadowResult.aiEvidence.endFrame -eq $reference.endFrame -and
                $shadowResult.aiEvidence.winnerTeam -eq $reference.winnerTeam) `
                "AI shadow case '$($shadowResult.determinismKey)' differs from the regular matrix outcome."
        }
    }
}

function Assert-Stage5AuthoritativeWorkEvidence {
    param([object[]]$Results)
    $stressParallel = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.configuration -match '^parallel-(?:2|4|8|16|auto)$'
    })
    Assert-Stage5Condition ($stressParallel.Count -gt 0) `
        'Overall Stage 5 acceptance requires a parallel AI stress scenario.'
    foreach ($result in $stressParallel) {
        Assert-Stage5Condition ($null -ne $result.aiEvidence -and
            $result.aiEvidence.authoritativeWorkStatus -ceq 'validated') `
            "AI stress entry $($result.sequence) is missing authoritative Stage 5 work evidence."
    }
    $authoritative = @($stressParallel | Where-Object {
        $_.aiEvidence.aiParallelAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.aiSubmittedJobs -gt 0 -and
        $_.aiEvidence.aiCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($authoritative.Count -gt 0) `
        'AI stress evidence has no authoritative Stage 5 owner commit backed by AI-specific submitted/completed jobs; global or shadow-only scheduler activity is insufficient.'
    $collisionAuthoritative = @($stressParallel | Where-Object {
        $_.aiEvidence.collisionAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.collisionCommittedCandidates -gt 0 -and
        $_.aiEvidence.collisionSubmittedJobs -gt 0 -and
        $_.aiEvidence.collisionCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($collisionAuthoritative.Count -gt 0) `
        'AI stress evidence has no authoritative collision contact commit backed by collision-specific submitted/completed jobs; AI counters cannot proxy collision work.'
	$physicsAuthoritative = @($stressParallel | Where-Object {
		$_.aiEvidence.physicsAuthoritativeBatches -gt 0 -and
		$_.aiEvidence.physicsCommittedPrefixes -gt 0 -and
		$_.aiEvidence.physicsRanges -gt 0 -and
		$_.aiEvidence.physicsSubmittedJobs -gt 0 -and
		$_.aiEvidence.physicsCompletedJobs -gt 0
	})
	Assert-Stage5Condition ($physicsAuthoritative.Count -gt 0) `
		'AI stress evidence has no authoritative physics prefix commit backed by physics-specific ranges and submitted/completed jobs; AI, collision, path, or global counters cannot proxy physics work.'
	$pathAuthoritative = @($stressParallel | Where-Object {
		$_.aiEvidence.pathWorkerExecuted -gt 1 -and
		$_.aiEvidence.pathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.pathAuthoritativeMultiWorkerCommits -gt 0 -and
		$_.aiEvidence.pathAuthoritativeCommits -gt 0
	})
	Assert-Stage5Condition ($pathAuthoritative.Count -gt 0) `
		'AI stress evidence has no authoritative direct-path commit backed by a multi-request batch using more than one physical path worker; global scheduler, AI, collision, owner-help, or shadow counters cannot proxy path work.'
    $spatialAuthoritative = @($stressParallel | Where-Object {
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($spatialAuthoritative.Count -gt 0) `
        'AI stress evidence has no authoritative immutable-spatial healing and point-defense-laser work backed by consumer-specific physical-worker jobs.'
    $coLocatedAuthoritative = @($stressParallel | Where-Object {
        $_.aiEvidence.aiParallelAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.aiSubmittedJobs -gt 0 -and
        $_.aiEvidence.aiCompletedJobs -gt 0 -and
        $_.aiEvidence.collisionAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.collisionCommittedCandidates -gt 0 -and
        $_.aiEvidence.collisionSubmittedJobs -gt 0 -and
        $_.aiEvidence.collisionCompletedJobs -gt 0 -and
        $_.aiEvidence.physicsAuthoritativeBatches -gt 0 -and
        $_.aiEvidence.physicsCommittedPrefixes -gt 0 -and
        $_.aiEvidence.physicsRanges -gt 0 -and
        $_.aiEvidence.physicsSubmittedJobs -gt 0 -and
        $_.aiEvidence.physicsCompletedJobs -gt 0 -and
		$_.aiEvidence.pathWorkerExecuted -gt 1 -and
		$_.aiEvidence.pathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.pathAuthoritativeMultiWorkerCommits -gt 0 -and
        $_.aiEvidence.pathAuthoritativeCommits -gt 0 -and
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($coLocatedAuthoritative.Count -gt 0) `
        'Overall Stage 5 acceptance requires AI, collision, physics, direct-path, healing-spatial, and PDL-spatial authority on the same qualifying parallel 4v2 stress execution; evidence split across executions is insufficient.'
    $collisionShadow = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.configuration -ceq 'shadow-16' -and
        $null -ne $_.aiEvidence -and
        $_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
        $_.aiEvidence.collisionShadowExecutions -gt 0 -and
        $_.aiEvidence.collisionShadowComparedCandidates -gt 0 -and
        $_.aiEvidence.collisionPreparedPairs -gt 0 -and
        $_.aiEvidence.collisionUniqueCandidates -gt 0 -and
        $_.aiEvidence.collisionSubmittedJobs -gt 0 -and
        $_.aiEvidence.collisionCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($collisionShadow.Count -gt 0) `
        'AI stress evidence has no installed shadow collision comparison covering a successful legacy insertion and backed by collision-specific prepared work and jobs.'
    $physicsShadow = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.configuration -ceq 'shadow-16' -and
        $null -ne $_.aiEvidence -and
        $_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
        $_.aiEvidence.physicsShadowExecutions -gt 0 -and
        $_.aiEvidence.physicsShadowPrefixes -gt 0 -and
        $_.aiEvidence.physicsShadowRanges -gt 0 -and
        $_.aiEvidence.physicsShadowSubmittedJobs -gt 0 -and
        $_.aiEvidence.physicsShadowCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($physicsShadow.Count -gt 0) `
        'AI stress evidence has no installed matching shadow physics comparison backed by physics-specific prefix, range, and submitted/completed jobs.'
    $spatialShadow = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.configuration -ceq 'shadow-16' -and
        $null -ne $_.aiEvidence -and
        $_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.shadowQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.shadowMatches -eq
            $_.aiEvidence.spatialEvidence.healing.shadowQueries -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.shadowQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.shadowMatches -eq
            $_.aiEvidence.spatialEvidence.pdl.shadowQueries -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($spatialShadow.Count -gt 0) `
        'AI stress evidence has no installed matching immutable-spatial healing and PDL shadow comparison backed by consumer-specific physical-worker jobs.'
}

function Assert-Stage5CollisionTimingEvidence {
    param([object]$TimingEvidence, [object]$CollisionEvidence, [string]$Context)
    if ($null -eq $CollisionEvidence -or
        ($CollisionEvidence.collisionAuthoritativeCommits -eq 0 -and
         $CollisionEvidence.collisionShadowExecutions -eq 0)) {
        return
    }
    foreach ($requiredPhase in @('collision_admission', 'simulation_snapshot',
        'simulation_parallel', 'simulation_wait', 'simulation_reduce',
        'collision_live_validation', 'simulation_commit')) {
        Assert-Stage5Condition ($TimingEvidence.phases -contains $requiredPhase) `
            "$Context collision evidence is missing timing phase '$requiredPhase'."
    }
    if ($CollisionEvidence.collisionShadowExecutions -gt 0) {
        foreach ($shadowPhase in @('collision_existing_filter',
            'collision_commit_prepare', 'simulation_shadow_compare')) {
            Assert-Stage5Condition ($TimingEvidence.phases -contains $shadowPhase) `
                "$Context collision shadow evidence is missing timing phase '$shadowPhase'."
        }
    }
}

function Assert-Stage5ReplayDeterminism {
    param([object[]]$Results)
    foreach ($group in @($Results | Where-Object { $_.kind -ceq 'replay' } | Group-Object determinismKey)) {
        $reference = $group.Group[0].replayResult
        foreach ($result in $group.Group) {
            Assert-Stage5Condition ($result.replayResult.finalFrame -eq $reference.finalFrame) `
                "Replay '$($group.Name)' final_frame differs across repeats or worker configurations."
            Assert-Stage5Condition ($result.replayResult.finalCRC -ceq $reference.finalCRC) `
                "Replay '$($group.Name)' final_crc differs across repeats or worker configurations."
        }
    }
}

function Get-Stage5Median {
    param([double[]]$Values)
    Assert-Stage5Condition ($Values.Count -gt 0) 'Median requires at least one value.'
    Assert-Stage5Condition (@($Values | Where-Object {
        [double]::IsNaN($_) -or [double]::IsInfinity($_)
    }).Count -eq 0) 'Median requires finite values.'
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Read-Stage5PerformanceBaseline {
    param([string]$Path, [string]$StressFixtureSha256, [string]$ExpectedExecutableSha256)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($Path)) `
        'Stage3PerformanceBaselinePath is required when the performance gate is requested.'
    Assert-Stage5Condition ($ExpectedExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'ExpectedStage3ExecutableSha256 is required and must contain exactly 64 hexadecimal characters.'
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Stage 3 performance baseline was not found: $full"
    $baseline = ConvertFrom-Stage5JsonDictionary $full
    $baselineNames = @('schemaVersion', 'stage', 'architecture', 'executableSha256',
        'fixtureSha256', 'configuration', 'physicalCoreCount', 'availableCpus',
        'warmupRuns', 'wallMilliseconds')
    Assert-Stage5JsonShape $baseline $baselineNames 'Stage 3 performance baseline'
    $schemaVersion = Get-Stage5JsonValue $baseline 'schemaVersion' 'Stage 3 performance baseline'
    $stage = Get-Stage5JsonValue $baseline 'stage' 'Stage 3 performance baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $stage -is [string] -and $stage -ceq 'Stage3') `
        'Stage 3 performance baseline identity is invalid.'
    $baselineExecutableHash = Get-Stage5JsonValue $baseline 'executableSha256' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($baselineExecutableHash -is [string] -and
        $baselineExecutableHash -match '^[0-9A-Fa-f]{64}$') `
        'Stage 3 performance baseline requires an exact executable SHA-256.'
    Assert-Stage5Condition ($baselineExecutableHash.ToUpperInvariant() -ceq
        $ExpectedExecutableSha256.ToUpperInvariant()) `
        'Stage 3 performance baseline executable hash does not match the independently supplied expected hash.'
    $baselineFixtureHash = Get-Stage5JsonValue $baseline 'fixtureSha256' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($baselineFixtureHash -is [string] -and
        $baselineFixtureHash.ToUpperInvariant() -ceq $StressFixtureSha256.ToUpperInvariant()) `
        'Stage 3 performance baseline fixture hash does not match the current stress fixture.'
    $architecture = Get-Stage5JsonValue $baseline 'architecture' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($architecture -is [string] -and $architecture -ceq 'x64') `
        'Stage 3 performance baseline must identify the native x64 architecture.'
    $configuration = Get-Stage5JsonValue $baseline 'configuration' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($configuration -is [string] -and $configuration -ceq 'parallel-1') `
        'Stage 3 performance baseline must use the forced one-worker configuration.'
    $baselinePhysicalCores = Get-Stage5JsonValue $baseline 'physicalCoreCount' 'Stage 3 performance baseline'
    $baselineAvailableCpus = Get-Stage5JsonValue $baseline 'availableCpus' 'Stage 3 performance baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $baselinePhysicalCores) -and
        (Test-Stage5JsonInteger $baselineAvailableCpus) -and $baselinePhysicalCores -gt 0 -and
        $baselineAvailableCpus -gt 0) `
        'Stage 3 performance baseline requires physical-core and available-CPU topology evidence.'
    $baselineWarmupRuns = Get-Stage5JsonValue $baseline 'warmupRuns' 'Stage 3 performance baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $baselineWarmupRuns) -and $baselineWarmupRuns -ge 1) `
        'Stage 3 performance baseline must identify at least one warm-up run.'
    $sampleValues = Get-Stage5JsonValue $baseline 'wallMilliseconds' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($sampleValues -is [Array]) `
        'Stage 3 performance baseline wallMilliseconds must be a JSON array.'
    Assert-Stage5Condition (@($sampleValues | Where-Object { -not (Test-Stage5JsonNumber $_) }).Count -eq 0) `
        'Stage 3 performance baseline wallMilliseconds must contain only finite JSON numbers.'
    $samples = @($sampleValues | ForEach-Object { [double]$_ })
    Assert-Stage5Condition ($samples.Count - [int]$baselineWarmupRuns -ge 3) `
        'Stage 3 performance baseline requires at least three measured runs after warm-up.'
    Assert-Stage5Condition (@($samples | Where-Object { $_ -le 0 }).Count -eq 0) `
        'Stage 3 performance baseline wall times must be positive.'
    return [pscustomobject]@{
        file = $full
        fileSha256 = Get-Stage5FileSha256 $full
        executableSha256 = $baselineExecutableHash.ToUpperInvariant()
        fixtureSha256 = $baselineFixtureHash.ToUpperInvariant()
        physicalCoreCount = [int]$baselinePhysicalCores
        availableCpus = [int]$baselineAvailableCpus
        warmupRuns = [int]$baselineWarmupRuns
        wallMilliseconds = $samples
        expectedExecutableSha256 = $ExpectedExecutableSha256.ToUpperInvariant()
        evidenceFile = $null
        measuredMedianMilliseconds = Get-Stage5Median @($samples | Select-Object -Skip ([int]$baselineWarmupRuns))
    }
}

function Get-Stage5MeasuredConfiguration {
    param([object[]]$Results, [string]$Configuration, [int]$WarmupRuns,
        [int]$MinimumMeasuredRuns)
    $ordered = @($Results | Where-Object { $_.configuration -ceq $Configuration } | Sort-Object sequence)
    Assert-Stage5Condition ($ordered.Count - $WarmupRuns -ge $MinimumMeasuredRuns) `
        "Performance configuration '$Configuration' requires one warm-up and at least $MinimumMeasuredRuns measured runs."
    $measured = @($ordered | Select-Object -Skip $WarmupRuns)
    Assert-Stage5Condition (@($ordered | Where-Object {
        $wall = [double]$_.wallMilliseconds
        [double]::IsNaN($wall) -or [double]::IsInfinity($wall) -or $wall -le 0
    }).Count -eq 0) `
        "Performance configuration '$Configuration' contains a non-finite or non-positive wall time."
    $workerCounts = @($measured | ForEach-Object { [UInt64]$_.replayMetrics.workers } | Select-Object -Unique)
    $availableCpuCounts = @($measured | ForEach-Object {
        [UInt64]$_.replayMetrics.availableCpus
    } | Select-Object -Unique)
    $selectedWorkerCpuCounts = @($measured | ForEach-Object {
        [UInt64]$_.replayMetrics.selectedWorkerCpus
    } | Select-Object -Unique)
    Assert-Stage5Condition ($workerCounts.Count -eq 1 -and $availableCpuCounts.Count -eq 1 -and
        $selectedWorkerCpuCounts.Count -eq 1) `
        "Performance configuration '$Configuration' topology varies across measured runs."
    $collisionPhaseNames = @('collision_admission', 'simulation_snapshot',
        'simulation_parallel', 'simulation_wait', 'simulation_reduce',
        'collision_live_validation', 'collision_existing_filter',
        'simulation_commit', 'collision_commit_prepare',
        'simulation_shadow_compare')
    $collisionPhaseEvidence = @($measured | ForEach-Object {
        $timingProperty = $_.PSObject.Properties['timingEvidence']
        $summaries = if ($null -ne $timingProperty -and
            $null -ne $timingProperty.Value -and
            $null -ne $timingProperty.Value.PSObject.Properties['phaseSummaries']) {
            @($timingProperty.Value.phaseSummaries | Where-Object {
                $collisionPhaseNames -ccontains $_.phase
            })
        }
        else { @() }
        [pscustomobject]@{ sequence = $_.sequence; phases = $summaries }
    })
    return [pscustomobject]@{
        configuration = $Configuration
        warmupRuns = $WarmupRuns
        measuredRuns = $measured.Count
        rawWallMilliseconds = @($ordered | ForEach-Object { [double]$_.wallMilliseconds })
        warmupWallMilliseconds = @($ordered | Select-Object -First $WarmupRuns |
            ForEach-Object { [double]$_.wallMilliseconds })
        measuredWallMilliseconds = @($measured | ForEach-Object { [double]$_.wallMilliseconds })
        medianWallMilliseconds = Get-Stage5Median @($measured | ForEach-Object { [double]$_.wallMilliseconds })
        workers = [UInt64]$workerCounts[0]
        availableCpus = [UInt64]$availableCpuCounts[0]
        selectedWorkerCpus = [UInt64]$selectedWorkerCpuCounts[0]
        collisionPhaseEvidence = $collisionPhaseEvidence
    }
}

function Measure-Stage5Performance {
    param([object[]]$Results, [int]$PhysicalCoreCount, [object]$Stage3Baseline,
        [int]$WarmupRuns = 1, [int]$MinimumMeasuredRuns = 3)
    Assert-Stage5Condition ($null -ne $Stage3Baseline) `
        'Stage 3 performance baseline evidence is required for the performance gate.'
    $stress = @($Results | Where-Object { $_.kind -ceq 'replay' -and $_.stress })
    Assert-Stage5Condition ($stress.Count -gt 0) 'Performance gate has no stress replay results.'
    $one = Get-Stage5MeasuredConfiguration $stress 'parallel-1' $WarmupRuns $MinimumMeasuredRuns
    $eight = Get-Stage5MeasuredConfiguration $stress 'parallel-8' $WarmupRuns $MinimumMeasuredRuns
    $regressionRatio = $one.medianWallMilliseconds / $Stage3Baseline.measuredMedianMilliseconds
    $speedup8 = $one.medianWallMilliseconds / $eight.medianWallMilliseconds
    $report = [ordered]@{
        schemaVersion = 2
        status = 'passed'
        measurementScope = 'aggregate-stage5-stress-replay-throughput'
        collisionSpecificSpeedupClaim = $false
        collisionEvidenceScope = 'separate-live-stress-authority-and-frame-phase-evidence'
        collisionTimingPhases = @('collision_admission', 'simulation_snapshot',
            'simulation_parallel', 'simulation_wait', 'simulation_reduce',
            'collision_live_validation', 'collision_existing_filter',
            'simulation_commit', 'collision_commit_prepare',
            'simulation_shadow_compare')
        physicalCoreCount = $PhysicalCoreCount
        warmupRuns = $WarmupRuns
        minimumMeasuredRuns = $MinimumMeasuredRuns
        stage3BaselineSourceFile = $Stage3Baseline.file
        stage3BaselineEvidenceFile = $Stage3Baseline.evidenceFile
        stage3ExecutableSha256 = $Stage3Baseline.executableSha256
        independentlyExpectedStage3ExecutableSha256 = $Stage3Baseline.expectedExecutableSha256
        stage3BaselineFileSha256 = $Stage3Baseline.fileSha256
        stage3RawWallMilliseconds = $Stage3Baseline.wallMilliseconds
        stage3PhysicalCoreCount = $Stage3Baseline.physicalCoreCount
        stage3AvailableCpus = $Stage3Baseline.availableCpus
        stage3OneWorkerMedianMilliseconds = $Stage3Baseline.measuredMedianMilliseconds
        currentOneWorker = $one
        currentEightWorker = $eight
        oneWorkerRegressionRatio = $regressionRatio
        eightWorkerSpeedup = $speedup8
        sixteenWorker = $null
        eightToSixteenSpeedup = $null
        sixteenWorkerStatus = 'unsupported-host-topology'
        failures = @()
    }
    $failures = New-Object 'Collections.Generic.List[string]'
    if ($Stage3Baseline.physicalCoreCount -ne $PhysicalCoreCount -or
        $Stage3Baseline.availableCpus -ne $one.availableCpus -or
        $eight.availableCpus -ne $one.availableCpus) {
        $failures.Add('Stage 3 baseline topology does not match the current one-worker measurement topology.') | Out-Null
        $report.status = 'failed'
    }
    $eightWorkerTopologyUnsupported = $PhysicalCoreCount -lt 8 -or
        $eight.availableCpus -lt 8 -or $eight.workers -lt 8 -or
        $eight.selectedWorkerCpus -lt 8
    if ($eightWorkerTopologyUnsupported) {
        $report.status = 'unsupported-host-topology'
        $failures.Add('Eight-worker performance target requires at least eight physical cores, available CPUs, workers, and selected worker CPUs.') | Out-Null
    }
    else {
        if ($speedup8 -lt 2.0) {
            $failures.Add(('Eight-worker median throughput speedup {0:N3}x is below 2.0x.' -f $speedup8)) | Out-Null
        }
    }
    if ($regressionRatio -gt 1.05) {
        $failures.Add(('One-worker median wall-time ratio {0:N3} exceeds the 1.05 Stage 3 regression limit.' -f $regressionRatio)) | Out-Null
        $report.status = 'failed'
    }
    if ($PhysicalCoreCount -ge 16) {
        $sixteen = Get-Stage5MeasuredConfiguration $stress 'parallel-16' $WarmupRuns $MinimumMeasuredRuns
        $report.sixteenWorker = $sixteen
        if ($sixteen.availableCpus -ne $one.availableCpus -or $sixteen.availableCpus -lt 16 -or
            $sixteen.workers -lt 16 -or $sixteen.selectedWorkerCpus -lt 16) {
            $report.sixteenWorkerStatus = 'unsupported-runtime-topology'
            $failures.Add('Host has 16 physical cores but the runtime did not expose 16 effective selected worker CPUs.') | Out-Null
        }
        else {
            $scale16 = $eight.medianWallMilliseconds / $sixteen.medianWallMilliseconds
            $report.eightToSixteenSpeedup = $scale16
            $report.sixteenWorkerStatus = if ($scale16 -gt 1.0) { 'passed' } else { 'failed' }
            if ($scale16 -le 1.0) {
                $failures.Add(('Sixteen-worker median throughput did not scale positively from eight workers ({0:N3}x).' -f $scale16)) | Out-Null
            }
        }
    }
    if ($failures.Count -gt 0 -and $report.status -ceq 'passed') { $report.status = 'failed' }
    if ($eightWorkerTopologyUnsupported) { $report.status = 'unsupported-host-topology' }
    $report.failures = $failures.ToArray()
    return [pscustomobject]$report
}

function Invoke-Stage5RegistryRestore {
    param([object[]]$Snapshots, [scriptblock]$RestoreAction)
    Assert-Stage5Condition ($null -ne $RestoreAction) 'Registry restore action is required.'
    $errors = New-Object 'Collections.Generic.List[string]'
    for ($index = $Snapshots.Count - 1; $index -ge 0; --$index) {
        try {
            & $RestoreAction $Snapshots[$index]
        }
        catch {
            $errors.Add("snapshot index $index`: $($_.Exception.Message)") | Out-Null
        }
    }
    if ($errors.Count -gt 0) {
        throw "Registry restoration failed after attempting every snapshot: $($errors -join ' | ')"
    }
}

function Invoke-Stage5RegistrySetupTransaction {
    param([string[]]$SubKeys, [scriptblock]$EnsureSubKeyAction,
        [scriptblock]$CaptureValueAction, [scriptblock]$SetValueAction,
        [scriptblock]$RegisterSnapshotAction, [scriptblock]$RestoreValueAction,
        [scriptblock]$CleanupCreatedSubKeysAction, [object]$ActionContext = $null,
        [scriptblock]$AfterSegmentAction = $null,
        [scriptblock]$AfterValueWriteAction = $null)
    Assert-Stage5Condition ($null -ne $EnsureSubKeyAction -and
        $null -ne $CaptureValueAction -and $null -ne $SetValueAction -and
        $null -ne $RegisterSnapshotAction -and $null -ne $RestoreValueAction -and
        $null -ne $CleanupCreatedSubKeysAction) `
        'Registry setup transaction requires every setup, registration, and rollback action.'
    $createdSubKeys = New-Object 'Collections.Generic.List[string]'
    $snapshot = $null
    $captureComplete = $false
    $setAttempted = $false
    try {
        foreach ($subKey in $SubKeys) {
            if ([bool](& $EnsureSubKeyAction $subKey $ActionContext)) {
                $createdSubKeys.Add($subKey) | Out-Null
            }
            if ($null -ne $AfterSegmentAction) {
                & $AfterSegmentAction $subKey $createdSubKeys.Count $ActionContext | Out-Null
            }
        }
        $snapshot = & $CaptureValueAction $createdSubKeys.ToArray() $ActionContext
        Assert-Stage5Condition ($null -ne $snapshot) `
            'Registry setup transaction did not capture an original-value snapshot.'
        $captureComplete = $true
        $setAttempted = $true
        & $SetValueAction $snapshot $ActionContext | Out-Null
        if ($null -ne $AfterValueWriteAction) {
            & $AfterValueWriteAction $snapshot $ActionContext | Out-Null
        }
        & $RegisterSnapshotAction $snapshot $ActionContext | Out-Null
        return
    }
    catch {
        $setupError = $_.Exception.Message
        $rollbackErrors = New-Object 'Collections.Generic.List[string]'
        if ($setAttempted -and $captureComplete) {
            try { & $RestoreValueAction $snapshot $ActionContext | Out-Null }
            catch { $rollbackErrors.Add("value restore: $($_.Exception.Message)") | Out-Null }
        }
        try { & $CleanupCreatedSubKeysAction $createdSubKeys.ToArray() $ActionContext | Out-Null }
        catch { $rollbackErrors.Add("created-key cleanup: $($_.Exception.Message)") | Out-Null }
        $rollbackStatus = if ($rollbackErrors.Count -eq 0) {
            'completed'
        }
        else {
            $rollbackErrors -join ' | '
        }
        throw "Registry setup transaction failed: setup: $setupError | rollback: $rollbackStatus"
    }
}

function Test-Stage5RegistryLeafRemoval {
    param([bool]$HadKey, [string[]]$ValueNames, [string[]]$SubKeyNames)
    return -not $HadKey -and @($ValueNames).Count -eq 0 -and @($SubKeyNames).Count -eq 0
}

function Invoke-Stage5CreatedRegistryKeyCleanup {
    param([string[]]$CreatedSubKeys, [scriptblock]$InspectAction,
        [scriptblock]$RemoveAction, [object]$ActionContext = $null)
    Assert-Stage5Condition ($null -ne $InspectAction -and $null -ne $RemoveAction) `
        'Created registry key cleanup requires inspect and remove actions.'
    $errors = New-Object 'Collections.Generic.List[string]'
    for ($index = $CreatedSubKeys.Count - 1; $index -ge 0; --$index) {
        try {
            $state = & $InspectAction $CreatedSubKeys[$index] $ActionContext
            if ($null -eq $state) { continue }
            if (Test-Stage5RegistryLeafRemoval $false @($state.valueNames) @($state.subKeyNames)) {
                & $RemoveAction $CreatedSubKeys[$index] $ActionContext
            }
        }
        catch {
            $errors.Add("'$($CreatedSubKeys[$index])': $($_.Exception.Message)") | Out-Null
        }
    }
    if ($errors.Count -gt 0) {
        throw "Created registry key cleanup failed after attempting every created key: $($errors -join ' | ')"
    }
}

function Get-Stage5FinalAcceptanceFileSha256 {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Resolve-Stage5FinalAcceptanceFile {
    param([string]$BaseDirectory, [string]$RelativePath, [string]$Context)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($RelativePath)) `
        "$Context path is empty."
    Assert-Stage5Condition (-not [IO.Path]::IsPathRooted($RelativePath)) `
        "$Context path must be manifest-relative."
    $base = [IO.Path]::GetFullPath($BaseDirectory)
    $candidate = [IO.Path]::GetFullPath((Join-Path $base $RelativePath))
    Assert-Stage5Condition ($candidate.StartsWith(
        $base + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path escapes its manifest directory."
    Assert-Stage5Condition (Test-Path -LiteralPath $candidate -PathType Leaf) `
        "$Context file was not found: $RelativePath"
    return $candidate
}

function Assert-Stage5FinalAcceptanceSha256 {
    param([string]$Path, [object]$Expected, [string]$Context)
    Assert-Stage5Condition ($Expected -is [string] -and
        $Expected -match '^[0-9A-Fa-f]{64}$') `
        "$Context SHA-256 must contain exactly 64 hexadecimal characters."
    $actual = Get-Stage5FinalAcceptanceFileSha256 $Path
    Assert-Stage5Condition ($actual -ceq $Expected.ToUpperInvariant()) `
        "$Context SHA-256 mismatch. Expected $Expected, got $actual."
    return $actual
}

function Assert-Stage5FinalAcceptanceBoolean {
    param([object]$Value, [string]$Context)
    Assert-Stage5Condition ($Value -is [bool]) "$Context must be a JSON boolean."
    Assert-Stage5Condition ([bool]$Value) "$Context must be true."
}

function Assert-Stage5FinalAcceptanceStringSet {
    param([object]$Value, [string[]]$Expected, [string]$Context)
    Assert-Stage5Condition ($Value -is [Array]) "$Context must be a JSON array."
    $actual = @($Value | ForEach-Object {
        Assert-Stage5Condition ($_ -is [string]) "$Context entries must be JSON strings."
        [string]$_
    })
    Assert-Stage5Condition ($actual.Count -eq $Expected.Count -and
        @($actual | Sort-Object -CaseSensitive -Unique).Count -eq $actual.Count) `
        "$Context must contain each required value exactly once."
    foreach ($required in $Expected) {
        Assert-Stage5Condition ($actual -ccontains $required) `
            "$Context is missing '$required'."
    }
}

function Assert-Stage5FinalAcceptanceDetails {
    param([string]$Kind, [object]$Details, [string]$SourceCommit,
        [Collections.IDictionary]$EvidenceHashes)
    $workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
        'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
    switch ($Kind) {
        'deterministic-runtime' {
            $names = @('gateName', 'isolatedPipelineMode', 'simulationModes',
                'workerConfigurations', 'isolatedMatrixPassed',
                'finalAcceptanceClaim', 'replayEvidenceSha256',
                'freshAiEvidenceSha256', 'performanceEvidenceSha256')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'gateName' "$Kind details") `
                -ceq 'deterministic-runtime') 'The runtime evidence must name the deterministic-runtime gate.'
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'isolatedPipelineMode' "$Kind details") `
                -ceq 'serial') 'The deterministic-runtime matrix must isolate Stage 5 with the serial Stage 4 pipeline.'
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'simulationModes' "$Kind details") `
                @('serial', 'parallel', 'shadow') "$Kind simulationModes"
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerConfigurations' "$Kind details") `
                $workerConfigurations "$Kind workerConfigurations"
            Assert-Stage5FinalAcceptanceBoolean `
                (Get-Stage5JsonValue $Details 'isolatedMatrixPassed' "$Kind details") `
                "$Kind isolatedMatrixPassed"
            $claim = Get-Stage5JsonValue $Details 'finalAcceptanceClaim' "$Kind details"
            Assert-Stage5Condition ($claim -is [bool] -and -not [bool]$claim) `
                'The deterministic-runtime gate must not claim final Stage 5 acceptance.'
            foreach ($binding in @(
                @('replayEvidenceSha256', 'replay-determinism'),
                @('freshAiEvidenceSha256', 'fresh-ai'),
                @('performanceEvidenceSha256', 'performance-scaling')
            )) {
                $value = Get-Stage5JsonValue $Details $binding[0] "$Kind details"
                Assert-Stage5Condition ($value -is [string] -and
                    $value.ToUpperInvariant() -ceq [string]$EvidenceHashes[$binding[1]]) `
                    "$Kind $($binding[0]) does not bind the independently hashed $($binding[1]) evidence."
            }
        }
        'replay-determinism' {
            $names = @('uniqueReplayCount', 'executionCount', 'matrixPasses',
                'stressExecutionsPerConfiguration', 'workerConfigurations',
                'allExecutionsPassed', 'deterministicAcrossWorkers')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            foreach ($metric in @(
                @('uniqueReplayCount', 10), @('executionCount', 168),
                @('matrixPasses', 2), @('stressExecutionsPerConfiguration', 6)
            )) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -ge [Int64]$metric[1]) `
                    "$Kind $($metric[0]) is below the required minimum $($metric[1])."
            }
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerConfigurations' "$Kind details") `
                $workerConfigurations "$Kind workerConfigurations"
            foreach ($name in @('allExecutionsPassed', 'deterministicAcrossWorkers')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        'fresh-ai' {
            $names = @('scenarios', 'distinctSeeds', 'repeats', 'workerConfigurations',
                'freshGames', 'allGamesCompleted', 'deterministicAcrossWorkers')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'scenarios' "$Kind details") `
                @('4v3', '4v2') "$Kind scenarios"
            foreach ($metric in @(@('distinctSeeds', 3), @('repeats', 2))) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -ge [Int64]$metric[1]) `
                    "$Kind $($metric[0]) is below the required minimum $($metric[1])."
            }
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerConfigurations' "$Kind details") `
                $workerConfigurations "$Kind workerConfigurations"
            foreach ($name in @('freshGames', 'allGamesCompleted', 'deterministicAcrossWorkers')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        'performance-scaling' {
            $names = @('physicalCoreCount', 'oneWorkerRegressionRatio',
                'eightWorkerSpeedup', 'sixteenWorkerStatus', 'eightToSixteenSpeedup')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            $cores = Get-Stage5JsonValue $Details 'physicalCoreCount' "$Kind details"
            $regression = Get-Stage5JsonValue $Details 'oneWorkerRegressionRatio' "$Kind details"
            $speedup = Get-Stage5JsonValue $Details 'eightWorkerSpeedup' "$Kind details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $cores) -and [Int64]$cores -ge 8) `
                'Performance evidence requires at least eight physical cores.'
            Assert-Stage5Condition ((Test-Stage5JsonNumber $regression) -and
                [double]$regression -gt 0 -and [double]$regression -le 1.05) `
                'Performance evidence exceeds the 5 percent one-worker regression limit.'
            Assert-Stage5Condition ((Test-Stage5JsonNumber $speedup) -and
                [double]$speedup -ge 2.0) `
                'Performance evidence is below the required 2.0x eight-worker speedup.'
            $sixteenStatus = Get-Stage5JsonValue $Details 'sixteenWorkerStatus' "$Kind details"
            $sixteenSpeedup = Get-Stage5JsonValue $Details 'eightToSixteenSpeedup' "$Kind details"
            if ([Int64]$cores -ge 16) {
                Assert-Stage5Condition ($sixteenStatus -is [string] -and
                    $sixteenStatus -ceq 'passed' -and
                    (Test-Stage5JsonNumber $sixteenSpeedup) -and [double]$sixteenSpeedup -gt 1.0) `
                    'A supported 16-core host requires positive scaling from eight to sixteen workers.'
            }
            else {
                Assert-Stage5Condition ($sixteenStatus -is [string] -and
                    $sixteenStatus -ceq 'unsupported-host-topology' -and $null -eq $sixteenSpeedup) `
                    'A host below 16 physical cores must report sixteen-worker evidence as unsupported-host-topology.'
            }
        }
        'mixed-worker-multiplayer' {
            $names = @('workerCounts', 'matchRecords', 'peerRecords', 'fixedSeeds',
                'topologies', 'provenKernelMask', 'allMatchesCompleted',
                'stateTracesIdentical', 'crossEpochRejected', 'contentMismatchRejected')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerCounts' "$Kind details") `
                @('1', '2', '4', '8', 'auto') "$Kind workerCounts"
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'topologies' "$Kind details") `
                @('two-peer-1-v-16', 'two-peer-2-v-auto', 'two-peer-4-v-8',
                    'four-peer-mixed-workers') "$Kind topologies"
            $seeds = Get-Stage5JsonValue $Details 'fixedSeeds' "$Kind details"
            Assert-Stage5Condition ($seeds -is [Array] -and $seeds.Count -eq 2 -and
                $seeds[0] -eq 23063 -and $seeds[1] -eq 49374) `
                'Mixed-worker multiplayer evidence requires the exact two fixed seeds.'
            foreach ($metric in @(@('matchRecords', 16), @('peerRecords', 40),
                @('provenKernelMask', 63))) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -eq [Int64]$metric[1]) `
                    "Mixed-worker multiplayer $($metric[0]) must equal $($metric[1])."
            }
            foreach ($name in @('allMatchesCompleted', 'stateTracesIdentical',
                'crossEpochRejected', 'contentMismatchRejected')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        'combined-stage4-stage5-installed-runtime' {
            $names = @('installedRuntime', 'pipelineMode', 'simulationMode',
                'workerPolicy', 'renderer', 'renderThread', 'bothTitlesPassed',
                'stage4AndStage5Concurrent', 'visualParityPassed',
                'deviceRecoveryPassed', 'gameplaySoakPassed')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            foreach ($pair in @(
                @('pipelineMode', 'parallel'), @('simulationMode', 'parallel'),
                @('workerPolicy', 'auto'), @('renderer', 'd3d11'),
                @('renderThread', 'dedicated')
            )) {
                Assert-Stage5Condition ((Get-Stage5JsonValue $Details $pair[0] "$Kind details") `
                    -ceq $pair[1]) "$Kind requires $($pair[0])=$($pair[1])."
            }
            foreach ($name in @('installedRuntime', 'bothTitlesPassed',
                'stage4AndStage5Concurrent', 'visualParityPassed',
                'deviceRecoveryPassed', 'gameplaySoakPassed')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        'premium-review' {
            $names = @('reviewedCommit', 'reviewRounds', 'independentReviewers',
                'completeDiffReviewed', 'fixesRetested', 'openP0', 'openP1', 'openP2')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'reviewedCommit' "$Kind details") `
                -ceq $SourceCommit) 'Premium review evidence identifies a different commit.'
            foreach ($metric in @(@('reviewRounds', 1), @('independentReviewers', 3))) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -ge [Int64]$metric[1]) `
                    "$Kind $($metric[0]) is below the required minimum $($metric[1])."
            }
            foreach ($name in @('completeDiffReviewed', 'fixesRetested')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
            foreach ($name in @('openP0', 'openP1', 'openP2')) {
                $value = Get-Stage5JsonValue $Details $name "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and [Int64]$value -eq 0) `
                    "$Kind $name must be zero."
            }
        }
        'manual-acceptance' {
            $names = @('approvalScope', 'approvedByUser', 'candidateHashVerified',
                'bothTitlesTested', 'graphicsPassed', 'audioPassed', 'inputPassed',
                'saveLoadPassed', 'largeMatchPassed', 'cleanExitPassed')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'approvalScope' "$Kind details") `
                -ceq 'final-stage5-installed-runtime') `
                'Manual evidence must cover the final Stage 5 installed runtime.'
            foreach ($name in @('approvedByUser', 'candidateHashVerified',
                'bothTitlesTested', 'graphicsPassed', 'audioPassed', 'inputPassed',
                'saveLoadPassed', 'largeMatchPassed', 'cleanExitPassed')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        default { throw "Unsupported final acceptance evidence kind '$Kind'." }
    }
}

function Read-Stage5Net3LoopbackEvidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedGeneralsExecutableSha256,
        [string]$ExpectedZeroHourExecutableSha256
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Installed NET3 loopback evidence was not found: $full"
    Assert-Stage5Condition ($ExpectedSourceCommit -match '^[0-9a-f]{40}$') `
        'ExpectedSourceCommit must be an independently supplied lowercase 40-hex commit.'
    foreach ($binding in @(
        @('ExpectedArtifactSetSha256', $ExpectedArtifactSetSha256),
        @('ExpectedGeneralsExecutableSha256', $ExpectedGeneralsExecutableSha256),
        @('ExpectedZeroHourExecutableSha256', $ExpectedZeroHourExecutableSha256)
    )) {
        Assert-Stage5Condition ($binding[1] -match '^[0-9A-F]{64}$') `
            "$($binding[0]) must be an independently supplied uppercase SHA-256."
    }
    $document = ConvertFrom-Stage5JsonDictionary $full
    $documentNames = @('schemaVersion', 'evidenceKind', 'status', 'sourceCommit',
        'artifactSetSha256', 'supportedKernelMask', 'executables', 'fixedSeeds', 'matches')
    Assert-Stage5JsonShape $document $documentNames 'Installed NET3 loopback evidence'
    $schemaVersion = Get-Stage5JsonValue $document 'schemaVersion' 'Installed NET3 loopback evidence'
    $kind = Get-Stage5JsonValue $document 'evidenceKind' 'Installed NET3 loopback evidence'
    $status = Get-Stage5JsonValue $document 'status' 'Installed NET3 loopback evidence'
    $sourceCommit = Get-Stage5JsonValue $document 'sourceCommit' 'Installed NET3 loopback evidence'
    $artifactHash = Get-Stage5JsonValue $document 'artifactSetSha256' 'Installed NET3 loopback evidence'
    $kernelMask = Get-Stage5JsonValue $document 'supportedKernelMask' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $kind -is [string] -and $kind -ceq 'installed-net3-loopback' -and
        $status -is [string] -and $status -ceq 'passed') `
        'Installed NET3 loopback evidence identity/status is invalid.'
    Assert-Stage5Condition ($sourceCommit -is [string] -and
        $sourceCommit -ceq $ExpectedSourceCommit) `
        'Installed NET3 loopback evidence source commit does not match independent provenance.'
    Assert-Stage5Condition ($artifactHash -is [string] -and
        $artifactHash -ceq $ExpectedArtifactSetSha256) `
        'Installed NET3 loopback evidence artifact-set SHA-256 does not match independent provenance.'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $kernelMask) -and [UInt64]$kernelMask -eq 0x3F) `
        'Installed NET3 loopback evidence must advertise exactly the integrated kernel mask 0x3F.'

    $executables = Get-Stage5JsonValue $document 'executables' 'Installed NET3 loopback evidence'
    Assert-Stage5JsonShape $executables @('Generals', 'ZeroHour') `
        'Installed NET3 loopback evidence executables'
    $expectedExecutableHashes = @{
        Generals = $ExpectedGeneralsExecutableSha256
        ZeroHour = $ExpectedZeroHourExecutableSha256
    }
    foreach ($title in @('Generals', 'ZeroHour')) {
        $hash = Get-Stage5JsonValue $executables $title `
            'Installed NET3 loopback evidence executables'
        Assert-Stage5Condition ($hash -is [string] -and
            $hash -ceq $expectedExecutableHashes[$title]) `
            "Installed NET3 loopback evidence executable hash for $title does not match independent provenance."
    }

    $fixedSeeds = Get-Stage5JsonValue $document 'fixedSeeds' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ($fixedSeeds -is [Array] -and $fixedSeeds.Count -eq 2 -and
        (Test-Stage5JsonInteger $fixedSeeds[0]) -and (Test-Stage5JsonInteger $fixedSeeds[1]) -and
        [UInt64]$fixedSeeds[0] -eq 23063 -and [UInt64]$fixedSeeds[1] -eq 49374) `
        'Installed NET3 loopback evidence requires the exact fixed nonzero seeds 23063 and 49374 in canonical order.'

    $topologies = @(
        [pscustomobject]@{ id = 'two-peer-1-v-16'; workers = @('1', '16') },
        [pscustomobject]@{ id = 'two-peer-2-v-auto'; workers = @('2', 'auto') },
        [pscustomobject]@{ id = 'two-peer-4-v-8'; workers = @('4', '8') },
        [pscustomobject]@{ id = 'four-peer-mixed-workers'; workers = @('1', '2', '8', 'auto') }
    )
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelBits = @(1, 2, 4, 8, 16, 32)
    # Do not name this variable $matches: PowerShell's case-insensitive
    # automatic $Matches table is rewritten by every later -match expression.
    $matchRecords = Get-Stage5JsonValue $document 'matches' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ($matchRecords -is [Array] -and $matchRecords.Count -eq 16) `
        'Installed NET3 loopback evidence requires exactly 16 match records.'
    $matchIndex = 0
    $peerRecordCount = 0
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($topology in $topologies) {
            foreach ($seed in @(23063, 49374)) {
                $match = $matchRecords[$matchIndex]
                $context = "Installed NET3 loopback match index $matchIndex"
                Assert-Stage5JsonShape $match @('recordId', 'sourceCommit', 'title',
                    'executableSha256', 'artifactSetSha256', 'topologyId', 'seed',
                    'networkHelloReady', 'rosterExact', 'rosterSha256', 'policyMask', 'peers') $context
                $recordId = "$title/$($topology.id)/$seed"
                Assert-Stage5Condition ((Get-Stage5JsonValue $match 'recordId' $context) -ceq $recordId -and
                    (Get-Stage5JsonValue $match 'sourceCommit' $context) -ceq $ExpectedSourceCommit -and
                    (Get-Stage5JsonValue $match 'title' $context) -ceq $title -and
                    (Get-Stage5JsonValue $match 'executableSha256' $context) -ceq
                        $expectedExecutableHashes[$title] -and
                    (Get-Stage5JsonValue $match 'artifactSetSha256' $context) -ceq
                        $ExpectedArtifactSetSha256 -and
                    (Get-Stage5JsonValue $match 'topologyId' $context) -ceq $topology.id -and
                    (Get-Stage5JsonValue $match 'seed' $context) -eq $seed) `
                    "$context provenance/topology identity is not canonical."
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $match 'networkHelloReady' $context) `
                    "$context networkHelloReady"
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $match 'rosterExact' $context) "$context rosterExact"
                $rosterHash = Get-Stage5JsonValue $match 'rosterSha256' $context
                Assert-Stage5Condition ($rosterHash -is [string] -and
                    $rosterHash -match '^[0-9A-F]{64}$') `
                    "$context rosterSha256 is invalid."
                $matchPolicy = Get-Stage5JsonValue $match 'policyMask' $context
                Assert-Stage5Condition ((Test-Stage5JsonInteger $matchPolicy) -and
                    [UInt64]$matchPolicy -eq 0x3F) "$context policyMask must equal 0x3F."
                $peers = Get-Stage5JsonValue $match 'peers' $context
                Assert-Stage5Condition ($peers -is [Array] -and
                    $peers.Count -eq $topology.workers.Count) `
                    "$context does not contain the exact topology peer roster."
                $referenceCRC = $null
                $referenceFrame = $null
                for ($peerIndex = 0; $peerIndex -lt $topology.workers.Count; ++$peerIndex) {
                    $peer = $peers[$peerIndex]
                    $peerContext = "$context peer $peerIndex"
                    Assert-Stage5JsonShape $peer @('ordinal', 'requestedWorkers',
                        'effectiveWorkers', 'networkHelloReady', 'rosterExact', 'rosterSha256',
                        'policyMask', 'finalFrame', 'finalCRC', 'exitCode', 'cleanShutdown', 'kernels') `
                        $peerContext
                    $ordinal = Get-Stage5JsonValue $peer 'ordinal' $peerContext
                    $requestedWorkers = Get-Stage5JsonValue $peer 'requestedWorkers' $peerContext
                    $effectiveWorkers = Get-Stage5JsonValue $peer 'effectiveWorkers' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $ordinal) -and
                        [int]$ordinal -eq $peerIndex -and $requestedWorkers -is [string] -and
                        $requestedWorkers -ceq $topology.workers[$peerIndex] -and
                        (Test-Stage5JsonInteger $effectiveWorkers) -and [int]$effectiveWorkers -ge 1) `
                        "$peerContext worker identity does not match the topology."
                    if ($requestedWorkers -ceq 'auto') {
                        Assert-Stage5Condition ([int]$effectiveWorkers -gt 1) `
                            "$peerContext automatic workers did not expose a multicore lane."
                    }
                    else {
                        Assert-Stage5Condition ([int]$effectiveWorkers -eq [int]$requestedWorkers) `
                            "$peerContext effective worker count differs from the forced count."
                    }
                    foreach ($name in @('networkHelloReady', 'rosterExact', 'cleanShutdown')) {
                        Assert-Stage5FinalAcceptanceBoolean `
                            (Get-Stage5JsonValue $peer $name $peerContext) "$peerContext $name"
                    }
                    Assert-Stage5Condition ((Get-Stage5JsonValue $peer 'rosterSha256' $peerContext) `
                        -ceq $rosterHash) "$peerContext roster SHA-256 differs from the exact match roster."
                    $peerPolicy = Get-Stage5JsonValue $peer 'policyMask' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $peerPolicy) -and
                        [UInt64]$peerPolicy -eq 0x3F) "$peerContext policyMask must equal 0x3F."
                    $exitCode = Get-Stage5JsonValue $peer 'exitCode' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $exitCode) -and
                        [Int64]$exitCode -eq 0) "$peerContext did not exit successfully."
                    $finalFrame = Get-Stage5JsonValue $peer 'finalFrame' $peerContext
                    $finalCRC = Get-Stage5JsonValue $peer 'finalCRC' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $finalFrame) -and
                        [UInt64]$finalFrame -gt 0 -and $finalCRC -is [string] -and
                        $finalCRC -match '^[0-9A-F]{8}$') `
                        "$peerContext final frame/CRC evidence is invalid."
                    if ($null -eq $referenceCRC) {
                        $referenceCRC = $finalCRC
                        $referenceFrame = [UInt64]$finalFrame
                    }
                    else {
                        Assert-Stage5Condition ($finalCRC -ceq $referenceCRC -and
                            [UInt64]$finalFrame -eq $referenceFrame) `
                            "$context peer final CRC/frame values differ."
                    }
                    $kernels = Get-Stage5JsonValue $peer 'kernels' $peerContext
                    Assert-Stage5Condition ($kernels -is [Array] -and $kernels.Count -eq 6) `
                        "$peerContext must contain exactly six kernel records."
                    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
                        $kernel = $kernels[$kernelIndex]
                        $kernelContext = "$peerContext kernel $kernelIndex"
                        Assert-Stage5JsonShape $kernel @('name', 'bit', 'submitted', 'completed',
                            'physicalWorkerJobs', 'distinctPhysicalWorkers') $kernelContext
                        Assert-Stage5Condition ((Get-Stage5JsonValue $kernel 'name' $kernelContext) `
                            -ceq $kernelNames[$kernelIndex] -and
                            (Get-Stage5JsonValue $kernel 'bit' $kernelContext) -eq $kernelBits[$kernelIndex]) `
                            "$kernelContext name/bit is not canonical."
                        $submitted = Get-Stage5JsonValue $kernel 'submitted' $kernelContext
                        $completed = Get-Stage5JsonValue $kernel 'completed' $kernelContext
                        $physical = Get-Stage5JsonValue $kernel 'physicalWorkerJobs' $kernelContext
                        $distinct = Get-Stage5JsonValue $kernel 'distinctPhysicalWorkers' $kernelContext
                        foreach ($counter in @($submitted, $completed, $physical, $distinct)) {
                            Assert-Stage5Condition ((Test-Stage5JsonInteger $counter) -and
                                [Int64]$counter -ge 0) "$kernelContext counters must be nonnegative integers."
                        }
                        Assert-Stage5Condition ([UInt64]$submitted -eq [UInt64]$completed) `
                            "$kernelContext submitted/completed jobs differ."
                        if ([int]$effectiveWorkers -eq 1) {
                            Assert-Stage5Condition ([UInt64]$submitted -eq 0 -and
                                [UInt64]$physical -eq 0 -and [UInt64]$distinct -eq 0) `
                                "$kernelContext forced-one evidence must report zero physical work."
                        }
                        else {
                            Assert-Stage5Condition ([UInt64]$submitted -gt 0 -and
                                [UInt64]$physical -gt 0 -and [UInt64]$physical -le [UInt64]$completed -and
                                [UInt64]$distinct -gt 1 -and [UInt64]$distinct -le [UInt64]$effectiveWorkers) `
                                "$kernelContext does not prove positive work on more than one physical worker."
                        }
                    }
                    ++$peerRecordCount
                }
                ++$matchIndex
            }
        }
    }
    Assert-Stage5Condition ($matchIndex -eq 16 -and $peerRecordCount -eq 40) `
        'Installed NET3 loopback evidence must contain exactly 16 matches and 40 nested peer records (20 peers per title).'
    return [pscustomobject]@{
        schemaVersion = 1
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        evidenceManifestSha256 = Get-Stage5FinalAcceptanceFileSha256 $full
        generalsExecutableSha256 = $ExpectedGeneralsExecutableSha256
        zeroHourExecutableSha256 = $ExpectedZeroHourExecutableSha256
        provenKernelMask = [UInt64]0x3F
        matchCount = 16
        peerRecordCount = 40
    }
}

function Read-Stage5PerformanceScalingEvidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedStage3BaselineSha256
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Stage 5 scaling evidence was not found: $full"
    Assert-Stage5Condition ($ExpectedSourceCommit -match '^[0-9a-f]{40}$') `
        'ExpectedSourceCommit must be an independently supplied lowercase 40-hex commit.'
    foreach ($binding in @(
        @('ExpectedArtifactSetSha256', $ExpectedArtifactSetSha256),
        @('ExpectedExecutableSha256', $ExpectedExecutableSha256),
        @('ExpectedStage3BaselineSha256', $ExpectedStage3BaselineSha256)
    )) {
        Assert-Stage5Condition ($binding[1] -match '^[0-9A-F]{64}$') `
            "$($binding[0]) must be an independently supplied uppercase SHA-256."
    }
    $document = ConvertFrom-Stage5JsonDictionary $full
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind', 'status',
        'sourceCommit', 'artifactSetSha256', 'title', 'executableSha256',
        'stage3BaselineSha256', 'topology',
        'measurementMode', 'installedRuntime', 'selectedLanes', 'oneWorkerPhases',
        'amdahl', 'kernelTimings', 'fixtures') `
        'Stage 5 scaling evidence'
    Assert-Stage5Condition ((Get-Stage5JsonValue $document 'schemaVersion' 'Stage 5 scaling evidence') -eq 1 -and
        (Get-Stage5JsonValue $document 'evidenceKind' 'Stage 5 scaling evidence') -ceq
            'stage5-performance-scaling' -and
        (Get-Stage5JsonValue $document 'status' 'Stage 5 scaling evidence') -ceq 'passed' -and
        (Get-Stage5JsonValue $document 'sourceCommit' 'Stage 5 scaling evidence') -ceq
            $ExpectedSourceCommit -and
        (Get-Stage5JsonValue $document 'artifactSetSha256' 'Stage 5 scaling evidence') -ceq
            $ExpectedArtifactSetSha256 -and
        (Get-Stage5JsonValue $document 'title' 'Stage 5 scaling evidence') -ceq 'ZeroHour' -and
        (Get-Stage5JsonValue $document 'executableSha256' 'Stage 5 scaling evidence') -ceq
            $ExpectedExecutableSha256 -and
        (Get-Stage5JsonValue $document 'stage3BaselineSha256' 'Stage 5 scaling evidence') -ceq
            $ExpectedStage3BaselineSha256 -and
        (Get-Stage5JsonValue $document 'measurementMode' 'Stage 5 scaling evidence') -ceq
            'headless-throughput' -and
        (Get-Stage5JsonValue $document 'installedRuntime' 'Stage 5 scaling evidence') -is [bool] -and
        (Get-Stage5JsonValue $document 'installedRuntime' 'Stage 5 scaling evidence')) `
        'Stage 5 scaling evidence provenance is invalid.'

    $topology = Get-Stage5JsonValue $document 'topology' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $topology @('source', 'topologySha256', 'physicalCoreCount',
        'logicalProcessorCount') 'Stage 5 scaling topology'
    $physicalCores = Get-Stage5JsonValue $topology 'physicalCoreCount' 'Stage 5 scaling topology'
    $logicalProcessors = Get-Stage5JsonValue $topology 'logicalProcessorCount' 'Stage 5 scaling topology'
    Assert-Stage5Condition ((Get-Stage5JsonValue $topology 'source' 'Stage 5 scaling topology') -ceq
        'GetSystemCpuSetInformation' -and
        (Get-Stage5JsonValue $topology 'topologySha256' 'Stage 5 scaling topology') -match
            '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $physicalCores) -and [int]$physicalCores -ge 16 -and
        (Test-Stage5JsonInteger $logicalProcessors) -and
        [int]$logicalProcessors -ge [int]$physicalCores) `
        'Stage 5 scaling evidence requires a hashed Windows CPU-set topology with at least 16 physical cores.'

    $lanes = Get-Stage5JsonValue $document 'selectedLanes' 'Stage 5 scaling evidence'
    Assert-Stage5Condition ($lanes -is [Array] -and $lanes.Count -eq 3) `
        'Stage 5 scaling evidence requires exactly forced-one, physical-8, and physical-16 lanes.'
    $laneNames = @('forced-one', 'physical-8', 'physical-16')
    $laneWorkers = @(1, 8, 16)
    for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
        $lane = $lanes[$laneIndex]
        $context = "Stage 5 scaling lane $laneIndex"
        Assert-Stage5JsonShape $lane @('name', 'requestedWorkers', 'selectedLogicalProcessors',
            'selectedDistinctPhysicalCores', 'selectedPhysicalCoreMask') $context
        $logical = Get-Stage5JsonValue $lane 'selectedLogicalProcessors' $context
        $distinct = Get-Stage5JsonValue $lane 'selectedDistinctPhysicalCores' $context
        $mask = Get-Stage5JsonValue $lane 'selectedPhysicalCoreMask' $context
        Assert-Stage5Condition ((Get-Stage5JsonValue $lane 'name' $context) -ceq
            $laneNames[$laneIndex] -and
            (Get-Stage5JsonValue $lane 'requestedWorkers' $context) -eq $laneWorkers[$laneIndex] -and
            (Test-Stage5JsonInteger $logical) -and [int]$logical -eq $laneWorkers[$laneIndex] -and
            (Test-Stage5JsonInteger $distinct) -and [int]$distinct -eq $laneWorkers[$laneIndex] -and
            $mask -is [string] -and $mask -match '^[0-9A-F]{16}$') `
            "$context does not prove the exact selected logical and distinct physical-core count."
        $maskValue = [Convert]::ToUInt64($mask, 16)
        $maskBits = 0
        while ($maskValue -ne 0) { $maskBits += [int]($maskValue -band 1); $maskValue = $maskValue -shr 1 }
        Assert-Stage5Condition ($maskBits -eq $laneWorkers[$laneIndex]) `
            "$context physical-core mask does not contain the exact selected core count."
    }

    $phases = Get-Stage5JsonValue $document 'oneWorkerPhases' 'Stage 5 scaling evidence'
    $phaseNames = @('owner-intake', 'world-queries', 'pathfinding', 'object-computation',
        'spatial-work', 'deterministic-commit', 'verification-publication')
    Assert-Stage5Condition ($phases -is [Array] -and $phases.Count -eq $phaseNames.Count) `
        'Stage 5 scaling evidence requires exact one-worker timing for all simulation phases.'
    [double]$totalOne = 0.0
    [double]$totalSerial = 0.0
    for ($phaseIndex = 0; $phaseIndex -lt $phaseNames.Count; ++$phaseIndex) {
        $phase = $phases[$phaseIndex]
        $context = "Stage 5 one-worker phase $phaseIndex"
        Assert-Stage5JsonShape $phase @('name', 'elapsedMilliseconds', 'serialMilliseconds') $context
        $elapsed = Get-Stage5JsonValue $phase 'elapsedMilliseconds' $context
        $serial = Get-Stage5JsonValue $phase 'serialMilliseconds' $context
        Assert-Stage5Condition ((Get-Stage5JsonValue $phase 'name' $context) -ceq
            $phaseNames[$phaseIndex] -and (Test-Stage5JsonNumber $elapsed) -and
            [double]$elapsed -gt 0.0 -and (Test-Stage5JsonNumber $serial) -and
            [double]$serial -ge 0.0 -and [double]$serial -le [double]$elapsed) `
            "$context timing is invalid."
        $totalOne += [double]$elapsed
        $totalSerial += [double]$serial
    }
    $amdahl = Get-Stage5JsonValue $document 'amdahl' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $amdahl @('totalOneWorkerMilliseconds', 'totalSerialMilliseconds',
        'serialFraction', 'maximumSpeedup', 'reachesTwoX') 'Stage 5 Amdahl evidence'
    $reportedOne = Get-Stage5JsonValue $amdahl 'totalOneWorkerMilliseconds' 'Stage 5 Amdahl evidence'
    $reportedSerial = Get-Stage5JsonValue $amdahl 'totalSerialMilliseconds' 'Stage 5 Amdahl evidence'
    $fraction = Get-Stage5JsonValue $amdahl 'serialFraction' 'Stage 5 Amdahl evidence'
    $maximum = Get-Stage5JsonValue $amdahl 'maximumSpeedup' 'Stage 5 Amdahl evidence'
    $computedFraction = $totalSerial / $totalOne
    $computedMaximum = if ($computedFraction -eq 0.0) { [double]::PositiveInfinity } else { 1.0 / $computedFraction }
    Assert-Stage5Condition ((Test-Stage5JsonNumber $reportedOne) -and
        [Math]::Abs([double]$reportedOne - $totalOne) -le 0.0001 -and
        (Test-Stage5JsonNumber $reportedSerial) -and
        [Math]::Abs([double]$reportedSerial - $totalSerial) -le 0.0001 -and
        (Test-Stage5JsonNumber $fraction) -and
        [Math]::Abs([double]$fraction - $computedFraction) -le 0.000001 -and
        (Test-Stage5JsonNumber $maximum) -and
        [Math]::Abs([double]$maximum - $computedMaximum) -le 0.0001 -and
        (Get-Stage5JsonValue $amdahl 'reachesTwoX' 'Stage 5 Amdahl evidence') -is [bool] -and
        (Get-Stage5JsonValue $amdahl 'reachesTwoX' 'Stage 5 Amdahl evidence') -and
        $computedMaximum -ge 2.0) `
        'Stage 5 Amdahl evidence does not prove that the measured workload can reach 2x.'

    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelTimings = Get-Stage5JsonValue $document 'kernelTimings' 'Stage 5 scaling evidence'
    Assert-Stage5Condition ($kernelTimings -is [Array] -and $kernelTimings.Count -eq 6) `
        'Stage 5 scaling evidence requires timing for exactly six integrated kernels.'
    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
        $kernel = $kernelTimings[$kernelIndex]
        $context = "Stage 5 scaling kernel timing $kernelIndex"
        Assert-Stage5JsonShape $kernel @('name', 'admittedSlices', 'captureMilliseconds',
            'scheduleMilliseconds', 'waitMilliseconds', 'validateMilliseconds',
            'commitMilliseconds', 'totalParallelMilliseconds', 'exactSerialOperationMilliseconds',
            'netSpeedup') $context
        $admitted = Get-Stage5JsonValue $kernel 'admittedSlices' $context
        $parts = @('captureMilliseconds', 'scheduleMilliseconds', 'waitMilliseconds',
            'validateMilliseconds', 'commitMilliseconds')
        [double]$parallelTotal = 0.0
        foreach ($part in $parts) {
            $value = Get-Stage5JsonValue $kernel $part $context
            Assert-Stage5Condition ((Test-Stage5JsonNumber $value) -and [double]$value -ge 0.0) `
                "$context $part is invalid."
            $parallelTotal += [double]$value
        }
        $reportedParallel = Get-Stage5JsonValue $kernel 'totalParallelMilliseconds' $context
        $serialOperation = Get-Stage5JsonValue $kernel 'exactSerialOperationMilliseconds' $context
        $netSpeedup = Get-Stage5JsonValue $kernel 'netSpeedup' $context
        Assert-Stage5Condition ((Get-Stage5JsonValue $kernel 'name' $context) -ceq
            $kernelNames[$kernelIndex] -and (Test-Stage5JsonInteger $admitted) -and
            [int]$admitted -gt 0 -and (Test-Stage5JsonNumber $reportedParallel) -and
            [Math]::Abs([double]$reportedParallel - $parallelTotal) -le 0.0001 -and
            $parallelTotal -gt 0.0 -and (Test-Stage5JsonNumber $serialOperation) -and
            [double]$serialOperation -gt $parallelTotal -and (Test-Stage5JsonNumber $netSpeedup) -and
            [Math]::Abs([double]$netSpeedup - ([double]$serialOperation / $parallelTotal)) -le 0.0001 -and
            [double]$netSpeedup -gt 1.0) `
            "$context does not prove positive net speedup over the exact serial operation."
    }

    $fixtures = Get-Stage5JsonValue $document 'fixtures' 'Stage 5 scaling evidence'
    $fixtureNames = @('one-thousand-units', 'four-thousand-units', 'eight-thousand-units',
        'dense-eight-player')
    $minimumUnits = @(1000, 4000, 8000, 8000)
    Assert-Stage5Condition ($fixtures -is [Array] -and $fixtures.Count -eq 4) `
        'Stage 5 scaling evidence requires exact 1k, 4k, 8k, and dense eight-player fixtures.'
    foreach ($fixtureIndex in 0..3) {
        $fixture = $fixtures[$fixtureIndex]
        $context = "Stage 5 scaling fixture $fixtureIndex"
        Assert-Stage5JsonShape $fixture @('name', 'playerCount', 'peakUnitCount', 'repeats',
            'stage3OneWorkerMilliseconds', 'stage5OneWorkerMilliseconds',
            'eightPhysicalCoreMilliseconds', 'sixteenPhysicalCoreMilliseconds',
            'oneWorkerRegressionRatio', 'eightPhysicalCoreSpeedup', 'eightToSixteenSpeedup') $context
        $players = Get-Stage5JsonValue $fixture 'playerCount' $context
        $units = Get-Stage5JsonValue $fixture 'peakUnitCount' $context
        $repeats = Get-Stage5JsonValue $fixture 'repeats' $context
        $stage3 = Get-Stage5JsonValue $fixture 'stage3OneWorkerMilliseconds' $context
        $stage5 = Get-Stage5JsonValue $fixture 'stage5OneWorkerMilliseconds' $context
        $eight = Get-Stage5JsonValue $fixture 'eightPhysicalCoreMilliseconds' $context
        $sixteen = Get-Stage5JsonValue $fixture 'sixteenPhysicalCoreMilliseconds' $context
        foreach ($timing in @($stage3, $stage5, $eight, $sixteen)) {
            Assert-Stage5Condition ((Test-Stage5JsonNumber $timing) -and [double]$timing -gt 0.0) `
                "$context contains an invalid measured timing."
        }
        $regression = [double]$stage5 / [double]$stage3
        $speedup8 = [double]$stage5 / [double]$eight
        $scale16 = [double]$eight / [double]$sixteen
        $reportedRegression = Get-Stage5JsonValue $fixture 'oneWorkerRegressionRatio' $context
        $reportedSpeedup8 = Get-Stage5JsonValue $fixture 'eightPhysicalCoreSpeedup' $context
        $reportedScale16 = Get-Stage5JsonValue $fixture 'eightToSixteenSpeedup' $context
        Assert-Stage5Condition ((Get-Stage5JsonValue $fixture 'name' $context) -ceq
            $fixtureNames[$fixtureIndex] -and (Test-Stage5JsonInteger $players) -and
            [int]$players -eq 8 -and (Test-Stage5JsonInteger $units) -and
            [int]$units -ge $minimumUnits[$fixtureIndex] -and
            (Test-Stage5JsonInteger $repeats) -and [int]$repeats -ge 3 -and
            (Test-Stage5JsonNumber $reportedRegression) -and
            [Math]::Abs([double]$reportedRegression - $regression) -le 0.0001 -and
            $regression -le 1.05 -and (Test-Stage5JsonNumber $reportedSpeedup8) -and
            [Math]::Abs([double]$reportedSpeedup8 - $speedup8) -le 0.0001 -and
            $speedup8 -ge 2.0 -and (Test-Stage5JsonNumber $reportedScale16) -and
            [Math]::Abs([double]$reportedScale16 - $scale16) -le 0.0001 -and $scale16 -gt 1.0) `
            "$context does not meet the physical-core scaling and regression gates."
    }
    return [pscustomobject]@{
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        evidenceManifestSha256 = Get-Stage5FinalAcceptanceFileSha256 $full
        executableSha256 = $ExpectedExecutableSha256
        physicalCoreCount = [int]$physicalCores
        fixtureCount = 4
        kernelCount = 6
        eightPhysicalCoreSpeedupFloor = 2.0
    }
}

function Invoke-Stage5FinalAcceptanceAggregation {
    param([string]$AcceptanceManifestPath)
    $requestPath = [IO.Path]::GetFullPath($AcceptanceManifestPath)
    Assert-Stage5Condition (Test-Path -LiteralPath $requestPath -PathType Leaf) `
        "Final acceptance manifest was not found: $requestPath"
    $requestDirectory = Split-Path -Parent $requestPath
    $request = ConvertFrom-Stage5JsonDictionary $requestPath
    $requestNames = @('schemaVersion', 'gateName', 'sourceCommit', 'artifactSet', 'evidence')
    Assert-Stage5JsonShape $request $requestNames 'Final acceptance manifest'
    $schemaVersion = Get-Stage5JsonValue $request 'schemaVersion' 'Final acceptance manifest'
    $gateName = Get-Stage5JsonValue $request 'gateName' 'Final acceptance manifest'
    $sourceCommit = Get-Stage5JsonValue $request 'sourceCommit' 'Final acceptance manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $gateName -is [string] -and $gateName -ceq 'final-stage5-acceptance' -and
        $sourceCommit -is [string] -and $sourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'Final acceptance manifest identity is invalid.'
    $sourceCommit = $sourceCommit.ToLowerInvariant()

    $artifactEntry = Get-Stage5JsonValue $request 'artifactSet' 'Final acceptance manifest'
    Assert-Stage5JsonShape $artifactEntry @('path', 'sha256') 'Final acceptance artifactSet'
    $artifactRelative = Get-Stage5JsonValue $artifactEntry 'path' 'Final acceptance artifactSet'
    $artifactExpectedHash = Get-Stage5JsonValue $artifactEntry 'sha256' 'Final acceptance artifactSet'
    Assert-Stage5Condition ($artifactRelative -is [string]) 'Final acceptance artifactSet path must be a JSON string.'
    $artifactPath = Resolve-Stage5FinalAcceptanceFile $requestDirectory $artifactRelative `
        'Final acceptance artifactSet'
    $artifactSetHash = Assert-Stage5FinalAcceptanceSha256 $artifactPath $artifactExpectedHash `
        'Final acceptance artifactSet'
    $artifactDirectory = Split-Path -Parent $artifactPath
    $artifactSet = ConvertFrom-Stage5JsonDictionary $artifactPath
    $artifactNames = @('schemaVersion', 'sourceCommit', 'productSet', 'architecture', 'artifacts')
    Assert-Stage5JsonShape $artifactSet $artifactNames 'Artifact set manifest'
    Assert-Stage5Condition ((Get-Stage5JsonValue $artifactSet 'schemaVersion' 'Artifact set manifest') -eq 1 -and
        (Get-Stage5JsonValue $artifactSet 'sourceCommit' 'Artifact set manifest') -ceq $sourceCommit -and
        (Get-Stage5JsonValue $artifactSet 'architecture' 'Artifact set manifest') -ceq 'x64') `
        'Artifact set manifest identity does not match the final acceptance request.'
    Assert-Stage5FinalAcceptanceStringSet `
        (Get-Stage5JsonValue $artifactSet 'productSet' 'Artifact set manifest') `
        @('Generals', 'ZeroHour') 'Artifact set productSet'
    $requiredArtifactRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifacts = Get-Stage5JsonValue $artifactSet 'artifacts' 'Artifact set manifest'
    Assert-Stage5Condition ($artifacts -is [Array]) 'Artifact set artifacts must be a JSON array.'
    $artifactRoles = New-Object 'Collections.Generic.List[string]'
    $artifactPaths = New-Object 'Collections.Generic.List[string]'
    $artifactHashes = @{}
    foreach ($artifact in $artifacts) {
        Assert-Stage5JsonShape $artifact @('role', 'path', 'sha256') 'Artifact set entry'
        $role = Get-Stage5JsonValue $artifact 'role' 'Artifact set entry'
        $relative = Get-Stage5JsonValue $artifact 'path' 'Artifact set entry'
        $expectedHash = Get-Stage5JsonValue $artifact 'sha256' 'Artifact set entry'
        Assert-Stage5Condition ($role -is [string] -and $relative -is [string]) `
            'Artifact set role and path must be JSON strings.'
        Assert-Stage5Condition (-not ($artifactRoles -contains $role)) `
            "Artifact set repeats role '$role'."
        $artifactFile = Resolve-Stage5FinalAcceptanceFile $artifactDirectory $relative `
            "Artifact set role '$role'"
        Assert-Stage5Condition (-not ($artifactPaths -contains $artifactFile.ToLowerInvariant())) `
            "Artifact set aliases path '$relative'."
        $verifiedArtifactHash = Assert-Stage5FinalAcceptanceSha256 $artifactFile $expectedHash `
            "Artifact set role '$role'"
        $artifactRoles.Add($role) | Out-Null
        $artifactPaths.Add($artifactFile.ToLowerInvariant()) | Out-Null
        $artifactHashes[$role] = $verifiedArtifactHash
    }
    Assert-Stage5FinalAcceptanceStringSet $artifactRoles.ToArray() $requiredArtifactRoles `
        'Artifact set roles'

    $requiredEvidenceKinds = @('deterministic-runtime', 'replay-determinism',
        'fresh-ai', 'performance-scaling', 'mixed-worker-multiplayer',
        'combined-stage4-stage5-installed-runtime', 'premium-review', 'manual-acceptance')
    $evidenceEntries = Get-Stage5JsonValue $request 'evidence' 'Final acceptance manifest'
    Assert-Stage5Condition ($evidenceEntries -is [Array]) `
        'Final acceptance evidence must be a JSON array.'
    $evidenceByKind = @{}
    $evidenceHashes = @{}
    $evidencePaths = New-Object 'Collections.Generic.List[string]'
    foreach ($entry in $evidenceEntries) {
        Assert-Stage5JsonShape $entry @('kind', 'path', 'sha256') 'Final acceptance evidence entry'
        $kind = Get-Stage5JsonValue $entry 'kind' 'Final acceptance evidence entry'
        $relative = Get-Stage5JsonValue $entry 'path' 'Final acceptance evidence entry'
        $expectedHash = Get-Stage5JsonValue $entry 'sha256' 'Final acceptance evidence entry'
        Assert-Stage5Condition ($kind -is [string] -and $relative -is [string]) `
            'Final acceptance evidence kind and path must be JSON strings.'
        Assert-Stage5Condition ($requiredEvidenceKinds -ccontains $kind) `
            "Final acceptance evidence kind '$kind' is unsupported."
        Assert-Stage5Condition (-not $evidenceByKind.ContainsKey($kind)) `
            "Final acceptance evidence repeats kind '$kind'."
        $evidencePath = Resolve-Stage5FinalAcceptanceFile $requestDirectory $relative `
            "Final acceptance evidence '$kind'"
        Assert-Stage5Condition (-not ($evidencePaths -contains $evidencePath.ToLowerInvariant())) `
            "Final acceptance evidence aliases path '$relative'."
        $evidenceHash = Assert-Stage5FinalAcceptanceSha256 $evidencePath $expectedHash `
            "Final acceptance evidence '$kind'"
        $evidenceDocument = ConvertFrom-Stage5JsonDictionary $evidencePath
        $evidenceNames = @('schemaVersion', 'evidenceKind', 'status', 'sourceCommit',
            'title', 'architecture', 'artifactSetSha256', 'recordedUtc',
            'attachments', 'details')
        Assert-Stage5JsonShape $evidenceDocument $evidenceNames "Evidence '$kind'"
        Assert-Stage5Condition ((Get-Stage5JsonValue $evidenceDocument 'schemaVersion' "Evidence '$kind'") -eq 1 -and
            (Get-Stage5JsonValue $evidenceDocument 'evidenceKind' "Evidence '$kind'") -ceq $kind -and
            (Get-Stage5JsonValue $evidenceDocument 'status' "Evidence '$kind'") -ceq 'passed' -and
            (Get-Stage5JsonValue $evidenceDocument 'sourceCommit' "Evidence '$kind'") -ceq $sourceCommit -and
            (Get-Stage5JsonValue $evidenceDocument 'architecture' "Evidence '$kind'") -ceq 'x64' -and
            (Get-Stage5JsonValue $evidenceDocument 'artifactSetSha256' "Evidence '$kind'") -ceq $artifactSetHash) `
            "Evidence '$kind' does not identify the same passed x64 commit and artifact set."
        $title = Get-Stage5JsonValue $evidenceDocument 'title' "Evidence '$kind'"
        Assert-Stage5Condition ($title -is [string] -and
            @('Generals', 'ZeroHour', 'Both') -ccontains $title) `
            "Evidence '$kind' has an invalid title scope."
        if ($kind -ceq 'combined-stage4-stage5-installed-runtime' -or
            $kind -ceq 'manual-acceptance') {
            Assert-Stage5Condition ($title -ceq 'Both') `
                "Evidence '$kind' must cover both titles."
        }
        $recordedUtc = Get-Stage5JsonValue $evidenceDocument 'recordedUtc' "Evidence '$kind'"
        [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ($recordedUtc -is [string] -and
            [DateTimeOffset]::TryParse($recordedUtc, [ref]$recorded)) `
            "Evidence '$kind' recordedUtc is not a valid timestamp."
        $evidenceByKind[$kind] = [pscustomobject]@{
            relativePath = $relative
            fullPath = $evidencePath
            sha256 = $evidenceHash
            document = $evidenceDocument
        }
        $evidenceHashes[$kind] = $evidenceHash
        $evidencePaths.Add($evidencePath.ToLowerInvariant()) | Out-Null
    }
    Assert-Stage5FinalAcceptanceStringSet @($evidenceByKind.Keys) $requiredEvidenceKinds `
        'Final acceptance evidence kinds'

    $attachmentRoles = @{
        'deterministic-runtime' = @('validation-plan', 'validation-results', 'performance-report')
        'replay-determinism' = @('replay-results', 'replay-fixture-manifest')
        'fresh-ai' = @('ai-results')
        'performance-scaling' = @('performance-report', 'stage3-baseline')
        'mixed-worker-multiplayer' = @('multiplayer-results')
        'combined-stage4-stage5-installed-runtime' = @('combined-results')
        'premium-review' = @('premium-review-results')
        'manual-acceptance' = @('manual-checklist')
    }
    $reportEvidence = New-Object 'Collections.Generic.List[object]'
    foreach ($kind in $requiredEvidenceKinds) {
        $record = $evidenceByKind[$kind]
        $document = $record.document
        $attachments = Get-Stage5JsonValue $document 'attachments' "Evidence '$kind'"
        Assert-Stage5Condition ($attachments -is [Array]) `
            "Evidence '$kind' attachments must be a JSON array."
        $seenRoles = New-Object 'Collections.Generic.List[string]'
        $seenPaths = New-Object 'Collections.Generic.List[string]'
        $attachmentReport = New-Object 'Collections.Generic.List[object]'
        $scalingAttachmentPath = $null
        $scalingAttachmentHash = $null
        $stage3BaselineAttachmentHash = $null
        $evidenceDirectory = Split-Path -Parent $record.fullPath
        foreach ($attachment in $attachments) {
            Assert-Stage5JsonShape $attachment @('role', 'path', 'sha256') `
                "Evidence '$kind' attachment"
            $role = Get-Stage5JsonValue $attachment 'role' "Evidence '$kind' attachment"
            $relative = Get-Stage5JsonValue $attachment 'path' "Evidence '$kind' attachment"
            $expectedHash = Get-Stage5JsonValue $attachment 'sha256' "Evidence '$kind' attachment"
            Assert-Stage5Condition ($role -is [string] -and $relative -is [string]) `
                "Evidence '$kind' attachment role and path must be JSON strings."
            Assert-Stage5Condition (-not ($seenRoles -contains $role)) `
                "Evidence '$kind' repeats attachment role '$role'."
            $attachmentPath = Resolve-Stage5FinalAcceptanceFile $evidenceDirectory $relative `
                "Evidence '$kind' attachment '$role'"
            Assert-Stage5Condition (-not ($seenPaths -contains $attachmentPath.ToLowerInvariant())) `
                "Evidence '$kind' aliases attachment path '$relative'."
            $attachmentHash = Assert-Stage5FinalAcceptanceSha256 $attachmentPath $expectedHash `
                "Evidence '$kind' attachment '$role'"
            if ($kind -ceq 'mixed-worker-multiplayer' -and $role -ceq 'multiplayer-results') {
                $net3Proof = Read-Stage5Net3LoopbackEvidence -Path $attachmentPath `
                    -ExpectedSourceCommit $sourceCommit `
                    -ExpectedArtifactSetSha256 $artifactSetHash `
                    -ExpectedGeneralsExecutableSha256 $artifactHashes['generals-executable'] `
                    -ExpectedZeroHourExecutableSha256 $artifactHashes['zerohour-executable']
                Assert-Stage5Condition ($net3Proof.evidenceManifestSha256 -ceq $attachmentHash -and
                    $net3Proof.provenKernelMask -eq 0x3F -and
                    $net3Proof.matchCount -eq 16 -and $net3Proof.peerRecordCount -eq 40) `
                    'Mixed-worker multiplayer attachment did not produce the exact canonical NET3 proof.'
            }
            if ($kind -ceq 'performance-scaling' -and $role -ceq 'performance-report') {
                $scalingAttachmentPath = $attachmentPath
                $scalingAttachmentHash = $attachmentHash
            }
            if ($kind -ceq 'performance-scaling' -and $role -ceq 'stage3-baseline') {
                $stage3BaselineAttachmentHash = $attachmentHash
            }
            $seenRoles.Add($role) | Out-Null
            $seenPaths.Add($attachmentPath.ToLowerInvariant()) | Out-Null
            $attachmentReport.Add([pscustomobject]@{
                role = $role; path = $relative; sha256 = $attachmentHash
            }) | Out-Null
        }
        Assert-Stage5FinalAcceptanceStringSet $seenRoles.ToArray() $attachmentRoles[$kind] `
            "Evidence '$kind' attachment roles"
        if ($kind -ceq 'performance-scaling') {
            Assert-Stage5Condition ($null -ne $scalingAttachmentPath -and
                $null -ne $stage3BaselineAttachmentHash) `
                'Performance evidence is missing its report or Stage 3 baseline binding.'
            $scalingProof = Read-Stage5PerformanceScalingEvidence -Path $scalingAttachmentPath `
                -ExpectedSourceCommit $sourceCommit `
                -ExpectedArtifactSetSha256 $artifactSetHash `
                -ExpectedExecutableSha256 $artifactHashes['zerohour-executable'] `
                -ExpectedStage3BaselineSha256 $stage3BaselineAttachmentHash
            Assert-Stage5Condition ($scalingProof.evidenceManifestSha256 -ceq $scalingAttachmentHash -and
                $scalingProof.fixtureCount -eq 4 -and $scalingProof.kernelCount -eq 6 -and
                $scalingProof.eightPhysicalCoreSpeedupFloor -ge 2.0) `
                'Performance attachment did not produce the exact physical-core scaling proof.'
        }
        Assert-Stage5FinalAcceptanceDetails $kind `
            (Get-Stage5JsonValue $document 'details' "Evidence '$kind'") `
            $sourceCommit $evidenceHashes
        $reportEvidence.Add([pscustomobject]@{
            kind = $kind
            path = $record.relativePath
            sha256 = $record.sha256
            recordedUtc = Get-Stage5JsonValue $document 'recordedUtc' "Evidence '$kind'"
            attachments = $attachmentReport.ToArray()
        }) | Out-Null
    }
    return [pscustomobject]@{
        schemaVersion = 1
        gateName = 'final-stage5-acceptance'
        status = 'passed'
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        sourceCommit = $sourceCommit
        artifactSet = [pscustomobject]@{
            path = [string]$artifactRelative
            sha256 = $artifactSetHash
        }
        evidence = $reportEvidence.ToArray()
    }
}

Export-ModuleMember -Function ConvertFrom-Stage5AiCompletion, ConvertFrom-Stage5ReplayMetrics, `
    ConvertFrom-Stage5ReplayResult, Get-Stage5TimingEvidence, Assert-Stage5AiDeterminism, Assert-Stage5ReplayDeterminism, `
    Assert-Stage5AuthoritativeWorkEvidence, Assert-Stage5CollisionTimingEvidence, `
    Read-Stage5PerformanceBaseline, Measure-Stage5Performance, Invoke-Stage5RegistryRestore, `
    Invoke-Stage5RegistrySetupTransaction, `
    Test-Stage5RegistryLeafRemoval, Invoke-Stage5CreatedRegistryKeyCleanup, `
    Invoke-Stage5FinalAcceptanceAggregation, Read-Stage5Net3LoopbackEvidence, `
    Read-Stage5PerformanceScalingEvidence
