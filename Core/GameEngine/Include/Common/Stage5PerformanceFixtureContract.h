/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <time.h>

namespace rts { namespace fixture {

enum { SlotCount = 8, MapKeyCapacity = 260, MaximumFrameBudget = 108000 };
enum Title { UnknownTitle, GeneralsTitle, ZeroHourTitle };

struct Request
{
	Request() : requested(false), seed(0), frameBudget(0) { mapKey[0] = '\0'; }
	bool requested;
	char mapKey[MapKeyCapacity];
	int seed;
	unsigned frameBudget;
};

struct SlotPlan
{
	bool human;
	int factionIndex, color, startPosition, team;
	const char *mapOwner;
};

struct Plan
{
	SlotPlan slots[SlotCount];
};

struct ObservedSlot
{
	bool human, brutalAi, observer, playableSide, ownerResolvesToPlayer;
	int factionIndex, originalFactionIndex, color, originalColor;
	int startPosition, originalStartPosition, team;
	const char *factionName;
};

struct Completion
{
	Title title, replayTitle;
	unsigned frameBudget, endFrame, replayFrameCount;
	int winnerTeam, replayEpoch;
	bool naturalVictory, recorderClosed, desync, quitEarly, finalCrcKnown;
	bool loadedMapVerified, rosterVerified;
	unsigned observedPlayers, observedFrameSamples;
	time_t startTime, endTime;
};

enum Progress { Running, Complete, TimedOut };

inline char LowerAscii(char value)
{
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

inline bool SameToken(const char *left, const char *right)
{
	if (!left || !right) return false;
	while (*left && LowerAscii(*left) == LowerAscii(*right)) { ++left; ++right; }
	return *left == '\0' && *right == '\0';
}

inline bool Fail(const char **error, const char *reason)
{
	if (error) *error = reason;
	return false;
}

inline bool ParsePositive(const char *text, unsigned limit, unsigned *result)
{
	if (!text || !*text || !result) return false;
	unsigned value = 0;
	for (; *text; ++text)
	{
		if (*text < '0' || *text > '9') return false;
		const unsigned digit = static_cast<unsigned>(*text - '0');
		if (value > (limit - digit) / 10U) return false;
		value = value * 10U + digit;
	}
	if (value == 0 || value > limit) return false;
	*result = value;
	return true;
}

inline bool ReservedPathComponent(const char *text, size_t length)
{
	size_t baseLength = 0;
	while (baseLength < length && text[baseLength] != '.') ++baseLength;
	if (baseLength != 3 && baseLength != 4) return false;
	char name[5] = { 0 };
	for (size_t i = 0; i < baseLength; ++i) name[i] = LowerAscii(text[i]);
	return strcmp(name, "con") == 0 || strcmp(name, "prn") == 0 ||
		strcmp(name, "aux") == 0 || strcmp(name, "nul") == 0 ||
		(baseLength == 4 && name[3] >= '1' && name[3] <= '9' &&
			((name[0] == 'c' && name[1] == 'o' && name[2] == 'm') ||
			 (name[0] == 'l' && name[1] == 'p' && name[2] == 't')));
}

inline bool NormalizeMapKey(const char *text, char output[MapKeyCapacity])
{
	if (!text || !output) return false;
	const size_t length = strlen(text);
	if (length < 10 || length >= MapKeyCapacity ||
		LowerAscii(text[0]) != 'm' || LowerAscii(text[1]) != 'a' ||
		LowerAscii(text[2]) != 'p' || LowerAscii(text[3]) != 's' ||
		(text[4] != '\\' && text[4] != '/')) return false;
	if (!SameToken(text + length - 4, ".map")) return false;
	size_t component = 0;
	unsigned components = 0;
	for (size_t i = 0; i <= length; ++i)
	{
		const char value = text[i];
		if (value == '\0' || value == '\\' || value == '/')
		{
			const size_t componentLength = i - component;
			if (componentLength == 0 || text[component] == '.' || text[component] == ' ' ||
				text[i - 1] == '.' || text[i - 1] == ' ' ||
				ReservedPathComponent(text + component, componentLength)) return false;
			++components;
			component = i + 1;
			output[i] = value ? '\\' : '\0';
		}
		else
		{
			if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
				(value >= '0' && value <= '9') || value == '_' || value == '-' ||
				value == ' ' || value == '.')) return false;
			output[i] = value;
		}
	}
	if (components < 3) return false;
	memcpy(output, "Maps", 4);
	return true;
}

inline bool IsConflictingOption(const char *option)
{
	const char *conflicts[] = { "-replay", "-jobs", "-installedNet3Validation",
		"-installedLockstepV2Validation", "-runSkirmishAITest", "-runSkirmishAITest4v2",
		"-runSkirmishAITestPractical1v7", "-runSkirmishAITestHardAI2v6",
		"-loadsave", "-benchmark", "-map", "-file" };
	for (unsigned i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); ++i)
		if (SameToken(option, conflicts[i])) return true;
	return false;
}

