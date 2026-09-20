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

enum SkirmishAITestScenario
{
	SKIRMISH_AI_TEST_SCENARIO_4V3,
	SKIRMISH_AI_TEST_SCENARIO_4V2
};

// CLI: -runSkirmishAIRecoveryTest <positive-seed> <case> <FactionTemplate>.
// Opt-in, full-engine Stage 1 recovery fixtures.  These are deliberately
// separate from the normal replay scenarios: they stop on fixture assertions
// and never report a fixture result as a gameplay/replay gate.
// Cases: surviving_builder, factory_only, no_path_laststand, repeated_cc,
// obstructed, low_cash, gla_hole, save_load. Factions are the 12 playable Zero Hour Faction*
// templates returned by GetSkirmishAIRecoveryFactionTemplateName().
enum SkirmishAIRecoveryFixtureCase
{
	SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER,
	SKIRMISH_AI_RECOVERY_FACTORY_ONLY,
	SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND,
	SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER,
	SKIRMISH_AI_RECOVERY_OBSTRUCTED,
	SKIRMISH_AI_RECOVERY_LOW_CASH,
	SKIRMISH_AI_RECOVERY_GLA_HOLE,
	SKIRMISH_AI_RECOVERY_SAVE_LOAD,
	SKIRMISH_AI_RECOVERY_FIXTURE_CASE_COUNT
};

enum SkirmishAIRecoveryFaction
{
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA,
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA_SUPER_WEAPON_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA_LASER_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA_AIR_FORCE_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA_TANK_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA_INFANTRY_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA_NUKE_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_GLA,
	SKIRMISH_AI_RECOVERY_FACTION_GLA_TOXIN_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_GLA_DEMOLITION_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_GLA_STEALTH_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_COUNT
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
Bool TryParseSkirmishAIRecoveryFixtureCase(const char *text, Int *fixtureCase);
Bool TryParseSkirmishAIRecoveryFaction(const char *text, Int *faction);
Bool IsSupportedSkirmishAIRecoveryFixtureCombination(Int fixtureCase, Int faction);
const char *GetSkirmishAIRecoveryFixtureCaseName(Int fixtureCase);
const char *GetSkirmishAIRecoveryFactionName(Int faction);
const char *GetSkirmishAIRecoveryFactionTemplateName(Int faction);
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
Bool IsSkirmishAITestShutdownTimedOut(UnsignedInt elapsedMilliseconds);

void ArmSkirmishAITestRunner(Int seed,
	SkirmishAITestScenario scenario = SKIRMISH_AI_TEST_SCENARIO_4V3);
void ArmSkirmishAIRecoveryFixtureRunner(Int seed, Int fixtureCase, Int faction);
Bool IsSkirmishAITestRunnerArmed();
Bool StartSkirmishAITestRunner();
void UpdateSkirmishAITestRunner();
Int FinalizeSkirmishAITestRunner(Int engineExitCode);
