/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Common/INI.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/SkirmishAITestRunner.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "Common/SkirmishAIReplayEpoch.h"
#include "Common/GeneralsPathfindingReplayEpoch.h"
#include "GameClient/GameText.h"
#include "GameLogic/AIPathfind.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#if defined(_WIN64)
#include <cstdint>
#include "AudioDevice/AudioAssetSource.h"
#include "AudioDevice/NullAudioManager.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioSettings.h"
#include "Common/FileSystem.h"
#include "Common/GameDefines.h"
#include "Common/LocalFileSystem.h"
#include "Common/RandomValue.h"
#include "GameLogic/ImmutableSpatialQueryRuntime.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#endif
#include "XferCrcSnapshotTest.h"
#include "WW3D2/textureloader.h"

#include <limits>
#include <stdio.h>
#include <string.h>
#include <windows.h>

class Win32Mouse;
HINSTANCE ApplicationHInstance = nullptr;
HWND ApplicationHWnd = nullptr;
Win32Mouse *TheWin32Mouse = nullptr;
DWORD TheMessageTime = 0;
const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
const char *gAppPrefix = "";
#if !defined(RTS_DEBUG)
ICoord2D TheMousePos = { 0, 0 };
#endif

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	return 0;
}

static Int s_failures = 0;

#define CHECK(expression) Check((expression), #expression, __LINE__)

static void Check(Bool result, const char *expression, Int line)
{
	if (!result)
	{
		printf("FAIL line %d: %s\n", line, expression);
		++s_failures;
	}
}

class SkirmishAITestGameText : public GameTextInterface
{
public:
	virtual void init() {}
	virtual void reset() {}
	virtual void update() {}

	virtual UnicodeString fetch(const Char *, Bool *exists = nullptr)
	{
		if (exists)
			*exists = FALSE;
		return UnicodeString();
	}
	virtual UnicodeString fetch(AsciiString, Bool *exists = nullptr)
	{
		if (exists)
			*exists = FALSE;
		return UnicodeString();
	}
	virtual UnicodeString fetchFormat(const Char *, ...)
	{
		return UnicodeString();
	}
	virtual UnicodeString fetchOrSubstitute(const Char *, const WideChar *)
	{
		return UnicodeString();
	}
	virtual UnicodeString fetchOrSubstituteFormat(const Char *, const WideChar *, ...)
	{
		return UnicodeString();
	}
	virtual UnicodeString fetchOrSubstituteFormatVA(const Char *, const WideChar *, va_list)
	{
		return UnicodeString();
	}
	virtual AsciiStringVec& getStringsWithLabelPrefix(AsciiString)
	{
		return m_strings;
	}
	virtual void initMapStringFile(const AsciiString&) {}

private:
	AsciiStringVec m_strings;
};

#if defined(_WIN64)
class TerminalReceiptTestEnvironment
{
public:
	bool set(const char *name, const char *value)
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
		return SetEnvironmentVariableA(name, value) != 0;
	}
	~TerminalReceiptTestEnvironment()
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

static void TestPerformanceReceiptTerminalAdmissions()
{
	using namespace rts::performance;
	// Exercise the real runtime owner hook without game initialization or any
	// publication. These process-local fixture values are always restored.
	TerminalReceiptTestEnvironment environment;
	const char *values[][2] = {
		{ "RTS_PERFORMANCE_ROLE", "performance-report" },
		{ "RTS_PERFORMANCE_RUN_ID", "terminal-owner-test-no-publication" },
		{ "RTS_PERFORMANCE_RUN_NONCE", "11111111-1111-4111-8111-111111111111" },
		{ "RTS_PERFORMANCE_COHORT_NONCE", "22222222-2222-4222-8222-222222222222" },
		{ "RTS_PERFORMANCE_COHORT_CREATED_UTC", "2026-01-01T00:00:00Z" },
		{ "RTS_PERFORMANCE_RECEIPT_DIR", "terminal-owner-test-no-publication" },
		{ "RTS_PERFORMANCE_SOURCE_COMMIT", "0123456789abcdef0123456789abcdef01234567" },
		{ "RTS_PERFORMANCE_ARTIFACT_SET_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		{ "RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
		{ "RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
		{ "RTS_PERFORMANCE_FIXTURE_ID", "terminal-owner-fixture" },
		{ "RTS_PERFORMANCE_RAW_LOG_PATH", "terminal-owner-test-no-publication/raw.log" },
		{ "RTS_PERFORMANCE_TIMING_PATH", "terminal-owner-test-no-publication/timing.csv" },
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
		CHECK(environment.set(values[index][0], values[index][1]));

	for (unsigned scenario = 0; scenario != 4; ++scenario)
	{
		PerformanceReceiptRuntime runtime;
		const bool begun = runtime.begin("fresh-ai-map", "");
		CHECK(begun);
		if (!begun) return;
		KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
		const KernelPerformanceBatch retained =
			ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 2, 1);
		CHECK(retained.valid());
		for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		{
			const KernelPerformanceInterval interval = ledger.beginInterval(retained,
				static_cast<KernelPerformanceStage>(stage));
			CHECK(interval.valid() && ledger.endInterval(interval));
		}
		// Missing CRC, unclean termination and rejected frame zero must not
		// seal admission. Only the fourth scenario is an accepted terminal.
		runtime.captureTerminalResult(scenario == 2 ? 0 : 2, 0x89ABCDEFU,
			scenario != 0, scenario != 1);
		const bool accepted = scenario == 3;
		KernelPerformanceBatchIdentity identity;
		CHECK(ledger.describeBatch(retained, identity) && identity.frame == 2);
		const KernelPerformanceBatch next =
			ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 3, 2);
		CHECK(next.valid() != accepted);
		if (next.valid()) CHECK(ledger.endBatch(next, KERNEL_PERFORMANCE_NOT_ADMITTED));
		if (accepted)
		{
			// A repeated terminal callback must not reopen or replace the run.
			runtime.captureTerminalResult(3, 0x11111111U, true, true);
			const KernelPerformanceBatch repeated =
				ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 4, 3);
			CHECK(!repeated.valid());
			if (repeated.valid()) CHECK(ledger.endBatch(repeated, KERNEL_PERFORMANCE_NOT_ADMITTED));
		}
		CHECK(ledger.endBatch(retained, KERNEL_PERFORMANCE_COMMITTED));
		const KernelPerformanceSnapshot timing = ledger.freeze();
		CHECK(timing.complete && timing.errors == 0 && timing.streamCount == 1);
		CHECK(timing.streams[0].attemptedBatches == (accepted ? 1U : 2U));
		CHECK(timing.streams[0].committedBatches == 1);
		// No Runtime::finish call: no raw file or receipt is fabricated. The
		// pure lifecycle cases below independently cover completed-frame/drain.
		KernelPerformanceReferenceLedger::instance().freeze();
	}
}

static void TestPerformanceReceiptOwnerLifecycle()
{
	PerformanceReceiptOwnerLifecycle idle;
	CHECK(!idle.begun() && !idle.finalized() && !idle.terminalResultKnown());
	CHECK(!idle.observeCompletedFrame(1));
	CHECK(!idle.captureTerminalResult(1, 0x12345678U));
	CHECK(!idle.finish(0, 0));

	PerformanceReceiptOwnerLifecycle complete;
	CHECK(complete.begin());
	CHECK(complete.begun() && !complete.begin());
	CHECK(!complete.observeCompletedFrame(0));
	CHECK(complete.observeCompletedFrame(1));
	CHECK(!complete.observeCompletedFrame(1));
	CHECK(complete.observeCompletedFrame(2));
	// The fresh-match victory event is frame 2; its real owner CRC was
	// computed at frame 3 before that completed-frame callback runs.
	const unsigned victoryFrame = 2;
	const unsigned actualCrcFrame = victoryFrame + 1;
	CHECK(complete.captureTerminalResult(actualCrcFrame, 0x89ABCDEFU));
	CHECK(complete.terminalResultKnown());
	CHECK(complete.terminalFrame() == actualCrcFrame);
	CHECK(complete.terminalFrame() != victoryFrame);
	CHECK(complete.terminalCrc() == 0x89ABCDEFU);
	CHECK(!complete.captureTerminalResult(4, 0x11111111U));
	CHECK(complete.observeCompletedFrame(actualCrcFrame));
	CHECK(!complete.observeCompletedFrame(0));
	CHECK(!complete.observeCompletedFrame(1));
	CHECK(!complete.observeCompletedFrame(4));
	CHECK(complete.lastCompletedFrame() == actualCrcFrame);
	CHECK(complete.finish(0, 0));
	CHECK(complete.finalized());
	CHECK(!complete.finish(0, 0));
	CHECK(!complete.begin());
	CHECK(!complete.observeCompletedFrame(4));
	CHECK(complete.terminalFrame() == actualCrcFrame &&
		complete.terminalCrc() == 0x89ABCDEFU);

	PerformanceReceiptOwnerLifecycle missingCompletedTerminal;
	CHECK(missingCompletedTerminal.begin());
	CHECK(missingCompletedTerminal.observeCompletedFrame(1));
	CHECK(missingCompletedTerminal.captureTerminalResult(2, 7));
	CHECK(!missingCompletedTerminal.finish(0, 0));
	CHECK(missingCompletedTerminal.finalized());
	CHECK(!missingCompletedTerminal.observeCompletedFrame(2));
	CHECK(!missingCompletedTerminal.finish(0, 0));

	PerformanceReceiptOwnerLifecycle missingTerminal;
	CHECK(missingTerminal.begin());
	CHECK(missingTerminal.observeCompletedFrame(1));
	CHECK(!missingTerminal.finish(0, 0));
	CHECK(missingTerminal.finalized());

	PerformanceReceiptOwnerLifecycle workerStillActive;
	CHECK(workerStillActive.begin());
	CHECK(workerStillActive.observeCompletedFrame(1));
	CHECK(workerStillActive.captureTerminalResult(1, 7));
	CHECK(!workerStillActive.finish(1, 0));
	CHECK(workerStillActive.finalized());
	CHECK(!workerStillActive.finish(0, 0));

	PerformanceReceiptOwnerLifecycle ownerCallbackPending;
	CHECK(ownerCallbackPending.begin());
	CHECK(ownerCallbackPending.observeCompletedFrame(1));
	CHECK(ownerCallbackPending.captureTerminalResult(1, 7));
	CHECK(!ownerCallbackPending.finish(0, 1));
	CHECK(ownerCallbackPending.finalized());
	CHECK(!ownerCallbackPending.finish(0, 0));

	PerformanceReceiptOwnerLifecycle incompleteRange;
	CHECK(incompleteRange.begin());
	CHECK(incompleteRange.observeCompletedFrame(1));
	CHECK(incompleteRange.observeCompletedFrame(3));
	CHECK(!incompleteRange.captureTerminalResult(0, 7));
	CHECK(!incompleteRange.captureTerminalResult(2, 7));
	CHECK(incompleteRange.captureTerminalResult(3, 7));
	CHECK(!incompleteRange.finish(0, 0));
	CHECK(incompleteRange.finalized());
	TestPerformanceReceiptTerminalAdmissions();
}
#endif

