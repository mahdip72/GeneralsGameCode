/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Source-connected fresh owner lifetime. Generated includes contain the real
// runner state/start/finalizers, title borrow methods, whole GameEngine
// destructor, and the actual GameMain cleanup tail. Only external lobby,
// subsystem and reporting collaborators are controlled. No map-load, victory,
// playable replay, receipt publication or baseline acceptance is claimed.
#include "Common/GameThreadOwnership.h"
#include "Common/INI.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "Common/SkirmishAIReplayEpoch.h"
#include "Common/SkirmishAITestRunner.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"
#include <windows.h>
#include <limits.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "NativeReceiptCallerFenceInputs.h"

// Actual shared plan helpers, not alternate scenario authorities.
Bool IsSkirmishAITest4v2(SkirmishAITestScenario scenario);
Bool IsSkirmishAITestHardAI2v6(SkirmishAITestScenario scenario);
Int ExpectedSkirmishAITestAiCount(SkirmishAITestScenario scenario);
const char *SkirmishAITestScenarioName(SkirmishAITestScenario scenario);

#ifdef TheGlobalData
#undef TheGlobalData
#endif
#ifdef TheWritableGlobalData
#undef TheWritableGlobalData
#endif
#undef DEBUG_LOG
#define DEBUG_LOG(message) ((void)0)
#undef NEW
#define NEW new

namespace performance_receipt_fresh_producer_fixture
{
unsigned failures = 0;
const char *caseName = "prerequisite";
void Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL [%s]: %s\n", caseName, message);
		++failures;
	}
}
void UnexpectedFatal(const char *message)
{
	fprintf(stderr, "Unexpected fresh producer fatal: %s\n", message);
	exit(2);
}
#undef RELEASE_CRASH
#define RELEASE_CRASH(message) UnexpectedFatal(message)

class PerformanceReceiptRuntime;
class GameLogic;
class GameEngine;
GameLogic *TheGameLogic = 0;
GameEngine *TheGameEngine = 0;
PerformanceReceiptRuntime *currentRuntime = 0;
struct Outcome
{
	Outcome() : instances(0), destroyed(0), finishCalls(0), messages(0),
		ownerDestroyed(false), expectReceipt(true), completionCalls(0),
		completionDestroyed(0), engineExecuteCalls(0), stage5StartCalls(0),
		engineQuitting(false), randomInitCalls(0) {}
	unsigned instances, destroyed, finishCalls, messages;
	bool ownerDestroyed, expectReceipt;
	unsigned completionCalls, completionDestroyed;
	unsigned engineExecuteCalls, stage5StartCalls;
	bool engineQuitting;
	unsigned randomInitCalls;
} outcome;

class AsciiString
{
public:
	AsciiString() {}
	AsciiString(const char *text) : value(text != 0 ? text : "") {}
	const char *str() const { return value.c_str(); }
	bool empty() const { return value.empty(); }
	void concat(const AsciiString &other) { value += other.value; }
private:
	std::string value;
};
class UnicodeString { public: void set(const wchar_t *) {} };
struct GlobalData
{
	GlobalData() : m_headless(TRUE), m_shellMapOn(FALSE), m_useFpsLimit(FALSE),
		m_clientRetaliationModeEnabled(FALSE), m_simulateReplayJobs(0) {}
	AsciiString m_simulateReplays, m_mapName;
	Bool m_headless, m_shellMapOn, m_useFpsLimit, m_clientRetaliationModeEnabled;
	Int m_simulateReplayJobs;
} globalData;
GlobalData *TheGlobalData = &globalData;
GlobalData *TheWritableGlobalData = &globalData;

class GameLogic
{
public:
	GameLogic() : m_performanceReceiptRuntime(0), m_isInUpdate(FALSE) {}
	~GameLogic()
	{
		Check(m_performanceReceiptRuntime == 0,
			"actual pre-destruction route released the expected runtime borrow");
		outcome.ownerDestroyed = true;
	}
	static bool isStage5PhaseGraphOwner(void *ownerContext);
	bool attachPerformanceReceiptRuntime(PerformanceReceiptRuntime *runtime);
	bool detachPerformanceReceiptRuntime(PerformanceReceiptRuntime *runtime);
	static void observeStage5PhaseGraphBoundary(
		rts::LiveSimulationPhaseObservationBoundary boundary,
		rts::SimulationPhaseId phaseId, unsigned generation,
		unsigned frame, void *ownerContext) noexcept;
	Bool isInGameLogicUpdate() const { return m_isInUpdate; }
	unsigned getFrame() const { return 0; }
	const rts::LiveSimulationPhaseRuntimeMetrics &getStage5PhaseRuntimeMetrics() const
	{
		return phaseMetrics;
	}
	PerformanceReceiptRuntime *m_performanceReceiptRuntime;
	Bool m_isInUpdate;
private:
	rts::LiveSimulationPhaseRuntimeMetrics phaseMetrics;
};

