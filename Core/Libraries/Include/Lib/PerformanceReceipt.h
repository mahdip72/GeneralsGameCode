/*
** Command & Conquer Generals performance receipt contract.
**
** This contract is intentionally separate from the human-readable scheduler
** diagnostics.  A receipt is useful to an installed-runtime verifier only
** when the executable can prove its own process, binary, run, content, and
** topology identity.  Missing host-provided evidence is a hard failure.
*/

#pragma once

// The executable-origin receipt is a native x64 qualification surface.  It
// deliberately has no VC6/32-bit adapter: the legacy oracle must not compile
// or link this producer.
#if !defined(_WIN64)
#error "PerformanceReceipt is available only in native x64 builds"
#endif

#include "Lib/JobSystem.h"
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"

#include <cstddef>
#include <string>
#include <vector>

namespace rts { namespace performance {

const unsigned PERFORMANCE_RECEIPT_SCHEMA_VERSION = 5;
const char *const PERFORMANCE_RECEIPT_PRODUCER =
	"game-executable-stage5-performance-report-v5";
const char *const PERFORMANCE_RECEIPT_EVIDENCE_KIND =
	"stage5-executable-originated-receipt";

struct PerformanceReceiptCpuSet
{
	PerformanceReceiptCpuSet();

	unsigned id;
	unsigned efficiencyClass;
	unsigned group;
	unsigned coreIndex;
	unsigned logicalProcessorIndex;
	bool parked;
	bool allocatedToOtherProcess;
	bool availableToProcess;
};

struct PerformanceReceiptPhase
{
	PerformanceReceiptPhase();

	std::string name;
	bool available;
	JobMetricCounter totalNanoseconds;
	JobMetricCounter maximumNanoseconds;
	JobMetricCounter sampleCount;
	JobMetricCounter serialNanoseconds;
	bool serialNanosecondsKnown;
	JobMetricCounter pureNanoseconds;
	bool pureNanosecondsKnown;
};

// Transport for the existing frozen reference/phase snapshots. These fields
// alone grant no source-selection or execution authority. The runtime fills
// them from its independently opened source; hashes/counts stay in the ledger.
struct PerformanceReceiptTraceFiles
{
	PerformanceReceiptTraceFiles();
	std::string tracePath;
	std::string sourceReceiptPath;
	std::string sourceRunId;
	std::string sourceRunNonce;
	unsigned sourceProcessId;
	JobMetricCounter sourceProcessCreationTimeUtc100ns;
};

// Diagnostics sampled after completed simulation frames, never instantaneous
// lifecycle counts. Requested workload remains a separate host input.
struct PerformanceReceiptWorkload
{
	PerformanceReceiptWorkload();
	JobMetricCounter sampleCount;
	unsigned firstFrame, lastFrame, playerCount;
	unsigned initialUnitCount, minimumUnitCount, peakUnitCount;
	bool rosterStable, contiguous;
};

bool IsPerformanceReceiptRosterPlayer(bool playableSide, bool observer);
bool IsPerformanceReceiptLiveUnit(bool infantry, bool vehicle,
	bool effectivelyDead, bool destroyed);
// Repeated/regressed frames are not new completed-frame observations. This
// also preserves the last match sample across terminal game-data reset.
bool ObservePerformanceReceiptWorkload(PerformanceReceiptWorkload &workload,
	unsigned frame, unsigned playerCount, unsigned liveUnitCount);

struct PerformanceReceiptKernel
{
	PerformanceReceiptKernel();

	std::string name;
	bool available;
	JobMetricCounter submittedJobs;
	JobMetricCounter completedJobs;
	JobMetricCounter physicalWorkerJobs;
	JobMetricCounter ownerHelpedJobs;
	JobMetricCounter physicalWorkerMask;
	unsigned distinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	JobMetricCounter elapsedNanoseconds;
	bool elapsedNanosecondsKnown;
};

struct PerformanceReceiptRawEvidence
{
	PerformanceReceiptRawEvidence();

	// A path is acceptable only with verifierBoundary.  Hashes are preferred,
	// but the executable cannot observe a caller-owned redirected stdout file
	// until the host closes it, so path-only evidence is an explicit boundary.
	std::string verifierBoundary;
	std::string rawLogPath;
	std::string rawLogSha256;
	std::string timingPath;
	std::string timingSha256;
	bool timingClosed;
	bool timingWriteSucceeded;
	bool timingTruncated;
	bool timingComplete;
	unsigned timingSessionCount;
	JobMetricCounter timingFrameSamples;
	unsigned timingFirstFrame, timingLastFrame;
};

struct PerformanceReceipt
{
	PerformanceReceipt();

	unsigned schemaVersion;
	std::string producer;
	std::string evidenceKind;
	std::string status;
	std::string role;
	std::string producerVersion;
	std::string title;
	std::string runId;
	std::string runNonce;
	std::string cohortNonce;
	std::string cohortCreatedUtc;
	std::string recordedUtc;
	std::string architecture;
	// Transport-only destination supplied by the installed-runtime host.  It
	// is required to publish the receipt but is intentionally not serialized as
	// evidence because it is not part of executable provenance.
	std::string outputDirectory;
	std::string receiptPath;
	std::string sourceCommit;
	std::string artifactSetSha256;
	std::string runtimeClosureDependencyManifestSha256;
	std::string runtimeClosureSha256;
	std::string executablePath;
	std::string executableSha256;
	std::string commandLine;

	unsigned processId;
	JobMetricCounter processCreationTimeUtc100ns;
	JobMetricCounter processStartTimeUtc100ns;
	JobMetricCounter processEndTimeUtc100ns;
	bool processIdentityAvailable;
	int processExitCode;
	bool processExitCodeKnown;
	std::string processExitBoundary;

