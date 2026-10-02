#include "Lib/KernelPerformanceReference.h"
#include <stdio.h>
#include <string.h>
#include <thread>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <bcrypt.h>

namespace {
using namespace rts;
using namespace rts::performance;
int failures = 0;
void check(bool value, const char *message)
{ if (!value) { ++failures; fprintf(stderr, "FAIL: %s\n", message); } }
struct Input { unsigned a, b; unsigned char irrelevantPadding[9]; };
struct Output { unsigned sum; };
unsigned computes = 0, writes = 0, clocks = 0;
JobMetricCounter tick = 10;
bool writeInput(KernelPerformanceCanonicalWriter &writer, const void *context)
{
	++writes;
	const Input &input = *static_cast<const Input *>(context);
	return writer.sequence(1, 2) && writer.u32(2, input.a) && writer.u32(3, input.b);
}
bool writeOutput(KernelPerformanceCanonicalWriter &writer, const void *context)
{ ++writes; return writer.u32(1, static_cast<const Output *>(context)->sum); }
bool compute(const void *context, void *destination)
{
	++computes;
	const Input &input = *static_cast<const Input *>(context);
	static_cast<Output *>(destination)->sum = input.a + input.b;
	return true;
}
bool throwingCompute(const void *, void *) { throw std::runtime_error("test"); }
JobMetricCounter clock(void *) { ++clocks; tick += 5; return tick; }
JobMetricCounter badClock(void *) { return 0; }
KernelPerformanceReferenceBatch observe(KernelPerformanceReferenceLedger &ledger,
	const Input &input, const Output &actual, Output &detached, JobMetricCounter ordinal = 1)
{
	return ledger.observeValidatedBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 7, ordinal, 1, 2,
		writeInput, &input, writeOutput, &actual, compute, &detached);
}
void modeQuery()
{
	Input input = { 1, 2, {} }; Output actual = { 3 }, detached = {};
	KernelPerformanceReferenceLedger ledger;
	check(ledger.mode() == KERNEL_REFERENCE_DISABLED, "unstarted mode query is inert");
	ledger.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	check(ledger.mode() == KERNEL_REFERENCE_THROUGHPUT_BINDING, "owner can gate throughput binding without serial allocation");
	std::thread foreign([&]() { check(ledger.mode() == KERNEL_REFERENCE_DISABLED, "foreign mode query is inert"); });
	foreign.join();
	auto token = observe(ledger, input, actual, detached);
	ledger.finishBatch(token, true);
	check(ledger.freeze().complete, "foreign mode query does not mutate or poison owner evidence");
	check(ledger.mode() == KERNEL_REFERENCE_DISABLED, "frozen mode query cannot start new instrumentation");
	ledger.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock);
	check(ledger.mode() == KERNEL_REFERENCE_SERIAL_ORACLE, "owner can gate detached oracle storage");
	ledger.finishBatch(token, true);
	check(ledger.mode() == KERNEL_REFERENCE_DISABLED, "failed run mode query disables further diagnostics");
	KernelPerformanceReferenceLedger zeroBased;
	zeroBased.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	const auto zero = observe(zeroBased, input, actual, detached, 0);
	check(zero.valid() && zeroBased.finishBatch(zero, true) && zeroBased.freeze().complete,
		"reference accepts the timing ledger's legitimate zero-based first ordinal");
}
void canonicalFields()
{
	KernelPerformanceCanonicalWriter first, second, different;
	check(first.begin(1) && second.begin(1) && different.begin(2), "canonical writers begin independent schemas");
	check(first.u32(5, 42) && second.u32(5, 42) && different.u32(5, 42), "canonical integer fields encode");
	const KernelPerformanceDigest a = first.finish(), b = second.finish(), c = different.finish();
	// Independently hashed from the literal protocol bytes with .NET SHA-256,
	// not computed by the writer under test.
	const unsigned char golden[32] = { 0xc6, 0x9d, 0x9a, 0x6d, 0x29, 0xc9, 0xf6, 0x85,
		0xe6, 0x6e, 0x66, 0xed, 0x7b, 0xb8, 0x69, 0x33, 0xab, 0x2c, 0x1c, 0xaf,
		0x2a, 0x8d, 0x38, 0x54, 0xd7, 0x2c, 0x21, 0xae, 0x39, 0xc6, 0xa5, 0x6d };
	check(a.valid && memcmp(a.bytes, golden, 32) == 0, "canonical little-endian protocol matches independent golden bytes");
	check(a.equals(b), "equal field values produce identical digests");
	check(!a.equals(c), "field schema is bound to digest");
	check(!first.u32(6, 99) && first.finish().equals(a), "closed writer rejects mutation and returns frozen digest");
	KernelPerformanceCanonicalWriter typed, tagged, signedZero, positiveZero;
	typed.begin(1); tagged.begin(1); signedZero.begin(1); positiveZero.begin(1);
	typed.i32(5, 42); tagged.u32(6, 42); signedZero.f32(5, -0.0f); positiveZero.f32(5, 0.0f);
	check(!typed.finish().equals(a), "scalar type is bound, not raw bytes alone");
	check(!tagged.finish().equals(a), "field tag is bound");
	check(!signedZero.finish().equals(positiveZero.finish()), "float signed zero is preserved without arithmetic");
}
void disabledAndMatchingReference()
{
	Input input = { 7, 9, {} }; Output actual = { 16 }, detached = { 777 };
	KernelPerformanceReferenceLedger disabled;
	computes = writes = clocks = 0;
	check(disabled.beginRun(KERNEL_REFERENCE_DISABLED, clock), "disabled reference lifecycle begins");
	check(!observe(disabled, input, actual, detached).valid(), "disabled reference returns no batch");
	check(computes == 0 && writes == 0 && clocks == 0 && detached.sum == 777,
		"disabled diagnostics execute no callbacks, clocks, or serial work");
	check(!disabled.freeze().complete, "disabled reference never qualifies");
	KernelPerformanceReferenceLedger throughput;
	check(throughput.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING, clock), "throughput binding begins");
	auto token = observe(throughput, input, actual, detached);
	check(token.valid() && throughput.finishBatch(token, true), "throughput batch binds one real commit");
	auto measured = throughput.freeze();
	check(measured.complete && measured.streamCount == 1 && measured.streams[0].committedBatchCount == 1 &&
		measured.streams[0].committedOperationCount == 2, "batch cardinality is distinct from operation cardinality");
	check(computes == 0 && clocks == 0 && detached.sum == 777 && measured.streams[0].serialSampleCount == 0 &&
		measured.streams[0].serialNanoseconds == 0, "throughput never runs or claims serial oracle time");
	KernelPerformanceReferenceLedger oracle;
	check(oracle.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock), "separate oracle lifecycle begins");
	memset(input.irrelevantPadding, 0xab, sizeof(input.irrelevantPadding));
	token = observe(oracle, input, actual, detached);
	check(token.valid() && oracle.finishBatch(token, true), "pure serial output matches captured production output");
	auto reference = oracle.freeze();
	check(reference.complete && computes == 1 && clocks == 2 && detached.sum == 16 && actual.sum == 16,
		"oracle computes once into detached output and leaves production output unchanged");
	check(reference.streams[0].serialNanoseconds == 5 && reference.streams[0].serialSampleCount == 1,
		"serial interval excludes canonical input and output hashing");
	check(reference.streams[0].inputDigest.equals(measured.streams[0].inputDigest) &&
		reference.streams[0].outputDigest.equals(measured.streams[0].outputDigest) &&
		reference.streams[0].commitDigest.equals(measured.streams[0].commitDigest),
		"separate modes pair canonical inputs, outputs, and commits independent of padding");
	check(reference.streams[0].inputDigest.equals(oracle.freeze().streams[0].inputDigest), "freeze is idempotent");
}
void failureAndCommitBoundaries()
{
	Input input = { 3, 4, {} }; Output actual = { 99 }, detached = {};
	KernelPerformanceReferenceLedger mismatch;
	mismatch.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock);
	check(!observe(mismatch, input, actual, detached).valid(), "wrong serial output fails closed");
	check((mismatch.freeze().errors & KERNEL_REFERENCE_ERROR_MISMATCH) != 0, "output mismatch is explicit");
	actual.sum = 7;
	KernelPerformanceReferenceLedger open;
	open.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	observe(open, input, actual, detached);
	check(!open.freeze().complete && (open.freeze().errors & KERNEL_PERFORMANCE_ERROR_INCOMPLETE),
		"validated output without final commit disposition cannot qualify");
	KernelPerformanceReferenceLedger aborted;
	aborted.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock);
	auto token = observe(aborted, input, actual, detached);
	check(aborted.finishBatch(token, false), "failed authoritative commit closes its reference token");
	auto receipt = aborted.freeze();
	check(receipt.complete && receipt.streams[0].abortedBatchCount == 1 &&
		receipt.streams[0].committedOperationCount == 0 && receipt.streams[0].serialNanoseconds == 0,
		"uncommitted serial operation cannot inflate matched committed reference time");
	KernelPerformanceReferenceLedger brokenClock;
	brokenClock.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, badClock);
	check(!observe(brokenClock, input, actual, detached).valid() && !brokenClock.freeze().complete,
		"unavailable serial clock fails closed");
	KernelPerformanceReferenceLedger exception;
	exception.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock);
	check(!exception.observeValidatedBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 7, 1, 1, 2,
		writeInput, &input, writeOutput, &actual, throwingCompute, &detached).valid() &&
		(exception.freeze().errors & KERNEL_REFERENCE_ERROR_CALLBACK), "diagnostic callback exceptions do not escape into gameplay");
}
void orderingAndOwnership()
{
	Input input = { 1, 2, {} }; Output actual = { 3 }, detached = {};
	KernelPerformanceReferenceLedger ledger;
	ledger.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	auto old = observe(ledger, input, actual, detached);
	ledger.finishBatch(old, true); ledger.freeze();
	check(ledger.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING), "new run receives a fresh generation");
	check(!ledger.finishBatch(old, true) && !ledger.freeze().complete, "stale commit cannot contaminate a new run");
	KernelPerformanceReferenceLedger duplicate;
	duplicate.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	observe(duplicate, input, actual, detached);
	check(!observe(duplicate, input, actual, detached).valid(), "duplicate batch/frame/ordinal cannot be counted twice");
	KernelPerformanceReferenceLedger foreign;
	foreign.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	const unsigned previousWrites = writes;
	std::thread worker([&]() { observe(foreign, input, actual, detached); }); worker.join();
	check((foreign.freeze().errors & KERNEL_PERFORMANCE_ERROR_OWNER) != 0 && writes == previousWrites,
		"foreign reference calls fail closed without callback execution");
	KernelPerformanceReferenceLedger overflow;
	overflow.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	auto huge = overflow.observeValidatedBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 7, 1, 1,
		~static_cast<JobMetricCounter>(0), writeInput, &input, writeOutput, &actual);
	check(huge.valid() && overflow.finishBatch(huge, true), "full-width operation counters remain exact");
	check(!observe(overflow, input, actual, detached, 2).valid() &&
		(overflow.freeze().errors & KERNEL_PERFORMANCE_ERROR_OVERFLOW), "operation counter overflow fails closed");
	KernelPerformanceReferenceLedger capacity;
	capacity.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	for (unsigned index = 1; index <= KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES; ++index)
		check(observe(capacity, input, actual, detached, index).valid(), "bounded reference batch slot is usable");
	check(!observe(capacity, input, actual, detached, KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES + 1).valid() &&
		(capacity.freeze().errors & KERNEL_PERFORMANCE_ERROR_CAPACITY), "open batch capacity cannot overwrite commit identities");
}

// The active mode must still disable diagnostics on failure; execution routing
// needs a separate immutable query so a failure cannot change the run's role.
void latchedRunModeSurvivesFailureAndFreeze()
{
	KernelPerformanceReferenceLedger ledger;
	check(ledger.runMode() == KERNEL_REFERENCE_DISABLED, "unstarted run identity is disabled");
	check(ledger.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock), "latched-mode fixture starts an oracle run");
	check(ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE, "run identity records the requested oracle role");
	check(!ledger.finishBatch(KernelPerformanceReferenceBatch(), true), "invalid commit poisons active diagnostics");
	check(ledger.mode() == KERNEL_REFERENCE_DISABLED && ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE,
		"diagnostic failure disables active mode without changing the latched role");
	std::thread foreign([&]() {
		check(ledger.mode() == KERNEL_REFERENCE_DISABLED && ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE,
			"foreign role query is safe and cannot reactivate failed diagnostics");
	});
	foreign.join();
	const auto failed = ledger.freeze();
	check(!failed.complete && failed.mode == KERNEL_REFERENCE_SERIAL_ORACLE &&
		ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE,
		"freeze retains failed run identity independently from active collection");
	check(ledger.beginRun(KERNEL_REFERENCE_DISABLED) && ledger.runMode() == KERNEL_REFERENCE_DISABLED,
		"an explicit new run can replace the frozen role without inheriting stale identity");
}

void rejectedConfigurationCannotReplaceRunMode()
{
	KernelPerformanceReferenceLedger ledger;
	check(ledger.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock), "reconfiguration fixture starts an oracle run");
	check(!ledger.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING) &&
		ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE && ledger.mode() == KERNEL_REFERENCE_DISABLED,
		"rejected active-run configuration poisons collection without changing execution identity");
	ledger.freeze();
	check(!ledger.beginRun(static_cast<KernelPerformanceReferenceMode>(255)) &&
		ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE,
		"unsupported requested mode cannot replace the frozen role");
	check(ledger.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING) &&
		ledger.runMode() == KERNEL_REFERENCE_THROUGHPUT_BINDING,
		"accepted explicit run replaces the historical mode");
	std::thread query([&]() {
		check(ledger.mode() == KERNEL_REFERENCE_DISABLED && ledger.runMode() == KERNEL_REFERENCE_THROUGHPUT_BINDING,
			"foreign active-mode query stays disabled while its role query remains truthful");
	});
	query.join();
	check(ledger.mode() == KERNEL_REFERENCE_THROUGHPUT_BINDING,
		"foreign read-only queries never poison active reference collection");
	ledger.freeze();
}

void rejectedConfigurationPreservesCompleteFrozenReceipt()
{
	Input input = { 2, 9, {} };
	Output actual = { 11 }, detached = {};
	KernelPerformanceReferenceLedger ledger;
	check(ledger.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock), "immutable receipt fixture starts its oracle run");
	const auto token = observe(ledger, input, actual, detached);
	check(token.valid() && ledger.finishBatch(token, true), "immutable receipt fixture binds a real successful serial sample");
	const auto before = ledger.freeze();
	const bool complete = before.complete && before.frozen && before.errors == 0 && before.streamCount == 1;
	check(complete, "configuration rejection fixture starts from a fully complete frozen receipt");
	if (!complete) return;
	const unsigned callsBefore = clocks, writesBefore = writes, computesBefore = computes;
	check(!ledger.beginRun(static_cast<KernelPerformanceReferenceMode>(255)), "unsupported mode is rejected after freeze");
	const auto after = ledger.freeze();
	check(after.frozen == before.frozen && after.complete == before.complete && after.errors == before.errors &&
		after.mode == before.mode && after.generation == before.generation && after.streamCount == before.streamCount,
		"rejected configuration preserves the already-published frozen status, errors, mode, and generation");
	const auto &a = before.streams[0];
	const auto &b = after.streams[0];
	check(a.kernel == b.kernel && a.subtype == b.subtype && a.fieldSchema == b.fieldSchema &&
		a.firstFrame == b.firstFrame && a.lastFrame == b.lastFrame &&
		a.validatedBatchCount == b.validatedBatchCount && a.committedBatchCount == b.committedBatchCount &&
		a.abortedBatchCount == b.abortedBatchCount && a.validatedOperationCount == b.validatedOperationCount &&
		a.committedOperationCount == b.committedOperationCount && a.serialSampleCount == b.serialSampleCount &&
		a.serialNanoseconds == b.serialNanoseconds && a.maximumSerialNanoseconds == b.maximumSerialNanoseconds &&
		a.inputDigest.equals(b.inputDigest) && a.outputDigest.equals(b.outputDigest) && a.commitDigest.equals(b.commitDigest),
		"rejected configuration leaves every frozen stream counter and canonical digest unchanged");
	check(ledger.runMode() == KERNEL_REFERENCE_SERIAL_ORACLE && ledger.mode() == KERNEL_REFERENCE_DISABLED &&
		clocks == callsBefore && writes == writesBefore && computes == computesBefore,
		"rejected frozen configuration neither relatches the role nor reruns callbacks or clocks");
	check(ledger.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, clock), "a valid explicit run can follow the rejected frozen configuration");
	const auto activeToken = observe(ledger, input, actual, detached);
	check(activeToken.valid() && ledger.finishBatch(activeToken, true), "active rejection fixture otherwise has complete evidence");
	check(!ledger.beginRun(static_cast<KernelPerformanceReferenceMode>(255)) && ledger.mode() == KERNEL_REFERENCE_DISABLED,
		"invalid reconfiguration still poisons an active run");
	const auto activeFailure = ledger.freeze();
	check(!activeFailure.complete && (activeFailure.errors & KERNEL_PERFORMANCE_ERROR_STATE) != 0 &&
		activeFailure.generation == before.generation + 1 && activeFailure.mode == KERNEL_REFERENCE_SERIAL_ORACLE,
		"preserving old frozen receipts does not forgive active-run reconfiguration");
}

// These are literal protocol bytes, not produced by the writer under test.
const unsigned char canonicalHeader[] = {
	'R', 'T', 'S', '-', 'K', 'E', 'R', 'N', 'E', 'L', '-', 'F', 'I', 'E', 'L', 'D', 'S', '-', 'v', '1',
	1, 0, 0, 0
};
struct ByteSink
{
	ByteSink() : calls(0), failCall(0), throws(false), reenter(0), reentryKind(0), reentryRejected(false) {}
	std::vector<unsigned char> bytes;
	std::vector<unsigned> chunks;
	unsigned calls, failCall;
	bool throws;
	KernelPerformanceCanonicalWriter *reenter;
	unsigned reentryKind;
	bool reentryRejected;
	static bool append(void *context, const unsigned char *source, unsigned count)
	{
		ByteSink &sink = *static_cast<ByteSink *>(context);
		++sink.calls;
		sink.chunks.push_back(count);
		if (sink.calls == sink.failCall)
		{
			// Real transports can fail after a partial write. That prefix must
			// never acquire a valid digest or be replayed as an accepted trace.
			sink.bytes.insert(sink.bytes.end(), source, source + count / 2);
			if (sink.throws) throw std::runtime_error("transport failure");
			return false;
		}
		sink.bytes.insert(sink.bytes.end(), source, source + count);
		if (sink.reenter != 0)
		{
			KernelPerformanceCanonicalWriter *writer = sink.reenter;
			sink.reenter = 0;
			if (sink.reentryKind == 0) sink.reentryRejected = !writer->u32(99, 99);
			if (sink.reentryKind == 1) sink.reentryRejected = !writer->begin(1);
			if (sink.reentryKind == 2) sink.reentryRejected = !writer->flush();
			if (sink.reentryKind == 3) sink.reentryRejected = !writer->finish().valid;
		}
		return true;
	}
};

bool beginTransport(KernelPerformanceCanonicalWriter &writer, ByteSink &sink, bool buffered = true)
{
	const bool started = writer.begin(1, ByteSink::append, &sink, buffered);
	check(started, "canonical byte-transport overload starts without publishing unsealed fields");
	return started;
}

void bufferedCanonicalLiteralBytes()
{
	KernelPerformanceCanonicalWriter buffered, direct;
	ByteSink sink, directSink;
	if (!beginTransport(buffered, sink)) return;
	if (!beginTransport(direct, directSink, false)) return;
	auto fields = [](KernelPerformanceCanonicalWriter &writer) {
		return writer.u32(5, 42) && writer.i32(6, -2) &&
			writer.u64(0x11223344, static_cast<JobMetricCounter>(0x0807060504030201ULL)) &&
			writer.f32(7, -0.0f) && writer.boolean(8, true) && writer.sequence(9, 0);
	};
	check(fields(buffered) && fields(direct), "mixed canonical fields enter both transports");
	check(sink.calls == 0, "buffered scalar fields do not invoke the sink individually");
	const unsigned char expected[] = {
		'R', 'T', 'S', '-', 'K', 'E', 'R', 'N', 'E', 'L', '-', 'F', 'I', 'E', 'L', 'D', 'S', '-', 'v', '1',
		1, 0, 0, 0,
		1, 5, 0, 0, 0, 42, 0, 0, 0,
		2, 6, 0, 0, 0, 0xfe, 0xff, 0xff, 0xff,
		3, 0x44, 0x33, 0x22, 0x11, 1, 2, 3, 4, 5, 6, 7, 8,
		4, 7, 0, 0, 0, 0, 0, 0, 0x80,
		5, 8, 0, 0, 0, 1,
		6, 9, 0, 0, 0, 0, 0, 0, 0
	};
	const auto digest = buffered.finish(), directDigest = direct.finish();
	check(digest.valid && digest.equals(directDigest) && sink.bytes == directSink.bytes &&
		sink.bytes.size() == sizeof(expected) && memcmp(sink.bytes.data(), expected, sizeof(expected)) == 0,
		"buffering preserves every literal type, tag, little-endian value, and signed-zero byte");
	check(sink.calls == 1 && sink.chunks[0] == sizeof(expected), "finish emits the complete residual buffer once");
	const unsigned before = sink.calls;
	check(buffered.finish().equals(digest) && !buffered.u32(5, 7) && buffered.finish().equals(digest) &&
		sink.calls == before, "closed buffered writer cannot duplicate bytes or change its frozen digest");
}

