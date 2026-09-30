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

// Pure Stage 4 decisions. The caller supplies only currently visible, eligible
// hostile combat objects and route anchors; this helper never queries game state.
enum SkirmishAIDefenseRoute
{
	SKIRMISH_AI_DEFENSE_NO_ROUTE = -1,
	SKIRMISH_AI_DEFENSE_CENTER = 0,
	SKIRMISH_AI_DEFENSE_FLANK = 1,
	SKIRMISH_AI_DEFENSE_BACKDOOR = 2,
	SKIRMISH_AI_DEFENSE_ROUTE_COUNT = 3
};

struct SkirmishAIDefenseAnchor
{
	int x;		// Quantized offset from the base center, at a common radius.
	int y;
	bool available;
};

struct SkirmishAIDefenseThreat
{
	int approachScore[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
	int penetrationScore[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
	int visibleCount[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
};

struct SkirmishAIDefenseBuildCounts
{
	int owned[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
	int queued[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
};

inline bool IsSkirmishAIDefenseRoute(int route)
{
	return route >= 0 && route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT;
}

inline bool TryConsumeSkirmishAITacticalPathQuery(int *used, int limit)
{
	if (!used || *used < 0 || limit <= 0 || *used >= limit)
		return false;
	++*used;
	return true;
}

inline void GetSkirmishAIDefensePlacementPhase(
	unsigned int attempt, double sideStep, double *radial,
	double *lateral)
{
	const double radialOffsets[8] =
		{ 0.0, 0.0, 35.0, -35.0, 70.0, -70.0, 105.0, -105.0 };
	const unsigned int phase = attempt % 8;
	if (radial) *radial = radialOffsets[phase];
	if (lateral) *lateral = phase & 1 ? sideStep * 0.5 : 0.0;
}

inline unsigned int GetSkirmishAITacticalTeamProbeStartIndex(
	unsigned int scanTick, unsigned int teamCount, unsigned int windowSize)
{
	if (!teamCount || !windowSize)
		return 0;
	const unsigned int windowsPerSweep =
		(teamCount - 1) / windowSize + 1;
	const unsigned int window = scanTick % windowsPerSweep;
	const unsigned int sweep = scanTick / windowsPerSweep;
	// Move each window by one team per sweep. A fixed step greater than one
	// can repeatedly skip teams when it shares a divisor with teamCount.
	return (window * windowSize + sweep % teamCount) % teamCount;
}

inline bool DoSkirmishAIDefenseFootprintsOverlap(
	double firstX, double firstY, double firstRadius,
	double secondX, double secondY, double secondRadius)
{
	if (firstRadius < 0.0 || secondRadius < 0.0)
		return false;
	const double dx = firstX - secondX;
	const double dy = firstY - secondY;
	const double radius = firstRadius + secondRadius;
	return dx * dx + dy * dy <= radius * radius;
}

inline int ClampSkirmishAIDefenseCoordinate(int value)
{
	if (value < -32767) return -32767;
	if (value > 32767) return 32767;
	return value;
}

// Anchor offsets must be on approximately the same base-centered ring. A
// missing waypoint is marked unavailable. Equal distances favor Center, then
// Flank, then Backdoor, independently of object/container iteration order.
inline int ClassifySkirmishAIDefenseRoute(
	int objectX, int objectY,
	const SkirmishAIDefenseAnchor anchors[SKIRMISH_AI_DEFENSE_ROUTE_COUNT])
{
	if (!anchors)
		return SKIRMISH_AI_DEFENSE_NO_ROUTE;
	objectX = ClampSkirmishAIDefenseCoordinate(objectX);
	objectY = ClampSkirmishAIDefenseCoordinate(objectY);
	int bestRoute = SKIRMISH_AI_DEFENSE_NO_ROUTE;
	__int64 bestDistance = 0;
	for (int route = 0; route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++route) {
		if (!anchors[route].available)
			continue;
		const __int64 dx = (__int64)objectX -
			ClampSkirmishAIDefenseCoordinate(anchors[route].x);
		const __int64 dy = (__int64)objectY -
			ClampSkirmishAIDefenseCoordinate(anchors[route].y);
		const __int64 distance = dx * dx + dy * dy;
		if (bestRoute == SKIRMISH_AI_DEFENSE_NO_ROUTE || distance < bestDistance) {
			bestRoute = route;
			bestDistance = distance;
		}
	}
	return bestRoute;
}

inline void ClearSkirmishAIDefenseThreat(SkirmishAIDefenseThreat *threat)
{
	if (!threat) return;
	for (int route = 0; route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++route) {
		threat->approachScore[route] = 0;
		threat->penetrationScore[route] = 0;
		threat->visibleCount[route] = 0;
	}
}

// The caller must use the product's shroud/stealth and enemy relationship
// checks. These flags make accidental hidden or friendly observations inert.
// Scores saturate, so a crowded route cannot overflow or depend on input size.
inline void AddSkirmishAIDefenseObservation(
	SkirmishAIDefenseThreat *threat, int route, bool visible,
	bool hostileCombat, bool penetrated, int value)
{
	if (!threat || !IsSkirmishAIDefenseRoute(route) || !visible ||
		!hostileCombat || value <= 0)
		return;
	if (value > 1000) value = 1000;
	int *score = penetrated ? &threat->penetrationScore[route]
		: &threat->approachScore[route];
	*score = *score >= 10000 - value ? 10000 : *score + value;
	if (threat->visibleCount[route] < 10000)
		++threat->visibleCount[route];
}

inline int GetSkirmishAIDefenseRouteScore(
	const SkirmishAIDefenseThreat *threat, int route)
{
	if (!threat || !IsSkirmishAIDefenseRoute(route)) return 0;
	const int approach = threat->approachScore[route];
	const int penetration = threat->penetrationScore[route];
	// A unit already inside the line weighs twice an approaching unit.
	return approach + 2 * penetration;
}

inline int SelectSkirmishAIDefenseRoute(
	const SkirmishAIDefenseThreat *threat, int minimumScore)
{
	if (!threat) return SKIRMISH_AI_DEFENSE_NO_ROUTE;
	if (minimumScore < 1) minimumScore = 1;
	int bestRoute = SKIRMISH_AI_DEFENSE_NO_ROUTE;
	int bestScore = minimumScore - 1;
	for (int route = 0; route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++route) {
		const int score = GetSkirmishAIDefenseRouteScore(threat, route);
		if (score > bestScore) {
			bestScore = score;
			bestRoute = route;
		}
	}
	return bestRoute;
}

// Count live, under-construction, and BuildListInfo entries in owned/queued.
// The caller still gates spending, legal placement, and critical recovery.
inline bool ShouldQueueSkirmishAIDefense(
	const SkirmishAIDefenseThreat *threat,
	const SkirmishAIDefenseBuildCounts *counts, int route,
	int minimumScore, int perRouteCap, int totalCap)
{
	if (!threat || !counts || !IsSkirmishAIDefenseRoute(route) ||
		perRouteCap <= 0 || totalCap <= 0 ||
		GetSkirmishAIDefenseRouteScore(threat, route) <= 0 ||
		GetSkirmishAIDefenseRouteScore(threat, route) < minimumScore)
		return false;
	__int64 total = 0;
	for (int i = 0; i < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++i) {
		if (counts->owned[i] < 0 || counts->queued[i] < 0)
			return false;
		if (i == route &&
			(__int64)counts->owned[i] + counts->queued[i] >= perRouteCap)
			return false;
		total += (__int64)counts->owned[i] + counts->queued[i];
	}
	return total < totalCap;
}

// Return an assignment only when the caller owns an eligible defensive team
// and its scripted orders permit an AI patrol command. A margin avoids route
// thrashing while the current route remains threatened.
inline int DecideSkirmishAIDefensePatrolRoute(
	const SkirmishAIDefenseThreat *threat, int assignedRoute,
	bool canCommandTeam, bool reassignmentDue,
	int minimumScore, int switchMargin)
{
	if (!canCommandTeam || !reassignmentDue)
		return assignedRoute;
	const int bestRoute = SelectSkirmishAIDefenseRoute(threat, minimumScore);
	if (bestRoute == SKIRMISH_AI_DEFENSE_NO_ROUTE)
		return SKIRMISH_AI_DEFENSE_NO_ROUTE;
	if (IsSkirmishAIDefenseRoute(assignedRoute) && assignedRoute != bestRoute) {
		if (switchMargin < 0) switchMargin = 0;
		const int current = GetSkirmishAIDefenseRouteScore(threat, assignedRoute);
		const int best = GetSkirmishAIDefenseRouteScore(threat, bestRoute);
		if (current >= minimumScore && best - current <= switchMargin)
			return assignedRoute;
	}
	return bestRoute;
}

inline int SelectQuietSkirmishAIDefenseRoute(
	const SkirmishAIDefenseAnchor anchors[SKIRMISH_AI_DEFENSE_ROUTE_COUNT],
	unsigned int patrolTick, int playerIndex)
{
	if (!anchors) return SKIRMISH_AI_DEFENSE_NO_ROUTE;
	const unsigned int start =
		(patrolTick + (unsigned int)(playerIndex < 0 ? 0 : playerIndex)) %
		SKIRMISH_AI_DEFENSE_ROUTE_COUNT;
	for (unsigned int offset = 0;
		offset < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++offset) {
		const int route = (int)((start + offset) %
			SKIRMISH_AI_DEFENSE_ROUTE_COUNT);
		if (anchors[route].available) return route;
	}
	return SKIRMISH_AI_DEFENSE_NO_ROUTE;
}

inline int AdvanceQuietSkirmishAIDefenseRoute(
	const SkirmishAIDefenseAnchor anchors[SKIRMISH_AI_DEFENSE_ROUTE_COUNT],
	int previousRoute, unsigned int patrolTick, int playerIndex)
{
	if (!anchors) return SKIRMISH_AI_DEFENSE_NO_ROUTE;
	if (!IsSkirmishAIDefenseRoute(previousRoute))
		return SelectQuietSkirmishAIDefenseRoute(anchors, patrolTick, playerIndex);
	for (int offset = 1; offset < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++offset) {
		const int route = (previousRoute + offset) %
			SKIRMISH_AI_DEFENSE_ROUTE_COUNT;
		if (anchors[route].available) return route;
	}
	return anchors[previousRoute].available ? previousRoute :
		SelectQuietSkirmishAIDefenseRoute(anchors, patrolTick, playerIndex);
}

inline bool ShouldHoldQuietSkirmishAIDefenseWaypoint(
	bool quiet, bool sameDefender, bool routeAvailable,
	bool deadlineReached, float distanceSquared, float arrivalRadius)
{
	return quiet && sameDefender && routeAvailable && !deadlineReached &&
		arrivalRadius > 0.0f &&
		distanceSquared > arrivalRadius * arrivalRadius;
}

// The same corridor is applied to completed defenses and pending build entries.
inline bool IsSkirmishAIDefenseLinePosition(
	int along, int lateral, int baseRadius, int structureRadius)
{
	if (baseRadius <= 0 || structureRadius < 0) return false;
	int minimumAlong = baseRadius * 55 / 100;
	if (minimumAlong < 120) minimumAlong = 120;
	int maximumLateral = structureRadius * 4 + 120;
	if (maximumLateral < 260) maximumLateral = 260;
	return along >= minimumAlong && along <= baseRadius + 250 &&
		lateral >= -maximumLateral && lateral <= maximumLateral;
}

inline bool ShouldSkirmishAIDefenderPursue(
	bool visibleHostile, bool capable, bool pathAvailable,
	int targetDistanceFromBase, int leash,
	int defenderValue, int nearbyEnemyValue)
{
	return visibleHostile && capable && pathAvailable && leash > 150 &&
		targetDistanceFromBase >= 0 &&
		targetDistanceFromBase <= leash - 150 &&
		defenderValue > 0 && nearbyEnemyValue > 0 &&
		(__int64)defenderValue * 4 >= (__int64)nearbyEnemyValue * 5;
}

inline bool ShouldRecallSkirmishAIDefender(
	bool targetVisibleAndHostile, bool teamInsideLeash,
	bool targetInsideLeash, bool counterStillFavorable,
	bool baseSafe, unsigned int elapsedFrames, unsigned int maximumFrames)
{
	return !targetVisibleAndHostile || !teamInsideLeash ||
		!targetInsideLeash || !counterStillFavorable || !baseSafe ||
		elapsedFrames >= maximumFrames;
}
