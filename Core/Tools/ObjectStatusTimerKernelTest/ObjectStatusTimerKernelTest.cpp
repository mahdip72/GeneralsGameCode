/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectStatusTimerKernel.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#include "../TestSupport/NativeKernelSourceConsumerTest.h"
#include <chrono>
#include <thread>
#include "Lib/SimulationCommandBuffer.h"
#endif

#include <limits.h>
#include <stdio.h>
#if defined(_WIN64)
#include <string.h>
#include <vector>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
#endif

namespace
{
int failures = 0;

void expect(bool condition, const char *message)
{
	if (condition)
		return;
	printf("FAIL: %s\n", message);
	++failures;
}

void clearSnapshot(rts::ObjectStatusTimerSnapshot &snapshot,
	unsigned objectID, unsigned ownerOrder)
{
	snapshot.objectID = objectID;
	snapshot.ownerOrder = ownerOrder;
	snapshot.activeMask = 0;
	for (unsigned type = 0;
		type != rts::OBJECT_STATUS_TIMER_MAX_TYPES; ++type)
		snapshot.expirationFrame[type] = UINT_MAX;
}

void setExpired(rts::ObjectStatusTimerSnapshot &snapshot, unsigned type,
	unsigned expirationFrame)
{
	snapshot.activeMask |= 1u << type;
	snapshot.expirationFrame[type] = expirationFrame;
}

void testLiveAdapterPreflightSkipsForcedSerialPreparation()
{
	unsigned preparedSnapshots = 0;
	if (rts::ShouldPrepareLiveObjectStatusTimerSnapshot(true, true, 1,
		false, true))
	{
		++preparedSnapshots;
	}
	expect(preparedSnapshots == 0,
		"forced-one live status lane performs zero snapshot preparation");
	expect(!rts::ShouldPrepareLiveObjectStatusTimerSnapshot(false, true, 8,
		false, true) &&
		!rts::ShouldPrepareLiveObjectStatusTimerSnapshot(true, false, 8,
			false, true) &&
		!rts::ShouldPrepareLiveObjectStatusTimerSnapshot(true, true, 8,
			true, false),
		"disabled policy and invalid scheduler ownership fail before capture");
	expect(rts::ShouldPrepareLiveObjectStatusTimerSnapshot(true, true, 8,
		false, true),
		"multi-worker game owner remains eligible for status preparation");
}

void testSerialTimerDecisionsAndLegacyOrder()
{
	rts::ObjectStatusTimerSnapshot snapshots[5];
	clearSnapshot(snapshots[0], 90, 4);
	clearSnapshot(snapshots[1], 120, 1);
	clearSnapshot(snapshots[2], 40, 3);
	clearSnapshot(snapshots[3], 300, 0);
	clearSnapshot(snapshots[4], 80, 2);
	setExpired(snapshots[0], 2, 99);
	setExpired(snapshots[1], 0, 100);
	setExpired(snapshots[1], 4, 101);
	setExpired(snapshots[2], 3, 1);
	setExpired(snapshots[3], 1, UINT_MAX);
	setExpired(snapshots[4], 5, 100);

	rts::ObjectStatusTimerCommand output[5];
	unsigned outputCount = 99;
	rts::ObjectStatusTimerOptions options;
	rts::ObjectStatusTimerMetrics metrics;
	const rts::ObjectStatusTimerResult result =
		rts::PrepareObjectStatusTimerCommands(snapshots, 5, 100, 13,
			output, 5, options, &outputCount, &metrics);
	expect(result == rts::OBJECT_STATUS_TIMER_SERIAL,
		"small owner snapshot uses serial command preparation");
	expect(outputCount == 4, "only objects with expired timers emit commands");
	expect(output[1].objectID == 80 && output[1].ownerOrder == 2 &&
		output[1].expiredMask == (1u << 5),
		"commands follow legacy traversal ordinal, not ObjectID sort");
	expect(output[2].objectID == 40 && output[2].ownerOrder == 3 &&
		output[2].expiredMask == (1u << 3),
		"middle command retains owner traversal order");
	expect(output[3].objectID == 90 && output[3].ownerOrder == 4 &&
		output[3].expiredMask == (1u << 2),
		"last expired command retains owner traversal order");
	// Owner order 1 appears before 2; type 4 is not expired at frame 100.
	expect(output[0].ownerOrder == 1 && output[0].objectID == 120 &&
		output[0].expiredMask == 1u,
		"inclusive expiry uses currentFrame >= expirationFrame");
	expect(metrics.evaluatedSnapshots == 5 && metrics.emittedCommands == 4 &&
		metrics.submittedJobs == 0 && metrics.serialFallbacks == 0,
		"serial metrics distinguish evaluation from scheduler fallback");
}

void fillParallelSnapshots(rts::ObjectStatusTimerSnapshot *snapshots,
	unsigned count)
{
	for (unsigned index = 0; index != count; ++index)
	{
		clearSnapshot(snapshots[index], 1000 + index, count - index - 1);
		setExpired(snapshots[index], index % 13, 50);
		if ((index % 7) == 0)
			setExpired(snapshots[index], (index + 3) % 13, 101);
	}
}

void testNoSchedulerIsQuietSerialDecision()
{
	enum { SNAPSHOT_COUNT = 300 };
	rts::ObjectStatusTimerSnapshot snapshots[SNAPSHOT_COUNT];
	fillParallelSnapshots(snapshots, SNAPSHOT_COUNT);
	rts::ObjectStatusTimerCommand serialOutput[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerCommand output[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerOptions options;
	options.parallel = true;
	rts::ObjectStatusTimerMetrics metrics;
	unsigned outputCount = 0;
	unsigned serialCount = 0;
	rts::ObjectStatusTimerOptions serialOptions;
	expect(rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
		100, 13, serialOutput, SNAPSHOT_COUNT, serialOptions, &serialCount) ==
		rts::OBJECT_STATUS_TIMER_SERIAL,
		"serial oracle prepares the >=256-snapshot fixture");
	const rts::ObjectStatusTimerResult result =
		rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT, 100,
			13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics);
	expect(result == rts::OBJECT_STATUS_TIMER_SERIAL,
		"missing scheduler computes the immutable snapshot serially");
	expect(outputCount == SNAPSHOT_COUNT,
		"quiet serial decision emits every expired object");
	expect(metrics.submittedJobs == 0 && metrics.serialFallbacks == 0,
		"missing scheduler does not add fallback metric noise");
	unsigned firstDifference = 99;
	expect(rts::ObjectStatusTimerCommandsEqual(serialOutput, serialCount,
		output, outputCount, &firstDifference),
		"quiet serial path matches the explicit serial command oracle");
	expect(output[0].ownerOrder == 0 &&
		output[SNAPSHOT_COUNT - 1].ownerOrder == SNAPSHOT_COUNT - 1,
		"serial decision still canonicalizes legacy owner order");
}

void testTransactionalFailureAndShadowComparison()
{
	rts::ObjectStatusTimerSnapshot snapshot;
	clearSnapshot(snapshot, 8, 0);
	setExpired(snapshot, 2, 1);
	rts::ObjectStatusTimerCommand output;
	output.objectID = 777;
	output.ownerOrder = 888;
	output.expiredMask = 999;
	unsigned outputCount = 42;
	rts::ObjectStatusTimerOptions options;
	const rts::ObjectStatusTimerResult result =
		rts::PrepareObjectStatusTimerCommands(&snapshot, 1, 3, 13,
			&output, 0, options, &outputCount);
	expect(result == rts::OBJECT_STATUS_TIMER_INVALID_INPUT,
		"insufficient publication capacity is rejected");
	expect(output.objectID == 777 && output.ownerOrder == 888 &&
		output.expiredMask == 999 && outputCount == 42,
		"failed preparation publishes no partial command state");

	rts::ObjectStatusTimerCommand serial[2];
	serial[0].objectID = 7;
	serial[0].ownerOrder = 2;
	serial[0].expiredMask = 1;
	serial[1].objectID = 9;
	serial[1].ownerOrder = 5;
	serial[1].expiredMask = 4;
	rts::ObjectStatusTimerCommand parallel[2];
	parallel[0] = serial[0];
	parallel[1] = serial[1];
	unsigned firstDifference = 99;
	expect(rts::ObjectStatusTimerCommandsEqual(serial, 2, parallel, 2,
		&firstDifference), "shadow comparison accepts identical commands");
	parallel[1].expiredMask = 8;
	expect(!rts::ObjectStatusTimerCommandsEqual(serial, 2, parallel, 2,
		&firstDifference) && firstDifference == 1,
		"shadow comparison reports the first mismatching command");
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
void testRealJobSystemPathAndFailure()
{
	enum { SNAPSHOT_COUNT = 300 };
	rts::ObjectStatusTimerSnapshot snapshots[SNAPSHOT_COUNT];
	fillParallelSnapshots(snapshots, SNAPSHOT_COUNT);
	rts::ObjectStatusTimerCommand serialOutput[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerCommand output[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerOptions options;
	options.parallel = true;
	rts::ObjectStatusTimerMetrics metrics;
	unsigned outputCount = 0;
	unsigned serialCount = 0;
	rts::ObjectStatusTimerOptions serialOptions;
	expect(rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
		100, 13, serialOutput, SNAPSHOT_COUNT, serialOptions, &serialCount) ==
		rts::OBJECT_STATUS_TIMER_SERIAL,
		"serial oracle prepares the real-worker fixture");

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "job system starts for the real parallel path");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"test thread registers as the simulation owner");

	rts::ObjectStatusTimerResult result =
		rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT, 100,
			13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics);
	expect(result == rts::OBJECT_STATUS_TIMER_PARALLEL,
		">=256 snapshots execute through the real job system");
	expect(outputCount == SNAPSHOT_COUNT && metrics.submittedJobs >= 2 &&
		metrics.submittedJobs == metrics.completedJobs &&
		metrics.physicalWorkerJobs == metrics.completedJobs &&
		metrics.ownerHelpedJobs == 0 && metrics.physicalWorkerMask != 0 &&
		metrics.distinctPhysicalWorkers != 0 &&
		metrics.peakConcurrentPhysicalWorkers != 0 &&
		metrics.serialFallbacks == 0,
		"parallel metrics report a complete fenced wave");
	unsigned firstDifference = 99;
	expect(rts::ObjectStatusTimerCommandsEqual(serialOutput, serialCount,
		output, outputCount, &firstDifference),
		"real parallel output exactly matches the serial command oracle");
	expect(output[0].ownerOrder == 0 &&
		output[SNAPSHOT_COUNT - 1].ownerOrder == SNAPSHOT_COUNT - 1,
		"parallel merge reproduces legacy owner traversal order");

#if defined(RTS_BUILD_CORE_EXTRAS)
	const rts::ObjectStatusTimerCommand retainedFirst = output[0];
	rts_job_system_set_test_fault(6, 1);
	outputCount = 73;
	result = rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
		100, 13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics);
	rts_job_system_set_test_fault(0, 0);
	expect(result == rts::OBJECT_STATUS_TIMER_SERIAL_FALLBACK,
		"partial admission failure fails closed to the owner legacy path");
	expect(outputCount == 73 && metrics.serialFallbacks == 1,
		"failed worker wave publishes nothing and records one fallback");
	expect(output[0].objectID == retainedFirst.objectID &&
		output[0].ownerOrder == retainedFirst.ownerOrder &&
		output[0].expiredMask == retainedFirst.expiredMask,
		"failed worker wave leaves existing output storage untouched");

	const unsigned groupAssignmentAllocationFault = 11;
	outputCount = 74;
	bool groupAssignmentExceptionEscaped = false;
	rts_job_system_set_test_fault(groupAssignmentAllocationFault, 1);
	try
	{
		result = rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
			100, 13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics);
	}
	catch (...)
	{
		groupAssignmentExceptionEscaped = true;
	}
	rts_job_system_set_test_fault(0, 0);
	expect(!groupAssignmentExceptionEscaped,
		"group assignment allocation failure is contained by the status kernel");
	expect(result == rts::OBJECT_STATUS_TIMER_SERIAL_FALLBACK,
		"group assignment allocation failure selects deterministic serial fallback");
	expect(outputCount == 74 && metrics.serialFallbacks == 1,
		"group assignment allocation failure publishes nothing and records one fallback");
	expect(output[0].objectID == retainedFirst.objectID &&
		output[0].ownerOrder == retainedFirst.ownerOrder &&
		output[0].expiredMask == retainedFirst.expiredMask,
		"group assignment allocation failure leaves existing output storage untouched");
