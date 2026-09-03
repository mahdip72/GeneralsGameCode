#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

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

unsigned diagnosticFailures = 0;

#if defined(_WIN64)
struct AccountingObservation;
#endif

void requireDiagnostic(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		++diagnosticFailures;
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
		  validationCount(0), commitCount(0), throwDuringValidationPhase(0),
		  standardException(false), annotateFailure(false),
		  ownerFrame(0), ownerPhaseFrame(0), ownerCursor(0),
		  titleExceptionMessage(0)
		  #if defined(_WIN64)
		  , accounting(0)
		  #endif
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
	unsigned throwDuringValidationPhase;
	bool standardException;
	bool annotateFailure;
	unsigned ownerFrame;
	unsigned ownerPhaseFrame;
	unsigned ownerCursor;
	const char *titleExceptionMessage;
	rts::LiveSimulationPhaseFailureDiagnostic nestedFailure;
	#if defined(_WIN64)
	AccountingObservation *accounting;
	#endif
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

#if defined(_WIN64)
struct AccountingClock
{
	AccountingClock() : now(100), reads(0) {}
	rts::JobMetricCounter now;
	unsigned reads;
	static rts::JobMetricCounter read(void *context)
	{
		AccountingClock &clock = *static_cast<AccountingClock *>(context);
		++clock.reads;
		return clock.now++;
	}
};

// The real run owner retains this ledger/token and latches invalidate() on an
// observation failure. A nullable adapter callback transports boundaries only:
// no replacement timer, inferred completed frame, or dispatch-policy result.
struct AccountingObservation
{
	AccountingObservation() : sampleOrdinal(0), actualOwnerFrame(40),
		entryFrame(0), entryGeneration(0), abortedFrames(0),
		failed(false), advanceOwnerWork(false), advanceSchedulerAtVerification(false)
	{
		std::memset(phaseFrames, 0, sizeof(phaseFrames));
		actual.submittedJobs = 17;
		actual.executedJobs = 17;
		actual.ownerHelpJobs = 3;
	}

	rts::performance::KernelPerformanceLedger ledger;
	rts::performance::KernelPerformanceFrame frame;
	rts::performance::KernelPerformanceSchedulerBoundary actual;
	AccountingClock clock;
	rts::JobMetricCounter sampleOrdinal;
	unsigned actualOwnerFrame, entryFrame, entryGeneration, abortedFrames;
	unsigned phaseFrames[5];
	bool failed, advanceOwnerWork, advanceSchedulerAtVerification;
};

void observeAccounting(rts::LiveSimulationPhaseObservationBoundary boundary,
	rts::SimulationPhaseId phaseId, unsigned generation, unsigned ownerFrame,
	void *context)
{
	using namespace rts::performance;
	AccountingObservation &observation =
		*static_cast<Harness *>(context)->accounting;
	bool accepted = true;
	switch (boundary)
	{
	case rts::LIVE_SIMULATION_PHASE_OBSERVE_FRAME_BEGIN:
		observation.entryFrame = ownerFrame;
		observation.entryGeneration = generation;
		observation.frame = observation.ledger.beginFrame(
			++observation.sampleOrdinal, ownerFrame, observation.actual);
		accepted = observation.frame.valid();
		break;
	case rts::LIVE_SIMULATION_PHASE_OBSERVE_PHASE_BEGIN:
		if (phaseId < 1 || phaseId > 5) { accepted = false; break; }
		observation.phaseFrames[phaseId - 1] = ownerFrame;
		accepted = observation.ledger.beginPhase(observation.frame,
			static_cast<KernelPerformancePhase>(phaseId - 1));
		break;
	case rts::LIVE_SIMULATION_PHASE_OBSERVE_PHASE_END:
		accepted = observation.ledger.endPhase(observation.frame,
			static_cast<KernelPerformancePhase>(phaseId - 1));
		break;
	case rts::LIVE_SIMULATION_PHASE_OBSERVE_FRAME_END:
		// Verification increments the real owner's frame after authority was
		// validated. Read that owner state, never ownerFrame + 1.
		accepted = observation.ledger.endFrame(observation.frame,
			observation.actualOwnerFrame, observation.actual);
		if (accepted) observation.frame = KernelPerformanceFrame();
		break;
	case rts::LIVE_SIMULATION_PHASE_OBSERVE_FRAME_ABORT:
		++observation.abortedFrames;
		accepted = false;
		// Preserve any open outer/partial frame and the run's latched failure.
		// In product this is PerformanceReceiptRuntime::invalidate, not a
		// request to reset evidence or change the baseline execution route.
		break;
	default:
		accepted = false;
		break;
	}
	if (!accepted) observation.failed = true;
}

void beginAccountingRun(AccountingObservation &observation,
	rts::performance::KernelPerformanceClock clock, void *clockContext)
{
	rts::performance::KernelPerformanceTimingRunOptions options;
	options.enabled = true;
	options.role = rts::performance::KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = clock;
	options.clockContext = clockContext;
	require(observation.ledger.beginRun(options), "owner accounting fixture starts");
}

rts::performance::KernelPerformanceSnapshot freezeAccounting(
	AccountingObservation &observation)
{
	// Closure errors remain observable in the real snapshot. Do not stop at a
	// setup assertion when the deliberately unimplemented transport is RED.
	observation.ledger.sealAdmissions();
	observation.ledger.sealExecutionClosure(observation.actual);
	return observation.ledger.freeze();
}
#endif

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
	#if defined(_WIN64)
	if (harness->accounting != 0 && harness->accounting->advanceOwnerWork)
	{
		const unsigned work[] = { 2, 3, 5, 7, 11 };
		harness->accounting->clock.now += work[phaseId - 1];
	}
	#endif
	if (harness->cancelDuringValidation)
	{
		harness->cancelDuringValidation = false;
		harness->adapter->requestCancellation();
	}
	if (harness->throwDuringValidationPhase == phaseId)
		throw std::runtime_error("validation sentinel");
	if (harness->validationFailurePhase == phaseId && harness->annotateFailure)
	{
		harness->adapter->annotateOwnerFailure(harness->ownerFrame,
			harness->ownerPhaseFrame, harness->ownerCursor);
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
	#if defined(_WIN64)
	if (harness->accounting != 0 && harness->accounting->advanceOwnerWork)
	{
		const unsigned work[] = { 3, 5, 7, 11, 13 };
		harness->accounting->clock.now += work[phaseId - 1];
	}
	#endif
	if (harness->throwDuringCommitPhase == phaseId)
	{
		if (harness->annotateFailure)
		{
			harness->adapter->annotateOwnerFailure(harness->ownerFrame,
				harness->ownerPhaseFrame, harness->ownerCursor,
				rts::LIVE_SIMULATION_PHASE_EXCEPTION_TITLE_INI,
				harness->titleExceptionMessage);
			// Later generic fallout must not replace the first owner details.
			harness->adapter->annotateOwnerFailure(999, 999, 999,
				rts::LIVE_SIMULATION_PHASE_EXCEPTION_UNKNOWN, "later fallout");
		}
		if (harness->standardException)
			throw std::runtime_error("owner commit sentinel");
		throw phaseId;
	}
	if (harness->reenterDuringCommitPhase == phaseId)
	{
		harness->reenterDuringCommitPhase = 0;
		harness->nestedResult = harness->adapter->runFrame(frame + 1000);
		harness->nestedFailure = harness->adapter->failureDiagnostic();
	}
	if (phaseId == rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE &&
		harness->retargetAfterIntake)
	{
		require(harness->adapter->retargetPendingFrameAfterIntake(
			harness->retargetFrame),
			"intake must be able to retarget never-claimed phase inputs");
		#if defined(_WIN64)
		if (harness->accounting != 0)
			harness->accounting->actualOwnerFrame = harness->retargetFrame;
		#endif
	}
	#if defined(_WIN64)
	if (harness->accounting != 0 &&
		phaseId == rts::LIVE_SIMULATION_PHASE_VERIFICATION_PUBLICATION)
	{
		++harness->accounting->actualOwnerFrame;
		if (harness->accounting->advanceSchedulerAtVerification)
			++harness->accounting->actual.executedJobs;
	}
	#endif
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

void testRejectedValidationRetainsOwnerFrameMismatch()
{
	Harness harness;
	harness.validationFailurePhase = rts::LIVE_SIMULATION_PHASE_SPATIAL;
	harness.annotateFailure = true;
	harness.ownerFrame = 41;
	harness.ownerPhaseFrame = 40;
	harness.ownerCursor = 2;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		adapter.committedPhaseCount() == 2 && harness.commitCount == 2,
		"diagnosing a later validation rejection must preserve the mutation cutoff");
	rts::LiveSimulationPhaseAuthorityEvidence committedEvidence;
	require(adapter.authorityEvidence(1, committedEvidence) && committedEvidence.committed,
		"validation failure must retain the already committed intake authority");
	const rts::LiveSimulationPhaseFailureDiagnostic &failure = adapter.failureDiagnostic();
	requireDiagnostic(failure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_OWNER_VALIDATION &&
		failure.phaseId == 3 && failure.jobKey == 0 && failure.frame == 40 &&
		failure.generation == 1 && failure.internalEpoch != 0 &&
		failure.internalEpoch == committedEvidence.internalEpoch,
		"validation diagnostic must retain the first failing phase and immutable identity");
	requireDiagnostic(failure.committedPhaseCount == 2 && failure.sequenceSignature == 12 &&
		failure.ownerCommitEntered && failure.ownerContextKnown &&
		failure.ownerFrame == 41 && failure.ownerPhaseFrame == 40 && failure.ownerCursor == 2,
		"validation diagnostic must retain owner-frame mismatch and prior real commits");
	requireDiagnostic(failure.graphState == rts::SIMULATION_PHASE_GRAPH_RUNNING &&
		failure.terminalCause == rts::SIMULATION_PHASE_WORK_SUCCEEDED &&
		failure.returnGraphState == rts::SIMULATION_PHASE_GRAPH_FAILED &&
		failure.returnTerminalCause == rts::SIMULATION_PHASE_WORK_FAILED &&
		failure.exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_NONE,
		"validation diagnostic must distinguish rejection from exception and containment fallout");
}

void testValidationExceptionIsNotRelabeledAsRejection()
{
	Harness harness;
	harness.throwDuringValidationPhase = rts::LIVE_SIMULATION_PHASE_SPATIAL;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(42) == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		harness.commitCount == 2 && adapter.committedPhaseCount() == 2,
		"validation exception containment must not repeat any owner callback");
	const rts::LiveSimulationPhaseFailureDiagnostic &failure = adapter.failureDiagnostic();
	requireDiagnostic(failure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_VALIDATE_EXCEPTION &&
		failure.phaseId == 3 && failure.frame == 42 && failure.committedPhaseCount == 2 &&
		failure.exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_STANDARD &&
		std::strcmp(failure.exceptionMessage, "validation sentinel") == 0,
		"validation exception category and original message must survive graph containment");
}

