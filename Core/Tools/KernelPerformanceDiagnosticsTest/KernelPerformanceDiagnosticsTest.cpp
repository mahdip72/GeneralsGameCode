#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#include "Lib/DeterministicAIPlanning.h"

#include <limits>
#include <stdio.h>
#include <thread>

using namespace rts::performance;

namespace
{
unsigned failures = 0;
void require(bool condition, const char *message)
{
	if (!condition) { ++failures; fprintf(stderr, "FAIL: %s\n", message); }
}

struct Clock
{
	Clock() : now(100), calls(0) {}
	rts::JobMetricCounter now;
	unsigned calls;
	static rts::JobMetricCounter read(void *context)
	{
		Clock &clock = *static_cast<Clock *>(context);
		++clock.calls;
		return clock.now;
	}
};

void interval(KernelPerformanceLedger &ledger, KernelPerformanceBatch batch,
	KernelPerformanceStage stage, Clock &clock, rts::JobMetricCounter elapsed)
{
	const KernelPerformanceInterval token = ledger.beginInterval(batch, stage);
	require(token.valid(), "an owner interval opens");
	clock.now += elapsed;
	require(ledger.endInterval(token), "an owner interval closes");
}

void allStages(KernelPerformanceLedger &ledger, KernelPerformanceBatch batch,
	Clock &clock)
{
	for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		interval(ledger, batch, static_cast<KernelPerformanceStage>(stage), clock, 1);
}

void testTerminalSealExcludesResetFrame()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	ledger.beginRun(true, Clock::read, &clock);
	const auto status = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 64143, 1);
	allStages(ledger, status, clock);
	require(ledger.endBatch(status, KERNEL_PERFORMANCE_COMMITTED),
		"terminal seal fixture retains a genuinely committed match status batch");
	const unsigned callsBeforeSeal = clock.calls;
	require(ledger.sealAdmissions() && ledger.sealAdmissions(),
		"owner terminal seal is successful and idempotent");
	// The real empty status sweep opens its capture scope before discovering
	// zero objects in the reset world. It must not admit frame zero or poison
	// the still-draining run through its canonical empty batch token.
	const auto reset = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 0, 2);
	require(reset.generation == 0 && reset.serial == 0 &&
		reset.slot == KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES,
		"post-terminal reset returns the canonical empty batch token");
	{ KernelPerformanceScope scope(&ledger, reset, KERNEL_PERFORMANCE_CAPTURE); }
	require(clock.calls == callsBeforeSeal, "terminal seal and reset scope read no clocks");
	const auto result = ledger.freeze();
	require(result.complete && result.errors == 0 && result.streamCount == 1 &&
		result.streams[0].attemptedBatches == 1 && result.streams[0].committedBatches == 1 &&
		result.streams[0].firstFrame == 64143 && result.streams[0].lastFrame == 64143,
		"post-terminal reset cannot corrupt the closed match timing snapshot");
}

void testSealedAdmissionCannotBecomeEvidence()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	ledger.beginRun(true, Clock::read, &clock);
	const auto retained = ledger.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 64143, 1);
	const auto capture = ledger.beginInterval(retained, KERNEL_PERFORMANCE_CAPTURE);
	ledger.sealAdmissions();
	const unsigned callsBeforeAttempt = clock.calls;
	const auto rejected = ledger.beginBatch(KERNEL_PERFORMANCE_AI, 0, 64144, 1);
	require(!rejected.valid(), "terminal seal rejects new batches even with increasing frame identities");
	{ KernelPerformanceScope scope(&ledger, rejected, KERNEL_PERFORMANCE_CAPTURE); }
	require(clock.calls == callsBeforeAttempt,
		"post-seal admission and canonical empty scope have no timing side effects");
	// Keep the inert-API RED bounded: close any incorrectly admitted batch.
	if (rejected.valid()) ledger.endBatch(rejected, KERNEL_PERFORMANCE_NOT_ADMITTED);
	KernelPerformanceBatchIdentity identity;
	require(ledger.describeBatch(retained, identity) && identity.frame == 64143,
		"terminal seal preserves exact identity of retained work");
	++clock.now;
	require(ledger.endInterval(capture), "a pre-seal interval can genuinely close after terminal capture");
	for (unsigned stage = KERNEL_PERFORMANCE_SCHEDULE; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		interval(ledger, retained, static_cast<KernelPerformanceStage>(stage), clock, 1);
	require(ledger.endBatch(retained, KERNEL_PERFORMANCE_COMMITTED),
		"retained work can observe remaining stages and commit after the seal");
	const auto result = ledger.freeze();
	require(result.complete && result.streamCount == 1 && result.streams[0].committedBatches == 1,
		"only retained work contributes to the sealed run snapshot");
	require(ledger.beginRun(true, Clock::read, &clock), "explicit new run may follow a sealed frozen run");
	const auto fresh = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 0, 1);
	require(fresh.valid(), "a new explicit generation reopens admission");
	ledger.endBatch(fresh, KERNEL_PERFORMANCE_NOT_ADMITTED);
	require(ledger.freeze().complete, "fresh generation does not inherit terminal admission state");
}