#endif

	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"simulation owner unregisters cleanly");
	jobs.shutdown();
}
#endif

void testRuntimeAuthorityRequiresPhysicalWorkers()
{
	const rts::ObjectStatusTimerRuntimeMetrics before =
		rts::GetObjectStatusTimerRuntimeMetrics();
	rts::ResetObjectStatusTimerRuntimeMetrics();
	rts::ObjectStatusTimerMetrics metrics;
	metrics.submittedJobs = 4;
	metrics.completedJobs = 4;
	metrics.physicalWorkerJobs = 4;
	metrics.physicalWorkerMask = 0xf;
	metrics.distinctPhysicalWorkers = 4;
	metrics.peakConcurrentPhysicalWorkers = 3;
	rts::RecordObjectStatusTimerAuthoritativeCommit(8, 8, metrics);
	rts::ObjectStatusTimerRuntimeMetrics runtime =
		rts::GetObjectStatusTimerRuntimeMetrics();
	expect(runtime.resetEpoch == before.resetEpoch + 1 &&
		runtime.authoritativeBatches == 1 && runtime.committedCommands == 8 &&
		runtime.submittedJobs == 4 && runtime.completedJobs == 4 &&
		runtime.physicalWorkerJobs == 4 && runtime.ownerHelpedJobs == 0 &&
		runtime.physicalWorkerMask == 0xf &&
		runtime.maximumDistinctPhysicalWorkers == 4 &&
		runtime.maximumPeakConcurrentPhysicalWorkers == 3,
		"qualifying physical status work records live authority");

	metrics.physicalWorkerJobs = 3;
	metrics.ownerHelpedJobs = 1;
	rts::RecordObjectStatusTimerAuthoritativeCommit(8, 8, metrics);
	rts::RecordObjectStatusTimerAuthoritativeCommit(8, 7, metrics);
	runtime = rts::GetObjectStatusTimerRuntimeMetrics();
	expect(runtime.authoritativeBatches == 1 && runtime.committedCommands == 8 &&
		runtime.ownerFallbacks == 2 && runtime.staleRejections == 1,
		"owner-help and stale status commits cannot certify live authority");

	rts::RecordObjectStatusTimerShadow(true, 8, metrics);
	rts::RecordObjectStatusTimerShadow(false, 8, metrics);
	runtime = rts::GetObjectStatusTimerRuntimeMetrics();
	expect(runtime.shadowExecutions == 2 && runtime.shadowCommands == 16 &&
		runtime.shadowMatches == 1 && runtime.shadowMismatches == 1,
		"status shadow evidence remains separate from live authority");
	rts::ResetObjectStatusTimerRuntimeMetrics();
	rts::ObjectStatusTimerMetrics highCoreMetrics;
	highCoreMetrics.submittedJobs = 65;
	highCoreMetrics.completedJobs = 65;
	highCoreMetrics.physicalWorkerJobs = 65;
	highCoreMetrics.physicalWorkerMask = ~static_cast<
		rts::ObjectStatusTimerMetricCounter>(0);
	highCoreMetrics.distinctPhysicalWorkers = 65;
	highCoreMetrics.physicalWorkerMaskComplete = false;
	highCoreMetrics.peakConcurrentPhysicalWorkers = 65;
	rts::RecordObjectStatusTimerAuthoritativeCommit(65, 65,
		highCoreMetrics);
	runtime = rts::GetObjectStatusTimerRuntimeMetrics();
	expect(runtime.authoritativeBatches == 1 &&
		runtime.maximumDistinctPhysicalWorkers == 65 &&
		!runtime.physicalWorkerMaskComplete,
		"status runtime retains exact identities beyond its diagnostic mask width");
	rts::ResetObjectStatusTimerRuntimeMetrics();
	runtime = rts::GetObjectStatusTimerRuntimeMetrics();
	expect(runtime.authoritativeBatches == 0 && runtime.committedCommands == 0 &&
		runtime.physicalWorkerMask == 0 && runtime.physicalWorkerMaskComplete &&
		runtime.shadowExecutions == 0,
		"status runtime reset clears prior-match authority");
}

