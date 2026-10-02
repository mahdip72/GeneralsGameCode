#include "Lib/PerformanceReceipt.h"
#include "../TestSupport/NativeKernelSourceConsumerTest.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

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

bool writeAtomicFixtureFile(const std::string &path, const char *bytes)
{
	HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, 0);
	if (file == INVALID_HANDLE_VALUE) return false;
	const DWORD count = static_cast<DWORD>(strlen(bytes));
	DWORD written = 0;
	const bool success = WriteFile(file, bytes, count, &written, 0) != FALSE &&
		written == count && FlushFileBuffers(file) != FALSE;
	return CloseHandle(file) != FALSE && success;
}

bool atomicFixtureFileEquals(const std::string &path, const char *expected)
{
	HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (file == INVALID_HANDLE_VALUE) return false;
	char bytes[64] = {};
	DWORD read = 0, extra = 0;
	const DWORD expectedCount = static_cast<DWORD>(strlen(expected));
	const bool success = expectedCount < sizeof(bytes) &&
		ReadFile(file, bytes, expectedCount, &read, 0) != FALSE &&
		read == expectedCount &&
		ReadFile(file, bytes + expectedCount, 1, &extra, 0) != FALSE && extra == 0 &&
		memcmp(bytes, expected, expectedCount) == 0;
	return CloseHandle(file) != FALSE && success;
}

struct AtomicCloseProbe
{
	AtomicCloseProbe() : calls(0), failCall(0) {}
	unsigned calls, failCall;
};

bool closeAtomicReceiptUsingProbe(void *context, void *nativeHandle)
{
	AtomicCloseProbe *probe = static_cast<AtomicCloseProbe *>(context);
	++probe->calls;
	const bool closed = CloseHandle(static_cast<HANDLE>(nativeHandle)) != FALSE;
	return closed && probe->calls != probe->failCall;
}

struct AtomicReplacementProbe
{
	std::string displacedPath;
	bool fired;
	AtomicReplacementProbe() : displacedPath(), fired(false) {}
};

bool replaceAtomicReceiptAfterCommit(void *context, void *,
	const char *publishedPath, std::string *)
{
	AtomicReplacementProbe *probe = static_cast<AtomicReplacementProbe *>(context);
	probe->fired = publishedPath != 0 &&
		MoveFileExA(publishedPath, probe->displacedPath.c_str(),
			MOVEFILE_WRITE_THROUGH) != FALSE &&
		writeAtomicFixtureFile(publishedPath, "replacement-receipt");
	return probe->fired;
}

int testAtomicPublicationFaultClosure()
{
	using namespace rts::performance;
	char current[MAX_PATH] = {};
	const DWORD currentLength = GetCurrentDirectoryA(sizeof(current), current);
	char leaf[96] = {};
	_snprintf(leaf, sizeof(leaf), "pra-%lu-%lu",
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long>(GetTickCount()));
	const std::string directory = currentLength != 0 && currentLength < sizeof(current) ?
		std::string(current) + "\\" + leaf : std::string();
	int result = check(!directory.empty() && directory.size() + 160 < MAX_PATH &&
		CreateDirectoryA(directory.c_str(), 0) != FALSE,
		"atomic publication uses one fresh bounded fixture directory");
	if (result != 0) return result;
	const std::string rawPath = directory + "\\run.log";
	const std::string timingPath = directory + "\\timing.csv";
	const std::string finalPath = directory +
		"\\performance-receipt-run-20260901-0001-4242.json";
	result |= check(writeAtomicFixtureFile(rawPath, "raw-evidence") &&
		writeAtomicFixtureFile(timingPath, "timing-evidence"),
		"atomic publication fixture evidence is exclusively created");
	PerformanceReceipt receipt = makeCompleteReceipt();
	receipt.outputDirectory = directory;
	receipt.rawEvidence.rawLogPath = rawPath;
	receipt.rawEvidence.timingPath = timingPath;
	AtomicCloseProbe closeProbe;
	closeProbe.failCall = 1;
	std::string writtenPath, reason;
	result |= check(!WritePerformanceReceiptAtomically(receipt, directory.c_str(),
		&writtenPath, &reason, 0, 0, closeAtomicReceiptUsingProbe, &closeProbe) &&
		closeProbe.calls == 1 && writtenPath.empty() &&
		GetFileAttributesA(finalPath.c_str()) == INVALID_FILE_ATTRIBUTES,
		"reported writer-handle close failure aborts before atomic rename");

	DeleteFileA(rawPath.c_str());
	DeleteFileA(timingPath.c_str());
	result |= check(writeAtomicFixtureFile(rawPath, "raw-evidence") &&
		writeAtomicFixtureFile(timingPath, "timing-evidence"),
		"replacement fixture evidence is recreated without reuse");
	receipt = makeCompleteReceipt();
	receipt.outputDirectory = directory;
	receipt.rawEvidence.rawLogPath = rawPath;
	receipt.rawEvidence.timingPath = timingPath;
	AtomicReplacementProbe replacement;
	replacement.displacedPath = finalPath + ".displaced";
	writtenPath.clear(); reason.clear();
	result |= check(!WritePerformanceReceiptAtomically(receipt, directory.c_str(),
		&writtenPath, &reason, replaceAtomicReceiptAfterCommit, &replacement) &&
		replacement.fired && writtenPath.empty() &&
		atomicFixtureFileEquals(finalPath, "replacement-receipt") &&
		GetFileAttributesA(replacement.displacedPath.c_str()) == INVALID_FILE_ATTRIBUTES,
		"post-commit pathname replacement is rejected and exact original identity is cleaned");
	DeleteFileA(finalPath.c_str());
	DeleteFileA(rawPath.c_str());
	DeleteFileA(timingPath.c_str());
	result |= check(writeAtomicFixtureFile(rawPath, "raw-evidence") &&
		writeAtomicFixtureFile(timingPath, "timing-evidence"),
		"final-close fixture evidence is recreated without reuse");
	receipt = makeCompleteReceipt();
	receipt.outputDirectory = directory;
	receipt.rawEvidence.rawLogPath = rawPath;
	receipt.rawEvidence.timingPath = timingPath;
	AtomicCloseProbe finalCloseProbe;
	finalCloseProbe.failCall = 2;
	writtenPath.clear(); reason.clear();
	result |= check(!WritePerformanceReceiptAtomically(receipt, directory.c_str(),
		&writtenPath, &reason, 0, 0, closeAtomicReceiptUsingProbe, &finalCloseProbe) &&
		finalCloseProbe.calls == 2 && writtenPath.empty() &&
		GetFileAttributesA(finalPath.c_str()) == INVALID_FILE_ATTRIBUTES,
		"reported published-handle close failure deletes the exact committed receipt");
	DeleteFileA(rawPath.c_str());
	DeleteFileA(timingPath.c_str());
	result |= check(RemoveDirectoryA(directory.c_str()) != FALSE,
		"atomic publication fault cases leave no hidden temporary artifact");
	return result;
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

int testNativeRunIdentityGoldenAndBoundaries()
{
	using namespace rts::performance;
	using rts::JobMetricCounter;
	int result = 0;
	PerformanceReceipt receipt;
	receipt.runId = "R-1";
	receipt.runNonce = "01234567-89ab-4cde-8fab-0123456789ab";
	receipt.processId = 0x1234abcd;
	receipt.processCreationTimeUtc100ns = 0x0102030405060708ULL;
	// Literal domain/schema, sequence1 with R-1, sequence2 with the exact
	// nonce spelling, u32 tag3 PID, u64 tag4 FILETIME; all integers are LE.
	// The 415 bytes and SHA were obtained independently of CanonicalWriter.
	static const unsigned char canonical[] = {
		0x52, 0x54, 0x53, 0x2d, 0x4b, 0x45, 0x52, 0x4e, 0x45, 0x4c, 0x2d, 0x46, 0x49, 0x45, 0x4c, 0x44,
		0x53, 0x2d, 0x76, 0x31, 0x03, 0x50, 0x00, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x2d,
		0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0x06, 0x02, 0x00, 0x00,
		0x00, 0x24, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x01, 0x02,
		0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
		0x01, 0x02, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x34, 0x00,
		0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00,
		0x36, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
		0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x39, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x61, 0x00, 0x00,
		0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x62, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x2d,
		0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
		0x00, 0x63, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x01, 0x02,
		0x00, 0x00, 0x00, 0x65, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00,
		0x01, 0x02, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x66, 0x00,
		0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x61, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00,
		0x62, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
		0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00,
		0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x35,
		0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
		0x00, 0x37, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0x02,
		0x00, 0x00, 0x00, 0x39, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x61, 0x00, 0x00, 0x00,
		0x01, 0x02, 0x00, 0x00, 0x00, 0x62, 0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0xcd, 0xab,
		0x34, 0x12, 0x03, 0x04, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01
	};
	static const unsigned char expected[] = {
		0xf3, 0xea, 0x40, 0x01, 0x2c, 0xf2, 0x19, 0x79, 0xc5, 0x0b, 0x11, 0x0d, 0x0f, 0xf6, 0x7b, 0x72,
		0x81, 0x76, 0xa4, 0x4d, 0xb6, 0x52, 0xdc, 0x6a, 0x22, 0x47, 0xac, 0x63, 0x40, 0x15, 0x92, 0x67
	};
	// Hash literal artifact bytes directly, without the production field encoder.
	KernelPerformanceDigest literal;
	BCRYPT_ALG_HANDLE algorithm = 0;
	BCRYPT_HASH_HANDLE hash = 0;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0) == 0)
	{
		if (BCryptCreateHash(algorithm, &hash, 0, 0, 0, 0, 0) == 0)
		{
			literal.valid = BCryptHashData(hash, const_cast<PUCHAR>(canonical),
				static_cast<ULONG>(sizeof(canonical)), 0) == 0 &&
				BCryptFinishHash(hash, literal.bytes, sizeof(literal.bytes), 0) == 0;
			BCryptDestroyHash(hash);
		}
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	result |= check(sizeof(canonical) == 415 && literal.valid &&
		memcmp(literal.bytes, expected, sizeof(expected)) == 0,
		"identity golden literal bytes have the independently calculated SHA-256");
	const KernelPerformanceDigest identity = GetPerformanceReceiptRunIdentity(receipt);
	result |= check(identity.valid && memcmp(identity.bytes, expected, sizeof(expected)) == 0,
		"native identity matches exact golden bytes before any completed receipt exists");
	const char *mutationNames[] = {
		"native identity binds exact runId bytes",
		"native identity binds exact nonce bytes",
		"native identity binds the full unsigned processId",
		"native identity preserves one FILETIME tick above 2^53",
		"native identity preserves canonical UUID letter case without normalization"
	};
	for (unsigned mutation = 0; mutation != 5; ++mutation)
	{
		PerformanceReceipt changed = receipt;
		if (mutation == 0) changed.runId = "R-2";
		if (mutation == 1) changed.runNonce[0] = '1';
		if (mutation == 2) changed.processId++;
		if (mutation == 3) changed.processCreationTimeUtc100ns++;
		if (mutation == 4) changed.runNonce[11] = 'A';
		const KernelPerformanceDigest actual = GetPerformanceReceiptRunIdentity(changed);
		result |= check(actual.valid && !actual.equals(identity), mutationNames[mutation]);
	}
	PerformanceReceipt boundary = receipt;
	boundary.runId = std::string(253, 'x') + "._-";
	result |= check(GetPerformanceReceiptRunIdentity(boundary).valid,
		"native identity accepts the exact 256-byte safe ASCII runId boundary");
	boundary = receipt;
	boundary.processId = ~0U;
	boundary.processCreationTimeUtc100ns = ~static_cast<JobMetricCounter>(0);
	const KernelPerformanceDigest maximum = GetPerformanceReceiptRunIdentity(boundary);
	result |= check(maximum.valid && !maximum.equals(identity),
		"native identity accepts full unsigned PID and FILETIME widths without narrowing");
	PerformanceReceipt unrelated = receipt;
	unrelated.schemaVersion = 6;
	unrelated.producer = "game-executable-stage5-performance-report-v6";
	unrelated.status = "failed";
	unrelated.runId = receipt.runId;
	unrelated.cohortNonce = "unrelated-host-label";
	unrelated.executablePath = "unrelated.exe";
	unrelated.processIdentityAvailable = true;
	unrelated.processStartTimeUtc100ns = 123;
	unrelated.processEndTimeUtc100ns = 456;
	unrelated.processExitCodeKnown = true;
	unrelated.processExitCode = 9;
	unrelated.kernelReference.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	result |= check(GetPerformanceReceiptRunIdentity(unrelated).equals(identity),
		"native identity has only four fields and does not confer receipt validity or a role");
	const char *invalidNames[] = {
		"native identity rejects empty runId", "native identity rejects oversized runId",
		"native identity rejects runId traversal", "native identity rejects runId slash",
		"native identity rejects runId backslash", "native identity rejects runId whitespace",
		"native identity rejects embedded runId NUL", "native identity rejects non-ASCII runId",
		"native identity rejects runId DEL", "native identity rejects empty nonce",
		"native identity rejects short nonce", "native identity rejects non-hex nonce",
		"native identity rejects shifted UUID hyphen", "native identity rejects UUID version zero",
		"native identity rejects non-RFC UUID variant", "native identity rejects embedded nonce NUL",
		"native identity rejects zero PID", "native identity rejects zero FILETIME"
	};
	for (unsigned mutation = 0; mutation != 18; ++mutation)
	{
		PerformanceReceipt invalid = receipt;
		if (mutation == 0) invalid.runId.clear();
		if (mutation == 1) invalid.runId = std::string(257, 'x');
		if (mutation == 2) invalid.runId = "a..b";
		if (mutation == 3) invalid.runId = "a/b";
		if (mutation == 4) invalid.runId = "a\\b";
		if (mutation == 5) invalid.runId = "a b";
		if (mutation == 6) invalid.runId = std::string("a\0b", 3);
		if (mutation == 7) invalid.runId = std::string("a") + static_cast<char>(0xe9);
		if (mutation == 8) invalid.runId = std::string("a") + static_cast<char>(0x7f);
		if (mutation == 9) invalid.runNonce.clear();
		if (mutation == 10) invalid.runNonce.resize(35);
		if (mutation == 11) invalid.runNonce[0] = 'g';
		if (mutation == 12) invalid.runNonce[8] = '0';
		if (mutation == 13) invalid.runNonce[14] = '0';
		if (mutation == 14) invalid.runNonce[19] = '7';
		if (mutation == 15) invalid.runNonce[20] = '\0';
		if (mutation == 16) invalid.processId = 0;
		if (mutation == 17) invalid.processCreationTimeUtc100ns = 0;
		result |= check(!GetPerformanceReceiptRunIdentity(invalid).valid, invalidNames[mutation]);
	}
	return result;
}

