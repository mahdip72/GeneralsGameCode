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
#include "Common/GameThreadOwnership.h"
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
#include <io.h>
#include <share.h>
#include <stdio.h>
#include "NativeReceiptTraceFiles.inc"
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
	ImmutableReplayReceiptSource()
		: m_file(INVALID_HANDLE_VALUE), m_guardCount(0), m_started(false),
		  m_finished(false), m_path(), m_directory(), m_fileIdentity()
	{
		m_sha256[0] = '\0';
	}
	~ImmutableReplayReceiptSource()
	{
		closeOwned();
	}
	bool open(const char *path)
	{
		if (path == 0 || m_started || m_finished ||
			m_file != INVALID_HANDLE_VALUE)
			return false;
		try
		{
			m_path = path;
			if (!native_receipt_files::safeNativePath(m_path))
				return failOpen();
			const size_t separator = m_path.rfind('\\');
			if (separator == std::string::npos || separator < 2 ||
				separator + 1 == m_path.size()) return failOpen();
			m_directory = separator == 2 ? m_path.substr(0, 3) :
				m_path.substr(0, separator);
			HANDLE exactDirectory = INVALID_HANDLE_VALUE;
			native_receipt_files::FileIdentity exactDirectoryIdentity;
			std::string reason;
			if (!native_receipt_files::openDirectoryComponents(m_directory,
				exactDirectory, m_guards, m_guardCount,
				exactDirectoryIdentity, &reason) || !captureGuardIdentities())
				return failOpen();
			if (!native_receipt_files::openExistingFile(m_path, m_file,
				m_fileIdentity, &reason) ||
				HashSkirmishAITestContentHandle(static_cast<void *>(m_file),
					m_sha256) == FALSE)
				return failOpen();
			m_started = true;
			return true;
		}
		catch (...)
		{
			return failOpen();
		}
	}
	bool finish()
	{
		if (!m_started || m_finished || m_file == INVALID_HANDLE_VALUE)
			return false;
		m_finished = true;
		char closedSha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] = {0};
		native_receipt_files::FileIdentity closedFileIdentity;
		const bool valid = validateGuardIdentities() &&
			native_receipt_files::queryIdentity(m_file, m_path, false,
				closedFileIdentity) &&
			native_receipt_files::sameIdentity(m_fileIdentity,
				closedFileIdentity) &&
			HashSkirmishAITestContentHandle(static_cast<void *>(m_file),
				closedSha256) != FALSE &&
			_stricmp(m_sha256, closedSha256) == 0;
		return closeOwned() && valid;
	}
	const char *sha256() const { return m_sha256; }
private:
	bool visitGuardIdentities(bool capture)
	{
		try
		{
			std::string current = m_directory.substr(0, 3);
			size_t start = 3;
			unsigned guardIndex = 0;
			for (;;)
			{
				if (guardIndex >= m_guardCount) return false;
				native_receipt_files::FileIdentity identity;
				if (!native_receipt_files::queryIdentity(m_guards[guardIndex],
					current, true, identity)) return false;
				if (capture) m_guardIdentities[guardIndex] = identity;
				else if (!native_receipt_files::sameIdentity(
					m_guardIdentities[guardIndex], identity)) return false;
				++guardIndex;
				if (start == m_directory.size())
					return guardIndex == m_guardCount;
				const size_t end = m_directory.find('\\', start);
				const size_t finish = end == std::string::npos ?
					m_directory.size() : end;
				current += current.size() == 3 ?
					m_directory.substr(start, finish - start) :
					std::string("\\") + m_directory.substr(start, finish - start);
				start = end == std::string::npos ? m_directory.size() : end + 1;
			}
		}
		catch (...) { return false; }
	}
	bool captureGuardIdentities() { return visitGuardIdentities(true); }
	bool validateGuardIdentities() { return visitGuardIdentities(false); }
	bool failOpen()
	{
		closeOwned();
		m_path.clear();
		m_directory.clear();
		m_sha256[0] = '\0';
		return false;
	}
	bool closeOwned()
	{
		bool result = native_receipt_files::closeOwnedHandle(m_file);
		if (!native_receipt_files::closeHandleList(m_guards, m_guardCount))
			result = false;
		m_started = false;
		return result;
	}
	HANDLE m_file;
	HANDLE m_guards[MAX_PATH];
	native_receipt_files::FileIdentity m_guardIdentities[MAX_PATH];
	unsigned m_guardCount;
	bool m_started;
	bool m_finished;
	std::string m_path;
	std::string m_directory;
	native_receipt_files::FileIdentity m_fileIdentity;
	char m_sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
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

bool projectPerformanceReceiptBaselineMetrics(
	rts::performance::PerformanceReceipt &receipt) noexcept
{
	using namespace rts::performance;
	if (receipt.kernelReference.mode !=
		KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
		return true;
	const KernelPerformancePhaseAccountingSnapshot &accounting =
		receipt.kernelTiming.phaseAccounting;
	if (receipt.schemaVersion != 6 ||
		receipt.kernelTiming.runRole !=
			KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE ||
		!accounting.requested || !accounting.frozen || !accounting.complete ||
		accounting.errors != 0 || !accounting.schedulerClosureKnown ||
		accounting.completedFrameCount == 0 ||
		receipt.phases.size() != KERNEL_PHASE_COUNT ||
		receipt.kernels.size() != KERNEL_PERFORMANCE_KERNEL_COUNT)
		return false;
	const char *const phaseNames[KERNEL_PHASE_COUNT] = {
		"owner-intake", "legacy-mutable-island", "spatial-work",
		"owner-tail", "verification-publication"
	};
	const char *const kernelNames[KERNEL_PERFORMANCE_KERNEL_COUNT] = {
		"physics", "status", "collision", "ai-planning", "spatial", "path"
	};
	const rts::JobMetricCounter maximum = ~static_cast<rts::JobMetricCounter>(0);
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &row = accounting.phases[index];
		if (receipt.phases[index].name != phaseNames[index] ||
			row.samples != accounting.completedFrameCount ||
			row.maximumNanoseconds > row.totalNanoseconds ||
			row.serialNanoseconds > maximum - row.pureNanoseconds ||
			row.serialNanoseconds + row.pureNanoseconds != row.totalNanoseconds)
			return false;
	}
	for (unsigned index = 0; index != KERNEL_PERFORMANCE_KERNEL_COUNT; ++index)
		if (receipt.kernels[index].name != kernelNames[index]) return false;

	// The completed-frame collector already owns the canonical vector storage.
	// Rewrite only scalar evidence here: finish is a no-allocation boundary.
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &row = accounting.phases[index];
		PerformanceReceiptPhase &phase = receipt.phases[index];
		phase.available = true;
		phase.totalNanoseconds = row.totalNanoseconds;
		phase.maximumNanoseconds = row.maximumNanoseconds;
		phase.sampleCount = row.samples;
		phase.serialNanoseconds = row.serialNanoseconds;
		phase.serialNanosecondsKnown = true;
		phase.pureNanoseconds = row.pureNanoseconds;
		phase.pureNanosecondsKnown = true;
	}
	for (unsigned index = 0; index != KERNEL_PERFORMANCE_KERNEL_COUNT; ++index)
	{
		PerformanceReceiptKernel &kernel = receipt.kernels[index];
		kernel.available = false;
		kernel.submittedJobs = 0;
		kernel.completedJobs = 0;
		kernel.physicalWorkerJobs = 0;
		kernel.ownerHelpedJobs = 0;
		kernel.physicalWorkerMask = 0;
		kernel.distinctPhysicalWorkers = 0;
		kernel.physicalWorkerMaskComplete = false;
		kernel.elapsedNanoseconds = 0;
		kernel.elapsedNanosecondsKnown = false;
	}
	return true;
}

bool resolvePerformanceReceiptTimingPath(
	rts::performance::PerformanceReceipt &receipt, HANDLE *retainedHandle = 0)
{
	rts::frame_timing::FinalizedCapture capture =
		rts::frame_timing::Capture::instance().finalize(retainedHandle != 0);
	if (retainedHandle != 0) *retainedHandle = INVALID_HANDLE_VALUE;
	receipt.rawEvidence.timingClosed = capture.closed;
	receipt.rawEvidence.timingWriteSucceeded = capture.writeSucceeded;
	receipt.rawEvidence.timingTruncated = capture.truncated;
	receipt.rawEvidence.timingComplete = capture.complete;
	receipt.rawEvidence.timingSessionCount = capture.sessionCount;
	receipt.rawEvidence.timingFrameSamples = capture.frameSamples;
	receipt.rawEvidence.timingFirstFrame = capture.firstFrame;
	receipt.rawEvidence.timingLastFrame = capture.lastFrame;
	if (!capture.complete || capture.sessionCount != 1 ||
		(retainedHandle != 0 && capture.nativeHandle == INVALID_HANDLE_VALUE))
	{
		if (capture.nativeHandle != INVALID_HANDLE_VALUE) CloseHandle(capture.nativeHandle);
		return false;
	}
	// The host may request the exact file or its directory; it cannot nominate
	// a different CSV. Resolve both paths without searching directory contents.
	char expected[MAX_PATH], actual[MAX_PATH];
	const DWORD expectedLength = GetFullPathNameA(receipt.rawEvidence.timingPath.c_str(),
		sizeof(expected), expected, 0);
	const DWORD actualLength = GetFullPathNameA(capture.path.c_str(),
		sizeof(actual), actual, 0);
	if (expectedLength == 0 || expectedLength >= sizeof(expected) ||
		actualLength == 0 || actualLength >= sizeof(actual))
	{
		if (capture.nativeHandle != INVALID_HANDLE_VALUE) CloseHandle(capture.nativeHandle);
		return false;
	}
	const DWORD attributes = GetFileAttributesA(expected);
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		if (capture.nativeHandle != INVALID_HANDLE_VALUE) CloseHandle(capture.nativeHandle);
		return false;
	}
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
		{
			if (capture.nativeHandle != INVALID_HANDLE_VALUE) CloseHandle(capture.nativeHandle);
			return false;
		}
	}
	else if (_stricmp(expectedPath.c_str(), actualPath.c_str()) != 0)
	{
		if (capture.nativeHandle != INVALID_HANDLE_VALUE) CloseHandle(capture.nativeHandle);
		return false;
	}
	receipt.rawEvidence.timingPath = actualPath;
	if (retainedHandle != 0) *retainedHandle = capture.nativeHandle;
	else if (capture.nativeHandle != INVALID_HANDLE_VALUE) CloseHandle(capture.nativeHandle);
	return true;
}

