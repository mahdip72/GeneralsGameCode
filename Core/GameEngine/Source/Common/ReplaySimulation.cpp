/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ReplaySimulation.h"
#include "Lib/FrameTimingDiagnostics.h"

#include "Common/GameEngine.h"
#include "Common/LocalFileSystem.h"
#include "Common/Recorder.h"
#include "Common/SkirmishAITestRunner.h"
#include "Common/WorkerProcess.h"
#include "GameLogic/GameLogic.h"
#include "GameClient/GameClient.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"
#if defined(_WIN64)
#include "Lib/PerformanceReceipt.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include <stdio.h>
#endif


Bool ReplaySimulation::s_isRunning = false;
UnsignedInt ReplaySimulation::s_replayIndex = 0;
UnsignedInt ReplaySimulation::s_replayCount = 0;

namespace
{
const char *simulationModeName(rts::SimulationExecutionMode mode)
{
	switch (mode)
	{
		case rts::SIMULATION_EXECUTION_PARALLEL: return "parallel";
		case rts::SIMULATION_EXECUTION_SHADOW: return "shadow";
		default: return "serial";
	}
}

const WideChar *simulationModeNameWide(rts::SimulationExecutionMode mode)
{
	switch (mode)
	{
		case rts::SIMULATION_EXECUTION_PARALLEL: return L"parallel";
		case rts::SIMULATION_EXECUTION_SHADOW: return L"shadow";
		default: return L"serial";
	}
}

const char *pipelineModeName(rts::PipelineExecutionMode mode)
{
	return mode == rts::PIPELINE_EXECUTION_SERIAL ? "serial" : "parallel";
}

#if defined(_WIN64)
void printHeadlessReplaySliceMetrics(
	const rts::CollisionCandidateRuntimeMetrics &collision,
	const rts::PhysicsIntegrationRuntimeMetrics &physics,
	const rts::ObjectStatusTimerRuntimeMetrics &status,
	const rts::ImmutableSpatialRuntimeMetrics &spatial)
{
	printf("COLLISION_CANDIDATE_MANIFEST authoritative_commits=%llu shadow_executions=%llu shadow_compared_candidates=%llu shadow_mismatches=%llu owner_fallbacks=%llu unexpected_fallbacks=%llu ineligible_slices=%llu stale_rejections=%llu committed_candidates=%llu prepared_pairs=%llu unique_candidates=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u physical_worker_mask_complete=%u\n",
		static_cast<unsigned long long>(collision.authoritativeCommits),
		static_cast<unsigned long long>(collision.shadowExecutions),
		static_cast<unsigned long long>(collision.shadowComparedCandidates),
		static_cast<unsigned long long>(collision.shadowMismatches),
		static_cast<unsigned long long>(collision.ownerFallbacks),
		static_cast<unsigned long long>(collision.unexpectedFallbacks),
		static_cast<unsigned long long>(collision.ineligibleSlices),
		static_cast<unsigned long long>(collision.staleRejections),
		static_cast<unsigned long long>(collision.committedCandidates),
		static_cast<unsigned long long>(collision.preparedPairs),
		static_cast<unsigned long long>(collision.uniqueCandidates),
		static_cast<unsigned long long>(collision.submittedJobs),
		static_cast<unsigned long long>(collision.completedJobs),
		static_cast<unsigned long long>(collision.physicalWorkerJobs),
		static_cast<unsigned long long>(collision.ownerHelpedJobs),
		static_cast<unsigned long long>(collision.physicalWorkerMask),
		collision.distinctPhysicalWorkers,
		collision.physicalWorkerMaskComplete ? 1U : 0U);
	printf("PHYSICS_INTEGRATION_MANIFEST authoritative_batches=%llu committed_prefixes=%llu ranges=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u physical_worker_mask_complete=%u peak_concurrent_physical_workers=%u allocated_bytes=%llu capture_ns=%llu prepare_ns=%llu wait_ns=%llu commit_ns=%llu storage_bytes=%llu storage_capacity_bytes=%llu storage_allocations=%llu shadow_executions=%llu shadow_prefixes=%llu shadow_ranges=%llu shadow_submitted_jobs=%llu shadow_completed_jobs=%llu shadow_matches=%llu shadow_mismatches=%llu owner_fallbacks=%llu ineligible_slices=%llu unexpected_fallbacks=%llu stale_rejections=%llu circuit_breaker_trips=%llu\n",
		static_cast<unsigned long long>(physics.acceptedBatches),
		static_cast<unsigned long long>(physics.acceptedPrefixes),
		static_cast<unsigned long long>(physics.acceptedRanges),
		static_cast<unsigned long long>(physics.acceptedSubmittedJobs),
		static_cast<unsigned long long>(physics.acceptedCompletedJobs),
		static_cast<unsigned long long>(physics.acceptedPhysicalWorkerJobs),
		static_cast<unsigned long long>(physics.acceptedOwnerHelpedJobs),
		static_cast<unsigned long long>(physics.acceptedPhysicalWorkerMask),
		physics.maximumAcceptedDistinctPhysicalWorkers,
		physics.acceptedPhysicalWorkerMaskComplete ? 1U : 0U,
		physics.maximumAcceptedPeakConcurrentPhysicalWorkers,
		static_cast<unsigned long long>(physics.acceptedAllocatedBytes),
		static_cast<unsigned long long>(physics.acceptedCaptureNanoseconds),
		static_cast<unsigned long long>(physics.acceptedPrepareNanoseconds),
		static_cast<unsigned long long>(physics.acceptedWaitNanoseconds),
		static_cast<unsigned long long>(physics.acceptedCommitNanoseconds),
		static_cast<unsigned long long>(physics.acceptedStorageBytes),
		static_cast<unsigned long long>(physics.acceptedStorageCapacityBytes),
		static_cast<unsigned long long>(physics.acceptedStorageAllocations),
		static_cast<unsigned long long>(physics.shadowBatches),
		static_cast<unsigned long long>(physics.shadowPrefixes),
		static_cast<unsigned long long>(physics.shadowRanges),
		static_cast<unsigned long long>(physics.shadowSubmittedJobs),
		static_cast<unsigned long long>(physics.shadowCompletedJobs),
		static_cast<unsigned long long>(physics.shadowMatches),
		static_cast<unsigned long long>(physics.shadowMismatches),
		static_cast<unsigned long long>(physics.ownerFallbacks),
		static_cast<unsigned long long>(physics.ineligibleSlices),
		static_cast<unsigned long long>(physics.unexpectedFallbacks),
		static_cast<unsigned long long>(physics.staleRejections),
		static_cast<unsigned long long>(physics.circuitBreakerTrips));
	printf("OBJECT_STATUS_TIMER_MANIFEST authoritative_batches=%llu committed_commands=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u physical_worker_mask_complete=%u peak_concurrent_physical_workers=%u shadow_executions=%llu shadow_commands=%llu shadow_matches=%llu shadow_mismatches=%llu owner_fallbacks=%llu stale_rejections=%llu\n",
		static_cast<unsigned long long>(status.authoritativeBatches),
		static_cast<unsigned long long>(status.committedCommands),
		static_cast<unsigned long long>(status.submittedJobs),
		static_cast<unsigned long long>(status.completedJobs),
		static_cast<unsigned long long>(status.physicalWorkerJobs),
		static_cast<unsigned long long>(status.ownerHelpedJobs),
		static_cast<unsigned long long>(status.physicalWorkerMask),
		status.maximumDistinctPhysicalWorkers,
		status.physicalWorkerMaskComplete ? 1U : 0U,
		status.maximumPeakConcurrentPhysicalWorkers,
		static_cast<unsigned long long>(status.shadowExecutions),
		static_cast<unsigned long long>(status.shadowCommands),
		static_cast<unsigned long long>(status.shadowMatches),
		static_cast<unsigned long long>(status.shadowMismatches),
		static_cast<unsigned long long>(status.ownerFallbacks),
		static_cast<unsigned long long>(status.staleRejections));
	printf("IMMUTABLE_SPATIAL_MANIFEST");
#define PRINT_SPATIAL_METRIC(name, value) \
	printf(" " name "=%llu", static_cast<unsigned long long>(value))
	PRINT_SPATIAL_METRIC("captured_arenas", spatial.capturedArenas);
	PRINT_SPATIAL_METRIC("capture_failures", spatial.captureFailures);
	PRINT_SPATIAL_METRIC("successful_collections", spatial.successfulCollections);
	PRINT_SPATIAL_METRIC("successful_collection_queries",
		spatial.successfulCollectionQueries);
	PRINT_SPATIAL_METRIC("successful_collection_ranges",
		spatial.successfulCollectionRanges);
	PRINT_SPATIAL_METRIC("multi_range_collections",
		spatial.multiRangeCollections);
	PRINT_SPATIAL_METRIC("collection_submitted_jobs",
		spatial.collectionSubmittedJobs);
	PRINT_SPATIAL_METRIC("collection_completed_jobs",
		spatial.collectionCompletedJobs);
	PRINT_SPATIAL_METRIC("collection_physical_worker_jobs",
		spatial.collectionPhysicalWorkerJobs);
	PRINT_SPATIAL_METRIC("collection_owner_helped_jobs",
		spatial.collectionOwnerHelpedJobs);
	PRINT_SPATIAL_METRIC("collection_physical_worker_mask",
		spatial.collectionPhysicalWorkerMask);
	PRINT_SPATIAL_METRIC("maximum_collection_queries",
		spatial.maximumCollectionQueries);
	PRINT_SPATIAL_METRIC("maximum_collection_ranges",
		spatial.maximumCollectionRanges);
	PRINT_SPATIAL_METRIC("maximum_collection_distinct_physical_workers",
		spatial.maximumCollectionDistinctPhysicalWorkers);
#define PRINT_SPATIAL_CONSUMER(prefix, consumer) \
	PRINT_SPATIAL_METRIC(prefix "_eligible_queries", consumer.eligibleQueries); \
	PRINT_SPATIAL_METRIC(prefix "_authoritative_queries", consumer.authoritativeQueries); \
	PRINT_SPATIAL_METRIC(prefix "_authoritative_candidates", consumer.authoritativeCandidates); \
	PRINT_SPATIAL_METRIC(prefix "_shadow_queries", consumer.shadowQueries); \
	PRINT_SPATIAL_METRIC(prefix "_shadow_matches", consumer.shadowMatches); \
	PRINT_SPATIAL_METRIC(prefix "_shadow_mismatches", consumer.shadowMismatches); \
	PRINT_SPATIAL_METRIC(prefix "_submitted_jobs", consumer.submittedJobs); \
	PRINT_SPATIAL_METRIC(prefix "_completed_jobs", consumer.completedJobs); \
	PRINT_SPATIAL_METRIC(prefix "_physical_worker_jobs", consumer.physicalWorkerJobs); \
	PRINT_SPATIAL_METRIC(prefix "_owner_helped_jobs", consumer.ownerHelpedJobs); \
	PRINT_SPATIAL_METRIC(prefix "_expected_fallbacks", consumer.expectedFallbacks); \
	PRINT_SPATIAL_METRIC(prefix "_unexpected_fallbacks", consumer.unexpectedFallbacks); \
	PRINT_SPATIAL_METRIC(prefix "_stale_rejections", consumer.staleRejections); \
	PRINT_SPATIAL_METRIC(prefix "_validation_failures", consumer.validationFailures); \
	PRINT_SPATIAL_METRIC(prefix "_circuit_breaker_trips", consumer.circuitBreakerTrips)
	PRINT_SPATIAL_CONSUMER("healing", spatial.healing);
	PRINT_SPATIAL_CONSUMER("pdl", spatial.pointDefenseLaser);
#undef PRINT_SPATIAL_CONSUMER
#undef PRINT_SPATIAL_METRIC
	printf("\n");
	fflush(stdout);
}

void appendPerformanceReceiptPhase(
	rts::performance::PerformanceReceipt &receipt, const char *name,
	bool available, rts::JobMetricCounter totalNanoseconds,
	rts::JobMetricCounter maximumNanoseconds,
	rts::JobMetricCounter sampleCount)
{
	rts::performance::PerformanceReceiptPhase phase;
	phase.name = name;
	phase.available = available;
	phase.totalNanoseconds = totalNanoseconds;
	phase.maximumNanoseconds = maximumNanoseconds;
	phase.sampleCount = sampleCount;
	receipt.phases.push_back(phase);
}

void appendPerformanceReceiptKernel(
	rts::performance::PerformanceReceipt &receipt, const char *name,
	bool available, rts::JobMetricCounter submittedJobs,
	rts::JobMetricCounter completedJobs,
	rts::JobMetricCounter physicalWorkerJobs,
	rts::JobMetricCounter ownerHelpedJobs,
	rts::JobMetricCounter physicalWorkerMask,
	unsigned distinctPhysicalWorkers, bool physicalWorkerMaskComplete)
{
	rts::performance::PerformanceReceiptKernel kernel;
	kernel.name = name;
	kernel.available = available;
	kernel.submittedJobs = submittedJobs;
	kernel.completedJobs = completedJobs;
	kernel.physicalWorkerJobs = physicalWorkerJobs;
	kernel.ownerHelpedJobs = ownerHelpedJobs;
	kernel.physicalWorkerMask = physicalWorkerMask;
	kernel.distinctPhysicalWorkers = distinctPhysicalWorkers;
	kernel.physicalWorkerMaskComplete = physicalWorkerMaskComplete;
	// Kernel-level timings are not exposed by the current executable APIs.  A
	// receipt must say so rather than derive a non-equivalent sum of substeps.
	kernel.elapsedNanoseconds = 0;
	kernel.elapsedNanosecondsKnown = false;
	receipt.kernels.push_back(kernel);
}

void capturePerformanceReceiptMetrics(
	rts::performance::PerformanceReceipt &receipt,
	const rts::CollisionCandidateRuntimeMetrics &collision,
	const rts::PhysicsIntegrationRuntimeMetrics &physics,
	const rts::ObjectStatusTimerRuntimeMetrics &status,
	const rts::ImmutableSpatialRuntimeMetrics &spatial)
{
	receipt.phases.clear();
	const rts::LiveSimulationPhaseRuntimeMetrics phaseMetrics =
		TheGameLogic != 0 ? TheGameLogic->getStage5PhaseRuntimeMetrics() :
		rts::LiveSimulationPhaseRuntimeMetrics();
	// Only the three phase clocks currently exposed by the executable are
	// marked available.  The remaining contract rows remain explicit but
	// unavailable until their owning phase publishes an exact clock.
	appendPerformanceReceiptPhase(receipt, "owner-intake",
		phaseMetrics.ownerPhaseSampleCount[0] != 0,
		phaseMetrics.ownerPhaseTotalNanoseconds[0],
		phaseMetrics.ownerPhaseMaximumNanoseconds[0],
		phaseMetrics.ownerPhaseSampleCount[0]);
	appendPerformanceReceiptPhase(receipt, "world-queries", false, 0, 0, 0);
	appendPerformanceReceiptPhase(receipt, "pathfinding", false, 0, 0, 0);
	appendPerformanceReceiptPhase(receipt, "object-computation", false, 0, 0,
		0);
	appendPerformanceReceiptPhase(receipt, "spatial-work",
		phaseMetrics.ownerPhaseSampleCount[2] != 0,
		phaseMetrics.ownerPhaseTotalNanoseconds[2],
		phaseMetrics.ownerPhaseMaximumNanoseconds[2],
		phaseMetrics.ownerPhaseSampleCount[2]);
	appendPerformanceReceiptPhase(receipt, "deterministic-commit", false, 0,
		0, 0);
	appendPerformanceReceiptPhase(receipt, "verification-publication",
		phaseMetrics.ownerPhaseSampleCount[4] != 0,
		phaseMetrics.ownerPhaseTotalNanoseconds[4],
		phaseMetrics.ownerPhaseMaximumNanoseconds[4],
		phaseMetrics.ownerPhaseSampleCount[4]);

	receipt.kernels.clear();
	appendPerformanceReceiptKernel(receipt, "physics", true,
		physics.acceptedSubmittedJobs, physics.acceptedCompletedJobs,
		physics.acceptedPhysicalWorkerJobs, physics.acceptedOwnerHelpedJobs,
		physics.acceptedPhysicalWorkerMask,
		physics.maximumAcceptedDistinctPhysicalWorkers,
		physics.acceptedPhysicalWorkerMaskComplete);
	appendPerformanceReceiptKernel(receipt, "status", true,
		status.submittedJobs, status.completedJobs, status.physicalWorkerJobs,
		status.ownerHelpedJobs, status.physicalWorkerMask,
		status.maximumDistinctPhysicalWorkers,
		status.physicalWorkerMaskComplete);
	appendPerformanceReceiptKernel(receipt, "collision", true,
		collision.submittedJobs, collision.completedJobs,
		collision.physicalWorkerJobs, collision.ownerHelpedJobs,
		collision.physicalWorkerMask, collision.distinctPhysicalWorkers,
		collision.physicalWorkerMaskComplete);
	const rts::AIPlanningRuntimeMetrics ai =
		rts::GetAIPlanningRuntimeMetrics();
	appendPerformanceReceiptKernel(receipt, "ai-planning", true,
		static_cast<rts::JobMetricCounter>(ai.submittedJobs),
		static_cast<rts::JobMetricCounter>(ai.completedJobs),
		static_cast<rts::JobMetricCounter>(ai.physicalWorkerExecutions),
		static_cast<rts::JobMetricCounter>(ai.ownerHelpedExecutions),
		static_cast<rts::JobMetricCounter>(ai.observedPhysicalWorkerMask),
		static_cast<unsigned>(ai.maximumDistinctPhysicalWorkers),
		ai.maximumDistinctPhysicalWorkers <= 64U);
	appendPerformanceReceiptKernel(receipt, "spatial", true,
		spatial.collectionSubmittedJobs, spatial.collectionCompletedJobs,
		spatial.collectionPhysicalWorkerJobs,
		spatial.collectionOwnerHelpedJobs,
		spatial.collectionPhysicalWorkerMask,
		static_cast<unsigned>(spatial.maximumCollectionDistinctPhysicalWorkers),
		spatial.maximumCollectionDistinctPhysicalWorkers <= 64U);
	appendPerformanceReceiptKernel(receipt, "pathfinding", false, 0, 0, 0,
		0, 0, 0, false);
}

bool resolvePerformanceReceiptTimingPath(
	rts::performance::PerformanceReceipt &receipt)
{
	std::string configuredPath = receipt.rawEvidence.timingPath;
	if (configuredPath.empty()) return false;
	const DWORD attributes = GetFileAttributesA(configuredPath.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return true;
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return false;
	std::string pattern = configuredPath;
	if (!pattern.empty() && pattern[pattern.size() - 1] != '\\' &&
		pattern[pattern.size() - 1] != '/')
		pattern += '\\';
	pattern += "frame-timing-*.csv";
	WIN32_FIND_DATAA data;
	HANDLE search = FindFirstFileA(pattern.c_str(), &data);
	if (search == INVALID_HANDLE_VALUE) return false;
	std::string resolvedPath;
	bool multiple = false;
	do
	{
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			if (!resolvedPath.empty()) multiple = true;
			resolvedPath = configuredPath;
			if (!resolvedPath.empty() &&
				resolvedPath[resolvedPath.size() - 1] != '\\' &&
				resolvedPath[resolvedPath.size() - 1] != '/')
				resolvedPath += '\\';
			resolvedPath += data.cFileName;
		}
	}
	while (FindNextFileA(search, &data) != 0);
	FindClose(search);
	if (multiple || resolvedPath.empty()) return false;
	receipt.rawEvidence.timingPath = resolvedPath;
	return true;
}

bool writePerformanceReceiptRawDiagnostic(
	const rts::performance::PerformanceReceipt &receipt)
{
	if (receipt.rawEvidence.rawLogPath.empty()) return false;
	FILE *file = fopen(receipt.rawEvidence.rawLogPath.c_str(), "wx");
	if (file == 0) return false;
	fprintf(file, "producer=game-executable-performance-receipt-v1\n");
	fprintf(file, "game_owned=1\n");
	fprintf(file, "run_id=%s\n", receipt.runId.c_str());
	fprintf(file, "process_id=%u\n", receipt.processId);
	fprintf(file, "executable_sha256=%s\n",
		receipt.executableSha256.c_str());
	fprintf(file, "fixture_id=%s\n", receipt.fixtureId.c_str());
	fprintf(file, "fixture_sha256=%s\n",
		receipt.fixtureContentSha256.c_str());
	fprintf(file, "frame=%u\n", receipt.frameEnd);
	fprintf(file, "final_crc=%08X\n", receipt.finalCrc);
	fprintf(file, "close_boundary=game-owned-raw-diagnostic-closed-v1\n");
	bool success = ferror(file) == 0 && fflush(file) == 0;
	if (fclose(file) != 0) success = false;
	return success;
}
#endif

void printHeadlessJobMetrics(const char *replayName,
	rts::SimulationExecutionMode requestedMode,
	rts::SimulationExecutionMode effectiveMode,
	rts::PipelineExecutionMode requestedPipelineMode, bool schedulerStarted,
	unsigned workerCount, const rts::JobSystemMetrics &metrics)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	printf("SIMULATION_JOB_METRICS replay=\"%s\" requested_mode=%s effective_mode=%s requested_pipeline=%s effective_pipeline=serial scheduler_started=0 workers=0\n",
		replayName, simulationModeName(requestedMode),
		simulationModeName(effectiveMode), pipelineModeName(requestedPipelineMode));
#else
	printf("SIMULATION_JOB_METRICS replay=\"%s\" requested_mode=%s effective_mode=%s requested_pipeline=%s effective_pipeline=serial scheduler_started=%u workers=%u submitted=%llu executed=%llu steals=%llu owner_help=%llu waits=%llu worker_wait_rejections=%llu failures=%llu cancelled=%llu fallback=%llu queue_latency_ns=%llu max_queue_latency_ns=%llu sleeps=%llu wakes=%llu affinity_failures=%llu queue_high_water=%u peak_active_workers=%u available_cpus=%u reserved_owner_cpus=%u selected_worker_cpus=%u\n",
		replayName, simulationModeName(requestedMode),
		simulationModeName(effectiveMode), pipelineModeName(requestedPipelineMode),
		schedulerStarted ? 1u : 0u,
		workerCount,
		static_cast<unsigned long long>(metrics.submittedJobCount),
		static_cast<unsigned long long>(metrics.executedJobCount),
		static_cast<unsigned long long>(metrics.stealCount),
		static_cast<unsigned long long>(metrics.ownerHelpCount),
		static_cast<unsigned long long>(metrics.waitCount),
		static_cast<unsigned long long>(metrics.workerWaitRejectionCount),
		static_cast<unsigned long long>(metrics.failedJobCount),
		static_cast<unsigned long long>(metrics.cancelledJobCount),
		static_cast<unsigned long long>(metrics.serialFallbackCount),
		static_cast<unsigned long long>(metrics.totalQueueLatencyNanoseconds),
		static_cast<unsigned long long>(metrics.maximumQueueLatencyNanoseconds),
		static_cast<unsigned long long>(metrics.workerSleepCount),
		static_cast<unsigned long long>(metrics.workerWakeCount),
		static_cast<unsigned long long>(metrics.affinityFailureCount),
		metrics.injectionHighWater, metrics.maximumActiveWorkers,
		metrics.availableLogicalCpuCount, metrics.reservedOwnerCpuCount,
		metrics.selectedWorkerCpuCount);
#endif
	fflush(stdout);
}