struct DeferredReference
{
	unsigned value;
	mutable unsigned writes;
	unsigned computes;
};
bool writeDeferredReference(KernelPerformanceCanonicalWriter &writer, const void *context)
{
	const DeferredReference &value = *static_cast<const DeferredReference *>(context);
	++value.writes;
	return writer.u32(1, value.value);
}
bool computeDeferredReference(const void *context, void *output)
{
	const DeferredReference &input = *static_cast<const DeferredReference *>(context);
	DeferredReference &detached = *static_cast<DeferredReference *>(output);
	++detached.computes;
	detached.value = input.value;
	return true;
}

void testSealedTimingRetainsLateReferenceBinding()
{
	Clock clock;
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	KernelPerformanceReferenceLedger reference;
	timing.beginRun(true, Clock::read, &clock);
	reference.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE, Clock::read, &clock);
	auto retained = timing.beginBatch(KERNEL_PERFORMANCE_AI, 0, 64143, 1);
	for (unsigned stage = KERNEL_PERFORMANCE_CAPTURE; stage != KERNEL_PERFORMANCE_VALIDATE; ++stage)
		interval(timing, retained, static_cast<KernelPerformanceStage>(stage), clock, 1);
	timing.sealAdmissions();
	require(reference.mode() == KERNEL_REFERENCE_SERIAL_ORACLE,
		"timing seal does not disable the reference mode needed by retained work");
	DeferredReference input = { 7, 0, 0 }, actual = { 7, 0, 0 }, detached = { 99, 0, 0 };
	KernelPerformanceReferenceBatch referenceBatch;
	rts::AIPlanningReferenceBatchTransport transport;
	transport.referenceLedger = &reference;
	transport.referenceBatch = &referenceBatch;
	transport.writeInput = writeDeferredReference;
	transport.immutableInput = &input;
	transport.writeOutput = writeDeferredReference;
	transport.productionOutput = &actual;
	transport.serialCompute = computeDeferredReference;
	transport.detachedSerialOutput = &detached;
	transport.operationCount = 1;
	{
		KernelPerformanceScope validate(&timing, retained, KERNEL_PERFORMANCE_VALIDATE);
		require(rts::ObserveAIPlanningReferenceBatch(&retained, &transport),
			"actual product reference helper can bind retained timing work after terminal seal");
	}
	require(referenceBatch.valid() && input.writes == 1 && actual.writes == 1 &&
		detached.writes == 1 && detached.computes == 1 && actual.value == 7,
		"late reference binding uses detached oracle output without replacing actual output");
	interval(timing, retained, KERNEL_PERFORMANCE_COMMIT, clock, 1);
	require(timing.endBatch(retained, KERNEL_PERFORMANCE_COMMITTED) &&
		rts::FinishAIPlanningReferenceBatch(&transport, true),
		"retained timing and reference tokens close only after their real commit");
	auto rejected = timing.beginBatch(KERNEL_PERFORMANCE_AI, 0, 64144, 2);
	require(!rts::ObserveAIPlanningReferenceBatch(&rejected, &transport) &&
		input.writes == 1 && actual.writes == 1 && detached.computes == 1,
		"actual product reference helper cannot hash or compute a post-seal batch");
	// Close incorrectly admitted RED evidence without leaving singleton work.
	if (rejected.valid()) timing.endBatch(rejected, KERNEL_PERFORMANCE_NOT_ADMITTED);
	if (referenceBatch.valid()) rts::FinishAIPlanningReferenceBatch(&transport, false);
	const auto timingResult = timing.freeze();
	const auto referenceResult = reference.freeze();
	require(timingResult.complete && referenceResult.complete &&
		timingResult.streams[0].attemptedBatches == 1 && referenceResult.streams[0].validatedBatchCount == 1 &&
		referenceResult.streams[0].committedBatchCount == 1,
		"sealed run freezes exactly the retained committed timing/reference pair");
}