void testPartialCommitExceptionRetainsFirstFailure()
{
	Harness harness;
	harness.throwDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_TAIL;
	harness.standardException = true;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(43) == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		harness.commitCount == 4 && adapter.committedPhaseCount() == 3,
		"a throwing fourth commit must remain unaccepted after partial owner mutation");
	const rts::LiveSimulationPhaseFailureDiagnostic &failure = adapter.failureDiagnostic();
	requireDiagnostic(failure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_COMMIT_EXCEPTION &&
		failure.phaseId == 4 && failure.frame == 43 && failure.committedPhaseCount == 3 &&
		failure.sequenceSignature == 123 && failure.ownerCommitEntered &&
		failure.exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_STANDARD &&
		std::strcmp(failure.exceptionMessage, "owner commit sentinel") == 0 &&
		!failure.exceptionMessageTruncated,
		"partial commit diagnostic must preserve phase, accepted prefix, and original exception text");
	requireDiagnostic(failure.graphState == rts::SIMULATION_PHASE_GRAPH_RUNNING &&
		failure.returnGraphState == rts::SIMULATION_PHASE_GRAPH_FAILED &&
		failure.returnTerminalCause == rts::SIMULATION_PHASE_WORK_FAILED,
		"cleanup must not overwrite the first commit failure with an advance-count failure");
	rts::LiveSimulationPhaseAuthorityEvidence evidence;
	require(adapter.authorityEvidence(4, evidence) && evidence.validated && !evidence.committed,
		"failure diagnostics must never manufacture authority publication");
}

