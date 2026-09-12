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
#include "Common/GlobalData.h"
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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "NativeReceiptCallerFenceInputs.h"

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

void Require(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "PREREQUISITE [%s]: %s\n", caseName, message);
		exit(2);
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
		terminalFrame(0), phaseRequested(false), playbackCalls(0),
		engineExecuteCalls(0), processStartCalls(0), allowInteractive(false), engineQuitting(false) {}
	unsigned runtimeInstances, runtimeDestroyed, recorderCalls;
	unsigned updateCalls, collectorCalls, endFrames;
	bool frameOpen, borrowAtRecorder, borrowAtEveryUpdate;
	bool collectorAfterEndFrame, detachedBeforeRuntimeDestruction;
	bool finalized, terminalKnown;
	unsigned lastCompletedFrame, terminalFrame;
	bool phaseRequested;
	unsigned playbackCalls, engineExecuteCalls, processStartCalls;
	bool allowInteractive, engineQuitting;
	std::wstring workerCommand;
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
	const char *find(char character) const { return strchr(value.c_str(), character); }
	int getLength() const { return static_cast<int>(value.size()); }
	char getCharAt(int index) const { return value[static_cast<size_t>(index)]; }
	void set(const char *text) { value = text != 0 ? text : ""; }
	void removeLastChar() { if (!value.empty()) value.resize(value.size() - 1); }
private:
	std::string value;
};

#include "NativeReceiptReplayShapeDependencies.inc"

struct GlobalData { Bool m_headless, m_windowed; } globalData = { TRUE, FALSE };
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
	void replaceFrameForFault(unsigned frame) { m_frame = frame; }
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
	const PerformanceReceiptWorkload &capturedWorkload() const
	{
		return m_receipt.workload;
	}
	bool qualificationFailed() const
	{
		return !m_failure.empty() ||
			m_phaseObservationFailed.load(std::memory_order_acquire);
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
		: m_failOpen(failOpen), m_updates(updates), m_updateStart(0) {}
	static AsciiString getReplayDir() { return AsciiString("."); }
	Bool playbackFile(const AsciiString &)
	{
		Check(outcome.allowInteractive, "headless fixture must not enter interactive playback");
		++outcome.playbackCalls;
		return m_failOpen ? FALSE : TRUE;
	}
	Bool simulateReplay(const AsciiString &)
	{
		m_updateStart = outcome.updateCalls;
		++outcome.recorderCalls;
		outcome.borrowAtRecorder = currentRuntime != 0 && currentRuntime->active() &&
			TheGameLogic != 0 && TheGameLogic->m_performanceReceiptRuntime == currentRuntime;
		return m_failOpen ? FALSE : TRUE;
	}
	const GameInfo *getGameInfo() const { return &recordedGame; }
	Bool hasReplayReadError() const { return FALSE; }
	Bool sawCRCMismatch() const { return FALSE; }
	unsigned getPlaybackFrameCount() const { return m_updates; }
	Bool isPlaybackInProgress() const { return outcome.updateCalls - m_updateStart < m_updates; }
private:
	bool m_failOpen;
	unsigned m_updates, m_updateStart;
};
RecorderClass *TheRecorder = 0;

struct GameEngine
{
	void execute()
	{
		Check(outcome.allowInteractive, "headless fixture must not enter interactive engine");
		++outcome.engineExecuteCalls;
	}
	void setQuitting(Bool value) { outcome.engineQuitting = value != FALSE; }
} gameEngine;
GameEngine *TheGameEngine = &gameEngine;

class ReplaySimulation
{
public:
	static int simulateReplaysInThisProcess(const std::vector<AsciiString> &filenames);
	static int simulateReplaysInWorkerProcesses(const std::vector<AsciiString> &filenames, int maxProcesses);
	static std::vector<AsciiString> resolveFilenameWildcards(const std::vector<AsciiString> &filenames);
	static int simulateReplays(const std::vector<AsciiString> &filenames, int maxProcesses);
	static Bool s_isRunning;
	static UnsignedInt s_replayIndex, s_replayCount;
};
Bool ReplaySimulation::s_isRunning = FALSE;
UnsignedInt ReplaySimulation::s_replayIndex = 0;
UnsignedInt ReplaySimulation::s_replayCount = 0;

