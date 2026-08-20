#include "Lib/HeightMapTerrainKernel.h"
#include "W3DDevice/Common/HeightMapTerrainPrepare.h"

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
enum HeightMapTerrainTaskRuntimeTestEvent
{
	HEIGHTMAP_TERRAIN_TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE = 4,
	HEIGHTMAP_TERRAIN_TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH = 5
};
extern "C" void rts_task_runtime_set_test_allocation_fault(
	unsigned event, unsigned occurrence);
#endif

static int check(bool value, const char *testName, const char *expression)
{
	if (!value)
	{
		fprintf(stderr, "%s: %s\n", testName, expression);
		return 1;
	}
	return 0;
}

#define CHECK(testName, expression) \
	do { if (check((expression), testName, #expression) != 0) return 1; } while (0)

static const unsigned kGuardBytes = 8;
static const unsigned kMaxCells = 16;
static const unsigned kOutputStorageBytes = 16384;
static const unsigned kSourceWidth = 4;
static const unsigned kSourceHeight = 4;

enum
{
	SOURCE_WRAP_X = 1,
	SOURCE_WRAP_Y = 2
};

union OutputStorage
{
	HeightMapTerrainVertex aligned[512];
	unsigned char bytes[kOutputStorageBytes];
};

struct TerrainFixture
{
	HeightMapTerrainSnapshot snapshot;
	HeightMapTerrainCellInput cells[kMaxCells];
	UnsignedByte source[kSourceWidth * kSourceHeight];
	HeightMapTerrainGlobalLight globalLights[4];
	HeightMapTerrainSceneLight sceneLights[4];
};

union CellAliasStorage
{
	HeightMapTerrainCellInput aligned[8];
	unsigned char bytes[1024];
};

static void setRgb(HeightMapTerrainRgb *rgb, Real red, Real green, Real blue)
{
	rgb->red = red;
	rgb->green = green;
	rgb->blue = blue;
}

/*
 * These fixtures record the legacy updateVB contract before a kernel exists:
 * each cell writes top-left, top-right, bottom-right, bottom-left; each
 * normal uses the left/right and back/forward height neighbors in that same
 * order; UV1/UV2 and alpha follow the four captured corners; FLIP_TRIANGLES
 * rotates complete vertices after lighting; doTheLight starts with terrain
 * ambient, visits scene lights in iterator order, adds global diffuse lights,
 * clamps channels, then applies depth fade at or below the water threshold.
 * Impassable/cliff visualization masks are owner-only and therefore remain
 * post-flip diagnostics rather than worker inputs.
 */
static int wrapSourceIndex(int index, unsigned limit)
{
	while (index < 0)
		index += (int)limit;
	while (index >= (int)limit)
		index -= (int)limit;
	return index;
}

static UnsignedByte sourceHeight(const TerrainFixture *fixture, int x, int y,
	unsigned mapping)
{
	if (mapping & SOURCE_WRAP_X)
		x = wrapSourceIndex(x, kSourceWidth);
	else if (x < 0)
		x = 0;
	else if (x >= (int)kSourceWidth)
		x = kSourceWidth - 1;

	if (mapping & SOURCE_WRAP_Y)
		y = wrapSourceIndex(y, kSourceHeight);
	else if (y < 0)
		y = 0;
	else if (y >= (int)kSourceHeight)
		y = kSourceHeight - 1;

	return fixture->source[y * kSourceWidth + x];
}

static void captureCellHeights(const TerrainFixture *fixture,
	HeightMapTerrainCellInput *cell, int sourceX, int sourceY,
	unsigned mapping)
{
	cell->vertexHeight[0] = sourceHeight(fixture, sourceX, sourceY, mapping);
	cell->vertexHeight[1] = sourceHeight(fixture, sourceX + 1, sourceY,
		mapping);
	cell->vertexHeight[2] = sourceHeight(fixture, sourceX + 1, sourceY + 1,
		mapping);
	cell->vertexHeight[3] = sourceHeight(fixture, sourceX, sourceY + 1,
		mapping);

	cell->leftRightHeightDelta[0] =
		(Int)sourceHeight(fixture, sourceX + 1, sourceY, mapping) -
			(Int)sourceHeight(fixture, sourceX - 1, sourceY, mapping);
	cell->backForwardHeightDelta[0] =
		(Int)sourceHeight(fixture, sourceX, sourceY + 1, mapping) -
			(Int)sourceHeight(fixture, sourceX, sourceY - 1, mapping);
	cell->leftRightHeightDelta[1] =
		(Int)sourceHeight(fixture, sourceX + 2, sourceY, mapping) -
			(Int)sourceHeight(fixture, sourceX, sourceY, mapping);
	cell->backForwardHeightDelta[1] =
		(Int)sourceHeight(fixture, sourceX + 1, sourceY + 1, mapping) -
			(Int)sourceHeight(fixture, sourceX + 1, sourceY - 1, mapping);
	cell->leftRightHeightDelta[2] =
		(Int)sourceHeight(fixture, sourceX + 2, sourceY + 1, mapping) -
			(Int)sourceHeight(fixture, sourceX, sourceY + 1, mapping);
	cell->backForwardHeightDelta[2] =
		(Int)sourceHeight(fixture, sourceX + 1, sourceY + 2, mapping) -
			(Int)sourceHeight(fixture, sourceX + 1, sourceY, mapping);
	cell->leftRightHeightDelta[3] =
		(Int)sourceHeight(fixture, sourceX + 1, sourceY + 1, mapping) -
			(Int)sourceHeight(fixture, sourceX - 1, sourceY + 1, mapping);
	cell->backForwardHeightDelta[3] =
		(Int)sourceHeight(fixture, sourceX, sourceY + 2, mapping) -
			(Int)sourceHeight(fixture, sourceX, sourceY, mapping);
}

static void refreshBatchBytes(TerrainFixture *fixture)
{
	fixture->snapshot.batchBytes =
		fixture->snapshot.cellRowStrideBytes * fixture->snapshot.height +
		fixture->snapshot.globalLightCount *
			sizeof(HeightMapTerrainGlobalLight) +
		fixture->snapshot.sceneLightCount *
			sizeof(HeightMapTerrainSceneLight) +
		fixture->snapshot.outputCapacityBytes;
}

static void initializeFixture(TerrainFixture *fixture, unsigned width,
	unsigned height, Real baseX, Real baseY, int flipCell)
{
	static const UnsignedByte defaultSource[kSourceWidth * kSourceHeight] = {
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0
	};
	unsigned cellIndex;
	const unsigned outputPayloadBytes = width *
		HEIGHTMAP_TERRAIN_VERTEX_COUNT * sizeof(HeightMapTerrainVertex);
	const unsigned outputStrideBytes = outputPayloadBytes +
		2 * sizeof(HeightMapTerrainVertex);

	memset(fixture, 0, sizeof(*fixture));
	for (cellIndex = 0; cellIndex < width * height; ++cellIndex)
	{
		const unsigned cellX = cellIndex % width;
		const unsigned cellY = cellIndex / width;
		HeightMapTerrainCellInput *cell = &fixture->cells[cellIndex];
		const Real x0 = baseX + (Real)(cellX * 10);
		const Real y0 = baseY + (Real)(cellY * 10);
		unsigned corner;

		cell->x[0] = x0;
		cell->y[0] = y0;
		cell->x[1] = x0 + 10.0f;
		cell->y[1] = y0;
		cell->x[2] = x0 + 10.0f;
		cell->y[2] = y0 + 10.0f;
		cell->x[3] = x0;
		cell->y[3] = y0 + 10.0f;
		for (corner = 0; corner < 4; ++corner)
		{
			cell->u1[corner] = 0.10f + (Real)(cellIndex * 4 + corner) * 0.01f;
			cell->v1[corner] = 0.20f + (Real)(cellIndex * 4 + corner) * 0.01f;
			cell->u2[corner] = 0.30f + (Real)(cellIndex * 4 + corner) * 0.01f;
			cell->v2[corner] = 0.40f + (Real)(cellIndex * 4 + corner) * 0.01f;
			cell->alpha[corner] = (UnsignedByte)(0x10 + corner + cellIndex);
		}
		cell->flip = cellIndex == (unsigned)flipCell ? 1 : 0;
	}
	memcpy(fixture->source, defaultSource, sizeof(defaultSource));
	for (cellIndex = 0; cellIndex < width * height; ++cellIndex)
	{
		const unsigned cellX = cellIndex % width;
		const unsigned cellY = cellIndex / width;
		captureCellHeights(fixture, &fixture->cells[cellIndex],
			(int)cellX + 1, (int)cellY + 1, 0);
	}

	fixture->snapshot.width = width;
	fixture->snapshot.height = height;
	fixture->snapshot.cellRowStrideBytes =
		width * sizeof(HeightMapTerrainCellInput);
	fixture->snapshot.cellCount = width * height;
	fixture->snapshot.cells = fixture->cells;
	fixture->snapshot.mapXYFactor = 1.0f;
	fixture->snapshot.mapHeightScale = 1.0f;
	setRgb(&fixture->snapshot.terrainAmbient, 0.0f, 0.0f, 0.0f);
	fixture->snapshot.globalLights = 0;
	fixture->snapshot.globalLightCount = 0;
	fixture->snapshot.sceneLights = 0;
	fixture->snapshot.sceneLightCount = 0;
	fixture->snapshot.useDepthFade = 0;
	fixture->snapshot.depthFadeR = 0.0f;
	fixture->snapshot.depthFadeG = 0.0f;
	fixture->snapshot.depthFadeB = 0.0f;
	fixture->snapshot.waterHeight = 0.0f;
	fixture->snapshot.outputStrideBytes = outputStrideBytes;
	fixture->snapshot.outputCapacityBytes = height * outputStrideBytes;
	refreshBatchBytes(fixture);
}

static HeightMapTerrainCellInput *fixtureCell(TerrainFixture *fixture,
	unsigned row, unsigned column)
{
	return reinterpret_cast<HeightMapTerrainCellInput *>(
		reinterpret_cast<unsigned char *>(fixture->cells) +
		row * fixture->snapshot.cellRowStrideBytes +
		column * sizeof(HeightMapTerrainCellInput));
}

static void initializePaddedFixture(TerrainFixture *fixture)
{
	HeightMapTerrainCellInput contiguous[4];
	const unsigned paddedStride = 2 * sizeof(HeightMapTerrainCellInput) +
		sizeof(Real);

	initializeFixture(fixture, 2, 2, 0.0f, 0.0f, -1);
	memcpy(contiguous, fixture->cells, sizeof(contiguous));
	memset(fixture->cells, 0, sizeof(fixture->cells));
	memcpy(reinterpret_cast<unsigned char *>(fixture->cells), contiguous,
		2 * sizeof(HeightMapTerrainCellInput));
	memcpy(reinterpret_cast<unsigned char *>(fixture->cells) + paddedStride,
		contiguous + 2, 2 * sizeof(HeightMapTerrainCellInput));
	fixture->snapshot.cellRowStrideBytes = paddedStride;
	refreshBatchBytes(fixture);
}

static unsigned char *outputBytes(OutputStorage *storage)
{
	return storage->bytes + kGuardBytes;
}

static HeightMapTerrainVertex *outputVertices(OutputStorage *storage)
{
	return reinterpret_cast<HeightMapTerrainVertex *>(outputBytes(storage));
}

static void initializeOutput(OutputStorage *storage)
{
	memset(storage->bytes, 0xA5, sizeof(storage->bytes));
}

static int checkOutputGuards(const OutputStorage *storage,
	const TerrainFixture *fixture, const char *testName)
{
	const unsigned payloadBytes = fixture->snapshot.width *
		HEIGHTMAP_TERRAIN_VERTEX_COUNT * sizeof(HeightMapTerrainVertex);
	const unsigned stride = fixture->snapshot.outputStrideBytes;
	const unsigned height = fixture->snapshot.height;
	unsigned index;

	for (index = 0; index < fixture->snapshot.outputCapacityBytes +
		2 * kGuardBytes; ++index)
	{
		bool payload = false;
		unsigned row;
		for (row = 0; row < height; ++row)
		{
			const unsigned rowStart = kGuardBytes + row * stride;
			if (index >= rowStart && index < rowStart + payloadBytes)
			{
				payload = true;
				break;
			}
		}
		if (!payload && storage->bytes[index] != 0xA5)
		{
			fprintf(stderr, "%s: guard byte %u was %u\n", testName,
				index, (unsigned)storage->bytes[index]);
			return 1;
		}
	}
	return 0;
}

static int checkUnassignedRows(const OutputStorage *storage,
	const TerrainFixture *fixture, unsigned assignedBegin,
	unsigned assignedEnd, const char *testName)
{
	const unsigned payloadBytes = fixture->snapshot.width *
		HEIGHTMAP_TERRAIN_VERTEX_COUNT * sizeof(HeightMapTerrainVertex);
	const unsigned stride = fixture->snapshot.outputStrideBytes;
	unsigned row;

	for (row = 0; row < fixture->snapshot.height; ++row)
	{
		unsigned byteIndex;
		if (row >= assignedBegin && row < assignedEnd)
			continue;
		for (byteIndex = 0; byteIndex < payloadBytes; ++byteIndex)
		{
			const unsigned index = kGuardBytes + row * stride + byteIndex;
			if (storage->bytes[index] != 0xA5)
			{
				fprintf(stderr, "%s: unassigned row %u byte %u was %u\n",
					testName, row, byteIndex,
					(unsigned)storage->bytes[index]);
				return 1;
			}
		}
	}
	return 0;
}

static int testVertexLayout()
{
	const char *testName = "testVertexLayout";
	CHECK(testName, sizeof(HeightMapTerrainVertex) == 32);
	CHECK(testName, offsetof(HeightMapTerrainVertex, x) == 0);
	CHECK(testName, offsetof(HeightMapTerrainVertex, y) == 4);
	CHECK(testName, offsetof(HeightMapTerrainVertex, z) == 8);
	CHECK(testName, offsetof(HeightMapTerrainVertex, diffuse) == 12);
	CHECK(testName, offsetof(HeightMapTerrainVertex, u1) == 16);
	CHECK(testName, offsetof(HeightMapTerrainVertex, v1) == 20);
	CHECK(testName, offsetof(HeightMapTerrainVertex, u2) == 24);
	CHECK(testName, offsetof(HeightMapTerrainVertex, v2) == 28);
	CHECK(testName, HEIGHTMAP_TERRAIN_LIGHT_POINT == 0);
	CHECK(testName, HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL == 1);
	CHECK(testName, HEIGHTMAP_TERRAIN_LIGHT_SPOT == 2);
	CHECK(testName, HEIGHTMAP_TERRAIN_MAX_GLOBAL_LIGHTS == 3);
	return 0;
}

static int compareFixtureRows(const TerrainFixture *fixture,
	const char *testName, bool checkFlipBytes)
{
	TerrainFixture serialFixture = *fixture;
	TerrainFixture splitFixture = *fixture;
	OutputStorage serialStorage;
	OutputStorage splitStorage;
	HeightMapTerrainVertex expected[4];
	unsigned corner;

	initializeOutput(&serialStorage);
	initializeOutput(&splitStorage);
	CHECK(testName, PrepareHeightMapTerrainRows(serialFixture.snapshot,
		outputVertices(&serialStorage), 0, serialFixture.snapshot.height));
	if (splitFixture.snapshot.height == 1)
	{
		CHECK(testName, PrepareHeightMapTerrainRows(splitFixture.snapshot,
			outputVertices(&splitStorage), 0, 1));
	}
	else
	{
		const unsigned split = splitFixture.snapshot.height / 2;
		CHECK(testName, PrepareHeightMapTerrainRows(splitFixture.snapshot,
			outputVertices(&splitStorage), 0, split));
		CHECK(testName, checkUnassignedRows(&splitStorage, &splitFixture, 0,
			split, testName) == 0);
		CHECK(testName, PrepareHeightMapTerrainRows(splitFixture.snapshot,
			outputVertices(&splitStorage), split,
			splitFixture.snapshot.height));
	}
	CHECK(testName, memcmp(serialStorage.bytes, splitStorage.bytes,
		sizeof(serialStorage.bytes)) == 0);
	CHECK(testName, checkOutputGuards(&serialStorage, &serialFixture,
		testName) == 0);
	CHECK(testName, checkOutputGuards(&splitStorage, &splitFixture,
		testName) == 0);

	if (checkFlipBytes)
	{
		const HeightMapTerrainCellInput &cell = serialFixture.cells[0];
		for (corner = 0; corner < 4; ++corner)
		{
			const unsigned source = (corner + 1) & 3;
			expected[corner].x = cell.x[source];
			expected[corner].y = cell.y[source];
			expected[corner].z = 0.0f;
			expected[corner].diffuse =
				(unsigned)cell.alpha[source] << 24;
			expected[corner].u1 = cell.u1[source];
			expected[corner].v1 = cell.v1[source];
			expected[corner].u2 = cell.u2[source];
			expected[corner].v2 = cell.v2[source];
		}
		CHECK(testName, memcmp(outputVertices(&serialStorage), expected,
			sizeof(expected)) == 0);
	}
	return 0;
}

static int testNamedLegacyGeometryFixtures()
{
	struct FixtureSpec
	{
		const char *name;
		unsigned width;
		unsigned height;
		Real baseX;
		Real baseY;
		int flipCell;
		bool checkFlipBytes;
	};
	const FixtureSpec fixtures[] = {
		{ "ordinary", 2, 4, 10.0f, 20.0f, -1, false },
		{ "flipped", 2, 4, 10.0f, 20.0f, 0, true },
		{ "border", 2, 4, 0.0f, 0.0f, -1, false },
		{ "non-zero-origin", 2, 4, 100.0f, -70.0f, -1, false },
		{ "odd-row", 3, 3, 30.0f, 40.0f, -1, false },
		{ "minimum-row", 1, 1, 0.0f, 0.0f, -1, false }
	};
	unsigned index;

	for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index)
	{
		TerrainFixture fixture;
		initializeFixture(&fixture, fixtures[index].width,
			fixtures[index].height, fixtures[index].baseX,
			fixtures[index].baseY, fixtures[index].flipCell);
		if (compareFixtureRows(&fixture, fixtures[index].name,
			fixtures[index].checkFlipBytes) != 0)
			return 1;
	}
	return 0;
}

/* Independent scalar oracle for the four legacy cross products. */
static Real scalarNormalZ(const TerrainFixture *fixture, unsigned corner)
{
	const int ox = 1;
	const int oy = 1;
	const Real span = 2.0f * fixture->snapshot.mapXYFactor;
	Int leftRight;
	Int backForward;
	Real crossX;
	Real crossY;
	Real crossZ;
	Real length;

	switch (corner)
	{
	case 0:
		leftRight = (Int)sourceHeight(fixture, ox + 1, oy, 0) -
			(Int)sourceHeight(fixture, ox - 1, oy, 0);
		backForward = (Int)sourceHeight(fixture, ox, oy + 1, 0) -
			(Int)sourceHeight(fixture, ox, oy - 1, 0);
		break;
	case 1:
		leftRight = (Int)sourceHeight(fixture, ox + 2, oy, 0) -
			(Int)sourceHeight(fixture, ox, oy, 0);
		backForward = (Int)sourceHeight(fixture, ox + 1, oy + 1, 0) -
			(Int)sourceHeight(fixture, ox + 1, oy - 1, 0);
		break;
	case 2:
		leftRight = (Int)sourceHeight(fixture, ox + 2, oy + 1, 0) -
			(Int)sourceHeight(fixture, ox, oy + 1, 0);
		backForward = (Int)sourceHeight(fixture, ox + 1, oy + 2, 0) -
			(Int)sourceHeight(fixture, ox + 1, oy, 0);
		break;
	default:
		leftRight = (Int)sourceHeight(fixture, ox + 1, oy + 1, 0) -
			(Int)sourceHeight(fixture, ox - 1, oy + 1, 0);
		backForward = (Int)sourceHeight(fixture, ox, oy + 2, 0) -
			(Int)sourceHeight(fixture, ox, oy, 0);
		break;
	}

	crossX = -fixture->snapshot.mapHeightScale *
		(Real)leftRight * span;
	crossY = -span * fixture->snapshot.mapHeightScale *
		(Real)backForward;
	crossZ = span * span;
	length = (Real)sqrt((double)(crossX * crossX + crossY * crossY +
		crossZ * crossZ));
	return crossZ / length;
}

static unsigned scalarGoldenChannel(Real normalZ, Real diffuse)
{
	Real shade = normalZ;
	if (shade > 1.0f)
		shade = 1.0f;
	if (shade < 0.0f)
		shade = 0.0f;
	return (unsigned)(shade * diffuse * 255.0f);
}

static HeightMapTerrainVertex makeScalarGoldenVertex(
	const TerrainFixture *fixture, unsigned corner)
{
	const HeightMapTerrainCellInput &cell = fixture->cells[0];
	const int x = 1 + ((corner == 1 || corner == 2) ? 1 : 0);
	const int y = 1 + ((corner >= 2) ? 1 : 0);
	const Real normalZ = scalarNormalZ(fixture, corner);
	const unsigned red = scalarGoldenChannel(normalZ,
		fixture->globalLights[0].diffuse.red);
	const unsigned green = scalarGoldenChannel(normalZ,
		fixture->globalLights[0].diffuse.green);
	const unsigned blue = scalarGoldenChannel(normalZ,
		fixture->globalLights[0].diffuse.blue);
	HeightMapTerrainVertex result;

	result.x = cell.x[corner];
	result.y = cell.y[corner];
	result.z = (Real)sourceHeight(fixture, x, y, 0) *
		fixture->snapshot.mapHeightScale;
	result.diffuse = (unsigned)cell.alpha[corner] << 24 |
		(red << 16) | (green << 8) | blue;
	result.u1 = cell.u1[corner];
	result.v1 = cell.v1[corner];
	result.u2 = cell.u2[corner];
	result.v2 = cell.v2[corner];
	return result;
}

static int testAsymmetricNonPlanarGoldenGeometry()
{
	const char *testName = "testAsymmetricNonPlanarGoldenGeometry";
	const UnsignedByte rawSource[16] = {
		1, 2, 4, 7,
		3, 5, 8, 13,
		6, 9, 14, 20,
		10, 15, 21, 28
	};
	const UnsignedByte expectedVertexHeight[4] = { 5, 8, 14, 9 };
	const Int expectedLeftRight[4] = { 5, 8, 11, 8 };
	const Int expectedBackForward[4] = { 7, 10, 13, 10 };
	TerrainFixture fixture;
	OutputStorage storage;
	HeightMapTerrainVertex expected[4];
	unsigned corner;

	/* The owner derives each capture from this independent asymmetric 4x4
	 * source table; the worker receives no shared map or halo. */
	initializeFixture(&fixture, 1, 1, 50.0f, -20.0f, -1);
	memcpy(fixture.source, rawSource, sizeof(rawSource));
	captureCellHeights(&fixture, &fixture.cells[0], 1, 1, 0);
	for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
	{
		CHECK(testName, fixture.cells[0].vertexHeight[corner] ==
			expectedVertexHeight[corner]);
		CHECK(testName, fixture.cells[0].leftRightHeightDelta[corner] ==
			expectedLeftRight[corner]);
		CHECK(testName, fixture.cells[0].backForwardHeightDelta[corner] ==
			expectedBackForward[corner]);
	}
	fixture.snapshot.mapXYFactor = 2.0f;
	fixture.snapshot.mapHeightScale = 0.5f;
	fixture.globalLights[0].rayX = 0.0f;
	fixture.globalLights[0].rayY = 0.0f;
	fixture.globalLights[0].rayZ = 1.0f;
	setRgb(&fixture.globalLights[0].diffuse, 0.37f, 0.23f, 0.17f);
	fixture.snapshot.globalLights = fixture.globalLights;
	fixture.snapshot.globalLightCount = 1;
	refreshBatchBytes(&fixture);
	for (corner = 0; corner < 4; ++corner)
		expected[corner] = makeScalarGoldenVertex(&fixture, corner);

	initializeOutput(&storage);
	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), 0, 1));
	CHECK(testName, memcmp(outputVertices(&storage), expected,
		sizeof(expected)) == 0);
	CHECK(testName, checkOutputGuards(&storage, &fixture, testName) == 0);
	return 0;
}

