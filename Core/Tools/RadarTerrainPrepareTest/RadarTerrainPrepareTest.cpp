#include "Lib/RadarTerrainKernel.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"

#include <stdio.h>
#include <string.h>

#if defined(RTS_BUILD_CORE_EXTRAS)
enum RadarTerrainPrepareTaskRuntimeTestEvent
{
	RADAR_TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE = 4,
	RADAR_TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH = 5
};

extern "C" void rts_task_runtime_set_test_allocation_fault(
	unsigned event, unsigned occurrence);
extern "C" void rts_radar_terrain_prepare_set_test_fault(
	unsigned fault, unsigned occurrence);
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

static int checkBytes(const unsigned char *actual,
	const unsigned char *expected, unsigned count, const char *testName)
{
	unsigned index;
	for (index = 0; index < count; ++index)
	{
		if (actual[index] != expected[index])
		{
			fprintf(stderr, "%s: byte %u was %u, expected %u\n", testName,
				index, (unsigned)actual[index], (unsigned)expected[index]);
			return 1;
		}
	}
	return 0;
}

static void setRgb(RadarTerrainRgb &rgb, Real red, Real green, Real blue)
{
	rgb.red = red;
	rgb.green = green;
	rgb.blue = blue;
}

static void setCell(RadarTerrainCellInput &cell, Real worldX, Real worldY)
{
	cell.worldX = worldX;
	cell.worldY = worldY;
	cell.groundZ = 10.0f;
	cell.centerUnderwater = 0;
	cell.centerWaterSurfaceZ = 10.0f;
	cell.centerWaterBottomZ = 10.0f;
	cell.neighborUnderwater = 1;
	cell.neighborWaterSurfaceZ = 10.0f;
	cell.neighborWaterBottomZ = 10.0f;
	cell.workingBridge = 0;
	setRgb(cell.terrainColor, 0.5f, 0.25f, 0.125f);
	setRgb(cell.bridgeColor, 1.0f, 1.0f, 1.0f);
	cell.bridgeHeight = 10.0f;
}

static RadarTerrainSnapshot makeSnapshot(RadarTerrainCellInput *cells,
	unsigned width, unsigned height, unsigned bytesPerPixel,
	unsigned formatCode, unsigned rowBytes)
{
	RadarTerrainSnapshot snapshot;
	snapshot.width = width;
	snapshot.height = height;
	snapshot.bytesPerPixel = bytesPerPixel;
	snapshot.formatCode = formatCode;
	snapshot.rowBytes = rowBytes;
	snapshot.terrainAverageZ = 10.0f;
	snapshot.mapHighZ = 10.0f;
	snapshot.mapLowZ = 0.0f;
	setRgb(snapshot.waterColor, 0.0f, 1.0f, 0.0f);
	snapshot.cells = cells;
	return snapshot;
}

static void fillCells(RadarTerrainCellInput *cells, unsigned width,
	unsigned height)
{
	unsigned y;
	for (y = 0; y < height; ++y)
	{
		unsigned x;
		for (x = 0; x < width; ++x)
		{
			setCell(cells[y * width + x],
				-100.0f + (Real)x, 500.0f + (Real)y);
		}
	}
}

static int checkGuards(const unsigned char *storage, unsigned storageSize,
	unsigned rowBytes, unsigned payloadBytes, unsigned height, unsigned guard,
	const char *testName)
{
	unsigned index;
	for (index = 0; index < storageSize; ++index)
	{
		bool payload = false;
		unsigned row;
		for (row = 0; row < height; ++row)
		{
			const unsigned rowStart = guard + row * rowBytes;
			if (index >= rowStart && index < rowStart + payloadBytes)
			{
				payload = true;
				break;
			}
		}
		if (!payload && storage[index] != 0xA5)
		{
			fprintf(stderr, "%s: guard byte %u was %u\n", testName,
				index, (unsigned)storage[index]);
			return 1;
		}
	}
	return 0;
}

static int testInterpolationKeepsLegacyArgumentOrderAndGuards()
{
	const char *testName =
		"testInterpolationKeepsLegacyArgumentOrderAndGuards";
	RadarTerrainRgb color;
	setRgb(color, 0.5f, 0.25f, 0.75f);
	InterpolateRadarColorForHeight(&color, 9.9f, 10.0f, 10.0f, 10.0f);
	CHECK(testName, color.red > 0.199f && color.red < 0.201f);
	CHECK(testName, color.green > 0.099f && color.green < 0.101f);
	CHECK(testName, color.blue > 0.299f && color.blue < 0.301f);

	setRgb(color, 0.5f, 0.25f, 0.75f);
	InterpolateRadarColorForHeight(&color, 15.0f, 20.0f, 10.0f, 0.0f);
	CHECK(testName, color.red > 0.737f && color.red < 0.738f);
	CHECK(testName, color.green > 0.606f && color.green < 0.607f);
	CHECK(testName, color.blue > 0.868f && color.blue < 0.869f);
	return 0;
}

static int runSinglePixelCase(unsigned formatCode, unsigned bytesPerPixel,
	const unsigned char *expected, const char *testName)
{
	RadarTerrainCellInput cell;
	RadarTerrainSnapshot snapshot;
	unsigned char output[4];
	setCell(cell, -1.0f, 700.0f);
	snapshot = makeSnapshot(&cell, 1, 1, bytesPerPixel, formatCode,
		bytesPerPixel);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, 1));
	CHECK(testName, checkBytes(output, expected, bytesPerPixel, testName) == 0);
	return 0;
}

static int testCenterBranchAndFormatBytes()
{
	const char *testName = "testCenterBranchAndFormatBytes";
	const unsigned char regularExpected[] = { 0x1F, 0x3F, 0x7F };
	const unsigned char bridgeExpected[] = { 0xFF, 0xFF, 0xFF };
	const unsigned char waterExpected[] = { 0x00, 0x65, 0x00 };
	RadarTerrainCellInput cell;
	RadarTerrainSnapshot snapshot;
	unsigned char output[4];

	setCell(cell, -1.0f, 700.0f);
	snapshot = makeSnapshot(&cell, 1, 1, 3,
		RADAR_TERRAIN_FORMAT_R8G8B8, 3);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, 1));
	CHECK(testName, checkBytes(output, regularExpected, 3, testName) == 0);

	cell.workingBridge = 1;
	setRgb(cell.bridgeColor, 1.0f, 1.0f, 1.0f);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, 1));
	CHECK(testName, checkBytes(output, bridgeExpected, 3, testName) == 0);

	cell.workingBridge = 0;
	cell.centerUnderwater = 1;
	cell.neighborUnderwater = 1;
	cell.neighborWaterBottomZ = 0.0f;
	setRgb(snapshot.waterColor, 0.0f, 1.0f, 0.0f);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, 1));
	CHECK(testName, checkBytes(output, waterExpected, 3, testName) == 0);
	return 0;
}

