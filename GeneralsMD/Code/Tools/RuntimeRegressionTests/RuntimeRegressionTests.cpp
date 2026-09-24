/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "GameNetwork/NetCommandValidation.h"
#include "GameNetwork/NetCommandWrapperList.h"
#include "GameNetwork/NetPacket.h"
#include "GameNetwork/NetPacketStructs.h"
#include "GameNetwork/NetCommandRef.h"
#include "GameNetwork/GameSpy/ThreadUtils.h"
#include "Common/FrameRateLimit.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/SkirmishAITestRunner.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "Common/SkirmishAILegacySaveTest.h"
#include "Common/SkirmishAIReplayEpoch.h"
#include "Common/PathfindQueueReplayEpoch.h"
#include "GameClient/GameText.h"
#include "GameLogic/AIPathfind.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include "XferCrcSnapshotTest.h"
#include "GameLogic/SkirmishAIDecision.h"
#include "GameLogic/SkirmishAIRecovery.h"
#include "GameLogic/SkirmishAIStrategy.h"
#include "GameLogic/SkirmishAILiveness.h"
#if defined(_WIN64)
#include "GameLogic/ImmutableSpatialQueryRuntime.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/PhysicsIntegrationKernel.h"
#endif

#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/RailedTransportDockUpdate.h"
#include "WW3D2/textureloader.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

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
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#endif


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
static void TestResolutionArchivePolicy()
{
	CHECK(!ArchiveFileSystem::isArchiveEligibleForResolution(
		"340_ControlBarPro-Fix2160ZH.big", 1920, 1080));
	CHECK(!ArchiveFileSystem::isArchiveEligibleForResolution(
		"H:\\Game\\340_ControlBarPro2160ZH.big", 1920, 1080));
	CHECK(!ArchiveFileSystem::isArchiveEligibleForResolution(
		"340_ControlBarPro-Fix2160ZH.big", 3839, 2160));
	CHECK(!ArchiveFileSystem::isArchiveEligibleForResolution(
		"340_ControlBarPro2160ZH.big", 3840, 2159));
	CHECK(ArchiveFileSystem::isArchiveEligibleForResolution(
		"340_ControlBarPro-Fix2160ZH.big", 3840, 2160));
	CHECK(ArchiveFileSystem::isArchiveEligibleForResolution(
		"H:/Game/340_controlbarpro2160zh.BIG", 3840, 2160));
	CHECK(ArchiveFileSystem::isArchiveEligibleForResolution(
		"340_ControlBarProZH.big", 1920, 1080));
	CHECK(ArchiveFileSystem::isArchiveEligibleForResolution(
		"EnglishZH.big", 1920, 1080));
	CHECK(ArchiveFileSystem::isArchiveEligibleForResolution(
		"Other340_ControlBarPro2160ZH.big", 1920, 1080));
}
#endif

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

static void TestNetworkValidation()
{
	WrappedCommandMetadata metadata;
	metadata.playerID = 1;
	metadata.chunkNumber = 0;
	metadata.numChunks = 1;
	metadata.totalDataLength = 1024;
	metadata.dataLength = 128;
	metadata.dataOffset = 0;

	CHECK(IsValidWrappedCommandMetadata(metadata));

	metadata.numChunks = 0;
	CHECK(!IsValidWrappedCommandMetadata(metadata));

	metadata.numChunks = 1;
	metadata.totalDataLength = MAX_WRAPPED_COMMAND_SIZE + 1;
	CHECK(!IsValidWrappedCommandMetadata(metadata));

	CHECK(!IsValidNetworkPayloadLength(4096, 16, MAX_WRAPPED_COMMAND_SIZE));

	UnsignedInt chunkOffsets[2] = { 0, 4 };
	UnsignedInt chunkLengths[2] = { 4, 4 };
	CHECK(IsCompleteWrappedCommandLayout(chunkOffsets, chunkLengths, 2, 8));

	chunkOffsets[1] = 5;
	CHECK(!IsCompleteWrappedCommandLayout(chunkOffsets, chunkLengths, 2, 8));

	chunkOffsets[1] = 3;
	CHECK(!IsCompleteWrappedCommandLayout(chunkOffsets, chunkLengths, 2, 8));

	CHECK(CanTrackWrappedCommand(0, 0, 0, 128));
	CHECK(!CanTrackWrappedCommand(MAX_WRAPPED_COMMAND_NODES, 0, 0, 128));
	CHECK(!CanTrackWrappedCommand(0, MAX_WRAPPED_COMMAND_NODES_PER_PLAYER, 0, 128));
	CHECK(!CanTrackWrappedCommand(0, 0, MAX_WRAPPED_COMMAND_MEMORY, 128));
	CHECK(!CanTrackWrappedCommand(0, 0, MAX_WRAPPED_COMMAND_MEMORY + 1, 1));

	CHECK(!IsWrappedCommandExpired(1000, 1000));
	CHECK(IsWrappedCommandExpired(1000 + WRAPPED_COMMAND_IDLE_TIMEOUT, 1000));
	CHECK(IsWrappedCommandExpired(10, 10U - WRAPPED_COMMAND_IDLE_TIMEOUT));

	CHECK(IsValidExternalBuffer("x", 1, 1, 1024));
	CHECK(!IsValidExternalBuffer(nullptr, 1, 1, 1024));
	CHECK(!IsValidExternalBuffer("", 0, 1, 1024));
	CHECK(!IsValidExternalBuffer("x", 1025, 1, 1024));

	CHECK(IsValidRunAheadFrameRate(1));
	CHECK(!IsValidRunAheadFrameRate(0));

	UnsignedInt totalArguments = 0;
	size_t payloadBytes = 0;
	CHECK(TryAccumulateGameMessageArguments(ARGUMENTDATATYPE_INTEGER, 250, totalArguments, payloadBytes));
	CHECK(totalArguments == 250);
	CHECK(payloadBytes == 250 * sizeof(Int));
	CHECK(!TryAccumulateGameMessageArguments(ARGUMENTDATATYPE_INTEGER, 6, totalArguments, payloadBytes));
	CHECK(!TryAccumulateGameMessageArguments(ARGUMENTDATATYPE_UNKNOWN, 1, totalArguments, payloadBytes));
	CHECK(!TryAccumulateGameMessageArguments(ARGUMENTDATATYPE_INTEGER, 0, totalArguments, payloadBytes));
}

static void TestPacketRouterFallbackSelection()
{
	UnsignedInt fallback[MAX_SLOTS];
	for (UnsignedInt i = 0; i < MAX_SLOTS; ++i)
		fallback[i] = static_cast<UnsignedInt>(-1);

	fallback[0] = 1;
	fallback[1] = 3;
	fallback[2] = 6;
	CHECK(FindNextPacketRouterSlot(fallback, 1) == 3);
	CHECK(FindNextPacketRouterSlot(fallback, 3) == 6);
	CHECK(FindNextPacketRouterSlot(fallback, 6) == 1);
	CHECK(FindNextPacketRouterSlot(fallback, 4) == static_cast<UnsignedInt>(-1));

	fallback[1] = static_cast<UnsignedInt>(-1);
	fallback[2] = static_cast<UnsignedInt>(-1);
	CHECK(FindNextPacketRouterSlot(fallback, 1) == static_cast<UnsignedInt>(-1));

	CHECK(IsValidPacketRouterSlot(1));
	CHECK(!IsValidPacketRouterSlot(MAX_SLOTS));
	CHECK(!IsValidPacketRouterSlot(static_cast<UnsignedInt>(-1)));
}

static void TestGameCommandParsing()
{
	UnsignedByte truncatedDescriptors[sizeof(Int) + sizeof(UnsignedByte)] = { 0 };
	size_t size = 0;
	size += network::writePrimitive(truncatedDescriptors + size, (Int)GameMessage::MSG_SELECTED_GROUP_COMMAND);
	size += network::writePrimitive(truncatedDescriptors + size, (UnsignedByte)255);
	NetworkGameMessageLayout layout;
	CHECK(!TryParseNetworkGameMessageLayout(truncatedDescriptors, size, layout));

	UnsignedByte invalidCount[sizeof(Int) + 3 * sizeof(UnsignedByte)] = { 0 };
	size = 0;
	size += network::writePrimitive(invalidCount + size, (Int)GameMessage::MSG_SELECTED_GROUP_COMMAND);
	size += network::writePrimitive(invalidCount + size, (UnsignedByte)1);
	size += network::writePrimitive(invalidCount + size, (UnsignedByte)ARGUMENTDATATYPE_INTEGER);
	size += network::writePrimitive(invalidCount + size, (UnsignedByte)0);
	CHECK(!TryParseNetworkGameMessageLayout(invalidCount, size, layout));

	UnsignedByte truncatedPayload[sizeof(Int) + 3 * sizeof(UnsignedByte)] = { 0 };
	size = 0;
	size += network::writePrimitive(truncatedPayload + size, (Int)GameMessage::MSG_SELECTED_GROUP_COMMAND);
	size += network::writePrimitive(truncatedPayload + size, (UnsignedByte)1);
	size += network::writePrimitive(truncatedPayload + size, (UnsignedByte)ARGUMENTDATATYPE_INTEGER);
	size += network::writePrimitive(truncatedPayload + size, (UnsignedByte)1);
	CHECK(!TryParseNetworkGameMessageLayout(truncatedPayload, size, layout));

	UnsignedByte invalidMessageType[sizeof(Int) + sizeof(UnsignedByte)] = { 0 };
	size = 0;
	size += network::writePrimitive(invalidMessageType + size, (Int)GameMessage::MSG_INVALID);
	size += network::writePrimitive(invalidMessageType + size, (UnsignedByte)0);
	CHECK(!TryParseNetworkGameMessageLayout(invalidMessageType, size, layout));

	UnsignedByte valid[sizeof(Int) + 3 * sizeof(UnsignedByte) + sizeof(Int)] = { 0 };
	size = 0;
	size += network::writePrimitive(valid + size, (Int)GameMessage::MSG_SELECTED_GROUP_COMMAND);
	size += network::writePrimitive(valid + size, (UnsignedByte)1);
	size += network::writePrimitive(valid + size, (UnsignedByte)ARGUMENTDATATYPE_INTEGER);
	size += network::writePrimitive(valid + size, (UnsignedByte)1);
	size += network::writePrimitive(valid + size, (Int)42);
	CHECK(TryParseNetworkGameMessageLayout(valid, size, layout));
	CHECK(layout.messageType == GameMessage::MSG_SELECTED_GROUP_COMMAND);
	CHECK(layout.argumentTypeCount == 1);
	CHECK(layout.argumentTypes[0] == ARGUMENTDATATYPE_INTEGER);
	CHECK(layout.argumentCounts[0] == 1);
	CHECK(layout.payloadBytes == sizeof(Int));
}

class StackNetGameCommandMsg : public NetGameCommandMsg
{
public:
	virtual ~StackNetGameCommandMsg() {}
};

class StackNetCommandRef : public NetCommandRef
{
public:
	StackNetCommandRef(NetCommandMsg *msg) : NetCommandRef(msg) {}
	virtual ~StackNetCommandRef() {}
};

static void TestMalformedGameCommandDeserialization()
{
	UnsignedByte truncatedDescriptors[sizeof(Int) + sizeof(UnsignedByte)] = { 0 };
	size_t size = 0;
	size += network::writePrimitive(truncatedDescriptors + size, (Int)GameMessage::MSG_SELECTED_GROUP_COMMAND);
	size += network::writePrimitive(truncatedDescriptors + size, (UnsignedByte)255);

	StackNetGameCommandMsg msg;
	msg.setPlayerID(1);
	StackNetCommandRef ref(&msg);
	CHECK(NetPacketGameCommandData::readMessage(ref, NetPacketBuf(truncatedDescriptors, size)) == size);
	CHECK(msg.getPlayerID() == MAX_SLOTS);
	CHECK(msg.getGameMessageType() == GameMessage::MSG_INVALID);
}

static void TestWrappedCommandRequiresCompleteWireRecord()
{
	UnsignedByte data[sizeof(NetPacketGameCommandBase::CommandBase) + sizeof(Int) + sizeof(UnsignedByte) + 1] = { 0 };
	NetPacketGameCommandBase::CommandBase base;
	base.commandType.commandType = static_cast<UnsignedByte>(NETCOMMANDTYPE_GAMECOMMAND);
	base.frame.frame = 0;
	base.relay.relay = 0;
	base.playerId.playerId = 0;
	base.commandId.commandId = 1;

	size_t size = network::writeObject(data, base);
	size += network::writePrimitive(data + size, static_cast<Int>(GameMessage::MSG_SELECTED_GROUP_COMMAND));
	size += network::writePrimitive(data + size, static_cast<UnsignedByte>(0));

	NetCommandRef *valid = NetPacket::ConstructNetCommandMsgFromRawData(data, static_cast<UnsignedInt>(size));
	CHECK(valid != nullptr);
	if (valid != nullptr)
		deleteInstance(valid);

	NetCommandRef *truncated = NetPacket::ConstructNetCommandMsgFromRawData(data, static_cast<UnsignedInt>(size - 1));
	CHECK(truncated == nullptr);
	if (truncated != nullptr)
		deleteInstance(truncated);

	NetCommandRef *trailing = NetPacket::ConstructNetCommandMsgFromRawData(data, static_cast<UnsignedInt>(size + 1));
	CHECK(trailing == nullptr);
	if (trailing != nullptr)
		deleteInstance(trailing);

	network::writePrimitive(data + sizeof(NetPacketGameCommandBase::CommandBase),
		static_cast<Int>(GameMessage::MSG_INVALID));
	NetCommandRef *invalid = NetPacket::ConstructNetCommandMsgFromRawData(data, static_cast<UnsignedInt>(size));
	CHECK(invalid == nullptr);
	if (invalid != nullptr)
		deleteInstance(invalid);
}

static void TestWrappedCommandRejectsMalformedHeader()
{
	const UnsignedByte unexpectedField[] = { '?' };
	NetCommandRef *unexpected = NetPacket::ConstructNetCommandMsgFromRawData(
		unexpectedField, static_cast<UnsignedInt>(sizeof(unexpectedField)));
	CHECK(unexpected == nullptr);
	if (unexpected != nullptr)
		deleteInstance(unexpected);

	UnsignedByte unknownCommand[sizeof(NetPacketCommandTypeField) + sizeof(NetPacketDataField)] = { 0 };
	NetPacketCommandTypeField commandType;
	commandType.commandType = static_cast<UnsignedByte>(NETCOMMANDTYPE_UNKNOWN);
	NetPacketDataField dataHeader;
	size_t size = network::writeObject(unknownCommand, commandType);
	size += network::writeObject(unknownCommand + size, dataHeader);
	NetCommandRef *unknown = NetPacket::ConstructNetCommandMsgFromRawData(
		unknownCommand, static_cast<UnsignedInt>(size));
	CHECK(unknown == nullptr);
	if (unknown != nullptr)
		deleteInstance(unknown);
}

static NetWrapperCommandMsg *CreateWrapperMessage(UnsignedByte playerID, UnsignedShort commandID,
	UnsignedInt chunkNumber, UnsignedInt numChunks, UnsignedInt totalLength, UnsignedInt dataOffset)
{
	NetWrapperCommandMsg *msg = newInstance(NetWrapperCommandMsg)();
	msg->setPlayerID(playerID);
	msg->setWrappedCommandID(commandID);
	msg->setChunkNumber(chunkNumber);
	msg->setNumChunks(numChunks);
	msg->setTotalDataLength(totalLength);
	msg->setDataOffset(dataOffset);
	NetCommandDataChunk data(1);
	data.data()[0] = static_cast<UnsignedByte>(commandID);
	msg->setData(data);
	return msg;
}

static Bool ProcessWrapper(NetCommandWrapperList &list, NetWrapperCommandMsg *msg)
{
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	msg->detach();
	const Bool accepted = list.processWrapper(ref);
	deleteInstance(ref);
	return accepted;
}

static void TestWrapperLifecycle()
{
	NetCommandWrapperList *list = newInstance(NetCommandWrapperList)();
	CHECK(ProcessWrapper(*list, CreateWrapperMessage(1, 1, 0, 2, 3, 0)));
	CHECK(list->getNodeCount() == 1);
	const UnsignedInt firstAllocation = list->getAllocatedBytes();

	CHECK(ProcessWrapper(*list, CreateWrapperMessage(1, 1, 0, 2, 3, 0)));
	CHECK(list->getNodeCount() == 1);
	CHECK(list->getAllocatedBytes() == firstAllocation);

	CHECK(!ProcessWrapper(*list, CreateWrapperMessage(1, 1, 0, 2, 3, 1)));
	NetWrapperCommandMsg *conflictingData = CreateWrapperMessage(1, 1, 0, 2, 3, 0);
	NetCommandDataChunk replacementData(1);
	replacementData.data()[0] = 99;
	conflictingData->setData(replacementData);
	CHECK(!ProcessWrapper(*list, conflictingData));
	NetWrapperCommandMsg *conflictingLength = CreateWrapperMessage(1, 1, 0, 2, 3, 0);
	NetCommandDataChunk longerData(2);
	longerData.data()[0] = 1;
	longerData.data()[1] = 1;
	conflictingLength->setData(longerData);
	CHECK(!ProcessWrapper(*list, conflictingLength));
	CHECK(list->getNodeCount() == 1);
	CHECK(list->getAllocatedBytes() == firstAllocation);

	// Active transfer metadata is immutable, preventing allocation churn.
	CHECK(!ProcessWrapper(*list, CreateWrapperMessage(1, 1, 0, 3, 3, 0)));
	CHECK(list->getNodeCount() == 1);
	CHECK(list->getAllocatedBytes() == firstAllocation);

	// Once expired, the command ID can be reused with new metadata.
	list->purgeExpired(timeGetTime() + WRAPPED_COMMAND_IDLE_TIMEOUT);
	CHECK(list->getNodeCount() == 0);
	CHECK(ProcessWrapper(*list, CreateWrapperMessage(1, 1, 0, 3, 3, 0)));
	CHECK(list->getNodeCount() == 1);

	list->reset();
	for (UnsignedShort commandID = 0; commandID < MAX_WRAPPED_COMMAND_NODES_PER_PLAYER; ++commandID)
		CHECK(ProcessWrapper(*list, CreateWrapperMessage(2, commandID, 0, 2, 2, 0)));
	CHECK(!ProcessWrapper(*list,
		CreateWrapperMessage(2, MAX_WRAPPED_COMMAND_NODES_PER_PLAYER, 0, 2, 2, 0)));
	CHECK(list->getNodeCount() == MAX_WRAPPED_COMMAND_NODES_PER_PLAYER);
	list->removeForPlayer(2);
	CHECK(list->getNodeCount() == 0);
	CHECK(list->getAllocatedBytes() == 0);

	// A complete but malformed reconstructed command is discarded safely.
	CHECK(ProcessWrapper(*list, CreateWrapperMessage(3, 9, 0, 1, 1, 0)));
	NetCommandList *ready = list->getReadyCommands();
	CHECK(ready->length() == 0);
	CHECK(list->getNodeCount() == 0);
	deleteInstance(ready);
	deleteInstance(list);
}

static void TestStringConversionAndZeroLengthReads()
{
	CHECK(MultiByteToWideCharSingleLine("alpha\nbeta\r") == L"alpha beta ");
	CHECK(MultiByteToWideCharSingleLine(nullptr).empty());
	CHECK(WideCharStringToMultiByte(L"snowman \x2603") == "snowman \xE2\x98\x83");
	CHECK(WideCharStringToMultiByte(nullptr).empty());
	CHECK(network::readBytes(nullptr, 0, NetPacketBuf(nullptr, 0)) == 0);
}

static void TestNetworkReceiveBudget()
{
	CHECK(ShouldReceiveNetworkMessage(0, TRUE));
	CHECK(ShouldReceiveNetworkMessage(MAX_MESSAGES - 1, TRUE));
	CHECK(!ShouldReceiveNetworkMessage(MAX_MESSAGES, TRUE));
	CHECK(!ShouldReceiveNetworkMessage(0, FALSE));
}

static ULONGLONG FileTimeToTicks(const FILETIME &fileTime)
{
	ULARGE_INTEGER ticks;
	ticks.LowPart = fileTime.dwLowDateTime;
	ticks.HighPart = fileTime.dwHighDateTime;
	return ticks.QuadPart;
}

static void TestFrameRateLimitCpuUsage()
{
	FILETIME createTime;
	FILETIME exitTime;
	FILETIME kernelStart;
	FILETIME userStart;
	FILETIME kernelEnd;
	FILETIME userEnd;
	LARGE_INTEGER frequency;
	LARGE_INTEGER wallStart;
	LARGE_INTEGER wallEnd;

	if (!GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelStart, &userStart) ||
		!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&wallStart))
	{
		CHECK(FALSE);
		return;
	}

	FrameRateLimit limiter;
	for (Int i = 0; i < 240; ++i)
		limiter.wait(480);

	if (!QueryPerformanceCounter(&wallEnd) ||
		!GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelEnd, &userEnd))
	{
		CHECK(FALSE);
		return;
	}

	const double wallSeconds = static_cast<double>(wallEnd.QuadPart - wallStart.QuadPart) / frequency.QuadPart;
	const Int64 cpuTicks =
		static_cast<Int64>(FileTimeToTicks(kernelEnd)) - static_cast<Int64>(FileTimeToTicks(kernelStart)) +
		static_cast<Int64>(FileTimeToTicks(userEnd)) - static_cast<Int64>(FileTimeToTicks(userStart));
	const double cpuSeconds = static_cast<double>(cpuTicks) / 10000000.0;
	const double cpuRatio = cpuSeconds / wallSeconds;
	printf("Frame limiter CPU ratio at 480 FPS: %.3f\n", cpuRatio);
	CHECK(wallSeconds >= 0.45);
	// Leave ample headroom for transient hosted-runner preemption while still
	// catching pathological stalls in the frame limiter.
	CHECK(wallSeconds < 1.50);
	CHECK(cpuRatio < 0.60);
}

static void TestFrameRateLimitWaitCalculation()
{
	CHECK(FrameRateLimit::calculateCoarseWaitTicks(1000, 200) == 800);
	CHECK(FrameRateLimit::calculateCoarseWaitTicks(200, 200) == 0);
	CHECK(FrameRateLimit::calculateCoarseWaitTicks(100, 200) == 0);
	CHECK(FrameRateLimit::calculateCoarseWaitTicks(-1, 200) == 0);
}

static void TestSkirmishAILivenessPolicies()
{
	CHECK(ShouldUseSkirmishAILivenessRecovery(false, false));
	CHECK(!ShouldUseSkirmishAILivenessRecovery(true, false));
	CHECK(ShouldUseSkirmishAILivenessRecovery(true, true));

	CHECK(GetPathQueueRetryDelay(true) == 0);
	CHECK(GetPathQueueRetryDelay(false) == 1);

	CHECK(IsSkirmishWaypointCandidateBetter(false, 0, false, 0.0f, 0, false, 0.0f));
	CHECK(IsSkirmishWaypointCandidateBetter(true, 0, false, 100.0f, 2, true, 400.0f));
	CHECK(!IsSkirmishWaypointCandidateBetter(true, 2, true, 400.0f, 0, false, 100.0f));
	CHECK(IsSkirmishWaypointCandidateBetter(true, 2, true, 400.0f, 2, true, 100.0f));
	CHECK(IsSkirmishWaypointCandidateBetter(true, 0, false, 100.0f, 1, false, 400.0f));
	CHECK(GetSkirmishWaypointFallbackPriority(false, false, 1) == 2);
	CHECK(GetSkirmishWaypointFallbackPriority(true, true, 1) == 1);
	CHECK(GetSkirmishWaypointFallbackPriority(true, true, 2) == 0);
	CHECK(GetSkirmishWaypointFallbackPriority(true, false, 0) == 0);

	CHECK(IsWorkOrderFactoryQueueValid(true, true, 1));
	CHECK(!IsWorkOrderFactoryQueueValid(false, true, 1));
	CHECK(!IsWorkOrderFactoryQueueValid(true, false, 1));
	CHECK(!IsWorkOrderFactoryQueueValid(true, true, 0));

	CHECK(IsUsableSupplyCenter(true, false));
	CHECK(!IsUsableSupplyCenter(false, false));
	CHECK(!IsUsableSupplyCenter(true, true));
}

static SkirmishAIRecoveryPolicyInput MakeSkirmishAIRecoveryPolicyInput()
{
	SkirmishAIRecoveryPolicyInput input;
	input.enabled = true;
	input.everCompleted = true;
	input.hasPrimaryCommandCenter = false;
	input.hasConstruction = false;
	input.hasBuilder = false;
	input.builderQueued = false;
	input.builderQueuePaid = false;
	input.hasBuilderFactory = true;
	input.noBuilderPath = false;
	input.builderAffordable = true;
	input.commandCenterAffordable = true;
	input.placementReady = true;
	input.commandCenterCost = 1200;
	input.builderCost = 400;
	input.protectedReserve = 300;
	return input;
}