class PerformanceReceiptRuntime : public ::PerformanceReceiptRuntime
{
public:
	PerformanceReceiptRuntime() : m_begun(false)
	{
		++outcome.instances;
		Check(currentRuntime == 0, "fresh start owns exactly one actual runtime");
		currentRuntime = this;
	}
	~PerformanceReceiptRuntime()
	{
		++outcome.destroyed;
		if (m_begun)
			Check(outcome.ownerDestroyed && TheGameLogic == 0 && TheGameEngine == 0,
				"begun runtime storage outlives real engine and owner destruction");
		currentRuntime = 0;
	}
	bool begin(const char *kind, const char *path)
	{
		m_begun = ::PerformanceReceiptRuntime::begin(kind, path);
		return m_begun;
	}
	void finish(int exitCode, const char *boundary)
	{
		++outcome.finishCalls;
		rts::JobSystem &jobs = rts::JobSystem::instance();
		Check(TheGameEngine == 0 && TheGameLogic == 0 && outcome.ownerDestroyed,
			"actual GameMain calls final publication only after engine destruction");
		Check(!jobs.isRunning() && jobs.outstandingJobCount() == 0 &&
			jobs.pendingOwnerCompletionCount() == 0,
			"runtime finalization observes the actual scheduler drain");
		::PerformanceReceiptRuntime::finish(exitCode, boundary);
	}
private:
	bool m_begun;
};

#include "FreshProducerOwnerMethods.inc"
#include "FreshProducerRunnerState.inc"
#include "FreshProducerCaptureState.inc"

// The extracted runner start/finalizer bodies retain the production reviewed
// map branch.  This lifetime fixture never arms a reviewed-map request, so
// keep its anonymous-namespace state and content gate local and fail closed if
// that unsupported shape is accidentally exercised.
#if defined(_WIN64)
rts::ai_fixture::MapRequest s_reviewedMapRequest;
rts::fixture::ResolvedMapIdentity s_reviewedMapIdentity;
char s_reviewedMapSha256[65] = {};

bool ResolveStage5MapIdentity(const char *, rts::fixture::ResolvedMapIdentity *out)
{
	if (out != 0) *out = rts::fixture::ResolvedMapIdentity();
	return false;
}

static Bool VerifyReviewedSkirmishMapBytes()
{
	return FALSE;
}
#endif

