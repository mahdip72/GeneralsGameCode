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

// Omitting owner time outside kernel intervals, inventing a terminal frame,
// or requiring a successful kernel stream breaks these independent totals.
void testWholeFrameSerialPartitionIncludesGapsAndFinalDrain()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	KernelPerformanceTimingRunOptions options;
	options.enabled = true;
	options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = Clock::read;
	options.clockContext = &clock;
	const bool started = ledger.beginRun(options);
	require(started, "phase baseline starts an explicit whole-frame accounting run");
	if (!started) return;
	KernelPerformanceSchedulerBoundary actual;
	actual.submittedJobs = 17;
	actual.executedJobs = 17;
	actual.ownerHelpJobs = 3;
	const auto frame = ledger.beginFrame(1, 40, actual);
	require(frame.valid(), "baseline frame opens with its real scheduler boundary");
	if (!frame.valid()) { ledger.freeze(); return; }
	const rts::JobMetricCounter starts[] = { 110, 135, 170, 190, 205 };
	const rts::JobMetricCounter ends[] = { 130, 165, 185, 200, 215 };
	const rts::JobMetricCounter totals[] = { 20, 30, 15, 10, 10 };
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const auto phase = static_cast<KernelPerformancePhase>(index);
		clock.now = starts[index];
		require(ledger.beginPhase(frame, phase), "ordered real baseline phase opens");
		clock.now = ends[index];
		require(ledger.endPhase(frame, phase), "ordered real baseline phase closes");
	}
	clock.now = 220;
	require(ledger.endFrame(frame, 41, actual),
		"completed frame identity is distinct from its pre-intake owner frame");
	require(ledger.sealAdmissions(), "existing terminal seal closes baseline ingress");
	const unsigned callsAtSeal = clock.calls;
	require(!ledger.beginFrame(2, 0, actual).valid() && clock.calls == callsAtSeal,
		"post-terminal reset cannot add a baseline frame or read its clock");
	clock.now = 230;
	const auto completion = ledger.beginCompletionSerial();
	require(completion.valid(), "actual deferred simulation cleanup opens a serial extent");
	clock.now = 237;
	require(ledger.endCompletionSerial(completion) && ledger.sealExecutionClosure(actual),
		"terminal serial work and unchanged actual scheduler counters close");
	const auto result = ledger.freeze();
	const auto &accounting = result.phaseAccounting;
	require(accounting.requested && accounting.frozen && accounting.complete && accounting.errors == 0,
		"all-serial frame accounting can complete without a fabricated kernel stream");
	require(!result.complete && result.streamCount == 0,
		"complete phase accounting does not invent successful kernel timing evidence");
	require(result.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE &&
		ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"frozen baseline evidence retains its explicit execution role");
	require(accounting.completedFrameCount == 1 && accounting.firstCompletedFrame == 41 &&
		accounting.lastCompletedFrame == 41 && accounting.frameNanoseconds == 120 &&
		accounting.maximumFrameNanoseconds == 120 && accounting.unscopedSerialNanoseconds == 35,
		"frame partition measures all five phases and the exact unscoped owner gaps");
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const auto &phase = accounting.phases[index];
		require(phase.totalNanoseconds == totals[index] && phase.serialNanoseconds == totals[index] &&
			phase.pureNanoseconds == 0 && phase.samples == 1 && phase.maximumNanoseconds == totals[index],
			"uncovered owner phase work is measured serial, never inferred pure");
	}
	require(accounting.completionSerialNanoseconds == 7 && accounting.completionSampleCount == 1,
		"completion cost is measured separately without charging the excluded wall gap");
	require(accounting.schedulerClosureKnown && accounting.schedulerBegin.submittedJobs == 17 &&
		accounting.schedulerEnd.executedJobs == 17 && accounting.schedulerEnd.ownerHelpJobs == 3,
		"actual nonzero historical counters are preserved rather than replaced with synthetic zeros");
}