static int testRequiredFormatBytes()
{
	const unsigned char r8Expected[] = { 0x1F, 0x3F, 0x7F };
	const unsigned char x8Expected[] = { 0x1F, 0x3F, 0x7F, 0xFF };
	const unsigned char r565Expected[] = { 0xE3, 0x79 };
	const unsigned char x1555Expected[] = { 0xE3, 0xBC };
	int result = 0;
	result |= runSinglePixelCase(RADAR_TERRAIN_FORMAT_R8G8B8, 3,
		r8Expected, "testR8G8B8Bytes");
	result |= runSinglePixelCase(RADAR_TERRAIN_FORMAT_X8R8G8B8, 4,
		x8Expected, "testX8G8B8Bytes");
	result |= runSinglePixelCase(RADAR_TERRAIN_FORMAT_R5G6B5, 2,
		r565Expected, "testR5G6B5Bytes");
	result |= runSinglePixelCase(RADAR_TERRAIN_FORMAT_X1R5G5B5, 2,
		x1555Expected, "testX1R5G5B5Bytes");
	return result;
}

static int testOnlySupportedTerrainFormatSizes()
{
	struct FormatSize
	{
		unsigned formatCode;
		unsigned bytesPerPixel;
	};
	const FormatSize formats[] = {
		{ RADAR_TERRAIN_FORMAT_UNKNOWN, 0 },
		{ RADAR_TERRAIN_FORMAT_R8G8B8, 3 },
		{ RADAR_TERRAIN_FORMAT_A8R8G8B8, 0 },
		{ RADAR_TERRAIN_FORMAT_X8R8G8B8, 4 },
		{ RADAR_TERRAIN_FORMAT_R5G6B5, 2 },
		{ RADAR_TERRAIN_FORMAT_X1R5G5B5, 2 },
		{ RADAR_TERRAIN_FORMAT_A1R5G5B5, 0 },
		{ RADAR_TERRAIN_FORMAT_A4R4G4B4, 0 },
		{ RADAR_TERRAIN_FORMAT_R3G3B2, 0 },
		{ RADAR_TERRAIN_FORMAT_A8, 0 },
		{ RADAR_TERRAIN_FORMAT_A8R3G3B2, 0 },
		{ RADAR_TERRAIN_FORMAT_X4R4G4B4, 0 },
		{ RADAR_TERRAIN_FORMAT_A8P8, 0 },
		{ RADAR_TERRAIN_FORMAT_P8, 0 },
		{ RADAR_TERRAIN_FORMAT_L8, 0 },
		{ RADAR_TERRAIN_FORMAT_A8L8, 0 },
		{ RADAR_TERRAIN_FORMAT_A4L4, 0 },
		{ RADAR_TERRAIN_FORMAT_U8V8, 0 },
		{ RADAR_TERRAIN_FORMAT_L6V5U5, 0 },
		{ RADAR_TERRAIN_FORMAT_X8L8V8U8, 0 },
		{ RADAR_TERRAIN_FORMAT_DXT1, 0 },
		{ RADAR_TERRAIN_FORMAT_DXT2, 0 },
		{ RADAR_TERRAIN_FORMAT_DXT3, 0 },
		{ RADAR_TERRAIN_FORMAT_DXT4, 0 },
		{ RADAR_TERRAIN_FORMAT_DXT5, 0 }
	};
	const char *testName = "testOnlySupportedTerrainFormatSizes";
	unsigned index;
	for (index = 0; index < sizeof(formats) / sizeof(formats[0]); ++index)
	{
		CHECK(testName, RadarTerrainBytesPerPixel(formats[index].formatCode) ==
			formats[index].bytesPerPixel);
	}
	return 0;
}

static int testUnsupportedFormatsRejectWithoutWriting()
{
	const unsigned unsupported[] = {
		RADAR_TERRAIN_FORMAT_UNKNOWN,
		RADAR_TERRAIN_FORMAT_A8R8G8B8,
		RADAR_TERRAIN_FORMAT_A1R5G5B5,
		RADAR_TERRAIN_FORMAT_A4R4G4B4,
		RADAR_TERRAIN_FORMAT_R3G3B2,
		RADAR_TERRAIN_FORMAT_A8,
		RADAR_TERRAIN_FORMAT_A8R3G3B2,
		RADAR_TERRAIN_FORMAT_X4R4G4B4,
		RADAR_TERRAIN_FORMAT_A8P8,
		RADAR_TERRAIN_FORMAT_P8,
		RADAR_TERRAIN_FORMAT_L8,
		RADAR_TERRAIN_FORMAT_A8L8,
		RADAR_TERRAIN_FORMAT_A4L4,
		RADAR_TERRAIN_FORMAT_U8V8,
		RADAR_TERRAIN_FORMAT_L6V5U5,
		RADAR_TERRAIN_FORMAT_X8L8V8U8,
		RADAR_TERRAIN_FORMAT_DXT1,
		RADAR_TERRAIN_FORMAT_DXT2,
		RADAR_TERRAIN_FORMAT_DXT3,
		RADAR_TERRAIN_FORMAT_DXT4,
		RADAR_TERRAIN_FORMAT_DXT5
	};
	const char *testName = "testUnsupportedFormatsRejectWithoutWriting";
	RadarTerrainCellInput cell;
	RadarTerrainSnapshot snapshot;
	unsigned char output[8];
	unsigned char expected[8];
	unsigned index;

	setCell(cell, 0.0f, 0.0f);
	snapshot = makeSnapshot(&cell, 1, 1, 4,
		RADAR_TERRAIN_FORMAT_A8R8G8B8, 4);
	memset(expected, 0xA5, sizeof(expected));
	for (index = 0; index < sizeof(unsupported) / sizeof(unsupported[0]); ++index)
	{
		snapshot.formatCode = unsupported[index];
		memset(output, 0xA5, sizeof(output));
		CHECK(testName, RadarTerrainBytesPerPixel(snapshot.formatCode) == 0);
		CHECK(testName, !ShadeRadarRows(snapshot, output, 0, 1));
		CHECK(testName, checkBytes(output, expected, sizeof(output),
			testName) == 0);
	}
	return 0;
}

