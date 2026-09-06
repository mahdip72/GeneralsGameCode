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
#if defined(_WIN64)
#include <memory>
#endif
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
PhysicsMetricAtomic s_acceptedPhysicalWorkerMaskIncomplete;
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

#if defined(_WIN64)
/* Reference schema 1: batch sequence=1; snapshot scalars=2..9,
** matrix/position/acceleration/velocity sequences=10/12/14/16 with value
** tags=11/13/15/17, then rates/gravity/material/direction=18..29. Output
** uses batch sequence=1, the same identity scalars=2..9, matrix/acceleration/
** velocity sequences=10/12/14 with value tags=11/13/15, then rates=16..18.
*/
enum { PHYSICS_REFERENCE_FIELD_SCHEMA = 1 };

struct PhysicsReferenceInput
{
	const PhysicsIntegrationSnapshot *snapshots;
	unsigned count;
};

struct PhysicsReferenceOutput
{
	PhysicsIntegrationOutput *outputs;
	unsigned count;
	unsigned capacity;
};

bool writePhysicsReferenceSnapshot(
	performance::KernelPerformanceCanonicalWriter &writer,
	const PhysicsIntegrationSnapshot &snapshot)
{
	if (!writer.u32(2, snapshot.frame) ||
		!writer.u32(3, snapshot.worldEpoch) ||
		!writer.u32(4, snapshot.objectID) ||
		!writer.u32(5, snapshot.motionGeneration) ||
		!writer.u32(6, snapshot.physicsGeneration) ||
		!writer.u32(7, snapshot.wakePriority) ||
		!writer.u32(8, snapshot.heapOrdinal) ||
		!writer.u32(9, snapshot.flags) ||
		!writer.sequence(10, PHYSICS_INTEGRATION_MATRIX_FLOATS))
		return false;
	for (unsigned index = 0; index != PHYSICS_INTEGRATION_MATRIX_FLOATS;
		++index)
		if (!writer.f32(11, snapshot.matrix[index])) return false;
	if (!writer.sequence(12, PHYSICS_INTEGRATION_VECTOR_FLOATS)) return false;
	for (unsigned index = 0; index != PHYSICS_INTEGRATION_VECTOR_FLOATS;
		++index)
		if (!writer.f32(13, snapshot.position[index])) return false;
	if (!writer.sequence(14, PHYSICS_INTEGRATION_VECTOR_FLOATS)) return false;
	for (unsigned index = 0; index != PHYSICS_INTEGRATION_VECTOR_FLOATS;
		++index)
		if (!writer.f32(15, snapshot.acceleration[index])) return false;
	if (!writer.sequence(16, PHYSICS_INTEGRATION_VECTOR_FLOATS)) return false;
	for (unsigned index = 0; index != PHYSICS_INTEGRATION_VECTOR_FLOATS;
		++index)
		if (!writer.f32(17, snapshot.velocity[index])) return false;
	return writer.f32(18, snapshot.yawRate) &&
		writer.f32(19, snapshot.rollRate) &&
		writer.f32(20, snapshot.pitchRate) &&
		writer.f32(21, snapshot.gravity) &&
		writer.f32(22, snapshot.mass) &&
		writer.f32(23, snapshot.forwardFriction) &&
		writer.f32(24, snapshot.lateralFriction) &&
		writer.f32(25, snapshot.aerodynamicFriction) &&
		writer.f32(26, snapshot.pitchRollYawFactor) &&
		writer.f32(27, snapshot.centerOfMassOffset) &&
		writer.f32(28, snapshot.directionX) &&
		writer.f32(29, snapshot.directionY);
}

bool writePhysicsReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const PhysicsReferenceInput &input =
		*static_cast<const PhysicsReferenceInput *>(context);
	if (input.snapshots == 0 || input.count == 0 ||
		!writer.sequence(1, input.count))
		return false;
	for (unsigned index = 0; index != input.count; ++index)
		if (!writePhysicsReferenceSnapshot(writer, input.snapshots[index]))
			return false;
	return true;
}