void testPhaseFailureCannotUnlatchBaselineRole()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	KernelPerformanceTimingRunOptions options;
	options.enabled = true;
	options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = Clock::read;
	options.clockContext = &clock;
	const bool started = ledger.beginRun(options);
	require(started, "phase ordering fixture starts an explicit baseline run");
	if (!started) return;
	KernelPerformanceSchedulerBoundary actual;
	const auto frame = ledger.beginFrame(1, 1, actual);
	require(!ledger.beginPhase(frame, KERNEL_PHASE_SPATIAL_WORK),
		"a baseline cannot skip intake and mutable phases");
	const auto result = ledger.freeze();
	require(!result.phaseAccounting.complete && result.phaseAccounting.errors != 0 &&
		ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE &&
		result.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"failed phase evidence stays a failed baseline, never ordinary pipeline execution");
}

bool startBaseline(KernelPerformanceLedger &ledger, Clock &clock)
{
	KernelPerformanceTimingRunOptions options;
	options.enabled = true;
	options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = Clock::read;
	options.clockContext = &clock;
	const bool started = ledger.beginRun(options);
	require(started, "baseline boundary fixture starts");
	return started;
}

void measuredPhases(KernelPerformanceLedger &ledger, KernelPerformanceFrame frame,
	Clock &clock, rts::JobMetricCounter each)
{
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const auto phase = static_cast<KernelPerformancePhase>(index);
		require(ledger.beginPhase(frame, phase), "boundary fixture opens its next phase");
		clock.now += each;
		require(ledger.endPhase(frame, phase), "boundary fixture closes its next phase");
	}
}

void testPhaseHooksAreInertOutsideBaseline()
{
	Clock clock;
	KernelPerformanceLedger pipeline;
	require(pipeline.beginRun(true, Clock::read, &clock), "ordinary pipeline fixture starts");
	KernelPerformanceSchedulerBoundary actual;
	require(!pipeline.beginFrame(1, 1, actual).valid() &&
		!pipeline.beginPhase(KernelPerformanceFrame(), KERNEL_PHASE_OWNER_INTAKE) &&
		!pipeline.endPhase(KernelPerformanceFrame(), KERNEL_PHASE_OWNER_INTAKE) &&
		!pipeline.endFrame(KernelPerformanceFrame(), 2, actual) &&
		!pipeline.beginCompletionSerial().valid() &&
		!pipeline.endCompletionSerial(KernelPerformanceInterval()) &&
		!pipeline.sealExecutionClosure(actual) && clock.calls == 0,
		"unrequested phase hooks are inert and never read the ordinary clock");
	const auto batch = pipeline.beginBatch(KERNEL_PERFORMANCE_PHYSICS, 0, 1, 1);
	allStages(pipeline, batch, clock);
	require(pipeline.endBatch(batch, KERNEL_PERFORMANCE_COMMITTED), "inert hooks preserve ordinary stages");
	const auto result = pipeline.freeze();
	require(result.complete && !result.phaseAccounting.requested && !result.phaseAccounting.complete,
		"ordinary evidence does not acquire a fabricated serial partition");
	KernelPerformanceLedger disabled;
	KernelPerformanceTimingRunOptions options;
	options.role = KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	options.clock = Clock::read;
	options.clockContext = &clock;
	const unsigned before = clock.calls;
	require(disabled.beginRun(options) && !disabled.beginFrame(1, 1, actual).valid(),
		"disabled baseline identity configures without collecting");
	const auto inactive = disabled.freeze();
	require(!inactive.phaseAccounting.requested && !inactive.phaseAccounting.complete &&
		inactive.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE && clock.calls == before,
		"disabled baseline retains identity but emits neither timings nor coverage");
}

