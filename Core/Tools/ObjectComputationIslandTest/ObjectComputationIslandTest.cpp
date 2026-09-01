/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/JobSystem.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/ObjectComputationIsland.h"

#include <cmath>
#include <stdio.h>

#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
#include <xmmintrin.h>
#endif

namespace
{
int failures = 0;

void expect(bool condition, const char *message)
{
	if (condition) return;
	printf("FAIL: %s\n", message);
	++failures;
}

bool noCaptureOrJobWork(const rts::ObjectComputationMetrics &metrics)
{
	return metrics.objectCount == 0 && metrics.moduleCount == 0 &&
		metrics.pairCount == 0 && metrics.rangeCount == 0 &&
		metrics.submittedJobs == 0 && metrics.completedJobs == 0 &&
		metrics.schedulerReleasedJobs == 0 &&
		metrics.physicalWorkerJobs == 0 &&
		metrics.distinctPhysicalWorkers == 0 &&
		metrics.ownerHelpedJobs == 0 && metrics.physicalWaitTimeouts == 0 &&
		metrics.emittedCommands == 0 && metrics.emittedCandidates == 0 &&
		metrics.visitedSpatialMembers == 0 &&
		metrics.candidatePayloadBytes == 0 &&
		metrics.spatialCellSpans == 0 && metrics.spatialMemberships == 0 &&
		metrics.allocatedBytes == 0 && metrics.arenaAllocations == 0;
}

void fillFixture(rts::SimulationReadObjectRecord *objects,
	rts::SimulationReadModuleRecord *modules,
	rts::SimulationReadScheduleEntry *schedule, unsigned objectCount,
	unsigned moduleCount)
{
	for (unsigned index = 0; index < objectCount; ++index)
	{
		objects[index].objectID = 1000 + index;
		objects[index].objectGeneration = 1 + index;
		objects[index].positionX = static_cast<float>(index % 32) * 4.0f;
		objects[index].positionY = static_cast<float>(index / 32) * 4.0f;
		objects[index].positionZ = static_cast<float>(index % 3);
		objects[index].boundingSphereRadius = 1.25f;
		objects[index].zCenterOffset = 1.0f;
	}
	for (unsigned moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex)
	{
		modules[moduleIndex].objectID = objects[moduleIndex * 3].objectID;
		modules[moduleIndex].moduleOrdinal = 7;
		modules[moduleIndex].moduleType =
			rts::OBJECT_COMPUTATION_MODULE_HORDE;
		modules[moduleIndex].wakePriority = 402;
		modules[moduleIndex].queryRadius = 18.0f;
		modules[moduleIndex].spatialCellIndex = 0;
		modules[moduleIndex].spatialCellRadius = 0;
	}
	// Exact owner order intentionally differs from stable ObjectID order.
	for (unsigned ownerOrder = 0; ownerOrder < moduleCount; ++ownerOrder)
	{
		schedule[ownerOrder].moduleIndex = moduleCount - ownerOrder - 1;
		schedule[ownerOrder].ownerOrder = ownerOrder;
	}
}

void fillSingleCellSpatialFixture(
	rts::SimulationReadSpatialCellSpan *cell,
	unsigned *spatialObjectIndices, unsigned objectCount)
{
	cell->cellIndex = 0;
	cell->objectIndexOffset = 0;
	cell->objectIndexCount = objectCount;
	for (unsigned index = 0; index != objectCount; ++index)
		spatialObjectIndices[index] = index;
}

bool independentLegacySphereCandidate(float dx, float dy, float dz,
	float ownerRadius, float candidateRadius, float queryRadius)
{
	const float actualSquared = dx * dx + dy * dy + dz * dz;
	const float totalRadius = ownerRadius + candidateRadius;
	const float actual = std::sqrt(actualSquared);
	const float boundary = actual - totalRadius;
	const float boundarySquared = boundary <= 0.0f ? 0.0f :
		boundary * boundary;
	return boundarySquared < queryRadius * queryRadius;
}

bool mathematicalSquaredSphereCandidate(float dx, float dy, float dz,
	float ownerRadius, float candidateRadius, float queryRadius)
{
	const double reach = static_cast<double>(queryRadius) + ownerRadius +
		candidateRadius;
	return static_cast<double>(dx) * dx + static_cast<double>(dy) * dy +
		static_cast<double>(dz) * dz < reach * reach;
}

void testReadViewValidation()
{
	rts::SimulationReadObjectRecord objects[4];
	rts::SimulationReadModuleRecord modules[2];
	rts::SimulationReadScheduleEntry schedule[2];
	fillFixture(objects, modules, schedule, 4, 2);
	rts::SimulationReadView valid(100, 5, objects, 4, modules, 2,
		schedule, 2);
	expect(valid.isValid(),
		"fixed immutable view accepts ObjectID/module-ordered records");
	objects[1].objectID = objects[0].objectID;
	expect(!valid.isValid(), "duplicate ObjectID rejects the read view");
	objects[1].objectID = 1001;
	schedule[1].moduleIndex = schedule[0].moduleIndex;
	expect(!valid.isValid(),
		"duplicate scheduled module rejects owner publication");
	schedule[0].moduleIndex = 1;
	schedule[1].moduleIndex = 0;
	rts::SimulationReadSpatialCellSpan cell;
	unsigned spatialObjectIndices[4];
	fillSingleCellSpatialFixture(&cell, spatialObjectIndices, 4);
	rts::SimulationReadView spatial(100, 6, objects, 4, modules, 2,
		schedule, 2, 1, 1, &cell, 1, spatialObjectIndices, 4);
	expect(spatial.isValid(),
		"immutable spatial view accepts compact ObjectID-ordered cell spans");
	spatialObjectIndices[2] = spatialObjectIndices[1];
	expect(!spatial.isValid(),
		"duplicate cell membership fails closed before worker publication");
}

void testCapturePreflightRejectsBeforeWork()
{
	rts::ObjectComputationOptions serialOptions;
	rts::ObjectComputationMetrics metrics;
	expect(rts::PreflightObjectComputationIsland(serialOptions, &metrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_REFERENCE &&
		noCaptureOrJobWork(metrics) && metrics.serialFallbacks == 0,
		"serial policy preflight performs no island capture, allocation, or jobs");

	rts::ObjectComputationOptions parallelOptions;
	parallelOptions.parallel = true;
	expect(rts::PreflightObjectComputationIsland(parallelOptions, &metrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		noCaptureOrJobWork(metrics) && metrics.serialFallbacks == 1,
		"unavailable parallel/shadow scheduler rejects before capture work");
}

void testReferenceAndParallelAgreement()
{
	enum { OBJECT_COUNT = 1024, MODULE_COUNT = 64 };
	rts::SimulationReadObjectRecord objects[OBJECT_COUNT];
	rts::SimulationReadModuleRecord modules[MODULE_COUNT];
	rts::SimulationReadScheduleEntry schedule[MODULE_COUNT];
	rts::SimulationReadSpatialCellSpan cell;
	unsigned spatialObjectIndices[OBJECT_COUNT];
	fillFixture(objects, modules, schedule, OBJECT_COUNT, MODULE_COUNT);
	fillSingleCellSpatialFixture(&cell, spatialObjectIndices, OBJECT_COUNT);
	rts::SimulationReadView view(100, 7, objects, OBJECT_COUNT, modules,
		MODULE_COUNT, schedule, MODULE_COUNT, 1, 1, &cell, 1,
		spatialObjectIndices, OBJECT_COUNT);

	rts::ObjectComputationIsland reference;
	rts::ObjectComputationOptions referenceOptions;
	rts::ObjectComputationMetrics referenceMetrics;
	expect(reference.prepare(view, referenceOptions, &referenceMetrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_REFERENCE,
		"one-owner reference computes the pointer-free candidate sets");
	expect(reference.commandCount() == MODULE_COUNT &&
		referenceMetrics.submittedJobs == 0,
		"reference emits one fixed command per scheduled module");

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 128;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "four-worker computation scheduler starts");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"test thread registers as the game owner");

	rts::ObjectComputationIsland parallel;
	rts::ObjectComputationOptions parallelOptions;
	parallelOptions.parallel = true;
	rts::ObjectComputationMetrics parallelMetrics;
	expect(parallel.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_PARALLEL,
		"eligible work executes as a multi-range object island");
	unsigned firstDifference = 99;
	expect(rts::ObjectComputationCommandsEqual(view, reference, parallel,
		&firstDifference),
		"parallel command payloads exactly match the owner reference");
	expect(parallelMetrics.rangeCount >= 2 &&
		parallelMetrics.submittedJobs == parallelMetrics.completedJobs &&
		parallelMetrics.schedulerReleasedJobs ==
			parallelMetrics.submittedJobs &&
		parallelMetrics.distinctPhysicalWorkers > 1 &&
		parallelMetrics.emittedCommands == MODULE_COUNT &&
		parallelMetrics.emittedCandidates > 0 &&
		parallelMetrics.candidatePayloadBytes <
			MODULE_COUNT * OBJECT_COUNT * sizeof(unsigned) &&
		parallelMetrics.spatialCellSpans == 1 &&
		parallelMetrics.spatialMemberships == OBJECT_COUNT,
		"parallel metrics prove ranged work, scheduler ownership, and fencing");
	for (unsigned commandIndex = 0; commandIndex < MODULE_COUNT;
		++commandIndex)
	{
		const rts::SimulationMergedCommand *command =
			parallel.commandAt(commandIndex);
		rts::ObjectComputationCandidateSetHeader header;
		expect(command != 0 && command->command()->orderKey().phase() ==
			commandIndex && rts::DecodeObjectComputationCandidateSet(
				view, *command, &header),
			"owner commands merge in captured scheduler order");
	}

	rts::ObjectComputationIsland failed;
	parallelOptions.testFault = rts::OBJECT_COMPUTATION_TEST_PRODUCER_FAILURE;
	parallelOptions.testOrdinal = 3;
	expect(failed.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		failed.commandCount() == 0 && parallelMetrics.serialFallbacks == 1,
		"injected producer failure publishes no partial owner commands");
	expect(parallelMetrics.schedulerReleasedJobs ==
		parallelMetrics.submittedJobs,
		"scheduler releases every admitted failing producer job exactly once");

	rts::ObjectComputationIsland cancelled;
	parallelOptions.testFault =
		rts::OBJECT_COMPUTATION_TEST_CANCEL_AFTER_ADMISSION;
	parallelOptions.testOrdinal = 0;
	expect(cancelled.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		cancelled.commandCount() == 0 &&
		parallelMetrics.schedulerReleasedJobs ==
			parallelMetrics.submittedJobs,
		"cancelled admitted wave preserves scheduler-owned job lifetime");

	rts::ObjectComputationIsland ownerHelped;
	parallelOptions.testFault =
		rts::OBJECT_COMPUTATION_TEST_OWNER_HELP_ONLY;
	expect(ownerHelped.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		ownerHelped.commandCount() == 0 &&
		parallelMetrics.physicalWorkerJobs == 0 &&
		parallelMetrics.ownerHelpedJobs == parallelMetrics.submittedJobs &&
		parallelMetrics.distinctPhysicalWorkers == 0 &&
		parallelMetrics.serialFallbacks == 1,
		"all-owner-help identity is evidence-only and never authoritative");

	rts::ObjectComputationIsland timedOut;
	parallelOptions.testFault =
		rts::OBJECT_COMPUTATION_TEST_PHYSICAL_WAIT_TIMEOUT;
	expect(timedOut.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		timedOut.commandCount() == 0 &&
		parallelMetrics.physicalWaitTimeouts == 1 &&
		parallelMetrics.submittedJobs != 0 &&
		parallelMetrics.schedulerReleasedJobs ==
			parallelMetrics.submittedJobs &&
		parallelMetrics.serialFallbacks == 1,
		"physical wait timeout cancels, fences, and publishes no commands");

	rts::ObjectComputationIsland mismatching;
	parallelOptions.testFault = rts::OBJECT_COMPUTATION_TEST_SHADOW_MISMATCH;
	parallelOptions.testOrdinal = 5;
	expect(mismatching.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_PARALLEL,
		"shadow mismatch fixture remains a structurally valid worker wave");
	expect(!rts::ObjectComputationCommandsEqual(view, reference, mismatching,
		&firstDifference) && firstDifference == 5,
		"shadow oracle reports the first injected command mismatch");

	rts::ObjectComputationIsland budgetFailure;
	parallelOptions.testFault =
		rts::OBJECT_COMPUTATION_TEST_PAYLOAD_BUDGET_FAILURE;
	parallelOptions.testOrdinal = 0;
	expect(budgetFailure.prepare(view, parallelOptions, &parallelMetrics) ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		budgetFailure.commandCount() == 0 &&
		parallelMetrics.submittedJobs == 0 &&
		parallelMetrics.serialFallbacks == 1,
		"bounded payload arena failure publishes nothing before admission");

	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"game owner unregisters after parallel tests");
	jobs.shutdown();
}

void testOwnerFloatingPointBoundaryParity()
{
	enum { OBJECT_COUNT = 1024, MODULE_COUNT = 64 };
	rts::SimulationReadObjectRecord objects[OBJECT_COUNT];
	rts::SimulationReadModuleRecord modules[MODULE_COUNT];
	rts::SimulationReadScheduleEntry schedule[MODULE_COUNT];
	rts::SimulationReadSpatialCellSpan cell;
	unsigned spatialObjectIndices[OBJECT_COUNT];
	fillFixture(objects, modules, schedule, OBJECT_COUNT, MODULE_COUNT);
	fillSingleCellSpatialFixture(&cell, spatialObjectIndices, OBJECT_COUNT);
	const unsigned insideIndex = OBJECT_COUNT - 2;
	const unsigned boundaryIndex = OBJECT_COUNT - 1;
	objects[insideIndex].positionX = std::nextafter(20.5f, 0.0f);
	objects[insideIndex].positionY = 0.0f;
	objects[insideIndex].positionZ = 0.0f;
	objects[boundaryIndex].positionX = 20.5f;
	objects[boundaryIndex].positionY = 0.0f;
	objects[boundaryIndex].positionZ = 0.0f;
	modules[0].queryRadius = 18.0f;
	rts::SimulationReadView view(300, 9, objects, OBJECT_COUNT, modules,
		MODULE_COUNT, schedule, MODULE_COUNT, 1, 1, &cell, 1,
		spatialObjectIndices, OBJECT_COUNT);

	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 128;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "floating-point parity scheduler starts");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"floating-point parity owner registers");

	const rts::JobFloatingPointState savedFloatingPointState;
