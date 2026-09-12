#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#include <stdio.h>

namespace
{

struct LegacyHarness
{
	LegacyHarness() : adapter(0), commits(0), expectedPhase(1) {}
	rts::LiveSimulationPhaseGraphOwnerAdapter *adapter;
	unsigned commits;
	unsigned expectedPhase;
};

bool isOwner(void *context)
{
	return context != 0;
}

bool validate(rts::SimulationPhaseId phaseId, unsigned generation,
	unsigned frame, void *context)
{
	LegacyHarness *harness = static_cast<LegacyHarness *>(context);
	return harness != 0 && phaseId == harness->expectedPhase &&
		generation != 0 && frame <= 9;
}

bool commit(rts::SimulationPhaseId phaseId, unsigned generation,
	unsigned frame, void *context)
{
	LegacyHarness *harness = static_cast<LegacyHarness *>(context);
	if (harness == 0 || phaseId != harness->expectedPhase || generation == 0)
		return false;
	++harness->commits;
	++harness->expectedPhase;
	(void)frame;
	return true;
}

} // namespace

int main()
{
	const rts::JobMetricCounter maximumCounter =
		~static_cast<rts::JobMetricCounter>(0);
	const rts::JobMetricCounter maximumSignedCounter = maximumCounter >> 1;
	const rts::JobMetricCounter largeEvenFrequency =
		maximumSignedCounter - 1;
	if (rts::LiveSimulationPhaseTicksToNanoseconds(1, 0) != 0 ||
		rts::LiveSimulationPhaseTicksToNanoseconds(15, 10) != 1500000000u ||
		rts::LiveSimulationPhaseTicksToNanoseconds(
			largeEvenFrequency / 2, largeEvenFrequency) != 500000000u ||
		rts::LiveSimulationPhaseTicksToNanoseconds(
			maximumSignedCounter - 1, maximumSignedCounter) != 999999999u ||
		rts::LiveSimulationPhaseTicksToNanoseconds(maximumCounter, 1) !=
			maximumCounter)
	{
		return 1;
	}
	if (rts::ShouldUseLiveSimulationPhaseGraph(false, false, 0, 1) ||
		!rts::ShouldUseLiveSimulationPhaseGraph(true, false, 0, 1) ||
		rts::ShouldUseLiveSimulationPhaseGraph(true, true, 0, 1) ||
		!rts::ShouldUseLiveSimulationPhaseGraph(true, true, 1, 1) ||
		!rts::IsLiveSimulationPhaseReleaseWorkerCount(1) ||
		!rts::IsLiveSimulationPhaseReleaseWorkerCount(16) ||
		rts::IsLiveSimulationPhaseReleaseWorkerCount(3))
	{
		return 1;
	}
	rts::LiveSimulationPhaseOwnerCallbacks callbacks;
	callbacks.isOwner = &isOwner;
	callbacks.validate = &validate;
	callbacks.commit = &commit;
	LegacyHarness harness;
	rts::LiveSimulationPhaseGraphOwnerAdapter adapter(callbacks, &harness);
	harness.adapter = &adapter;
	if (adapter.runFrame(9) != rts::LIVE_SIMULATION_PHASE_COMPLETED ||
		harness.commits != 5 || adapter.generation() != 1)
	{
		return 1;
	}
	harness.expectedPhase = 1;
	if (adapter.runFrame(0) != rts::LIVE_SIMULATION_PHASE_COMPLETED ||
		harness.commits != 10 || adapter.generation() != 2)
	{
		return 1;
	}
	rts::LiveSimulationPhaseAuthorityEvidence evidence;
	if (!adapter.authorityEvidence(
		rts::LIVE_SIMULATION_PHASE_VERIFICATION_PUBLICATION, evidence) ||
		!evidence.validated || !evidence.committed ||
		evidence.executionKind != rts::SIMULATION_PHASE_EXECUTION_OWNER_HELP)
	{
		return 1;
	}
	if (!rts::HasStableLiveSimulationPhaseEvidence(adapter.runtimeMetrics()))
	{
		return 1;
	}
	adapter.resetRuntimeMetrics();
	rts::JobMetricCounter directNanoseconds[5] = { 1, 2, 3, 4, 5 };
	if (rts::LIVE_SIMULATION_PHASE_PERFORMANCE_SCHEMA_VERSION != 1 ||
		!adapter.recordDirectFramePerformance(directNanoseconds, 5, 20) ||
		adapter.runtimeMetrics().ownerPhaseTotalNanoseconds[1] != 2 ||
		adapter.runtimeMetrics().serialIslandTotalNanoseconds != 2 ||
		adapter.runtimeMetrics().frameSimulationTotalNanoseconds != 20)
	{
		return 1;
	}
	printf("SimulationPhaseGraph owner-adapter legacy fixture passed.\n");
	return 0;
}
