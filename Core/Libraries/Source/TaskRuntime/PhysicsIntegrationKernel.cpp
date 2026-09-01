/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/PhysicsIntegrationKernel.h"

#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"

#include <float.h>
#include <math.h>
#include <new>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER < 1300
#include <sys/timeb.h>
#else
#include <atomic>
#include <chrono>
#endif

namespace rts
{
namespace
{
#if defined(_MSC_VER) && _MSC_VER < 1300
typedef PhysicsIntegrationMetricCounter PhysicsMetricAtomic;
inline PhysicsIntegrationMetricCounter loadMetric(const PhysicsMetricAtomic &value) { return value; }
inline void resetMetric(PhysicsMetricAtomic &value) { value = 0; }
inline void addMetric(PhysicsMetricAtomic &value, PhysicsIntegrationMetricCounter amount) { value += amount; }
#else
typedef std::atomic<PhysicsIntegrationMetricCounter> PhysicsMetricAtomic;
inline PhysicsIntegrationMetricCounter loadMetric(const PhysicsMetricAtomic &value)
{
	return value.load(std::memory_order_relaxed);
}
inline void resetMetric(PhysicsMetricAtomic &value)
{
	value.store(0, std::memory_order_relaxed);
}
inline void addMetric(PhysicsMetricAtomic &value, PhysicsIntegrationMetricCounter amount)
{
	value.fetch_add(amount, std::memory_order_relaxed);
}
#endif

#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned PhysicsJobAtomicUnsigned;
inline unsigned incrementJobCounter(PhysicsJobAtomicUnsigned &value) { return ++value; }
inline void decrementJobCounter(PhysicsJobAtomicUnsigned &value) { --value; }
inline unsigned loadJobCounter(const PhysicsJobAtomicUnsigned &value) { return value; }
inline void maximizeJobCounter(PhysicsJobAtomicUnsigned &value, unsigned candidate)
{
	if (candidate > value) value = candidate;
}
#else
typedef std::atomic<unsigned> PhysicsJobAtomicUnsigned;
inline unsigned incrementJobCounter(PhysicsJobAtomicUnsigned &value)
{
	return value.fetch_add(1, std::memory_order_acq_rel) + 1;
}
inline void decrementJobCounter(PhysicsJobAtomicUnsigned &value)
{
	value.fetch_sub(1, std::memory_order_acq_rel);
}
inline unsigned loadJobCounter(const PhysicsJobAtomicUnsigned &value)
{
	return value.load(std::memory_order_relaxed);
}
inline void maximizeJobCounter(PhysicsJobAtomicUnsigned &value, unsigned candidate)
{
	unsigned observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
#endif

inline void orMetric(PhysicsMetricAtomic &value, PhysicsIntegrationMetricCounter mask)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	value |= mask;
#else
	value.fetch_or(mask, std::memory_order_relaxed);
#endif
}

inline void maximizeMetric(PhysicsMetricAtomic &value,
	PhysicsIntegrationMetricCounter candidate)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	if (candidate > value) value = candidate;
#else
	PhysicsIntegrationMetricCounter observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
#endif
}

PhysicsMetricAtomic s_acceptedBatches;
PhysicsMetricAtomic s_resetEpoch;
PhysicsMetricAtomic s_acceptedPrefixes;
PhysicsMetricAtomic s_acceptedRanges;
PhysicsMetricAtomic s_acceptedSubmittedJobs;
PhysicsMetricAtomic s_acceptedCompletedJobs;
PhysicsMetricAtomic s_acceptedPhysicalWorkerJobs;
PhysicsMetricAtomic s_acceptedOwnerHelpedJobs;
PhysicsMetricAtomic s_acceptedPhysicalWorkerMask;
PhysicsMetricAtomic s_maximumAcceptedDistinctPhysicalWorkers;
PhysicsMetricAtomic s_maximumAcceptedPeakConcurrentPhysicalWorkers;
PhysicsMetricAtomic s_acceptedAllocatedBytes;
PhysicsMetricAtomic s_acceptedCaptureNanoseconds;
PhysicsMetricAtomic s_acceptedPrepareNanoseconds;
PhysicsMetricAtomic s_acceptedWaitNanoseconds;
PhysicsMetricAtomic s_acceptedCommitNanoseconds;
PhysicsMetricAtomic s_acceptedStorageBytes;
PhysicsMetricAtomic s_acceptedStorageCapacityBytes;
PhysicsMetricAtomic s_acceptedStorageAllocations;
PhysicsMetricAtomic s_shadowBatches;
PhysicsMetricAtomic s_shadowPrefixes;
PhysicsMetricAtomic s_shadowRanges;
PhysicsMetricAtomic s_shadowSubmittedJobs;
PhysicsMetricAtomic s_shadowCompletedJobs;
PhysicsMetricAtomic s_shadowMatches;
PhysicsMetricAtomic s_shadowMismatches;
PhysicsMetricAtomic s_ownerFallbacks;
PhysicsMetricAtomic s_ineligibleSlices;
PhysicsMetricAtomic s_unexpectedFallbacks;
PhysicsMetricAtomic s_staleRejections;
PhysicsMetricAtomic s_circuitBreakerTrips;

bool finiteFloat(float value)
{
	return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

bool finiteFloats(const float *values, unsigned count)
{
	if (values == 0)
		return false;
	for (unsigned index = 0; index != count; ++index)
	{
		if (!finiteFloat(values[index]))
			return false;
	}
	return true;
}

bool sameFloat(float left, float right)
{
	return memcmp(&left, &right, sizeof(float)) == 0;
}

bool ownerIndexLess(const PhysicsIntegrationOwnerIndexEntry &left,
	const PhysicsIntegrationOwnerIndexEntry &right)
{
	return left.objectID < right.objectID ||
		(left.objectID == right.objectID && left.batchIndex < right.batchIndex);
}

void swapOwnerIndex(PhysicsIntegrationOwnerIndexEntry &left,
	PhysicsIntegrationOwnerIndexEntry &right)
{
	const PhysicsIntegrationOwnerIndexEntry temporary = left;
	left = right;
	right = temporary;
}

void siftOwnerIndex(PhysicsIntegrationOwnerIndexEntry *entries,
	unsigned root, unsigned end)
{
	for (;;)
	{
		const unsigned child = root * 2 + 1;
		if (child >= end)
			return;
		unsigned selected = child;
		if (child + 1 < end && ownerIndexLess(entries[child], entries[child + 1]))
			selected = child + 1;
		if (!ownerIndexLess(entries[root], entries[selected]))
			return;
		swapOwnerIndex(entries[root], entries[selected]);
		root = selected;
	}
}

void rotateX(float *matrix, float theta)
{
	float temporary1;
	float temporary2;
	const float sine = sinf(theta);
	const float cosine = cosf(theta);
	for (unsigned row = 0; row != 3; ++row)
	{
		const unsigned base = row * 4;
		temporary1 = matrix[base + 1];
		temporary2 = matrix[base + 2];
		matrix[base + 1] = (float)(cosine * temporary1 + sine * temporary2);
		matrix[base + 2] = (float)(-sine * temporary1 + cosine * temporary2);
	}
}

void rotateY(float *matrix, float theta)
{
	float temporary1;
	float temporary2;
	const float sine = sinf(theta);
	const float cosine = cosf(theta);
	for (unsigned row = 0; row != 3; ++row)
	{
		const unsigned base = row * 4;
		temporary1 = matrix[base];
		temporary2 = matrix[base + 2];
		matrix[base] = (float)(cosine * temporary1 - sine * temporary2);
		matrix[base + 2] = (float)(sine * temporary1 + cosine * temporary2);
	}
}

void rotateZ(float *matrix, float theta)
{
	float temporary1;
	float temporary2;
	const float cosine = cosf(theta);
	const float sine = sinf(theta);
	for (unsigned row = 0; row != 3; ++row)
	{
		const unsigned base = row * 4;
		temporary1 = matrix[base];
		temporary2 = matrix[base + 1];
		matrix[base] = (float)(cosine * temporary1 + sine * temporary2);
		matrix[base + 1] = (float)(-sine * temporary1 + cosine * temporary2);
	}
}

bool validSnapshot(const PhysicsIntegrationSnapshot &snapshot)
{
	const unsigned supportedFlags =
		PHYSICS_INTEGRATION_APPLY_FRICTION_2D_WHEN_AIRBORNE |
		PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN |
		PHYSICS_INTEGRATION_MOTIVE | PHYSICS_INTEGRATION_BRAKING |
		PHYSICS_INTEGRATION_PROJECTILE |
		PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW |
		PHYSICS_INTEGRATION_AIRBORNE_AT_START |
		PHYSICS_INTEGRATION_UPDATE_EVER_RUN |
		PHYSICS_INTEGRATION_WAS_AIRBORNE_LAST_FRAME;
	if (snapshot.objectID == 0 || snapshot.mass == 0.0f ||
		(snapshot.flags & ~supportedFlags) != 0 ||
		!finiteFloats(snapshot.matrix, PHYSICS_INTEGRATION_MATRIX_FLOATS) ||
		!finiteFloats(snapshot.position, PHYSICS_INTEGRATION_VECTOR_FLOATS) ||
		!finiteFloats(snapshot.acceleration, PHYSICS_INTEGRATION_VECTOR_FLOATS) ||
		!finiteFloats(snapshot.velocity, PHYSICS_INTEGRATION_VECTOR_FLOATS))
		return false;
	const float scalarValues[] = {
		snapshot.yawRate, snapshot.rollRate, snapshot.pitchRate,
		snapshot.gravity, snapshot.mass, snapshot.forwardFriction,
		snapshot.lateralFriction, snapshot.aerodynamicFriction,
		snapshot.pitchRollYawFactor, snapshot.centerOfMassOffset,
		snapshot.directionX, snapshot.directionY
	};
	return finiteFloats(scalarValues,
		static_cast<unsigned>(sizeof(scalarValues) / sizeof(scalarValues[0])));
}

bool validOutputValues(const PhysicsIntegrationOutput &output)
{
	const float scalarValues[] = {
		output.yawRate, output.rollRate, output.pitchRate
	};
	return finiteFloats(output.matrix, PHYSICS_INTEGRATION_MATRIX_FLOATS) &&
		finiteFloats(output.acceleration, PHYSICS_INTEGRATION_VECTOR_FLOATS) &&
		finiteFloats(output.velocity, PHYSICS_INTEGRATION_VECTOR_FLOATS) &&
		finiteFloats(scalarValues,
			static_cast<unsigned>(sizeof(scalarValues) / sizeof(scalarValues[0])));
}

struct PhysicsIntegrationExecutionRecord
{
	PhysicsIntegrationExecutionRecord()
		: completed(false), physicalWorker(false), ownerHelped(false),
		  physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX) {}
	bool completed;
	bool physicalWorker;
	bool ownerHelped;
	unsigned physicalWorkerIndex;
};

class PhysicsPhysicalExecutionScope
{
public:
	PhysicsPhysicalExecutionScope(bool physicalWorker,
		PhysicsJobAtomicUnsigned *active, PhysicsJobAtomicUnsigned *peak)
		: m_active(physicalWorker ? active : 0)
	{
		if (m_active != 0)
		{
			const unsigned current = incrementJobCounter(*m_active);
			maximizeJobCounter(*peak, current);
		}
	}
	~PhysicsPhysicalExecutionScope()
	{
		if (m_active != 0)
			decrementJobCounter(*m_active);
	}
private:
	PhysicsJobAtomicUnsigned *m_active;
};

class PhysicsIntegrationJob : public Job
{
public:
	PhysicsIntegrationJob(const PhysicsIntegrationSnapshot *snapshots,
		PhysicsIntegrationOutput *scratch, unsigned rangeIndex,
		unsigned begin, unsigned end, const JobFloatingPointState &floatingPointState,
		PhysicsIntegrationTestFault testFault, unsigned testOrdinal,
		PhysicsIntegrationExecutionRecord *execution,
		PhysicsJobAtomicUnsigned *activePhysicalWorkers,
		PhysicsJobAtomicUnsigned *peakPhysicalWorkers)
		: m_snapshots(snapshots), m_scratch(scratch),
		  m_rangeIndex(rangeIndex), m_begin(begin), m_end(end),
		  m_floatingPointState(floatingPointState), m_testFault(testFault),
		  m_testOrdinal(testOrdinal), m_execution(execution),
		  m_activePhysicalWorkers(activePhysicalWorkers),
		  m_peakPhysicalWorkers(peakPhysicalWorkers)
	{
	}

