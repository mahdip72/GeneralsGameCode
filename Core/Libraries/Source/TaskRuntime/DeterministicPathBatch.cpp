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
#include <vector>

namespace rts
{
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
};

#if defined(RTS_BUILD_CORE_EXTRAS)
std::atomic<unsigned> s_directPathTestPauseMask(0);
std::atomic<unsigned> s_directPathTestPauseReachedMask(0);
std::atomic<unsigned> s_directPathTestPauseReachedCount(0);
std::atomic<unsigned> s_directPathTestPauseReleasedMask(0);
std::atomic<unsigned> s_directPathTestFaultMask(0);

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

struct DirectPathBatchWork
{
	DirectPathBatchWork() : requestCount(0), activeWorkers(0),
		peakActiveWorkers(0), liveJobs(0), ownsActiveSlot(false) {}

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
};

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
		DirectPathWork &work = m_batch->requests[m_requestIndex];
		#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseDirectPathTest(1);
		#endif
		if (context.isCancellationRequested())
		{
			publishDirectPathCancellation(work);
			s_directPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
			return;
		}
		const bool workerExecution = context.isPhysicalWorkerExecution();
		const unsigned runningState = workerExecution ?
			DIRECT_PATH_WORK_RUNNING_WORKER : DIRECT_PATH_WORK_RUNNING_OWNER;
		unsigned expectedState = DIRECT_PATH_WORK_PENDING;
		if (!work.executionState.compare_exchange_strong(expectedState,
			runningState, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			if (expectedState == DIRECT_PATH_WORK_CANCELLED)
			{
				s_directPathLateDrainExecutions.fetch_add(1,
					std::memory_order_relaxed);
			}
			return;
		}

		if (workerExecution)
		{
			work.physicalWorkerIndex.store(context.physicalWorkerIndex(),
				std::memory_order_release);
			const unsigned active = m_batch->activeWorkers.fetch_add(1,
				std::memory_order_acq_rel) + 1;
			publishPeak(m_batch->peakActiveWorkers, active);
			#if defined(RTS_BUILD_CORE_EXTRAS)
			pauseDirectPathTest(2);
			#endif
			#if defined(RTS_BUILD_CORE_EXTRAS)
			if ((s_directPathTestFaultMask.load(std::memory_order_acquire) & 1) == 0)
			#endif
				FindDeterministicDirectPath(work.snapshot, work.result);
			m_batch->activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
		}
		else
		{
			#if defined(RTS_BUILD_CORE_EXTRAS)
			if ((s_directPathTestFaultMask.load(std::memory_order_acquire) & 1) == 0)
			#endif
				FindDeterministicDirectPath(work.snapshot, work.result);
		}

		#if defined(RTS_BUILD_CORE_EXTRAS)
		const bool injectedFailure =
			(s_directPathTestFaultMask.load(std::memory_order_acquire) & 1) != 0;
		#else
		const bool injectedFailure = false;
		#endif
		const unsigned completedState = workerExecution ?
			(injectedFailure ? DIRECT_PATH_WORK_FAILURE : DIRECT_PATH_WORK_WORKER) :
			(injectedFailure ? DIRECT_PATH_WORK_FAILURE : DIRECT_PATH_WORK_OWNER);
		expectedState = runningState;
		if (!work.executionState.compare_exchange_strong(expectedState,
			completedState, std::memory_order_release, std::memory_order_acquire) &&
			expectedState == DIRECT_PATH_WORK_CANCELLED)
		{
			s_directPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
		}
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
	if (snapshots == nullptr || requestCount < 2 ||
		requestCount > DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS ||
		workerWaitTimeoutMilliseconds == 0 || !jobs.isRunning() ||
		jobs.workerCount() == 0 || !jobs.isCurrentThread(JOB_OWNER_GAME))
	{
		return false;
	}
	unsigned expectedActiveBatches = 0;
	if (!s_activeDirectPathBatches.compare_exchange_strong(expectedActiveBatches,
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
		s_activeDirectPathBatches.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	DirectPathBatchWork &batch = *m_state->work;
	batch.ownsActiveSlot.store(true, std::memory_order_release);
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
	capture.end();
	PathPerformanceInterval schedule(performanceBatch,
		performance::KERNEL_PERFORMANCE_SCHEDULE);
	#endif
	const JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		m_state->work.reset();
		return false;
	}
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
		m_state->work.reset();
		return false;
	}
	if (!jobs.trySubmitBatch(submissions, static_cast<unsigned>(requestCount),
		group, handles))
	{
		for (std::size_t i = 0; i < requestCount; ++i)
			delete submissions[i].job;
		m_state->work.reset();
		return false;
	}
	m_state->submittedJobCount = requestCount;
	#if defined(_WIN64)
	schedule.end();
	PathPerformanceInterval wait(performanceBatch,
		performance::KERNEL_PERFORMANCE_WAIT);
	#endif

	#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_directPathTestPauseMask.load(std::memory_order_acquire) & 1) != 0 &&
		!waitForDirectPathTestPause(1, 1, 15000))
	{
		for (std::size_t i = 0; i < requestCount; ++i)
			publishDirectPathCancellation(batch.requests[i]);
		jobs.cancel(group);
		return false;
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
	#endif
	releaseDirectPathActiveSlot(batch);
	m_state->completed = joined && !group.failed() && !group.wasCancelled();
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
	#endif
	return true;
}

