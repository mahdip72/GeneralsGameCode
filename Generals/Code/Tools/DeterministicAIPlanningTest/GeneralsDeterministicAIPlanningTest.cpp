/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#if defined(NDEBUG)
#undef NDEBUG
#endif

#include "Utility/CppMacros.h"
#include "Common/SkirmishAIReplayEpoch.h"
#include "Common/GeneralsPathfindingReplayEpoch.h"
#include "GameLogic/GeneralsAIPlanningRuntime.h"
#include "GameLogic/GeneralsAIReplayPolicy.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <type_traits>
#if defined(_WIN64)
#include <chrono>
#include "../../../../Core/Tools/TestSupport/NativeKernelSourceConsumerTest.h"
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

static_assert(std::is_standard_layout_v<GeneralsAIEnemyCandidateFact>);
static_assert(std::is_trivially_copyable_v<GeneralsAIEnemyCandidateFact>);
static_assert(std::is_standard_layout_v<GeneralsAIEnemyPlanningSnapshot>);
static_assert(std::is_trivially_copyable_v<GeneralsAIEnemyPlanningSnapshot>);
static_assert(std::is_standard_layout_v<GeneralsAIEnemyPlanningResult>);
static_assert(std::is_trivially_copyable_v<GeneralsAIEnemyPlanningResult>);

namespace
{
const float kRetailEnemyThreshold = 1.0e12f;

void TestGeneralsPathfindingReplayEpochContract()
{
	const WideChar *current =
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]";
	assert(GetGeneralsPathfindingReplayEpoch(current) ==
		GENERALS_PATHFINDING_REPLAY_EPOCH_CURRENT);
	assert(GetSkirmishAIReplayEpoch(current) == SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	const WideChar *legacy[] =
	{
		NULL, L"", L"build",
		L"build [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=1]",
		L"build [GeneralsAIPlanningEpoch=1] [GeneralsPathfindingEpoch=1]",
		L"build [GeneralsPathfindingEpoch=1][GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=1] middle [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] ",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] trailing",
		L"build [GeneralsPathfindingEpoch=0] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=2] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=01] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=-1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=bogus] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=0]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=2]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=01]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=2] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch:1] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=bogus] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsAIPlanningEpoch=1] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsAIPlanningEpoch=2] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsAIPlanningEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsAIPlanningEpoch=bogus] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] [GeneralsPathfindingEpoch]",
		L"build [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] [GeneralsAIPlanningEpoch]",
		L"build [PathfindQueueEpoch=1] [GeneralsAIPlanningEpoch=1]"
	};
	for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i)
		assert(GetGeneralsPathfindingReplayEpoch(legacy[i]) ==
			GENERALS_PATHFINDING_REPLAY_EPOCH_LEGACY);
	// Adding the path contract must not reinterpret existing AI-only recordings.
	assert(GetSkirmishAIReplayEpoch(L"build [GeneralsAIPlanningEpoch=1]") ==
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT);

	const Int localModes[] = { GAME_SINGLE_PLAYER, GAME_SKIRMISH };
	for (unsigned i = 0; i < sizeof(localModes) / sizeof(localModes[0]); ++i)
	{
		assert(GetGeneralsPathfindingRecordingEpoch(localModes[i], true, false, 1) == 1);
		assert(GetGeneralsPathfindingRecordingEpoch(localModes[i], false, false, 1) == 0);
		assert(GetGeneralsPathfindingRecordingEpoch(localModes[i], true, true, 1) == 0);
		assert(GetGeneralsPathfindingRecordingEpoch(localModes[i], true, false, 0) == 0);
		assert(GetGeneralsPathfindingRecordingEpoch(localModes[i], true, false, 2) == 0);
		assert(GetGeneralsPathfindingPlaybackEpoch(current, localModes[i], true) == 1);
		assert(GetGeneralsPathfindingPlaybackEpoch(current, localModes[i], false) == 0);
		for (unsigned j = 0; j < sizeof(legacy) / sizeof(legacy[0]); ++j)
			assert(GetGeneralsPathfindingPlaybackEpoch(legacy[j], localModes[i], true) == 0);
	}
	const Int otherModes[] =
		{ GAME_LAN, GAME_INTERNET, GAME_REPLAY, GAME_SHELL, GAME_NONE, -1, 999 };
	for (unsigned i = 0; i < sizeof(otherModes) / sizeof(otherModes[0]); ++i)
	{
		assert(GetGeneralsPathfindingRecordingEpoch(otherModes[i], true, false, 1) == 0);
		assert(GetGeneralsPathfindingPlaybackEpoch(current, otherModes[i], true) == 0);
	}
	for (Int pathEpoch = -1; pathEpoch <= 2; ++pathEpoch)
		for (Int aiEpoch = -1; aiEpoch <= 2; ++aiEpoch)
			assert(HasCurrentGeneralsPathfindingReplayEpoch(pathEpoch, aiEpoch) ==
				(pathEpoch == 1 && aiEpoch == 1));
}