// Literal frozen snapshots below test receipt serialization and validation,
// not actual source/body execution or file-origin authentication. A's real
// source-consumer tests and D's existing producer lifecycle own those proofs.
using namespace rts::performance;
using rts::JobMetricCounter;

KernelPerformanceDigest phaseWireDigest(unsigned char first)
{
    KernelPerformanceDigest value;
    value.valid = true;
    for (unsigned i = 0; i != 32; ++i)
        value.bytes[i] = static_cast<unsigned char>(first + i);
    return value;
}

std::string phaseWireSha256(const KernelPerformanceDigest &digest)
{
    const char hex[] = "0123456789ABCDEF";
    std::string value;
    for (unsigned i = 0; i != 32; ++i)
    {
        value += hex[digest.bytes[i] >> 4];
        value += hex[digest.bytes[i] & 15];
    }
    return value;
}

KernelPerformanceDigest phaseWireRunIdentity(const std::string &runId,
    const std::string &nonce, unsigned processId, JobMetricCounter created)
{
    // Fixture input construction only. The independent 0x5003 golden bytes
    // are checked by testNativeRunIdentityGoldenAndBoundaries, not this helper.
    KernelPerformanceCanonicalWriter writer;
    writer.begin(0x5003);
    writer.sequence(1, static_cast<unsigned>(runId.size()));
    for (unsigned i = 0; i != runId.size(); ++i)
        writer.u32(1, static_cast<unsigned char>(runId[i]));
    writer.sequence(2, static_cast<unsigned>(nonce.size()));
    for (unsigned i = 0; i != nonce.size(); ++i)
        writer.u32(2, static_cast<unsigned char>(nonce[i]));
    writer.u32(3, processId);
    writer.u64(4, created);
    return writer.finish();
}

PerformanceReceipt makeV6MixedPhaseWireFixture()
{
    PerformanceReceipt receipt = makeCompleteReceipt();
    receipt.schemaVersion = 6;
    receipt.producer = "game-executable-stage5-performance-report-v6";
    receipt.producerVersion = "6";
    receipt.runId = "baseline-ordinal-1";
    receipt.runNonce = "33333333-3333-4333-8333-333333333333";
    receipt.processId = 4243;
    receipt.processCreationTimeUtc100ns = 133000000000000003ULL;
    receipt.processStartTimeUtc100ns = 133000000000000004ULL;
    receipt.processEndTimeUtc100ns = 133000000000000010ULL;
    receipt.receiptPath = "H:\\evidence\\performance-receipt-baseline-ordinal-1-4243.json";
    receipt.frameStart = 7;
    receipt.frameEnd = receipt.finalFrame = 8;
    receipt.workload.sampleCount = 1;
    receipt.workload.firstFrame = receipt.workload.lastFrame = 8;
    // Deliberately distinct from A's 200 ns owner-accounting frame.
    receipt.frameSimulationTotalNanoseconds = 777;
    receipt.frameSimulationMaximumNanoseconds = 777;
    receipt.frameSimulationSampleCount = 1;
    receipt.rawEvidence.timingFrameSamples = 2;
    receipt.rawEvidence.timingFirstFrame = 7;
    receipt.rawEvidence.timingLastFrame = 8;

    KernelPerformanceSnapshot &timing = receipt.kernelTiming;
    timing.runRole = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
    timing.enabled = timing.frozen = timing.complete = true;
    timing.errors = 0; timing.generation = 1; timing.streamCount = 1;
    KernelPerformancePhaseAccountingSnapshot &phase = timing.phaseAccounting;
    phase.requested = phase.frozen = phase.complete = true;
    phase.errors = 0;
    phase.completedFrameCount = 1;
    phase.firstCompletedFrame = phase.lastCompletedFrame = 8;
    phase.frameNanoseconds = phase.maximumFrameNanoseconds = 200;
    phase.unscopedSerialNanoseconds = 40;
    phase.completionSerialNanoseconds = 70;
    phase.completionSampleCount = 1;
    phase.schedulerClosureKnown = true;
    const JobMetricCounter totals[5] = {10, 20, 100, 10, 20};
    const JobMetricCounter serial[5] = {10, 20, 65, 10, 20};
    const JobMetricCounter pure[5] = {0, 0, 35, 0, 0};
    for (unsigned i = 0; i != 5; ++i)
    {
        phase.phases[i].totalNanoseconds = totals[i];
        phase.phases[i].serialNanoseconds = serial[i];
        phase.phases[i].pureNanoseconds = pure[i];
        phase.phases[i].samples = 1;
        phase.phases[i].maximumNanoseconds = totals[i];
        receipt.phases[i].available = true;
        receipt.phases[i].totalNanoseconds = totals[i];
        receipt.phases[i].maximumNanoseconds = totals[i];
        receipt.phases[i].sampleCount = 1;
        receipt.phases[i].serialNanoseconds = serial[i];
        receipt.phases[i].serialNanosecondsKnown = true;
        receipt.phases[i].pureNanoseconds = pure[i];
        receipt.phases[i].pureNanosecondsKnown = true;
    }

    KernelPerformanceStream &pipeline = timing.streams[0];
    pipeline.kernel = KERNEL_PERFORMANCE_PATH; pipeline.subtype = 0;
    pipeline.firstFrame = pipeline.lastFrame = 7;
    pipeline.attemptedBatches = 4; pipeline.admittedBatches = 2;
    pipeline.committedBatches = 1; pipeline.abortedBatches = 1;
    for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
    {
        pipeline.stageSamples[stage] = stage == KERNEL_PERFORMANCE_WAIT ? 0 : 1;
        pipeline.stageNanoseconds[stage] = stage == KERNEL_PERFORMANCE_WAIT ? 0 : 10;
    }
    pipeline.activePipelineNanoseconds = 40;
    pipeline.inclusiveBatchNanoseconds = 100;
    pipeline.maximumBatchNanoseconds = 70;

    KernelPerformanceReferenceSnapshot &reference = receipt.kernelReference;
    reference.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
    reference.frozen = reference.complete = true;
    reference.errors = 0; reference.generation = 1; reference.streamCount = 1;
    KernelPerformanceReferenceStream &stream = reference.streams[0];
    stream.kernel = KERNEL_PERFORMANCE_PATH; stream.subtype = 0;
    stream.fieldSchema = 1; stream.firstFrame = stream.lastFrame = 7;
    stream.validatedBatchCount = stream.committedBatchCount = 1;
    stream.validatedOperationCount = stream.committedOperationCount = 2;
    stream.inputDigest = phaseWireDigest(0x11);
    stream.outputDigest = phaseWireDigest(0x22);
    stream.commitDigest = phaseWireDigest(0x33);

    KernelPerformanceTraceSnapshot &trace = reference.trace;
    trace.mode = KERNEL_TRACE_CONSUME;
    trace.requested = trace.frozen = trace.complete = true;
    trace.observationSealed = trace.executionSealed = true;
    trace.errors = 0;
    trace.limits.maximumBytes = 1048576; trace.limits.maximumRecords = 100000;
    trace.limits.maximumLogicalEvents = 100000; trace.limits.maximumAttempts = 10000;
    trace.limits.maximumRanges = 500000;
    trace.residentAttemptCapacity = 15; trace.residentRangeCapacity = 340;
    trace.residentAttemptHighWater = 2; trace.residentRangeHighWater = 3;
    trace.attemptCount = 4; trace.admittedAttemptCount = 2;
    trace.notAdmittedAttemptCount = 2; trace.abortedAfterAdmissionAttemptCount = 1;
    trace.reapCount = 4; trace.capturedAttemptCount = 2;
    trace.capturedOperationCount = 5; trace.dispatchCount = 2;
    trace.rangeCount = trace.releasedRangeCount = 5;
    trace.recordCount = trace.logicalEventCount = 34;
    trace.windowBoundaryCount = 12;
    trace.completedWindowCount = 1;
    trace.controlWindowCount = 0;
    // Synthetic wire length/hash only, not A's golden packet length.
    trace.byteCount = 4096; trace.digest = phaseWireDigest(0x44);
    trace.sourceReceiptDigest = phaseWireDigest(0x55);
    receipt.traceFiles.tracePath = "H:\\evidence\\source-trace.bin";
    receipt.traceFiles.sourceReceiptPath = "H:\\evidence\\source-receipt.json";
    receipt.traceFiles.sourceRunId = "source-ordinal-1";
    receipt.traceFiles.sourceRunNonce = "11111111-1111-4111-8111-111111111111";
    receipt.traceFiles.sourceProcessId = 4242;
    receipt.traceFiles.sourceProcessCreationTimeUtc100ns = 133000000000000001ULL;
    trace.binding.nativeRunIdentity = phaseWireRunIdentity(
        receipt.traceFiles.sourceRunId, receipt.traceFiles.sourceRunNonce,
        receipt.traceFiles.sourceProcessId,
        receipt.traceFiles.sourceProcessCreationTimeUtc100ns);
    trace.binding.executable = phaseWireDigest(0x66);
    trace.binding.fixture = phaseWireDigest(0x77);
    trace.binding.sourcePolicy = phaseWireDigest(0x88);
    receipt.executableSha256 = phaseWireSha256(trace.binding.executable);
    receipt.fixtureContentSha256 = phaseWireSha256(trace.binding.fixture);
    for (unsigned i = 0; i != receipt.kernels.size(); ++i)
    {
        PerformanceReceiptKernel &kernel = receipt.kernels[i];
        kernel.submittedJobs = kernel.completedJobs = kernel.physicalWorkerJobs = 0;
        kernel.ownerHelpedJobs = kernel.physicalWorkerMask = 0;
        kernel.distinctPhysicalWorkers = 0;
        kernel.elapsedNanoseconds = 0; kernel.elapsedNanosecondsKnown = false;
    }
    return receipt;
}

