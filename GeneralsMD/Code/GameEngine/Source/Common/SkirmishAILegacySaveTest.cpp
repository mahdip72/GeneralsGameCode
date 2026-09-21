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
#include "Common/BuildAssistant.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/ScoreKeeper.h"
#include "Common/SkirmishAILegacySaveTest.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameNetwork/GameInfo.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace
{
enum
{
	LEGACY_SAVE_NAME_CAPACITY = 64,
	LEGACY_SAVE_SETTLE_FRAMES = 120,
	LEGACY_SAVE_RECOVERY_TIMEOUT_FRAMES = 18000,
	LEGACY_SAVE_MAX_UPDATE_CALLS = 24000,
	LEGACY_SAVE_MAX_CONSTRUCTION_SCAFFOLDS = 24
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
Bool s_recoveryFaultApplied = FALSE;
Bool s_sawConstructionProgress = FALSE;
Bool s_sawOrdinaryBuilderProvenance = FALSE;
Int s_recoveryPlayerIndex = -1;
ObjectID s_destroyedCenterID = INVALID_ID;
ObjectID s_recoveryConstructionID = INVALID_ID;
UnsignedInt s_recoveryFaultFrame = 0;
Int s_recoveryCenterCost = 0;
Int s_recoveryBaselineMoneySpent = 0;
Int s_recoveryConstructionScaffolds = 0;
Real s_recoveryConstructionPercent = 0.0f;
const ThingTemplate *s_recoveryCenterTemplate = nullptr;

Bool isLiveObject(const Object *object)
{
	return object != nullptr && !object->isDestroyed() &&
		!object->isEffectivelyDead();
}

BuildListInfo *findBuildInfo(
	Player *player, const ThingTemplate *thing, ObjectID objectID)
{
	if (!player || !thing || !TheThingFactory)
		return nullptr;
	for (BuildListInfo *info = player->getBuildList(); info; info = info->getNext())
	{
		const ThingTemplate *plan = TheThingFactory->findTemplate(info->getTemplateName());
		if (plan && plan->isEquivalentTo(thing) &&
			info->getObjectID() == objectID)
			return info;
	}
	return nullptr;
}

Int countMatchingCommandCenters(
	Player *player, const ThingTemplate *thing, Object **first,
	Bool *underConstruction)
{
	if (first)
		*first = nullptr;
	if (underConstruction)
		*underConstruction = FALSE;
	if (!player || !thing || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!isLiveObject(object) || object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_COMMANDCENTER) ||
			object->testStatus(OBJECT_STATUS_SOLD) || !object->getTemplate() ||
			!object->getTemplate()->isEquivalentTo(thing))
			continue;
		++count;
		if (underConstruction &&
			object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			*underConstruction = TRUE;
		if (first && (*first == nullptr || object->getID() < (*first)->getID()))
			*first = object;
	}
	return count;
}

Int countCompatibleBuilders(
	Player *player, const ThingTemplate *primaryTemplate)
{
	if (!player || !primaryTemplate || !TheGameLogic || !TheBuildAssistant)
		return 0;
	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsSkirmishAILegacySaveBuilderLive(
				object != nullptr, object->isDestroyed(),
				object->isEffectivelyDead(),
				object->testStatus(OBJECT_STATUS_SOLD)) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) || !object->getAIUpdateInterface() ||
			!object->getAIUpdateInterface()->getDozerAIInterface() ||
			object->isDisabledByType(DISABLED_UNMANNED))
			continue;
		if (!TheBuildAssistant->isPossibleToMakeUnit(object, primaryTemplate))
			continue;
		++count;
	}
	return count;
}

Bool hasOtherStructure(Player *player, Object *excluded)
{
	if (!player || !TheGameLogic)
		return FALSE;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (object != excluded && isLiveObject(object) && object->isStructure() &&
			object->getControllingPlayer() == player &&
			!object->testStatus(OBJECT_STATUS_SOLD))
			return TRUE;
	}
	return FALSE;
}

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

