/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "GameLogic/SkirmishAIStrategy.h"

const Int SKIRMISH_AI_ALLIED_MAX_PLAYERS = 16;

struct SkirmishAIAlliedPlayerFacts
{
	Bool valid;
	Bool alive;
	Bool isAI;
	Bool missingIncome;
	Bool missingProduction;
	Bool recoverable;
	Bool hasReadyForce;
	Bool donationBlocked;
	Bool aidBlocked;

	Int playerIndex;
	Int economyHealth;
	Int baseIntegrity;
	Int armyReadiness;
	Int immediateThreat;
	Int distress;
	Int cash;
	Int protectedReserve;
	Int combatValue;
	Int localCombatValue;
	Int localEnemyValue;
	Int supportAvailableValue;
	Int targetEnemyIndex;
	Int targetScore;

	ObjectID targetObjectID;
	UnsignedInt alliedMask;
	SkirmishStrategyMode mode;
};

struct SkirmishAIAlliedDecision
{
	Int starvationStreaks[16];
	Int assaultLeaderIndices[16];
	Int assaultEnemyIndices[16];
	Int supportRecipientIndices[16];
	Int supportBudgets[16];
	ObjectID assaultTargetIDs[16];

	Int donorIndex;
	Int recipientIndex;
	Int donationAmount;
};

namespace SkirmishAIAlliedCoordinationPolicy
{
	inline Int ClampMetric(Int value)
	{
		if (value < 0)
			return 0;
		if (value > 100)
			return 100;
		return value;
	}

	inline Int ClampStarvationStreak(Int value)
	{
		if (value < 0)
			return 0;
		if (value > 2)
			return 2;
		return value;
	}

	inline Bool IsLivePlayer(const SkirmishAIAlliedPlayerFacts *fact)
	{
		return fact != 0 && fact->valid && fact->alive;
	}

	inline Bool MaskContains(UnsignedInt mask, Int playerIndex)
	{
		return (mask & (((UnsignedInt)1) << playerIndex)) != 0;
	}

	inline Bool AreMutuallyAllied(const SkirmishAIAlliedPlayerFacts *const *players,
		Int firstIndex, Int secondIndex)
	{
		if (firstIndex < 0 || firstIndex >= SKIRMISH_AI_ALLIED_MAX_PLAYERS ||
			secondIndex < 0 || secondIndex >= SKIRMISH_AI_ALLIED_MAX_PLAYERS)
			return FALSE;
		if (firstIndex == secondIndex)
			return TRUE;

		const SkirmishAIAlliedPlayerFacts *first = players[firstIndex];
		const SkirmishAIAlliedPlayerFacts *second = players[secondIndex];
		return IsLivePlayer(first) && IsLivePlayer(second) &&
			MaskContains(first->alliedMask, secondIndex) &&
			MaskContains(second->alliedMask, firstIndex);
	}

	inline Bool IsHostileTo(const SkirmishAIAlliedPlayerFacts *const *players,
		Int playerIndex, Int enemyIndex)
	{
		if (playerIndex < 0 || playerIndex >= SKIRMISH_AI_ALLIED_MAX_PLAYERS ||
			enemyIndex < 0 || enemyIndex >= SKIRMISH_AI_ALLIED_MAX_PLAYERS ||
			playerIndex == enemyIndex || !IsLivePlayer(players[playerIndex]) ||
			!IsLivePlayer(players[enemyIndex]))
			return FALSE;

		// A one-sided ally bit is enough to keep this pair out of assault targeting.
		return !MaskContains(players[playerIndex]->alliedMask, enemyIndex) &&
			!MaskContains(players[enemyIndex]->alliedMask, playerIndex);
	}

	inline Bool HasValidEnemyTarget(const SkirmishAIAlliedPlayerFacts *const *players,
		Int playerIndex)
	{
		const SkirmishAIAlliedPlayerFacts *fact = players[playerIndex];
		return fact->targetObjectID != INVALID_ID &&
			IsHostileTo(players, playerIndex, fact->targetEnemyIndex);
	}