	virtual void execute(JobContext &context)
	{
		JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_execution->physicalWorker = context.isPhysicalWorkerExecution();
		m_execution->ownerHelped = !m_execution->physicalWorker;
		if (m_execution->physicalWorker)
			m_execution->physicalWorkerIndex = context.physicalWorkerIndex();
		PhysicsPhysicalExecutionScope physicalScope(m_execution->physicalWorker,
			m_activePhysicalWorkers, m_peakPhysicalWorkers);
		if (m_testFault == PHYSICS_INTEGRATION_TEST_WORKER_FAILURE &&
			m_rangeIndex == m_testOrdinal)
		{
			context.fail();
			return;
		}
		for (unsigned index = m_begin; index != m_end; ++index)
		{
			if ((index - m_begin) % 64 == 0 && context.isCancellationRequested())
				return;
			if (!ComputePhysicsIntegrationPrefix(m_snapshots[index], m_scratch[index]))
			{
				context.fail();
				return;
			}
			if (m_testFault == PHYSICS_INTEGRATION_TEST_NONFINITE_OUTPUT &&
				m_rangeIndex == m_testOrdinal && index == m_begin)
				m_scratch[index].velocity[0] = FLT_MAX * FLT_MAX;
		}
		if (context.isCancellationRequested())
			return;
		m_execution->completed = true;
	}

private:
	const PhysicsIntegrationSnapshot *m_snapshots;
	PhysicsIntegrationOutput *m_scratch;
	unsigned m_rangeIndex;
	unsigned m_begin;
	unsigned m_end;
	JobFloatingPointState m_floatingPointState;
	PhysicsIntegrationTestFault m_testFault;
	unsigned m_testOrdinal;
	PhysicsIntegrationExecutionRecord *m_execution;
	PhysicsJobAtomicUnsigned *m_activePhysicalWorkers;
	PhysicsJobAtomicUnsigned *m_peakPhysicalWorkers;
};

PhysicsIntegrationBatchResult fallback(JobSystem *jobs,
	PhysicsIntegrationMetrics &metrics,
	PhysicsIntegrationBatchResult result = PHYSICS_INTEGRATION_SERIAL_FALLBACK)
{
	++metrics.serialFallbacks;
	if (jobs != 0)
		jobs->recordSerialFallback();
	return result;
}
}

