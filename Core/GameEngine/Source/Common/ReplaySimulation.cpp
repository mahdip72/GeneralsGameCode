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
#include "Common/PerformanceReceiptRuntime.h"
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
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Object.h"
#include "Lib/PerformanceReceipt.h"
#include "Lib/ReplayPathContract.h"
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

#if defined(_WIN64)
PerformanceReceiptOwnerLifecycle::PerformanceReceiptOwnerLifecycle()
	: m_begun(false), m_finalized(false), m_terminalResultKnown(false),
	  m_contiguous(true), m_lastCompletedFrame(0), m_terminalFrame(0), m_terminalCrc(0)
{
}

bool PerformanceReceiptOwnerLifecycle::begin()
{
	if (m_begun) return false;
	m_begun = true;
	return true;
}

bool PerformanceReceiptOwnerLifecycle::observeCompletedFrame(unsigned frame)
{
	if (!m_begun || m_finalized || frame == 0 || frame <= m_lastCompletedFrame ||
		(m_terminalResultKnown && frame > m_terminalFrame)) return false;
	if (frame != m_lastCompletedFrame + 1) m_contiguous = false;
	m_lastCompletedFrame = frame;
	return true;
}

bool PerformanceReceiptOwnerLifecycle::captureTerminalResult(unsigned actualFrame, unsigned crc)
{
	if (!m_begun || m_finalized || m_terminalResultKnown || actualFrame == 0 ||
		actualFrame < m_lastCompletedFrame) return false;
	m_terminalResultKnown = true;
	m_terminalFrame = actualFrame;
	m_terminalCrc = crc;
	return true;
}

bool PerformanceReceiptOwnerLifecycle::finish(unsigned outstandingJobs,
	unsigned pendingOwnerCompletions)
{
	if (!m_begun || m_finalized) return false;
	m_finalized = true;
	return m_terminalResultKnown && m_contiguous && m_lastCompletedFrame != 0 &&
		m_lastCompletedFrame == m_terminalFrame && outstandingJobs == 0 &&
		pendingOwnerCompletions == 0;
}
#endif

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
// Prevent replacement or mutation of the exact loose replay while both the
// diagnostic hasher and Recorder read it. Failure disables evidence only.
class ImmutableReplayReceiptSource
{
public:
	ImmutableReplayReceiptSource() : m_file(INVALID_HANDLE_VALUE) { m_sha256[0] = '\0'; }
	~ImmutableReplayReceiptSource()
	{
		if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
	}
	bool open(const char *path)
	{
		m_file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
		return m_file != INVALID_HANDLE_VALUE &&
			HashSkirmishAITestContentFile(path, m_sha256) != FALSE;
	}
	const char *sha256() const { return m_sha256; }
private:
	HANDLE m_file;
	char m_sha256[65];
	ImmutableReplayReceiptSource(const ImmutableReplayReceiptSource &);
	ImmutableReplayReceiptSource &operator=(const ImmutableReplayReceiptSource &);
};

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
	const rts::ImmutableSpatialRuntimeMetrics &spatial,
	const rts::LiveSimulationPhaseRuntimeMetrics &phaseMetrics,
	const rts::AIPlanningRuntimeMetrics &ai,
	const OrdinaryPathRuntimeMetrics &path)
{
	receipt.phases.clear();
	const char *names[] = { "owner-intake", "legacy-mutable-island",
		"spatial-work", "owner-tail", "verification-publication" };
	for (unsigned index = 0; index != 5; ++index)
		appendPerformanceReceiptPhase(receipt, names[index],
			phaseMetrics.ownerPhaseSampleCount[index] != 0,
			phaseMetrics.ownerPhaseTotalNanoseconds[index],
			phaseMetrics.ownerPhaseMaximumNanoseconds[index],
			phaseMetrics.ownerPhaseSampleCount[index]);
	// Inclusive owner phases are not serial portions: worker kernels execute
	// inside them. The receipt retains the default unknown serial coverage.
	receipt.frameSimulationTotalNanoseconds =
		phaseMetrics.frameSimulationTotalNanoseconds;
	receipt.frameSimulationMaximumNanoseconds =
		phaseMetrics.frameSimulationMaximumNanoseconds;
	receipt.frameSimulationSampleCount = phaseMetrics.frameSimulationSampleCount;

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
	appendPerformanceReceiptKernel(receipt, "path", true,
		path.submittedRangeJobs,
		static_cast<rts::JobMetricCounter>(path.workerExecutedRangeJobs) +
			path.ownerHelpedRangeJobs,
		path.workerExecutedRangeJobs, path.ownerHelpedRangeJobs,
		path.physicalWorkerMask, path.distinctPhysicalWorkers,
		path.physicalWorkerMaskComplete != FALSE);
}

