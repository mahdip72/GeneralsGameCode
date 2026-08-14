#include "W3DDevice/GameClient/W3DScreenshotCodec.h"

#include <stdio.h>

static int check(bool value, const char *testName, const char *expression)
{
	if (!value)
	{
		fprintf(stderr, "%s: %s\n", testName, expression);
		return 1;
	}
	return 0;
}

#define CHECK(testName, expression) do { if (check((expression), testName, #expression) != 0) return 1; } while (0)

static int checkBytes(const unsigned char *actual, const unsigned char *expected, unsigned count,
	const char *testName)
{
	unsigned index;
	for (index = 0; index < count; ++index)
	{
		if (actual[index] != expected[index])
		{
			fprintf(stderr, "%s: byte %u was %u, expected %u\n", testName, index,
				(unsigned)actual[index], (unsigned)expected[index]);
			return 1;
		}
	}
	return 0;
}

static void convertFullImage(const ScreenshotPixelSource &source, unsigned char *destination)
{
	ConvertScreenshotRows(source, 0, source.height, destination);
}

static int testArgb32SerialConversionUsesPitch()
{
	const char *testName = "testArgb32SerialConversionUsesPitch";
	const unsigned pixelCount = 3 * 3;
	const unsigned int pixels[] = {
		0xA0123456, 0xFFABCDEF, 0x00010203, 0xDEADBEEF,
		0xFFFFFFFF, 0x13579BDF, 0x2468ACE0, 0xDEADBEEF,
		0x80C0FFEE, 0x7F112233, 0xB0445566, 0xDEADBEEF
	};
	const unsigned char expected[] = {
		0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03,
		0xFF, 0xFF, 0xFF, 0x57, 0x9B, 0xDF, 0x68, 0xAC, 0xE0,
		0xC0, 0xFF, 0xEE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66
	};
	unsigned char converted[3 * pixelCount];
	ScreenshotPixelSource source;

	source.pixels = reinterpret_cast<const unsigned char *>(pixels);
	source.width = 3;
	source.height = 3;
	source.pitch = 4 * sizeof(unsigned int);
	source.format = SCREENSHOT_SOURCE_ARGB32;
	convertFullImage(source, converted);
	CHECK(testName, checkBytes(converted, expected, 3 * pixelCount, testName) == 0);
	return 0;
}

static int testRgb565SerialConversionUsesPitch()
{
	const char *testName = "testRgb565SerialConversionUsesPitch";
	const unsigned pixelCount = 3 * 3;
	const unsigned short pixels[] = {
		0xF800, 0x07E0, 0x001F, 0xAAAA,
		0xFFFF, 0x0000, 0x7BEF, 0xAAAA,
		0x1234, 0xABCD, 0xF81F, 0xAAAA
	};
	const unsigned char expected[] = {
		0xF8, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0xF8,
		0xF8, 0xFC, 0xF8, 0x00, 0x00, 0x00, 0x78, 0x7C, 0x78,
		0x10, 0x44, 0xA0, 0xA8, 0x78, 0x68, 0xF8, 0x00, 0xF8
	};
	unsigned char converted[3 * pixelCount];
	ScreenshotPixelSource source;

	source.pixels = reinterpret_cast<const unsigned char *>(pixels);
	source.width = 3;
	source.height = 3;
	source.pitch = 4 * sizeof(unsigned short);
	source.format = SCREENSHOT_SOURCE_RGB565;
	convertFullImage(source, converted);
	CHECK(testName, checkBytes(converted, expected, 3 * pixelCount, testName) == 0);
	return 0;
}

static int testRowRangeLeavesOtherRowsUntouched()
{
	const char *testName = "testRowRangeLeavesOtherRowsUntouched";
	const unsigned int pixels[] = {
		0xA0123456, 0xFFABCDEF, 0x00010203, 0xDEADBEEF,
		0xFFFFFFFF, 0x13579BDF, 0x2468ACE0, 0xDEADBEEF,
		0x80C0FFEE, 0x7F112233, 0xB0445566, 0xDEADBEEF
	};
	const unsigned char expected[] = {
		0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD,
		0xFF, 0xFF, 0xFF, 0x57, 0x9B, 0xDF, 0x68, 0xAC, 0xE0,
		0xC0, 0xFF, 0xEE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66
	};
	unsigned char converted[3 * 3 * 3];
	unsigned index;
	ScreenshotPixelSource source;

	for (index = 0; index < sizeof(converted); ++index)
	{
		converted[index] = 0xCD;
	}
	source.pixels = reinterpret_cast<const unsigned char *>(pixels);
	source.width = 3;
	source.height = 3;
	source.pitch = 4 * sizeof(unsigned int);
	source.format = SCREENSHOT_SOURCE_ARGB32;
	ConvertScreenshotRows(source, 1, source.height, converted);
	CHECK(testName, checkBytes(converted, expected, sizeof(converted), testName) == 0);
	return 0;
}

int main()
{
	int result = 0;
	result |= testArgb32SerialConversionUsesPitch();
	result |= testRgb565SerialConversionUsesPitch();
	result |= testRowRangeLeavesOtherRowsUntouched();
	return result;
}
