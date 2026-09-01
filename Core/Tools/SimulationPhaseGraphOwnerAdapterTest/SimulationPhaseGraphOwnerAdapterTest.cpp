#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#include <cstdio>
#include <cstdlib>

namespace
{

void require(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}

struct Harness
{
	Harness()
		: adapter(0), owner(true), stopAfterIntake(false),
		  cancelDuringValidation(false), validationFailurePhase(0),
		  throwDuringCommitPhase(0), retargetAfterIntake(false),
		  retargetFrame(0), reenterDuringCommitPhase(0),
		  nestedResult(rts::LIVE_SIMULATION_PHASE_COMPLETED),
		  validationCount(0), commitCount(0)
	{
		unsigned index;
		for (index = 0; index != 32; ++index)
		{
			validated[index] = 0;
			committed[index] = 0;
			validatedFrame[index] = 0;
		}
	}

	rts::LiveSimulationPhaseGraphOwnerAdapter *adapter;
	bool owner;
	bool stopAfterIntake;
	bool cancelDuringValidation;
	unsigned validationFailurePhase;
	unsigned throwDuringCommitPhase;
	bool retargetAfterIntake;
	unsigned retargetFrame;
	unsigned reenterDuringCommitPhase;
	rts::LiveSimulationPhaseRunResult nestedResult;
	unsigned validated[32];
	unsigned committed[32];
	unsigned validatedFrame[32];
	unsigned validationCount;
	unsigned commitCount;
};

struct FakeClock
{
	FakeClock() : count(0), index(0) {}
	rts::JobMetricCounter values[32];
	unsigned count;
	unsigned index;
};

rts::JobMetricCounter readFakeClock(void *context)
{
	FakeClock *clock = static_cast<FakeClock *>(context);
	if (clock == 0 || clock->count == 0)
		return 0;
	if (clock->index >= clock->count)
		return clock->values[clock->count - 1];
	return clock->values[clock->index++];
}

bool isOwner(void *context)
{
	return static_cast<Harness *>(context)->owner;
}

bool validate(rts::SimulationPhaseId phaseId, unsigned generation,
	unsigned frame, void *context)
{
	Harness *harness = static_cast<Harness *>(context);
	require(generation != 0, "validation must receive a nonzero generation");
	harness->validated[harness->validationCount] = phaseId;
	harness->validatedFrame[harness->validationCount] = frame;
	++harness->validationCount;
	if (harness->cancelDuringValidation)
	{
		harness->cancelDuringValidation = false;
		harness->adapter->requestCancellation();
	}
	return harness->validationFailurePhase != phaseId;
}

bool commit(rts::SimulationPhaseId phaseId, unsigned generation,
	unsigned frame, void *context)
{
	Harness *harness = static_cast<Harness *>(context);
	require(generation != 0, "commit must receive a nonzero generation");
	require(harness->commitCount < 32, "commit trace overflow");
	harness->committed[harness->commitCount++] = phaseId;
	if (harness->throwDuringCommitPhase == phaseId)
		throw phaseId;
	if (harness->reenterDuringCommitPhase == phaseId)
	{
		harness->reenterDuringCommitPhase = 0;
		harness->nestedResult = harness->adapter->runFrame(frame + 1000);
	}
	if (phaseId == rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE &&
		harness->retargetAfterIntake)
	{
		require(harness->adapter->retargetPendingFrameAfterIntake(
			harness->retargetFrame),
			"intake must be able to retarget never-claimed phase inputs");
	}
	(void)frame;
	return !(phaseId == rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE &&
		harness->stopAfterIntake);
}

rts::LiveSimulationPhaseOwnerCallbacks callbacks()
{
	rts::LiveSimulationPhaseOwnerCallbacks result;
	result.isOwner = &isOwner;
	result.validate = &validate;
	result.commit = &commit;
	return result;
}

void requireCanonicalEvidence(
	const rts::LiveSimulationPhaseGraphOwnerAdapter &adapter,
	unsigned frame, unsigned generation)
{
	unsigned phaseId;
	for (phaseId = rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
		phaseId != rts::LIVE_SIMULATION_PHASE_COUNT; ++phaseId)
	{
		rts::LiveSimulationPhaseAuthorityEvidence evidence;
		require(adapter.authorityEvidence(phaseId, evidence),
			"every committed phase must publish authority evidence");
		require(evidence.phaseId == phaseId && evidence.jobKey == 0,
			"authority identity must use canonical phase and job keys");
		require(evidence.frame == frame && evidence.generation == generation &&
			evidence.internalEpoch != 0,
			"authority evidence must retain frame and private epoch identity");
		require(evidence.executionKind ==
			rts::SIMULATION_PHASE_EXECUTION_OWNER_HELP &&
			evidence.physicalWorkerIndex ==
				rts::SIMULATION_PHASE_INVALID_WORKER_INDEX,
			"live owner-only phases must never claim physical-worker authority");
		require(evidence.validated && evidence.committed,
			"authority evidence must distinguish validation and commit");
	}
}

void testCanonicalDependenciesAndAuthority()
{
	require(!rts::ShouldUseLiveSimulationPhaseGraph(false, false, 0, 3),
		"legacy runtime must retain the legacy phase path");
	require(rts::ShouldUseLiveSimulationPhaseGraph(true, false, 0, 3),
		"fresh native runtime must use the live phase graph");
	require(!rts::ShouldUseLiveSimulationPhaseGraph(true, true, 0, 3) &&
		!rts::ShouldUseLiveSimulationPhaseGraph(true, true, 2, 3) &&
		rts::ShouldUseLiveSimulationPhaseGraph(true, true, 3, 3),
		"only the exact current replay epoch may use the live phase graph");
	require(rts::IsLiveSimulationPhaseReleaseWorkerCount(1) &&
		rts::IsLiveSimulationPhaseReleaseWorkerCount(2) &&
		rts::IsLiveSimulationPhaseReleaseWorkerCount(4) &&
		rts::IsLiveSimulationPhaseReleaseWorkerCount(8) &&
		rts::IsLiveSimulationPhaseReleaseWorkerCount(16) &&
		!rts::IsLiveSimulationPhaseReleaseWorkerCount(0) &&
		!rts::IsLiveSimulationPhaseReleaseWorkerCount(3),
		"release phase evidence must identify the 1/2/4/8/16 worker matrix");
	rts::SimulationPhaseId dependency = 99;
	require(!rts::LiveSimulationPhaseGraphOwnerAdapter::canonicalDependency(
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE, dependency) &&
		dependency == rts::SIMULATION_PHASE_INVALID_ID,
		"owner intake must have no dependency");
	unsigned phaseId;
	for (phaseId = rts::LIVE_SIMULATION_PHASE_LEGACY_MUTABLE_ISLAND;
		phaseId != rts::LIVE_SIMULATION_PHASE_COUNT; ++phaseId)
	{
		require(rts::LiveSimulationPhaseGraphOwnerAdapter::canonicalDependency(
			phaseId, dependency) && dependency + 1 == phaseId,
			"live phases must form the explicit canonical chain");
	}

	Harness harness;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(41) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"canonical live frame must complete");
	require(adapter.configurationStatus() ==
		rts::SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID,
		"live adapter graph must configure successfully");
	require(harness.commitCount == 5 && adapter.committedPhaseCount() == 5,
		"canonical live frame must commit exactly five phases");
	for (phaseId = 0; phaseId != 5; ++phaseId)
		require(harness.committed[phaseId] == phaseId + 1,
			"owner commits must follow canonical phase order");
	const rts::LiveSimulationPhaseRuntimeMetrics &metrics =
		adapter.runtimeMetrics();
	require(metrics.attemptedFrames == 1 && metrics.completedFrames == 1 &&
		metrics.stableSequenceFrames == 1 && metrics.committedPhases == 5,
		"completed live frame must publish one stable five-phase sequence");
	require(metrics.sequenceViolationFrames == 0 &&
		metrics.lastFrame == 41 && metrics.lastGeneration == 1 &&
		metrics.lastCommittedPhaseCount == 5 &&
		metrics.lastSequenceSignature == 12345,
		"live runtime evidence must retain canonical frame and sequence identity");
	require(rts::HasStableLiveSimulationPhaseEvidence(metrics),
		"canonical runtime evidence must prove every attempted frame was stable");
	requireCanonicalEvidence(adapter, 41, 1);
}

