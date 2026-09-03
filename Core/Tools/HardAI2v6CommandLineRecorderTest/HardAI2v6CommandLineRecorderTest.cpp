/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
**
** Native x64 source-connected contract for true8AI2v6 command-line dispatch,
** GameMain scenario arming, and Recorder local-index compatibility.
**
** This target consumes extracted production source and bounded dependency
** doubles. It does not implement or alter the production paths.
*/

#if !defined(_WIN64)
#error This source-connected test is native-x64 only.
#endif
#if defined(RTS_DEBUG) || defined(DEBUG_LOGGING) || defined(DEBUG_CRC) || defined(DEBUG_CRASHING) || \
	defined(DEBUG_STACKTRACE) || defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
#error This source-connected test must preserve native Release parser/table guards.
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

typedef bool Bool;
typedef int Int;
typedef unsigned int UnsignedInt;
const Bool TRUE = true;
const Bool FALSE = false;

#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#define NEW new
#define strnicmp _strnicmp

namespace hard_ai_2v6_fixture
{

class CommandLine;
class GlobalData;

class CommandLine
{
public:
	static void parseCommandLineForStartup();
	static void parseCommandLineForEngineInit();
};

class AsciiString
{
public:
	AsciiString() : m_value() {}
	AsciiString(const char *value) : m_value(value == 0 ? "" : value) {}
	AsciiString(const std::string &value) : m_value(value) {}

	const char *str() const { return m_value.c_str(); }
	void clear() { m_value.clear(); }
	void set(const char *value) { m_value = value == 0 ? "" : value; }
	void format(const char *format, Int value)
	{
		char buffer[32];
		sprintf(buffer, format, value);
		m_value = buffer;
	}
	Bool endsWithNoCase(const char *suffix) const
	{
		const size_t suffixLength = strlen(suffix);
		if (m_value.size() < suffixLength)
			return FALSE;
		for (size_t i = 0; i < suffixLength; ++i)
		{
			char left = m_value[m_value.size() - suffixLength + i];
			char right = suffix[i];
			if (left >= 'A' && left <= 'Z') left = static_cast<char>(left - 'A' + 'a');
			if (right >= 'A' && right <= 'Z') right = static_cast<char>(right - 'A' + 'a');
			if (left != right) return FALSE;
		}
		return TRUE;
	}
	Bool operator==(const char *value) const { return m_value == (value == 0 ? "" : value); }

private:
	std::string m_value;
};

// The title header is extracted by CMake, so the request conflict rules under
// test are the actual CommandLineData methods, not a duplicated test model.
#include HARD_AI2V6_GLOBAL_DATA_COMMAND_LINE_DATA_INC

struct GlobalData
{
	GlobalData()
		: m_headless(FALSE), m_windowed(FALSE), m_playIntro(TRUE), m_playSizzle(TRUE),
		  m_shellMapOn(TRUE), m_useFpsLimit(TRUE), m_framesPerSecondLimit(30),
		  m_simulateReplayJobs(-1), m_simulateReplays(), m_commandLineData() {}

	CommandLineData m_commandLineData;
	Bool m_headless;
	Bool m_windowed;
	Bool m_playIntro;
	Bool m_playSizzle;
	Bool m_shellMapOn;
	Bool m_useFpsLimit;
	Int m_framesPerSecondLimit;
	Int m_simulateReplayJobs;
	std::vector<AsciiString> m_simulateReplays;
};

struct Scenario
{
	Scenario()
		: command(), multiInstanceCalls(0), skipPrimaryCalls(0), armCalls(0),
		  armedSeed(0), armedScenario(-1), rendererWindowed(TRUE) {}

