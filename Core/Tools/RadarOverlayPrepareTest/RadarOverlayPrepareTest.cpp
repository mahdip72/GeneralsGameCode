#include "Lib/RadarOverlayKernel.h"

#include <limits.h>
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

static unsigned bytesPerPixel(unsigned formatCode)
{
	return RadarOverlayBytesPerPixel(formatCode);
}

static unsigned rowBytes(unsigned width, unsigned formatCode, unsigned guard)
{
	return width * bytesPerPixel(formatCode) + guard * 2;
}

static unsigned storageSize(unsigned height, unsigned stride, unsigned guard)
{
	return guard + height * stride + guard;
}

static void initializeStorage(unsigned char *storage, unsigned count,
	unsigned char value)
{
	memset(storage, value, count);
}

static unsigned char *pixel(unsigned char *output, unsigned stride,
	unsigned bpp, unsigned x, unsigned y)
{
	return output + y * stride + x * bpp;
}

static const unsigned char *pixel(const unsigned char *output, unsigned stride,
	unsigned bpp, unsigned x, unsigned y)
{
	return output + y * stride + x * bpp;
}

static void writeExpected(unsigned char *output, unsigned stride, unsigned bpp,
	unsigned x, unsigned y, unsigned packedColor)
{
	unsigned byteIndex;
	unsigned char *destination = pixel(output, stride, bpp, x, y);
	for (byteIndex = 0; byteIndex < bpp; ++byteIndex)
	{
		destination[byteIndex] = static_cast<unsigned char>(
			(packedColor >> (byteIndex * 8)) & 0xFFu);
	}
}

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

static int checkGuards(const unsigned char *storage, unsigned storageCount,
	const unsigned char *output, unsigned stride, unsigned bpp,
	unsigned width, unsigned height, unsigned char guardValue,
	const char *testName)
{
	unsigned index;
	for (index = 0; index < storageCount; ++index)
	{
		bool payload = false;
		unsigned y;
		for (y = 0; y < height; ++y)
		{
			const unsigned char *row = output + y * stride;
			const unsigned char *rowEnd = row + width * bpp;
			if (storage + index >= row && storage + index < rowEnd)
			{
				payload = true;
				break;
			}
		}
		if (!payload && storage[index] != guardValue)
		{
			fprintf(stderr, "%s: guard byte %u was %u\n", testName,
				index, (unsigned)storage[index]);
			return 1;
		}
	}
	return 0;
}

static RadarObjectOverlaySnapshot makeObjectSnapshot(
	const RadarObjectOverlayCommand *commands, unsigned commandCount,
	unsigned width, unsigned height, unsigned formatCode, unsigned char *output,
	unsigned stride)
{
	RadarObjectOverlaySnapshot snapshot;
	snapshot.width = width;
	snapshot.height = height;
	snapshot.bytesPerPixel = bytesPerPixel(formatCode);
	snapshot.formatCode = formatCode;
	snapshot.rowBytes = stride;
	snapshot.commandCount = commandCount;
	snapshot.commandCapacity = commandCount;
	snapshot.commands = commands;
	snapshot.output = output;
	return snapshot;
}

static RadarShroudOverlaySnapshot makeShroudSnapshot(
	const RadarShroudOverlayCommand *commands, unsigned commandCount,
	unsigned width, unsigned height, unsigned formatCode, unsigned char *output,
	unsigned stride)
{
	RadarShroudOverlaySnapshot snapshot;
	snapshot.width = width;
	snapshot.height = height;
	snapshot.bytesPerPixel = bytesPerPixel(formatCode);
	snapshot.formatCode = formatCode;
	snapshot.rowBytes = stride;
	snapshot.commandCount = commandCount;
	snapshot.commandCapacity = commandCount;
	snapshot.commands = commands;
	snapshot.output = output;
	return snapshot;
}

static int checkPixels(const unsigned char *actual, const unsigned char *expected,
	unsigned stride, unsigned bpp, unsigned width, unsigned height,
	const char *testName)
{
	unsigned x;
	unsigned y;
	for (y = 0; y < height; ++y)
	{
		for (x = 0; x < width; ++x)
		{
			if (checkBytes(pixel(actual, stride, bpp, x, y),
				pixel(expected, stride, bpp, x, y), bpp, testName) != 0)
			{
				return 1;
			}
		}
	}
	return 0;
}

