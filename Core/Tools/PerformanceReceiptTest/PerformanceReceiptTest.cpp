#include "Lib/PerformanceReceipt.h"

#include <stdio.h>
#include <windows.h>

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

class ReceiptTestEnvironment
{
public:
	bool set(const char *name, const char *value)
	{
		Saved saved; saved.name = name;
		const DWORD length = GetEnvironmentVariableA(name, 0, 0);
		saved.present = length != 0;
		if (saved.present)
		{
			std::vector<char> buffer(length);
			GetEnvironmentVariableA(name, &buffer[0], length);
			saved.value = &buffer[0];
		}
		m_saved.push_back(saved);
		return SetEnvironmentVariableA(name, value) != 0;
	}
	~ReceiptTestEnvironment()
	{
		for (std::size_t index = m_saved.size(); index != 0; --index)
		{
			const Saved &saved = m_saved[index - 1];
			SetEnvironmentVariableA(saved.name.c_str(), saved.present ? saved.value.c_str() : 0);
		}
	}
private:
	struct Saved { std::string name, value; bool present; };
	std::vector<Saved> m_saved;
};

rts::performance::PerformanceReceipt makeCompleteReceipt()
{
	using namespace rts::performance;
	PerformanceReceipt receipt;
	receipt.status = "passed";
	receipt.title = "Generals";
	receipt.runId = "run-20260901-0001";
	receipt.runNonce = "11111111-1111-4111-8111-111111111111";
	receipt.cohortNonce = "22222222-2222-4222-8222-222222222222";
	receipt.cohortCreatedUtc = "2026-09-01T00:00:00.000Z";
	receipt.recordedUtc = "2026-09-01T00:01:00.000Z";
	receipt.receiptPath = "H:\\evidence\\performance-receipt-run-20260901-0001-4242.json";
	receipt.role = "performance-report";
	receipt.producerVersion = "5";
	// No admitted instrumentation is useful local diagnostics, not an exact
	// kernel or serial-reference qualification claim.
	receipt.kernelTiming.enabled = true;
	receipt.kernelTiming.frozen = true;
	receipt.kernelTiming.generation = 1;
	receipt.kernelReference.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
	receipt.kernelReference.frozen = true;
	receipt.kernelReference.generation = 1;
	receipt.architecture = "x64";
	receipt.sourceCommit =
		"0123456789abcdef0123456789abcdef01234567";
	receipt.artifactSetSha256 =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	receipt.runtimeClosureDependencyManifestSha256 =
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	receipt.runtimeClosureSha256 =
		"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
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
	receipt.fixtureContentPath = receipt.replayPath;
	receipt.fixtureIdentityObserved = true;
	receipt.seed = 49374;
	receipt.seedKnown = true;
	receipt.requestedPlayerCount = 8;
	receipt.requestedMinimumUnitCount = 8000;
	receipt.workload.sampleCount = 4096;
	receipt.workload.firstFrame = 1;
	receipt.workload.lastFrame = 4096;
	receipt.workload.playerCount = 8;
	receipt.workload.initialUnitCount = 8000;
	receipt.workload.minimumUnitCount = 7900;
	receipt.workload.peakUnitCount = 8200;
	receipt.frameSimulationTotalNanoseconds = 6000;
	receipt.frameSimulationMaximumNanoseconds = 1000;
	receipt.frameSimulationSampleCount = 4096;
	receipt.frameStart = 0;
	receipt.frameEnd = 4096;
	receipt.finalFrame = 4096;
	receipt.finalCrcKnown = true;
	receipt.finalCrc = 0x1234abcd;
	receipt.requestedWorkerCount = 8;
	receipt.simulationMode = "parallel";
	receipt.schedulerStarted = true;
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
		"owner-intake", "legacy-mutable-island", "spatial-work", "owner-tail",
		"verification-publication"
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
	receipt.rawEvidence.timingClosed = true;
	receipt.rawEvidence.timingWriteSucceeded = true;
	receipt.rawEvidence.timingComplete = true;
	receipt.rawEvidence.timingSessionCount = 1;
	receipt.rawEvidence.timingFrameSamples = 4097;
	receipt.rawEvidence.timingFirstFrame = 0;
	receipt.rawEvidence.timingLastFrame = 4096;
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
	result |= check(document.find("game-executable-stage5-performance-report-v5") !=
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
	receipt = makeCompleteReceipt();
	receipt.rawEvidence.timingClosed = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"flushed but unclosed timing fails closed");
	receipt = makeCompleteReceipt();
	receipt.rawEvidence.timingWriteSucceeded = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"failed timing output fails closed");
	receipt = makeCompleteReceipt();
	receipt.rawEvidence.timingTruncated = true;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"truncated timing cannot qualify even with a file hash");
	receipt = makeCompleteReceipt();
	receipt.rawEvidence.timingComplete = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"unfinished timing capture fails closed");
	receipt = makeCompleteReceipt();
	receipt.rawEvidence.timingLastFrame = 4095;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"timing capture must cover the final result frame");
	receipt = makeCompleteReceipt();
	receipt.workload.initialUnitCount = 7999;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"observed initial workload below the requested minimum fails closed");
	receipt = makeCompleteReceipt();
	receipt.workload.peakUnitCount = 10000;
	result |= check(ValidatePerformanceReceipt(receipt, &reason),
		"later production above the requested minimum remains truthful");
	receipt = makeCompleteReceipt();
	receipt.workload.rosterStable = false;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"changing playable roster fails closed");
	receipt = makeCompleteReceipt();
	receipt.workload.sampleCount = 4095;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"missing completed-frame workload observations fail closed");
	receipt = makeCompleteReceipt();
	receipt.phases[1].serialNanoseconds = receipt.phases[1].totalNanoseconds;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"unknown serial portion cannot be populated from the inclusive island clock");
	receipt = makeCompleteReceipt();
	receipt.phases[1].totalNanoseconds = 6001;
	result |= check(!ValidatePerformanceReceipt(receipt, &reason),
		"owner phases cannot double-count more wall time than the frame total");
	return result;
}