static int testHandAuthoredClippedAveragesAndBridgePrecedence()
{
	const char *testName =
		"testHandAuthoredClippedAveragesAndBridgePrecedence";
	const unsigned char cornerExpected[] = { 0x19, 0x7F, 0x85 };
	const unsigned char edgeExpected[] = { 0x3F, 0x72, 0x59 };
	const unsigned char centerExpected[] = { 0x46, 0x72, 0x6B };
	const unsigned char bridgeExpected[] = { 0x00, 0x00, 0xFF };
	RadarTerrainCellInput cells[9];
	RadarTerrainSnapshot snapshot;
	unsigned char output[27];

	fillCells(cells, 3, 3);
	setRgb(cells[0].terrainColor, 1.0f, 0.0f, 0.0f);
	cells[0].groundZ = 10.0f;
	setRgb(cells[1].terrainColor, 0.0f, 1.0f, 0.0f);
	cells[1].groundZ = 20.0f;
	setRgb(cells[2].terrainColor, 0.0f, 0.0f, 1.0f);
	cells[2].groundZ = 0.0f;
	setRgb(cells[3].terrainColor, 1.0f, 1.0f, 0.0f);
	cells[3].groundZ = 20.0f;
	setRgb(cells[4].terrainColor, 1.0f, 0.0f, 1.0f);
	cells[4].groundZ = 0.0f;
	setRgb(cells[5].terrainColor, 0.0f, 1.0f, 1.0f);
	cells[5].groundZ = 10.0f;
	setRgb(cells[6].terrainColor, 1.0f, 1.0f, 1.0f);
	cells[6].groundZ = 20.0f;
	setRgb(cells[7].terrainColor, 0.0f, 0.0f, 0.0f);
	cells[7].groundZ = 0.0f;
	setRgb(cells[8].terrainColor, 1.0f, 0.5f, 0.0f);
	cells[8].groundZ = 10.0f;

	snapshot = makeSnapshot(cells, 3, 3, 3,
		RADAR_TERRAIN_FORMAT_R8G8B8, 9);
	snapshot.terrainAverageZ = 10.0f;
	snapshot.mapHighZ = 20.0f;
	snapshot.mapLowZ = 0.0f;
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, 3));
	CHECK(testName, checkBytes(output, cornerExpected, 3, testName) == 0);
	CHECK(testName, checkBytes(output + 3, edgeExpected, 3, testName) == 0);
	CHECK(testName, checkBytes(output + 4 * 3, centerExpected, 3,
		testName) == 0);

	/* A center that is both underwater and on a live bridge must use bridge. */
	fillCells(cells, 3, 3);
	setRgb(snapshot.waterColor, 0.0f, 0.0f, 1.0f);
	cells[4].workingBridge = 1;
	cells[4].centerUnderwater = 1;
	cells[4].bridgeHeight = 20.0f;
	setRgb(cells[4].bridgeColor, 1.0f, 0.0f, 0.0f);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, 3));
	CHECK(testName, checkBytes(output + 4 * 3, bridgeExpected, 3,
		testName) == 0);
	return 0;
}

/*
 * Keep a deliberately independent legacy-style rasterizer in this test.  It
 * is an oracle, not a second production helper: it does not call any radar
 * kernel function, and its neighborhood traversal, interpolation, and byte
 * packing are written out separately so that a shared defect cannot make the
 * production and expected buffers agree.
 */
static void legacyRadarReferenceInterpolate(RadarTerrainRgb *color,
	Real height, Real hiZ, Real midZ, Real loZ)
{
	const Real howBright = 0.95f;
	const Real howDark = 0.60f;
	Real t;
	RadarTerrainRgb target;

	if (hiZ == midZ)
	{
		hiZ = midZ + 0.1f;
	}
	if (midZ == loZ)
	{
		loZ = midZ - 0.1f;
	}
	if (hiZ == loZ)
	{
		hiZ = loZ + 0.2f;
	}

	if (height >= midZ)
	{
		t = (height - midZ) / (hiZ - midZ);
		target.red = color->red + (1.0f - color->red) * howBright;
		target.green = color->green + (1.0f - color->green) * howBright;
		target.blue = color->blue + (1.0f - color->blue) * howBright;
	}
	else
	{
		t = (midZ - height) / (midZ - loZ);
		target.red = color->red + (0.0f - color->red) * howDark;
		target.green = color->green + (0.0f - color->green) * howDark;
		target.blue = color->blue + (0.0f - color->blue) * howDark;
	}

	color->red = color->red + (target.red - color->red) * t;
	color->green = color->green + (target.green - color->green) * t;
	color->blue = color->blue + (target.blue - color->blue) * t;
	if (color->red < 0.0f)
	{
		color->red = 0.0f;
	}
	if (color->red > 1.0f)
	{
		color->red = 1.0f;
	}
	if (color->green < 0.0f)
	{
		color->green = 0.0f;
	}
	if (color->green > 1.0f)
	{
		color->green = 1.0f;
	}
	if (color->blue < 0.0f)
	{
		color->blue = 0.0f;
	}
	if (color->blue > 1.0f)
	{
		color->blue = 1.0f;
	}
}

static void legacyRadarReferenceWrite(RadarTerrainSnapshot const &snapshot,
	RadarTerrainRgb const &color, unsigned char *destination)
{
	const unsigned red = static_cast<unsigned>(static_cast<unsigned char>(
		color.red * 255.0f));
	const unsigned green = static_cast<unsigned>(static_cast<unsigned char>(
		color.green * 255.0f));
	const unsigned blue = static_cast<unsigned>(static_cast<unsigned char>(
		color.blue * 255.0f));
	unsigned packed;
	unsigned byteIndex;

	/* These are the four production surface formats, kept explicit here. */
	switch (snapshot.formatCode)
	{
	case RADAR_TERRAIN_FORMAT_R8G8B8:
		packed = (red << 16) | (green << 8) | blue;
		break;
	case RADAR_TERRAIN_FORMAT_X8R8G8B8:
		packed = (0xFFu << 24) | (red << 16) | (green << 8) | blue;
		break;
	case RADAR_TERRAIN_FORMAT_R5G6B5:
		packed = ((red >> 3) << 11) | ((green >> 2) << 5) |
			(blue >> 3);
		break;
	case RADAR_TERRAIN_FORMAT_X1R5G5B5:
		packed = (1u << 15) | ((red >> 3) << 10) |
			((green >> 3) << 5) | (blue >> 3);
		break;
	default:
		packed = 0;
		break;
	}

	for (byteIndex = 0; byteIndex < snapshot.bytesPerPixel; ++byteIndex)
	{
		destination[byteIndex] = static_cast<unsigned char>(
			(packed >> (byteIndex * 8)) & 0xFFu);
	}
}

