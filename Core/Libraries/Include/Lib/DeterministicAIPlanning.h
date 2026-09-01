/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/CounterBasedRng.h"

namespace rts
{
enum
{
	AI_PLANNING_MAX_PLAYERS = 16,
	AI_PLANNING_MAX_PRODUCTION_CANDIDATES = 256,
	// These are bounded owner-capture capacities. A product that exceeds either
	// limit fails closed into its existing owner-serial selector.
	AI_PLANNING_MAX_PRODUCTION_FACTORIES = 32,
	AI_PLANNING_MAX_PRODUCTION_UNITS = 8,
	AI_PLANNING_PRODUCTION_SHARD_SIZE = 16,
	AI_PLANNING_MAX_PRODUCTION_SOURCE_JOBS =
		AI_PLANNING_MAX_PRODUCTION_CANDIDATES /
		AI_PLANNING_PRODUCTION_SHARD_SIZE,
	AI_PLANNING_MAX_JOB_EXECUTIONS = AI_PLANNING_MAX_PLAYERS *
		(AI_PLANNING_MAX_PRODUCTION_SOURCE_JOBS + 1)
};

static const uint32_t AI_PLANNING_INVALID_ORDINAL = 0xffffffffU;

// The team timer expires before doTeamBuilding resets the retry delay. Both
// products use this pure boundary predicate during owner-side admission.
inline bool IsAIProductionPlanningDue(bool readyToBuildTeam,
	int32_t teamTimer, int32_t teamDelay)
{
	return readyToBuildTeam ? teamDelay <= 1 : teamTimer <= 1;
}

enum AIPlanningDifficulty
{
	AI_PLANNING_DIFFICULTY_EASY = 0,
	AI_PLANNING_DIFFICULTY_NORMAL = 1,
	AI_PLANNING_DIFFICULTY_HARD = 2
};

enum AIPlanningRouteClass
{
	AI_PLANNING_ROUTE_UNKNOWN = 0,
	AI_PLANNING_ROUTE_GROUND_REACHABLE = 1,
	AI_PLANNING_ROUTE_MIXED_UNREACHABLE = 2,
	AI_PLANNING_ROUTE_GROUND_UNREACHABLE = 3
};

enum AIPlanningTargetRouteClass
{
	AI_PLANNING_TARGET_ROUTE_UNKNOWN = 0,
	AI_PLANNING_TARGET_ROUTE_REACHABLE = 1,
	AI_PLANNING_TARGET_ROUTE_UNREACHABLE = 2
};

enum AIPlanningSubphase
{
	AI_PLANNING_SUBPHASE_ENEMY_TARGET = 0,
	AI_PLANNING_SUBPHASE_TEAM_PRODUCTION = 1
};

enum AIPlanningExecutionMode
{
	AI_PLANNING_EXECUTION_SERIAL = 0,
	AI_PLANNING_EXECUTION_PARALLEL = 1,
	AI_PLANNING_EXECUTION_SHADOW = 2
};

// Owner-thread commit sorting key. Player index is before subphase so a batch
// always publishes in the legacy PlayerList order, then in each player's
// explicit AI subphase order.
struct AIPlanningOrderKey
{
	uint32_t frame;
	uint32_t playerIndex;
	uint32_t subphase;
	uint32_t sourceOrdinal;
	uint32_t emissionOrdinal;
};

struct AIEnemyCandidateFact
{
	uint32_t sourceOrdinal;
	int32_t playerIndex;
	int32_t knownAssetValue;
	int32_t distance;
	int32_t alliedAIsTargeting;
	int32_t routeClass;
	uint32_t hasKnownPosition;
	uint32_t targetingThisAI;
	uint32_t hasKnownObject;
	uint32_t hasKnownUnit;
	uint32_t hasKnownBuildFacility;
};

struct AIEnemyCandidateScore
{
	uint32_t sourceOrdinal;
	int32_t playerIndex;
	int32_t knownAssetScore;
	int32_t retaliationScore;
	int32_t routeScore;
	int32_t allyTargetScore;
	int32_t distanceScore;
	int32_t crippledScore;
	int32_t totalScore;
};

struct AIEnemyPlanningSnapshot
{
	uint32_t frame;
	uint32_t ownerPlayerIndex;
	int32_t currentEnemyPlayerIndex;
	int32_t switchScoreThreshold;
	uint32_t candidateCount;
	AIEnemyCandidateFact candidates[AI_PLANNING_MAX_PLAYERS];
};

struct AIEnemyPlanningResult
{
	uint32_t valid;
	int32_t selectedPlayerIndex;
	int32_t selectedScore;
	int32_t bestPlayerIndex;
	int32_t bestScore;
	uint32_t candidateScoreCount;
	AIPlanningOrderKey orderKey;
	AIEnemyCandidateScore candidateScores[AI_PLANNING_MAX_PLAYERS];
};

struct AIProductionCandidateFact
{
	uint32_t sourceOrdinal;
	uint32_t candidateStableId;
	int32_t configuredPriority;
	int32_t counterFitScore;
	int32_t minimumCost;
	int32_t plannedCost;
	int32_t factoryWaitFrames;
	int32_t routeClass;
	int32_t recentLossCount;
	int32_t recentPathFailureCount;
	uint32_t eligible;
};

struct AIProductionCandidateScore
{
	uint32_t sourceOrdinal;
	uint32_t candidateStableId;
	int32_t counterFitScore;
	int32_t economyScore;
	int32_t factoryWaitScore;
	int32_t routeScore;
	int32_t lossScore;
	int32_t pathFailureScore;
	int32_t rawContextScore;
	int64_t finalScore;
	uint32_t considered;
};

enum AIProductionUnitSourceFlags
{
	AI_PRODUCTION_SOURCE_AIRCRAFT = 1U,
	AI_PRODUCTION_SOURCE_VEHICLE = 2U,
	AI_PRODUCTION_SOURCE_INFANTRY = 4U,
	AI_PRODUCTION_SOURCE_ATTACKS_AIRCRAFT = 8U,
	AI_PRODUCTION_SOURCE_ATTACKS_GROUND = 16U,
	AI_PRODUCTION_SOURCE_PREFERS_VEHICLE = 32U,
	AI_PRODUCTION_SOURCE_PREFERS_INFANTRY = 64U
};

// All fields below are captured on the game owner thread. No live Object,
// ThingTemplate, weapon, factory, queue, or pathfinder pointer crosses the
// JobSystem boundary. Quantities are indexed by the stable captured factory
// ordinal and are zero when the factory cannot make this unit.
struct AIProductionUnitSourceFact
{
	int32_t cost;
	int32_t buildFrames;
	int32_t minUnits;
	int32_t maxUnits;
	uint32_t flags;
	uint32_t compatibleFactoryMask;
	uint8_t productionQuantity[AI_PLANNING_MAX_PRODUCTION_FACTORIES];
};

struct AIProductionCandidateSourceFact
{
	uint32_t valid;
	uint32_t unitCount;
	AIProductionUnitSourceFact units[AI_PLANNING_MAX_PRODUCTION_UNITS];
};

struct AIProductionFactorySourceFact
{
	int32_t projectedFrames;
	uint32_t valid;
	uint32_t idle;
};

struct AIProductionPlanningSourceFacts
{
	uint32_t valid;
	uint32_t factoryCount;
	AIProductionFactorySourceFact factories[
		AI_PLANNING_MAX_PRODUCTION_FACTORIES];
	int32_t enemyAircraftValue;
	int32_t enemyVehicleValue;
	int32_t enemyInfantryValue;
	uint32_t hasRouteTarget;
	uint32_t groundRouteKnown;
	uint32_t groundRouteReachable;
	AIProductionCandidateSourceFact candidates[
		AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
};

struct AIProductionPlanningSnapshot
{
	uint32_t frame;
	uint32_t ownerPlayerIndex;
	int32_t resources;
	int32_t logicFramesPerSecond;
	int32_t initialReserve;
	int32_t retryReserve;
	int32_t difficulty;
	int32_t contextInfluencePercent;
	uint32_t retryWithoutInitialReserve;
	uint32_t candidateCount;
	AICounterRngKey tieBreakKey;
	AIProductionCandidateFact candidates[AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
	// Optional immutable source view used by the current ZH production lane.
	// Legacy callers leave valid == 0 and continue to use candidates directly.
	AIProductionPlanningSourceFacts sourceFacts;
};

struct AIProductionPlanningResult
{
	uint32_t valid;
	uint32_t hasSelection;
	uint32_t selectedSourceOrdinal;
	uint32_t selectedStableId;
	int64_t selectedScore;
	int32_t highestPriority;
	int32_t committedReserve;
	uint32_t tieCount;
	uint32_t usedRetryReserve;
	uint32_t candidateScoreCount;
	AIPlanningOrderKey orderKey;
	AICounterRngLedgerRecord randomLedger;
	AIProductionCandidateScore candidateScores[AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
};

// Compact result used only by the owner-thread overflow lane. Unlike the
// worker result, it intentionally has no fixed-capacity score array.
struct AIProductionSelectionResult
{
	uint32_t valid;
	uint32_t hasSelection;
	uint32_t selectedSourceOrdinal;
	uint32_t selectedStableId;
	int64_t selectedScore;
	int32_t highestPriority;
	int32_t committedReserve;
	uint32_t tieCount;
	uint32_t usedRetryReserve;
	AICounterRngLedgerRecord randomLedger;
};

struct AIPlayerPlanningSnapshot
{
	uint32_t frame;
	uint32_t playerIndex;
	uint32_t planEnemyTarget;
	uint32_t planProduction;
	AIEnemyPlanningSnapshot enemyTarget;
	AIProductionPlanningSnapshot production;
};

struct AIPlayerPlanningResult
{
	uint32_t valid;
	uint32_t playerIndex;
	AIEnemyPlanningResult enemyTarget;
	AIProductionPlanningResult production;
};

struct AIPlanningBatchStatus
{
	uint32_t requestedMode;
	uint32_t committedMode;
	uint32_t parallelSucceeded;
	uint32_t shadowMatched;
	uint32_t usedSerialFallback;
	uint32_t mismatchPlayerOrdinal;
	uint64_t physicalWorkerMask;
	uint32_t distinctPhysicalWorkers;
	uint32_t peakConcurrentPhysicalWorkers;
	uint32_t ownerHelpedJobs;
};

// AI-only scheduler/accounting data. These counters deliberately exclude
// renderer, collision, pathfinding, and other JobSystem consumers so an
// installed-runtime manifest can prove that real AI planning work ran.
struct AIPlanningRuntimeMetrics
{
	uint64_t capturedSnapshots;
	uint64_t capturedCandidates;
	uint64_t requestedBatches;
	uint64_t submittedJobs;
	uint64_t completedJobs;
	uint64_t serialFallbacks;
	uint64_t shadowMatches;
	uint64_t shadowMismatches;
	uint64_t validationFailures;
	// Test/diagnostic seam: one increment per canonical production oracle call.
	// A normal accepted batch must account for this exactly once per production
	// snapshot; callers must not re-run the whole batch after publication.
	uint64_t canonicalValidationInvocations;
	// Every successful owner mutation is a committed batch. Only the separate
	// parallel-authority counter proves that an accepted batch came from the
	// requested parallel lane without a serial fallback.
	uint64_t committedBatches;
	uint64_t parallelAuthoritativeCommits;
	uint64_t rejectedCommits;
	uint64_t physicalWorkerExecutions;
	uint64_t ownerHelpedExecutions;
	uint64_t observedPhysicalWorkerMask;
	uint64_t maximumDistinctPhysicalWorkers;
	uint64_t maximumConcurrentPhysicalWorkers;
};

// A policy-owned parallel runner may schedule one PlanAIPlayer call per input.
// It receives only immutable POD snapshots and disjoint POD result slots.
typedef bool (*AIPlanningParallelBatchRunner)(
	const AIPlayerPlanningSnapshot *snapshots, uint32_t snapshotCount,
	AIPlayerPlanningResult *results, void *userData);

void ClearAIEnemyPlanningSnapshot(AIEnemyPlanningSnapshot *snapshot);
void ClearAIProductionPlanningSnapshot(AIProductionPlanningSnapshot *snapshot);
void ClearAIPlayerPlanningSnapshot(AIPlayerPlanningSnapshot *snapshot);
void ClearAIPlayerPlanningResult(AIPlayerPlanningResult *result);

bool PlanAIEnemyTarget(const AIEnemyPlanningSnapshot &snapshot,
	AIEnemyPlanningResult *result);
bool PlanAIProduction(const AIProductionPlanningSnapshot &snapshot,
	AIProductionPlanningResult *result);
bool PlanAIProductionSelectionOwnerSerial(
	const AIProductionPlanningSnapshot &context,
	const AIProductionCandidateFact *candidates, uint32_t candidateCount,
	AIProductionSelectionResult *result);
bool RequiresAIProductionOwnerSerialFallback(uint32_t candidateCount);
bool PlanAIPlayer(const AIPlayerPlanningSnapshot &snapshot,
	AIPlayerPlanningResult *result);
bool PlanAIPlayerBatchSerial(const AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, AIPlayerPlanningResult *results);

bool EqualAIEnemyPlanningResult(const AIEnemyPlanningResult &left,
	const AIEnemyPlanningResult &right);
bool EqualAIProductionPlanningResult(const AIProductionPlanningResult &left,
	const AIProductionPlanningResult &right);
bool EqualAIPlayerPlanningResult(const AIPlayerPlanningResult &left,
	const AIPlayerPlanningResult &right);

// Recomputes the complete canonical answer, including winner, score, tie set,
// counter-RNG ledger and selected tie index, before accepting an untrusted
// worker result.
bool ValidateAIProductionPlanningResult(
	const AIProductionPlanningSnapshot &snapshot,
	const AIProductionPlanningResult &result);

// Validates the redundant worker-prepared candidate views before either view
// can replace the immutable source facts used by the owner reduction.
bool ValidateAIProductionPreparedFacts(
	const AIProductionCandidateFact *prepared,
	const AIProductionCandidateFact *verification, uint32_t candidateCount);

// Checks the entire immutable-snapshot contract, including frame/player,
// subphase order keys, candidate score identity, and selected membership.
bool ValidateAIPlayerPlanningResult(const AIPlayerPlanningSnapshot &snapshot,
	const AIPlayerPlanningResult &result);
bool ValidateAIPlanningBatchResults(const AIPlayerPlanningSnapshot *snapshots,
	const AIPlayerPlanningResult *results, uint32_t resultCount);

// No result becomes commit-visible until the whole requested path succeeds.
// Parallel failure and shadow mismatch publish the complete serial batch.
bool ExecuteAIPlanningBatch(AIPlanningExecutionMode mode,
	const AIPlayerPlanningSnapshot *snapshots, uint32_t snapshotCount,
	AIPlayerPlanningResult *committedResults,
	AIPlayerPlanningResult *serialScratch,
	AIPlayerPlanningResult *parallelScratch,
	AIPlanningParallelBatchRunner parallelRunner,
	void *parallelUserData,
	AIPlanningBatchStatus *status);

// Production runner. The owner submits one meaningful per-player planning
// collection to the shared JobSystem. Workers see only immutable POD snapshots
// and disjoint POD result slots. Any admission/execution/validation failure is
// joined and recomputed serially before this function returns.
bool ExecuteAIPlanningBatchOnJobSystem(AIPlanningExecutionMode mode,
	const AIPlayerPlanningSnapshot *snapshots, uint32_t snapshotCount,
	AIPlayerPlanningResult *committedResults,
	AIPlayerPlanningResult *serialScratch,
	AIPlayerPlanningResult *parallelScratch,
	AIPlanningBatchStatus *status);

void ResetAIPlanningRuntimeMetrics();
AIPlanningRuntimeMetrics GetAIPlanningRuntimeMetrics();
void RecordAIPlanningOwnerCapture(uint32_t candidateCount);
void RecordAIPlanningOwnerCommit(bool accepted,
	const AIPlanningBatchStatus *status = 0);
}