#include "PerformanceReceiptNativePathIdentity.inc"
#include "PerformanceReceiptProducerSourceHold.inc"
#include "PerformanceReceiptProducerPrintMetrics.inc"
#include "PerformanceReceiptRawProducer.inc"
#include "PerformanceReceiptProducerJobScope.inc"
#include "PerformanceReceiptProducerCountProcesses.inc"

void TestRawProducerSchemaLabel()
{
	const char *const expected[] = {
		"producer=game-executable-performance-receipt-v5\n",
		"producer=game-executable-performance-receipt-v6\n"
	};
	for (unsigned index = 0; index != 2; ++index)
	{
		rts::performance::PerformanceReceipt receipt;
		receipt.schemaVersion = index == 0 ? 5 : 6;
		char path[128] = {};
		_snprintf(path, sizeof(path), "raw-producer-%lu-%u.log",
			static_cast<unsigned long>(GetCurrentProcessId()), index);
		remove(path);
		receipt.rawEvidence.rawLogPath = path;
		Check(writePerformanceReceiptRawDiagnostic(receipt),
			"schema-specific raw diagnostic closes successfully");
		// Read as text so the assertion tests the producer identity rather than
		// depending on the Windows text writer's CRLF representation.
		FILE *file = fopen(path, "r");
		char line[128] = {};
		Check(file != 0 && fgets(line, sizeof(line), file) != 0 &&
			strcmp(line, expected[index]) == 0,
			"raw diagnostic producer label matches its actual V5/V6 receipt schema");
		if (file != 0) Check(fclose(file) == 0, "raw diagnostic assertion closes its reader");
		remove(path);
	}
}
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
#include "PerformanceReceiptProducerWorkerLoop.inc"
#include "PerformanceReceiptProducerResolveWildcards.inc"
#include "PerformanceReceiptProducerPublicRoute.inc"

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
	SourceFile() : created(false), directoryCreated(false) {}
	bool create()
	{
		char workingDirectory[MAX_PATH], name[96], absolute[MAX_PATH];
		const DWORD size = GetCurrentDirectoryA(sizeof(workingDirectory), workingDirectory);
		if (size == 0 || size >= sizeof(workingDirectory)) return false;
		_snprintf(name, sizeof(name), "producer-contract-%lu-%lu",
			GetCurrentProcessId(), GetTickCount());
		name[sizeof(name) - 1] = '\0';
		const int directoryLength = _snprintf(absolute, sizeof(absolute), "%s\\%s",
			workingDirectory, name);
		if (directoryLength <= 0 || directoryLength >= static_cast<int>(sizeof(absolute))) return false;
		directory = absolute;
		if (CreateDirectoryA(directory.c_str(), 0) == FALSE) return false;
		directoryCreated = true;
		const int length = _snprintf(absolute, sizeof(absolute), "%s\\source.rep",
			directory.c_str());
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
		if (directoryCreated) Check(RemoveDirectoryA(directory.c_str()) != FALSE,
			"only the exclusively created fixture source directory is removed after all holds close");
	}
	std::string path, directory;
private:
	bool created, directoryCreated;
};

