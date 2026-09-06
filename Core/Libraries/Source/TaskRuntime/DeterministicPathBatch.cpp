/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicPathBatch.h"

#include "Lib/JobSystem.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace rts
{
#if defined(_WIN64)
DeterministicPathOwnerCompletion::DeterministicPathOwnerCompletion() noexcept
	: m_expectedOperations(0), m_completedOperations(0),
	  m_allOperationsCommitted(true),
	  m_activeOperationNeedsLegacyFallback(false),
	  m_batchNeedsLegacyFallback(false), m_fallbackEntered(false),
	  m_fallbackCompleted(false)
{
}

void DeterministicPathOwnerCompletion::reset(
	std::size_t expectedOperations) noexcept
{
	m_expectedOperations = expectedOperations;
	m_completedOperations = 0;
	m_allOperationsCommitted = true;
	m_activeOperationNeedsLegacyFallback = false;
	m_batchNeedsLegacyFallback = false;
	m_fallbackEntered = false;
	m_fallbackCompleted = false;
}

void DeterministicPathOwnerCompletion::beginOperation() noexcept
{
	m_activeOperationNeedsLegacyFallback = false;
}

void DeterministicPathOwnerCompletion::finishOperation(
	bool committedOperation, bool materializationBegan) noexcept
{
	if (m_completedOperations < m_expectedOperations)
		++m_completedOperations;
	else
		m_allOperationsCommitted = false;
	if (!committedOperation)
	{
		m_allOperationsCommitted = false;
		m_activeOperationNeedsLegacyFallback = !materializationBegan;
	}
}

void DeterministicPathOwnerCompletion::expectLegacyFallback() noexcept
{
	m_batchNeedsLegacyFallback = true;
}

bool DeterministicPathOwnerCompletion::beginLegacyFallback() noexcept
{
	if (!m_activeOperationNeedsLegacyFallback && !m_batchNeedsLegacyFallback)
		return false;
	m_activeOperationNeedsLegacyFallback = false;
	m_batchNeedsLegacyFallback = false;
	m_fallbackEntered = true;
	m_fallbackCompleted = false;
	return true;
}

void DeterministicPathOwnerCompletion::completeLegacyFallback(
	bool entered) noexcept
{
	if (entered && m_fallbackEntered)
		m_fallbackCompleted = true;
}

bool DeterministicPathOwnerCompletion::committed() const noexcept
{
	return m_expectedOperations != 0 &&
		m_completedOperations == m_expectedOperations &&
		m_allOperationsCommitted;
}

bool DeterministicPathOwnerCompletion::fallbackEntered() const noexcept
{
	return m_fallbackEntered;
}

bool DeterministicPathOwnerCompletion::fallbackCompleted() const noexcept
{
	return m_fallbackCompleted;
}
#endif

namespace
{

#if defined(_WIN64)
class PathPerformanceInterval
{
public:
	PathPerformanceInterval(performance::KernelPerformanceBatch *batch,
		performance::KernelPerformanceStage stage) : m_ledger(nullptr)
	{
		if (batch != nullptr && batch->valid())
		{
			m_ledger = &performance::KernelPerformanceLedger::instance();
			m_interval = m_ledger->beginInterval(*batch, stage);
		}
	}

	~PathPerformanceInterval()
	{
		end();
	}

	void end()
	{
		if (m_ledger != nullptr && m_interval.valid())
		{
			m_ledger->endInterval(m_interval);
			m_interval = performance::KernelPerformanceInterval();
		}
	}

private:
	performance::KernelPerformanceLedger *m_ledger;
	performance::KernelPerformanceInterval m_interval;
	PathPerformanceInterval(const PathPerformanceInterval &);
	PathPerformanceInterval &operator=(const PathPerformanceInterval &);
};
#endif

std::atomic<unsigned> s_activeDirectPathBatches(0);
std::atomic<unsigned> s_directPathLateDrainExecutions(0);

enum DirectPathWorkState
{
	DIRECT_PATH_WORK_PENDING = 0,
	DIRECT_PATH_WORK_RUNNING_WORKER,
	DIRECT_PATH_WORK_RUNNING_OWNER,
	DIRECT_PATH_WORK_CANCELLED,
	DIRECT_PATH_WORK_WORKER,
	DIRECT_PATH_WORK_OWNER,
	DIRECT_PATH_WORK_FAILURE
#if defined(_WIN64)
	, DIRECT_PATH_WORK_RUNNING_INLINE,
	DIRECT_PATH_WORK_INLINE
#endif
};

#if defined(RTS_BUILD_CORE_EXTRAS)
std::atomic<unsigned> s_directPathTestPauseMask(0);
std::atomic<unsigned> s_directPathTestPauseReachedMask(0);
std::atomic<unsigned> s_directPathTestPauseReachedCount(0);
std::atomic<unsigned> s_directPathTestPauseReleasedMask(0);
std::atomic<unsigned> s_directPathTestFaultMask(0);
const unsigned DIRECT_PATH_TEST_SOURCE_RECORD_ALLOCATION_FAILURE = 32U;
const unsigned DIRECT_PATH_TEST_CHECKPOINT_ALLOCATION_FAILURE = 64U;
const unsigned DIRECT_PATH_TEST_GROUP_COPY_ALLOCATION_FAILURE = 128U;

void pauseDirectPathTest(unsigned pausePoint)
{
	if ((s_directPathTestPauseMask.load(std::memory_order_acquire) &
		pausePoint) == 0)
	{
		return;
	}
	s_directPathTestPauseReachedMask.fetch_or(pausePoint,
		std::memory_order_acq_rel);
	s_directPathTestPauseReachedCount.fetch_add(1, std::memory_order_acq_rel);
	while ((s_directPathTestPauseMask.load(std::memory_order_acquire) &
		pausePoint) != 0 &&
		(s_directPathTestPauseReleasedMask.load(std::memory_order_acquire) &
			pausePoint) == 0)
	{
		std::this_thread::yield();
	}
}

bool waitForDirectPathTestPause(unsigned pausePoint,
	unsigned requiredCount, unsigned timeoutMilliseconds)
{
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeoutMilliseconds);
	while ((s_directPathTestPauseReachedMask.load(std::memory_order_acquire) &
		pausePoint) == 0 ||
		s_directPathTestPauseReachedCount.load(std::memory_order_acquire) <
			requiredCount)
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::yield();
	}
	return true;
}
#endif

struct DirectPathWork
{
	DirectPathWork() : executionState(DIRECT_PATH_WORK_PENDING),
		physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX)
	{
		snapshot = {};
		result = {};
	}

	DirectPathSnapshot snapshot;
	DirectPathSearchResult result;
	std::array<DirectPathCellFact,
		DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS> callbacks;
	std::array<DirectPathCellFact,
		DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT> startNeighbors;
	std::array<DeterministicPathPoint,
		DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS> rawPoints;
	std::atomic<unsigned> executionState;
	std::atomic<unsigned> physicalWorkerIndex;
};

#if defined(_WIN64)
struct DirectPathSourceRecord
{
	DirectPathSourceRecord() : ledger(nullptr), workers(0), pending(0),
		outstanding(0), baseline(false), admitted(false), planned(false),
		collected(false), failed(false), sourceOwnerCommitAllowed(false) {}

	performance::KernelPerformanceReferenceLedger *ledger;
	performance::KernelPerformanceAttempt attempt;
	performance::KernelPerformanceDigest facts;
	std::unique_ptr<performance::KernelPerformanceCheckpointProbe[]> checkpoints;
	JobGroup group;
	unsigned workers;
	JobMetricCounter pending, outstanding;
	bool baseline, admitted, planned, collected, failed;
	bool sourceOwnerCommitAllowed;
	performance::KernelPerformanceInlineBody inlineBody;
};

struct OrdinaryPathSourceRecord;

// These gates deliberately require the owner thread, an authentic attempt and
// a retained source record.  A missing owner/attempt is a refusal, never an
// invitation to execute a serial-looking body or reap a live group.
struct InlineBodyReadiness
{
	static bool direct(const DirectPathSourceRecord &source,
		const JobSystem &jobs);
	static bool ordinary(const OrdinaryPathSourceRecord &source,
		const JobSystem &jobs);
};

struct ReapReadiness
{
	static bool direct(const DirectPathSourceRecord &source,
		const JobSystem &jobs);
	static bool ordinary(const OrdinaryPathSourceRecord &source,
		const JobSystem &jobs);
};
#endif

struct DirectPathBatchWork
{
	DirectPathBatchWork() : requestCount(0), activeWorkers(0),
		peakActiveWorkers(0), liveJobs(0), ownsActiveSlot(false),
		referenceAdmissionAccepted(false)
	{}

	~DirectPathBatchWork()
	{
		if (ownsActiveSlot.exchange(false, std::memory_order_acq_rel))
			s_activeDirectPathBatches.fetch_sub(1, std::memory_order_acq_rel);
	}

	std::array<DirectPathWork,
		DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS> requests;
	std::size_t requestCount;
	std::atomic<unsigned> activeWorkers;
	std::atomic<unsigned> peakActiveWorkers;
	std::atomic<unsigned> liveJobs;
	std::atomic<bool> ownsActiveSlot;
	bool referenceAdmissionAccepted;
#if defined(_WIN64)
	std::unique_ptr<DirectPathSourceRecord> reference;
#endif
};

#if defined(_WIN64)
bool InlineBodyReadiness::direct(const DirectPathSourceRecord &source,
	const JobSystem &jobs)
{
	return source.ledger != nullptr && source.attempt.valid() &&
		source.baseline && !source.failed && !source.inlineBody.valid() &&
		jobs.isCurrentThread(JOB_OWNER_GAME);
}

bool ReapReadiness::direct(const DirectPathSourceRecord &source,
	const JobSystem &jobs)
{
	return source.ledger != nullptr && source.attempt.valid() &&
		!source.baseline && jobs.isCurrentThread(JOB_OWNER_GAME) &&
		source.group.isValid() && source.group.isComplete();
}
#endif

#if defined(_WIN64)
/*
** PATH reference schema 1 is pointer-free and order-sensitive.  Input tags
** carry the complete immutable direct snapshot (ordered callback and start
** neighbour facts); output tags carry every scalar result field and the
** ordered raw chain.  Capacities and pointers are storage identities, not
** path semantics, so they are deliberately excluded.
*/
struct DirectPathReferenceInput
{
	const DirectPathBatchWork *batch;
	std::size_t requestCount;
};

struct DirectPathReferenceOutputView
{
	const DirectPathSearchResult *results;
	std::size_t count;
	const void *detachedStorage;
};

struct DirectPathReferenceDetachedOutput
{
	DirectPathReferenceDetachedOutput() : view(), count(0)
	{
		view.results = results.data();
		view.count = 0;
		view.detachedStorage = this;
	}

	DirectPathReferenceOutputView view;
	std::size_t count;
	std::array<DirectPathSearchResult,
		DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS> results;
	std::array<std::array<DeterministicPathPoint,
		DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS>,
		DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS> rawPoints;
};

bool WriteDirectPathFact(performance::KernelPerformanceCanonicalWriter &writer,
	const DirectPathCellFact &fact)
{
	return writer.i32(3, fact.x) && writer.i32(4, fact.y) &&
		writer.u32(5, fact.zone) && writer.u32(6, fact.flags) &&
		writer.u32(7, fact.hasPathfindInfo);
}

bool WriteDirectPathReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const DirectPathReferenceInput &input =
		*static_cast<const DirectPathReferenceInput *>(context);
	if (input.batch == nullptr || input.requestCount == 0 ||
		input.requestCount > DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS ||
		!writer.sequence(1, static_cast<unsigned>(input.requestCount)))
		return false;
	for (std::size_t index = 0; index < input.requestCount; ++index)
	{
		const DirectPathSnapshot &snapshot =
			input.batch->requests[index].snapshot;
		if (!writer.sequence(2, static_cast<unsigned>(snapshot.callbackCount)))
			return false;
		for (std::size_t callback = 0; callback < snapshot.callbackCount;
			++callback)
		{
			if (!WriteDirectPathFact(writer, snapshot.callbacks[callback]))
				return false;
		}
		if (!writer.sequence(8,
			static_cast<unsigned>(snapshot.startNeighborCount)))
			return false;
		for (std::size_t neighbor = 0;
			neighbor < snapshot.startNeighborCount; ++neighbor)
		{
			if (!WriteDirectPathFact(writer,
				snapshot.startNeighbors[neighbor]))
				return false;
		}
		if (!writer.u32(10, snapshot.topologyOccupancyGeneration) ||
			!writer.u32(11, snapshot.requestToken) ||
			!writer.u32(12, snapshot.objectId) ||
			!writer.u32(13, snapshot.availableCellInfoCount) ||
			!writer.i32(14, snapshot.startX) ||
			!writer.i32(15, snapshot.startY) ||
			!writer.i32(16, snapshot.goalX) ||
			!writer.i32(17, snapshot.goalY) ||
			!writer.u32(18, snapshot.requiredZone) ||
			!writer.u32(19, snapshot.expectedLayer))
			return false;
	}
	return true;
}

bool WriteDirectPathReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const DirectPathReferenceOutputView &output =
		*static_cast<const DirectPathReferenceOutputView *>(context);
	if (output.results == nullptr || output.count == 0 ||
		!writer.sequence(20, static_cast<unsigned>(output.count)))
		return false;
	for (std::size_t index = 0; index < output.count; ++index)
	{
		const DirectPathSearchResult &result = output.results[index];
		if (!writer.u32(21, static_cast<unsigned>(result.status)) ||
			!writer.u32(22, static_cast<unsigned>(result.rawPointCount)) ||
			!writer.u32(23, static_cast<unsigned>(result.callbackCount)) ||
			!writer.u32(24, static_cast<unsigned>(result.requiredCellInfoCount)) ||
			!writer.u32(25, static_cast<unsigned>(
				result.startNeighborAllocationCount)) ||
			!writer.u32(26, static_cast<unsigned>(result.openCellCountAfterGoal)) ||
			!writer.u32(27, static_cast<unsigned>(result.cumulativeCellCount)) ||
			!writer.u32(28, result.topologyOccupancyGeneration) ||
			!writer.u32(29, result.requestToken) ||
			!writer.u32(30, result.objectId) ||
			!writer.sequence(31, static_cast<unsigned>(result.rawPointCount)))
			return false;
		for (std::size_t point = 0; point < result.rawPointCount; ++point)
		{
			const DeterministicPathPoint &value = result.rawPoints[point];
			if (!writer.i32(32, value.x) || !writer.i32(33, value.y) ||
				!writer.u32(34, value.layer))
				return false;
		}
	}
	return true;
}

bool SerialComputeDirectPathReference(const void *immutableInput,
	void *detachedSerialOutput)
{
	const DirectPathReferenceInput &input =
		*static_cast<const DirectPathReferenceInput *>(immutableInput);
	DirectPathReferenceOutputView &view =
		*static_cast<DirectPathReferenceOutputView *>(detachedSerialOutput);
	if (view.detachedStorage == nullptr)
		return false;
	DirectPathReferenceDetachedOutput &detached =
		*static_cast<DirectPathReferenceDetachedOutput *>(
			const_cast<void *>(view.detachedStorage));
	if (input.batch == nullptr || input.requestCount == 0 ||
		input.requestCount > DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS)
		return false;
	detached.count = input.requestCount;
	detached.view.count = input.requestCount;
	for (std::size_t index = 0; index < input.requestCount; ++index)
	{
		DirectPathSearchResult &result = detached.results[index];
		result = {};
		result.rawPoints = detached.rawPoints[index].data();
		result.rawPointCapacity = detached.rawPoints[index].size();
		FindDeterministicDirectPath(input.batch->requests[index].snapshot,
			result);
	}
	return true;
}

// Defined below the job body; source execution can publish cancellation from
// either a physical worker or an authenticated owner-inline body.
void publishDirectPathCancellation(DirectPathWork &work);
void publishPeak(std::atomic<unsigned> &peak, unsigned value);

bool PrepareDirectPathSourceRecord(DirectPathBatchWork &batch,
	JobSystem &jobs, performance::KernelPerformanceReferenceLedger *ledger,
	performance::KernelPerformanceAttempt attempt)
{
	if (ledger == nullptr)
		return true;
	if (!attempt.valid())
		return !ledger->traceRequested();
	const performance::KernelPerformanceReferenceMode mode = ledger->runMode();
	if (mode != performance::KERNEL_REFERENCE_THROUGHPUT_BINDING &&
		mode != performance::KERNEL_REFERENCE_SERIAL_ORACLE &&
		mode != performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
		return false;
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestFaultMask.load(std::memory_order_acquire) &
		DIRECT_PATH_TEST_SOURCE_RECORD_ALLOCATION_FAILURE) != 0)
		throw std::bad_alloc();
	#endif
	std::unique_ptr<DirectPathSourceRecord> source(new DirectPathSourceRecord());
	source->ledger = ledger;
	source->attempt = attempt;
	source->baseline = mode == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	source->workers = jobs.workerCount();
	source->pending = jobs.pendingOwnerCompletionCount();
	source->outstanding = jobs.outstandingJobCount();
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestFaultMask.load(std::memory_order_acquire) &
		DIRECT_PATH_TEST_CHECKPOINT_ALLOCATION_FAILURE) != 0)
		throw std::bad_alloc();
	#endif
	source->checkpoints.reset(new performance::KernelPerformanceCheckpointProbe[
		batch.requestCount]);
	performance::KernelPerformanceCanonicalWriter facts;
	const DirectPathReferenceInput input = {&batch, batch.requestCount};
	if (!facts.begin(1) || !WriteDirectPathReferenceInput(facts, &input))
		return false;
	source->facts = facts.finish();
	if (!source->facts.valid || !ledger->bindCapturedInput(attempt, 1,
		batch.requestCount, WriteDirectPathReferenceInput, &input))
		return false;
	batch.reference = std::move(source);
	return true;
}