PhysicsIntegrationOptions::PhysicsIntegrationOptions()
	: minimumGrain(PHYSICS_INTEGRATION_DEFAULT_MINIMUM_GRAIN),
	  testFault(PHYSICS_INTEGRATION_TEST_NO_FAULT), testOrdinal(0)
{
}

PhysicsIntegrationMetrics::PhysicsIntegrationMetrics()
	: snapshotCount(0), rangeCount(0), effectiveMinimumGrain(0),
	  submittedJobs(0), completedJobs(0), physicalWorkerJobs(0),
	  ownerHelpedJobs(0), physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  peakConcurrentPhysicalWorkers(0), serialFallbacks(0), allocatedBytes(0),
	  captureNanoseconds(0), prepareNanoseconds(0), waitNanoseconds(0),
	  commitNanoseconds(0), storageBytes(0), storageCapacityBytes(0),
	  storageAllocations(0)
{
}

PhysicsIntegrationRuntimeMetrics::PhysicsIntegrationRuntimeMetrics()
	: resetEpoch(0), acceptedBatches(0), acceptedPrefixes(0), acceptedRanges(0),
	  acceptedSubmittedJobs(0), acceptedCompletedJobs(0),
	  acceptedPhysicalWorkerJobs(0), acceptedOwnerHelpedJobs(0),
	  acceptedPhysicalWorkerMask(0), maximumAcceptedDistinctPhysicalWorkers(0),
	  maximumAcceptedPeakConcurrentPhysicalWorkers(0),
	  acceptedAllocatedBytes(0),
	  acceptedCaptureNanoseconds(0), acceptedPrepareNanoseconds(0),
	  acceptedWaitNanoseconds(0), acceptedCommitNanoseconds(0),
	  acceptedStorageBytes(0), acceptedStorageCapacityBytes(0),
	  acceptedStorageAllocations(0), shadowBatches(0), shadowPrefixes(0),
	  shadowRanges(0), shadowSubmittedJobs(0), shadowCompletedJobs(0),
	  shadowMatches(0), shadowMismatches(0), ownerFallbacks(0),
	  ineligibleSlices(0), unexpectedFallbacks(0), staleRejections(0),
	  circuitBreakerTrips(0)
{
}

PhysicsIntegrationMetricCounter PhysicsIntegrationClockNowNanoseconds()
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	struct _timeb value;
	_ftime(&value);
	return static_cast<PhysicsIntegrationMetricCounter>(value.time) * 1000000000ui64 +
		static_cast<PhysicsIntegrationMetricCounter>(value.millitm) * 1000000ui64;
#else
	return static_cast<PhysicsIntegrationMetricCounter>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