// Choose repetitions of fixed literal fields to reach an exact byte offset;
// no canonical encoding routine is duplicated in this fixture.
bool appendPaddingToSize(KernelPerformanceCanonicalWriter &writer,
	std::vector<unsigned char> &expected, unsigned desired)
{
	const unsigned char wideField[] = { 3, 3, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
	const unsigned char flagField[] = { 5, 4, 0, 0, 0, 1 };
	const unsigned char intField[] = { 1, 2, 0, 0, 0, 0x78, 0x56, 0x34, 0x12 };
	expected.assign(canonicalHeader, canonicalHeader + sizeof(canonicalHeader));
	unsigned remaining = desired - static_cast<unsigned>(sizeof(canonicalHeader));
	const unsigned wideCount = remaining % 3;
	remaining -= 13 * wideCount;
	const unsigned flagCount = (2 * ((remaining / 3) % 3)) % 3;
	remaining -= 6 * flagCount;
	for (unsigned index = 0; index != wideCount; ++index)
	{
		if (!writer.u64(3, static_cast<JobMetricCounter>(0x0807060504030201ULL))) return false;
		expected.insert(expected.end(), wideField, wideField + sizeof(wideField));
	}
	for (unsigned index = 0; index != flagCount; ++index)
	{
		if (!writer.boolean(4, true)) return false;
		expected.insert(expected.end(), flagField, flagField + sizeof(flagField));
	}
	for (unsigned index = 0; index != remaining / 9; ++index)
	{
		if (!writer.u32(2, 0x12345678)) return false;
		expected.insert(expected.end(), intField, intField + sizeof(intField));
	}
	return expected.size() == desired;
}

void bufferedCanonicalEveryWideFieldSplit()
{
	for (unsigned split = 1; split != 13; ++split)
	{
		KernelPerformanceCanonicalWriter buffered, direct;
		ByteSink sink, directSink;
		if (!beginTransport(buffered, sink)) return;
		if (!beginTransport(direct, directSink, false)) return;
		std::vector<unsigned char> expected, directExpected;
		check(appendPaddingToSize(buffered, expected, 65536 - split) &&
			appendPaddingToSize(direct, directExpected, 65536 - split), "split fixture reaches the literal boundary offset");
		check(sink.calls == 0, "a partial 64-KiB buffer is retained before the crossing field");
		check(buffered.u64(9, static_cast<JobMetricCounter>(0xa8a7a6a5a4a3a2a1ULL)) &&
			direct.u64(9, static_cast<JobMetricCounter>(0xa8a7a6a5a4a3a2a1ULL)), "wide scalar crosses the buffer boundary");
		const unsigned char tail[] = { 3, 9, 0, 0, 0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8 };
		expected.insert(expected.end(), tail, tail + sizeof(tail));
		check(sink.calls == 1 && sink.chunks[0] == 65536 && sink.bytes.size() == 65536,
			"capacity flush emits exactly 64 KiB even when the cut splits a field");
		const auto digest = buffered.finish();
		check(digest.valid && digest.equals(direct.finish()) && sink.bytes == expected && sink.bytes == directSink.bytes &&
			sink.calls == 2 && sink.chunks[1] == 13 - split,
			"each of the twelve internal u64 cuts preserves its exact suffix and SHA");
	}
}

void bufferedCanonicalExactCapacityBoundaries()
{
	const unsigned lengths[] = { 65535, 65536, 65537 };
	for (unsigned index = 0; index != 3; ++index)
	{
		KernelPerformanceCanonicalWriter writer;
		ByteSink sink;
		if (!beginTransport(writer, sink)) return;
		std::vector<unsigned char> expected;
		check(appendPaddingToSize(writer, expected, lengths[index]), "capacity fixture writes its exact literal length");
		check(sink.calls == (index == 0 ? 0u : 1u), "a full buffer flushes at capacity, not on a later scalar");
		check(writer.finish().valid && sink.bytes == expected && sink.calls == (index == 2 ? 2u : 1u),
			"finish emits only a nonempty residual and never drops a boundary byte");
		for (unsigned count : sink.chunks)
			check(count != 0 && count <= 65536, "every transport append is nonempty and bounded by 64 KiB");
	}
}

void canonicalExplicitFlushAndSuccessfulReset()
{
	KernelPerformanceCanonicalWriter writer;
	ByteSink sink;
	if (!beginTransport(writer, sink)) return;
	check(writer.u32(5, 42) && writer.flush(), "a measured boundary explicitly drains its pending canonical bytes");
	check(sink.calls == 1 && sink.bytes.size() == 33 && writer.flush() && sink.calls == 1,
		"empty explicit flush is idempotent and does not invent another record");
	check(writer.boolean(6, false), "writing resumes after a nonterminal boundary flush");
	const auto first = writer.finish();
	check(first.valid && sink.calls == 2 && sink.chunks[1] == 6 && writer.flush() && sink.calls == 2,
		"finish seals only the real suffix and later flush is inert");
	sink.bytes.clear(); sink.chunks.clear(); sink.calls = 0;
	check(writer.begin(1, ByteSink::append, &sink) && writer.u32(5, 42), "a successfully sealed writer starts the next independent span");
	const auto reset = writer.finish();
	const unsigned char golden[32] = { 0xc6, 0x9d, 0x9a, 0x6d, 0x29, 0xc9, 0xf6, 0x85,
		0xe6, 0x6e, 0x66, 0xed, 0x7b, 0xb8, 0x69, 0x33, 0xab, 0x2c, 0x1c, 0xaf,
		0x2a, 0x8d, 0x38, 0x54, 0xd7, 0x2c, 0x21, 0xae, 0x39, 0xc6, 0xa5, 0x6d };
	check(reset.valid && memcmp(reset.bytes, golden, 32) == 0 && sink.calls == 1 && sink.bytes.size() == 33,
		"buffer reset emits the original golden stream without a prior-span prefix");
	ByteSink abandoned;
	{
		KernelPerformanceCanonicalWriter unfinished;
		if (!beginTransport(unfinished, abandoned)) return;
		check(unfinished.u32(5, 42), "unfinished fixture retains an unsealed suffix");
	}
	check(abandoned.calls == 0 && abandoned.bytes.empty(), "destruction cannot silently publish an unfinished trace suffix");
}

void canonicalTransportFailuresStayPoisoned()
{
	for (unsigned failure = 0; failure != 4; ++failure)
	{
		KernelPerformanceCanonicalWriter writer;
		ByteSink sink;
		if (!beginTransport(writer, sink)) return;
		sink.failCall = 1;
		sink.throws = failure >= 2;
		bool rejected = false, escaped = false;
		try
		{
			if (failure % 2 == 0)
			{
				std::vector<unsigned char> expected;
				check(appendPaddingToSize(writer, expected, 65535), "full-flush failure fixture fills its pending buffer");
				rejected = !writer.boolean(9, true);
			}
			else
			{
				check(writer.u32(5, 42), "final-flush failure fixture retains its suffix");
				rejected = !writer.finish().valid;
			}
		}
		catch (...) { escaped = true; }
		const unsigned before = sink.calls;
		check(rejected && !escaped && before == 1 && !writer.finish().valid && !writer.flush() &&
			!writer.u32(1, 1) && !writer.begin(1, ByteSink::append, &sink) && sink.calls == before,
			"partial, false, or throwing full/final flush poisons the stream without retries or a valid prefix digest");
	}
}

void canonicalTransportReentryIsRejected()
{
	for (unsigned kind = 0; kind != 4; ++kind)
	{
		KernelPerformanceCanonicalWriter writer;
		ByteSink sink;
		if (!beginTransport(writer, sink)) return;
		check(writer.u32(5, 42), "reentrant sink fixture has actual pending bytes");
		sink.reenter = &writer;
		sink.reentryKind = kind;
		check(!writer.flush() && sink.reentryRejected && !writer.finish().valid && sink.calls == 1,
			"sink callbacks cannot recursively append, reset, flush, or finalize the active writer");
	}
}

void canonicalBufferedHashWithoutTransport()
{
	KernelPerformanceCanonicalWriter buffered, legacy;
	const bool started = buffered.begin(1, 0, 0);
	check(started, "buffered semantic digest does not require an output transport");
	if (!started) return;
	check(legacy.begin(1) && buffered.u32(5, 42) && legacy.u32(5, 42) &&
		buffered.finish().equals(legacy.finish()), "hash-only span buffering preserves the legacy canonical digest");
	KernelPerformanceCanonicalWriter empty;
	ByteSink sink;
	if (!beginTransport(empty, sink)) return;
	check(empty.finish().valid && sink.calls == 1 && sink.bytes.size() == sizeof(canonicalHeader) &&
		memcmp(sink.bytes.data(), canonicalHeader, sizeof(canonicalHeader)) == 0,
		"an explicitly sealed zero-field stream still binds its exact domain and schema");
}

// Independent released-source fixture, never derived by the probe under test.
KernelPerformanceCheckpointProgress cancelledSourceProgress()
{
	KernelPerformanceCheckpointProgress source = {};
	source.entered = true;
	source.pollCount = 3;
	source.firstTruePoll = 3;
	source.firstTrueCheckpoint = { 12, 128, 0 };
	source.finalCheckpoint = { 12, 128, 0 };
	source.completedWorkUnits = 128;
	source.terminal = KERNEL_RANGE_CANCELLED;
	return source;
}

bool checkpointIs(const KernelPerformanceCheckpoint &actual,
	unsigned site, JobMetricCounter first, JobMetricCounter second)
{
	return actual.site == site && actual.first == first && actual.second == second;
}

void checkpointRecordPreservesSourcePredicateAndPrefix()
{
	// Break caught: recording changes cancellation or loses its ordinal/key/prefix.
	const unsigned clocksBefore = clocks, writesBefore = writes, computesBefore = computes;
	KernelPerformanceCheckpointProbe disabled;
	const bool disabledFalse = disabled.cancelled({ 11, 0, 0 }, false);
	const bool disabledTrue = disabled.cancelled({ 12, 64, 0 }, true);
	const auto empty = disabled.snapshot();
	check(!disabledFalse && disabledTrue && !empty.entered && empty.errors == 0 &&
		empty.pollCount == 0 && empty.firstTruePoll == 0 && empty.completedWorkUnits == 0 &&
		empty.terminal == KERNEL_RANGE_NEVER_ENTERED &&
		checkpointIs(empty.firstTrueCheckpoint, 0, 0, 0) && checkpointIs(empty.finalCheckpoint, 0, 0, 0),
		"disabled checkpoint probe preserves the source predicate without recording progress");
	KernelPerformanceCheckpointProbe recorded;
	check(recorded.beginRecord(), "checkpoint record enters one real range");
	const bool first = recorded.cancelled({ 11, 0, 0 }, false);
	const bool second = recorded.cancelled({ 12, 64, 0 }, false);
	const bool third = recorded.cancelled({ 12, 128, 0 }, true);
	check(!first && !second && third, "checkpoint recording returns each actual cancellation predicate unchanged");
	check(recorded.finish({ 12, 128, 0 }, 128, KERNEL_RANGE_CANCELLED),
		"checkpoint record closes the observed cancelled prefix");
	const auto actual = recorded.snapshot();
	check(actual.entered && actual.errors == 0 && actual.pollCount == 3 && actual.firstTruePoll == 3 &&
		actual.completedWorkUnits == 128 && actual.terminal == KERNEL_RANGE_CANCELLED &&
		checkpointIs(actual.firstTrueCheckpoint, 12, 128, 0) && checkpointIs(actual.finalCheckpoint, 12, 128, 0),
		"checkpoint record retains the literal three-poll cut and 128-unit prefix");
	struct TerminalCase { KernelPerformanceRangeTerminal terminal; bool cancel; const char *message; };
	const TerminalCase terminalCases[] = {
		{ KERNEL_RANGE_COMPLETED, false, "completed record without cancellation retains an absent cut" },
		{ KERNEL_RANGE_FAILED, false, "failed record without cancellation retains its real failure" },
		{ KERNEL_RANGE_FAILED, true, "failed record after cancellation retains both facts" }
	};
	for (const auto &test : terminalCases)
	{
		KernelPerformanceCheckpointProbe probe;
		const bool started = probe.beginRecord();
		const bool cancelled = probe.cancelled({ 21, 9, 4 }, test.cancel);
		const bool finished = probe.finish({ 21, 9, 4 }, 9, test.terminal);
		const auto progress = probe.snapshot();
		check(started && cancelled == test.cancel && finished && progress.entered && progress.errors == 0 &&
			progress.pollCount == 1 && progress.firstTruePoll == (test.cancel ? 1 : 0) &&
			progress.completedWorkUnits == 9 && progress.terminal == test.terminal &&
			checkpointIs(progress.firstTrueCheckpoint, test.cancel ? 21 : 0, test.cancel ? 9 : 0, test.cancel ? 4 : 0) &&
			checkpointIs(progress.finalCheckpoint, 21, 9, 4), test.message);
	}
	check(clocks == clocksBefore && writes == writesBefore && computes == computesBefore,
		"range-local checkpoint recording invokes no ledger clock or canonical or compute callback");
}

void checkpointReplayUsesSourceCutNotActualCancellation()
{
	// Break caught: replay still obeys actual cancellation or borrows mutable source progress.
	const unsigned clocksBefore = clocks, writesBefore = writes, computesBefore = computes;
	auto source = cancelledSourceProgress();
	KernelPerformanceCheckpointProbe replay;
	check(replay.beginReplay(source), "checkpoint replay accepts the literal released-source cut");
	source = {}; // Initialization must have copied the released value, not retained this reference.
	const bool first = replay.cancelled({ 11, 0, 0 }, true);
	const bool second = replay.cancelled({ 12, 64, 0 }, true);
	const bool third = replay.cancelled({ 12, 128, 0 }, false);
	check(!first && !second && third, "checkpoint replay uses the source cut despite opposite actual predicates");
	check(replay.finish({ 12, 128, 0 }, 128, KERNEL_RANGE_CANCELLED),
		"checkpoint replay closes only the exact source terminal and work prefix");
	const auto actual = replay.snapshot();
	check(actual.entered && actual.errors == 0 && actual.pollCount == 3 && actual.firstTruePoll == 3 &&
		actual.completedWorkUnits == 128 && actual.terminal == KERNEL_RANGE_CANCELLED &&
		checkpointIs(actual.firstTrueCheckpoint, 12, 128, 0) && checkpointIs(actual.finalCheckpoint, 12, 128, 0),
		"checkpoint replay reports independently observed progress equal to the literal source");
	KernelPerformanceCheckpointProgress noPollSource = {};
	noPollSource.entered = true;
	noPollSource.finalCheckpoint = { 21, 0, 7 };
	noPollSource.terminal = KERNEL_RANGE_COMPLETED;
	KernelPerformanceCheckpointProbe noPoll;
	const bool noPollStarted = noPoll.beginReplay(noPollSource);
	const bool noPollFinished = noPoll.finish({ 21, 0, 7 }, 0, KERNEL_RANGE_COMPLETED);
	const auto noPollActual = noPoll.snapshot();
	check(noPollStarted && noPollFinished && noPollActual.entered && noPollActual.errors == 0 &&
		noPollActual.pollCount == 0 && noPollActual.firstTruePoll == 0 && noPollActual.completedWorkUnits == 0 &&
		noPollActual.terminal == KERNEL_RANGE_COMPLETED && checkpointIs(noPollActual.firstTrueCheckpoint, 0, 0, 0) &&
		checkpointIs(noPollActual.finalCheckpoint, 21, 0, 7),
		"checkpoint replay can complete a genuinely entered zero-poll zero-work body");
	const KernelPerformanceRangeTerminal terminals[] = { KERNEL_RANGE_COMPLETED, KERNEL_RANGE_FAILED };
	for (const auto terminal : terminals)
	{
		KernelPerformanceCheckpointProgress noCutSource = {};
		noCutSource.entered = true;
		noCutSource.pollCount = 2;
		noCutSource.completedWorkUnits = 17;
		noCutSource.finalCheckpoint = { 22, 17, 3 };
		noCutSource.terminal = terminal;
		KernelPerformanceCheckpointProbe noCut;
		const bool started = noCut.beginReplay(noCutSource);
		const bool before = noCut.cancelled({ 21, 0, 3 }, true);
		const bool after = noCut.cancelled({ 21, 16, 3 }, true);
		const bool finished = noCut.finish({ 22, 17, 3 }, 17, terminal);
		const auto progress = noCut.snapshot();
		check(started && !before && !after && finished && progress.errors == 0 && progress.pollCount == 2 &&
			progress.firstTruePoll == 0 && checkpointIs(progress.firstTrueCheckpoint, 0, 0, 0) &&
			progress.completedWorkUnits == 17 && progress.terminal == terminal,
			"checkpoint replay preserves a recorded no-cut completed or failed terminal");
	}
	auto failedSource = cancelledSourceProgress();
	failedSource.terminal = KERNEL_RANGE_FAILED;
	KernelPerformanceCheckpointProbe failed;
	const bool failedStarted = failed.beginReplay(failedSource);
	const bool failedFirst = failed.cancelled({ 11, 0, 0 }, true);
	const bool failedSecond = failed.cancelled({ 12, 64, 0 }, true);
	const bool failedThird = failed.cancelled({ 12, 128, 0 }, false);
	const bool failedFinished = failed.finish({ 12, 128, 0 }, 128, KERNEL_RANGE_FAILED);
	check(failedStarted && !failedFirst && !failedSecond && failedThird && failedFinished &&
		failed.snapshot().errors == 0 && failed.snapshot().firstTruePoll == 3 &&
		failed.snapshot().terminal == KERNEL_RANGE_FAILED,
		"checkpoint replay preserves a failed terminal even when the source also cancelled");
	check(clocks == clocksBefore && writes == writesBefore && computes == computesBefore,
		"range-local checkpoint replay invokes no ledger clock or canonical or compute callback");
}

void checkpointReplayRejectsChangedCutAndTerminal()
{
	// Break caught: trusting only a count or expected terminal hides actual replay divergence.
	enum Mutation { CutSite, CutFirst, CutSecond, MissingPoll, ExtraPoll, Prefix,
		FinalSite, FinalFirst, FinalSecond, Terminal, MutationCount };
	const char *messages[] = {
		"checkpoint replay rejects a changed cancellation site",
		"checkpoint replay rejects a changed cancellation first index",
		"checkpoint replay rejects a changed cancellation second index",
		"checkpoint replay rejects a missing cancellation poll",
		"checkpoint replay rejects an extra poll after the source cut",
		"checkpoint replay rejects a changed completed prefix",
		"checkpoint replay rejects a changed final site",
		"checkpoint replay rejects a changed final first index",
		"checkpoint replay rejects a changed final second index",
		"checkpoint replay rejects a changed terminal disposition"
	};
	for (unsigned mutation = 0; mutation != MutationCount; ++mutation)
	{
		KernelPerformanceCheckpointProbe replay;
		check(replay.beginReplay(cancelledSourceProgress()), "changed-cut fixture starts from a valid literal source");
		replay.cancelled({ 11, 0, 0 }, false);
		replay.cancelled({ 12, 64, 0 }, false);
		KernelPerformanceCheckpoint cut = { 12, 128, 0 };
		if (mutation == CutSite) cut.site = 13;
		if (mutation == CutFirst) cut.first = 129;
		if (mutation == CutSecond) cut.second = 1;
		if (mutation != MissingPoll) replay.cancelled(cut, false);
		if (mutation == ExtraPoll) replay.cancelled({ 12, 192, 0 }, false);
		KernelPerformanceCheckpoint final = { 12, 128, 0 };
		if (mutation == FinalSite) final.site = 13;
		if (mutation == FinalFirst) final.first = 129;
		if (mutation == FinalSecond) final.second = 1;
		const bool finished = replay.finish(final, mutation == Prefix ? 127 : 128,
			mutation == Terminal ? KERNEL_RANGE_COMPLETED : KERNEL_RANGE_CANCELLED);
		check(!finished && (replay.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0, messages[mutation]);
		const unsigned errors = replay.snapshot().errors;
		const bool stopped = replay.cancelled({ 12, 128, 0 }, false);
		const bool retried = replay.finish({ 12, 128, 0 }, 128, KERNEL_RANGE_CANCELLED);
		check(stopped && !retried && (replay.snapshot().errors & errors) == errors,
			"checkpoint replay mismatch remains sticky and cannot be replaced by a matching retry");
	}
}

void checkpointNeverEnteredAndMalformedSourceCannotExecute()
{
	// Break caught: malformed or default source progress authorizes phantom execution.
	enum Mutation { NeverEntered, UnenteredWithProgress, CutBeyondPolls, AbsentOrdinalWithKey,
		CancelledWithoutCut, CompletedWithCut, SourceError, UnknownTerminal, EnteredNeverTerminal,
		MissingCutSite, MissingFinalSite, CutWithoutPolls, HiddenAbsentIndex, PollAfterCut, MutationCount };
	for (unsigned mutation = 0; mutation != MutationCount; ++mutation)
	{
		auto source = cancelledSourceProgress();
		switch (mutation)
		{
		case NeverEntered: source = {}; break;
		case UnenteredWithProgress: source.entered = false; break;
		case CutBeyondPolls: source.firstTruePoll = 4; break;
		case AbsentOrdinalWithKey: source.firstTruePoll = 0; break;
		case CancelledWithoutCut: source.firstTruePoll = 0; source.firstTrueCheckpoint = {}; break;
		case CompletedWithCut: source.terminal = KERNEL_RANGE_COMPLETED; break;
		case SourceError: source.errors = KERNEL_REFERENCE_ERROR_CHECKPOINT; break;
		case UnknownTerminal: source.terminal = static_cast<KernelPerformanceRangeTerminal>(99); break;
		case EnteredNeverTerminal: source.terminal = KERNEL_RANGE_NEVER_ENTERED; break;
		case MissingCutSite: source.firstTrueCheckpoint.site = 0; break;
		case MissingFinalSite: source.finalCheckpoint.site = 0; break;
		case CutWithoutPolls: source.pollCount = 0; break;
		case HiddenAbsentIndex:
			source.firstTruePoll = 0; source.firstTrueCheckpoint = { 0, 0, 1 };
			source.terminal = KERNEL_RANGE_COMPLETED; break;
		case PollAfterCut: source.firstTruePoll = 2; break;
		}
		KernelPerformanceCheckpointProbe replay;
		const bool started = replay.beginReplay(source);
		const unsigned errors = replay.snapshot().errors;
		const bool stopped = replay.cancelled({ 11, 0, 0 }, false);
		const bool finished = replay.finish({ 12, 128, 0 }, 128, KERNEL_RANGE_CANCELLED);
		const bool recordRetry = replay.beginRecord();
		const bool sourceRetry = replay.beginReplay(cancelledSourceProgress());
		check(!started && (errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 && stopped && !finished &&
			!recordRetry && !sourceRetry && (replay.snapshot().errors & errors) == errors &&
			replay.cancelled({ 11, 0, 0 }, false),
			"malformed or never-entered source stays non-executable after attempted mode or source replacement");
	}
}

void checkpointLocalLifecycleCannotEraseFailure()
{
	// Break caught: lifecycle reset or shared current-range state hides a failed range.
	KernelPerformanceCheckpointProbe doubleRecord;
	check(doubleRecord.beginRecord(), "record lifecycle fixture enters its first range");
	const bool recordRestart = doubleRecord.beginRecord();
	const bool afterRecordErrorFalse = doubleRecord.cancelled({ 11, 0, 0 }, false);
	const bool afterRecordErrorTrue = doubleRecord.cancelled({ 11, 0, 0 }, true);
	check(!recordRestart && !afterRecordErrorFalse && afterRecordErrorTrue &&
		(doubleRecord.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 &&
		!doubleRecord.beginReplay(cancelledSourceProgress()) &&
		!doubleRecord.cancelled({ 11, 0, 0 }, false) &&
		!doubleRecord.finish({ 11, 0, 0 }, 0, KERNEL_RANGE_CANCELLED),
		"record lifecycle failure remains sticky without changing subsequent actual predicates");
	KernelPerformanceCheckpointProbe doubleReplay;
	check(doubleReplay.beginReplay(cancelledSourceProgress()), "replay lifecycle fixture enters its first range");
	check(!doubleReplay.beginRecord() && !doubleReplay.beginReplay(cancelledSourceProgress()) &&
		doubleReplay.cancelled({ 11, 0, 0 }, false) &&
		(doubleReplay.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0,
		"replay lifecycle failure cannot reset into source-record or a fresh replay mode");
	KernelPerformanceCheckpointProbe finished;
	check(finished.beginRecord(), "finish lifecycle fixture enters its first range");
	check(finished.finish({ 21, 9, 4 }, 9, KERNEL_RANGE_COMPLETED),
		"entered record can finish with no cancellation polls");
	const auto before = finished.snapshot();
	check(!finished.finish({ 22, 99, 5 }, 99, KERNEL_RANGE_FAILED),
		"checkpoint body cannot finish twice with replacement progress");
	const auto after = finished.snapshot();
	check((after.errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 &&
		after.entered == before.entered && after.pollCount == before.pollCount &&
		after.firstTruePoll == before.firstTruePoll && after.completedWorkUnits == before.completedWorkUnits &&
		after.terminal == before.terminal && checkpointIs(after.finalCheckpoint, 21, 9, 4),
		"duplicate finish poisons but never rewrites the original completed body facts");
	KernelPerformanceCheckpointProbe postFinish;
	check(postFinish.beginRecord() && postFinish.finish({ 21, 9, 4 }, 9, KERNEL_RANGE_COMPLETED),
		"post-finish fixture closes one real body");
	const bool postFalse = postFinish.cancelled({ 21, 9, 4 }, false);
	const bool postTrue = postFinish.cancelled({ 21, 9, 4 }, true);
	check(!postFalse && postTrue && (postFinish.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 &&
		postFinish.snapshot().pollCount == 0 && postFinish.snapshot().completedWorkUnits == 9,
		"polling after finish fails recording without changing predicates or extending the completed body");
	KernelPerformanceCheckpointProbe invalidPoll;
	check(invalidPoll.beginRecord(), "invalid-site fixture enters one record range");
	const bool invalidFalse = invalidPoll.cancelled({ 0, 0, 0 }, false);
	const bool invalidTrue = invalidPoll.cancelled({ 11, 1, 0 }, true);
	check(!invalidFalse && invalidTrue && (invalidPoll.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 &&
		!invalidPoll.finish({ 11, 1, 0 }, 1, KERNEL_RANGE_CANCELLED),
		"invalid recording site fails the trace while preserving real cancellation");
	KernelPerformanceCheckpointProbe afterCut;
	const bool afterCutStarted = afterCut.beginRecord();
	const bool cut = afterCut.cancelled({ 11, 0, 0 }, true);
	const bool laterFalse = afterCut.cancelled({ 12, 64, 0 }, false);
	const bool laterTrue = afterCut.cancelled({ 12, 128, 0 }, true);
	check(afterCutStarted && cut && !laterFalse && laterTrue &&
		(afterCut.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 &&
		afterCut.snapshot().firstTruePoll == 1 && checkpointIs(afterCut.snapshot().firstTrueCheckpoint, 11, 0, 0) &&
		!afterCut.finish({ 12, 128, 0 }, 128, KERNEL_RANGE_CANCELLED),
		"source polls after the first true cut fail V1 recording without changing predicates or replacing the cut");
	KernelPerformanceCheckpointProbe unclosed;
	check(unclosed.beginRecord(), "unclosed fixture enters one record range");
	unclosed.cancelled({ 11, 0, 0 }, false);
	const auto unfinished = unclosed.snapshot();
	KernelPerformanceCheckpointProbe cannotReplayUnfinished;
	check(unfinished.entered && unfinished.terminal == KERNEL_RANGE_NEVER_ENTERED &&
		!cannotReplayUnfinished.beginReplay(unfinished),
		"missing finish cannot publish executable completed source progress");
	struct BadFinish { unsigned site; KernelPerformanceRangeTerminal terminal; bool cancel; };
	const BadFinish badFinishes[] = {
		{ 0, KERNEL_RANGE_COMPLETED, false },
		{ 21, KERNEL_RANGE_CANCELLED, false },
		{ 21, KERNEL_RANGE_COMPLETED, true },
		{ 21, KERNEL_RANGE_NEVER_ENTERED, false },
		{ 21, static_cast<KernelPerformanceRangeTerminal>(99), false }
	};
	for (const auto &test : badFinishes)
	{
		KernelPerformanceCheckpointProbe probe;
		check(probe.beginRecord(), "invalid-terminal fixture enters one real record range");
		probe.cancelled({ 21, 9, 4 }, test.cancel);
		check(!probe.finish({ test.site, 9, 4 }, 9, test.terminal) &&
			(probe.snapshot().errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0,
			"invalid final site or impossible source terminal cannot become released valid progress");
	}
	KernelPerformanceCheckpointProbe first, second;
	const bool firstStarted = first.beginRecord(), secondStarted = second.beginRecord();
	const bool firstCut = first.cancelled({ 31, 8, 2 }, false);
	const bool secondCut = second.cancelled({ 31, 8, 2 }, true);
	const bool secondFinished = second.finish({ 31, 8, 2 }, 8, KERNEL_RANGE_CANCELLED);
	const bool firstFinished = first.finish({ 31, 8, 2 }, 8, KERNEL_RANGE_COMPLETED);
	const auto firstProgress = first.snapshot(), secondProgress = second.snapshot();
	check(firstStarted && secondStarted && !firstCut && secondCut && firstFinished && secondFinished &&
		firstProgress.errors == 0 && secondProgress.errors == 0 && firstProgress.pollCount == 1 &&
		secondProgress.pollCount == 1 && firstProgress.firstTruePoll == 0 && secondProgress.firstTruePoll == 1 &&
		firstProgress.terminal == KERNEL_RANGE_COMPLETED && secondProgress.terminal == KERNEL_RANGE_CANCELLED &&
		checkpointIs(firstProgress.firstTrueCheckpoint, 0, 0, 0) && checkpointIs(secondProgress.firstTrueCheckpoint, 31, 8, 2),
		"independent range probes may reuse checkpoint keys without sharing cut or terminal state");
}

void sourceAttemptNoCaptureRejectionClosesWithoutCanonicalStream()
{
	// Break caught: preflight rejection disappears because only validated batches
	// are observed, or finishing a rejected attempt fabricates a canonical stream.
	ByteSink sink;
	KernelPerformanceReferenceRunOptions options;
	options.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
	options.clock = clock;
	options.trace.mode = KERNEL_TRACE_RECORD;
	options.trace.append = ByteSink::append;
	options.trace.context = &sink;
	options.trace.limits.maximumBytes = 1048576;
	options.trace.limits.maximumRecords = 100000;
	options.trace.limits.maximumLogicalEvents = 100000;
	options.trace.limits.maximumAttempts = 10000;
	options.trace.limits.maximumRanges = 500000;
	// Literal W=4 integration-derived fixture bounds: 11+4=15 attempts;
	// 256+16+4+4*16=340 reusable live ranges. Core must not derive game policy.
	options.trace.residentAttemptCapacity = 15;
	options.trace.residentRangeCapacity = 340;
	// Opaque, independent binding fixtures. D owns native tuple derivation using
	// the approved four-field 0x5003 identity schema, not this trace-ledger test.
	const unsigned char identityBytes[32] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
	};
	options.trace.binding.nativeRunIdentity.valid = true;
	memcpy(options.trace.binding.nativeRunIdentity.bytes, identityBytes, sizeof(identityBytes));
	options.trace.binding.executable = options.trace.binding.nativeRunIdentity;
	options.trace.binding.executable.bytes[0] = 0x20;
	options.trace.binding.fixture = options.trace.binding.nativeRunIdentity;
	options.trace.binding.fixture.bytes[0] = 0x40;
	options.trace.binding.sourcePolicy = options.trace.binding.nativeRunIdentity;
	options.trace.binding.sourcePolicy.bytes[0] = 0x60;
	KernelPerformanceAttemptIdentity identity = {};
	identity.workKind = KERNEL_PERFORMANCE_PATH;
	identity.subtype = 0;
	identity.sampleOrdinal = 1;
	identity.attemptOrdinal = 0;
	identity.phase = KERNEL_PHASE_SPATIAL_WORK;
	identity.ownerFrame = 7;
	KernelPerformanceAttemptDecision decision = {};
	decision.decisionOrdinal = 0;
	decision.site = 11;
	decision.reasonSchema = 1;
	decision.reason = 1;
	decision.deterministicEligible = false;
	decision.deterministicFacts = options.trace.binding.fixture;
	decision.admission = KERNEL_ADMISSION_NOT_REQUESTED;
	decision.sourceConfiguredWorkers = 4;
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_NOT_ADMITTED;
	finish.reasonSchema = 1;
	finish.reason = 1;
	finish.fallbackEntered = true;
	finish.fallbackCompleted = true;
	KernelPerformanceAttemptReap reap = {};
	reap.reasonSchema = 1;
	reap.reason = 1;
	const unsigned clocksBefore = clocks, writesBefore = writes, computesBefore = computes;
	KernelPerformanceReferenceLedger source;
	check(source.beginRun(options), "source attempt recording starts with explicit binding and resident limits");
	const auto attempt = source.beginAttempt(identity);
	check(attempt.valid(), "source trace observes a zero-ordinal attempt before any capture or validation");
	check(source.observeDecision(attempt, decision), "source trace records the actual no-capture preflight rejection");
	check(source.finishAttempt(attempt, finish), "source trace finishes the unchanged fallback without a validated batch");
	check(source.reapAttempt(attempt, reap), "source trace reaps the synchronous rejected attempt exactly once");
	check(source.sealObservationWindow(), "source trace seals new observation ingress after its rejected attempt");
	check(source.sealExecutionClosure(), "source trace seals its empty retained-attempt set after ingress");
	const auto receipt = source.freeze();
	const auto &trace = receipt.trace;
	check(trace.requested && trace.mode == KERNEL_TRACE_RECORD && trace.frozen && trace.complete &&
		trace.errors == 0 && trace.observationSealed && trace.executionSealed,
		"a fully closed no-capture source trace is complete independently from canonical stream completeness");
	check(trace.attemptCount == 1 && trace.admittedAttemptCount == 0 && trace.notAdmittedAttemptCount == 1 &&
		trace.abortedAfterAdmissionAttemptCount == 0 && trace.reapCount == 1 &&
		trace.residentAttemptCount == 0 && trace.residentAttemptHighWater == 1 &&
		trace.recordCount == 8 && trace.logicalEventCount == 8 &&
		trace.coalescedSpanCount == 0 && trace.coalescedAttemptCount == 0,
		"no-capture source counts one rejected and reaped attempt plus eight actual framing and lifecycle records");
	check(receipt.frozen && !receipt.complete && receipt.errors == 0 && receipt.streamCount == 0 &&
		receipt.mode == KERNEL_REFERENCE_THROUGHPUT_BINDING && source.runMode() == KERNEL_REFERENCE_THROUGHPUT_BINDING,
		"a complete rejection trace never fabricates a validated or committed canonical stream");
	check(trace.binding.nativeRunIdentity.equals(options.trace.binding.nativeRunIdentity) &&
		trace.binding.executable.equals(options.trace.binding.executable) &&
		trace.binding.fixture.equals(options.trace.binding.fixture) &&
		trace.binding.sourcePolicy.equals(options.trace.binding.sourcePolicy) &&
		trace.limits.maximumBytes == 1048576 && trace.limits.maximumRecords == 100000 &&
		trace.limits.maximumLogicalEvents == 100000 && trace.limits.maximumAttempts == 10000 &&
		trace.limits.maximumRanges == 500000 && trace.residentAttemptCapacity == 15 && trace.residentRangeCapacity == 340,
		"source trace retains four independent bindings and keeps resident capacities distinct from five volume limits");
	// Literal canonical prefix: field schema0x5001; kind1 at tag1; record ordinal1
	// at tag2; trace version1 at tag3; resident capacities15/340 at tags4/5.
	// Freeze the entire header independently: omitting/reordering a limit or
	// binding on the wire must fail even when the snapshot copies remain right.
	const unsigned char headerPrefix[] = {
		'R', 'T', 'S', '-', 'K', 'E', 'R', 'N', 'E', 'L', '-', 'F', 'I', 'E', 'L', 'D', 'S', '-', 'v', '1',
		0x01, 0x50, 0x00, 0x00,
		1, 1, 0, 0, 0, 1, 0, 0, 0,
		3, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		1, 3, 0, 0, 0, 1, 0, 0, 0,
		3, 4, 0, 0, 0, 15, 0, 0, 0, 0, 0, 0, 0,
		3, 5, 0, 0, 0, 0x54, 0x01, 0, 0, 0, 0, 0, 0,
		// Five historical limits: 1048576, 100000, 100000, 10000, 500000.
		3, 6, 0, 0, 0, 0x00, 0x00, 0x10, 0, 0, 0, 0, 0,
		3, 7, 0, 0, 0, 0xa0, 0x86, 0x01, 0, 0, 0, 0, 0,
		3, 8, 0, 0, 0, 0xa0, 0x86, 0x01, 0, 0, 0, 0, 0,
		3, 9, 0, 0, 0, 0x10, 0x27, 0x00, 0, 0, 0, 0, 0,
		3, 10, 0, 0, 0, 0x20, 0xa1, 0x07, 0, 0, 0, 0, 0,
		6, 11, 0, 0, 0, 4, 0, 0, 0,
		// Native run binding, then executable, fixture, and source policy.
		6, 13, 0, 0, 0, 4, 0, 0, 0,
		3, 14, 0, 0, 0, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		3, 15, 0, 0, 0, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		3, 16, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		3, 17, 0, 0, 0, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		6, 13, 0, 0, 0, 4, 0, 0, 0,
		3, 14, 0, 0, 0, 0x20, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		3, 15, 0, 0, 0, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		3, 16, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		3, 17, 0, 0, 0, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		6, 13, 0, 0, 0, 4, 0, 0, 0,
		3, 14, 0, 0, 0, 0x40, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		3, 15, 0, 0, 0, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		3, 16, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		3, 17, 0, 0, 0, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		6, 13, 0, 0, 0, 4, 0, 0, 0,
		3, 14, 0, 0, 0, 0x60, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		3, 15, 0, 0, 0, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		3, 16, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		3, 17, 0, 0, 0, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
	};
	check(sink.calls != 0 && sink.bytes.size() > sizeof(headerPrefix) &&
		memcmp(sink.bytes.data(), headerPrefix, sizeof(headerPrefix)) == 0 &&
		trace.byteCount == sink.bytes.size() && trace.digest.valid,
		"source trace emits the literal full header with resident capacities, five limits, four bindings, and full byte count");
	check(clocks == clocksBefore && writes == writesBefore && computes == computesBefore,
		"no-capture rejection recording executes no input or output or detached compute callback and reads no clock");
	const unsigned appendCalls = sink.calls;
	const auto again = source.freeze();
	check(again.trace.complete == trace.complete && again.trace.errors == trace.errors &&
		again.trace.attemptCount == trace.attemptCount && again.trace.recordCount == trace.recordCount &&
		again.trace.byteCount == trace.byteCount && again.trace.digest.valid == trace.digest.valid &&
		memcmp(again.trace.digest.bytes, trace.digest.bytes, sizeof(trace.digest.bytes)) == 0 && sink.calls == appendCalls,
		"freezing the rejected source twice cannot append a duplicate footer or change its frozen counters and hash");
}

struct SourceAbortFixture
{
	SourceAbortFixture() : input(), rejected(), accepted(), refused(), notAdmitted(), aborted(), reap(),
		dispatch(), firstRange(), secondRange(), cancelled(), neverEntered()
	{
		options.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
		options.clock = clock;
		options.trace.mode = KERNEL_TRACE_RECORD;
		options.trace.append = ByteSink::append;
		options.trace.context = &sink;
		options.trace.limits.maximumBytes = 1048576;
		options.trace.limits.maximumRecords = 100000;
		options.trace.limits.maximumLogicalEvents = 100000;
		options.trace.limits.maximumAttempts = 10000;
		options.trace.limits.maximumRanges = 500000;
		options.trace.residentAttemptCapacity = 15;
		options.trace.residentRangeCapacity = 340;
		const unsigned char identityBytes[32] = {
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
			0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
			0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
		};
		options.trace.binding.nativeRunIdentity.valid = true;
		memcpy(options.trace.binding.nativeRunIdentity.bytes, identityBytes, sizeof(identityBytes));
		options.trace.binding.executable = options.trace.binding.nativeRunIdentity;
		options.trace.binding.executable.bytes[0] = 0x20;
		options.trace.binding.fixture = options.trace.binding.nativeRunIdentity;
		options.trace.binding.fixture.bytes[0] = 0x40;
		options.trace.binding.sourcePolicy = options.trace.binding.nativeRunIdentity;
		options.trace.binding.sourcePolicy.bytes[0] = 0x60;
		input.a = 3; input.b = 4;
		rejected.site = 11; rejected.reasonSchema = 1; rejected.reason = 1;
		rejected.deterministicFacts = options.trace.binding.fixture;
		rejected.sourceConfiguredWorkers = 4;
		accepted = rejected;
		accepted.site = 21; accepted.reason = 2;
		accepted.deterministicEligible = true;
		accepted.admission = KERNEL_ADMISSION_ACCEPTED;
		refused = accepted;
		refused.site = 22; refused.reason = 4;
		refused.admission = KERNEL_ADMISSION_REFUSED;
		refused.dynamicFactsKnownMask = 4; refused.activeSlots = 1;
		notAdmitted.disposition = KERNEL_PERFORMANCE_NOT_ADMITTED;
		notAdmitted.reasonSchema = 1; notAdmitted.reason = 1;
		notAdmitted.fallbackEntered = notAdmitted.fallbackCompleted = true;
		aborted = notAdmitted;
		aborted.disposition = KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
		aborted.reason = 3;
		reap.reasonSchema = 1; reap.reason = 1;
		reap.dynamicFactsKnownMask = 4;
		dispatch.bodySchema = dispatch.checkpointSchema = 1;
		dispatch.rangeCount = 2; dispatch.operationCount = 2;
		dispatch.sourceGrain = 1; dispatch.sourceLimit = 2;
		firstRange.bodyKind = 1; firstRange.end = 1; firstRange.operationCount = 1;
		secondRange = firstRange;
		secondRange.rangeOrdinal = 1; secondRange.begin = 1; secondRange.end = 2;
		const KernelPerformanceCheckpointProgress literalCancelled = {
			true, 0, 3, 3, 128, { 12, 128, 0 }, { 12, 128, 0 }, KERNEL_RANGE_CANCELLED
		};
		cancelled.checkpoint = literalCancelled;
		cancelled.publication = KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL;
		// neverEntered remains all-zero POD: no probe/body was ever invoked.
	}
	KernelPerformanceAttemptIdentity identity(JobMetricCounter ordinal) const
	{
		KernelPerformanceAttemptIdentity value = {};
		value.workKind = KERNEL_PERFORMANCE_PATH;
		value.sampleOrdinal = 1; value.attemptOrdinal = ordinal;
		value.phase = KERNEL_PHASE_SPATIAL_WORK; value.ownerFrame = 7;
		return value;
	}
	ByteSink sink;
	KernelPerformanceReferenceRunOptions options;
	Input input;
	KernelPerformanceAttemptDecision rejected, accepted, refused;
	KernelPerformanceAttemptFinish notAdmitted, aborted;
	KernelPerformanceAttemptReap reap;
	KernelPerformanceDispatchPlan dispatch;
	KernelPerformanceRangePlan firstRange, secondRange;
	KernelPerformanceRangeProgress cancelled, neverEntered;
};

void sourceAbortedAttemptRetainsIdentityUntilReleasedReap()
{
	// Break caught: finished-but-unreleased B disappears, or C's slot-busy
	// rejection is confused with B's admission and late released range records.
	SourceAbortFixture fixture;
	KernelPerformanceReferenceLedger source;
	const unsigned clocksBefore = clocks, writesBefore = writes, computesBefore = computes;
	check(source.beginRun(fixture.options), "aborted source fixture begins with frozen source bounds");
	const auto a = source.beginAttempt(fixture.identity(0));
	check(a.valid(), "A begins before its actual no-capture preflight");
	check(source.observeDecision(a, fixture.rejected), "A records its deterministic no-capture rejection");
	check(source.finishAttempt(a, fixture.notAdmitted), "A records completed unchanged fallback");
	check(source.reapAttempt(a, fixture.reap), "A reaps before the independently admitted B attempt");
	const auto b = source.beginAttempt(fixture.identity(1));
	check(b.valid(), "B receives a new identity after A was reaped");
	check(source.bindCapturedInput(b, 1, 2, writeInput, &fixture.input), "B canonicalizes its real two-operation input once");
	check(source.observeDecision(b, fixture.accepted), "B records actual source acceptance independently from validation");
	check(source.observeDispatch(b, fixture.dispatch), "B records its actual two-range dispatch plan");
	check(source.observeRangePlan(b, fixture.firstRange), "B binds cancelled request range zero to its dispatch");
	check(source.observeRangePlan(b, fixture.secondRange), "B binds never-entered request range one to its dispatch");
	check(source.finishAttempt(b, fixture.aborted), "B finishes aborted fallback before either range is owner-imported");
	const auto c = source.beginAttempt(fixture.identity(2));
	check(c.valid(), "C remains a separate observation while finished B is still retained");
	check(source.observeDecision(c, fixture.refused), "C retains actual source active-slot refusal without changing B admission");
	check(source.finishAttempt(c, fixture.notAdmitted), "C finishes its own unchanged fallback without capture");
	check(source.reapAttempt(c, fixture.reap), "C reaps without reaping the older pending B identity");
	// Supplied POD is imported only after the fixture's declared release point.
	// Real scheduler/group proof belongs to the native producer integration test.
	check(source.observeReleasedRange(b, fixture.firstRange, fixture.cancelled), "B imports its released cancelled 128-unit prefix");
	check(source.observeReleasedRange(b, fixture.secondRange, fixture.neverEntered), "B acknowledges the released never-entered range without a body");
	check(source.reapAttempt(b, fixture.reap), "B reaps only after both distinct planned ranges were acknowledged");
	check(source.sealObservationWindow(), "aborted source seals new observation ingress after all three attempts");
	check(source.sealExecutionClosure(), "aborted source seals reference closure after its actual retained set is empty");
	const auto receipt = source.freeze();
	const auto &trace = receipt.trace;
	check(trace.requested && trace.frozen && trace.complete && trace.errors == 0 &&
		trace.observationSealed && trace.executionSealed, "closed aborted source trace qualifies independently of canonical success");
	check(trace.attemptCount == 3 && trace.admittedAttemptCount == 1 && trace.notAdmittedAttemptCount == 2 &&
		trace.abortedAfterAdmissionAttemptCount == 1 && trace.reapCount == 3 &&
		trace.residentAttemptCount == 0 && trace.residentAttemptHighWater == 2,
		"A B C remain three attempts with one admission and two simultaneously retained identities");
	check(trace.capturedAttemptCount == 1 && trace.capturedOperationCount == 2 && trace.dispatchCount == 1 &&
		trace.rangeCount == 2 && trace.releasedRangeCount == 2 && trace.residentRangeCount == 0 && trace.residentRangeHighWater == 2,
		"two request ranges and 128 search work units never become extra captured operations or admitted batches");
	check(trace.recordCount == 22 && trace.logicalEventCount == 22 &&
		trace.coalescedSpanCount == 0 && trace.coalescedAttemptCount == 0,
		"eighteen actual lifecycle records plus header two seals and footer produce exactly twenty-two records");
	check(receipt.frozen && !receipt.complete && receipt.streamCount == 0 &&
		receipt.mode == KERNEL_REFERENCE_THROUGHPUT_BINDING && source.runMode() == KERNEL_REFERENCE_THROUGHPUT_BINDING,
		"aborted source work fabricates no validated or committed canonical stream");
	check(writes == writesBefore + 1 && clocks == clocksBefore && computes == computesBefore &&
		fixture.input.a == 3 && fixture.input.b == 4,
		"aborted source records input once with no output callback detached body clock or authoritative input mutation");
	// Independent final footer extension: tags16..22 bind all seven derived
	// capture/range counters; no snapshot-only or production-generated oracle.
	const unsigned char footerSuffix[] = {
		3, 16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 17, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 18, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 19, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 20, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 22, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	check(fixture.sink.bytes.size() >= sizeof(footerSuffix) &&
		memcmp(fixture.sink.bytes.data() + fixture.sink.bytes.size() - sizeof(footerSuffix), footerSuffix, sizeof(footerSuffix)) == 0 &&
		trace.byteCount == fixture.sink.bytes.size() && trace.digest.valid,
		"one final V1 footer binds range counts and zero successful/window totals at tags sixteen through twenty-nine");
}

KernelPerformanceAttempt startFinishedAbortedSource(KernelPerformanceReferenceLedger &source,
	SourceAbortFixture &fixture, bool plansBeforeAdmission = false)
{
	const bool started = source.beginRun(fixture.options);
	const auto b = source.beginAttempt(fixture.identity(1));
	const bool captured = source.bindCapturedInput(b, 1, 2, writeInput, &fixture.input);
	bool admitted = false;
	if (!plansBeforeAdmission) admitted = source.observeDecision(b, fixture.accepted);
	const bool dispatched = source.observeDispatch(b, fixture.dispatch);
	const bool firstPlanned = source.observeRangePlan(b, fixture.firstRange);
	const bool secondPlanned = source.observeRangePlan(b, fixture.secondRange);
	if (plansBeforeAdmission) admitted = source.observeDecision(b, fixture.accepted);
	const bool finished = source.finishAttempt(b, fixture.aborted);
	check(started && b.valid() && captured && admitted && dispatched && firstPlanned && secondPlanned && finished,
		"retention fixture records actual capture plans admission and aborted finish before owner release");
	return b;
}
void observeAndReapRefusedSource(KernelPerformanceReferenceLedger &source, SourceAbortFixture &fixture)
{
	const auto c = source.beginAttempt(fixture.identity(2));
	const bool refused = source.observeDecision(c, fixture.refused);
	const bool finished = source.finishAttempt(c, fixture.notAdmitted);
	const bool reaped = source.reapAttempt(c, fixture.reap);
	check(c.valid() && refused && finished && reaped,
		"independent slot-busy C is recorded and reaped while finished B still owns its identity");
}
KernelPerformanceReferenceSnapshot releaseAndCloseAbortedSource(KernelPerformanceReferenceLedger &source,
	SourceAbortFixture &fixture, KernelPerformanceAttempt b)
{
	check(source.observeReleasedRange(b, fixture.firstRange, fixture.cancelled), "retained B accepts its first real released range");
	check(source.observeReleasedRange(b, fixture.secondRange, fixture.neverEntered), "retained B accepts its second real released range");
	check(source.reapAttempt(b, fixture.reap), "retained B reaps after both released ranges not after fallback alone");
	check(source.sealObservationWindow(), "released abort closes observation ingress");
	check(source.sealExecutionClosure(), "released abort closes reference execution without a scheduler claim");
	return source.freeze();
}

void sourceAbortedRetentionRejectsEarlyReapAndClosure()
{
	// Break caught: one early/partial release or another attempt's cleanup can
	// recycle B's live metadata and turn incomplete source work into a receipt.
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		const auto b = startFinishedAbortedSource(source, fixture);
		check(!source.reapAttempt(b, fixture.reap), "aborted fallback cannot reap before either released range");
		const auto trace = source.freeze().trace;
		check(!trace.complete && trace.errors != 0 && trace.residentAttemptCount == 1 &&
			trace.abortedAfterAdmissionAttemptCount == 1 && trace.residentRangeCount == 2 && trace.releasedRangeCount == 0,
			"early reap failure retains the admitted attempt and both unreleased ranges");
	}
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		const auto b = startFinishedAbortedSource(source, fixture);
		check(source.observeReleasedRange(b, fixture.firstRange, fixture.cancelled), "partial release imports only B range zero");
		check(!source.reapAttempt(b, fixture.reap), "one released range cannot silently acknowledge the never-entered second range");
		const auto trace = source.freeze().trace;
		check(!trace.complete && trace.errors != 0 && trace.residentAttemptCount == 1 &&
			trace.residentRangeCount == 1 && trace.rangeCount == 2 && trace.releasedRangeCount == 1,
			"partial release leaves one range resident and cannot close the enclosing attempt");
	}
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		startFinishedAbortedSource(source, fixture);
		observeAndReapRefusedSource(source, fixture);
		check(source.sealObservationWindow(), "pending B may survive a normal observation ingress seal");
		check(!source.sealExecutionClosure(), "C reap cannot permit execution closure while B is pending");
		const auto trace = source.freeze().trace;
		check(!trace.complete && trace.errors != 0 && trace.observationSealed && !trace.executionSealed &&
			trace.attemptCount == 2 && trace.reapCount == 1 && trace.residentAttemptCount == 1 && trace.residentRangeCount == 2,
			"failed premature closure preserves sealed ingress and B identity after C was reaped");
	}
}

void sourceAbortedCapacityAndPreAdmissionPlanOrder()
{
	// Break caught: finish frees a slot, or diagnostics require moving the real
	// source admission ahead of its already-prepared dispatch/range plan.
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		fixture.options.trace.residentAttemptCapacity = 1;
		startFinishedAbortedSource(source, fixture);
		check(!source.beginAttempt(fixture.identity(2)).valid(), "one-slot fixture cannot reuse B identity before release and reap");
		const auto trace = source.freeze().trace;
		check(!trace.complete && (trace.errors & KERNEL_PERFORMANCE_ERROR_CAPACITY) != 0 &&
			trace.attemptCount == 1 && trace.residentAttemptCount == 1 && trace.residentRangeCount == 2,
			"exhausted live capacity is explicit without changing or clearing the retained source attempt");
	}
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		fixture.options.trace.residentAttemptCapacity = 2;
		const auto b = startFinishedAbortedSource(source, fixture);
		observeAndReapRefusedSource(source, fixture);
		const auto receipt = releaseAndCloseAbortedSource(source, fixture, b);
		const auto &trace = receipt.trace;
		check(trace.complete && trace.errors == 0 && trace.attemptCount == 2 && trace.admittedAttemptCount == 1 &&
			trace.notAdmittedAttemptCount == 1 && trace.abortedAfterAdmissionAttemptCount == 1 && trace.reapCount == 2 &&
			trace.residentAttemptCount == 0 && trace.residentAttemptHighWater == 2 &&
			trace.residentRangeCount == 0 && trace.releasedRangeCount == 2 && trace.recordCount == 18,
			"two-slot fixture keeps B across C cleanup then releases exactly the two distinct source identities");
	}
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		const auto b = startFinishedAbortedSource(source, fixture, true);
		const auto receipt = releaseAndCloseAbortedSource(source, fixture, b);
		const auto &trace = receipt.trace;
		check(trace.complete && trace.errors == 0 && trace.attemptCount == 1 && trace.admittedAttemptCount == 1 &&
			trace.abortedAfterAdmissionAttemptCount == 1 && trace.capturedAttemptCount == 1 &&
			trace.capturedOperationCount == 2 && trace.dispatchCount == 1 && trace.rangeCount == 2 &&
			trace.releasedRangeCount == 2 && trace.reapCount == 1 && trace.recordCount == 14 &&
			trace.residentAttemptCount == 0 && trace.residentRangeCount == 0,
			"pre-admission plans retain actual source ordering and still require recorded acceptance before released work");
	}
}
// Source/consumer authorization fixture. Native helper and release-boundary
// integration is tested separately; these are modelled imported job facts.
struct CohortInput { unsigned count, values[3]; };
struct CohortCounts
{
	unsigned bodies = 0, fallbacks = 0, commits = 0;
	unsigned inputWrites = 0, outputWrites = 0, detached = 0;
};
struct CohortCanonicalInput { const CohortInput *input; CohortCounts *counts; };
struct CohortCanonicalOutput { const Output *output; CohortCounts *counts; };
bool writeCohortInput(KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const auto &view = *static_cast<const CohortCanonicalInput *>(context);
	++view.counts->inputWrites;
	if (!writer.sequence(1, view.input->count)) return false;
	for (unsigned i = 0; i != view.input->count; ++i)
		if (!writer.u32(2, view.input->values[i])) return false;
	return true;
}
bool writeCohortOutput(KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const auto &view = *static_cast<const CohortCanonicalOutput *>(context);
	++view.counts->outputWrites;
	return writer.u32(1, view.output->sum);
}
bool forbiddenCohortDetached(const void *context, void *)
{
	++static_cast<const CohortCanonicalInput *>(context)->counts->detached;
	return false;
}
unsigned cohortLegacyFallback(const CohortInput &input, CohortCounts &counts)
{
	++counts.fallbacks;
	unsigned sum = 0;
	for (unsigned i = 0; i != input.count; ++i) sum += input.values[i];
	return sum;
}
bool cohortAuthoritativeCommit(const Output &output, unsigned &authoritative, CohortCounts &counts,
	bool reject = false)
{
	++counts.commits;
	if (reject) return false;
	authoritative += output.sum;
	return true;
}

// Same arithmetic is called by record and consume. The counter is at the real
// helper entry, not an observer callback. This is a core authorization fixture,
// not a claim that the native ordinary-path helper has been exercised.
KernelPerformanceCheckpointProgress cohortPrefixBody(const CohortInput &input,
	const KernelPerformanceRangePlan &range, KernelPerformanceCheckpointProbe &probe,
	bool sourceCancels, bool invertActualPredicate, Output &output, CohortCounts &counts)
{
	++counts.bodies;
	unsigned poll = 1, completed = 0;
	KernelPerformanceCheckpoint last = { 11, 0, 0 };
	bool actualCancel = sourceCancels && poll == 3;
	bool cancelled = probe.cancelled(last, invertActualPredicate ? !actualCancel : actualCancel);
	while (!cancelled && completed != 192)
	{
		if (completed != 0 && completed % 64 == 0)
		{
			++poll;
			last = { 12, completed, 0 };
			actualCancel = sourceCancels && poll == 3;
			cancelled = probe.cancelled(last, invertActualPredicate ? !actualCancel : actualCancel);
			if (cancelled) break;
		}
		output.sum += input.values[static_cast<unsigned>(range.begin)];
		++completed;
	}
	if (!cancelled) last = { 13, 192, 0 };
	check(probe.finish(last, completed, cancelled ? KERNEL_RANGE_CANCELLED : KERNEL_RANGE_COMPLETED),
		"the actual helper finishes its observed poll sequence and prefix");
	return probe.snapshot();
}

struct CohortFixture
{
	SourceAbortFixture base;
	CohortInput empty = { 0, { 0, 0, 0 } };
	CohortInput committedInput = { 2, { 3, 4, 0 } };
	CohortInput abortedInput = { 3, { 3, 4, 5 } };
	KernelPerformanceAttemptIdentity identity(JobMetricCounter ordinal) const
	{
		auto value = base.identity(ordinal);
		value.subtype = 0; // Existing ordinary path stream; direct path is 1.
		return value;
	}
	KernelPerformanceRangePlan range(unsigned ordinal) const
	{
		auto value = base.firstRange;
		value.rangeOrdinal = ordinal;
		value.begin = ordinal; value.end = ordinal + 1;
		return value;
	}
	KernelPerformanceDispatchPlan abortedDispatch() const
	{
		auto value = base.dispatch;
		value.rangeCount = 3; value.operationCount = 3; value.sourceLimit = 3;
		return value;
	}
	KernelPerformanceAttemptFinish committed(KernelPerformanceReferenceBatch batch) const
	{
		KernelPerformanceAttemptFinish value = {};
		value.disposition = KERNEL_PERFORMANCE_COMMITTED;
		value.reasonSchema = 1; value.reason = 5; value.validatedBatch = batch;
		return value;
	}
};

struct CohortSource
{
	CohortCounts counts;
	unsigned authoritative = 0;
	KernelPerformanceReferenceSnapshot snapshot;
};
bool recordCohort(CohortFixture &fixture, CohortSource &result)
{
	KernelPerformanceReferenceLedger source;
	if (!source.beginRun(fixture.base.options)) return false;
	const auto a = source.beginAttempt(fixture.identity(0));
	if (!a.valid() || !source.observeDecision(a, fixture.base.rejected)) return false;
	result.authoritative += cohortLegacyFallback(fixture.empty, result.counts);
	if (!source.finishAttempt(a, fixture.base.notAdmitted) || !source.reapAttempt(a, fixture.base.reap)) return false;

	const auto s = source.beginAttempt(fixture.identity(1));
	const CohortCanonicalInput successfulInput = { &fixture.committedInput, &result.counts };
	if (!s.valid() || !source.bindCapturedInput(s, 1, 2, writeCohortInput, &successfulInput) ||
		!source.observeDecision(s, fixture.base.accepted) || !source.observeDispatch(s, fixture.base.dispatch)) return false;
	if (!source.observeRangePlan(s, fixture.range(0)) || !source.observeRangePlan(s, fixture.range(1))) return false;
	Output successfulOutput = {};
	for (unsigned i = 0; i != 2; ++i)
	{
		KernelPerformanceCheckpointProbe probe;
		if (!probe.beginRecord()) return false;
		KernelPerformanceRangeProgress released = {};
		released.checkpoint = cohortPrefixBody(fixture.committedInput, fixture.range(i), probe,
			false, false, successfulOutput, result.counts);
		released.publication = KERNEL_PUBLICATION_PUBLISHED;
		check(released.checkpoint.pollCount == 3 && released.checkpoint.completedWorkUnits == 192 &&
			released.checkpoint.terminal == KERNEL_RANGE_COMPLETED, "published source range has the literal completed prefix");
		if (!source.observeReleasedRange(s, fixture.range(i), released)) return false;
	}
	check(successfulOutput.sum == 1344, "source arithmetic produces independently calculated 192 times three plus four");
	const CohortCanonicalOutput outputView = { &successfulOutput, &result.counts };
	const auto validated = source.observeValidatedAttempt(s, writeCohortOutput, &outputView);
	check(validated.valid(), "traced source links the already produced successful output without recapturing input");
	if (!validated.valid()) return false;
	const bool committed = cohortAuthoritativeCommit(successfulOutput, result.authoritative, result.counts);
	if (!source.finishBatch(validated, committed) || !source.finishAttempt(s, fixture.committed(validated)) ||
		!source.reapAttempt(s, fixture.base.reap)) return false;

	const auto b = source.beginAttempt(fixture.identity(2));
	const CohortCanonicalInput abortedInput = { &fixture.abortedInput, &result.counts };
	if (!b.valid() || !source.bindCapturedInput(b, 1, 3, writeCohortInput, &abortedInput) ||
		!source.observeDecision(b, fixture.base.accepted) || !source.observeDispatch(b, fixture.abortedDispatch())) return false;
	for (unsigned i = 0; i != 3; ++i)
		if (!source.observeRangePlan(b, fixture.range(i))) return false;
	result.authoritative += cohortLegacyFallback(fixture.abortedInput, result.counts);
	if (!source.finishAttempt(b, fixture.base.aborted)) return false;
	const auto c = source.beginAttempt(fixture.identity(3));
	if (!c.valid() || !source.observeDecision(c, fixture.base.refused)) return false;
	result.authoritative += cohortLegacyFallback(fixture.empty, result.counts);
	if (!source.finishAttempt(c, fixture.base.notAdmitted) || !source.reapAttempt(c, fixture.base.reap) ||
		!source.sealObservationWindow()) return false;

	// Native release is external to this unit fixture. No groupComplete flag.
	Output discarded = {};
	for (unsigned i = 0; i != 3; ++i)
	{
		KernelPerformanceRangeProgress released = {};
		if (i != 1)
		{
			KernelPerformanceCheckpointProbe probe;
			if (!probe.beginRecord()) return false;
			released.checkpoint = cohortPrefixBody(fixture.abortedInput, fixture.range(i), probe,
				i == 0, false, discarded, result.counts);
			released.publication = KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL;
			check(released.checkpoint.completedWorkUnits == (i == 0 ? 128 : 192) &&
				released.checkpoint.firstTruePoll == (i == 0 ? 3 : 0),
				"late source import distinguishes cancelled prefix from completed-but-discarded work");
		}
		if (!source.observeReleasedRange(b, fixture.range(i), released)) return false;
	}
	if (!source.reapAttempt(b, fixture.base.reap) || !source.sealExecutionClosure()) return false;
	result.snapshot = source.freeze();
	return result.snapshot.complete && result.snapshot.trace.complete;
}

struct CohortRead
{
	const std::vector<unsigned char> *bytes;
	unsigned calls = 0;
	JobMetricCounter bytesRead = 0;
	bool fail = false;
	bool throws = false;
	KernelPerformanceReferenceLedger *reenter = 0;
	KernelPerformanceAttemptIdentity reentryIdentity = {};
	bool reentryRejected = false;
	static bool read(void *context, JobMetricCounter offset, unsigned char *destination, unsigned count)
	{
		auto &value = *static_cast<CohortRead *>(context);
		++value.calls;
		if (value.throws) throw std::runtime_error("exact source read fault");
		if (value.reenter != 0)
		{
			auto *ledger = value.reenter;
			value.reenter = 0;
			value.reentryRejected = !ledger->beginAttempt(value.reentryIdentity).valid();
		}
		if (value.fail || offset > value.bytes->size() || count > value.bytes->size() - offset) return false;
		memcpy(destination, value.bytes->data() + static_cast<std::size_t>(offset), count);
		value.bytesRead += count;
		return true;
	}
};
KernelPerformanceReferenceRunOptions consumeCohortOptions(const CohortFixture &fixture,
	const CohortSource &source, CohortRead &read)
{
	auto options = fixture.base.options;
	options.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	options.trace.mode = KERNEL_TRACE_CONSUME;
	options.trace.append = 0; options.trace.readAt = CohortRead::read; options.trace.context = &read;
	options.trace.sourceByteCount = source.snapshot.trace.byteCount;
	options.trace.sourceTraceDigest = source.snapshot.trace.digest;
	// Literal separately selected receipt identity. D/E supplies the actual
	// immutable receipt binding in native integration; this is not a file hash.
	options.trace.sourceReceiptDigest = fixture.base.options.trace.binding.nativeRunIdentity;
	options.trace.sourceReceiptDigest.bytes[0] = 0x80;
	return options;
}

struct CohortClock
{
	JobMetricCounter now = 100;
	unsigned reads = 0;
	static JobMetricCounter read(void *context)
	{ auto &value = *static_cast<CohortClock *>(context); ++value.reads; return value.now; }
};

void validatedDiscardRoundTripsWithoutPublishingAuthority()
{
	CohortFixture fixture;
	CohortSource recorded;
	KernelPerformanceReferenceLedger source;
	check(source.beginRun(fixture.base.options),
		"validated-discard source starts with an authenticated trace binding");
	const auto sourceAttempt = source.beginAttempt(fixture.identity(1));
	CohortCounts sourceCounts;
	CohortCanonicalInput sourceInput = { &fixture.committedInput, &sourceCounts };
	check(sourceAttempt.valid() &&
		source.bindCapturedInput(sourceAttempt, 1, 2, writeCohortInput, &sourceInput) &&
		source.observeDecision(sourceAttempt, fixture.base.accepted) &&
		source.observeDispatch(sourceAttempt, fixture.base.dispatch) &&
		source.observeRangePlan(sourceAttempt, fixture.range(0)) &&
		source.observeRangePlan(sourceAttempt, fixture.range(1)),
		"validated-discard source admits and plans both authenticated ranges");
	for (unsigned i = 0; i != 2; ++i)
	{
		KernelPerformanceCheckpointProbe probe;
		KernelPerformanceRangeProgress progress = {};
		const KernelPerformanceCheckpoint last = { 21, 1, i };
		check(probe.beginRecord() && probe.finish(last, 1, KERNEL_RANGE_COMPLETED),
			"validated-discard source records one completed body prefix");
		progress.checkpoint = probe.snapshot();
		progress.publication = KERNEL_PUBLICATION_PUBLISHED;
		check(source.observeReleasedRange(sourceAttempt, fixture.range(i), progress),
			"validated-discard source releases a completed authenticated range");
	}
	Output sourceOutput = { 7 };
	CohortCanonicalOutput sourceOutputView = { &sourceOutput, &sourceCounts };
	const auto sourceBatch = source.observeValidatedAttempt(sourceAttempt,
		writeCohortOutput, &sourceOutputView);
	KernelPerformanceAttemptFinish sourceFinish = fixture.base.aborted;
	sourceFinish.validatedBatch = sourceBatch;
	check(sourceBatch.valid() && source.finishBatch(sourceBatch, false) &&
		source.finishAttempt(sourceAttempt, sourceFinish) &&
		source.reapAttempt(sourceAttempt, fixture.base.reap) &&
		source.sealObservationWindow() && source.sealExecutionClosure(),
		"validated output may be safely discarded only after an explicit noncommit");
	recorded.snapshot = source.freeze();
	check(recorded.snapshot.complete && recorded.snapshot.trace.complete &&
		recorded.snapshot.streamCount == 1 &&
		recorded.snapshot.streams[0].validatedBatchCount == 1 &&
		recorded.snapshot.streams[0].committedBatchCount == 0 &&
		recorded.snapshot.streams[0].abortedBatchCount == 1,
		"validated-discard source keeps validation evidence without minting committed authority");
	if (!recorded.snapshot.complete || !recorded.snapshot.trace.complete)
		return;

	CohortRead read = { &fixture.base.sink.bytes };
	KernelPerformanceReferenceLedger baseline;
	check(baseline.beginRun(consumeCohortOptions(fixture, recorded, read)),
		"validated-discard consumer prevalidates the complete immutable source");
	CohortClock time;
	KernelPerformanceLedger timing;
	KernelPerformanceTimingRunOptions timingOptions;
	timingOptions.enabled = true;
	timingOptions.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	timingOptions.clock = CohortClock::read;
	timingOptions.clockContext = &time;
	check(timing.beginRun(timingOptions),
		"validated-discard consumer starts the serial baseline timing role");
	const KernelPerformanceSchedulerBoundary scheduler;
	++time.now;
	const auto frame = timing.beginFrame(1, 7, scheduler);
	for (unsigned phase = 0; phase != KERNEL_PHASE_SPATIAL_WORK; ++phase)
	{
		++time.now;
		check(timing.beginPhase(frame,
			static_cast<KernelPerformancePhase>(phase)),
			"validated-discard consumer enters a preceding simulation phase");
		++time.now;
		check(timing.endPhase(frame,
			static_cast<KernelPerformancePhase>(phase)),
			"validated-discard consumer closes a preceding simulation phase");
	}
	++time.now;
	check(timing.beginPhase(frame, KERNEL_PHASE_SPATIAL_WORK),
		"validated-discard consumer enters the owning simulation phase");
	const auto attempt = baseline.beginAttempt(fixture.identity(1));
	CohortCounts counts;
	CohortCanonicalInput inputView = { &fixture.committedInput, &counts };
	KernelPerformanceAttemptDecision decision = {};
	check(attempt.valid() &&
		baseline.bindCapturedInput(attempt, 1, 2, writeCohortInput, &inputView) &&
		baseline.replayDecision(attempt, fixture.base.accepted.site, true,
			fixture.base.accepted.deterministicFacts, decision) &&
		decision.admission == KERNEL_ADMISSION_ACCEPTED &&
		baseline.observeDispatch(attempt, fixture.base.dispatch) &&
		baseline.observeRangePlan(attempt, fixture.range(0)) &&
		baseline.observeRangePlan(attempt, fixture.range(1)),
		"validated-discard consumer reconstructs source admission and partition");
	KernelPerformanceAttemptFinish expected = {};
	check(baseline.readSourceFinish(attempt, expected) &&
		expected.disposition == KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION &&
		expected.validationObserved,
		"source lookahead distinguishes validated discard from an unvalidated abort");
	for (unsigned i = 0; i != 2; ++i)
	{
		KernelPerformanceInlineBody body;
		KernelPerformanceCheckpointProbe probe;
		const auto action = baseline.beginInlineBody(attempt, fixture.range(i),
			timing, body, probe);
		KernelPerformanceRangeProgress progress = {};
		const KernelPerformanceCheckpoint last = { 21, 1, i };
		check(action == KERNEL_INLINE_EXECUTE &&
			probe.finish(last, 1, KERNEL_RANGE_COMPLETED),
			"validated-discard consumer executes each authenticated body once");
		progress.checkpoint = probe.snapshot();
		progress.publication = KERNEL_PUBLICATION_PUBLISHED;
		check(baseline.finishInlineBody(body, progress) &&
			baseline.observeReleasedRange(attempt, fixture.range(i), progress),
			"validated-discard consumer matches each source completion prefix");
	}
	Output output = { 7 };
	CohortCanonicalOutput outputView = { &output, &counts };
	const auto batch = baseline.observeValidatedAttempt(attempt,
		writeCohortOutput, &outputView);
	KernelPerformanceAttemptFinish finish = fixture.base.aborted;
	finish.validatedBatch = batch;
	check(batch.valid() && baseline.finishBatch(batch, false) &&
		baseline.finishAttempt(attempt, finish) &&
		baseline.reapAttempt(attempt, fixture.base.reap),
		"validated-discard consumer links the digest and reproduces the noncommit");
	++time.now;
	check(timing.endPhase(frame, KERNEL_PHASE_SPATIAL_WORK) &&
		(++time.now, timing.beginPhase(frame, KERNEL_PHASE_OWNER_TAIL)) &&
		(++time.now, timing.endPhase(frame, KERNEL_PHASE_OWNER_TAIL)) &&
		(++time.now, timing.beginPhase(frame, KERNEL_PHASE_VERIFICATION_PUBLICATION)) &&
		(++time.now, timing.endPhase(frame, KERNEL_PHASE_VERIFICATION_PUBLICATION)) &&
		(++time.now, timing.endFrame(frame, 7, scheduler)) && timing.sealAdmissions() &&
		baseline.sealObservationWindow() && baseline.sealExecutionClosure() &&
		timing.sealExecutionClosure(scheduler),
		"validated-discard consumer closes timing and retained attempt state");
	const auto consumed = baseline.freeze();
	check(consumed.complete && consumed.trace.complete && consumed.streamCount == 1 &&
		consumed.streams[0].validatedBatchCount == 1 &&
		consumed.streams[0].committedBatchCount == 0 &&
		consumed.streams[0].abortedBatchCount == 1 &&
		consumed.trace.digest.equals(recorded.snapshot.trace.digest),
		"validated discard round-trips without publishing authoritative output");
}
bool replayCohortDecision(KernelPerformanceReferenceLedger &baseline, KernelPerformanceAttempt attempt,
	const KernelPerformanceAttemptDecision &expected)
{
	KernelPerformanceAttemptDecision inherited = {};
	if (!baseline.replayDecision(attempt, expected.site, expected.deterministicEligible,
		expected.deterministicFacts, inherited)) return false;
	check(inherited.admission == expected.admission && inherited.sourceConfiguredWorkers == 4 &&
		inherited.activeSlots == expected.activeSlots && inherited.dynamicFactsKnownMask == expected.dynamicFactsKnownMask,
		"baseline inherits authenticated dynamic source facts without consulting an empty physical queue");
	return true;
}
void checkCohortCounts(const KernelPerformanceReferenceSnapshot &snapshot, const CohortCounts &counts,
	unsigned authoritative)
{
	const auto &trace = snapshot.trace;
	check(snapshot.complete && trace.complete && snapshot.errors == 0 && trace.errors == 0,
		"complete cohort closes both canonical and source attempt evidence");
	check(trace.recordCount == 34 && trace.logicalEventCount == 34 && trace.coalescedSpanCount == 0 &&
		trace.coalescedAttemptCount == 0, "literal cohort records exactly thirty-four full owner events including envelopes");
	check(trace.attemptCount == 4 && trace.admittedAttemptCount == 2 && trace.notAdmittedAttemptCount == 2 &&
		trace.abortedAfterAdmissionAttemptCount == 1 && trace.reapCount == 4 &&
		trace.residentAttemptCount == 0 && trace.residentAttemptHighWater == 2,
		"cohort separates four attempts two admissions one success one retained abort and independent C reap");
	check(trace.capturedAttemptCount == 2 && trace.capturedOperationCount == 5 && trace.dispatchCount == 2 &&
		trace.rangeCount == 5 && trace.releasedRangeCount == 5 && trace.residentRangeCount == 0 &&
		trace.residentRangeHighWater == 3, "cohort operation counts do not become range or batch counts");
	check(snapshot.streamCount == 1 && snapshot.streams[0].validatedBatchCount == 1 &&
		snapshot.streams[0].committedBatchCount == 1 && snapshot.streams[0].validatedOperationCount == 2 &&
		snapshot.streams[0].committedOperationCount == 2, "only S contributes successful canonical output and one commit");
	check(authoritative == 1356 && counts.bodies == 4 && counts.fallbacks == 3 && counts.commits == 1 &&
		counts.inputWrites == 2 && counts.outputWrites == 1 && counts.detached == 0,
		"actual helper entries fallback effects callback cardinality and single commit match independent literals");
}

void sourceConsumerCohortExecutesEachBodyOnceWithExactPartition(bool slowerBodies = false,
	bool changeSourceBeforeFreeze = false)
{
	// Breaks: no consume; rehash input; detached rerun; early B release; source
	// wall-time cut; discarded work pure; materialization only partly serial.
	CohortFixture fixture; CohortSource source;
	const bool sourceComplete = recordCohort(fixture, source);
	check(sourceComplete, "source cohort records success plus modelled imported abort progress before baseline initialization");
	if (!sourceComplete) return; // No successful setup fabricated by the test.
	checkCohortCounts(source.snapshot, source.counts, source.authoritative);
	CohortRead read = { &fixture.base.sink.bytes };
	KernelPerformanceReferenceLedger baseline;
	const bool consumerStarted = baseline.beginRun(consumeCohortOptions(fixture, source, read));
	check(consumerStarted, "consumer validates the complete selected source before any helper execution");
	if (!consumerStarted) return;
	CohortClock time; KernelPerformanceLedger timing;
	KernelPerformanceTimingRunOptions timingOptions;
	timingOptions.enabled = true; timingOptions.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	timingOptions.clock = CohortClock::read; timingOptions.clockContext = &time;
	check(timing.beginRun(timingOptions), "cohort uses the existing timing ledger in owner baseline role");
	const KernelPerformanceSchedulerBoundary scheduler;
	const auto frame = timing.beginFrame(1, 7, scheduler);
	CohortCounts counts; unsigned authoritative = 0;
	const auto a = baseline.beginAttempt(fixture.identity(0));
	check(a.valid() && replayCohortDecision(baseline, a, fixture.base.rejected), "baseline reproduces A before capture");
	authoritative += cohortLegacyFallback(fixture.empty, counts);
	check(baseline.finishAttempt(a, fixture.base.notAdmitted) && baseline.reapAttempt(a, fixture.base.reap),
		"A consumes its actual serial fallback and synchronous reap");
	time.now = 110; check(timing.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "intake begins at literal110");
	time.now = 120; check(timing.endPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "intake ends at literal120");
	time.now = 125; check(timing.beginPhase(frame, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND), "mutable phase begins at125");
	time.now = 145; check(timing.endPhase(frame, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND), "mutable phase ends at145");
	time.now = 150; check(timing.beginPhase(frame, KERNEL_PHASE_SPATIAL_WORK), "spatial phase begins at150");

	const auto s = baseline.beginAttempt(fixture.identity(1));
	CohortCanonicalInput inputView = { &fixture.committedInput, &counts };
	check(s.valid() && baseline.bindCapturedInput(s, 1, 2, writeCohortInput, &inputView) &&
		replayCohortDecision(baseline, s, fixture.base.accepted) && baseline.observeDispatch(s, fixture.base.dispatch),
		"baseline independently captures S and reconstructs its source partition");
	check(baseline.observeRangePlan(s, fixture.range(0)) && baseline.observeRangePlan(s, fixture.range(1)),
		"both independently reconstructed source range plans are bound before any body");
	KernelPerformanceAttemptFinish expectedFinish = {};
	check(baseline.readSourceFinish(s, expectedFinish) && baseline.readSourceFinish(s, expectedFinish) &&
		expectedFinish.disposition == KERNEL_PERFORMANCE_COMMITTED && !expectedFinish.validatedBatch.valid() && counts.bodies == 0,
		"immutable lookahead supplies disposition without a local commit token body call or cursor advance");
	const JobMetricCounter lookupBytesBefore = read.bytesRead;
	for (unsigned repeat = 0; repeat != 64; ++repeat)
	{
		KernelPerformanceDispatchPlan dispatch = {}; KernelPerformanceRangePlan range = {};
		check(baseline.readSourceDispatch(s, 0, dispatch) && baseline.readSourceRange(s, 0, 0, range) &&
			dispatch.rangeCount == 2 && dispatch.sourceGrain == 1 && range.begin == 0 && range.end == 1,
			"repeated bounded lookahead retains independently expected plan fields without consuming them");
	}
	check(read.bytesRead - lookupBytesBefore <= 2 * source.snapshot.trace.byteCount + 64 * 512 && counts.bodies == 0,
		"repeated lookahead reuses live-attempt offsets instead of rescanning the complete source per query");
	Output successfulOutput = {};
	for (unsigned i = 0; i != 2; ++i)
	{
		time.now = i == 0 ? 160 : 185;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		const auto action = baseline.beginInlineBody(s, fixture.range(i), timing, body, probe);
		check(action == KERNEL_INLINE_EXECUTE && body.valid(), "one authenticated source-published S range may execute");
		if (action != KERNEL_INLINE_EXECUTE) return;
		if (i == 0)
		{
			time.now = 165;
			const auto serial = baseline.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
			check(serial.valid(), "only an active authenticated owner body can open materialization serial extent");
			time.now = 170;
			check(baseline.endInlineOwnerSerial(serial), "balanced materialization resumes the original committed-body category");
		}
		KernelPerformanceRangeProgress actual = {};
		actual.checkpoint = cohortPrefixBody(fixture.committedInput, fixture.range(i), probe,
			false, true, successfulOutput, counts); // Opposite actual predicate cannot create cancellation.
		actual.publication = KERNEL_PUBLICATION_PUBLISHED;
		time.now = i == 0 ? (slowerBodies ? 185 : 180) : (slowerBodies ? 210 : 205);
		check(baseline.finishInlineBody(body, actual) && baseline.observeReleasedRange(s, fixture.range(i), actual),
			"S closes pure timing before importing and comparing actual progress");
	}
	const CohortCanonicalOutput outputView = { &successfulOutput, &counts };
	const auto validated = baseline.observeValidatedAttempt(s, writeCohortOutput, &outputView);
	check(validated.valid(), "baseline validates only its once-produced output without detached serial compute");
	const bool committed = cohortAuthoritativeCommit(successfulOutput, authoritative, counts);
	check(baseline.finishBatch(validated, committed) && baseline.finishAttempt(s, fixture.committed(validated)) &&
		baseline.reapAttempt(s, fixture.base.reap), "S reconciles exactly one real owner commit and final source linkage");

	const auto b = baseline.beginAttempt(fixture.identity(2));
	inputView.input = &fixture.abortedInput;
	check(b.valid() && baseline.bindCapturedInput(b, 1, 3, writeCohortInput, &inputView) &&
		replayCohortDecision(baseline, b, fixture.base.accepted) && baseline.observeDispatch(b, fixture.abortedDispatch()),
		"B retains independently captured immutable input for source-matched late work");
	for (unsigned i = 0; i != 3; ++i) check(baseline.observeRangePlan(b, fixture.range(i)), "B retains each actual source range");
	time.now = 210; authoritative += cohortLegacyFallback(fixture.abortedInput, counts);
	time.now = 220; check(baseline.finishAttempt(b, fixture.base.aborted), "B fallback finishes before late range execution");
	const auto c = baseline.beginAttempt(fixture.identity(3));
	check(c.valid() && replayCohortDecision(baseline, c, fixture.base.refused), "C inherits busy refusal while B remains retained");
	time.now = 225; authoritative += cohortLegacyFallback(fixture.empty, counts);
	time.now = 230; check(baseline.finishAttempt(c, fixture.base.notAdmitted) && baseline.reapAttempt(c, fixture.base.reap),
		"C closes only its own fallback and reap");
	time.now = 250; check(timing.endPhase(frame, KERNEL_PHASE_SPATIAL_WORK), "spatial phase ends at250");
	time.now = 255; check(timing.beginPhase(frame, KERNEL_PHASE_OWNER_TAIL), "owner tail begins at255");
	time.now = 265; check(timing.endPhase(frame, KERNEL_PHASE_OWNER_TAIL), "owner tail ends at265");
	time.now = 270; check(timing.beginPhase(frame, KERNEL_PHASE_VERIFICATION_PUBLICATION), "verification begins at270");
	time.now = 290; check(timing.endPhase(frame, KERNEL_PHASE_VERIFICATION_PUBLICATION), "verification ends at290");
	time.now = 300; check(timing.endFrame(frame, 7, scheduler) && timing.sealAdmissions() &&
		baseline.sealObservationWindow(), "completed source window seals ingress while B survives");
	check(counts.bodies == 2 && counts.fallbacks == 3, "source late bodies have not been pulled before their owner release event");

	time.now = 400; const auto completion = timing.beginCompletionSerial();
	Output discarded = {};
	for (unsigned i = 0; i != 3; ++i)
	{
		if (i == 0) time.now = 410;
		if (i == 2) time.now = 435;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		const auto action = baseline.beginInlineBody(b, fixture.range(i), timing, body, probe);
		KernelPerformanceRangeProgress actual = {};
		if (i == 1)
		{
			check(action == KERNEL_INLINE_SKIP_SOURCE_NEVER_ENTERED && !body.valid() && !probe.snapshot().entered,
				"never-entered source range acknowledges once with no executable body or timing sample");
		}
		else
		{
			check(action == KERNEL_INLINE_EXECUTE && body.valid(), "late source body executes once in completion serial extent");
			if (action != KERNEL_INLINE_EXECUTE) return;
			if (i == 2)
			{
				time.now = 438;
				const auto serial = baseline.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
				check(serial.valid(), "completed-discarded body may enter actual materialization only as serial work");
				time.now = 443;
				check(baseline.endInlineOwnerSerial(serial), "discarded body's enclosing category remains serial on resume");
			}
			actual.checkpoint = cohortPrefixBody(fixture.abortedInput, fixture.range(i), probe,
				i == 0, true, discarded, counts);
			actual.publication = KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL;
			time.now = i == 0 ? 430 : 455;
			check(baseline.finishInlineBody(body, actual), "cancelled and completed-discarded bodies close wholly serial");
			check(actual.checkpoint.completedWorkUnits == (i == 0 ? 128 : 192),
				"baseline opposite actual predicates still reproduce the exact authenticated source prefix");
		}
		check(baseline.observeReleasedRange(b, fixture.range(i), actual), "late progress is compared at its source owner event");
	}
	check(baseline.reapAttempt(b, fixture.base.reap) && baseline.sealExecutionClosure(),
		"all three B range acknowledgements precede its group-terminal reference reap");
	time.now = 470;
	check(timing.endCompletionSerial(completion) && timing.sealExecutionClosure(scheduler),
		"simulation-owned late work ends before independent scheduler closure");
	if (changeSourceBeforeFreeze) fixture.base.sink.bytes[0] ^= 1;
	const auto consumed = baseline.freeze(); const auto timed = timing.freeze();
	if (changeSourceBeforeFreeze)
	{
		check(!consumed.trace.complete && (consumed.trace.errors & KERNEL_REFERENCE_ERROR_TRACE_BINDING) != 0 &&
			baseline.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"completed consumer rechecks immutable source bytes at freeze and cannot qualify a changed artifact");
		return;
	}
	checkCohortCounts(consumed, counts, authoritative);
	check(consumed.trace.digest.equals(source.snapshot.trace.digest) && consumed.trace.byteCount == source.snapshot.trace.byteCount &&
		consumed.streams[0].inputDigest.equals(source.snapshot.streams[0].inputDigest) &&
		consumed.streams[0].outputDigest.equals(source.snapshot.streams[0].outputDigest) &&
		consumed.streams[0].commitDigest.equals(source.snapshot.streams[0].commitDigest) &&
		consumed.trace.sourceReceiptDigest.equals(consumeCohortOptions(fixture, source, read).trace.sourceReceiptDigest),
		"consumer reproduces source content and all existing successful canonical streams");
	const auto &phase = timed.phaseAccounting;
	check(phase.complete && phase.frameNanoseconds == 200 && phase.unscopedSerialNanoseconds == 40 &&
		phase.completionSerialNanoseconds == 70 && phase.completionSampleCount == 1,
		"frame gap and late completion are independently measured without counting the excluded wall gap");
	check(phase.phases[KERNEL_PHASE_SPATIAL_WORK].totalNanoseconds == 100 &&
		phase.phases[KERNEL_PHASE_SPATIAL_WORK].pureNanoseconds == (slowerBodies ? 45 : 35) &&
		phase.phases[KERNEL_PHASE_SPATIAL_WORK].serialNanoseconds == (slowerBodies ? 55 : 65),
		"two real committed body extents minus whole materialization serial extent give exactly thirty-five pure ticks");
	JobMetricCounter phaseTotal = 0, pure = 0, serial = 0;
	for (unsigned i = 0; i != KERNEL_PHASE_COUNT; ++i)
	{
		phaseTotal += phase.phases[i].totalNanoseconds;
		pure += phase.phases[i].pureNanoseconds;
		serial += phase.phases[i].serialNanoseconds;
	}
	check(phaseTotal == 160 && serial == (slowerBodies ? 115 : 125) && pure == (slowerBodies ? 45 : 35) &&
		phase.frameNanoseconds + phase.completionSerialNanoseconds == 270 &&
		serial + phase.unscopedSerialNanoseconds + phase.completionSerialNanoseconds == (slowerBodies ? 225 : 235),
		"independent literal total270 serial235 pure35 reconcile without assigning totals from their expected sums");
	check(baseline.mode() == KERNEL_REFERENCE_DISABLED && baseline.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
		"frozen baseline retains execution identity independently of active diagnostics");
}

void sourceConsumerRejectsBrokenBindingsBeforeAnyBody()
{
	// Break: source preflight trusts external metadata, failed read, or an
	// unbound header. Valid-source successful round trip above is the control.
	CohortFixture fixture; CohortSource source;
	const bool recorded = recordCohort(fixture, source);
	check(recorded, "binding failure matrix has one real complete source control");
	if (!recorded) return;
	for (unsigned mutation = 0; mutation != 19; ++mutation)
	{
		CohortRead read = { &fixture.base.sink.bytes };
		auto options = consumeCohortOptions(fixture, source, read);
		switch (mutation)
		{
		case 0: --options.trace.sourceByteCount; break;
		case 1: ++options.trace.sourceByteCount; break;
		case 2: options.trace.sourceTraceDigest.bytes[0] ^= 1; break;
		case 3: options.trace.binding.nativeRunIdentity.bytes[0] ^= 1; break;
		case 4: options.trace.binding.executable.bytes[0] ^= 1; break;
		case 5: options.trace.binding.fixture.bytes[0] ^= 1; break;
		case 6: options.trace.binding.sourcePolicy.bytes[0] ^= 1; break;
		case 7: ++options.trace.limits.maximumRanges; break;
		case 8: ++options.trace.residentRangeCapacity; break;
		case 9: read.fail = true; break;
		case 10: ++options.trace.limits.maximumBytes; break;
		case 11: ++options.trace.limits.maximumRecords; break;
		case 12: ++options.trace.limits.maximumLogicalEvents; break;
		case 13: ++options.trace.limits.maximumAttempts; break;
		case 14: ++options.trace.residentAttemptCapacity; break;
		case 15: options.trace.sourceReceiptDigest.valid = false; break;
		case 16: options.trace.sourceTraceDigest.valid = false; break;
		case 17: read.throws = true; break;
		case 18: options.trace.readAt = 0; break;
		}
		KernelPerformanceReferenceLedger rejected;
		check(!rejected.beginRun(options), "changed complete-source binding or transport fails before attempt/body admission");
		check(!rejected.beginAttempt(fixture.identity(0)).valid() && !rejected.freeze().trace.complete,
			"a failed source preflight cannot expose a collectable or complete consumer run");
	}
}

// This utility hashes actual test artifact bytes directly. It never calls the
// production canonical writer/parser to manufacture an expected artifact hash.
KernelPerformanceDigest independentArtifactHash(const std::vector<unsigned char> &bytes)
{
	KernelPerformanceDigest result;
	BCRYPT_ALG_HANDLE algorithm = 0; BCRYPT_HASH_HANDLE hash = 0;
	if (bytes.size() > ~0U || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0) < 0) return result;
	if (BCryptCreateHash(algorithm, &hash, 0, 0, 0, 0, 0) >= 0)
	{
		if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
			BCryptFinishHash(hash, result.bytes, 32, 0) >= 0) result.valid = true;
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return result;
}
struct ArtifactField { std::size_t offset; unsigned type, tag; JobMetricCounter value; };
std::vector<ArtifactField> knownArtifactFields(const std::vector<unsigned char> &bytes)
{
	// Test-only navigation of an already valid fixture, not a replacement V1
	// validator. Literal canonical prefix is 24; widths are protocol literals.
	std::vector<ArtifactField> fields;
	for (std::size_t offset = 24; offset < bytes.size();)
	{
		if (bytes.size() - offset < 5) return {};
		ArtifactField field = { offset, bytes[offset], 0, 0 };
		for (unsigned i = 0; i != 4; ++i) field.tag |= static_cast<unsigned>(bytes[offset + 1 + i]) << (8 * i);
		const unsigned width = field.type == 3 ? 8 : (field.type == 5 ? 1 : 4);
		if (field.type == 0 || field.type > 6 || bytes.size() - offset - 5 < width) return {};
		for (unsigned i = 0; i != width; ++i) field.value |= static_cast<JobMetricCounter>(bytes[offset + 5 + i]) << (8 * i);
		fields.push_back(field); offset += 5 + width;
	}
	return fields;
}
std::size_t artifactField(const std::vector<unsigned char> &bytes, unsigned kind,
	unsigned occurrence, unsigned tag, unsigned type)
{
	const auto fields = knownArtifactFields(bytes);
	bool selected = false;
	for (std::size_t i = 0; i != fields.size(); ++i)
	{
		const auto &field = fields[i];
		if (field.type == 1 && field.tag == 1 && i + 1 < fields.size() &&
			fields[i + 1].type == 3 && fields[i + 1].tag == 2)
		{
			if (selected) break;
			if (field.value == kind)
			{
				if (occurrence == 0) selected = true; else --occurrence;
			}
		}
		if (selected && field.tag == tag && field.type == type) return field.offset;
	}
	return bytes.size();
}
std::size_t artifactRecordEnd(const std::vector<unsigned char> &bytes, std::size_t start)
{
	const auto fields = knownArtifactFields(bytes);
	for (std::size_t i = 0; i + 1 < fields.size(); ++i)
		if (fields[i].offset > start && fields[i].type == 1 && fields[i].tag == 1 &&
			fields[i + 1].type == 3 && fields[i + 1].tag == 2) return fields[i].offset;
	return bytes.size();
}
void sourceSuccessWireHasOneLiteralLinkAndFinalFooter()
{
	// Break: success metadata exists only in snapshots, its digest uses stream
	// output rather than the captured batch output, or the final V1 footer is
	// silently interpreted as either the old or the new shape.
	CohortFixture fixture; CohortSource source;
	const bool recorded = recordCohort(fixture, source);
	check(recorded, "literal success wire fixture records a complete source cohort");
	if (!recorded) return;
	const auto &bytes = fixture.base.sink.bytes;
	const unsigned char link[] = {
		5, 9, 0, 0, 0, 1,
		1, 10, 0, 0, 0, 1, 0, 0, 0,
		3, 11, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 12, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		6, 13, 0, 0, 0, 4, 0, 0, 0,
		// SHA-256 independently computed from literal schema1 canonical bytes:
		// "RTS-KERNEL-FIELDS-v1", 01 00 00 00, type1/tag1/value1344.
		3, 14, 0, 0, 0, 0x33, 0x9f, 0x86, 0x72, 0x67, 0x2b, 0xdc, 0xea,
		3, 15, 0, 0, 0, 0xa3, 0x47, 0xce, 0x00, 0xeb, 0xe2, 0x0d, 0x60,
		3, 16, 0, 0, 0, 0xca, 0xfa, 0x8c, 0x31, 0xac, 0x8b, 0xeb, 0x35,
		3, 17, 0, 0, 0, 0xaa, 0x3b, 0xb9, 0xd2, 0x3d, 0xbc, 0x78, 0x4c,
		5, 18, 0, 0, 0, 1
	};
	const auto linked = artifactField(bytes, 9, 1, 9, 5);
	check(linked <= bytes.size() && bytes.size() - linked >= sizeof(link) &&
		memcmp(bytes.data() + linked, link, sizeof(link)) == 0 &&
		artifactRecordEnd(bytes, artifactField(bytes, 9, 1, 1, 1)) == linked + sizeof(link),
		"successful attempt finish carries exactly the literal schema count ordinal digest and actual-commit suffix");
	const unsigned char footer[] = {
		3, 23, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 24, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 25, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 26, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	check(bytes.size() >= sizeof(footer) &&
		memcmp(bytes.data() + bytes.size() - sizeof(footer), footer, sizeof(footer)) == 0,
		"final footer binds independently literal one validated/committed batch and two operations");
}
void sourceConsumerParsesGrammarEvenWhenArtifactHashMatches()
{
	// Break: checking the externally supplied SHA instead of parsing exact V1
	// structure, lifecycle, single linkage and final counter reconciliation.
	CohortFixture fixture; CohortSource source;
	const bool recorded = recordCohort(fixture, source);
	check(recorded, "grammar mutation matrix starts from one complete real source-writer artifact");
	if (!recorded) return;
	check(independentArtifactHash(fixture.base.sink.bytes).equals(source.snapshot.trace.digest),
		"independent raw-byte SHA confirms the source writer's complete artifact");
	struct ScalarMutation { unsigned kind, occurrence, tag, type; unsigned byte, replacement; };
	const ScalarMutation mutations[] = {
		{ 1, 0, 3, 1, 5, 2 }, // unknown trace version
		{ 3, 0, 4, 1, 0, 2 }, // signed type substituted for work kind
		{ 3, 0, 4, 1, 1, 99 }, // unknown tag
		{ 1, 0, 11, 6, 5, 3 }, // wrong binding digest sequence count
		{ 3, 0, 1, 1, 5, 99 }, // unknown record kind
		{ 3, 0, 2, 3, 5, 1 }, // duplicate/reordered record ordinal
		{ 6, 0, 7, 1, 5, 0 }, // zero planned range count
		{ 7, 0, 5, 1, 5, 1 }, // first range ordinal is not zero
		{ 9, 1, 9, 5, 5, 2 }, // noncanonical has-link boolean
		{ 9, 1, 13, 6, 5, 3 }, // truncated output digest grammar
		{ 9, 1, 18, 5, 5, 0 }, // committed finish with failed actual commit
		{ 9, 1, 11, 3, 5, 3 }, // validated operation count differs from capture
		{ 8, 0, 22, 1, 5, 2 }, // successful required range was discarded, not published
		{ 8, 0, 21, 1, 5, 3 }, // successful required range failed
		{ 10, 3, 3, 3, 5, 1 }, // B reap redirected to already reaped A
		{ 11, 0, 5, 3, 5, 3 }, // wrong total attempts
		{ 11, 0, 19, 3, 5, 4 }, // wrong range count
		{ 11, 0, 20, 3, 5, 4 }, // wrong released count
		{ 11, 0, 14, 5, 5, 0 }, // missing observation closure
		{ 11, 0, 15, 5, 5, 0 }, // missing execution closure
		{ 11, 0, 23, 3, 5, 2 }, // wrong validated batches
		{ 11, 0, 24, 3, 5, 2 }, // wrong committed batches
		{ 11, 0, 25, 3, 5, 3 }, // wrong validated operations
		{ 11, 0, 26, 3, 5, 3 }, // wrong committed operations
		{ 11, 0, 27, 3, 5, 1 }, // invented explicit window boundary
		{ 11, 0, 28, 3, 5, 1 }, // invented completed window
		{ 11, 0, 29, 3, 5, 1 }  // invented control window
	};
	for (const auto &mutation : mutations)
	{
		auto bytes = fixture.base.sink.bytes;
		const auto offset = artifactField(bytes, mutation.kind, mutation.occurrence, mutation.tag, mutation.type);
		check(offset < bytes.size(), "literal mutation addresses its expected source record field");
		if (offset == bytes.size()) continue;
		bytes[offset + mutation.byte] = static_cast<unsigned char>(mutation.replacement);
		CohortRead read = { &bytes };
		auto options = consumeCohortOptions(fixture, source, read);
		options.trace.sourceTraceDigest = independentArtifactHash(bytes);
		KernelPerformanceReferenceLedger consumer;
		check(options.trace.sourceTraceDigest.valid && !consumer.beginRun(options),
			"valid artifact SHA cannot authenticate changed grammar lifecycle or footer totals");
		check(!consumer.freeze().trace.complete, "malformed source never becomes complete consumed evidence");
	}
	for (unsigned mutation = 0; mutation != 8; ++mutation)
	{
		auto bytes = fixture.base.sink.bytes;
		const std::size_t start = artifactField(bytes, mutation == 4 ? 11 : 9, mutation == 4 ? 0 : 1, 1, 1);
		const std::size_t end = artifactRecordEnd(bytes, start);
		check(start < end && end <= bytes.size(), "structural mutation selects the complete expected record");
		if (start >= end || end > bytes.size()) continue;
		if (mutation == 0) bytes.erase(bytes.begin() + start, bytes.begin() + end); // missing finish
		if (mutation == 1)
		{
			const std::vector<unsigned char> duplicate(bytes.begin() + start, bytes.begin() + end);
			bytes.insert(bytes.begin() + end, duplicate.begin(), duplicate.end());
		}
		if (mutation == 2)
		{
			const std::size_t nextEnd = artifactRecordEnd(bytes, end);
			std::rotate(bytes.begin() + start, bytes.begin() + end, bytes.begin() + nextEnd); // reap before finish
		}
		if (mutation == 3) bytes.push_back(0); // trailing data
		if (mutation == 4) bytes.erase(bytes.begin() + start, bytes.end()); // missing footer
		if (mutation == 5) bytes.pop_back(); // truncated final scalar
		if (mutation == 6) bytes.resize(bytes.size() - 91); // obsolete footer without tags23..29
		if (mutation == 7) bytes.resize(bytes.size() - 39); // obsolete footer without tags27..29
		CohortRead read = { &bytes }; auto options = consumeCohortOptions(fixture, source, read);
		options.trace.sourceByteCount = bytes.size(); options.trace.sourceTraceDigest = independentArtifactHash(bytes);
		KernelPerformanceReferenceLedger consumer;
		check(options.trace.sourceTraceDigest.valid && !consumer.beginRun(options),
			"missing repeated reordered truncated extra or obsolete records fail the single final V1 grammar");
	}
	CohortRead reentrant = { &fixture.base.sink.bytes }; KernelPerformanceReferenceLedger consumer;
	reentrant.reenter = &consumer; reentrant.reentryIdentity = fixture.identity(0);
	check(!consumer.beginRun(consumeCohortOptions(fixture, source, reentrant)) && reentrant.reentryRejected &&
		!consumer.freeze().trace.complete, "source transport reentry cannot admit an attempt while immutable preflight is active");
}

void sourceConsumerRejectsEnteredRangeWithNotApplicablePublication()
{
	// Break: N/A is accepted for entered aborted work because no successful
	// linkage later requires these ranges to have a published outcome.
	CohortFixture fixture; CohortSource source;
	const bool recorded = recordCohort(fixture, source);
	check(recorded, "entered N/A mutation starts from a complete real source artifact");
	if (!recorded) return;
	const unsigned occurrences[] = { 2, 4 }; // B cancelled, then completed/discarded.
	for (unsigned occurrence : occurrences)
	{
		auto bytes = fixture.base.sink.bytes;
		const auto entered = artifactField(bytes, 8, occurrence, 10, 5);
		const auto publication = artifactField(bytes, 8, occurrence, 22, 1);
		const unsigned char discarded[] = { 1, 22, 0, 0, 0, 2, 0, 0, 0 };
		const bool located = entered < bytes.size() && bytes.size() - entered >= 6 && bytes[entered + 5] == 1 &&
			publication < bytes.size() && bytes.size() - publication >= sizeof(discarded) &&
			memcmp(bytes.data() + publication, discarded, sizeof(discarded)) == 0;
		check(located, "literal mutation selects an entered non-published B range");
		if (!located) continue;
		bytes[publication + 5] = 0; // Literal reserved NOT_APPLICABLE; all other bytes stay unchanged.
		CohortRead read = { &bytes };
		auto options = consumeCohortOptions(fixture, source, read);
		options.trace.sourceTraceDigest = independentArtifactHash(bytes);
		check(options.trace.sourceTraceDigest.valid && !options.trace.sourceTraceDigest.equals(source.snapshot.trace.digest),
			"independent raw-byte SHA authenticates the changed N/A artifact, not the original bytes");
		KernelPerformanceReferenceLedger consumer;
		check(!consumer.beginRun(options),
			"entered cancelled or completed-discarded N/A publication fails source prevalidation despite its matching SHA");
		const auto snapshot = consumer.freeze();
		check(!snapshot.trace.complete && (snapshot.trace.errors & KERNEL_REFERENCE_ERROR_CHECKPOINT) != 0 &&
			consumer.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"malformed entered publication keeps checkpoint failure sticky without unlatching baseline identity");
	}
}

struct ConsumerAtSuccessfulBody
{
	CohortFixture fixture; CohortSource source; CohortRead read;
	KernelPerformanceReferenceLedger reference; KernelPerformanceLedger timing;
	CohortClock time; CohortCounts counts; Output output = {}; unsigned authoritative = 0;
	KernelPerformanceAttempt a, s, b; KernelPerformanceFrame frame;
	ConsumerAtSuccessfulBody() : read{ &fixture.base.sink.bytes } {}
	bool begin()
	{
		if (!recordCohort(fixture, source)) { check(false, "negative fixture requires complete source success linkage"); return false; }
		if (!reference.beginRun(consumeCohortOptions(fixture, source, read)))
		{ check(false, "negative fixture requires authenticated complete-source consumer setup"); return false; }
		KernelPerformanceTimingRunOptions options; options.enabled = true;
		options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE; options.clock = CohortClock::read; options.clockContext = &time;
		if (!timing.beginRun(options)) return false;
		frame = timing.beginFrame(1, 7, KernelPerformanceSchedulerBoundary());
		a = reference.beginAttempt(fixture.identity(0));
		if (!a.valid() || !replayCohortDecision(reference, a, fixture.base.rejected)) return false;
		authoritative += cohortLegacyFallback(fixture.empty, counts);
		if (!reference.finishAttempt(a, fixture.base.notAdmitted) || !reference.reapAttempt(a, fixture.base.reap)) return false;
		time.now = 110; if (!timing.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE)) return false;
		time.now = 120; if (!timing.endPhase(frame, KERNEL_PHASE_OWNER_INTAKE)) return false;
		time.now = 125; if (!timing.beginPhase(frame, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND)) return false;
		time.now = 145; if (!timing.endPhase(frame, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND)) return false;
		time.now = 150; if (!timing.beginPhase(frame, KERNEL_PHASE_SPATIAL_WORK)) return false;
		s = reference.beginAttempt(fixture.identity(1));
		return s.valid();
	}
	bool captureAndPlan()
	{
		const CohortCanonicalInput input = { &fixture.committedInput, &counts };
		return reference.bindCapturedInput(s, 1, 2, writeCohortInput, &input) &&
			replayCohortDecision(reference, s, fixture.base.accepted) && reference.observeDispatch(s, fixture.base.dispatch) &&
			reference.observeRangePlan(s, fixture.range(0)) && reference.observeRangePlan(s, fixture.range(1));
	}
	bool execute(unsigned index, KernelPerformanceInlineBody *retained = 0)
	{
		time.now = index == 0 ? 160 : 185;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		if (reference.beginInlineBody(s, fixture.range(index), timing, body, probe) != KERNEL_INLINE_EXECUTE) return false;
		KernelPerformanceRangeProgress progress = {};
		progress.checkpoint = cohortPrefixBody(fixture.committedInput, fixture.range(index), probe, false, true, output, counts);
		progress.publication = KERNEL_PUBLICATION_PUBLISHED; time.now = index == 0 ? 180 : 205;
		if (retained != 0) *retained = body;
		return reference.finishInlineBody(body, progress) && reference.observeReleasedRange(s, fixture.range(index), progress);
	}
	KernelPerformanceReferenceBatch validate()
	{
		const CohortCanonicalOutput view = { &output, &counts };
		return reference.observeValidatedAttempt(s, writeCohortOutput, &view);
	}
	bool reachLateAttempt(bool enterCompletion)
	{
		if (!captureAndPlan() || !execute(0) || !execute(1)) return false;
		const auto token = validate();
		if (!token.valid()) return false;
		const bool committed = cohortAuthoritativeCommit(output, authoritative, counts);
		if (!reference.finishBatch(token, committed) || !reference.finishAttempt(s, fixture.committed(token)) ||
			!reference.reapAttempt(s, fixture.base.reap)) return false;
		b = reference.beginAttempt(fixture.identity(2));
		const CohortCanonicalInput input = { &fixture.abortedInput, &counts };
		if (!b.valid() || !reference.bindCapturedInput(b, 1, 3, writeCohortInput, &input) ||
			!replayCohortDecision(reference, b, fixture.base.accepted) || !reference.observeDispatch(b, fixture.abortedDispatch())) return false;
		for (unsigned i = 0; i != 3; ++i) if (!reference.observeRangePlan(b, fixture.range(i))) return false;
		if (!enterCompletion) return true;
		authoritative += cohortLegacyFallback(fixture.abortedInput, counts);
		if (!reference.finishAttempt(b, fixture.base.aborted)) return false;
		const auto c = reference.beginAttempt(fixture.identity(3));
		if (!c.valid() || !replayCohortDecision(reference, c, fixture.base.refused)) return false;
		authoritative += cohortLegacyFallback(fixture.empty, counts);
		if (!reference.finishAttempt(c, fixture.base.notAdmitted) || !reference.reapAttempt(c, fixture.base.reap)) return false;
		time.now = 250; if (!timing.endPhase(frame, KERNEL_PHASE_SPATIAL_WORK)) return false;
		for (unsigned i = 3; i != KERNEL_PHASE_COUNT; ++i)
		{
			const auto phase = static_cast<KernelPerformancePhase>(i);
			time.now += 5; if (!timing.beginPhase(frame, phase)) return false;
			time.now += 10; if (!timing.endPhase(frame, phase)) return false;
		}
		time.now = 300;
		if (!timing.endFrame(frame, 7, KernelPerformanceSchedulerBoundary()) || !timing.sealAdmissions() ||
			!reference.sealObservationWindow()) return false;
		time.now = 400;
		return timing.beginCompletionSerial().valid();
	}
};

void consumerAuthenticatedBodyRejectsOrdinaryPipelineIntervals()
{
	// Break: beginInterval pushes an ordinary stage inside an authenticated
	// body, so the stage's owner work is incorrectly charged to pure time.
	ConsumerAtSuccessfulBody test;
	if (!test.begin()) return;
	const bool prepared = test.captureAndPlan();
	check(prepared, "nested ordinary interval fixture prepares authenticated source ranges");
	if (!prepared) return;
	test.time.now = 155;
	const auto batch = test.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 7, 1);
	const auto outside = test.timing.beginInterval(batch, KERNEL_PERFORMANCE_CAPTURE);
	test.time.now = 158;
	check(batch.valid() && outside.valid() && test.timing.endInterval(outside),
		"ordinary pipeline interval remains legal outside an authenticated body");
	test.time.now = 160;
	KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
	const bool opened = test.reference.beginInlineBody(test.s, test.fixture.range(0), test.timing, body, probe) == KERNEL_INLINE_EXECUTE;
	check(opened && body.valid(), "ordinary nesting negative opens a real source-authenticated pure body");
	if (!opened) return;
	const unsigned before = test.time.reads;
	test.time.now = 165;
	const auto nested = test.timing.beginInterval(batch, KERNEL_PERFORMANCE_VALIDATE);
	check(!nested.valid() && test.time.reads == before,
		"ordinary pipeline interval inside an authenticated body is rejected before a clock read");
	const unsigned after = test.time.reads;
	test.time.now = 170;
	check(!test.timing.beginInterval(batch, KERNEL_PERFORMANCE_COMMIT).valid() && test.time.reads == after,
		"rejected ordinary nesting leaves subsequent interval writes sticky and clock-free");
	const auto snapshot = test.timing.freeze();
	check(!snapshot.complete && !snapshot.phaseAccounting.complete &&
		(snapshot.errors & KERNEL_PERFORMANCE_ERROR_ORDER) != 0 &&
		snapshot.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"ordinary nesting cannot qualify phase accounting or erase its ordering failure and baseline role");
}

void consumerRejectsChangedActualCaptureDecisionAndPartition()
{
	// Break: replaying expected decisions/plans instead of matching independent
	// current capture/eligibility/partition observations.
	for (unsigned mutation = 0; mutation != 11; ++mutation)
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin()) continue;
		auto input = test.fixture.committedInput;
		if (mutation == 0) ++input.values[0];
		const CohortCanonicalInput view = { &input, &test.counts };
		bool accepted = test.reference.bindCapturedInput(test.s, 1, 2, writeCohortInput, &view);
		if (mutation != 0)
		{
			check(accepted, "changed-plan fixture independently matches its capture first");
			auto facts = test.fixture.base.accepted.deterministicFacts;
			if (mutation == 3) facts.bytes[0] ^= 1;
			KernelPerformanceAttemptDecision inherited = {};
			accepted = test.reference.replayDecision(test.s, mutation == 1 ? 99 : 21, mutation != 2, facts, inherited);
			if (mutation >= 4)
			{
				check(accepted, "changed-plan fixture matches its deterministic decision first");
				auto dispatch = test.fixture.base.dispatch;
				if (mutation == 4) ++dispatch.sourceGrain;
				if (mutation == 5) ++dispatch.sourceLimit;
				if (mutation == 6) ++dispatch.operationCount;
				if (mutation == 7) ++dispatch.rangeCount;
				accepted = test.reference.observeDispatch(test.s, dispatch);
				if (mutation >= 8)
				{
					check(accepted, "changed-range fixture matches its source dispatch first");
					auto range = test.fixture.range(0);
					if (mutation == 8) ++range.begin;
					if (mutation == 9) ++range.end;
					if (mutation == 10) ++range.rangeOrdinal;
					accepted = test.reference.observeRangePlan(test.s, range);
				}
			}
		}
		check(!accepted && test.counts.bodies == 0 && test.counts.commits == 0 &&
			test.reference.mode() == KERNEL_REFERENCE_DISABLED &&
			test.reference.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"changed actual deterministic fact or source plan fails before body execution and keeps baseline routing latched");
	}
}
void consumerBodyAndOwnerSerialAuthorityCannotBeForgedOrReused()
{
	// Break: token scalar coincidence, public probe init, role alone, second
	// begin, nested body or foreign owner confers executable/pure authority.
	for (unsigned mutation = 0; mutation != 12; ++mutation)
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin()) continue;
		const bool prepared = test.captureAndPlan();
		check(prepared, "body-authority fixture binds the real capture and both source plans");
		if (!prepared) continue;
		auto attempt = test.s; auto range = test.fixture.range(0);
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		KernelPerformanceReferenceLedger foreign; ByteSink foreignSink;
		if (mutation == 0) attempt = KernelPerformanceAttempt();
		if (mutation == 1) attempt = test.a;
		if (mutation == 2)
		{
			auto options = test.fixture.base.options; options.trace.context = &foreignSink;
			check(foreign.beginRun(options), "coincident foreign attempt begins its own ledger");
			const auto a = foreign.beginAttempt(test.fixture.identity(0));
			check(foreign.observeDecision(a, test.fixture.base.rejected) &&
				foreign.finishAttempt(a, test.fixture.base.notAdmitted) && foreign.reapAttempt(a, test.fixture.base.reap),
				"coincident foreign ledger advances to the same scalar attempt position");
			attempt = foreign.beginAttempt(test.fixture.identity(1));
		}
		if (mutation == 3) ++range.end;
		if (mutation == 4)
		{
			const KernelPerformanceCheckpointProgress source = {
				true, 0, 3, 0, 192, { 0, 0, 0 }, { 13, 192, 0 }, KERNEL_RANGE_COMPLETED };
			check(probe.beginReplay(source), "public probe may validate progress without granting body authority");
			attempt = KernelPerformanceAttempt();
		}
		if (mutation == 5 || mutation == 6)
		{
			check(test.reference.beginInlineBody(attempt, range, test.timing, body, probe) == KERNEL_INLINE_EXECUTE,
				"duplicate/nested fixture opens exactly one authenticated body first");
			if (mutation == 6) range = test.fixture.range(1);
		}
		if (mutation == 8)
		{
			test.reference.freeze();
			check(test.reference.beginRun(consumeCohortOptions(test.fixture, test.source, test.read)),
				"new consumer generation must not reuse an old attempt identity");
		}
		const unsigned clockReads = test.time.reads;
		if (mutation == 9)
			check(!test.reference.beginInlineOwnerSerial(KernelPerformanceInlineBody(),
				KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION).valid(), "default body cannot authorize owner serial extent");
		else if (mutation == 10)
			check(!test.reference.endInlineOwnerSerial(KernelPerformanceInlineOwnerSerial()),
				"default serial extent cannot end an authenticated owner scope");
		else if (mutation == 11)
			check(!test.reference.finishInlineBody(KernelPerformanceInlineBody(), KernelPerformanceRangeProgress()),
				"default body cannot fabricate completion");
		else
		{
			KernelPerformanceInlineBody refused; KernelPerformanceCheckpointProbe refusedProbe;
			KernelPerformanceInlineAction action = KERNEL_INLINE_EXECUTE;
			if (mutation == 7)
			{
				std::thread worker([&]() { action = test.reference.beginInlineBody(attempt, range,
					test.timing, refused, refusedProbe); });
				worker.join();
			}
			else action = test.reference.beginInlineBody(attempt, range, test.timing, refused, refusedProbe);
			check(action == KERNEL_INLINE_INVALID && !refused.valid(),
				"forged stale reused nested or foreign source body never receives executable authority");
		}
		check(test.counts.bodies == 0 && test.time.reads == clockReads && test.counts.commits == 0 &&
			!test.reference.freeze().trace.complete,
			"rejected authority performs no helper callback timing read commit or complete-prefix publication");
	}
}
void consumerActualProgressAndSerialScopeFailuresStaySticky()
{
	// Break: a completed source lookahead is accepted in place of independently
	// finished progress, or unbalanced/nested/foreign serial scope resumes pure.
	for (unsigned mutation = 0; mutation != 11; ++mutation)
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin()) continue;
		const bool prepared = test.captureAndPlan();
		check(prepared, "progress negative fixture prepares both source ranges");
		if (!prepared) continue;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		const bool opened = test.reference.beginInlineBody(test.s, test.fixture.range(0), test.timing, body, probe) == KERNEL_INLINE_EXECUTE;
		check(opened, "progress negative fixture opens one authenticated source body");
		if (!opened) continue;
		KernelPerformanceRangeProgress actual = {};
		actual.checkpoint = cohortPrefixBody(test.fixture.committedInput, test.fixture.range(0), probe,
			false, true, test.output, test.counts);
		actual.publication = KERNEL_PUBLICATION_PUBLISHED;
		if (mutation == 0) --actual.checkpoint.completedWorkUnits;
		if (mutation == 1) ++actual.checkpoint.finalCheckpoint.second;
		if (mutation == 2) --actual.checkpoint.pollCount;
		if (mutation == 3) actual.publication = KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL;
		if (mutation == 4) actual.checkpoint.terminal = KERNEL_RANGE_FAILED;
		bool accepted = false;
		if (mutation < 5) accepted = test.reference.finishInlineBody(body, actual);
		else if (mutation == 5)
			accepted = test.reference.beginInlineOwnerSerial(body, static_cast<KernelPerformanceInlineOwnerSerialKind>(99)).valid();
		else if (mutation == 10)
		{
			check(test.reference.finishInlineBody(body, actual), "actual body closes once before duplicate-finish test");
			accepted = test.reference.finishInlineBody(body, actual);
		}
		else
		{
			const auto serial = test.reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
			check(serial.valid(), "serial scope negative opens a valid typed owner subextent first");
			if (mutation == 6) accepted = test.reference.finishInlineBody(body, actual);
			if (mutation == 7)
			{
				check(test.reference.endInlineOwnerSerial(serial), "serial token closes once before duplicate-use test");
				accepted = test.reference.endInlineOwnerSerial(serial);
			}
			if (mutation == 8)
			{
				std::thread worker([&]() { accepted = test.reference.endInlineOwnerSerial(serial); }); worker.join();
			}
			if (mutation == 9) accepted = test.timing.endPhase(test.frame, KERNEL_PHASE_SPATIAL_WORK);
		}
		check(!accepted && test.counts.bodies == 1 && test.counts.commits == 0,
			"changed actual progress or invalid serial/phase close cannot complete an authenticated body");
		check(!test.reference.freeze().trace.complete && !test.timing.freeze().phaseAccounting.complete &&
			test.reference.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"body/scope failure cannot publish phase or source completion or unlatch baseline execution");
	}
}
void consumerSuccessfulLinkUsesOnlyActualOutputAndSingleOwnerCommit()
{
	// Break: detached recomputation, anonymous success, relinking an output,
	// foreign scalar token, commit substitution or omitted body is accepted.
	for (unsigned mutation = 0; mutation != 12; ++mutation)
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin()) continue;
		const bool prepared = test.captureAndPlan() && test.execute(0) && (mutation == 11 || test.execute(1));
		check(prepared, "success-link negative fixture executes its authenticated source bodies");
		if (!prepared) continue;
		if (mutation == 0) ++test.output.sum;
		CohortCanonicalOutput output = { &test.output, &test.counts };
		CohortCanonicalInput input = { &test.fixture.committedInput, &test.counts }; Output detached = {};
		bool accepted = false;
		if (mutation == 1 || mutation == 2)
		{
			accepted = test.reference.observeValidatedAttempt(test.s, writeCohortOutput, &output,
				mutation == 1 ? forbiddenCohortDetached : 0, &input, &detached).valid();
			check(test.counts.detached == 0 && test.counts.outputWrites == 0,
				"baseline rejects detached callback/storage before invoking or hashing it");
		}
		else if (mutation == 3)
		{
			accepted = test.reference.observeValidatedBatch(KERNEL_PERFORMANCE_PATH, 0, 7, 1, 1, 2,
				writeCohortInput, &input, writeCohortOutput, &output).valid();
			check(test.counts.inputWrites == 1 && test.counts.outputWrites == 0,
				"anonymous validated batch cannot bypass captured-attempt linkage or hash input twice");
		}
		else
		{
			const auto token = test.validate();
			if (mutation == 0 || mutation == 11) accepted = token.valid();
			else
			{
				check(token.valid(), "commit identity negative first links the actually produced output");
				if (!token.valid()) continue;
				if (mutation == 4)
				{
					accepted = test.validate().valid();
					check(test.counts.outputWrites == 1, "second validated link cannot hash the same output again");
				}
				if (mutation == 5)
				{
					KernelPerformanceReferenceLedger foreign; foreign.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
					const auto other = foreign.observeValidatedBatch(KERNEL_PERFORMANCE_PATH, 0, 7, 1, 1, 2,
						writeCohortInput, &input, writeCohortOutput, &output);
					check(other.valid() && other.generation == token.generation && other.serial == token.serial && other.slot == token.slot,
						"foreign validated token deliberately has identical public generation serial and slot");
					accepted = test.reference.finishBatch(other, true);
				}
				if (mutation == 6)
				{
					auto changed = token; ++changed.slot;
					accepted = test.reference.finishBatch(changed, true);
				}
				if (mutation == 7)
				{
					const bool committed = cohortAuthoritativeCommit(test.output, test.authoritative, test.counts, true);
					check(test.reference.finishBatch(token, committed),
						"failed owner commit closes its validated token as an explicit noncommit");
					accepted = test.reference.finishAttempt(test.s,
						test.fixture.committed(token));
					check(test.authoritative == 0, "source expected success never substitutes for actual owner commit failure");
				}
				if (mutation == 8)
				{
					const bool committed = cohortAuthoritativeCommit(test.output, test.authoritative, test.counts);
					check(test.reference.finishBatch(token, committed), "actual owner commit closes once before duplicate test");
					accepted = test.reference.finishBatch(token, true);
				}
				if (mutation == 9) accepted = test.reference.finishAttempt(test.s, test.fixture.committed(token));
				if (mutation == 10)
				{
					const bool committed = cohortAuthoritativeCommit(test.output, test.authoritative, test.counts);
					check(test.reference.finishBatch(token, committed), "wrong finish fixture retains a real successful owner commit");
					accepted = test.reference.finishAttempt(test.s, test.fixture.base.aborted);
				}
			}
		}
		check(!accepted && test.counts.detached == 0 && test.counts.commits <= 1 &&
			!test.reference.freeze().trace.complete && test.reference.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"missing actual body output or single exact local commit cannot become matching source success");
	}
}
struct CohortOutputFault
{
	KernelPerformanceReferenceLedger *ledger;
	KernelPerformanceAttempt attempt;
	unsigned kind, calls = 0;
};
bool faultingCohortOutput(KernelPerformanceCanonicalWriter &writer, const void *context)
{
	auto &fault = *const_cast<CohortOutputFault *>(static_cast<const CohortOutputFault *>(context));
	++fault.calls;
	if (fault.kind == 0) return false;
	if (fault.kind == 1) throw std::runtime_error("actual linked output callback fault");
	check(!fault.ledger->observeValidatedAttempt(fault.attempt, faultingCohortOutput, &fault).valid(),
		"canonical output callback cannot recursively link a second output");
	return writer.u32(1, 1344);
}
void consumerActualOutputCallbackFailuresCannotPublishSuccess()
{
	// Break: false/throw/reentry of the actual once-produced output callback is
	// replaced by source success or leaks a validated token/second callback.
	for (unsigned kind = 0; kind != 3; ++kind)
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin()) continue;
		const bool prepared = test.captureAndPlan() && test.execute(0) && test.execute(1);
		check(prepared, "output callback negative executes the actual two bodies first");
		if (!prepared) continue;
		CohortOutputFault fault = { &test.reference, test.s, kind };
		const auto token = test.reference.observeValidatedAttempt(test.s, faultingCohortOutput, &fault);
		check(!token.valid() && fault.calls == 1 && test.counts.bodies == 2 && test.counts.commits == 0 &&
			!test.reference.freeze().trace.complete && test.reference.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"actual linked output failure remains once-only incomplete evidence without commit or rerun");
	}
}
void consumerLateSourceEventsCannotExecuteOrReapEarly()
{
	// Break: complete immutable lookahead erases source fallback/busy/release
	// order, or never-entered/partial release is mistaken for a complete group.
	for (unsigned mutation = 0; mutation != 7; ++mutation)
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin()) continue;
		const bool prepared = test.reachLateAttempt(mutation >= 2);
		check(prepared, "late negative fixture consumes the actual source success and retained abort prefix");
		if (!prepared) continue;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		bool accepted = false;
		if (mutation == 0)
			accepted = test.reference.beginInlineBody(test.b, test.fixture.range(0), test.timing, body, probe) == KERNEL_INLINE_EXECUTE;
		if (mutation == 1)
		{
			test.authoritative += cohortLegacyFallback(test.fixture.abortedInput, test.counts);
			check(test.reference.finishAttempt(test.b, test.fixture.base.aborted), "modelled B fallback is observed before early reap");
			accepted = test.reference.reapAttempt(test.b, test.fixture.base.reap);
		}
		if (mutation == 2) accepted = test.reference.sealExecutionClosure();
		if (mutation == 3)
			accepted = test.reference.beginInlineBody(test.b, test.fixture.range(1), test.timing, body, probe) != KERNEL_INLINE_INVALID;
		if (mutation >= 4)
		{
			const bool opened = test.reference.beginInlineBody(test.b, test.fixture.range(0), test.timing, body, probe) == KERNEL_INLINE_EXECUTE;
			check(opened, "late negative opens only the source's next released range");
			if (!opened) continue;
			KernelPerformanceRangeProgress actual = {};
			actual.checkpoint = cohortPrefixBody(test.fixture.abortedInput, test.fixture.range(0), probe,
				true, true, test.output, test.counts);
			actual.publication = KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL;
			if (mutation == 6)
				accepted = test.timing.sealExecutionClosure(KernelPerformanceSchedulerBoundary());
			else
			{
				check(test.reference.finishInlineBody(body, actual) &&
					test.reference.observeReleasedRange(test.b, test.fixture.range(0), actual), "only first B progress is acknowledged");
				if (mutation == 4) accepted = test.reference.reapAttempt(test.b, test.fixture.base.reap);
				if (mutation == 5)
				{
					const unsigned before = test.time.reads;
					check(test.reference.beginInlineBody(test.b, test.fixture.range(1), test.timing, body, probe) ==
						KERNEL_INLINE_SKIP_SOURCE_NEVER_ENTERED && !body.valid() && test.time.reads == before,
						"never-entered acknowledgement is once-only and clock-free");
					accepted = test.reference.beginInlineBody(test.b, test.fixture.range(1), test.timing, body, probe) != KERNEL_INLINE_INVALID;
				}
			}
		}
		check(!accepted && test.counts.bodies == (mutation >= 4 ? 3 : 2) &&
			!test.reference.freeze().trace.complete && !test.timing.freeze().phaseAccounting.complete,
			"early reordered duplicate or partial late work cannot complete the source-matched lifecycle");
	}
}