static bool legacyRadarReferenceRasterize(
	RadarTerrainSnapshot const &snapshot, unsigned char *output)
{
	unsigned y;

	if (output == 0 || snapshot.cells == 0 || snapshot.width == 0 ||
		snapshot.height == 0)
	{
		return false;
	}

	for (y = 0; y < snapshot.height; ++y)
	{
		unsigned x;
		for (x = 0; x < snapshot.width; ++x)
		{
			const RadarTerrainCellInput *center =
				&snapshot.cells[y * snapshot.width + x];
			RadarTerrainRgb sum;
			unsigned samples = 0;
			int dy;

			sum.red = 0.0f;
			sum.green = 0.0f;
			sum.blue = 0.0f;

			for (dy = -1; dy <= 1; ++dy)
			{
				int dx;
				const int sourceY = static_cast<int>(y) + dy;
				if (sourceY < 0 || sourceY >= static_cast<int>(snapshot.height))
				{
					continue;
				}

				for (dx = -1; dx <= 1; ++dx)
				{
					const int sourceX = static_cast<int>(x) + dx;
					RadarTerrainRgb sample;
					const RadarTerrainCellInput *neighbor;

					if (sourceX < 0 ||
						sourceX >= static_cast<int>(snapshot.width))
					{
						continue;
					}
					neighbor = &snapshot.cells[sourceY * snapshot.width + sourceX];

					if (center->workingBridge)
					{
						sample = center->bridgeColor;
						legacyRadarReferenceInterpolate(&sample,
							center->bridgeHeight, snapshot.terrainAverageZ,
							snapshot.mapHighZ, snapshot.mapLowZ);
					}
					else if (center->centerUnderwater)
					{
						if (!neighbor->neighborUnderwater)
						{
							continue;
						}
						sample = snapshot.waterColor;
						legacyRadarReferenceInterpolate(&sample,
							neighbor->neighborWaterBottomZ,
							center->centerWaterSurfaceZ,
							center->centerWaterSurfaceZ, snapshot.mapLowZ);
					}
					else
					{
						sample = neighbor->terrainColor;
						legacyRadarReferenceInterpolate(&sample, neighbor->groundZ,
							snapshot.terrainAverageZ, snapshot.mapHighZ,
							snapshot.mapLowZ);
					}

					sum.red += sample.red;
					sum.green += sample.green;
					sum.blue += sample.blue;
					++samples;
				}
			}

			if (samples == 0)
			{
				samples = 1;
			}
			sum.red /= static_cast<Real>(samples);
			sum.green /= static_cast<Real>(samples);
			sum.blue /= static_cast<Real>(samples);
			legacyRadarReferenceWrite(snapshot, sum,
				output + y * snapshot.rowBytes + x * snapshot.bytesPerPixel);
		}
	}
	return true;
}

static void makeLegacyReferenceFixture(RadarTerrainCellInput *cells,
	unsigned width, unsigned height)
{
	unsigned y;
	for (y = 0; y < height; ++y)
	{
		unsigned x;
		for (x = 0; x < width; ++x)
		{
			const unsigned index = y * width + x;
			setCell(cells[index], static_cast<Real>(x), static_cast<Real>(y));
			cells[index].groundZ = 2.0f + (Real)(x * 3 + y * 2);
			setRgb(cells[index].terrainColor,
				0.12f + 0.07f * (Real)x,
				0.18f + 0.06f * (Real)y,
				0.10f + 0.04f * (Real)(x + y));
		}
	}

	/* Top-left bridge and top-edge water exercise clipped neighborhoods. */
	cells[0].workingBridge = 1;
	cells[0].bridgeHeight = 24.0f;
	setRgb(cells[0].bridgeColor, 0.95f, 0.20f, 0.05f);
	cells[1].centerUnderwater = 1;
	cells[1].centerWaterSurfaceZ = 16.0f;
	cells[1].neighborWaterBottomZ = 3.0f;

	/* Interior water and a second corner bridge cover non-clipped traversal. */
	cells[1 + width * 2].centerUnderwater = 1;
	cells[1 + width * 2].centerWaterSurfaceZ = 22.0f;
	cells[1 + width * 2].neighborWaterBottomZ = 6.0f;
	cells[width * height - 1].workingBridge = 1;
	cells[width * height - 1].bridgeHeight = 8.0f;
	setRgb(cells[width * height - 1].bridgeColor, 0.05f, 0.75f, 0.95f);
}

static int testIndependentReferenceRasterizerMatchesAllFormats()
{
	const char *testName =
		"testIndependentReferenceRasterizerMatchesAllFormats";
	const unsigned width = 5;
	const unsigned height = 4;
	const unsigned guard = 3;
	const unsigned formatCodes[] = {
		RADAR_TERRAIN_FORMAT_R8G8B8,
		RADAR_TERRAIN_FORMAT_X8R8G8B8,
		RADAR_TERRAIN_FORMAT_R5G6B5,
		RADAR_TERRAIN_FORMAT_X1R5G5B5
	};
	const unsigned bytesPerPixel[] = { 3, 4, 2, 2 };
	RadarTerrainCellInput cells[width * height];
	RadarTerrainSnapshot snapshot;
	unsigned char actual[128];
	unsigned char expected[128];
	unsigned formatIndex;

	makeLegacyReferenceFixture(cells, width, height);
	snapshot = makeSnapshot(cells, width, height, bytesPerPixel[0],
		formatCodes[0], width * bytesPerPixel[0] + guard * 2);
	snapshot.terrainAverageZ = 12.0f;
	snapshot.mapHighZ = 30.0f;
	snapshot.mapLowZ = -5.0f;
	setRgb(snapshot.waterColor, 0.05f, 0.55f, 0.95f);

	for (formatIndex = 0;
		formatIndex < sizeof(formatCodes) / sizeof(formatCodes[0]);
		++formatIndex)
	{
		const unsigned payloadBytes = width * bytesPerPixel[formatIndex];
		const unsigned rowBytes = payloadBytes + guard * 2;
		const unsigned storageSize = guard + rowBytes * height + guard;
		snapshot.formatCode = formatCodes[formatIndex];
		snapshot.bytesPerPixel = bytesPerPixel[formatIndex];
		snapshot.rowBytes = rowBytes;
		memset(actual, 0xA5, sizeof(actual));
		memset(expected, 0xA5, sizeof(expected));
		CHECK(testName, ShadeRadarRows(snapshot, actual + guard, 0, height));
		CHECK(testName, legacyRadarReferenceRasterize(snapshot,
			expected + guard));
		CHECK(testName, checkBytes(actual, expected, storageSize,
			testName) == 0);
		CHECK(testName, checkGuards(actual, storageSize, rowBytes,
			payloadBytes, height, guard, testName) == 0);
	}
	return 0;
}

