/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Source-connected replay producer contract, not a playable replay fixture.
// Generated includes contain the real loop, job scope, title owner methods,
// source-file hold, metric copying, and completed-frame collector body.
// The real Runtime begin/observe/terminal/scheduler/finish and ledgers are linked.
// Only external Recorder/world data and observation of call order are controlled.
#include "Common/GameThreadOwnership.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/SkirmishAITestRunner.h"
#include "GameLogic/AIPathfind.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/FrameTimingDiagnostics.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PerformanceReceipt.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/ReplayPathContract.h"
#include "Lib/SimulationExecutionPolicy.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <atomic>

// No production test API: dependency headers are already included above.
// Access changes only for the real Runtime fields in this fixture TU.
#define private protected
#include "Common/PerformanceReceiptRuntime.h"
#undef private

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef TheGlobalData
#undef TheGlobalData
#endif

namespace performance_receipt_producer_fixture
{
using namespace rts::performance;

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
	fprintf(stderr, "Unexpected producer fixture fatal: %s\n", message);
	exit(2);
}

#undef RELEASE_CRASH
#define RELEASE_CRASH(message) UnexpectedFatal(message)

class PerformanceReceiptRuntime;
class GameLogic;
GameLogic *TheGameLogic = 0;
PerformanceReceiptRuntime *currentRuntime = 0;

struct Outcome
{
	Outcome() : runtimeInstances(0), runtimeDestroyed(0), recorderCalls(0),
		updateCalls(0), collectorCalls(0), endFrames(0), frameOpen(false),
		borrowAtRecorder(false), borrowAtEveryUpdate(true),
		collectorAfterEndFrame(true), detachedBeforeRuntimeDestruction(false),
		finalized(false), terminalKnown(false), lastCompletedFrame(0),
		terminalFrame(0), phaseRequested(false) {}
	unsigned runtimeInstances, runtimeDestroyed, recorderCalls;
	unsigned updateCalls, collectorCalls, endFrames;
	bool frameOpen, borrowAtRecorder, borrowAtEveryUpdate;
	bool collectorAfterEndFrame, detachedBeforeRuntimeDestruction;
	bool finalized, terminalKnown;
	unsigned lastCompletedFrame, terminalFrame;
	bool phaseRequested;
	PerformanceReceiptWorkload workload;
};

Outcome outcome;

// Small external data collaborators. They do not implement receipt authority.
class AsciiString
{
public:
	AsciiString() {}
	AsciiString(const char *text) : value(text != 0 ? text : "") {}
	const char *str() const { return value.c_str(); }
private:
	std::string value;
};

struct GlobalData { Bool m_headless; } globalData = { TRUE };
GlobalData *TheGlobalData = &globalData;

class Player
{
public:
	Bool isPlayableSide() const { return TRUE; }
	Bool isPlayerObserver() const { return FALSE; }
};

class PlayerList
{
public:
	Int getPlayerCount() const { return 1; }
	Player *getNthPlayer(Int index) { return index == 0 ? &player : 0; }
private:
	Player player;
};
PlayerList playerList;
PlayerList *ThePlayerList = &playerList;

enum { KINDOF_INFANTRY = 1, KINDOF_VEHICLE = 2 };
class Object
{
public:
	Object *getNextObject() const { return 0; }
	Bool isKindOf(Int kind) const { return kind == KINDOF_INFANTRY; }
	Bool isEffectivelyDead() const { return FALSE; }
	Bool isDestroyed() const { return FALSE; }
};