void testUnknownFirstCommitExceptionKeepsUnsafeCutoff()
{
	Harness harness;
	harness.throwDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(44) == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		harness.commitCount == 1 && adapter.committedPhaseCount() == 0,
		"unknown first-commit exception must never authorize a legacy retry");
	const rts::LiveSimulationPhaseFailureDiagnostic &failure = adapter.failureDiagnostic();
	requireDiagnostic(failure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_COMMIT_EXCEPTION &&
		failure.phaseId == 1 && failure.frame == 44 && failure.ownerCommitEntered &&
		failure.committedPhaseCount == 0 && failure.sequenceSignature == 0 &&
		failure.exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_UNKNOWN &&
		failure.exceptionMessage[0] == '\0',
		"unrecognized exception must be explicitly unknown without invented message or commit");
}

void testOwnerExceptionAnnotationIsCopiedBoundedAndFirstWins()
{
	char message[512];
	std::memset(message, 'x', sizeof(message) - 1);
	message[sizeof(message) - 1] = '\0';
	Harness harness;
	harness.throwDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_TAIL;
	harness.annotateFailure = true;
	harness.ownerFrame = 46;
	harness.ownerPhaseFrame = 45;
	harness.ownerCursor = 3;
	harness.titleExceptionMessage = message;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(45) == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION,
		"owner exception annotation must not consume or recover the thrown callback");
	message[0] = 'z';
	const rts::LiveSimulationPhaseFailureDiagnostic &failure = adapter.failureDiagnostic();
	requireDiagnostic(failure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_COMMIT_EXCEPTION &&
		failure.exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_TITLE_INI &&
		failure.ownerContextKnown && failure.ownerFrame == 46 &&
		failure.ownerPhaseFrame == 45 && failure.ownerCursor == 3,
		"first title exception annotation must survive later generic annotations and containment");
	requireDiagnostic(failure.exceptionMessageTruncated &&
		std::strlen(failure.exceptionMessage) == sizeof(failure.exceptionMessage) - 1 &&
		failure.exceptionMessage[0] == 'x' &&
		failure.exceptionMessage[sizeof(failure.exceptionMessage) - 2] == 'x' &&
		failure.exceptionMessage[sizeof(failure.exceptionMessage) - 1] == '\0',
		"exception text must be an owned bounded terminated copy with truthful truncation");
	harness.throwDuringCommitPhase = 0;
	harness.annotateFailure = false;
	require(adapter.runFrame(46) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"fresh top-level frame must preserve existing recovery behavior");
	requireDiagnostic(adapter.failureDiagnostic().boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_NONE &&
		!adapter.failureDiagnostic().ownerContextKnown &&
		adapter.failureDiagnostic().exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_NONE &&
		adapter.failureDiagnostic().exceptionMessage[0] == '\0',
		"fresh top-level frame must not expose a stale exception or owner mismatch");
}