bool resolvePerformanceReceiptTimingPath(
	rts::performance::PerformanceReceipt &receipt)
{
	const rts::frame_timing::FinalizedCapture capture =
		rts::frame_timing::Capture::instance().finalize();
	receipt.rawEvidence.timingClosed = capture.closed;
	receipt.rawEvidence.timingWriteSucceeded = capture.writeSucceeded;
	receipt.rawEvidence.timingTruncated = capture.truncated;
	receipt.rawEvidence.timingComplete = capture.complete;
	receipt.rawEvidence.timingSessionCount = capture.sessionCount;
	receipt.rawEvidence.timingFrameSamples = capture.frameSamples;
	receipt.rawEvidence.timingFirstFrame = capture.firstFrame;
	receipt.rawEvidence.timingLastFrame = capture.lastFrame;
	if (!capture.complete || capture.sessionCount != 1)
		return false;
	// The host may request the exact file or its directory; it cannot nominate
	// a different CSV. Resolve both paths without searching directory contents.
	char expected[MAX_PATH], actual[MAX_PATH];
	const DWORD expectedLength = GetFullPathNameA(receipt.rawEvidence.timingPath.c_str(),
		sizeof(expected), expected, 0);
	const DWORD actualLength = GetFullPathNameA(capture.path.c_str(),
		sizeof(actual), actual, 0);
	if (expectedLength == 0 || expectedLength >= sizeof(expected) ||
		actualLength == 0 || actualLength >= sizeof(actual))
		return false;
	const DWORD attributes = GetFileAttributesA(expected);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return false;
	std::string expectedPath(expected), actualPath(actual);
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		while (!expectedPath.empty() &&
			(expectedPath[expectedPath.size() - 1] == '\\' ||
				expectedPath[expectedPath.size() - 1] == '/'))
			expectedPath.erase(expectedPath.size() - 1);
		const std::size_t separator = actualPath.find_last_of("\\/");
		if (separator == std::string::npos ||
			_stricmp(expectedPath.c_str(), actualPath.substr(0, separator).c_str()) != 0)
			return false;
	}
	else if (_stricmp(expectedPath.c_str(), actualPath.c_str()) != 0)
		return false;
	receipt.rawEvidence.timingPath = actualPath;
	return true;
}

bool writePerformanceReceiptRawDiagnostic(
	const rts::performance::PerformanceReceipt &receipt)
{
	if (receipt.rawEvidence.rawLogPath.empty()) return false;
	FILE *file = fopen(receipt.rawEvidence.rawLogPath.c_str(), "wx");
	if (file == 0) return false;
	fprintf(file, "producer=game-executable-performance-receipt-v5\n");
	fprintf(file, "game_owned=1\n");
	fprintf(file, "run_id=%s\n", receipt.runId.c_str());
	fprintf(file, "process_id=%u\n", receipt.processId);
	fprintf(file, "process_creation_time_utc_100ns=%llu\n",
		static_cast<unsigned long long>(receipt.processCreationTimeUtc100ns));
	fprintf(file, "executable_sha256=%s\n",
		receipt.executableSha256.c_str());
	fprintf(file, "command_line=%s\n", receipt.commandLine.c_str());
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
			static_cast<PerformanceReceiptRuntime *>(performanceReceipt))
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
		m_ordinaryPathMetricsAtStart = GetOrdinaryPathRuntimeMetrics();
		memset(&m_ordinaryPathMetricsFrozen, 0, sizeof(m_ordinaryPathMetricsFrozen));
		m_ordinaryPathMetricsAwaitingInitialReset = TRUE;