#if defined(_WIN64)
struct KernelPerformanceClock
{
	KernelPerformanceClock() : now(1000) {}
	rts::JobMetricCounter now;
	static rts::JobMetricCounter read(void *context)
	{
		KernelPerformanceClock &clock = *static_cast<KernelPerformanceClock *>(context);
		clock.now += 10;
		return clock.now;
	}
};

void testKernelPerformanceTokenReachesStatusStages()
{
	enum { SNAPSHOT_COUNT = 300 };
	rts::ObjectStatusTimerSnapshot snapshots[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerCommand output[SNAPSHOT_COUNT];
	fillParallelSnapshots(snapshots, SNAPSHOT_COUNT);

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "job system starts for kernel diagnostics");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"diagnostic test thread registers as simulation owner");

	KernelPerformanceClock clock;
	rts::performance::KernelPerformanceLedger &ledger =
		rts::performance::KernelPerformanceLedger::instance();
	expect(ledger.beginRun(true, KernelPerformanceClock::read, &clock),
		"status diagnostics run starts");
	const rts::performance::KernelPerformanceBatch token = ledger.beginBatch(
		rts::performance::KERNEL_PERFORMANCE_STATUS, 0, 100, 1);
	expect(token.valid(), "status diagnostics batch starts");
	{
		rts::performance::KernelPerformanceScope capture(&ledger, token,
			rts::performance::KERNEL_PERFORMANCE_CAPTURE);
		clock.now += 10;
	}

	rts::ObjectStatusTimerOptions options;
	options.parallel = true;
	options.minimumGrain = 1;
	options.performanceBatch = token;
	rts::ObjectStatusTimerMetrics metrics;
	unsigned outputCount = 0;
	expect(rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
		100, 13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics) ==
		rts::OBJECT_STATUS_TIMER_PARALLEL,
		"status diagnostics token follows the real parallel path");
	{
		rts::performance::KernelPerformanceScope commit(&ledger, token,
			rts::performance::KERNEL_PERFORMANCE_COMMIT);
		clock.now += 10;
	}
	expect(ledger.endBatch(token,
		rts::performance::KERNEL_PERFORMANCE_COMMITTED),
		"status diagnostics batch commits after all stages");
	const rts::performance::KernelPerformanceSnapshot snapshot = ledger.freeze();
	expect(snapshot.complete && snapshot.streamCount == 1,
		"status diagnostics freezes one complete stream");
	if (snapshot.streamCount == 1)
	{
		const rts::performance::KernelPerformanceStream &stream = snapshot.streams[0];
		expect(stream.kernel == rts::performance::KERNEL_PERFORMANCE_STATUS &&
			stream.subtype == 0 && stream.attemptedBatches == 1 &&
			stream.admittedBatches == 1 && stream.committedBatches == 1,
			"status diagnostics preserves batch identity and disposition");
		for (unsigned stage = 0;
			stage != rts::performance::KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
			expect(stream.stageSamples[stage] >= 1 &&
				stream.stageNanoseconds[stage] > 0,
				"status diagnostics records every measured stage");
	}

	jobs.shutdown();
	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"diagnostic test thread unregisters cleanly");
}

