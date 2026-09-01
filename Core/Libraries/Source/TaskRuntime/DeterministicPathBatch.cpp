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
	unsigned workerWaitTimeoutMilliseconds)
{
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
	releaseDirectPathActiveSlot(batch);
	m_state->completed = joined && !group.failed() && !group.wasCancelled();
	if (!m_state->completed)
		return false;
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
	std::size_t requestCount, unsigned workerWaitTimeoutMilliseconds)
{
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
	releaseOrdinaryPathActiveSlot(batch);
	m_state->completed = joined && !group.failed() && !group.wasCancelled();
	if (!m_state->completed)
		return false;
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