bool writePhysicsReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const PhysicsReferenceOutput &output =
		*static_cast<const PhysicsReferenceOutput *>(context);
	if (output.outputs == 0 || output.count == 0 ||
		!writer.sequence(1, output.count))
		return false;
	for (unsigned index = 0; index != output.count; ++index)
	{
		const PhysicsIntegrationOutput &value = output.outputs[index];
		if (!writer.u32(2, value.frame) ||
			!writer.u32(3, value.worldEpoch) ||
			!writer.u32(4, value.objectID) ||
			!writer.u32(5, value.motionGeneration) ||
			!writer.u32(6, value.physicsGeneration) ||
			!writer.u32(7, value.wakePriority) ||
			!writer.u32(8, value.heapOrdinal) ||
			!writer.u32(9, value.flags) ||
			!writer.sequence(10, PHYSICS_INTEGRATION_MATRIX_FLOATS))
			return false;
		for (unsigned element = 0;
			element != PHYSICS_INTEGRATION_MATRIX_FLOATS; ++element)
			if (!writer.f32(11, value.matrix[element])) return false;
		if (!writer.sequence(12, PHYSICS_INTEGRATION_VECTOR_FLOATS))
			return false;
		for (unsigned element = 0;
			element != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++element)
			if (!writer.f32(13, value.acceleration[element])) return false;
		if (!writer.sequence(14, PHYSICS_INTEGRATION_VECTOR_FLOATS))
			return false;
		for (unsigned element = 0;
			element != PHYSICS_INTEGRATION_VECTOR_FLOATS; ++element)
			if (!writer.f32(15, value.velocity[element])) return false;
		if (!writer.f32(16, value.yawRate) ||
			!writer.f32(17, value.rollRate) ||
			!writer.f32(18, value.pitchRate))
			return false;
	}
	return true;
}

bool computePhysicsReferenceSerial(const void *immutableInput,
	void *detachedOutput)
{
	const PhysicsReferenceInput &input =
		*static_cast<const PhysicsReferenceInput *>(immutableInput);
	PhysicsReferenceOutput &output =
		*static_cast<PhysicsReferenceOutput *>(detachedOutput);
	if (input.snapshots == 0 || input.count == 0 || output.outputs == 0 ||
		output.capacity < input.count)
		return false;
	for (unsigned index = 0; index != input.count; ++index)
		if (!ComputePhysicsIntegrationPrefix(input.snapshots[index],
			output.outputs[index]))
			return false;
	output.count = input.count;
	return true;
}

void observePhysicsReferenceBatch(const PhysicsIntegrationOptions &options,
	const PhysicsIntegrationSnapshot *snapshots, unsigned count,
	PhysicsIntegrationOutput *outputs)
{
	if (options.performanceReferenceBatch == 0)
		return;
	*options.performanceReferenceBatch =
		performance::KernelPerformanceReferenceBatch();
	if (options.performanceReferenceLedger == 0 ||
		options.performanceBatch.valid() == false)
		return;
	const performance::KernelPerformanceReferenceMode mode =
		options.performanceReferenceLedger->mode();
	if (mode == performance::KERNEL_REFERENCE_DISABLED)
		return;
	performance::KernelPerformanceBatchIdentity identity;
	if (!performance::KernelPerformanceLedger::instance().describeBatch(
		options.performanceBatch, identity) ||
		identity.kernel != performance::KERNEL_PERFORMANCE_PHYSICS ||
		identity.subtype != 0)
		return;
	PhysicsReferenceInput input;
	input.snapshots = snapshots;
	input.count = count;
	PhysicsReferenceOutput production;
	production.outputs = outputs;
	production.count = count;
	production.capacity = count;
	if (options.performanceReferenceAttempt.valid())
	{
		*options.performanceReferenceBatch =
			options.performanceReferenceLedger->observeValidatedAttempt(
				options.performanceReferenceAttempt, writePhysicsReferenceOutput, &production);
		return;
	}
	PhysicsReferenceOutput detached;
	detached.outputs = mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
		options.performanceReferenceOutput : 0;
	detached.count = 0;
	detached.capacity = options.performanceReferenceOutputCapacity;
	const performance::KernelPerformanceSerialCallback serialCompute =
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
		computePhysicsReferenceSerial : 0;
	void *detachedContext =
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
		&detached : 0;
	*options.performanceReferenceBatch =
		options.performanceReferenceLedger->observeValidatedBatch(
		identity.kernel, identity.subtype, identity.frame, identity.ordinal,
		PHYSICS_REFERENCE_FIELD_SCHEMA, count, writePhysicsReferenceInput,
		&input, writePhysicsReferenceOutput, &production, serialCompute,
		detachedContext);
}
#endif

struct PhysicsIntegrationExecutionRecord
{
	PhysicsIntegrationExecutionRecord()
		: completed(false), physicalWorker(false), ownerHelped(false),
		  physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX)
#if defined(_WIN64)
		, traceSource(false), range()
#endif
	{}
	bool completed;
	bool physicalWorker;
	bool ownerHelped;
	unsigned physicalWorkerIndex;
#if defined(_WIN64)
	bool traceSource;
	performance::KernelPerformanceRangePlan range;
	performance::KernelPerformanceCheckpointProbe checkpoint;
#endif
};