int testV6CompletePartitionWire()
{
    const PerformanceReceipt complete = makeV6MixedPhaseWireFixture();
    int result = 0;
    std::string reason, json;
    result |= check(ValidatePerformanceReceipt(complete, &reason),
        "complete mixed baseline is valid coverage even though serial235/total270 fails 2x");
    result |= check(SerializePerformanceReceipt(complete, json, &reason) &&
        json.find("\"measurementRole\":\"phase-serial-baseline\"") != std::string::npos &&
        json.find("\"frameNanoseconds\":200") != std::string::npos &&
        json.find("\"completionSerialNanoseconds\":70") != std::string::npos &&
        json.find("\"totalNanoseconds\":777") != std::string::npos &&
        json.find("133000000000000001") != std::string::npos,
        "V6 keeps independent legacy duration, exact phase/completion and source FILETIME");
    for (unsigned mutation = 0; mutation != 15; ++mutation)
    {
        PerformanceReceipt invalid = complete;
        auto &phase = invalid.kernelTiming.phaseAccounting;
        auto &trace = invalid.kernelReference.trace;
        if (mutation == 0) phase.unscopedSerialNanoseconds--;
        if (mutation == 1) invalid.phases[2].pureNanoseconds++;
        if (mutation == 2) invalid.phases[2].serialNanosecondsKnown = false;
        if (mutation == 3) phase.completionSampleCount = 0;
        if (mutation == 4) phase.schedulerEnd.executedJobs++;
        if (mutation == 5) phase.schedulerEnd.pendingJobs = 1;
        if (mutation == 6) phase.schedulerClosureKnown = false;
        if (mutation == 7) trace.requested = false;
        if (mutation == 8) trace.residentRangeCount = 1;
        if (mutation == 9) trace.reapCount--;
        if (mutation == 10) trace.sourceReceiptDigest.valid = false;
        if (mutation == 11) invalid.traceFiles.sourceProcessCreationTimeUtc100ns++;
        if (mutation == 12) invalid.kernelReference.streams[0].serialSampleCount = 1;
        if (mutation == 13) invalid.kernelTiming.streams[0].stageSamples[KERNEL_PERFORMANCE_WAIT] = 1;
        if (mutation == 14) phase.frameNanoseconds = ~static_cast<JobMetricCounter>(0);
        result |= check(!ValidatePerformanceReceipt(invalid, &reason),
            "V6 source/partition/closure mutation cannot remain a valid native receipt");
    }
    return result;
}

int testV6RoleAndControlWire()
{
    int result = 0;
    std::string json;
    PerformanceReceipt baseline = makeV6MixedPhaseWireFixture();
    result |= check(SerializePerformanceReceipt(baseline, json) &&
        json.find("\"controlAccounting\":") != std::string::npos &&
        json.find("\"windowCount\":0") != std::string::npos,
        "baseline wire explicitly represents no control window without inventing world frame zero");
    PerformanceReceipt throughput = makeCompleteReceipt();
    throughput.schemaVersion = 6;
    throughput.producer = "game-executable-stage5-performance-report-v6";
    throughput.producerVersion = "6";
    result |= check(ValidatePerformanceReceipt(throughput),
        "untraced V6 throughput keeps real inclusive phases and unknown serial/pure values");
    result |= check(SerializePerformanceReceipt(throughput, json) &&
        json.find("\"phaseAccounting\":null") != std::string::npos &&
        json.find("\"attemptTrace\":null") != std::string::npos &&
        json.find("\"pureNanosecondsKnown\":false") != std::string::npos,
        "ordinary V6 null means not requested and cannot backfill a known zero fraction");
    return result;
}

struct ReceiptWindowClock
{
    rts::JobMetricCounter value;
    unsigned reads;
    static rts::JobMetricCounter read(void *context)
    {
        ReceiptWindowClock &clock = *static_cast<ReceiptWindowClock *>(context);
        ++clock.reads;
        return clock.value;
    }
};

int testActualControlEngineReceiptProjection()
{
    using namespace rts::performance;
    using rts::JobMetricCounter;
    ReceiptWindowClock clock = {100, 0};
    KernelPerformanceLedger timing;
    KernelPerformanceTimingRunOptions options;
    options.enabled = true;
    options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
    options.clock = ReceiptWindowClock::read;
    options.clockContext = &clock;
    int result = check(timing.beginRun(options), "control projection starts the actual accounting engine");
    const KernelPerformanceSchedulerBoundary scheduler;
    const JobMetricCounter starts[3] = {100,250,400};
    const JobMetricCounter ends[3] = {200,330,600};
    const JobMetricCounter phaseStarts[3][5] = {
        {110,145,160,175,190}, {260,275,290,305,320}, {410,455,480,525,560}
    };
    const JobMetricCounter phaseEnds[3][5] = {
        {140,155,170,185,195}, {270,285,300,315,325}, {450,475,520,555,590}
    };
    for (unsigned sample = 0; sample != 3; ++sample)
    {
        clock.value = starts[sample];
        const KernelPerformanceFrame frame = timing.beginFrame(sample + 1, sample == 0 ? 83 : 0, scheduler);
        result |= check(frame.valid(), "real control and world sample ordinals share one engine");
        for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
        {
            const KernelPerformancePhase phase = static_cast<KernelPerformancePhase>(index);
            clock.value = phaseStarts[sample][index];
            result |= check(timing.beginPhase(frame, phase), "actual control/world phase begins");
            if (index == 0 && sample == 0)
                result |= check(timing.observeControlTransition(frame, KERNEL_CONTROL_DEFERRED_START_DECLARED),
                    "one declaration precedes repeated pending controls");
            if (index == 0 && sample == 2)
                result |= check(timing.observeControlTransition(frame, KERNEL_CONTROL_DEFERRED_START_CONSUMED),
                    "loaded world consumes the retained declaration");
            clock.value = phaseEnds[sample][index];
            result |= check(timing.endPhase(frame, phase), "actual control/world phase ends");
        }
        clock.value = ends[sample];
        result |= check(sample < 2 ? timing.endControlWindow(frame, 0, scheduler) : timing.endFrame(frame, 1, scheduler),
            "two pending control samples are not completed world frames");
    }
    result |= check(timing.sealAdmissions(), "control projection seals new timing admissions");
    clock.value = 700;
    const KernelPerformanceInterval completion = timing.beginCompletionSerial();
    clock.value = 710;
    result |= check(completion.valid() && timing.endCompletionSerial(completion) && timing.sealExecutionClosure(scheduler),
        "late completion remains a separate serial extent");
    const KernelPerformanceSnapshot observed = timing.freeze();
    const KernelPerformancePhaseAccountingSnapshot &phase = observed.phaseAccounting;
    result |= check(phase.complete && phase.errors == 0 && phase.controlWindowCount == 2 &&
        phase.firstControlSampleOrdinal == 1 && phase.lastControlSampleOrdinal == 2 &&
        phase.controlNanoseconds == 180 && phase.maximumControlNanoseconds == 100 &&
        phase.controlUnscopedSerialNanoseconds == 70 && phase.completedFrameCount == 1 &&
        phase.firstCompletedFrame == 1 && phase.lastCompletedFrame == 1 && phase.frameNanoseconds == 200 &&
        phase.completionSerialNanoseconds == 10 && phase.completionSampleCount == 1,
        "independent literals match the actual frozen engine");
    const JobMetricCounter controlTotals[] = {40, 20, 20, 20, 10};
    const JobMetricCounter controlMaxima[] = {30, 10, 10, 10, 5};
    const JobMetricCounter worldTotals[] = {40, 20, 40, 30, 30};
    JobMetricCounter serial = phase.unscopedSerialNanoseconds + phase.controlUnscopedSerialNanoseconds +
        phase.completionSerialNanoseconds, pure = 0;
    for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
    {
        const KernelPerformancePhaseAccountingRow &control = phase.controlPhases[index];
        const KernelPerformancePhaseAccountingRow &world = phase.phases[index];
        result |= check(control.totalNanoseconds == controlTotals[index] &&
            control.serialNanoseconds == controlTotals[index] && control.pureNanoseconds == 0 &&
            control.samples == 2 && control.maximumNanoseconds == controlMaxima[index] &&
            world.totalNanoseconds == worldTotals[index] && world.serialNanoseconds == worldTotals[index] &&
            world.pureNanoseconds == 0 && world.samples == 1 && world.maximumNanoseconds == worldTotals[index],
            "actual control and world rows retain independent totals maxima samples and known all-serial work");
        serial += control.serialNanoseconds + world.serialNanoseconds;
        pure += control.pureNanoseconds + world.pureNanoseconds;
    }
    result |= check(serial == 390 && pure == 0 &&
        phase.frameNanoseconds + phase.controlNanoseconds + phase.completionSerialNanoseconds == 390,
        "actual two-control world and late-completion extent reconciles to390 before any aggregation");
    if (result != 0) return result; // A prerequisite failure is not an E receipt RED.

    PerformanceReceipt receipt = makeV6MixedPhaseWireFixture();
    receipt.kernelTiming = observed; // Copy the real frozen snapshot, not expected literals.
    receipt.frameStart = 0; receipt.frameEnd = receipt.finalFrame = 1;
    receipt.workload.sampleCount = 1; receipt.workload.firstFrame = receipt.workload.lastFrame = 1;
    receipt.rawEvidence.timingFirstFrame = 0; receipt.rawEvidence.timingLastFrame = 1;
    receipt.kernelReference.streamCount = 0; receipt.kernelReference.complete = false;
    for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
    {
        const KernelPerformancePhaseAccountingRow &row = phase.phases[index];
        PerformanceReceiptPhase &wire = receipt.phases[index];
        wire.totalNanoseconds = row.totalNanoseconds; wire.maximumNanoseconds = row.maximumNanoseconds;
        wire.sampleCount = row.samples; wire.serialNanoseconds = row.serialNanoseconds;
        wire.pureNanoseconds = row.pureNanoseconds;
    }
    // This enclosing source snapshot remains a clearly labelled WIRE fixture.
    // A/D's native source fixture separately proves the window record grammar.
    KernelPerformanceTraceSnapshot &trace = receipt.kernelReference.trace;
    trace.windowBoundaryCount = 38; trace.completedWindowCount = 1; trace.controlWindowCount = 2;
    trace.recordCount = trace.logicalEventCount = 42; // header/footer2 + boundaries38 + seals2
    trace.attemptCount = trace.admittedAttemptCount = trace.notAdmittedAttemptCount = 0;
    trace.abortedAfterAdmissionAttemptCount = trace.reapCount = trace.capturedAttemptCount = 0;
    trace.capturedOperationCount = trace.dispatchCount = trace.rangeCount = trace.releasedRangeCount = 0;
    trace.residentAttemptHighWater = trace.residentRangeHighWater = 0;
    std::string json;
    result |= check(ValidatePerformanceReceipt(receipt), "closed all-serial control accounting remains useful diagnostics");
    result |= check(SerializePerformanceReceipt(receipt, json) &&
        json.find("\"windowCount\":2") != std::string::npos &&
        json.find("\"totalNanoseconds\":180") != std::string::npos &&
        json.find("\"windowBoundaryCount\":38") != std::string::npos &&
        json.find("\"completedWindowCount\":1") != std::string::npos &&
        json.find("\"controlWindowCount\":2") != std::string::npos,
        "receipt projects actual control rows and binds the three source window counts");
    // This exact subtree is independently literal. Looking only for total180
    // elsewhere would miss an empty fabricated control row or a dropped scope.
    const char *expectedControl =
        "\"controlAccounting\":{\"windowCount\":2,\"firstSampleOrdinal\":1,\"lastSampleOrdinal\":2,"
        "\"totalNanoseconds\":180,\"maximumNanoseconds\":100,\"unscopedSerialNanoseconds\":70,\"phases\":["
        "{\n\"name\":\"owner-intake\",\"available\":true,\"totalNanoseconds\":40,\"maximumNanoseconds\":30,\"sampleCount\":2,\"serialNanoseconds\":40,\"serialNanosecondsKnown\":true,\"pureNanoseconds\":0,\"pureNanosecondsKnown\":true\n},"
        "{\n\"name\":\"legacy-mutable-island\",\"available\":true,\"totalNanoseconds\":20,\"maximumNanoseconds\":10,\"sampleCount\":2,\"serialNanoseconds\":20,\"serialNanosecondsKnown\":true,\"pureNanoseconds\":0,\"pureNanosecondsKnown\":true\n},"
        "{\n\"name\":\"spatial-work\",\"available\":true,\"totalNanoseconds\":20,\"maximumNanoseconds\":10,\"sampleCount\":2,\"serialNanoseconds\":20,\"serialNanosecondsKnown\":true,\"pureNanoseconds\":0,\"pureNanosecondsKnown\":true\n},"
        "{\n\"name\":\"owner-tail\",\"available\":true,\"totalNanoseconds\":20,\"maximumNanoseconds\":10,\"sampleCount\":2,\"serialNanoseconds\":20,\"serialNanosecondsKnown\":true,\"pureNanoseconds\":0,\"pureNanosecondsKnown\":true\n},"
        "{\n\"name\":\"verification-publication\",\"available\":true,\"totalNanoseconds\":10,\"maximumNanoseconds\":5,\"sampleCount\":2,\"serialNanoseconds\":10,\"serialNanosecondsKnown\":true,\"pureNanoseconds\":0,\"pureNanosecondsKnown\":true\n}]}";
    result |= check(json.find(expectedControl) != std::string::npos,
        "receipt preserves all five real control rows and their exact ordinal scope");
    const char *mutationNames[] = {
        "control total must reconcile with its own rows and unscoped time",
        "each control row covers both actual control samples",
        "control scope has a positive first sample ordinal",
        "two control samples cannot share one ordinal",
        "source completed-window coverage matches the real completed workload",
        "source control count matches every observed control sample",
        "a requested whole-window trace cannot omit boundary coverage",
        "control maximum cannot exceed control total",
        "control row serial plus pure equals its inclusive total",
        "control row pure work cannot be silently fabricated",
        "control unscoped time remains part of control accounting",
        "a nonempty control extent cannot be relabelled absent",
        "control count cannot outgrow its actual rows and ordinal range",
        "control and world sums must reject unsigned overflow"
    };
    for (unsigned mutation = 0; mutation != 14; ++mutation)
    {
        PerformanceReceipt invalid = receipt;
        if (mutation == 0) invalid.kernelTiming.phaseAccounting.controlNanoseconds--;
        if (mutation == 1) invalid.kernelTiming.phaseAccounting.controlPhases[0].samples--;
        if (mutation == 2) invalid.kernelTiming.phaseAccounting.firstControlSampleOrdinal = 0;
        if (mutation == 3) invalid.kernelTiming.phaseAccounting.lastControlSampleOrdinal = 1;
        if (mutation == 4) invalid.kernelReference.trace.completedWindowCount = 0;
        if (mutation == 5) invalid.kernelReference.trace.controlWindowCount = 1;
        if (mutation == 6) invalid.kernelReference.trace.windowBoundaryCount = 0;
        if (mutation == 7) invalid.kernelTiming.phaseAccounting.maximumControlNanoseconds = 181;
        if (mutation == 8) invalid.kernelTiming.phaseAccounting.controlPhases[0].serialNanoseconds--;
        if (mutation == 9) invalid.kernelTiming.phaseAccounting.controlPhases[0].pureNanoseconds = 1;
        if (mutation == 10) invalid.kernelTiming.phaseAccounting.controlUnscopedSerialNanoseconds--;
        if (mutation == 11) invalid.kernelTiming.phaseAccounting.controlWindowCount = 0;
        if (mutation == 12) invalid.kernelTiming.phaseAccounting.controlWindowCount = 3;
        if (mutation == 13) invalid.kernelTiming.phaseAccounting.controlNanoseconds = ~static_cast<JobMetricCounter>(0);
        result |= check(!ValidatePerformanceReceipt(invalid), mutationNames[mutation]);
    }
    return result;
}

