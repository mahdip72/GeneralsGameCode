/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Each native title utility links the real receipt runtime and ledgers. The
// generated include contains that title's real callback factory, borrowing
// guards, observer and validate/commit methods. Only world phase bodies are
// controlled here; this is transport/accounting evidence, not a playable
// phase-baseline role or a source/workload qualification fixture.
#include "Common/GameThreadOwnership.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

namespace performance_receipt_owner_bridge_fixture
{
using namespace rts::performance;

unsigned failures = 0;

void Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

void UnexpectedFatal(const char *message)
{
	fprintf(stderr, "Unexpected phase bridge fatal: %s\n", message);
	exit(2);
}

#undef RELEASE_CRASH
#define RELEASE_CRASH(message) UnexpectedFatal(message)

class GameLogic
{
public:
	GameLogic(unsigned frame = 0, bool retarget = false)
		: m_stage5PhaseGraph(makeStage5PhaseGraphCallbacks(), this),
		  m_stage5PhaseCursor(0), m_stage5PhaseNow(0),
		  m_performanceReceiptRuntime(0), m_isInUpdate(FALSE),
		  m_frame(frame), m_retarget(retarget), phaseCalls(0),
		  mutationDuringIntake(0), attachedDuringIntake(true),
		  detachedDuringIntake(true)
	{
	}

	// All these definitions come from the selected title, not from this shell.
	static rts::LiveSimulationPhaseOwnerCallbacks makeStage5PhaseGraphCallbacks();
	static bool isStage5PhaseGraphOwner(void *ownerContext);
	static bool validateStage5PhaseGraphCommit(unsigned phaseId,
		unsigned generation, unsigned frame, void *ownerContext);
	static bool commitStage5PhaseGraphPhase(unsigned phaseId,
		unsigned generation, unsigned frame, void *ownerContext);
	static void observeStage5PhaseGraphBoundary(
		rts::LiveSimulationPhaseObservationBoundary boundary,
		rts::SimulationPhaseId phaseId, unsigned generation,
		unsigned frame, void *ownerContext) noexcept;
	Bool getStage5PhaseAuthorityEvidence(UnsignedInt phaseId,
		rts::LiveSimulationPhaseAuthorityEvidence &evidence) const;
	bool attachPerformanceReceiptRuntime(PerformanceReceiptRuntime *runtime);
	bool detachPerformanceReceiptRuntime(PerformanceReceiptRuntime *expectedRuntime);

	Bool isInGameLogicUpdate() const { return m_isInUpdate; }
	UnsignedInt getFrame() { return m_frame; }

	rts::LiveSimulationPhaseRunResult update()
	{
		m_isInUpdate = TRUE;
		m_stage5PhaseCursor = 0;
		const rts::LiveSimulationPhaseRunResult result =
			m_stage5PhaseGraph.runFrame(getFrame());
		m_isInUpdate = FALSE;
		return result;
	}

	Bool runOwnerIntakePhase(UnsignedInt &now)
	{
		++phaseCalls;
		now = getFrame();
		if (m_retarget) m_frame = 0;
		if (mutationDuringIntake != 0)
		{
			attachedDuringIntake = attachPerformanceReceiptRuntime(mutationDuringIntake);
			detachedDuringIntake = detachPerformanceReceiptRuntime(mutationDuringIntake);
		}
		return TRUE;
	}
	void runLegacyMutableIslandPhase(UnsignedInt) { ++phaseCalls; }
	void runSpatialPhase() { ++phaseCalls; }
	void runOwnerTailPhase() { ++phaseCalls; }
	void runVerificationAndPublicationPhase() { ++phaseCalls; ++m_frame; }

