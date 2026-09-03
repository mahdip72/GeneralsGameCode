/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// This fixture includes the production CaptureSkirmishAITestRuntimeState
// function through a CMake-generated extraction.  Only the external runtime
// collaborators are controlled here; the phase snapshot boundary itself is
// the production code under test.
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#include <stdio.h>

namespace stage5_phase_snapshot_fixture
{
struct RunnerState
{
	bool ending;
	unsigned effectiveWorkerCount;
};

struct GameLogic
{
	rts::LiveSimulationPhaseRuntimeMetrics phaseMetrics;

	const rts::LiveSimulationPhaseRuntimeMetrics &
		getStage5PhaseRuntimeMetrics() const
	{
		return phaseMetrics;
	}
};

unsigned workerCount = 0;
unsigned sliceCaptureCount = 0;
RunnerState s_runner = { false, 0 };
GameLogic *TheGameLogic = 0;
rts::LiveSimulationPhaseRuntimeMetrics s_phaseMetricsLast;

void CaptureSkirmishAITestSliceMetrics()
{
	++sliceCaptureCount;
}
} // namespace stage5_phase_snapshot_fixture

namespace rts
{
class Stage5PhaseSnapshotJobSystem
{
public:
	static Stage5PhaseSnapshotJobSystem &instance()
	{
		static Stage5PhaseSnapshotJobSystem value;
		return value;
	}

	unsigned workerCount() const
	{
		return stage5_phase_snapshot_fixture::workerCount;
	}
};
} // namespace rts

