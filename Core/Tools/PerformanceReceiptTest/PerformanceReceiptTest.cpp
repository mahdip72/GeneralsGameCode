#include "Lib/PerformanceReceipt.h"

#include <stdio.h>

namespace
{
int check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

rts::performance::PerformanceReceipt makeCompleteReceipt()
{
	using namespace rts::performance;
	PerformanceReceipt receipt;
	receipt.status = "complete";
	receipt.title = "Generals";
	receipt.runId = "run-20260901-0001";
	receipt.sourceCommit =
		"0123456789abcdef0123456789abcdef01234567";
	receipt.artifactSetSha256 =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	receipt.executablePath = "H:\\installed\\generals.exe";
	receipt.executableSha256 =
		"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	receipt.commandLine = "generals.exe -headless -workerCount 8";
	receipt.processId = 4242;
	receipt.processCreationTimeUtc100ns = 100;
	receipt.processStartTimeUtc100ns = 200;
	receipt.processEndTimeUtc100ns = 300;
	receipt.processIdentityAvailable = true;
	receipt.processExitCode = 0;
	receipt.processExitCodeKnown = true;
	receipt.processExitBoundary =
		"ReplaySimulation::simulateReplaysInThisProcess:return";
	receipt.fixtureId = "dense-8-player";
	receipt.fixtureContentSha256 =
		"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
	receipt.replayPath = "Stage5Scaling\\dense-8-player.rep";
	receipt.seed = 49374;
	receipt.seedKnown = true;
	receipt.playerCount = 8;
	receipt.playerCountKnown = true;
	receipt.unitCount = 8000;
	receipt.unitCountKnown = true;
	receipt.frameStart = 0;
	receipt.frameEnd = 4096;
	receipt.finalFrame = 4096;
	receipt.finalCrcKnown = true;
	receipt.finalCrc = 0x1234abcd;
	receipt.requestedWorkerCount = 8;
	receipt.effectiveWorkerCount = 8;
	receipt.workerPolicy = "auto";
	receipt.workersPinned = true;
	receipt.availableLogicalCpuCount = 16;
	receipt.reservedOwnerCpuCount = 1;
	receipt.selectedWorkerCpuCount = 8;
	receipt.selectedWorkerPhysicalCoreCount = 8;
	receipt.selectedWorkerPhysicalCoreMask = 0xff;
	receipt.selectedWorkerPhysicalCoreMaskComplete = true;

	PerformanceReceiptCpuSet owner;
	owner.id = 10;
	owner.coreIndex = 0;
	owner.logicalProcessorIndex = 0;
	owner.availableToProcess = true;
	receipt.cpuSets.push_back(owner);
	for (unsigned index = 0; index < 8; ++index)
	{
		PerformanceReceiptCpuSet worker;
		worker.id = 100 + index;
		worker.coreIndex = 1 + index;
		worker.logicalProcessorIndex = 1 + index;
		worker.availableToProcess = true;
		receipt.cpuSets.push_back(worker);
		receipt.selectedWorkerCpuSetIds.push_back(worker.id);
	}
	receipt.ownerCpuSetIds.push_back(owner.id);

	const char *phaseNames[] =
	{
		"owner-intake", "world-queries", "pathfinding", "object-computation",
		"spatial-work", "deterministic-commit", "verification-publication"
	};
	for (unsigned index = 0; index < sizeof(phaseNames) / sizeof(phaseNames[0]);
		++index)
	{
		PerformanceReceiptPhase phase;
		phase.name = phaseNames[index];
		phase.available = true;
		phase.totalNanoseconds = 1000 + index;
		phase.maximumNanoseconds = phase.totalNanoseconds;
		phase.sampleCount = 1;
		receipt.phases.push_back(phase);
	}
	const char *kernelNames[] =
	{
		"physics", "status", "collision", "ai-planning", "spatial",
		"path"
	};
	for (unsigned index = 0; index < sizeof(kernelNames) / sizeof(kernelNames[0]);
		++index)
	{
		PerformanceReceiptKernel kernel;
		kernel.name = kernelNames[index];
		kernel.available = true;
		kernel.submittedJobs = 8;
		kernel.completedJobs = kernel.submittedJobs;
		kernel.physicalWorkerJobs = 8;
		kernel.physicalWorkerMask = 0xff;
		kernel.distinctPhysicalWorkers = 8;
		kernel.physicalWorkerMaskComplete = true;
		kernel.elapsedNanoseconds = 2000 + index;
		kernel.elapsedNanosecondsKnown = true;
		receipt.kernels.push_back(kernel);
	}
	receipt.rawEvidence.verifierBoundary =
		"game-receipt-before-host-log-close";
	receipt.rawEvidence.rawLogPath = "H:\\evidence\\run.log";
	receipt.rawEvidence.rawLogSha256 =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	receipt.rawEvidence.timingPath = "H:\\evidence\\timing.csv";
	receipt.rawEvidence.timingSha256 =
		"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
	return receipt;
}

int testSerializationAndEscaping()
{
	rts::performance::PerformanceReceipt receipt = makeCompleteReceipt();
	receipt.commandLine = "game.exe -fixture \"dense\\8\"";
	std::string document;
	int result = 0;
	result |= check(rts::performance::SerializePerformanceReceipt(receipt,
		document), "complete receipt serializes");
	result |= check(document.find("game-executable-performance-receipt-v1") !=
		std::string::npos, "serialized producer is explicit");
	result |= check(document.find("\\\"dense\\\\8\\\"") !=
		std::string::npos, "JSON string escaping is deterministic");
	result |= check(document.find("GetSystemCpuSetInformation") !=
		std::string::npos, "serialized topology source is explicit");
	return result;
}

int testStrictValidation()
{
	using namespace rts::performance;
	int result = 0;
	PerformanceReceipt receipt = makeCompleteReceipt();
	std::string reason;
	result |= check(ValidatePerformanceReceipt(receipt, &reason),
		"complete executable receipt validates");
	receipt.processIdentityAvailable = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason) &&
		reason.find("process provenance") != std::string::npos,
		"missing process provenance fails closed");
	receipt = makeCompleteReceipt();
	receipt.rawEvidence.verifierBoundary.clear();
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"missing verifier boundary fails closed");
	receipt = makeCompleteReceipt();
	receipt.selectedWorkerCpuSetIds[0] = 9999;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"selected CPU-set substitution fails closed");
	receipt = makeCompleteReceipt();
	receipt.status = "pending";
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"incomplete result fails closed");
	receipt = makeCompleteReceipt();
	receipt.workersPinned = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"unpinned worker policy fails closed");
	receipt = makeCompleteReceipt();
	receipt.processExitCodeKnown = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"unknown process exit fails closed");
	receipt = makeCompleteReceipt();
	receipt.phases[0].name = "world-queries";
	result |= check(!ValidatePerformanceReceipt(receipt, &reason) &&
		reason.find("exact canonical set") != std::string::npos,
		"phase name/order substitution fails closed");
	receipt = makeCompleteReceipt();
	receipt.kernels[5].name = "pathfinding";
	result |= check(!ValidatePerformanceReceipt(receipt, &reason) &&
		reason.find("exact canonical set") != std::string::npos,
		"legacy pathfinding kernel name fails closed");
	receipt = makeCompleteReceipt();
	receipt.phases[1].available = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason) &&
		reason.find("unavailable phase") != std::string::npos,
		"unavailable phase cannot carry timing data");
	receipt = makeCompleteReceipt();
	receipt.kernels[0].elapsedNanosecondsKnown = true;
	receipt.kernels[0].elapsedNanoseconds = 0;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason) &&
		reason.find("known kernel timing") != std::string::npos,
		"zero known kernel timing fails closed");
	return result;
}

int testMissingEnvironmentFailsClosed()
{
	std::string reason;
	rts::performance::PerformanceReceipt receipt;
	return check(!rts::performance::BeginPerformanceReceipt(receipt,
		"Generals", "fixture.rep", 0, &reason) &&
		reason.find("environment") != std::string::npos,
		"missing host run contract fails before receipt capture");
}
}

int main()
{
	int result = 0;
	result |= testSerializationAndEscaping();
	result |= testStrictValidation();
	result |= testMissingEnvironmentFailsClosed();
	return result;
}
