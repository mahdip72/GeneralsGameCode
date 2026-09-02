#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/ProjectedTerrainGridKernel.h"
#include "../TestSupport/LocalCapacityTestLane.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#if defined(RTS_BUILD_CORE_EXTRAS)
enum ProjectedTerrainGridJobSystemTestEvent
{
	PROJECTED_GRID_JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH = 6
};
extern "C" void rts_job_system_set_test_fault(unsigned event,
	unsigned occurrence);
extern "C" void rts_radar_terrain_prepare_set_test_fault(
	unsigned fault, unsigned occurrence);
#endif

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

class ProjectedTerrainGridWork : public RadarPrepareRowWork
{
public:
	ProjectedTerrainGridWork(const ProjectedTerrainGridSnapshot &snapshot,
		ProjectedTerrainGridVertex *vertices, UnsignedShort *indices)
		: m_snapshot(snapshot), m_vertices(vertices), m_indices(indices)
	{
	}
	virtual unsigned minimumRowsPerTask() const
	{
		return m_snapshot.cellWidth == 0 ? 1 :
			(PROJECTED_TERRAIN_GRID_MIN_PARALLEL_CELLS +
			 m_snapshot.cellWidth - 1) / m_snapshot.cellWidth;
	}
	virtual bool executeRows(unsigned begin, unsigned end)
	{
		return PrepareProjectedTerrainGridRows(m_snapshot, m_vertices,
			m_indices, begin, end);
	}
private:
	ProjectedTerrainGridWork(const ProjectedTerrainGridWork &);
	ProjectedTerrainGridWork &operator=(const ProjectedTerrainGridWork &);
	ProjectedTerrainGridSnapshot m_snapshot;
	ProjectedTerrainGridVertex *m_vertices;
	UnsignedShort *m_indices;
};

struct Fixture
{
	Fixture(unsigned requestedWidth, unsigned requestedHeight,
		unsigned requestedKind)
		: width(requestedWidth), height(requestedHeight), kind(requestedKind),
		  expectedVertices(requestedWidth * requestedHeight),
		  expectedIndices((requestedWidth - 1) * (requestedHeight - 1) * 6)
	{
		memset(&snapshot, 0, sizeof(snapshot));
	}

	unsigned width;
	unsigned height;
	unsigned kind;
	ProjectedTerrainGridScratch scratch;
	ProjectedTerrainGridSnapshot snapshot;
	std::vector<ProjectedTerrainGridVertex> expectedVertices;
	std::vector<UnsignedShort> expectedIndices;
};

/* The test fixture intentionally uses a non-square tile and alternating flips
 * so row partitioning and both legacy triangle windings are exercised. */
static int initializeFixture(Fixture *fixture)
{
	unsigned row;
	unsigned column;
	CHECK(fixture != 0);
	CHECK(fixture->scratch.ensure(fixture->width, fixture->height));
	memset(&fixture->snapshot, 0, sizeof(fixture->snapshot));
	fixture->snapshot.width = fixture->width;
	fixture->snapshot.height = fixture->height;
	fixture->snapshot.cellWidth = fixture->width - 1;
	fixture->snapshot.cellHeight = fixture->height - 1;
	fixture->snapshot.kind = fixture->kind;
	fixture->snapshot.firstMapX = 7;
	fixture->snapshot.firstMapY = 11;
	fixture->snapshot.coordinateBiasX = fixture->kind ==
		PROJECTED_TERRAIN_GRID_DECAL ? 3 : 0;
	fixture->snapshot.coordinateBiasY = fixture->kind ==
		PROJECTED_TERRAIN_GRID_DECAL ? 3 : 0;
	fixture->snapshot.mapXYFactor = 10.0f;
	fixture->snapshot.mapHeightScale = 0.5f;
	fixture->snapshot.clampToLayerHeight = fixture->kind ==
		PROJECTED_TERRAIN_GRID_DECAL ? 1 : 0;
	fixture->snapshot.layerHeight = 42.0f;
	fixture->snapshot.heightBias = 0.1f;
	fixture->snapshot.diffuse = 0xA1B2C3D4u;
	fixture->snapshot.uAxisX = 0.01f;
	fixture->snapshot.uAxisY = -0.02f;
	fixture->snapshot.vAxisX = 0.03f;
	fixture->snapshot.vAxisY = 0.04f;
	fixture->snapshot.objectX = 61.0f;
	fixture->snapshot.objectY = 95.0f;
	fixture->snapshot.uOffset = 0.25f;
	fixture->snapshot.vOffset = 0.75f;
	fixture->snapshot.heights = fixture->scratch.heights();
	fixture->snapshot.flips = fixture->scratch.flips();

	for (row = 0; row < fixture->height; ++row)
	{
		for (column = 0; column < fixture->width; ++column)
			fixture->scratch.heights()[row * fixture->width + column] =
				static_cast<Real>((row * 13 + column * 7) % 251);
	}
	for (row = 0; row < fixture->height - 1; ++row)
	{
		for (column = 0; column < fixture->width - 1; ++column)
			fixture->scratch.flips()[row * (fixture->width - 1) + column] =
				static_cast<UnsignedByte>((row + column) & 1);
	}
	return 0;
}