void noCaptureTraceRoundTripRemainsIndependentFromSuccessfulStreams()
{
	// Independent consumer RED: does not wait for the successful-output stub.
	CohortFixture fixture; CohortSource source; KernelPerformanceReferenceLedger record;
	check(record.beginRun(fixture.base.options), "no-capture record control starts");
	const auto attempt = record.beginAttempt(fixture.identity(0));
	check(attempt.valid() && record.observeDecision(attempt, fixture.base.rejected) &&
		record.finishAttempt(attempt, fixture.base.notAdmitted) && record.reapAttempt(attempt, fixture.base.reap) &&
		record.sealObservationWindow() && record.sealExecutionClosure(), "no-capture control closes the actual source lifecycle");
	source.snapshot = record.freeze();
	check(source.snapshot.trace.complete && !source.snapshot.complete && source.snapshot.trace.recordCount == 8,
		"complete no-capture source trace has no successful canonical stream");
	const unsigned char successfulFooterZeros[] = {
		3, 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	const auto &bytes = fixture.base.sink.bytes;
	check(bytes.size() >= sizeof(successfulFooterZeros) &&
		memcmp(bytes.data() + bytes.size() - sizeof(successfulFooterZeros), successfulFooterZeros, sizeof(successfulFooterZeros)) == 0,
		"no-capture source emits the single final footer grammar with four literal zero success totals");
	CohortRead read = { &fixture.base.sink.bytes }; KernelPerformanceReferenceLedger consume;
	const bool started = consume.beginRun(consumeCohortOptions(fixture, source, read));
	check(started, "no-capture source alone can initialize authenticated baseline consumption");
	if (!started) return;
	const unsigned previousClocks = clocks, previousWrites = writes, previousComputes = computes;
	const auto replay = consume.beginAttempt(fixture.identity(0));
	check(replay.valid() && replayCohortDecision(consume, replay, fixture.base.rejected) &&
		consume.finishAttempt(replay, fixture.base.notAdmitted) && consume.reapAttempt(replay, fixture.base.reap) &&
		consume.sealObservationWindow() && consume.sealExecutionClosure(), "no-capture consumer reproduces the full source owner sequence");
	const auto result = consume.freeze();
	check(result.trace.complete && !result.complete && result.streamCount == 0 && result.trace.recordCount == 8 &&
		result.trace.digest.equals(source.snapshot.trace.digest) && clocks == previousClocks && writes == previousWrites &&
		computes == previousComputes, "no-capture consume grants no helper output detached computation or pure timing");
}
void coincidentValidatedBatchCannotCrossLedgerInstances()
{
	// Actual behavioral provenance RED independent of the new consumer path.
	KernelPerformanceReferenceLedger first, second;
	first.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING); second.beginRun(KERNEL_REFERENCE_THROUGHPUT_BINDING);
	Input input = { 3, 4, {} }; Output output = { 7 }, detached = {};
	const auto a = observe(first, input, output, detached), b = observe(second, input, output, detached);
	check(a.valid() && b.valid() && a.generation == b.generation && a.serial == b.serial && a.slot == b.slot,
		"separate validated ledgers deliberately return coincident public token fields");
	check(!first.finishBatch(b, true), "a foreign validated token cannot commit a coincident local pending slot");
	check(!first.freeze().complete && second.finishBatch(b, true) && second.freeze().complete,
		"foreign misuse poisons only the destination while the true token remains valid in its own ledger");
}
void inlineSurfacesAreInertWithoutAuthenticatedConsumption()
{
	// Break: the new APIs provide compute/timing authority or poison ordinary
	// untraced evidence when no selected source consumer exists.
	for (unsigned scenario = 0; scenario != 3; ++scenario)
	{
		KernelPerformanceReferenceLedger reference; KernelPerformanceLedger timing;
		if (scenario != 0)
			check(reference.beginRun(scenario == 1 ? KERNEL_REFERENCE_DISABLED : KERNEL_REFERENCE_THROUGHPUT_BINDING, clock),
				"inactive inline control starts its existing untraced role");
		Input input = { 3, 4, {} }; Output output = { 7 }, detached = { 99 };
		const unsigned beforeClocks = clocks, beforeWrites = writes, beforeComputes = computes;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		KernelPerformanceAttemptDecision decision = {}; decision.site = 99;
		KernelPerformanceDispatchPlan dispatch = {}; dispatch.rangeCount = 99;
		KernelPerformanceRangePlan range = {}; range.begin = 99;
		KernelPerformanceAttemptFinish finish = {}; finish.reason = 99;
		check(!reference.replayDecision(KernelPerformanceAttempt(), 1, true, KernelPerformanceDigest(), decision) &&
			!reference.readSourceDispatch(KernelPerformanceAttempt(), 0, dispatch) &&
			!reference.readSourceRange(KernelPerformanceAttempt(), 0, 0, range) &&
			!reference.readSourceFinish(KernelPerformanceAttempt(), finish) &&
			reference.beginInlineBody(KernelPerformanceAttempt(), range, timing, body, probe) == KERNEL_INLINE_INVALID &&
			!reference.finishInlineBody(body, KernelPerformanceRangeProgress()) &&
			!reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION).valid() &&
			!reference.endInlineOwnerSerial(KernelPerformanceInlineOwnerSerial()) &&
			!reference.observeValidatedAttempt(KernelPerformanceAttempt(), writeOutput, &output, compute, &input, &detached).valid(),
			"unstarted disabled and untraced runs cannot grant source-bound body or success authority");
		check(!body.valid() && !probe.snapshot().entered && decision.site == 99 && dispatch.rangeCount == 99 &&
			range.begin == 99 && finish.reason == 99 && clocks == beforeClocks && writes == beforeWrites &&
			computes == beforeComputes && detached.sum == 99, "inactive inline calls preserve outputs and perform no callbacks or clocks");
		if (scenario == 2)
		{
			const auto token = observe(reference, input, output, detached);
			check(token.valid() && reference.finishBatch(token, true) && reference.freeze().complete,
				"inert new surfaces do not weaken or poison the existing valid untraced throughput path");
		}
	}
}