static int testCapturedWrapAndClampCells()
{
	const char *testName = "testCapturedWrapAndClampCells";
	static const UnsignedByte conflictingSource[16] = {
		5, 20, 40, 80,
		7, 22, 42, 82,
		9, 24, 44, 84,
		11, 26, 46, 86
	};
	static const UnsignedByte expectedWrapVertexHeight[4] = {
		82, 7, 9, 84
	};
	static const Int expectedWrapLeftRight[4] = {
		-35, -60, -60, -35
	};
	static const Int expectedWrapBackForward[4] = {
		4, 4, 4, 4
	};
	static const UnsignedByte expectedClampVertexHeight[4] = {
		82, 82, 84, 84
	};
	static const Int expectedClampLeftRight[4] = {
		40, 0, 0, 40
	};
	static const Int expectedClampBackForward[4] = {
		4, 4, 4, 4
	};
	TerrainFixture fixture;
	OutputStorage storage;
	HeightMapTerrainVertex *vertices;
	unsigned corner;

	initializeFixture(&fixture, 2, 1, 0.0f, 0.0f, -1);
	memcpy(fixture.source, conflictingSource, sizeof(conflictingSource));
	/* Both cells capture the same nominal source location.  The first uses
	 * the owner's wrapped x coordinate and the second uses the owner's
	 * clamped x coordinate, so their captured neighborhoods intentionally
	 * disagree at the source seam. */
	captureCellHeights(&fixture, &fixture.cells[0], 3, 1, SOURCE_WRAP_X);
	captureCellHeights(&fixture, &fixture.cells[1], 3, 1, 0);
	refreshBatchBytes(&fixture);
	initializeOutput(&storage);
	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), 0, 1));
	vertices = outputVertices(&storage);
	for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
	{
		CHECK(testName, fixture.cells[0].vertexHeight[corner] ==
			expectedWrapVertexHeight[corner]);
		CHECK(testName, fixture.cells[0].leftRightHeightDelta[corner] ==
			expectedWrapLeftRight[corner]);
		CHECK(testName, fixture.cells[0].backForwardHeightDelta[corner] ==
			expectedWrapBackForward[corner]);
		CHECK(testName, fixture.cells[1].vertexHeight[corner] ==
			expectedClampVertexHeight[corner]);
		CHECK(testName, fixture.cells[1].leftRightHeightDelta[corner] ==
			expectedClampLeftRight[corner]);
		CHECK(testName, fixture.cells[1].backForwardHeightDelta[corner] ==
			expectedClampBackForward[corner]);
		CHECK(testName, vertices[corner].z ==
			(Real)fixture.cells[0].vertexHeight[corner]);
		CHECK(testName, vertices[HEIGHTMAP_TERRAIN_VERTEX_COUNT + corner].z ==
			(Real)fixture.cells[1].vertexHeight[corner]);
	}
	CHECK(testName, checkOutputGuards(&storage, &fixture, testName) == 0);
	return 0;
}