#endif
	}

	~HeadlessSimulationJobSystemScope()
	{
		if (!m_startAttempted) return;
		rts::JobSystem &jobs = rts::JobSystem::instance();
		#if defined(_WIN64)
		if (m_performanceReceipt != 0)
			m_performanceReceipt->captureSchedulerBeforeTeardown();
		#endif
		if (m_started) jobs.shutdown();
		const rts::JobSystemMetrics metrics = jobs.metrics();
#if defined(_WIN64)
		captureSliceMetrics();
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
		const OrdinaryPathRuntimeMetrics pathMetrics = GetOrdinaryPathRuntimeMetrics();
		AccumulateSkirmishAITestOrdinaryPathMetrics(&m_ordinaryPathMetricsAtStart,
			pathMetrics, &m_ordinaryPathMetricsFrozen,
			&m_ordinaryPathMetricsAwaitingInitialReset);
#endif
	}

	void captureCompletedFrameMetrics(unsigned previousFrame)
	{
#if defined(_WIN64)
		if (m_performanceReceipt != 0)
			m_performanceReceipt->captureCompletedFrame(previousFrame,
				m_collisionMetricsFrozen, m_physicsMetricsFrozen,
				m_statusMetricsFrozen, m_spatialMetricsFrozen, m_ordinaryPathMetricsFrozen);
#else
		(void)previousFrame;
#endif
	}

	void setReplayResult(unsigned finalFrame, unsigned finalCrc,
		bool finalCrcKnown, bool clean)
	{
#if defined(_WIN64)
		if (m_performanceReceipt != 0)
			m_performanceReceipt->captureTerminalResult(finalFrame, finalCrc, finalCrcKnown, clean);
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
	PerformanceReceiptRuntime *m_performanceReceipt;
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
	OrdinaryPathRuntimeMetrics m_ordinaryPathMetricsAtStart;
	OrdinaryPathRuntimeMetrics m_ordinaryPathMetricsFrozen;
	Bool m_ordinaryPathMetricsAwaitingInitialReset;
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

#if defined(_WIN64)
PerformanceReceiptRuntime::PerformanceReceiptRuntime() : m_active(false) {}

bool PerformanceReceiptRuntime::begin(const char *fixtureKind, const char *replayPath)
{
	if (!m_lifecycle.begin()) return false;
#if defined(RTS_GENERALS)
	const char *title = "Generals";
#elif defined(RTS_ZEROHOUR)
	const char *title = "ZeroHour";
#else
	const char *title = "Unknown";
#endif
	std::string reason;
	if (!rts::performance::BeginPerformanceReceipt(m_receipt, title, replayPath, 0, &reason) ||
		m_receipt.fixtureKind != fixtureKind)
	{
		if (reason.empty()) reason = "receipt fixture kind does not match its real owner";
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", reason.c_str());
		fflush(stdout);
		return false;
	}
	m_receipt.simulationMode = simulationModeName(rts::GetSimulationExecutionMode());
	rts::performance::KernelPerformanceLedger::instance().beginRun(true);
	rts::performance::KernelPerformanceReferenceLedger::instance().beginRun(
		m_receipt.kernelReference.mode);
	m_active = true;
	return true;
}

void PerformanceReceiptRuntime::invalidate(const char *reason)
{
	if (active() && m_failure.empty()) m_failure = reason;
}

void PerformanceReceiptRuntime::bindFixture(const char *kind, const char *contentPath,
	const char *sha256, unsigned seed)
{
	if (!active()) return;
	std::string reason;
	if (!rts::performance::BindPerformanceReceiptFixtureObservation(m_receipt,
		kind, contentPath, sha256, seed, &reason)) invalidate(reason.c_str());
}

void PerformanceReceiptRuntime::captureCompletedFrame(unsigned previousFrame,
	const rts::CollisionCandidateRuntimeMetrics &collision,
	const rts::PhysicsIntegrationRuntimeMetrics &physics,
	const rts::ObjectStatusTimerRuntimeMetrics &status,
	const rts::ImmutableSpatialRuntimeMetrics &spatial,
	const OrdinaryPathRuntimeMetrics &path)
{
	if (!active() || TheGameLogic == 0 || ThePlayerList == 0) return;
	const unsigned frame = TheGameLogic->getFrame();
	if (frame <= previousFrame || !m_lifecycle.observeCompletedFrame(frame)) return;
	unsigned players = 0;
	for (Int index = 0; index < ThePlayerList->getPlayerCount(); ++index)
	{
		Player *player = ThePlayerList->getNthPlayer(index);
		if (player != 0 && rts::performance::IsPerformanceReceiptRosterPlayer(
			player->isPlayableSide() != FALSE, player->isPlayerObserver() != FALSE)) ++players;
	}
	unsigned units = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object != 0;
		object = object->getNextObject())
	{
		if (rts::performance::IsPerformanceReceiptLiveUnit(
			object->isKindOf(KINDOF_INFANTRY) != FALSE,
			object->isKindOf(KINDOF_VEHICLE) != FALSE,
			object->isEffectivelyDead() != FALSE, object->isDestroyed() != FALSE)) ++units;
	}
	if (!rts::performance::ObservePerformanceReceiptWorkload(m_receipt.workload,
		frame, players, units))
	{
		invalidate("completed-frame workload could not be retained");
		return;
	}
	capturePerformanceReceiptMetrics(m_receipt, collision, physics, status, spatial,
		TheGameLogic->getStage5PhaseRuntimeMetrics(), rts::GetAIPlanningRuntimeMetrics(), path);
}