void testGenerationDoesNotReuseWorldFrame()
{
	Harness harness;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(9) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"first generation must complete");
	require(adapter.generation() == 1, "first generation must be one");
	require(adapter.runFrame(9) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"repeated frozen world-frame value must use a new graph generation");
	require(adapter.generation() == 2, "repeated frame must not reuse generation");
	require(adapter.runFrame(0) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"world-frame rollback must not roll back graph generation");
	require(adapter.generation() == 3,
		"world-frame rollback must retain monotonic graph generation");
	requireCanonicalEvidence(adapter, 0, 3);
}

void testPostIntakeFrameRetarget()
{
	Harness harness;
	harness.retargetAfterIntake = true;
	harness.retargetFrame = 0;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(83) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"new-game frame retarget must complete");
	require(harness.validatedFrame[0] == 83,
		"intake authority must retain its entry frame");
	unsigned index;
	for (index = 1; index != 5; ++index)
		require(harness.validatedFrame[index] == 0,
			"never-claimed phases must adopt the post-intake frame");
	rts::LiveSimulationPhaseAuthorityEvidence intake;
	rts::LiveSimulationPhaseAuthorityEvidence tail;
	require(adapter.authorityEvidence(
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE, intake) && intake.frame == 83,
		"intake evidence must retain the pre-reset frame");
	require(adapter.authorityEvidence(
		rts::LIVE_SIMULATION_PHASE_VERIFICATION_PUBLICATION, tail) &&
		tail.frame == 0,
		"later evidence must use the post-reset frame");
}