static int testSerialAndTwoRangeOutputsAreByteExact()
{
	const char *testName = "testSerialAndTwoRangeOutputsAreByteExact";
	const unsigned width = 3;
	const unsigned height = 4;
	const unsigned bytesPerPixel = 3;
	const unsigned payloadBytes = width * bytesPerPixel;
	const unsigned guard = 2;
	const unsigned rowBytes = payloadBytes + guard * 2;
	const unsigned storageSize = guard + rowBytes * height + guard;
	RadarTerrainCellInput cells[width * height];
	RadarTerrainSnapshot snapshot;
	unsigned char serialStorage[64];
	unsigned char splitStorage[64];
	unsigned row;

	fillCells(cells, width, height);
	/* Exercise all three center branches at clipped map edges. */
	cells[0].workingBridge = 1;
	cells[1].centerUnderwater = 1;
	cells[1].neighborWaterBottomZ = 0.0f;
	snapshot = makeSnapshot(cells, width, height, bytesPerPixel,
		RADAR_TERRAIN_FORMAT_R8G8B8, rowBytes);
	memset(serialStorage, 0xA5, sizeof(serialStorage));
	memset(splitStorage, 0xA5, sizeof(splitStorage));
	CHECK(testName, ShadeRadarRows(snapshot, serialStorage + guard, 0, height));
	CHECK(testName, ShadeRadarRows(snapshot, splitStorage + guard, 0, 2));
	CHECK(testName, ShadeRadarRows(snapshot, splitStorage + guard, 2, height));
	CHECK(testName, checkGuards(serialStorage, storageSize, rowBytes,
		payloadBytes, height, guard, testName) == 0);
	CHECK(testName, checkGuards(splitStorage, storageSize, rowBytes,
		payloadBytes, height, guard, testName) == 0);
	for (row = 0; row < height; ++row)
	{
		const unsigned char *serialRow = serialStorage + guard + row * rowBytes;
		const unsigned char *splitRow = splitStorage + guard + row * rowBytes;
		CHECK(testName, checkBytes(serialRow, splitRow, payloadBytes,
			testName) == 0);
	}
	return 0;
}

static int testInvalidRangesAndSizesDoNotWrite()
{
	const char *testName = "testInvalidRangesAndSizesDoNotWrite";
	RadarTerrainCellInput cell;
	RadarTerrainSnapshot snapshot;
	unsigned char output[8];
	unsigned char expected[8];
	setCell(cell, 0.0f, 0.0f);
	snapshot = makeSnapshot(&cell, 1, 1, 3,
		RADAR_TERRAIN_FORMAT_R8G8B8, 3);

	memset(expected, 0xA5, sizeof(expected));
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, !ShadeRadarRows(snapshot, output, 1, 0));
	CHECK(testName, !ShadeRadarRows(snapshot, output, 0, 2));
	CHECK(testName, !ShadeRadarRows(snapshot, 0, 0, 1));
	CHECK(testName, checkBytes(output, expected, sizeof(output), testName) == 0);

	snapshot.bytesPerPixel = 4;
	CHECK(testName, !ShadeRadarRows(snapshot, output, 0, 1));
	snapshot.bytesPerPixel = 3;
	snapshot.rowBytes = 2;
	CHECK(testName, !ShadeRadarRows(snapshot, output, 0, 1));
	snapshot.rowBytes = 3;
	snapshot.cells = 0;
	CHECK(testName, !ShadeRadarRows(snapshot, output, 0, 1));
	snapshot.cells = &cell;
	snapshot.width = 0;
	CHECK(testName, !ShadeRadarRows(snapshot, output, 0, 1));
	return 0;
}

static int testOwnerBatchStorageIsBoundedAndSingleOwned()
{
	const char *testName = "testOwnerBatchStorageIsBoundedAndSingleOwned";
	RadarTerrainBatch batch;
	CHECK(testName, batch.initialize(2, 2, RADAR_TERRAIN_FORMAT_R8G8B8));
	CHECK(testName, batch.isAllocated());
	CHECK(testName, batch.snapshot().width == 2);
	CHECK(testName, batch.snapshot().height == 2);
	CHECK(testName, batch.snapshot().bytesPerPixel == 3);
	CHECK(testName, batch.snapshot().rowBytes == 6);
	CHECK(testName, batch.snapshot().cells == batch.cells());
	CHECK(testName, batch.output() != 0);
	CHECK(testName, !batch.isComplete());
	batch.reset();
	CHECK(testName, !batch.isAllocated());
	CHECK(testName, !batch.isComplete());
	CHECK(testName, batch.snapshot().cells == 0);
	CHECK(testName, batch.output() == 0);

	CHECK(testName, !batch.initialize(2, 2, RADAR_TERRAIN_FORMAT_A8R8G8B8));
	CHECK(testName, !batch.initialize(0, 2, RADAR_TERRAIN_FORMAT_R8G8B8));
	CHECK(testName, !batch.initialize(2, 0, RADAR_TERRAIN_FORMAT_R8G8B8));
	CHECK(testName, !batch.initialize(512, 512, RADAR_TERRAIN_FORMAT_R8G8B8));
	CHECK(testName, !batch.initialize(0xFFFFFFFFu, 2,
		RADAR_TERRAIN_FORMAT_R8G8B8));
	return 0;
}