struct SourceCallbackFault
{
	KernelPerformanceReferenceLedger *ledger;
	KernelPerformanceAttemptIdentity nested;
	unsigned kind, calls = 0;
};
bool sourceFaultingInput(KernelPerformanceCanonicalWriter &, const void *context)
{
	auto &fault = *const_cast<SourceCallbackFault *>(static_cast<const SourceCallbackFault *>(context));
	++fault.calls;
	if (fault.kind == 0) return false;
	if (fault.kind == 1) throw std::runtime_error("actual input callback fault");
	check(!fault.ledger->beginAttempt(fault.nested).valid(), "canonical callback cannot reenter the source lifecycle");
	return true;
}
void sourceLifecycleCallbackAndCounterFailuresRemainSticky()
{
	// Breaks: callback false/throw/reentry publishes a prefix, or captured-op
	// arithmetic wraps. These control existing behavior in the cohesive wave;
	// no claim that all controls are currently RED is made.
	for (unsigned kind = 0; kind != 3; ++kind)
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		check(source.beginRun(fixture.options), "callback fault fixture begins");
		const auto attempt = source.beginAttempt(fixture.identity(0));
		SourceCallbackFault fault = { &source, fixture.identity(1), kind };
		check(!source.bindCapturedInput(attempt, 1, 2, sourceFaultingInput, &fault) && fault.calls == 1,
			"real canonical input callback failure or reentry rejects that capture exactly once");
		check(source.mode() == KERNEL_REFERENCE_DISABLED && source.runMode() == KERNEL_REFERENCE_THROUGHPUT_BINDING &&
			!source.freeze().trace.complete, "failed callback preserves role and cannot publish a complete trace prefix");
	}
	SourceAbortFixture fixture; KernelPerformanceReferenceLedger overflow;
	check(overflow.beginRun(fixture.options), "captured counter boundary begins without a public counter setter");
	const auto first = overflow.beginAttempt(fixture.identity(0));
	check(overflow.bindCapturedInput(first, 1, ~static_cast<JobMetricCounter>(0), writeInput, &fixture.input) &&
		overflow.observeDecision(first, fixture.rejected) && overflow.finishAttempt(first, fixture.notAdmitted) &&
		overflow.reapAttempt(first, fixture.reap), "one full-width captured operation count remains exact");
	const auto second = overflow.beginAttempt(fixture.identity(1));
	check(!overflow.bindCapturedInput(second, 1, 1, writeInput, &fixture.input),
		"next captured operation cannot wrap a full-width aggregate");
	const auto failed = overflow.freeze();
	check(!failed.trace.complete && (failed.trace.errors & KERNEL_PERFORMANCE_ERROR_OVERFLOW) != 0,
		"captured operation overflow is explicit sticky incomplete evidence");
}