void testMultipleFramesHaveExactIndependentTotals()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	if (!startBaseline(ledger, clock)) return;
	KernelPerformanceSchedulerBoundary actual;
	const auto first = ledger.beginFrame(0, 10, actual);
	clock.now += 2;
	measuredPhases(ledger, first, clock, 1);
	clock.now += 1;
	require(ledger.endFrame(first, 11, actual), "first measured frame closes");
	clock.now = 500;
	const auto second = ledger.beginFrame(1, 11, actual);
	clock.now += 4;
	measuredPhases(ledger, second, clock, 2);
	clock.now += 2;
	require(ledger.endFrame(second, 12, actual), "second measured frame closes");
	const unsigned before = clock.calls;
	require(ledger.sealAdmissions() && ledger.sealExecutionClosure(actual),
		"zero terminal work closes without inventing a completion sample");
	const auto result = ledger.freeze();
	const auto repeated = ledger.freeze();
	const auto &a = result.phaseAccounting;
	require(a.complete && a.completedFrameCount == 2 && a.firstCompletedFrame == 11 &&
		a.lastCompletedFrame == 12 && a.frameNanoseconds == 24 && a.maximumFrameNanoseconds == 16 &&
		a.unscopedSerialNanoseconds == 9 && a.completionSampleCount == 0 &&
		a.completionSerialNanoseconds == 0,
		"declared frames exclude the wall gap and preserve independent sums and maxima");
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
		require(a.phases[index].totalNanoseconds == 3 && a.phases[index].serialNanoseconds == 3 &&
			a.phases[index].pureNanoseconds == 0 && a.phases[index].samples == 2 &&
			a.phases[index].maximumNanoseconds == 2, "phase totals retain each actual frame sample");
	require(repeated.phaseAccounting.complete && repeated.phaseAccounting.frameNanoseconds == 24 &&
		clock.calls == before, "closure and repeated freeze are immutable and clock-free");
}

void testBaselinePipelineNestingDoesNotChangeSerialClassification()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	if (!startBaseline(ledger, clock)) return;
	KernelPerformanceSchedulerBoundary actual;
	const auto frame = ledger.beginFrame(1, 1, actual);
	require(ledger.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "nested timing phase opens");
	clock.now = 110;
	const auto batch = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
	const auto capture = ledger.beginInterval(batch, KERNEL_PERFORMANCE_CAPTURE);
	clock.now = 120;
	interval(ledger, batch, KERNEL_PERFORMANCE_SCHEDULE, clock, 20);
	interval(ledger, batch, KERNEL_PERFORMANCE_WAIT, clock, 5);
	clock.now = 150;
	require(ledger.endInterval(capture), "nested owner capture closes");
	clock.now = 160;
	require(ledger.endBatch(batch, KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION),
		"failed attempt retains its actual ordinary stage evidence");
	clock.now = 170;
	require(ledger.endPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "nested timing phase closes");
	for (unsigned index = 1; index != KERNEL_PHASE_COUNT; ++index)
	{
		const auto phase = static_cast<KernelPerformancePhase>(index);
		require(ledger.beginPhase(frame, phase), "remaining nested-test phase opens");
		++clock.now;
		require(ledger.endPhase(frame, phase), "remaining nested-test phase closes");
	}
	clock.now = 180;
	require(ledger.endFrame(frame, 2, actual) && ledger.sealAdmissions() &&
		ledger.sealExecutionClosure(actual), "nested timing extent closes");
	const auto result = ledger.freeze();
	require(result.complete && result.phaseAccounting.complete &&
		result.phaseAccounting.frameNanoseconds == 80 && result.phaseAccounting.unscopedSerialNanoseconds == 6 &&
		result.phaseAccounting.phases[0].totalNanoseconds == 70 &&
		result.phaseAccounting.phases[0].serialNanoseconds == 70 &&
		result.phaseAccounting.phases[0].pureNanoseconds == 0,
		"owner time, failed work, and genuine WAIT intervals stay serial without double counting");
	require(result.streams[0].activePipelineNanoseconds == 40 &&
		result.streams[0].inclusiveBatchNanoseconds == 50 &&
		result.streams[0].stageNanoseconds[KERNEL_PERFORMANCE_WAIT] == 5,
		"phase accounting does not rewrite or subtract the independent legacy stage ledger");
}

