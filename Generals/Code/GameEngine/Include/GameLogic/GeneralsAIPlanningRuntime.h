/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "GameLogic/GeneralsAIPlanningPolicy.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"

#include <atomic>
#include <new>

class GeneralsAIEnemyPlanningJob : public rts::Job
{
public:
	struct ExecutionRecord
	{
		ExecutionRecord() : completed(false), physicalWorker(false),
			physicalWorkerIndex(rts::JOB_INVALID_PHYSICAL_WORKER_INDEX),
			ownerHelped(false) {}

		Bool completed;
		Bool physicalWorker;
		UnsignedInt physicalWorkerIndex;
		Bool ownerHelped;
	};

	GeneralsAIEnemyPlanningJob(
		const GeneralsAIEnemyPlanningSnapshot *snapshot,
		GeneralsAIEnemyPlanningResult *result, ExecutionRecord *execution,
		UnsignedInt jobOrdinal, UnsignedInt injectedFailureOrdinal,
		std::atomic<UnsignedInt> *activePhysicalWorkers,
		std::atomic<UnsignedInt> *peakPhysicalWorkers,
		const rts::JobFloatingPointState &floatingPointState) :
		m_snapshot(snapshot), m_result(result), m_execution(execution),
		m_jobOrdinal(jobOrdinal),
		m_injectedFailureOrdinal(injectedFailureOrdinal),
		m_activePhysicalWorkers(activePhysicalWorkers),
		m_peakPhysicalWorkers(peakPhysicalWorkers),
		m_floatingPointState(floatingPointState) {}

	void execute(rts::JobContext &context) override
	{
		const rts::JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_execution->physicalWorker = context.isPhysicalWorkerExecution();
		m_execution->ownerHelped = !m_execution->physicalWorker;
		if (m_execution->physicalWorker)
		{
			m_execution->physicalWorkerIndex = context.physicalWorkerIndex();
			const UnsignedInt active = m_activePhysicalWorkers->fetch_add(1U,
				std::memory_order_acq_rel) + 1U;
			UnsignedInt observed = m_peakPhysicalWorkers->load(
				std::memory_order_relaxed);
			while (observed < active &&
				!m_peakPhysicalWorkers->compare_exchange_weak(observed, active,
					std::memory_order_relaxed, std::memory_order_relaxed))
			{
			}
		}

		const Bool planned = !context.isCancellationRequested() &&
			m_jobOrdinal != m_injectedFailureOrdinal &&
			PlanGeneralsAIEnemyTarget(*m_snapshot, m_result);
		if (m_execution->physicalWorker)
			m_activePhysicalWorkers->fetch_sub(1U, std::memory_order_acq_rel);
		if (!planned)
		{
			context.fail();
			return;
		}
		m_execution->completed = true;
	}

private:
	const GeneralsAIEnemyPlanningSnapshot *m_snapshot;
	GeneralsAIEnemyPlanningResult *m_result;
	ExecutionRecord *m_execution;
	UnsignedInt m_jobOrdinal;
	UnsignedInt m_injectedFailureOrdinal;
	std::atomic<UnsignedInt> *m_activePhysicalWorkers;
	std::atomic<UnsignedInt> *m_peakPhysicalWorkers;
	const rts::JobFloatingPointState m_floatingPointState;
};

struct GeneralsAIPlanningJobSystemEvidence
{
	GeneralsAIPlanningJobSystemEvidence() : physicalWorkerMask(0U),
		distinctPhysicalWorkers(0U), peakConcurrentPhysicalWorkers(0U),
		physicalWorkerExecutions(0U), ownerHelpedJobs(0U) {}

	void collect(const GeneralsAIEnemyPlanningJob::ExecutionRecord *execution,
		UnsignedInt executionCount, UnsignedInt peak)
	{
		physicalWorkerMask = 0U;
		distinctPhysicalWorkers = 0U;
		peakConcurrentPhysicalWorkers = peak;
		physicalWorkerExecutions = 0U;
		ownerHelpedJobs = 0U;
		for (UnsignedInt index = 0U; index < executionCount; ++index)
		{
			if (execution[index].ownerHelped)
				++ownerHelpedJobs;
			if (!execution[index].physicalWorker)
				continue;
			++physicalWorkerExecutions;
			const UnsignedInt workerIndex = execution[index].physicalWorkerIndex;
			if (workerIndex < 64U)
				physicalWorkerMask |= (static_cast<rts::JobMetricCounter>(1U) <<
					workerIndex);
			Bool firstExecutionOnWorker = true;
			for (UnsignedInt previous = 0U; previous < index; ++previous)
			{
				if (execution[previous].physicalWorker &&
					execution[previous].physicalWorkerIndex == workerIndex)
				{
					firstExecutionOnWorker = false;
					break;
				}
			}
			if (firstExecutionOnWorker)
				++distinctPhysicalWorkers;
		}
	}

