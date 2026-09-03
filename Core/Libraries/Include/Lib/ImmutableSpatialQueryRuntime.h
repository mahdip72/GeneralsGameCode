/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/ImmutableSpatialQuery.h"
#include "Lib/JobSystem.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#endif

namespace rts
{
enum ImmutableSpatialConsumer
{
	IMMUTABLE_SPATIAL_CONSUMER_HEALING = 0,
	IMMUTABLE_SPATIAL_CONSUMER_POINT_DEFENSE_LASER,
	IMMUTABLE_SPATIAL_CONSUMER_COUNT
};

enum ImmutableSpatialJobSystemResult
{
	IMMUTABLE_SPATIAL_JOB_SYSTEM_SUCCESS = 0,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_INELIGIBLE,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_CANCELLED,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_FAILED
};

typedef JobMetricCounter ImmutableSpatialMetricCounter;

enum ImmutableSpatialAdmissionResult
{
	IMMUTABLE_SPATIAL_ADMISSION_ELIGIBLE = 0,
	IMMUTABLE_SPATIAL_ADMISSION_POLICY_INELIGIBLE,
	IMMUTABLE_SPATIAL_ADMISSION_INVALID
};

// Deterministic structural work estimated without walking the live partition.
// maximumRangeCost is the actual maximum cost of the contiguous query ranges
// that the kernel will dispatch. Owner scan/sort/lookup work is included so a
// rejected collection models the complete transaction, not only worker work.
struct ImmutableSpatialAdmissionCost
{
	ImmutableSpatialAdmissionCost();

	unsigned queryCount;
	unsigned workerCount;
	ImmutableSpatialMetricCounter queryCellVisits;
	ImmutableSpatialMetricCounter queryMemberVisits;
	ImmutableSpatialMetricCounter objectCount;
	ImmutableSpatialMetricCounter cellCount;
	ImmutableSpatialMetricCounter memberCount;
	ImmutableSpatialMetricCounter radiusOffsetCount;
	ImmutableSpatialMetricCounter maximumRangeCost;
	ImmutableSpatialMetricCounter ownerScanCount;
	ImmutableSpatialMetricCounter ownerSortComparisons;
	ImmutableSpatialMetricCounter ownerLookupComparisons;
	bool rebuildTopology;
	bool refreshFacts;
};

enum ImmutableSpatialJobSystemTestFault
{
	IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_NONE = 0,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_GROUP_FAILURE,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_ADMISSION_FAILURE,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_CANCEL_AFTER_ADMISSION,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_RANGE_FAILURE,
	IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_TIMEOUT
};

#if defined(_WIN64)
struct ImmutableSpatialQueryOwnerIdentity
{
	ImmutableSpatialUInt32 objectID;
	ImmutableSpatialConsumer consumer;
	ImmutableSpatialUInt32 wakePriority;
};

struct ImmutableSpatialConsumerCompletionToken
{
	ImmutableSpatialConsumerCompletionToken();
	ImmutableSpatialUInt32 batchEpoch;
	ImmutableSpatialUInt32 queryOrdinal;
};

// Owner-only observational state, shared with the focused runtime tests.
// It never decides whether a consumer may publish authoritative gameplay.
struct ImmutableSpatialCollectionCompletion
{
	enum { MAXIMUM_QUERIES = 256 };
	ImmutableSpatialCollectionCompletion();
	void reset(ImmutableSpatialUInt32 epoch);
	bool pending(const ImmutableSpatialConsumerCompletionToken &token) const;
	bool complete(ImmutableSpatialConsumer consumer,
		const ImmutableSpatialConsumerCompletionToken &token, bool committed);
	bool finished() const;

	ImmutableSpatialUInt32 batchEpoch;
	unsigned expectedConsumers;
	unsigned completedConsumers;
	bool allConsumersCommitted;
private:
	bool m_completed[MAXIMUM_QUERIES];
};
#endif

struct ImmutableSpatialJobSystemOptions
{
	ImmutableSpatialJobSystemOptions();

	ImmutableSpatialJobSystemTestFault testFault;
	unsigned testDispatchOrdinal;
	unsigned testRangeOrdinal;
	unsigned testSpinIterations;
#if defined(_WIN64)
	// Optional owner-created diagnostic identity. The shared executor only
	// consumes this transport for observational timing; invalid tokens are
	// inert and never change admission, fallback, or publication behavior.
	performance::KernelPerformanceLedger *performanceLedger;
	performance::KernelPerformanceBatch performanceBatch;
	performance::KernelPerformanceReferenceLedger *referenceLedger;
	performance::KernelPerformanceReferenceBatch *referenceBatch;
	const ImmutableSpatialQueryOwnerIdentity *queryOwners;
	ImmutableSpatialUInt32 queryOwnerCount;
#endif
};

struct ImmutableSpatialJobSystemMetrics
{
	ImmutableSpatialJobSystemMetrics();

	unsigned dispatches;
	unsigned ranges;
	unsigned submittedJobs;
	unsigned completedJobs;
	unsigned physicalWorkerJobs;
	unsigned ownerHelpedJobs;
	JobMetricCounter physicalWorkerMask;
	unsigned distinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	// Exact maximum number of physical workers executing either dispatch of
	// this immutable collection at the same time. This is non-authoritative.
	unsigned peakConcurrentPhysicalWorkers;
};

struct ImmutableSpatialConsumerRuntimeMetrics
{
	ImmutableSpatialConsumerRuntimeMetrics();

