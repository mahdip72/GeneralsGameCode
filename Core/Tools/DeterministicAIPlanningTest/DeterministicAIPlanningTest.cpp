#include "Lib/CounterBasedRng.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

#if defined(_WIN64)
#include <chrono>
#include <thread>
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#include "../TestSupport/NativeKernelSourceConsumerTest.h"
#endif

namespace rts
{
bool AIOwnerPrefixInvalidResultPlanner(
	const AIProductionPlanningSnapshot &context,
	const AIProductionCandidateFact *candidates, uint32_t candidateCount,
	AIProductionSelectionResult *result)
{
	const bool planned = PlanAIProductionSelectionOwnerSerial(
		context, candidates, candidateCount, result);
	if (result != 0)
		result->valid = 0U;
	return planned;
}
}

namespace
{
rts::AICounterRngKey MakeTestRandomKey()
{
	rts::AICounterRngKey key;
	rts::ClearAICounterRngKey(&key);
	key.simulationEpoch = 7U;
	key.matchSeed = 0x12345678U;
	key.frame = 900U;
	key.domain = rts::AI_COUNTER_RNG_DOMAIN_PLAYER_PLANNING;
	key.playerIndex = 3U;
	key.ownerStableId = 3U;
	key.eventKind = rts::AI_COUNTER_RNG_EVENT_PRODUCTION_TIE;
	return key;
}

bool RunExtractedAIOwnerProductionPrefix(
	const rts::AIProductionPlanningSnapshot &context,
	const std::vector<rts::AIProductionCandidateFact> &candidateFacts)
{
typedef uint32_t UnsignedInt;
#include "AIOwnerProductionPrefix.inc"
	return true;
}

bool RunExtractedAIOwnerProductionPrefixWithInvalidResult(
	const rts::AIProductionPlanningSnapshot &context,
	const std::vector<rts::AIProductionCandidateFact> &candidateFacts)
{
typedef uint32_t UnsignedInt;
#define PlanAIProductionSelectionOwnerSerial AIOwnerPrefixInvalidResultPlanner
#include "AIOwnerProductionPrefix.inc"
#undef PlanAIProductionSelectionOwnerSerial
	return true;
}

rts::AIProductionPlanningSnapshot MakeAIOwnerPrefixContext(int resources)
{
	rts::AIProductionPlanningSnapshot context;
	rts::ClearAIProductionPlanningSnapshot(&context);
	context.frame = 901U;
	context.ownerPlayerIndex = 3U;
	context.resources = resources;
	context.logicFramesPerSecond = 30;
	context.initialReserve = 0;
	context.retryReserve = 0;
	context.difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
	context.contextInfluencePercent = 100;
	context.tieBreakKey = MakeTestRandomKey();
	return context;
}

std::vector<rts::AIProductionCandidateFact> MakeAIOwnerPrefixCandidates(
	int minimumCost)
{
	std::vector<rts::AIProductionCandidateFact> candidates(1U);
	candidates[0].sourceOrdinal = 4U;
	candidates[0].candidateStableId = 44U;
	candidates[0].configuredPriority = 100;
	candidates[0].minimumCost = minimumCost;
	candidates[0].plannedCost = minimumCost;
	candidates[0].factoryWaitFrames = 1;
	candidates[0].counterFitScore = 100;
	candidates[0].eligible = 1U;
	return candidates;
}

void TestCounterRngGoldenVector()
{
	rts::AICounterRngKey key = MakeTestRandomKey();
	assert(rts::GenerateAICounterRngWord(key) == 0xb3b7e622U);
	assert(rts::GenerateAICounterRngWord(
		rts::AdvanceAICounterRngDraw(key, 1U)) == 0x180e0951U);
	uint32_t selected = 0U;
	rts::AICounterRngLedgerRecord ledger;
	assert(rts::GenerateAICounterRngIndex(key, 3U, &selected, &ledger));
	assert(selected == 2U);
	assert(ledger.valid == 1U);
	assert(rts::EqualAICounterRngKey(key, ledger.key));
}

void TestEnemyScoringAndHysteresis()
{
	rts::AIEnemyPlanningSnapshot snapshot;
	rts::ClearAIEnemyPlanningSnapshot(&snapshot);
	snapshot.frame = 120U;
	snapshot.ownerPlayerIndex = 3U;
	snapshot.currentEnemyPlayerIndex = 2;
	snapshot.candidateCount = 3U;

	snapshot.candidates[0].sourceOrdinal = 1U;
	snapshot.candidates[0].playerIndex = 5;
	snapshot.candidates[0].knownAssetValue = 100;
	snapshot.candidates[0].distance = 100;
	snapshot.candidates[0].routeClass = rts::AI_PLANNING_TARGET_ROUTE_REACHABLE;
	snapshot.candidates[0].hasKnownPosition = 1U;
	snapshot.candidates[0].hasKnownObject = 1U;
	snapshot.candidates[0].hasKnownUnit = 1U;

	snapshot.candidates[1].sourceOrdinal = 2U;
	snapshot.candidates[1].playerIndex = 2;
	snapshot.candidates[1].knownAssetValue = 1000;
	snapshot.candidates[1].distance = 500;
	snapshot.candidates[1].hasKnownPosition = 1U;
	snapshot.candidates[1].hasKnownObject = 1U;
	snapshot.candidates[1].hasKnownUnit = 1U;

	snapshot.candidates[2].sourceOrdinal = 4U;
	snapshot.candidates[2].playerIndex = 7;
	snapshot.candidates[2].distance = 300;
	snapshot.candidates[2].routeClass = rts::AI_PLANNING_TARGET_ROUTE_UNREACHABLE;
	snapshot.candidates[2].hasKnownPosition = 1U;
	snapshot.candidates[2].hasKnownObject = 1U;

	rts::AIEnemyPlanningResult result;
	assert(rts::PlanAIEnemyTarget(snapshot, &result));
	assert(result.bestPlayerIndex == 5);
	assert(result.bestScore == 180);
	assert(result.selectedPlayerIndex == 2);

	snapshot.switchScoreThreshold = 100;
	assert(rts::PlanAIEnemyTarget(snapshot, &result));
	assert(result.selectedPlayerIndex == 5);
	assert(result.orderKey.playerIndex == 3U);
	assert(result.orderKey.subphase == rts::AI_PLANNING_SUBPHASE_ENEMY_TARGET);
	assert(result.orderKey.sourceOrdinal == 1U);
}

void TestProductionScoringTieAndRetry()
{
	rts::AIProductionPlanningSnapshot snapshot;
	rts::ClearAIProductionPlanningSnapshot(&snapshot);
	snapshot.frame = 900U;
	snapshot.ownerPlayerIndex = 3U;
	snapshot.resources = 1000;
	snapshot.logicFramesPerSecond = 30;
	snapshot.difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
	snapshot.contextInfluencePercent = 100;
	snapshot.candidateCount = 3U;
	snapshot.tieBreakKey = MakeTestRandomKey();

	snapshot.candidates[0].sourceOrdinal = 2U;
	snapshot.candidates[0].candidateStableId = 10U;
	snapshot.candidates[0].configuredPriority = 100;
	snapshot.candidates[0].eligible = 1U;
	snapshot.candidates[1] = snapshot.candidates[0];
	snapshot.candidates[1].sourceOrdinal = 4U;
	snapshot.candidates[1].candidateStableId = 11U;
	snapshot.candidates[1].counterFitScore = 300;
	snapshot.candidates[1].routeClass = rts::AI_PLANNING_ROUTE_GROUND_REACHABLE;
	snapshot.candidates[2] = snapshot.candidates[0];
	snapshot.candidates[2].sourceOrdinal = 7U;
	snapshot.candidates[2].candidateStableId = 12U;
	snapshot.candidates[2].configuredPriority = 90;
	snapshot.candidates[2].counterFitScore = 300;

	rts::AIProductionPlanningResult result;
	assert(rts::PlanAIProduction(snapshot, &result));
	assert(result.hasSelection == 1U);
	assert(result.selectedStableId == 11U);
	assert(result.selectedScore == 100450);
	assert(result.randomLedger.valid == 0U);

	snapshot.candidates[1].counterFitScore = 0;
	snapshot.candidates[1].routeClass = rts::AI_PLANNING_ROUTE_UNKNOWN;
	assert(rts::PlanAIProduction(snapshot, &result));
	assert(result.tieCount == 2U);
	assert(result.randomLedger.valid == 1U);
	assert(result.selectedStableId == 10U);

	snapshot.candidateCount = 1U;
	snapshot.resources = 150;
	snapshot.initialReserve = 100;
	snapshot.retryReserve = 0;
	snapshot.retryWithoutInitialReserve = 1U;
	snapshot.candidates[0].minimumCost = 100;
	snapshot.candidates[0].plannedCost = 100;
	assert(rts::PlanAIProduction(snapshot, &result));
	assert(result.hasSelection == 1U);
	assert(result.usedRetryReserve == 1U);
	assert(result.committedReserve == 0);
}

void TestProductionWinnerAndTieForgeryRejection()
{
	rts::AIPlayerPlanningSnapshot snapshot;
	rts::ClearAIPlayerPlanningSnapshot(&snapshot);
	snapshot.frame = 900U;
	snapshot.playerIndex = 3U;
	snapshot.planProduction = 1U;
	snapshot.production.frame = snapshot.frame;
	snapshot.production.ownerPlayerIndex = snapshot.playerIndex;
	snapshot.production.resources = 1000;
	snapshot.production.logicFramesPerSecond = 30;
	snapshot.production.difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
	snapshot.production.contextInfluencePercent = 100;
	snapshot.production.tieBreakKey = MakeTestRandomKey();
	snapshot.production.candidateCount = 2U;
	for (uint32_t i = 0; i < 2U; ++i)
	{
		snapshot.production.candidates[i].sourceOrdinal = i + 4U;
		snapshot.production.candidates[i].candidateStableId = i + 40U;
		snapshot.production.candidates[i].configuredPriority = 100;
		snapshot.production.candidates[i].eligible = 1U;
	}
	snapshot.production.candidates[0].counterFitScore = 300;

	rts::AIPlayerPlanningResult canonical;
	assert(rts::PlanAIPlayer(snapshot, &canonical));
	assert(canonical.production.selectedStableId == 40U);
	assert(rts::ValidateAIPlayerPlanningResult(snapshot, canonical));

	// Every membership/order field is internally consistent, but the selected
	// candidate is not the maximum-scoring candidate.
	rts::AIPlayerPlanningResult forgedWinner = canonical;
	forgedWinner.production.selectedSourceOrdinal =
		snapshot.production.candidates[1].sourceOrdinal;
	forgedWinner.production.selectedStableId =
		snapshot.production.candidates[1].candidateStableId;
	forgedWinner.production.selectedScore =
		canonical.production.candidateScores[1].finalScore;
	forgedWinner.production.orderKey.sourceOrdinal =
		forgedWinner.production.selectedSourceOrdinal;
	assert(!rts::ValidateAIProductionPlanningResult(
		snapshot.production, forgedWinner.production));
	assert(!rts::ValidateAIPlayerPlanningResult(snapshot, forgedWinner));

	// Equalize both candidates, then forge selection of the other tie member
	// while retaining the canonical counter ledger/tie index.
	snapshot.production.candidates[0].counterFitScore = 0;
	assert(rts::PlanAIPlayer(snapshot, &canonical));
	assert(canonical.production.tieCount == 2U);
	assert(canonical.production.randomLedger.valid == 1U);
	const uint32_t canonicalIndex =
		canonical.production.randomLedger.selectedIndex;
	const uint32_t otherIndex = canonicalIndex == 0U ? 1U : 0U;
	rts::AIPlayerPlanningResult forgedTieWinner = canonical;
	forgedTieWinner.production.selectedSourceOrdinal =
		snapshot.production.candidates[otherIndex].sourceOrdinal;
	forgedTieWinner.production.selectedStableId =
		snapshot.production.candidates[otherIndex].candidateStableId;
	forgedTieWinner.production.orderKey.sourceOrdinal =
		forgedTieWinner.production.selectedSourceOrdinal;
	assert(!rts::ValidateAIPlayerPlanningResult(snapshot, forgedTieWinner));

	rts::AIPlayerPlanningResult forgedTieIndex = canonical;
	forgedTieIndex.production.randomLedger.selectedIndex = otherIndex;
	assert(!rts::ValidateAIPlayerPlanningResult(snapshot, forgedTieIndex));
}

void TestProductionOwnerSerialOverflowBoundary()
{
	assert(!rts::RequiresAIProductionOwnerSerialFallback(256U));
	assert(rts::RequiresAIProductionOwnerSerialFallback(257U));

	rts::AIProductionPlanningSnapshot context;
	rts::ClearAIProductionPlanningSnapshot(&context);
	context.frame = 900U;
	context.ownerPlayerIndex = 3U;
	context.resources = 1000;
	context.logicFramesPerSecond = 30;
	context.difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
	context.contextInfluencePercent = 100;
	context.tieBreakKey = MakeTestRandomKey();

	std::vector<rts::AIProductionCandidateFact> candidates(257U);
	for (uint32_t i = 0; i < 257U; ++i)
	{
		candidates[i].sourceOrdinal = i;
		candidates[i].candidateStableId = i + 1000U;
		candidates[i].configuredPriority = 100;
		candidates[i].eligible = 1U;
	}

	rts::AIProductionSelectionResult selection256;
	assert(rts::PlanAIProductionSelectionOwnerSerial(
		context, &candidates[0], 256U, &selection256));
	assert(selection256.valid == 1U);
	assert(selection256.hasSelection == 1U);
	assert(selection256.tieCount == 256U);
	assert(selection256.randomLedger.exclusiveUpperBound == 256U);

	rts::AIProductionPlanningSnapshot fixedSnapshot = context;
	fixedSnapshot.candidateCount = 256U;
	for (uint32_t i = 0; i < fixedSnapshot.candidateCount; ++i)
		fixedSnapshot.candidates[i] = candidates[i];
	rts::AIProductionPlanningResult fixedResult;
	assert(rts::PlanAIProduction(fixedSnapshot, &fixedResult));
	assert(selection256.selectedSourceOrdinal == fixedResult.selectedSourceOrdinal);
	assert(selection256.selectedStableId == fixedResult.selectedStableId);
	assert(selection256.selectedScore == fixedResult.selectedScore);
	assert(rts::EqualAICounterRngLedgerRecord(
		selection256.randomLedger, fixedResult.randomLedger));

	rts::AIProductionSelectionResult selection257;
	assert(rts::PlanAIProductionSelectionOwnerSerial(
		context, &candidates[0], 257U, &selection257));
	assert(selection257.valid == 1U);
	assert(selection257.hasSelection == 1U);
	assert(selection257.tieCount == 257U);
	assert(selection257.randomLedger.valid == 1U);
	assert(selection257.randomLedger.exclusiveUpperBound == 257U);
	assert(selection257.randomLedger.selectedIndex < 257U);
	assert(selection257.selectedSourceOrdinal ==
		selection257.randomLedger.selectedIndex);
}

void TestAIOwnerProductionBookkeeping()
{
	const rts::AIProductionPlanningSnapshot noSelectionContext =
		MakeAIOwnerPrefixContext(0);
	const std::vector<rts::AIProductionCandidateFact> noSelectionCandidates =
		MakeAIOwnerPrefixCandidates(100);
	rts::ResetAIPlanningRuntimeMetrics();
	assert(!RunExtractedAIOwnerProductionPrefix(
		noSelectionContext, noSelectionCandidates));
	rts::AIPlanningRuntimeMetrics metrics =
		rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.capturedSnapshots == 1U);
	assert(metrics.rejectedCommits == 0U);
	assert(metrics.committedBatches == 0U);

