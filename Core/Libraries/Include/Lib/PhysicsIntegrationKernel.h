/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

namespace rts
{
#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned __int64 PhysicsIntegrationMetricCounter;
#else
typedef unsigned long long PhysicsIntegrationMetricCounter;
#endif

enum
{
	PHYSICS_INTEGRATION_MATRIX_FLOATS = 12,
	PHYSICS_INTEGRATION_VECTOR_FLOATS = 3,
	PHYSICS_INTEGRATION_DEFAULT_MINIMUM_GRAIN = 32,
	PHYSICS_INTEGRATION_MAXIMUM_SNAPSHOTS = 65536
};

enum PhysicsIntegrationFlags
{
	PHYSICS_INTEGRATION_APPLY_FRICTION_2D_WHEN_AIRBORNE = 1 << 0,
	PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN = 1 << 1,
	PHYSICS_INTEGRATION_MOTIVE = 1 << 2,
	PHYSICS_INTEGRATION_BRAKING = 1 << 3,
	PHYSICS_INTEGRATION_PROJECTILE = 1 << 4,
	PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW = 1 << 5,
	PHYSICS_INTEGRATION_AIRBORNE_AT_START = 1 << 6,
	PHYSICS_INTEGRATION_UPDATE_EVER_RUN = 1 << 7,
	PHYSICS_INTEGRATION_WAS_AIRBORNE_LAST_FRAME = 1 << 8
};

// Complete pointer-free owner snapshot for the prefix of the exact legacy
// PhysicsBehavior update. The prefix ends immediately before the first ground
// height/carrier query. Stable identity and generation fields are retained in
// worker output so the owner can reject stale work without dereferencing a
// pointer published by a worker.
struct PhysicsIntegrationSnapshot
{
	unsigned frame;
	unsigned worldEpoch;
	unsigned objectID;
	unsigned motionGeneration;
	unsigned physicsGeneration;
	unsigned wakePriority;
	unsigned heapOrdinal;
	unsigned flags;
	float matrix[PHYSICS_INTEGRATION_MATRIX_FLOATS];
	float position[PHYSICS_INTEGRATION_VECTOR_FLOATS];
	float acceleration[PHYSICS_INTEGRATION_VECTOR_FLOATS];
	float velocity[PHYSICS_INTEGRATION_VECTOR_FLOATS];
	float yawRate;
	float rollRate;
	float pitchRate;
	float gravity;
	float mass;
	float forwardFriction;
	float lateralFriction;
	float aerodynamicFriction;
	float pitchRollYawFactor;
	float centerOfMassOffset;
	float directionX;
	float directionY;
};

struct PhysicsIntegrationOutput
{
	unsigned frame;
	unsigned worldEpoch;
	unsigned objectID;
	unsigned motionGeneration;
	unsigned physicsGeneration;
	unsigned wakePriority;
	unsigned heapOrdinal;
	unsigned flags;
	float matrix[PHYSICS_INTEGRATION_MATRIX_FLOATS];
	float acceleration[PHYSICS_INTEGRATION_VECTOR_FLOATS];
	float velocity[PHYSICS_INTEGRATION_VECTOR_FLOATS];
	float yawRate;
	float rollRate;
	float pitchRate;
};

// Owner-only lookup metadata. Sorting these pointer-free entries never changes
// snapshot or sleepy-heap order; batchIndex retains the captured ordinal.
struct PhysicsIntegrationOwnerIndexEntry
{
	unsigned objectID;
	unsigned batchIndex;
};

enum PhysicsIntegrationBatchResult
{
	PHYSICS_INTEGRATION_PARALLEL = 0,
	PHYSICS_INTEGRATION_POLICY_INELIGIBLE,
	PHYSICS_INTEGRATION_SERIAL_FALLBACK,
	PHYSICS_INTEGRATION_CANCELLED,
	PHYSICS_INTEGRATION_INVALID_INPUT
};

enum PhysicsIntegrationTestFault
{
	PHYSICS_INTEGRATION_TEST_NO_FAULT = 0,
	PHYSICS_INTEGRATION_TEST_ALLOCATION_FAILURE,
	PHYSICS_INTEGRATION_TEST_GROUP_FAILURE,
	PHYSICS_INTEGRATION_TEST_JOB_ALLOCATION_FAILURE,
	PHYSICS_INTEGRATION_TEST_ADMISSION_FAILURE,
	PHYSICS_INTEGRATION_TEST_WORKER_FAILURE,
	PHYSICS_INTEGRATION_TEST_CANCEL_AFTER_ADMISSION,
	PHYSICS_INTEGRATION_TEST_NONFINITE_OUTPUT,
	PHYSICS_INTEGRATION_TEST_PHYSICAL_WAIT_TIMEOUT
};

struct PhysicsIntegrationOptions
{
	PhysicsIntegrationOptions();
	unsigned minimumGrain;
	PhysicsIntegrationTestFault testFault;
	unsigned testOrdinal;
};

struct PhysicsIntegrationMetrics
{
	PhysicsIntegrationMetrics();
	unsigned snapshotCount;
	unsigned rangeCount;
	unsigned effectiveMinimumGrain;
	unsigned submittedJobs;
	unsigned completedJobs;
	unsigned physicalWorkerJobs;
	unsigned ownerHelpedJobs;
	PhysicsIntegrationMetricCounter physicalWorkerMask;
	unsigned distinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	unsigned peakConcurrentPhysicalWorkers;
	unsigned serialFallbacks;
	unsigned allocatedBytes;
	PhysicsIntegrationMetricCounter captureNanoseconds;
	PhysicsIntegrationMetricCounter prepareNanoseconds;
	PhysicsIntegrationMetricCounter waitNanoseconds;
	PhysicsIntegrationMetricCounter commitNanoseconds;
	unsigned storageBytes;
	unsigned storageCapacityBytes;
	unsigned storageAllocations;
};

struct PhysicsIntegrationRuntimeMetrics
{
	PhysicsIntegrationRuntimeMetrics();
	PhysicsIntegrationMetricCounter resetEpoch;
	PhysicsIntegrationMetricCounter acceptedBatches;
	PhysicsIntegrationMetricCounter acceptedPrefixes;
	PhysicsIntegrationMetricCounter acceptedRanges;
	PhysicsIntegrationMetricCounter acceptedSubmittedJobs;
	PhysicsIntegrationMetricCounter acceptedCompletedJobs;
	PhysicsIntegrationMetricCounter acceptedPhysicalWorkerJobs;
	PhysicsIntegrationMetricCounter acceptedOwnerHelpedJobs;
	PhysicsIntegrationMetricCounter acceptedPhysicalWorkerMask;
	unsigned maximumAcceptedDistinctPhysicalWorkers;
	bool acceptedPhysicalWorkerMaskComplete;
	unsigned maximumAcceptedPeakConcurrentPhysicalWorkers;
	PhysicsIntegrationMetricCounter acceptedAllocatedBytes;
	PhysicsIntegrationMetricCounter acceptedCaptureNanoseconds;
	PhysicsIntegrationMetricCounter acceptedPrepareNanoseconds;
	PhysicsIntegrationMetricCounter acceptedWaitNanoseconds;
	PhysicsIntegrationMetricCounter acceptedCommitNanoseconds;
	PhysicsIntegrationMetricCounter acceptedStorageBytes;
	PhysicsIntegrationMetricCounter acceptedStorageCapacityBytes;
	PhysicsIntegrationMetricCounter acceptedStorageAllocations;
	PhysicsIntegrationMetricCounter shadowBatches;
	PhysicsIntegrationMetricCounter shadowPrefixes;
	PhysicsIntegrationMetricCounter shadowRanges;
	PhysicsIntegrationMetricCounter shadowSubmittedJobs;
	PhysicsIntegrationMetricCounter shadowCompletedJobs;
	PhysicsIntegrationMetricCounter shadowMatches;
	PhysicsIntegrationMetricCounter shadowMismatches;
	PhysicsIntegrationMetricCounter ownerFallbacks;
	PhysicsIntegrationMetricCounter ineligibleSlices;
	PhysicsIntegrationMetricCounter unexpectedFallbacks;
	PhysicsIntegrationMetricCounter staleRejections;
	PhysicsIntegrationMetricCounter circuitBreakerTrips;
};

// Scalar extraction of the exact gravity/friction/damping, Euler velocity,
// clamp, translation, and pitch/roll/yaw matrix prefix. It reads and writes no
// global or live game state.
bool ComputePhysicsIntegrationPrefix(
	const PhysicsIntegrationSnapshot &snapshot,
	PhysicsIntegrationOutput &output);
bool ValidatePhysicsIntegrationSnapshot(
	const PhysicsIntegrationSnapshot &snapshot);
PhysicsIntegrationMetricCounter PhysicsIntegrationClockNowNanoseconds();
bool BuildPhysicsIntegrationOwnerIndex(
	const PhysicsIntegrationSnapshot *snapshots, unsigned snapshotCount,
	PhysicsIntegrationOwnerIndexEntry *entries, unsigned entryCapacity);
bool FindPhysicsIntegrationOwnerIndex(
	const PhysicsIntegrationOwnerIndexEntry *entries, unsigned entryCount,
	unsigned objectID, unsigned *batchIndex);

// Scheduler-only admission check used by the owner before scanning or
// allocating any live PhysicsBehavior batch storage. Safety failures count one
// JobSystem serial fallback; a forced one-worker lane remains policy-ineligible.
PhysicsIntegrationBatchResult PreflightPhysicsIntegrationPrefixes();

// Workers write only disjoint scratch slots. The owner validates the complete
// wave after the fence and copies to output transactionally. Allocation,
// admission, cancellation, worker, or validation failure leaves output bytes
// untouched so every module can execute its complete legacy scalar update.
PhysicsIntegrationBatchResult PreparePhysicsIntegrationPrefixes(
	const PhysicsIntegrationSnapshot *snapshots,
	unsigned snapshotCount,
	PhysicsIntegrationOutput *output,
	unsigned outputCapacity,
	PhysicsIntegrationOutput *scratch,
	unsigned scratchCapacity,
	const PhysicsIntegrationOptions &options,
	PhysicsIntegrationMetrics *metrics = 0);

bool PhysicsIntegrationSnapshotsEqual(
	const PhysicsIntegrationSnapshot &left,
	const PhysicsIntegrationSnapshot &right,
	unsigned *firstField = 0);
bool PhysicsIntegrationOutputsEqual(
	const PhysicsIntegrationOutput &left,
	const PhysicsIntegrationOutput &right,
	unsigned *firstField = 0);
bool ValidatePhysicsIntegrationOutput(
	const PhysicsIntegrationSnapshot &snapshot,
	const PhysicsIntegrationOutput &output);
bool ValidatePhysicsIntegrationCommit(
	const PhysicsIntegrationSnapshot &captured,
	const PhysicsIntegrationSnapshot &current,
	const PhysicsIntegrationOutput &output,
	bool actualHeapRoot, bool objectResolved, bool exactPhysics);

void ResetPhysicsIntegrationRuntimeMetrics();
PhysicsIntegrationRuntimeMetrics GetPhysicsIntegrationRuntimeMetrics();
void RecordPhysicsIntegrationAuthoritativeCommit(unsigned prefixCount);
void RecordPhysicsIntegrationAuthoritativeSlice(unsigned prefixCount,
	const PhysicsIntegrationMetrics &sliceMetrics);
void RecordPhysicsIntegrationShadow(bool matched, unsigned prefixCount,
	const PhysicsIntegrationMetrics &sliceMetrics);
void RecordPhysicsIntegrationOwnerFallback(bool stale);
void RecordPhysicsIntegrationIneligibleSlice();
void RecordPhysicsIntegrationUnexpectedFallback();
void RecordPhysicsIntegrationCircuitBreakerTrip();
}
