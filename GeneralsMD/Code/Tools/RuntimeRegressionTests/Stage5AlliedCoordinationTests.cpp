/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/
#include "Common/AlliedMoneyTransfer.h"
#include "GameLogic/SkirmishAIAlliedCoordination.h"

#include <stdio.h>
#include <string.h>

namespace
{
int s_failures = 0;

#define STAGE5_CHECK(expression) \
	do { \
		if (!(expression)) { \
			++s_failures; \
			fprintf(stderr, "Stage 5 allied coordination check failed at line %d: %s\n", \
				__LINE__, #expression); \
		} \
	} while (0)

static void InitializeFacts(SkirmishAIAlliedPlayerFacts facts[16])
{
	memset(facts, 0, sizeof(SkirmishAIAlliedPlayerFacts) * 16);
	for (Int i = 0; i < 16; ++i)
	{
		facts[i].playerIndex = i;
		facts[i].starvationCashLimit = 2500;
		facts[i].targetEnemyIndex = -1;
		facts[i].targetObjectID = INVALID_ID;
		facts[i].mode = SKIRMISH_STRATEGY_BALANCED;
	}
}

static void MakeLive(SkirmishAIAlliedPlayerFacts *fact, Int index, Bool isAI)
{
	fact->valid = TRUE;
	fact->alive = TRUE;
	fact->isAI = isAI;
	fact->playerIndex = index;
	fact->economyHealth = 50;
	fact->baseIntegrity = 65;
	fact->armyReadiness = 60;
	fact->immediateThreat = 0;
	fact->hasReadyForce = TRUE;
	fact->mode = SKIRMISH_STRATEGY_BALANCED;
	fact->targetEnemyIndex = -1;
	fact->targetObjectID = INVALID_ID;
}

static void MakeStarving(SkirmishAIAlliedPlayerFacts *fact)
{
	fact->economyHealth = 25;
	fact->missingProduction = TRUE;
	fact->recoverable = TRUE;
}

static void AddMutualAlliance(SkirmishAIAlliedPlayerFacts facts[16], Int first, Int second)
{
	facts[first].alliedMask |= ((UnsignedInt)1U << second);
	facts[second].alliedMask |= ((UnsignedInt)1U << first);
}

static void CopyStreaks(const SkirmishAIAlliedDecision& decision, Int streaks[16])
{
	for (Int i = 0; i < 16; ++i)
		streaks[i] = decision.starvationStreaks[i];
}

static Bool SameDecision(const SkirmishAIAlliedDecision& left,
	const SkirmishAIAlliedDecision& right)
{
	if (left.donorIndex != right.donorIndex ||
		left.recipientIndex != right.recipientIndex ||
		left.donationAmount != right.donationAmount)
		return FALSE;
	for (Int i = 0; i < 16; ++i)
	{
		if (left.starvationStreaks[i] != right.starvationStreaks[i] ||
			left.assaultLeaderIndices[i] != right.assaultLeaderIndices[i] ||
			left.assaultEnemyIndices[i] != right.assaultEnemyIndices[i] ||
			left.assaultTargetIDs[i] != right.assaultTargetIDs[i] ||
			left.supportRecipientIndices[i] != right.supportRecipientIndices[i] ||
			left.supportBudgets[i] != right.supportBudgets[i])
			return FALSE;
	}
	return TRUE;
}

static void TestPlayerZeroAndStarvationLifecycle()
{
	SkirmishAIAlliedPlayerFacts facts[16];
	InitializeFacts(facts);
	MakeLive(&facts[0], 0, FALSE);
	MakeStarving(&facts[0]);
	MakeLive(&facts[7], 7, TRUE);
	facts[7].economyHealth = 75;
	facts[7].baseIntegrity = 80;
	facts[7].immediateThreat = 40;
	facts[7].cash = 2500;
	facts[7].protectedReserve = 1000;
	AddMutualAlliance(facts, 0, 7);

	Int previous[16] = { 0 };
	SkirmishAIAlliedDecision decision;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 1);
	STAGE5_CHECK(decision.donorIndex == -1 && decision.recipientIndex == -1);

	CopyStreaks(decision, previous);
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 2);
	STAGE5_CHECK(decision.donorIndex == 7 && decision.recipientIndex == 0);
	STAGE5_CHECK(decision.donationAmount == 500);