class HeadlessSimulationJobSystemScope
{
public:
	HeadlessSimulationJobSystemScope(const char *replayName,
		rts::PipelineExecutionMode requestedPipelineMode,
		void *performanceReceipt = 0)
		: m_replayName(replayName),
		  m_requestedMode(rts::GetSimulationExecutionMode()),
		  m_effectiveMode(rts::SIMULATION_EXECUTION_SERIAL),
		  m_requestedPipelineMode(requestedPipelineMode),
		  m_startAttempted(false), m_started(false), m_workerCount(0)
		#if defined(_WIN64)
		  , m_performanceReceipt(
			static_cast<rts::performance::PerformanceReceipt *>(performanceReceipt))
		#endif
	{
#if !defined(_WIN64)
		(void)performanceReceipt;
#endif
#if defined(_WIN64)
		m_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();
		m_collisionMetricsFrozen = rts::CollisionCandidateRuntimeMetrics();
		m_collisionMetricsAwaitingInitialReset = TRUE;
		m_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();
		m_physicsMetricsFrozen = rts::PhysicsIntegrationRuntimeMetrics();
		m_physicsMetricsAwaitingInitialReset = TRUE;
		m_statusMetricsAtStart = rts::GetObjectStatusTimerRuntimeMetrics();
		m_statusMetricsFrozen = rts::ObjectStatusTimerRuntimeMetrics();
		m_statusMetricsAwaitingInitialReset = TRUE;
		m_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();
		m_spatialMetricsFrozen = rts::ImmutableSpatialRuntimeMetrics();
		m_spatialMetricsAwaitingInitialReset = TRUE;
#endif
	}