struct MapMetaData
{
	MapMetaData() : m_doesExist(TRUE), m_isMultiplayer(TRUE), m_numPlayers(8),
		m_CRC(0x10203040U), m_filesize(7) {}
	Bool m_doesExist, m_isMultiplayer;
	Int m_numPlayers;
	unsigned m_CRC, m_filesize;
};
class MapCache
{
public:
	const MapMetaData *findMap(const char *) const { return &map; }
private:
	MapMetaData map;
};
MapCache *TheMapCache = 0;
class GameSlot
{
public:
	void setState(SlotState, const UnicodeString &, unsigned) {}
	void setPlayerTemplate(Int) {}
	void setColor(Int) {}
	void setStartPos(Int) {}
	void setTeamNumber(Int) {}
	void setAccept() {}
	void setMapAvailability(Bool) {}
};
class SkirmishGameInfo
{
public:
	void init() {}
	void clearSlotList() {}
	void reset() {}
	void setLocalIP(unsigned) {}
	void enterGame() {}
	GameSlot *getSlot(Int index) { return &slots[index]; }
	void setMap(const char *) {}
	void setMapCRC(unsigned) {}
	void setMapSize(unsigned) {}
	void setSeed(Int) {}
	void startGame(unsigned) {}
private:
	GameSlot slots[SKIRMISH_AI_TEST_SLOT_COUNT];
};
SkirmishGameInfo *TheSkirmishGameInfo = 0;
class GameMessage
{
public:
	enum { MSG_NEW_GAME = 1 };
	void appendIntegerArgument(Int) {}
};
class MessageStream
{
public:
	GameMessage *appendMessage(Int type)
	{
		++outcome.messages;
		Check(type == GameMessage::MSG_NEW_GAME,
			"real runner queues its unchanged NEW_GAME message");
		Check(TheGameLogic != 0 &&
			(outcome.expectReceipt ?
				(currentRuntime != 0 && currentRuntime->active() &&
				 TheGameLogic->m_performanceReceiptRuntime == currentRuntime) :
				 TheGameLogic->m_performanceReceiptRuntime == 0),
			"real fresh producer borrows its begun runtime before NEW_GAME ingress");
		return &message;
	}
private:
	GameMessage message;
} messageStream;
MessageStream *TheMessageStream = &messageStream;
enum { RECORDERMODETYPE_RECORD = 1 };
class RecorderClass
{
public:
	struct ReplayHeader
	{
		AsciiString filename;
		Bool forPlayback, desyncGame, quitEarly;
		unsigned frameCount;
		time_t startTime, endTime;
		const wchar_t *versionTimeString;
	};
	static AsciiString getReplayDir() { return AsciiString("."); }
	void setArchiveEnabled(Bool) {}
	Int getMode() const { return 0; }
	void stopRecording() {}
	Bool readReplayHeader(ReplayHeader &)
	{
		Check(false, "incomplete-lifetime fixture cannot claim a closed replay");
		return FALSE;
	}
	static Bool replayMatchesGameVersion(const ReplayHeader &) { return FALSE; }
} recorder;
RecorderClass *TheRecorder = &recorder;
void InitRandom(unsigned) { ++outcome.randomInitCalls; } // External RNG setup; no simulation runs here.
#include "FreshProducerFailure.inc"
Bool IsSkirmishAITestRunnerArmed() { return s_runner.armed; }
void PrintSkirmishAITestManifest() {} // Reporting is outside this lifetime test.

#include "FreshProducerStart.inc"
#include "FreshProducerFinalizers.inc"

Bool s_headlessSimulationJobSystemStartAttempted = FALSE;
Bool s_headlessSimulationJobSystemStarted = FALSE;
struct EmptyOwned {};
EmptyOwned *TheLAN = 0, *TheNAT = 0, *TheNetwork = 0;
EmptyOwned *TheCommandList = 0, *TheNameKeyGenerator = 0, *TheFileSystem = 0;
EmptyOwned *TheGameLODManager = 0;
struct ResultsQueue { void endThreads() {} } resultsQueue;
ResultsQueue *TheGameResultsQueue = &resultsQueue;
struct Subsystems
{
	void shutdownAll()
	{
		Check(!rts::JobSystem::instance().isRunning() &&
			rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
			"real GameEngine drain precedes subsystem and GameLogic destruction");
		delete TheGameLogic;
		TheGameLogic = 0;
	}
};
Subsystems *TheSubsystemList = 0;
struct Drawable { static void killStaticImages() {} };
struct Module { void Term() {} } _Module;
void printHeadlessSimulationJobMetrics(const rts::JobSystemMetrics &,
	const rts::PhysicsIntegrationRuntimeMetrics &,
	const rts::ObjectStatusTimerRuntimeMetrics &) {}
class GameEngine
{
public:
	~GameEngine();
	void reset() {}
	void execute() { ++outcome.engineExecuteCalls; }
	void setQuitting(Bool value) { outcome.engineQuitting = value != FALSE; }
};
#include "FreshProducerEngineDestructor.inc"
struct FramePacer {};
FramePacer *TheFramePacer = 0;
Bool StartStage5PerformanceFixtureRunner()
{
	++outcome.stage5StartCalls;
	return TRUE;
}
class ReplaySimulation
{
public:
	static Int simulateReplays(const AsciiString &, Int)
	{
		Check(false, "fresh caller must not dispatch a replay");
		return 1;
	}
};
Int FinalizeStage5PerformanceFixtureRunner(Int code) { return code; }
Int CleanupThroughActualGameMain(Int exitcode)
{
	const Bool performanceFixtureRequested = FALSE;
#include "FreshProducerGameMainCleanup.inc"
	return exitcode;
}

