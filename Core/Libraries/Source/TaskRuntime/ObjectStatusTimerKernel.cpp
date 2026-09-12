/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectStatusTimerKernel.h"

#include "Lib/SimulationCommandBuffer.h"

#include <new>
#if defined(_WIN64)
#include <memory>
#endif
#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <atomic>
#endif

namespace rts
{
namespace
{
enum
{
	OBJECT_STATUS_TIMER_MODULE_TYPE = 0x5354,
	OBJECT_STATUS_TIMER_CLEAR_EXPIRED = 1
};

#if defined(_MSC_VER) && _MSC_VER < 1300
typedef ObjectStatusTimerMetricCounter StatusMetricAtomic;
typedef unsigned StatusJobAtomicUnsigned;
inline ObjectStatusTimerMetricCounter loadMetric(const StatusMetricAtomic &value) { return value; }
inline void resetMetric(StatusMetricAtomic &value) { value = 0; }
inline void addMetric(StatusMetricAtomic &value, ObjectStatusTimerMetricCounter amount) { value += amount; }
inline void orMetric(StatusMetricAtomic &value, ObjectStatusTimerMetricCounter mask) { value |= mask; }
inline void maximizeMetric(StatusMetricAtomic &value, ObjectStatusTimerMetricCounter candidate)
{
	if (candidate > value) value = candidate;
}
inline unsigned incrementJobCounter(StatusJobAtomicUnsigned &value) { return ++value; }
inline void decrementJobCounter(StatusJobAtomicUnsigned &value) { --value; }
inline unsigned loadJobCounter(const StatusJobAtomicUnsigned &value) { return value; }
inline void maximizeJobCounter(StatusJobAtomicUnsigned &value, unsigned candidate)
{
	if (candidate > value) value = candidate;
}
#else
typedef std::atomic<ObjectStatusTimerMetricCounter> StatusMetricAtomic;
typedef std::atomic<unsigned> StatusJobAtomicUnsigned;
inline ObjectStatusTimerMetricCounter loadMetric(const StatusMetricAtomic &value)
{
	return value.load(std::memory_order_relaxed);
}
inline void resetMetric(StatusMetricAtomic &value)
{
	value.store(0, std::memory_order_relaxed);
}
inline void addMetric(StatusMetricAtomic &value, ObjectStatusTimerMetricCounter amount)
{
	value.fetch_add(amount, std::memory_order_relaxed);
}
inline void orMetric(StatusMetricAtomic &value, ObjectStatusTimerMetricCounter mask)
{
	value.fetch_or(mask, std::memory_order_relaxed);
}
inline void maximizeMetric(StatusMetricAtomic &value,
	ObjectStatusTimerMetricCounter candidate)
{
	ObjectStatusTimerMetricCounter observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
inline unsigned incrementJobCounter(StatusJobAtomicUnsigned &value)
{
	return value.fetch_add(1, std::memory_order_acq_rel) + 1;
}
inline void decrementJobCounter(StatusJobAtomicUnsigned &value)
{
	value.fetch_sub(1, std::memory_order_acq_rel);
}
inline unsigned loadJobCounter(const StatusJobAtomicUnsigned &value)
{
	return value.load(std::memory_order_relaxed);
}
inline void maximizeJobCounter(StatusJobAtomicUnsigned &value, unsigned candidate)
{
	unsigned observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
#endif

StatusMetricAtomic s_resetEpoch;
StatusMetricAtomic s_authoritativeBatches;
StatusMetricAtomic s_committedCommands;
StatusMetricAtomic s_submittedJobs;
StatusMetricAtomic s_completedJobs;
StatusMetricAtomic s_physicalWorkerJobs;
StatusMetricAtomic s_ownerHelpedJobs;
StatusMetricAtomic s_physicalWorkerMask;
StatusMetricAtomic s_maximumDistinctPhysicalWorkers;
StatusMetricAtomic s_physicalWorkerMaskIncomplete;
StatusMetricAtomic s_maximumPeakConcurrentPhysicalWorkers;
StatusMetricAtomic s_shadowExecutions;
StatusMetricAtomic s_shadowCommands;
StatusMetricAtomic s_shadowMatches;
StatusMetricAtomic s_shadowMismatches;
StatusMetricAtomic s_ownerFallbacks;
StatusMetricAtomic s_staleRejections;

struct ObjectStatusTimerPayload
{
	unsigned expiredMask;
};

struct ObjectStatusTimerExecutionRecord
{
	ObjectStatusTimerExecutionRecord()
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
class StatusSourceBodyScope
{
public:
	StatusSourceBodyScope(ObjectStatusTimerExecutionRecord &execution,
		const unsigned &units, bool inlineExecution) : m_execution(execution), m_units(units)
	{ if (m_execution.traceSource && !inlineExecution) m_execution.checkpoint.beginRecord(); }
	~StatusSourceBodyScope()
	{
		if (!m_execution.traceSource) return;
		const performance::KernelPerformanceCheckpoint at = {4, m_units, m_execution.range.end};
		m_execution.checkpoint.finish(at, m_units, m_execution.completed ?
			performance::KERNEL_RANGE_COMPLETED : m_execution.checkpoint.snapshot().firstTruePoll != 0 ?
			performance::KERNEL_RANGE_CANCELLED : performance::KERNEL_RANGE_FAILED);
	}
private:
	ObjectStatusTimerExecutionRecord &m_execution;
	const unsigned &m_units;
};

bool statusSourceCheckpoint(ObjectStatusTimerExecutionRecord *execution,
	unsigned site, unsigned units, bool actual)
{
	if (execution == 0 || !execution->traceSource) return actual;
	const performance::KernelPerformanceCheckpoint at = {site, units, execution->range.end};
	return execution->checkpoint.cancelled(at, actual);
}
#endif

unsigned expiredMaskForSnapshot(const ObjectStatusTimerSnapshot &snapshot,
	unsigned currentFrame, unsigned disabledTypeCount)
{
	unsigned expiredMask = 0;
	for (unsigned type = 0; type != disabledTypeCount; ++type)
	{
		const unsigned bit = 1u << type;
		if ((snapshot.activeMask & bit) != 0 &&
			currentFrame >= snapshot.expirationFrame[type])
			expiredMask |= bit;
	}
	return expiredMask;
}

class ObjectStatusPhysicalExecutionScope
{
public:
	ObjectStatusPhysicalExecutionScope(bool physicalWorker,
		StatusJobAtomicUnsigned *active, StatusJobAtomicUnsigned *peak)
		: m_active(physicalWorker ? active : 0)
	{
		if (m_active != 0)
		{
			const unsigned current = incrementJobCounter(*m_active);
			maximizeJobCounter(*peak, current);
		}
	}
	~ObjectStatusPhysicalExecutionScope()
	{
		if (m_active != 0)
			decrementJobCounter(*m_active);
	}
private:
	StatusJobAtomicUnsigned *m_active;
};

#if defined(_WIN64)
void observeStatusTest(const ObjectStatusTimerTestHooks *hooks,
	ObjectStatusTimerTestEvent event, unsigned rangeIndex, unsigned begin,
	unsigned end, unsigned workUnits = 0, bool completed = false,
	SimulationCommand *storage = 0)
{
	if (hooks != 0 && hooks->observe != 0)
		hooks->observe(hooks->context, event, rangeIndex, begin, end,
			workUnits, completed, storage);
}

bool statusTestCheckpoint(const ObjectStatusTimerTestHooks *hooks,
	unsigned rangeIndex, ObjectStatusTimerTestCheckpoint site,
	unsigned workUnits, bool actual)
{
	return hooks != 0 && hooks->checkpoint != 0 ?
		hooks->checkpoint(hooks->context, rangeIndex, site, workUnits, actual) : actual;
}

class StatusTestBodyScope
{
public:
	StatusTestBodyScope(const ObjectStatusTimerTestHooks *hooks, unsigned range,
		unsigned begin, unsigned end, const unsigned &units, const bool &completed)
		: m_hooks(hooks), m_range(range), m_begin(begin), m_end(end),
		m_units(units), m_completed(completed)
	{ observeStatusTest(m_hooks, OBJECT_STATUS_TIMER_TEST_RANGE_ENTERED, m_range, m_begin, m_end); }
	~StatusTestBodyScope()
	{ observeStatusTest(m_hooks, OBJECT_STATUS_TIMER_TEST_RANGE_FINISHED, m_range, m_begin, m_end, m_units, m_completed); }
private:
	const ObjectStatusTimerTestHooks *m_hooks;
	unsigned m_range, m_begin, m_end;
	const unsigned &m_units;
	const bool &m_completed;
};
#endif

bool evaluateRange(const ObjectStatusTimerSnapshot *snapshots,
	unsigned begin, unsigned end, unsigned currentFrame,
	unsigned disabledTypeCount, SimulationCommandBuffer &buffer,
	JobContext *context
#if defined(_WIN64)
	, const ObjectStatusTimerTestHooks *testHooks = 0, unsigned rangeIndex = 0,
	unsigned *completedUnits = 0, ObjectStatusTimerExecutionRecord *execution = 0
#endif
	)
{
	for (unsigned index = begin; index != end; ++index)
	{
		if ((index - begin) % 64 == 0 && (context != 0
#if defined(_WIN64)
			|| (execution != 0 && execution->traceSource)
#endif
			))
		{
			bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
			cancelled = statusTestCheckpoint(testHooks, rangeIndex,
				OBJECT_STATUS_TIMER_TEST_CHECKPOINT_BLOCK, index - begin, cancelled);
			cancelled = statusSourceCheckpoint(execution, 2, index - begin, cancelled);
#endif
			if (cancelled) return false;
		}

		const ObjectStatusTimerSnapshot &snapshot = snapshots[index];
#if defined(_WIN64)
		observeStatusTest(testHooks, OBJECT_STATUS_TIMER_TEST_ITEM_EVALUATED,
			rangeIndex, begin, end, index - begin);
#endif
		const unsigned expiredMask = expiredMaskForSnapshot(snapshot,
			currentFrame, disabledTypeCount);
#if defined(_WIN64)
		if (completedUnits != 0) *completedUnits = index - begin + 1;
#endif
		if (expiredMask == 0)
			continue;

		ObjectStatusTimerPayload payload;
		payload.expiredMask = expiredMask;
		if (!buffer.append(snapshot.ownerOrder,
			SimulationStableHandle(snapshot.objectID), SimulationStableHandle(),
			OBJECT_STATUS_TIMER_CLEAR_EXPIRED, &payload, sizeof(payload)))
			return false;
#if defined(_WIN64)
		observeStatusTest(testHooks, OBJECT_STATUS_TIMER_TEST_COMMAND_APPENDED,
			rangeIndex, begin, end, index - begin);
#endif
	}
	return true;
}

#if defined(_WIN64)
/* Reference schema 1: batch currentFrame=2, disabledTypeCount=3,
** snapshot sequence=1 with objectID/ownerOrder/activeMask=4..6 and all 16
** expiration values as sequence=7/value tag=8. Output command sequence=1
** with objectID/ownerOrder/expiredMask=2..4. */
enum { STATUS_REFERENCE_FIELD_SCHEMA = 1 };

struct StatusReferenceInput
{
	const ObjectStatusTimerSnapshot *snapshots;
	unsigned count;
	unsigned currentFrame;
	unsigned disabledTypeCount;
};

struct StatusReferenceOutput
{
	ObjectStatusTimerCommand *commands;
	unsigned count;
	unsigned capacity;
};

bool statusReferenceCommandLess(const ObjectStatusTimerCommand &left,
	const ObjectStatusTimerCommand &right)
{
	return left.ownerOrder < right.ownerOrder ||
		(left.ownerOrder == right.ownerOrder && left.objectID < right.objectID);
}

void siftStatusReferenceCommands(ObjectStatusTimerCommand *commands,
	unsigned root, unsigned end)
{
	for (;;)
	{
		const unsigned child = root * 2 + 1;
		if (child >= end)
			return;
		unsigned selected = child;
		if (child + 1 < end &&
			statusReferenceCommandLess(commands[selected], commands[child + 1]))
			selected = child + 1;
		if (!statusReferenceCommandLess(commands[root], commands[selected]))
			return;
		const ObjectStatusTimerCommand temporary = commands[root];
		commands[root] = commands[selected];
		commands[selected] = temporary;
		root = selected;
	}
}

bool computeStatusReferenceSerial(const void *immutableInput,
	void *detachedOutput)
{
	const StatusReferenceInput &input =
		*static_cast<const StatusReferenceInput *>(immutableInput);
	StatusReferenceOutput &output =
		*static_cast<StatusReferenceOutput *>(detachedOutput);
	if (input.snapshots == 0 || input.count == 0 ||
		input.disabledTypeCount == 0 ||
		input.disabledTypeCount > OBJECT_STATUS_TIMER_MAX_TYPES ||
		output.commands == 0 || output.capacity < input.count)
		return false;
	unsigned commandCount = 0;
	for (unsigned index = 0; index != input.count; ++index)
	{
		const unsigned expiredMask = expiredMaskForSnapshot(
			input.snapshots[index], input.currentFrame,
			input.disabledTypeCount);
		if (expiredMask == 0)
			continue;
		ObjectStatusTimerCommand &command = output.commands[commandCount++];
		command.objectID = input.snapshots[index].objectID;
		command.ownerOrder = input.snapshots[index].ownerOrder;
		command.expiredMask = expiredMask;
	}
	if (commandCount > 1)
	{
		unsigned heapStart = commandCount / 2;
		while (heapStart != 0)
		{
			--heapStart;
			siftStatusReferenceCommands(output.commands, heapStart,
				commandCount);
		}
		unsigned heapEnd = commandCount;
		while (heapEnd > 1)
		{
			--heapEnd;
			const ObjectStatusTimerCommand temporary = output.commands[0];
			output.commands[0] = output.commands[heapEnd];
			output.commands[heapEnd] = temporary;
			siftStatusReferenceCommands(output.commands, 0, heapEnd);
		}
	}
	for (unsigned index = 1; index != commandCount; ++index)
		if (output.commands[index - 1].ownerOrder ==
			output.commands[index].ownerOrder &&
			output.commands[index - 1].objectID ==
			output.commands[index].objectID)
			return false;
	output.count = commandCount;
	return true;
}

bool writeStatusReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const StatusReferenceInput &input =
		*static_cast<const StatusReferenceInput *>(context);
	if (input.snapshots == 0 || input.count == 0 ||
		!writer.u32(2, input.currentFrame) ||
		!writer.u32(3, input.disabledTypeCount) ||
		!writer.sequence(1, input.count))
		return false;
	for (unsigned index = 0; index != input.count; ++index)
	{
		const ObjectStatusTimerSnapshot &snapshot = input.snapshots[index];
		if (!writer.u32(4, snapshot.objectID) ||
			!writer.u32(5, snapshot.ownerOrder) ||
			!writer.u32(6, snapshot.activeMask) ||
			!writer.sequence(7, OBJECT_STATUS_TIMER_MAX_TYPES))
			return false;
		for (unsigned type = 0; type != OBJECT_STATUS_TIMER_MAX_TYPES; ++type)
			if (!writer.u32(8, snapshot.expirationFrame[type])) return false;
	}
	return true;
}

bool writeStatusReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const StatusReferenceOutput &output =
		*static_cast<const StatusReferenceOutput *>(context);
	if ((output.count != 0 && output.commands == 0) ||
		!writer.sequence(1, output.count))
		return false;
	for (unsigned index = 0; index != output.count; ++index)
	{
		const ObjectStatusTimerCommand &command = output.commands[index];
		if (!writer.u32(2, command.objectID) ||
			!writer.u32(3, command.ownerOrder) ||
			!writer.u32(4, command.expiredMask))
			return false;
	}
	return true;
}

void observeStatusReferenceBatch(const ObjectStatusTimerOptions &options,
	const ObjectStatusTimerSnapshot *snapshots, unsigned snapshotCount,
	unsigned currentFrame, unsigned disabledTypeCount,
	const ObjectStatusTimerCommand *output, unsigned outputCount)
{
	if (options.performanceReferenceBatch == 0)
		return;
	*options.performanceReferenceBatch =
		performance::KernelPerformanceReferenceBatch();
	if (options.performanceReferenceLedger == 0 ||
		!options.performanceBatch.valid())
		return;
	const performance::KernelPerformanceReferenceMode mode =
		options.performanceReferenceLedger->mode();
	if (mode == performance::KERNEL_REFERENCE_DISABLED)
		return;
	performance::KernelPerformanceBatchIdentity identity;
	if (!performance::KernelPerformanceLedger::instance().describeBatch(
		options.performanceBatch, identity) ||
		identity.kernel != performance::KERNEL_PERFORMANCE_STATUS ||
		identity.subtype != 0)
		return;
	StatusReferenceInput input;
	input.snapshots = snapshots;
	input.count = snapshotCount;
	input.currentFrame = currentFrame;
	input.disabledTypeCount = disabledTypeCount;
	StatusReferenceOutput production;
	production.commands = const_cast<ObjectStatusTimerCommand *>(output);
	production.count = outputCount;
	production.capacity = outputCount;
	if (options.performanceReferenceAttempt.valid())
	{
		*options.performanceReferenceBatch =
			options.performanceReferenceLedger->observeValidatedAttempt(
				options.performanceReferenceAttempt, writeStatusReferenceOutput, &production);
		return;
	}
	StatusReferenceOutput detached;
	detached.commands = mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
		options.performanceReferenceOutput : 0;
	detached.count = 0;
	detached.capacity = options.performanceReferenceOutputCapacity;
	const performance::KernelPerformanceSerialCallback serialCompute =
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
		computeStatusReferenceSerial : 0;
	void *detachedContext =
		mode == performance::KERNEL_REFERENCE_SERIAL_ORACLE ?
		&detached : 0;
	*options.performanceReferenceBatch =
		options.performanceReferenceLedger->observeValidatedBatch(
		identity.kernel, identity.subtype, identity.frame, identity.ordinal,
		STATUS_REFERENCE_FIELD_SCHEMA, snapshotCount,
		writeStatusReferenceInput, &input, writeStatusReferenceOutput,
		&production, serialCompute, detachedContext);
}
#endif

#if defined(_WIN64)
// The owner records only admitted ranges after their native group has drained.
class StatusSourceAttempt
{
public:
	StatusSourceAttempt(const ObjectStatusTimerOptions &options,
		const ObjectStatusTimerSnapshot *snapshots, unsigned count, unsigned frame,
		unsigned types, bool useParallel, JobSystem &jobs,
		ObjectStatusTimerMetrics &metrics) : m_options(options), m_metrics(metrics),
		m_grain(options.minimumGrain != 0 ? options.minimumGrain : OBJECT_STATUS_TIMER_DEFAULT_MINIMUM_GRAIN),
		m_workers(jobs.workerCount()), m_pending(jobs.pendingOwnerCompletionCount()),
		m_outstanding(jobs.outstandingJobCount()), m_enabled(false), m_released(false)
	{
		if (!useParallel || options.performanceReferenceLedger == 0 || !options.performanceReferenceAttempt.valid()) return;
		const performance::KernelPerformanceReferenceMode mode = options.performanceReferenceLedger->mode();
		if (mode != performance::KERNEL_REFERENCE_THROUGHPUT_BINDING &&
			mode != performance::KERNEL_REFERENCE_SERIAL_ORACLE) return;
		StatusReferenceInput input;
		input.snapshots = snapshots; input.count = count; input.currentFrame = frame; input.disabledTypeCount = types;
		m_enabled = options.performanceReferenceLedger->bindCapturedInput(
			options.performanceReferenceAttempt, STATUS_REFERENCE_FIELD_SCHEMA,
			count, writeStatusReferenceInput, &input);
		performance::KernelPerformanceCanonicalWriter facts;
		if (m_enabled && facts.begin(1) && facts.u32(1, count) && facts.u32(2, m_grain) &&
			facts.u32(3, frame) && facts.u32(4, types)) m_facts = facts.finish();
	}
	~StatusSourceAttempt() { release(0, 0, false, false); }
	void plan(ObjectStatusTimerExecutionRecord &execution, unsigned ordinal,
		unsigned begin, unsigned end)
	{
		execution.traceSource = m_enabled;
		execution.range = {1, ordinal, 1, begin, end, end - begin};
	}
	void release(ObjectStatusTimerExecutionRecord *executions, unsigned submitted,
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
		performance::KernelPerformanceDispatchPlan dispatch = {1, 1, 1, submitted,
			0, m_grain, OBJECT_STATUS_TIMER_MAXIMUM_JOBS};
		for (unsigned i = 0; i != submitted; ++i) dispatch.operationCount += executions[i].range.operationCount;
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
	const ObjectStatusTimerOptions &m_options;
	ObjectStatusTimerMetrics &m_metrics;
	unsigned m_grain, m_workers;
	JobMetricCounter m_pending, m_outstanding;
	bool m_enabled, m_released;
	performance::KernelPerformanceDigest m_facts;
};
#endif

class ObjectStatusTimerJob : public Job
{
public:
	ObjectStatusTimerJob(const ObjectStatusTimerSnapshot *snapshots,
		unsigned begin, unsigned end, unsigned currentFrame,
		unsigned disabledTypeCount, SimulationCommandBuffer *buffer,
		ObjectStatusTimerExecutionRecord *execution,
		StatusJobAtomicUnsigned *activePhysicalWorkers,
		StatusJobAtomicUnsigned *peakPhysicalWorkers
#if defined(_WIN64)
		, const ObjectStatusTimerTestHooks *testHooks, unsigned rangeIndex
#endif
		)
		: m_snapshots(snapshots), m_begin(begin), m_end(end),
		  m_currentFrame(currentFrame), m_disabledTypeCount(disabledTypeCount),
		  m_buffer(buffer), m_execution(execution),
		  m_activePhysicalWorkers(activePhysicalWorkers),
		  m_peakPhysicalWorkers(peakPhysicalWorkers)
#if defined(_WIN64)
		, m_testHooks(testHooks), m_rangeIndex(rangeIndex)
#endif
	{
	}

	virtual void execute(JobContext &context) { executeBody(&context); }
#if defined(_WIN64)
	void executeInline() { executeBody(0); }
#endif

	void executeBody(JobContext *context)
	{
		m_execution->physicalWorker = context != 0 && context->isPhysicalWorkerExecution();
		m_execution->ownerHelped = context != 0 && !m_execution->physicalWorker;
		if (m_execution->physicalWorker)
			m_execution->physicalWorkerIndex = context->physicalWorkerIndex();
		ObjectStatusPhysicalExecutionScope physicalScope(
			m_execution->physicalWorker, m_activePhysicalWorkers,
			m_peakPhysicalWorkers);
#if defined(_WIN64)
		unsigned completedUnits = 0;
		StatusSourceBodyScope sourceScope(*m_execution, completedUnits, context == 0);
		StatusTestBodyScope testScope(m_testHooks, m_rangeIndex, m_begin, m_end,
			completedUnits, m_execution->completed);
#endif
		bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = statusTestCheckpoint(m_testHooks, m_rangeIndex,
			OBJECT_STATUS_TIMER_TEST_CHECKPOINT_ENTRY, 0, cancelled);
		cancelled = statusSourceCheckpoint(m_execution, 1, 0, cancelled);
#endif
		if (cancelled || !evaluateRange(m_snapshots, m_begin, m_end, m_currentFrame,
			m_disabledTypeCount, *m_buffer, context
#if defined(_WIN64)
			, m_testHooks, m_rangeIndex, &completedUnits, m_execution
#endif
			))
		{
			m_buffer->fail();
			return;
		}
		cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = statusTestCheckpoint(m_testHooks, m_rangeIndex,
			OBJECT_STATUS_TIMER_TEST_CHECKPOINT_POST_BODY, completedUnits, cancelled);
		cancelled = statusSourceCheckpoint(m_execution, 3, completedUnits, cancelled);
#endif
		if (cancelled || !m_buffer->complete())
		{
			m_buffer->fail();
			return;
		}
		m_execution->completed = true;
	}

private:
	const ObjectStatusTimerSnapshot *m_snapshots;
	unsigned m_begin;
	unsigned m_end;
	unsigned m_currentFrame;
	unsigned m_disabledTypeCount;
	SimulationCommandBuffer *m_buffer;
	ObjectStatusTimerExecutionRecord *m_execution;
	StatusJobAtomicUnsigned *m_activePhysicalWorkers;
	StatusJobAtomicUnsigned *m_peakPhysicalWorkers;
#if defined(_WIN64)
	const ObjectStatusTimerTestHooks *m_testHooks;
	unsigned m_rangeIndex;
#endif
};

void deleteBuffers(SimulationCommandBuffer **buffers, unsigned count)
{
	for (unsigned index = 0; index != count; ++index)
		delete buffers[index];
}


bool mergeStatusPreparedCommands(const SimulationCommandBuffer *const *producerSlots,
	unsigned jobCount, SimulationCommand *commandStorage, SimulationMergedCommand *merged,
	SimulationMergedCommand *mergeScratch, unsigned snapshotCount, ObjectStatusTimerCommand *output,
	unsigned *outputCount, const ObjectStatusTimerOptions &options, ObjectStatusTimerMetrics &metrics,
	bool observePublication)
{
#if defined(_WIN64)
	observeStatusTest(options.testHooks, OBJECT_STATUS_TIMER_TEST_OWNER_REDUCTION,
		0, 0, snapshotCount, snapshotCount, true, commandStorage);
#endif
	const SimulationCommandMergeResult mergeResult = MergeSimulationCommandSlots(
		producerSlots, jobCount, merged, mergeScratch, snapshotCount);
	if (!mergeResult.succeeded()) return false;
	unsigned previousOwnerOrder = 0;
	for (unsigned index = 0; index != mergeResult.commandCount; ++index)
	{
		const SimulationCommand *command = merged[index].command();
		const ObjectStatusTimerPayload *payload =
			reinterpret_cast<const ObjectStatusTimerPayload *>(merged[index].payload());
		if (command == 0 || payload == 0 || command->commandType() != OBJECT_STATUS_TIMER_CLEAR_EXPIRED ||
			command->payloadSize() != sizeof(ObjectStatusTimerPayload) ||
			command->orderKey().moduleType() != OBJECT_STATUS_TIMER_MODULE_TYPE ||
			command->orderKey().target().isNull() || !command->orderKey().source().isNull() ||
			(index != 0 && command->orderKey().phase() <= previousOwnerOrder)) return false;
		previousOwnerOrder = command->orderKey().phase();
	}
	for (unsigned index = 0; index != mergeResult.commandCount; ++index)
	{
		const SimulationCommand *command = merged[index].command();
		const ObjectStatusTimerPayload *payload =
			reinterpret_cast<const ObjectStatusTimerPayload *>(merged[index].payload());
		output[index].objectID = command->orderKey().target().objectID();
		output[index].ownerOrder = command->orderKey().phase();
		output[index].expiredMask = payload->expiredMask;
	}
	metrics.evaluatedSnapshots = snapshotCount;
	metrics.emittedCommands = mergeResult.commandCount;
	*outputCount = mergeResult.commandCount;
#if defined(_WIN64)
	if (observePublication)
		observeStatusTest(options.testHooks, OBJECT_STATUS_TIMER_TEST_PUBLICATION,
			0, 0, snapshotCount, mergeResult.commandCount, true);
#endif
	return true;
}

#if defined(_WIN64)
ObjectStatusTimerResult consumeStatusCommands(const ObjectStatusTimerSnapshot *snapshots,
	unsigned count, unsigned frame, unsigned types, ObjectStatusTimerCommand *output,
	unsigned *outputCount, const ObjectStatusTimerOptions &options, ObjectStatusTimerMetrics &metrics)
{
	using namespace performance;
	KernelPerformanceReferenceLedger &ledger = *options.performanceReferenceLedger;
	const KernelPerformanceAttempt attempt = options.performanceReferenceAttempt;
	if (!attempt.valid() || ledger.mode() != KERNEL_REFERENCE_PHASE_BASELINE_BINDING ||
		!options.performanceBatch.valid()) return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
	const unsigned grain = options.minimumGrain != 0 ? options.minimumGrain : OBJECT_STATUS_TIMER_DEFAULT_MINIMUM_GRAIN;
	StatusReferenceInput input;
	input.snapshots = snapshots; input.count = count; input.currentFrame = frame; input.disabledTypeCount = types;
	KernelPerformanceCanonicalWriter facts;
	KernelPerformanceAttemptDecision decision = {};
	if (!facts.begin(1) || !facts.u32(1, count) || !facts.u32(2, grain) || !facts.u32(3, frame) || !facts.u32(4, types) ||
		!ledger.bindCapturedInput(attempt, STATUS_REFERENCE_FIELD_SCHEMA, count, writeStatusReferenceInput, &input) ||
		!ledger.replayDecision(attempt, 1, options.parallel && count >= OBJECT_STATUS_TIMER_MINIMUM_PARALLEL_SNAPSHOTS,
			facts.finish(), decision) || decision.admission != KERNEL_ADMISSION_ACCEPTED ||
		decision.reasonSchema != 1 || (decision.reason != 1 && decision.reason != 3))
		return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
	metrics.referenceAdmissionAccepted = true;
	KernelPerformanceDispatchPlan dispatch = {};
	KernelPerformanceAttemptFinish sourceFinish = {};
	unsigned planned = JobSystem::chooseRangeCount(count, grain, decision.sourceConfiguredWorkers);
	if (planned > OBJECT_STATUS_TIMER_MAXIMUM_JOBS) planned = OBJECT_STATUS_TIMER_MAXIMUM_JOBS;
	if (!ledger.readSourceDispatch(attempt, 1, dispatch) || !ledger.readSourceFinish(attempt, sourceFinish) ||
		dispatch.bodySchema != 1 || dispatch.checkpointSchema != 1 || dispatch.sourceGrain != grain ||
		dispatch.sourceLimit != OBJECT_STATUS_TIMER_MAXIMUM_JOBS || dispatch.rangeCount == 0 ||
		dispatch.rangeCount > planned || planned < 2 ||
		(decision.reason == 3 && sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED))
		return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
	std::unique_ptr<SimulationCommand[]> commands(new (std::nothrow) SimulationCommand[count]);
	std::unique_ptr<unsigned char[]> payloads(new (std::nothrow) unsigned char[count * sizeof(ObjectStatusTimerPayload)]);
	std::unique_ptr<SimulationMergedCommand[]> merged(new (std::nothrow) SimulationMergedCommand[count]);
	std::unique_ptr<SimulationMergedCommand[]> mergeScratch(new (std::nothrow) SimulationMergedCommand[count]);
	std::unique_ptr<ObjectStatusTimerCommand[]> prepared(new (std::nothrow)
		ObjectStatusTimerCommand[count]);
	std::unique_ptr<SimulationCommandBuffer> buffers[OBJECT_STATUS_TIMER_MAXIMUM_JOBS];
	const SimulationCommandBuffer *producerSlots[OBJECT_STATUS_TIMER_MAXIMUM_JOBS] = {};
	ObjectStatusTimerExecutionRecord executions[OBJECT_STATUS_TIMER_MAXIMUM_JOBS];
	{
		// Real owner plan setup ends before any authenticated inline body.
		KernelPerformanceScope schedule(&KernelPerformanceLedger::instance(), options.performanceBatch, KERNEL_PERFORMANCE_SCHEDULE);
		if (!commands || !payloads || !merged || !mergeScratch || !prepared ||
			!ledger.observeDispatch(attempt, dispatch))
			return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
		JobMetricCounter operations = 0;
		for (unsigned i = 0; i != dispatch.rangeCount; ++i)
		{
			JobRange range;
			KernelPerformanceRangePlan source = {};
			if (!JobSystem::rangeForIndex(count, planned, i, range) ||
				!ledger.readSourceRange(attempt, 1, i, source) || source.bodyKind != 1 ||
				source.begin != range.begin || source.end != range.end || source.operationCount != range.end - range.begin ||
				!ledger.observeRangePlan(attempt, source)) return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
			executions[i].range = source; operations += source.operationCount;
			buffers[i].reset(new (std::nothrow) SimulationCommandBuffer(commands.get() + range.begin,
				range.end - range.begin, payloads.get() + range.begin * sizeof(ObjectStatusTimerPayload),
				(range.end - range.begin) * sizeof(ObjectStatusTimerPayload), i, OBJECT_STATUS_TIMER_MODULE_TYPE));
			if (!buffers[i]) return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
			producerSlots[i] = buffers[i].get();
		}
		if (operations != dispatch.operationCount) return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
	}
	bool complete = dispatch.rangeCount == planned;
	for (unsigned i = 0; i != dispatch.rangeCount; ++i)
	{
		ObjectStatusTimerExecutionRecord &record = executions[i];
		KernelPerformanceInlineBody body;
		const KernelPerformanceInlineAction action = ledger.beginInlineBody(attempt,
			record.range, KernelPerformanceLedger::instance(), body, record.checkpoint);
		if (action == KERNEL_INLINE_INVALID) return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
		if (action == KERNEL_INLINE_EXECUTE)
		{
			record.traceSource = true;
			ObjectStatusTimerJob job(snapshots, static_cast<unsigned>(record.range.begin),
				static_cast<unsigned>(record.range.end), frame, types, buffers[i].get(),
				&record, 0, 0, options.testHooks, i);
			job.executeInline();
		}
		KernelPerformanceRangeProgress progress = {};
		progress.checkpoint = record.checkpoint.snapshot();
		progress.publication = !progress.checkpoint.entered ? KERNEL_PUBLICATION_NOT_APPLICABLE :
			decision.reason == 3 || progress.checkpoint.terminal == KERNEL_RANGE_CANCELLED ?
			KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : sourceFinish.validationObserved ?
			KERNEL_PUBLICATION_PUBLISHED : KERNEL_PUBLICATION_REJECTED;
		if ((action == KERNEL_INLINE_EXECUTE && !ledger.finishInlineBody(body, progress)) ||
			!ledger.observeReleasedRange(attempt, record.range, progress)) return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
		complete = record.completed && complete;
	}
	bool published = false;
	{
		KernelPerformanceScope validate(&KernelPerformanceLedger::instance(), options.performanceBatch, KERNEL_PERFORMANCE_VALIDATE);
		unsigned preparedCount = 0;
		const bool validated = decision.reason != 3 && complete &&
			mergeStatusPreparedCommands(producerSlots, planned, commands.get(),
				merged.get(), mergeScratch.get(), count, prepared.get(),
				&preparedCount, options, metrics,
				sourceFinish.validationObserved);
		if (validated && sourceFinish.validationObserved)
		{
			observeStatusReferenceBatch(options, snapshots, count, frame, types,
				prepared.get(), preparedCount);
			if (options.performanceReferenceBatch != 0 &&
				options.performanceReferenceBatch->valid() &&
				sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED)
			{
				if (preparedCount != 0)
					memcpy(output, prepared.get(), preparedCount *
						sizeof(ObjectStatusTimerCommand));
				*outputCount = preparedCount;
				published = true;
			}
		}
	}
	for (unsigned i = 0; i != dispatch.rangeCount; ++i) buffers[i].reset();
	commands.reset(); payloads.reset(); merged.reset(); mergeScratch.reset();
	prepared.reset();
	for (unsigned i = 0; i != dispatch.rangeCount; ++i)
	{
		const KernelPerformanceRangePlan &range = executions[i].range;
		observeStatusTest(options.testHooks, OBJECT_STATUS_TIMER_TEST_RANGE_RELEASED, i,
			static_cast<unsigned>(range.begin), static_cast<unsigned>(range.end));
	}
	return published ? OBJECT_STATUS_TIMER_PARALLEL : OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
}
#endif

ObjectStatusTimerResult recordFallback(JobSystem *jobs,
	ObjectStatusTimerMetrics &metrics)
{
	++metrics.serialFallbacks;
	if (jobs != 0)
		jobs->recordSerialFallback();
	return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
}
}

ObjectStatusTimerOptions::ObjectStatusTimerOptions()
	: parallel(false), minimumGrain(OBJECT_STATUS_TIMER_DEFAULT_MINIMUM_GRAIN)
#if defined(_WIN64)
	, performanceBatch(), performanceReferenceLedger(0), performanceReferenceAttempt(), testHooks(0),
	performanceReferenceBatch(0), performanceReferenceOutput(0),
	performanceReferenceOutputCapacity(0)
#endif
{
}

ObjectStatusTimerMetrics::ObjectStatusTimerMetrics()
	: evaluatedSnapshots(0), emittedCommands(0), submittedJobs(0),
	  completedJobs(0), physicalWorkerJobs(0), ownerHelpedJobs(0),
	  physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  physicalWorkerMaskComplete(true), referenceAdmissionAccepted(false),
	  peakConcurrentPhysicalWorkers(0),
	  serialFallbacks(0)
{
}

ObjectStatusTimerRuntimeMetrics::ObjectStatusTimerRuntimeMetrics()
	: resetEpoch(0), authoritativeBatches(0), committedCommands(0),
	  submittedJobs(0), completedJobs(0), physicalWorkerJobs(0),
	  ownerHelpedJobs(0), physicalWorkerMask(0),
	  maximumDistinctPhysicalWorkers(0), physicalWorkerMaskComplete(true),
	  maximumPeakConcurrentPhysicalWorkers(0),
	  shadowExecutions(0), shadowCommands(0), shadowMatches(0),
	  shadowMismatches(0), ownerFallbacks(0), staleRejections(0)
{
}

ObjectStatusTimerResult PrepareObjectStatusTimerCommands(
	const ObjectStatusTimerSnapshot *snapshots, unsigned snapshotCount,
	unsigned currentFrame, unsigned disabledTypeCount,
	ObjectStatusTimerCommand *output, unsigned outputCapacity,
	const ObjectStatusTimerOptions &options, unsigned *outputCount,
	ObjectStatusTimerMetrics *metrics)
{
	ObjectStatusTimerMetrics localMetrics;
	if (metrics == 0)
		metrics = &localMetrics;
	*metrics = ObjectStatusTimerMetrics();

	if (outputCount == 0 || snapshotCount > OBJECT_STATUS_TIMER_MAXIMUM_SNAPSHOTS ||
		disabledTypeCount == 0 ||
		disabledTypeCount > OBJECT_STATUS_TIMER_MAX_TYPES ||
		(snapshotCount != 0 && (snapshots == 0 || output == 0)) ||
		outputCapacity < snapshotCount)
		return OBJECT_STATUS_TIMER_INVALID_INPUT;
	if (snapshotCount == 0)
	{
		*outputCount = 0;
		return OBJECT_STATUS_TIMER_SERIAL;
	}
	for (unsigned index = 0; index != snapshotCount; ++index)
	{
		if (snapshots[index].objectID == 0)
			return OBJECT_STATUS_TIMER_INVALID_INPUT;
	}

	JobSystem &jobs = JobSystem::instance();
#if defined(_WIN64)
	if (options.performanceReferenceLedger != 0 &&
		options.performanceReferenceLedger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
	{
		if (jobs.isWorkerThread() || !jobs.isRunning() || !jobs.isCurrentThread(JOB_OWNER_GAME))
			return OBJECT_STATUS_TIMER_SERIAL_FALLBACK;
		return consumeStatusCommands(snapshots, snapshotCount, currentFrame, disabledTypeCount,
			output, outputCount, options, *metrics);
	}
#endif
	bool useParallel = options.parallel &&
		snapshotCount >= OBJECT_STATUS_TIMER_MINIMUM_PARALLEL_SNAPSHOTS &&
		!jobs.isWorkerThread() && jobs.isRunning() &&
		jobs.isCurrentThread(JOB_OWNER_GAME) && jobs.workerCount() > 1;
	unsigned jobCount = 1;
	if (useParallel)
	{
		const unsigned minimumGrain = options.minimumGrain != 0 ?
			options.minimumGrain : OBJECT_STATUS_TIMER_DEFAULT_MINIMUM_GRAIN;
		jobCount = JobSystem::chooseRangeCount(snapshotCount, minimumGrain,
			jobs.workerCount());
		if (jobCount > OBJECT_STATUS_TIMER_MAXIMUM_JOBS)
			jobCount = OBJECT_STATUS_TIMER_MAXIMUM_JOBS;
		if (jobCount < 2)
		{
			// A one-range wave is an ordinary serial decision, not a failure.
			useParallel = false;
			jobCount = 1;
		}
	}

	SimulationCommand *commandStorage = new (std::nothrow)
		SimulationCommand[snapshotCount];
	unsigned char *payloadStorage = new (std::nothrow)
		unsigned char[snapshotCount * sizeof(ObjectStatusTimerPayload)];
	SimulationMergedCommand *merged = new (std::nothrow)
		SimulationMergedCommand[snapshotCount];
	SimulationMergedCommand *mergeScratch = new (std::nothrow)
		SimulationMergedCommand[snapshotCount];
	if (commandStorage == 0 || payloadStorage == 0 || merged == 0 ||
		mergeScratch == 0)
	{
		delete[] commandStorage;
		delete[] payloadStorage;
		delete[] merged;
		delete[] mergeScratch;
		return recordFallback(useParallel ? &jobs : 0, *metrics);
	}

	SimulationCommandBuffer *buffers[OBJECT_STATUS_TIMER_MAXIMUM_JOBS] = { 0 };
	const SimulationCommandBuffer *producerSlots[
		OBJECT_STATUS_TIMER_MAXIMUM_JOBS] = { 0 };
	ObjectStatusTimerExecutionRecord executions[
		OBJECT_STATUS_TIMER_MAXIMUM_JOBS];
	StatusJobAtomicUnsigned activePhysicalWorkers(0);
	StatusJobAtomicUnsigned peakPhysicalWorkers(0);
	unsigned createdBuffers = 0;
	unsigned submitted = 0;
	ObjectStatusTimerResult result = useParallel ?
		OBJECT_STATUS_TIMER_PARALLEL : OBJECT_STATUS_TIMER_SERIAL;
#if defined(_WIN64)
	performance::KernelPerformanceLedger *performanceLedger =
		options.performanceBatch.valid() ?
		&performance::KernelPerformanceLedger::instance() : 0;
	StatusSourceAttempt sourceAttempt(options, snapshots, snapshotCount,
		currentFrame, disabledTypeCount, useParallel, jobs, *metrics);
	bool sourceCancelled = false;
#endif
	{
#if defined(_WIN64)
		// Completion accounting, merge, and publication are the owner-side
		// validation boundary. Nested schedule/wait scopes settle this interval
		// so their time is not double-counted as validation work.
		performance::KernelPerformanceScope validateScope(performanceLedger,
			options.performanceBatch, performance::KERNEL_PERFORMANCE_VALIDATE);
#endif

	if (!useParallel)
	{
		JobRange range;
		if (!JobSystem::rangeForIndex(snapshotCount, 1, 0, range))
		{
			result = recordFallback(0, *metrics);
		}
		else
		{
			buffers[0] = new (std::nothrow) SimulationCommandBuffer(
				commandStorage, snapshotCount, payloadStorage,
				snapshotCount * sizeof(ObjectStatusTimerPayload), 0,
				OBJECT_STATUS_TIMER_MODULE_TYPE);
			if (buffers[0] == 0)
				result = recordFallback(0, *metrics);
			else
			{
				producerSlots[0] = buffers[0];
				createdBuffers = 1;
				if (!evaluateRange(snapshots, range.begin, range.end,
					currentFrame, disabledTypeCount, *buffers[0], 0) ||
					!buffers[0]->complete())
					result = recordFallback(0, *metrics);
			}
		}
	}
	else
	{
		JobGroup group;
		{
		#if defined(_WIN64)
			performance::KernelPerformanceScope scheduleScope(performanceLedger,
				options.performanceBatch, performance::KERNEL_PERFORMANCE_SCHEDULE);
		#endif
			try
			{
				group = jobs.createGroup();
			}
			catch (...)
			{
				result = recordFallback(&jobs, *metrics);
			}
			if (result == OBJECT_STATUS_TIMER_PARALLEL && !group.isValid())
				result = recordFallback(&jobs, *metrics);
			for (unsigned producer = 0;
				result == OBJECT_STATUS_TIMER_PARALLEL && producer != jobCount;
				++producer)
			{
				JobRange range;
				if (!JobSystem::rangeForIndex(snapshotCount, jobCount, producer,
					range))
				{
					result = recordFallback(&jobs, *metrics);
					break;
				}
				buffers[producer] = new (std::nothrow) SimulationCommandBuffer(
					commandStorage + range.begin, range.end - range.begin,
					payloadStorage + range.begin * sizeof(ObjectStatusTimerPayload),
					(range.end - range.begin) * sizeof(ObjectStatusTimerPayload),
					producer, OBJECT_STATUS_TIMER_MODULE_TYPE);
				if (buffers[producer] == 0)
				{
					result = recordFallback(&jobs, *metrics);
					break;
				}
				producerSlots[producer] = buffers[producer];
				++createdBuffers;
#if defined(_WIN64)
				sourceAttempt.plan(executions[producer], producer, range.begin, range.end);
#endif
				ObjectStatusTimerJob *job = new (std::nothrow)
					ObjectStatusTimerJob(snapshots, range.begin, range.end,
						currentFrame, disabledTypeCount, buffers[producer],
						executions + producer, &activePhysicalWorkers,
						&peakPhysicalWorkers
#if defined(_WIN64)
						, options.testHooks, producer
#endif
						);
				JobHandle handle = job != 0 ? jobs.trySubmit(job,
					JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
				if (!handle.isValid())
				{
					delete job;
					result = recordFallback(&jobs, *metrics);
					break;
				}
				++submitted;
				++metrics->submittedJobs;
			}
		}
		if (result != OBJECT_STATUS_TIMER_PARALLEL && group.isValid())
			jobs.cancel(group);
		bool physicalFenceCompleted = true;
		{
#if defined(_WIN64)
			performance::KernelPerformanceScope waitScope(performanceLedger,
				options.performanceBatch, performance::KERNEL_PERFORMANCE_WAIT);
#endif
			if (submitted != 0)
			{
			unsigned physicalCompletionTimeoutMilliseconds = 8;
#if defined(_WIN64)
			if (options.testHooks != 0 && options.testHooks->physicalWaitMilliseconds != 0)
				physicalCompletionTimeoutMilliseconds = options.testHooks->physicalWaitMilliseconds < 1000 ?
					options.testHooks->physicalWaitMilliseconds : 1000;
			if (options.testHooks != 0 && options.testHooks->beforeWait != 0)
				options.testHooks->beforeWait(options.testHooks->context);
#endif
			physicalFenceCompleted = jobs.waitWithoutOwnerHelp(group,
				physicalCompletionTimeoutMilliseconds);
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
				jobs.wait(group);
			}
		}
		}
		for (unsigned index = 0; index != submitted; ++index)
		{
			if (executions[index].completed)
			{
				++metrics->completedJobs;
				if (executions[index].physicalWorker)
				{
					++metrics->physicalWorkerJobs;
					const unsigned workerIndex =
						executions[index].physicalWorkerIndex;
					if (workerIndex < sizeof(ObjectStatusTimerMetricCounter) * 8)
						metrics->physicalWorkerMask |=
							static_cast<ObjectStatusTimerMetricCounter>(1) <<
							workerIndex;
					else
						metrics->physicalWorkerMaskComplete = false;
					bool firstWorker = true;
					for (unsigned previous = 0; previous != index; ++previous)
					{
						if (executions[previous].completed &&
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
				else if (executions[index].ownerHelped)
					++metrics->ownerHelpedJobs;
			}
		}
		metrics->peakConcurrentPhysicalWorkers =
			loadJobCounter(peakPhysicalWorkers);
#if defined(_WIN64)
		sourceCancelled = group.wasCancelled();
#endif
		if (result == OBJECT_STATUS_TIMER_PARALLEL &&
			(!physicalFenceCompleted || group.failed() || group.wasCancelled() ||
			 submitted != jobCount ||
			 metrics->completedJobs != metrics->submittedJobs ||
			 metrics->physicalWorkerJobs != metrics->completedJobs ||
			 metrics->ownerHelpedJobs != 0))
				result = recordFallback(&jobs, *metrics);
	}

	if ((result == OBJECT_STATUS_TIMER_SERIAL || result == OBJECT_STATUS_TIMER_PARALLEL) &&
		!mergeStatusPreparedCommands(producerSlots, jobCount, commandStorage, merged,
			mergeScratch, snapshotCount, output, outputCount, options, *metrics, true))
		result = recordFallback(useParallel ? &jobs : 0, *metrics);
#if defined(_WIN64)
	sourceAttempt.release(executions, submitted, result == OBJECT_STATUS_TIMER_PARALLEL, sourceCancelled);
	if (result == OBJECT_STATUS_TIMER_PARALLEL)
		observeStatusReferenceBatch(options, snapshots, snapshotCount,
			currentFrame, disabledTypeCount, output, *outputCount);
#endif
	}

	deleteBuffers(buffers, createdBuffers);
	delete[] commandStorage;
	delete[] payloadStorage;
	delete[] merged;
	delete[] mergeScratch;
#if defined(_WIN64)
	if (options.testHooks != 0)
		for (unsigned released = 0; released != createdBuffers; ++released)
		{
			JobRange range;
			JobSystem::rangeForIndex(snapshotCount, jobCount, released, range);
			observeStatusTest(options.testHooks, OBJECT_STATUS_TIMER_TEST_RANGE_RELEASED,
				released, range.begin, range.end);
		}
#endif
	return result;
}

bool ObjectStatusTimerCommandsEqual(const ObjectStatusTimerCommand *left,
	unsigned leftCount, const ObjectStatusTimerCommand *right,
	unsigned rightCount, unsigned *firstDifference)
{
	if (firstDifference != 0)
		*firstDifference = 0;
	if ((leftCount != 0 && left == 0) || (rightCount != 0 && right == 0))
		return false;
	const unsigned commonCount = leftCount < rightCount ? leftCount : rightCount;
	for (unsigned index = 0; index != commonCount; ++index)
	{
		if (left[index].objectID != right[index].objectID ||
			left[index].ownerOrder != right[index].ownerOrder ||
			left[index].expiredMask != right[index].expiredMask)
		{
			if (firstDifference != 0)
				*firstDifference = index;
			return false;
		}
	}
	if (leftCount != rightCount)
	{
		if (firstDifference != 0)
			*firstDifference = commonCount;
		return false;
	}
	return true;
}

void ResetObjectStatusTimerRuntimeMetrics()
{
	addMetric(s_resetEpoch, 1);
	resetMetric(s_authoritativeBatches);
	resetMetric(s_committedCommands);
	resetMetric(s_submittedJobs);
	resetMetric(s_completedJobs);
	resetMetric(s_physicalWorkerJobs);
	resetMetric(s_ownerHelpedJobs);
	resetMetric(s_physicalWorkerMask);
	resetMetric(s_maximumDistinctPhysicalWorkers);
	resetMetric(s_physicalWorkerMaskIncomplete);
	resetMetric(s_maximumPeakConcurrentPhysicalWorkers);
	resetMetric(s_shadowExecutions);
	resetMetric(s_shadowCommands);
	resetMetric(s_shadowMatches);
	resetMetric(s_shadowMismatches);
	resetMetric(s_ownerFallbacks);
	resetMetric(s_staleRejections);
}

ObjectStatusTimerRuntimeMetrics GetObjectStatusTimerRuntimeMetrics()
{
	ObjectStatusTimerRuntimeMetrics metrics;
	metrics.resetEpoch = loadMetric(s_resetEpoch);
	metrics.authoritativeBatches = loadMetric(s_authoritativeBatches);
	metrics.committedCommands = loadMetric(s_committedCommands);
	metrics.submittedJobs = loadMetric(s_submittedJobs);
	metrics.completedJobs = loadMetric(s_completedJobs);
	metrics.physicalWorkerJobs = loadMetric(s_physicalWorkerJobs);
	metrics.ownerHelpedJobs = loadMetric(s_ownerHelpedJobs);
	metrics.physicalWorkerMask = loadMetric(s_physicalWorkerMask);
	metrics.maximumDistinctPhysicalWorkers = static_cast<unsigned>(
		loadMetric(s_maximumDistinctPhysicalWorkers));
	metrics.physicalWorkerMaskComplete =
		loadMetric(s_physicalWorkerMaskIncomplete) == 0;
	metrics.maximumPeakConcurrentPhysicalWorkers = static_cast<unsigned>(
		loadMetric(s_maximumPeakConcurrentPhysicalWorkers));
	metrics.shadowExecutions = loadMetric(s_shadowExecutions);
	metrics.shadowCommands = loadMetric(s_shadowCommands);
	metrics.shadowMatches = loadMetric(s_shadowMatches);
	metrics.shadowMismatches = loadMetric(s_shadowMismatches);
	metrics.ownerFallbacks = loadMetric(s_ownerFallbacks);
	metrics.staleRejections = loadMetric(s_staleRejections);
	return metrics;
}

void RecordObjectStatusTimerAuthoritativeCommit(unsigned preparedCommandCount,
	unsigned committedCommandCount, const ObjectStatusTimerMetrics &sliceMetrics)
{
	if (preparedCommandCount == 0)
		return;
	const bool stale = preparedCommandCount != committedCommandCount;
	const bool qualifying = !stale &&
		sliceMetrics.submittedJobs >= 2 &&
		sliceMetrics.completedJobs == sliceMetrics.submittedJobs &&
		sliceMetrics.physicalWorkerJobs == sliceMetrics.completedJobs &&
		sliceMetrics.ownerHelpedJobs == 0 &&
		sliceMetrics.distinctPhysicalWorkers > 1 &&
		sliceMetrics.peakConcurrentPhysicalWorkers > 1;
	if (!qualifying)
	{
		addMetric(s_ownerFallbacks, 1);
		if (stale)
			addMetric(s_staleRejections, 1);
		return;
	}
	addMetric(s_authoritativeBatches, 1);
	addMetric(s_committedCommands, committedCommandCount);
	addMetric(s_submittedJobs, sliceMetrics.submittedJobs);
	addMetric(s_completedJobs, sliceMetrics.completedJobs);
	addMetric(s_physicalWorkerJobs, sliceMetrics.physicalWorkerJobs);
	addMetric(s_ownerHelpedJobs, sliceMetrics.ownerHelpedJobs);
	orMetric(s_physicalWorkerMask, sliceMetrics.physicalWorkerMask);
	if (!sliceMetrics.physicalWorkerMaskComplete)
		addMetric(s_physicalWorkerMaskIncomplete, 1);
	maximizeMetric(s_maximumDistinctPhysicalWorkers,
		sliceMetrics.distinctPhysicalWorkers);
	maximizeMetric(s_maximumPeakConcurrentPhysicalWorkers,
		sliceMetrics.peakConcurrentPhysicalWorkers);
}

void RecordObjectStatusTimerShadow(bool matched, unsigned commandCount,
	const ObjectStatusTimerMetrics &sliceMetrics)
{
	addMetric(s_shadowExecutions, 1);
	addMetric(s_shadowCommands, commandCount);
	addMetric(matched ? s_shadowMatches : s_shadowMismatches, 1);
	// Shadow work never proves live authority, but preserve unexpected owner-help
	// as a fail-closed diagnostic in the same evidence surface.
	if (sliceMetrics.ownerHelpedJobs != 0)
		addMetric(s_ownerFallbacks, sliceMetrics.ownerHelpedJobs);
}

void RecordObjectStatusTimerOwnerFallback(bool stale)
{
	addMetric(s_ownerFallbacks, 1);
	if (stale)
		addMetric(s_staleRejections, 1);
}
}