	std::string command;
	unsigned multiInstanceCalls;
	unsigned skipPrimaryCalls;
	unsigned armCalls;
	Int armedSeed;
	Int armedScenario;
	Bool rendererWindowed;
};

static Scenario s_scenario;
static GlobalData s_globalData;
GlobalData *TheGlobalData = &s_globalData;
GlobalData *TheWritableGlobalData = &s_globalData;

namespace rts
{
namespace render
{
Bool GameRenderer_IsWindowed = TRUE;
}

namespace fixture
{
struct Request
{
	Request() : requested(FALSE) {}
	Bool requested;
};

static Bool ParseCommandLine(Int, const char *const *, Bool, Request *request,
	const char **error)
{
	if (request != 0)
		request->requested = FALSE;
	if (error != 0)
		*error = 0;
	return TRUE;
}
}

class JobSystem
{
public:
	static void setStartupWorkerCount(unsigned) {}
	static Bool setStartupWorkerPolicy(const char *) { return TRUE; }
};

class ClientInstance
{
public:
	static void setMultiInstance(Bool) { ++s_scenario.multiInstanceCalls; }
	static void skipPrimaryInstance() { ++s_scenario.skipPrimaryCalls; }
};
}

static Bool ConfigureStage5PerformanceFixture(const rts::fixture::Request &)
{
	return TRUE;
}

class RecorderClass
{
public:
	static const char *getReplayExtention() { return ".rep"; }
};

// This enum is extracted from the production runner header.  The test never
// restates its numeric values, including the old practical alias.
#include HARD_AI2V6_SCENARIO_ENUM_INC

void ArmSkirmishAITestRunner(Int seed, SkirmishAITestScenario scenario)
{
	++s_scenario.armCalls;
	s_scenario.armedSeed = seed;
	s_scenario.armedScenario = static_cast<Int>(scenario);
}

static const char *GetCommandLineA()
{
	return s_scenario.command.c_str();
}

struct CommandLineExit
{
	explicit CommandLineExit(Int value) : code(value) {}
	Int code;
};

struct UnexpectedCallback
{
	explicit UnexpectedCallback(const char *value) : name(value) {}
	const char *name;
};

static void CaptureExit(Int code)
{
	throw CommandLineExit(code);
}

// Match Common/Debug.h's native Release definitions for the extracted source.
#define DEBUG_ASSERTCRASH(condition, message) ((void)0)
#define DEBUG_LOG(message) ((void)0)

// The generated source includes every real table row.  Dependencies that are
// not part of this contract are generated as throwing stubs by CMake, so an
// accidental dispatch is a test failure rather than hidden behavior.
#define exit CaptureExit
#include HARD_AI2V6_COMMAND_LINE_SOURCE_INC
#undef exit

// The exact GameMain arming statement is extracted; all surrounding game
// lifecycle work is intentionally outside this focused contract.
static void RunActualGameMainArmingBranch()
{
	const Bool net3ValidationRequested = FALSE;
	Bool lockstepV2ValidationRequested = FALSE;
#include HARD_AI2V6_GAME_MAIN_ARM_SOURCE_INC
}

static unsigned s_failures = 0;

static void Check(Bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++s_failures;
	}
}

static void Reset(const char *command)
{
	s_scenario = Scenario();
	s_scenario.command = command == 0 ? "game" : command;
	s_globalData = GlobalData();
	TheGlobalData = &s_globalData;
	TheWritableGlobalData = &s_globalData;
	rts::render::GameRenderer_IsWindowed = TRUE;
}

static Bool RunPass(Bool startup, Int *exitCode)
{
	*exitCode = 0;
	try
	{
		if (startup)
			CommandLine::parseCommandLineForStartup();
		else
			CommandLine::parseCommandLineForEngineInit();
		return TRUE;
	}
	catch (const CommandLineExit &value)
	{
		*exitCode = value.code;
		return FALSE;
	}
}

static Bool RunFullCommandLine(Int *exitCode)
{
	if (!RunPass(TRUE, exitCode))
		return FALSE;
	return RunPass(FALSE, exitCode);
}

