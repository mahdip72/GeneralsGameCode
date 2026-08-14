#include "W3DDevice/GameClient/W3DScreenshotCodec.h"
#include "Lib/TaskRuntime.h"

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

static int checkRanges(unsigned height, unsigned workerCount, unsigned expectedCount,
	const char *testName)
{
	ScreenshotRowRange ranges[16];
	unsigned index;
	const unsigned rangeCount = BuildScreenshotRowRanges(height, workerCount,
		ranges, sizeof(ranges) / sizeof(ranges[0]));

	CHECK(testName, rangeCount == expectedCount);
	CHECK(testName, ranges[0].yBegin == 0);
	CHECK(testName, ranges[rangeCount - 1].yEnd == height);
	for (index = 0; index < rangeCount; ++index)
	{
		CHECK(testName, ranges[index].yBegin < ranges[index].yEnd);
		if (index > 0)
		{
			CHECK(testName, ranges[index - 1].yEnd == ranges[index].yBegin);
		}
	}
	return 0;
}

static int testRowRangePlanning()
{
	const char *testName = "testRowRangePlanning";
	ScreenshotRowRange range;

	CHECK(testName, checkRanges(1, 2, 1, testName) == 0);
	CHECK(testName, checkRanges(127, 2, 1, testName) == 0);
	CHECK(testName, checkRanges(128, 2, 2, testName) == 0);
	CHECK(testName, checkRanges(1080, 2, 4, testName) == 0);
	CHECK(testName, checkRanges(1080, 1, 1, testName) == 0);
	CHECK(testName, BuildScreenshotRowRanges(1080, 2, &range, 1) == 1);
	CHECK(testName, range.yBegin == 0 && range.yEnd == 1080);
	CHECK(testName, BuildScreenshotRowRanges(0, 2, &range, 1) == 0);
	CHECK(testName, BuildScreenshotRowRanges(128, 2, 0, 1) == 0);
	CHECK(testName, BuildScreenshotRowRanges(128, 2, &range, 0) == 0);
	return 0;
}

class ConvertRangeTask : public rts::Task
{
public:
	ConvertRangeTask(const ScreenshotPixelSource &source, const ScreenshotRowRange &range,
		unsigned char *destination)
		: m_source(source), m_range(range), m_destination(destination)
	{
	}

	virtual void execute()
	{
		ConvertScreenshotRows(m_source, m_range.yBegin, m_range.yEnd, m_destination);
	}

private:
	ScreenshotPixelSource m_source;
	ScreenshotRowRange m_range;
	unsigned char *m_destination;
};

static int checkParallelConversion(const ScreenshotPixelSource &source, const char *testName)
{
	ScreenshotRowRange ranges[16];
	rts::Task *tasks[16];
	const unsigned byteCount = source.width * source.height * 3;
	unsigned char *serial = new unsigned char[byteCount];
	unsigned char *striped = new unsigned char[byteCount];
	rts::TaskRuntime runtime;
	unsigned rangeCount;
	unsigned index;
	int result = 0;

	memset(serial, 0, byteCount);
	memset(striped, 0xCD, byteCount);
	convertFullImage(source, serial);
	if (!runtime.start(2, 16))
	{
		fprintf(stderr, "%s: failed to start task runtime\n", testName);
		result = 1;
	}
	else
	{
		rangeCount = BuildScreenshotRowRanges(source.height, runtime.workerCount(),
			ranges, sizeof(ranges) / sizeof(ranges[0]));
		for (index = 0; index < rangeCount; ++index)
		{
			tasks[index] = new ConvertRangeTask(source, ranges[index], striped);
		}
		if (!runtime.trySubmitBatch(tasks, rangeCount))
		{
			fprintf(stderr, "%s: failed to submit conversion batch\n", testName);
			for (index = 0; index < rangeCount; ++index)
			{
				delete tasks[index];
			}
			result = 1;
		}
		else
		{
			runtime.waitUntilIdle();
			result = checkBytes(striped, serial, byteCount, testName);
		}
		runtime.shutdown();
	}

	delete[] serial;
	delete[] striped;
	return result;
}

static int testArgb32ParallelConversionMatchesSerial()
{
	const char *testName = "testArgb32ParallelConversionMatchesSerial";
	const unsigned width = 3;
	const unsigned height = 129;
	const unsigned pitch = 16;
	unsigned char pixels[pitch * height];
	ScreenshotPixelSource source;
	unsigned y;

	memset(pixels, 0xA5, sizeof(pixels));
	for (y = 0; y < height; ++y)
	{
		unsigned int *row = reinterpret_cast<unsigned int *>(pixels + y * pitch);
		row[0] = 0xFF000000 | ((y & 0xFF) << 16) | 0x00001234;
		row[1] = 0xFFABC000 | (y & 0xFF);
		row[2] = 0xFF102030 | ((y & 0x0F) << 8);
	}
	source.pixels = pixels;
	source.width = width;
	source.height = height;
	source.pitch = pitch;
	source.format = SCREENSHOT_SOURCE_ARGB32;
	return checkParallelConversion(source, testName);
}

static int testRgb565ParallelConversionMatchesSerial()
{
	const char *testName = "testRgb565ParallelConversionMatchesSerial";
	const unsigned width = 5;
	const unsigned height = 131;
	const unsigned pitch = 12;
	unsigned char pixels[pitch * height];
	ScreenshotPixelSource source;
	unsigned y;

	memset(pixels, 0x5A, sizeof(pixels));
	for (y = 0; y < height; ++y)
	{
		unsigned short *row = reinterpret_cast<unsigned short *>(pixels + y * pitch);
		row[0] = (unsigned short)(0xF800 | (y & 0x001F));
		row[1] = (unsigned short)(0x07E0 | (y & 0x001F));
		row[2] = (unsigned short)(0x001F | ((y & 0x003F) << 5));
		row[3] = (unsigned short)(0x7BEF ^ y);
		row[4] = (unsigned short)(0xFFFF - y);
	}
	source.pixels = pixels;
	source.width = width;
	source.height = height;
	source.pitch = pitch;
	source.format = SCREENSHOT_SOURCE_RGB565;
	return checkParallelConversion(source, testName);
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
	result |= testRowRangePlanning();
	result |= testArgb32SerialConversionUsesPitch();
	result |= testRgb565SerialConversionUsesPitch();
	result |= testRowRangeLeavesOtherRowsUntouched();
	result |= testArgb32ParallelConversionMatchesSerial();
	result |= testRgb565ParallelConversionMatchesSerial();
	return result;
}