void TestImmutableReplayReceiptSource(const char *path)
{
	caseName = "immutable-replay-source";
	const unsigned char bytes[] = { 0x44, 0x32, 0x42, 0x01, 0x00, 0x83, 0x01 };
	char expected[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] = {0};
	Check(HashSkirmishAITestBytes(bytes, sizeof(bytes), expected),
		"independent byte hash prerequisite is available");
	{
		ImmutableReplayReceiptSource source;
		Check(source.open(path),
			"immutable replay source opens the exact source handle and hashes it");
		Check(strcmp(source.sha256(), expected) == 0,
			"immutable replay source digest comes from the held handle bytes");
		SetLastError(ERROR_SUCCESS);
		HANDLE writer = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
		const DWORD writeError = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
		if (writer != INVALID_HANDLE_VALUE)
			CloseHandle(writer);
		Check(writer == INVALID_HANDLE_VALUE && writeError == ERROR_SHARING_VIOLATION,
			"held replay source denies mutation while its digest and replay identity are live");
		char replacement[MAX_PATH] = {0};
		const int replacementLength = _snprintf(replacement, sizeof(replacement),
			"%s.alias", path);
		SetLastError(ERROR_SUCCESS);
		const BOOL removedExisting = DeleteFileA(replacement);
		const DWORD removeError = removedExisting ? ERROR_SUCCESS : GetLastError();
		Check(replacementLength > 0 && replacementLength < static_cast<int>(sizeof(replacement)) &&
			(removedExisting != FALSE || removeError == ERROR_FILE_NOT_FOUND),
			"immutable replay alias fixture starts absent");
		const BOOL moved = MoveFileExA(path, replacement, 0);
		if (moved)
		{
			Check(MoveFileExA(replacement, path, 0) != FALSE,
				"unexpected replay replacement is restored before the fixture continues");
		}
		Check(moved == FALSE,
			"held replay source denies pathname replacement while replay is admitted");
		char directoryReplacement[MAX_PATH] = {0};
		const char *leaf = strrchr(path, '\\');
		Require(leaf != 0, "absolute replay source has a leaf component");
		const std::string directory(path, static_cast<size_t>(leaf - path));
		const int directoryReplacementLength = _snprintf(directoryReplacement,
			sizeof(directoryReplacement), "%s.alias", directory.c_str());
		DeleteFileA(directoryReplacement);
		RemoveDirectoryA(directoryReplacement);
		const BOOL movedDirectory = MoveFileExA(directory.c_str(), directoryReplacement, 0);
		if (movedDirectory)
			Check(MoveFileExA(directoryReplacement, directory.c_str(), 0) != FALSE,
				"unexpected replay ancestor replacement is restored before the fixture continues");
		Check(directoryReplacementLength > 0 &&
			directoryReplacementLength < static_cast<int>(sizeof(directoryReplacement)) &&
			movedDirectory == FALSE,
			"each replay ancestor remains held against pathname retargeting through Recorder");
		Check(source.finish(),
			"immutable replay source rechecks its extent and identity before checked closure");
	}
	const char *leaf = strrchr(path, '\\');
	Require(leaf != 0, "absolute replay source has a leaf component");
	std::string nonCanonical(path, static_cast<size_t>(leaf - path + 1));
	nonCanonical += ".\\";
	nonCanonical += leaf + 1;
	{
		ImmutableReplayReceiptSource aliasedAncestor;
		Check(!aliasedAncestor.open(nonCanonical.c_str()),
			"replay admission rejects a pathname alias before any source identity is trusted");
	}
	char alias[MAX_PATH] = {0};
	const int aliasLength = _snprintf(alias, sizeof(alias), "%s.hardlink", path);
	Check(aliasLength > 0 && aliasLength < static_cast<int>(sizeof(alias)),
		"immutable replay alias path remains bounded");
	DeleteFileA(alias);
	Check(CreateHardLinkA(alias, path, 0) != FALSE,
		"hard-link alias prerequisite is available for replay identity fencing");
	{
		ImmutableReplayReceiptSource aliased;
		Check(!aliased.open(alias),
			"immutable replay source rejects a hard-link pathname alias before hashing");
	}
	Check(DeleteFileA(alias) != FALSE,
		"only the task-owned replay hard-link alias is removed after the identity test");
	char oversized[MAX_PATH] = {0};
	const int oversizedLength = _snprintf(oversized, sizeof(oversized), "%s.oversized", path);
	Require(oversizedLength > 0 && oversizedLength < static_cast<int>(sizeof(oversized)),
		"oversized replay fixture path remains bounded");
	DeleteFileA(oversized);
	HANDLE oversizedFile = CreateFileA(oversized, GENERIC_WRITE, 0, 0,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
	Require(oversizedFile != INVALID_HANDLE_VALUE,
		"oversized replay fixture is exclusively created");
	LARGE_INTEGER oversizedExtent = {};
	oversizedExtent.QuadPart = static_cast<LONGLONG>(SKIRMISH_AI_TEST_MAX_REPLAY_BYTES) + 1;
	const bool oversizedCreated = SetFilePointerEx(oversizedFile, oversizedExtent, 0,
		FILE_BEGIN) != FALSE && SetEndOfFile(oversizedFile) != FALSE &&
		CloseHandle(oversizedFile) != FALSE;
	Require(oversizedCreated, "sparse oversized replay fixture has an exact bounded extent");
	{
		ImmutableReplayReceiptSource oversizedSource;
		Check(!oversizedSource.open(oversized),
			"replay admission rejects content above its fixed extent ceiling before hashing");
	}
	Check(DeleteFileA(oversized) != FALSE,
		"only the task-owned oversized replay fixture is removed");
}

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

void CaptureCurrentOwnerCompletion(PerformanceReceiptRuntime &runtime,
	unsigned previousFrame)
{
	// Call the actual collector, not a replacement completion accumulator.
	runtime.captureCompletedFrame(previousFrame,
		rts::GetCollisionCandidateRuntimeMetrics(),
		rts::GetPhysicsIntegrationRuntimeMetrics(),
		rts::GetObjectStatusTimerRuntimeMetrics(),
		rts::GetImmutableSpatialRuntimeMetrics(), GetOrdinaryPathRuntimeMetrics());
}

struct OrdinaryObservationClock
{
	OrdinaryObservationClock() : reads(0) {}
	static rts::JobMetricCounter Read(void *context)
	{
		OrdinaryObservationClock &clock = *static_cast<OrdinaryObservationClock *>(context);
		return 100 + 10 * clock.reads++;
	}
	unsigned reads;
};

void TestBaselineMetricProjection()
{
	caseName = "baseline-metric-projection";
	const char *const phaseNames[KERNEL_PHASE_COUNT] = {
		"owner-intake", "legacy-mutable-island", "spatial-work",
		"owner-tail", "verification-publication"
	};
	const char *const kernelNames[KERNEL_PERFORMANCE_KERNEL_COUNT] = {
		"physics", "status", "collision", "ai-planning", "spatial", "path"
	};
	const rts::JobMetricCounter totals[KERNEL_PHASE_COUNT] = { 101, 202, 303, 404, 505 };
	const rts::JobMetricCounter maximums[KERNEL_PHASE_COUNT] = { 51, 92, 153, 204, 255 };
	const rts::JobMetricCounter serials[KERNEL_PHASE_COUNT] = { 31, 82, 123, 164, 205 };
	const rts::JobMetricCounter pures[KERNEL_PHASE_COUNT] = { 70, 120, 180, 240, 300 };

	PerformanceReceipt baseline;
	baseline.schemaVersion = 6;
	baseline.kernelReference.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	baseline.kernelTiming.runRole = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	KernelPerformancePhaseAccountingSnapshot &accounting =
		baseline.kernelTiming.phaseAccounting;
	accounting.requested = true;
	accounting.frozen = true;
	accounting.complete = true;
	accounting.errors = 0;
	accounting.completedFrameCount = 3;
	accounting.schedulerClosureKnown = true;
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		KernelPerformancePhaseAccountingRow &row = accounting.phases[index];
		row.totalNanoseconds = totals[index];
		row.maximumNanoseconds = maximums[index];
		row.samples = 3;
		row.serialNanoseconds = serials[index];
		row.pureNanoseconds = pures[index];
		appendPerformanceReceiptPhase(baseline, phaseNames[index], true,
			900 + index, 800 + index, 7);
		baseline.phases[index].serialNanoseconds = 700 + index;
		baseline.phases[index].serialNanosecondsKnown = false;
		baseline.phases[index].pureNanoseconds = 600 + index;
		baseline.phases[index].pureNanosecondsKnown = false;
	}
	for (unsigned index = 0; index != KERNEL_PERFORMANCE_KERNEL_COUNT; ++index)
	{
		appendPerformanceReceiptKernel(baseline, kernelNames[index], true,
			11 + index, 21 + index, 31 + index, 41 + index,
			51 + index, 2 + index, true);
		baseline.kernels[index].elapsedNanoseconds = 61 + index;
		baseline.kernels[index].elapsedNanosecondsKnown = true;
	}

	PerformanceReceipt throughput = baseline;
	throughput.kernelReference.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
	PerformanceReceiptPhase *const throughputPhases = &throughput.phases[0];
	PerformanceReceiptKernel *const throughputKernels = &throughput.kernels[0];
	const std::size_t throughputPhaseCapacity = throughput.phases.capacity();
	const std::size_t throughputKernelCapacity = throughput.kernels.capacity();
	Check(projectPerformanceReceiptBaselineMetrics(throughput),
		"throughput projection is a successful strict no-op");
	Check(&throughput.phases[0] == throughputPhases &&
		throughput.phases.capacity() == throughputPhaseCapacity &&
		throughput.phases[0].totalNanoseconds == 900 &&
		throughput.phases[0].serialNanoseconds == 700 &&
		!throughput.phases[0].serialNanosecondsKnown,
		"throughput projection preserves existing phase storage and evidence");
	Check(&throughput.kernels[0] == throughputKernels &&
		throughput.kernels.capacity() == throughputKernelCapacity &&
		throughput.kernels[0].available &&
		throughput.kernels[0].submittedJobs == 11 &&
		throughput.kernels[0].elapsedNanoseconds == 61 &&
		throughput.kernels[0].elapsedNanosecondsKnown,
		"throughput projection preserves existing kernel storage and evidence");

	PerformanceReceipt incomplete = baseline;
	incomplete.kernelTiming.phaseAccounting.complete = false;
	Check(!projectPerformanceReceiptBaselineMetrics(incomplete),
		"incomplete frozen baseline accounting fails closed");
	Check(incomplete.phases[0].totalNanoseconds == 900 &&
		incomplete.phases[0].serialNanoseconds == 700 &&
		incomplete.kernels[0].available &&
		incomplete.kernels[0].submittedJobs == 11 &&
		incomplete.kernels[0].elapsedNanosecondsKnown,
		"failed projection does not partially rewrite receipt metrics");

	PerformanceReceiptPhase *const baselinePhases = &baseline.phases[0];
	PerformanceReceiptKernel *const baselineKernels = &baseline.kernels[0];
	const std::size_t baselinePhaseCapacity = baseline.phases.capacity();
	const std::size_t baselineKernelCapacity = baseline.kernels.capacity();
	Check(projectPerformanceReceiptBaselineMetrics(baseline),
		"complete V6 phase baseline projects its frozen accounting");
	Check(&baseline.phases[0] == baselinePhases &&
		baseline.phases.capacity() == baselinePhaseCapacity &&
		&baseline.kernels[0] == baselineKernels &&
		baseline.kernels.capacity() == baselineKernelCapacity,
		"baseline projection mutates existing canonical rows in place");
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const PerformanceReceiptPhase &phase = baseline.phases[index];
		Check(phase.name == phaseNames[index] && phase.available &&
			phase.totalNanoseconds == totals[index] &&
			phase.maximumNanoseconds == maximums[index] &&
			phase.sampleCount == 3 &&
			phase.serialNanoseconds == serials[index] &&
			phase.serialNanosecondsKnown &&
			phase.pureNanoseconds == pures[index] &&
			phase.pureNanosecondsKnown,
			"each baseline wire phase exactly matches its frozen partition row");
	}
	for (unsigned index = 0; index != KERNEL_PERFORMANCE_KERNEL_COUNT; ++index)
	{
		const PerformanceReceiptKernel &kernel = baseline.kernels[index];
		Check(kernel.name == kernelNames[index] && !kernel.available &&
			kernel.submittedJobs == 0 && kernel.completedJobs == 0 &&
			kernel.physicalWorkerJobs == 0 && kernel.ownerHelpedJobs == 0 &&
			kernel.physicalWorkerMask == 0 && kernel.distinctPhysicalWorkers == 0 &&
			!kernel.physicalWorkerMaskComplete && kernel.elapsedNanoseconds == 0 &&
			!kernel.elapsedNanosecondsKnown,
			"phase baseline clears every physical kernel execution claim");
	}
}