bool ObserveDirectPathSourceAdmission(DirectPathBatchWork &batch,
	bool admitted)
{
	if (!batch.reference)
		return true;
	DirectPathSourceRecord &source = *batch.reference;
	source.admitted = admitted;
	performance::KernelPerformanceAttemptDecision decision = {};
	decision.site = 1;
	decision.reasonSchema = 1;
	decision.reason = admitted ? 1 : 2;
	decision.deterministicEligible = true;
	decision.deterministicFacts = source.facts;
	decision.admission = admitted ? performance::KERNEL_ADMISSION_ACCEPTED :
		performance::KERNEL_ADMISSION_REFUSED;
	decision.sourceConfiguredWorkers = source.workers;
	decision.dynamicFactsKnownMask = 7;
	decision.pendingJobs = source.pending;
	decision.outstandingJobs = source.outstanding;
	decision.activeSlots = 0;
	if (!source.ledger->observeDecision(source.attempt, decision))
	{
		source.failed = true;
		return false;
	}
	if (!admitted)
		return true;
	batch.referenceAdmissionAccepted = true;
	const performance::KernelPerformanceDispatchPlan dispatch = {
		1, 1, 1, static_cast<unsigned>(batch.requestCount),
		static_cast<rts::JobMetricCounter>(batch.requestCount), 1,
		source.workers};
	if (!source.ledger->observeDispatch(source.attempt, dispatch))
	{
		source.failed = true;
		return false;
	}
	for (unsigned i = 0; i < batch.requestCount; ++i)
	{
		const performance::KernelPerformanceRangePlan range = {
			1, i, 0, i, i + 1, 1};
		if (!source.ledger->observeRangePlan(source.attempt, range))
		{
			source.failed = true;
			return false;
		}
	}
	source.planned = true;
	return true;
}

performance::KernelPerformanceRangeProgress DirectPathReleasedProgress(
	const DirectPathBatchWork &batch, unsigned index)
{
	performance::KernelPerformanceRangeProgress progress = {};
	progress.checkpoint = batch.reference->checkpoints[index].snapshot();
	progress.publication = !progress.checkpoint.entered ?
		performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
		batch.reference->group.wasCancelled() ||
		progress.checkpoint.terminal == performance::KERNEL_RANGE_CANCELLED ?
		performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL :
		progress.checkpoint.terminal == performance::KERNEL_RANGE_COMPLETED ?
		performance::KERNEL_PUBLICATION_PUBLISHED :
		performance::KERNEL_PUBLICATION_REJECTED;
	return progress;
}

bool CollectDirectPathSourceRecord(DirectPathBatchWork &batch, JobSystem &jobs)
{
	if (!jobs.isCurrentThread(JOB_OWNER_GAME) || !batch.reference)
		return false;
	DirectPathSourceRecord &source = *batch.reference;
	if (source.failed)
		return false;
	if (source.collected)
		return true;
	if (source.baseline)
		return false;
	if (!ReapReadiness::direct(source, jobs) || !source.admitted ||
		!source.planned)
		return false;
	for (unsigned i = 0; i < batch.requestCount; ++i)
	{
		const performance::KernelPerformanceRangeProgress progress =
			DirectPathReleasedProgress(batch, i);
		const performance::KernelPerformanceRangePlan range = {1, i, 0,
			i, i + 1, 1};
		if (!source.ledger->observeReleasedRange(source.attempt, range,
			progress))
		{
			source.failed = true;
			return false;
		}
	}
	source.collected = true;
	return true;
}

class DirectPathSourceCheckpointScope
{
public:
	DirectPathSourceCheckpointScope(DirectPathBatchWork &batch,
		unsigned requestIndex) :
		m_probe(batch.reference ? &batch.reference->checkpoints[requestIndex] : nullptr),
		m_range(requestIndex), m_completed(0), m_last{1, requestIndex,
			requestIndex}, m_cancelled(false), m_succeeded(false)
	{
		if (m_probe != nullptr && !batch.reference->baseline)
			m_probe->beginRecord();
	}
	~DirectPathSourceCheckpointScope()
	{
		if (m_probe == nullptr)
			return;
		const performance::KernelPerformanceCheckpoint end = {
			3, m_range, m_range + m_completed};
		m_probe->finish(m_cancelled ? m_last : end, m_completed,
			m_cancelled ? performance::KERNEL_RANGE_CANCELLED :
			m_succeeded ? performance::KERNEL_RANGE_COMPLETED :
			performance::KERNEL_RANGE_FAILED);
	}
	bool cancelled(unsigned site, std::size_t request, bool actual)
	{
		m_last = {site, m_range, request};
		m_cancelled = m_probe != nullptr ? m_probe->cancelled(m_last,
			actual) : actual;
		return m_cancelled;
	}
	void completedRequest() { ++m_completed; }
	void finish(bool succeeded) { m_succeeded = succeeded; }
private:
	performance::KernelPerformanceCheckpointProbe *m_probe;
	unsigned m_range;
	std::size_t m_completed;
	performance::KernelPerformanceCheckpoint m_last;
	bool m_cancelled, m_succeeded;
};

bool ExecuteDirectPathBody(const std::shared_ptr<DirectPathBatchWork> &batch,
	std::size_t requestIndex, JobContext *context)
{
	DirectPathWork &work = batch->requests[requestIndex];
	bool inlineExecution = false;
	#if defined(_WIN64)
	inlineExecution = context == nullptr && batch->reference &&
		batch->reference->baseline && batch->reference->inlineBody.valid();
	#endif
	if (context == nullptr && !inlineExecution)
		return false;
	#if defined(_WIN64)
	DirectPathSourceCheckpointScope checkpoint(*batch,
		static_cast<unsigned>(requestIndex));
	#endif
	const bool cancelled = context != nullptr && context->isCancellationRequested();
	#if defined(_WIN64)
	if (checkpoint.cancelled(1, requestIndex, cancelled))
	#else
	if (cancelled)
	#endif
	{
		publishDirectPathCancellation(work);
		if (context != nullptr)
			s_directPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
		return false;
	}
	const bool workerExecution = context != nullptr &&
		context->isPhysicalWorkerExecution();
	const unsigned runningState =
		#if defined(_WIN64)
		inlineExecution ? DIRECT_PATH_WORK_RUNNING_INLINE :
		#endif
		workerExecution ? DIRECT_PATH_WORK_RUNNING_WORKER :
		DIRECT_PATH_WORK_RUNNING_OWNER;
	const unsigned completedState =
		#if defined(_WIN64)
		inlineExecution ? DIRECT_PATH_WORK_INLINE :
		#endif
		workerExecution ? DIRECT_PATH_WORK_WORKER : DIRECT_PATH_WORK_OWNER;
	unsigned expectedState = DIRECT_PATH_WORK_PENDING;
	if (!work.executionState.compare_exchange_strong(expectedState,
		runningState, std::memory_order_acq_rel, std::memory_order_acquire))
	{
		if (expectedState == DIRECT_PATH_WORK_CANCELLED && context != nullptr)
			s_directPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
		return false;
	}
	if (workerExecution)
	{
		work.physicalWorkerIndex.store(context->physicalWorkerIndex(),
			std::memory_order_release);
		const unsigned active = batch->activeWorkers.fetch_add(1,
			std::memory_order_acq_rel) + 1;
		publishPeak(batch->peakActiveWorkers, active);
		#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseDirectPathTest(2);
		#endif
	}
	bool succeeded = true;
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestFaultMask.load(std::memory_order_acquire) & 1) != 0)
		succeeded = false;
	#endif
	if (succeeded)
		FindDeterministicDirectPath(work.snapshot, work.result);
	#if defined(_WIN64)
	if (succeeded)
		checkpoint.completedRequest();
	checkpoint.finish(succeeded);
	#endif
	if (workerExecution)
		batch->activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
	expectedState = runningState;
	if (!work.executionState.compare_exchange_strong(expectedState,
		(succeeded ? completedState : DIRECT_PATH_WORK_FAILURE),
		std::memory_order_release, std::memory_order_acquire) &&
		expectedState == DIRECT_PATH_WORK_CANCELLED)
	{
		if (context != nullptr)
			s_directPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
	}
	return succeeded;
}

void ObserveDirectPathReference(const DirectPathBatchWork &batch,
	std::size_t requestCount, performance::KernelPerformanceBatch *timingBatch,
	performance::KernelPerformanceReferenceLedger *referenceLedger,
	performance::KernelPerformanceReferenceBatch *referenceBatch)
{
	if (referenceBatch == nullptr)
		return;
	*referenceBatch = performance::KernelPerformanceReferenceBatch();
	if (timingBatch == nullptr || referenceLedger == nullptr ||
		!timingBatch->valid())
		return;
	const performance::KernelPerformanceReferenceMode mode =
		referenceLedger->mode();
	if (mode == performance::KERNEL_REFERENCE_DISABLED)
		return;
	if (batch.reference && (!batch.reference->collected ||
		batch.reference->failed))
		return;
	performance::KernelPerformanceBatchIdentity identity;
	if (!performance::KernelPerformanceLedger::instance().describeBatch(
		*timingBatch, identity) || identity.kernel !=
		performance::KERNEL_PERFORMANCE_PATH || identity.subtype != 1)
		return;

	std::array<DirectPathSearchResult,
		DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS> productionResults;
	for (std::size_t index = 0; index < requestCount; ++index)
		productionResults[index] = batch.requests[index].result;
	DirectPathReferenceInput input = {&batch, requestCount};
	DirectPathReferenceOutputView production = {
		productionResults.data(), requestCount, nullptr};
	std::unique_ptr<DirectPathReferenceDetachedOutput> detached;
	if (mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE)
	{
		try
		{
			detached.reset(new DirectPathReferenceDetachedOutput());
		}
		catch (...)
		{
			return;
		}
	}
	if (batch.reference)
	{
		*referenceBatch = referenceLedger->observeValidatedAttempt(
			batch.reference->attempt, WriteDirectPathReferenceOutput,
			&production);
		return;
	}
	*referenceBatch = referenceLedger->observeValidatedBatch(
		performance::KERNEL_PERFORMANCE_PATH, identity.subtype, identity.frame,
		identity.ordinal, 1, static_cast<rts::JobMetricCounter>(requestCount),
		WriteDirectPathReferenceInput, &input,
		WriteDirectPathReferenceOutput, &production,
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
			SerialComputeDirectPathReference : nullptr,
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
			static_cast<void *>(&detached->view) : nullptr);
}

bool ConsumeDirectPathReference(const std::shared_ptr<DirectPathBatchWork> &work,
	JobSystem &jobs, PathPerformanceInterval &schedule)
{
	using namespace performance;
	DirectPathBatchWork &batch = *work;
	if (!InlineBodyReadiness::direct(*batch.reference, jobs))
		return false;
	DirectPathSourceRecord &source = *batch.reference;
	KernelPerformanceReferenceLedger &ledger = *source.ledger;
	KernelPerformanceAttemptDecision decision = {};
	if (!ledger.replayDecision(source.attempt, 1, true, source.facts, decision) ||
		decision.admission != KERNEL_ADMISSION_ACCEPTED ||
		decision.reasonSchema != 1 || decision.reason != 1 ||
		decision.sourceConfiguredWorkers != source.workers)
	{
		source.failed = true;
		return false;
	}
	source.admitted = true;
	batch.referenceAdmissionAccepted = true;
	const KernelPerformanceDispatchPlan dispatch = {1, 1, 1,
		static_cast<unsigned>(batch.requestCount),
		static_cast<JobMetricCounter>(batch.requestCount), 1, source.workers};
	if (!ledger.observeDispatch(source.attempt, dispatch))
	{
		source.failed = true;
		return false;
	}
	for (unsigned i = 0; i < batch.requestCount; ++i)
	{
		const KernelPerformanceRangePlan plan = {1, i, 0, i, i + 1, 1};
		if (!ledger.observeRangePlan(source.attempt, plan))
		{
			source.failed = true;
			return false;
		}
	}
	source.planned = true;
	schedule.end();
	bool allSucceeded = true;
	for (unsigned i = 0; i < batch.requestCount; ++i)
	{
		const KernelPerformanceRangePlan plan = {1, i, 0, i, i + 1, 1};
		if (!InlineBodyReadiness::direct(source, jobs))
		{
			source.failed = true;
			return false;
		}
		const KernelPerformanceInlineAction action = ledger.beginInlineBody(
			source.attempt, plan, KernelPerformanceLedger::instance(),
			source.inlineBody, source.checkpoints[i]);
		if (action == KERNEL_INLINE_INVALID)
		{
			source.failed = true;
			return false;
		}
		bool succeeded = action == KERNEL_INLINE_SKIP_SOURCE_NEVER_ENTERED;
		if (action == KERNEL_INLINE_EXECUTE)
		{
			try
			{
				succeeded = ExecuteDirectPathBody(work, i, nullptr);
			}
			catch (...)
			{
				succeeded = false;
			}
		}
		const KernelPerformanceRangeProgress progress =
			DirectPathReleasedProgress(batch, i);
		if (source.failed ||
			(action == KERNEL_INLINE_EXECUTE &&
				!ledger.finishInlineBody(source.inlineBody, progress)))
		{
			source.failed = true;
			return false;
		}
		source.inlineBody = KernelPerformanceInlineBody();
		if (!ledger.observeReleasedRange(source.attempt, plan, progress))
		{
			source.failed = true;
			return false;
		}
		allSucceeded = allSucceeded && succeeded;
	}
	source.collected = true;
	KernelPerformanceAttemptFinish sourceFinish = {};
	if (!ledger.readSourceFinish(source.attempt, sourceFinish) ||
		(sourceFinish.disposition != KERNEL_PERFORMANCE_COMMITTED &&
		 sourceFinish.disposition != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION) ||
		(sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED &&
		 !sourceFinish.validationObserved))
	{
		source.failed = true;
		return false;
	}
	source.sourceOwnerCommitAllowed =
		sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED;
	return allSucceeded;
}
#endif

void releaseDirectPathActiveSlot(DirectPathBatchWork &batch)
{
	if (batch.ownsActiveSlot.exchange(false, std::memory_order_acq_rel))
		s_activeDirectPathBatches.fetch_sub(1, std::memory_order_acq_rel);
}

void publishDirectPathCancellation(DirectPathWork &work)
{
	unsigned state = work.executionState.load(std::memory_order_acquire);
	while ((state == DIRECT_PATH_WORK_PENDING ||
		state == DIRECT_PATH_WORK_RUNNING_WORKER ||
		state == DIRECT_PATH_WORK_RUNNING_OWNER) &&
		!work.executionState.compare_exchange_weak(state,
			DIRECT_PATH_WORK_CANCELLED, std::memory_order_acq_rel,
			std::memory_order_acquire))
	{
	}
}

void publishPeak(std::atomic<unsigned> &peak, unsigned value)
{
	unsigned previous = peak.load(std::memory_order_acquire);
	while (previous < value && !peak.compare_exchange_weak(previous, value,
		std::memory_order_acq_rel, std::memory_order_acquire))
	{
	}
}

class DirectPathJob final : public Job
{
public:
	DirectPathJob(const std::shared_ptr<DirectPathBatchWork> &batch,
		std::size_t requestIndex) : m_batch(batch),
		m_requestIndex(requestIndex) {}

	~DirectPathJob() override
	{
		if (m_batch->liveJobs.fetch_sub(1, std::memory_order_acq_rel) == 1)
			releaseDirectPathActiveSlot(*m_batch);
	}

	void execute(JobContext &context) override
	{
		#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseDirectPathTest(1);
		#endif
		ExecuteDirectPathBody(m_batch, m_requestIndex, &context);
	}

private:
	std::shared_ptr<DirectPathBatchWork> m_batch;
	std::size_t m_requestIndex;
};

} // namespace

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_direct_path_set_test_pause_mask(unsigned pauseMask)
{
	s_directPathTestPauseReleasedMask.store(0, std::memory_order_release);
	s_directPathTestPauseReachedMask.store(0, std::memory_order_release);
	s_directPathTestPauseReachedCount.store(0, std::memory_order_release);
	s_directPathTestPauseMask.store(pauseMask, std::memory_order_release);
}

extern "C" bool rts_direct_path_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds)
{
	return waitForDirectPathTestPause(pausePoint, 1, timeoutMilliseconds);
}

extern "C" bool rts_direct_path_wait_for_test_pause_count(
	unsigned pausePoint, unsigned requiredCount, unsigned timeoutMilliseconds)
{
	return requiredCount != 0 && waitForDirectPathTestPause(pausePoint,
		requiredCount, timeoutMilliseconds);
}

extern "C" void rts_direct_path_release_test_pause(unsigned pausePoint)
{
	s_directPathTestPauseReleasedMask.fetch_or(pausePoint,
		std::memory_order_acq_rel);
}

extern "C" void rts_direct_path_set_test_fault_mask(unsigned faultMask)
{
	s_directPathTestFaultMask.store(faultMask, std::memory_order_release);
}
#endif

struct DeterministicDirectPathBatch::State
{
	State() : requestCount(0), submittedJobCount(0), completed(false),
		timedOut(false) {}