void TestTopologyAndReplayGates()
{
	assert(GetGeneralsAIPlanningExecutionMode(false,
		rts::SIMULATION_EXECUTION_PARALLEL, 2U, true) ==
		rts::AI_PLANNING_EXECUTION_PARALLEL);
	assert(GetGeneralsAIPlanningExecutionMode(false,
		rts::SIMULATION_EXECUTION_SHADOW, 2U, true) ==
		rts::AI_PLANNING_EXECUTION_SHADOW);
	assert(GetGeneralsAIPlanningExecutionMode(false,
		rts::SIMULATION_EXECUTION_SERIAL, 2U, true) ==
		rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(GetGeneralsAIPlanningExecutionMode(true,
		rts::SIMULATION_EXECUTION_PARALLEL, 2U, true) ==
		rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(GetGeneralsAIPlanningExecutionMode(false,
		rts::SIMULATION_EXECUTION_PARALLEL, 1U, true) ==
		rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(GetGeneralsAIPlanningExecutionMode(false,
		rts::SIMULATION_EXECUTION_PARALLEL, 2U, false) ==
		rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(!ShouldRunGeneralsAIPlanning(rts::AI_PLANNING_EXECUTION_SERIAL));

	assert(GetSkirmishAIReplayEpoch(L"retail build time") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	assert(GetSkirmishAIReplayEpoch(
		L"retail build time [GeneralsAIPlanningEpoch=1]") ==
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	assert(GetSkirmishAIReplayEpoch(
		L"retail [GeneralsAIPlanningEpoch=2]") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	assert(GetSkirmishAIReplayEpoch(
		L"retail [GeneralsAIPlanningEpoch=1] trailing") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	assert(GetSkirmishAIReplayEpoch(
		L"retail [GeneralsAIPlanningEpoch=1] [GeneralsAIPlanningEpoch=1]") ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	assert(!ShouldUseSkirmishAIDeterministicPlanning(true,
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY));
	assert(ShouldUseSkirmishAIDeterministicPlanning(true,
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT));
	assert(!ShouldUseGeneralsAICanonicalPlanning(
		false, false, false, false, false));
	assert(ShouldUseGeneralsAICanonicalPlanning(
		false, false, false, false, true));
	// Network topology selects serial/parallel admission; it does not disable
	// the current canonical epoch. Legacy/unmarked replay remains gated off.
	assert(ShouldUseGeneralsAICanonicalPlanning(
		true, false, false, false, true));
	assert(!ShouldUseGeneralsAICanonicalPlanning(
		false, true, false, false, true));
	assert(ShouldUseGeneralsAICanonicalPlanning(
		false, true, false, true, false));
	assert(!ShouldUseGeneralsAICanonicalPlanning(
		false, false, true, false, true));
	assert(ShouldUseGeneralsAICanonicalPlanning(
		false, false, true, true, false));
	assert(ShouldEnableGeneralsAICanonicalRuntimeEpoch());
	SetGeneralsAICanonicalRuntimeEpoch(true);
	assert(IsGeneralsAICanonicalRuntimeEpoch());
	assert(ShouldMarkGeneralsAICanonicalRecording(
		GAME_SINGLE_PLAYER, true));
	assert(ShouldMarkGeneralsAICanonicalRecording(GAME_SKIRMISH, true));
	assert(!ShouldMarkGeneralsAICanonicalRecording(GAME_LAN, true));
	assert(!ShouldMarkGeneralsAICanonicalRecording(GAME_INTERNET, true));
	assert(!ShouldMarkGeneralsAICanonicalRecording(
		GAME_SINGLE_PLAYER, false));
	assert(!ShouldMarkGeneralsAICanonicalRecording(GAME_SKIRMISH, false));
	assert(GetGeneralsAIRecordingEpoch(GAME_SINGLE_PLAYER, true) ==
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	assert(GetGeneralsAIRecordingEpoch(GAME_SKIRMISH, true) ==
		SKIRMISH_AI_REPLAY_EPOCH_CURRENT);
	assert(GetGeneralsAIRecordingEpoch(GAME_LAN, true) ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	assert(GetGeneralsAIRecordingEpoch(GAME_INTERNET, true) ==
		SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
	SetGeneralsAICanonicalRuntimeEpoch(false);
}

void MakeEnemySnapshot(GeneralsAIEnemyPlanningSnapshot *snapshot,
	uint32_t playerIndex, uint32_t candidateOrdinal, int32_t enemyIndex)
{
	ClearGeneralsAIEnemyPlanningSnapshot(snapshot);
	snapshot->frame = 500U;
	snapshot->ownerPlayerIndex = playerIndex;
	snapshot->initialBestDistanceSquared = kRetailEnemyThreshold;
	snapshot->candidateCount = 1U;
	snapshot->candidates[0].sourceOrdinal = candidateOrdinal;
	snapshot->candidates[0].playerIndex = enemyIndex;
	snapshot->candidates[0].baseDistanceSquared = 1000.0f;
}

void TestGeneralsScoringAndUntrustedResults()
{
	GeneralsAIEnemyPlanningSnapshot empty;
	ClearGeneralsAIEnemyPlanningSnapshot(&empty);
	empty.frame = 1U;
	empty.ownerPlayerIndex = 2U;
	empty.initialBestDistanceSquared = kRetailEnemyThreshold;
	GeneralsAIEnemyPlanningResult emptyResult;
	assert(PlanGeneralsAIEnemyTarget(empty, &emptyResult));
	assert(emptyResult.valid == 1U);
	assert(emptyResult.selectedPlayerIndex == -1);
	assert(emptyResult.orderKey.sourceOrdinal == rts::AI_PLANNING_INVALID_ORDINAL);
	assert(ValidateGeneralsAIEnemyPlanningResult(empty, emptyResult));

	GeneralsAIEnemyPlanningSnapshot snapshot;
	ClearGeneralsAIEnemyPlanningSnapshot(&snapshot);
	snapshot.frame = 77U;
	snapshot.ownerPlayerIndex = 2U;
	snapshot.initialBestDistanceSquared = kRetailEnemyThreshold;
	snapshot.candidateCount = 2U;
	snapshot.candidates[0].sourceOrdinal = 1U;
	snapshot.candidates[0].playerIndex = 4;
	snapshot.candidates[0].baseDistanceSquared = 1000.0f;
	snapshot.candidates[0].targetingCandidateMask = 1U << 3U;
	snapshot.candidates[0].targetingOwnerMask = 1U << 4U;
	snapshot.candidates[1].sourceOrdinal = 3U;
	snapshot.candidates[1].playerIndex = 5;
	snapshot.candidates[1].baseDistanceSquared = 2000.0f;

	GeneralsAIEnemyPlanningResult result;
	assert(PlanGeneralsAIEnemyTarget(snapshot, &result));
	assert(result.selectedPlayerIndex == 5);
	assert(result.orderKey.sourceOrdinal == 3U);
	assert(ValidateGeneralsAIEnemyPlanningResult(snapshot, result));

	// Equal distances retain the first PlayerList candidate, matching retail's
	// strict less-than comparison.
	snapshot.candidates[0].targetingCandidateMask = 0U;
	snapshot.candidates[0].targetingOwnerMask = 0U;
	snapshot.candidates[1].baseDistanceSquared = 1000.0f;
	assert(PlanGeneralsAIEnemyTarget(snapshot, &result));
	assert(result.selectedPlayerIndex == 4);
	assert(result.orderKey.sourceOrdinal == 1U);
	GeneralsAIEnemyPlanningResult forged = result;
	forged.orderKey.sourceOrdinal =
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS;
	assert(!ValidateGeneralsAIEnemyPlanningResult(snapshot, forged));

	forged = result;
	forged.orderKey.sourceOrdinal = 3U;
	forged.selectedPlayerIndex = 5;
	assert(!ValidateGeneralsAIEnemyPlanningResult(snapshot, forged));
	forged = result;
	forged.selectedDistanceSquared += 1.0f;
	assert(!ValidateGeneralsAIEnemyPlanningResult(snapshot, forged));

	snapshot.candidates[1].sourceOrdinal =
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS;
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));
	snapshot.candidates[1].sourceOrdinal = 3U;
	snapshot.candidateCount = GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS + 1U;
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));
	snapshot.candidateCount = 2U;
	snapshot.candidates[1].baseDistanceSquared =
		std::numeric_limits<float>::infinity();
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));
	snapshot.candidates[1].baseDistanceSquared = -1.0f;
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));
	snapshot.candidates[1].baseDistanceSquared = 1000.0f;
	snapshot.candidates[1].targetingOwnerMask = 1U << 20U;
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));
	snapshot.candidates[1].targetingOwnerMask = 1U << 3U;
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));
	snapshot.candidates[1].targetingOwnerMask = 0U;
	snapshot.candidates[1].playerIndex =
		GENERALS_AI_ENEMY_PLANNING_MAX_PLAYERS;
	assert(!ValidateGeneralsAIEnemyPlanningSnapshot(snapshot));

	// Generals clamps after each preference subtraction in PlayerList order.
	ClearGeneralsAIEnemyPlanningSnapshot(&snapshot);
	snapshot.frame = 88U;
	snapshot.ownerPlayerIndex = 2U;
	snapshot.initialBestDistanceSquared = kRetailEnemyThreshold;
	snapshot.candidateCount = 1U;
	snapshot.candidates[0].sourceOrdinal = 1U;
	snapshot.candidates[0].playerIndex = 4;
	snapshot.candidates[0].baseDistanceSquared = 500.0f;
	snapshot.candidates[0].targetingOwnerMask = (1U << 2U) | (1U << 4U);
	snapshot.candidates[0].targetingCandidateMask = 1U << 3U;
	assert(PlanGeneralsAIEnemyTarget(snapshot, &result));
	assert(result.selectedDistanceSquared == 249375.0f);

	// Retail starts at HUGE_DIST squared and uses strict less-than. A candidate
	// at or beyond that threshold leaves the current target unchanged.
	snapshot.candidates[0].targetingOwnerMask = 0U;
	snapshot.candidates[0].targetingCandidateMask = 0U;
	snapshot.candidates[0].baseDistanceSquared = kRetailEnemyThreshold;
	assert(PlanGeneralsAIEnemyTarget(snapshot, &result));
	assert(result.selectedPlayerIndex == -1);
	assert(result.orderKey.sourceOrdinal == rts::AI_PLANNING_INVALID_ORDINAL);
	assert(ValidateGeneralsAIEnemyPlanningResult(snapshot, result));
	snapshot.candidates[0].baseDistanceSquared = 1.1e12f;
	assert(PlanGeneralsAIEnemyTarget(snapshot, &result));
	assert(result.selectedPlayerIndex == -1);
	assert(ValidateGeneralsAIEnemyPlanningResult(snapshot, result));
}