namespace
{
using stage5_phase_snapshot_fixture::CaptureSkirmishAITestSliceMetrics;
using stage5_phase_snapshot_fixture::GameLogic;
using stage5_phase_snapshot_fixture::TheGameLogic;
using stage5_phase_snapshot_fixture::s_phaseMetricsLast;
using stage5_phase_snapshot_fixture::s_runner;
using stage5_phase_snapshot_fixture::sliceCaptureCount;
using stage5_phase_snapshot_fixture::workerCount;

#define JobSystem Stage5PhaseSnapshotJobSystem
#include "SkirmishPhaseCaptureUnderTest.inc"
#undef JobSystem

unsigned failures = 0;

void Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

rts::LiveSimulationPhaseRuntimeMetrics MakeMetrics(unsigned seed)
{
	rts::LiveSimulationPhaseRuntimeMetrics value;
	value.attemptedFrames = seed + 1;
	value.completedFrames = seed + 2;
	value.stableSequenceFrames = seed + 3;
	value.stoppedByOwnerFrames = seed + 4;
	value.fallbackBeforeMutationFrames = seed + 5;
	value.failedAfterMutationFrames = seed + 6;
	value.committedPhases = seed + 7;
	value.sequenceViolationFrames = seed + 8;
	value.lastFrame = seed + 9;
	value.lastGeneration = seed + 10;
	value.lastCommittedPhaseCount = seed + 11;
	value.lastSequenceSignature = seed + 12;
	for (unsigned phase = 0; phase < rts::LIVE_SIMULATION_PHASE_COUNT - 1; ++phase)
	{
		value.ownerPhaseTotalNanoseconds[phase] = seed + 100 + phase;
		value.ownerPhaseMaximumNanoseconds[phase] = seed + 110 + phase;
		value.ownerPhaseSampleCount[phase] = seed + 120 + phase;
	}
	value.frameSimulationTotalNanoseconds = seed + 200;
	value.frameSimulationMaximumNanoseconds = seed + 201;
	value.frameSimulationSampleCount = seed + 202;
	value.serialIslandTotalNanoseconds = seed + 203;
	value.serialIslandMaximumNanoseconds = seed + 204;
	value.serialIslandSampleCount = seed + 205;
	return value;
}

bool SameMetrics(const rts::LiveSimulationPhaseRuntimeMetrics &left,
	const rts::LiveSimulationPhaseRuntimeMetrics &right)
{
	if (left.attemptedFrames != right.attemptedFrames ||
		left.completedFrames != right.completedFrames ||
		left.stableSequenceFrames != right.stableSequenceFrames ||
		left.stoppedByOwnerFrames != right.stoppedByOwnerFrames ||
		left.fallbackBeforeMutationFrames != right.fallbackBeforeMutationFrames ||
		left.failedAfterMutationFrames != right.failedAfterMutationFrames ||
		left.committedPhases != right.committedPhases ||
		left.sequenceViolationFrames != right.sequenceViolationFrames ||
		left.lastFrame != right.lastFrame ||
		left.lastGeneration != right.lastGeneration ||
		left.lastCommittedPhaseCount != right.lastCommittedPhaseCount ||
		left.lastSequenceSignature != right.lastSequenceSignature ||
		left.frameSimulationTotalNanoseconds !=
			right.frameSimulationTotalNanoseconds ||
		left.frameSimulationMaximumNanoseconds !=
			right.frameSimulationMaximumNanoseconds ||
		left.frameSimulationSampleCount != right.frameSimulationSampleCount ||
		left.serialIslandTotalNanoseconds != right.serialIslandTotalNanoseconds ||
		left.serialIslandMaximumNanoseconds != right.serialIslandMaximumNanoseconds ||
		left.serialIslandSampleCount != right.serialIslandSampleCount)
		return false;
	for (unsigned phase = 0; phase < rts::LIVE_SIMULATION_PHASE_COUNT - 1; ++phase)
	{
		if (left.ownerPhaseTotalNanoseconds[phase] !=
			right.ownerPhaseTotalNanoseconds[phase] ||
			left.ownerPhaseMaximumNanoseconds[phase] !=
				right.ownerPhaseMaximumNanoseconds[phase] ||
			left.ownerPhaseSampleCount[phase] !=
				right.ownerPhaseSampleCount[phase])
			return false;
	}
	return true;
}

void ResetFixture()
{
	workerCount = 0;
	sliceCaptureCount = 0;
	s_runner.ending = false;
	s_runner.effectiveWorkerCount = 0;
	TheGameLogic = 0;
	s_phaseMetricsLast = MakeMetrics(1);
}

void TestActiveCapturePublishesPhaseSnapshot()
{
	ResetFixture();
	GameLogic logic;
	const rts::LiveSimulationPhaseRuntimeMetrics expected = MakeMetrics(200);
	logic.phaseMetrics = expected;
	TheGameLogic = &logic;
	workerCount = 4;
	s_runner.effectiveWorkerCount = 1;

	CaptureSkirmishAITestRuntimeState();

	Check(SameMetrics(s_phaseMetricsLast, expected),
		"active runner captures the complete phase snapshot");
	Check(s_runner.effectiveWorkerCount == 4,
		"active runner records the effective worker maximum");
	Check(sliceCaptureCount == 1,
		"active runner continues slice capture");
}

void TestEndingCapturePreservesTerminalPhaseSnapshot()
{
	ResetFixture();
	const rts::LiveSimulationPhaseRuntimeMetrics terminal = MakeMetrics(300);
	const rts::LiveSimulationPhaseRuntimeMetrics reset = MakeMetrics(400);
	GameLogic logic;
	logic.phaseMetrics = reset;
	TheGameLogic = &logic;
	s_phaseMetricsLast = terminal;
	s_runner.ending = true;
	s_runner.effectiveWorkerCount = 2;
	workerCount = 8;

	CaptureSkirmishAITestRuntimeState();

	Check(SameMetrics(s_phaseMetricsLast, terminal),
		"ending runner preserves every terminal phase metric after reset");
	Check(s_runner.effectiveWorkerCount == 8,
		"ending runner still records the effective worker maximum");
	Check(sliceCaptureCount == 1,
		"ending runner still captures slice metrics");
}

void TestNullGameLogicStillCapturesIndependentState()
{
	ResetFixture();
	const rts::LiveSimulationPhaseRuntimeMetrics before = s_phaseMetricsLast;
	s_runner.ending = true;
	workerCount = 16;

	CaptureSkirmishAITestRuntimeState();

	Check(SameMetrics(s_phaseMetricsLast, before),
		"missing game logic leaves the phase snapshot unchanged");
	Check(s_runner.effectiveWorkerCount == 16,
		"missing game logic does not suppress worker capture");
	Check(sliceCaptureCount == 1,
		"missing game logic does not suppress slice capture");
}
} // namespace

int main()
{
	TestActiveCapturePublishesPhaseSnapshot();
	TestEndingCapturePreservesTerminalPhaseSnapshot();
	TestNullGameLogicStillCapturesIndependentState();
	if (failures != 0)
		return 1;
	printf("Skirmish phase snapshot tests passed.\n");
	return 0;
}