	std::shared_ptr<DirectPathBatchWork> work;
	std::size_t requestCount;
	std::size_t submittedJobCount;
	bool completed;
	bool timedOut;
};

DeterministicDirectPathBatch::DeterministicDirectPathBatch() :
	m_state(nullptr)
{
	try
	{
		m_state = new State;
	}
	catch (...)
	{
		m_state = nullptr;
	}
}

DeterministicDirectPathBatch::~DeterministicDirectPathBatch()
{
	delete m_state;
}

bool DeterministicDirectPathBatch::executeSynchronously(JobSystem &jobs,
	const DirectPathSnapshot *snapshots, std::size_t requestCount,
	unsigned workerWaitTimeoutMilliseconds
#if defined(_WIN64)
	, performance::KernelPerformanceBatch *performanceBatch
	, performance::KernelPerformanceReferenceLedger *performanceReferenceLedger
	, performance::KernelPerformanceReferenceBatch *performanceReferenceBatch
	, performance::KernelPerformanceAttempt performanceReferenceAttempt
#endif
	)
{
	#if defined(_WIN64)
	if (performanceReferenceBatch != nullptr)
		*performanceReferenceBatch = performance::KernelPerformanceReferenceBatch();
	#endif
	if (m_state == nullptr)
		return false;
	m_state->work.reset();
	m_state->requestCount = requestCount;
	m_state->submittedJobCount = 0;
	m_state->completed = false;
	m_state->timedOut = false;
	#if defined(_WIN64)
	const bool traceRequested = performanceReferenceLedger != nullptr &&
		performanceReferenceLedger->traceRequested();
	const bool sourceBoundInline = performanceReferenceLedger != nullptr &&
		performanceReferenceLedger->runMode() ==
			performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if ((traceRequested || sourceBoundInline) &&
		!performanceReferenceAttempt.valid())
		return false;
	#else
	const bool sourceBoundInline = false;
	#endif
	if (snapshots == nullptr || requestCount < 2 ||
		requestCount > DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS ||
		workerWaitTimeoutMilliseconds == 0 || !jobs.isRunning() ||
		jobs.workerCount() == 0 || !jobs.isCurrentThread(JOB_OWNER_GAME))
	{
		return false;
	}
	unsigned expectedActiveBatches = 0;
	if (!sourceBoundInline && !s_activeDirectPathBatches.compare_exchange_strong(expectedActiveBatches,
		1, std::memory_order_acq_rel, std::memory_order_acquire))
	{
		return false;
	}
	#if defined(_WIN64)
	PathPerformanceInterval capture(performanceBatch,
		performance::KERNEL_PERFORMANCE_CAPTURE);
	#endif
	try
	{
		m_state->work = std::make_shared<DirectPathBatchWork>();
	}
	catch (...)
	{
		if (!sourceBoundInline)
			s_activeDirectPathBatches.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	DirectPathBatchWork &batch = *m_state->work;
	batch.ownsActiveSlot.store(!sourceBoundInline, std::memory_order_release);
	batch.requestCount = requestCount;

	for (std::size_t requestIndex = 0; requestIndex < requestCount;
		++requestIndex)
	{
		const DirectPathSnapshot &snapshot = snapshots[requestIndex];
		if (snapshot.callbacks == nullptr || snapshot.callbackCount == 0 ||
			snapshot.callbackCount > DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS ||
			snapshot.startNeighbors == nullptr ||
			snapshot.startNeighborCount !=
				DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT)
		{
			m_state->work.reset();
			return false;
		}
		DirectPathWork &work = batch.requests[requestIndex];
		work.snapshot = snapshot;
		for (std::size_t i = 0; i < snapshot.callbackCount; ++i)
			work.callbacks[i] = snapshot.callbacks[i];
		for (std::size_t i = 0; i < snapshot.startNeighborCount; ++i)
			work.startNeighbors[i] = snapshot.startNeighbors[i];
		work.snapshot.callbacks = work.callbacks.data();
		work.snapshot.startNeighbors = work.startNeighbors.data();
		work.result = {};
		work.result.rawPoints = work.rawPoints.data();
		work.result.rawPointCapacity = snapshot.callbackCount;
		work.executionState.store(DIRECT_PATH_WORK_PENDING,
			std::memory_order_relaxed);
		work.physicalWorkerIndex.store(JOB_INVALID_PHYSICAL_WORKER_INDEX,
			std::memory_order_relaxed);
	}
	#if defined(_WIN64)
	try
	{
		if (!PrepareDirectPathSourceRecord(batch, jobs, performanceReferenceLedger,
			performanceReferenceAttempt))
		{
			m_state->work.reset();
			return false;
		}
	}
	catch (...)
	{
		m_state->work.reset();
		return false;
	}
	#endif
	#if defined(_WIN64)
	capture.end();
	PathPerformanceInterval schedule(performanceBatch,
		performance::KERNEL_PERFORMANCE_SCHEDULE);
	#endif
	#if defined(_WIN64)
	if (sourceBoundInline)
	{
		m_state->completed = ConsumeDirectPathReference(m_state->work, jobs,
			schedule);
		if (!m_state->completed)
			return false;
		PathPerformanceInterval validate(performanceBatch,
			performance::KERNEL_PERFORMANCE_VALIDATE);
		for (std::size_t i = 0; i < batch.requestCount; ++i)
		{
			if (batch.requests[i].executionState.load(std::memory_order_acquire) !=
				DIRECT_PATH_WORK_INLINE)
			{
				m_state->completed = false;
				return false;
			}
		}
		ObserveDirectPathReference(batch, requestCount, performanceBatch,
			performanceReferenceLedger, performanceReferenceBatch);
		if (performanceReferenceBatch == nullptr ||
			!performanceReferenceBatch->valid() ||
			!batch.reference->sourceOwnerCommitAllowed)
		{
			m_state->completed = false;
			return false;
		}
		return true;
	}
	#endif
	const JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		#if defined(_WIN64)
		ObserveDirectPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	#if defined(_WIN64)
	if (batch.reference)
	{
		try
		{
			#if defined(RTS_BUILD_CORE_EXTRAS)
			if ((s_directPathTestFaultMask.load(std::memory_order_acquire) &
				DIRECT_PATH_TEST_GROUP_COPY_ALLOCATION_FAILURE) != 0)
				throw std::bad_alloc();
			#endif
			batch.reference->group = group;
		}
		catch (...)
		{
			ObserveDirectPathSourceAdmission(batch, false);
			m_state->work.reset();
			return false;
		}
	}
	#endif
	JobSubmission submissions[DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS];
	JobHandle handles[DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS];
	std::size_t allocated = 0;
	for (; allocated < requestCount; ++allocated)
	{
		submissions[allocated].job = new (std::nothrow)
			DirectPathJob(m_state->work, allocated);
		if (submissions[allocated].job == nullptr)
			break;
		submissions[allocated].priority = JOB_PRIORITY_FRAME_CRITICAL;
	}
	batch.liveJobs.store(static_cast<unsigned>(allocated),
		std::memory_order_release);
	if (allocated != requestCount)
	{
		for (std::size_t i = 0; i < allocated; ++i)
			delete submissions[i].job;
		if (allocated == 0)
			releaseDirectPathActiveSlot(batch);
		#if defined(_WIN64)
		ObserveDirectPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	if (!jobs.trySubmitBatch(submissions, static_cast<unsigned>(requestCount),
		group, handles))
	{
		for (std::size_t i = 0; i < requestCount; ++i)
			delete submissions[i].job;
		#if defined(_WIN64)
		ObserveDirectPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	m_state->submittedJobCount = requestCount;
	#if defined(_WIN64)
	ObserveDirectPathSourceAdmission(batch, true);
	schedule.end();
	PathPerformanceInterval wait(performanceBatch,
		performance::KERNEL_PERFORMANCE_WAIT);
	#endif

	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestPauseMask.load(std::memory_order_acquire) & 1) != 0)
	{
		const unsigned requiredWorkers = static_cast<unsigned>(requestCount) <
			jobs.workerCount() ? static_cast<unsigned>(requestCount) :
			jobs.workerCount();
		if (!waitForDirectPathTestPause(1, requiredWorkers, 15000))
		{
			for (std::size_t i = 0; i < requestCount; ++i)
				publishDirectPathCancellation(batch.requests[i]);
			jobs.cancel(group);
			return false;
		}
	}
	#endif
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestPauseMask.load(std::memory_order_acquire) & 2) != 0)
	{
		const unsigned requiredWorkers = static_cast<unsigned>(requestCount) <
			jobs.workerCount() ? static_cast<unsigned>(requestCount) :
			jobs.workerCount();
		if (!waitForDirectPathTestPause(2, requiredWorkers, 15000))
		{
			for (std::size_t i = 0; i < requestCount; ++i)
				publishDirectPathCancellation(batch.requests[i]);
			jobs.cancel(group);
			return false;
		}
	}
	#endif

	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() +
		std::chrono::milliseconds(workerWaitTimeoutMilliseconds);
	while (!group.isComplete() && jobs.isRunning() &&
		jobs.workerCount() != 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	bool schedulerRunning = jobs.isRunning();
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestFaultMask.load(std::memory_order_acquire) & 2) != 0)
		schedulerRunning = false;
	#endif
	if (!group.isComplete() || !schedulerRunning)
	{
		for (std::size_t i = 0; i < requestCount; ++i)
			publishDirectPathCancellation(batch.requests[i]);
		jobs.cancel(group);
		m_state->timedOut = true;
		return false;
	}

	// A completed group has no runnable job for the owner to help.  This is
	// the batch's one join and cannot create an owner execution identity.
	const bool joined = jobs.wait(group);
	#if defined(_WIN64)
	wait.end();
	const bool collected = !batch.reference ||
		CollectDirectPathSourceRecord(batch, jobs);
	#else
	const bool collected = true;
	#endif
	releaseDirectPathActiveSlot(batch);
	m_state->completed = joined && collected && !group.failed() &&
		!group.wasCancelled();
	if (!m_state->completed)
		return false;
	#if defined(_WIN64)
	PathPerformanceInterval validate(performanceBatch,
		performance::KERNEL_PERFORMANCE_VALIDATE);
	#endif
	for (std::size_t i = 0; i < requestCount; ++i)
	{
		if (!handles[i].succeeded() ||
			batch.requests[i].executionState.load(std::memory_order_acquire) !=
				DIRECT_PATH_WORK_WORKER)
		{
			m_state->completed = false;
			return false;
		}
	}
	#if defined(_WIN64)
	ObserveDirectPathReference(batch, requestCount, performanceBatch,
		performanceReferenceLedger, performanceReferenceBatch);
	if (batch.reference && (performanceReferenceBatch == nullptr ||
		!performanceReferenceBatch->valid()))
	{
		m_state->completed = false;
		return false;
	}
	#endif
	return true;
}

#if defined(_WIN64)
bool DeterministicDirectPathBatch::collectPerformanceReference(JobSystem &jobs)
{
	return m_state != nullptr && m_state->work != nullptr &&
		CollectDirectPathSourceRecord(*m_state->work, jobs);
}
#endif

DeterministicDirectPathBatchExecutionSnapshot
DeterministicDirectPathBatch::executionSnapshot() const
{
	DeterministicDirectPathBatchExecutionSnapshot snapshot = {};
	if (m_state == nullptr)
		return snapshot;
	snapshot.requestCount = m_state->requestCount;
	snapshot.submittedJobCount = m_state->submittedJobCount;
	snapshot.referenceAdmissionAccepted = m_state->work != nullptr &&
		m_state->work->referenceAdmissionAccepted;
	snapshot.completed = m_state->completed;
	snapshot.timedOut = m_state->timedOut;
	if (m_state->work == nullptr)
		return snapshot;
	const DirectPathBatchWork &batch = *m_state->work;
	unsigned workerIndices[DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS];
	unsigned distinctWorkerCount = 0;
	for (std::size_t i = 0; i < m_state->submittedJobCount; ++i)
	{
		const unsigned state = batch.requests[i].executionState.load(
			std::memory_order_acquire);
		if (state == DIRECT_PATH_WORK_WORKER)
		{
			++snapshot.workerExecutedJobCount;
			const unsigned workerIndex = batch.requests[i].physicalWorkerIndex.load(
				std::memory_order_acquire);
			unsigned previous = 0;
			while (previous < distinctWorkerCount &&
				workerIndices[previous] != workerIndex)
			{
				++previous;
			}
			if (previous == distinctWorkerCount &&
				workerIndex != JOB_INVALID_PHYSICAL_WORKER_INDEX)
			{
				workerIndices[distinctWorkerCount++] = workerIndex;
			}
		}
		else if (state == DIRECT_PATH_WORK_OWNER)
		{
			++snapshot.ownerExecutedJobCount;
		}
		else if (state == DIRECT_PATH_WORK_FAILURE ||
			state == DIRECT_PATH_WORK_CANCELLED)
		{
			++snapshot.failedJobCount;
		}
	}
	snapshot.distinctPhysicalWorkerCount = distinctWorkerCount;
	snapshot.peakActiveWorkers = batch.peakActiveWorkers.load(
		std::memory_order_acquire);
	return snapshot;
}

DeterministicDirectPathExecutionSnapshot
DeterministicDirectPathBatch::requestExecutionSnapshot(
	std::size_t requestIndex) const
{
	DeterministicDirectPathExecutionSnapshot snapshot = {
		DIRECT_PATH_EXECUTION_PENDING, JOB_INVALID_PHYSICAL_WORKER_INDEX,
		false, false
	};
	if (m_state == nullptr || m_state->work == nullptr ||
		requestIndex >= m_state->requestCount)
	{
		return snapshot;
	}
	snapshot.submitted = requestIndex < m_state->submittedJobCount;
	const DirectPathWork &work = m_state->work->requests[requestIndex];
	const unsigned state = work.executionState.load(std::memory_order_acquire);
	if (state == DIRECT_PATH_WORK_CANCELLED)
		snapshot.state = DIRECT_PATH_EXECUTION_CANCELLED;
	else if (state == DIRECT_PATH_WORK_WORKER)
		snapshot.state = DIRECT_PATH_EXECUTION_WORKER;
	else if (state == DIRECT_PATH_WORK_OWNER)
		snapshot.state = DIRECT_PATH_EXECUTION_OWNER;
	#if defined(_WIN64)
	else if (state == DIRECT_PATH_WORK_INLINE)
		snapshot.state = DIRECT_PATH_EXECUTION_INLINE;
	#endif
	else if (state == DIRECT_PATH_WORK_FAILURE)
		snapshot.state = DIRECT_PATH_EXECUTION_FAILURE;
	if (snapshot.state == DIRECT_PATH_EXECUTION_WORKER)
	{
		snapshot.physicalWorkerIndex = work.physicalWorkerIndex.load(
			std::memory_order_acquire);
	}
	snapshot.succeeded = m_state->completed &&
		(snapshot.state == DIRECT_PATH_EXECUTION_WORKER
	#if defined(_WIN64)
			|| snapshot.state == DIRECT_PATH_EXECUTION_INLINE
	#endif
		);
	return snapshot;
}

const DirectPathSearchResult &DeterministicDirectPathBatch::result(
	std::size_t requestIndex) const
{
	static const DirectPathSearchResult invalidResult = {};
	if (m_state == nullptr || m_state->work == nullptr ||
		!m_state->completed || requestIndex >= m_state->requestCount ||
		m_state->work->requests[requestIndex].executionState.load(
			std::memory_order_acquire) != DIRECT_PATH_WORK_WORKER
	#if defined(_WIN64)
		&& m_state->work->requests[requestIndex].executionState.load(
			std::memory_order_acquire) != DIRECT_PATH_WORK_INLINE
	#endif
		)
	{
		return invalidResult;
	}
	return m_state->work->requests[requestIndex].result;
}

unsigned GetDeterministicDirectPathLateDrainExecutionCount()
{
	return s_directPathLateDrainExecutions.load(std::memory_order_acquire);
}

std::uint64_t ComputeDeterministicOrdinaryPathPlanHash(
	const DeterministicPathPoint *points, std::size_t pointCount,
	const std::uint32_t *allocationOrder, std::size_t allocationCount,
	const std::uint32_t *cleanupOrder, std::size_t cleanupCount,
	const std::uint32_t *passableBlockIndices, std::size_t passableBlockCount,
	bool hierarchyAllPassable,
	std::uint32_t snapshotGeneration, std::uint32_t objectId,
	std::uint64_t ownerToken) noexcept
{
	if ((pointCount != 0 && points == nullptr) ||
		(allocationCount != 0 && allocationOrder == nullptr) ||
		(cleanupCount != 0 && cleanupOrder == nullptr) ||
		(passableBlockCount != 0 && passableBlockIndices == nullptr))
	{
		return 0;
	}
	std::uint64_t hash = UINT64_C(1469598103934665603);
	const auto mix = [&](std::uint64_t value)
	{
		hash ^= value;
		hash *= UINT64_C(1099511628211);
	};
	mix(snapshotGeneration);
	mix(objectId);
	mix(ownerToken);
	mix(pointCount);
	for (std::size_t i = 0; i < pointCount; ++i)
	{
		mix(static_cast<std::uint32_t>(points[i].x));
		mix(static_cast<std::uint32_t>(points[i].y));
		mix(points[i].layer);
	}
	mix(allocationCount);
	for (std::size_t i = 0; i < allocationCount; ++i)
		mix(allocationOrder[i]);
	mix(cleanupCount);
	for (std::size_t i = 0; i < cleanupCount; ++i)
		mix(cleanupOrder[i]);
	mix(hierarchyAllPassable ? 1U : 0U);
	mix(passableBlockCount);
	for (std::size_t i = 0; i < passableBlockCount; ++i)
		mix(passableBlockIndices[i]);
	return hash == 0 ? 1 : hash;
}

