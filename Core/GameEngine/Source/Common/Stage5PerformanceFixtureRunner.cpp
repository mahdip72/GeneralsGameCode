/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "PreRTS.h"

#if defined(_WIN64)
#include "Common/Stage5PerformanceFixtureRunner.h"
#include "Common/FileSystem.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "Common/SkirmishAITestRunner.h"
#include "Common/Team.h"
#include "Common/crc.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "Lib/PerformanceReceipt.h"

#include <limits.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {
rts::fixture::Request s_request;
struct RunnerState
{
	bool started, ending, finished, failed, loaded, naturalVictory, finalCrcKnown;
	const char *failureReason;
	unsigned startupMilliseconds, progressMilliseconds, shutdownMilliseconds, lastFrame;
	unsigned mapCrc, mapSize, endFrame, finalCrc;
	int winnerTeam;
	rts::fixture::Plan plan;
	rts::performance::PerformanceReceiptWorkload workload;
	unsigned initialPlayerUnits[8], peakPlayerUnits[8];
	unsigned initialUnrosteredUnits, peakUnrosteredUnits;
	char mapSha256[65], executableSha256[65], replayFileName[1024], nonce[65];
};
RunnerState s_runner = {};

rts::fixture::Title FixtureTitle()
{
#if defined(RTS_GENERALS)
	return rts::fixture::GeneralsTitle;
#elif defined(RTS_ZEROHOUR)
	return rts::fixture::ZeroHourTitle;
#else
	return rts::fixture::UnknownTitle;
#endif
}

const char *FixtureTitleName()
{
	return FixtureTitle() == rts::fixture::GeneralsTitle ? "Generals" :
		(FixtureTitle() == rts::fixture::ZeroHourTitle ? "ZeroHour" : "Unknown");
}

struct CloseFixtureFile
{
	void operator()(File *file) const { if (file) file->close(); }
};

void FailFixture(const char *reason)
{
	if (!s_runner.failed) s_runner.failureReason = reason;
	s_runner.failed = true;
}

// Failure shutdown may discard a partial recording, but can never produce a
// completion receipt. Successful shutdown only follows observed natural victory.
void RequestStop()
{
	if (TheGameLogic && TheGameLogic->isInGame())
	{
		if (!s_runner.ending)
		{
			TheGameLogic->exitGame();
			s_runner.ending = true;
			s_runner.shutdownMilliseconds = GetTickCount();
		}
	}
	else if (TheGameEngine) TheGameEngine->setQuitting(TRUE);
}

bool HashVirtualMap(unsigned *crc, unsigned *size, char sha256[65])
{
	if (!TheFileSystem) return false;
	std::unique_ptr<File, CloseFixtureFile> file(TheFileSystem->openFile(
		s_request.mapKey, File::READ | File::BINARY | File::STREAMING));
	if (!file) return false;
	const Int length = file->size();
	// Fixture maps are deliberately bounded; never allocate an untrusted map size.
	if (length <= 0 || length > 64 * 1024 * 1024) return false;
	try
	{
		std::vector<unsigned char> bytes(static_cast<size_t>(length));
		Int offset = 0;
		while (offset < length)
		{
			const Int count = file->read(&bytes[static_cast<size_t>(offset)], length - offset);
			if (count <= 0 || count > length - offset) break;
			offset += count;
		}
		unsigned char extra = 0;
		const bool exact = offset == length && file->read(&extra, 1) == 0;
		if (!exact) return false;
		CRC computed;
		computed.computeCRC(&bytes[0], length);
		*crc = computed.get();
		*size = static_cast<unsigned>(length);
		return HashSkirmishAITestBytes(&bytes[0], bytes.size(), sha256) != FALSE;
	}
	catch (const std::bad_alloc &)
	{
		// The virtual file is closed by its owner on every return and unwind.
		return false;
	}
}

unsigned CountRosterPlayers()
{
	unsigned count = 0;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player && rts::performance::IsPerformanceReceiptRosterPlayer(
			player->isPlayableSide() != FALSE, player->isPlayerObserver() != FALSE)) ++count;
	}
	return count;
}