static int testOwnerCaptureCellSeam()
{
	const char *testName = "testOwnerCaptureCellSeam";
	static const UnsignedByte source[30] = {
		1, 2, 3, 4, 5, 6,
		10, 20, 31, 43, 56, 70,
		80, 91, 103, 116, 130, 145,
		160, 171, 183, 197, 215, 225,
		240, 251, 252, 254, 255, 255
	};
	const Real u1[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
	const Real v1[4] = { 0.5f, 0.6f, 0.7f, 0.8f };
	const Real u2[4] = { 0.9f, 1.0f, 1.1f, 1.2f };
	const Real v2[4] = { 1.3f, 1.4f, 1.5f, 1.6f };
	const UnsignedByte alpha[4] = { 1, 2, 3, 4 };
	HeightMapTerrainCellInput cell;
	memset(&cell, 0xA5, sizeof(cell));
	CHECK(testName, CaptureHeightMapTerrainCellInput(cell, source, 6, 5,
		0, 0, 2, 2, 1, 4, 1, 4, 5, 6, 1, 2.0f,
		u1, v1, u2, v2, alpha, 1));
	CHECK(testName, cell.vertexHeight[0] == 103);
	CHECK(testName, cell.vertexHeight[1] == 116);
	CHECK(testName, cell.vertexHeight[2] == 197);
	CHECK(testName, cell.vertexHeight[3] == 183);
	CHECK(testName, cell.leftRightHeightDelta[0] == 25);
	CHECK(testName, cell.leftRightHeightDelta[1] == 27);
	CHECK(testName, cell.leftRightHeightDelta[2] == 32);
	CHECK(testName, cell.leftRightHeightDelta[3] == 26);
	CHECK(testName, cell.backForwardHeightDelta[0] == 152);
	CHECK(testName, cell.backForwardHeightDelta[1] == 154);
	CHECK(testName, cell.backForwardHeightDelta[2] == 138);
	CHECK(testName, cell.backForwardHeightDelta[3] == 149);
	CHECK(testName, cell.x[0] == 8.0f && cell.y[0] == 10.0f);
	CHECK(testName, cell.x[1] == 10.0f && cell.y[1] == 10.0f);
	CHECK(testName, cell.x[2] == 10.0f && cell.y[2] == 12.0f);
	CHECK(testName, cell.x[3] == 8.0f && cell.y[3] == 12.0f);
	CHECK(testName, cell.u1[2] == u1[2] && cell.v2[3] == v2[3]);
	CHECK(testName, cell.alpha[1] == 2 && cell.flip == 1);

	memset(&cell, 0x5A, sizeof(cell));
	CHECK(testName, !CaptureHeightMapTerrainCellInput(cell, source, 6, 5,
		0, 0, 2, 2, -1, 4, 1, 4, 5, 6, 1, 2.0f,
		u1, v1, u2, v2, alpha, 0));

	/* X-edge clamp: keep the left sample asymmetric while preserving the
	 * right and forward halo. */
	CHECK(testName, CaptureHeightMapTerrainCellInput(cell, source, 6, 5,
		0, 0, 0, 2, 0, 2, 1, 4, 2, 3, 0, 1.0f,
		u1, v1, u2, v2, alpha, 0));
	CHECK(testName, cell.vertexHeight[0] == 80 && cell.vertexHeight[1] == 91 &&
		cell.vertexHeight[2] == 171 && cell.vertexHeight[3] == 160);
	CHECK(testName, cell.leftRightHeightDelta[0] == 11 &&
		cell.leftRightHeightDelta[1] == 23 &&
		cell.leftRightHeightDelta[2] == 23 &&
		cell.leftRightHeightDelta[3] == 11);
	CHECK(testName, cell.backForwardHeightDelta[0] == 150 &&
		cell.backForwardHeightDelta[1] == 151 &&
		cell.backForwardHeightDelta[2] == 160 &&
		cell.backForwardHeightDelta[3] == 160);

	/* Y-edge clamp: top and bottom halos are intentionally different from the
	 * X-edge case so a transposed or duplicated stencil cannot pass. */
	CHECK(testName, CaptureHeightMapTerrainCellInput(cell, source, 6, 5,
		0, 0, 2, 0, 1, 4, 0, 2, 5, 6, 0, 1.0f,
		u1, v1, u2, v2, alpha, 0));
	CHECK(testName, cell.vertexHeight[0] == 3 && cell.vertexHeight[1] == 4 &&
		cell.vertexHeight[2] == 43 && cell.vertexHeight[3] == 31);
	CHECK(testName, cell.leftRightHeightDelta[0] == 2 &&
		cell.leftRightHeightDelta[1] == 2 &&
		cell.leftRightHeightDelta[2] == 25 &&
		cell.leftRightHeightDelta[3] == 23);
	CHECK(testName, cell.backForwardHeightDelta[0] == 28 &&
		cell.backForwardHeightDelta[1] == 39 &&
		cell.backForwardHeightDelta[2] == 112 &&
		cell.backForwardHeightDelta[3] == 100);

	/* Both origins are non-zero; the expected values are from physical source
	 * coordinates, not from a helper-derived oracle. */
	CHECK(testName, CaptureHeightMapTerrainCellInput(cell, source, 6, 5,
		1, 1, 1, 1, 0, 3, 0, 3, 4, 5, 1, 2.0f,
		u1, v1, u2, v2, alpha, 1));
	CHECK(testName, cell.vertexHeight[0] == 103 && cell.vertexHeight[1] == 116 &&
		cell.vertexHeight[2] == 197 && cell.vertexHeight[3] == 183);
	CHECK(testName, cell.leftRightHeightDelta[0] == 25 &&
		cell.leftRightHeightDelta[1] == 27 &&
		cell.leftRightHeightDelta[2] == 32 &&
		cell.leftRightHeightDelta[3] == 26);
	CHECK(testName, cell.backForwardHeightDelta[0] == 152 &&
		cell.backForwardHeightDelta[1] == 154 &&
		cell.backForwardHeightDelta[2] == 138 &&
		cell.backForwardHeightDelta[3] == 149);

	/* On the 32-bit retail target, row-offset arithmetic must reject a
	 * dimension product that wraps before indexing the caller's storage. */
	CHECK(testName, !CaptureHeightMapTerrainCellInput(cell, source,
		UINT_MAX, UINT_MAX, 0, 0, 3, 1, 2, 4, 0, 2, 4, 5, 0, 1.0f,
		u1, v1, u2, v2, alpha, 0));
	return 0;
}

static unsigned runOneLightingFixture(TerrainFixture *fixture,
	const char *testName)
{
	OutputStorage storage;
	refreshBatchBytes(fixture);
	initializeOutput(&storage);
	if (!PrepareHeightMapTerrainRows(fixture->snapshot,
		outputVertices(&storage), 0, 1))
	{
		fprintf(stderr, "%s: lighting preparation rejected fixture\n", testName);
		return 0xFFFFFFFFu;
	}
	if (checkOutputGuards(&storage, fixture, testName) != 0)
		return 0xFFFFFFFFu;
	return outputVertices(&storage)->diffuse;
}

static int testStaticAmbientGlobalAndDirectionalLighting()
{
	const char *testName = "testStaticAmbientGlobalAndDirectionalLighting";
	TerrainFixture fixture;
	initializeFixture(&fixture, 1, 1, 0.0f, 0.0f, -1);
	setRgb(&fixture.snapshot.terrainAmbient, 0.125f, 0.125f, 0.125f);
	fixture.globalLights[0].rayX = 0.0f;
	fixture.globalLights[0].rayY = 0.0f;
	fixture.globalLights[0].rayZ = 1.0f;
	setRgb(&fixture.globalLights[0].diffuse, 0.125f, 0.25f, 0.375f);
	fixture.snapshot.globalLights = fixture.globalLights;
	fixture.snapshot.globalLightCount = 1;
	fixture.sceneLights[0].type = HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL;
	fixture.sceneLights[0].directionX = 0.0f;
	fixture.sceneLights[0].directionY = 0.0f;
	fixture.sceneLights[0].directionZ = -1.0f;
	setRgb(&fixture.sceneLights[0].diffuse, 0.0f, 0.125f, 0.25f);
	setRgb(&fixture.sceneLights[0].ambient, 0.0f, 0.0f, 0.0f);
	fixture.snapshot.sceneLights = fixture.sceneLights;
	fixture.snapshot.sceneLightCount = 1;
	CHECK(testName, runOneLightingFixture(&fixture, testName) ==
		0x103F7FBFu);
	return 0;
}

static int testPointSpotAttenuationAndAmbient()
{
	const char *testName = "testPointSpotAttenuationAndAmbient";
	const unsigned types[] = {
		HEIGHTMAP_TERRAIN_LIGHT_POINT,
		HEIGHTMAP_TERRAIN_LIGHT_SPOT
	};
	unsigned index;

	for (index = 0; index < sizeof(types) / sizeof(types[0]); ++index)
	{
		TerrainFixture fixture;
		initializeFixture(&fixture, 1, 1, 0.0f, 0.0f, -1);
		fixture.sceneLights[0].type = types[index];
		fixture.sceneLights[0].positionX = 0.0f;
		fixture.sceneLights[0].positionY = 0.0f;
		fixture.sceneLights[0].positionZ = 3.0f;
		fixture.sceneLights[0].range = 4.0f;
		fixture.sceneLights[0].midRange = 2.0f;
		setRgb(&fixture.sceneLights[0].diffuse, 0.5f, 0.5f, 0.5f);
		setRgb(&fixture.sceneLights[0].ambient, 0.125f, 0.25f, 0.375f);
		fixture.snapshot.sceneLights = fixture.sceneLights;
		fixture.snapshot.sceneLightCount = 1;
		CHECK(testName, runOneLightingFixture(&fixture, testName) ==
			0x104F5F6Fu);
	}
	return 0;
}

static int testLightingOrderAndChannelClamping()
{
	const char *testName = "testLightingOrderAndChannelClamping";
	TerrainFixture fixture;
	initializeFixture(&fixture, 1, 1, 0.0f, 0.0f, -1);
	fixture.sceneLights[0].type = HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL;
	fixture.sceneLights[0].directionZ = -1.0f;
	setRgb(&fixture.sceneLights[0].diffuse, 0.125f, 0.25f, 0.375f);
	setRgb(&fixture.sceneLights[0].ambient, 0.0f, 0.0f, 0.0f);
	fixture.sceneLights[1].type = HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL;
	fixture.sceneLights[1].directionZ = -1.0f;
	setRgb(&fixture.sceneLights[1].diffuse, 0.25f, 0.125f, 0.0625f);
	setRgb(&fixture.sceneLights[1].ambient, 0.0f, 0.0f, 0.0f);
	fixture.snapshot.sceneLights = fixture.sceneLights;
	fixture.snapshot.sceneLightCount = 2;
	CHECK(testName, runOneLightingFixture(&fixture, testName) ==
		0x105F5F6Fu);

	setRgb(&fixture.sceneLights[0].diffuse, 2.0f, -1.0f, 1.0f);
	setRgb(&fixture.sceneLights[0].ambient, 0.75f, -2.0f, 1.0f);
	fixture.snapshot.sceneLightCount = 1;
	CHECK(testName, runOneLightingFixture(&fixture, testName) ==
		0x10FF00FFu);
	return 0;
}

static int testDepthFadeAtAndAroundWaterThreshold()
{
	const char *testName = "testDepthFadeAtAndAroundWaterThreshold";
	TerrainFixture fixture;
	initializeFixture(&fixture, 1, 1, 0.0f, 0.0f, -1);
	fixture.snapshot.mapHeightScale = 0.1f;
	fixture.globalLights[0].rayZ = 1.0f;
	setRgb(&fixture.globalLights[0].diffuse, 0.5f, 0.5f, 0.5f);
	fixture.snapshot.globalLights = fixture.globalLights;
	fixture.snapshot.globalLightCount = 1;
	fixture.snapshot.useDepthFade = 1;
	fixture.snapshot.depthFadeR = 0.5f;
	fixture.snapshot.depthFadeG = 0.25f;
	fixture.snapshot.depthFadeB = 0.0f;
	fixture.snapshot.waterHeight = 1.4f;

	fixture.cells[0].vertexHeight[0] = 0;
	fixture.cells[0].vertexHeight[1] = 0;
	fixture.cells[0].vertexHeight[2] = 0;
	fixture.cells[0].vertexHeight[3] = 0;
	CHECK(testName, runOneLightingFixture(&fixture, testName) ==
		0x103F1F00u);
	fixture.cells[0].vertexHeight[0] = 14;
	fixture.cells[0].vertexHeight[1] = 14;
	fixture.cells[0].vertexHeight[2] = 14;
	fixture.cells[0].vertexHeight[3] = 14;
	CHECK(testName, runOneLightingFixture(&fixture, testName) ==
		0x107F7F7Fu);
	fixture.cells[0].vertexHeight[0] = 15;
	fixture.cells[0].vertexHeight[1] = 15;
	fixture.cells[0].vertexHeight[2] = 15;
	fixture.cells[0].vertexHeight[3] = 15;
	CHECK(testName, runOneLightingFixture(&fixture, testName) ==
		0x107F7F7Fu);
	return 0;
}

static int callInvalidSnapshot(HeightMapTerrainSnapshot snapshot,
	OutputStorage *storage, unsigned yBegin, unsigned yEnd,
	const char *testName)
{
	OutputStorage before;

	initializeOutput(storage);
	memcpy(before.bytes, storage->bytes, sizeof(before.bytes));
	CHECK(testName, !PrepareHeightMapTerrainRows(snapshot,
		outputVertices(storage), yBegin, yEnd));
	CHECK(testName, memcmp(storage->bytes, before.bytes,
		sizeof(storage->bytes)) == 0);
	return 0;
}

static int callInvalidOutput(HeightMapTerrainSnapshot snapshot,
	OutputStorage *storage, HeightMapTerrainVertex *output,
	unsigned yBegin, unsigned yEnd, const char *testName)
{
	OutputStorage before;

	initializeOutput(storage);
	memcpy(before.bytes, storage->bytes, sizeof(before.bytes));
	CHECK(testName, !PrepareHeightMapTerrainRows(snapshot, output,
		yBegin, yEnd));
	CHECK(testName, memcmp(storage->bytes, before.bytes,
		sizeof(storage->bytes)) == 0);
	return 0;
}

static int callInvalidOverlappingOutput(TerrainFixture *fixture,
	const char *testName)
{
	HeightMapTerrainCellInput before[kMaxCells];

	memcpy(before, fixture->cells, sizeof(before));
	CHECK(testName, !PrepareHeightMapTerrainRows(fixture->snapshot,
		reinterpret_cast<HeightMapTerrainVertex *>(fixture->cells), 0,
		fixture->snapshot.height));
	CHECK(testName, memcmp(fixture->cells, before, sizeof(before)) == 0);
	return 0;
}

static int callInvalidSnapshotObjectOverlap(TerrainFixture *fixture,
	const char *testName)
{
	HeightMapTerrainSnapshot before = fixture->snapshot;

	CHECK(testName, !PrepareHeightMapTerrainRows(fixture->snapshot,
		reinterpret_cast<HeightMapTerrainVertex *>(&fixture->snapshot), 0,
		fixture->snapshot.height));
	CHECK(testName, memcmp(&fixture->snapshot, &before,
		sizeof(before)) == 0);
	return 0;
}

static Real makeNaNReal()
{
	volatile Real zero = 0.0f;
	return zero / zero;
}

static int testPreparedOutputValidation()
{
	const char *testName = "testPreparedOutputValidation";
	TerrainFixture fixture;
	OutputStorage storage;
	HeightMapTerrainBatch batch;
	HeightMapTerrainSnapshot invalid;
	HeightMapTerrainVertex *vertices;
	unsigned row;
	unsigned column;

	initializeFixture(&fixture, 2, 2, 0.0f, 0.0f, -1);
	CHECK(testName, batch.initialize(2, 2, 0, 0));
	CHECK(testName, !ValidatePreparedHeightMapTerrainOutput(batch.snapshot(),
		batch.output()));
	CHECK(testName, PrepareHeightMapTerrainRows(batch.snapshot(),
		batch.output(), 0, 1));
	CHECK(testName, !ValidatePreparedHeightMapTerrainOutput(batch.snapshot(),
		batch.output()));
	CHECK(testName, PrepareHeightMapTerrainRows(batch.snapshot(),
		batch.output(), 1, batch.snapshot().height));
	CHECK(testName, ValidatePreparedHeightMapTerrainOutput(batch.snapshot(),
		batch.output()));

	initializeOutput(&storage);
	vertices = outputVertices(&storage);
	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot, vertices,
		0, fixture.snapshot.height));
	CHECK(testName, ValidatePreparedHeightMapTerrainOutput(fixture.snapshot,
		vertices));

	vertices[1].u2 = makeNaNReal();
	CHECK(testName, !ValidatePreparedHeightMapTerrainOutput(fixture.snapshot,
		vertices));

	initializeOutput(&storage);
	vertices = outputVertices(&storage);
	for (row = 0; row < fixture.snapshot.height; ++row)
	{
		HeightMapTerrainVertex *outputRow =
			reinterpret_cast<HeightMapTerrainVertex *>(
				outputBytes(&storage) + row * fixture.snapshot.outputStrideBytes);
		for (column = 0; column < fixture.snapshot.width *
			HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++column)
		{
			outputRow[column].z = makeNaNReal();
		}
	}
	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot, vertices,
		0, 1));
	CHECK(testName, !ValidatePreparedHeightMapTerrainOutput(fixture.snapshot,
		vertices));

	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot, vertices,
		1, fixture.snapshot.height));
	invalid = fixture.snapshot;
	--invalid.outputCapacityBytes;
	CHECK(testName, !ValidatePreparedHeightMapTerrainOutput(invalid, vertices));
	return 0;
}