#if defined(_WIN64)
class PhysicsSourceBodyScope
{
public:
	PhysicsSourceBodyScope(PhysicsIntegrationExecutionRecord &execution,
		const unsigned &units, bool inlineExecution) : m_execution(execution), m_units(units)
	{ if (m_execution.traceSource && !inlineExecution) m_execution.checkpoint.beginRecord(); }
	~PhysicsSourceBodyScope()
	{
		if (!m_execution.traceSource) return;
		const performance::KernelPerformanceCheckpoint at = {4, m_units, m_execution.range.end};
		m_execution.checkpoint.finish(at, m_units, m_execution.completed ?
			performance::KERNEL_RANGE_COMPLETED : m_execution.checkpoint.snapshot().firstTruePoll != 0 ?
			performance::KERNEL_RANGE_CANCELLED : performance::KERNEL_RANGE_FAILED);
	}
private:
	PhysicsIntegrationExecutionRecord &m_execution;
	const unsigned &m_units;
};

bool physicsSourceCheckpoint(PhysicsIntegrationExecutionRecord &execution,
	unsigned site, unsigned units, bool actual)
{
	if (!execution.traceSource) return actual;
	const performance::KernelPerformanceCheckpoint at = {site, units, execution.range.end};
	return execution.checkpoint.cancelled(at, actual);
}

// Owner-only metadata import follows the real acquire; it never executes work.
class PhysicsSourceAttempt
{
public:
	PhysicsSourceAttempt(const PhysicsIntegrationOptions &options,
		const PhysicsIntegrationSnapshot *snapshots, unsigned count, unsigned grain,
		JobSystem &jobs, PhysicsIntegrationMetrics &metrics) : m_options(options),
		m_metrics(metrics), m_count(count), m_grain(grain),
		m_workers(jobs.workerCount()), m_pending(jobs.pendingOwnerCompletionCount()),
		m_outstanding(jobs.outstandingJobCount()), m_enabled(false), m_released(false)
	{
		if (options.performanceReferenceLedger == 0 || !options.performanceReferenceAttempt.valid()) return;
		const performance::KernelPerformanceReferenceMode mode = options.performanceReferenceLedger->mode();
		if (mode != performance::KERNEL_REFERENCE_THROUGHPUT_BINDING &&
			mode != performance::KERNEL_REFERENCE_SERIAL_ORACLE) return;
		const PhysicsReferenceInput input = {snapshots, count};
		m_enabled = options.performanceReferenceLedger->bindCapturedInput(
			options.performanceReferenceAttempt, PHYSICS_REFERENCE_FIELD_SCHEMA,
			count, writePhysicsReferenceInput, &input);
		performance::KernelPerformanceCanonicalWriter facts;
		if (m_enabled && facts.begin(1) && facts.u32(1, count) && facts.u32(2, grain))
			m_facts = facts.finish();
	}
	~PhysicsSourceAttempt() { release(0, 0, false, false); }
	void plan(PhysicsIntegrationExecutionRecord &execution, unsigned ordinal,
		unsigned begin, unsigned end)
	{
		execution.traceSource = m_enabled;
		execution.range = {1, ordinal, 1, begin, end, end - begin};
	}
	void release(PhysicsIntegrationExecutionRecord *executions, unsigned submitted,
		bool published, bool cancelled)
	{
		if (!m_enabled || m_released) return;
		m_released = true;
		performance::KernelPerformanceReferenceLedger &ledger = *m_options.performanceReferenceLedger;
		const performance::KernelPerformanceAttempt attempt = m_options.performanceReferenceAttempt;
		performance::KernelPerformanceAttemptDecision decision = {};
		decision.site = 1; decision.reasonSchema = 1; decision.reason = submitted == 0 ? 2 : cancelled ? 3 : 1;
		decision.deterministicEligible = true; decision.deterministicFacts = m_facts;
		decision.admission = submitted != 0 ? performance::KERNEL_ADMISSION_ACCEPTED : performance::KERNEL_ADMISSION_REFUSED;
		decision.sourceConfiguredWorkers = m_workers; decision.dynamicFactsKnownMask = 3;
		decision.pendingJobs = m_pending; decision.outstandingJobs = m_outstanding;
		if (m_options.testHooks != 0 && m_options.testHooks->releasedGroup != 0)
		{
			unsigned completedBodies = 0;
			for (unsigned i = 0; i != submitted; ++i) if (executions[i].completed) ++completedBodies;
			m_options.testHooks->releasedGroup(m_options.testHooks->context,
				cancelled, completedBodies, submitted, decision.reason);
		}
		ledger.observeDecision(attempt, decision);
		if (submitted == 0) return;
		m_metrics.referenceAdmissionAccepted = true;
		const performance::KernelPerformanceDispatchPlan dispatch = {1, 1, 1, submitted,
			m_count, m_grain, PHYSICS_INTEGRATION_MAXIMUM_SNAPSHOTS};
		ledger.observeDispatch(attempt, dispatch);
		for (unsigned i = 0; i != submitted; ++i) ledger.observeRangePlan(attempt, executions[i].range);
		for (unsigned i = 0; i != submitted; ++i)
		{
			performance::KernelPerformanceRangeProgress progress = {};
			progress.checkpoint = executions[i].checkpoint.snapshot();
			progress.publication = !progress.checkpoint.entered ? performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
				published ? performance::KERNEL_PUBLICATION_PUBLISHED : cancelled ||
				progress.checkpoint.terminal == performance::KERNEL_RANGE_CANCELLED ?
				performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : performance::KERNEL_PUBLICATION_REJECTED;
			ledger.observeReleasedRange(attempt, executions[i].range, progress);
		}
	}
private:
	const PhysicsIntegrationOptions &m_options;
	PhysicsIntegrationMetrics &m_metrics;
	unsigned m_count, m_grain, m_workers;
	JobMetricCounter m_pending, m_outstanding;
	bool m_enabled, m_released;
	performance::KernelPerformanceDigest m_facts;
};
#endif

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

