#include "Lib/KernelPerformanceReference.h"
#include <stdio.h>
#include <string.h>
#include <thread>
#include <stdexcept>
#include <vector>

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
		3, 22, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0
	};
	check(fixture.sink.bytes.size() >= sizeof(footerSuffix) &&
		memcmp(fixture.sink.bytes.data() + fixture.sink.bytes.size() - sizeof(footerSuffix), footerSuffix, sizeof(footerSuffix)) == 0 &&
		trace.byteCount == fixture.sink.bytes.size() && trace.digest.valid,
		"aborted source footer binds the literal seven derived counts at tags sixteen through twenty-two");
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
	return failures == 0 ? 0 : 1;
}