	const rts::AIProductionPlanningSnapshot selectedContext =
		MakeAIOwnerPrefixContext(1000);
	const std::vector<rts::AIProductionCandidateFact> selectedCandidates =
		MakeAIOwnerPrefixCandidates(10);
	rts::ResetAIPlanningRuntimeMetrics();
	assert(RunExtractedAIOwnerProductionPrefix(
		selectedContext, selectedCandidates));
	metrics = rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.capturedSnapshots == 1U);
	assert(metrics.rejectedCommits == 0U);
	// The extracted prefix reaches the live selected-prototype continuation;
	// it must not invent an accepted owner commit before that continuation.
	assert(metrics.committedBatches == 0U);

	// Drive the actual extracted owner prefix through its invalid-result branch
	// using a test-only forwarding shim; no production API or owner logic is
	// duplicated, and the real planner still supplies the result shape first.
	rts::ResetAIPlanningRuntimeMetrics();
	assert(!RunExtractedAIOwnerProductionPrefixWithInvalidResult(
		selectedContext, selectedCandidates));
	metrics = rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.capturedSnapshots == 1U);
	assert(metrics.rejectedCommits == 1U);
	assert(metrics.committedBatches == 0U);
}

void TestProductionDueExpiryBoundary()
{
	// The non-ready timer is admitted on its expiry frame regardless of the
	// prior retry delay; a ready player uses the retry-delay boundary instead.
	assert(rts::IsAIProductionPlanningDue(false, 1, 99));
	assert(!rts::IsAIProductionPlanningDue(false, 2, 0));
	assert(rts::IsAIProductionPlanningDue(true, 99, 1));
	assert(!rts::IsAIProductionPlanningDue(true, 99, 2));
}

