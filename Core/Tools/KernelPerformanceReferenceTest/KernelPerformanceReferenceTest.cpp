#include "Lib/KernelPerformanceReference.h"
#include <stdio.h>
#include <string.h>
#include <thread>
#include <stdexcept>

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
}
int main()
{
	modeQuery(); canonicalFields(); disabledAndMatchingReference(); failureAndCommitBoundaries(); orderingAndOwnership();
	return failures == 0 ? 0 : 1;
}
