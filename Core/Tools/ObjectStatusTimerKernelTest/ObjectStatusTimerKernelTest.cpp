/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectStatusTimerKernel.h"

#include <limits.h>
#include <stdio.h>

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
	if (failures != 0)
		return 1;
	printf("Object status timer kernel tests passed.\n");
	return 0;
}