void TestProductionSourceViewParity()
{
	std::vector<rts::AIProductionPlanningSnapshot> direct(1U);
	std::vector<rts::AIProductionPlanningSnapshot> source(1U);
	rts::ClearAIProductionPlanningSnapshot(&direct[0]);
	rts::ClearAIProductionPlanningSnapshot(&source[0]);
	direct[0].frame = source[0].frame = 903U;
	direct[0].ownerPlayerIndex = source[0].ownerPlayerIndex = 3U;
	direct[0].resources = source[0].resources = 1000;
	direct[0].logicFramesPerSecond = source[0].logicFramesPerSecond = 30;
	direct[0].difficulty = source[0].difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
	direct[0].contextInfluencePercent = source[0].contextInfluencePercent = 100;
	direct[0].tieBreakKey = source[0].tieBreakKey = MakeTestRandomKey();
	direct[0].candidateCount = source[0].candidateCount = 1U;
	direct[0].candidates[0].sourceOrdinal = source[0].candidates[0].sourceOrdinal = 4U;
	direct[0].candidates[0].candidateStableId =
		source[0].candidates[0].candidateStableId = 44U;
	direct[0].candidates[0].configuredPriority =
		source[0].candidates[0].configuredPriority = 100;
	direct[0].candidates[0].eligible = source[0].candidates[0].eligible = 1U;
	direct[0].candidates[0].minimumCost = 10;
	direct[0].candidates[0].plannedCost = 20;
	direct[0].candidates[0].factoryWaitFrames = 12;
	direct[0].candidates[0].counterFitScore = 300;
	direct[0].candidates[0].routeClass = rts::AI_PLANNING_ROUTE_GROUND_REACHABLE;

	rts::AIProductionPlanningSourceFacts &facts = source[0].sourceFacts;
	facts.valid = 1U;
	facts.factoryCount = 2U;
	facts.factories[0].valid = facts.factories[1].valid = 1U;
	facts.factories[0].projectedFrames = 3;
	facts.factories[1].projectedFrames = 7;
	facts.enemyVehicleValue = 100;
	facts.enemyInfantryValue = 50;
	facts.hasRouteTarget = 1U;
	facts.groundRouteKnown = 1U;
	facts.groundRouteReachable = 1U;
	facts.candidates[0].valid = 1U;
	facts.candidates[0].unitCount = 1U;
	rts::AIProductionUnitSourceFact &unit = facts.candidates[0].units[0];
	unit.cost = 10;
	unit.buildFrames = 5;
	unit.minUnits = 1;
	unit.maxUnits = 2;
	unit.flags = rts::AI_PRODUCTION_SOURCE_VEHICLE |
		rts::AI_PRODUCTION_SOURCE_ATTACKS_GROUND;
	unit.compatibleFactoryMask = 3U;
	unit.productionQuantity[0] = unit.productionQuantity[1] = 1U;

	rts::AIProductionPlanningResult directResult;
	rts::AIProductionPlanningResult sourceResult;
	assert(rts::PlanAIProduction(direct[0], &directResult));
	assert(rts::PlanAIProduction(source[0], &sourceResult));
	assert(rts::EqualAIProductionPlanningResult(directResult, sourceResult));

	// Worker-prepared facts are accepted only when their independently derived
	// verification view agrees field-for-field. A forged priority must fail
	// before it can redefine the owner reduction's canonical input.
	rts::AIProductionCandidateFact prepared[2];
	rts::AIProductionCandidateFact verification[2];
	prepared[0] = verification[0] = direct[0].candidates[0];
	prepared[1] = verification[1] = direct[0].candidates[0];
	prepared[1].sourceOrdinal = verification[1].sourceOrdinal = 1U;
	prepared[1].candidateStableId = verification[1].candidateStableId = 45U;
	assert(rts::ValidateAIProductionPreparedFacts(prepared, verification, 2U));
	verification[1].configuredPriority ^= 1;
	assert(!rts::ValidateAIProductionPreparedFacts(prepared, verification, 2U));
	assert(!rts::ValidateAIProductionPreparedFacts(prepared, verification,
		rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES + 1U));
}

bool RunBatch(const rts::AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, rts::AIPlayerPlanningResult *results, void *userData)
{
	if (!rts::PlanAIPlayerBatchSerial(snapshots, snapshotCount, results))
		return false;
	if (userData != 0 && snapshotCount != 0U)
		results[0].production.selectedScore ^= 1;
	return true;
}

bool FailBatch(const rts::AIPlayerPlanningSnapshot *, uint32_t,
	rts::AIPlayerPlanningResult *, void *)
{
	return false;
}

bool MalformedCountBatch(const rts::AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, rts::AIPlayerPlanningResult *results, void *)
{
	if (!rts::PlanAIPlayerBatchSerial(snapshots, snapshotCount, results) ||
		snapshotCount == 0U)
		return false;
	results[0].production.candidateScoreCount =
		rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES + 1U;
	return true;
}

bool MalformedSelectionBatch(const rts::AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, rts::AIPlayerPlanningResult *results, void *)
{
	if (!rts::PlanAIPlayerBatchSerial(snapshots, snapshotCount, results) ||
		snapshotCount == 0U)
		return false;
	results[0].production.selectedSourceOrdinal += 1U;
	results[0].production.orderKey.sourceOrdinal =
		results[0].production.selectedSourceOrdinal;
	return true;
}

bool ForgedEligibleWinnerBatch(const rts::AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, rts::AIPlayerPlanningResult *results, void *)
{
	if (!rts::PlanAIPlayerBatchSerial(snapshots, snapshotCount, results) ||
		snapshotCount == 0U || snapshots[0].production.candidateCount < 2U)
		return false;
	results[0].production.selectedSourceOrdinal =
		snapshots[0].production.candidates[1].sourceOrdinal;
	results[0].production.selectedStableId =
		snapshots[0].production.candidates[1].candidateStableId;
	results[0].production.selectedScore =
		results[0].production.candidateScores[1].finalScore;
	results[0].production.orderKey.sourceOrdinal =
		results[0].production.selectedSourceOrdinal;
	return true;
}

bool CorruptBatchAtOrdinal(const rts::AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, rts::AIPlayerPlanningResult *results, void *userData)
{
	if (!rts::PlanAIPlayerBatchSerial(snapshots, snapshotCount, results) ||
		userData == 0)
		return false;
	const uint32_t ordinal = *static_cast<const uint32_t *>(userData);
	if (ordinal >= snapshotCount)
		return false;
	results[ordinal].production.selectedScore ^= 1;
	return true;
}

void InitializeProductionPlanningBatch(
	std::vector<rts::AIPlayerPlanningSnapshot> *snapshots)
{
	assert(snapshots != 0);
	for (uint32_t i = 0; i < snapshots->size(); ++i)
	{
		rts::AIPlayerPlanningSnapshot &snapshot = (*snapshots)[i];
		rts::ClearAIPlayerPlanningSnapshot(&snapshot);
		snapshot.frame = 902U;
		snapshot.playerIndex = i + 1U;
		snapshot.planProduction = 1U;
		snapshot.production.frame = snapshot.frame;
		snapshot.production.ownerPlayerIndex = snapshot.playerIndex;
		snapshot.production.resources = 2000;
		snapshot.production.logicFramesPerSecond = 30;
		snapshot.production.initialReserve = 0;
		snapshot.production.retryReserve = 0;
		snapshot.production.difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
		snapshot.production.contextInfluencePercent = 100;
		snapshot.production.tieBreakKey = MakeTestRandomKey();
		snapshot.production.tieBreakKey.frame = snapshot.frame;
		snapshot.production.tieBreakKey.playerIndex = snapshot.playerIndex;
		snapshot.production.tieBreakKey.ownerStableId = snapshot.playerIndex;
		snapshot.production.candidateCount = 3U;
		for (uint32_t candidate = 0; candidate < 3U; ++candidate)
		{
			rts::AIProductionCandidateFact &fact =
				snapshot.production.candidates[candidate];
			fact.sourceOrdinal = candidate * 2U + 1U;
			fact.candidateStableId = (i + 1U) * 100U + candidate;
			fact.configuredPriority = candidate == 2U ? 90 : 100;
			fact.counterFitScore = candidate * 75;
			fact.minimumCost = 10 + (int32_t)candidate;
			fact.plannedCost = 20 + (int32_t)candidate;
			fact.factoryWaitFrames = (int32_t)candidate * 3;
			fact.routeClass = rts::AI_PLANNING_ROUTE_GROUND_REACHABLE;
			fact.eligible = 1U;
		}
	}
}

void InitializeSinglePlayerSourceShardBatch(
	rts::AIPlayerPlanningSnapshot *snapshot)
{
	assert(snapshot != 0);
	rts::ClearAIPlayerPlanningSnapshot(snapshot);
	snapshot->frame = 904U;
	snapshot->playerIndex = 1U;
	snapshot->planProduction = 1U;
	rts::AIProductionPlanningSnapshot &production = snapshot->production;
	production.frame = snapshot->frame;
	production.ownerPlayerIndex = snapshot->playerIndex;
	production.resources = 1000000;
	production.logicFramesPerSecond = 30;
	production.difficulty = rts::AI_PLANNING_DIFFICULTY_HARD;
	production.contextInfluencePercent = 100;
	production.candidateCount = rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES;
	production.sourceFacts.valid = 1U;
	production.sourceFacts.factoryCount =
		rts::AI_PLANNING_MAX_PRODUCTION_FACTORIES;
	for (uint32_t factory = 0U;
		factory < rts::AI_PLANNING_MAX_PRODUCTION_FACTORIES; ++factory)
	{
		production.sourceFacts.factories[factory].valid = 1U;
		production.sourceFacts.factories[factory].idle = 1U;
		production.sourceFacts.factories[factory].projectedFrames =
			(int32_t)factory;
	}
	for (uint32_t candidate = 0U;
		candidate < production.candidateCount; ++candidate)
	{
		rts::AIProductionCandidateFact &fact = production.candidates[candidate];
		fact.sourceOrdinal = candidate;
		fact.candidateStableId = 1000U + candidate;
		fact.configuredPriority = 100 - (int32_t)(candidate % 3U);
		fact.eligible = 1U;
		rts::AIProductionCandidateSourceFact &source =
			production.sourceFacts.candidates[candidate];
		source.valid = 1U;
		source.unitCount = rts::AI_PLANNING_MAX_PRODUCTION_UNITS;
		for (uint32_t unitIndex = 0U;
			unitIndex < rts::AI_PLANNING_MAX_PRODUCTION_UNITS; ++unitIndex)
		{
			rts::AIProductionUnitSourceFact &unit = source.units[unitIndex];
			unit.cost = 10 + (int32_t)unitIndex;
			unit.buildFrames = 15 + (int32_t)unitIndex;
			unit.minUnits = 1;
			unit.maxUnits = 2;
			unit.flags = rts::AI_PRODUCTION_SOURCE_ATTACKS_GROUND;
			unit.compatibleFactoryMask = 0xffffffffU;
			for (uint32_t factory = 0U;
				factory < rts::AI_PLANNING_MAX_PRODUCTION_FACTORIES; ++factory)
				unit.productionQuantity[factory] = 1U;
		}
	}
}

#if defined(_WIN64)
struct AIPlanningPerformanceTestClock
{
	AIPlanningPerformanceTestClock() : now(100U) {}

	rts::JobMetricCounter now;