bool VerifyRoster()
{
	rts::fixture::ObservedSlot observed[8] = {};
	AsciiString factionNames[8];
	for (int i = 0; i < 8; ++i)
	{
		const GameSlot *slot = TheGameInfo->getConstSlot(i);
		Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
		if (!slot || !player || !player->getPlayerTemplate()) return false;
		Team *owner = TheTeamFactory->findTeam(s_runner.plan.slots[i].mapOwner);
		rts::fixture::ObservedSlot &actual = observed[i];
		actual.human = slot->isHuman() && player->getPlayerType() == PLAYER_HUMAN;
		actual.brutalAi = slot->getState() == SLOT_BRUTAL_AI && player->getPlayerType() == PLAYER_COMPUTER;
		actual.observer = player->isPlayerObserver() != FALSE;
		actual.playableSide = player->isPlayableSide() != FALSE;
		actual.ownerResolvesToPlayer = owner && owner->getControllingPlayer() == player;
		actual.factionIndex = slot->getPlayerTemplate();
		actual.originalFactionIndex = slot->getOriginalPlayerTemplate();
		actual.color = slot->getColor();
		actual.originalColor = slot->getOriginalColor();
		actual.startPosition = slot->getStartPos();
		actual.originalStartPosition = slot->getOriginalStartPos();
		actual.team = slot->getTeamNumber();
		factionNames[i] = player->getPlayerTemplate()->getName();
		actual.factionName = factionNames[i].str();
	}
	if (!rts::fixture::IsExpectedRoster(s_runner.plan, observed, CountRosterPlayers())) return false;
	for (int i = 0; i < 8; ++i)
	{
		printf("STAGE5_PERFORMANCE_FIXTURE_ROSTER slot=%d controller=%s faction=%s faction_index=%d "
			"start=%d color=%d team=%d observer=0 map_owner=%s owner_verified=1\n", i,
			observed[i].human ? "human" : "brutal-ai", observed[i].factionName,
			observed[i].factionIndex, observed[i].startPosition, observed[i].color,
			observed[i].team, s_runner.plan.slots[i].mapOwner);
	}
	fflush(stdout);
	return true;
}

bool VerifyLoadedMap()
{
	const AsciiString gameMap = TheGameInfo->getMap();
	const AsciiString globalMap = TheGlobalData->m_mapName;
	const AsciiString terrainMap = TheTerrainLogic->getSourceFilename();
	SkirmishAITestPlan expected = {};
	expected.mapName = s_request.mapKey;
	expected.seed = s_request.seed;
	SkirmishAITestLoadedState actual = { gameMap.str(), globalMap.str(), terrainMap.str(),
		TheGameInfo->getMapCRC(), TheGameInfo->getMapSize(), TheGameInfo->getSeed() };
	unsigned crc = 0, size = 0;
	char sha256[65];
	return IsExpectedSkirmishAITestLoadedState(expected, s_runner.mapCrc, s_runner.mapSize, &actual) &&
		HashVirtualMap(&crc, &size, sha256) && crc == s_runner.mapCrc && size == s_runner.mapSize &&
		strcmp(sha256, s_runner.mapSha256) == 0;
}