void testTerminalSealPreservesFailureBoundaries()
{
	Clock clock;
	for (unsigned malformed = 0; malformed != 4; ++malformed)
	{
		KernelPerformanceLedger ledger;
		ledger.beginRun(true, Clock::read, &clock);
		auto stale = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
		ledger.endBatch(stale, KERNEL_PERFORMANCE_NOT_ADMITTED);
		ledger.sealAdmissions();
		KernelPerformanceBatch token;
		if (malformed == 0) token = stale;
		if (malformed == 1) token.generation = stale.generation;
		if (malformed == 2) token.serial = stale.serial;
		if (malformed == 3) token.slot = 0;
		require(!ledger.beginInterval(token, KERNEL_PERFORMANCE_CAPTURE).valid() &&
			(ledger.freeze().errors & KERNEL_PERFORMANCE_ERROR_IDENTITY) != 0,
			"seal does not excuse stale, partial or malformed nonempty scope tokens");
	}
	KernelPerformanceLedger open;
	open.beginRun(true, Clock::read, &clock);
	const auto retained = open.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 1, 1);
	open.beginInterval(retained, KERNEL_PERFORMANCE_WAIT);
	open.sealAdmissions();
	require((open.freeze().errors & KERNEL_PERFORMANCE_ERROR_INCOMPLETE) != 0,
		"terminal sealing cannot disguise unclosed retained work as a drained run");
	for (unsigned action = 0; action != 2; ++action)
	{
		KernelPerformanceLedger foreign;
		foreign.beginRun(true, Clock::read, &clock);
		const auto closed = foreign.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
		foreign.endBatch(closed, KERNEL_PERFORMANCE_NOT_ADMITTED);
		foreign.sealAdmissions();
		std::thread worker([&]() {
			if (action == 0) foreign.sealAdmissions();
			else foreign.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 2, 2);
		});
		worker.join();
		require((foreign.freeze().errors & KERNEL_PERFORMANCE_ERROR_OWNER) != 0,
			"foreign post-seal actions remain ownership failures");
	}
	KernelPerformanceLedger disabled;
	disabled.beginRun(false, Clock::read, &clock);
	const unsigned calls = clock.calls;
	disabled.sealAdmissions();
	const auto empty = disabled.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 0, 1);
	{ KernelPerformanceScope scope(&disabled, empty, KERNEL_PERFORMANCE_CAPTURE); }
	const auto result = disabled.freeze();
	require(!result.enabled && result.errors == 0 && clock.calls == calls,
		"disabled seal remains clock-free and produces no fabricated evidence");
}