void PerformanceReceiptRuntime::captureTerminalResult(unsigned actualFrame,
	unsigned crc, bool crcKnown, bool clean)
{
	if (!active()) return;
	if (!crcKnown || !clean || !m_lifecycle.captureTerminalResult(actualFrame, crc))
		invalidate("clean terminal owner frame and CRC were not captured exactly once");
	else
		// Teardown may reset the world frame while owner work is still draining.
		// Stop new observations, but keep existing timing/reference tokens live.
		rts::performance::KernelPerformanceLedger::instance().sealAdmissions();
}

void PerformanceReceiptRuntime::captureSchedulerBeforeTeardown()
{
	if (!active()) return;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	m_receipt.simulationMode = simulationModeName(rts::GetSimulationExecutionMode());
	m_receipt.schedulerStarted = m_receipt.schedulerStarted || jobs.isRunning();
	std::string reason;
	if (!rts::performance::CapturePerformanceReceiptJobSystem(m_receipt,
		jobs, jobs.metrics(), &reason)) invalidate(reason.c_str());
}

void PerformanceReceiptRuntime::retainClosedReplay(const char *path, const char *sha256)
{
	if (!active()) return;
	m_receipt.retainedReplayPath = path;
	m_receipt.retainedReplaySha256 = sha256;
}