static SkirmishStrategyMetrics MakeSkirmishStrategyMetrics()
{
	SkirmishStrategyMetrics metrics;
	metrics.economyHealth = 100;
	metrics.baseIntegrity = 100;
	metrics.armyReadiness = 100;
	metrics.immediateThreat = 0;
	metrics.attackConfidence = 100;
	metrics.enemyOpportunity = 100;
	metrics.alliedDistress = 0;
	metrics.availableCombatValue = 10000;
	metrics.hasStrategicTarget = true;
	metrics.assaultLostHalfForce = false;
	metrics.assaultObjectiveComplete = false;
	metrics.viableAssaultForceAssembled = false;
	return metrics;
}

static void TestSkirmishAIStrategyPolicies()
{
	SkirmishStrategyMetrics metrics = MakeSkirmishStrategyMetrics();
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 0);
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 10000);

	metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 0;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 3000);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.economyHealth = 0;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 2500);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.immediateThreat = 100;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 2000);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.armyReadiness = 0;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 1500);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.attackConfidence = 0;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 1000);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.economyHealth = 200;
	metrics.baseIntegrity = -20;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 3000);

	metrics = MakeSkirmishStrategyMetrics();
	metrics.economyHealth = 0;
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 7500);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 0;
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 8000);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.armyReadiness = 0;
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 7500);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.attackConfidence = 0;
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 8500);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.enemyOpportunity = 0;
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 8500);

	SkirmishStrategyState state;
	InitializeOldSaveSkirmishStrategyState(&state, 0);
	CHECK(state.currentMode == SKIRMISH_STRATEGY_BALANCED);
	CHECK(state.pendingMode == SKIRMISH_STRATEGY_NONE);
	CHECK(state.nextEvaluationFrame == 0);
	CHECK(state.strategicTargetID == INVALID_ID);
	CHECK(!state.strategicTargetObserved);
	CHECK(state.strategicTargetLastSeenFrame == 0);
	CHECK(HasSkirmishStrategyDurationElapsed(
		0, state.modeEntryFrame, 90 * LOGICFRAMES_PER_SECOND));
	InitializeSkirmishStrategyState(&state, 0);
	CHECK(!HasSkirmishStrategyDurationElapsed(
		0, state.modeEntryFrame, 90 * LOGICFRAMES_PER_SECOND));
	CHECK(IsSkirmishStrategyTargetObservationAvailable(
		true, 29 * LOGICFRAMES_PER_SECOND, 0));
	CHECK(!IsSkirmishStrategyTargetObservationAvailable(
		true, 30 * LOGICFRAMES_PER_SECOND, 0));
	CHECK(!IsSkirmishStrategyTargetObservationAvailable(false, 0, 0));
	CHECK(IsSkirmishStrategyTargetObservationAvailable(
		true, 5, UINT_MAX - 5));

	metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 34;
	SkirmishStrategyDecision decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.modeChanged);
	CHECK(decision.nextState.currentMode == SKIRMISH_STRATEGY_FORTIFY);
	CHECK(decision.reason == SKIRMISH_STRATEGY_REASON_FORTIFY_CRITICAL_BASE);
	metrics.baseIntegrity = 35;
	metrics.immediateThreat = 85;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.modeChanged);
	CHECK(decision.reason == SKIRMISH_STRATEGY_REASON_FORTIFY_SEVERE_THREAT);

	InitializeOldSaveSkirmishStrategyState(&state, 0);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 35;
	metrics.economyHealth = 0;
	metrics.armyReadiness = 0;
	metrics.attackConfidence = 0;
	CHECK(CalculateSkirmishFortifyPressure(metrics) == 6950);
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_FORTIFY);
	CHECK(!decision.modeChanged);
	state = decision.nextState;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.immediateThreat = 60;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 5 * LOGICFRAMES_PER_SECOND);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_NONE);
	CHECK(decision.reason == SKIRMISH_STRATEGY_REASON_PENDING_CLEARED);

	InitializeOldSaveSkirmishStrategyState(&state, 0);
	metrics = MakeSkirmishStrategyMetrics();
	metrics.economyHealth = 50;
	metrics.baseIntegrity = 65;
	metrics.armyReadiness = 60;
	metrics.attackConfidence = 100;
	metrics.enemyOpportunity = 100;
	metrics.immediateThreat = 59;
	CHECK(CalculateSkirmishAssaultReadiness(metrics) == 7050);
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_ASSAULT);
	state = decision.nextState;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 10 * LOGICFRAMES_PER_SECOND - 1);
	CHECK(!decision.modeChanged);
	state.nextEvaluationFrame = 10 * LOGICFRAMES_PER_SECOND;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 10 * LOGICFRAMES_PER_SECOND);
	CHECK(decision.modeChanged);
	CHECK(decision.nextState.currentMode == SKIRMISH_STRATEGY_ASSAULT);
	metrics.immediateThreat = 60;
	InitializeOldSaveSkirmishStrategyState(&state, 0);
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode != SKIRMISH_STRATEGY_ASSAULT);
	metrics.immediateThreat = 59;
	metrics.hasStrategicTarget = false;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode != SKIRMISH_STRATEGY_ASSAULT);

	InitializeOldSaveSkirmishStrategyState(&state, 0);
	state.currentMode = SKIRMISH_STRATEGY_FORTIFY;
	state.modeEntryFrame = 0 - 120 * LOGICFRAMES_PER_SECOND;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.enemyOpportunity = 0;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_BALANCED);
	state = decision.nextState;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 10 * LOGICFRAMES_PER_SECOND);
	CHECK(decision.nextState.currentMode == SKIRMISH_STRATEGY_BALANCED);
	InitializeOldSaveSkirmishStrategyState(&state, 0);
	state.currentMode = SKIRMISH_STRATEGY_ASSAULT;
	state.strategicTargetID = (ObjectID)123;
	state.strategicTargetObserved = true;
	state.strategicTargetLastSeenFrame = 0;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.assaultObjectiveComplete = true;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.modeChanged);
	CHECK(decision.nextState.currentMode == SKIRMISH_STRATEGY_BALANCED);
	CHECK(decision.nextState.strategicTargetID == INVALID_ID);
	CHECK(!decision.nextState.strategicTargetObserved);
	CHECK(decision.nextState.strategicTargetLastSeenFrame == 0);
	InitializeOldSaveSkirmishStrategyState(&state, 0);
	state.currentMode = SKIRMISH_STRATEGY_ASSAULT;
	state.modeEntryFrame = 0 - 90 * LOGICFRAMES_PER_SECOND;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.assaultLostHalfForce = true;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_BALANCED);

	InitializeSkirmishStrategyState(&state, 0);
	state.currentMode = SKIRMISH_STRATEGY_ASSAULT;
	state.strategicTargetID = (ObjectID)123;
	state.strategicTargetObserved = true;
	state.strategicTargetLastSeenFrame = 0;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.hasStrategicTarget = false;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(!decision.modeChanged);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_BALANCED);
	CHECK(decision.reason == SKIRMISH_STRATEGY_REASON_PENDING_STARTED);
	state = decision.nextState;
	state.nextEvaluationFrame = 10 * LOGICFRAMES_PER_SECOND;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 10 * LOGICFRAMES_PER_SECOND);
	CHECK(!decision.modeChanged);
	CHECK(decision.reason == SKIRMISH_STRATEGY_REASON_MINIMUM_DURATION);
	state = decision.nextState;
	state.nextEvaluationFrame = 90 * LOGICFRAMES_PER_SECOND;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 90 * LOGICFRAMES_PER_SECOND);
	CHECK(decision.modeChanged);
	CHECK(decision.nextState.currentMode == SKIRMISH_STRATEGY_BALANCED);
	CHECK(decision.reason ==
		SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_TARGET_UNAVAILABLE);
	CHECK(decision.nextState.strategicTargetID == INVALID_ID);
	CHECK(!decision.nextState.strategicTargetObserved);
	CHECK(decision.nextState.strategicTargetLastSeenFrame == 0);

	InitializeOldSaveSkirmishStrategyState(&state, 0);
	state.currentMode = SKIRMISH_STRATEGY_ASSAULT;
	state.strategicTargetID = (ObjectID)123;
	state.strategicTargetObserved = true;
	state.strategicTargetLastSeenFrame = 0;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.hasStrategicTarget = false;
	decision = EvaluateSkirmishStrategy(state, metrics, DIFFICULTY_HARD, 0);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_BALANCED);
	state = decision.nextState;
	metrics.hasStrategicTarget = true;
	state.nextEvaluationFrame = 5 * LOGICFRAMES_PER_SECOND;
	decision = EvaluateSkirmishStrategy(
		state, metrics, DIFFICULTY_HARD, 5 * LOGICFRAMES_PER_SECOND);
	CHECK(!decision.modeChanged);
	CHECK(decision.nextState.pendingMode == SKIRMISH_STRATEGY_NONE);
	CHECK(decision.reason == SKIRMISH_STRATEGY_REASON_PENDING_CLEARED);
	CHECK(decision.nextState.strategicTargetID == (ObjectID)123);
	CHECK(decision.nextState.strategicTargetObserved);
	CHECK(decision.nextState.strategicTargetLastSeenFrame == 0);

	CHECK(!IsSkirmishStrategyFrameReached(UINT_MAX - 3, 5));
	CHECK(IsSkirmishStrategyFrameReached(5, 5));
	CHECK(HasSkirmishStrategyDurationElapsed(5, UINT_MAX - 5, 11));
}

static void CheckSkirmishAIRecoveryDecision(
	const SkirmishAIRecoveryPolicyInput &input,
	Bool expectedQueueBuilder, Bool expectedConstructCommandCenter,
	Bool expectedRecoveryImpossible, Bool expectedRetry, Int expectedReserveCost)
{
	const SkirmishAIRecoveryPolicyResult result = DecideSkirmishAIRecovery(input);
	CHECK(result.shouldQueueBuilder == expectedQueueBuilder);
	CHECK(result.shouldConstructCommandCenter == expectedConstructCommandCenter);
	CHECK(result.recoveryImpossible == expectedRecoveryImpossible);
	CHECK(result.shouldRetry == expectedRetry);
	CHECK(result.reserveCost == expectedReserveCost);
}

