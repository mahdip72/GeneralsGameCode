/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectStatusTimerKernel.h"

#include "Lib/SimulationCommandBuffer.h"

#include <new>
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
		  physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX) {}
	bool completed;
	bool physicalWorker;
	bool ownerHelped;
	unsigned physicalWorkerIndex;
};

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

bool evaluateRange(const ObjectStatusTimerSnapshot *snapshots,
	unsigned begin, unsigned end, unsigned currentFrame,
	unsigned disabledTypeCount, SimulationCommandBuffer &buffer,
	JobContext *context)
{
	for (unsigned index = begin; index != end; ++index)
	{
		if (context != 0 && (index - begin) % 64 == 0 &&
			context->isCancellationRequested())
			return false;

		const ObjectStatusTimerSnapshot &snapshot = snapshots[index];
		unsigned expiredMask = 0;
		for (unsigned type = 0; type != disabledTypeCount; ++type)
		{
			const unsigned bit = 1u << type;
			if ((snapshot.activeMask & bit) != 0 &&
				currentFrame >= snapshot.expirationFrame[type])
				expiredMask |= bit;
		}
		if (expiredMask == 0)
			continue;

		ObjectStatusTimerPayload payload;
		payload.expiredMask = expiredMask;
		if (!buffer.append(snapshot.ownerOrder,
			SimulationStableHandle(snapshot.objectID), SimulationStableHandle(),
			OBJECT_STATUS_TIMER_CLEAR_EXPIRED, &payload, sizeof(payload)))
			return false;
	}
	return true;
}

class ObjectStatusTimerJob : public Job
{
public:
	ObjectStatusTimerJob(const ObjectStatusTimerSnapshot *snapshots,
		unsigned begin, unsigned end, unsigned currentFrame,
		unsigned disabledTypeCount, SimulationCommandBuffer *buffer,
		ObjectStatusTimerExecutionRecord *execution,
		StatusJobAtomicUnsigned *activePhysicalWorkers,
		StatusJobAtomicUnsigned *peakPhysicalWorkers)
		: m_snapshots(snapshots), m_begin(begin), m_end(end),
		  m_currentFrame(currentFrame), m_disabledTypeCount(disabledTypeCount),
		  m_buffer(buffer), m_execution(execution),
		  m_activePhysicalWorkers(activePhysicalWorkers),
		  m_peakPhysicalWorkers(peakPhysicalWorkers)
	{
	}

	virtual void execute(JobContext &context)
	{
		m_execution->physicalWorker = context.isPhysicalWorkerExecution();
		m_execution->ownerHelped = !m_execution->physicalWorker;
		if (m_execution->physicalWorker)
			m_execution->physicalWorkerIndex = context.physicalWorkerIndex();
		ObjectStatusPhysicalExecutionScope physicalScope(
			m_execution->physicalWorker, m_activePhysicalWorkers,
			m_peakPhysicalWorkers);
		if (context.isCancellationRequested() ||
			!evaluateRange(m_snapshots, m_begin, m_end, m_currentFrame,
				m_disabledTypeCount, *m_buffer, &context) ||
			context.isCancellationRequested() || !m_buffer->complete())
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
};

void deleteBuffers(SimulationCommandBuffer **buffers, unsigned count)
{
	for (unsigned index = 0; index != count; ++index)
		delete buffers[index];
}

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
{
}

ObjectStatusTimerMetrics::ObjectStatusTimerMetrics()
	: evaluatedSnapshots(0), emittedCommands(0), submittedJobs(0),
	  completedJobs(0), physicalWorkerJobs(0), ownerHelpedJobs(0),
	  physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  physicalWorkerMaskComplete(true), peakConcurrentPhysicalWorkers(0),
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
		JobGroup group = jobs.createGroup();
		if (!group.isValid())
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
			ObjectStatusTimerJob *job = new (std::nothrow)
				ObjectStatusTimerJob(snapshots, range.begin, range.end,
					currentFrame, disabledTypeCount, buffers[producer],
					executions + producer, &activePhysicalWorkers,
					&peakPhysicalWorkers);
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
		if (result != OBJECT_STATUS_TIMER_PARALLEL && group.isValid())
			jobs.cancel(group);
		bool physicalFenceCompleted = true;
		if (submitted != 0)
		{
			const unsigned physicalCompletionTimeoutMilliseconds = 8;
			physicalFenceCompleted = jobs.waitWithoutOwnerHelp(group,
				physicalCompletionTimeoutMilliseconds);
			if (!physicalFenceCompleted)
			{
				jobs.cancel(group);
				jobs.wait(group);
			}
			else
			{
				jobs.wait(group);
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
		if (result == OBJECT_STATUS_TIMER_PARALLEL &&
			(!physicalFenceCompleted || group.failed() || group.wasCancelled() ||
			 submitted != jobCount ||
			 metrics->completedJobs != metrics->submittedJobs ||
			 metrics->physicalWorkerJobs != metrics->completedJobs ||
			 metrics->ownerHelpedJobs != 0))
			result = recordFallback(&jobs, *metrics);
	}

	if (result == OBJECT_STATUS_TIMER_SERIAL ||
		result == OBJECT_STATUS_TIMER_PARALLEL)
	{
		SimulationCommandMergeResult mergeResult = MergeSimulationCommandSlots(
			producerSlots, jobCount, merged, mergeScratch, snapshotCount);
		if (!mergeResult.succeeded())
			result = recordFallback(useParallel ? &jobs : 0, *metrics);
		else
		{
			bool valid = true;
			unsigned previousOwnerOrder = 0;
			for (unsigned index = 0; index != mergeResult.commandCount; ++index)
			{
				const SimulationCommand *command = merged[index].command();
				const ObjectStatusTimerPayload *payload =
					reinterpret_cast<const ObjectStatusTimerPayload *>(
						merged[index].payload());
				if (command == 0 || payload == 0 ||
					command->commandType() != OBJECT_STATUS_TIMER_CLEAR_EXPIRED ||
					command->payloadSize() != sizeof(ObjectStatusTimerPayload) ||
					command->orderKey().moduleType() !=
						OBJECT_STATUS_TIMER_MODULE_TYPE ||
					command->orderKey().target().isNull() ||
					!command->orderKey().source().isNull() ||
					(index != 0 && command->orderKey().phase() <=
						previousOwnerOrder))
				{
					valid = false;
					break;
				}
				previousOwnerOrder = command->orderKey().phase();
			}
			if (!valid)
				result = recordFallback(useParallel ? &jobs : 0, *metrics);
			else
			{
				for (unsigned index = 0; index != mergeResult.commandCount; ++index)
				{
					const SimulationCommand *command = merged[index].command();
					const ObjectStatusTimerPayload *payload =
						reinterpret_cast<const ObjectStatusTimerPayload *>(
							merged[index].payload());
					output[index].objectID =
						command->orderKey().target().objectID();
					output[index].ownerOrder = command->orderKey().phase();
					output[index].expiredMask = payload->expiredMask;
				}
				metrics->evaluatedSnapshots = snapshotCount;
				metrics->emittedCommands = mergeResult.commandCount;
				*outputCount = mergeResult.commandCount;
			}
		}
	}

	deleteBuffers(buffers, createdBuffers);
	delete[] commandStorage;
	delete[] payloadStorage;
	delete[] merged;
	delete[] mergeScratch;
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