void testKernelPerformanceReferenceTransportReachesStatusParallelPath()
{
	enum { SNAPSHOT_COUNT = 300 };
	rts::ObjectStatusTimerSnapshot snapshots[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerCommand output[SNAPSHOT_COUNT];
	fillParallelSnapshots(snapshots, SNAPSHOT_COUNT);

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "reference status job system starts");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"reference status test registers simulation owner");

	KernelPerformanceClock timingClock;
	rts::performance::KernelPerformanceLedger &timingLedger =
		rts::performance::KernelPerformanceLedger::instance();
	expect(timingLedger.beginRun(true, KernelPerformanceClock::read,
		&timingClock), "reference status timing run starts");
	const rts::performance::KernelPerformanceBatch timingBatch =
		timingLedger.beginBatch(rts::performance::KERNEL_PERFORMANCE_STATUS,
		0, 100, 1);
	expect(timingBatch.valid(), "reference status timing batch starts");
	{
		rts::performance::KernelPerformanceScope capture(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_CAPTURE);
		timingClock.now += 10;
	}

	KernelPerformanceClock referenceClock;
	rts::performance::KernelPerformanceReferenceLedger referenceLedger;
	expect(referenceLedger.beginRun(
		rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING,
		KernelPerformanceClock::read, &referenceClock),
		"reference status throughput run starts");
	rts::performance::KernelPerformanceReferenceBatch referenceBatch;
	rts::ObjectStatusTimerOptions options;
	options.parallel = true;
	options.minimumGrain = 1;
	options.performanceBatch = timingBatch;
	options.performanceReferenceLedger = &referenceLedger;
	options.performanceReferenceBatch = &referenceBatch;
	rts::ObjectStatusTimerMetrics metrics;
	unsigned outputCount = 0;
	expect(rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
		100, 13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics) ==
		rts::OBJECT_STATUS_TIMER_PARALLEL,
		"reference status throughput follows the real parallel path");
	{
		rts::performance::KernelPerformanceScope commit(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_COMMIT);
		timingClock.now += 10;
	}
	expect(timingLedger.endBatch(timingBatch,
		rts::performance::KERNEL_PERFORMANCE_COMMITTED),
		"reference status timing batch commits");
	expect(referenceBatch.valid(),
		"reference status throughput observes one validated batch");
	if (referenceBatch.valid())
		expect(referenceLedger.finishBatch(referenceBatch, true),
			"reference status throughput commit closes reference batch");
	const rts::performance::KernelPerformanceReferenceSnapshot reference =
		referenceLedger.freeze();
	expect(reference.complete && reference.streamCount == 1,
		"reference status throughput freezes complete evidence");
	if (reference.streamCount == 1)
	{
		const rts::performance::KernelPerformanceReferenceStream &stream =
			reference.streams[0];
		expect(stream.kernel == rts::performance::KERNEL_PERFORMANCE_STATUS &&
			stream.subtype == 0 && stream.validatedBatchCount == 1 &&
			stream.committedBatchCount == 1 &&
			stream.validatedOperationCount == SNAPSHOT_COUNT &&
			stream.committedOperationCount == SNAPSHOT_COUNT &&
			stream.serialSampleCount == 0 && stream.serialNanoseconds == 0,
			"reference status throughput keeps batch and operation cardinality separate");
	}
	expect(referenceClock.now == 1000,
		"reference status throughput invokes no serial clock");
	expect(timingLedger.freeze().complete,
		"reference status timing evidence remains complete");
	jobs.shutdown();
	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"reference status throughput unregisters simulation owner");
}