	// A recipient cooldown suppresses aid without erasing its consecutive streak.
	facts[0].aidBlocked = TRUE;
	previous[0] = 2;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 2);
	STAGE5_CHECK(decision.donorIndex == -1 && decision.donationAmount == 0);

	facts[0].aidBlocked = FALSE;
	facts[0].recoverable = FALSE;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);
	STAGE5_CHECK(decision.recipientIndex == -1);

	facts[0].recoverable = TRUE;
	facts[0].missingProduction = FALSE;
	facts[0].missingIncome = TRUE;
	previous[0] = 0;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 1);

	facts[0].economyHealth = 26;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);

	facts[0].economyHealth = 25;
	facts[0].missingIncome = FALSE;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);

	// Duplicate actual player indices remain rejected, regardless of fact order.
	SkirmishAIAlliedPlayerFacts duplicates[2];
	InitializeFacts(facts);
	MakeLive(&facts[0], 0, FALSE);
	MakeStarving(&facts[0]);
	duplicates[0] =facts[0];
	duplicates[1] =facts[0];
	EvaluateSkirmishAIAlliedCoordination(duplicates, 2, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);
	STAGE5_CHECK(decision.recipientIndex == -1);

	SkirmishAIAlliedPlayerFacts tooMany[17];
	for (Int i = 0; i < 17; ++i)
	{
		memset(&tooMany[i], 0, sizeof(tooMany[i]));
		tooMany[i].playerIndex = i;
	}
	EvaluateSkirmishAIAlliedCoordination(tooMany, 17, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1 && decision.recipientIndex == -1);
}

static void TestHumanStarvationCashBoundary()
{
	SkirmishAIAlliedPlayerFacts facts[16];
	InitializeFacts(facts);
	MakeLive(&facts[0], 0, FALSE);
	MakeStarving(&facts[0]);
	Int previous[16] = { 0 };
	SkirmishAIAlliedDecision decision;

	facts[0].cash = 2500; // Configured threshold is inclusive.
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 1);
	previous[0] = 1;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 2);

	facts[0].cash = 2501;
	previous[0] = 2;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);
	facts[0].cash = 100000;
	facts[0].missingIncome = FALSE;
	facts[0].missingProduction = FALSE;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.recipientIndex == -1);

	facts[0].cash = 1000;
	facts[0].starvationCashLimit = 0;
	previous[0] = 0;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);
	facts[0].starvationCashLimit = -1;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);

	facts[0].starvationCashLimit = 2500;
	facts[0].missingProduction = FALSE;
	facts[0].missingIncome = FALSE;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);
	facts[0].missingIncome = TRUE;
	facts[0].recoverable = FALSE;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.starvationStreaks[0] == 0);
}

static void TestDonationBoundariesAndDeterministicPair()
{
	SkirmishAIAlliedPlayerFacts facts[16];
	InitializeFacts(facts);
	MakeLive(&facts[0], 0, FALSE);
	MakeStarving(&facts[0]);
	MakeLive(&facts[1], 1, TRUE);
	facts[1].economyHealth = 75;
	facts[1].baseIntegrity = 80;
	facts[1].immediateThreat = 40;
	facts[1].cash = 2500;
	AddMutualAlliance(facts, 0, 1);
	Int previous[16] = { 0 };
	previous[0] = 1;
	SkirmishAIAlliedDecision decision;

	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == 1 && decision.recipientIndex == 0);
	STAGE5_CHECK(decision.donationAmount == 500);

	facts[1].economyHealth = 74;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1);
	facts[1].economyHealth = 75;
	facts[1].baseIntegrity = 79;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1);
	facts[1].baseIntegrity = 80;
	facts[1].immediateThreat = 41;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1);
	facts[1].immediateThreat = 40;
	facts[1].donationBlocked = TRUE;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1);
	facts[1].donationBlocked = FALSE;
	facts[1].cash = 2499;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1);
	facts[1].cash = 2500;
	facts[1].protectedReserve = 2001;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == -1);
	facts[1].protectedReserve = 2000;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == 1 && decision.donationAmount == 500);

	// Only one pair is selected: lowest recipient economy wins, then the donor
	// with greatest surplus, with actual player indices resolving exact ties.
	MakeLive(&facts[2], 2, TRUE);
	facts[2].economyHealth = 75;
	facts[2].baseIntegrity = 80;
	facts[2].cash = 6000;
	facts[2].protectedReserve = 1000;
	AddMutualAlliance(facts, 0, 2);
	AddMutualAlliance(facts, 2, 4);
	MakeLive(&facts[3], 3, TRUE);
	facts[3].economyHealth = 75;
	facts[3].baseIntegrity = 80;
	facts[3].cash = 6000;
	facts[3].protectedReserve = 1000;
	AddMutualAlliance(facts, 0, 3);
	AddMutualAlliance(facts, 3, 4);
	MakeLive(&facts[4], 4, FALSE);
	MakeStarving(&facts[4]);
	facts[4].economyHealth = 10;
	previous[0] = 1;
	previous[4] = 1;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.recipientIndex == 4);
	STAGE5_CHECK(decision.donorIndex == 2);
	facts[4].economyHealth = 25;
	previous[0] = 1;
	previous[4] = 1;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.recipientIndex == 0);
	STAGE5_CHECK(decision.donorIndex == 2);
	STAGE5_CHECK(decision.donationAmount == 1200);

	// Without a mutual edge, otherwise valid donor and recipient cannot pair.
	facts[0].alliedMask &= ~(((UnsignedInt)1U) << 2);
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.donorIndex == 3);
}