class EmptyJob : public rts::Job
{
public:
	void execute(rts::JobContext &) {}
};
class RetainedOwnerCompletion : public rts::OwnerCompletion
{
public:
	~RetainedOwnerCompletion() { ++outcome.completionDestroyed; }
	void complete(bool, bool)
	{
		++outcome.completionCalls;
		Check(TheGameLogic != 0 && currentRuntime != 0 && !outcome.ownerDestroyed,
			"real drain keeps owner state and the actual runtime alive for retained work");
	}
};

class ReceiptEnvironment
{
public:
	void set(const char *name, const char *value)
	{
		Saved saved;
		saved.name = name;
		const DWORD size = GetEnvironmentVariableA(name, 0, 0);
		saved.present = size != 0;
		if (saved.present)
		{
			std::vector<char> buffer(size);
			GetEnvironmentVariableA(name, &buffer[0], size);
			saved.value = &buffer[0];
		}
		values.push_back(saved);
		Check(SetEnvironmentVariableA(name, value) != FALSE, "fixture sets process-local receipt input");
	}
	~ReceiptEnvironment()
	{
		for (size_t i = values.size(); i != 0; --i)
			SetEnvironmentVariableA(values[i - 1].name.c_str(),
				values[i - 1].present ? values[i - 1].value.c_str() : 0);
	}
private:
	struct Saved { std::string name, value; bool present; };
	std::vector<Saved> values;
};

void RunLifetimeCase(unsigned scenario)
{
	const char *names[] = { "fresh-never-started", "fresh-disarmed-before-cleanup",
		"fresh-retained-owner-drain", "fresh-unsupported-receipt",
		"fresh-explicit-preflight" };
	caseName = names[scenario];
	outcome = Outcome();
	if (TheSkirmishGameInfo != 0)
	{
		delete TheSkirmishGameInfo;
		TheSkirmishGameInfo = 0;
	}
	outcome.expectReceipt = scenario != 3 && scenario != 4;
	// Give the rejection case observable pre-existing game/global state. The
	// common intent preflight must leave every field and collaborator untouched.
	globalData.m_headless = FALSE;
	globalData.m_shellMapOn = TRUE;
	globalData.m_useFpsLimit = TRUE;
	globalData.m_clientRetaliationModeEnabled = TRUE;
	globalData.m_simulateReplayJobs = 7;
	globalData.m_mapName = AsciiString("before-explicit-preflight");
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
	ReceiptEnvironment role;
	if (scenario == 3) role.set("RTS_PERFORMANCE_ROLE", 0);
	if (scenario == 4)
		role.set("RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", "fresh-explicit-preflight.trace");
	const Bool started = StartSkirmishAITestRunner();
	if (scenario == 4)
	{
		Check(started == FALSE && outcome.messages == 0,
			"fresh explicit intent is rejected before NEW_GAME ingress");
		Check(outcome.instances == 0 && outcome.randomInitCalls == 0,
			"fresh explicit rejection allocates no receipt and does not initialize RNG");
		Check(TheSkirmishGameInfo == 0,
			"fresh explicit rejection creates no new lobby/game-info state");
		Check(globalData.m_headless == FALSE && globalData.m_shellMapOn == TRUE &&
			globalData.m_useFpsLimit == TRUE &&
			globalData.m_clientRetaliationModeEnabled == TRUE &&
			globalData.m_simulateReplayJobs == 7 &&
			strcmp(globalData.m_mapName.str(), "before-explicit-preflight") == 0,
			"fresh explicit rejection preserves all pre-existing global state");
	}
	else
	{
		Check(started != FALSE && outcome.messages == 1,
			"actual fresh start preserves NEW_GAME ingress even when receipt is unsupported");
		Check(outcome.instances == 1, "fresh start attempts only one actual runtime");
	}

	rts::JobGroup retainedGroup;
	rts::JobHandle retainedJob;
	if (scenario == 2)
	{
		rts::JobSystem &jobs = rts::JobSystem::instance();
		rts::JobSystemConfig config;
		config.workerCount = 1;
		config.queueCapacity = 8;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		Check(jobs.start(config) && jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
			"bounded drain fixture starts and owns one real scheduler worker");
		s_headlessSimulationJobSystemStartAttempted = TRUE;
		s_headlessSimulationJobSystemStarted = jobs.isRunning() ? TRUE : FALSE;
		retainedGroup = jobs.createGroup();
		std::unique_ptr<EmptyJob> job(new EmptyJob);
		std::unique_ptr<RetainedOwnerCompletion> completion(new RetainedOwnerCompletion);
		retainedJob = jobs.trySubmit(job.get(), rts::JOB_PRIORITY_NORMAL,
			retainedGroup, completion.get());
		if (retainedJob.isValid())
		{
			job.release();
			completion.release();
		}
		Check(retainedJob.isValid(), "real scheduler owns the submitted work and completion");
	}
	// Intentionally incomplete: no fake victory, replay receipt or terminal frame.
	s_runner.failed = TRUE;
	s_runner.failureReason = "fixture_stopped_before_first_update";
	if (scenario == 1) s_runner.armed = FALSE;
	const Int code = CleanupThroughActualGameMain(0);
	Check(code == (scenario == 1 ? 0 : 1),
		"actual finalizer preserves incomplete and disarmed exit classification");
	Check(outcome.destroyed == (scenario == 4 ? 0 : 1) &&
		outcome.finishCalls == (scenario == 3 || scenario == 4 ? 0 : 1),
		"actual cleanup finalizes each begun runtime once and releases its storage");
	if (scenario == 2)
		Check(outcome.completionCalls == 1 && outcome.completionDestroyed == 1 &&
			retainedGroup.isComplete() && retainedJob.isComplete(),
			"actual destructor drains and releases retained owner work exactly once");
	Check(!GameThreadOwnership::IsAttached(),
		"actual GameMain releases owner identity only after receipt finalization");
}
} // namespace performance_receipt_fresh_producer_fixture