void testKernelPerformanceReferenceSerialStatusUsesDetachedOutput()
{
	enum { SNAPSHOT_COUNT = 300 };
	rts::ObjectStatusTimerSnapshot snapshots[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerCommand output[SNAPSHOT_COUNT];
	rts::ObjectStatusTimerCommand referenceOutput[SNAPSHOT_COUNT];
	fillParallelSnapshots(snapshots, SNAPSHOT_COUNT);
	for (unsigned index = 0; index != SNAPSHOT_COUNT; ++index)
	{
		referenceOutput[index].objectID = 0xdead0000u + index;
		referenceOutput[index].ownerOrder = 0xbeef0000u + index;
		referenceOutput[index].expiredMask = 0xa5a50000u + index;
	}

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "serial reference status job system starts");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"serial reference status registers simulation owner");

	KernelPerformanceClock timingClock;
	rts::performance::KernelPerformanceLedger &timingLedger =
		rts::performance::KernelPerformanceLedger::instance();
	expect(timingLedger.beginRun(true, KernelPerformanceClock::read,
		&timingClock), "serial reference status timing run starts");
	const rts::performance::KernelPerformanceBatch timingBatch =
		timingLedger.beginBatch(rts::performance::KERNEL_PERFORMANCE_STATUS,
		0, 100, 1);
	expect(timingBatch.valid(), "serial reference status timing batch starts");
	{
		rts::performance::KernelPerformanceScope capture(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_CAPTURE);
		timingClock.now += 10;
	}

	KernelPerformanceClock referenceClock;
	rts::performance::KernelPerformanceReferenceLedger referenceLedger;
	expect(referenceLedger.beginRun(
		rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE,
		KernelPerformanceClock::read, &referenceClock),
		"serial reference status oracle starts");
	rts::performance::KernelPerformanceReferenceBatch referenceBatch;
	rts::ObjectStatusTimerOptions options;
	options.parallel = true;
	options.minimumGrain = 1;
	options.performanceBatch = timingBatch;
	options.performanceReferenceLedger = &referenceLedger;
	options.performanceReferenceBatch = &referenceBatch;
	options.performanceReferenceOutput = referenceOutput;
	options.performanceReferenceOutputCapacity = SNAPSHOT_COUNT;
	rts::ObjectStatusTimerMetrics metrics;
	unsigned outputCount = 0;
	expect(rts::PrepareObjectStatusTimerCommands(snapshots, SNAPSHOT_COUNT,
		100, 13, output, SNAPSHOT_COUNT, options, &outputCount, &metrics) ==
		rts::OBJECT_STATUS_TIMER_PARALLEL,
		"serial reference status oracle follows the real parallel path");
	expect(outputCount == SNAPSHOT_COUNT,
		"serial reference status fixture emits every command");
	unsigned firstDifference = 0;
	expect(rts::ObjectStatusTimerCommandsEqual(referenceOutput, outputCount,
		output, outputCount, &firstDifference),
		"serial reference status output is detached and ordered like production");
	{
		rts::performance::KernelPerformanceScope commit(&timingLedger,
			timingBatch, rts::performance::KERNEL_PERFORMANCE_COMMIT);
		timingClock.now += 10;
	}
	expect(timingLedger.endBatch(timingBatch,
		rts::performance::KERNEL_PERFORMANCE_COMMITTED),
		"serial reference status timing batch commits");
	expect(referenceBatch.valid(),
		"serial reference status observes one validated batch");
	if (referenceBatch.valid())
		expect(referenceLedger.finishBatch(referenceBatch, true),
			"serial reference status commit closes reference batch");
	const rts::performance::KernelPerformanceReferenceSnapshot reference =
		referenceLedger.freeze();
	expect(reference.complete && reference.streamCount == 1,
		"serial reference status freezes complete evidence");
	if (reference.streamCount == 1)
	{
		const rts::performance::KernelPerformanceReferenceStream &stream =
			reference.streams[0];
		expect(stream.kernel == rts::performance::KERNEL_PERFORMANCE_STATUS &&
			stream.subtype == 0 && stream.validatedBatchCount == 1 &&
			stream.committedBatchCount == 1 &&
			stream.validatedOperationCount == SNAPSHOT_COUNT &&
			stream.committedOperationCount == SNAPSHOT_COUNT &&
			stream.serialSampleCount == 1 && stream.serialNanoseconds != 0,
			"serial reference status keeps measured detached work separate");
	}
	expect(referenceClock.now == 1020,
		"serial reference status measures only detached serial work");
	expect(timingLedger.freeze().complete,
		"serial reference status timing evidence remains complete");
	jobs.shutdown();
	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"serial reference status unregisters simulation owner");
}

// These tests enter the real public native kernel. They do not implement a
// dispatcher, range body, checkpoint probe, canonical serializer or oracle.
struct ActualNativeStatusObservations
{
	std::atomic<unsigned> entries[2]{}, finishes[2]{}, items[384]{}, polls[2][4]{};
	std::atomic<unsigned> units[2]{}, completed[2]{}, releases[2]{};
	std::atomic<unsigned> validations{0}, reductions{0}, publications{0};
	std::atomic<bool> wrongIdentity{false};
	std::atomic<unsigned> held{0}, truePredicates[2]{};
	std::atomic<bool> releaseHeld{false}, waitExpired{false};
	unsigned waitNotifications = 0, cancelNotifications = 0, releaseNotifications = 0;
	unsigned releasedCompleted = 0, releasedSubmitted = 0, releasedReason = 0;
	bool releasedCancelled = false;
	std::atomic<unsigned> appends[2]{};
	rts_test::NativeKernelClock *clock = 0;
	bool baseline = false;
	unsigned variant = 0;
	~ActualNativeStatusObservations() { releaseHeld.store(true, std::memory_order_release); }

