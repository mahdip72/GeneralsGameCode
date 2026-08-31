/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Common/INI.h"
#include "Common/SkirmishAITestRunner.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

class Win32Mouse;
HINSTANCE ApplicationHInstance = nullptr;
HWND ApplicationHWnd = nullptr;
Win32Mouse *TheWin32Mouse = nullptr;
DWORD TheMessageTime = 0;
const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
const char *gAppPrefix = "";
#if !defined(RTS_DEBUG)
ICoord2D TheMousePos = { 0, 0 };
#endif

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	return 0;
}

static Int s_failures = 0;

#define CHECK(expression) Check((expression), #expression, __LINE__)

static void Check(Bool result, const char *expression, Int line)
{
	if (!result)
	{
		printf("FAIL line %d: %s\n", line, expression);
		++s_failures;
	}
}

int main()
{
	CHECK(!IsSkirmishAITestRunnerArmed());
	Int seed = 0;
	CHECK(TryParseSkirmishAITestSeed("1729", &seed));
	CHECK(seed == 1729);
	CHECK(!TryParseSkirmishAITestSeed(nullptr, &seed));
	CHECK(!TryParseSkirmishAITestSeed("", &seed));
	CHECK(!TryParseSkirmishAITestSeed("0", &seed));
	CHECK(!TryParseSkirmishAITestSeed("-1", &seed));
	CHECK(!TryParseSkirmishAITestSeed("12x", &seed));
	CHECK(!TryParseSkirmishAITestSeed("2147483648", &seed));
	CHECK(!TryParseSkirmishAITestSeed("1", nullptr));

	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(1729, &plan);
	CHECK(plan.seed == 1729);
	CHECK(strcmp(plan.mapName, "Maps\\Twilight Flame\\Twilight Flame.map") == 0);
	CHECK(plan.slots[0].state == SLOT_PLAYER);
	CHECK(plan.slots[0].playerTemplate == PLAYERTEMPLATE_OBSERVER);
	CHECK(plan.slots[0].color == -1);
	CHECK(plan.slots[0].startPosition == -1);
	CHECK(plan.slots[0].teamNumber == -1);

	for (Int i = 1; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		CHECK(plan.slots[i].state == SLOT_BRUTAL_AI);
		CHECK(plan.slots[i].playerTemplate == PLAYERTEMPLATE_RANDOM);
		CHECK(plan.slots[i].color == i - 1);
		CHECK(plan.slots[i].startPosition == i - 1);
		CHECK(plan.slots[i].teamNumber == (i <= 4 ? 0 : 1));
	}

	SkirmishAITestPlan plan4v2;
	BuildSkirmishAITestPlan(1730, SKIRMISH_AI_TEST_SCENARIO_4V2, &plan4v2);
	CHECK(plan4v2.seed == 1730);
	CHECK(strcmp(plan4v2.mapName, "Maps\\Twilight Flame\\Twilight Flame.map") == 0);
	CHECK(plan4v2.slots[0].state == SLOT_PLAYER);
	CHECK(plan4v2.slots[0].playerTemplate == PLAYERTEMPLATE_OBSERVER);
	for (Int slot4v2 = 1; slot4v2 <= 6; ++slot4v2)
	{
		CHECK(plan4v2.slots[slot4v2].state == SLOT_BRUTAL_AI);
		CHECK(plan4v2.slots[slot4v2].playerTemplate == PLAYERTEMPLATE_RANDOM);
		CHECK(plan4v2.slots[slot4v2].color == slot4v2 - 1);
		CHECK(plan4v2.slots[slot4v2].startPosition == slot4v2 - 1);
		CHECK(plan4v2.slots[slot4v2].teamNumber == (slot4v2 <= 4 ? 0 : 1));
	}
	CHECK(plan4v2.slots[7].state == SLOT_CLOSED);
	CHECK(plan4v2.slots[7].playerTemplate == -1);
	CHECK(plan4v2.slots[7].color == -1);
	CHECK(plan4v2.slots[7].startPosition == -1);
	CHECK(plan4v2.slots[7].teamNumber == -1);

	const UnsignedInt expectedMapCRC = 0x12345678U;
	const UnsignedInt expectedMapSize = 0x00123456U;
	SkirmishAITestLoadedState loadedState = {
		"maps\\twilight flame\\twilight flame.map",
		"MAPS\\TWILIGHT FLAME\\TWILIGHT FLAME.MAP",
		"Maps\\Twilight Flame\\Twilight Flame.map",
		expectedMapCRC,
		expectedMapSize,
		1729
	};
	CHECK(IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, nullptr));
	loadedState.gameInfoMapName = "Maps\\Tournament Desert\\Tournament Desert.map";
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.gameInfoMapName = plan.mapName;
	loadedState.globalMapName = nullptr;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.globalMapName = plan.mapName;
	loadedState.terrainMapName = "Maps\\Tournament Desert\\Tournament Desert.map";
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.terrainMapName = plan.mapName;
	loadedState.mapCRC ^= 1U;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.mapCRC = expectedMapCRC;
	loadedState.mapSize++;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));
	loadedState.mapSize = expectedMapSize;
	loadedState.seed++;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan, expectedMapCRC, expectedMapSize, &loadedState));

	SkirmishAITestLoadedState loaded4v2 = {
		"maps\\twilight flame\\twilight flame.map",
		"MAPS\\TWILIGHT FLAME\\TWILIGHT FLAME.MAP",
		"Maps\\Twilight Flame\\Twilight Flame.map",
		expectedMapCRC,
		expectedMapSize,
		1730
	};
	CHECK(IsExpectedSkirmishAITestLoadedState(plan4v2, expectedMapCRC, expectedMapSize, &loaded4v2));
	loaded4v2.seed = 1729;
	CHECK(!IsExpectedSkirmishAITestLoadedState(plan4v2, expectedMapCRC, expectedMapSize, &loaded4v2));

	CHECK(EvaluateSkirmishAITestProgress(0, 0) == SKIRMISH_AI_TEST_RUNNING);
	CHECK(EvaluateSkirmishAITestProgress(0, 107999) == SKIRMISH_AI_TEST_RUNNING);
	CHECK(EvaluateSkirmishAITestProgress(42000, 42001) == SKIRMISH_AI_TEST_COMPLETE);
	CHECK(EvaluateSkirmishAITestProgress(0, 108000) == SKIRMISH_AI_TEST_TIMED_OUT);
	CHECK(!IsSkirmishAITestStartupTimedOut(299999));
	CHECK(IsSkirmishAITestStartupTimedOut(300000));
	CHECK(!IsSkirmishAITestProgressStalled(29999));
	CHECK(IsSkirmishAITestProgressStalled(30000));
	CHECK(!IsSkirmishAITestShutdownTimedOut(29999));
	CHECK(IsSkirmishAITestShutdownTimedOut(30000));

	if (s_failures != 0)
	{
		printf("%d Generals skirmish AI runner contract test(s) failed.\n", s_failures);
		return 1;
	}

	printf("All Generals skirmish AI runner contract tests passed.\n");
	return 0;
}
