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
#include "GameLogic/AIPathfind.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#if defined(_WIN64)
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
	MarkReplayVersionForSkirmishAICurrentEpoch(marked);
	CHECK(CountSkirmishAIReplayMarkers(marked.str(),
		GetSkirmishAIReplayMarkerPrefix()) == 1);
}

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

int main(int argc, char **argv)
{
#if defined(_WIN64)
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
	CHECK(EvaluateSkirmishAITestProgress(42000, 42001) == SKIRMISH_AI_TEST_COMPLETE);
	CHECK(EvaluateSkirmishAITestProgress(0, 108000) == SKIRMISH_AI_TEST_TIMED_OUT);
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
