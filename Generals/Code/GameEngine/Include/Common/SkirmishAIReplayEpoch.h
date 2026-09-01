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

#include "Common/UnicodeString.h"

#include <wchar.h>

enum SkirmishAIReplayEpochType
{
	SKIRMISH_AI_REPLAY_EPOCH_LEGACY = 0,
	SKIRMISH_AI_REPLAY_EPOCH_DETERMINISTIC_PLANNING = 1,
	SKIRMISH_AI_REPLAY_EPOCH_CURRENT =
		SKIRMISH_AI_REPLAY_EPOCH_DETERMINISTIC_PLANNING
};

inline const WideChar *GetSkirmishAICurrentReplayMarker()
{
	return L" [GeneralsAIPlanningEpoch=1]";
}

inline const WideChar *GetSkirmishAIReplayMarkerPrefix()
{
	return L"[GeneralsAIPlanningEpoch=";
}

inline Int CountSkirmishAIReplayMarkers(const WideChar *versionTimeString,
	const WideChar *marker)
{
	if (!versionTimeString || !marker || !*marker)
		return 0;
	Int count = 0;
	const size_t markerLength = wcslen(marker);
	const WideChar *position = versionTimeString;
	while ((position = wcsstr(position, marker)) != NULL)
	{
		++count;
		position += markerLength;
	}
	return count;
}

inline Bool SkirmishAIReplayVersionEndsWith(const WideChar *versionTimeString,
	const WideChar *suffix)
{
	if (!versionTimeString || !suffix)
		return false;
	const size_t valueLength = wcslen(versionTimeString);
	const size_t suffixLength = wcslen(suffix);
	return suffixLength <= valueLength &&
		wcscmp(versionTimeString + valueLength - suffixLength, suffix) == 0;
}

inline void MarkReplayVersionForSkirmishAICurrentEpoch(
	UnicodeString &versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString.str(),
		GetSkirmishAIReplayMarkerPrefix()) == 0)
	{
		versionTimeString.concat(GetSkirmishAICurrentReplayMarker());
	}
}

inline Int GetSkirmishAIReplayEpoch(const WideChar *versionTimeString)
{
	const Int markerLikeCount = CountSkirmishAIReplayMarkers(
		versionTimeString, GetSkirmishAIReplayMarkerPrefix());
	const Int currentMarkerCount = CountSkirmishAIReplayMarkers(
		versionTimeString, GetSkirmishAICurrentReplayMarker());
	if (markerLikeCount == 1 && currentMarkerCount == 1 &&
		SkirmishAIReplayVersionEndsWith(versionTimeString,
			GetSkirmishAICurrentReplayMarker()))
	{
		return SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
	}
	return SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
}

inline Int GetSkirmishAIReplayEpoch(const UnicodeString &versionTimeString)
{
	return GetSkirmishAIReplayEpoch(versionTimeString.str());
}

inline Bool ShouldUseSkirmishAIDeterministicPlanning(Bool isReplayGame,
	Int replayEpoch)
{
	return !isReplayGame || replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
}

inline Bool &GeneralsAICanonicalRuntimeEpochStorage()
{
	static Bool currentEpoch = false;
	return currentEpoch;
}

inline Bool ShouldEnableGeneralsAICanonicalRuntimeEpoch()
{
#if defined(_WIN64)
	return true;
#else
	return false;
#endif
}

inline void SetGeneralsAICanonicalRuntimeEpoch(Bool currentEpoch)
{
	GeneralsAICanonicalRuntimeEpochStorage() = currentEpoch;
}

inline Bool IsGeneralsAICanonicalRuntimeEpoch()
{
	return GeneralsAICanonicalRuntimeEpochStorage();
}