// Including nested intervals in both parent and child, or including the idle
// gaps between object commits, breaks these hand-calculated totals.
void testActiveBatchIdentityQuery()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	KernelPerformanceBatchIdentity identity;
	identity.ordinal = 999;
	require(!ledger.describeBatch(KernelPerformanceBatch(), identity) && identity.ordinal == 999,
		"unstarted identity query preserves caller storage");
	ledger.beginRun(true, Clock::read, &clock);
	const auto first = ledger.beginBatch(KERNEL_PERFORMANCE_COLLISION, 0, 1, 0);
	const auto second = ledger.beginBatch(KERNEL_PERFORMANCE_COLLISION, 0, 2, 10);
	const unsigned calls = clock.calls;
	require(ledger.describeBatch(first, identity) && identity.kernel == KERNEL_PERFORMANCE_COLLISION &&
		identity.subtype == 0 && identity.frame == 1 && identity.ordinal == 0,
		"identity query retains each active batch frame and zero-based ordinal");
	require(ledger.describeBatch(second, identity) && identity.frame == 2 && identity.ordinal == 10,
		"later active batch has a distinct exact identity");
	std::thread foreign([&]() {
		KernelPerformanceBatchIdentity untouched; untouched.ordinal = 999;
		require(!ledger.describeBatch(first, untouched) && untouched.ordinal == 999,
			"foreign identity query is inert and preserves output");
	});
	foreign.join();
	require(clock.calls == calls, "identity queries do not sample clocks");
	ledger.endBatch(first, KERNEL_PERFORMANCE_NOT_ADMITTED);
	require(!ledger.describeBatch(first, identity), "closed token cannot disclose a reused slot identity");
	ledger.endBatch(second, KERNEL_PERFORMANCE_NOT_ADMITTED);
	require(ledger.freeze().complete, "invalid and foreign queries do not poison accounting");
	require(!ledger.describeBatch(second, identity), "frozen identity query is inert");
}
void testExclusiveIntervalsAndInclusiveLatency()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	require(ledger.beginRun(true, Clock::read, &clock), "enabled run starts");
	const KernelPerformanceBatch batch = ledger.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 10, 1);
	require(batch.valid(), "batch starts");
	const KernelPerformanceInterval capture = ledger.beginInterval(batch, KERNEL_PERFORMANCE_CAPTURE);
	clock.now = 110;
	interval(ledger, batch, KERNEL_PERFORMANCE_SCHEDULE, clock, 20);
	clock.now = 140;
	require(ledger.endInterval(capture), "capture resumes after its nested schedule");
	interval(ledger, batch, KERNEL_PERFORMANCE_WAIT, clock, 30);
	clock.now = 200;
	interval(ledger, batch, KERNEL_PERFORMANCE_VALIDATE, clock, 5);
	interval(ledger, batch, KERNEL_PERFORMANCE_COMMIT, clock, 5);
	clock.now = 220;
	interval(ledger, batch, KERNEL_PERFORMANCE_COMMIT, clock, 3);
	clock.now = 230;
	require(ledger.endBatch(batch, KERNEL_PERFORMANCE_COMMITTED), "fully observed batch commits");
	const KernelPerformanceSnapshot result = ledger.freeze();
	require(result.complete && result.streamCount == 1, "complete run freezes one stream");
	const KernelPerformanceStream &stream = result.streams[0];
	require(stream.stageNanoseconds[KERNEL_PERFORMANCE_CAPTURE] == 20, "capture excludes nested schedule");
	require(stream.stageNanoseconds[KERNEL_PERFORMANCE_SCHEDULE] == 20, "schedule is measured once");
	require(stream.stageNanoseconds[KERNEL_PERFORMANCE_WAIT] == 30, "passive wait is measured once");
	require(stream.stageNanoseconds[KERNEL_PERFORMANCE_VALIDATE] == 5, "validation is separate");
	require(stream.stageNanoseconds[KERNEL_PERFORMANCE_COMMIT] == 8, "distributed commits retain only active intervals");
	require(stream.stageSamples[KERNEL_PERFORMANCE_COMMIT] == 2, "distributed commit segments remain counted");
	require(stream.activePipelineNanoseconds == 83, "active pipeline excludes nested duplication and idle gaps");
	require(stream.inclusiveBatchNanoseconds == 130, "inclusive latency retains idle gaps separately");
	require(stream.maximumBatchNanoseconds == 130, "maximum latency is an observed batch");
	require(stream.attemptedBatches == 1 && stream.admittedBatches == 1 && stream.committedBatches == 1,
		"admission and commit counts correlate");
	require(stream.firstFrame == 10 && stream.lastFrame == 10, "stream is frame-bound");
}

void testCrossKernelNestingAndAbortedWork()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	require(ledger.beginRun(true, Clock::read, &clock), "nested run starts");
	const KernelPerformanceBatch outer = ledger.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 1, 1);
	const KernelPerformanceInterval parent = ledger.beginInterval(outer, KERNEL_PERFORMANCE_CAPTURE);
	clock.now = 110;
	const KernelPerformanceBatch inner = ledger.beginBatch(KERNEL_PERFORMANCE_PATH, 1, 1, 1);
	interval(ledger, inner, KERNEL_PERFORMANCE_SCHEDULE, clock, 20);
	require(ledger.endBatch(inner, KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION), "aborted admitted work remains observable");
	clock.now = 140;
	require(ledger.endInterval(parent), "outer interval closes");
	for (unsigned stage = KERNEL_PERFORMANCE_SCHEDULE; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		interval(ledger, outer, static_cast<KernelPerformanceStage>(stage), clock, 0);
	require(ledger.endBatch(outer, KERNEL_PERFORMANCE_COMMITTED), "outer batch commits");
	const KernelPerformanceSnapshot result = ledger.freeze();
	require(result.complete && result.streamCount == 2, "aborted work does not corrupt accounting completeness");
	require(result.streams[0].activePipelineNanoseconds == 20 &&
		result.streams[1].activePipelineNanoseconds == 20, "cross-kernel nesting is exclusive across the ledger");
	require(result.streams[1].admittedBatches == 1 && result.streams[1].abortedBatches == 1 &&
		result.streams[1].committedBatches == 0, "aborted work cannot masquerade as an accepted speedup sample");
}

