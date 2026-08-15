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

enum SkirmishAIDecisionDifficulty
{
	SKIRMISH_AI_DIFFICULTY_EASY = 0,
	SKIRMISH_AI_DIFFICULTY_NORMAL,
	SKIRMISH_AI_DIFFICULTY_HARD
};

enum SkirmishAIRouteClass
{
	SKIRMISH_AI_ROUTE_UNKNOWN = 0,
	SKIRMISH_AI_ROUTE_GROUND_REACHABLE,
	SKIRMISH_AI_ROUTE_MIXED_UNREACHABLE,
	SKIRMISH_AI_ROUTE_GROUND_UNREACHABLE
};

struct SkirmishAICostRange
{
	int minimumCost;
	int plannedCost;
};

struct SkirmishAITeamScoreInput
{
	int configuredPriority;
	int counterFitScore;
	int resources;
	int minimumCost;
	int plannedCost;
	int reserve;
	int factoryWaitFrames;
	int logicFramesPerSecond;
	SkirmishAIRouteClass routeClass;
	int recentLossCount;
	int recentPathFailureCount;
	SkirmishAIDecisionDifficulty difficulty;
};

struct SkirmishAITeamScoreResult
{
	int counterFitScore;
	int economyScore;
	int factoryWaitScore;
	int routeScore;
	int lossScore;
	int pathFailureScore;
	int rawContextScore;
	__int64 finalScore;
};