	~HeadlessSimulationJobSystemScope()
	{
		if (!m_startAttempted) return;
		rts::JobSystem &jobs = rts::JobSystem::instance();
		#if defined(_WIN64)
		if (m_performanceReceipt != 0 && m_started)
		{
			std::string reason;
			rts::performance::CapturePerformanceReceiptJobSystem(
				*m_performanceReceipt, jobs, jobs.metrics(), &reason);
		}
		#endif
		if (m_started) jobs.shutdown();
		const rts::JobSystemMetrics metrics = jobs.metrics();
#if defined(_WIN64)
		captureSliceMetrics();
		if (m_performanceReceipt != 0)
		{
			std::string reason;
			rts::performance::CapturePerformanceReceiptJobSystem(
				*m_performanceReceipt, jobs, metrics, &reason);
			capturePerformanceReceiptMetrics(*m_performanceReceipt,
				m_collisionMetricsFrozen, m_physicsMetricsFrozen,
				m_statusMetricsFrozen, m_spatialMetricsFrozen);
		}
#endif
		if (m_started && jobs.isCurrentThread(rts::JOB_OWNER_GAME) &&
			!jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME))
		{
			printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=owner_unregister_failed\n",
				simulationModeName(m_requestedMode));
		}
		printHeadlessJobMetrics(m_replayName, m_requestedMode, m_effectiveMode,
			m_requestedPipelineMode, m_started, m_workerCount, metrics);
