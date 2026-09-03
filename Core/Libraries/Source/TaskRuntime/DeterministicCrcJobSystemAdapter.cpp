/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicCrcJobSystemAdapter.h"

#include <new>
#include <string.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <chrono>
#include <thread>
#endif

namespace rts
{
namespace
{

void InitializeDecision(DeterministicCrcRuntimeDecision *decision,
	DeterministicCrcLane lane, uint32_t generation,
	uint32_t serialOracleChecksum)
{
	memset(decision, 0, sizeof(*decision));
	decision->status = DETERMINISTIC_CRC_RUNTIME_FINALIZE_NOT_PREPARED;
	decision->foldStatus = DETERMINISTIC_CRC_FOLD_OK;
	decision->generation = generation;
	decision->serialOracleChecksum = serialOracleChecksum;
	decision->selectedChecksum = serialOracleChecksum;
	decision->rejectedPartition =
		DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION;
	decision->folded.lane = lane;
	decision->folded.generation = generation;
	decision->folded.physicalWorkerMaskComplete = true;
}

bool ValidLane(DeterministicCrcLane lane)
{
	return lane == DETERMINISTIC_CRC_LEGACY_XFER ||
		lane == DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH;
}

void FinalizePreparedFallback(DeterministicCrcRuntimeAdapter *runtime,
	DeterministicCrcLane lane, uint32_t serialOracleChecksum,
	DeterministicCrcJobSystemResult *result)
{
	result->finalizeStatus = runtime->finalize(lane,
		serialOracleChecksum, &result->decision);
	result->decision.parallelVerified = false;
	result->decision.selectedChecksum = serialOracleChecksum;
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
class DeterministicCrcPartitionJob : public Job
{
public:
	DeterministicCrcPartitionJob(DeterministicCrcRuntimeAdapter *runtime,
		size_t partitionIndex, DeterministicCrcRuntimeWorkStatus *workStatus)
		: m_runtime(runtime), m_partitionIndex(partitionIndex),
		  m_workStatus(workStatus) {}

	virtual void execute(JobContext &context)
	{
		if (context.isCancellationRequested())
		{
			*m_workStatus = DETERMINISTIC_CRC_RUNTIME_WORK_CANCELLED;
			context.fail();
			return;
		}
		const unsigned workerIndex = context.physicalWorkerIndex();
		*m_workStatus = m_runtime->executePartition(m_partitionIndex,
			context.isPhysicalWorkerExecution() ?
				static_cast<uint32_t>(workerIndex) :
				DETERMINISTIC_CRC_RUNTIME_INVALID_PHYSICAL_WORKER_ID);
		if (*m_workStatus != DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED ||
			context.isCancellationRequested())
		{
			context.fail();
		}
	}

private:
	DeterministicCrcRuntimeAdapter *m_runtime;
	size_t m_partitionIndex;
	DeterministicCrcRuntimeWorkStatus *m_workStatus;
};
#endif

} // namespace

DeterministicCrcJobSystemOptions::DeterministicCrcJobSystemOptions()
	: priority(JOB_PRIORITY_FRAME_CRITICAL),
	  physicalWaitMilliseconds(
		DETERMINISTIC_CRC_JOB_SYSTEM_DEFAULT_WAIT_MILLISECONDS)
{
}

DeterministicCrcJobSystemResult::DeterministicCrcJobSystemResult()
	: status(DETERMINISTIC_CRC_JOB_SYSTEM_INVALID_ARGUMENT),
	  prepareStatus(DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_ARGUMENT),
	  finalizeStatus(DETERMINISTIC_CRC_RUNTIME_FINALIZE_NOT_PREPARED),
	  failedPartition(DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION),
	  timedOut(false)
{
	InitializeDecision(&decision, DETERMINISTIC_CRC_LEGACY_XFER, 0U, 0U);
}

DeterministicCrcJobSystemStatus VerifyDeterministicCrcWithJobSystem(
	DeterministicCrcRuntimeAdapter *runtime,
	const DeterministicCrcRangeKey *fixedPlan,
	const DeterministicCrcRuntimePartitionInput *partitionInputs,
	DeterministicCrcPartitionResult *partitionResults,
	size_t partitionCount,
	uint32_t generation,
	uint32_t capturedLanes,
	const DeterministicCrcCaptureControl *control,
	DeterministicCrcLane lane,
	uint32_t serialOracleChecksum,
	const DeterministicCrcJobSystemOptions &options,
	DeterministicCrcJobSystemResult *result)
{
	if (result == 0) return DETERMINISTIC_CRC_JOB_SYSTEM_INVALID_ARGUMENT;
	*result = DeterministicCrcJobSystemResult();
	InitializeDecision(&result->decision, lane, generation,
		serialOracleChecksum);
	if (runtime == 0 || fixedPlan == 0 || partitionInputs == 0 ||
		partitionResults == 0 || partitionCount == 0U ||
		partitionCount > DETERMINISTIC_CRC_JOB_SYSTEM_MAX_PARTITIONS ||
		generation == 0U || !ValidLane(lane) ||
		options.priority < JOB_PRIORITY_FRAME_CRITICAL ||
		options.priority >= JOB_PRIORITY_COUNT)
	{
		return result->status;
	}

#if defined(_MSC_VER) && _MSC_VER < 1300
	result->status = DETERMINISTIC_CRC_JOB_SYSTEM_UNAVAILABLE;
	return result->status;
#else
	JobSystem &jobs = JobSystem::instance();
	if (!jobs.isRunning() || jobs.workerCount() == 0U ||
		jobs.isWorkerThread())
	{
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_UNAVAILABLE;
		return result->status;
	}

	result->prepareStatus = runtime->prepare(fixedPlan, partitionInputs,
		partitionResults, partitionCount, generation, capturedLanes, control,
		&result->failedPartition);
	if (result->prepareStatus != DETERMINISTIC_CRC_RUNTIME_PREPARED)
	{
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_PREPARE_FALLBACK;
		return result->status;
	}

	JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_GROUP_FALLBACK;
		FinalizePreparedFallback(runtime, lane, serialOracleChecksum, result);
		return result->status;
	}

