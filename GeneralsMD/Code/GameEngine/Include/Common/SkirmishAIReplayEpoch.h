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
	// Epoch 2 is already used by the PR7-PR9 AI behavior and must remain
	// readable so those recordings continue to replay with their original
	// decisions.
	SKIRMISH_AI_REPLAY_EPOCH_CURRENT = 2,
	SKIRMISH_AI_REPLAY_EPOCH_RECOVERY = 3
};

inline const WideChar *GetSkirmishAILivenessReplayMarker()
{
	return L" [SkirmishAILiveness=1]";
}

inline const WideChar *GetSkirmishAICurrentReplayMarker()
{
	return L" [SkirmishAIEpoch=2]";
}

inline const WideChar *GetSkirmishAIRecoveryReplayMarker()
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

inline void MarkReplayVersionForSkirmishAIRecoveryEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAIRecoveryReplayMarker());
}

// Global writer helper. RecorderClass shadows its unchanged member call only
// for playback compatibility checks; this helper keeps new recordings at the
// latest AI behavior epoch.
inline void MarkReplayVersionForSkirmishAICurrentEpoch(UnicodeString& versionTimeString)
{
	MarkReplayVersionForSkirmishAIRecoveryEpoch(versionTimeString);
}

// Compatibility-only stamp for the already-shipped epoch-2 behavior. The
// RecorderClass playback allow-list uses this on a freshly constructed build
// time string; the global current-epoch writer above intentionally remains the
// epoch-3 stamp for newly recorded replays.
inline void MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAICurrentReplayMarker());
}

inline Int GetSkirmishAIReplayEpoch(const UnicodeString& versionTimeString)
{
	Int livenessMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAILivenessReplayMarker());
	Int currentMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAICurrentReplayMarker());
	Int recoveryMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIRecoveryReplayMarker());
	Int markerLikeCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix());
	if (markerLikeCount != 1 || livenessMarkerCount + currentMarkerCount + recoveryMarkerCount != 1)
		return SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	if (recoveryMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAIRecoveryReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_RECOVERY;
	if (currentMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAICurrentReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
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
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_CURRENT ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY;
}

inline Bool ShouldUseSkirmishAIRecoveryBehavior(Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame || replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY;
}