// Reusable source fixture for the later parser partition. Its trace bytes come
// from A's actual ordinary RECORD ledger, not a literal trace or cloned parser.
// Metadata outside this generated trace remains test-only enclosing receipt
// input, not an installed-native process/topology or selected-profile claim.
bool makeActualWorldTraceSourceReceipt(rts::performance::PerformanceReceipt &receipt,
    std::vector<unsigned char> &traceBytes)
{
    using namespace rts::performance;
    receipt = makeCompleteReceipt();
    receipt.schemaVersion = 6;
    receipt.producer = "game-executable-stage5-performance-report-v6";
    receipt.producerVersion = "6";
    receipt.runId = "R-1";
    receipt.runNonce = "01234567-89ab-4cde-8fab-0123456789ab";
    receipt.processId = 0x1234abcd;
    receipt.processCreationTimeUtc100ns = 0x0102030405060708ULL;
    receipt.processStartTimeUtc100ns = receipt.processCreationTimeUtc100ns + 1;
    receipt.processEndTimeUtc100ns = receipt.processCreationTimeUtc100ns + 2;
    receipt.receiptPath = "H:\\evidence\\performance-receipt-R-1-305441741.json";
    receipt.traceFiles.tracePath = "H:\\evidence\\attempt-trace.bin";
    receipt.frameStart = 0; receipt.frameEnd = receipt.finalFrame = 1;
    receipt.workload.sampleCount = 1; receipt.workload.firstFrame = receipt.workload.lastFrame = 1;
    receipt.frameSimulationSampleCount = 1;
    receipt.frameSimulationMaximumNanoseconds = receipt.frameSimulationTotalNanoseconds;
    receipt.rawEvidence.timingFrameSamples = 2;
    receipt.rawEvidence.timingFirstFrame = 0; receipt.rawEvidence.timingLastFrame = 1;
    rts_test::NativeKernelTrace artifact(73);
    artifact.options.trace.binding.nativeRunIdentity = GetPerformanceReceiptRunIdentity(receipt);
    artifact.options.trace.binding.executable = phaseWireDigest(0x66);
    artifact.options.trace.binding.fixture = phaseWireDigest(0x77);
    // Explicit core-fixture policy/capacities, not D's selected native0x5004 policy.
    artifact.options.trace.binding.sourcePolicy = phaseWireDigest(0x88);
    receipt.executableSha256 = phaseWireSha256(artifact.options.trace.binding.executable);
    receipt.fixtureContentSha256 = phaseWireSha256(artifact.options.trace.binding.fixture);
    ReceiptWindowClock clock = {100, 0};
    artifact.options.clock = ReceiptWindowClock::read;
    artifact.options.clockContext = &clock;
    KernelPerformanceReferenceLedger source;
    if (!source.beginRun(artifact.options)) return false;
    KernelPerformanceWindowBoundary boundary;
    boundary.kind = KERNEL_WINDOW_BEGIN;
    boundary.sampleOrdinal = 1;
    boundary.phase = KERNEL_PHASE_COUNT;
    boundary.ownerFrameAtEntry = boundary.authorityFrame = boundary.actualOwnerFrame = 0;
    if (!source.observeWindowBoundary(boundary)) return false;
    for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
    {
        boundary.phase = static_cast<KernelPerformancePhase>(index);
        boundary.kind = KERNEL_WINDOW_PHASE_BEGIN;
        boundary.actualOwnerFrame = 0;
        if (!source.observeWindowBoundary(boundary)) return false;
        boundary.kind = KERNEL_WINDOW_PHASE_END;
        boundary.actualOwnerFrame = index + 1 == KERNEL_PHASE_COUNT ? 1 : 0;
        if (!source.observeWindowBoundary(boundary)) return false;
    }
    boundary.kind = KERNEL_WINDOW_WORLD_END;
    boundary.phase = KERNEL_PHASE_COUNT;
    boundary.actualOwnerFrame = 1;
    if (!source.observeWindowBoundary(boundary) || !source.sealObservationWindow() ||
        !source.sealExecutionClosure()) return false;
    receipt.kernelReference = source.freeze();
    traceBytes = artifact.bytes;
    const KernelPerformanceTraceSnapshot &trace = receipt.kernelReference.trace;
    // 399-byte header, twelve80-byte boundaries, two44-byte seals,359-byte footer.
    return clock.reads == 0 && !receipt.kernelReference.complete && receipt.kernelReference.streamCount == 0 &&
        trace.requested && trace.frozen && trace.complete && trace.errors == 0 &&
        trace.observationSealed && trace.executionSealed && trace.digest.valid &&
        trace.recordCount == 16 && trace.logicalEventCount == 16 &&
        trace.windowBoundaryCount == 12 && trace.completedWindowCount == 1 && trace.controlWindowCount == 0 &&
        trace.attemptCount == 0 && trace.reapCount == 0 && trace.rangeCount == 0 &&
        trace.byteCount == 1806 && traceBytes.size() == 1806;
}

int testActualWorldTraceReceiptProjection()
{
    using namespace rts::performance;
    PerformanceReceipt source;
    std::vector<unsigned char> traceBytes;
    int result = check(makeActualWorldTraceSourceReceipt(source, traceBytes),
        "source projection starts from the actual complete16-record no-attempt world artifact without a clock");
    if (result != 0) return result; // A/identity prerequisite, not E behavioral RED.
    result |= check(ValidatePerformanceReceipt(source),
        "an actual whole-world RECORD snapshot is valid enclosing V6 diagnostic evidence");
    std::string json;
    result |= check(SerializePerformanceReceipt(source, json) &&
        json.find("\"windowBoundaryCount\":12") != std::string::npos &&
        json.find("\"completedWindowCount\":1") != std::string::npos &&
        json.find("\"controlWindowCount\":0") != std::string::npos &&
        json.find("\"byteCount\":1806") != std::string::npos &&
        json.find("\"phaseAccounting\":null") != std::string::npos &&
        json.find("\"mode\":\"record\"") != std::string::npos,
        "the real writer projects all actual source window counts without a test patch or fabricated phase baseline");
    for (unsigned mutation = 0; mutation != 3; ++mutation)
    {
        PerformanceReceipt invalid = source;
        if (mutation == 0) invalid.kernelReference.trace.completedWindowCount = 0;
        if (mutation == 1) invalid.kernelReference.trace.completedWindowCount = 2;
        if (mutation == 2) invalid.kernelReference.trace.windowBoundaryCount = 0;
        result |= check(!ValidatePerformanceReceipt(invalid),
            "actual source window coverage cannot be omitted or disagree with its completed workload");
    }
    return result;
}