void testPhaseIdentityAndScopeFailuresAreSticky()
{
	for (unsigned misuse = 0; misuse != 8; ++misuse)
	{
		Clock clock;
		KernelPerformanceLedger ledger;
		if (!startBaseline(ledger, clock)) return;
		KernelPerformanceSchedulerBoundary actual;
		auto frame = ledger.beginFrame(1, 1, actual);
		bool rejected = false;
		if (misuse == 0) rejected = !ledger.beginFrame(2, 1, actual).valid();
		else if (misuse == 1) rejected = !ledger.endFrame(frame, 2, actual);
		else if (misuse == 2) { ++frame.sampleOrdinal; rejected = !ledger.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE); }
		else if (misuse == 3) { ++frame.ownerFrameAtEntry; rejected = !ledger.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE); }
		else if (misuse == 4) rejected = !ledger.beginPhase(frame, static_cast<KernelPerformancePhase>(KERNEL_PHASE_COUNT));
		else
		{
			require(ledger.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "misuse fixture opens intake");
			if (misuse == 5) rejected = !ledger.beginPhase(frame, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND);
			else if (misuse == 6) rejected = !ledger.endPhase(frame, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND);
			else
			{
				const auto batch = ledger.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 1, 1);
				ledger.beginInterval(batch, KERNEL_PERFORMANCE_CAPTURE);
				rejected = !ledger.endPhase(frame, KERNEL_PHASE_OWNER_INTAKE);
			}
		}
		const auto failed = ledger.freeze();
		require(rejected && !failed.phaseAccounting.complete && failed.phaseAccounting.errors != 0 &&
			ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
			"overlap, omissions, forged identities, wrong phases, and escaping stages fail closed");
	}
	for (unsigned misuse = 0; misuse != 3; ++misuse)
	{
		Clock clock;
		KernelPerformanceLedger ledger;
		if (!startBaseline(ledger, clock)) return;
		KernelPerformanceSchedulerBoundary actual;
		const auto old = ledger.beginFrame(1, 1, actual);
		measuredPhases(ledger, old, clock, 1);
		require(ledger.endFrame(old, 2, actual), "identity fixture completes its initial frame");
		if (misuse == 0)
			require(!ledger.beginFrame(1, 2, actual).valid(), "sample ordinals cannot repeat");
		else if (misuse == 1)
		{
			const auto fresh = ledger.beginFrame(2, 2, actual);
			measuredPhases(ledger, fresh, clock, 1);
			require(!ledger.endFrame(fresh, 2, actual), "completed frame identities cannot repeat");
		}
		else
		{
			require(ledger.sealAdmissions() && ledger.sealExecutionClosure(actual), "old generation closes");
			require(ledger.freeze().phaseAccounting.complete, "old frame was complete before reset");
			if (!startBaseline(ledger, clock)) return;
			ledger.beginFrame(1, 1, actual);
			require(!ledger.beginPhase(old, KERNEL_PHASE_OWNER_INTAKE), "old-generation frame token cannot address a new frame");
		}
		require(!ledger.freeze().phaseAccounting.complete, "identity failure cannot be repaired by freezing");
	}
}

void testActualSchedulerBoundariesCannotBeSynthesizedOrReset()
{
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		Clock clock;
		KernelPerformanceLedger ledger;
		if (!startBaseline(ledger, clock)) return;
		KernelPerformanceSchedulerBoundary actual;
		actual.submittedJobs = actual.executedJobs = 17;
		actual.ownerHelpJobs = 3;
		if (mismatch == 0 || mismatch == 1)
		{
			if (mismatch == 0) actual.pendingJobs = 1; else actual.outstandingJobs = 1;
			require(!ledger.beginFrame(1, 1, actual).valid(), "baseline entry refuses real queued or outstanding work");
		}
		else
		{
			const auto frame = ledger.beginFrame(1, 1, actual);
			measuredPhases(ledger, frame, clock, 1);
			if (mismatch == 2) ++actual.submittedJobs;
			if (mismatch == 3) ++actual.executedJobs;
			if (mismatch == 4) ++actual.ownerHelpJobs;
			if (mismatch == 5) actual.submittedJobs = actual.executedJobs = actual.ownerHelpJobs = 0;
			if (mismatch != 6)
				require(!ledger.endFrame(frame, 2, actual), "physical dispatch, execution, owner-help, and counter resets fail closure");
			else
			{
				require(ledger.endFrame(frame, 2, actual) && ledger.sealAdmissions(), "terminal boundary fixture seals ingress");
				++actual.executedJobs;
				require(!ledger.sealExecutionClosure(actual), "late execution cannot escape the terminal closure guard");
			}
		}
		const auto failed = ledger.freeze();
		require(!failed.phaseAccounting.complete && !failed.phaseAccounting.schedulerClosureKnown &&
			failed.phaseAccounting.errors != 0, "unsafe actual scheduler counters never become a complete serial baseline");
	}
}