	static rts::JobMetricCounter read(void *context)
	{
		return static_cast<AIPlanningPerformanceTestClock *>(context)->now;
	}
};

void ObserveAIPlanningTestStage(
	rts::performance::KernelPerformanceLedger &ledger,
	rts::performance::KernelPerformanceBatch batch,
	rts::performance::KernelPerformanceStage stage,
	AIPlanningPerformanceTestClock &clock)
{
	const rts::performance::KernelPerformanceInterval interval =
		ledger.beginInterval(batch, stage);
	assert(interval.valid());
	++clock.now;
	assert(ledger.endInterval(interval));
}

typedef rts::AIPlanningReferencePlayerInputView
	AIPlanningReferencePlayerInputView;
typedef rts::AIPlanningReferencePlayerOutputView
	AIPlanningReferencePlayerOutputView;

bool WriteTestAIPlanningReferenceInput(
	rts::performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	return rts::WriteAIPlanningReferenceInput(writer, context);
}

bool WriteTestAIPlanningReferenceOutput(
	rts::performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	return rts::WriteAIPlanningReferenceOutput(writer, context);
}

unsigned g_aiReferenceSerialComputes = 0U;

bool ComputeTestAIPlanningReferenceSerial(const void *immutableInput,
	void *detachedOutput)
{
	++g_aiReferenceSerialComputes;
	return rts::ComputeAIPlanningReferenceSerial(immutableInput, detachedOutput);
}

void InitializeAIReferenceSourceFacts(
	rts::AIPlayerPlanningSnapshot *snapshots, uint32_t count)
{
	for (uint32_t i = 0U; i < count; ++i)
	{
		rts::AIProductionPlanningSnapshot &production =
			snapshots[i].production;
		production.sourceFacts.valid = 1U;
		production.sourceFacts.factoryCount = 1U;
		production.sourceFacts.factories[0].valid = 1U;
		production.sourceFacts.factories[0].idle = 1U;
		production.sourceFacts.factories[0].projectedFrames = 2;
		for (uint32_t candidate = 0U;
			candidate < production.candidateCount; ++candidate)
		{
			rts::AIProductionCandidateSourceFact &source =
				production.sourceFacts.candidates[candidate];
			source.valid = 1U;
			source.unitCount = 1U;
			rts::AIProductionUnitSourceFact &unit = source.units[0];
			unit.cost = 10 + static_cast<int32_t>(candidate);
			unit.buildFrames = 5;
			unit.minUnits = 1;
			unit.maxUnits = 2;
			unit.flags = rts::AI_PRODUCTION_SOURCE_VEHICLE |
				rts::AI_PRODUCTION_SOURCE_ATTACKS_GROUND;
			unit.compatibleFactoryMask = 1U;
			unit.productionQuantity[0] = 1U;
		}
	}
}