class GameLogic
{
public:
	GameLogic(unsigned entryFrame, bool controlPass)
		: m_stage5PhaseGraph(makeStage5PhaseGraphCallbacks(), this),
		  m_stage5PhaseCursor(0), m_stage5PhaseNow(0),
		  m_performanceReceiptRuntime(0), m_isInUpdate(FALSE),
		  m_frame(entryFrame), m_controlPass(controlPass), m_updates(0) {}
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
	unsigned getFrame() const { return m_frame; }
	Object *getFirstObject() { return &object; }
	unsigned getCRC(Int) const { return 0x89ABCDEFU; }
	const rts::LiveSimulationPhaseRuntimeMetrics &getStage5PhaseRuntimeMetrics() const
	{
		return m_stage5PhaseGraph.runtimeMetrics();
	}
	void UPDATE();
	Bool runOwnerIntakePhase(UnsignedInt &now)
	{
		now = m_frame;
		// Controlled world transition, not a replacement owner/producer loop.
		if (m_updates == 0) m_frame = 0;
		return TRUE;
	}
	void runLegacyMutableIslandPhase(UnsignedInt) {}
	void runSpatialPhase() {}
	void runOwnerTailPhase() {}
	void runVerificationAndPublicationPhase()
	{
		if (!(m_controlPass && m_updates == 0)) ++m_frame;
	}
	rts::LiveSimulationPhaseGraphOwnerAdapter m_stage5PhaseGraph;
	UnsignedInt m_stage5PhaseCursor, m_stage5PhaseNow;
	PerformanceReceiptRuntime *m_performanceReceiptRuntime;
	Bool m_isInUpdate;
private:
	unsigned m_frame;
	bool m_controlPass;
	unsigned m_updates;
	Object object;
};

// A fixture-only derived view, not an alternative receipt implementation.
// All lifecycle/ledger behavior is inherited unchanged from the real runtime.
// The collector body below is generated verbatim from ReplaySimulation.cpp;
// only its qualified function name changes to permit ordering observation.
class PerformanceReceiptRuntime : public ::PerformanceReceiptRuntime
{
public:
	PerformanceReceiptRuntime()
	{
		++outcome.runtimeInstances;
		Check(currentRuntime == 0, "one actual runtime owns this producer call");
		currentRuntime = this;
	}
	~PerformanceReceiptRuntime()
	{
		++outcome.runtimeDestroyed;
		outcome.detachedBeforeRuntimeDestruction = TheGameLogic != 0 &&
			TheGameLogic->m_performanceReceiptRuntime == 0;
		outcome.finalized = m_lifecycle.finalized();
		outcome.terminalKnown = m_lifecycle.terminalResultKnown();
		outcome.lastCompletedFrame = m_lifecycle.lastCompletedFrame();
		outcome.terminalFrame = m_lifecycle.terminalFrame();
		outcome.workload = m_receipt.workload;
		outcome.phaseRequested = m_receipt.kernelTiming.phaseAccounting.requested;
		currentRuntime = 0;
	}
	void captureCompletedFrame(unsigned previousFrame,
		const rts::CollisionCandidateRuntimeMetrics &collision,
		const rts::PhysicsIntegrationRuntimeMetrics &physics,
		const rts::ObjectStatusTimerRuntimeMetrics &status,
		const rts::ImmutableSpatialRuntimeMetrics &spatial,
		const OrdinaryPathRuntimeMetrics &path)
	{
		++outcome.collectorCalls;
		outcome.collectorAfterEndFrame = outcome.collectorAfterEndFrame &&
			!outcome.frameOpen && outcome.endFrames == outcome.collectorCalls;
		captureCompletedFrameFromSource(previousFrame, collision, physics,
			status, spatial, path);
	}
private:
	void captureCompletedFrameFromSource(unsigned previousFrame,
		const rts::CollisionCandidateRuntimeMetrics &collision,
		const rts::PhysicsIntegrationRuntimeMetrics &physics,
		const rts::ObjectStatusTimerRuntimeMetrics &status,
		const rts::ImmutableSpatialRuntimeMetrics &spatial,
		const OrdinaryPathRuntimeMetrics &path);
};

#include "PerformanceReceiptProducerOwnerMethods.inc"
#include "PerformanceReceiptProducerMetricCopy.inc"
#include "PerformanceReceiptProducerCapture.inc"