static void printUnsupportedPerformanceSnapshot(
	const rts::performance::PerformanceReceipt &receipt,
	unsigned outstanding, unsigned ownerCompletions)
{
	using namespace rts::performance;
	const KernelPerformanceSnapshot &timing = receipt.kernelTiming;
	const KernelPerformanceReferenceSnapshot &reference = receipt.kernelReference;
	// These are retained snapshots only, after the owner-dependent capture and
	// drain boundary. Bounded failure-only output never changes qualification.
	printf("SIMULATION_PERFORMANCE_SNAPSHOT timing_enabled=%u timing_frozen=%u timing_complete=%u timing_errors=%u timing_generation=%llu timing_streams=%u reference_mode=%u reference_frozen=%u reference_complete=%u reference_errors=%u reference_generation=%llu reference_streams=%u outstanding_jobs=%u pending_owner_completions=%u frame_start=%u frame_end=%u final_frame=%u\n",
		static_cast<unsigned>(timing.enabled), static_cast<unsigned>(timing.frozen),
		static_cast<unsigned>(timing.complete), timing.errors,
		static_cast<unsigned long long>(timing.generation), timing.streamCount,
		static_cast<unsigned>(reference.mode), static_cast<unsigned>(reference.frozen),
		static_cast<unsigned>(reference.complete), reference.errors,
		static_cast<unsigned long long>(reference.generation), reference.streamCount,
		outstanding, ownerCompletions, receipt.frameStart, receipt.frameEnd, receipt.finalFrame);
	for (unsigned index = 0; index < timing.streamCount &&
		index < KERNEL_PERFORMANCE_MAXIMUM_STREAMS; ++index)
	{
		const KernelPerformanceStream &stream = timing.streams[index];
		printf("SIMULATION_PERFORMANCE_TIMING_STREAM index=%u kernel=%u subtype=%u first_frame=%u last_frame=%u attempted=%llu admitted=%llu committed=%llu aborted=%llu active_ns=%llu inclusive_ns=%llu maximum_ns=%llu",
			index, static_cast<unsigned>(stream.kernel), stream.subtype,
			stream.firstFrame, stream.lastFrame,
			static_cast<unsigned long long>(stream.attemptedBatches),
			static_cast<unsigned long long>(stream.admittedBatches),
			static_cast<unsigned long long>(stream.committedBatches),
			static_cast<unsigned long long>(stream.abortedBatches),
			static_cast<unsigned long long>(stream.activePipelineNanoseconds),
			static_cast<unsigned long long>(stream.inclusiveBatchNanoseconds),
			static_cast<unsigned long long>(stream.maximumBatchNanoseconds));
		for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
			printf(" stage%u_ns=%llu stage%u_samples=%llu", stage,
				static_cast<unsigned long long>(stream.stageNanoseconds[stage]), stage,
				static_cast<unsigned long long>(stream.stageSamples[stage]));
		printf("\n");
	}
	for (unsigned index = 0; index < reference.streamCount &&
		index < KERNEL_PERFORMANCE_MAXIMUM_STREAMS; ++index)
	{
		const KernelPerformanceReferenceStream &stream = reference.streams[index];
		printf("SIMULATION_PERFORMANCE_REFERENCE_STREAM index=%u kernel=%u subtype=%u field_schema=%u first_frame=%u last_frame=%u validated_batches=%llu committed_batches=%llu aborted_batches=%llu validated_operations=%llu committed_operations=%llu serial_samples=%llu serial_ns=%llu maximum_serial_ns=%llu input_digest_valid=%u output_digest_valid=%u commit_digest_valid=%u\n",
			index, static_cast<unsigned>(stream.kernel), stream.subtype, stream.fieldSchema,
			stream.firstFrame, stream.lastFrame,
			static_cast<unsigned long long>(stream.validatedBatchCount),
			static_cast<unsigned long long>(stream.committedBatchCount),
			static_cast<unsigned long long>(stream.abortedBatchCount),
			static_cast<unsigned long long>(stream.validatedOperationCount),
			static_cast<unsigned long long>(stream.committedOperationCount),
			static_cast<unsigned long long>(stream.serialSampleCount),
			static_cast<unsigned long long>(stream.serialNanoseconds),
			static_cast<unsigned long long>(stream.maximumSerialNanoseconds),
			static_cast<unsigned>(stream.inputDigest.valid),
			static_cast<unsigned>(stream.outputDigest.valid),
			static_cast<unsigned>(stream.commitDigest.valid));
	}
}

void PerformanceReceiptRuntime::finish(int exitCode, const char *boundary)
{
	if (!active()) return;
	// No game globals or owner registration are needed below this boundary.
	const rts::JobSystem &jobs = rts::JobSystem::instance();
	const unsigned outstanding = jobs.outstandingJobCount();
	const unsigned ownerCompletions = jobs.pendingOwnerCompletionCount();
	const bool complete = m_lifecycle.finish(outstanding, ownerCompletions);
	if (outstanding == 0 && ownerCompletions == 0)
	{
		m_receipt.kernelTiming = rts::performance::KernelPerformanceLedger::instance().freeze();
		m_receipt.kernelReference = rts::performance::KernelPerformanceReferenceLedger::instance().freeze();
	}
	m_receipt.frameStart = 0;
	m_receipt.frameEnd = m_lifecycle.terminalFrame();
	m_receipt.finalFrame = m_lifecycle.terminalFrame();
	m_receipt.finalCrc = m_lifecycle.terminalCrc();
	m_receipt.finalCrcKnown = m_lifecycle.terminalResultKnown();
	const bool timingClosed = resolvePerformanceReceiptTimingPath(m_receipt);
	std::string reason = m_failure;
	if (reason.empty() && !complete)
		reason = "owner lifecycle is incomplete or authoritative work is not drained";
	if (reason.empty() && exitCode != 0) reason = "owner run did not exit cleanly";
	if (reason.empty() && !timingClosed) reason = "closed frame-timing CSV could not be resolved";
	const bool clean = reason.empty();
	const bool rawClosed = clean && writePerformanceReceiptRawDiagnostic(m_receipt);
	if (clean && !rawClosed) reason = "game-owned raw diagnostic could not be closed";
	const bool resultCaptured = rawClosed && rts::performance::SetPerformanceReceiptReplayResult(
		m_receipt, m_receipt.frameStart, m_receipt.finalFrame, m_receipt.finalCrc,
		m_receipt.finalCrcKnown, exitCode, true, boundary, true, &reason);
	std::string writtenPath;
	const bool written = resultCaptured && rts::performance::WritePerformanceReceiptAtomically(
		m_receipt, m_receipt.outputDirectory.c_str(), &writtenPath, &reason);
	if (written)
		printf("SIMULATION_PERFORMANCE_RECEIPT status=written path=%s\n", writtenPath.c_str());
	else
	{
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", reason.c_str());
		printUnsupportedPerformanceSnapshot(m_receipt, outstanding, ownerCompletions);
	}
	fflush(stdout);
	m_active = false;
}
#endif

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
	PerformanceReceiptRuntime performanceReceipt;
	char performanceSourcePath[MAX_PATH] = {0};
	const bool performanceSourcePathValid = filenames.size() == 1 &&
		rts::replay::ResolveReplayPlaybackPath(RecorderClass::getReplayDir().str(),
			filenames[0].str(), true, performanceSourcePath, sizeof(performanceSourcePath));
	if (filenames.size() == 1 && !performanceSourcePathValid)
	{
		printf("Invalid native headless replay path: %s\n", filenames[0].str());
		return 1;
	}
	const bool performanceReceiptRequested = performanceSourcePathValid &&
		performanceReceipt.begin("replay", performanceSourcePath);
	ImmutableReplayReceiptSource performanceSource;
	if (performanceReceiptRequested)
	{
		if (!performanceSource.open(performanceSourcePath))
			performanceReceipt.invalidate("immutable replay content could not be hashed");
	}