// Independently authored complete V6 source wire fixture.
// Not produced by SerializePerformanceReceipt, and not a claim that the
// literal trace hash/metadata describes an actual native binary artifact.
// The separate makeActualWorldTraceSourceReceipt fixture supplies that proof.
// Root ordering intentionally differs from the production writer. Nonempty
// timing/reference streams keep their complete nested shapes under test.
static const char sourceReaderLiteral[] = R"receipt({
"attemptTrace":{"schemaVersion":1,"encoding":"typed-canonical-le-v1","fieldSchema":20481,"mode":"record","frozen":true,"complete":true,"errors":0,"observationIngressSealed":true,"executionClosureSealed":true,
"file":{"path":"H:\\evidence\\attempt-trace.bin","sha256":"4444444444444444444444444444444444444444444444444444444444444444","byteCount":3000},
"binding":{"nativeRunIdentitySha256":"F3EA40012CF21979C50B110D0FF67B728176A44DB652DC6A2247AC6340159267","executableSha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","fixtureSha256":"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB","sourcePolicySha256":"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"},
"limits":{"maximumBytes":1048576,"maximumRecords":100000,"maximumLogicalEvents":100000,"maximumAttempts":10000,"maximumRanges":500000},
"residentAttemptCapacity":15,"residentRangeCapacity":340,"residentAttemptCount":0,"residentAttemptHighWater":1,"residentRangeCount":0,"residentRangeHighWater":2,
"recordCount":26,"logicalEventCount":26,"coalescedSpanCount":0,"coalescedAttemptCount":0,"attemptCount":1,"admittedAttemptCount":1,"notAdmittedAttemptCount":0,"abortedAfterAdmissionAttemptCount":0,"reapCount":1,"capturedAttemptCount":1,"capturedOperationCount":2,"dispatchCount":1,"rangeCount":2,"releasedRangeCount":2,
"windowBoundaryCount":12,"completedWindowCount":1,"controlWindowCount":0,"sourceBinding":null},
"phaseAccounting":null,
"kernelReference":{"schemaVersion":1,"mode":"throughput-binding","frozen":true,"complete":true,"errors":0,"generation":1,"streams":[{"name":"path","subtype":0,"fieldSchema":1,"firstFrame":0,"lastFrame":0,"validatedBatchCount":1,"committedBatchCount":1,"abortedBatchCount":0,"validatedOperationCount":2,"committedOperationCount":2,"serialSampleCount":0,"serialNanoseconds":0,"maximumSerialNanoseconds":0,"inputSha256":"1111111111111111111111111111111111111111111111111111111111111111","outputSha256":"2222222222222222222222222222222222222222222222222222222222222222","commitSha256":"3333333333333333333333333333333333333333333333333333333333333333"}]},
"kernelTiming":{"schemaVersion":1,"mode":"owner-pipeline-observation","attribution":"owner-stack-exclusive-v1","enabled":true,"frozen":true,"complete":true,"errors":0,"generation":1,"serialReferenceKnown":false,"streams":[{"name":"path","subtype":0,"attemptedBatches":1,"admittedBatches":1,"committedBatches":1,"abortedBatches":0,"firstFrame":0,"lastFrame":0,"activePipelineNanoseconds":5,"inclusiveBatchNanoseconds":6,"maximumBatchNanoseconds":6,"stages":[{"name":"capture","totalNanoseconds":1,"sampleCount":1},{"name":"schedule","totalNanoseconds":1,"sampleCount":1},{"name":"wait","totalNanoseconds":1,"sampleCount":1},{"name":"validate","totalNanoseconds":1,"sampleCount":1},{"name":"commit","totalNanoseconds":1,"sampleCount":1}]}]},
"phases":[
{"name":"owner-intake","available":true,"totalNanoseconds":1000,"maximumNanoseconds":1000,"sampleCount":1,"serialNanoseconds":0,"serialNanosecondsKnown":false,"pureNanoseconds":0,"pureNanosecondsKnown":false},
{"name":"legacy-mutable-island","available":true,"totalNanoseconds":1001,"maximumNanoseconds":1001,"sampleCount":1,"serialNanoseconds":0,"serialNanosecondsKnown":false,"pureNanoseconds":0,"pureNanosecondsKnown":false},
{"name":"spatial-work","available":true,"totalNanoseconds":1002,"maximumNanoseconds":1002,"sampleCount":1,"serialNanoseconds":0,"serialNanosecondsKnown":false,"pureNanoseconds":0,"pureNanosecondsKnown":false},
{"name":"owner-tail","available":true,"totalNanoseconds":1003,"maximumNanoseconds":1003,"sampleCount":1,"serialNanoseconds":0,"serialNanosecondsKnown":false,"pureNanoseconds":0,"pureNanosecondsKnown":false},
{"name":"verification-publication","available":true,"totalNanoseconds":1004,"maximumNanoseconds":1004,"sampleCount":1,"serialNanoseconds":0,"serialNanosecondsKnown":false,"pureNanoseconds":0,"pureNanosecondsKnown":false}],
"kernels":[
{"name":"physics","available":true,"submittedJobs":8,"completedJobs":8,"physicalWorkerJobs":8,"ownerHelpedJobs":0,"physicalWorkerMask":255,"distinctPhysicalWorkers":8,"physicalWorkerMaskComplete":true,"elapsedNanoseconds":2000,"elapsedNanosecondsKnown":true},
{"name":"status","available":true,"submittedJobs":8,"completedJobs":8,"physicalWorkerJobs":8,"ownerHelpedJobs":0,"physicalWorkerMask":255,"distinctPhysicalWorkers":8,"physicalWorkerMaskComplete":true,"elapsedNanoseconds":2001,"elapsedNanosecondsKnown":true},
{"name":"collision","available":true,"submittedJobs":8,"completedJobs":8,"physicalWorkerJobs":8,"ownerHelpedJobs":0,"physicalWorkerMask":255,"distinctPhysicalWorkers":8,"physicalWorkerMaskComplete":true,"elapsedNanoseconds":2002,"elapsedNanosecondsKnown":true},
{"name":"ai-planning","available":true,"submittedJobs":8,"completedJobs":8,"physicalWorkerJobs":8,"ownerHelpedJobs":0,"physicalWorkerMask":255,"distinctPhysicalWorkers":8,"physicalWorkerMaskComplete":true,"elapsedNanoseconds":2003,"elapsedNanosecondsKnown":true},
{"name":"spatial","available":true,"submittedJobs":8,"completedJobs":8,"physicalWorkerJobs":8,"ownerHelpedJobs":0,"physicalWorkerMask":255,"distinctPhysicalWorkers":8,"physicalWorkerMaskComplete":true,"elapsedNanoseconds":2004,"elapsedNanosecondsKnown":true},
{"name":"path","available":true,"submittedJobs":8,"completedJobs":8,"physicalWorkerJobs":8,"ownerHelpedJobs":0,"physicalWorkerMask":255,"distinctPhysicalWorkers":8,"physicalWorkerMaskComplete":true,"elapsedNanoseconds":2005,"elapsedNanosecondsKnown":true}],
"schedulerMetrics":{"submittedJobCount":0,"executedJobCount":0,"stealCount":0,"ownerHelpCount":0,"waitCount":0,"workerWaitRejectionCount":0,"failedJobCount":0,"cancelledJobCount":0,"serialFallbackCount":0,"totalQueueLatencyNanoseconds":0,"maximumQueueLatencyNanoseconds":0,"workerBusyNanoseconds":0,"workerWaitNanoseconds":0,"affinityFailureCount":0,"injectionHighWater":0,"maximumActiveWorkers":0,"availableLogicalCpuCount":0,"reservedOwnerCpuCount":0,"selectedWorkerCpuCount":0,"selectedWorkerPhysicalCoreCount":0,"selectedWorkerPhysicalCoreMask":0,"selectedWorkerPhysicalCoreMaskComplete":false},
"provenance":{"kind":"native-executable-observation","receiptPath":"H:\\evidence\\performance-receipt-R-1-305441741.json","processId":305441741,"processCreationUtc":"1831-02-20T09:26:19.0382856Z","executablePath":"H:\\installed\\generals.exe","executableSha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","commandLine":"game.exe -headless","exitCode":0},
"rawLogs":[{"name":"raw-log","path":"H:\\evidence\\run.log","sha256":"5555555555555555555555555555555555555555555555555555555555555555"},{"name":"timing","path":"H:\\evidence\\timing.csv","sha256":"6666666666666666666666666666666666666666666666666666666666666666"}],
"rawEvidence":{"verifierBoundary":"game-receipt-before-host-log-close","rawLogPath":"H:\\evidence\\run.log","rawLogSha256":"5555555555555555555555555555555555555555555555555555555555555555","timingPath":"H:\\evidence\\timing.csv","timingSha256":"6666666666666666666666666666666666666666666666666666666666666666","timingClosed":true,"timingWriteSucceeded":true,"timingTruncated":false,"timingComplete":true,"timingSessionCount":1,"timingFrameSamples":2,"timingFirstFrame":0,"timingLastFrame":1},
"topology":{"source":"GetSystemCpuSetInformation","cpuSets":[
{"id":10,"efficiencyClass":0,"group":0,"coreIndex":0,"logicalProcessorIndex":0,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":100,"efficiencyClass":0,"group":0,"coreIndex":1,"logicalProcessorIndex":1,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":101,"efficiencyClass":0,"group":0,"coreIndex":2,"logicalProcessorIndex":2,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":102,"efficiencyClass":0,"group":0,"coreIndex":3,"logicalProcessorIndex":3,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":103,"efficiencyClass":0,"group":0,"coreIndex":4,"logicalProcessorIndex":4,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":104,"efficiencyClass":0,"group":0,"coreIndex":5,"logicalProcessorIndex":5,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":105,"efficiencyClass":0,"group":0,"coreIndex":6,"logicalProcessorIndex":6,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":106,"efficiencyClass":0,"group":0,"coreIndex":7,"logicalProcessorIndex":7,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true},
{"id":107,"efficiencyClass":0,"group":0,"coreIndex":8,"logicalProcessorIndex":8,"parked":false,"allocatedToOtherProcess":false,"availableToProcess":true}],"ownerCpuSetIds":[10],"selectedWorkerCpuSetIds":[100,101,102,103,104,105,106,107]},
"worker":{"requestedCount":8,"effectiveCount":8,"policy":"auto","pinned":true,"availableLogicalCpuCount":16,"reservedOwnerCpuCount":1,"selectedWorkerCpuCount":8,"selectedWorkerPhysicalCoreCount":8,"selectedWorkerPhysicalCoreMask":255,"selectedWorkerPhysicalCoreMaskComplete":true},
"frames":{"start":0,"end":1,"final":1,"finalCrcKnown":true,"finalCrc":305441741},
"frameSimulation":{"totalNanoseconds":6000,"maximumNanoseconds":6000,"sampleCount":1},
"workload":{"sampling":"completed-simulation-frame-boundary-v1","sampleCount":1,"firstFrame":1,"lastFrame":1,"playerCount":8,"rosterStable":true,"contiguous":true,"initialUnitCount":8000,"minimumUnitCount":7900,"peakUnitCount":8200},
"fixture":{"id":"dense-8-player","kind":"replay","workloadQualification":"minimum-qualified","contentPath":"Stage5Scaling\\dense-8-player.rep","identityObserved":true,"contentSha256":"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB","replayPath":"Stage5Scaling\\dense-8-player.rep","retainedReplayPath":"","retainedReplaySha256":"","seed":49374,"seedKnown":true,"requestedPlayerCount":8,"requestedMinimumUnitCount":8000},
"process":{"id":305441741,"creationTimeUtc100ns":72623859790382856,"startTimeUtc100ns":72623859790382857,"endTimeUtc100ns":72623859790382858,"identityAvailable":true,"exitCodeKnown":true,"exitCode":0,"exitBoundary":"ReplaySimulation::simulateReplaysInThisProcess:return"},
"commandLine":"game.exe -headless",
"executableSha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
"executablePath":"H:\\installed\\generals.exe",
"runtimeClosure":{"dependencyManifestSha256":"7777777777777777777777777777777777777777777777777777777777777777","closureSha256":"8888888888888888888888888888888888888888888888888888888888888888"},
"artifactSetSha256":"9999999999999999999999999999999999999999999999999999999999999999",
"sourceCommit":"0123456789abcdef0123456789abcdef01234567",
"architecture":"x64",
"recordedUtc":"1831-02-20T09:27:00.000Z",
"cohortCreatedUtc":"1831-02-20T09:00:00.000Z",
"cohortNonce":"22222222-2222-4222-8222-222222222222",
"runNonce":"01234567-89ab-4cde-8fab-0123456789ab",
"runId":"R-1",
"title":"Generals",
"producerVersion":"6",
"schedulerStarted":true,
"simulationMode":"parallel",
"measurementRole":"throughput",
"role":"performance-report",
"status":"passed",
"evidenceKind":"stage5-executable-originated-receipt",
"producer":"game-executable-stage5-performance-report-v6",
"schemaVersion":6
})receipt";