static void TestSkirmishAIRecoveryPolicies()
{
	SkirmishAIRecoveryPolicyInput input = MakeSkirmishAIRecoveryPolicyInput();
	CHECK(IsSkirmishAIRecoveryLastStandCombatCandidate(
		true, false, false, false, true));
	CHECK(!IsSkirmishAIRecoveryLastStandCombatCandidate(
		true, false, true, false, true));
	CHECK(!IsSkirmishAIRecoveryLastStandCombatCandidate(
		true, false, false, true, true));
	CHECK(!IsSkirmishAIRecoveryLastStandCombatCandidate(
		true, true, false, false, true));

	// Disabled recovery, an AI that never completed a command center, an
	// existing command center, and a command center scaffold must not impose
	// a recovery reserve on ordinary construction or production.
	input.enabled = false;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, FALSE, 0);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.everCompleted = false;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, FALSE, 0);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasPrimaryCommandCenter = true;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, FALSE, 0);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasConstruction = true;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, FALSE, 0);

	// A completed command center that is now missing still needs recovery. If
	// a builder survives, the next legal action is the command center build.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasBuilder = true;
	input.protectedReserve = 300;
	CheckSkirmishAIRecoveryDecision(input, FALSE, TRUE, FALSE, FALSE, 1200);

	// Builder-first recovery is allowed when only the builder is affordable;
	// the reserve still accounts for the later command center and builder cost.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.commandCenterAffordable = false;
	input.protectedReserve = 0;
	CHECK(GetSkirmishAIRecoveryReserveCost(input) == 1600);
	CheckSkirmishAIRecoveryDecision(input, TRUE, FALSE, FALSE, FALSE, 1600);
	input.hasBuilder = true;
	CHECK(GetSkirmishAIRecoveryReserveCost(input) == 1200);

	// Lack of cash and a temporarily unavailable builder factory are retryable
	// states, not permanent recovery failure.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.builderAffordable = false;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1600);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasBuilderFactory = false;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1600);

	// A contained builder is not usable. During its bounded evacuation grace it
	// remains a recovery route; afterward a factory may queue a replacement, or
	// recovery becomes impossible when no other physical route survives.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.noBuilderPath = IsSkirmishAIRecoveryBuilderPathUnavailable(
		false, true, false, true);
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, TRUE, FALSE, FALSE, FALSE, 1600);
	input.hasBuilderFactory = false;
	input.noBuilderPath = IsSkirmishAIRecoveryBuilderPathUnavailable(
		false, true, false, false);
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1600);
	CHECK(IsSkirmishAIRecoveryBuilderPathUnavailable(
		false, false, false, false));
	CHECK(!IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
		true, false, false, false));
	CHECK(IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
		true, false, false, true));
	CHECK(IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
		false, true, false, true));
	CHECK(IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
		false, false, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
		false, true, false, false));
	CHECK(!IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
		false, false, true, false));
	CHECK(IsSkirmishAIRecoveryFactorySchedulingBounded(
		true, false, false, false));
	CHECK(IsSkirmishAIRecoveryFactorySchedulingBounded(
		false, true, false, false));
	CHECK(IsSkirmishAIRecoveryFactorySchedulingBounded(
		false, false, true, false));
	CHECK(!IsSkirmishAIRecoveryFactorySchedulingBounded(
		true, false, false, true));
	// A scheduler-blocked lower-ID CANMAKE_OK factory cannot outrank a healthy
	// higher-ID candidate merely because deterministic selection sees it first.
	CHECK(!IsSkirmishAIRecoveryFactoryBestCandidate(true, false));
	CHECK(IsSkirmishAIRecoveryFactoryBestCandidate(true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryBestCandidate(false, true));
	CHECK(IsSkirmishAIRecoveryBuilderAdmissionActionable(true, true));
	CHECK(!IsSkirmishAIRecoveryBuilderAdmissionActionable(true, false));
	CHECK(!IsSkirmishAIRecoveryBuilderAdmissionActionable(false, true));
	CHECK(IsSkirmishAIRecoveryBuilderUpdateBounded(true, false));
	CHECK(!IsSkirmishAIRecoveryBuilderUpdateBounded(true, true));
	CHECK(!IsSkirmishAIRecoveryBuilderUpdateBounded(false, false));
	CHECK(IsSkirmishAIRecoveryAdmissionBounded(
		true, false, false, false));
	CHECK(IsSkirmishAIRecoveryAdmissionBounded(
		false, true, false, false));
	CHECK(IsSkirmishAIRecoveryAdmissionBounded(
		false, false, true, false));
	CHECK(IsSkirmishAIRecoveryAdmissionBounded(
		false, false, false, true));
	CHECK(!IsSkirmishAIRecoveryAdmissionBounded(
		false, false, false, false));
	CHECK(IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		true, false, false));
	CHECK(IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		true, true, false));
	CHECK(IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		false, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		false, true, false));
	CHECK(!IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		false, false, true));
	CHECK(!ShouldLatchSkirmishAIRecoveryLastStand(true));
	CHECK(ShouldLatchSkirmishAIRecoveryLastStand(false));
	CHECK(ShouldPreserveSkirmishAIRecoveryTrackedObject(
		false, true, true, false));
	CHECK(ShouldPreserveSkirmishAIRecoveryTrackedObject(
		false, true, false, true));
	CHECK(!ShouldPreserveSkirmishAIRecoveryTrackedObject(
		true, true, true, false));
	CHECK(!ShouldPreserveSkirmishAIRecoveryTrackedObject(
		false, false, true, false));
	CHECK(!ShouldPreserveSkirmishAIRecoveryTrackedObject(
		false, true, false, false));
	CHECK(!IsSkirmishAIRecoveryPaidQueueBounded(
		false, true, false));
	CHECK(!IsSkirmishAIRecoveryPaidQueueBounded(
		true, true, true));
	CHECK(IsSkirmishAIRecoveryPaidQueueBounded(
		true, false, true));
	CHECK(IsSkirmishAIRecoveryPaidQueueBounded(
		true, true, false));
	CHECK(ShouldFailoverSkirmishAIRecoveryPaidQueue(
		true, false, false, true, false));
	CHECK(!ShouldFailoverSkirmishAIRecoveryPaidQueue(
		true, true, false, true, false));
	CHECK(!ShouldFailoverSkirmishAIRecoveryPaidQueue(
		true, false, true, true, false));
	CHECK(!ShouldFailoverSkirmishAIRecoveryPaidQueue(
		true, false, false, false, false));
	CHECK(!ShouldFailoverSkirmishAIRecoveryPaidQueue(
		true, false, false, true, true));
	CHECK(!ShouldFailoverSkirmishAIRecoveryPaidQueue(
		false, false, false, true, false));
	CHECK(ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, false, true, false));
	CHECK(!ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, true, false, true, false));
	CHECK(!ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, true, true, false));
	// Epoch 7 consumes the one bounded-queue failover only after an alternate
	// commits. Epoch 6 ignores the new persisted bound during playback.
	CHECK(!ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, false, true, true));
	CHECK(ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, false, false, true));
	CHECK(!GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
		false, false));
	CHECK(GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
		false, true));
	CHECK(GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
		true, false));
	CHECK(ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
		true, true, false));
	const bool completedEpisodeConsumed =
		ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
			true, true, true);
	CHECK(!completedEpisodeConsumed);
	CHECK(!ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
		completedEpisodeConsumed, true, false));
	CHECK(!ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
		true, false, false));
	// A bounded paid entry that failed failover is not progress. If an exact
	// unusable native worker is blocking respawn, disposition must reach RESET;
	// other native states retain the existing retryable bounded-queue behavior.
	CHECK(!ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
		true, false, true));
	CHECK(ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
		true, false, false));
	CHECK(ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
		true, true, true));
	CHECK(!ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
		false, false, false));
	CHECK(IsSkirmishAIRecoveryFailoverAdmissionEligible(
		true, false, false, false, false, false, false));
	CHECK(IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, true, false, true, false, false, false));
	CHECK(!IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, true, false, false, false, false, false));
	CHECK(IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, false, true, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, false, true, true, false, true, true));
	CHECK(!IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, false, true, true, true, false, true));
	CHECK(!IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, false, true, true, true, true, false));
	CHECK(!IsSkirmishAIRecoveryFailoverAdmissionEligible(
		false, false, true, false, true, true, true));
	// An unowned bounded queue cannot contribute a refund or free a max slot.
	// Its alternate must be independently affordable and immediately admissible.
	CHECK(IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
		true, true, true));
	CHECK(!IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
		false, true, true));
	CHECK(!IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
		true, false, true));
	CHECK(!IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
		true, true, false));
	CHECK(GetSkirmishAIRecoveryFailoverAdmissionRank(
		true, false, false) == 0);
	CHECK(GetSkirmishAIRecoveryFailoverAdmissionRank(
		false, true, false) == 1);
	CHECK(GetSkirmishAIRecoveryFailoverAdmissionRank(
		false, false, true) == 2);
	CHECK(ShouldClearSkirmishAIRecoveryFailoverBinding(
		true, true, true));
	CHECK(!ShouldClearSkirmishAIRecoveryFailoverBinding(
		false, true, true));
	CHECK(!ShouldClearSkirmishAIRecoveryFailoverBinding(
		true, false, true));
	CHECK(!ShouldClearSkirmishAIRecoveryFailoverBinding(
		true, true, false));
	CHECK(WouldSkirmishAIRecoveryCancellationFreeMax(1, 0));
	CHECK(!WouldSkirmishAIRecoveryCancellationFreeMax(1, 1));
	CHECK(!WouldSkirmishAIRecoveryCancellationFreeMax(2, 2));
	CHECK(WouldSkirmishAIRecoveryCancellationFreeMax(2, 1));
	CHECK(!WouldSkirmishAIRecoveryCancellationFreeMax(0, 0));
	CHECK(ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
		false, false, false));
	CHECK(ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
		true, false, true));
	CHECK(!ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
		true, true, false));
	CHECK(!ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
		true, false, false));
	CHECK(!ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
		true, true, true));
	// A progressing ordinary compatible queue on B outranks bounded tracked A,
	// so recovery waits for B and cannot enter failover or pay an alternate.
	CHECK(ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
		true, false, true));
	CHECK(!ShouldPreferTrackedSkirmishAIRecoveryPaidQueue(true, true));
	const bool progressingOrdinaryQueueBounded =
		IsSkirmishAIRecoveryPaidQueueBounded(true, true, true);
	CHECK(!progressingOrdinaryQueueBounded);
	CHECK(!ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		progressingOrdinaryQueueBounded, false, false, true, false));
	// When every compatible queue is bounded, exact tracked A wins over the
	// deterministic ordinary B fallback. Refunding A before paying C preserves
	// both cash and the number of paid compatible entries.
	CHECK(ShouldPreferTrackedSkirmishAIRecoveryPaidQueue(true, false));
	CHECK(!ShouldPreferTrackedSkirmishAIRecoveryPaidQueue(false, false));
	const int boundedPaidEntriesBeforeFailover = 2;
	const int trackedBuilderCost = 200;
	const int boundedCashBeforeFailover = 1000;
	const int boundedCashAfterRefund = AddSkirmishAIRecoveryCost(
		boundedCashBeforeFailover, trackedBuilderCost);
	const int boundedCashAfterAlternate =
		boundedCashAfterRefund - trackedBuilderCost;
	const int boundedPaidEntriesAfterFailover =
		boundedPaidEntriesBeforeFailover - 1 + 1;
	CHECK(boundedCashAfterAlternate == boundedCashBeforeFailover);
	CHECK(boundedPaidEntriesAfterFailover ==
		boundedPaidEntriesBeforeFailover);
	CHECK(ShouldClearSkirmishAIRecoveryExactFailoverBinding(
		true, true, true, true));
	CHECK(!ShouldClearSkirmishAIRecoveryExactFailoverBinding(
		false, true, true, true));
	// A completed equivalent center is authoritative over an older scaffold.
	// Within the same construction state, keep the lowest ObjectID so iteration
	// order cannot affect simulation state.
	CHECK(ShouldSelectSkirmishAIPrimaryCommandCenter(
		true, false, 3, true, 6));
	CHECK(!ShouldSelectSkirmishAIPrimaryCommandCenter(
		true, true, 6, false, 3));
	CHECK(ShouldSelectSkirmishAIPrimaryCommandCenter(
		true, true, 6, true, 3));
	CHECK(!ShouldSelectSkirmishAIPrimaryCommandCenter(
		true, false, 3, false, 6));
	CHECK(!ShouldSelectSkirmishAIPrimaryCommandCenter(
		true, true, 3, true, 3));
	const bool validContainedBuilderRoute =
		IsSkirmishAIRecoveryContainedBuilderRoute(
			true, true, true, true, true, true);
	CHECK(validContainedBuilderRoute);
	CHECK(!IsSkirmishAIRecoveryContainedBuilderRoute(
		true, false, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryContainedBuilderRoute(
		true, true, false, true, true, true));
	CHECK(!IsSkirmishAIRecoveryContainedBuilderRoute(
		true, true, true, false, true, true));
	CHECK(!IsSkirmishAIRecoveryContainedBuilderRoute(
		true, true, true, true, false, true));
	CHECK(!IsSkirmishAIRecoveryContainedBuilderRoute(
		true, true, true, true, true, false));
	CHECK(ShouldOrderSkirmishAIRecoveryBuilderExit(
		validContainedBuilderRoute, false));
	// Production state is deliberately absent from the exit policy. A tunnel
	// upgrade therefore cannot suppress recovery evacuation.
	CHECK(!ShouldOrderSkirmishAIRecoveryBuilderExit(
		validContainedBuilderRoute, true));
	CHECK(IsSkirmishAIRecoveryInsuranceBuilderCandidate(
		false, true, false));
	CHECK(!IsSkirmishAIRecoveryInsuranceBuilderCandidate(
		true, true, false));
	CHECK(!IsSkirmishAIRecoveryInsuranceBuilderCandidate(
		false, false, false));
	CHECK(!IsSkirmishAIRecoveryInsuranceBuilderCandidate(
		false, true, true));
	// A paid recovery worker is authoritative while the center is missing, so a
	// resource WorkOrder cannot pay a duplicate when its producer resumes. An
	// unpaid resource order remains an income route and the completed center
	// restores normal admission. Non-resource builders retain reserve gating.
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false, 200, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false, 200, false));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, true, 200, true));
	const bool underConstructionCenterIsCompleted = false;
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false,
		underConstructionCenterIsCompleted, 200, true));
	const bool completedPrimaryCenter = true;
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false,
		completedPrimaryCenter, 200, true));
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		false, true, true, true, false, false, 200, false));
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		false, true, true, true, false, false, 0, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, false, true, true, false, false, 200, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, false, true, false, false, 200, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, false, false, false, 200, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, true, false, 200, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		false, true, true, true, false, false, 0, false));
	// Compose the low-cash GLA resource-worker route across ordinary admission,
	// recovery payment, completion routing, and later command-center admission.
	// The ordinary resource order remains visible, but the reserve rejects its
	// unowned purchase; recovery then pays exactly one worker and reuses it.
	const Int resourceWorkerCost = 400;
	const Int resourceCommandCenterCost = 1200;
	Int resourceRecoveryCash = resourceWorkerCost;
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false,
		resourceWorkerCost + resourceCommandCenterCost, false));
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.commandCenterAffordable = false;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(
		input, TRUE, FALSE, FALSE, FALSE,
		resourceWorkerCost + resourceCommandCenterCost);
	const SkirmishAIRecoveryQueueCommit resourceQueueCommit =
		GetSkirmishAIRecoveryQueueCommit(true, true);
	CHECK(resourceQueueCommit.bindReusableWorkOrder);
	CHECK(resourceQueueCommit.storeProductionIdentity);
	resourceRecoveryCash -= resourceWorkerCost;
	const SkirmishAIRecoveryResourceRoutingDecision liveResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
				FALSE, SKIRMISH_AI_REPLAY_EPOCH_LEGACY),
			true, true, true, resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(liveResourceRouting.resourceGathererDuringCallback);
	CHECK(liveResourceRouting.resourceGathererAfterCallback);
	CHECK(!liveResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryResourceRoutingDecision epoch8ResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
				TRUE, SKIRMISH_AI_REPLAY_EPOCH_RESOURCE_WORKER_PRESERVATION),
			true, true, true, resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(epoch8ResourceRouting.resourceGathererDuringCallback);
	CHECK(epoch8ResourceRouting.resourceGathererAfterCallback);
	CHECK(!epoch8ResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryResourceRoutingDecision epoch7ResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
				TRUE, SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER),
			true, true, true, resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!epoch7ResourceRouting.resourceGathererDuringCallback);
	CHECK(epoch7ResourceRouting.resourceGathererAfterCallback);
	CHECK(!epoch7ResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryResourceRoutingDecision nonResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			true, true, false, false,
			resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!nonResourceRouting.resourceGathererDuringCallback);
	CHECK(!nonResourceRouting.resourceGathererAfterCallback);
	CHECK(!nonResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryQueueCommit directResourceQueueCommit =
		GetSkirmishAIRecoveryQueueCommit(false, true);
	CHECK(!directResourceQueueCommit.bindReusableWorkOrder);
	CHECK(directResourceQueueCommit.storeProductionIdentity);
	const SkirmishAIRecoveryResourceRoutingDecision directResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
				TRUE, SKIRMISH_AI_REPLAY_EPOCH_RESOURCE_WORKER_PRESERVATION),
			false, false, true, resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!directResourceRouting.resourceGathererDuringCallback);
	CHECK(!directResourceRouting.resourceGathererAfterCallback);
	CHECK(directResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryResourceRoutingDecision directNonResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			true, false, false, false,
			resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!directNonResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryResourceRoutingDecision epoch7DirectResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
				TRUE, SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER),
			false, false, true, resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!epoch7DirectResourceRouting.startDirectResourceGatheringAfterCallback);
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false,
		resourceCommandCenterCost, true));
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasBuilder = true;
	input.commandCenterAffordable = false;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(
		input, FALSE, FALSE, FALSE, TRUE, resourceCommandCenterCost);
	resourceRecoveryCash = resourceCommandCenterCost;
	const SkirmishAIRecoveryResourceRoutingDecision affordableResourceRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			true, true, true, true,
			resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!affordableResourceRouting.resourceGathererDuringCallback);
	CHECK(affordableResourceRouting.resourceGathererAfterCallback);
	CHECK(!affordableResourceRouting.startDirectResourceGatheringAfterCallback);
	const SkirmishAIRecoveryResourceRoutingDecision affordableDirectRouting =
		GetSkirmishAIRecoveryResourceRoutingDecision(
			true, false, false, true,
			resourceRecoveryCash, resourceCommandCenterCost);
	CHECK(!affordableDirectRouting.startDirectResourceGatheringAfterCallback);
	input.commandCenterAffordable = true;
	CheckSkirmishAIRecoveryDecision(
		input, FALSE, TRUE, FALSE, FALSE, resourceCommandCenterCost);
	// A primary native reconstruction owns its free-worker lifecycle. This
	// overrides both the resource-order exception and a cleared reserve after
	// exact paid-queue cancellation, while unrelated units remain unaffected.
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false, 0, false, true));
	// The same still-owned native lifecycle must continue suppressing this
	// resource WorkOrder on later queue updates, after the cancellation frame.
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false, 0, false, true));
	CHECK(ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		false, true, true, true, false, false, 0, false, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, false, true, true, false, false, 0, false, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, true, 0, false, true));
	// Recovery may reuse only default-team, non-reinforcement requests. A
	// resource request deterministically outranks an earlier eligible dozer
	// request, while candidates at the same rank retain queue order.
	CHECK(!IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, false, false));
	CHECK(!IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, true, true));
	CHECK(IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, true, false));
	bool hasReusableOrder = false;
	bool selectedReusableOrderIsResource = false;
	const bool customTeamCandidate = IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, false, false);
	if (ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
			hasReusableOrder, selectedReusableOrderIsResource,
			customTeamCandidate, false)) {
		hasReusableOrder = true;
		selectedReusableOrderIsResource = false;
	}
	const bool reinforcementCandidate = IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, true, true);
	if (ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
			hasReusableOrder, selectedReusableOrderIsResource,
			reinforcementCandidate, true)) {
		hasReusableOrder = true;
		selectedReusableOrderIsResource = true;
	}
	CHECK(!hasReusableOrder); // The live path queues directly without an order.
	const bool defaultDozerCandidate = IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, true, false);
	CHECK(ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
		hasReusableOrder, selectedReusableOrderIsResource,
		defaultDozerCandidate, false));
	hasReusableOrder = true;
	selectedReusableOrderIsResource = false;
	const bool defaultResourceCandidate = IsSkirmishAIRecoveryReusableWorkOrder(
		true, true, true, true, false);
	CHECK(ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
		hasReusableOrder, selectedReusableOrderIsResource,
		defaultResourceCandidate, true));
	selectedReusableOrderIsResource = true;
	CHECK(!ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
		hasReusableOrder, selectedReusableOrderIsResource,
		defaultResourceCandidate, true));
	// Direct recovery production stores exact provenance without manufacturing
	// an orphanable WorkOrder. A reused order binds only after queue success;
	// all failed queues leave the existing order untouched.
	SkirmishAIRecoveryQueueCommit queueCommit =
		GetSkirmishAIRecoveryQueueCommit(false, true);
	CHECK(!queueCommit.bindReusableWorkOrder);
	CHECK(queueCommit.storeProductionIdentity);
	queueCommit = GetSkirmishAIRecoveryQueueCommit(false, false);
	CHECK(!queueCommit.bindReusableWorkOrder);
	CHECK(!queueCommit.storeProductionIdentity);
	queueCommit = GetSkirmishAIRecoveryQueueCommit(true, true);
	CHECK(queueCommit.bindReusableWorkOrder);
	CHECK(queueCommit.storeProductionIdentity);
	queueCommit = GetSkirmishAIRecoveryQueueCommit(true, false);
	CHECK(!queueCommit.bindReusableWorkOrder);
	CHECK(!queueCommit.storeProductionIdentity);
	CHECK(IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, true, true, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		true, false, true, true, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, true, true, true, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, false, true, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, true, false, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, true, true, false, true, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, true, true, true, false, true, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, true, true, true, true, false, true));
	CHECK(!IsSkirmishAIRecoveryFactoryFinisherUsable(
		false, false, true, true, true, true, true, false));
	CHECK(IsSkirmishAIRecoveryHoleConstructionMatch(
		true, true, true, 41, 41));
	CHECK(!IsSkirmishAIRecoveryHoleConstructionMatch(
		false, true, true, 41, 41));
	CHECK(!IsSkirmishAIRecoveryHoleConstructionMatch(
		true, false, true, 41, 41));
	CHECK(!IsSkirmishAIRecoveryHoleConstructionMatch(
		true, true, false, 41, 41));
	CHECK(!IsSkirmishAIRecoveryHoleConstructionMatch(
		true, true, true, 40, 41));
	CHECK(ShouldRestoreSkirmishAIRecoveryScaffoldBuilder(true, false));
	CHECK(!ShouldRestoreSkirmishAIRecoveryScaffoldBuilder(false, false));
	CHECK(!ShouldRestoreSkirmishAIRecoveryScaffoldBuilder(true, true));
	const int placementOffsetCount = 37;
	CHECK(!HasSkirmishAIRecoveryScaffoldReplacementAttempt(
		0, placementOffsetCount));
	CHECK(!HasSkirmishAIRecoveryScaffoldReplacementAttempt(
		placementOffsetCount - 1, placementOffsetCount));
	CHECK(HasSkirmishAIRecoveryScaffoldReplacementAttempt(
		placementOffsetCount, placementOffsetCount));
	CHECK(MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
		0, placementOffsetCount) == placementOffsetCount);
	CHECK(MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
		8, placementOffsetCount) == placementOffsetCount + 8);
	CHECK(MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
		8, placementOffsetCount) % placementOffsetCount == 8);
	CHECK(MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
		placementOffsetCount + 8, placementOffsetCount) ==
		placementOffsetCount + 8);
	CHECK(MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
		-1, placementOffsetCount) == placementOffsetCount);
	CHECK(!HasSkirmishAIRecoveryObservedReplacement(
		placementOffsetCount + 8, placementOffsetCount));
	CHECK(HasSkirmishAIRecoveryObservedReplacement(
		2 * placementOffsetCount, placementOffsetCount));
	CHECK(MarkSkirmishAIRecoveryObservedReplacement(
		placementOffsetCount + 8, placementOffsetCount) ==
		2 * placementOffsetCount + 8);
	// A vanished paid queue reopens the replacement route until a compatible
	// builder has actually emerged.  An observed replacement keeps the marker.
	CHECK(ReconcileSkirmishAIRecoveryReplacementAttempt(
		placementOffsetCount + 8, placementOffsetCount,
		true) == placementOffsetCount + 8);
	CHECK(ReconcileSkirmishAIRecoveryReplacementAttempt(
		placementOffsetCount + 8, placementOffsetCount,
		false) == 8);
	CHECK(ReconcileSkirmishAIRecoveryReplacementAttempt(
		2 * placementOffsetCount + 8, placementOffsetCount,
		false) == 2 * placementOffsetCount + 8);
	// A pre-existing compatible builder is not attributable production and is
	// deliberately absent from reconciliation.  Once the paid queue vanishes,
	// the unconfirmed marker clears and a healthy alternate factory is usable.
	const int vanishedQueueWithPreexistingBuilder =
		ReconcileSkirmishAIRecoveryReplacementAttempt(
			placementOffsetCount + 8, placementOffsetCount, false);
	CHECK(!HasSkirmishAIRecoveryScaffoldReplacementAttempt(
		vanishedQueueWithPreexistingBuilder, placementOffsetCount));
	const bool healthyAlternateFactoryPotential =
		!HasSkirmishAIRecoveryScaffoldReplacementAttempt(
			vanishedQueueWithPreexistingBuilder, placementOffsetCount) &&
		IsSkirmishAIRecoveryFactoryPotentialDuringGrace(true, false, false);
	CHECK(healthyAlternateFactoryPotential);
	CHECK(!IsSkirmishAIRecoveryBuilderPathUnavailable(
		false, false, false, healthyAlternateFactoryPotential));
	CHECK(AdvanceSkirmishAIRecoveryPlacementAttempt(
		0, placementOffsetCount) == 1);
	CHECK(AdvanceSkirmishAIRecoveryPlacementAttempt(
		placementOffsetCount - 1, placementOffsetCount) == 0);
	CHECK(AdvanceSkirmishAIRecoveryPlacementAttempt(
		placementOffsetCount, placementOffsetCount) ==
		placementOffsetCount + 1);
	CHECK(AdvanceSkirmishAIRecoveryPlacementAttempt(
		2 * placementOffsetCount - 1, placementOffsetCount) ==
		placementOffsetCount);
	CHECK(AdvanceSkirmishAIRecoveryPlacementAttempt(
		3 * placementOffsetCount - 1, placementOffsetCount) ==
		2 * placementOffsetCount);
	CHECK(AdvanceSkirmishAIRecoveryPlacementAttempt(
		-1, placementOffsetCount) == 1);
	CHECK(!ShouldSellSkirmishAIRecoveryScaffold(
		true, true, false, true, true));
	CHECK(ShouldSellSkirmishAIRecoveryScaffold(
		false, false, false, true, false));
	CHECK(!ShouldSellSkirmishAIRecoveryScaffold(
		true, false, true, true, true));
	// A possible factory remains retryable while cash or admission is
	// temporarily unavailable, until a paid replacement is actually attempted.
	CHECK(!ShouldSellSkirmishAIRecoveryScaffold(
		true, false, false, false, true));
	CHECK(ShouldSellSkirmishAIRecoveryScaffold(
		true, false, false, true, true));
	CHECK(ShouldSellSkirmishAIRecoveryScaffold(
		true, false, false, false, false));
	const bool stalledScaffoldWouldSell =
		ShouldSellSkirmishAIRecoveryScaffold(
			true, false, false, false, false);
	const unsigned int nativeRetryFrames = 2U * 30U;
	const unsigned int nativeWorkerRespawnFrames = 10U * 30U;
	CHECK(nativeRetryFrames < nativeWorkerRespawnFrames);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, true) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RECYCLE_NATIVE_WORKER);
	// Once advancing routes have returned, a present unusable exact worker must
	// reset even when the independent scaffold-relocation predicate says KEEP.
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	const bool boundedFailoverFailureReturns =
		ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
			true, false, true);
	CHECK(!boundedFailoverFailureReturns &&
		GetSkirmishAIRecoveryStalledScaffoldAction(
			false, true, true, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	const bool bareFactoryDefersDisposition =
		ShouldDeferSkirmishAIRecoveryStalledDisposition(
			false, true, true, true);
	const bool replacementAttemptedWouldSell =
		ShouldSellSkirmishAIRecoveryScaffold(
			true, false, false, true, true);
	CHECK(!bareFactoryDefersDisposition && replacementAttemptedWouldSell &&
		GetSkirmishAIRecoveryStalledScaffoldAction(
			replacementAttemptedWouldSell, true, true, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	const SkirmishAIRecoveryStalledScaffoldAction unusableResetAction =
		GetSkirmishAIRecoveryStalledScaffoldAction(
			false, true, true, false);
	const bool liveForeignAssignedBuilder = true;
	// Exact cancellation is part of RESET itself, independent of the scaffold's
	// live foreign builder, so production cannot resume during the retry window.
	CHECK(liveForeignAssignedBuilder &&
		ShouldCancelSkirmishAIRecoveryPaidQueueForNativeWorkerReset(
			unusableResetAction, true, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeWorkerReset(
		unusableResetAction, true, false)); // exact entry already vanished
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeWorkerReset(
		unusableResetAction, false, true)); // unrelated entry is never exact
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeWorkerReset(
		SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP, true, true));
	CHECK(ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(
		true, false)); // captured worker survives while the hole detaches
	CHECK(!ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(
		true, true));
	CHECK(!ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(
		false, false));
	const ThingTemplate *exactNativeVariant =
		reinterpret_cast<const ThingTemplate *>(1);
	const ThingTemplate *equivalentPrimaryVariant =
		reinterpret_cast<const ThingTemplate *>(2);
	CHECK(GetSkirmishAIRecoveryExactNativeRebuildTemplate(
		exactNativeVariant, equivalentPrimaryVariant) == exactNativeVariant);
	CHECK(ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		true, true, false, false, false, false));
	CHECK(!ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		false, true, false, false, false, false));
	CHECK(!ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		true, false, false, false, false, false));
	CHECK(!ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		true, true, true, false, false, false)); // container owns selection state
	CHECK(!ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		true, true, false, true, false, false)); // masking owns selection state
	CHECK(!ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		true, true, false, false, false, true)); // slaver/sold/scuttle owner
	CHECK(ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
		true, true, false, false, true, false)); // script-held alone owns no bit
	const ObjectID dockedWorkerID = static_cast<ObjectID>(91);
	const ObjectID unloadingWorkerID = static_cast<ObjectID>(93);
	const ObjectID unrelatedWorkerID = static_cast<ObjectID>(92);
	CHECK(IsExactRailedTransportDockOwnedObject(
		dockedWorkerID, INVALID_ID, dockedWorkerID));
	CHECK(IsExactRailedTransportDockOwnedObject(
		INVALID_ID, unloadingWorkerID, unloadingWorkerID));
	CHECK(!IsExactRailedTransportDockOwnedObject(
		dockedWorkerID, unloadingWorkerID,
		unrelatedWorkerID)); // unrelated ferry
	CHECK(!IsExactRailedTransportDockOwnedObject(
		INVALID_ID, INVALID_ID, dockedWorkerID));
	const int capturedNativeWorkerID = 73;
	const int invalidNativeWorkerID = -1;
	const bool capturedNativeWorkerDetached =
		ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(true, false);
	CHECK(GetSkirmishAIRecoveryNativeWorkerIDAfterOwnershipDetach(
		capturedNativeWorkerDetached, capturedNativeWorkerID,
		invalidNativeWorkerID) == invalidNativeWorkerID);
	const bool resumeGraceWouldOtherwiseReturn = true;
	CHECK(resumeGraceWouldOtherwiseReturn &&
		ShouldImmediatelyAbandonSkirmishAINativeLineageAfterCapture(
			capturedNativeWorkerDetached, true));
	const SkirmishAINativeCapturedWorkerTerminalTransition
		secondCapturedWorkerTransition =
			GetSkirmishAINativeCapturedWorkerTerminalTransition(
				capturedNativeWorkerDetached, true);
	CHECK(secondCapturedWorkerTransition.abandonImmediately);
	CHECK(secondCapturedWorkerTransition.preserveDetachedWorker);
	CHECK(secondCapturedWorkerTransition.removeHole);
	CHECK(secondCapturedWorkerTransition.removeScaffold);
	CHECK(secondCapturedWorkerTransition.clearRecycleMarker);
	CHECK(!secondCapturedWorkerTransition.spawnReplacementWorker);
	CHECK(!ShouldImmediatelyAbandonSkirmishAINativeLineageAfterCapture(
		capturedNativeWorkerDetached, false));
	CHECK(ClearSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		3 * 9 + 2, 9) == 2);
	// The captured unit remains independently live while the hole observes an
	// invalid worker ID before either the assigned replacement or paid queue can
	// return progress. Both paths therefore preserve native respawn ownership and
	// cancel only the exact tracked paid duplicate.
	const bool capturedUnitSurvivesDetach = true;
	const bool assignedReplacementCanResume = true;
	const bool paidReplacementWasProgressing = true;
	CHECK(capturedUnitSurvivesDetach && assignedReplacementCanResume &&
		ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
			true, false, false, false, false, false));
	CHECK(capturedUnitSurvivesDetach && paidReplacementWasProgressing &&
		ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
			true, true, true, true));
	CHECK(GetSkirmishAIRecoveryNativeWorkerIDAfterOwnershipDetach(
		capturedNativeWorkerDetached, capturedNativeWorkerID,
		invalidNativeWorkerID) == invalidNativeWorkerID);
	// The first evaluation recycles the exact live hole worker.  On the next
	// earlier AI retry that worker is absent while the native respawn timer is
	// pending, so KEEP proves recovery will not restart the longer countdown.
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		stalledScaffoldWouldSell, true, true, true) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RECYCLE_NATIVE_WORKER);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		stalledScaffoldWouldSell, true, false, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP);
	// A captured, dead-present, non-dozer, or incompatible exact object cannot
	// start native respawn while its ID still resolves. Reset that exact object
	// once instead of masking the bounded disposition with KEEP forever.
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		stalledScaffoldWouldSell, true, true, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		stalledScaffoldWouldSell, false, false, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_SELL);
	UnsignedInt evacuationDeadline = GetSkirmishAIRecoveryEvacuationDeadline(
		100, 0, true, 60);
	CHECK(evacuationDeadline == 160);
	CHECK(GetSkirmishAIRecoveryEvacuationDeadline(
		120, evacuationDeadline, true, 60) == evacuationDeadline);
	CHECK(IsSkirmishAIRecoveryEvacuationGraceActive(
		159, evacuationDeadline, true));
	CHECK(!IsSkirmishAIRecoveryEvacuationGraceActive(
		160, evacuationDeadline, true));
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasBuilderFactory = false;
	input.noBuilderPath = IsSkirmishAIRecoveryBuilderPathUnavailable(
		false, IsSkirmishAIRecoveryEvacuationGraceActive(
			160, evacuationDeadline, true), false, false);
	input.protectedReserve = 750;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, TRUE, FALSE, 0);
	CHECK(GetSkirmishAIRecoveryEvacuationDeadline(
		120, evacuationDeadline, false, 60) == 0);
	// Expiring a transient external or unreachable route releases its reserve
	// without latching recovery off.  A later internally progressing admission
	// becomes potential again on the next scan.
	UnsignedInt transientRouteDeadline =
		GetSkirmishAIRecoveryEvacuationDeadline(200, 0, true, 60);
	CHECK(IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		false, true, IsSkirmishAIRecoveryEvacuationGraceActive(
			259, transientRouteDeadline, true)));
	CHECK(!IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		false, true, IsSkirmishAIRecoveryEvacuationGraceActive(
			260, transientRouteDeadline, true)));
	CHECK(!ShouldLatchSkirmishAIRecoveryLastStand(true));
	CHECK(IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
		true, false, false));
	CHECK(GetSkirmishAIRecoveryEvacuationDeadlineForVersion(3, 999) == 0);
	CHECK(GetSkirmishAIRecoveryEvacuationDeadlineForVersion(4, 999) == 999);
	SkirmishAIRecoveryProductionIdentity oldIdentity =
		GetSkirmishAIRecoveryProductionIdentityForVersion(
			4, 17, 23, 0, 0);
	CHECK(oldIdentity.factoryID == 0);
	CHECK(oldIdentity.productionID == 0);
	SkirmishAIRecoveryProductionIdentity savedIdentity =
		GetSkirmishAIRecoveryProductionIdentityForVersion(
			5, 17, 23, 0, 0);
	CHECK(savedIdentity.factoryID == 17);
	CHECK(savedIdentity.productionID == 23);
	CHECK(!GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
		4, true));
	CHECK(!GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
		5, true));
	CHECK(GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
		6, true));
	CHECK(!GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
		6, false));
	CHECK(!GetSkirmishAIRecoveryBuilderFailoverConsumedForVersion(6, true));
	CHECK(GetSkirmishAIRecoveryBuilderFailoverConsumedForVersion(7, true));
	CHECK(!GetSkirmishAIRecoveryBuilderFailoverConsumedForVersion(7, false));
	CHECK(IsSkirmishAIRecoveryProductionIdentityTracked(17, 23, 0, 0));
	CHECK(!IsSkirmishAIRecoveryProductionIdentityTracked(0, 23, 0, 0));
	CHECK(!IsSkirmishAIRecoveryProductionIdentityTracked(17, 0, 0, 0));
	CHECK(IsSkirmishAIRecoveryProductionIdentityMatch(
		17, 23, 17, 23, 0, 0));
	CHECK(!IsSkirmishAIRecoveryProductionIdentityMatch(
		17, 23, 18, 23, 0, 0));
	CHECK(!IsSkirmishAIRecoveryProductionIdentityMatch(
		17, 23, 17, 24, 0, 0));
	// A queued recovery identity shields earlier ordinary compatible output.
	CHECK(!ShouldAdoptSkirmishAIRecoveryProduction(true, true, true));
	CHECK(ShouldAdoptSkirmishAIRecoveryProduction(false, true, true));
	CHECK(!ShouldAdoptSkirmishAIRecoveryProduction(false, false, true));
	CHECK(!ShouldAdoptSkirmishAIRecoveryProduction(false, true, false));
	// A v4 load has no saved identity.  Load post-processing binds the
	// authoritative paid entry before the first production callback while the
	// replacement remains active and unobserved.
	CHECK(ShouldBindSkirmishAIRecoveryProductionIdentity(
		true, false, true, false,
		IsSkirmishAIRecoveryProductionIdentityTracked(
			oldIdentity.factoryID, oldIdentity.productionID, 0, 0),
		true, true, true));
	CHECK(!ShouldBindSkirmishAIRecoveryProductionIdentity(
		true, false, true, true, false, true, true, true));
	CHECK(!ShouldBindSkirmishAIRecoveryProductionIdentity(
		true, false, true, false, true, true, true, true));
	CHECK(!ShouldBindSkirmishAIRecoveryProductionIdentity(
		true, false, true, false, false, true, true, false));
	CHECK(!ShouldBindSkirmishAIRecoveryProductionIdentity(
		true, false, false, false, false, true, true, true));
	CHECK(!ShouldBindSkirmishAIRecoveryProductionIdentity(
		false, false, true, false, false, true, true, true));
	CHECK(!ShouldBindSkirmishAIRecoveryProductionIdentity(
		true, true, true, false, false, true, true, true));
	CHECK(!ShouldClearSkirmishAIRecoveryProductionIdentity(true, true));
	CHECK(ShouldClearSkirmishAIRecoveryProductionIdentity(false, true));
	CHECK(ShouldClearSkirmishAIRecoveryProductionIdentity(true, false));
	// loadPostProcess validates the migrated v5 identity against the loaded
	// production queue. A missing exact entry clears both identity and ownership.
	bool migratedV5Ownership =
		GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
			5, false);
	if (ShouldClearSkirmishAIRecoveryProductionIdentity(true, false))
		migratedV5Ownership = false;
	CHECK(!migratedV5Ownership);
	// An ordinary compatible queue remains an authoritative paid route, but its
	// adopted identity never grants cancellation or refund authority.
	int ordinaryQueueEntries = 1;
	int ordinaryCash = 700;
	if (ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
			false, true, true)) {
		--ordinaryQueueEntries;
		ordinaryCash += 200;
	}
	if (ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
			true, false, true, true)) {
		--ordinaryQueueEntries;
		ordinaryCash += 200;
	}
	CHECK(ordinaryQueueEntries == 1);
	CHECK(ordinaryCash == 700);
	CHECK(ShouldUseSkirmishAIRecoveryNonCancellingFailover(
		false, true, true));
	CHECK(ShouldUseSkirmishAIRecoveryUnownedQueueFailover(FALSE, 0));
	CHECK(!ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP));
	CHECK(ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER));
	CHECK(!ShouldUseSkirmishAIRecoveryNonCancellingFailover(
		true, true, true));
	CHECK(!ShouldUseSkirmishAIRecoveryNonCancellingFailover(
		false, false, true));
	CHECK(!ShouldUseSkirmishAIRecoveryNonCancellingFailover(
		false, true, false));
	// Disabled unowned A remains paid and bound. An independently affordable
	// healthy B adds one owned route without refunding A; tracking changes only
	// after B succeeds.
	int unownedAEntries = 1;
	int ownedBEntries = 0;
	int nonCancellingCash = 700;
	int trackedFactory = 17;
	bool trackedOwned =
		GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(5, true);
	CHECK(!trackedOwned);
	if (ShouldUseSkirmishAIRecoveryUnownedQueueFailover(FALSE, 0) &&
		ShouldUseSkirmishAIRecoveryNonCancellingFailover(
			trackedOwned, true, unownedAEntries == 1) &&
		IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
			true, nonCancellingCash >= 200, true)) {
		++ownedBEntries;
		nonCancellingCash -= 200;
		trackedFactory = 18;
		trackedOwned = true;
	}
	bool unownedFailoverConsumed =
		GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
			false, ownedBEntries == 1);
	CHECK(unownedAEntries == 1);
	CHECK(ownedBEntries == 1);
	CHECK(nonCancellingCash == 500);
	CHECK(trackedFactory == 18);
	CHECK(trackedOwned);
	CHECK(unownedFailoverConsumed);
	// Neither an unaffordable alternate nor a max-blocked alternate may debit,
	// cancel A, or switch identity.
	int blockedAEntries = 1;
	int blockedBEntries = 0;
	int blockedCash = 100;
	int blockedTrackedFactory = 17;
	if (IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
			true, false, true) ||
		IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
			false, true, true)) {
		--blockedAEntries;
		++blockedBEntries;
		blockedCash -= 200;
		blockedTrackedFactory = 18;
	}
	CHECK(blockedAEntries == 1);
	CHECK(blockedBEntries == 0);
	CHECK(blockedCash == 100);
	CHECK(blockedTrackedFactory == 17);
	// A temporarily disabled A that is progressing again never enters failover,
	// so recovery does not buy B as a duplicate.
	int temporaryAEntries = 1;
	int temporaryBEntries = 0;
	if (ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
			false, false, false, true, false))
		++temporaryBEntries;
	CHECK(temporaryAEntries == 1);
	CHECK(temporaryBEntries == 0);
	// A queue created by this recovery instance retains exact cancellation and
	// refund authority; a same-cost failover leaves one paid route and net cash.
	int ownedQueueEntries = 1;
	int ownedCash = 700;
	if (ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
			true, true, true)) {
		--ownedQueueEntries;
		ownedCash += 200;
		++ownedQueueEntries;
		ownedCash -= 200;
	}
	bool ownedFailoverConsumed =
		GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
			false, ownedQueueEntries == 1 && ownedCash == 700);
	CHECK(ownedQueueEntries == 1);
	CHECK(ownedCash == 700);
	CHECK(ownedFailoverConsumed);
	CHECK(!ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, false, true, ownedFailoverConsumed));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
		true, false, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
		true, true, false));
	// With two simultaneous native rebuild holes, each free worker remains
	// reserved for the hole that spawned it. A different worker ID and a dead
	// or foreign hole do not reserve the candidate.
	CHECK(IsSkirmishAIRecoveryReservedNativeWorker(true, 41, 41, 0));
	CHECK(!IsSkirmishAIRecoveryReservedNativeWorker(true, 41, 42, 0));
	CHECK(!IsSkirmishAIRecoveryReservedNativeWorker(false, 41, 41, 0));
	CHECK(!IsSkirmishAIRecoveryReservedNativeWorker(true, 0, 0, 0));
	// Worker A belongs to the primary hole. Replacement B may temporarily own
	// the scaffold; once B dies, the still-live compatible A must re-enter the
	// dedicated assigned-builder path rather than being treated as a respawn gap.
	CHECK(!ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
		true, true, true, true));
	CHECK(ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
		false, true, true, true));
	CHECK(!ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
		false, true, false, true));
	CHECK(!ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
		false, true, true, false));
	CHECK(!ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
		false, false, true, true));
	// A merely live assigned worker cannot mask the exact native worker. Each
	// operational and path condition must hold before the center binding wins.
	CHECK(IsSkirmishAIRecoveryAssignedBuilderOperational(
		true, false, false, true, true, true));
	CHECK(!IsSkirmishAIRecoveryAssignedBuilderOperational(
		true, true, false, true, true, true)); // contained
	CHECK(!IsSkirmishAIRecoveryAssignedBuilderOperational(
		true, false, true, true, true, true)); // unmanned
	CHECK(!IsSkirmishAIRecoveryAssignedBuilderOperational(
		true, false, false, false, false, true)); // no AI/DozerAI
	CHECK(!IsSkirmishAIRecoveryAssignedBuilderOperational(
		true, false, false, true, true, false)); // update cannot advance
	CHECK(!IsSkirmishAIRecoveryAssignedBuilderUsable(
		true, true, false)); // path blocked
	CHECK(!IsSkirmishAIRecoveryAssignedBuilderUsable(
		true, false, true)); // no valid build dock despite a pathable fallback
	// The legacy target-returning wrapper discards a failed dock search and can
	// leave its fallback working position path-reachable. Recovery uses the Bool
	// search result directly, so that fallback cannot claim native progress,
	// cancel an exact paid route, or suppress an ordinary compatible WorkOrder.
	const bool buildDockSearchSucceeded = false;
	const bool fallbackWorkingPositionPathReachable = true;
	const bool falsePositiveDockRoute =
		IsSkirmishAIRecoveryAssignedBuilderUsable(
			true, buildDockSearchSucceeded,
			fallbackWorkingPositionPathReachable);
	CHECK(!falsePositiveDockRoute);
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, true, falsePositiveDockRoute));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
		falsePositiveDockRoute, true, true, true));
	CHECK(!ShouldSuppressSkirmishAIRecoveryBuilderOrder(
		true, true, true, true, false, false, 0, false,
		falsePositiveDockRoute));
	CHECK(IsSkirmishAIRecoveryNativeWorkerActivelyBuilding(
		true, true, true, true, true));
	CHECK(!IsSkirmishAIRecoveryNativeWorkerActivelyBuilding(
		true, true, true, false, true)); // pending/moving, not building at dock
	CHECK(!IsSkirmishAIRecoveryNativeWorkerActivelyBuilding(
		true, true, true, true, false)); // wrong scaffold
	const bool liveUnusableAssigned =
		!IsSkirmishAIRecoveryAssignedBuilderUsable(true, true, false);
	CHECK(liveUnusableAssigned &&
		ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
			false, true, true, true)); // present native replaces it
	CHECK(liveUnusableAssigned &&
		ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
			true, false, false, false, false, false)); // absent native retries
	const bool nativeWrongResumeTarget =
		!IsSkirmishAIRecoveryAssignedBuilderUsable(true, false, true);
	const bool nativePathBlocked =
		!IsSkirmishAIRecoveryAssignedBuilderUsable(true, true, false);
	CHECK(nativeWrongResumeTarget &&
		!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
			true, true, true, true, true, false));
	CHECK(nativePathBlocked &&
		!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
			true, true, true, true, true, false));
	CHECK(liveUnusableAssigned && nativePathBlocked &&
		!ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
			false, true, true, false));
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	const int nativeRecycleMarker =
		MarkSkirmishAIRecoveryNativeWorkerRecycleAttempt(2, placementOffsetCount);
	CHECK(HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		nativeRecycleMarker, placementOffsetCount));
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, false, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, false, true) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_ABANDON_NATIVE_LINEAGE);
	// The persisted recycle band survives placement advancement and replacement
	// observation, proving a respawned path-blocked worker cannot reset forever.
	CHECK(HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		AdvanceSkirmishAIRecoveryPlacementAttempt(
			nativeRecycleMarker, placementOffsetCount), placementOffsetCount));
	CHECK(HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		MarkSkirmishAIRecoveryObservedReplacement(
			nativeRecycleMarker, placementOffsetCount), placementOffsetCount));
	CHECK(ClearSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		nativeRecycleMarker, placementOffsetCount) == 2);
	// External scaffold sale/capture or destruction can remove both members of
	// the native lineage without visiting its terminal teardown. The next
	// no-center scan clears only band 3, retaining the lower placement value so a
	// viable factory remains an ordinary recovery route instead of inheriting a
	// stale one-recycle terminal state.
	const int afterExternalNativeLineageLoss =
		ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
			nativeRecycleMarker, placementOffsetCount, false, false);
	CHECK(afterExternalNativeLineageLoss == 2);
	CHECK(!HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		afterExternalNativeLineageLoss, placementOffsetCount));
	// Bands 1 and 2 are existing placement/replacement observations, including
	// epoch-3-compatible snapshot state. A lineage scan must not modulo those
	// values merely because the current-epoch native scaffold is absent.
	CHECK(ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
		placementOffsetCount + 2, placementOffsetCount, false, false) ==
		placementOffsetCount + 2);
	CHECK(ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
		2 * placementOffsetCount + 2, placementOffsetCount, false, false) ==
		2 * placementOffsetCount + 2);
	CHECK(ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
		3 * placementOffsetCount + 2, placementOffsetCount, false, false) == 2);
	// A live scaffold without a matching hole owns no recycle band. Clearing it
	// allows one paid factory replacement instead of indefinite native deferral.
	const int afterLiveScaffoldLosesNativeHole =
		ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
			nativeRecycleMarker, placementOffsetCount, true, false);
	CHECK(afterLiveScaffoldLosesNativeHole == 2);
	CHECK(!HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		afterLiveScaffoldLosesNativeHole, placementOffsetCount));
	CHECK(ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
		nativeRecycleMarker, placementOffsetCount, false, true) ==
		nativeRecycleMarker);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(
		input, TRUE, FALSE, FALSE, FALSE, 1600);
	// Exact multi-cycle policy: transient grace, one persisted canonical recycle,
	// an absent-worker respawn gap, then deterministic lineage abandonment if the
	// respawned worker is still unusable. Clearing the band enables relocation.
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, false,
		HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
			2, placementOffsetCount)) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, false, false, true) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		false, true, true, false,
		HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
			nativeRecycleMarker, placementOffsetCount)) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_ABANDON_NATIVE_LINEAGE);
	// Only a missing exact worker or an existing usable owned compatible dozer
	// preserves current-epoch native ownership. An exact object that still
	// resolves cannot enter the hole's respawn process, so unusable variants
	// must fall through to generic recovery.
	CHECK(ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, false, false, false, false, false)); // absent: respawn gap
	CHECK(ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, true, true)); // usable exact native worker
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, false, true, true, true)); // captured or otherwise unowned
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, false, true)); // incompatible command set
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, false, true, true)); // non-dozer exact object
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, false, true, true, false)); // effectively dead but still present
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		false, false, false, false, false, false)); // unrelated/no native hole
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, true, false)); // contained
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, true, false)); // disabled/unmanned
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, true, false)); // missing DozerAI
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
		true, true, true, true, true, false)); // update cannot advance
	// Epoch 3 preserves the original unconditional producer-linked native-hole
	// return. Live games and epoch 4 inspect the assigned/native worker instead.
	CHECK(ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
		false, true));
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
		true, true));
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
		false, false));
	// A native respawn cancels only a still-existing exact paid recovery entry.
	CHECK(ShouldCancelSkirmishAIRecoveryPaidQueueForNativeRespawn(
		true, false, true, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeRespawn(
		true, true, true, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeRespawn(
		true, false, false, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeRespawn(
		true, false, true, false));
	CHECK(!ShouldCancelSkirmishAIRecoveryPaidQueueForNativeRespawn(
		false, false, true, true));
	// Current native ownership cancels its exact paid duplicate before any live
	// foreign assigned builder can return successfully from scaffold resume.
	const bool foreignAssignedBuilderLive = true;
	CHECK(foreignAssignedBuilderLive &&
		ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
			true, true, true, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
		true, true, true, false));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
		true, true, false, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
		false, true, true, true));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
		true, false, true, true)); // ordinary compatible production is not owned
	evacuationDeadline = GetSkirmishAIRecoveryEvacuationDeadline(
		0xFFFFFFFEU, 0, true, 3);
	CHECK(evacuationDeadline == 1U);
	CHECK(IsSkirmishAIRecoveryEvacuationGraceActive(
		0xFFFFFFFFU, evacuationDeadline, true));
	CHECK(IsSkirmishAIRecoveryEvacuationGraceActive(
		0U, evacuationDeadline, true));
	CHECK(!IsSkirmishAIRecoveryEvacuationGraceActive(
		1U, evacuationDeadline, true));

	// With no builder route left, the completed-but-missing AI has reached a
	// genuine last stand. Retaining a construction reserve cannot help.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.noBuilderPath = true;
	input.protectedReserve = 750;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, TRUE, FALSE, 0);

	// Fund an existing unpaid builder request rather than letting the reserve
	// block its only recovery path. The runtime reuses that work order.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.builderQueued = true;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, TRUE, FALSE, FALSE, FALSE, 1600);
	// A pending unpaid request that cannot be funded yet remains retryable; it
	// must not be reported as a successful duplicate queue operation.
	input.builderAffordable = false;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1600);
	// An unpaid request alone is not a surviving physical recovery route.
	input.hasBuilderFactory = false;
	input.noBuilderPath = true;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, TRUE, FALSE, 0);
	// A paid queue must not reserve or charge the builder a second time.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.builderQueuePaid = true;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1200);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.builderQueued = true;
	input.builderQueuePaid = true;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1200);

	// A builder with insufficient funds or an obstructed placement keeps the
	// command center recovery pending for the bounded retry timer.
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasBuilder = true;
	input.commandCenterAffordable = false;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1200);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.hasBuilder = true;
	input.placementReady = false;
	CheckSkirmishAIRecoveryDecision(input, FALSE, FALSE, FALSE, TRUE, 1200);

	// Recovery cost arithmetic clamps invalid negative inputs and saturates
	// rather than wrapping when both required purchases exceed Int::max.
	CHECK(AddSkirmishAIRecoveryCost(-100, -200) == 0);
	CHECK(AddSkirmishAIRecoveryCost(-100, 5) == 5);
	CHECK(AddSkirmishAIRecoveryCost(INT_MAX, 1) == INT_MAX);
	CHECK(AddSkirmishAIRecoveryCost(INT_MAX, -1) == INT_MAX);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.commandCenterCost = INT_MAX;
	input.builderCost = 100;
	input.protectedReserve = 0;
	CheckSkirmishAIRecoveryDecision(input, TRUE, FALSE, FALSE, FALSE, INT_MAX);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.commandCenterCost = -10;
	input.builderCost = -20;
	input.protectedReserve = -30;
	CheckSkirmishAIRecoveryDecision(input, TRUE, FALSE, FALSE, FALSE, 0);
	input = MakeSkirmishAIRecoveryPolicyInput();
	input.protectedReserve = 2000;
	CheckSkirmishAIRecoveryDecision(input, TRUE, FALSE, FALSE, FALSE, 2000);

	// The recovery mode policy must follow the real engine enum values. This
	// catches drift from hard-coded mode integers while keeping unsupported
	// shell, replay, single-player, and none modes isolated.
	CHECK(!IsSkirmishAIRecoveryGameMode(GAME_SINGLE_PLAYER));
	CHECK(IsSkirmishAIRecoveryGameMode(GAME_LAN));
	CHECK(IsSkirmishAIRecoveryGameMode(GAME_SKIRMISH));
	CHECK(!IsSkirmishAIRecoveryGameMode(GAME_REPLAY));
	CHECK(!IsSkirmishAIRecoveryGameMode(GAME_SHELL));
	CHECK(IsSkirmishAIRecoveryGameMode(GAME_INTERNET));
	CHECK(!IsSkirmishAIRecoveryGameMode(GAME_NONE));
	CHECK(!IsSkirmishAIRecoveryGameMode(99));

	// Retry frame zero is the unset sentinel, even when the current frame is
	// large. Scheduled deadlines use unsigned wrap-safe comparisons and never
	// publish zero, including an exact wrap from UINT_MAX + 1.
	CHECK(IsSkirmishAIRecoveryRetryDue(0xF0000000U, 0U));
	CHECK(GetSkirmishAIRecoveryRetryFrame(0xF0000000U, 0U) == 0xF0000000U);
	CHECK(GetSkirmishAIRecoveryRetryFrame(0U, 0U) == 1U);
	CHECK(!IsSkirmishAIRecoveryRetryDue(99U, 100U));
	CHECK(IsSkirmishAIRecoveryRetryDue(100U, 100U));
	CHECK(IsSkirmishAIRecoveryRetryDue(101U, 100U));
	CHECK(!IsSkirmishAIRecoveryBoundedGraceExpired(100U, 0U));
	CHECK(!IsSkirmishAIRecoveryBoundedGraceExpired(99U, 100U));
	CHECK(IsSkirmishAIRecoveryBoundedGraceExpired(100U, 100U));
	CHECK(ShouldReleaseSkirmishAIRecoveryReserveForExpiredGrace(
		true, true));
	CHECK(!ShouldReleaseSkirmishAIRecoveryReserveForExpiredGrace(
		true, false));
	CHECK(!ShouldReleaseSkirmishAIRecoveryReserveForExpiredGrace(
		false, true));
	CHECK(ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
		true, false));
	CHECK(ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
		false, true));
	CHECK(!ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
		false, false));
	CHECK(ShouldDeferSkirmishAIRecoveryStalledDisposition(
		true, false, true, true)); // progressing paid queue always defers disposition
	CHECK(!ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, true, false, true)); // factory capability cannot mask native RESET
	CHECK(!ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, false, false, true));
	const bool queueFullAdvancingFactory =
		IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
			false, true, false, true);
	CHECK(ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, queueFullAdvancingFactory, false, false));
	const bool unfinishedFactoryWithFinisher =
		IsSkirmishAIRecoveryFactoryPotentialDuringGrace(true, true, false);
	CHECK(ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, unfinishedFactoryWithFinisher, false, false));
	// Model the live order: a paid replacement emerged, was observed, then became
	// geographically unusable while a healthy factory remained CANMAKE_OK. The
	// attempt marker closes the one-purchase queue gate, so bare factory potential
	// cannot defer the scaffold disposition forever.
	const int observedReplacementAttempt =
		MarkSkirmishAIRecoveryObservedReplacement(
			MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
				0, placementOffsetCount), placementOffsetCount);
	const bool replacementBudgetSpent =
		HasSkirmishAIRecoveryScaffoldReplacementAttempt(
			observedReplacementAttempt, placementOffsetCount);
	const bool observedUnreachableReplacement =
		HasSkirmishAIRecoveryObservedReplacement(
			observedReplacementAttempt, placementOffsetCount);
	const bool replacementQueueGateOpen = !replacementBudgetSpent;
	CHECK(!replacementQueueGateOpen);
	CHECK(!ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, true, replacementBudgetSpent, false));
	const bool observedReplacementWouldSell =
		ShouldSellSkirmishAIRecoveryScaffold(
			true, false, false, replacementBudgetSpent, true);
	CHECK(observedUnreachableReplacement && observedReplacementWouldSell);
	CHECK(GetSkirmishAIRecoveryStalledScaffoldAction(
		observedReplacementWouldSell, false, false, false) ==
		SKIRMISH_AI_RECOVERY_SCAFFOLD_SELL);
	CHECK(ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
		false, IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
			false, true, false, true)));
	CHECK(!ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
		false, IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
			false, true, false, false)));
	CHECK(GetSkirmishAIRecoveryRetryFrame(100U, 5U) == 105U);
	CHECK(!IsSkirmishAIRecoveryRetryDue(0xFFFFFFFEU, 1U));
	CHECK(!IsSkirmishAIRecoveryRetryDue(0xFFFFFFFFU, 1U));
	CHECK(!IsSkirmishAIRecoveryRetryDue(0U, 1U));
	CHECK(IsSkirmishAIRecoveryRetryDue(1U, 1U));
	CHECK(IsSkirmishAIRecoveryBoundedGraceExpired(1U, 0xFFFFFFFEU));
	CHECK(GetSkirmishAIRecoveryRetryFrame(0xFFFFFFFEU, 3U) == 1U);
	CHECK(GetSkirmishAIRecoveryRetryFrame(0xFFFFFFFFU, 1U) == 1U);
}