	static void beforeWait(void *opaque)
	{
		auto &self = *static_cast<ActualNativeStatusObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongIdentity = true;
		++self.waitNotifications;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
		while (self.held.load(std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (self.held.load(std::memory_order_acquire) != 2)
		{ self.waitExpired = true; self.releaseHeld.store(true, std::memory_order_release); }
	}
	static void afterCancel(void *opaque)
	{
		auto &self = *static_cast<ActualNativeStatusObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME)) self.wrongIdentity = true;
		++self.cancelNotifications;
		self.releaseHeld.store(true, std::memory_order_release);
	}
	static void releasedGroup(void *opaque, bool cancelled, unsigned completedBodies, unsigned submitted, unsigned reason)
	{
		auto &self = *static_cast<ActualNativeStatusObservations *>(opaque);
		if (self.baseline || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) ||
			rts::JobSystem::instance().outstandingJobCount() != 0 ||
			rts::JobSystem::instance().pendingOwnerCompletionCount() != 0) self.wrongIdentity = true;
		++self.releaseNotifications; self.releasedCancelled = cancelled;
		self.releasedCompleted = completedBodies; self.releasedSubmitted = submitted; self.releasedReason = reason;
	}

	static void observe(void *opaque, rts::ObjectStatusTimerTestEvent event,
		unsigned rangeIndex, unsigned begin, unsigned end, unsigned workUnits,
		bool complete, rts::SimulationCommand *mutableStorage)
	{
		auto &self = *static_cast<ActualNativeStatusObservations *>(opaque);
		const bool body = event <= rts::OBJECT_STATUS_TIMER_TEST_RANGE_FINISHED;
		if (rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) !=
			(body ? self.baseline : true)) self.wrongIdentity = true;
		if (body || event == rts::OBJECT_STATUS_TIMER_TEST_RANGE_RELEASED)
		{
			if (rangeIndex >= 2 || begin != rangeIndex * 192 || end != begin + 192)
			{ self.wrongIdentity = true; return; }
		}
		if (event == rts::OBJECT_STATUS_TIMER_TEST_RANGE_ENTERED)
		{
			++self.entries[rangeIndex];
			if (!self.baseline)
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
				while ((self.entries[0] == 0 || self.entries[1] == 0) && std::chrono::steady_clock::now() < deadline)
					std::this_thread::yield();
				if (self.entries[0] != 1 || self.entries[1] != 1) self.wrongIdentity = true;
			}
		}
		else if (event == rts::OBJECT_STATUS_TIMER_TEST_ITEM_EVALUATED)
		{
			if (workUnits >= 192) self.wrongIdentity = true;
			else ++self.items[begin + workUnits];
			++self.clock->now;
		}
		else if (event == rts::OBJECT_STATUS_TIMER_TEST_COMMAND_APPENDED) { ++self.appends[rangeIndex]; ++self.clock->now; }
		else if (event == rts::OBJECT_STATUS_TIMER_TEST_RANGE_FINISHED)
		{
			++self.finishes[rangeIndex]; self.units[rangeIndex] = workUnits;
			self.completed[rangeIndex] = complete ? 1 : 0;
			if (self.variant == 5 && !self.baseline)
			{
				if (!complete || workUnits != 192) self.wrongIdentity = true;
				self.held.fetch_add(1, std::memory_order_release);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
				while (!self.releaseHeld.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
					std::this_thread::yield();
				if (!self.releaseHeld.load(std::memory_order_acquire)) self.waitExpired = true;
			}
		}
		else if (event == rts::OBJECT_STATUS_TIMER_TEST_RANGE_RELEASED)
		{
			++self.releases[rangeIndex];
			const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
			if (scheduler.pendingJobs != 0 || scheduler.outstandingJobs != 0) self.wrongIdentity = true;
			self.clock->now.fetch_add(13);
		}
		else
		{
			if (event == rts::OBJECT_STATUS_TIMER_TEST_OWNER_REDUCTION)
			{
				++self.reductions;
				if (self.variant == 4) { if (mutableStorage == 0) self.wrongIdentity = true; else mutableStorage[0] = rts::SimulationCommand(); }
			}
			else if (event == rts::OBJECT_STATUS_TIMER_TEST_PUBLICATION) ++self.publications;
			else self.wrongIdentity = true;
			self.clock->now.fetch_add(17);
		}
	}

	static bool checkpoint(void *opaque, unsigned rangeIndex,
		rts::ObjectStatusTimerTestCheckpoint site, unsigned workUnits, bool actual)
	{
		auto &self = *static_cast<ActualNativeStatusObservations *>(opaque);
		if (rangeIndex >= 2 || static_cast<unsigned>(site) >= 4 ||
			rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) != self.baseline)
		{ self.wrongIdentity = true; return true; }
		++self.polls[rangeIndex][static_cast<unsigned>(site)];
		if ((site == rts::OBJECT_STATUS_TIMER_TEST_CHECKPOINT_ENTRY && workUnits != 0) ||
			(site == rts::OBJECT_STATUS_TIMER_TEST_CHECKPOINT_BLOCK && (workUnits % 64 != 0 || workUnits >= 192)) ||
			(site >= rts::OBJECT_STATUS_TIMER_TEST_CHECKPOINT_POST_BODY && workUnits != 192))
			self.wrongIdentity = true;
		const bool cut = rangeIndex == 0 && (
			(self.variant == 1 && site == rts::OBJECT_STATUS_TIMER_TEST_CHECKPOINT_ENTRY) ||
			(self.variant == 2 && site == rts::OBJECT_STATUS_TIMER_TEST_CHECKPOINT_BLOCK && workUnits == 128) ||
			(self.variant == 3 && site == rts::OBJECT_STATUS_TIMER_TEST_CHECKPOINT_POST_BODY));
		// A current deadline is deliberately the opposite of the source cut.
		// Only the authenticated native replay probe may select baseline work.
		if (!self.baseline && (actual || cut)) ++self.truePredicates[rangeIndex];
		return self.baseline ? !cut : actual || cut;
	}
};