/* Independent owner-side oracle for the legacy vertex/index formulas.  The
 * production kernel is then checked against this before the scheduler path
 * is compared, so a split-row implementation cannot agree with itself while
 * drifting from the original winding, height, UV, or color contract. */
static int prepareReference(Fixture *fixture)
{
	unsigned row;
	unsigned column;
	CHECK(fixture != 0);
	for (row = 0; row < fixture->height; ++row)
	{
		for (column = 0; column < fixture->width; ++column)
		{
			const unsigned vertexIndex = row * fixture->width + column;
			const Int mapX = fixture->snapshot.firstMapX +
				static_cast<Int>(column) - fixture->snapshot.coordinateBiasX;
			const Int mapY = fixture->snapshot.firstMapY +
				static_cast<Int>(row) - fixture->snapshot.coordinateBiasY;
			const Real x = static_cast<Real>(mapX) *
				fixture->snapshot.mapXYFactor;
			const Real y = static_cast<Real>(mapY) *
				fixture->snapshot.mapXYFactor;
			const Real height = fixture->scratch.heights()[vertexIndex] *
				fixture->snapshot.mapHeightScale;
			ProjectedTerrainGridVertex &vertex =
				fixture->expectedVertices[vertexIndex];
			vertex.x = x;
			vertex.y = y;
			vertex.z = height;
			vertex.diffuse = 0;
			vertex.u = 0.0f;
			vertex.v = 0.0f;
			if (fixture->snapshot.kind == PROJECTED_TERRAIN_GRID_DECAL)
			{
				if (fixture->snapshot.clampToLayerHeight)
					vertex.z = height > fixture->snapshot.layerHeight ?
						height : fixture->snapshot.layerHeight;
				else
					vertex.z = height + fixture->snapshot.heightBias;
				vertex.diffuse = fixture->snapshot.diffuse;
				vertex.u = fixture->snapshot.uAxisX *
					(x - fixture->snapshot.objectX) +
					fixture->snapshot.uAxisY *
					(y - fixture->snapshot.objectY) +
					fixture->snapshot.uOffset;
				vertex.v = fixture->snapshot.vAxisX *
					(x - fixture->snapshot.objectX) +
					fixture->snapshot.vAxisY *
					(y - fixture->snapshot.objectY) +
					fixture->snapshot.vOffset;
			}
		}
	}
	for (row = 0; row < fixture->height - 1; ++row)
	{
		for (column = 0; column < fixture->width - 1; ++column)
		{
			const unsigned cellIndex = (row * (fixture->width - 1) +
				column) * 6;
			const unsigned vertex = row * fixture->width + column;
			const unsigned nextRow = vertex + fixture->width;
			UnsignedShort *indices = &fixture->expectedIndices[cellIndex];
			if (fixture->scratch.flips()[row * (fixture->width - 1) +
				column] != 0)
			{
				indices[0] = static_cast<UnsignedShort>(vertex + 1);
				indices[1] = static_cast<UnsignedShort>(nextRow);
				indices[2] = static_cast<UnsignedShort>(vertex);
				indices[3] = static_cast<UnsignedShort>(vertex + 1);
				indices[4] = static_cast<UnsignedShort>(nextRow + 1);
				indices[5] = static_cast<UnsignedShort>(nextRow);
			}
			else
			{
				indices[0] = static_cast<UnsignedShort>(vertex);
				indices[1] = static_cast<UnsignedShort>(nextRow + 1);
				indices[2] = static_cast<UnsignedShort>(nextRow);
				indices[3] = static_cast<UnsignedShort>(vertex);
				indices[4] = static_cast<UnsignedShort>(vertex + 1);
				indices[5] = static_cast<UnsignedShort>(nextRow + 1);
			}
		}
	}
	return 0;
}

static int comparePrepared(Fixture *fixture);

static int prepareSerial(Fixture *fixture)
{
	CHECK(prepareReference(fixture) == 0);
	CHECK(PrepareProjectedTerrainGridRows(fixture->snapshot,
		&fixture->scratch.vertices()[0], &fixture->scratch.indices()[0],
		0, fixture->height));
	CHECK(ValidatePreparedProjectedTerrainGridOutput(fixture->snapshot,
		fixture->scratch.vertices(), fixture->scratch.indices()));
	CHECK(comparePrepared(fixture) == 0);
	return 0;
}