#endif
	for (size_t i = 0; i < filenames.size(); i++)
	{
		rts::frame_timing::Session frameTimingSession("headless");
		AsciiString filename = filenames[i];
		AsciiString playbackFilename = filename;
#if defined(_WIN64)
		if (performanceSourcePathValid)
			playbackFilename = performanceSourcePath;
		else
		{
			char resolvedPath[MAX_PATH];
			if (!rts::replay::ResolveReplayPlaybackPath(RecorderClass::getReplayDir().str(),
				filename.str(), true, resolvedPath, sizeof(resolvedPath)))
			{
				printf("Invalid native headless replay path: %s\n", filename.str());
				++numErrors;
				continue;
			}
			playbackFilename = resolvedPath;
		}
#endif
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
		if (TheRecorder->simulateReplay(playbackFilename))
		{
			Bool replayFailed = FALSE;
#if defined(_WIN64)
			if (performanceReceiptRequested)
			{
				const GameInfo *recordedGame = TheRecorder->getGameInfo();
				if (recordedGame != 0 && performanceSource.sha256()[0] != '\0')
					performanceReceipt.bindFixture("replay", performanceSourcePath,
						performanceSource.sha256(), static_cast<unsigned>(recordedGame->getSeed()));
				else
					performanceReceipt.invalidate("loaded replay identity was unavailable");
			}
#endif
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
				const unsigned previousFrame = TheGameLogic->getFrame();
				rts::frame_timing::BeginFrame(previousFrame);
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
				// The diagnostic object scan remains inside the measured process
				// runtime; no estimated observer cost is subtracted.
				simulationJobs.captureCompletedFrameMetrics(previousFrame);
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
		performanceReceipt.finish(numErrors != 0 ? 1 : 0,
			"ReplaySimulation::simulateReplaysInThisProcess:return");
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
#if defined(_WIN64)
	if (TheGlobalData->m_headless)
	{
		for (size_t i = 0; i < filenames.size(); ++i)
		{
			char resolvedPath[MAX_PATH];
			if (!rts::replay::ResolveReplayPlaybackPath(RecorderClass::getReplayDir().str(),
				filenames[i].str(), true, resolvedPath, sizeof(resolvedPath)))
			{
				printf("Invalid native headless replay path: %s\n", filenames[i].str());
				return 1;
			}
		}
	}
#endif
	std::vector<AsciiString> filenamesResolved = resolveFilenameWildcards(filenames);
	if (maxProcesses == SIMULATE_REPLAYS_SEQUENTIAL)
		return simulateReplaysInThisProcess(filenamesResolved);
	else
		return simulateReplaysInWorkerProcesses(filenamesResolved, maxProcesses);
}
