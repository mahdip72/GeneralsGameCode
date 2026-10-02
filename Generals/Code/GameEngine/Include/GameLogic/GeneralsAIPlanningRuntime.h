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
#include <thread>

#if defined(_WIN64)
inline void ObserveGeneralsAIPlanningTest(rts::AIPlanningTestHooks *hooks,
	rts::AIPlanningTestEvent event, UnsignedInt ordinal)
{
	if (hooks != 0 && hooks->observe != 0)
		hooks->observe(hooks->context, event, ordinal, 0, 1, 0);
}
#endif

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
#if defined(_WIN64)
		bool traceSource = false;
		rts::performance::KernelPerformanceCheckpointProbe checkpoint;
#endif
	};

	GeneralsAIEnemyPlanningJob(
		const GeneralsAIEnemyPlanningSnapshot *snapshot,
		GeneralsAIEnemyPlanningResult *result, ExecutionRecord *execution,
		UnsignedInt jobOrdinal, UnsignedInt injectedFailureOrdinal,
		std::atomic<UnsignedInt> *activePhysicalWorkers,
		std::atomic<UnsignedInt> *peakPhysicalWorkers,
		const rts::JobFloatingPointState &floatingPointState,
		std::atomic<UnsignedInt> *testRendezvous = 0,
		UnsignedInt testRendezvousTarget = 0U
#if defined(_WIN64)
		, rts::AIPlanningTestHooks *testHooks = 0
#endif
		) :
		m_snapshot(snapshot), m_result(result), m_execution(execution),
		m_jobOrdinal(jobOrdinal),
		m_injectedFailureOrdinal(injectedFailureOrdinal),
		m_activePhysicalWorkers(activePhysicalWorkers),
		m_peakPhysicalWorkers(peakPhysicalWorkers),
		m_floatingPointState(floatingPointState),
		m_testRendezvous(testRendezvous),
		m_testRendezvousTarget(testRendezvousTarget)
#if defined(_WIN64)
		, m_testHooks(testHooks)
#endif
		{}

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
			// Focused tests may hold tiny jobs at this point until every expected
			// physical worker has arrived. Production never supplies this hook.
			if (m_testRendezvous && m_testRendezvousTarget > 1U)
			{
				m_testRendezvous->fetch_add(1U, std::memory_order_acq_rel);
				while (m_testRendezvous->load(std::memory_order_acquire) <
					m_testRendezvousTarget && !context.isCancellationRequested())
					std::this_thread::yield();
			}
		}

#if defined(_WIN64)
		if (m_execution->traceSource) m_execution->checkpoint.beginRecord();
#endif
		const Bool planned = planBody(context.isCancellationRequested());
		if (m_execution->physicalWorker)
			m_activePhysicalWorkers->fetch_sub(1U, std::memory_order_acq_rel);
		if (!planned) context.fail();
	}
#if defined(_WIN64)
	Bool executeInline()
	{
		const rts::JobFloatingPointScope floatingPointScope(m_floatingPointState);
		return planBody(false);
	}
#endif