static void TestSkirmishAIReplayEpoch()
{
	CHECK(GetSkirmishAIReplayEpoch(L"retail build time") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(GetSkirmishAIReplayEpoch(
		L"retail build time [GeneralsAIPlanningEpoch=1]") ==
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayEpoch(
		L"retail [GeneralsAIPlanningEpoch=2]") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(GetSkirmishAIReplayEpoch(
		L"retail [GeneralsAIPlanningEpoch=1] trailing") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(GetSkirmishAIReplayEpoch(
		L"retail [GeneralsAIPlanningEpoch=1] [GeneralsAIPlanningEpoch=1]") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(!ShouldUseSkirmishAIDeterministicPlanning(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_LEGACY));
	CHECK(ShouldUseSkirmishAIDeterministicPlanning(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	CHECK(ShouldUseSkirmishAIDeterministicPlanning(
		FALSE, SKIRMISH_AI_REPLAY_EPOCH_LEGACY));

	UnicodeString marked = L"native recording";
	MarkReplayVersionForSkirmishAICurrentEpoch(marked);
	CHECK(GetSkirmishAIReplayEpoch(marked) ==
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayRecordingEpoch() ==
		GetSkirmishAIReplayEpoch(marked));
	MarkReplayVersionForSkirmishAICurrentEpoch(marked);
	CHECK(CountSkirmishAIReplayMarkers(marked.str(),
		GetSkirmishAIReplayMarkerPrefix()) == 1);
}

static void TestGeneralsPathfindingReplayEpochWriter()
{
	UnicodeString version = L"native build";
	MarkReplayVersionForGeneralsPathfindingCurrentEpoch(version);
	CHECK(version == L"native build [GeneralsPathfindingEpoch=1]");
	MarkReplayVersionForGeneralsPathfindingCurrentEpoch(version);
	CHECK(version == L"native build [GeneralsPathfindingEpoch=1]");
	// The path marker precedes the existing terminal AI marker on the wire.
	MarkReplayVersionForSkirmishAICurrentEpoch(version);
	CHECK(version == L"native build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]");
	CHECK(GetGeneralsPathfindingReplayEpoch(version) == 1);
	CHECK(GetSkirmishAIReplayEpoch(version) == 1);
	MarkReplayVersionForGeneralsPathfindingCurrentEpoch(version);
	MarkReplayVersionForSkirmishAICurrentEpoch(version);
	CHECK(version == L"native build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]");

	// Never retrofit an AI-only header in the wrong order or repair a future,
	// duplicate, or malformed family into an apparently current recording.
	const WideChar *unchanged[] =
	{
		L"native build [GeneralsAIPlanningEpoch=1]",
		L"native build [GeneralsAIPlanningEpoch=2]",
		L"native build [GeneralsAIPlanningEpoch]",
		L"native build [GeneralsAIPlanningEpoch=bogus]",
		L"native build [GeneralsPathfindingEpoch=0]",
		L"native build [GeneralsPathfindingEpoch=2]",
		L"native build [GeneralsPathfindingEpoch]",
		L"native build [GeneralsPathfindingEpoch=bogus]",
		L"native build [GeneralsPathfindingEpoch=1] trailing",
		L"native build [GeneralsPathfindingEpoch=1] [GeneralsPathfindingEpoch=1]",
		L"native build [GeneralsPathfindingEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"native build [GeneralsAIPlanningEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]"
	};
	for (unsigned i = 0; i < sizeof(unchanged) / sizeof(unchanged[0]); ++i)
	{
		UnicodeString rejected = unchanged[i];
		MarkReplayVersionForGeneralsPathfindingCurrentEpoch(rejected);
		CHECK(rejected == unchanged[i]);
		CHECK(GetGeneralsPathfindingReplayEpoch(rejected) == 0);
	}
	CHECK(GetSkirmishAIReplayEpoch(unchanged[0]) == 1);
}

#if defined(_WIN64)
namespace GeneralsPathfindingRecorderEpochTest
{
// Only external I/O and surrounding object state are supplied here. Each
// included fragment is extracted from the actual Recorder.cpp by CMake.
struct VersionSource
{
	UnicodeString getUnicodeVersion() const { return L"native version"; }
	UnicodeString getUnicodeBuildTime() const { return L"native build"; }
};
static VersionSource s_version;
static VersionSource *TheVersion = &s_version;
static void *TheNetwork = NULL;

struct LaunchStream
{
	std::uint32_t words[4];
	unsigned available;
	unsigned consumed;
};

static Bool readNativeReplayU32Field(LaunchStream *stream, std::uint32_t *value)
{
	if (!stream || stream->consumed >= stream->available)
		return FALSE;
	*value = stream->words[stream->consumed++];
	return TRUE;
}

struct GameInfoState
{
	unsigned endCount;
	unsigned resetCount;
	GameInfoState() : endCount(0), resetCount(0) {}
	void endGame() { ++endCount; }
	void reset() { ++resetCount; }
};

struct Header
{
	UnicodeString versionTimeString;
};

struct RecorderEpochState
{
	Int m_skirmishAIReplayEpoch;
	Int m_generalsPathfindingReplayEpoch;
	Int m_originalGameMode;
	Bool m_replayReadError;
	Bool m_nativeReplayContainer;
	std::uint32_t m_nativeReplayRecordBytes;
	LaunchStream stream;
	LaunchStream *m_file;
	GameInfoState m_gameInfo;

	RecorderEpochState() : m_skirmishAIReplayEpoch(0),
		m_generalsPathfindingReplayEpoch(0), m_originalGameMode(GAME_NONE),
		m_replayReadError(FALSE), m_nativeReplayContainer(FALSE),
		m_nativeReplayRecordBytes(0), m_file(&stream) {}

	Int getGameMode() const { return m_originalGameMode; }

	void resetEpochs()
	{
#include "GeneralsRecorderInitEpochUnderTest.inc"
	}

	UnicodeString recordEpoch(Int originalGameMode)
	{
		resetEpochs();
		// startRecording resets the member mode before writing the header; only
		// its originalGameMode argument still describes this new recording.
		m_originalGameMode = GAME_NONE;
#include "GeneralsRecorderRecordingEpochUnderTest.inc"
		return versionTimeString;
	}

	Bool failReplayHeaderRead()
	{
		m_file = NULL;
		return FALSE;
	}

	Bool playbackEpoch(const WideChar *version, Int originalMode,
		unsigned availableWords, Bool headerValid)
	{
		stream.words[0] = 2U;
		stream.words[1] = static_cast<std::uint32_t>(originalMode);
		stream.words[2] = 0U;
		stream.words[3] = 30U;
		stream.available = availableWords;
		stream.consumed = 0;
		m_file = &stream;
#include "GeneralsRecorderPlaybackResetUnderTest.inc"
		if (!headerValid)
			return FALSE;
		Header header;
		header.versionTimeString = version;
#include "GeneralsRecorderPlaybackHeaderEpochUnderTest.inc"
		// The decoded original mode is not available yet. Merely parsing a
		// current-looking string must not enable local path semantics here.
		CHECK(m_generalsPathfindingReplayEpoch == 0);
#include "GeneralsRecorderPlaybackLaunchEpochUnderTest.inc"
		return TRUE;
	}
};

static void TestLifecycle()
{
	const Bool savedRuntimeEpoch = IsGeneralsAICanonicalRuntimeEpoch();
	SetGeneralsAICanonicalRuntimeEpoch(TRUE);
	RecorderEpochState recorder;
	recorder.m_skirmishAIReplayEpoch = 1;
	recorder.m_generalsPathfindingReplayEpoch = 1;
	recorder.resetEpochs();
	CHECK(recorder.m_skirmishAIReplayEpoch == 0);
	CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	const WideChar *current =
		L"native build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]";
	const Int localModes[] = { GAME_SINGLE_PLAYER, GAME_SKIRMISH };
	for (unsigned i = 0; i < sizeof(localModes) / sizeof(localModes[0]); ++i)
	{
		TheNetwork = NULL;
		const UnicodeString recording = recorder.recordEpoch(localModes[i]);
		CHECK(recording == current);
		CHECK(recorder.m_originalGameMode == GAME_NONE);
		CHECK(recorder.m_skirmishAIReplayEpoch == 1);
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 1);
		CHECK(HasCurrentGeneralsPathfindingReplayEpoch(
			recorder.m_generalsPathfindingReplayEpoch, recorder.m_skirmishAIReplayEpoch));
		TheNetwork = &recorder;
		CHECK(recorder.recordEpoch(localModes[i]) ==
			L"native build [GeneralsAIPlanningEpoch=1]");
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
		TheNetwork = NULL;
		SetGeneralsAICanonicalRuntimeEpoch(FALSE);
		CHECK(recorder.recordEpoch(localModes[i]) == L"native build");
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
		CHECK(recorder.m_skirmishAIReplayEpoch == 0);
		SetGeneralsAICanonicalRuntimeEpoch(TRUE);
		CHECK(recorder.playbackEpoch(current, localModes[i], 4, TRUE));
		CHECK(recorder.m_originalGameMode == localModes[i]);
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 1);
		CHECK(recorder.m_skirmishAIReplayEpoch == 1);
		CHECK(recorder.stream.consumed == 4);
		CHECK(recorder.playbackEpoch(L"native build [GeneralsAIPlanningEpoch=1]",
			localModes[i], 4, TRUE));
		CHECK(recorder.m_skirmishAIReplayEpoch == 1);
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	}
	const Int networkModes[] = { GAME_LAN, GAME_INTERNET };
	for (unsigned i = 0; i < sizeof(networkModes) / sizeof(networkModes[0]); ++i)
	{
		CHECK(recorder.recordEpoch(networkModes[i]) ==
			L"native build [GeneralsAIPlanningEpoch=1]");
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
		CHECK(recorder.playbackEpoch(current, networkModes[i], 4, TRUE));
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	}
	const Int otherModes[] = { GAME_REPLAY, GAME_SHELL, GAME_NONE, -1, 999 };
	for (unsigned i = 0; i < sizeof(otherModes) / sizeof(otherModes[0]); ++i)
	{
		CHECK(recorder.recordEpoch(otherModes[i]) == L"native build");
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
		CHECK(recorder.playbackEpoch(current, otherModes[i], 4, TRUE));
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	}
	const WideChar *legacy[] =
	{
		L"native build",
		L"native build [GeneralsPathfindingEpoch=2] [GeneralsAIPlanningEpoch=1]",
		L"native build [GeneralsPathfindingEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"native build [GeneralsAIPlanningEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]"
	};
	for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i)
	{
		recorder.m_generalsPathfindingReplayEpoch = 1;
		CHECK(recorder.playbackEpoch(legacy[i], GAME_SKIRMISH, 4, TRUE));
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	}
	for (unsigned available = 0; available < 4; ++available)
	{
		recorder.m_generalsPathfindingReplayEpoch = 1;
		const unsigned ends = recorder.m_gameInfo.endCount;
		const unsigned resets = recorder.m_gameInfo.resetCount;
		CHECK(!recorder.playbackEpoch(current, GAME_SKIRMISH, available, TRUE));
		CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
		CHECK(recorder.stream.consumed == available);
		CHECK(recorder.m_gameInfo.endCount == ends + 1);
		CHECK(recorder.m_gameInfo.resetCount == resets + 1);
		CHECK(recorder.m_file == NULL);
	}
	recorder.m_generalsPathfindingReplayEpoch = 1;
	recorder.m_skirmishAIReplayEpoch = 1;
	CHECK(!recorder.playbackEpoch(current, GAME_SKIRMISH, 4, FALSE));
	CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	CHECK(recorder.m_skirmishAIReplayEpoch == 0);
	CHECK(recorder.playbackEpoch(current, GAME_SKIRMISH, 4, TRUE));
	CHECK(recorder.m_generalsPathfindingReplayEpoch == 1);
	recorder.resetEpochs();
	CHECK(recorder.m_generalsPathfindingReplayEpoch == 0);
	SetGeneralsAICanonicalRuntimeEpoch(savedRuntimeEpoch);
}
} // namespace GeneralsPathfindingRecorderEpochTest
#endif

static void TestTextureLoadQueuePublication()
{
	SynchronizedTextureLoadTaskListClass queue;
	TextureLoadTaskClass lowPending;
	TextureLoadTaskClass readyFirst;
	TextureLoadTaskClass readySecond;
	TextureLoadTaskClass highPending;

	queue.Push_Back(&lowPending);
	CHECK(readyFirst.Begin_Async_Prepare());
	readyFirst.Set_State(TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	queue.Publish_Completed(&readyFirst);
	CHECK(readySecond.Begin_Async_Prepare());
	readySecond.Set_State(TextureLoadTaskClass::STATE_LOAD_COMPLETE);
	queue.Publish_Failed(&readySecond);
	highPending.Set_Priority(TextureLoadTaskClass::PRIORITY_HIGH);
	queue.Push_Front(&highPending);

	CHECK(readyFirst.Is_Async_Prepare_Complete());
	CHECK(readySecond.Is_Async_Prepare_Complete());
	CHECK(readySecond.Get_State() == TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	CHECK(queue.Pop_Front() == &highPending);
	CHECK(queue.Pop_Front() == &readyFirst);
	CHECK(queue.Pop_Front() == &readySecond);
	CHECK(queue.Pop_Front() == &lowPending);
	CHECK(queue.Is_Empty());

	TextureLoadTaskClass promotedReady;
	TextureLoadTaskClass pendingAfterPromotion;
	queue.Push_Back(&pendingAfterPromotion);
	CHECK(promotedReady.Begin_Async_Prepare());
	promotedReady.Set_State(TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	queue.Publish_Completed(&promotedReady);
	CHECK(queue.Promote_Prepare_Job(&promotedReady));
	CHECK(promotedReady.Get_Priority() == TextureLoadTaskClass::PRIORITY_HIGH);
	CHECK(queue.Pop_Front() == &promotedReady);
	CHECK(queue.Pop_Front() == &pendingAfterPromotion);

	TextureLoadTaskClass promotedBeforePublication;
	TextureLoadTaskClass lowReady;
	CHECK(lowReady.Begin_Async_Prepare());
	lowReady.Set_State(TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	queue.Publish_Completed(&lowReady);
	promotedBeforePublication.Set_Priority(TextureLoadTaskClass::PRIORITY_HIGH);
	CHECK(promotedBeforePublication.Begin_Async_Prepare());
	promotedBeforePublication.Set_State(TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	queue.Publish_Completed(&promotedBeforePublication);
	CHECK(queue.Pop_Front() == &promotedBeforePublication);
	CHECK(queue.Pop_Front() == &lowReady);

	TextureLoadTaskClass removableReady;
	CHECK(removableReady.Begin_Async_Prepare());
	removableReady.Set_State(TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	queue.Publish_Completed(&removableReady);
	queue.Remove(&removableReady);
	CHECK(queue.Is_Empty());

	TextureLoadTaskClass backReady;
	CHECK(backReady.Begin_Async_Prepare());
	backReady.Set_State(TextureLoadTaskClass::STATE_LOAD_MIPMAP);
	queue.Publish_Completed(&backReady);
	CHECK(queue.Pop_Back() == &backReady);
	CHECK(queue.Is_Empty());
}

#if defined(_WIN64)
// Only the external environment is substituted here. AudioEventRTS, GameAudio,
// NullAudioManager and XAudio2AudioManager are the linked production sources.
// Deny all file access, including localized asset probes, without using an
// installed game, a shared temporary directory, or user-profile files.
class LogicalAudioLocalFileSystem final : public LocalFileSystem
{
public:
	void init() override {}
	void reset() override {}
	void update() override {}
	File *openFile(const Char *, Int, size_t) override { CHECK(FALSE); return nullptr; }
	Bool doesFileExist(const Char *) const override { return FALSE; }
	void getFileListInDirectory(const AsciiString &, const AsciiString &,
		const AsciiString &, FilenameList &, Bool) const override { CHECK(FALSE); }
	Bool getFileInfo(const AsciiString &, FileInfo *) const override { return FALSE; }
	Bool createDirectory(AsciiString) override { CHECK(FALSE); return FALSE; }
	AsciiString normalizePath(const AsciiString &path) const override { return path; }
};

class LogicalAudioArchiveFileSystem final : public ArchiveFileSystem
{
public:
	void init() override {}
	void reset() override {}
	void update() override {}
	void postProcessLoad() override {}
	ArchiveFile *openArchiveFile(const Char *) override { CHECK(FALSE); return nullptr; }
	void closeArchiveFile(const Char *) override {}
	void closeAllArchiveFiles() override {}
	File *openFile(const Char *, Int, FileInstance) override { CHECK(FALSE); return nullptr; }
	void closeAllFiles() override {}
	Bool doesFileExist(const Char *, FileInstance) const override { return FALSE; }
	Bool loadBigFilesFromDirectory(AsciiString, AsciiString, Bool) override
	{
		CHECK(FALSE);
		return FALSE;
	}
};

class LogicalAudioEnvironment
{
public:
	LogicalAudioEnvironment() :
		m_previousAudio(TheAudio), m_previousFileSystem(TheFileSystem),
		m_previousLocal(TheLocalFileSystem), m_previousArchive(TheArchiveFileSystem)
	{
		TheFileSystem = &m_fileSystem;
		TheLocalFileSystem = &m_local;
		TheArchiveFileSystem = &m_archive;
	}

	~LogicalAudioEnvironment()
	{
		TheAudio = m_previousAudio;
		TheFileSystem = m_previousFileSystem;
		TheLocalFileSystem = m_previousLocal;
		TheArchiveFileSystem = m_previousArchive;
	}

private:
	FileSystem m_fileSystem;
	LogicalAudioLocalFileSystem m_local;
	LogicalAudioArchiveFileSystem m_archive;
	AudioManager *m_previousAudio;
	FileSystem *m_previousFileSystem;
	LocalFileSystem *m_previousLocal;
	ArchiveFileSystem *m_previousArchive;
};

class LogicalAudioEngineBackend final : public IXAudio2AudioEngineBackend
{
public:
	HRESULT open(CriticalErrorCallback, void *) noexcept override { return S_OK; }
	HRESULT start() noexcept override { return S_OK; }
	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &) noexcept override
	{
		// Admission never services a request or creates a physical audio voice.
		CHECK(FALSE);
		return E_FAIL;
	}
	HRESULT stop() noexcept override { return S_OK; }
	HRESULT close() noexcept override { return S_OK; }
};

static AudioEventInfo *ConfigureLogicalAudioFixture(AudioManager &manager, Real minimumVolume,
	AudioType soundType)
{
	TheAudio = &manager;
	manager.AudioManager::reset();
	AudioSettings *settings = manager.friend_getAudioSettings();
	settings->m_audioRoot = "native-logical-audio-fixture";
	settings->m_soundsFolder = "sounds";
	settings->m_soundsExtension = "wav";
	settings->m_minVolume = minimumVolume;

	AudioEventInfo *info = manager.newAudioEventInfo("logical-audio-seed");
	info->m_audioName = "logical-audio-seed";
	info->m_soundType = soundType;
	if (soundType == AT_Music || soundType == AT_Streaming)
	{
		info->m_filename = "logical-audio-track";
	}
	info->m_type = ST_WORLD;
	info->m_control = AC_RANDOM;
	info->m_priority = AP_NORMAL;
	info->m_volume = 1.0f;
	info->m_minVolume = 0.0f;
	info->m_volumeShift = -0.25f;
	info->m_pitchShiftMin = 0.9f;
	info->m_pitchShiftMax = 1.1f;
	info->m_delayMin = 0;
	info->m_delayMax = 10;
	info->m_limit = 0;
	info->m_loopCount = 1;
	info->m_lowPassFreq = 1.0f;
	info->m_minDistance = 0.0f;
	info->m_maxDistance = 100.0f;
	info->m_sounds.push_back("main-a");
	info->m_sounds.push_back("main-b");
	info->m_sounds.push_back("main-c");
	info->m_attackSounds.push_back("attack-a");
	info->m_attackSounds.push_back("attack-b");
	info->m_decaySounds.push_back("decay-a");
	info->m_decaySounds.push_back("decay-b");
	return info;
}

static void ConfigureLogicalAudioEvent(AudioEventRTS &event,
	const AudioEventInfo *info, Bool logical)
{
	event.setAudioEventInfo(info);
	event.setIsLogicalAudio(logical);
	// Player filtering is a separate contract. This avoids requiring a game
	// world, but does not bypass native range, capacity or event-volume culling.
	event.setUninterruptible(TRUE);
	event.setNextPlayPortion(PP_Sound);
}

enum LogicalAudioSettingCase
{
	LogicalAudioSetting_Enabled,
	LogicalAudioSetting_SoundOff,
	LogicalAudioSetting_Sound3DOff,
	LogicalAudioSetting_MusicOff,
	LogicalAudioSetting_SpeechOff
};

static AudioAffect GetLogicalAudioDisabledAffect(LogicalAudioSettingCase setting)
{
	switch (setting)
	{
		case LogicalAudioSetting_SoundOff: return AudioAffect_Sound;
		case LogicalAudioSetting_Sound3DOff: return AudioAffect_Sound3D;
		case LogicalAudioSetting_MusicOff: return AudioAffect_Music;
		case LogicalAudioSetting_SpeechOff: return AudioAffect_Speech;
		default: return static_cast<AudioAffect>(0);
	}
}

static AudioType GetLogicalAudioSettingType(LogicalAudioSettingCase setting)
{
	return setting == LogicalAudioSetting_MusicOff ? AT_Music
		: setting == LogicalAudioSetting_SpeechOff ? AT_Streaming : AT_SoundEffect;
}

static Bool IsLogicalAudioSettingPositional(LogicalAudioSettingCase setting)
{
	return setting == LogicalAudioSetting_Enabled
		|| setting == LogicalAudioSetting_Sound3DOff;
}

static UnsignedInt NullLogicalAudioSeed(UnsignedInt seed, Bool logical, Int &playingIndex,
	LogicalAudioSettingCase setting, Bool settingEnabled)
{
	NullAudioManager manager;
	// The real common/Null path culls this AFTER filename/play-info generation.
	// This provides a device-free RNG oracle without initializing SoundManager
	// or changing any event methods, even when native rejects before queueing.
	const AudioType soundType = GetLogicalAudioSettingType(setting);
	AudioEventInfo *info = ConfigureLogicalAudioFixture(manager, 2.0f, soundType);
	Coord3D nearPosition = { 1.0f, 0.0f, 0.0f };
	AudioEventRTS event(info->m_audioName);
	if (IsLogicalAudioSettingPositional(setting))
	{
		event.setPosition(&nearPosition);
	}
	ConfigureLogicalAudioEvent(event, info, logical);
	const AudioAffect disabledAffect = GetLogicalAudioDisabledAffect(setting);
	if (disabledAffect != static_cast<AudioAffect>(0))
	{
		manager.setOn(settingEnabled, disabledAffect);
	}
	InitRandom(seed);
	const AudioHandle result = manager.addAudioEvent(&event);
	CHECK(result == (settingEnabled ? AHSV_Muted : AHSV_NoSound));
	playingIndex = event.getPlayingAudioIndex();
	if (soundType == AT_SoundEffect
		&& (settingEnabled || RETAIL_COMPATIBLE_CRC))
	{
		CHECK(playingIndex >= 0 && playingIndex < 3);
	}
	else
	{
		CHECK(playingIndex == -1);
	}
	return GetGameLogicRandomSeedCRC();
}

enum LogicalAudioAdmissionCase
{
	LogicalAudio_Near,
	LogicalAudio_Far,
	LogicalAudio_Capacity,
	LogicalAudio_Muted,
	LogicalAudio_Closed
};

static void CheckNativeLogicalAudioSeed(UnsignedInt seed, Bool logical,
	LogicalAudioAdmissionCase admission, UnsignedInt expectedCRC, Int expectedIndex)
{
	XAudio2AudioService service(std::make_unique<LogicalAudioEngineBackend>());
	AudioAssetCatalog assets;
	XAudio2AudioManager manager(&service, &assets);
	AudioEventInfo *info = ConfigureLogicalAudioFixture(manager, 0.01f, AT_SoundEffect);
	manager.setChannelLimitsForTest(1, 1, 1);
	manager.openDevice();
	CHECK(manager.isOpen());
	Coord3D position = { 1.0f, 0.0f, 0.0f };
	if (admission == LogicalAudio_Far)
	{
		position.x = 200.0f;
	}
	if (admission == LogicalAudio_Capacity)
	{
		AudioEventRTS occupyingEvent(info->m_audioName, &position);
		ConfigureLogicalAudioEvent(occupyingEvent, info, FALSE);
		CHECK(manager.addAudioEvent(&occupyingEvent) >= AHSV_FirstHandle);
		CHECK(manager.getPendingAudioRequestCount() == 1);
		CHECK(manager.getNumAvailable3DSamples() == 0);
	}
	if (admission == LogicalAudio_Muted)
	{
		manager.setAudioEventVolumeOverride(info->m_audioName, 0.0f);
	}
	if (admission == LogicalAudio_Closed)
	{
		manager.closeDevice();
		CHECK(!manager.isOpen());
	}

	AudioEventRTS event(info->m_audioName, &position);
	ConfigureLogicalAudioEvent(event, info, logical);
	InitRandom(seed); // The capacity occupant must not influence this comparison.
	const AudioHandle result = manager.addAudioEvent(&event);
	const UnsignedInt actualCRC = GetGameLogicRandomSeedCRC();
	if (actualCRC != expectedCRC)
	{
		printf("Logical audio seed mismatch: seed=%u logical=%d admission=%d null=%u native=%u\n",
			seed, logical, admission, expectedCRC, actualCRC);
	}
	CHECK(actualCRC == expectedCRC);
#if RETAIL_COMPATIBLE_CRC
	if (logical)
	{
		CHECK(event.getPlayingAudioIndex() == expectedIndex);
	}
#else
	(void)expectedIndex;
#endif
	if (admission == LogicalAudio_Near)
	{
		CHECK(result >= AHSV_FirstHandle);
	}
	else
	{
		CHECK(result == (admission == LogicalAudio_Far ? AHSV_NotForLocal
			: admission == LogicalAudio_Muted ? AHSV_Muted : AHSV_NoSound));
	}
	CHECK(manager.getPendingAudioRequestCount()
		== (admission == LogicalAudio_Near || admission == LogicalAudio_Capacity ? 1U : 0U));
	CHECK(manager.getActiveAudioCount() == 0);
}

static void CheckNativeLogicalAudioSettingSeed(UnsignedInt seed,
	LogicalAudioSettingCase setting, Bool settingEnabled, UnsignedInt expectedCRC,
	Int expectedIndex)
{
	XAudio2AudioService service(std::make_unique<LogicalAudioEngineBackend>());
	AudioAssetCatalog assets;
	XAudio2AudioManager manager(&service, &assets);
	const AudioType soundType = GetLogicalAudioSettingType(setting);
	AudioEventInfo *info = ConfigureLogicalAudioFixture(manager, 0.01f, soundType);
	manager.setChannelLimitsForTest(1, 1, 1);
	manager.openDevice();
	CHECK(manager.isOpen());
	Coord3D position = { 1.0f, 0.0f, 0.0f };
	AudioEventRTS event(info->m_audioName);
	if (IsLogicalAudioSettingPositional(setting))
	{
		event.setPosition(&position);
	}
	ConfigureLogicalAudioEvent(event, info, TRUE);
	const AudioAffect disabledAffect = GetLogicalAudioDisabledAffect(setting);
	CHECK(disabledAffect != static_cast<AudioAffect>(0));
	manager.setOn(settingEnabled, disabledAffect);
	InitRandom(seed);
	const AudioHandle result = manager.addAudioEvent(&event);
	const UnsignedInt actualCRC = GetGameLogicRandomSeedCRC();
	if (actualCRC != expectedCRC)
	{
		printf("Logical audio setting seed mismatch: seed=%u setting=%d null=%u native=%u\n",
			seed, setting, expectedCRC, actualCRC);
	}
	CHECK(actualCRC == expectedCRC);
#if RETAIL_COMPATIBLE_CRC
	CHECK(event.getPlayingAudioIndex() == expectedIndex);
#else
	(void)expectedIndex;
#endif
	if (settingEnabled)
	{
		CHECK(result >= AHSV_FirstHandle);
		CHECK(manager.getPendingAudioRequestCount() == 1);
	}
	else
	{
		CHECK(result == AHSV_NoSound);
		CHECK(manager.getPendingAudioRequestCount() == 0);
	}
	CHECK(manager.getActiveAudioCount() == 0);
}

static void TestNativeLogicalAudioSeed()
{
	LogicalAudioEnvironment environment;
	const UnsignedInt seeds[] = { 0x01234567U, 0x89abcdefU };
	for (Int seedIndex = 0; seedIndex < 2; ++seedIndex)
	{
		for (Int logical = 0; logical < 2; ++logical)
		{
			InitRandom(seeds[seedIndex]);
			const UnsignedInt initialCRC = GetGameLogicRandomSeedCRC();
			Int expectedIndex = -1;
			const UnsignedInt expectedCRC = NullLogicalAudioSeed(seeds[seedIndex], logical,
				expectedIndex, LogicalAudioSetting_Enabled, TRUE);
#if RETAIL_COMPATIBLE_CRC
			CHECK(logical ? expectedCRC != initialCRC : expectedCRC == initialCRC);
#else
			CHECK(expectedCRC == initialCRC);
#endif
			for (Int admission = LogicalAudio_Near; admission <= LogicalAudio_Closed; ++admission)
			{
				CheckNativeLogicalAudioSeed(seeds[seedIndex], logical,
					static_cast<LogicalAudioAdmissionCase>(admission), expectedCRC, expectedIndex);
			}
			if (logical)
			{
				for (Int setting = LogicalAudioSetting_SoundOff;
					setting <= LogicalAudioSetting_SpeechOff; ++setting)
				{
					const LogicalAudioSettingCase settingCase =
						static_cast<LogicalAudioSettingCase>(setting);
					Int expectedEnabledIndex = -1;
					Int expectedDisabledIndex = -1;
					const UnsignedInt expectedEnabledCRC = NullLogicalAudioSeed(
						seeds[seedIndex], TRUE, expectedEnabledIndex, settingCase, TRUE);
					const UnsignedInt expectedDisabledCRC = NullLogicalAudioSeed(
						seeds[seedIndex], TRUE, expectedDisabledIndex, settingCase, FALSE);
#if RETAIL_COMPATIBLE_CRC
					CHECK(expectedEnabledCRC != initialCRC);
					CHECK(expectedDisabledCRC != initialCRC);
#else
					CHECK(expectedEnabledCRC == initialCRC);
					CHECK(expectedDisabledCRC == initialCRC);
#endif
					CHECK(expectedDisabledCRC == expectedEnabledCRC);
					CheckNativeLogicalAudioSettingSeed(seeds[seedIndex], settingCase, TRUE,
						expectedEnabledCRC, expectedEnabledIndex);
					CheckNativeLogicalAudioSettingSeed(seeds[seedIndex], settingCase, FALSE,
						expectedDisabledCRC, expectedDisabledIndex);
				}
			}
		}
	}
}
#endif

#if defined(_WIN64)
struct StableHealCommitProbe
{
	StableHealCommitProbe()
		: count(0), lifecycleInvalidated(FALSE),
		  secondObservedAfterInvalidation(FALSE)
	{
		objects[0] = nullptr;
		objects[1] = nullptr;
	}

	Object *objects[2];
	UnsignedInt count;
	Bool lifecycleInvalidated;
	Bool secondObservedAfterInvalidation;
};

static void RecordStableHealCommit(Object *object, void *context)
{
	StableHealCommitProbe *probe = static_cast<StableHealCommitProbe *>(context);
	if (probe->count >= 2)
		return;
	probe->objects[probe->count++] = object;
	if (probe->count == 1)
	{
		InvalidateLiveImmutableSpatialLifecycle();
		probe->lifecycleInvalidated = TRUE;
	}
	else
	{
		probe->secondObservedAfterInvalidation = probe->lifecycleInvalidated;
	}
}
#endif

#if defined(_WIN32)
struct SkirmishAITestReplayCommitProbe
{
	Int callCount;
	Int sharingFailures;
	Bool persistentSharing;
	DWORD terminalError;
	Bool temporaryMissingDuringFailure;
	const char *substitutionPath;
};

static Bool SkirmishAITestReplayFileExists(const char *path)
{
	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static Bool WriteSkirmishAITestReplayFile(const char *path,
	const char *contents)
{
	if (path == nullptr || contents == nullptr)
		return FALSE;
	FILE *file = fopen(path, "wb");
	if (file == nullptr)
		return FALSE;
	const size_t length = strlen(contents);
	const Bool written = fwrite(contents, 1, length, file) == length;
	const int closeResult = fclose(file);
	return written && closeResult == 0;
}

static Bool SkirmishAITestReplayFileHasContents(const char *path,
	const char *expectedContents)
{
	if (path == nullptr || expectedContents == nullptr)
		return FALSE;
	const size_t expectedLength = strlen(expectedContents);
	char contents[128];
	if (expectedLength >= sizeof(contents))
		return FALSE;
	FILE *file = fopen(path, "rb");
	if (file == nullptr)
		return FALSE;
	const size_t readCount = fread(contents, 1, expectedLength, file);
	const Bool matches = readCount == expectedLength &&
		memcmp(contents, expectedContents, expectedLength) == 0 &&
		fgetc(file) == EOF;
	const int closeResult = fclose(file);
	return matches && closeResult == 0;
}

static Bool CommitSkirmishAITestReplayUsingProbe(const char *temporaryPath,
	const char *destinationPath, void *context)
{
	SkirmishAITestReplayCommitProbe *probe =
		static_cast<SkirmishAITestReplayCommitProbe *>(context);
	if (probe == nullptr)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	++probe->callCount;
	if (probe->persistentSharing ||
		probe->callCount <= probe->sharingFailures)
	{
		if (!SkirmishAITestReplayFileExists(temporaryPath))
			probe->temporaryMissingDuringFailure = TRUE;
		SetLastError(ERROR_SHARING_VIOLATION);
		return FALSE;
	}
	if (probe->terminalError != ERROR_SUCCESS)
	{
		SetLastError(probe->terminalError);
		return FALSE;
	}
	if (!MoveFileExA(temporaryPath, destinationPath, MOVEFILE_WRITE_THROUGH))
		return FALSE;
	if (probe->substitutionPath != nullptr)
	{
		if (!MoveFileExA(destinationPath, probe->substitutionPath, MOVEFILE_WRITE_THROUGH) ||
			!WriteSkirmishAITestReplayFile(destinationPath, "replacement-bytes"))
			return FALSE;
	}
	return TRUE;
}

struct SkirmishAITestReplayFinalCloseProbe
{
	Int callCount;
	Bool fail;
};

static Bool CloseSkirmishAITestReplayUsingProbe(void *nativeHandle, void *context)
{
	SkirmishAITestReplayFinalCloseProbe *probe =
		static_cast<SkirmishAITestReplayFinalCloseProbe *>(context);
	if (probe == nullptr || nativeHandle == nullptr)
		return FALSE;
	++probe->callCount;
	const Bool closed = CloseHandle(static_cast<HANDLE>(nativeHandle)) ? TRUE : FALSE;
	return closed && !probe->fail;
}

static void TestSkirmishAITestReplayRetentionCommitPolicy()
{
	char currentDirectory[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	const DWORD directoryLength = GetCurrentDirectoryA(
		static_cast<DWORD>(sizeof(currentDirectory)), currentDirectory);
	CHECK(directoryLength != 0 && directoryLength < sizeof(currentDirectory));
	if (directoryLength == 0 || directoryLength >= sizeof(currentDirectory))
		return;
	const unsigned long processId =
		static_cast<unsigned long>(GetCurrentProcessId());
	char sourcePath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	char destinationPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	char temporaryPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	char substitutionPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	_snprintf(sourcePath, sizeof(sourcePath),
		"%s\\SkirmishAITestRetentionPolicy-%lu-source.rep",
		currentDirectory, processId);
	_snprintf(destinationPath, sizeof(destinationPath),
		"%s\\SkirmishAITestRetentionPolicy-%lu-retained.rep",
		currentDirectory, processId);
	_snprintf(temporaryPath, sizeof(temporaryPath),
		"%s\\SkirmishAITestRetentionPolicy-%lu-retained.rep.tmp",
		currentDirectory, processId);
	sourcePath[sizeof(sourcePath) - 1] = '\0';
	destinationPath[sizeof(destinationPath) - 1] = '\0';
	temporaryPath[sizeof(temporaryPath) - 1] = '\0';
	_snprintf(substitutionPath, sizeof(substitutionPath),
		"%s\\SkirmishAITestRetentionPolicy-%lu-substituted.rep",
		currentDirectory, processId);
	substitutionPath[sizeof(substitutionPath) - 1] = '\0';

	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	remove(substitutionPath);
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
	strcpy(digest, "unchanged");
	SkirmishAITestReplayCommitProbe sourceFailure =
		{ 0, 0, TRUE, ERROR_SUCCESS, FALSE, nullptr };
	CHECK(!SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&sourceFailure));
	CHECK(sourceFailure.callCount == 0);
	CHECK(strcmp(digest, "unchanged") == 0);

	CHECK(WriteSkirmishAITestReplayFile(sourcePath, "replay-fixture"));
	SkirmishAITestReplayCommitProbe sharingThenSuccess =
		{ 0, 1, FALSE, ERROR_SUCCESS, FALSE, nullptr };
	strcpy(digest, "unchanged");
	CHECK(SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&sharingThenSuccess));
	CHECK(sharingThenSuccess.callCount == 2);
	CHECK(strcmp(digest,
		"8742EBC99881266FF5BADEDD521E1CD24066EAD2E88A9D544C3C1F466AE534DA") == 0);
	CHECK(SkirmishAITestReplayFileHasContents(destinationPath,
		"replay-fixture"));
	CHECK(SkirmishAITestReplayFileExists(sourcePath));

	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	CHECK(WriteSkirmishAITestReplayFile(sourcePath, "replay-fixture"));
	SkirmishAITestReplayCommitProbe persistentSharing =
		{ 0, 0, TRUE, ERROR_SUCCESS, FALSE, nullptr };
	strcpy(digest, "unchanged");
	CHECK(!SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&persistentSharing));
	CHECK(persistentSharing.callCount ==
		8);
	CHECK(!persistentSharing.temporaryMissingDuringFailure);
	CHECK(strcmp(digest, "unchanged") == 0);
	CHECK(SkirmishAITestReplayFileExists(sourcePath));
	CHECK(!SkirmishAITestReplayFileExists(destinationPath));
	CHECK(!SkirmishAITestReplayFileExists(temporaryPath));

	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	CHECK(WriteSkirmishAITestReplayFile(sourcePath, "replay-fixture"));
	SkirmishAITestReplayCommitProbe nonSharingFailure =
		{ 0, 0, FALSE, ERROR_ACCESS_DENIED, FALSE, nullptr };
	strcpy(digest, "unchanged");
	CHECK(!SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&nonSharingFailure));
	CHECK(nonSharingFailure.callCount == 1);
	CHECK(strcmp(digest, "unchanged") == 0);
	CHECK(SkirmishAITestReplayFileExists(sourcePath));
	CHECK(!SkirmishAITestReplayFileExists(destinationPath));
	CHECK(!SkirmishAITestReplayFileExists(temporaryPath));

	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	CHECK(WriteSkirmishAITestReplayFile(sourcePath, "replay-fixture"));
	CHECK(WriteSkirmishAITestReplayFile(destinationPath,
		"destination-sentinel"));
	SkirmishAITestReplayCommitProbe existingDestination =
		{ 0, 0, FALSE, ERROR_SUCCESS, FALSE, nullptr };
	strcpy(digest, "unchanged");
	CHECK(!SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&existingDestination));
	CHECK(existingDestination.callCount == 1);
	CHECK(strcmp(digest, "unchanged") == 0);
	CHECK(SkirmishAITestReplayFileHasContents(destinationPath,
		"destination-sentinel"));
	CHECK(SkirmishAITestReplayFileExists(sourcePath));
	CHECK(!SkirmishAITestReplayFileExists(temporaryPath));

	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	remove(substitutionPath);
	CHECK(WriteSkirmishAITestReplayFile(sourcePath, "replay-fixture"));
	SkirmishAITestReplayCommitProbe substitutedAfterCommit =
		{ 0, 0, FALSE, ERROR_SUCCESS, FALSE, substitutionPath };
	strcpy(digest, "unchanged");
	CHECK(!SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&substitutedAfterCommit));
	CHECK(strcmp(digest, "unchanged") == 0);
	CHECK(SkirmishAITestReplayFileHasContents(destinationPath, "replacement-bytes"));
	CHECK(!SkirmishAITestReplayFileExists(substitutionPath));

	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	CHECK(WriteSkirmishAITestReplayFile(sourcePath, "replay-fixture"));
	SkirmishAITestReplayCommitProbe finalCloseCommit =
		{ 0, 0, FALSE, ERROR_SUCCESS, FALSE, nullptr };
	SkirmishAITestReplayFinalCloseProbe finalClose = { 0, TRUE };
	strcpy(digest, "unchanged");
	CHECK(!SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(sourcePath,
		destinationPath, digest, CommitSkirmishAITestReplayUsingProbe,
		&finalCloseCommit, CloseSkirmishAITestReplayUsingProbe, &finalClose));
	CHECK(finalCloseCommit.callCount == 1 && finalClose.callCount == 1);
	CHECK(strcmp(digest, "unchanged") == 0);
	CHECK(SkirmishAITestReplayFileExists(sourcePath));
	CHECK(!SkirmishAITestReplayFileExists(destinationPath));
	CHECK(!SkirmishAITestReplayFileExists(temporaryPath));
	remove(sourcePath);
	remove(destinationPath);
	remove(temporaryPath);
	remove(substitutionPath);
}
#endif

static void TestSkirmishAITestReceiptContract()
{
	SkirmishAITestPlan practicalPlan;
	BuildSkirmishAITestPlan(1731,
		SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7, &practicalPlan);
	CHECK(IsSkirmishAITestPracticalControllerScenario(
		SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7));
	CHECK(IsValidSkirmishAITestPracticalControllerPlan(practicalPlan));
	CHECK(practicalPlan.slots[0].state == SLOT_PLAYER &&
		practicalPlan.slots[0].playerTemplate != PLAYERTEMPLATE_OBSERVER &&
		practicalPlan.slots[0].isController);
	CHECK(practicalPlan.slots[1].color == 1 &&
		practicalPlan.slots[7].color == 7 &&
		practicalPlan.slots[1].teamNumber == 0 &&
		practicalPlan.slots[7].teamNumber == 1);
	SkirmishAITestPlan invalidPracticalPlan = practicalPlan;
	invalidPracticalPlan.slots[0].playerTemplate = PLAYERTEMPLATE_OBSERVER;
	CHECK(!IsValidSkirmishAITestPracticalControllerPlan(invalidPracticalPlan));

	SkirmishAITestReplayReceipt receipt = { 0 };
	receipt.seed = 1731;
	receipt.winnerTeam = 0;
	receipt.endFrame = 42000;
	receipt.replayEpoch = SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
	strcpy(receipt.scenario, "4v3");
	strcpy(receipt.executableSha256,
		"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF");
	strcpy(receipt.replaySha256,
		"8742EBC99881266FF5BADEDD521E1CD24066EAD2E88A9D544C3C1F466AE534DA");
	strcpy(receipt.runNonce, "00000001-00000002-00000003");
	strcpy(receipt.replayPath, "retained-replay.rep");
	CHECK(IsValidSkirmishAITestReplayReceipt(receipt,
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	receipt.replayEpoch = SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	CHECK(!IsValidSkirmishAITestReplayReceipt(receipt,
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	receipt.replayEpoch = SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
	receipt.runNonce[0] = '!';
	CHECK(!IsValidSkirmishAITestReplayReceipt(receipt,
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	receipt.runNonce[0] = '0';
	strcpy(receipt.scenario, "practical-1v7");
	CHECK(IsValidSkirmishAITestReplayReceipt(receipt,
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT));

	char currentDirectory[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	const DWORD directoryLength = GetCurrentDirectoryA(
		static_cast<DWORD>(sizeof(currentDirectory)), currentDirectory);
	CHECK(directoryLength != 0 && directoryLength < sizeof(currentDirectory));
	if (directoryLength == 0 || directoryLength >= sizeof(currentDirectory))
		return;
	char sourcePath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	char destinationPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	_snprintf(sourcePath, sizeof(sourcePath), "%s\\SkirmishAITestReceipt-%lu.rep",
		currentDirectory, static_cast<unsigned long>(GetCurrentProcessId()));
	_snprintf(destinationPath, sizeof(destinationPath), "%s\\SkirmishAITestReceipt-%lu-retained.rep",
		currentDirectory, static_cast<unsigned long>(GetCurrentProcessId()));
	sourcePath[sizeof(sourcePath) - 1] = '\0';
	destinationPath[sizeof(destinationPath) - 1] = '\0';
	remove(sourcePath);
	remove(destinationPath);
	FILE *source = fopen(sourcePath, "wb");
	CHECK(source != nullptr);
	if (source == nullptr)
		return;
	const char *fixture = "replay-fixture";
	CHECK(fwrite(fixture, 1, strlen(fixture), source) == strlen(fixture));
	CHECK(fclose(source) == 0);
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
	CHECK(RetainSkirmishAITestReplayAtomically(sourcePath, destinationPath, digest));
	CHECK(strcmp(digest,
		"8742EBC99881266FF5BADEDD521E1CD24066EAD2E88A9D544C3C1F466AE534DA") == 0);
	CHECK(!RetainSkirmishAITestReplayAtomically(sourcePath, destinationPath, digest));
	remove(sourcePath);
	remove(destinationPath);
}

static void TestSkirmishAITestHardAI2v6Contract()
{
	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(1733,
		SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6, &plan);
	CHECK(plan.seed == 1733);
	CHECK(!plan.slots[0].isController);

	Int plannedAiCount = 0;
	Int plannedTeamCounts[2] = { 0, 0 };
	Int slotIndex;
	for (slotIndex = 0; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const SkirmishAITestSlotPlan &slot = plan.slots[slotIndex];
		CHECK(slot.state == SLOT_BRUTAL_AI);
		CHECK(slot.playerTemplate == PLAYERTEMPLATE_RANDOM);
		CHECK(slot.color == slotIndex);
		CHECK(slot.startPosition == slotIndex);
		CHECK(slot.teamNumber == (slotIndex < 2 ? 0 : 1));
		CHECK(!slot.isController);
		if (slot.state == SLOT_BRUTAL_AI)
		{
			++plannedAiCount;
			if (slot.teamNumber >= 0 && slot.teamNumber < 2)
				++plannedTeamCounts[slot.teamNumber];
		}
	}
	CHECK(plannedAiCount == 8);
	CHECK(plannedTeamCounts[0] == 2 && plannedTeamCounts[1] == 6);

	// Exercise the actual GameInfo slot semantics: all eight slots are hard AI,
	// there is no human controller, and the local slot remains -1.
	GlobalData *previousGlobalData = TheWritableGlobalData;
	GameTextInterface *previousGameText = TheGameText;
	SkirmishAITestGameText testGameText;
	TheGameText = &testGameText;
	GlobalData *testGlobalData = new GlobalData;
	TheWritableGlobalData = testGlobalData;
	GameInfo *actualGameInfo = new GameInfo;
	GameSlot actualSlots[SKIRMISH_AI_TEST_SLOT_COUNT];
	for (slotIndex = 0; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
		actualGameInfo->setSlotPointer(slotIndex, &actualSlots[slotIndex]);
	actualGameInfo->enterGame();
	actualGameInfo->setLocalIP(0);
	for (slotIndex = 0; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const SkirmishAITestSlotPlan &expected = plan.slots[slotIndex];
		GameSlot *actual = actualGameInfo->getSlot(slotIndex);
		actual->setState(expected.state);
		actual->setPlayerTemplate(expected.playerTemplate);
		actual->setColor(expected.color);
		actual->setStartPos(expected.startPosition);
		actual->setTeamNumber(expected.teamNumber);
	}
	CHECK(actualGameInfo->getNumPlayers() == 8);
	CHECK(actualGameInfo->getNumNonObserverPlayers() == 8);
	CHECK(actualGameInfo->getLocalSlotNum() == -1);

	Int actualAiCount = 0;
	Int actualTeamCounts[2] = { 0, 0 };
	for (slotIndex = 0; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const GameSlot *actual = actualGameInfo->getConstSlot(slotIndex);
		CHECK(actual->isAI() && !actual->isHuman());
		if (actual->isAI())
		{
			++actualAiCount;
			if (actual->getTeamNumber() >= 0 && actual->getTeamNumber() < 2)
				++actualTeamCounts[actual->getTeamNumber()];
		}
	}
	CHECK(actualAiCount == 8);
	CHECK(actualTeamCounts[0] == 2 && actualTeamCounts[1] == 6);
	delete actualGameInfo;
	TheWritableGlobalData = testGlobalData;
	delete testGlobalData;
	TheWritableGlobalData = previousGlobalData;
	TheGameText = previousGameText;
}


#if defined(_WIN64)
int RunHeadlessMetricStartupTitleTests();
int RunPerformanceReceiptOwnerBridgeTitleTests();
int RunPerformanceReceiptProducerTitleTests();
int RunPerformanceReceiptFreshProducerTitleTests();
int RunPerformanceReceiptAttemptTitleTests();
int RunPerformanceReceiptBootstrapTitleTests();
int RunPerformanceReceiptPolicyTitleTests();
int RunPerformanceReceiptModeTitleTests();
int RunPerformanceReceiptWidePathTitleTests();
int RunPerformanceReceiptConsumeWidePathTitleTests();
int RunPerformanceReceiptReplayOwnerShapeTitleTests();
int RunPerformanceReceiptPracticalOwnerShapeTitleTests();
int RunPerformanceReceiptReplayCallerFenceTitleTests();
int RunPerformanceReceiptFreshCallerFenceTitleTests();
int RunPerformanceReceiptTraceFileTests(const wchar_t *freshRoot, bool reparseAliasOnly);
#endif

static void TestRenderedBattleDiagnosticSearch()
{
	// Pure placement only: no terrain, occupancy or route validity is implied.
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 4860.0f, 4690.0f) == 130);
	for (Int candidate = 0; candidate < 49; ++candidate)
	{
		Coord3D center;
		CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 4860.0f, 4690.0f, candidate, &center));
		CHECK(center.x == (0.0f + 4860.0f) * 0.5f + (candidate % 7 - 3) * 240.0f);
		CHECK(center.y == (0.0f + 4690.0f) * 0.5f + (candidate / 7 - 3) * 240.0f);
		CHECK(center.z == 0.0f);
	}
	for (Int grid = 0; grid < 81; ++grid)
	{
		Coord3D center;
		CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 4860.0f, 4690.0f, 49 + grid, &center));
		CHECK(center.x == 447.0f + (grid % 9) * 495.75f);
		CHECK(center.y == 410.0f + (grid / 9) * 483.75f);
		CHECK(center.x - 403.0f - 22.0f - 22.0f >= 0.0f && center.x + 403.0f + 22.0f + 22.0f <= 4860.0f);
		CHECK(center.y - 366.0f - 22.0f - 22.0f >= 0.0f && center.y + 366.0f + 22.0f + 22.0f <= 4690.0f);
		CHECK(center.z == 0.0f);
	}
	Coord3D center = { 1.0f, 2.0f, 3.0f };
	CHECK(GetRenderedBattleDiagnosticSearchCenter(100.0f, -200.0f, 4960.0f, 4490.0f, 49, &center));
	CHECK(center.x == 547.0f && center.y == 210.0f);
	CHECK(GetRenderedBattleDiagnosticSearchCenter(100.0f, -200.0f, 4960.0f, 4490.0f, 129, &center));
	CHECK(center.x == 4513.0f && center.y == 4080.0f);
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 894.0f, 820.0f) == 130);
	for (Int candidate = 49; candidate < 130; ++candidate)
	{
		CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 894.0f, 820.0f, candidate, &center));
		CHECK(center.x == 447.0f && center.y == 410.0f);
	}
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 893.5f, 820.0f) == 49);
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 894.0f, 819.5f) == 49);
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 0.0f, 0.0f) == 49);
	CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 893.5f, 820.0f, 48, &center));
	center.x = 1.0f; center.y = 2.0f; center.z = 3.0f;
	CHECK(!GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 893.5f, 820.0f, 49, &center));
	CHECK(!GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 4860.0f, 4690.0f, -1, &center));
	CHECK(!GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 4860.0f, 4690.0f, 130, &center));
	CHECK(!GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 4860.0f, 4690.0f, 0, nullptr));
	CHECK(GetRenderedBattleDiagnosticSearchCount(1.0f, 0.0f, 0.0f, 4690.0f) == 0);
	CHECK(!GetRenderedBattleDiagnosticSearchCenter(1.0f, 0.0f, 0.0f, 4690.0f, 0, &center));
	const Real invalid[] = { std::numeric_limits<Real>::quiet_NaN(),
		std::numeric_limits<Real>::infinity(), -std::numeric_limits<Real>::infinity() };
	for (Int index = 0; index < 3; ++index)
	{
		CHECK(GetRenderedBattleDiagnosticSearchCount(invalid[index], 0.0f, 4860.0f, 4690.0f) == 0);
		CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, invalid[index], 4860.0f, 4690.0f) == 0);
		CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, invalid[index], 4690.0f) == 0);
		CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 4860.0f, invalid[index]) == 0);
		CHECK(!GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, invalid[index], 4690.0f, 49, &center));
	}
	CHECK(center.x == 1.0f && center.y == 2.0f && center.z == 3.0f);
}