void testDisabledRunHasNoClockSideEffects()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	require(ledger.beginRun(false, Clock::read, &clock), "disabled run configures");
	const KernelPerformanceBatch batch = ledger.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 1, 1);
	{ KernelPerformanceScope scope(&ledger, batch, KERNEL_PERFORMANCE_CAPTURE); }
	require(!batch.valid() && !ledger.endBatch(batch, KERNEL_PERFORMANCE_NOT_ADMITTED), "disabled hooks return no evidence");
	const KernelPerformanceSnapshot result = ledger.freeze();
	require(!result.enabled && !result.complete && result.streamCount == 0 && clock.calls == 0,
		"disabled diagnostics read no clock and produce no timing");
}

void testFreezeAndResetGeneration()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	require(ledger.beginRun(true, Clock::read, &clock), "first generation starts");
	const KernelPerformanceBatch old = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
	allStages(ledger, old, clock);
	require(ledger.endBatch(old, KERNEL_PERFORMANCE_COMMITTED), "first generation commits");
	const KernelPerformanceSnapshot first = ledger.freeze();
	require(first.complete, "first generation complete");
	const unsigned callsAtFreeze = clock.calls;
	require(!ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 2, 2).valid(), "frozen run rejects new batches");
	require(ledger.freeze().complete && clock.calls == callsAtFreeze, "repeated freeze is immutable and clock-free");
	require(ledger.beginRun(true, Clock::read, &clock), "explicit new generation resets frozen observations");
	const KernelPerformanceBatch fresh = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
	allStages(ledger, fresh, clock);
	require(ledger.endBatch(fresh, KERNEL_PERFORMANCE_COMMITTED), "new generation accepts reused frame identity");
	const KernelPerformanceSnapshot second = ledger.freeze();
	require(second.complete && second.streams[0].attemptedBatches == 1 &&
		second.generation != first.generation, "reset cannot combine generations");
	require(ledger.beginRun(true, Clock::read, &clock), "third generation starts");
	require(!ledger.beginInterval(old, KERNEL_PERFORMANCE_CAPTURE).valid(), "stale token cannot write into new generation");
	require(!ledger.freeze().complete, "stale-token misuse fails closed");
}

void testIncompleteAndMisorderedScopesFailClosed()
{
	Clock clock;
	KernelPerformanceLedger missing;
	require(missing.beginRun(true, Clock::read, &clock), "missing-stage run starts");
	const KernelPerformanceBatch batch = missing.beginBatch(KERNEL_PERFORMANCE_COLLISION, 0, 1, 1);
	interval(missing, batch, KERNEL_PERFORMANCE_CAPTURE, clock, 1);
	require(!missing.endBatch(batch, KERNEL_PERFORMANCE_COMMITTED), "committed batch requires every stage to be explicitly observed");
	require(!missing.freeze().complete, "missing stage is not an invented zero");
	KernelPerformanceLedger ordered;
	require(ordered.beginRun(true, Clock::read, &clock), "scope-order run starts");
	const KernelPerformanceBatch orderedBatch = ordered.beginBatch(KERNEL_PERFORMANCE_COLLISION, 0, 1, 1);
	const KernelPerformanceInterval outer = ordered.beginInterval(orderedBatch, KERNEL_PERFORMANCE_CAPTURE);
	ordered.beginInterval(orderedBatch, KERNEL_PERFORMANCE_VALIDATE);
	require(!ordered.endInterval(outer) && !ordered.freeze().complete, "non-LIFO scopes fail closed");
	KernelPerformanceLedger open;
	require(open.beginRun(true, Clock::read, &clock), "open-batch run starts");
	open.beginBatch(KERNEL_PERFORMANCE_COLLISION, 0, 1, 1);
	require(!open.freeze().complete, "freeze cannot silently discard an open batch");
}