bool ObserveCompletedFrame(unsigned frame)
{
	if (frame == 0 || (s_runner.workload.sampleCount != 0 && frame <= s_runner.workload.lastFrame)) return true;
	unsigned units = 0, playerUnits[8] = {}, unrosteredUnits = 0;
	Player *players[8];
	for (int i = 0; i < 8; ++i) players[i] = ThePlayerList->getPlayerFromSlotIndex(i);
	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
	{
		if (!rts::performance::IsPerformanceReceiptLiveUnit(object->isKindOf(KINDOF_INFANTRY) != FALSE,
			object->isKindOf(KINDOF_VEHICLE) != FALSE, object->isEffectivelyDead() != FALSE,
			object->isDestroyed() != FALSE)) continue;
		++units;
		int slot = 0;
		for (; slot < 8; ++slot)
			if (players[slot] == object->getControllingPlayer()) { ++playerUnits[slot]; break; }
		if (slot == 8) ++unrosteredUnits;
	}
	const bool first = s_runner.workload.sampleCount == 0;
	if (!rts::performance::ObservePerformanceReceiptWorkload(s_runner.workload, frame, CountRosterPlayers(), units)) return false;
	for (int i = 0; i < 8; ++i)
	{
		if (first) s_runner.initialPlayerUnits[i] = playerUnits[i];
		if (playerUnits[i] > s_runner.peakPlayerUnits[i]) s_runner.peakPlayerUnits[i] = playerUnits[i];
	}
	if (first) s_runner.initialUnrosteredUnits = unrosteredUnits;
	if (unrosteredUnits > s_runner.peakUnrosteredUnits) s_runner.peakUnrosteredUnits = unrosteredUnits;
	if (first)
	{
		printf("STAGE5_PERFORMANCE_FIXTURE_OBSERVED first_frame=%u actual_players=%u initial_units=%u "
			"initial_unrostered_units=%u\n", frame, s_runner.workload.playerCount, units, unrosteredUnits);
		fflush(stdout);
	}
	return s_runner.workload.rosterStable && s_runner.workload.contiguous && s_runner.workload.playerCount == 8;
}

bool VerifyReplaySetup(const RecorderClass::ReplayHeader &header)
{
	SkirmishGameInfo recorded;
	recorded.init();
	recorded.clearSlotList();
	recorded.reset();
	recorded.enterGame();
	if (!ParseAsciiStringToGameInfo(&recorded, header.gameOptions) || header.localPlayerIndex != 0 ||
		!rts::fixture::SameToken(recorded.getMap().str(), s_request.mapKey) ||
		recorded.getSeed() != s_request.seed || recorded.getMapCRC() != s_runner.mapCrc ||
		recorded.getMapSize() != s_runner.mapSize) return false;
	for (int i = 0; i < 8; ++i)
	{
		const GameSlot *slot = recorded.getConstSlot(i);
		const rts::fixture::SlotPlan &expected = s_runner.plan.slots[i];
		if (!slot || slot->getState() != (expected.human ? SLOT_PLAYER : SLOT_BRUTAL_AI) ||
			slot->getPlayerTemplate() != expected.factionIndex || slot->getColor() != expected.color ||
			slot->getStartPos() != expected.startPosition || slot->getTeamNumber() != expected.team) return false;
	}
	return true;
}
}

Bool ConfigureStage5PerformanceFixture(const rts::fixture::Request &request)
{
	if (s_request.requested || !request.requested) return FALSE;
	s_request = request;
	return TRUE;
}

Bool IsStage5PerformanceFixtureRequested() { return s_request.requested ? TRUE : FALSE; }