	rts::LiveSimulationPhaseGraphOwnerAdapter m_stage5PhaseGraph;
	UnsignedInt m_stage5PhaseCursor, m_stage5PhaseNow;
	PerformanceReceiptRuntime *m_performanceReceiptRuntime;
	Bool m_isInUpdate;
	UnsignedInt m_frame;
	bool m_retarget;
	unsigned phaseCalls;
	PerformanceReceiptRuntime *mutationDuringIntake;
	bool attachedDuringIntake, detachedDuringIntake;
};

#include "PerformanceReceiptOwnerBridgeTitleMethods.inc"

class ReceiptEnvironment
{
public:
	void set(const char *name, const char *value)
	{
		Saved saved;
		saved.name = name;
		const DWORD length = GetEnvironmentVariableA(name, 0, 0);
		saved.present = length != 0;
		if (saved.present)
		{
			std::vector<char> buffer(length);
			GetEnvironmentVariableA(name, &buffer[0], length);
			saved.value = &buffer[0];
		}
		m_saved.push_back(saved);
		Check(SetEnvironmentVariableA(name, value) != 0,
			"bridge fixture sets process-local receipt environment");
	}
	~ReceiptEnvironment()
	{
		for (std::size_t index = m_saved.size(); index != 0; --index)
		{
			const Saved &saved = m_saved[index - 1];
			SetEnvironmentVariableA(saved.name.c_str(),
				saved.present ? saved.value.c_str() : 0);
		}
	}
private:
	struct Saved { std::string name, value; bool present; };
	std::vector<Saved> m_saved;
};

struct Clock
{
	Clock() : reads(0) {}
	static rts::JobMetricCounter Read(void *context)
	{
		Clock &clock = *static_cast<Clock *>(context);
		return 100 + 10 * clock.reads++;
	}
	unsigned reads;
};

bool BeginRuntime(PerformanceReceiptRuntime &runtime)
{
	const bool begun = runtime.begin("fresh-ai-map", "");
	Check(begun && runtime.active(), "bridge uses the actual begun receipt runtime");
	return begun;
}

void BeginControlledTiming(Clock &clock)
{
	// Replace only the fixture's observation ledger configuration, after real
	// Runtime::begin has run unchanged. No production role/parser is enabled.
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	ledger.freeze();
	KernelPerformanceTimingRunOptions options;
	options.enabled = true;
	options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = &Clock::Read;
	options.clockContext = &clock;
	Check(ledger.beginRun(options), "controlled timing fixture begins the existing ledger");
}

KernelPerformanceSnapshot CloseTiming(PerformanceReceiptRuntime &runtime,
	unsigned actualFrame)
{
	// The real accepted terminal hook seals ingress. Only execution closure is
	// supplied here; no workload samples, receipt files or success are invented.
	runtime.captureTerminalResult(actualFrame, 0x89ABCDEFU, true, true);
	rts::JobSystem &jobs = rts::JobSystem::instance();
	const rts::JobSystemMetrics metrics = jobs.metrics();
	KernelPerformanceSchedulerBoundary actual;
	actual.submittedJobs = metrics.submittedJobCount;
	actual.executedJobs = metrics.executedJobCount;
	actual.ownerHelpJobs = metrics.ownerHelpCount;
	actual.outstandingJobs = jobs.outstandingJobCount();
	actual.pendingJobs = jobs.pendingOwnerCompletionCount();
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	Check(ledger.sealExecutionClosure(actual), "real completed prefix accepts actual scheduler closure");
	KernelPerformanceReferenceLedger::instance().freeze();
	return ledger.freeze();
}

void TestOrdinaryRuntimeKeepsPipelineConfiguration()
{
	PerformanceReceiptRuntime runtime;
	if (!BeginRuntime(runtime)) return;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	Check(ledger.runRole() == KERNEL_PERFORMANCE_PIPELINE,
		"ordinary Runtime::begin retains its production pipeline role");
	const KernelPerformanceBatch retained =
		ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
	Check(retained.valid(), "ordinary runtime owns the existing pipeline ledger");
	GameLogic logic;
	Check(logic.attachPerformanceReceiptRuntime(&runtime), "ordinary runtime attaches to real title bridge");
	Check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		logic.getFrame() == 1 && logic.phaseCalls == 5,
		"ordinary attached bridge leaves real phase dispatch unchanged");
	runtime.captureTerminalResult(1, 0x89ABCDEFU, true, true);
	Check(!ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 2, 2).valid(),
		"ordinary real terminal still seals the original ledger");
	Check(ledger.endBatch(retained, KERNEL_PERFORMANCE_NOT_ADMITTED),
		"ordinary retained pipeline token survives title observation");
	const KernelPerformanceSnapshot snapshot = ledger.freeze();
	Check(snapshot.complete && !snapshot.phaseAccounting.requested &&
		snapshot.phaseAccounting.completedFrameCount == 0,
		"ordinary real begin never becomes a phase-baseline receipt");
	Check(logic.detachPerformanceReceiptRuntime(&runtime), "ordinary expected owner detaches");
	KernelPerformanceReferenceLedger::instance().freeze();
}

