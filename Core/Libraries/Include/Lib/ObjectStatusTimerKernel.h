/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceReference.h"
#endif

namespace rts
{
#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned __int64 ObjectStatusTimerMetricCounter;
#else
typedef unsigned long long ObjectStatusTimerMetricCounter;
#endif

enum
{
	OBJECT_STATUS_TIMER_MAX_TYPES = 16,
	OBJECT_STATUS_TIMER_MINIMUM_PARALLEL_SNAPSHOTS = 256,
	OBJECT_STATUS_TIMER_DEFAULT_MINIMUM_GRAIN = 128,
	OBJECT_STATUS_TIMER_MAXIMUM_JOBS = 64,
	OBJECT_STATUS_TIMER_MAXIMUM_SNAPSHOTS = 16384
};

// Pointer-free owner snapshot. ownerOrder is the object's ordinal in the
// legacy GameLogic list and preserves cross-object callback order at commit.
struct ObjectStatusTimerSnapshot
{
	unsigned objectID;
	unsigned ownerOrder;
	unsigned activeMask;
	unsigned expirationFrame[OBJECT_STATUS_TIMER_MAX_TYPES];
};

struct ObjectStatusTimerCommand
{
	unsigned objectID;
	unsigned ownerOrder;
	unsigned expiredMask;
};

enum ObjectStatusTimerResult
{
	OBJECT_STATUS_TIMER_SERIAL = 0,
	OBJECT_STATUS_TIMER_PARALLEL,
	OBJECT_STATUS_TIMER_SERIAL_FALLBACK,
	OBJECT_STATUS_TIMER_INVALID_INPUT
};

#if defined(_WIN64)
class SimulationCommand;
// Optional native-site observations and fault injection. Null hooks leave the
// shipping scheduler, body and publication behavior unchanged.
enum ObjectStatusTimerTestEvent
{
	OBJECT_STATUS_TIMER_TEST_RANGE_ENTERED,
	OBJECT_STATUS_TIMER_TEST_ITEM_EVALUATED,
	OBJECT_STATUS_TIMER_TEST_COMMAND_APPENDED,
	OBJECT_STATUS_TIMER_TEST_RANGE_FINISHED,
	OBJECT_STATUS_TIMER_TEST_OWNER_REDUCTION,
	OBJECT_STATUS_TIMER_TEST_PUBLICATION,
	OBJECT_STATUS_TIMER_TEST_RANGE_RELEASED
};
enum ObjectStatusTimerTestCheckpoint
{
	OBJECT_STATUS_TIMER_TEST_CHECKPOINT_ENTRY,
	OBJECT_STATUS_TIMER_TEST_CHECKPOINT_BLOCK,
	OBJECT_STATUS_TIMER_TEST_CHECKPOINT_POST_BODY
};
struct ObjectStatusTimerTestHooks
{
	ObjectStatusTimerTestHooks() : context(0), observe(0), checkpoint(0), beforeWait(0), afterCancel(0),
		releasedGroup(0), physicalWaitMilliseconds(0) {}
	void *context;
	void (*observe)(void *, ObjectStatusTimerTestEvent, unsigned rangeIndex,
		unsigned begin, unsigned end, unsigned completedWorkUnits, bool completed,
		SimulationCommand *mutableOwnerStorage);
	bool (*checkpoint)(void *, unsigned rangeIndex, ObjectStatusTimerTestCheckpoint,
		unsigned completedWorkUnits, bool actualCancellation);
	void (*beforeWait)(void *);
	void (*afterCancel)(void *);
	void (*releasedGroup)(void *, bool cancelled, unsigned completedBodies,
		unsigned submitted, unsigned reason);
	// Bounded readiness budget for an explicitly controlled test source only.
	unsigned physicalWaitMilliseconds;
};
#endif

struct ObjectStatusTimerOptions
{
	ObjectStatusTimerOptions();
	bool parallel;
	unsigned minimumGrain;
#if defined(_WIN64)
	// Optional owner token for observational kernel timing. VC6 keeps the
	// historical options layout and behavior unchanged.
	performance::KernelPerformanceBatch performanceBatch;
	// Optional reference evidence is injected by the owner. The kernel leaves
	// these inert until a native reference mode is active.
	performance::KernelPerformanceReferenceLedger *performanceReferenceLedger;
	performance::KernelPerformanceAttempt performanceReferenceAttempt;
	const ObjectStatusTimerTestHooks *testHooks;
	performance::KernelPerformanceReferenceBatch *performanceReferenceBatch;
	ObjectStatusTimerCommand *performanceReferenceOutput;
	unsigned performanceReferenceOutputCapacity;
#endif
};

struct ObjectStatusTimerMetrics
{
	ObjectStatusTimerMetrics();
	unsigned evaluatedSnapshots;
	unsigned emittedCommands;
	unsigned submittedJobs;
	unsigned completedJobs;
	unsigned physicalWorkerJobs;
	unsigned ownerHelpedJobs;
	ObjectStatusTimerMetricCounter physicalWorkerMask;
	unsigned distinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	// True once the authenticated source/reference admission decision accepts
	// this batch, including baseline inline execution with no physical submits.
	bool referenceAdmissionAccepted;
	unsigned peakConcurrentPhysicalWorkers;
	unsigned serialFallbacks;
};

struct ObjectStatusTimerRuntimeMetrics
{
	ObjectStatusTimerRuntimeMetrics();
	ObjectStatusTimerMetricCounter resetEpoch;
	ObjectStatusTimerMetricCounter authoritativeBatches;
	ObjectStatusTimerMetricCounter committedCommands;
	ObjectStatusTimerMetricCounter submittedJobs;
	ObjectStatusTimerMetricCounter completedJobs;
	ObjectStatusTimerMetricCounter physicalWorkerJobs;
	ObjectStatusTimerMetricCounter ownerHelpedJobs;
	ObjectStatusTimerMetricCounter physicalWorkerMask;
	unsigned maximumDistinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	unsigned maximumPeakConcurrentPhysicalWorkers;
	ObjectStatusTimerMetricCounter shadowExecutions;
	ObjectStatusTimerMetricCounter shadowCommands;
	ObjectStatusTimerMetricCounter shadowMatches;
	ObjectStatusTimerMetricCounter shadowMismatches;
	ObjectStatusTimerMetricCounter ownerFallbacks;
	ObjectStatusTimerMetricCounter staleRejections;
};

// Cheap live-adapter admission must run before the owner scans or allocates a
// snapshot. Forced-one/serial validation therefore pays exactly the legacy
// sweep cost and records no prepared-lane work.
inline bool ShouldPrepareLiveObjectStatusTimerSnapshot(bool policyEnabled,
	bool schedulerRunning, unsigned workerCount, bool workerThread,
	bool gameOwnerThread)
{
	return policyEnabled && schedulerRunning && workerCount > 1 &&
		!workerThread && gameOwnerThread;
}

// Workers receive immutable snapshots and own disjoint command/payload slots.
// The SimulationCommandBuffer merge is ordered by ownerOrder, then ObjectID and
// the fixed module adapter ID. Output is published only after every producer
// completes and the merged commands validate. Failure leaves outputCount and
// output storage untouched so the owner can execute the legacy loop.
ObjectStatusTimerResult PrepareObjectStatusTimerCommands(
	const ObjectStatusTimerSnapshot *snapshots, unsigned snapshotCount,
	unsigned currentFrame, unsigned disabledTypeCount,
	ObjectStatusTimerCommand *output, unsigned outputCapacity,
	const ObjectStatusTimerOptions &options, unsigned *outputCount,
	ObjectStatusTimerMetrics *metrics = 0);

bool ObjectStatusTimerCommandsEqual(const ObjectStatusTimerCommand *left,
	unsigned leftCount, const ObjectStatusTimerCommand *right,
	unsigned rightCount, unsigned *firstDifference);

void ResetObjectStatusTimerRuntimeMetrics();
ObjectStatusTimerRuntimeMetrics GetObjectStatusTimerRuntimeMetrics();
void RecordObjectStatusTimerAuthoritativeCommit(unsigned preparedCommandCount,
	unsigned committedCommandCount, const ObjectStatusTimerMetrics &sliceMetrics);
void RecordObjectStatusTimerShadow(bool matched, unsigned commandCount,
	const ObjectStatusTimerMetrics &sliceMetrics);
void RecordObjectStatusTimerOwnerFallback(bool stale);
}