#if defined(_WIN64)
		printHeadlessReplaySliceMetrics(m_collisionMetricsFrozen,
			m_physicsMetricsFrozen, m_statusMetricsFrozen,
			m_spatialMetricsFrozen);
#endif
	}

	void startAfterUnsafeInitialization()
	{
		if (m_startAttempted) return;
		m_startAttempted = true;
		rts::JobSystem &jobs = rts::JobSystem::instance();
		jobs.resetMetrics();

		if (rts::GetPipelineExecutionMode() != rts::PIPELINE_EXECUTION_SERIAL &&
			!rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL))
		{
			rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
			rts::LockSimulationExecutionMode();
			printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=pipeline_mode_locked\n",
				simulationModeName(m_requestedMode));
			fflush(stdout);
			return;
		}
		rts::LockPipelineExecutionMode();

#if defined(_MSC_VER) && _MSC_VER < 1300
		if (m_requestedMode != rts::SIMULATION_EXECUTION_SERIAL)
			rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
		rts::LockSimulationExecutionMode();
		return;
#else
		if (rts::GetSimulationExecutionMode() == rts::SIMULATION_EXECUTION_SERIAL)
		{
			rts::LockSimulationExecutionMode();
			return;
		}

		if (!jobs.start(rts::JobSystem::startupConfig()))
		{
			rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
			rts::LockSimulationExecutionMode();
			printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=start_failed\n",
				simulationModeName(m_requestedMode));
			fflush(stdout);
			return;
		}
		if (!jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
		{
			jobs.shutdown();
			rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
			rts::LockSimulationExecutionMode();
			printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=owner_registration_failed\n",
				simulationModeName(m_requestedMode));
			fflush(stdout);
			return;
		}
		rts::LockSimulationExecutionMode();
		m_started = true;
		m_workerCount = jobs.workerCount();
		m_effectiveMode = rts::GetSimulationExecutionMode();
		printf("SIMULATION_JOB_SYSTEM_START requested_mode=%s effective_mode=%s requested_pipeline=%s effective_pipeline=serial workers=%u\n",
			simulationModeName(m_requestedMode), simulationModeName(m_effectiveMode),
			pipelineModeName(m_requestedPipelineMode), m_workerCount);
		fflush(stdout);