void testClockFailureAndAggregateOverflow()
{
	Clock clock;
	KernelPerformanceLedger backwards;
	require(backwards.beginRun(true, Clock::read, &clock), "clock run starts");
	const KernelPerformanceBatch batch = backwards.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1);
	clock.now = 99;
	require(!backwards.beginInterval(batch, KERNEL_PERFORMANCE_CAPTURE).valid(), "backward clock is rejected");
	require(!backwards.freeze().complete, "clock failure cannot become huge unsigned elapsed time");
	KernelPerformanceLedger zero;
	clock.now = 0;
	require(zero.beginRun(true, Clock::read, &clock), "unread clock may configure");
	require(!zero.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1).valid() && !zero.freeze().complete,
		"unavailable clock fails closed");
	KernelPerformanceLedger overflow;
	clock.now = 1;
	require(overflow.beginRun(true, Clock::read, &clock), "overflow run starts");
	const KernelPerformanceBatch first = overflow.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1);
	const KernelPerformanceBatch second = overflow.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 2);
	clock.now = std::numeric_limits<rts::JobMetricCounter>::max() - 1;
	require(overflow.endBatch(first, KERNEL_PERFORMANCE_NOT_ADMITTED), "first large inclusive latency fits");
	require(!overflow.endBatch(second, KERNEL_PERFORMANCE_NOT_ADMITTED) && !overflow.freeze().complete,
		"overlapping inclusive latencies cannot silently wrap their aggregate");
}

void testForeignOwnerAndCapacityFailClosed()
{
	Clock clock;
	KernelPerformanceLedger owner;
	require(owner.beginRun(true, Clock::read, &clock), "owner run starts");
	bool foreignRejected = false;
	std::thread foreign([&]() {
		foreignRejected = !owner.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1).valid();
	});
	foreign.join();
	require(foreignRejected && !owner.freeze().complete, "foreign writer is rejected and poisons qualification");
	KernelPerformanceLedger bounded;
	require(bounded.beginRun(true, Clock::read, &clock), "bounded run starts");
	for (unsigned ordinal = 0; ordinal != KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES; ++ordinal)
		require(bounded.beginBatch(KERNEL_PERFORMANCE_SPATIAL, 0, 1, ordinal).valid(), "bounded batch slot is available");
	require(!bounded.beginBatch(KERNEL_PERFORMANCE_SPATIAL, 0, 1,
		KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES).valid() && !bounded.freeze().complete,
		"capacity exhaustion is explicit rather than dropping evidence");
}

void testScopeGuardAndSubtypeIdentity()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	require(ledger.beginRun(true, Clock::read, &clock), "scope guard run starts");
	const KernelPerformanceBatch enemy = ledger.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1);
	{ KernelPerformanceScope scope(&ledger, enemy, KERNEL_PERFORMANCE_CAPTURE); clock.now += 7; }
	require(ledger.endBatch(enemy, KERNEL_PERFORMANCE_NOT_ADMITTED), "scope guard closes on return");
	const KernelPerformanceBatch production = ledger.beginBatch(KERNEL_PERFORMANCE_AI, 1, 1, 1);
	allStages(ledger, production, clock);
	require(ledger.endBatch(production, KERNEL_PERFORMANCE_COMMITTED), "different subtype has independent ordinal space");
	const KernelPerformanceSnapshot result = ledger.freeze();
	require(result.complete && result.streamCount == 2 && result.streams[0].subtype == 0 &&
		result.streams[1].subtype == 1 && result.streams[0].activePipelineNanoseconds == 7,
		"AI enemy and production evidence cannot collapse into one indistinguishable sample");
	require(ledger.beginRun(true, Clock::read, &clock), "identity validation run starts");
	ledger.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1);
	require(!ledger.beginBatch(KERNEL_PERFORMANCE_AI, 0, 1, 1).valid() && !ledger.freeze().complete,
		"duplicate batch identities fail closed");
}
}

int main()
{
	testTerminalSealExcludesResetFrame();
	testSealedAdmissionCannotBecomeEvidence();
	testSealedTimingRetainsLateReferenceBinding();
	testTerminalSealPreservesFailureBoundaries();
	testExclusiveIntervalsAndInclusiveLatency();
	testActiveBatchIdentityQuery();
	testCrossKernelNestingAndAbortedWork();
	testDisabledRunHasNoClockSideEffects();
	testFreezeAndResetGeneration();
	testIncompleteAndMisorderedScopesFailClosed();
	testClockFailureAndAggregateOverflow();
	testForeignOwnerAndCapacityFailClosed();
	testScopeGuardAndSubtypeIdentity();
	if (failures != 0) fprintf(stderr, "%u kernel performance diagnostics assertions failed\n", failures);
	return failures == 0 ? 0 : 1;
}