bool sourceReaderParseBytes(const std::string &bytes,
    rts::performance::PerformanceReceipt &source, std::string *reason = 0)
{
    return rts::performance::ParsePerformanceReceiptSource(
        reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), source, reason);
}

bool sourceReaderReplace(std::string &input, const std::string &before,
    const std::string &after, unsigned expectedCount = 1)
{
    // Test-input mutation only, not a parser or an expected-value constructor.
    unsigned count = 0;
    for (std::size_t at = input.find(before); at != std::string::npos;
        at = input.find(before, at + after.size()))
    {
        input.replace(at, before.size(), after);
        ++count;
    }
    return count == expectedCount;
}

int sourceReaderRejectsAndClears(const std::string &bytes,
    const rts::performance::PerformanceReceipt &previous, const char *name)
{
    using namespace rts::performance;
    PerformanceReceipt output = previous;
    std::string reason;
    const bool accepted = sourceReaderParseBytes(bytes, output, &reason);
    return check(!accepted && output.status != "passed" && output.runId.empty() &&
        output.runNonce.empty() && output.receiptPath.empty() && output.processId == 0 &&
        output.processCreationTimeUtc100ns == 0 && output.traceFiles.tracePath.empty() &&
        output.traceFiles.sourceReceiptPath.empty() && !output.kernelReference.trace.requested &&
        !output.kernelReference.trace.complete && !output.kernelReference.trace.binding.nativeRunIdentity.valid &&
        output.kernelReference.streamCount == 0 && output.fixtureId.empty() && output.executablePath.empty() &&
        output.rawEvidence.rawLogPath.empty() && output.cpuSets.empty() && output.phases.empty() &&
        output.kernels.empty() && output.workload.sampleCount == 0 && !reason.empty() && reason.size() <= 512, name);
}

int testSourceReaderActualWriterBytes()
{
    using namespace rts::performance;
    PerformanceReceipt source;
    std::vector<unsigned char> trace;
    std::string document;
    int result = check(makeActualWorldTraceSourceReceipt(source, trace) &&
        SerializePerformanceReceipt(source, document) &&
        document.find("\"windowBoundaryCount\":12") != std::string::npos &&
        document.find("\"completedWindowCount\":1") != std::string::npos &&
        document.find("\"controlWindowCount\":0") != std::string::npos,
        "reader prerequisite is real A source plus real complete V6 writer output with no JSON patch");
    if (result != 0) return result;
    PerformanceReceipt parsed;
    result |= check(sourceReaderParseBytes(document, parsed),
        "real reader accepts the actual writer's complete16-record source receipt");
    result |= check(parsed.runId == source.runId && parsed.runNonce == source.runNonce &&
        parsed.processId == source.processId &&
        parsed.processCreationTimeUtc100ns == 0x0102030405060708ULL &&
        parsed.processStartTimeUtc100ns == 0x0102030405060709ULL &&
        parsed.processEndTimeUtc100ns == 0x010203040506070aULL &&
        parsed.receiptPath == source.receiptPath && parsed.traceFiles.tracePath == source.traceFiles.tracePath &&
        parsed.kernelReference.trace.digest.equals(source.kernelReference.trace.digest) &&
        parsed.kernelReference.trace.binding.nativeRunIdentity.equals(GetPerformanceReceiptRunIdentity(source)) &&
        parsed.kernelReference.trace.byteCount == 1806 && parsed.kernelReference.trace.recordCount == 16 &&
        parsed.kernelReference.trace.windowBoundaryCount == 12 && parsed.kernelReference.trace.completedWindowCount == 1 &&
        parsed.kernelReference.trace.controlWindowCount == 0 && parsed.kernelReference.trace.attemptCount == 0 &&
        parsed.kernelReference.streamCount == 0 && !parsed.kernelReference.complete &&
        parsed.rawEvidence.rawLogPath == source.rawEvidence.rawLogPath &&
        parsed.rawEvidence.timingSha256 == source.rawEvidence.timingSha256 && parsed.phases.size() == 5 &&
        !parsed.phases[0].serialNanosecondsKnown && !parsed.phases[0].pureNanosecondsKnown,
        "source parse retains exact identity artifact coverage raw closure and unknown phase attribution");
    std::string guarded = document + std::string("\0{}", 3);
    result |= check(ParsePerformanceReceiptSource(reinterpret_cast<const unsigned char *>(guarded.data()),
        document.size(), parsed) && parsed.runId == source.runId,
        "reader uses the exact nonterminated input extent and ignores bytes beyond byteCount");
    result |= sourceReaderRejectsAndClears(guarded, source,
        "the same trailing NUL and root fail when inside the declared extent");
    result |= check(sourceReaderParseBytes(document, parsed), "reader can recover after a failed independent parse");
    const std::string copiedPath = parsed.traceFiles.tracePath;
    document.assign(document.size(), 'x');
    result |= check(!copiedPath.empty() && parsed.traceFiles.tracePath == copiedPath,
        "successful parse owns its strings rather than retaining a mutable input pointer");
    return result;
}