static int testPreparedRowScatter()
{
	const char *testName = "testPreparedRowScatter";
	enum { DESTINATION_BYTES = 1024, DESTINATION_STRIDE = 320,
		DESTINATION_FIRST_ROW = 1, DESTINATION_COLUMN_BYTES = 32,
		OVERLAP_BYTES = DESTINATION_BYTES + 32 };
	TerrainFixture fixture;
	OutputStorage storage;
	HeightMapTerrainSnapshot invalid;
	unsigned char backup[DESTINATION_BYTES];
	unsigned char hardware[DESTINATION_BYTES];
	unsigned char expected[DESTINATION_BYTES];
	unsigned char beforeBackup[DESTINATION_BYTES];
	unsigned char beforeHardware[DESTINATION_BYTES];
	unsigned char overlap[OVERLAP_BYTES];
	unsigned char beforeOverlap[OVERLAP_BYTES];
	CellAliasStorage cellAlias;
	CellAliasStorage beforeCellAlias;
	const unsigned rowBytes = 2 * HEIGHTMAP_TERRAIN_VERTEX_COUNT *
		sizeof(HeightMapTerrainVertex);
	const unsigned requiredEnd = (DESTINATION_FIRST_ROW + 1) *
		DESTINATION_STRIDE + DESTINATION_COLUMN_BYTES + rowBytes;
	unsigned row;

	initializeFixture(&fixture, 2, 2, 0.0f, 0.0f, -1);
	initializeOutput(&storage);
	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), 0, fixture.snapshot.height));
	memset(backup, 0xA5, sizeof(backup));
	memset(hardware, 0xA5, sizeof(hardware));
	memset(expected, 0xA5, sizeof(expected));
	for (row = 0; row < fixture.snapshot.height; ++row)
	{
		memcpy(expected + (DESTINATION_FIRST_ROW + row) *
			DESTINATION_STRIDE + DESTINATION_COLUMN_BYTES,
			outputBytes(&storage) + row * fixture.snapshot.outputStrideBytes,
			rowBytes);
	}
	CHECK(testName, ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, backup, sizeof(backup),
		hardware, sizeof(hardware)));
	CHECK(testName, memcmp(backup, expected, sizeof(backup)) == 0);
	CHECK(testName, memcmp(hardware, expected, sizeof(hardware)) == 0);

	memset(backup, 0xA5, sizeof(backup));
	memset(hardware, 0x5A, sizeof(hardware));
	memcpy(beforeBackup, backup, sizeof(backup));
	memcpy(beforeHardware, hardware, sizeof(hardware));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, backup, sizeof(backup),
		hardware, requiredEnd - 1));
	CHECK(testName, memcmp(backup, beforeBackup, sizeof(backup)) == 0);
	CHECK(testName, memcmp(hardware, beforeHardware, sizeof(hardware)) == 0);

	/* Vertex-misaligned layouts are rejected even when their byte ranges fit. */
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		16, DESTINATION_STRIDE, backup,
		sizeof(backup), hardware, sizeof(hardware)));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE + 1, backup,
		sizeof(backup), hardware, sizeof(hardware)));
	CHECK(testName, memcmp(backup, beforeBackup, sizeof(backup)) == 0);
	CHECK(testName, memcmp(hardware, beforeHardware, sizeof(hardware)) == 0);

	/* Neither exact nor partial destination aliasing may publish any row. */
	memset(overlap, 0x3C, sizeof(overlap));
	memcpy(beforeOverlap, overlap, sizeof(overlap));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, overlap,
		DESTINATION_BYTES, overlap, DESTINATION_BYTES));
	CHECK(testName, memcmp(overlap, beforeOverlap, sizeof(overlap)) == 0);
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, overlap,
		DESTINATION_BYTES, overlap + 32, DESTINATION_BYTES));
	CHECK(testName, memcmp(overlap, beforeOverlap, sizeof(overlap)) == 0);

	/* Renderer destinations may not overwrite immutable captured inputs. */
	memset(&cellAlias, 0x3C, sizeof(cellAlias));
	memcpy(cellAlias.aligned, fixture.cells,
		fixture.snapshot.cellCount * sizeof(HeightMapTerrainCellInput));
	memcpy(&beforeCellAlias, &cellAlias, sizeof(cellAlias));
	invalid = fixture.snapshot;
	invalid.cells = cellAlias.aligned;
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(invalid,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, cellAlias.bytes,
		sizeof(cellAlias), hardware, sizeof(hardware)));
	CHECK(testName, memcmp(&cellAlias, &beforeCellAlias,
		sizeof(cellAlias)) == 0);

	/* Null and overflowing placement metadata fail before either write. */
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, 0, sizeof(backup),
		hardware, sizeof(hardware)));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, backup, sizeof(backup),
		0, sizeof(hardware)));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		0, DESTINATION_FIRST_ROW, DESTINATION_COLUMN_BYTES,
		DESTINATION_STRIDE, backup, sizeof(backup), hardware,
		sizeof(hardware)));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), UINT_MAX, DESTINATION_COLUMN_BYTES,
		DESTINATION_STRIDE, backup, sizeof(backup), hardware,
		sizeof(hardware)));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW, UINT_MAX,
		DESTINATION_STRIDE, backup, sizeof(backup), hardware,
		sizeof(hardware)));
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, UINT_MAX, backup, sizeof(backup), hardware,
		sizeof(hardware)));
	CHECK(testName, memcmp(backup, beforeBackup, sizeof(backup)) == 0);
	CHECK(testName, memcmp(hardware, beforeHardware, sizeof(hardware)) == 0);

	outputVertices(&storage)[3].v1 = makeNaNReal();
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, backup, sizeof(backup),
		hardware, sizeof(hardware)));
	CHECK(testName, memcmp(backup, beforeBackup, sizeof(backup)) == 0);
	CHECK(testName, memcmp(hardware, beforeHardware, sizeof(hardware)) == 0);

	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), 0, fixture.snapshot.height));
	invalid = fixture.snapshot;
	--invalid.outputCapacityBytes;
	CHECK(testName, !ScatterPreparedHeightMapTerrainRows(invalid,
		outputVertices(&storage), DESTINATION_FIRST_ROW,
		DESTINATION_COLUMN_BYTES, DESTINATION_STRIDE, backup, sizeof(backup),
		hardware, sizeof(hardware)));
	CHECK(testName, memcmp(backup, beforeBackup, sizeof(backup)) == 0);
	CHECK(testName, memcmp(hardware, beforeHardware, sizeof(hardware)) == 0);
	return 0;
}