static int testOwnerBatchCapturePreflight()
{
	const char *testName = "testOwnerBatchCapturePreflight";
	RadarTerrainBatch batch;

	CHECK(testName, !RadarTerrainBatchCapturePreflight(batch, 2, 2,
		1.0f, 1.0f));
	CHECK(testName, batch.initialize(2, 2, RADAR_TERRAIN_FORMAT_R8G8B8));
	CHECK(testName, RadarTerrainBatchCapturePreflight(batch, 2, 2,
		1.0f, 1.0f));
	CHECK(testName, !RadarTerrainBatchCapturePreflight(batch, 2, 2,
		0.0f, 1.0f));
	CHECK(testName, !RadarTerrainBatchCapturePreflight(batch, 2, 2,
		1.0f, 0.0f));
	CHECK(testName, !RadarTerrainBatchCapturePreflight(batch, 3, 2,
		1.0f, 1.0f));
	CHECK(testName, !RadarTerrainBatchCapturePreflight(batch, 2, 3,
		1.0f, 1.0f));
	CHECK(testName, !batch.isComplete());
	batch.reset();
	CHECK(testName, !batch.isAllocated());
	CHECK(testName, !batch.isComplete());
	return 0;
}

static int compareServiceOutput(const unsigned char *actual,
	const unsigned char *expected, unsigned count, const char *testName)
{
	return checkBytes(actual, expected, count, testName);
}

static void makeServiceFixture(RadarTerrainCellInput *cells,
	RadarTerrainSnapshot *snapshot, unsigned char *serialOutput)
{
	fillCells(cells, 3, 4);
	cells[0].workingBridge = 1;
	cells[1].centerUnderwater = 1;
	cells[1].neighborWaterBottomZ = 0.0f;
	cells[7].centerUnderwater = 1;
	cells[7].neighborWaterBottomZ = 0.0f;
	*snapshot = makeSnapshot(cells, 3, 4, 3,
		RADAR_TERRAIN_FORMAT_R8G8B8, 9);
	memset(serialOutput, 0xA5, 36);
	ShadeRadarRows(*snapshot, serialOutput, 0, snapshot->height);
}

static int testRowSplitCoversRequestedRowsExactlyOnce()
{
	const char *testName = "testRowSplitCoversRequestedRowsExactlyOnce";
	struct SplitCase
	{
		unsigned begin;
		unsigned end;
		unsigned firstEnd;
	};
	const SplitCase cases[] = {
		{ 0, 1, 1 },
		{ 0, 2, 1 },
		{ 2, 7, 5 },
		{ 3, 9, 6 },
		{ 6, 7, 7 },
		{ 4, 4, 4 }
	};
	unsigned caseIndex;
	for (caseIndex = 0; caseIndex < sizeof(cases) / sizeof(cases[0]);
		++caseIndex)
	{
		RadarTerrainRowRange ranges[2];
		unsigned coverage[12];
		unsigned rangeIndex;
		unsigned row;
		memset(coverage, 0, sizeof(coverage));
		CHECK(testName, SplitRadarTerrainRowRanges(cases[caseIndex].begin,
			cases[caseIndex].end, ranges));
		CHECK(testName, ranges[0].begin == cases[caseIndex].begin);
		CHECK(testName, ranges[0].end == cases[caseIndex].firstEnd);
		CHECK(testName, ranges[1].begin == cases[caseIndex].firstEnd);
		CHECK(testName, ranges[1].end == cases[caseIndex].end);
		CHECK(testName, ranges[0].end == ranges[1].begin);
		CHECK(testName, ranges[0].begin <= ranges[0].end);
		CHECK(testName, ranges[1].begin <= ranges[1].end);
		for (rangeIndex = 0; rangeIndex < 2; ++rangeIndex)
		{
			for (row = ranges[rangeIndex].begin;
				row < ranges[rangeIndex].end; ++row)
			{
				CHECK(testName, row < sizeof(coverage) / sizeof(coverage[0]));
				++coverage[row];
			}
		}
		for (row = 0; row < sizeof(coverage) / sizeof(coverage[0]); ++row)
		{
			const unsigned expected = row >= cases[caseIndex].begin &&
				row < cases[caseIndex].end ? 1 : 0;
			CHECK(testName, coverage[row] == expected);
		}
	}
	{
		RadarTerrainRowRange ranges[2];
		CHECK(testName, !SplitRadarTerrainRowRanges(2, 1, ranges));
	}
	return 0;
}

static int testPrepareServiceSuccessfulLeaseAndRows()
{
	const char *testName = "testPrepareServiceSuccessfulLeaseAndRows";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
	CHECK(testName, !service.tryAcquire(2));
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	CHECK(testName, service.hasLease());
	service.release(1);
	CHECK(testName, !service.hasLease());
	CHECK(testName, !service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, service.tryAcquire(2));
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.release(2);
	service.shutdown();
	CHECK(testName, !service.isInitialized());
	CHECK(testName, !service.tryAcquire(3));
	return 0;
}

static int testPrepareServiceStartFailureRetriesOneWorker()
{
	const char *testName = "testPrepareServiceStartFailureRetriesOneWorker";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(
		RADAR_TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE, 1);
#endif
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(0, 0);
#endif
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.release(1);
	service.shutdown();
	return 0;
}

static int testPrepareServiceTaskAllocationRetriesOneWorker()
{
	const char *testName = "testPrepareServiceTaskAllocationRetriesOneWorker";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_radar_terrain_prepare_set_test_fault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 1);
#endif
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.release(1);
	service.shutdown();
	return 0;
}

static int testPrepareServiceQueueBackpressureFallsBackToSerial()
{
	const char *testName = "testPrepareServiceQueueBackpressureFallsBackToSerial";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 1));
	CHECK(testName, service.tryAcquire(1));
	CHECK(testName, !service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.release(1);
	service.shutdown();
	return 0;
}

static int testPrepareServiceSubmissionRollbackRetriesOneWorker()
{
	const char *testName = "testPrepareServiceSubmissionRollbackRetriesOneWorker";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(
		RADAR_TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH, 2);
#endif
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(0, 0);
#endif
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.release(1);
	service.shutdown();
	return 0;
}

static int testPrepareServiceBothAttemptsFailUseSerialOracle()
{
	const char *testName = "testPrepareServiceBothAttemptsFailUseSerialOracle";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(
		RADAR_TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE, 2);
	rts_radar_terrain_prepare_set_test_fault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 1);