static void TestRenderedBattleDiagnosticLocalPlacement()
{
	// Pure coordinates, ranking and planned geometry only, never map admission.
	CHECK(RENDERED_BATTLE_DIAGNOSTIC_PLACEMENT_SCHEMA_VERSION == 2);
	CHECK(RENDERED_BATTLE_DIAGNOSTIC_LOCAL_TRIAL_COUNT == 9);
	CHECK(RENDERED_BATTLE_DIAGNOSTIC_LOCAL_STEP == 22);
	CHECK(RENDERED_BATTLE_DIAGNOSTIC_LOCAL_ARENA_CAP == 8);
	const Int stencil[9][2] = { {0,0}, {22,0}, {-22,0}, {0,-22}, {0,22},
		{22,-22}, {22,22}, {-22,-22}, {-22,22} };
	for (Int slot = 0; slot < 8; ++slot)
		for (Int trial = 0; trial < 9; ++trial)
		{
			Coord3D local;
			CHECK(GetRenderedBattleDiagnosticLocalOffset(slot, trial, &local));
			CHECK(local.x == (slot < 4 ? -1 : 1) * stencil[trial][0]);
			CHECK(local.y == stencil[trial][1] && local.z == 0.0f);
		}
	Coord3D unchanged = {1.0f, 2.0f, 3.0f};
	CHECK(!GetRenderedBattleDiagnosticLocalOffset(-1, 0, &unchanged));
	CHECK(!GetRenderedBattleDiagnosticLocalOffset(8, 0, &unchanged));
	CHECK(!GetRenderedBattleDiagnosticLocalOffset(0, -1, &unchanged));
	CHECK(!GetRenderedBattleDiagnosticLocalOffset(0, 9, &unchanged));
	CHECK(!GetRenderedBattleDiagnosticLocalOffset(0, 0, nullptr));
	CHECK(unchanged.x == 1.0f && unchanged.y == 2.0f && unchanged.z == 3.0f);
	// Every grid point supports the full fixed formation + all nine offsets
	// and maximum footprint, including the exact-fit map without division by 0.
	for (Int grid = 0; grid < 81; ++grid)
	{
		Coord3D center, exact;
		CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 4860.0f, 4690.0f, 49 + grid, &center));
		CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 894.0f, 820.0f, 49 + grid, &exact));
		for (Int slot = 0; slot < 8; ++slot)
			for (Int unit = 0; unit < 32; ++unit)
				for (Int trial = 0; trial < 9; ++trial)
				{
					Coord3D base, local;
					CHECK(GetRenderedBattleDiagnosticOffset(slot, unit, &base));
					CHECK(GetRenderedBattleDiagnosticLocalOffset(slot, trial, &local));
					const Real x = base.x + local.x, y = base.y + local.y;
					CHECK(center.x + x - 22.0f >= 0.0f && center.x + x + 22.0f <= 4860.0f);
					CHECK(center.y + y - 22.0f >= 0.0f && center.y + y + 22.0f <= 4690.0f);
					CHECK(exact.x + x - 22.0f >= 0.0f && exact.x + x + 22.0f <= 894.0f);
					CHECK(exact.y + y - 22.0f >= 0.0f && exact.y + y + 22.0f <= 820.0f);
				}
	}
	Coord3D first = {0.0f, 0.0f, 0.0f}, second = {18.0f, 0.0f, 7.0f};
	CHECK(AreRenderedBattleDiagnosticPositionsSeparated(first, 10.0f, second, 5.0f));
	CHECK(AreRenderedBattleDiagnosticPositionsSeparated(second, 5.0f, first, 10.0f));
	second.x = 17.999f;
	CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(first, 10.0f, second, 5.0f));
	CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(second, 5.0f, first, 10.0f));
	second.x = 18.001f;
	CHECK(AreRenderedBattleDiagnosticPositionsSeparated(first, 10.0f, second, 5.0f));
	second.x = 9.0f; second.y = 12.0f;
	CHECK(AreRenderedBattleDiagnosticPositionsSeparated(first, 7.0f, second, 5.0f)); // 15 exact diagonal
	CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(first, 7.001f, second, 5.0f));
	CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(first, 1.0f, first, 1.0f));
	// Avoiding one world blocker is insufficient if a local trial overlaps a
	// different planned unit. This contract adds admission, never removes it.
	Coord3D prior = {44.0f, 0.0f, 0.0f}, displaced = {22.0f, 0.0f, 0.0f};
	CHECK(AreRenderedBattleDiagnosticPositionsSeparated(first, 10.0f, prior, 10.0f));
	CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(displaced, 10.0f, prior, 10.0f));
	const Real invalidRadii[] = {0.0f, -1.0f, 21.001f, std::numeric_limits<Real>::quiet_NaN(),
		std::numeric_limits<Real>::infinity(), -std::numeric_limits<Real>::infinity()};
	for (Int index = 0; index < 6; ++index)
	{
		CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(first, invalidRadii[index], prior, 1.0f));
		CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(first, 1.0f, prior, invalidRadii[index]));
	}
	const Real invalidCoords[] = {std::numeric_limits<Real>::quiet_NaN(),
		std::numeric_limits<Real>::infinity(), -std::numeric_limits<Real>::infinity()};
	for (Int index = 0; index < 3; ++index)
		for (Int axis = 0; axis < 3; ++axis)
		{
			Coord3D invalid = prior;
			if (axis == 0) invalid.x = invalidCoords[index];
			if (axis == 1) invalid.y = invalidCoords[index];
			if (axis == 2) invalid.z = invalidCoords[index];
			CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(first, 1.0f, invalid, 1.0f));
			CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(invalid, 1.0f, first, 1.0f));
		}
	Int candidates[9], prefixes[9], count = 0;
	for (Int index = 0; index < 9; ++index) candidates[index] = prefixes[index] = -99;
	for (Int candidate = 9; candidate >= 0; --candidate)
		CHECK(RememberRenderedBattleDiagnosticArena(candidate, 100, candidates, prefixes, &count));
	CHECK(count == 8);
	for (Int index = 0; index < 8; ++index)
		CHECK(candidates[index] == index && prefixes[index] == 100); // earliest ties retained
	CHECK(RememberRenderedBattleDiagnosticArena(129, 255, candidates, prefixes, &count));
	CHECK(candidates[0] == 129 && prefixes[0] == 255 && candidates[7] == 6);
	CHECK(!RememberRenderedBattleDiagnosticArena(128, 99, candidates, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(129, 254, candidates, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(-1, 100, candidates, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(130, 100, candidates, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(10, -1, candidates, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(10, 256, candidates, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(10, 100, nullptr, prefixes, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(10, 100, candidates, nullptr, &count));
	CHECK(!RememberRenderedBattleDiagnosticArena(10, 100, candidates, prefixes, nullptr));
	count = -1;
	CHECK(!RememberRenderedBattleDiagnosticArena(10, 100, candidates, prefixes, &count));
	CHECK(count == -1);
	count = 9;
	CHECK(!RememberRenderedBattleDiagnosticArena(10, 100, candidates, prefixes, &count));
	CHECK(count == 9);
	CHECK(candidates[0] == 129 && prefixes[0] == 255 && candidates[7] == 6 && prefixes[7] == 100);
	CHECK(candidates[8] == -99 && prefixes[8] == -99);
}

static void TestRenderedBattleDiagnosticContract()
{
	TestRenderedBattleDiagnosticSearch();
	TestRenderedBattleDiagnosticLocalPlacement();

	// Dense benchmark uses the same original 32 mixed units plus 32 infantry.
	// Pure bounds/geometry tests do not qualify live terrain or actual FPS.
	const char *benchmarkArgs[] = { "game", "-win", "-runRenderedBattleBenchmark", "1729" };
	const char *benchmarkReason = nullptr;
	CHECK(ValidateRenderedBattleDiagnosticArguments(4, benchmarkArgs, TRUE, &benchmarkReason));
	CHECK(!ValidateRenderedBattleDiagnosticArguments(4, benchmarkArgs, FALSE, &benchmarkReason));
	CHECK(strcmp(benchmarkReason, "unsupported_title") == 0);
	const char *benchmarkInvalid[] = { "game", "-runRenderedBattleBenchmark", "0" };
	CHECK(!ValidateRenderedBattleDiagnosticArguments(3, benchmarkInvalid, TRUE, &benchmarkReason));
	const char *benchmarkMixed[] = { "game", "-runRenderedBattleBenchmark", "1729",
		"-runRenderedBattleDiagnostic", "1730" };
	CHECK(!ValidateRenderedBattleDiagnosticArguments(5, benchmarkMixed, TRUE, &benchmarkReason));
	CHECK(strcmp(benchmarkReason, "duplicate_option") == 0);
	const char *benchmarkConflicts[] = { "-noFPSLimit", "-headless", "-replay", "-noshaders", "-particleEdit" };
	for (UnsignedInt index = 0; index < ARRAY_SIZE(benchmarkConflicts); ++index)
	{
		const char *before[] = { "game", benchmarkConflicts[index], "-runRenderedBattleBenchmark", "1729" };
		const char *after[] = { "game", "-runRenderedBattleBenchmark", "1729", benchmarkConflicts[index] };
		CHECK(!ValidateRenderedBattleDiagnosticArguments(4, before, TRUE, &benchmarkReason));
		CHECK(!ValidateRenderedBattleDiagnosticArguments(4, after, TRUE, &benchmarkReason));
	}
	SkirmishAITestPlan benchmarkPlan;
	BuildSkirmishAITestPlan(1729, SKIRMISH_AI_TEST_SCENARIO_RENDERED_BATTLE_BENCHMARK, &benchmarkPlan);
	CHECK(strcmp(benchmarkPlan.mapName, "Maps\\Fortress Avalanche\\Fortress Avalanche.map") == 0);
	for (Int slot = 0; slot < 8; ++slot)
	{
		CHECK(benchmarkPlan.slots[slot].teamNumber == (slot < 4 ? 0 : 1));
		for (Int unit = 0; unit < 64; ++unit)
		{
			Coord3D dense, mirrored;
			CHECK(GetRenderedBattleDiagnosticOffset(slot, unit, &dense, TRUE));
			CHECK(GetRenderedBattleDiagnosticOffset((slot + 4) % 8, unit, &mirrored, TRUE));
			CHECK(dense.x == -mirrored.x && dense.y == mirrored.y);
			CHECK(dense.x >= -459.0f && dense.x <= 459.0f && dense.y >= -390.0f && dense.y <= 416.0f);
			if (unit < 32)
			{
				Coord3D original;
				CHECK(GetRenderedBattleDiagnosticOffset(slot, unit, &original));
				CHECK(original.x == (slot < 4 ? -1.0f : 1.0f) * (95.0f + (unit % 8) * 44.0f));
				CHECK(original.y == ((slot % 4) - 1.5f) * 200.0f + (unit / 8 - 1.5f) * 44.0f);
				CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(slot, unit),
					GetRenderedBattleDiagnosticObjectName(slot, unit, TRUE)) == 0);
			}
			else CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(slot, unit, TRUE),
				GetRenderedBattleDiagnosticObjectName(slot, 2 + unit % 2)) == 0);
			// Exhaust every nominal pair with conservative raw21 vehicle and
			// raw10 infantry radii, including all adjacent player-band pairs.
			const Real radius = unit < 32 && unit % 4 < 2 ? 21.0f : 10.0f;
			for (Int earlierSlot = 0; earlierSlot <= slot; ++earlierSlot)
				for (Int earlierUnit = 0; earlierUnit < 64; ++earlierUnit)
				{
					if (earlierSlot == slot && earlierUnit >= unit) break;
					Coord3D earlier;
					CHECK(GetRenderedBattleDiagnosticOffset(earlierSlot, earlierUnit, &earlier, TRUE));
					const Real earlierRadius = earlierUnit < 32 && earlierUnit % 4 < 2 ? 21.0f : 10.0f;
					CHECK(AreRenderedBattleDiagnosticPositionsSeparated(dense, radius, earlier, earlierRadius));
				}
		}
	}
	Coord3D unchanged = { 1.0f, 2.0f, 3.0f };
	CHECK(GetRenderedBattleDiagnosticObjectName(0, 64, TRUE) == nullptr);
	CHECK(!GetRenderedBattleDiagnosticOffset(0, 64, &unchanged, TRUE));
	CHECK(unchanged.x == 1.0f && unchanged.y == 2.0f && unchanged.z == 3.0f);
	CHECK(ValidateRenderedBattleBenchmarkNominalGeometry(21.0f, 10.0f));
	CHECK(!ValidateRenderedBattleBenchmarkNominalGeometry(21.001f, 10.0f));
	CHECK(!ValidateRenderedBattleBenchmarkNominalGeometry(21.0f, 21.001f));
	CHECK(!ValidateRenderedBattleBenchmarkNominalGeometry(21.0f, 13.0f)); // mixed34->37 exceeds sqrt1352
	CHECK(!ValidateRenderedBattleBenchmarkNominalGeometry(0.0f, 10.0f));
	CHECK(!ValidateRenderedBattleBenchmarkNominalGeometry(21.0f, 0.0f));

	// Reproduce the observed candidate55 trap: the extra infantry's only
	// live-valid trial conflicts with the earlier vehicle nominal position.
	// MRV reserves that infantry first and revises the unconstrained vehicle.
	Coord3D domainPositions[512 * 9] = {};
	Real domainRadii[512] = {};
	UnsignedInt domainMasks[512] = {};
	Int chosen[512], repeated[512];
	for (Int index = 0; index < 512; ++index) chosen[index] = repeated[index] = -7;
	domainPositions[0].x = 2580.5f; domainPositions[0].y = 434.0f;
	domainPositions[1].x = 2602.5f; domainPositions[1].y = 456.0f;
	domainPositions[9 + 8].x = 2576.5f; domainPositions[9 + 8].y = 430.0f;
	domainRadii[0] = 15.811f; domainRadii[1] = 10.0f;
	domainMasks[0] = 3u; domainMasks[1] = 1u << 8;
	CHECK(!AreRenderedBattleDiagnosticPositionsSeparated(domainPositions[0], domainRadii[0],
		domainPositions[17], domainRadii[1]));
	RenderedBattleBenchmarkPlacementStats placementStats, repeatedStats;
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_SOLVED);
	CHECK(chosen[0] == 1 && chosen[1] == 8 && chosen[2] == -7 && placementStats.maxAssigned == 2);
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		1000, repeated, &repeatedStats) == RB_BENCHMARK_PLACEMENT_SOLVED);
	CHECK(repeated[0] == chosen[0] && repeated[1] == chosen[1]);
	CHECK(memcmp(&placementStats, &repeatedStats, sizeof(placementStats)) == 0);
	CHECK(domainMasks[0] == 3u && domainMasks[1] == (1u << 8)); // caller domains immutable

	// A two-choice tie needs actual backtracking, not just constrained-first
	// reordering: choosing A0 forces B1, which removes both C positions.
	memset(domainPositions, 0, sizeof(domainPositions));
	domainPositions[1].x = 100.0f; domainPositions[10].x = 200.0f;
	domainPositions[18].x = 200.0f; domainPositions[19].x = 201.0f;
	for (Int index = 0; index < 3; ++index) { domainRadii[index] = 1.0f; domainMasks[index] = 3u; }
	CHECK(SolveRenderedBattleBenchmarkPlacement(3, domainPositions, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_SOLVED);
	CHECK(chosen[0] == 1 && chosen[1] == 0 && chosen[2] == 0 && placementStats.backtracks > 0);

	for (Int index = 0; index < 512; ++index) chosen[index] = -7;
	domainMasks[0] = domainMasks[1] = 1u; // coincident singletons: proven unsatisfiable
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_UNSATISFIABLE);
	CHECK(placementStats.emptyDomains == 0 && placementStats.pairComparisons > 0);
	CHECK(chosen[0] == -7 && chosen[1] == -7);
	domainMasks[0] = 0;
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		0, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_EMPTY_DOMAIN);
	CHECK(placementStats.emptyDomains == 1 && placementStats.operations == 0);
	domainMasks[1] = 0;
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		0, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_EMPTY_DOMAIN);
	CHECK(placementStats.emptyDomains == 2 && chosen[0] == -7 && chosen[1] == -7);
	domainMasks[0] = domainMasks[1] = 1u;
	for (Int budget = 0; budget < 5; ++budget)
	{
		CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
			budget, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_BUDGET_EXHAUSTED);
		CHECK(placementStats.operations == budget && chosen[0] == -7 && chosen[1] == -7);
	}
	CHECK(SolveRenderedBattleBenchmarkPlacement(513, domainPositions, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_INVALID);
	domainMasks[0] = 512u;
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_INVALID);
	domainMasks[0] = 1u; domainRadii[0] = 21.001f;
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, domainPositions, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_INVALID);
	CHECK(chosen[0] == -7 && chosen[1] == -7);
	CHECK(SolveRenderedBattleBenchmarkPlacement(2, nullptr, domainRadii, domainMasks,
		1000, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_INVALID);

	// All 512 actual canonical indices, with the conservative nominal gate.
	// A solver may plan in any order but must return the original roster order.
	for (Int index = 0; index < 512; ++index)
	{
		const Int slot = index / 64, unit = index % 64;
		domainRadii[index] = unit < 32 && unit % 4 < 2 ? 21.0f : 10.0f;
		domainMasks[index] = 1u;
		CHECK(GetRenderedBattleDiagnosticOffset(slot, unit, &domainPositions[index * 9], TRUE));
	}
	CHECK(SolveRenderedBattleBenchmarkPlacement(512, domainPositions, domainRadii, domainMasks,
		RENDERED_BATTLE_BENCHMARK_ARENA_SEARCH_OPERATIONS, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_SOLVED);
	CHECK(placementStats.maxAssigned == 512 && placementStats.assignments == 512 &&
		placementStats.pairComparisons == 130816 &&
		placementStats.operations < RENDERED_BATTLE_BENCHMARK_ARENA_SEARCH_OPERATIONS);
	for (Int index = 0; index < 512; ++index)
	{
		CHECK(chosen[index] == 0);
		for (Int earlier = 0; earlier < index; ++earlier)
			CHECK(AreRenderedBattleDiagnosticPositionsSeparated(domainPositions[index * 9 + chosen[index]], domainRadii[index],
				domainPositions[earlier * 9 + chosen[earlier]], domainRadii[earlier]));
	}

	// Full nine-position domains also retain every safe nominal position.
	for (Int index = 0; index < 512; ++index)
	{
		domainMasks[index] = 511u;
		for (Int trial = 0; trial < 9; ++trial)
		{
			Coord3D local;
			CHECK(GetRenderedBattleDiagnosticOffset(index / 64, index % 64, &domainPositions[index * 9 + trial], TRUE));
			CHECK(GetRenderedBattleDiagnosticLocalOffset(index / 64, trial, &local));
			domainPositions[index * 9 + trial].x += local.x;
			domainPositions[index * 9 + trial].y += local.y;
		}
	}
	CHECK(SolveRenderedBattleBenchmarkPlacement(512, domainPositions, domainRadii, domainMasks,
		RENDERED_BATTLE_BENCHMARK_ARENA_SEARCH_OPERATIONS, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_SOLVED);
	CHECK(placementStats.maxAssigned == 512 && placementStats.operations <= RENDERED_BATTLE_BENCHMARK_ARENA_SEARCH_OPERATIONS);
	for (Int index = 0; index < 512; ++index) CHECK(chosen[index] == 0);
	const Int completedOperations = placementStats.operations;
	for (Int index = 0; index < 512; ++index) chosen[index] = -7;
	CHECK(SolveRenderedBattleBenchmarkPlacement(512, domainPositions, domainRadii, domainMasks,
		completedOperations - 1, chosen, &placementStats) == RB_BENCHMARK_PLACEMENT_BUDGET_EXHAUSTED);
	CHECK(placementStats.operations == completedOperations - 1);
	for (Int index = 0; index < 512; ++index) CHECK(chosen[index] == -7);

	CHECK(RENDERED_BATTLE_BENCHMARK_TOTAL_SEARCH_OPERATIONS == 16000000 &&
		RENDERED_BATTLE_BENCHMARK_ARENA_SEARCH_OPERATIONS == 4000000);

	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 1006.0f, 920.0f, TRUE) == 130);
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 1005.0f, 920.0f, TRUE) == 49);
	CHECK(GetRenderedBattleDiagnosticSearchCount(0.0f, 0.0f, 1006.0f, 919.0f, TRUE) == 49);
	Coord3D exact;
	CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 1006.0f, 920.0f, 129, &exact, TRUE));
	CHECK(exact.x == 503.0f && exact.y == 460.0f);
	for (Int grid = 0; grid < 81; ++grid)
	{
		Coord3D fit;
		CHECK(GetRenderedBattleDiagnosticSearchCenter(0.0f, 0.0f, 1006.0f, 920.0f, 49 + grid, &fit, TRUE));
		CHECK(fit.x == 503.0f && fit.y == 460.0f);
		for (Int slot = 0; slot < 8; ++slot)
			for (Int unit = 0; unit < 64; ++unit)
				for (Int trial = 0; trial < 9; ++trial)
				{
					Coord3D nominal, local;
					CHECK(GetRenderedBattleDiagnosticOffset(slot, unit, &nominal, TRUE));
					CHECK(GetRenderedBattleDiagnosticLocalOffset(slot, trial, &local));
					const Real x = fit.x + nominal.x + local.x, y = fit.y + nominal.y + local.y;
					CHECK(x - 22.0f >= 0.0f && x + 22.0f <= 1006.0f);
					CHECK(y - 22.0f >= 0.0f && y + 22.0f <= 920.0f);
				}
	}
	Int denseCandidates[8] = { 0 }, densePrefixes[8] = { 0 }, denseCount = 0;
	CHECK(RememberRenderedBattleDiagnosticArena(0, 511, denseCandidates, densePrefixes, &denseCount, TRUE));
	CHECK(!RememberRenderedBattleDiagnosticArena(1, 512, denseCandidates, densePrefixes, &denseCount, TRUE));
	CHECK(denseCount == 1 && densePrefixes[0] == 511);

	// The placement permutation reserves every original unit before extras,
	// while retaining canonical slot64 storage and exact original slot32 order.
	Bool denseSeen[512] = { FALSE };
	for (Int rank = 0; rank < 512; ++rank)
	{
		Int slot = -1, unit = -1;
		CHECK(GetRenderedBattleDiagnosticPlacementUnit(rank, TRUE, &slot, &unit));
		CHECK(slot >= 0 && slot < 8 && unit >= 0 && unit < 64);
		const Int canonical = slot * 64 + unit;
		CHECK(!denseSeen[canonical]);
		denseSeen[canonical] = TRUE;
		if (rank < 256)
		{
			CHECK(slot == rank / 32 && unit == rank % 32);
			Int legacySlot = -1, legacyUnit = -1;
			CHECK(GetRenderedBattleDiagnosticPlacementUnit(rank, FALSE, &legacySlot, &legacyUnit));
			CHECK(legacySlot == slot && legacyUnit == unit);
		}
		else CHECK(unit >= 32);
	}
	for (Int canonical = 0; canonical < 512; ++canonical) CHECK(denseSeen[canonical]);
	Int unchangedSlot = 9, unchangedUnit = 99;
	CHECK(!GetRenderedBattleDiagnosticPlacementUnit(-1, TRUE, &unchangedSlot, &unchangedUnit));
	CHECK(!GetRenderedBattleDiagnosticPlacementUnit(512, TRUE, &unchangedSlot, &unchangedUnit));
	CHECK(!GetRenderedBattleDiagnosticPlacementUnit(256, FALSE, &unchangedSlot, &unchangedUnit));
	CHECK(!GetRenderedBattleDiagnosticPlacementUnit(0, TRUE, nullptr, &unchangedUnit));
	CHECK(!GetRenderedBattleDiagnosticPlacementUnit(0, TRUE, &unchangedSlot, nullptr));
	CHECK(unchangedSlot == 9 && unchangedUnit == 99);
	// Failed live v1 collided at canonical index472 (slot7 unit24), then
	// slot7 unit25. Both now reserve positions before the first extra infantry.
	Int orderedSlot = -1, orderedUnit = -1;
	CHECK(GetRenderedBattleDiagnosticPlacementUnit(248, TRUE, &orderedSlot, &orderedUnit));
	CHECK(orderedSlot == 7 && orderedUnit == 24 && orderedSlot * 64 + orderedUnit == 472);
	CHECK(GetRenderedBattleDiagnosticPlacementUnit(249, TRUE, &orderedSlot, &orderedUnit));
	CHECK(orderedSlot == 7 && orderedUnit == 25);
	CHECK(GetRenderedBattleDiagnosticPlacementUnit(256, TRUE, &orderedSlot, &orderedUnit));
	CHECK(orderedSlot == 0 && orderedUnit == 32);
	// Production fast-mode guard covers all engine compile-policy combinations.
	for (Int fast = 0; fast < 2; ++fast)
		for (Int replay = 0; replay < 2; ++replay)
		{
			CHECK(IsRenderedBattleBenchmarkFastModeActive(fast != 0, replay != 0, TRUE) == (fast != 0));
			CHECK(IsRenderedBattleBenchmarkFastModeActive(fast != 0, replay != 0, FALSE) == (fast != 0 && replay != 0));
		}

