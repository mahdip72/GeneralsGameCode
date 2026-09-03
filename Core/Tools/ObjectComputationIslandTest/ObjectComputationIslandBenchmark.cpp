/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
// Focused modern-only scaling evidence. The timed island interval includes
// immutable capture, physical-worker wait, merge, decode, and owner commit.
// Legacy title cost is deliberately not approximated by an artificial M*N
// loop here. Production admission uses live PartitionManager cell/member visit
// totals and the shared benchmark-calibrated transaction model. This tool
// measures island capture+wait+decode and deterministic worker scaling. The
// correctness lane may account for a real owner fallback, but never qualifies
// it as scaling. CMake registration is intentionally owned by the integration
// lane.
#include "Lib/JobSystem.h"
#include "Lib/ObjectComputationIsland.h"
#include "ObjectComputationTestMode.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
struct Evidence
{
	unsigned candidates;
};

bool collectCandidateEvidence(const rts::SimulationReadView &view,
	const rts::ObjectComputationIsland &island, Evidence *evidence)
{
	if (evidence == 0) return false;
	evidence->candidates = 0;
	for (unsigned command = 0; command != island.commandCount(); ++command)
	{
		const rts::SimulationMergedCommand *merged = island.commandAt(command);
		rts::ObjectComputationCandidateSetHeader header;
		if (merged == 0 || !rts::DecodeObjectComputationCandidateSet(view,
			*merged, &header))
			return false;
		for (unsigned ordinal = 0; ordinal != header.candidateCount; ++ordinal)
		{
			UnsignedInt objectIndex = 0;
			if (!rts::ObjectComputationCandidateIndexAt(view, *merged,
				ordinal, &objectIndex))
				return false;
			++evidence->candidates;
		}
	}
	return true;
}