void GameLogic::UPDATE()
{
	++outcome.updateCalls;
	outcome.borrowAtEveryUpdate = outcome.borrowAtEveryUpdate &&
		currentRuntime != 0 && m_performanceReceiptRuntime == currentRuntime;
	m_isInUpdate = TRUE;
	m_stage5PhaseCursor = 0;
	const rts::LiveSimulationPhaseRunResult result =
		m_stage5PhaseGraph.runFrame(m_frame);
	m_isInUpdate = FALSE;
	++m_updates;
	Check(result == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"controlled world executes the real title owner phase transport");
}

struct GameInfo { Int getSeed() const { return 17; } } recordedGame;

class RecorderClass
{
public:
	RecorderClass(bool failOpen, unsigned updates)
		: m_failOpen(failOpen), m_updates(updates) {}
	static AsciiString getReplayDir() { return AsciiString("."); }
	Bool playbackFile(const AsciiString &) { return FALSE; }
	Bool simulateReplay(const AsciiString &)
	{
		++outcome.recorderCalls;
		outcome.borrowAtRecorder = currentRuntime != 0 && currentRuntime->active() &&
			TheGameLogic != 0 && TheGameLogic->m_performanceReceiptRuntime == currentRuntime;
		return m_failOpen ? FALSE : TRUE;
	}
	const GameInfo *getGameInfo() const { return &recordedGame; }
	Bool hasReplayReadError() const { return FALSE; }
	Bool sawCRCMismatch() const { return FALSE; }
	unsigned getPlaybackFrameCount() const { return m_updates; }
	Bool isPlaybackInProgress() const { return outcome.updateCalls < m_updates; }
private:
	bool m_failOpen;
	unsigned m_updates;
};
RecorderClass *TheRecorder = 0;

struct GameEngine
{
	void execute() { Check(false, "headless fixture must not enter interactive engine"); }
	void setQuitting(Bool) {}
} gameEngine;
GameEngine *TheGameEngine = &gameEngine;

class ReplaySimulation
{
public:
	static int simulateReplaysInThisProcess(const std::vector<AsciiString> &filenames);
	static Bool s_isRunning;
	static UnsignedInt s_replayIndex, s_replayCount;
};
Bool ReplaySimulation::s_isRunning = FALSE;
UnsignedInt ReplaySimulation::s_replayIndex = 0;
UnsignedInt ReplaySimulation::s_replayCount = 0;

#include "PerformanceReceiptProducerSourceHold.inc"
#include "PerformanceReceiptProducerPrintMetrics.inc"
#include "PerformanceReceiptProducerJobScope.inc"
} // namespace performance_receipt_producer_fixture

namespace rts { namespace frame_timing {
inline void ProducerTestBeginFrame(unsigned frame)
{
	BeginFrame(frame);
	performance_receipt_producer_fixture::outcome.frameOpen = true;
}
inline void ProducerTestEndFrame(unsigned frame)
{
	EndFrame(frame);
	performance_receipt_producer_fixture::outcome.frameOpen = false;
	++performance_receipt_producer_fixture::outcome.endFrames;
}
} }

namespace performance_receipt_producer_fixture
{
// Preserve actual frame diagnostics; observe only real callsite ordering.
#define BeginFrame ProducerTestBeginFrame
#define EndFrame ProducerTestEndFrame
#include "PerformanceReceiptProducerLoop.inc"
#undef EndFrame
#undef BeginFrame

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
			"process-local receipt environment is set");
	}
	~ReceiptEnvironment()
	{
		for (std::size_t i = m_saved.size(); i != 0; --i)
		{
			const Saved &saved = m_saved[i - 1];
			SetEnvironmentVariableA(saved.name.c_str(),
				saved.present ? saved.value.c_str() : 0);
		}
	}
private:
	struct Saved { std::string name, value; bool present; };
	std::vector<Saved> m_saved;
};

