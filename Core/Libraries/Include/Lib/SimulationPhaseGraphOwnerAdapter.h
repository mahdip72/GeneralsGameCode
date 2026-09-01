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

#include "Lib/JobSystem.h"
#include "Lib/SimulationPhaseGraph.h"

namespace rts
{

const unsigned LIVE_SIMULATION_PHASE_PERFORMANCE_SCHEMA_VERSION = 1;

typedef JobMetricCounter (*LiveSimulationPhaseClockFunction)(void *context);
// Exact floor conversion used by the product clock and legacy boundary fixture.
// It performs no floating-point work and saturates instead of wrapping.
JobMetricCounter LiveSimulationPhaseTicksToNanoseconds(JobMetricCounter ticks,
	JobMetricCounter ticksPerSecond);
JobMetricCounter LiveSimulationPhaseClockNowNanoseconds(void *context);

enum LiveSimulationPhaseId
{
	LIVE_SIMULATION_PHASE_OWNER_INTAKE = 1,
	LIVE_SIMULATION_PHASE_LEGACY_MUTABLE_ISLAND,
	LIVE_SIMULATION_PHASE_SPATIAL,
	LIVE_SIMULATION_PHASE_OWNER_TAIL,
	LIVE_SIMULATION_PHASE_VERIFICATION_PUBLICATION,
	LIVE_SIMULATION_PHASE_COUNT
};

enum LiveSimulationPhaseRunResult
{
	LIVE_SIMULATION_PHASE_COMPLETED = 0,
	LIVE_SIMULATION_PHASE_STOPPED_BY_OWNER,
	LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION,
	LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION
};

struct LiveSimulationPhaseAuthorityEvidence
{
	LiveSimulationPhaseAuthorityEvidence();

	SimulationPhaseId phaseId;
	SimulationPhaseJobKey jobKey;
	unsigned frame;
	unsigned generation;
	unsigned internalEpoch;
	SimulationPhaseExecutionKind executionKind;
	unsigned physicalWorkerIndex;
	bool validated;
	bool committed;
};

// Owner-thread telemetry for the product adapter. A completed stable frame has
// one attempt, five committed phases, and the canonical 1-2-3-4-5 signature.
// These counters deliberately describe phase dispatch/publication only; the
// established subsystems retain ownership of their worker-level metrics.
struct LiveSimulationPhaseRuntimeMetrics
{
	LiveSimulationPhaseRuntimeMetrics();

	JobMetricCounter attemptedFrames;
	JobMetricCounter completedFrames;
	JobMetricCounter stableSequenceFrames;
	JobMetricCounter stoppedByOwnerFrames;
	JobMetricCounter fallbackBeforeMutationFrames;
	JobMetricCounter failedAfterMutationFrames;
	JobMetricCounter committedPhases;
	JobMetricCounter sequenceViolationFrames;
	unsigned lastFrame;
	unsigned lastGeneration;
	unsigned lastCommittedPhaseCount;
	unsigned lastSequenceSignature;
	// Telemetry only. These values are never serialized, transferred, hashed,
	// consulted by phase validation, or used to select simulation behavior.
	JobMetricCounter ownerPhaseTotalNanoseconds[
		LIVE_SIMULATION_PHASE_COUNT - 1];
	JobMetricCounter ownerPhaseMaximumNanoseconds[
		LIVE_SIMULATION_PHASE_COUNT - 1];
	JobMetricCounter ownerPhaseSampleCount[
		LIVE_SIMULATION_PHASE_COUNT - 1];
	JobMetricCounter frameSimulationTotalNanoseconds;
	JobMetricCounter frameSimulationMaximumNanoseconds;
	JobMetricCounter frameSimulationSampleCount;
	JobMetricCounter serialIslandTotalNanoseconds;
	JobMetricCounter serialIslandMaximumNanoseconds;
	JobMetricCounter serialIslandSampleCount;
};

// Product epoch gate shared by both titles. Legacy/VC6 runtime and unmarked or
// unknown replay epochs retain the byte-for-byte legacy tick path.
bool ShouldUseLiveSimulationPhaseGraph(bool nativeRuntime,
	bool replayGame, unsigned replayEpoch, unsigned currentReplayEpoch);
bool IsLiveSimulationPhaseReleaseWorkerCount(unsigned workerCount);
bool HasStableLiveSimulationPhaseEvidence(
	const LiveSimulationPhaseRuntimeMetrics &metrics);

typedef bool (*LiveSimulationPhaseOwnerFunction)(void *ownerContext);
typedef bool (*LiveSimulationPhaseValidateFunction)(
	SimulationPhaseId phaseId, unsigned generation, unsigned frame,
	void *ownerContext);

// Returning false is an intentional end-of-update request. The live title
// adapters use it only for the owner-intake time-frozen early exit. Like the
// graph commit callback itself, this function must be non-throwing.
typedef bool (*LiveSimulationPhaseCommitFunction)(
	SimulationPhaseId phaseId, unsigned generation, unsigned frame,
	void *ownerContext);

struct LiveSimulationPhaseOwnerCallbacks
{
	LiveSimulationPhaseOwnerCallbacks();

