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
		'ordinary_path_eligible', 'ordinary_path_submitted_requests',
		'ordinary_path_submitted_ranges',
		'ordinary_path_worker_executed_requests',
		'ordinary_path_worker_executed_range_jobs',
		'ordinary_path_owner_helped_range_jobs',
		'ordinary_path_failed_range_jobs',
		'ordinary_path_physical_worker_mask',
		'ordinary_path_distinct_physical_workers',
		'ordinary_path_authoritative_commits',
		'ordinary_path_authoritative_multiworker_commits',
		'ordinary_path_stale_rejections',
		'ordinary_path_validation_failures',
		'ordinary_path_serial_fallbacks',
		'ordinary_path_shadow_comparisons',
		'ordinary_path_shadow_mismatches',
		'ordinary_path_timeouts', 'ordinary_path_late_drains',
		'ordinary_path_peak_active_workers',
		'ordinary_path_max_batch_requests',
		'ordinary_path_max_range_count',
		'ordinary_path_max_grain_size',
        'collision_authoritative_commits', 'collision_shadow_executions',
        'collision_shadow_compared_candidates',
        'collision_shadow_mismatches', 'collision_owner_fallbacks',
        'collision_unexpected_fallbacks', 'collision_ineligible_slices',
        'collision_stale_rejections', 'collision_committed_candidates',
        'collision_prepared_pairs', 'collision_unique_candidates',
        'collision_submitted_jobs', 'collision_completed_jobs',
        'collision_physical_worker_jobs', 'collision_owner_helped_jobs',
        'collision_physical_worker_mask',
        'collision_distinct_physical_workers',
		'collision_physical_worker_mask_complete',
        'physics_authoritative_batches', 'physics_committed_prefixes',
        'physics_ranges', 'physics_submitted_jobs', 'physics_completed_jobs',
		'physics_physical_worker_jobs', 'physics_owner_helped_jobs',
		'physics_physical_worker_mask', 'physics_distinct_physical_workers',
		'physics_physical_worker_mask_complete',
		'physics_peak_concurrent_physical_workers',
        'physics_allocated_bytes', 'physics_capture_ns', 'physics_prepare_ns',
        'physics_wait_ns', 'physics_commit_ns', 'physics_storage_bytes',
        'physics_storage_capacity_bytes', 'physics_storage_allocations',
        'physics_shadow_executions', 'physics_shadow_prefixes',
        'physics_shadow_ranges', 'physics_shadow_submitted_jobs',
        'physics_shadow_completed_jobs', 'physics_shadow_matches',
        'physics_shadow_mismatches', 'physics_owner_fallbacks',
        'physics_ineligible_slices', 'physics_unexpected_fallbacks',
        'physics_stale_rejections', 'physics_circuit_breaker_trips',
		'status_authoritative_batches', 'status_committed_commands',
		'status_submitted_jobs', 'status_completed_jobs',
		'status_physical_worker_jobs', 'status_owner_helped_jobs',
		'status_physical_worker_mask', 'status_distinct_physical_workers',
		'status_physical_worker_mask_complete',
		'status_peak_concurrent_physical_workers', 'status_shadow_executions',
		'status_shadow_commands', 'status_shadow_matches',
		'status_shadow_mismatches', 'status_owner_fallbacks',
		'status_stale_rejections') +
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
	[UInt64]$ordinaryPathWorkerExecutedRequests = 0
	[UInt64]$ordinaryPathWorkerExecutedRangeJobs = 0
	[UInt64]$ordinaryPathOwnerHelpedRangeJobs = 0
	[UInt64]$ordinaryPathPhysicalWorkerMask = 0
	[UInt64]$ordinaryPathDistinctPhysicalWorkers = 0
	[UInt64]$ordinaryPathAuthoritativeCommits = 0
	[UInt64]$ordinaryPathAuthoritativeMultiWorkerCommits = 0
	[UInt64]$ordinaryPathPeakWorkers = 0
	[UInt64]$ordinaryPathShadowComparisons = 0
    [UInt64]$collisionAuthoritativeCommits = 0
    [UInt64]$collisionShadowExecutions = 0
    [UInt64]$collisionShadowComparedCandidates = 0
    [UInt64]$collisionOwnerFallbacks = 0
    [UInt64]$collisionCommittedCandidates = 0
    [UInt64]$collisionPreparedPairs = 0
    [UInt64]$collisionUniqueCandidates = 0
    [UInt64]$collisionSubmittedJobs = 0
    [UInt64]$collisionCompletedJobs = 0
    [UInt64]$collisionPhysicalWorkerJobs = 0
    [UInt64]$collisionOwnerHelpedJobs = 0
    [UInt64]$collisionPhysicalWorkerMask = 0
    [UInt64]$collisionDistinctPhysicalWorkers = 0
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

		$ordinaryEligible = Get-Stage5UInt64Field $fields `
			'ordinary_path_eligible' $context
		$ordinarySubmittedRequests = Get-Stage5UInt64Field $fields `
			'ordinary_path_submitted_requests' $context
		$ordinarySubmittedRanges = Get-Stage5UInt64Field $fields `
			'ordinary_path_submitted_ranges' $context
		$ordinaryPathWorkerExecutedRequests = Get-Stage5UInt64Field $fields `
			'ordinary_path_worker_executed_requests' $context
		$ordinaryPathWorkerExecutedRangeJobs = Get-Stage5UInt64Field $fields `
			'ordinary_path_worker_executed_range_jobs' $context
		$ordinaryPathOwnerHelpedRangeJobs = Get-Stage5UInt64Field $fields `
			'ordinary_path_owner_helped_range_jobs' $context
		$ordinaryFailedRangeJobs = Get-Stage5UInt64Field $fields `
			'ordinary_path_failed_range_jobs' $context
		$ordinaryPathPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
			'ordinary_path_physical_worker_mask' $context
		$ordinaryPathDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'ordinary_path_distinct_physical_workers' $context
		$ordinaryPathAuthoritativeCommits = Get-Stage5UInt64Field $fields `
			'ordinary_path_authoritative_commits' $context
		$ordinaryPathAuthoritativeMultiWorkerCommits = Get-Stage5UInt64Field `
			$fields 'ordinary_path_authoritative_multiworker_commits' $context
		$ordinaryShadowComparisons = Get-Stage5UInt64Field $fields `
			'ordinary_path_shadow_comparisons' $context
		$ordinaryPathShadowComparisons = $ordinaryShadowComparisons
		$ordinaryPathPeakWorkers = Get-Stage5UInt64Field $fields `
			'ordinary_path_peak_active_workers' $context
		$ordinaryMaximumBatchRequests = Get-Stage5UInt64Field $fields `
			'ordinary_path_max_batch_requests' $context
		$ordinaryMaximumRangeCount = Get-Stage5UInt64Field $fields `
			'ordinary_path_max_range_count' $context
		$ordinaryMaximumGrainSize = Get-Stage5UInt64Field $fields `
			'ordinary_path_max_grain_size' $context
		Assert-Stage5Condition ($ordinarySubmittedRequests -le $ordinaryEligible) `
			"$context reports more submitted ordinary-path requests than eligible requests."
		Assert-Stage5Condition ($ordinaryPathWorkerExecutedRequests -le
			$ordinarySubmittedRequests) `
			"$context reports more worker-executed ordinary-path requests than submitted requests."
		Assert-Stage5Condition (($ordinaryPathWorkerExecutedRangeJobs +
			$ordinaryPathOwnerHelpedRangeJobs + $ordinaryFailedRangeJobs) -eq
			$ordinarySubmittedRanges) `
			"$context ordinary-path range execution identities do not account for every submitted range."
		Assert-Stage5Condition ($ordinaryPathOwnerHelpedRangeJobs -eq 0) `
			"$context reports owner-helped ordinary-path range jobs; authority is physical-worker-only."
		Assert-Stage5Condition ($ordinaryFailedRangeJobs -eq 0) `
			"$context reports failed ordinary-path range jobs."
		Assert-Stage5Condition ($ordinaryPathDistinctPhysicalWorkers -eq
			(Get-Stage5UInt64BitCount $ordinaryPathPhysicalWorkerMask)) `
			"$context ordinary-path physical-worker mask and distinct count disagree."
		Assert-Stage5Condition (($ordinaryPathWorkerExecutedRangeJobs -eq 0) -eq
			($ordinaryPathPhysicalWorkerMask -eq 0)) `
			"$context ordinary-path physical-worker jobs and identity mask disagree."
		Assert-Stage5Condition ($pathEffectiveWorkers -ge 64 -or
			($ordinaryPathPhysicalWorkerMask -shr [int]$pathEffectiveWorkers) -eq 0) `
			"$context ordinary-path physical-worker mask exceeds the effective worker lane."
		Assert-Stage5Condition ($ordinaryPathDistinctPhysicalWorkers -le
			$ordinaryPathWorkerExecutedRangeJobs -and
			$ordinaryPathDistinctPhysicalWorkers -le $pathEffectiveWorkers) `
			"$context reports an impossible ordinary-path physical-worker identity count."
		Assert-Stage5Condition ($ordinaryPathPeakWorkers -le
			$ordinaryPathDistinctPhysicalWorkers -and
			$ordinaryPathPeakWorkers -le $pathEffectiveWorkers) `
			"$context reports an impossible ordinary-path active-worker peak."
		Assert-Stage5Condition ($ordinaryPathAuthoritativeCommits -le
			$ordinaryPathWorkerExecutedRequests) `
			"$context reports ordinary-path authority not backed by physical-worker request execution."
		Assert-Stage5Condition ($ordinaryPathAuthoritativeMultiWorkerCommits -le
			$ordinaryPathAuthoritativeCommits) `
			"$context reports more multi-worker ordinary-path commits than authoritative commits."
		Assert-Stage5Condition ($ordinaryPathAuthoritativeMultiWorkerCommits -eq 0 -or
			($ordinarySubmittedRequests -ge 2 -and
			 $ordinaryPathWorkerExecutedRequests -ge 2 -and
			 $ordinaryPathDistinctPhysicalWorkers -gt 1 -and
			 $ordinaryPathPeakWorkers -gt 1)) `
			"$context reports ordinary-path multi-worker authority without a concurrent multi-request physical-worker batch."
		Assert-Stage5Condition ($ordinaryMaximumBatchRequests -le
			$ordinarySubmittedRequests -and $ordinaryMaximumRangeCount -le
			$ordinarySubmittedRanges -and $ordinaryMaximumRangeCount -le
			$pathEffectiveWorkers -and $ordinaryMaximumGrainSize -le
			$ordinaryMaximumBatchRequests) `
			"$context reports impossible ordinary-path adaptive batch gauges."
		Assert-Stage5Condition (($ordinarySubmittedRanges -eq 0 -and
			$ordinaryMaximumBatchRequests -eq 0 -and
			$ordinaryMaximumRangeCount -eq 0 -and
			$ordinaryMaximumGrainSize -eq 0 -and
			$ordinaryPathPeakWorkers -eq 0) -or
			($ordinarySubmittedRanges -gt 0 -and
			 $ordinaryMaximumBatchRequests -gt 0 -and
			 $ordinaryMaximumRangeCount -gt 0 -and
			 $ordinaryMaximumGrainSize -gt 0 -and
			 $ordinaryPathPeakWorkers -gt 0)) `
			"$context reports inconsistent ordinary-path batch activity gauges."
		foreach ($zeroInvariant in @('ordinary_path_stale_rejections',
			'ordinary_path_validation_failures', 'ordinary_path_shadow_mismatches',
			'ordinary_path_timeouts')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $fields $zeroInvariant $context) -eq 0) `
				"$context reports forbidden ordinary-path acceptance evidence in '$zeroInvariant'."
		}
		if ($isQualifyingPathStress) {
			Assert-Stage5Condition ($ordinaryEligible -ge 2 -and
				$ordinarySubmittedRequests -ge 2 -and
				$ordinarySubmittedRanges -ge 2 -and
				$ordinaryPathWorkerExecutedRequests -ge 2 -and
				$ordinaryPathWorkerExecutedRangeJobs -ge 2 -and
				$ordinaryPathDistinctPhysicalWorkers -gt 1 -and
				$ordinaryPathPeakWorkers -gt 1 -and
				$ordinaryMaximumBatchRequests -ge 2 -and
				$ordinaryMaximumRangeCount -ge 2 -and
				$ordinaryPathAuthoritativeCommits -gt 0 -and
				$ordinaryPathAuthoritativeMultiWorkerCommits -gt 0) `
				"$context qualifying parallel stress has no authoritative ordinary A* batch backed by concurrent physical path workers."
		}
		if ($Entry.simulationMode -ceq 'serial' -or
			$Entry.configuration -ceq 'parallel-1') {
			Assert-Stage5Condition ($ordinarySubmittedRequests -eq 0 -and
				$ordinarySubmittedRanges -eq 0 -and
				$ordinaryPathWorkerExecutedRequests -eq 0 -and
				$ordinaryPathWorkerExecutedRangeJobs -eq 0 -and
				$ordinaryPathAuthoritativeCommits -eq 0 -and
				$ordinaryPathAuthoritativeMultiWorkerCommits -eq 0 -and
				$ordinaryPathPhysicalWorkerMask -eq 0 -and
				$ordinaryPathPeakWorkers -eq 0) `
				"$context serial or parallel-one-worker lane reports ordinary-path batch work or authority."
		}
		if ($Entry.simulationMode -cne 'parallel') {
			Assert-Stage5Condition ($ordinaryPathAuthoritativeCommits -eq 0 -and
				$ordinaryPathAuthoritativeMultiWorkerCommits -eq 0) `
				"$context reports ordinary-path authority outside parallel simulation."
		}
		if ($Entry.simulationMode -cne 'shadow') {
			Assert-Stage5Condition ($ordinaryShadowComparisons -eq 0) `
				"$context reports ordinary-path shadow comparisons outside shadow simulation."
		}
		$isQualifyingOrdinaryShadow = $Entry.scenario -ceq '4v2' -and
			$Entry.simulationMode -ceq 'shadow' -and
			$Entry.configuration -ceq 'shadow-16'
		if ($isQualifyingOrdinaryShadow) {
			Assert-Stage5Condition ($ordinaryShadowComparisons -gt 0 -and
				$ordinaryPathWorkerExecutedRequests -gt 0 -and
				$ordinaryPathWorkerExecutedRangeJobs -gt 0) `
				"$context shadow stress has no physical-worker ordinary-path comparison."
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
        $collisionPhysicalWorkerJobs = Get-Stage5UInt64Field $fields `
            'collision_physical_worker_jobs' $context
        $collisionOwnerHelpedJobs = Get-Stage5UInt64Field $fields `
            'collision_owner_helped_jobs' $context
        $collisionPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
            'collision_physical_worker_mask' $context
        $collisionDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
            'collision_distinct_physical_workers' $context
		$collisionPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'collision_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($collisionPhysicalWorkerMaskComplete -le 1) `
			"$context collision physical-worker mask completeness is not boolean."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
            'collision_shadow_mismatches' $context) -eq 0) `
            "$context reports collision shadow mismatches."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
            'collision_unexpected_fallbacks' $context) -eq 0) `
            "$context reports unexpected collision owner fallbacks."
        Assert-Stage5Condition ($collisionCompletedJobs -eq $collisionSubmittedJobs) `
            "$context collision submitted/completed job counts do not match."
        Assert-Stage5Condition (($collisionPhysicalWorkerJobs +
            $collisionOwnerHelpedJobs) -eq $collisionCompletedJobs) `
            "$context collision execution identities do not account for every completed job."
		Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -ge
            (Get-Stage5UInt64BitCount $collisionPhysicalWorkerMask)) `
			"$context collision physical-worker mask exceeds the explicit distinct count."
		if ($collisionPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -eq
				(Get-Stage5UInt64BitCount $collisionPhysicalWorkerMask)) `
				"$context complete collision physical-worker mask and distinct count disagree."
		}
		else {
			Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -gt
				(Get-Stage5UInt64BitCount $collisionPhysicalWorkerMask)) `
				"$context incomplete collision physical-worker mask has no exact out-of-mask identity."
		}
        Assert-Stage5Condition (($collisionPhysicalWorkerJobs -eq 0) -eq
            ($collisionPhysicalWorkerMask -eq 0)) `
            "$context collision physical-worker jobs and identity mask disagree."
        [UInt64]$collisionWorkerBound = Get-Stage5UInt64Field $fields `
            'effective_workers' $context
        Assert-Stage5Condition ($collisionWorkerBound -ge 64 -or
            ($collisionPhysicalWorkerMask -shr [int]$collisionWorkerBound) -eq 0) `
            "$context collision physical-worker mask exceeds the effective worker lane."
        Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -le
            $collisionPhysicalWorkerJobs -and
            $collisionDistinctPhysicalWorkers -le
                $collisionWorkerBound) `
            "$context reports an impossible collision physical-worker identity count."
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
        $isQualifyingCollisionStress = $Entry.scenario -ceq '4v2' -and
            $Entry.simulationMode -ceq 'parallel' -and
            $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
        if ($isQualifyingCollisionStress) {
			$collisionStaleRejections = Get-Stage5UInt64Field $fields `
				'collision_stale_rejections' $context
			Assert-Stage5Condition ($collisionAuthoritativeCommits -gt 0 -and
				$collisionCommittedCandidates -gt 0 -and
				$collisionOwnerFallbacks -eq 0 -and $collisionStaleRejections -eq 0 -and
				$collisionPhysicalWorkerJobs -gt 0 -and
				$collisionDistinctPhysicalWorkers -ge 2) `
                "$context qualifying parallel stress has no collision work executed by at least two distinct physical collision workers."
			Assert-Stage5Condition ($collisionWorkerBound -gt 64 -or
				$collisionPhysicalWorkerMaskComplete -eq 1) `
				"$context qualifying collision stress below the mask width reports incomplete physical-worker identity evidence."
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
                'collision_submitted_jobs', 'collision_completed_jobs',
                'collision_physical_worker_jobs',
                'collision_owner_helped_jobs',
                'collision_physical_worker_mask',
                'collision_distinct_physical_workers')) {
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
                $collisionPhysicalWorkerJobs -eq 0 -and
                $collisionOwnerHelpedJobs -eq 0 -and
                $collisionPhysicalWorkerMask -eq 0 -and
                $collisionDistinctPhysicalWorkers -eq 0 -and
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
		$physicsPhysicalWorkerJobs = Get-Stage5UInt64Field $fields `
			'physics_physical_worker_jobs' $context
		$physicsOwnerHelpedJobs = Get-Stage5UInt64Field $fields `
			'physics_owner_helped_jobs' $context
		$physicsPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
			'physics_physical_worker_mask' $context
		$physicsDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'physics_distinct_physical_workers' $context
		$physicsPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'physics_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($physicsPhysicalWorkerMaskComplete -le 1) `
			"$context physics physical-worker mask completeness is not boolean."
		$physicsPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'physics_peak_concurrent_physical_workers' $context
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
		Assert-Stage5Condition (($physicsPhysicalWorkerJobs +
			$physicsOwnerHelpedJobs) -eq $physicsCompletedJobs) `
			"$context physics execution identities do not account for every completed job."
		[UInt64]$physicsMaskBitCount = Get-Stage5UInt64BitCount `
			$physicsPhysicalWorkerMask
		Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -ge
			$physicsMaskBitCount) `
			"$context physics physical-worker mask exceeds the exact distinct count."
		if ($physicsPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -eq
				$physicsMaskBitCount) `
				"$context complete physics physical-worker mask and distinct count disagree."
		}
		else {
			Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -gt
				$physicsMaskBitCount) `
				"$context incomplete physics physical-worker mask has no exact out-of-mask identity."
		}
		[UInt64]$physicsWorkerBound = Get-Stage5UInt64Field $fields `
			'effective_workers' $context
		Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -le
			$physicsPhysicalWorkerJobs -and
			$physicsDistinctPhysicalWorkers -le $physicsWorkerBound -and
			$physicsPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
			"$context reports impossible physics physical-worker evidence."
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
				$physicsSubmittedJobs -gt 0 -and $physicsCompletedJobs -gt 0 -and
				$physicsPhysicalWorkerJobs -eq $physicsCompletedJobs -and
				$physicsOwnerHelpedJobs -eq 0 -and
				$physicsDistinctPhysicalWorkers -ge 2 -and
				$physicsPeakConcurrentPhysicalWorkers -ge 2) `
				"$context qualifying parallel stress has no positive authoritative physics batch, prefix, range, and job evidence."
			Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
				$physicsPhysicalWorkerMaskComplete -eq 1) `
				"$context qualifying physics stress below the mask width reports incomplete physical-worker identity evidence."
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
				$physicsSubmittedJobs -eq 0 -and $physicsCompletedJobs -eq 0 -and
				$physicsPhysicalWorkerJobs -eq 0 -and
				$physicsOwnerHelpedJobs -eq 0 -and
				$physicsPhysicalWorkerMask -eq 0 -and
				$physicsDistinctPhysicalWorkers -eq 0 -and
				$physicsPeakConcurrentPhysicalWorkers -eq 0) `
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
				$physicsCommittedPrefixes -eq 0 -and
				$physicsPhysicalWorkerJobs -eq 0 -and
				$physicsOwnerHelpedJobs -eq 0) `
                "$context reports physics authority outside parallel simulation."
        }

		$statusAuthoritativeBatches = Get-Stage5UInt64Field $fields `
			'status_authoritative_batches' $context
		$statusCommittedCommands = Get-Stage5UInt64Field $fields `
			'status_committed_commands' $context
		$statusSubmittedJobs = Get-Stage5UInt64Field $fields 'status_submitted_jobs' $context
		$statusCompletedJobs = Get-Stage5UInt64Field $fields 'status_completed_jobs' $context
		$statusPhysicalWorkerJobs = Get-Stage5UInt64Field $fields `
			'status_physical_worker_jobs' $context
		$statusOwnerHelpedJobs = Get-Stage5UInt64Field $fields `
			'status_owner_helped_jobs' $context
		$statusPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
			'status_physical_worker_mask' $context
		$statusDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'status_distinct_physical_workers' $context
		$statusPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'status_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($statusPhysicalWorkerMaskComplete -le 1) `
			"$context status physical-worker mask completeness is not boolean."
		$statusPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'status_peak_concurrent_physical_workers' $context
		$statusShadowExecutions = Get-Stage5UInt64Field $fields `
			'status_shadow_executions' $context
		$statusShadowCommands = Get-Stage5UInt64Field $fields `
			'status_shadow_commands' $context
		$statusShadowMatches = Get-Stage5UInt64Field $fields 'status_shadow_matches' $context
		$statusShadowMismatches = Get-Stage5UInt64Field $fields `
			'status_shadow_mismatches' $context
		$statusOwnerFallbacks = Get-Stage5UInt64Field $fields 'status_owner_fallbacks' $context
		$statusStaleRejections = Get-Stage5UInt64Field $fields 'status_stale_rejections' $context
		Assert-Stage5Condition ($statusSubmittedJobs -eq $statusCompletedJobs -and
			($statusPhysicalWorkerJobs + $statusOwnerHelpedJobs) -eq
				$statusCompletedJobs) `
			"$context reports inconsistent status submitted/completed/identity counters."
		[UInt64]$statusMaskBitCount = Get-Stage5UInt64BitCount `
			$statusPhysicalWorkerMask
		Assert-Stage5Condition ($statusDistinctPhysicalWorkers -ge
			$statusMaskBitCount -and
			$statusDistinctPhysicalWorkers -le $statusPhysicalWorkerJobs -and
			$statusDistinctPhysicalWorkers -le $physicsWorkerBound -and
			$statusPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
			"$context reports impossible status physical-worker evidence."
		if ($statusPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($statusDistinctPhysicalWorkers -eq
				$statusMaskBitCount) `
				"$context complete status physical-worker mask and distinct count disagree."
		}
		else {
			Assert-Stage5Condition ($statusDistinctPhysicalWorkers -gt
				$statusMaskBitCount) `
				"$context incomplete status physical-worker mask has no exact out-of-mask identity."
		}
		Assert-Stage5Condition ($statusShadowExecutions -eq
			($statusShadowMatches + $statusShadowMismatches)) `
			"$context reports inconsistent status shadow counters."
		Assert-Stage5Condition ($statusShadowMismatches -eq 0 -and
			$statusOwnerFallbacks -eq 0 -and $statusStaleRejections -eq 0) `
			"$context reports forbidden status fallback, stale, or shadow mismatch evidence."
		$isQualifyingStatusStress = $Entry.scenario -ceq '4v2' -and
			$Entry.simulationMode -ceq 'parallel' -and
			$Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
		if ($isQualifyingStatusStress) {
			Assert-Stage5Condition ($statusAuthoritativeBatches -gt 0 -and
				$statusCommittedCommands -gt 0 -and $statusSubmittedJobs -gt 0 -and
				$statusPhysicalWorkerJobs -eq $statusCompletedJobs -and
				$statusOwnerHelpedJobs -eq 0 -and
				$statusDistinctPhysicalWorkers -ge 2 -and
				$statusPeakConcurrentPhysicalWorkers -ge 2) `
				"$context qualifying parallel stress has no physical live status authority."
			Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
				$statusPhysicalWorkerMaskComplete -eq 1) `
				"$context qualifying status stress below the mask width reports incomplete physical-worker identity evidence."
		}
		if ($Entry.simulationMode -ceq 'shadow') {
			Assert-Stage5Condition ($statusShadowExecutions -gt 0 -and
				$statusShadowMatches -eq $statusShadowExecutions -and
				$statusShadowCommands -gt 0 -and $statusAuthoritativeBatches -eq 0 -and
				$statusCommittedCommands -eq 0) `
				"$context shadow status evidence is missing or claims authority."
		}
		elseif ($statusShadowExecutions -ne 0 -or $statusShadowCommands -ne 0 -or
			$statusShadowMatches -ne 0) {
			throw "$context reports status shadow work outside shadow simulation."
		}
		if ($Entry.configuration -ceq 'serial-1' -or
			$Entry.configuration -ceq 'parallel-1' -or
			$Entry.simulationMode -cne 'parallel') {
			Assert-Stage5Condition ($statusAuthoritativeBatches -eq 0 -and
				$statusCommittedCommands -eq 0 -and $statusSubmittedJobs -eq 0 -and
				$statusCompletedJobs -eq 0 -and $statusPhysicalWorkerJobs -eq 0 -and
				$statusOwnerHelpedJobs -eq 0 -and $statusPhysicalWorkerMask -eq 0 -and
				$statusDistinctPhysicalWorkers -eq 0 -and
				$statusPeakConcurrentPhysicalWorkers -eq 0) `
				"$context nonparallel status lane reports live authority."
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
		ordinaryPathWorkerExecutedRequests =
			$ordinaryPathWorkerExecutedRequests
		ordinaryPathWorkerExecutedRangeJobs =
			$ordinaryPathWorkerExecutedRangeJobs
		ordinaryPathOwnerHelpedRangeJobs =
			$ordinaryPathOwnerHelpedRangeJobs
		ordinaryPathPhysicalWorkerMask = $ordinaryPathPhysicalWorkerMask
		ordinaryPathDistinctPhysicalWorkers =
			$ordinaryPathDistinctPhysicalWorkers
		ordinaryPathAuthoritativeCommits =
			$ordinaryPathAuthoritativeCommits
		ordinaryPathAuthoritativeMultiWorkerCommits =
			$ordinaryPathAuthoritativeMultiWorkerCommits
		ordinaryPathPeakActiveWorkers = $ordinaryPathPeakWorkers
		ordinaryPathShadowComparisons = $ordinaryPathShadowComparisons
        collisionAuthoritativeCommits = $collisionAuthoritativeCommits
        collisionShadowExecutions = $collisionShadowExecutions
        collisionShadowComparedCandidates = $collisionShadowComparedCandidates
        collisionOwnerFallbacks = $collisionOwnerFallbacks
        collisionCommittedCandidates = $collisionCommittedCandidates
        collisionPreparedPairs = $collisionPreparedPairs
        collisionUniqueCandidates = $collisionUniqueCandidates
        collisionSubmittedJobs = $collisionSubmittedJobs
        collisionCompletedJobs = $collisionCompletedJobs
        collisionPhysicalWorkerJobs = $collisionPhysicalWorkerJobs
        collisionOwnerHelpedJobs = $collisionOwnerHelpedJobs
        collisionPhysicalWorkerMask = $collisionPhysicalWorkerMask
        collisionDistinctPhysicalWorkers = $collisionDistinctPhysicalWorkers
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
        'prepared_pairs', 'unique_candidates', 'submitted_jobs', 'completed_jobs',
        'physical_worker_jobs', 'owner_helped_jobs', 'physical_worker_mask',
		'distinct_physical_workers', 'physical_worker_mask_complete')
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
    $collisionPhysicalWorkerJobs = Get-Stage5UInt64Field $collisionFields `
        'physical_worker_jobs' $context
    $collisionOwnerHelpedJobs = Get-Stage5UInt64Field $collisionFields `
        'owner_helped_jobs' $context
    $collisionPhysicalWorkerMask = Get-Stage5UInt64Field $collisionFields `
        'physical_worker_mask' $context
    $collisionDistinctPhysicalWorkers = Get-Stage5UInt64Field $collisionFields `
        'distinct_physical_workers' $context
	$collisionPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $collisionFields `
		'physical_worker_mask_complete' $context
	Assert-Stage5Condition ($collisionPhysicalWorkerMaskComplete -le 1) `
		"$context collision physical-worker mask completeness is not boolean."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
        'shadow_mismatches' $context) -eq 0) `
        "$context reports collision shadow mismatches."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
        'unexpected_fallbacks' $context) -eq 0) `
        "$context reports unexpected collision owner fallbacks."
    Assert-Stage5Condition ($collisionCompletedJobs -eq $collisionSubmittedJobs) `
        "$context collision submitted/completed job counts do not match."
    Assert-Stage5Condition (($collisionPhysicalWorkerJobs +
        $collisionOwnerHelpedJobs) -eq $collisionCompletedJobs) `
        "$context collision execution identities do not account for every completed job."
	Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -ge
        (Get-Stage5UInt64BitCount $collisionPhysicalWorkerMask)) `
		"$context collision physical-worker mask exceeds the explicit distinct count."
	if ($collisionPhysicalWorkerMaskComplete -eq 1) {
		Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -eq
			(Get-Stage5UInt64BitCount $collisionPhysicalWorkerMask)) `
			"$context complete collision physical-worker mask and distinct count disagree."
	}
	else {
		Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -gt
			(Get-Stage5UInt64BitCount $collisionPhysicalWorkerMask)) `
			"$context incomplete collision physical-worker mask has no exact out-of-mask identity."
	}
    Assert-Stage5Condition (($collisionPhysicalWorkerJobs -eq 0) -eq
        ($collisionPhysicalWorkerMask -eq 0)) `
        "$context collision physical-worker jobs and identity mask disagree."
    [UInt64]$collisionWorkerBound = Get-Stage5UInt64Field $fields `
        'workers' $context
    Assert-Stage5Condition ($collisionWorkerBound -ge 64 -or
        ($collisionPhysicalWorkerMask -shr [int]$collisionWorkerBound) -eq 0) `
        "$context collision physical-worker mask exceeds the effective worker lane."
    Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -le
        $collisionPhysicalWorkerJobs -and
        $collisionDistinctPhysicalWorkers -le
            $collisionWorkerBound) `
        "$context reports an impossible collision physical-worker identity count."
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
    $isQualifyingCollisionStress = $Entry.stress -and
        $Entry.simulationMode -ceq 'parallel' -and
        $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
    if ($isQualifyingCollisionStress) {
		$collisionStaleRejections = Get-Stage5UInt64Field $collisionFields `
			'stale_rejections' $context
		Assert-Stage5Condition ($collisionAuthoritativeCommits -gt 0 -and
			$collisionCommittedCandidates -gt 0 -and
			$collisionOwnerFallbacks -eq 0 -and $collisionStaleRejections -eq 0 -and
			$collisionPhysicalWorkerJobs -gt 0 -and
            $collisionDistinctPhysicalWorkers -ge 2) `
            "$context qualifying stress replay has no collision work executed by at least two distinct physical collision workers."
		Assert-Stage5Condition ($collisionWorkerBound -gt 64 -or
			$collisionPhysicalWorkerMaskComplete -eq 1) `
			"$context qualifying collision replay below the mask width reports incomplete physical-worker identity evidence."
    }
    if ($Entry.simulationMode -ceq 'serial') {
		foreach ($serialCollisionField in @($collisionFieldNames | Where-Object {
			$_ -cne 'physical_worker_mask_complete' })) {
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
            $collisionPhysicalWorkerJobs -eq 0 -and
            $collisionOwnerHelpedJobs -eq 0 -and
            $collisionPhysicalWorkerMask -eq 0 -and
            $collisionDistinctPhysicalWorkers -eq 0 -and
            (Get-Stage5UInt64Field $collisionFields 'stale_rejections' $context) -eq 0) `
            "$context one-worker ineligible replay reports collision prepared/publication work."
    }

    $physicsLine = Get-Stage5SingleLine $Output 'PHYSICS_INTEGRATION_MANIFEST' $context
    $physicsFields = ConvertFrom-Stage5MetricLine $physicsLine `
        'PHYSICS_INTEGRATION_MANIFEST' "$context physics manifest"
	$physicsFieldNames = @('authoritative_batches', 'committed_prefixes', 'ranges',
		'submitted_jobs', 'completed_jobs', 'physical_worker_jobs',
		'owner_helped_jobs', 'physical_worker_mask', 'distinct_physical_workers',
		'physical_worker_mask_complete',
		'peak_concurrent_physical_workers', 'allocated_bytes', 'capture_ns',
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
	$physicsPhysicalWorkerJobs = Get-Stage5UInt64Field $physicsFields `
		'physical_worker_jobs' $context
	$physicsOwnerHelpedJobs = Get-Stage5UInt64Field $physicsFields `
		'owner_helped_jobs' $context
	$physicsPhysicalWorkerMask = Get-Stage5UInt64Field $physicsFields `
		'physical_worker_mask' $context
	$physicsDistinctPhysicalWorkers = Get-Stage5UInt64Field $physicsFields `
		'distinct_physical_workers' $context
	$physicsPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $physicsFields `
		'physical_worker_mask_complete' $context
	Assert-Stage5Condition ($physicsPhysicalWorkerMaskComplete -le 1) `
		"$context physics physical-worker mask completeness is not boolean."
	$physicsPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $physicsFields `
		'peak_concurrent_physical_workers' $context
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
	[UInt64]$physicsMaskBitCount = Get-Stage5UInt64BitCount `
		$physicsPhysicalWorkerMask
	Assert-Stage5Condition (($physicsPhysicalWorkerJobs + $physicsOwnerHelpedJobs) -eq
		$physicsCompletedJobs -and $physicsDistinctPhysicalWorkers -ge
		$physicsMaskBitCount) `
		"$context reports inconsistent physics execution identities."
	if ($physicsPhysicalWorkerMaskComplete -eq 1) {
		Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -eq
			$physicsMaskBitCount) `
			"$context complete physics physical-worker mask and distinct count disagree."
	}
	else {
		Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -gt
			$physicsMaskBitCount) `
			"$context incomplete physics physical-worker mask has no exact out-of-mask identity."
	}
	[UInt64]$physicsWorkerBound = Get-Stage5UInt64Field $fields 'workers' $context
	Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -le
		$physicsPhysicalWorkerJobs -and
		$physicsDistinctPhysicalWorkers -le $physicsWorkerBound -and
		$physicsPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
		"$context reports impossible physics physical-worker evidence."
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
			$physicsSubmittedJobs -gt 0 -and $physicsCompletedJobs -gt 0 -and
			$physicsPhysicalWorkerJobs -eq $physicsCompletedJobs -and
			$physicsOwnerHelpedJobs -eq 0 -and
			$physicsDistinctPhysicalWorkers -ge 2 -and
			$physicsPeakConcurrentPhysicalWorkers -ge 2) `
			"$context qualifying stress replay has no positive authoritative physics batch, prefix, range, and job evidence."
		Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
			$physicsPhysicalWorkerMaskComplete -eq 1) `
			"$context qualifying physics replay below the mask width reports incomplete physical-worker identity evidence."
    }
    if ($Entry.configuration -ceq 'serial-1' -or
        $Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
            $physicsCommittedPrefixes -eq 0 -and $physicsRanges -eq 0 -and
			$physicsSubmittedJobs -eq 0 -and $physicsCompletedJobs -eq 0 -and
			$physicsPhysicalWorkerJobs -eq 0 -and $physicsOwnerHelpedJobs -eq 0 -and
			$physicsPhysicalWorkerMask -eq 0 -and
			$physicsDistinctPhysicalWorkers -eq 0 -and
			$physicsPeakConcurrentPhysicalWorkers -eq 0) `
            "$context nonqualifying serial/one-worker replay reports physics authority or prepared jobs."
		foreach ($physicsPreparationField in @('allocated_bytes', 'capture_ns',
			'prepare_ns', 'wait_ns', 'commit_ns', 'storage_bytes',
			'storage_capacity_bytes', 'storage_allocations')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $physicsFields `
				$physicsPreparationField $context) -eq 0) `
				"$context nonqualifying serial/one-worker replay reports physics pre-scan, capture, or storage work in '$physicsPreparationField'."
		}
    }

	$statusLine = Get-Stage5SingleLine $Output 'OBJECT_STATUS_TIMER_MANIFEST' $context
	$statusFields = ConvertFrom-Stage5MetricLine $statusLine `
		'OBJECT_STATUS_TIMER_MANIFEST' "$context status manifest"
	$statusFieldNames = @('authoritative_batches', 'committed_commands',
		'submitted_jobs', 'completed_jobs', 'physical_worker_jobs',
		'owner_helped_jobs', 'physical_worker_mask', 'distinct_physical_workers',
		'physical_worker_mask_complete',
		'peak_concurrent_physical_workers', 'shadow_executions', 'shadow_commands',
		'shadow_matches', 'shadow_mismatches', 'owner_fallbacks', 'stale_rejections')
	foreach ($numeric in $statusFieldNames) {
		Get-Stage5UInt64Field $statusFields $numeric "$context status manifest" | Out-Null
	}
	$statusAuthoritativeBatches = Get-Stage5UInt64Field $statusFields 'authoritative_batches' $context
	$statusCommittedCommands = Get-Stage5UInt64Field $statusFields 'committed_commands' $context
	$statusSubmittedJobs = Get-Stage5UInt64Field $statusFields 'submitted_jobs' $context
	$statusCompletedJobs = Get-Stage5UInt64Field $statusFields 'completed_jobs' $context
	$statusPhysicalWorkerJobs = Get-Stage5UInt64Field $statusFields 'physical_worker_jobs' $context
	$statusOwnerHelpedJobs = Get-Stage5UInt64Field $statusFields 'owner_helped_jobs' $context
	$statusPhysicalWorkerMask = Get-Stage5UInt64Field $statusFields 'physical_worker_mask' $context
	$statusDistinctPhysicalWorkers = Get-Stage5UInt64Field $statusFields 'distinct_physical_workers' $context
	$statusPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $statusFields `
		'physical_worker_mask_complete' $context
	Assert-Stage5Condition ($statusPhysicalWorkerMaskComplete -le 1) `
		"$context status physical-worker mask completeness is not boolean."
	$statusPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $statusFields `
		'peak_concurrent_physical_workers' $context
	$statusShadowExecutions = Get-Stage5UInt64Field $statusFields 'shadow_executions' $context
	$statusShadowCommands = Get-Stage5UInt64Field $statusFields 'shadow_commands' $context
	$statusShadowMatches = Get-Stage5UInt64Field $statusFields 'shadow_matches' $context
	$statusShadowMismatches = Get-Stage5UInt64Field $statusFields 'shadow_mismatches' $context
	$statusOwnerFallbacks = Get-Stage5UInt64Field $statusFields 'owner_fallbacks' $context
	$statusStaleRejections = Get-Stage5UInt64Field $statusFields 'stale_rejections' $context
	[UInt64]$statusMaskBitCount = Get-Stage5UInt64BitCount `
		$statusPhysicalWorkerMask
	Assert-Stage5Condition ($statusSubmittedJobs -eq $statusCompletedJobs -and
		($statusPhysicalWorkerJobs + $statusOwnerHelpedJobs) -eq $statusCompletedJobs -and
		$statusDistinctPhysicalWorkers -ge $statusMaskBitCount -and
		$statusDistinctPhysicalWorkers -le $statusPhysicalWorkerJobs -and
		$statusDistinctPhysicalWorkers -le $physicsWorkerBound -and
		$statusPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
		"$context reports inconsistent status physical execution evidence."
	if ($statusPhysicalWorkerMaskComplete -eq 1) {
		Assert-Stage5Condition ($statusDistinctPhysicalWorkers -eq
			$statusMaskBitCount) `
			"$context complete status physical-worker mask and distinct count disagree."
	}
	else {
		Assert-Stage5Condition ($statusDistinctPhysicalWorkers -gt
			$statusMaskBitCount) `
			"$context incomplete status physical-worker mask has no exact out-of-mask identity."
	}
	Assert-Stage5Condition ($statusShadowExecutions -eq
		($statusShadowMatches + $statusShadowMismatches) -and
		$statusShadowMismatches -eq 0 -and $statusOwnerFallbacks -eq 0 -and
		$statusStaleRejections -eq 0) `
		"$context reports forbidden status fallback, stale, or shadow mismatch evidence."
	$isQualifyingStatusStress = $Entry.stress -and
		$Entry.simulationMode -ceq 'parallel' -and
		$Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
	if ($isQualifyingStatusStress) {
		Assert-Stage5Condition ($statusAuthoritativeBatches -gt 0 -and
			$statusCommittedCommands -gt 0 -and $statusSubmittedJobs -gt 0 -and
			$statusPhysicalWorkerJobs -eq $statusCompletedJobs -and
			$statusOwnerHelpedJobs -eq 0 -and
			$statusDistinctPhysicalWorkers -ge 2 -and
			$statusPeakConcurrentPhysicalWorkers -ge 2) `
			"$context qualifying stress replay has no physical live status authority."
		Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
			$statusPhysicalWorkerMaskComplete -eq 1) `
			"$context qualifying status replay below the mask width reports incomplete physical-worker identity evidence."
	}
	Assert-Stage5Condition ($statusShadowExecutions -eq 0 -and
		$statusShadowCommands -eq 0 -and $statusShadowMatches -eq 0) `
		"$context replay reports status shadow work outside shadow mode."
	if ($Entry.configuration -ceq 'serial-1' -or $Entry.configuration -ceq 'parallel-1') {
		Assert-Stage5Condition ($statusAuthoritativeBatches -eq 0 -and
			$statusCommittedCommands -eq 0 -and $statusSubmittedJobs -eq 0 -and
			$statusCompletedJobs -eq 0 -and $statusPhysicalWorkerJobs -eq 0 -and
			$statusOwnerHelpedJobs -eq 0 -and $statusPhysicalWorkerMask -eq 0 -and
			$statusDistinctPhysicalWorkers -eq 0 -and
			$statusPeakConcurrentPhysicalWorkers -eq 0) `
			"$context nonqualifying replay reports status authority."
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
        collisionPhysicalWorkerJobs = $collisionPhysicalWorkerJobs
        collisionOwnerHelpedJobs = $collisionOwnerHelpedJobs
        collisionPhysicalWorkerMask = $collisionPhysicalWorkerMask
        collisionDistinctPhysicalWorkers = $collisionDistinctPhysicalWorkers
        physicsAuthoritativeBatches = $physicsAuthoritativeBatches
        physicsCommittedPrefixes = $physicsCommittedPrefixes
        physicsRanges = $physicsRanges
        physicsSubmittedJobs = $physicsSubmittedJobs
        physicsCompletedJobs = $physicsCompletedJobs
		physicsPhysicalWorkerJobs = $physicsPhysicalWorkerJobs
		physicsOwnerHelpedJobs = $physicsOwnerHelpedJobs
		physicsDistinctPhysicalWorkers = $physicsDistinctPhysicalWorkers
		physicsPeakConcurrentPhysicalWorkers = $physicsPeakConcurrentPhysicalWorkers
        physicsShadowExecutions = $physicsShadowExecutions
        physicsShadowPrefixes = $physicsShadowPrefixes
        physicsShadowRanges = $physicsShadowRanges
        physicsShadowSubmittedJobs = $physicsShadowSubmittedJobs
        physicsShadowCompletedJobs = $physicsShadowCompletedJobs
        spatialEvidence = $spatialEvidence
        collisionFields = $collisionFields
        physicsFields = $physicsFields
		statusFields = $statusFields
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
        $_.aiEvidence.collisionCompletedJobs -gt 0 -and
        $_.aiEvidence.collisionPhysicalWorkerJobs -gt 0 -and
        $_.aiEvidence.collisionDistinctPhysicalWorkers -ge 2
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
	$ordinaryPathAuthoritative = @($stressParallel | Where-Object {
		$_.aiEvidence.ordinaryPathWorkerExecutedRequests -gt 1 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 1 -and
		$_.aiEvidence.ordinaryPathDistinctPhysicalWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathAuthoritativeCommits -gt 0 -and
		$_.aiEvidence.ordinaryPathAuthoritativeMultiWorkerCommits -gt 0
	})
	Assert-Stage5Condition ($ordinaryPathAuthoritative.Count -gt 0) `
		'AI stress evidence has no authoritative ordinary A* commit backed by concurrent physical-worker range jobs; compact-direct, global scheduler, AI, collision, owner-help, or shadow counters cannot proxy ordinary path work.'
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
        $_.aiEvidence.collisionPhysicalWorkerJobs -gt 0 -and
        $_.aiEvidence.collisionDistinctPhysicalWorkers -ge 2 -and
        $_.aiEvidence.physicsAuthoritativeBatches -gt 0 -and
        $_.aiEvidence.physicsCommittedPrefixes -gt 0 -and
        $_.aiEvidence.physicsRanges -gt 0 -and
        $_.aiEvidence.physicsSubmittedJobs -gt 0 -and
        $_.aiEvidence.physicsCompletedJobs -gt 0 -and
		$_.aiEvidence.pathWorkerExecuted -gt 1 -and
		$_.aiEvidence.pathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.pathAuthoritativeMultiWorkerCommits -gt 0 -and
        $_.aiEvidence.pathAuthoritativeCommits -gt 0 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRequests -gt 1 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 1 -and
		$_.aiEvidence.ordinaryPathDistinctPhysicalWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathAuthoritativeCommits -gt 0 -and
		$_.aiEvidence.ordinaryPathAuthoritativeMultiWorkerCommits -gt 0 -and
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($coLocatedAuthoritative.Count -gt 0) `
		'Overall Stage 5 acceptance requires AI, collision, physics, direct-path, ordinary-path, healing-spatial, and PDL-spatial authority on the same qualifying parallel 4v2 stress execution; evidence split across executions is insufficient.'
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
	$ordinaryPathShadow = @($Results | Where-Object {
		$_.kind -ceq 'ai' -and $_.stress -and
		$_.configuration -ceq 'shadow-16' -and
		$null -ne $_.aiEvidence -and
		$_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
		$_.aiEvidence.ordinaryPathShadowComparisons -gt 0 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRequests -gt 0 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 0 -and
		$_.aiEvidence.ordinaryPathAuthoritativeCommits -eq 0
	})
	Assert-Stage5Condition ($ordinaryPathShadow.Count -gt 0) `
		'AI stress evidence has no installed ordinary-path shadow comparison backed by physical-worker request and range execution.'
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

function Get-Stage5FinalAcceptanceReceiptContract {
    param([string]$Role)
    $contracts = @{
        'validation-plan' = [pscustomobject]@{
            producer = 'installed-runtime-validation-plan-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            detailNames = @('gateName', 'validationSet', 'entryCount')
        }
        'validation-results' = [pscustomobject]@{
            producer = 'installed-runtime-validation-results-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            detailNames = @('resultCount', 'allExecutionsPassed', 'resultsSha256')
        }
        'replay-results' = [pscustomobject]@{
            producer = 'installed-runtime-replay-results-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            detailNames = @('uniqueReplayCount', 'executionCount',
                'crcTreeSha256', 'allExecutionsPassed')
        }
        'replay-fixture-manifest' = [pscustomobject]@{
            producer = 'reviewed-replay-fixture-manifest-v2'
            producerVersion = '2'
            currentProducer = 'caller-supplied replay fixture manifest'
            detailNames = @('fixtureCount', 'stressFixtureCount',
                'fixtureSetSha256')
        }
        'ai-results' = [pscustomobject]@{
            producer = 'installed-runtime-ai-results-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            detailNames = @('scenarioCount', 'distinctSeedCount', 'repeatCount',
                'allGamesCompleted', 'digestTreeSha256')
        }
        'combined-results' = [pscustomobject]@{
            producer = 'installed-runtime-combined-results-v2'
            producerVersion = '2'
            currentProducer = 'none'
            detailNames = @('pipelineMode', 'simulationMode', 'workerPolicy',
                'renderer', 'renderThread', 'bothTitlesPassed')
        }
        'premium-review-results' = [pscustomobject]@{
            producer = 'stage5-premium-review-receipt-v2'
            producerVersion = '2'
            currentProducer = 'none'
            detailNames = @('reviewedCommit', 'reviewRounds',
                'independentReviewers', 'openP0', 'openP1', 'openP2')
        }
        'manual-checklist' = [pscustomobject]@{
            producer = 'installed-runtime-manual-acceptance-v2'
            producerVersion = '2'
            currentProducer = 'none'
            detailNames = @('approvalScope', 'candidateHashVerified',
                'bothTitlesTested', 'cleanExitPassed')
        }
    }
    Assert-Stage5Condition ($contracts.ContainsKey($Role)) `
        "No immutable final-acceptance receipt contract exists for attachment role '$Role'."
    return $contracts[$Role]
}

function Read-Stage5FinalAcceptanceImmutableReceipt {
    param(
        [string]$Path,
        [string]$Kind,
        [string]$Role,
        [string]$EvidenceTitle,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [Collections.IDictionary]$SeenRunNonces = $null
    )
    $context = "Final acceptance '$Kind' attachment '$Role'"
    $contract = Get-Stage5FinalAcceptanceReceiptContract $Role
    $document = ConvertFrom-Stage5JsonDictionary $Path
    Assert-Stage5Condition ($document -is [Collections.IDictionary]) `
        "missing-producer: $context is not an executable-originated JSON receipt."
    $names = @('schemaVersion', 'evidenceKind', 'status', 'role', 'producer',
        'producerVersion', 'runNonce', 'sourceCommit', 'title', 'architecture',
        'artifactSetSha256', 'executableSha256', 'recordedUtc', 'rawLogs',
        'details')
    $missing = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $names) {
        if (@($document.Keys | Where-Object { [string]$_ -ceq $name }).Count -eq 0) {
            $missing.Add($name) | Out-Null
        }
    }
    if ($missing.Count -gt 0) {
        throw "missing-producer: $context lacks immutable receipt fields '$($missing -join ', ')'. Expected producer '$($contract.producer)' version $($contract.producerVersion); current producer '$($contract.currentProducer)' does not emit this receipt."
    }
    Assert-Stage5JsonShape $document $names $context
    $schemaVersion = Get-Stage5JsonValue $document 'schemaVersion' $context
    $evidenceKind = Get-Stage5JsonValue $document 'evidenceKind' $context
    $status = Get-Stage5JsonValue $document 'status' $context
    $receiptRole = Get-Stage5JsonValue $document 'role' $context
    $producer = Get-Stage5JsonValue $document 'producer' $context
    $producerVersion = Get-Stage5JsonValue $document 'producerVersion' $context
    $runNonce = Get-Stage5JsonValue $document 'runNonce' $context
    $sourceCommit = Get-Stage5JsonValue $document 'sourceCommit' $context
    $title = Get-Stage5JsonValue $document 'title' $context
    $architecture = Get-Stage5JsonValue $document 'architecture' $context
    $artifactSetSha256 = Get-Stage5JsonValue $document 'artifactSetSha256' $context
    $executableSha256 = Get-Stage5JsonValue $document 'executableSha256' $context
    $recordedUtc = Get-Stage5JsonValue $document 'recordedUtc' $context
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and
        [Int64]$schemaVersion -eq 1 -and
        $evidenceKind -is [string] -and
        $evidenceKind -ceq 'stage5-executable-originated-receipt' -and
        $status -is [string] -and $status -ceq 'passed') `
        "$context has an invalid immutable receipt identity."
    Assert-Stage5Condition ($receiptRole -is [string] -and $receiptRole -ceq $Role) `
        "$context role is substituted; receipt role must be '$Role'."
    Assert-Stage5Condition ($producer -is [string] -and
        $producer -ceq [string]$contract.producer -and
        $producerVersion -is [string] -and
        $producerVersion -ceq [string]$contract.producerVersion) `
        "missing-producer: $context has an unregistered producer/version. Expected '$($contract.producer)' version $($contract.producerVersion); current producer '$($contract.currentProducer)' does not emit this receipt."
    Assert-Stage5Condition ($runNonce -is [string] -and
        $runNonce -match '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
        "$context runNonce is not a canonical UUID nonce."
    Assert-Stage5Condition ($sourceCommit -is [string] -and
        $sourceCommit -ceq $ExpectedSourceCommit) `
        "$context sourceCommit is stale or does not match the final acceptance commit."
    Assert-Stage5Condition ($title -is [string] -and $title -ceq $EvidenceTitle) `
        "$context title scope is substituted; expected '$EvidenceTitle'."
    Assert-Stage5Condition ($architecture -is [string] -and
        $architecture -ceq 'x64') "$context must identify x64 architecture."
    Assert-Stage5Condition ($artifactSetSha256 -is [string] -and
        $artifactSetSha256 -match '^[0-9A-Fa-f]{64}$' -and
        $artifactSetSha256.ToUpperInvariant() -ceq
            $ExpectedArtifactSetSha256.ToUpperInvariant()) `
        "$context artifactSetSha256 does not bind the independently hashed artifact set."
    Assert-Stage5Condition ($recordedUtc -is [string]) `
        "$context recordedUtc must be a JSON string."
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($recordedUtc, [ref]$recorded)) `
        "$context recordedUtc is not a valid timestamp."

    $expectedGenerals = [string]$ArtifactHashes['generals-executable']
    $expectedZeroHour = [string]$ArtifactHashes['zerohour-executable']
    if ($EvidenceTitle -ceq 'Both') {
        Assert-Stage5JsonShape $executableSha256 @('Generals', 'ZeroHour') `
            "$context executableSha256"
        Assert-Stage5Condition (
            (Get-Stage5JsonValue $executableSha256 'Generals' "$context executableSha256") -is [string] -and
            (Get-Stage5JsonValue $executableSha256 'ZeroHour' "$context executableSha256") -is [string] -and
            (Get-Stage5JsonValue $executableSha256 'Generals' "$context executableSha256").ToUpperInvariant() -ceq
                $expectedGenerals.ToUpperInvariant() -and
            (Get-Stage5JsonValue $executableSha256 'ZeroHour' "$context executableSha256").ToUpperInvariant() -ceq
                $expectedZeroHour.ToUpperInvariant()) `
            "$context executable SHA-256 binding does not match the artifact set."
    }
    else {
        $expectedExecutable = if ($EvidenceTitle -ceq 'Generals') {
            $expectedGenerals
        }
        else { $expectedZeroHour }
        Assert-Stage5Condition ($executableSha256 -is [string] -and
            $executableSha256 -match '^[0-9A-Fa-f]{64}$' -and
            $executableSha256.ToUpperInvariant() -ceq $expectedExecutable.ToUpperInvariant()) `
            "$context executable SHA-256 binding does not match the artifact set."
    }

    $rawLogs = Get-Stage5JsonValue $document 'rawLogs' $context
    Assert-Stage5Condition ($rawLogs -is [Array] -and $rawLogs.Count -gt 0) `
        "missing-producer: $context has no executable-originated raw-log hash bindings."
    $rawNames = New-Object 'Collections.Generic.List[string]'
    $rawPaths = New-Object 'Collections.Generic.List[string]'
    $validatedRawLogs = New-Object 'Collections.Generic.List[object]'
    foreach ($rawLog in $rawLogs) {
        Assert-Stage5JsonShape $rawLog @('name', 'path', 'sha256') `
            "$context raw log"
        $rawName = Get-Stage5JsonValue $rawLog 'name' "$context raw log"
        $rawRelative = Get-Stage5JsonValue $rawLog 'path' "$context raw log"
        $rawExpectedHash = Get-Stage5JsonValue $rawLog 'sha256' "$context raw log"
        Assert-Stage5Condition ($rawName -is [string] -and
            $rawRelative -is [string] -and
            $rawExpectedHash -is [string]) `
            "$context raw log name, path, and SHA-256 must be strings."
        Assert-Stage5Condition (-not ($rawNames -contains $rawName)) `
            "$context repeats raw log '$rawName'."
        $rawPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
            $rawRelative "$context raw log '$rawName'"
        Assert-Stage5Condition (-not ($rawPaths -contains $rawPath.ToLowerInvariant())) `
            "$context aliases raw log path '$rawRelative'."
        $rawHash = Assert-Stage5FinalAcceptanceSha256 $rawPath $rawExpectedHash `
            "$context raw log '$rawName'"
        $rawNames.Add($rawName) | Out-Null
        $rawPaths.Add($rawPath.ToLowerInvariant()) | Out-Null
        $validatedRawLogs.Add([pscustomobject]@{
            name = $rawName; path = $rawRelative; sha256 = $rawHash
        }) | Out-Null
    }

    $details = Get-Stage5JsonValue $document 'details' $context
    try {
        foreach ($detailName in $contract.detailNames) {
            Get-Stage5JsonValue $details $detailName "$context details" | Out-Null
        }
    }
    catch {
        throw "missing-producer: $context does not contain semantically parsed role-specific details. Current producer '$($contract.currentProducer)' does not emit the required immutable receipt: $($_.Exception.Message)"
    }
    switch ($Role) {
        'validation-plan' {
            Assert-Stage5Condition ((Get-Stage5JsonValue $details 'gateName' "$context details") -is [string] -and
                (Get-Stage5JsonValue $details 'gateName' "$context details") -ceq 'deterministic-runtime' -and
                (Get-Stage5JsonValue $details 'validationSet' "$context details") -is [string] -and
                (Get-Stage5JsonValue $details 'validationSet' "$context details") -ceq 'All' -and
                (Test-Stage5JsonInteger (Get-Stage5JsonValue $details 'entryCount' "$context details")) -and
                [Int64](Get-Stage5JsonValue $details 'entryCount' "$context details") -gt 0) `
                "$context role details are not semantically valid."
        }
        'validation-results' {
            $resultCount = Get-Stage5JsonValue $details 'resultCount' "$context details"
            $allPassed = Get-Stage5JsonValue $details 'allExecutionsPassed' "$context details"
            $resultsHash = Get-Stage5JsonValue $details 'resultsSha256' "$context details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $resultCount) -and
                [Int64]$resultCount -gt 0 -and $allPassed -is [bool] -and
                $allPassed -and $resultsHash -is [string] -and
                $resultsHash -match '^[0-9A-Fa-f]{64}$') `
                "$context role details are not semantically valid."
        }
        'replay-results' {
            $unique = Get-Stage5JsonValue $details 'uniqueReplayCount' "$context details"
            $executions = Get-Stage5JsonValue $details 'executionCount' "$context details"
            $crcTree = Get-Stage5JsonValue $details 'crcTreeSha256' "$context details"
            $allPassed = Get-Stage5JsonValue $details 'allExecutionsPassed' "$context details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $unique) -and
                [Int64]$unique -ge 10 -and (Test-Stage5JsonInteger $executions) -and
                [Int64]$executions -ge [Int64]$unique -and $allPassed -is [bool] -and
                $allPassed -and $crcTree -is [string] -and
                $crcTree -match '^[0-9A-Fa-f]{64}$') `
                "$context role details are not semantically valid."
        }
        'replay-fixture-manifest' {
            $fixtureCount = Get-Stage5JsonValue $details 'fixtureCount' "$context details"
            $stressCount = Get-Stage5JsonValue $details 'stressFixtureCount' "$context details"
            $fixtureHash = Get-Stage5JsonValue $details 'fixtureSetSha256' "$context details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $fixtureCount) -and
                [Int64]$fixtureCount -ge 10 -and (Test-Stage5JsonInteger $stressCount) -and
                [Int64]$stressCount -eq 1 -and $fixtureHash -is [string] -and
                $fixtureHash -match '^[0-9A-Fa-f]{64}$') `
                "$context role details are not semantically valid."
        }
        'ai-results' {
            $scenarioCount = Get-Stage5JsonValue $details 'scenarioCount' "$context details"
            $seedCount = Get-Stage5JsonValue $details 'distinctSeedCount' "$context details"
            $repeatCount = Get-Stage5JsonValue $details 'repeatCount' "$context details"
            $allCompleted = Get-Stage5JsonValue $details 'allGamesCompleted' "$context details"
            $digestTree = Get-Stage5JsonValue $details 'digestTreeSha256' "$context details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $scenarioCount) -and
                [Int64]$scenarioCount -ge 2 -and (Test-Stage5JsonInteger $seedCount) -and
                [Int64]$seedCount -ge 3 -and (Test-Stage5JsonInteger $repeatCount) -and
                [Int64]$repeatCount -ge 2 -and $allCompleted -is [bool] -and
                $allCompleted -and $digestTree -is [string] -and
                $digestTree -match '^[0-9A-Fa-f]{64}$') `
                "$context role details are not semantically valid."
        }
        'combined-results' {
            Assert-Stage5Condition ((Get-Stage5JsonValue $details 'pipelineMode' "$context details") -ceq 'parallel' -and
                (Get-Stage5JsonValue $details 'simulationMode' "$context details") -ceq 'parallel' -and
                (Get-Stage5JsonValue $details 'workerPolicy' "$context details") -ceq 'auto' -and
                (Get-Stage5JsonValue $details 'renderer' "$context details") -ceq 'd3d11' -and
                (Get-Stage5JsonValue $details 'renderThread' "$context details") -ceq 'dedicated' -and
                (Get-Stage5JsonValue $details 'bothTitlesPassed' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'bothTitlesPassed' "$context details")) `
                "$context role details are not semantically valid."
        }
        'premium-review-results' {
            $reviewedCommit = Get-Stage5JsonValue $details 'reviewedCommit' "$context details"
            $rounds = Get-Stage5JsonValue $details 'reviewRounds' "$context details"
            $reviewers = Get-Stage5JsonValue $details 'independentReviewers' "$context details"
            $openP0 = Get-Stage5JsonValue $details 'openP0' "$context details"
            $openP1 = Get-Stage5JsonValue $details 'openP1' "$context details"
            $openP2 = Get-Stage5JsonValue $details 'openP2' "$context details"
            Assert-Stage5Condition ($reviewedCommit -is [string] -and
                $reviewedCommit -ceq $ExpectedSourceCommit -and
                (Test-Stage5JsonInteger $rounds) -and [Int64]$rounds -gt 0 -and
                (Test-Stage5JsonInteger $reviewers) -and [Int64]$reviewers -gt 0 -and
                (Test-Stage5JsonInteger $openP0) -and [Int64]$openP0 -eq 0 -and
                (Test-Stage5JsonInteger $openP1) -and [Int64]$openP1 -eq 0 -and
                (Test-Stage5JsonInteger $openP2) -and [Int64]$openP2 -eq 0) `
                "$context role details are not semantically valid."
        }
        'manual-checklist' {
            Assert-Stage5Condition ((Get-Stage5JsonValue $details 'approvalScope' "$context details") -ceq
                'final-stage5-installed-runtime' -and
                (Get-Stage5JsonValue $details 'candidateHashVerified' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'candidateHashVerified' "$context details") -and
                (Get-Stage5JsonValue $details 'bothTitlesTested' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'bothTitlesTested' "$context details") -and
                (Get-Stage5JsonValue $details 'cleanExitPassed' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'cleanExitPassed' "$context details")) `
                "$context role details are not semantically valid."
        }
    }
    if ($null -ne $SeenRunNonces) {
        if ($SeenRunNonces.Contains($runNonce)) {
            throw "replayed: $context reuses runNonce '$runNonce' already bound to '$($SeenRunNonces[$runNonce])'."
        }
        $SeenRunNonces[$runNonce] = "$Kind/$Role"
    }
    return [pscustomobject]@{
        role = $Role
        runNonce = $runNonce
        producer = $producer
        producerVersion = $producerVersion
        rawLogs = $validatedRawLogs.ToArray()
        acceptanceFailure = "missing-producer: $context is structurally valid but no executable-originated producer is registered. Expected '$($contract.producer)' version $($contract.producerVersion); current producer '$($contract.currentProducer)' does not emit immutable run-nonce receipts."
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
        [string]$ExpectedZeroHourExecutableSha256,
        [UInt32]$ExpectedGeneralsBuildCompatibilityCrc = 0,
        [UInt32]$ExpectedZeroHourBuildCompatibilityCrc = 0,
        [UInt32]$ExpectedGeneralsContentCrc = 0,
        [UInt32]$ExpectedZeroHourContentCrc = 0
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
    $documentNames = @('schemaVersion', 'evidenceKind', 'status', 'producer',
        'validationMode', 'installedRuntime', 'independentProcessHashing',
        'sourceCommit', 'artifactSetSha256', 'supportedKernelMask',
        'policySchema', 'engineEpoch', 'determinismEpoch',
        'buildCompatibilityCrc', 'contentCrc', 'executables', 'fixedSeeds', 'matches')
    Assert-Stage5JsonShape $document $documentNames 'Installed NET3 loopback evidence'
    $schemaVersion = Get-Stage5JsonValue $document 'schemaVersion' 'Installed NET3 loopback evidence'
    $kind = Get-Stage5JsonValue $document 'evidenceKind' 'Installed NET3 loopback evidence'
    $status = Get-Stage5JsonValue $document 'status' 'Installed NET3 loopback evidence'
    $producer = Get-Stage5JsonValue $document 'producer' 'Installed NET3 loopback evidence'
    $validationMode = Get-Stage5JsonValue $document 'validationMode' 'Installed NET3 loopback evidence'
    $sourceCommit = Get-Stage5JsonValue $document 'sourceCommit' 'Installed NET3 loopback evidence'
    $artifactHash = Get-Stage5JsonValue $document 'artifactSetSha256' 'Installed NET3 loopback evidence'
    $kernelMask = Get-Stage5JsonValue $document 'supportedKernelMask' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $kind -is [string] -and $kind -ceq 'installed-net3-loopback' -and
        $status -is [string] -and $status -ceq 'passed' -and
        $producer -is [string] -and $producer -ceq 'installed-runtime-runner-v1' -and
        $validationMode -is [string] -and
        $validationMode -ceq 'scoped-net3-loopback-release-proof') `
        'Installed NET3 loopback evidence identity/status is invalid.'
    Assert-Stage5FinalAcceptanceBoolean `
        (Get-Stage5JsonValue $document 'installedRuntime' 'Installed NET3 loopback evidence') `
        'Installed NET3 loopback evidence installedRuntime'
    Assert-Stage5FinalAcceptanceBoolean `
        (Get-Stage5JsonValue $document 'independentProcessHashing' 'Installed NET3 loopback evidence') `
        'Installed NET3 loopback evidence independentProcessHashing'
    Assert-Stage5Condition ($sourceCommit -is [string] -and
        $sourceCommit -ceq $ExpectedSourceCommit) `
        'Installed NET3 loopback evidence source commit does not match independent provenance.'
    Assert-Stage5Condition ($artifactHash -is [string] -and
        $artifactHash -ceq $ExpectedArtifactSetSha256) `
        'Installed NET3 loopback evidence artifact-set SHA-256 does not match independent provenance.'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $kernelMask) -and [UInt64]$kernelMask -eq 0x3F) `
        'Installed NET3 loopback evidence must advertise exactly the integrated kernel mask 0x3F.'

    foreach ($epochBinding in @(
        @('policySchema', 1), @('engineEpoch', 1), @('determinismEpoch', 1)
    )) {
        $epochValue = Get-Stage5JsonValue $document $epochBinding[0] `
            'Installed NET3 loopback evidence'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $epochValue) -and
            [UInt64]$epochValue -eq [UInt64]$epochBinding[1]) `
            "Installed NET3 loopback evidence $($epochBinding[0]) is incompatible."
    }

    $buildCrcs = Get-Stage5JsonValue $document 'buildCompatibilityCrc' `
        'Installed NET3 loopback evidence'
    $contentCrcs = Get-Stage5JsonValue $document 'contentCrc' `
        'Installed NET3 loopback evidence'
    Assert-Stage5JsonShape $buildCrcs @('Generals', 'ZeroHour') `
        'Installed NET3 loopback build compatibility CRCs'
    Assert-Stage5JsonShape $contentCrcs @('Generals', 'ZeroHour') `
        'Installed NET3 loopback content CRCs'
    $validatedBuildCrcs = @{}
    $validatedContentCrcs = @{}
    $expectedBuildCrcs = @{
        Generals = $ExpectedGeneralsBuildCompatibilityCrc
        ZeroHour = $ExpectedZeroHourBuildCompatibilityCrc
    }
    $expectedContentCrcs = @{
        Generals = $ExpectedGeneralsContentCrc
        ZeroHour = $ExpectedZeroHourContentCrc
    }
    foreach ($title in @('Generals', 'ZeroHour')) {
        $buildCrc = Get-Stage5JsonValue $buildCrcs $title `
            'Installed NET3 loopback build compatibility CRCs'
        $contentCrc = Get-Stage5JsonValue $contentCrcs $title `
            'Installed NET3 loopback content CRCs'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $buildCrc) -and
            [UInt64]$buildCrc -gt 0 -and [UInt64]$buildCrc -le [UInt32]::MaxValue -and
            (Test-Stage5JsonInteger $contentCrc) -and
            [UInt64]$contentCrc -gt 0 -and [UInt64]$contentCrc -le [UInt32]::MaxValue) `
            "Installed NET3 loopback evidence has invalid build/content CRC for $title."
        if ($expectedBuildCrcs[$title] -ne 0) {
            Assert-Stage5Condition ([UInt32]$buildCrc -eq $expectedBuildCrcs[$title]) `
                "Installed NET3 loopback build CRC for $title does not match independent provenance."
        }
        if ($expectedContentCrcs[$title] -ne 0) {
            Assert-Stage5Condition ([UInt32]$contentCrc -eq $expectedContentCrcs[$title]) `
                "Installed NET3 loopback content CRC for $title does not match independent provenance."
        }
        $validatedBuildCrcs[$title] = [UInt32]$buildCrc
        $validatedContentCrcs[$title] = [UInt32]$contentCrc
    }

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
    $observedRawOutputPaths = @{}
	$validatedRawOutputs = @()
	[UInt64]$provenKernelMask = 0
    $topologyCaseIndices = @{
        'two-peer-1-v-16' = 0
        'two-peer-2-v-auto' = 1
        'two-peer-4-v-8' = 2
        'four-peer-mixed-workers' = 3
    }
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
                    Assert-Stage5JsonShape $peer @('ordinal', 'processId',
                        'observedExecutableSha256', 'observedArtifactSetSha256',
                        'rawOutputPath', 'rawOutputSha256', 'requestedWorkers',
                        'effectiveWorkers', 'networkHelloReady', 'rosterExact', 'rosterSha256',
                        'policyMask', 'finalFrame', 'finalCRC', 'exitCode', 'cleanShutdown', 'kernels') `
                        $peerContext
                    $ordinal = Get-Stage5JsonValue $peer 'ordinal' $peerContext
                    $processId = Get-Stage5JsonValue $peer 'processId' $peerContext
                    $observedExecutableHash = Get-Stage5JsonValue $peer `
                        'observedExecutableSha256' $peerContext
                    $observedArtifactHash = Get-Stage5JsonValue $peer `
                        'observedArtifactSetSha256' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $processId) -and
                        [UInt64]$processId -gt 0 -and
                        $observedExecutableHash -is [string] -and
                        $observedExecutableHash -ceq $expectedExecutableHashes[$title] -and
                        $observedArtifactHash -is [string] -and
                        $observedArtifactHash -ceq $ExpectedArtifactSetSha256) `
                        "$peerContext lacks an independent exact process/artifact hash observation."
                    $rawOutputPath = Get-Stage5JsonValue $peer 'rawOutputPath' $peerContext
                    $rawOutputSha = Get-Stage5JsonValue $peer 'rawOutputSha256' $peerContext
                    Assert-Stage5Condition ($rawOutputPath -is [string] -and
                        $rawOutputPath.StartsWith('Net3Raw\', [StringComparison]::Ordinal) -and
                        -not [IO.Path]::IsPathRooted($rawOutputPath) -and
                        -not $rawOutputPath.Contains(':') -and
                        -not $rawOutputPath.Contains('..') -and
                        -not $observedRawOutputPaths.ContainsKey($rawOutputPath)) `
                        "$peerContext raw output path is unsafe, duplicated, or outside Net3Raw."
                    $observedRawOutputPaths[$rawOutputPath] = $true
                    $rawOutputFull = Resolve-Stage5FinalAcceptanceFile `
                        (Split-Path -Parent $full) $rawOutputPath "$peerContext raw output"
                    Assert-Stage5FinalAcceptanceSha256 $rawOutputFull $rawOutputSha `
                        "$peerContext raw output" | Out-Null
                    $validatedRawOutputs += [pscustomobject]@{
                        path = $rawOutputPath
                        sha256 = ([string]$rawOutputSha).ToUpperInvariant()
                    }
                    $rawRecord = ConvertFrom-Stage5JsonDictionary $rawOutputFull
                    Assert-Stage5JsonShape $rawRecord @('schemaVersion', 'producer',
                        'validationMode', 'kernelFixture', 'processId', 'title', 'caseIndex', 'seed',
                        'ordinal', 'peerCount', 'sourceCommit', 'executableSha256',
                        'artifactSetSha256', 'buildCompatibilityCrc', 'contentCrc',
                        'requestedWorkers', 'effectiveWorkers', 'networkHelloReady',
                        'rosterExact', 'rosterSha256', 'policyMask', 'finalFrame',
                        'finalCRC', 'cleanShutdown', 'kernels') "$peerContext raw output"
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
                    Assert-Stage5Condition (
                        (Get-Stage5JsonValue $rawRecord 'schemaVersion' "$peerContext raw output") -eq 1 -and
                        (Get-Stage5JsonValue $rawRecord 'producer' "$peerContext raw output") -ceq
                            'installed-runtime-net3-peer-v1' -and
                        (Get-Stage5JsonValue $rawRecord 'validationMode' "$peerContext raw output") -ceq
                            'scoped-net3-loopback-release-proof' -and
                        (Get-Stage5JsonValue $rawRecord 'kernelFixture' "$peerContext raw output") -ceq
                            'actual-stage5-kernels-v1' -and
                        (Get-Stage5JsonValue $rawRecord 'processId' "$peerContext raw output") -eq $processId -and
                        (Get-Stage5JsonValue $rawRecord 'title' "$peerContext raw output") -ceq $title -and
                        (Get-Stage5JsonValue $rawRecord 'caseIndex' "$peerContext raw output") -eq
                            $topologyCaseIndices[$topology.id] -and
                        (Get-Stage5JsonValue $rawRecord 'seed' "$peerContext raw output") -eq $seed -and
                        (Get-Stage5JsonValue $rawRecord 'ordinal' "$peerContext raw output") -eq $peerIndex -and
                        (Get-Stage5JsonValue $rawRecord 'peerCount' "$peerContext raw output") -eq
                            $topology.workers.Count -and
                        (Get-Stage5JsonValue $rawRecord 'sourceCommit' "$peerContext raw output") -ceq
                            $ExpectedSourceCommit -and
                        (Get-Stage5JsonValue $rawRecord 'executableSha256' "$peerContext raw output") -ceq
                            $expectedExecutableHashes[$title] -and
                        (Get-Stage5JsonValue $rawRecord 'artifactSetSha256' "$peerContext raw output") -ceq
                            $ExpectedArtifactSetSha256 -and
                        (Get-Stage5JsonValue $rawRecord 'buildCompatibilityCrc' "$peerContext raw output") -eq
                            $validatedBuildCrcs[$title] -and
                        (Get-Stage5JsonValue $rawRecord 'contentCrc' "$peerContext raw output") -eq
                            $validatedContentCrcs[$title] -and
                        (Get-Stage5JsonValue $rawRecord 'requestedWorkers' "$peerContext raw output") -ceq
                            $requestedWorkers -and
                        (Get-Stage5JsonValue $rawRecord 'effectiveWorkers' "$peerContext raw output") -eq
                            $effectiveWorkers -and
                        (Get-Stage5JsonValue $rawRecord 'networkHelloReady' "$peerContext raw output") -eq $true -and
                        (Get-Stage5JsonValue $rawRecord 'rosterExact' "$peerContext raw output") -eq $true -and
                        (Get-Stage5JsonValue $rawRecord 'rosterSha256' "$peerContext raw output") -ceq
                            $rosterHash -and
                        (Get-Stage5JsonValue $rawRecord 'policyMask' "$peerContext raw output") -eq 63 -and
                        (Get-Stage5JsonValue $rawRecord 'finalFrame' "$peerContext raw output") -eq $finalFrame -and
                        (Get-Stage5JsonValue $rawRecord 'finalCRC' "$peerContext raw output") -ceq $finalCRC -and
                        (Get-Stage5JsonValue $rawRecord 'cleanShutdown' "$peerContext raw output") -eq $true) `
                        "$peerContext raw peer record does not match the independently observed evidence."
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
                    $rawKernels = Get-Stage5JsonValue $rawRecord 'kernels' "$peerContext raw output"
                    Assert-Stage5Condition ($kernels -is [Array] -and $kernels.Count -eq 6) `
                        "$peerContext must contain exactly six kernel records."
                    Assert-Stage5Condition ($rawKernels -is [Array] -and $rawKernels.Count -eq 6) `
                        "$peerContext raw output must contain exactly six kernel records."
                    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
                        $kernel = $kernels[$kernelIndex]
                        $kernelContext = "$peerContext kernel $kernelIndex"
						Assert-Stage5JsonShape $kernel @('name', 'bit', 'submitted', 'completed',
							'physicalWorkerJobs', 'ownerHelpedJobs', 'physicalWorkerMask',
							'distinctPhysicalWorkers', 'physicalWorkerMaskComplete',
							'peakConcurrentPhysicalWorkers') $kernelContext
                        Assert-Stage5Condition ((Get-Stage5JsonValue $kernel 'name' $kernelContext) `
                            -ceq $kernelNames[$kernelIndex] -and
                            (Get-Stage5JsonValue $kernel 'bit' $kernelContext) -eq $kernelBits[$kernelIndex]) `
                            "$kernelContext name/bit is not canonical."
                        $submitted = Get-Stage5JsonValue $kernel 'submitted' $kernelContext
                        $completed = Get-Stage5JsonValue $kernel 'completed' $kernelContext
                        $physical = Get-Stage5JsonValue $kernel 'physicalWorkerJobs' $kernelContext
                        $ownerHelped = Get-Stage5JsonValue $kernel 'ownerHelpedJobs' $kernelContext
                        $physicalMask = Get-Stage5JsonValue $kernel 'physicalWorkerMask' $kernelContext
						$distinct = Get-Stage5JsonValue $kernel 'distinctPhysicalWorkers' $kernelContext
						$maskComplete = Get-Stage5JsonValue $kernel 'physicalWorkerMaskComplete' $kernelContext
						$peak = Get-Stage5JsonValue $kernel 'peakConcurrentPhysicalWorkers' $kernelContext
                        $rawKernel = $rawKernels[$kernelIndex]
                        Assert-Stage5JsonShape $rawKernel @('name', 'bit', 'submitted',
                            'completed', 'physicalWorkerJobs', 'ownerHelpedJobs',
							'physicalWorkerMask', 'distinctPhysicalWorkers',
							'physicalWorkerMaskComplete', 'peakConcurrentPhysicalWorkers') `
                            "$kernelContext raw output"
                        Assert-Stage5Condition (
                            (Get-Stage5JsonValue $rawKernel 'name' "$kernelContext raw output") -ceq
                                (Get-Stage5JsonValue $kernel 'name' $kernelContext) -and
                            (Get-Stage5JsonValue $rawKernel 'bit' "$kernelContext raw output") -eq
                                (Get-Stage5JsonValue $kernel 'bit' $kernelContext) -and
                            (Get-Stage5JsonValue $rawKernel 'submitted' "$kernelContext raw output") -eq $submitted -and
                            (Get-Stage5JsonValue $rawKernel 'completed' "$kernelContext raw output") -eq $completed -and
                            (Get-Stage5JsonValue $rawKernel 'physicalWorkerJobs' "$kernelContext raw output") -eq $physical -and
                            (Get-Stage5JsonValue $rawKernel 'ownerHelpedJobs' "$kernelContext raw output") -eq $ownerHelped -and
                            (Get-Stage5JsonValue $rawKernel 'physicalWorkerMask' "$kernelContext raw output") -eq $physicalMask -and
							(Get-Stage5JsonValue $rawKernel 'distinctPhysicalWorkers' "$kernelContext raw output") -eq $distinct -and
							(Get-Stage5JsonValue $rawKernel 'physicalWorkerMaskComplete' "$kernelContext raw output") -eq $maskComplete -and
							(Get-Stage5JsonValue $rawKernel 'peakConcurrentPhysicalWorkers' "$kernelContext raw output") -eq $peak) `
                            "$kernelContext raw counters do not match the accepted peer evidence."
                        foreach ($counter in @($submitted, $completed, $physical, $ownerHelped,
                            $physicalMask, $distinct, $peak)) {
                            Assert-Stage5Condition ((Test-Stage5JsonInteger $counter) -and
                                [Int64]$counter -ge 0) "$kernelContext counters must be nonnegative integers."
                        }
                        Assert-Stage5Condition ([UInt64]$submitted -eq [UInt64]$completed) `
                            "$kernelContext submitted/completed jobs differ."
                        Assert-Stage5Condition ([UInt64]$physical + [UInt64]$ownerHelped -eq
                            [UInt64]$completed) `
                            "$kernelContext physical/owner execution accounting differs from completed jobs."
						Assert-Stage5Condition ($maskComplete -is [bool]) `
							"$kernelContext physicalWorkerMaskComplete must be a boolean."
						Assert-Stage5Condition ([UInt64]$distinct -ge
							(Get-Stage5UInt64BitCount ([UInt64]$physicalMask))) `
							"$kernelContext physical-worker mask exceeds the explicit distinct count."
						if ($maskComplete) {
							Assert-Stage5Condition ([UInt64]$distinct -eq
								(Get-Stage5UInt64BitCount ([UInt64]$physicalMask))) `
								"$kernelContext complete physical-worker mask/count is inconsistent."
						}
                        if ([int]$effectiveWorkers -eq 1) {
                            Assert-Stage5Condition ([UInt64]$submitted -eq 0 -and
                                [UInt64]$physical -eq 0 -and [UInt64]$ownerHelped -eq 0 -and
                                [UInt64]$physicalMask -eq 0 -and [UInt64]$distinct -eq 0 -and
                                [UInt64]$peak -eq 0) `
                                "$kernelContext forced-one evidence must report zero physical work."
                        }
						else {
							Assert-Stage5Condition ([UInt64]$submitted -gt 0 -and
								[UInt64]$physical -gt 0 -and [UInt64]$physical -eq [UInt64]$completed -and
								[UInt64]$ownerHelped -eq 0 -and [UInt64]$distinct -gt 1 -and
                                [UInt64]$distinct -le [UInt64]$effectiveWorkers -and
                                [UInt64]$peak -gt 1 -and [UInt64]$peak -le [UInt64]$effectiveWorkers) `
								"$kernelContext does not prove concurrent work on more than one physical worker."
							$provenKernelMask = $provenKernelMask -bor [UInt64]$kernelBits[$kernelIndex]
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
	Assert-Stage5Condition ($provenKernelMask -eq [UInt64]0x3F) `
		'Installed NET3 loopback evidence did not prove every advertised live kernel.'
    return [pscustomobject]@{
        schemaVersion = 1
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        evidenceManifestSha256 = Get-Stage5FinalAcceptanceFileSha256 $full
        generalsExecutableSha256 = $ExpectedGeneralsExecutableSha256
        zeroHourExecutableSha256 = $ExpectedZeroHourExecutableSha256
        generalsBuildCompatibilityCrc = $validatedBuildCrcs.Generals
        zeroHourBuildCompatibilityCrc = $validatedBuildCrcs.ZeroHour
        generalsContentCrc = $validatedContentCrcs.Generals
        zeroHourContentCrc = $validatedContentCrcs.ZeroHour
		provenKernelMask = $provenKernelMask
        matchCount = 16
        peerRecordCount = 40
        rawEvidenceEntries = $validatedRawOutputs
    }
}

function Get-Stage5ScalingRunCommand {
    param([string]$Title, [string]$Fixture, [string]$Lane,
        [string]$ExecutableSha256)
    $executable = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
    $workerCount = switch ($Lane) {
        'stage3-forced-one' { 1 }
        'forced-one' { 1 }
        'physical-8' { 8 }
        'physical-16' { 16 }
        default { throw "Unsupported Stage 5 scaling lane '$Lane'." }
    }
    $replayArgument = "Stage5Scaling\$Fixture.rep"
    return "$executable -headless -noFPSLimit -pipelineMode serial -simulationMode parallel -workerPolicy auto -validationExecutableSha256 $ExecutableSha256 -workerCount $workerCount -replay $replayArgument"
}

function Read-Stage5PerformanceScalingTopologyReceipt {
    param([string]$Path, [string]$ExpectedSourceCommit,
        [string]$ExpectedExecutableSha256, [string]$ExpectedTitle)
    $document = ConvertFrom-Stage5JsonDictionary $Path
    Assert-Stage5JsonShape $document @('schemaVersion', 'producer', 'source',
        'sourceCommit', 'executableSha256', 'processId', 'commandLine',
        'logicalProcessors', 'selectedLanes') 'Stage 5 scaling topology receipt'
    $processId = Get-Stage5JsonValue $document 'processId' 'Stage 5 scaling topology receipt'
    $expectedCommand = Get-Stage5ScalingRunCommand $ExpectedTitle `
        'one-thousand-units' 'forced-one' $ExpectedExecutableSha256
    Assert-Stage5Condition ((Get-Stage5JsonValue $document 'schemaVersion' 'Stage 5 scaling topology receipt') -eq 1 -and
        (Get-Stage5JsonValue $document 'producer' 'Stage 5 scaling topology receipt') -ceq
            'installed-runtime-scaling-runner-v1' -and
        (Get-Stage5JsonValue $document 'source' 'Stage 5 scaling topology receipt') -ceq
            'GetSystemCpuSetInformation' -and
        (Get-Stage5JsonValue $document 'sourceCommit' 'Stage 5 scaling topology receipt') -ceq
            $ExpectedSourceCommit -and
        (Get-Stage5JsonValue $document 'executableSha256' 'Stage 5 scaling topology receipt') -ceq
            $ExpectedExecutableSha256 -and
        (Test-Stage5JsonInteger $processId) -and [Int64]$processId -gt 0 -and
        (Get-Stage5JsonValue $document 'commandLine' 'Stage 5 scaling topology receipt') -ceq
            $expectedCommand) `
        'Stage 5 scaling topology receipt is not bound to the exact installed executable command.'

    $logicalProcessors = Get-Stage5JsonValue $document 'logicalProcessors' `
        'Stage 5 scaling topology receipt'
    Assert-Stage5Condition ($logicalProcessors -is [Array] -and $logicalProcessors.Count -ge 16) `
        'Stage 5 scaling topology receipt requires at least 16 logical processor rows.'
    $physicalByLogical = @{}
    $physicalCores = @{}
    for ($index = 0; $index -lt $logicalProcessors.Count; ++$index) {
        $logical = $logicalProcessors[$index]
        $context = "Stage 5 scaling topology logical processor $index"
        Assert-Stage5JsonShape $logical @('logicalProcessorIndex', 'physicalCoreIndex') $context
        $logicalIndex = Get-Stage5JsonValue $logical 'logicalProcessorIndex' $context
        $physicalIndex = Get-Stage5JsonValue $logical 'physicalCoreIndex' $context
        Assert-Stage5Condition ((Test-Stage5JsonInteger $logicalIndex) -and
            [int]$logicalIndex -eq $index -and (Test-Stage5JsonInteger $physicalIndex) -and
            [int]$physicalIndex -ge 0 -and [int]$physicalIndex -lt 64) `
            "$context is not a canonical CPU-set mapping."
        $physicalByLogical[$index] = [int]$physicalIndex
        $physicalCores[[int]$physicalIndex] = $true
    }
    Assert-Stage5Condition ($physicalCores.Count -ge 16) `
        'Stage 5 scaling topology receipt does not contain 16 distinct physical cores.'

    $rawLanes = Get-Stage5JsonValue $document 'selectedLanes' 'Stage 5 scaling topology receipt'
    $laneNames = @('forced-one', 'physical-8', 'physical-16')
    $laneWorkers = @(1, 8, 16)
    Assert-Stage5Condition ($rawLanes -is [Array] -and $rawLanes.Count -eq 3) `
        'Stage 5 scaling topology receipt requires exactly three selected lanes.'
    $lanes = @()
    for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
        $lane = $rawLanes[$laneIndex]
        $context = "Stage 5 scaling topology lane $laneIndex"
        Assert-Stage5JsonShape $lane @('name', 'requestedWorkers',
            'selectedLogicalProcessorIndices') $context
        $selected = Get-Stage5JsonValue $lane 'selectedLogicalProcessorIndices' $context
        Assert-Stage5Condition ((Get-Stage5JsonValue $lane 'name' $context) -ceq
            $laneNames[$laneIndex] -and
            (Get-Stage5JsonValue $lane 'requestedWorkers' $context) -eq $laneWorkers[$laneIndex] -and
            $selected -is [Array] -and $selected.Count -eq $laneWorkers[$laneIndex]) `
            "$context does not identify the exact requested physical lane."
        $seenLogical = @{}
        $seenPhysical = @{}
        [UInt64]$physicalMask = 0
        foreach ($selectedIndex in $selected) {
            Assert-Stage5Condition ((Test-Stage5JsonInteger $selectedIndex) -and
                [int]$selectedIndex -ge 0 -and
                [int]$selectedIndex -lt $logicalProcessors.Count -and
                -not $seenLogical.ContainsKey([int]$selectedIndex)) `
                "$context contains a duplicate or unavailable logical processor."
            $seenLogical[[int]$selectedIndex] = $true
            $physicalIndex = [int]$physicalByLogical[[int]$selectedIndex]
            Assert-Stage5Condition (-not $seenPhysical.ContainsKey($physicalIndex)) `
                "$context selects sibling logical processors from one physical core."
            $seenPhysical[$physicalIndex] = $true
            $physicalMask = $physicalMask -bor ([UInt64]1 -shl $physicalIndex)
        }
        Assert-Stage5Condition ($seenPhysical.Count -eq $laneWorkers[$laneIndex]) `
            "$context does not select the exact distinct physical-core count."
        $lanes += [pscustomobject]@{
            name = $laneNames[$laneIndex]
            requestedWorkers = $laneWorkers[$laneIndex]
            selectedLogicalProcessors = $selected.Count
            selectedDistinctPhysicalCores = $seenPhysical.Count
            selectedPhysicalCoreMask = $physicalMask.ToString('X16')
        }
    }
    return [pscustomobject]@{
        processId = [Int64]$processId
        commandLine = $expectedCommand
        physicalCoreCount = $physicalCores.Count
        logicalProcessorCount = $logicalProcessors.Count
        selectedLanes = $lanes
    }
}

function Read-Stage5PerformanceScalingRawSamples {
    param([string]$Path, [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256, [string]$ExpectedExecutableSha256,
        [string]$ExpectedStage3BaselineSha256, [string]$ExpectedTitle)
    $full = [IO.Path]::GetFullPath($Path)
    $document = ConvertFrom-Stage5JsonDictionary $full
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind', 'producer',
        'sourceCommit', 'artifactSetSha256', 'title', 'executableSha256',
        'stage3SourceCommit', 'stage3ExecutableSha256', 'stage3BaselineSha256',
        'measurementMode', 'installedRuntime', 'topologyReceipt', 'fixtureSamples',
        'phaseSamples', 'kernelSamples') 'Stage 5 scaling raw-sample manifest'
    $stage3SourceCommit = Get-Stage5JsonValue $document 'stage3SourceCommit' `
        'Stage 5 scaling raw-sample manifest'
    $stage3ExecutableSha256 = Get-Stage5JsonValue $document 'stage3ExecutableSha256' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ((Get-Stage5JsonValue $document 'schemaVersion' 'Stage 5 scaling raw-sample manifest') -eq 1 -and
        (Get-Stage5JsonValue $document 'evidenceKind' 'Stage 5 scaling raw-sample manifest') -ceq
            'stage5-performance-scaling-raw-samples' -and
        (Get-Stage5JsonValue $document 'producer' 'Stage 5 scaling raw-sample manifest') -ceq
            'installed-runtime-scaling-runner-v1' -and
        (Get-Stage5JsonValue $document 'sourceCommit' 'Stage 5 scaling raw-sample manifest') -ceq
            $ExpectedSourceCommit -and
        (Get-Stage5JsonValue $document 'artifactSetSha256' 'Stage 5 scaling raw-sample manifest') -ceq
            $ExpectedArtifactSetSha256 -and
        (Get-Stage5JsonValue $document 'title' 'Stage 5 scaling raw-sample manifest') -ceq
            $ExpectedTitle -and
        (Get-Stage5JsonValue $document 'executableSha256' 'Stage 5 scaling raw-sample manifest') -ceq
            $ExpectedExecutableSha256 -and
        $stage3SourceCommit -is [string] -and $stage3SourceCommit -match '^[0-9a-f]{40}$' -and
        $stage3ExecutableSha256 -is [string] -and $stage3ExecutableSha256 -match '^[0-9A-F]{64}$' -and
        (Get-Stage5JsonValue $document 'stage3BaselineSha256' 'Stage 5 scaling raw-sample manifest') -ceq
            $ExpectedStage3BaselineSha256 -and
        (Get-Stage5JsonValue $document 'measurementMode' 'Stage 5 scaling raw-sample manifest') -ceq
            'headless-throughput' -and
        (Get-Stage5JsonValue $document 'installedRuntime' 'Stage 5 scaling raw-sample manifest') -eq $true) `
        'Stage 5 scaling raw-sample manifest provenance is invalid.'

    $topologyEntry = Get-Stage5JsonValue $document 'topologyReceipt' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $topologyEntry @('path', 'sha256') `
        'Stage 5 scaling topology receipt reference'
    $topologyPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        (Get-Stage5JsonValue $topologyEntry 'path' 'Stage 5 scaling topology receipt reference') `
        'Stage 5 scaling topology receipt'
    $topologySha256 = Assert-Stage5FinalAcceptanceSha256 $topologyPath `
        (Get-Stage5JsonValue $topologyEntry 'sha256' 'Stage 5 scaling topology receipt reference') `
        'Stage 5 scaling topology receipt'
    $topology = Read-Stage5PerformanceScalingTopologyReceipt $topologyPath `
        $ExpectedSourceCommit $ExpectedExecutableSha256 $ExpectedTitle

    $fixtureNames = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $minimumUnits = @(1000, 4000, 8000, 8000)
    $laneNames = @('stage3-forced-one', 'forced-one', 'physical-8', 'physical-16')
    $fixtureSamples = Get-Stage5JsonValue $document 'fixtureSamples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($fixtureSamples -is [Array] -and
        $fixtureSamples.Count -ge 48 -and $fixtureSamples.Count % 16 -eq 0) `
        'Stage 5 scaling raw samples require equal per-repeat rows for four fixtures and four lanes.'
    $repeatCount = [int]($fixtureSamples.Count / 16)
    Assert-Stage5Condition ($repeatCount -ge 3) `
        'Stage 5 scaling raw samples require at least three repeats per fixture lane.'
    $seenProcesses = @{}
    $runReceipts = @{}
    $fixtureAggregates = @()
    $rowIndex = 0
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $laneValues = @{}
        foreach ($laneName in $laneNames) { $laneValues[$laneName] = @() }
        $peakUnitCount = $null
        for ($laneIndex = 0; $laneIndex -lt 4; ++$laneIndex) {
            for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
                $sample = $fixtureSamples[$rowIndex++]
                $context = "Stage 5 scaling fixture raw sample $($rowIndex - 1)"
                Assert-Stage5JsonShape $sample @('fixture', 'playerCount', 'peakUnitCount',
                    'lane', 'repeat', 'processId', 'executableSha256', 'commandLine',
                    'elapsedMilliseconds') $context
                $units = Get-Stage5JsonValue $sample 'peakUnitCount' $context
                $processId = Get-Stage5JsonValue $sample 'processId' $context
                $elapsed = Get-Stage5JsonValue $sample 'elapsedMilliseconds' $context
                $expectedHash = if ($laneIndex -eq 0) {
                    $stage3ExecutableSha256
                } else { $ExpectedExecutableSha256 }
                $expectedCommand = Get-Stage5ScalingRunCommand $ExpectedTitle `
                    $fixtureNames[$fixtureIndex] $laneNames[$laneIndex] $expectedHash
                Assert-Stage5Condition ((Get-Stage5JsonValue $sample 'fixture' $context) -ceq
                    $fixtureNames[$fixtureIndex] -and
                    (Get-Stage5JsonValue $sample 'playerCount' $context) -eq 8 -and
                    (Test-Stage5JsonInteger $units) -and [int]$units -ge $minimumUnits[$fixtureIndex] -and
                    (Get-Stage5JsonValue $sample 'lane' $context) -ceq $laneNames[$laneIndex] -and
                    (Get-Stage5JsonValue $sample 'repeat' $context) -eq $repeat -and
                    (Test-Stage5JsonInteger $processId) -and [Int64]$processId -gt 0 -and
                    -not $seenProcesses.ContainsKey([string]$processId) -and
                    (Get-Stage5JsonValue $sample 'executableSha256' $context) -ceq $expectedHash -and
                    (Get-Stage5JsonValue $sample 'commandLine' $context) -ceq $expectedCommand -and
                    (Test-Stage5JsonNumber $elapsed) -and [double]$elapsed -gt 0.0) `
                    "$context is not an exact installed per-process timing receipt."
                if ($null -eq $peakUnitCount) { $peakUnitCount = [int]$units }
                Assert-Stage5Condition ([int]$units -eq $peakUnitCount) `
                    "$context changes peak unit count within one fixture."
                $seenProcesses[[string]$processId] = $true
                $runReceipts["$($fixtureNames[$fixtureIndex])|$($laneNames[$laneIndex])|$repeat"] =
                    [pscustomobject]@{ processId = [Int64]$processId; commandLine = $expectedCommand }
                $laneValues[$laneNames[$laneIndex]] += [double]$elapsed
            }
        }
        $stage3 = Get-Stage5Median $laneValues['stage3-forced-one']
        $stage5 = Get-Stage5Median $laneValues['forced-one']
        $eight = Get-Stage5Median $laneValues['physical-8']
        $sixteen = Get-Stage5Median $laneValues['physical-16']
        $fixtureAggregates += [pscustomobject]@{
            name = $fixtureNames[$fixtureIndex]
            playerCount = 8
            peakUnitCount = $peakUnitCount
            repeats = $repeatCount
            stage3OneWorkerMilliseconds = $stage3
            stage5OneWorkerMilliseconds = $stage5
            eightPhysicalCoreMilliseconds = $eight
            sixteenPhysicalCoreMilliseconds = $sixteen
            oneWorkerRegressionRatio = $stage5 / $stage3
            eightPhysicalCoreSpeedup = $stage5 / $eight
            eightToSixteenSpeedup = $eight / $sixteen
        }
    }

    $topologyRunReceipt = $runReceipts['one-thousand-units|forced-one|0']
    Assert-Stage5Condition ($topology.processId -eq $topologyRunReceipt.processId -and
        $topology.commandLine -ceq $topologyRunReceipt.commandLine) `
        'Stage 5 scaling topology receipt is not correlated with its exact installed one-worker run.'

    $phaseNames = @('owner-intake', 'world-queries', 'pathfinding', 'object-computation',
        'spatial-work', 'deterministic-commit', 'verification-publication')
    $phaseSamples = Get-Stage5JsonValue $document 'phaseSamples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($phaseSamples -is [Array] -and
        $phaseSamples.Count -eq $phaseNames.Count * $repeatCount) `
        'Stage 5 scaling raw samples require every one-worker phase for every repeat.'
    $phaseAggregates = @()
    $rowIndex = 0
    foreach ($phaseName in $phaseNames) {
        $elapsedValues = @()
        $serialValues = @()
        for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
            $sample = $phaseSamples[$rowIndex++]
            $context = "Stage 5 scaling phase raw sample $($rowIndex - 1)"
            Assert-Stage5JsonShape $sample @('phase', 'repeat', 'processId', 'commandLine',
                'elapsedMilliseconds', 'serialMilliseconds') $context
            $receipt = $runReceipts["dense-eight-player|forced-one|$repeat"]
            $elapsed = Get-Stage5JsonValue $sample 'elapsedMilliseconds' $context
            $serial = Get-Stage5JsonValue $sample 'serialMilliseconds' $context
            Assert-Stage5Condition ((Get-Stage5JsonValue $sample 'phase' $context) -ceq $phaseName -and
                (Get-Stage5JsonValue $sample 'repeat' $context) -eq $repeat -and
                (Get-Stage5JsonValue $sample 'processId' $context) -eq $receipt.processId -and
                (Get-Stage5JsonValue $sample 'commandLine' $context) -ceq $receipt.commandLine -and
                (Test-Stage5JsonNumber $elapsed) -and [double]$elapsed -gt 0.0 -and
                (Test-Stage5JsonNumber $serial) -and [double]$serial -ge 0.0 -and
                [double]$serial -le [double]$elapsed) `
                "$context is not correlated with its exact installed one-worker run."
            $elapsedValues += [double]$elapsed
            $serialValues += [double]$serial
        }
        $phaseAggregates += [pscustomobject]@{
            name = $phaseName
            elapsedMilliseconds = Get-Stage5Median $elapsedValues
            serialMilliseconds = Get-Stage5Median $serialValues
        }
    }

    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelSamples = Get-Stage5JsonValue $document 'kernelSamples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($kernelSamples -is [Array] -and
        $kernelSamples.Count -eq $kernelNames.Count * $repeatCount) `
        'Stage 5 scaling raw samples require every kernel timing for every repeat.'
    $kernelAggregates = @()
    $parts = @('captureMilliseconds', 'scheduleMilliseconds', 'waitMilliseconds',
        'validateMilliseconds', 'commitMilliseconds')
    $rowIndex = 0
    foreach ($kernelName in $kernelNames) {
        $admittedValues = @()
        $partValues = @{}
        foreach ($part in $parts) { $partValues[$part] = @() }
        $serialValues = @()
        for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
            $sample = $kernelSamples[$rowIndex++]
            $context = "Stage 5 scaling kernel raw sample $($rowIndex - 1)"
            Assert-Stage5JsonShape $sample @('kernel', 'repeat', 'processId', 'commandLine',
                'admittedSlices', 'captureMilliseconds', 'scheduleMilliseconds',
                'waitMilliseconds', 'validateMilliseconds', 'commitMilliseconds',
                'exactSerialOperationMilliseconds') $context
            $receipt = $runReceipts["dense-eight-player|physical-8|$repeat"]
            $admitted = Get-Stage5JsonValue $sample 'admittedSlices' $context
            $serial = Get-Stage5JsonValue $sample 'exactSerialOperationMilliseconds' $context
            Assert-Stage5Condition ((Get-Stage5JsonValue $sample 'kernel' $context) -ceq $kernelName -and
                (Get-Stage5JsonValue $sample 'repeat' $context) -eq $repeat -and
                (Get-Stage5JsonValue $sample 'processId' $context) -eq $receipt.processId -and
                (Get-Stage5JsonValue $sample 'commandLine' $context) -ceq $receipt.commandLine -and
                (Test-Stage5JsonInteger $admitted) -and [int]$admitted -gt 0 -and
                (Test-Stage5JsonNumber $serial) -and [double]$serial -gt 0.0) `
                "$context is not correlated with its exact installed physical-8 run."
            $admittedValues += [double]$admitted
            foreach ($part in $parts) {
                $value = Get-Stage5JsonValue $sample $part $context
                Assert-Stage5Condition ((Test-Stage5JsonNumber $value) -and [double]$value -ge 0.0) `
                    "$context $part is invalid."
                $partValues[$part] += [double]$value
            }
            $serialValues += [double]$serial
        }
        $capture = Get-Stage5Median $partValues.captureMilliseconds
        $schedule = Get-Stage5Median $partValues.scheduleMilliseconds
        $wait = Get-Stage5Median $partValues.waitMilliseconds
        $validate = Get-Stage5Median $partValues.validateMilliseconds
        $commit = Get-Stage5Median $partValues.commitMilliseconds
        $parallelTotal = $capture + $schedule + $wait + $validate + $commit
        $serialOperation = Get-Stage5Median $serialValues
        $kernelAggregates += [pscustomobject]@{
            name = $kernelName
            admittedSlices = [int](Get-Stage5Median $admittedValues)
            captureMilliseconds = $capture
            scheduleMilliseconds = $schedule
            waitMilliseconds = $wait
            validateMilliseconds = $validate
            commitMilliseconds = $commit
            totalParallelMilliseconds = $parallelTotal
            exactSerialOperationMilliseconds = $serialOperation
            netSpeedup = $serialOperation / $parallelTotal
        }
    }
    return [pscustomobject]@{
        manifestSha256 = Get-Stage5FinalAcceptanceFileSha256 $full
        topologySha256 = $topologySha256
        topology = $topology
        repeatCount = $repeatCount
        phases = $phaseAggregates
        kernels = $kernelAggregates
        fixtures = $fixtureAggregates
    }
}

function Read-Stage5PerformanceScalingEvidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedStage3BaselineSha256,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle = 'ZeroHour'
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
        'stage3BaselineSha256', 'rawSampleManifest', 'topology',
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
        (Get-Stage5JsonValue $document 'title' 'Stage 5 scaling evidence') -ceq $ExpectedTitle -and
        (Get-Stage5JsonValue $document 'executableSha256' 'Stage 5 scaling evidence') -ceq
            $ExpectedExecutableSha256 -and
        (Get-Stage5JsonValue $document 'stage3BaselineSha256' 'Stage 5 scaling evidence') -ceq
            $ExpectedStage3BaselineSha256 -and
        (Get-Stage5JsonValue $document 'measurementMode' 'Stage 5 scaling evidence') -ceq
            'headless-throughput' -and
        (Get-Stage5JsonValue $document 'installedRuntime' 'Stage 5 scaling evidence') -is [bool] -and
        (Get-Stage5JsonValue $document 'installedRuntime' 'Stage 5 scaling evidence')) `
        'Stage 5 scaling evidence provenance is invalid.'

    $rawEntry = Get-Stage5JsonValue $document 'rawSampleManifest' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $rawEntry @('path', 'sha256') `
        'Stage 5 scaling raw-sample manifest reference'
    $rawPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        (Get-Stage5JsonValue $rawEntry 'path' 'Stage 5 scaling raw-sample manifest reference') `
        'Stage 5 scaling raw-sample manifest'
    $rawManifestSha256 = Assert-Stage5FinalAcceptanceSha256 $rawPath `
        (Get-Stage5JsonValue $rawEntry 'sha256' 'Stage 5 scaling raw-sample manifest reference') `
        'Stage 5 scaling raw-sample manifest'
    $raw = Read-Stage5PerformanceScalingRawSamples $rawPath $ExpectedSourceCommit `
        $ExpectedArtifactSetSha256 $ExpectedExecutableSha256 `
        $ExpectedStage3BaselineSha256 $ExpectedTitle
    Assert-Stage5Condition ($raw.manifestSha256 -ceq $rawManifestSha256) `
        'Stage 5 scaling raw-sample manifest hash changed during validation.'

    $topology = Get-Stage5JsonValue $document 'topology' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $topology @('source', 'topologySha256', 'physicalCoreCount',
        'logicalProcessorCount') 'Stage 5 scaling topology'
    $physicalCores = Get-Stage5JsonValue $topology 'physicalCoreCount' 'Stage 5 scaling topology'
    $logicalProcessors = Get-Stage5JsonValue $topology 'logicalProcessorCount' 'Stage 5 scaling topology'
    Assert-Stage5Condition ((Get-Stage5JsonValue $topology 'source' 'Stage 5 scaling topology') -ceq
        'GetSystemCpuSetInformation' -and
        (Get-Stage5JsonValue $topology 'topologySha256' 'Stage 5 scaling topology') -ceq
            $raw.topologySha256 -and
        (Test-Stage5JsonInteger $physicalCores) -and
        [int]$physicalCores -eq $raw.topology.physicalCoreCount -and
        (Test-Stage5JsonInteger $logicalProcessors) -and
        [int]$logicalProcessors -eq $raw.topology.logicalProcessorCount) `
        'Stage 5 scaling evidence topology does not match its parsed CPU-set receipt.'

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
        $rawLane = $raw.topology.selectedLanes[$laneIndex]
        Assert-Stage5Condition ($rawLane.name -ceq $laneNames[$laneIndex] -and
            $rawLane.requestedWorkers -eq $laneWorkers[$laneIndex] -and
            $rawLane.selectedLogicalProcessors -eq [int]$logical -and
            $rawLane.selectedDistinctPhysicalCores -eq [int]$distinct -and
            $rawLane.selectedPhysicalCoreMask -ceq $mask) `
            "$context summary does not match the raw CPU-set lane receipt."
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
            [double]$serial -ge 0.0 -and [double]$serial -le [double]$elapsed -and
            [Math]::Abs([double]$elapsed -
                [double]$raw.phases[$phaseIndex].elapsedMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$serial -
                [double]$raw.phases[$phaseIndex].serialMilliseconds) -le 0.0001) `
            "$context timing does not match the raw per-repeat median."
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
        $rawKernel = $raw.kernels[$kernelIndex]
        Assert-Stage5Condition ((Get-Stage5JsonValue $kernel 'name' $context) -ceq
            $kernelNames[$kernelIndex] -and (Test-Stage5JsonInteger $admitted) -and
            [int]$admitted -eq $rawKernel.admittedSlices -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'captureMilliseconds' $context) -
                [double]$rawKernel.captureMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'scheduleMilliseconds' $context) -
                [double]$rawKernel.scheduleMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'waitMilliseconds' $context) -
                [double]$rawKernel.waitMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'validateMilliseconds' $context) -
                [double]$rawKernel.validateMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'commitMilliseconds' $context) -
                [double]$rawKernel.commitMilliseconds) -le 0.0001 -and
            (Test-Stage5JsonNumber $reportedParallel) -and
            [Math]::Abs([double]$reportedParallel - $parallelTotal) -le 0.0001 -and
            [Math]::Abs([double]$reportedParallel -
                [double]$rawKernel.totalParallelMilliseconds) -le 0.0001 -and
            $parallelTotal -gt 0.0 -and (Test-Stage5JsonNumber $serialOperation) -and
            [Math]::Abs([double]$serialOperation -
                [double]$rawKernel.exactSerialOperationMilliseconds) -le 0.0001 -and
            [double]$serialOperation -gt $parallelTotal -and (Test-Stage5JsonNumber $netSpeedup) -and
            [Math]::Abs([double]$netSpeedup - ([double]$serialOperation / $parallelTotal)) -le 0.0001 -and
            [Math]::Abs([double]$netSpeedup - [double]$rawKernel.netSpeedup) -le 0.0001 -and
            [double]$netSpeedup -gt 1.0) `
            "$context does not match raw installed runs or prove positive net speedup."
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
        $rawFixture = $raw.fixtures[$fixtureIndex]
        Assert-Stage5Condition ((Get-Stage5JsonValue $fixture 'name' $context) -ceq
            $fixtureNames[$fixtureIndex] -and (Test-Stage5JsonInteger $players) -and
            [int]$players -eq 8 -and (Test-Stage5JsonInteger $units) -and
            [int]$units -eq $rawFixture.peakUnitCount -and
            [int]$units -ge $minimumUnits[$fixtureIndex] -and
            (Test-Stage5JsonInteger $repeats) -and [int]$repeats -eq $raw.repeatCount -and
            [Math]::Abs([double]$stage3 - [double]$rawFixture.stage3OneWorkerMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$stage5 - [double]$rawFixture.stage5OneWorkerMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$eight - [double]$rawFixture.eightPhysicalCoreMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$sixteen - [double]$rawFixture.sixteenPhysicalCoreMilliseconds) -le 0.0001 -and
            (Test-Stage5JsonNumber $reportedRegression) -and
            [Math]::Abs([double]$reportedRegression - $regression) -le 0.0001 -and
            [Math]::Abs([double]$reportedRegression - [double]$rawFixture.oneWorkerRegressionRatio) -le 0.0001 -and
            $regression -le 1.05 -and (Test-Stage5JsonNumber $reportedSpeedup8) -and
            [Math]::Abs([double]$reportedSpeedup8 - $speedup8) -le 0.0001 -and
            [Math]::Abs([double]$reportedSpeedup8 - [double]$rawFixture.eightPhysicalCoreSpeedup) -le 0.0001 -and
            $speedup8 -ge 2.0 -and (Test-Stage5JsonNumber $reportedScale16) -and
            [Math]::Abs([double]$reportedScale16 - $scale16) -le 0.0001 -and
            [Math]::Abs([double]$reportedScale16 - [double]$rawFixture.eightToSixteenSpeedup) -le 0.0001 -and
            $scale16 -gt 1.0) `
            "$context does not match raw per-repeat medians or meet the scaling gates."
    }
    return [pscustomobject]@{
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        evidenceManifestSha256 = Get-Stage5FinalAcceptanceFileSha256 $full
        rawSampleManifestSha256 = $rawManifestSha256
        executableSha256 = $ExpectedExecutableSha256
        physicalCoreCount = [int]$physicalCores
        fixtureCount = 4
        kernelCount = 6
        eightPhysicalCoreSpeedupFloor = 2.0
    }
}

