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

// This header deliberately contains only POD data and pure policy helpers.
// Keep it usable by VC6/C++98 regression tests and by the live AI code.

struct SkirmishAIRecoveryPolicyInput
{
	bool enabled;
	bool everCompleted;
	bool hasPrimaryCommandCenter;
	bool hasConstruction;
	bool hasBuilder;
	bool builderQueued;
	bool builderQueuePaid;
	bool hasBuilderFactory;
	bool noBuilderPath;
	bool builderAffordable;
	bool commandCenterAffordable;
	bool placementReady;
	int commandCenterCost;
	int builderCost;
	int protectedReserve;
};

struct SkirmishAIRecoveryPolicyResult
{
	bool shouldQueueBuilder;
	bool shouldConstructCommandCenter;
	bool recoveryImpossible;
	bool shouldRetry;
	int reserveCost;
};

inline int AddSkirmishAIRecoveryCost(int first, int second)
{
	if (first < 0)
		first = 0;
	if (second < 0)
		second = 0;
	if (first > 2147483647 - second)
		return 2147483647;
	return first + second;
}

inline bool IsSkirmishAIRecoveryBuilderPathUnavailable(
	bool hasBuilder, bool hasContainedBuilderRoute,
	bool builderQueuePaid, bool hasPotentialFactory)
{
	return !hasBuilder && !hasContainedBuilderRoute &&
		!builderQueuePaid && !hasPotentialFactory;
}

inline bool ShouldOrderSkirmishAIRecoveryBuilderExit(
	bool hasCompatibleContainedBuilder, bool hasLiveOwnedContainer,
	bool hasContainInterface, bool containerHasActiveProduction,
	bool builderAlreadyExiting)
{
	return hasCompatibleContainedBuilder && hasLiveOwnedContainer &&
		hasContainInterface && !containerHasActiveProduction &&
		!builderAlreadyExiting;
}

inline bool IsSkirmishAIRecoveryProductionActive(
	unsigned int productionCount)
{
	return productionCount > 0;
}

inline bool IsSkirmishAIRecoveryInsuranceBuilderCandidate(
	bool isContained, bool isDozer, bool isUnmanned)
{
	return !isContained && isDozer && !isUnmanned;
}

inline int GetSkirmishAIRecoveryReserveCost(
	const SkirmishAIRecoveryPolicyInput &input)
{
	if (!input.enabled || !input.everCompleted ||
		input.hasPrimaryCommandCenter || input.hasConstruction ||
		input.noBuilderPath)
		return 0;

	int required = input.commandCenterCost;
	if (!input.hasBuilder && !input.builderQueuePaid)
		required = AddSkirmishAIRecoveryCost(required, input.builderCost);
	return required > input.protectedReserve ? required :
		(input.protectedReserve > 0 ? input.protectedReserve : 0);
}

inline SkirmishAIRecoveryPolicyResult DecideSkirmishAIRecovery(
	const SkirmishAIRecoveryPolicyInput &input)
{
	SkirmishAIRecoveryPolicyResult result;
	result.shouldQueueBuilder = false;
	result.shouldConstructCommandCenter = false;
	result.recoveryImpossible = false;
	result.shouldRetry = false;
	result.reserveCost = GetSkirmishAIRecoveryReserveCost(input);

	if (!input.enabled || !input.everCompleted ||
		input.hasPrimaryCommandCenter || input.hasConstruction)
		return result;

	if (input.noBuilderPath && !input.hasBuilder && !input.builderQueuePaid)
	{
		result.recoveryImpossible = true;
		return result;
	}

	if (!input.hasBuilder && !input.builderQueuePaid)
	{
		// Queue a paid unit, reusing any existing unpaid work order. Merely
		// waiting on that request would let its own reserve starve it forever.
		if (input.hasBuilderFactory && input.builderAffordable)
		{
			result.shouldQueueBuilder = true;
		}
		else
		{
			// Cash, queue, disabled-factory, and placement failures are
			// retryable.  The caller supplies the bounded retry timer.
			result.shouldRetry = true;
		}
		return result;
	}

	if (input.hasBuilder && input.commandCenterAffordable && input.placementReady)
	{
		result.shouldConstructCommandCenter = true;
		return result;
	}

	result.shouldRetry = true;
	return result;
}

inline bool IsSkirmishAIRecoveryGameMode(int gameMode)
{
	// GameMode values are declared in GameLogic.h: LAN=1, SKIRMISH=2,
	// INTERNET=5.  Keep the policy header independent of engine globals.
	return gameMode == 1 || gameMode == 2 || gameMode == 5;
}

// A zero deadline is the unset sentinel and therefore means the retry is due
// immediately.  Keep this check separate from HasSkirmishAIFrameArrived so a
// legitimate frame value at or beyond the signed half-wrap is handled safely.
inline bool IsSkirmishAIRecoveryRetryDue(
	unsigned int currentFrame, unsigned int retryFrame)
{
	return retryFrame == 0 ||
		(int)(currentFrame - retryFrame) >= 0;
}

// Never produce zero for a scheduled deadline: zero is reserved for the
// unset/due sentinel.  Unsigned subtraction in the due check keeps ordinary
// short delays correct across the 32-bit frame wrap.
inline unsigned int GetSkirmishAIRecoveryRetryFrame(
	unsigned int currentFrame, unsigned int delayFrames)
{
	unsigned int retryFrame = currentFrame + delayFrames;
	if (retryFrame == 0)
		retryFrame = 1;
	return retryFrame;
}

inline unsigned int GetSkirmishAIRecoveryEvacuationDeadline(
	unsigned int currentFrame, unsigned int existingDeadline,
	bool hasContainedBuilder, unsigned int graceFrames)
{
	if (!hasContainedBuilder)
		return 0;
	return existingDeadline != 0 ? existingDeadline :
		GetSkirmishAIRecoveryRetryFrame(currentFrame, graceFrames);
}

inline bool IsSkirmishAIRecoveryEvacuationGraceActive(
	unsigned int currentFrame, unsigned int deadline,
	bool hasContainedBuilder)
{
	return hasContainedBuilder && deadline != 0 &&
		!IsSkirmishAIRecoveryRetryDue(currentFrame, deadline);
}

inline unsigned int GetSkirmishAIRecoveryEvacuationDeadlineForVersion(
	int version, unsigned int storedDeadline)
{
	return version >= 4 ? storedDeadline : 0;
}