// Validate the complete command line before any handler can start another
// validation lane. Only the explicit fixture option opts ordinary startup in.
inline bool ParseCommandLine(int argc, const char *const *argv, bool native,
	Request *request, const char **error)
{
	if (!request || argc < 0 || (argc != 0 && !argv)) return Fail(error, "invalid_arguments");
	*request = Request();
	if (error) *error = 0;
	int fixtureIndex = -1;
	bool headless = false;
	bool conflict = false;
	for (int i = 1; i < argc; ++i)
	{
		if (SameToken(argv[i], "-runStage5PerformanceFixture"))
		{
			if (fixtureIndex != -1) return Fail(error, "duplicate_option");
			fixtureIndex = i;
		}
		if (SameToken(argv[i], "-headless")) headless = true;
		if (IsConflictingOption(argv[i])) conflict = true;
	}
	if (fixtureIndex == -1) return true;
	if (!native) return Fail(error, "native_x64_required");
	if (!headless) return Fail(error, "explicit_headless_required");
	if (conflict) return Fail(error, "conflicting_option");
	if (argc - fixtureIndex < 4) return Fail(error, "missing_arguments");
	Request parsed;
	unsigned seed = 0;
	if (!NormalizeMapKey(argv[fixtureIndex + 1], parsed.mapKey)) return Fail(error, "invalid_map_key");
	if (!ParsePositive(argv[fixtureIndex + 2], INT_MAX, &seed)) return Fail(error, "invalid_seed");
	if (!ParsePositive(argv[fixtureIndex + 3], MaximumFrameBudget, &parsed.frameBudget))
		return Fail(error, "invalid_frame_budget");
	parsed.seed = static_cast<int>(seed);
	parsed.requested = true;
	*request = parsed;
	return true;
}

inline bool BuildPlan(int americaFactionIndex, Plan *plan)
{
	if (americaFactionIndex < 0 || !plan) return false;
	const char *owners[SlotCount] = { "teamplayer0", "teamSkirmishAmerica1",
		"teamSkirmishAmerica2", "teamSkirmishAmerica3", "teamSkirmishAmerica4",
		"teamSkirmishAmerica5", "teamSkirmishAmerica6", "teamSkirmishAmerica7" };
	for (int i = 0; i < SlotCount; ++i)
	{
		SlotPlan &slot = plan->slots[i];
		slot.human = i == 0;
		slot.factionIndex = americaFactionIndex;
		slot.color = slot.startPosition = i;
		slot.team = i < 4 ? 0 : 1;
		slot.mapOwner = owners[i];
	}
	return true;
}

inline bool IsExpectedRoster(const Plan &plan, const ObservedSlot *slots, unsigned count)
{
	if (!slots || count != SlotCount) return false;
	for (unsigned i = 0; i < SlotCount; ++i)
	{
		const SlotPlan &expected = plan.slots[i];
		const ObservedSlot &actual = slots[i];
		if (actual.human != expected.human || actual.brutalAi == expected.human ||
			actual.observer || !actual.playableSide || !actual.ownerResolvesToPlayer ||
			!SameToken(actual.factionName, "FactionAmerica") ||
			actual.factionIndex != expected.factionIndex ||
			actual.originalFactionIndex != expected.factionIndex ||
			actual.color != expected.color || actual.originalColor != expected.color ||
			actual.startPosition != expected.startPosition ||
			actual.originalStartPosition != expected.startPosition || actual.team != expected.team)
			return false;
	}
	return true;
}

inline Progress EvaluateProgress(unsigned endFrame, unsigned currentFrame, unsigned frameBudget)
{
	if (frameBudget == 0 || frameBudget > MaximumFrameBudget || endFrame > frameBudget) return TimedOut;
	if (endFrame != 0) return Complete;
	return currentFrame >= frameBudget ? TimedOut : Running;
}

inline bool IsValidCompletion(const Completion &value)
{
	const int expectedEpoch = value.title == GeneralsTitle ? 1 :
		(value.title == ZeroHourTitle ? 3 : 0);
	return expectedEpoch != 0 && value.replayTitle == value.title &&
		value.replayEpoch == expectedEpoch &&
		value.frameBudget > 0 && value.frameBudget <= MaximumFrameBudget &&
		value.endFrame > 0 && value.endFrame <= value.frameBudget &&
		value.replayFrameCount == value.endFrame + 1U &&
		(value.winnerTeam == 0 || value.winnerTeam == 1) &&
		value.naturalVictory && value.recorderClosed && !value.desync && !value.quitEarly &&
		value.finalCrcKnown && value.loadedMapVerified && value.rosterVerified &&
		value.observedPlayers == SlotCount && value.observedFrameSamples != 0 &&
		value.startTime > 0 && value.endTime >= value.startTime;
}

} }
