/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/WaterPolygonKernel.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#include <new>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#if defined(_WIN32) && !defined(_WIN64)
#include <float.h>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
#endif

namespace
{
unsigned failures = 0;

#if defined(_WIN32) && !defined(_WIN64)
class GameFloatingPointScope
{
public:
	GameFloatingPointScope()
		: m_control(_controlfp(0, 0))
	{
		_controlfp(_PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);
	}
	~GameFloatingPointScope()
	{
		_controlfp(m_control, _MCW_PC | _MCW_RC);
	}

private:
	unsigned m_control;
};
#endif

#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), #condition); \
} } while (0)

enum
{
	JOB_SYSTEM_TEST_FAIL_GROUP_ALLOCATION = 4,
	JOB_SYSTEM_TEST_FAIL_JOB_ALLOCATION = 5,
	JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH = 6,
	WATER_TEST_MAX_JOBS = 64
};

Real s_fastSinTable[WATER_POLYGON_FAST_SIN_TABLE_SIZE];

static void initializeSinTable()
{
	for (unsigned index = 0; index < WATER_POLYGON_FAST_SIN_TABLE_SIZE; ++index)
	{
		const Real angle = static_cast<Real>(index) * 2.0f * 3.141592654f /
			static_cast<Real>(WATER_POLYGON_FAST_SIN_TABLE_SIZE);
		s_fastSinTable[index] = static_cast<Real>(sin(angle));
	}
}

static WaterPolygonSnapshot makeSnapshot(unsigned cellsX, unsigned cellsY,
	bool wavy)
{
	WaterPolygonSnapshot snapshot;
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.origin.x = 19.25f;
	snapshot.origin.y = -7.5f;
	snapshot.origin.z = 3.0f;
	snapshot.uVector.x = 8.0f;
	snapshot.uVector.y = 1.25f;
	snapshot.uVector.z = 0.5f;
	snapshot.vVector.x = -2.0f;
	snapshot.vVector.y = 9.0f;
	snapshot.vVector.z = 1.0f;
	snapshot.bilinear.x = 1.0f;
	snapshot.bilinear.y = -0.75f;
	snapshot.bilinear.z = 0.25f;
	snapshot.uCount = cellsX + 1;
	snapshot.vCount = cellsY + 1;
	snapshot.rectangleCount = cellsX * cellsY;
	snapshot.diffuse = 0x8fc08734u;
	snapshot.featherAlpha = wavy ? 140 : 0;
	snapshot.wavy = wavy;
	snapshot.waterFactor = 150.0f;
	snapshot.bumpSize = 50.0f;
	snapshot.phaseBase = 25.0f * 0.0375f;
	snapshot.mapCoeff = 3.14159265359f / (4.0f * 10.0f);
	snapshot.amplitude = 0.5f;
	snapshot.wobbleU = static_cast<Real>(0.02 * cos(11.0 * 0.0375));
	snapshot.wobbleV = static_cast<Real>(0.02 * cos(5.0 * 0.0375));
	snapshot.wavyWobbleU = 0.02 * cos(11.0 * 0.0375);
	snapshot.wavyWobbleV = 0.02 * cos(5.0 * 0.0375);
	snapshot.flatUScale = 1.0f / snapshot.waterFactor;
	snapshot.flatVScale = 1.0f / snapshot.waterFactor;
	snapshot.flatPhaseBase = snapshot.phaseBase;
	snapshot.flatMapCoeff = snapshot.mapCoeff;
	snapshot.flatRowScale = 1.0f / static_cast<Real>(cellsY);
	snapshot.flatColumnScale = 1.0f / static_cast<Real>(cellsX);
	snapshot.flatSinScale = static_cast<Real>(WATER_POLYGON_FAST_SIN_TABLE_SIZE) /
		(2.0f * 3.141592654f);
	snapshot.flatSinTable = s_fastSinTable;
	return snapshot;
}

static Real legacyFastSin(Real value, const WaterPolygonSnapshot &snapshot)
{
	value *= snapshot.flatSinScale;
	const int index0 = static_cast<int>(floor(value));
	const int index1 = index0 + 1;
	const Real fraction = value - static_cast<Real>(index0);
	const unsigned tableIndex0 = static_cast<unsigned>(index0) &
		(WATER_POLYGON_FAST_SIN_TABLE_SIZE - 1);
	const unsigned tableIndex1 = static_cast<unsigned>(index1) &
		(WATER_POLYGON_FAST_SIN_TABLE_SIZE - 1);
	return (1.0f - fraction) * snapshot.flatSinTable[tableIndex0] +
		fraction * snapshot.flatSinTable[tableIndex1];
}