static double makeNaNDouble()
{
	volatile double zero = 0.0;
	return zero / zero;
}

static int checkInvalidRealField(HeightMapTerrainSnapshot *snapshot,
	OutputStorage *storage, Real *field, const char *testName)
{
	const Real saved = *field;
	int result;

	*field = makeNaNReal();
	result = callInvalidSnapshot(*snapshot, storage, 0,
		snapshot->height, testName);
	*field = saved;
	return result;
}

static int checkInvalidDoubleField(HeightMapTerrainSnapshot *snapshot,
	OutputStorage *storage, double *field, const char *testName)
{
	const double saved = *field;
	int result;

	*field = makeNaNDouble();
	result = callInvalidSnapshot(*snapshot, storage, 0,
		snapshot->height, testName);
	*field = saved;
	return result;
}

static int testPaddedRowsAndFiniteScan()
{
	const char *testName = "testPaddedRowsAndFiniteScan";
	TerrainFixture fixture;
	OutputStorage storage;
	Real *padding;

	initializePaddedFixture(&fixture);
	padding = reinterpret_cast<Real *>(
		reinterpret_cast<unsigned char *>(fixture.cells) +
		2 * sizeof(HeightMapTerrainCellInput));
	*padding = makeNaNReal();
	initializeOutput(&storage);
	CHECK(testName, PrepareHeightMapTerrainRows(fixture.snapshot,
		outputVertices(&storage), 0, fixture.snapshot.height));
	CHECK(testName, checkOutputGuards(&storage, &fixture, testName) == 0);

	initializePaddedFixture(&fixture);
	fixtureCell(&fixture, 1, 1)->x[0] = makeNaNReal();
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0,
		fixture.snapshot.height, testName) == 0);
	return 0;
}