void testCompletionAndClockFailuresCannotProduceCoverage()
{
	for (unsigned misuse = 0; misuse != 7; ++misuse)
	{
		Clock clock;
		KernelPerformanceLedger ledger;
		if (!startBaseline(ledger, clock)) return;
		KernelPerformanceSchedulerBoundary actual;
		const auto frame = ledger.beginFrame(1, 1, actual);
		measuredPhases(ledger, frame, clock, 1);
		require(ledger.endFrame(frame, 2, actual), "terminal misuse fixture completes frame");
		if (misuse == 0)
			require(!ledger.sealExecutionClosure(actual), "execution closure requires the real ingress seal");
		else
		{
			require(ledger.sealAdmissions(), "terminal misuse fixture seals ingress");
			if (misuse != 1)
			{
				auto completion = ledger.beginCompletionSerial();
				require(completion.valid(), "terminal misuse fixture begins real serial work");
				if (misuse == 2) require(!ledger.beginCompletionSerial().valid(), "completion extents cannot overlap");
				if (misuse == 3) { ++completion.serial; require(!ledger.endCompletionSerial(completion), "completion identity must match"); }
				if (misuse == 4) require(!ledger.sealExecutionClosure(actual), "closure cannot hide an open terminal extent");
				if (misuse == 5) { --clock.now; require(!ledger.endCompletionSerial(completion), "terminal clock reversal fails closed"); }
				// Case 6 freezes the still-open extent; case 1 omits closure entirely.
			}
		}
		const auto failed = ledger.freeze();
		require(!failed.phaseAccounting.complete && failed.phaseAccounting.errors != 0,
			"missing, malformed, or unfinished terminal accounting cannot qualify");
	}
	for (unsigned invalid = 0; invalid != 2; ++invalid)
	{
		Clock clock;
		clock.now = invalid == 0 ? 0 : std::numeric_limits<rts::JobMetricCounter>::max();
		KernelPerformanceLedger ledger;
		if (!startBaseline(ledger, clock)) return;
		KernelPerformanceSchedulerBoundary actual;
		require(!ledger.beginFrame(1, 1, actual).valid() &&
			(ledger.freeze().phaseAccounting.errors & KERNEL_PERFORMANCE_ERROR_CLOCK) != 0,
			"zero and saturated clocks cannot become exact phase measurements");
	}
}

void testTimingRoleSurvivesRejectedReconfigurationAndForeignAccess()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	if (!startBaseline(ledger, clock)) return;
	std::thread query([&]() {
		require(ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
			"foreign execution-role query reads only the atomic latch");
	});
	query.join();
	require(!ledger.beginRun(true, Clock::read, &clock) &&
		ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"rejected active-run reset cannot switch a baseline to pipeline routing");
	ledger.freeze();
	KernelPerformanceTimingRunOptions invalid;
	invalid.enabled = true;
	invalid.role = static_cast<KernelPerformanceRunRole>(2);
	require(!ledger.beginRun(invalid) && ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"invalid requested role cannot replace frozen run identity");
	require(ledger.beginRun(true, Clock::read, &clock) && ledger.runRole() == KERNEL_PERFORMANCE_PIPELINE,
		"accepted legacy beginRun explicitly restores the ordinary pipeline role");
	ledger.freeze();
}