#if RTS_ZEROHOUR
	CommandLineData denseCommand;
	CHECK(!denseCommand.isRenderedBattleBenchmark());
	CHECK(denseCommand.requestRenderedBattleDiagnostic(1729, TRUE));
	CHECK(denseCommand.isRenderedBattleBenchmark() && denseCommand.hasRenderedBattleDiagnosticRequest());
	CHECK(!denseCommand.requestRenderedBattleDiagnostic(1730));
	CHECK(denseCommand.isRenderedBattleBenchmark() && denseCommand.getRenderedBattleDiagnosticSeed() == 1729);
#endif

	char report[8] = "";
	UnsignedInt used = 0;
	CHECK(AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), &used, "abc\n", 4));
	CHECK(used == 4 && strcmp(report, "abc\n") == 0);
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), &used, "defg", 4));
	CHECK(used == 4 && strcmp(report, "abc\n") == 0);
	CHECK(AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), &used, "def", 3));
	CHECK(used == 7 && report[7] == '\0' && strcmp(report, "abc\ndef") == 0);
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), &used, "x", 1));
	CHECK(used == 7 && strcmp(report, "abc\ndef") == 0);
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(nullptr, sizeof(report), &used, "x", 1));
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), nullptr, "x", 1));
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), &used, nullptr, 1));
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(report, 0, &used, "x", 1));
	used = 8;
	CHECK(!AppendRenderedBattleDiagnosticReportRecord(report, sizeof(report), &used, "x", 1));
	CHECK(used == 8 && strcmp(report, "abc\ndef") == 0);
	// New enum appended; existing scenario numeric values retain their meanings.
	CHECK(SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6 == 3);
	CHECK(SKIRMISH_AI_TEST_SCENARIO_RENDERED_BATTLE_DIAGNOSTIC == 4);
	const char *reason = nullptr;
	const char *ordinary[] = { "game", "-headless", "-noFPSLimit", "-replay", "old.rep" };
	CHECK(ValidateRenderedBattleDiagnosticArguments(5, ordinary, FALSE, &reason));
	CHECK(reason == nullptr);
	const char *valid[] = { "game", "-win", "-nologo", "-xres", "1920", "-yres", "1080",
		"-simulationMode", "parallel", "-workerPolicy", "auto", "-runRenderedBattleDiagnostic", "1729" };
	CHECK(ValidateRenderedBattleDiagnosticArguments(13, valid, TRUE, &reason));
	CHECK(!ValidateRenderedBattleDiagnosticArguments(13, valid, FALSE, &reason));
	CHECK(strcmp(reason, "unsupported_title") == 0);
	const char *invalidSeed[] = { "game", "-runRenderedBattleDiagnostic", "0" };
	CHECK(!ValidateRenderedBattleDiagnosticArguments(3, invalidSeed, TRUE, &reason));
	const char *missingSeed[] = { "game", "-runRenderedBattleDiagnostic" };
	CHECK(!ValidateRenderedBattleDiagnosticArguments(2, missingSeed, TRUE, &reason));
	const char *duplicate[] = { "game", "-runRenderedBattleDiagnostic", "1729",
		"-runRenderedBattleDiagnostic", "1729" };
	CHECK(!ValidateRenderedBattleDiagnosticArguments(5, duplicate, TRUE, &reason));
	const char *conflicts[] = { "-headless", "-noFPSLimit", "-replay", "-loadsave",
		"-runSkirmishAITestPractical1v7", "-runSkirmishAITestHardAI2v6",
		"-runSkirmishAIRecoveryTest", "-runStage5PerformanceFixture", "-mod", "-map" };
	for (UnsignedInt i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); ++i)
	{
		const char *before[] = { "game", conflicts[i], "-runRenderedBattleDiagnostic", "1729" };
		const char *after[] = { "game", "-runRenderedBattleDiagnostic", "1729", conflicts[i] };
		CHECK(!ValidateRenderedBattleDiagnosticArguments(4, before, TRUE, &reason));
		CHECK(!ValidateRenderedBattleDiagnosticArguments(4, after, TRUE, &reason));
	}
