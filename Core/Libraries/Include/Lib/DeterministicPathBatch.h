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
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#endif

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
#if defined(_WIN64)
	// Authenticated native ordinary body; neither worker execution nor owner-help.
	, DIRECT_PATH_EXECUTION_INLINE
#endif
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
	bool referenceAdmissionAccepted;
	bool completed;
	bool timedOut;
};

inline bool IsDeterministicDirectPathConcurrentMultiWorkerBatch(
	const DeterministicDirectPathBatchExecutionSnapshot &execution)
{
	return execution.distinctPhysicalWorkerCount > 1 &&
		execution.peakActiveWorkers > 1;
}

#if defined(_WIN64)
// Owner-only completion accounting shared by direct and ordinary path lanes.
// A batch is authoritative only when every captured request commits. Fallback
// flags describe an actual legacy path invocation, not merely discarded or
// deferred native work.
class DeterministicPathOwnerCompletion
{
public:
	DeterministicPathOwnerCompletion() noexcept;
	void reset(std::size_t expectedOperations) noexcept;
	void beginOperation() noexcept;
	void finishOperation(bool committed, bool materializationBegan) noexcept;
	void expectLegacyFallback() noexcept;
	bool beginLegacyFallback() noexcept;
	void completeLegacyFallback(bool entered) noexcept;
	bool committed() const noexcept;
	bool fallbackEntered() const noexcept;
	bool fallbackCompleted() const noexcept;

private:
	std::size_t m_expectedOperations;
	std::size_t m_completedOperations;
	bool m_allOperationsCommitted;
	bool m_activeOperationNeedsLegacyFallback;
	bool m_batchNeedsLegacyFallback;
	bool m_fallbackEntered;
	bool m_fallbackCompleted;
};
#endif

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
		unsigned workerWaitTimeoutMilliseconds = 50
#if defined(_WIN64)
		, performance::KernelPerformanceBatch *performanceBatch = nullptr
		, performance::KernelPerformanceReferenceLedger *performanceReferenceLedger = nullptr
		, performance::KernelPerformanceReferenceBatch *performanceReferenceBatch = nullptr
		, performance::KernelPerformanceAttempt performanceReferenceAttempt =
			performance::KernelPerformanceAttempt()
#endif
		);
	#if defined(_WIN64)
	bool collectPerformanceReference(JobSystem &jobs);
	#endif
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
	// False when one or more exact worker identities cannot be represented by
	// the fixed-width diagnostic mask.  Authority uses the explicit count.
	bool physicalWorkerMaskComplete;
	unsigned peakActiveWorkers;
	bool referenceAdmissionAccepted;
	bool completed;
	bool timedOut;
};

inline bool IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(
	const DeterministicOrdinaryPathBatchExecutionSnapshot &execution)
{
	return execution.distinctPhysicalWorkerCount > 1 &&
		execution.peakActiveWorkers > 1;
}

#if defined(_WIN64)
// Optional native-site observations. Null hooks leave native behavior unchanged.
// The range-planned callback is owner-call-borrowed. Request/release callback
// values are copied into native work; their context must outlive real drain and
// reference collection, including when executeSynchronously times out.
enum DeterministicOrdinaryPathTestEvent
{
	DETERMINISTIC_ORDINARY_PATH_TEST_RANGE_PLANNED = 0,
	// Reference-serial entry has no request/range coordinates (both are zero).
	DETERMINISTIC_ORDINARY_PATH_TEST_REFERENCE_SERIAL_ENTER,
	DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_ENTER,
	DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_EXIT,
	DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_ENTER,
	DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_DISCOVERED_ALLOCATION,
	DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_SORT,
	DETERMINISTIC_ORDINARY_PATH_TEST_AFTER_SORT,
	DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_BYTE_ARITHMETIC,
	DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_GRANT,
	DETERMINISTIC_ORDINARY_PATH_TEST_PHYSICAL_RESERVE,
	DETERMINISTIC_ORDINARY_PATH_TEST_GRANT_RETURNED,
	// May inject allocation failure inside the existing output-allocation try.
	DETERMINISTIC_ORDINARY_PATH_TEST_FIRST_OUTPUT_ALLOCATION,
	DETERMINISTIC_ORDINARY_PATH_TEST_REFUND_COMPLETED,
	DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_FINAL_VALIDATION,
	DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_HASH,
	DETERMINISTIC_ORDINARY_PATH_TEST_AFTER_HASH,
	DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_EXIT,
	DETERMINISTIC_ORDINARY_PATH_TEST_BODY_EXIT
};

struct DeterministicOrdinaryPathTestHooks
{
	DeterministicOrdinaryPathTestHooks() : context(0), observe(0),
		observeRequest(0), observeReleasedBudget(0), observeReleasedRange(0),
		observeReferenceResult(0) {}
	void *context;
	void (*observe)(void *, DeterministicOrdinaryPathTestEvent,
		unsigned rangeIndex, std::size_t begin, std::size_t end);
	void (*observeRequest)(void *, DeterministicOrdinaryPathTestEvent,
		unsigned rangeIndex, std::size_t requestIndex, std::size_t actualBytes,
		bool actualGranted);
	void (*observeReleasedBudget)(void *, const performance::KernelPerformanceRangePlan &,
		const performance::KernelPerformanceRequestBudget &);
	void (*observeReleasedRange)(void *, const performance::KernelPerformanceRangePlan &,
		const performance::KernelPerformanceRangeProgress &);
	// Read-only values from the actual production view after native completion.
	void (*observeReferenceResult)(void *, std::size_t requestIndex,
		DeterministicPathSearchStatus actualStatus, std::size_t actualSearchPointCount,
		std::size_t actualMaterializedPointCount, std::size_t actualCanonicalPointCount,
		bool actualCanonicalHasPoints);
};
#endif

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
		unsigned workerWaitTimeoutMilliseconds = 50
#if defined(_WIN64)
		, performance::KernelPerformanceBatch *performanceBatch = nullptr
		, performance::KernelPerformanceReferenceLedger *performanceReferenceLedger = nullptr
		, performance::KernelPerformanceReferenceBatch *performanceReferenceBatch = nullptr
		, const DeterministicOrdinaryPathTestHooks *testHooks = nullptr
		, performance::KernelPerformanceAttempt performanceReferenceAttempt =
			performance::KernelPerformanceAttempt()
#endif
		);
#if defined(_WIN64)
	bool collectPerformanceReference(JobSystem &jobs);
#endif
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