static void TestSkirmishAIReplayEpoch()
{
	UnicodeString unmarked = L"Aug 14 2026 21:00:00";
	CHECK(GetSkirmishAIReplayEpoch(unmarked) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(!ReplayVersionUsesSkirmishAILivenessRecovery(unmarked));

	// Live games always use the current and recovery paths. Replays retain the
	// behavior selected by their recording epoch; an unknown epoch is legacy.
	const Int replayEpochs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
	const Bool expectedReplayCurrentBehavior[] =
		{ FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedReplayRecoveryBehavior[] =
		{ FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedRecoveryCRCFields[] =
		{ FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedCancellationOwnership[] =
		{ FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedUnownedQueueFailover[] =
		{ FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedBoundedFailover[] =
		{ FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedResourceWorkerPreservation[] =
		{ FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedStrategyBehavior[] =
		{ FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, FALSE };
	const Bool expectedProductionBehavior[] =
		{ FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, FALSE };
	for (Int i = 0; i < (Int)ARRAY_SIZE(replayEpochs); ++i)
	{
		CHECK(ShouldUseSkirmishAICurrentBehavior(FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIRecoveryBehavior(FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIRecoveryNativeHoleOwnership(FALSE, replayEpochs[i]));
		CHECK(ShouldIncludeSkirmishAIRecoveryCRCFields(FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIRecoveryCancellationOwnership(
			FALSE, replayEpochs[i]));
		CHECK(ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
			FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
			FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIRecoveryBoundedFailover(
			FALSE, replayEpochs[i]));
		CHECK(ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
			FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
			FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAIStrategyBehavior(FALSE, replayEpochs[i]));
		CHECK(ShouldIncludeSkirmishAIStrategyCRCFields(FALSE, replayEpochs[i]));
		CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, replayEpochs[i])
			== expectedReplayCurrentBehavior[i]);
		CHECK(ShouldUseSkirmishAIRecoveryBehavior(TRUE, replayEpochs[i])
			== expectedReplayRecoveryBehavior[i]);
		CHECK(ShouldUseSkirmishAIRecoveryNativeHoleOwnership(TRUE, replayEpochs[i])
			== expectedRecoveryCRCFields[i]);
		CHECK(ShouldIncludeSkirmishAIRecoveryCRCFields(TRUE, replayEpochs[i])
			== expectedRecoveryCRCFields[i]);
		CHECK(ShouldUseSkirmishAIRecoveryCancellationOwnership(
			TRUE, replayEpochs[i]) == expectedCancellationOwnership[i]);
		CHECK(ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
			TRUE, replayEpochs[i]) == expectedCancellationOwnership[i]);
		CHECK(ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
			TRUE, replayEpochs[i]) == expectedUnownedQueueFailover[i]);
		CHECK(ShouldUseSkirmishAIRecoveryBoundedFailover(
			TRUE, replayEpochs[i]) == expectedBoundedFailover[i]);
		CHECK(ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
			TRUE, replayEpochs[i]) == expectedBoundedFailover[i]);
		CHECK(ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
			TRUE, replayEpochs[i]) == expectedResourceWorkerPreservation[i]);
		CHECK(ShouldUseSkirmishAIStrategyBehavior(TRUE, replayEpochs[i])
			== expectedStrategyBehavior[i]);
		CHECK(ShouldIncludeSkirmishAIStrategyCRCFields(TRUE, replayEpochs[i])
			== expectedStrategyBehavior[i]);
		CHECK(ShouldUseSkirmishAIProductionBehavior(TRUE, replayEpochs[i])
			== expectedProductionBehavior[i]);
		CHECK(ShouldIncludeSkirmishAIProductionCRCFields(TRUE, replayEpochs[i])
			== expectedProductionBehavior[i]);
	}

	UnicodeString livenessOnly = unmarked;
	MarkReplayVersionForSkirmishAILivenessRecovery(livenessOnly);
	CHECK(livenessOnly.compare(L"Aug 14 2026 21:00:00 [SkirmishAILiveness=1]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(livenessOnly) == SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(livenessOnly));
	CHECK(!ShouldUseSkirmishAICurrentBehavior(true, SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS));

	UnicodeString adaptiveGlobalRngEpoch = unmarked;
	MarkReplayVersionForSkirmishAIAdaptiveGlobalRngEpoch(adaptiveGlobalRngEpoch);
	CHECK(adaptiveGlobalRngEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=11]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(adaptiveGlobalRngEpoch) == SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(adaptiveGlobalRngEpoch));
	CHECK(ShouldUseSkirmishAICurrentBehavior(true, SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG));
	CHECK(!ShouldUseSkirmishAICounterRng(true, SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG));

	UnicodeString currentEpoch = unmarked;
	MarkReplayVersionForSkirmishAICurrentEpoch(currentEpoch);
	CHECK(currentEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=12]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(currentEpoch) == SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(currentEpoch));
	CHECK(ShouldUseSkirmishAICurrentBehavior(true, SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG));
	CHECK(ShouldUseSkirmishAICounterRng(true, SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG));
	MarkReplayVersionForSkirmishAICurrentEpoch(currentEpoch);
	CHECK(currentEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=12]") == 0);

	UnicodeString recordingWithoutCounterPlanning = unmarked;
	MarkReplayVersionForSkirmishAIRecordingCapability(
		recordingWithoutCounterPlanning, FALSE);
	CHECK(GetSkirmishAIReplayEpoch(recordingWithoutCounterPlanning) ==
		SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG);
	UnicodeString recordingWithCounterPlanning = unmarked;
	MarkReplayVersionForSkirmishAIRecordingCapability(
		recordingWithCounterPlanning, TRUE);
	CHECK(GetSkirmishAIReplayEpoch(recordingWithCounterPlanning) ==
		SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG);

	UnicodeString architectureRecordingEpoch = unmarked;
	MarkReplayVersionForSkirmishAIRecordingEpoch(architectureRecordingEpoch);
#if defined(_WIN64)
	CHECK(BuildSupportsSkirmishAICounterRngPlanning());
	CHECK(GetSkirmishAIReplayEpoch(architectureRecordingEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG);
#else
	CHECK(!BuildSupportsSkirmishAICounterRngPlanning());
	CHECK(GetSkirmishAIReplayEpoch(architectureRecordingEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG);
#endif
	CHECK(GetSkirmishAIReplayRecordingEpoch() ==
		GetSkirmishAIReplayEpoch(architectureRecordingEpoch));
	// The recording header keeps the path marker first and the architecture's
	// AI marker last. Playback must restamp each released epoch exactly.
	for (Int supportsCounterRng = 0; supportsCounterRng < 2; ++supportsCounterRng)
	{
		UnicodeString recordingHeader = unmarked;
		MarkReplayVersionForPathfindQueueCurrentEpoch(recordingHeader);
		MarkReplayVersionForSkirmishAIRecordingCapability(
			recordingHeader, supportsCounterRng != 0);
		CHECK(GetPathfindQueueReplayEpoch(recordingHeader) ==
			PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
		CHECK(GetSkirmishAIReplayEpoch(recordingHeader) ==
			(supportsCounterRng ? SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG :
				SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG));
	}
	for (Int epoch = SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
		epoch <= SKIRMISH_AI_REPLAY_EPOCH_LATEST; ++epoch)
	{
		UnicodeString compatibleHeader = unmarked;
		MarkReplayVersionForPathfindQueueCurrentEpoch(compatibleHeader);
		MarkReplayVersionForSkirmishAIPlaybackCompatibilityEpoch(
			compatibleHeader, epoch);
		CHECK(GetSkirmishAIReplayEpoch(compatibleHeader) == epoch);
		CHECK(GetPathfindQueueReplayEpoch(compatibleHeader) ==
			PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
		CHECK(ShouldUseSkirmishAIAdaptiveGlobalRng(TRUE, epoch) ==
			(epoch >= SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG));
		CHECK(ShouldUseSkirmishAICounterRng(TRUE, epoch) ==
			(epoch == SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG));
		UnicodeString repeatedHeader = compatibleHeader;
		MarkReplayVersionForSkirmishAIPlaybackCompatibilityEpoch(
			repeatedHeader, epoch);
		CHECK(repeatedHeader == compatibleHeader);
	}
	UnicodeString unknownCompatibilityHeader = unmarked;
	MarkReplayVersionForSkirmishAIPlaybackCompatibilityEpoch(
		unknownCompatibilityHeader, SKIRMISH_AI_REPLAY_EPOCH_LATEST + 1);
	CHECK(GetSkirmishAIReplayEpoch(unknownCompatibilityHeader) ==
		SKIRMISH_AI_REPLAY_EPOCH_LATEST);
	CHECK(!ShouldUseSkirmishAICurrentBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS));
	CHECK(!ShouldUseSkirmishAIRecoveryBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS));

	// Epoch 2 is already shipped behavior and must continue to select current
	// behavior without opting into the newer recovery behavior.
	UnicodeString preservedEpoch2 =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2]";
	CHECK(GetSkirmishAIReplayEpoch(preservedEpoch2) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(preservedEpoch2));
	CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	CHECK(!ShouldUseSkirmishAIRecoveryBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	MarkReplayVersionForSkirmishAICurrentEpoch(preservedEpoch2);
	CHECK(preservedEpoch2.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2]") == 0);
	UnicodeString compatibilityEpoch2 = unmarked;
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityEpoch2);
	CHECK(compatibilityEpoch2.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(compatibilityEpoch2) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	CHECK(!ShouldUseSkirmishAIRecoveryBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityEpoch2);
	CHECK(compatibilityEpoch2.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2]") == 0);

	// Epoch 3 retains the recovery behavior and CRC layout used by existing
	// recordings. The explicit compatibility writer remains idempotent.
	UnicodeString recoveryEpoch3 = unmarked;
	MarkReplayVersionForSkirmishAIRecoveryEpoch(recoveryEpoch3);
	CHECK(recoveryEpoch3.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(recoveryEpoch3) == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(recoveryEpoch3));
	CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY));
	CHECK(ShouldUseSkirmishAIRecoveryBehavior(TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY));
	CHECK(!ShouldIncludeSkirmishAIRecoveryCRCFields(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY));
	CHECK(!ShouldUseSkirmishAIRecoveryNativeHoleOwnership(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY));
	CHECK(ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
		ShouldUseSkirmishAIRecoveryNativeHoleOwnership(
			TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY), true));
	MarkReplayVersionForSkirmishAIRecoveryEpoch(recoveryEpoch3);
	CHECK(recoveryEpoch3.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3]") == 0);
	UnicodeString compatibilityEpoch3 = recoveryEpoch3;
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityEpoch3);
	CHECK(compatibilityEpoch3.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3]") == 0);

	// Epoch 4 retains its original decision rules and exact CRC extension:
	// deadline, factory ID, and production ID, without cancellation ownership.
	UnicodeString recoveryCRCEpoch = unmarked;
	MarkReplayVersionForSkirmishAIRecoveryCRCEpoch(recoveryCRCEpoch);
	CHECK(recoveryCRCEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=4]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(recoveryCRCEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(recoveryCRCEpoch));
	CHECK(ShouldUseSkirmishAICurrentBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC));
	CHECK(ShouldUseSkirmishAIRecoveryBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC));
	CHECK(ShouldIncludeSkirmishAIRecoveryCRCFields(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC));
	CHECK(!ShouldUseSkirmishAIRecoveryCancellationOwnership(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC));
	CHECK(!ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC));
	const bool epoch4StoredCancellationOwnership = false;
	const bool epoch4EffectiveCancellationOwnership =
		!ShouldUseSkirmishAIRecoveryCancellationOwnership(
			TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC) ||
		epoch4StoredCancellationOwnership;
	CHECK(ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
		epoch4EffectiveCancellationOwnership, true, true));
	const bool epoch4EffectiveReplacementAttempted =
		ShouldUseSkirmishAIRecoveryCancellationOwnership(
			TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC) && true;
	CHECK(ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, true, epoch4EffectiveReplacementAttempted, false));
	CHECK(ShouldUseSkirmishAIRecoveryNativeHoleOwnership(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC));
	CHECK(!ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
		ShouldUseSkirmishAIRecoveryNativeHoleOwnership(
			TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC), true));
	MarkReplayVersionForSkirmishAIRecoveryCRCEpoch(recoveryCRCEpoch);
	CHECK(recoveryCRCEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=4]") == 0);

	// Epoch 5 opts into cancellation ownership in both decisions and CRC state,
	// but retains the original refusal to replace an unowned bounded queue.
	UnicodeString recoveryOwnershipEpoch = unmarked;
	MarkReplayVersionForSkirmishAIRecoveryOwnershipEpoch(recoveryOwnershipEpoch);
	CHECK(recoveryOwnershipEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=5]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(recoveryOwnershipEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP);
	CHECK(ShouldUseSkirmishAIRecoveryCancellationOwnership(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP));
	CHECK(ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP));
	CHECK(!ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
		false, true, true));
	CHECK(!ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP));
	CHECK(!ShouldDeferSkirmishAIRecoveryStalledDisposition(
		false, true, true, false));
	MarkReplayVersionForSkirmishAIRecoveryOwnershipEpoch(
		recoveryOwnershipEpoch);
	CHECK(recoveryOwnershipEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=5]") == 0);

	// Epoch 6 keeps the epoch-5 CRC layout and enables only the independently
	// funded, non-cancelling unowned-queue failover.
	UnicodeString nonCancellingFailoverEpoch = unmarked;
	MarkReplayVersionForSkirmishAINonCancellingFailoverEpoch(
		nonCancellingFailoverEpoch);
	CHECK(nonCancellingFailoverEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=6]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(nonCancellingFailoverEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER);
	CHECK(ShouldUseSkirmishAIRecoveryCancellationOwnership(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER));
	CHECK(ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER));
	CHECK(ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER));
	CHECK(!ShouldUseSkirmishAIRecoveryBoundedFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER));
	CHECK(!ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER));
	CHECK(ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, false,
		ShouldUseSkirmishAIRecoveryBoundedFailover(
			TRUE, SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER), true));
	MarkReplayVersionForSkirmishAINonCancellingFailoverEpoch(
		nonCancellingFailoverEpoch);
	CHECK(nonCancellingFailoverEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=6]") == 0);

	// Epoch 7 keeps its additional CRC field and decision gate, consuming one
	// successful paid-queue failover per recovery episode without opting into
	// resource-worker preservation.
	UnicodeString boundedFailoverEpoch = unmarked;
	MarkReplayVersionForSkirmishAIBoundedFailoverEpoch(boundedFailoverEpoch);
	CHECK(boundedFailoverEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=7]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(boundedFailoverEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER);
	CHECK(ShouldUseSkirmishAIRecoveryBoundedFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER));
	CHECK(ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER));
	CHECK(!ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER));
	CHECK(!ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
		true, false, false,
		ShouldUseSkirmishAIRecoveryBoundedFailover(
			TRUE, SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER), true));
	MarkReplayVersionForSkirmishAIBoundedFailoverEpoch(boundedFailoverEpoch);
	CHECK(boundedFailoverEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=7]") == 0);

	// Epoch 8 retains the complete epoch-7 CRC layout and enables only the
	// low-cash recovery resource-worker routing change.
	UnicodeString resourceWorkerPreservationEpoch = unmarked;
	MarkReplayVersionForSkirmishAIResourceWorkerPreservationEpoch(
		resourceWorkerPreservationEpoch);
	CHECK(resourceWorkerPreservationEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=8]") == 0);

	// Epoch 9 remains the strategy-controller compatibility epoch. New
	// recordings use epoch 10 and add the Stage 3 production policy while
	// retaining every earlier behavior and CRC field.
	UnicodeString strategyControllerEpoch = unmarked;
	MarkReplayVersionForSkirmishAIStrategyControllerEpoch(strategyControllerEpoch);
	CHECK(strategyControllerEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=9]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(strategyControllerEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_STRATEGY_CONTROLLER);
	CHECK(ShouldUseSkirmishAICurrentBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_STRATEGY_CONTROLLER));
	CHECK(ShouldUseSkirmishAIRecoveryBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_STRATEGY_CONTROLLER));
	CHECK(ShouldUseSkirmishAIStrategyBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_STRATEGY_CONTROLLER));
	CHECK(ShouldIncludeSkirmishAIStrategyCRCFields(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_STRATEGY_CONTROLLER));
	MarkReplayVersionForSkirmishAIStrategyControllerEpoch(
		strategyControllerEpoch);
	CHECK(strategyControllerEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=9]") == 0);
	UnicodeString productionEpoch = unmarked;
	MarkReplayVersionForSkirmishAIProductionEpoch(productionEpoch);
	CHECK(productionEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=10]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(productionEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION);
	CHECK(ShouldUseSkirmishAICurrentBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION));
	CHECK(ShouldUseSkirmishAIRecoveryBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION));
	CHECK(ShouldUseSkirmishAIStrategyBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION));
	CHECK(ShouldIncludeSkirmishAIStrategyCRCFields(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION));
	CHECK(ShouldUseSkirmishAIProductionBehavior(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION));
	CHECK(ShouldIncludeSkirmishAIProductionCRCFields(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_PRODUCTION));
	MarkReplayVersionForSkirmishAIProductionEpoch(productionEpoch);
	CHECK(productionEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=10]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(resourceWorkerPreservationEpoch) ==
		SKIRMISH_AI_REPLAY_EPOCH_RESOURCE_WORKER_PRESERVATION);
	CHECK(ShouldUseSkirmishAIRecoveryBoundedFailover(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RESOURCE_WORKER_PRESERVATION));
	CHECK(ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RESOURCE_WORKER_PRESERVATION));
	CHECK(ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
		TRUE, SKIRMISH_AI_REPLAY_EPOCH_RESOURCE_WORKER_PRESERVATION));
	MarkReplayVersionForSkirmishAIResourceWorkerPreservationEpoch(
		resourceWorkerPreservationEpoch);
	CHECK(resourceWorkerPreservationEpoch.compare(
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=8]") == 0);

	UnicodeString unrelatedSuffix = L"Aug 14 2026 21:00:00 [SkirmishAILiveness=2]";
	CHECK(GetSkirmishAIReplayEpoch(unrelatedSuffix) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(!ReplayVersionUsesSkirmishAILivenessRecovery(unrelatedSuffix));
	UnicodeString futureEpoch = L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=13]";
	CHECK(GetSkirmishAIReplayEpoch(futureEpoch) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString malformedEpoch = L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=x]";
	CHECK(GetSkirmishAIReplayEpoch(malformedEpoch) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString mixedMarkers =
		L"Aug 14 2026 21:00:00 [SkirmishAILiveness=1] [SkirmishAIEpoch=3]";
	CHECK(GetSkirmishAIReplayEpoch(mixedMarkers) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString mixedEpoch2Markers =
		L"Aug 14 2026 21:00:00 [SkirmishAILiveness=1] [SkirmishAIEpoch=2]";
	CHECK(GetSkirmishAIReplayEpoch(mixedEpoch2Markers) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString mixedEpoch2And3 =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2] [SkirmishAIEpoch=3]";
	CHECK(GetSkirmishAIReplayEpoch(mixedEpoch2And3) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString mixedEpoch3And4 =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3] [SkirmishAIEpoch=4]";
	CHECK(GetSkirmishAIReplayEpoch(mixedEpoch3And4) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString malformedRecoveryEpoch =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=03]";
	CHECK(GetSkirmishAIReplayEpoch(malformedRecoveryEpoch) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString unterminatedRecoveryEpoch =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3";
	CHECK(GetSkirmishAIReplayEpoch(unterminatedRecoveryEpoch) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString trailingRecoveryGarbage =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3] trailing";
	CHECK(GetSkirmishAIReplayEpoch(trailingRecoveryGarbage) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString duplicateEpoch2Markers =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2] [SkirmishAIEpoch=2]";
	CHECK(GetSkirmishAIReplayEpoch(duplicateEpoch2Markers) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString duplicateMarkers =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3] [SkirmishAIEpoch=3]";
	CHECK(GetSkirmishAIReplayEpoch(duplicateMarkers) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString unknownThenCurrent =
		L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=13] [SkirmishAIEpoch=12]";
	CHECK(GetSkirmishAIReplayEpoch(unknownThenCurrent) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString malformedThenLiveness =
		L"Aug 14 2026 21:00:00 [SkirmishAILiveness=x] [SkirmishAILiveness=1]";
	CHECK(GetSkirmishAIReplayEpoch(malformedThenLiveness) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString unknownWriterInput = futureEpoch;
	MarkReplayVersionForSkirmishAICurrentEpoch(unknownWriterInput);
	CHECK(unknownWriterInput.compare(futureEpoch) == 0);
	CHECK(!ShouldUseSkirmishAICurrentBehavior(true, 13));
	CHECK(ShouldUseSkirmishAICounterRng(false, SKIRMISH_AI_REPLAY_EPOCH_LEGACY));
	UnicodeString compatibilityMalformed = malformedRecoveryEpoch;
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityMalformed);
	CHECK(compatibilityMalformed == malformedRecoveryEpoch);
	UnicodeString compatibilityDuplicate = duplicateEpoch2Markers;
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityDuplicate);
	CHECK(compatibilityDuplicate == duplicateEpoch2Markers);
	UnicodeString compatibilityMixed = mixedEpoch2And3;
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityMixed);
	CHECK(compatibilityMixed == mixedEpoch2And3);
	UnicodeString compatibilityUnknown = futureEpoch;
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(compatibilityUnknown);
	CHECK(compatibilityUnknown == futureEpoch);
	CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, 9));
	CHECK(ShouldUseSkirmishAIRecoveryBehavior(TRUE, 9));
	CHECK(ShouldIncludeSkirmishAIRecoveryCRCFields(TRUE, 9));
	CHECK(ShouldUseSkirmishAIRecoveryNativeHoleOwnership(TRUE, 9));
	CHECK(ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(TRUE, 9));
	CHECK(ShouldUseSkirmishAIStrategyBehavior(TRUE, 9));
	CHECK(ShouldIncludeSkirmishAIStrategyCRCFields(TRUE, 9));
	CHECK(!ShouldUseSkirmishAIStrategyBehavior(TRUE, 8));
	CHECK(!ShouldIncludeSkirmishAIStrategyCRCFields(TRUE, 8));
}