void testNestedFailureDoesNotReplaceOuterAuthority()
{
	Harness harness;
	harness.reenterDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(47) == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		harness.nestedResult == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		adapter.committedPhaseCount() == 5 && harness.commitCount == 5,
		"nested failure diagnostics must leave the outer five owner commits intact");
	requireDiagnostic(harness.nestedFailure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_NESTED_ENTRY &&
		harness.nestedFailure.phaseId == 1 && harness.nestedFailure.frame == 1047 &&
		harness.nestedFailure.generation == 1 && harness.nestedFailure.committedPhaseCount == 0 &&
		harness.nestedFailure.ownerCommitEntered,
		"rejected nested call must expose its requested identity before outer completion");
	requireCanonicalEvidence(adapter, 47, 1);
	requireDiagnostic(adapter.failureDiagnostic().boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_NESTED_ENTRY &&
		adapter.failureDiagnostic().frame == 1047,
		"outer completion must not erase a nested rejection before its owner can report it");
	require(adapter.runFrame(48) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"next nonnested top-level frame must still complete");
	requireDiagnostic(adapter.failureDiagnostic().boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_NONE,
		"next top-level frame must clear the preceding nested diagnostic");
}

void testLaterOuterExceptionCannotRewriteNestedFirstFailure()
{
	Harness harness;
	harness.reenterDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	harness.throwDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_TAIL;
	harness.annotateFailure = true;
	harness.ownerFrame = 52;
	harness.ownerPhaseFrame = 51;
	harness.ownerCursor = 3;
	harness.titleExceptionMessage = "later outer exception";
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(51) == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		harness.nestedResult == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		adapter.committedPhaseCount() == 3 && harness.commitCount == 4,
		"nested rejection followed by an outer throw must retain the original unsafe cutoff and commits");
	const rts::LiveSimulationPhaseFailureDiagnostic &failure = adapter.failureDiagnostic();
	const rts::LiveSimulationPhaseFailureDiagnostic &first = harness.nestedFailure;
	requireDiagnostic(failure.boundary == rts::LIVE_SIMULATION_PHASE_FAILURE_NESTED_ENTRY &&
		failure.phaseId == 1 && failure.frame == 1051 && failure.generation == 1 &&
		failure.internalEpoch == first.internalEpoch && failure.committedPhaseCount == 0 &&
		failure.sequenceSignature == 0 && failure.ownerCommitEntered,
		"later outer failure must preserve the nested rejection identity and accepted prefix");
	requireDiagnostic(!failure.ownerContextKnown &&
		failure.ownerFrame == first.ownerFrame && failure.ownerPhaseFrame == first.ownerPhaseFrame &&
		failure.ownerCursor == first.ownerCursor &&
		failure.exceptionCategory == rts::LIVE_SIMULATION_PHASE_EXCEPTION_NONE &&
		failure.exceptionMessage[0] == '\0' && !failure.exceptionMessageTruncated,
		"later outer exception must not annotate the already claimed nested first failure");
	requireDiagnostic(failure.graphState == first.graphState &&
		failure.terminalCause == first.terminalCause &&
		failure.returnGraphState == first.returnGraphState &&
		failure.returnTerminalCause == first.returnTerminalCause,
		"outer containment must not replace the nested rejection return-state snapshot");
	rts::LiveSimulationPhaseAuthorityEvidence evidence;
	require(adapter.authorityEvidence(4, evidence) && evidence.validated && !evidence.committed,
		"first-failure immutability must not manufacture the later throwing commit");
}