namespace
{

const std::size_t ORDINARY_PATH_MAX_SCRATCH_BYTES =
	static_cast<std::size_t>(256) * 1024U * 1024U;
const std::size_t ORDINARY_PATH_MAX_RESULT_BYTES =
	static_cast<std::size_t>(128) * 1024U * 1024U;
const unsigned ORDINARY_PATH_TEST_ENTRY_PAUSE = 4U;
const unsigned ORDINARY_PATH_TEST_ACTIVE_PAUSE = 8U;
const unsigned ORDINARY_PATH_TEST_EXECUTION_FAILURE = 4U;
const unsigned ORDINARY_PATH_TEST_SCHEDULER_STOPPED = 8U;
const unsigned ORDINARY_PATH_TEST_SOURCE_COLLECTION_FAILURE = 16U;
const unsigned ORDINARY_PATH_TEST_DISPATCH_VECTOR_ALLOCATION_FAILURE = 256U;

std::atomic<unsigned> s_activeOrdinaryPathBatches(0);
std::atomic<unsigned> s_ordinaryPathLateDrainExecutions(0);

struct OrdinaryPathRequestWork
{
	OrdinaryPathRequestWork() : ownerToken(0), materializationPlanHash(0),
		executionState(DIRECT_PATH_WORK_PENDING),
		physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX)
	{
		request = {};
		result = {};
	}

	DeterministicPathRequest request;
	std::uint64_t ownerToken;
	std::uint64_t materializationPlanHash;
	DeterministicPathSearchResult result;
	std::vector<DeterministicPathPoint> points;
	std::vector<std::uint32_t> allocationOrder;
	std::vector<std::uint32_t> cleanupOrder;
	std::vector<std::uint32_t> passableBlocks;
	std::atomic<unsigned> executionState;
	std::atomic<unsigned> physicalWorkerIndex;
};

struct OrdinaryPathRangeWork
{
	OrdinaryPathRangeWork() : begin(0), end(0),
		executionState(DIRECT_PATH_WORK_PENDING),
		physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX) {}

	std::size_t begin;
	std::size_t end;
	std::vector<DeterministicPathSearchNode> nodes;
	std::vector<std::uint32_t> heap;
	std::vector<DeterministicPathPoint> pointScratch;
	std::vector<std::uint8_t> hierarchyPassableScratch;
	std::vector<std::uint32_t> hierarchyBlockScratch;
	std::atomic<unsigned> executionState;
	std::atomic<unsigned> physicalWorkerIndex;
};

#if defined(_WIN64)
struct OrdinaryPathSourceRecord
{
	OrdinaryPathSourceRecord() : ledger(nullptr), workers(0), pending(0),
		outstanding(0), baseline(false), admitted(false), planned(false), collected(false), failed(false),
		sourceOwnerCommitAllowed(false) {}

	performance::KernelPerformanceReferenceLedger *ledger;
	performance::KernelPerformanceAttempt attempt;
	performance::KernelPerformanceDigest facts;
	std::unique_ptr<performance::KernelPerformanceCheckpointProbe[]> checkpoints;
	std::unique_ptr<performance::KernelPerformanceRequestBudget[]> budgets;
	JobGroup group;
	performance::KernelPerformanceInlineBody inlineBody;
	performance::KernelPerformanceInlineOwnerSerial materialization;
	unsigned workers;
	JobMetricCounter pending, outstanding;
	bool baseline, admitted, planned, collected, failed;
	bool sourceOwnerCommitAllowed;
};

bool InlineBodyReadiness::ordinary(const OrdinaryPathSourceRecord &source,
	const JobSystem &jobs)
{
	return source.ledger != nullptr && source.attempt.valid() &&
		source.baseline && !source.failed && !source.inlineBody.valid() &&
		!source.materialization.valid() && jobs.isCurrentThread(JOB_OWNER_GAME);
}

bool ReapReadiness::ordinary(const OrdinaryPathSourceRecord &source,
	const JobSystem &jobs)
{
	return source.ledger != nullptr && source.attempt.valid() &&
		!source.baseline && jobs.isCurrentThread(JOB_OWNER_GAME) &&
		source.group.isValid() && source.group.isComplete();
}
#endif

struct OrdinaryPathBatchWork
{
	OrdinaryPathBatchWork() : requestCount(0), rangeCount(0), grainSize(0),
		activeWorkers(0), peakActiveWorkers(0), liveJobs(0),
		resultStorageBytes(0), ownsActiveSlot(false),
		referenceAdmissionAccepted(false)
	{}

	~OrdinaryPathBatchWork()
	{
		if (ownsActiveSlot.exchange(false, std::memory_order_acq_rel))
			s_activeOrdinaryPathBatches.fetch_sub(1, std::memory_order_acq_rel);
	}

	std::vector<DeterministicPathCell> cells;
	ImmutableNavigationGrid grid;
	std::unique_ptr<OrdinaryPathRequestWork[]> requests;
	std::unique_ptr<OrdinaryPathRangeWork[]> ranges;
	std::size_t requestCount;
	unsigned rangeCount;
	unsigned grainSize;
	std::atomic<unsigned> activeWorkers;
	std::atomic<unsigned> peakActiveWorkers;
	std::atomic<unsigned> liveJobs;
	std::atomic<std::size_t> resultStorageBytes;
	std::atomic<bool> ownsActiveSlot;
	bool referenceAdmissionAccepted;
#if defined(_WIN64)
	DeterministicOrdinaryPathTestHooks testHooks;
	// Separate trace-only storage preserves the ordinary request-size admission bound.
	std::unique_ptr<OrdinaryPathSourceRecord> reference;
#endif
};

#if defined(_WIN64)
void ObserveOrdinaryPathTestRequest(const OrdinaryPathBatchWork &batch,
	DeterministicOrdinaryPathTestEvent site, unsigned rangeIndex,
	std::size_t requestIndex, std::size_t actualBytes = 0, bool actualGranted = false)
{
	if (batch.testHooks.observeRequest != nullptr)
		batch.testHooks.observeRequest(batch.testHooks.context, site,
			rangeIndex, requestIndex, actualBytes, actualGranted);
}

void ObserveOrdinaryPathMaterializationTest(const OrdinaryPathBatchWork &batch,
	const OrdinaryPathRangeWork &range, const OrdinaryPathRequestWork &work,
	DeterministicOrdinaryPathTestEvent site, std::size_t actualBytes = 0,
	bool actualGranted = false)
{
	ObserveOrdinaryPathTestRequest(batch, site,
		static_cast<unsigned>(&range - batch.ranges.get()),
		static_cast<std::size_t>(&work - batch.requests.get()), actualBytes, actualGranted);
}

class OrdinaryPathMaterializationTestScope
{
public:
	OrdinaryPathMaterializationTestScope(const OrdinaryPathBatchWork &batch,
		const OrdinaryPathRangeWork &range, const OrdinaryPathRequestWork &work) :
		m_batch(batch), m_range(range), m_work(work)
	{
		ObserveOrdinaryPathMaterializationTest(m_batch, m_range, m_work,
			DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_ENTER);
	}
	~OrdinaryPathMaterializationTestScope()
	{
		ObserveOrdinaryPathMaterializationTest(m_batch, m_range, m_work,
			DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_EXIT);
	}
private:
	const OrdinaryPathBatchWork &m_batch;
	const OrdinaryPathRangeWork &m_range;
	const OrdinaryPathRequestWork &m_work;
};

// The same native body records on a physical worker or advances an already
// authenticated inline probe. Physical POD import follows the real group fence.
class OrdinaryPathSourceCheckpointScope
{
public:
	OrdinaryPathSourceCheckpointScope(OrdinaryPathBatchWork &batch, unsigned range) :
		m_probe(batch.reference ? &batch.reference->checkpoints[range] : nullptr),
		m_range(range), m_begin(batch.ranges[range].begin), m_completed(0),
		m_last{1, range, m_begin}, m_cancelled(false), m_succeeded(false)
	{
		if (m_probe != nullptr && !batch.reference->baseline) m_probe->beginRecord();
	}
	~OrdinaryPathSourceCheckpointScope()
	{
		if (m_probe == nullptr) return;
		const performance::KernelPerformanceCheckpoint end = {3, m_range, m_begin + m_completed};
		m_probe->finish(m_cancelled ? m_last : end, m_completed,
			m_cancelled ? performance::KERNEL_RANGE_CANCELLED : m_succeeded ?
			performance::KERNEL_RANGE_COMPLETED : performance::KERNEL_RANGE_FAILED);
	}
	bool cancelled(unsigned site, std::size_t request, bool actual)
	{
		m_last = {site, m_range, request};
		m_cancelled = m_probe != nullptr ? m_probe->cancelled(m_last, actual) : actual;
		return m_cancelled;
	}
	void completedRequest() { ++m_completed; }
	void finish(bool succeeded) { m_succeeded = succeeded; }
private:
	performance::KernelPerformanceCheckpointProbe *m_probe;
	unsigned m_range;
	std::size_t m_begin, m_completed;
	performance::KernelPerformanceCheckpoint m_last;
	bool m_cancelled, m_succeeded;
};
#endif

void releaseOrdinaryPathActiveSlot(OrdinaryPathBatchWork &batch)
{
	if (batch.ownsActiveSlot.exchange(false, std::memory_order_acq_rel))
		s_activeOrdinaryPathBatches.fetch_sub(1, std::memory_order_acq_rel);
}

void publishOrdinaryPathCancellation(OrdinaryPathRangeWork &range)
{
	unsigned state = range.executionState.load(std::memory_order_acquire);
	while ((state == DIRECT_PATH_WORK_PENDING ||
		state == DIRECT_PATH_WORK_RUNNING_WORKER ||
		state == DIRECT_PATH_WORK_RUNNING_OWNER) &&
		!range.executionState.compare_exchange_weak(state,
			DIRECT_PATH_WORK_CANCELLED, std::memory_order_acq_rel,
			std::memory_order_acquire))
	{
	}
}

bool reserveOrdinaryPathResultStorage(OrdinaryPathBatchWork &batch,
	std::size_t byteCount)
{
	std::size_t current = batch.resultStorageBytes.load(std::memory_order_acquire);
	while (current <= ORDINARY_PATH_MAX_RESULT_BYTES &&
		byteCount <= ORDINARY_PATH_MAX_RESULT_BYTES - current)
	{
		if (batch.resultStorageBytes.compare_exchange_weak(current,
			current + byteCount, std::memory_order_acq_rel,
			std::memory_order_acquire))
		{
			return true;
		}
	}
	return false;
}

bool hasInitialCellInfo(const OrdinaryPathBatchWork &batch,
	std::uint32_t cellIndex)
{
	return (batch.cells[cellIndex].navigationFlags &
		DETERMINISTIC_PATH_HAS_CELL_INFO) != 0;
}

#if defined(_WIN64)
struct OrdinaryPathReferenceInput
{
	const OrdinaryPathBatchWork *batch;
	std::size_t requestCount;
	std::size_t cellCount;
	std::size_t hierarchyBlockCount;
};

struct OrdinaryPathReferenceResultView
{
	const DeterministicPathPoint *points;
	std::size_t pointCount;
	const std::uint32_t *allocationOrder;
	std::size_t allocationCount;
	const std::uint32_t *cleanupOrder;
	std::size_t cleanupCount;
	const std::uint32_t *passableBlockIndices;
	std::size_t passableBlockCount;
	std::uint32_t snapshotGeneration;
	std::uint32_t objectId;
	std::uint32_t expandedNodeCount;
	std::uint32_t discoveredNodeCount;
	std::uint32_t requiredCellInfoCount;
	std::uint32_t cumulativeCellCount;
	std::uint64_t ownerToken;
	std::uint64_t materializationPlanHash;
	bool hierarchyAllPassable;
	DeterministicPathSearchStatus status;
};

struct OrdinaryPathReferenceOutputView
{
	const OrdinaryPathReferenceResultView *results;
	std::size_t count;
	const void *detachedStorage;
};

struct OrdinaryPathReferenceDetachedOperation
{
	OrdinaryPathReferenceDetachedOperation() : result(), view(),
		materializationPlanHash(0)
	{
		result = {};
		view = {};
	}

	DeterministicPathSearchResult result;
	std::vector<DeterministicPathPoint> points;
	std::vector<std::uint32_t> passableBlocks;
	std::vector<std::uint32_t> allocationOrder;
	std::vector<std::uint32_t> cleanupOrder;
	OrdinaryPathReferenceResultView view;
	std::uint64_t materializationPlanHash;
};

struct OrdinaryPathReferenceDetachedOutput
{
	OrdinaryPathReferenceDetachedOutput() : view(), count(0)
	{
		view.results = nullptr;
		view.count = 0;
		view.detachedStorage = this;
	}

	OrdinaryPathReferenceOutputView view;
	std::size_t count;
	std::vector<OrdinaryPathReferenceResultView> results;
	std::vector<OrdinaryPathReferenceDetachedOperation> operations;
	std::vector<DeterministicPathSearchNode> nodes;
	std::vector<std::uint32_t> heap;
	std::vector<std::uint8_t> hierarchyPassable;
	std::vector<std::uint32_t> discovered;
	std::vector<std::uint32_t> open;
	std::vector<std::uint32_t> closed;
};

struct OrdinaryPathReferenceBundle
{
	OrdinaryPathReferenceBundle() : input(), production(), productionResults(),
		detached()
	{
		input = {};
		production = {};
	}

	OrdinaryPathReferenceInput input;
	OrdinaryPathReferenceOutputView production;
	std::vector<OrdinaryPathReferenceResultView> productionResults;
	std::unique_ptr<OrdinaryPathReferenceDetachedOutput> detached;
};

bool WriteOrdinaryPathReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const OrdinaryPathReferenceInput &input =
		*static_cast<const OrdinaryPathReferenceInput *>(context);
	if (input.batch == nullptr || input.requestCount == 0 ||
		input.batch->requestCount != input.requestCount ||
		input.batch->grid.cells == nullptr ||
		input.cellCount != input.batch->cells.size() ||
		!writer.u32(1, input.batch->grid.width) ||
		!writer.u32(2, input.batch->grid.height) ||
		!writer.i32(3, input.batch->grid.originX) ||
		!writer.i32(4, input.batch->grid.originY) ||
		!writer.u32(5, input.batch->grid.snapshotGeneration) ||
		!writer.u32(6, static_cast<unsigned>(input.hierarchyBlockCount)) ||
		!writer.sequence(10, static_cast<unsigned>(input.cellCount)))
		return false;
	for (std::size_t index = 0; index < input.cellCount; ++index)
	{
		const DeterministicPathCell &cell = input.batch->cells[index];
		if (!writer.u32(11, cell.traversalMask) ||
			!writer.u32(12, cell.obstacleObjectId) ||
			!writer.u32(13, cell.positionObjectId) ||
			!writer.u32(14, cell.goalObjectId) ||
			!writer.u32(15, cell.blockZone) ||
			!writer.u32(16, cell.globalZone) ||
			!writer.u32(17, cell.zone) ||
			!writer.u32(18, cell.type) || !writer.u32(19, cell.flags) ||
			!writer.u32(20, cell.layer) ||
			!writer.u32(21, cell.connectsToLayer) ||
			!writer.u32(22, cell.pinched) ||
			!writer.u32(23, cell.blockPassable) ||
			!writer.u32(24, cell.navigationFlags))
			return false;
	}
	if (!writer.sequence(30, static_cast<unsigned>(input.requestCount)))
		return false;
	for (std::size_t index = 0; index < input.requestCount; ++index)
	{
		const OrdinaryPathRequestWork &work = input.batch->requests[index];
		const DeterministicPathRequest &request = work.request;
		if (!writer.u32(31, request.expectedSnapshotGeneration) ||
			!writer.u32(32, request.objectId) ||
			!writer.i32(33, request.startX) ||
			!writer.i32(34, request.startY) ||
			!writer.i32(35, request.goalX) ||
			!writer.i32(36, request.goalY) ||
			!writer.u32(37, request.traversalMask) ||
			!writer.u32(38, request.maximumExpandedNodes) ||
			!writer.u32(39, request.availableCellInfoCount) ||
			!writer.u32(40, request.requiredZone) ||
			!writer.u32(41, request.footprintRadius) ||
			!writer.u32(42, request.centerInCell) ||
			!writer.u32(43, request.allowDiagonal) ||
			!writer.u32(44, request.allowBlockedStart) ||
			!writer.u32(45, request.expectedLayer) ||
			!writer.u32(46, request.requireLegacyDirectLine) ||
			!writer.u32(47, request.requireObstructedSearch) ||
			!writer.u32(48, request.isHuman) ||
			!writer.u32(49, request.hierarchyMode) ||
			!writer.u32(50, request.hierarchyBlockSize) ||
			!writer.u64(51, work.ownerToken))
			return false;
	}
	return true;
}

