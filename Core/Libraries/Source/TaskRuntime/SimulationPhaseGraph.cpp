/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/SimulationPhaseGraph.h"

#include <stddef.h>

#if defined(_MSC_VER) && _MSC_VER < 1300
#pragma warning(disable : 4711)
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_MSC_VER) && _MSC_VER < 1300
#include "Utility/interlocked_adapter.h"
#endif
#endif

namespace rts
{
namespace
{

const long SIMULATION_PHASE_OUTCOME_INACTIVE = 0;
const long SIMULATION_PHASE_OUTCOME_OPEN = 1;
const long SIMULATION_PHASE_OUTCOME_COMPLETED = 2;
const long SIMULATION_PHASE_OUTCOME_CANCELLED = 3;
const long SIMULATION_PHASE_OUTCOME_FAILED = 4;
const long SIMULATION_PHASE_OUTCOME_STALE = 5;

const long SIMULATION_PHASE_TERMINAL_CANCELLED = 1;
const long SIMULATION_PHASE_TERMINAL_STALE = 2;
const long SIMULATION_PHASE_TERMINAL_FAILED = 4;

long OutcomeForWorkStatus(SimulationPhaseWorkStatus status)
{
	if (status == SIMULATION_PHASE_WORK_CANCELLED)
		return SIMULATION_PHASE_OUTCOME_CANCELLED;
	if (status == SIMULATION_PHASE_WORK_STALE_GENERATION)
		return SIMULATION_PHASE_OUTCOME_STALE;
	return SIMULATION_PHASE_OUTCOME_FAILED;
}

long TerminalBitForWorkStatus(SimulationPhaseWorkStatus status)
{
	if (status == SIMULATION_PHASE_WORK_FAILED)
		return SIMULATION_PHASE_TERMINAL_FAILED;
	if (status == SIMULATION_PHASE_WORK_STALE_GENERATION)
		return SIMULATION_PHASE_TERMINAL_STALE;
	return SIMULATION_PHASE_TERMINAL_CANCELLED;
}

SimulationPhaseWorkStatus WorkStatusForTerminalMask(long terminalMask)
{
	// Fixed precedence makes mixed worker outcomes independent of finish order.
	if ((terminalMask & SIMULATION_PHASE_TERMINAL_FAILED) != 0)
		return SIMULATION_PHASE_WORK_FAILED;
	if ((terminalMask & SIMULATION_PHASE_TERMINAL_STALE) != 0)
		return SIMULATION_PHASE_WORK_STALE_GENERATION;
	if ((terminalMask & SIMULATION_PHASE_TERMINAL_CANCELLED) != 0)
		return SIMULATION_PHASE_WORK_CANCELLED;
	return SIMULATION_PHASE_WORK_SUCCEEDED;
}

SimulationPhaseWorkStatus WorkStatusForPublicationOutcome(long outcome)
{
	if (outcome == SIMULATION_PHASE_OUTCOME_FAILED)
		return SIMULATION_PHASE_WORK_FAILED;
	if (outcome == SIMULATION_PHASE_OUTCOME_STALE)
		return SIMULATION_PHASE_WORK_STALE_GENERATION;
	if (outcome == SIMULATION_PHASE_OUTCOME_CANCELLED)
		return SIMULATION_PHASE_WORK_CANCELLED;
	return SIMULATION_PHASE_WORK_SUCCEEDED;
}

bool IsTerminalOutcome(long outcome)
{
	return outcome == SIMULATION_PHASE_OUTCOME_CANCELLED ||
		outcome == SIMULATION_PHASE_OUTCOME_FAILED ||
		outcome == SIMULATION_PHASE_OUTCOME_STALE;
}

long AtomicLoad(const volatile long *value)
{
#if defined(_WIN32)
	return InterlockedCompareExchange(const_cast<volatile long *>(value), 0, 0);
#else
	return __sync_val_compare_and_swap(const_cast<volatile long *>(value), 0, 0);
#endif
}

void AtomicStore(volatile long *value, long desired)
{
#if defined(_WIN32)
	InterlockedExchange(value, desired);
#else
	// __sync_lock_test_and_set is only an acquire barrier on some targets.
	// A CAS loop preserves the full sequential barrier required when a worker
	// publishes output/identity immediately before its terminal state.
	long observed = __sync_val_compare_and_swap(value, 0, 0);
	while (!__sync_bool_compare_and_swap(value, observed, desired))
		observed = __sync_val_compare_and_swap(value, 0, 0);
#endif
}

bool AtomicCompareExchange(volatile long *value, long expected, long desired)
{
#if defined(_WIN32)
	return InterlockedCompareExchange(value, desired, expected) == expected;
#else
	return __sync_bool_compare_and_swap(value, expected, desired);
#endif
}

long AtomicIncrement(volatile long *value)
{
	long observed = AtomicLoad(value);
	while (!AtomicCompareExchange(value, observed, observed + 1))
		observed = AtomicLoad(value);
	return observed + 1;
}

long AtomicDecrement(volatile long *value)
{
	long observed = AtomicLoad(value);
	while (!AtomicCompareExchange(value, observed, observed - 1))
		observed = AtomicLoad(value);
	return observed - 1;
}

void AtomicOr(volatile long *value, long bits)
{
	long observed = AtomicLoad(value);
	while (!AtomicCompareExchange(value, observed, observed | bits))
		observed = AtomicLoad(value);
}

void AcquireAtomicLock(volatile long *lock)
{
	while (!AtomicCompareExchange(lock, 0, 1))
	{
#if defined(_WIN32)
		Sleep(0);
#endif
	}
}

class ScopedAtomicLock
{
public:
	explicit ScopedAtomicLock(volatile long *lock) : m_lock(lock)
	{
		AcquireAtomicLock(m_lock);
	}

	~ScopedAtomicLock()
	{
		release();
	}

	void release()
	{
		if (m_lock != 0)
		{
			AtomicStore(m_lock, 0);
			m_lock = 0;
		}
	}

private:
	ScopedAtomicLock(const ScopedAtomicLock &);
	ScopedAtomicLock &operator=(const ScopedAtomicLock &);
	volatile long *m_lock;
};

class ScopedAtomicCounter
{
public:
	explicit ScopedAtomicCounter(volatile long *counter) : m_counter(counter)
	{
		AtomicIncrement(m_counter);
	}