#if defined(_WIN64)
rts::LiveSimulationPhaseOwnerCallbacks accountingCallbacks()
{
	rts::LiveSimulationPhaseOwnerCallbacks result = callbacks();
	result.observe = &observeAccounting;
	return result;
}

// Moving phase observations after the real callbacks, omitting a boundary, or
// substituting legacy telemetry loses these independently specified work costs.
void testOwnerAccountingEnclosesValidationAndCommit()
{
	AccountingObservation observation;
	observation.advanceOwnerWork = true;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		harness.commitCount == 5, "accounting must preserve the real owner commits");
	const auto result = freezeAccounting(observation);
	const auto &a = result.phaseAccounting;
	requireDiagnostic(!observation.failed && a.complete && a.errors == 0,
		"adapter boundaries must close complete real-ledger phase accounting");
	requireDiagnostic(a.completedFrameCount == 1 && a.firstCompletedFrame == 41 &&
		a.lastCompletedFrame == 41 && a.frameNanoseconds == 78 &&
		a.unscopedSerialNanoseconds == 6 && observation.clock.reads == 12,
		"owner work and six graph gaps must partition the independently measured frame");
	const rts::JobMetricCounter totals[] = { 6, 9, 13, 19, 25 };
	for (unsigned index = 0; index != 5; ++index)
	{
		requireDiagnostic(a.phases[index].totalNanoseconds == totals[index] &&
			a.phases[index].serialNanoseconds == totals[index] &&
			a.phases[index].pureNanoseconds == 0 && a.phases[index].samples == 1,
			"both real validation and commit costs remain inside their serial phase");
	}
	requireDiagnostic(a.schedulerClosureKnown && a.schedulerBegin.submittedJobs == 17 &&
		a.schedulerEnd.executedJobs == 17 && a.schedulerEnd.ownerHelpJobs == 3 &&
		result.runRole == rts::performance::KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"owner observations preserve actual historical scheduler counters and latched role");
}