#if RTS_ZEROHOUR
	CommandLineData commandLine;
	CHECK(!commandLine.hasRenderedBattleDiagnosticRequest());
	CHECK(commandLine.getRenderedBattleDiagnosticSeed() == 0);
	CHECK(!commandLine.requestRenderedBattleDiagnostic(0));
	CHECK(!commandLine.hasRenderedBattleDiagnosticRequest());
	CHECK(commandLine.requestRenderedBattleDiagnostic(1729));
	CHECK(commandLine.getRenderedBattleDiagnosticSeed() == 1729);
	CHECK(!commandLine.requestRenderedBattleDiagnostic(1730));
	CHECK(!commandLine.requestSkirmishAITest(1730));
	CHECK(!commandLine.requestSkirmishAITest4v2(1730));
	CHECK(!commandLine.requestSkirmishAITestPractical1v7(1730));
	CHECK(!commandLine.requestSkirmishAIRecoveryTest(1730, 0, 0));
#if defined(_WIN64)
	CHECK(!commandLine.requestSkirmishAITestHardAI2v6(1730));
#endif
	CommandLineData practical;
	CHECK(practical.requestSkirmishAITestPractical1v7(1729));
	CHECK(!practical.hasRenderedBattleDiagnosticRequest());
	CHECK(!practical.requestRenderedBattleDiagnostic(1729));
