/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Common/GameType.h"
#include <math.h>

namespace SkirmishAITunnelRoute
{
	enum { MAX_GENERATED_FORWARD_ENDPOINTS = 3,
		MAX_EXHAUSTED_FORWARD_TARGETS = 8 };

	struct Endpoint
	{
		ObjectID objectID;
		Real x;
		Real y;
		Bool usable;
		Bool groundApproachReachable;
		Bool groundExitReachable;
	};

	struct Plan
	{
		ObjectID entryTunnelID;
		ObjectID exitTunnelID;
		Bool found;
	};

	inline Bool IsForwardSiteClearOfBlocker(Real blockerX, Real blockerY,
		Real targetX, Real targetY, Real siteX, Real siteY,
		Real blockerRadius, Real targetRadius, Real tunnelRadius,
		Real cellClearance)
	{
		const Real dx = targetX - blockerX;
		const Real dy = targetY - blockerY;
		const Real distance = (Real)sqrt((double)dx * dx + (double)dy * dy);
		const Real fromBlockerX = siteX - blockerX;
		const Real fromBlockerY = siteY - blockerY;
		const Real clearance = blockerRadius + tunnelRadius + cellClearance;
		const Real targetClearance = targetRadius + tunnelRadius + cellClearance;
		if (distance <= clearance + targetClearance)
			return FALSE;
		const Real projection =
			(fromBlockerX * dx + fromBlockerY * dy) / distance;
		return projection + 0.01f >= clearance &&
			projection <= distance - targetClearance &&
			fromBlockerX * fromBlockerX + fromBlockerY * fromBlockerY + 0.01f >=
				clearance * clearance;
	}

	// A ground approach is outside the structure footprint, facing the origin.
	// The second probe turns to one deterministic side of the same ring.
	inline Bool GetApproachPoint(Real centerX, Real centerY,
		Real originX, Real originY, Real radius, Real clearance,
		Int candidate, Real *x, Real *y)
	{
		if (!x || !y || candidate < 0 || candidate > 1)
			return false;
		Real dx = originX - centerX;
		Real dy = originY - centerY;
		Real length = (Real)sqrt((double)dx * dx + (double)dy * dy);
		if (length <= 1.0f) {
			dx = 1.0f;
			dy = 0.0f;
			length = 1.0f;
		}
		if (candidate == 1) {
			const Real turnX = -dy;
			dy = dx;
			dx = turnX;
		}
		const Real offset = radius + clearance;
		*x = centerX + dx * offset / length;
		*y = centerY + dy * offset / length;
		return true;
	}

	// Enumerate every unordered pair once before wrapping. The caller advances
	// the cursor only when a probe actually runs, so skipped scans lose no pairs.
	inline Bool SelectPairIndices(Int endpointCount, UnsignedInt cursor,
		Int *entryIndex, Int *exitIndex)
	{
		if (endpointCount < 2 || !entryIndex || !exitIndex)
			return false;
		const UnsignedInt pairCount =
			(UnsignedInt)endpointCount * (UnsignedInt)(endpointCount - 1) / 2;
		UnsignedInt pair = cursor % pairCount;
		for (Int first = 0; first < endpointCount - 1; ++first) {
			const UnsignedInt rowLength = (UnsignedInt)(endpointCount - first - 1);
			if (pair < rowLength) {
				*entryIndex = first;
				*exitIndex = first + 1 + (Int)pair;
				return true;
			}
			pair -= rowLength;
		}
		return false;
	}

	inline Int SelectSiteIndex(Int siteCount, UnsignedInt cursor)
	{
		return siteCount > 0 ? (Int)(cursor % (UnsignedInt)siteCount) : -1;
	}

	// Advance only after a complete negative endpoint probe. A zero remainder
	// records a completed sweep rather than restarting the same prefix.
	inline Bool AdvanceEndpointSweep(UnsignedInt *cursor,
		UnsignedInt *remaining)
	{
		if (!cursor || !remaining || !*remaining)
			return FALSE;
		++*cursor;
		--*remaining;
		return TRUE;
	}

	inline Bool ShouldRestartEndpointSweep(UnsignedInt remaining,
		UnsignedInt now, UnsignedInt retryAfterFrame)
	{
		return remaining == 0 && retryAfterFrame != 0 &&
			now - retryAfterFrame < 0x80000000U;
	}

