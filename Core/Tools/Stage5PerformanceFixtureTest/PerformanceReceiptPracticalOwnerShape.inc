// Source-connected practical-controller shape rejection. Include after the
// existing fresh caller include; reuse its actual GameMain start/cleanup.
namespace performance_receipt_fresh_producer_fixture
{
void RunPracticalOwnerShapeCase(unsigned scenario)
{
	const native_receipt_caller_fence::Case &test = native_receipt_caller_fence::cases[scenario];
	const std::string label = std::string("practical-controller/") + test.name;
	caseName = label.c_str();
	native_receipt_caller_fence::Inputs input(scenario, L"fresh-ai-map");
	rts::performance::PerformanceReceipt parsed;
	const bool didParse = rts::performance::BeginPerformanceReceipt(parsed,
		native_receipt_caller_fence::Title(), "", 0);
	native_receipt_caller_fence::Require(didParse == test.beginParses,
		"practical input reaches its intended actual Begin branch");
	outcome = Outcome();
	outcome.expectReceipt = false;
	globalData = GlobalData();
	globalData.m_headless = FALSE;
	globalData.m_useFpsLimit = TRUE;
	globalData.m_clientRetaliationModeEnabled = TRUE;
	TheGameLogic = new GameLogic;
	TheGameEngine = new GameEngine;
	TheMapCache = new MapCache;
	TheSubsystemList = new Subsystems;
	TheFramePacer = new FramePacer;
	s_runner = SkirmishAITestRunnerState();
	s_runner.armed = TRUE;
	s_runner.seed = 17;
	s_runner.scenario = SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7;
	s_performanceReceiptAttempted = false;
	s_headlessSimulationJobSystemStartAttempted = FALSE;
	s_headlessSimulationJobSystemStarted = FALSE;
	native_receipt_caller_fence::Require(IsSkirmishAITestPracticalControllerScenario(s_runner.scenario),
		"linked actual practical-controller classifier selects this shape");
	GameThreadOwnership::AttachCurrentThread();
	const Int startCode = StartThroughActualGameMain();
	const std::string failureBeforeCleanup = s_runner.failureReason != 0 ? s_runner.failureReason : "";
	if (test.explicitTrace)
	{
		Check(s_runner.failed && !s_runner.started && !failureBeforeCleanup.empty(),
			"explicit trace fails through the real practical start path");
		Check(outcome.messages == 0, "unsupported practical trace cannot queue NEW_GAME");
		Check(outcome.engineExecuteCalls == 0 && outcome.engineQuitting && outcome.stage5StartCalls == 0,
			"actual paired GameMain blocks downstream start and interactive execute after practical rejection");
	}
	else
	{
		Check(!s_runner.failed && s_runner.started && outcome.messages == 1,
			"ordinary practical-controller plan retains its real NEW_GAME ingress");
		Check(outcome.engineExecuteCalls == 1 && !outcome.engineQuitting && outcome.stage5StartCalls == 1,
			"ordinary practical-controller run keeps its original GameMain execute branch");
	}
	Check(!globalData.m_headless && globalData.m_useFpsLimit && globalData.m_clientRetaliationModeEnabled,
		"shape-only fence preserves actual interactive, FPS-limit, and retaliation settings");
	Check(outcome.instances == 0 && currentRuntime == 0 && !s_performanceReceipt &&
		s_performanceReceiptOwner == 0 && TheGameLogic->m_performanceReceiptRuntime == 0,
		"practical shape never allocates or borrows an automated receipt runtime");
	const Int code = CleanupThroughActualGameMain(startCode);
	Check(code != 0, "real cleanup rejects explicit or deliberately incomplete ordinary practical runs");
	if (test.explicitTrace && !failureBeforeCleanup.empty())
		Check(s_runner.failureReason != 0 && failureBeforeCleanup == s_runner.failureReason,
			"actual cleanup preserves the original practical admission failure");
	Check(outcome.destroyed == 0 && TheGameEngine == 0 && TheGameLogic == 0 &&
		!GameThreadOwnership::IsAttached() && !rts::JobSystem::instance().isRunning() &&
		rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"actual practical teardown leaves no runtime, owner, or scheduler work");
	Check(input.bundleAbsent(), "unsupported or incomplete practical runs cannot publish trace evidence");
	globalData = GlobalData();
}
} // namespace performance_receipt_fresh_producer_fixture

int RunPerformanceReceiptPracticalOwnerShapeTitleTests()
{
	using namespace performance_receipt_fresh_producer_fixture;
	failures = 0;
	caseName = "practical-shape-prerequisite";
	native_receipt_caller_fence::Require(!GameThreadOwnership::IsAttached() &&
		!rts::JobSystem::instance().isRunning() && rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"practical shape fixture begins without unrelated owner or scheduler work");
	native_receipt_caller_fence::RequireBaselineParser();
	const unsigned inputs[] = { 0, 4, 6, 11, 12 };
	for (unsigned index = 0; index != ARRAY_SIZE(inputs); ++index)
		RunPracticalOwnerShapeCase(inputs[index]);
	if (failures != 0) return 1;
	printf("Practical-controller explicit-intent shape title tests passed.\n");
	return 0;
}