int testCompletedFrameWorkload()
{
	using namespace rts::performance;
	int result = 0;
	result |= check(IsPerformanceReceiptRosterPlayer(true, false) &&
		!IsPerformanceReceiptRosterPlayer(false, false) &&
		!IsPerformanceReceiptRosterPlayer(true, true),
		"roster includes playable non-observers without alive-state filtering");
	result |= check(IsPerformanceReceiptLiveUnit(true, false, false, false) &&
		IsPerformanceReceiptLiveUnit(false, true, false, false) &&
		!IsPerformanceReceiptLiveUnit(false, false, false, false) &&
		!IsPerformanceReceiptLiveUnit(true, false, true, false) &&
		!IsPerformanceReceiptLiveUnit(false, true, false, true),
		"workload excludes scenery, buildings, dead and destroyed units");
	PerformanceReceiptWorkload workload;
	result |= check(ObservePerformanceReceiptWorkload(workload, 10, 8, 1000) &&
		ObservePerformanceReceiptWorkload(workload, 11, 8, 800) &&
		ObservePerformanceReceiptWorkload(workload, 12, 8, 1200),
		"completed frames record observations");
	result |= check(workload.sampleCount == 3 && workload.firstFrame == 10 &&
		workload.lastFrame == 12 && workload.initialUnitCount == 1000 &&
		workload.minimumUnitCount == 800 && workload.peakUnitCount == 1200 &&
		workload.rosterStable && workload.contiguous,
		"sampled workload extrema and interval are measured exactly");
	result |= check(!ObservePerformanceReceiptWorkload(workload, 12, 1, 0) &&
		!ObservePerformanceReceiptWorkload(workload, 0, 1, 0) &&
		workload.sampleCount == 3 && workload.playerCount == 8 &&
		workload.minimumUnitCount == 800,
		"duplicate or reset frame cannot erase completed-match observations");
	result |= check(ObservePerformanceReceiptWorkload(workload, 14, 7, 1000) &&
		!workload.contiguous && !workload.rosterStable,
		"skipped frame and changed roster remain explicit invalid coverage");
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

int testObservedOnlyContractBoundary()
{
	using namespace rts::performance;
	PerformanceReceipt receipt = makeCompleteReceipt();
	std::string document;
	int result = check(SerializePerformanceReceipt(receipt, document) &&
		document.find("\"workloadQualification\":\"minimum-qualified\"") != std::string::npos &&
		document.find("\"kind\":\"replay\"") != std::string::npos &&
		document.find("\"identityObserved\":true") != std::string::npos,
		"scaling receipt explicitly distinguishes qualified workload from observed fixture identity");
	result |= check(document.find("\"simulationMode\":\"parallel\"") != std::string::npos &&
		document.find("\"schedulerStarted\":true") != std::string::npos,
		"parallel worker claims identify an actually started scheduler");
	return result;
}

int testObservedFixtureAndSerialOnlyValidation()
{
	using namespace rts::performance;
	int result = 0;
	PerformanceReceipt receipt = makeCompleteReceipt();
	receipt.fixtureIdentityObserved = false;
	result |= check(!ValidatePerformanceReceipt(receipt), "host fixture hash and seed cannot replace actual observation");
	receipt.expectedFixtureContentSha256 = receipt.fixtureContentSha256;
	receipt.expectedSeed = receipt.seed;
	receipt.expectedSeedKnown = true;
	result |= check(BindPerformanceReceiptFixtureObservation(receipt, "replay", receipt.replayPath.c_str(),
		receipt.fixtureContentSha256.c_str(), receipt.seed) && ValidatePerformanceReceipt(receipt),
		"actual replay observation matching the explicit expectation permits publication");
	result |= check(!BindPerformanceReceiptFixtureObservation(receipt, "replay", "other.rep",
		receipt.fixtureContentSha256.c_str(), receipt.seed) && !ValidatePerformanceReceipt(receipt),
		"a second identity binding invalidates the run rather than replacing its fixture");
	receipt = makeCompleteReceipt(); receipt.fixtureIdentityObserved = false;
	receipt.expectedSeedKnown = true; receipt.expectedSeed = receipt.seed + 1;
	result |= check(!BindPerformanceReceiptFixtureObservation(receipt, "replay", receipt.replayPath.c_str(),
		receipt.fixtureContentSha256.c_str(), receipt.seed) && !ValidatePerformanceReceipt(receipt),
		"actual seed mismatch remains fail closed even when a host seed was present");
	receipt = makeCompleteReceipt(); receipt.fixtureIdentityObserved = false;
	receipt.expectedFixtureContentSha256 = std::string(64, 'A');
	result |= check(!BindPerformanceReceiptFixtureObservation(receipt, "replay", receipt.replayPath.c_str(),
		receipt.fixtureContentSha256.c_str(), receipt.seed) && !ValidatePerformanceReceipt(receipt),
		"actual content mismatch cannot inherit the host hash as observed truth");
	receipt = makeCompleteReceipt();
	receipt.workloadQualification = "observed-only";
	receipt.requestedPlayerCount = 0; receipt.requestedMinimumUnitCount = 0;
	receipt.workload.playerCount = 7; receipt.workload.initialUnitCount = 0;
	receipt.workload.minimumUnitCount = 0;
	result |= check(ValidatePerformanceReceipt(receipt), "observed-only records actual roster and units without a fabricated minimum");
	std::string document;
	result |= check(SerializePerformanceReceipt(receipt, document) &&
		document.find("\"requestedPlayerCount\":null") != std::string::npos &&
		document.find("\"requestedMinimumUnitCount\":null") != std::string::npos,
		"unknown requested workload is serialized as null, not an observed zero-player match");
	receipt.requestedMinimumUnitCount = 1;
	result |= check(!ValidatePerformanceReceipt(receipt), "observed-only cannot smuggle a qualified requested minimum");
	receipt.requestedMinimumUnitCount = 0;
	receipt.simulationMode = "serial"; receipt.schedulerStarted = false;
	receipt.effectiveWorkerCount = 0; receipt.workersPinned = false;
	receipt.availableLogicalCpuCount = 0; receipt.reservedOwnerCpuCount = 0;
	receipt.selectedWorkerCpuCount = 0; receipt.selectedWorkerPhysicalCoreCount = 0;
	receipt.selectedWorkerPhysicalCoreMask = 0; receipt.selectedWorkerPhysicalCoreMaskComplete = false;
	receipt.cpuSets.clear(); receipt.ownerCpuSetIds.clear(); receipt.selectedWorkerCpuSetIds.clear();
	for (unsigned index = 0; index != receipt.kernels.size(); ++index)
	{
		const std::string name = receipt.kernels[index].name;
		receipt.kernels[index] = PerformanceReceiptKernel(); receipt.kernels[index].name = name;
	}
	result |= check(ValidatePerformanceReceipt(receipt), "actual serial simulation may truthfully report no scheduler or selected workers");
	result |= check(SerializePerformanceReceipt(receipt, document) &&
		document.find("\"source\":\"scheduler-not-started\"") != std::string::npos,
		"absent scheduler topology is not labeled as a CPU-set measurement");
	receipt.simulationMode = "parallel";
	result |= check(!ValidatePerformanceReceipt(receipt), "parallel simulation cannot use the serial zero-worker exception");
	receipt.simulationMode = "shadow";
	result |= check(!ValidatePerformanceReceipt(receipt), "shadow simulation cannot use the serial zero-worker exception");
	receipt.simulationMode = "serial"; receipt.schedulerStarted = true;
	result |= check(!ValidatePerformanceReceipt(receipt), "a started scheduler cannot be hidden as zero selected workers");
	receipt = makeCompleteReceipt(); receipt.simulationMode = "shadow";
	result |= check(ValidatePerformanceReceipt(receipt), "actual shadow simulation is represented without relabeling it parallel");
	receipt.workloadQualification = "observed-only"; receipt.requestedPlayerCount = 0; receipt.requestedMinimumUnitCount = 0;
	receipt.fixtureKind = "fresh-ai-map"; receipt.fixtureIdentityObserved = false;
	receipt.replayPath.clear();
	result |= check(BindPerformanceReceiptFixtureObservation(receipt, "fresh-ai-map", "Maps/Test/Test.map",
		receipt.fixtureContentSha256.c_str(), receipt.seed), "fresh AI binds loaded map bytes and actual game seed");
	result |= check(!ValidatePerformanceReceipt(receipt), "fresh AI cannot publish before closed retained replay identity is available");
	receipt.retainedReplayPath = "retained.rep"; receipt.retainedReplaySha256 = std::string(64, 'C');
	result |= check(ValidatePerformanceReceipt(receipt), "fresh AI map and retained replay are separate content identities");
	receipt.replayPath = receipt.fixtureContentPath;
	result |= check(!ValidatePerformanceReceipt(receipt), "map content path cannot be mislabeled as a replay source");
	return result;
}

int testFixtureEnvironmentExpectations()
{
	using namespace rts::performance;
	ReceiptTestEnvironment environment;
	const char *values[][2] = {
		{ "RTS_PERFORMANCE_ROLE", "performance-report" },
		{ "RTS_PERFORMANCE_RUN_ID", "environment-contract" },
		{ "RTS_PERFORMANCE_RUN_NONCE", "11111111-1111-4111-8111-111111111111" },
		{ "RTS_PERFORMANCE_COHORT_NONCE", "22222222-2222-4222-8222-222222222222" },
		{ "RTS_PERFORMANCE_COHORT_CREATED_UTC", "2026-01-01T00:00:00Z" },
		{ "RTS_PERFORMANCE_RECEIPT_DIR", "receipt-environment-test" },
		{ "RTS_PERFORMANCE_SOURCE_COMMIT", "0123456789abcdef0123456789abcdef01234567" },
		{ "RTS_PERFORMANCE_ARTIFACT_SET_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		{ "RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
		{ "RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
		{ "RTS_PERFORMANCE_FIXTURE_ID", "environment-fixture" },
		{ "RTS_PERFORMANCE_FIXTURE_SHA256", "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD" },
		{ "RTS_PERFORMANCE_RAW_LOG_PATH", "raw.log" },
		{ "RTS_PERFORMANCE_TIMING_PATH", "timing.csv" },
		{ "RTS_PERFORMANCE_VERIFIER_BOUNDARY", "test-only-no-publication" },
		{ "RTS_PERFORMANCE_REFERENCE_MODE", "throughput-binding" },
		{ "RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "minimum-qualified" },
		{ "RTS_PERFORMANCE_FIXTURE_KIND", "replay" },
		{ "RTS_PERFORMANCE_PLAYER_COUNT", "8" },
		{ "RTS_PERFORMANCE_UNIT_COUNT", "1000" },
		{ "RTS_PERFORMANCE_SEED", "1729" }
	};
	int result = 0;
	for (unsigned index = 0; index != sizeof(values) / sizeof(values[0]); ++index)
		result |= check(environment.set(values[index][0], values[index][1]), "test process environment is writable");
	PerformanceReceipt receipt;
	result |= check(BeginPerformanceReceipt(receipt, "Generals", "fixture.rep", 0) &&
		receipt.expectedSeedKnown && receipt.expectedSeed == 1729 &&
		!receipt.seedKnown && !receipt.fixtureIdentityObserved && receipt.fixtureContentSha256.empty(),
		"Begin retains host seed and content as expectations, never actual observations");
	const char *invalidUnsigned[] = { "-1", "+1", " 1", "4294967296", "18446744073709551616" };
	for (unsigned index = 0; index != sizeof(invalidUnsigned) / sizeof(invalidUnsigned[0]); ++index)
	{
		environment.set("RTS_PERFORMANCE_SEED", invalidUnsigned[index]);
		result |= check(!BeginPerformanceReceipt(receipt, "Generals", "fixture.rep", 0),
			"signed, padded or overflowed seed cannot alias a native unsigned expectation");
	}
	environment.set("RTS_PERFORMANCE_SEED", "4294967295");
	result |= check(BeginPerformanceReceipt(receipt, "Generals", "fixture.rep", 0) && receipt.expectedSeed == 0xffffffffU,
		"maximum canonical unsigned seed remains representable");
	environment.set("RTS_PERFORMANCE_SEED", 0);
	result |= check(!BeginPerformanceReceipt(receipt, "Generals", "fixture.rep", 0), "minimum-qualified replay requires an expected seed");
	environment.set("RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "observed-only");
	environment.set("RTS_PERFORMANCE_PLAYER_COUNT", 0); environment.set("RTS_PERFORMANCE_UNIT_COUNT", 0);
	result |= check(BeginPerformanceReceipt(receipt, "Generals", "fixture.rep", 0) &&
		!receipt.expectedSeedKnown && !receipt.seedKnown && receipt.requestedPlayerCount == 0 && receipt.requestedMinimumUnitCount == 0,
		"generic replay may omit unknown expected seed and requested workload");
	environment.set("RTS_PERFORMANCE_FIXTURE_KIND", "fresh-ai-map");
	environment.set("RTS_PERFORMANCE_FIXTURE_SHA256", 0);
	result |= check(BeginPerformanceReceipt(receipt, "Generals", "", 0) && !receipt.fixtureIdentityObserved &&
		receipt.replayPath.empty() && receipt.expectedFixtureContentSha256.empty(),
		"fresh AI can begin before map content is observed without inventing a replay path or hash");
	environment.set("RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "assumed-eight");
	result |= check(!BeginPerformanceReceipt(receipt, "Generals", "", 0), "unknown workload qualification fails closed");
	return result;
}

int testMeasuredEvidenceContract()
{
	using namespace rts::performance;
	int result = 0;
	PerformanceReceipt legacy = makeCompleteReceipt();
	legacy.schemaVersion = 1;
	legacy.producer = "game-executable-stage5-performance-report-v2";
	legacy.producerVersion = "2";
	result |= check(!ValidatePerformanceReceipt(legacy),
		"legacy receipt with host-asserted workload and no close proof must fail closed");

	PerformanceReceipt measured = makeCompleteReceipt();
	measured.phases.clear();
	const char *names[] = { "owner-intake", "legacy-mutable-island",
		"spatial-work", "owner-tail", "verification-publication" };
	for (unsigned index = 0; index != 5; ++index)
	{
		PerformanceReceiptPhase phase;
		phase.name = names[index];
		phase.available = true;
		phase.totalNanoseconds = 1000;
		phase.maximumNanoseconds = 1000;
		phase.sampleCount = 1;
		measured.phases.push_back(phase);
	}
	std::string document;
	result |= check(SerializePerformanceReceipt(measured, document),
		"measured owner-phase receipt serializes");
	result |= check(document.find("\"serialNanosecondsKnown\":false") !=
		std::string::npos,
		"inclusive owner clocks explicitly leave serial portions unknown");
	result |= check(document.find("\"requestedMinimumUnitCount\"") !=
		std::string::npos && document.find("\"workload\"") != std::string::npos,
		"requested workload is separate from executable observations");
	return result;
}

int testKernelTimingEvidenceContract()
{
	using namespace rts::performance;
	PerformanceReceipt legacy = makeCompleteReceipt();
	legacy.schemaVersion = 3;
	legacy.producer = "game-executable-stage5-performance-report-v3";
	legacy.producerVersion = "3";
	int result = check(!ValidatePerformanceReceipt(legacy),
		"receipt without run-finalized kernel timing cannot satisfy the new contract");
	std::string document;
	result |= check(SerializePerformanceReceipt(legacy, document),
		"kernel timing diagnostic contract serializes");
	result |= check(document.find("\"kernelTiming\":") != std::string::npos,
		"kernel timing retains a distinct executable snapshot");
	result |= check(document.find("\"serialReferenceKnown\":false") != std::string::npos,
		"kernel timing cannot self-attest an unexecuted serial reference");
	return result;
}

rts::JobMetricCounter receiptClock(void *context)
{
	return ++*static_cast<rts::JobMetricCounter *>(context);
}

int testKernelTimingSnapshotValidation()
{
	using namespace rts::performance;
	PerformanceReceipt measured = makeCompleteReceipt();
	KernelPerformanceLedger ledger;
	rts::JobMetricCounter tick = 100;
	ledger.beginRun(true, receiptClock, &tick);
	KernelPerformanceBatch batch = ledger.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 10, 1);
	for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
	{
		KernelPerformanceScope scope(&ledger, batch, static_cast<KernelPerformanceStage>(stage));
	}
	ledger.endBatch(batch, KERNEL_PERFORMANCE_COMMITTED);
	measured.kernelTiming = ledger.freeze();
	int result = check(ValidatePerformanceReceipt(measured),
		"real finalized exclusive ledger is accepted as local timing evidence");
	PerformanceReceipt invalid = measured;
	invalid.kernelTiming.frozen = false;
	result |= check(!ValidatePerformanceReceipt(invalid), "unclosed kernel ledger is rejected");
	invalid = measured;
	invalid.kernelTiming.errors = KERNEL_PERFORMANCE_ERROR_OWNER;
	result |= check(!ValidatePerformanceReceipt(invalid), "kernel ownership failure is rejected");
	invalid = measured;
	invalid.kernelTiming.streams[0].activePipelineNanoseconds++;
	result |= check(!ValidatePerformanceReceipt(invalid), "inclusive latency cannot replace exclusive stage sum");
	invalid = measured;
	invalid.kernelTiming.streams[0].stageSamples[0] = 0;
	result |= check(!ValidatePerformanceReceipt(invalid), "committed kernel cannot omit capture coverage");
	invalid = measured;
	invalid.kernelTiming.streams[0].abortedBatches = 1;
	result |= check(!ValidatePerformanceReceipt(invalid), "kernel admission and disposition counts must reconcile");
	invalid = measured;
	invalid.kernelTiming.streams[0].lastFrame = 4097;
	result |= check(!ValidatePerformanceReceipt(invalid), "kernel frames must belong to the replay");
	invalid = measured;
	invalid.kernelTiming.streams[0].subtype = 1;
	result |= check(!ValidatePerformanceReceipt(invalid), "unknown kernel subtype is rejected");
	invalid = measured;
	invalid.kernelTiming.streams[1] = invalid.kernelTiming.streams[0];
	invalid.kernelTiming.streamCount = 2;
	result |= check(!ValidatePerformanceReceipt(invalid), "duplicate kernel streams cannot double count timing");
	invalid = measured;
	invalid.kernelTiming.streamCount = KERNEL_PERFORMANCE_MAXIMUM_STREAMS + 1;
	std::string document;
	result |= check(!SerializePerformanceReceipt(invalid, document), "malformed kernel storage cannot overrun serialization");
	invalid = measured;
	invalid.kernelTiming.streams[0].kernel = static_cast<KernelPerformanceKernel>(-1);
	result |= check(!SerializePerformanceReceipt(invalid, document), "unknown kernel cannot index serializer names");
	return result;
}

int testMatchedReferenceReceiptContract()
{
	using namespace rts::performance;
	PerformanceReceipt unbound = makeCompleteReceipt();
	unbound.schemaVersion = 4;
	unbound.producer = "game-executable-stage5-performance-report-v4";
	unbound.producerVersion = "4";
	int result = check(!ValidatePerformanceReceipt(unbound),
		"receipt without an explicit throughput/oracle role and canonical reference snapshot cannot satisfy V5");
	std::string document;
	result |= check(SerializePerformanceReceipt(unbound, document), "reference-role receipt serializes");
	result |= check(document.find("\"kernelReference\":") != std::string::npos,
		"receipt retains canonical inputs outputs and commit correlation");
	result |= check(document.find("\"measurementRole\":\"throughput\"") != std::string::npos,
		"throughput receipt explicitly excludes serial-oracle elapsed evidence");
	return result;
}

bool writeReferenceValue(rts::performance::KernelPerformanceCanonicalWriter &writer,
	const void *value)
{
	return writer.u32(1, *static_cast<const unsigned *>(value));
}

bool copyReferenceValue(const void *input, void *output)
{
	*static_cast<unsigned *>(output) = *static_cast<const unsigned *>(input);
	return true;
}

int testReferenceSnapshotValidation()
{
	using namespace rts::performance;
	PerformanceReceipt measured = makeCompleteReceipt();
	KernelPerformanceLedger timing;
	KernelPerformanceReferenceLedger reference;
	rts::JobMetricCounter tick = 100;
	timing.beginRun(true, receiptClock, &tick);
	reference.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	const KernelPerformanceBatch batch = timing.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 10, 0);
	unsigned input = 42, output = 42;
	KernelPerformanceReferenceBatch binding;
	for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
	{
		KernelPerformanceScope scope(&timing, batch, static_cast<KernelPerformanceStage>(stage));
		if (stage == KERNEL_PERFORMANCE_VALIDATE)
			binding = reference.observeValidatedBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 10, 0,
				1, 2, writeReferenceValue, &input, writeReferenceValue, &output);
	}
	timing.endBatch(batch, KERNEL_PERFORMANCE_COMMITTED);
	reference.finishBatch(binding, true);
	measured.kernelTiming = timing.freeze();
	measured.kernelReference = reference.freeze();
	int result = check(ValidatePerformanceReceipt(measured),
		"one committed batch containing two operations binds to one timing batch");
	std::string document;
	result |= check(SerializePerformanceReceipt(measured, document) &&
		document.find("\"mode\":\"throughput-binding\"") != std::string::npos &&
		document.find("\"inputSha256\":\"") != std::string::npos &&
		document.find("\"committedOperationCount\":2") != std::string::npos,
		"throughput publishes canonical binding without claiming a serial measurement");
	PerformanceReceipt invalid = measured;
	invalid.kernelReference.frozen = false;
	result |= check(!ValidatePerformanceReceipt(invalid), "reference snapshot must be frozen");
	invalid = measured;
	invalid.kernelReference.errors = KERNEL_REFERENCE_ERROR_MISMATCH;
	result |= check(!ValidatePerformanceReceipt(invalid), "serial mismatch cannot become valid receipt evidence");
	invalid = measured;
	invalid.kernelReference.streams[0].serialSampleCount = 1;
	result |= check(!ValidatePerformanceReceipt(invalid), "throughput cannot include serial samples");
	invalid = measured;
	invalid.kernelReference.streams[0].serialNanoseconds = 1;
	result |= check(!ValidatePerformanceReceipt(invalid), "throughput cannot include hidden serial cost");
	invalid = measured;
	invalid.kernelReference.streams[0].committedBatchCount = 0;
	invalid.kernelReference.streams[0].abortedBatchCount = 1;
	invalid.kernelReference.streams[0].committedOperationCount = 0;
	result |= check(!ValidatePerformanceReceipt(invalid), "reference disposition must match authoritative timing commit");
	invalid = measured;
	invalid.kernelReference.streams[0].committedOperationCount = 3;
	result |= check(!ValidatePerformanceReceipt(invalid), "reference cannot commit more operations than it validated");
	invalid = measured;
	invalid.kernelReference.streams[0].validatedOperationCount = 3;
	result |= check(!ValidatePerformanceReceipt(invalid), "without an aborted batch no validated operation can disappear");
	invalid = measured;
	invalid.kernelTiming.streams[0].committedBatches = 0;
	invalid.kernelTiming.streams[0].abortedBatches = 1;
	invalid.kernelReference.streams[0].committedBatchCount = 0;
	invalid.kernelReference.streams[0].abortedBatchCount = 1;
	invalid.kernelReference.streams[0].committedOperationCount = 1;
	result |= check(!ValidatePerformanceReceipt(invalid), "zero committed batches cannot own a committed operation");
	invalid = measured;
	invalid.kernelReference.streams[0].inputDigest.valid = false;
	result |= check(!ValidatePerformanceReceipt(invalid), "missing canonical input hash fails closed");
	invalid = measured;
	invalid.kernelReference.streams[0].fieldSchema = 0;
	result |= check(!ValidatePerformanceReceipt(invalid), "canonical field layout requires an explicit version");
	invalid = measured;
	invalid.kernelReference.streams[0].firstFrame = 9;
	result |= check(!ValidatePerformanceReceipt(invalid), "reference batch frames must belong to its timing stream");
	invalid = measured;
	invalid.kernelReference.streams[0].kernel = KERNEL_PERFORMANCE_STATUS;
	result |= check(!ValidatePerformanceReceipt(invalid), "unmatched reference stream cannot borrow another kernel timing");
	invalid = measured;
	invalid.kernelReference.streams[1] = invalid.kernelReference.streams[0];
	invalid.kernelReference.streamCount = 2;
	result |= check(!ValidatePerformanceReceipt(invalid), "duplicate reference streams cannot duplicate serial cost");
	invalid = measured;
	invalid.kernelReference.streamCount = KERNEL_PERFORMANCE_MAXIMUM_STREAMS + 1;
	result |= check(!SerializePerformanceReceipt(invalid, document), "reference serialization checks fixed storage bounds");
	invalid = measured;
	invalid.kernelReference.streams[0].kernel = static_cast<KernelPerformanceKernel>(-1);
	result |= check(!SerializePerformanceReceipt(invalid, document), "reference serialization cannot index an unknown kernel");

	reference.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, receiptClock, &tick);
	unsigned detached = 0;
	binding = reference.observeValidatedBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 10, 0,
		1, 2, writeReferenceValue, &input, writeReferenceValue, &output,
		copyReferenceValue, &detached);
	reference.finishBatch(binding, true);
	measured.kernelReference = reference.freeze();
	result |= check(ValidatePerformanceReceipt(measured) && output == 42 && detached == 42,
		"independent oracle snapshot retains a real serial sample and leaves production output unchanged");
	result |= check(SerializePerformanceReceipt(measured, document) &&
		document.find("\"measurementRole\":\"serial-oracle\"") != std::string::npos &&
		document.find("\"mode\":\"owner-pipeline-observation\"") != std::string::npos,
		"oracle process elapsed and pipeline observations are explicitly not throughput");
	invalid = measured;
	invalid.kernelReference.streams[0].serialSampleCount = 0;
	result |= check(!ValidatePerformanceReceipt(invalid), "oracle sample count must equal committed batch count not operation count");
	invalid = measured;
	invalid.kernelReference.streams[0].maximumSerialNanoseconds = 2;
	result |= check(!ValidatePerformanceReceipt(invalid), "maximum serial sample cannot exceed total serial cost");
	return result;
}
}

int main()
{
	int result = 0;
	result |= testSerializationAndEscaping();
	result |= testStrictValidation();
	result |= testMissingEnvironmentFailsClosed();
	result |= testObservedOnlyContractBoundary();
	result |= testObservedFixtureAndSerialOnlyValidation();
	result |= testFixtureEnvironmentExpectations();
	result |= testMeasuredEvidenceContract();
	result |= testCompletedFrameWorkload();
	result |= testKernelTimingEvidenceContract();
	result |= testKernelTimingSnapshotValidation();
	result |= testMatchedReferenceReceiptContract();
	result |= testReferenceSnapshotValidation();
	return result;
}