	inline UnsignedInt BuilderWindowCount(UnsignedInt builderCount,
		UnsignedInt windowSize)
	{
		return builderCount && windowSize ?
			1 + (builderCount - 1) / windowSize : 0;
	}

	inline Bool ShouldReplaceLostEndpoint(Bool attempted,
		ObjectID builtEndpointID, Bool endpointLive)
	{
		return attempted && builtEndpointID != INVALID_ID && !endpointLive;
	}

	inline Int FindFreeGeneratedForwardEndpointSlot(const ObjectID *endpointIDs)
	{
		if (!endpointIDs)
			return -1;
		for (Int i = 0; i < MAX_GENERATED_FORWARD_ENDPOINTS; ++i)
			if (endpointIDs[i] == INVALID_ID)
				return i;
		return -1;
	}

	inline Bool RecordGeneratedForwardEndpoint(ObjectID *endpointIDs,
		ObjectID *targetIDs, ObjectID endpointID, ObjectID targetID)
	{
		if (!endpointIDs || !targetIDs || endpointID == INVALID_ID)
			return FALSE;
		for (Int i = 0; i < MAX_GENERATED_FORWARD_ENDPOINTS; ++i)
			if (endpointIDs[i] == endpointID) {
				targetIDs[i] = targetID;
				return TRUE;
			}
		const Int slot = FindFreeGeneratedForwardEndpointSlot(endpointIDs);
		if (slot < 0)
			return FALSE;
		endpointIDs[slot] = endpointID;
		targetIDs[slot] = targetID;
		return TRUE;
	}

	inline Bool IsTrackedGeneratedForwardEndpoint(ObjectID endpointID,
		const ObjectID *generatedEndpointIDs)
	{
		if (endpointID == INVALID_ID || !generatedEndpointIDs)
			return FALSE;
		for (Int i = 0; i < MAX_GENERATED_FORWARD_ENDPOINTS; ++i)
			if (generatedEndpointIDs[i] == endpointID)
				return TRUE;
		return FALSE;
	}

	inline Bool ShouldOfferForwardRetry(Bool allowRetry,
		Bool retryConsumedForCurrentTarget)
	{
		return allowRetry && !retryConsumedForCurrentTarget;
	}

	inline Bool IsForwardRetryForTarget(Bool retryPhaseAvailable,
		ObjectID retryTargetID, ObjectID requestedTargetID)
	{
		return retryPhaseAvailable && requestedTargetID != INVALID_ID &&
			retryTargetID == requestedTargetID;
	}

	// A full shared tunnel can clear when another team exits. Keep one
	// occupancy episode bounded even if strategy changes objectives.
	inline Bool DeferFullTunnelCapacity(ObjectID targetID, UnsignedInt now,
		UnsignedInt waitFrames, ObjectID *waitingTargetID,
		UnsignedInt *deadlineFrame)
	{
		if (targetID == INVALID_ID || !waitFrames || !waitingTargetID ||
			!deadlineFrame)
			return FALSE;
		if (!*deadlineFrame) {
			*deadlineFrame = now + waitFrames;
			if (!*deadlineFrame)
				*deadlineFrame = 1;
		}
		*waitingTargetID = targetID;
		return now - *deadlineFrame >= 0x80000000U;
	}

	// A completed exit can fund an attempt against a later blocked objective.
	// Keep each live generated exit tied to its original target so target
	// oscillation does not duplicate it. Stock tunnels consume no slots.
	inline Bool CanAttemptForwardEndpoint(Bool attempted,
		ObjectID attemptedTargetID, ObjectID latestEndpointID,
		Bool latestEndpointLive, ObjectID requestedTargetID,
		const ObjectID *generatedEndpointIDs,
		const ObjectID *generatedTargetIDs, Bool retryAvailable,
		const ObjectID *exhaustedTargetIDs)
	{
		(void)latestEndpointID;
		(void)latestEndpointLive;
		if (requestedTargetID == INVALID_ID || !generatedEndpointIDs ||
			!generatedTargetIDs || !exhaustedTargetIDs ||
			FindFreeGeneratedForwardEndpointSlot(generatedEndpointIDs) < 0)
			return FALSE;
		for (Int exhaustedIndex = 0; exhaustedIndex < MAX_EXHAUSTED_FORWARD_TARGETS; ++exhaustedIndex)
			if (exhaustedTargetIDs[exhaustedIndex] == requestedTargetID)
				return FALSE;
		for (Int generatedIndex = 0; generatedIndex < MAX_GENERATED_FORWARD_ENDPOINTS; ++generatedIndex)
			if (generatedEndpointIDs[generatedIndex] != INVALID_ID &&
				generatedTargetIDs[generatedIndex] == requestedTargetID)
				return FALSE;
		if (!attempted || retryAvailable)
			return TRUE;
		Bool roomForHistory = FALSE;
		for (Int historyIndex = 0; historyIndex < MAX_EXHAUSTED_FORWARD_TARGETS; ++historyIndex)
			if (exhaustedTargetIDs[historyIndex] == INVALID_ID) {
				roomForHistory = TRUE;
				break;
			}
		// A failed target stays exhausted above. A later target gets its own
		// first attempt even when the latest generated exit was destroyed.
		return roomForHistory && attemptedTargetID != INVALID_ID &&
			attemptedTargetID != requestedTargetID;
	}