	~ScopedAtomicCounter()
	{
		AtomicDecrement(m_counter);
	}

private:
	ScopedAtomicCounter(const ScopedAtomicCounter &);
	ScopedAtomicCounter &operator=(const ScopedAtomicCounter &);
	volatile long *m_counter;
};

class ScopedAtomicFlagRelease
{
public:
	explicit ScopedAtomicFlagRelease(volatile long *flag) : m_flag(flag) {}
	~ScopedAtomicFlagRelease()
	{
		AtomicStore(m_flag, 0);
	}

private:
	ScopedAtomicFlagRelease(const ScopedAtomicFlagRelease &);
	ScopedAtomicFlagRelease &operator=(const ScopedAtomicFlagRelease &);
	volatile long *m_flag;
};

#if defined(RTS_BUILD_CORE_EXTRAS)
SimulationPhaseExecuteAdmissionHook g_executeAdmissionHook = 0;
void *g_executeAdmissionHookContext = 0;
#endif

bool IsPointerSizePairValid(const void *pointer, unsigned byteCount)
{
	return (pointer == 0 && byteCount == 0) ||
		(pointer != 0 && byteCount != 0);
}

struct ByteSpan
{
	ByteSpan() : begin(0), end(0) {}
	size_t begin;
	size_t end;
};

bool MakeByteSpan(const void *pointer, size_t byteCount, ByteSpan &span)
{
	if (pointer == 0 && byteCount != 0) return false;
	span.begin = reinterpret_cast<size_t>(pointer);
	span.end = span.begin + byteCount;
	return span.end >= span.begin;
}

bool MakeElementSpan(const void *pointer, unsigned elementCount,
	size_t elementSize, ByteSpan &span)
{
	const size_t maximumSize = static_cast<size_t>(-1);
	if (elementSize != 0 &&
		static_cast<size_t>(elementCount) > maximumSize / elementSize)
	{
		return false;
	}
	return MakeByteSpan(pointer,
		static_cast<size_t>(elementCount) * elementSize, span);
}

bool SpansOverlap(const ByteSpan &left, const ByteSpan &right)
{
	return left.begin < left.end && right.begin < right.end &&
		left.begin < right.end && right.begin < left.end;
}

bool RangesOverlap(const void *left, unsigned leftBytes,
	const void *right, unsigned rightBytes)
{
	if (leftBytes == 0 || rightBytes == 0) return false;
	ByteSpan leftSpan;
	ByteSpan rightSpan;
	if (!MakeByteSpan(left, leftBytes, leftSpan) ||
		!MakeByteSpan(right, rightBytes, rightSpan))
	{
		return true;
	}
	return SpansOverlap(leftSpan, rightSpan);
}

bool IsTerminalJobState(SimulationPhaseJobState state)
{
	return state == SIMULATION_PHASE_JOB_SUCCEEDED ||
		state == SIMULATION_PHASE_JOB_COMMITTED ||
		state == SIMULATION_PHASE_JOB_CANCELLED ||
		state == SIMULATION_PHASE_JOB_FAILED ||
		state == SIMULATION_PHASE_JOB_STALE_GENERATION;
}

SimulationPhaseWorkStatus WorkStatusForJobState(SimulationPhaseJobState state)
{
	if (state == SIMULATION_PHASE_JOB_CANCELLED)
		return SIMULATION_PHASE_WORK_CANCELLED;
	if (state == SIMULATION_PHASE_JOB_STALE_GENERATION)
		return SIMULATION_PHASE_WORK_STALE_GENERATION;
	if (state == SIMULATION_PHASE_JOB_SUCCEEDED ||
		state == SIMULATION_PHASE_JOB_COMMITTED)
	{
		return SIMULATION_PHASE_WORK_SUCCEEDED;
	}
	return SIMULATION_PHASE_WORK_FAILED;
}

SimulationPhaseJobState JobStateForWorkStatus(SimulationPhaseWorkStatus status)
{
	if (status == SIMULATION_PHASE_WORK_SUCCEEDED)
		return SIMULATION_PHASE_JOB_SUCCEEDED;
	if (status == SIMULATION_PHASE_WORK_CANCELLED)
		return SIMULATION_PHASE_JOB_CANCELLED;
	if (status == SIMULATION_PHASE_WORK_STALE_GENERATION)
		return SIMULATION_PHASE_JOB_STALE_GENERATION;
	return SIMULATION_PHASE_JOB_FAILED;
}

} // namespace

#if defined(RTS_BUILD_CORE_EXTRAS)
void setSimulationPhaseExecuteAdmissionHookForTest(
	SimulationPhaseExecuteAdmissionHook hook, void *context)
{
	g_executeAdmissionHook = hook;
	g_executeAdmissionHookContext = context;
}
#endif

SimulationPhaseExecutionIdentity::SimulationPhaseExecutionIdentity() :
	m_kind(SIMULATION_PHASE_EXECUTION_NONE),
	m_physicalWorkerIndex(SIMULATION_PHASE_INVALID_WORKER_INDEX)
{
}

SimulationPhaseExecutionIdentity
SimulationPhaseExecutionIdentity::physicalWorker(unsigned workerIndex)
{
	SimulationPhaseExecutionIdentity identity;
	if (workerIndex != SIMULATION_PHASE_INVALID_WORKER_INDEX)
	{
		identity.m_kind = SIMULATION_PHASE_EXECUTION_PHYSICAL_WORKER;
		identity.m_physicalWorkerIndex = workerIndex;
	}
	return identity;
}

SimulationPhaseExecutionIdentity
SimulationPhaseExecutionIdentity::ownerHelp()
{
	SimulationPhaseExecutionIdentity identity;
	identity.m_kind = SIMULATION_PHASE_EXECUTION_OWNER_HELP;
	return identity;
}

SimulationPhaseExecutionKind SimulationPhaseExecutionIdentity::kind() const
{
	return m_kind;
}

unsigned SimulationPhaseExecutionIdentity::physicalWorkerIndex() const
{
	return m_physicalWorkerIndex;
}

bool SimulationPhaseExecutionIdentity::isValid() const
{
	return (m_kind == SIMULATION_PHASE_EXECUTION_PHYSICAL_WORKER &&
		m_physicalWorkerIndex != SIMULATION_PHASE_INVALID_WORKER_INDEX) ||
		(m_kind == SIMULATION_PHASE_EXECUTION_OWNER_HELP &&
		m_physicalWorkerIndex == SIMULATION_PHASE_INVALID_WORKER_INDEX);
}

SimulationPhaseJobContext::SimulationPhaseJobContext(
	const SimulationPhaseGraph *graph, SimulationPhaseId phaseId,
	SimulationPhaseJobKey jobKey, unsigned generation, unsigned internalEpoch,
	const SimulationPhaseExecutionIdentity &executionIdentity) :
	m_graph(graph), m_phaseId(phaseId), m_jobKey(jobKey),
	m_generation(generation), m_internalEpoch(internalEpoch),
	m_executionIdentity(executionIdentity)
{
}

SimulationPhaseId SimulationPhaseJobContext::phaseId() const
{
	return m_phaseId;
}

SimulationPhaseJobKey SimulationPhaseJobContext::jobKey() const
{
	return m_jobKey;
}

unsigned SimulationPhaseJobContext::generation() const
{
	return m_generation;
}

unsigned SimulationPhaseJobContext::internalEpoch() const
{
	return m_internalEpoch;
}

const SimulationPhaseExecutionIdentity &
SimulationPhaseJobContext::executionIdentity() const
{
	return m_executionIdentity;
}

bool SimulationPhaseJobContext::isCancellationRequested() const
{
	return m_graph == 0 || m_graph->cancellationRequested(m_generation,
		m_internalEpoch);
}

SimulationPhaseCommitContext::SimulationPhaseCommitContext(
	SimulationPhaseId phaseId, SimulationPhaseJobKey jobKey,
	unsigned generation, unsigned internalEpoch,
	const SimulationPhaseExecutionIdentity &executionIdentity) :
	m_phaseId(phaseId), m_jobKey(jobKey), m_generation(generation),
	m_internalEpoch(internalEpoch),
	m_executionIdentity(executionIdentity)
{
}

SimulationPhaseId SimulationPhaseCommitContext::phaseId() const
{
	return m_phaseId;
}

SimulationPhaseJobKey SimulationPhaseCommitContext::jobKey() const
{
	return m_jobKey;
}

unsigned SimulationPhaseCommitContext::generation() const
{
	return m_generation;
}

unsigned SimulationPhaseCommitContext::internalEpoch() const
{
	return m_internalEpoch;
}

const SimulationPhaseExecutionIdentity &
SimulationPhaseCommitContext::executionIdentity() const
{
	return m_executionIdentity;
}

SimulationPhaseNodeStorage::SimulationPhaseNodeStorage() :
	m_id(SIMULATION_PHASE_INVALID_ID), m_dependencyMask(0),
	m_immutableInput(0), m_immutableInputBytes(0), m_firstJob(0),
	m_jobCount(0), m_state(SIMULATION_PHASE_NODE_BLOCKED)
{
}

SimulationPhaseJobStorage::SimulationPhaseJobStorage() :
	m_phaseId(SIMULATION_PHASE_INVALID_ID),
	m_key(SIMULATION_PHASE_INVALID_JOB_KEY), m_phaseOrdinal(0),
	m_privateOutput(0), m_privateOutputBytes(0), m_execute(0),
	m_validate(0), m_commit(0)
{
}

SimulationPhaseJobResultStorage::SimulationPhaseJobResultStorage() :
	m_state(SIMULATION_PHASE_JOB_PENDING), m_generation(0), m_internalEpoch(0),
	m_executionIdentity()
{
}

SimulationPhaseJobTicket::SimulationPhaseJobTicket() :
	m_jobOrdinal(0xffffffffu), m_phaseId(SIMULATION_PHASE_INVALID_ID),
	m_jobKey(SIMULATION_PHASE_INVALID_JOB_KEY), m_generation(0),
	m_internalEpoch(0)
{
}

bool SimulationPhaseJobTicket::isValid() const
{
	return m_jobOrdinal != 0xffffffffu &&
		m_phaseId != SIMULATION_PHASE_INVALID_ID &&
		m_jobKey != SIMULATION_PHASE_INVALID_JOB_KEY && m_generation != 0 &&
		m_internalEpoch != 0;
}

SimulationPhaseId SimulationPhaseJobTicket::phaseId() const
{
	return m_phaseId;
}

SimulationPhaseJobKey SimulationPhaseJobTicket::jobKey() const
{
	return m_jobKey;
}

unsigned SimulationPhaseJobTicket::generation() const
{
	return m_generation;
}

unsigned SimulationPhaseJobTicket::internalEpoch() const
{
	return m_internalEpoch;
}

SimulationPhaseJobSnapshot::SimulationPhaseJobSnapshot() :
	state(SIMULATION_PHASE_JOB_PENDING), generation(0), internalEpoch(0),
	executionIdentity()
{
}

SimulationPhaseGraph::SimulationPhaseGraph(
	SimulationPhaseNodeStorage *nodeStorage, unsigned nodeCapacity,
	SimulationPhaseJobStorage *jobStorage, unsigned jobCapacity,
	SimulationPhaseJobResultStorage *resultStorage, unsigned resultCapacity,
	SimulationPhaseIsOwnerFunction isOwnerFunction, void *ownerContext) :
	m_nodes(nodeStorage), m_jobs(jobStorage), m_results(resultStorage),
	m_nodeCapacity(nodeCapacity), m_jobCapacity(jobCapacity),
	m_resultCapacity(resultCapacity), m_phaseCount(0), m_jobCount(0),
	m_generation(0), m_lastGeneration(0), m_internalEpoch(0),
	m_nextCommitPhase(0), m_committedPhaseMask(0),
	m_controlLock(0), m_activeExecutors(0), m_advanceOwnerActive(0),
	m_reentrantAdvanceDetected(0), m_terminalStatusMask(0),
	m_state(SIMULATION_PHASE_GRAPH_UNCONFIGURED),
	m_publicationOutcome(SIMULATION_PHASE_OUTCOME_INACTIVE),
	m_isOwner(isOwnerFunction), m_ownerContext(ownerContext)
{
}

bool SimulationPhaseGraph::isOwner() const
{
	return m_isOwner != 0 && m_isOwner(m_ownerContext);
}

SimulationPhaseGraphConfigurationStatus SimulationPhaseGraph::configure(
	const SimulationPhaseDefinition *phases, unsigned phaseCount,
	const SimulationPhaseJobDefinition *jobs, unsigned jobCount)
{
	if (!isOwner()) return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
	ScopedAtomicLock controlLock(&m_controlLock);
	if (phases == 0 || phaseCount == 0 ||
		(jobs == 0 && jobCount != 0) || m_nodes == 0 || m_jobs == 0 ||
		m_results == 0)
	{
		return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
	}
	if (!isQuiescentUnlocked())
		return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
	const SimulationPhaseGraphState current = state();
	if (current != SIMULATION_PHASE_GRAPH_UNCONFIGURED &&
		current != SIMULATION_PHASE_GRAPH_CONFIGURED &&
		current != SIMULATION_PHASE_GRAPH_COMPLETED &&
		current != SIMULATION_PHASE_GRAPH_CANCELLED &&
		current != SIMULATION_PHASE_GRAPH_FAILED &&
		current != SIMULATION_PHASE_GRAPH_STALE_GENERATION)
	{
		return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
	}
	if (phaseCount > SIMULATION_PHASE_GRAPH_MAX_PHASES)
		return SIMULATION_PHASE_GRAPH_TOO_MANY_PHASES;
	if (phaseCount > m_nodeCapacity || jobCount > m_jobCapacity ||
		jobCount > m_resultCapacity)
	{
		return SIMULATION_PHASE_GRAPH_STORAGE_TOO_SMALL;
	}
	if (!advanceInternalEpoch())
		return SIMULATION_PHASE_GRAPH_EPOCH_EXHAUSTED;

	AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_UNCONFIGURED);
	m_phaseCount = 0;
	m_jobCount = 0;
	m_generation = 0;
	m_nextCommitPhase = 0;
	m_committedPhaseMask = 0;
	AtomicStore(&m_publicationOutcome,
		SIMULATION_PHASE_OUTCOME_INACTIVE);
	AtomicStore(&m_terminalStatusMask, 0);

