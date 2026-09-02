[CmdletBinding()]
param(
    [string]$SourceRoot = '',
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-ReplayWorkerPropagation {
    param([string]$Content, [string]$Context)
    $loopMarker = 'while (numProcessesRunning < maxProcesses'
    $start = $Content.IndexOf($loopMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) {
        throw "$Context is missing the replay worker launch loop."
    }
    $endMarker = 'processes.back().startProcess(command);'
    $end = $Content.IndexOf($endMarker, $start, [StringComparison]::Ordinal)
    if ($end -lt 0) {
        throw "$Context is missing the replay worker start boundary."
    }
    $block = $Content.Substring($start, $end + $endMarker.Length - $start)
    foreach ($token in @('-workerCount', '-workerPolicy', '-replay', '-pipelineMode', '-simulationMode')) {
        if ($block.IndexOf($token, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context does not propagate $token into replay child processes."
        }
    }
    if ($block.IndexOf('-jobs', [StringComparison]::Ordinal) -ge 0) {
        throw "$Context recursively propagates process-level -jobs into replay child processes."
    }
}

function Assert-ReplayResultContract {
    param([string]$Content, [string]$Context)
    $crcMarker = 'const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_RECALC);'
    $crcIndex = $Content.IndexOf($crcMarker, [StringComparison]::Ordinal)
    if ($crcIndex -lt 0) {
        throw "$Context does not assign finalCRC from the authoritative recalculated GameLogic CRC."
    }
    $resultMarker = 'SIMULATION_REPLAY_RESULT replay=\"%s\" final_frame=%u final_crc=%08X'
    $resultIndex = $Content.IndexOf($resultMarker, [StringComparison]::Ordinal)
    if ($resultIndex -lt 0) {
        throw "$Context does not emit the structured replay result contract."
    }
    if ($crcIndex -gt $resultIndex) {
        throw "$Context emits the replay result before recalculating final state."
    }
    $between = $Content.Substring($crcIndex + $crcMarker.Length,
        $resultIndex - ($crcIndex + $crcMarker.Length))
    if ($between -notmatch '^\s*simulationJobs\.setReplayResult\(finalFrame,\s*finalCRC,\s*true,\s*true\);\s*printf\("$') {
        throw "$Context does not bind executable-origin performance evidence and emit the replay result immediately after authoritative finalCRC assignment."
    }
}

function Assert-HeadlessTimingSessionContract {
    param([string]$Content, [string]$Context)
    $executeMarker = 'void GameEngine::execute()'
    $executeIndex = $Content.IndexOf($executeMarker, [StringComparison]::Ordinal)
    if ($executeIndex -lt 0) {
        throw "$Context is missing GameEngine::execute."
    }
    $loopMarker = 'while( !m_quitting )'
    $loopIndex = $Content.IndexOf($loopMarker, $executeIndex, [StringComparison]::Ordinal)
    if ($loopIndex -lt 0) {
        throw "$Context is missing the GameEngine::execute loop boundary."
    }
    $executePreamble = $Content.Substring($executeIndex, $loopIndex - $executeIndex)
    $sessionMarker = 'rts::frame_timing::Session frameTimingSession('
    if ($executePreamble.IndexOf($sessionMarker, [StringComparison]::Ordinal) -lt 0) {
        throw "$Context is missing the GameEngine::execute frame-timing session."
    }
    $modernModeMarker = 'TheGlobalData != nullptr && TheGlobalData->m_headless ? "headless" : "interactive"'
    $legacyModeMarker = 'TheGlobalData != 0 && TheGlobalData->m_headless ? "headless" : "interactive"'
    if ($executePreamble.IndexOf($modernModeMarker, [StringComparison]::Ordinal) -lt 0 -and
        $executePreamble.IndexOf($legacyModeMarker, [StringComparison]::Ordinal) -lt 0) {
        throw "$Context does not bind frame-timing mode to the live headless flag."
    }
}

function Assert-MonotonicFrameTimingResetContract {
    param([string]$HeaderContent, [string]$TestContent, [string]$Context)
    $guardMarker = 'if (frame >= m_frameEnd)'
    $guardIndex = $HeaderContent.IndexOf($guardMarker, [StringComparison]::Ordinal)
    if ($guardIndex -lt 0) {
        throw "$Context does not guard the final frame against teardown reset regression."
    }
    $activeMarker = 'm_active = false;'
    $activeIndex = $HeaderContent.IndexOf($activeMarker, $guardIndex, [StringComparison]::Ordinal)
    if ($activeIndex -lt 0) {
        throw "$Context is missing the endFrame active-state boundary."
    }
    $guardBlock = $HeaderContent.Substring($guardIndex, $activeIndex - $guardIndex)
    foreach ($stateMarker in @('m_logicFrames += frame - m_frameEnd;', 'm_frameEnd = frame;')) {
        if ($guardBlock.IndexOf($stateMarker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context does not keep '$stateMarker' inside the monotonic frame guard."
        }
    }
    foreach ($testMarker in @(
        'capture.endFrame(0); // Game teardown can reset GameLogic before EndFrame.',
        'data.back().first == 1000 && data.back().last == 1005',
        'session end preserves the final pre-reset frame range'
    )) {
        if ($TestContent.IndexOf($testMarker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing the teardown-reset timing regression assertion."
        }
    }
}

function Assert-AuthoritativeWorkManifestContract {
    param([string]$Content, [string]$Context)
    foreach ($marker in @(
        's_aiPlanningMetricsAtStart = rts::GetAIPlanningRuntimeMetrics();',
        'const rts::AIPlanningRuntimeMetrics ai = rts::GetAIPlanningRuntimeMetrics();',
        'aiParallelAuthoritativeCommits = JobMetricDelta(',
        'ai.parallelAuthoritativeCommits,',
        's_aiPlanningMetricsAtStart.parallelAuthoritativeCommits);',
        'PrintJobMetric("authoritative_commits", aiParallelAuthoritativeCommits);',
        'PrintJobMetric("shadow_executions", aiShadowMatches + aiShadowMismatches);',
        'PrintJobMetric("owner_fallbacks", aiSerialFallbacks);',
        'PrintJobMetric("ai_submitted_jobs", aiSubmittedJobs);',
        'PrintJobMetric("ai_completed_jobs", aiCompletedJobs);',
        'PrintJobMetric("ai_committed_batches", aiCommittedBatches);',
        'PrintJobMetric("ai_parallel_authoritative_commits",',
		's_directPathMetricsAtStart = GetDirectPathRuntimeMetrics();',
		's_directPathMetricsAwaitingInitialReset = TRUE;',
		'AccumulateSkirmishAITestDirectPathMetrics(&s_directPathMetricsAtStart,',
		'CAPTURE_PATH_COUNTER(authoritativeMultiWorkerCommits);',
		'const DirectPathRuntimeMetrics &path = s_directPathMetricsFrozen;',
		'PrintJobMetric("direct_executed", path.executedJobs);',
		'PrintJobMetric("direct_worker_executed", path.workerExecutedJobs);',
		'PrintJobMetric("direct_owner_helped", path.ownerHelpedJobs);',
		'PrintJobMetric("direct_authoritative_commits", path.authoritativeCommits);',
		'PrintJobMetric("direct_authoritative_multiworker_commits",',
		'PrintJobMetric("direct_unsupported_authority",',
		'PrintJobMetric("direct_stale_acceptance", path.staleAuthoritativeCommits);',
		'PrintJobMetric("direct_malformed_acceptance",',
		'PrintJobMetric("direct_validation_failures", path.validationFailures);',
		'PrintJobMetric("direct_timeouts", path.timeoutCancellations);',
		'PrintJobMetric("direct_late_drains", path.lateDrainExecutions);',
		's_ordinaryPathMetricsAtStart = GetOrdinaryPathRuntimeMetrics();',
		's_ordinaryPathMetricsAwaitingInitialReset = TRUE;',
		'AccumulateSkirmishAITestOrdinaryPathMetrics(',
		'CAPTURE_ORDINARY_PATH_COUNTER(authoritativeMultiWorkerCommits);',
		'const OrdinaryPathRuntimeMetrics &ordinaryPath =',
		'PrintJobMetric("ordinary_path_worker_executed_requests",',
		'PrintJobMetric("ordinary_path_worker_executed_range_jobs",',
		'PrintJobMetric("ordinary_path_owner_helped_range_jobs",',
		'PrintJobMetric("ordinary_path_physical_worker_mask",',
		'PrintJobMetric("ordinary_path_distinct_physical_workers",',
		'PrintJobMetric("ordinary_path_authoritative_commits",',
		'PrintJobMetric("ordinary_path_authoritative_multiworker_commits",',
		'PrintJobMetric("ordinary_path_validation_failures",',
		'PrintJobMetric("ordinary_path_timeouts",',
        's_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();',
        's_collisionMetricsAwaitingInitialReset = TRUE;',
        'AccumulateSkirmishAITestCollisionMetrics(&s_collisionMetricsAtStart,',
        'const rts::CollisionCandidateRuntimeMetrics &collision =',
        'PrintJobMetric("collision_authoritative_commits",',
        'PrintJobMetric("collision_shadow_compared_candidates",',
        'PrintJobMetric("collision_shadow_mismatches", collisionShadowMismatches);',
        'PrintJobMetric("collision_unexpected_fallbacks",',
        'PrintJobMetric("collision_committed_candidates",',
        'PrintJobMetric("collision_submitted_jobs", collisionSubmittedJobs);',
        'PrintJobMetric("collision_completed_jobs", collisionCompletedJobs);',
        'PrintJobMetric("collision_physical_worker_jobs",',
        'PrintJobMetric("collision_owner_helped_jobs", collisionOwnerHelpedJobs);',
        'PrintJobMetric("collision_physical_worker_mask",',
        'PrintJobMetric("collision_distinct_physical_workers",',
        's_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();',
        's_physicsMetricsAwaitingInitialReset = TRUE;',
        'AccumulateSkirmishAITestPhysicsMetrics(&s_physicsMetricsAtStart,',
        'PrintJobMetric("physics_authoritative_batches",',
        'PrintJobMetric("physics_submitted_jobs",',
        'PrintJobMetric("physics_completed_jobs",',
        'PrintJobMetric("physics_shadow_mismatches",',
        'PrintJobMetric("physics_shadow_prefixes",',
        'PrintJobMetric("physics_shadow_submitted_jobs",',
        'PrintJobMetric("physics_unexpected_fallbacks",',
        'PrintJobMetric("physics_circuit_breaker_trips",',
        's_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();',
        's_spatialMetricsAwaitingInitialReset = TRUE;',
        'AccumulateSkirmishAITestImmutableSpatialMetrics(&s_spatialMetricsAtStart,',
        'PrintJobMetric("spatial_successful_collections",',
        'PrintJobMetric("spatial_successful_collection_queries",',
        'PrintJobMetric("spatial_successful_collection_ranges",',
        'PrintJobMetric("spatial_multi_range_collections",',
        'PrintJobMetric("spatial_collection_submitted_jobs",',
        'PrintJobMetric("spatial_collection_physical_worker_jobs",',
        'PrintJobMetric("spatial_collection_owner_helped_jobs",',
		'PrintJobMetric("spatial_collection_physical_worker_mask",',
		'PrintJobMetric("spatial_maximum_collection_ranges",',
		'PrintJobMetric("spatial_maximum_collection_distinct_physical_workers",'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing authoritative-work marker '$marker'."
        }
    }
}

function Assert-AIPlanningFloatingPointJobContract {
    param([string]$CoreContent, [string]$GeneralsContent, [string]$Context)
    foreach ($contract in @(
        [pscustomobject]@{
            content = $CoreContent
            label = 'shared deterministic AI planning'
            markers = @(
                '#include "Lib/JobFloatingPointState.h"',
                'const JobFloatingPointScope floatingPointScope(m_floatingPointState);',
                'const JobFloatingPointState m_floatingPointState;',
                'const JobFloatingPointState floatingPointState;'
            )
        },
        [pscustomobject]@{
            content = $GeneralsContent
            label = 'Generals deterministic AI planning'
            markers = @(
                '#include "Lib/JobFloatingPointState.h"',
                'const rts::JobFloatingPointScope floatingPointScope(m_floatingPointState);',
                'const rts::JobFloatingPointState m_floatingPointState;',
                'const rts::JobFloatingPointState floatingPointState;'
            )
        }
    )) {
        foreach ($marker in $contract.markers) {
            if ($contract.content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
                throw "$Context $($contract.label) is missing owner-state capture/worker-scope marker '$marker'."
            }
        }
    }
}

function Assert-PhysicsLifecycleManifestContract {
    param([string]$Content, [string]$Context)
    foreach ($marker in @(
        '#include "Lib/PhysicsIntegrationKernel.h"',
        'rts::ResetPhysicsIntegrationRuntimeMetrics();',
        'const rts::PhysicsIntegrationRuntimeMetrics headlessPhysicsMetrics =',
        'rts::GetPhysicsIntegrationRuntimeMetrics();',
        'PHYSICS_INTEGRATION_MANIFEST authoritative_batches=%llu',
        'committed_prefixes=%llu ranges=%llu submitted_jobs=%llu completed_jobs=%llu',
        'shadow_executions=%llu shadow_prefixes=%llu shadow_ranges=%llu shadow_submitted_jobs=%llu shadow_completed_jobs=%llu',
        'shadow_mismatches=%llu owner_fallbacks=%llu ineligible_slices=%llu unexpected_fallbacks=%llu',
        'stale_rejections=%llu circuit_breaker_trips=%llu'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing physics lifecycle marker '$marker'."
        }
    }
}

function Assert-ReplaySliceManifestLifecycleContract {
    param([string]$Content, [string]$Context)
    foreach ($marker in @(
        '#include "Lib/CollisionCandidateKernel.h"',
        '#include "Lib/PhysicsIntegrationKernel.h"',
        '#include "Lib/ImmutableSpatialQueryRuntime.h"',
        'm_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();',
        'm_collisionMetricsAwaitingInitialReset = TRUE;',
        'm_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();',
        'm_physicsMetricsAwaitingInitialReset = TRUE;',
        'm_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();',
        'm_spatialMetricsAwaitingInitialReset = TRUE;',
        'if (m_started) jobs.shutdown();',
        'const rts::CollisionCandidateRuntimeMetrics collisionMetrics =',
        'const rts::PhysicsIntegrationRuntimeMetrics physicsMetrics =',
        'const rts::ImmutableSpatialRuntimeMetrics spatialMetrics =',
        'AccumulateSkirmishAITestCollisionMetrics(&m_collisionMetricsAtStart,',
        'AccumulateSkirmishAITestPhysicsMetrics(&m_physicsMetricsAtStart,',
        'AccumulateSkirmishAITestImmutableSpatialMetrics(&m_spatialMetricsAtStart,',
		'simulationJobs.captureSliceMetrics();',
        'printHeadlessReplaySliceMetrics(m_collisionMetricsFrozen,',
        'COLLISION_CANDIDATE_MANIFEST authoritative_commits=%llu shadow_executions=%llu shadow_compared_candidates=%llu',
        'collision.shadowComparedCandidates',
        'physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u',
        'collision.physicalWorkerJobs',
        'collision.ownerHelpedJobs',
        'collision.physicalWorkerMask',
        'PHYSICS_INTEGRATION_MANIFEST authoritative_batches=%llu',
        'physics.acceptedBatches',
        'physics.circuitBreakerTrips',
        'IMMUTABLE_SPATIAL_MANIFEST',
        'successful_collections',
        'collection_physical_worker_jobs',
		'collection_physical_worker_mask',
		'maximum_collection_ranges',
		'maximum_collection_distinct_physical_workers'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing replay-scope collision/physics lifecycle marker '$marker'."
        }
    }
}

function Assert-ImmutableSpatialBatchRuntimeContract {
    param([string]$KernelContent, [string]$LiveContent,
        [string]$GameLogicContent, [string]$AutoHealContent,
        [string]$PointDefenseContent, [string]$Context)
    foreach ($marker in @(
        'return queryCount >= 2 && workerCount >= 2',
        'executionOptions.workerCount = jobs.workerCount();',
		'jobs.waitWithoutOwnerHelp(group,',
		'IMMUTABLE_SPATIAL_PHYSICAL_WAIT_MILLISECONDS',
		'jobs.cancel(group);',
		'jobs.wait(group);',
        'SPATIAL_PHYSICAL_WORKER',
		'jobContext.physicalWorkerIndex()',
		'physicalWorkerMask',
		'distinctPhysicalWorkers',
        'jobMetrics->ownerHelpedJobs == 0',
        'RecordImmutableSpatialSuccessfulCollection(unsigned queryCount',
        'addMetric(s_successfulCollectionQueries, queryCount);',
        'addMetric(s_successfulCollectionRanges, rangeCount);'
    )) {
        if ($KernelContent.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context shared spatial kernel is missing batching marker '$marker'."
        }
    }
    foreach ($marker in @(
        'LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES = 256',
		'PreflightLiveImmutableSpatialQueryScheduler()',
		'PreflightLiveImmutableSpatialQueryCollection(',
		'queueableQueryCount > LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES',
		'return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;',
		'void PublishLiveImmutableSpatialNoCaptureState(',
		'liveSpatialRuntime().publishNoCaptureState(result, manager, frame);',
        'queueableQueryCount, jobs.workerCount())',
        'return liveSpatialRuntime().canQueueConsumer(consumer);',
        'if (m_queryCount < 2)',
        'std::min<UnsignedInt>(',
        'jobs.workerCount(), m_queryCount',
        'std::sort(m_ownerIndex, m_ownerIndex + m_queryCount, ownerIndexLess);',
        'ExecuteImmutableSpatialQueryBatchOnJobSystem(m_arena,',
        'RecordImmutableSpatialSuccessfulCollection(m_queryCount,',
        'prepared.owner != owner',
        'owner->friend_getPriority() != prepared.wakePriority',
        'owner->friend_getNextCallFrame() != frame',
        'view->queryOrdinal = batchIndex;',
        'view->batchEpoch = m_batchEpoch;'
    )) {
        if ($LiveContent.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context live spatial runtime is missing owner-batch marker '$marker'."
        }
    }
    if ($LiveContent.IndexOf('LIVE_IMMUTABLE_SPATIAL_MAXIMUM_PHYSICAL_WORKERS',
            [StringComparison]::Ordinal) -ge 0) {
        throw "$Context live spatial runtime retains a subsystem-specific physical-worker cap."
    }
    foreach ($marker in @(
        'UnsignedInt countImmutableSpatialQueryCollection(',
        'void prepareImmutableSpatialQueryCollection(',
        'canQueueImmutableSpatialQuery();',
        'BeginLiveImmutableSpatialQueryCollection(queueableQueryCount)',
        'static_cast<PointDefenseLaserUpdate *>(update)->',
        'ExecuteLiveImmutableSpatialQueryCollection();',
		'PreflightLiveImmutableSpatialQueryScheduler();',
		'PreflightLiveImmutableSpatialQueryCollection(',
		'PublishLiveImmutableSpatialNoCaptureState(preflight,',
        'CaptureLiveImmutableSpatialArena(ThePartitionManager, now)',
		'if (ShouldCaptureLiveImmutableSpatialArena(',
		'queueableQueryCount = countImmutableSpatialQueryCollection(',
		'm_sleepyUpdates, s_immutableSpatialQueries, now,',
		'prepareImmutableSpatialQueryCollection(s_immutableSpatialQueries, now,'
    )) {
        if ($GameLogicContent.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context GameLogic is missing spatial collection marker '$marker'."
        }
    }
	$spatialSchedulerPreflightIndex = $GameLogicContent.IndexOf(
		'PreflightLiveImmutableSpatialQueryScheduler();', [StringComparison]::Ordinal)
	$spatialCountIndex = if ($spatialSchedulerPreflightIndex -ge 0) {
		$GameLogicContent.IndexOf('countImmutableSpatialQueryCollection(',
			$spatialSchedulerPreflightIndex, [StringComparison]::Ordinal)
	} else { -1 }
	$spatialPreflightIndex = $GameLogicContent.IndexOf(
		'PreflightLiveImmutableSpatialQueryCollection(', [StringComparison]::Ordinal)
    $spatialCaptureIndex = $GameLogicContent.IndexOf(
        'CaptureLiveImmutableSpatialArena(ThePartitionManager, now)',
        [StringComparison]::Ordinal)
	if ($spatialSchedulerPreflightIndex -lt 0 -or $spatialCountIndex -lt 0 -or
		$spatialPreflightIndex -lt 0 -or
		$spatialSchedulerPreflightIndex -ge $spatialCountIndex -or
		$spatialCountIndex -ge $spatialPreflightIndex -or
		$spatialPreflightIndex -ge $spatialCaptureIndex) {
		throw "$Context GameLogic does not admit the scheduler before heap scan and the stable queueable collection before arena capture."
    }
    foreach ($consumerContract in @(
        [pscustomobject]@{
            content = $AutoHealContent
            label = 'AutoHeal'
            markers = @('Bool AutoHealBehavior::canQueueImmutableSpatialQuery()',
                'Bool AutoHealBehavior::queueImmutableSpatialQuery()',
                'IsLiveImmutableSpatialConsumerQueueable(',
                'QueueLiveImmutableSpatialQuery( this, ThePartitionManager,',
                'QueryLiveImmutableSpatialCandidates( this, ThePartitionManager,',
                'ThePartitionManager->iterateObjectsInRange(')
        },
        [pscustomobject]@{
            content = $PointDefenseContent
            label = 'PointDefense'
            markers = @('Bool PointDefenseLaserUpdate::canQueueImmutableSpatialQuery()',
                'Bool PointDefenseLaserUpdate::queueImmutableSpatialQuery()',
                'IsLiveImmutableSpatialConsumerQueueable(',
                'QueueLiveImmutableSpatialQuery( this, ThePartitionManager,',
                'QueryLiveImmutableSpatialCandidates( this, ThePartitionManager,',
                'return scanClosestTarget();')
        }
    )) {
        foreach ($marker in $consumerContract.markers) {
            if ($consumerContract.content.IndexOf($marker,
                [StringComparison]::Ordinal) -lt 0) {
                throw "$Context $($consumerContract.label) is missing spatial owner/oracle marker '$marker'."
            }
        }
    }
}

function Assert-EarlyPhysicsIntegrationPreflightContract {
	param([string]$KernelContent, [string]$GameLogicContent, [string]$Context)
	foreach ($marker in @('PreflightPhysicsIntegrationPrefixes()',
		'if (!jobs.isRunning() || jobs.isWorkerThread()',
		'if (jobs.workerCount() <= 1)',
		'return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;')) {
		if ($KernelContent.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
			throw "$Context physics kernel is missing early scheduler marker '$marker'."
		}
	}
	foreach ($marker in @('rts::PreflightPhysicsIntegrationPrefixes();',
		'preparePhysicsIntegrationBatch(this, m_sleepyUpdates,')) {
		if ($GameLogicContent.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
			throw "$Context GameLogic is missing early physics marker '$marker'."
		}
	}
	$preflightIndex = $GameLogicContent.IndexOf(
		'rts::PreflightPhysicsIntegrationPrefixes();', [StringComparison]::Ordinal)
	$scanIndex = $GameLogicContent.IndexOf(
		'preparePhysicsIntegrationBatch(this, m_sleepyUpdates,',
		[StringComparison]::Ordinal)
	if ($preflightIndex -lt 0 -or $scanIndex -lt 0 -or $preflightIndex -ge $scanIndex) {
		throw "$Context GameLogic does not reject an ineligible scheduler before physics heap scanning."
	}
}

function Assert-ReplayDestructorManifestSuppressionContract {
    param([string]$Content, [string]$Context)
    foreach ($marker in @(
        'const Bool reportHeadlessMetrics = TheGlobalData->m_headless &&',
        'TheGlobalData->m_simulateReplays.empty();',
        'if (reportHeadlessMetrics)',
        'printHeadlessSimulationJobMetrics(headlessMetrics, headlessPhysicsMetrics,',
        'headlessStatusMetrics);'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing replay destructor-manifest suppression marker '$marker'."
        }
    }
}

function Assert-SliceMetricFreezeTestContract {
    param([string]$Content, [string]$Context)
    foreach ($marker in @(
        'AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,',
        'collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 0',
        '!collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 3',
        'collisionFrozen.committedCandidates == 12',
        'collisionFrozen.physicalWorkerJobs == 3',
        'collisionFrozen.ownerHelpedJobs == 1',
        'collisionFrozen.physicalWorkerMask == 5',
        'AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,',
        'physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 0',
        '!physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 5',
        'physicsFrozen.acceptedPrefixes == 96',
        'physicsFrozen.shadowBatches == 2',
		'physicsFrozen.shadowPrefixes == 48',
		'AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,',
		'spatialFrozen.collectionPhysicalWorkerMask == 3',
		'spatialFrozen.maximumCollectionDistinctPhysicalWorkers == 2'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing collision/physics reset-epoch freeze marker '$marker'."
        }
    }
}

function Assert-PathMetricFreezeTestContract {
    param([string]$Content, [string]$ExpectedWorkerMarker, [string]$Context)
    foreach ($marker in @(
        'AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,',
        $ExpectedWorkerMarker,
        'Bool awaitingInitialReset = TRUE;',
        'CHECK(awaitingInitialReset && !hasFrozenActivity &&',
        'CHECK(!awaitingInitialReset && hasFrozenActivity &&',
		'current.timeoutCancellations = 1;',
		'frozen.timeoutCancellations == 1',
        'baseline = current;',
        'CHECK(frozen.workerExecutedJobs ==',
        'memset(&frozen, 0, sizeof(frozen));',
        'CHECK(!awaitingInitialReset && !hasFrozenActivity &&',
		'baseline.resetEpoch ==',
		'frozen.timeoutCancellations == 0',
		'AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,',
		'ordinaryFrozen.workerExecutedRangeJobs == 4',
		'ordinaryFrozen.physicalWorkerMask == 5',
		'ordinaryFrozen.authoritativeCommits == 3',
		'ordinaryFrozen.authoritativeMultiWorkerCommits == 2',
		'ordinaryFrozen.maximumBatchRequests == 6',
		'ordinaryFrozen.authoritativeCommits == 0'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
			throw "$Context is missing direct/ordinary-path metric freeze marker '$marker'."
        }
    }
}

function Assert-CollisionLifecycleManifestContract {
    param([string]$Content, [string]$Context)
    foreach ($marker in @(
        '#include "Lib/CollisionCandidateKernel.h"',
        'const rts::CollisionCandidateRuntimeMetrics collision =',
        'COLLISION_CANDIDATE_MANIFEST authoritative_commits=%llu',
        'shadow_executions=%llu shadow_compared_candidates=%llu',
        'shadow_mismatches=%llu owner_fallbacks=%llu unexpected_fallbacks=%llu',
        'committed_candidates=%llu prepared_pairs=%llu unique_candidates=%llu',
        'submitted_jobs=%llu completed_jobs=%llu'
    )) {
        if ($Content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing collision lifecycle marker '$marker'."
        }
    }
}

function Assert-CollisionResetEpochContract {
    param([string]$HeaderContent, [string]$KernelContent,
        [string]$GameLogicContent, [string]$Context)
    foreach ($marker in @(
        'JobMetricCounter resetEpoch;',
        'JobMetricCounter nextEpoch = s_runtimeMetrics.resetEpoch + 1;',
        's_runtimeMetrics.resetEpoch = nextEpoch;',
        'rts::ResetCollisionCandidateRuntimeMetrics();',
        'rts::ResetPhysicsIntegrationRuntimeMetrics();'
    )) {
        $content = if ($marker -ceq 'JobMetricCounter resetEpoch;') { $HeaderContent }
            elseif ($marker -match 's_runtimeMetrics') { $KernelContent }
            else { $GameLogicContent }
        if ($content.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing collision reset-epoch marker '$marker'."
        }
    }
}

function Assert-CollisionAdapterContract {
    param([string]$AdapterContent, [string]$KernelContent, [string]$Context)
    foreach ($marker in @(
        'class LivePartitionCollisionWorkspace',
        'rts::CollisionAdmissionSampler admissionSampler;',
        'admissionSampler.hasUsefulSpread()',
        'rts::RecordCollisionCandidateIneligibleSlice();',
        'rts::frame_timing::CollisionAdmission',
        'rts::frame_timing::SimulationSnapshot',
        'rts::frame_timing::SimulationParallel',
        'rts::RecordCollisionCandidateParallelWork(',
        'rts::frame_timing::CollisionLiveValidation',
        'ctList->containsContact(this,',
        'rts::frame_timing::CollisionExistingFilter',
        'rts::frame_timing::SimulationCommit',
        'rts::frame_timing::CollisionCommitPrepare',
        'rts::frame_timing::SimulationShadowCompare',
        'SIMULATION_COLLISION_MISMATCH frame=%u phase=partition_contact_commit item=%u diff=%s'
    )) {
        if ($AdapterContent.IndexOf($marker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing collision adapter marker '$marker'."
        }
    }
    foreach ($kernelMarker in @(
        'rts::frame_timing::SimulationWait',
        'rts::frame_timing::SimulationReduce'
    )) {
        if ($KernelContent.IndexOf($kernelMarker, [StringComparison]::Ordinal) -lt 0) {
            throw "$Context is missing collision kernel marker '$kernelMarker'."
        }
    }
}

if ($SelfTest) {
    $valid = @'
while (numProcessesRunning < maxProcesses && filenamePositionStarted < filenames.size())
{
    command.format("-workerCount %u -workerPolicy %s -pipelineMode %s -simulationMode %s -replay %s");
    processes.back().startProcess(command);
}
'@
    Assert-ReplayWorkerPropagation $valid 'valid fixture'
    foreach ($missing in @('-workerCount', '-pipelineMode', '-simulationMode', '-workerPolicy', '-replay')) {
        $malformed = $valid.Replace($missing, '-removed')
        try {
            Assert-ReplayWorkerPropagation $malformed "missing $missing fixture"
            throw "Self-test failed to reject missing $missing propagation."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    try {
        Assert-ReplayWorkerPropagation ($valid.Replace('-replay %s', '-jobs 4 -replay %s')) `
            'recursive jobs fixture'
        throw 'Self-test failed to reject recursive -jobs propagation.'
    }
    catch {
        if ($_.Exception.Message -like 'Self-test failed*') { throw }
    }
    $validResult = @'
const UnsignedInt finalFrame = TheGameLogic->getFrame();
const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_RECALC);
simulationJobs.setReplayResult(finalFrame, finalCRC, true, true);
printf("SIMULATION_REPLAY_RESULT replay=\"%s\" final_frame=%u final_crc=%08X\n", replay, finalFrame, finalCRC);
'@
    Assert-ReplayResultContract $validResult 'valid result fixture'
    $wrongOrderResult = @'
printf("SIMULATION_REPLAY_RESULT replay=\"%s\" final_frame=%u final_crc=%08X\n", replay, finalFrame, finalCRC);
const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_RECALC);
'@
    $unrelatedRecalcResult = @'
TheGameLogic->getCRC(CRC_RECALC);
const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_CACHED);
printf("SIMULATION_REPLAY_RESULT replay=\"%s\" final_frame=%u final_crc=%08X\n", replay, finalFrame, finalCRC);
'@
    foreach ($malformedResult in @(
        $validResult.Replace('getCRC(CRC_RECALC)', 'getCRC(CRC_CACHED)'),
        $validResult.Replace('SIMULATION_REPLAY_RESULT', 'REMOVED_REPLAY_RESULT'),
        $validResult.Replace('simulationJobs.setReplayResult(finalFrame, finalCRC, true, true);', ''),
        $validResult.Replace('finalCRC, true, true', 'finalCRC, false, true'),
        $wrongOrderResult,
        $unrelatedRecalcResult
    )) {
        try {
            Assert-ReplayResultContract $malformedResult 'malformed result fixture'
            throw 'Self-test failed to reject a malformed replay result contract.'
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validTimingSession = @'
void GameEngine::execute()
{
    rts::frame_timing::Session frameTimingSession(
        TheGlobalData != nullptr && TheGlobalData->m_headless ? "headless" : "interactive");
    while( !m_quitting )
    {
    }
}
'@
    Assert-HeadlessTimingSessionContract $validTimingSession 'valid timing-session fixture'
    Assert-HeadlessTimingSessionContract ($validTimingSession.Replace('!= nullptr', '!= 0')) `
        'valid VC6 timing-session fixture'
    foreach ($malformedTimingSession in @(
        $validTimingSession.Replace(
            'TheGlobalData != nullptr && TheGlobalData->m_headless ? "headless" : "interactive"',
            '"interactive"'),
        $validTimingSession.Replace('rts::frame_timing::Session frameTimingSession(',
            'REMOVED_FRAME_TIMING_SESSION(')
    )) {
        try {
            Assert-HeadlessTimingSessionContract $malformedTimingSession `
                'malformed timing-session fixture'
            throw 'Self-test failed to reject a malformed headless timing-session contract.'
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validResetHeader = @'
if (frame >= m_frameEnd)
{
    m_logicFrames += frame - m_frameEnd;
    m_frameEnd = frame;
}
m_active = false;
'@
    $validResetTest = @'
capture.endFrame(0); // Game teardown can reset GameLogic before EndFrame.
check(data.back().first == 1000 && data.back().last == 1005,
    "session end preserves the final pre-reset frame range");
'@
    Assert-MonotonicFrameTimingResetContract $validResetHeader $validResetTest `
        'valid teardown-reset fixture'
    foreach ($malformedReset in @(
        [pscustomobject]@{ header = $validResetHeader.Replace(
            'if (frame >= m_frameEnd)', 'if (true)'); test = $validResetTest },
        [pscustomobject]@{ header = $validResetHeader; test = $validResetTest.Replace(
            'capture.endFrame(0);', 'capture.endFrame(1005);') }
    )) {
        try {
            Assert-MonotonicFrameTimingResetContract $malformedReset.header $malformedReset.test `
                'malformed teardown-reset fixture'
            throw 'Self-test failed to reject a malformed teardown-reset contract.'
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validWorkManifest = @'
s_aiPlanningMetricsAtStart = rts::GetAIPlanningRuntimeMetrics();
const rts::AIPlanningRuntimeMetrics ai = rts::GetAIPlanningRuntimeMetrics();
aiParallelAuthoritativeCommits = JobMetricDelta(
ai.parallelAuthoritativeCommits,
s_aiPlanningMetricsAtStart.parallelAuthoritativeCommits);
PrintJobMetric("authoritative_commits", aiParallelAuthoritativeCommits);
PrintJobMetric("shadow_executions", aiShadowMatches + aiShadowMismatches);
PrintJobMetric("owner_fallbacks", aiSerialFallbacks);
PrintJobMetric("ai_submitted_jobs", aiSubmittedJobs);
PrintJobMetric("ai_completed_jobs", aiCompletedJobs);
PrintJobMetric("ai_committed_batches", aiCommittedBatches);
PrintJobMetric("ai_parallel_authoritative_commits",
s_directPathMetricsAtStart = GetDirectPathRuntimeMetrics();
s_directPathMetricsAwaitingInitialReset = TRUE;
AccumulateSkirmishAITestDirectPathMetrics(&s_directPathMetricsAtStart,
CAPTURE_PATH_COUNTER(authoritativeMultiWorkerCommits);
const DirectPathRuntimeMetrics &path = s_directPathMetricsFrozen;
PrintJobMetric("direct_executed", path.executedJobs);
PrintJobMetric("direct_worker_executed", path.workerExecutedJobs);
PrintJobMetric("direct_owner_helped", path.ownerHelpedJobs);
PrintJobMetric("direct_authoritative_commits", path.authoritativeCommits);
PrintJobMetric("direct_authoritative_multiworker_commits",
PrintJobMetric("direct_unsupported_authority",
PrintJobMetric("direct_stale_acceptance", path.staleAuthoritativeCommits);
PrintJobMetric("direct_malformed_acceptance",
PrintJobMetric("direct_validation_failures", path.validationFailures);
PrintJobMetric("direct_timeouts", path.timeoutCancellations);
PrintJobMetric("direct_late_drains", path.lateDrainExecutions);
s_ordinaryPathMetricsAtStart = GetOrdinaryPathRuntimeMetrics();
s_ordinaryPathMetricsAwaitingInitialReset = TRUE;
AccumulateSkirmishAITestOrdinaryPathMetrics(
CAPTURE_ORDINARY_PATH_COUNTER(authoritativeMultiWorkerCommits);
const OrdinaryPathRuntimeMetrics &ordinaryPath =
PrintJobMetric("ordinary_path_worker_executed_requests",
PrintJobMetric("ordinary_path_worker_executed_range_jobs",
PrintJobMetric("ordinary_path_owner_helped_range_jobs",
PrintJobMetric("ordinary_path_physical_worker_mask",
PrintJobMetric("ordinary_path_distinct_physical_workers",
PrintJobMetric("ordinary_path_authoritative_commits",
PrintJobMetric("ordinary_path_authoritative_multiworker_commits",
PrintJobMetric("ordinary_path_validation_failures",
PrintJobMetric("ordinary_path_timeouts",
s_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();
s_collisionMetricsAwaitingInitialReset = TRUE;
AccumulateSkirmishAITestCollisionMetrics(&s_collisionMetricsAtStart,
const rts::CollisionCandidateRuntimeMetrics &collision =
    rts::GetCollisionCandidateRuntimeMetrics();
PrintJobMetric("collision_authoritative_commits", collisionAuthoritativeCommits);
PrintJobMetric("collision_shadow_compared_candidates", collisionShadowComparedCandidates);
PrintJobMetric("collision_shadow_mismatches", collisionShadowMismatches);
PrintJobMetric("collision_unexpected_fallbacks", collisionUnexpectedFallbacks);
PrintJobMetric("collision_committed_candidates", collisionCommittedCandidates);
PrintJobMetric("collision_submitted_jobs", collisionSubmittedJobs);
PrintJobMetric("collision_completed_jobs", collisionCompletedJobs);
PrintJobMetric("collision_physical_worker_jobs", collisionPhysicalWorkerJobs);
PrintJobMetric("collision_owner_helped_jobs", collisionOwnerHelpedJobs);
PrintJobMetric("collision_physical_worker_mask", collisionPhysicalWorkerMask);
PrintJobMetric("collision_distinct_physical_workers", collisionDistinctPhysicalWorkers);
s_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();
s_physicsMetricsAwaitingInitialReset = TRUE;
AccumulateSkirmishAITestPhysicsMetrics(&s_physicsMetricsAtStart,
PrintJobMetric("physics_authoritative_batches",
PrintJobMetric("physics_submitted_jobs",
PrintJobMetric("physics_completed_jobs",
PrintJobMetric("physics_shadow_mismatches",
PrintJobMetric("physics_shadow_prefixes",
PrintJobMetric("physics_shadow_submitted_jobs",
PrintJobMetric("physics_unexpected_fallbacks",
PrintJobMetric("physics_circuit_breaker_trips",
s_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();
s_spatialMetricsAwaitingInitialReset = TRUE;
AccumulateSkirmishAITestImmutableSpatialMetrics(&s_spatialMetricsAtStart,
PrintJobMetric("spatial_successful_collections",
PrintJobMetric("spatial_successful_collection_queries",
PrintJobMetric("spatial_successful_collection_ranges",
PrintJobMetric("spatial_multi_range_collections",
PrintJobMetric("spatial_collection_submitted_jobs",
PrintJobMetric("spatial_collection_physical_worker_jobs",
PrintJobMetric("spatial_collection_owner_helped_jobs",
PrintJobMetric("spatial_collection_physical_worker_mask",
PrintJobMetric("spatial_maximum_collection_ranges",
PrintJobMetric("spatial_maximum_collection_distinct_physical_workers",
'@
    Assert-AuthoritativeWorkManifestContract $validWorkManifest `
        'valid authoritative-work fixture'
    foreach ($missingWorkMarker in @('authoritative_commits',
		'ai.parallelAuthoritativeCommits', 'ai_submitted_jobs',
		'ai_committed_batches', 'ai_parallel_authoritative_commits',
		'direct_worker_executed', 'direct_authoritative_commits',
		'direct_authoritative_multiworker_commits',
		'authoritativeMultiWorkerCommits',
		'direct_validation_failures', 'direct_timeouts', 'direct_late_drains',
		'direct_stale_acceptance',
		'ordinary_path_worker_executed_requests',
		'ordinary_path_worker_executed_range_jobs',
		'ordinary_path_owner_helped_range_jobs',
		'ordinary_path_physical_worker_mask',
		'ordinary_path_distinct_physical_workers',
		'ordinary_path_authoritative_commits',
		'ordinary_path_authoritative_multiworker_commits',
		'CAPTURE_ORDINARY_PATH_COUNTER',
		'ordinary_path_validation_failures', 'ordinary_path_timeouts',
		's_ordinaryPathMetricsAwaitingInitialReset',
		'collision_authoritative_commits',
        'collision_shadow_compared_candidates', 'collision_unexpected_fallbacks',
        'collision_completed_jobs', 'collision_physical_worker_jobs',
        'collision_owner_helped_jobs', 'collision_physical_worker_mask',
        'collision_distinct_physical_workers',
        'physics_authoritative_batches', 'physics_unexpected_fallbacks',
		's_collisionMetricsAwaitingInitialReset',
		's_physicsMetricsAwaitingInitialReset',
		's_spatialMetricsAwaitingInitialReset',
		'spatial_successful_collections',
		'spatial_collection_physical_worker_jobs',
		'spatial_collection_physical_worker_mask',
		'spatial_maximum_collection_distinct_physical_workers')) {
        try {
            Assert-AuthoritativeWorkManifestContract `
                ($validWorkManifest.Replace($missingWorkMarker, 'removed_metric')) `
                'malformed authoritative-work fixture'
            throw "Self-test failed to reject missing $missingWorkMarker work evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validCoreAiFloatingPoint = @'
#include "Lib/JobFloatingPointState.h"
const JobFloatingPointScope floatingPointScope(m_floatingPointState);
const JobFloatingPointState m_floatingPointState;
const JobFloatingPointState floatingPointState;
'@
    $validGeneralsAiFloatingPoint = @'
#include "Lib/JobFloatingPointState.h"
const rts::JobFloatingPointScope floatingPointScope(m_floatingPointState);
const rts::JobFloatingPointState m_floatingPointState;
const rts::JobFloatingPointState floatingPointState;
'@
    Assert-AIPlanningFloatingPointJobContract $validCoreAiFloatingPoint `
        $validGeneralsAiFloatingPoint 'valid AI floating-point fixture'
    foreach ($malformedAiFloatingPoint in @(
        [pscustomobject]@{
            core = $validCoreAiFloatingPoint.Replace(
                'const JobFloatingPointScope floatingPointScope(m_floatingPointState);',
                'removed_core_worker_scope')
            generals = $validGeneralsAiFloatingPoint
        },
        [pscustomobject]@{
            core = $validCoreAiFloatingPoint
            generals = $validGeneralsAiFloatingPoint.Replace(
                'const rts::JobFloatingPointState floatingPointState;',
                'removed_generals_owner_capture')
        }
    )) {
        try {
            Assert-AIPlanningFloatingPointJobContract $malformedAiFloatingPoint.core `
                $malformedAiFloatingPoint.generals 'malformed AI floating-point fixture'
            throw 'Self-test failed to reject a malformed AI floating-point job contract.'
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validPhysicsLifecycle = @'
#include "Lib/PhysicsIntegrationKernel.h"
rts::ResetPhysicsIntegrationRuntimeMetrics();
const rts::PhysicsIntegrationRuntimeMetrics headlessPhysicsMetrics =
    rts::GetPhysicsIntegrationRuntimeMetrics();
printf("PHYSICS_INTEGRATION_MANIFEST authoritative_batches=%llu committed_prefixes=%llu ranges=%llu submitted_jobs=%llu completed_jobs=%llu allocated_bytes=%llu capture_ns=%llu prepare_ns=%llu wait_ns=%llu commit_ns=%llu storage_bytes=%llu storage_capacity_bytes=%llu storage_allocations=%llu shadow_executions=%llu shadow_prefixes=%llu shadow_ranges=%llu shadow_submitted_jobs=%llu shadow_completed_jobs=%llu shadow_matches=%llu shadow_mismatches=%llu owner_fallbacks=%llu ineligible_slices=%llu unexpected_fallbacks=%llu stale_rejections=%llu circuit_breaker_trips=%llu\n");
'@
    Assert-PhysicsLifecycleManifestContract $validPhysicsLifecycle `
        'valid physics lifecycle fixture'
    foreach ($missingPhysicsMarker in @('ResetPhysicsIntegrationRuntimeMetrics',
        'authoritative_batches', 'unexpected_fallbacks', 'circuit_breaker_trips')) {
        try {
            Assert-PhysicsLifecycleManifestContract `
                ($validPhysicsLifecycle.Replace($missingPhysicsMarker,
                    'removed_physics_marker')) 'malformed physics lifecycle fixture'
            throw "Self-test failed to reject missing $missingPhysicsMarker physics evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
$validPathFreezeTest = @'
Bool awaitingInitialReset = TRUE;
AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
CHECK(awaitingInitialReset && !hasFrozenActivity &&
current.timeoutCancellations = 1;
CHECK(!awaitingInitialReset && hasFrozenActivity &&
frozen.timeoutCancellations == 1
baseline = current;
CHECK(frozen.workerExecutedJobs == 3 && frozen.authoritativeCommits == 2 &&
memset(&frozen, 0, sizeof(frozen));
CHECK(!awaitingInitialReset && !hasFrozenActivity &&
baseline.resetEpoch == 13 &&
frozen.timeoutCancellations == 0
AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
ordinaryFrozen.workerExecutedRangeJobs == 4
ordinaryFrozen.physicalWorkerMask == 5
ordinaryFrozen.authoritativeCommits == 3
ordinaryFrozen.authoritativeMultiWorkerCommits == 2
ordinaryFrozen.maximumBatchRequests == 6
ordinaryFrozen.authoritativeCommits == 0
'@
    Assert-PathMetricFreezeTestContract $validPathFreezeTest `
        'frozen.workerExecutedJobs == 3' 'valid direct-path freeze fixture'
    foreach ($missingPathFreezeMarker in @('AccumulateSkirmishAITestDirectPathMetrics',
        'awaitingInitialReset = TRUE', 'CHECK(awaitingInitialReset',
		'timeoutCancellations = 1', 'frozen.timeoutCancellations == 1',
        'CHECK(!awaitingInitialReset', 'baseline = current', 'memset(&frozen',
		'frozen.timeoutCancellations == 0',
		'baseline.resetEpoch ==',
		'AccumulateSkirmishAITestOrdinaryPathMetrics',
		'ordinaryFrozen.workerExecutedRangeJobs == 4',
		'ordinaryFrozen.physicalWorkerMask == 5',
		'ordinaryFrozen.authoritativeCommits == 3',
		'ordinaryFrozen.authoritativeMultiWorkerCommits == 2',
		'ordinaryFrozen.maximumBatchRequests == 6',
		'ordinaryFrozen.authoritativeCommits == 0')) {
        try {
            Assert-PathMetricFreezeTestContract `
                ($validPathFreezeTest.Replace($missingPathFreezeMarker,
                    'removed_path_freeze_marker')) 'frozen.workerExecutedJobs == 3' `
                    'malformed direct-path freeze fixture'
            throw "Self-test failed to reject missing $missingPathFreezeMarker path freeze evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validCollisionLifecycle = @'
#include "Lib/CollisionCandidateKernel.h"
const rts::CollisionCandidateRuntimeMetrics collision =
    rts::GetCollisionCandidateRuntimeMetrics();
printf("COLLISION_CANDIDATE_MANIFEST authoritative_commits=%llu shadow_executions=%llu shadow_compared_candidates=%llu shadow_mismatches=%llu owner_fallbacks=%llu unexpected_fallbacks=%llu ineligible_slices=%llu stale_rejections=%llu committed_candidates=%llu prepared_pairs=%llu unique_candidates=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u\n");
'@
    Assert-CollisionLifecycleManifestContract $validCollisionLifecycle `
        'valid collision lifecycle fixture'
    foreach ($missingCollisionMarker in @('shadow_compared_candidates',
        'unexpected_fallbacks', 'committed_candidates',
        'completed_jobs')) {
        try {
            Assert-CollisionLifecycleManifestContract `
                ($validCollisionLifecycle.Replace($missingCollisionMarker,
                    'removed_collision_marker')) 'malformed collision lifecycle fixture'
            throw "Self-test failed to reject missing $missingCollisionMarker collision evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
$validReplaySliceLifecycle = @'
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
m_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();
m_collisionMetricsAwaitingInitialReset = TRUE;
m_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();
m_physicsMetricsAwaitingInitialReset = TRUE;
m_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();
m_spatialMetricsAwaitingInitialReset = TRUE;
if (m_started) jobs.shutdown();
const rts::CollisionCandidateRuntimeMetrics collisionMetrics =
const rts::PhysicsIntegrationRuntimeMetrics physicsMetrics =
const rts::ImmutableSpatialRuntimeMetrics spatialMetrics =
AccumulateSkirmishAITestCollisionMetrics(&m_collisionMetricsAtStart,
AccumulateSkirmishAITestPhysicsMetrics(&m_physicsMetricsAtStart,
AccumulateSkirmishAITestImmutableSpatialMetrics(&m_spatialMetricsAtStart,
simulationJobs.captureSliceMetrics();
printHeadlessReplaySliceMetrics(m_collisionMetricsFrozen,
printf("COLLISION_CANDIDATE_MANIFEST authoritative_commits=%llu shadow_executions=%llu shadow_compared_candidates=%llu");
collision.shadowComparedCandidates
physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u
collision.physicalWorkerJobs
collision.ownerHelpedJobs
collision.physicalWorkerMask
printf("PHYSICS_INTEGRATION_MANIFEST authoritative_batches=%llu");
physics.acceptedBatches
physics.circuitBreakerTrips
IMMUTABLE_SPATIAL_MANIFEST
successful_collections
collection_physical_worker_jobs
collection_physical_worker_mask
maximum_collection_ranges
maximum_collection_distinct_physical_workers
'@
    Assert-ReplaySliceManifestLifecycleContract $validReplaySliceLifecycle `
        'valid replay slice lifecycle fixture'
    foreach ($missingReplaySliceMarker in @('m_collisionMetricsAtStart',
        'm_collisionMetricsAwaitingInitialReset', 'm_physicsMetricsAtStart',
        'm_physicsMetricsAwaitingInitialReset', 'm_spatialMetricsAtStart',
        'm_spatialMetricsAwaitingInitialReset', 'jobs.shutdown',
        'simulationJobs.captureSliceMetrics',
        'shadow_compared_candidates',
        'physical_worker_jobs', 'owner_helped_jobs',
        'physical_worker_mask', 'distinct_physical_workers',
        'acceptedBatches', 'circuitBreakerTrips',
		'successful_collections', 'collection_physical_worker_mask',
		'maximum_collection_ranges',
		'maximum_collection_distinct_physical_workers')) {
        try {
            Assert-ReplaySliceManifestLifecycleContract `
                ($validReplaySliceLifecycle.Replace($missingReplaySliceMarker,
                    'removed_replay_slice_marker')) 'malformed replay slice lifecycle fixture'
            throw "Self-test failed to reject missing $missingReplaySliceMarker replay-scope evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    $validSliceFreezeTest = @'
AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
CHECK(collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 0);
CHECK(!collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 3 &&
    collisionFrozen.committedCandidates == 12 &&
    collisionFrozen.physicalWorkerJobs == 3 &&
    collisionFrozen.ownerHelpedJobs == 1 &&
    collisionFrozen.physicalWorkerMask == 5);
AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
CHECK(physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 0);
CHECK(!physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 5 &&
    physicsFrozen.acceptedPrefixes == 96);
CHECK(physicsFrozen.shadowBatches == 2 && physicsFrozen.shadowPrefixes == 48);
AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
CHECK(spatialFrozen.collectionPhysicalWorkerMask == 3 &&
    spatialFrozen.maximumCollectionDistinctPhysicalWorkers == 2);
'@
    Assert-SliceMetricFreezeTestContract $validSliceFreezeTest `
        'valid collision/physics freeze fixture'
    foreach ($missingSliceFreezeMarker in @('collisionAwaitingInitialReset',
        'collisionFrozen.committedCandidates',
        'collisionFrozen.physicalWorkerJobs',
        'collisionFrozen.ownerHelpedJobs',
        'collisionFrozen.physicalWorkerMask', 'physicsAwaitingInitialReset',
        'physicsFrozen.acceptedPrefixes', 'physicsFrozen.shadowBatches',
		'physicsFrozen.shadowPrefixes',
		'spatialFrozen.collectionPhysicalWorkerMask',
		'spatialFrozen.maximumCollectionDistinctPhysicalWorkers')) {
        try {
            Assert-SliceMetricFreezeTestContract `
                ($validSliceFreezeTest.Replace($missingSliceFreezeMarker,
                    'removed_slice_freeze_marker')) 'malformed collision/physics freeze fixture'
            throw "Self-test failed to reject missing $missingSliceFreezeMarker freeze evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    Assert-CollisionResetEpochContract `
        'JobMetricCounter resetEpoch;' `
        'JobMetricCounter nextEpoch = s_runtimeMetrics.resetEpoch + 1; s_runtimeMetrics.resetEpoch = nextEpoch;' `
        'rts::ResetCollisionCandidateRuntimeMetrics(); rts::ResetPhysicsIntegrationRuntimeMetrics();' `
        'valid collision reset-epoch fixture'
    try {
        Assert-CollisionResetEpochContract 'JobMetricCounter resetEpoch;' `
            's_runtimeMetrics.resetEpoch = nextEpoch;' `
            'rts::ResetCollisionCandidateRuntimeMetrics(); rts::ResetPhysicsIntegrationRuntimeMetrics();' `
            'malformed collision reset-epoch fixture'
        throw 'Self-test failed to reject missing collision epoch increment evidence.'
    }
    catch {
        if ($_.Exception.Message -like 'Self-test failed*') { throw }
    }
    $validReplayDestructorSuppression = @'
const Bool reportHeadlessMetrics = TheGlobalData->m_headless &&
    TheGlobalData->m_simulateReplays.empty();
if (reportHeadlessMetrics)
    printHeadlessSimulationJobMetrics(headlessMetrics, headlessPhysicsMetrics,
        headlessStatusMetrics);
'@
    Assert-ReplayDestructorManifestSuppressionContract $validReplayDestructorSuppression `
        'valid replay destructor suppression fixture'
    try {
        Assert-ReplayDestructorManifestSuppressionContract `
            ($validReplayDestructorSuppression.Replace('m_simulateReplays.empty()',
                'true')) 'malformed replay destructor suppression fixture'
        throw 'Self-test failed to reject duplicate replay destructor manifest publication.'
    }
    catch {
        if ($_.Exception.Message -like 'Self-test failed*') { throw }
    }
    try {
        Assert-ReplayDestructorManifestSuppressionContract `
            ($validReplayDestructorSuppression.Replace('headlessStatusMetrics);',
                'removedStatusMetrics);')) 'malformed replay destructor metrics fixture'
        throw 'Self-test failed to reject incomplete replay destructor metrics publication.'
    }
    catch {
        if ($_.Exception.Message -like 'Self-test failed*') { throw }
    }
    $validSpatialKernel = @'
return queryCount >= 2 && workerCount >= 2
executionOptions.workerCount = jobs.workerCount();
jobs.waitWithoutOwnerHelp(group,
IMMUTABLE_SPATIAL_PHYSICAL_WAIT_MILLISECONDS
jobs.cancel(group);
jobs.wait(group);
SPATIAL_PHYSICAL_WORKER
jobContext.physicalWorkerIndex()
physicalWorkerMask
distinctPhysicalWorkers
jobMetrics->ownerHelpedJobs == 0
RecordImmutableSpatialSuccessfulCollection(unsigned queryCount
addMetric(s_successfulCollectionQueries, queryCount);
addMetric(s_successfulCollectionRanges, rangeCount);
'@
    $validLiveSpatial = @'
LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES = 256
PreflightLiveImmutableSpatialQueryScheduler()
PreflightLiveImmutableSpatialQueryCollection(
queueableQueryCount > LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES
return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;
void PublishLiveImmutableSpatialNoCaptureState(
liveSpatialRuntime().publishNoCaptureState(result, manager, frame);
queueableQueryCount, jobs.workerCount())
return liveSpatialRuntime().canQueueConsumer(consumer);
if (m_queryCount < 2)
std::min<UnsignedInt>(
jobs.workerCount(), m_queryCount
std::sort(m_ownerIndex, m_ownerIndex + m_queryCount, ownerIndexLess);
ExecuteImmutableSpatialQueryBatchOnJobSystem(m_arena,
RecordImmutableSpatialSuccessfulCollection(m_queryCount,
prepared.owner != owner
owner->friend_getPriority() != prepared.wakePriority
owner->friend_getNextCallFrame() != frame
view->queryOrdinal = batchIndex;
view->batchEpoch = m_batchEpoch;
'@
    $validSpatialGameLogic = @'
UnsignedInt countImmutableSpatialQueryCollection(
void prepareImmutableSpatialQueryCollection(
canQueueImmutableSpatialQuery();
BeginLiveImmutableSpatialQueryCollection(queueableQueryCount)
static_cast<PointDefenseLaserUpdate *>(update)->
ExecuteLiveImmutableSpatialQueryCollection();
PreflightLiveImmutableSpatialQueryScheduler();
countImmutableSpatialQueryCollection(
PreflightLiveImmutableSpatialQueryCollection(
PublishLiveImmutableSpatialNoCaptureState(preflight,
CaptureLiveImmutableSpatialArena(ThePartitionManager, now)
if (ShouldCaptureLiveImmutableSpatialArena(
queueableQueryCount = countImmutableSpatialQueryCollection(
m_sleepyUpdates, s_immutableSpatialQueries, now,
prepareImmutableSpatialQueryCollection(s_immutableSpatialQueries, now,
'@
    $validAutoHealSpatial = @'
Bool AutoHealBehavior::canQueueImmutableSpatialQuery()
Bool AutoHealBehavior::queueImmutableSpatialQuery()
IsLiveImmutableSpatialConsumerQueueable(
QueueLiveImmutableSpatialQuery( this, ThePartitionManager,
QueryLiveImmutableSpatialCandidates( this, ThePartitionManager,
ThePartitionManager->iterateObjectsInRange(
'@
    $validPointDefenseSpatial = @'
Bool PointDefenseLaserUpdate::canQueueImmutableSpatialQuery()
Bool PointDefenseLaserUpdate::queueImmutableSpatialQuery()
IsLiveImmutableSpatialConsumerQueueable(
QueueLiveImmutableSpatialQuery( this, ThePartitionManager,
QueryLiveImmutableSpatialCandidates( this, ThePartitionManager,
return scanClosestTarget();
'@
    Assert-ImmutableSpatialBatchRuntimeContract $validSpatialKernel `
        $validLiveSpatial $validSpatialGameLogic $validAutoHealSpatial `
        $validPointDefenseSpatial 'valid immutable-spatial batch fixture'
    foreach ($malformedSpatialBatch in @(
        [pscustomobject]@{
            kernel = $validSpatialKernel.Replace('queryCount >= 2',
                'queryCount >= 1')
            live = $validLiveSpatial
            gameLogic = $validSpatialGameLogic
        },
        [pscustomobject]@{
            kernel = $validSpatialKernel
            live = $validLiveSpatial.Replace('prepared.owner != owner',
                'removed_owner_revalidation')
            gameLogic = $validSpatialGameLogic
        },
        [pscustomobject]@{
            kernel = $validSpatialKernel
            live = $validLiveSpatial
            gameLogic = $validSpatialGameLogic.Replace(
				'PreflightLiveImmutableSpatialQueryScheduler();',
				'removed_pre_scan_scheduler_admission();')
		},
		[pscustomobject]@{
			kernel = $validSpatialKernel
			live = $validLiveSpatial
			gameLogic = $validSpatialGameLogic.Replace(
				'PreflightLiveImmutableSpatialQueryCollection(',
                'removed_pre_capture_policy_admission(')
		},
		[pscustomobject]@{
			kernel = $validSpatialKernel.Replace(
				'jobContext.physicalWorkerIndex()',
				'removed_physical_worker_identity')
			live = $validLiveSpatial
			gameLogic = $validSpatialGameLogic
		},
		[pscustomobject]@{
			kernel = $validSpatialKernel.Replace(
				'jobs.waitWithoutOwnerHelp(group,',
				'removed_passive_spatial_fence(')
			live = $validLiveSpatial
			gameLogic = $validSpatialGameLogic
		},
		[pscustomobject]@{
			kernel = $validSpatialKernel
			live = $validLiveSpatial.Replace(
				'liveSpatialRuntime().publishNoCaptureState(result, manager, frame);',
				'removed_current_frame_no_capture_publication')
			gameLogic = $validSpatialGameLogic
        }
    )) {
        try {
            Assert-ImmutableSpatialBatchRuntimeContract `
                $malformedSpatialBatch.kernel $malformedSpatialBatch.live `
                $malformedSpatialBatch.gameLogic $validAutoHealSpatial `
                $validPointDefenseSpatial 'malformed immutable-spatial batch fixture'
            throw 'Self-test failed to reject a malformed immutable-spatial batch contract.'
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
	$validPhysicsPreflightKernel = @'
PreflightPhysicsIntegrationPrefixes()
if (!jobs.isRunning() || jobs.isWorkerThread()
if (jobs.workerCount() <= 1)
return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;
'@
	$validPhysicsPreflightGameLogic = @'
rts::PreflightPhysicsIntegrationPrefixes();
preparePhysicsIntegrationBatch(this, m_sleepyUpdates,
'@
	Assert-EarlyPhysicsIntegrationPreflightContract $validPhysicsPreflightKernel `
		$validPhysicsPreflightGameLogic 'valid early physics preflight fixture'
	try {
		Assert-EarlyPhysicsIntegrationPreflightContract $validPhysicsPreflightKernel `
			($validPhysicsPreflightGameLogic.Replace(
				'rts::PreflightPhysicsIntegrationPrefixes();',
				'removed_early_physics_preflight();')) `
			'malformed early physics preflight fixture'
		throw 'Self-test failed to reject physics heap scanning before scheduler preflight.'
	}
	catch {
		if ($_.Exception.Message -like 'Self-test failed*') { throw }
	}
    $validCollisionAdapter = @'
class LivePartitionCollisionWorkspace
rts::CollisionAdmissionSampler admissionSampler;
admissionSampler.hasUsefulSpread()
rts::RecordCollisionCandidateIneligibleSlice();
rts::frame_timing::CollisionAdmission
rts::frame_timing::SimulationSnapshot
rts::frame_timing::SimulationParallel
rts::RecordCollisionCandidateParallelWork(
rts::frame_timing::CollisionLiveValidation
ctList->containsContact(this,
rts::frame_timing::CollisionExistingFilter
rts::frame_timing::SimulationCommit
rts::frame_timing::CollisionCommitPrepare
rts::frame_timing::SimulationShadowCompare
SIMULATION_COLLISION_MISMATCH frame=%u phase=partition_contact_commit item=%u diff=%s
'@
    $validCollisionKernel = @'
rts::frame_timing::SimulationWait
rts::frame_timing::SimulationReduce
'@
    Assert-CollisionAdapterContract $validCollisionAdapter $validCollisionKernel `
        'valid collision adapter fixture'
    foreach ($missingAdapterMarker in @('LivePartitionCollisionWorkspace',
        'CollisionAdmissionSampler', 'CollisionLiveValidation',
        'CollisionExistingFilter', 'CollisionCommitPrepare', 'containsContact',
        'SimulationShadowCompare', 'SIMULATION_COLLISION_MISMATCH')) {
        try {
            Assert-CollisionAdapterContract `
                ($validCollisionAdapter.Replace($missingAdapterMarker,
                    'removed_collision_adapter_marker')) $validCollisionKernel `
                'malformed collision adapter fixture'
            throw "Self-test failed to reject missing $missingAdapterMarker collision adapter evidence."
        }
        catch {
            if ($_.Exception.Message -like 'Self-test failed*') { throw }
        }
    }
    Write-Output 'Replay mode propagation audit self-test passed.'
    return
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw 'SourceRoot is required unless SelfTest is selected.'
}
$sourceFull = [IO.Path]::GetFullPath($SourceRoot)
$replaySource = Join-Path $sourceFull 'Core\GameEngine\Source\Common\ReplaySimulation.cpp'
if (-not (Test-Path -LiteralPath $replaySource -PathType Leaf)) {
    throw "ReplaySimulation.cpp was not found below SourceRoot: $sourceFull"
}
$replayContent = Get-Content -LiteralPath $replaySource -Raw
Assert-ReplayWorkerPropagation $replayContent 'ReplaySimulation.cpp'
Assert-ReplayResultContract $replayContent 'ReplaySimulation.cpp'
Assert-ReplaySliceManifestLifecycleContract $replayContent 'ReplaySimulation.cpp'
foreach ($gameEngineRelative in @(
    'Generals\Code\GameEngine\Source\Common\GameEngine.cpp',
    'GeneralsMD\Code\GameEngine\Source\Common\GameEngine.cpp'
)) {
    $gameEngineSource = Join-Path $sourceFull $gameEngineRelative
    if (-not (Test-Path -LiteralPath $gameEngineSource -PathType Leaf)) {
        throw "GameEngine.cpp timing-session source was not found: $gameEngineRelative"
    }
    $gameEngineContent = Get-Content -LiteralPath $gameEngineSource -Raw
    Assert-HeadlessTimingSessionContract $gameEngineContent $gameEngineRelative
    Assert-CollisionLifecycleManifestContract $gameEngineContent $gameEngineRelative
    Assert-PhysicsLifecycleManifestContract $gameEngineContent $gameEngineRelative
    Assert-ReplayDestructorManifestSuppressionContract $gameEngineContent $gameEngineRelative
}
$frameTimingHeader = Join-Path $sourceFull 'Core\Libraries\Include\Lib\FrameTimingDiagnostics.h'
$frameTimingTest = Join-Path $sourceFull `
    'Core\Tools\FrameTimingDiagnosticsTest\FrameTimingDiagnosticsTest.cpp'
foreach ($timingContractFile in @($frameTimingHeader, $frameTimingTest)) {
    if (-not (Test-Path -LiteralPath $timingContractFile -PathType Leaf)) {
        throw "Frame timing teardown-reset contract source was not found: $timingContractFile"
    }
}
Assert-MonotonicFrameTimingResetContract `
    (Get-Content -LiteralPath $frameTimingHeader -Raw) `
    (Get-Content -LiteralPath $frameTimingTest -Raw) 'FrameTimingDiagnostics'
$aiRunnerSource = Join-Path $sourceFull `
    'Core\GameEngine\Source\Common\SkirmishAITestRunner.cpp'
if (-not (Test-Path -LiteralPath $aiRunnerSource -PathType Leaf)) {
    throw "Skirmish AI authoritative-work contract source was not found: $aiRunnerSource"
}
Assert-AuthoritativeWorkManifestContract `
    (Get-Content -LiteralPath $aiRunnerSource -Raw) 'SkirmishAITestRunner.cpp'
$coreAiPlanningSource = Join-Path $sourceFull `
    'Core\Libraries\Source\TaskRuntime\DeterministicAIPlanning.cpp'
$generalsAiPlanningRuntime = Join-Path $sourceFull `
    'Generals\Code\GameEngine\Include\GameLogic\GeneralsAIPlanningRuntime.h'
foreach ($aiFloatingPointSource in @($coreAiPlanningSource, $generalsAiPlanningRuntime)) {
    if (-not (Test-Path -LiteralPath $aiFloatingPointSource -PathType Leaf)) {
        throw "Deterministic AI floating-point job source was not found: $aiFloatingPointSource"
    }
}
Assert-AIPlanningFloatingPointJobContract `
    (Get-Content -LiteralPath $coreAiPlanningSource -Raw) `
    (Get-Content -LiteralPath $generalsAiPlanningRuntime -Raw) `
    'Deterministic AI planning jobs'
foreach ($pathFreezeFixture in @(
    [pscustomobject]@{
        relative = 'Generals\Code\Tools\RuntimeRegressionTests\SkirmishAITestRunnerContractTest.cpp'
        workerMarker = 'frozen.workerExecutedJobs == 3'
    },
    [pscustomobject]@{
        relative = 'GeneralsMD\Code\Tools\RuntimeRegressionTests\RuntimeRegressionTests.cpp'
        workerMarker = 'frozen.workerExecutedJobs == 4'
    }
)) {
    $pathFreezeSource = Join-Path $sourceFull $pathFreezeFixture.relative
    if (-not (Test-Path -LiteralPath $pathFreezeSource -PathType Leaf)) {
        throw "Direct-path metric freeze fixture was not found: $($pathFreezeFixture.relative)"
    }
    Assert-PathMetricFreezeTestContract `
        (Get-Content -LiteralPath $pathFreezeSource -Raw) `
        $pathFreezeFixture.workerMarker $pathFreezeFixture.relative
    Assert-SliceMetricFreezeTestContract `
        (Get-Content -LiteralPath $pathFreezeSource -Raw) $pathFreezeFixture.relative
}
$collisionKernelSource = Join-Path $sourceFull `
    'Core\Libraries\Source\TaskRuntime\CollisionCandidateKernel.cpp'
if (-not (Test-Path -LiteralPath $collisionKernelSource -PathType Leaf)) {
    throw "Collision candidate kernel source was not found: $collisionKernelSource"
}
$collisionKernelContent = Get-Content -LiteralPath $collisionKernelSource -Raw
$collisionKernelHeader = Join-Path $sourceFull `
    'Core\Libraries\Include\Lib\CollisionCandidateKernel.h'
if (-not (Test-Path -LiteralPath $collisionKernelHeader -PathType Leaf)) {
    throw "Collision candidate kernel header was not found: $collisionKernelHeader"
}
foreach ($gameLogicRelative in @(
    'Generals\Code\GameEngine\Source\GameLogic\System\GameLogic.cpp',
    'GeneralsMD\Code\GameEngine\Source\GameLogic\System\GameLogic.cpp'
)) {
    $gameLogicSource = Join-Path $sourceFull $gameLogicRelative
    if (-not (Test-Path -LiteralPath $gameLogicSource -PathType Leaf)) {
        throw "Collision reset lifecycle source was not found: $gameLogicRelative"
    }
    Assert-CollisionResetEpochContract `
        (Get-Content -LiteralPath $collisionKernelHeader -Raw) $collisionKernelContent `
        (Get-Content -LiteralPath $gameLogicSource -Raw) $gameLogicRelative
}
foreach ($partitionRelative in @(
    'Generals\Code\GameEngine\Source\GameLogic\Object\PartitionManager.cpp',
    'GeneralsMD\Code\GameEngine\Source\GameLogic\Object\PartitionManager.cpp'
)) {
    $partitionSource = Join-Path $sourceFull $partitionRelative
    if (-not (Test-Path -LiteralPath $partitionSource -PathType Leaf)) {
        throw "Collision partition adapter source was not found: $partitionRelative"
    }
    Assert-CollisionAdapterContract (Get-Content -LiteralPath $partitionSource -Raw) `
        $collisionKernelContent $partitionRelative
}
$spatialKernelSource = Join-Path $sourceFull `
    'Core\Libraries\Source\TaskRuntime\ImmutableSpatialQueryRuntime.cpp'
$physicsKernelSource = Join-Path $sourceFull `
	'Core\Libraries\Source\TaskRuntime\PhysicsIntegrationKernel.cpp'
$liveSpatialSource = Join-Path $sourceFull `
    'Core\GameEngine\Source\GameLogic\System\ImmutableSpatialQueryRuntime.cpp'
foreach ($spatialRuntimeSource in @($spatialKernelSource, $liveSpatialSource,
	$physicsKernelSource)) {
    if (-not (Test-Path -LiteralPath $spatialRuntimeSource -PathType Leaf)) {
        throw "Immutable-spatial batch runtime source was not found: $spatialRuntimeSource"
    }
}
$spatialKernelContent = Get-Content -LiteralPath $spatialKernelSource -Raw
$liveSpatialContent = Get-Content -LiteralPath $liveSpatialSource -Raw
$physicsKernelContent = Get-Content -LiteralPath $physicsKernelSource -Raw
foreach ($titleRoot in @('Generals', 'GeneralsMD')) {
    $gameLogicRelative = "$titleRoot\Code\GameEngine\Source\GameLogic\System\GameLogic.cpp"
    $autoHealRelative = "$titleRoot\Code\GameEngine\Source\GameLogic\Object\Behavior\AutoHealBehavior.cpp"
    $pointDefenseRelative = "$titleRoot\Code\GameEngine\Source\GameLogic\Object\Update\PointDefenseLaserUpdate.cpp"
    foreach ($relative in @($gameLogicRelative, $autoHealRelative,
        $pointDefenseRelative)) {
        $spatialTitleSource = Join-Path $sourceFull $relative
        if (-not (Test-Path -LiteralPath $spatialTitleSource -PathType Leaf)) {
            throw "Immutable-spatial title integration source was not found: $relative"
        }
    }
	$gameLogicContent = Get-Content -LiteralPath `
		(Join-Path $sourceFull $gameLogicRelative) -Raw
	Assert-EarlyPhysicsIntegrationPreflightContract $physicsKernelContent `
		$gameLogicContent "$titleRoot early physics preflight"
    Assert-ImmutableSpatialBatchRuntimeContract $spatialKernelContent `
        $liveSpatialContent `
		$gameLogicContent `
        (Get-Content -LiteralPath (Join-Path $sourceFull $autoHealRelative) -Raw) `
        (Get-Content -LiteralPath (Join-Path $sourceFull $pointDefenseRelative) -Raw) `
        "$titleRoot immutable-spatial integration"
}
Write-Output 'Replay propagation, final-result, timing, and independent AI/collision/direct-path/ordinary-path/physics/immutable-spatial lifecycle, owner-batch, adapter-oracle, phase, and authoritative-work contracts are present.'