void AssertEqualBatch(const GeneralsAIEnemyPlanningResult *expected,
	const GeneralsAIEnemyPlanningResult *actual, uint32_t count)
{
	for (uint32_t i = 0U; i < count; ++i)
		assert(EqualGeneralsAIEnemyPlanningResult(expected[i], actual[i]));
}

void TestRetailOrderedInitialEnemyProjection()
{
	GeneralsAIEnemyPlanningSnapshot captured[2];
	ClearGeneralsAIEnemyPlanningSnapshot(&captured[0]);
	captured[0].frame = 1U;
	captured[0].ownerPlayerIndex = 0U;
	captured[0].initialBestDistanceSquared = kRetailEnemyThreshold;
	captured[0].candidateCount = 2U;
	captured[0].candidates[0].sourceOrdinal = 2U;
	captured[0].candidates[0].playerIndex = 2;
	captured[0].candidates[0].baseDistanceSquared = 1000.0f;
	captured[0].candidates[1].sourceOrdinal = 3U;
	captured[0].candidates[1].playerIndex = 3;
	captured[0].candidates[1].baseDistanceSquared = 2000.0f;
	MakeEnemySnapshot(&captured[1], 1U, 2U, 2);
	captured[1].frame = 1U;

	GeneralsAIEnemyPlanningResult simultaneous[2];
	assert(PlanGeneralsAIEnemyPlanningBatchSerial(captured, 2U, simultaneous));
	assert(simultaneous[0].selectedPlayerIndex == 2);
	assert(simultaneous[1].selectedPlayerIndex == 2);

	const UnsignedInt ownerSources[2] = { 0U, 1U };
	const Bool skirmishBySource[4] = { true, true, false, false };
	const Int initialTargets[4] = { -1, -1, -1, -1 };
	GeneralsAIEnemyPlanningSnapshot projected[2];
	GeneralsAIEnemyPlanningResult projectedResults[2];
	UnsignedInt publicationOrder[2];
	assert(ProjectGeneralsAIEnemyPlanningOrder(captured, ownerSources, 2U,
		skirmishBySource, initialTargets, 4U, projected, projectedResults,
		publicationOrder));
	// Owner zero recursively observes owner one acquire source two before its
	// own first score, exactly matching the retail getCurrentEnemy recursion.
	assert(projected[0].candidates[0].targetingCandidateMask == (1U << 1U));
	assert(projectedResults[1].selectedPlayerIndex == 2);
	assert(projectedResults[0].selectedPlayerIndex == 3);
	assert(publicationOrder[0] == 1U);
	assert(publicationOrder[1] == 0U);

	GeneralsAIEnemyPlanningResult parallelInputs[2];
	assert(PlanGeneralsAIEnemyPlanningBatchSerial(
		projected, 2U, parallelInputs));
	AssertEqualBatch(projectedResults, parallelInputs, 2U);
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2U;
	config.queueCapacity = 64U;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	rts::AIPlanningBatchStatus status;
	std::atomic<UnsignedInt> physicalRendezvous(0U);
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, projected, 2U,
		parallelInputs, rts::AI_PLANNING_INVALID_ORDINAL, &status,
		&physicalRendezvous));
	AssertEqualBatch(projectedResults, parallelInputs, 2U);
	assert(physicalRendezvous.load(std::memory_order_acquire) == 2U);
	assert(status.distinctPhysicalWorkers == 2U);
	assert(status.peakConcurrentPhysicalWorkers == 2U);
	assert(status.parallelSucceeded == 1U);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));

	// A retained non-due target participates in the same observation table and
	// must not be mistaken for an all-null first-tick view.
	const Bool oneDueSkirmish[4] = { true, true, false, false };
	const Int retainedTargets[4] = { -1, 2, -1, -1 };
	assert(ProjectGeneralsAIEnemyPlanningOrder(captured, ownerSources, 1U,
		oneDueSkirmish, retainedTargets, 4U, projected, projectedResults,
		publicationOrder));
	assert(projected[0].candidates[0].targetingCandidateMask == (1U << 1U));
	assert(projectedResults[0].selectedPlayerIndex == 3);
}