void TestAIPlanningReferenceTransport()
{
	using namespace rts::performance;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2U;
	config.queueCapacity = 64U;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));

	AIPlanningPerformanceTestClock clock;
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	assert(timing.beginRun(true, AIPlanningPerformanceTestClock::read, &clock));
	KernelPerformanceReferenceLedger reference;
	assert(reference.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE,
		AIPlanningPerformanceTestClock::read, &clock));
	const uint32_t snapshotCount = 2U;

	for (uint32_t subtype = 0U; subtype <= 1U; ++subtype)
	{
		// Production results contain fixed-capacity score/source arrays. Keep the
		// reference fixture off the 1 MB Windows test stack even when its admitted
		// batch is intentionally small.
		std::vector<rts::AIPlayerPlanningSnapshot> snapshots(snapshotCount);
		std::vector<rts::AIPlayerPlanningResult> committed(snapshotCount);
		std::vector<rts::AIPlayerPlanningResult> serialScratch(snapshotCount);
		std::vector<rts::AIPlayerPlanningResult> parallelScratch(snapshotCount);
		for (uint32_t i = 0U; i < snapshotCount; ++i)
		{
			rts::ClearAIPlayerPlanningSnapshot(&snapshots[i]);
			snapshots[i].frame = 910U + subtype;
			snapshots[i].playerIndex = i + 1U;
			if (subtype == 0U)
			{
				snapshots[i].planEnemyTarget = 1U;
				snapshots[i].enemyTarget.frame = snapshots[i].frame;
				snapshots[i].enemyTarget.ownerPlayerIndex =
					snapshots[i].playerIndex;
				snapshots[i].enemyTarget.candidateCount = 1U;
				snapshots[i].enemyTarget.candidates[0].sourceOrdinal = 7U;
				snapshots[i].enemyTarget.candidates[0].playerIndex = 7;
				snapshots[i].enemyTarget.candidates[0].knownAssetValue =
					100 + static_cast<int32_t>(i);
				snapshots[i].enemyTarget.candidates[0].hasKnownObject = 1U;
				snapshots[i].enemyTarget.candidates[0].hasKnownUnit = 1U;
			}
			else
			{
				snapshots[i].planProduction = 1U;
				snapshots[i].production.frame = snapshots[i].frame;
				snapshots[i].production.ownerPlayerIndex =
					snapshots[i].playerIndex;
				snapshots[i].production.resources = 2000;
				snapshots[i].production.logicFramesPerSecond = 30;
				snapshots[i].production.difficulty =
					rts::AI_PLANNING_DIFFICULTY_HARD;
				snapshots[i].production.contextInfluencePercent = 100;
				snapshots[i].production.tieBreakKey = MakeTestRandomKey();
				snapshots[i].production.tieBreakKey.frame = snapshots[i].frame;
				snapshots[i].production.tieBreakKey.playerIndex =
					snapshots[i].playerIndex;
				snapshots[i].production.candidateCount = 2U;
				for (uint32_t candidate = 0U; candidate < 2U; ++candidate)
				{
					rts::AIProductionCandidateFact &fact =
						snapshots[i].production.candidates[candidate];
					fact.sourceOrdinal = candidate + 1U;
					fact.candidateStableId = 100U + candidate;
					fact.configuredPriority = candidate == 1U ? -100 : -200;
					fact.minimumCost = 10;
					fact.plannedCost = 20;
					fact.eligible = 1U;
				}
				InitializeAIReferenceSourceFacts(&snapshots[i], 1U);
			}
		}

		const KernelPerformanceBatch timingBatch = timing.beginBatch(
			KERNEL_PERFORMANCE_AI, subtype, snapshots[0].frame, subtype + 1U);
		assert(timingBatch.valid());
		KernelPerformanceBatch mutableTimingBatch = timingBatch;
		const KernelPerformanceInterval capture = timing.beginInterval(
			mutableTimingBatch, KERNEL_PERFORMANCE_CAPTURE);
		assert(capture.valid());
		++clock.now;
		assert(timing.endInterval(capture));

		AIPlanningReferencePlayerInputView inputView = {
			snapshots.data(), snapshotCount, subtype};
		// The reference observer runs inside the shared VALIDATE scope, before
		// ExecuteAIPlanningBatch copies the accepted worker results into the
		// caller's committed buffer. Hash the validated parallel scratch itself.
		AIPlanningReferencePlayerOutputView outputView = {
			parallelScratch.data(), snapshotCount, subtype};
		std::vector<rts::AIPlayerPlanningResult> detached(snapshotCount);
		AIPlanningReferencePlayerOutputView detachedOutputView = {
			detached.data(), snapshotCount, subtype};
		KernelPerformanceReferenceBatch referenceBatch;
		rts::AIPlanningReferenceBatchTransport transport;
		transport.referenceLedger = &reference;
		transport.referenceBatch = &referenceBatch;
		transport.writeInput = WriteTestAIPlanningReferenceInput;
		transport.immutableInput = &inputView;
		transport.writeOutput = WriteTestAIPlanningReferenceOutput;
		transport.productionOutput = &outputView;
		transport.serialCompute = ComputeTestAIPlanningReferenceSerial;
		transport.detachedSerialOutput = &detachedOutputView;
		transport.operationCount = snapshotCount;
		transport.fieldSchema = 1U;

		rts::AIPlanningBatchStatus status;
		assert(rts::ExecuteAIPlanningBatchOnJobSystem(
			rts::AI_PLANNING_EXECUTION_PARALLEL, snapshots.data(), snapshotCount,
			committed.data(), serialScratch.data(), parallelScratch.data(), &status,
			&mutableTimingBatch,
			&transport));
		// The observer runs after worker validation but before the runner's
		// physical-authority decision. A short fixture may therefore validate a
		// real worker result and then take the documented serial fallback when
		// two workers did not overlap; that must not invalidate the receipt path.
		assert(status.ownerHelpedJobs == 0U);
		assert(status.distinctPhysicalWorkers > 0U);
		assert(status.peakConcurrentPhysicalWorkers > 0U);
		if (status.parallelSucceeded != 0U)
		{
			assert(status.usedSerialFallback == 0U);
			assert(status.committedMode == rts::AI_PLANNING_EXECUTION_PARALLEL);
			assert(status.distinctPhysicalWorkers > 1U);
			assert(status.peakConcurrentPhysicalWorkers > 1U);
		}
		else
			assert(status.usedSerialFallback == 1U);
		assert(referenceBatch.valid());
		assert(g_aiReferenceSerialComputes == subtype + 1U);
		const bool parallelAuthoritative = status.parallelSucceeded != 0U &&
			status.committedMode == rts::AI_PLANNING_EXECUTION_PARALLEL &&
			status.usedSerialFallback == 0U;
		assert(FinishAIPlanningReferenceBatch(&transport, parallelAuthoritative));
		if (parallelAuthoritative)
		{
			++clock.now;
			const KernelPerformanceInterval commit = timing.beginInterval(
				mutableTimingBatch, KERNEL_PERFORMANCE_COMMIT);
			assert(commit.valid());
			++clock.now;
			assert(timing.endInterval(commit));
			assert(timing.endBatch(mutableTimingBatch,
				KERNEL_PERFORMANCE_COMMITTED));
		}
		else
		{
			assert(timing.endBatch(mutableTimingBatch,
				KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION));
		}
	}

	const KernelPerformanceReferenceSnapshot referenceSnapshot = reference.freeze();
	assert(referenceSnapshot.complete && referenceSnapshot.streamCount == 2U);
	for (unsigned streamIndex = 0U; streamIndex < referenceSnapshot.streamCount;
		++streamIndex)
	{
		const KernelPerformanceReferenceStream &stream =
			referenceSnapshot.streams[streamIndex];
		assert(stream.fieldSchema == 1U);
		assert(stream.validatedBatchCount == 1U);
		assert(stream.committedBatchCount <= 1U);
		assert(stream.abortedBatchCount + stream.committedBatchCount == 1U);
		assert(stream.validatedOperationCount == snapshotCount);
		if (stream.committedBatchCount != 0U)
		{
			assert(stream.committedOperationCount == snapshotCount);
			assert(stream.serialSampleCount == 1U);
		}
		else
		{
			assert(stream.committedOperationCount == 0U);
			assert(stream.serialSampleCount == 0U);
		}
	}

	const KernelPerformanceSnapshot timingSnapshot = timing.freeze();
	assert(timingSnapshot.complete && timingSnapshot.streamCount == 2U);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestAIPlanningPerformanceTransport()
{
	using namespace rts::performance;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2U;
	config.queueCapacity = 64U;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));

	AIPlanningPerformanceTestClock clock;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	assert(ledger.beginRun(true, AIPlanningPerformanceTestClock::read, &clock));

	rts::AIPlayerPlanningSnapshot snapshots[2];
	rts::AIPlayerPlanningResult committed[2];
	rts::AIPlayerPlanningResult serialScratch[2];
	rts::AIPlayerPlanningResult parallelScratch[2];
	rts::AIPlanningBatchStatus status;
	KernelPerformanceBatch batch = ledger.beginBatch(
		KERNEL_PERFORMANCE_AI, 1U, 904U, 1U);
	assert(batch.valid());

	// The title owner begins capture before materializing its first snapshot.
	const KernelPerformanceInterval capture = ledger.beginInterval(
		batch, KERNEL_PERFORMANCE_CAPTURE);
	assert(capture.valid());
	InitializeSinglePlayerSourceShardBatch(&snapshots[0]);
	snapshots[1] = snapshots[0];
	snapshots[1].playerIndex = 2U;
	snapshots[1].production.ownerPlayerIndex = 2U;
	snapshots[1].production.tieBreakKey.playerIndex = 2U;
	++clock.now;
	assert(ledger.endInterval(capture));

	assert(rts::ExecuteAIPlanningBatchOnJobSystem(
		rts::AI_PLANNING_EXECUTION_PARALLEL, snapshots, 2U, committed,
		serialScratch, parallelScratch, &status, &batch));
	assert(status.parallelSucceeded == 1U);
	assert(status.usedSerialFallback == 0U);
	ObserveAIPlanningTestStage(ledger, batch, KERNEL_PERFORMANCE_COMMIT, clock);

	// Shared scheduling, passive wait, and result validation must be explicit
	// samples alongside the owner capture and commit intervals.
	assert(ledger.endBatch(batch, KERNEL_PERFORMANCE_COMMITTED));
	const KernelPerformanceSnapshot result = ledger.freeze();
	assert(result.complete && result.streamCount == 1U);
	const KernelPerformanceStream &stream = result.streams[0];
	for (unsigned stage = 0U; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		assert(stream.stageSamples[stage] >= 1U);

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

int g_nativeAIPhaseFailures = 0;
unsigned g_nativeAIPhaseVariant = 0;
bool g_nativeAIPhaseBaseline = false;
void NativeAIExpect(bool condition, const char *message)
{
	if (!condition)
	{
		++g_nativeAIPhaseFailures;
		std::cerr << "FAIL [AI " << (g_nativeAIPhaseBaseline ? "consumer" : "source") <<
			" variant=" << g_nativeAIPhaseVariant << "]: " << message << '\n';
	}
}

struct NativeAIObservations
{
	std::atomic<unsigned> entries[3]{};
	std::atomic<unsigned> prepared[33]{};
	std::atomic<unsigned> verification[33]{};
	std::atomic<unsigned> comparisons{0}, winners{0}, fallbacks{0};
	std::atomic<bool> wrongOwner{false};
	std::atomic<unsigned> held{0};
	std::atomic<bool> releaseHeld{false}, waitExpired{false};
	unsigned cancelNotifications = 0, releaseNotifications = 0, releasedCompleted = 0, releasedSubmitted = 0;
	unsigned releasedReason = 0;
	bool releasedCancelled = false;
	rts_test::NativeKernelClock *clock = 0;
	unsigned variant = 0;
	bool baseline = false;
	~NativeAIObservations() { releaseHeld.store(true, std::memory_order_release); }
	static void beforeWait(void *opaque)
	{
		auto &self = *static_cast<NativeAIObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongOwner = true;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
		while (self.held.load(std::memory_order_acquire) != 3 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (self.held.load(std::memory_order_acquire) != 3)
		{ self.waitExpired = true; self.releaseHeld.store(true, std::memory_order_release); }
	}
	static void afterCancel(void *opaque)
	{
		auto &self = *static_cast<NativeAIObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongOwner = true;
		++self.cancelNotifications;
		self.releaseHeld.store(true, std::memory_order_release);
	}
	static void releasedGroup(void *opaque, bool cancelled, unsigned completed, unsigned submitted, unsigned reason)
	{
		auto &self = *static_cast<NativeAIObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongOwner = true;
		++self.releaseNotifications; self.releasedCancelled = cancelled;
		self.releasedCompleted = completed; self.releasedSubmitted = submitted; self.releasedReason = reason;
	}
	static void observe(void *opaque, rts::AIPlanningTestEvent event,
		uint32_t player, uint32_t begin, uint32_t end,
		rts::AIProductionCandidateFact *verificationFact)
	{
		auto &self = *static_cast<NativeAIObservations *>(opaque);
		const bool owner = rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME);
		const bool body = event <= rts::AI_PLANNING_TEST_VERIFICATION_FACT;
		if (owner != (body ? self.baseline : true) || player != 0) self.wrongOwner = true;
		unsigned ticks = 41;
		if (event == rts::AI_PLANNING_TEST_RANGE_ENTRY)
		{
			if (begin % 16 != 0 || begin > 32 || end != (begin == 32 ? 33 : begin + 16))
				self.wrongOwner = true;
			else ++self.entries[begin / 16];
			ticks = 7;
		}
		else if (event == rts::AI_PLANNING_TEST_PREPARED_FACT || event == rts::AI_PLANNING_TEST_VERIFICATION_FACT)
		{
			if (begin >= 33 || end != begin + 1) self.wrongOwner = true;
			else if (event == rts::AI_PLANNING_TEST_PREPARED_FACT) ++self.prepared[begin];
			else
			{
				++self.verification[begin];
				if (self.variant == 2 && begin == 17 && verificationFact != 0)
					++verificationFact->minimumCost;
			}
			ticks = 3;
		}
		else if (event == rts::AI_PLANNING_TEST_OWNER_FACT_COMPARISON) ++self.comparisons;
		else if (event == rts::AI_PLANNING_TEST_OWNER_WINNER) ++self.winners;
		else if (event == rts::AI_PLANNING_TEST_SERIAL_FALLBACK) ++self.fallbacks;
		self.clock->now.fetch_add(ticks);
		if (!self.baseline && self.variant == 3 && event == rts::AI_PLANNING_TEST_VERIFICATION_FACT &&
			(end == 16 || end == 32 || end == 33))
		{
			// All original entry polls already happened. Hold the final real fact
			// derivation until the owner has actually cancelled the group.
			self.held.fetch_add(1, std::memory_order_release);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
			while (!self.releaseHeld.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
			if (!self.releaseHeld.load(std::memory_order_acquire))
			{ self.waitExpired = true; self.releaseHeld.store(true, std::memory_order_release); }
		}
	}
	static bool cancel(void *opaque, uint32_t, uint32_t begin, uint32_t)
	{
		const auto &self = *static_cast<NativeAIObservations *>(opaque);
		// The baseline's current predicate is deliberately opposite. Only the
		// authenticated source checkpoint, not a new deadline, selects its cut.
		return self.baseline ? self.variant != 1 : self.variant == 1 && begin == 16;
	}
};

bool RunNativeAIPhaseRole(rts_test::NativeKernelTrace &trace, bool baseline, unsigned variant)
{
	using namespace rts::performance;
	g_nativeAIPhaseVariant = variant; g_nativeAIPhaseBaseline = baseline;
	const int failuresBefore = g_nativeAIPhaseFailures;
	std::cerr << "BEGIN [AI " << (baseline ? "consumer" : "source") << " variant=" << variant << "]\n";
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4; config.queueCapacity = 64;
	config.scratchBytesPerWorker = 4096; config.pinWorkers = false;
	if (!jobs.start(config) || !jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{ NativeAIExpect(false, "native AI phase fixture starts four workers"); return false; }
	rts_test::NativeKernelOwnerRun run;
	const bool started = run.begin(trace, baseline, 904, KERNEL_PHASE_OWNER_INTAKE);
	NativeAIExpect(started, "native AI role validates its real ledger and source binding");
	if (!started) { jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false; }
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_AI, 1);
	NativeAIExpect(attempt.valid(), "AI core owner opens authentic attempt before capture");
	auto timingBatch = run.timing.beginBatch(KERNEL_PERFORMANCE_AI, 1, 904, 1);
	const auto capture = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_CAPTURE);
	std::vector<rts::AIPlayerPlanningSnapshot> snapshots(1);
	std::vector<rts::AIPlayerPlanningResult> committed(1), serial(1), parallel(1), detached(1);
	InitializeSinglePlayerSourceShardBatch(snapshots.data());
	snapshots[0].production.candidateCount = 33;
	for (unsigned index = 0; index != 33; ++index)
		snapshots[0].production.candidates[index].configuredPriority = index == 0 ? 200 : 100;
	run.clock.now.fetch_add(5);
	NativeAIExpect(run.timing.endInterval(capture), "AI capture closes before native executor");
	rts::AIPlanningReferencePlayerInputView input = {snapshots.data(), 1, 1};
	rts::AIPlanningReferencePlayerOutputView output = {parallel.data(), 1, 1};
	rts::AIPlanningReferencePlayerOutputView oracle = {detached.data(), 1, 1};
	KernelPerformanceReferenceBatch validated;
	rts::AIPlanningReferenceBatchTransport transport;
	transport.referenceLedger = &run.reference; transport.referenceAttempt = attempt;
	transport.referenceBatch = &validated; transport.writeInput = rts::WriteAIPlanningReferenceInput;
	transport.immutableInput = &input; transport.writeOutput = rts::WriteAIPlanningReferenceOutput;
	transport.productionOutput = &output; transport.serialCompute = ComputeTestAIPlanningReferenceSerial;
	transport.detachedSerialOutput = &oracle; transport.operationCount = 1;
	NativeAIObservations observed;
	observed.clock = &run.clock; observed.variant = variant; observed.baseline = baseline;
	std::atomic<uint32_t> rendezvous(0);
	rts::AIPlanningTestHooks hooks;
	hooks.context = &observed; hooks.observe = NativeAIObservations::observe;
	hooks.cancelAtEntry = NativeAIObservations::cancel;
	// This controlled source proves body semantics, not ordinary 8-ms admission.
	hooks.rendezvous = baseline || variant == 3 ? 0 : &rendezvous; hooks.rendezvousTarget = 3;
	if (variant == 3)
	{
		hooks.beforeWait = NativeAIObservations::beforeWait;
		hooks.afterCancel = NativeAIObservations::afterCancel;
		hooks.releasedGroup = NativeAIObservations::releasedGroup;
	}
	transport.testHooks = &hooks;
	const unsigned detachedBefore = g_aiReferenceSerialComputes;
	rts::AIPlanningBatchStatus status;
	const bool executed = rts::ExecuteAIPlanningBatchOnJobSystem(rts::AI_PLANNING_EXECUTION_PARALLEL,
		snapshots.data(), 1, committed.data(), serial.data(), parallel.data(), &status,
		&timingBatch, &transport);
	NativeAIExpect(executed && committed[0].production.valid == 1 &&
		committed[0].production.selectedSourceOrdinal == 0 && committed[0].production.selectedStableId == 1000 &&
		committed[0].production.tieCount == 1 && committed[0].production.candidateScoreCount == 33,
		"real shared AI publishes literal unique winner zero even after serial fallback");
	NativeAIExpect(status.usedSerialFallback == (variant == 0 ? 0U : 1U),
		"predeclared AI success or abort is retained without retry");
	NativeAIExpect(status.nativeAdmissionAccepted == 1U,
		"AI status preserves authentic accepted-range admission for owner disposition");
	for (unsigned range = 0; range != 3; ++range)
		NativeAIExpect(observed.entries[range] == 1, "native AI enters each source-shaped shard once");
	for (unsigned index = 0; index != 33; ++index)
	{
		const unsigned expected = variant == 1 && index >= 16 && index < 32 ? 0 : 1;
		NativeAIExpect(observed.prepared[index] == expected && observed.verification[index] == expected,
			"native AI builds both distinct fact-table entries once at the recorded prefix");
	}
	NativeAIExpect(observed.comparisons == (variant == 1 || variant == 3 ? 0U : 1U) &&
		observed.winners == (variant == 0 ? 1U : 0U) &&
		observed.fallbacks == (variant == 0 ? 0U : 1U) && !observed.wrongOwner,
		"AI owner comparison rejects corrupted verification before winner reduction");
	NativeAIExpect(g_aiReferenceSerialComputes == detachedBefore, "native source and baseline never invoke detached AI oracle");
	if (variant == 3 && !baseline)
	{
		NativeAIExpect(!observed.waitExpired && observed.held == 3 && observed.cancelNotifications == 1 &&
			observed.releaseNotifications == 1 && observed.releasedCancelled &&
			observed.releasedCompleted == 3 && observed.releasedSubmitted == 3,
			"AI native timeout cancels a real group then drains three late-completed bodies");
		NativeAIExpect(observed.releasedReason == 3,
			"AI source reason records actual group cancellation independently of completed checkpoints");
	}
	if (variant == 3 && baseline)
		NativeAIExpect(observed.held == 0 && observed.cancelNotifications == 0 && observed.releaseNotifications == 0,
			"AI baseline replays late disposal without waiting or issuing a new group cancellation");
	const auto actualScheduler = rts_test::NativeKernelSchedulerBoundary();
	if (baseline)
		NativeAIExpect(actualScheduler.submittedJobs == 0 && actualScheduler.executedJobs == 0 &&
			actualScheduler.ownerHelpJobs == 0 && status.physicalWorkerMask == 0 &&
			status.distinctPhysicalWorkers == 0 && status.peakConcurrentPhysicalWorkers == 0 &&
			status.parallelSucceeded == 0, "AI baseline publishes no fabricated physical-worker authority");
	else
		NativeAIExpect(actualScheduler.submittedJobs == 3 && actualScheduler.executedJobs == 3 &&
			status.ownerHelpedJobs == 0 && (variant != 0 || status.parallelSucceeded == 1),
			"AI controlled source is the real three-job native lane");
	const bool accepted = executed && status.usedSerialFallback == 0;
	const auto linked = validated;
	rts::RecordAIPlanningOwnerCommit(executed, &status);
	if (linked.valid()) NativeAIExpect(rts::FinishAIPlanningReferenceBatch(&transport, accepted),
		"actual core owner closes canonical AI publication before attempt finish");
	NativeAIExpect(linked.valid() == accepted, "AI native helper links only actually validated output to its attempt");
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = accepted ? KERNEL_PERFORMANCE_COMMITTED : KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1; finish.reason = accepted ? 1 : 2;
	finish.fallbackEntered = finish.fallbackCompleted = executed && status.usedSerialFallback != 0;
	finish.validatedBatch = linked;
	NativeAIExpect(run.reference.finishAttempt(attempt, finish), "AI actual outcome closes authentic source attempt");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	reap.pendingJobs = actualScheduler.pendingJobs; reap.outstandingJobs = actualScheduler.outstandingJobs;
	NativeAIExpect(run.reference.reapAttempt(attempt, reap), "AI source attempt reaps only after real synchronous drain");
	if (accepted)
	{
		const auto commit = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_COMMIT);
		run.clock.now.fetch_add(11); run.timing.endInterval(commit);
	}
	NativeAIExpect(run.timing.endBatch(timingBatch, finish.disposition), "AI timing records actual admission outcome");
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.closeTiming(actualScheduler);
	NativeAIExpect(timingClosed, "AI owner phase and scheduler closure reconcile");
	if (baseline && timingClosed)
	{
		const auto &phase = run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_OWNER_INTAKE];
		NativeAIExpect(phase.pureNanoseconds == (variant == 0 ? 219U : 0U) && phase.serialNanoseconds >= 16,
			"only three successful AI shards are pure; comparison winner and aborted work stay serial");
	}
	const bool canonicalState = variant == 0 ? snapshot.complete && snapshot.streamCount == 1 :
		!snapshot.complete && snapshot.streamCount == 0;
	NativeAIExpect(sealed && canonicalState && snapshot.errors == 0 && snapshot.trace.complete && snapshot.trace.attemptCount == 1 &&
		snapshot.trace.capturedAttemptCount == 1 && snapshot.trace.dispatchCount == 1 &&
		snapshot.trace.rangeCount == 3 && snapshot.trace.releasedRangeCount == 3 && snapshot.trace.reapCount == 1,
		"actual AI native executor records complete admitted dispatch and released-range evidence");
	if (!baseline) trace.source = snapshot;
	else if (snapshot.complete && variant == 0)
		NativeAIExpect(snapshot.streams[0].outputDigest.equals(trace.source.streams[0].outputDigest),
			"AI baseline binds the output from its once-only native bodies to source");
	jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	std::cerr << "END [AI " << (baseline ? "consumer" : "source") << " variant=" << variant <<
		"] failures=" << g_nativeAIPhaseFailures - failuresBefore << " traceComplete=" << snapshot.trace.complete << '\n';
	return canonicalState && snapshot.errors == 0 && snapshot.trace.complete &&
		(variant != 3 || baseline || observed.releasedReason == 3);
}