static const char *performanceReceiptRawProducer(
	const rts::performance::PerformanceReceipt &receipt)
{
	return receipt.schemaVersion == 6 ?
		"game-executable-performance-receipt-v6" :
		"game-executable-performance-receipt-v5";
}

bool writePerformanceReceiptRawDiagnostic(
	const rts::performance::PerformanceReceipt &receipt, HANDLE *retainedHandle = 0)
{
	if (retainedHandle != 0) *retainedHandle = INVALID_HANDLE_VALUE;
	if (receipt.rawEvidence.rawLogPath.empty()) return false;
	FILE *file = _fsopen(receipt.rawEvidence.rawLogPath.c_str(), "w+x", _SH_DENYWR);
	if (file == 0) return false;
	fprintf(file, "producer=%s\n", performanceReceiptRawProducer(receipt));
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
	bool success = ferror(file) == 0 && fflush(file) == 0 &&
		_commit(_fileno(file)) == 0;
	HANDLE transitional = INVALID_HANDLE_VALUE;
	if (success && retainedHandle != 0)
	{
		const intptr_t native = _get_osfhandle(_fileno(file));
		if (native != -1)
			transitional = ReOpenFile(reinterpret_cast<HANDLE>(native), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
		success = transitional != INVALID_HANDLE_VALUE &&
			SetHandleInformation(transitional, HANDLE_FLAG_INHERIT, 0) != FALSE;
	}
	if (fclose(file) != 0) success = false;
	HANDLE retained = INVALID_HANDLE_VALUE;
	if (success && retainedHandle != 0)
	{
		retained = ReOpenFile(transitional, GENERIC_READ, FILE_SHARE_READ,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
		success = retained != INVALID_HANDLE_VALUE &&
			SetHandleInformation(retained, HANDLE_FLAG_INHERIT, 0) != FALSE;
	}
	if (transitional != INVALID_HANDLE_VALUE && CloseHandle(transitional) == FALSE)
		success = false;
	if (!success && retained != INVALID_HANDLE_VALUE) CloseHandle(retained);
	if (success && retainedHandle != 0) *retainedHandle = retained;
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
#if defined(_WIN64)
	class PerformanceReceiptOwnerBorrow
	{
	public:
		PerformanceReceiptOwnerBorrow(GameLogic *owner, PerformanceReceiptRuntime *runtime)
			: m_owner(0), m_runtime(runtime)
		{
			if (m_runtime == 0) return;
			if (owner != 0 && owner->attachPerformanceReceiptRuntime(m_runtime))
				m_owner = owner;
			else
				m_runtime->invalidate("replay owner could not borrow the receipt runtime");
		}

		~PerformanceReceiptOwnerBorrow()
		{
			if (m_owner == 0) return;
			// The replay call cannot outlive its GameLogic owner. Do not
			// dereference a replaced owner or leave a borrowed stack pointer.
			if (TheGameLogic != m_owner ||
				!m_owner->detachPerformanceReceiptRuntime(m_runtime))
			{
				RELEASE_CRASH("replay receipt runtime owner release failed");
				abort();
			}
		}

	private:
		GameLogic *m_owner;
		PerformanceReceiptRuntime *m_runtime;
		PerformanceReceiptOwnerBorrow(const PerformanceReceiptOwnerBorrow &);
		PerformanceReceiptOwnerBorrow &operator=(const PerformanceReceiptOwnerBorrow &);
	};
#endif

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
		  , m_performanceReceiptOwner(TheGameLogic, m_performanceReceipt)
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
	// Member release follows real job teardown, including the never-started
	// destructor return, and precedes the outer stack runtime's destruction.
	PerformanceReceiptOwnerBorrow m_performanceReceiptOwner;
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
// BEGIN RTS_NATIVE_RECEIPT_WIDE_INPUTS_PRIVATE
#include <new>
static const wchar_t *const kNativeReceiptSelectionKeys[] = {
	L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH",
	L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH",
	L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256"
};
static const wchar_t *const kNativeReceiptPathKeys[] = {
	L"RTS_PERFORMANCE_RECEIPT_DIR",
	L"RTS_PERFORMANCE_RAW_LOG_PATH",
	L"RTS_PERFORMANCE_TIMING_PATH",
	L"RTS_FRAME_TIMING_DIR"
};
static const wchar_t kNativeReceiptBaselineMode[] = L"phase-baseline-binding";
static const wchar_t kNativeReceiptThroughputMode[] = L"throughput-binding";
static const wchar_t kNativeReceiptSerialMode[] = L"serial-oracle";

struct NativeReceiptWideValue
{
	NativeReceiptWideValue() : present(false), truncated(false), error(ERROR_ENVVAR_NOT_FOUND) {}
	std::wstring value;
	bool present;
	bool truncated;
	DWORD error;
};

struct NativeReceiptWideInputSnapshot
{
	NativeReceiptWideInputSnapshot() : requested(false), captured(false) {}
	NativeReceiptWideValue selection[3];
	NativeReceiptWideValue paths[4];
	NativeReceiptWideValue referenceMode;
	bool requested;
	bool captured;
};

// Intent detection and admission must share one original-W observation. A
// second GetEnvironmentVariableW call is not a snapshot: a same-length
// replacement between those calls can otherwise change the authority that is
// admitted. The one-shot heap object also lets the public shape guard hand its
// exact observation to Runtime::begin without widening the public API.
static NativeReceiptWideInputSnapshot *g_nativeReceiptWideInputSnapshot = 0;
enum NativeReceiptWideInputSnapshotState
{
	NATIVE_RECEIPT_WIDE_SNAPSHOT_EMPTY,
	NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURED,
	NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURE_FAILED
};
static NativeReceiptWideInputSnapshotState g_nativeReceiptWideInputSnapshotState =
	NATIVE_RECEIPT_WIDE_SNAPSHOT_EMPTY;

static bool captureNativeReceiptWideValue(const wchar_t *name,
	NativeReceiptWideValue &captured)
{
	captured = NativeReceiptWideValue();
	if (name == 0) return false;
	static const DWORD capacity = 32768;
	wchar_t value[capacity] = {};
	SetLastError(ERROR_SUCCESS);
	const DWORD count = GetEnvironmentVariableW(name, value, capacity);
	captured.error = GetLastError();
	if (count == 0)
	{
		captured.present = captured.error != ERROR_ENVVAR_NOT_FOUND;
		return captured.error == ERROR_SUCCESS ||
			captured.error == ERROR_ENVVAR_NOT_FOUND;
	}
	captured.present = true;
	captured.truncated = count >= capacity;
	const DWORD retained = count < capacity ? count : capacity - 1;
	try { captured.value.assign(value, retained); }
	catch (...) { captured.value.clear(); return false; }
	return true;
}

static bool captureNativeReceiptWideInputSnapshot(
	NativeReceiptWideInputSnapshot &snapshot)
{
	try
	{
		snapshot = NativeReceiptWideInputSnapshot();
		const unsigned selectionCount = sizeof(kNativeReceiptSelectionKeys) /
			sizeof(kNativeReceiptSelectionKeys[0]);
		for (unsigned index = 0; index != selectionCount; ++index)
			if (!captureNativeReceiptWideValue(kNativeReceiptSelectionKeys[index],
				snapshot.selection[index])) return false;
		const unsigned pathCount = sizeof(kNativeReceiptPathKeys) /
			sizeof(kNativeReceiptPathKeys[0]);
		for (unsigned index = 0; index != pathCount; ++index)
			if (!captureNativeReceiptWideValue(kNativeReceiptPathKeys[index],
				snapshot.paths[index])) return false;
		if (!captureNativeReceiptWideValue(L"RTS_PERFORMANCE_REFERENCE_MODE",
			snapshot.referenceMode)) return false;
		bool requested = false;
		for (unsigned index = 0; index != selectionCount; ++index)
			if (snapshot.selection[index].present) requested = true;
		const auto exactMode = [&snapshot](const wchar_t *expected, size_t count) {
			return !snapshot.referenceMode.truncated &&
				snapshot.referenceMode.value.size() == count &&
				snapshot.referenceMode.value.compare(0, count, expected, count) == 0;
		};
		const bool ordinaryMode = exactMode(kNativeReceiptThroughputMode,
			sizeof(kNativeReceiptThroughputMode) / sizeof(kNativeReceiptThroughputMode[0]) - 1) ||
			exactMode(kNativeReceiptSerialMode,
				sizeof(kNativeReceiptSerialMode) / sizeof(kNativeReceiptSerialMode[0]) - 1);
		if (snapshot.referenceMode.present && !ordinaryMode) requested = true;
		snapshot.requested = requested;
		snapshot.captured = true;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static bool publishNativeReceiptWideInputSnapshot()
{
	delete g_nativeReceiptWideInputSnapshot;
	g_nativeReceiptWideInputSnapshot = 0;
	g_nativeReceiptWideInputSnapshotState = NATIVE_RECEIPT_WIDE_SNAPSHOT_EMPTY;
	NativeReceiptWideInputSnapshot *snapshot =
		new (std::nothrow) NativeReceiptWideInputSnapshot;
	if (snapshot == 0)
	{
		g_nativeReceiptWideInputSnapshotState =
			NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURE_FAILED;
		return true;
	}
	const bool captured = captureNativeReceiptWideInputSnapshot(*snapshot);
	if (!captured)
	{
		delete snapshot;
		g_nativeReceiptWideInputSnapshotState =
			NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURE_FAILED;
		return true;
	}
	g_nativeReceiptWideInputSnapshot = snapshot;
	g_nativeReceiptWideInputSnapshotState = NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURED;
	return snapshot->requested;
}

static NativeReceiptWideInputSnapshot *takeNativeReceiptWideInputSnapshot(
	bool &captureFailed)
{
	NativeReceiptWideInputSnapshot *snapshot = g_nativeReceiptWideInputSnapshot;
	captureFailed = g_nativeReceiptWideInputSnapshotState ==
		NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURE_FAILED;
	g_nativeReceiptWideInputSnapshot = 0;
	g_nativeReceiptWideInputSnapshotState = NATIVE_RECEIPT_WIDE_SNAPSHOT_EMPTY;
	return snapshot;
}

static bool preflightNativeReceiptReferenceIntent(std::string *reason)
{
	if (reason != 0) reason->clear();
	if (g_nativeReceiptWideInputSnapshotState == NATIVE_RECEIPT_WIDE_SNAPSHOT_EMPTY)
		publishNativeReceiptWideInputSnapshot();
	if (g_nativeReceiptWideInputSnapshotState ==
		NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURE_FAILED ||
		g_nativeReceiptWideInputSnapshot == 0)
	{
		if (reason != 0) *reason = "native trace original-W environment could not be captured";
		return false;
	}
	const NativeReceiptWideInputSnapshot &snapshot =
		*g_nativeReceiptWideInputSnapshot;
	if (!snapshot.requested) return true;
	const bool throughput = snapshot.referenceMode.present &&
		!snapshot.referenceMode.truncated &&
		snapshot.referenceMode.error == ERROR_SUCCESS &&
		snapshot.referenceMode.value == kNativeReceiptThroughputMode;
	const bool baseline = snapshot.referenceMode.present &&
		!snapshot.referenceMode.truncated &&
		snapshot.referenceMode.error == ERROR_SUCCESS &&
		snapshot.referenceMode.value == kNativeReceiptBaselineMode;
	const bool implicitThroughput = !snapshot.referenceMode.present &&
		snapshot.referenceMode.error == ERROR_ENVVAR_NOT_FOUND;
	const bool selectionShape = (throughput || implicitThroughput) ?
		(snapshot.selection[0].present && !snapshot.selection[1].present &&
			!snapshot.selection[2].present) :
		(baseline && !snapshot.selection[0].present &&
			snapshot.selection[1].present && snapshot.selection[2].present);
	if (!selectionShape)
	{
		if (reason != 0) *reason =
			"native trace reference mode or selection shape is unsupported";
		return false;
	}
	return true;
}

struct NativeReceiptWideInputSnapshotRelease
{
	NativeReceiptWideInputSnapshotRelease(NativeReceiptWideInputSnapshot *value,
		NativeReceiptWideInputSnapshot *local) : value(value), local(local) {}
	~NativeReceiptWideInputSnapshotRelease()
	{
		if (value != 0 && value != local) delete value;
	}
	NativeReceiptWideInputSnapshot *value;
	NativeReceiptWideInputSnapshot *local;
};

static bool captureNativeReceiptWideInputs(
	DWORD *selectionLengths, bool *selectionPresent,
	wchar_t *referenceMode, DWORD *modeLength, DWORD *modeError)
{
	if (selectionLengths == 0 || selectionPresent == 0 || referenceMode == 0 ||
		modeLength == 0 || modeError == 0) return false;
	NativeReceiptWideInputSnapshot snapshot;
	if (!captureNativeReceiptWideInputSnapshot(snapshot)) return false;
	const unsigned selectionCount = sizeof(kNativeReceiptSelectionKeys) /
		sizeof(kNativeReceiptSelectionKeys[0]);
	for (unsigned index = 0; index != selectionCount; ++index)
	{
		selectionLengths[index] = static_cast<DWORD>(snapshot.selection[index].value.size());
		selectionPresent[index] = snapshot.selection[index].present;
	}
	const DWORD modeCapacity = sizeof(kNativeReceiptBaselineMode) /
		sizeof(kNativeReceiptBaselineMode[0]);
	for (DWORD index = 0; index != modeCapacity; ++index)
		referenceMode[index] = 0;
	*modeLength = static_cast<DWORD>(snapshot.referenceMode.value.size());
	*modeError = snapshot.referenceMode.error;
	const DWORD copied = *modeLength < modeCapacity - 1 ? *modeLength : modeCapacity - 1;
	for (DWORD index = 0; index != copied; ++index)
		referenceMode[index] = snapshot.referenceMode.value[index];
	return snapshot.requested;
}

static bool readNativeReceiptTracePathInputs(
	const rts::performance::PerformanceReceipt &receipt,
	rts::performance::PerformanceReceiptTraceFiles &paths,
	rts::performance::KernelPerformanceDigest &expectedSourceReceiptDigest,
	std::string *reason, bool *explicitTraceRequested = 0)
{
	paths = rts::performance::PerformanceReceiptTraceFiles();
	expectedSourceReceiptDigest = rts::performance::KernelPerformanceDigest();
	if (reason != 0) reason->clear();
	// A rejected ANSI receipt parse must not erase an original-W request,
	// including a present empty value or malformed UTF-16 path. Consume the
	// exact one-shot snapshot when intent was detected by the caller; direct
	// helper users capture their own immutable observation here.
	NativeReceiptWideInputSnapshot localSnapshot;
	bool publishedCaptureFailed = false;
	NativeReceiptWideInputSnapshot *snapshot =
		takeNativeReceiptWideInputSnapshot(publishedCaptureFailed);
	if (publishedCaptureFailed)
	{
		if (explicitTraceRequested != 0) *explicitTraceRequested = true;
		if (reason != 0) *reason = "native trace original-W environment could not be captured";
		return false;
	}
	if (snapshot == 0)
	{
		if (!captureNativeReceiptWideInputSnapshot(localSnapshot))
		{
			if (explicitTraceRequested != 0) *explicitTraceRequested = true;
			if (reason != 0) *reason = "native trace original-W environment could not be captured";
			return false;
		}
		snapshot = &localSnapshot;
	}
	NativeReceiptWideInputSnapshotRelease release(snapshot, &localSnapshot);
	const NativeReceiptWideInputSnapshot &captured = *snapshot;
	if (!captured.captured)
	{
		if (explicitTraceRequested != 0) *explicitTraceRequested = true;
		if (reason != 0) *reason = "native trace original-W environment could not be captured";
		return false;
	}
	const unsigned selectionCount = sizeof(kNativeReceiptSelectionKeys) /
		sizeof(kNativeReceiptSelectionKeys[0]);
	const bool requested = captured.requested;
	if (explicitTraceRequested != 0) *explicitTraceRequested = requested;
	const auto fail = [reason](const char *message) {
		if (reason != 0) *reason = message;
		return false;
	};
	const auto ascii = [&fail](const wchar_t *wide, size_t count, std::string &narrow) {
		narrow.clear();
		for (size_t index = 0; index != count; ++index)
		{
			if (wide[index] == 0 || wide[index] > 127)
				return fail("native trace qualification requires original-W ASCII inputs");
			narrow.push_back(static_cast<char>(wide[index]));
		}
		return true;
	};
	const auto readValue = [&fail, &ascii](const NativeReceiptWideValue &capturedValue,
		std::string &value) {
		if (!capturedValue.present || capturedValue.truncated ||
			capturedValue.error != ERROR_SUCCESS) return fail("native trace input is empty or unreadable");
		if (capturedValue.value.empty()) return fail("native trace input is empty or unreadable");
		if (capturedValue.value.size() > MAX_PATH) return fail("native trace input exceeds MAX_PATH");
		return ascii(capturedValue.value.c_str(), capturedValue.value.size(), value);
	};
	std::string selections[3];
	for (unsigned index = 0; index != selectionCount; ++index)
		if (captured.selection[index].present && !readValue(captured.selection[index],
			selections[index]))
			return false;
	if (!requested)
		return true;
	const std::wstring &referenceMode = captured.referenceMode.value;
	const DWORD modeLength = static_cast<DWORD>(referenceMode.size());
	const DWORD modeError = captured.referenceMode.error;
	const bool baselineRequested = !captured.referenceMode.truncated &&
		modeLength == sizeof(kNativeReceiptBaselineMode) /
			sizeof(kNativeReceiptBaselineMode[0]) - 1 &&
		referenceMode == kNativeReceiptBaselineMode;
	if (captured.referenceMode.truncated || (modeLength == 0 && modeError != ERROR_SUCCESS &&
		modeError != ERROR_ENVVAR_NOT_FOUND))
		return fail("native trace reference mode is unreadable or unsupported");
	std::string mode;
	if (!ascii(referenceMode.c_str(), referenceMode.size(), mode)) return false;
	const bool record = mode == "throughput-binding" ||
		(mode.empty() && modeError == ERROR_ENVVAR_NOT_FOUND);
	if ((!record && !baselineRequested) ||
		(record && receipt.kernelReference.mode != rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING) ||
		(baselineRequested && receipt.kernelReference.mode != rts::performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING))
		return fail("native trace reference mode does not match original-W input");
	if ((record && (!captured.selection[0].present || captured.selection[1].present || captured.selection[2].present)) ||
		(baselineRequested && (captured.selection[0].present || !captured.selection[1].present || !captured.selection[2].present)))
		return fail("native trace selection keys are missing or conflicting");
	const std::string *const retainedPaths[] = { &receipt.outputDirectory,
		&receipt.rawEvidence.rawLogPath, &receipt.rawEvidence.timingPath };
	const unsigned pathCount = sizeof(kNativeReceiptPathKeys) /
		sizeof(kNativeReceiptPathKeys[0]);
	for (unsigned index = 0; index != pathCount; ++index)
	{
		if (index == 3 && !captured.paths[index].present) continue;
		std::string path;
		if (!readValue(captured.paths[index], path)) return false;
		if (index < 3 && path != *retainedPaths[index])
			return fail("native receipt path does not match original-W input");
	}
	wchar_t executable[MAX_PATH] = {}, directory[MAX_PATH] = {};
	const DWORD executableLength = GetModuleFileNameW(0, executable, MAX_PATH);
	const DWORD directoryLength = GetCurrentDirectoryW(MAX_PATH, directory);
	if (executableLength == 0 || executableLength >= MAX_PATH || directoryLength == 0 || directoryLength >= MAX_PATH)
		return fail("native trace process paths are unavailable or exceed MAX_PATH");
	std::string executableAscii, directoryAscii, commandAscii;
	if (!ascii(executable, executableLength, executableAscii) || !ascii(directory, directoryLength, directoryAscii))
		return false;
	const wchar_t *const command = GetCommandLineW();
	if (command == 0) return fail("native trace command line is unavailable");
	size_t commandLength = 0;
	while (commandLength != 32768 && command[commandLength] != 0) ++commandLength;
	if (commandLength == 0 || commandLength == 32768) return fail("native trace command line is empty or oversized");
	if (!ascii(command, commandLength, commandAscii)) return false;
	if (executableAscii != receipt.executablePath || commandAscii != receipt.commandLine)
		return fail("native receipt process provenance does not match original-W input");
	if (baselineRequested)
	{
		rts::performance::KernelPerformanceDigest decoded;
		const unsigned expectedHexLength = static_cast<unsigned>(sizeof(decoded.bytes) * 2);
		if (selections[2].size() != expectedHexLength)
			return fail("native selected-source SHA256 must contain exactly 64 hexadecimal characters");
		const auto hexValue = [](char value) -> unsigned {
			if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
			if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
			if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
			return 0x100;
		};
		for (unsigned index = 0; index != sizeof(decoded.bytes); ++index)
		{
			const unsigned high = hexValue(selections[2][index * 2]);
			const unsigned low = hexValue(selections[2][index * 2 + 1]);
			if (high > 0x0F || low > 0x0F)
				return fail("native selected-source SHA256 contains a non-hexadecimal character");
			decoded.bytes[index] = static_cast<unsigned char>((high << 4) | low);
		}
		decoded.valid = true;
		paths.sourceReceiptPath = selections[1];
		expectedSourceReceiptDigest = decoded;
	}
	else
		paths.tracePath = selections[0];
	return true;
}

static bool deriveNativeReceiptTraceCapacities(unsigned workers,
	rts::JobMetricCounter &attempts, rts::JobMetricCounter &ranges) noexcept
{
	using rts::JobMetricCounter;
	attempts = 0;
	ranges = 0;
	if (workers == 0 || PATHFIND_QUEUE_LEN <= 1) return false;
	const JobMetricCounter maximum = ~static_cast<JobMetricCounter>(0);
	const JobMetricCounter w = workers;
	if (w > maximum / 4 || w > maximum - 11) return false;
	const JobMetricCounter fourWorkers = w * 4;
	const JobMetricCounter queueLimit = static_cast<JobMetricCounter>(PATHFIND_QUEUE_LEN) - 1;
	const JobMetricCounter pathRanges = w < queueLimit ? w : queueLimit;
	const JobMetricCounter simultaneousRanges[] = {
		fourWorkers < 2048 ? fourWorkers : 2048,
		64,
		fourWorkers < 512 ? fourWorkers : 512,
		256,
		w < 256 ? w : 256,
		16,
		pathRanges,
		fourWorkers < 1024 ? fourWorkers : 1024,
		64
	};
	JobMetricCounter activeRanges = 0;
	for (unsigned index = 0; index != sizeof(simultaneousRanges) / sizeof(simultaneousRanges[0]); ++index)
		if (simultaneousRanges[index] > activeRanges) activeRanges = simultaneousRanges[index];
	// Conditional on actual native owner import/reap: retain the two owning
	// path slots and up to W genuinely worker-closing groups, without a drain.
	const JobMetricCounter closingRangesPerWorker = pathRanges > 16 ? pathRanges : 16;
	if (w > maximum / closingRangesPerWorker || activeRanges > maximum - 16 ||
		activeRanges + 16 > maximum - pathRanges) return false;
	const JobMetricCounter closingRanges = w * closingRangesPerWorker;
	const JobMetricCounter ownerRanges = activeRanges + 16 + pathRanges;
	if (ownerRanges > maximum - closingRanges) return false;
	const JobMetricCounter attemptCapacity = 11 + w;
	const JobMetricCounter rangeCapacity = ownerRanges + closingRanges;
	const JobMetricCounter indexLimit = static_cast<JobMetricCounter>(~0u);
	const JobMetricCounter allocationCountLimit = static_cast<JobMetricCounter>(~static_cast<size_t>(0));
	if (attemptCapacity > indexLimit || rangeCapacity > indexLimit ||
		attemptCapacity > allocationCountLimit || rangeCapacity > allocationCountLimit) return false;
	attempts = attemptCapacity;
	ranges = rangeCapacity;
	return true;
}

static rts::performance::KernelPerformanceDigest makeNativeReceiptSourcePolicy(
	const rts::performance::PerformanceReceipt &receipt,
	const rts::JobSystemConfig &config,
	rts::SimulationExecutionMode simulation,
	rts::PipelineExecutionMode pipeline,
	unsigned terminalFrame,
	const rts::performance::KernelPerformanceTraceOptions &trace)
{
	using namespace rts::performance;
	KernelPerformanceDigest invalid;
	rts::JobMetricCounter attempts = 0, ranges = 0;
	const bool observedOnly = receipt.workloadQualification == "observed-only";
	if ((receipt.title != "Generals" && receipt.title != "ZeroHour") ||
		receipt.fixtureKind != "replay" || receipt.fixtureId.empty() ||
		!receipt.fixtureIdentityObserved || receipt.fixtureObservationFailed || !receipt.seedKnown ||
		terminalFrame == 0 || (!observedOnly && receipt.workloadQualification != "minimum-qualified") ||
		(observedOnly ? (receipt.requestedPlayerCount != 0 || receipt.requestedMinimumUnitCount != 0) :
		 (receipt.requestedPlayerCount == 0 || receipt.requestedMinimumUnitCount == 0)) ||
		(simulation != rts::SIMULATION_EXECUTION_SERIAL && simulation != rts::SIMULATION_EXECUTION_PARALLEL &&
		 simulation != rts::SIMULATION_EXECUTION_SHADOW) ||
		(pipeline != rts::PIPELINE_EXECUTION_PARALLEL && pipeline != rts::PIPELINE_EXECUTION_SERIAL) ||
		(config.workerPolicy != rts::JOB_WORKER_POLICY_AUTO && config.workerPolicy != rts::JOB_WORKER_POLICY_ALL) ||
		config.queueCapacity == 0 || config.scratchBytesPerWorker == 0 ||
		(trace.mode != KERNEL_TRACE_RECORD && trace.mode != KERNEL_TRACE_CONSUME) ||
		trace.limits.maximumBytes == 0 || trace.limits.maximumRecords == 0 ||
		trace.limits.maximumLogicalEvents == 0 || trace.limits.maximumAttempts == 0 ||
		trace.limits.maximumRanges == 0 ||
		!deriveNativeReceiptTraceCapacities(config.workerCount, attempts, ranges) ||
		trace.residentAttemptCapacity != attempts || trace.residentRangeCapacity != ranges)
		return invalid;
	const char policy[] = "paired-source-admissions-v1";
	const char *const text[] = { policy, receipt.title.c_str(), receipt.fixtureKind.c_str(),
		receipt.workloadQualification.c_str(), receipt.fixtureId.c_str() };
	const size_t length[] = { sizeof(policy) - 1, receipt.title.size(), receipt.fixtureKind.size(),
		receipt.workloadQualification.size(), receipt.fixtureId.size() };
	for (unsigned field = 0; field != sizeof(text) / sizeof(text[0]); ++field)
	{
		if (length[field] > static_cast<size_t>(~0u)) return invalid;
		for (size_t index = 0; index != length[field]; ++index)
		{
			const unsigned char value = static_cast<unsigned char>(text[field][index]);
			if (value == 0 || value > 127) return invalid;
		}
	}
	// These are canonical input facts, not a source-selection or execution
	// grant. Native held-file/policy preflight remains a separate prerequisite.
	KernelPerformanceCanonicalWriter writer;
	if (!writer.begin(0x5004) || !writer.u32(1, 1)) return invalid;
	for (unsigned field = 0; field != sizeof(text) / sizeof(text[0]); ++field)
	{
		const unsigned tag = field + 2;
		if (!writer.sequence(tag, static_cast<unsigned>(length[field]))) return invalid;
		for (size_t index = 0; index != length[field]; ++index)
			if (!writer.u32(tag, static_cast<unsigned char>(text[field][index]))) return invalid;
	}
	if (!writer.u32(7, receipt.seed) || !writer.u32(8, receipt.requestedPlayerCount) ||
		!writer.u32(9, receipt.requestedMinimumUnitCount) ||
		!writer.u32(10, static_cast<unsigned>(simulation)) ||
		!writer.u32(11, static_cast<unsigned>(pipeline)) ||
		!writer.u32(12, config.workerCount) || !writer.u32(13, static_cast<unsigned>(config.workerPolicy)) ||
		!writer.u32(14, config.queueCapacity) || !writer.u32(15, config.scratchBytesPerWorker) ||
		!writer.boolean(16, config.pinWorkers) || !writer.u32(17, 1) || !writer.u32(18, terminalFrame) ||
		!writer.u64(19, terminalFrame) || !writer.u64(20, 1) || !writer.u32(21, 1) ||
		!writer.u64(22, trace.limits.maximumBytes) || !writer.u64(23, trace.limits.maximumRecords) ||
		!writer.u64(24, trace.limits.maximumLogicalEvents) || !writer.u64(25, trace.limits.maximumAttempts) ||
		!writer.u64(26, trace.limits.maximumRanges) ||
		!writer.u64(27, attempts) || !writer.u64(28, ranges)) return invalid;
	return writer.finish();
}

static bool decodeNativeReceiptDigest(const std::string &text,
	rts::performance::KernelPerformanceDigest &digest)
{
	digest = rts::performance::KernelPerformanceDigest();
	const unsigned expectedLength = static_cast<unsigned>(sizeof(digest.bytes) * 2);
	if (text.size() != expectedLength) return false;
	const auto hexValue = [](char value) -> unsigned {
		if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
		if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
		if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
		return 0x100;
	};
	for (unsigned index = 0; index != sizeof(digest.bytes); ++index)
	{
		const unsigned high = hexValue(text[index * 2]);
		const unsigned low = hexValue(text[index * 2 + 1]);
		if (high > 0x0F || low > 0x0F) return false;
		digest.bytes[index] = static_cast<unsigned char>((high << 4) | low);
	}
	digest.valid = true;
	return true;
}

PerformanceReceiptRuntime::PerformanceReceiptRuntime()
	: m_active(false), m_traceRequested(false), m_tracePending(false), m_traceActive(false),
	  m_traceTerminalFrame(0), m_tracePaths(), m_sourceReceiptDigest(),
	  m_nativeTraceFiles(0), m_phaseSampleOrdinal(0), m_attemptOrdinal(0),
	  m_phaseOwnerIdentity(0), m_phaseGeneration(0), m_phaseEntryFrame(0),
	  m_phaseAuthorityFrame(0), m_phaseActive(rts::performance::KERNEL_PHASE_COUNT),
	  m_phaseNext(0), m_pendingOwnerObservation(false), m_pendingEntryFrame(0),
	  m_pendingFrame(0), m_phaseObservationFailed(false)
{
}

PerformanceReceiptRuntime::~PerformanceReceiptRuntime()
{
	releaseNativeTrace();
}

void PerformanceReceiptRuntime::releaseNativeTrace()
{
	delete m_nativeTraceFiles;
	m_nativeTraceFiles = 0;
	m_tracePending = false;
	m_traceActive = false;
	m_traceTerminalFrame = 0;
}

bool PerformanceReceiptRuntime::explicitTraceRequestedFromEnvironment()
{
	return publishNativeReceiptWideInputSnapshot();
}

bool PerformanceReceiptRuntime::begin(const char *fixtureKind, const char *replayPath)
{
#if defined(RTS_GENERALS)
	const char *title = "Generals";
#elif defined(RTS_ZEROHOUR)
	const char *title = "ZeroHour";
#else
	const char *title = "Unknown";
#endif
	std::string reason;
	if (!preflightNativeReceiptReferenceIntent(&reason))
	{
		// A rejected one-shot snapshot is still explicit native receipt intent.
		// Retain that fact for the caller fence, then consume the snapshot so a
		// later independent replay cannot inherit this rejected selection.
		m_traceRequested = true;
		bool captureFailed = false;
		delete takeNativeReceiptWideInputSnapshot(captureFailed);
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", reason.c_str());
		fflush(stdout);
		return false;
	}
	if (!m_lifecycle.begin()) return false;
	const bool receiptBegan = rts::performance::BeginPerformanceReceipt(
		m_receipt, title, replayPath, 0, &reason);
	rts::performance::PerformanceReceiptTraceFiles tracePaths;
	rts::performance::KernelPerformanceDigest sourceReceiptDigest;
	std::string traceReason;
	const bool traceInputsValid = readNativeReceiptTracePathInputs(
		m_receipt, tracePaths, sourceReceiptDigest, &traceReason, &m_traceRequested);
	if (!receiptBegan || m_receipt.fixtureKind != fixtureKind)
	{
		if (reason.empty()) reason = "receipt fixture kind does not match its real owner";
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", reason.c_str());
		fflush(stdout);
		return false;
	}
	if (!traceInputsValid)
	{
		if (traceReason.empty()) traceReason = "native trace input admission failed";
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", traceReason.c_str());
		fflush(stdout);
		return false;
	}
	m_receipt.simulationMode = simulationModeName(rts::GetSimulationExecutionMode());
	if (!m_traceRequested)
	{
		if (!rts::performance::KernelPerformanceLedger::instance().beginRun(true) ||
			!rts::performance::KernelPerformanceReferenceLedger::instance().beginRun(
				m_receipt.kernelReference.mode))
		{
			printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=native performance ledgers could not begin\n");
			fflush(stdout);
			return false;
		}
		m_active = true;
		return true;
	}
	// A trace-backed receipt always uses the V6 wire identity. The mode remains
	// the Begin-owned reference role; only the trace transport is armed here.
	m_receipt.schemaVersion = 6;
	m_receipt.producer = "game-executable-stage5-performance-report-v6";
	m_receipt.producerVersion = "6";
	m_tracePaths = tracePaths;
	m_sourceReceiptDigest = sourceReceiptDigest;
	const bool baseline = m_receipt.kernelReference.mode ==
		rts::performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if (m_receipt.kernelReference.mode !=
		rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING && !baseline)
	{
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=native trace reference role is not traceable\n");
		fflush(stdout);
		return false;
	}
	const rts::JobMetricCounter maximumTraceBytes =
		static_cast<rts::JobMetricCounter>(16ULL * 1024ULL * 1024ULL * 1024ULL);
	m_nativeTraceFiles = new (std::nothrow) native_receipt_files::NativeReceiptTraceFiles;
	if (m_nativeTraceFiles == 0 ||
		!m_nativeTraceFiles->holdOutputDestination(m_receipt.outputDirectory, &traceReason))
	{
		releaseNativeTrace();
		if (traceReason.empty()) traceReason = "native output destination could not be held";
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", traceReason.c_str());
		fflush(stdout);
		return false;
	}
	if (baseline)
	{
		if (!m_nativeTraceFiles->openSource(m_tracePaths.sourceReceiptPath,
				m_sourceReceiptDigest, maximumTraceBytes, &traceReason))
		{
			releaseNativeTrace();
			if (traceReason.empty()) traceReason = "native source receipt could not be held";
			printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", traceReason.c_str());
			fflush(stdout);
			return false;
		}
		m_sourceReceiptDigest = m_nativeTraceFiles->sourceReceiptDigest();
	}
	rts::performance::KernelPerformanceTimingRunOptions timingOptions;
	timingOptions.enabled = true;
	timingOptions.role = baseline ?
		rts::performance::KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE :
		rts::performance::KERNEL_PERFORMANCE_PIPELINE;
	if (!rts::performance::KernelPerformanceLedger::instance().beginRun(timingOptions))
	{
		releaseNativeTrace();
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=native timing ledger could not begin\n");
		fflush(stdout);
		return false;
	}
	m_tracePending = true;
	m_active = true;
	return true;
}

void PerformanceReceiptRuntime::invalidate(const char *reason)
{
	if (active() && m_failure.empty()) m_failure = reason;
}

rts::performance::KernelPerformanceAttempt PerformanceReceiptRuntime::beginAttempt(
	unsigned workKind, unsigned subtype) noexcept
{
	using namespace rts::performance;
	// The native owner supplies only a family. Identity comes from the real
	// borrowed phase observation and current world, including intake retarget.
	if (!GameThreadOwnership::IsCurrentThread() || !active() ||
		m_lifecycle.terminalResultKnown() || !m_failure.empty() ||
		m_phaseObservationFailed.load(std::memory_order_acquire) ||
		TheGameLogic == 0 || TheGameLogic != m_phaseOwnerIdentity ||
		!TheGameLogic->isInGameLogicUpdate() || m_phaseGeneration == 0 ||
		m_phaseActive >= KERNEL_PHASE_COUNT)
		return KernelPerformanceAttempt();
	if (m_tracePending)
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return KernelPerformanceAttempt();
	}
	if (m_attemptOrdinal == ~static_cast<rts::JobMetricCounter>(0))
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return KernelPerformanceAttempt();
	}
	KernelPerformanceAttemptIdentity identity;
	identity.workKind = workKind;
	identity.subtype = subtype;
	identity.sampleOrdinal = m_phaseSampleOrdinal;
	identity.attemptOrdinal = ++m_attemptOrdinal;
	identity.phase = static_cast<KernelPerformancePhase>(m_phaseActive);
	identity.ownerFrame = TheGameLogic->getFrame();
	return KernelPerformanceReferenceLedger::instance().beginAttempt(identity);
}

static rts::performance::KernelPerformanceSchedulerBoundary
capturePerformancePhaseSchedulerBoundary()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	const rts::JobSystemMetrics metrics = jobs.metrics();
	rts::performance::KernelPerformanceSchedulerBoundary actual;
	actual.submittedJobs = metrics.submittedJobCount;
	actual.executedJobs = metrics.executedJobCount;
	actual.ownerHelpJobs = metrics.ownerHelpCount;
	actual.outstandingJobs = jobs.outstandingJobCount();
	actual.pendingJobs = jobs.pendingOwnerCompletionCount();
	return actual;
}

static bool observePerformanceReferenceWindow(
	rts::performance::KernelPerformanceWindowBoundaryKind kind,
	rts::JobMetricCounter sampleOrdinal,
	rts::performance::KernelPerformancePhase phase,
	unsigned ownerFrameAtEntry, unsigned authorityFrame, unsigned actualOwnerFrame) noexcept
{
	using namespace rts::performance;
	KernelPerformanceWindowBoundary observation;
	observation.kind = kind;
	observation.sampleOrdinal = sampleOrdinal;
	observation.phase = phase;
	observation.ownerFrameAtEntry = ownerFrameAtEntry;
	observation.authorityFrame = authorityFrame;
	observation.actualOwnerFrame = actualOwnerFrame;
	KernelPerformanceReferenceLedger &reference = KernelPerformanceReferenceLedger::instance();
	if (reference.observeWindowBoundary(observation)) return true;
	// The caller has already authenticated the native owner. An untraced
	// ordinary ledger declines the hook without disabling its active mode;
	// requested-trace failures are sticky and disable that mode. Baseline can
	// never treat a declined observation as ordinary untraced execution.
	const KernelPerformanceReferenceMode mode = reference.runMode();
	return (mode == KERNEL_REFERENCE_THROUGHPUT_BINDING || mode == KERNEL_REFERENCE_SERIAL_ORACLE) &&
		reference.mode() == mode;
}

void PerformanceReceiptRuntime::observePhaseBoundary(
	rts::LiveSimulationPhaseObservationBoundary boundary,
	rts::SimulationPhaseId phaseId, unsigned generation,
	unsigned authorityFrame, unsigned actualOwnerFrame,
	const void *ownerIdentity) noexcept
{
	using namespace rts::performance;
	// A foreign observation changes only this atomic failure latch, never the
	// owner's token, ledger state, world data, or gameplay execution policy.
	if (!GameThreadOwnership::IsCurrentThread())
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return;
	}
	if (!active() || m_lifecycle.terminalResultKnown()) return;
	if (m_tracePending)
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return;
	}
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	// Ordinary roles retain owner identity and completion ordering without
	// opening timing extents, reading clocks, or changing dispatch policy.
	const bool timed = ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	try
	{
		if (boundary == rts::LIVE_SIMULATION_PHASE_OBSERVE_FRAME_ABORT)
		{
			// In particular, nested rejection cannot replace or discard the
			// outer token. That real outer frame may still finish accounting.
			m_phaseObservationFailed.store(true, std::memory_order_release);
			return;
		}
		if (boundary == rts::LIVE_SIMULATION_PHASE_OBSERVE_FRAME_BEGIN)
		{
			if (m_phaseGeneration != 0 || m_phaseFrame.valid() || generation == 0 ||
				ownerIdentity == 0 || authorityFrame != actualOwnerFrame ||
				(m_phaseOwnerIdentity != 0 && m_phaseOwnerIdentity != ownerIdentity) ||
				m_phaseSampleOrdinal == ~static_cast<rts::JobMetricCounter>(0))
			{
				m_phaseObservationFailed.store(true, std::memory_order_release);
				return;
			}
			// Keep the pending identity intact on failure. Independent timing
			// may still close, but a skipped workload sample cannot qualify.
			if (m_pendingOwnerObservation)
				m_phaseObservationFailed.store(true, std::memory_order_release);
			if (timed)
			{
				m_phaseFrame = ledger.beginFrame(m_phaseSampleOrdinal + 1,
					authorityFrame, capturePerformancePhaseSchedulerBoundary());
				if (!m_phaseFrame.valid())
				{
					m_phaseObservationFailed.store(true, std::memory_order_release);
					return;
				}
			}
			++m_phaseSampleOrdinal;
			m_phaseOwnerIdentity = ownerIdentity;
			m_phaseGeneration = generation;
			m_phaseEntryFrame = authorityFrame;
			m_phaseAuthorityFrame = authorityFrame;
			m_phaseActive = KERNEL_PHASE_COUNT;
			m_phaseNext = 0;
			if (!observePerformanceReferenceWindow(KERNEL_WINDOW_BEGIN,
				m_phaseSampleOrdinal, KERNEL_PHASE_COUNT, m_phaseEntryFrame,
				authorityFrame, actualOwnerFrame))
				m_phaseObservationFailed.store(true, std::memory_order_release);
			return;
		}
		if (m_phaseGeneration == 0 || generation != m_phaseGeneration ||
			ownerIdentity != m_phaseOwnerIdentity || (timed && !m_phaseFrame.valid()))
		{
			m_phaseObservationFailed.store(true, std::memory_order_release);
			return;
		}
		if (boundary == rts::LIVE_SIMULATION_PHASE_OBSERVE_FRAME_END)
		{
			// FRAME_END retains entry authority, while the last real phase
			// can have adopted a new world frame during owner intake.
			if (authorityFrame != m_phaseEntryFrame ||
				m_phaseActive != KERNEL_PHASE_COUNT || m_phaseNext != KERNEL_PHASE_COUNT ||
				m_phaseAuthorityFrame == ~0u ||
				(actualOwnerFrame == 0 ? m_phaseAuthorityFrame != 0 :
				 actualOwnerFrame != m_phaseAuthorityFrame + 1))
			{
				m_phaseObservationFailed.store(true, std::memory_order_release);
				return;
			}
			if (actualOwnerFrame != 0 && !m_pendingOwnerObservation)
			{
				m_pendingOwnerObservation = true;
				m_pendingEntryFrame = m_phaseEntryFrame;
				m_pendingFrame = actualOwnerFrame;
			}
			// Zero is a control extent only when its real deferred-start branch
			// was observed. Reference and Diagnostics retain that provenance
			// across repeated movie-pending updates, separately from world1+.
			if (!observePerformanceReferenceWindow(actualOwnerFrame == 0 ?
				KERNEL_WINDOW_CONTROL_END : KERNEL_WINDOW_WORLD_END,
				m_phaseSampleOrdinal, KERNEL_PHASE_COUNT, m_phaseEntryFrame,
				authorityFrame, actualOwnerFrame))
				m_phaseObservationFailed.store(true, std::memory_order_release);
			if (timed)
			{
				const KernelPerformanceSchedulerBoundary actual = capturePerformancePhaseSchedulerBoundary();
				const bool ended = actualOwnerFrame == 0 ?
					ledger.endControlWindow(m_phaseFrame, actualOwnerFrame, actual) :
					ledger.endFrame(m_phaseFrame, actualOwnerFrame, actual);
				if (!ended) m_phaseObservationFailed.store(true, std::memory_order_release);
			}
			m_phaseFrame = KernelPerformanceFrame();
			m_phaseGeneration = 0;
			return;
		}
		KernelPerformancePhase phase;
		switch (phaseId)
		{
		case rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE:
			phase = KERNEL_PHASE_OWNER_INTAKE; break;
		case rts::LIVE_SIMULATION_PHASE_LEGACY_MUTABLE_ISLAND:
			phase = KERNEL_PHASE_LEGACY_MUTABLE_ISLAND; break;
		case rts::LIVE_SIMULATION_PHASE_SPATIAL:
			phase = KERNEL_PHASE_SPATIAL_WORK; break;
		case rts::LIVE_SIMULATION_PHASE_OWNER_TAIL:
			phase = KERNEL_PHASE_OWNER_TAIL; break;
		case rts::LIVE_SIMULATION_PHASE_VERIFICATION_PUBLICATION:
			phase = KERNEL_PHASE_VERIFICATION_PUBLICATION; break;
		default:
			m_phaseObservationFailed.store(true, std::memory_order_release);
			return;
		}
		if (boundary == rts::LIVE_SIMULATION_PHASE_OBSERVE_PHASE_BEGIN)
		{
			if (m_phaseActive != KERNEL_PHASE_COUNT || phase != m_phaseNext ||
				authorityFrame != actualOwnerFrame ||
				(timed && !ledger.beginPhase(m_phaseFrame, phase)))
			{
				m_phaseObservationFailed.store(true, std::memory_order_release);
				return;
			}
			m_phaseActive = phase;
			m_phaseAuthorityFrame = authorityFrame;
			if (!observePerformanceReferenceWindow(KERNEL_WINDOW_PHASE_BEGIN,
				m_phaseSampleOrdinal, phase, m_phaseEntryFrame, authorityFrame, actualOwnerFrame))
				m_phaseObservationFailed.store(true, std::memory_order_release);
		}
		else if (boundary != rts::LIVE_SIMULATION_PHASE_OBSERVE_PHASE_END ||
			m_phaseActive != phase || authorityFrame != m_phaseAuthorityFrame ||
			(timed && !ledger.endPhase(m_phaseFrame, phase)))
			m_phaseObservationFailed.store(true, std::memory_order_release);
		else
		{
			if (!observePerformanceReferenceWindow(KERNEL_WINDOW_PHASE_END,
				m_phaseSampleOrdinal, phase, m_phaseEntryFrame, authorityFrame, actualOwnerFrame))
				m_phaseObservationFailed.store(true, std::memory_order_release);
			m_phaseActive = KERNEL_PHASE_COUNT;
			++m_phaseNext;
		}
	}
	catch (...)
	{
		// Failure reporting here cannot allocate or throw through a gameplay
		// callback. Final publication consumes the latch after teardown.
		m_phaseObservationFailed.store(true, std::memory_order_release);
	}
}