static int testSupportedFormatsAndPacking()
{
	const char *testName = "testSupportedFormatsAndPacking";
	CHECK(testName, RadarOverlayBytesPerPixel(
		RADAR_OVERLAY_FORMAT_A8R8G8B8) == 4);
	CHECK(testName, RadarOverlayBytesPerPixel(
		RADAR_OVERLAY_FORMAT_A4R4G4B4) == 2);
	CHECK(testName, RadarOverlayBytesPerPixel(0) == 0);
	CHECK(testName, RadarOverlayBytesPerPixel(0xFFFFFFFFu) == 0);
	return 0;
}

static int testObjectRowsPreserveOrderClippingAndGuards(unsigned formatCode)
{
	const char *testName = "testObjectRowsPreserveOrderClippingAndGuards";
	const unsigned width = 4;
	const unsigned height = 4;
	const unsigned guard = 3;
	const unsigned bpp = bytesPerPixel(formatCode);
	const unsigned stride = rowBytes(width, formatCode, guard);
	const unsigned count = storageSize(height, stride, guard);
	const unsigned char guardValue = 0xE1;
	const unsigned firstColor = 0x11223344u;
	const unsigned secondColor = 0xAABBCCDDu;
	const unsigned clippedColor = 0x0F1E2D3Cu;
	const unsigned edgeColor = 0x55667788u;
	RadarObjectOverlayCommand commands[4];
	unsigned char serialStorage[128];
	unsigned char splitStorage[128];
	unsigned char expectedStorage[128];
	RadarObjectOverlaySnapshot serial;
	RadarObjectOverlaySnapshot split;
	unsigned char *serialOutput;
	unsigned char *splitOutput;
	unsigned char *expectedOutput;

	commands[0].x = 0;
	commands[0].y = 0;
	commands[0].packedColor = firstColor;
	commands[1].x = 1;
	commands[1].y = 0;
	commands[1].packedColor = secondColor;
	commands[2].x = -1;
	commands[2].y = 1;
	commands[2].packedColor = clippedColor;
	commands[3].x = 3;
	commands[3].y = 3;
	commands[3].packedColor = edgeColor;

	initializeStorage(serialStorage, sizeof(serialStorage), guardValue);
	initializeStorage(splitStorage, sizeof(splitStorage), guardValue);
	initializeStorage(expectedStorage, sizeof(expectedStorage), guardValue);
	serialOutput = serialStorage + guard;
	splitOutput = splitStorage + guard;
	expectedOutput = expectedStorage + guard;
	serial = makeObjectSnapshot(commands, 4, width, height, formatCode,
		serialOutput, stride);
	split = makeObjectSnapshot(commands, 4, width, height, formatCode,
		splitOutput, stride);

	/* Command 0: (0,0), (0,1), (1,1), (1,0). */
	writeExpected(expectedOutput, stride, bpp, 0, 0, firstColor);
	writeExpected(expectedOutput, stride, bpp, 0, 1, firstColor);
	writeExpected(expectedOutput, stride, bpp, 1, 1, firstColor);
	writeExpected(expectedOutput, stride, bpp, 1, 0, firstColor);
	/* Command 1 overwrites the two overlaps and writes two new pixels. */
	writeExpected(expectedOutput, stride, bpp, 1, 0, secondColor);
	writeExpected(expectedOutput, stride, bpp, 1, 1, secondColor);
	writeExpected(expectedOutput, stride, bpp, 2, 1, secondColor);
	writeExpected(expectedOutput, stride, bpp, 2, 0, secondColor);
	/* x == -1 still permits the x + 1 footprint points at column zero. */
	writeExpected(expectedOutput, stride, bpp, 0, 2, clippedColor);
	writeExpected(expectedOutput, stride, bpp, 0, 1, clippedColor);
	/* Only the first point of the bottom-right command is in bounds. */
	writeExpected(expectedOutput, stride, bpp, 3, 3, edgeColor);

	CHECK(testName, PackRadarObjectRows(serial, 0, height));
	CHECK(testName, checkPixels(serialOutput, expectedOutput, stride, bpp,
		width, height, testName) == 0);
	CHECK(testName, checkGuards(serialStorage, count, serialOutput, stride, bpp,
		width, height, guardValue, testName) == 0);
	CHECK(testName, PackRadarObjectRows(split, 0, height / 2));
	CHECK(testName, PackRadarObjectRows(split, height / 2, height));
	CHECK(testName, checkBytes(splitStorage, serialStorage, count, testName) == 0);
	CHECK(testName, checkGuards(splitStorage, count, splitOutput, stride, bpp,
		width, height, guardValue, testName) == 0);
	return 0;
}