	ByteSpan nodeStorageSpan;
	ByteSpan jobStorageSpan;
	ByteSpan resultStorageSpan;
	ByteSpan phaseDefinitionSpan;
	ByteSpan jobDefinitionSpan;
	if (!MakeElementSpan(m_nodes, m_nodeCapacity,
		sizeof(SimulationPhaseNodeStorage), nodeStorageSpan) ||
		!MakeElementSpan(m_jobs, m_jobCapacity,
		sizeof(SimulationPhaseJobStorage), jobStorageSpan) ||
		!MakeElementSpan(m_results, m_resultCapacity,
		sizeof(SimulationPhaseJobResultStorage), resultStorageSpan) ||
		!MakeElementSpan(phases, phaseCount,
		sizeof(SimulationPhaseDefinition), phaseDefinitionSpan) ||
		!MakeElementSpan(jobs, jobCount,
		sizeof(SimulationPhaseJobDefinition), jobDefinitionSpan) ||
		SpansOverlap(nodeStorageSpan, jobStorageSpan) ||
		SpansOverlap(nodeStorageSpan, resultStorageSpan) ||
		SpansOverlap(jobStorageSpan, resultStorageSpan) ||
		SpansOverlap(nodeStorageSpan, phaseDefinitionSpan) ||
		SpansOverlap(nodeStorageSpan, jobDefinitionSpan) ||
		SpansOverlap(jobStorageSpan, phaseDefinitionSpan) ||
		SpansOverlap(jobStorageSpan, jobDefinitionSpan) ||
		SpansOverlap(resultStorageSpan, phaseDefinitionSpan) ||
		SpansOverlap(resultStorageSpan, jobDefinitionSpan))
	{
		return SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
	}