void PerformanceReceiptRuntime::observeControlTransition(
	rts::performance::KernelPerformanceControlTransition transition,
	const void *ownerIdentity, unsigned actualOwnerFrame) noexcept
{
	using namespace rts::performance;
	if (!GameThreadOwnership::IsCurrentThread())
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return;
	}
	if (!active() || m_lifecycle.terminalResultKnown()) return;
	if (m_tracePending)
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return;
	}
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	const bool timed = ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	if (ownerIdentity == 0 || ownerIdentity != m_phaseOwnerIdentity ||
		m_phaseGeneration == 0 || m_phaseSampleOrdinal == 0 ||
		m_phaseActive != KERNEL_PHASE_OWNER_INTAKE || m_phaseNext != KERNEL_PHASE_OWNER_INTAKE ||
		actualOwnerFrame != 0 || m_pendingOwnerObservation ||
		m_lifecycle.lastCompletedFrame() != 0 || (timed && !m_phaseFrame.valid()) ||
		(transition != KERNEL_CONTROL_DEFERRED_START_DECLARED &&
		 transition != KERNEL_CONTROL_DEFERRED_START_CONSUMED))
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return;
	}
	try
	{
		if (!observePerformanceReferenceWindow(transition == KERNEL_CONTROL_DEFERRED_START_DECLARED ?
			KERNEL_WINDOW_DEFERRED_START_DECLARED : KERNEL_WINDOW_DEFERRED_START_CONSUMED,
			m_phaseSampleOrdinal, KERNEL_PHASE_OWNER_INTAKE, m_phaseEntryFrame,
			m_phaseAuthorityFrame, actualOwnerFrame))
			m_phaseObservationFailed.store(true, std::memory_order_release);
		if (timed && !ledger.observeControlTransition(m_phaseFrame, transition))
			m_phaseObservationFailed.store(true, std::memory_order_release);
	}
	catch (...)
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
	}
}