static int testInvalidInputsDoNotWrite()
{
	const char *testName = "testInvalidInputsDoNotWrite";
	TerrainFixture fixture;
	OutputStorage storage;
	HeightMapTerrainSnapshot invalid;
	unsigned corner;
	initializeFixture(&fixture, 2, 2, 0.0f, 0.0f, -1);

	invalid = fixture.snapshot;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 2, 1,
		testName) == 0);
	invalid = fixture.snapshot;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 3,
		testName) == 0);
	invalid = fixture.snapshot;
	initializeOutput(&storage);
	{
		OutputStorage before;
		memcpy(before.bytes, storage.bytes, sizeof(before.bytes));
		CHECK(testName, !PrepareHeightMapTerrainRows(invalid, 0, 0, 2));
		CHECK(testName, memcmp(storage.bytes, before.bytes,
			sizeof(storage.bytes)) == 0);
	}

	invalid = fixture.snapshot;
	invalid.cells = 0;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.cellCount--;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.cellRowStrideBytes = sizeof(HeightMapTerrainCellInput) - 1;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.outputStrideBytes =
		2 * sizeof(HeightMapTerrainVertex) - 1;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.outputCapacityBytes--;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.batchBytes = HEIGHTMAP_TERRAIN_MAX_BATCH_BYTES + 1;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.outputCapacityBytes += sizeof(HeightMapTerrainVertex);
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);

	/* Finite but extreme values are rejected before derived vector math or
	 * REAL_TO_INT conversion can produce an invalid result. */
	invalid = fixture.snapshot;
	invalid.mapXYFactor = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.mapXYFactor = 0.000999f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.mapHeightScale = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.depthFadeR = 1.1f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.depthFadeG = -0.1f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.useDepthFade = 1;
	invalid.waterHeight = 0.001f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.useDepthFade = 1;
	invalid.mapHeightScale = 4096.0f;
	invalid.waterHeight = 0.01f;
	invalid.depthFadeR = 0.0f;
	invalid.depthFadeG = 0.0f;
	invalid.depthFadeB = 0.0f;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	fixture.cells[0].leftRightHeightDelta[0] = 256;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.cells[0].leftRightHeightDelta[0] = 0;
	fixture.cells[0].x[0] = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.cells[0].x[0] = 0.0f;
	fixture.cells[0].u1[0] = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.cells[0].u1[0] = 0.10f;

	invalid = fixture.snapshot;
	invalid.width = UINT_MAX;
	invalid.height = 2;
	invalid.cellCount = UINT_MAX;
	invalid.cellRowStrideBytes = UINT_MAX;
	invalid.outputStrideBytes = UINT_MAX;
	invalid.outputCapacityBytes = UINT_MAX;
	invalid.batchBytes = UINT_MAX;
	initializeOutput(&storage);
	{
		OutputStorage before;
		memcpy(before.bytes, storage.bytes, sizeof(before.bytes));
		CHECK(testName, !PrepareHeightMapTerrainRows(invalid,
			outputVertices(&storage), 0, 2));
		CHECK(testName, memcmp(storage.bytes, before.bytes,
			sizeof(storage.bytes)) == 0);
	}

	invalid = fixture.snapshot;
	invalid.globalLightCount = HEIGHTMAP_TERRAIN_MAX_GLOBAL_LIGHTS + 1;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.globalLightCount = 1;
	invalid.globalLights = 0;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.sceneLightCount = 1;
	invalid.sceneLights = 0;
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);

	/* The checked batch must cover every captured input array, including
	 * active global and scene lights, before the output can be touched. */
	fixture.snapshot.globalLights = fixture.globalLights;
	fixture.snapshot.globalLightCount = 1;
	fixture.snapshot.sceneLights = fixture.sceneLights;
	fixture.snapshot.sceneLightCount = 1;
	fixture.sceneLights[0].type = HEIGHTMAP_TERRAIN_LIGHT_POINT;
	fixture.sceneLights[0].range = 4.0;
	fixture.sceneLights[0].midRange = 2.0;
	refreshBatchBytes(&fixture);
	invalid = fixture.snapshot;
	invalid.batchBytes -= sizeof(HeightMapTerrainSceneLight);
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.globalLights = reinterpret_cast<const HeightMapTerrainGlobalLight *>(
		reinterpret_cast<const unsigned char *>(fixture.globalLights) + 1);
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	invalid = fixture.snapshot;
	invalid.sceneLights = reinterpret_cast<const HeightMapTerrainSceneLight *>(
		reinterpret_cast<const unsigned char *>(fixture.sceneLights) + 1);
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);

	/* All floating snapshot, cell, and light fields are admitted as one
	 * complete snapshot.  Each NaN case must leave the guarded output alone. */
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.mapXYFactor, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.mapHeightScale, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.terrainAmbient.red, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.terrainAmbient.green, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.terrainAmbient.blue, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.depthFadeR, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.depthFadeG, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.depthFadeB, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.snapshot.waterHeight, testName) == 0);

	/* Cell fields are all read before the first row write. */
	for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
	{
		CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
			&fixture.cells[0].x[corner], testName) == 0);
		CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
			&fixture.cells[0].y[corner], testName) == 0);
		CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
			&fixture.cells[0].u1[corner], testName) == 0);
		CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
			&fixture.cells[0].v1[corner], testName) == 0);
		CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
			&fixture.cells[0].u2[corner], testName) == 0);
		CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
			&fixture.cells[0].v2[corner], testName) == 0);
	}

	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.globalLights[0].rayX, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.globalLights[0].rayY, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.globalLights[0].rayZ, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.globalLights[0].diffuse.red, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.globalLights[0].diffuse.green, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.globalLights[0].diffuse.blue, testName) == 0);
	fixture.globalLights[0].rayX = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.globalLights[0].rayX = 0.0f;
	fixture.globalLights[0].diffuse.red = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.globalLights[0].diffuse.red = 0.0f;

	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].positionX, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].positionY, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].positionZ, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].directionX, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].directionY, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].directionZ, testName) == 0);
	CHECK(testName, checkInvalidDoubleField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].range, testName) == 0);
	CHECK(testName, checkInvalidDoubleField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].midRange, testName) == 0);
	fixture.sceneLights[0].positionX = 1.0e30f;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.sceneLights[0].positionX = 0.0f;
	fixture.sceneLights[0].range = 1.0e30;
	CHECK(testName, callInvalidSnapshot(fixture.snapshot, &storage, 0, 2,
		testName) == 0);
	fixture.sceneLights[0].range = 4.0;
	fixture.sceneLights[0].midRange = 2.0;
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].diffuse.red, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].diffuse.green, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].diffuse.blue, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].ambient.red, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].ambient.green, testName) == 0);
	CHECK(testName, checkInvalidRealField(&fixture.snapshot, &storage,
		&fixture.sceneLights[0].ambient.blue, testName) == 0);

	/* Alignment and output/input aliasing are pure admission checks. */
	invalid = fixture.snapshot;
	invalid.cells = reinterpret_cast<const HeightMapTerrainCellInput *>(
		storage.bytes + kGuardBytes + 1);
	CHECK(testName, callInvalidSnapshot(invalid, &storage, 0, 2,
		testName) == 0);
	CHECK(testName, callInvalidOutput(fixture.snapshot, &storage,
		reinterpret_cast<HeightMapTerrainVertex *>(
			storage.bytes + kGuardBytes + 1), 0, 2, testName) == 0);
	CHECK(testName, callInvalidOverlappingOutput(&fixture, testName) == 0);
	CHECK(testName, callInvalidSnapshotObjectOverlap(&fixture, testName) == 0);
	return 0;
}

#if defined(_WIN32)
typedef DWORD HeightMapTerrainTestThreadId;
#else
typedef pthread_t HeightMapTerrainTestThreadId;
#endif

static HeightMapTerrainTestThreadId heightMapTerrainCurrentThreadId()
{
#if defined(_WIN32)
	return GetCurrentThreadId();
#else
	return pthread_self();
#endif
}

static bool heightMapTerrainThreadIdsEqual(
	HeightMapTerrainTestThreadId left, HeightMapTerrainTestThreadId right)
{
#if defined(_WIN32)
	return left == right;
#else
	return pthread_equal(left, right) != 0;
#endif
}