	rts::JobMetricCounter physicalWorkerMask;
	UnsignedInt distinctPhysicalWorkers;
	UnsignedInt peakConcurrentPhysicalWorkers;
	UnsignedInt physicalWorkerExecutions;
	UnsignedInt ownerHelpedJobs;
};

inline void InitializeGeneralsAIPlanningBatchStatus(
	rts::AIPlanningBatchStatus *status,
	rts::AIPlanningExecutionMode executionMode)
{
	if (!status)
		return;
	status->requestedMode = static_cast<UnsignedInt>(executionMode);
	status->committedMode = rts::AI_PLANNING_EXECUTION_SERIAL;
	status->parallelSucceeded = 0U;
	status->shadowMatched = 0U;
	status->usedSerialFallback = 0U;
	status->mismatchPlayerOrdinal = rts::AI_PLANNING_INVALID_ORDINAL;
	status->physicalWorkerMask = 0U;
	status->distinctPhysicalWorkers = 0U;
	status->peakConcurrentPhysicalWorkers = 0U;
	status->ownerHelpedJobs = 0U;
}

inline void PublishGeneralsAIPlanningJobSystemEvidence(
	rts::AIPlanningBatchStatus *status,
	const GeneralsAIPlanningJobSystemEvidence &evidence)
{
	if (!status)
		return;
	status->physicalWorkerMask = evidence.physicalWorkerMask;
	status->distinctPhysicalWorkers = evidence.distinctPhysicalWorkers;
	status->peakConcurrentPhysicalWorkers =
		evidence.peakConcurrentPhysicalWorkers;
	status->ownerHelpedJobs = evidence.ownerHelpedJobs;
}

inline Bool ValidateGeneralsAIEnemyPlanningBatchInputs(
	const GeneralsAIEnemyPlanningSnapshot *snapshots, UnsignedInt snapshotCount)
{
	if (!snapshots || snapshotCount == 0U ||
		snapshotCount > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS)
	{
		return false;
	}
	for (UnsignedInt i = 0U; i < snapshotCount; ++i)
	{
		if (!ValidateGeneralsAIEnemyPlanningSnapshot(snapshots[i]))
			return false;
		for (UnsignedInt prior = 0U; prior < i; ++prior)
		{
			if (snapshots[i].ownerPlayerIndex == snapshots[prior].ownerPlayerIndex)
				return false;
		}
	}
	return true;
}