void sourceLifecycleRejectsUnboundAndMalformedReleasedRanges()
{
	// Breaks: a free slot/aggregate count authenticates an unrelated source
	// token or range; malformed never-entered/publication/cut is accepted.
	for (unsigned mutation = 0; mutation != 11; ++mutation)
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		auto b = startFinishedAbortedSource(source, fixture);
		auto range = fixture.firstRange;
		auto progress = fixture.cancelled;
		KernelPerformanceReferenceLedger foreign;
		ByteSink foreignSink;
		if (mutation == 1)
		{
			// Same generation, ordinal and first occupied slot in another
			// ledger must still carry different opaque instance provenance.
			auto foreignOptions = fixture.options;
			foreignOptions.trace.context = &foreignSink;
			check(foreign.beginRun(foreignOptions), "foreign attempt fixture begins with coincident counters");
			b = foreign.beginAttempt(fixture.identity(1));
		}
		switch (mutation)
		{
		case 0: b = KernelPerformanceAttempt(); break;
		case 1: break;
		case 2: ++range.dispatchOrdinal; break;
		case 3: ++range.rangeOrdinal; break;
		case 4: ++range.begin; break;
		case 5: ++range.end; break;
		case 6: ++range.operationCount; break;
		case 7: progress.checkpoint.firstTruePoll = 0; break;
		case 8: progress.publication = KERNEL_PUBLICATION_PUBLISHED; break;
		case 9: progress = fixture.neverEntered; progress.checkpoint.completedWorkUnits = 1; break;
		case 10: progress.checkpoint.errors = KERNEL_REFERENCE_ERROR_CHECKPOINT; break;
		}
		check(!source.observeReleasedRange(b, range, progress),
			"released progress must bind an authentic live token exact plan and structurally valid terminal");
		const auto trace = source.freeze().trace;
		check(!trace.complete && trace.errors != 0 && trace.releasedRangeCount == 0 && trace.residentRangeCount == 2,
			"bad release cannot acknowledge either of the two actually planned ranges");
	}
}

KernelPerformanceAttempt prepareSecondReusableDispatch(KernelPerformanceReferenceLedger &source,
	SourceAbortFixture &fixture, KernelPerformanceRangePlan &second0, KernelPerformanceRangePlan &second1)
{
	fixture.options.trace.residentRangeCapacity = 2;
	check(source.beginRun(fixture.options), "two-slot range reuse fixture begins");
	const auto attempt = source.beginAttempt(fixture.identity(0));
	check(attempt.valid() && source.bindCapturedInput(attempt, 1, 2, writeInput, &fixture.input) &&
		source.observeDecision(attempt, fixture.accepted) && source.observeDispatch(attempt, fixture.dispatch) &&
		source.observeRangePlan(attempt, fixture.firstRange) && source.observeRangePlan(attempt, fixture.secondRange),
		"first dispatch occupies both reusable range slots");
	KernelPerformanceRangeProgress complete = {};
	complete.checkpoint = { true, 0, 1, 0, 1, { 0, 0, 0 }, { 21, 1, 0 }, KERNEL_RANGE_COMPLETED };
	complete.publication = KERNEL_PUBLICATION_PUBLISHED;
	check(source.observeReleasedRange(attempt, fixture.firstRange, complete) &&
		source.observeReleasedRange(attempt, fixture.secondRange, complete),
		"first dispatch releases both ranges before next source dispatch");
	auto dispatch = fixture.dispatch; dispatch.dispatchOrdinal = 1;
	second0 = fixture.firstRange; second0.dispatchOrdinal = 1;
	second1 = fixture.secondRange; second1.dispatchOrdinal = 1;
	check(source.observeDispatch(attempt, dispatch) && source.observeRangePlan(attempt, second0) &&
		source.observeRangePlan(attempt, second1), "second dispatch reuses released capacity under a new dispatch identity");
	return attempt;
}
void sourceSequentialDispatchesReuseCapacityWithoutResurrectingOldPlans()
{
	// Break: one range array per history, or recycled slot accepts old dispatch.
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		KernelPerformanceRangePlan second0 = {}, second1 = {};
		const auto attempt = prepareSecondReusableDispatch(source, fixture, second0, second1);
		check(source.finishAttempt(attempt, fixture.aborted) &&
			source.observeReleasedRange(attempt, second0, fixture.cancelled) &&
			source.observeReleasedRange(attempt, second1, fixture.neverEntered) &&
			source.reapAttempt(attempt, fixture.reap) && source.sealObservationWindow() && source.sealExecutionClosure(),
			"actual second-dispatch abort retains and closes both new plans");
		const auto trace = source.freeze().trace;
		check(trace.complete && trace.dispatchCount == 2 && trace.rangeCount == 4 && trace.releasedRangeCount == 4 &&
			trace.residentRangeCapacity == 2 && trace.residentRangeHighWater == 2 && trace.residentRangeCount == 0,
			"four historical ranges use only two live slots without losing cardinality");
	}
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		KernelPerformanceRangePlan second0 = {}, second1 = {};
		const auto attempt = prepareSecondReusableDispatch(source, fixture, second0, second1);
		check(!source.observeReleasedRange(attempt, fixture.firstRange, fixture.cancelled),
			"recycled resident range slot cannot accept a repeated first-dispatch release");
		const auto trace = source.freeze().trace;
		check(!trace.complete && trace.releasedRangeCount == 2 && trace.residentRangeCount == 2,
			"stale historical range cannot release either new dispatch slot");
	}
}


struct BootstrapCohort
{
	CohortFixture fixture;
	CohortSource source;
	KernelPerformanceAttemptIdentity identity() const
	{
		auto value = fixture.identity(1);
		value.ownerFrame = 0; value.phase = KERNEL_PHASE_OWNER_INTAKE;
		return value;
	}
};
KernelPerformanceWindowBoundary bootstrapBoundary(KernelPerformanceWindowBoundaryKind kind,
	JobMetricCounter sample, KernelPerformancePhase phase, unsigned entry, unsigned authority, unsigned actual)
{
	KernelPerformanceWindowBoundary boundary;
	boundary.kind = kind; boundary.sampleOrdinal = sample; boundary.phase = phase;
	boundary.ownerFrameAtEntry = entry; boundary.authorityFrame = authority; boundary.actualOwnerFrame = actual;
	return boundary;
}
bool driveBootstrapCohort(KernelPerformanceReferenceLedger &reference, BootstrapCohort &test,
	CohortCounts &counts, unsigned &authoritative, KernelPerformanceLedger *timing = 0,
	CohortClock *clock = 0, bool slower = false, bool wrongActualFrame = false, bool transitionInsideBody = false)
{
	// This core model drives the same body and canonical callbacks in both
	// roles. D separately proves the actual deferred/new-game native branches.
	const JobMetricCounter begin[] = { 100, 250, 400 }, end[] = { 200, 330, 600 };
	const JobMetricCounter starts[3][5] = { {110,145,160,175,190}, {260,275,290,305,320}, {410,455,480,525,560} };
	const JobMetricCounter ends[3][5] = { {140,155,170,185,195}, {270,285,300,315,325}, {450,475,520,555,590} };
	for (unsigned window = 0; window != 3; ++window)
	{
		const JobMetricCounter sample = window + 1;
		const unsigned entry = window == 0 ? 83 : 0;
		KernelPerformanceFrame frame;
		if (timing != 0)
		{
			clock->now = begin[window]; frame = timing->beginFrame(sample, entry, KernelPerformanceSchedulerBoundary());
			if (!frame.valid()) return false;
		}
		if (!reference.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_BEGIN, sample,
			KERNEL_PHASE_COUNT, entry, entry, entry))) return false;
		for (unsigned ordinal = 0; ordinal != 5; ++ordinal)
		{
			const auto phase = static_cast<KernelPerformancePhase>(ordinal);
			const unsigned authority = window == 0 && ordinal == 0 ? 83 : 0;
			if (!reference.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_PHASE_BEGIN, sample,
				phase, entry, authority, authority))) return false;
			if (timing != 0)
			{
				clock->now = starts[window][ordinal]; if (!timing->beginPhase(frame, phase)) return false;
			}
			if (ordinal == 0 && window != 1)
			{
				const auto kind = window == 0 ? KERNEL_WINDOW_DEFERRED_START_DECLARED : KERNEL_WINDOW_DEFERRED_START_CONSUMED;
				if (!reference.observeWindowBoundary(bootstrapBoundary(kind, sample, phase, entry, authority, 0))) return false;
				if (timing != 0)
				{
					const unsigned reads = clock->reads;
					if (!timing->observeControlTransition(frame, window == 0 ?
						KERNEL_CONTROL_DEFERRED_START_DECLARED : KERNEL_CONTROL_DEFERRED_START_CONSUMED)) return false;
					check(clock->reads == reads, "native control transition changes no measured clock or duration");
				}
			}
			if (window == 0 && ordinal == 0)
			{
				auto identity = test.identity();
				if (wrongActualFrame) identity.ownerFrame = 83;
				const auto attempt = reference.beginAttempt(identity);
				const CohortCanonicalInput input = { &test.fixture.committedInput, &counts };
				if (!attempt.valid() || !reference.bindCapturedInput(attempt, 1, 2, writeCohortInput, &input)) return false;
				if (timing != 0)
				{ if (!replayCohortDecision(reference, attempt, test.fixture.base.accepted)) return false; }
				else if (!reference.observeDecision(attempt, test.fixture.base.accepted)) return false;
				if (!reference.observeDispatch(attempt, test.fixture.base.dispatch) ||
					!reference.observeRangePlan(attempt, test.fixture.range(0)) ||
					!reference.observeRangePlan(attempt, test.fixture.range(1))) return false;
				Output output = {};
				for (unsigned range = 0; range != 2; ++range)
				{
					KernelPerformanceCheckpointProbe probe; KernelPerformanceInlineBody body;
					if (timing != 0)
					{
						clock->now = range == 0 ? 115 : 130;
						if (reference.beginInlineBody(attempt, test.fixture.range(range), *timing, body, probe) != KERNEL_INLINE_EXECUTE) return false;
						if (range == 0 && transitionInsideBody)
						{
							const unsigned reads = clock->reads;
							check(!timing->observeControlTransition(frame, KERNEL_CONTROL_DEFERRED_START_CONSUMED) && clock->reads == reads,
								"control consumption cannot interrupt an authenticated body or read its clock");
							return false;
						}
						if (range == 0)
						{
							clock->now = 117;
							const auto serial = reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
							clock->now = 120;
							if (!serial.valid() || !reference.endInlineOwnerSerial(serial)) return false;
						}
					}
					else if (!probe.beginRecord()) return false;
					KernelPerformanceRangeProgress progress = {};
					progress.checkpoint = cohortPrefixBody(test.fixture.committedInput, test.fixture.range(range),
						probe, false, timing != 0, output, counts);
					progress.publication = KERNEL_PUBLICATION_PUBLISHED;
					if (timing != 0)
					{
						clock->now = range == 0 ? (slower ? 127 : 125) : 135;
						if (!reference.finishInlineBody(body, progress)) return false;
					}
					if (!reference.observeReleasedRange(attempt, test.fixture.range(range), progress)) return false;
				}
				const CohortCanonicalOutput view = { &output, &counts };
				const auto validated = reference.observeValidatedAttempt(attempt, writeCohortOutput, &view);
				if (!validated.valid() || !reference.finishBatch(validated, cohortAuthoritativeCommit(output, authoritative, counts)) ||
					!reference.finishAttempt(attempt, test.fixture.committed(validated)) ||
					!reference.reapAttempt(attempt, test.fixture.base.reap)) return false;
			}
			const unsigned actual = window == 2 && ordinal == 4 ? 1 : 0;
			if (timing != 0)
			{ clock->now = ends[window][ordinal]; if (!timing->endPhase(frame, phase)) return false; }
			if (!reference.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_PHASE_END, sample,
				phase, entry, authority, actual))) return false;
		}
		if (!reference.observeWindowBoundary(bootstrapBoundary(window == 2 ? KERNEL_WINDOW_WORLD_END : KERNEL_WINDOW_CONTROL_END,
			sample, KERNEL_PHASE_COUNT, entry, entry, window == 2 ? 1 : 0))) return false;
		if (timing != 0)
		{
			clock->now = end[window];
			if (window == 2 ? !timing->endFrame(frame, 1, KernelPerformanceSchedulerBoundary()) :
				!timing->endControlWindow(frame, 0, KernelPerformanceSchedulerBoundary())) return false;
		}
	}
	if (!reference.sealObservationWindow()) return false;
	if (timing != 0)
	{
		if (!timing->sealAdmissions()) return false;
		clock->now = 700; const auto completion = timing->beginCompletionSerial(); clock->now = 710;
		if (!completion.valid() || !timing->endCompletionSerial(completion) ||
			!timing->sealExecutionClosure(KernelPerformanceSchedulerBoundary())) return false;
	}
	return reference.sealExecutionClosure();
}
bool recordBootstrapCohort(BootstrapCohort &test)
{
	KernelPerformanceReferenceLedger source;
	if (!source.beginRun(test.fixture.base.options) ||
		!driveBootstrapCohort(source, test, test.source.counts, test.source.authoritative)) return false;
	test.source.snapshot = source.freeze();
	return test.source.snapshot.complete && test.source.snapshot.trace.complete;
}
void sourceControlWindowsRetainRetargetedBodiesAndExactAccounting(bool slower)
{
	BootstrapCohort test;
	const unsigned before = clocks;
	const bool recorded = recordBootstrapCohort(test);
	check(recorded, "bootstrap source records one declaration two control samples and one loaded world frame");
	if (!recorded) return;
	check(clocks == before && test.source.authoritative == 1344 && test.source.counts.bodies == 2 &&
		test.source.counts.inputWrites == 1 && test.source.counts.outputWrites == 1 &&
		test.source.counts.commits == 1 && test.source.counts.detached == 0,
		"control source captures input and actual output once without phase clock or detached body");
	const auto &recordedTrace = test.source.snapshot.trace;
	check(recordedTrace.recordCount == 52 && recordedTrace.logicalEventCount == 52 &&
		recordedTrace.windowBoundaryCount == 38 && recordedTrace.completedWindowCount == 1 && recordedTrace.controlWindowCount == 2 &&
		recordedTrace.attemptCount == 1 && recordedTrace.admittedAttemptCount == 1 && recordedTrace.reapCount == 1 &&
		recordedTrace.capturedOperationCount == 2 && recordedTrace.rangeCount == 2 && recordedTrace.releasedRangeCount == 2,
		"literal 52-record window cohort preserves separate control world attempt and range cardinalities");
	CohortRead read = { &test.fixture.base.sink.bytes }; KernelPerformanceReferenceLedger consumer;
	const bool started = consumer.beginRun(consumeCohortOptions(test.fixture, test.source, read));
	check(started, "complete window grammar is prevalidated before control-body consumption");
	if (!started) return;
	CohortClock time; KernelPerformanceLedger timing; KernelPerformanceTimingRunOptions options;
	options.enabled = true; options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = CohortClock::read; options.clockContext = &time;
	check(timing.beginRun(options), "control consumer starts the existing phase timing engine");
	CohortCounts counts; unsigned authoritative = 0;
	const bool consumed = driveBootstrapCohort(consumer, test, counts, authoritative, &timing, &time, slower);
	check(consumed, "actual frame-zero body consumes once within intake authority83 and preserves two control windows");
	if (!consumed) return;
	const auto reference = consumer.freeze(); const auto snapshot = timing.freeze();
	const auto &a = snapshot.phaseAccounting;
	check(reference.trace.complete && reference.trace.digest.equals(recordedTrace.digest) &&
		reference.trace.byteCount == recordedTrace.byteCount && reference.trace.recordCount == 52 &&
		reference.trace.windowBoundaryCount == 38 && reference.trace.completedWindowCount == 1 && reference.trace.controlWindowCount == 2 &&
		authoritative == 1344 && counts.bodies == 2 && counts.commits == 1 && counts.inputWrites == 1 && counts.outputWrites == 1 && counts.detached == 0,
		"control consumer reproduces exact source bytes counters output and once-only callbacks");
	check(a.complete && a.controlWindowCount == 2 && a.controlNanoseconds == 180 && a.maximumControlNanoseconds == 100 &&
		a.controlUnscopedSerialNanoseconds == 70 && a.firstControlSampleOrdinal == 1 && a.lastControlSampleOrdinal == 2 &&
		a.completedFrameCount == 1 && a.firstCompletedFrame == 1 && a.lastCompletedFrame == 1 &&
		a.frameNanoseconds == 200 && a.unscopedSerialNanoseconds == 40 && a.completionSerialNanoseconds == 10,
		"control time and sample identity never become completed-frame0 or completion-tail time");
	const JobMetricCounter controlTotals[] = { 40,20,20,20,10 }, worldTotals[] = { 40,20,40,30,30 };
	JobMetricCounter serial = a.unscopedSerialNanoseconds + a.controlUnscopedSerialNanoseconds + a.completionSerialNanoseconds, pure = 0;
	for (unsigned i = 0; i != 5; ++i)
	{
		const auto &control = a.controlPhases[i]; const auto &world = a.phases[i];
		const JobMetricCounter expectedPure = i == 0 ? (slower ? 14 : 12) : 0;
		check(control.samples == 2 && control.totalNanoseconds == controlTotals[i] && control.pureNanoseconds == expectedPure &&
			control.serialNanoseconds == controlTotals[i] - expectedPure && world.samples == 1 && world.totalNanoseconds == worldTotals[i] &&
			world.serialNanoseconds == worldTotals[i] && world.pureNanoseconds == 0,
			"each control and world row retains independently literal pure and serial partitions");
		serial += control.serialNanoseconds + world.serialNanoseconds; pure += control.pureNanoseconds + world.pureNanoseconds;
	}
	check(serial == (slower ? 376 : 378) && pure == (slower ? 14 : 12) && serial + pure == 390 &&
		a.frameNanoseconds + a.controlNanoseconds + a.completionSerialNanoseconds == 390,
		"source-matched control body delays change measured partition only, never source events or total extent");
}
void sourceWindowWireAndFooterHaveOneLiteralGrammar()
{
	BootstrapCohort test;
	const bool recorded = recordBootstrapCohort(test);
	check(recorded, "window wire fixture produces the complete source control cohort");
	if (!recorded) return;
	const auto &bytes = test.fixture.base.sink.bytes;
	struct Literal { unsigned occurrence; unsigned char bytes[80]; };
	const Literal boundaries[] = {
		{0, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,2,0,0,0,0,0,0,0,
			1,3,0,0,0,3,0,0,0, 3,4,0,0,0,1,0,0,0,0,0,0,0, 1,5,0,0,0,5,0,0,0,
			1,6,0,0,0,83,0,0,0, 1,7,0,0,0,83,0,0,0, 1,8,0,0,0,83,0,0,0}},
		{1, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,3,0,0,0,0,0,0,0,
			1,3,0,0,0,4,0,0,0, 3,4,0,0,0,1,0,0,0,0,0,0,0, 1,5,0,0,0,0,0,0,0,
			1,6,0,0,0,83,0,0,0, 1,7,0,0,0,83,0,0,0, 1,8,0,0,0,83,0,0,0}},
		{2, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,4,0,0,0,0,0,0,0,
			1,3,0,0,0,8,0,0,0, 3,4,0,0,0,1,0,0,0,0,0,0,0, 1,5,0,0,0,0,0,0,0,
			1,6,0,0,0,83,0,0,0, 1,7,0,0,0,83,0,0,0, 1,8,0,0,0,0,0,0,0}},
		{3, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,15,0,0,0,0,0,0,0,
			1,3,0,0,0,5,0,0,0, 3,4,0,0,0,1,0,0,0,0,0,0,0, 1,5,0,0,0,0,0,0,0,
			1,6,0,0,0,83,0,0,0, 1,7,0,0,0,83,0,0,0, 1,8,0,0,0,0,0,0,0}},
		{4, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,16,0,0,0,0,0,0,0,
			1,3,0,0,0,4,0,0,0, 3,4,0,0,0,1,0,0,0,0,0,0,0, 1,5,0,0,0,1,0,0,0,
			1,6,0,0,0,83,0,0,0, 1,7,0,0,0,0,0,0,0, 1,8,0,0,0,0,0,0,0}},
		{12, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,24,0,0,0,0,0,0,0,
			1,3,0,0,0,7,0,0,0, 3,4,0,0,0,1,0,0,0,0,0,0,0, 1,5,0,0,0,5,0,0,0,
			1,6,0,0,0,83,0,0,0, 1,7,0,0,0,83,0,0,0, 1,8,0,0,0,0,0,0,0}},
		{27, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,39,0,0,0,0,0,0,0,
			1,3,0,0,0,9,0,0,0, 3,4,0,0,0,3,0,0,0,0,0,0,0, 1,5,0,0,0,0,0,0,0,
			1,6,0,0,0,0,0,0,0, 1,7,0,0,0,0,0,0,0, 1,8,0,0,0,0,0,0,0}},
		{37, {1,1,0,0,0,2,0,0,0, 3,2,0,0,0,49,0,0,0,0,0,0,0,
			1,3,0,0,0,6,0,0,0, 3,4,0,0,0,3,0,0,0,0,0,0,0, 1,5,0,0,0,5,0,0,0,
			1,6,0,0,0,0,0,0,0, 1,7,0,0,0,0,0,0,0, 1,8,0,0,0,1,0,0,0}}
	};
	for (const auto &literal : boundaries)
	{
		const auto offset = artifactField(bytes, 2, literal.occurrence, 1, 1);
		check(offset <= bytes.size() && bytes.size() - offset >= sizeof(literal.bytes) &&
			memcmp(bytes.data() + offset, literal.bytes, sizeof(literal.bytes)) == 0 &&
			artifactRecordEnd(bytes, offset) == offset + 80,
			"literal 80-byte boundary retains sample phase entry borrowed authority and actual frame independently");
	}
	const unsigned char footer[] = {
		1,1,0,0,0,11,0,0,0, 3,2,0,0,0,52,0,0,0,0,0,0,0,
		3,3,0,0,0,52,0,0,0,0,0,0,0, 3,4,0,0,0,52,0,0,0,0,0,0,0,
		3,5,0,0,0,1,0,0,0,0,0,0,0, 3,6,0,0,0,1,0,0,0,0,0,0,0,
		3,7,0,0,0,0,0,0,0,0,0,0,0, 3,8,0,0,0,0,0,0,0,0,0,0,0,
		3,9,0,0,0,1,0,0,0,0,0,0,0, 3,10,0,0,0,0,0,0,0,0,0,0,0,
		3,11,0,0,0,1,0,0,0,0,0,0,0, 3,12,0,0,0,0,0,0,0,0,0,0,0,
		3,13,0,0,0,0,0,0,0,0,0,0,0, 5,14,0,0,0,1, 5,15,0,0,0,1,
		3,16,0,0,0,1,0,0,0,0,0,0,0, 3,17,0,0,0,2,0,0,0,0,0,0,0,
		3,18,0,0,0,1,0,0,0,0,0,0,0, 3,19,0,0,0,2,0,0,0,0,0,0,0,
		3,20,0,0,0,2,0,0,0,0,0,0,0, 3,21,0,0,0,0,0,0,0,0,0,0,0,
		3,22,0,0,0,2,0,0,0,0,0,0,0, 3,23,0,0,0,1,0,0,0,0,0,0,0,
		3,24,0,0,0,1,0,0,0,0,0,0,0, 3,25,0,0,0,2,0,0,0,0,0,0,0,
		3,26,0,0,0,2,0,0,0,0,0,0,0, 3,27,0,0,0,38,0,0,0,0,0,0,0,
		3,28,0,0,0,1,0,0,0,0,0,0,0, 3,29,0,0,0,2,0,0,0,0,0,0,0
	};
	const auto offset = artifactField(bytes, 11, 0, 1, 1);
	check(offset <= bytes.size() && bytes.size() - offset == sizeof(footer) &&
		memcmp(bytes.data() + offset, footer, sizeof(footer)) == 0,
		"complete literal footer has exactly one 29-field grammar with source window and success totals");
}
void sourceWindowGrammarAndNativeIdentityCannotBeSubstituted()
{
	BootstrapCohort test;
	const bool recorded = recordBootstrapCohort(test);
	check(recorded, "window mutation matrix starts with a real complete 52-record artifact");
	if (!recorded) return;
	struct Mutation { unsigned occurrence, tag, type, byte, replacement; };
	const Mutation mutations[] = {
		{0,3,1,5,99}, {0,4,3,0,1}, {0,4,3,5,0}, {0,5,1,5,0}, {0,6,1,5,82},
		{1,7,1,5,0}, {1,8,1,5,0}, {2,3,1,5,9}, {2,8,1,5,1}, {3,7,1,5,0},
		{3,8,1,5,1}, {4,7,1,5,83}, {12,3,1,5,6}, {12,8,1,5,1}, {13,4,3,5,1},
		{27,3,1,5,8}, {27,5,1,5,1}, {37,3,1,5,7}, {37,8,1,5,0}
	};
	for (const auto &mutation : mutations)
	{
		auto bytes = test.fixture.base.sink.bytes;
		const auto offset = artifactField(bytes, 2, mutation.occurrence, mutation.tag, mutation.type);
		check(offset < bytes.size(), "window mutation locates its exact typed source scalar");
		if (offset == bytes.size()) continue;
		bytes[offset + mutation.byte] = static_cast<unsigned char>(mutation.replacement);
		CohortRead read = { &bytes }; auto options = consumeCohortOptions(test.fixture, test.source, read);
		options.trace.sourceTraceDigest = independentArtifactHash(bytes);
		KernelPerformanceReferenceLedger consumer;
		check(!consumer.beginRun(options) && !consumer.freeze().trace.complete,
			"independently rehashed wrong window kind type identity phase or control provenance fails prevalidation");
	}
	for (unsigned mutation = 0; mutation != 4; ++mutation)
	{
		auto bytes = test.fixture.base.sink.bytes;
		const auto offset = artifactField(bytes, 2, 2, 1, 1), end = artifactRecordEnd(bytes, offset);
		if (mutation == 0) bytes.erase(bytes.begin() + offset, bytes.begin() + end);
		if (mutation == 1) { const std::vector<unsigned char> copy(bytes.begin() + offset, bytes.begin() + end); bytes.insert(bytes.begin() + end, copy.begin(), copy.end()); }
		if (mutation == 2) std::rotate(bytes.begin() + offset, bytes.begin() + end, bytes.begin() + artifactRecordEnd(bytes, end));
		if (mutation == 3) bytes.resize(bytes.size() - 39);
		CohortRead read = { &bytes }; auto options = consumeCohortOptions(test.fixture, test.source, read);
		options.trace.sourceByteCount = bytes.size(); options.trace.sourceTraceDigest = independentArtifactHash(bytes);
		KernelPerformanceReferenceLedger consumer;
		check(!consumer.beginRun(options), "missing repeated reordered window transition or obsolete footer never becomes a valid source");
	}
	CohortRead read = { &test.fixture.base.sink.bytes }; KernelPerformanceReferenceLedger consumer;
	check(consumer.beginRun(consumeCohortOptions(test.fixture, test.source, read)), "native identity negative prevalidates the unchanged real source");
	CohortClock clock; KernelPerformanceLedger timing; KernelPerformanceTimingRunOptions options;
	options.enabled = true; options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE; options.clock = CohortClock::read; options.clockContext = &clock;
	check(timing.beginRun(options), "native identity negative starts real baseline timing");
	CohortCounts counts; unsigned authoritative = 0;
	check(!driveBootstrapCohort(consumer, test, counts, authoritative, &timing, &clock, false, true) &&
		counts.bodies == 0 && counts.inputWrites == 0 && counts.commits == 0 && !consumer.freeze().trace.complete,
		"substituting borrowed83 for actual attempt frame0 fails before capture body or owner commit");
}

