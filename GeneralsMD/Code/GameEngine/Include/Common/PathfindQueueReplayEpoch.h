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

enum PathfindQueueReplayEpochType
{
	PATHFIND_QUEUE_REPLAY_EPOCH_LEGACY = 0,
	PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT = 1
};

enum
{
	PATHFIND_CELL_INFO_LEGACY_CAPACITY = 30000,
	PATHFIND_CELL_INFO_CURRENT_CAPACITY = 150000
};

inline const WideChar *GetPathfindQueueReplayMarker()
{
	return L" [PathfindQueueEpoch=1]";
}

inline const WideChar *GetPathfindQueueReplayMarkerPrefix()
{
	return L"[PathfindQueueEpoch=";
}

inline Int CountPathfindQueueReplayMarkers(const UnicodeString& versionTimeString, const WideChar *marker)
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

inline void MarkReplayVersionForPathfindQueueCurrentEpoch(UnicodeString& versionTimeString)
{
	if (CountPathfindQueueReplayMarkers(versionTimeString, GetPathfindQueueReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetPathfindQueueReplayMarker());
}

inline Int GetPathfindQueueReplayEpoch(const UnicodeString& versionTimeString)
{
	const Int markerCount = CountPathfindQueueReplayMarkers(versionTimeString, GetPathfindQueueReplayMarkerPrefix());
	const Int currentCount = CountPathfindQueueReplayMarkers(versionTimeString, GetPathfindQueueReplayMarker());
	// The SkirmishAI epoch marker is intentionally appended after this marker,
	// so only exact-count validation is used here rather than requiring the
	// pathfinding marker to be the final suffix.
	if (markerCount != 1 || currentCount != 1)
		return PATHFIND_QUEUE_REPLAY_EPOCH_LEGACY;
	return PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT;
}

inline Bool ReplayVersionUsesPathfindQueueCapacity(const UnicodeString& versionTimeString)
{
	return GetPathfindQueueReplayEpoch(versionTimeString) == PATHFIND_QUEUE_REPLAY_EPOCH_CURRENT;
}

inline Bool ShouldUsePathfindQueueCapacity(Bool isReplayGame, Bool replayRecordedWithCapacity)
{
	return !isReplayGame || replayRecordedWithCapacity;
}

inline UnsignedInt GetPathfindCellInfoCapacityForPolicy(Bool isZeroHour, Bool isReplayGame,
	Bool replayRecordedWithCapacity)
{
	if (!isZeroHour || !ShouldUsePathfindQueueCapacity(isReplayGame, replayRecordedWithCapacity))
		return PATHFIND_CELL_INFO_LEGACY_CAPACITY;
	return PATHFIND_CELL_INFO_CURRENT_CAPACITY;
}
