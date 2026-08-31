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
#include "Common/SkirmishAIReplayEpoch.h"
#include "Common/PathfindQueueReplayEpoch.h"
#include "GameLogic/SkirmishAIDecision.h"
#include "GameLogic/SkirmishAILiveness.h"
#include "WW3D2/textureloader.h"

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

static void TestSkirmishAIReplayEpoch()
{
	UnicodeString unmarked = L"Aug 14 2026 21:00:00";
	CHECK(GetSkirmishAIReplayEpoch(unmarked) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(!ReplayVersionUsesSkirmishAILivenessRecovery(unmarked));
	CHECK(ShouldUseSkirmishAICurrentBehavior(false, SKIRMISH_AI_REPLAY_EPOCH_LEGACY));
	CHECK(!ShouldUseSkirmishAICurrentBehavior(true, SKIRMISH_AI_REPLAY_EPOCH_LEGACY));

	UnicodeString livenessOnly = unmarked;
	MarkReplayVersionForSkirmishAILivenessRecovery(livenessOnly);
	CHECK(livenessOnly.compare(L"Aug 14 2026 21:00:00 [SkirmishAILiveness=1]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(livenessOnly) == SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(livenessOnly));
	CHECK(!ShouldUseSkirmishAICurrentBehavior(true, SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS));

	UnicodeString currentEpoch = unmarked;
	MarkReplayVersionForSkirmishAICurrentEpoch(currentEpoch);
	CHECK(currentEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2]") == 0);
	CHECK(GetSkirmishAIReplayEpoch(currentEpoch) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	CHECK(ReplayVersionUsesSkirmishAILivenessRecovery(currentEpoch));
	CHECK(ShouldUseSkirmishAICurrentBehavior(true, SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	MarkReplayVersionForSkirmishAICurrentEpoch(currentEpoch);
	CHECK(currentEpoch.compare(L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2]") == 0);

	UnicodeString unrelatedSuffix = L"Aug 14 2026 21:00:00 [SkirmishAILiveness=2]";
	CHECK(GetSkirmishAIReplayEpoch(unrelatedSuffix) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	CHECK(!ReplayVersionUsesSkirmishAILivenessRecovery(unrelatedSuffix));
	UnicodeString futureEpoch = L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3]";
	CHECK(GetSkirmishAIReplayEpoch(futureEpoch) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString malformedEpoch = L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=x]";
	CHECK(GetSkirmishAIReplayEpoch(malformedEpoch) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString mixedMarkers = L"Aug 14 2026 21:00:00 [SkirmishAILiveness=1] [SkirmishAIEpoch=2]";
	CHECK(GetSkirmishAIReplayEpoch(mixedMarkers) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString duplicateMarkers = L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=2] [SkirmishAIEpoch=2]";
	CHECK(GetSkirmishAIReplayEpoch(duplicateMarkers) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString unknownThenCurrent = L"Aug 14 2026 21:00:00 [SkirmishAIEpoch=3] [SkirmishAIEpoch=2]";
	CHECK(GetSkirmishAIReplayEpoch(unknownThenCurrent) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString malformedThenLiveness = L"Aug 14 2026 21:00:00 [SkirmishAILiveness=x] [SkirmishAILiveness=1]";
	CHECK(GetSkirmishAIReplayEpoch(malformedThenLiveness) == SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	UnicodeString unknownWriterInput = futureEpoch;
	MarkReplayVersionForSkirmishAICurrentEpoch(unknownWriterInput);
	CHECK(unknownWriterInput.compare(futureEpoch) == 0);
	CHECK(!ShouldUseSkirmishAICurrentBehavior(true, 3));
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

	UnicodeString combined = unmarked;
	MarkReplayVersionForPathfindQueueCurrentEpoch(combined);
	MarkReplayVersionForSkirmishAICurrentEpoch(combined);
	CHECK(GetPathfindQueueReplayEpoch(combined) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT);
	CHECK(GetSkirmishAIReplayEpoch(combined) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);

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
	CHECK(ScoreSkirmishAITeam(input).factoryWaitScore == -250);
	CHECK(ScoreSkirmishAITeam(input).lossScore == -225);
	CHECK(ScoreSkirmishAITeam(input).pathFailureScore == -300);
	CHECK(ScoreSkirmishAITeam(input).rawContextScore == -1000);

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

static void TestSkirmishAITestRunnerContract()
{
	CommandLineData commandLineData;
	CHECK(!commandLineData.hasSkirmishAITestRequest());
	CHECK(commandLineData.getSkirmishAITestSeed() == 0);
	commandLineData.requestSkirmishAITest(1729);
	CHECK(commandLineData.hasSkirmishAITestRequest());
	CHECK(commandLineData.getSkirmishAITestSeed() == 1729);

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

int main(int argc, char **argv)
{
	initMemoryManager();
#if defined(_WIN64)
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
	TestSkirmishAICorrectnessPolicies();
	TestSkirmishAIProductionPolicies();
	TestSkirmishAITargetingPolicies();
	TestSkirmishAIFeedbackPolicies();
	TestSkirmishAITestRunnerContract();
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