void TestCanonicalBatchAcrossTopologiesAndFailure()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.queueCapacity = 64U;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	GeneralsAIEnemyPlanningSnapshot snapshots[2];
	ClearGeneralsAIEnemyPlanningSnapshot(&snapshots[0]);
	snapshots[0].frame = 500U;
	snapshots[0].ownerPlayerIndex = 2U;
	snapshots[0].initialBestDistanceSquared = kRetailEnemyThreshold;
	snapshots[0].candidateCount = 2U;
	snapshots[0].candidates[0].sourceOrdinal = 4U;
	snapshots[0].candidates[0].playerIndex = 4;
	snapshots[0].candidates[0].baseDistanceSquared = 1000.0f;
	snapshots[0].candidates[1].sourceOrdinal = 5U;
	snapshots[0].candidates[1].playerIndex = 5;
	snapshots[0].candidates[1].baseDistanceSquared = 2000.0f;
	MakeEnemySnapshot(&snapshots[1], 3U, 4U, 4);

	GeneralsAIEnemyPlanningResult canonical[2];
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_SERIAL, false, snapshots, 2U, canonical));
	assert(canonical[0].selectedPlayerIndex == 4);
	assert(canonical[1].selectedPlayerIndex == 4);

	// Retail recursively asks the other owner for its current enemy. If that
	// owner acquires candidate 4 during the first owner's scan, candidate 4
	// gains the 500 squared gang-up penalty and the later score changes.
	GeneralsAIEnemyPlanningSnapshot retailMutated = snapshots[0];
	retailMutated.candidates[0].targetingCandidateMask = 1U << 3U;
	GeneralsAIEnemyPlanningResult retailResult;
	assert(PlanGeneralsAIEnemyTarget(retailMutated, &retailResult));
	assert(retailResult.selectedPlayerIndex == 5);
	assert(retailResult.selectedPlayerIndex != canonical[0].selectedPlayerIndex);

	GeneralsAIEnemyPlanningResult results[2];
	rts::AIPlanningBatchStatus status;
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, snapshots, 2U, results,
		rts::AI_PLANNING_INVALID_ORDINAL, &status));
	AssertEqualBatch(canonical, results, 2U);
	assert(status.requestedMode == rts::AI_PLANNING_EXECUTION_PARALLEL);
	assert(status.committedMode == rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(status.parallelSucceeded == 0U);
	assert(status.usedSerialFallback == 1U);

	config.workerCount = 1U;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	jobs.resetMetrics();
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, snapshots, 2U, results,
		rts::AI_PLANNING_INVALID_ORDINAL, &status));
	AssertEqualBatch(canonical, results, 2U);
	assert(status.committedMode == rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(status.usedSerialFallback == 1U);
	assert(jobs.metrics().submittedJobCount == 0U);
	assert(jobs.metrics().serialFallbackCount >= 1U);
	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));

	config.workerCount = 2U;
	assert(jobs.start(config));
	assert(jobs.workerCount() == 2U);
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	jobs.resetMetrics();
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_SERIAL, false, snapshots, 2U, results));
	AssertEqualBatch(canonical, results, 2U);
	assert(!ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, true, snapshots, 2U, results));
	std::atomic<UnsignedInt> physicalRendezvous(0U);
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, snapshots, 2U, results,
		rts::AI_PLANNING_INVALID_ORDINAL, &status, &physicalRendezvous));
	AssertEqualBatch(canonical, results, 2U);
	assert(status.parallelSucceeded == 1U);
	assert(status.committedMode == rts::AI_PLANNING_EXECUTION_PARALLEL);
	assert(status.usedSerialFallback == 0U);
	const rts::JobSystemMetrics metrics = jobs.metrics();
	assert(metrics.submittedJobCount >= 2U);
	assert(metrics.executedJobCount >= 2U);
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	// Workers started with the process defaults. Change only the owner state,
	// then require physical-worker planning to match the owner-serial oracle and
	// leave the owner state untouched across submission/wait.
	const unsigned savedMxcsr = _mm_getcsr();
	const unsigned ownerMxcsr =
		(savedMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_UP;
	_mm_setcsr(ownerMxcsr);
	physicalRendezvous.store(0U, std::memory_order_release);
	GeneralsAIEnemyPlanningResult floatingPointOracle[2];
	assert(PlanGeneralsAIEnemyPlanningBatchSerial(
		snapshots, 2U, floatingPointOracle));
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, snapshots, 2U, results,
		rts::AI_PLANNING_INVALID_ORDINAL, &status, &physicalRendezvous));
	AssertEqualBatch(floatingPointOracle, results, 2U);
	assert((_mm_getcsr() & ~0x3fU) == (ownerMxcsr & ~0x3fU));
	_mm_setcsr(savedMxcsr);
