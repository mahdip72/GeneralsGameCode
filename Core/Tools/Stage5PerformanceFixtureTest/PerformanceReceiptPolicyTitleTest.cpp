/*
** Source-connected native policy encoding and resident-capacity contracts.
** The generated include supplies the actual private runtime helper bodies.
*/
#include "GameLogic/AIPathfind.h"
#include "Lib/PerformanceReceipt.h"
#include "Lib/SimulationExecutionPolicy.h"
#include "Lib/PipelineExecutionPolicy.h"
#include <windows.h>
#include <bcrypt.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace performance_receipt_policy_fixture
{
using namespace rts;
using namespace rts::performance;
unsigned failures = 0;
void Check(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
// Whole private native helper definitions; no test implementation is supplied.
#include "PerformanceReceiptNativePolicy.inc"

bool Same(const KernelPerformanceDigest &a, const KernelPerformanceDigest &b)
{
	return a.valid && b.valid && memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}
unsigned Nibble(char value)
{
	return value <= '9' ? value - '0' : value - 'a' + 10;
}
// Literal canonical transcript, independently laid out from the approved
// domain/type/tag/LE-value grammar. No production writer generates expected data.
const char GoldenHex[] =
	"5254532d4b45524e454c2d4649454c44532d76310450000001010000000100000006020000001b000000010200000070"
	"000000010200000061000000010200000069000000010200000072000000010200000065000000010200000064000000"
	"01020000002d00000001020000007300000001020000006f000000010200000075000000010200000072000000010200"
	"00006300000001020000006500000001020000002d00000001020000006100000001020000006400000001020000006d"
	"00000001020000006900000001020000007300000001020000007300000001020000006900000001020000006f000000"
	"01020000006e00000001020000007300000001020000002d000000010200000076000000010200000031000000060300"
	"00000800000001030000004700000001030000006500000001030000006e000000010300000065000000010300000072"
	"00000001030000006100000001030000006c000000010300000073000000060400000006000000010400000072000000"
	"01040000006500000001040000007000000001040000006c000000010400000061000000010400000079000000060500"
	"00001100000001050000006d00000001050000006900000001050000006e00000001050000006900000001050000006d"
	"00000001050000007500000001050000006d00000001050000002d000000010500000071000000010500000075000000"
	"01050000006100000001050000006c000000010500000069000000010500000066000000010500000069000000010500"
	"00006500000001050000006400000006060000001200000001060000006400000001060000006500000001060000006e"
	"00000001060000007300000001060000006500000001060000002d000000010600000065000000010600000069000000"
	"01060000006700000001060000006800000001060000007400000001060000002d000000010600000070000000010600"
	"00006c000000010600000061000000010600000079000000010600000065000000010600000072000000010700000007"
	"0000000108000000080000000109000000d0070000010a00000001000000010b00000001000000010c00000004000000"
	"010d00000000000000010e00000000100000010f00000000000400051000000001011100000001000000011200000064"
	"000000031300000064000000000000000314000000010000000000000001150000000100000003160000000000000004"
	"000000031700000000000008000000000318000000000000100000000003190000000000000200000000031a00000000"
	"00000100000000031b0000000f00000000000000031c0000005401000000000000";
KernelPerformanceDigest Golden()
{
	KernelPerformanceDigest result;
	std::vector<unsigned char> bytes((sizeof(GoldenHex) - 1) / 2);
	for (size_t i = 0; i != bytes.size(); ++i)
		bytes[i] = static_cast<unsigned char>((Nibble(GoldenHex[2*i]) << 4) |
			Nibble(GoldenHex[2*i+1]));
	BCRYPT_ALG_HANDLE algorithm = 0;
	BCRYPT_HASH_HANDLE hash = 0;
	DWORD objectSize = 0, copied = 0;
	NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0);
	std::vector<unsigned char> object;
	if (status >= 0) status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0);
	if (status >= 0)
	{
		object.resize(objectSize);
		status = BCryptCreateHash(algorithm, &hash, &object[0], objectSize, 0, 0, 0);
	}
	if (status >= 0) status = BCryptHashData(hash, &bytes[0], static_cast<ULONG>(bytes.size()), 0);
	if (status >= 0) status = BCryptFinishHash(hash, result.bytes, sizeof(result.bytes), 0);
	if (hash) BCryptDestroyHash(hash);
	if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	result.valid = status >= 0;
	Check(result.valid, "literal policy SHA-256 dependency succeeds");
	return result;
}
struct Inputs
{
	Inputs() : simulation(SIMULATION_EXECUTION_PARALLEL),
		pipeline(PIPELINE_EXECUTION_SERIAL), terminalFrame(100)
	{
		receipt.title = "Generals";
		receipt.fixtureKind = "replay";
		receipt.workloadQualification = "minimum-qualified";
		receipt.fixtureId = "dense-eight-player";
		receipt.seed = 7;
		receipt.seedKnown = true;
		receipt.fixtureIdentityObserved = true;
		receipt.requestedPlayerCount = 8;
		receipt.requestedMinimumUnitCount = 2000;
		config.workerCount = 4;
		config.workerPolicy = JOB_WORKER_POLICY_AUTO;
		config.queueCapacity = 4096;
		config.scratchBytesPerWorker = 262144;
		config.pinWorkers = true;
		trace.mode = KERNEL_TRACE_RECORD;
		trace.limits.maximumBytes = 17179869184ull;
		trace.limits.maximumRecords = 134217728;
		trace.limits.maximumLogicalEvents = 268435456;
		trace.limits.maximumAttempts = 33554432;
		trace.limits.maximumRanges = 16777216;
		trace.residentAttemptCapacity = 15;
		trace.residentRangeCapacity = 340;
	}
	KernelPerformanceDigest Encode() const
	{
		return makeNativeReceiptSourcePolicy(receipt, config, simulation,
			pipeline, terminalFrame, trace);
	}
	PerformanceReceipt receipt;
	JobSystemConfig config;
	SimulationExecutionMode simulation;
	PipelineExecutionMode pipeline;
	unsigned terminalFrame;
	KernelPerformanceTraceOptions trace;
};
void TestGoldenAndPolicyFields()
{
	const Inputs original;
	const KernelPerformanceDigest expected = Golden();
	const KernelPerformanceDigest actual = original.Encode();
	Check(Same(actual, expected), "native 28-field policy equals independent literal canonical bytes");
	// Constants tags1/2/17/20/21 are checked by the literal above. A changed
	// independently variable input must alter the hash or be rejected as invalid.
#define CHANGED(expression, message) { Inputs changed; expression; \
	Check(!Same(actual, changed.Encode()), message); }
	CHANGED(changed.receipt.title = "ZeroHour", "tag3 title is bound");
	CHANGED(changed.receipt.fixtureKind = "fresh-ai-map", "tag4 fresh source cannot inherit replay policy");
	CHANGED(changed.receipt.workloadQualification = "observed-only", "tag5 workload qualification is bound");
	CHANGED(changed.receipt.fixtureId += "-changed", "tag6 selected fixture ID is bound");
	CHANGED(++changed.receipt.seed, "tag7 independently observed seed is bound");
	CHANGED(++changed.receipt.requestedPlayerCount, "tag8 requested player count is bound");
	CHANGED(++changed.receipt.requestedMinimumUnitCount, "tag9 requested minimum units is bound");
	CHANGED(changed.simulation = SIMULATION_EXECUTION_SERIAL, "tag10 native simulation lane is bound");
	CHANGED(changed.pipeline = PIPELINE_EXECUTION_PARALLEL, "tag11 native pipeline lane is bound");
	CHANGED(changed.config.workerCount = 1, "tag12 forced-one baseline cannot share W4 policy");
	CHANGED(changed.config.workerPolicy = JOB_WORKER_POLICY_ALL, "tag13 worker selection policy is bound");
	CHANGED(++changed.config.queueCapacity, "tag14 startup queue capacity is bound");
	CHANGED(++changed.config.scratchBytesPerWorker, "tag15 per-worker scratch limit is bound");
	CHANGED(changed.config.pinWorkers = false, "tag16 configured pinning is bound");
	CHANGED(++changed.terminalFrame, "tags18/19 predeclared terminal frame and count are bound");
	CHANGED(++changed.trace.limits.maximumBytes, "tag22 byte ceiling preserves exact UInt64");
	CHANGED(++changed.trace.limits.maximumRecords, "tag23 record ceiling is bound");
	CHANGED(++changed.trace.limits.maximumLogicalEvents, "tag24 logical-event ceiling is bound");
	CHANGED(++changed.trace.limits.maximumAttempts, "tag25 attempt ceiling is bound");
	CHANGED(++changed.trace.limits.maximumRanges, "tag26 range ceiling is bound");
	CHANGED(++changed.trace.residentAttemptCapacity, "tag27 resident attempt capacity is bound");
	CHANGED(++changed.trace.residentRangeCapacity, "tag28 resident range capacity is bound");
