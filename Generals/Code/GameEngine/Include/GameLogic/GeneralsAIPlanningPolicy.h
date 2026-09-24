/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Common/SkirmishAIReplayEpoch.h"
#include "Lib/BaseType.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/SimulationExecutionPolicy.h"

#include <cmath>
#include <string.h>

enum
{
	GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS = 16
};

struct GeneralsAIEnemyCandidateFact
{
	UnsignedInt sourceOrdinal;
	Int playerIndex;
	Real baseDistanceSquared;
	UnsignedInt targetingCandidateMask;
	UnsignedInt targetingOwnerMask;
};

struct GeneralsAIEnemyPlanningSnapshot
{
	UnsignedInt frame;
	UnsignedInt ownerPlayerIndex;
	Real initialBestDistanceSquared;
	UnsignedInt candidateCount;
	GeneralsAIEnemyCandidateFact candidates[GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS];
};

struct GeneralsAIEnemyPlanningResult
{
	UnsignedInt valid;
	Int selectedPlayerIndex;
	Real selectedDistanceSquared;
	rts::AIPlanningOrderKey orderKey;
};

inline void ClearGeneralsAIEnemyPlanningSnapshot(
	GeneralsAIEnemyPlanningSnapshot *snapshot)
{
	if (snapshot)
		memset(snapshot, 0, sizeof(*snapshot));
}

inline void ClearGeneralsAIEnemyPlanningResult(
	GeneralsAIEnemyPlanningResult *result)
{
	if (!result)
		return;
	memset(result, 0, sizeof(*result));
	result->selectedPlayerIndex = -1;
	result->orderKey.sourceOrdinal = rts::AI_PLANNING_INVALID_ORDINAL;
}

inline Bool ValidateGeneralsAIEnemyPlanningSnapshot(
	const GeneralsAIEnemyPlanningSnapshot &snapshot)
{
	if (snapshot.ownerPlayerIndex >= GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS ||
		!std::isfinite(snapshot.initialBestDistanceSquared) ||
		snapshot.initialBestDistanceSquared < 0.0f ||
		snapshot.candidateCount > GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS)
		return false;
	const UnsignedInt validMask =
		(1U << GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS) - 1U;
	for (UnsignedInt i = 0U; i < snapshot.candidateCount; ++i)
	{
		const GeneralsAIEnemyCandidateFact &candidate = snapshot.candidates[i];
		if (candidate.sourceOrdinal >=
			GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS ||
			candidate.playerIndex < 0 || candidate.playerIndex >=
				GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS ||
			(UnsignedInt)candidate.playerIndex == snapshot.ownerPlayerIndex ||
			!std::isfinite(candidate.baseDistanceSquared) ||
			candidate.baseDistanceSquared < 0.0f ||
			(candidate.targetingCandidateMask & ~validMask) != 0U ||
			(candidate.targetingOwnerMask & ~validMask) != 0U ||
			(candidate.targetingCandidateMask & (1U << candidate.sourceOrdinal)) != 0U ||
			(candidate.targetingOwnerMask & (1U << candidate.sourceOrdinal)) != 0U ||
			(i != 0U && candidate.sourceOrdinal <=
				snapshot.candidates[i - 1U].sourceOrdinal))
		{
			return false;
		}
		for (UnsignedInt prior = 0U; prior < i; ++prior)
		{
			if (candidate.playerIndex == snapshot.candidates[prior].playerIndex)
				return false;
		}
	}
	return true;
}

inline Bool PlanGeneralsAIEnemyTarget(
	const GeneralsAIEnemyPlanningSnapshot &snapshot,
	GeneralsAIEnemyPlanningResult *result)
{
	if (!result || !ValidateGeneralsAIEnemyPlanningSnapshot(snapshot))
		return false;
	ClearGeneralsAIEnemyPlanningResult(result);
	result->orderKey.frame = snapshot.frame;
	result->orderKey.playerIndex = snapshot.ownerPlayerIndex;
	result->orderKey.subphase = rts::AI_PLANNING_SUBPHASE_ENEMY_TARGET;

	Real bestDistanceSquared = snapshot.initialBestDistanceSquared;
	for (UnsignedInt i = 0U; i < snapshot.candidateCount; ++i)
	{
		const GeneralsAIEnemyCandidateFact &candidate = snapshot.candidates[i];
		Real distanceSquared = candidate.baseDistanceSquared;
		for (UnsignedInt playerOrdinal = 0U;
			playerOrdinal < GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS;
			++playerOrdinal)
		{
			if (playerOrdinal == candidate.sourceOrdinal)
				continue;
			const UnsignedInt bit = 1U << playerOrdinal;
			if ((candidate.targetingCandidateMask & bit) != 0U)
			{
				distanceSquared += 500.0f * 500.0f;
				if (!std::isfinite(distanceSquared))
					return false;
			}
			if ((candidate.targetingOwnerMask & bit) != 0U)
			{
				distanceSquared -= 25.0f * 25.0f;
				if (!std::isfinite(distanceSquared))
					return false;
				if (distanceSquared < 0.0f)
					distanceSquared = 0.0f;
			}
		}
		if (distanceSquared < bestDistanceSquared)
		{
			bestDistanceSquared = distanceSquared;
			result->selectedPlayerIndex = candidate.playerIndex;
			result->selectedDistanceSquared = distanceSquared;
			result->orderKey.sourceOrdinal = candidate.sourceOrdinal;
		}
	}
	result->valid = 1U;
	return true;
}

