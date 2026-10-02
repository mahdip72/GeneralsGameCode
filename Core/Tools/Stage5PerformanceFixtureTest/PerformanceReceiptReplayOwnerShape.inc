// Source-connected unsupported-owner shape tests. Include after the existing
// producer/caller fixture; all ReplaySimulation method bodies are generated.
namespace performance_receipt_producer_fixture
{
enum ReplayOwnerShape { MULTI_HEADLESS, INTERACTIVE_SINGLE, WORKER_SINGLE, EMPTY_HEADLESS };

void RunReplayOwnerShapeCase(ReplayOwnerShape shape, unsigned scenario, const char *sourcePath)
{
	const native_receipt_caller_fence::Case &test = native_receipt_caller_fence::cases[scenario];
	const char *shapeName = shape == MULTI_HEADLESS ? "multi-headless" :
		shape == INTERACTIVE_SINGLE ? "interactive-single" :
		shape == WORKER_SINGLE ? "worker-single" : "empty-headless";
	const std::string label = std::string(shapeName) + "/" + test.name;
	caseName = label.c_str();
	native_receipt_caller_fence::Inputs input(scenario, L"replay");
	rts::performance::PerformanceReceipt parsed;
	const bool didParse = rts::performance::BeginPerformanceReceipt(parsed,
		native_receipt_caller_fence::Title(), sourcePath, 0);
	native_receipt_caller_fence::Require(didParse == test.beginParses,
		"shape input reaches its independently checked real Begin branch");
	outcome = Outcome();
	outcome.allowInteractive = shape == INTERACTIVE_SINGLE;
	globalData.m_headless = shape == INTERACTIVE_SINGLE ? FALSE : TRUE;
	globalData.m_windowed = FALSE;
	GameLogic logic(0, false);
	RecorderClass recorder(false, 2);
	TheGameLogic = &logic;
	TheRecorder = &recorder;
	const unsigned filenameCount = shape == MULTI_HEADLESS ? 2 : shape == EMPTY_HEADLESS ? 0 : 1;
	std::vector<AsciiString> filenames(filenameCount, AsciiString(sourcePath));
	const int processCount = shape == WORKER_SINGLE ? 2 : SIMULATE_REPLAYS_SEQUENTIAL;
	const int result = ReplaySimulation::simulateReplays(filenames, processCount);
	if (test.explicitTrace)
	{
		Check(result != 0, "unsupported explicit owner shape returns failure through the actual public route");
		Check(outcome.recorderCalls == 0 && outcome.playbackCalls == 0 &&
			outcome.engineExecuteCalls == 0 && outcome.processStartCalls == 0,
			"unsupported explicit shape cannot enter Recorder, interactive engine, or child-process launch boundary");
		Check(outcome.updateCalls == 0 && outcome.endFrames == 0 && outcome.collectorCalls == 0,
			"unsupported explicit shape performs no owner update or completion observation");
		Check(outcome.runtimeInstances == 0 && outcome.runtimeDestroyed == 0 &&
			logic.m_performanceReceiptRuntime == 0 && outcome.workload.sampleCount == 0 && !outcome.terminalKnown,
			"shape rejection precedes receipt allocation, borrowing, and any aggregate evidence");
	}
	else
	{
		Check(result == 0, "ordinary unsupported-for-trace shape keeps its existing public replay result");
		if (shape == MULTI_HEADLESS)
		{
			Check(outcome.recorderCalls == 2 && outcome.updateCalls == 4 && outcome.endFrames == 4 &&
				outcome.playbackCalls == 0 && outcome.engineExecuteCalls == 0 && outcome.processStartCalls == 0,
				"ordinary two-replay input executes both real headless loops");
			Check(outcome.runtimeInstances == 1 && outcome.runtimeDestroyed == 1 &&
				outcome.workload.sampleCount == 0 && !outcome.terminalKnown,
				"ordinary multi replay retains its inactive stack receipt without inventing an aggregate");
		}
		else if (shape == INTERACTIVE_SINGLE)
		{
			Check(outcome.playbackCalls == 1 && outcome.engineExecuteCalls == 1 &&
				outcome.recorderCalls == 0 && outcome.updateCalls == 0 && outcome.processStartCalls == 0,
				"ordinary interactive replay reaches its real playback and engine-execute branches");
			Check(outcome.runtimeInstances == 0 && !outcome.engineQuitting,
				"ordinary interactive replay allocates no receipt and preserves its quit-reset behavior");
		}
		else if (shape == WORKER_SINGLE)
		{
			Check(outcome.processStartCalls == 1 && outcome.recorderCalls == 0 &&
				outcome.playbackCalls == 0 && outcome.engineExecuteCalls == 0 && outcome.updateCalls == 0,
				"ordinary worker dispatch reaches exactly one external process boundary, never a local replay");
			Check(outcome.runtimeInstances == 0 &&
				outcome.workerCommand.find(L" -headless") != std::wstring::npos &&
				outcome.workerCommand.find(L" -replay \"") != std::wstring::npos,
				"actual worker command retains headless replay arguments without a parent receipt");
		}
		else
		{
			Check(outcome.recorderCalls == 0 && outcome.playbackCalls == 0 && outcome.engineExecuteCalls == 0 &&
				outcome.processStartCalls == 0 && outcome.updateCalls == 0,
				"ordinary empty input remains an empty replay operation");
			Check(outcome.runtimeInstances == 1 && outcome.runtimeDestroyed == 1 &&
				outcome.workload.sampleCount == 0 && !outcome.terminalKnown,
				"ordinary empty input creates no synthetic receipt evidence");
		}
	}
	Check(!ReplaySimulation::s_isRunning && ReplaySimulation::s_replayIndex == 0 &&
		ReplaySimulation::s_replayCount == 0 && currentRuntime == 0 && logic.m_performanceReceiptRuntime == 0,
		"actual public route leaves replay flags and runtime borrowing closed");
	Check(!rts::JobSystem::instance().isRunning() && rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0 && input.bundleAbsent(),
		"shape fixture leaves no scheduler work or trace publication");
	logic.m_performanceReceiptRuntime = 0; // Failure-only cleanup, after assertions.
	TheGameLogic = 0;
	TheRecorder = 0;
	globalData.m_headless = TRUE;
	globalData.m_windowed = FALSE;
}

void RunReplayWildcardShapeCase(const char *sourcePath)
{
	caseName = "wildcard-headless-explicit-shape";
	native_receipt_caller_fence::Inputs input(0, L"replay");
	rts::performance::PerformanceReceipt parsed;
	native_receipt_caller_fence::Require(
		rts::performance::BeginPerformanceReceipt(parsed,
			native_receipt_caller_fence::Title(), sourcePath, 0),
		"wildcard shape test reaches the checked explicit baseline parser");
	outcome = Outcome();
	outcome.allowInteractive = false;
	globalData.m_headless = TRUE;
	globalData.m_windowed = FALSE;
	GameLogic logic(0, false);
	RecorderClass recorder(false, 2);
	TheGameLogic = &logic;
	TheRecorder = &recorder;
	localFileSystem.wildcardEnumerationCalls = 0;
	std::vector<AsciiString> filenames(1, AsciiString("*.rep"));
	const int result = ReplaySimulation::simulateReplays(filenames,
		SIMULATE_REPLAYS_SEQUENTIAL);
	Check(result != 0,
		"explicit native trace rejects wildcard replay input at the public shape boundary");
	Check(localFileSystem.wildcardEnumerationCalls == 0,
		"explicit wildcard rejection precedes filesystem enumeration and expansion");
	Check(outcome.recorderCalls == 0 && outcome.playbackCalls == 0 &&
		outcome.processStartCalls == 0 && outcome.runtimeInstances == 0,
		"wildcard shape rejection performs no replay, child launch, or receipt setup");
	Check(!ReplaySimulation::s_isRunning && ReplaySimulation::s_replayIndex == 0 &&
		ReplaySimulation::s_replayCount == 0 && currentRuntime == 0 &&
		logic.m_performanceReceiptRuntime == 0 && input.bundleAbsent(),
		"wildcard shape rejection leaves replay and publication state untouched");
	logic.m_performanceReceiptRuntime = 0;
	TheGameLogic = 0;
	TheRecorder = 0;
	globalData.m_headless = TRUE;
	globalData.m_windowed = FALSE;
}
} // namespace performance_receipt_producer_fixture