void testOwnerStopCancelsOnlyUncommittedPhases()
{
	Harness harness;
	harness.stopAfterIntake = true;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(12) ==
		rts::LIVE_SIMULATION_PHASE_STOPPED_BY_OWNER,
		"time-frozen intake must stop the live update");
	require(harness.commitCount == 1 && harness.committed[0] ==
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE,
		"time-frozen intake must not mutate later phases");
	require(adapter.runtimeMetrics().stoppedByOwnerFrames == 1 &&
		adapter.runtimeMetrics().stableSequenceFrames == 0,
		"intentional owner stop must not claim a stable full-frame sequence");
	harness.stopAfterIntake = false;
	require(adapter.runFrame(12) == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		adapter.generation() == 2,
		"post-freeze attempt must use a fresh generation and complete");
	require(adapter.runtimeMetrics().attemptedFrames == 2 &&
		adapter.runtimeMetrics().completedFrames == 1 &&
		adapter.runtimeMetrics().stableSequenceFrames == 1 &&
		adapter.runtimeMetrics().committedPhases == 6,
		"runtime evidence must distinguish stopped and completed attempts");
}

void testFailureAndCancellationFallbackBoundary()
{
	Harness harness;
	harness.validationFailurePhase =
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(1) ==
		rts::LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION,
		"first-phase validation failure must permit legacy fallback");
	require(harness.commitCount == 0 && adapter.committedPhaseCount() == 0,
		"preflight failure must not publish owner mutation");

	harness.validationFailurePhase = 0;
	harness.cancelDuringValidation = true;
	require(adapter.runFrame(2) ==
		rts::LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION,
		"pre-commit cancellation must permit legacy fallback");
	require(harness.commitCount == 0,
		"pre-commit cancellation must not publish owner mutation");

	harness.validationFailurePhase = rts::LIVE_SIMULATION_PHASE_SPATIAL;
	require(adapter.runFrame(3) ==
		rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION,
		"later validation failure must fail closed after mutation");
	require(harness.commitCount == 2 &&
		harness.committed[0] == rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE &&
		harness.committed[1] ==
			rts::LIVE_SIMULATION_PHASE_LEGACY_MUTABLE_ISLAND,
		"later failure must never replay or duplicate committed phases");
}

void testOwnerLossFallsBackBeforeConfiguration()
{
	Harness harness;
	harness.owner = false;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(5) ==
		rts::LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION,
		"missing owner attachment must fail before configuration and mutation");
	require(harness.commitCount == 0,
		"owner loss must not publish a phase");
	harness.owner = true;
	require(adapter.runFrame(5) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"adapter must configure when owner attachment becomes valid");
}

void testThrowAfterOwnerMutationNeverFallsBack()
{
	Harness harness;
	harness.throwDuringCommitPhase =
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(6) ==
		rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION,
		"throw after entering owner mutation must never permit legacy replay");
	require(harness.commitCount == 1 && adapter.committedPhaseCount() == 0,
		"throwing callback may mutate even though graph commit was not accepted");
	rts::LiveSimulationPhaseAuthorityEvidence evidence;
	require(adapter.authorityEvidence(
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE, evidence) &&
		evidence.validated && !evidence.committed,
		"throwing owner publication must retain validation without false commit evidence");
}