#endif
	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(1729, SKIRMISH_AI_TEST_SCENARIO_RENDERED_BATTLE_DIAGNOSTIC, &plan);
	CHECK(plan.seed == 1729);
	// Bounded stock metadata proves eight starts; live terrain/occupancy/routes
	// remain production preflight requirements, not synthetic fixture passes.
	CHECK(strcmp(plan.mapName, "Maps\\Fortress Avalanche\\Fortress Avalanche.map") == 0);
	SkirmishAITestPlan ordinaryPlan;
	BuildSkirmishAITestPlan(1729, SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7, &ordinaryPlan);
	CHECK(strcmp(ordinaryPlan.mapName, "Maps\\Twilight Flame\\Twilight Flame.map") == 0);
	CHECK(plan.slots[0].isController && plan.slots[0].state == SLOT_PLAYER);
	for (Int slot = 0; slot < 8; ++slot)
	{
		CHECK(plan.slots[slot].teamNumber == (slot < 4 ? 0 : 1));
		CHECK(plan.slots[slot].startPosition == slot);
		if (slot) CHECK(plan.slots[slot].state == SLOT_BRUTAL_AI && !plan.slots[slot].isController);
		CHECK(strcmp(GetRenderedBattleDiagnosticFactionName(slot),
			GetRenderedBattleDiagnosticFactionName((slot + 4) % 8)) == 0);
		for (Int unit = 0; unit < RENDERED_BATTLE_DIAGNOSTIC_UNITS_PER_PLAYER; ++unit)
		{
			CHECK(GetRenderedBattleDiagnosticObjectName(slot, unit) != nullptr);
			Coord3D a, mirror;
			CHECK(GetRenderedBattleDiagnosticOffset(slot, unit, &a));
			CHECK(GetRenderedBattleDiagnosticOffset((slot + 4) % 8, unit, &mirror));
			CHECK(a.x == -mirror.x && a.y == mirror.y && a.z == 0.0f);
			// Exact formation envelope reported by the live preflight diagnostic.
			// These pure helper checks do not qualify real map terrain or routes.
			CHECK(slot < 4 ? (a.x >= -403.0f && a.x <= -95.0f) :
				(a.x >= 95.0f && a.x <= 403.0f));
			CHECK(a.y >= -366.0f && a.y <= 366.0f);
			if (slot == 0 && unit == 0) CHECK(a.x == -95.0f && a.y == -366.0f);
			if (slot == 7 && unit == 31) CHECK(a.x == 403.0f && a.y == 366.0f);
			for (Int otherSlot = 0; otherSlot <= slot; ++otherSlot)
				for (Int otherUnit = 0; otherUnit < RENDERED_BATTLE_DIAGNOSTIC_UNITS_PER_PLAYER; ++otherUnit)
				{
					if (otherSlot == slot && otherUnit >= unit) break;
					Coord3D p; CHECK(GetRenderedBattleDiagnosticOffset(otherSlot, otherUnit, &p));
					const Real dx = p.x - a.x, dy = p.y - a.y;
					CHECK(dx * dx + dy * dy >= 44.0f * 44.0f);
				}
		}
	}
	CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(0, 0), "AmericaTankCrusader") == 0);
	CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(1, 3), "ChinaInfantryTankHunter") == 0);
	CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(2, 2), "GLAInfantryRebel") == 0);
	// Stock Zero Hour Object identifiers, not localized unit display names.
	for (Int unit = 0; unit < RENDERED_BATTLE_DIAGNOSTIC_UNITS_PER_PLAYER; ++unit)
	{
		if (unit % 4 == 1)
		{
			CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(1, unit), "ChinaTankGattling") == 0);
			CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(5, unit), "ChinaTankGattling") == 0);
		}
		if (unit % 4 == 3)
		{
			CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(2, unit), "GLAInfantryTunnelDefender") == 0);
			CHECK(strcmp(GetRenderedBattleDiagnosticObjectName(6, unit), "GLAInfantryTunnelDefender") == 0);
		}
	}
	CHECK(GetRenderedBattleDiagnosticFactionName(-1) == nullptr);
	CHECK(GetRenderedBattleDiagnosticFactionName(8) == nullptr);
	CHECK(GetRenderedBattleDiagnosticObjectName(0, -1) == nullptr);
	CHECK(GetRenderedBattleDiagnosticObjectName(0, 32) == nullptr);
	CHECK(!GetRenderedBattleDiagnosticOffset(0, 0, nullptr));
	SkirmishAITestLoadedState loaded = { plan.mapName, plan.mapName, plan.mapName, 0x1234u, 65536u, 1729 };
	CHECK(IsExpectedSkirmishAITestLoadedState(plan, 0x1234u, 65536u, &loaded));
	loaded.mapCRC = 0x4321u;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, 0x1234u, 65536u, &loaded));
	CHECK(RENDERED_BATTLE_DIAGNOSTIC_MAX_FRAMES == 1800);
	CHECK(RENDERED_BATTLE_DIAGNOSTIC_MAX_MILLISECONDS == 120000);
}