void TestPendingOwnerCompletion(const char *sourcePath)
{
	const char *names[] = {
		"terminal-before-matching-consumption", "unobserved-world-frame",
		"duplicate-completion-consumption", "unconsumed-previous-completion",
		"world-changed-before-consumption", "outer-loop-without-update",
		"collector-inside-update", "missing-roster-at-consumption",
		"wrong-update-entry-frame", "terminal-consumed-before-real-tail-updates",
		"terminal-pending-world-mismatch"
	};
	for (unsigned scenario = 0; scenario != ARRAY_SIZE(names); ++scenario)
	{
		caseName = names[scenario];
		outcome = Outcome();
		GameLogic logic(scenario == 8 ? 83 : 0, false);
		TheGameLogic = &logic;
		{
			PerformanceReceiptRuntime runtime;
			Check(runtime.begin("replay", sourcePath),
				"completion contract begins the actual receipt runtime");
			OrdinaryObservationClock clock;
			KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
			timing.freeze();
			KernelPerformanceTimingRunOptions timingOptions;
			timingOptions.enabled = true;
			timingOptions.role = KERNEL_PERFORMANCE_PIPELINE;
			timingOptions.clock = &OrdinaryObservationClock::Read;
			timingOptions.clockContext = &clock;
			Check(timing.beginRun(timingOptions),
				"ordinary clock observer uses the unchanged pipeline ledger role");
			Check(logic.attachPerformanceReceiptRuntime(&runtime),
				"completion contract uses the actual title borrow");
			if (scenario == 1)
				logic.replaceFrameForFault(1);
			else
				logic.UPDATE();

			if (scenario == 0 || scenario == 9 || scenario == 10)
				runtime.captureTerminalResult(1, 0x89ABCDEFU, true, true);
			if (scenario == 3)
				logic.UPDATE(); // No collector consumed the first real completion.
			if (scenario == 4 || scenario == 10)
				logic.replaceFrameForFault(2);
			if (scenario == 6)
				logic.m_isInUpdate = TRUE;
			if (scenario == 7)
				ThePlayerList = 0;

			CaptureCurrentOwnerCompletion(runtime, scenario == 3 ? 1 : 0);
			logic.m_isInUpdate = FALSE;
			ThePlayerList = &playerList;
			if (scenario == 2)
				CaptureCurrentOwnerCompletion(runtime, 0);
			if (scenario == 5)
				CaptureCurrentOwnerCompletion(runtime, 1);
			if (scenario == 9)
			{
				// The genuine terminal completion was consumed above. Later
				// real owner updates belong to teardown, not this workload.
				logic.UPDATE();
				CaptureCurrentOwnerCompletion(runtime, 1);
				CaptureCurrentOwnerCompletion(runtime, 2); // Outer tick without UPDATE.
				logic.UPDATE();
				CaptureCurrentOwnerCompletion(runtime, 2);
			}

			const bool valid = scenario == 0 || scenario == 5 || scenario == 9;
			Check(clock.reads == 0 && timing.runRole() == KERNEL_PERFORMANCE_PIPELINE,
				"ordinary completion tracking adds no phase clocks or baseline authority");
			Check(runtime.qualificationFailed() == !valid,
				"only the matching once-consumed real owner completion qualifies");
			const PerformanceReceiptWorkload &workload = runtime.capturedWorkload();
			if (valid || scenario == 2)
			{
				Check(workload.sampleCount == 1 && workload.firstFrame == 1 &&
					workload.lastFrame == 1,
					"matching completion contributes one independently observed world sample");
			}
			else
			{
				Check(workload.sampleCount == 0,
					"unobserved, skipped, mismatched or unsafe completion adds no world sample");
			}
			Check(logic.detachPerformanceReceiptRuntime(&runtime),
				"completion contract releases the actual expected title borrow");
			KernelPerformanceReferenceLedger::instance().freeze();
			KernelPerformanceLedger::instance().freeze();
		}
		TheGameLogic = 0;
	}
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
			TestRawProducerSchemaLabel();
			TestBaselineMetricProjection();
			TestImmutableReplayReceiptSource(source.path.c_str());
			RunCase("normal", source.path.c_str(), 0, false, false);
			RunCase("reset83-to1", source.path.c_str(), 83, false, false);
			RunCase("control0-then1", source.path.c_str(), 83, true, false);
			RunCase("failed-open", source.path.c_str(), 83, false, true);
			TestPendingOwnerCompletion(source.path.c_str());
		}
	}
	if (attachOwner) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Performance receipt producer title tests passed (replay and owner completion contracts).\n");
	return 0;
}

#include "PerformanceReceiptReplayCallerFence.inc"
#include "PerformanceReceiptReplayOwnerShape.inc"
