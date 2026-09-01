/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

namespace rts
{

typedef unsigned SimulationPhaseId;
typedef unsigned SimulationPhaseJobKey;

enum
{
	SIMULATION_PHASE_GRAPH_MAX_PHASES = 32
};

const unsigned SIMULATION_PHASE_INVALID_ID = ~0u;
const unsigned SIMULATION_PHASE_INVALID_JOB_KEY = ~0u;
const unsigned SIMULATION_PHASE_INVALID_WORKER_INDEX = ~0u;

enum SimulationPhaseExecutionKind
{
	SIMULATION_PHASE_EXECUTION_NONE = 0,
	SIMULATION_PHASE_EXECUTION_PHYSICAL_WORKER,
	SIMULATION_PHASE_EXECUTION_OWNER_HELP
};

// The scheduler adapter supplies execution identity. Owner help deliberately
// has no physical-worker index and can therefore never be misreported as
// physical worker execution.
class SimulationPhaseExecutionIdentity
{
public:
	SimulationPhaseExecutionIdentity();
	static SimulationPhaseExecutionIdentity physicalWorker(unsigned workerIndex);
	static SimulationPhaseExecutionIdentity ownerHelp();

	SimulationPhaseExecutionKind kind() const;
	unsigned physicalWorkerIndex() const;
	bool isValid() const;

private:
	SimulationPhaseExecutionKind m_kind;
	unsigned m_physicalWorkerIndex;
};

enum SimulationPhaseWorkStatus
{
	SIMULATION_PHASE_WORK_SUCCEEDED = 0,
	SIMULATION_PHASE_WORK_FAILED,
	SIMULATION_PHASE_WORK_CANCELLED,
	SIMULATION_PHASE_WORK_STALE_GENERATION
};

class SimulationPhaseGraph;

// Immutable metadata for one claimed job. isCancellationRequested is a
// nonblocking observation; workers never wait on dependent phases.
class SimulationPhaseJobContext
{
public:
	SimulationPhaseId phaseId() const;
	SimulationPhaseJobKey jobKey() const;
	unsigned generation() const;
	unsigned internalEpoch() const;
	const SimulationPhaseExecutionIdentity &executionIdentity() const;
	bool isCancellationRequested() const;

private:
	friend class SimulationPhaseGraph;
	SimulationPhaseJobContext(const SimulationPhaseGraph *graph,
		SimulationPhaseId phaseId, SimulationPhaseJobKey jobKey,
		unsigned generation, unsigned internalEpoch,
		const SimulationPhaseExecutionIdentity &executionIdentity);

	const SimulationPhaseGraph *m_graph;
	SimulationPhaseId m_phaseId;
	SimulationPhaseJobKey m_jobKey;
	unsigned m_generation;
	unsigned m_internalEpoch;
	SimulationPhaseExecutionIdentity m_executionIdentity;
};

// Owner validation and commit receive the worker identity that produced the
// output. A title adapter can reject owner-helped output before publication.
class SimulationPhaseCommitContext
{
public:
	SimulationPhaseId phaseId() const;
	SimulationPhaseJobKey jobKey() const;
	unsigned generation() const;
	unsigned internalEpoch() const;
	const SimulationPhaseExecutionIdentity &executionIdentity() const;

private:
	friend class SimulationPhaseGraph;
	SimulationPhaseCommitContext(SimulationPhaseId phaseId,
		SimulationPhaseJobKey jobKey, unsigned generation,
		unsigned internalEpoch,
		const SimulationPhaseExecutionIdentity &executionIdentity);