int main(int argc, char **argv)
{
#if defined(_WIN64)
	if (argc >= 2 && (strcmp(argv[1], "--performance-receipt-files") == 0 ||
		strcmp(argv[1], "--performance-receipt-file-aliases") == 0))
	{
		if (argc != 3)
		{
			fprintf(stderr, "PREREQUISITE: held-file selector requires one exact fresh scratch root.\n");
			return 2;
		}
		wchar_t root[MAX_PATH];
		unsigned index = 0;
		for (; argv[2][index] != '\0'; ++index)
		{
			const unsigned char byte = static_cast<unsigned char>(argv[2][index]);
			if (index >= MAX_PATH - 1 || byte > 0x7f)
			{
				fprintf(stderr, "PREREQUISITE: held-file scratch root must be bounded ASCII.\n");
				return 2;
			}
			root[index] = static_cast<wchar_t>(byte);
		}
		root[index] = L'\0';
		return RunPerformanceReceiptTraceFileTests(root,
			strcmp(argv[1], "--performance-receipt-file-aliases") == 0);
	}
	if (argc == 2 && strcmp(argv[1], "--generals-pathfinding-recorder-epoch") == 0)
	{
		GeneralsPathfindingRecorderEpochTest::TestLifecycle();
		printf("Generals recorder path epoch: %d failure(s).\n", s_failures);
		return s_failures != 0 ? 1 : 0;
	}
	if (argc == 2 && strcmp(argv[1], "--headless-metric-startup") == 0)
		return RunHeadlessMetricStartupTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-owner-bridge") == 0)
		return RunPerformanceReceiptOwnerBridgeTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-producer") == 0)
		return RunPerformanceReceiptProducerTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-fresh-producer") == 0)
		return RunPerformanceReceiptFreshProducerTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-attempt") == 0)
		return RunPerformanceReceiptAttemptTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-bootstrap") == 0)
		return RunPerformanceReceiptBootstrapTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-policy") == 0)
		return RunPerformanceReceiptPolicyTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-mode") == 0)
		return RunPerformanceReceiptModeTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-wide-path") == 0)
		return RunPerformanceReceiptWidePathTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-consume-wide-path") == 0)
		return RunPerformanceReceiptConsumeWidePathTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-replay-owner-shape") == 0)
		return RunPerformanceReceiptReplayOwnerShapeTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-practical-owner-shape") == 0)
		return RunPerformanceReceiptPracticalOwnerShapeTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-replay-caller-fence") == 0)
		return RunPerformanceReceiptReplayCallerFenceTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-fresh-caller-fence") == 0)
		return RunPerformanceReceiptFreshCallerFenceTitleTests();
	if (argc == 2 && strcmp(argv[1], "--performance-receipt-lifecycle") == 0)
	{
		TestPerformanceReceiptOwnerLifecycle();
		return s_failures != 0 ? 1 : 0;
	}
#endif
	if (argc == 2 && strcmp(argv[1], "--xfer-crc-snapshot") == 0)
		return RunXferCrcSnapshotTests();
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-replay-epoch") == 0)
	{
		TestSkirmishAIReplayEpoch();
		if (s_failures != 0)
		{
			printf("%d Generals skirmish AI replay epoch test(s) failed.\n", s_failures);
			return 1;
		}
		printf("All Generals skirmish AI replay epoch tests passed.\n");
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--generals-pathfinding-replay-epoch") == 0)
	{
		TestGeneralsPathfindingReplayEpochWriter();
		printf("Generals path epoch writer: %d failure(s).\n", s_failures);
		return s_failures != 0 ? 1 : 0;
	}
#if defined(_WIN64)
	if (argc == 2 && strcmp(argv[1], "--native-logical-audio") == 0)
	{
		initMemoryManager();
		printf("Running 36 Generals production-linked, device-free logical audio cases.\n");
		fflush(stdout);
		TestNativeLogicalAudioSeed();
		if (s_failures != 0)
		{
			printf("%d Generals native logical audio test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All Generals native logical audio tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
#endif
	if (argc == 2 && strcmp(argv[1], "--texture-load-queue-contract") == 0)
	{
		TestTextureLoadQueuePublication();
		if (s_failures != 0)
		{
			printf("%d Generals texture load queue contract test(s) failed.\n", s_failures);
			return 1;
		}
		printf("All Generals texture load queue contract tests passed.\n");
		return 0;
	}

	CHECK(!IsSkirmishAITestRunnerArmed());
	TestRenderedBattleDiagnosticContract();
#if defined(_WIN64)
	TestPerformanceReceiptOwnerLifecycle();
#endif
	CommandLineData practicalCommandLine;
	CHECK(!practicalCommandLine.hasSkirmishAITestPractical1v7Request());
	CHECK(practicalCommandLine.getSkirmishAITestPractical1v7Seed() == 0);
	CHECK(practicalCommandLine.requestSkirmishAITestPractical1v7(1731));
	CHECK(practicalCommandLine.hasSkirmishAITestPractical1v7Request());
	CHECK(practicalCommandLine.getSkirmishAITestPractical1v7Seed() == 1731);
	CHECK(!practicalCommandLine.requestSkirmishAITest(1732));
	CHECK(!practicalCommandLine.requestSkirmishAITest4v2(1732));
	CHECK(!practicalCommandLine.requestSkirmishAITestPractical1v7(1732));
	Int seed = 0;
	CHECK(TryParseSkirmishAITestSeed("1729", &seed));
	CHECK(seed == 1729);
	CHECK(!TryParseSkirmishAITestSeed(nullptr, &seed));
	CHECK(!TryParseSkirmishAITestSeed("", &seed));
	CHECK(!TryParseSkirmishAITestSeed("0", &seed));
	CHECK(!TryParseSkirmishAITestSeed("-1", &seed));
	CHECK(!TryParseSkirmishAITestSeed("12x", &seed));
	CHECK(!TryParseSkirmishAITestSeed("2147483648", &seed));
	CHECK(!TryParseSkirmishAITestSeed("1", nullptr));
	const char *executableHash =
		"0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";
	CHECK(SetSkirmishAITestExecutableHashInput(executableHash));
	CHECK(!SetSkirmishAITestExecutableHashInput(nullptr));
	CHECK(!SetSkirmishAITestExecutableHashInput("0123456789abcdef"));
	CHECK(!SetSkirmishAITestExecutableHashInput(
		"G123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF"));
	CHECK(SetSkirmishAITestSimulationModeInput("serial"));
	CHECK(SetSkirmishAITestSimulationModeInput("parallel"));
	CHECK(SetSkirmishAITestSimulationModeInput("shadow"));
	CHECK(!SetSkirmishAITestSimulationModeInput(nullptr));
	CHECK(!SetSkirmishAITestSimulationModeInput("automatic"));
	SetSkirmishAITestFinalDigest(0x12345678U);
	TestSkirmishAITestReceiptContract();
#if defined(_WIN32)
	TestSkirmishAITestReplayRetentionCommitPolicy();
#endif
	TestSkirmishAITestHardAI2v6Contract();
	CHECK(!rts::ShouldUseLiveSimulationPhaseGraph(false, false, 0, 1));
	CHECK(rts::ShouldUseLiveSimulationPhaseGraph(true, false, 0, 1));
	CHECK(!rts::ShouldUseLiveSimulationPhaseGraph(true, true, 0, 1));
	CHECK(rts::ShouldUseLiveSimulationPhaseGraph(true, true, 1, 1));
	CHECK(rts::IsLiveSimulationPhaseReleaseWorkerCount(1));
	CHECK(rts::IsLiveSimulationPhaseReleaseWorkerCount(2));
	CHECK(rts::IsLiveSimulationPhaseReleaseWorkerCount(4));
	CHECK(rts::IsLiveSimulationPhaseReleaseWorkerCount(8));
	CHECK(rts::IsLiveSimulationPhaseReleaseWorkerCount(16));
	CHECK(!rts::IsLiveSimulationPhaseReleaseWorkerCount(3));
	rts::LiveSimulationPhaseRuntimeMetrics phaseMetrics;
	phaseMetrics.attemptedFrames = 2;
	phaseMetrics.completedFrames = 2;
	phaseMetrics.stableSequenceFrames = 2;
	phaseMetrics.committedPhases = 10;
	phaseMetrics.lastFrame = 2;
	phaseMetrics.lastGeneration = 2;
	phaseMetrics.lastCommittedPhaseCount = 5;
	phaseMetrics.lastSequenceSignature = 12345;
	UnsignedInt performancePhaseOrdinal;
	for (performancePhaseOrdinal = 0;
		performancePhaseOrdinal < rts::LIVE_SIMULATION_PHASE_COUNT - 1;
		++performancePhaseOrdinal)
	{
		phaseMetrics.ownerPhaseTotalNanoseconds[performancePhaseOrdinal] = 20;
		phaseMetrics.ownerPhaseMaximumNanoseconds[performancePhaseOrdinal] = 10;
		phaseMetrics.ownerPhaseSampleCount[performancePhaseOrdinal] = 2;
	}
	phaseMetrics.frameSimulationTotalNanoseconds = 100;
	phaseMetrics.frameSimulationSampleCount = 2;
	phaseMetrics.serialIslandTotalNanoseconds = 40;
	phaseMetrics.serialIslandSampleCount = 2;
	CHECK(rts::HasStableLiveSimulationPhaseEvidence(phaseMetrics));
	CHECK(rts::LIVE_SIMULATION_PHASE_PERFORMANCE_SCHEMA_VERSION == 1);
	CHECK(phaseMetrics.serialIslandTotalNanoseconds <=
		phaseMetrics.frameSimulationTotalNanoseconds &&
		phaseMetrics.frameSimulationSampleCount ==
			phaseMetrics.serialIslandSampleCount &&
		phaseMetrics.ownerPhaseSampleCount[0] ==
			phaseMetrics.frameSimulationSampleCount &&
		phaseMetrics.ownerPhaseSampleCount[4] ==
			phaseMetrics.frameSimulationSampleCount);
	phaseMetrics.sequenceViolationFrames = 1;
	CHECK(!rts::HasStableLiveSimulationPhaseEvidence(phaseMetrics));

	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(1729, &plan);
	CHECK(plan.seed == 1729);
	CHECK(strcmp(plan.mapName, "Maps\\Twilight Flame\\Twilight Flame.map") == 0);
	CHECK(plan.slots[0].state == SLOT_PLAYER);
	CHECK(plan.slots[0].playerTemplate == PLAYERTEMPLATE_OBSERVER);
	CHECK(plan.slots[0].color == -1);
	CHECK(plan.slots[0].startPosition == -1);
	CHECK(plan.slots[0].teamNumber == -1);

	for (Int i = 1; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		CHECK(plan.slots[i].state == SLOT_BRUTAL_AI);
		CHECK(plan.slots[i].playerTemplate == PLAYERTEMPLATE_RANDOM);
		CHECK(plan.slots[i].color == i - 1);
		CHECK(plan.slots[i].startPosition == i - 1);
		CHECK(plan.slots[i].teamNumber == (i <= 4 ? 0 : 1));
	}

	SkirmishAITestPlan plan4v2;
	BuildSkirmishAITestPlan(1730, SKIRMISH_AI_TEST_SCENARIO_4V2, &plan4v2);
	CHECK(plan4v2.seed == 1730);
	CHECK(strcmp(plan4v2.mapName, "Maps\\Twilight Flame\\Twilight Flame.map") == 0);
	CHECK(plan4v2.slots[0].state == SLOT_PLAYER);
	CHECK(plan4v2.slots[0].playerTemplate == PLAYERTEMPLATE_OBSERVER);
	for (Int slot4v2 = 1; slot4v2 <= 6; ++slot4v2)
	{
		CHECK(plan4v2.slots[slot4v2].state == SLOT_BRUTAL_AI);
		CHECK(plan4v2.slots[slot4v2].playerTemplate == PLAYERTEMPLATE_RANDOM);
		CHECK(plan4v2.slots[slot4v2].color == slot4v2 - 1);
		CHECK(plan4v2.slots[slot4v2].startPosition == slot4v2 - 1);
		CHECK(plan4v2.slots[slot4v2].teamNumber == (slot4v2 <= 4 ? 0 : 1));
	}
	CHECK(plan4v2.slots[7].state == SLOT_CLOSED);
	CHECK(plan4v2.slots[7].playerTemplate == -1);
	CHECK(plan4v2.slots[7].color == -1);
	CHECK(plan4v2.slots[7].startPosition == -1);
	CHECK(plan4v2.slots[7].teamNumber == -1);

	const UnsignedInt expectedMapCRC = 0x12345678U;
	const UnsignedInt expectedMapSize = 0x00123456U;
	SkirmishAITestLoadedState loadedState = {
		"maps\\twilight flame\\twilight flame.map",
		"MAPS\\TWILIGHT FLAME\\TWILIGHT FLAME.MAP",
		"Maps\\Twilight Flame\\Twilight Flame.map",
		expectedMapCRC,
		expectedMapSize,
		1729
	};
	CHECK(IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, nullptr));
	loadedState.gameInfoMapName = "Maps\\Tournament Desert\\Tournament Desert.map";
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.gameInfoMapName = plan.mapName;
	loadedState.globalMapName = nullptr;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.globalMapName = plan.mapName;
	loadedState.terrainMapName = "Maps\\Tournament Desert\\Tournament Desert.map";
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.terrainMapName = plan.mapName;
	loadedState.mapCRC ^= 1U;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.mapCRC = expectedMapCRC;
	loadedState.mapSize++;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.mapSize = expectedMapSize;
	loadedState.seed++;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));

	SkirmishAITestLoadedState loaded4v2 = {
		"maps\\twilight flame\\twilight flame.map",
		"MAPS\\TWILIGHT FLAME\\TWILIGHT FLAME.MAP",
		"Maps\\Twilight Flame\\Twilight Flame.map",
		expectedMapCRC,
		expectedMapSize,
		1730
	};
	CHECK(IsExpectedSkirmishAITestLoadedState(plan4v2, expectedMapCRC, expectedMapSize, &loaded4v2));
	loaded4v2.seed = 1729;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan4v2, expectedMapCRC, expectedMapSize, &loaded4v2));

	CHECK(EvaluateSkirmishAITestProgress(0, 0) == SKIRMISH_AI_TEST_RUNNING);
	CHECK(EvaluateSkirmishAITestProgress(0, 107999) == SKIRMISH_AI_TEST_RUNNING);
	CHECK(EvaluateSkirmishAITestProgress(0, 108000) == SKIRMISH_AI_TEST_RUNNING);
	CHECK(EvaluateSkirmishAITestProgress(0, 215999) == SKIRMISH_AI_TEST_RUNNING);
	CHECK(EvaluateSkirmishAITestProgress(42000, 42001) == SKIRMISH_AI_TEST_COMPLETE);
	CHECK(EvaluateSkirmishAITestProgress(0, 216000) == SKIRMISH_AI_TEST_TIMED_OUT);
	CHECK(!IsSkirmishAITestStartupTimedOut(299999));
	CHECK(IsSkirmishAITestStartupTimedOut(300000));
	CHECK(!IsSkirmishAITestProgressStalled(29999));
	CHECK(IsSkirmishAITestProgressStalled(30000));
	CHECK(!IsSkirmishAITestShutdownTimedOut(29999));
	CHECK(IsSkirmishAITestShutdownTimedOut(30000));

	DirectPathRuntimeMetrics baseline;
	DirectPathRuntimeMetrics current;
	DirectPathRuntimeMetrics frozen;
	memset(&baseline, 0, sizeof(baseline));
	memset(&current, 0, sizeof(current));
	memset(&frozen, 0, sizeof(frozen));
	baseline.resetEpoch = 10;
	current.resetEpoch = 10;
	current.eligibleRequests = 99;
	current.workerExecutedJobs = 99;
	current.authoritativeCommits = 99;
	current.authoritativeMultiWorkerCommits = 99;
	Bool hasFrozenActivity = FALSE;
	Bool awaitingInitialReset = TRUE;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(awaitingInitialReset && !hasFrozenActivity &&
		frozen.workerExecutedJobs == 0 && frozen.authoritativeCommits == 0 &&
		frozen.authoritativeMultiWorkerCommits == 0);
	memset(&current, 0, sizeof(current));
	current.resetEpoch = 11;
	current.eligibleRequests = 5;
	current.submittedJobs = 4;
	current.executedJobs = 4;
	current.workerExecutedJobs = 3;
	current.authoritativeCommits = 2;
	current.authoritativeMultiWorkerCommits = 1;
	current.timeoutCancellations = 1;
	current.peakActiveWorkers = 1;
	current.minimumCallbackCount = 8;
	current.maximumCallbackCount = 24;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(!awaitingInitialReset && hasFrozenActivity &&
		frozen.workerExecutedJobs == 3 &&
		frozen.authoritativeCommits == 2 &&
		frozen.authoritativeMultiWorkerCommits == 1 &&
		frozen.timeoutCancellations == 1);
	memset(&current, 0, sizeof(current));
	current.resetEpoch = 12;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(frozen.workerExecutedJobs == 3 && frozen.authoritativeCommits == 2 &&
		frozen.authoritativeMultiWorkerCommits == 1 &&
		frozen.timeoutCancellations == 1 &&
		frozen.minimumCallbackCount == 8 && frozen.maximumCallbackCount == 24);
	baseline = current;
	memset(&frozen, 0, sizeof(frozen));
	hasFrozenActivity = FALSE;
	awaitingInitialReset = TRUE;
	current.resetEpoch = 13;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(!awaitingInitialReset && !hasFrozenActivity &&
		baseline.resetEpoch == 13 &&
		frozen.authoritativeCommits == 0 &&
		frozen.authoritativeMultiWorkerCommits == 0 &&
		frozen.timeoutCancellations == 0);

	OrdinaryPathRuntimeMetrics ordinaryBaseline;
	OrdinaryPathRuntimeMetrics ordinaryCurrent;
	OrdinaryPathRuntimeMetrics ordinaryFrozen;
	memset(&ordinaryBaseline, 0, sizeof(ordinaryBaseline));
	memset(&ordinaryCurrent, 0, sizeof(ordinaryCurrent));
	memset(&ordinaryFrozen, 0, sizeof(ordinaryFrozen));
	ordinaryBaseline.resetEpoch = 20;
	ordinaryCurrent.resetEpoch = 20;
	ordinaryCurrent.workerExecutedRangeJobs = 91;
	ordinaryCurrent.authoritativeCommits = 90;
	Bool ordinaryAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
		ordinaryCurrent, &ordinaryFrozen, &ordinaryAwaitingInitialReset);
	CHECK(ordinaryAwaitingInitialReset &&
		ordinaryFrozen.workerExecutedRangeJobs == 0 &&
		ordinaryFrozen.authoritativeCommits == 0);
	memset(&ordinaryCurrent, 0, sizeof(ordinaryCurrent));
	ordinaryCurrent.resetEpoch = 21;
	ordinaryCurrent.eligibleRequests = 8;
	ordinaryCurrent.submittedRequests = 6;
	ordinaryCurrent.submittedRangeJobs = 4;
	ordinaryCurrent.workerExecutedRequests = 6;
	ordinaryCurrent.workerExecutedRangeJobs = 4;
	ordinaryCurrent.physicalWorkerMask = 5;
	ordinaryCurrent.distinctPhysicalWorkers = 65;
	ordinaryCurrent.physicalWorkerMaskComplete = FALSE;
	ordinaryCurrent.authoritativeCommits = 3;
	ordinaryCurrent.authoritativeMultiWorkerCommits = 2;
	ordinaryCurrent.peakActiveWorkers = 2;
	ordinaryCurrent.maximumBatchRequests = 6;
	ordinaryCurrent.maximumRangeCount = 4;
	ordinaryCurrent.maximumGrainSize = 2;
	AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
		ordinaryCurrent, &ordinaryFrozen, &ordinaryAwaitingInitialReset);
	CHECK(!ordinaryAwaitingInitialReset &&
		ordinaryFrozen.workerExecutedRangeJobs == 4 &&
		ordinaryFrozen.physicalWorkerMask == 5 &&
		ordinaryFrozen.distinctPhysicalWorkers == 65 &&
		!ordinaryFrozen.physicalWorkerMaskComplete &&
		ordinaryFrozen.authoritativeCommits == 3 &&
		ordinaryFrozen.authoritativeMultiWorkerCommits == 2);
	memset(&ordinaryCurrent, 0, sizeof(ordinaryCurrent));
	ordinaryCurrent.resetEpoch = 22;
	AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
		ordinaryCurrent, &ordinaryFrozen, &ordinaryAwaitingInitialReset);
	CHECK(ordinaryFrozen.workerExecutedRangeJobs == 4 &&
		ordinaryFrozen.physicalWorkerMask == 5 &&
		ordinaryFrozen.authoritativeCommits == 3 &&
		ordinaryFrozen.maximumBatchRequests == 6);
	ordinaryBaseline = ordinaryCurrent;
	memset(&ordinaryFrozen, 0, sizeof(ordinaryFrozen));
	ordinaryAwaitingInitialReset = TRUE;
	ordinaryCurrent.resetEpoch = 23;
	AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
		ordinaryCurrent, &ordinaryFrozen, &ordinaryAwaitingInitialReset);
	CHECK(!ordinaryAwaitingInitialReset &&
		ordinaryFrozen.workerExecutedRangeJobs == 0 &&
		ordinaryFrozen.physicalWorkerMask == 0 &&
		ordinaryFrozen.authoritativeCommits == 0);