bool PrepareOrdinaryPathSourceRecord(OrdinaryPathBatchWork &batch,
	std::size_t hierarchyBlockCount, JobSystem &jobs,
	performance::KernelPerformanceReferenceLedger *ledger,
	performance::KernelPerformanceAttempt attempt)
{
	if (ledger == nullptr) return true;
	if (!attempt.valid()) return !ledger->traceRequested();
	const bool baseline = ledger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if (!baseline && ledger->runMode() != performance::KERNEL_REFERENCE_THROUGHPUT_BINDING) return false;
	std::unique_ptr<OrdinaryPathSourceRecord> source(new OrdinaryPathSourceRecord());
	source->ledger = ledger; source->attempt = attempt;
	source->baseline = baseline;
	source->workers = jobs.workerCount();
	source->pending = jobs.pendingOwnerCompletionCount();
	source->outstanding = jobs.outstandingJobCount();
	source->checkpoints.reset(new performance::KernelPerformanceCheckpointProbe[batch.rangeCount]);
	source->budgets.reset(new performance::KernelPerformanceRequestBudget[batch.requestCount]);
	for (std::size_t i = 0; i != batch.requestCount; ++i)
		source->budgets[i].requestOrdinal = i;
	performance::KernelPerformanceCanonicalWriter facts;
	if (!facts.begin(1) || !facts.u64(1, batch.requestCount) ||
		!facts.u64(2, batch.cells.size()) || !facts.u64(3, hierarchyBlockCount) ||
		!facts.u64(4, ORDINARY_PATH_MAX_SCRATCH_BYTES) ||
		!facts.u64(5, ORDINARY_PATH_MAX_RESULT_BYTES)) return false;
	source->facts = facts.finish();
	const OrdinaryPathReferenceInput input = {&batch, batch.requestCount,
		batch.cells.size(), hierarchyBlockCount};
	if (!source->facts.valid || !ledger->bindCapturedInput(attempt, 1,
		batch.requestCount, WriteOrdinaryPathReferenceInput, &input)) return false;
	batch.reference = std::move(source);
	return true;
}

void ObserveOrdinaryPathSourceAdmission(OrdinaryPathBatchWork &batch, bool admitted)
{
	if (!batch.reference) return;
	OrdinaryPathSourceRecord &source = *batch.reference;
	source.admitted = admitted;
	performance::KernelPerformanceAttemptDecision decision = {};
	decision.site = 1; decision.reasonSchema = 1; decision.reason = admitted ? 1 : 2;
	decision.deterministicEligible = true; decision.deterministicFacts = source.facts;
	decision.admission = admitted ? performance::KERNEL_ADMISSION_ACCEPTED :
		performance::KERNEL_ADMISSION_REFUSED;
	decision.sourceConfiguredWorkers = source.workers;
	decision.dynamicFactsKnownMask = 7;
	decision.pendingJobs = source.pending; decision.outstandingJobs = source.outstanding;
	// The existing active-slot CAS succeeded from zero before capture.
	decision.activeSlots = 0;
	if (!source.ledger->observeDecision(source.attempt, decision))
	{ source.failed = true; return; }
	if (!admitted) return;
	batch.referenceAdmissionAccepted = true;
	const performance::KernelPerformanceDispatchPlan dispatch = {1, 2, 1,
		batch.rangeCount, batch.requestCount, batch.grainSize, source.workers};
	if (!source.ledger->observeDispatch(source.attempt, dispatch))
	{ source.failed = true; return; }
	for (unsigned i = 0; i != batch.rangeCount; ++i)
	{
		const OrdinaryPathRangeWork &range = batch.ranges[i];
		const performance::KernelPerformanceRangePlan plan = {1, i, 0,
			range.begin, range.end, range.end - range.begin};
		if (!source.ledger->observeRangePlan(source.attempt, plan))
		{ source.failed = true; return; }
	}
	source.planned = true;
}

performance::KernelPerformanceRangeProgress OrdinaryPathReleasedProgress(
	const OrdinaryPathBatchWork &batch, unsigned index)
{
	performance::KernelPerformanceRangeProgress progress = {};
	progress.checkpoint = batch.reference->checkpoints[index].snapshot();
	progress.publication = !progress.checkpoint.entered ? performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
		batch.reference->group.wasCancelled() || progress.checkpoint.terminal == performance::KERNEL_RANGE_CANCELLED ?
		performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL :
		progress.checkpoint.terminal == performance::KERNEL_RANGE_COMPLETED ?
		performance::KERNEL_PUBLICATION_PUBLISHED : performance::KERNEL_PUBLICATION_REJECTED;
	return progress;
}

bool ImportOrdinaryPathReferenceRange(OrdinaryPathBatchWork &batch, unsigned index,
	const performance::KernelPerformanceRangeProgress &progress)
{
	OrdinaryPathSourceRecord &source = *batch.reference;
	const OrdinaryPathRangeWork &range = batch.ranges[index];
	const performance::KernelPerformanceRangePlan plan = {1, index, 0,
		range.begin, range.end, range.end - range.begin};
	for (std::size_t request = range.begin; request != range.end; ++request)
	{
		const performance::KernelPerformanceRequestBudget &budget = source.budgets[request];
		if (!source.ledger->observeReleasedRequestBudget(source.attempt, plan, budget))
		{ source.failed = true; return false; }
		if (batch.testHooks.observeReleasedBudget != nullptr)
			batch.testHooks.observeReleasedBudget(batch.testHooks.context, plan, budget);
	}
	if (!source.ledger->observeReleasedRange(source.attempt, plan, progress))
	{ source.failed = true; return false; }
	if (batch.testHooks.observeReleasedRange != nullptr)
		batch.testHooks.observeReleasedRange(batch.testHooks.context, plan, progress);
	return true;
}

bool CollectOrdinaryPathSourceRecord(OrdinaryPathBatchWork &batch, JobSystem &jobs)
{
	if (!jobs.isCurrentThread(JOB_OWNER_GAME) || !batch.reference) return false;
	OrdinaryPathSourceRecord &source = *batch.reference;
	#if defined(RTS_BUILD_CORE_EXTRAS)
	const bool forceReportedFailure =
		(s_directPathTestFaultMask.load(std::memory_order_acquire) &
		 ORDINARY_PATH_TEST_SOURCE_COLLECTION_FAILURE) != 0;
	#else
	const bool forceReportedFailure = false;
	#endif
	if (source.failed) return false;
	if (source.collected) return true;
	if (source.baseline || !source.admitted || !source.planned ||
		!ReapReadiness::ordinary(source, jobs)) return false;
	for (unsigned i = 0; i != batch.rangeCount; ++i)
		if (!ImportOrdinaryPathReferenceRange(batch, i, OrdinaryPathReleasedProgress(batch, i))) return false;
	source.collected = true;
	// The injected failure exercises propagation after terminal source evidence
	// has been collected.  It must not strand the admitted attempt or erase the
	// already-observed range releases needed for an authenticated reap.
	return !forceReportedFailure;
}

bool WriteOrdinaryPathReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const OrdinaryPathReferenceOutputView &output =
		*static_cast<const OrdinaryPathReferenceOutputView *>(context);
	if (output.results == nullptr || output.count == 0 ||
		!writer.sequence(60, static_cast<unsigned>(output.count)))
		return false;
	for (std::size_t index = 0; index < output.count; ++index)
	{
		const OrdinaryPathReferenceResultView &result = output.results[index];
		if (!writer.u32(61, static_cast<unsigned>(result.status)) ||
			!writer.u32(62, result.snapshotGeneration) ||
			!writer.u32(63, result.objectId) ||
			!writer.u32(64, result.expandedNodeCount) ||
			!writer.u32(65, result.discoveredNodeCount) ||
			!writer.u32(66, result.requiredCellInfoCount) ||
			!writer.u32(67, result.cumulativeCellCount) ||
			!writer.u64(68, result.ownerToken) ||
			!writer.boolean(69, result.hierarchyAllPassable) ||
			!writer.u64(70, result.materializationPlanHash) ||
			!writer.sequence(71, static_cast<unsigned>(result.pointCount)))
			return false;
		for (std::size_t point = 0; point < result.pointCount; ++point)
		{
			const DeterministicPathPoint &value = result.points[point];
			if (!writer.i32(72, value.x) || !writer.i32(73, value.y) ||
				!writer.u32(74, value.layer))
				return false;
		}
		if (!writer.sequence(75, static_cast<unsigned>(result.allocationCount)))
			return false;
		for (std::size_t item = 0; item < result.allocationCount; ++item)
			if (!writer.u32(76, result.allocationOrder[item])) return false;
		if (!writer.sequence(77, static_cast<unsigned>(result.cleanupCount)))
			return false;
		for (std::size_t item = 0; item < result.cleanupCount; ++item)
			if (!writer.u32(78, result.cleanupOrder[item])) return false;
		if (!writer.sequence(79,
			static_cast<unsigned>(result.passableBlockCount)))
			return false;
		for (std::size_t item = 0; item < result.passableBlockCount; ++item)
			if (!writer.u32(80, result.passableBlockIndices[item])) return false;
	}
	return true;
}

void SetOrdinaryPathReferenceResultView(
	OrdinaryPathReferenceResultView &view,
	const DeterministicPathSearchResult &result,
	const OrdinaryPathRequestWork &work,
	const std::vector<DeterministicPathPoint> &points,
	const std::vector<std::uint32_t> &allocationOrder,
	const std::vector<std::uint32_t> &cleanupOrder,
	const std::vector<std::uint32_t> &passableBlocks,
	std::uint64_t materializationPlanHash)
{
	view.points = points.empty() ? nullptr : points.data();
	view.pointCount = points.size();
	view.allocationOrder = allocationOrder.empty() ? nullptr :
		allocationOrder.data();
	view.allocationCount = allocationOrder.size();
	view.cleanupOrder = cleanupOrder.empty() ? nullptr : cleanupOrder.data();
	view.cleanupCount = cleanupOrder.size();
	view.passableBlockIndices = passableBlocks.empty() ? nullptr :
		passableBlocks.data();
	view.passableBlockCount = passableBlocks.size();
	view.snapshotGeneration = result.snapshotGeneration;
	view.objectId = work.request.objectId;
	view.expandedNodeCount = result.expandedNodeCount;
	view.discoveredNodeCount = result.discoveredNodeCount;
	view.requiredCellInfoCount = result.requiredCellInfoCount;
	view.cumulativeCellCount = result.cumulativeCellCount;
	view.ownerToken = work.ownerToken;
	view.materializationPlanHash = materializationPlanHash;
	view.hierarchyAllPassable = result.hierarchyAllPassable != 0;
	view.status = result.status;
}

bool BuildOrdinaryPathReferencePlan(const OrdinaryPathBatchWork &batch,
	const OrdinaryPathRequestWork &work, OrdinaryPathReferenceDetachedOutput &storage,
	OrdinaryPathReferenceDetachedOperation &operation)
{
	const DeterministicPathSearchResult &result = operation.result;
	if (result.status != DETERMINISTIC_PATH_FOUND)
	{
		// The detached vectors are sized from the production result before the
		// serial clock starts.  Refuse a divergent count rather than allowing a
		// malformed non-found result to make the canonical writer read beyond
		// detached storage.
		if (result.pointCount > operation.points.size() ||
			result.passableBlockCount > operation.passableBlocks.size())
			return false;
		operation.points.resize(result.pointCount);
		operation.passableBlocks.resize(result.passableBlockCount);
		operation.allocationOrder.clear();
		operation.cleanupOrder.clear();
		SetOrdinaryPathReferenceResultView(operation.view, result, work,
			operation.points, operation.allocationOrder, operation.cleanupOrder,
			operation.passableBlocks, 0);
		return true;
	}
	if (batch.grid.width == 0 || batch.grid.height == 0 ||
		batch.grid.width > std::numeric_limits<std::size_t>::max() /
			batch.grid.height)
		return false;
	const std::size_t cellCount = static_cast<std::size_t>(batch.grid.width) *
		batch.grid.height;
	if (cellCount != storage.nodes.size() || cellCount != batch.cells.size())
		return false;
	const std::int64_t startX = work.request.startX;
	const std::int64_t startY = work.request.startY;
	const std::int64_t goalX = work.request.goalX;
	const std::int64_t goalY = work.request.goalY;
	const std::int64_t originX = batch.grid.originX;
	const std::int64_t originY = batch.grid.originY;
	if (startX < originX || startY < originY || goalX < originX ||
		goalY < originY || static_cast<std::uint64_t>(startX - originX) >=
			batch.grid.width || static_cast<std::uint64_t>(startY - originY) >=
			batch.grid.height || static_cast<std::uint64_t>(goalX - originX) >=
			batch.grid.width || static_cast<std::uint64_t>(goalY - originY) >=
			batch.grid.height)
		return false;
	const std::uint32_t startIndex = static_cast<std::uint32_t>(
		(static_cast<std::uint64_t>(startY - originY) * batch.grid.width) +
		static_cast<std::uint64_t>(startX - originX));
	const std::uint32_t goalIndex = static_cast<std::uint32_t>(
		(static_cast<std::uint64_t>(goalY - originY) * batch.grid.width) +
		static_cast<std::uint64_t>(goalX - originX));
	if (storage.discovered.size() < cellCount || storage.open.size() < cellCount ||
		storage.closed.size() < cellCount ||
		operation.allocationOrder.size() > cellCount ||
		operation.cleanupOrder.size() > cellCount)
		return false;
	std::size_t discoveredCount = 0;
	std::size_t openCount = 0;
	std::size_t closedCount = 0;
	for (std::size_t index = 0; index < cellCount; ++index)
	{
		const DeterministicPathSearchNode &node = storage.nodes[index];
		if (node.pathCost == std::numeric_limits<std::uint32_t>::max())
			continue;
		storage.discovered[discoveredCount++] = static_cast<std::uint32_t>(index);
		if (node.state == DETERMINISTIC_PATH_NODE_OPEN)
			storage.open[openCount++] = static_cast<std::uint32_t>(index);
		else if (node.state == DETERMINISTIC_PATH_NODE_CLOSED)
			storage.closed[closedCount++] = static_cast<std::uint32_t>(index);
	}
	if (discoveredCount != result.discoveredNodeCount ||
		openCount + closedCount != result.cumulativeCellCount)
		return false;
	std::sort(storage.discovered.begin(),
		storage.discovered.begin() + discoveredCount,
		[&](std::uint32_t left, std::uint32_t right)
		{
			return storage.nodes[left].discoveryOrdinal <
				storage.nodes[right].discoveryOrdinal;
		});
	std::sort(storage.open.begin(), storage.open.begin() + openCount,
		[&](std::uint32_t left, std::uint32_t right)
		{
			const DeterministicPathSearchNode &leftNode = storage.nodes[left];
			const DeterministicPathSearchNode &rightNode = storage.nodes[right];
			if (leftNode.estimatedTotalCost != rightNode.estimatedTotalCost)
				return leftNode.estimatedTotalCost < rightNode.estimatedTotalCost;
			return leftNode.insertionOrdinal < rightNode.insertionOrdinal;
		});
	std::sort(storage.closed.begin(), storage.closed.begin() + closedCount,
		[&](std::uint32_t left, std::uint32_t right)
		{
			return storage.nodes[left].closeOrdinal >
				storage.nodes[right].closeOrdinal;
		});

	std::size_t expectedAllocationCount = 0;
	if (!hasInitialCellInfo(batch, goalIndex)) ++expectedAllocationCount;
	if (startIndex != goalIndex && !hasInitialCellInfo(batch, startIndex))
		++expectedAllocationCount;
	for (std::size_t index = 0; index < discoveredCount; ++index)
	{
		const std::uint32_t cellIndex = storage.discovered[index];
		if (cellIndex != startIndex && cellIndex != goalIndex &&
			!hasInitialCellInfo(batch, cellIndex))
			++expectedAllocationCount;
	}
	if (expectedAllocationCount != result.requiredCellInfoCount ||
		expectedAllocationCount != operation.allocationOrder.size() ||
		openCount + closedCount != operation.cleanupOrder.size() ||
		result.passableBlockCount > operation.passableBlocks.size() ||
		result.pointCount > operation.points.size())
		return false;
	std::size_t allocationIndex = 0;
	if (!hasInitialCellInfo(batch, goalIndex))
		operation.allocationOrder[allocationIndex++] = goalIndex;
	if (startIndex != goalIndex && !hasInitialCellInfo(batch, startIndex))
		operation.allocationOrder[allocationIndex++] = startIndex;
	for (std::size_t index = 0; index < discoveredCount; ++index)
	{
		const std::uint32_t cellIndex = storage.discovered[index];
		if (cellIndex != startIndex && cellIndex != goalIndex &&
			!hasInitialCellInfo(batch, cellIndex))
			operation.allocationOrder[allocationIndex++] = cellIndex;
	}
	for (std::size_t index = 0; index < openCount; ++index)
		operation.cleanupOrder[index] = storage.open[index];
	for (std::size_t index = 0; index < closedCount; ++index)
		operation.cleanupOrder[openCount + index] = storage.closed[index];
	for (std::size_t index = 0; index < result.passableBlockCount; ++index)
		operation.passableBlocks[index] = result.passableBlockIndices[index];
	operation.passableBlocks.resize(result.passableBlockCount);
	operation.materializationPlanHash = ComputeDeterministicOrdinaryPathPlanHash(
		operation.points.data(), result.pointCount,
		operation.allocationOrder.data(), operation.allocationOrder.size(),
		operation.cleanupOrder.data(), operation.cleanupOrder.size(),
		operation.passableBlocks.data(), operation.passableBlocks.size(),
		result.hierarchyAllPassable != 0, result.snapshotGeneration,
		work.request.objectId, work.ownerToken);
	if (operation.materializationPlanHash == 0)
		return false;
	SetOrdinaryPathReferenceResultView(operation.view, result, work,
			operation.points, operation.allocationOrder, operation.cleanupOrder,
			operation.passableBlocks, operation.materializationPlanHash);
	return true;
}

