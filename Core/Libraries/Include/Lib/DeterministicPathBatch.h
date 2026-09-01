/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/DeterministicPathSearch.h"

#include <cstddef>

namespace rts
{

class JobSystem;

enum DeterministicDirectPathExecutionState
{
	DIRECT_PATH_EXECUTION_PENDING = 0,
	DIRECT_PATH_EXECUTION_CANCELLED,
	DIRECT_PATH_EXECUTION_WORKER,
	DIRECT_PATH_EXECUTION_OWNER,
	DIRECT_PATH_EXECUTION_FAILURE
};

struct DeterministicDirectPathExecutionSnapshot
{
	DeterministicDirectPathExecutionState state;
	unsigned physicalWorkerIndex;
	bool submitted;
	bool succeeded;
};

enum
{
	DETERMINISTIC_DIRECT_PATH_MAX_BATCH_REQUESTS = 16
};

struct DeterministicDirectPathBatchExecutionSnapshot
{
	std::size_t requestCount;
	std::size_t submittedJobCount;
	std::size_t workerExecutedJobCount;
	std::size_t ownerExecutedJobCount;
	std::size_t failedJobCount;
	unsigned distinctPhysicalWorkerCount;
	unsigned peakActiveWorkers;
	bool completed;
	bool timedOut;
};

inline bool IsDeterministicDirectPathConcurrentMultiWorkerBatch(
	const DeterministicDirectPathBatchExecutionSnapshot &execution)
{
	return execution.distinctPhysicalWorkerCount > 1 &&
		execution.peakActiveWorkers > 1;
}

// One bounded immutable request set is admitted as one JobGroup.  Every job
// owns one request-local snapshot/result, workers never wait, and the owner
// passively waits for the group before one non-helping join.
class DeterministicDirectPathBatch
{
public:
	DeterministicDirectPathBatch();
	~DeterministicDirectPathBatch();

	bool executeSynchronously(JobSystem &jobs,
		const DirectPathSnapshot *snapshots,
		std::size_t requestCount,
		unsigned workerWaitTimeoutMilliseconds = 50);
	DeterministicDirectPathBatchExecutionSnapshot executionSnapshot() const;
	DeterministicDirectPathExecutionSnapshot requestExecutionSnapshot(
		std::size_t requestIndex) const;
	const DirectPathSearchResult &result(std::size_t requestIndex) const;

private:
	DeterministicDirectPathBatch(const DeterministicDirectPathBatch &);
	DeterministicDirectPathBatch &operator=(const DeterministicDirectPathBatch &);
	struct State;
	State *m_state;
};

// Process-local diagnostic for work that reached DirectPathJob::execute but
// lost the owner-side timeout/cancellation race.  It is never an accepted
// execution identity and never grants path authority.
unsigned GetDeterministicDirectPathLateDrainExecutionCount();

struct DeterministicOrdinaryPathBatchRequest
{
	DeterministicPathRequest search;
	std::uint64_t ownerToken;
};

struct DeterministicOrdinaryPathBatchResult
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
	std::uint32_t cumulativeCellCount;
	std::uint64_t ownerToken;
	std::uint64_t materializationPlanHash;
	bool hierarchyAllPassable;
	DeterministicPathSearchStatus status;
};

std::uint64_t ComputeDeterministicOrdinaryPathPlanHash(
	const DeterministicPathPoint *points, std::size_t pointCount,
	const std::uint32_t *allocationOrder, std::size_t allocationCount,
	const std::uint32_t *cleanupOrder, std::size_t cleanupCount,
	const std::uint32_t *passableBlockIndices, std::size_t passableBlockCount,
	bool hierarchyAllPassable,
	std::uint32_t snapshotGeneration, std::uint32_t objectId,
	std::uint64_t ownerToken) noexcept;

struct DeterministicOrdinaryPathBatchExecutionSnapshot
{
	std::size_t requestCount;
	std::size_t submittedRangeJobCount;
	std::size_t workerExecutedRangeJobCount;
	std::size_t ownerExecutedRangeJobCount;
	std::size_t failedRangeJobCount;
	std::size_t resultStorageBytes;
	unsigned rangeCount;
	unsigned grainSize;
	std::uint64_t physicalWorkerMask;
	unsigned distinctPhysicalWorkerCount;
	unsigned peakActiveWorkers;
	bool completed;
	bool timedOut;
};

inline bool IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(
	const DeterministicOrdinaryPathBatchExecutionSnapshot &execution)
{
	return execution.distinctPhysicalWorkerCount > 1 &&
		execution.peakActiveWorkers > 1;
}

// Adaptive, memory-bounded independent-request A*. One immutable navigation
// generation and one JobGroup are shared by the batch; each contiguous range
// owns request-local search scratch. There is intentionally no request-count
// product cap. Memory pressure, cancellation or any incomplete physical-worker
// execution returns false and leaves every owner request on its serial lane.
class DeterministicOrdinaryPathBatch
{
public:
	DeterministicOrdinaryPathBatch();
	~DeterministicOrdinaryPathBatch();

	bool executeSynchronously(JobSystem &jobs,
		const ImmutableNavigationGrid &grid,
		const DeterministicOrdinaryPathBatchRequest *requests,
		std::size_t requestCount,
		unsigned workerWaitTimeoutMilliseconds = 50);
	DeterministicOrdinaryPathBatchExecutionSnapshot executionSnapshot() const;
	DeterministicDirectPathExecutionSnapshot requestExecutionSnapshot(
		std::size_t requestIndex) const;
	DeterministicOrdinaryPathBatchResult result(std::size_t requestIndex) const;

private:
	DeterministicOrdinaryPathBatch(const DeterministicOrdinaryPathBatch &);
	DeterministicOrdinaryPathBatch &operator=(
		const DeterministicOrdinaryPathBatch &);
	struct State;
	State *m_state;
};

unsigned GetDeterministicOrdinaryPathLateDrainExecutionCount();

} // namespace rts