Bool startRecoveryProbeFor(
	Player *player, Object *center, BuildListInfo *info,
	Int builderCount, UnsignedInt frame)
{
	if (!player || !center || !info || !center->getTemplate() ||
		!player->getScoreKeeper() || builderCount <= 0 ||
		s_admissionPlayerIndex != player->getPlayerIndex())
		return FALSE;

	s_recoveryPlayerIndex = s_admissionPlayerIndex;
	s_destroyedCenterID = center->getID();
	s_recoveryCenterTemplate = center->getTemplate();
	s_recoveryCenterCost = s_recoveryCenterTemplate->calcCostToBuild(player);
	s_recoveryBaselineMoneySpent = player->getScoreKeeper()->getTotalMoneySpent();
	s_recoveryFaultFrame = frame;
	info->setNumRebuilds(0);
	TheGameLogic->destroyObject(center);
	s_recoveryFaultApplied = TRUE;
	printf("LEGACY_SAVE_TEST_RECOVERY_FAULT frame=%u player=%d center=%u "
		"template=%s center_cost=%d cash=%u rebuilds=%d "
		"expected=recovery route=surviving_builder builders=%d\n",
		frame, s_recoveryPlayerIndex, s_destroyedCenterID,
		s_recoveryCenterTemplate->getName().str(), s_recoveryCenterCost,
		player->getMoney()->countMoney(), info->getNumRebuilds(), builderCount);
	fflush(stdout);
	return TRUE;
}

struct LegacySaveRecoveryCandidate
{
	LegacySaveRecoveryCandidate() :
		player(nullptr),
		center(nullptr),
		info(nullptr),
		builderCount(0),
		cash(0)
	{
	}

	Player *player;
	Object *center;
	BuildListInfo *info;
	Int builderCount;
	UnsignedInt cash;
};

Bool findRecoveryProbeCandidate(LegacySaveRecoveryCandidate *selected)
{
	if (!selected)
		return FALSE;

	Bool hasSelection = FALSE;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (!player)
			continue;

		SkirmishAILegacySaveCandidateInput input;
		input.isComputer = player->getPlayerType() == PLAYER_COMPUTER;
		input.isSkirmishAI = player->isSkirmishAIPlayer();
		input.hasPlayerTemplate = player->getPlayerTemplate() != nullptr;
		if (!input.isComputer || !input.isSkirmishAI ||
			!input.hasPlayerTemplate)
			continue;

		const AsciiString startingBuilding =
			player->getPlayerTemplate()->getStartingBuilding();
		const ThingTemplate *primaryTemplate = startingBuilding.isNotEmpty()
			? TheThingFactory->findTemplate(startingBuilding) : nullptr;
		if (!primaryTemplate || !primaryTemplate->isKindOf(KINDOF_COMMANDCENTER))
			continue;
		Object *center = nullptr;
		Bool underConstruction = FALSE;
		input.hasCompletedPrimaryCenter = countMatchingCommandCenters(
			player, primaryTemplate, &center, &underConstruction) == 1 &&
			center && !underConstruction;
		input.hasOtherStructure = input.hasCompletedPrimaryCenter &&
			hasOtherStructure(player, center);
		BuildListInfo *info = input.hasCompletedPrimaryCenter
			? findBuildInfo(player, primaryTemplate, center->getID()) : nullptr;
		input.hasBuildInfo = info != nullptr;
		input.hasScoreKeeper = player->getScoreKeeper() != nullptr;
		input.compatibleBuilderCount = countCompatibleBuilders(
			player, primaryTemplate);
		input.centerCost = primaryTemplate->calcCostToBuild(player);
		input.cash = player->getMoney() ? player->getMoney()->countMoney() : 0;
		input.reserveAdmitted = player->getMoney() &&
			input.cash <= static_cast<UnsignedInt>(INT_MAX) &&
			player->canSpendForSkirmishAIRecovery(
				static_cast<Int>(input.cash), nullptr, FALSE);
		const Int selectedPlayerIndex = selected->player
			? selected->player->getPlayerIndex() : -1;
		if (!ShouldSelectSkirmishAILegacySaveCandidate(
				hasSelection, selectedPlayerIndex,
				player->getPlayerIndex(), input))
			continue;

		selected->player = player;
		selected->center = center;
		selected->info = info;
		selected->builderCount = input.compatibleBuilderCount;
		selected->cash = input.cash;
		hasSelection = TRUE;
	}
	return hasSelection;
}