inline int ClampSkirmishAIDecisionValue(int value, int minimum, int maximum)
{
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

inline int AddSkirmishAICostValue(int total, int unitCost, int count)
{
	if (unitCost <= 0 || count <= 0)
		return total;
	double result = (double)total + (double)unitCost * (double)count;
	if (result > 2147483647.0)
		return 2147483647;
	return (int)result;
}

inline SkirmishAICostRange MakeSkirmishAICostRange()
{
	SkirmishAICostRange result;
	result.minimumCost = 0;
	result.plannedCost = 0;
	return result;
}

inline SkirmishAICostRange AddSkirmishAIUnitCost(
	SkirmishAICostRange range, int unitCost, int minimumCount, int plannedCount)
{
	if (minimumCount < 0)
		minimumCount = 0;
	if (plannedCount < minimumCount)
		plannedCount = minimumCount;
	range.minimumCost = AddSkirmishAICostValue(range.minimumCost, unitCost, minimumCount);
	range.plannedCost = AddSkirmishAICostValue(range.plannedCost, unitCost, plannedCount);
	return range;
}

inline int GetSkirmishAIPriorityMagnitude(int priority)
{
	if (priority >= 0)
		return priority;
	if (priority == (-2147483647 - 1))
		return 2147483647;
	return -priority;
}

inline int GetSkirmishAIPriorityBandWidth(int highestPriority, SkirmishAIDecisionDifficulty difficulty)
{
	int magnitude = GetSkirmishAIPriorityMagnitude(highestPriority);
	if (difficulty == SKIRMISH_AI_DIFFICULTY_EASY)
		return 0;
	if (difficulty == SKIRMISH_AI_DIFFICULTY_NORMAL)
	{
		int width = magnitude / 20;
		return width > 1 ? width : 1;
	}
	int width = magnitude / 10;
	return width > 2 ? width : 2;
}

inline bool IsSkirmishAIPriorityAdmitted(
	int candidatePriority, int highestPriority, SkirmishAIDecisionDifficulty difficulty)
{
	if (candidatePriority > highestPriority)
		return false;
	return (double)highestPriority - (double)candidatePriority <=
		(double)GetSkirmishAIPriorityBandWidth(highestPriority, difficulty);
}

inline bool ShouldReplaceSkirmishAIHighestPriority(
	bool hasCandidate, int candidatePriority, int highestPriority)
{
	return !hasCandidate || candidatePriority > highestPriority;
}

inline int GetSkirmishAIReserve(int resourcesPoor, int rebuildCost)
{
	if (resourcesPoor < 0)
		resourcesPoor = 0;
	if (rebuildCost < 0)
		rebuildCost = 0;
	return resourcesPoor > rebuildCost ? resourcesPoor : rebuildCost;
}

inline bool IsSkirmishAIAffordable(int resources, int minimumCost, int reserve)
{
	if (minimumCost < 0)
		minimumCost = 0;
	if (reserve < 0)
		reserve = 0;
	return (double)resources >= (double)minimumCost + (double)reserve;
}

inline bool ShouldRetrySkirmishAIReserve(
	bool criticalRebuildCanStart, bool rebuildReserveApplied, bool anyCandidatePreservesReserve)
{
	return rebuildReserveApplied && !criticalRebuildCanStart && !anyCandidatePreservesReserve;
}

inline bool IsSkirmishAICriticalRebuildStartable(
	bool hasDozer, bool canMake, bool hasAI, bool rebuildReady, bool clearOfEnemies)
{
	return hasDozer && canMake && hasAI && rebuildReady && clearOfEnemies;
}

inline bool ShouldSkirmishAIConsiderRebuild(
	bool isAutomatic, bool isPriority, bool isPower, bool isUnderpowered)
{
	return isAutomatic || isPriority || (isPower && isUnderpowered);
}

inline int AddSkirmishAIFrameValue(int total, int frames)
{
	if (frames <= 0)
		return total;
	if (total > 2147483647 - frames)
		return 2147483647;
	return total + frames;
}

inline int GetSkirmishAIProductionEntryWaitFrames(
	int buildFrames, float percentComplete, bool firstEntry, int /*productionQuantityRemaining*/ = 1)
{
	if (buildFrames <= 0)
		return 0;
	if (!firstEntry)
		return buildFrames;
	if (percentComplete < 0.0f)
		percentComplete = 0.0f;
	if (percentComplete > 100.0f)
		percentComplete = 100.0f;
	return (int)((double)buildFrames * (100.0 - (double)percentComplete) / 100.0);
}

inline int GetSkirmishAIUnitsRemainingAfterProductionEntry(
	int unitsRemaining, int productionQuantity)
{
	if (unitsRemaining <= 0)
		return 0;
	if (productionQuantity < 1)
		productionQuantity = 1;
	unitsRemaining -= productionQuantity;
	return unitsRemaining > 0 ? unitsRemaining : 0;
}

inline int GetSkirmishAILeastLoadedFactoryIndex(
	const int *projectedLoads, const int *compatibleFactories, int factoryCount)
{
	int selected = -1;
	for (int i = 0; i < factoryCount; ++i)
	{
		if (!compatibleFactories[i])
			continue;
		if (selected < 0 || projectedLoads[i] < projectedLoads[selected])
			selected = i;
	}
	return selected;
}

inline bool IsSkirmishAIGroundRouteTarget(
	bool isStructure, bool isAircraft, bool isVehicle, bool isInfantry)
{
	return !isStructure && !isAircraft && (isVehicle || isInfantry);
}

inline int GetSkirmishAICounterFitScore(
	int enemyAircraftValue,
	int enemyVehicleValue,
	int enemyInfantryValue,
	int candidatePlannedValue,
	int candidateAntiAircraftValue,
	int candidateAntiVehicleValue,
	int candidateAntiInfantryValue)
{
	if (enemyAircraftValue < 0)
		enemyAircraftValue = 0;
	if (enemyVehicleValue < 0)
		enemyVehicleValue = 0;
	if (enemyInfantryValue < 0)
		enemyInfantryValue = 0;
	double enemyTotal = (double)enemyAircraftValue + enemyVehicleValue + enemyInfantryValue;
	if (enemyTotal <= 0.0 || candidatePlannedValue <= 0)
		return 0;
	double matchedValue = 0.0;
	double coverage = (double)candidateAntiAircraftValue / (double)candidatePlannedValue;
	if (coverage > 1.0)
		coverage = 1.0;
	if (coverage > 0.0)
		matchedValue += (double)enemyAircraftValue * coverage;
	coverage = (double)candidateAntiVehicleValue / (double)candidatePlannedValue;
	if (coverage > 1.0)
		coverage = 1.0;
	if (coverage > 0.0)
		matchedValue += (double)enemyVehicleValue * coverage;
	coverage = (double)candidateAntiInfantryValue / (double)candidatePlannedValue;
	if (coverage > 1.0)
		coverage = 1.0;
	if (coverage > 0.0)
		matchedValue += (double)enemyInfantryValue * coverage;
	return ClampSkirmishAIDecisionValue((int)(300.0 * matchedValue / enemyTotal + 0.5), 0, 300);
}

inline int GetSkirmishAIEconomyScore(
	int resources, int minimumCost, int plannedCost, int reserve)
{
	if (IsSkirmishAIAffordable(resources, plannedCost, reserve))
		return 100;
	if (IsSkirmishAIAffordable(resources, minimumCost, reserve))
		return -150;
	return -350;
}

inline int GetSkirmishAIFactoryWaitScore(int waitFrames, int logicFramesPerSecond)
{
	if (waitFrames <= 0 || logicFramesPerSecond <= 0)
		return 0;
	int maximumFrames = 30 * logicFramesPerSecond;
	if (waitFrames >= maximumFrames)
		return -250;
	return -(250 * waitFrames / maximumFrames);
}

inline int GetSkirmishAIRouteScore(SkirmishAIRouteClass routeClass)
{
	switch (routeClass)
	{
		case SKIRMISH_AI_ROUTE_GROUND_REACHABLE:
			return 50;
		case SKIRMISH_AI_ROUTE_MIXED_UNREACHABLE:
			return -125;
		case SKIRMISH_AI_ROUTE_GROUND_UNREACHABLE:
			return -250;
		default:
			return 0;
	}
}

inline int GetSkirmishAIFeedbackScore(int count, int scorePerEvent)
{
	return ClampSkirmishAIDecisionValue(count, 0, 3) * scorePerEvent;
}

inline int GetSkirmishAIContextInfluencePercent(SkirmishAIDecisionDifficulty difficulty)
{
	if (difficulty == SKIRMISH_AI_DIFFICULTY_EASY)
		return 25;
	if (difficulty == SKIRMISH_AI_DIFFICULTY_NORMAL)
		return 50;
	return 100;
}

inline __int64 GetSkirmishAIFinalScore(int configuredPriority, int contextScore, int contextPercent)
{
	return (__int64)configuredPriority * 1000 +
		(__int64)contextScore * contextPercent / 100;
}

inline SkirmishAITeamScoreResult ScoreSkirmishAITeam(const SkirmishAITeamScoreInput &input)
{
	SkirmishAITeamScoreResult result;
	result.counterFitScore = ClampSkirmishAIDecisionValue(input.counterFitScore, 0, 300);
	result.economyScore = GetSkirmishAIEconomyScore(
		input.resources, input.minimumCost, input.plannedCost, input.reserve);
	result.factoryWaitScore = GetSkirmishAIFactoryWaitScore(
		input.factoryWaitFrames, input.logicFramesPerSecond);
	result.routeScore = GetSkirmishAIRouteScore(input.routeClass);
	result.lossScore = GetSkirmishAIFeedbackScore(input.recentLossCount, -75);
	result.pathFailureScore = GetSkirmishAIFeedbackScore(input.recentPathFailureCount, -100);
	result.rawContextScore = ClampSkirmishAIDecisionValue(
		result.counterFitScore + result.economyScore + result.factoryWaitScore + result.routeScore +
		result.lossScore + result.pathFailureScore,
		-1000,
		1000);
	result.finalScore = GetSkirmishAIFinalScore(
		input.configuredPriority,
		result.rawContextScore,
		GetSkirmishAIContextInfluencePercent(input.difficulty));
	return result;
}

inline bool IsSkirmishAITeamScoreTie(__int64 firstScore, __int64 secondScore)
{
	return firstScore == secondScore;
}

inline int GetSkirmishAITieSelectionIndex(int tieCount, int seededRandomIndex)
{
	if (tieCount <= 1)
		return 0;
	return ClampSkirmishAIDecisionValue(seededRandomIndex, 0, tieCount - 1);
}

template <typename PlanType>
inline PlanType *GetSkirmishAutomaticConstructionPlan(PlanType *currentPlan, PlanType * /*previousPlan*/)
{
	return currentPlan;
}

inline int GetSupplyDefenseMemoryFrames(int logicFramesPerSecond)
{
	return 10 * logicFramesPerSecond;
}

inline bool ShouldPreferSkirmishRetaliation(bool candidateIsSkirmishAI, bool candidateTargetsThisAI)
{
	return candidateIsSkirmishAI && candidateTargetsThisAI;
}

inline bool HasSkirmishRallyOffset(float x, float y)
{
	return x > 1.0f || x < -1.0f || y > 1.0f || y < -1.0f;
}