#endif
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, snapshots, 2U, results, 0U,
		&status));
	AssertEqualBatch(canonical, results, 2U);
	assert(status.parallelSucceeded == 0U);
	assert(status.committedMode == rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(status.usedSerialFallback == 1U);
	assert(jobs.metrics().failedJobCount >= 1U);
	physicalRendezvous.store(0U, std::memory_order_release);
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_SHADOW, false, snapshots, 2U, results,
		rts::AI_PLANNING_INVALID_ORDINAL, &status, &physicalRendezvous));
	AssertEqualBatch(canonical, results, 2U);
	assert(status.requestedMode == rts::AI_PLANNING_EXECUTION_SHADOW);
	assert(status.parallelSucceeded == 1U);
	assert(status.committedMode == rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(status.shadowMatched == 1U);
	assert(status.usedSerialFallback == 0U);

	jobs.shutdown();
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	assert(ExecuteGeneralsAIEnemyPlanningBatch(
		rts::AI_PLANNING_EXECUTION_PARALLEL, false, snapshots, 2U, results,
		rts::AI_PLANNING_INVALID_ORDINAL, &status));
	AssertEqualBatch(canonical, results, 2U);
	assert(status.committedMode == rts::AI_PLANNING_EXECUTION_SERIAL);
	assert(status.usedSerialFallback == 1U);
}

