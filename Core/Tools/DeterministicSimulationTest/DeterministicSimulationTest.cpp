#include "Lib/SimulationCommandBuffer.h"
#include "Lib/SimulationExecutionPolicy.h"
#if defined(RTS_BUILD_CORE_EXTRAS)
#include "Lib/JobSystem.h"
#endif

#include <stdio.h>
#include <string.h>

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
#endif

namespace
{
#if defined(RTS_BUILD_CORE_EXTRAS)
enum JobSystemTestFault
{
	JOB_SYSTEM_TEST_FAIL_START = 1
};

bool ensureSimulationJobsStarted(void *context)
{
	return static_cast<rts::JobSystem *>(context)->ensureStarted();
}
#endif

int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

int TestExecutionPolicy()
{
	int result = 0;
	result |= Check(rts::GetSimulationExecutionMode() ==
		rts::SIMULATION_EXECUTION_SERIAL &&
		!rts::PrepareSimulationCommandsOffThread(),
		"simulation execution defaults to the serial oracle");
	result |= Check(rts::SetSimulationExecutionMode("PaRaLlEl") &&
		rts::UseParallelSimulation() &&
		rts::PrepareSimulationCommandsOffThread(),
		"parallel mode prepares and commits commands off-thread");
	result |= Check(rts::SetSimulationExecutionMode("shadow") &&
		!rts::UseParallelSimulation() && rts::UseSimulationShadowOracle() &&
		rts::PrepareSimulationCommandsOffThread(),
		"shadow mode prepares commands without authoritative parallel commit");
	result |= Check(!rts::SetSimulationExecutionMode("parallelism") &&
		!rts::SetSimulationExecutionMode(
			static_cast<rts::SimulationExecutionMode>(99)) &&
		rts::UseSimulationShadowOracle(),
		"invalid execution modes do not mutate the selected policy");
	rts::LockSimulationExecutionMode();
	result |= Check(rts::IsSimulationExecutionModeLocked() &&
		!rts::SetSimulationExecutionMode("serial") &&
		rts::UseSimulationShadowOracle(),
		"locked simulation policy is process immutable");
	return result;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
int TestStartupFailure(const char *requestedMode)
{
	int result = 0;
	rts::JobSystem &jobSystem = rts::JobSystem::instance();

	result |= Check(rts::SetSimulationExecutionMode(requestedMode),
		"startup failure fixture selects its requested simulation mode");
	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_START, 1);
	const rts::SimulationExecutionStartupResult startupResult =
		rts::PrepareSimulationExecutionStartup(
			ensureSimulationJobsStarted, &jobSystem);
	result |= Check(startupResult ==
		rts::SIMULATION_EXECUTION_STARTUP_SERIAL_FALLBACK &&
		!jobSystem.isRunning() && jobSystem.workerCount() == 0 &&
		rts::IsSimulationExecutionModeLocked() &&
		rts::GetSimulationExecutionMode() ==
			rts::SIMULATION_EXECUTION_SERIAL,
		"parallel or shadow startup failure downgrades to verified serial before locking");
	return result;
}
#endif

int TestCanonicalOrderAndCompletionIndependence()
{
	rts::SimulationCommand commands0[2];
	rts::SimulationCommand commands1[1];
	rts::SimulationCommand commands2[1];
	unsigned char payload0[2];
	unsigned char payload1[1];
	unsigned char payload2[1];
	rts::SimulationCommandBuffer producer0(commands0, 2, payload0, 2, 2, 2);
	rts::SimulationCommandBuffer producer1(commands1, 1, payload1, 1, 0, 1);
	rts::SimulationCommandBuffer producer2(commands2, 1, payload2, 1, 1, 1);
	const rts::SimulationCommandBuffer *slots[3] = {
		&producer0, &producer1, &producer2
	};
	rts::SimulationMergedCommand output[4];
	rts::SimulationMergedCommand scratch[4];
	UnsignedInt firstTargets[4];
	UnsignedInt firstPhases[4];
	unsigned char values[4] = { 30, 40, 20, 10 };
	unsigned index;
	int result = 0;

	result |= Check(producer0.append(1, rts::SimulationStableHandle(30),
		rts::SimulationStableHandle(2), 1, values + 0, 1) &&
		producer0.append(0, rts::SimulationStableHandle(40),
		rts::SimulationStableHandle(2), 1, values + 1, 1),
		"producer zero emits into its private fixed storage");
	result |= Check(producer1.append(0, rts::SimulationStableHandle(20),
		rts::SimulationStableHandle(1), 1, values + 2, 1) &&
		producer2.append(0, rts::SimulationStableHandle(10),
		rts::SimulationStableHandle(1), 1, values + 3, 1),
		"other producers emit into disjoint fixed slots");
	result |= Check(producer2.complete() && producer0.complete() &&
		producer1.complete(),
		"producer completion order may differ from fixed slot order");
	rts::SimulationCommandMergeResult merge = rts::MergeSimulationCommandSlots(
		slots, 3, output, scratch, 4);
	result |= Check(merge.succeeded() && merge.commandCount == 4,
		"completed producer slots merge successfully");
	for (index = 0; index < merge.commandCount; ++index)
	{
		firstTargets[index] = output[index].command()->orderKey().target().objectID();
		firstPhases[index] = output[index].command()->orderKey().phase();
		result |= Check(output[index].payload() != 0,
			"merged payload remains in producer-owned bounded storage");
	}
	result |= Check(firstPhases[0] == 0 && firstTargets[0] == 10 &&
		firstPhases[1] == 0 && firstTargets[1] == 20 &&
		firstPhases[2] == 0 && firstTargets[2] == 40 &&
		firstPhases[3] == 1 && firstTargets[3] == 30,
		"merge follows phase then target ObjectID before producer completion");

	producer0.reset();
	producer1.reset();
	producer2.reset();
	producer0.append(1, rts::SimulationStableHandle(30),
		rts::SimulationStableHandle(2), 1, values + 0, 1);
	producer0.append(0, rts::SimulationStableHandle(40),
		rts::SimulationStableHandle(2), 1, values + 1, 1);
	producer1.append(0, rts::SimulationStableHandle(20),
		rts::SimulationStableHandle(1), 1, values + 2, 1);
	producer2.append(0, rts::SimulationStableHandle(10),
		rts::SimulationStableHandle(1), 1, values + 3, 1);
	producer1.complete();
	producer2.complete();
	producer0.complete();
	merge = rts::MergeSimulationCommandSlots(slots, 3, output, scratch, 4);
	result |= Check(merge.succeeded() && merge.commandCount == 4,
		"a second completion order also merges successfully");
	for (index = 0; index < merge.commandCount; ++index)
	{
		result |= Check(firstTargets[index] ==
			output[index].command()->orderKey().target().objectID() &&
			firstPhases[index] == output[index].command()->orderKey().phase(),
			"completion order cannot change canonical command order");
	}
	return result;
}

int TestOverflowAndFaultValidation()
{
	rts::SimulationCommand commands[1];
	unsigned char guardedPayload[3] = { 0xA5, 0xA5, 0xA5 };
	unsigned char bytes[2] = { 1, 2 };
	rts::SimulationCommandBuffer commandOverflow(commands, 1,
		guardedPayload + 1, 1, 0, 1);
	int result = 0;

	result |= Check(commandOverflow.append(0,
		rts::SimulationStableHandle(1), rts::SimulationStableHandle(),
		1, bytes, 1), "bounded command fixture accepts its first command");
	result |= Check(!commandOverflow.append(0,
		rts::SimulationStableHandle(2), rts::SimulationStableHandle(),
		1, 0, 0) && commandOverflow.status() ==
		rts::SIMULATION_COMMAND_BUFFER_COMMAND_OVERFLOW &&
		commandOverflow.commandCount() == 1 &&
		guardedPayload[0] == 0xA5 && guardedPayload[2] == 0xA5,
		"command overflow is explicit and cannot cross storage guards");
	result |= Check(rts::ValidateSimulationCommandBuffer(commandOverflow, 0) ==
		rts::SIMULATION_COMMANDS_COMMAND_OVERFLOW,
		"command overflow fails result validation");

	rts::SimulationCommand payloadCommands[1];
	rts::SimulationCommandBuffer payloadOverflow(payloadCommands, 1,
		guardedPayload + 1, 1, 1, 1);
	result |= Check(!payloadOverflow.append(0,
		rts::SimulationStableHandle(1), rts::SimulationStableHandle(),
		1, bytes, 2) && payloadOverflow.status() ==
		rts::SIMULATION_COMMAND_BUFFER_PAYLOAD_OVERFLOW &&
		payloadOverflow.commandCount() == 0 &&
		guardedPayload[0] == 0xA5 && guardedPayload[2] == 0xA5,
		"payload overflow fails before any bounded output is published");

	rts::SimulationCommand faultCommands[1];
	rts::SimulationCommandBuffer fault(faultCommands, 1, 0, 0, 2, 1);
	const rts::SimulationCommandBuffer *slots[1] = { &fault };
	rts::SimulationMergedCommand output[1];
	fault.fail();
	rts::SimulationCommandMergeResult merge = rts::MergeSimulationCommandSlots(
		slots, 1, output, 0, 1);
	result |= Check(!merge.succeeded() && merge.status ==
		rts::SIMULATION_COMMANDS_PRODUCER_FAULT &&
		merge.commandCount == 0 && merge.producerSlot == 0,
		"producer fault rejects the whole wave with no consumable commands");

	rts::SimulationCommand incompleteCommands[1];
	rts::SimulationCommandBuffer incomplete(incompleteCommands, 1, 0, 0, 3, 1);
	slots[0] = &incomplete;
	merge = rts::MergeSimulationCommandSlots(slots, 1, output, 0, 1);
	result |= Check(merge.status == rts::SIMULATION_COMMANDS_PRODUCER_INCOMPLETE &&
		merge.commandCount == 0,
		"unpublished producer result rejects owner commit");
	return result;
}

int TestDuplicateKeyAndOutputCapacity()
{
	rts::SimulationCommand commands0[1];
	rts::SimulationCommand commands1[1];
	rts::SimulationCommandBuffer producer0(commands0, 1, 0, 0, 7, 3);
	rts::SimulationCommandBuffer producer1(commands1, 1, 0, 0, 7, 3);
	const rts::SimulationCommandBuffer *slots[2] = { &producer0, &producer1 };
	rts::SimulationMergedCommand output[2];
	rts::SimulationMergedCommand scratch[2];
	int result = 0;

	producer0.append(2, rts::SimulationStableHandle(9),
		rts::SimulationStableHandle(4), 1, 0, 0);
	producer1.append(2, rts::SimulationStableHandle(9),
		rts::SimulationStableHandle(4), 2, 0, 0);
	producer1.complete();
	producer0.complete();
	rts::SimulationCommandMergeResult merge = rts::MergeSimulationCommandSlots(
		slots, 2, output, scratch, 2);
	result |= Check(merge.status == rts::SIMULATION_COMMANDS_DUPLICATE_KEY &&
		merge.commandCount == 0 && merge.producerSlot == 1,
		"duplicate full canonical keys reject the entire wave");

	producer1.reset();
	producer1.append(2, rts::SimulationStableHandle(10),
		rts::SimulationStableHandle(4), 2, 0, 0);
	producer1.complete();
	merge = rts::MergeSimulationCommandSlots(slots, 2, output, scratch, 1);
	result |= Check(merge.status == rts::SIMULATION_COMMANDS_OUTPUT_OVERFLOW &&
		merge.commandCount == 0,
		"insufficient owner merge capacity fails before output publication");
	return result;
}

int TestOrderKeyTieBreakers()
{
	const rts::SimulationCommandOrderKey base(1,
		rts::SimulationStableHandle(2), rts::SimulationStableHandle(3),
		4, rts::MakeSimulationProducerSequence(5, 6));
	const rts::SimulationCommandOrderKey laterSource(1,
		rts::SimulationStableHandle(2), rts::SimulationStableHandle(4),
		1, rts::MakeSimulationProducerSequence(0, 0));
	const rts::SimulationCommandOrderKey laterModule(1,
		rts::SimulationStableHandle(2), rts::SimulationStableHandle(3),
		5, rts::MakeSimulationProducerSequence(0, 0));
	const rts::SimulationCommandOrderKey laterSequence(1,
		rts::SimulationStableHandle(2), rts::SimulationStableHandle(3),
		4, rts::MakeSimulationProducerSequence(5, 7));
	int result = 0;
	result |= Check(rts::CompareSimulationCommandOrderKeys(base,
		laterSource) < 0, "source ObjectID is the third canonical key field");
	result |= Check(rts::CompareSimulationCommandOrderKeys(base,
		laterModule) < 0, "stable module type precedes producer sequence");
	result |= Check(rts::CompareSimulationCommandOrderKeys(base,
		laterSequence) < 0 &&
		rts::SimulationProducerOrdinal(base.producerSequence()) == 5 &&
		rts::SimulationProducerLocalIndex(base.producerSequence()) == 6,
		"producer sequence is the final canonical tie breaker");
	return result;
}
}

int main(int argc, char *argv[])
{
#if defined(RTS_BUILD_CORE_EXTRAS)
	if (argc == 3 && strcmp(argv[1], "startup-failure") == 0 &&
		(strcmp(argv[2], "parallel") == 0 || strcmp(argv[2], "shadow") == 0))
		return TestStartupFailure(argv[2]);
#endif
	if (argc != 1)
	{
		fprintf(stderr, "Expected no arguments or startup-failure <parallel|shadow>.\n");
		return 1;
	}
	int result = 0;
	result |= TestCanonicalOrderAndCompletionIndependence();
	result |= TestOverflowAndFaultValidation();
	result |= TestDuplicateKeyAndOutputCapacity();
	result |= TestOrderKeyTieBreakers();
	result |= TestExecutionPolicy();
	if (result == 0) printf("Deterministic simulation foundation tests passed.\n");
	return result;
}