private:
	Bool planBody(Bool cancellationRequested)
	{
#if defined(_WIN64)
		ObserveGeneralsAIPlanningTest(m_testHooks, rts::AI_PLANNING_TEST_RANGE_ENTRY, m_jobOrdinal);
#endif
		Bool cancelled = cancellationRequested;
#if defined(_WIN64)
		if (!cancelled && m_jobOrdinal != m_injectedFailureOrdinal &&
			m_testHooks != 0 && m_testHooks->cancelAtEntry != 0)
			cancelled = m_testHooks->cancelAtEntry(m_testHooks->context, m_jobOrdinal, 0, 1);
		const rts::performance::KernelPerformanceCheckpoint entry = {1, m_jobOrdinal, 0};
		if (m_execution->traceSource)
			cancelled = m_execution->checkpoint.cancelled(entry, cancelled);
#endif
		Bool planned = !cancelled && m_jobOrdinal != m_injectedFailureOrdinal;
		if (planned)
		{
#if defined(_WIN64)
			ObserveGeneralsAIPlanningTest(m_testHooks, rts::AI_PLANNING_TEST_PLAYER_BODY, m_jobOrdinal);
#endif
			planned = PlanGeneralsAIEnemyTarget(*m_snapshot, m_result);
		}
#if defined(_WIN64)
		if (m_execution->traceSource)
		{
			const rts::performance::KernelPerformanceCheckpoint end = {2, m_jobOrdinal, planned ? 1U : 0U};
			m_execution->checkpoint.finish(cancelled ? entry : end, planned ? 1 : 0,
				cancelled ? rts::performance::KERNEL_RANGE_CANCELLED : planned ?
				rts::performance::KERNEL_RANGE_COMPLETED : rts::performance::KERNEL_RANGE_FAILED);
		}
#endif
		m_execution->completed = planned;
		return planned;
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
	std::atomic<UnsignedInt> *m_testRendezvous;
	UnsignedInt m_testRendezvousTarget;
#if defined(_WIN64)
	rts::AIPlanningTestHooks *m_testHooks;
#endif
};

struct GeneralsAIPlanningJobSystemEvidence
{
	GeneralsAIPlanningJobSystemEvidence() : physicalWorkerMask(0U),
		distinctPhysicalWorkers(0U), peakConcurrentPhysicalWorkers(0U),
		physicalWorkerExecutions(0U), ownerHelpedJobs(0U),
		nativeAdmissionAccepted(false) {}

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
	Bool nativeAdmissionAccepted;
};

#if defined(_WIN64)
inline rts::performance::KernelPerformanceDigest GeneralsAIPlanningDecisionFacts(UnsignedInt count)
{
	rts::performance::KernelPerformanceCanonicalWriter facts;
	if (!facts.begin(1) || !facts.u32(1, count)) return rts::performance::KernelPerformanceDigest();
	return facts.finish();
}

