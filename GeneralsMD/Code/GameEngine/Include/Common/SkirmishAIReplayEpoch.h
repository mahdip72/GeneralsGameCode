/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include "Common/UnicodeString.h"

#include <wchar.h>

enum SkirmishAIReplayEpochType
{
	SKIRMISH_AI_REPLAY_EPOCH_LEGACY = 0,
	SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS = 1,
	SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG = 2,
	SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG = 3,
	SKIRMISH_AI_REPLAY_EPOCH_CURRENT = SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG
};

inline const WideChar *GetSkirmishAILivenessReplayMarker()
{
	return L" [SkirmishAILiveness=1]";
}

inline const WideChar *GetSkirmishAIAdaptiveGlobalRngReplayMarker()
{
	return L" [SkirmishAIEpoch=2]";
}

inline const WideChar *GetSkirmishAICurrentReplayMarker()
{
	return L" [SkirmishAIEpoch=3]";
}

inline const WideChar *GetSkirmishAIReplayMarkerPrefix()
{
	return L"[SkirmishAI";
}

inline Int CountSkirmishAIReplayMarkers(const UnicodeString& versionTimeString, const WideChar *marker)
{
	Int count = 0;
	const WideChar *position = versionTimeString.str();
	const size_t markerLength = wcslen(marker);
	while ((position = wcsstr(position, marker)) != NULL) {
		++count;
		position += markerLength;
	}
	return count;
}

inline void MarkReplayVersionForSkirmishAILivenessRecovery(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAILivenessReplayMarker());
}

inline void MarkReplayVersionForSkirmishAICurrentEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAICurrentReplayMarker());
}

inline void MarkReplayVersionForSkirmishAIAdaptiveGlobalRngEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAIAdaptiveGlobalRngReplayMarker());
}

inline void MarkReplayVersionForSkirmishAIRecordingCapability(
	UnicodeString& versionTimeString, Bool supportsCounterRngPlanning)
{
	if (supportsCounterRngPlanning)
		MarkReplayVersionForSkirmishAICurrentEpoch(versionTimeString);
	else
		MarkReplayVersionForSkirmishAIAdaptiveGlobalRngEpoch(versionTimeString);
}

inline Bool BuildSupportsSkirmishAICounterRngPlanning()
{
#if defined(_WIN64)
	return TRUE;
#else
	return FALSE;
#endif
}

inline void MarkReplayVersionForSkirmishAIRecordingEpoch(UnicodeString& versionTimeString)
{
	MarkReplayVersionForSkirmishAIRecordingCapability(
		versionTimeString, BuildSupportsSkirmishAICounterRngPlanning());
}

inline Int GetSkirmishAIReplayEpoch(const UnicodeString& versionTimeString)
{
	Int livenessMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAILivenessReplayMarker());
	Int adaptiveMarkerCount = CountSkirmishAIReplayMarkers(
		versionTimeString, GetSkirmishAIAdaptiveGlobalRngReplayMarker());
	Int currentMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAICurrentReplayMarker());
	Int markerLikeCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix());
	if (markerLikeCount != 1 || livenessMarkerCount + adaptiveMarkerCount + currentMarkerCount != 1)
		return SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	if (currentMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAICurrentReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
	if (adaptiveMarkerCount == 1 &&
		versionTimeString.endsWith(GetSkirmishAIAdaptiveGlobalRngReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG;
	if (livenessMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAILivenessReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS;
	return SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
}

inline Bool ReplayVersionUsesSkirmishAILivenessRecovery(const UnicodeString& versionTimeString)
{
	return GetSkirmishAIReplayEpoch(versionTimeString) >= SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS;
}

inline Bool ShouldUseSkirmishAICurrentBehavior(Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG;
}

inline Bool ShouldUseSkirmishAICounterRng(Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame || replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG;
}