static int testObjectExtremeCoordinatesDoNotOverflow()
{
	const char *testName = "testObjectExtremeCoordinatesDoNotOverflow";
	const unsigned width = 3;
	const unsigned height = 3;
	const unsigned bpp = 4;
	const unsigned stride = width * bpp + 4;
	unsigned char storage[64];
	RadarObjectOverlayCommand commands[4];
	RadarObjectOverlaySnapshot snapshot;
	unsigned char *output = storage + 2;
	unsigned x;
	unsigned y;

	commands[0].x = INT_MIN;
	commands[0].y = INT_MIN;
	commands[0].packedColor = 0x01020304u;
	commands[1].x = INT_MAX;
	commands[1].y = INT_MAX;
	commands[1].packedColor = 0x05060708u;
	commands[2].x = -1;
	commands[2].y = -1;
	commands[2].packedColor = 0xA1B2C3D4u;
	commands[3].x = INT_MAX;
	commands[3].y = 0;
	commands[3].packedColor = 0xF1F2F3F4u;
	initializeStorage(storage, sizeof(storage), 0xC7);
	snapshot = makeObjectSnapshot(commands, 4, width, height,
		RADAR_OVERLAY_FORMAT_A8R8G8B8, output, stride);
	CHECK(testName, PackRadarObjectRows(snapshot, 0, height));
	for (y = 0; y < height; ++y)
	{
		for (x = 0; x < width; ++x)
		{
			const unsigned char *actual = pixel(output, stride, bpp, x, y);
			if (x == 0 && y == 0)
			{
				const unsigned char expected[] = { 0xD4, 0xC3, 0xB2, 0xA1 };
				CHECK(testName, checkBytes(actual, expected, 4, testName) == 0);
			}
			else
			{
				unsigned byteIndex;
				for (byteIndex = 0; byteIndex < bpp; ++byteIndex)
					CHECK(testName, actual[byteIndex] == 0xC7);
			}
		}
	}
	return 0;
}