#if defined(_WIN64)
int g_generalsPhaseFailures = 0;
unsigned g_generalsPhaseVariant = 0;
bool g_generalsPhaseBaseline = false;
unsigned g_generalsDetachedCalls = 0;
void GeneralsPhaseExpect(bool condition, const char *message)
{
	if (!condition)
	{
		++g_generalsPhaseFailures;
		std::cerr << "FAIL [Generals AI " << (g_generalsPhaseBaseline ? "consumer" : "source") <<
			" variant=" << g_generalsPhaseVariant << "]: " << message << '\n';
	}
}
bool CountGeneralsDetached(const void *input, void *output)
{
	++g_generalsDetachedCalls;
	return ComputeGeneralsAIEnemyReferenceSerial(input, output);
}
bool WriteGeneralsAIEnemyReferenceOutputWithInjectedDigestFailure(
	rts::performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const GeneralsAIEnemyReferenceView *view =
		static_cast<const GeneralsAIEnemyReferenceView *>(context);
	if (view == 0 || view->results == 0 || view->count == 0U)
		return false;
	GeneralsAIEnemyPlanningResult *results =
		const_cast<GeneralsAIEnemyPlanningResult *>(view->results);
	const GeneralsAIEnemyPlanningResult saved = results[0];
	// Keep the authoritative result unchanged while making the observer's
	// canonical digest disagree with the recorded source output.
	results[0].selectedPlayerIndex = saved.selectedPlayerIndex == -1 ?
		0 : saved.selectedPlayerIndex + 1;
	const bool wrote = WriteGeneralsAIEnemyReferenceOutput(writer, context);
	results[0] = saved;
	return wrote;
}
struct GeneralsPhaseObservations
{
	std::atomic<unsigned> entries[4]{}, bodies[4]{}, validations[4]{};
	std::atomic<bool> wrongOwner{false};
	std::atomic<unsigned> held{0};
	std::atomic<bool> releaseHeld{false}, waitExpired{false};
	unsigned cancelNotifications = 0, releaseNotifications = 0, releasedCompleted = 0, releasedSubmitted = 0;
	unsigned releasedReason = 0;
	bool releasedCancelled = false, lateCancel = false;
	rts_test::NativeKernelClock *clock = 0;
	bool baseline = false;
	~GeneralsPhaseObservations() { releaseHeld.store(true, std::memory_order_release); }
	static void beforeWait(void *opaque)
	{
		auto &self = *static_cast<GeneralsPhaseObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongOwner = true;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
		while (self.held.load(std::memory_order_acquire) != 4 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (self.held.load(std::memory_order_acquire) != 4)
		{ self.waitExpired = true; self.releaseHeld.store(true, std::memory_order_release); }
	}
	static void afterCancel(void *opaque)
	{
		auto &self = *static_cast<GeneralsPhaseObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongOwner = true;
		++self.cancelNotifications; self.releaseHeld.store(true, std::memory_order_release);
	}
	static void releasedGroup(void *opaque, bool cancelled, unsigned completed, unsigned submitted, unsigned reason)
	{
		auto &self = *static_cast<GeneralsPhaseObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongOwner = true;
		++self.releaseNotifications; self.releasedCancelled = cancelled;
		self.releasedCompleted = completed; self.releasedSubmitted = submitted; self.releasedReason = reason;
	}
	static void observe(void *opaque, rts::AIPlanningTestEvent event, uint32_t ordinal,
		uint32_t begin, uint32_t end, rts::AIProductionCandidateFact *)
	{
		auto &self = *static_cast<GeneralsPhaseObservations *>(opaque);
		const bool owner = rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME);
		const bool validation = event == rts::AI_PLANNING_TEST_OWNER_VALIDATION;
		if (ordinal >= 4 || begin != 0 || end != 1 || owner != (validation || self.baseline))
		{ self.wrongOwner = true; return; }
		if (event == rts::AI_PLANNING_TEST_RANGE_ENTRY) { ++self.entries[ordinal]; self.clock->now.fetch_add(7); }
		else if (event == rts::AI_PLANNING_TEST_PLAYER_BODY) { ++self.bodies[ordinal]; self.clock->now.fetch_add(13); }
		else if (validation) { ++self.validations[ordinal]; self.clock->now.fetch_add(41); }
		if (!self.baseline && self.lateCancel && event == rts::AI_PLANNING_TEST_PLAYER_BODY)
		{
			// The actual entry poll is behind us; the native planner has no later
			// cancellation poll and must complete after the real group cancel.
			self.held.fetch_add(1, std::memory_order_release);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
			while (!self.releaseHeld.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
			if (!self.releaseHeld.load(std::memory_order_acquire))
			{ self.waitExpired = true; self.releaseHeld.store(true, std::memory_order_release); }
		}
	}
	static bool cancel(void *opaque, uint32_t, uint32_t, uint32_t)
	{ return static_cast<GeneralsPhaseObservations *>(opaque)->baseline; }
};

bool RunGeneralsNativePhaseRole(rts_test::NativeKernelTrace &trace, bool baseline, unsigned variant)
{
	using namespace rts::performance;
	const bool injectFailure = variant == 1, lateCancel = variant == 2;
	const bool observerFailure = baseline && variant == 3;
	const bool abortExpected = injectFailure || lateCancel || observerFailure;
	g_generalsPhaseVariant = static_cast<int>(variant); g_generalsPhaseBaseline = baseline;
	const int failuresBefore = g_generalsPhaseFailures;
	std::cerr << "BEGIN [Generals AI " << (baseline ? "consumer" : "source") <<
		" variant=" << g_generalsPhaseVariant << "]\n";
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4; config.queueCapacity = 64; config.scratchBytesPerWorker = 4096; config.pinWorkers = false;
	if (!jobs.start(config) || !jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{ GeneralsPhaseExpect(false, "Generals phase fixture starts four real workers"); return false; }
	rts_test::NativeKernelOwnerRun run;
	const bool started = run.begin(trace, baseline, 500, KERNEL_PHASE_OWNER_INTAKE);
	GeneralsPhaseExpect(started, "Generals native role accepts its real ledger and immutable source");
	if (!started) { jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false; }
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_AI, 0);
	GeneralsPhaseExpect(attempt.valid(), "Generals core owner opens authentic attempt before capture");
	auto timingBatch = run.timing.beginBatch(KERNEL_PERFORMANCE_AI, 0, 500, 1);
	const auto capture = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_CAPTURE);
	GeneralsAIEnemyPlanningSnapshot snapshots[4];
	GeneralsAIEnemyPlanningResult results[4] = {}, detached[4] = {};
	for (unsigned index = 0; index != 4; ++index)
	{
		MakeEnemySnapshot(&snapshots[index], index + 1, 0, 10);
		snapshots[index].candidateCount = 2;
		snapshots[index].candidates[1] = snapshots[index].candidates[0];
		snapshots[index].candidates[1].sourceOrdinal = 1;
		snapshots[index].candidates[1].playerIndex = 11;
	}
	run.clock.now.fetch_add(5);
	GeneralsPhaseExpect(run.timing.endInterval(capture), "Generals capture precedes the actual title-specific executor");
	GeneralsAIEnemyReferenceView input = {snapshots, 0, 4};
	GeneralsAIEnemyReferenceView output = {snapshots, results, 4};
	GeneralsAIEnemyReferenceView oracle = {snapshots, detached, 4};
	KernelPerformanceReferenceBatch validated;
	rts::AIPlanningReferenceBatchTransport transport;
	transport.referenceLedger = &run.reference; transport.referenceAttempt = attempt; transport.referenceBatch = &validated;
	transport.writeInput = WriteGeneralsAIEnemyReferenceInput; transport.immutableInput = &input;
	transport.writeOutput = observerFailure ?
		WriteGeneralsAIEnemyReferenceOutputWithInjectedDigestFailure :
		WriteGeneralsAIEnemyReferenceOutput; transport.productionOutput = &output;
	transport.serialCompute = CountGeneralsDetached; transport.detachedSerialOutput = &oracle; transport.operationCount = 4;
	GeneralsPhaseObservations observations; observations.clock = &run.clock; observations.baseline = baseline;
	observations.lateCancel = lateCancel;
	rts::AIPlanningTestHooks hooks; hooks.context = &observations;
	hooks.observe = GeneralsPhaseObservations::observe; hooks.cancelAtEntry = GeneralsPhaseObservations::cancel;
	if (lateCancel)
	{
		hooks.beforeWait = GeneralsPhaseObservations::beforeWait;
		hooks.afterCancel = GeneralsPhaseObservations::afterCancel;
		hooks.releasedGroup = GeneralsPhaseObservations::releasedGroup;
	}
	transport.testHooks = &hooks;
	std::atomic<UnsignedInt> rendezvous(0);
	rts::AIPlanningBatchStatus status;
	const unsigned detachedBefore = g_generalsDetachedCalls;
	const bool executed = ExecuteGeneralsAIEnemyPlanningBatch(rts::AI_PLANNING_EXECUTION_PARALLEL, false,
		snapshots, 4, results, injectFailure ? 1 : rts::AI_PLANNING_INVALID_ORDINAL, &status,
		baseline || lateCancel ? 0 : &rendezvous, &timingBatch, &transport);
	GeneralsPhaseExpect(executed && status.usedSerialFallback == (abortExpected ? 1U : 0U),
		"Generals source success or injected failure keeps its predeclared native outcome");
	for (unsigned index = 0; index != 4; ++index)
	{
		GeneralsPhaseExpect(results[index].valid == 1 && results[index].selectedPlayerIndex == 10 &&
			results[index].selectedDistanceSquared == 1000.0f && results[index].orderKey.sourceOrdinal == 0,
			"actual Generals strict tie selects literal first candidate ten, not shared ZH scoring");
		GeneralsPhaseExpect(observations.entries[index] == 1 &&
			observations.bodies[index] == (injectFailure && index == 1 ? 0U : 1U) &&
			observations.validations[index] == (abortExpected && !observerFailure ? 0U : 1U),
			"Generals enters four ranges once and retains distinct serial owner validations");
	}
	GeneralsPhaseExpect(!observations.wrongOwner && g_generalsDetachedCalls == detachedBefore,
		"Generals body provenance is real and no detached planner supplies baseline output");
	if (lateCancel && !baseline)
	{
		GeneralsPhaseExpect(!observations.waitExpired && observations.held == 4 && observations.cancelNotifications == 1 &&
			observations.releaseNotifications == 1 && observations.releasedCancelled &&
			observations.releasedCompleted == 4 && observations.releasedSubmitted == 4,
			"Generals native timeout cancels a real group then drains four late-completed planners");
		GeneralsPhaseExpect(observations.releasedReason == 3,
			"Generals source reason records actual group cancellation independently of completed checkpoints");
	}
	if (lateCancel && baseline)
		GeneralsPhaseExpect(observations.held == 0 && observations.cancelNotifications == 0 && observations.releaseNotifications == 0,
			"Generals baseline replays late disposal without a wait or a new physical cancellation");
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	if (baseline)
		GeneralsPhaseExpect(scheduler.submittedJobs == 0 && scheduler.executedJobs == 0 && scheduler.ownerHelpJobs == 0 &&
			status.physicalWorkerMask == 0 && status.distinctPhysicalWorkers == 0 &&
			status.peakConcurrentPhysicalWorkers == 0 && status.parallelSucceeded == 0,
			"Generals baseline executes no physical or owner-help jobs and fabricates no worker authority");
	else GeneralsPhaseExpect(scheduler.submittedJobs == 4 && scheduler.executedJobs == 4 &&
		status.ownerHelpedJobs == 0 && (abortExpected || status.parallelSucceeded == 1),
		"Generals controlled source uses its own actual four-job executor");
	const bool accepted = executed && status.usedSerialFallback == 0;
	const auto linked = validated;
	rts::RecordAIPlanningOwnerCommit(executed, &status);
	if (linked.valid()) GeneralsPhaseExpect(rts::FinishAIPlanningReferenceBatch(&transport, accepted),
		"Generals owner completes canonical publication before source attempt finish");
	GeneralsPhaseExpect(linked.valid() == accepted, "Generals native executor links only independently validated output");
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = accepted ? KERNEL_PERFORMANCE_COMMITTED : KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1; finish.reason = accepted ? 1 : 2;
	finish.fallbackEntered = finish.fallbackCompleted = executed && status.usedSerialFallback != 0;
	finish.validatedBatch = linked;
	GeneralsPhaseExpect(run.reference.finishAttempt(attempt, finish) == !observerFailure,
		"Generals records observer failure before attempting receipt closure");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	reap.pendingJobs = scheduler.pendingJobs; reap.outstandingJobs = scheduler.outstandingJobs;
	GeneralsPhaseExpect(run.reference.reapAttempt(attempt, reap) == !observerFailure,
		"Generals does not silently reap a poisoned observer attempt");
	if (accepted)
	{
		const auto commit = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_COMMIT);
		run.clock.now.fetch_add(11); run.timing.endInterval(commit);
	}
	GeneralsPhaseExpect(run.timing.endBatch(timingBatch, finish.disposition), "Generals timing retains native outcome");
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.closeTiming(scheduler);
	GeneralsPhaseExpect(timingClosed, "Generals whole owner phase and scheduler close");
	if (baseline && timingClosed && !observerFailure)
	{
		const auto &phase = run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_OWNER_INTAKE];
		GeneralsPhaseExpect(phase.pureNanoseconds == (abortExpected ? 0U : 80U) &&
			phase.serialNanoseconds >= (abortExpected ? 5U : 180U),
			"Generals four planner bodies are pure only on success; canonical owner validations remain serial");
	}
	const bool canonicalState = observerFailure ?
		!snapshot.complete && snapshot.streamCount == 0 : abortExpected ?
		!snapshot.complete && snapshot.streamCount == 0 :
		snapshot.complete && snapshot.streamCount == 1;
	if (observerFailure)
		GeneralsPhaseExpect(!sealed && canonicalState && snapshot.errors != 0 && !snapshot.trace.complete,
			"Generals rejects a mismatched observer digest before owner publication and leaves no valid receipt");
	else
		GeneralsPhaseExpect(sealed && canonicalState && snapshot.errors == 0 && snapshot.trace.complete && snapshot.trace.attemptCount == 1 &&
			snapshot.trace.capturedOperationCount == 4 && snapshot.trace.dispatchCount == 1 &&
			snapshot.trace.rangeCount == 4 && snapshot.trace.releasedRangeCount == 4 && snapshot.trace.reapCount == 1,
			"actual Generals enemy executor supplies complete source and consumed attempt evidence");
	if (!baseline) trace.source = snapshot;
	else if (snapshot.complete && !abortExpected)
		GeneralsPhaseExpect(snapshot.streams[0].outputDigest.equals(trace.source.streams[0].outputDigest),
			"Generals same native canonical serializer binds once-only consumer output to source");
	jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	std::cerr << "END [Generals AI " << (baseline ? "consumer" : "source") <<
		" variant=" << g_generalsPhaseVariant << "] failures=" << g_generalsPhaseFailures - failuresBefore <<
		" traceComplete=" << snapshot.trace.complete << '\n';
	return observerFailure ? canonicalState && snapshot.errors != 0 && !snapshot.trace.complete :
		canonicalState && snapshot.errors == 0 && snapshot.trace.complete &&
		(!lateCancel || baseline || observations.releasedReason == 3);
}
void TestGeneralsActualNativeSourceConsumer()
{
	for (unsigned variant = 0; variant != 4; ++variant)
	{
		rts_test::NativeKernelTrace trace(30 + variant);
		if (RunGeneralsNativePhaseRole(trace, false, variant))
			RunGeneralsNativePhaseRole(trace, true, variant);
	}
}
#endif
}

int main()
{
#if defined(_MSC_VER)
	_set_error_mode(_OUT_TO_STDERR);
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	TestGeneralsPathfindingReplayEpochContract();
	TestTopologyAndReplayGates();
	TestGeneralsScoringAndUntrustedResults();
	TestRetailOrderedInitialEnemyProjection();
	TestCanonicalBatchAcrossTopologiesAndFailure();
#if defined(_WIN64)
	TestGeneralsActualNativeSourceConsumer();
	if (g_generalsPhaseFailures != 0) return 1;
#endif
	std::cout << "Generals deterministic AI planning tests passed.\n";
	return 0;
}