#if defined(_WIN32) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	_mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | _MM_ROUND_DOWN);
#endif
	rts::ObjectComputationIsland reference;
	rts::ObjectComputationOptions referenceOptions;
	expect(reference.prepare(view, referenceOptions) ==
		rts::OBJECT_COMPUTATION_SERIAL_REFERENCE,
		"boundary reference uses the submitting owner floating-point state");
	rts::ObjectComputationIsland parallel;
	rts::ObjectComputationOptions parallelOptions;
	parallelOptions.parallel = true;
	rts::ObjectComputationMetrics metrics;
	expect(parallel.prepare(view, parallelOptions, &metrics) ==
		rts::OBJECT_COMPUTATION_PARALLEL &&
		metrics.distinctPhysicalWorkers > 1,
		"boundary candidate wave executes on multiple physical workers");
	expect(rts::ObjectComputationCommandsEqual(view, reference, parallel),
		"worker candidate indices preserve owner rounding-mode parity");
	const rts::SimulationMergedCommand *moduleZeroCommand = 0;
	for (unsigned index = 0; index != reference.commandCount(); ++index)
	{
		rts::ObjectComputationCandidateSetHeader header;
		const rts::SimulationMergedCommand *candidate = reference.commandAt(index);
		if (candidate != 0 && rts::DecodeObjectComputationCandidateSet(
			view, *candidate, &header) && header.moduleIndex == 0)
		{
			moduleZeroCommand = candidate;
			break;
		}
	}
	expect(moduleZeroCommand != 0 &&
		rts::ObjectComputationCandidateAt(view, *moduleZeroCommand,
			insideIndex) &&
		!rts::ObjectComputationCandidateAt(view, *moduleZeroCommand,
			boundaryIndex),
		"strict sphere boundary includes next-inside and excludes exact edge");

	savedFloatingPointState.apply();
	{
		rts::SimulationReadObjectRecord counterObjects[OBJECT_COUNT];
		rts::SimulationReadModuleRecord counterModules[MODULE_COUNT];
		rts::SimulationReadScheduleEntry counterSchedule[MODULE_COUNT];
		rts::SimulationReadSpatialCellSpan counterCell;
		unsigned counterIndices[OBJECT_COUNT];
		fillFixture(counterObjects, counterModules, counterSchedule,
			OBJECT_COUNT, MODULE_COUNT);
		fillSingleCellSpatialFixture(&counterCell, counterIndices, OBJECT_COUNT);
		const float dx = 10.45473099f;
		const float dy = 294.51879883f;
		const float dz = 0.0016162046f;
		const float ownerRadius = 34.68774796f;
		const float candidateRadius = 50.08576202f;
		const float queryRadius = 209.93080139f;
		counterObjects[0].positionX = 0.0f;
		counterObjects[0].positionY = 0.0f;
		counterObjects[0].positionZ = 0.0f;
		counterObjects[0].zCenterOffset = 0.0f;
		counterObjects[0].boundingSphereRadius = ownerRadius;
		counterObjects[1].positionX = dx;
		counterObjects[1].positionY = dy;
		counterObjects[1].positionZ = dz;
		counterObjects[1].zCenterOffset = 0.0f;
		counterObjects[1].boundingSphereRadius = candidateRadius;
		counterModules[0].queryRadius = queryRadius;
		rts::SimulationReadView counterView(301, 10, counterObjects,
			OBJECT_COUNT, counterModules, MODULE_COUNT, counterSchedule,
			MODULE_COUNT, 1, 1, &counterCell, 1, counterIndices,
			OBJECT_COUNT);
		expect(!independentLegacySphereCandidate(dx, dy, dz, ownerRadius,
			candidateRadius, queryRadius) &&
			mathematicalSquaredSphereCandidate(dx, dy, dz, ownerRadius,
				candidateRadius, queryRadius),
			"independent fixture proves squared-distance equivalence flips the legacy edge");
		rts::ObjectComputationIsland counterReference;
		rts::ObjectComputationIsland counterParallel;
		rts::ObjectComputationOptions counterReferenceOptions;
		rts::ObjectComputationOptions counterParallelOptions;
		counterParallelOptions.parallel = true;
		expect(counterReference.prepare(counterView, counterReferenceOptions) ==
			rts::OBJECT_COMPUTATION_SERIAL_REFERENCE &&
			counterParallel.prepare(counterView, counterParallelOptions) ==
				rts::OBJECT_COMPUTATION_PARALLEL &&
			rts::ObjectComputationCommandsEqual(counterView, counterReference,
				counterParallel),
			"worker reproduces the legacy float/sqrt/subtract/clamp/square sequence");
		const rts::SimulationMergedCommand *counterCommand = 0;
		for (unsigned index = 0; index != counterReference.commandCount(); ++index)
		{
			rts::ObjectComputationCandidateSetHeader header;
			const rts::SimulationMergedCommand *candidate =
				counterReference.commandAt(index);
			if (candidate != 0 && rts::DecodeObjectComputationCandidateSet(
				counterView, *candidate, &header) && header.moduleIndex == 0)
			{
				counterCommand = candidate;
				break;
			}
		}
		expect(counterCommand != 0 &&
			!rts::ObjectComputationCandidateAt(counterView, *counterCommand, 1),
			"counterexample remains excluded exactly as legacy PartitionManager does");
	}
	savedFloatingPointState.apply();
	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"floating-point parity owner unregisters");
	jobs.shutdown();
}