	SimulationPhaseId m_phaseId;
	SimulationPhaseJobKey m_jobKey;
	unsigned m_generation;
	unsigned m_internalEpoch;
	SimulationPhaseExecutionIdentity m_executionIdentity;
};

typedef SimulationPhaseWorkStatus (*SimulationPhaseExecuteFunction)(
	const SimulationPhaseJobContext &context,
	const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
	void *privateJobOutput, unsigned privateJobOutputBytes);

typedef SimulationPhaseWorkStatus (*SimulationPhaseValidateFunction)(
	const SimulationPhaseCommitContext &context,
	const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
	const void *privateJobOutput, unsigned privateJobOutputBytes,
	void *ownerContext);

// Validation is the fail-closed preflight. Once validation for every job in a
// phase succeeds, commit must be a non-throwing owner-only publication step.
typedef void (*SimulationPhaseCommitFunction)(
	const SimulationPhaseCommitContext &context,
	const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
	const void *privateJobOutput, unsigned privateJobOutputBytes,
	void *ownerContext);

typedef bool (*SimulationPhaseIsOwnerFunction)(void *ownerContext);

#if defined(RTS_BUILD_CORE_EXTRAS)
typedef void (*SimulationPhaseExecuteAdmissionHook)(void *context);
void setSimulationPhaseExecuteAdmissionHookForTest(
	SimulationPhaseExecuteAdmissionHook hook, void *context);
#endif

struct SimulationPhaseDefinition
{
	SimulationPhaseId id;
	const SimulationPhaseId *dependencies;
	unsigned dependencyCount;
	const void *immutableInput;
	unsigned immutableInputBytes;
};

struct SimulationPhaseJobDefinition
{
	SimulationPhaseId phaseId;
	SimulationPhaseJobKey key;
	void *privateOutput;
	unsigned privateOutputBytes;
	SimulationPhaseExecuteFunction execute;
	// Required. Every job in a phase is validated before its first commit.
	SimulationPhaseValidateFunction validate;
	SimulationPhaseCommitFunction commit;
};

enum SimulationPhaseGraphConfigurationStatus
{
	SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID = 0,
	SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT,
	SIMULATION_PHASE_GRAPH_STORAGE_TOO_SMALL,
	SIMULATION_PHASE_GRAPH_TOO_MANY_PHASES,
	SIMULATION_PHASE_GRAPH_DUPLICATE_PHASE_ID,
	SIMULATION_PHASE_GRAPH_UNKNOWN_DEPENDENCY,
	SIMULATION_PHASE_GRAPH_DUPLICATE_DEPENDENCY,
	SIMULATION_PHASE_GRAPH_CYCLE,
	SIMULATION_PHASE_GRAPH_UNKNOWN_JOB_PHASE,
	SIMULATION_PHASE_GRAPH_DUPLICATE_JOB_KEY,
	SIMULATION_PHASE_GRAPH_MISSING_JOB_CALLBACK,
	SIMULATION_PHASE_GRAPH_INVALID_INPUT_OWNERSHIP,
	SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP,
	SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP,
	SIMULATION_PHASE_GRAPH_EPOCH_EXHAUSTED
};

enum SimulationPhaseGraphState
{
	SIMULATION_PHASE_GRAPH_UNCONFIGURED = 0,
	SIMULATION_PHASE_GRAPH_CONFIGURED,
	SIMULATION_PHASE_GRAPH_READY,
	SIMULATION_PHASE_GRAPH_RUNNING,
	SIMULATION_PHASE_GRAPH_COMPLETED,
	SIMULATION_PHASE_GRAPH_CANCELLED,
	SIMULATION_PHASE_GRAPH_FAILED,
	SIMULATION_PHASE_GRAPH_STALE_GENERATION
};

enum SimulationPhaseNodeState
{
	SIMULATION_PHASE_NODE_BLOCKED = 0,
	SIMULATION_PHASE_NODE_READY,
	SIMULATION_PHASE_NODE_RUNNING,
	SIMULATION_PHASE_NODE_COMMITTED,
	SIMULATION_PHASE_NODE_CANCELLED,
	SIMULATION_PHASE_NODE_FAILED,
	SIMULATION_PHASE_NODE_STALE_GENERATION
};

enum SimulationPhaseJobState
{
	SIMULATION_PHASE_JOB_PENDING = 0,
	SIMULATION_PHASE_JOB_CLAIMED,
	SIMULATION_PHASE_JOB_RUNNING,
	SIMULATION_PHASE_JOB_SUCCEEDED,
	SIMULATION_PHASE_JOB_COMMITTED,
	SIMULATION_PHASE_JOB_CANCELLED,
	SIMULATION_PHASE_JOB_FAILED,
	SIMULATION_PHASE_JOB_STALE_GENERATION
};

// These three arrays are persistent caller-owned storage. configure copies the
// complete graph shape and job descriptors into them and performs no heap
// allocation. Their full capacity spans must be mutually disjoint and must not
// overlap immutable inputs or private outputs. reset reuses them for subsequent
// strictly increasing frame generations.
class SimulationPhaseNodeStorage
{
public:
	SimulationPhaseNodeStorage();

private:
	friend class SimulationPhaseGraph;
	SimulationPhaseId m_id;
	unsigned m_dependencyMask;
	const void *m_immutableInput;
	unsigned m_immutableInputBytes;
	unsigned m_firstJob;
	unsigned m_jobCount;
	volatile long m_state;
};

class SimulationPhaseJobStorage
{
public:
	SimulationPhaseJobStorage();

private:
	friend class SimulationPhaseGraph;
	SimulationPhaseId m_phaseId;
	SimulationPhaseJobKey m_key;
	unsigned m_phaseOrdinal;
	void *m_privateOutput;
	unsigned m_privateOutputBytes;
	SimulationPhaseExecuteFunction m_execute;
	SimulationPhaseValidateFunction m_validate;
	SimulationPhaseCommitFunction m_commit;
};

class SimulationPhaseJobResultStorage
{
public:
	SimulationPhaseJobResultStorage();

private:
	friend class SimulationPhaseGraph;
	volatile long m_state;
	unsigned m_generation;
	unsigned m_internalEpoch;
	SimulationPhaseExecutionIdentity m_executionIdentity;
};

class SimulationPhaseJobTicket
{
public:
	SimulationPhaseJobTicket();
	bool isValid() const;
	SimulationPhaseId phaseId() const;
	SimulationPhaseJobKey jobKey() const;
	unsigned generation() const;
	unsigned internalEpoch() const;

private:
	friend class SimulationPhaseGraph;
	unsigned m_jobOrdinal;
	SimulationPhaseId m_phaseId;
	SimulationPhaseJobKey m_jobKey;
	unsigned m_generation;
	unsigned m_internalEpoch;
};

struct SimulationPhaseJobSnapshot
{
	SimulationPhaseJobSnapshot();
	SimulationPhaseJobState state;
	unsigned generation;
	unsigned internalEpoch;
	SimulationPhaseExecutionIdentity executionIdentity;
};

// Title-independent deterministic phase executor. The owner claims ready jobs,
// an adapter executes tickets on physical workers or explicit owner help, and
// the owner advances validation/commit in stable phase-ID then job-key order.
class SimulationPhaseGraph
{
public:
	SimulationPhaseGraph(SimulationPhaseNodeStorage *nodeStorage,
		unsigned nodeCapacity, SimulationPhaseJobStorage *jobStorage,
		unsigned jobCapacity, SimulationPhaseJobResultStorage *resultStorage,
		unsigned resultCapacity, SimulationPhaseIsOwnerFunction isOwner,
		void *ownerContext);