static void TestPathfindQueueReplayEpoch()
{
	UnicodeString unmarked = L"Aug 14 2026 21:00:00";
	CHECK(GetPathfindQueueReplayEpoch(unmarked) == PATHFIND_QUEUE_REPLAY_EPOCH_LEGACY);
	CHECK(!ReplayVersionUsesPathfindQueueCapacity(unmarked));

	UnicodeString marked = unmarked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(marked);
	CHECK(GetPathfindQueueReplayEpoch(marked) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(ReplayVersionUsesPathfindQueueCapacity(marked));
	CHECK(GetSkirmishAIReplayEpoch(marked) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(ShouldUsePathfindQueueCapacity(false, false));
	CHECK(!ShouldUsePathfindQueueCapacity(true, false));
	CHECK(ShouldUsePathfindQueueCapacity(true, true));
	CHECK(GetPathfindCellInfoCapacityForPolicy(false, false, false) == PATHFIND_CELL_INFO_LEGACY_CAPACITY);
	CHECK(GetPathfindCellInfoCapacityForPolicy(true, true, false) == PATHFIND_CELL_INFO_LEGACY_CAPACITY);
	CHECK(GetPathfindCellInfoCapacityForPolicy(true, true, true) == PATHFIND_CELL_INFO_CURRENT_CAPACITY);
	CHECK(GetPathfindCellInfoCapacityForPolicy(true, false, false) == PATHFIND_CELL_INFO_CURRENT_CAPACITY);

	UnicodeString pathCompatibility = unmarked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(pathCompatibility);
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(pathCompatibility);
	CHECK(pathCompatibility.compare(
		L"Aug 14 2026 21:00:00 [PathfindQueueEpoch=1] [SkirmishAIEpoch=2]") == 0);
	CHECK(GetPathfindQueueReplayEpoch(pathCompatibility) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayEpoch(pathCompatibility) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	MarkReplayVersionForPathfindQueueCurrentEpoch(pathCompatibility);
	MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(pathCompatibility);
	CHECK(pathCompatibility.compare(
		L"Aug 14 2026 21:00:00 [PathfindQueueEpoch=1] [SkirmishAIEpoch=2]") == 0);

	UnicodeString pathEpoch2 =
		L"Aug 14 2026 21:00:00 [PathfindQueueEpoch=1] [SkirmishAIEpoch=2]";
	CHECK(GetPathfindQueueReplayEpoch(pathEpoch2) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayEpoch(pathEpoch2) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, GetSkirmishAIReplayEpoch(pathEpoch2)));
	CHECK(!ShouldUseSkirmishAIRecoveryBehavior(TRUE, GetSkirmishAIReplayEpoch(pathEpoch2)));

	UnicodeString combined = unmarked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(combined);
	MarkReplayVersionForSkirmishAICurrentEpoch(combined);
	CHECK(GetPathfindQueueReplayEpoch(combined) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(combined.compare(L"Aug 14 2026 21:00:00 [PathfindQueueEpoch=1] [SkirmishAIEpoch=12]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(combined) ==
		SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG);
	CHECK(ShouldUseSkirmishAICurrentBehavior(TRUE, GetSkirmishAIReplayEpoch(combined)));
	CHECK(ShouldUseSkirmishAIRecoveryBehavior(TRUE, GetSkirmishAIReplayEpoch(combined)));
	MarkReplayVersionForPathfindQueueCurrentEpoch(combined);
	MarkReplayVersionForSkirmishAICurrentEpoch(combined);
	CHECK(combined.compare(L"Aug 14 2026 21:00:00 [PathfindQueueEpoch=1] [SkirmishAIEpoch=12]") == 0);

	UnicodeString pathAdaptiveGlobalRng = unmarked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(pathAdaptiveGlobalRng);
	MarkReplayVersionForSkirmishAIAdaptiveGlobalRngEpoch(pathAdaptiveGlobalRng);
	CHECK(GetPathfindQueueReplayEpoch(pathAdaptiveGlobalRng) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayEpoch(pathAdaptiveGlobalRng) ==
		SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG);
	CHECK(!ShouldUseSkirmishAICounterRng(true,
		GetSkirmishAIReplayEpoch(pathAdaptiveGlobalRng)));

	UnicodeString pathLiveness = unmarked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(pathLiveness);
	MarkReplayVersionForSkirmishAILivenessRecovery(pathLiveness);
	CHECK(GetPathfindQueueReplayEpoch(pathLiveness) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayEpoch(pathLiveness) == SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS);

	UnicodeString duplicate = marked;
	duplicate.concat(GetPathfindQueueReplayMarker());
	CHECK(GetPathfindQueueReplayEpoch(duplicate) == PATHFIND_QUEUE_REPLAY_EPOCH_LEGACY);

	UnicodeString unknown = unmarked;
	unknown.concat(L" [PathfindQueueEpoch=2]");
	CHECK(GetPathfindQueueReplayEpoch(unknown) == PATHFIND_QUEUE_REPLAY_EPOCH_LEGACY);

	UnicodeString idempotent = marked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(idempotent);
	CHECK(idempotent == marked);
}

static void TestSkirmishAICorrectnessPolicies()
{
	const Int currentPlanToken = 1;
	const Int previousPlanToken = 2;
	const Int *currentPlan = &currentPlanToken;
	const Int *previousPlan = &previousPlanToken;
	CHECK(GetSkirmishAutomaticConstructionPlan(currentPlan, previousPlan) == currentPlan);
	CHECK(GetSkirmishAutomaticConstructionPlan(static_cast<const Int *>(nullptr), previousPlan) == nullptr);

	CHECK(GetSupplyDefenseMemoryFrames(30) == 300);

	CHECK(ShouldPreferSkirmishRetaliation(true, true));
	CHECK(!ShouldPreferSkirmishRetaliation(false, true));
	CHECK(!ShouldPreferSkirmishRetaliation(true, false));

	CHECK(HasSkirmishRallyOffset(1.01f, 0.0f));
	CHECK(HasSkirmishRallyOffset(-1.01f, 0.0f));
	CHECK(HasSkirmishRallyOffset(0.0f, 1.01f));
	CHECK(HasSkirmishRallyOffset(0.0f, -1.01f));
	CHECK(!HasSkirmishRallyOffset(1.0f, -1.0f));
	CHECK(!HasSkirmishRallyOffset(-1.0f, 1.0f));
}

static void TestSkirmishAIProductionPolicies()
{
	SkirmishAICostRange costs = MakeSkirmishAICostRange();
	costs = AddSkirmishAIUnitCost(costs, 800, 2, 5);
	costs = AddSkirmishAIUnitCost(costs, 300, 1, 3);
	CHECK(costs.minimumCost == 1900);
	CHECK(costs.plannedCost == 4900);

	CHECK(GetSkirmishAIPriorityBandWidth(100, SKIRMISH_AI_DIFFICULTY_EASY) == 0);
	CHECK(GetSkirmishAIPriorityBandWidth(100, SKIRMISH_AI_DIFFICULTY_NORMAL) == 5);
	CHECK(GetSkirmishAIPriorityBandWidth(100, SKIRMISH_AI_DIFFICULTY_HARD) == 10);
	CHECK(GetSkirmishAIPriorityBandWidth(-5, SKIRMISH_AI_DIFFICULTY_NORMAL) == 1);
	CHECK(GetSkirmishAIPriorityBandWidth(-5, SKIRMISH_AI_DIFFICULTY_HARD) == 2);
	CHECK(IsSkirmishAIPriorityAdmitted(95, 100, SKIRMISH_AI_DIFFICULTY_NORMAL));
	CHECK(!IsSkirmishAIPriorityAdmitted(94, 100, SKIRMISH_AI_DIFFICULTY_NORMAL));
	CHECK(!IsSkirmishAIPriorityAdmitted(99, 100, SKIRMISH_AI_DIFFICULTY_EASY));
	CHECK(IsSkirmishAIPriorityAdmitted(
		(-2147483647 - 1), (-2147483647 - 1), SKIRMISH_AI_DIFFICULTY_HARD));
	CHECK(ShouldReplaceSkirmishAIHighestPriority(false, -99999, -99999));
	CHECK(ShouldReplaceSkirmishAIHighestPriority(false, (-2147483647 - 1), (-2147483647 - 1)));
	CHECK(!ShouldReplaceSkirmishAIHighestPriority(true, -100000, -99999));

	CHECK(GetSkirmishAIReserve(1200, 900) == 1200);
	CHECK(GetSkirmishAIReserve(1200, 1800) == 1800);
	CHECK(IsSkirmishAIAffordable(5000, 1900, 1200));
	CHECK(!IsSkirmishAIAffordable(3000, 1900, 1200));
	CHECK(ShouldRetrySkirmishAIReserve(false, true, false));
	CHECK(!ShouldRetrySkirmishAIReserve(true, true, false));
	CHECK(!ShouldRetrySkirmishAIReserve(false, false, false));
	CHECK(!ShouldRetrySkirmishAIReserve(false, true, true));
	CHECK(IsSkirmishAICriticalRebuildStartable(true, true, true, true, true));
	CHECK(!IsSkirmishAICriticalRebuildStartable(true, true, true, false, true));
	CHECK(!IsSkirmishAICriticalRebuildStartable(true, true, true, true, false));
	CHECK(ShouldSkirmishAIConsiderRebuild(true, false, false, false));
	CHECK(ShouldSkirmishAIConsiderRebuild(false, true, false, false));
	CHECK(ShouldSkirmishAIConsiderRebuild(false, false, true, true));
	CHECK(!ShouldSkirmishAIConsiderRebuild(false, false, true, false));
	CHECK(!ShouldSkirmishAIConsiderRebuild(false, false, false, true));

	CHECK(GetSkirmishAIProductionEntryWaitFrames(900, 25.0f, true, 4) == 675);
	CHECK(GetSkirmishAIProductionEntryWaitFrames(900, 25.0f, false, 4) == 900);
	CHECK(GetSkirmishAIProductionEntryWaitFrames(0, 25.0f, true, 4) == 0);
	CHECK(GetSkirmishAIProductionEntryWaitFrames(-1, 25.0f, true, 4) == 0);
	CHECK(GetSkirmishAIProductionEntryWaitFrames(900, -25.0f, true, 4) == 900);
	CHECK(GetSkirmishAIProductionEntryWaitFrames(900, 125.0f, true, 4) == 0);
	CHECK(GetSkirmishAIProductionEntryWaitFrames(900, 25.0f, false, 0) == 900);
	CHECK(GetSkirmishAIUnitsRemainingAfterProductionEntry(0, 4) == 0);
	CHECK(GetSkirmishAIUnitsRemainingAfterProductionEntry(4, 4) == 0);
	CHECK(GetSkirmishAIUnitsRemainingAfterProductionEntry(5, 4) == 1);
	CHECK(GetSkirmishAIUnitsRemainingAfterProductionEntry(5, 0) == 4);
	Int batchWorkOrderEntries = 0;
	Int requiredUnitsRemaining = 1;
	Int optionalUnitsRemaining = 3;
	while (requiredUnitsRemaining > 0) {
		requiredUnitsRemaining = GetSkirmishAIUnitsRemainingAfterProductionEntry(
			requiredUnitsRemaining, 4);
		batchWorkOrderEntries++;
	}
	while (optionalUnitsRemaining > 0) {
		optionalUnitsRemaining = GetSkirmishAIUnitsRemainingAfterProductionEntry(
			optionalUnitsRemaining, 4);
		batchWorkOrderEntries++;
	}
	CHECK(batchWorkOrderEntries == 2);
	Int projectedLoads[2] = { 100, 100 };
	Int compatibleFactories[2] = { 1, 1 };
	CHECK(GetSkirmishAILeastLoadedFactoryIndex(projectedLoads, compatibleFactories, 2) == 0);
	projectedLoads[0] = AddSkirmishAIFrameValue(projectedLoads[0], 300);
	CHECK(GetSkirmishAILeastLoadedFactoryIndex(projectedLoads, compatibleFactories, 2) == 1);
	compatibleFactories[1] = 0;
	CHECK(GetSkirmishAILeastLoadedFactoryIndex(projectedLoads, compatibleFactories, 2) == 0);
	CHECK(GetSkirmishAILeastLoadedFactoryIndex(projectedLoads, compatibleFactories, 0) == -1);

	CHECK(!IsSkirmishAIGroundRouteTarget(true, false, false, false));
	CHECK(!IsSkirmishAIGroundRouteTarget(false, true, false, false));
	CHECK(IsSkirmishAIGroundRouteTarget(false, false, true, false));
	CHECK(IsSkirmishAIGroundRouteTarget(false, false, false, true));

	CHECK(GetSkirmishAICounterFitScore(1000, 0, 0, 1000, 1000, 0, 0) == 300);
	CHECK(GetSkirmishAICounterFitScore(1000, 0, 0, 1000, 500, 0, 0) == 150);
	CHECK(GetSkirmishAICounterFitScore(1000, 1000, 0, 2000, 1000, 0, 0) == 75);
	CHECK(GetSkirmishAICounterFitScore(0, 0, 0, 1000, 1000, 1000, 1000) == 0);

	SkirmishAITeamScoreInput input;
	input.configuredPriority = 10;
	input.counterFitScore = 300;
	input.resources = 6000;
	input.minimumCost = 2000;
	input.plannedCost = 4000;
	input.reserve = 1000;
	input.factoryWaitFrames = 15 * 30;
	input.logicFramesPerSecond = 30;
	input.routeClass = SKIRMISH_AI_ROUTE_GROUND_REACHABLE;
	input.recentLossCount = 1;
	input.recentPathFailureCount = 1;

	input.difficulty = SKIRMISH_AI_DIFFICULTY_HARD;
	SkirmishAITeamScoreResult hard = ScoreSkirmishAITeam(input);
	CHECK(hard.economyScore == 100);
	CHECK(hard.factoryWaitScore == -125);
	CHECK(hard.routeScore == 50);
	CHECK(hard.lossScore == -75);
	CHECK(hard.pathFailureScore == -100);
	CHECK(hard.rawContextScore == 150);
	CHECK(hard.finalScore == 10150);

	input.difficulty = SKIRMISH_AI_DIFFICULTY_NORMAL;
	CHECK(ScoreSkirmishAITeam(input).finalScore == 10075);
	input.difficulty = SKIRMISH_AI_DIFFICULTY_EASY;
	CHECK(ScoreSkirmishAITeam(input).finalScore == 10037);
	input.resources = 4500;
	CHECK(ScoreSkirmishAITeam(input).economyScore == -150);
	input.resources = 2500;
	CHECK(ScoreSkirmishAITeam(input).economyScore == -350);

	input.counterFitScore = 3000;
	input.resources = 0;
	input.recentLossCount = 20;
	input.recentPathFailureCount = 20;
	input.factoryWaitFrames = 300 * 30;
	input.routeClass = SKIRMISH_AI_ROUTE_GROUND_UNREACHABLE;
	input.difficulty = SKIRMISH_AI_DIFFICULTY_HARD;
	CHECK(ScoreSkirmishAITeam(input).counterFitScore == 300);
	CHECK(ScoreSkirmishAITeam(input, true).counterFitScore == 500);
	CHECK(ScoreSkirmishAITeam(input).factoryWaitScore == -250);
	CHECK(ScoreSkirmishAITeam(input).lossScore == -225);
	CHECK(ScoreSkirmishAITeam(input).pathFailureScore == -300);
	CHECK(ScoreSkirmishAITeam(input).rawContextScore == -1000);
	CHECK(ScoreSkirmishAITeam(input, true).rawContextScore == -875);

	CHECK(GetSkirmishAIRouteScore(SKIRMISH_AI_ROUTE_GROUND_REACHABLE) == 50);
	CHECK(GetSkirmishAIRouteScore(SKIRMISH_AI_ROUTE_UNKNOWN) == 0);
	CHECK(GetSkirmishAIRouteScore(SKIRMISH_AI_ROUTE_MIXED_UNREACHABLE) == -125);
	CHECK(GetSkirmishAIRouteScore(SKIRMISH_AI_ROUTE_GROUND_UNREACHABLE) == -250);
	CHECK(IsSkirmishAITeamScoreTie(1200, 1200));
	CHECK(!IsSkirmishAITeamScoreTie(1200, 1199));
	CHECK(GetSkirmishAIFinalScore(2147483647, 1000, 100) ==
		(__int64)2147483647 * 1000 + 1000);
	CHECK(!IsSkirmishAITeamScoreTie(
		(__int64)2147483647 * 1000, (__int64)2147483646 * 1000));
	CHECK(GetSkirmishAITieSelectionIndex(1, 99) == 0);
	CHECK(GetSkirmishAITieSelectionIndex(3, 2) == 2);
}

static void TestSkirmishAIStage3Policies()
{
	// Stage 3 keeps a single aggregate reserve boundary for ordinary
	// production.  The max operation and the affordability check must remain
	// well-defined at the signed 32-bit limit.
	const Int maximumInt = 2147483647;
	CHECK(GetSkirmishAIAggregateReserve(1200, 900, 1000) == 1200);
	CHECK(GetSkirmishAIAggregateReserve(1200, 1800, 1000) == 1800);
	CHECK(GetSkirmishAIAggregateReserve(1200, 900, 2400) == 2400);
	CHECK(GetSkirmishAIAggregateReserve(-1, -2, -3) == 0);
	CHECK(GetSkirmishAIAggregateReserve(
		maximumInt, maximumInt, maximumInt) == maximumInt);
	CHECK(IsSkirmishAIAffordable(maximumInt, maximumInt, 0));
	CHECK(!IsSkirmishAIAffordable(maximumInt, maximumInt, 1));
	CHECK(!IsSkirmishAIAffordable(maximumInt, maximumInt, maximumInt));
	CHECK(IsSkirmishAIAffordable(0, -1, -1));
	CHECK(!IsSkirmishAIAffordable(-1, 0, 0));

	// Fortify and Assault deliberately amplify the same deterministic counter
	// fit signal; Balanced retains the unweighted score.
	CHECK(GetSkirmishAIProductionCounterFitScore(
		300, SKIRMISH_AI_PRODUCTION_BALANCED) == 300);
	CHECK(GetSkirmishAIProductionCounterFitScore(
		300, SKIRMISH_AI_PRODUCTION_FORTIFY) == 400);
	CHECK(GetSkirmishAIProductionCounterFitScore(
		300, SKIRMISH_AI_PRODUCTION_ASSAULT) == 500);
	CHECK(GetSkirmishAIProductionCounterFitScore(
		-20, SKIRMISH_AI_PRODUCTION_ASSAULT) == 0);
	CHECK(GetSkirmishAIProductionCounterFitScore(
		1000, SKIRMISH_AI_PRODUCTION_FORTIFY) == 400);
	SkirmishAITeamScoreInput weightedScoreInput;
	weightedScoreInput.configuredPriority = 1;
	weightedScoreInput.counterFitScore = 500;
	weightedScoreInput.resources = 0;
	weightedScoreInput.minimumCost = 0;
	weightedScoreInput.plannedCost = 0;
	weightedScoreInput.reserve = 0;
	weightedScoreInput.factoryWaitFrames = 0;
	weightedScoreInput.logicFramesPerSecond = LOGICFRAMES_PER_SECOND;
	weightedScoreInput.routeClass = SKIRMISH_AI_ROUTE_UNKNOWN;
	weightedScoreInput.recentLossCount = 0;
	weightedScoreInput.recentPathFailureCount = 0;
	weightedScoreInput.difficulty = SKIRMISH_AI_DIFFICULTY_HARD;
	CHECK(ScoreSkirmishAITeam(weightedScoreInput).counterFitScore == 300);
	CHECK(ScoreSkirmishAITeam(weightedScoreInput, true).counterFitScore == 500);

	// Offensive powers require a live hostile slot with at least one object.
	// This prevents the historical end-of-game fallback to a human ally.
	CHECK(IsSkirmishAIOffensiveTargetEligible(true, true, true));
	CHECK(!IsSkirmishAIOffensiveTargetEligible(false, true, true));
	CHECK(!IsSkirmishAIOffensiveTargetEligible(true, false, true));
	CHECK(!IsSkirmishAIOffensiveTargetEligible(true, true, false));
	CHECK(HasSkirmishAIOffensiveTargetObjects(false, true, false));
	CHECK(!HasSkirmishAIOffensiveTargetObjects(false, false, true));
	CHECK(HasSkirmishAIOffensiveTargetObjects(true, false, true));
	CHECK(!HasSkirmishAIOffensiveTargetObjects(true, true, false));
	CHECK(IsSkirmishAIOffensiveTargetObjectEligible(
		true, true, false, false, false, false, false, false,
		false, false, false, false, 1));
	CHECK(!IsSkirmishAIOffensiveTargetObjectEligible(
		true, true, true, false, false, false, false, false,
		false, false, false, false, 1000));
	CHECK(IsSkirmishAIOffensiveTargetObjectEligible(
		true, true, false, false, false, false, true, false,
		false, false, false, false, 1000));
	CHECK(IsSkirmishAIOffensiveTargetObjectEligible(
		true, true, false, false, false, false, false, true,
		false, false, false, false, 1000));
	CHECK(!IsSkirmishAIOffensiveTargetObjectEligible(
		true, true, false, false, false, false, false, false,
		true, false, false, false, 1000));
	CHECK(IsSkirmishAIOffensiveTargetObjectEligible(
		true, true, false, false, false, false, false, false,
		false, false, false, false, 0));

	// Aggregate demand scales with every usable center and counts only assigned,
	// available collectors. One pending collector suppresses duplicate work.
	CHECK(GetSkirmishAISupplyCollectorDemand(8, 3, false) == 5);
	CHECK(GetSkirmishAISupplyCollectorDemand(8, 8, false) == 0);
	CHECK(GetSkirmishAISupplyCollectorDemand(8, 3, true) == 0);
	CHECK(GetSkirmishAISupplyCollectorDemand(-1, -1, false) == 0);
	Int aggregateDeficit = AddSkirmishAISupplyCollectorDeficit(0, 4, 6);
	aggregateDeficit = AddSkirmishAISupplyCollectorDeficit(
		aggregateDeficit, 4, 0);
	CHECK(aggregateDeficit == 4);
	CHECK(IsSkirmishAIPendingCollectorEntry(true, false, 1, 1, false));
	CHECK(!IsSkirmishAIPendingCollectorEntry(false, true, 1, 1, false));
	CHECK(!IsSkirmishAIPendingCollectorEntry(true, true, 2, 2, false));
	CHECK(!IsSkirmishAIPendingCollectorEntry(true, false, 1, 2, false));
	CHECK(IsSkirmishAIPendingCollectorEntry(false, false, 0, 1, true));
	CHECK(!IsSkirmishAIPendingCollectorEntry(false, false, 0, 1, false));
	CHECK(IsSkirmishAIEligibleCollector(
		true, true, true, true, false, false, false, false, false,
		false, false, false, true));
	CHECK(!IsSkirmishAIEligibleCollector(
		true, true, true, true, false, false, false, false, false,
		false, false, true, true));
	CHECK(!IsSkirmishAIEligibleCollector(
		true, true, true, true, false, false, false, false, false,
		true, false, false, true));
	CHECK(ShouldRouteSkirmishAICollectorToCenter(true, 4, 3));
	CHECK(!ShouldRouteSkirmishAICollectorToCenter(true, 4, 4));
	CHECK(!ShouldRouteSkirmishAICollectorToCenter(false, 4, 0));

	// A satisfied OR block cannot create demand for one of its alternatives.
	CHECK(ShouldInspectSkirmishAIPrerequisiteAlternative(true, false));
	CHECK(!ShouldInspectSkirmishAIPrerequisiteAlternative(true, true));
	CHECK(!ShouldInspectSkirmishAIPrerequisiteAlternative(false, false));

	// Spending uses the current aggregate, and free recruitment is considered
	// from an otherwise valid candidate before paid affordability is relevant.
	CHECK(GetFreshSkirmishAIProductionReserve(1200, 1800, 1000) == 1800);
	CHECK(GetFreshSkirmishAIProductionReserve(1200, 900, 2400) == 2400);
	CHECK(!CanSkirmishAISpendWithAuthorization(2500, 1000, 2000, 800,
		SKIRMISH_AI_SPEND_AUTHORIZATION_NONE));
	CHECK(CanSkirmishAISpendWithAuthorization(2500, 1000, 2000, 800,
		SKIRMISH_AI_SPEND_AUTHORIZATION_BUILDER));
	CHECK(CanSkirmishAISpendWithAuthorization(2500, 1000, 2000, 800,
		SKIRMISH_AI_SPEND_AUTHORIZATION_COLLECTOR));
	CHECK(CanSkirmishAISpendWithAuthorization(2500, 1000, 2000, 800,
		SKIRMISH_AI_SPEND_AUTHORIZATION_PRIORITY_STRUCTURE));
	CHECK(!CanSkirmishAISpendWithAuthorization(1799, 1000, 2000, 800,
		SKIRMISH_AI_SPEND_AUTHORIZATION_COLLECTOR));
	CHECK(ShouldTrySkirmishAIRecruitBeforePaidTraining(0, 1, true, false));
	CHECK(ShouldTrySkirmishAIRecruitBeforePaidTraining(0, 1, true, true));
	CHECK(!ShouldTrySkirmishAIRecruitBeforePaidTraining(1, 1, true, false));
	CHECK(ShouldTrySkirmishAIRecruitBeforePaidTraining(0, 1, false, false));
	CHECK(ShouldSelectSkirmishAIReinforcementCandidate(true, false, false));
	CHECK(ShouldSelectSkirmishAIReinforcementCandidate(false, true, true));
	CHECK(!ShouldSelectSkirmishAIReinforcementCandidate(false, false, true));
	CHECK(!ShouldSelectSkirmishAIReinforcementCandidate(false, true, false));
	CHECK(IsSkirmishAIOperationalProducer(
		true, false, false, false, false, false, false, false, false, true));
	CHECK(!IsSkirmishAIOperationalProducer(
		true, false, false, false, false, false, false, true, false, true));
	CHECK(!IsSkirmishAIOperationalProducer(
		true, false, false, false, false, false, false, false, true, true));
	CHECK(IsSkirmishAICollectorProducerEligible(false, false));
	CHECK(IsSkirmishAICollectorProducerEligible(true, true));
	CHECK(!IsSkirmishAICollectorProducerEligible(true, false));
	CHECK(ShouldRetainSkirmishAIDepletedCollectorProduction(
		true, true, 0.25f));
	CHECK(!ShouldRetainSkirmishAIDepletedCollectorProduction(
		true, true, 0.0f));
	CHECK(!ShouldRetainSkirmishAIDepletedCollectorProduction(
		true, false, 0.75f));
	CHECK(IsSkirmishAIDepletedCollectorQueueMappingUnambiguous(3, 3, false));
	CHECK(!IsSkirmishAIDepletedCollectorQueueMappingUnambiguous(2, 3, false));
	CHECK(!IsSkirmishAIDepletedCollectorQueueMappingUnambiguous(3, 3, true));
	CHECK(GetSkirmishAIDepletedCollectorRetainedOrderQuantity(4, 3) == 3);
	CHECK(GetSkirmishAIDepletedCollectorRetainedOrderQuantity(2, 3) == 2);
	CHECK(GetSkirmishAIDepletedCollectorRetainedOrderQuantity(2, 0) == 0);

	// Structure admission preserves explicit script priority, then repairs
	// critical infrastructure and revenue before ordinary production and
	// fortification. A depleted supply point is therefore not a revenue
	// priority, while a prerequisite-only depot remains admissible separately.
	CHECK(GetSkirmishAIStructurePriority(
		true, false, false, false, false, false, false, false, false) == 1000);
	CHECK(GetSkirmishAIStructurePriority(
		false, true, false, false, false, false, false, false, false) == 900);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, true, false, false, false, false, false, false) == 850);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, false, true, false, false, false, false, false) == 800);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, false, false, true, false, false, false, false) == 775);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, false, false, false, true, false, false, false) == 700);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, false, false, false, false, true, true, false) == 675);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, false, false, false, false, true, false, false) == 550);
	CHECK(GetSkirmishAIStructurePriority(
		false, false, false, false, false, false, false, false, true) == 625);
	CHECK(IsSkirmishAIAlternateIncomeStructure(true, false, false, false));
	CHECK(IsSkirmishAIAlternateIncomeStructure(false, true, false, false));
	CHECK(IsSkirmishAIAlternateIncomeStructure(false, false, true, false));
	CHECK(IsSkirmishAIAlternateIncomeStructure(false, false, false, true));
	CHECK(!IsSkirmishAIAlternateIncomeStructure(false, false, false, false));
	CHECK(IsSkirmishAISupplyRevenueAvailable(
		true, true, false, true, false, 1));
	CHECK(!IsSkirmishAISupplyRevenueAvailable(
		false, true, false, true, false, 1));
	CHECK(!IsSkirmishAISupplyRevenueAvailable(
		true, false, false, true, false, 1));
	CHECK(!IsSkirmishAISupplyRevenueAvailable(
		true, true, true, true, false, 1));
	CHECK(!IsSkirmishAISupplyRevenueAvailable(
		true, true, false, false, false, 1));
	CHECK(!IsSkirmishAISupplyRevenueAvailable(
		true, true, false, true, true, 1));
	CHECK(!IsSkirmishAISupplyRevenueAvailable(
		true, true, false, true, false, 0));
	CHECK(!ShouldBuildSkirmishAIPrerequisiteSupplyCenter(true, false, false));
	CHECK(ShouldBuildSkirmishAIPrerequisiteSupplyCenter(false, false, false));
	CHECK(!ShouldBuildSkirmishAIPrerequisiteSupplyCenter(false, true, false));
	CHECK(!ShouldBuildSkirmishAIPrerequisiteSupplyCenter(false, false, true));
	CHECK(ShouldKeepSkirmishAISuperweaponConstructionPending(
		true, true, true, false, false, false, false, true, false));
	CHECK(ShouldKeepSkirmishAISuperweaponConstructionPending(
		true, true, true, false, false, false, false, false, true));
	CHECK(!ShouldKeepSkirmishAISuperweaponConstructionPending(
		true, true, true, false, false, false, true, true, false));
	CHECK(!ShouldKeepSkirmishAISuperweaponConstructionPending(
		true, false, true, false, false, false, false, true, false));
	CHECK(ShouldContinueSkirmishAIStrategyAfterRecovery(false, false));
	CHECK(ShouldContinueSkirmishAIStrategyAfterRecovery(false, true));
	CHECK(!ShouldContinueSkirmishAIStrategyAfterRecovery(true, false));
	CHECK(ShouldContinueSkirmishAIStrategyAfterRecovery(true, true));
	CHECK(DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, false, false, false, false, false, false));
	CHECK(!DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, true, false, false, false, false, false));
	CHECK(!DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, false, true, false, false, false, false));
	CHECK(!DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, false, false, true, false, false, false));
	CHECK(!DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, false, false, false, true, false, false));
	CHECK(!DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, false, false, false, false, true, false));
	CHECK(!DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
		true, true, false, false, false, false, false, true));

	// Outside current Stage 3 Fortify behavior, native script selection remains
	// unchanged. Inside it, strategic powers require a pending attempt and the
	// exact source chosen for that compatible power.
	CHECK(ShouldUseSkirmishAIStrategicPowerSource(
		false, true, true, false, false));
	CHECK(ShouldUseSkirmishAIStrategicPowerSource(
		true, false, true, false, false));
	CHECK(ShouldUseSkirmishAIStrategicPowerSource(
		true, true, false, false, false));
	CHECK(ShouldUseSkirmishAIStrategicPowerSource(
		true, true, true, true, true));
	CHECK(!ShouldUseSkirmishAIStrategicPowerSource(
		true, true, true, true, false));
	CHECK(!ShouldUseSkirmishAIStrategicPowerSource(
		true, true, true, false, true));
	CHECK(ShouldLockSkirmishAIStrategicPowerSource(true, false, true));
	CHECK(!ShouldLockSkirmishAIStrategicPowerSource(true, true, true));
	CHECK(!ShouldLockSkirmishAIStrategicPowerSource(false, false, true));
	CHECK(!ShouldLockSkirmishAIStrategicPowerSource(true, false, false));
	CHECK(!ShouldFailSkirmishAIStrategicPowerSourceLock(true, true, true));
	CHECK(ShouldFailSkirmishAIStrategicPowerSourceLock(true, true, false));
	CHECK(!ShouldFailSkirmishAIStrategicPowerSourceLock(true, false, false));
	CHECK(!ShouldFailSkirmishAIStrategicPowerSourceLock(false, true, false));
	// Version 10 lacks the exact power/source identity and therefore restores
	// unlocked. A valid lock rejects every later request independent of the
	// newly requested power. A paused, disabled, destroyed, or module-less
	// accepted source consumes the attempt as failed before the lock is cleared.
	CHECK(!GetSkirmishAIStrategicPowerSourceLockForVersion(10, true));
	CHECK(GetSkirmishAIStrategicPowerSourceLockForVersion(11, true));
	CHECK(!GetSkirmishAIStrategicPowerSourceLockForVersion(11, false));
	CHECK(ShouldRejectSkirmishAIStrategicPowerRequest(true, true));
	CHECK(!ShouldRejectSkirmishAIStrategicPowerRequest(true, false));
	CHECK(IsSkirmishAIStrategicPowerDispatchable(false, false));
	CHECK(!IsSkirmishAIStrategicPowerDispatchable(true, false));
	CHECK(!IsSkirmishAIStrategicPowerDispatchable(false, true));
	// The persisted TeamID cursor partitions equal-priority candidates into the
	// after-cursor pass and a wrapped pass, and advances only on success.
	CHECK(GetSkirmishAIReinforcementCursorForVersion(10, 17) == 0);
	CHECK(GetSkirmishAIReinforcementCursorForVersion(11, 17) == 17);
	CHECK(GetSkirmishAIReinforcementRoundRobinPass(18, 17) == 0);
	CHECK(GetSkirmishAIReinforcementRoundRobinPass(16, 17) == 1);
	CHECK(GetSkirmishAIReinforcementRoundRobinPass(17, 17) == 2);
	CHECK(AdvanceSkirmishAIRoundRobinCursor(17, 18, false) == 17);
	CHECK(AdvanceSkirmishAIRoundRobinCursor(17, 18, true) == 18);
	// An accepted delayed command retains its still-present lock. A synchronous
	// trigger has already cleared the lock, while a rejected call has no
	// acceptance acknowledgement and is cleared by the caller-side resolver.
	CHECK(ShouldRetainSkirmishAIStrategicPowerDispatchLock(true, true));
	CHECK(!ShouldRetainSkirmishAIStrategicPowerDispatchLock(true, false));
	CHECK(!ShouldRetainSkirmishAIStrategicPowerDispatchLock(false, true));

	// Entering Fortify opens one episode and one bounded assembly deadline.
	// The deadline is intentionally difficulty-specific.
	const GameDifficulty difficulties[] = {
		DIFFICULTY_EASY, DIFFICULTY_NORMAL, DIFFICULTY_HARD };
	const UnsignedInt deadlineSeconds[] = { 240, 180, 120 };
	Int difficultyIndex;
	for (difficultyIndex = 0; difficultyIndex < 3; ++difficultyIndex)
	{
		SkirmishStrategyState state;
		InitializeSkirmishStrategyState(&state, 0);
		SkirmishStrategyMetrics metrics = MakeSkirmishStrategyMetrics();
		metrics.baseIntegrity = 34;
		SkirmishStrategyDecision decision = EvaluateSkirmishStrategy(
			state, metrics, difficulties[difficultyIndex], 0, true);
		CHECK(decision.modeChanged);
		CHECK(decision.nextState.currentMode == SKIRMISH_STRATEGY_FORTIFY);
		CHECK(decision.nextState.fortifyAttemptStatus ==
			SKIRMISH_STRATEGY_ATTEMPT_PENDING);
		CHECK(decision.nextState.superweaponAttemptStatus ==
			SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED);
		CHECK(decision.nextState.assaultAssemblyDeadlineActive);
		CHECK(decision.nextState.assaultAssemblyDeadlineFrame ==
			deadlineSeconds[difficultyIndex] * LOGICFRAMES_PER_SECOND);
	}

	SkirmishStrategyState entryState;
	InitializeSkirmishStrategyState(&entryState, 0);
	SkirmishStrategyMetrics metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 34;
	SkirmishStrategyDecision entry = EvaluateSkirmishStrategy(
		entryState, metrics, DIFFICULTY_HARD, 0, true);

	// At the hard deadline, a target and an assembled force immediately escape
	// Fortify into Assault. Both unresolved attempts become terminal for the
	// episode, and the deadline is cleared.
	SkirmishStrategyState deadlineState = entry.nextState;
	deadlineState.nextEvaluationFrame = 120 * LOGICFRAMES_PER_SECOND;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.viableAssaultForceAssembled = true;
	SkirmishStrategyDecision deadlineAssault = EvaluateSkirmishStrategy(
		deadlineState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(deadlineAssault.modeChanged);
	CHECK(deadlineAssault.nextState.currentMode == SKIRMISH_STRATEGY_ASSAULT);
	CHECK(deadlineAssault.reason == SKIRMISH_STRATEGY_REASON_ASSAULT_FORTIFY_DEADLINE);
	CHECK(deadlineAssault.nextState.fortifyAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);
	CHECK(deadlineAssault.nextState.superweaponAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);
	CHECK(!deadlineAssault.nextState.assaultAssemblyDeadlineActive);
	CHECK(deadlineAssault.nextState.assaultAssemblyDeadlineFrame == 0);

	// With neither a viable force nor a target, the same deadline exits to
	// Balanced and still closes the pending attempts.
	SkirmishStrategyState balancedDeadlineState = entry.nextState;
	balancedDeadlineState.nextEvaluationFrame = 120 * LOGICFRAMES_PER_SECOND;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.hasStrategicTarget = false;
	metrics.viableAssaultForceAssembled = false;
	SkirmishStrategyDecision deadlineBalanced = EvaluateSkirmishStrategy(
		balancedDeadlineState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(deadlineBalanced.modeChanged);
	CHECK(deadlineBalanced.nextState.currentMode == SKIRMISH_STRATEGY_BALANCED);
	CHECK(deadlineBalanced.reason == SKIRMISH_STRATEGY_REASON_BALANCED_FORTIFY_DEADLINE);
	CHECK(deadlineBalanced.nextState.fortifyAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);
	CHECK(deadlineBalanced.nextState.superweaponAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);
	CHECK(!deadlineBalanced.nextState.assaultAssemblyDeadlineActive);

	// Emergency Fortify triggers retain precedence only before the hard bound.
	// At the deadline a critical base still exits to Assault when a target and
	// usable force exist; severe threat without them exits to Balanced.
	SkirmishStrategyState criticalDeadlineState = entry.nextState;
	criticalDeadlineState.nextEvaluationFrame =
		120 * LOGICFRAMES_PER_SECOND;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 34;
	metrics.hasStrategicTarget = true;
	metrics.viableAssaultForceAssembled = true;
	SkirmishStrategyDecision criticalDeadline = EvaluateSkirmishStrategy(
		criticalDeadlineState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(criticalDeadline.modeChanged);
	CHECK(criticalDeadline.nextState.currentMode ==
		SKIRMISH_STRATEGY_ASSAULT);
	CHECK(criticalDeadline.reason ==
		SKIRMISH_STRATEGY_REASON_ASSAULT_FORTIFY_DEADLINE);

	SkirmishStrategyState threatDeadlineState = entry.nextState;
	threatDeadlineState.nextEvaluationFrame =
		120 * LOGICFRAMES_PER_SECOND;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.immediateThreat = 85;
	metrics.hasStrategicTarget = false;
	metrics.viableAssaultForceAssembled = false;
	SkirmishStrategyDecision threatDeadline = EvaluateSkirmishStrategy(
		threatDeadlineState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(threatDeadline.modeChanged);
	CHECK(threatDeadline.nextState.currentMode ==
		SKIRMISH_STRATEGY_BALANCED);
	CHECK(threatDeadline.reason ==
		SKIRMISH_STRATEGY_REASON_BALANCED_FORTIFY_DEADLINE);

	// Re-entering a later Fortify episode resets the terminal attempt state and
	// receives a new bounded deadline; a previous failure cannot authorize a
	// second attempt inside the same episode.
	SkirmishStrategyState resetState = deadlineBalanced.nextState;
	resetState.currentMode = SKIRMISH_STRATEGY_BALANCED;
	resetState.modeEntryFrame = 0;
	resetState.nextEvaluationFrame = 0;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.baseIntegrity = 34;
	SkirmishStrategyDecision reset = EvaluateSkirmishStrategy(
		resetState, metrics, DIFFICULTY_HARD, 0, true);
	CHECK(reset.nextState.currentMode == SKIRMISH_STRATEGY_FORTIFY);
	CHECK(reset.nextState.fortifyAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_PENDING);
	CHECK(reset.nextState.superweaponAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED);
	CHECK(reset.nextState.assaultAssemblyDeadlineActive);

	// A conventional force that assembles before the deadline succeeds in the
	// Fortify episode. The not-started superweapon attempt becomes terminal
	// without being retried after the mode change.
	SkirmishStrategyState successState;
	InitializeSkirmishStrategyState(&successState, 0);
	successState.currentMode = SKIRMISH_STRATEGY_FORTIFY;
	successState.modeEntryFrame = 0;
	successState.nextEvaluationFrame = 0;
	successState.fortifyAttemptStatus = SKIRMISH_STRATEGY_ATTEMPT_PENDING;
	successState.superweaponAttemptStatus = SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED;
	successState.assaultAssemblyDeadlineActive = true;
	successState.assaultAssemblyDeadlineFrame = 1000000;
	metrics = MakeSkirmishStrategyMetrics();
	metrics.viableAssaultForceAssembled = true;
	SkirmishStrategyDecision pendingAssault = EvaluateSkirmishStrategy(
		successState, metrics, DIFFICULTY_HARD, 0, true);
	CHECK(!pendingAssault.modeChanged);
	CHECK(pendingAssault.nextState.pendingMode == SKIRMISH_STRATEGY_ASSAULT);
	successState = pendingAssault.nextState;
	successState.nextEvaluationFrame = 120 * LOGICFRAMES_PER_SECOND;
	SkirmishStrategyDecision assembledAssault = EvaluateSkirmishStrategy(
		successState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(assembledAssault.modeChanged);
	CHECK(assembledAssault.nextState.currentMode == SKIRMISH_STRATEGY_ASSAULT);
	CHECK(assembledAssault.reason == SKIRMISH_STRATEGY_REASON_ASSAULT_FORCE_ASSEMBLED);
	CHECK(assembledAssault.nextState.fortifyAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED);
	CHECK(assembledAssault.nextState.superweaponAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);
	CHECK(!assembledAssault.nextState.assaultAssemblyDeadlineActive);

	// A Fortify attempt that already expired stays failed even if the force
	// assembles while an Assault transition is still pending.
	SkirmishStrategyState failedAssemblyState = successState;
	failedAssemblyState.fortifyAttemptStatus =
		SKIRMISH_STRATEGY_ATTEMPT_FAILED;
	SkirmishStrategyDecision failedAssemblyAssault = EvaluateSkirmishStrategy(
		failedAssemblyState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(failedAssemblyAssault.modeChanged);
	CHECK(failedAssemblyAssault.nextState.currentMode ==
		SKIRMISH_STRATEGY_ASSAULT);
	CHECK(failedAssemblyAssault.nextState.fortifyAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);

	// A successful superweapon attempt is already terminal: it selects Assault
	// but must survive the episode transition without being rewritten as a
	// second attempt.
	SkirmishStrategyState firedState = successState;
	firedState.pendingMode = SKIRMISH_STRATEGY_NONE;
	firedState.pendingSinceFrame = 0;
	firedState.superweaponAttemptStatus = SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED;
	firedState.nextEvaluationFrame = 0;
	SkirmishStrategyDecision pendingFired = EvaluateSkirmishStrategy(
		firedState, metrics, DIFFICULTY_HARD, 0, true);
	CHECK(!pendingFired.modeChanged);
	CHECK(pendingFired.reason == SKIRMISH_STRATEGY_REASON_PENDING_STARTED);
	firedState = pendingFired.nextState;
	firedState.nextEvaluationFrame = 120 * LOGICFRAMES_PER_SECOND;
	SkirmishStrategyDecision firedAssault = EvaluateSkirmishStrategy(
		firedState, metrics, DIFFICULTY_HARD,
		120 * LOGICFRAMES_PER_SECOND, true);
	CHECK(firedAssault.modeChanged);
	CHECK(firedAssault.reason == SKIRMISH_STRATEGY_REASON_ASSAULT_SUPERWEAPON_FIRED);
	CHECK(firedAssault.nextState.fortifyAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_FAILED);
	CHECK(firedAssault.nextState.superweaponAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED);
}

static void TestSkirmishAITargetingPolicies()
{
	CHECK(IsSkirmishAIIntelEligible(true, true, false, false, false, false));
	CHECK(IsSkirmishAIIntelEligible(true, false, true, false, false, false));
	CHECK(!IsSkirmishAIIntelEligible(true, false, false, false, false, false));
	CHECK(IsSkirmishAIIntelEligible(false, true, false, false, false, false));
	CHECK(!IsSkirmishAIIntelEligible(false, false, true, false, false, false));
	CHECK(!IsSkirmishAIIntelEligible(false, true, false, true, false, false));
	CHECK(IsSkirmishAIIntelEligible(false, true, false, true, true, false));
	CHECK(!IsSkirmishAIIntelEligible(false, true, false, false, true, true));
	CHECK(!IsSkirmishAIKnownCrippled(false, false, false));
	CHECK(IsSkirmishAIKnownCrippled(true, false, false));
	CHECK(!IsSkirmishAIKnownCrippled(true, true, false));
	CHECK(!IsSkirmishAIKnownCrippled(true, false, true));

	CHECK(GetSkirmishAIKnownAssetScore(0, 1000) == 0);
	CHECK(GetSkirmishAIKnownAssetScore(500, 1000) == 150);
	CHECK(GetSkirmishAIKnownAssetScore(1000, 1000) == 300);
	CHECK(GetSkirmishAIKnownAssetScore(1000, 0) == 0);
	CHECK(GetSkirmishAIDistanceScore(100, 100, 500) == 0);
	CHECK(GetSkirmishAIDistanceScore(300, 100, 500) == -150);
	CHECK(GetSkirmishAIDistanceScore(500, 100, 500) == -300);
	CHECK(GetSkirmishAIDistanceScore(100, 100, 100) == 0);

	SkirmishAIEnemyScoreInput input;
	input.knownAssetScore = 225;
	input.targetingThisAI = true;
	input.routeClass = SKIRMISH_AI_TARGET_ROUTE_REACHABLE;
	input.alliedAIsTargeting = 2;
	input.distanceScore = -120;
	input.crippled = false;
	SkirmishAIEnemyScoreResult score = ScoreSkirmishAIEnemy(input);
	CHECK(score.knownAssetScore == 225);
	CHECK(score.retaliationScore == 250);
	CHECK(score.routeScore == 150);
	CHECK(score.allyTargetScore == -300);
	CHECK(score.distanceScore == -120);
	CHECK(score.crippledScore == 0);
	CHECK(score.totalScore == 205);
	input.alliedAIsTargeting = 10;
	input.routeClass = SKIRMISH_AI_TARGET_ROUTE_UNREACHABLE;
	input.crippled = true;
	score = ScoreSkirmishAIEnemy(input);
	CHECK(score.allyTargetScore == -450);
	CHECK(score.routeScore == -150);
	CHECK(score.crippledScore == -600);

	CHECK(ShouldReplaceSkirmishAITargetCandidate(false, 0, 7, 0, 2));
	CHECK(ShouldReplaceSkirmishAITargetCandidate(true, 101, 7, 100, 2));
	CHECK(ShouldReplaceSkirmishAITargetCandidate(true, 100, 2, 100, 7));
	CHECK(!ShouldReplaceSkirmishAITargetCandidate(true, 100, 7, 100, 2));
	CHECK(!ShouldSwitchSkirmishAITarget(true, true, 100, 299));
	CHECK(ShouldSwitchSkirmishAITarget(true, true, 100, 300));
	CHECK(ShouldSwitchSkirmishAITarget(false, true, 100, -1000));
	CHECK(!ShouldSwitchSkirmishAITarget(false, false, 100, 1000));
	CHECK(ShouldEvaluateSkirmishAITarget(false, 150, 150, true));
	CHECK(ShouldEvaluateSkirmishAITarget(true, 100, 150, true));
	CHECK(!ShouldEvaluateSkirmishAITarget(true, 150, 150, false));

	SkirmishAITargetSnapshotState oldState = GetSkirmishAITargetSnapshotState(1, 5, 900);
	CHECK(oldState.enemyPlayerIndex == -1);
	CHECK(oldState.nextEvaluationFrame == 0);
	SkirmishAITargetSnapshotState newState = GetSkirmishAITargetSnapshotState(2, 5, 900);
	CHECK(newState.enemyPlayerIndex == 5);
	CHECK(newState.nextEvaluationFrame == 900);
}

static void TestSkirmishAIFeedbackPolicies()
{
	const UnsignedInt decayFrames = 30 * 30;
	const UnsignedInt pathRateLimitFrames = 5 * 30;
	SkirmishAIFeedbackState state = MakeSkirmishAIFeedbackState();
	CHECK(state.recentLossCount == 0);
	CHECK(state.recentPathFailureCount == 0);
	CHECK(state.lastLossFrame == 0);
	CHECK(state.lastPathFailureFrame == 0);
	CHECK(state.nextDecayFrame == 0);
	CHECK(!state.hasPathFailureFrame);

	state = RecordSkirmishAILoss(state, 0, decayFrames);
	state = RecordSkirmishAILoss(state, 1, decayFrames);
	state = RecordSkirmishAILoss(state, 2, decayFrames);
	state = RecordSkirmishAILoss(state, 3, decayFrames);
	CHECK(state.recentLossCount == 3);
	CHECK(state.lastLossFrame == 3);
	CHECK(state.nextDecayFrame == decayFrames);

	state = RecordSkirmishAIPathFailure(state, 0, pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 1);
	state = RecordSkirmishAIPathFailure(state, pathRateLimitFrames - 1,
		pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 1);
	state = RecordSkirmishAIPathFailure(state, pathRateLimitFrames,
		pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 2);
	state = RecordSkirmishAIPathFailure(state, 2 * pathRateLimitFrames,
		pathRateLimitFrames, decayFrames);
	state = RecordSkirmishAIPathFailure(state, 3 * pathRateLimitFrames,
		pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 3);
	CHECK(state.lastPathFailureFrame == 3 * pathRateLimitFrames);

	SkirmishAIFeedbackState beforeDecay = DecaySkirmishAIFeedback(
		state, decayFrames - 1, decayFrames);
	CHECK(beforeDecay.recentLossCount == 3);
	CHECK(beforeDecay.recentPathFailureCount == 3);
	state = DecaySkirmishAIFeedback(state, decayFrames, decayFrames);
	CHECK(state.recentLossCount == 2);
	CHECK(state.recentPathFailureCount == 2);
	state = DecaySkirmishAIFeedback(state, 3 * decayFrames, decayFrames);
	CHECK(state.recentLossCount == 0);
	CHECK(state.recentPathFailureCount == 0);
	CHECK(state.nextDecayFrame == 0);

	state = MakeSkirmishAIFeedbackState();
	state = RecordSkirmishAILoss(state, 100, decayFrames);
	state = RecordSkirmishAIPathFailure(state, 100, pathRateLimitFrames, decayFrames);
	state = ApplySkirmishAITeamSuccess(state, 101, decayFrames);
	CHECK(state.recentLossCount == 0);
	CHECK(state.recentPathFailureCount == 0);
	CHECK(state.nextDecayFrame == 0);
	state = RecordSkirmishAIPathFailure(state, 101, pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 0);
	state = RecordSkirmishAIPathFailure(state, 100 + pathRateLimitFrames,
		pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 1);
	CHECK(state.nextDecayFrame == 100 + pathRateLimitFrames + decayFrames);

	state = MakeSkirmishAIFeedbackState();
	state = RecordSkirmishAIPathFailure(state, 0, pathRateLimitFrames, decayFrames);
	state = ApplySkirmishAITeamSuccess(state, 1, decayFrames);
	state = RecordSkirmishAIPathFailure(state, 1, pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 0);
	CHECK(state.hasPathFailureFrame);
	state = RecordSkirmishAIPathFailure(state, pathRateLimitFrames,
		pathRateLimitFrames, decayFrames);
	CHECK(state.recentPathFailureCount == 1);

	UnsignedInt wrapStartFrame = 0 - decayFrames;
	state = MakeSkirmishAIFeedbackState();
	state = RecordSkirmishAILoss(state, wrapStartFrame, decayFrames);
	CHECK(state.nextDecayFrame == 0);
	state = DecaySkirmishAIFeedback(state, (UnsignedInt)-1, decayFrames);
	CHECK(state.recentLossCount == 1);
	state = DecaySkirmishAIFeedback(state, 0, decayFrames);
	CHECK(state.recentLossCount == 0);
	CHECK(state.nextDecayFrame == 0);

	state = MakeSkirmishAIFeedbackState();
	state = RecordSkirmishAILoss(state, 50, decayFrames);
	state = RecordSkirmishAIPathFailure(state, 50,
		pathRateLimitFrames, decayFrames);
	SkirmishAIFeedbackState unchangedOwner =
		ResetSkirmishAIFeedbackForOwnerChange(state, false);
	CHECK(unchangedOwner.recentLossCount == 1);
	CHECK(unchangedOwner.recentPathFailureCount == 1);
	SkirmishAIFeedbackState changedOwner =
		ResetSkirmishAIFeedbackForOwnerChange(state, true);
	CHECK(changedOwner.recentLossCount == 0);
	CHECK(changedOwner.recentPathFailureCount == 0);
	CHECK(changedOwner.lastLossFrame == 0);
	CHECK(changedOwner.lastPathFailureFrame == 0);
	CHECK(changedOwner.nextDecayFrame == 0);
	CHECK(!changedOwner.hasPathFailureFrame);

	CHECK(ShouldRecordSkirmishAITeamLoss(true, true, true, false));
	CHECK(!ShouldRecordSkirmishAITeamLoss(false, true, true, false));
	CHECK(!ShouldRecordSkirmishAITeamLoss(true, false, true, false));
	CHECK(!ShouldRecordSkirmishAITeamLoss(true, true, false, false));
	CHECK(!ShouldRecordSkirmishAITeamLoss(true, true, true, true));
	CHECK(ShouldRecordSkirmishAIPathFailure(true, true, true, false, false, false, false));
	CHECK(ShouldRecordSkirmishAIPathFailure(true, true, false, true, false, false, false));
	CHECK(!ShouldRecordSkirmishAIPathFailure(false, true, true, false, false, false, false));
	CHECK(!ShouldRecordSkirmishAIPathFailure(true, false, true, false, false, false, false));
	CHECK(!ShouldRecordSkirmishAIPathFailure(true, true, false, false, false, false, false));
	CHECK(!ShouldRecordSkirmishAIPathFailure(true, true, true, false, true, false, false));
	CHECK(!ShouldRecordSkirmishAIPathFailure(true, true, true, false, false, true, false));
	CHECK(!ShouldRecordSkirmishAIPathFailure(true, true, true, false, false, false, true));

	SkirmishAIFeedbackState oldState = GetSkirmishAIFeedbackSnapshotState(
		1, 3, 2, 10, 20, 30, true);
	CHECK(oldState.recentLossCount == 0);
	CHECK(oldState.recentPathFailureCount == 0);
	CHECK(oldState.lastLossFrame == 0);
	CHECK(oldState.lastPathFailureFrame == 0);
	CHECK(oldState.nextDecayFrame == 0);
	CHECK(!oldState.hasPathFailureFrame);
	SkirmishAIFeedbackState newState = GetSkirmishAIFeedbackSnapshotState(
		2, 3, 2, 10, 20, 30, true);
	CHECK(newState.recentLossCount == 3);
	CHECK(newState.recentPathFailureCount == 2);
	CHECK(newState.lastLossFrame == 10);
	CHECK(newState.lastPathFailureFrame == 20);
	CHECK(newState.nextDecayFrame == 30);
	CHECK(newState.hasPathFailureFrame);
}

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


static void TestSkirmishAITestRunnerContract()
{
#if defined(_WIN64)
	TestPerformanceReceiptOwnerLifecycle();
#endif
	TestSkirmishAITestReceiptContract();
#if defined(_WIN32)
	TestSkirmishAITestReplayRetentionCommitPolicy();
#endif
	TestSkirmishAITestHardAI2v6Contract();
	CHECK(!rts::ShouldUseLiveSimulationPhaseGraph(false, false, 0, 3));
	CHECK(rts::ShouldUseLiveSimulationPhaseGraph(true, false, 0, 3));
	CHECK(!rts::ShouldUseLiveSimulationPhaseGraph(true, true, 2, 3));
	CHECK(rts::ShouldUseLiveSimulationPhaseGraph(true, true, 3, 3));
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

	DirectPathRuntimeMetrics baseline;
	DirectPathRuntimeMetrics current;
	DirectPathRuntimeMetrics frozen;
	memset(&baseline, 0, sizeof(baseline));
	memset(&current, 0, sizeof(current));
	memset(&frozen, 0, sizeof(frozen));
	baseline.resetEpoch = 20;
	current.resetEpoch = 20;
	current.eligibleRequests = 88;
	current.workerExecutedJobs = 88;
	current.authoritativeCommits = 88;
	current.authoritativeMultiWorkerCommits = 88;
	Bool hasFrozenActivity = FALSE;
	Bool awaitingInitialReset = TRUE;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(awaitingInitialReset && !hasFrozenActivity &&
		frozen.workerExecutedJobs == 0 && frozen.authoritativeCommits == 0 &&
		frozen.authoritativeMultiWorkerCommits == 0);
	memset(&current, 0, sizeof(current));
	current.resetEpoch = 21;
	current.eligibleRequests = 7;
	current.submittedJobs = 6;
	current.executedJobs = 6;
	current.workerExecutedJobs = 4;
	current.authoritativeCommits = 3;
	current.authoritativeMultiWorkerCommits = 2;
	current.timeoutCancellations = 1;
	current.peakActiveWorkers = 1;
	current.minimumCallbackCount = 9;
	current.maximumCallbackCount = 27;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(!awaitingInitialReset && hasFrozenActivity &&
		frozen.workerExecutedJobs == 4 &&
		frozen.authoritativeCommits == 3 &&
		frozen.authoritativeMultiWorkerCommits == 2 &&
		frozen.timeoutCancellations == 1);
	memset(&current, 0, sizeof(current));
	current.resetEpoch = 22;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(frozen.workerExecutedJobs == 4 && frozen.authoritativeCommits == 3 &&
		frozen.authoritativeMultiWorkerCommits == 2 &&
		frozen.timeoutCancellations == 1 &&
		frozen.minimumCallbackCount == 9 && frozen.maximumCallbackCount == 27);
	baseline = current;
	memset(&frozen, 0, sizeof(frozen));
	hasFrozenActivity = FALSE;
	awaitingInitialReset = TRUE;
	current.resetEpoch = 23;
	AccumulateSkirmishAITestDirectPathMetrics(&baseline, current, &frozen,
		&hasFrozenActivity, &awaitingInitialReset);
	CHECK(!awaitingInitialReset && !hasFrozenActivity &&
		baseline.resetEpoch == 23 &&
		frozen.authoritativeCommits == 0 &&
		frozen.authoritativeMultiWorkerCommits == 0 &&
		frozen.timeoutCancellations == 0);

	OrdinaryPathRuntimeMetrics ordinaryBaseline;
	OrdinaryPathRuntimeMetrics ordinaryCurrent;
	OrdinaryPathRuntimeMetrics ordinaryFrozen;
	memset(&ordinaryBaseline, 0, sizeof(ordinaryBaseline));
	memset(&ordinaryCurrent, 0, sizeof(ordinaryCurrent));
	memset(&ordinaryFrozen, 0, sizeof(ordinaryFrozen));
	ordinaryBaseline.resetEpoch = 30;
	ordinaryCurrent.resetEpoch = 30;
	ordinaryCurrent.workerExecutedRangeJobs = 91;
	ordinaryCurrent.authoritativeCommits = 90;
	Bool ordinaryAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
		ordinaryCurrent, &ordinaryFrozen, &ordinaryAwaitingInitialReset);
	CHECK(ordinaryAwaitingInitialReset &&
		ordinaryFrozen.workerExecutedRangeJobs == 0 &&
		ordinaryFrozen.authoritativeCommits == 0);
	memset(&ordinaryCurrent, 0, sizeof(ordinaryCurrent));
	ordinaryCurrent.resetEpoch = 31;
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
	ordinaryCurrent.resetEpoch = 32;
	AccumulateSkirmishAITestOrdinaryPathMetrics(&ordinaryBaseline,
		ordinaryCurrent, &ordinaryFrozen, &ordinaryAwaitingInitialReset);
	CHECK(ordinaryFrozen.workerExecutedRangeJobs == 4 &&
		ordinaryFrozen.physicalWorkerMask == 5 &&
		ordinaryFrozen.authoritativeCommits == 3 &&
		ordinaryFrozen.maximumBatchRequests == 6);
	ordinaryBaseline = ordinaryCurrent;
	memset(&ordinaryFrozen, 0, sizeof(ordinaryFrozen));
	ordinaryAwaitingInitialReset = TRUE;
	ordinaryCurrent.resetEpoch = 33;
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
	collisionBaseline.resetEpoch = 50;
	collisionBaseline.authoritativeCommits = 90;
	collisionCurrent = collisionBaseline;
	Bool collisionAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestCollisionMetrics(&collisionBaseline,
		collisionCurrent, &collisionFrozen, &collisionAwaitingInitialReset);
	CHECK(collisionAwaitingInitialReset && collisionFrozen.authoritativeCommits == 0);
	collisionCurrent = rts::CollisionCandidateRuntimeMetrics();
	collisionCurrent.resetEpoch = 51;
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
	collisionCurrent.resetEpoch = 52;
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
	collisionCurrent.resetEpoch = 53;
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
	physicsBaseline.resetEpoch = 60;
	physicsBaseline.acceptedBatches = 80;
	physicsCurrent = physicsBaseline;
	Bool physicsAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 0);
	physicsCurrent = rts::PhysicsIntegrationRuntimeMetrics();
	physicsCurrent.resetEpoch = 62; // New-game and scheduler resets may coalesce.
	physicsCurrent.acceptedBatches = 5;
	physicsCurrent.acceptedPrefixes = 96;
	physicsCurrent.acceptedSubmittedJobs = 4;
	physicsCurrent.acceptedCompletedJobs = 4;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(!physicsAwaitingInitialReset && physicsFrozen.acceptedBatches == 5 &&
		physicsFrozen.acceptedPrefixes == 96);
	physicsCurrent = rts::PhysicsIntegrationRuntimeMetrics();
	physicsCurrent.resetEpoch = 63;
	AccumulateSkirmishAITestPhysicsMetrics(&physicsBaseline,
		physicsCurrent, &physicsFrozen, &physicsAwaitingInitialReset);
	CHECK(physicsFrozen.acceptedBatches == 5 && physicsFrozen.acceptedPrefixes == 96);
	physicsBaseline = physicsCurrent;
	physicsFrozen = rts::PhysicsIntegrationRuntimeMetrics();
	physicsAwaitingInitialReset = TRUE;
	physicsCurrent.resetEpoch = 64;
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
	spatialBaseline.resetEpoch = 70;
	spatialBaseline.capturedArenas = 90;
	spatialCurrent = spatialBaseline;
	Bool spatialAwaitingInitialReset = TRUE;
	AccumulateSkirmishAITestImmutableSpatialMetrics(&spatialBaseline,
		spatialCurrent, &spatialFrozen, &spatialAwaitingInitialReset);
	CHECK(spatialAwaitingInitialReset && spatialFrozen.capturedArenas == 0 &&
		spatialFrozen.healing.authoritativeQueries == 0);
	spatialCurrent = rts::ImmutableSpatialRuntimeMetrics();
	spatialCurrent.resetEpoch = 71;
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
	spatialCurrent.resetEpoch = 72;
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
	spatialCurrent.resetEpoch = 73;
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
}

static void TestSkirmishAILegacySaveCandidateSelection()
{
	CHECK(IsSkirmishAILegacySaveBuilderLive(TRUE, FALSE, FALSE, FALSE));
	CHECK(!IsSkirmishAILegacySaveBuilderLive(TRUE, FALSE, FALSE, TRUE));
	CHECK(!IsSkirmishAILegacySaveBuilderLive(TRUE, TRUE, FALSE, FALSE));
	CHECK(!IsSkirmishAILegacySaveBuilderLive(TRUE, FALSE, TRUE, FALSE));
	CHECK(!IsSkirmishAILegacySaveBuilderLive(FALSE, FALSE, FALSE, FALSE));

	SkirmishAILegacySaveCandidateInput candidate;
	candidate.isComputer = TRUE;
	candidate.isSkirmishAI = TRUE;
	candidate.hasPlayerTemplate = TRUE;
	candidate.hasCompletedPrimaryCenter = TRUE;
	candidate.hasOtherStructure = TRUE;
	candidate.hasBuildInfo = TRUE;
	candidate.hasScoreKeeper = TRUE;
	candidate.compatibleBuilderCount = 1;
	candidate.centerCost = 2000;
	candidate.cash = 5000;
	candidate.reserveAdmitted = TRUE;
	Int selectedPlayerIndex = -1;
	Bool hasSelection = FALSE;
	SkirmishAILegacySaveCandidateInput ineligible = candidate;
	ineligible.hasOtherStructure = FALSE;
	CHECK(!ShouldSelectSkirmishAILegacySaveCandidate(
		hasSelection, selectedPlayerIndex, 1, ineligible));
	CHECK(ShouldSelectSkirmishAILegacySaveCandidate(
		hasSelection, selectedPlayerIndex, 7, candidate));
	selectedPlayerIndex = 7;
	hasSelection = TRUE;
	CHECK(ShouldSelectSkirmishAILegacySaveCandidate(
		hasSelection, selectedPlayerIndex, 3, candidate));
	selectedPlayerIndex = 3;
	CHECK(!ShouldSelectSkirmishAILegacySaveCandidate(
		hasSelection, selectedPlayerIndex, 5, candidate));
	ineligible = candidate;
	ineligible.reserveAdmitted = FALSE;
	CHECK(!ShouldSelectSkirmishAILegacySaveCandidate(
		hasSelection, selectedPlayerIndex, 2, ineligible));
	ineligible = candidate;
	ineligible.cash = 1999;
	CHECK(!ShouldSelectSkirmishAILegacySaveCandidate(
		hasSelection, selectedPlayerIndex, 1, ineligible));
	CHECK(selectedPlayerIndex == 3);
}

static void TestSkirmishAITestRunnerContractContinuation()
{
	Int i;
	const char *recoveryCaseNames[] = {
		"surviving_builder",
		"factory_only",
		"no_path_laststand",
		"repeated_cc",
		"obstructed",
		"low_cash",
		"gla_hole",
		"save_load",
		"disabled_factory"
	};
	const Int expectedRecoveryCases[] = {
		SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER,
		SKIRMISH_AI_RECOVERY_FACTORY_ONLY,
		SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND,
		SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER,
		SKIRMISH_AI_RECOVERY_OBSTRUCTED,
		SKIRMISH_AI_RECOVERY_LOW_CASH,
		SKIRMISH_AI_RECOVERY_GLA_HOLE,
		SKIRMISH_AI_RECOVERY_SAVE_LOAD,
		SKIRMISH_AI_RECOVERY_DISABLED_FACTORY
	};
	for (i = 0; i < 9; ++i)
	{
		Int fixtureCase = -1;
		CHECK(TryParseSkirmishAIRecoveryFixtureCase(recoveryCaseNames[i], &fixtureCase));
		CHECK(fixtureCase == expectedRecoveryCases[i]);
		CHECK(strcmp(GetSkirmishAIRecoveryFixtureCaseName(fixtureCase), recoveryCaseNames[i]) == 0);
	}
	CHECK(strcmp(GetSkirmishAIRecoveryFixtureCaseName(-1), "invalid") == 0);
	CHECK(strcmp(GetSkirmishAIRecoveryFixtureCaseName(SKIRMISH_AI_RECOVERY_FIXTURE_CASE_COUNT), "invalid") == 0);

	const char *recoveryFactionNames[] = {
		"FactionAmerica",
		"FactionAmericaSuperWeaponGeneral",
		"FactionAmericaLaserGeneral",
		"FactionAmericaAirForceGeneral",
		"FactionChina",
		"FactionChinaTankGeneral",
		"FactionChinaInfantryGeneral",
		"FactionChinaNukeGeneral",
		"FactionGLA",
		"FactionGLAToxinGeneral",
		"FactionGLADemolitionGeneral",
		"FactionGLAStealthGeneral"
	};
	const Int expectedRecoveryFactions[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
	};
	for (i = 0; i < 12; ++i)
	{
		Int faction = -1;
		CHECK(TryParseSkirmishAIRecoveryFaction(recoveryFactionNames[i], &faction));
		CHECK(faction == expectedRecoveryFactions[i]);
		CHECK(strcmp(GetSkirmishAIRecoveryFactionName(faction), recoveryFactionNames[i]) == 0);
		CHECK(strcmp(GetSkirmishAIRecoveryFactionTemplateName(faction), recoveryFactionNames[i]) == 0);
	}
	CHECK(strcmp(GetSkirmishAIRecoveryFactionName(-1), "invalid") == 0);
	CHECK(strcmp(GetSkirmishAIRecoveryFactionName(SKIRMISH_AI_RECOVERY_FACTION_COUNT), "invalid") == 0);
	CHECK(strcmp(GetSkirmishAIRecoveryFactionTemplateName(SKIRMISH_AI_RECOVERY_FACTION_COUNT), "invalid") == 0);

	const char *invalidRecoveryCases[] = {
		nullptr,
		"",
		"unknown_case",
		"surviving_builder ",
		" surviving_builder",
		"surviving_builderx"
	};
	for (i = 0; i < 6; ++i)
	{
		Int fixtureCase = 1731;
		CHECK(!TryParseSkirmishAIRecoveryFixtureCase(invalidRecoveryCases[i], &fixtureCase));
		CHECK(fixtureCase == 1731);
	}
	CHECK(!TryParseSkirmishAIRecoveryFixtureCase("surviving_builder", nullptr));

	const char *invalidRecoveryFactions[] = {
		nullptr,
		"",
		"FactionUnknown",
		"FactionAmerica ",
		" FactionAmerica",
		"FactionAmericaX",
		"FactionAmericaTankCommand",
		"FactionAmericaSpecialForces",
		"FactionAmericaAirForce",
		"FactionChinaRedArmy",
		"FactionChinaSpecialWeapons",
		"FactionChinaSecretPolice",
		"FactionGLATerrorCell",
		"FactionGLABiowarCommand",
		"FactionGLAWarlordCommand"
	};
	for (i = 0; i < (Int)(sizeof(invalidRecoveryFactions) / sizeof(invalidRecoveryFactions[0])); ++i)
	{
		Int faction = 1732;
		CHECK(!TryParseSkirmishAIRecoveryFaction(invalidRecoveryFactions[i], &faction));
		CHECK(faction == 1732);
	}
	CHECK(!TryParseSkirmishAIRecoveryFaction("FactionAmerica", nullptr));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_FACTORY_ONLY, SKIRMISH_AI_RECOVERY_FACTION_AMERICA));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_GLA_HOLE, SKIRMISH_AI_RECOVERY_FACTION_CHINA));
	CHECK(IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_FACTORY_ONLY, SKIRMISH_AI_RECOVERY_FACTION_GLA));
	CHECK(IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_DISABLED_FACTORY, SKIRMISH_AI_RECOVERY_FACTION_GLA));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_DISABLED_FACTORY, SKIRMISH_AI_RECOVERY_FACTION_CHINA));
	CHECK(IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_SAVE_LOAD, SKIRMISH_AI_RECOVERY_FACTION_AMERICA));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		-1, SKIRMISH_AI_RECOVERY_FACTION_AMERICA));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_FIXTURE_CASE_COUNT, SKIRMISH_AI_RECOVERY_FACTION_AMERICA));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_FACTORY_ONLY, -1));
	CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
		SKIRMISH_AI_RECOVERY_FACTORY_ONLY, SKIRMISH_AI_RECOVERY_FACTION_COUNT));
	for (i = SKIRMISH_AI_RECOVERY_FACTION_AMERICA;
		i < SKIRMISH_AI_RECOVERY_FACTION_GLA; ++i)
	{
		CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
			SKIRMISH_AI_RECOVERY_FACTORY_ONLY, i));
		CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
			SKIRMISH_AI_RECOVERY_DISABLED_FACTORY, i));
		CHECK(!IsSupportedSkirmishAIRecoveryFixtureCombination(
			SKIRMISH_AI_RECOVERY_GLA_HOLE, i));
	}
	for (i = SKIRMISH_AI_RECOVERY_FACTION_GLA;
		i < SKIRMISH_AI_RECOVERY_FACTION_COUNT; ++i)
	{
		CHECK(IsSupportedSkirmishAIRecoveryFixtureCombination(
			SKIRMISH_AI_RECOVERY_FACTORY_ONLY, i));
		CHECK(IsSupportedSkirmishAIRecoveryFixtureCombination(
			SKIRMISH_AI_RECOVERY_DISABLED_FACTORY, i));
		CHECK(IsSupportedSkirmishAIRecoveryFixtureCombination(
			SKIRMISH_AI_RECOVERY_GLA_HOLE, i));
	}

	CommandLineData commandLineData;
	CHECK(!commandLineData.hasSkirmishAITestRequest());
	CHECK(commandLineData.getSkirmishAITestSeed() == 0);
	CHECK(!commandLineData.hasSkirmishAITest4v2Request());
	CHECK(commandLineData.getSkirmishAITest4v2Seed() == 0);
	CHECK(commandLineData.requestSkirmishAITest(1729));
	CHECK(commandLineData.hasSkirmishAITestRequest());
	CHECK(commandLineData.getSkirmishAITestSeed() == 1729);
	CHECK(!commandLineData.requestSkirmishAITest(1730));
	CHECK(!commandLineData.requestSkirmishAITest4v2(1730));
	CHECK(!commandLineData.requestSkirmishAITestPractical1v7(1730));
	CHECK(!commandLineData.requestSkirmishAIRecoveryTest(1730, 0, 0));

	CommandLineData commandLineData4v2;
	CHECK(commandLineData4v2.requestSkirmishAITest4v2(1730));
	CHECK(commandLineData4v2.hasSkirmishAITest4v2Request());
	CHECK(commandLineData4v2.getSkirmishAITest4v2Seed() == 1730);
	CHECK(!commandLineData4v2.requestSkirmishAITest4v2(1731));
	CHECK(!commandLineData4v2.requestSkirmishAITest(1731));
	CHECK(!commandLineData4v2.requestSkirmishAITestPractical1v7(1731));

	CommandLineData practicalCommandLineData;
	CHECK(!practicalCommandLineData.hasSkirmishAITestPractical1v7Request());
	CHECK(practicalCommandLineData.getSkirmishAITestPractical1v7Seed() == 0);
	CHECK(practicalCommandLineData.requestSkirmishAITestPractical1v7(1732));
	CHECK(practicalCommandLineData.hasSkirmishAITestPractical1v7Request());
	CHECK(practicalCommandLineData.getSkirmishAITestPractical1v7Seed() == 1732);
	CHECK(!practicalCommandLineData.requestSkirmishAITest(1733));
	CHECK(!practicalCommandLineData.requestSkirmishAITest4v2(1733));
	CHECK(!practicalCommandLineData.requestSkirmishAITestPractical1v7(1733));
	CHECK(!commandLineData4v2.requestSkirmishAIRecoveryTest(1731, 0, 0));

	CommandLineData commandLineDataRecovery;
	CHECK(!commandLineDataRecovery.hasSkirmishAIRecoveryTestRequest());
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryTestSeed() == 0);
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryFixtureCase() == 0);
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryFaction() == 0);
	CHECK(commandLineDataRecovery.requestSkirmishAIRecoveryTest(
		1733, SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER,
		SKIRMISH_AI_RECOVERY_FACTION_GLA));
	CHECK(commandLineDataRecovery.hasSkirmishAIRecoveryTestRequest());
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryTestSeed() == 1733);
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryFixtureCase() ==
		SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER);
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryFaction() ==
		SKIRMISH_AI_RECOVERY_FACTION_GLA);
	CHECK(!commandLineDataRecovery.hasSkirmishAITestRequest());
	CHECK(!commandLineDataRecovery.hasSkirmishAITest4v2Request());
	CHECK(!commandLineDataRecovery.requestSkirmishAIRecoveryTest(
		1734, SKIRMISH_AI_RECOVERY_LOW_CASH, SKIRMISH_AI_RECOVERY_FACTION_AMERICA));
	CHECK(!commandLineDataRecovery.requestSkirmishAITest(1734));
	CHECK(!commandLineDataRecovery.requestSkirmishAITest4v2(1734));
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryTestSeed() == 1733);
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryFixtureCase() ==
		SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER);
	CHECK(commandLineDataRecovery.getSkirmishAIRecoveryFaction() ==
		SKIRMISH_AI_RECOVERY_FACTION_GLA);

	CHECK(!IsSkirmishAITestRunnerArmed());
	CHECK(!ShouldBypassFramePacingForSkirmishAITest(FALSE));
	CHECK(ShouldBypassFramePacingForSkirmishAITest(TRUE));
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

	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(1729, &plan);
	CHECK(plan.seed == 1729);
	CHECK(strcmp(plan.mapName, "Maps\\Twilight Flame\\Twilight Flame.map") == 0);

	CHECK(plan.slots[0].state == SLOT_PLAYER);
	CHECK(plan.slots[0].playerTemplate == PLAYERTEMPLATE_OBSERVER);
	CHECK(plan.slots[0].color == -1);
	CHECK(plan.slots[0].startPosition == -1);
	CHECK(plan.slots[0].teamNumber == -1);

	for (i = 1; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
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

	CHECK(IsValidSkirmishAITestReplayResult(42000, 42001, FALSE, FALSE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(0, 0, FALSE, FALSE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(UINT_MAX, 0, FALSE, FALSE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(42000, 42000, FALSE, FALSE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(42000, 42002, FALSE, FALSE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(42000, 42001, TRUE, FALSE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(42000, 42001, FALSE, TRUE, 100, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(42000, 42001, FALSE, FALSE, 0, 200));
	CHECK(!IsValidSkirmishAITestReplayResult(42000, 42001, FALSE, FALSE, 200, 199));
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
	initMemoryManager();
	if (argc == 2 && strcmp(argv[1], "--xfer-crc-snapshot") == 0)
	{
		const int result = RunXferCrcSnapshotTests();
		shutdownMemoryManager();
		return result;
	}
#if defined(_WIN64)
	if (argc == 2 && strcmp(argv[1], "--resolution-archive-policy") == 0)
	{
		TestResolutionArchivePolicy();
		shutdownMemoryManager();
		return s_failures != 0 ? 1 : 0;
	}
	if (argc == 2 && strcmp(argv[1], "--native-logical-audio") == 0)
	{
		printf("Running 36 production-linked, device-free logical audio cases.\n");
		fflush(stdout);
		TestNativeLogicalAudioSeed();
		if (s_failures != 0)
		{
			printf("%d native logical audio test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All native logical audio tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
#endif
	if (argc == 2 && strcmp(argv[1], "--texture-load-queue-contract") == 0)
	{
		TestTextureLoadQueuePublication();
		if (s_failures != 0)
		{
			printf("%d texture load queue contract test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All texture load queue contract tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-runner-contract") == 0)
	{
		TestSkirmishAITestRunnerContract();
		TestSkirmishAITestRunnerContractContinuation();
		if (s_failures != 0)
		{
			printf("%d skirmish AI runner contract test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All skirmish AI runner contract tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-legacy-save-selection") == 0)
	{
		TestSkirmishAILegacySaveCandidateSelection();
		if (s_failures != 0)
		{
			printf("%d skirmish AI legacy-save selection test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All skirmish AI legacy-save selection tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-replay-epoch") == 0)
	{
		TestSkirmishAILivenessPolicies();
		TestSkirmishAIReplayEpoch();
		TestPathfindQueueReplayEpoch();
		if (s_failures != 0)
		{
			printf("%d skirmish AI replay epoch test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All skirmish AI replay epoch tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-recovery") == 0)
	{
		TestSkirmishAIRecoveryPolicies();
		if (s_failures != 0)
		{
			printf("%d skirmish AI recovery policy test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All skirmish AI recovery policy tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-strategy") == 0)
	{
		TestSkirmishAIStrategyPolicies();
		if (s_failures != 0)
		{
			printf("%d skirmish AI strategy policy test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All skirmish AI strategy policy tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--skirmish-ai-stage3") == 0)
	{
		TestSkirmishAIReplayEpoch();
		TestSkirmishAIProductionPolicies();
		TestSkirmishAIStage3Policies();
		if (s_failures != 0)
		{
			printf("%d skirmish AI Stage 3 test(s) failed.\n", s_failures);
			shutdownMemoryManager();
			return 1;
		}
		printf("All skirmish AI Stage 3 tests passed.\n");
		shutdownMemoryManager();
		return 0;
	}

	TestNetworkValidation();
	TestPacketRouterFallbackSelection();
	TestGameCommandParsing();
	TestMalformedGameCommandDeserialization();
	TestWrappedCommandRequiresCompleteWireRecord();
	TestWrappedCommandRejectsMalformedHeader();
	TestWrapperLifecycle();
	TestNetworkReceiveBudget();
	TestStringConversionAndZeroLengthReads();
	TestFrameRateLimitWaitCalculation();
	TestSkirmishAILivenessPolicies();
	TestSkirmishAIRecoveryPolicies();
	TestSkirmishAIStrategyPolicies();
	TestSkirmishAIReplayEpoch();
	TestPathfindQueueReplayEpoch();
	TestSkirmishAICorrectnessPolicies();
	TestSkirmishAIProductionPolicies();
	TestSkirmishAIStage3Policies();
	TestSkirmishAITargetingPolicies();
	TestSkirmishAIFeedbackPolicies();
	TestSkirmishAILegacySaveCandidateSelection();
	TestSkirmishAITestRunnerContract();
	TestSkirmishAITestRunnerContractContinuation();
	TestFrameRateLimitCpuUsage();

	if (s_failures != 0)
	{
		printf("%d runtime regression test(s) failed.\n", s_failures);
		shutdownMemoryManager();
		return 1;
	}

	printf("All runtime regression tests passed.\n");
	shutdownMemoryManager();
	return 0;
}