	inline Bool IsAssaultEligible(const SkirmishAIAlliedPlayerFacts *const *players,
		Int playerIndex)
	{
		const SkirmishAIAlliedPlayerFacts *fact = players[playerIndex];
		return IsLivePlayer(fact) && fact->isAI &&
			fact->mode != SKIRMISH_STRATEGY_FORTIFY &&
			ClampMetric(fact->baseIntegrity) >= 65 &&
			ClampMetric(fact->economyHealth) >= 50 &&
			ClampMetric(fact->armyReadiness) >= 60 &&
			ClampMetric(fact->immediateThreat) < 60 &&
			fact->hasReadyForce && HasValidEnemyTarget(players, playerIndex);
	}

	inline Bool IsDonationDonor(const SkirmishAIAlliedPlayerFacts *fact)
	{
		return IsLivePlayer(fact) && fact->isAI && !fact->donationBlocked &&
			ClampMetric(fact->economyHealth) >= 75 &&
			ClampMetric(fact->baseIntegrity) >= 80 &&
			ClampMetric(fact->immediateThreat) <= 40;
	}

	inline Int DonationSurplus(const SkirmishAIAlliedPlayerFacts *fact)
	{
		const Int cash = fact->cash > 0 ? fact->cash : 0;
		const Int reserve = fact->protectedReserve > 0 ? fact->protectedReserve : 0;
		return cash > reserve ? cash - reserve : 0;
	}

	inline Int DonationAmount(const SkirmishAIAlliedPlayerFacts *fact)
	{
		const Int cash = fact->cash > 0 ? fact->cash : 0;
		const Int fifthOfCash = cash / 5;
		const Int surplus = DonationSurplus(fact);
		return fifthOfCash < surplus ? fifthOfCash : surplus;
	}

	inline Bool IsStarving(const SkirmishAIAlliedPlayerFacts *fact)
	{
		return IsLivePlayer(fact) &&
			ClampMetric(fact->economyHealth) <= 25 &&
			(fact->missingIncome || fact->missingProduction) && fact->recoverable;
	}

	inline Bool IsDonationRecipient(const SkirmishAIAlliedPlayerFacts *fact,
		Int starvationStreak)
	{
		return IsStarving(fact) && !fact->aidBlocked && starvationStreak >= 2;
	}

	inline Bool HasBetterTargetTieBreak(Int enemyIndex, ObjectID targetID,
		Int bestEnemyIndex, ObjectID bestTargetID)
	{
		return enemyIndex < bestEnemyIndex ||
			(enemyIndex == bestEnemyIndex && targetID < bestTargetID);
	}
}