Bool tryStartRecoveryProbe(UnsignedInt frame)
{
	LegacySaveRecoveryCandidate candidate;
	if (!findRecoveryProbeCandidate(&candidate))
		return FALSE;

	s_admissionPlayerIndex = candidate.player->getPlayerIndex();
	printf("LEGACY_SAVE_TEST_ADMISSION_OK player=%d cash=%u\n",
		s_admissionPlayerIndex, candidate.cash);
	fflush(stdout);
	return startRecoveryProbeFor(candidate.player, candidate.center,
		candidate.info, candidate.builderCount, frame);
}

void finishSuccess(UnsignedInt endFrame, const char *outcome)
{
	printf("LEGACY_SAVE_TEST_SIMULATION_OK start_frame=%u end_frame=%u frames=%u "
		"updates=%u max_delta=%u recovery=%s duplicate_cc=0 "
		"construction_scaffolds=%d\n",
		s_startFrame, endFrame, s_framesAdvanced, s_updateCalls,
		s_maxFrameDelta, outcome, s_recoveryConstructionScaffolds);
	fflush(stdout);
	s_result = 0;
	s_state = LEGACY_SAVE_TEST_FINISHED;
	emit("PASS");
	requestQuit();
}

void observeRecoveryProbe(Player *player, UnsignedInt frame)
{
	const Int currentMoneySpent = player->getScoreKeeper()->getTotalMoneySpent();
	Object *commandCenter = nullptr;
	Bool underConstruction = FALSE;
	const Int commandCenterCount = countMatchingCommandCenters(
		player, s_recoveryCenterTemplate, &commandCenter, &underConstruction);
	if (commandCenterCount > 1)
	{
		finishFailure("recovery_duplicate_command_center");
		return;
	}

	if (commandCenter && commandCenter->getID() == s_destroyedCenterID)
	{
		finishFailure("destroyed_center_remained_live");
		return;
	}
	if (commandCenter && underConstruction)
	{
		if (commandCenter->getID() != s_recoveryConstructionID)
		{
			s_recoveryConstructionID = commandCenter->getID();
			++s_recoveryConstructionScaffolds;
			s_sawConstructionProgress = FALSE;
			s_sawOrdinaryBuilderProvenance = FALSE;
			s_recoveryConstructionPercent = commandCenter->getConstructionPercent();
			if (s_recoveryConstructionScaffolds >
				LEGACY_SAVE_MAX_CONSTRUCTION_SCAFFOLDS)
			{
				finishFailure("recovery_scaffold_bound");
				return;
			}
			const ObjectID builderID = commandCenter->getBuilderID();
			Object *builder = TheGameLogic->findObjectByID(builderID);
			if (commandCenter->getProducerID() != builderID ||
				commandCenter->testStatus(OBJECT_STATUS_RECONSTRUCTING) ||
				!isLiveObject(builder) || builder->isContained() ||
				builder->getControllingPlayer() != player ||
				!builder->isKindOf(KINDOF_DOZER) ||
				!builder->getAIUpdateInterface() ||
				!builder->getAIUpdateInterface()->getDozerAIInterface() ||
				!TheBuildAssistant->isPossibleToMakeUnit(
					builder, s_recoveryCenterTemplate))
			{
				finishFailure("recovery_scaffold_provenance_invalid");
				return;
			}
			s_sawOrdinaryBuilderProvenance = TRUE;
			printf("LEGACY_SAVE_TEST_RECOVERY_CONSTRUCTION frame=%u player=%d "
				"construction=%u builder=%u producer=%u reconstructing=0 "
				"route=ordinary_builder scaffold=%d\n",
				frame, s_recoveryPlayerIndex, s_recoveryConstructionID,
				builderID, commandCenter->getProducerID(),
				s_recoveryConstructionScaffolds);
			fflush(stdout);
		}
		else if (commandCenter->getConstructionPercent() >
			s_recoveryConstructionPercent)
			s_sawConstructionProgress = TRUE;
		s_recoveryConstructionPercent = commandCenter->getConstructionPercent();
		return;
	}
	if (commandCenter && !underConstruction)
	{
		const Int aggregateSpend =
			currentMoneySpent - s_recoveryBaselineMoneySpent;
		if (s_recoveryConstructionID == INVALID_ID ||
			commandCenter->getID() != s_recoveryConstructionID ||
			!s_sawConstructionProgress ||
			!s_sawOrdinaryBuilderProvenance ||
			aggregateSpend < s_recoveryCenterCost)
		{
			finishFailure("recovery_completion_unattributed");
			return;
		}
		printf("LEGACY_SAVE_TEST_RECOVERY_OK frame=%u player=%d center=%u "
			"destroyed_center=%u route=ordinary_builder provenance=verified "
			"aggregate_spend_delta=%d spend_floor=%d progress=verified "
			"duplicate_cc=0\n",
			frame, s_recoveryPlayerIndex, commandCenter->getID(),
			s_destroyedCenterID, aggregateSpend, s_recoveryCenterCost);
		fflush(stdout);
		finishSuccess(frame, "completed");
		return;
	}
}
}