int testSourceReaderIndependentLiteralAndBounds()
{
    using namespace rts::performance;
    using rts::JobMetricCounter;
    const std::string literal(sourceReaderLiteral);
    PerformanceReceipt parsed;
    int result = check(sourceReaderParseBytes(literal, parsed),
        "reader accepts independently authored reordered full V6 JSON with populated timing and reference streams");
    result |= check(parsed.schemaVersion == 6 && parsed.producerVersion == "6" && parsed.runId == "R-1" &&
        parsed.processCreationTimeUtc100ns == 72623859790382856ULL && parsed.processId == 305441741 &&
        parsed.kernelTiming.streamCount == 1 && parsed.kernelTiming.streams[0].kernel == KERNEL_PERFORMANCE_PATH &&
        parsed.kernelTiming.streams[0].subtype == 0 && parsed.kernelTiming.streams[0].stageSamples[KERNEL_PERFORMANCE_WAIT] == 1 &&
        parsed.kernelReference.streamCount == 1 && parsed.kernelReference.streams[0].fieldSchema == 1 &&
        parsed.kernelReference.streams[0].validatedOperationCount == 2 &&
        parsed.kernelReference.streams[0].serialSampleCount == 0 && parsed.kernelReference.trace.rangeCount == 2 &&
        parsed.kernelReference.trace.sourceReceiptDigest.valid == false && parsed.traceFiles.sourceRunId.empty() &&
        parsed.workload.sampleCount == 1 && parsed.cpuSets.size() == 9 && parsed.ownerCpuSetIds.size() == 1 &&
        parsed.selectedWorkerCpuSetIds.size() == 8 && parsed.kernels.size() == 6,
        "independent JSON maps complete nested typed streams topology and source-only null binding");
    const PerformanceReceipt previous = parsed;
    std::string unicode = literal;
    result |= check(sourceReaderReplace(unicode, "game.exe -headless", "game.exe caf\\u00e9 \\ud83d\\ude80", 2) &&
        sourceReaderReplace(unicode, "\"runId\":", "\"run\\u0049d\":"),
        "valid Unicode fixture changes both internally duplicated command lines and one decoded key");
    result |= check(sourceReaderParseBytes(unicode, parsed) &&
        parsed.commandLine == std::string("game.exe caf\xc3\xa9 \xf0\x9f\x9a\x80") && parsed.runId == "R-1",
        "reader decodes escaped BMP supplementary Unicode and an escaped key without ANSI substitution");
    std::string directUtf8 = literal;
    result |= check(sourceReaderReplace(directUtf8, "game.exe -headless",
        std::string("game.exe caf\xc3\xa9 \xf0\x9f\x9a\x80"), 2), "direct UTF8 fixture is explicit");
    result |= check(sourceReaderParseBytes(directUtf8, parsed) &&
        parsed.commandLine == std::string("game.exe caf\xc3\xa9 \xf0\x9f\x9a\x80"),
        "reader accepts the equivalent shortest-form UTF8 bytes independently of native path support");
    std::string maximum = literal;
    result |= check(sourceReaderReplace(maximum, "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":18446744073709551615"),
        "maximum UInt64 fixture changes one diagnostic counter");
    result |= check(sourceReaderParseBytes(maximum, parsed) && parsed.schedulerMetrics.workerBusyNanoseconds == ~static_cast<JobMetricCounter>(0),
        "reader retains a full-width unsigned diagnostic counter without a floating intermediate");
    std::string stringLimit = literal;
    const std::string command(131072, 'x');
    result |= check(sourceReaderReplace(stringLimit, "game.exe -headless", command, 2), "decoded string cap fixture is explicit");
    result |= check(sourceReaderParseBytes(stringLimit, parsed) && parsed.commandLine.size() == 131072,
        "reader accepts the exact128KiB decoded-string boundary");
    std::string tooLongString = literal;
    result |= check(sourceReaderReplace(tooLongString, "game.exe -headless", std::string(131073, 'x'), 2),
        "over-cap string fixture is explicit");
    result |= sourceReaderRejectsAndClears(tooLongString, previous, "reader rejects one decoded byte above128KiB");
    std::string fileLimit = literal;
    fileLimit.append(4194304 - fileLimit.size(), ' ');
    result |= check(sourceReaderParseBytes(fileLimit, parsed) && parsed.runId == "R-1",
        "reader accepts the exact4MiB byte extent including trailing JSON whitespace");
    fileLimit.push_back(' ');
    result |= sourceReaderRejectsAndClears(fileLimit, previous, "reader rejects one byte above the fixed4MiB receipt cap");
    return result;
}

int testSourceReaderClosedShapeMutations()
{
    using namespace rts::performance;
    const std::string literal(sourceReaderLiteral);
    // Seed the output with a passed, nonempty source independently of whether
    // the positive parser is implemented yet. Every failure must clear it.
    PerformanceReceipt previous = makeV6MixedPhaseWireFixture();
    struct Mutation { const char *name, *before, *after; };
    const Mutation mutations[] = {
        {"duplicate root key", "\"schemaVersion\":6", "\"schemaVersion\":6,\"schemaVersion\":6"},
        {"escaped duplicate root key", "\"runId\":\"R-1\"", "\"runId\":\"R-1\",\"run\\u0049d\":\"R-1\""},
        {"unknown root key after a valid trace", "\"schemaVersion\":6", "\"schemaVersion\":6,\"unknown\":0"},
        {"missing nullable field is not absence", "\"phaseAccounting\":null,", ""},
        {"duplicate closure key", "\"runtimeClosure\":{", "\"runtimeClosure\":{\"closureSha256\":\"x\","},
        {"duplicate process key", "\"process\":{", "\"process\":{\"id\":305441741,"},
        {"duplicate fixture key", "\"fixture\":{", "\"fixture\":{\"seed\":49374,"},
        {"duplicate workload key", "\"workload\":{", "\"workload\":{\"sampleCount\":1,"},
        {"duplicate frame timing key", "\"frameSimulation\":{", "\"frameSimulation\":{\"sampleCount\":1,"},
        {"duplicate frame result key", "\"frames\":{", "\"frames\":{\"final\":1,"},
        {"duplicate worker key", "\"worker\":{", "\"worker\":{\"requestedCount\":8,"},
        {"duplicate topology key", "\"topology\":{", "\"topology\":{\"ownerCpuSetIds\":[10],"},
        {"duplicate CPU-set row key", "{\"id\":10,", "{\"id\":10,\"id\":10,"},
        {"duplicate raw evidence key", "\"rawEvidence\":{", "\"rawEvidence\":{\"timingClosed\":true,"},
        {"duplicate raw log row key", "{\"name\":\"raw-log\",", "{\"name\":\"raw-log\",\"name\":\"raw-log\","},
        {"duplicate provenance key", "\"provenance\":{", "\"provenance\":{\"processId\":305441741,"},
        {"duplicate otherwise ignored scheduler key", "\"schedulerMetrics\":{", "\"schedulerMetrics\":{\"stealCount\":0,"},
        {"duplicate phase row key", "{\"name\":\"owner-intake\",", "{\"name\":\"owner-intake\",\"pureNanosecondsKnown\":false,"},
        {"duplicate kernel row key", "{\"name\":\"physics\",", "{\"name\":\"physics\",\"elapsedNanosecondsKnown\":true,"},
        {"duplicate timing envelope key", "\"kernelTiming\":{", "\"kernelTiming\":{\"enabled\":true,"},
        {"duplicate reference envelope key", "\"kernelReference\":{", "\"kernelReference\":{\"frozen\":true,"},
        {"duplicate timing stream key", "\"attemptedBatches\":1", "\"attemptedBatches\":1,\"attemptedBatches\":1"},
        {"duplicate timing stage key", "{\"name\":\"capture\",", "{\"name\":\"capture\",\"sampleCount\":1,"},
        {"duplicate reference stream key", "\"validatedBatchCount\":1", "\"validatedBatchCount\":1,\"validatedBatchCount\":1"},
        {"duplicate trace envelope key", "\"attemptTrace\":{", "\"attemptTrace\":{\"fieldSchema\":20481,"},
        {"duplicate trace file key", "\"file\":{", "\"file\":{\"byteCount\":3000,"},
        {"duplicate trace binding key", "\"binding\":{", "\"binding\":{\"fixtureSha256\":\"x\","},
        {"duplicate trace limit key", "\"limits\":{", "\"limits\":{\"maximumRecords\":100000,"},
        {"unknown nested metrics key", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":0,\"ignored\":true"},
        {"missing nested metric key", "\"workerBusyNanoseconds\":0,", ""},
        {"malformed ignored diagnostic after valid trace", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":wat"},
        {"quoted version integer", "\"schemaVersion\":6", "\"schemaVersion\":\"6\""},
        {"floating version alias", "\"schemaVersion\":6", "\"schemaVersion\":6.0"},
        {"exponent integer alias", "\"schemaVersion\":6", "\"schemaVersion\":6e0"},
        {"leading-zero integer alias", "\"schemaVersion\":6", "\"schemaVersion\":06"},
        {"positive-sign integer alias", "\"schemaVersion\":6", "\"schemaVersion\":+6"},
        {"negative-zero unsigned alias", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":-0"},
        {"negative unsigned counter", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":-1"},
        {"fractional unsigned counter", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":0.5"},
        {"UInt64 overflow", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":18446744073709551616"},
        {"u32 overflow", "\"requestedCount\":8", "\"requestedCount\":4294967296"},
        {"boolean counter coercion", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":true"},
        {"null counter coercion", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":null"},
        {"nonfinite token", "\"workerBusyNanoseconds\":0", "\"workerBusyNanoseconds\":NaN"},
        {"string boolean coercion", "\"schedulerStarted\":true", "\"schedulerStarted\":\"true\""},
        {"one-tick FILETIME binding mismatch", "\"creationTimeUtc100ns\":72623859790382856", "\"creationTimeUtc100ns\":72623859790382857"},
        {"rounded FILETIME token", "\"creationTimeUtc100ns\":72623859790382856", "\"creationTimeUtc100ns\":7.2623859790382856e16"},
        {"one-tick provenance UTC mismatch", "1831-02-20T09:26:19.0382856Z", "1831-02-20T09:26:19.0382857Z"},
        {"timestamp trailing bytes", "1831-02-20T09:26:19.0382856Z", "1831-02-20T09:26:19.0382856Zjunk"},
        {"impossible timestamp component", "1831-02-20T09:26:19.0382856Z", "1831-02-30T09:26:19.0382856Z"},
        {"source receipt role cannot become baseline", "\"measurementRole\":\"throughput\"", "\"measurementRole\":\"phase-serial-baseline\""},
        {"source receipt status cannot become failed", "\"status\":\"passed\"", "\"status\":\"failed\""},
        {"V5 does not qualify through source reader", "\"schemaVersion\":6", "\"schemaVersion\":5"},
        {"trace consume is not source record", "\"mode\":\"record\"", "\"mode\":\"consume\""},
        {"trace error cannot qualify", "\"errors\":0,\"observationIngressSealed\":true", "\"errors\":2048,\"observationIngressSealed\":true"},
        {"trace incomplete seal", "\"executionClosureSealed\":true", "\"executionClosureSealed\":false"},
        {"source identity digest disagrees with exact tuple", "F3EA40012CF21979C50B110D0FF67B728176A44DB652DC6A2247AC6340159267", "F2EA40012CF21979C50B110D0FF67B728176A44DB652DC6A2247AC6340159267"},
        {"unrepresented wire constant cannot claim known serial reference", "\"serialReferenceKnown\":false", "\"serialReferenceKnown\":true"},
        {"required source binding is exactly null", "\"sourceBinding\":null", "\"sourceBinding\":{}"},
        {"missing source world count", "\"completedWindowCount\":1,", ""},
        {"source world count disagrees with workload", "\"completedWindowCount\":1", "\"completedWindowCount\":2"},
        {"source boundary coverage cannot be zero", "\"windowBoundaryCount\":12", "\"windowBoundaryCount\":0"},
        {"phase baseline metadata is forbidden on source", "\"phaseAccounting\":null", "\"phaseAccounting\":{}"},
        {"raw path duplicate disagrees", "\"rawLogPath\":\"H:\\\\evidence\\\\run.log\"", "\"rawLogPath\":\"H:\\\\evidence\\\\other.log\""},
        {"provenance PID disagrees", "\"processId\":305441741", "\"processId\":305441742"},
        {"embedded identity NUL", "\"runId\":\"R-1\"", "\"runId\":\"R-1\\u0000x\""},
        {"invalid JSON string escape", "\"runId\":\"R-1\"", "\"runId\":\"R-\\q1\""},
        {"missing comma", "\"status\":\"passed\",", "\"status\":\"passed\""},
        {"trailing comma", "\"schemaVersion\":6", "\"schemaVersion\":6,"},
        {"comment token", "\"schemaVersion\":6", "\"schemaVersion\":/*ignored*/6"}
    };
    int result = 0;
    for (unsigned index = 0; index != sizeof(mutations) / sizeof(mutations[0]); ++index)
    {
        std::string invalid = literal;
        const Mutation &mutation = mutations[index];
        const bool changed = sourceReaderReplace(invalid, mutation.before, mutation.after);
        result |= check(changed, "reader mutation must address exactly one independently literal field");
        if (changed) result |= sourceReaderRejectsAndClears(invalid, previous, mutation.name);
    }
    const std::size_t traceStart = literal.find("\"attemptTrace\":");
    const std::size_t traceEnd = literal.find(",\n\"phaseAccounting\":", traceStart);
    const bool traceLocated = traceStart != std::string::npos && traceEnd != std::string::npos && traceEnd > traceStart;
    result |= check(traceLocated, "null and missing trace mutations identify the complete independent literal envelope");
    if (traceLocated)
    {
        std::string nullTrace = literal;
        nullTrace.replace(traceStart, traceEnd - traceStart, "\"attemptTrace\":null");
        result |= sourceReaderRejectsAndClears(nullTrace, previous,
            "an unrequested null trace cannot be selected as a complete source");
        std::string missingTrace = literal;
        missingTrace.erase(traceStart, traceEnd - traceStart + 1);
        result |= sourceReaderRejectsAndClears(missingTrace, previous,
            "a missing trace field cannot downgrade selected V6 source metadata");
    }
    const std::string malformedStrings[] = {
        "\\ud800", "\\udc00", "\\udc00\\ud800", "\\ud800x", "\\u00zz", "\\u0000",
        std::string(1, '\x01'), std::string("\xc0\xaf", 2), std::string("\xe0\x80\xaf", 3),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe2\x82", 2), std::string("\x80", 1), std::string("\xff", 1)
    };
    for (unsigned index = 0; index != sizeof(malformedStrings) / sizeof(malformedStrings[0]); ++index)
    {
        std::string invalid = literal;
        result |= check(sourceReaderReplace(invalid, "game.exe -headless", malformedStrings[index], 2),
            "invalid string fixture changes both command-line representations");
        result |= sourceReaderRejectsAndClears(invalid, previous,
            "malformed UTF8 surrogate escape control or decoded NUL fails even in a nonselection string");
    }
    const std::string invalidDocuments[] = {
        "", "null", "[]", "true", "{}", literal.substr(0, literal.size() - 1),
        literal + "{}", literal + "x", literal + std::string(1, '\0'),
        std::string("\xef\xbb\xbf", 3) + literal
    };
    for (unsigned index = 0; index != sizeof(invalidDocuments) / sizeof(invalidDocuments[0]); ++index)
        result |= sourceReaderRejectsAndClears(invalidDocuments[index], previous,
            "reader requires one full object with exact EOF and no BOM or embedded NUL");
    PerformanceReceipt cleared = previous;
    result |= check(!ParsePerformanceReceiptSource(0, 1, cleared) && cleared.runId.empty() &&
        !cleared.kernelReference.trace.requested, "null byte pointer fails closed before dereference");
    cleared = previous;
    result |= check(!ParsePerformanceReceiptSource(0, 0, cleared) && cleared.runId.empty(),
        "empty null input clears any prior successful output");
    return result;
}

int testSourceReaderClosedResourceExtents()
{
    using namespace rts::performance;
    const std::string literal(sourceReaderLiteral);
    const PerformanceReceipt previous = makeV6MixedPhaseWireFixture();
    PerformanceReceipt parsed;
    int result = 0;
    std::string rows;
    for (unsigned index = 0; index != 4087; ++index)
        rows += ",{\"id\":" + std::to_string(1000 + index) +
            ",\"efficiencyClass\":0,\"group\":" + std::to_string(1 + index / 64) +
            ",\"coreIndex\":" + std::to_string(index % 64) +
            ",\"logicalProcessorIndex\":" + std::to_string(index % 64) +
            ",\"parked\":false,\"allocatedToOtherProcess\":false,\"availableToProcess\":false}";
    const std::string arrayEnd = "],\"ownerCpuSetIds\":[10]";
    std::string topologyLimit = literal;
    result |= check(sourceReaderReplace(topologyLimit, arrayEnd, rows + arrayEnd),
        "topology cap fixture appends unique nonselected rows to the independent nine-row source");
    result |= check(sourceReaderParseBytes(topologyLimit, parsed) && parsed.cpuSets.size() == 4096 &&
        parsed.selectedWorkerCpuSetIds.size() == 8,
        "reader accepts exact4096 CPU-set rows without silently selecting the extra unavailable rows");
    result |= check(sourceReaderReplace(topologyLimit, arrayEnd,
        ",{\"id\":9999,\"efficiencyClass\":0,\"group\":65,\"coreIndex\":0,\"logicalProcessorIndex\":0,"
        "\"parked\":false,\"allocatedToOtherProcess\":false,\"availableToProcess\":false}" + arrayEnd),
        "over-cap topology fixture adds one distinct row");
    result |= sourceReaderRejectsAndClears(topologyLimit, previous,
        "reader rejects CPU-set row4097 before publishing a parsed source");
    std::string manyIds = "[10";
    for (unsigned index = 1; index != 4097; ++index) manyIds += ",10";
    manyIds += ']';
    std::string ownerOversized = literal;
    result |= check(sourceReaderReplace(ownerOversized, "\"ownerCpuSetIds\":[10]", "\"ownerCpuSetIds\":" + manyIds),
        "oversized owner array fixture is explicit");
    result |= sourceReaderRejectsAndClears(ownerOversized, previous,
        "oversized owner ID arrays cannot bypass bounds or uniqueness");
    std::string selectedOversized = literal;
    result |= check(sourceReaderReplace(selectedOversized, "\"selectedWorkerCpuSetIds\":[100,101,102,103,104,105,106,107]",
        "\"selectedWorkerCpuSetIds\":" + manyIds), "oversized selected array fixture is explicit");
    result |= sourceReaderRejectsAndClears(selectedOversized, previous,
        "oversized selected ID arrays cannot bypass bounds or uniqueness");
    const char *envelopes[] = {"\"kernelTiming\":{", "\"kernelReference\":{"};
    for (unsigned index = 0; index != 2; ++index)
    {
        // These two independently authored literal envelopes each occupy one
        // line. Locate their one existing outer streams array, not JSON data
        // from an arbitrary source or a second schema parser.
        const std::size_t start = literal.find(envelopes[index]);
        const std::size_t end = literal.find('\n', start);
        const bool foundEnvelope = start != std::string::npos && end != std::string::npos;
        result |= check(foundEnvelope, "fixed literal stream mutation identifies its envelope line");
        if (!foundEnvelope) continue;
        const std::string envelope = literal.substr(start, end - start);
        const std::size_t first = envelope.find("\"streams\":[") + 11;
        const std::size_t last = envelope.rfind(']');
        const bool located = first >= 11 && first < last && last < envelope.size();
        result |= check(located, "fixed literal stream mutation identifies one complete existing array");
        if (!located) continue;
        const std::string stream = envelope.substr(first, last - first);
        std::string repeated = stream;
        for (unsigned repeat = 1; repeat != 17; ++repeat) repeated += ',' + stream;
        std::string oversizedStreams = literal;
        oversizedStreams.replace(start + first, last - first, repeated);
        result |= sourceReaderRejectsAndClears(oversizedStreams, previous,
            "seventeen timing or reference streams cannot overrun fixed native storage or hide duplicate identities");
    }
    std::string members;
    for (unsigned index = 0; index != 65; ++index)
        members += ",\"extra" + std::to_string(index) + "\":0";
    std::string oversizedObject = literal;
    result |= check(sourceReaderReplace(oversizedObject, "\"schemaVersion\":6", "\"schemaVersion\":6" + members),
        "oversized closed object fixture is explicit");
    result |= sourceReaderRejectsAndClears(oversizedObject, previous,
        "too many object members are not an ignored metadata extension");
    std::string nested(17, '['); nested += '0'; nested.append(17, ']');
    std::string overdeep = literal;
    result |= check(sourceReaderReplace(overdeep, "\"schemaVersion\":6", "\"schemaVersion\":" + nested),
        "overdeep wrong-type subtree fixture is explicit");
    result |= sourceReaderRejectsAndClears(overdeep, previous,
        "overdeep wrong-type arrays cannot be skipped as an opaque field");
    std::string tokens = "[0";
    for (unsigned index = 1; index != 262145; ++index) tokens += ",0";
    tokens += ']';
    std::string overtokens = literal;
    result |= check(sourceReaderReplace(overtokens, "\"ownerCpuSetIds\":[10]", "\"ownerCpuSetIds\":" + tokens),
        "over-token input remains below the raw byte cap");
    result |= sourceReaderRejectsAndClears(overtokens, previous,
        "oversized token streams cannot outrun the tighter known-array shape limits");
    // This whole closed schema has less than16 valid nesting levels and fewer
    // than64 allowed keys per object. Its earlier field/type/array rejection
    // deliberately dominates generic caps; do not add parser introspection or
    // pretend an invalid shape is a valid cap-boundary document.
    return result;
}

bool sourceReaderProcessCpu100ns(ULONGLONG &ticks)
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER kernelTicks, userTicks;
    kernelTicks.LowPart = kernel.dwLowDateTime; kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime; userTicks.HighPart = user.dwHighDateTime;
    ticks = kernelTicks.QuadPart + userTicks.QuadPart;
    return true;
}