void sourceWindowBoundaryFailuresRemainStickyAndClockFree()
{
	for (unsigned mutation = 0; mutation != 10; ++mutation)
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		check(source.beginRun(fixture.options), "window misuse starts a fresh source run");
		const auto begin = bootstrapBoundary(KERNEL_WINDOW_BEGIN, 1, KERNEL_PHASE_COUNT, 83, 83, 83);
		const unsigned before = clocks;
		bool accepted = false;
		if (mutation == 0) accepted = source.observeWindowBoundary(KernelPerformanceWindowBoundary());
		else
		{
			const bool opened = source.observeWindowBoundary(begin);
			check(opened, "window misuse opens one authentic observation window");
			if (!opened) continue;
			if (mutation == 1) accepted = source.observeWindowBoundary(begin);
			if (mutation == 2) accepted = source.sealObservationWindow();
			if (mutation == 3) accepted = source.beginAttempt(fixture.identity(0)).valid();
			if (mutation == 4) accepted = source.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_CONTROL_END, 1, KERNEL_PHASE_COUNT, 83, 83, 0));
			if (mutation >= 5)
			{
				const bool phase = source.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_PHASE_BEGIN, 1, KERNEL_PHASE_OWNER_INTAKE, 83, 83, 83));
				check(phase, "window misuse opens current intake authority"); if (!phase) continue;
				if (mutation == 5) { auto identity = fixture.identity(0); accepted = source.beginAttempt(identity).valid(); }
				if (mutation == 6) accepted = source.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_DEFERRED_START_CONSUMED, 1, KERNEL_PHASE_OWNER_INTAKE, 83, 83, 0));
				if (mutation == 7) accepted = source.observeWindowBoundary(bootstrapBoundary(KERNEL_WINDOW_PHASE_END, 1, KERNEL_PHASE_OWNER_INTAKE, 83, 0, 0));
				if (mutation == 8) { std::thread worker([&]() { accepted = source.observeWindowBoundary(begin); }); worker.join(); }
				if (mutation == 9) accepted = source.observeWindowBoundary(bootstrapBoundary(static_cast<KernelPerformanceWindowBoundaryKind>(1), 1, KERNEL_PHASE_COUNT, 83, 83, 83));
			}
		}
		check(!accepted && clocks == before && !source.freeze().trace.complete && source.runMode() == KERNEL_REFERENCE_THROUGHPUT_BINDING,
			"window misuse preserves sticky incomplete source identity without a clock or invented closure");
	}
}
void sourceControlTransitionCannotInterruptAuthenticatedBody()
{
	BootstrapCohort test;
	const bool recorded = recordBootstrapCohort(test);
	check(recorded, "body/control scope negative starts from a complete bootstrap source");
	if (!recorded) return;
	CohortRead read = { &test.fixture.base.sink.bytes }; KernelPerformanceReferenceLedger consumer;
	check(consumer.beginRun(consumeCohortOptions(test.fixture, test.source, read)), "body/control scope negative prevalidates its source");
	CohortClock clock; KernelPerformanceLedger timing; KernelPerformanceTimingRunOptions options;
	options.enabled = true; options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE; options.clock = CohortClock::read; options.clockContext = &clock;
	check(timing.beginRun(options), "body/control scope negative starts baseline timing");
	CohortCounts counts; unsigned authoritative = 0;
	check(!driveBootstrapCohort(consumer, test, counts, authoritative, &timing, &clock, false, false, true),
		"illegal in-body control transition cannot finish a measured bootstrap cohort");
	const auto snapshot = timing.freeze();
	check((snapshot.errors & KERNEL_PERFORMANCE_ERROR_ORDER) != 0 && !snapshot.phaseAccounting.complete &&
		counts.bodies == 0 && counts.commits == 0 && !consumer.freeze().trace.complete,
		"in-body control failure remains sticky before helper execution or canonical commit");
}

struct RequestBudgetFixture
{
	CohortFixture base;
	std::vector<unsigned> values;
	unsigned rangeCount, neverEnteredRange = ~0U, skipGrantRequest = ~0U;
	unsigned workKind = KERNEL_PERFORMANCE_PATH, subtype = 0, bodySchema = 2;
	bool refundFirst = true, refuseSecond = true;
	RequestBudgetFixture(unsigned requests = 3, unsigned ranges = 3) : values(requests, 1), rangeCount(ranges)
	{
		if (!values.empty()) values[0] = 3;
		if (values.size() > 1) values[1] = 2;
		base.base.options.trace.residentAttemptCapacity = 1;
		base.base.options.trace.residentRangeCapacity = ranges;
	}
	KernelPerformanceDispatchPlan dispatch() const
	{
		auto plan = base.abortedDispatch();
		plan.dispatchOrdinal = 1; plan.bodySchema = bodySchema; plan.rangeCount = rangeCount;
		plan.operationCount = values.size(); plan.sourceGrain = values.size() / rangeCount;
		plan.sourceLimit = values.size(); return plan;
	}
	KernelPerformanceRangePlan range(unsigned ordinal) const
	{
		const auto grain = values.size() / rangeCount;
		KernelPerformanceRangePlan plan = {1, ordinal, 1, ordinal * grain, (ordinal + 1) * grain, grain};
		return plan;
	}
};
struct RequestBudgetRun
{
	CohortSource source;
	std::vector<KernelPerformanceRequestBudget> requests;
	std::vector<KernelPerformanceRangeProgress> ranges;
	std::vector<std::vector<unsigned char> > retained;
	Output discarded = {};
	unsigned materializations = 0, grantCalls = 0, grants = 0, refusals = 0, refunds = 0;
	void initialize(const RequestBudgetFixture &fixture)
	{
		requests.resize(fixture.values.size()); ranges.resize(fixture.rangeCount); retained.resize(fixture.values.size());
		for (unsigned i = 0; i != requests.size(); ++i) requests[i].requestOrdinal = i;
	}
};
struct RequestBudgetInput { const std::vector<unsigned> *values; CohortCounts *counts; };
bool writeRequestBudgetInput(KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const auto &view = *static_cast<const RequestBudgetInput *>(context);
	++view.counts->inputWrites;
	if (!writer.sequence(1, static_cast<unsigned>(view.values->size()))) return false;
	for (unsigned value : *view.values) if (!writer.u32(2, value)) return false;
	return true;
}
bool prepareRequestBudgetAttempt(KernelPerformanceReferenceLedger &ledger,
	const RequestBudgetFixture &fixture, RequestBudgetRun &actual,
	KernelPerformanceAttempt &attempt, bool consume, JobMetricCounter ordinal = 1)
{
	auto identity = fixture.base.identity(ordinal); identity.workKind = fixture.workKind; identity.subtype = fixture.subtype;
	attempt = ledger.beginAttempt(identity);
	const RequestBudgetInput input = { &fixture.values, &actual.source.counts };
	if (!attempt.valid() || !ledger.bindCapturedInput(attempt, 1, fixture.values.size(), writeRequestBudgetInput, &input)) return false;
	if (consume ? !replayCohortDecision(ledger, attempt, fixture.base.base.accepted) :
		!ledger.observeDecision(attempt, fixture.base.base.accepted)) return false;
	if (!ledger.observeDispatch(attempt, fixture.dispatch())) return false;
	for (unsigned i = 0; i != fixture.rangeCount; ++i)
		if (!ledger.observeRangePlan(attempt, fixture.range(i))) return false;
	++actual.source.counts.fallbacks;
	for (unsigned value : fixture.values) actual.source.authoritative += value;
	return ledger.finishAttempt(attempt, fixture.base.base.aborted);
}

// One actual model materialization, called identically in both roles. The
// tiny source predicate is a literal imported-outcome model, not native quota
// evidence. Native path tests separately execute the real128MiB CAS/refund fixture.
bool requestBudgetMaterialization(const RequestBudgetFixture &fixture, unsigned request,
	RequestBudgetRun &actual, KernelPerformanceReferenceLedger *consumer,
	KernelPerformanceInlineOwnerSerial extent, bool changeAllocationOutcome = false)
{
	++actual.materializations;
	if (request == fixture.skipGrantRequest) return false;
	auto &pod = actual.requests[request];
	pod.grantSite = 1; pod.localGrantOrdinal = 1;
	pod.requestedBytes = static_cast<JobMetricCounter>(fixture.values[request]) * 32;
	// Baseline's empty physical quota would grant every request. Its output
	// starts true so only the authenticated source can preserve a refusal.
	bool granted = consumer != 0 || !fixture.refuseSecond || request != 1;
	++actual.grantCalls;
	if (consumer != 0 && !consumer->replayRequestBudgetGrant(extent, request, 1, 1, pod.requestedBytes, granted)) return false;
	bool completed = true;
	if (!granted)
	{
		++actual.refusals; pod.disposition = KERNEL_REQUEST_BUDGET_REFUSED;
	}
	else
	{
		++actual.grants; pod.grantedBytes = pod.requestedBytes;
		try
		{
			// A real test-helper exception exercises the model catch. This is
			// not a requested source disposition or a native allocation claim.
			if (fixture.refundFirst && request == 0 && !changeAllocationOutcome)
				throw std::runtime_error("model materialization allocation failed");
			actual.retained[request].assign(static_cast<std::size_t>(pod.requestedBytes), 1);
			actual.discarded.sum += static_cast<unsigned>(actual.retained[request].size());
			pod.disposition = KERNEL_REQUEST_BUDGET_RETAINED; pod.consumedBytes = pod.grantedBytes;
		}
		catch (...)
		{
			++actual.refunds; pod.disposition = KERNEL_REQUEST_BUDGET_REFUNDED;
			pod.refundSite = 2; pod.localRefundOrdinal = 2; pod.refundedBytes = pod.grantedBytes;
			actual.retained[request].clear();
			completed = false;
		}
	}
	if (consumer != 0 && !consumer->finishInlineRequestBudget(extent, pod)) return false;
	return completed;
}
bool runRequestBudgetBody(const RequestBudgetFixture &fixture, unsigned rangeOrdinal,
	RequestBudgetRun &actual, KernelPerformanceCheckpointProbe &probe,
	KernelPerformanceReferenceLedger *consumer = 0, KernelPerformanceInlineBody body = KernelPerformanceInlineBody(),
	CohortClock *clock = 0, bool changeAllocationOutcome = false, unsigned workDelay = 1)
{
	const auto plan = fixture.range(rangeOrdinal);
	++actual.source.counts.bodies;
	KernelPerformanceCheckpoint last = {41, plan.begin, plan.end};
	bool cancelled = probe.cancelled(last, consumer != 0);
	bool completed = !cancelled;
	JobMetricCounter units = 0;
	for (JobMetricCounter request = plan.begin; completed && request != plan.end; ++request)
	{
		last = {42, request, plan.end};
		cancelled = probe.cancelled(last, consumer != 0);
		if (cancelled) { completed = false; break; }
		KernelPerformanceInlineOwnerSerial extent;
		if (consumer != 0)
		{
			clock->now += workDelay; extent = consumer->beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
			if (!extent.valid()) return false;
		}
		completed = requestBudgetMaterialization(fixture, static_cast<unsigned>(request), actual, consumer, extent, changeAllocationOutcome);
		if (consumer != 0)
		{
			clock->now += workDelay;
			if (!consumer->endInlineOwnerSerial(extent)) return false;
		}
		if (completed) ++units;
	}
	if (completed) last = {43, plan.end, plan.end};
	if (!probe.finish(last, units, cancelled ? KERNEL_RANGE_CANCELLED : completed ? KERNEL_RANGE_COMPLETED : KERNEL_RANGE_FAILED)) return false;
	actual.ranges[rangeOrdinal].checkpoint = probe.snapshot();
	actual.ranges[rangeOrdinal].publication = KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL;
	return true;
}
bool recordRequestBudgetFixture(RequestBudgetFixture &fixture, RequestBudgetRun &actual)
{
	actual.initialize(fixture); KernelPerformanceReferenceLedger ledger; KernelPerformanceAttempt attempt;
	if (!ledger.beginRun(fixture.base.base.options) || !prepareRequestBudgetAttempt(ledger, fixture, actual, attempt, false) ||
		!ledger.sealObservationWindow()) return false;
	for (unsigned i = 0; i != fixture.rangeCount; ++i)
	{
		const auto plan = fixture.range(i);
		if (i != fixture.neverEnteredRange)
		{
			KernelPerformanceCheckpointProbe probe;
			if (!probe.beginRecord() || !runRequestBudgetBody(fixture, i, actual, probe)) return false;
		}
		// Imported after the model release point. No group-complete flag;
		// Native tests prove the physical release/acquire boundary independently.
		for (JobMetricCounter request = plan.begin; request != plan.end; ++request)
			if (!ledger.observeReleasedRequestBudget(attempt, plan, actual.requests[static_cast<std::size_t>(request)])) return false;
		if (!ledger.observeReleasedRange(attempt, plan, actual.ranges[i])) return false;
	}
	if (!ledger.reapAttempt(attempt, fixture.base.base.reap) || !ledger.sealExecutionClosure()) return false;
	actual.source.snapshot = ledger.freeze();
	return actual.source.snapshot.trace.complete && !actual.source.snapshot.complete;
}
struct RequestBudgetConsumer
{
	RequestBudgetFixture fixture; RequestBudgetRun recorded, actual; CohortRead read;
	KernelPerformanceReferenceLedger reference; KernelPerformanceLedger timing; CohortClock clock;
	KernelPerformanceAttempt attempt; KernelPerformanceInterval completion;
	RequestBudgetConsumer(unsigned requests = 3, unsigned ranges = 3) : fixture(requests, ranges), read{ &fixture.base.base.sink.bytes } {}
	bool begin()
	{
		if (!recordRequestBudgetFixture(fixture, recorded))
		{ check(false, "budget consumer fixture requires complete source request records before execution"); return false; }
		actual.initialize(fixture);
		if (!reference.beginRun(consumeCohortOptions(fixture.base, recorded.source, read)))
		{ check(false, "budget consumer prevalidates its complete immutable source"); return false; }
		KernelPerformanceTimingRunOptions options; options.enabled = true; options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
		options.clock = CohortClock::read; options.clockContext = &clock;
		if (!timing.beginRun(options)) return false;
		const auto frame = timing.beginFrame(1, 7, KernelPerformanceSchedulerBoundary());
		const JobMetricCounter starts[] = {110,125,150,255,270}, ends[] = {120,145,250,265,290};
		for (unsigned i = 0; i != 5; ++i)
		{
			clock.now = starts[i]; if (!timing.beginPhase(frame, static_cast<KernelPerformancePhase>(i))) return false;
			if (i == KERNEL_PHASE_SPATIAL_WORK && !prepareRequestBudgetAttempt(reference, fixture, actual, attempt, true)) return false;
			clock.now = ends[i]; if (!timing.endPhase(frame, static_cast<KernelPerformancePhase>(i))) return false;
		}
		clock.now = 300;
		if (!timing.endFrame(frame, 7, KernelPerformanceSchedulerBoundary()) || !timing.sealAdmissions() || !reference.sealObservationWindow()) return false;
		clock.now = 400; completion = timing.beginCompletionSerial();
		return completion.valid();
	}
	bool open(unsigned i, KernelPerformanceInlineBody &body, KernelPerformanceCheckpointProbe &probe)
	{
		const auto action = reference.beginInlineBody(attempt, fixture.range(i), timing, body, probe);
		return i == fixture.neverEnteredRange ? action == KERNEL_INLINE_SKIP_SOURCE_NEVER_ENTERED : action == KERNEL_INLINE_EXECUTE;
	}
	bool execute(unsigned i, bool changeAllocationOutcome = false, unsigned workDelay = 1)
	{
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		if (!open(i, body, probe)) return false;
		if (i != fixture.neverEnteredRange)
		{
			if (!runRequestBudgetBody(fixture, i, actual, probe, &reference, body, &clock, changeAllocationOutcome, workDelay)) return false;
			++clock.now;
			if (!reference.finishInlineBody(body, actual.ranges[i])) return false;
		}
		const auto plan = fixture.range(i);
		for (JobMetricCounter request = plan.begin; request != plan.end; ++request)
			if (!reference.observeReleasedRequestBudget(attempt, plan, actual.requests[static_cast<std::size_t>(request)])) return false;
		return reference.observeReleasedRange(attempt, plan, actual.ranges[i]);
	}
	bool finish(JobMetricCounter at)
	{
		if (!reference.reapAttempt(attempt, fixture.base.base.reap) || !reference.sealExecutionClosure()) return false;
		clock.now = at;
		return timing.endCompletionSerial(completion) && timing.sealExecutionClosure(KernelPerformanceSchedulerBoundary());
	}
};

void sourceRequestBudgetRoundTripKeepsRefusalAfterEarlierOwnerRefund(bool delayed = false)
{
	RequestBudgetConsumer test; const unsigned sourceClocks = clocks;
	if (!test.begin()) return;
	const auto &source = test.recorded.source.snapshot;
	check(source.trace.recordCount == 19 && source.trace.logicalEventCount == 19 && source.trace.attemptCount == 1 &&
		source.trace.admittedAttemptCount == 1 && source.trace.abortedAfterAdmissionAttemptCount == 1 && source.trace.reapCount == 1 &&
		source.trace.capturedAttemptCount == 1 && source.trace.capturedOperationCount == 3 && source.trace.rangeCount == 3 &&
		source.trace.releasedRangeCount == 3 && source.trace.residentAttemptHighWater == 1 && source.trace.residentRangeHighWater == 3 &&
		!source.complete && source.streamCount == 0 && clocks == sourceClocks,
		"literal19-record budget source retains three requests and aborts without success or source clock");
	for (unsigned i = 0; i != 3; ++i)
	{
		test.clock.now = 410 + i * 40;
		const bool executed = test.execute(i, false, delayed ? 3 : 1);
		check(executed, "each late request uses its source grant once inside actual whole materialization");
		if (!executed) return;
	}
	check(test.finish(delayed ? 580 : 550), "request settlements and all released ranges precede real completion closure");
	const auto consumed = test.reference.freeze(); const auto timed = test.timing.freeze();
	check(consumed.trace.complete && consumed.trace.digest.equals(source.trace.digest) && consumed.trace.byteCount == source.trace.byteCount &&
		consumed.trace.recordCount == 19 && !consumed.complete && consumed.streamCount == 0,
		"budget consumer reproduces exact source bytes and never invents a validated output stream");
	for (const auto *run : { &test.recorded, &test.actual })
		check(run->source.counts.bodies == 3 && run->materializations == 3 && run->grantCalls == 3 && run->grants == 2 &&
			run->refusals == 1 && run->refunds == 1 && run->source.counts.inputWrites == 1 && run->source.counts.outputWrites == 0 &&
			run->source.counts.detached == 0 && run->source.counts.commits == 0 && run->source.counts.fallbacks == 1 &&
			run->source.authoritative == 6 && run->discarded.sum == 32,
			"same actual model helper fallback and allocation catch run once without detached recomputation");
	check(test.actual.requests[0].refundedBytes == 96 && test.actual.requests[1].requestedBytes == 64 &&
		test.actual.requests[1].disposition == KERNEL_REQUEST_BUDGET_REFUSED && test.actual.requests[1].grantedBytes == 0 &&
		test.actual.requests[2].grantedBytes == 32 && test.actual.requests[2].consumedBytes == 32,
		"earlier owner refund cannot turn request1 refusal into a grant or erase request2 grant");
	JobMetricCounter pure = 0, serial = 0;
	for (const auto &row : timed.phaseAccounting.phases) { pure += row.pureNanoseconds; serial += row.serialNanoseconds; }
	check(timed.phaseAccounting.complete && timed.phaseAccounting.frameNanoseconds == 200 &&
		timed.phaseAccounting.unscopedSerialNanoseconds == 40 && timed.phaseAccounting.completionSerialNanoseconds == (delayed ? 180 : 150) &&
		pure == 0 && serial == 160 && timed.phaseAccounting.frameNanoseconds + timed.phaseAccounting.completionSerialNanoseconds == (delayed ? 380 : 350),
		"aborted request search materialization refund and late import are wholly serial with excluded wall gap preserved");
}

void sourceRequestBudgetPrefixIncludesEveryDeclaredRequest()
{
	for (unsigned variant = 0; variant != 3; ++variant)
	{
		RequestBudgetConsumer test(4, 2);
		if (variant == 1) test.fixture.neverEnteredRange = 1;
		if (variant == 2) { test.fixture.refundFirst = false; test.fixture.skipGrantRequest = 0; }
		if (!test.begin()) continue;
		check(test.execute(0) && test.execute(1) && test.finish(550), "budget-aware ranges retain every untouched request slot through owner release");
		const auto result = test.reference.freeze();
		check(result.trace.complete && result.trace.recordCount == 18 && result.trace.capturedOperationCount == 4 &&
			test.actual.requests[1].disposition == KERNEL_REQUEST_BUDGET_NOT_REACHED && test.actual.requests[1].grantSite == 0 &&
			test.actual.requests[1].requestedBytes == 0 && test.actual.requests[1].consumedBytes == 0,
			"early request failure preserves explicit NOT_REACHED for the unexecuted request rather than a zero-byte grant");
		if (variant == 1) check(test.actual.source.counts.bodies == 1 && test.actual.requests[2].disposition == KERNEL_REQUEST_BUDGET_NOT_REACHED &&
			test.actual.requests[3].disposition == KERNEL_REQUEST_BUDGET_NOT_REACHED,
			"never-entered range imports all request absences with no helper or probe authority");
	}
}

void sourceRequestBudgetZeroByteEventsRemainDifferentFromNotReached()
{
	RequestBudgetConsumer test;
	std::fill(test.fixture.values.begin(), test.fixture.values.end(), 0);
	if (!test.begin()) return;
	check(test.execute(0) && test.execute(1) && test.execute(2) && test.finish(550),
		"real zero-byte grant refusal and refund events retain their independent source outcomes");
	const auto snapshot = test.reference.freeze();
	check(snapshot.trace.complete && snapshot.trace.recordCount == 19 && test.actual.grants == 2 &&
		test.actual.refusals == 1 && test.actual.refunds == 1 && test.actual.materializations == 3 &&
		test.actual.requests[0].disposition == KERNEL_REQUEST_BUDGET_REFUNDED && test.actual.requests[0].grantSite == 1 &&
		test.actual.requests[0].refundSite == 2 && test.actual.requests[0].refundedBytes == 0 &&
		test.actual.requests[1].disposition == KERNEL_REQUEST_BUDGET_REFUSED && test.actual.requests[1].localGrantOrdinal == 1 &&
		test.actual.requests[1].requestedBytes == 0 && test.actual.requests[2].disposition == KERNEL_REQUEST_BUDGET_RETAINED &&
		test.actual.requests[2].localGrantOrdinal == 1 && test.actual.requests[2].consumedBytes == 0,
		"disposition and local sites distinguish three reached zero-byte events from an absent grant");
}

void sourceRequestBudgetMalformedRehashedFieldsCannotInitialize()
{
	RequestBudgetFixture fixture; RequestBudgetRun source;
	const bool recorded = recordRequestBudgetFixture(fixture, source);
	check(recorded, "budget grammar mutation matrix requires one complete source artifact"); if (!recorded) return;
	struct Mutation { unsigned occurrence, tag, type, byte, value; };
	const Mutation mutations[] = {
		{0,1,1,5,99}, {0,2,3,5,10}, {0,3,3,5,2}, {0,4,3,5,2}, {0,5,1,5,1}, {0,6,3,5,1},
		{0,7,1,5,2}, {0,8,1,5,4}, {0,9,1,5,2}, {0,10,3,5,2}, {0,11,3,5,95}, {0,12,3,5,95},
		{0,13,1,5,1}, {0,14,3,5,1}, {0,15,3,5,95}, {0,16,3,5,1}, {0,6,3,0,1},
		{1,8,1,5,0}, {1,12,3,5,64}, {2,16,3,5,31}
	};
	for (const auto &mutation : mutations)
	{
		auto bytes = fixture.base.base.sink.bytes;
		const auto at = artifactField(bytes, 13, mutation.occurrence, mutation.tag, mutation.type);
		check(at < bytes.size(), "budget mutation locates its independent typed field"); if (at == bytes.size()) continue;
		bytes[at + mutation.byte] = static_cast<unsigned char>(mutation.value);
		CohortRead read = { &bytes }; auto options = consumeCohortOptions(fixture.base, source.source, read);
		options.trace.sourceTraceDigest = independentArtifactHash(bytes); KernelPerformanceReferenceLedger consumer;
		check(!consumer.beginRun(options) && !consumer.freeze().trace.complete,
			"rehashed malformed request identity state local sites or reconciliation fails structural prevalidation");
	}
	for (unsigned variant = 0; variant != 6; ++variant)
	{
		auto bytes = fixture.base.base.sink.bytes; const auto at = artifactField(bytes, 13, 0, 1, 1), end = artifactRecordEnd(bytes, at);
		if (variant == 0) bytes.erase(bytes.begin() + at, bytes.begin() + end);
		if (variant == 1) { std::vector<unsigned char> copy(bytes.begin() + at, bytes.begin() + end); bytes.insert(bytes.begin() + end, copy.begin(), copy.end()); }
		if (variant == 2) std::rotate(bytes.begin() + at, bytes.begin() + end, bytes.begin() + artifactRecordEnd(bytes, end));
		if (variant == 3) { const auto last = artifactField(bytes, 13, 2, 1, 1); bytes.erase(bytes.begin() + last, bytes.begin() + artifactRecordEnd(bytes, last)); }
		if (variant == 4) { const auto value = artifactField(bytes, 13, 0, 16, 3); memset(bytes.data() + value + 5, 0xff, 8); }
		if (variant == 5) { const auto schema = artifactField(bytes, 6, 0, 5, 1); bytes[schema + 5] = 1; }
		CohortRead read = { &bytes }; auto options = consumeCohortOptions(fixture.base, source.source, read);
		options.trace.sourceByteCount = bytes.size(); options.trace.sourceTraceDigest = independentArtifactHash(bytes);
		KernelPerformanceReferenceLedger consumer;
		check(!consumer.beginRun(options), "missing duplicate reordered overflowing or wrong-profile budget prefix cannot become source evidence");
	}
}

void sourceRequestBudgetCallsRequireCurrentTypedExtentAndOnceOnlyState()
{
	for (unsigned mutation = 0; mutation != 17; ++mutation)
	{
		RequestBudgetConsumer test; if (!test.begin()) continue;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		const bool opened = test.open(0, body, probe); check(opened, "budget authority negative opens only the current source body"); if (!opened) continue;
		KernelPerformanceInlineOwnerSerial extent;
		if (mutation != 0) extent = test.reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		if (mutation != 0) check(extent.valid(), "budget authority negative enters actual typed materialization");
		bool granted = true, accepted = false;
		if (mutation <= 5)
		{
			if (mutation == 5) { std::thread worker([&]() { accepted = test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted); }); worker.join(); }
			else accepted = test.reference.replayRequestBudgetGrant(extent, mutation == 1 ? 1 : 0,
				mutation == 2 ? 2 : 1, mutation == 3 ? 2 : 1, mutation == 4 ? 95 : 96, granted);
			check(granted, "rejected grant lookup preserves its caller output sentinel");
		}
		else if (mutation == 6) accepted = test.reference.finishInlineRequestBudget(extent, test.recorded.requests[0]);
		else
		{
			check(test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted) && granted,
				"once-only negative first reads the exact current authenticated grant");
			if (mutation == 7) accepted = test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted);
			if (mutation == 8) accepted = test.reference.endInlineOwnerSerial(extent);
			if (mutation == 9) accepted = test.reference.finishInlineBody(body, test.recorded.ranges[0]);
			if (mutation == 10) accepted = test.reference.observeReleasedRequestBudget(test.attempt, test.fixture.range(0), test.recorded.requests[0]);
			if (mutation >= 11 && mutation <= 14)
			{
				auto changed = test.recorded.requests[0];
				if (mutation == 11) changed.requestOrdinal = 1;
				if (mutation == 12) changed.refundSite = 1;
				if (mutation == 13) changed.refundedBytes = 95;
				if (mutation == 14) { changed.disposition = KERNEL_REQUEST_BUDGET_RETAINED; changed.refundSite = 0; changed.localRefundOrdinal = 0; changed.refundedBytes = 0; changed.consumedBytes = 96; }
				accepted = test.reference.finishInlineRequestBudget(extent, changed);
			}
			if (mutation >= 15)
			{
				check(test.reference.finishInlineRequestBudget(extent, test.recorded.requests[0]), "exact actual settlement closes once before reuse negative");
				if (mutation == 15) accepted = test.reference.finishInlineRequestBudget(extent, test.recorded.requests[0]);
				else { check(test.reference.endInlineOwnerSerial(extent), "settled extent closes before stale-token lookup"); accepted = test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted); }
			}
		}
		check(!accepted && test.actual.source.counts.bodies == 0 && test.actual.source.counts.commits == 0 &&
			!test.reference.freeze().trace.complete && !test.timing.freeze().phaseAccounting.complete &&
			test.reference.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"invalid budget identity owner scope order settlement or token reuse is sticky before model helper or commit");
	}
}

void sourceRequestBudgetActualAllocationAndMissingGrantCannotBeReplaced()
{
	for (unsigned mutation = 0; mutation != 3; ++mutation)
	{
		RequestBudgetConsumer test;
		if (mutation == 2) test.fixture.skipGrantRequest = 0;
		if (!test.begin()) continue;
		if (mutation == 1) test.fixture.skipGrantRequest = 0;
		if (mutation == 2) test.fixture.skipGrantRequest = ~0U;
		check(!test.execute(0, mutation == 0) && !test.reference.freeze().trace.complete &&
			!test.timing.freeze().phaseAccounting.complete && test.actual.source.counts.commits == 0,
			"actual allocation or reached-site divergence fails instead of forcing the expected source outcome");
		if (mutation == 0) check(test.actual.refunds == 0 && test.actual.requests[0].consumedBytes == 96,
			"consumer's real successful allocation remains observed rather than forged into a source refund");
	}
}