	JobSubmission submissions[DETERMINISTIC_CRC_JOB_SYSTEM_MAX_PARTITIONS];
	JobHandle handles[DETERMINISTIC_CRC_JOB_SYSTEM_MAX_PARTITIONS];
	DeterministicCrcRuntimeWorkStatus workStatuses[
		DETERMINISTIC_CRC_JOB_SYSTEM_MAX_PARTITIONS];
	size_t allocated = 0U;
	for (; allocated < partitionCount; ++allocated)
	{
		workStatuses[allocated] = DETERMINISTIC_CRC_RUNTIME_WORK_NOT_PREPARED;
		submissions[allocated].job = new (std::nothrow)
			DeterministicCrcPartitionJob(runtime, allocated,
				workStatuses + allocated);
		if (submissions[allocated].job == 0) break;
		submissions[allocated].priority = options.priority;
	}
	if (allocated != partitionCount)
	{
		for (size_t index = 0U; index < allocated; ++index)
			delete submissions[index].job;
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_ALLOCATION_FALLBACK;
		result->failedPartition = allocated;
		FinalizePreparedFallback(runtime, lane, serialOracleChecksum, result);
		return result->status;
	}

	if (!jobs.trySubmitBatch(submissions,
		static_cast<unsigned>(partitionCount), group, handles))
	{
		for (size_t index = 0U; index < partitionCount; ++index)
			delete submissions[index].job;
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_ADMISSION_FALLBACK;
		FinalizePreparedFallback(runtime, lane, serialOracleChecksum, result);
		return result->status;
	}

	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() +
		std::chrono::milliseconds(options.physicalWaitMilliseconds);
	while (!group.isComplete() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	if (!group.isComplete())
	{
		result->timedOut = true;
		jobs.cancel(group);
	}
	// A completed group cannot trigger owner help. After timeout, cancellation
	// makes queued jobs terminal before this owner join can claim them.
	jobs.wait(group);

	bool allWorkSucceeded = !result->timedOut && !group.failed() &&
		!group.wasCancelled();
	for (size_t index = 0U; index < partitionCount; ++index)
	{
		if (!handles[index].succeeded() ||
			workStatuses[index] != DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED)
		{
			if (result->failedPartition ==
				DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION)
			{
				result->failedPartition = index;
			}
			allWorkSucceeded = false;
		}
	}

	result->finalizeStatus = runtime->finalize(lane, serialOracleChecksum,
		&result->decision);
	if (result->timedOut)
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_TIMEOUT_FALLBACK;
	else if (!allWorkSucceeded)
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_WORK_FALLBACK;
	else if (result->finalizeStatus !=
		DETERMINISTIC_CRC_RUNTIME_FINALIZE_VERIFIED)
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_FINALIZE_FALLBACK;
	else
		result->status = DETERMINISTIC_CRC_JOB_SYSTEM_VERIFIED;

	if (result->status != DETERMINISTIC_CRC_JOB_SYSTEM_VERIFIED)
	{
		result->decision.parallelVerified = false;
		result->decision.selectedChecksum = serialOracleChecksum;
	}
	return result->status;
#endif
}

} // namespace rts