static void TestAssaultCohortDeterminismAndDeclines()
{
	SkirmishAIAlliedPlayerFacts facts[16];
	InitializeFacts(facts);
	for (Int i = 0; i <= 4; ++i)
		MakeLive(&facts[i], i, i < 3);
	facts[0].immediateThreat = 59;
	facts[1].economyHealth = 50;
	facts[1].baseIntegrity = 65;
	facts[1].armyReadiness = 60;
	facts[1].immediateThreat = 59;
	facts[2].immediateThreat = 0;
	facts[0].targetEnemyIndex = 3;
	facts[0].targetObjectID = (ObjectID)40;
	facts[0].targetScore = 100;
	facts[1].targetEnemyIndex = 4;
	facts[1].targetObjectID = (ObjectID)35;
	facts[1].targetScore = 101;
	facts[1].enemyMask = (((UnsignedInt)1U << 3) | ((UnsignedInt)1U << 4));
	facts[2].targetEnemyIndex = 3;
	facts[2].targetObjectID = (ObjectID)12;
	facts[2].targetScore = 1000;
	facts[2].enemyMask = ((UnsignedInt)1U << 3);
	facts[0].enemyMask = (((UnsignedInt)1U << 3) | ((UnsignedInt)1U << 4));
	facts[0].alliedMask |= ((UnsignedInt)1U << 4); // One-sided edge suppresses this target.
	AddMutualAlliance(facts, 0, 1);
	AddMutualAlliance(facts, 1, 2); // Player 2 is allied only through player 1.

	Int previous[16] = { 0 };
	SkirmishAIAlliedDecision decision;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.assaultLeaderIndices[0] == 0);
	STAGE5_CHECK(decision.assaultLeaderIndices[1] == 0);
	STAGE5_CHECK(decision.assaultEnemyIndices[0] == 3);
	STAGE5_CHECK(decision.assaultEnemyIndices[1] == 3);
	STAGE5_CHECK(decision.assaultTargetIDs[0] == (ObjectID)40);
	STAGE5_CHECK(decision.assaultLeaderIndices[2] == -1);
	// A neutral relation invalidates an otherwise-ready cohort participant.
	facts[1].enemyMask &= ~(((UnsignedInt)1U) << 4);
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.assaultLeaderIndices[0] == -1);
	STAGE5_CHECK(decision.assaultLeaderIndices[2] == -1);
	facts[1].enemyMask |= ((UnsignedInt)1U << 4);
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);

	// The one-sided edge blocks player 1's higher-scoring target for leader 0.
	STAGE5_CHECK(decision.assaultEnemyIndices[0] == 3);
	STAGE5_CHECK(decision.assaultTargetIDs[0] == (ObjectID)40);
	facts[0].alliedMask &= ~(((UnsignedInt)1U) << 4);
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.assaultEnemyIndices[0] == 4);
	STAGE5_CHECK(decision.assaultTargetIDs[0] == (ObjectID)35);
	facts[1].targetEnemyIndex = 3;
	facts[1].targetScore = 100;

	// Equal scores choose the lower object id for the same enemy.
	facts[0].targetEnemyIndex = 3;
	facts[0].targetObjectID = (ObjectID)40;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.assaultTargetIDs[0] == (ObjectID)35);
	// A strictly higher score takes precedence over the object-id tie-break.
	facts[0].targetScore = 101;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.assaultTargetIDs[0] == (ObjectID)40);

	// Input enumeration order cannot change the indexed decision.
	SkirmishAIAlliedPlayerFacts reversed[16];
	for (Int i = 0; i < 16; ++i)
		reversed[i] =facts[15 - i];
	SkirmishAIAlliedDecision reorderedDecision;
	EvaluateSkirmishAIAlliedCoordination(reversed, 16, previous, &reorderedDecision);
	STAGE5_CHECK(SameDecision(decision, reorderedDecision));

	// Each critical participant condition declines the entire pair.
	for (Int decline = 0; decline < 6; ++decline)
	{
		facts[1].economyHealth = 50;
		facts[1].baseIntegrity = 65;
		facts[1].armyReadiness = 60;
		facts[1].immediateThreat = 59;
		facts[1].hasReadyForce = TRUE;
		facts[1].mode = SKIRMISH_STRATEGY_BALANCED;
		if (decline == 0)facts[1].immediateThreat = 60;
		if (decline == 1)facts[1].economyHealth = 49;
		if (decline == 2)facts[1].baseIntegrity = 64;
		if (decline == 3)facts[1].armyReadiness = 59;
		if (decline == 4)facts[1].hasReadyForce = FALSE;
		if (decline == 5)facts[1].mode = SKIRMISH_STRATEGY_FORTIFY;
		EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
		STAGE5_CHECK(decision.assaultLeaderIndices[0] == -1);
		STAGE5_CHECK(decision.assaultLeaderIndices[1] == -1);
	}
}