bool SerialComputeOrdinaryPathReference(const void *immutableInput,
	void *detachedSerialOutput)
{
	const OrdinaryPathReferenceInput &input =
		*static_cast<const OrdinaryPathReferenceInput *>(immutableInput);
	if (input.batch != nullptr)
		ObserveOrdinaryPathTestRequest(*input.batch,
			DETERMINISTIC_ORDINARY_PATH_TEST_REFERENCE_SERIAL_ENTER, 0, 0);
	OrdinaryPathReferenceOutputView &view =
		*static_cast<OrdinaryPathReferenceOutputView *>(detachedSerialOutput);
	if (view.detachedStorage == nullptr)
		return false;
	OrdinaryPathReferenceDetachedOutput &detached =
		*static_cast<OrdinaryPathReferenceDetachedOutput *>(
			const_cast<void *>(view.detachedStorage));
	if (input.batch == nullptr || input.requestCount == 0 ||
		input.requestCount != detached.operations.size() ||
		input.requestCount != detached.results.size())
		return false;
	detached.count = input.requestCount;
	detached.view.count = input.requestCount;
	for (std::size_t index = 0; index < input.requestCount; ++index)
	{
		OrdinaryPathReferenceDetachedOperation &operation =
			detached.operations[index];
		operation.result = {};
		operation.result.points = operation.points.empty() ? nullptr :
			operation.points.data();
		operation.result.pointCapacity = operation.points.size();
		operation.result.passableBlockIndices =
			operation.passableBlocks.empty() ? nullptr :
			operation.passableBlocks.data();
		operation.result.passableBlockCapacity = operation.passableBlocks.size();
		DeterministicPathSearchScratch scratch = {
			detached.nodes.data(), detached.nodes.size(), detached.heap.data(),
			detached.heap.size(), detached.hierarchyPassable.data(),
			detached.hierarchyPassable.size()
		};
		FindDeterministicPath(input.batch->grid,
			input.batch->requests[index].request, scratch, operation.result);
		if (!BuildOrdinaryPathReferencePlan(*input.batch,
			input.batch->requests[index], detached, operation))
			return false;
		detached.results[index] = operation.view;
	}
	return true;
}

bool PrepareOrdinaryPathReferenceBundle(const OrdinaryPathBatchWork &batch,
	std::size_t requestCount, performance::KernelPerformanceReferenceMode mode,
	OrdinaryPathReferenceBundle &bundle)
{
	if (requestCount == 0 || requestCount != batch.requestCount ||
		batch.grid.cells == nullptr || batch.grid.width == 0 ||
		batch.grid.height == 0 || batch.grid.width >
		std::numeric_limits<std::size_t>::max() / batch.grid.height)
		return false;
	const std::size_t cellCount = static_cast<std::size_t>(batch.grid.width) *
		batch.grid.height;
	const std::size_t hierarchyBlockWidth =
		(static_cast<std::size_t>(batch.grid.width) + 9U) / 10U;
	const std::size_t hierarchyBlockHeight =
		(static_cast<std::size_t>(batch.grid.height) + 9U) / 10U;
	if (hierarchyBlockWidth == 0 || hierarchyBlockHeight == 0 ||
		hierarchyBlockWidth > std::numeric_limits<std::size_t>::max() /
			hierarchyBlockHeight)
		return false;
	const std::size_t hierarchyBlockCount = hierarchyBlockWidth *
		hierarchyBlockHeight;
	bundle.input.batch = &batch;
	bundle.input.requestCount = requestCount;
	bundle.input.cellCount = cellCount;
	bundle.input.hierarchyBlockCount = hierarchyBlockCount;
	bundle.productionResults.resize(requestCount);
	for (std::size_t index = 0; index < requestCount; ++index)
	{
		const OrdinaryPathRequestWork &work = batch.requests[index];
		const DeterministicPathSearchResult &result = work.result;
		const std::uint64_t planHash = result.status ==
			DETERMINISTIC_PATH_FOUND ? ComputeDeterministicOrdinaryPathPlanHash(
			work.points.empty() ? nullptr : work.points.data(), work.points.size(),
			work.allocationOrder.empty() ? nullptr : work.allocationOrder.data(),
			work.allocationOrder.size(),
			work.cleanupOrder.empty() ? nullptr : work.cleanupOrder.data(),
			work.cleanupOrder.size(),
			work.passableBlocks.empty() ? nullptr : work.passableBlocks.data(),
			work.passableBlocks.size(), result.hierarchyAllPassable != 0,
			result.snapshotGeneration, work.request.objectId, work.ownerToken) : 0;
		if (result.status == DETERMINISTIC_PATH_FOUND && planHash == 0)
			return false;
		SetOrdinaryPathReferenceResultView(bundle.productionResults[index],
			result, work, work.points, work.allocationOrder, work.cleanupOrder,
			work.passableBlocks, planHash);
		if (batch.testHooks.observeReferenceResult != nullptr)
			batch.testHooks.observeReferenceResult(batch.testHooks.context, index,
				result.status, result.pointCount, work.points.size(),
				bundle.productionResults[index].pointCount,
				bundle.productionResults[index].points != nullptr);
	}
	bundle.production.results = bundle.productionResults.data();
	bundle.production.count = requestCount;
	bundle.production.detachedStorage = nullptr;
	if (mode != performance::KERNEL_REFERENCE_SERIAL_ORACLE)
		return true;
	try
	{
		bundle.detached.reset(new OrdinaryPathReferenceDetachedOutput());
		OrdinaryPathReferenceDetachedOutput &detached = *bundle.detached;
		detached.results.resize(requestCount);
		detached.operations.resize(requestCount);
		detached.nodes.resize(cellCount);
		detached.heap.resize(cellCount);
		detached.hierarchyPassable.resize(hierarchyBlockCount);
		detached.discovered.resize(cellCount);
		detached.open.resize(cellCount);
		detached.closed.resize(cellCount);
		for (std::size_t index = 0; index < requestCount; ++index)
		{
			OrdinaryPathReferenceDetachedOperation &operation =
				detached.operations[index];
			const OrdinaryPathReferenceResultView &production =
				bundle.productionResults[index];
			operation.points.resize(production.pointCount);
			operation.passableBlocks.resize(production.passableBlockCount);
			operation.allocationOrder.resize(production.allocationCount);
			operation.cleanupOrder.resize(production.cleanupCount);
		}
	}
	catch (...)
	{
		return false;
	}
	bundle.detached->view.results = bundle.detached->results.data();
	bundle.detached->view.count = requestCount;
	return true;
}

void ObserveOrdinaryPathReference(const OrdinaryPathBatchWork &batch,
	std::size_t requestCount, performance::KernelPerformanceBatch *timingBatch,
	performance::KernelPerformanceReferenceLedger *referenceLedger,
	performance::KernelPerformanceReferenceBatch *referenceBatch)
{
	if (referenceBatch == nullptr)
		return;
	*referenceBatch = performance::KernelPerformanceReferenceBatch();
	if (timingBatch == nullptr || referenceLedger == nullptr ||
		!timingBatch->valid())
		return;
	const performance::KernelPerformanceReferenceMode mode =
		referenceLedger->mode();
	if (mode == performance::KERNEL_REFERENCE_DISABLED)
		return;
	if (batch.reference && (!batch.reference->collected || batch.reference->failed))
		return;
	performance::KernelPerformanceBatchIdentity identity;
	if (!performance::KernelPerformanceLedger::instance().describeBatch(
		*timingBatch, identity) || identity.kernel !=
		performance::KERNEL_PERFORMANCE_PATH || identity.subtype != 0)
		return;
	OrdinaryPathReferenceBundle bundle;
	try
	{
		if (!PrepareOrdinaryPathReferenceBundle(batch, requestCount, mode,
			bundle))
			return;
	}
	catch (...)
	{
		return;
	}
	if (batch.reference)
	{
		*referenceBatch = referenceLedger->observeValidatedAttempt(batch.reference->attempt,
			WriteOrdinaryPathReferenceOutput, &bundle.production);
		return;
	}
	*referenceBatch = referenceLedger->observeValidatedBatch(
		performance::KERNEL_PERFORMANCE_PATH, identity.subtype, identity.frame,
		identity.ordinal, 1, static_cast<rts::JobMetricCounter>(requestCount),
		WriteOrdinaryPathReferenceInput, &bundle.input,
		WriteOrdinaryPathReferenceOutput, &bundle.production,
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
			SerialComputeOrdinaryPathReference : nullptr,
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
			static_cast<void *>(&bundle.detached->view) : nullptr);
}
#endif

#if defined(_WIN64)
class OrdinaryPathInlineMaterializationScope
{
public:
	OrdinaryPathInlineMaterializationScope(OrdinaryPathSourceRecord &source,
		std::size_t request) : m_source(source), m_budget(source.budgets[request])
	{
		m_source.materialization = m_source.ledger->beginInlineOwnerSerial(m_source.inlineBody,
			performance::KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		if (!valid()) m_source.failed = true;
	}
	~OrdinaryPathInlineMaterializationScope()
	{
		if (!valid()) return;
		// The whole native helper has returned, including its local-vector and
		// test-scope destruction. Settle only a grant site it actually reached.
		if (m_budget.grantSite != 0 &&
			!m_source.ledger->finishInlineRequestBudget(m_source.materialization, m_budget))
			m_source.failed = true;
		if (!m_source.ledger->endInlineOwnerSerial(m_source.materialization)) m_source.failed = true;
		m_source.materialization = performance::KernelPerformanceInlineOwnerSerial();
	}
	bool valid() const { return m_source.materialization.valid(); }
private:
	OrdinaryPathSourceRecord &m_source;
	performance::KernelPerformanceRequestBudget &m_budget;
};
#endif

bool buildOrdinaryPathMaterializationPlan(OrdinaryPathBatchWork &batch,
	OrdinaryPathRangeWork &range, OrdinaryPathRequestWork &work)
{
	#if defined(_WIN64)
	const OrdinaryPathMaterializationTestScope testScope(batch, range, work);
	#endif
	if (work.result.status != DETERMINISTIC_PATH_FOUND)
		return true;
	const std::size_t cellCount = batch.cells.size();
	const std::uint32_t startIndex =
		static_cast<std::uint32_t>(work.request.startY - batch.grid.originY) *
			batch.grid.width +
		static_cast<std::uint32_t>(work.request.startX - batch.grid.originX);
	const std::uint32_t goalIndex =
		static_cast<std::uint32_t>(work.request.goalY - batch.grid.originY) *
			batch.grid.width +
		static_cast<std::uint32_t>(work.request.goalX - batch.grid.originX);

	std::vector<std::uint32_t> discovered;
	std::vector<std::uint32_t> open;
	std::vector<std::uint32_t> closed;
	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_DISCOVERED_ALLOCATION);
	#endif
	try
	{
		discovered.reserve(work.result.discoveredNodeCount);
		open.reserve(work.result.cumulativeCellCount);
		closed.reserve(work.result.expandedNodeCount);
		for (std::size_t i = 0; i < cellCount; ++i)
		{
			const DeterministicPathSearchNode &node = range.nodes[i];
			if (node.pathCost == std::numeric_limits<std::uint32_t>::max())
				continue;
			discovered.push_back(static_cast<std::uint32_t>(i));
			if (node.state == DETERMINISTIC_PATH_NODE_OPEN)
				open.push_back(static_cast<std::uint32_t>(i));
			else if (node.state == DETERMINISTIC_PATH_NODE_CLOSED)
				closed.push_back(static_cast<std::uint32_t>(i));
		}
	}
	catch (...)
	{
		return false;
	}
	if (discovered.size() !=
			static_cast<std::size_t>(work.result.discoveredNodeCount) ||
		open.size() + closed.size() !=
			static_cast<std::size_t>(work.result.cumulativeCellCount))
	{
		return false;
	}

	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_SORT);
	#endif
	std::sort(discovered.begin(), discovered.end(),
		[&](std::uint32_t left, std::uint32_t right)
		{
			return range.nodes[left].discoveryOrdinal <
				range.nodes[right].discoveryOrdinal;
		});
	std::sort(open.begin(), open.end(),
		[&](std::uint32_t left, std::uint32_t right)
		{
			const DeterministicPathSearchNode &leftNode = range.nodes[left];
			const DeterministicPathSearchNode &rightNode = range.nodes[right];
			if (leftNode.estimatedTotalCost != rightNode.estimatedTotalCost)
				return leftNode.estimatedTotalCost < rightNode.estimatedTotalCost;
			return leftNode.insertionOrdinal < rightNode.insertionOrdinal;
		});
	std::sort(closed.begin(), closed.end(),
		[&](std::uint32_t left, std::uint32_t right)
		{
			return range.nodes[left].closeOrdinal >
				range.nodes[right].closeOrdinal;
		});
	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_AFTER_SORT);
	#endif

	std::size_t allocationCount = 0;
	if (!hasInitialCellInfo(batch, goalIndex))
		++allocationCount;
	if (startIndex != goalIndex && !hasInitialCellInfo(batch, startIndex))
		++allocationCount;
	for (std::size_t i = 0; i < discovered.size(); ++i)
	{
		const std::uint32_t index = discovered[i];
		if (index != startIndex && index != goalIndex &&
			!hasInitialCellInfo(batch, index))
		{
			++allocationCount;
		}
	}
	if (allocationCount !=
		static_cast<std::size_t>(work.result.requiredCellInfoCount))
		return false;

	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_BYTE_ARITHMETIC);
	#endif
	if (work.result.pointCount >
		std::numeric_limits<std::size_t>::max() /
			sizeof(DeterministicPathPoint) ||
		allocationCount > std::numeric_limits<std::size_t>::max() /
			sizeof(std::uint32_t) ||
		work.result.cumulativeCellCount >
			std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) ||
		work.result.passableBlockCount >
			std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
	{
		return false;
	}
	const std::size_t pointBytes =
		work.result.pointCount * sizeof(DeterministicPathPoint);
	const std::size_t allocationBytes =
		allocationCount * sizeof(std::uint32_t);
	const std::size_t cleanupBytes =
		work.result.cumulativeCellCount * sizeof(std::uint32_t);
	const std::size_t passableBlockBytes =
		work.result.passableBlockCount * sizeof(std::uint32_t);
	if (pointBytes > std::numeric_limits<std::size_t>::max() - allocationBytes ||
		pointBytes + allocationBytes >
			std::numeric_limits<std::size_t>::max() - cleanupBytes ||
		pointBytes + allocationBytes + cleanupBytes >
			std::numeric_limits<std::size_t>::max() - passableBlockBytes)
	{
		return false;
	}
	const std::size_t outputBytes = pointBytes + allocationBytes + cleanupBytes +
		passableBlockBytes;
	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_GRANT, outputBytes);
	#endif
	bool storageReserved = false;
	#if defined(_WIN64)
	if (batch.reference && batch.reference->baseline)
	{
		OrdinaryPathSourceRecord &source = *batch.reference;
		if (!source.ledger->replayRequestBudgetGrant(source.materialization,
			&work - batch.requests.get(), 1, 1, outputBytes, storageReserved))
		{ source.failed = true; return false; }
	}
	else
	#endif
	{
		#if defined(_WIN64)
		ObserveOrdinaryPathMaterializationTest(batch, range, work,
			DETERMINISTIC_ORDINARY_PATH_TEST_PHYSICAL_RESERVE, outputBytes);
		#endif
		storageReserved = reserveOrdinaryPathResultStorage(batch, outputBytes);
	}
	#if defined(_WIN64)
	if (batch.reference)
	{
		performance::KernelPerformanceRequestBudget &budget =
			batch.reference->budgets[&work - batch.requests.get()];
		budget.grantSite = 1; budget.localGrantOrdinal = 1;
		budget.requestedBytes = outputBytes;
		budget.grantedBytes = storageReserved ? outputBytes : 0;
		budget.consumedBytes = budget.grantedBytes;
		budget.disposition = storageReserved ? performance::KERNEL_REQUEST_BUDGET_RETAINED :
			performance::KERNEL_REQUEST_BUDGET_REFUSED;
	}
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_GRANT_RETURNED, outputBytes, storageReserved);
	#endif
	if (!storageReserved)
	{
		work.result.status = DETERMINISTIC_PATH_BUDGET_EXHAUSTED;
		return true;
	}

	try
	{
		#if defined(_WIN64)
		ObserveOrdinaryPathMaterializationTest(batch, range, work,
			DETERMINISTIC_ORDINARY_PATH_TEST_FIRST_OUTPUT_ALLOCATION, outputBytes);
		#endif
		work.points.assign(range.pointScratch.begin(),
			range.pointScratch.begin() + work.result.pointCount);
		work.allocationOrder.reserve(allocationCount);
		if (!hasInitialCellInfo(batch, goalIndex))
			work.allocationOrder.push_back(goalIndex);
		if (startIndex != goalIndex && !hasInitialCellInfo(batch, startIndex))
			work.allocationOrder.push_back(startIndex);
		for (std::size_t i = 0; i < discovered.size(); ++i)
		{
			const std::uint32_t index = discovered[i];
			if (index != startIndex && index != goalIndex &&
				!hasInitialCellInfo(batch, index))
			{
				work.allocationOrder.push_back(index);
			}
		}
		work.cleanupOrder.reserve(open.size() + closed.size());
		work.cleanupOrder.insert(work.cleanupOrder.end(), open.begin(), open.end());
		work.cleanupOrder.insert(work.cleanupOrder.end(), closed.begin(), closed.end());
		work.passableBlocks.assign(range.hierarchyBlockScratch.begin(),
			range.hierarchyBlockScratch.begin() + work.result.passableBlockCount);
	}
	catch (...)
	{
		#if defined(_WIN64)
		if (!batch.reference || !batch.reference->baseline)
		#endif
			batch.resultStorageBytes.fetch_sub(outputBytes, std::memory_order_acq_rel);
		work.points.clear();
		work.allocationOrder.clear();
		work.cleanupOrder.clear();
		work.passableBlocks.clear();
		#if defined(_WIN64)
		if (batch.reference)
		{
			performance::KernelPerformanceRequestBudget &budget =
				batch.reference->budgets[&work - batch.requests.get()];
			budget.refundSite = 2; budget.localRefundOrdinal = 2;
			budget.refundedBytes = outputBytes; budget.consumedBytes = 0;
			budget.disposition = performance::KERNEL_REQUEST_BUDGET_REFUNDED;
		}
		ObserveOrdinaryPathMaterializationTest(batch, range, work,
			DETERMINISTIC_ORDINARY_PATH_TEST_REFUND_COMPLETED, outputBytes);
		#endif
		return false;
	}
	work.result.points = work.points.data();
	work.result.pointCapacity = work.points.size();
	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_FINAL_VALIDATION);
	#endif
	if (work.allocationOrder.size() != allocationCount ||
		work.cleanupOrder.size() !=
			static_cast<std::size_t>(work.result.cumulativeCellCount) ||
		work.passableBlocks.size() != work.result.passableBlockCount)
	{
		return false;
	}
	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_HASH);
	#endif
	work.materializationPlanHash = ComputeDeterministicOrdinaryPathPlanHash(
		work.points.data(), work.points.size(), work.allocationOrder.data(),
		work.allocationOrder.size(), work.cleanupOrder.data(),
		work.cleanupOrder.size(), work.passableBlocks.data(),
		work.passableBlocks.size(), work.result.hierarchyAllPassable != 0,
		work.result.snapshotGeneration,
		work.request.objectId, work.ownerToken);
	#if defined(_WIN64)
	ObserveOrdinaryPathMaterializationTest(batch, range, work,
		DETERMINISTIC_ORDINARY_PATH_TEST_AFTER_HASH);
	#endif
	return work.materializationPlanHash != 0;
}