void sourceRequestBudgetOwnerImportMustFinishBeforeRangeRelease()
{
	for (unsigned mutation = 0; mutation != 7; ++mutation)
	{
		RequestBudgetFixture fixture; RequestBudgetRun actual; actual.initialize(fixture);
		fixture.base.base.options.trace.residentAttemptCapacity = 2;
		const unsigned beforeClocks = clocks;
		KernelPerformanceReferenceLedger source; KernelPerformanceAttempt attempt;
		check(source.beginRun(fixture.base.base.options) && prepareRequestBudgetAttempt(source, fixture, actual, attempt, false),
			"budget source-order negative prepares actual captured dispatch and abort");
		KernelPerformanceCheckpointProbe probe; check(probe.beginRecord() && runRequestBudgetBody(fixture, 0, actual, probe), "source-order negative gathers actual released request POD");
		bool accepted = false;
		if (mutation == 0) accepted = source.observeReleasedRange(attempt, fixture.range(0), actual.ranges[0]);
		else
		{
			auto pod = actual.requests[0]; auto range = fixture.range(0);
			if (mutation == 1) pod.requestOrdinal = 1;
			if (mutation == 2) ++range.dispatchOrdinal;
			if (mutation == 3) pod.consumedBytes = ~static_cast<JobMetricCounter>(0);
			if (mutation >= 4)
			{
				const bool first = source.observeReleasedRequestBudget(attempt, range, pod);
				check(first, "source-order negative imports one well-formed released request first"); if (!first) continue;
				if (mutation == 4) accepted = source.observeReleasedRequestBudget(attempt, range, pod);
				if (mutation == 5) accepted = source.reapAttempt(attempt, fixture.base.base.reap);
				if (mutation == 6)
				{
					const auto unrelated = source.beginAttempt(fixture.base.identity(2));
					accepted = unrelated.valid();
				}
			}
			else accepted = source.observeReleasedRequestBudget(attempt, range, pod);
		}
		check(!accepted && !source.freeze().trace.complete && clocks == beforeClocks,
			"missing wrong duplicate interrupted or unreaped budget range cannot qualify a source prefix");
	}
}

void sourceRequestBudgetLookupRemainsBoundedAcrossLargeRangePrefix()
{
	RequestBudgetConsumer test(512, 1); test.fixture.refundFirst = test.fixture.refuseSecond = false;
	if (!test.begin()) return;
	const auto before = test.read.bytesRead;
	check(test.execute(0) && test.finish(test.clock.now + 10), "large released request prefix consumes every real model materialization once");
	const auto snapshot = test.reference.freeze();
	check(snapshot.trace.complete && test.actual.materializations == 512 && test.actual.grantCalls == 512 &&
		snapshot.trace.residentRangeHighWater == 1 && snapshot.trace.residentAttemptHighWater == 1 &&
		test.read.bytesRead - before <= 12 * test.recorded.source.snapshot.trace.byteCount + 4 * 65536,
		"request grant lookups reuse monotone live-range offsets rather than rescanning full trace per request");
}

void sourceRequestBudgetWireAndFooterAreLiteral()
{
	RequestBudgetFixture fixture; RequestBudgetRun source;
	const bool recorded = recordRequestBudgetFixture(fixture, source);
	check(recorded, "budget literal fixture records a complete19-record source"); if (!recorded) return;
	const unsigned char request[] = {
		1, 1, 0, 0, 0,13, 0, 0, 0,
		3, 2, 0, 0, 0,11, 0, 0, 0, 0, 0, 0, 0,
		3, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		1, 5, 0, 0, 0, 0, 0, 0, 0,
		3, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 7, 0, 0, 0, 1, 0, 0, 0,
		1, 8, 0, 0, 0, 3, 0, 0, 0,
		1, 9, 0, 0, 0, 1, 0, 0, 0,
		3,10, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3,11, 0, 0, 0,96, 0, 0, 0, 0, 0, 0, 0,
		3,12, 0, 0, 0,96, 0, 0, 0, 0, 0, 0, 0,
		1,13, 0, 0, 0, 2, 0, 0, 0,
		3,14, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
		3,15, 0, 0, 0,96, 0, 0, 0, 0, 0, 0, 0,
		3,16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	static_assert(sizeof(request) == 184, "one budget record is six u32 and ten u64 fields");
	const unsigned char footer[] = {
		1, 1, 0, 0, 0,11, 0, 0, 0,
		3, 2, 0, 0, 0,19, 0, 0, 0, 0, 0, 0, 0,
		3, 3, 0, 0, 0,19, 0, 0, 0, 0, 0, 0, 0,
		3, 4, 0, 0, 0,19, 0, 0, 0, 0, 0, 0, 0,
		3, 5, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 6, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 8, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3, 9, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3,10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3,12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		5,14, 0, 0, 0, 1,
		5,15, 0, 0, 0, 1,
		3,16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3,17, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
		3,18, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		3,19, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
		3,20, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
		3,21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,22, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
		3,23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3,29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	static_assert(sizeof(footer) == 359, "complete V1 footer has exactly29 fields");
	const auto &bytes = fixture.base.base.sink.bytes;
	const auto at = artifactField(bytes, 13, 0, 1, 1), foot = artifactField(bytes, 11, 0, 1, 1);
	check(at < bytes.size() && bytes.size() - at >= sizeof(request) &&
		memcmp(bytes.data() + at, request, sizeof(request)) == 0 && artifactRecordEnd(bytes, at) == at + sizeof(request),
		"released request identity local event sites and exact byte reconciliation use one independent184-byte grammar");
	check(foot < bytes.size() && bytes.size() - foot == sizeof(footer) && memcmp(bytes.data() + foot, footer, sizeof(footer)) == 0,
		"budget records reconcile through the unchanged independent29-field final footer");
}

void sourceRequestBudgetForeignExtentAndUnbudgetedBodyCannotAuthorizeGrant()
{
	{
		RequestBudgetConsumer first, second;
		if (!first.begin() || !second.begin()) return;
		KernelPerformanceInlineBody a, b; KernelPerformanceCheckpointProbe pa, pb;
		check(first.open(0, a, pa) && second.open(0, b, pb), "foreign budget fixture opens scalar-coincident bodies in separate ledgers");
		const auto extent = first.reference.beginInlineOwnerSerial(a, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		const auto other = second.reference.beginInlineOwnerSerial(b, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		check(extent.valid() && other.valid(), "both independent typed extents are valid before the foreign-token mutation");
		bool granted = true; const unsigned before = second.clock.reads;
		check(!second.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted) && granted && second.clock.reads == before &&
			!second.reference.freeze().trace.complete, "coincident foreign owner-serial token cannot access another request's source budget");
	}
	{
		ConsumerAtSuccessfulBody test;
		if (!test.begin() || !test.captureAndPlan()) return;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		check(test.reference.beginInlineBody(test.s, test.fixture.range(0), test.timing, body, probe) == KERNEL_INLINE_EXECUTE,
			"unbudgeted schema1 control retains existing ordinary authenticated body behavior");
		const auto extent = test.reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		bool granted = false; const unsigned before = test.time.reads;
		check(!test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted) && !granted && test.time.reads == before &&
			!test.reference.freeze().trace.complete, "typed materialization alone cannot reinterpret unbudgeted bodySchema1 as budget profile2");
	}
	{
		RequestBudgetConsumer test; if (!test.begin()) return;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		check(test.open(0, body, probe), "stale budget fixture opens its old body");
		const auto extent = test.reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		test.reference.freeze();
		check(test.reference.beginRun(consumeCohortOptions(test.fixture.base, test.recorded.source, test.read)), "new generation prevalidates independently without reusing old scope authority");
		bool granted = false;
		check(!test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted) && !granted && !test.reference.freeze().trace.complete,
			"old-generation typed extent cannot grant from a new consumer run");
	}
}

void sourceRequestBudgetRangeSlotReuseRetainsAttemptProvenance()
{
	for (unsigned mutation = 0; mutation != 2; ++mutation)
	{
		RequestBudgetFixture fixture(1, 1); fixture.refundFirst = fixture.refuseSecond = false;
		KernelPerformanceReferenceLedger ledger; check(ledger.beginRun(fixture.base.base.options), "budget reuse starts one-slot resident metadata");
		KernelPerformanceAttempt oldAttempt;
		bool completed = true;
		for (unsigned batch = 0; batch != 2; ++batch)
		{
			RequestBudgetRun actual; actual.initialize(fixture); KernelPerformanceAttempt attempt;
			check(prepareRequestBudgetAttempt(ledger, fixture, actual, attempt, false, batch + 1), "each reused request attempt captures once with increasing original identity");
			KernelPerformanceCheckpointProbe probe; check(probe.beginRecord() && runRequestBudgetBody(fixture, 0, actual, probe), "reused source slot executes its own actual model request");
			if (batch == 1 && mutation != 0)
			{
				check(!ledger.observeReleasedRequestBudget(oldAttempt, fixture.range(0), actual.requests[0]) && !ledger.freeze().trace.complete,
					"reused range/request coordinates cannot revive an already reaped attempt's opaque provenance");
				completed = false; break;
			}
			const bool imported = ledger.observeReleasedRequestBudget(attempt, fixture.range(0), actual.requests[0]);
			check(imported, "reused bounded range imports exactly its current attempt's released request");
			if (!imported) { completed = false; break; }
			check(ledger.observeReleasedRange(attempt, fixture.range(0), actual.ranges[0]) && ledger.reapAttempt(attempt, fixture.base.base.reap),
				"all request and range acknowledgements precede each genuine attempt reap");
			oldAttempt = attempt;
		}
		if (completed)
		{
			check(ledger.sealObservationWindow() && ledger.sealExecutionClosure(), "two sequential budget attempts close using one live slot");
			const auto result = ledger.freeze();
			check(result.trace.complete && result.trace.recordCount == 22 && result.trace.attemptCount == 2 &&
				result.trace.capturedOperationCount == 2 && result.trace.residentAttemptHighWater == 1 && result.trace.residentRangeHighWater == 1,
				"historical request/attempt count never inflates reusable core metadata capacity");
		}
	}
}

void sourceRequestBudgetQueriesAreInertWithoutConsumerAuthority()
{
	for (unsigned mode = 0; mode != 3; ++mode)
	{
		KernelPerformanceReferenceLedger ledger; RequestBudgetFixture fixture;
		check(mode == 2 ? ledger.beginRun(fixture.base.base.options) : ledger.beginRun(mode == 0 ? KERNEL_REFERENCE_DISABLED : KERNEL_REFERENCE_THROUGHPUT_BINDING),
			"inert budget query starts its actual disabled untraced or source-record role");
		bool actualGrant = mode != 0; const bool beforeGrant = actualGrant; const unsigned before = clocks;
		KernelPerformanceRequestBudget pod; pod.requestOrdinal = 0;
		check(!ledger.replayRequestBudgetGrant(KernelPerformanceInlineOwnerSerial(), 0, 1, 1, 96, actualGrant) && actualGrant == beforeGrant &&
			!ledger.finishInlineRequestBudget(KernelPerformanceInlineOwnerSerial(), pod) && clocks == before,
			"non-consumer budget lookup never changes the real predicate output or reads a clock");
	}
}

void sourceRequestBudgetExactReadFailureCannotGrantFromUnverifiedBytes()
{
	for (unsigned failure = 0; failure != 2; ++failure)
	{
		// The >64KiB budget prefix makes beginInlineBody's mandatory lookahead
		// evict its first request. No public cache/reset hook is manufactured.
		RequestBudgetConsumer test(512, 1); test.fixture.refundFirst = test.fixture.refuseSecond = false;
		if (!test.begin()) continue;
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		check(test.open(0, body, probe), "read-failure fixture authenticates the range outcome before actual request execution");
		const auto extent = test.reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
		check(extent.valid(), "read-failure fixture opens the exact materialization scope");
		test.read.fail = failure == 0; test.read.throws = failure != 0;
		bool grant = true; const unsigned reads = test.read.calls;
		check(!test.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, grant) && grant && test.read.calls > reads &&
			test.actual.materializations == 0 && !test.reference.freeze().trace.complete && !test.timing.freeze().phaseAccounting.complete,
			"false or throwing exact read at a budget site fails before allocating or trusting a source grant");
	}
}

void sourceRequestBudgetProfileMismatchFailsBeforeImport()
{
	for (unsigned variant = 0; variant != 3; ++variant)
	{
		RequestBudgetFixture fixture(1, 1); RequestBudgetRun actual; actual.initialize(fixture);
		if (variant == 0) fixture.bodySchema = 1;
		if (variant == 1) fixture.workKind = KERNEL_PERFORMANCE_PHYSICS;
		if (variant == 2) fixture.subtype = 1;
		KernelPerformanceReferenceLedger ledger; KernelPerformanceAttempt attempt;
		check(ledger.beginRun(fixture.base.base.options) && prepareRequestBudgetAttempt(ledger, fixture, actual, attempt, false),
			"wrong-profile fixture has a valid ordinary full attempt before budget import");
		KernelPerformanceCheckpointProbe probe;
		check(probe.beginRecord() && runRequestBudgetBody(fixture, 0, actual, probe), "wrong-profile negative independently gathers one request outcome");
		check(!ledger.observeReleasedRequestBudget(attempt, fixture.range(0), actual.requests[0]) && !ledger.freeze().trace.complete,
			"budget POD cannot silently change ordinary schema1 physics or direct-path subtype into compiled ordinary profile2");
	}
}

struct LateBudgetWindowFixture
{
	RequestBudgetFixture budget;
	RequestBudgetRun recorded;
	bool committedSource = false;
};
struct LateBudgetWindowRun
{
	LateBudgetWindowFixture &test;
	RequestBudgetRun actual;
	CohortRead read;
	KernelPerformanceReferenceLedger reference;
	KernelPerformanceLedger timing;
	CohortClock clock;
	KernelPerformanceAttempt attempt;
	KernelPerformanceFrame frame;
	KernelPerformanceSnapshot timed;
	bool baseline = false;
	explicit LateBudgetWindowRun(LateBudgetWindowFixture &fixture) : test(fixture), read{ &fixture.budget.base.base.sink.bytes } {}
	bool boundary(KernelPerformanceWindowBoundaryKind kind, JobMetricCounter sample,
		KernelPerformancePhase phase, unsigned entry, unsigned actualFrame)
	{
		return reference.observeWindowBoundary(bootstrapBoundary(kind, sample, phase, entry, entry, actualFrame));
	}
	bool begin(bool consume)
	{
		baseline = consume; actual.initialize(test.budget);
		if (!reference.beginRun(consume ? consumeCohortOptions(test.budget.base, test.recorded.source, read) : test.budget.base.base.options)) return false;
		if (!consume) return true;
		KernelPerformanceTimingRunOptions options; options.enabled = true; options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
		options.clock = CohortClock::read; options.clockContext = &clock;
		return timing.beginRun(options);
	}
	bool capture(unsigned actualFrame)
	{
		auto identity = test.budget.base.identity(1); identity.ownerFrame = actualFrame;
		attempt = reference.beginAttempt(identity);
		const RequestBudgetInput input = { &test.budget.values, &actual.source.counts };
		if (!attempt.valid() || !reference.bindCapturedInput(attempt, 1, test.budget.values.size(), writeRequestBudgetInput, &input)) return false;
		if (baseline ? !replayCohortDecision(reference, attempt, test.budget.base.base.accepted) :
			!reference.observeDecision(attempt, test.budget.base.base.accepted)) return false;
		if (!reference.observeDispatch(attempt, test.budget.dispatch())) return false;
		for (unsigned i = 0; i != test.budget.rangeCount; ++i)
			if (!reference.observeRangePlan(attempt, test.budget.range(i))) return false;
		if (test.committedSource) return true;
		++actual.source.counts.fallbacks;
		for (unsigned value : test.budget.values) actual.source.authoritative += value;
		return reference.finishAttempt(attempt, test.budget.base.base.aborted);
	}
	bool firstWindow(unsigned actualAttemptFrame = 7)
	{
		if (baseline) { clock.now = 100; frame = timing.beginFrame(1, 7, KernelPerformanceSchedulerBoundary()); if (!frame.valid()) return false; }
		if (!boundary(KERNEL_WINDOW_BEGIN, 1, KERNEL_PHASE_COUNT, 7, 7)) return false;
		const JobMetricCounter starts[] = {110,125,150,255,270}, ends[] = {120,145,250,265,290};
		for (unsigned i = 0; i != 5; ++i)
		{
			const auto phase = static_cast<KernelPerformancePhase>(i);
			if (!boundary(KERNEL_WINDOW_PHASE_BEGIN, 1, phase, 7, 7)) return false;
			if (baseline) { clock.now = starts[i]; if (!timing.beginPhase(frame, phase)) return false; }
			if (phase == KERNEL_PHASE_SPATIAL_WORK && !capture(actualAttemptFrame)) return false;
			if (baseline) { clock.now = ends[i]; if (!timing.endPhase(frame, phase)) return false; }
			if (!boundary(KERNEL_WINDOW_PHASE_END, 1, phase, 7, i == 4 ? 8 : 7)) return false;
		}
		if (!boundary(KERNEL_WINDOW_WORLD_END, 1, KERNEL_PHASE_COUNT, 7, 8)) return false;
		if (baseline) { clock.now = 300; if (!timing.endFrame(frame, 8, KernelPerformanceSchedulerBoundary())) return false; }
		return true;
	}
	bool secondIntake(JobMetricCounter actualTimingSample = 2,
		KernelPerformancePhase actualTimingPhase = KERNEL_PHASE_OWNER_INTAKE, bool consumeSourceWindow = true)
	{
		if (baseline)
		{
			clock.now = 400; frame = timing.beginFrame(actualTimingSample, 8, KernelPerformanceSchedulerBoundary());
			if (!frame.valid()) return false;
		}
		if (consumeSourceWindow && (!boundary(KERNEL_WINDOW_BEGIN, 2, KERNEL_PHASE_COUNT, 8, 8) ||
			!boundary(KERNEL_WINDOW_PHASE_BEGIN, 2, KERNEL_PHASE_OWNER_INTAKE, 8, 8))) return false;
		if (baseline)
		{
			for (unsigned i = 0; i <= static_cast<unsigned>(actualTimingPhase); ++i)
			{
				const auto phase = static_cast<KernelPerformancePhase>(i);
				clock.now = 410 + i;
				if (!timing.beginPhase(frame, phase)) return false;
				if (phase != actualTimingPhase && !timing.endPhase(frame, phase)) return false;
			}
		}
		return true;
	}
	bool open(unsigned i, KernelPerformanceInlineBody &body, KernelPerformanceCheckpointProbe &probe)
	{
		clock.now = 415 + 30 * i;
		return reference.beginInlineBody(attempt, test.budget.range(i), timing, body, probe) == KERNEL_INLINE_EXECUTE;
	}
	bool execute(unsigned i)
	{
		KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
		if (baseline ? !open(i, body, probe) : !probe.beginRecord()) return false;
		if (!runRequestBudgetBody(test.budget, i, actual, probe, baseline ? &reference : 0, body, baseline ? &clock : 0)) return false;
		if (test.committedSource) actual.ranges[i].publication = KERNEL_PUBLICATION_PUBLISHED;
		if (baseline) { ++clock.now; if (!reference.finishInlineBody(body, actual.ranges[i])) return false; }
		const auto plan = test.budget.range(i);
		for (JobMetricCounter request = plan.begin; request != plan.end; ++request)
			if (!reference.observeReleasedRequestBudget(attempt, plan, actual.requests[static_cast<std::size_t>(request)])) return false;
		return reference.observeReleasedRange(attempt, plan, actual.ranges[i]);
	}
	bool reap()
	{
		if (test.committedSource)
		{
			const CohortCanonicalOutput output = { &actual.discarded, &actual.source.counts };
			const auto validated = reference.observeValidatedAttempt(attempt, writeCohortOutput, &output);
			if (!validated.valid() || !reference.finishBatch(validated,
				cohortAuthoritativeCommit(actual.discarded, actual.source.authoritative, actual.source.counts)) ||
				!reference.finishAttempt(attempt, test.budget.base.committed(validated))) return false;
		}
		return reference.reapAttempt(attempt, test.budget.base.base.reap);
	}
	bool finish()
	{
		if (baseline) { clock.now = 510; if (!timing.endPhase(frame, KERNEL_PHASE_OWNER_INTAKE)) return false; }
		if (!boundary(KERNEL_WINDOW_PHASE_END, 2, KERNEL_PHASE_OWNER_INTAKE, 8, 8)) return false;
		const JobMetricCounter starts[] = {410,515,540,555,570}, ends[] = {510,535,550,565,590};
		for (unsigned i = 1; i != 5; ++i)
		{
			const auto phase = static_cast<KernelPerformancePhase>(i);
			if (!boundary(KERNEL_WINDOW_PHASE_BEGIN, 2, phase, 8, 8)) return false;
			if (baseline)
			{
				clock.now = starts[i]; if (!timing.beginPhase(frame, phase)) return false;
				clock.now = ends[i]; if (!timing.endPhase(frame, phase)) return false;
			}
			if (!boundary(KERNEL_WINDOW_PHASE_END, 2, phase, 8, i == 4 ? 9 : 8)) return false;
		}
		if (!boundary(KERNEL_WINDOW_WORLD_END, 2, KERNEL_PHASE_COUNT, 8, 9)) return false;
		if (baseline)
		{
			clock.now = 600;
			if (!timing.endFrame(frame, 9, KernelPerformanceSchedulerBoundary()) || !timing.sealAdmissions() ||
				!timing.sealExecutionClosure(KernelPerformanceSchedulerBoundary())) return false;
			timed = timing.freeze();
		}
		if (!reference.sealObservationWindow() || !reference.sealExecutionClosure()) return false;
		actual.source.snapshot = reference.freeze();
		return actual.source.snapshot.trace.complete;
	}
};
bool recordLateBudgetWindows(LateBudgetWindowFixture &test)
{
	LateBudgetWindowRun source(test);
	if (!source.begin(false) || !source.firstWindow() || !source.secondIntake() ||
		!source.execute(0) || !source.execute(1) || !source.execute(2) || !source.reap() || !source.finish()) return false;
	test.recorded = source.actual;
	return true;
}
bool beginLateBudgetConsumer(LateBudgetWindowFixture &test, LateBudgetWindowRun &consumer)
{
	const bool recorded = recordLateBudgetWindows(test);
	check(recorded, "late-window fixture requires complete source events before any consumer body");
	if (!recorded) return false;
	const bool begun = consumer.begin(true);
	check(begun, "late-window consumer prevalidates source identity and both explicit windows");
	return begun;
}

void sourceAbortedBodyUsesCurrentAuthenticatedOwnerWindow()
{
	// Break: original attempt identity is used as its late timing destination,
	// or the current local timing phase alone can mint serial replay authority.
	LateBudgetWindowFixture test; LateBudgetWindowRun consumer(test); const unsigned sourceClocks = clocks;
	if (!beginLateBudgetConsumer(test, consumer)) return;
	check(test.recorded.source.snapshot.trace.recordCount == 43 && test.recorded.source.snapshot.trace.logicalEventCount == 43 &&
		test.recorded.source.snapshot.trace.windowBoundaryCount == 24 && test.recorded.source.snapshot.trace.completedWindowCount == 2 &&
		test.recorded.source.snapshot.trace.controlWindowCount == 0 && clocks == sourceClocks,
		"literal43-record source preserves two full windows while an aborted attempt remains retained");
	check(consumer.firstWindow() && consumer.secondIntake(), "original capture and later owner-intake observations replay before late materialization");
	for (unsigned i = 0; i != 3; ++i)
	{
		const bool executed = consumer.execute(i);
		check(executed, "source-aborted body executes once in the authenticated later live owner window");
		if (!executed) return;
	}
	check(consumer.reap() && consumer.finish(), "all late request/range acknowledgements precede real reap and second window closure");
	const auto &reference = consumer.actual.source.snapshot; const auto &timed = consumer.timed.phaseAccounting;
	check(reference.trace.complete && reference.trace.digest.equals(test.recorded.source.snapshot.trace.digest) &&
		reference.trace.recordCount == 43 && !reference.complete && reference.streamCount == 0,
		"later serial attribution changes no attempt source bytes or canonical success entitlement");
	for (const auto *run : { &test.recorded, &consumer.actual })
		check(run->source.counts.inputWrites == 1 && run->source.counts.bodies == 3 && run->materializations == 3 &&
			run->source.counts.fallbacks == 1 && run->source.counts.commits == 0 && run->source.counts.detached == 0 &&
			run->source.authoritative == 6 && run->grants == 2 && run->refusals == 1 && run->refunds == 1 && run->discarded.sum == 32,
			"modeled late import runs actual helper catch fallback and request outcomes once without a native quota claim");
	check(timed.complete && timed.completedFrameCount == 2 && timed.frameNanoseconds == 400 && timed.unscopedSerialNanoseconds == 80 &&
		timed.completionSerialNanoseconds == 0 && timed.controlWindowCount == 0,
		"still-open live admissions preserve400 measured nanoseconds and exclude the inter-window gap without a completion tail");
	const JobMetricCounter rows[] = {110,40,110,20,40};
	for (unsigned i = 0; i != 5; ++i)
		check(timed.phases[i].totalNanoseconds == rows[i] && timed.phases[i].serialNanoseconds == rows[i] &&
			timed.phases[i].pureNanoseconds == 0 && timed.phases[i].samples == 2,
			"late aborted work remains current-window serial with literal independent phase totals");
	const auto &bytes = test.budget.base.base.sink.bytes;
	const auto sample = artifactField(bytes, 3, 0, 6, 3), phase = artifactField(bytes, 3, 0, 7, 1), owner = artifactField(bytes, 3, 0, 8, 1);
	const unsigned char originalScope[] = {
		3,6,0,0,0,1,0,0,0,0,0,0,0,
		1,7,0,0,0,2,0,0,0,
		1,8,0,0,0,7,0,0,0
	};
	static_assert(sizeof(originalScope) == 31, "original attempt scope is one u64 and two u32 fields");
	check(sample < bytes.size() && bytes.size() - sample >= sizeof(originalScope) && phase == sample + 13 && owner == phase + 9 &&
		memcmp(bytes.data() + sample, originalScope, sizeof(originalScope)) == 0,
		"wire attempt identity stays sample1 spatial actual7 rather than later sample2 intake actual8");
}

void lateAbortedBodyRequiresMatchingConsumedWindowAndActualTiming()
{
	for (unsigned mutation = 0; mutation != 3; ++mutation)
	{
		LateBudgetWindowFixture test; LateBudgetWindowRun consumer(test);
		if (!beginLateBudgetConsumer(test, consumer)) continue;
		check(consumer.firstWindow() && consumer.secondIntake(mutation == 0 ? 3 : 2,
			mutation == 1 ? KERNEL_PHASE_SPATIAL_WORK : KERNEL_PHASE_OWNER_INTAKE, mutation != 2),
			"late destination negative prepares its separate source and timing owner observations");
		check(!consumer.execute(0) && !consumer.reference.freeze().trace.complete && !consumer.timing.freeze().phaseAccounting.complete &&
			consumer.actual.source.counts.bodies == 0 && consumer.actual.materializations == 0 && consumer.actual.source.counts.commits == 0,
			"different timing sample phase or unconsumed source window cannot authorize late body execution");
	}
}

void lateAbortedWindowCannotRelabelAttemptOrTurnPureBodySerial()
{
	{
		LateBudgetWindowFixture test; LateBudgetWindowRun consumer(test);
		if (!beginLateBudgetConsumer(test, consumer)) return;
		check(!consumer.firstWindow(8) && !consumer.reference.freeze().trace.complete &&
			!consumer.timing.freeze().phaseAccounting.complete && consumer.actual.source.counts.bodies == 0,
			"future owner frame cannot replace the original captured attempt identity to make late timing fit");
	}
	{
		LateBudgetWindowFixture test; test.committedSource = true; test.budget.refundFirst = test.budget.refuseSecond = false;
		LateBudgetWindowRun consumer(test);
		if (!beginLateBudgetConsumer(test, consumer)) return;
		check(test.recorded.source.snapshot.complete && test.recorded.source.snapshot.trace.complete &&
			test.recorded.source.counts.commits == 1 && consumer.firstWindow() && consumer.secondIntake(),
			"pure relocation negative starts from independently validated committed source output");
		check(!consumer.execute(0) && !consumer.reference.freeze().trace.complete && !consumer.timing.freeze().phaseAccounting.complete &&
			consumer.actual.source.counts.bodies == 0 && consumer.actual.source.counts.commits == 0,
			"only authenticated aborted serial work may use the current window; committed pure work keeps original attribution");
	}
}

void lateWindowDoesNotResetBodyOrSettlementFailure()
{
	for (unsigned mutation = 0; mutation != 2; ++mutation)
	{
		LateBudgetWindowFixture test; LateBudgetWindowRun consumer(test);
		if (!beginLateBudgetConsumer(test, consumer)) continue;
		check(consumer.firstWindow() && consumer.secondIntake(), "late once-only fixture enters the actual later source owner window");
		if (mutation == 0)
		{
			const bool first = consumer.execute(0); check(first, "one late body must finish before testing duplicate authority"); if (!first) continue;
			KernelPerformanceInlineBody repeated; KernelPerformanceCheckpointProbe probe;
			check(consumer.reference.beginInlineBody(consumer.attempt, test.budget.range(0), consumer.timing, repeated, probe) == KERNEL_INLINE_INVALID &&
				consumer.actual.source.counts.bodies == 1,
				"current owner window cannot revive the source range after its one body and release");
		}
		else
		{
			KernelPerformanceInlineBody body; KernelPerformanceCheckpointProbe probe;
			const bool opened = consumer.open(0, body, probe); check(opened, "late settlement negative opens its once-only authenticated body"); if (!opened) continue;
			const auto extent = consumer.reference.beginInlineOwnerSerial(body, KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION);
			bool granted = false;
			check(extent.valid() && consumer.reference.replayRequestBudgetGrant(extent, 0, 1, 1, 96, granted) && granted,
				"late materialization authenticates its real source grant before missing-settlement negative");
			check(!consumer.reference.endInlineOwnerSerial(extent), "later-window serial attribution cannot close an unsettled grant");
		}
		check(!consumer.boundary(KERNEL_WINDOW_PHASE_END, 2, KERNEL_PHASE_OWNER_INTAKE, 8, 8) &&
			!consumer.reference.freeze().trace.complete && !consumer.timing.freeze().phaseAccounting.complete && consumer.actual.source.counts.commits == 0,
			"a phase transition cannot clear late once-only or pending-settlement failure");
	}
}

void lateLiveWindowCannotBeReplacedByPostSealCompletion()
{
	LateBudgetWindowFixture test; LateBudgetWindowRun consumer(test);
	if (!beginLateBudgetConsumer(test, consumer)) return;
	check(consumer.firstWindow() && consumer.secondIntake(), "completion-tail negative keeps actual second-window admissions open");
	check(!consumer.timing.beginCompletionSerial().valid() && !consumer.execute(0) &&
		!consumer.reference.freeze().trace.complete && !consumer.timing.freeze().phaseAccounting.complete && consumer.actual.source.counts.bodies == 0,
		"completionSerial cannot replace a still-live owner window or silently drop retained first-window work");
}


