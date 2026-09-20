/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "PreRTS.h"

#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/SkirmishAILegacySaveTest.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace
{
enum
{
	LEGACY_SAVE_NAME_CAPACITY = 64,
	LEGACY_SAVE_SIMULATION_FRAMES = 120,
	LEGACY_SAVE_MAX_UPDATE_CALLS = 480
};

enum LegacySaveTestState
{
	LEGACY_SAVE_TEST_NOT_STARTED,
	LEGACY_SAVE_TEST_WAITING_FOR_LOAD,
	LEGACY_SAVE_TEST_SIMULATING,
	LEGACY_SAVE_TEST_FINISHED
};

Bool s_requested = FALSE;
char s_saveName[LEGACY_SAVE_NAME_CAPACITY] = { 0 };
LegacySaveTestState s_state = LEGACY_SAVE_TEST_NOT_STARTED;
Int s_result = 1;
UnsignedInt s_startFrame = 0;
UnsignedInt s_lastFrame = 0;
UnsignedInt s_framesAdvanced = 0;
UnsignedInt s_maxFrameDelta = 0;
UnsignedInt s_loadUpdateCalls = 0;
UnsignedInt s_updateCalls = 0;
GameInfo *s_loadedGameInfo = nullptr;
AsciiString s_loadedMapName;
AsciiString s_loadedPristineMapName;
Int s_loadedPlayerCount = 0;
Int s_loadedAIPlayers = 0;
Int s_admissionPlayerIndex = -1;
Bool s_identityEstablished = FALSE;
Bool s_identitySettleReported = FALSE;

Bool isSafeSaveBasename(const char *basename)
{
	if (basename == nullptr)
		return FALSE;

	const size_t length = strlen(basename);
	if (length < 5 || length >= LEGACY_SAVE_NAME_CAPACITY)
		return FALSE;
	if (basename[0] == '.' || basename[length - 4] != '.')
		return FALSE;
	if (!((basename[length - 3] == 's' || basename[length - 3] == 'S') &&
		(basename[length - 2] == 'a' || basename[length - 2] == 'A') &&
		(basename[length - 1] == 'v' || basename[length - 1] == 'V')))
		return FALSE;

	for (size_t i = 0; i < length; ++i)
	{
		const char c = basename[i];
		const Bool alphaNumeric =
			(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9');
		if (!alphaNumeric && c != '-' && c != '_' && c != '.')
			return FALSE;
	}
	return TRUE;
}

void emit(const char *marker)
{
	printf("LEGACY_SAVE_TEST_%s\n", marker);
	fflush(stdout);
}

void emitFailure(const char *reason)
{
	printf("LEGACY_SAVE_TEST_FAIL reason=%s\n", reason ? reason : "unknown");
	fflush(stdout);
}

void requestQuit()
{
	if (TheGameEngine != nullptr)
		TheGameEngine->setQuitting(TRUE);
	emit("QUIT_REQUESTED");
}

void finishFailure(const char *reason)
{
	emitFailure(reason);
	s_result = 1;
	s_state = LEGACY_SAVE_TEST_FINISHED;
	requestQuit();
}
}

void RequestSkirmishAILegacySaveTest(const char *basename)
{
	s_requested = TRUE;
	s_saveName[0] = '\0';
	s_state = LEGACY_SAVE_TEST_NOT_STARTED;
	s_result = 1;
	s_loadUpdateCalls = 0;
	if (!isSafeSaveBasename(basename))
	{
		emitFailure("unsafe_save_basename");
		s_state = LEGACY_SAVE_TEST_FINISHED;
		return;
	}

	memcpy(s_saveName, basename, strlen(basename) + 1);
	if (TheWritableGlobalData != nullptr)
	{
		// Match -loadsave before GameClient constructs its intro state.
		TheWritableGlobalData->m_loadSaveGame.set(s_saveName);
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;
	}
}

Bool IsSkirmishAILegacySaveTestRequested()
{
	return s_requested;
}

Bool IsSkirmishAILegacySaveTestActive()
{
	return s_requested && s_state != LEGACY_SAVE_TEST_FINISHED;
}

Bool StartSkirmishAILegacySaveTest()
{
	if (!s_requested)
		return TRUE;
	if (s_state == LEGACY_SAVE_TEST_FINISHED)
		return FALSE;

	emit("START");

	if (s_saveName[0] == '\0')
	{
		finishFailure("unsafe_save_basename");
		return FALSE;
	}
	if (TheGameState == nullptr || TheGameEngine == nullptr ||
		TheGameLogic == nullptr || ThePlayerList == nullptr ||
		TheWritableGlobalData == nullptr)
	{
		finishFailure("engine_not_ready");
		return FALSE;
	}

	AvailableGameInfo gameInfo;
	gameInfo.filename.set(s_saveName);
	gameInfo.next = nullptr;
	gameInfo.prev = nullptr;

	try
	{
		const AsciiString filepath =
			TheGameState->getFilePathInSaveDirectory(gameInfo.filename);
		TheGameState->getSaveGameInfoFromFile(
			filepath, &gameInfo.saveGameInfo);
	}
	catch (...)
	{
		finishFailure("header_read_failed");
		return FALSE;
	}

	if (gameInfo.saveGameInfo.saveFileType != SAVE_FILE_TYPE_NORMAL ||
		!gameInfo.saveGameInfo.campaignSide.isEmpty() ||
		gameInfo.saveGameInfo.missionNumber != -1)
	{
		finishFailure("campaign_or_mission_save");
		return FALSE;
	}
	emit("HEADER_OK");

	// GameClient will consume the startup queue after its intro state completes,
	// then hide the shell, prepare the game, and load through GameState.
	TheWritableGlobalData->m_loadSaveGame.set(s_saveName);
	s_state = LEGACY_SAVE_TEST_WAITING_FOR_LOAD;
	return TRUE;
}

void UpdateSkirmishAILegacySaveTest()
{
	if (!s_requested || s_state == LEGACY_SAVE_TEST_NOT_STARTED ||
		s_state == LEGACY_SAVE_TEST_FINISHED)
		return;

	if (s_state == LEGACY_SAVE_TEST_WAITING_FOR_LOAD)
	{
		if (TheGameEngine->getQuitting())
		{
			finishFailure("load_failed");
			return;
		}
		if (TheWritableGlobalData->m_loadSaveGame.isNotEmpty())
		{
			if (s_loadUpdateCalls >= LEGACY_SAVE_MAX_UPDATE_CALLS)
			{
				finishFailure("load_stalled");
				return;
			}
			++s_loadUpdateCalls;
			return;
		}
		if (TheGameLogic->getGameMode() != GAME_SKIRMISH ||
			!TheGameLogic->isInGame())
		{
			finishFailure("loaded_mode_not_skirmish");
			return;
		}
		emit("LOAD_OK");

		Player *cashPlayer = nullptr;
		Int skirmishAIPlayers = 0;
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		{
			Player *player = ThePlayerList->getNthPlayer(i);
			if (!player || player->getPlayerType() != PLAYER_COMPUTER)
				continue;
			if (!player->isSkirmishAIPlayer())
				continue;
			++skirmishAIPlayers;
			if (!cashPlayer && player->getMoney()->countMoney() > 0)
				cashPlayer = player;
		}
		if (skirmishAIPlayers == 0)
		{
			finishFailure("computer_skirmish_ai_missing");
			return;
		}
		printf("LEGACY_SAVE_TEST_AI_OK players=%d\n", skirmishAIPlayers);
		fflush(stdout);
		if (cashPlayer == nullptr)
		{
			finishFailure("no_positive_cash_ai");
			return;
		}

		const UnsignedInt cash = cashPlayer->getMoney()->countMoney();
		if (cash > static_cast<UnsignedInt>(INT_MAX) ||
			!cashPlayer->canSpendForSkirmishAIRecovery(
				static_cast<Int>(cash), nullptr, FALSE))
		{
			finishFailure("recovery_reserve_admission_failed");
			return;
		}
		printf("LEGACY_SAVE_TEST_ADMISSION_OK player=%d cash=%u\n",
			cashPlayer->getPlayerIndex(), cash);
		fflush(stdout);

		s_startFrame = TheGameLogic->getFrame();
		s_lastFrame = s_startFrame;
		s_framesAdvanced = 0;
		s_maxFrameDelta = 0;
		s_updateCalls = 1;
		s_loadedGameInfo = TheGameInfo;
		s_loadedMapName = TheGlobalData->m_mapName;
		s_loadedPristineMapName = TheGameState->getPristineMapName();
		s_loadedPlayerCount = ThePlayerList->getPlayerCount();
		s_loadedAIPlayers = skirmishAIPlayers;
		s_admissionPlayerIndex = cashPlayer->getPlayerIndex();
		s_identityEstablished = FALSE;
		s_identitySettleReported = FALSE;
		s_state = LEGACY_SAVE_TEST_SIMULATING;
		return;
	}

	const UnsignedInt endFrame = TheGameLogic->getFrame();
	if (TheGameLogic->getGameMode() != GAME_SKIRMISH ||
		!TheGameLogic->isInGame())
	{
		printf("LEGACY_SAVE_TEST_IDENTITY_REJECT reason=mode frame=%u updates=%u\n",
			endFrame, s_updateCalls);
		fflush(stdout);
		finishFailure("simulation_game_replaced");
		return;
	}

	Int currentAIPlayers = 0;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player && player->getPlayerType() == PLAYER_COMPUTER &&
			player->isSkirmishAIPlayer())
		{
			++currentAIPlayers;
		}
	}
	Player *admissionPlayer = ThePlayerList->getNthPlayer(s_admissionPlayerIndex);
	if (ThePlayerList->getPlayerCount() != s_loadedPlayerCount ||
		currentAIPlayers != s_loadedAIPlayers || admissionPlayer == nullptr ||
		admissionPlayer->getPlayerType() != PLAYER_COMPUTER ||
		!admissionPlayer->isSkirmishAIPlayer())
	{
		printf("LEGACY_SAVE_TEST_IDENTITY_REJECT reason=players frame=%u updates=%u players=%d ai_players=%d\n",
			endFrame, s_updateCalls, ThePlayerList->getPlayerCount(),
			currentAIPlayers);
		fflush(stdout);
		finishFailure("simulation_game_replaced");
		return;
	}

	const Bool gameInfoChanged = TheGameInfo != s_loadedGameInfo;
	const Bool activeMapChanged = TheGlobalData->m_mapName != s_loadedMapName;
	const Bool pristineMapChanged =
		TheGameState->getPristineMapName() != s_loadedPristineMapName;
	if (s_identityEstablished &&
		(gameInfoChanged || activeMapChanged || pristineMapChanged))
	{
		printf("LEGACY_SAVE_TEST_IDENTITY_REJECT reason=settled_identity frame=%u updates=%u game_info_changed=%d active_map_changed=%d pristine_map_changed=%d\n",
			endFrame, s_updateCalls, gameInfoChanged, activeMapChanged,
			pristineMapChanged);
		fflush(stdout);
		finishFailure("simulation_game_replaced");
		return;
	}
	if (!s_identityEstablished && !s_identitySettleReported &&
		(gameInfoChanged || activeMapChanged || pristineMapChanged))
	{
		printf("LEGACY_SAVE_TEST_IDENTITY_SETTLE frame=%u updates=%u game_info_changed=%d active_map_changed=%d pristine_map_changed=%d\n",
			endFrame, s_updateCalls, gameInfoChanged, activeMapChanged,
			pristineMapChanged);
		fflush(stdout);
		s_identitySettleReported = TRUE;
	}
	if (endFrame < s_lastFrame)
	{
		printf("LEGACY_SAVE_TEST_FRAME_REJECT last_frame=%u current_frame=%u updates=%u\n",
			s_lastFrame, endFrame, s_updateCalls);
		fflush(stdout);
		finishFailure("simulation_frame_reversed");
		return;
	}

	const UnsignedInt frameDelta = endFrame - s_lastFrame;
	if (frameDelta > 0)
	{
		if (!s_identityEstablished)
		{
			s_loadedGameInfo = TheGameInfo;
			s_loadedMapName = TheGlobalData->m_mapName;
			s_loadedPristineMapName = TheGameState->getPristineMapName();
			s_identityEstablished = TRUE;
		}
		s_lastFrame = endFrame;
		s_framesAdvanced += frameDelta;
		if (frameDelta > s_maxFrameDelta)
			s_maxFrameDelta = frameDelta;
	}
	if (s_framesAdvanced >= LEGACY_SAVE_SIMULATION_FRAMES)
	{
		printf("LEGACY_SAVE_TEST_SIMULATION_OK start_frame=%u end_frame=%u frames=%u updates=%u max_delta=%u\n",
			s_startFrame, endFrame, s_framesAdvanced, s_updateCalls,
			s_maxFrameDelta);
		fflush(stdout);
		s_result = 0;
		s_state = LEGACY_SAVE_TEST_FINISHED;
		emit("PASS");
		requestQuit();
		return;
	}
	if (TheGameEngine->getQuitting())
	{
		finishFailure("simulation_failed");
		return;
	}
	if (s_updateCalls >= LEGACY_SAVE_MAX_UPDATE_CALLS)
	{
		finishFailure("simulation_stalled");
		return;
	}
	++s_updateCalls;
}

Int FinalizeSkirmishAILegacySaveTest(Int engineExitCode)
{
	if (!s_requested)
		return engineExitCode;
	if (s_state != LEGACY_SAVE_TEST_FINISHED)
		finishFailure("engine_exited_before_completion");

	const Int result = s_result == 0 ? engineExitCode : s_result;
	s_requested = FALSE;
	s_saveName[0] = '\0';
	s_state = LEGACY_SAVE_TEST_NOT_STARTED;
	return result;
}