bool run(unsigned objectCount, unsigned requestedWorkerCount,
	bool localCapacity, rts_test::ObjectComputationTestMode mode)
{
	const unsigned workerCount = rts_test::ResolveActualWorkerCount(
		requestedWorkerCount, localCapacity);
	rts_test::PrintWorkerCountSubstitution(
		"Object computation island benchmark", requestedWorkerCount,
		workerCount, localCapacity);
	const unsigned cellCountX = 64;
	const unsigned cellCountY = (objectCount + cellCountX - 1) / cellCountX;
	const unsigned moduleCount = objectCount / 8 < 1024 ?
		objectCount / 8 : 1024;
	std::vector<rts::SimulationReadObjectRecord> objects(objectCount);
	std::vector<rts::SimulationReadModuleRecord> modules(moduleCount);
	std::vector<rts::SimulationReadScheduleEntry> schedule(moduleCount);
	std::vector<rts::SimulationReadSpatialCellSpan> cells(objectCount);
	std::vector<UnsignedInt> members(objectCount);

	const std::chrono::steady_clock::time_point captureBegin =
		std::chrono::steady_clock::now();
	for (unsigned object = 0; object != objectCount; ++object)
	{
		rts::SimulationReadObjectRecord &record = objects[object];
		record.objectID = object + 1;
		record.objectGeneration = 1;
		record.positionX = static_cast<float>(object % cellCountX) * 4.0f;
		record.positionY = static_cast<float>(object / cellCountX) * 4.0f;
		record.positionZ = static_cast<float>(object % 5);
		record.boundingSphereRadius = 1.25f;
		record.zCenterOffset = 1.0f;
		cells[object].cellIndex = object;
		cells[object].objectIndexOffset = object;
		cells[object].objectIndexCount = 1;
		members[object] = object;
	}
	for (unsigned module = 0; module != moduleCount; ++module)
	{
		const unsigned owner = module * objectCount / moduleCount;
		modules[module].objectID = owner + 1;
		modules[module].moduleOrdinal = 7;
		modules[module].moduleType = rts::OBJECT_COMPUTATION_MODULE_HORDE;
		modules[module].wakePriority = 402;
		modules[module].queryRadius = 18.0f;
		modules[module].spatialCellIndex = owner;
		modules[module].spatialCellRadius = 6;
		schedule[module].moduleIndex = moduleCount - module - 1;
		schedule[module].ownerOrder = module;
	}
	const std::chrono::steady_clock::time_point captureEnd =
		std::chrono::steady_clock::now();
	rts::JobSystem &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = workerCount;
	config.queueCapacity = 2048;
	config.scratchBytesPerWorker = 64 * 1024;
	config.pinWorkers = false;
	if (!jobs.start(config))
		return false;
	if (!jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{
		jobs.shutdown();
		return false;
	}
	const std::chrono::steady_clock::time_point islandBegin =
		std::chrono::steady_clock::now();
	rts::SimulationReadView view(600, objectCount + requestedWorkerCount,
		objects.data(), objectCount, modules.data(), moduleCount,
		schedule.data(), moduleCount, cellCountX, cellCountY, cells.data(),
		objectCount, members.data(), objectCount);
	rts::ObjectComputationIsland island;
	rts::ObjectComputationOptions options;
	options.parallel = workerCount > 1;
	rts::ObjectComputationMetrics metrics;
	const rts::ObjectComputationResult computation = island.prepare(view,
		options, &metrics);
	Evidence committed = { 0 };
	const bool committedOutputValid = computation ==
		rts::OBJECT_COMPUTATION_SERIAL_FALLBACK ? true :
		collectCandidateEvidence(view, island, &committed);
	const std::chrono::steady_clock::time_point islandEnd =
		std::chrono::steady_clock::now();
	rts::ObjectComputationIsland reference;
	rts::ObjectComputationOptions referenceOptions;
	rts::ObjectComputationMetrics referenceMetrics;
	const bool referenceReady =
		reference.prepare(view, referenceOptions, &referenceMetrics) ==
			rts::OBJECT_COMPUTATION_SERIAL_REFERENCE;
	Evidence referenceEvidence = { 0 };
	const bool referenceOutputValid = referenceReady &&
		collectCandidateEvidence(view, reference, &referenceEvidence) &&
		reference.commandCount() == moduleCount &&
		referenceMetrics.emittedCommands == moduleCount &&
		referenceEvidence.candidates == referenceMetrics.emittedCandidates;
	const bool outputParity = referenceOutputValid && committedOutputValid &&
		rts::ObjectComputationCommandsEqual(view, reference, island);
	const bool candidateParity = referenceOutputValid &&
		(committed.candidates == metrics.emittedCandidates) &&
		(computation == rts::OBJECT_COMPUTATION_SERIAL_FALLBACK ||
			committed.candidates == referenceEvidence.candidates);
	const bool strictExpectedResult = workerCount == 1 ?
		computation == rts::OBJECT_COMPUTATION_SERIAL_REFERENCE &&
			metrics.submittedJobs == 0 :
		computation == rts::OBJECT_COMPUTATION_PARALLEL &&
			metrics.distinctPhysicalWorkers > 1 &&
			metrics.visitedSpatialMembers < moduleCount * objectCount / 4 &&
			metrics.arenaBudgetBytes <= 16u * 1024u * 1024u;
	const bool dispositionAccepted = computation ==
		rts::OBJECT_COMPUTATION_SERIAL_REFERENCE ?
		(strictExpectedResult && referenceOutputValid) :
		rts_test::ObjectComputationResultIsAccepted(mode, computation, island,
		metrics, moduleCount);
	bool accepted = false;
	bool qualified = false;
	const char *resultName = "invalid";
	if (computation == rts::OBJECT_COMPUTATION_SERIAL_REFERENCE)
	{
		resultName = "serial-reference";
		accepted = dispositionAccepted && outputParity && candidateParity;
	}
	else if (computation == rts::OBJECT_COMPUTATION_PARALLEL)
	{
		resultName = "parallel";
		qualified = strictExpectedResult;
		accepted = dispositionAccepted && outputParity && candidateParity &&
			(rts_test::ObjectComputationAllowsSerialFallback(mode) ||
				qualified);
	}
	else if (computation == rts::OBJECT_COMPUTATION_SERIAL_FALLBACK)
	{
		resultName = "serial-fallback";
		const bool fallbackAccounted =
			rts_test::ObjectComputationSerialFallbackIsWellAccounted(island,
			metrics);
		rts::ObjectComputationIsland ownerFallback;
		rts::ObjectComputationMetrics ownerMetrics;
		const bool ownerReady = rts_test::ObjectComputationAllowsSerialFallback(
			mode) && fallbackAccounted &&
			ownerFallback.prepare(view, referenceOptions, &ownerMetrics) ==
			rts::OBJECT_COMPUTATION_SERIAL_REFERENCE;
		Evidence ownerEvidence = { 0 };
		const bool ownerOutputValid = ownerReady &&
			collectCandidateEvidence(view, ownerFallback, &ownerEvidence) &&
			ownerFallback.commandCount() == moduleCount &&
			ownerMetrics.emittedCommands == moduleCount &&
			ownerEvidence.candidates == ownerMetrics.emittedCandidates;
		const bool ownerParity = ownerOutputValid && referenceOutputValid &&
			rts::ObjectComputationCommandsEqual(view, reference,
				ownerFallback) &&
			ownerEvidence.candidates == referenceEvidence.candidates;
		accepted = dispositionAccepted && ownerParity;
	}
	const double islandMs = std::chrono::duration<double, std::milli>(
		captureEnd - captureBegin).count() +
		std::chrono::duration<double, std::milli>(
			islandEnd - islandBegin).count();
	std::printf("objects=%u workers=%u island_total_ms=%.3f "
		"physical=%u visited=%u payload=%u mode=%s result=%s "
		"qualification=%s timing=attempted-island parity=%s\n", objectCount,
		workerCount, islandMs, metrics.distinctPhysicalWorkers,
		metrics.visitedSpatialMembers, metrics.candidatePayloadBytes,
		rts_test::ObjectComputationTestModeName(mode), resultName,
		qualified ? "qualified" : "non-qualifying",
		accepted ? "yes" : "no");
	jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	jobs.shutdown();
	return accepted;
}
}

int main(int argc, char **argv)
{
	bool localCapacity = false;
	rts_test::ObjectComputationTestMode mode =
		rts_test::OBJECT_COMPUTATION_TEST_STRICT_SCALING;
	if (!rts_test::ParseObjectComputationTestMode(argc, argv,
		&localCapacity, &mode))
	{
		std::fprintf(stderr,
			"Usage: core_object_computation_island_benchmark "
			"[--local-capacity] "
			"[--strict-scaling|--allow-serial-fallback]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	std::printf("Object computation test mode: %s\n",
		rts_test::ObjectComputationTestModeName(mode));
	const unsigned sizes[] = { 1000, 4000, 8000 };
	const unsigned workers[] = { 1, 2, 4, 8, 16 };
	for (unsigned size = 0; size != sizeof(sizes) / sizeof(sizes[0]); ++size)
		for (unsigned worker = 0;
			worker != sizeof(workers) / sizeof(workers[0]); ++worker)
			if (!run(sizes[size], workers[worker], localCapacity, mode))
				return 1;
	return 0;
}
