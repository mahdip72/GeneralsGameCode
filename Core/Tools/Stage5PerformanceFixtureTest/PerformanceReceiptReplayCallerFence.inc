// Source-connected caller-fence tests. Include after the existing replay fixture namespace is closed.
// Reuses its actual generated loop/owner scope and linked Runtime; no fake Begin.
namespace performance_receipt_producer_fixture
{
void RunReplayCallerFenceCase(unsigned scenario, const char *sourcePath)
{
	const native_receipt_caller_fence::Case &test = native_receipt_caller_fence::cases[scenario];
	caseName = test.name;
	native_receipt_caller_fence::Inputs input(scenario, L"replay");
	rts::performance::PerformanceReceipt parsed;
	const bool didParse = rts::performance::BeginPerformanceReceipt(parsed,
		native_receipt_caller_fence::Title(), sourcePath, 0);
	native_receipt_caller_fence::Require(didParse == test.beginParses,
		"case reaches its intended real Begin branch, independently of caller behavior");
	outcome = Outcome();
	GameLogic logic(0, false);
	RecorderClass recorder(false, 2);
	TheGameLogic = &logic;
	TheRecorder = &recorder;
	std::vector<AsciiString> filenames(1, AsciiString(sourcePath));
	const int result = ReplaySimulation::simulateReplaysInThisProcess(filenames);
	if (test.explicitTrace)
	{
		Check(result != 0, "explicit trace admission failure becomes a failing real replay result");
		Check(outcome.recorderCalls == 0, "rejected explicit intent cannot reach Recorder entry");
		Check(outcome.updateCalls == 0 && outcome.endFrames == 0 && outcome.collectorCalls == 0,
			"rejected explicit intent cannot execute an ordinary owner UPDATE or completion collector");
		Check(outcome.workload.sampleCount == 0 && !outcome.terminalKnown,
			"rejected explicit intent cannot manufacture completed workload or terminal evidence");
	}
	else
	{
		Check(result == 0 && outcome.recorderCalls == 1 && outcome.updateCalls == 2,
			"no-trace ordinary replay retains existing behavior even when receipt environment is missing");
		if (test.beginParses)
			Check(outcome.workload.sampleCount == 2 && outcome.terminalKnown,
				"valid untraced receipt retains its two real completed-world observations");
		else
			Check(outcome.workload.sampleCount == 0 && !outcome.terminalKnown,
				"ordinary receipt-disabled control does not invent a receipt for its real replay work");
	}
	Check(outcome.runtimeInstances == 1 && outcome.runtimeDestroyed == 1 &&
		logic.m_performanceReceiptRuntime == 0,
		"real replay releases exactly its own runtime without leaving a borrowed pointer");
	Check(!rts::JobSystem::instance().isRunning() &&
		rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"bounded serial caller fixture never starts workers or leaves scheduler work");
	Check(input.bundleAbsent(), "caller admission failure and ordinary controls cannot publish a trace bundle");
	// Failure-only fixture cleanup, after the actual release assertion above.
	logic.m_performanceReceiptRuntime = 0;
	TheGameLogic = 0;
	TheRecorder = 0;
}
} // namespace performance_receipt_producer_fixture

int RunPerformanceReceiptReplayCallerFenceTitleTests()
{
	using namespace performance_receipt_producer_fixture;
	failures = 0;
	caseName = "caller-prerequisite";
	native_receipt_caller_fence::Require(!rts::JobSystem::instance().isRunning() &&
		rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"caller fixture cannot overlap scheduler work");
	native_receipt_caller_fence::Require(
		rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL) &&
		rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL),
		"caller fixture selects ordinary serial modes before any policy lock");
	const bool attach = !GameThreadOwnership::IsAttached();
	if (attach) GameThreadOwnership::AttachCurrentThread();
	native_receipt_caller_fence::Require(GameThreadOwnership::IsCurrentThread(), "real game owner is current");
	native_receipt_caller_fence::RequireBaselineParser();
	{
		SourceFile source;
		native_receipt_caller_fence::Require(source.create(), "existing fixture creates one exclusive opaque replay source");
		for (unsigned scenario = 0; scenario != sizeof(native_receipt_caller_fence::cases) /
			sizeof(native_receipt_caller_fence::cases[0]); ++scenario)
			if (!native_receipt_caller_fence::cases[scenario].freshOnly)
				RunReplayCallerFenceCase(scenario, source.path.c_str());
	}
	if (attach) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Replay explicit-intent caller-fence title tests passed.\n");
	return 0;
}