bool PerformanceReceiptRuntime::activateNativeTrace(unsigned terminalFrame)
{
	using namespace rts::performance;
	if (!m_traceRequested || !m_tracePending || !active()) return false;
	if (terminalFrame == 0)
	{
		invalidate("native trace terminal frame was unavailable at fixture binding");
		return false;
	}
	const bool baseline = m_receipt.kernelReference.mode ==
		KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	const rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	// The source-policy digest deliberately binds configured W. An automatic
	// zero is resolved only when JobSystem starts, after this fixture callback;
	// admitting it here would mint a policy that may not describe the running
	// scheduler, so native trace qualification fails closed.
	if (config.workerCount == 0)
	{
		invalidate("native trace requires an explicit configured worker count");
		return false;
	}
	rts::JobMetricCounter attempts = 0, ranges = 0;
	if (!deriveNativeReceiptTraceCapacities(config.workerCount, attempts, ranges))
	{
		invalidate("native trace resident envelope could not be derived from configured workers");
		return false;
	}
	const rts::JobMetricCounter maximumTraceBytes =
		static_cast<rts::JobMetricCounter>(16ULL * 1024ULL * 1024ULL * 1024ULL);
	KernelPerformanceTraceOptions trace;
	trace.mode = baseline ? KERNEL_TRACE_CONSUME : KERNEL_TRACE_RECORD;
	trace.limits.maximumBytes = maximumTraceBytes;
	trace.limits.maximumRecords = static_cast<rts::JobMetricCounter>(134217728ULL);
	trace.limits.maximumLogicalEvents = static_cast<rts::JobMetricCounter>(268435456ULL);
	trace.limits.maximumAttempts = static_cast<rts::JobMetricCounter>(33554432ULL);
	trace.limits.maximumRanges = static_cast<rts::JobMetricCounter>(16777216ULL);
	trace.residentAttemptCapacity = attempts;
	trace.residentRangeCapacity = ranges;
	KernelPerformanceDigest executable, fixture;
	if (!decodeNativeReceiptDigest(m_receipt.executableSha256, executable) ||
		!decodeNativeReceiptDigest(m_receipt.fixtureContentSha256, fixture))
	{
		invalidate("native trace executable or fixture digest was not retained in canonical form");
		return false;
	}
	if (baseline)
	{
		const PerformanceReceipt *source = m_nativeTraceFiles == 0 ? 0 :
			m_nativeTraceFiles->source();
		if (source == 0 || source->kernelReference.mode !=
			KERNEL_REFERENCE_THROUGHPUT_BINDING ||
			source->kernelReference.trace.mode != KERNEL_TRACE_RECORD)
		{
			invalidate("native source receipt did not retain a throughput trace");
			return false;
		}
		const KernelPerformanceTraceSnapshot &sourceTrace =
			source->kernelReference.trace;
		trace.binding = sourceTrace.binding;
		if (!trace.binding.nativeRunIdentity.valid ||
			!trace.binding.executable.valid || !trace.binding.fixture.valid ||
			!trace.binding.sourcePolicy.valid ||
			!trace.binding.executable.equals(executable) ||
			!trace.binding.fixture.equals(fixture))
		{
			invalidate("native source trace binding differs from this executable or fixture");
			return false;
		}
		trace.limits = sourceTrace.limits;
		trace.residentAttemptCapacity = sourceTrace.residentAttemptCapacity;
		trace.residentRangeCapacity = sourceTrace.residentRangeCapacity;
		if (trace.residentAttemptCapacity != attempts ||
			trace.residentRangeCapacity != ranges)
		{
			invalidate("native source trace resident envelope differs from configured workers");
			return false;
		}
		trace.readAt = native_receipt_files::NativeReceiptTraceFiles::readAt;
		trace.context = m_nativeTraceFiles;
		trace.sourceByteCount = sourceTrace.byteCount;
		trace.sourceTraceDigest = sourceTrace.digest;
		trace.sourceReceiptDigest = m_sourceReceiptDigest;
		if (!trace.sourceTraceDigest.valid || !trace.sourceReceiptDigest.valid)
		{
			invalidate("native source trace digest metadata was unavailable");
			return false;
		}
		const KernelPerformanceDigest policy = makeNativeReceiptSourcePolicy(
			m_receipt, config, rts::GetSimulationExecutionMode(),
			rts::GetPipelineExecutionMode(), terminalFrame, trace);
		if (!policy.valid || !policy.equals(trace.binding.sourcePolicy))
		{
			invalidate("native source policy does not match this observed replay run");
			return false;
		}
		m_receipt.traceFiles.tracePath = source->traceFiles.tracePath;
		m_receipt.traceFiles.sourceReceiptPath = m_tracePaths.sourceReceiptPath;
		m_receipt.traceFiles.sourceRunId = source->runId;
		m_receipt.traceFiles.sourceRunNonce = source->runNonce;
		m_receipt.traceFiles.sourceProcessId = source->processId;
		m_receipt.traceFiles.sourceProcessCreationTimeUtc100ns =
			source->processCreationTimeUtc100ns;
	}
	else
	{
		trace.binding.nativeRunIdentity = GetPerformanceReceiptRunIdentity(m_receipt);
		trace.binding.executable = executable;
		trace.binding.fixture = fixture;
		trace.binding.sourcePolicy = makeNativeReceiptSourcePolicy(
			m_receipt, config, rts::GetSimulationExecutionMode(),
			rts::GetPipelineExecutionMode(), terminalFrame, trace);
		if (!trace.binding.nativeRunIdentity.valid ||
			!trace.binding.sourcePolicy.valid)
		{
			invalidate("native trace binding could not be derived from observed receipt facts");
			return false;
		}
		std::string reason;
		if (m_nativeTraceFiles == 0 ||
			!m_nativeTraceFiles->openOutput(m_receipt.outputDirectory,
				m_tracePaths.tracePath, maximumTraceBytes, &reason))
		{
			if (reason.empty()) reason = "native trace output could not be held";
			invalidate(reason.c_str());
			return false;
		}
		trace.append = native_receipt_files::NativeReceiptTraceFiles::append;
		trace.context = m_nativeTraceFiles;
		m_receipt.traceFiles.tracePath = m_tracePaths.tracePath;
	}
	KernelPerformanceReferenceRunOptions options;
	options.mode = m_receipt.kernelReference.mode;
	options.trace = trace;
	if (!KernelPerformanceReferenceLedger::instance().beginRun(options))
	{
		invalidate("native trace reference ledger could not activate");
		return false;
	}
	m_traceTerminalFrame = terminalFrame;
	m_tracePending = false;
	m_traceActive = true;
	return true;
}