#if defined(_WIN64)
	rts::CollisionCandidateRuntimeMetrics collisionBaseline;
	rts::CollisionCandidateRuntimeMetrics collisionCurrent;
	rts::CollisionCandidateRuntimeMetrics collisionFrozen;
	collisionBaseline.resetEpoch = 30;
	collisionBaseline.authoritativeCommits = 90;
	collisionCurrent = collisionBaseline;
	Bool collisionAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
		collisionCurrent, &collisionFrozen, &collisionAwaitingInitialReset);
	CHECK(collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 0);
	collisionCurrent = rts::CollisionCandidateRuntimeMetrics();
	collisionCurrent.resetEpoch = 31;
	collisionCurrent.authoritativeCommits = 3;
	collisionCurrent.committedCandidates = 12;
	collisionCurrent.submittedJobs = 4;
	collisionCurrent.completedJobs = 4;
	collisionCurrent.physicalWorkerJobs = 3;
	collisionCurrent.ownerHelpedJobs = 1;
	collisionCurrent.physicalWorkerMask = 5;
	collisionCurrent.distinctPhysicalWorkers = 65;
	collisionCurrent.physicalWorkerMaskComplete = false;
	AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
		collisionCurrent, &collisionFrozen, &collisionAwaitingInitialReset);
	CHECK(!collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 3 &&
		collisionFrozen.committedCandidates == 12 &&
		collisionFrozen.physicalWorkerJobs == 3 &&
		collisionFrozen.ownerHelpedJobs == 1 &&
		collisionFrozen.physicalWorkerMask == 5 &&
		collisionFrozen.distinctPhysicalWorkers == 65 &&
		!collisionFrozen.physicalWorkerMaskComplete);
	collisionCurrent = rts::CollisionCandidateRuntimeMetrics();
	collisionCurrent.resetEpoch = 32;
	AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
		collisionCurrent, &collisionFrozen, &collisionAwaitingInitialReset);
	CHECK(collisionFrozen.authoritativeCommits == 3 &&
		collisionFrozen.committedCandidates == 12 &&
		collisionFrozen.physicalWorkerJobs == 3 &&
		collisionFrozen.ownerHelpedJobs == 1 &&
		collisionFrozen.physicalWorkerMask == 5);
	collisionBaseline = collisionCurrent;
	collisionFrozen = rts::CollisionCandidateRuntimeMetrics();
	collisionAwaitingInitialReset = TRUE;
	collisionCurrent.resetEpoch = 33;
	AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
		collisionCurrent, &collisionFrozen, &collisionAwaitingInitialReset);
	CHECK(!collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 0);
	collisionCurrent.authoritativeCommits = 2;
	collisionCurrent.committedCandidates = 7;
	AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
		collisionCurrent, &collisionFrozen, &collisionAwaitingInitialReset);
	CHECK(collisionFrozen.authoritativeCommits == 2 &&
		collisionFrozen.committedCandidates == 7);

	rts::PhysicsIntegrationRuntimeMetrics physicsBaseline;
	rts::PhysicsIntegrationRuntimeMetrics physicsCurrent;
	rts::PhysicsIntegrationRuntimeMetrics physicsFrozen;
	physicsBaseline.resetEpoch = 40;
	physicsBaseline.acceptedBatches = 80;
	physicsCurrent = physicsBaseline;
	Bool physicsAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 0);
	physicsCurrent = rts::PhysicsIntegrationRuntimeMetrics();
	physicsCurrent.resetEpoch = 42; // New-game and scheduler resets may coalesce.
	physicsCurrent.acceptedBatches = 5;
	physicsCurrent.acceptedPrefixes = 96;
	physicsCurrent.acceptedSubmittedJobs = 4;
	physicsCurrent.acceptedCompletedJobs = 4;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(!physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 5 &&
		physicsFrozen.acceptedPrefixes == 96);
	physicsCurrent = rts::PhysicsIntegrationRuntimeMetrics();
	physicsCurrent.resetEpoch = 43;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(physicsFrozen.acceptedBatches == 5 && physicsFrozen.acceptedPrefixes == 96);
	physicsBaseline = physicsCurrent;
	physicsFrozen = rts::PhysicsIntegrationRuntimeMetrics();
	physicsAwaitingInitialReset = TRUE;
	physicsCurrent.resetEpoch = 44;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(!physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 0);
	physicsCurrent.shadowBatches = 2;
	physicsCurrent.shadowPrefixes = 48;
	physicsCurrent.shadowRanges = 4;
	physicsCurrent.shadowSubmittedJobs = 4;
	physicsCurrent.shadowCompletedJobs = 4;
	physicsCurrent.shadowMatches = 2;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(physicsFrozen.acceptedBatches == 0 && physicsFrozen.shadowBatches == 2 &&
		physicsFrozen.shadowPrefixes == 48 && physicsFrozen.shadowMatches == 2);

	rts::ImmutableSpatialRuntimeMetrics spatialBaseline;
	rts::ImmutableSpatialRuntimeMetrics spatialCurrent;
	rts::ImmutableSpatialRuntimeMetrics spatialFrozen;
#if defined(_WIN64)
	const Bool physicsBatchAttempted = FALSE;
	const Bool earlierNormalMoverIsSpatialConsumer = FALSE;
	const Bool followingAutoHealIsSpatialConsumer = TRUE;
	CHECK(!ShouldCaptureLiveImmutableSpatialArena(FALSE,
		earlierNormalMoverIsSpatialConsumer));
	CHECK(!physicsBatchAttempted && ShouldCaptureLiveImmutableSpatialArena(FALSE,
		followingAutoHealIsSpatialConsumer));
	CHECK(!ShouldCaptureLiveImmutableSpatialArena(TRUE, TRUE));
	CHECK(!ShouldCaptureLiveImmutableSpatialArena(FALSE, FALSE));
	unsigned char firstHealToken = 0;
	unsigned char secondHealToken = 0;
	Object *stableHealObjects[2] = {
		reinterpret_cast<Object *>(&firstHealToken),
		reinterpret_cast<Object *>(&secondHealToken)
	};
	StableHealCommitProbe stableHealProbe;
	CommitLiveImmutableSpatialObjectSequence(stableHealObjects, 2,
		&RecordStableHealCommit, &stableHealProbe);
	CHECK(stableHealProbe.count == 2 &&
		stableHealProbe.secondObservedAfterInvalidation &&
		stableHealProbe.objects[0] == stableHealObjects[0] &&
		stableHealProbe.objects[1] == stableHealObjects[1]);
#endif
	spatialBaseline.resetEpoch = 50;
	spatialBaseline.capturedArenas = 90;
	spatialCurrent = spatialBaseline;
	Bool spatialAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
		spatialCurrent, &spatialFrozen, &spatialAwaitingInitialReset);
	CHECK(spatialAwaitingInitialReset && spatialFrozen.capturedArenas == 0 &&
		spatialFrozen.healing.authoritativeQueries == 0);
	spatialCurrent = rts::ImmutableSpatialRuntimeMetrics();
	spatialCurrent.resetEpoch = 51;
	spatialCurrent.capturedArenas = 4;
	spatialCurrent.successfulCollections = 4;
	spatialCurrent.successfulCollectionQueries = 20;
	spatialCurrent.successfulCollectionRanges = 8;
	spatialCurrent.multiRangeCollections = 4;
	spatialCurrent.collectionSubmittedJobs = 16;
	spatialCurrent.collectionCompletedJobs = 16;
	spatialCurrent.collectionPhysicalWorkerJobs = 16;
	spatialCurrent.collectionPhysicalWorkerMask = 3;
	spatialCurrent.maximumCollectionQueries = 5;
	spatialCurrent.maximumCollectionRanges = 2;
	spatialCurrent.maximumCollectionDistinctPhysicalWorkers = 2;
	spatialCurrent.healing.authoritativeQueries = 3;
	spatialCurrent.healing.authoritativeCandidates = 8;
	spatialCurrent.healing.submittedJobs = 6;
	spatialCurrent.healing.completedJobs = 6;
	spatialCurrent.healing.physicalWorkerJobs = 6;
	spatialCurrent.pointDefenseLaser.authoritativeQueries = 2;
	spatialCurrent.pointDefenseLaser.authoritativeCandidates = 5;
	AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
		spatialCurrent, &spatialFrozen, &spatialAwaitingInitialReset);
	CHECK(!spatialAwaitingInitialReset && spatialFrozen.capturedArenas == 4 &&
		spatialFrozen.successfulCollections == 4 &&
		spatialFrozen.successfulCollectionQueries == 20 &&
		spatialFrozen.successfulCollectionRanges == 8 &&
		spatialFrozen.multiRangeCollections == 4 &&
		spatialFrozen.collectionSubmittedJobs == 16 &&
		spatialFrozen.collectionCompletedJobs == 16 &&
		spatialFrozen.collectionPhysicalWorkerJobs == 16 &&
		spatialFrozen.collectionOwnerHelpedJobs == 0 &&
		spatialFrozen.collectionPhysicalWorkerMask == 3 &&
		spatialFrozen.maximumCollectionQueries == 5 &&
		spatialFrozen.maximumCollectionRanges == 2 &&
		spatialFrozen.maximumCollectionDistinctPhysicalWorkers == 2 &&
		spatialFrozen.healing.authoritativeQueries == 3 &&
		spatialFrozen.healing.authoritativeCandidates == 8 &&
		spatialFrozen.healing.physicalWorkerJobs == 6 &&
		spatialFrozen.pointDefenseLaser.authoritativeQueries == 2 &&
		spatialFrozen.pointDefenseLaser.authoritativeCandidates == 5);
	spatialCurrent = rts::ImmutableSpatialRuntimeMetrics();
	spatialCurrent.resetEpoch = 52;
	AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
		spatialCurrent, &spatialFrozen, &spatialAwaitingInitialReset);
	CHECK(spatialFrozen.capturedArenas == 4 &&
		spatialFrozen.successfulCollections == 4 &&
		spatialFrozen.maximumCollectionRanges == 2 &&
		spatialFrozen.collectionPhysicalWorkerMask == 3 &&
		spatialFrozen.maximumCollectionDistinctPhysicalWorkers == 2 &&
		spatialFrozen.healing.authoritativeQueries == 3 &&
		spatialFrozen.pointDefenseLaser.authoritativeQueries == 2);
	spatialBaseline = spatialCurrent;
	spatialFrozen = rts::ImmutableSpatialRuntimeMetrics();
	spatialAwaitingInitialReset = TRUE;
	spatialCurrent.resetEpoch = 53;
	AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
		spatialCurrent, &spatialFrozen, &spatialAwaitingInitialReset);
	CHECK(!spatialAwaitingInitialReset && spatialFrozen.healing.shadowQueries == 0);
	spatialCurrent.healing.shadowQueries = 2;
	spatialCurrent.healing.shadowMatches = 2;
	spatialCurrent.pointDefenseLaser.shadowQueries = 3;
	spatialCurrent.pointDefenseLaser.shadowMatches = 3;
	AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
		spatialCurrent, &spatialFrozen, &spatialAwaitingInitialReset);
	CHECK(spatialFrozen.healing.shadowQueries == 2 &&
		spatialFrozen.healing.shadowMatches == 2 &&
		spatialFrozen.pointDefenseLaser.shadowQueries == 3 &&
		spatialFrozen.pointDefenseLaser.shadowMatches == 3);
#endif

	if (s_failures != 0)
	{
		printf("%d Generals skirmish AI runner contract test(s) failed.\n", s_failures);
		return 1;
	}

	printf("All Generals skirmish AI runner contract tests passed.\n");
	return 0;
}
