/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectStatusTimerKernel.h"

#include "Lib/SimulationCommandBuffer.h"

#include <new>

namespace rts
{
namespace
{
enum
{
	OBJECT_STATUS_TIMER_MODULE_TYPE = 0x5354,
	OBJECT_STATUS_TIMER_CLEAR_EXPIRED = 1
};

struct ObjectStatusTimerPayload
{
	unsigned expiredMask;
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
		bool *completed)
		: m_snapshots(snapshots), m_begin(begin), m_end(end),
		  m_currentFrame(currentFrame), m_disabledTypeCount(disabledTypeCount),
		  m_buffer(buffer), m_completed(completed)
	{
	}

	virtual void execute(JobContext &context)
	{
		if (context.isCancellationRequested() ||
			!evaluateRange(m_snapshots, m_begin, m_end, m_currentFrame,
				m_disabledTypeCount, *m_buffer, &context) ||
			context.isCancellationRequested() || !m_buffer->complete())
		{
			m_buffer->fail();
			return;
		}
		*m_completed = true;
	}

private:
	const ObjectStatusTimerSnapshot *m_snapshots;
	unsigned m_begin;
	unsigned m_end;
	unsigned m_currentFrame;
	unsigned m_disabledTypeCount;
	SimulationCommandBuffer *m_buffer;
	bool *m_completed;
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
	  completedJobs(0), serialFallbacks(0)
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
	bool completed[OBJECT_STATUS_TIMER_MAXIMUM_JOBS] = { false };
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
					completed + producer);
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
		if (submitted != 0)
			jobs.wait(group);
		for (unsigned index = 0; index != submitted; ++index)
		{
			if (completed[index])
				++metrics->completedJobs;
		}
		if (result == OBJECT_STATUS_TIMER_PARALLEL &&
			(group.failed() || group.wasCancelled() || submitted != jobCount ||
			 metrics->completedJobs != metrics->submittedJobs))
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
}