inline Bool ValidateGeneralsAIEnemyPlanningResult(
	const GeneralsAIEnemyPlanningSnapshot &snapshot,
	const GeneralsAIEnemyPlanningResult &result)
{
	if (!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot) ||
		result.valid != 1U || result.orderKey.frame != snapshot.frame ||
		result.orderKey.playerIndex != snapshot.ownerPlayerIndex ||
		result.orderKey.subphase != rts::AI_PLANNING_SUBPHASE_ENEMY_TARGET ||
		result.orderKey.emissionOrdinal != 0U)
	{
		return false;
	}
	GeneralsAIEnemyPlanningResult expected;
	if (!PlanGeneralsAIEnemyTarget(snapshot, &expected))
		return false;
	return result.valid == expected.valid &&
		result.selectedPlayerIndex == expected.selectedPlayerIndex &&
		result.selectedDistanceSquared == expected.selectedDistanceSquared &&
		result.orderKey.frame == expected.orderKey.frame &&
		result.orderKey.playerIndex == expected.orderKey.playerIndex &&
		result.orderKey.subphase == expected.orderKey.subphase &&
		result.orderKey.sourceOrdinal == expected.orderKey.sourceOrdinal &&
		result.orderKey.emissionOrdinal == expected.orderKey.emissionOrdinal;
}

inline Bool EqualGeneralsAIEnemyPlanningResult(
	const GeneralsAIEnemyPlanningResult &left,
	const GeneralsAIEnemyPlanningResult &right)
{
	return left.valid == right.valid &&
		left.selectedPlayerIndex == right.selectedPlayerIndex &&
		left.selectedDistanceSquared == right.selectedDistanceSquared &&
		left.orderKey.frame == right.orderKey.frame &&
		left.orderKey.playerIndex == right.orderKey.playerIndex &&
		left.orderKey.subphase == right.orderKey.subphase &&
		left.orderKey.sourceOrdinal == right.orderKey.sourceOrdinal &&
		left.orderKey.emissionOrdinal == right.orderKey.emissionOrdinal;
}

inline rts::AIPlanningExecutionMode GetGeneralsAIPlanningExecutionMode(
	Bool isMultiplayerPolicyBlocked, rts::SimulationExecutionMode simulationMode,
	UnsignedInt workerCount, Bool jobSystemReady)
{
	// This selects topology only. The separate replay/runtime epoch gate decides
	// whether serial means the legacy lane or canonical owner-batch execution.
	if (isMultiplayerPolicyBlocked || !jobSystemReady || workerCount < 2U)
		return rts::AI_PLANNING_EXECUTION_SERIAL;
	if (simulationMode == rts::SIMULATION_EXECUTION_PARALLEL)
		return rts::AI_PLANNING_EXECUTION_PARALLEL;
	if (simulationMode == rts::SIMULATION_EXECUTION_SHADOW)
		return rts::AI_PLANNING_EXECUTION_SHADOW;
	return rts::AI_PLANNING_EXECUTION_SERIAL;
}

inline Bool ShouldUseGeneralsAICanonicalPlanning(
	Bool isNetworkGame, Bool isRecordingGame, Bool isReplayGame,
	Bool replayUsesCurrentEpoch, Bool runtimeUsesCurrentEpoch)
{
	// Network topology selects the negotiated execution lane; it must not turn
	// off the current deterministic epoch.  An unmarked network recording/replay
	// still takes the legacy branch through the explicit epoch inputs below.
	(void)isNetworkGame;
	if (isRecordingGame || isReplayGame)
		return replayUsesCurrentEpoch;
	return runtimeUsesCurrentEpoch;
}

inline Bool ShouldRunGeneralsAIPlanning(
	rts::AIPlanningExecutionMode executionMode)
{
	return executionMode == rts::AI_PLANNING_EXECUTION_PARALLEL ||
		executionMode == rts::AI_PLANNING_EXECUTION_SHADOW;
}