Bool StartStage5PerformanceFixtureRunner()
{
	if (!s_request.requested) return TRUE;
	s_runner.startupMilliseconds = s_runner.progressMilliseconds = GetTickCount();
	s_runner.lastFrame = UINT_MAX;
	s_runner.winnerTeam = -1;
	_snprintf(s_runner.nonce, sizeof(s_runner.nonce), "%08X-%08X-%08X",
		GetCurrentProcessId(), GetTickCount(), static_cast<unsigned>(s_request.seed));
	s_runner.nonce[sizeof(s_runner.nonce) - 1] = '\0';
	if (!TheGlobalData->m_headless || !TheGlobalData->m_simulateReplays.empty() ||
		IsSkirmishAITestRunnerArmed()) { FailFixture("conflicting_runtime_mode"); return FALSE; }
	if (!TheMapCache || !TheMessageStream || !TheRecorder || !TheWritableGlobalData ||
		!ThePlayerTemplateStore || !TheFileSystem) { FailFixture("engine_not_ready"); return FALSE; }
	if (!CaptureSkirmishAITestValidatedExecutableHash(s_runner.executableSha256))
		{ FailFixture("executable_hash_unavailable_or_mismatch"); return FALSE; }
	int americaIndex = -1;
	for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate *candidate = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (candidate && candidate->getName() == AsciiString("FactionAmerica")) { americaIndex = i; break; }
	}
	if (!rts::fixture::BuildPlan(americaIndex, &s_runner.plan)) { FailFixture("america_template_unavailable"); return FALSE; }
	const MapMetaData *map = TheMapCache->findMap(s_request.mapKey);
	if (!map || !map->m_doesExist || !map->m_isMultiplayer || map->m_numPlayers != 8)
		{ FailFixture("eight_start_map_unavailable"); return FALSE; }
	if (!HashVirtualMap(&s_runner.mapCrc, &s_runner.mapSize, s_runner.mapSha256) ||
		s_runner.mapCrc != map->m_CRC || s_runner.mapSize != map->m_filesize)
		{ FailFixture("map_content_metadata_mismatch"); return FALSE; }
	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = NEW SkirmishGameInfo;
	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->setLocalIP(0);
	TheSkirmishGameInfo->enterGame();
	for (int i = 0; i < 8; ++i)
	{
		const rts::fixture::SlotPlan &expected = s_runner.plan.slots[i];
		GameSlot *slot = TheSkirmishGameInfo->getSlot(i);
		UnicodeString name;
		if (expected.human) name.set(L"Performance Fixture Controller");
		slot->setState(expected.human ? SLOT_PLAYER : SLOT_BRUTAL_AI, name, 0);
		slot->setPlayerTemplate(expected.factionIndex);
		slot->setColor(expected.color);
		slot->setStartPos(expected.startPosition);
		slot->setTeamNumber(expected.team);
		if (expected.human) { slot->setAccept(); slot->setMapAvailability(TRUE); }
	}
	TheSkirmishGameInfo->setMap(s_request.mapKey);
	TheSkirmishGameInfo->setMapCRC(s_runner.mapCrc);
	TheSkirmishGameInfo->setMapSize(s_runner.mapSize);
	TheSkirmishGameInfo->setSeed(s_request.seed);
	TheSkirmishGameInfo->startGame(0);
	TheWritableGlobalData->m_mapName = s_request.mapKey;
	TheWritableGlobalData->m_useFpsLimit = FALSE;
	TheWritableGlobalData->m_shellMapOn = FALSE;
	TheRecorder->setArchiveEnabled(FALSE);
	InitRandom(static_cast<UnsignedInt>(s_request.seed));
	GameMessage *message = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	message->appendIntegerArgument(GAME_SKIRMISH);
	message->appendIntegerArgument(DIFFICULTY_NORMAL);
	message->appendIntegerArgument(0);
	s_runner.started = true;
	printf("STAGE5_PERFORMANCE_FIXTURE_START category=native-performance-fixture title=%s map=\"%s\" seed=%d "
		"frame_budget=%u map_crc=%08X map_size=%u map_sha256=%s executable_sha256=%s run_nonce=%s\n",
		FixtureTitleName(), s_request.mapKey, s_request.seed, s_request.frameBudget, s_runner.mapCrc, s_runner.mapSize,
		s_runner.mapSha256, s_runner.executableSha256, s_runner.nonce);
	fflush(stdout);
	return TRUE;
}