// Records released native job facts; it never owns execution or game commit.
class GeneralsAIPlanningSourceAttempt
{
public:
	GeneralsAIPlanningSourceAttempt(rts::AIPlanningReferenceBatchTransport *transport,
		GeneralsAIEnemyPlanningJob::ExecutionRecord *execution, UnsignedInt count,
		rts::JobSystem &jobs) : m_transport(transport), m_execution(execution), m_count(count),
		m_workers(0), m_pending(0), m_outstanding(0), m_enabled(false), m_released(false)
	{
		if (transport == 0 || transport->referenceLedger == 0 || !transport->referenceAttempt.valid()) return;
		const rts::performance::KernelPerformanceReferenceMode mode = transport->referenceLedger->mode();
		if (mode != rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING &&
			mode != rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE) return;
		m_workers = jobs.workerCount(); m_pending = jobs.pendingOwnerCompletionCount();
		m_outstanding = jobs.outstandingJobCount();
		m_enabled = transport->referenceLedger->bindCapturedInput(transport->referenceAttempt,
			transport->fieldSchema, transport->operationCount, transport->writeInput, transport->immutableInput);
		if (m_enabled) m_facts = GeneralsAIPlanningDecisionFacts(count);
		for (UnsignedInt i = 0; i != count; ++i) execution[i].traceSource = m_enabled;
	}
	~GeneralsAIPlanningSourceAttempt() { release(0, false); }
	void release(UnsignedInt submitted, bool cancelled)
	{
		if (!m_enabled || m_released) return;
		m_released = true;
		rts::performance::KernelPerformanceReferenceLedger &ledger = *m_transport->referenceLedger;
		const rts::performance::KernelPerformanceAttempt attempt = m_transport->referenceAttempt;
		rts::performance::KernelPerformanceAttemptDecision decision = {};
		decision.site = 1; decision.reasonSchema = 1;
		decision.reason = submitted != 0 ? (cancelled ? 3 : 1) : 2;
		decision.deterministicEligible = m_count >= 2; decision.deterministicFacts = m_facts;
		decision.admission = submitted != 0 ? rts::performance::KERNEL_ADMISSION_ACCEPTED :
			rts::performance::KERNEL_ADMISSION_REFUSED;
		decision.sourceConfiguredWorkers = m_workers;
		decision.dynamicFactsKnownMask = 3; decision.pendingJobs = m_pending;
		decision.outstandingJobs = m_outstanding;
		rts::AIPlanningTestHooks *hooks = m_transport->testHooks;
		if (hooks != 0 && hooks->releasedGroup != 0)
		{
			unsigned completed = 0;
			for (UnsignedInt i = 0; i != submitted; ++i) if (m_execution[i].completed) ++completed;
			hooks->releasedGroup(hooks->context, cancelled, completed, submitted, decision.reason);
		}
		ledger.observeDecision(attempt, decision);
		if (submitted == 0) return;
		const rts::performance::KernelPerformanceDispatchPlan dispatch = {1, 1, 1, submitted,
			submitted, 1, GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS};
		ledger.observeDispatch(attempt, dispatch);
		for (UnsignedInt i = 0; i != submitted; ++i)
		{
			const rts::performance::KernelPerformanceRangePlan range = {1, i, 1, i, i + 1, 1};
			ledger.observeRangePlan(attempt, range);
		}
		for (UnsignedInt i = 0; i != submitted; ++i)
		{
			const rts::performance::KernelPerformanceRangePlan range = {1, i, 1, i, i + 1, 1};
			rts::performance::KernelPerformanceRangeProgress progress = {};
			progress.checkpoint = m_execution[i].checkpoint.snapshot();
			progress.publication = !progress.checkpoint.entered ? rts::performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
				cancelled || progress.checkpoint.terminal == rts::performance::KERNEL_RANGE_CANCELLED ?
				rts::performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : m_execution[i].completed ?
				rts::performance::KERNEL_PUBLICATION_PUBLISHED : rts::performance::KERNEL_PUBLICATION_REJECTED;
			ledger.observeReleasedRange(attempt, range, progress);
		}
	}
private:
	rts::AIPlanningReferenceBatchTransport *m_transport;
	GeneralsAIEnemyPlanningJob::ExecutionRecord *m_execution;
	UnsignedInt m_count, m_workers;
	rts::JobMetricCounter m_pending, m_outstanding;
	bool m_enabled, m_released;
	rts::performance::KernelPerformanceDigest m_facts;
};