static void legacyPosition(const WaterPolygonSnapshot &snapshot,
	unsigned row, unsigned column, Real &x, Real &y, Real &z)
{
	Real du;
	Real dv;
	if (snapshot.wavy)
	{
		dv = static_cast<Real>(row);
		dv /= (snapshot.vCount - 1);
		du = static_cast<Real>(column);
		du /= (snapshot.uCount - 1);
	}
	else
	{
		dv = static_cast<Real>(row) * snapshot.flatRowScale;
		du = static_cast<Real>(column) * snapshot.flatColumnScale;
	}
	x = snapshot.origin.x;
	y = snapshot.origin.y;
	z = snapshot.origin.z;
	x += snapshot.uVector.x * du;
	y += snapshot.uVector.y * du;
	z += snapshot.uVector.z * du;
	x += snapshot.vVector.x * dv;
	y += snapshot.vVector.y * dv;
	z += snapshot.vVector.z * dv;
	x += (dv) * (du) * snapshot.bilinear.x;
	y += (dv) * (du) * snapshot.bilinear.y;
	z += (dv) * (du) * snapshot.bilinear.z;
}

static WaterPolygonVertex legacyVertex(const WaterPolygonSnapshot &snapshot,
	unsigned index)
{
	WaterPolygonVertex vertex;
	const unsigned row = index / snapshot.uCount;
	const unsigned column = index % snapshot.uCount;
	Real x, y, z;
	legacyPosition(snapshot, row, column, x, y, z);
	vertex.x = x;
	vertex.y = y;
	if (snapshot.wavy)
	{
		const Real phase = snapshot.phaseBase + x * snapshot.mapCoeff;
		const Real wave = (sin(phase) - 1.0f) * snapshot.amplitude;
		vertex.z = z + wave;
		vertex.diffuse = (snapshot.diffuse & 0x00ffffffu) |
			((snapshot.featherAlpha & 0xffu) << 24);
		vertex.u1 = (x / snapshot.waterFactor) +
			snapshot.wavyWobbleU * wave;
		vertex.v1 = (y / snapshot.waterFactor) +
			snapshot.wavyWobbleV * wave;
	}
	else
	{
		vertex.z = z;
		vertex.diffuse = snapshot.diffuse;
		vertex.u1 = x * snapshot.flatUScale + snapshot.wobbleU *
			legacyFastSin(snapshot.flatPhaseBase + x * snapshot.flatMapCoeff,
				snapshot);
		vertex.v1 = y * snapshot.flatVScale + snapshot.wobbleV *
			legacyFastSin(snapshot.flatPhaseBase + y * snapshot.flatMapCoeff,
				snapshot);
	}
	vertex.u2 = x / snapshot.bumpSize;
	if (snapshot.wavy)
		vertex.v2 = y / snapshot.bumpSize + 0.3f * x / snapshot.bumpSize;
	else
		vertex.v2 = (y + 0.3f * x) / snapshot.bumpSize;
	vertex.nx = 0;
	vertex.ny = 0;
	vertex.nz = 1.0f;
	return vertex;
}

static unsigned short legacyIndex(const WaterPolygonSnapshot &snapshot,
	unsigned cell, unsigned slot)
{
	const unsigned cellsX = snapshot.uCount - 1;
	const unsigned row = cell / cellsX;
	const unsigned column = cell % cellsX;
	const unsigned rowStart = row * snapshot.uCount;
	const unsigned nextRowStart = (row + 1) * snapshot.uCount;
	const unsigned values[6] = {
		rowStart + column, nextRowStart + column + 1,
		nextRowStart + column, rowStart + column,
		rowStart + column + 1, nextRowStart + column + 1
	};
	return static_cast<unsigned short>(values[slot]);
}

class KernelJob : public rts::Job
{
public:
	static void *operator new(size_t bytes, const std::nothrow_t &) throw()
	{
		return malloc(bytes);
	}
	static void operator delete(void *memory) throw() { free(memory); }
	static void operator delete(void *memory, const std::nothrow_t &) throw()
	{
		free(memory);
	}

	KernelJob(const WaterPolygonSnapshot &snapshot,
		WaterPolygonVertex *vertices, unsigned short *indices,
		unsigned begin, unsigned end, bool indexJob)
		: m_snapshot(snapshot), m_vertices(vertices), m_indices(indices),
		  m_begin(begin), m_end(end), m_indexJob(indexJob),
		  m_floatingPointState() {}