#endif
	CHECK(testName, !service.runRows(&snapshot, output, 0, snapshot.height));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(0, 0);
	rts_radar_terrain_prepare_set_test_fault(0, 0);
#endif
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.release(1);
	service.shutdown();
	return 0;
}

static int testPrepareServiceShutdownClosesLease()
{
	const char *testName = "testPrepareServiceShutdownClosesLease";
	RadarTerrainPrepareService service;

	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
	service.shutdown();
	CHECK(testName, !service.isInitialized());
	CHECK(testName, !service.hasLease());
	CHECK(testName, !service.tryAcquire(1));
	service.release(1);
	service.shutdown();
	return 0;
}

static int testPrepareServiceShutdownAfterRunRestartsCleanly()
{
	const char *testName = "testPrepareServiceShutdownAfterRunRestartsCleanly";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, service.pendingTaskCount() == 0);
	service.shutdown();
	CHECK(testName, !service.isInitialized());
	CHECK(testName, !service.hasLease());
	CHECK(testName, service.pendingTaskCount() == 0);

	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(2));
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
	service.release(2);
	service.shutdown();
	return 0;
}

static int testPrepareServiceRollbackThenRetryAllocationFailureIsClean()
{
	const char *testName =
		"testPrepareServiceRollbackThenRetryAllocationFailureIsClean";
	RadarTerrainCellInput cells[12];
	RadarTerrainSnapshot snapshot;
	unsigned char serialOutput[36];
	unsigned char output[36];
	RadarTerrainPrepareService service;

	makeServiceFixture(cells, &snapshot, serialOutput);
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(2, 2));
	CHECK(testName, service.tryAcquire(1));
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(
		RADAR_TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH, 2);
	rts_radar_terrain_prepare_set_test_fault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION, 3);