void testCallbackFrameReentryCannotRewriteOuterAttempt()
{
	Harness harness;
	harness.reenterDuringCommitPhase =
		rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(14) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"outer frame must remain intact after rejected callback reentry");
	require(harness.nestedResult ==
		rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION,
		"nested frame entry must never authorize legacy fallback");
	require(adapter.generation() == 1 && adapter.committedPhaseCount() == 5 &&
		harness.commitCount == 5,
		"nested entry must not reset generation or duplicate owner commits");
	require(adapter.runtimeMetrics().attemptedFrames == 2 &&
		adapter.runtimeMetrics().completedFrames == 1 &&
		adapter.runtimeMetrics().stableSequenceFrames == 1 &&
		adapter.runtimeMetrics().failedAfterMutationFrames == 1 &&
		adapter.runtimeMetrics().committedPhases == 5,
		"rejected nested entry must not duplicate outer-frame commit evidence");
	unsigned index;
	for (index = 0; index != 5; ++index)
		require(harness.committed[index] == index + 1,
			"outer commit trace must retain canonical order after reentry");
	requireCanonicalEvidence(adapter, 14, 1);
}

void testPerformanceEvidenceUsesInjectedClockAndSaturates()
{
	Harness harness;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	FakeClock clock;
	const rts::JobMetricCounter samples[] = {
		100, 110, 120, 125, 145, 150, 180, 190, 230, 250, 300, 400
	};
	unsigned index;
	for (index = 0; index != sizeof(samples) / sizeof(samples[0]); ++index)
		clock.values[index] = samples[index];
	clock.count = sizeof(samples) / sizeof(samples[0]);
	require(adapter.setPerformanceClockForTesting(&readFakeClock, &clock),
		"inactive adapter must accept a deterministic performance clock");
	require(adapter.runFrame(21) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"fake-clock performance frame must complete");
	const rts::LiveSimulationPhaseRuntimeMetrics &metrics =
		adapter.runtimeMetrics();
	const rts::JobMetricCounter expectedPhaseNanoseconds[5] = {
		10, 20, 30, 40, 50
	};
	for (index = 0; index != 5; ++index)
	{
		require(metrics.ownerPhaseTotalNanoseconds[index] ==
			expectedPhaseNanoseconds[index] &&
			metrics.ownerPhaseMaximumNanoseconds[index] ==
				expectedPhaseNanoseconds[index] &&
			metrics.ownerPhaseSampleCount[index] == 1,
			"fake clock must publish exact owner-phase total/max/sample evidence");
	}
	require(metrics.frameSimulationTotalNanoseconds == 300 &&
		metrics.frameSimulationMaximumNanoseconds == 300 &&
		metrics.frameSimulationSampleCount == 1 &&
		metrics.serialIslandTotalNanoseconds == 20 &&
		metrics.serialIslandMaximumNanoseconds == 20 &&
		metrics.serialIslandSampleCount == 1,
		"fake clock must publish exact frame and serial-island evidence");
	adapter.resetRuntimeMetrics();
	require(adapter.runtimeMetrics().frameSimulationSampleCount == 0 &&
		adapter.runtimeMetrics().ownerPhaseSampleCount[0] == 0,
		"match reset must clear phase performance evidence");

	const rts::JobMetricCounter maximum =
		~static_cast<rts::JobMetricCounter>(0);
	rts::JobMetricCounter directDurations[5] = { 1, maximum - 5, 3, 4, 5 };
	require(adapter.recordDirectFramePerformance(directDurations, 5,
		maximum - 5), "direct lane must publish measured phase evidence");
	directDurations[1] = 10;
	require(adapter.recordDirectFramePerformance(directDurations, 5, 10),
		"direct lane must accept a second performance sample");
	require(adapter.runtimeMetrics().serialIslandTotalNanoseconds == maximum &&
		adapter.runtimeMetrics().frameSimulationTotalNanoseconds == maximum &&
		adapter.runtimeMetrics().serialIslandSampleCount == 2 &&
		adapter.runtimeMetrics().frameSimulationSampleCount == 2,
		"performance totals must saturate instead of wrapping");
	require(rts::LIVE_SIMULATION_PHASE_PERFORMANCE_SCHEMA_VERSION == 1,
		"phase performance schema marker must remain explicit");
}

} // namespace

int main()
{
	testCanonicalDependenciesAndAuthority();
	testGenerationDoesNotReuseWorldFrame();
	testPostIntakeFrameRetarget();
	testOwnerStopCancelsOnlyUncommittedPhases();
	testFailureAndCancellationFallbackBoundary();
	testOwnerLossFallsBackBeforeConfiguration();
	testThrowAfterOwnerMutationNeverFallsBack();
	testCallbackFrameReentryCannotRewriteOuterAttempt();
	testPerformanceEvidenceUsesInjectedClockAndSaturates();
	std::printf("SimulationPhaseGraph owner-adapter tests passed.\n");
	return 0;
}