void testBaselineCompletionRetainsActualLateStageWork()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	if (!startBaseline(ledger, clock)) return;
	KernelPerformanceSchedulerBoundary actual;
	const auto frame = ledger.beginFrame(1, 1, actual);
	measuredPhases(ledger, frame, clock, 1);
	const auto retained = ledger.beginBatch(KERNEL_PERFORMANCE_PATH, 1, 1, 1);
	clock.now = 106;
	require(ledger.endFrame(frame, 2, actual) && ledger.sealAdmissions(),
		"terminal frame seals ingress without discarding a retained timing batch");
	KernelPerformanceBatchIdentity identity;
	require(ledger.describeBatch(retained, identity) && identity.kernel == KERNEL_PERFORMANCE_PATH,
		"baseline ingress seal preserves retained timing identity");
	clock.now = 200;
	const auto completion = ledger.beginCompletionSerial();
	interval(ledger, retained, KERNEL_PERFORMANCE_VALIDATE, clock, 7);
	clock.now = 210;
	require(ledger.endBatch(retained, KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION),
		"retained failed work closes at its actual owner release");
	clock.now = 215;
	require(ledger.endCompletionSerial(completion) && ledger.sealExecutionClosure(actual),
		"measured final release permits execution closure");
	const auto result = ledger.freeze();
	require(result.complete && result.phaseAccounting.complete &&
		result.phaseAccounting.frameNanoseconds == 6 && result.phaseAccounting.unscopedSerialNanoseconds == 1 &&
		result.phaseAccounting.completionSerialNanoseconds == 15 && result.phaseAccounting.completionSampleCount == 1 &&
		result.streams[0].inclusiveBatchNanoseconds == 105 && result.streams[0].activePipelineNanoseconds == 7,
		"completion includes late owner work exactly once without importing inclusive latency or the wall gap");
}

void testPhaseIntegerPrecisionAndForeignMutation()
{
	Clock clock;
	KernelPerformanceLedger ledger;
	if (!startBaseline(ledger, clock)) return;
	KernelPerformanceSchedulerBoundary actual;
	const auto frame = ledger.beginFrame(1, 1, actual);
	require(ledger.beginPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "large integer phase opens");
	const rts::JobMetricCounter large = (static_cast<rts::JobMetricCounter>(1) << 54) + 1;
	clock.now += large;
	require(ledger.endPhase(frame, KERNEL_PHASE_OWNER_INTAKE), "large integer phase closes exactly");
	for (unsigned index = 1; index != KERNEL_PHASE_COUNT; ++index)
	{
		const auto phase = static_cast<KernelPerformancePhase>(index);
		require(ledger.beginPhase(frame, phase), "unit integer phase opens");
		++clock.now;
		require(ledger.endPhase(frame, phase), "unit integer phase closes");
	}
	clock.now += 3;
	require(ledger.endFrame(frame, 2, actual) && ledger.sealAdmissions() && ledger.sealExecutionClosure(actual),
		"integer partition closes without converting through floating point");
	const auto exact = ledger.freeze();
	require(exact.phaseAccounting.complete && exact.phaseAccounting.frameNanoseconds == large + 7 &&
		exact.phaseAccounting.phases[0].serialNanoseconds == large && exact.phaseAccounting.unscopedSerialNanoseconds == 3,
		"phase and frame accounting preserve integers beyond exact double precision");
	if (!startBaseline(ledger, clock)) return;
	const auto next = ledger.beginFrame(1, 1, actual);
	std::thread foreign([&]() {
		require(!ledger.beginPhase(next, KERNEL_PHASE_OWNER_INTAKE) &&
			ledger.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
			"foreign phase writer is rejected without reading or changing owner state");
	});
	foreign.join();
	const auto failed = ledger.freeze();
	require(!failed.phaseAccounting.complete && (failed.phaseAccounting.errors & KERNEL_PERFORMANCE_ERROR_OWNER) != 0 &&
		failed.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE,
		"foreign mutation fails qualification while retaining the baseline role");
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
	testWholeFrameSerialPartitionIncludesGapsAndFinalDrain();
	testPhaseFailureCannotUnlatchBaselineRole();
	testPhaseHooksAreInertOutsideBaseline();
	testMultipleFramesHaveExactIndependentTotals();
	testBaselinePipelineNestingDoesNotChangeSerialClassification();
	testPhaseIdentityAndScopeFailuresAreSticky();
	testActualSchedulerBoundariesCannotBeSynthesizedOrReset();
	testCompletionAndClockFailuresCannotProduceCoverage();
	testTimingRoleSurvivesRejectedReconfigurationAndForeignAccess();
	testBaselineCompletionRetainsActualLateStageWork();
	testPhaseIntegerPrecisionAndForeignMutation();
	if (failures != 0) fprintf(stderr, "%u kernel performance diagnostics assertions failed\n", failures);
	return failures == 0 ? 0 : 1;
}