void UpdateStage5PerformanceFixtureRunner()
{
	if (!s_request.requested || !s_runner.started || s_runner.finished) return;
	TheWritableGlobalData->m_useFpsLimit = FALSE;
	const unsigned now = GetTickCount();
	if (!TheGameLogic) { FailFixture("runtime_unavailable"); RequestStop(); return; }
	if (s_runner.ending)
	{
		if (!TheGameLogic->isInGame())
		{
			s_runner.finished = true;
			TheGameEngine->setQuitting(TRUE);
		}
		else if (now - s_runner.shutdownMilliseconds >= SKIRMISH_AI_TEST_MAX_SHUTDOWN_MILLISECONDS)
		{
			FailFixture("shutdown_timeout");
			if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD) TheRecorder->stopRecording();
			TheGameLogic->clearGameData(FALSE);
			TheGameEngine->setQuitting(TRUE);
		}
		return;
	}
	if (!TheGameLogic->isInGame() || TheGameLogic->isLoadingMap() || !TheGameInfo)
	{
		if (s_runner.loaded) { FailFixture("game_ended_without_natural_victory"); RequestStop(); }
		else if (now - s_runner.startupMilliseconds >= SKIRMISH_AI_TEST_MAX_STARTUP_MILLISECONDS)
			{ FailFixture("startup_timeout"); RequestStop(); }
		return;
	}
	if (!TheVictoryConditions || !ThePlayerList || !TheRecorder || !TheTerrainLogic || !TheTeamFactory)
		{ FailFixture("runtime_unavailable"); RequestStop(); return; }
	if (!s_runner.loaded)
	{
		if (!VerifyLoadedMap()) { FailFixture("loaded_map_mismatch"); RequestStop(); return; }
		if (!VerifyRoster()) { FailFixture("observed_roster_mismatch"); RequestStop(); return; }
		s_runner.loaded = true;
	}
	TheRecorder->setArchiveEnabled(FALSE);
	if (TheRecorder->getMode() != RECORDERMODETYPE_RECORD || !TheRecorder->hasOpenRecordingFile())
		{ FailFixture("recording_unavailable"); RequestStop(); return; }
	if (!s_runner.replayFileName[0])
		strlcpy(s_runner.replayFileName, TheRecorder->getRecordingFileName().str(), sizeof(s_runner.replayFileName));
	const unsigned frame = TheGameLogic->getFrame();
	if (!ObserveCompletedFrame(frame)) { FailFixture("observed_workload_invalid"); RequestStop(); return; }
	const unsigned endFrame = TheVictoryConditions->getEndFrame();
	const rts::fixture::Progress progress = rts::fixture::EvaluateProgress(endFrame, frame, s_request.frameBudget);
	if (progress == rts::fixture::TimedOut) { FailFixture("frame_budget"); RequestStop(); return; }
	if (progress == rts::fixture::Running)
	{
		if (frame != s_runner.lastFrame) { s_runner.lastFrame = frame; s_runner.progressMilliseconds = now; }
		else if (now - s_runner.progressMilliseconds >= SKIRMISH_AI_TEST_MAX_STALLED_MILLISECONDS)
			{ FailFixture("frame_stalled"); RequestStop(); }
		return;
	}
	int winner = -1;
	for (int i = 0; i < 8; ++i)
	{
		Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
		if (player && TheVictoryConditions->hasAchievedVictory(player))
		{
			const int team = TheGameInfo->getConstSlot(i)->getTeamNumber();
			if (winner != -1 && team != winner) { FailFixture("conflicting_winners"); RequestStop(); return; }
			winner = team;
		}
	}
	if (winner != 0 && winner != 1) { FailFixture("winner_unavailable"); RequestStop(); return; }
	s_runner.winnerTeam = winner;
	s_runner.endFrame = endFrame;
	s_runner.naturalVictory = true;
	s_runner.finalCrc = TheGameLogic->getCRC(CRC_RECALC);
	s_runner.finalCrcKnown = true;
	RequestStop();
}