DeterministicDirectPathBatchExecutionSnapshot
DeterministicDirectPathBatch::executionSnapshot() const
{
	DeterministicDirectPathBatchExecutionSnapshot snapshot = {};
	if (m_state == nullptr)
		return snapshot;
	snapshot.requestCount = m_state->requestCount;
	snapshot.submittedJobCount = m_state->submittedJobCount;
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
	else if (state == DIRECT_PATH_WORK_FAILURE)
		snapshot.state = DIRECT_PATH_EXECUTION_FAILURE;
	if (snapshot.state == DIRECT_PATH_EXECUTION_WORKER)
	{
		snapshot.physicalWorkerIndex = work.physicalWorkerIndex.load(
			std::memory_order_acquire);
	}
	snapshot.succeeded = m_state->completed &&
		snapshot.state == DIRECT_PATH_EXECUTION_WORKER;
	return snapshot;
}

const DirectPathSearchResult &DeterministicDirectPathBatch::result(
	std::size_t requestIndex) const
{
	static const DirectPathSearchResult invalidResult = {};
	if (m_state == nullptr || m_state->work == nullptr ||
		!m_state->completed || requestIndex >= m_state->requestCount ||
		m_state->work->requests[requestIndex].executionState.load(
			std::memory_order_acquire) != DIRECT_PATH_WORK_WORKER)
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

struct OrdinaryPathBatchWork
{
	OrdinaryPathBatchWork() : requestCount(0), rangeCount(0), grainSize(0),
		activeWorkers(0), peakActiveWorkers(0), liveJobs(0),
		resultStorageBytes(0), ownsActiveSlot(false) {}

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
};

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
	view.pointCount = result.pointCount;
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

bool buildOrdinaryPathMaterializationPlan(OrdinaryPathBatchWork &batch,
	OrdinaryPathRangeWork &range, OrdinaryPathRequestWork &work)
{
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
	if (!reserveOrdinaryPathResultStorage(batch, outputBytes))
	{
		work.result.status = DETERMINISTIC_PATH_BUDGET_EXHAUSTED;
		return true;
	}

	try
	{
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
		batch.resultStorageBytes.fetch_sub(outputBytes, std::memory_order_acq_rel);
		work.points.clear();
		work.allocationOrder.clear();
		work.cleanupOrder.clear();
		work.passableBlocks.clear();
		return false;
	}
	work.result.points = work.points.data();
	work.result.pointCapacity = work.points.size();
	if (work.allocationOrder.size() != allocationCount ||
		work.cleanupOrder.size() !=
			static_cast<std::size_t>(work.result.cumulativeCellCount) ||
		work.passableBlocks.size() != work.result.passableBlockCount)
	{
		return false;
	}
	work.materializationPlanHash = ComputeDeterministicOrdinaryPathPlanHash(
		work.points.data(), work.points.size(), work.allocationOrder.data(),
		work.allocationOrder.size(), work.cleanupOrder.data(),
		work.cleanupOrder.size(), work.passableBlocks.data(),
		work.passableBlocks.size(), work.result.hierarchyAllPassable != 0,
		work.result.snapshotGeneration,
		work.request.objectId, work.ownerToken);
	return work.materializationPlanHash != 0;
}

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
		OrdinaryPathRangeWork &range = m_batch->ranges[m_rangeIndex];
		#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseDirectPathTest(ORDINARY_PATH_TEST_ENTRY_PAUSE);
		#endif
		if (context.isCancellationRequested())
		{
			publishOrdinaryPathCancellation(range);
			s_ordinaryPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
			return;
		}
		const bool workerExecution = context.isPhysicalWorkerExecution();
		const unsigned runningState = workerExecution ?
			DIRECT_PATH_WORK_RUNNING_WORKER : DIRECT_PATH_WORK_RUNNING_OWNER;
		unsigned expectedState = DIRECT_PATH_WORK_PENDING;
		if (!range.executionState.compare_exchange_strong(expectedState,
			runningState, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			if (expectedState == DIRECT_PATH_WORK_CANCELLED)
				s_ordinaryPathLateDrainExecutions.fetch_add(1,
					std::memory_order_relaxed);
			return;
		}

		if (workerExecution)
		{
			range.physicalWorkerIndex.store(context.physicalWorkerIndex(),
				std::memory_order_release);
			const unsigned active = m_batch->activeWorkers.fetch_add(1,
				std::memory_order_acq_rel) + 1;
			publishPeak(m_batch->peakActiveWorkers, active);
			#if defined(RTS_BUILD_CORE_EXTRAS)
			pauseDirectPathTest(ORDINARY_PATH_TEST_ACTIVE_PAUSE);
			#endif
		}

		bool succeeded = workerExecution;
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
			if (context.isCancellationRequested() ||
				range.executionState.load(std::memory_order_acquire) ==
					DIRECT_PATH_WORK_CANCELLED)
			{
				succeeded = false;
				break;
			}
			OrdinaryPathRequestWork &work = m_batch->requests[requestIndex];
			unsigned requestExpected = DIRECT_PATH_WORK_PENDING;
			if (!work.executionState.compare_exchange_strong(requestExpected,
				DIRECT_PATH_WORK_RUNNING_WORKER, std::memory_order_acq_rel,
				std::memory_order_acquire))
			{
				succeeded = false;
				break;
			}
			work.physicalWorkerIndex.store(context.physicalWorkerIndex(),
				std::memory_order_release);
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
			FindDeterministicPath(m_batch->grid, work.request, scratch,
				work.result);
			if (!buildOrdinaryPathMaterializationPlan(*m_batch, range, work))
			{
				work.executionState.store(DIRECT_PATH_WORK_FAILURE,
					std::memory_order_release);
				succeeded = false;
				break;
			}
			requestExpected = DIRECT_PATH_WORK_RUNNING_WORKER;
			if (!work.executionState.compare_exchange_strong(requestExpected,
				DIRECT_PATH_WORK_WORKER, std::memory_order_release,
				std::memory_order_acquire))
			{
				succeeded = false;
				break;
			}
		}

		if (workerExecution)
			m_batch->activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
		const unsigned completedState = succeeded ?
			DIRECT_PATH_WORK_WORKER : DIRECT_PATH_WORK_FAILURE;
		expectedState = runningState;
		if (!range.executionState.compare_exchange_strong(expectedState,
			completedState, std::memory_order_release, std::memory_order_acquire) &&
			expectedState == DIRECT_PATH_WORK_CANCELLED)
		{
			s_ordinaryPathLateDrainExecutions.fetch_add(1,
				std::memory_order_relaxed);
		}
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
	if (!s_activeOrdinaryPathBatches.compare_exchange_strong(
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
		s_activeOrdinaryPathBatches.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	OrdinaryPathBatchWork &batch = *m_state->work;
	batch.ownsActiveSlot.store(true, std::memory_order_release);
	batch.grid = grid;
	batch.grid.cells = batch.cells.data();
	batch.requestCount = requestCount;
	batch.rangeCount = rangeCount;
	batch.grainSize = grainSize;
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
			range.begin = static_cast<std::size_t>(rangeIndex) * grainSize;
			range.end = std::min(requestCount, range.begin + grainSize);
			range.nodes.resize(cellCount);
			range.heap.resize(cellCount);
			range.pointScratch.resize(cellCount);
			range.hierarchyPassableScratch.resize(hierarchyBlockCount);
			range.hierarchyBlockScratch.resize(hierarchyBlockCount);
		}
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
	#endif

	const JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		m_state->work.reset();
		return false;
	}
	std::vector<JobSubmission> submissions(rangeCount);
	std::vector<JobHandle> handles(rangeCount);
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
		m_state->work.reset();
		return false;
	}
	if (!jobs.trySubmitBatch(submissions.data(), rangeCount, group,
		handles.data()))
	{
		for (unsigned i = 0; i < rangeCount; ++i)
			delete submissions[i].job;
		m_state->work.reset();
		return false;
	}
	m_state->submittedRangeJobCount = rangeCount;
	#if defined(_WIN64)
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
	#endif
	releaseOrdinaryPathActiveSlot(batch);
	m_state->completed = joined && !group.failed() && !group.wasCancelled();
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
	#endif
	return true;
}