int RunPerformanceReceiptFreshProducerTitleTests()
{
	using namespace performance_receipt_fresh_producer_fixture;
	failures = 0;
	Check(!GameThreadOwnership::IsAttached() && !rts::JobSystem::instance().isRunning(),
		"fresh lifetime utility begins without unrelated owner or scheduler work");
	if (failures != 0) return 2;
	ReceiptEnvironment environment;
	const char *inputs[][2] = {
		{ "RTS_PERFORMANCE_ROLE", "performance-report" },
		{ "RTS_PERFORMANCE_RUN_ID", "fresh-owner-lifetime-no-publication" },
		{ "RTS_PERFORMANCE_RUN_NONCE", "11111111-1111-4111-8111-111111111111" },
		{ "RTS_PERFORMANCE_COHORT_NONCE", "22222222-2222-4222-8222-222222222222" },
		{ "RTS_PERFORMANCE_COHORT_CREATED_UTC", "2026-01-01T00:00:00Z" },
		{ "RTS_PERFORMANCE_RECEIPT_DIR", "fresh-lifetime-no-publication" },
		{ "RTS_PERFORMANCE_SOURCE_COMMIT", "0123456789abcdef0123456789abcdef01234567" },
		{ "RTS_PERFORMANCE_ARTIFACT_SET_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		{ "RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
		{ "RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
		{ "RTS_PERFORMANCE_FIXTURE_ID", "fresh-owner-lifetime" },
		{ "RTS_PERFORMANCE_RAW_LOG_PATH", "fresh-lifetime-no-publication/raw.log" },
		{ "RTS_PERFORMANCE_TIMING_PATH", "fresh-lifetime-no-publication/timing.csv" },
		{ "RTS_PERFORMANCE_VERIFIER_BOUNDARY", "test-only-no-publication" },
		{ "RTS_PERFORMANCE_REFERENCE_MODE", "throughput-binding" },
		{ "RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "observed-only" },
		{ "RTS_PERFORMANCE_FIXTURE_KIND", "fresh-ai-map" },
		{ "RTS_PERFORMANCE_FIXTURE_SHA256", 0 },
		{ "RTS_PERFORMANCE_PLAYER_COUNT", 0 },
		{ "RTS_PERFORMANCE_UNIT_COUNT", 0 },
		{ "RTS_PERFORMANCE_SEED", 0 },
		{ "RTS_FRAME_TIMING_DIR", 0 }
	};
	for (unsigned i = 0; i != ARRAY_SIZE(inputs); ++i)
		environment.set(inputs[i][0], inputs[i][1]);
	for (unsigned scenario = 0; scenario != 5; ++scenario) RunLifetimeCase(scenario);
	if (failures != 0) return 1;
	printf("Fresh receipt producer lifetime title tests passed.\n");
	return 0;
}

#include "PerformanceReceiptFreshCallerFence.inc"
#include "PerformanceReceiptPracticalOwnerShape.inc"