void TestNativeAIActualSourceConsumer()
{
	// Breaks caught: omitted attempts, one fact table reused as its own oracle,
	// detached re-execution, physical baseline jobs, wrong cut, or pure reduction.
	for (unsigned variant = 0; variant != 4; ++variant)
	{
		rts_test::NativeKernelTrace trace(20 + variant);
		const bool sourceComplete = RunNativeAIPhaseRole(trace, false, variant);
		// No fabricated source and no favorable retry: missing source evidence is
		// already a failure above; consumer execution requires its valid bytes.
		if (sourceComplete) RunNativeAIPhaseRole(trace, true, variant);
	}
}
#endif

void TestBatchSerialParityAndFaults()
{
	std::vector<rts::AIPlayerPlanningSnapshot> snapshots(16U);
	InitializeProductionPlanningBatch(&snapshots);
	const uint32_t requestedCounts[] = { 1U, 2U, 4U, 8U, 16U };
	for (uint32_t countIndex = 0;
		countIndex < sizeof(requestedCounts) / sizeof(requestedCounts[0]);
		++countIndex)
	{
		const uint32_t count = requestedCounts[countIndex];
		std::vector<rts::AIPlayerPlanningResult> oracle(count);
		std::vector<rts::AIPlayerPlanningResult> serial(count);
		std::vector<rts::AIPlayerPlanningResult> scratch(count);
		std::vector<rts::AIPlayerPlanningResult> parallel(count);
		rts::AIPlanningBatchStatus status;
		assert(rts::PlanAIPlayerBatchSerial(
			&snapshots[0], count, &oracle[0]));
		assert(rts::ExecuteAIPlanningBatch(
			rts::AI_PLANNING_EXECUTION_SERIAL, &snapshots[0], count,
			&serial[0], &scratch[0], &parallel[0], 0, 0, &status));
		for (uint32_t i = 0; i < count; ++i)
			assert(rts::EqualAIPlayerPlanningResult(serial[i], oracle[i]));

		uint32_t corruptOrdinal = count - 1U;
		assert(rts::ExecuteAIPlanningBatch(
			rts::AI_PLANNING_EXECUTION_PARALLEL, &snapshots[0], count,
			&serial[0], &scratch[0], &parallel[0], CorruptBatchAtOrdinal,
			&corruptOrdinal, &status));
		assert(status.parallelSucceeded == 0U);
		assert(status.usedSerialFallback == 1U);
		for (uint32_t i = 0; i < count; ++i)
			assert(rts::EqualAIPlayerPlanningResult(serial[i], oracle[i]));
	}
}

