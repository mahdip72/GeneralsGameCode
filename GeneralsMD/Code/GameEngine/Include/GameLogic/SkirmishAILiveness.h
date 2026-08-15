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

inline bool ShouldUseSkirmishAILivenessRecovery(bool isReplayGame, bool replayRecordedWithRecovery)
{
	return !isReplayGame || replayRecordedWithRecovery;
}

inline int GetPathQueueRetryDelay(bool queued)
{
	return queued ? 0 : 1;
}

inline bool IsSkirmishWaypointCandidateBetter(
	bool hasCurrent,
	int currentPriority,
	bool currentPassable,
	float currentDistanceSqr,
	int candidatePriority,
	bool candidatePassable,
	float candidateDistanceSqr)
{
	if (!hasCurrent)
		return true;
	if (candidatePassable != currentPassable)
		return candidatePassable;
	if (candidatePriority != currentPriority)
		return candidatePriority > currentPriority;
	return candidateDistanceSqr < currentDistanceSqr;
}

inline int GetSkirmishWaypointFallbackPriority(bool hasIncomingLink, bool bidirectional, unsigned int samePathLinkCount)
{
	if (!hasIncomingLink)
		return 2;
	if (bidirectional && samePathLinkCount <= 1)
		return 1;
	return 0;
}

inline bool IsWorkOrderFactoryQueueValid(bool sameOwner, bool hasProduction, unsigned int queuedUnitCount)
{
	return sameOwner && hasProduction && queuedUnitCount > 0;
}

inline bool IsUsableSupplyCenter(bool exists, bool isRebuildHole)
{
	return exists && !isRebuildHole;
}