static void TestHardFlagCapturesExactlyOnce()
{
	Reset("game -runSkirmishAITestHardAI2v6 1733");
	Int exitCode = 0;
	Check(RunFullCommandLine(&exitCode), "hard flag startup/init succeeds");
	Check(exitCode == 0, "hard flag has no nonzero exit");
	Check(s_globalData.m_headless == TRUE && s_globalData.m_playIntro == FALSE &&
		s_globalData.m_playSizzle == FALSE && s_globalData.m_shellMapOn == FALSE &&
		s_globalData.m_useFpsLimit == FALSE &&
		rts::render::GameRenderer_IsWindowed == FALSE,
		"hard flag alone applies the complete headless startup controls");
	Check(s_globalData.m_commandLineData.hasSkirmishAITestHardAI2v6Request(),
		"hard flag sets the hard request");
	Check(s_globalData.m_commandLineData.getSkirmishAITestHardAI2v6Seed() == 1733,
		"hard flag preserves its seed");
	Check(s_scenario.multiInstanceCalls == 1 && s_scenario.skipPrimaryCalls == 1,
		"hard startup side effects run once");
	Check(RunPass(TRUE, &exitCode), "repeated startup is inert");
	Check(s_scenario.multiInstanceCalls == 1 && s_scenario.skipPrimaryCalls == 1,
		"startup does not dispatch twice");

	RunActualGameMainArmingBranch();
	Check(s_scenario.armCalls == 1 && s_scenario.armedSeed == 1733 &&
		s_scenario.armedScenario == static_cast<Int>(SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6),
		"actual GameMain arms hard-ai-2v6");
}

static void TestInvalidHardSeedsAreFatal()
{
	const char *commands[] =
	{
		"game -runSkirmishAITestHardAI2v6",
		"game -runSkirmishAITestHardAI2v6 0",
		"game -runSkirmishAITestHardAI2v6 -1",
		"game -runSkirmishAITestHardAI2v6 bogus",
	};
	for (unsigned i = 0; i < ARRAY_SIZE(commands); ++i)
	{
		Reset(commands[i]);
		Int exitCode = 0;
		Check(!RunPass(TRUE, &exitCode) && exitCode == 2,
			"invalid hard seed is rejected before init");
		Check(!s_globalData.m_commandLineData.hasSkirmishAITestHardAI2v6Request(),
			"invalid hard seed never arms a request");
	}
}

static void TestHardAndExistingModesAreMutuallyExclusive()
{
	Reset("game -runSkirmishAITestHardAI2v6 1733 -runSkirmishAITestHardAI2v6 1734");
	Int duplicateExitCode = 0;
	Check(RunPass(TRUE, &duplicateExitCode), "duplicate hard startup remains accepted");
	Check(!RunPass(FALSE, &duplicateExitCode) && duplicateExitCode == 2,
		"duplicate hard flags fail as duplicate_option");

	const char *existing[] =
	{
		"-runSkirmishAITest 101",
		"-runSkirmishAITest4v2 102",
		"-runSkirmishAITestPractical1v7 103",
	};
	for (unsigned i = 0; i < ARRAY_SIZE(existing); ++i)
	{
		const char *orders[] = { existing[i], "-runSkirmishAITestHardAI2v6 1733" };
		for (unsigned order = 0; order < 2; ++order)
		{
			std::string command = "game ";
			if (order == 0)
				command += orders[0], command += " ", command += orders[1];
			else
				command += orders[1], command += " ", command += orders[0];
			Reset(command.c_str());
			Int exitCode = 0;
			Check(RunPass(TRUE, &exitCode), "mutual exclusion startup remains accepted");
			Check(!RunPass(FALSE, &exitCode) && exitCode == 2,
				"hard and existing scenario fail as duplicate_option");
		}
	}
}

static void TestExistingModesRemainInert()
{
	const char *commands[] =
	{
		"game -runSkirmishAITest 101",
		"game -runSkirmishAITest4v2 102",
		"game -runSkirmishAITestPractical1v7 103",
	};
	for (unsigned i = 0; i < ARRAY_SIZE(commands); ++i)
	{
		Reset(commands[i]);
		Int exitCode = 0;
		Check(RunFullCommandLine(&exitCode), "existing scenario remains accepted");
		Check(!s_globalData.m_commandLineData.hasSkirmishAITestHardAI2v6Request(),
			"existing scenario does not set hard request");
		RunActualGameMainArmingBranch();
		Check(s_scenario.armCalls == 1 &&
			s_scenario.armedScenario != static_cast<Int>(SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6),
			"existing scenario does not arm hard-ai-2v6");
	}
}

