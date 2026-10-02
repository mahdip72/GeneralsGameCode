/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Linked into each title's existing runtime utility. CMake extracts that
// title's actual one-shot startup prelude, ending before pipeline policy or
// worker startup. The real metric reset/getter and collector functions stay
// linked from the product; only the outer headless/one-shot state is local.
#include "Common/INI.h"
#include "Common/SkirmishAITestRunner.h"
#include "GameLogic/AIPathfind.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"

#include <stdio.h>
#include <string.h>

namespace headless_metric_startup_fixture
{
struct GlobalData
{
	Bool m_headless;
};

GlobalData globalData = { FALSE };
GlobalData *TheGlobalData = &globalData;
Bool s_headlessSimulationJobSystemStartAttempted = FALSE;
rts::SimulationExecutionMode s_requestedHeadlessSimulationMode =
	rts::SIMULATION_EXECUTION_SERIAL;
rts::PipelineExecutionMode s_requestedHeadlessPipelineMode =
	rts::PIPELINE_EXECUTION_SERIAL;

#include "HeadlessMetricStartupUnderTest.inc"

unsigned failures = 0;

void Check(Bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

void ResetAllMetricFamilies()
{
	ResetDirectPathRuntimeMetrics();
	ResetOrdinaryPathRuntimeMetrics();
	rts::ResetCollisionCandidateRuntimeMetrics();
	rts::ResetImmutableSpatialRuntimeMetrics();
}

struct Samples
{
	Samples()
		: direct(GetDirectPathRuntimeMetrics()),
		  ordinary(GetOrdinaryPathRuntimeMetrics()),
		  collision(rts::GetCollisionCandidateRuntimeMetrics()),
		  spatial(rts::GetImmutableSpatialRuntimeMetrics())
	{
	}

	DirectPathRuntimeMetrics direct;
	OrdinaryPathRuntimeMetrics ordinary;
	rts::CollisionCandidateRuntimeMetrics collision;
	rts::ImmutableSpatialRuntimeMetrics spatial;
};

void CheckEpochs(const Samples &before, const Samples &after,
	unsigned increment, const char *context)
{
	Check(after.direct.resetEpoch == before.direct.resetEpoch + increment,
		context);
	Check(after.ordinary.resetEpoch == before.ordinary.resetEpoch + increment,
		context);
	Check(after.collision.resetEpoch == before.collision.resetEpoch + increment,
		context);
	Check(after.spatial.resetEpoch == before.spatial.resetEpoch + increment,
		context);
}

struct Collectors
{
	Collectors()
		: directAwaiting(TRUE), ordinaryAwaiting(TRUE),
		  collisionAwaiting(TRUE), spatialAwaiting(TRUE),
		  directHasActivity(FALSE)
	{
		memset(&directFrozen, 0, sizeof(directFrozen));
		memset(&ordinaryFrozen, 0, sizeof(ordinaryFrozen));
	}

	void capture(const Samples &current)
	{
		AccumulateSkirmishAITestDirectPathMetrics(&baseline.direct,
			current.direct, &directFrozen, &directHasActivity, &directAwaiting);
		AccumulateSkirmishAITestOrdinaryPathMetrics(&baseline.ordinary,
			current.ordinary, &ordinaryFrozen, &ordinaryAwaiting);
		AccumulateSkirmishAITestCollisionMetrics(&baseline.collision,
			current.collision, &collisionFrozen, &collisionAwaiting);
		AccumulateSkirmishAITestImmutableSpatialMetrics(&baseline.spatial,
			current.spatial, &spatialFrozen, &spatialAwaiting);
	}

	void checkAwaiting(Bool expected)
	{
		Check(directAwaiting == expected, "direct-path collector startup epoch");
		Check(ordinaryAwaiting == expected, "ordinary-path collector startup epoch");
		Check(collisionAwaiting == expected, "collision collector startup epoch");
		Check(spatialAwaiting == expected, "spatial collector startup epoch");
	}

	void checkActivity()
	{
		Check(directHasActivity && directFrozen.eligibleRequests == 5,
			"direct-path collector retains the same-epoch fixture sample");
		Check(ordinaryFrozen.eligibleRequests == 7,
			"ordinary-path collector retains the same-epoch fixture sample");
		Check(collisionFrozen.ineligibleSlices == 1,
			"collision collector retains the real diagnostic increment");
		Check(spatialFrozen.healing.expectedFallbacks == 1,
			"spatial collector retains the real diagnostic increment");
	}

	Samples baseline;
	DirectPathRuntimeMetrics directFrozen;
	OrdinaryPathRuntimeMetrics ordinaryFrozen;
	rts::CollisionCandidateRuntimeMetrics collisionFrozen;
	rts::ImmutableSpatialRuntimeMetrics spatialFrozen;
	Bool directAwaiting, ordinaryAwaiting, collisionAwaiting, spatialAwaiting;
	Bool directHasActivity;
};
} // namespace headless_metric_startup_fixture

int RunHeadlessMetricStartupTitleTests()
{
	using namespace headless_metric_startup_fixture;
	failures = 0;
	Check(!rts::JobSystem::instance().isRunning(),
		"startup metric fixture must not start or overlap workers");
	if (failures != 0)
		return 1;

	ResetAllMetricFamilies();
	rts::RecordCollisionCandidateIneligibleSlice();
	rts::RecordImmutableSpatialExpectedFallback(
		rts::IMMUTABLE_SPATIAL_CONSUMER_HEALING);
	Collectors collectors;
	const Samples before;
	globalData.m_headless = FALSE;
	s_headlessSimulationJobSystemStartAttempted = FALSE;
	startHeadlessSimulationJobsAfterUnsafeInitialization();
	const Samples interactive;
	CheckEpochs(before, interactive, 0, "non-headless startup leaves metric epochs unchanged");
	Check(!s_headlessSimulationJobSystemStartAttempted,
		"non-headless startup does not consume the one-shot boundary");
	Check(interactive.collision.ineligibleSlices == 1 &&
		interactive.spatial.healing.expectedFallbacks == 1,
		"non-headless startup preserves existing diagnostics");
	collectors.capture(interactive);
	collectors.checkAwaiting(TRUE);

	globalData.m_headless = TRUE;
	startHeadlessSimulationJobsAfterUnsafeInitialization();
	const Samples firstStartup;
	CheckEpochs(before, firstStartup, 1, "first headless startup advances each metric epoch once");
	Check(s_headlessSimulationJobSystemStartAttempted,
		"first headless startup consumes the one-shot boundary");
	Check(firstStartup.collision.ineligibleSlices == 0 &&
		firstStartup.spatial.healing.expectedFallbacks == 0,
		"first headless startup clears pre-match diagnostics");
	collectors.capture(firstStartup);
	collectors.checkAwaiting(FALSE);

	rts::RecordCollisionCandidateIneligibleSlice();
	rts::RecordImmutableSpatialExpectedFallback(
		rts::IMMUTABLE_SPATIAL_CONSUMER_HEALING);
	Samples active;
	// These path fields are deliberately controlled collector-input fixtures,
	// not live path jobs or gameplay-coverage evidence. Their reset epochs and
	// every reset/getter/collector operation above and below are production code.
	active.direct.eligibleRequests = 5;
	active.ordinary.eligibleRequests = 7;
	collectors.capture(active);
	collectors.checkActivity();

	startHeadlessSimulationJobsAfterUnsafeInitialization();
	const Samples repeatedStartup;
	CheckEpochs(firstStartup, repeatedStartup, 0,
		"repeated startup does not reset an active match epoch");
	Check(repeatedStartup.collision.ineligibleSlices == 1 &&
		repeatedStartup.spatial.healing.expectedFallbacks == 1,
		"repeated startup preserves accumulated diagnostics");
	collectors.capture(repeatedStartup);
	collectors.checkActivity();

	ResetAllMetricFamilies();
	const Samples teardown;
	CheckEpochs(firstStartup, teardown, 1, "teardown advances the real metric epochs");
	collectors.capture(teardown);
	collectors.checkAwaiting(FALSE);
	collectors.checkActivity();
	Check(!rts::JobSystem::instance().isRunning(),
		"startup metric fixture never starts workers");

	if (failures != 0)
		return 1;
	printf("Headless metric startup title tests passed.\n");
	return 0;
}