static int testShroudRowsAreInclusiveOrderedAndClipped(unsigned formatCode)
{
	const char *testName = "testShroudRowsAreInclusiveOrderedAndClipped";
	const unsigned width = 5;
	const unsigned height = 4;
	const unsigned guard = 2;
	const unsigned bpp = bytesPerPixel(formatCode);
	const unsigned stride = rowBytes(width, formatCode, guard);
	const unsigned count = storageSize(height, stride, guard);
	const unsigned char guardValue = 0xD2;
	const unsigned firstColor = 0x10203040u;
	const unsigned secondColor = 0xA0B0C0D0u;
	const unsigned clippedColor = 0x50607080u;
	RadarShroudOverlayCommand commands[4];
	unsigned char serialStorage[128];
	unsigned char splitStorage[128];
	unsigned char expectedStorage[128];
	RadarShroudOverlaySnapshot serial;
	RadarShroudOverlaySnapshot split;
	unsigned char *serialOutput;
	unsigned char *splitOutput;
	unsigned char *expectedOutput;

	commands[0].minX = 1;
	commands[0].minY = 1;
	commands[0].maxX = 3;
	commands[0].maxY = 2;
	commands[0].packedColor = firstColor;
	commands[1].minX = 2;
	commands[1].minY = 0;
	commands[1].maxX = 4;
	commands[1].maxY = 1;
	commands[1].packedColor = secondColor;
	commands[2].minX = -2;
	commands[2].minY = -1;
	commands[2].maxX = 1;
	commands[2].maxY = 0;
	commands[2].packedColor = clippedColor;
	commands[3].minX = 4;
	commands[3].minY = 3;
	commands[3].maxX = 2;
	commands[3].maxY = 3;
	commands[3].packedColor = 0xFFFFFFFFu;

	initializeStorage(serialStorage, sizeof(serialStorage), guardValue);
	initializeStorage(splitStorage, sizeof(splitStorage), guardValue);
	initializeStorage(expectedStorage, sizeof(expectedStorage), guardValue);
	serialOutput = serialStorage + guard;
	splitOutput = splitStorage + guard;
	expectedOutput = expectedStorage + guard;
	serial = makeShroudSnapshot(commands, 4, width, height, formatCode,
		serialOutput, stride);
	split = makeShroudSnapshot(commands, 4, width, height, formatCode,
		splitOutput, stride);

	/* Command 0 is inclusive; command 1 overwrites its row-1 overlap. */
	writeExpected(expectedOutput, stride, bpp, 1, 1, firstColor);
	writeExpected(expectedOutput, stride, bpp, 1, 2, firstColor);
	writeExpected(expectedOutput, stride, bpp, 2, 1, secondColor);
	writeExpected(expectedOutput, stride, bpp, 3, 1, secondColor);
	writeExpected(expectedOutput, stride, bpp, 2, 2, firstColor);
	writeExpected(expectedOutput, stride, bpp, 3, 2, firstColor);
	writeExpected(expectedOutput, stride, bpp, 4, 0, secondColor);
	writeExpected(expectedOutput, stride, bpp, 4, 1, secondColor);
	writeExpected(expectedOutput, stride, bpp, 2, 0, secondColor);
	writeExpected(expectedOutput, stride, bpp, 3, 0, secondColor);
	writeExpected(expectedOutput, stride, bpp, 0, 0, clippedColor);
	writeExpected(expectedOutput, stride, bpp, 1, 0, clippedColor);
	CHECK(testName, PackRadarShroudRows(serial, 0, height));
	CHECK(testName, checkPixels(serialOutput, expectedOutput, stride, bpp,
		width, height, testName) == 0);
	CHECK(testName, checkGuards(serialStorage, count, serialOutput, stride, bpp,
		width, height, guardValue, testName) == 0);
	CHECK(testName, PackRadarShroudRows(split, 0, 2));
	CHECK(testName, PackRadarShroudRows(split, 2, height));
	CHECK(testName, checkBytes(splitStorage, serialStorage, count, testName) == 0);
	CHECK(testName, checkGuards(splitStorage, count, splitOutput, stride, bpp,
		width, height, guardValue, testName) == 0);
	return 0;
}

static int testShroudExtremeAndEmptyCommands()
{
	const char *testName = "testShroudExtremeAndEmptyCommands";
	const unsigned width = 3;
	const unsigned height = 2;
	const unsigned bpp = 4;
	const unsigned stride = width * bpp + 2;
	unsigned char storage[32];
	RadarShroudOverlayCommand commands[2];
	RadarShroudOverlaySnapshot snapshot;
	unsigned char *output = storage + 1;
	unsigned x;
	unsigned y;

	commands[0].minX = INT_MIN;
	commands[0].minY = INT_MIN;
	commands[0].maxX = INT_MAX;
	commands[0].maxY = INT_MAX;
	commands[0].packedColor = 0x01020304u;
	commands[1].minX = INT_MAX;
	commands[1].minY = INT_MAX;
	commands[1].maxX = INT_MIN;
	commands[1].maxY = INT_MIN;
	commands[1].packedColor = 0xAABBCCDDu;
	initializeStorage(storage, sizeof(storage), 0xB6);
	snapshot = makeShroudSnapshot(commands, 2, width, height,
		RADAR_OVERLAY_FORMAT_A8R8G8B8, output, stride);
	CHECK(testName, PackRadarShroudRows(snapshot, 0, height));
	for (y = 0; y < height; ++y)
		for (x = 0; x < width; ++x)
		{
			const unsigned char *actual = pixel(output, stride, bpp, x, y);
			const unsigned char expected[] = { 0x04, 0x03, 0x02, 0x01 };
			CHECK(testName, checkBytes(actual, expected, bpp, testName) == 0);
		}
	return 0;
}