bool runActualNativeStatusRole(rts_test::NativeKernelTrace &trace, bool baseline, unsigned variant)
{
	using namespace rts::performance;
	printf("B_NATIVE Status BEGIN role=%s variant=%u\n",
		baseline ? "consumer" : "source", variant);
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = baseline ? 1 : 2; config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096; config.pinWorkers = false;
	if (!jobs.start(config) || !jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{ expect(false, "Status source fixture starts its declared native worker policy"); return false; }
	rts_test::NativeKernelOwnerRun run;
	const bool started = run.begin(trace, baseline, 100, KERNEL_PHASE_OWNER_TAIL);
	expect(started, "Status owner validates real source artifact binding before native entry");
	if (!started) { jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false; }
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_STATUS, 0);
	expect(attempt.valid(), "Status owner opens the authentic attempt before native capture");
	auto timingBatch = run.timing.beginBatch(KERNEL_PERFORMANCE_STATUS, 0, 100, 1);
	const auto capture = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_CAPTURE);
	std::vector<rts::ObjectStatusTimerSnapshot> input(384);
	std::vector<rts::ObjectStatusTimerCommand> output(384), untouched(384), detached(384);
	fillParallelSnapshots(input.data(), 384);
	if (variant == 6)
		for (unsigned index = 0; index != 384; ++index)
			input[index].activeMask = 0;
	memset(output.data(), 0xcd, output.size() * sizeof(output[0])); untouched = output; detached = output;
	unsigned outputCount = 0xdeadbeefu;
	run.clock.now.fetch_add(5);
	expect(run.timing.endInterval(capture), "Status immutable owner capture closes before native work");
	KernelPerformanceReferenceBatch validated;
	ActualNativeStatusObservations observed;
	observed.clock = &run.clock; observed.baseline = baseline; observed.variant = variant;
	rts::ObjectStatusTimerTestHooks hooks;
	hooks.context = &observed; hooks.observe = ActualNativeStatusObservations::observe;
	hooks.checkpoint = ActualNativeStatusObservations::checkpoint;
	hooks.physicalWaitMilliseconds = 1000;
	if (variant == 5)
	{
		hooks.beforeWait = ActualNativeStatusObservations::beforeWait;
		hooks.afterCancel = ActualNativeStatusObservations::afterCancel;
		hooks.releasedGroup = ActualNativeStatusObservations::releasedGroup;
		hooks.physicalWaitMilliseconds = 1;
	}
	rts::ObjectStatusTimerOptions options; options.parallel = true;
	options.minimumGrain = 192; options.testHooks = &hooks;
	options.performanceBatch = timingBatch; options.performanceReferenceLedger = &run.reference;
	options.performanceReferenceAttempt = attempt; options.performanceReferenceBatch = &validated;
	options.performanceReferenceOutput = detached.data(); options.performanceReferenceOutputCapacity = 384;
	rts::ObjectStatusTimerMetrics metrics;
	const auto result = rts::PrepareObjectStatusTimerCommands(input.data(), 384, 100, 13,
		output.data(), 384, options, &outputCount, &metrics);
	const bool accepted = result == rts::OBJECT_STATUS_TIMER_PARALLEL;
	expect(accepted == (variant == 0 || variant == 6),
		"Status native outcome matches the predeclared source case without retries");
	if (variant == 5)
		expect(result == rts::OBJECT_STATUS_TIMER_SERIAL_FALLBACK,
			"Status late real group cancellation retains the exact native serial fallback result");
	if (accepted)
	{
		const unsigned expectedCommands = variant == 6 ? 0U : 384U;
		expect(outputCount == expectedCommands && metrics.evaluatedSnapshots == 384 &&
			metrics.emittedCommands == expectedCommands,
			"actual native status evaluates the full roster and preserves a valid empty command set");
		if (variant == 6)
			expect(memcmp(output.data(), untouched.data(), output.size() * sizeof(output[0])) == 0,
				"zero-command authoritative status leaves every unused output byte unchanged");
		else
			for (unsigned index = 0; index != 384; ++index)
				expect(output[index].objectID == 1383 - index && output[index].ownerOrder == index &&
					output[index].expiredMask == (1u << ((383 - index) % 13)),
					"actual native status owner merge retains literal reverse-input order and exact expired mask");
	}
	else expect(outputCount == 0xdeadbeefu && memcmp(output.data(), untouched.data(), output.size() * sizeof(output[0])) == 0,
		"actual native status abort preserves count and every output byte");
	expect(memcmp(detached.data(), untouched.data(), detached.size() * sizeof(detached[0])) == 0,
		"Status source and baseline do not execute detached serial-reference storage");
	const unsigned targetUnits = variant == 1 ? 0 : variant == 2 ? 128 : 192;
	for (unsigned range = 0; range != 2; ++range)
	{
		const unsigned units = range == 0 ? targetUnits : 192;
		expect(observed.entries[range] == 1 && observed.finishes[range] == 1 && observed.releases[range] == 1 &&
			observed.units[range] == units && observed.completed[range] == ((range == 0 && variant >= 1 && variant <= 3) ? 0U : 1U),
			"Status real admitted range enters once, retains its exact terminal prefix and releases after drain");
		for (unsigned index = 0; index != 192; ++index)
			expect(observed.items[range * 192 + index] == (index < units ? 1U : 0U),
				"Status actual compiled item helper executes precisely the recorded prefix once");
		expect(observed.polls[range][0] == 1 &&
			observed.polls[range][1] == ((range == 0 && variant == 1) ? 0U : 3U) &&
			observed.polls[range][2] == ((range == 0 && (variant == 1 || variant == 2)) ? 0U : 1U),
			"Status exact native entry, 64-item and post-body checkpoint sites are reached");
		expect(observed.appends[range] == (variant == 6 ? 0U : units),
			"actual native status appends only commands emitted by evaluated fixture items");
	}
	expect(!observed.wrongIdentity && observed.publications ==
		((variant == 0 || variant == 6) ? 1U : 0U) &&
		observed.reductions == ((variant == 0 || variant == 4 || variant == 6) ? 1U : 0U),
		"Status owner-only validation/reduction rejects finished-discarded bodies before publication");
	if (variant == 5 && !baseline)
	{
		expect(!observed.waitExpired && observed.held == 2 && observed.waitNotifications == 1 &&
			observed.cancelNotifications == 1 && observed.releaseNotifications == 1 && observed.releasedCancelled &&
			observed.releasedCompleted == 2 && observed.releasedSubmitted == 2 &&
			observed.truePredicates[0] == 0 && observed.truePredicates[1] == 0 && metrics.completedJobs == 2,
			"Status real owner timeout cancels and drains two completed bodies without inventing a true body poll");
		expect(observed.releasedReason == 3,
			"Status source reason records actual late group cancellation independently of completed checkpoints");
	}
	if (variant == 5 && baseline)
		expect(!observed.waitExpired && observed.held == 0 && observed.waitNotifications == 0 &&
			observed.cancelNotifications == 0 && observed.releaseNotifications == 0,
			"Status baseline replays late disposal without a physical wait, cancellation or source release callback");
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	expect(scheduler.pendingJobs == 0 && scheduler.outstandingJobs == 0 && scheduler.ownerHelpJobs == 0,
		"Status native return follows actual release/acquire and scheduler drain");
	expect(metrics.referenceAdmissionAccepted,
		"Status metrics retain authenticated admission for physical and baseline-inline execution");
	if (baseline)
		expect(scheduler.submittedJobs == 0 && scheduler.executedJobs == 0 && metrics.submittedJobs == 0 &&
			metrics.physicalWorkerJobs == 0 && metrics.ownerHelpedJobs == 0 && metrics.physicalWorkerMask == 0 &&
			metrics.distinctPhysicalWorkers == 0 && metrics.peakConcurrentPhysicalWorkers == 0,
			"Status source-shaped baseline bodies manufacture no worker or owner-help authority");
	else
		expect(scheduler.submittedJobs == 2 && scheduler.executedJobs == 2 && metrics.submittedJobs == 2,
			"Status source dispatch is the two real admitted native jobs");
	expect(validated.valid() == accepted, "Status kernel links only actually published output to its attempt");
	if (validated.valid()) expect(run.reference.finishBatch(validated, accepted),
		"Status owner closes the actual validated batch before attempt finish");
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = accepted ? KERNEL_PERFORMANCE_COMMITTED : KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1; finish.reason = accepted ? 1 : 2; finish.validatedBatch = validated;
	expect(run.reference.finishAttempt(attempt, finish), "Status authentic native attempt closes its actual outcome");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	reap.pendingJobs = scheduler.pendingJobs; reap.outstandingJobs = scheduler.outstandingJobs;
	expect(run.reference.reapAttempt(attempt, reap), "Status native owner reaps only after real storage release and drain");
	if (accepted)
	{
		const auto commit = run.timing.beginInterval(timingBatch, KERNEL_PERFORMANCE_COMMIT);
		run.clock.now.fetch_add(11); run.timing.endInterval(commit);
	}
	expect(run.timing.endBatch(timingBatch, finish.disposition), "Status timing records actual native outcome");
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.closeTiming(scheduler);
	expect(timingClosed, "Status actual scheduler and owner phase timing reconcile");
	if (baseline && timingClosed)
	{
		const auto &phase = run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_OWNER_TAIL];
		const unsigned expectedPure = variant == 0 ? 768U :
			variant == 6 ? 384U : 0U;
		expect(phase.pureNanoseconds == expectedPure &&
			phase.serialNanoseconds >= 31,
			"Status only committed actual native bodies are pure; reduction and all discarded work stay serial");
	}
	const bool canonicalState = (variant == 0 || variant == 6) ?
		snapshot.complete && snapshot.streamCount == 1 :
		!snapshot.complete && snapshot.streamCount == 0;
	const bool sourceComplete = sealed && canonicalState && snapshot.errors == 0 && snapshot.trace.complete &&
		snapshot.trace.attemptCount == 1 && snapshot.trace.admittedAttemptCount == 1 &&
		snapshot.trace.capturedAttemptCount == 1 && snapshot.trace.capturedOperationCount == 384 &&
		snapshot.trace.dispatchCount == 1 && snapshot.trace.rangeCount == 2 &&
		snapshot.trace.releasedRangeCount == 2 && snapshot.trace.reapCount == 1;
	expect(sourceComplete, "Status actual native entry supplies capture, dispatch, exact released bodies and attempt closure");
	if (!baseline) trace.source = snapshot;
	else if (sourceComplete && accepted)
		expect(snapshot.streams[0].inputDigest.equals(trace.source.streams[0].inputDigest) &&
			snapshot.streams[0].outputDigest.equals(trace.source.streams[0].outputDigest) &&
			snapshot.streams[0].commitDigest.equals(trace.source.streams[0].commitDigest),
			"Status once-only native consumer binds every canonical input/output/commit byte to source");
	printf("B_NATIVE Status END role=%s variant=%u source_closure=%u\n",
		baseline ? "consumer" : "source", variant, static_cast<unsigned>(sourceComplete));
	jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	return sourceComplete && (variant != 5 || baseline || observed.releasedReason == 3);
}