bool MaterializeOrdinaryPathRequest(OrdinaryPathBatchWork &batch,
	OrdinaryPathRangeWork &range, OrdinaryPathRequestWork &work)
{
	#if defined(_WIN64)
	if (batch.reference && batch.reference->baseline)
	{
		bool materialized = false;
		{
			OrdinaryPathInlineMaterializationScope extent(*batch.reference, &work - batch.requests.get());
			if (!extent.valid()) return false;
			materialized = buildOrdinaryPathMaterializationPlan(batch, range, work);
		}
		return materialized && !batch.reference->failed;
	}
	#endif
	return buildOrdinaryPathMaterializationPlan(batch, range, work);
}

bool ExecuteOrdinaryPathRangeBody(const std::shared_ptr<OrdinaryPathBatchWork> &batch,
	unsigned rangeIndex, JobContext *context)
{
	OrdinaryPathRangeWork &range = batch->ranges[rangeIndex];
	bool inlineExecution = false;
	#if defined(_WIN64)
	inlineExecution = context == nullptr && batch->reference && batch->reference->baseline &&
		batch->reference->inlineBody.valid();
	#endif
	if (context == nullptr && !inlineExecution) return false;
	#if defined(_WIN64)
	OrdinaryPathSourceCheckpointScope checkpoint(*batch, rangeIndex);
	#endif
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if (context != nullptr) pauseDirectPathTest(ORDINARY_PATH_TEST_ENTRY_PAUSE);
	#endif
	const bool entryCancelled = context != nullptr && context->isCancellationRequested();
	#if defined(_WIN64)
	if (checkpoint.cancelled(1, range.begin, entryCancelled))
	#else
	if (entryCancelled)
	#endif
	{
		publishOrdinaryPathCancellation(range);
		if (context != nullptr) s_ordinaryPathLateDrainExecutions.fetch_add(1,
			std::memory_order_relaxed);
		return false;
	}
	const bool workerExecution = context != nullptr && context->isPhysicalWorkerExecution();
	unsigned runningState = workerExecution ?
		DIRECT_PATH_WORK_RUNNING_WORKER : DIRECT_PATH_WORK_RUNNING_OWNER;
	unsigned requestRunningState = DIRECT_PATH_WORK_RUNNING_WORKER;
	unsigned requestCompletedState = DIRECT_PATH_WORK_WORKER;
	#if defined(_WIN64)
	if (inlineExecution)
	{
		runningState = requestRunningState = DIRECT_PATH_WORK_RUNNING_INLINE;
		requestCompletedState = DIRECT_PATH_WORK_INLINE;
	}
	#endif
	unsigned expectedState = DIRECT_PATH_WORK_PENDING;
	if (!range.executionState.compare_exchange_strong(expectedState,
		runningState, std::memory_order_acq_rel, std::memory_order_acquire))
	{
		if (expectedState == DIRECT_PATH_WORK_CANCELLED && context != nullptr)
			s_ordinaryPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
		return false;
	}

	if (workerExecution)
	{
		range.physicalWorkerIndex.store(context->physicalWorkerIndex(),
			std::memory_order_release);
		const unsigned active = batch->activeWorkers.fetch_add(1,
			std::memory_order_acq_rel) + 1;
		publishPeak(batch->peakActiveWorkers, active);
		#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseDirectPathTest(ORDINARY_PATH_TEST_ACTIVE_PAUSE);
		#endif
	}

	bool succeeded = workerExecution || inlineExecution;
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestFaultMask.load(std::memory_order_acquire) &
		ORDINARY_PATH_TEST_EXECUTION_FAILURE) != 0)
	{
		succeeded = false;
	}
	#endif
	for (std::size_t requestIndex = range.begin;
		succeeded && requestIndex < range.end; ++requestIndex)
	{
		const bool requestCancelled = (context != nullptr && context->isCancellationRequested()) ||
			range.executionState.load(std::memory_order_acquire) ==
				DIRECT_PATH_WORK_CANCELLED;
		#if defined(_WIN64)
		if (checkpoint.cancelled(2, requestIndex, requestCancelled))
		#else
		if (requestCancelled)
		#endif
		{
			succeeded = false;
			break;
		}
		OrdinaryPathRequestWork &work = batch->requests[requestIndex];
		unsigned requestExpected = DIRECT_PATH_WORK_PENDING;
		if (!work.executionState.compare_exchange_strong(requestExpected,
			requestRunningState, std::memory_order_acq_rel,
			std::memory_order_acquire))
		{
			succeeded = false;
			break;
		}
		if (workerExecution)
			work.physicalWorkerIndex.store(context->physicalWorkerIndex(), std::memory_order_release);
		DeterministicPathSearchScratch scratch = {
			range.nodes.data(), range.nodes.size(), range.heap.data(),
			range.heap.size(), range.hierarchyPassableScratch.data(),
			range.hierarchyPassableScratch.size()
		};
		work.result = {};
		work.result.points = range.pointScratch.data();
		work.result.pointCapacity = range.pointScratch.size();
		work.result.passableBlockIndices = range.hierarchyBlockScratch.data();
		work.result.passableBlockCapacity =
			range.hierarchyBlockScratch.size();
		#if defined(_WIN64)
		ObserveOrdinaryPathTestRequest(*batch,
			DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_ENTER, rangeIndex, requestIndex);
		#endif
		FindDeterministicPath(batch->grid, work.request, scratch,
			work.result);
		#if defined(_WIN64)
		ObserveOrdinaryPathTestRequest(*batch,
			DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_EXIT, rangeIndex, requestIndex);
		#endif
		const bool materialized = MaterializeOrdinaryPathRequest(*batch, range, work);
		#if defined(_WIN64)
		ObserveOrdinaryPathTestRequest(*batch,
			DETERMINISTIC_ORDINARY_PATH_TEST_BODY_EXIT, rangeIndex, requestIndex);
		#endif
		if (!materialized)
		{
			work.executionState.store(DIRECT_PATH_WORK_FAILURE,
				std::memory_order_release);
			succeeded = false;
			break;
		}
		requestExpected = requestRunningState;
		if (!work.executionState.compare_exchange_strong(requestExpected,
			requestCompletedState, std::memory_order_release,
			std::memory_order_acquire))
		{
			succeeded = false;
			break;
		}
		#if defined(_WIN64)
		checkpoint.completedRequest();
		#endif
	}

	#if defined(_WIN64)
	checkpoint.finish(succeeded);
	#endif
	if (workerExecution)
		batch->activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
	const unsigned completedState = succeeded ? requestCompletedState : DIRECT_PATH_WORK_FAILURE;
	expectedState = runningState;
	if (!range.executionState.compare_exchange_strong(expectedState,
		completedState, std::memory_order_release, std::memory_order_acquire) &&
		expectedState == DIRECT_PATH_WORK_CANCELLED)
	{
		if (context != nullptr) s_ordinaryPathLateDrainExecutions.fetch_add(1,
			std::memory_order_relaxed);
	}
	return succeeded;
}

#if defined(_WIN64)
bool ConsumeOrdinaryPathReference(const std::shared_ptr<OrdinaryPathBatchWork> &work,
	JobSystem &jobs, PathPerformanceInterval &schedule)
{
	using namespace performance;
	OrdinaryPathBatchWork &batch = *work;
	if (!jobs.isCurrentThread(JOB_OWNER_GAME) || !batch.reference || !batch.reference->baseline)
		return false;
	OrdinaryPathSourceRecord &source = *batch.reference;
	if (!InlineBodyReadiness::ordinary(source, jobs))
		return false;
	KernelPerformanceReferenceLedger &ledger = *source.ledger;
	KernelPerformanceAttemptDecision decision = {};
	if (!ledger.replayDecision(source.attempt, 1, true, source.facts, decision) ||
		decision.admission != KERNEL_ADMISSION_ACCEPTED || decision.reasonSchema != 1 ||
		decision.reason != 1 || decision.sourceConfiguredWorkers != source.workers)
	{ source.failed = true; return false; }
	source.admitted = true;
	batch.referenceAdmissionAccepted = true;
	const KernelPerformanceDispatchPlan dispatch = {1, 2, 1, batch.rangeCount,
		batch.requestCount, batch.grainSize, source.workers};
	if (!ledger.observeDispatch(source.attempt, dispatch))
	{ source.failed = true; return false; }
	for (unsigned i = 0; i != batch.rangeCount; ++i)
	{
		if (!InlineBodyReadiness::ordinary(source, jobs))
		{
			source.failed = true;
			return false;
		}
		const OrdinaryPathRangeWork &range = batch.ranges[i];
		const KernelPerformanceRangePlan plan = {1, i, 0, range.begin, range.end, range.end - range.begin};
		if (!ledger.observeRangePlan(source.attempt, plan))
		{ source.failed = true; return false; }
	}
	source.planned = true;
	schedule.end();
	bool allSucceeded = true;
	for (unsigned i = 0; i != batch.rangeCount; ++i)
	{
		const OrdinaryPathRangeWork &range = batch.ranges[i];
		const KernelPerformanceRangePlan plan = {1, i, 0, range.begin, range.end, range.end - range.begin};
		const KernelPerformanceInlineAction action = ledger.beginInlineBody(source.attempt, plan,
			KernelPerformanceLedger::instance(), source.inlineBody, source.checkpoints[i]);
		if (action == KERNEL_INLINE_INVALID)
		{ source.failed = true; return false; }
		bool succeeded = false;
		if (action == KERNEL_INLINE_EXECUTE)
		{
			try { succeeded = ExecuteOrdinaryPathRangeBody(work, i, nullptr); }
			catch (...) { succeeded = false; }
		}
		const KernelPerformanceRangeProgress progress = OrdinaryPathReleasedProgress(batch, i);
		if (source.failed || (action == KERNEL_INLINE_EXECUTE &&
			!ledger.finishInlineBody(source.inlineBody, progress)))
		{ source.failed = true; return false; }
		source.inlineBody = KernelPerformanceInlineBody();
		if (!ImportOrdinaryPathReferenceRange(batch, i, progress)) return false;
		allSucceeded = succeeded && allSucceeded;
	}
	source.collected = true;
	KernelPerformanceAttemptFinish sourceFinish = {};
	if (!ledger.readSourceFinish(source.attempt, sourceFinish) ||
		(sourceFinish.disposition != KERNEL_PERFORMANCE_COMMITTED &&
		 sourceFinish.disposition != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION) ||
		(sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED &&
		 !sourceFinish.validationObserved))
	{
		source.failed = true;
		return false;
	}
	source.sourceOwnerCommitAllowed =
		sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED;
	return allSucceeded;
}
#endif

class OrdinaryPathRangeJob final : public Job
{
public:
	OrdinaryPathRangeJob(const std::shared_ptr<OrdinaryPathBatchWork> &batch,
		unsigned rangeIndex) : m_batch(batch), m_rangeIndex(rangeIndex) {}

	~OrdinaryPathRangeJob() override
	{
		if (m_batch->liveJobs.fetch_sub(1, std::memory_order_acq_rel) == 1)
			releaseOrdinaryPathActiveSlot(*m_batch);
	}

	void execute(JobContext &context) override
	{
		ExecuteOrdinaryPathRangeBody(m_batch, m_rangeIndex, &context);
	}

private:
	std::shared_ptr<OrdinaryPathBatchWork> m_batch;
	unsigned m_rangeIndex;
};

} // namespace

struct DeterministicOrdinaryPathBatch::State
{
	State() : requestCount(0), submittedRangeJobCount(0), completed(false),
		timedOut(false) {}

	std::shared_ptr<OrdinaryPathBatchWork> work;
	std::size_t requestCount;
	std::size_t submittedRangeJobCount;
	bool completed;
	bool timedOut;
};

DeterministicOrdinaryPathBatch::DeterministicOrdinaryPathBatch() :
	m_state(nullptr)
{
	try
	{
		m_state = new State;
	}
	catch (...)
	{
		m_state = nullptr;
	}
}

DeterministicOrdinaryPathBatch::~DeterministicOrdinaryPathBatch()
{
	delete m_state;
}