static void initializeObjectSnapshot(RadarObjectOverlaySnapshot *snapshot,
	unsigned char *output)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->width = 2;
	snapshot->height = 2;
	snapshot->bytesPerPixel = 4;
	snapshot->formatCode = RADAR_OVERLAY_FORMAT_A8R8G8B8;
	snapshot->rowBytes = 8;
	snapshot->output = output;
}

static void initializeShroudSnapshot(RadarShroudOverlaySnapshot *snapshot,
	unsigned char *output)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->width = 2;
	snapshot->height = 2;
	snapshot->bytesPerPixel = 4;
	snapshot->formatCode = RADAR_OVERLAY_FORMAT_A8R8G8B8;
	snapshot->rowBytes = 8;
	snapshot->output = output;
}

static int testInvalidSnapshotsDoNotWrite()
{
	const char *testName = "testInvalidSnapshotsDoNotWrite";
	unsigned char storage[32];
	unsigned char before[32];
	RadarObjectOverlayCommand objectCommand;
	RadarShroudOverlayCommand shroudCommand;
	RadarObjectOverlaySnapshot object;
	RadarShroudOverlaySnapshot shroud;

	objectCommand.x = 0;
	objectCommand.y = 0;
	objectCommand.packedColor = 0x11223344u;
	shroudCommand.minX = 0;
	shroudCommand.minY = 0;
	shroudCommand.maxX = 1;
	shroudCommand.maxY = 1;
	shroudCommand.packedColor = 0x55667788u;
	initializeStorage(storage, sizeof(storage), 0x9A);
	memcpy(before, storage, sizeof(storage));
	initializeObjectSnapshot(&object, storage);
	initializeShroudSnapshot(&shroud, storage);

	object.commands = &objectCommand;
	object.commandCount = 1;
	object.commandCapacity = 1;
	shroud.commands = &shroudCommand;
	shroud.commandCount = 1;
	shroud.commandCapacity = 1;
	CHECK(testName, PackRadarObjectRows(object, 0, 2));
	memcpy(storage, before, sizeof(storage));
	CHECK(testName, PackRadarShroudRows(shroud, 0, 2));
	memcpy(storage, before, sizeof(storage));

	object.width = 0;
	CHECK(testName, !PackRadarObjectRows(object, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	object.width = static_cast<unsigned>(INT_MAX) + 1u;
	CHECK(testName, !PackRadarObjectRows(object, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	object.width = 2;
	object.bytesPerPixel = 2;
	CHECK(testName, !PackRadarObjectRows(object, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	object.bytesPerPixel = 4;
	object.rowBytes = 7;
	CHECK(testName, !PackRadarObjectRows(object, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	object.rowBytes = 8;
	object.commands = 0;
	CHECK(testName, !PackRadarObjectRows(object, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	object.commands = &objectCommand;
	object.commandCount = 1;
	object.commandCapacity = 0;
	CHECK(testName, !PackRadarObjectRows(object, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	object.commandCapacity = 1;
	CHECK(testName, !PackRadarObjectRows(object, 2, 1));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	CHECK(testName, !PackRadarObjectRows(object, 0, 3));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);

	shroud.width = 0;
	CHECK(testName, !PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	shroud.width = 2;
	shroud.bytesPerPixel = 4;
	shroud.formatCode = RADAR_OVERLAY_FORMAT_UNKNOWN;
	CHECK(testName, !PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	shroud.formatCode = RADAR_OVERLAY_FORMAT_A8R8G8B8;
	shroud.commands = 0;
	shroud.commandCount = 1;
	CHECK(testName, !PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	shroud.commands = &shroudCommand;
	shroud.commandCapacity = 0;
	CHECK(testName, !PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	shroud.commandCapacity = 1;
	shroud.rowBytes = 7;
	CHECK(testName, !PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	shroud.rowBytes = 8;
	CHECK(testName, shroud.output != 0);
	shroud.output = 0;
	CHECK(testName, !PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	return 0;
}

static int testKernelsWriteOnlyAssignedRows()
{
	const char *testName = "testKernelsWriteOnlyAssignedRows";
	const unsigned width = 4;
	const unsigned height = 4;
	const unsigned stride = width * 4 + 3;
	unsigned char objectStorage[80];
	unsigned char shroudStorage[80];
	unsigned char before[80];
	RadarObjectOverlayCommand objectCommand;
	RadarShroudOverlayCommand shroudCommand;
	RadarObjectOverlaySnapshot object;
	RadarShroudOverlaySnapshot shroud;
	unsigned row;

	objectCommand.x = 0;
	objectCommand.y = 0;
	objectCommand.packedColor = 0x11223344u;
	shroudCommand.minX = 0;
	shroudCommand.minY = 0;
	shroudCommand.maxX = 3;
	shroudCommand.maxY = 3;
	shroudCommand.packedColor = 0x55667788u;
	initializeStorage(objectStorage, sizeof(objectStorage), 0x6A);
	initializeStorage(shroudStorage, sizeof(shroudStorage), 0x6A);
	initializeStorage(before, sizeof(before), 0x6A);
	object = makeObjectSnapshot(&objectCommand, 1, width, height,
		RADAR_OVERLAY_FORMAT_A8R8G8B8, objectStorage, stride);
	shroud = makeShroudSnapshot(&shroudCommand, 1, width, height,
		RADAR_OVERLAY_FORMAT_A8R8G8B8, shroudStorage, stride);

	CHECK(testName, PackRadarObjectRows(object, 1, 2));
	CHECK(testName, PackRadarShroudRows(shroud, 1, 2));
	for (row = 0; row < height; ++row)
	{
		if (row == 1)
			continue;
		CHECK(testName, checkBytes(objectStorage + row * stride,
			before + row * stride, stride, testName) == 0);
		CHECK(testName, checkBytes(shroudStorage + row * stride,
			before + row * stride, stride, testName) == 0);
	}
	return 0;
}

static int testEmptyCommandsPreserveExistingBytes()
{
	const char *testName = "testEmptyCommandsPreserveExistingBytes";
	unsigned char storage[32];
	unsigned char before[32];
	RadarObjectOverlaySnapshot object;
	RadarShroudOverlaySnapshot shroud;
	initializeStorage(storage, sizeof(storage), 0x4D);
	memcpy(before, storage, sizeof(storage));
	initializeObjectSnapshot(&object, storage);
	initializeShroudSnapshot(&shroud, storage);
	CHECK(testName, PackRadarObjectRows(object, 0, 0));
	CHECK(testName, PackRadarObjectRows(object, 0, 2));
	CHECK(testName, PackRadarShroudRows(shroud, 0, 0));
	CHECK(testName, PackRadarShroudRows(shroud, 0, 2));
	CHECK(testName, checkBytes(storage, before, sizeof(storage), testName) == 0);
	return 0;
}

int main()
{
	int result = 0;
	result |= testSupportedFormatsAndPacking();
	result |= testObjectRowsPreserveOrderClippingAndGuards(
		RADAR_OVERLAY_FORMAT_A8R8G8B8);
	result |= testObjectRowsPreserveOrderClippingAndGuards(
		RADAR_OVERLAY_FORMAT_A4R4G4B4);
	result |= testObjectExtremeCoordinatesDoNotOverflow();
	result |= testShroudRowsAreInclusiveOrderedAndClipped(
		RADAR_OVERLAY_FORMAT_A8R8G8B8);
	result |= testShroudRowsAreInclusiveOrderedAndClipped(
		RADAR_OVERLAY_FORMAT_A4R4G4B4);
	result |= testShroudExtremeAndEmptyCommands();
	result |= testInvalidSnapshotsDoNotWrite();
	result |= testEmptyCommandsPreserveExistingBytes();
	result |= testKernelsWriteOnlyAssignedRows();
	return result;
}