static void TestSupportSelectionAndMoneyRules()
{
	SkirmishAIAlliedPlayerFacts facts[16];
	InitializeFacts(facts);
	MakeLive(&facts[0], 0, TRUE);
	facts[0].immediateThreat = 49;
	facts[0].supportAvailableValue = 200;
	MakeLive(&facts[1], 1, FALSE);
	facts[1].distress = 70;
	MakeLive(&facts[2], 2, FALSE);
	facts[2].distress = 100; // Not mutually allied.
	MakeLive(&facts[3], 3, FALSE);
	facts[3].distress = 80;
	MakeLive(&facts[4], 4, FALSE);
	facts[4].distress = 80;
	AddMutualAlliance(facts, 0, 1);
	AddMutualAlliance(facts, 0, 3);
	AddMutualAlliance(facts, 0, 4);
	facts[0].alliedMask |= ((UnsignedInt)1U << 2); // One-sided edge is insufficient.

	Int previous[16] = { 0 };
	SkirmishAIAlliedDecision decision;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.supportRecipientIndices[0] == 3);
	STAGE5_CHECK(decision.supportBudgets[0] == 200);
	facts[0].immediateThreat = 50;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.supportRecipientIndices[0] == -1);
	facts[0].immediateThreat = 49;
	facts[0].supportAvailableValue = 0;
	EvaluateSkirmishAIAlliedCoordination(facts, 16, previous, &decision);
	STAGE5_CHECK(decision.supportRecipientIndices[0] == -1);

	STAGE5_CHECK(AlliedMoneyTransfer::IsHumanAmountValid(100));
	STAGE5_CHECK(AlliedMoneyTransfer::IsHumanAmountValid(10000));
	STAGE5_CHECK(!AlliedMoneyTransfer::IsHumanAmountValid(99));
	STAGE5_CHECK(!AlliedMoneyTransfer::IsHumanAmountValid(101));
	STAGE5_CHECK(!AlliedMoneyTransfer::IsHumanAmountValid(10100));
	STAGE5_CHECK(!AlliedMoneyTransfer::IsHumanAmountValid(0));
	STAGE5_CHECK(!AlliedMoneyTransfer::IsHumanAmountValid(-100));

	STAGE5_CHECK(AlliedMoneyTransfer::CanTransfer(101, 1001, 500,
		TRUE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(0, 1001, 500,
		TRUE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(-1, 1001, 500,
		TRUE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(1002, 1001, 500,
		TRUE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(1, 1001, 500,
		FALSE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(1, 1001, 500,
		TRUE, FALSE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(1, 1001, 500,
		TRUE, TRUE, FALSE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(1, 1001, 500,
		TRUE, TRUE, TRUE, FALSE));
	STAGE5_CHECK(AlliedMoneyTransfer::CanTransfer(1, UINT_MAX, UINT_MAX - 1U,
		TRUE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(2, UINT_MAX, UINT_MAX - 1U,
		TRUE, TRUE, TRUE, TRUE));
	STAGE5_CHECK(!AlliedMoneyTransfer::CanTransfer(1, UINT_MAX, UINT_MAX,
		TRUE, TRUE, TRUE, TRUE));
	if (AlliedMoneyTransfer::CanTransfer(101, 1001, 500,
		TRUE, TRUE, TRUE, TRUE))
	{
		const UnsignedInt donorAfter = 1001U - 101U;
		const UnsignedInt recipientAfter = 500U + 101U;
		STAGE5_CHECK(donorAfter == 900U && recipientAfter == 601U);
		STAGE5_CHECK(donorAfter + recipientAfter == 1501U);
	}
}
}

int RunStage5AlliedCoordinationTests()
{
	s_failures = 0;
	TestPlayerZeroAndStarvationLifecycle();
	TestHumanStarvationCashBoundary();
	TestDonationBoundariesAndDeterministicPair();
	TestAssaultCohortDeterminismAndDeclines();
	TestSupportSelectionAndMoneyRules();
	if (s_failures == 0)
		printf("All Stage 5 allied coordination policy tests passed.\n");
	else
		printf("%d Stage 5 allied coordination policy test(s) failed.\n", s_failures);
	return s_failures == 0 ? 0 : 1;
}