#undef CHANGED
	Inputs baseline;
	baseline.trace.mode = KERNEL_TRACE_CONSUME;
	baseline.receipt.runId = "different-current-native-run";
	baseline.receipt.runNonce = "33333333-3333-4333-8333-333333333333";
	baseline.receipt.processId = 83;
	baseline.receipt.processCreationTimeUtc100ns = 84;
	baseline.receipt.outputDirectory = "H:\\different-current-output";
	baseline.receipt.traceFiles.sourceReceiptPath = "H:\\selected-source\\receipt.json";
	baseline.trace.binding.nativeRunIdentity.valid = true;
	baseline.trace.binding.nativeRunIdentity.bytes[0] = 99;
	Check(Same(expected, baseline.Encode()),
		"same native W4 policy excludes role, paths and separately authenticated native identities");
	Check(baseline.config.workerCount == 4, "baseline encoder never forces or rewrites configured W");
	Inputs invalid;
	invalid.terminalFrame = 0;
	Check(!invalid.Encode().valid, "unknown predeclared terminal frame cannot mint source policy");
	invalid = Inputs(); invalid.receipt.seedKnown = false;
	Check(!invalid.Encode().valid, "unobserved seed cannot mint source policy");
	invalid = Inputs(); invalid.receipt.fixtureIdentityObserved = false;
	Check(!invalid.Encode().valid, "unbound replay cannot mint source policy");
}
void TestCheckedResidentEnvelope()
{
	const unsigned workers[] = {4, 8, 16, 64};
	const JobMetricCounter attempts[] = {15, 19, 27, 75};
	const JobMetricCounter ranges[] = {340, 408, 544, 4432};
	for (unsigned i = 0; i != 4; ++i)
	{
		JobMetricCounter a = 0, r = 0;
		Check(deriveNativeReceiptTraceCapacities(workers[i], a, r) &&
			a == attempts[i] && r == ranges[i],
			"actual checked helper derives the predeclared lifetime-conditioned envelope");
	}
	JobMetricCounter a = 91, r = 92;
	Check(!deriveNativeReceiptTraceCapacities(0, a, r) && a == 0 && r == 0,
		"automatic zero-worker configuration fails closed and clears both outputs");
	a = 91; r = 92;
	Check(!deriveNativeReceiptTraceCapacities(UINT_MAX, a, r) && a == 0 && r == 0,
		"overflow or unsigned allocation-width excess rejects without allocation");
	// This is arithmetic evidence ONLY. Actual owner import/reap/late-release
	// proof is a separate prerequisite; no boolean in this fixture grants it.
}
} // namespace performance_receipt_policy_fixture
int RunPerformanceReceiptPolicyTitleTests()
{
	using namespace performance_receipt_policy_fixture;
	failures = 0;
	TestGoldenAndPolicyFields();
	TestCheckedResidentEnvelope();
	if (failures) return 1;
	printf("Native policy source-connected title tests passed.\n");
	return 0;
}