void testOneWorkerFallback()
{
	enum { OBJECT_COUNT = 256, MODULE_COUNT = 24 };
	rts::SimulationReadObjectRecord objects[OBJECT_COUNT];
	rts::SimulationReadModuleRecord modules[MODULE_COUNT];
	rts::SimulationReadScheduleEntry schedule[MODULE_COUNT];
	rts::SimulationReadSpatialCellSpan cell;
	unsigned spatialObjectIndices[OBJECT_COUNT];
	fillFixture(objects, modules, schedule, OBJECT_COUNT, MODULE_COUNT);
	fillSingleCellSpatialFixture(&cell, spatialObjectIndices, OBJECT_COUNT);
	rts::SimulationReadView view(200, 8, objects, OBJECT_COUNT, modules,
		MODULE_COUNT, schedule, MODULE_COUNT, 1, 1, &cell, 1,
		spatialObjectIndices, OBJECT_COUNT);
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 32;
	config.scratchBytesPerWorker = 32 * 1024;
	config.pinWorkers = false;
	expect(jobs.start(config), "one-worker scheduler starts");
	expect(jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
		"one-worker fixture registers the owner");
	rts::ObjectComputationIsland island;
	rts::ObjectComputationOptions options;
	options.parallel = true;
	rts::ObjectComputationMetrics preflightMetrics;
	expect(rts::PreflightObjectComputationIsland(options,
		&preflightMetrics) == rts::OBJECT_COMPUTATION_POLICY_INELIGIBLE &&
		noCaptureOrJobWork(preflightMetrics) &&
		preflightMetrics.serialFallbacks == 0,
		"one-worker preflight rejects before island capture or allocation");
	rts::ObjectComputationMetrics prepareMetrics;
	expect(island.prepare(view, options, &prepareMetrics) ==
		rts::OBJECT_COMPUTATION_POLICY_INELIGIBLE && island.commandCount() == 0 &&
		prepareMetrics.rangeCount == 0 && prepareMetrics.submittedJobs == 0 &&
		prepareMetrics.allocatedBytes == 0 &&
		prepareMetrics.serialFallbacks == 0,
		"one worker deterministically retains the allocation-free legacy path");
	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"one-worker owner unregisters");
	jobs.shutdown();
}
}

int main()
{
	testReadViewValidation();
	testCapturePreflightRejectsBeforeWork();
	testReferenceAndParallelAgreement();
	testOwnerFloatingPointBoundaryParity();
	testOneWorkerFallback();
	if (failures != 0) return 1;
	printf("Object computation island tests passed.\n");
	return 0;
}