	std::string fixtureId;
	std::string fixtureKind;
	std::string workloadQualification;
	std::string fixtureContentPath;
	bool fixtureIdentityObserved;
	bool fixtureObservationFailed;
	// Host expectations are not executable observations and are not serialized.
	std::string expectedFixtureContentSha256;
	unsigned expectedSeed;
	bool expectedSeedKnown;
	std::string fixtureContentSha256;
	std::string replayPath;
	std::string retainedReplayPath;
	std::string retainedReplaySha256;
	unsigned seed;
	bool seedKnown;
	unsigned requestedPlayerCount;
	unsigned requestedMinimumUnitCount;
	PerformanceReceiptWorkload workload;
	JobMetricCounter frameSimulationTotalNanoseconds;
	JobMetricCounter frameSimulationMaximumNanoseconds;
	JobMetricCounter frameSimulationSampleCount;

	unsigned frameStart;
	unsigned frameEnd;
	unsigned finalFrame;
	bool finalCrcKnown;
	unsigned finalCrc;

	unsigned requestedWorkerCount;
	std::string simulationMode;
	bool schedulerStarted;
	unsigned effectiveWorkerCount;
	std::string workerPolicy;
	bool workersPinned;
	unsigned availableLogicalCpuCount;
	unsigned reservedOwnerCpuCount;
	unsigned selectedWorkerCpuCount;
	unsigned selectedWorkerPhysicalCoreCount;
	JobMetricCounter selectedWorkerPhysicalCoreMask;
	bool selectedWorkerPhysicalCoreMaskComplete;

	JobSystemMetrics schedulerMetrics;
	std::vector<PerformanceReceiptCpuSet> cpuSets;
	std::vector<unsigned> ownerCpuSetIds;
	std::vector<unsigned> selectedWorkerCpuSetIds;
	std::vector<PerformanceReceiptPhase> phases;
	std::vector<PerformanceReceiptKernel> kernels;
	KernelPerformanceSnapshot kernelTiming;
	// Mode is requested at Begin, then replaced by the actual frozen ledger.
	// It derives measurementRole; a host label cannot disguise oracle elapsed.
	KernelPerformanceReferenceSnapshot kernelReference;
	PerformanceReceiptTraceFiles traceFiles;
	PerformanceReceiptRawEvidence rawEvidence;
};

// Canonical 0x5003 binding of only runId, runNonce, u32 PID and u64 creation
// FILETIME. Invalid ASCII safe token/UUID or zero PID/time returns no digest.
// This can precede completion; a digest grants no source or execution authority.
KernelPerformanceDigest GetPerformanceReceiptRunIdentity(
	const PerformanceReceipt &receipt);

// Pure bounded reader for a complete V6 throughput RECORD source receipt.
// It owns no file/path or host-tuple authority. Failure clears the output.
bool ParsePerformanceReceiptSource(const unsigned char *bytes,
	std::size_t byteCount, PerformanceReceipt &source, std::string *reason = 0);

// Reads only the explicit performance-run contract from the environment and
// captures executable-owned process identity. It never fabricates a hash,
// run ID, fixture identity, or host evidence.
bool BeginPerformanceReceipt(PerformanceReceipt &receipt, const char *title,
	const char *replayPath, unsigned ordinal, std::string *reason = 0);

// The owner supplies a hash read from the actual replay/map content and the
// seed from the loaded game, not from the host environment. A failed or second
// binding invalidates publication. Fresh-AI retained replay proof is separate.
bool BindPerformanceReceiptFixtureObservation(PerformanceReceipt &receipt,
	const char *kind, const char *contentPath, const char *observedContentSha256,
	unsigned observedSeed, std::string *reason = 0);

// Copies the immutable CPU-set topology retained by JobSystem after startup
// and the scheduler counters observed at the completion boundary.
bool CapturePerformanceReceiptJobSystem(PerformanceReceipt &receipt,
	const JobSystem &jobs, const JobSystemMetrics &metrics,
	std::string *reason = 0);

// The game owns the final frame/CRC and process boundary. A receipt remains
// invalid until all of these are known.
bool SetPerformanceReceiptReplayResult(PerformanceReceipt &receipt,
	unsigned frameStart, unsigned finalFrame, unsigned finalCrc,
	bool finalCrcKnown, int processExitCode, bool processExitCodeKnown,
	const char *exitBoundary, bool clean, std::string *reason = 0);

// Deterministic, escaped JSON serialization used by both the executable
// writer and focused contract tests.
bool SerializePerformanceReceipt(const PerformanceReceipt &receipt,
	std::string &document, std::string *reason = 0);

// Strict validation. In particular, unavailable process/host provenance,
// missing raw-evidence boundaries, incomplete topology, and incomplete exit
// identity all fail closed.
bool ValidatePerformanceReceipt(const PerformanceReceipt &receipt,
	std::string *reason = 0);

// Writes a validated receipt using exclusive temporary creation, flush, and
// an atomic rename. The destination directory must already exist.
typedef bool (*PerformanceReceiptPublicationObserver)(void *context,
	void *nativeHandle, const char *publishedPath, std::string *reason);
// Testable checked-close boundary. A supplied closer consumes nativeHandle
// exactly once and reports whether the kernel close qualified.
typedef bool (*PerformanceReceiptHandleCloser)(void *context,
	void *nativeHandle);
bool WritePerformanceReceiptAtomically(PerformanceReceipt &receipt,
	const char *directory, std::string *writtenPath = 0,
	std::string *reason = 0,
	PerformanceReceiptPublicationObserver observer = 0,
	void *observerContext = 0,
	PerformanceReceiptHandleCloser closer = 0,
	void *closerContext = 0);

} }