#endif
	}

	void captureSliceMetrics()
	{
#if defined(_WIN64)
		const rts::CollisionCandidateRuntimeMetrics collisionMetrics =
			rts::GetCollisionCandidateRuntimeMetrics();
		const rts::PhysicsIntegrationRuntimeMetrics physicsMetrics =
			rts::GetPhysicsIntegrationRuntimeMetrics();
		const rts::ObjectStatusTimerRuntimeMetrics statusMetrics =
			rts::GetObjectStatusTimerRuntimeMetrics();
		AccumulateSkirmishAITestCollisionMetrics(&m_collisionMetricsAtStart,
			collisionMetrics, &m_collisionMetricsFrozen,
			&m_collisionMetricsAwaitingInitialReset);
		AccumulateSkirmishAITestPhysicsMetrics(&m_physicsMetricsAtStart,
			physicsMetrics, &m_physicsMetricsFrozen,
			&m_physicsMetricsAwaitingInitialReset);
		AccumulateSkirmishAITestObjectStatusTimerMetrics(&m_statusMetricsAtStart,
			statusMetrics, &m_statusMetricsFrozen,
			&m_statusMetricsAwaitingInitialReset);
		const rts::ImmutableSpatialRuntimeMetrics spatialMetrics =
			rts::GetImmutableSpatialRuntimeMetrics();
		AccumulateSkirmishAITestImmutableSpatialMetrics(&m_spatialMetricsAtStart,
			spatialMetrics, &m_spatialMetricsFrozen,
			&m_spatialMetricsAwaitingInitialReset);
#endif
	}

	void setReplayResult(unsigned finalFrame, unsigned finalCrc,
		bool finalCrcKnown, bool clean)
	{
#if defined(_WIN64)
		if (m_performanceReceipt != 0)
		{
			m_performanceReceipt->frameStart = 0;
			m_performanceReceipt->frameEnd = finalFrame;
			m_performanceReceipt->finalFrame = finalFrame;
			m_performanceReceipt->finalCrc = finalCrc;
			m_performanceReceipt->finalCrcKnown = finalCrcKnown;
			m_performanceReceipt->status = clean ? "complete" : "failed";
		}
#else
		(void)finalFrame;
		(void)finalCrc;
		(void)finalCrcKnown;
		(void)clean;
#endif
	}

