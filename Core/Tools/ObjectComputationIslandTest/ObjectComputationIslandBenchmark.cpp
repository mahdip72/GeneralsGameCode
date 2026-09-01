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
// measures island capture+wait+decode and deterministic worker scaling only.
// CMake registration is intentionally owned by the integration lane.
#include "Lib/JobSystem.h"
#include "Lib/ObjectComputationIsland.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
struct Evidence
{
	unsigned candidates;
};

bool run(unsigned objectCount, unsigned workerCount)
{
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
	if (!jobs.start(config) || !jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
		return false;
	const std::chrono::steady_clock::time_point islandBegin =
		std::chrono::steady_clock::now();
	rts::SimulationReadView view(600, objectCount + workerCount,
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
	for (unsigned command = 0; command != island.commandCount(); ++command)
	{
		const rts::SimulationMergedCommand *merged = island.commandAt(command);
		rts::ObjectComputationCandidateSetHeader header;
		if (merged == 0 || !rts::DecodeObjectComputationCandidateSet(view,
			*merged, &header)) return false;
		for (unsigned ordinal = 0; ordinal != header.candidateCount; ++ordinal)
		{
			UnsignedInt objectIndex = 0;
			if (!rts::ObjectComputationCandidateIndexAt(view, *merged, ordinal,
				&objectIndex)) return false;
			++committed.candidates;
		}
	}
	const std::chrono::steady_clock::time_point islandEnd =
		std::chrono::steady_clock::now();
	rts::ObjectComputationIsland reference;
	rts::ObjectComputationOptions referenceOptions;
	const bool referenceParity = workerCount == 1 ||
		(reference.prepare(view, referenceOptions) ==
			rts::OBJECT_COMPUTATION_SERIAL_REFERENCE &&
		rts::ObjectComputationCommandsEqual(view, reference, island));
	const bool expectedResult = workerCount == 1 ?
		computation == rts::OBJECT_COMPUTATION_SERIAL_REFERENCE &&
			metrics.submittedJobs == 0 :
		computation == rts::OBJECT_COMPUTATION_PARALLEL &&
			metrics.distinctPhysicalWorkers > 1 &&
			metrics.visitedSpatialMembers < moduleCount * objectCount / 4 &&
			metrics.arenaBudgetBytes <= 16u * 1024u * 1024u;
	const bool parity = expectedResult && referenceParity &&
		committed.candidates == metrics.emittedCandidates;
	const double islandMs = std::chrono::duration<double, std::milli>(
		captureEnd - captureBegin).count() +
		std::chrono::duration<double, std::milli>(
			islandEnd - islandBegin).count();
	std::printf("objects=%u workers=%u island_total_ms=%.3f "
		"physical=%u visited=%u payload=%u parity=%s\n", objectCount,
		workerCount, islandMs, metrics.distinctPhysicalWorkers,
		metrics.visitedSpatialMembers, metrics.candidatePayloadBytes,
		parity ? "yes" : "no");
	jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	jobs.shutdown();
	return parity;
}
}

int main()
{
	const unsigned sizes[] = { 1000, 4000, 8000 };
	const unsigned workers[] = { 1, 2, 4, 8, 16 };
	for (unsigned size = 0; size != sizeof(sizes) / sizeof(sizes[0]); ++size)
		for (unsigned worker = 0;
			worker != sizeof(workers) / sizeof(workers[0]); ++worker)
			if (!run(sizes[size], workers[worker])) return 1;
	return 0;
}