inline Bool ConsumeGeneralsAIPlanningRanges(rts::AIPlanningReferenceBatchTransport *transport,
	const GeneralsAIEnemyPlanningSnapshot *snapshots, UnsignedInt count,
	GeneralsAIEnemyPlanningResult *results, UnsignedInt injectedFailureOrdinal,
	GeneralsAIEnemyPlanningJob::ExecutionRecord *execution,
	const rts::JobFloatingPointState &floatingPointState)
{
	using namespace rts::performance;
	if (transport == 0 || transport->referenceLedger == 0 || !transport->referenceAttempt.valid())
		return false;
	KernelPerformanceReferenceLedger &ledger = *transport->referenceLedger;
	const KernelPerformanceAttempt attempt = transport->referenceAttempt;
	KernelPerformanceAttemptDecision decision = {};
	if (!ledger.bindCapturedInput(attempt, transport->fieldSchema, transport->operationCount,
		transport->writeInput, transport->immutableInput) ||
		!ledger.replayDecision(attempt, 1, count >= 2, GeneralsAIPlanningDecisionFacts(count), decision) ||
		decision.admission != KERNEL_ADMISSION_ACCEPTED) return false;
	if (decision.reasonSchema != 1 || (decision.reason != 1 && decision.reason != 3)) return false;
	const bool sourceCancelled = decision.reason == 3;
	KernelPerformanceDispatchPlan source = {};
	if (!ledger.readSourceDispatch(attempt, 1, source) || source.rangeCount == 0 ||
		source.rangeCount > count) return false;
	const KernelPerformanceDispatchPlan dispatch = {1, 1, 1, source.rangeCount,
		source.rangeCount, 1, GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS};
	if (!ledger.observeDispatch(attempt, dispatch)) return false;
	for (UnsignedInt i = 0; i != dispatch.rangeCount; ++i)
	{
		const KernelPerformanceRangePlan range = {1, i, 1, i, i + 1, 1};
		if (!ledger.observeRangePlan(attempt, range)) return false;
	}
	Bool completed = dispatch.rangeCount == count && !sourceCancelled;
	for (UnsignedInt i = 0; i != dispatch.rangeCount; ++i)
	{
		GeneralsAIEnemyPlanningJob::ExecutionRecord &record = execution[i];
		const KernelPerformanceRangePlan range = {1, i, 1, i, i + 1, 1};
		KernelPerformanceInlineBody body;
		const KernelPerformanceInlineAction action = ledger.beginInlineBody(attempt, range,
			KernelPerformanceLedger::instance(), body, record.checkpoint);
		if (action == KERNEL_INLINE_INVALID) return false;
		if (action == KERNEL_INLINE_EXECUTE)
		{
			record.traceSource = true;
			GeneralsAIEnemyPlanningJob job(snapshots + i, results + i, &record, i,
				injectedFailureOrdinal, 0, 0, floatingPointState, 0, 0, transport->testHooks);
			job.executeInline();
		}
		KernelPerformanceRangeProgress progress = {};
		progress.checkpoint = record.checkpoint.snapshot();
		progress.publication = !progress.checkpoint.entered ? KERNEL_PUBLICATION_NOT_APPLICABLE :
			sourceCancelled || progress.checkpoint.terminal == KERNEL_RANGE_CANCELLED ? KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL :
			record.completed ? KERNEL_PUBLICATION_PUBLISHED : KERNEL_PUBLICATION_REJECTED;
		if (action == KERNEL_INLINE_EXECUTE && !ledger.finishInlineBody(body, progress)) return false;
		if (!ledger.observeReleasedRange(attempt, range, progress)) return false;
		completed = record.completed && completed;
	}
	return completed;
}
#endif

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
	status->nativeAdmissionAccepted = 0U;
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
	status->nativeAdmissionAccepted =
		evidence.nativeAdmissionAccepted ? 1U : 0U;
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

// Retail Generals evaluates due skirmish owners recursively while scoring the
// first owner in PlayerList order.  The live batch cannot publish those nested
// targets during capture, so project the same recursion into immutable masks.
// The resulting snapshots remain independent and can still be planned by the
// worker batch; only the owner-observation order is serialized here.
class GeneralsAIEnemyPlanningOrderProjector
{
public:
	GeneralsAIEnemyPlanningOrderProjector(
		const GeneralsAIEnemyPlanningSnapshot *captured,
		const UnsignedInt *ownerSourceOrdinals, UnsignedInt ownerCount,
		const Bool *skirmishBySource, const Int *initialEnemySourceOrdinals,
		UnsignedInt playerCount, GeneralsAIEnemyPlanningSnapshot *projected,
		GeneralsAIEnemyPlanningResult *results,
		UnsignedInt *publicationOrder) :
		m_captured(captured), m_ownerSourceOrdinals(ownerSourceOrdinals),
		m_ownerCount(ownerCount), m_skirmishBySource(skirmishBySource),
		m_playerCount(playerCount), m_projected(projected), m_results(results),
		m_publicationOrder(publicationOrder), m_publicationCount(0U)
	{
		for (UnsignedInt i = 0U; i < GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS; ++i)
		{
			m_dueOwnerBySource[i] = -1;
			m_projectedEnemyBySource[i] = -1;
			m_state[i] = 0U;
		}
		for (UnsignedInt i = 0U; i < playerCount; ++i)
			m_projectedEnemyBySource[i] = initialEnemySourceOrdinals[i];
		for (UnsignedInt i = 0U; i < ownerCount; ++i)
			m_dueOwnerBySource[ownerSourceOrdinals[i]] = (Int)i;
	}