void TestCanonicalValidationInvocationCount()
{
	std::vector<rts::AIPlayerPlanningSnapshot> snapshots(2U);
	InitializeProductionPlanningBatch(&snapshots);
	rts::AIPlayerPlanningResult committed[2];
	rts::AIPlayerPlanningResult serial[2];
	rts::AIPlayerPlanningResult parallel[2];
	rts::AIPlanningBatchStatus status;
	rts::ResetAIPlanningRuntimeMetrics();
	assert(rts::ExecuteAIPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, &snapshots[0], 2U,
		committed, serial, parallel, RunBatch, 0, &status));
	assert(rts::GetAIPlanningRuntimeMetrics().canonicalValidationInvocations == 2U);

	// Shadow compares the same already-validated worker result against its
	// serial oracle; it must not add a second post-commit batch validation.
	rts::ResetAIPlanningRuntimeMetrics();
	assert(rts::ExecuteAIPlanningBatch(
		rts::AI_PLANNING_EXECUTION_SHADOW, &snapshots[0], 2U,
		committed, serial, parallel, RunBatch, 0, &status));
	assert(rts::GetAIPlanningRuntimeMetrics().canonicalValidationInvocations == 2U);
}

void TestPhysicalIdentityAndPeakByBatchShape(bool localCapacity)
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = rts_test::ResolveActualWorkerCount(16U,
		localCapacity);
	rts_test::PrintWorkerCountSubstitution(
		"Deterministic AI planning", 16U, config.workerCount,
		localCapacity);
	config.queueCapacity = 64U;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));

	std::vector<rts::AIPlayerPlanningSnapshot> snapshots(16U);
	InitializeProductionPlanningBatch(&snapshots);
	const uint32_t requestedCounts[] = { 1U, 2U, 4U, 8U, 16U };
	for (uint32_t countIndex = 0;
		countIndex < sizeof(requestedCounts) / sizeof(requestedCounts[0]);
		++countIndex)
	{
		const uint32_t count = requestedCounts[countIndex];
		std::vector<rts::AIPlayerPlanningResult> committed(count);
		std::vector<rts::AIPlayerPlanningResult> serial(count);
		std::vector<rts::AIPlayerPlanningResult> parallel(count);
		rts::AIPlanningBatchStatus status;
		assert(rts::ExecuteAIPlanningBatchOnJobSystem(
			rts::AI_PLANNING_EXECUTION_PARALLEL, &snapshots[0], count,
			&committed[0], &serial[0], &parallel[0], &status));
		if (count == 1U)
			assert(jobs.metrics().submittedJobCount == 0U);

		for (uint32_t i = 0; i < count; ++i)
			assert(rts::ValidateAIPlayerPlanningResult(
				snapshots[i], committed[i]));
		uint32_t observedPhysicalWorkers = 0U;
		for (uint32_t worker = 0; worker < 64U; ++worker)
			if ((status.physicalWorkerMask & (uint64_t(1U) << worker)) != 0U)
				++observedPhysicalWorkers;
		assert(observedPhysicalWorkers == status.distinctPhysicalWorkers);
		assert(status.distinctPhysicalWorkers <= count);
		assert(status.peakConcurrentPhysicalWorkers <= count);
		assert(status.peakConcurrentPhysicalWorkers <= jobs.workerCount());
		if (status.parallelSucceeded != 0U)
		{
			assert(status.usedSerialFallback == 0U);
			assert(status.ownerHelpedJobs == 0U);
			assert(status.distinctPhysicalWorkers > 1U);
			assert(status.peakConcurrentPhysicalWorkers > 1U);
		}
		else
		{
			assert(status.usedSerialFallback == 1U);
		}
	}

	// A single due AI with a large immutable source view still supplies sixteen
	// meaningful shard jobs. It must be admitted without changing the canonical
	// serial answer, and any authoritative result must prove physical workers.
	std::vector<rts::AIPlayerPlanningSnapshot> sourceSnapshots(1U);
	InitializeSinglePlayerSourceShardBatch(&sourceSnapshots[0]);
	rts::AIPlayerPlanningResult sourceOracle[1];
	rts::AIPlayerPlanningResult sourceCommitted[1];
	rts::AIPlayerPlanningResult sourceSerial[1];
	rts::AIPlayerPlanningResult sourceParallel[1];
	assert(rts::PlanAIPlayerBatchSerial(&sourceSnapshots[0], 1U, sourceOracle));
	const uint64_t submittedBefore = jobs.metrics().submittedJobCount;
	rts::AIPlanningBatchStatus sourceStatus;
	assert(rts::ExecuteAIPlanningBatchOnJobSystem(
		rts::AI_PLANNING_EXECUTION_PARALLEL, &sourceSnapshots[0], 1U,
		sourceCommitted, sourceSerial, sourceParallel, &sourceStatus));
	assert(jobs.metrics().submittedJobCount - submittedBefore ==
		rts::AI_PLANNING_MAX_PRODUCTION_SOURCE_JOBS);
	assert(rts::EqualAIPlayerPlanningResult(sourceCommitted[0], sourceOracle[0]));
	assert(sourceStatus.ownerHelpedJobs == 0U);
	if (sourceStatus.parallelSucceeded != 0U)
	{
		assert(sourceStatus.distinctPhysicalWorkers > 1U);
		assert(sourceStatus.peakConcurrentPhysicalWorkers > 1U);
	}
	else
	{
		assert(sourceStatus.usedSerialFallback == 1U);
	}

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}