DeterministicOrdinaryPathBatchExecutionSnapshot
DeterministicOrdinaryPathBatch::executionSnapshot() const
{
	DeterministicOrdinaryPathBatchExecutionSnapshot snapshot = {};
	snapshot.physicalWorkerMaskComplete = true;
	if (m_state == nullptr)
		return snapshot;
	snapshot.requestCount = m_state->requestCount;
	snapshot.submittedRangeJobCount = m_state->submittedRangeJobCount;
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
	if (snapshot.state == DIRECT_PATH_EXECUTION_WORKER)
		snapshot.physicalWorkerIndex = work.physicalWorkerIndex.load(
			std::memory_order_acquire);
	snapshot.succeeded = m_state->completed &&
		snapshot.state == DIRECT_PATH_EXECUTION_WORKER;
	return snapshot;
}

DeterministicOrdinaryPathBatchResult
DeterministicOrdinaryPathBatch::result(std::size_t requestIndex) const
{
	DeterministicOrdinaryPathBatchResult result = {};
	if (m_state == nullptr || m_state->work == nullptr ||
		!m_state->completed || requestIndex >= m_state->requestCount ||
		m_state->work->requests[requestIndex].executionState.load(
			std::memory_order_acquire) != DIRECT_PATH_WORK_WORKER)
	{
		return result;
	}
	const OrdinaryPathRequestWork &work = m_state->work->requests[requestIndex];
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