private:
	const char *m_replayName;
	rts::SimulationExecutionMode m_requestedMode;
	rts::SimulationExecutionMode m_effectiveMode;
	rts::PipelineExecutionMode m_requestedPipelineMode;
	bool m_startAttempted;
	bool m_started;
	unsigned m_workerCount;
#if defined(_WIN64)
	rts::performance::PerformanceReceipt *m_performanceReceipt;
	rts::CollisionCandidateRuntimeMetrics m_collisionMetricsAtStart;
	rts::CollisionCandidateRuntimeMetrics m_collisionMetricsFrozen;
	Bool m_collisionMetricsAwaitingInitialReset;
	rts::PhysicsIntegrationRuntimeMetrics m_physicsMetricsAtStart;
	rts::PhysicsIntegrationRuntimeMetrics m_physicsMetricsFrozen;
	Bool m_physicsMetricsAwaitingInitialReset;
	rts::ObjectStatusTimerRuntimeMetrics m_statusMetricsAtStart;
	rts::ObjectStatusTimerRuntimeMetrics m_statusMetricsFrozen;
	Bool m_statusMetricsAwaitingInitialReset;
	rts::ImmutableSpatialRuntimeMetrics m_spatialMetricsAtStart;
	rts::ImmutableSpatialRuntimeMetrics m_spatialMetricsFrozen;
	Bool m_spatialMetricsAwaitingInitialReset;
#endif
};

int countProcessesRunning(const std::vector<WorkerProcess>& processes)
{
	int numProcessesRunning = 0;
	size_t i = 0;
	for (; i < processes.size(); ++i)
	{
		if (processes[i].isRunning())
			++numProcessesRunning;
	}
	return numProcessesRunning;
}
} // namespace

int ReplaySimulation::simulateReplaysInThisProcess(const std::vector<AsciiString> &filenames)
{
	int numErrors = 0;

	if (!TheGlobalData->m_headless)
	{
		s_isRunning = true;
		s_replayIndex = 0;
		s_replayCount = static_cast<UnsignedInt>(filenames.size());

		// If we are not in headless mode, we need to run the replay in the engine.
		for (; s_replayIndex < s_replayCount; ++s_replayIndex)
		{
			if (!TheRecorder->playbackFile(filenames[s_replayIndex]))
			{
				numErrors++;
				continue;
			}
			TheGameEngine->execute();
			if (TheRecorder->sawCRCMismatch() || TheRecorder->hasReplayReadError())
				numErrors++;
			if (!s_isRunning)
				break;
			TheGameEngine->setQuitting(FALSE);
		}
		s_isRunning = false;
		s_replayIndex = 0;
		s_replayCount = 0;
		return numErrors != 0 ? 1 : 0;
	}
	// Note that we use printf here because this is run from cmd.
	DWORD totalStartTimeMillis = GetTickCount();
	const rts::PipelineExecutionMode requestedPipelineMode =
		rts::GetPipelineExecutionMode();
#if defined(_WIN64)
	// A receipt is intentionally one executable-owned artifact per process.
	// Multi-replay and worker-process orchestration remain caller-owned and do
	// not get a synthetic aggregate receipt here.
	rts::performance::PerformanceReceipt performanceReceipt;
	bool performanceReceiptRequested = filenames.size() == 1;
	if (performanceReceiptRequested)
	{
#if defined(RTS_GENERALS)
		const char *performanceReceiptTitle = "Generals";
#elif defined(RTS_ZEROHOUR)
		const char *performanceReceiptTitle = "ZeroHour";
#else
		const char *performanceReceiptTitle = "Unknown";
#endif
		std::string reason;
		if (!rts::performance::BeginPerformanceReceipt(performanceReceipt,
			performanceReceiptTitle, filenames[0].str(), 0, &reason))
		{
			performanceReceiptRequested = false;
			printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n",
				reason.c_str());
			fflush(stdout);
		}
	}