	LiveSimulationPhaseOwnerFunction isOwner;
	LiveSimulationPhaseValidateFunction validate;
	LiveSimulationPhaseCommitFunction commit;
};

// One fixed owner-authority job is configured for each live GameLogic phase:
//
// owner intake -> legacy mutable island -> spatial -> owner tail
//              -> verification/publication
//
// Job execution only prepares a private authority token. Gameplay mutation
// remains in the validated owner commit. Worker-capable physics, collision,
// immutable-spatial, and status-timer kernels remain inside their established
// phase implementations and retain their existing deterministic reduce/commit.
class LiveSimulationPhaseGraphOwnerAdapter
{
public:
	LiveSimulationPhaseGraphOwnerAdapter(
		const LiveSimulationPhaseOwnerCallbacks &callbacks,
		void *ownerContext);

	// Each call advances a private, never-reused update generation. frame is the
	// stable simulation-frame identity carried by every authority token.
	LiveSimulationPhaseRunResult runFrame(unsigned frame);
	void requestCancellation();
	// Owner intake may start/reset a game and thereby change m_frame. Only the
	// still-blocked, never-claimed phases can adopt that post-intake identity.
	bool retargetPendingFrameAfterIntake(unsigned frame);

	unsigned generation() const;
	unsigned committedPhaseCount() const;
	const LiveSimulationPhaseRuntimeMetrics &runtimeMetrics() const;
	void resetRuntimeMetrics();
	// Deterministic unit fixtures may replace the monotonic clock only while no
	// frame is active. Product callers retain the default clock.
	bool setPerformanceClockForTesting(
		LiveSimulationPhaseClockFunction clockFunction, void *clockContext);
	JobMetricCounter performanceClockNowNanoseconds() const;
	// The legacy/direct lane measures the exact same five owner phases outside
	// the graph and publishes them through this telemetry-only sink.
	bool recordDirectFramePerformance(const JobMetricCounter *phaseNanoseconds,
		unsigned phaseCount, JobMetricCounter frameNanoseconds);
	SimulationPhaseGraphConfigurationStatus configurationStatus() const;
	bool authorityEvidence(SimulationPhaseId phaseId,
		LiveSimulationPhaseAuthorityEvidence &evidence) const;

	static bool canonicalDependency(SimulationPhaseId phaseId,
		SimulationPhaseId &dependency);

private:
	LiveSimulationPhaseGraphOwnerAdapter(
		const LiveSimulationPhaseGraphOwnerAdapter &);
	LiveSimulationPhaseGraphOwnerAdapter &operator=(
		const LiveSimulationPhaseGraphOwnerAdapter &);

	struct PhaseInput
	{
		SimulationPhaseId phaseId;
		unsigned frame;
		unsigned generation;
	};

	struct AuthorityToken
	{
		SimulationPhaseId phaseId;
		SimulationPhaseJobKey jobKey;
		unsigned frame;
		unsigned generation;
		unsigned internalEpoch;
		SimulationPhaseExecutionKind executionKind;
		unsigned physicalWorkerIndex;
	};

	bool ensureConfigured();
	LiveSimulationPhaseRunResult failureResult() const;
	LiveSimulationPhaseRunResult finishRun(
		LiveSimulationPhaseRunResult result, unsigned frame);
	void recordOwnerPhasePerformance(unsigned phaseOrdinalValue,
		JobMetricCounter elapsedNanoseconds);
	void recordFramePerformance(JobMetricCounter elapsedNanoseconds);
	unsigned phaseOrdinal(SimulationPhaseId phaseId) const;

	static bool isGraphOwner(void *adapterContext);
	static SimulationPhaseWorkStatus executeAuthorityToken(
		const SimulationPhaseJobContext &context,
		const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
		void *privateJobOutput, unsigned privateJobOutputBytes);
	static SimulationPhaseWorkStatus validateAuthorityToken(
		const SimulationPhaseCommitContext &context,
		const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
		const void *privateJobOutput, unsigned privateJobOutputBytes,
		void *adapterContext);
	static void commitOwnerPhase(
		const SimulationPhaseCommitContext &context,
		const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
		const void *privateJobOutput, unsigned privateJobOutputBytes,
		void *adapterContext);

	SimulationPhaseNodeStorage m_nodeStorage[LIVE_SIMULATION_PHASE_COUNT - 1];
	SimulationPhaseJobStorage m_jobStorage[LIVE_SIMULATION_PHASE_COUNT - 1];
	SimulationPhaseJobResultStorage m_resultStorage[LIVE_SIMULATION_PHASE_COUNT - 1];
	PhaseInput m_inputs[LIVE_SIMULATION_PHASE_COUNT - 1];
	AuthorityToken m_outputs[LIVE_SIMULATION_PHASE_COUNT - 1];
	LiveSimulationPhaseAuthorityEvidence
		m_evidence[LIVE_SIMULATION_PHASE_COUNT - 1];
	SimulationPhaseGraph m_graph;
	LiveSimulationPhaseOwnerCallbacks m_callbacks;
	void *m_ownerContext;
	SimulationPhaseGraphConfigurationStatus m_configurationStatus;
	unsigned m_nextGeneration;
	unsigned m_committedPhaseCount;
	unsigned m_expectedPhaseOrdinal;
	unsigned m_currentSequenceSignature;
	LiveSimulationPhaseClockFunction m_performanceClockFunction;
	void *m_performanceClockContext;
	JobMetricCounter m_currentFrameStartNanoseconds;
	LiveSimulationPhaseRuntimeMetrics m_runtimeMetrics;
	bool m_configured;
	bool m_frameActive;
	bool m_ownerCommitEntered;
	bool m_stopAfterCommit;
	bool m_sequenceViolation;
};

} // namespace rts