void TestTransactionalShadowFallback()
{
	rts::AIPlayerPlanningSnapshot snapshot;
	rts::ClearAIPlayerPlanningSnapshot(&snapshot);
	snapshot.frame = 900U;
	snapshot.playerIndex = 3U;
	snapshot.planProduction = 1U;
	snapshot.production.frame = 900U;
	snapshot.production.ownerPlayerIndex = 3U;
	snapshot.production.difficulty = rts::AI_PLANNING_DIFFICULTY_EASY;
	snapshot.production.contextInfluencePercent = 100;
	snapshot.production.candidateCount = 2U;
	snapshot.production.candidates[0].sourceOrdinal = 0U;
	snapshot.production.candidates[0].candidateStableId = 42U;
	snapshot.production.candidates[0].configuredPriority = 10;
	snapshot.production.candidates[0].counterFitScore = 300;
	snapshot.production.candidates[0].eligible = 1U;
	snapshot.production.candidates[1] = snapshot.production.candidates[0];
	snapshot.production.candidates[1].sourceOrdinal = 1U;
	snapshot.production.candidates[1].candidateStableId = 43U;
	snapshot.production.candidates[1].counterFitScore = 0;

	rts::AIPlayerPlanningResult committed[1];
	rts::AIPlayerPlanningResult serial[1];
	rts::AIPlayerPlanningResult parallel[1];
	rts::AIPlanningBatchStatus status;
	assert(rts::ExecuteAIPlanningBatch(rts::AI_PLANNING_EXECUTION_SHADOW,
		&snapshot, 1U, committed, serial, parallel, RunBatch, 0, &status));
	assert(status.shadowMatched == 1U);
	assert(status.usedSerialFallback == 0U);
	assert(committed[0].production.selectedStableId == 42U);

	int corrupt = 1;
	assert(rts::ExecuteAIPlanningBatch(rts::AI_PLANNING_EXECUTION_SHADOW,
		&snapshot, 1U, committed, serial, parallel, RunBatch, &corrupt, &status));
	assert(status.parallelSucceeded == 0U);
	assert(status.shadowMatched == 0U);
	assert(status.usedSerialFallback == 1U);
	assert(status.mismatchPlayerOrdinal == rts::AI_PLANNING_INVALID_ORDINAL);
	assert(committed[0].production.selectedStableId == 42U);

	assert(rts::ExecuteAIPlanningBatch(rts::AI_PLANNING_EXECUTION_PARALLEL,
		&snapshot, 1U, committed, serial, parallel, FailBatch, 0, &status));
	assert(status.parallelSucceeded == 0U);
	assert(status.usedSerialFallback == 1U);
	assert(committed[0].production.selectedStableId == 42U);

	// A runner is untrusted until its entire fixed-capacity result contract is
	// validated. Oversized counts and forged selection ordinals must fall back
	// before shadow comparison can index result arrays.
	assert(rts::ExecuteAIPlanningBatch(rts::AI_PLANNING_EXECUTION_SHADOW,
		&snapshot, 1U, committed, serial, parallel, MalformedCountBatch, 0, &status));
	assert(status.parallelSucceeded == 0U);
	assert(status.usedSerialFallback == 1U);
	assert(committed[0].production.selectedSourceOrdinal == 0U);
	assert(committed[0].production.selectedStableId == 42U);
	parallel[0] = committed[0];
	parallel[0].production.candidateScoreCount =
		rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES + 1U;
	assert(!rts::EqualAIPlayerPlanningResult(committed[0], parallel[0]));

	assert(rts::ExecuteAIPlanningBatch(rts::AI_PLANNING_EXECUTION_PARALLEL,
		&snapshot, 1U, committed, serial, parallel, MalformedSelectionBatch, 0, &status));
	assert(status.parallelSucceeded == 0U);
	assert(status.usedSerialFallback == 1U);
	assert(committed[0].production.selectedSourceOrdinal == 0U);
	assert(committed[0].production.selectedStableId == 42U);

	assert(rts::ExecuteAIPlanningBatch(rts::AI_PLANNING_EXECUTION_PARALLEL,
		&snapshot, 1U, committed, serial, parallel, ForgedEligibleWinnerBatch, 0, &status));
	assert(status.parallelSucceeded == 0U);
	assert(status.usedSerialFallback == 1U);
	assert(committed[0].production.selectedSourceOrdinal == 0U);
	assert(committed[0].production.selectedStableId == 42U);
}

void TestModeSpecificOwnerCommitAuthorityMetrics()
{
	rts::ResetAIPlanningRuntimeMetrics();
	rts::AIPlanningBatchStatus status;
	status.requestedMode = rts::AI_PLANNING_EXECUTION_SERIAL;
	status.committedMode = rts::AI_PLANNING_EXECUTION_SERIAL;
	status.parallelSucceeded = 0U;
	status.shadowMatched = 0U;
	status.usedSerialFallback = 0U;
	status.mismatchPlayerOrdinal = rts::AI_PLANNING_INVALID_ORDINAL;
	status.physicalWorkerMask = 0U;
	status.distinctPhysicalWorkers = 0U;
	status.peakConcurrentPhysicalWorkers = 0U;
	status.ownerHelpedJobs = 0U;
	rts::RecordAIPlanningOwnerCommit(true, &status);

	status.requestedMode = rts::AI_PLANNING_EXECUTION_SHADOW;
	status.committedMode = rts::AI_PLANNING_EXECUTION_PARALLEL;
	status.parallelSucceeded = 1U;
	status.shadowMatched = 1U;
	rts::RecordAIPlanningOwnerCommit(true, &status);

	status.requestedMode = rts::AI_PLANNING_EXECUTION_PARALLEL;
	status.committedMode = rts::AI_PLANNING_EXECUTION_SERIAL;
	status.usedSerialFallback = 1U;
	rts::RecordAIPlanningOwnerCommit(true, &status);

	rts::RecordAIPlanningOwnerCommit(true);
	rts::AIPlanningRuntimeMetrics metrics = rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.committedBatches == 4U);
	assert(metrics.parallelAuthoritativeCommits == 0U);

	status.committedMode = rts::AI_PLANNING_EXECUTION_PARALLEL;
	status.usedSerialFallback = 0U;
	status.ownerHelpedJobs = 2U;
	rts::RecordAIPlanningOwnerCommit(true, &status);
	metrics = rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.committedBatches == 5U);
	assert(metrics.parallelAuthoritativeCommits == 0U);

	status.ownerHelpedJobs = 0U;
	status.physicalWorkerMask = 3U;
	status.distinctPhysicalWorkers = 2U;
	status.peakConcurrentPhysicalWorkers = 2U;
	rts::RecordAIPlanningOwnerCommit(true, &status);
	rts::RecordAIPlanningOwnerCommit(false, &status);
	metrics = rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.committedBatches == 6U);
	assert(metrics.parallelAuthoritativeCommits == 1U);
	assert(metrics.rejectedCommits == 1U);
}

void TestRealJobSystemRunner()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2U;
	config.queueCapacity = 64U;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	rts::ResetAIPlanningRuntimeMetrics();
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	// Workers were created with the process defaults. The AI runner must capture
	// this later owner-only state and restore it after every physical job.
	const unsigned savedMxcsr = _mm_getcsr();
	const unsigned ownerMxcsr =
		(savedMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_UP;
	_mm_setcsr(ownerMxcsr);
#endif

	rts::AIPlayerPlanningSnapshot snapshots[2];
	for (uint32_t i = 0; i < 2U; ++i)
	{
		rts::ClearAIPlayerPlanningSnapshot(&snapshots[i]);
		snapshots[i].frame = 901U;
		snapshots[i].playerIndex = i + 2U;
		snapshots[i].planEnemyTarget = 1U;
		snapshots[i].enemyTarget.frame = snapshots[i].frame;
		snapshots[i].enemyTarget.ownerPlayerIndex = snapshots[i].playerIndex;
		snapshots[i].enemyTarget.candidateCount = 1U;
		snapshots[i].enemyTarget.candidates[0].sourceOrdinal = 7U - i;
		snapshots[i].enemyTarget.candidates[0].playerIndex = 7 - (int32_t)i;
		snapshots[i].enemyTarget.candidates[0].hasKnownObject = 1U;
		snapshots[i].enemyTarget.candidates[0].hasKnownUnit = 1U;
	}

	rts::AIPlayerPlanningResult committed[2];
	rts::AIPlayerPlanningResult serial[2];
	rts::AIPlayerPlanningResult parallel[2];
	rts::AIPlanningBatchStatus status;
	assert(rts::ExecuteAIPlanningBatchOnJobSystem(
		rts::AI_PLANNING_EXECUTION_PARALLEL, snapshots, 2U,
		committed, serial, parallel, &status));
	const bool physicallyParallel = status.parallelSucceeded == 1U &&
		status.usedSerialFallback == 0U;
	if (physicallyParallel)
	{
		assert(status.ownerHelpedJobs == 0U);
		assert(status.distinctPhysicalWorkers > 1U);
		assert(status.peakConcurrentPhysicalWorkers > 1U);
	}
	else
	{
		assert(status.usedSerialFallback == 1U);
		assert(status.distinctPhysicalWorkers <= 1U ||
			status.peakConcurrentPhysicalWorkers <= 1U ||
			status.ownerHelpedJobs != 0U);
	}
	assert(committed[0].enemyTarget.selectedPlayerIndex == 7);
	assert(committed[1].enemyTarget.selectedPlayerIndex == 6);
	rts::RecordAIPlanningOwnerCommit(true, &status);
	rts::AIPlanningRuntimeMetrics metrics = rts::GetAIPlanningRuntimeMetrics();
	assert(metrics.requestedBatches == 1U);
	assert(metrics.submittedJobs == 2U);
	assert(metrics.completedJobs == 2U);
	assert(metrics.committedBatches == 1U);
	assert(metrics.physicalWorkerExecutions <= 2U);
	if (physicallyParallel)
		assert(metrics.physicalWorkerExecutions == 2U);
	assert(metrics.ownerHelpedExecutions == 0U);
	assert(metrics.parallelAuthoritativeCommits ==
		(physicallyParallel ? 1U : 0U));
	assert(metrics.maximumDistinctPhysicalWorkers ==
		status.distinctPhysicalWorkers);
	assert(metrics.maximumConcurrentPhysicalWorkers ==
		status.peakConcurrentPhysicalWorkers);
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	assert((_mm_getcsr() & ~0x3fU) == (ownerMxcsr & ~0x3fU));
	_mm_setcsr(savedMxcsr);
#endif

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
}
}

int main(int argc, char **argv)
{
#if defined(_MSC_VER)
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	bool localCapacity = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity))
	{
		std::cerr << "Usage: core_deterministic_ai_planning_tests "
			"[--local-capacity|--external-qualification]\n";
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	TestCounterRngGoldenVector();
	TestEnemyScoringAndHysteresis();
	TestProductionScoringTieAndRetry();
	TestProductionWinnerAndTieForgeryRejection();
	TestProductionOwnerSerialOverflowBoundary();
	TestAIOwnerProductionBookkeeping();
	TestProductionDueExpiryBoundary();
	TestProductionSourceViewParity();
	TestTransactionalShadowFallback();
	TestBatchSerialParityAndFaults();
	TestCanonicalValidationInvocationCount();
	TestModeSpecificOwnerCommitAuthorityMetrics();
	TestRealJobSystemRunner();
	TestPhysicalIdentityAndPeakByBatchShape(localCapacity);
#if defined(_WIN64)
	TestAIPlanningReferenceTransport();
	TestAIPlanningPerformanceTransport();
	TestNativeAIActualSourceConsumer();
	if (g_nativeAIPhaseFailures != 0) return 1;
#endif
	std::cout << "Deterministic AI planning tests passed.\n";
	return 0;
}