#endif
	for (size_t i = 0; i < filenames.size(); i++)
	{
		rts::frame_timing::Session frameTimingSession("headless");
		AsciiString filename = filenames[i];
		HeadlessSimulationJobSystemScope simulationJobs(filename.str(),
			requestedPipelineMode,
#if defined(_WIN64)
			performanceReceiptRequested ?
				static_cast<void *>(&performanceReceipt) : 0
#else
			0
#endif
			);
		printf("Simulating Replay \"%s\"\n", filename.str());
		fflush(stdout);
		DWORD startTimeMillis = GetTickCount();
		if (TheRecorder->simulateReplay(filename))
		{
			Bool replayFailed = FALSE;
			if (TheRecorder->hasReplayReadError())
			{
				printf("REPLAY_FAIL reason=malformed_command\n");
				fflush(stdout);
				simulationJobs.setReplayResult(
					TheGameLogic != 0 ? TheGameLogic->getFrame() : 0,
					0, false, false);
				numErrors++;
				continue;
			}
			UnsignedInt totalTimeSec = TheRecorder->getPlaybackFrameCount() / LOGICFRAMES_PER_SECOND;
			while (TheRecorder->isPlaybackInProgress())
			{
				rts::frame_timing::BeginFrame(TheGameLogic->getFrame());
				// Freeze the last completed frame before a terminal
				// MSG_CLEAR_GAME_DATA can advance the slice reset epochs.
				simulationJobs.captureSliceMetrics();
				const int progressFrameInterval = 10*60*LOGICFRAMES_PER_SECOND;
				if (TheGameLogic->getFrame() != 0 && TheGameLogic->getFrame() % progressFrameInterval == 0)
				{
					// Print progress report
					UnsignedInt gameTimeSec = TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND;
					UnsignedInt realTimeSec = (GetTickCount()-startTimeMillis) / 1000;
					printf("Elapsed Time: %02d:%02d Game Time: %02d:%02d/%02d:%02d\n",
							realTimeSec/60, realTimeSec%60, gameTimeSec/60, gameTimeSec%60, totalTimeSec/60, totalTimeSec%60);
					fflush(stdout);
				}
				{
					rts::frame_timing::Scope frameTiming(rts::frame_timing::Logic);
					TheGameLogic->UPDATE();
				}
				simulationJobs.captureSliceMetrics();
				rts::frame_timing::EndFrame(TheGameLogic->getFrame());
				if (TheRecorder->hasReplayReadError())
				{
					printf("REPLAY_FAIL reason=malformed_command\n");
					fflush(stdout);
					numErrors++;
					replayFailed = TRUE;
					simulationJobs.setReplayResult(TheGameLogic->getFrame(),
						0, false, false);
					break;
				}
				if (TheRecorder->sawCRCMismatch())
				{
					numErrors++;
					replayFailed = TRUE;
					simulationJobs.setReplayResult(TheGameLogic->getFrame(),
						0, false, false);
					break;
				}
				simulationJobs.startAfterUnsafeInitialization();
			}
			if (replayFailed)
				continue;
			const UnsignedInt finalFrame = TheGameLogic->getFrame();
			const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_RECALC);
			simulationJobs.setReplayResult(finalFrame, finalCRC, true, true);
			printf("SIMULATION_REPLAY_RESULT replay=\"%s\" final_frame=%u final_crc=%08X\n",
				filename.str(), finalFrame, finalCRC);
			UnsignedInt gameTimeSec = TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND;
			UnsignedInt realTimeSec = (GetTickCount()-startTimeMillis) / 1000;
			printf("Elapsed Time: %02d:%02d Game Time: %02d:%02d/%02d:%02d\n",
					realTimeSec/60, realTimeSec%60, gameTimeSec/60, gameTimeSec%60, totalTimeSec/60, totalTimeSec%60);
			fflush(stdout);
		}
		else if (TheRecorder->hasReplayReadError())
		{
			printf("REPLAY_FAIL reason=malformed_command\n");
			fflush(stdout);
			simulationJobs.setReplayResult(
				TheGameLogic != 0 ? TheGameLogic->getFrame() : 0,
				0, false, false);
			numErrors++;
		}
		else
		{
			printf("Cannot open replay\n");
			simulationJobs.setReplayResult(0, 0, false, false);
			numErrors++;
		}
	}
	if (filenames.size() > 1)
	{
		printf("Simulation of all replays completed. Errors occurred: %d\n", numErrors);

		UnsignedInt realTime = (GetTickCount()-totalStartTimeMillis) / 1000;
		printf("Total Time: %d:%02d:%02d\n", realTime/60/60, realTime/60%60, realTime%60);
		fflush(stdout);
	}

#if defined(_WIN64)
	if (performanceReceiptRequested)
	{
		std::string reason;
		std::string writtenPath;
		const bool clean = numErrors == 0;
		// The frame-timing session is destroyed at the end of the replay loop,
		// so resolve its single closed CSV before the executable hashes it.
		const bool timingEvidenceClosed =
			resolvePerformanceReceiptTimingPath(performanceReceipt);
		const bool rawEvidenceClosed =
			timingEvidenceClosed &&
			writePerformanceReceiptRawDiagnostic(performanceReceipt);
		const bool resultCaptured = rawEvidenceClosed &&
			rts::performance::SetPerformanceReceiptReplayResult(
				performanceReceipt, performanceReceipt.frameStart,
				performanceReceipt.finalFrame, performanceReceipt.finalCrc,
				performanceReceipt.finalCrcKnown,
				clean ? 0 : 1, true,
				"ReplaySimulation::simulateReplaysInThisProcess:return", clean,
				&reason);
		if (!rawEvidenceClosed)
			reason = timingEvidenceClosed ?
				"game-owned raw diagnostic could not be closed" :
				"closed frame-timing CSV could not be resolved";
		const bool written = resultCaptured &&
			rts::performance::WritePerformanceReceiptAtomically(
				performanceReceipt, performanceReceipt.outputDirectory.c_str(),
				&writtenPath, &reason);
		if (written)
			printf("SIMULATION_PERFORMANCE_RECEIPT status=written path=%s\n",
				writtenPath.c_str());
		else
			printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n",
				reason.c_str());
		fflush(stdout);
	}