	unsigned sourceDependencyMasks[SIMULATION_PHASE_GRAPH_MAX_PHASES] = { 0 };
	unsigned phaseIndex;
	for (phaseIndex = 0; phaseIndex < phaseCount; ++phaseIndex)
	{
		const SimulationPhaseDefinition &phase = phases[phaseIndex];
		if (phase.id == SIMULATION_PHASE_INVALID_ID ||
			(phase.dependencyCount != 0 && phase.dependencies == 0))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
		}
		if (!IsPointerSizePairValid(phase.immutableInput,
			phase.immutableInputBytes))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_INPUT_OWNERSHIP;
		}
		ByteSpan inputSpan;
		if (!MakeByteSpan(phase.immutableInput,
			phase.immutableInputBytes, inputSpan))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_INPUT_OWNERSHIP;
		}
		if (SpansOverlap(inputSpan, nodeStorageSpan) ||
			SpansOverlap(inputSpan, jobStorageSpan) ||
			SpansOverlap(inputSpan, resultStorageSpan))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
		}
		ByteSpan dependencySpan;
		if (!MakeElementSpan(phase.dependencies, phase.dependencyCount,
			sizeof(SimulationPhaseId), dependencySpan))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
		}
		if (SpansOverlap(dependencySpan, nodeStorageSpan) ||
			SpansOverlap(dependencySpan, jobStorageSpan) ||
			SpansOverlap(dependencySpan, resultStorageSpan))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
		}
		unsigned otherPhase;
		for (otherPhase = 0; otherPhase < phaseIndex; ++otherPhase)
		{
			if (phases[otherPhase].id == phase.id)
				return SIMULATION_PHASE_GRAPH_DUPLICATE_PHASE_ID;
		}
		unsigned dependencyIndex;
		for (dependencyIndex = 0;
			dependencyIndex < phase.dependencyCount; ++dependencyIndex)
		{
			const SimulationPhaseId dependency =
				phase.dependencies[dependencyIndex];
			bool found = false;
			for (otherPhase = 0; otherPhase < phaseCount; ++otherPhase)
			{
				if (phases[otherPhase].id == dependency)
				{
					found = true;
					break;
				}
			}
			if (!found)
				return SIMULATION_PHASE_GRAPH_UNKNOWN_DEPENDENCY;
			if (dependency == phase.id)
				return SIMULATION_PHASE_GRAPH_CYCLE;
			unsigned earlierDependency;
			for (earlierDependency = 0;
				earlierDependency < dependencyIndex; ++earlierDependency)
			{
				if (phase.dependencies[earlierDependency] == dependency)
					return SIMULATION_PHASE_GRAPH_DUPLICATE_DEPENDENCY;
			}
			sourceDependencyMasks[phaseIndex] |= (1u << otherPhase);
		}
	}

	// Validate every output against every immutable phase input before any
	// graph storage is populated. This is the complete concurrency alias
	// matrix, not merely the producing phase's input.
	unsigned jobIndex;
	for (jobIndex = 0; jobIndex < jobCount; ++jobIndex)
	{
		const SimulationPhaseJobDefinition &job = jobs[jobIndex];
		bool phaseFound = false;
		for (phaseIndex = 0; phaseIndex < phaseCount; ++phaseIndex)
		{
			if (phases[phaseIndex].id == job.phaseId)
			{
				phaseFound = true;
				break;
			}
		}
		if (!phaseFound)
			return SIMULATION_PHASE_GRAPH_UNKNOWN_JOB_PHASE;
		if (job.key == SIMULATION_PHASE_INVALID_JOB_KEY)
			return SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
		if (job.execute == 0 || job.validate == 0 || job.commit == 0)
		{
			return SIMULATION_PHASE_GRAPH_MISSING_JOB_CALLBACK;
		}
		if (job.privateOutput == 0 || job.privateOutputBytes == 0)
			return SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP;

		ByteSpan outputSpan;
		if (!MakeByteSpan(job.privateOutput, job.privateOutputBytes,
			outputSpan))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP;
		}
		if (SpansOverlap(outputSpan, nodeStorageSpan) ||
			SpansOverlap(outputSpan, jobStorageSpan) ||
			SpansOverlap(outputSpan, resultStorageSpan))
		{
			return SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
		}
		for (phaseIndex = 0; phaseIndex < phaseCount; ++phaseIndex)
		{
			if (RangesOverlap(job.privateOutput, job.privateOutputBytes,
				phases[phaseIndex].immutableInput,
				phases[phaseIndex].immutableInputBytes))
			{
				return SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP;
			}
		}
		unsigned earlierJob;
		for (earlierJob = 0; earlierJob < jobIndex; ++earlierJob)
		{
			if (jobs[earlierJob].phaseId == job.phaseId &&
				jobs[earlierJob].key == job.key)
			{
				return SIMULATION_PHASE_GRAPH_DUPLICATE_JOB_KEY;
			}
			if (RangesOverlap(job.privateOutput, job.privateOutputBytes,
				jobs[earlierJob].privateOutput,
				jobs[earlierJob].privateOutputBytes))
			{
				return SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP;
			}
		}
	}

	// Build the stable topology from the copied dependency masks before writing
	// any persistent caller storage. Caller dependency arrays are never reread.
	unsigned orderedSourceIndices[SIMULATION_PHASE_GRAPH_MAX_PHASES];
	SimulationPhaseId orderedPhaseIds[SIMULATION_PHASE_GRAPH_MAX_PHASES];
	unsigned orderedDependencyMasks[SIMULATION_PHASE_GRAPH_MAX_PHASES];
	bool phaseSelected[SIMULATION_PHASE_GRAPH_MAX_PHASES] = { false };
	unsigned selectedSourceMask = 0;

	// Stable Kahn ordering: the smallest explicit phase ID wins every tie.
	unsigned ordinal;
	for (ordinal = 0; ordinal < phaseCount; ++ordinal)
	{
		unsigned chosen = 0xffffffffu;
		for (phaseIndex = 0; phaseIndex < phaseCount; ++phaseIndex)
		{
			if (phaseSelected[phaseIndex]) continue;
			const bool dependenciesSelected =
				(sourceDependencyMasks[phaseIndex] & selectedSourceMask) ==
					sourceDependencyMasks[phaseIndex];
			if (dependenciesSelected &&
				(chosen == 0xffffffffu ||
				 phases[phaseIndex].id < phases[chosen].id))
			{
				chosen = phaseIndex;
			}
		}
		if (chosen == 0xffffffffu)
			return SIMULATION_PHASE_GRAPH_CYCLE;

		phaseSelected[chosen] = true;
		selectedSourceMask |= (1u << chosen);
		orderedSourceIndices[ordinal] = chosen;
		orderedPhaseIds[ordinal] = phases[chosen].id;
		orderedDependencyMasks[ordinal] = 0;
		unsigned dependencyOrdinal;
		for (dependencyOrdinal = 0; dependencyOrdinal < ordinal;
			++dependencyOrdinal)
		{
			if ((sourceDependencyMasks[chosen] &
				(1u << orderedSourceIndices[dependencyOrdinal])) != 0)
			{
				orderedDependencyMasks[ordinal] |=
					(1u << dependencyOrdinal);
			}
		}
	}

	for (ordinal = 0; ordinal < phaseCount; ++ordinal)
	{
		const SimulationPhaseDefinition &phase =
			phases[orderedSourceIndices[ordinal]];
		SimulationPhaseNodeStorage &node = m_nodes[ordinal];
		node.m_id = orderedPhaseIds[ordinal];
		node.m_dependencyMask = orderedDependencyMasks[ordinal];
		node.m_immutableInput = phase.immutableInput;
		node.m_immutableInputBytes = phase.immutableInputBytes;
		node.m_firstJob = jobCount;
		node.m_jobCount = 0;
		AtomicStore(&node.m_state, SIMULATION_PHASE_NODE_BLOCKED);
	}
	m_phaseCount = phaseCount;

	for (jobIndex = 0; jobIndex < jobCount; ++jobIndex)
	{
		const SimulationPhaseJobDefinition &job = jobs[jobIndex];
		const unsigned phaseOrdinal = findPhaseOrdinal(job.phaseId);

		SimulationPhaseJobStorage &copy = m_jobs[jobIndex];
		copy.m_phaseId = job.phaseId;
		copy.m_key = job.key;
		copy.m_phaseOrdinal = phaseOrdinal;
		copy.m_privateOutput = job.privateOutput;
		copy.m_privateOutputBytes = job.privateOutputBytes;
		copy.m_execute = job.execute;
		copy.m_validate = job.validate;
		copy.m_commit = job.commit;
	}

	// Stable canonical job layout. Keys are unique, so selection sort has no
	// finish-order or caller-order tie to preserve.
	for (ordinal = 0; ordinal < jobCount; ++ordinal)
	{
		unsigned best = ordinal;
		for (jobIndex = ordinal + 1; jobIndex < jobCount; ++jobIndex)
		{
			if (m_jobs[jobIndex].m_phaseOrdinal <
				m_jobs[best].m_phaseOrdinal ||
				(m_jobs[jobIndex].m_phaseOrdinal ==
				 m_jobs[best].m_phaseOrdinal &&
				 m_jobs[jobIndex].m_key < m_jobs[best].m_key))
			{
				best = jobIndex;
			}
		}
		if (best != ordinal)
		{
			const SimulationPhaseJobStorage temporary = m_jobs[ordinal];
			m_jobs[ordinal] = m_jobs[best];
			m_jobs[best] = temporary;
		}
	}

	for (ordinal = 0; ordinal < phaseCount; ++ordinal)
	{
		m_nodes[ordinal].m_firstJob = jobCount;
		m_nodes[ordinal].m_jobCount = 0;
	}
	for (jobIndex = 0; jobIndex < jobCount; ++jobIndex)
	{
		SimulationPhaseNodeStorage &node =
			m_nodes[m_jobs[jobIndex].m_phaseOrdinal];
		if (node.m_jobCount == 0) node.m_firstJob = jobIndex;
		++node.m_jobCount;
		AtomicStore(&m_results[jobIndex].m_state,
			SIMULATION_PHASE_JOB_PENDING);
		m_results[jobIndex].m_generation = 0;
		m_results[jobIndex].m_internalEpoch = m_internalEpoch;
		m_results[jobIndex].m_executionIdentity =
			SimulationPhaseExecutionIdentity();
	}

	m_jobCount = jobCount;
	AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_CONFIGURED);
	return SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID;
}

