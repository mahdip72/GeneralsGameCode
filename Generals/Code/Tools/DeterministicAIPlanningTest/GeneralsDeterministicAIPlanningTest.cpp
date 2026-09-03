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
	TestCanonicalBatchAcrossTopologiesAndFailure();
	std::cout << "Generals deterministic AI planning tests passed.\n";
	return 0;
}