#if defined(_WIN64)
void observePhysicsTest(const PhysicsIntegrationTestHooks *hooks,
	PhysicsIntegrationTestEvent event, unsigned rangeIndex, unsigned begin,
	unsigned end, unsigned workUnits = 0, bool completed = false,
	PhysicsIntegrationOutput *storage = 0)
{
	if (hooks != 0 && hooks->observe != 0)
		hooks->observe(hooks->context, event, rangeIndex, begin, end,
			workUnits, completed, storage);
}

bool physicsTestCheckpoint(const PhysicsIntegrationTestHooks *hooks,
	unsigned rangeIndex, PhysicsIntegrationTestCheckpoint site,
	unsigned workUnits, bool actual)
{
	return hooks != 0 && hooks->checkpoint != 0 ?
		hooks->checkpoint(hooks->context, rangeIndex, site, workUnits, actual) : actual;
}

class PhysicsTestBodyScope
{
public:
	PhysicsTestBodyScope(const PhysicsIntegrationTestHooks *hooks, unsigned range,
		unsigned begin, unsigned end, const unsigned &units, const bool &completed)
		: m_hooks(hooks), m_range(range), m_begin(begin), m_end(end),
		m_units(units), m_completed(completed)
	{ observePhysicsTest(m_hooks, PHYSICS_INTEGRATION_TEST_RANGE_ENTERED, m_range, m_begin, m_end); }
	~PhysicsTestBodyScope()
	{ observePhysicsTest(m_hooks, PHYSICS_INTEGRATION_TEST_RANGE_FINISHED, m_range, m_begin, m_end, m_units, m_completed); }
private:
	const PhysicsIntegrationTestHooks *m_hooks;
	unsigned m_range, m_begin, m_end;
	const unsigned &m_units;
	const bool &m_completed;
};
#endif

class PhysicsIntegrationJob : public Job
{
public:
	PhysicsIntegrationJob(const PhysicsIntegrationSnapshot *snapshots,
		PhysicsIntegrationOutput *scratch, unsigned rangeIndex,
		unsigned begin, unsigned end, const JobFloatingPointState &floatingPointState,
		PhysicsIntegrationTestFault testFault, unsigned testOrdinal,
		PhysicsIntegrationExecutionRecord *execution,
		PhysicsJobAtomicUnsigned *activePhysicalWorkers,
		PhysicsJobAtomicUnsigned *peakPhysicalWorkers
#if defined(_WIN64)
		, const PhysicsIntegrationTestHooks *testHooks
#endif
		)
		: m_snapshots(snapshots), m_scratch(scratch),
		  m_rangeIndex(rangeIndex), m_begin(begin), m_end(end),
		  m_floatingPointState(floatingPointState), m_testFault(testFault),
		  m_testOrdinal(testOrdinal), m_execution(execution),
		  m_activePhysicalWorkers(activePhysicalWorkers),
		  m_peakPhysicalWorkers(peakPhysicalWorkers)
#if defined(_WIN64)
		, m_testHooks(testHooks)
#endif
	{
	}

	virtual void execute(JobContext &context) { executeBody(&context); }
#if defined(_WIN64)
	void executeInline() { executeBody(0); }
#endif