function ConvertTo-Stage5LockstepReceiptUInt64 {
    param([object]$Value, [string]$Field)
    Assert-Stage5Condition ($Value -is [string] -and $Value -match '^[0-9]+$') `
        "Lockstep-v2 receipt field '$Field' is not an unsigned decimal integer."
    [UInt64]$parsed = 0
    try {
        $parsed = [UInt64]::Parse($Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "Lockstep-v2 receipt field '$Field' is outside the UInt64 range."
    }
    return $parsed
}

function ConvertTo-Stage5LockstepReceiptUInt32 {
    param([object]$Value, [string]$Field)
    $parsed = ConvertTo-Stage5LockstepReceiptUInt64 $Value $Field
    Assert-Stage5Condition ($parsed -le [UInt64]4294967295) `
        "Lockstep-v2 receipt field '$Field' is outside the UInt32 range."
    return [UInt32]$parsed
}

function ConvertTo-Stage5LockstepReceiptBoolean {
    param([object]$Value, [string]$Field)
    Assert-Stage5Condition ($Value -is [string] -and ($Value -ceq '0' -or $Value -ceq '1')) `
        "Lockstep-v2 receipt field '$Field' is not a canonical boolean."
    return $Value -ceq '1'
}

function Get-Stage5LockstepReceiptDigest {
    param([Collections.IDictionary]$Pairs)
    Add-Type -AssemblyName System.Numerics
    [Numerics.BigInteger]$hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    [Numerics.BigInteger]$prime = [Numerics.BigInteger]::Parse('1099511628211')
    [Numerics.BigInteger]$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    function Update-Stage5LockstepFnv {
        param([Numerics.BigInteger]$Hash, [UInt64]$Value, [int]$Bytes,
            [Numerics.BigInteger]$Prime, [Numerics.BigInteger]$Mask)
        $updated = $Hash
        for ($byteIndex = 0; $byteIndex -lt $Bytes; ++$byteIndex) {
            $updated = (($updated -bxor
                ([Numerics.BigInteger]($Value -band 255))) * $Prime) -band $Mask
            $Value = $Value -shr 8
        }
        return $updated
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $hash = Update-Stage5LockstepFnv $hash ([UInt64]$slot) 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs["peer_${slot}_command_count"] `
                "peer_${slot}_command_count") 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs["peer_${slot}_first_command_frame"] `
                "peer_${slot}_first_command_frame") 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs["peer_${slot}_last_command_frame"] `
                "peer_${slot}_last_command_frame") 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            ([UInt64](ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["peer_${slot}_last_command_id"] `
                "peer_${slot}_last_command_id")) 2 $prime $mask
        $hasLast = ConvertTo-Stage5LockstepReceiptBoolean `
            $Pairs["peer_${slot}_has_last_command_id"] "peer_${slot}_has_last_command_id"
        $hash = Update-Stage5LockstepFnv $hash `
            ([UInt64]$(if ($hasLast) { 1 } else { 0 })) 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["peer_${slot}_last_command_digest"] `
                "peer_${slot}_last_command_digest") 8 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["peer_${slot}_command_digest"] `
                "peer_${slot}_command_digest") 8 $prime $mask
    }
    return $hash
}

function Get-Stage5LockstepReceiptProjectionSha256 {
    param([Collections.IDictionary]$Pairs)
    $projection = [ordered]@{}
    foreach ($key in @('mode', 'schema', 'protocol_epoch', 'peer_count', 'roster_mask',
        'build_compatibility_crc', 'content_crc', 'map_crc', 'common_stop_frame',
        'proven_kernel_mask', 'packet_router_slot', 'origin_mode', 'session_nonce',
        'executable_sha256', 'source_revision', 'final_frame', 'frame_count',
        'contributed_peer_mask', 'checkpoint_count', 'validation_authority_mask',
        'executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown')) {
        $projection[$key] = $Pairs[$key]
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        foreach ($suffix in @('command_count', 'first_command_frame',
            'last_command_frame', 'last_command_id', 'has_last_command_id',
            'last_command_digest', 'command_digest')) {
            $key = "peer_${slot}_${suffix}"
            $projection[$key] = $Pairs[$key]
        }
    }
    for ($checkpoint = 0; $checkpoint -lt 129; ++$checkpoint) {
        foreach ($suffix in @('frame', 'crc', 'command_digest')) {
            $key = "checkpoint_${checkpoint}_${suffix}"
            $projection[$key] = $Pairs[$key]
        }
    }
    $text = $projection | ConvertTo-Json -Compress -Depth 5
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-Stage5LockstepReceiptPairs {
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Lockstep-v2 receipt was not found: $full"
    $text = [IO.File]::ReadAllText($full)
    Assert-Stage5Condition ($text.IndexOf("`r", [StringComparison]::Ordinal) -lt 0) `
        "Lockstep-v2 receipt contains non-canonical CR line endings: $full"
    $lines = $text.Split(@("`n"), [StringSplitOptions]::None)
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') {
        $lines = $lines[0..($lines.Count - 2)]
    }
    Assert-Stage5Condition ($lines.Count -ge 4 -and
        $lines[0] -ceq 'RTS_LOCKSTEP_V2_RECEIPT' -and
        $lines[$lines.Count - 1] -ceq 'END') `
        "Lockstep-v2 receipt is not a canonical v2 document: $full"
    $pairs = [ordered]@{}
    for ($index = 1; $index -lt $lines.Count - 1; ++$index) {
        $line = $lines[$index]
        Assert-Stage5Condition ($line.Length -gt 0) `
            "Lockstep-v2 receipt contains an empty line: $full"
        $equals = $line.IndexOf('=', [StringComparison]::Ordinal)
        Assert-Stage5Condition ($equals -gt 0 -and $equals -lt ($line.Length - 1) -and
            $line.Substring(0, $equals) -match '^[A-Za-z_][A-Za-z0-9_]*$') `
            "Lockstep-v2 receipt contains a malformed key/value line: $full"
        $key = $line.Substring(0, $equals)
        Assert-Stage5Condition (-not $pairs.Contains($key)) `
            "Lockstep-v2 receipt repeats field '$key': $full"
        $pairs[$key] = $line.Substring($equals + 1)
    }
    Assert-Stage5Condition ($pairs.Contains('checkpoint_count')) `
        "Lockstep-v2 receipt has no checkpoint_count: $full"
    $checkpointCount = ConvertTo-Stage5LockstepReceiptUInt32 `
        $pairs['checkpoint_count'] 'checkpoint_count'
    Assert-Stage5Condition ($checkpointCount -eq 129) `
        "Lockstep-v2 receipt must contain exactly 129 checkpoints: $full"
    $expectedKeys = New-Object 'Collections.Generic.List[string]'
    foreach ($key in @('producer', 'mode', 'schema', 'protocol_epoch', 'local_slot',
        'peer_count', 'roster_mask', 'build_compatibility_crc', 'content_crc',
        'map_crc', 'common_stop_frame', 'proven_kernel_mask', 'packet_router_slot',
        'origin_mode', 'run_nonce', 'session_nonce', 'executable_sha256',
        'source_revision', 'network_session_token', 'final_frame', 'frame_count',
        'contributed_peer_mask', 'checkpoint_count', 'validation_authority_mask',
        'executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown')) {
        $expectedKeys.Add($key) | Out-Null
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        foreach ($suffix in @('command_count', 'first_command_frame',
            'last_command_frame', 'last_command_id', 'has_last_command_id',
            'last_command_digest', 'command_digest')) {
            $expectedKeys.Add("peer_${slot}_${suffix}") | Out-Null
        }
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        foreach ($suffix in @('physical_worker_mask', 'physical_worker_jobs',
            'distinct_physical_workers', 'peak_concurrent_physical_workers',
            'physical_worker_mask_complete')) {
            $expectedKeys.Add("kernel_${kernel}_${suffix}") | Out-Null
        }
    }
    for ($checkpoint = 0; $checkpoint -lt 129; ++$checkpoint) {
        foreach ($suffix in @('frame', 'crc', 'command_digest')) {
            $expectedKeys.Add("checkpoint_${checkpoint}_${suffix}") | Out-Null
        }
    }
    Assert-Stage5Condition ($pairs.Count -eq $expectedKeys.Count) `
        "Lockstep-v2 receipt has an unexpected field count: $full"
    $actualKeys = @($pairs.Keys)
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        Assert-Stage5Condition ($actualKeys[$index] -ceq $expectedKeys[$index]) `
            "Lockstep-v2 receipt field order/shape mismatch at ${index}: $full"
    }
    return [pscustomobject]@{ path = $full; pairs = $pairs; text = $text }
}

function Read-Stage5LockstepV2Receipt {
    param(
        [string]$Path,
        [int]$ExpectedLocalSlot,
        [int]$ExpectedPeerCount,
        [UInt32]$ExpectedMapCrc,
        [string]$ExpectedRunNonce,
        [string]$ExpectedSessionNonce,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedSourceCommit
    )
    $parsed = Get-Stage5LockstepReceiptPairs $Path
    $pairs = $parsed.pairs
    $context = "Lockstep-v2 receipt '$Path'"
    Assert-Stage5Condition ($pairs['producer'] -ceq 'installed-lockstep-v2' -and
        $pairs['mode'] -ceq 'installed-lockstep-v2-production') `
        "$context has a noncanonical producer/mode boundary."
    foreach ($check in @(
        @('schema', 2), @('protocol_epoch', 2),
        @('local_slot', $ExpectedLocalSlot), @('peer_count', $ExpectedPeerCount),
        @('roster_mask', ((1 -shl $ExpectedPeerCount) - 1)),
        @('map_crc', $ExpectedMapCrc), @('common_stop_frame', 4096),
        @('proven_kernel_mask', 63), @('packet_router_slot', 0),
        @('origin_mode', 2), @('final_frame', 4096),
        @('frame_count', 4096),
        @('contributed_peer_mask', ((1 -shl $ExpectedPeerCount) - 1)),
        @('checkpoint_count', 129), @('validation_authority_mask', 63)
    )) {
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs[$check[0]] $check[0]) -eq [UInt64]$check[1]) `
            "$context field '$($check[0])' does not match the v2 qualification contract."
    }
    foreach ($crcField in @('build_compatibility_crc', 'content_crc')) {
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs[$crcField] $crcField) -gt 0) `
            "$context field '$crcField' is not a positive executable-originated CRC."
    }
    Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt64 `
        $pairs['network_session_token'] 'network_session_token') -gt 0) `
        "$context has no network session token."
    Assert-Stage5Condition ($pairs['run_nonce'] -ceq $ExpectedRunNonce -and
        $pairs['run_nonce'] -cmatch '^[0-9A-F]{32}$' -and
        $pairs['session_nonce'] -ceq $ExpectedSessionNonce -and
        $pairs['session_nonce'] -cmatch '^[0-9A-F]{32}$' -and
        $pairs['executable_sha256'] -cmatch '^[0-9A-F]{64}$' -and
        $pairs['executable_sha256'] -ceq $ExpectedExecutableSha256.ToUpperInvariant() -and
        $pairs['source_revision'] -cmatch '^[0-9a-f]{40}$' -and
        $pairs['source_revision'] -ceq $ExpectedSourceCommit) `
        "$context executable/source/run identity does not match the installed x64 binding."
    foreach ($field in @('executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown')) {
        Assert-Stage5Condition (ConvertTo-Stage5LockstepReceiptBoolean `
            $pairs[$field] $field) "$context field '$field' is not true."
    }
    $expectedFrames = New-Object 'Collections.Generic.List[UInt32]'
    $expectedFrames.Add(1) | Out-Null
    for ($frame = 32; $frame -le 4096; $frame += 32) {
        $expectedFrames.Add([UInt32]$frame) | Out-Null
    }
    for ($index = 0; $index -lt $expectedFrames.Count; ++$index) {
        $frame = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["checkpoint_${index}_frame"] "checkpoint_${index}_frame"
        Assert-Stage5Condition ($frame -eq $expectedFrames[$index]) `
            "$context checkpoint $index is not on the canonical 4096-frame boundary."
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["checkpoint_${index}_crc"] "checkpoint_${index}_crc") -gt 0) `
            "$context checkpoint $index has no executable-originated CRC."
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["checkpoint_${index}_command_digest"] `
                "checkpoint_${index}_command_digest") -gt 0) `
            "$context checkpoint $index has no command digest."
    }
    $authorityDigest = Get-Stage5LockstepReceiptDigest $pairs
    $lastDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
        $pairs['checkpoint_128_command_digest'] 'checkpoint_128_command_digest'
    Assert-Stage5Condition ($lastDigest -gt 0 -and
        $authorityDigest -eq [Numerics.BigInteger]$lastDigest) `
        "$context command digest is not the canonical peer contribution digest."
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $count = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["peer_${slot}_command_count"] "peer_${slot}_command_count"
        $first = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["peer_${slot}_first_command_frame"] "peer_${slot}_first_command_frame"
        $last = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["peer_${slot}_last_command_frame"] "peer_${slot}_last_command_frame"
        $id = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["peer_${slot}_last_command_id"] "peer_${slot}_last_command_id"
        $has = ConvertTo-Stage5LockstepReceiptBoolean `
            $pairs["peer_${slot}_has_last_command_id"] "peer_${slot}_has_last_command_id"
        $lastCommandDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["peer_${slot}_last_command_digest"] "peer_${slot}_last_command_digest"
        $commandDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["peer_${slot}_command_digest"] "peer_${slot}_command_digest"
        if ($slot -lt $ExpectedPeerCount) {
            Assert-Stage5Condition ($count -ge 1 -and $first -ge 1 -and
                $first -le 4096 -and $last -ge $first -and $last -le 4096 -and
                $has -and $id -gt 0 -and
                $lastCommandDigest -gt 0 -and $commandDigest -gt 0) `
                "$context peer $slot has no valid gameplay command contribution in frames 1..4096."
        }
        else {
            Assert-Stage5Condition ($count -eq 0 -and $first -eq 0 -and $last -eq 0 -and
                $id -eq 0 -and -not $has -and $lastCommandDigest -eq 0 -and
                $commandDigest -eq 0) `
                "$context non-roster peer $slot has a command contribution."
        }
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        $mask = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["kernel_${kernel}_physical_worker_mask"] `
            "kernel_${kernel}_physical_worker_mask"
        $jobs = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["kernel_${kernel}_physical_worker_jobs"] `
            "kernel_${kernel}_physical_worker_jobs"
        $distinct = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["kernel_${kernel}_distinct_physical_workers"] `
            "kernel_${kernel}_distinct_physical_workers"
        $peak = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["kernel_${kernel}_peak_concurrent_physical_workers"] `
            "kernel_${kernel}_peak_concurrent_physical_workers"
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptBoolean `
            $pairs["kernel_${kernel}_physical_worker_mask_complete"] `
                "kernel_${kernel}_physical_worker_mask_complete") -and
            $mask -ne 0 -and $jobs -gt 0 -and $distinct -ge 2 -and $peak -ge 2 -and
            $distinct -eq (Get-Stage5UInt64BitCount $mask)) `
            "$context kernel $kernel lacks complete executable-origin worker telemetry."
    }
    return [pscustomobject]@{
        parsed = $parsed
        projectionSha256 = Get-Stage5LockstepReceiptProjectionSha256 $pairs
        finalFrame = [UInt32]4096
        finalCRC = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs['checkpoint_128_crc'] 'checkpoint_128_crc'
    }
}

function Test-Stage5LockstepSafeMapName {
    param([object]$Value)
    return $Value -is [string] -and $Value.Length -ge 4 -and
        $Value.Length -lt 248 -and $Value.EndsWith('.map',
            [StringComparison]::OrdinalIgnoreCase) -and
        $Value -notmatch '(^[\\/]|:|\.\.|\s)'
}

function Assert-Stage5LockstepLauncherContract {
    param(
        [object]$Contract,
        [string]$Title,
        [Collections.IDictionary]$ArtifactHashes,
        [string]$Context
    )
    Assert-Stage5JsonShape $Contract @('schemaVersion', 'mode', 'configPath',
        'configSha256', 'launcherPath', 'launcherSha256', 'directory',
        'executable', 'launcherTarget', 'launcherArguments',
        'launcherWorkingDirectory', 'directExecutable',
        'directWorkingDirectory', 'directArguments', 'childExitCodeObserved') $Context
    $executableRole = if ($Title -ceq 'Generals') {
        'generals-executable'
    }
    else { 'zerohour-executable' }
    $launcherRole = if ($Title -ceq 'Generals') {
        'generals-launcher'
    }
    else { 'zerohour-launcher' }
    $configRole = if ($Title -ceq 'Generals') {
        'generals-launcher-config'
    }
    else { 'zerohour-launcher-config' }
    $expectedExecutableSha256 = [string]$ArtifactHashes[$executableRole]
    $expectedLauncherSha256 = [string]$ArtifactHashes[$launcherRole]
    $expectedConfigSha256 = [string]$ArtifactHashes[$configRole]
    foreach ($hashBinding in @(
        @('launcherSha256', $expectedLauncherSha256),
        @('configSha256', $expectedConfigSha256)
    )) {
        Assert-Stage5Condition ($Contract[$hashBinding[0]] -is [string] -and
            $Contract[$hashBinding[0]] -cmatch '^[0-9A-F]{64}$' -and
            $Contract[$hashBinding[0]] -ceq $hashBinding[1].ToUpperInvariant()) `
            "$Context $($hashBinding[0]) does not bind the artifact-set hash."
    }
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Contract['schemaVersion']) -and
        [Int64]$Contract['schemaVersion'] -eq 1 -and
        $Contract['mode'] -is [string] -and $Contract['mode'] -ceq 'headless-direct-exception' -and
        $Contract['directory'] -is [string] -and $Contract['directory'] -ceq '.' -and
        $Contract['childExitCodeObserved'] -is [bool] -and
        [bool]$Contract['childExitCodeObserved']) `
        "$Context is not the reviewed bounded launcher-equivalence contract."
    foreach ($pathField in @('configPath', 'launcherPath', 'launcherTarget',
        'launcherWorkingDirectory', 'directExecutable', 'directWorkingDirectory')) {
        Assert-Stage5Condition ($Contract[$pathField] -is [string] -and
            [IO.Path]::IsPathRooted([string]$Contract[$pathField]) -and
            [string]$Contract[$pathField] -notmatch '[\";]') `
            "$Context $pathField is not an absolute executable-origin path."
    }
    $configPath = [IO.Path]::GetFullPath([string]$Contract['configPath'])
    $launcherPath = [IO.Path]::GetFullPath([string]$Contract['launcherPath'])
    $targetPath = [IO.Path]::GetFullPath([string]$Contract['launcherTarget'])
    $directPath = [IO.Path]::GetFullPath([string]$Contract['directExecutable'])
    $launcherWorkingDirectory = [IO.Path]::GetFullPath([string]$Contract['launcherWorkingDirectory'])
    $directWorkingDirectory = [IO.Path]::GetFullPath([string]$Contract['directWorkingDirectory'])
    Assert-Stage5Condition ([IO.Path]::GetFileName($configPath) -ceq 'launcher.lcf' -and
        [IO.Path]::GetFileName($launcherPath) -ceq 'launcher.exe' -and
        [IO.Path]::GetFileName($directPath) -ceq [string]$Contract['executable'] -and
        [IO.Path]::GetFileName($targetPath) -ceq [string]$Contract['executable'] -and
        $targetPath -ceq $directPath -and
        [IO.Path]::GetFullPath((Split-Path -Parent $configPath)) -ceq $directWorkingDirectory -and
        [IO.Path]::GetFullPath((Split-Path -Parent $launcherPath)) -ceq $directWorkingDirectory -and
        $launcherWorkingDirectory -ceq $directWorkingDirectory -and
        [string]$Contract['executable'] -match '^[A-Za-z0-9._-]+\.exe$') `
        "$Context does not bind launcher, configuration, and executable to one runtime directory."
    Assert-Stage5Condition ($Contract['launcherArguments'] -is [Array] -and
        $Contract['directArguments'] -is [Array] -and
        $Contract['launcherArguments'].Count -eq 4 -and
        $Contract['directArguments'].Count -eq 4 -and
        [string]$Contract['launcherArguments'][0] -ceq '-simulationMode' -and
        [string]$Contract['launcherArguments'][1] -ceq 'parallel' -and
        [string]$Contract['launcherArguments'][2] -ceq '-workerPolicy' -and
        [string]$Contract['launcherArguments'][3] -ceq 'auto' -and
        (@($Contract['directArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            (@($Contract['launcherArguments'] | ForEach-Object { [string]$_ }) -join '|')) `
        "$Context does not retain the exact reviewed launcher defaults."
    Assert-Stage5Condition ($Contract['launcherTarget'] -ceq $Contract['directExecutable'] -and
        $Contract['launcherWorkingDirectory'] -ceq $Contract['directWorkingDirectory']) `
        "$Context launcher target is not the direct executable identity."
    return $Contract
}

function Assert-Stage5LockstepTitleSessionContract {
    param(
        [object]$Contract,
        [string]$Title,
        [string]$SessionDirectory,
        [object]$LauncherContract,
        [string]$Context
    )
    Assert-Stage5JsonShape $Contract @('schemaVersion', 'title',
        'sessionRoot', 'runtimeDirectory', 'documentsRoot', 'profileLeaf',
        'profileRoot', 'peerRoot', 'profileConcurrency', 'environmentValues',
        'environmentVariableNames', 'registryViews', 'registryValues') $Context
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Contract['schemaVersion']) -and
        [Int64]$Contract['schemaVersion'] -eq 1 -and
        $Contract['title'] -is [string] -and $Contract['title'] -ceq $Title -and
        $Contract['profileConcurrency'] -is [string] -and
        $Contract['profileConcurrency'] -ceq 'shared-title-profile-read-only') `
        "$Context has an invalid title-session profile identity."
    $outputTitleFull = [IO.Path]::GetFullPath($SessionDirectory)
    $sessionFull = Join-Path $outputTitleFull 'TitleSession'
    $runtimeFull = [IO.Path]::GetFullPath([string]$LauncherContract['directWorkingDirectory'])
    $documentsRoot = Join-Path $sessionFull 'Documents'
    $profileLeaf = if ($Title -ceq 'Generals') {
        'Command and Conquer Generals Data'
    }
    else { 'GGC-LockstepV2-ZeroHour' }
    $profileRoot = Join-Path $documentsRoot $profileLeaf
    $peerRoot = Join-Path $sessionFull 'Peers'
    Assert-Stage5Condition ($Contract['sessionRoot'] -is [string] -and
        [string]$Contract['sessionRoot'] -ceq $sessionFull) `
        "$Context sessionRoot is stale or substituted (actual='$($Contract['sessionRoot'])', expected='$sessionFull')."
    Assert-Stage5Condition ($Contract['runtimeDirectory'] -is [string] -and
        [string]$Contract['runtimeDirectory'] -ceq $runtimeFull) `
        "$Context runtimeDirectory is stale or substituted."
    Assert-Stage5Condition ($Contract['documentsRoot'] -is [string] -and
        [string]$Contract['documentsRoot'] -ceq $documentsRoot) `
        "$Context documentsRoot is stale or substituted."
    Assert-Stage5Condition ($Contract['profileLeaf'] -is [string] -and
        [string]$Contract['profileLeaf'] -ceq $profileLeaf) `
        "$Context profileLeaf is stale or substituted."
    Assert-Stage5Condition ($Contract['profileRoot'] -is [string] -and
        [string]$Contract['profileRoot'] -ceq $profileRoot) `
        "$Context profileRoot is stale or substituted."
    Assert-Stage5Condition ($Contract['peerRoot'] -is [string] -and
        [string]$Contract['peerRoot'] -ceq $peerRoot) `
        "$Context peerRoot is stale or substituted."
    $expectedEnvironment = [ordered]@{
        TEMP = Join-Path $sessionFull 'Temp'
        TMP = Join-Path $sessionFull 'Tmp'
        LOCALAPPDATA = Join-Path $sessionFull 'LocalAppData'
        APPDATA = Join-Path $sessionFull 'AppData'
        USERPROFILE = $sessionFull
        HOMEDRIVE = 'H:'
        HOMEPATH = $sessionFull.Substring(2)
        RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileRoot
        RTS_STAGE5_VALIDATION_CACHE_ROOT = Join-Path $sessionFull 'Cache'
        RTS_STAGE5_VALIDATION_LOG_ROOT = Join-Path $sessionFull 'Logs'
        RTS_STAGE5_VALIDATION_DUMP_ROOT = Join-Path $sessionFull 'Dumps'
        RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT = $sessionFull
    }
    $environmentNames = @($expectedEnvironment.Keys | ForEach-Object { [string]$_ })
    Assert-Stage5JsonShape $Contract['environmentValues'] $environmentNames `
        "$Context environment values"
    foreach ($name in $environmentNames) {
        Assert-Stage5Condition ($Contract['environmentValues'][$name] -is [string] -and
            [string]$Contract['environmentValues'][$name] -ceq
                [string]$expectedEnvironment[$name]) `
            "$Context environment value '$name' is stale or substituted."
    }
    Assert-Stage5Condition ($Contract['environmentVariableNames'] -is [Array] -and
        (@($Contract['environmentVariableNames'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($environmentNames -join '|')) `
        "$Context environment variable names are stale or reordered."
    $expectedViews = @('Registry32', 'Registry64')
    Assert-Stage5Condition ($Contract['registryViews'] -is [Array] -and
        (@($Contract['registryViews'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedViews -join '|')) `
        "$Context registry-view coverage is incomplete or substituted."
    $expectedRegistryValues = New-Object 'Collections.Generic.List[object]'
    $expectedRegistryValues.Add([ordered]@{
        subKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders'
        name = 'Personal'; value = $documentsRoot; purpose = 'known-folder-documents'
    }) | Out-Null
    $expectedRegistryValues.Add([ordered]@{
        subKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders'
        name = 'Personal'; value = $documentsRoot; purpose = 'known-folder-documents'
    }) | Out-Null
    if ($Title -ceq 'Generals') {
        $expectedRegistryValues.Add([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Generals'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
    }
    else {
        $expectedRegistryValues.Add([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
        $expectedRegistryValues.Add([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'UserDataLeafName'; value = $profileLeaf; purpose = 'title-profile-leaf'
        }) | Out-Null
    }
    Assert-Stage5Condition ($Contract['registryValues'] -is [Array] -and
        $Contract['registryValues'].Count -eq $expectedRegistryValues.Count) `
        "$Context registry-value evidence has an unexpected count."
    for ($index = 0; $index -lt $expectedRegistryValues.Count; ++$index) {
        $actualEntry = $Contract['registryValues'][$index]
        $expectedEntry = $expectedRegistryValues[$index]
        Assert-Stage5JsonShape $actualEntry @('subKey', 'name', 'value', 'purpose') `
            "$Context registry value $index"
        foreach ($field in @('subKey', 'name', 'value', 'purpose')) {
            Assert-Stage5Condition ([string]$actualEntry[$field] -ceq
                [string]$expectedEntry[$field]) `
                "$Context registry value $index field '$field' is stale or substituted."
        }
    }
    return $Contract
}

function Assert-Stage5LockstepRegistryEquivalence {
    param(
        [object]$Equivalence,
        [object]$TitleSessionProfile,
        [string]$Context
    )
    Assert-Stage5JsonShape $Equivalence @('strategy', 'views', 'values',
        'profileRoot') $Context
    Assert-Stage5Condition ($Equivalence['strategy'] -is [string] -and
        $Equivalence['strategy'] -ceq 'known-folder-registry-redirect' -and
        $Equivalence['profileRoot'] -is [string] -and
        $Equivalence['profileRoot'] -ceq $TitleSessionProfile['profileRoot'] -and
        $Equivalence['views'] -is [Array] -and
        (@($Equivalence['views'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            (@($TitleSessionProfile['registryViews'] | ForEach-Object { [string]$_ }) -join '|') -and
        ($Equivalence['values'] | ConvertTo-Json -Compress -Depth 12) -ceq
            ($TitleSessionProfile['registryValues'] | ConvertTo-Json -Compress -Depth 12)) `
        "$Context is not bound to the reviewed registry equivalence."
    return $Equivalence
}

function Assert-Stage5LockstepEnvironmentEquivalence {
    param(
        [object]$Equivalence,
        [object]$TitleSessionProfile,
        [int]$PeerIndex,
        [string]$Context
    )
    Assert-Stage5JsonShape $Equivalence @('peer', 'root', 'values',
        'variableNames') $Context
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Equivalence['peer']) -and
        [Int64]$Equivalence['peer'] -eq $PeerIndex) `
        "$Context peer identity is stale or substituted."
    $expectedRoot = Join-Path $TitleSessionProfile['peerRoot'] "peer-$PeerIndex"
    Assert-Stage5Condition ($Equivalence['root'] -is [string] -and
        [string]$Equivalence['root'] -ceq $expectedRoot) `
        "$Context root is not bound to the title-session peer root."
    $baseValues = $TitleSessionProfile['environmentValues']
    $environmentNames = @($TitleSessionProfile['environmentVariableNames'] |
        ForEach-Object { [string]$_ })
    $expectedValues = [ordered]@{}
    foreach ($name in $environmentNames) {
        $baseValue = [string]$baseValues[$name]
        if ($name -ceq 'TEMP' -or $name -ceq 'TMP' -or
            $name -ceq 'LOCALAPPDATA' -or $name -ceq 'APPDATA' -or
            $name -ceq 'RTS_STAGE5_VALIDATION_CACHE_ROOT' -or
            $name -ceq 'RTS_STAGE5_VALIDATION_LOG_ROOT' -or
            $name -ceq 'RTS_STAGE5_VALIDATION_DUMP_ROOT') {
            $expectedValues[$name] = Join-Path $expectedRoot ([IO.Path]::GetFileName($baseValue))
        }
        else { $expectedValues[$name] = $baseValue }
    }
    Assert-Stage5JsonShape $Equivalence['values'] $environmentNames `
        "$Context environment values"
    foreach ($name in $environmentNames) {
        Assert-Stage5Condition ($Equivalence['values'][$name] -is [string] -and
            [string]$Equivalence['values'][$name] -ceq [string]$expectedValues[$name]) `
            "$Context environment value '$name' is stale or substituted."
    }
    Assert-Stage5Condition ($Equivalence['variableNames'] -is [Array] -and
        (@($Equivalence['variableNames'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($environmentNames -join '|')) `
        "$Context environment variable names are stale or reordered."
    return $Equivalence
}

function Assert-Stage5LockstepPeerLaunchBinding {
    param(
        [object]$Peer,
        [object]$LauncherContract,
        [string]$Title,
        [int]$PeerIndex,
        [int]$PeerCount,
        [int[]]$Ports,
        [string]$RunNonce,
        [string]$SessionNonce,
        [string]$MapName,
        [UInt32]$MapCrc,
        [int]$Seed,
        [string]$SessionDirectory,
        [string]$Context
    )
    Assert-Stage5Condition ($Peer['directExecutionOptIn'] -is [bool] -and
        [bool]$Peer['directExecutionOptIn'] -and
        $Peer['workingDirectory'] -is [string] -and
        [string]$Peer['workingDirectory'] -ceq [string]$LauncherContract['directWorkingDirectory'] -and
        [string]$Peer['launcherPath'] -ceq [string]$LauncherContract['launcherPath'] -and
        [string]$Peer['launcherConfigPath'] -ceq [string]$LauncherContract['configPath'] -and
        [string]$Peer['launcherSha256'] -ceq [string]$LauncherContract['launcherSha256'] -and
        [string]$Peer['launcherConfigSha256'] -ceq [string]$LauncherContract['configSha256']) `
        "$Context launch provenance is substituted or stale."
    Assert-Stage5Condition ($Peer['launcherDefaultArguments'] -is [Array] -and
        $Peer['launcherDefaultArguments'].Count -eq 4 -and
        (@($Peer['launcherDefaultArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            (@($LauncherContract['launcherArguments'] | ForEach-Object { [string]$_ }) -join '|') -and
        $Peer['arguments'] -is [Array] -and
        $Peer['directArguments'] -is [Array] -and
        (@($Peer['directArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            (@($Peer['arguments'] | ForEach-Object { [string]$_ }) -join '|') -and
        [string]$Peer['arguments'][0] -ceq '-simulationMode' -and
        [string]$Peer['arguments'][1] -ceq 'parallel' -and
        [string]$Peer['arguments'][2] -ceq '-workerPolicy' -and
        [string]$Peer['arguments'][3] -ceq 'auto') `
        "$Context process arguments do not bind the reviewed lockstep-v2 launch."
    $arguments = @($Peer['arguments'] | ForEach-Object { [string]$_ })
    $markerIndices = @(
        for ($argumentIndex = 4; $argumentIndex -lt $arguments.Count; ++$argumentIndex) {
            if ($arguments[$argumentIndex] -ceq '-installedLockstepV2Validation') {
                $argumentIndex
            }
        }
    )
    Assert-Stage5Condition ($markerIndices.Count -eq 1 -and
        $markerIndices[0] -gt 4 -and $markerIndices[0] -lt ($arguments.Count - 1)) `
        "$Context process arguments do not contain one terminal lockstep-v2 validation switch."
    $overrideArguments = @($Peer['workerOverride'].overrideArguments |
        ForEach-Object { [string]$_ })
    Assert-Stage5Condition ($markerIndices[0] -eq (4 + $overrideArguments.Count) -and
        (@($arguments[4..($markerIndices[0] - 1)]) -join '|') -ceq
            ($overrideArguments -join '|')) `
        "$Context process arguments do not bind the executable worker override."
    $configuration = $arguments[$markerIndices[0] + 1]
    $portsText = ($Ports | ForEach-Object { [string]$_ }) -join ','
    $pattern = '^peer=(?<peer>[0-9]+);peers=(?<peers>[0-9]+);ports=(?<ports>[0-9,]+);run=(?<run>[0-9A-F]{32});session=(?<session>[0-9A-F]{32});exe=(?<exe>[0-9A-F]{64});source=(?<source>[0-9a-f]{40});map=(?<map>[^;]+);map_crc=(?<mapCrc>[0-9]+);seed=(?<seed>[0-9]+);dir=(?<dir>[^;]+);receipt=(?<receipt>[^;]+);mode=trusted-router;router=0$'
    $match = [regex]::Match($configuration, $pattern)
    Assert-Stage5Condition ($match.Success -and
        $match.Groups['peer'].Value -ceq [string]$PeerIndex -and
        $match.Groups['peers'].Value -ceq [string]$PeerCount -and
        $match.Groups['ports'].Value -ceq $portsText -and
        $match.Groups['run'].Value -ceq $RunNonce -and
        $match.Groups['session'].Value -ceq $SessionNonce -and
        $match.Groups['exe'].Value -ceq ([string]$Peer['executableSha256']).ToUpperInvariant() -and
        $match.Groups['source'].Value -ceq $Peer['sourceCommit'] -and
        $match.Groups['map'].Value -ceq $MapName -and
        $match.Groups['mapCrc'].Value -ceq [string]$MapCrc -and
        $match.Groups['seed'].Value -ceq [string]$Seed -and
        [IO.Path]::GetFullPath($match.Groups['dir'].Value) -ceq
            [IO.Path]::GetFullPath($SessionDirectory) -and
        $match.Groups['receipt'].Value -ceq [string]$Peer['receiptPath']) `
        "$Context configuration does not bind the exact peer, port, run, map, or receipt identity."
    Assert-Stage5Condition ($Peer['commandLine'] -is [string] -and
        ([string]$Peer['commandLine']).StartsWith('"' + [string]$LauncherContract['directExecutable'] + '" ',
            [StringComparison]::Ordinal) -and
        ([string]$Peer['commandLine']).IndexOf([string]$configuration) -ge 0) `
        "$Context command line does not bind the retained executable and configuration identity."
}

function Assert-Stage5LockstepWorkerEvidence {
    param(
        [object]$Peer,
        [int]$PeerIndex,
        [string]$Context
    )
    $expectedProfile = if (($PeerIndex % 2) -eq 0) {
        [ordered]@{
            profile = 'explicit-two-workers'; requestedWorkers = '2'
            workerPolicy = 'all'; overrideArguments = @('-workerCount', '2', '-workerPolicy', 'all')
        }
    }
    else {
        [ordered]@{
            profile = 'automatic-workers'; requestedWorkers = 'auto'
            workerPolicy = 'auto'; overrideArguments = @('-workerPolicy', 'auto')
        }
    }
    Assert-Stage5JsonShape $Peer['workerOverride'] @('profile',
        'requestedWorkers', 'workerPolicy', 'overrideArguments') "$Context worker override"
    Assert-Stage5Condition ($Peer['workerOverride']['profile'] -ceq $expectedProfile.profile -and
        $Peer['workerOverride']['requestedWorkers'] -ceq $expectedProfile.requestedWorkers -and
        $Peer['workerOverride']['workerPolicy'] -ceq $expectedProfile.workerPolicy -and
        $Peer['workerOverride']['overrideArguments'] -is [Array] -and
        (@($Peer['workerOverride']['overrideArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedProfile.overrideArguments -join '|')) `
        "$Context worker override is stale, homogeneous, or substituted."
    Assert-Stage5Condition ($Peer['requestedWorkers'] -ceq $expectedProfile.requestedWorkers -and
        $Peer['workerPolicy'] -ceq $expectedProfile.workerPolicy) `
        "$Context worker profile summary is substituted."
    Assert-Stage5JsonShape $Peer['receiptWorkerTelemetry'] @('requestedWorkers',
        'workerPolicy', 'effectiveWorkers', 'distinctPhysicalWorkers',
        'physicalWorkerMasks', 'executableOrigin') "$Context receipt worker telemetry"
    Assert-Stage5Condition ($Peer['receiptWorkerTelemetry']['requestedWorkers'] -ceq
        $expectedProfile.requestedWorkers -and
        $Peer['receiptWorkerTelemetry']['workerPolicy'] -ceq $expectedProfile.workerPolicy -and
        (Test-Stage5JsonInteger $Peer['receiptWorkerTelemetry']['effectiveWorkers']) -and
        [Int64]$Peer['receiptWorkerTelemetry']['effectiveWorkers'] -ge 2 -and
        $Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'] -is [Array] -and
        $Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'].Count -eq 6 -and
        $Peer['receiptWorkerTelemetry']['physicalWorkerMasks'] -is [Array] -and
        $Peer['receiptWorkerTelemetry']['physicalWorkerMasks'].Count -eq 6 -and
        $Peer['receiptWorkerTelemetry']['executableOrigin'] -is [bool] -and
        [bool]$Peer['receiptWorkerTelemetry']['executableOrigin']) `
        "$Context receipt worker telemetry is incomplete or not executable-originated."
    foreach ($distinctValue in @($Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'])) {
        Assert-Stage5Condition ((Test-Stage5JsonInteger $distinctValue) -and
            [Int64]$distinctValue -ge 2) `
            "$Context receipt worker telemetry has a non-integer effective worker count."
    }
    foreach ($maskValue in @($Peer['receiptWorkerTelemetry']['physicalWorkerMasks'])) {
        Assert-Stage5Condition ((Test-Stage5JsonInteger $maskValue) -and
            [UInt64]$maskValue -gt 0) `
            "$Context receipt worker telemetry has an empty physical-worker mask."
    }
    $effective = [int]$Peer['receiptWorkerTelemetry']['effectiveWorkers']
    $distinct = @($Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'] |
        ForEach-Object { [int]$_ })
    Assert-Stage5Condition (@($distinct | Select-Object -Unique).Count -eq 1 -and
        $distinct[0] -eq $effective -and
        (($expectedProfile.requestedWorkers -ceq 'auto' -and $effective -gt 2) -or
            ($expectedProfile.requestedWorkers -ne 'auto' -and $effective -eq 2))) `
        "$Context receipt worker telemetry does not prove the reviewed effective worker profile."
    Assert-Stage5JsonShape $Peer['stdoutProof'] @('executableOrigin', 'peer',
        'pid', 'frameLimit', 'activeMarker', 'passMarker', 'finalCrc') "$Context stdout proof"
    Assert-Stage5Condition ($Peer['stdoutProof']['executableOrigin'] -is [bool] -and
        [bool]$Peer['stdoutProof']['executableOrigin'] -and
        (Test-Stage5JsonInteger $Peer['stdoutProof']['peer']) -and
        [Int64]$Peer['stdoutProof']['peer'] -eq $PeerIndex -and
        (Test-Stage5JsonInteger $Peer['stdoutProof']['pid']) -and
        [Int64]$Peer['stdoutProof']['pid'] -gt 0 -and
        (Test-Stage5JsonInteger $Peer['stdoutProof']['frameLimit']) -and
        [Int64]$Peer['stdoutProof']['frameLimit'] -eq 4096 -and
        $Peer['stdoutProof']['activeMarker'] -is [string] -and
        $Peer['stdoutProof']['passMarker'] -is [string] -and
        $Peer['stdoutProof']['finalCrc'] -is [string] -and
        $Peer['stdoutProof']['finalCrc'] -cmatch '^[0-9A-F]{8}$') `
        "$Context stdout proof is not a canonical bounded lockstep-v2 marker set."
    return [pscustomobject]@{
        effectiveWorkers = $effective
        expectedProfile = $expectedProfile
    }
}

function Read-Stage5LockstepV2Evidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Lockstep-v2 multiplayer evidence was not found: $full"
    Assert-Stage5Condition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Lockstep-v2 ExpectedSourceCommit must be an independently supplied lowercase 40-hex commit.'
    Assert-Stage5Condition ($ExpectedArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
        'Lockstep-v2 ExpectedArtifactSetSha256 must be an independently supplied SHA-256.'
    Assert-Stage5Condition ($ArtifactHashes -is [Collections.IDictionary] -and
        $ArtifactHashes.Contains('generals-executable') -and
        $ArtifactHashes.Contains('zerohour-executable')) `
        'Lockstep-v2 executable bindings are incomplete.'
    $document = ConvertFrom-Stage5JsonDictionary $full
    Assert-Stage5Condition ($document -is [Collections.IDictionary]) `
        'Lockstep-v2 multiplayer evidence must be a JSON object.'
    $isDiagnosticV1 = $false
    if ($document.Keys -contains 'schemaVersion' -and
        $document.Keys -contains 'evidenceKind' -and
        $document.Keys -contains 'producer' -and
        $document.Keys -contains 'validationMode') {
        $isDiagnosticV1 = (Test-Stage5JsonInteger $document['schemaVersion']) -and
            [Int64]$document['schemaVersion'] -eq 1 -and
            [string]$document['evidenceKind'] -ceq 'installed-net3-loopback' -and
            [string]$document['producer'] -ceq 'installed-runtime-runner-v1' -and
            [string]$document['validationMode'] -ceq 'scoped-net3-loopback-release-proof'
    }
    $boundary = "schemaVersion=2, evidenceKind='lockstep-v2-multiplayer', producer='installed-lockstep-v2', validationMode='installed-lockstep-v2-production'"
    if ($isDiagnosticV1) {
        throw "Mixed-worker multiplayer attachment is diagnostic NET3 v1 and cannot satisfy final Stage 5 acceptance. It is supplementary only. Required boundary: $boundary"
    }
    $names = @('schemaVersion', 'evidenceKind', 'status', 'producer',
        'validationMode', 'architecture', 'sourceCommit', 'artifactSetSha256',
        'recordedUtc', 'allowHeadlessDirectExecution', 'launcherEquivalence',
        'commonStopFrame', 'peerCount', 'mapName', 'mapCrc', 'seed',
        'v1Accepted', 'profileStrategy', 'registryViews',
        'environmentVariables', 'profileConcurrency', 'sessions')
    Assert-Stage5JsonShape $document $names 'Lockstep-v2 multiplayer evidence'
    foreach ($field in @('schemaVersion', 'commonStopFrame', 'peerCount', 'mapCrc', 'seed')) {
        Assert-Stage5Condition (Test-Stage5JsonInteger $document[$field]) `
            "Lockstep-v2 multiplayer evidence field '$field' must be an integer."
    }
    Assert-Stage5Condition ([Int64]$document['schemaVersion'] -eq 2 -and
        [string]$document['evidenceKind'] -ceq 'lockstep-v2-multiplayer' -and
        [string]$document['status'] -ceq 'passed' -and
        [string]$document['producer'] -ceq 'installed-lockstep-v2' -and
        [string]$document['validationMode'] -ceq 'installed-lockstep-v2-production' -and
        [string]$document['architecture'] -ceq 'x64') `
        "Lockstep-v2 multiplayer evidence has an invalid schema/producer/mode boundary; required boundary: $boundary"
    Assert-Stage5Condition ([string]$document['sourceCommit'] -ceq $ExpectedSourceCommit -and
        [string]$document['sourceCommit'] -cmatch '^[0-9a-f]{40}$') `
        'Lockstep-v2 multiplayer evidence sourceCommit is stale or substituted.'
    Assert-Stage5Condition ([string]$document['artifactSetSha256'] -match '^[0-9A-Fa-f]{64}$' -and
        [string]$document['artifactSetSha256'].ToUpperInvariant() -ceq
            $ExpectedArtifactSetSha256.ToUpperInvariant()) `
        'Lockstep-v2 multiplayer evidence artifactSetSha256 does not match the independently hashed artifact set.'
    Assert-Stage5Condition ([Int64]$document['commonStopFrame'] -eq 4096 -and
        [Int64]$document['peerCount'] -ge 2 -and [Int64]$document['peerCount'] -le 8 -and
        [Int64]$document['mapCrc'] -gt 0 -and [Int64]$document['mapCrc'] -le 4294967295 -and
        [Int64]$document['seed'] -gt 0 -and $document['mapName'] -is [string] -and
        (Test-Stage5LockstepSafeMapName $document['mapName']) -and
        $document['v1Accepted'] -is [bool] -and -not [bool]$document['v1Accepted']) `
        'Lockstep-v2 multiplayer evidence does not prove the bounded x64 4096-frame v2 contract.'
    Assert-Stage5Condition ($document['recordedUtc'] -is [string]) `
        'Lockstep-v2 multiplayer evidence recordedUtc must be a JSON string.'
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($document['recordedUtc'], [ref]$recorded)) `
        'Lockstep-v2 multiplayer evidence recordedUtc is not a valid timestamp.'
    Assert-Stage5Condition ($document['allowHeadlessDirectExecution'] -is [bool] -and
        [bool]$document['allowHeadlessDirectExecution']) `
        'Lockstep-v2 multiplayer evidence did not record the reviewed direct-execution opt-in.'
    Assert-Stage5Condition ($document['profileStrategy'] -is [string] -and
        $document['profileStrategy'] -ceq 'known-folder-registry-redirect' -and
        $document['profileConcurrency'] -is [string] -and
        $document['profileConcurrency'] -ceq 'shared-title-profile-read-only') `
        'Lockstep-v2 multiplayer evidence did not retain the reviewed profile-isolation strategy.'
    $expectedRegistryViews = @('Registry32', 'Registry64')
    Assert-Stage5Condition ($document['registryViews'] -is [Array] -and
        (@($document['registryViews'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedRegistryViews -join '|')) `
        'Lockstep-v2 multiplayer evidence does not cover both reviewed registry views.'
    $expectedEnvironmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA',
        'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
        'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
        'RTS_STAGE5_VALIDATION_CACHE_ROOT',
        'RTS_STAGE5_VALIDATION_LOG_ROOT',
        'RTS_STAGE5_VALIDATION_DUMP_ROOT')
    Assert-Stage5Condition ($document['environmentVariables'] -is [Array] -and
        (@($document['environmentVariables'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedEnvironmentVariables -join '|')) `
        'Lockstep-v2 multiplayer evidence does not retain the reviewed peer environment boundary.'
    $aggregateLauncherContracts = $document['launcherEquivalence']
    Assert-Stage5JsonShape $aggregateLauncherContracts @('Generals', 'ZeroHour') `
        'Lockstep-v2 launcher-equivalence aggregate'
    foreach ($launcherTitle in @('Generals', 'ZeroHour')) {
        [void](Assert-Stage5LockstepLauncherContract $aggregateLauncherContracts[$launcherTitle] `
            $launcherTitle $ArtifactHashes "Lockstep-v2 $launcherTitle launcher-equivalence"
        )
    }

    $sessions = $document['sessions']
    Assert-Stage5Condition ($sessions -is [Array] -and $sessions.Count -eq 2) `
        'Lockstep-v2 multiplayer evidence must contain exactly the Generals and ZeroHour sessions.'
    $sessionTitles = @('Generals', 'ZeroHour')
    $seenSessionNonces = @{}
    $seenProcessIds = @{}
    $seenPorts = @{}
    $seenRunNonces = @{}
    $sessionReports = New-Object 'Collections.Generic.List[object]'
    for ($sessionIndex = 0; $sessionIndex -lt $sessions.Count; ++$sessionIndex) {
        $session = $sessions[$sessionIndex]
        $title = $sessionTitles[$sessionIndex]
        $sessionContext = "Lockstep-v2 '$title' session"
        $sessionNames = @('title', 'peerCount', 'ports', 'sessionNonce',
            'launcherEquivalence', 'titleSessionProfile',
            'registryEquivalence', 'workerProfiles', 'effectiveWorkerCounts',
            'mixedWorkerProof', 'comparableProjectionSha256', 'peers',
            'profileReadOnlyVerified', 'profileFilesAfterRun')
        Assert-Stage5JsonShape $session $sessionNames $sessionContext
        [void](Assert-Stage5LockstepLauncherContract $session['launcherEquivalence'] `
            $title $ArtifactHashes "$sessionContext launcher-equivalence")
        Assert-Stage5Condition (
            ($session['launcherEquivalence'] | ConvertTo-Json -Compress -Depth 8) -ceq
            ($aggregateLauncherContracts[$title] | ConvertTo-Json -Compress -Depth 8)) `
            "$sessionContext launcher-equivalence is not bound to the aggregate contract."
        Assert-Stage5Condition ($session['title'] -is [string] -and
            $session['title'] -ceq $title -and
            (Test-Stage5JsonInteger $session['peerCount']) -and
            [Int64]$session['peerCount'] -eq [Int64]$document['peerCount']) `
            "$sessionContext title or peer count is substituted."
        Assert-Stage5Condition ($session['sessionNonce'] -is [string] -and
            $session['sessionNonce'] -cmatch '^[0-9A-F]{32}$' -and
            -not $seenSessionNonces.ContainsKey($session['sessionNonce'])) `
            "$sessionContext has a duplicate or noncanonical session nonce."
        $seenSessionNonces[$session['sessionNonce']] = $true
        Assert-Stage5Condition ($session['ports'] -is [Array] -and
            $session['ports'].Count -eq [Int64]$document['peerCount']) `
            "$sessionContext does not list exactly one port per peer."
        $sessionPorts = @()
        for ($portIndex = 0; $portIndex -lt $session['ports'].Count; ++$portIndex) {
            $port = $session['ports'][$portIndex]
            Assert-Stage5Condition ((Test-Stage5JsonInteger $port) -and
                [Int64]$port -ge 1024 -and [Int64]$port -le 65535) `
                "$sessionContext has an invalid UDP port at peer $portIndex."
            $portKey = [string][Int64]$port
            Assert-Stage5Condition (-not $seenPorts.ContainsKey($portKey)) `
                "$sessionContext reuses UDP port $port."
            $seenPorts[$portKey] = $true
            $sessionPorts += [int]$port
        }
        Assert-Stage5Condition ($session['workerProfiles'] -is [Array] -and
            $session['workerProfiles'].Count -eq [Int64]$document['peerCount']) `
            "$sessionContext does not list one executable worker profile per peer."
        Assert-Stage5Condition ($session['effectiveWorkerCounts'] -is [Array] -and
            $session['effectiveWorkerCounts'].Count -eq [Int64]$document['peerCount']) `
            "$sessionContext does not list one effective worker count per peer."
        Assert-Stage5Condition ($session['mixedWorkerProof'] -is [bool] -and
            [bool]$session['mixedWorkerProof']) `
            "$sessionContext does not prove mixed executable worker profiles."
        for ($profileIndex = 0; $profileIndex -lt $session['workerProfiles'].Count; ++$profileIndex) {
            $profileContext = "$sessionContext worker profile $profileIndex"
            $profile = $session['workerProfiles'][$profileIndex]
            Assert-Stage5JsonShape $profile @('profile', 'requestedWorkers',
                'workerPolicy', 'overrideArguments') $profileContext
            $expectedProfile = if (($profileIndex % 2) -eq 0) {
                [ordered]@{
                    profile = 'explicit-two-workers'; requestedWorkers = '2'
                    workerPolicy = 'all'; overrideArguments = @('-workerCount', '2', '-workerPolicy', 'all')
                }
            }
            else {
                [ordered]@{
                    profile = 'automatic-workers'; requestedWorkers = 'auto'
                    workerPolicy = 'auto'; overrideArguments = @('-workerPolicy', 'auto')
                }
            }
            Assert-Stage5Condition ($profile['profile'] -ceq $expectedProfile.profile -and
                $profile['requestedWorkers'] -ceq $expectedProfile.requestedWorkers -and
                $profile['workerPolicy'] -ceq $expectedProfile.workerPolicy -and
                $profile['overrideArguments'] -is [Array] -and
                (@($profile['overrideArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
                    ($expectedProfile.overrideArguments -join '|')) `
                "$profileContext is stale, homogeneous, or substituted."
            Assert-Stage5Condition (Test-Stage5JsonInteger $session['effectiveWorkerCounts'][$profileIndex]) `
                "$sessionContext effective worker count $profileIndex is not an integer."
        }
        $sessionDirectory = Join-Path (Split-Path -Parent $full) $title
        [void](Assert-Stage5LockstepTitleSessionContract `
            $session['titleSessionProfile'] $title $sessionDirectory `
            $session['launcherEquivalence'] "$sessionContext title-session profile")
        [void](Assert-Stage5LockstepRegistryEquivalence `
            $session['registryEquivalence'] $session['titleSessionProfile'] `
            "$sessionContext registry equivalence")
        Assert-Stage5Condition ($session['profileReadOnlyVerified'] -is [bool] -and
            [bool]$session['profileReadOnlyVerified'] -and
            $session['profileFilesAfterRun'] -is [Array] -and
            $session['profileFilesAfterRun'].Count -eq 0) `
            "$sessionContext did not prove the shared title profile remained read-only."
        Assert-Stage5Condition ($session['comparableProjectionSha256'] -is [string] -and
            $session['comparableProjectionSha256'] -cmatch '^[0-9A-F]{64}$') `
            "$sessionContext has no canonical cross-peer projection hash."
        $peers = $session['peers']
        Assert-Stage5Condition ($peers -is [Array] -and
            $peers.Count -eq [Int64]$document['peerCount']) `
            "$sessionContext must contain every peer contribution."
        $referenceProjection = $null
        $peerReports = New-Object 'Collections.Generic.List[object]'
        for ($peerIndex = 0; $peerIndex -lt $peers.Count; ++$peerIndex) {
            $peer = $peers[$peerIndex]
            $peerContext = "$sessionContext peer $peerIndex"
            $peerNames = @('schemaVersion', 'producer', 'validationMode', 'title',
                'processId', 'peer', 'peerCount', 'port', 'runNonce', 'sessionNonce',
                'executableSha256', 'sourceCommit', 'launcherEquivalence',
                'launcherPath', 'launcherSha256', 'launcherConfigPath',
                'launcherConfigSha256', 'directExecutionOptIn', 'workingDirectory',
                'commandLine', 'arguments', 'launcherDefaultArguments',
                'directArguments', 'workerOverride', 'stdoutProof',
                'receiptWorkerTelemetry', 'requestedWorkers', 'workerPolicy',
                'effectiveWorkers', 'titleSessionProfile', 'registryEquivalence',
                'environmentEquivalence', 'receiptPath', 'receiptSha256', 'stdoutSha256',
                'stderrSha256', 'exitCode', 'finalFrame', 'finalCRC',
                'comparableProjectionSha256', 'lockstepV2Receipt',
                'v1ReceiptAccepted')
            Assert-Stage5JsonShape $peer $peerNames $peerContext
            foreach ($field in @('schemaVersion', 'processId', 'peer', 'peerCount',
                'port', 'exitCode', 'finalFrame', 'finalCRC')) {
                Assert-Stage5Condition (Test-Stage5JsonInteger $peer[$field]) `
                    "$peerContext field '$field' must be an integer."
            }
            $expectedExecutable = [string]$ArtifactHashes[($(if ($title -ceq 'Generals') {
                'generals-executable'
            } else { 'zerohour-executable' }))].ToUpperInvariant()
            Assert-Stage5Condition ([Int64]$peer['schemaVersion'] -eq 2 -and
                [string]$peer['producer'] -ceq 'installed-lockstep-v2' -and
                [string]$peer['validationMode'] -ceq 'installed-lockstep-v2-production' -and
                [string]$peer['title'] -ceq $title -and [Int64]$peer['peer'] -eq $peerIndex -and
                [Int64]$peer['peerCount'] -eq [Int64]$document['peerCount'] -and
                [Int64]$peer['port'] -eq $sessionPorts[$peerIndex] -and
                [Int64]$peer['processId'] -gt 0 -and
                [Int64]$peer['exitCode'] -eq 0 -and [Int64]$peer['finalFrame'] -eq 4096 -and
                [Int64]$peer['finalCRC'] -gt 0 -and
                [string]$peer['sourceCommit'] -ceq $ExpectedSourceCommit -and
                [string]$peer['executableSha256'] -match '^[0-9A-F]{64}$' -and
                [string]$peer['executableSha256'] -ceq $expectedExecutable -and
                $peer['lockstepV2Receipt'] -is [bool] -and [bool]$peer['lockstepV2Receipt'] -and
                $peer['v1ReceiptAccepted'] -is [bool] -and -not [bool]$peer['v1ReceiptAccepted']) `
                "$peerContext has a stale, substituted, or non-clean process identity."
            [void](Assert-Stage5LockstepLauncherContract $peer['launcherEquivalence'] `
                $title $ArtifactHashes "$peerContext launcher-equivalence")
            Assert-Stage5Condition (
                ($peer['launcherEquivalence'] | ConvertTo-Json -Compress -Depth 8) -ceq
                ($session['launcherEquivalence'] | ConvertTo-Json -Compress -Depth 8)) `
                "$peerContext launcher-equivalence is substituted from another title."
            Assert-Stage5Condition (
                ($peer['titleSessionProfile'] | ConvertTo-Json -Compress -Depth 12) -ceq
                ($session['titleSessionProfile'] | ConvertTo-Json -Compress -Depth 12)) `
                "$peerContext title-session profile is substituted from another session."
            [void](Assert-Stage5LockstepRegistryEquivalence `
                $peer['registryEquivalence'] $session['titleSessionProfile'] `
                "$peerContext registry equivalence")
            [void](Assert-Stage5LockstepEnvironmentEquivalence `
                $peer['environmentEquivalence'] $session['titleSessionProfile'] `
                $peerIndex "$peerContext environment equivalence")
            [void](Assert-Stage5LockstepPeerLaunchBinding $peer `
                $session['launcherEquivalence'] $title $peerIndex `
                ([int]$document['peerCount']) $sessionPorts `
                ([string]$peer['runNonce']) ([string]$session['sessionNonce']) `
                ([string]$document['mapName']) ([UInt32]$document['mapCrc']) `
                ([int]$document['seed']) $sessionDirectory $peerContext)
            $workerEvidence = Assert-Stage5LockstepWorkerEvidence $peer $peerIndex $peerContext
            Assert-Stage5Condition ($workerEvidence.expectedProfile.profile -ceq
                $session['workerProfiles'][$peerIndex]['profile'] -and
                $workerEvidence.expectedProfile.requestedWorkers -ceq
                    $session['workerProfiles'][$peerIndex]['requestedWorkers'] -and
                $workerEvidence.expectedProfile.workerPolicy -ceq
                    $session['workerProfiles'][$peerIndex]['workerPolicy'] -and
                (@($workerEvidence.expectedProfile.overrideArguments | ForEach-Object { [string]$_ }) -join '|') -ceq
                    (@($session['workerProfiles'][$peerIndex]['overrideArguments'] |
                        ForEach-Object { [string]$_ }) -join '|')) `
                "$peerContext worker override is not bound to its session profile."
            Assert-Stage5Condition ([Int64]$peer['effectiveWorkers'] -eq
                [Int64]$workerEvidence.effectiveWorkers -and
                [Int64]$session['effectiveWorkerCounts'][$peerIndex] -eq
                    [Int64]$workerEvidence.effectiveWorkers) `
                "$peerContext effective worker count is stale or substituted."
            $processKey = [string][Int64]$peer['processId']
            Assert-Stage5Condition (-not $seenProcessIds.ContainsKey($processKey)) `
                "$peerContext reuses a process identity."
            $seenProcessIds[$processKey] = $true
            Assert-Stage5Condition ($peer['runNonce'] -is [string] -and
                $peer['runNonce'] -cmatch '^[0-9A-F]{32}$' -and
                -not $seenRunNonces.ContainsKey($peer['runNonce'])) `
                "$peerContext has a duplicate or noncanonical run nonce."
            $seenRunNonces[$peer['runNonce']] = $true
            Assert-Stage5Condition ($peer['sessionNonce'] -ceq $session['sessionNonce']) `
                "$peerContext is bound to a different session nonce."
            foreach ($hashField in @('receiptSha256', 'stdoutSha256', 'stderrSha256',
                'comparableProjectionSha256')) {
                Assert-Stage5Condition ($peer[$hashField] -is [string] -and
                    $peer[$hashField] -cmatch '^[0-9A-F]{64}$') `
                    "$peerContext field '$hashField' is not a canonical SHA-256."
            }
            $receiptLeaf = "lockstep-v2-$title-peer-$peerIndex.receipt"
            Assert-Stage5Condition ($peer['receiptPath'] -is [string] -and
                [string]$peer['receiptPath'] -ceq $receiptLeaf) `
                "$peerContext receipt path is substituted."
            $receiptPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory `
                $receiptLeaf "$peerContext receipt"
            $stdoutLeaf = "peer-$peerIndex.stdout.log"
            $stderrLeaf = "peer-$peerIndex.stderr.log"
            $stdoutPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory `
                $stdoutLeaf "$peerContext stdout"
            $stderrPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory `
                $stderrLeaf "$peerContext stderr"
            Assert-Stage5Condition ((Get-Stage5FinalAcceptanceFileSha256 $receiptPath) -ceq
                [string]$peer['receiptSha256']) `
                "$peerContext receipt SHA-256 binding does not match the producer file."
            Assert-Stage5Condition ((Get-Stage5FinalAcceptanceFileSha256 $stdoutPath) -ceq
                [string]$peer['stdoutSha256']) `
                "$peerContext stdout SHA-256 binding does not match the producer file."
            Assert-Stage5Condition ((Get-Stage5FinalAcceptanceFileSha256 $stderrPath) -ceq
                [string]$peer['stderrSha256']) `
                "$peerContext stderr SHA-256 binding does not match the producer file."
            $stdoutText = [IO.File]::ReadAllText($stdoutPath)
            $stdoutLines = @($stdoutText -split "`n" | ForEach-Object {
                $_.TrimEnd("`r")
            })
            $activeMarkerLines = @($stdoutLines | Where-Object {
                $_.StartsWith('LOCKSTEP_V2_VALIDATION_ACTIVE ',
                    [StringComparison]::Ordinal)
            })
            $passMarkerLines = @($stdoutLines | Where-Object {
                $_.StartsWith('LOCKSTEP_V2_VALIDATION_PASS ',
                    [StringComparison]::Ordinal)
            })
            Assert-Stage5Condition ($activeMarkerLines.Count -eq 1 -and
                $passMarkerLines.Count -eq 1 -and
                $stdoutText -notmatch 'NET3_VALIDATION_PEER_PASS') `
                "$peerContext stdout is not an exclusive bounded lockstep-v2 clean exit."
            $activeMarkerMatch = [regex]::Match($activeMarkerLines[0],
                '^LOCKSTEP_V2_VALIDATION_ACTIVE peer=(?<peer>[0-9]+) frame_limit=(?<frame>[0-9]+)$')
            $passMarkerMatch = [regex]::Match($passMarkerLines[0],
                '^LOCKSTEP_V2_VALIDATION_PASS peer=(?<peer>[0-9]+) pid=(?<pid>[0-9]+) frame=(?<frame>[0-9]+) crc=(?<crc>[0-9A-Fa-f]{8})$')
            $expectedStdoutCrc = '{0:X8}' -f ([UInt32]$peer['finalCRC'])
            Assert-Stage5Condition ($activeMarkerMatch.Success -and
                [Int64]$activeMarkerMatch.Groups['peer'].Value -eq $peerIndex -and
                [Int64]$activeMarkerMatch.Groups['frame'].Value -eq 4096 -and
                $passMarkerMatch.Success -and
                [Int64]$passMarkerMatch.Groups['peer'].Value -eq $peerIndex -and
                [Int64]$passMarkerMatch.Groups['pid'].Value -eq [Int64]$peer['processId'] -and
                [Int64]$passMarkerMatch.Groups['frame'].Value -eq 4096 -and
                $passMarkerMatch.Groups['crc'].Value.ToUpperInvariant() -ceq $expectedStdoutCrc -and
                [Int64]$peer['stdoutProof']['pid'] -eq [Int64]$peer['processId'] -and
                [string]$peer['stdoutProof']['activeMarker'] -ceq $activeMarkerLines[0] -and
                [string]$peer['stdoutProof']['passMarker'] -ceq $passMarkerLines[0] -and
                [string]$peer['stdoutProof']['finalCrc'] -ceq $expectedStdoutCrc) `
                "$peerContext stdout proof is stale or detached from the clean executable exit."
            $receipt = Read-Stage5LockstepV2Receipt $receiptPath $peerIndex `
                ([int]$document['peerCount']) ([UInt32]$document['mapCrc']) `
                ([string]$peer['runNonce']) ([string]$session['sessionNonce']) `
                ([string]$peer['executableSha256']) $ExpectedSourceCommit
            for ($kernel = 0; $kernel -lt 6; ++$kernel) {
                $receiptMask = ConvertTo-Stage5LockstepReceiptUInt64 `
                    $receipt.parsed.pairs["kernel_${kernel}_physical_worker_mask"] `
                    "kernel_${kernel}_physical_worker_mask"
                $receiptDistinct = ConvertTo-Stage5LockstepReceiptUInt32 `
                    $receipt.parsed.pairs["kernel_${kernel}_distinct_physical_workers"] `
                    "kernel_${kernel}_distinct_physical_workers"
                Assert-Stage5Condition ([UInt64]$peer['receiptWorkerTelemetry']['physicalWorkerMasks'][$kernel] -eq
                    $receiptMask -and
                    [Int64]$peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'][$kernel] -eq
                        [Int64]$receiptDistinct) `
                    "$peerContext worker telemetry is detached from the executable receipt kernel $kernel."
            }
            Assert-Stage5Condition ([Int64]$peer['finalFrame'] -eq $receipt.finalFrame -and
                [Int64]$peer['finalCRC'] -eq $receipt.finalCRC -and
                [string]$peer['comparableProjectionSha256'] -ceq
                    [string]$receipt.projectionSha256) `
                "$peerContext aggregate fields do not bind the canonical receipt projection."
            if ($null -eq $referenceProjection) {
                $referenceProjection = $receipt.projectionSha256
            }
            else {
                Assert-Stage5Condition ($referenceProjection -ceq $receipt.projectionSha256) `
                    "$sessionContext peers disagree on CRC, checkpoint, or command digests."
            }
            $rawLeaf = "peer-$peerIndex.raw.json"
            $rawPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory $rawLeaf `
                "$peerContext raw receipt index"
            $raw = ConvertFrom-Stage5JsonDictionary $rawPath
            Assert-Stage5JsonShape $raw $peerNames "$peerContext raw receipt index"
            foreach ($field in $peerNames) {
                $aggregateValue = $peer[$field]
                $rawValue = $raw[$field]
                $aggregateJson = $aggregateValue | ConvertTo-Json -Compress -Depth 20
                $rawJson = $rawValue | ConvertTo-Json -Compress -Depth 20
                Assert-Stage5Condition ($aggregateJson -ceq $rawJson) `
                    "$peerContext raw receipt index field '$field' is substituted."
            }
            $peerReports.Add([pscustomobject]@{
                peer = $peerIndex; processId = [int]$peer['processId']
                port = [int]$peer['port']; runNonce = [string]$peer['runNonce']
                receiptSha256 = [string]$peer['receiptSha256']
                stdoutSha256 = [string]$peer['stdoutSha256']
                stderrSha256 = [string]$peer['stderrSha256']
                projectionSha256 = [string]$receipt.projectionSha256
            }) | Out-Null
        }
        Assert-Stage5Condition ($referenceProjection -ceq
            [string]$session['comparableProjectionSha256']) `
            "$sessionContext comparableProjectionSha256 is stale or substituted."
        $sessionReports.Add([pscustomobject]@{
            title = $title; peerCount = [int]$document['peerCount']
            sessionNonce = [string]$session['sessionNonce']
            comparableProjectionSha256 = [string]$referenceProjection
            peers = $peerReports.ToArray()
        }) | Out-Null
    }
    return [pscustomobject]@{
        schemaVersion = 2; evidenceKind = 'lockstep-v2-multiplayer'
        producer = 'installed-lockstep-v2'
        validationMode = 'installed-lockstep-v2-production'
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256.ToUpperInvariant()
        commonStopFrame = 4096; peerCount = [int]$document['peerCount']
        sessions = $sessionReports.ToArray()
    }
}

function Get-Stage5LockstepV2AcceptanceFailure {
    param(
        [string]$Path,
        [string]$Context,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes
    )
    try {
        [void](Read-Stage5LockstepV2Evidence $Path $ExpectedSourceCommit `
            $ExpectedArtifactSetSha256 $ArtifactHashes)
        return $null
    }
    catch {
        return "$Context failed strict lockstep-v2 final acceptance: $($_.Exception.Message)"
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
    $lockstepV2Failure = $null
    $immutableReceiptRoles = @('validation-plan', 'validation-results',
        'replay-results', 'replay-fixture-manifest', 'ai-results',
        'combined-results', 'premium-review-results', 'manual-checklist')
    $receiptFailures = New-Object 'Collections.Generic.List[string]'
    $receiptRunNonces = @{}
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
        $evidenceTitle = Get-Stage5JsonValue $document 'title' "Evidence '$kind'"
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
            if ($immutableReceiptRoles -ccontains $role) {
                try {
                    $receipt = Read-Stage5FinalAcceptanceImmutableReceipt `
                        -Path $attachmentPath -Kind $kind -Role $role `
                        -EvidenceTitle $evidenceTitle `
                        -ExpectedSourceCommit $sourceCommit `
                        -ExpectedArtifactSetSha256 $artifactSetHash `
                        -ArtifactHashes $artifactHashes `
                        -SeenRunNonces $receiptRunNonces
                    if ($null -ne $receipt.acceptanceFailure) {
                        $receiptFailures.Add($receipt.acceptanceFailure) | Out-Null
                    }
                }
                catch {
                    $receiptFailures.Add($_.Exception.Message) | Out-Null
                }
            }
            if ($kind -ceq 'mixed-worker-multiplayer' -and $role -ceq 'multiplayer-results') {
                # Read-Stage5Net3LoopbackEvidence remains available for the
                # supplementary v1 diagnostic gate. It is deliberately not a
                # final-acceptance authority. The v2 host runner is the only
                # producer accepted by the final gate; defer this failure until
                # all other evidence checks have run so negative tests remain
                # specific.
                $lockstepV2Failure = Get-Stage5LockstepV2AcceptanceFailure `
                    $attachmentPath 'Mixed-worker multiplayer attachment' `
                    $sourceCommit $artifactSetHash $artifactHashes
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
    if ($null -ne $lockstepV2Failure -and
        $lockstepV2Failure -match 'diagnostic NET3 v1') {
        throw $lockstepV2Failure
    }
    if ($receiptFailures.Count -gt 0) {
        throw ($receiptFailures -join ' | ')
    }
    if ($null -ne $lockstepV2Failure) {
        throw $lockstepV2Failure
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

# These structural and scalar helpers are part of the validation-module
# boundary. Installed-runtime validators use the same strict JSON and counter
# rules as the aggregate readers; keeping them exported avoids dot-sourcing a
# .psm1 file into a caller scope, where Export-ModuleMember is invalid and the
# private commands are unavailable.
Export-ModuleMember -Function ConvertFrom-Stage5JsonDictionary, Get-Stage5JsonValue, `
    Assert-Stage5JsonShape, Test-Stage5JsonInteger, Get-Stage5UInt64BitCount, Get-Stage5FileSha256, `
    ConvertFrom-Stage5AiCompletion, ConvertFrom-Stage5ReplayMetrics, `
    ConvertFrom-Stage5ReplayResult, Get-Stage5TimingEvidence, Assert-Stage5AiDeterminism, Assert-Stage5ReplayDeterminism, `
    Assert-Stage5AuthoritativeWorkEvidence, Assert-Stage5CollisionTimingEvidence, `
    Read-Stage5PerformanceBaseline, Measure-Stage5Performance, Invoke-Stage5RegistryRestore, `
    Invoke-Stage5RegistrySetupTransaction, `
    Test-Stage5RegistryLeafRemoval, Invoke-Stage5CreatedRegistryKeyCleanup, `
    Invoke-Stage5FinalAcceptanceAggregation, Read-Stage5Net3LoopbackEvidence, `
    Read-Stage5PerformanceScalingEvidence, Read-Stage5FinalAcceptanceImmutableReceipt, `
    Read-Stage5LockstepV2Evidence