Int FinalizeStage5PerformanceFixtureRunner(Int engineExitCode)
{
	if (!s_request.requested) return engineExitCode;
	if (engineExitCode != 0) FailFixture("engine_exit");
	if (!s_runner.finished || !s_runner.naturalVictory) FailFixture("incomplete");
	RecorderClass::ReplayHeader header;
	if (!s_runner.failed)
	{
		// Never close a recording here to manufacture a valid terminal header.
		if (!TheRecorder || TheRecorder->getMode() == RECORDERMODETYPE_RECORD ||
			TheRecorder->hasOpenRecordingFile()) FailFixture("recorder_not_naturally_closed");
		else
		{
			header.filename = s_runner.replayFileName;
			header.forPlayback = FALSE;
			if (!TheRecorder->readReplayHeader(header) || !RecorderClass::replayMatchesGameVersion(header) ||
				!VerifyReplaySetup(header)) FailFixture("replay_content_or_setup_invalid");
		}
	}
	if (!s_runner.failed)
	{
		rts::fixture::Completion completion = {};
		completion.title = FixtureTitle();
		completion.frameBudget = s_request.frameBudget;
		completion.endFrame = s_runner.endFrame;
		completion.replayFrameCount = header.frameCount;
		completion.winnerTeam = s_runner.winnerTeam;
		completion.replayEpoch = GetSkirmishAIReplayEpoch(header.versionTimeString);
		// The title-specific parser accepts only that title's exact marker family.
		completion.replayTitle = completion.replayEpoch != SKIRMISH_AI_REPLAY_EPOCH_LEGACY ?
			FixtureTitle() : rts::fixture::UnknownTitle;
		completion.naturalVictory = s_runner.naturalVictory;
		completion.recorderClosed = true;
		completion.desync = header.desyncGame != FALSE;
		completion.quitEarly = header.quitEarly != FALSE;
		completion.finalCrcKnown = s_runner.finalCrcKnown;
		completion.loadedMapVerified = completion.rosterVerified = s_runner.loaded;
		completion.observedPlayers = s_runner.workload.playerCount;
		completion.observedFrameSamples = static_cast<unsigned>(s_runner.workload.sampleCount);
		completion.startTime = header.startTime;
		completion.endTime = header.endTime;
		if (!rts::fixture::IsValidCompletion(completion)) FailFixture("natural_replay_closure_invalid");
	}
	char retained[1024] = {}, replaySha256[65] = {};
	if (!s_runner.failed)
	{
		AsciiString source = RecorderClass::getReplayDir();
		source.concat(s_runner.replayFileName);
		const AsciiString directory = RecorderClass::getReplayDir();
		const size_t length = directory.getLength();
		const char *separator = length && (directory.str()[length - 1] == '\\' || directory.str()[length - 1] == '/') ? "" : "\\";
		const int count = _snprintf(retained, sizeof(retained), "%s%sStage5Performance-%d-%s.rep",
			directory.str(), separator, s_request.seed, s_runner.nonce);
		retained[sizeof(retained) - 1] = '\0';
		if (count < 0 || !RetainSkirmishAITestReplayAtomically(source.str(), retained, replaySha256))
			FailFixture("replay_retention_failed");
	}
	if (s_runner.failed)
	{
		printf("STAGE5_PERFORMANCE_FIXTURE_FAIL category=native-performance-fixture title=%s seed=%d frame_budget=%u "
			"run_nonce=%s reason=%s\n", FixtureTitleName(), s_request.seed, s_request.frameBudget, s_runner.nonce,
			s_runner.failureReason ? s_runner.failureReason : "unknown");
		fflush(stdout);
		return 1;
	}
	for (int i = 0; i < 8; ++i)
		printf("STAGE5_PERFORMANCE_FIXTURE_PLAYER_UNITS slot=%d initial_units=%u peak_units=%u\n",
			i, s_runner.initialPlayerUnits[i], s_runner.peakPlayerUnits[i]);
	printf("STAGE5_PERFORMANCE_FIXTURE_COMPLETE category=native-performance-fixture title=%s map=\"%s\" seed=%d "
		"frame_budget=%u map_crc=%08X map_size=%u map_sha256=%s actual_players=%u initial_units=%u peak_units=%u "
		"initial_unrostered_units=%u peak_unrostered_units=%u observed_first_frame=%u observed_last_frame=%u "
		"observed_frame_samples=%llu winner_team=%d end_frame=%u final_crc=%08X replay_epoch=%d ai_epoch_marker=\"%ls\" "
		"replay_frame_count=%u executable_sha256=%s run_nonce=%s replay_sha256=%s retained_replay=\"%s\"\n",
		FixtureTitleName(), s_request.mapKey, s_request.seed, s_request.frameBudget, s_runner.mapCrc, s_runner.mapSize,
		s_runner.mapSha256, s_runner.workload.playerCount, s_runner.workload.initialUnitCount, s_runner.workload.peakUnitCount,
		s_runner.initialUnrosteredUnits, s_runner.peakUnrosteredUnits, s_runner.workload.firstFrame, s_runner.workload.lastFrame,
		static_cast<unsigned long long>(s_runner.workload.sampleCount), s_runner.winnerTeam, s_runner.endFrame, s_runner.finalCrc,
		GetSkirmishAIReplayEpoch(header.versionTimeString), GetSkirmishAICurrentReplayMarker(),
		header.frameCount, s_runner.executableSha256, s_runner.nonce, replaySha256, retained);
	fflush(stdout);
	return 0;
}
#endif
