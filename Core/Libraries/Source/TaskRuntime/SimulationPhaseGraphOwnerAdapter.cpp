/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#include <exception>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace rts
{

namespace
{
const unsigned LIVE_PHASE_STORAGE_COUNT = LIVE_SIMULATION_PHASE_COUNT - 1;
const unsigned NANOSECONDS_PER_SECOND = 1000000000u;

JobMetricCounter maximumMetricCounter()
{
	return ~static_cast<JobMetricCounter>(0);
}

void saturatingAdd(JobMetricCounter &value, JobMetricCounter increment)
{
	const JobMetricCounter maximum = maximumMetricCounter();
	value = increment > maximum - value ? maximum : value + increment;
}

void saturatingIncrement(JobMetricCounter &value)
{
	if (value != maximumMetricCounter())
		++value;
}

JobMetricCounter elapsedNanoseconds(JobMetricCounter start,
	JobMetricCounter end)
{
	return end >= start ? end - start : 0;
}

unsigned addModulo(JobMetricCounter &value, JobMetricCounter increment,
	JobMetricCounter modulus)
{
	// Both inputs are already reduced. Subtraction-based comparison avoids the
	// overflow that a direct value + increment check would introduce.
	if (value >= modulus - increment)
	{
		value -= modulus - increment;
		return 1;
	}
	value += increment;
	return 0;
}

JobMetricCounter multiplyFraction(JobMetricCounter numerator,
	unsigned multiplier, JobMetricCounter denominator)
{
	// numerator < denominator, so the final quotient is < multiplier. Build
	// the product one multiplier bit at a time while retaining only its reduced
	// remainder; neither the quotient nor the remainder can overflow.
	JobMetricCounter quotient = 0;
	JobMetricCounter remainder = 0;
	unsigned bit = 1;
	while (bit <= multiplier / 2)
		bit <<= 1;
	for (; bit != 0; bit >>= 1)
	{
		const unsigned doubledCarry = addModulo(remainder, remainder,
			denominator);
		quotient = quotient * 2 + doubledCarry;
		if ((multiplier & bit) != 0)
			quotient += addModulo(remainder, numerator, denominator);
	}
	return quotient;
}
}

JobMetricCounter LiveSimulationPhaseTicksToNanoseconds(JobMetricCounter ticks,
	JobMetricCounter ticksPerSecond)
{
	if (ticksPerSecond == 0)
		return 0;
	const JobMetricCounter maximum = maximumMetricCounter();
	const JobMetricCounter seconds = ticks / ticksPerSecond;
	if (seconds > maximum / NANOSECONDS_PER_SECOND)
		return maximum;
	const JobMetricCounter whole = seconds * NANOSECONDS_PER_SECOND;
	const JobMetricCounter remainder = ticks % ticksPerSecond;
	const JobMetricCounter fractional = multiplyFraction(remainder,
		NANOSECONDS_PER_SECOND, ticksPerSecond);
	return fractional > maximum - whole ? maximum : whole + fractional;
}

JobMetricCounter LiveSimulationPhaseClockNowNanoseconds(void *context)
{
	(void)context;
#if defined(_WIN32)
	LARGE_INTEGER frequency;
	LARGE_INTEGER counter;
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
		!QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
	{
		return 0;
	}
	const JobMetricCounter ticks =
		static_cast<JobMetricCounter>(counter.QuadPart);
	const JobMetricCounter ticksPerSecond =
		static_cast<JobMetricCounter>(frequency.QuadPart);
	return LiveSimulationPhaseTicksToNanoseconds(ticks, ticksPerSecond);
#else
	const clock_t counter = clock();
	if (counter < 0 || CLOCKS_PER_SEC <= 0)
		return 0;
	const JobMetricCounter ticks = static_cast<JobMetricCounter>(counter);
	const JobMetricCounter ticksPerSecond =
		static_cast<JobMetricCounter>(CLOCKS_PER_SEC);
	return LiveSimulationPhaseTicksToNanoseconds(ticks, ticksPerSecond);
#endif
}

LiveSimulationPhaseAuthorityEvidence::LiveSimulationPhaseAuthorityEvidence()
	: phaseId(SIMULATION_PHASE_INVALID_ID),
	  jobKey(SIMULATION_PHASE_INVALID_JOB_KEY), frame(0), generation(0),
	  internalEpoch(0), executionKind(SIMULATION_PHASE_EXECUTION_NONE),
	  physicalWorkerIndex(SIMULATION_PHASE_INVALID_WORKER_INDEX),
	  validated(false), committed(false)
{
}

LiveSimulationPhaseFailureDiagnostic::LiveSimulationPhaseFailureDiagnostic()
	: boundary(LIVE_SIMULATION_PHASE_FAILURE_NONE),
	  phaseId(SIMULATION_PHASE_INVALID_ID), jobKey(SIMULATION_PHASE_INVALID_JOB_KEY),
	  frame(0), generation(0), internalEpoch(0), committedPhaseCount(0),
	  sequenceSignature(0), ownerCommitEntered(false),
	  graphState(SIMULATION_PHASE_GRAPH_UNCONFIGURED),
	  terminalCause(SIMULATION_PHASE_WORK_SUCCEEDED),
	  returnGraphState(SIMULATION_PHASE_GRAPH_UNCONFIGURED),
	  returnTerminalCause(SIMULATION_PHASE_WORK_SUCCEEDED),
	  ownerContextKnown(false), ownerFrame(0), ownerPhaseFrame(0), ownerCursor(0),
	  exceptionCategory(LIVE_SIMULATION_PHASE_EXCEPTION_NONE),
	  exceptionMessageTruncated(false)
{
	exceptionMessage[0] = '\0';
}

const LiveSimulationPhaseFailureDiagnostic &
LiveSimulationPhaseGraphOwnerAdapter::failureDiagnostic() const
{
	return m_failureDiagnostic;
}

void LiveSimulationPhaseGraphOwnerAdapter::annotateOwnerFailure(
	unsigned ownerFrame, unsigned ownerPhaseFrame, unsigned ownerCursor,
	LiveSimulationPhaseExceptionCategory category, const char *message)
{
	if (m_failureDiagnostic.boundary != LIVE_SIMULATION_PHASE_FAILURE_NONE ||
		m_failureDiagnostic.ownerContextKnown)
		return;
	m_failureDiagnostic.ownerContextKnown = true;
	m_failureDiagnostic.ownerFrame = ownerFrame;
	m_failureDiagnostic.ownerPhaseFrame = ownerPhaseFrame;
	m_failureDiagnostic.ownerCursor = ownerCursor;
	recordException(category, message);
}

void LiveSimulationPhaseGraphOwnerAdapter::recordException(
	LiveSimulationPhaseExceptionCategory category, const char *message)
{
	if (m_failureDiagnostic.boundary != LIVE_SIMULATION_PHASE_FAILURE_NONE ||
		m_failureDiagnostic.exceptionCategory != LIVE_SIMULATION_PHASE_EXCEPTION_NONE)
		return;
	m_failureDiagnostic.exceptionCategory = category;
	unsigned length = 0;
	const unsigned capacity = sizeof(m_failureDiagnostic.exceptionMessage);
	if (message != 0)
	{
		while (length + 1 < capacity && message[length] != '\0')
		{
			m_failureDiagnostic.exceptionMessage[length] = message[length];
			++length;
		}
	}
	m_failureDiagnostic.exceptionMessage[length] = '\0';
	m_failureDiagnostic.exceptionMessageTruncated =
		message != 0 && message[length] != '\0';
}

void LiveSimulationPhaseGraphOwnerAdapter::recordFailure(
	LiveSimulationPhaseFailureBoundary boundary, SimulationPhaseId phaseId,
	unsigned frame)
{
	if (m_failureDiagnostic.boundary != LIVE_SIMULATION_PHASE_FAILURE_NONE)
		return;
	m_failureDiagnostic.boundary = boundary;
	m_failureDiagnostic.phaseId = phaseId;
	m_failureDiagnostic.jobKey = phaseOrdinal(phaseId) < LIVE_PHASE_STORAGE_COUNT ?
		0 : SIMULATION_PHASE_INVALID_JOB_KEY;
	m_failureDiagnostic.frame = frame;
	m_failureDiagnostic.generation = m_graph.generation();
	m_failureDiagnostic.internalEpoch = m_graph.internalEpoch();
	m_failureDiagnostic.committedPhaseCount = m_committedPhaseCount;
	m_failureDiagnostic.sequenceSignature = m_currentSequenceSignature;
	m_failureDiagnostic.ownerCommitEntered = m_ownerCommitEntered;
	m_failureDiagnostic.graphState = m_graph.state();
	m_failureDiagnostic.terminalCause = m_graph.terminalCause();
	m_failureDiagnostic.returnGraphState = m_failureDiagnostic.graphState;
	m_failureDiagnostic.returnTerminalCause = m_failureDiagnostic.terminalCause;
}

LiveSimulationPhaseRuntimeMetrics::LiveSimulationPhaseRuntimeMetrics()
	: attemptedFrames(0), completedFrames(0), stableSequenceFrames(0),
	  stoppedByOwnerFrames(0), fallbackBeforeMutationFrames(0),
	  failedAfterMutationFrames(0), committedPhases(0),
	  sequenceViolationFrames(0), lastFrame(0), lastGeneration(0),
	  lastCommittedPhaseCount(0), lastSequenceSignature(0),
	  frameSimulationTotalNanoseconds(0),
	  frameSimulationMaximumNanoseconds(0), frameSimulationSampleCount(0),
	  serialIslandTotalNanoseconds(0),
	  serialIslandMaximumNanoseconds(0), serialIslandSampleCount(0)
{
	unsigned phaseOrdinalValue;
	for (phaseOrdinalValue = 0; phaseOrdinalValue < LIVE_PHASE_STORAGE_COUNT;
		++phaseOrdinalValue)
	{
		ownerPhaseTotalNanoseconds[phaseOrdinalValue] = 0;
		ownerPhaseMaximumNanoseconds[phaseOrdinalValue] = 0;
		ownerPhaseSampleCount[phaseOrdinalValue] = 0;
	}
}

bool ShouldUseLiveSimulationPhaseGraph(bool nativeRuntime,
	bool replayGame, unsigned replayEpoch, unsigned currentReplayEpoch)
{
	return nativeRuntime && (!replayGame ||
		(currentReplayEpoch != 0 && replayEpoch == currentReplayEpoch));
}

bool IsLiveSimulationPhaseReleaseWorkerCount(unsigned workerCount)
{
	return workerCount == 1 || workerCount == 2 || workerCount == 4 ||
		workerCount == 8 || workerCount == 16;
}

bool HasStableLiveSimulationPhaseEvidence(
	const LiveSimulationPhaseRuntimeMetrics &metrics)
{
	return metrics.attemptedFrames != 0 &&
		metrics.completedFrames == metrics.attemptedFrames &&
		metrics.stableSequenceFrames == metrics.attemptedFrames &&
		metrics.stoppedByOwnerFrames == 0 &&
		metrics.fallbackBeforeMutationFrames == 0 &&
		metrics.failedAfterMutationFrames == 0 &&
		metrics.sequenceViolationFrames == 0 &&
		metrics.committedPhases % (LIVE_SIMULATION_PHASE_COUNT - 1) == 0 &&
		metrics.committedPhases / (LIVE_SIMULATION_PHASE_COUNT - 1) ==
			metrics.attemptedFrames &&
		metrics.lastGeneration != 0 && metrics.lastCommittedPhaseCount ==
			LIVE_SIMULATION_PHASE_COUNT - 1 &&
		metrics.lastSequenceSignature == 12345u;
}

LiveSimulationPhaseOwnerCallbacks::LiveSimulationPhaseOwnerCallbacks()
	: isOwner(0), validate(0), commit(0)
{
}

LiveSimulationPhaseGraphOwnerAdapter::LiveSimulationPhaseGraphOwnerAdapter(
	const LiveSimulationPhaseOwnerCallbacks &callbacks, void *ownerContext)
	: m_graph(m_nodeStorage, LIVE_PHASE_STORAGE_COUNT,
		m_jobStorage, LIVE_PHASE_STORAGE_COUNT,
		m_resultStorage, LIVE_PHASE_STORAGE_COUNT,
		&LiveSimulationPhaseGraphOwnerAdapter::isGraphOwner, this),
	  m_callbacks(callbacks), m_ownerContext(ownerContext),
	  m_configurationStatus(SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT),
	  m_nextGeneration(0), m_committedPhaseCount(0),
	  m_expectedPhaseOrdinal(0), m_currentSequenceSignature(0),
	  m_performanceClockFunction(&LiveSimulationPhaseClockNowNanoseconds),
	  m_performanceClockContext(0), m_currentFrameStartNanoseconds(0),
	  m_configured(false), m_frameActive(false), m_ownerCommitEntered(false),
	  m_stopAfterCommit(false), m_sequenceViolation(false)
{
	unsigned phaseOrdinalValue;
	for (phaseOrdinalValue = 0; phaseOrdinalValue < LIVE_PHASE_STORAGE_COUNT;
		++phaseOrdinalValue)
	{
		m_inputs[phaseOrdinalValue].phaseId = phaseOrdinalValue + 1;
		m_inputs[phaseOrdinalValue].frame = 0;
		m_inputs[phaseOrdinalValue].generation = 0;
		m_outputs[phaseOrdinalValue].phaseId = SIMULATION_PHASE_INVALID_ID;
		m_outputs[phaseOrdinalValue].jobKey =
			SIMULATION_PHASE_INVALID_JOB_KEY;
		m_outputs[phaseOrdinalValue].frame = 0;
		m_outputs[phaseOrdinalValue].generation = 0;
		m_outputs[phaseOrdinalValue].internalEpoch = 0;
		m_outputs[phaseOrdinalValue].executionKind =
			SIMULATION_PHASE_EXECUTION_NONE;
		m_outputs[phaseOrdinalValue].physicalWorkerIndex =
			SIMULATION_PHASE_INVALID_WORKER_INDEX;
	}
}

bool LiveSimulationPhaseGraphOwnerAdapter::canonicalDependency(
	SimulationPhaseId phaseId, SimulationPhaseId &dependency)
{
	if (phaseId <= LIVE_SIMULATION_PHASE_OWNER_INTAKE ||
		phaseId >= LIVE_SIMULATION_PHASE_COUNT)
	{
		dependency = SIMULATION_PHASE_INVALID_ID;
		return false;
	}
	dependency = phaseId - 1;
	return true;
}

bool LiveSimulationPhaseGraphOwnerAdapter::ensureConfigured()
{
	if (m_configured) return true;
	if (m_callbacks.isOwner == 0 || m_callbacks.validate == 0 ||
		m_callbacks.commit == 0 || !m_callbacks.isOwner(m_ownerContext))
	{
		m_configurationStatus = SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT;
		return false;
	}

	SimulationPhaseDefinition phases[LIVE_PHASE_STORAGE_COUNT];
	SimulationPhaseJobDefinition jobs[LIVE_PHASE_STORAGE_COUNT];
	SimulationPhaseId dependencies[LIVE_PHASE_STORAGE_COUNT - 1];
	unsigned phaseOrdinalValue;
	for (phaseOrdinalValue = 0; phaseOrdinalValue < LIVE_PHASE_STORAGE_COUNT;
		++phaseOrdinalValue)
	{
		const SimulationPhaseId phaseId = phaseOrdinalValue + 1;
		phases[phaseOrdinalValue].id = phaseId;
		phases[phaseOrdinalValue].dependencies = phaseOrdinalValue == 0 ?
			0 : &dependencies[phaseOrdinalValue - 1];
		phases[phaseOrdinalValue].dependencyCount =
			phaseOrdinalValue == 0 ? 0 : 1;
		phases[phaseOrdinalValue].immutableInput = &m_inputs[phaseOrdinalValue];
		phases[phaseOrdinalValue].immutableInputBytes = sizeof(PhaseInput);

		if (phaseOrdinalValue != 0)
			dependencies[phaseOrdinalValue - 1] = phaseId - 1;

		jobs[phaseOrdinalValue].phaseId = phaseId;
		jobs[phaseOrdinalValue].key = 0;
		jobs[phaseOrdinalValue].privateOutput = &m_outputs[phaseOrdinalValue];
		jobs[phaseOrdinalValue].privateOutputBytes = sizeof(AuthorityToken);
		jobs[phaseOrdinalValue].execute = &executeAuthorityToken;
		jobs[phaseOrdinalValue].validate = &validateAuthorityToken;
		jobs[phaseOrdinalValue].commit = &commitOwnerPhase;
	}

	m_configurationStatus = m_graph.configure(phases,
		LIVE_PHASE_STORAGE_COUNT, jobs, LIVE_PHASE_STORAGE_COUNT);
	m_configured = m_configurationStatus ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID;
	return m_configured;
}

LiveSimulationPhaseRunResult
LiveSimulationPhaseGraphOwnerAdapter::failureResult() const
{
	return m_committedPhaseCount == 0 && !m_ownerCommitEntered ?
		LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION :
		LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION;
}

LiveSimulationPhaseRunResult LiveSimulationPhaseGraphOwnerAdapter::finishRun(
	LiveSimulationPhaseRunResult result, unsigned frame)
{
	if (m_failureDiagnostic.boundary != LIVE_SIMULATION_PHASE_FAILURE_NONE &&
		m_failureDiagnostic.boundary != LIVE_SIMULATION_PHASE_FAILURE_NESTED_ENTRY &&
		(result == LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION ||
		 result == LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION))
	{
		m_failureDiagnostic.returnGraphState = m_graph.state();
		m_failureDiagnostic.returnTerminalCause = m_graph.terminalCause();
	}
	if (result != LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION)
	{
		recordFramePerformance(elapsedNanoseconds(
			m_currentFrameStartNanoseconds, performanceClockNowNanoseconds()));
	}
	++m_runtimeMetrics.attemptedFrames;
	m_runtimeMetrics.committedPhases += m_committedPhaseCount;
	m_runtimeMetrics.lastFrame = frame;
	m_runtimeMetrics.lastGeneration = m_graph.generation();
	m_runtimeMetrics.lastCommittedPhaseCount = m_committedPhaseCount;
	m_runtimeMetrics.lastSequenceSignature = m_currentSequenceSignature;

	switch (result)
	{
	case LIVE_SIMULATION_PHASE_COMPLETED:
		++m_runtimeMetrics.completedFrames;
		if (m_committedPhaseCount == LIVE_PHASE_STORAGE_COUNT &&
			m_currentSequenceSignature == 12345u && !m_sequenceViolation)
		{
			++m_runtimeMetrics.stableSequenceFrames;
		}
		else
		{
			m_sequenceViolation = true;
		}
		break;
	case LIVE_SIMULATION_PHASE_STOPPED_BY_OWNER:
		++m_runtimeMetrics.stoppedByOwnerFrames;
		break;
	case LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION:
		++m_runtimeMetrics.fallbackBeforeMutationFrames;
		break;
	case LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION:
		++m_runtimeMetrics.failedAfterMutationFrames;
		break;
	}
	if (m_sequenceViolation)
		++m_runtimeMetrics.sequenceViolationFrames;
	return result;
}

unsigned LiveSimulationPhaseGraphOwnerAdapter::phaseOrdinal(
	SimulationPhaseId phaseId) const
{
	if (phaseId < LIVE_SIMULATION_PHASE_OWNER_INTAKE ||
		phaseId >= LIVE_SIMULATION_PHASE_COUNT)
		return LIVE_PHASE_STORAGE_COUNT;
	return phaseId - 1;
}

LiveSimulationPhaseRunResult
LiveSimulationPhaseGraphOwnerAdapter::runFrame(unsigned frame)
{
	// Title callbacks execute inside advanceOwner. Reject nested frame entry
	// before rewriting any outer-attempt identity or fallback cutoff.
	if (m_frameActive)
	{
		recordFailure(LIVE_SIMULATION_PHASE_FAILURE_NESTED_ENTRY,
			m_expectedPhaseOrdinal + 1, frame);
		// This rejected call did not own or commit the outer frame's work. Keep
		// its evidence independent so aggregate commit totals remain truthful.
		++m_runtimeMetrics.attemptedFrames;
		++m_runtimeMetrics.failedAfterMutationFrames;
		m_runtimeMetrics.lastFrame = frame;
		m_runtimeMetrics.lastGeneration = m_graph.generation();
		m_runtimeMetrics.lastCommittedPhaseCount = 0;
		m_runtimeMetrics.lastSequenceSignature = 0;
		return LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION;
	}
	// Reset validity without copying or formatting a message on successful
	// frames. All identity fields are replaced together at the first failure.
	m_failureDiagnostic.boundary = LIVE_SIMULATION_PHASE_FAILURE_NONE;
	m_failureDiagnostic.ownerContextKnown = false;
	m_failureDiagnostic.exceptionCategory = LIVE_SIMULATION_PHASE_EXCEPTION_NONE;
	m_failureDiagnostic.exceptionMessage[0] = '\0';
	m_failureDiagnostic.exceptionMessageTruncated = false;
	m_currentFrameStartNanoseconds = performanceClockNowNanoseconds();
	m_committedPhaseCount = 0;
	m_expectedPhaseOrdinal = 0;
	m_currentSequenceSignature = 0;
	m_ownerCommitEntered = false;
	m_stopAfterCommit = false;
	m_sequenceViolation = false;
	if (!ensureConfigured() || m_nextGeneration == ~0u)
	{
		recordFailure(LIVE_SIMULATION_PHASE_FAILURE_CONFIGURATION,
			SIMULATION_PHASE_INVALID_ID, frame);
		return finishRun(LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION, frame);
	}

	const unsigned generationValue = m_nextGeneration + 1;
	unsigned phaseOrdinalValue;
	for (phaseOrdinalValue = 0; phaseOrdinalValue < LIVE_PHASE_STORAGE_COUNT;
		++phaseOrdinalValue)
	{
		m_inputs[phaseOrdinalValue].frame = frame;
		m_inputs[phaseOrdinalValue].generation = generationValue;
		m_evidence[phaseOrdinalValue] = LiveSimulationPhaseAuthorityEvidence();
	}
	if (!m_graph.reset(generationValue))
	{
		recordFailure(LIVE_SIMULATION_PHASE_FAILURE_RESET,
			SIMULATION_PHASE_INVALID_ID, frame);
		return finishRun(LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION, frame);
	}
	m_nextGeneration = generationValue;
	m_frameActive = true;

	for (phaseOrdinalValue = 0; phaseOrdinalValue < LIVE_PHASE_STORAGE_COUNT;
		++phaseOrdinalValue)
	{
		SimulationPhaseJobTicket ticket;
		if (!m_graph.tryClaimReadyJob(ticket) ||
			ticket.phaseId() != phaseOrdinalValue + 1 ||
			ticket.jobKey() != 0)
		{
			recordFailure(LIVE_SIMULATION_PHASE_FAILURE_CLAIM,
				phaseOrdinalValue + 1, m_inputs[phaseOrdinalValue].frame);
			m_sequenceViolation = true;
			m_graph.requestCancellation();
			m_graph.advanceOwner();
			m_frameActive = false;
			return finishRun(failureResult(), frame);
		}

		const SimulationPhaseWorkStatus executeStatus =
			m_graph.executeClaimedJob(ticket,
				SimulationPhaseExecutionIdentity::ownerHelp());
		if (executeStatus != SIMULATION_PHASE_WORK_SUCCEEDED)
		{
			recordFailure(LIVE_SIMULATION_PHASE_FAILURE_EXECUTE,
				phaseOrdinalValue + 1, m_inputs[phaseOrdinalValue].frame);
			m_graph.advanceOwner();
			m_frameActive = false;
			return finishRun(failureResult(), frame);
		}

		const JobMetricCounter phaseStartNanoseconds =
			performanceClockNowNanoseconds();
		const unsigned committedBeforeAdvance = m_committedPhaseCount;
		unsigned committedJobs = 0;
		const bool advanced = m_graph.advanceOwner(&committedJobs);
		const JobMetricCounter phaseEndNanoseconds =
			performanceClockNowNanoseconds();
		if (m_committedPhaseCount != committedBeforeAdvance)
		{
			recordOwnerPhasePerformance(phaseOrdinalValue, elapsedNanoseconds(
				phaseStartNanoseconds, phaseEndNanoseconds));
		}
		if (!advanced || committedJobs != 1 ||
			m_committedPhaseCount != phaseOrdinalValue + 1)
		{
			recordFailure(LIVE_SIMULATION_PHASE_FAILURE_ADVANCE,
				phaseOrdinalValue + 1, m_inputs[phaseOrdinalValue].frame);
			m_sequenceViolation = true;
			m_graph.requestCancellation();
			m_graph.advanceOwner();
			m_frameActive = false;
			return finishRun(failureResult(), frame);
		}

		if (m_stopAfterCommit)
		{
			if (phaseOrdinalValue != 0)
				recordFailure(LIVE_SIMULATION_PHASE_FAILURE_UNEXPECTED_STOP,
					phaseOrdinalValue + 1, m_inputs[phaseOrdinalValue].frame);
			m_graph.requestCancellation();
			m_graph.advanceOwner();
			m_frameActive = false;
			return finishRun(phaseOrdinalValue == 0 ?
				LIVE_SIMULATION_PHASE_STOPPED_BY_OWNER :
				LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION, frame);
		}
	}

	m_frameActive = false;
	if (m_graph.state() != SIMULATION_PHASE_GRAPH_COMPLETED ||
		m_committedPhaseCount != LIVE_PHASE_STORAGE_COUNT)
	{
		recordFailure(LIVE_SIMULATION_PHASE_FAILURE_FINAL_STATE,
			LIVE_SIMULATION_PHASE_VERIFICATION_PUBLICATION,
			m_inputs[LIVE_PHASE_STORAGE_COUNT - 1].frame);
	}
	return finishRun(m_graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED &&
		m_committedPhaseCount == LIVE_PHASE_STORAGE_COUNT ?
		LIVE_SIMULATION_PHASE_COMPLETED : failureResult(), frame);
}

void LiveSimulationPhaseGraphOwnerAdapter::requestCancellation()
{
	if (m_frameActive)
		m_graph.requestCancellation();
}

bool LiveSimulationPhaseGraphOwnerAdapter::retargetPendingFrameAfterIntake(
	unsigned frame)
{
	if (!m_frameActive || m_expectedPhaseOrdinal != 0 ||
		m_committedPhaseCount != 0)
	{
		return false;
	}
	unsigned phaseOrdinalValue;
	for (phaseOrdinalValue = 1; phaseOrdinalValue < LIVE_PHASE_STORAGE_COUNT;
		++phaseOrdinalValue)
	{
		m_inputs[phaseOrdinalValue].frame = frame;
	}
	return true;
}

unsigned LiveSimulationPhaseGraphOwnerAdapter::generation() const
{
	return m_graph.generation();
}

unsigned LiveSimulationPhaseGraphOwnerAdapter::committedPhaseCount() const
{
	return m_committedPhaseCount;
}

const LiveSimulationPhaseRuntimeMetrics &
LiveSimulationPhaseGraphOwnerAdapter::runtimeMetrics() const
{
	return m_runtimeMetrics;
}

void LiveSimulationPhaseGraphOwnerAdapter::resetRuntimeMetrics()
{
	if (!m_frameActive)
		m_runtimeMetrics = LiveSimulationPhaseRuntimeMetrics();
}

bool LiveSimulationPhaseGraphOwnerAdapter::setPerformanceClockForTesting(
	LiveSimulationPhaseClockFunction clockFunction, void *clockContext)
{
	if (m_frameActive || clockFunction == 0)
		return false;
	m_performanceClockFunction = clockFunction;
	m_performanceClockContext = clockContext;
	return true;
}

JobMetricCounter
LiveSimulationPhaseGraphOwnerAdapter::performanceClockNowNanoseconds() const
{
	return m_performanceClockFunction != 0 ?
		m_performanceClockFunction(m_performanceClockContext) : 0;
}

void LiveSimulationPhaseGraphOwnerAdapter::recordOwnerPhasePerformance(
	unsigned phaseOrdinalValue, JobMetricCounter elapsed)
{
	if (phaseOrdinalValue >= LIVE_PHASE_STORAGE_COUNT)
		return;
	saturatingAdd(m_runtimeMetrics.ownerPhaseTotalNanoseconds[
		phaseOrdinalValue], elapsed);
	if (elapsed > m_runtimeMetrics.ownerPhaseMaximumNanoseconds[
		phaseOrdinalValue])
	{
		m_runtimeMetrics.ownerPhaseMaximumNanoseconds[
			phaseOrdinalValue] = elapsed;
	}
	saturatingIncrement(m_runtimeMetrics.ownerPhaseSampleCount[
		phaseOrdinalValue]);
	if (phaseOrdinalValue ==
		LIVE_SIMULATION_PHASE_LEGACY_MUTABLE_ISLAND - 1)
	{
		saturatingAdd(m_runtimeMetrics.serialIslandTotalNanoseconds, elapsed);
		if (elapsed > m_runtimeMetrics.serialIslandMaximumNanoseconds)
			m_runtimeMetrics.serialIslandMaximumNanoseconds = elapsed;
		saturatingIncrement(m_runtimeMetrics.serialIslandSampleCount);
	}
}

void LiveSimulationPhaseGraphOwnerAdapter::recordFramePerformance(
	JobMetricCounter elapsed)
{
	saturatingAdd(m_runtimeMetrics.frameSimulationTotalNanoseconds, elapsed);
	if (elapsed > m_runtimeMetrics.frameSimulationMaximumNanoseconds)
		m_runtimeMetrics.frameSimulationMaximumNanoseconds = elapsed;
	saturatingIncrement(m_runtimeMetrics.frameSimulationSampleCount);
}

bool LiveSimulationPhaseGraphOwnerAdapter::recordDirectFramePerformance(
	const JobMetricCounter *phaseNanoseconds, unsigned phaseCount,
	JobMetricCounter frameNanoseconds)
{
	if (m_frameActive || phaseNanoseconds == 0 ||
		phaseCount > LIVE_PHASE_STORAGE_COUNT)
	{
		return false;
	}
	unsigned phaseOrdinalValue;
	for (phaseOrdinalValue = 0; phaseOrdinalValue < phaseCount;
		++phaseOrdinalValue)
	{
		recordOwnerPhasePerformance(phaseOrdinalValue,
			phaseNanoseconds[phaseOrdinalValue]);
	}
	recordFramePerformance(frameNanoseconds);
	return true;
}

SimulationPhaseGraphConfigurationStatus
LiveSimulationPhaseGraphOwnerAdapter::configurationStatus() const
{
	return m_configurationStatus;
}

bool LiveSimulationPhaseGraphOwnerAdapter::authorityEvidence(
	SimulationPhaseId phaseId,
	LiveSimulationPhaseAuthorityEvidence &evidence) const
{
	const unsigned phaseOrdinalValue = phaseOrdinal(phaseId);
	if (phaseOrdinalValue >= LIVE_PHASE_STORAGE_COUNT)
		return false;
	evidence = m_evidence[phaseOrdinalValue];
	return evidence.phaseId == phaseId;
}

bool LiveSimulationPhaseGraphOwnerAdapter::isGraphOwner(void *adapterContext)
{
	LiveSimulationPhaseGraphOwnerAdapter *adapter =
		static_cast<LiveSimulationPhaseGraphOwnerAdapter *>(adapterContext);
	return adapter != 0 && adapter->m_callbacks.isOwner != 0 &&
		adapter->m_callbacks.isOwner(adapter->m_ownerContext);
}

SimulationPhaseWorkStatus
LiveSimulationPhaseGraphOwnerAdapter::executeAuthorityToken(
	const SimulationPhaseJobContext &context,
	const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
	void *privateJobOutput, unsigned privateJobOutputBytes)
{
	if (immutablePhaseInput == 0 ||
		immutablePhaseInputBytes != sizeof(PhaseInput) ||
		privateJobOutput == 0 || privateJobOutputBytes != sizeof(AuthorityToken))
		return SIMULATION_PHASE_WORK_FAILED;

	const PhaseInput &input = *static_cast<const PhaseInput *>(
		immutablePhaseInput);
	if (context.phaseId() != input.phaseId ||
		context.jobKey() != 0 ||
		context.generation() != input.generation ||
		context.executionIdentity().kind() !=
			SIMULATION_PHASE_EXECUTION_OWNER_HELP ||
		context.executionIdentity().physicalWorkerIndex() !=
			SIMULATION_PHASE_INVALID_WORKER_INDEX ||
		context.isCancellationRequested())
	{
		return context.isCancellationRequested() ?
			SIMULATION_PHASE_WORK_CANCELLED : SIMULATION_PHASE_WORK_FAILED;
	}

	AuthorityToken &output = *static_cast<AuthorityToken *>(privateJobOutput);
	output.phaseId = context.phaseId();
	output.jobKey = context.jobKey();
	output.frame = input.frame;
	output.generation = context.generation();
	output.internalEpoch = context.internalEpoch();
	output.executionKind = context.executionIdentity().kind();
	output.physicalWorkerIndex =
		context.executionIdentity().physicalWorkerIndex();
	return SIMULATION_PHASE_WORK_SUCCEEDED;
}

SimulationPhaseWorkStatus
LiveSimulationPhaseGraphOwnerAdapter::validateAuthorityToken(
	const SimulationPhaseCommitContext &context,
	const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
	const void *privateJobOutput, unsigned privateJobOutputBytes,
	void *adapterContext)
{
	LiveSimulationPhaseGraphOwnerAdapter *adapter =
		static_cast<LiveSimulationPhaseGraphOwnerAdapter *>(adapterContext);
	if (adapter == 0 || immutablePhaseInput == 0 ||
		immutablePhaseInputBytes != sizeof(PhaseInput) ||
		privateJobOutput == 0 || privateJobOutputBytes != sizeof(AuthorityToken))
	{
		if (adapter != 0)
			adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_AUTHORITY_VALIDATION,
				context.phaseId(), 0);
		return SIMULATION_PHASE_WORK_FAILED;
	}

	const PhaseInput &input = *static_cast<const PhaseInput *>(
		immutablePhaseInput);
	const AuthorityToken &output = *static_cast<const AuthorityToken *>(
		privateJobOutput);
	const unsigned phaseOrdinalValue = adapter->phaseOrdinal(context.phaseId());
	if (phaseOrdinalValue >= LIVE_PHASE_STORAGE_COUNT ||
		phaseOrdinalValue != adapter->m_expectedPhaseOrdinal ||
		input.phaseId != context.phaseId() ||
		input.generation != context.generation() ||
		output.phaseId != context.phaseId() ||
		output.jobKey != context.jobKey() || output.frame != input.frame ||
		output.generation != context.generation() ||
		output.internalEpoch != context.internalEpoch() ||
		output.executionKind != SIMULATION_PHASE_EXECUTION_OWNER_HELP ||
		output.physicalWorkerIndex != SIMULATION_PHASE_INVALID_WORKER_INDEX ||
		adapter->m_callbacks.validate == 0)
	{
		adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_AUTHORITY_VALIDATION,
			context.phaseId(), input.frame);
		return SIMULATION_PHASE_WORK_FAILED;
	}
	try
	{
		if (!adapter->m_callbacks.validate(context.phaseId(),
			context.generation(), input.frame, adapter->m_ownerContext))
		{
			adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_OWNER_VALIDATION,
				context.phaseId(), input.frame);
			return SIMULATION_PHASE_WORK_FAILED;
		}
	}
	catch (const std::exception &exception)
	{
		adapter->recordException(LIVE_SIMULATION_PHASE_EXCEPTION_STANDARD, exception.what());
		adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_VALIDATE_EXCEPTION,
			context.phaseId(), input.frame);
		throw;
	}
	catch (...)
	{
		adapter->recordException(LIVE_SIMULATION_PHASE_EXCEPTION_UNKNOWN, 0);
		adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_VALIDATE_EXCEPTION,
			context.phaseId(), input.frame);
		throw;
	}

	LiveSimulationPhaseAuthorityEvidence &evidence =
		adapter->m_evidence[phaseOrdinalValue];
	evidence.phaseId = output.phaseId;
	evidence.jobKey = output.jobKey;
	evidence.frame = output.frame;
	evidence.generation = output.generation;
	evidence.internalEpoch = output.internalEpoch;
	evidence.executionKind = output.executionKind;
	evidence.physicalWorkerIndex = output.physicalWorkerIndex;
	evidence.validated = true;
	return SIMULATION_PHASE_WORK_SUCCEEDED;
}

