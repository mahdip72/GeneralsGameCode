#include "Lib/CollisionCandidateKernel.h"
#include "Lib/KernelPerformanceDiagnostics.h"

#include <limits>
#include <stdio.h>

// CMake extracts the actual diagnostic admission and gameplay dirty-order
// statements from one title per executable. No duplicate admission predicate
// or invented traversal-to-observation mapping lives in this fixture.
typedef unsigned UnsignedInt;
struct CollisionIdentityTestGameLogic
{
	unsigned frame;
	unsigned getFrame() const { return frame; }
};
static CollisionIdentityTestGameLogic s_gameLogic = { 0 };
static CollisionIdentityTestGameLogic *TheGameLogic = &s_gameLogic;

// Empty before the fix; once the title uses its private monotonic helper, the
// exact helper declaration/definition is extracted here as well.
#include "CollisionPerformanceIdentityTitleHelper.inc"

namespace
{
using namespace rts::performance;
unsigned failures = 0;

void require(bool condition, const char *message)
{
	if (!condition)
	{
		++failures;
		fprintf(stderr, "%s FAIL: %s\n", COLLISION_PERFORMANCE_TITLE, message);
	}
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
		return ++clock.now;
	}
};

KernelPerformanceBatch beginActualTitleBatch(unsigned frame, UnsignedInt ownerOrdinal)
{
	s_gameLogic.frame = frame;
	KernelPerformanceLedger *performanceLedger = &KernelPerformanceLedger::instance();
#include "CollisionPerformanceIdentityTitleBegin.inc"
	return performanceBatch;
}

unsigned actualGameplayDirtyOrder(UnsignedInt ownerOrdinal)
{
	rts::PartitionCollisionObjectSnapshot ownerSnapshot;
#include "CollisionPerformanceIdentityTitleDirtyOrder.inc"
	return ownerSnapshot.dirtyOrder;
}

void closeIneligibleBatch(KernelPerformanceBatch batch)
{
	if (!batch.valid()) return;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	// The observed first map-load attempt contains these two real capture
	// segments (workspace reservation and snapshot capture), then is ineligible.
	{ KernelPerformanceScope reserve(&ledger, batch, KERNEL_PERFORMANCE_CAPTURE); }
	{ KernelPerformanceScope capture(&ledger, batch, KERNEL_PERFORMANCE_CAPTURE); }
	require(ledger.endBatch(batch, KERNEL_PERFORMANCE_NOT_ADMITTED),
		"a genuine ineligible collision attempt closes without invented admission");
}

void testRepeatedFrameZeroTraversal(unsigned firstOwner, unsigned secondOwner)
{
	Clock clock;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	require(ledger.beginRun(true, Clock::read, &clock), "frame-zero run begins");
	const auto first = beginActualTitleBatch(0, firstOwner);
	KernelPerformanceBatchIdentity firstIdentity;
	require(ledger.describeBatch(first, firstIdentity), "first frame-zero collision is admitted to diagnostics");
	closeIneligibleBatch(first);
	const auto second = beginActualTitleBatch(0, secondOwner);
	KernelPerformanceBatchIdentity secondIdentity;
	require(ledger.describeBatch(second, secondIdentity) && secondIdentity.frame == 0 &&
		secondIdentity.ordinal > firstIdentity.ordinal,
		"a repeated partition update at frame zero receives a new diagnostic identity");
	closeIneligibleBatch(second);
	const auto result = ledger.freeze();
	require(result.complete && result.errors == 0 && result.streamCount == 1 &&
		result.streams[0].attemptedBatches == 2 && result.streams[0].admittedBatches == 0 &&
		result.streams[0].stageSamples[KERNEL_PERFORMANCE_CAPTURE] == 4 &&
		result.streams[0].firstFrame == 0 && result.streams[0].lastFrame == 0,
		"distinct map-load attempts remain represented in one valid frame-zero stream");
}

void testNormalFramesAndGameplayOrder()
{
	Clock clock;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	require(ledger.beginRun(true, Clock::read, &clock), "ordinary collision run begins");
	closeIneligibleBatch(beginActualTitleBatch(0, 0));
	closeIneligibleBatch(beginActualTitleBatch(0, 1));
	closeIneligibleBatch(beginActualTitleBatch(1, 0));
	const auto result = ledger.freeze();
	require(result.complete && result.errors == 0 && result.streamCount == 1 &&
		result.streams[0].attemptedBatches == 3 &&
		result.streams[0].firstFrame == 0 && result.streams[0].lastFrame == 1,
		"normal ordered traversal and advancing world frames remain valid");
	require(actualGameplayDirtyOrder(0) == 0 && actualGameplayDirtyOrder(9) == 9 &&
		actualGameplayDirtyOrder(std::numeric_limits<unsigned>::max()) ==
			std::numeric_limits<unsigned>::max(),
		"authoritative dirty order remains the traversal ordinal, not diagnostic sequence");
}

void testTerminalSealStillRejectsWorldReset()
{
	Clock clock;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	require(ledger.beginRun(true, Clock::read, &clock), "terminal collision run begins");
	closeIneligibleBatch(beginActualTitleBatch(64143, 0));
	require(ledger.sealAdmissions(), "terminal collision admissions seal");
	const unsigned calls = clock.calls;
	const auto reset = beginActualTitleBatch(0, 0);
	require(!reset.valid(), "actual title admission cannot reopen a terminally sealed stream");
	{ KernelPerformanceScope emptyCapture(&ledger, reset, KERNEL_PERFORMANCE_CAPTURE); }
	const auto result = ledger.freeze();
	require(result.complete && result.errors == 0 && clock.calls == calls &&
		result.streamCount == 1 && result.streams[0].attemptedBatches == 1 &&
		result.streams[0].lastFrame == 64143,
		"post-terminal reset remains harmless without dropping pre-terminal evidence");
}
}

int main()
{
	testRepeatedFrameZeroTraversal(0, 0);
	testRepeatedFrameZeroTraversal(9, 0);
	testNormalFramesAndGameplayOrder();
	testTerminalSealStillRejectsWorldReset();
	return failures == 0 ? 0 : 1;
}