	virtual void execute(rts::JobContext &context)
	{
		rts::JobFloatingPointScope floatingPointScope(m_floatingPointState);
		if (context.isCancellationRequested()) return;
		const bool completed = m_indexJob ?
			PrepareWaterPolygonIndices(m_snapshot, m_indices, m_begin, m_end) :
			PrepareWaterPolygonVertices(m_snapshot, m_vertices, m_begin, m_end);
		if (!completed) context.fail();
	}

private:
	const WaterPolygonSnapshot m_snapshot;
	WaterPolygonVertex *m_vertices;
	unsigned short *m_indices;
	unsigned m_begin, m_end;
	bool m_indexJob;
	const rts::JobFloatingPointState m_floatingPointState;
};

static bool submitRanges(const WaterPolygonSnapshot &snapshot,
	WaterPolygonVertex *vertices, unsigned short *indices, bool cancel)
{
	rts::JobSystem &system = rts::JobSystem::instance();
	if (!system.isRunning() || system.workerCount() == 0)
		return false;
	const unsigned vertexCount = snapshot.uCount * snapshot.vCount;
	const unsigned vertexJobs = rts::JobSystem::chooseRangeCount(vertexCount,
		256, system.workerCount());
	const unsigned indexJobs = rts::JobSystem::chooseRangeCount(
		snapshot.rectangleCount, 256, system.workerCount());
	const unsigned totalJobs = vertexJobs + indexJobs;
	if (totalJobs == 0 || totalJobs > WATER_TEST_MAX_JOBS)
		return false;
	rts::JobSubmission submissions[WATER_TEST_MAX_JOBS];
	rts::JobHandle handles[WATER_TEST_MAX_JOBS];
	unsigned created = 0;
	const rts::JobGroup group = system.createGroup();
	if (!group.isValid()) return false;
	for (; created < vertexJobs; ++created)
	{
		rts::JobRange range;
		CHECK(rts::JobSystem::rangeForIndex(vertexCount, vertexJobs, created,
			range));
		KernelJob *job = new (std::nothrow) KernelJob(snapshot, vertices, 0,
			range.begin, range.end, false);
		if (job == 0) break;
		submissions[created].job = job;
		submissions[created].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
	}
	if (created == vertexJobs)
	{
		for (unsigned index = 0; index < indexJobs; ++index, ++created)
		{
			rts::JobRange range;
			CHECK(rts::JobSystem::rangeForIndex(snapshot.rectangleCount, indexJobs,
				index, range));
			KernelJob *job = new (std::nothrow) KernelJob(snapshot, 0, indices,
				range.begin, range.end, true);
			if (job == 0) break;
			submissions[created].job = job;
			submissions[created].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
		}
	}
	if (created != totalJobs || !system.trySubmitBatch(submissions, totalJobs,
		group, handles))
	{
		for (unsigned index = 0; index < created; ++index)
			delete submissions[index].job;
		return false;
	}
	if (cancel) system.cancel(group);
	if (!system.wait(group)) return false;
	return !group.failed() && !group.wasCancelled();
}

static int checkCase(const WaterPolygonSnapshot &snapshot, bool runJobs)
{
	const unsigned vertexCount = snapshot.uCount * snapshot.vCount;
	const unsigned indexCount = snapshot.rectangleCount * 6;
	std::vector<WaterPolygonVertex> expected(vertexCount);
	std::vector<WaterPolygonVertex> serial(vertexCount);
	std::vector<unsigned short> expectedIndices(indexCount);
	std::vector<unsigned short> serialIndices(indexCount);
	for (unsigned index = 0; index < vertexCount; ++index)
		expected[index] = legacyVertex(snapshot, index);
	for (unsigned cell = 0; cell < snapshot.rectangleCount; ++cell)
		for (unsigned slot = 0; slot < 6; ++slot)
			expectedIndices[cell * 6 + slot] = legacyIndex(snapshot, cell, slot);
	CHECK(PrepareWaterPolygonVertices(snapshot, &serial[0], 0, vertexCount));
	CHECK(PrepareWaterPolygonIndices(snapshot, &serialIndices[0], 0, snapshot.rectangleCount));
	CHECK(memcmp(&serial[0], &expected[0],
		vertexCount * sizeof(WaterPolygonVertex)) == 0);
	CHECK(memcmp(&serialIndices[0], &expectedIndices[0],
		indexCount * sizeof(unsigned short)) == 0);
	if (runJobs && snapshot.rectangleCount >= WATER_POLYGON_MIN_PARALLEL_CELLS)
	{
		std::vector<WaterPolygonVertex> actual(vertexCount);
		std::vector<unsigned short> actualIndices(indexCount);
		memset(&actual[0], 0x5a, actual.size() * sizeof(WaterPolygonVertex));
		memset(&actualIndices[0], 0x5a,
			actualIndices.size() * sizeof(unsigned short));
		const bool prepared = submitRanges(snapshot, &actual[0],
			&actualIndices[0], false);
		if (!prepared)
		{
			CHECK(PrepareWaterPolygonVertices(snapshot, &actual[0], 0,
				vertexCount));
			CHECK(PrepareWaterPolygonIndices(snapshot, &actualIndices[0], 0,
				snapshot.rectangleCount));
		}
		CHECK(memcmp(&actual[0], &expected[0],
			vertexCount * sizeof(WaterPolygonVertex)) == 0);
		CHECK(memcmp(&actualIndices[0], &expectedIndices[0],
			indexCount * sizeof(unsigned short)) == 0);
	}
	return 0;
}