#endif
	CHECK(testName, !service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, service.pendingTaskCount() == 0);
	CHECK(testName, ShadeRadarRows(snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	service.shutdown();
	CHECK(testName, !service.isInitialized());
	CHECK(testName, !service.hasLease());
	CHECK(testName, service.pendingTaskCount() == 0);

	/* One worker is an intentional degraded/test configuration. */
	memset(output, 0xA5, sizeof(output));
	CHECK(testName, service.initialize(1, 2));
	CHECK(testName, service.tryAcquire(2));
	CHECK(testName, service.runRows(&snapshot, output, 0, snapshot.height));
	CHECK(testName, compareServiceOutput(output, serialOutput,
		snapshot.rowBytes * snapshot.height, testName) == 0);
	CHECK(testName, service.pendingTaskCount() == 0);
#if defined(RTS_BUILD_CORE_EXTRAS)
	rts_task_runtime_set_test_allocation_fault(0, 0);
	rts_radar_terrain_prepare_set_test_fault(0, 0);
#endif
	service.release(2);
	service.shutdown();
	return 0;
}

static bool makeSourcePathNearTest(char *path, unsigned pathSize,
	const char *relativePath)
{
	const char *testFile = __FILE__;
	const char *slash = strrchr(testFile, '\\');
	const char *forwardSlash = strrchr(testFile, '/');
	unsigned directoryLength;
	unsigned relativeLength;

	if (forwardSlash != 0 && (slash == 0 || forwardSlash > slash))
	{
		slash = forwardSlash;
	}
	if (slash == 0)
	{
		return false;
	}

	directoryLength = static_cast<unsigned>(slash - testFile);
	relativeLength = static_cast<unsigned>(strlen(relativePath));
	if (directoryLength + 1 + relativeLength + 1 > pathSize)
	{
		return false;
	}
	memcpy(path, testFile, directoryLength);
	path[directoryLength] = '\\';
	memcpy(path + directoryLength + 1, relativePath, relativeLength + 1);
	return true;
}

static int auditSourceRange(const char *path, const char *startMarker,
	const char *endMarker, const char *const *forbidden,
	unsigned forbiddenCount, const char *testName)
{
	FILE *sourceFile = fopen(path, "rb");
	char source[65536];
	long sourceLength;
	char *start;
	char *end;
	unsigned index;

	if (sourceFile == 0)
	{
		return check(false, testName, "sourceFile != 0");
	}
	fseek(sourceFile, 0, SEEK_END);
	sourceLength = ftell(sourceFile);
	fseek(sourceFile, 0, SEEK_SET);
	if (sourceLength <= 0 || sourceLength >= static_cast<long>(sizeof(source)))
	{
		fclose(sourceFile);
		return check(false, testName, "sourceLength fits audit buffer");
	}
	if (fread(source, 1, static_cast<size_t>(sourceLength), sourceFile) !=
		static_cast<size_t>(sourceLength))
	{
		fclose(sourceFile);
		return check(false, testName, "fread(source) == sourceLength");
	}
	fclose(sourceFile);
	source[sourceLength] = '\0';

	start = strstr(source, startMarker);
	if (start == 0)
	{
		return check(false, testName, "startMarker != 0");
	}
	end = endMarker == 0 ? source + sourceLength : strstr(start, endMarker);
	if (end == 0)
	{
		return check(false, testName, "endMarker != 0");
	}
	*end = '\0';
	for (index = 0; index < forbiddenCount; ++index)
	{
		if (strstr(start, forbidden[index]) != 0)
		{
			fprintf(stderr, "%s: forbidden source token '%s'\n", testName,
				forbidden[index]);
			return 1;
		}
	}
	return 0;
}

static int auditSourceMarkers(const char *path,
	const char *const *required, unsigned requiredCount, const char *testName)
{
	FILE *sourceFile = fopen(path, "rb");
	char source[65536];
	long sourceLength;
	unsigned index;

	if (sourceFile == 0)
	{
		return check(false, testName, "sourceFile != 0");
	}
	fseek(sourceFile, 0, SEEK_END);
	sourceLength = ftell(sourceFile);
	fseek(sourceFile, 0, SEEK_SET);
	if (sourceLength <= 0 || sourceLength >= static_cast<long>(sizeof(source)))
	{
		fclose(sourceFile);
		return check(false, testName, "sourceLength fits audit buffer");
	}
	if (fread(source, 1, static_cast<size_t>(sourceLength), sourceFile) !=
		static_cast<size_t>(sourceLength))
	{
		fclose(sourceFile);
		return check(false, testName, "fread(source) == sourceLength");
	}
	fclose(sourceFile);
	source[sourceLength] = '\0';

	for (index = 0; index < requiredCount; ++index)
	{
		if (strstr(source, required[index]) == 0)
		{
			fprintf(stderr, "%s: required source token '%s' was absent\n",
				testName, required[index]);
			return 1;
		}
	}
	return 0;
}

static int testWorkerSourceAudit()
{
	const char *testName = "testWorkerSourceAudit";
	const char *const workerForbidden[] = {
		"D3D", "The", "TerrainLogic", "TerrainVisual", "Object",
		"Bridge", "new ", "delete ", "wait", "rand", "RNG",
		"replay", "save", "network"
	};
	const char *const kernelForbidden[] = {
		"D3D", "The", "new ", "delete ", "wait", "rand", "RNG",
		"replay", "save", "network"
	};
	const char *const kernelHelpers[] = {
		"static unsigned radarTerrainBytesPerPixelForFormat",
		"unsigned RadarTerrainBytesPerPixel",
		"void InterpolateRadarColorForHeight",
		"static unsigned radarTerrainColorByte",
		"static unsigned radarTerrainPackColor",
		"static void radarTerrainWritePixel",
		"static void radarTerrainAddSample",
		"void ShadeRadarPixel",
		"bool ShadeRadarRows"
	};
	char preparePath[1024];
	char kernelPath[1024];
	int result = 0;

	CHECK(testName, makeSourcePathNearTest(preparePath, sizeof(preparePath),
		"..\\..\\GameEngineDevice\\Source\\W3DDevice\\Common\\System\\RadarTerrainPrepare.cpp"));
	CHECK(testName, makeSourcePathNearTest(kernelPath, sizeof(kernelPath),
		"..\\..\\Libraries\\Source\\TaskRuntime\\RadarTerrainKernel.cpp"));
	result |= auditSourceRange(preparePath, "class RadarTerrainRowTask",
		"static RadarTerrainRowTask *radarTerrainAllocateRowTask",
		workerForbidden,
		sizeof(workerForbidden) / sizeof(workerForbidden[0]), testName);
	/* Audit the complete worker-reachable kernel, including its private
	 * color/packing/write/add helpers, not only the two public shade bodies. */
	result |= auditSourceRange(kernelPath,
		"static unsigned radarTerrainBytesPerPixelForFormat", 0,
		kernelForbidden,
		sizeof(kernelForbidden) / sizeof(kernelForbidden[0]), testName);
	result |= auditSourceMarkers(kernelPath, kernelHelpers,
		sizeof(kernelHelpers) / sizeof(kernelHelpers[0]), testName);
	return result;
}

/*
 * The owner-flow seam is deliberately source-level: constructing W3DRadar
 * would require a live D3D device and title globals.  Keep the assertions
 * focused on the safety boundary that can be checked without that runtime.
 */
static int testOwnerFlowSourceAudit()
{
	const char *testName = "testOwnerFlowSourceAudit";
	char radarPath[1024];
	static char source[131072];
	FILE *sourceFile;
	long sourceLength;
	const char *const required[] = {
		"static Bool mapRadarTerrainFormat( WW3DFormat surfaceFormat",
		"case WW3D_FORMAT_R8G8B8:",
		"case WW3D_FORMAT_X8R8G8B8:",
		"case WW3D_FORMAT_R5G6B5:",
		"case WW3D_FORMAT_X1R5G5B5:",
		"*kernelFormat = RADAR_TERRAIN_FORMAT_UNKNOWN;",
		"class RadarTerrainLeaseGuard",
		"radarLease.acquire();",
		"mapRadarTerrainFormat( surfaceDesc.Format, &formatCode )",
		"static Bool uploadPreparedRadarTerrain( SurfaceClass *surface",
		"unsignedPitch >= snapshot.rowBytes",
		"surface->Unlock();"
	};
	unsigned index;

	CHECK(testName, makeSourcePathNearTest(radarPath, sizeof(radarPath),
		"..\\..\\GameEngineDevice\\Source\\W3DDevice\\Common\\System\\W3DRadar.cpp"));
	sourceFile = fopen(radarPath, "rb");
	CHECK(testName, sourceFile != 0);
	fseek(sourceFile, 0, SEEK_END);
	sourceLength = ftell(sourceFile);
	fseek(sourceFile, 0, SEEK_SET);
	CHECK(testName, sourceLength > 0 &&
		sourceLength < static_cast<long>(sizeof(source)));
	CHECK(testName, fread(source, 1, static_cast<size_t>(sourceLength),
		sourceFile) == static_cast<size_t>(sourceLength));
	fclose(sourceFile);
	source[sourceLength] = '\0';

	for(index = 0; index < sizeof(required) / sizeof(required[0]); ++index)
		CHECK(testName, strstr(source, required[index]) != 0);
	return 0;
}

int main()
{
	int result = 0;
	result |= testInterpolationKeepsLegacyArgumentOrderAndGuards();
	result |= testCenterBranchAndFormatBytes();
	result |= testRequiredFormatBytes();
	result |= testOnlySupportedTerrainFormatSizes();
	result |= testUnsupportedFormatsRejectWithoutWriting();
	result |= testHandAuthoredClippedAveragesAndBridgePrecedence();
	result |= testIndependentReferenceRasterizerMatchesAllFormats();
	result |= testSerialAndTwoRangeOutputsAreByteExact();
	result |= testInvalidRangesAndSizesDoNotWrite();
	result |= testOwnerBatchStorageIsBoundedAndSingleOwned();
	result |= testOwnerBatchCapturePreflight();
	result |= testRowSplitCoversRequestedRowsExactlyOnce();
	result |= testPrepareServiceSuccessfulLeaseAndRows();
	result |= testPrepareServiceStartFailureRetriesOneWorker();
	result |= testPrepareServiceTaskAllocationRetriesOneWorker();
	result |= testPrepareServiceQueueBackpressureFallsBackToSerial();
	result |= testPrepareServiceSubmissionRollbackRetriesOneWorker();
	result |= testPrepareServiceBothAttemptsFailUseSerialOracle();
	result |= testPrepareServiceShutdownClosesLease();
	result |= testPrepareServiceShutdownAfterRunRestartsCleanly();
	result |= testPrepareServiceRollbackThenRetryAllocationFailureIsClean();
	result |= testWorkerSourceAudit();
	result |= testOwnerFlowSourceAudit();
	return result;
}