static int testOwnerBatchBoundsAndCompleteStorage()
{
	const char *testName = "testOwnerBatchBoundsAndCompleteStorage";
	HeightMapTerrainBatch batch;
	const HeightMapTerrainSnapshot *snapshot;

	CHECK(testName, !batch.isAllocated());
	CHECK(testName, batch.initialize(2, 2, 1, 1));
	CHECK(testName, batch.isAllocated());
	CHECK(testName, !batch.isComplete());
	CHECK(testName, batch.markComplete());
	CHECK(testName, batch.isComplete());
	snapshot = &batch.snapshot();
	CHECK(testName, snapshot->width == 2);
	CHECK(testName, snapshot->height == 2);
	CHECK(testName, snapshot->cellCount == 4);
	CHECK(testName, snapshot->cells == batch.cells());
	CHECK(testName, snapshot->globalLights != 0);
	CHECK(testName, snapshot->sceneLights != 0);
	CHECK(testName, snapshot->outputCapacityBytes ==
		snapshot->outputStrideBytes * snapshot->height);
	CHECK(testName, snapshot->batchBytes ==
		snapshot->cellRowStrideBytes * snapshot->height +
		snapshot->globalLightCount * sizeof(HeightMapTerrainGlobalLight) +
		snapshot->sceneLightCount * sizeof(HeightMapTerrainSceneLight) +
		snapshot->outputCapacityBytes);
	CHECK(testName, snapshot->batchBytes <=
		HEIGHTMAP_TERRAIN_MAX_BATCH_BYTES);
	CHECK(testName, batch.output() != 0);
	CHECK(testName, reinterpret_cast<const unsigned char *>(batch.cells()) !=
		reinterpret_cast<const unsigned char *>(batch.output()));
	CHECK(testName, reinterpret_cast<const unsigned char *>(batch.globalLights()) !=
		reinterpret_cast<const unsigned char *>(batch.output()));
	CHECK(testName, reinterpret_cast<const unsigned char *>(batch.sceneLights()) !=
		reinterpret_cast<const unsigned char *>(batch.output()));

	batch.reset();
	CHECK(testName, !batch.isAllocated());
	CHECK(testName, !batch.isComplete());
	CHECK(testName, !batch.initialize(1024, 1024, 0, 0));
	CHECK(testName, !batch.isAllocated());
	CHECK(testName, !batch.initialize(UINT_MAX, 1, 0, 0));
	CHECK(testName, !batch.isAllocated());
	return 0;
}

static int testOwnerBatchSerialSmallAndInvalidFallback()
{
	const char *testName = "testOwnerBatchSerialSmallAndInvalidFallback";
	HeightMapTerrainBatch batch;
	RadarTerrainPrepareService service;
	HeightMapTerrainSnapshot snapshot;
	HeightMapTerrainVertex expected[16];
	unsigned expectedBytes;
	bool ranParallel;

	CHECK(testName, batch.initialize(2, 2, 0, 0));
	CHECK(testName, batch.markComplete());
	snapshot = batch.snapshot();
	memset(expected, 0, sizeof(expected));
	CHECK(testName, PrepareHeightMapTerrainRows(snapshot, expected, 0,
		snapshot.height));
	memset(batch.output(), 0xA5, snapshot.outputCapacityBytes);
	CHECK(testName, service.initialize(2, 2));
	ranParallel = true;
	CHECK(testName, RunHeightMapTerrainBatch(batch, service, false,
		&ranParallel));
	CHECK(testName, !ranParallel);
	expectedBytes = snapshot.outputCapacityBytes;
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);
	CHECK(testName, service.tryAcquire(9));
	service.release(9);
	CHECK(testName, service.pendingTaskCount() == 0);
	service.shutdown();

	batch.reset();
	CHECK(testName, !RunHeightMapTerrainBatch(batch, service));
	return 0;
}

enum HeightMapTerrainBatchMetadataMutation
{
	BATCH_MUTATE_WIDTH,
	BATCH_MUTATE_HEIGHT,
	BATCH_MUTATE_CELL_STRIDE,
	BATCH_MUTATE_CELL_COUNT,
	BATCH_MUTATE_CELLS,
	BATCH_MUTATE_GLOBAL_COUNT,
	BATCH_MUTATE_GLOBAL_LIGHTS,
	BATCH_MUTATE_SCENE_COUNT,
	BATCH_MUTATE_SCENE_LIGHTS,
	BATCH_MUTATE_OUTPUT_STRIDE,
	BATCH_MUTATE_OUTPUT_CAPACITY,
	BATCH_MUTATE_BATCH_BYTES,
	BATCH_MUTATION_COUNT
};

static int testOwnerBatchMetadataFreeze()
{
	const char *testName = "testOwnerBatchMetadataFreeze";
	unsigned mutation;

	for (mutation = 0; mutation < BATCH_MUTATION_COUNT; ++mutation)
	{
		HeightMapTerrainBatch batch;
		HeightMapTerrainSnapshot *snapshot;

		CHECK(testName, batch.initialize(2, 2, 1, 1));
		snapshot = const_cast<HeightMapTerrainSnapshot *>(&batch.snapshot());
		switch (mutation)
		{
		case BATCH_MUTATE_WIDTH: ++snapshot->width; break;
		case BATCH_MUTATE_HEIGHT: ++snapshot->height; break;
		case BATCH_MUTATE_CELL_STRIDE: ++snapshot->cellRowStrideBytes; break;
		case BATCH_MUTATE_CELL_COUNT: ++snapshot->cellCount; break;
		case BATCH_MUTATE_CELLS: snapshot->cells = 0; break;
		case BATCH_MUTATE_GLOBAL_COUNT: ++snapshot->globalLightCount; break;
		case BATCH_MUTATE_GLOBAL_LIGHTS: snapshot->globalLights = 0; break;
		case BATCH_MUTATE_SCENE_COUNT: ++snapshot->sceneLightCount; break;
		case BATCH_MUTATE_SCENE_LIGHTS: snapshot->sceneLights = 0; break;
		case BATCH_MUTATE_OUTPUT_STRIDE: ++snapshot->outputStrideBytes; break;
		case BATCH_MUTATE_OUTPUT_CAPACITY: ++snapshot->outputCapacityBytes; break;
		case BATCH_MUTATE_BATCH_BYTES: ++snapshot->batchBytes; break;
		default: return 1;
		}

		CHECK(testName, !batch.markComplete());
		CHECK(testName, !RunHeightMapTerrainBatch(batch));
	}

	{
		HeightMapTerrainBatch batch;
		HeightMapTerrainSnapshot *snapshot;
		CHECK(testName, batch.initialize(2, 2, 1, 1));
		CHECK(testName, batch.markComplete());
		snapshot = const_cast<HeightMapTerrainSnapshot *>(&batch.snapshot());
		++snapshot->outputCapacityBytes;
		CHECK(testName, !batch.isComplete());
		CHECK(testName, !RunHeightMapTerrainBatch(batch));
		CHECK(testName, !batch.setParameters(1.0f, 1.0f,
			batch.snapshot().terrainAmbient, 0, 0.0f, 0.0f, 0.0f, 0.0f));
	}
	return 0;
}