static int invalidAndRangeChecks()
{
	WaterPolygonSnapshot snapshot = makeSnapshot(50, 50, false);
	WaterPolygonVertex vertices[WATER_POLYGON_MAX_VERTICES];
	unsigned short indices[WATER_POLYGON_MAX_INDICES];
	CHECK(PrepareWaterPolygonVertices(snapshot, vertices, 0, 0));
	CHECK(PrepareWaterPolygonIndices(snapshot, indices, 0, 0));
	CHECK(!PrepareWaterPolygonVertices(snapshot, vertices, 4, 3));
	CHECK(!PrepareWaterPolygonIndices(snapshot, indices, 4, 3));
	CHECK(!PrepareWaterPolygonVertices(snapshot, vertices, 0,
		WATER_POLYGON_MAX_VERTICES + 1));
	CHECK(!PrepareWaterPolygonIndices(snapshot, indices, 0,
		WATER_POLYGON_MAX_INDICES));
	snapshot.uCount = WATER_POLYGON_MAX_EDGE_CELLS + 2;
	CHECK(!PrepareWaterPolygonVertices(snapshot, vertices, 0, 1));
	snapshot = makeSnapshot(50, 50, false);
	snapshot.waterFactor = 0.0f;
	CHECK(!PrepareWaterPolygonVertices(snapshot, vertices, 0, 1));
	snapshot = makeSnapshot(50, 50, false);
	snapshot.flatSinTable = 0;
	CHECK(!PrepareWaterPolygonVertices(snapshot, vertices, 0, 1));
	return 0;
}

static int failureAndCancellation(const WaterPolygonSnapshot &snapshot)
{
	const unsigned vertexCount = snapshot.uCount * snapshot.vCount;
	const unsigned indexCount = snapshot.rectangleCount * 6;
	std::vector<WaterPolygonVertex> expected(vertexCount);
	std::vector<unsigned short> expectedIndices(indexCount);
	std::vector<WaterPolygonVertex> actual(vertexCount);
	std::vector<unsigned short> actualIndices(indexCount);
	for (unsigned index = 0; index < vertexCount; ++index)
		expected[index] = legacyVertex(snapshot, index);
	for (unsigned cell = 0; cell < snapshot.rectangleCount; ++cell)
		for (unsigned slot = 0; slot < 6; ++slot)
			expectedIndices[cell * 6 + slot] = legacyIndex(snapshot, cell, slot);
	const unsigned faults[] = {
		JOB_SYSTEM_TEST_FAIL_GROUP_ALLOCATION,
		JOB_SYSTEM_TEST_FAIL_JOB_ALLOCATION,
		JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH
	};
	for (unsigned faultIndex = 0; faultIndex < sizeof(faults)/sizeof(faults[0]);
		++faultIndex)
	{
		memset(&actual[0], 0x5a, actual.size() * sizeof(WaterPolygonVertex));
		memset(&actualIndices[0], 0x5a,
			actualIndices.size() * sizeof(unsigned short));
#if defined(RTS_BUILD_CORE_EXTRAS)
		rts_job_system_set_test_fault(faults[faultIndex], 1);
#endif
		const bool submitted = submitRanges(snapshot, &actual[0],
			&actualIndices[0], false);
#if defined(RTS_BUILD_CORE_EXTRAS)
		rts_job_system_set_test_fault(0, 0);
#endif
#if defined(RTS_BUILD_CORE_EXTRAS)
#if defined(_MSC_VER) && _MSC_VER < 1300
		/* The VC6 adapter executes inline and only supports queue-push fault
		 * admission. Its group/job allocation paths have no fault hook, so
		 * those selectors leave the batch admitted. */
		CHECK(submitted ==
			(faults[faultIndex] != JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH));
#else
		CHECK(!submitted);
#endif
#else
		(void)submitted;
#endif
		CHECK(PrepareWaterPolygonVertices(snapshot, &actual[0], 0, vertexCount));
		CHECK(PrepareWaterPolygonIndices(snapshot, &actualIndices[0], 0,
			snapshot.rectangleCount));
		CHECK(memcmp(&actual[0], &expected[0],
			vertexCount * sizeof(WaterPolygonVertex)) == 0);
		CHECK(memcmp(&actualIndices[0], &expectedIndices[0],
			indexCount * sizeof(unsigned short)) == 0);
	}
	memset(&actual[0], 0x5a, actual.size() * sizeof(WaterPolygonVertex));
	memset(&actualIndices[0], 0x5a,
		actualIndices.size() * sizeof(unsigned short));
	CHECK(!submitRanges(snapshot, &actual[0], &actualIndices[0], true));
	CHECK(PrepareWaterPolygonVertices(snapshot, &actual[0], 0, vertexCount));
	CHECK(PrepareWaterPolygonIndices(snapshot, &actualIndices[0], 0,
		snapshot.rectangleCount));
	CHECK(memcmp(&actual[0], &expected[0],
		vertexCount * sizeof(WaterPolygonVertex)) == 0);
	CHECK(memcmp(&actualIndices[0], &expectedIndices[0],
		indexCount * sizeof(unsigned short)) == 0);
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
	return 0;
}