	inline void RememberExhaustedForwardTarget(ObjectID targetID,
		ObjectID *exhaustedTargetIDs)
	{
		if (targetID == INVALID_ID || !exhaustedTargetIDs)
			return;
		for (Int knownIndex = 0; knownIndex < MAX_EXHAUSTED_FORWARD_TARGETS; ++knownIndex)
			if (exhaustedTargetIDs[knownIndex] == targetID)
				return;
		for (Int freeIndex = 0; freeIndex < MAX_EXHAUSTED_FORWARD_TARGETS; ++freeIndex)
			if (exhaustedTargetIDs[freeIndex] == INVALID_ID) {
				exhaustedTargetIDs[freeIndex] = targetID;
				return;
			}
	}

	inline Bool ShouldConsumeHomeRetry(UnsignedInt liveEndpointCount)
	{
		return liveEndpointCount >= 2;
	}

	inline Bool CanReservePairQueries(Int required, Int attemptUsed,
		Int updateUsed, Int attemptLimit, Int updateLimit)
	{
		return required >= 0 && attemptUsed >= 0 && updateUsed >= 0 &&
			attemptUsed <= attemptLimit && updateUsed <= updateLimit &&
			required <= attemptLimit - attemptUsed &&
			required <= updateLimit - updateUsed;
	}

	inline Int EndpointSideQueryAllowance(Int memberCount)
	{
		return memberCount >= 0 && memberCount <= 32 ?
			memberCount * 2 : -1;
	}

	inline Int LivePairQueryAllowance(Int memberCount)
	{
		return memberCount >= 0 && memberCount <= 32 ?
			memberCount * 4 : -1;
	}

	inline Bool ProbeEpochExpired(UnsignedInt now,
		UnsignedInt startFrame, UnsignedInt durationFrames)
	{
		return now - startFrame >= durationFrames;
	}

	inline double DistanceSquared(Real x1, Real y1, Real x2, Real y2)
	{
		double dx = (double)x1 - (double)x2;
		double dy = (double)y1 - (double)y2;
		return dx * dx + dy * dy;
	}

	// Only the endpoint farther from the target can be a useful entry.
	inline Bool SelectDirectedPair(const Endpoint *endpoints, Real targetX,
		Real targetY, double minimumGainSquared, Int *entryIndex,
		Int *exitIndex)
	{
		if (!endpoints || !entryIndex || !exitIndex)
			return false;
		const double first = DistanceSquared(endpoints[0].x, endpoints[0].y,
			targetX, targetY);
		const double second = DistanceSquared(endpoints[1].x, endpoints[1].y,
			targetX, targetY);
		if (second + minimumGainSquared < first) {
			*entryIndex = 0;
			*exitIndex = 1;
			return true;
		}
		if (first + minimumGainSquared < second) {
			*entryIndex = 1;
			*exitIndex = 0;
			return true;
		}
		return false;
	}

	// Ignore defenses beyond either objective and those hugging an endpoint;
	// a corridor blocker must lie near the interior of the assault segment.
	inline Bool IsCorridorBlocker(Real startX, Real startY,
		Real targetX, Real targetY, Real defenseX, Real defenseY,
		Real maximumLateralDistance)
	{
		const double vx = (double)targetX - startX;
		const double vy = (double)targetY - startY;
		const double lengthSquared = vx * vx + vy * vy;
		if (lengthSquared <= 1.0 || maximumLateralDistance <= 0.0f)
			return false;
		const double wx = (double)defenseX - startX;
		const double wy = (double)defenseY - startY;
		const double projection = (wx * vx + wy * vy) / lengthSquared;
		if (projection < 0.1 || projection > 0.9)
			return false;
		const double lateralX = wx - projection * vx;
		const double lateralY = wy - projection * vy;
		return lateralX * lateralX + lateralY * lateralY <=
			(double)maximumLateralDistance * maximumLateralDistance;
	}