class SourceFile
{
public:
	SourceFile() : created(false) {}
	bool create()
	{
		char directory[MAX_PATH], name[96], absolute[MAX_PATH];
		const DWORD size = GetCurrentDirectoryA(sizeof(directory), directory);
		if (size == 0 || size >= sizeof(directory)) return false;
		_snprintf(name, sizeof(name), "producer-contract-%lu-%lu.rep",
			GetCurrentProcessId(), GetTickCount());
		name[sizeof(name) - 1] = '\0';
		const int length = _snprintf(absolute, sizeof(absolute), "%s\\%s", directory, name);
		if (length <= 0 || length >= static_cast<int>(sizeof(absolute))) return false;
		path = absolute;
		HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, 0,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
		if (file == INVALID_HANDLE_VALUE) return false;
		created = true;
		// Opaque bytes only: Recorder behavior is controlled, not a claimed replay.
		const unsigned char bytes[] = { 0x44, 0x32, 0x42, 0x01, 0x00, 0x83, 0x01 };
		DWORD written = 0;
		const bool ok = WriteFile(file, bytes, sizeof(bytes), &written, 0) != FALSE &&
			written == sizeof(bytes);
		const bool closed = CloseHandle(file) != FALSE;
		return ok && closed;
	}
	~SourceFile()
	{
		if (created) Check(DeleteFileA(path.c_str()) != FALSE,
			"only the exclusively created fixture source is removed after all holds close");
	}
	std::string path;
private:
	bool created;
};

void RunCase(const char *name, const char *sourcePath, unsigned entryFrame,
	bool controlPass, bool failOpen)
{
	caseName = name;
	outcome = Outcome();
	GameLogic logic(entryFrame, controlPass);
	RecorderClass recorder(failOpen, controlPass ? 3 : 2);
	TheGameLogic = &logic;
	TheRecorder = &recorder;
	std::vector<AsciiString> filenames(1, AsciiString(sourcePath));
	const int result = ReplaySimulation::simulateReplaysInThisProcess(filenames);
	Check(result == (failOpen ? 1 : 0), "real producer preserves Recorder return classification");
	Check(outcome.runtimeInstances == 1 && outcome.runtimeDestroyed == 1,
		"real producer constructs and releases exactly one actual runtime");
	Check(outcome.recorderCalls == 1 && outcome.borrowAtRecorder,
		"real job scope borrows its active runtime before Recorder entry");
	Check(outcome.detachedBeforeRuntimeDestruction,
		"job-scope release precedes runtime destruction, including failed-open early path");
	Check(outcome.finalized, "real Runtime::finish ran before its storage was released");
	Check(!outcome.phaseRequested,
		"ordinary pipeline receipt never becomes a phase baseline");
	if (failOpen)
	{
		Check(outcome.updateCalls == 0 && outcome.collectorCalls == 0 &&
			outcome.endFrames == 0 && outcome.workload.sampleCount == 0 &&
			!outcome.terminalKnown, "failed open invents no completed workload or terminal result");
	}
	else
	{
		const unsigned updates = controlPass ? 3 : 2;
		Check(outcome.updateCalls == updates && outcome.borrowAtEveryUpdate,
			"each actual replay UPDATE retains the same real runtime borrow");
		Check(outcome.collectorCalls == updates && outcome.endFrames == updates &&
			outcome.collectorAfterEndFrame,
			"real loop invokes workload collector once after each actual EndFrame");
		Check(outcome.workload.sampleCount == 2 && outcome.workload.firstFrame == 1 &&
			outcome.workload.lastFrame == 2 && outcome.workload.playerCount == 1,
			"actual collector retains completed1 and2 exactly once, never control0");
		Check(outcome.lastCompletedFrame == 2 && outcome.terminalKnown &&
			outcome.terminalFrame == 2,
			"real lifecycle terminal agrees with retained actual completed workload");
	}
	Check(!rts::JobSystem::instance().isRunning() &&
		rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"bounded serial producer leaves actual scheduler idle without launching workers");
	// Failure-only fixture cleanup: never dereference a possibly stale borrow.
	// The behavioral assertion above has already checked actual production release.
	logic.m_performanceReceiptRuntime = 0;
	TheGameLogic = 0;
	TheRecorder = 0;
}
} // namespace performance_receipt_producer_fixture