	Bool run()
	{
		for (UnsignedInt i = 0U; i < m_ownerCount; ++i)
		{
			if (!project(i))
				return false;
		}
		return true;
	}

private:
	Bool project(UnsignedInt ownerIndex)
	{
		if (m_state[ownerIndex] == 2U)
			return true;
		// getAiEnemy advances the due frame before entering acquireEnemy, so a
		// recursive query of the same owner observes its pre-existing target.
		if (m_state[ownerIndex] == 1U)
			return true;
		m_state[ownerIndex] = 1U;
		m_projected[ownerIndex] = m_captured[ownerIndex];
		GeneralsAIEnemyPlanningSnapshot &snapshot = m_projected[ownerIndex];
		const UnsignedInt ownerSource = m_ownerSourceOrdinals[ownerIndex];
		for (UnsignedInt candidateIndex = 0U;
			candidateIndex < snapshot.candidateCount; ++candidateIndex)
		{
			GeneralsAIEnemyCandidateFact &candidate =
				snapshot.candidates[candidateIndex];
			candidate.targetingCandidateMask = 0U;
			candidate.targetingOwnerMask = 0U;
			for (UnsignedInt source = 0U; source < m_playerCount; ++source)
			{
				if (source == candidate.sourceOrdinal || !m_skirmishBySource[source])
					continue;
				const Int dueOwner = m_dueOwnerBySource[source];
				if (dueOwner >= 0 && m_state[(UnsignedInt)dueOwner] == 0U &&
					!project((UnsignedInt)dueOwner))
				{
					return false;
				}
				const Int target = m_projectedEnemyBySource[source];
				const UnsignedInt sourceBit = 1U << source;
				if (target == (Int)candidate.sourceOrdinal)
					candidate.targetingCandidateMask |= sourceBit;
				if (target == (Int)ownerSource)
					candidate.targetingOwnerMask |= sourceBit;
			}
		}
		if (!PlanGeneralsAIEnemyTarget(snapshot, &m_results[ownerIndex]) ||
			!ValidateGeneralsAIEnemyPlanningResult(
				snapshot, m_results[ownerIndex]))
		{
			return false;
		}
		if (m_results[ownerIndex].selectedPlayerIndex >= 0)
		{
			m_projectedEnemyBySource[ownerSource] =
				(Int)m_results[ownerIndex].orderKey.sourceOrdinal;
		}
		m_state[ownerIndex] = 2U;
		m_publicationOrder[m_publicationCount++] = ownerIndex;
		return true;
	}