bool SimulationPhaseGraph::reset(unsigned generationValue)
{
	if (!isOwner()) return false;
	ScopedAtomicLock controlLock(&m_controlLock);
	if (generationValue == 0 ||
		generationValue <= m_lastGeneration || m_phaseCount == 0 ||
		!isQuiescentUnlocked())
	{
		return false;
	}
	const SimulationPhaseGraphState current = state();
	if (current != SIMULATION_PHASE_GRAPH_CONFIGURED &&
		current != SIMULATION_PHASE_GRAPH_COMPLETED &&
		current != SIMULATION_PHASE_GRAPH_CANCELLED &&
		current != SIMULATION_PHASE_GRAPH_FAILED &&
		current != SIMULATION_PHASE_GRAPH_STALE_GENERATION)
	{
		return false;
	}
	if (!advanceInternalEpoch()) return false;

	m_generation = generationValue;
	m_lastGeneration = generationValue;
	m_nextCommitPhase = 0;
	m_committedPhaseMask = 0;
	AtomicStore(&m_reentrantAdvanceDetected, 0);
	AtomicStore(&m_terminalStatusMask, 0);
	AtomicStore(&m_publicationOutcome, SIMULATION_PHASE_OUTCOME_OPEN);
	unsigned phaseOrdinal;
	for (phaseOrdinal = 0; phaseOrdinal < m_phaseCount; ++phaseOrdinal)
	{
		AtomicStore(&m_nodes[phaseOrdinal].m_state,
			m_nodes[phaseOrdinal].m_dependencyMask == 0 ?
			SIMULATION_PHASE_NODE_READY : SIMULATION_PHASE_NODE_BLOCKED);
	}
	unsigned jobOrdinal;
	for (jobOrdinal = 0; jobOrdinal < m_jobCount; ++jobOrdinal)
	{
		m_results[jobOrdinal].m_generation = generationValue;
		m_results[jobOrdinal].m_internalEpoch = m_internalEpoch;
		m_results[jobOrdinal].m_executionIdentity =
			SimulationPhaseExecutionIdentity();
		AtomicStore(&m_results[jobOrdinal].m_state,
			SIMULATION_PHASE_JOB_PENDING);
	}
	AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_READY);
	return true;
}

bool SimulationPhaseGraph::tryClaimReadyJob(SimulationPhaseJobTicket &ticket)
{
	ticket = SimulationPhaseJobTicket();
	if (!isOwner() || AtomicLoad(&m_publicationOutcome) !=
		SIMULATION_PHASE_OUTCOME_OPEN)
		return false;
	const SimulationPhaseGraphState graphState = state();
	if (graphState != SIMULATION_PHASE_GRAPH_READY &&
		graphState != SIMULATION_PHASE_GRAPH_RUNNING)
	{
		return false;
	}

	unsigned jobOrdinal;
	for (jobOrdinal = 0; jobOrdinal < m_jobCount; ++jobOrdinal)
	{
		const unsigned phaseOrdinal = m_jobs[jobOrdinal].m_phaseOrdinal;
		const SimulationPhaseNodeState nodeState =
			static_cast<SimulationPhaseNodeState>(
				AtomicLoad(&m_nodes[phaseOrdinal].m_state));
		if (nodeState != SIMULATION_PHASE_NODE_READY &&
			nodeState != SIMULATION_PHASE_NODE_RUNNING)
		{
			continue;
		}
		if (!AtomicCompareExchange(&m_results[jobOrdinal].m_state,
			SIMULATION_PHASE_JOB_PENDING, SIMULATION_PHASE_JOB_CLAIMED))
		{
			continue;
		}
		AtomicStore(&m_nodes[phaseOrdinal].m_state,
			SIMULATION_PHASE_NODE_RUNNING);
		AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_RUNNING);
		ticket.m_jobOrdinal = jobOrdinal;
		ticket.m_phaseId = m_jobs[jobOrdinal].m_phaseId;
		ticket.m_jobKey = m_jobs[jobOrdinal].m_key;
		ticket.m_generation = m_generation;
		ticket.m_internalEpoch = m_internalEpoch;
		return true;
	}
	return false;
}