int RunPerformanceReceiptProducerTitleTests()
{
	using namespace performance_receipt_producer_fixture;
	failures = 0;
	caseName = "prerequisite";
	Check(!rts::JobSystem::instance().isRunning() &&
		rts::JobSystem::instance().outstandingJobCount() == 0 &&
		rts::JobSystem::instance().pendingOwnerCompletionCount() == 0,
		"producer fixture must not overlap scheduler work");
	if (failures != 0) return 2;
	Check(rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL),
		"fixture selects existing ordinary serial execution before policy locks");
	Check(rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL),
		"fixture selects existing ordinary serial pipeline before policy locks");
	const bool attachOwner = !GameThreadOwnership::IsAttached();
	if (attachOwner) GameThreadOwnership::AttachCurrentThread();
	Check(GameThreadOwnership::IsCurrentThread(), "actual game-owner identity is current");
	if (failures != 0) return 2;
	{
		ReceiptEnvironment environment;
		const char *values[][2] = {
			{ "RTS_PERFORMANCE_ROLE", "performance-report" },
			{ "RTS_PERFORMANCE_RUN_ID", "replay-producer-contract-no-publication" },
			{ "RTS_PERFORMANCE_RUN_NONCE", "11111111-1111-4111-8111-111111111111" },
			{ "RTS_PERFORMANCE_COHORT_NONCE", "22222222-2222-4222-8222-222222222222" },
			{ "RTS_PERFORMANCE_COHORT_CREATED_UTC", "2026-01-01T00:00:00Z" },
			{ "RTS_PERFORMANCE_RECEIPT_DIR", "producer-contract-no-publication" },
			{ "RTS_PERFORMANCE_SOURCE_COMMIT", "0123456789abcdef0123456789abcdef01234567" },
			{ "RTS_PERFORMANCE_ARTIFACT_SET_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
			{ "RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
			{ "RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
			{ "RTS_PERFORMANCE_FIXTURE_ID", "replay-producer-contract" },
			{ "RTS_PERFORMANCE_RAW_LOG_PATH", "producer-contract-no-publication/raw.log" },
			{ "RTS_PERFORMANCE_TIMING_PATH", "producer-contract-no-publication/timing.csv" },
			{ "RTS_PERFORMANCE_VERIFIER_BOUNDARY", "test-only-no-publication" },
			{ "RTS_PERFORMANCE_REFERENCE_MODE", "throughput-binding" },
			{ "RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "observed-only" },
			{ "RTS_PERFORMANCE_FIXTURE_KIND", "replay" },
			{ "RTS_PERFORMANCE_FIXTURE_SHA256", 0 },
			{ "RTS_PERFORMANCE_PLAYER_COUNT", 0 },
			{ "RTS_PERFORMANCE_UNIT_COUNT", 0 },
			{ "RTS_PERFORMANCE_SEED", 0 },
			{ "RTS_FRAME_TIMING_DIR", 0 }
		};
		for (unsigned i = 0; i != sizeof(values) / sizeof(values[0]); ++i)
			environment.set(values[i][0], values[i][1]);
		SourceFile source;
		Check(source.create(), "unique source bytes are created in the dedicated test working directory");
		if (failures == 0)
		{
			RunCase("normal", source.path.c_str(), 0, false, false);
			RunCase("reset83-to1", source.path.c_str(), 83, false, false);
			RunCase("control0-then1", source.path.c_str(), 83, true, false);
			RunCase("failed-open", source.path.c_str(), 83, false, true);
		}
	}
	if (attachOwner) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Performance receipt producer title tests passed (four bounded replay cases).\n");
	return 0;
}