void LiveSimulationPhaseGraphOwnerAdapter::commitOwnerPhase(
	const SimulationPhaseCommitContext &context,
	const void *immutablePhaseInput, unsigned immutablePhaseInputBytes,
	const void *privateJobOutput, unsigned privateJobOutputBytes,
	void *adapterContext)
{
	LiveSimulationPhaseGraphOwnerAdapter *adapter =
		static_cast<LiveSimulationPhaseGraphOwnerAdapter *>(adapterContext);
	const PhaseInput &input = *static_cast<const PhaseInput *>(
		immutablePhaseInput);
	const AuthorityToken &output = *static_cast<const AuthorityToken *>(
		privateJobOutput);
	const unsigned phaseOrdinalValue = adapter->phaseOrdinal(context.phaseId());

	// validateAuthorityToken established all pointers, byte counts, identity,
	// generation, and canonical order before this non-throwing publication.
	(void)immutablePhaseInputBytes;
	(void)privateJobOutputBytes;
	// Cross the fallback cutoff before entering title code. A nominally
	// non-throwing legacy phase can still throw after partial mutation; graph
	// containment must never reinterpret that state as safe to replay.
	adapter->m_ownerCommitEntered = true;
	try
	{
		adapter->m_stopAfterCommit = !adapter->m_callbacks.commit(
			context.phaseId(), context.generation(), input.frame,
			adapter->m_ownerContext);
	}
	catch (const std::exception &exception)
	{
		adapter->recordException(LIVE_SIMULATION_PHASE_EXCEPTION_STANDARD, exception.what());
		adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_COMMIT_EXCEPTION,
			context.phaseId(), input.frame);
		throw;
	}
	catch (...)
	{
		adapter->recordException(LIVE_SIMULATION_PHASE_EXCEPTION_UNKNOWN, 0);
		adapter->recordFailure(LIVE_SIMULATION_PHASE_FAILURE_COMMIT_EXCEPTION,
			context.phaseId(), input.frame);
		throw;
	}
	LiveSimulationPhaseAuthorityEvidence &evidence =
		adapter->m_evidence[phaseOrdinalValue];
	evidence.committed = true;
	adapter->m_currentSequenceSignature =
		adapter->m_currentSequenceSignature * 10u + context.phaseId();
	++adapter->m_committedPhaseCount;
	++adapter->m_expectedPhaseOrdinal;
	(void)output;
}

} // namespace rts