bool ComputePhysicsIntegrationPrefix(const PhysicsIntegrationSnapshot &snapshot,
	PhysicsIntegrationOutput &output)
{
	if (!validSnapshot(snapshot))
		return false;

	PhysicsIntegrationOutput prepared;
	prepared.frame = snapshot.frame;
	prepared.worldEpoch = snapshot.worldEpoch;
	prepared.objectID = snapshot.objectID;
	prepared.motionGeneration = snapshot.motionGeneration;
	prepared.physicsGeneration = snapshot.physicsGeneration;
	prepared.wakePriority = snapshot.wakePriority;
	prepared.heapOrdinal = snapshot.heapOrdinal;
	prepared.flags = snapshot.flags;
	memcpy(prepared.matrix, snapshot.matrix, sizeof(prepared.matrix));
	memcpy(prepared.acceleration, snapshot.acceleration,
		sizeof(prepared.acceleration));
	memcpy(prepared.velocity, snapshot.velocity, sizeof(prepared.velocity));
	prepared.yawRate = snapshot.yawRate;
	prepared.rollRate = snapshot.rollRate;
	prepared.pitchRate = snapshot.pitchRate;

	// Preserve the scalar PhysicsBehavior operation order exactly.
	prepared.acceleration[2] += snapshot.gravity;
	const bool groundFriction =
		(snapshot.flags & PHYSICS_INTEGRATION_APPLY_FRICTION_2D_WHEN_AIRBORNE) != 0 ||
		(snapshot.flags & PHYSICS_INTEGRATION_SIGNIFICANTLY_ABOVE_TERRAIN) == 0;
	if (groundFriction)
	{
		const float damping = 1.0f - 0.15f;
		prepared.pitchRate *= damping;
		prepared.rollRate *= damping;
		prepared.yawRate *= damping;
		if (snapshot.velocity[0] || snapshot.velocity[1])
		{
			const float negativeDirectionY = -snapshot.directionY;
			const float lateralDot = snapshot.velocity[0] * negativeDirectionY +
				snapshot.velocity[1] * snapshot.directionX;
			const float lateralVelocityX = lateralDot * negativeDirectionY;
			const float lateralVelocityY = lateralDot * snapshot.directionX;
			const float lateralForce = snapshot.mass * snapshot.lateralFriction;
			float forceX = -(lateralForce * lateralVelocityX);
			float forceY = -(lateralForce * lateralVelocityY);
			const bool motive =
				(snapshot.flags & PHYSICS_INTEGRATION_MOTIVE) != 0;
			if (!motive)
			{
				const float forwardDot = snapshot.velocity[0] * snapshot.directionX +
					snapshot.velocity[1] * snapshot.directionY;
				const float forwardVelocityX = forwardDot * snapshot.directionX;
				const float forwardVelocityY = forwardDot * snapshot.directionY;
				const float forwardForce = snapshot.mass * snapshot.forwardFriction;
				forceX += -(forwardForce * forwardVelocityX);
				forceY += -(forwardForce * forwardVelocityY);
			}

			float modifiedForceX = forceX;
			float modifiedForceY = forceY;
			if (motive)
			{
				const float projectedLateralDot = forceX * negativeDirectionY +
					forceY * snapshot.directionX;
				modifiedForceX = projectedLateralDot * negativeDirectionY;
				modifiedForceY = projectedLateralDot * snapshot.directionX;
			}
			const float inverseMass = 1.0f / snapshot.mass;
			prepared.acceleration[0] += modifiedForceX * inverseMass;
			prepared.acceleration[1] += modifiedForceY * inverseMass;
			prepared.acceleration[2] += 0.0f * inverseMass;
		}
	}
	else
	{
		const float aerodynamics = -snapshot.aerodynamicFriction;
		prepared.acceleration[0] += snapshot.velocity[0] * aerodynamics;
		prepared.acceleration[1] += snapshot.velocity[1] * aerodynamics;
		prepared.acceleration[2] += snapshot.velocity[2] * aerodynamics;
		const float damping = 1.0f + aerodynamics;
		prepared.pitchRate *= damping;
		prepared.rollRate *= damping;
		prepared.yawRate *= damping;
	}
	if (prepared.pitchRate != 0.0f || prepared.rollRate != 0.0f ||
		prepared.yawRate != 0.0f)
		prepared.flags |= PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
	else
		prepared.flags &= ~PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;

	prepared.velocity[0] += prepared.acceleration[0];
	prepared.velocity[1] += prepared.acceleration[1];
	prepared.velocity[2] += prepared.acceleration[2];
	const float threshold = 0.001f;
	if (fabsf(prepared.velocity[0]) < threshold) prepared.velocity[0] = 0.0f;
	if (fabsf(prepared.velocity[1]) < threshold) prepared.velocity[1] = 0.0f;
	if (fabsf(prepared.velocity[2]) < threshold) prepared.velocity[2] = 0.0f;

	if ((snapshot.flags & PHYSICS_INTEGRATION_BRAKING) != 0)
	{
		if ((snapshot.flags & PHYSICS_INTEGRATION_PROJECTILE) == 0)
			prepared.matrix[11] += prepared.velocity[2];
	}
	else
	{
		prepared.matrix[3] += prepared.velocity[0];
		prepared.matrix[7] += prepared.velocity[1];
		prepared.matrix[11] += prepared.velocity[2];
	}

	if ((prepared.flags & PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW) != 0)
	{
		const float yawRateToUse = prepared.yawRate * snapshot.pitchRollYawFactor;
		float pitchRateToUse = prepared.pitchRate * snapshot.pitchRollYawFactor;
		const float rollRateToUse = prepared.rollRate * snapshot.pitchRollYawFactor;
		if (snapshot.centerOfMassOffset != 0.0f)
		{
			const float xVectorX = prepared.matrix[0];
			const float xVectorY = prepared.matrix[4];
			const float xVectorZ = prepared.matrix[8];
			const float xy = sqrtf(xVectorX * xVectorX + xVectorY * xVectorY);
			const float pitchAngle = (float)atan2(xVectorZ, xy);
			const float halfPi = 3.14159265359f / 2;
			const float remainingAngle = snapshot.centerOfMassOffset > 0.0f ?
				(halfPi - pitchAngle) : (-halfPi + pitchAngle);
			const float sine = sinf(remainingAngle);
			pitchRateToUse *= sine;
		}
		rotateX(prepared.matrix, rollRateToUse);
		rotateY(prepared.matrix, pitchRateToUse);
		rotateZ(prepared.matrix, yawRateToUse);
	}

	if (!validOutputValues(prepared))
		return false;
	output = prepared;
	return true;
}

bool ValidatePhysicsIntegrationSnapshot(
	const PhysicsIntegrationSnapshot &snapshot)
{
	return validSnapshot(snapshot);
}

bool BuildPhysicsIntegrationOwnerIndex(
	const PhysicsIntegrationSnapshot *snapshots, unsigned snapshotCount,
	PhysicsIntegrationOwnerIndexEntry *entries, unsigned entryCapacity)
{
	if (snapshots == 0 || entries == 0 || snapshotCount == 0 ||
		snapshotCount > PHYSICS_INTEGRATION_MAXIMUM_SNAPSHOTS ||
		entryCapacity < snapshotCount)
		return false;
	for (unsigned ownerFillIndex = 0; ownerFillIndex != snapshotCount;
		++ownerFillIndex)
	{
		if (snapshots[ownerFillIndex].objectID == 0)
			return false;
		entries[ownerFillIndex].objectID = snapshots[ownerFillIndex].objectID;
		entries[ownerFillIndex].batchIndex = ownerFillIndex;
	}
	if (snapshotCount > 1)
	{
		unsigned ownerHeapStart = (snapshotCount - 2) / 2 + 1;
		while (ownerHeapStart != 0)
		{
			--ownerHeapStart;
			siftOwnerIndex(entries, ownerHeapStart, snapshotCount);
		}
		unsigned ownerHeapEnd = snapshotCount;
		while (ownerHeapEnd > 1)
		{
			--ownerHeapEnd;
			swapOwnerIndex(entries[0], entries[ownerHeapEnd]);
			siftOwnerIndex(entries, 0, ownerHeapEnd);
		}
	}
	for (unsigned ownerDuplicateIndex = 1;
		ownerDuplicateIndex != snapshotCount; ++ownerDuplicateIndex)
	{
		if (entries[ownerDuplicateIndex - 1].objectID ==
			entries[ownerDuplicateIndex].objectID)
			return false;
	}
	return true;
}