	/*
	 * Select a ground-only GLA tunnel route after the caller has found a reason
	 * to bypass the direct assault corridor. Endpoint usability includes owner,
	 * lifecycle, and network-capacity checks. Reachability must be checked for
	 * every ground movement profile that the assault group will send through the
	 * tunnel.
	 *
	 * The target-distance gain is in squared world units. Geometric distance is
	 * only used for deterministic endpoint ranking; it does not replace the
	 * caller's pathfinder checks.
	 */
	inline Bool SelectAssaultPlan(
		Bool isGLA,
		Bool hasGroundForce,
		Bool hasAirForce,
		Bool hasTarget,
		Bool bypassRequested,
		Real assaultX,
		Real assaultY,
		Real targetX,
		Real targetY,
		double minimumTargetDistanceGainSquared,
		const Endpoint *endpoints,
		Int endpointCount,
		Plan *plan)
	{
		if (!plan)
			return false;

		plan->entryTunnelID = INVALID_ID;
		plan->exitTunnelID = INVALID_ID;
		plan->found = false;

		if (!isGLA || !hasGroundForce || hasAirForce || !hasTarget ||
			!bypassRequested || !endpoints || endpointCount < 2)
			return false;

		Bool found = false;
		Bool bestImprovesAssaultPosition = false;
		double bestScore = 0.0;
		ObjectID bestEntryID = INVALID_ID;
		ObjectID bestExitID = INVALID_ID;
		const double assaultTargetDistanceSquared = DistanceSquared(
			assaultX, assaultY, targetX, targetY);

		for (Int entryIndex = 0; entryIndex < endpointCount; ++entryIndex)
		{
			const Endpoint &entry = endpoints[entryIndex];
			if (entry.objectID == INVALID_ID || !entry.usable ||
				!entry.groundApproachReachable)
				continue;

			for (Int exitIndex = 0; exitIndex < endpointCount; ++exitIndex)
			{
				const Endpoint &exit = endpoints[exitIndex];
				if (exit.objectID == INVALID_ID || !exit.usable ||
					exit.objectID == entry.objectID ||
					!exit.groundExitReachable)
					continue;

				double entryTargetDistanceSquared = DistanceSquared(
					entry.x, entry.y, targetX, targetY);
				double exitTargetDistanceSquared = DistanceSquared(
					exit.x, exit.y, targetX, targetY);
				if (exitTargetDistanceSquared + minimumTargetDistanceGainSquared >=
					entryTargetDistanceSquared)
					continue;

				// Prefer a route that also advances from the assault's current
				// position. If none does, retain a reachable, useful entry-to-exit
				// bypass: geometry alone cannot rule out a route around a blocked
				// corridor.
				const Bool improvesAssaultPosition =
					exitTargetDistanceSquared + minimumTargetDistanceGainSquared <
					assaultTargetDistanceSquared;
				double score = DistanceSquared(
					assaultX, assaultY, entry.x, entry.y) +
					exitTargetDistanceSquared;
				if (!found ||
					(improvesAssaultPosition && !bestImprovesAssaultPosition) ||
					(improvesAssaultPosition == bestImprovesAssaultPosition &&
					 score < bestScore) ||
					(improvesAssaultPosition == bestImprovesAssaultPosition &&
					 score == bestScore && entry.objectID < bestEntryID) ||
					(improvesAssaultPosition == bestImprovesAssaultPosition &&
					 score == bestScore && entry.objectID == bestEntryID &&
					 exit.objectID < bestExitID))
				{
					found = true;
					bestImprovesAssaultPosition = improvesAssaultPosition;
					bestScore = score;
					bestEntryID = entry.objectID;
					bestExitID = exit.objectID;
				}
			}
		}

		if (!found)
			return false;

		plan->entryTunnelID = bestEntryID;
		plan->exitTunnelID = bestExitID;
		plan->found = true;
		return true;
	}
}