SimulationPhaseWorkStatus SimulationPhaseGraph::executeClaimedJob(
	const SimulationPhaseJobTicket &ticket,
	const SimulationPhaseExecutionIdentity &executionIdentity)
{
	if (!ticket.isValid())
		return SIMULATION_PHASE_WORK_STALE_GENERATION;
#if defined(RTS_BUILD_CORE_EXTRAS)
	if (g_executeAdmissionHook != 0)
		g_executeAdmissionHook(g_executeAdmissionHookContext);
#endif
	ScopedAtomicLock controlLock(&m_controlLock);
	ScopedAtomicCounter activeExecutor(&m_activeExecutors);
	if (ticket.m_jobOrdinal >= m_jobCount ||
		ticket.m_generation != m_generation ||
		ticket.m_internalEpoch != m_internalEpoch)
	{
		return SIMULATION_PHASE_WORK_STALE_GENERATION;
	}
	SimulationPhaseJobStorage &job = m_jobs[ticket.m_jobOrdinal];
	SimulationPhaseJobResultStorage &result = m_results[ticket.m_jobOrdinal];
	if (ticket.m_phaseId != job.m_phaseId || ticket.m_jobKey != job.m_key ||
		result.m_generation != ticket.m_generation ||
		result.m_internalEpoch != ticket.m_internalEpoch)
	{
		return SIMULATION_PHASE_WORK_STALE_GENERATION;
	}
	if (!AtomicCompareExchange(&result.m_state,
		SIMULATION_PHASE_JOB_CLAIMED, SIMULATION_PHASE_JOB_RUNNING))
	{
		return WorkStatusForJobState(static_cast<SimulationPhaseJobState>(
			AtomicLoad(&result.m_state)));
	}
	controlLock.release();

	result.m_executionIdentity = executionIdentity;
	SimulationPhaseWorkStatus workStatus = SIMULATION_PHASE_WORK_FAILED;
	if (!executionIdentity.isValid())
	{
		workStatus = SIMULATION_PHASE_WORK_FAILED;
	}
	else if (cancellationRequested(ticket.m_generation,
		ticket.m_internalEpoch))
	{
		workStatus = SIMULATION_PHASE_WORK_CANCELLED;
	}
	else
	{
		const SimulationPhaseNodeStorage &phase =
			m_nodes[job.m_phaseOrdinal];
		const SimulationPhaseJobContext context(this, job.m_phaseId,
			job.m_key, ticket.m_generation, ticket.m_internalEpoch,
			executionIdentity);
		try
		{
			workStatus = job.m_execute(context, phase.m_immutableInput,
				phase.m_immutableInputBytes, job.m_privateOutput,
				job.m_privateOutputBytes);
		}
		catch (...)
		{
			workStatus = SIMULATION_PHASE_WORK_FAILED;
		}
		if (workStatus != SIMULATION_PHASE_WORK_SUCCEEDED &&
			workStatus != SIMULATION_PHASE_WORK_FAILED &&
			workStatus != SIMULATION_PHASE_WORK_CANCELLED &&
			workStatus != SIMULATION_PHASE_WORK_STALE_GENERATION)
		{
			workStatus = SIMULATION_PHASE_WORK_FAILED;
		}
		if (workStatus == SIMULATION_PHASE_WORK_SUCCEEDED &&
			cancellationRequested(ticket.m_generation,
				ticket.m_internalEpoch))
			workStatus = SIMULATION_PHASE_WORK_CANCELLED;
	}
	if (workStatus != SIMULATION_PHASE_WORK_SUCCEEDED)
		recordTerminalCause(workStatus);
	AtomicStore(&result.m_state, JobStateForWorkStatus(workStatus));
	return workStatus;
}

bool SimulationPhaseGraph::advanceOwner(unsigned *committedJobCount)
{
	if (committedJobCount != 0) *committedJobCount = 0;
	if (!isOwner()) return false;
	SimulationPhaseGraphState graphState = state();
	if (graphState != SIMULATION_PHASE_GRAPH_READY &&
		graphState != SIMULATION_PHASE_GRAPH_RUNNING)
	{
		return graphState == SIMULATION_PHASE_GRAPH_COMPLETED ||
			graphState == SIMULATION_PHASE_GRAPH_CANCELLED ||
			graphState == SIMULATION_PHASE_GRAPH_FAILED ||
			graphState == SIMULATION_PHASE_GRAPH_STALE_GENERATION;
	}
	AtomicStore(&m_reentrantAdvanceDetected, 0);
	if (!AtomicCompareExchange(&m_advanceOwnerActive, 0, 1))
	{
		AtomicStore(&m_reentrantAdvanceDetected, 1);
		recordTerminalCause(SIMULATION_PHASE_WORK_FAILED);
		return false;
	}
	ScopedAtomicFlagRelease advanceGuard(&m_advanceOwnerActive);

	if (IsTerminalOutcome(AtomicLoad(&m_publicationOutcome)))
	{
		finishRecordedTerminalIfQuiescent();
		return true;
	}
	unsigned committed = 0;
	while (m_nextCommitPhase < m_phaseCount)
	{
		const unsigned phaseOrdinal = m_nextCommitPhase;
		SimulationPhaseNodeStorage &phase = m_nodes[phaseOrdinal];
		if ((phase.m_dependencyMask & m_committedPhaseMask) !=
			phase.m_dependencyMask)
		{
			break;
		}

		bool allTerminal = true;
		SimulationPhaseWorkStatus terminalStatus =
			SIMULATION_PHASE_WORK_SUCCEEDED;
		unsigned terminalJobOrdinal = 0xffffffffu;
		unsigned localJob;
		for (localJob = 0; localJob < phase.m_jobCount; ++localJob)
		{
			const unsigned jobOrdinal = phase.m_firstJob + localJob;
			const SimulationPhaseJobState jobState =
				static_cast<SimulationPhaseJobState>(
					AtomicLoad(&m_results[jobOrdinal].m_state));
			if (!IsTerminalJobState(jobState))
			{
				allTerminal = false;
				break;
			}
			if (terminalStatus == SIMULATION_PHASE_WORK_SUCCEEDED &&
				jobState != SIMULATION_PHASE_JOB_SUCCEEDED &&
				jobState != SIMULATION_PHASE_JOB_COMMITTED)
			{
				terminalStatus = WorkStatusForJobState(jobState);
				terminalJobOrdinal = jobOrdinal;
			}
		}
		if (!allTerminal) break;
		if (IsTerminalOutcome(AtomicLoad(&m_publicationOutcome)))
		{
			finishRecordedTerminalIfQuiescent();
			break;
		}
		if (terminalStatus != SIMULATION_PHASE_WORK_SUCCEEDED)
		{
			finishTerminal(terminalStatus, phaseOrdinal,
				terminalJobOrdinal);
			break;
		}

		// Validate the entire phase before the first canonical publication.
		for (localJob = 0; localJob < phase.m_jobCount; ++localJob)
		{
			const unsigned jobOrdinal = phase.m_firstJob + localJob;
			SimulationPhaseJobStorage &job = m_jobs[jobOrdinal];
			const SimulationPhaseCommitContext context(job.m_phaseId,
				job.m_key, m_generation, m_internalEpoch,
				m_results[jobOrdinal].m_executionIdentity);
			SimulationPhaseWorkStatus validationStatus =
				SIMULATION_PHASE_WORK_FAILED;
			try
			{
				validationStatus = job.m_validate(context,
					phase.m_immutableInput, phase.m_immutableInputBytes,
					job.m_privateOutput, job.m_privateOutputBytes,
					m_ownerContext);
			}
			catch (...)
			{
				validationStatus = SIMULATION_PHASE_WORK_FAILED;
			}
			if (AtomicLoad(&m_reentrantAdvanceDetected) != 0)
			{
				AtomicStore(&m_results[jobOrdinal].m_state,
					SIMULATION_PHASE_JOB_FAILED);
				finishTerminal(SIMULATION_PHASE_WORK_FAILED,
					phaseOrdinal, jobOrdinal);
				if (committedJobCount != 0)
					*committedJobCount = committed;
				return true;
			}
			if (validationStatus != SIMULATION_PHASE_WORK_SUCCEEDED)
			{
				if (validationStatus != SIMULATION_PHASE_WORK_CANCELLED &&
					validationStatus !=
						SIMULATION_PHASE_WORK_STALE_GENERATION)
				{
					validationStatus = SIMULATION_PHASE_WORK_FAILED;
				}
				AtomicStore(&m_results[jobOrdinal].m_state,
					JobStateForWorkStatus(validationStatus));
				finishTerminal(validationStatus, phaseOrdinal, jobOrdinal);
				if (committedJobCount != 0)
					*committedJobCount = committed;
				return true;
			}
		}

		// The final phase and cancellation contend for one atomic publication
		// outcome. Earlier phases retain the same acquire/full-barrier gate.
		const bool finalPhase = m_nextCommitPhase + 1 == m_phaseCount;
		if (finalPhase)
		{
			if (!AtomicCompareExchange(&m_publicationOutcome,
				SIMULATION_PHASE_OUTCOME_OPEN,
				SIMULATION_PHASE_OUTCOME_COMPLETED))
			{
				finishRecordedTerminalIfQuiescent();
				if (committedJobCount != 0)
					*committedJobCount = committed;
				return true;
			}
		}
		else if (AtomicLoad(&m_publicationOutcome) !=
			SIMULATION_PHASE_OUTCOME_OPEN)
		{
			finishRecordedTerminalIfQuiescent();
			if (committedJobCount != 0)
				*committedJobCount = committed;
			return true;
		}

		for (localJob = 0; localJob < phase.m_jobCount; ++localJob)
		{
			const unsigned jobOrdinal = phase.m_firstJob + localJob;
			SimulationPhaseJobStorage &job = m_jobs[jobOrdinal];
			const SimulationPhaseCommitContext context(job.m_phaseId,
				job.m_key, m_generation, m_internalEpoch,
				m_results[jobOrdinal].m_executionIdentity);
			try
			{
				job.m_commit(context, phase.m_immutableInput,
					phase.m_immutableInputBytes, job.m_privateOutput,
					job.m_privateOutputBytes, m_ownerContext);
			}
			catch (...)
			{
				AtomicStore(&m_results[jobOrdinal].m_state,
					SIMULATION_PHASE_JOB_FAILED);
				finishTerminal(SIMULATION_PHASE_WORK_FAILED,
					phaseOrdinal, jobOrdinal);
				if (committedJobCount != 0)
					*committedJobCount = committed;
				return true;
			}
			if (AtomicLoad(&m_reentrantAdvanceDetected) != 0)
			{
				AtomicStore(&m_results[jobOrdinal].m_state,
					SIMULATION_PHASE_JOB_FAILED);
				finishTerminal(SIMULATION_PHASE_WORK_FAILED,
					phaseOrdinal, jobOrdinal);
				if (committedJobCount != 0)
					*committedJobCount = committed;
				return true;
			}
			AtomicStore(&m_results[jobOrdinal].m_state,
				SIMULATION_PHASE_JOB_COMMITTED);
			++committed;
		}
		AtomicStore(&phase.m_state, SIMULATION_PHASE_NODE_COMMITTED);
		m_committedPhaseMask |= (1u << phaseOrdinal);
		++m_nextCommitPhase;
		refreshReadyPhases();
	}

	if (m_nextCommitPhase == m_phaseCount &&
		AtomicLoad(&m_publicationOutcome) ==
			SIMULATION_PHASE_OUTCOME_COMPLETED)
		AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_COMPLETED);
	else if (state() == SIMULATION_PHASE_GRAPH_READY)
		AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_RUNNING);
	if (committedJobCount != 0) *committedJobCount = committed;
	return true;
}

