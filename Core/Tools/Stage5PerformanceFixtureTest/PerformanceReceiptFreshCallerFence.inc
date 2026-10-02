// Source-connected caller-fence tests. Include after the existing fresh fixture namespace is closed.
// Uses the fixture's dispatch observations and exact GameMain start include.
// Receipt begin and caller gates remain real native code.
namespace performance_receipt_fresh_producer_fixture
{
Int StartThroughActualGameMain()
{
	const Bool validationOptionsConflict = FALSE;
	const Bool net3ValidationRequested = FALSE;
	const Bool lockstepV2ValidationRequested = FALSE;
	Int exitcode = 0;
#include "FreshProducerGameMainStart.inc"
	return exitcode;
}

void RunFreshCallerFenceCase(unsigned scenario)
{
	const native_receipt_caller_fence::Case &test = native_receipt_caller_fence::cases[scenario];
	caseName = test.name;
	native_receipt_caller_fence::Inputs input(scenario, L"fresh-ai-map");
	rts::performance::PerformanceReceipt parsed;
	const bool didParse = rts::performance::BeginPerformanceReceipt(parsed,
		native_receipt_caller_fence::Title(), "", 0);
	native_receipt_caller_fence::Require(didParse == test.beginParses,
		"fresh case reaches the intended actual Begin branch");
	outcome = Outcome();
	// Only the pre-existing MessageStream collaborator's borrow check uses this.
	// Current mode prerequisite rejects baseline; other valid Begin inputs still
	// borrow today. A correct fence queues no message for any explicit case.
	outcome.expectReceipt = test.beginParses && !test.baseline;
	TheGameLogic = new GameLogic;
	TheGameEngine = new GameEngine;
	TheMapCache = new MapCache;
	TheSubsystemList = new Subsystems;
	TheFramePacer = new FramePacer;
	s_runner = SkirmishAITestRunnerState();
	s_runner.armed = TRUE;
	s_runner.seed = 17;
	s_runner.scenario = SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6;
	s_performanceReceiptAttempted = false;
	s_headlessSimulationJobSystemStartAttempted = FALSE;
	s_headlessSimulationJobSystemStarted = FALSE;
	GameThreadOwnership::AttachCurrentThread();
	const Int startCode = StartThroughActualGameMain();
	const std::string failureBeforeCleanup = s_runner.failureReason != 0 ? s_runner.failureReason : "";
	if (test.explicitTrace)
	{
		Check(s_runner.failed && !s_runner.started && !failureBeforeCleanup.empty(),
			"fresh explicit trace request fails in the real start path before marking gameplay started");
		Check(outcome.messages == 0, "unsupported fresh trace cannot queue NEW_GAME");
		Check(outcome.engineExecuteCalls == 0 && outcome.engineQuitting && outcome.stage5StartCalls == 0,
			"actual paired GameMain short-circuits downstream start and execute after fresh rejection");
		Check(TheGameLogic->m_performanceReceiptRuntime == 0 && s_performanceReceiptOwner == 0,
			"rejected fresh trace leaves no gameplay runtime borrow");
	}
	else
	{
		Check(!s_runner.failed && s_runner.started && outcome.messages == 1,
			"ordinary fresh inputs retain their unchanged real NEW_GAME ingress");
		Check(outcome.engineExecuteCalls == 1 && !outcome.engineQuitting && outcome.stage5StartCalls == 1,
			"ordinary fresh request still reaches the actual GameMain execute branch");
	}
	// No test-authored success/failure, victory, or terminal value is installed.
	// The real finalizer diagnoses incomplete ordinary controls on its own.
	const Int code = CleanupThroughActualGameMain(startCode);
	Check(code != 0, "actual cleanup returns failure for rejected or deliberately incomplete fresh runs");
	if (test.explicitTrace && !failureBeforeCleanup.empty())
		Check(s_runner.failureReason != 0 && failureBeforeCleanup == s_runner.failureReason,
			"real cleanup preserves the original explicit admission failure rather than replacing it");
	const unsigned expectedRuntimeCount = test.explicitTrace ? 0 : 1;
	Check(outcome.instances == expectedRuntimeCount && outcome.destroyed == expectedRuntimeCount &&
		currentRuntime == 0 && s_performanceReceiptOwner == 0 && !s_performanceReceipt,
		"actual cleanup retains no runtime, including fail-before-allocation explicit admission");
	Check(TheGameEngine == 0 && TheGameLogic == 0 && !GameThreadOwnership::IsAttached() &&
		!rts::JobSystem::instance().isRunning() && rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"actual paired destructor and cleanup preserve owner-before-runtime teardown without workers");
	Check(input.bundleAbsent(), "fresh rejection and incomplete ordinary controls cannot publish evidence");
}
} // namespace performance_receipt_fresh_producer_fixture

int RunPerformanceReceiptFreshCallerFenceTitleTests()
{
	using namespace performance_receipt_fresh_producer_fixture;
	failures = 0;
	caseName = "fresh-caller-prerequisite";
	native_receipt_caller_fence::Require(!GameThreadOwnership::IsAttached() &&
		!rts::JobSystem::instance().isRunning() && rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"fresh caller fixture begins without unrelated owner or scheduler work");
	native_receipt_caller_fence::RequireBaselineParser();
	for (unsigned scenario = 0; scenario != sizeof(native_receipt_caller_fence::cases) /
		sizeof(native_receipt_caller_fence::cases[0]); ++scenario)
		RunFreshCallerFenceCase(scenario);
	if (failures != 0) return 1;
	printf("Fresh explicit-intent caller-fence title tests passed.\n");
	return 0;
}