void PerformanceReceiptRuntime::bindFixture(const char *kind, const char *contentPath,
	const char *sha256, unsigned seed, unsigned terminalFrame)
{
	if (!active()) return;
	std::string reason;
	if (!rts::performance::BindPerformanceReceiptFixtureObservation(m_receipt,
		kind, contentPath, sha256, seed, &reason))
	{
		invalidate(reason.c_str());
		return;
	}
	if (m_tracePending && !activateNativeTrace(terminalFrame))
		if (m_failure.empty()) invalidate("native trace activation failed after fixture binding");
}

void PerformanceReceiptRuntime::captureCompletedFrame(unsigned previousFrame,
	const rts::CollisionCandidateRuntimeMetrics &collision,
	const rts::PhysicsIntegrationRuntimeMetrics &physics,
	const rts::ObjectStatusTimerRuntimeMetrics &status,
	const rts::ImmutableSpatialRuntimeMetrics &spatial,
	const OrdinaryPathRuntimeMetrics &path)
{
	if (!active()) return;
	if (!GameThreadOwnership::IsCurrentThread())
	{
		m_phaseObservationFailed.store(true, std::memory_order_release);
		return;
	}
	// The pending terminal frame must still be consumed once. After that,
	// teardown updates are outside the sealed workload and may destroy globals.
	if (m_lifecycle.terminalResultKnown() && !m_pendingOwnerObservation) return;
	if (TheGameLogic == 0 || ThePlayerList == 0 || TheGameLogic->isInGameLogicUpdate())
	{
		invalidate("completed-frame owner or roster was unavailable or still updating");
		return;
	}
	const unsigned frame = TheGameLogic->getFrame();
	if (!m_pendingOwnerObservation)
	{
		// Fresh loading may not call this hook until its first loaded frame.
		// Zero never grants a completed sample or bootstrap qualification.
		if (frame == 0) return;
		// An outer engine tick without UPDATE is not a second completion.
		if (frame != previousFrame)
			invalidate("workload capture has no unconsumed real owner observation");
		return;
	}
	if (m_phaseObservationFailed.load(std::memory_order_acquire) ||
		m_phaseGeneration != 0 || TheGameLogic != m_phaseOwnerIdentity ||
		frame != m_pendingFrame || previousFrame != m_pendingEntryFrame)
	{
		invalidate("workload capture disagrees with its pending owner observation");
		return;
	}
	m_pendingOwnerObservation = false;
	// Reset83 -> completed1 remains valid; pending identity, not numeric
	// comparison with the old world, authorizes this one workload sample.
	if (!m_lifecycle.observeCompletedFrame(frame))
	{
		invalidate("completed owner frames were not consumed exactly once in order");
		return;
	}
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
	const bool drained = outstanding == 0 && ownerCompletions == 0;
	const bool complete = m_lifecycle.finish(outstanding, ownerCompletions) &&
		!m_pendingOwnerObservation && m_phaseGeneration == 0;
	bool referenceClosed = true;
	bool timingLedgerClosed = true;
	bool receiptMetricsProjected = true;
	if (m_traceActive && drained)
	{
		rts::performance::KernelPerformanceReferenceLedger &reference =
			rts::performance::KernelPerformanceReferenceLedger::instance();
		referenceClosed = reference.sealObservationWindow() &&
		reference.sealExecutionClosure();
		if (m_receipt.kernelReference.mode ==
			rts::performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
			timingLedgerClosed =
				rts::performance::KernelPerformanceLedger::instance().sealExecutionClosure(
					capturePerformancePhaseSchedulerBoundary());
	}
	if (drained)
	{
		m_receipt.kernelTiming = rts::performance::KernelPerformanceLedger::instance().freeze();
		if (m_traceActive || !m_traceRequested)
		{
			m_receipt.kernelReference = rts::performance::KernelPerformanceReferenceLedger::instance().freeze();
			receiptMetricsProjected =
				projectPerformanceReceiptBaselineMetrics(m_receipt);
		}
	}
	m_receipt.frameStart = 0;
	m_receipt.frameEnd = m_lifecycle.terminalFrame();
	m_receipt.finalFrame = m_lifecycle.terminalFrame();
	m_receipt.finalCrc = m_lifecycle.terminalCrc();
	m_receipt.finalCrcKnown = m_lifecycle.terminalResultKnown();
	HANDLE timingEvidence = INVALID_HANDLE_VALUE;
	const bool timingClosed = resolvePerformanceReceiptTimingPath(m_receipt,
		m_traceRequested ? &timingEvidence : 0);
	std::string reason = m_failure;
	if (reason.empty() && m_traceRequested && m_tracePending)
		reason = "native trace was not activated after fixture binding";
	if (reason.empty() && m_phaseObservationFailed.load(std::memory_order_acquire))
		reason = "phase observation did not retain a complete owner frame identity";
	if (reason.empty() && !complete)
		reason = "owner lifecycle is incomplete or authoritative work is not drained";
	if (reason.empty() && m_traceActive && m_lifecycle.terminalResultKnown() &&
		m_traceTerminalFrame != m_lifecycle.terminalFrame())
		reason = "native trace terminal frame differs from the owner result";
	if (reason.empty() && m_traceRequested && !drained)
		reason = "native trace could not close before authoritative work drained";
	if (reason.empty() && m_traceActive && !referenceClosed)
		reason = "native reference trace observation or execution closure failed";
	if (reason.empty() && m_traceActive && !timingLedgerClosed)
		reason = "native baseline timing execution closure failed";
	if (reason.empty() && !receiptMetricsProjected)
		reason = "native baseline receipt metrics could not be projected";
	if (reason.empty() && exitCode != 0) reason = "owner run did not exit cleanly";
	if (reason.empty() && !timingClosed) reason = "closed frame-timing CSV could not be resolved";
	if (m_traceRequested && m_nativeTraceFiles != 0)
	{
		std::string traceReason;
		const bool baseline = m_receipt.kernelReference.mode ==
			rts::performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
		const bool traceSealed = baseline ?
			m_nativeTraceFiles->sealSource(&traceReason) :
			m_nativeTraceFiles->sealOutput(m_receipt.kernelReference.trace, &traceReason);
		if (reason.empty() && !traceSealed)
		{
			reason = traceReason.empty() ? "native trace identity seal failed" : traceReason;
		}
	}
	const bool clean = reason.empty();
	HANDLE rawEvidence = INVALID_HANDLE_VALUE;
	const bool rawClosed = clean && writePerformanceReceiptRawDiagnostic(m_receipt,
		m_traceRequested ? &rawEvidence : 0);
	if (clean && !rawClosed) reason = "game-owned raw diagnostic could not be closed";
	bool evidenceHeld = rawClosed;
	if (evidenceHeld && m_traceRequested && m_nativeTraceFiles != 0)
	{
		std::string evidenceReason;
		evidenceHeld = m_nativeTraceFiles->holdEvidence(rawEvidence,
			m_receipt.rawEvidence.rawLogPath, timingEvidence,
			m_receipt.rawEvidence.timingPath, &evidenceReason);
		// holdEvidence consumes both handles on success and failure.
		rawEvidence = INVALID_HANDLE_VALUE;
		timingEvidence = INVALID_HANDLE_VALUE;
		if (!evidenceHeld) reason = evidenceReason.empty() ?
			"native raw/timing evidence could not be held" : evidenceReason;
	}
	if (rawEvidence != INVALID_HANDLE_VALUE)
		native_receipt_files::closeOwnedHandle(rawEvidence);
	if (timingEvidence != INVALID_HANDLE_VALUE)
		native_receipt_files::closeOwnedHandle(timingEvidence);
	const bool resultCaptured = evidenceHeld && rts::performance::SetPerformanceReceiptReplayResult(
		m_receipt, m_receipt.frameStart, m_receipt.finalFrame, m_receipt.finalCrc,
		m_receipt.finalCrcKnown, exitCode, true, boundary, true, &reason);
	std::string writtenPath;
	const bool written = resultCaptured && rts::performance::WritePerformanceReceiptAtomically(
		m_receipt, m_receipt.outputDirectory.c_str(), &writtenPath, &reason,
		m_traceRequested ? native_receipt_files::NativeReceiptTraceFiles::retainPublication : 0,
		m_traceRequested ? m_nativeTraceFiles : 0);
	bool publicationClosed = written;
	if (written && m_traceRequested && m_nativeTraceFiles != 0)
	{
		std::string closureReason;
		publicationClosed = m_nativeTraceFiles->finishPublication(writtenPath, &closureReason);
		if (!publicationClosed) reason = closureReason.empty() ?
			"native publication checked closure failed" : closureReason;
	}
	if (written && publicationClosed)
		printf("SIMULATION_PERFORMANCE_RECEIPT status=written path=%s\n", writtenPath.c_str());
	else
	{
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=%s\n", reason.c_str());
		printUnsupportedPerformanceSnapshot(m_receipt, outstanding, ownerCompletions);
	}
	fflush(stdout);
	m_active = false;
	releaseNativeTrace();
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
	if (performanceReceipt.traceRequested() && !performanceReceiptRequested) return 1;
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
						performanceSource.sha256(), static_cast<unsigned>(recordedGame->getSeed()),
						static_cast<unsigned>(TheRecorder->getPlaybackFrameCount()));
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
	{
		if (!performanceSource.finish())
			performanceReceipt.invalidate(
				"immutable replay identity or extent changed before checked closure");
		performanceReceipt.finish(numErrors != 0 ? 1 : 0,
			"ReplaySimulation::simulateReplaysInThisProcess:return");
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
#if defined(_WIN64)
	// Native receipt ownership is defined for one concrete, sequential,
	// headless replay only. Make this decision from the caller's original shape
	// before wildcard expansion can enumerate or substitute any filesystem
	// entries; a wildcard that happens to resolve to one file is still not an
	// identity-safe single-replay request.
	const bool explicitTraceRequested =
		PerformanceReceiptRuntime::explicitTraceRequestedFromEnvironment();
	const bool concreteSingleReplay = filenames.size() == 1 &&
		!filenames[0].find('*') && !filenames[0].find('?');
	const bool supportedNativeShape = TheGlobalData->m_headless &&
		concreteSingleReplay && maxProcesses == SIMULATE_REPLAYS_SEQUENTIAL;
	if (!supportedNativeShape && explicitTraceRequested)
	{
		printf("SIMULATION_PERFORMANCE_RECEIPT status=unsupported reason=native trace owner shape is unsupported\n");
		fflush(stdout);
		return 1;
	}
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