	SimulationPhaseGraphConfigurationStatus configure(
		const SimulationPhaseDefinition *phases, unsigned phaseCount,
		const SimulationPhaseJobDefinition *jobs, unsigned jobCount);
	// Generation is caller-visible and strictly increasing for this graph.
	// The executor also advances an independent never-reused internal epoch.
	bool reset(unsigned generation);

	// Nonblocking owner operation. A false return means that no dependency-ready
	// unclaimed job exists at this instant.
	bool tryClaimReadyJob(SimulationPhaseJobTicket &ticket);
	SimulationPhaseWorkStatus executeClaimedJob(
		const SimulationPhaseJobTicket &ticket,
		const SimulationPhaseExecutionIdentity &executionIdentity);

	// Performs fail-closed validation, then owner-only commits. Later independent
	// phases never commit ahead of an earlier stable topological phase. Final
	// publication and cancellation contend for one atomic outcome; cancellation
	// after completion wins is a no-op.
	bool advanceOwner(unsigned *committedJobCount = 0);
	void requestCancellation();

	SimulationPhaseGraphState state() const;
	unsigned generation() const;
	unsigned internalEpoch() const;
	// Mixed terminal results use fixed FAILED, STALE_GENERATION, CANCELLED
	// precedence so worker completion order cannot select the graph outcome.
	SimulationPhaseWorkStatus terminalCause() const;
	unsigned phaseCount() const;
	unsigned jobCount() const;
	bool isQuiescent() const;
	SimulationPhaseId phaseIdAt(unsigned topologicalOrdinal) const;
	SimulationPhaseNodeState phaseState(SimulationPhaseId phaseId) const;
	bool jobSnapshot(SimulationPhaseId phaseId, SimulationPhaseJobKey key,
		SimulationPhaseJobSnapshot &snapshot) const;

#if defined(RTS_BUILD_CORE_EXTRAS)
	// Focused wraparound fault injection; never available in production builds.
	bool forceInternalEpochExhaustionForTest();
#endif

private:
	SimulationPhaseGraph(const SimulationPhaseGraph &);
	SimulationPhaseGraph &operator=(const SimulationPhaseGraph &);

	bool isOwner() const;
	bool cancellationRequested(unsigned generation,
		unsigned internalEpoch) const;
	unsigned findPhaseOrdinal(SimulationPhaseId phaseId) const;
	unsigned findJobOrdinal(SimulationPhaseId phaseId,
		SimulationPhaseJobKey key) const;
	void refreshReadyPhases();
	void cancelInactiveJobs();
	void recordTerminalCause(SimulationPhaseWorkStatus status);
	void findTerminalSource(SimulationPhaseWorkStatus status,
		unsigned &phaseOrdinal, unsigned &jobOrdinal) const;
	bool finishRecordedTerminalIfQuiescent();
	bool advanceInternalEpoch();
	void finishTerminal(SimulationPhaseWorkStatus status,
		unsigned phaseOrdinal, unsigned jobOrdinal = ~0u);

	SimulationPhaseNodeStorage *m_nodes;
	SimulationPhaseJobStorage *m_jobs;
	SimulationPhaseJobResultStorage *m_results;
	unsigned m_nodeCapacity;
	unsigned m_jobCapacity;
	unsigned m_resultCapacity;
	unsigned m_phaseCount;
	unsigned m_jobCount;
	unsigned m_generation;
	unsigned m_lastGeneration;
	unsigned m_internalEpoch;
	unsigned m_nextCommitPhase;
	unsigned m_committedPhaseMask;
	mutable volatile long m_controlLock;
	volatile long m_activeExecutors;
	volatile long m_advanceOwnerActive;
	volatile long m_reentrantAdvanceDetected;
	volatile long m_terminalStatusMask;
	volatile long m_state;
	volatile long m_publicationOutcome;
	SimulationPhaseIsOwnerFunction m_isOwner;
	void *m_ownerContext;

	friend class SimulationPhaseJobContext;
};

} // namespace rts