bool FindPhysicsIntegrationOwnerIndex(
	const PhysicsIntegrationOwnerIndexEntry *entries, unsigned entryCount,
	unsigned objectID, unsigned *batchIndex)
{
	if (entries == 0 || entryCount == 0 || objectID == 0 || batchIndex == 0)
		return false;
	unsigned ownerLow = 0;
	unsigned ownerHigh = entryCount;
	while (ownerLow < ownerHigh)
	{
		const unsigned ownerMiddle = ownerLow + (ownerHigh - ownerLow) / 2;
		if (entries[ownerMiddle].objectID < objectID)
			ownerLow = ownerMiddle + 1;
		else
			ownerHigh = ownerMiddle;
	}
	if (ownerLow == entryCount || entries[ownerLow].objectID != objectID ||
		entries[ownerLow].batchIndex >= entryCount)
		return false;
	*batchIndex = entries[ownerLow].batchIndex;
	return true;
}

PhysicsIntegrationBatchResult PreflightPhysicsIntegrationPrefixes()
{
	JobSystem &jobs = JobSystem::instance();
	PhysicsIntegrationMetrics metrics;
	if (!jobs.isRunning() || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME))
		return fallback(&jobs, metrics);
	if (jobs.workerCount() <= 1)
		return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;
	return PHYSICS_INTEGRATION_PARALLEL;
}

PhysicsIntegrationBatchResult PreparePhysicsIntegrationPrefixes(
	const PhysicsIntegrationSnapshot *snapshots, unsigned snapshotCount,
	PhysicsIntegrationOutput *output, unsigned outputCapacity,
	PhysicsIntegrationOutput *scratch, unsigned scratchCapacity,
	const PhysicsIntegrationOptions &options, PhysicsIntegrationMetrics *metrics)
{
	const PhysicsIntegrationMetricCounter prepareStart =
		PhysicsIntegrationClockNowNanoseconds();
	PhysicsIntegrationMetrics localMetrics;
	if (metrics == 0)
		metrics = &localMetrics;
	*metrics = PhysicsIntegrationMetrics();
	metrics->snapshotCount = snapshotCount;
	metrics->effectiveMinimumGrain = options.minimumGrain != 0 ?
		options.minimumGrain : PHYSICS_INTEGRATION_DEFAULT_MINIMUM_GRAIN;

	if (snapshotCount == 0 || snapshotCount > PHYSICS_INTEGRATION_MAXIMUM_SNAPSHOTS ||
		snapshots == 0 || output == 0 || scratch == 0 ||
		outputCapacity < snapshotCount || scratchCapacity < snapshotCount)
		return PHYSICS_INTEGRATION_INVALID_INPUT;
	for (unsigned snapshotIndex = 0; snapshotIndex != snapshotCount; ++snapshotIndex)
	{
		if (!validSnapshot(snapshots[snapshotIndex]))
			return PHYSICS_INTEGRATION_INVALID_INPUT;
	}

	JobSystem &jobs = JobSystem::instance();
	if (!jobs.isRunning() || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME))
		return fallback(&jobs, *metrics);
	if (jobs.workerCount() <= 1)
		return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;
	const unsigned rangeCount = JobSystem::chooseRangeCount(snapshotCount,
		metrics->effectiveMinimumGrain, jobs.workerCount());
	metrics->rangeCount = rangeCount;
	if (rangeCount <= 1)
		return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;

	if (rangeCount > static_cast<unsigned>(~static_cast<unsigned>(0)) /
		(sizeof(JobSubmission) + sizeof(JobHandle) +
		 sizeof(PhysicsIntegrationJob *) + sizeof(PhysicsIntegrationJob) +
		 sizeof(PhysicsIntegrationExecutionRecord)))
		return fallback(&jobs, *metrics);
	metrics->allocatedBytes = rangeCount * static_cast<unsigned>(
		sizeof(JobSubmission) + sizeof(JobHandle) +
		sizeof(PhysicsIntegrationJob *) + sizeof(PhysicsIntegrationJob) +
		sizeof(PhysicsIntegrationExecutionRecord));
	if (options.testFault == PHYSICS_INTEGRATION_TEST_ALLOCATION_FAILURE)
		return fallback(&jobs, *metrics);
	JobSubmission *submissions = new (std::nothrow) JobSubmission[rangeCount];
	JobHandle *handles = new (std::nothrow) JobHandle[rangeCount];
	PhysicsIntegrationJob **jobPointers = new (std::nothrow)
		PhysicsIntegrationJob *[rangeCount];
	PhysicsIntegrationExecutionRecord *executions = new (std::nothrow)
		PhysicsIntegrationExecutionRecord[rangeCount];
	if (submissions == 0 || handles == 0 || jobPointers == 0 || executions == 0)
	{
		delete[] submissions;
		delete[] handles;
		delete[] jobPointers;
		delete[] executions;
		return fallback(&jobs, *metrics);
	}
	for (unsigned pointerIndex = 0; pointerIndex != rangeCount; ++pointerIndex)
		jobPointers[pointerIndex] = 0;

	JobGroup group;
	if (options.testFault != PHYSICS_INTEGRATION_TEST_GROUP_FAILURE)
		group = jobs.createGroup();
	if (!group.isValid())
	{
		delete[] submissions;
		delete[] handles;
		delete[] jobPointers;
		delete[] executions;
		return fallback(&jobs, *metrics);
	}

	const JobFloatingPointState floatingPointState;
	PhysicsJobAtomicUnsigned activePhysicalWorkers(0);
	PhysicsJobAtomicUnsigned peakPhysicalWorkers(0);
	bool jobsReady = true;
	for (unsigned rangeIndex = 0; rangeIndex != rangeCount; ++rangeIndex)
	{
		JobRange range;
		if (!JobSystem::rangeForIndex(snapshotCount, rangeCount, rangeIndex, range))
		{
			jobsReady = false;
			break;
		}
		if (options.testFault == PHYSICS_INTEGRATION_TEST_JOB_ALLOCATION_FAILURE &&
			rangeIndex == options.testOrdinal)
		{
			jobsReady = false;
			break;
		}
		jobPointers[rangeIndex] = new (std::nothrow) PhysicsIntegrationJob(
			snapshots, scratch, rangeIndex, range.begin, range.end,
			floatingPointState, options.testFault, options.testOrdinal,
			executions + rangeIndex, &activePhysicalWorkers,
			&peakPhysicalWorkers);
		if (jobPointers[rangeIndex] == 0)
		{
			jobsReady = false;
			break;
		}
		submissions[rangeIndex].job = jobPointers[rangeIndex];
		submissions[rangeIndex].priority = JOB_PRIORITY_FRAME_CRITICAL;
	}

	bool admitted = false;
	if (jobsReady && !(options.testFault == PHYSICS_INTEGRATION_TEST_ADMISSION_FAILURE &&
		options.testOrdinal < rangeCount))
		admitted = jobs.trySubmitBatch(submissions, rangeCount, group, handles);
	if (!admitted)
	{
		for (unsigned cleanupIndex = 0; cleanupIndex != rangeCount; ++cleanupIndex)
			delete jobPointers[cleanupIndex];
		delete[] submissions;
		delete[] handles;
		delete[] jobPointers;
		delete[] executions;
		return fallback(&jobs, *metrics);
	}
	metrics->submittedJobs = rangeCount;
	if (options.testFault == PHYSICS_INTEGRATION_TEST_CANCEL_AFTER_ADMISSION)
		jobs.cancel(group);
	metrics->prepareNanoseconds = PhysicsIntegrationClockNowNanoseconds() -
		prepareStart;
	const PhysicsIntegrationMetricCounter waitStart =
		PhysicsIntegrationClockNowNanoseconds();
	const unsigned physicalCompletionTimeoutMilliseconds = 8;
	const bool forcePhysicalTimeout = options.testFault ==
		PHYSICS_INTEGRATION_TEST_PHYSICAL_WAIT_TIMEOUT;
	const bool physicalFenceCompleted = !forcePhysicalTimeout &&
		jobs.waitWithoutOwnerHelp(group, physicalCompletionTimeoutMilliseconds);
	if (!physicalFenceCompleted)
	{
		jobs.cancel(group);
		jobs.wait(group);
	}
	else
	{
		// The passive fence proved that every job completed without owner help.
		jobs.wait(group);
	}
	metrics->waitNanoseconds = PhysicsIntegrationClockNowNanoseconds() - waitStart;
	const PhysicsIntegrationMetricCounter finalizeStart =
		PhysicsIntegrationClockNowNanoseconds();
	for (unsigned completionIndex = 0; completionIndex != rangeCount; ++completionIndex)
	{
		if (handles[completionIndex].succeeded() && executions[completionIndex].completed)
		{
			++metrics->completedJobs;
			if (executions[completionIndex].physicalWorker)
			{
				++metrics->physicalWorkerJobs;
				const unsigned workerIndex =
					executions[completionIndex].physicalWorkerIndex;
				if (workerIndex < sizeof(PhysicsIntegrationMetricCounter) * 8)
					metrics->physicalWorkerMask |=
						static_cast<PhysicsIntegrationMetricCounter>(1) << workerIndex;
				bool firstWorker = true;
				for (unsigned previous = 0; previous != completionIndex; ++previous)
				{
					if (handles[previous].succeeded() && executions[previous].completed &&
						executions[previous].physicalWorker &&
						executions[previous].physicalWorkerIndex == workerIndex)
					{
						firstWorker = false;
						break;
					}
				}
				if (firstWorker)
					++metrics->distinctPhysicalWorkers;
			}
			else if (executions[completionIndex].ownerHelped)
				++metrics->ownerHelpedJobs;
		}
	}
	metrics->peakConcurrentPhysicalWorkers = loadJobCounter(peakPhysicalWorkers);

	PhysicsIntegrationBatchResult result = PHYSICS_INTEGRATION_PARALLEL;
	if (!physicalFenceCompleted || group.wasCancelled())
		result = fallback(&jobs, *metrics, PHYSICS_INTEGRATION_CANCELLED);
	else if (group.failed() || metrics->completedJobs != metrics->submittedJobs)
		result = fallback(&jobs, *metrics);
	else if (metrics->physicalWorkerJobs != metrics->completedJobs ||
		metrics->ownerHelpedJobs != 0)
		result = fallback(&jobs, *metrics);
	if (result == PHYSICS_INTEGRATION_PARALLEL)
	{
		for (unsigned validationIndex = 0; validationIndex != snapshotCount; ++validationIndex)
		{
			if (!ValidatePhysicsIntegrationOutput(snapshots[validationIndex], scratch[validationIndex]))
			{
				result = fallback(&jobs, *metrics);
				break;
			}
		}
	}
	if (result == PHYSICS_INTEGRATION_PARALLEL)
		memcpy(output, scratch, snapshotCount * sizeof(PhysicsIntegrationOutput));

	delete[] submissions;
	delete[] handles;
	delete[] jobPointers;
	delete[] executions;
	// prepareNanoseconds is owner CPU overhead on both sides of the fence:
	// validation/allocation/submission plus validation/publication/reclamation.
	metrics->prepareNanoseconds +=
		PhysicsIntegrationClockNowNanoseconds() - finalizeStart;
	return result;
}