void SimulationPhaseGraph::requestCancellation()
{
	recordTerminalCause(SIMULATION_PHASE_WORK_CANCELLED);
}

SimulationPhaseGraphState SimulationPhaseGraph::state() const
{
	return static_cast<SimulationPhaseGraphState>(AtomicLoad(&m_state));
}

unsigned SimulationPhaseGraph::generation() const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	return m_generation;
}

unsigned SimulationPhaseGraph::internalEpoch() const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	return m_internalEpoch;
}

SimulationPhaseWorkStatus SimulationPhaseGraph::terminalCause() const
{
	long terminalMask = AtomicLoad(&m_terminalStatusMask);
	if (terminalMask != 0) return WorkStatusForTerminalMask(terminalMask);
	const long outcome = AtomicLoad(&m_publicationOutcome);
	// A worker publishes its status bit before closing the outcome, while
	// cancellation closes the outcome before publishing its bit. The second
	// mask read makes both orders one coherent diagnostic observation.
	terminalMask = AtomicLoad(&m_terminalStatusMask);
	return terminalMask != 0 ? WorkStatusForTerminalMask(terminalMask) :
		WorkStatusForPublicationOutcome(outcome);
}

unsigned SimulationPhaseGraph::phaseCount() const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	return m_phaseCount;
}

unsigned SimulationPhaseGraph::jobCount() const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	return m_jobCount;
}

bool SimulationPhaseGraph::isQuiescent() const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	return isQuiescentUnlocked();
}

bool SimulationPhaseGraph::isQuiescentUnlocked() const
{
	if (AtomicLoad(&m_activeExecutors) != 0) return false;
	unsigned jobOrdinal;
	for (jobOrdinal = 0; jobOrdinal < m_jobCount; ++jobOrdinal)
	{
		const SimulationPhaseJobState jobState =
			static_cast<SimulationPhaseJobState>(
				AtomicLoad(&m_results[jobOrdinal].m_state));
		if (jobState == SIMULATION_PHASE_JOB_CLAIMED ||
			jobState == SIMULATION_PHASE_JOB_RUNNING)
		{
			return false;
		}
	}
	return true;
}

SimulationPhaseId SimulationPhaseGraph::phaseIdAt(
	unsigned topologicalOrdinal) const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	return topologicalOrdinal < m_phaseCount ?
		m_nodes[topologicalOrdinal].m_id : SIMULATION_PHASE_INVALID_ID;
}

SimulationPhaseNodeState SimulationPhaseGraph::phaseState(
	SimulationPhaseId phaseId) const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	const unsigned ordinal = findPhaseOrdinal(phaseId);
	return ordinal < m_phaseCount ? static_cast<SimulationPhaseNodeState>(
		AtomicLoad(&m_nodes[ordinal].m_state)) : SIMULATION_PHASE_NODE_BLOCKED;
}

bool SimulationPhaseGraph::jobSnapshot(SimulationPhaseId phaseId,
	SimulationPhaseJobKey key, SimulationPhaseJobSnapshot &snapshot) const
{
	ScopedAtomicLock controlLock(&m_controlLock);
	const unsigned ordinal = findJobOrdinal(phaseId, key);
	if (ordinal >= m_jobCount) return false;
	snapshot.state = static_cast<SimulationPhaseJobState>(
		AtomicLoad(&m_results[ordinal].m_state));
	snapshot.generation = m_results[ordinal].m_generation;
	snapshot.internalEpoch = m_results[ordinal].m_internalEpoch;
	// The worker publishes identity immediately before its terminal release.
	// Do not race that write when diagnostics inspect a running job.
	if (IsTerminalJobState(snapshot.state))
	{
		snapshot.executionIdentity =
			m_results[ordinal].m_executionIdentity;
	}
	else
	{
		snapshot.executionIdentity = SimulationPhaseExecutionIdentity();
	}
	return true;
}

bool SimulationPhaseGraph::cancellationRequested(unsigned generationValue,
	unsigned internalEpochValue) const
{
	return generationValue != m_generation ||
		internalEpochValue != m_internalEpoch ||
		IsTerminalOutcome(AtomicLoad(&m_publicationOutcome));
}

#if defined(RTS_BUILD_CORE_EXTRAS)
bool SimulationPhaseGraph::forceInternalEpochExhaustionForTest()
{
	if (!isOwner()) return false;
	ScopedAtomicLock controlLock(&m_controlLock);
	if (!isQuiescentUnlocked()) return false;
	m_internalEpoch = ~0u;
	return true;
}
#endif

unsigned SimulationPhaseGraph::findPhaseOrdinal(
	SimulationPhaseId phaseId) const
{
	unsigned ordinal;
	for (ordinal = 0; ordinal < m_phaseCount; ++ordinal)
	{
		if (m_nodes[ordinal].m_id == phaseId) return ordinal;
	}
	return 0xffffffffu;
}