static int testOwnerBatchParallelByteParity()
{
	const char *testName = "testOwnerBatchParallelByteParity";
	const unsigned width = 32;
	const unsigned height = 16;
	const unsigned cellCount = width * height;
	HeightMapTerrainBatch batch;
	RadarTerrainPrepareService service;
	HeightMapTerrainVertex expected[cellCount * HEIGHTMAP_TERRAIN_VERTEX_COUNT];
	HeightMapTerrainRgb ambient;
	unsigned index;
	bool ranParallel;

	CHECK(testName, batch.initialize(width, height, 1, 1));
	for (index = 0; index < cellCount; ++index)
	{
		HeightMapTerrainCellInput *cell = &batch.cells()[index];
		const unsigned x = index % width;
		const unsigned y = index / width;
		unsigned corner;
		for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
		{
			cell->x[corner] = (Real)(x * 2 + (corner == 1 || corner == 2));
			cell->y[corner] = (Real)(y * 3 + (corner >= 2));
			cell->u1[corner] = 0.01f * (Real)(index + corner + 1);
			cell->v1[corner] = 0.02f * (Real)(index + corner + 1);
			cell->u2[corner] = 0.03f * (Real)(index + corner + 1);
			cell->v2[corner] = 0.04f * (Real)(index + corner + 1);
			cell->alpha[corner] = (UnsignedByte)(corner * 17 + index);
			cell->vertexHeight[corner] =
				(UnsignedByte)((index + corner * 3) & 31);
			cell->leftRightHeightDelta[corner] = (Int)(corner + 1);
			cell->backForwardHeightDelta[corner] = (Int)(-((Int)corner + 1));
		}
		cell->flip = (UnsignedByte)(index & 1);
	}
	setRgb(&ambient, 0.08f, 0.12f, 0.16f);
	batch.globalLights()[0].rayX = 0.2f;
	batch.globalLights()[0].rayY = -0.3f;
	batch.globalLights()[0].rayZ = 0.9f;
	setRgb(&batch.globalLights()[0].diffuse, 0.35f, 0.25f, 0.15f);
	batch.sceneLights()[0].type = HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL;
	batch.sceneLights()[0].directionX = -0.4f;
	batch.sceneLights()[0].directionY = 0.1f;
	batch.sceneLights()[0].directionZ = -0.8f;
	setRgb(&batch.sceneLights()[0].diffuse, 0.2f, 0.3f, 0.4f);
	setRgb(&batch.sceneLights()[0].ambient, 0.03f, 0.02f, 0.01f);
	CHECK(testName, batch.setParameters(1.25f, 1.5f, ambient, 1,
		0.4f, 0.5f, 0.6f, 10.0f));
	CHECK(testName, batch.markComplete());
	CHECK(testName, PrepareHeightMapTerrainRows(batch.snapshot(), expected,
		0, height));
	CHECK(testName, service.initialize(2, 2));
	memset(batch.output(), 0xA5, batch.snapshot().outputCapacityBytes);
	ranParallel = false;
	CHECK(testName, RunHeightMapTerrainBatch(batch, service, false,
		&ranParallel));
	CHECK(testName, ranParallel ==
		(rts::JobSystem::instance().workerCount() > 1));
	CHECK(testName, memcmp(batch.output(), expected,
		batch.snapshot().outputCapacityBytes) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
	CHECK(testName, service.tryAcquire(4));
	memset(batch.output(), 0xA5, batch.snapshot().outputCapacityBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service, true));
	CHECK(testName, service.hasLease());
	CHECK(testName, service.activeConsumer() == 4);
	CHECK(testName, memcmp(batch.output(), expected,
		batch.snapshot().outputCapacityBytes) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
	service.release(4);
	service.shutdown();
	return 0;
}

class HeightMapTerrainWorkerProbe : public RadarPrepareRowWork
{
public:
	HeightMapTerrainWorkerProbe(unsigned height,
		HeightMapTerrainTestThreadId *threadIds, unsigned char *arrivals)
		: m_height(height), m_threadIds(threadIds), m_arrivals(arrivals)
	#if defined(_WIN32)
		, m_firstStarted(CreateEventA(0, TRUE, FALSE, 0)),
		  m_secondStarted(CreateEventA(0, TRUE, FALSE, 0))
	#endif
	{
	#if !defined(_WIN32)
		pthread_mutex_init(&m_gateMutex, 0);
		pthread_cond_init(&m_gateCondition, 0);
		m_startedMask = 0;
	#endif
	}

	virtual ~HeightMapTerrainWorkerProbe()
	{
	#if defined(_WIN32)
		if (m_firstStarted != 0)
			CloseHandle(m_firstStarted);
		if (m_secondStarted != 0)
			CloseHandle(m_secondStarted);
	#else
		pthread_cond_destroy(&m_gateCondition);
		pthread_mutex_destroy(&m_gateMutex);
	#endif
	}

	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd)
	{
		const unsigned gateSlot = rowBegin == 0 ? 0 : 1;
		if (rowBegin >= rowEnd || rowEnd > m_height ||
			m_threadIds == 0 || m_arrivals == 0)
			return false;
		/* Adaptive splitting can produce more than two ranges. Index by the
		 * stable range start so later ranges cannot overwrite the identity
		 * that satisfied the overlap gate. */
		m_threadIds[rowBegin] = heightMapTerrainCurrentThreadId();
		m_arrivals[rowBegin] = 1;
	#if defined(_WIN32)
		/* Hold both tasks at a native event gate so a single worker cannot
		 * execute both ranges and still make the identity assertion pass. */
		if (gateSlot == 0)
		{
			if (m_firstStarted == 0 || m_secondStarted == 0 ||
				SetEvent(m_firstStarted) == FALSE ||
				WaitForSingleObject(m_secondStarted, 5000) != WAIT_OBJECT_0)
				return false;
		}
		else if (m_secondStarted == 0 || m_firstStarted == 0 ||
			SetEvent(m_secondStarted) == FALSE ||
			WaitForSingleObject(m_firstStarted, 5000) != WAIT_OBJECT_0)
		{
			return false;
		}
	#else
		struct timespec deadline;
		if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
			return false;
		deadline.tv_sec += 5;
		pthread_mutex_lock(&m_gateMutex);
		m_startedMask |= 1u << gateSlot;
		pthread_cond_broadcast(&m_gateCondition);
		while (m_startedMask != 3u)
		{
			if (pthread_cond_timedwait(&m_gateCondition, &m_gateMutex,
				&deadline) != 0)
			{
				pthread_mutex_unlock(&m_gateMutex);
				return false;
			}
		}
		pthread_mutex_unlock(&m_gateMutex);
	#endif
		return true;
	}

private:
	HeightMapTerrainWorkerProbe(const HeightMapTerrainWorkerProbe &);
	HeightMapTerrainWorkerProbe &operator=(const HeightMapTerrainWorkerProbe &);

	unsigned m_height;
	HeightMapTerrainTestThreadId *m_threadIds;
	unsigned char *m_arrivals;
	#if defined(_WIN32)
	HANDLE m_firstStarted;
	HANDLE m_secondStarted;
	#else
	pthread_mutex_t m_gateMutex;
	pthread_cond_t m_gateCondition;
	unsigned m_startedMask;
	#endif
};

static int testEligibleBatchUsesWorkersAndJoins()
{
	const char *testName = "testEligibleBatchUsesWorkersAndJoins";
	HeightMapTerrainBatch batch;
	RadarTerrainPrepareService service;
	HeightMapTerrainTestThreadId threadIds[32];
	unsigned char arrivals[32];
	const HeightMapTerrainTestThreadId ownerThreadId =
		heightMapTerrainCurrentThreadId();
	HeightMapTerrainWorkerProbe probe(32, threadIds, arrivals);
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;

	memset(arrivals, 0, sizeof(arrivals));
	system.shutdown();
	CHECK(testName, system.start(config));
	CHECK(testName, system.workerCount() == 2);
	CHECK(testName, batch.initialize(32, 16, 0, 0));
	CHECK(testName, batch.markComplete());
	CHECK(testName, service.initialize(2, 2));
	memset(batch.output(), 0xA5, batch.snapshot().outputCapacityBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, service.pendingTaskCount() == 0);
	CHECK(testName, service.tryAcquire(4));
	memset(batch.output(), 0xA5, batch.snapshot().outputCapacityBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service, true));
	CHECK(testName, service.hasLease());
	CHECK(testName, service.activeConsumer() == 4);
	CHECK(testName, service.pendingTaskCount() == 0);
	service.release(4);
	CHECK(testName, service.tryAcquire(4));
	memset(batch.output(), 0xA5, batch.snapshot().outputCapacityBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, service.hasLease());
	CHECK(testName, service.activeConsumer() == 4);
	service.release(4);
	CHECK(testName, service.tryAcquire(4));
	CHECK(testName, service.runRows(probe, 0, 16));
	CHECK(testName, arrivals[0] != 0);
	bool observedOtherRange = false;
	bool observedDistinctThread = false;
	bool observedNonOwnerThread = !heightMapTerrainThreadIdsEqual(
		threadIds[0], ownerThreadId);
	for (unsigned row = 1; row < 16; ++row)
	{
		if (arrivals[row] == 0)
			continue;
		observedOtherRange = true;
		observedDistinctThread = observedDistinctThread ||
			!heightMapTerrainThreadIdsEqual(threadIds[0], threadIds[row]);
		observedNonOwnerThread = observedNonOwnerThread ||
			!heightMapTerrainThreadIdsEqual(threadIds[row], ownerThreadId);
	}
	CHECK(testName, observedOtherRange);
	CHECK(testName, observedDistinctThread);
	CHECK(testName, observedNonOwnerThread);
	CHECK(testName, service.pendingTaskCount() == 0);
	service.release(4);
	service.shutdown();
	system.shutdown();
	return 0;
}

static int testOwnerBatchFallbackFailuresAndCleanup()
{
	const char *testName = "testOwnerBatchFallbackFailuresAndCleanup";
	HeightMapTerrainBatch batch;
	RadarTerrainPrepareService service;
	HeightMapTerrainSnapshot snapshot;
	HeightMapTerrainVertex expected[32 * 16 * 4];
	unsigned expectedBytes;

	CHECK(testName, batch.initialize(32, 16, 0, 0));
	CHECK(testName, batch.markComplete());
	snapshot = batch.snapshot();
	CHECK(testName, PrepareHeightMapTerrainRows(snapshot, expected, 0,
		snapshot.height));
	expectedBytes = snapshot.outputCapacityBytes;

	/* Unavailable runtime falls back to the complete serial result. */
	memset(batch.output(), 0xA5, expectedBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);

	/* A different consumer owns the lease, so this batch remains serial. */
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(8));
	memset(batch.output(), 0xA5, expectedBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);
	service.release(8);
	service.shutdown();

	/* The two-worker allocation fails; the existing one-worker retry completes
	 * the same batch without falling back to a partial result. */
	CHECK(testName, service.initialize(2, 2));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_radar_terrain_prepare_set_test_fault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 1);
#endif
	memset(batch.output(), 0xA5, expectedBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
	#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_radar_terrain_prepare_set_test_fault(0, 0);
	#endif
	service.shutdown();

	/* Submission rejection on both worker-count attempts falls back to one
	 * complete serial result and leaves no accepted task behind. */
	CHECK(testName, service.initialize(2, 2));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(
		HEIGHTMAP_TERRAIN_TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH, 2);
#endif
	memset(batch.output(), 0xA5, expectedBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
	#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(0, 0);
	#endif
	service.shutdown();

	/* Force both worker-count attempts to fail before any task is accepted;
	 * the adapter then computes the complete serial oracle. */
	CHECK(testName, service.initialize(2, 2));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(
		HEIGHTMAP_TERRAIN_TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE, 2);
	rts_radar_terrain_prepare_set_test_fault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 1);
#endif
	memset(batch.output(), 0xA5, expectedBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(0, 0);
	rts_radar_terrain_prepare_set_test_fault(0, 0);
#endif
	service.shutdown();

	/* Shutdown is a clean unavailable-service fallback and leaves no task. */
	CHECK(testName, service.initialize(2, 2));
	service.shutdown();
	memset(batch.output(), 0xA5, expectedBytes);
	CHECK(testName, RunHeightMapTerrainBatch(batch, service));
	CHECK(testName, memcmp(batch.output(), expected, expectedBytes) == 0);
	return 0;
}

int main()
{
	int result = 0;
	result |= testVertexLayout();
	result |= testNamedLegacyGeometryFixtures();
	result |= testAsymmetricNonPlanarGoldenGeometry();
	result |= testCapturedWrapAndClampCells();
	result |= testOwnerCaptureCellSeam();
	result |= testPaddedRowsAndFiniteScan();
	result |= testStaticAmbientGlobalAndDirectionalLighting();
	result |= testPointSpotAttenuationAndAmbient();
	result |= testLightingOrderAndChannelClamping();
	result |= testDepthFadeAtAndAroundWaterThreshold();
	result |= testInvalidInputsDoNotWrite();
	result |= testOwnerBatchBoundsAndCompleteStorage();
	result |= testPreparedOutputValidation();
	result |= testPreparedRowScatter();
	result |= testOwnerBatchSerialSmallAndInvalidFallback();
	result |= testOwnerBatchMetadataFreeze();
	result |= testOwnerBatchParallelByteParity();
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	result |= testEligibleBatchUsesWorkersAndJoins();
#endif
	result |= testOwnerBatchFallbackFailuresAndCleanup();

	if (result != 0)
	{
		fprintf(stderr, "Height-map terrain preparation tests failed.\n");
		return 1;
	}
	printf("Height-map terrain preparation tests passed.\n");
	return 0;
}