void RequestSkirmishAILegacySaveTest(const char *basename)
{
	s_requested = TRUE;
	s_saveName[0] = '\0';
	s_state = LEGACY_SAVE_TEST_NOT_STARTED;
	s_result = 1;
	s_loadUpdateCalls = 0;
	s_admissionPlayerIndex = -1;
	s_recoveryFaultApplied = FALSE;
	s_sawConstructionProgress = FALSE;
	s_sawOrdinaryBuilderProvenance = FALSE;
	s_recoveryPlayerIndex = -1;
	s_destroyedCenterID = INVALID_ID;
	s_recoveryConstructionID = INVALID_ID;
	s_recoveryFaultFrame = 0;
	s_recoveryCenterCost = 0;
	s_recoveryBaselineMoneySpent = 0;
	s_recoveryConstructionScaffolds = 0;
	s_recoveryConstructionPercent = 0.0f;
	s_recoveryCenterTemplate = nullptr;
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

		Int skirmishAIPlayers = 0;
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		{
			Player *player = ThePlayerList->getNthPlayer(i);
			if (!player || player->getPlayerType() != PLAYER_COMPUTER)
				continue;
			if (!player->isSkirmishAIPlayer())
				continue;
			++skirmishAIPlayers;
		}
		if (skirmishAIPlayers == 0)
		{
			finishFailure("computer_skirmish_ai_missing");
			return;
		}
		printf("LEGACY_SAVE_TEST_AI_OK players=%d\n", skirmishAIPlayers);
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
	Player *admissionPlayer = s_admissionPlayerIndex >= 0
		? ThePlayerList->getNthPlayer(s_admissionPlayerIndex) : nullptr;
	if (ThePlayerList->getPlayerCount() != s_loadedPlayerCount ||
		currentAIPlayers != s_loadedAIPlayers ||
		(s_admissionPlayerIndex >= 0 &&
			(!admissionPlayer || admissionPlayer->getPlayerType() != PLAYER_COMPUTER ||
				!admissionPlayer->isSkirmishAIPlayer())))
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
	if (!s_recoveryFaultApplied &&
		s_framesAdvanced >= LEGACY_SAVE_SETTLE_FRAMES)
	{
		if (!tryStartRecoveryProbe(endFrame) &&
			s_framesAdvanced >= LEGACY_SAVE_RECOVERY_TIMEOUT_FRAMES)
		{
			finishFailure("recovery_subject_unavailable");
			return;
		}
	}
	if (s_recoveryFaultApplied)
	{
		Player *recoveryPlayer =
			ThePlayerList->getNthPlayer(s_recoveryPlayerIndex);
		if (!recoveryPlayer || recoveryPlayer->getPlayerType() != PLAYER_COMPUTER ||
			!recoveryPlayer->isSkirmishAIPlayer())
		{
			finishFailure("recovery_player_lost");
			return;
		}
		observeRecoveryProbe(recoveryPlayer, endFrame);
		if (s_state == LEGACY_SAVE_TEST_FINISHED)
			return;
		if (endFrame - s_recoveryFaultFrame >
			LEGACY_SAVE_RECOVERY_TIMEOUT_FRAMES)
		{
			finishFailure("recovery_timeout");
			return;
		}
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