// Core group6 structural and ordinary-wire tests. Positive native bypass
// authority belongs exclusively to the compiled title tests; this file never
// defines PartitionCollisionBypassObserver or constructs a positive proof.
//
// Fixture bytes below are authored from the fixed wire table, not generated
// by KernelPerformanceCanonicalWriter or the parser under test.
void group6Number(std::vector<unsigned char> &bytes, unsigned type, unsigned tag, JobMetricCounter value)
{
	bytes.push_back(static_cast<unsigned char>(type));
	for (unsigned i = 0; i != 4; ++i) bytes.push_back(static_cast<unsigned char>(tag >> (8 * i)));
	const unsigned width = type == 3 ? 8 : type == 5 ? 1 : 4;
	for (unsigned i = 0; i != width; ++i) bytes.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void group6Digest(std::vector<unsigned char> &bytes, unsigned tag, const KernelPerformanceDigest &digest)
{
	group6Number(bytes, 6, tag, 4);
	for (unsigned word = 0; word != 4; ++word)
	{
		JobMetricCounter value = 0;
		for (unsigned i = 0; i != 8; ++i) value |= static_cast<JobMetricCounter>(digest.bytes[8 * word + i]) << (8 * i);
		group6Number(bytes, 3, tag + word + 1, value);
	}
}
void group6Record(std::vector<unsigned char> &bytes, unsigned kind, JobMetricCounter ordinal)
{
	group6Number(bytes, 1, 1, kind); group6Number(bytes, 3, 2, ordinal);
}
void group6Header(std::vector<unsigned char> &bytes, const SourceAbortFixture &fixture)
{
	const unsigned char domain[] = {
		0x52,0x54,0x53,0x2d,0x4b,0x45,0x52,0x4e,0x45,0x4c,
		0x2d,0x46,0x49,0x45,0x4c,0x44,0x53,0x2d,0x76,0x31,1,0x50,0,0
	};
	bytes.insert(bytes.end(), domain, domain + sizeof(domain));
	group6Record(bytes, 1, 1); group6Number(bytes, 1, 3, 1);
	const JobMetricCounter values[] = {15,340,1048576,100000,100000,10000,500000};
	for (unsigned i = 0; i != 7; ++i) group6Number(bytes, 3, 4 + i, values[i]);
	group6Number(bytes, 6, 11, 4);
	group6Digest(bytes, 13, fixture.options.trace.binding.nativeRunIdentity);
	group6Digest(bytes, 13, fixture.options.trace.binding.executable);
	group6Digest(bytes, 13, fixture.options.trace.binding.fixture);
	group6Digest(bytes, 13, fixture.options.trace.binding.sourcePolicy);
}
void group6Seals(std::vector<unsigned char> &bytes, unsigned first)
{
	for (unsigned i = 0; i != 2; ++i)
	{
		group6Record(bytes, 2, first + i);
		group6Number(bytes, 1, 3, i + 1); group6Number(bytes, 3, 4, 0);
	}
}
void group6Footer(std::vector<unsigned char> &bytes, unsigned stored, JobMetricCounter logical,
	JobMetricCounter attempts, JobMetricCounter operations, JobMetricCounter spans,
	JobMetricCounter highWater = 1)
{
	group6Record(bytes, 11, stored);
	// Tags3..29: all values come from this fixture's literal event table.
	const JobMetricCounter values[] = {
		stored,logical,attempts,0,attempts,0,attempts,0,highWater,spans,
		spans == 0 ? 0 : attempts,1,1,attempts,operations,0,0,0,0,0,0,0,0,0,0,0,0
	};
	static_assert(sizeof(values) / sizeof(values[0]) == 27, "one strict29-field footer");
	for (unsigned i = 0; i != 27; ++i) group6Number(bytes, i == 11 || i == 12 ? 5 : 3, i + 3, values[i]);
}
void group6Replace(std::vector<unsigned char> &bytes, unsigned kind, unsigned occurrence,
	unsigned tag, unsigned type, JobMetricCounter value)
{
	const std::size_t at = artifactField(bytes, kind, occurrence, tag, type);
	check(at < bytes.size(), "group6 mutation names an existing literal wire field");
	if (at == bytes.size()) return;
	const unsigned width = type == 3 ? 8 : type == 5 ? 1 : 4;
	for (unsigned i = 0; i != width; ++i) bytes[at + 5 + i] = static_cast<unsigned char>(value >> (8 * i));
}
KernelPerformanceReferenceRunOptions group6ConsumeOptions(const SourceAbortFixture &fixture,
	const std::vector<unsigned char> &bytes, CohortRead &read)
{
	auto options = fixture.options;
	options.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	options.trace.mode = KERNEL_TRACE_CONSUME;
	options.trace.append = 0; options.trace.readAt = CohortRead::read; options.trace.context = &read;
	options.trace.sourceByteCount = bytes.size();
	options.trace.sourceTraceDigest = independentArtifactHash(bytes);
	options.trace.sourceReceiptDigest = fixture.options.trace.binding.nativeRunIdentity;
	options.trace.sourceReceiptDigest.bytes[0] = 0x80;
	return options;
}
bool group6Prevalidates(const SourceAbortFixture &fixture, const std::vector<unsigned char> &bytes)
{
	CohortRead read = {&bytes};
	KernelPerformanceReferenceLedger ledger;
	return ledger.beginRun(group6ConsumeOptions(fixture, bytes, read));
}
static const unsigned char kGroup6OwnerOnlyEnvelope[] = {
    0x01, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x50, 0x00, 0x00, 0x03,
    0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x07,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x0A, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0B, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x0D, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0E, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0F, 0x00, 0x00, 0x00, 0x09,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x10, 0x00, 0x00, 0x00, 0xB0, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x06, 0x11, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x12, 0x00,
    0x00, 0x00, 0x46, 0xB7, 0xDC, 0xD0, 0xBB, 0xD6, 0x2D, 0xA3, 0x03, 0x13, 0x00, 0x00, 0x00, 0x73,
    0x86, 0x8F, 0x84, 0xD2, 0x95, 0xC1, 0x82, 0x03, 0x14, 0x00, 0x00, 0x00, 0xEC, 0x43, 0xA8, 0x36,
    0xC8, 0x6C, 0xC1, 0x7E, 0x03, 0x15, 0x00, 0x00, 0x00, 0xC9, 0x8A, 0x32, 0x69, 0x36, 0xC2, 0xC6,
    0x34,
};
std::vector<unsigned char> group6SpanSource(const SourceAbortFixture &fixture, bool zero = false)
{
	std::vector<unsigned char> bytes;
	group6Header(bytes, fixture);
	bytes.insert(bytes.end(), kGroup6OwnerOnlyEnvelope, kGroup6OwnerOnlyEnvelope + sizeof(kGroup6OwnerOnlyEnvelope));
	if (zero)
	{
		const unsigned char digest[] = {
			0x78,0x1b,0x18,0x90,0x78,0xfc,0x3b,0x39,0x3a,0xf9,0x6c,0x58,0xbd,0xc3,0xe3,0x5d,
			0x19,0x76,0x30,0x7a,0x0a,0x9e,0x77,0xe9,0x22,0x32,0xc0,0x4a,0x47,0x44,0xc8,0x38
		};
		group6Replace(bytes, 12, 0, 16, 3, 899);
		for (unsigned word = 0; word != 4; ++word)
		{
			JobMetricCounter value = 0;
			for (unsigned i = 0; i != 8; ++i) value |= static_cast<JobMetricCounter>(digest[8 * word + i]) << (8 * i);
			group6Replace(bytes, 12, 0, word + 18, 3, value);
		}
	}
	group6Seals(bytes, 3); group6Footer(bytes, 5, 13, 1, zero ? 0 : 1, 1);
	return bytes;
}
void group6Begin(std::vector<unsigned char> &bytes, unsigned record, unsigned serial, unsigned ordinal)
{
	group6Record(bytes, 3, record);
	group6Number(bytes, 3, 3, serial); group6Number(bytes, 1, 4, 2); group6Number(bytes, 1, 5, 0);
	group6Number(bytes, 3, 6, 1); group6Number(bytes, 1, 7, 2); group6Number(bytes, 1, 8, 1);
	group6Number(bytes, 3, 9, ordinal);
}
void group6Decision(std::vector<unsigned char> &bytes, unsigned record, unsigned serial,
	unsigned ordinal, unsigned site, unsigned schema, unsigned reason, bool eligible,
	const KernelPerformanceDigest &facts, unsigned workers = 2)
{
	group6Record(bytes, 4, record); group6Number(bytes, 3, 3, serial); group6Number(bytes, 3, 4, ordinal);
	group6Number(bytes, 1, 5, site); group6Number(bytes, 1, 6, schema); group6Number(bytes, 1, 7, reason);
	group6Number(bytes, 5, 8, eligible); group6Number(bytes, 1, 9, 0); group6Number(bytes, 1, 10, workers);
	group6Number(bytes, 1, 11, 0);
	for (unsigned tag = 12; tag != 15; ++tag) group6Number(bytes, 3, tag, 0);
	group6Digest(bytes, 15, facts);
}
void group6Capture(std::vector<unsigned char> &bytes, unsigned record, unsigned schema,
	unsigned operations, const KernelPerformanceDigest &input)
{
	group6Record(bytes, 5, record); group6Number(bytes, 3, 3, 1); group6Number(bytes, 1, 4, schema);
	group6Number(bytes, 3, 5, operations); group6Digest(bytes, 6, input);
}
void group6Finish(std::vector<unsigned char> &bytes, unsigned record, unsigned serial, unsigned schema, unsigned reason)
{
	group6Record(bytes, 9, record); group6Number(bytes, 3, 3, serial);
	group6Number(bytes, 1, 4, 0); group6Number(bytes, 1, 5, schema); group6Number(bytes, 1, 6, reason);
	group6Number(bytes, 5, 7, 1); group6Number(bytes, 5, 8, 1); group6Number(bytes, 5, 9, 0);
}
void group6Reap(std::vector<unsigned char> &bytes, unsigned record, unsigned serial)
{
	group6Record(bytes, 10, record); group6Number(bytes, 3, 3, serial);
	group6Number(bytes, 1, 4, 1); group6Number(bytes, 1, 5, 1); group6Number(bytes, 1, 6, 0);
	for (unsigned tag = 7; tag != 10; ++tag) group6Number(bytes, 3, tag, 0);
}
const unsigned char kGroup6EmptyFullContact[] = {
	0x52,0x54,0x53,0x2d,0x4b,0x45,0x52,0x4e,0x45,0x4c,
	0x2d,0x46,0x49,0x45,0x4c,0x44,0x53,0x2d,0x76,0x31,6,0x51,0,0,
	1,150,0,0,0,9,0,0,0,
	3,151,0,0,0,9,0,0,0,0,0,0,0,
	5,152,0,0,0,1,
	1,170,0,0,0,2,0,0,0,
	3,171,0,0,0,0,0,0,0,0,0,0,0,
	3,172,0,0,0,0,0,0,0,0,0,0,0,
	5,173,0,0,0,1
};
static_assert(sizeof(kGroup6EmptyFullContact) == 93, "full5106 hashes only its contact subsection");
std::vector<unsigned char> group6FullFallbackSource(const SourceAbortFixture &fixture, unsigned operations,
	bool diagnosticIdentityFailure = false)
{
	std::vector<unsigned char> bytes;
	group6Header(bytes, fixture); group6Begin(bytes, 2, 1, 1);
	for (unsigned site = 1; site != 6; ++site)
	{
		const unsigned reason = site == 3 ? 2 : site == 4 && diagnosticIdentityFailure ? 4 : 1;
		group6Decision(bytes, site + 2, 1, site - 1, site, site == 5 ? 0x5002 : 1,
			reason, site != 5 && !(site == 4 && diagnosticIdentityFailure), fixture.options.trace.binding.fixture);
	}
	// Input/facts digests are opaque to structural preflight. These literals
	// test the restricted grammar, not native semantic/capture equivalence.
	group6Capture(bytes, 8, 0x5100, operations, fixture.options.trace.binding.fixture);
	group6Finish(bytes, 9, 1, 0x5002, 1);
	group6Number(bytes, 3, 19, 0); group6Number(bytes, 3, 20, 0);
	const std::vector<unsigned char> contacts(kGroup6EmptyFullContact,
		kGroup6EmptyFullContact + sizeof(kGroup6EmptyFullContact));
	group6Digest(bytes, 21, independentArtifactHash(contacts));
	group6Reap(bytes, 10, 1); group6Seals(bytes, 11); group6Footer(bytes, 13, 13, 1, operations, 0);
	return bytes;
}
void group6LiteralSpanPreflightDoesNotMintNativeCompletion()
{
	// Break: kind12 rejected wholesale, or structural acceptance is confused
	// with native execution and copied into actual consumer counters.
	for (unsigned zero = 0; zero != 2; ++zero)
	{
		SourceAbortFixture fixture;
		const auto bytes = group6SpanSource(fixture, zero != 0);
		check(sizeof(kGroup6OwnerOnlyEnvelope) == 241 && bytes.size() == 1087,
			"group6 literal source has399 header241 span88 seals359 footer");
		CohortRead read = {&bytes}; KernelPerformanceReferenceLedger ledger;
		const unsigned clocksBefore = clocks;
		const bool accepted = ledger.beginRun(group6ConsumeOptions(fixture, bytes, read));
		check(accepted, "literal owner-only or genuine-zero envelope passes structural preflight without a proof factory");
		const auto incomplete = ledger.freeze();
		check(!incomplete.complete && !incomplete.trace.complete && incomplete.streamCount == 0 &&
			incomplete.trace.attemptCount == 0 && incomplete.trace.capturedOperationCount == 0 && clocks == clocksBefore,
			"preflight cannot copy hidden source totals or manufacture a completed actual native bypass");
	}
}
void group6FixedEnvelopeMutationsFailBeforeNativeExecution()
{
	// Break: malformed compressed arithmetic/context/footer is trusted merely
	// because the caller supplied a matching whole-artifact hash.
	SourceAbortFixture fixture; const auto original = group6SpanSource(fixture);
	struct Mutation { unsigned kind, occurrence, tag, type; JobMetricCounter value; };
	const Mutation mutations[] = {
		{12,0,3,1,0x5001},{12,0,4,3,0},{12,0,6,1,5},{12,0,7,1,1},{12,0,8,1,1},
		{12,0,9,1,0},{12,0,9,1,3},{12,0,10,3,1},{12,0,11,3,9},
		{12,0,12,3,2},{12,0,13,3,0},{12,0,14,3,0},{12,0,14,3,1025},
		{12,0,15,3,8},{12,0,15,3,10},{12,0,16,3,898},{12,0,17,6,3},
		{12,0,2,3,3},{12,0,11,3,~JobMetricCounter(0)},{12,0,15,3,~JobMetricCounter(0)},
		{11,0,3,3,6},{11,0,4,3,14},{11,0,5,3,2},{11,0,6,3,1},{11,0,7,3,0},
		{11,0,8,3,1},{11,0,9,3,0},{11,0,10,3,1},{11,0,11,3,0},{11,0,12,3,0},
		{11,0,13,3,0},{11,0,14,5,0},{11,0,15,5,0},{11,0,16,3,0},
		{11,0,18,3,1},{11,0,19,3,1},{11,0,20,3,1},{11,0,21,3,1},{11,0,22,3,1},
		{11,0,23,3,1},{11,0,24,3,1},{11,0,25,3,1},{11,0,26,3,1},{11,0,27,3,1},
		{11,0,28,3,1},{11,0,29,3,1}
	};
	for (const auto &mutation : mutations)
	{
		auto bytes = original;
		group6Replace(bytes, mutation.kind, mutation.occurrence, mutation.tag, mutation.type, mutation.value);
		check(!group6Prevalidates(fixture, bytes), "rehashed malformed group6 envelope or derivable footer is rejected before native execution");
	}
	for (unsigned mutation = 0; mutation != 5; ++mutation)
	{
		auto bytes = original;
		if (mutation == 0) bytes.erase(bytes.begin() + 640, bytes.begin() + 684); // missing observation seal
		if (mutation == 1) bytes.insert(bytes.end(), 0); // trailing data
		if (mutation == 2) bytes.pop_back(); // incomplete strict footer
		if (mutation == 3) bytes[399] = 3; // wrong kind type
		if (mutation == 4) bytes.erase(bytes.begin() + 728, bytes.end()); // absent footer
		check(!group6Prevalidates(fixture, bytes), "group6 source cannot omit framing closure or append an unparsed suffix");
	}
}
void group6FullZeroAndGrowthHaveRestrictedCompleteGrammar()
{
	// Break: all zero input is rejected, growth cannot remain full, or the
	// private zero rule leaks to successful/public arbitrary input.
	SourceAbortFixture fixture;
	for (unsigned variant = 0; variant != 3; ++variant)
	{
		const auto bytes = group6FullFallbackSource(fixture, variant == 0 ? 0 : 1, variant == 2);
		check(bytes.size() == 2369, "full captured fallback has thirteen records and exact87-byte restricted finish suffix");
		check(group6Prevalidates(fixture, bytes),
			"complete full5100 accepts genuine zero growth and diagnostic identity-invalid fallback without positive proof authority");
	}
}
void group6FullProfileTransplantsAndMissingTerminalsReject()
{
	SourceAbortFixture fixture;
	const auto original = group6FullFallbackSource(fixture, 0);
	struct Mutation { unsigned kind, occurrence, tag, type; JobMetricCounter value; };
	const Mutation mutations[] = {
		{3,0,4,1,1},{3,0,5,1,1},{5,0,4,1,1},{5,0,4,1,0x5101},
		{5,0,5,3,256},{4,4,6,1,1},{4,4,7,1,2},{4,4,8,5,1},
		{4,0,7,1,2},{4,1,7,1,2},{4,2,7,1,3},{4,3,7,1,2},
		{4,3,7,1,3},{4,2,8,5,0},{4,0,9,1,1},{4,4,9,1,2},
		{4,2,4,3,1},{4,3,5,1,5},{4,0,11,1,1},{4,0,12,3,1},
		{9,0,4,1,1},{9,0,4,1,2},{9,0,5,1,1},{9,0,6,1,2},
		{9,0,7,5,0},{9,0,8,5,0},{9,0,9,5,1},{9,0,19,3,1},{9,0,20,3,1},
		{9,0,21,6,3},{10,0,4,1,2},{10,0,5,1,2},{10,0,6,1,1},
		{11,0,17,3,1}
	};
	for (unsigned i = 0; i != sizeof(mutations) / sizeof(mutations[0]); ++i)
	{
		auto bytes = original; const auto &mutation = mutations[i];
		group6Replace(bytes, mutation.kind, mutation.occurrence, mutation.tag, mutation.type, mutation.value);
		check(!group6Prevalidates(fixture, bytes), "rehashed zero-profile transplant or invalid native terminal cannot initialize consumption");
	}
	const unsigned kinds[] = {4,5,9,10};
	for (unsigned kind : kinds)
	{
		auto bytes = original;
		const auto begin = artifactField(bytes, kind, 0, 1, 1), end = artifactRecordEnd(bytes, begin);
		check(begin < end && end <= bytes.size(), "restricted grammar deletion identifies one actual record");
		if (begin == bytes.size()) continue;
		bytes.erase(bytes.begin() + begin, bytes.begin() + end);
		check(!group6Prevalidates(fixture, bytes), "missing native decision capture finish or synchronous reap is never an implicit success");
	}
	KernelPerformanceReferenceLedger publicLedger;
	check(publicLedger.beginRun(fixture.options), "public-zero negative initializes ordinary recording");
	auto identity = fixture.identity(1); identity.workKind = KERNEL_PERFORMANCE_COLLISION;
	const auto attempt = publicLedger.beginAttempt(identity);
	check(!publicLedger.bindCapturedInput(attempt, 1, 0, writeInput, &fixture.input) &&
		!publicLedger.freeze().trace.complete, "restricted native zero capture never loosens public ordinary zero rejection");
}
void group6SpanBlockBoundaryArithmeticStaysBounded()
{
	// Opaque fixed-envelope preflight only. B owns actual1023/1024/1025 and
	// million-call native span generation; no loop here makes a native proof.
	SourceAbortFixture fixture;
	for (unsigned count : {1023U,1024U,1025U})
	{
		auto bytes = group6SpanSource(fixture);
		bytes.resize(640); // complete header + first span only
		const unsigned firstCount = count > 1024 ? 1024 : count;
		group6Replace(bytes, 12, 0, 11, 3, 1 + 9 * firstCount);
		group6Replace(bytes, 12, 0, 13, 3, firstCount);
		group6Replace(bytes, 12, 0, 14, 3, firstCount);
		group6Replace(bytes, 12, 0, 15, 3, 9 * firstCount);
		group6Replace(bytes, 12, 0, 16, 3, 91 + 853 * firstCount);
		if (count == 1025)
		{
			bytes.insert(bytes.end(), kGroup6OwnerOnlyEnvelope, kGroup6OwnerOnlyEnvelope + sizeof(kGroup6OwnerOnlyEnvelope));
			group6Replace(bytes, 12, 1, 2, 3, 3);
			group6Replace(bytes, 12, 1, 10, 3, 9218); group6Replace(bytes, 12, 1, 11, 3, 9226);
			group6Replace(bytes, 12, 1, 12, 3, 1025); group6Replace(bytes, 12, 1, 13, 3, 1025);
		}
		const unsigned spans = count == 1025 ? 2 : 1;
		group6Seals(bytes, 2 + spans); group6Footer(bytes, 4 + spans, 4 + 9 * count, count, count, spans);
		check(group6Prevalidates(fixture, bytes), "bounded fixed span envelopes accept1023/1024/1025 declared attempts without expansion");
		auto tooMany = bytes; group6Replace(tooMany, 12, 0, 14, 3, 1025);
		check(!group6Prevalidates(fixture, tooMany), "one stored envelope cannot hide more than1024 completed native attempts");
	}
}
void group6UnarmedCollisionKeepsOrdinaryWireAndNonConsumingLookups()
{
	// Break: a generic collision is accidentally compacted, first replay reads
	// an unmaterialized begin, or readSource consumes the pending ordinary event.
	SourceAbortFixture fixture;
	auto identity = fixture.identity(1); identity.workKind = KERNEL_PERFORMANCE_COLLISION; identity.ownerFrame = 1;
	auto decision = fixture.rejected; decision.sourceConfiguredWorkers = 2;
	auto reap = fixture.reap; reap.dynamicFactsKnownMask = 0;
	std::vector<unsigned char> inputBytes = {
		0x52,0x54,0x53,0x2d,0x4b,0x45,0x52,0x4e,0x45,0x4c,0x2d,0x46,0x49,0x45,0x4c,0x44,0x53,0x2d,0x76,0x31,1,0,0,0,
		6,1,0,0,0,2,0,0,0,1,2,0,0,0,3,0,0,0,1,3,0,0,0,4,0,0,0
	};
	std::vector<unsigned char> expected; group6Header(expected, fixture); group6Begin(expected, 2, 1, 1);
	group6Decision(expected, 3, 1, 0, 11, 1, 1, false, decision.deterministicFacts);
	group6Capture(expected, 4, 1, 2, independentArtifactHash(inputBytes));
	group6Finish(expected, 5, 1, 1, 1); group6Reap(expected, 6, 1); group6Seals(expected, 7);
	group6Footer(expected, 9, 9, 1, 2, 0);
	KernelPerformanceReferenceLedger source;
	check(source.beginRun(fixture.options), "generic collision source starts under historical limits and bindings");
	const auto attempt = source.beginAttempt(identity);
	const bool recorded = attempt.valid() && source.observeDecision(attempt, decision) &&
		source.bindCapturedInput(attempt, 1, 2, writeInput, &fixture.input) &&
		source.finishAttempt(attempt, fixture.notAdmitted) && source.reapAttempt(attempt, reap) &&
		source.sealObservationWindow() && source.sealExecutionClosure();
	const auto snapshot = source.freeze();
	check(recorded && snapshot.trace.complete && expected.size() == 1450 && fixture.sink.bytes == expected &&
		snapshot.trace.coalescedSpanCount == 0 && snapshot.trace.logicalEventCount == 9,
		"unarmed COLLISION0 materializes the exact1450-byte ordinary trace without coalescing or reordering");
	if (!recorded || !snapshot.trace.complete) return;
	CohortRead read = {&expected}; KernelPerformanceReferenceLedger consumer;
	check(consumer.beginRun(group6ConsumeOptions(fixture, expected, read)), "ordinary collision literal source prevalidates");
	const auto replay = consumer.beginAttempt(identity);
	KernelPerformanceAttemptFinish finish = {}; KernelPerformanceDispatchPlan absent = {};
	check(replay.valid() && consumer.readSourceFinish(replay, finish) &&
		finish.disposition == KERNEL_PERFORMANCE_NOT_ADMITTED && !consumer.readSourceDispatch(replay, 1, absent),
		"lookups before the first collision mutation retain current event and do not invent a dispatch");
	KernelPerformanceAttemptDecision inherited = {};
	const bool matched = consumer.replayDecision(replay, 11, false, decision.deterministicFacts, inherited) &&
		consumer.bindCapturedInput(replay, 1, 2, writeInput, &fixture.input) && consumer.finishAttempt(replay, fixture.notAdmitted) &&
		consumer.reapAttempt(replay, reap) && consumer.sealObservationWindow() && consumer.sealExecutionClosure();
	check(matched && consumer.freeze().trace.complete, "first ordinary replay decision follows pending begin exactly once after non-consuming lookups");
}
void group6UnarmedLimitsAndAbandonedStateFailClosed()
{
	for (unsigned limit = 0; limit != 3; ++limit)
	{
		SourceAbortFixture fixture;
		if (limit == 0) fixture.options.trace.limits.maximumLogicalEvents = 1;
		if (limit == 1) fixture.options.trace.limits.maximumRecords = 1;
		if (limit == 2) fixture.options.trace.residentAttemptCapacity = 1;
		KernelPerformanceReferenceLedger ledger;
		check(ledger.beginRun(fixture.options), "pending-prefix limit test starts a valid bounded run");
		auto identity = fixture.identity(1); identity.workKind = KERNEL_PERFORMANCE_COLLISION;
		const auto first = ledger.beginAttempt(identity);
		if (limit == 2)
		{
			identity.attemptOrdinal = 2;
			check(first.valid() && !ledger.beginAttempt(identity).valid(), "unarmed begin occupies its real resident slot before another attempt");
		}
		else check(!first.valid(), "deferred representation cannot evade logical or physical record exhaustion");
		check(!ledger.freeze().trace.complete, "pending-prefix capacity failure cannot seal complete evidence");
	}
	SourceAbortFixture fixture; KernelPerformanceReferenceLedger ledger;
	check(ledger.beginRun(fixture.options), "abandoned unarmed begin starts a real run");
	auto identity = fixture.identity(1); identity.workKind = KERNEL_PERFORMANCE_COLLISION;
	check(ledger.beginAttempt(identity).valid() && ledger.sealObservationWindow(), "observation seal preserves a still-resident ordinary attempt");
	const auto incomplete = ledger.freeze();
	check(!incomplete.trace.complete && incomplete.trace.residentAttemptCount == 1 &&
		incomplete.trace.notAdmittedAttemptCount == 0 && incomplete.trace.reapCount == 0,
		"freeze never invents fallback completion or reap for an abandoned unarmed prefix");
}
void group6DefaultAndForeignAuthorityCannotMintBypass()
{
	// No positive proof constructor or native friend appears in this core test.
	const KernelPerformanceDeterministicBypassProof proof;
	for (unsigned operation = 0; operation != 4; ++operation)
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger ledger;
		check(ledger.beginRun(fixture.options), "default bypass rejection starts an actual source ledger");
		auto identity = fixture.identity(1); identity.workKind = KERNEL_PERFORMANCE_COLLISION;
		const auto attempt = ledger.beginAttempt(identity);
		bool rejected = false;
		if (operation == 0) rejected = !ledger.beginDeterministicBypass(attempt, proof).valid();
		if (operation == 1) rejected = !ledger.observeBypassFallbackContact(KernelPerformanceDeterministicBypass(), 10, 20, true);
		if (operation == 2) rejected = !ledger.finishDeterministicBypass(KernelPerformanceDeterministicBypass(), 1, 1);
		if (operation == 3)
		{
			std::thread foreign([&] { rejected = !ledger.beginDeterministicBypass(attempt, proof).valid(); });
			foreign.join();
		}
		const auto incomplete = ledger.freeze();
		check(rejected && !incomplete.trace.complete && incomplete.trace.coalescedSpanCount == 0 &&
			incomplete.trace.coalescedAttemptCount == 0 && incomplete.trace.notAdmittedAttemptCount == 0 && incomplete.streamCount == 0,
			"default or foreign bypass calls cannot close native work or create successful evidence");
	}
	KernelPerformanceReferenceLedger disabled;
	const unsigned clocksBefore = clocks, writesBefore = writes, computesBefore = computes;
	check(!disabled.beginDeterministicBypass(KernelPerformanceAttempt(), proof).valid() &&
		!disabled.observeBypassFallbackContact(KernelPerformanceDeterministicBypass(), 10, 20, false) &&
		!disabled.finishDeterministicBypass(KernelPerformanceDeterministicBypass(), 0, 0) &&
		clocks == clocksBefore && writes == writesBefore && computes == computesBefore,
		"disabled bypass surface is inert and cannot run a callback or clock");
}

void group6OrdinarySecondBeginPreservesBothOriginalPrefixes()
{
	SourceAbortFixture fixture;
	auto firstIdentity = fixture.identity(1); firstIdentity.workKind = KERNEL_PERFORMANCE_COLLISION; firstIdentity.ownerFrame = 1;
	auto secondIdentity = firstIdentity; secondIdentity.attemptOrdinal = 2;
	auto decision = fixture.rejected; decision.sourceConfiguredWorkers = 2;
	auto reap = fixture.reap; reap.dynamicFactsKnownMask = 0;
	std::vector<unsigned char> expected; group6Header(expected, fixture);
	group6Begin(expected, 2, 1, 1); group6Begin(expected, 3, 2, 2);
	group6Decision(expected, 4, 1, 0, 11, 1, 1, false, decision.deterministicFacts);
	group6Decision(expected, 5, 2, 0, 11, 1, 1, false, decision.deterministicFacts);
	group6Finish(expected, 6, 1, 1, 1); group6Reap(expected, 7, 1);
	group6Finish(expected, 8, 2, 1, 1); group6Reap(expected, 9, 2);
	group6Seals(expected, 10); group6Footer(expected, 12, 12, 2, 0, 0, 2);
	group6Replace(expected, 11, 0, 16, 3, 0);
	KernelPerformanceReferenceLedger source;
	check(source.beginRun(fixture.options), "two generic collision attempts share one bounded source ledger");
	const auto a = source.beginAttempt(firstIdentity), b = source.beginAttempt(secondIdentity);
	const bool recorded = a.valid() && b.valid() && source.observeDecision(a, decision) && source.observeDecision(b, decision) &&
		source.finishAttempt(a, fixture.notAdmitted) && source.reapAttempt(a, reap) &&
		source.finishAttempt(b, fixture.notAdmitted) && source.reapAttempt(b, reap) &&
		source.sealObservationWindow() && source.sealExecutionClosure();
	const auto snapshot = source.freeze();
	check(recorded && snapshot.trace.complete && expected.size() == 1818 && fixture.sink.bytes == expected &&
		snapshot.trace.residentAttemptHighWater == 2 && snapshot.trace.coalescedAttemptCount == 0,
		"second begin materializes the first prefix before itself and keeps ordinary1818-byte order and true occupancy");
	check(group6Prevalidates(fixture, expected), "both ordinary full collision prefixes remain valid structural source records");
}

void group6UnarmedInputFailuresRemainOnceOnlyAndSticky()
{
	for (unsigned kind = 0; kind != 3; ++kind)
	{
		SourceAbortFixture fixture; KernelPerformanceReferenceLedger source;
		check(source.beginRun(fixture.options), "pending collision callback negative initializes its source");
		auto identity = fixture.identity(1); identity.workKind = KERNEL_PERFORMANCE_COLLISION;
		const auto attempt = source.beginAttempt(identity); identity.attemptOrdinal = 2;
		SourceCallbackFault fault = {&source, identity, kind};
		check(attempt.valid() && !source.bindCapturedInput(attempt, 1, 2, sourceFaultingInput, &fault) && fault.calls == 1 &&
			!source.sealObservationWindow() && !source.freeze().trace.complete,
			"ordinary first collision capture cannot hide a false throwing or reentrant callback behind deferred representation");
	}
}

void group6SpanInterleaveDoesNotReapHeldFullAttempt()
{
	// Literal structural source, not a worker/cancellation claim. B's real
	// native retained-abort fixture supplies execution and lifetime evidence.
	SourceAbortFixture fixture; std::vector<unsigned char> bytes;
	group6Header(bytes, fixture); group6Begin(bytes, 2, 1, 1);
	group6Capture(bytes, 3, 1, 1, fixture.options.trace.binding.fixture);
	group6Decision(bytes, 4, 1, 0, 1, 1, 1, true, fixture.options.trace.binding.fixture);
	group6Replace(bytes, 4, 0, 9, 1, 2); // actual source accepted
	group6Record(bytes, 6, 5); group6Number(bytes, 3, 3, 1); group6Number(bytes, 3, 4, 1);
	for (unsigned tag = 5; tag <= 7; ++tag) group6Number(bytes, 1, tag, 1);
	for (unsigned tag = 8; tag <= 10; ++tag) group6Number(bytes, 3, tag, 1);
	group6Record(bytes, 7, 6); group6Number(bytes, 3, 3, 1); group6Number(bytes, 3, 4, 1);
	group6Number(bytes, 1, 5, 0); group6Number(bytes, 1, 6, 1);
	group6Number(bytes, 3, 7, 0); group6Number(bytes, 3, 8, 1); group6Number(bytes, 3, 9, 1);
	group6Finish(bytes, 7, 1, 1, 3); group6Replace(bytes, 9, 0, 4, 1, 1); // aborted, range still live
	bytes.insert(bytes.end(), kGroup6OwnerOnlyEnvelope, kGroup6OwnerOnlyEnvelope + sizeof(kGroup6OwnerOnlyEnvelope));
	group6Replace(bytes, 12, 0, 2, 3, 8); group6Replace(bytes, 12, 0, 10, 3, 8); group6Replace(bytes, 12, 0, 11, 3, 16);
	group6Replace(bytes, 12, 0, 12, 3, 2); group6Replace(bytes, 12, 0, 13, 3, 2);
	group6Record(bytes, 8, 9); group6Number(bytes, 3, 3, 1); group6Number(bytes, 3, 4, 1);
	group6Number(bytes, 1, 5, 0); group6Number(bytes, 1, 6, 1);
	group6Number(bytes, 3, 7, 0); group6Number(bytes, 3, 8, 1); group6Number(bytes, 3, 9, 1);
	for (unsigned tag = 10; tag <= 22; ++tag)
	{
		const unsigned type = tag == 10 ? 5 : tag == 11 || tag == 15 || tag == 18 || tag == 21 || tag == 22 ? 1 : 3;
		group6Number(bytes, type, tag, 0); // authentic structural never-entered/N/A release
	}
	group6Reap(bytes, 10, 1); group6Seals(bytes, 11); group6Footer(bytes, 13, 21, 2, 2, 1, 2);
	group6Replace(bytes, 11, 0, 6, 3, 1); group6Replace(bytes, 11, 0, 7, 3, 1); group6Replace(bytes, 11, 0, 8, 3, 1);
	group6Replace(bytes, 11, 0, 13, 3, 1);
	for (unsigned tag : {18U,19U,20U,22U}) group6Replace(bytes, 11, 0, tag, 3, 1);
	check(group6Prevalidates(fixture, bytes), "fixed span arithmetic preserves a separately held full attempt and later real-shaped release/reap");
	auto understated = bytes; group6Replace(understated, 11, 0, 11, 3, 1);
	check(!group6Prevalidates(fixture, understated), "span's synchronous attempt still adds one to held full resident high-water");
	auto premature = bytes;
	const auto start = artifactField(premature, 8, 0, 1, 1), end = artifactRecordEnd(premature, start);
	premature.erase(premature.begin() + start, premature.begin() + end);
	check(!group6Prevalidates(fixture, premature), "coalesced reap cannot acknowledge or remove an unrelated full range release");
}

}
int main()
{
	modeQuery(); canonicalFields(); disabledAndMatchingReference(); failureAndCommitBoundaries(); orderingAndOwnership();
	latchedRunModeSurvivesFailureAndFreeze();
	rejectedConfigurationCannotReplaceRunMode();
	rejectedConfigurationPreservesCompleteFrozenReceipt();
	bufferedCanonicalLiteralBytes();
	bufferedCanonicalEveryWideFieldSplit();
	bufferedCanonicalExactCapacityBoundaries();
	canonicalExplicitFlushAndSuccessfulReset();
	canonicalTransportFailuresStayPoisoned();
	canonicalTransportReentryIsRejected();
	canonicalBufferedHashWithoutTransport();
	checkpointRecordPreservesSourcePredicateAndPrefix();
	checkpointReplayUsesSourceCutNotActualCancellation();
	checkpointReplayRejectsChangedCutAndTerminal();
	checkpointNeverEnteredAndMalformedSourceCannotExecute();
	checkpointLocalLifecycleCannotEraseFailure();
	sourceAttemptNoCaptureRejectionClosesWithoutCanonicalStream();
	sourceAbortedAttemptRetainsIdentityUntilReleasedReap();
	sourceAbortedRetentionRejectsEarlyReapAndClosure();
	sourceAbortedCapacityAndPreAdmissionPlanOrder();
	sourceConsumerCohortExecutesEachBodyOnceWithExactPartition();
	sourceConsumerCohortExecutesEachBodyOnceWithExactPartition(true);
	sourceConsumerCohortExecutesEachBodyOnceWithExactPartition(false, true);
	validatedDiscardRoundTripsWithoutPublishingAuthority();
	sourceConsumerRejectsBrokenBindingsBeforeAnyBody();
	sourceSuccessWireHasOneLiteralLinkAndFinalFooter();
	sourceConsumerParsesGrammarEvenWhenArtifactHashMatches();
	sourceConsumerRejectsEnteredRangeWithNotApplicablePublication();
	consumerAuthenticatedBodyRejectsOrdinaryPipelineIntervals();
	consumerRejectsChangedActualCaptureDecisionAndPartition();
	consumerBodyAndOwnerSerialAuthorityCannotBeForgedOrReused();
	consumerActualProgressAndSerialScopeFailuresStaySticky();
	consumerSuccessfulLinkUsesOnlyActualOutputAndSingleOwnerCommit();
	consumerActualOutputCallbackFailuresCannotPublishSuccess();
	consumerLateSourceEventsCannotExecuteOrReapEarly();
	noCaptureTraceRoundTripRemainsIndependentFromSuccessfulStreams();
	coincidentValidatedBatchCannotCrossLedgerInstances();
	inlineSurfacesAreInertWithoutAuthenticatedConsumption();
	sourceLifecycleCallbackAndCounterFailuresRemainSticky();
	sourceLifecycleRejectsUnboundAndMalformedReleasedRanges();
	sourceSequentialDispatchesReuseCapacityWithoutResurrectingOldPlans();
	sourceControlWindowsRetainRetargetedBodiesAndExactAccounting(false);
	sourceControlWindowsRetainRetargetedBodiesAndExactAccounting(true);
	sourceWindowWireAndFooterHaveOneLiteralGrammar();
	sourceWindowGrammarAndNativeIdentityCannotBeSubstituted();
	sourceWindowBoundaryFailuresRemainStickyAndClockFree();
	sourceControlTransitionCannotInterruptAuthenticatedBody();
	sourceRequestBudgetRoundTripKeepsRefusalAfterEarlierOwnerRefund();
	sourceRequestBudgetRoundTripKeepsRefusalAfterEarlierOwnerRefund(true);
	sourceRequestBudgetPrefixIncludesEveryDeclaredRequest();
	sourceRequestBudgetZeroByteEventsRemainDifferentFromNotReached();
	sourceRequestBudgetMalformedRehashedFieldsCannotInitialize();
	sourceRequestBudgetCallsRequireCurrentTypedExtentAndOnceOnlyState();
	sourceRequestBudgetActualAllocationAndMissingGrantCannotBeReplaced();
	sourceRequestBudgetOwnerImportMustFinishBeforeRangeRelease();
	sourceRequestBudgetLookupRemainsBoundedAcrossLargeRangePrefix();
	sourceRequestBudgetWireAndFooterAreLiteral();
	sourceRequestBudgetForeignExtentAndUnbudgetedBodyCannotAuthorizeGrant();
	sourceRequestBudgetRangeSlotReuseRetainsAttemptProvenance();
	sourceRequestBudgetQueriesAreInertWithoutConsumerAuthority();
	sourceRequestBudgetExactReadFailureCannotGrantFromUnverifiedBytes();
	sourceRequestBudgetProfileMismatchFailsBeforeImport();
	sourceAbortedBodyUsesCurrentAuthenticatedOwnerWindow();
	lateAbortedBodyRequiresMatchingConsumedWindowAndActualTiming();
	lateAbortedWindowCannotRelabelAttemptOrTurnPureBodySerial();
	lateWindowDoesNotResetBodyOrSettlementFailure();
	lateLiveWindowCannotBeReplacedByPostSealCompletion();
	group6LiteralSpanPreflightDoesNotMintNativeCompletion();
	group6FixedEnvelopeMutationsFailBeforeNativeExecution();
	group6FullZeroAndGrowthHaveRestrictedCompleteGrammar();
	group6FullProfileTransplantsAndMissingTerminalsReject();
	group6SpanBlockBoundaryArithmeticStaysBounded();
	group6UnarmedCollisionKeepsOrdinaryWireAndNonConsumingLookups();
	group6UnarmedLimitsAndAbandonedStateFailClosed();
	group6DefaultAndForeignAuthorityCannotMintBypass();
	group6OrdinarySecondBeginPreservesBothOriginalPrefixes();
	group6UnarmedInputFailuresRemainOnceOnlyAndSticky();
	group6SpanInterleaveDoesNotReapHeldFullAttempt();
	return failures == 0 ? 0 : 1;
}