void TestNormalAndRetargetedCompletion()
{
	for (unsigned scenario = 0; scenario != 2; ++scenario)
	{
		PerformanceReceiptRuntime runtime;
		if (!BeginRuntime(runtime)) return;
		Clock clock;
		BeginControlledTiming(clock);
		GameLogic logic(scenario == 0 ? 0 : 83, scenario != 0);
		Check(logic.attachPerformanceReceiptRuntime(&runtime), "timed runtime attaches to actual title factory");
		Check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
			logic.getFrame() == 1 && logic.phaseCalls == 5,
			"normal or retargeted title commits each real phase once");
		const KernelPerformanceSnapshot snapshot = CloseTiming(runtime, logic.getFrame());
		const KernelPerformancePhaseAccountingSnapshot &phase = snapshot.phaseAccounting;
		Check(phase.complete && phase.errors == 0 && phase.completedFrameCount == 1 &&
			phase.firstCompletedFrame == 1 && phase.lastCompletedFrame == 1,
			"title observer supplies actual completed frame one, including entry 83 retarget");
		Check(clock.reads == 12 && phase.frameNanoseconds == 110 &&
			phase.unscopedSerialNanoseconds == 60,
			"actual runtime accounts five phases and all frame gaps with one ledger clock");
		for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
			Check(phase.phases[index].samples == 1 &&
				phase.phases[index].totalNanoseconds == 10 &&
				phase.phases[index].serialNanoseconds == 10 &&
				phase.phases[index].pureNanoseconds == 0,
				"actual runtime explicitly maps each title phase into the same ledger");
		Check(logic.detachPerformanceReceiptRuntime(&runtime), "timed expected owner detaches");
	}
}

void TestNullAndInactiveAttachmentDoNotObserve()
{
	Clock clock;
	BeginControlledTiming(clock);
	GameLogic logic;
	PerformanceReceiptRuntime inactive;
	Check(!logic.attachPerformanceReceiptRuntime(0) &&
		!logic.attachPerformanceReceiptRuntime(&inactive),
		"null and inactive owners cannot be attached");
	Check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		logic.getFrame() == 1 && logic.phaseCalls == 5,
		"unattached title still executes its existing frame");
	Check(clock.reads == 0, "no borrowed runtime means no Diagnostics clock read");
	Check(!logic.detachPerformanceReceiptRuntime(&inactive),
		"detach cannot claim ownership of an unattached runtime");
	KernelPerformanceLedger::instance().freeze();
}

void TestReplacementAndInUpdateMutationPreserveBorrow()
{
	PerformanceReceiptRuntime replacement, runtime;
	if (!BeginRuntime(replacement)) return;
	// A rejected replacement only needs an active owner identity, not a second
	// live ledger. Keep its real lifecycle active but freeze its fixture data.
	KernelPerformanceLedger::instance().freeze();
	KernelPerformanceReferenceLedger::instance().freeze();
	if (!BeginRuntime(runtime)) return;
	Clock clock;
	BeginControlledTiming(clock);
	GameLogic logic;
	Check(logic.attachPerformanceReceiptRuntime(&runtime) &&
		logic.attachPerformanceReceiptRuntime(&runtime),
		"same active owner may confirm its existing borrow outside update");
	Check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"first borrowed frame completes");
	Check(!logic.attachPerformanceReceiptRuntime(&replacement) &&
		!logic.detachPerformanceReceiptRuntime(&replacement),
		"different active owner cannot replace or detach the borrowed run");
	logic.mutationDuringIntake = &runtime;
	Check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		!logic.attachedDuringIntake && !logic.detachedDuringIntake,
		"attachment and detachment are rejected during real owner commit");
	const KernelPerformanceSnapshot snapshot = CloseTiming(runtime, logic.getFrame());
	Check(snapshot.phaseAccounting.complete && snapshot.phaseAccounting.errors == 0 &&
		snapshot.phaseAccounting.completedFrameCount == 2 &&
		snapshot.phaseAccounting.lastCompletedFrame == 2 && clock.reads == 24,
		"rejected mutations preserve the original runtime token and ordinal progression");
	Check(logic.detachPerformanceReceiptRuntime(&runtime), "matching runtime releases its borrow");
	const unsigned reads = clock.reads;
	Check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		logic.getFrame() == 3 && clock.reads == reads,
		"detached title continues gameplay without observing the completed run");
	Check(!logic.detachPerformanceReceiptRuntime(&runtime), "borrow releases exactly once");
}
} // namespace performance_receipt_owner_bridge_fixture