bool PhysicsIntegrationSnapshotsEqual(const PhysicsIntegrationSnapshot &left,
	const PhysicsIntegrationSnapshot &right, unsigned *firstField)
{
	unsigned field = 0;
#define PHYSICS_COMPARE_VALUE(member) \
	do { if (left.member != right.member) { if (firstField != 0) *firstField = field; return false; } ++field; } while (0)
#define PHYSICS_COMPARE_FLOAT(member) \
	do { if (!sameFloat(left.member, right.member)) { if (firstField != 0) *firstField = field; return false; } ++field; } while (0)
	PHYSICS_COMPARE_VALUE(frame);
	PHYSICS_COMPARE_VALUE(worldEpoch);
	PHYSICS_COMPARE_VALUE(objectID);
	PHYSICS_COMPARE_VALUE(motionGeneration);
	PHYSICS_COMPARE_VALUE(physicsGeneration);
	PHYSICS_COMPARE_VALUE(wakePriority);
	PHYSICS_COMPARE_VALUE(heapOrdinal);
	PHYSICS_COMPARE_VALUE(flags);
	for (unsigned matrixIndex = 0; matrixIndex != PHYSICS_INTEGRATION_MATRIX_FLOATS; ++matrixIndex, ++field)
	{
		if (!sameFloat(left.matrix[matrixIndex], right.matrix[matrixIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	for (unsigned positionIndex = 0; positionIndex != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++positionIndex, ++field)
	{
		if (!sameFloat(left.position[positionIndex], right.position[positionIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	for (unsigned accelerationIndex = 0; accelerationIndex != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++accelerationIndex, ++field)
	{
		if (!sameFloat(left.acceleration[accelerationIndex], right.acceleration[accelerationIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	for (unsigned velocityIndex = 0; velocityIndex != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++velocityIndex, ++field)
	{
		if (!sameFloat(left.velocity[velocityIndex], right.velocity[velocityIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	PHYSICS_COMPARE_FLOAT(yawRate);
	PHYSICS_COMPARE_FLOAT(rollRate);
	PHYSICS_COMPARE_FLOAT(pitchRate);
	PHYSICS_COMPARE_FLOAT(gravity);
	PHYSICS_COMPARE_FLOAT(mass);
	PHYSICS_COMPARE_FLOAT(forwardFriction);
	PHYSICS_COMPARE_FLOAT(lateralFriction);
	PHYSICS_COMPARE_FLOAT(aerodynamicFriction);
	PHYSICS_COMPARE_FLOAT(pitchRollYawFactor);
	PHYSICS_COMPARE_FLOAT(centerOfMassOffset);
	PHYSICS_COMPARE_FLOAT(directionX);
	PHYSICS_COMPARE_FLOAT(directionY);
#undef PHYSICS_COMPARE_FLOAT
#undef PHYSICS_COMPARE_VALUE
	if (firstField != 0) *firstField = field;
	return true;
}

bool PhysicsIntegrationOutputsEqual(const PhysicsIntegrationOutput &left,
	const PhysicsIntegrationOutput &right, unsigned *firstField)
{
	unsigned field = 0;
#define PHYSICS_COMPARE_OUTPUT_VALUE(member) \
	do { if (left.member != right.member) { if (firstField != 0) *firstField = field; return false; } ++field; } while (0)
#define PHYSICS_COMPARE_OUTPUT_FLOAT(member) \
	do { if (!sameFloat(left.member, right.member)) { if (firstField != 0) *firstField = field; return false; } ++field; } while (0)
	PHYSICS_COMPARE_OUTPUT_VALUE(frame);
	PHYSICS_COMPARE_OUTPUT_VALUE(worldEpoch);
	PHYSICS_COMPARE_OUTPUT_VALUE(objectID);
	PHYSICS_COMPARE_OUTPUT_VALUE(motionGeneration);
	PHYSICS_COMPARE_OUTPUT_VALUE(physicsGeneration);
	PHYSICS_COMPARE_OUTPUT_VALUE(wakePriority);
	PHYSICS_COMPARE_OUTPUT_VALUE(heapOrdinal);
	PHYSICS_COMPARE_OUTPUT_VALUE(flags);
	for (unsigned outputMatrixIndex = 0; outputMatrixIndex != PHYSICS_INTEGRATION_MATRIX_FLOATS; ++outputMatrixIndex, ++field)
	{
		if (!sameFloat(left.matrix[outputMatrixIndex], right.matrix[outputMatrixIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	for (unsigned outputAccelerationIndex = 0; outputAccelerationIndex != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++outputAccelerationIndex, ++field)
	{
		if (!sameFloat(left.acceleration[outputAccelerationIndex], right.acceleration[outputAccelerationIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	for (unsigned outputVelocityIndex = 0; outputVelocityIndex != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++outputVelocityIndex, ++field)
	{
		if (!sameFloat(left.velocity[outputVelocityIndex], right.velocity[outputVelocityIndex]))
		{
			if (firstField != 0) *firstField = field;
			return false;
		}
	}
	PHYSICS_COMPARE_OUTPUT_FLOAT(yawRate);
	PHYSICS_COMPARE_OUTPUT_FLOAT(rollRate);
	PHYSICS_COMPARE_OUTPUT_FLOAT(pitchRate);
#undef PHYSICS_COMPARE_OUTPUT_FLOAT
#undef PHYSICS_COMPARE_OUTPUT_VALUE
	if (firstField != 0) *firstField = field;
	return true;
}

bool ValidatePhysicsIntegrationOutput(const PhysicsIntegrationSnapshot &snapshot,
	const PhysicsIntegrationOutput &output)
{
	unsigned expectedFlags = snapshot.flags;
	if (output.pitchRate != 0.0f || output.rollRate != 0.0f ||
		output.yawRate != 0.0f)
		expectedFlags |= PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
	else
		expectedFlags &= ~PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW;
	return validSnapshot(snapshot) && validOutputValues(output) &&
		output.frame == snapshot.frame &&
		output.worldEpoch == snapshot.worldEpoch &&
		output.objectID == snapshot.objectID &&
		output.motionGeneration == snapshot.motionGeneration &&
		output.physicsGeneration == snapshot.physicsGeneration &&
		output.wakePriority == snapshot.wakePriority &&
		output.heapOrdinal == snapshot.heapOrdinal &&
		output.flags == expectedFlags;
}

bool ValidatePhysicsIntegrationCommit(
	const PhysicsIntegrationSnapshot &captured,
	const PhysicsIntegrationSnapshot &current,
	const PhysicsIntegrationOutput &output,
	bool actualHeapRoot, bool objectResolved, bool exactPhysics)
{
	return actualHeapRoot && objectResolved && exactPhysics &&
		PhysicsIntegrationSnapshotsEqual(captured, current) &&
		ValidatePhysicsIntegrationOutput(captured, output);
}

void ResetPhysicsIntegrationRuntimeMetrics()
{
	addMetric(s_resetEpoch, 1);
	resetMetric(s_acceptedBatches);
	resetMetric(s_acceptedPrefixes);
	resetMetric(s_acceptedRanges);
	resetMetric(s_acceptedSubmittedJobs);
	resetMetric(s_acceptedCompletedJobs);
	resetMetric(s_acceptedPhysicalWorkerJobs);
	resetMetric(s_acceptedOwnerHelpedJobs);
	resetMetric(s_acceptedPhysicalWorkerMask);
	resetMetric(s_maximumAcceptedDistinctPhysicalWorkers);
	resetMetric(s_maximumAcceptedPeakConcurrentPhysicalWorkers);
	resetMetric(s_acceptedAllocatedBytes);
	resetMetric(s_acceptedCaptureNanoseconds);
	resetMetric(s_acceptedPrepareNanoseconds);
	resetMetric(s_acceptedWaitNanoseconds);
	resetMetric(s_acceptedCommitNanoseconds);
	resetMetric(s_acceptedStorageBytes);
	resetMetric(s_acceptedStorageCapacityBytes);
	resetMetric(s_acceptedStorageAllocations);
	resetMetric(s_shadowBatches);
	resetMetric(s_shadowPrefixes);
	resetMetric(s_shadowRanges);
	resetMetric(s_shadowSubmittedJobs);
	resetMetric(s_shadowCompletedJobs);
	resetMetric(s_shadowMatches);
	resetMetric(s_shadowMismatches);
	resetMetric(s_ownerFallbacks);
	resetMetric(s_ineligibleSlices);
	resetMetric(s_unexpectedFallbacks);
	resetMetric(s_staleRejections);
	resetMetric(s_circuitBreakerTrips);
}

PhysicsIntegrationRuntimeMetrics GetPhysicsIntegrationRuntimeMetrics()
{
	PhysicsIntegrationRuntimeMetrics metrics;
	metrics.resetEpoch = loadMetric(s_resetEpoch);
	metrics.acceptedBatches = loadMetric(s_acceptedBatches);
	metrics.acceptedPrefixes = loadMetric(s_acceptedPrefixes);
	metrics.acceptedRanges = loadMetric(s_acceptedRanges);
	metrics.acceptedSubmittedJobs = loadMetric(s_acceptedSubmittedJobs);
	metrics.acceptedCompletedJobs = loadMetric(s_acceptedCompletedJobs);
	metrics.acceptedPhysicalWorkerJobs = loadMetric(s_acceptedPhysicalWorkerJobs);
	metrics.acceptedOwnerHelpedJobs = loadMetric(s_acceptedOwnerHelpedJobs);
	metrics.acceptedPhysicalWorkerMask = loadMetric(s_acceptedPhysicalWorkerMask);
	metrics.maximumAcceptedDistinctPhysicalWorkers = static_cast<unsigned>(
		loadMetric(s_maximumAcceptedDistinctPhysicalWorkers));
	metrics.maximumAcceptedPeakConcurrentPhysicalWorkers = static_cast<unsigned>(
		loadMetric(s_maximumAcceptedPeakConcurrentPhysicalWorkers));
	metrics.acceptedAllocatedBytes = loadMetric(s_acceptedAllocatedBytes);
	metrics.acceptedCaptureNanoseconds = loadMetric(s_acceptedCaptureNanoseconds);
	metrics.acceptedPrepareNanoseconds = loadMetric(s_acceptedPrepareNanoseconds);
	metrics.acceptedWaitNanoseconds = loadMetric(s_acceptedWaitNanoseconds);
	metrics.acceptedCommitNanoseconds = loadMetric(s_acceptedCommitNanoseconds);
	metrics.acceptedStorageBytes = loadMetric(s_acceptedStorageBytes);
	metrics.acceptedStorageCapacityBytes = loadMetric(s_acceptedStorageCapacityBytes);
	metrics.acceptedStorageAllocations = loadMetric(s_acceptedStorageAllocations);
	metrics.shadowBatches = loadMetric(s_shadowBatches);
	metrics.shadowPrefixes = loadMetric(s_shadowPrefixes);
	metrics.shadowRanges = loadMetric(s_shadowRanges);
	metrics.shadowSubmittedJobs = loadMetric(s_shadowSubmittedJobs);
	metrics.shadowCompletedJobs = loadMetric(s_shadowCompletedJobs);
	metrics.shadowMatches = loadMetric(s_shadowMatches);
	metrics.shadowMismatches = loadMetric(s_shadowMismatches);
	metrics.ownerFallbacks = loadMetric(s_ownerFallbacks);
	metrics.ineligibleSlices = loadMetric(s_ineligibleSlices);
	metrics.unexpectedFallbacks = loadMetric(s_unexpectedFallbacks);
	metrics.staleRejections = loadMetric(s_staleRejections);
	metrics.circuitBreakerTrips = loadMetric(s_circuitBreakerTrips);
	return metrics;
}

void RecordPhysicsIntegrationAuthoritativeCommit(unsigned prefixCount)
{
	addMetric(s_acceptedBatches, 1);
	addMetric(s_acceptedPrefixes, prefixCount);
}

void RecordPhysicsIntegrationAuthoritativeSlice(unsigned prefixCount,
	const PhysicsIntegrationMetrics &sliceMetrics)
{
	if (prefixCount == 0 || sliceMetrics.submittedJobs < 2 ||
		sliceMetrics.completedJobs != sliceMetrics.submittedJobs ||
		sliceMetrics.physicalWorkerJobs != sliceMetrics.completedJobs ||
		sliceMetrics.ownerHelpedJobs != 0 ||
		sliceMetrics.distinctPhysicalWorkers <= 1 ||
		sliceMetrics.peakConcurrentPhysicalWorkers <= 1)
	{
		addMetric(s_ownerFallbacks, 1);
		return;
	}
	RecordPhysicsIntegrationAuthoritativeCommit(prefixCount);
	addMetric(s_acceptedRanges, sliceMetrics.rangeCount);
	addMetric(s_acceptedSubmittedJobs, sliceMetrics.submittedJobs);
	addMetric(s_acceptedCompletedJobs, sliceMetrics.completedJobs);
	addMetric(s_acceptedPhysicalWorkerJobs, sliceMetrics.physicalWorkerJobs);
	addMetric(s_acceptedOwnerHelpedJobs, sliceMetrics.ownerHelpedJobs);
	orMetric(s_acceptedPhysicalWorkerMask, sliceMetrics.physicalWorkerMask);
	maximizeMetric(s_maximumAcceptedDistinctPhysicalWorkers,
		sliceMetrics.distinctPhysicalWorkers);
	maximizeMetric(s_maximumAcceptedPeakConcurrentPhysicalWorkers,
		sliceMetrics.peakConcurrentPhysicalWorkers);
	addMetric(s_acceptedAllocatedBytes, sliceMetrics.allocatedBytes);
	addMetric(s_acceptedCaptureNanoseconds, sliceMetrics.captureNanoseconds);
	addMetric(s_acceptedPrepareNanoseconds, sliceMetrics.prepareNanoseconds);
	addMetric(s_acceptedWaitNanoseconds, sliceMetrics.waitNanoseconds);
	addMetric(s_acceptedCommitNanoseconds, sliceMetrics.commitNanoseconds);
	addMetric(s_acceptedStorageBytes, sliceMetrics.storageBytes);
	addMetric(s_acceptedStorageCapacityBytes, sliceMetrics.storageCapacityBytes);
	addMetric(s_acceptedStorageAllocations, sliceMetrics.storageAllocations);
}

void RecordPhysicsIntegrationShadow(bool matched, unsigned prefixCount,
	const PhysicsIntegrationMetrics &sliceMetrics)
{
	addMetric(s_shadowBatches, 1);
	addMetric(s_shadowPrefixes, prefixCount);
	addMetric(s_shadowRanges, sliceMetrics.rangeCount);
	addMetric(s_shadowSubmittedJobs, sliceMetrics.submittedJobs);
	addMetric(s_shadowCompletedJobs, sliceMetrics.completedJobs);
	addMetric(matched ? s_shadowMatches : s_shadowMismatches, 1);
}

void RecordPhysicsIntegrationOwnerFallback(bool stale)
{
	addMetric(s_ownerFallbacks, 1);
	if (stale)
		addMetric(s_staleRejections, 1);
}

void RecordPhysicsIntegrationIneligibleSlice()
{
	addMetric(s_ineligibleSlices, 1);
}

void RecordPhysicsIntegrationUnexpectedFallback()
{
	addMetric(s_unexpectedFallbacks, 1);
}

void RecordPhysicsIntegrationCircuitBreakerTrip()
{
	addMetric(s_circuitBreakerTrips, 1);
}
}