void testOwnerAccountingKeepsUnscopedTimeDistinctFromLegacyTelemetry()
{
	AccountingObservation observation;
	FakeClock accountingClock;
	const rts::JobMetricCounter accountingSamples[] = {
		100, 110, 130, 135, 165, 170, 185, 190, 200, 205, 215, 220
	};
	FakeClock legacyClock;
	const rts::JobMetricCounter legacySamples[] = {
		1000, 1010, 1020, 1025, 1045, 1050, 1080, 1090, 1130, 1150, 1200, 1300
	};
	for (unsigned index = 0; index != 12; ++index)
	{
		accountingClock.values[index] = accountingSamples[index];
		legacyClock.values[index] = legacySamples[index];
	}
	accountingClock.count = legacyClock.count = 12;
	beginAccountingRun(observation, &readFakeClock, &accountingClock);
	Harness harness;
	harness.accounting = &observation;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.setPerformanceClockForTesting(&readFakeClock, &legacyClock) &&
		adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"distinct legacy telemetry and run-owned accounting clocks are accepted");
	const auto result = freezeAccounting(observation);
	const auto &a = result.phaseAccounting;
	requireDiagnostic(!observation.failed && a.complete && a.frameNanoseconds == 120 &&
		a.maximumFrameNanoseconds == 120 && a.unscopedSerialNanoseconds == 35,
		"real ledger accounts graph setup, inter-phase gaps, and the frame tail exactly");
	const rts::JobMetricCounter totals[] = { 20, 30, 15, 10, 10 };
	for (unsigned index = 0; index != 5; ++index)
		requireDiagnostic(a.phases[index].totalNanoseconds == totals[index] &&
			a.phases[index].serialNanoseconds == totals[index] &&
			a.phases[index].pureNanoseconds == 0 &&
			a.phases[index].maximumNanoseconds == totals[index],
			"baseline phase totals come only from the run-owned ledger clock");
	requireDiagnostic(adapter.runtimeMetrics().frameSimulationTotalNanoseconds == 300 &&
		accountingClock.index == 12 && legacyClock.index == 12,
		"legacy telemetry remains independent rather than being copied into accounting");
}

void testOwnerAccountingRetargetUsesActualCompletedFrameAndSurvivesMetricsReset()
{
	AccountingObservation observation;
	observation.actualOwnerFrame = 83;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	harness.retargetAfterIntake = true;
	harness.retargetFrame = 0;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(83) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"accounted intake retarget preserves the real owner path");
	requireDiagnostic(observation.entryFrame == 83 && observation.entryGeneration == 1 &&
		observation.phaseFrames[0] == 83 && observation.phaseFrames[1] == 0 &&
		observation.phaseFrames[4] == 0,
		"measurement transports pre-intake identity and actual retargeted phase identities");
	adapter.resetRuntimeMetrics();
	harness.retargetAfterIntake = false;
	require(adapter.runFrame(1) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"a later frame completes after resetting only legacy telemetry");
	const auto result = freezeAccounting(observation);
	requireDiagnostic(!observation.failed && result.phaseAccounting.complete &&
		result.phaseAccounting.completedFrameCount == 2 &&
		result.phaseAccounting.firstCompletedFrame == 1 &&
		result.phaseAccounting.lastCompletedFrame == 2 &&
		observation.sampleOrdinal == 2 && adapter.runtimeMetrics().completedFrames == 1,
		"run-owned samples survive metrics reset and bind actual post-verification frames");
}

void testOwnerAccountingEarlyStopCannotHideBehindPriorCompleteFrame()
{
	AccountingObservation observation;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"early-stop fixture first completes a real frame");
	harness.stopAfterIntake = true;
	require(adapter.runFrame(41) == rts::LIVE_SIMULATION_PHASE_STOPPED_BY_OWNER &&
		harness.commitCount == 6, "stopped intake does not execute the other four phases");
	const auto result = freezeAccounting(observation);
	const auto &a = result.phaseAccounting;
	requireDiagnostic(observation.failed && observation.abortedFrames == 1 &&
		!a.complete && a.errors != 0 && a.completedFrameCount == 1 &&
		a.firstCompletedFrame == 41 && a.lastCompletedFrame == 41 &&
		a.frameNanoseconds == 11 && a.phases[0].samples == 1 && a.phases[4].samples == 1,
		"stopped owner frame stays incomplete without fabricated phases or erased prior evidence");
	requireDiagnostic(observation.ledger.runRole() ==
		rts::performance::KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"observation failure cannot switch the latched baseline role to physical execution");
}