int RunPerformanceReceiptOwnerBridgeTitleTests()
{
	using namespace performance_receipt_owner_bridge_fixture;
	failures = 0;
	Check(!rts::JobSystem::instance().isRunning(), "bridge fixture must not overlap workers");
	if (failures != 0) return 1;
	const bool attachOwner = !GameThreadOwnership::IsAttached();
	if (attachOwner) GameThreadOwnership::AttachCurrentThread();
	Check(GameThreadOwnership::IsCurrentThread(), "bridge fixture uses the actual game owner thread");
	if (failures != 0) return 1;
	ReceiptEnvironment environment;
	const char *values[][2] = {
		{ "RTS_PERFORMANCE_ROLE", "performance-report" },
		{ "RTS_PERFORMANCE_RUN_ID", "phase-owner-bridge-no-publication" },
		{ "RTS_PERFORMANCE_RUN_NONCE", "11111111-1111-4111-8111-111111111111" },
		{ "RTS_PERFORMANCE_COHORT_NONCE", "22222222-2222-4222-8222-222222222222" },
		{ "RTS_PERFORMANCE_COHORT_CREATED_UTC", "2026-01-01T00:00:00Z" },
		{ "RTS_PERFORMANCE_RECEIPT_DIR", "phase-owner-bridge-no-publication" },
		{ "RTS_PERFORMANCE_SOURCE_COMMIT", "0123456789abcdef0123456789abcdef01234567" },
		{ "RTS_PERFORMANCE_ARTIFACT_SET_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		{ "RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
		{ "RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
		{ "RTS_PERFORMANCE_FIXTURE_ID", "phase-owner-bridge-fixture" },
		{ "RTS_PERFORMANCE_RAW_LOG_PATH", "phase-owner-bridge-no-publication/raw.log" },
		{ "RTS_PERFORMANCE_TIMING_PATH", "phase-owner-bridge-no-publication/timing.csv" },
		{ "RTS_PERFORMANCE_VERIFIER_BOUNDARY", "test-only-no-publication" },
		{ "RTS_PERFORMANCE_REFERENCE_MODE", "throughput-binding" },
		{ "RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "observed-only" },
		{ "RTS_PERFORMANCE_FIXTURE_KIND", "fresh-ai-map" },
		{ "RTS_PERFORMANCE_FIXTURE_SHA256", 0 },
		{ "RTS_PERFORMANCE_PLAYER_COUNT", 0 },
		{ "RTS_PERFORMANCE_UNIT_COUNT", 0 },
		{ "RTS_PERFORMANCE_SEED", 0 }
	};
	for (unsigned index = 0; index != sizeof(values) / sizeof(values[0]); ++index)
		environment.set(values[index][0], values[index][1]);
	TestOrdinaryRuntimeKeepsPipelineConfiguration();
	TestNormalAndRetargetedCompletion();
	TestNullAndInactiveAttachmentDoNotObserve();
	TestReplacementAndInUpdateMutationPreserveBorrow();
	if (attachOwner) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Performance receipt owner bridge title tests passed.\n");
	return 0;
}