int testSourceReaderSelectedTopologyBudget()
{
    using namespace rts::performance;
    // Regression: repeatedly looking up each selected CPU inside the core-pair
    // loop turns this supported input extent into billions of row comparisons.
    // Synthetic receipt metadata only: no scheduler, worker or thread is started.
    std::string document(sourceReaderLiteral);
    std::string topology = "\"topology\":{\"source\":\"GetSystemCpuSetInformation\",\"cpuSets\":[";
    std::string selected = "\"selectedWorkerCpuSetIds\":[";
    for (unsigned index = 0; index != 4096; ++index)
    {
        if (index != 0) { topology += ','; selected += ','; }
        topology += "{\"id\":" + std::to_string(index) +
            ",\"efficiencyClass\":0,\"group\":" + std::to_string(index / 64) +
            ",\"coreIndex\":" + std::to_string(index % 64) +
            ",\"logicalProcessorIndex\":" + std::to_string(index % 64) +
            ",\"parked\":false,\"allocatedToOtherProcess\":false,\"availableToProcess\":true}";
        selected += std::to_string(index);
    }
    topology += "],\"ownerCpuSetIds\":[]," + selected + "]}";
    // Replace one known literal block, not an arbitrary JSON document. The two
    // large arrays contribute 147,458 lexical tokens; all other fields retain
    // the small independent fixture, keeping the complete wire below the cap.
    const std::size_t first = document.find("\"topology\":{");
    const std::size_t last = document.find(",\n\"worker\":{", first);
    int result = check(first != std::string::npos && last != std::string::npos && last > first,
        "large selected topology targets the one independent literal block");
    if (result != 0) return result;
    document.replace(first, last - first, topology);
    result |= check(sourceReaderReplace(document, "\"requestedCount\":8", "\"requestedCount\":4096") &&
        sourceReaderReplace(document, "\"effectiveCount\":8", "\"effectiveCount\":4096") &&
        sourceReaderReplace(document, "\"availableLogicalCpuCount\":16", "\"availableLogicalCpuCount\":4096") &&
        sourceReaderReplace(document, "\"reservedOwnerCpuCount\":1", "\"reservedOwnerCpuCount\":0") &&
        sourceReaderReplace(document, "\"selectedWorkerCpuCount\":8", "\"selectedWorkerCpuCount\":4096") &&
        sourceReaderReplace(document, "\"selectedWorkerPhysicalCoreCount\":8", "\"selectedWorkerPhysicalCoreCount\":4096") &&
        document.size() < 4194304,
        "large selected topology keeps exact worker counts and the bounded byte extent");
    if (result != 0) return result;

    PerformanceReceipt parsed;
    std::string reason;
    ULONGLONG beginTicks = 0, endTicks = 0;
    result |= check(sourceReaderProcessCpu100ns(beginTicks), "source-reader CPU clock is available");
    if (result != 0) return result;
    const bool accepted = sourceReaderParseBytes(document, parsed, &reason);
    result |= check(sourceReaderProcessCpu100ns(endTicks) && endTicks >= beginTicks,
        "source-reader CPU clock remains valid");
    if (result != 0) return result;
    const ULONGLONG elapsedTicks = endTicks - beginTicks;
    fprintf(stdout, "Source reader selected-topology CPU: %.3f seconds for 4096 rows\n",
        static_cast<double>(elapsedTicks) / 10000000.0);
    result |= check(accepted && reason.empty() && parsed.runId == "R-1" &&
        parsed.cpuSets.size() == 4096 && parsed.selectedWorkerCpuSetIds.size() == 4096 &&
        parsed.ownerCpuSetIds.empty() && parsed.cpuSets.front().id == 0 && parsed.cpuSets.back().id == 4095 &&
        parsed.selectedWorkerCpuSetIds.front() == 0 && parsed.selectedWorkerCpuSetIds.back() == 4095 &&
        parsed.selectedWorkerPhysicalCoreCount == 4096,
        "reader retains all distinct selected topology rows without weakening the existing schema policy");
    // Five process-CPU seconds is deliberately generous for 4,096 rows;
    // using CPU time avoids failures merely from host scheduling delays.
    // The existing CTest process timeout is the outer liveness backstop.
    result |= check(elapsedTicks <= 50000000ULL,
        "4096 selected CPU rows validate within the generous five-second CPU budget");
    if (result != 0) return result;

    std::string sharedCore(sourceReaderLiteral);
    result |= check(sourceReaderReplace(sharedCore,
        "\"id\":101,\"efficiencyClass\":0,\"group\":0,\"coreIndex\":2",
        "\"id\":101,\"efficiencyClass\":0,\"group\":0,\"coreIndex\":1"),
        "shared-core negative fixture changes exactly one small topology row");
    if (result != 0) return result;
    result |= sourceReaderRejectsAndClears(sharedCore, parsed,
        "cached topology validation still rejects two selected IDs sharing one physical core");
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
	result |= testNativeRunIdentityGoldenAndBoundaries();
	result |= testV6CompletePartitionWire();
	result |= testV6RoleAndControlWire();
	result |= testActualControlEngineReceiptProjection();
	result |= testActualWorldTraceReceiptProjection();
	result |= testSourceReaderActualWriterBytes();
	result |= testSourceReaderIndependentLiteralAndBounds();
	result |= testSourceReaderClosedShapeMutations();
	result |= testSourceReaderClosedResourceExtents();
	result |= testSourceReaderSelectedTopologyBudget();
	result |= testAtomicPublicationFaultClosure();
	return result;
}