static int comparePrepared(Fixture *fixture)
{
	CHECK(memcmp(fixture->scratch.vertices(), &fixture->expectedVertices[0],
		fixture->expectedVertices.size() * sizeof(ProjectedTerrainGridVertex)) == 0);
	CHECK(memcmp(fixture->scratch.indices(), &fixture->expectedIndices[0],
		fixture->expectedIndices.size() * sizeof(UnsignedShort)) == 0);
	return 0;
}

static int prepareThroughService(Fixture *fixture,
	RadarTerrainPrepareService &service, unsigned consumer, bool *parallel)
{
	ProjectedTerrainGridWork work(fixture->snapshot,
		fixture->scratch.vertices(), fixture->scratch.indices());
	memset(fixture->scratch.vertices(), 0xA5,
		fixture->expectedVertices.size() * sizeof(ProjectedTerrainGridVertex));
	memset(fixture->scratch.indices(), 0xA5,
		fixture->expectedIndices.size() * sizeof(UnsignedShort));
	CHECK(service.tryAcquire(consumer));
	CHECK(service.runRows(work, 0, fixture->height, parallel));
	service.release(consumer);
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
	CHECK(!service.hasLease());
	CHECK(ValidatePreparedProjectedTerrainGridOutput(fixture->snapshot,
		fixture->scratch.vertices(), fixture->scratch.indices()));
	return comparePrepared(fixture);
}

static int checkExplicitWinding(Fixture *fixture)
{
	CHECK(fixture->expectedIndices.size() >= 12);
	/* Cell (0,0) is unflipped; cell (1,0) is flipped. */
	CHECK(fixture->expectedIndices[0] == 0);
	CHECK(fixture->expectedIndices[1] == fixture->width + 1);
	CHECK(fixture->expectedIndices[2] == fixture->width);
	CHECK(fixture->expectedIndices[6] == 2);
	CHECK(fixture->expectedIndices[7] == fixture->width + 1);
	CHECK(fixture->expectedIndices[8] == 1);
	return 0;
}

static int kernelParity(RadarTerrainPrepareService *service,
	bool *sawMultiRange)
{
	const unsigned kinds[] = {
		PROJECTED_TERRAIN_GRID_SHADOW, PROJECTED_TERRAIN_GRID_DECAL
	};
	unsigned kind;
	if (sawMultiRange != 0)
		*sawMultiRange = false;
	for (kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); ++kind)
	{
		Fixture fixture(65, 33, kinds[kind]);
		CHECK(initializeFixture(&fixture) == 0);
		CHECK(prepareSerial(&fixture) == 0);
		CHECK(checkExplicitWinding(&fixture) == 0);
		if (service != 0)
		{
			bool ranParallel = false;
			CHECK(prepareThroughService(&fixture, *service, 80 + kind,
				&ranParallel) == 0);
			if (sawMultiRange != 0 && ranParallel)
				*sawMultiRange = true;
		}
		else
		{
			memset(fixture.scratch.vertices(), 0xA5,
				fixture.expectedVertices.size() * sizeof(ProjectedTerrainGridVertex));
			memset(fixture.scratch.indices(), 0xA5,
				fixture.expectedIndices.size() * sizeof(UnsignedShort));
			CHECK(PrepareProjectedTerrainGridRows(fixture.snapshot,
				fixture.scratch.vertices(), fixture.scratch.indices(),
				0, fixture.height));
			CHECK(comparePrepared(&fixture) == 0);
		}

		/* Layer clamping and non-layer bias are distinct legacy branches. */
		if (kinds[kind] == PROJECTED_TERRAIN_GRID_DECAL)
		{
			fixture.snapshot.clampToLayerHeight = 0;
			CHECK(PrepareProjectedTerrainGridRows(fixture.snapshot,
				fixture.scratch.vertices(), fixture.scratch.indices(), 0,
				fixture.height));
			CHECK(fixture.scratch.vertices()[0].z ==
				fixture.scratch.heights()[0] * fixture.snapshot.mapHeightScale +
				fixture.snapshot.heightBias);
		}
	}
	return 0;
}