	void executeBody(JobContext *context)
	{
		JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_execution->physicalWorker = context != 0 && context->isPhysicalWorkerExecution();
		m_execution->ownerHelped = context != 0 && !m_execution->physicalWorker;
		if (m_execution->physicalWorker)
			m_execution->physicalWorkerIndex = context->physicalWorkerIndex();
		PhysicsPhysicalExecutionScope physicalScope(m_execution->physicalWorker,
			m_activePhysicalWorkers, m_peakPhysicalWorkers);
#if defined(_WIN64)
		unsigned completedUnits = 0;
		PhysicsSourceBodyScope sourceScope(*m_execution, completedUnits, context == 0);
		PhysicsTestBodyScope testScope(m_testHooks, m_rangeIndex, m_begin, m_end,
			completedUnits, m_execution->completed);
#endif
		if (m_testFault == PHYSICS_INTEGRATION_TEST_WORKER_FAILURE &&
			m_rangeIndex == m_testOrdinal)
		{
			if (context != 0) context->fail();
			return;
		}
		for (unsigned index = m_begin; index != m_end; ++index)
		{
			if ((index - m_begin) % 64 == 0)
			{
				bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
				cancelled = physicsTestCheckpoint(m_testHooks, m_rangeIndex,
					index == m_begin ? PHYSICS_INTEGRATION_TEST_CHECKPOINT_ENTRY :
					PHYSICS_INTEGRATION_TEST_CHECKPOINT_BLOCK, index - m_begin, cancelled);
				cancelled = physicsSourceCheckpoint(*m_execution,
					index == m_begin ? 1 : 2, index - m_begin, cancelled);
#endif
				if (cancelled) return;
			}
#if defined(_WIN64)
			observePhysicsTest(m_testHooks, PHYSICS_INTEGRATION_TEST_ITEM_EVALUATED,
				m_rangeIndex, m_begin, m_end, index - m_begin);
#endif
			if (!ComputePhysicsIntegrationPrefix(m_snapshots[index], m_scratch[index]))
			{
				if (context != 0) context->fail();
				return;
			}
			if (m_testFault == PHYSICS_INTEGRATION_TEST_NONFINITE_OUTPUT &&
				m_rangeIndex == m_testOrdinal && index == m_begin)
				m_scratch[index].velocity[0] = FLT_MAX * FLT_MAX;
#if defined(_WIN64)
			completedUnits = index - m_begin + 1;
#endif
		}
		bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = physicsTestCheckpoint(m_testHooks, m_rangeIndex,
			PHYSICS_INTEGRATION_TEST_CHECKPOINT_POST_BODY, completedUnits, cancelled);
		cancelled = physicsSourceCheckpoint(*m_execution, 3, completedUnits, cancelled);
#endif
		if (cancelled)
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
#if defined(_WIN64)
	const PhysicsIntegrationTestHooks *m_testHooks;
#endif
};


#if defined(_WIN64)
bool validatePhysicsPreparedOutput(const PhysicsIntegrationOptions &options,
	const PhysicsIntegrationSnapshot *snapshots, unsigned count, PhysicsIntegrationOutput *scratch)
{
	observePhysicsTest(options.testHooks, PHYSICS_INTEGRATION_TEST_OWNER_VALIDATION,
		0, 0, count, count, true, scratch);
	for (unsigned i = 0; i != count; ++i)
		if (!ValidatePhysicsIntegrationOutput(snapshots[i], scratch[i])) return false;
	return true;
}

PhysicsIntegrationBatchResult consumePhysicsPrefixes(
	const PhysicsIntegrationSnapshot *snapshots, unsigned count, PhysicsIntegrationOutput *output,
	PhysicsIntegrationOutput *scratch, const PhysicsIntegrationOptions &options,
	PhysicsIntegrationMetrics &metrics)
{
	using namespace performance;
	KernelPerformanceReferenceLedger &ledger = *options.performanceReferenceLedger;
	const KernelPerformanceAttempt attempt = options.performanceReferenceAttempt;
	if (!attempt.valid() || ledger.mode() != KERNEL_REFERENCE_PHASE_BASELINE_BINDING ||
		!options.performanceBatch.valid()) return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
	const unsigned grain = metrics.effectiveMinimumGrain;
	const PhysicsReferenceInput input = {snapshots, count};
	KernelPerformanceCanonicalWriter facts;
	KernelPerformanceAttemptDecision decision = {};
	if (!facts.begin(1) || !facts.u32(1, count) || !facts.u32(2, grain) ||
		!ledger.bindCapturedInput(attempt, PHYSICS_REFERENCE_FIELD_SCHEMA, count, writePhysicsReferenceInput, &input) ||
		!ledger.replayDecision(attempt, 1, true, facts.finish(), decision) ||
		decision.admission != KERNEL_ADMISSION_ACCEPTED || decision.reasonSchema != 1 ||
		(decision.reason != 1 && decision.reason != 3)) return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
	metrics.referenceAdmissionAccepted = true;
	KernelPerformanceDispatchPlan dispatch = {};
	KernelPerformanceAttemptFinish sourceFinish = {};
	if (!ledger.readSourceDispatch(attempt, 1, dispatch) || !ledger.readSourceFinish(attempt, sourceFinish) ||
		dispatch.bodySchema != 1 || dispatch.checkpointSchema != 1 || dispatch.operationCount != count ||
		dispatch.sourceGrain != grain || dispatch.sourceLimit != PHYSICS_INTEGRATION_MAXIMUM_SNAPSHOTS ||
		dispatch.rangeCount < 2 || dispatch.rangeCount > count ||
		dispatch.rangeCount != JobSystem::chooseRangeCount(count, grain, decision.sourceConfiguredWorkers) ||
		(decision.reason == 3 && sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED))
		return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
	const unsigned rangeCount = dispatch.rangeCount;
	std::unique_ptr<PhysicsIntegrationExecutionRecord[]> executions(
		new (std::nothrow) PhysicsIntegrationExecutionRecord[rangeCount]);
	{
		// Real owner plan setup ends before any authenticated inline body.
		KernelPerformanceScope schedule(&KernelPerformanceLedger::instance(), options.performanceBatch, KERNEL_PERFORMANCE_SCHEDULE);
		if (!executions || !ledger.observeDispatch(attempt, dispatch)) return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
		for (unsigned i = 0; i != rangeCount; ++i)
		{
			JobRange range;
			KernelPerformanceRangePlan source = {};
			if (!JobSystem::rangeForIndex(count, rangeCount, i, range) ||
				!ledger.readSourceRange(attempt, 1, i, source) || source.bodyKind != 1 ||
				source.begin != range.begin || source.end != range.end || source.operationCount != range.end - range.begin ||
				!ledger.observeRangePlan(attempt, source)) return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
			executions[i].range = source;
		}
	}
	bool complete = true;
	const JobFloatingPointState floatingPointState;
	for (unsigned i = 0; i != rangeCount; ++i)
	{
		PhysicsIntegrationExecutionRecord &record = executions[i];
		KernelPerformanceInlineBody body;
		const KernelPerformanceInlineAction action = ledger.beginInlineBody(attempt,
			record.range, KernelPerformanceLedger::instance(), body, record.checkpoint);
		if (action == KERNEL_INLINE_INVALID) return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
		if (action == KERNEL_INLINE_EXECUTE)
		{
			record.traceSource = true;
			PhysicsIntegrationJob job(snapshots, scratch, i, static_cast<unsigned>(record.range.begin),
				static_cast<unsigned>(record.range.end), floatingPointState, options.testFault,
				options.testOrdinal, &record, 0, 0, options.testHooks);
			job.executeInline();
		}
		KernelPerformanceRangeProgress progress = {};
		progress.checkpoint = record.checkpoint.snapshot();
		progress.publication = !progress.checkpoint.entered ? KERNEL_PUBLICATION_NOT_APPLICABLE :
			decision.reason == 3 || progress.checkpoint.terminal == KERNEL_RANGE_CANCELLED ?
			KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : sourceFinish.validationObserved ?
			KERNEL_PUBLICATION_PUBLISHED : KERNEL_PUBLICATION_REJECTED;
		if ((action == KERNEL_INLINE_EXECUTE && !ledger.finishInlineBody(body, progress)) ||
			!ledger.observeReleasedRange(attempt, record.range, progress)) return PHYSICS_INTEGRATION_SERIAL_FALLBACK;
		complete = record.completed && complete;
	}
	bool published = false;
	{
		KernelPerformanceScope validate(&KernelPerformanceLedger::instance(),
			options.performanceBatch, KERNEL_PERFORMANCE_VALIDATE);
		if (decision.reason != 3 && complete &&
			validatePhysicsPreparedOutput(options, snapshots, count, scratch) &&
			sourceFinish.validationObserved)
		{
			observePhysicsReferenceBatch(options, snapshots, count, scratch);
			if (options.performanceReferenceBatch != 0 &&
				options.performanceReferenceBatch->valid() &&
				sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED)
			{
				memcpy(output, scratch, count * sizeof(PhysicsIntegrationOutput));
				observePhysicsTest(options.testHooks, PHYSICS_INTEGRATION_TEST_PUBLICATION,
					0, 0, count, count, true);
				published = true;
			}
		}
	}
	executions.reset();
	for (unsigned i = 0; i != rangeCount; ++i)
	{
		JobRange range; JobSystem::rangeForIndex(count, rangeCount, i, range);
		observePhysicsTest(options.testHooks, PHYSICS_INTEGRATION_TEST_RANGE_RELEASED, i, range.begin, range.end);
	}
	metrics.rangeCount = rangeCount;
	return published ? PHYSICS_INTEGRATION_PARALLEL : decision.reason == 3 ?
		PHYSICS_INTEGRATION_CANCELLED : PHYSICS_INTEGRATION_SERIAL_FALLBACK;
}
#endif

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
#if defined(_WIN64)
	, performanceBatch(), performanceReferenceLedger(0), performanceReferenceAttempt(), testHooks(0),
	performanceReferenceBatch(0), performanceReferenceOutput(0),
	performanceReferenceOutputCapacity(0)
#endif
{
}

PhysicsIntegrationMetrics::PhysicsIntegrationMetrics()
	: snapshotCount(0), rangeCount(0), effectiveMinimumGrain(0),
	  submittedJobs(0), completedJobs(0), physicalWorkerJobs(0),
	  ownerHelpedJobs(0), physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  physicalWorkerMaskComplete(true), referenceAdmissionAccepted(false),
	  peakConcurrentPhysicalWorkers(0),
	  serialFallbacks(0), allocatedBytes(0),
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
	  acceptedPhysicalWorkerMaskComplete(true),
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
#if defined(_WIN64)
	if (options.performanceReferenceLedger != 0 &&
		options.performanceReferenceLedger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
		return consumePhysicsPrefixes(snapshots, snapshotCount, output, scratch, options, *metrics);
#endif
	if (jobs.workerCount() <= 1)
		return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;
	const unsigned rangeCount = JobSystem::chooseRangeCount(snapshotCount,
		metrics->effectiveMinimumGrain, jobs.workerCount());
	metrics->rangeCount = rangeCount;
	if (rangeCount <= 1)
		return PHYSICS_INTEGRATION_POLICY_INELIGIBLE;

#if defined(_WIN64)
	PhysicsSourceAttempt sourceAttempt(options, snapshots, snapshotCount,
		metrics->effectiveMinimumGrain, jobs, *metrics);
#endif

	JobSubmission *submissions = 0;
	JobHandle *handles = 0;
	PhysicsIntegrationJob **jobPointers = 0;
	PhysicsIntegrationExecutionRecord *executions = 0;
	JobGroup group;
	const JobFloatingPointState floatingPointState;
	PhysicsJobAtomicUnsigned activePhysicalWorkers(0);
	PhysicsJobAtomicUnsigned peakPhysicalWorkers(0);
	bool jobsReady = true;
	bool admitted = false;
#if defined(_WIN64)
	performance::KernelPerformanceLedger *performanceLedger =
		options.performanceBatch.valid() ?
		&performance::KernelPerformanceLedger::instance() : 0;
#endif
	{
#if defined(_WIN64)
		performance::KernelPerformanceScope scheduleScope(performanceLedger,
			options.performanceBatch, performance::KERNEL_PERFORMANCE_SCHEDULE);
#endif
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
		submissions = new (std::nothrow) JobSubmission[rangeCount];
		handles = new (std::nothrow) JobHandle[rangeCount];
		jobPointers = new (std::nothrow)
			PhysicsIntegrationJob *[rangeCount];
		executions = new (std::nothrow)
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

		try
		{
			if (options.testFault != PHYSICS_INTEGRATION_TEST_GROUP_FAILURE)
				group = jobs.createGroup();
		}
		catch (...)
		{
			// Treat allocator exhaustion exactly like an invalid scheduler group.
		}
		if (!group.isValid())
		{
			delete[] submissions;
			delete[] handles;
			delete[] jobPointers;
			delete[] executions;
			return fallback(&jobs, *metrics);
		}

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
#if defined(_WIN64)
			sourceAttempt.plan(executions[rangeIndex], rangeIndex, range.begin, range.end);
#endif
			jobPointers[rangeIndex] = new (std::nothrow) PhysicsIntegrationJob(
				snapshots, scratch, rangeIndex, range.begin, range.end,
				floatingPointState, options.testFault, options.testOrdinal,
				executions + rangeIndex, &activePhysicalWorkers,
				&peakPhysicalWorkers
#if defined(_WIN64)
				, options.testHooks
#endif
				);
			if (jobPointers[rangeIndex] == 0)
			{
				jobsReady = false;
				break;
			}
			submissions[rangeIndex].job = jobPointers[rangeIndex];
			submissions[rangeIndex].priority = JOB_PRIORITY_FRAME_CRITICAL;
		}

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
	}
	if (options.testFault == PHYSICS_INTEGRATION_TEST_CANCEL_AFTER_ADMISSION)
		jobs.cancel(group);
	metrics->prepareNanoseconds = PhysicsIntegrationClockNowNanoseconds() -
		prepareStart;
	const PhysicsIntegrationMetricCounter waitStart =
		PhysicsIntegrationClockNowNanoseconds();
	unsigned physicalCompletionTimeoutMilliseconds = 8;
#if defined(_WIN64)
	if (options.testHooks != 0 && options.testHooks->physicalWaitMilliseconds != 0)
		physicalCompletionTimeoutMilliseconds = options.testHooks->physicalWaitMilliseconds < 1000 ?
			options.testHooks->physicalWaitMilliseconds : 1000;
#endif
	const bool forcePhysicalTimeout = options.testFault ==
		PHYSICS_INTEGRATION_TEST_PHYSICAL_WAIT_TIMEOUT;
	bool physicalFenceCompleted = false;
	{
#if defined(_WIN64)
		performance::KernelPerformanceScope waitScope(performanceLedger,
			options.performanceBatch, performance::KERNEL_PERFORMANCE_WAIT);
		if (options.testHooks != 0 && options.testHooks->beforeWait != 0)
			options.testHooks->beforeWait(options.testHooks->context);
#endif
		physicalFenceCompleted = !forcePhysicalTimeout &&
			jobs.waitWithoutOwnerHelp(group, physicalCompletionTimeoutMilliseconds);
		if (!physicalFenceCompleted)
		{
			jobs.cancel(group);
#if defined(_WIN64)
			if (options.testHooks != 0 && options.testHooks->afterCancel != 0)
				options.testHooks->afterCancel(options.testHooks->context);
#endif
			jobs.wait(group);
		}
		else
		{
			// The passive fence proved that every job completed without owner help.
			jobs.wait(group);
		}
	}
	metrics->waitNanoseconds = PhysicsIntegrationClockNowNanoseconds() - waitStart;
	const PhysicsIntegrationMetricCounter finalizeStart =
		PhysicsIntegrationClockNowNanoseconds();
	PhysicsIntegrationBatchResult result = PHYSICS_INTEGRATION_PARALLEL;
	{
#if defined(_WIN64)
		performance::KernelPerformanceScope validateScope(performanceLedger,
			options.performanceBatch, performance::KERNEL_PERFORMANCE_VALIDATE);
#endif
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
					else
						metrics->physicalWorkerMaskComplete = false;
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

		if (!physicalFenceCompleted || group.wasCancelled())
			result = fallback(&jobs, *metrics, PHYSICS_INTEGRATION_CANCELLED);
		else if (group.failed() || metrics->completedJobs != metrics->submittedJobs)
			result = fallback(&jobs, *metrics);
		else if (metrics->physicalWorkerJobs != metrics->completedJobs ||
			metrics->ownerHelpedJobs != 0)
			result = fallback(&jobs, *metrics);
		if (result == PHYSICS_INTEGRATION_PARALLEL)
		{
#if defined(_WIN64)
			if (!validatePhysicsPreparedOutput(options, snapshots, snapshotCount, scratch))
				result = fallback(&jobs, *metrics);
#else
			for (unsigned validationIndex = 0; validationIndex != snapshotCount; ++validationIndex)
			{
				if (!ValidatePhysicsIntegrationOutput(snapshots[validationIndex], scratch[validationIndex]))
				{
					result = fallback(&jobs, *metrics);
					break;
				}
			}
#endif
		}
		if (result == PHYSICS_INTEGRATION_PARALLEL)
		{
			memcpy(output, scratch, snapshotCount * sizeof(PhysicsIntegrationOutput));
		#if defined(_WIN64)
			observePhysicsTest(options.testHooks, PHYSICS_INTEGRATION_TEST_PUBLICATION,
				0, 0, snapshotCount, snapshotCount, true);
			sourceAttempt.release(executions, metrics->submittedJobs, true, false);
			// Reference hashing and any detached oracle work belong to the owner
			// validation interval, after the real output is complete.
			observePhysicsReferenceBatch(options, snapshots, snapshotCount, output);
		#endif
		}
#if defined(_WIN64)
		else sourceAttempt.release(executions, metrics->submittedJobs, false, group.wasCancelled());
#endif
	}

	delete[] submissions;
	delete[] handles;
	delete[] jobPointers;
	delete[] executions;
#if defined(_WIN64)
	if (options.testHooks != 0)
		for (unsigned released = 0; released != rangeCount; ++released)
		{
			JobRange range;
			JobSystem::rangeForIndex(snapshotCount, rangeCount, released, range);
			observePhysicsTest(options.testHooks, PHYSICS_INTEGRATION_TEST_RANGE_RELEASED,
				released, range.begin, range.end);
		}
#endif
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
	resetMetric(s_acceptedPhysicalWorkerMaskIncomplete);
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
	metrics.acceptedPhysicalWorkerMaskComplete =
		loadMetric(s_acceptedPhysicalWorkerMaskIncomplete) == 0;
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
	if (!sliceMetrics.physicalWorkerMaskComplete)
		addMetric(s_acceptedPhysicalWorkerMaskIncomplete, 1);
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