	ImmutableSpatialMetricCounter eligibleQueries;
	ImmutableSpatialMetricCounter authoritativeQueries;
	ImmutableSpatialMetricCounter authoritativeCandidates;
	ImmutableSpatialMetricCounter shadowQueries;
	ImmutableSpatialMetricCounter shadowMatches;
	ImmutableSpatialMetricCounter shadowMismatches;
	ImmutableSpatialMetricCounter submittedJobs;
	ImmutableSpatialMetricCounter completedJobs;
	ImmutableSpatialMetricCounter physicalWorkerJobs;
	ImmutableSpatialMetricCounter ownerHelpedJobs;
	ImmutableSpatialMetricCounter expectedFallbacks;
	ImmutableSpatialMetricCounter unexpectedFallbacks;
	ImmutableSpatialMetricCounter staleRejections;
	ImmutableSpatialMetricCounter validationFailures;
	ImmutableSpatialMetricCounter circuitBreakerTrips;
};

struct ImmutableSpatialRuntimeMetrics
{
	ImmutableSpatialRuntimeMetrics();

	ImmutableSpatialMetricCounter resetEpoch;
	ImmutableSpatialMetricCounter capturedArenas;
	ImmutableSpatialMetricCounter captureFailures;
	ImmutableSpatialMetricCounter successfulCollections;
	ImmutableSpatialMetricCounter successfulCollectionQueries;
	ImmutableSpatialMetricCounter successfulCollectionRanges;
	ImmutableSpatialMetricCounter multiRangeCollections;
	ImmutableSpatialMetricCounter collectionSubmittedJobs;
	ImmutableSpatialMetricCounter collectionCompletedJobs;
	ImmutableSpatialMetricCounter collectionPhysicalWorkerJobs;
	ImmutableSpatialMetricCounter collectionOwnerHelpedJobs;
	ImmutableSpatialMetricCounter collectionPhysicalWorkerMask;
	bool collectionPhysicalWorkerMaskComplete;
	ImmutableSpatialMetricCounter maximumCollectionQueries;
	ImmutableSpatialMetricCounter maximumCollectionRanges;
	ImmutableSpatialMetricCounter maximumCollectionDistinctPhysicalWorkers;
	ImmutableSpatialMetricCounter maximumCollectionPeakConcurrentPhysicalWorkers;
	ImmutableSpatialConsumerRuntimeMetrics healing;
	ImmutableSpatialConsumerRuntimeMetrics pointDefenseLaser;
};

// The wrapper supplies only physical JobSystem workers. The owner passively
// waits for both immutable count/fill passes, validates every handle, and
// rejects the transaction if owner help occurred. On failure the kernel's
// publication buffers and output count remain untouched.
ImmutableSpatialJobSystemResult ExecuteImmutableSpatialQueryBatchOnJobSystem(
	const void *arena,
	ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialQuery *queries,
	ImmutableSpatialUInt32 queryCount,
	ImmutableSpatialArenaGenerationResolver arenaResolver,
	ImmutableSpatialObjectGenerationResolver objectResolver,
	void *generationContext,
	const ImmutableSpatialBatchScratch &scratch,
	ImmutableSpatialResult *output,
	ImmutableSpatialUInt32 outputCapacity,
	ImmutableSpatialResultSpan *outputSpans,
	ImmutableSpatialUInt32 outputSpanCapacity,
	ImmutableSpatialUInt32 *outputCount,
	const ImmutableSpatialJobSystemOptions &options,
	ImmutableSpatialJobSystemMetrics *jobMetrics,
	ImmutableSpatialExecutionMetrics *executionMetrics = 0,
	ImmutableSpatialStatus *kernelStatus = 0);

// Live consumers intentionally do not submit a one-query/one-range job and
// wait for it. Only a real multi-query collection with at least two available
// physical workers is eligible for the JobSystem wrapper.
bool ShouldDispatchImmutableSpatialQueryCollection(unsigned queryCount,
	unsigned workerCount);

ImmutableSpatialAdmissionResult EvaluateImmutableSpatialQueryAdmission(
	const ImmutableSpatialAdmissionCost &cost,
	ImmutableSpatialMetricCounter *legacyCost = 0,
	ImmutableSpatialMetricCounter *parallelCost = 0);

void ResetImmutableSpatialRuntimeMetrics();
ImmutableSpatialRuntimeMetrics GetImmutableSpatialRuntimeMetrics();
void RecordImmutableSpatialArenaCapture(bool succeeded);
// A collection is recorded only after one all-or-nothing multi-query kernel
// execution succeeds. rangeCount is the number of concurrently dispatchable
// deterministic query ranges, while job metrics include both count/fill passes.
void RecordImmutableSpatialSuccessfulCollection(unsigned queryCount,
	unsigned rangeCount, const ImmutableSpatialJobSystemMetrics &metrics);
void RecordImmutableSpatialEligibleQuery(ImmutableSpatialConsumer consumer);
void RecordImmutableSpatialAuthoritativeQuery(ImmutableSpatialConsumer consumer,
	unsigned candidateCount, const ImmutableSpatialJobSystemMetrics &metrics);
void RecordImmutableSpatialShadowQuery(ImmutableSpatialConsumer consumer,
	bool matched, const ImmutableSpatialJobSystemMetrics &metrics);
void RecordImmutableSpatialExpectedFallback(ImmutableSpatialConsumer consumer);
void RecordImmutableSpatialUnexpectedFallback(ImmutableSpatialConsumer consumer,
	bool stale, bool validationFailure,
	const ImmutableSpatialJobSystemMetrics *metrics = 0);
void RecordImmutableSpatialCircuitBreakerTrip(ImmutableSpatialConsumer consumer);
}