inline Bool PlanGeneralsAIEnemyPlanningBatchSerial(
	const GeneralsAIEnemyPlanningSnapshot *snapshots, UnsignedInt snapshotCount,
	GeneralsAIEnemyPlanningResult *results)
{
	if (!results ||
		!ValidateGeneralsAIEnemyPlanningBatchInputs(snapshots, snapshotCount))
	{
		return false;
	}
	GeneralsAIEnemyPlanningResult serial[
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
	for (UnsignedInt i = 0U; i < snapshotCount; ++i)
	{
		if (!PlanGeneralsAIEnemyTarget(snapshots[i], &serial[i]) ||
			!ValidateGeneralsAIEnemyPlanningResult(snapshots[i], serial[i]))
		{
			return false;
		}
	}
	for (UnsignedInt i = 0U; i < snapshotCount; ++i)
		results[i] = serial[i];
	return true;
}

// Production executor shared by the game and focused test. Once the current
// planning epoch has captured snapshots, every unavailable or failed parallel
// path recomputes that same complete batch serially. It never switches epochs.
inline Bool ExecuteGeneralsAIEnemyPlanningBatch(
	rts::AIPlanningExecutionMode executionMode, Bool isNetworkGame,
	const GeneralsAIEnemyPlanningSnapshot *snapshots, UnsignedInt snapshotCount,
	GeneralsAIEnemyPlanningResult *results,
	UnsignedInt injectedFailureOrdinal = rts::AI_PLANNING_INVALID_ORDINAL,
	rts::AIPlanningBatchStatus *status = 0)
{
	InitializeGeneralsAIPlanningBatchStatus(status, executionMode);
	// A negotiated network serial lane is still the current canonical owner
	// oracle; only a network request for worker execution is rejected here.
	if ((isNetworkGame && executionMode != rts::AI_PLANNING_EXECUTION_SERIAL) ||
		!results ||
		(executionMode != rts::AI_PLANNING_EXECUTION_SERIAL &&
			executionMode != rts::AI_PLANNING_EXECUTION_PARALLEL &&
			executionMode != rts::AI_PLANNING_EXECUTION_SHADOW) ||
		!ValidateGeneralsAIEnemyPlanningBatchInputs(snapshots, snapshotCount))
	{
		return false;
	}

	rts::JobSystem &jobs = rts::JobSystem::instance();
	if (executionMode == rts::AI_PLANNING_EXECUTION_SERIAL ||
		snapshotCount < 2U || !jobs.isRunning() || jobs.workerCount() < 2U)
	{
		if (executionMode != rts::AI_PLANNING_EXECUTION_SERIAL)
			jobs.recordSerialFallback();
		if (status && executionMode != rts::AI_PLANNING_EXECUTION_SERIAL)
			status->usedSerialFallback = 1U;
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
	if (jobs.isWorkerThread() ||
		!jobs.isCurrentThread(rts::JOB_OWNER_GAME))
		return false;

	GeneralsAIEnemyPlanningJob::ExecutionRecord execution[
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
	std::atomic<UnsignedInt> activePhysicalWorkers(0U);
	std::atomic<UnsignedInt> peakPhysicalWorkers(0U);
	const rts::JobFloatingPointState floatingPointState;
	rts::JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		jobs.recordSerialFallback();
		if (status)
			status->usedSerialFallback = 1U;
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
	UnsignedInt submitted = 0U;
	for (; submitted < snapshotCount; ++submitted)
	{
		GeneralsAIEnemyPlanningJob *job = new (std::nothrow)
			GeneralsAIEnemyPlanningJob(snapshots + submitted,
				results + submitted, execution + submitted, submitted,
				injectedFailureOrdinal, &activePhysicalWorkers,
				&peakPhysicalWorkers, floatingPointState);
		rts::JobHandle handle = job ? jobs.trySubmit(job,
			rts::JOB_PRIORITY_FRAME_CRITICAL, group) : rts::JobHandle();
		if (!handle.isValid())
		{
			delete job;
			break;
		}
	}
	if (submitted != snapshotCount)
	{
		jobs.cancel(group);
		jobs.wait(group);
		GeneralsAIPlanningJobSystemEvidence evidence;
		evidence.collect(execution, submitted,
			peakPhysicalWorkers.load(std::memory_order_relaxed));
		PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
		jobs.recordSerialFallback();
		if (status)
			status->usedSerialFallback = 1U;
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
	// Never let the owner execute queued work while deciding whether this batch
	// earned physical-worker authority.  A bounded passive fence either proves
	// the requested topology or the group is cancelled and recomputed serially.
	const UnsignedInt physicalCompletionTimeoutMilliseconds = 8U;
	if (!jobs.waitWithoutOwnerHelp(group,
		physicalCompletionTimeoutMilliseconds))
	{
		jobs.cancel(group);
		jobs.wait(group);
		GeneralsAIPlanningJobSystemEvidence evidence;
		evidence.collect(execution, snapshotCount,
			peakPhysicalWorkers.load(std::memory_order_relaxed));
		PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
		jobs.recordSerialFallback();
		if (status)
			status->usedSerialFallback = 1U;
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
	GeneralsAIPlanningJobSystemEvidence evidence;
	evidence.collect(execution, snapshotCount,
		peakPhysicalWorkers.load(std::memory_order_relaxed));
	PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
	if (group.failed() || group.wasCancelled() ||
		evidence.physicalWorkerExecutions != snapshotCount ||
		evidence.ownerHelpedJobs != 0U ||
		evidence.distinctPhysicalWorkers <= 1U ||
		evidence.peakConcurrentPhysicalWorkers <= 1U)
	{
		jobs.recordSerialFallback();
		if (status)
			status->usedSerialFallback = 1U;
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
	for (UnsignedInt i = 0U; i < snapshotCount; ++i)
	{
		if (!execution[i].completed ||
			!ValidateGeneralsAIEnemyPlanningResult(snapshots[i], results[i]))
		{
			PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
			jobs.recordSerialFallback();
			if (status)
				status->usedSerialFallback = 1U;
			return PlanGeneralsAIEnemyPlanningBatchSerial(
				snapshots, snapshotCount, results);
		}
	}
	if (status)
		status->parallelSucceeded = 1U;

	if (executionMode == rts::AI_PLANNING_EXECUTION_SHADOW)
	{
		GeneralsAIEnemyPlanningResult serialResults[
			GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
		if (!PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, serialResults))
		{
			if (status)
				status->parallelSucceeded = 0U;
			return false;
		}
		Bool matched = true;
		for (UnsignedInt i = 0U; i < snapshotCount; ++i)
		{
			if (!EqualGeneralsAIEnemyPlanningResult(serialResults[i], results[i]))
			{
				if (status && status->mismatchPlayerOrdinal ==
					rts::AI_PLANNING_INVALID_ORDINAL)
					status->mismatchPlayerOrdinal = i;
				matched = false;
			}
		}
		if (!matched)
		{
			jobs.recordSerialFallback();
			if (status)
				status->usedSerialFallback = 1U;
		}
		else if (status)
		{
			status->shadowMatched = 1U;
			// Shadow is an oracle check; the committed result is the owner-serial
			// value even when the physical worker matched it exactly.
			status->committedMode = rts::AI_PLANNING_EXECUTION_SERIAL;
		}
		for (UnsignedInt i = 0U; i < snapshotCount; ++i)
			results[i] = serialResults[i];
	}
	else if (status)
	{
		status->committedMode = rts::AI_PLANNING_EXECUTION_PARALLEL;
	}
	return true;
}