// -------------------------------------------------------------------------
// Recorder source-connected field round trip.

struct ReplaySlot
{
	ReplaySlot() : ip(0), human(FALSE) {}
	Int getIP() const { return ip; }
	Int ip;
	Bool human;
};

struct ReplayGameInfo
{
	ReplayGameInfo() : localIP(0), localIPSet(FALSE), slots(), slotList() {}
	void setCRCInterval(Int) {}
	Int getLocalIP() const { return localIP; }
	Int getLocalSlotNum() const
	{
		for (Int i = 0; i < 8; ++i)
			if (slots[i].human && slots[i].ip == localIP)
				return i;
		return -1;
	}
	ReplaySlot *getSlot(Int index) { return &slots[index]; }
	const ReplaySlot *getConstSlot(Int index) const { return &slots[index]; }
	void setLocalIP(Int value) { localIP = value; localIPSet = TRUE; }
	void reset() { localIP = 0; localIPSet = FALSE; }
	void enterGame() {}
	void startGame(Int) {}
	Int localIP;
	Bool localIPSet;
	ReplaySlot slots[8];
	AsciiString slotList;
};

// The extracted legacy writer names its LAN object `GameInfo`; this alias is
// only the dependency fixture for that unchanged source block.
typedef ReplayGameInfo GameInfo;

struct ReplayHeader
{
	ReplayHeader() : forPlayback(FALSE), localPlayerIndex(-99), gameOptions() {}
	Bool forPlayback;
	Int localPlayerIndex;
	AsciiString gameOptions;
};

static ReplayGameInfo *TheSkirmishGameInfo = 0;
static ReplayGameInfo *TheGameSpyGame = 0;
static Bool TheNetwork = FALSE;
static const Int MAX_SLOTS = 8;
static const Int REPLAY_CRC_INTERVAL = 30;

struct LANFixture
{
	ReplayGameInfo *GetMyGame() { return 0; }
};
static LANFixture *TheLAN = 0;

struct ReplayFile
{
	ReplayFile() : fields() {}
	std::vector<std::string> fields;
};

static ReplayFile s_replayFile;

static AsciiString GameInfoToAsciiString(const ReplayGameInfo *game)
{
	return game->slotList;
}

static Bool writeNativeReplayAsciiString(ReplayFile *file, const char *value)
{
	file->fields.push_back(value == 0 ? "" : value);
	return TRUE;
}

class RecorderLocalIndexSlice
{
public:
	RecorderLocalIndexSlice() : m_file(&s_replayFile), m_gameInfo(), m_originalGameMode(0) {}

	void logGameStart(AsciiString) {}

	void writeSkirmishHeader()
	{
		Bool nativeHeaderWriteOk = TRUE;
#include HARD_AI2V6_RECORDER_WRITER_SOURCE_INC
	}

	Bool failReplayHeaderRead()
	{
		return FALSE;
	}

	Bool decodeAndApplyLocalIndex(const char *serialized, Int *decoded)
	{
		ReplayHeader header;
		AsciiString playerIndex(serialized);
#include HARD_AI2V6_RECORDER_READER_PARSE_SOURCE_INC
		*decoded = header.localPlayerIndex;
		return TRUE;
	}

	void applyLocalIndex(ReplayHeader &header)
	{
#include HARD_AI2V6_RECORDER_READER_APPLY_SOURCE_INC
	}

	ReplayFile *m_file;
	ReplayGameInfo m_gameInfo;
	Int m_originalGameMode;
};

