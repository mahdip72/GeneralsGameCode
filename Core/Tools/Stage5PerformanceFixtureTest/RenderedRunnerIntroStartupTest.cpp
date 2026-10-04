/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifdef RTS_ZEROHOUR
#undef RTS_ZEROHOUR
#endif
#define RTS_ZEROHOUR 1

#include <stdio.h>

typedef bool Bool;
enum { FALSE = 0, TRUE = 1 };

struct QueuedSaveGame
{
	QueuedSaveGame() : m_notEmpty(FALSE) {}
	Bool isNotEmpty() const { return m_notEmpty; }
	Bool m_notEmpty;
};

struct GlobalData
{
	GlobalData() : m_headless(FALSE), m_breakTheMovie(FALSE) {}
	Bool m_headless;
	Bool m_breakTheMovie;
	QueuedSaveGame m_loadSaveGame;
};

GlobalData globalData;
GlobalData *TheGlobalData = &globalData;
GlobalData *TheWritableGlobalData = &globalData;

struct ShellStub
{
	ShellStub() : m_showMapCalls(0), m_showShellCalls(0) {}
	void showShellMap(Bool) { ++m_showMapCalls; }
	void showShell() { ++m_showShellCalls; }
	unsigned m_showMapCalls;
	unsigned m_showShellCalls;
} shellStub;

ShellStub *TheShell = &shellStub;

struct GameStateStub
{
	GameStateStub() : m_queuedSaveLoads(0) {}
	void loadQueuedSaveGame() { ++m_queuedSaveLoads; }
	unsigned m_queuedSaveLoads;
} gameStateStub;

GameStateStub *TheGameState = &gameStateStub;

Bool s_skirmishAITestRunnerArmed = FALSE;
Bool s_skirmishAILegacySaveTestActive = FALSE;

Bool IsSkirmishAITestRunnerArmed()
{
	return s_skirmishAITestRunnerArmed;
}

Bool IsSkirmishAILegacySaveTestActive()
{
	return s_skirmishAILegacySaveTestActive;
}

struct IntroStub
{
	explicit IntroStub(Bool done) : m_done(done), m_updateCalls(0) {}
	~IntroStub() { ++s_destructions; }
	void update() { ++m_updateCalls; }
	Bool isDone() const { return m_done; }

	static unsigned s_destructions;
	Bool m_done;
	unsigned m_updateCalls;
};

unsigned IntroStub::s_destructions = 0;

struct GameClientIntroFixture
{
	GameClientIntroFixture() : m_intro(nullptr) {}
	~GameClientIntroFixture() { delete m_intro; }

	void updateZeroHourIntro()
	{
#include "RenderedRunnerIntroZeroHourUnderTest.inc"
	}

	void updateGeneralsIntro()
	{
#include "RenderedRunnerIntroGeneralsUnderTest.inc"
	}

	IntroStub *m_intro;
};

typedef void (GameClientIntroFixture::*IntroUpdate)();
unsigned s_failures = 0;

void Check(const char *title, Bool condition, const char *message)
{
	if (!condition)
	{
		++s_failures;
		printf("FAIL [%s]: %s\n", title, message);
	}
}

void ResetCase(Bool runnerArmed, Bool legacySaveActive, Bool headless,
	Bool movieBlocked)
{
	s_skirmishAITestRunnerArmed = runnerArmed;
	s_skirmishAILegacySaveTestActive = legacySaveActive;
	TheGlobalData->m_headless = headless;
	TheGlobalData->m_breakTheMovie = movieBlocked;
	TheGlobalData->m_loadSaveGame.m_notEmpty = FALSE;
	shellStub.m_showMapCalls = 0;
	shellStub.m_showShellCalls = 0;
	gameStateStub.m_queuedSaveLoads = 0;
	IntroStub::s_destructions = 0;
}