bool DeterministicOrdinaryPathBatch::executeSynchronously(JobSystem &jobs,
	const ImmutableNavigationGrid &grid,
	const DeterministicOrdinaryPathBatchRequest *requests,
	std::size_t requestCount, unsigned workerWaitTimeoutMilliseconds
#if defined(_WIN64)
	, performance::KernelPerformanceBatch *performanceBatch
	, performance::KernelPerformanceReferenceLedger *performanceReferenceLedger
	, performance::KernelPerformanceReferenceBatch *performanceReferenceBatch
	, const DeterministicOrdinaryPathTestHooks *testHooks
	, performance::KernelPerformanceAttempt performanceReferenceAttempt
#endif
	)
{
	#if defined(_WIN64)
	if (performanceReferenceBatch != nullptr)
		*performanceReferenceBatch = performance::KernelPerformanceReferenceBatch();
	#endif
	if (m_state == nullptr)
		return false;
	m_state->work.reset();
	m_state->requestCount = requestCount;
	m_state->submittedRangeJobCount = 0;
	m_state->completed = false;
	m_state->timedOut = false;
	#if defined(_WIN64)
	const bool traceRequested = performanceReferenceLedger != nullptr &&
		performanceReferenceLedger->traceRequested();
	const bool sourceBoundInline = performanceReferenceLedger != nullptr &&
		performanceReferenceLedger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if ((traceRequested || sourceBoundInline) &&
		!performanceReferenceAttempt.valid()) return false;
	#else
	const bool sourceBoundInline = false;
	#endif
	if (grid.cells == nullptr || grid.width == 0 || grid.height == 0 ||
		requests == nullptr || requestCount == 0 ||
		requestCount > std::numeric_limits<unsigned>::max() ||
		workerWaitTimeoutMilliseconds == 0 || !jobs.isRunning() ||
		jobs.workerCount() == 0 || !jobs.isCurrentThread(JOB_OWNER_GAME) ||
		grid.width > std::numeric_limits<std::size_t>::max() / grid.height)
	{
		return false;
	}
	const std::size_t cellCount = static_cast<std::size_t>(grid.width) * grid.height;
	if (cellCount == 0 || cellCount > std::numeric_limits<std::uint32_t>::max())
		return false;
	const std::size_t hierarchyBlockWidth =
		(static_cast<std::size_t>(grid.width) + 9U) / 10U;
	const std::size_t hierarchyBlockHeight =
		(static_cast<std::size_t>(grid.height) + 9U) / 10U;
	if (hierarchyBlockWidth == 0 || hierarchyBlockHeight == 0 ||
		hierarchyBlockWidth > std::numeric_limits<std::size_t>::max() /
			hierarchyBlockHeight)
	{
		return false;
	}
	const std::size_t hierarchyBlockCount =
		hierarchyBlockWidth * hierarchyBlockHeight;
	const std::size_t bytesPerCell = sizeof(DeterministicPathSearchNode) +
		4U * sizeof(std::uint32_t) + sizeof(DeterministicPathPoint);
	if (cellCount > std::numeric_limits<std::size_t>::max() / bytesPerCell)
		return false;
	if (cellCount > ORDINARY_PATH_MAX_SCRATCH_BYTES /
		sizeof(DeterministicPathCell) ||
		requestCount > ORDINARY_PATH_MAX_RESULT_BYTES /
			sizeof(OrdinaryPathRequestWork))
	{
		return false;
	}
	const std::size_t navigationBytes =
		cellCount * sizeof(DeterministicPathCell);
	if (hierarchyBlockCount >
		std::numeric_limits<std::size_t>::max() /
			(sizeof(std::uint8_t) + sizeof(std::uint32_t)))
	{
		return false;
	}
	const std::size_t hierarchyBytesPerRange = hierarchyBlockCount *
		(sizeof(std::uint8_t) + sizeof(std::uint32_t));
	if (cellCount * bytesPerCell >
		std::numeric_limits<std::size_t>::max() - hierarchyBytesPerRange)
	{
		return false;
	}
	const std::size_t scratchBytesPerRange =
		cellCount * bytesPerCell + hierarchyBytesPerRange;
	const std::size_t rangesByMemory = scratchBytesPerRange == 0 ? 0 :
		(ORDINARY_PATH_MAX_SCRATCH_BYTES - navigationBytes) /
			scratchBytesPerRange;
	if (rangesByMemory == 0)
		return false;
	const std::size_t requestedRanges = std::min(requestCount,
		static_cast<std::size_t>(jobs.workerCount()));
	const unsigned rangeCount = static_cast<unsigned>(std::min(requestedRanges,
		rangesByMemory));
	if (rangeCount == 0)
		return false;
	const unsigned grainSize = static_cast<unsigned>(
		(requestCount + rangeCount - 1) / rangeCount);

	unsigned expectedActiveBatches = 0;
	if (!sourceBoundInline && !s_activeOrdinaryPathBatches.compare_exchange_strong(
		expectedActiveBatches, 1, std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return false;
	}
	#if defined(_WIN64)
	PathPerformanceInterval capture(performanceBatch,
		performance::KERNEL_PERFORMANCE_CAPTURE);
	#endif
	try
	{
		m_state->work = std::make_shared<OrdinaryPathBatchWork>();
		m_state->work->cells.assign(grid.cells, grid.cells + cellCount);
		m_state->work->requests.reset(new OrdinaryPathRequestWork[requestCount]);
		m_state->work->ranges.reset(new OrdinaryPathRangeWork[rangeCount]);
	}
	catch (...)
	{
		m_state->work.reset();
		if (!sourceBoundInline) s_activeOrdinaryPathBatches.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	OrdinaryPathBatchWork &batch = *m_state->work;
	batch.ownsActiveSlot.store(!sourceBoundInline, std::memory_order_release);
	batch.grid = grid;
	batch.grid.cells = batch.cells.data();
	batch.requestCount = requestCount;
	batch.rangeCount = rangeCount;
	batch.grainSize = grainSize;
	#if defined(_WIN64)
	if (testHooks != nullptr) batch.testHooks = *testHooks;
	#endif
	try
	{
		for (std::size_t i = 0; i < requestCount; ++i)
		{
			batch.requests[i].request = requests[i].search;
			batch.requests[i].ownerToken = requests[i].ownerToken;
		}
		for (unsigned rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex)
		{
			OrdinaryPathRangeWork &range = batch.ranges[rangeIndex];
			JobRange rangeBounds = {};
			if (!JobSystem::rangeForIndex(static_cast<unsigned>(requestCount),
				rangeCount, rangeIndex, rangeBounds))
			{
				m_state->work.reset();
				return false;
			}
			range.begin = rangeBounds.begin;
			range.end = rangeBounds.end;
			#if defined(_WIN64)
			if (testHooks != nullptr && testHooks->observe != nullptr)
				testHooks->observe(testHooks->context,
					DETERMINISTIC_ORDINARY_PATH_TEST_RANGE_PLANNED,
					rangeIndex, range.begin, range.end);
			#endif
			range.nodes.resize(cellCount);
			range.heap.resize(cellCount);
			range.pointScratch.resize(cellCount);
			range.hierarchyPassableScratch.resize(hierarchyBlockCount);
			range.hierarchyBlockScratch.resize(hierarchyBlockCount);
		}
		#if defined(_WIN64)
		if (!PrepareOrdinaryPathSourceRecord(batch, hierarchyBlockCount, jobs,
			performanceReferenceLedger, performanceReferenceAttempt))
		{
			m_state->work.reset();
			return false;
		}
		#endif
	}
	catch (...)
	{
		m_state->work.reset();
		return false;
	}
	#if defined(_WIN64)
	capture.end();
	PathPerformanceInterval schedule(performanceBatch,
		performance::KERNEL_PERFORMANCE_SCHEDULE);
	if (sourceBoundInline)
	{
		m_state->completed = ConsumeOrdinaryPathReference(m_state->work, jobs, schedule);
		if (!m_state->completed) return false;
		PathPerformanceInterval validate(performanceBatch, performance::KERNEL_PERFORMANCE_VALIDATE);
		for (unsigned i = 0; i != batch.rangeCount; ++i)
			if (batch.ranges[i].executionState.load(std::memory_order_acquire) != DIRECT_PATH_WORK_INLINE)
			{ m_state->completed = false; return false; }
		ObserveOrdinaryPathReference(batch, requestCount, performanceBatch,
			performanceReferenceLedger, performanceReferenceBatch);
		if (performanceReferenceBatch == nullptr ||
			!performanceReferenceBatch->valid() ||
			!batch.reference->sourceOwnerCommitAllowed)
		{
			m_state->completed = false;
			return false;
		}
		return true;
	}
	#endif

	const JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		#if defined(_WIN64)
		ObserveOrdinaryPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	#if defined(_WIN64)
	if (batch.reference)
	{
		// JobGroup copies allocate their small handle. Retain it before any
		// submission, never as a rescue allocation after an owner timeout.
		try { batch.reference->group = group; }
		catch (...)
		{
			ObserveOrdinaryPathSourceAdmission(batch, false);
			m_state->work.reset();
			return false;
		}
	}
	#endif
	std::vector<JobSubmission> submissions;
	std::vector<JobHandle> handles;
	try
	{
		#if defined(RTS_BUILD_CORE_EXTRAS)
		if ((s_directPathTestFaultMask.load(std::memory_order_acquire) &
			ORDINARY_PATH_TEST_DISPATCH_VECTOR_ALLOCATION_FAILURE) != 0)
			throw std::bad_alloc();
		#endif
		submissions.resize(rangeCount);
		handles.resize(rangeCount);
	}
	catch (...)
	{
		#if defined(_WIN64)
		ObserveOrdinaryPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	unsigned allocated = 0;
	for (; allocated < rangeCount; ++allocated)
	{
		submissions[allocated].job = new (std::nothrow)
			OrdinaryPathRangeJob(m_state->work, allocated);
		if (submissions[allocated].job == nullptr)
			break;
		submissions[allocated].priority = JOB_PRIORITY_FRAME_CRITICAL;
	}
	batch.liveJobs.store(allocated, std::memory_order_release);
	if (allocated != rangeCount)
	{
		for (unsigned i = 0; i < allocated; ++i)
			delete submissions[i].job;
		if (allocated == 0)
			releaseOrdinaryPathActiveSlot(batch);
		#if defined(_WIN64)
		ObserveOrdinaryPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	if (!jobs.trySubmitBatch(submissions.data(), rangeCount, group,
		handles.data()))
	{
		for (unsigned i = 0; i < rangeCount; ++i)
			delete submissions[i].job;
		#if defined(_WIN64)
		ObserveOrdinaryPathSourceAdmission(batch, false);
		#endif
		m_state->work.reset();
		return false;
	}
	m_state->submittedRangeJobCount = rangeCount;
	#if defined(_WIN64)
	ObserveOrdinaryPathSourceAdmission(batch, true);
	schedule.end();
	PathPerformanceInterval wait(performanceBatch,
		performance::KERNEL_PERFORMANCE_WAIT);
	#endif

	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestPauseMask.load(std::memory_order_acquire) &
		ORDINARY_PATH_TEST_ENTRY_PAUSE) != 0 &&
		!waitForDirectPathTestPause(ORDINARY_PATH_TEST_ENTRY_PAUSE, 1, 15000))
	{
		for (unsigned i = 0; i < rangeCount; ++i)
			publishOrdinaryPathCancellation(batch.ranges[i]);
		jobs.cancel(group);
		return false;
	}
	if ((s_directPathTestPauseMask.load(std::memory_order_acquire) &
		ORDINARY_PATH_TEST_ACTIVE_PAUSE) != 0 &&
		!waitForDirectPathTestPause(ORDINARY_PATH_TEST_ACTIVE_PAUSE,
			rangeCount, 15000))
	{
		for (unsigned i = 0; i < rangeCount; ++i)
			publishOrdinaryPathCancellation(batch.ranges[i]);
		jobs.cancel(group);
		return false;
	}
	#endif

	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() +
		std::chrono::milliseconds(workerWaitTimeoutMilliseconds);
	while (!group.isComplete() && jobs.isRunning() &&
		jobs.workerCount() != 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	bool schedulerRunning = jobs.isRunning();
	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestFaultMask.load(std::memory_order_acquire) &
		ORDINARY_PATH_TEST_SCHEDULER_STOPPED) != 0)
	{
		schedulerRunning = false;
	}
	#endif
	if (!group.isComplete() || !schedulerRunning)
	{
		for (unsigned i = 0; i < rangeCount; ++i)
			publishOrdinaryPathCancellation(batch.ranges[i]);
		jobs.cancel(group);
		m_state->timedOut = true;
		return false;
	}

	const bool joined = jobs.wait(group);
	#if defined(_WIN64)
	wait.end();
	const bool collected = !batch.reference ||
		CollectOrdinaryPathSourceRecord(batch, jobs);
	#else
	const bool collected = true;
	#endif
	releaseOrdinaryPathActiveSlot(batch);
	m_state->completed = joined && collected && !group.failed() &&
		!group.wasCancelled();
	if (!m_state->completed)
		return false;
	#if defined(_WIN64)
	PathPerformanceInterval validate(performanceBatch,
		performance::KERNEL_PERFORMANCE_VALIDATE);
	#endif
	for (unsigned i = 0; i < rangeCount; ++i)
	{
		if (!handles[i].succeeded() ||
			batch.ranges[i].executionState.load(std::memory_order_acquire) !=
				DIRECT_PATH_WORK_WORKER)
		{
			m_state->completed = false;
			return false;
		}
	}
	#if defined(_WIN64)
	ObserveOrdinaryPathReference(batch, requestCount, performanceBatch,
		performanceReferenceLedger, performanceReferenceBatch);
	if (batch.reference && (performanceReferenceBatch == nullptr ||
		!performanceReferenceBatch->valid()))
	{
		m_state->completed = false;
		return false;
	}
	#endif
	return true;
}

#if defined(_WIN64)
bool DeterministicOrdinaryPathBatch::collectPerformanceReference(JobSystem &jobs)
{
	return m_state != nullptr && m_state->work != nullptr &&
		CollectOrdinaryPathSourceRecord(*m_state->work, jobs);
}
#endif

DeterministicOrdinaryPathBatchExecutionSnapshot
DeterministicOrdinaryPathBatch::executionSnapshot() const
{
	DeterministicOrdinaryPathBatchExecutionSnapshot snapshot = {};
	snapshot.physicalWorkerMaskComplete = true;
	if (m_state == nullptr)
		return snapshot;
	snapshot.requestCount = m_state->requestCount;
	snapshot.submittedRangeJobCount = m_state->submittedRangeJobCount;
	snapshot.referenceAdmissionAccepted = m_state->work != nullptr &&
		m_state->work->referenceAdmissionAccepted;
	snapshot.completed = m_state->completed;
	snapshot.timedOut = m_state->timedOut;
	if (m_state->work == nullptr)
		return snapshot;
	const OrdinaryPathBatchWork &batch = *m_state->work;
	snapshot.rangeCount = batch.rangeCount;
	snapshot.grainSize = batch.grainSize;
	snapshot.resultStorageBytes = batch.resultStorageBytes.load(
		std::memory_order_acquire);
	for (std::size_t i = 0; i < m_state->submittedRangeJobCount; ++i)
	{
		const unsigned state = batch.ranges[i].executionState.load(
			std::memory_order_acquire);
		if (state == DIRECT_PATH_WORK_WORKER)
		{
			++snapshot.workerExecutedRangeJobCount;
			const unsigned workerIndex = batch.ranges[i].physicalWorkerIndex.load(
				std::memory_order_acquire);
			bool firstRangeForWorker =
				workerIndex != JOB_INVALID_PHYSICAL_WORKER_INDEX;
			for (std::size_t prior = 0; firstRangeForWorker && prior < i; ++prior)
			{
				firstRangeForWorker =
					batch.ranges[prior].executionState.load(
						std::memory_order_acquire) != DIRECT_PATH_WORK_WORKER ||
					batch.ranges[prior].physicalWorkerIndex.load(
						std::memory_order_acquire) != workerIndex;
			}
			if (firstRangeForWorker)
			{
				++snapshot.distinctPhysicalWorkerCount;
				if (workerIndex < 64)
					snapshot.physicalWorkerMask |=
						std::uint64_t(1) << workerIndex;
				else
					snapshot.physicalWorkerMaskComplete = false;
			}
		}
		else if (state == DIRECT_PATH_WORK_OWNER)
			++snapshot.ownerExecutedRangeJobCount;
		else if (state == DIRECT_PATH_WORK_FAILURE ||
			state == DIRECT_PATH_WORK_CANCELLED)
			++snapshot.failedRangeJobCount;
	}
	snapshot.peakActiveWorkers = batch.peakActiveWorkers.load(
		std::memory_order_acquire);
	return snapshot;
}

DeterministicDirectPathExecutionSnapshot
DeterministicOrdinaryPathBatch::requestExecutionSnapshot(
	std::size_t requestIndex) const
{
	DeterministicDirectPathExecutionSnapshot snapshot = {
		DIRECT_PATH_EXECUTION_PENDING, JOB_INVALID_PHYSICAL_WORKER_INDEX,
		false, false
	};
	if (m_state == nullptr || m_state->work == nullptr ||
		requestIndex >= m_state->requestCount)
	{
		return snapshot;
	}
	snapshot.submitted = m_state->submittedRangeJobCount != 0;
	const OrdinaryPathRequestWork &work = m_state->work->requests[requestIndex];
	const unsigned state = work.executionState.load(std::memory_order_acquire);
	if (state == DIRECT_PATH_WORK_CANCELLED)
		snapshot.state = DIRECT_PATH_EXECUTION_CANCELLED;
	else if (state == DIRECT_PATH_WORK_WORKER)
		snapshot.state = DIRECT_PATH_EXECUTION_WORKER;
	else if (state == DIRECT_PATH_WORK_OWNER)
		snapshot.state = DIRECT_PATH_EXECUTION_OWNER;
	else if (state == DIRECT_PATH_WORK_FAILURE)
		snapshot.state = DIRECT_PATH_EXECUTION_FAILURE;
	#if defined(_WIN64)
	else if (state == DIRECT_PATH_WORK_INLINE)
		snapshot.state = DIRECT_PATH_EXECUTION_INLINE;
	#endif
	if (snapshot.state == DIRECT_PATH_EXECUTION_WORKER)
		snapshot.physicalWorkerIndex = work.physicalWorkerIndex.load(
			std::memory_order_acquire);
	snapshot.succeeded = m_state->completed &&
		(snapshot.state == DIRECT_PATH_EXECUTION_WORKER
		#if defined(_WIN64)
		|| snapshot.state == DIRECT_PATH_EXECUTION_INLINE
		#endif
		);
	return snapshot;
}

DeterministicOrdinaryPathBatchResult
DeterministicOrdinaryPathBatch::result(std::size_t requestIndex) const
{
	DeterministicOrdinaryPathBatchResult result = {};
	if (m_state == nullptr || m_state->work == nullptr ||
		!m_state->completed || requestIndex >= m_state->requestCount)
	{
		return result;
	}
	const OrdinaryPathRequestWork &work = m_state->work->requests[requestIndex];
	const unsigned state = work.executionState.load(std::memory_order_acquire);
	if (state != DIRECT_PATH_WORK_WORKER
		#if defined(_WIN64)
		&& state != DIRECT_PATH_WORK_INLINE
		#endif
		) return result;
	result.points = work.points.empty() ? nullptr : work.points.data();
	result.pointCount = work.points.size();
	result.allocationOrder = work.allocationOrder.empty() ? nullptr :
		work.allocationOrder.data();
	result.allocationCount = work.allocationOrder.size();
	result.cleanupOrder = work.cleanupOrder.empty() ? nullptr :
		work.cleanupOrder.data();
	result.cleanupCount = work.cleanupOrder.size();
	result.passableBlockIndices = work.passableBlocks.empty() ? nullptr :
		work.passableBlocks.data();
	result.passableBlockCount = work.passableBlocks.size();
	result.snapshotGeneration = work.result.snapshotGeneration;
	result.objectId = work.request.objectId;
	result.expandedNodeCount = work.result.expandedNodeCount;
	result.discoveredNodeCount = work.result.discoveredNodeCount;
	result.cumulativeCellCount = work.result.cumulativeCellCount;
	result.ownerToken = work.ownerToken;
	result.materializationPlanHash = work.materializationPlanHash;
	result.hierarchyAllPassable = work.result.hierarchyAllPassable != 0;
	result.status = work.result.status;
	return result;
}

unsigned GetDeterministicOrdinaryPathLateDrainExecutionCount()
{
	return s_ordinaryPathLateDrainExecutions.load(std::memory_order_acquire);
}

} // namespace rts