#endif

	return numErrors != 0 ? 1 : 0;
}

int ReplaySimulation::simulateReplaysInWorkerProcesses(const std::vector<AsciiString> &filenames, int maxProcesses)
{
	DWORD totalStartTimeMillis = GetTickCount();

	WideChar exePath[1024];
	GetModuleFileNameW(nullptr, exePath, ARRAY_SIZE(exePath));

	std::vector<WorkerProcess> processes;
	int filenamePositionStarted = 0;
	int filenamePositionDone = 0;
	int numErrors = 0;

	while (true)
	{
		int i;
		for (i = 0; i < processes.size(); i++)
			processes[i].update();

		// Get result of finished processes and print output in order
		while (!processes.empty())
		{
			if (!processes[0].isDone())
				break;
			AsciiString stdOutput = processes[0].getStdOutput();
			printf("%d/%d %s", filenamePositionDone+1, (int)filenames.size(), stdOutput.str());
			DWORD exitcode = processes[0].getExitCode();
			if (exitcode != 0)
				printf("Error!\n");
			fflush(stdout);
			numErrors += exitcode == 0 ? 0 : 1;
			processes.erase(processes.begin());
			filenamePositionDone++;
		}

		int numProcessesRunning = countProcessesRunning(processes);

		// Add new processes when we are below the limit and there are replays left
		while (numProcessesRunning < maxProcesses && filenamePositionStarted < filenames.size())
		{
			UnicodeString filenameWide;
			filenameWide.translate(filenames[filenamePositionStarted]);
			const rts::JobSystemConfig jobConfig = rts::JobSystem::startupConfig();
			const WideChar *workerPolicy = jobConfig.workerPolicy ==
				rts::JOB_WORKER_POLICY_ALL ? L"all" : L"auto";
			const WideChar *pipelineMode = rts::GetPipelineExecutionMode() ==
				rts::PIPELINE_EXECUTION_SERIAL ? L"serial" : L"parallel";
			const WideChar *simulationMode = simulationModeNameWide(
				rts::GetSimulationExecutionMode());
			UnicodeString command;
			if (jobConfig.workerCount != 0)
			{
				command.format(L"\"%s\"%s%s -workerCount %u -workerPolicy %s -pipelineMode %s -simulationMode %s -replay \"%s\"",
					exePath,
					TheGlobalData->m_windowed ? L" -win" : L"",
					TheGlobalData->m_headless ? L" -headless" : L"",
					jobConfig.workerCount, workerPolicy, pipelineMode,
					simulationMode, filenameWide.str());
			}
			else
			{
				command.format(L"\"%s\"%s%s -workerPolicy %s -pipelineMode %s -simulationMode %s -replay \"%s\"",
					exePath,
					TheGlobalData->m_windowed ? L" -win" : L"",
					TheGlobalData->m_headless ? L" -headless" : L"",
					workerPolicy, pipelineMode, simulationMode,
					filenameWide.str());
			}

			processes.push_back(WorkerProcess());
			processes.back().startProcess(command);

			filenamePositionStarted++;
			numProcessesRunning++;
		}

		if (processes.empty())
			break;

		// Don't waste CPU here, our workers need every bit of CPU time they can get
		Sleep(100);
	}

	DEBUG_ASSERTCRASH(filenamePositionStarted == filenames.size(), ("inconsistent file position 1"));
	DEBUG_ASSERTCRASH(filenamePositionDone == filenames.size(), ("inconsistent file position 2"));

	printf("Simulation of all replays completed. Errors occurred: %d\n", numErrors);

	UnsignedInt realTime = (GetTickCount()-totalStartTimeMillis) / 1000;
	printf("Total Wall Time: %d:%02d:%02d\n", realTime/60/60, realTime/60%60, realTime%60);
	fflush(stdout);

	return numErrors != 0 ? 1 : 0;
}

std::vector<AsciiString> ReplaySimulation::resolveFilenameWildcards(const std::vector<AsciiString> &filenames)
{
	// If some filename contains wildcards, search for actual filenames.
	// Note that we cannot do this in parseReplay because we require TheLocalFileSystem initialized.
	std::vector<AsciiString> filenamesResolved;
	for (std::vector<AsciiString>::const_iterator filename = filenames.begin(); filename != filenames.end(); ++filename)
	{
		if (filename->find('*') || filename->find('?'))
		{
			AsciiString dir1 = TheRecorder->getReplayDir();
			AsciiString dir2 = *filename;
			AsciiString wildcard = *filename;
			{
				int len = dir2.getLength();
				while (len)
				{
					char c = dir2.getCharAt(len-1);
					if (c == '/' || c == '\\')
					{
						wildcard.set(wildcard.str()+dir2.getLength());
						break;
					}
					dir2.removeLastChar();
					len--;
				}
			}

			FilenameList files;
			TheLocalFileSystem->getFileListInDirectory(dir2.str(), dir1.str(), wildcard, files, FALSE);
			for (FilenameList::iterator it = files.begin(); it != files.end(); ++it)
			{
				AsciiString file;
				file.set(it->str() + dir1.getLength());
				filenamesResolved.push_back(file);
			}
		}
		else
			filenamesResolved.push_back(*filename);
	}
	return filenamesResolved;
}

int ReplaySimulation::simulateReplays(const std::vector<AsciiString> &filenames, int maxProcesses)
{
	std::vector<AsciiString> filenamesResolved = resolveFilenameWildcards(filenames);
	if (maxProcesses == SIMULATE_REPLAYS_SEQUENTIAL)
		return simulateReplaysInThisProcess(filenamesResolved);
	else
		return simulateReplaysInWorkerProcesses(filenamesResolved, maxProcesses);
}