int RunPerformanceReceiptReplayOwnerShapeTitleTests()
{
	using namespace performance_receipt_producer_fixture;
	failures = 0;
	caseName = "owner-shape-prerequisite";
	native_receipt_caller_fence::Require(!rts::JobSystem::instance().isRunning() &&
		rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"shape fixture cannot overlap scheduler work");
	native_receipt_caller_fence::Require(rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL) &&
		rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL),
		"shape fixture selects actual serial policies without starting workers");
	const bool attached = !GameThreadOwnership::IsAttached();
	if (attached) GameThreadOwnership::AttachCurrentThread();
	native_receipt_caller_fence::Require(GameThreadOwnership::IsCurrentThread(), "actual owner is current");
	native_receipt_caller_fence::RequireBaselineParser();
	{
		SourceFile source;
		native_receipt_caller_fence::Require(source.create(), "creates one exclusive opaque test source");
		const unsigned inputs[] = { 0, 4, 6, 11, 12 };
		for (unsigned shape = 0; shape != 3; ++shape)
			for (unsigned index = 0; index != ARRAY_SIZE(inputs); ++index)
				RunReplayOwnerShapeCase(static_cast<ReplayOwnerShape>(shape), inputs[index], source.path.c_str());
		RunReplayOwnerShapeCase(EMPTY_HEADLESS, 0, source.path.c_str());
		RunReplayOwnerShapeCase(EMPTY_HEADLESS, 12, source.path.c_str());
		RunReplayWildcardShapeCase(source.path.c_str());
	}
	if (attached) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Unsupported replay owner-shape title tests passed; no native child process executed.\n");
	return 0;
}