static void TestRecorderLocalIndexRoundTrip()
{
	ReplayGameInfo hardInfo;
	for (Int i = 0; i < 8; ++i)
	{
		hardInfo.slots[i].human = FALSE;
		hardInfo.slots[i].ip = 0;
	}
	hardInfo.slotList = AsciiString(
		"M=00;MC=ABCD;MS=1234;SD=1733;C=30;S=CH,0,100,0,0:CH,1,100,1,0:CH,2,100,2,1:CH,3,100,3,1:CH,4,100,4,1:CH,5,100,5,1:CH,6,100,6,1:CH,7,100,7,1:;");
	TheSkirmishGameInfo = &hardInfo;
	TheNetwork = FALSE;
	s_replayFile.fields.clear();
	RecorderLocalIndexSlice hard;
	hard.writeSkirmishHeader();
	const Bool hardWriterFieldsPresent = s_replayFile.fields.size() >= 2;
	Check(hardWriterFieldsPresent &&
		s_replayFile.fields[0] ==
			"M=00;MC=ABCD;MS=1234;SD=1733;C=30;S=CH,0,100,0,0:CH,1,100,1,0:CH,2,100,2,1:CH,3,100,3,1:CH,4,100,4,1:CH,5,100,5,1:CH,6,100,6,1:CH,7,100,7,1:;" &&
		s_replayFile.fields[1] == "-1",
		"all-AI skirmish writes localIndex -1");
	Int decoded = -99;
	Check(hard.decodeAndApplyLocalIndex("-1", &decoded) && decoded == -1,
		"all-AI localIndex -1 is accepted by actual reader parser");
	ReplayHeader hardHeader;
	hardHeader.localPlayerIndex = decoded;
	hard.applyLocalIndex(hardHeader);
	Check(!hard.m_gameInfo.localIPSet && hard.m_gameInfo.getLocalSlotNum() == -1,
		"all-AI replay remains without a GameInfo local slot");

	ReplayGameInfo humanInfo;
	humanInfo.slots[0].human = TRUE;
	humanInfo.slots[0].ip = 0;
	humanInfo.slotList = AsciiString(
		"M=00;MC=ABCD;MS=1234;SD=1733;C=30;S=HObserver,0,0,TF,0,0,0,0,0:CH,1,100,1,0:CH,2,100,2,1:CH,3,100,3,1:CH,4,100,4,1:CH,5,100,5,1:CH,6,100,6,1:CH,7,100,7,1:;");
	for (Int i = 1; i < 8; ++i)
	{
		humanInfo.slots[i].human = FALSE;
		humanInfo.slots[i].ip = 0;
	}
	TheSkirmishGameInfo = &humanInfo;
	s_replayFile.fields.clear();
	RecorderLocalIndexSlice human;
	human.writeSkirmishHeader();
	const Bool humanWriterFieldsPresent = s_replayFile.fields.size() >= 2;
	Check(humanWriterFieldsPresent && s_replayFile.fields[1] == "0",
		"human slot zero retains localIndex zero");
	decoded = -99;
	Check(human.decodeAndApplyLocalIndex("0", &decoded) && decoded == 0,
		"human localIndex zero is accepted by actual reader parser");
	ReplayHeader humanHeader;
	humanHeader.localPlayerIndex = decoded;
	human.m_gameInfo = humanInfo;
	human.applyLocalIndex(humanHeader);
	Check(human.m_gameInfo.localIPSet && human.m_gameInfo.getLocalSlotNum() == 0,
		"human replay restores GameInfo local slot zero");
}

static void TestMalformedLocalIndexRemainsRejected()
{
	RecorderLocalIndexSlice recorder;
	const char *invalid[] = { "", "-2", "8", "0tail", "tail0" };
	for (unsigned i = 0; i < ARRAY_SIZE(invalid); ++i)
	{
		Int decoded = -99;
		Check(!recorder.decodeAndApplyLocalIndex(invalid[i], &decoded),
			"actual reader rejects malformed/out-of-range local index");
	}
}

} // namespace hard_ai_2v6_fixture

int main()
{
	using namespace hard_ai_2v6_fixture;
	TestHardFlagCapturesExactlyOnce();
	TestInvalidHardSeedsAreFatal();
	TestHardAndExistingModesAreMutuallyExclusive();
	TestExistingModesRemainInert();
	TestRecorderLocalIndexRoundTrip();
	TestMalformedLocalIndexRemainsRejected();
	if (s_failures != 0)
	{
		fprintf(stderr, "%u hard-ai-2v6 integration assertion(s) failed.\n", s_failures);
		return 1;
	}
	printf("Hard-ai-2v6 command-line/recorder integration assertions passed.\n");
	return 0;
}
