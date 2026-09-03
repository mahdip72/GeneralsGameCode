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

struct DirectPathRuntimeMetrics;
struct OrdinaryPathRuntimeMetrics;
#if defined(_WIN64)
namespace rts
{
struct CollisionCandidateRuntimeMetrics;
struct ImmutableSpatialRuntimeMetrics;
struct ObjectStatusTimerRuntimeMetrics;
struct PhysicsIntegrationRuntimeMetrics;
}
#endif

#include "GameNetwork/GameInfo.h"
#include "Common/SkirmishAITestReceipt.h"

enum
{
	SKIRMISH_AI_TEST_SLOT_COUNT = 8,
	SKIRMISH_AI_TEST_MAX_FRAME = 108000,
	SKIRMISH_AI_TEST_MAX_STARTUP_MILLISECONDS = 300000,
	SKIRMISH_AI_TEST_MAX_STALLED_MILLISECONDS = 30000,
	SKIRMISH_AI_TEST_MAX_SHUTDOWN_MILLISECONDS = 30000
};

enum SkirmishAITestScenario
{
	SKIRMISH_AI_TEST_SCENARIO_4V3,
	SKIRMISH_AI_TEST_SCENARIO_4V2,
	// This is a practical/manual lane.  It is intentionally distinct from
	// the observer-backed automated lanes and is not a replay gate scenario.
	SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7,
	SKIRMISH_AI_TEST_SCENARIO_ONE_CONTROLLER_7_AI =
		SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7,
	// Eight occupied hard-AI slots: two allied against six. The local player
	// is the engine-created replay observer, not a GameInfo slot.
	SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6
};

enum SkirmishAITestProgress
{
	SKIRMISH_AI_TEST_RUNNING,
	SKIRMISH_AI_TEST_COMPLETE,
	SKIRMISH_AI_TEST_TIMED_OUT
};

struct SkirmishAITestSlotPlan
{
	SlotState state;
	Int playerTemplate;
	Int color;
	Int startPosition;
	Int teamNumber;
	Bool isController;
};

struct SkirmishAITestPlan
{
	Int seed;
	const char *mapName;
	SkirmishAITestSlotPlan slots[SKIRMISH_AI_TEST_SLOT_COUNT];
};

struct SkirmishAITestLoadedState
{
	const char *gameInfoMapName;
	const char *globalMapName;
	const char *terrainMapName;
	UnsignedInt mapCRC;
	UnsignedInt mapSize;
	Int seed;
};

Bool TryParseSkirmishAITestSeed(const char *text, Int *seed);
Bool ShouldBypassFramePacingForSkirmishAITest(Bool runnerArmed);
void BuildSkirmishAITestPlan(Int seed, SkirmishAITestPlan *plan);
void BuildSkirmishAITestPlan(Int seed, SkirmishAITestScenario scenario,
	SkirmishAITestPlan *plan);
Bool IsExpectedSkirmishAITestLoadedState(const SkirmishAITestPlan &plan,
	UnsignedInt expectedMapCRC, UnsignedInt expectedMapSize,
	const SkirmishAITestLoadedState *loadedState);
Bool IsValidSkirmishAITestReplayResult(UnsignedInt expectedFrameCount,
	UnsignedInt actualFrameCount, Bool desyncGame, Bool quitEarly,
	time_t startTime, time_t endTime);
SkirmishAITestProgress EvaluateSkirmishAITestProgress(UnsignedInt endFrame, UnsignedInt currentFrame);
Bool IsSkirmishAITestStartupTimedOut(UnsignedInt elapsedMilliseconds);
Bool IsSkirmishAITestProgressStalled(UnsignedInt elapsedMilliseconds);

// Pure lifecycle accumulator used by the installed runner and focused tests.
// It freezes nonzero path authority before a later game-data reset epoch.
void AccumulateSkirmishAITestDirectPathMetrics(
	DirectPathRuntimeMetrics *baseline,
	const DirectPathRuntimeMetrics &current,
	DirectPathRuntimeMetrics *frozen,
	Bool *hasFrozenActivity,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestOrdinaryPathMetrics(
	OrdinaryPathRuntimeMetrics *baseline,
	const OrdinaryPathRuntimeMetrics &current,
	OrdinaryPathRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
#if defined(_WIN64)
// Pure reset-epoch accumulators shared by installed lifecycle code and paired
// title fixtures. The first observed reset rebases shell state; a later reset
// is teardown and cannot erase the frozen match/replay evidence.
void AccumulateSkirmishAITestCollisionMetrics(
	rts::CollisionCandidateRuntimeMetrics *baseline,
	const rts::CollisionCandidateRuntimeMetrics &current,
	rts::CollisionCandidateRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestPhysicsMetrics(
	rts::PhysicsIntegrationRuntimeMetrics *baseline,
	const rts::PhysicsIntegrationRuntimeMetrics &current,
	rts::PhysicsIntegrationRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestObjectStatusTimerMetrics(
	rts::ObjectStatusTimerRuntimeMetrics *baseline,
	const rts::ObjectStatusTimerRuntimeMetrics &current,
	rts::ObjectStatusTimerRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestImmutableSpatialMetrics(
	rts::ImmutableSpatialRuntimeMetrics *baseline,
	const rts::ImmutableSpatialRuntimeMetrics &current,
	rts::ImmutableSpatialRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
#endif
Bool IsSkirmishAITestShutdownTimedOut(UnsignedInt elapsedMilliseconds);

Bool IsSkirmishAITestPracticalControllerScenario(SkirmishAITestScenario scenario);
Bool IsValidSkirmishAITestPracticalControllerPlan(
	const SkirmishAITestPlan &plan);

// Narrow per-invocation seam for retention commit-policy tests. Production
// callers use the ordinary three-argument wrapper below.
namespace SkirmishAITestDetail
{
typedef Bool (*ReplayCommitCallback)(
	const char *temporaryPath, const char *destinationPath, void *context);
Bool RetainSkirmishAITestReplayAtomically(
	const char *sourcePath, const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1],
	ReplayCommitCallback commitCallback, void *context);
}

Bool SetSkirmishAITestExecutableHashInput(const char *sha256);
// Shared validation hashing; these do not arm or alter any AI-test scenario.
Bool HashSkirmishAITestBytes(const void *bytes, size_t byteCount,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
Bool HashSkirmishAITestContentFile(const char *path,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
Bool CaptureSkirmishAITestValidatedExecutableHash(
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
Bool SetSkirmishAITestSimulationModeInput(const char *mode);
void SetSkirmishAITestFinalDigest(UnsignedInt digest);

void ArmSkirmishAITestRunner(Int seed,
	SkirmishAITestScenario scenario = SKIRMISH_AI_TEST_SCENARIO_4V3);
Bool IsSkirmishAITestRunnerArmed();
Bool StartSkirmishAITestRunner();
void UpdateSkirmishAITestRunner();
Int FinalizeSkirmishAITestRunner(Int engineExitCode);
#if defined(_WIN64)
void ObserveSkirmishAITestCompletedFrame(unsigned previousFrame);
// Call only after the real engine has drained and been destroyed. This uses
// retained diagnostic snapshots, never reset/destroyed game globals.
void FinalizeSkirmishAITestPerformanceReceipt(Int engineExitCode);
#endif
