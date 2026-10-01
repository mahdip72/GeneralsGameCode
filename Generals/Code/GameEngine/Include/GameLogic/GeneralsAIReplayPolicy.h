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
#include "GameLogic/GameLogic.h"

inline Bool ShouldMarkGeneralsAICanonicalRecording(
	Int originalGameMode, Bool runtimeUsesCurrentEpoch)
{
	if (!runtimeUsesCurrentEpoch)
		return false;
	switch ((GameMode)originalGameMode)
	{
		case GAME_SINGLE_PLAYER:
		case GAME_SKIRMISH:
		case GAME_LAN:
		case GAME_INTERNET:
			// Fresh native network games use this same epoch even when negotiated
			// worker admission falls back to the canonical owner implementation.
			return true;
		case GAME_REPLAY:
		case GAME_SHELL:
		case GAME_NONE:
		default:
			return false;
	}
}

inline Int GetGeneralsAIRecordingEpoch(
	Int originalGameMode, Bool runtimeUsesCurrentEpoch)
{
	return ShouldMarkGeneralsAICanonicalRecording(originalGameMode,
		runtimeUsesCurrentEpoch) ? SKIRMISH_AI_REPLAY_EPOCH_CURRENT :
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
}