void testActualNativeStatusSourceConsumer()
{
	// Breaks caught: missing native integration, copied/detached executor,
	// recomputed baseline range shape, changed polling, partial publication,
	// premature release, or treating completed-discarded bodies as pure.
	// Variant five catches a real owner cancellation after every body poll and
	// body completion, while the native jobs are still awaiting retirement.
	for (unsigned variant = 0; variant != 7; ++variant)
	{
		rts_test::NativeKernelTrace trace(70 + variant);
		// A real source failure is not repaired by synthesizing trace records.
		// All source buffers leave scope before the authenticated consumer call.
		if (runActualNativeStatusRole(trace, false, variant))
			runActualNativeStatusRole(trace, true, variant);
	}
}
#endif
}

int main()
{
#if defined(_MSC_VER)
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	testLiveAdapterPreflightSkipsForcedSerialPreparation();
	testSerialTimerDecisionsAndLegacyOrder();
	testNoSchedulerIsQuietSerialDecision();
	testTransactionalFailureAndShadowComparison();
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	testRealJobSystemPathAndFailure();
#endif
	testRuntimeAuthorityRequiresPhysicalWorkers();
#if defined(_WIN64)
	testKernelPerformanceTokenReachesStatusStages();
	testKernelPerformanceReferenceTransportReachesStatusParallelPath();
	testKernelPerformanceReferenceSerialStatusUsesDetachedOutput();
	testActualNativeStatusSourceConsumer();
#endif
	if (failures != 0)
		return 1;
	printf("Object status timer kernel tests passed.\n");
	return 0;
}