void CheckRenderedRunnerClears(const char *title, IntroUpdate update)
{
	ResetCase(TRUE, FALSE, FALSE, TRUE);
	GameClientIntroFixture client;
	client.m_intro = new IntroStub(TRUE);
	(client.*update)();
	Check(title, TheGlobalData->m_breakTheMovie == FALSE,
		"rendered runner completion clears the stale movie block");
	Check(title, client.m_intro == nullptr && IntroStub::s_destructions == 1,
		"completed intro is destroyed at the handoff");
	Check(title, shellStub.m_showMapCalls == 0 && shellStub.m_showShellCalls == 0,
		"rendered runner continues to skip main-menu startup");
}

void CheckHeadlessRunnerPreservesMovieBlock(const char *title, IntroUpdate update)
{
	ResetCase(TRUE, FALSE, TRUE, TRUE);
	GameClientIntroFixture client;
	client.m_intro = new IntroStub(TRUE);
	(client.*update)();
	Check(title, TheGlobalData->m_breakTheMovie == TRUE,
		"headless runner does not clear movie state");
	Check(title, shellStub.m_showMapCalls == 0 && shellStub.m_showShellCalls == 0,
		"headless runner continues to skip main-menu startup");
}

void CheckOrdinaryIntroKeepsMenuBehavior(const char *title, IntroUpdate update)
{
	ResetCase(FALSE, FALSE, FALSE, TRUE);
	GameClientIntroFixture client;
	client.m_intro = new IntroStub(TRUE);
	(client.*update)();
	Check(title, TheGlobalData->m_breakTheMovie == TRUE,
		"ordinary intro does not clear movie state");
	Check(title, shellStub.m_showMapCalls == 1 && shellStub.m_showShellCalls == 1,
		"ordinary intro still opens the main-menu shell");
}

void CheckUnfinishedIntroDoesNotClear(const char *title, IntroUpdate update)
{
	ResetCase(TRUE, FALSE, FALSE, TRUE);
	GameClientIntroFixture client;
	client.m_intro = new IntroStub(FALSE);
	(client.*update)();
	Check(title, TheGlobalData->m_breakTheMovie == TRUE,
		"unfinished intro does not clear movie state");
	Check(title, client.m_intro != nullptr && client.m_intro->m_updateCalls == 1,
		"unfinished intro remains active");
	Check(title, shellStub.m_showMapCalls == 0 && shellStub.m_showShellCalls == 0,
		"unfinished intro does not open the menu early");
}

void CheckLegacySaveKeepsMovieState()
{
	ResetCase(FALSE, TRUE, FALSE, TRUE);
	TheGlobalData->m_loadSaveGame.m_notEmpty = TRUE;
	GameClientIntroFixture client;
	client.m_intro = new IntroStub(TRUE);
	client.updateZeroHourIntro();
	Check("Zero Hour", TheGlobalData->m_breakTheMovie == TRUE,
		"legacy-save startup does not clear movie state");
	Check("Zero Hour", shellStub.m_showMapCalls == 0 && shellStub.m_showShellCalls == 0,
		"legacy-save startup continues to skip the main menu");
	Check("Zero Hour", gameStateStub.m_queuedSaveLoads == 1,
		"legacy-save startup still loads the queued save");
}

int main()
{
	CheckRenderedRunnerClears("Zero Hour", &GameClientIntroFixture::updateZeroHourIntro);
	CheckHeadlessRunnerPreservesMovieBlock("Zero Hour", &GameClientIntroFixture::updateZeroHourIntro);
	CheckOrdinaryIntroKeepsMenuBehavior("Zero Hour", &GameClientIntroFixture::updateZeroHourIntro);
	CheckUnfinishedIntroDoesNotClear("Zero Hour", &GameClientIntroFixture::updateZeroHourIntro);
	CheckLegacySaveKeepsMovieState();

	CheckRenderedRunnerClears("Generals", &GameClientIntroFixture::updateGeneralsIntro);
	CheckHeadlessRunnerPreservesMovieBlock("Generals", &GameClientIntroFixture::updateGeneralsIntro);
	CheckOrdinaryIntroKeepsMenuBehavior("Generals", &GameClientIntroFixture::updateGeneralsIntro);
	CheckUnfinishedIntroDoesNotClear("Generals", &GameClientIntroFixture::updateGeneralsIntro);

	if (s_failures != 0)
		return 1;
	printf("Rendered runner intro startup tests passed.\n");
	return 0;
}