unsigned SimulationPhaseGraph::findJobOrdinal(SimulationPhaseId phaseId,
	SimulationPhaseJobKey key) const
{
	unsigned ordinal;
	for (ordinal = 0; ordinal < m_jobCount; ++ordinal)
	{
		if (m_jobs[ordinal].m_phaseId == phaseId &&
			m_jobs[ordinal].m_key == key)
		{
			return ordinal;
		}
	}
	return 0xffffffffu;
}

void SimulationPhaseGraph::refreshReadyPhases()
{
	unsigned ordinal;
	for (ordinal = 0; ordinal < m_phaseCount; ++ordinal)
	{
		if (AtomicLoad(&m_nodes[ordinal].m_state) ==
			SIMULATION_PHASE_NODE_BLOCKED &&
			(m_nodes[ordinal].m_dependencyMask & m_committedPhaseMask) ==
				m_nodes[ordinal].m_dependencyMask)
		{
			AtomicStore(&m_nodes[ordinal].m_state,
				SIMULATION_PHASE_NODE_READY);
		}
	}
}

void SimulationPhaseGraph::cancelInactiveJobs()
{
	unsigned ordinal;
	for (ordinal = 0; ordinal < m_jobCount; ++ordinal)
	{
		if (!AtomicCompareExchange(&m_results[ordinal].m_state,
			SIMULATION_PHASE_JOB_PENDING, SIMULATION_PHASE_JOB_CANCELLED))
		{
			AtomicCompareExchange(&m_results[ordinal].m_state,
				SIMULATION_PHASE_JOB_CLAIMED,
				SIMULATION_PHASE_JOB_CANCELLED);
		}
	}
}

void SimulationPhaseGraph::recordTerminalCause(
	SimulationPhaseWorkStatus status)
{
	if (status == SIMULATION_PHASE_WORK_SUCCEEDED) return;
	long outcome = AtomicLoad(&m_publicationOutcome);
	if (outcome == SIMULATION_PHASE_OUTCOME_INACTIVE) return;

	if (status == SIMULATION_PHASE_WORK_CANCELLED)
	{
		for (;;)
		{
			outcome = AtomicLoad(&m_publicationOutcome);
			if (outcome == SIMULATION_PHASE_OUTCOME_COMPLETED ||
				outcome == SIMULATION_PHASE_OUTCOME_INACTIVE)
			{
				return;
			}
			if (outcome == SIMULATION_PHASE_OUTCOME_OPEN)
			{
				if (!AtomicCompareExchange(&m_publicationOutcome,
					SIMULATION_PHASE_OUTCOME_OPEN,
					SIMULATION_PHASE_OUTCOME_CANCELLED))
				{
					continue;
				}
			}
			AtomicOr(&m_terminalStatusMask,
				SIMULATION_PHASE_TERMINAL_CANCELLED);
			return;
		}
	}

	AtomicOr(&m_terminalStatusMask, TerminalBitForWorkStatus(status));
	outcome = AtomicLoad(&m_publicationOutcome);
	if (outcome == SIMULATION_PHASE_OUTCOME_OPEN)
	{
		AtomicCompareExchange(&m_publicationOutcome,
			SIMULATION_PHASE_OUTCOME_OPEN, OutcomeForWorkStatus(status));
	}
	else if (outcome == SIMULATION_PHASE_OUTCOME_COMPLETED &&
		status == SIMULATION_PHASE_WORK_FAILED)
	{
		// The final publication gate can only be followed by the documented
		// non-throwing commit step. Preserve a caught contract violation as a
		// failure; cancellation still cannot supersede completion.
		AtomicCompareExchange(&m_publicationOutcome,
			SIMULATION_PHASE_OUTCOME_COMPLETED,
			SIMULATION_PHASE_OUTCOME_FAILED);
	}
}

void SimulationPhaseGraph::findTerminalSource(
	SimulationPhaseWorkStatus status, unsigned &phaseOrdinal,
	unsigned &jobOrdinal) const
{
	phaseOrdinal = 0xffffffffu;
	jobOrdinal = 0xffffffffu;
	SimulationPhaseJobState wantedState;
	if (status == SIMULATION_PHASE_WORK_FAILED)
		wantedState = SIMULATION_PHASE_JOB_FAILED;
	else if (status == SIMULATION_PHASE_WORK_STALE_GENERATION)
		wantedState = SIMULATION_PHASE_JOB_STALE_GENERATION;
	else
		return;

	// m_jobs is already in canonical phase-ID/job-key order. Selecting the
	// first matching terminal result makes simultaneous faults deterministic.
	unsigned ordinal;
	for (ordinal = 0; ordinal < m_jobCount; ++ordinal)
	{
		if (AtomicLoad(&m_results[ordinal].m_state) == wantedState)
		{
			phaseOrdinal = m_jobs[ordinal].m_phaseOrdinal;
			jobOrdinal = ordinal;
			return;
		}
	}
}

bool SimulationPhaseGraph::finishRecordedTerminalIfQuiescent()
{
	cancelInactiveJobs();
	if (!isQuiescent()) return false;
	SimulationPhaseWorkStatus status = terminalCause();
	if (status == SIMULATION_PHASE_WORK_SUCCEEDED)
		status = SIMULATION_PHASE_WORK_CANCELLED;
	unsigned phaseOrdinal;
	unsigned jobOrdinal;
	findTerminalSource(status, phaseOrdinal, jobOrdinal);
	finishTerminal(status, phaseOrdinal, jobOrdinal);
	return true;
}

bool SimulationPhaseGraph::advanceInternalEpoch()
{
	if (m_internalEpoch == ~0u)
	{
		AtomicStore(&m_terminalStatusMask,
			SIMULATION_PHASE_TERMINAL_FAILED);
		AtomicStore(&m_publicationOutcome,
			SIMULATION_PHASE_OUTCOME_FAILED);
		AtomicStore(&m_state, SIMULATION_PHASE_GRAPH_FAILED);
		return false;
	}
	++m_internalEpoch;
	return true;
}

void SimulationPhaseGraph::finishTerminal(SimulationPhaseWorkStatus status,
	unsigned phaseOrdinal, unsigned jobOrdinal)
{
	recordTerminalCause(status);
	const SimulationPhaseWorkStatus recordedCause = terminalCause();
	if (recordedCause != SIMULATION_PHASE_WORK_SUCCEEDED)
		status = recordedCause;
	cancelInactiveJobs();
	SimulationPhaseNodeState nodeState = SIMULATION_PHASE_NODE_FAILED;
	SimulationPhaseGraphState graphState = SIMULATION_PHASE_GRAPH_FAILED;
	if (status == SIMULATION_PHASE_WORK_CANCELLED)
	{
		nodeState = SIMULATION_PHASE_NODE_CANCELLED;
		graphState = SIMULATION_PHASE_GRAPH_CANCELLED;
	}
	else if (status == SIMULATION_PHASE_WORK_STALE_GENERATION)
	{
		nodeState = SIMULATION_PHASE_NODE_STALE_GENERATION;
		graphState = SIMULATION_PHASE_GRAPH_STALE_GENERATION;
	}
	if (jobOrdinal < m_jobCount)
		AtomicStore(&m_results[jobOrdinal].m_state,
			JobStateForWorkStatus(status));
	if (phaseOrdinal < m_phaseCount)
		AtomicStore(&m_nodes[phaseOrdinal].m_state, nodeState);
	unsigned ordinal;
	for (ordinal = 0; ordinal < m_phaseCount; ++ordinal)
	{
		if (ordinal != phaseOrdinal &&
			AtomicLoad(&m_nodes[ordinal].m_state) !=
			SIMULATION_PHASE_NODE_COMMITTED)
		{
			AtomicStore(&m_nodes[ordinal].m_state,
				SIMULATION_PHASE_NODE_CANCELLED);
		}
	}
	AtomicStore(&m_state, graphState);
}

} // namespace rts
