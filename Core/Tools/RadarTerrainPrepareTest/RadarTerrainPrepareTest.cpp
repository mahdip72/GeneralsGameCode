#include "Lib/RadarTerrainKernel.h"

#include <stdio.h>
#include <string.h>

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

int main()
{
	int result = 0;
	result |= testInterpolationKeepsLegacyArgumentOrderAndGuards();
	result |= testCenterBranchAndFormatBytes();
	result |= testRequiredFormatBytes();
	result |= testOnlySupportedTerrainFormatSizes();
	result |= testUnsupportedFormatsRejectWithoutWriting();
	result |= testHandAuthoredClippedAveragesAndBridgePrecedence();
	result |= testSerialAndTwoRangeOutputsAreByteExact();
	result |= testInvalidRangesAndSizesDoNotWrite();
	return result;
}
