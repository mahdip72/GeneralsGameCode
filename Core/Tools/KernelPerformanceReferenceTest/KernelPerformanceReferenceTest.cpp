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
	return failures == 0 ? 0 : 1;
}