	const GeneralsAIEnemyPlanningSnapshot *m_captured;
	const UnsignedInt *m_ownerSourceOrdinals;
	UnsignedInt m_ownerCount;
	const Bool *m_skirmishBySource;
	UnsignedInt m_playerCount;
	GeneralsAIEnemyPlanningSnapshot *m_projected;
	GeneralsAIEnemyPlanningResult *m_results;
	UnsignedInt *m_publicationOrder;
	UnsignedInt m_publicationCount;
	Int m_dueOwnerBySource[GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
	Int m_projectedEnemyBySource[GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
	UnsignedInt m_state[GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
};

inline Bool ProjectGeneralsAIEnemyPlanningOrder(
	const GeneralsAIEnemyPlanningSnapshot *captured,
	const UnsignedInt *ownerSourceOrdinals, UnsignedInt ownerCount,
	const Bool *skirmishBySource, const Int *initialEnemySourceOrdinals,
	UnsignedInt playerCount, GeneralsAIEnemyPlanningSnapshot *projected,
	GeneralsAIEnemyPlanningResult *results, UnsignedInt *publicationOrder)
{
	if (!captured || !ownerSourceOrdinals || !skirmishBySource ||
		!initialEnemySourceOrdinals || !projected || !results || !publicationOrder ||
		ownerCount == 0U ||
		ownerCount > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS || playerCount == 0U ||
		playerCount > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS)
	{
		return false;
	}
	Bool seen[GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS] = { false };
	for (UnsignedInt source = 0U; source < playerCount; ++source)
	{
		if (initialEnemySourceOrdinals[source] < -1 ||
			initialEnemySourceOrdinals[source] >= (Int)playerCount)
		{
			return false;
		}
	}
	for (UnsignedInt i = 0U; i < ownerCount; ++i)
	{
		const UnsignedInt source = ownerSourceOrdinals[i];
		if (source >= playerCount || seen[source] || !skirmishBySource[source] ||
			!ValidateGeneralsAIEnemyPlanningSnapshot(captured[i]))
		{
			return false;
		}
		seen[source] = true;
	}
	GeneralsAIEnemyPlanningOrderProjector projector(captured,
		ownerSourceOrdinals, ownerCount, skirmishBySource,
		initialEnemySourceOrdinals, playerCount, projected, results,
		publicationOrder);
	return projector.run();
}

// Production executor shared by the game and focused test. Once the current
// planning epoch has captured snapshots, every unavailable or failed parallel
// path recomputes that same complete batch serially. It never switches epochs.
inline Bool ExecuteGeneralsAIEnemyPlanningBatch(
	rts::AIPlanningExecutionMode executionMode, Bool isNetworkGame,
	const GeneralsAIEnemyPlanningSnapshot *snapshots, UnsignedInt snapshotCount,
	GeneralsAIEnemyPlanningResult *results,
	UnsignedInt injectedFailureOrdinal = rts::AI_PLANNING_INVALID_ORDINAL,
	rts::AIPlanningBatchStatus *status = 0,
	std::atomic<UnsignedInt> *testRendezvous = 0
#if defined(_WIN64)
	, rts::performance::KernelPerformanceBatch *performanceBatch = 0
	, rts::AIPlanningReferenceBatchTransport *referenceBatch = 0
#endif
	)
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
#if defined(_WIN64)
	const bool sourceBoundInline = referenceBatch != 0 && referenceBatch->referenceLedger != 0 &&
		referenceBatch->referenceLedger->runMode() == rts::performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	GeneralsAIEnemyPlanningJob::ExecutionRecord execution[
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
	GeneralsAIPlanningSourceAttempt source(referenceBatch, execution,
		snapshotCount, jobs);
#endif
	if (executionMode == rts::AI_PLANNING_EXECUTION_SERIAL ||
		snapshotCount < 2U ||
#if defined(_WIN64)
		(!sourceBoundInline &&
#endif
		(!jobs.isRunning() || jobs.workerCount() < 2U)
#if defined(_WIN64)
		)
#endif
		)
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

#if defined(_WIN64)
	rts::AIPlanningPerformanceInterval schedule(performanceBatch,
		rts::performance::KERNEL_PERFORMANCE_SCHEDULE);
	rts::AIPlanningTestHooks *testHooks = referenceBatch != 0 ? referenceBatch->testHooks : 0;
#endif
#if !defined(_WIN64)
	GeneralsAIEnemyPlanningJob::ExecutionRecord execution[
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
#endif
	std::atomic<UnsignedInt> activePhysicalWorkers(0U);
	std::atomic<UnsignedInt> peakPhysicalWorkers(0U);
	const rts::JobFloatingPointState floatingPointState;
	GeneralsAIPlanningJobSystemEvidence evidence;
#if defined(_WIN64)
	if (sourceBoundInline)
	{
		evidence.nativeAdmissionAccepted = true;
		schedule.end();
		if (!ConsumeGeneralsAIPlanningRanges(referenceBatch, snapshots, snapshotCount,
			results, injectedFailureOrdinal, execution, floatingPointState))
		{
			PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
			jobs.recordSerialFallback();
			if (status) status->usedSerialFallback = 1U;
			return PlanGeneralsAIEnemyPlanningBatchSerial(snapshots, snapshotCount, results);
		}
	}
	else
	{
#endif
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
				&peakPhysicalWorkers, floatingPointState, testRendezvous,
				testRendezvous ? snapshotCount : 0U
#if defined(_WIN64)
				, testHooks
#endif
				);
		rts::JobHandle handle = job ? jobs.trySubmit(job,
			rts::JOB_PRIORITY_FRAME_CRITICAL, group) : rts::JobHandle();
		if (!handle.isValid())
		{
			delete job;
			break;
		}
		evidence.nativeAdmissionAccepted = true;
	}
#if defined(_WIN64)
	schedule.end();
#endif
	if (submitted != snapshotCount)
	{
		jobs.cancel(group);
		jobs.wait(group);
#if defined(_WIN64)
		source.release(submitted, group.wasCancelled());
#endif
		GeneralsAIPlanningJobSystemEvidence evidence;
		evidence.nativeAdmissionAccepted = submitted != 0U;
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
	const UnsignedInt physicalCompletionTimeoutMilliseconds =
		testRendezvous ? 1000U : 8U;
#if defined(_WIN64)
	if (testHooks != 0 && testHooks->beforeWait != 0) testHooks->beforeWait(testHooks->context);
	rts::AIPlanningPerformanceInterval wait(performanceBatch,
		rts::performance::KERNEL_PERFORMANCE_WAIT);
#endif
	const Bool passiveWaitCompleted = jobs.waitWithoutOwnerHelp(group,
		physicalCompletionTimeoutMilliseconds);
#if defined(_WIN64)
	wait.end();
#endif
	if (!passiveWaitCompleted)
	{
		jobs.cancel(group);
#if defined(_WIN64)
		if (testHooks != 0 && testHooks->afterCancel != 0) testHooks->afterCancel(testHooks->context);
#endif
		jobs.wait(group);
#if defined(_WIN64)
		source.release(submitted, group.wasCancelled());
#endif
		GeneralsAIPlanningJobSystemEvidence evidence;
		evidence.nativeAdmissionAccepted = submitted != 0U;
		evidence.collect(execution, snapshotCount,
			peakPhysicalWorkers.load(std::memory_order_relaxed));
		PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
		jobs.recordSerialFallback();
		if (status)
			status->usedSerialFallback = 1U;
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
#if defined(_WIN64)
	source.release(submitted, group.wasCancelled());
#endif
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
#if defined(_WIN64)
	}
#endif
#if defined(_WIN64)
	rts::AIPlanningPerformanceInterval validate(performanceBatch,
		rts::performance::KERNEL_PERFORMANCE_VALIDATE);
#endif
	for (UnsignedInt i = 0U; i < snapshotCount; ++i)
	{
#if defined(_WIN64)
		if (execution[i].completed)
			ObserveGeneralsAIPlanningTest(testHooks, rts::AI_PLANNING_TEST_OWNER_VALIDATION, i);
#endif
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
#if defined(_WIN64)
	const bool referenceObserved = rts::ObserveAIPlanningReferenceBatch(
		performanceBatch, referenceBatch);
	const bool validationClosed = validate.end();
	if (referenceBatch != 0 && referenceBatch->referenceAttempt.valid() &&
		(!referenceObserved || !validationClosed))
	{
		// A live reference attempt is part of the adapter's admission contract:
		// a failed observer or validation extent close must not let
		// validated-but-unlinked output reach the owner. Recompute this captured
		// epoch through the established serial path before the caller can publish
		// anything.
		PublishGeneralsAIPlanningJobSystemEvidence(status, evidence);
		jobs.recordSerialFallback();
		if (status)
		{
			status->usedSerialFallback = 1U;
			status->committedMode = rts::AI_PLANNING_EXECUTION_SERIAL;
		}
		return PlanGeneralsAIEnemyPlanningBatchSerial(
			snapshots, snapshotCount, results);
	}
#endif
	if (status)
		status->parallelSucceeded =
#if defined(_WIN64)
			sourceBoundInline ? 0U :
#endif
			1U;

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
		status->committedMode =
#if defined(_WIN64)
			sourceBoundInline ? rts::AI_PLANNING_EXECUTION_SERIAL :
#endif
			rts::AI_PLANNING_EXECUTION_PARALLEL;
	}
	return true;
}

#if defined(_WIN64)
struct GeneralsAIEnemyReferenceView
{
	const GeneralsAIEnemyPlanningSnapshot *snapshots;
	GeneralsAIEnemyPlanningResult *results;
	UnsignedInt count;
};

inline Bool WriteGeneralsAIEnemyReferenceInput(
	rts::performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const GeneralsAIEnemyReferenceView *view =
		static_cast<const GeneralsAIEnemyReferenceView *>(context);
	if (!view || !view->snapshots || view->count == 0U ||
		view->count > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS ||
		!writer.u32(1U, 0U) || !writer.sequence(2U, view->count))
		return false;
	for (UnsignedInt i = 0U; i < view->count; ++i)
	{
		const GeneralsAIEnemyPlanningSnapshot &snapshot = view->snapshots[i];
		if (snapshot.candidateCount > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS ||
			!writer.u32(3U, snapshot.frame) ||
			!writer.u32(4U, snapshot.ownerPlayerIndex) ||
			!writer.f32(5U, snapshot.initialBestDistanceSquared) ||
			!writer.sequence(6U, snapshot.candidateCount))
			return false;
		for (UnsignedInt candidateIndex = 0U;
			candidateIndex < snapshot.candidateCount; ++candidateIndex)
		{
			const GeneralsAIEnemyCandidateFact &candidate =
				snapshot.candidates[candidateIndex];
			if (!writer.u32(7U, candidate.sourceOrdinal) ||
				!writer.i32(8U, candidate.playerIndex) ||
				!writer.f32(9U, candidate.baseDistanceSquared) ||
				!writer.u32(10U, candidate.targetingCandidateMask) ||
				!writer.u32(11U, candidate.targetingOwnerMask))
				return false;
		}
	}
	return true;
}

inline Bool WriteGeneralsAIEnemyReferenceOutput(
	rts::performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const GeneralsAIEnemyReferenceView *view =
		static_cast<const GeneralsAIEnemyReferenceView *>(context);
	if (!view || !view->results || view->count == 0U ||
		view->count > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS ||
		!writer.u32(1U, 0U) || !writer.sequence(2U, view->count))
		return false;
	for (UnsignedInt i = 0U; i < view->count; ++i)
	{
		const GeneralsAIEnemyPlanningResult &result = view->results[i];
		if (!writer.u32(3U, result.valid) ||
			!writer.i32(4U, result.selectedPlayerIndex) ||
			!writer.f32(5U, result.selectedDistanceSquared) ||
			!writer.u32(6U, result.orderKey.frame) ||
			!writer.u32(7U, result.orderKey.playerIndex) ||
			!writer.u32(8U, result.orderKey.subphase) ||
			!writer.u32(9U, result.orderKey.sourceOrdinal) ||
			!writer.u32(10U, result.orderKey.emissionOrdinal))
			return false;
	}
	return true;
}

inline Bool ComputeGeneralsAIEnemyReferenceSerial(const void *immutableInput,
	void *detachedOutput)
{
	const GeneralsAIEnemyReferenceView *inputView =
		static_cast<const GeneralsAIEnemyReferenceView *>(immutableInput);
	GeneralsAIEnemyReferenceView *outputView =
		static_cast<GeneralsAIEnemyReferenceView *>(detachedOutput);
	return inputView != 0 && outputView != 0 && inputView->snapshots != 0 &&
		outputView->results != 0 && inputView->count != 0U &&
		inputView->count <= GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS &&
		outputView->count == inputView->count &&
		PlanGeneralsAIEnemyPlanningBatchSerial(inputView->snapshots,
			inputView->count, outputView->results);
}
#endif