void testOwnerAccountingRejectedOwnerCannotPublishAFrame()
{
	AccountingObservation observation;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	harness.owner = false;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION &&
		harness.validationCount == 0 && harness.commitCount == 0,
		"measurement does not promote missing owner authority into a commit");
	const auto result = freezeAccounting(observation);
	requireDiagnostic(observation.failed && observation.abortedFrames == 1 &&
		!result.phaseAccounting.complete && result.phaseAccounting.completedFrameCount == 0,
		"owner preflight refusal invalidates qualification without inventing a completed frame");
}

void testOwnerAccountingNestedFailureCannotResetOuterFrameOrQualification()
{
	AccountingObservation observation;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	harness.reenterDuringCommitPhase = rts::LIVE_SIMULATION_PHASE_OWNER_INTAKE;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		harness.nestedResult == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		harness.commitCount == 5, "rejected nested entry preserves the real outer commits");
	const auto result = freezeAccounting(observation);
	requireDiagnostic(observation.failed && observation.abortedFrames == 1 &&
		observation.sampleOrdinal == 1 && result.phaseAccounting.complete &&
		result.phaseAccounting.completedFrameCount == 1 &&
		result.phaseAccounting.lastCompletedFrame == 41,
		"nested rejection latches run failure while preserving exactly one outer measurement");
}

void testOwnerAccountingObservesActualSchedulerEndWithoutChangingGameplay()
{
	AccountingObservation observation;
	observation.advanceSchedulerAtVerification = true;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(accountingCallbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		harness.commitCount == 5, "scheduler evidence failure does not select owner dispatch");
	const auto result = freezeAccounting(observation);
	requireDiagnostic(observation.failed && !result.phaseAccounting.complete &&
		result.phaseAccounting.schedulerBegin.executedJobs == 17 &&
		result.phaseAccounting.schedulerEnd.executedJobs == 18 &&
		result.runRole == rts::performance::KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"unexpected real scheduler work fails accounting instead of copying the entry boundary");
}

void testAbsentOwnerObserverDoesNotReadDiagnosticsClock()
{
	AccountingObservation observation;
	beginAccountingRun(observation, &AccountingClock::read, &observation.clock);
	Harness harness;
	harness.accounting = &observation;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks(), &harness);
	harness.adapter = &adapter;
	require(adapter.runFrame(40) == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		harness.commitCount == 5 && rts::HasStableLiveSimulationPhaseEvidence(adapter.runtimeMetrics()),
		"default nullable observation transport preserves ordinary owner execution");
	const auto result = observation.ledger.freeze();
	requireDiagnostic(observation.clock.reads == 0 && observation.sampleOrdinal == 0 &&
		result.phaseAccounting.completedFrameCount == 0 && !result.phaseAccounting.complete,
		"absent observer never reads the diagnostic clock or fabricates frame coverage");
}
#endif

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
	testRejectedValidationRetainsOwnerFrameMismatch();
	testValidationExceptionIsNotRelabeledAsRejection();
	testPartialCommitExceptionRetainsFirstFailure();
	testUnknownFirstCommitExceptionKeepsUnsafeCutoff();
	testOwnerExceptionAnnotationIsCopiedBoundedAndFirstWins();
	testNestedFailureDoesNotReplaceOuterAuthority();
	testLaterOuterExceptionCannotRewriteNestedFirstFailure();
	#if defined(_WIN64)
	testOwnerAccountingEnclosesValidationAndCommit();
	testOwnerAccountingKeepsUnscopedTimeDistinctFromLegacyTelemetry();
	testOwnerAccountingRetargetUsesActualCompletedFrameAndSurvivesMetricsReset();
	testOwnerAccountingEarlyStopCannotHideBehindPriorCompleteFrame();
	testOwnerAccountingRejectedOwnerCannotPublishAFrame();
	testOwnerAccountingNestedFailureCannotResetOuterFrameOrQualification();
	testOwnerAccountingObservesActualSchedulerEndWithoutChangingGameplay();
	testAbsentOwnerObserverDoesNotReadDiagnosticsClock();
	#endif
	if (diagnosticFailures != 0)
		return 1;
	std::printf("SimulationPhaseGraph owner-adapter tests passed.\n");
	return 0;
}
