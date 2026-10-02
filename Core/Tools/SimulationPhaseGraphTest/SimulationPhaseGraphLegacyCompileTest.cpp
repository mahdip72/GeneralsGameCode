/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Lib/SimulationPhaseGraph.h"

#include <stdio.h>

namespace
{

int g_committedValue = 0;

bool IsOwner(void *)
{
	return true;
}

rts::SimulationPhaseWorkStatus Execute(
	const rts::SimulationPhaseJobContext &context,
	const void *input, unsigned inputBytes, void *output,
	unsigned outputBytes)
{
	if (context.isCancellationRequested() || inputBytes != sizeof(int) ||
		outputBytes != sizeof(int))
	{
		return rts::SIMULATION_PHASE_WORK_FAILED;
	}
	*static_cast<int *>(output) = *static_cast<const int *>(input) +
		static_cast<int>(context.jobKey());
	return rts::SIMULATION_PHASE_WORK_SUCCEEDED;
}

rts::SimulationPhaseWorkStatus Validate(
	const rts::SimulationPhaseCommitContext &context,
	const void *input, unsigned inputBytes, const void *output,
	unsigned outputBytes, void *)
{
	if (inputBytes != sizeof(int) || outputBytes != sizeof(int))
		return rts::SIMULATION_PHASE_WORK_FAILED;
	const int expected = *static_cast<const int *>(input) +
		static_cast<int>(context.jobKey());
	return *static_cast<const int *>(output) == expected ?
		rts::SIMULATION_PHASE_WORK_SUCCEEDED :
		rts::SIMULATION_PHASE_WORK_FAILED;
}

void Commit(const rts::SimulationPhaseCommitContext &,
	const void *, unsigned, const void *output, unsigned, void *)
{
	g_committedValue = *static_cast<const int *>(output);
}

} // namespace

int main()
{
	rts::SimulationPhaseNodeStorage nodes[1];
	rts::SimulationPhaseJobStorage jobs[1];
	rts::SimulationPhaseJobResultStorage results[1];
	rts::SimulationPhaseGraph graph(nodes, 1, jobs, 1, results, 1,
		IsOwner, 0);
	int input = 7;
	int output = 0;
	rts::SimulationPhaseDefinition phase;
	phase.id = 3;
	phase.dependencies = 0;
	phase.dependencyCount = 0;
	phase.immutableInput = &input;
	phase.immutableInputBytes = sizeof(input);
	rts::SimulationPhaseJobDefinition job;
	job.phaseId = 3;
	job.key = 2;
	job.privateOutput = &output;
	job.privateOutputBytes = sizeof(output);
	job.execute = Execute;
	job.validate = Validate;
	job.commit = Commit;
	const rts::SimulationPhaseGraphConfigurationStatus configurationStatus =
		graph.configure(&phase, 1, &job, 1);
	if (configurationStatus !=
		rts::SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID)
	{
		printf("Simulation phase graph legacy configure failed: %d\n",
			static_cast<int>(configurationStatus));
		return 1;
	}
	if (!graph.reset(1))
	{
		printf("Simulation phase graph legacy reset failed\n");
		return 1;
	}
	rts::SimulationPhaseJobTicket ticket;
	if (!graph.tryClaimReadyJob(ticket) ||
		graph.executeClaimedJob(ticket,
			rts::SimulationPhaseExecutionIdentity::ownerHelp()) !=
			rts::SIMULATION_PHASE_WORK_SUCCEEDED ||
		!graph.advanceOwner() ||
		graph.state() != rts::SIMULATION_PHASE_GRAPH_COMPLETED ||
		g_committedValue != 9)
	{
		return 2;
	}
	printf("Simulation phase graph legacy contract passed\n");
	return 0;
}
