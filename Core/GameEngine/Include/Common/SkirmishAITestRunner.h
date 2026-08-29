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

#include "GameNetwork/GameInfo.h"

enum
{
	SKIRMISH_AI_TEST_SLOT_COUNT = 8,
	SKIRMISH_AI_TEST_MAX_FRAME = 108000,
	SKIRMISH_AI_TEST_MAX_STARTUP_MILLISECONDS = 300000,
	SKIRMISH_AI_TEST_MAX_STALLED_MILLISECONDS = 30000,
	SKIRMISH_AI_TEST_MAX_SHUTDOWN_MILLISECONDS = 30000
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
Bool IsExpectedSkirmishAITestLoadedState(const SkirmishAITestPlan &plan,
	UnsignedInt expectedMapCRC, UnsignedInt expectedMapSize,
	const SkirmishAITestLoadedState *loadedState);
Bool IsValidSkirmishAITestReplayResult(UnsignedInt expectedFrameCount,
	UnsignedInt actualFrameCount, Bool desyncGame, Bool quitEarly,
	time_t startTime, time_t endTime);
SkirmishAITestProgress EvaluateSkirmishAITestProgress(UnsignedInt endFrame, UnsignedInt currentFrame);
Bool IsSkirmishAITestStartupTimedOut(UnsignedInt elapsedMilliseconds);
Bool IsSkirmishAITestProgressStalled(UnsignedInt elapsedMilliseconds);
Bool IsSkirmishAITestShutdownTimedOut(UnsignedInt elapsedMilliseconds);

void ArmSkirmishAITestRunner(Int seed);
Bool IsSkirmishAITestRunnerArmed();
Bool StartSkirmishAITestRunner();
void UpdateSkirmishAITestRunner();
Int FinalizeSkirmishAITestRunner(Int engineExitCode);
