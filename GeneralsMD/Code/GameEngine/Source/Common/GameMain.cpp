/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// GameMain.cpp
// The main entry point for the game
// Author: Michael S. Booth, April 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GameThreadOwnership.h"
#include "Common/GlobalData.h"
#include "Common/ReplaySimulation.h"
#include "Common/SkirmishAITestRunner.h"
#include "GameNetwork/InstalledNet3Validation.h"
#if defined(_WIN64)
#include "Common/Stage5PerformanceFixtureRunner.h"
#include "GameNetwork/InstalledLockstepV2Validation.h"
#endif


/**
 * This is the entry point for the game system.
 */
Int GameMain()
{
	GameThreadOwnership::AttachCurrentThread();
	int exitcode = 0;
	// initialize the game engine using factory function
	TheFramePacer = new FramePacer();
	TheFramePacer->enableFramesPerSecondLimit(TRUE);
	TheGameEngine = CreateGameEngine();
	TheGameEngine->init();
	const Bool net3ValidationRequested = rts::IsInstalledNet3ValidationRequested();
	Bool lockstepV2ValidationRequested = FALSE;
	Bool performanceFixtureRequested = FALSE;
#if defined(_WIN64)
	performanceFixtureRequested = IsStage5PerformanceFixtureRequested();
	lockstepV2ValidationRequested =
		rts::IsInstalledLockstepV2QualificationRequested() ? TRUE : FALSE;
#endif
	const Bool skirmishValidationRequested =
		TheGlobalData->m_commandLineData.hasSkirmishAITestRequest() ||
		TheGlobalData->m_commandLineData.hasSkirmishAITest4v2Request() ||
		TheGlobalData->m_commandLineData.hasSkirmishAITestPractical1v7Request();
	const Bool validationOptionsConflict =
		(performanceFixtureRequested && (net3ValidationRequested || lockstepV2ValidationRequested ||
			skirmishValidationRequested || !TheGlobalData->m_simulateReplays.empty())) ||
		(net3ValidationRequested && lockstepV2ValidationRequested) ||
		((net3ValidationRequested || lockstepV2ValidationRequested) &&
			skirmishValidationRequested) ||
		((net3ValidationRequested || lockstepV2ValidationRequested) &&
			!TheGlobalData->m_simulateReplays.empty());
	if (validationOptionsConflict)
	{
		printf("VALIDATION_FAIL reason=installed_validation_options_are_mutually_exclusive\n");
		fflush(stdout);
		exitcode = 2;
		TheGameEngine->setQuitting(TRUE);
	}
	else if (net3ValidationRequested)
	{
		exitcode = rts::RunInstalledNet3Validation(
			TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC);
		TheGameEngine->setQuitting(TRUE);
	}
#if defined(_WIN64)
	else if (lockstepV2ValidationRequested)
	{
		Bool serviceSucceeded =
			TheGameEngine->prepareHeadlessSimulationJobsForInstalledQualification();
		if (serviceSucceeded)
			serviceSucceeded = rts::PrepareInstalledLockstepV2Qualification(
			TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC) ? TRUE : FALSE;
		while (serviceSucceeded &&
			!rts::IsInstalledLockstepV2ProofStarted() &&
			!rts::IsInstalledLockstepV2QualificationFailed())
		{
			serviceSucceeded = rts::ServiceInstalledLockstepV2Qualification(
				TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC) ? TRUE : FALSE;
		}
		if (serviceSucceeded && rts::IsInstalledLockstepV2ProofStarted())
			TheGameEngine->execute();
		const Bool clean = serviceSucceeded &&
			!rts::IsInstalledLockstepV2QualificationFailed() &&
			rts::IsInstalledLockstepV2StopRequested();
		if (!rts::FinalizeInstalledLockstepV2Qualification(clean != FALSE))
			serviceSucceeded = FALSE;
		exitcode = serviceSucceeded && clean ? 0 : 2;
		TheGameEngine->setQuitting(TRUE);
	}
#endif
	if (!net3ValidationRequested && !lockstepV2ValidationRequested)
	{
		if (TheGlobalData->m_commandLineData.hasSkirmishAITestRequest())
			ArmSkirmishAITestRunner(
				TheGlobalData->m_commandLineData.getSkirmishAITestSeed(),
				SKIRMISH_AI_TEST_SCENARIO_4V3);
		else if (TheGlobalData->m_commandLineData.hasSkirmishAITest4v2Request())
			ArmSkirmishAITestRunner(
				TheGlobalData->m_commandLineData.getSkirmishAITest4v2Seed(),
				SKIRMISH_AI_TEST_SCENARIO_4V2);
		else if (TheGlobalData->m_commandLineData.hasSkirmishAITestPractical1v7Request())
			ArmSkirmishAITestRunner(
				TheGlobalData->m_commandLineData.getSkirmishAITestPractical1v7Seed(),
				SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7);
	}
	const Bool canRun = !validationOptionsConflict && !net3ValidationRequested &&
		!lockstepV2ValidationRequested &&
		StartSkirmishAITestRunner()
#if defined(_WIN64)
		&& StartStage5PerformanceFixtureRunner()
#endif
		;

	if (!canRun)
	{
		TheGameEngine->setQuitting(TRUE);
	}
	else if (!TheGlobalData->m_simulateReplays.empty())
	{
		exitcode = ReplaySimulation::simulateReplays(TheGlobalData->m_simulateReplays, TheGlobalData->m_simulateReplayJobs);
	}
	else
	{
		// run it
		TheGameEngine->execute();
	}
	if (IsSkirmishAITestRunnerArmed())
		exitcode = FinalizeSkirmishAITestRunner(exitcode);
#if defined(_WIN64)
	if (performanceFixtureRequested)
		exitcode = FinalizeStage5PerformanceFixtureRunner(exitcode);
#endif

	// since execute() returned, we are exiting the game
	delete TheFramePacer;
	TheFramePacer = nullptr;
	delete TheGameEngine;
	TheGameEngine = nullptr;
#if defined(_WIN64)
	FinalizeSkirmishAITestPerformanceReceipt(exitcode);
#endif
	GameThreadOwnership::DetachCurrentThread();

	return exitcode;
}