inline void EvaluateSkirmishAIAlliedCoordination(
	const SkirmishAIAlliedPlayerFacts *facts, Int factCount,
	const Int *previousStarvationStreaks, SkirmishAIAlliedDecision *decision)
{
	using namespace SkirmishAIAlliedCoordinationPolicy;

	if (decision == 0)
		return;

	for (Int playerIndex = 0; playerIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++playerIndex)
	{
		decision->starvationStreaks[playerIndex] = 0;
		decision->assaultLeaderIndices[playerIndex] = -1;
		decision->assaultEnemyIndices[playerIndex] = -1;
		decision->supportRecipientIndices[playerIndex] = -1;
		decision->supportBudgets[playerIndex] = 0;
		decision->assaultTargetIDs[playerIndex] = INVALID_ID;
	}
	decision->donorIndex = -1;
	decision->recipientIndex = -1;
	decision->donationAmount = 0;

	if (!facts || factCount <= 0 ||
		factCount > SKIRMISH_AI_ALLIED_MAX_PLAYERS)
		return;

	const SkirmishAIAlliedPlayerFacts *players[16];
	Bool duplicatePlayer[16];
	for (Int playerIndex = 0; playerIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++playerIndex)
	{
		players[playerIndex] = 0;
		duplicatePlayer[playerIndex] = FALSE;
	}

	// Reject every duplicate index so a reordered snapshot cannot change the policy.
	for (Int factIndex = 0; factIndex < factCount; ++factIndex)
	{
		const Int playerIndex = facts[factIndex].playerIndex;
		if (playerIndex < 0 || playerIndex >= SKIRMISH_AI_ALLIED_MAX_PLAYERS)
			continue;

		if (players[playerIndex] != 0 || duplicatePlayer[playerIndex])
		{
			players[playerIndex] = 0;
			duplicatePlayer[playerIndex] = TRUE;
		}
		else
			players[playerIndex] = &facts[factIndex];
	}

	for (Int playerIndex = 0; playerIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++playerIndex)
	{
		if (duplicatePlayer[playerIndex])
			players[playerIndex] = 0;

		const SkirmishAIAlliedPlayerFacts *fact = players[playerIndex];
		if (!IsLivePlayer(fact))
			continue;

		if (IsStarving(fact))
		{
			const Int previous = previousStarvationStreaks
				? ClampStarvationStreak(previousStarvationStreaks[playerIndex]) : 0;
			decision->starvationStreaks[playerIndex] = previous < 2 ? previous + 1 : 2;
		}
	}

	// Select exactly one automatic aid pair: most severe economy first, then
	// recipient index, donor surplus descending, and donor index.
	Int bestRecipient = -1;
	Int bestDonor = -1;
	Int bestSeverity = 101;
	Int bestSurplus = -1;
	Int bestAmount = 0;
	for (Int recipientIndex = 0; recipientIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS;
		++recipientIndex)
	{
		const SkirmishAIAlliedPlayerFacts *recipient = players[recipientIndex];
		if (!IsDonationRecipient(recipient, decision->starvationStreaks[recipientIndex]))
			continue;

		const Int severity = ClampMetric(recipient->economyHealth);
		for (Int donorIndex = 0; donorIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++donorIndex)
		{
			const SkirmishAIAlliedPlayerFacts *donor = players[donorIndex];
			if (donorIndex == recipientIndex || !IsDonationDonor(donor) ||
				!AreMutuallyAllied(players, donorIndex, recipientIndex))
				continue;

			const Int amount = DonationAmount(donor);
			if (amount < 500)
				continue;

			const Int surplus = DonationSurplus(donor);
			if (bestRecipient < 0 || severity < bestSeverity ||
				(severity == bestSeverity && recipientIndex < bestRecipient) ||
				(severity == bestSeverity && recipientIndex == bestRecipient &&
				(surplus > bestSurplus ||
				(surplus == bestSurplus && donorIndex < bestDonor))))
			{
				bestRecipient = recipientIndex;
				bestDonor = donorIndex;
				bestSeverity = severity;
				bestSurplus = surplus;
				bestAmount = amount;
			}
		}
	}
	if (bestRecipient >= 0)
	{
		decision->donorIndex = bestDonor;
		decision->recipientIndex = bestRecipient;
		decision->donationAmount = bestAmount;
	}

	// Each lowest unassigned eligible AI leads a cohort of direct mutual allies.
	// Cohorts are non-overlapping and never follow transitive alliance edges.
	Bool assigned[16];
	for (Int playerIndex = 0; playerIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++playerIndex)
		assigned[playerIndex] = FALSE;

	for (Int leaderIndex = 0; leaderIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++leaderIndex)
	{
		if (assigned[leaderIndex] || !IsAssaultEligible(players, leaderIndex))
			continue;

		Bool cohort[16];
		Int cohortCount = 1;
		for (Int memberIndex = 0; memberIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++memberIndex)
			cohort[memberIndex] = FALSE;
		cohort[leaderIndex] = TRUE;
		assigned[leaderIndex] = TRUE;

		for (Int memberIndex = leaderIndex + 1;
			memberIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS; ++memberIndex)
		{
			if (!assigned[memberIndex] && IsAssaultEligible(players, memberIndex) &&
				AreMutuallyAllied(players, leaderIndex, memberIndex))
			{
				cohort[memberIndex] = TRUE;
				assigned[memberIndex] = TRUE;
				++cohortCount;
			}
		}
		if (cohortCount < 2)
			continue;

		Int bestEnemyIndex = -1;
		Int bestTargetScore = 0;
		ObjectID bestTargetID = INVALID_ID;
		for (Int memberIndex = 0; memberIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS;
			++memberIndex)
		{
			if (!cohort[memberIndex])
				continue;

			const SkirmishAIAlliedPlayerFacts *member = players[memberIndex];
			const Int enemyIndex = member->targetEnemyIndex;
			const ObjectID targetID = member->targetObjectID;
			if (targetID == INVALID_ID ||
				!IsHostileTo(players, leaderIndex, enemyIndex))
				continue;

			if (bestEnemyIndex < 0 || member->targetScore > bestTargetScore ||
				(member->targetScore == bestTargetScore &&
				HasBetterTargetTieBreak(enemyIndex, targetID,
					bestEnemyIndex, bestTargetID)))
			{
				bestEnemyIndex = enemyIndex;
				bestTargetScore = member->targetScore;
				bestTargetID = targetID;
			}
		}
		if (bestEnemyIndex < 0)
			continue;

		Int participantCount = 0;
		for (Int memberIndex = 0; memberIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS;
			++memberIndex)
		{
			if (cohort[memberIndex] &&
				AreMutuallyAllied(players, leaderIndex, memberIndex) &&
				IsHostileTo(players, memberIndex, bestEnemyIndex))
				++participantCount;
		}
		if (participantCount < 2)
			continue;

		for (Int memberIndex = 0; memberIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS;
			++memberIndex)
		{
			if (!cohort[memberIndex] ||
				!AreMutuallyAllied(players, leaderIndex, memberIndex) ||
				!IsHostileTo(players, memberIndex, bestEnemyIndex))
				continue;

			decision->assaultLeaderIndices[memberIndex] = leaderIndex;
			decision->assaultEnemyIndices[memberIndex] = bestEnemyIndex;
			decision->assaultTargetIDs[memberIndex] = bestTargetID;
		}
	}

	// A responder selects one directly allied distressed player, preferring
	// greater distress and then the lower actual player index.
	for (Int responderIndex = 0; responderIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS;
		++responderIndex)
	{
		const SkirmishAIAlliedPlayerFacts *responder = players[responderIndex];
		if (!IsLivePlayer(responder) || !responder->isAI ||
			ClampMetric(responder->immediateThreat) >= 50 ||
			responder->supportAvailableValue <= 0)
			continue;

		Int bestRecipientIndex = -1;
		Int bestDistress = -1;
		for (Int recipientIndex = 0; recipientIndex < SKIRMISH_AI_ALLIED_MAX_PLAYERS;
			++recipientIndex)
		{
			const SkirmishAIAlliedPlayerFacts *recipient = players[recipientIndex];
			if (recipientIndex == responderIndex || !IsLivePlayer(recipient) ||
				ClampMetric(recipient->distress) < 70 ||
				!AreMutuallyAllied(players, responderIndex, recipientIndex))
				continue;

			const Int distress = ClampMetric(recipient->distress);
			if (bestRecipientIndex < 0 || distress > bestDistress ||
				(distress == bestDistress && recipientIndex < bestRecipientIndex))
			{
				bestRecipientIndex = recipientIndex;
				bestDistress = distress;
			}
		}

		if (bestRecipientIndex >= 0)
		{
			decision->supportRecipientIndices[responderIndex] = bestRecipientIndex;
			decision->supportBudgets[responderIndex] = responder->supportAvailableValue;
		}
	}
}