static int invalidAndReuse()
{
	Fixture fixture(65, 33, PROJECTED_TERRAIN_GRID_DECAL);
	ProjectedTerrainGridSnapshot invalid;
	ProjectedTerrainGridVertex *savedVertices;
	CHECK(initializeFixture(&fixture) == 0);
	savedVertices = fixture.scratch.vertices();
	CHECK(fixture.scratch.ensure(65, 33));
	CHECK(savedVertices == fixture.scratch.vertices());
	CHECK(!fixture.scratch.ensure(65536, 65536));
	CHECK(!ValidateProjectedTerrainGridInput(fixture.snapshot,
		fixture.scratch.vertices(), 0));
	invalid = fixture.snapshot;
	invalid.kind = 99;
	CHECK(!ValidateProjectedTerrainGridInput(invalid,
		fixture.scratch.vertices(), fixture.scratch.indices()));
	invalid = fixture.snapshot;
	invalid.cellWidth = 0;
	CHECK(!ValidateProjectedTerrainGridInput(invalid,
		fixture.scratch.vertices(), fixture.scratch.indices()));
	return 0;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
static int faultFallback(RadarTerrainPrepareService &service)
{
	Fixture fixture(65, 33, PROJECTED_TERRAIN_GRID_DECAL);
	CHECK(initializeFixture(&fixture) == 0);
	CHECK(prepareSerial(&fixture) == 0);
	ProjectedTerrainGridWork workStorage(fixture.snapshot,
		fixture.scratch.vertices(), fixture.scratch.indices());
	memset(fixture.scratch.vertices(), 0xA5,
		fixture.expectedVertices.size() * sizeof(ProjectedTerrainGridVertex));
	memset(fixture.scratch.indices(), 0xA5,
		fixture.expectedIndices.size() * sizeof(UnsignedShort));
	CHECK(service.tryAcquire(91));
	rts_job_system_set_test_fault(PROJECTED_GRID_JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH, 1);
	rts_radar_terrain_prepare_set_test_fault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 1);
	CHECK(!service.runRows(workStorage, 0, fixture.height));
	rts_job_system_set_test_fault(0, 0);
	rts_radar_terrain_prepare_set_test_fault(0, 0);
	/* The owner must replace every partial byte with the complete reference. */
	CHECK(PrepareProjectedTerrainGridRows(fixture.snapshot,
		fixture.scratch.vertices(), fixture.scratch.indices(), 0,
		fixture.height));
	service.release(91);
	CHECK(!service.hasLease());
	CHECK(comparePrepared(&fixture) == 0);
	CHECK(rts::JobSystem::instance().outstandingJobCount() == 0);
	return 0;
}
#endif

static int serialPolicy()
{
	CHECK(rts::SetPipelineExecutionMode("serial"));
	CHECK(kernelParity(0, 0) == 0);
	CHECK(invalidAndReuse() == 0);
	CHECK(rts::JobSystem::instance().metrics().submittedJobCount == 0);
	puts("projected terrain grid serial policy uses no compute jobs");
	return 0;
}

int main(int argc, char **argv)
{
	bool localCapacity = false;
	bool serialPipelines = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity,
		&serialPipelines, "--serial"))
	{
		fprintf(stderr, "Usage: projected_terrain_grid_tests [--local-capacity] [--serial]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	const unsigned workers[] = { 1, 2, 4, 8, 16, 0 };
	unsigned worker;
	rts::JobSystem &system = rts::JobSystem::instance();
	if (serialPipelines)
		return serialPolicy();

	for (worker = 0; worker < sizeof(workers) / sizeof(workers[0]); ++worker)
	{
		const unsigned effectiveWorkerCount =
			rts_test::ResolveActualWorkerCount(workers[worker], localCapacity);
		rts_test::PrintWorkerCountSubstitution("projected terrain grid",
			workers[worker], effectiveWorkerCount, localCapacity);
		rts::JobSystemConfig config = rts::JobSystem::startupConfig();
		config.workerCount = effectiveWorkerCount;
		config.queueCapacity = 128;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		CHECK(system.start(config));
		RadarTerrainPrepareService service;
		bool sawMultiRange = false;
		CHECK(service.initialize(16, 128));
		CHECK(kernelParity(&service, &sawMultiRange) == 0);
		CHECK(invalidAndReuse() == 0);
#if defined(RTS_BUILD_CORE_EXTRAS)
		CHECK(faultFallback(service) == 0);
#endif
		{
			const rts::JobSystemMetrics metrics = system.metrics();
			const unsigned expectedRanges = rts::JobSystem::chooseRangeCount(
				33, 8, system.workerCount());
			printf("projected-grid workers=%u actual=%u ranges=%u multi-range=%u submitted=%u observed-active=%u fallback=%u\n",
				workers[worker], system.workerCount(),
				expectedRanges, sawMultiRange ? 1u : 0u,
				static_cast<unsigned>(metrics.submittedJobCount),
				metrics.maximumActiveWorkers,
				static_cast<unsigned>(metrics.serialFallbackCount));
			if (workers[worker] >= 4 && system.workerCount() > 2)
			{
				/* The completed byte oracle proves every submitted range ran;
				 * JobSystem tests own the separate simultaneous-worker proof. */
				CHECK(expectedRanges > 2);
				CHECK(sawMultiRange);
			}
		}
		service.shutdown();
		CHECK(system.outstandingJobCount() == 0);
		system.shutdown();
	}
	CHECK(invalidAndReuse() == 0);
	puts("projected terrain grid POD parity, winding, bounds and fallback checks passed");
	return 0;
}
