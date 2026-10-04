/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// CMake extracts the native title's complete production startup function and
// diagnostic mode-name helpers. Only the outer engine globals are local.
#include "Common/INI.h"
#include "Common/SkirmishAITestRunner.h"
#include "GameLogic/AIPathfind.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/ResourceIoPipeline.h"
#include "Lib/SimulationExecutionPolicy.h"

#include <stdio.h>

namespace headless_simulation_startup_fixture
{
struct GlobalData { Bool m_headless; };
GlobalData globalData = { TRUE };
GlobalData *TheGlobalData = &globalData;
Bool s_headlessSimulationJobSystemStartAttempted = FALSE;
Bool s_headlessSimulationJobSystemStarted = FALSE;
unsigned s_headlessSimulationWorkerCount = 0;
rts::SimulationExecutionMode s_requestedHeadlessSimulationMode =
	rts::SIMULATION_EXECUTION_SERIAL;
rts::PipelineExecutionMode s_requestedHeadlessPipelineMode =
	rts::PIPELINE_EXECUTION_PARALLEL;

#include "HeadlessSimulationStartupUnderTest.inc"

unsigned failures = 0;
void Check(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}

int RunHeadlessSimulationStartupTitleTests(const char *simulation, const char *pipeline)
{
	using namespace headless_simulation_startup_fixture;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	Check(rts::SetSimulationExecutionMode(simulation) &&
		rts::SetPipelineExecutionMode(pipeline) &&
		rts::JobSystem::setStartupWorkerCount(2), "fresh-process startup policies are accepted");
	if (failures) return 1;
	const rts::SimulationExecutionMode requestedSimulation = rts::GetSimulationExecutionMode();
	const rts::PipelineExecutionMode requestedPipeline = rts::GetPipelineExecutionMode();

	// Reproduce headless initialization: lazy compute is disabled, but texture
	// resource I/O owns a real separate thread and freezes the pipeline policy.
	jobs.shutdown();
	Check(!jobs.ensureStarted() && !jobs.isRunning() && jobs.workerCount() == 0,
		"unsafe initialization cannot lazily start compute workers");
	rts::ResourceIoPipeline resources;
	const bool ioStarted = resources.start(rts::ResourceIoConfig(), rts::JobGroup());
	Check(ioStarted && rts::IsPipelineExecutionModeLocked() && !jobs.isRunning(),
		"real pre-worker I/O ownership freezes the selected pipeline");
	if (!ioStarted) return 1;
	Check(!rts::IsSimulationExecutionModeLocked(), "simulation remains selectable before its safe boundary");

	startHeadlessSimulationJobsAfterUnsafeInitialization();
	const bool requiresWorkers = requestedSimulation != rts::SIMULATION_EXECUTION_SERIAL;
	Check(rts::GetPipelineExecutionMode() == requestedPipeline &&
		s_requestedHeadlessPipelineMode == requestedPipeline,
		"late compute startup preserves the immutable selected pipeline");
	Check(rts::GetSimulationExecutionMode() == requestedSimulation &&
		rts::IsSimulationExecutionModeLocked(), "parallel, shadow and serial simulation policies are preserved");
	Check(s_headlessSimulationJobSystemStarted == requiresWorkers &&
		jobs.isRunning() == requiresWorkers &&
		jobs.workerCount() == (requiresWorkers ? 2U : 0U) &&
		jobs.isCurrentThread(rts::JOB_OWNER_GAME) == requiresWorkers,
		"production startup creates real requested workers and registers the game owner");
	const unsigned epoch = rts::GetCollisionCandidateRuntimeMetrics().resetEpoch;
	startHeadlessSimulationJobsAfterUnsafeInitialization();
	Check(rts::GetCollisionCandidateRuntimeMetrics().resetEpoch == epoch &&
		jobs.workerCount() == (requiresWorkers ? 2U : 0U), "repeated startup preserves the active epoch and scheduler");

	jobs.shutdown();
	if (jobs.isCurrentThread(rts::JOB_OWNER_GAME))
		Check(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME), "game owner releases after compute drain");
	resources.shutdown();
	Check(resources.metrics().ownershipFailures == 0 && !jobs.isRunning(),
		"real I/O owner and workers shut down without ownership failures");
	if (failures) return 1;
	printf("Headless simulation startup title tests passed.\n");
	return 0;
}
