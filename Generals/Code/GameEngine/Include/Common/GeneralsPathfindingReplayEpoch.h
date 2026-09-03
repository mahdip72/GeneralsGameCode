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
#include "GameLogic/GeneralsAIReplayPolicy.h"

enum GeneralsPathfindingReplayEpochType
{
	GENERALS_PATHFINDING_REPLAY_EPOCH_LEGACY = 0,
	GENERALS_PATHFINDING_REPLAY_EPOCH_CURRENT = 1
};

inline const WideChar *GetGeneralsPathfindingCurrentReplayMarker()
{
	return L" [GeneralsPathfindingEpoch=1]";
}

inline const WideChar *GetGeneralsPathfindingReplayMarkerPrefix()
{
	return L"[GeneralsPathfindingEpoch";
}

// Write this on the base build time before the existing terminal AI marker.
// Existing families, including malformed/future spellings, are never repaired
// or reordered into a current recording by this writer.
inline void MarkReplayVersionForGeneralsPathfindingCurrentEpoch(
	UnicodeString &versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString.str(),
		GetGeneralsPathfindingReplayMarkerPrefix()) == 0 &&
		CountSkirmishAIReplayMarkers(versionTimeString.str(),
			L"[GeneralsAIPlanningEpoch") == 0)
	{
		versionTimeString.concat(GetGeneralsPathfindingCurrentReplayMarker());
	}
}

inline Int GetGeneralsPathfindingReplayEpoch(const WideChar *versionTimeString)
{
	// Count the entire family root, not just the '=' spelling, so a valid pair
	// cannot conceal an earlier malformed or unknown marker from either family.
	if (CountSkirmishAIReplayMarkers(versionTimeString,
		GetGeneralsPathfindingReplayMarkerPrefix()) == 1 &&
		CountSkirmishAIReplayMarkers(versionTimeString,
			L"[GeneralsAIPlanningEpoch") == 1 &&
		SkirmishAIReplayVersionEndsWith(versionTimeString,
			L" [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]"))
	{
		return GENERALS_PATHFINDING_REPLAY_EPOCH_CURRENT;
	}
	return GENERALS_PATHFINDING_REPLAY_EPOCH_LEGACY;
}

inline Int GetGeneralsPathfindingReplayEpoch(const UnicodeString &versionTimeString)
{
	return GetGeneralsPathfindingReplayEpoch(versionTimeString.str());
}

inline Int GetGeneralsPathfindingRecordingEpoch(Int originalGameMode,
	Bool nativeRuntime, Bool networkGame, Int aiPlanningEpoch)
{
	return nativeRuntime && !networkGame &&
		aiPlanningEpoch == SKIRMISH_AI_REPLAY_EPOCH_CURRENT &&
		ShouldMarkGeneralsAICanonicalRecording(originalGameMode, true) ?
		GENERALS_PATHFINDING_REPLAY_EPOCH_CURRENT :
		GENERALS_PATHFINDING_REPLAY_EPOCH_LEGACY;
}

inline Int GetGeneralsPathfindingPlaybackEpoch(const WideChar *versionTimeString,
	Int originalGameMode, Bool nativeRuntime)
{
	return nativeRuntime &&
		ShouldMarkGeneralsAICanonicalRecording(originalGameMode, true) ?
		GetGeneralsPathfindingReplayEpoch(versionTimeString) :
		GENERALS_PATHFINDING_REPLAY_EPOCH_LEGACY;
}

inline Bool HasCurrentGeneralsPathfindingReplayEpoch(Int pathfindingEpoch,
	Int aiPlanningEpoch)
{
	return pathfindingEpoch == GENERALS_PATHFINDING_REPLAY_EPOCH_CURRENT &&
		aiPlanningEpoch == SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
}