static int serialPolicy()
{
	CHECK(rts::SetPipelineExecutionMode("serial"));
	CHECK(checkCase(makeSnapshot(2, 2, false), false) == 0);
	CHECK(checkCase(makeSnapshot(50, 50, true), false) == 0);
	CHECK(rts::JobSystem::instance().metrics().submittedJobCount == 0);
	puts("water polygon serial policy uses no compute jobs");
	return failures == 0 ? 0 : 1;
}
}

int main(int argc, char **argv)
{
#if defined(_WIN32) && !defined(_WIN64)
	GameFloatingPointScope gameFloatingPointScope;
#endif
	bool localCapacity = false;
	bool serialPipelines = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity,
		&serialPipelines, "--serial-policy"))
	{
		fprintf(stderr, "Usage: core_water_polygon_kernel_tests [--local-capacity] [--serial-policy]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	initializeSinTable();
	if (serialPipelines)
		return serialPolicy();
	const unsigned workers[] = {1, 2, 4, 8, 16, 0};
	rts::JobSystem &system = rts::JobSystem::instance();
	const WaterPolygonSnapshot small = makeSnapshot(2, 2, false);
	const WaterPolygonSnapshot tiny = makeSnapshot(1, 1, false);
	const WaterPolygonSnapshot flat = makeSnapshot(50, 50, false);
	const WaterPolygonSnapshot wavy = makeSnapshot(50, 50, true);
	CHECK(invalidAndRangeChecks() == 0);
	for (unsigned workerIndex = 0;
		workerIndex < sizeof(workers)/sizeof(workers[0]); ++workerIndex)
	{
		system.shutdown();
		const unsigned effectiveWorkerCount =
			rts_test::ResolveActualWorkerCount(workers[workerIndex],
			localCapacity);
		rts_test::PrintWorkerCountSubstitution("water polygon",
			workers[workerIndex], effectiveWorkerCount, localCapacity);
		rts::JobSystemConfig config = rts::JobSystem::startupConfig();
		config.workerCount = effectiveWorkerCount;
		config.queueCapacity = 1024;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		CHECK(system.start(config));
		CHECK(checkCase(tiny, true) == 0);
		CHECK(checkCase(small, true) == 0);
		const rts::JobMetricCounter submittedBeforeFlat =
			system.metrics().submittedJobCount;
		CHECK(checkCase(flat, true) == 0);
		CHECK(system.metrics().submittedJobCount > submittedBeforeFlat);
		CHECK(checkCase(wavy, true) == 0);
		CHECK(failureAndCancellation(flat) == 0);
		CHECK(system.outstandingJobCount() == 0);
		printf("water polygon workers=%u actual=%u\n", workers[workerIndex],
			system.workerCount());
		system.shutdown();
	}
	return failures == 0 ? 0 : 1;
}
