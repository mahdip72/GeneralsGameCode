#include "W3DDevice/GameClient/W3DScreenshotCodec.h"
#include "Lib/JobSystem.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#endif

enum
{
	TWO_WORKER_GATE_TIMEOUT_MS = 5000
};

#if !defined(_WIN32)
static void deadlineAfter(unsigned timeoutMilliseconds, struct timespec &deadline)
{
	struct timeval now;
	gettimeofday(&now, 0);
	deadline.tv_sec = now.tv_sec + timeoutMilliseconds / 1000;
	deadline.tv_nsec = now.tv_usec * 1000 +
		(timeoutMilliseconds % 1000) * 1000000;
	if (deadline.tv_nsec >= 1000000000)
	{
		++deadline.tv_sec;
		deadline.tv_nsec -= 1000000000;
	}
}
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

class WorkerGate
{
public:
	explicit WorkerGate(unsigned targetCount)
#if defined(_WIN32)
		: m_targetEntered(CreateEvent(0, TRUE, FALSE, 0)),
		  m_open(CreateEvent(0, TRUE, FALSE, 0)),
		  m_enteredCount(0), m_targetCount(targetCount)
#endif
	{
#if !defined(_WIN32)
		pthread_mutex_init(&m_mutex, 0);
		pthread_cond_init(&m_condition, 0);
		m_enteredCount = 0;
		m_open = false;
		m_targetCount = targetCount;
#endif
	}

	~WorkerGate()
	{
#if defined(_WIN32)
		if (m_targetEntered != 0)
		{
			CloseHandle(m_targetEntered);
		}
		if (m_open != 0)
		{
			CloseHandle(m_open);
		}
#else
		pthread_cond_destroy(&m_condition);
		pthread_mutex_destroy(&m_mutex);
#endif
	}

	void enterAndWait()
	{
#if defined(_WIN32)
		if (InterlockedIncrement(&m_enteredCount) >= (LONG)m_targetCount &&
			m_targetEntered != 0)
		{
			SetEvent(m_targetEntered);
		}
		if (m_open != 0)
		{
			WaitForSingleObject(m_open, TWO_WORKER_GATE_TIMEOUT_MS);
		}
		InterlockedDecrement(&m_enteredCount);
#else
		struct timespec deadline;
		int waitResult = 0;
		deadlineAfter(TWO_WORKER_GATE_TIMEOUT_MS, deadline);
		pthread_mutex_lock(&m_mutex);
		++m_enteredCount;
		pthread_cond_broadcast(&m_condition);
		while (!m_open && waitResult == 0)
		{
			waitResult = pthread_cond_timedwait(&m_condition, &m_mutex, &deadline);
		}
		--m_enteredCount;
		pthread_mutex_unlock(&m_mutex);
#endif
	}

	bool waitForTargetEntries(unsigned timeoutMilliseconds)
	{
#if defined(_WIN32)
		return m_targetEntered != 0 && m_open != 0 &&
			WaitForSingleObject(m_targetEntered, timeoutMilliseconds) == WAIT_OBJECT_0;
#else
		struct timespec deadline;
		int waitResult = 0;
		bool reached;

		deadlineAfter(timeoutMilliseconds, deadline);
		pthread_mutex_lock(&m_mutex);
		while (m_enteredCount < m_targetCount && waitResult == 0)
		{
			waitResult = pthread_cond_timedwait(&m_condition, &m_mutex, &deadline);
		}
		reached = m_enteredCount >= m_targetCount;
		pthread_mutex_unlock(&m_mutex);
		return reached;
#endif
	}

	void open()
	{
#if defined(_WIN32)
		if (m_open != 0)
		{
			SetEvent(m_open);
		}
#else
		pthread_mutex_lock(&m_mutex);
		m_open = true;
		pthread_cond_broadcast(&m_condition);
		pthread_mutex_unlock(&m_mutex);
#endif
	}

private:
	WorkerGate(const WorkerGate &);
	WorkerGate &operator=(const WorkerGate &);

#if defined(_WIN32)
	HANDLE m_targetEntered;
	HANDLE m_open;
	LONG m_enteredCount;
	unsigned m_targetCount;
#else
	pthread_mutex_t m_mutex;
	pthread_cond_t m_condition;
	unsigned m_enteredCount;
	bool m_open;
	unsigned m_targetCount;
#endif
};

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
	CHECK(testName, checkRanges(1080, 8, 16, testName) == 0);
	CHECK(testName, checkRanges(1080, 1, 1, testName) == 0);
	CHECK(testName, BuildScreenshotRowRanges(1080, 2, &range, 1) == 1);
	CHECK(testName, range.yBegin == 0 && range.yEnd == 1080);
	CHECK(testName, BuildScreenshotRowRanges(0, 2, &range, 1) == 0);
	CHECK(testName, BuildScreenshotRowRanges(128, 2, 0, 1) == 0);
	CHECK(testName, BuildScreenshotRowRanges(128, 2, &range, 0) == 0);
	return 0;
}

class ConvertRangeTask : public rts::Job
{
public:
	ConvertRangeTask(const ScreenshotPixelSource &source, const ScreenshotRowRange &range,
		unsigned char *destination, WorkerGate *gate)
		: m_source(source), m_range(range), m_destination(destination), m_gate(gate)
	{
	}

	virtual void execute(rts::JobContext &)
	{
		m_gate->enterAndWait();
		ConvertScreenshotRows(m_source, m_range.yBegin, m_range.yEnd, m_destination);
	}

private:
	ScreenshotPixelSource m_source;
	ScreenshotRowRange m_range;
	unsigned char *m_destination;
	WorkerGate *m_gate;
};

static int checkParallelConversion(const ScreenshotPixelSource &source,
	const char *testName, bool localCapacity)
{
	ScreenshotRowRange ranges[16];
	rts::Job *tasks[16];
	rts::JobSubmission submissions[16];
	rts::JobHandle handles[16];
	const unsigned byteCount = source.width * source.height * 3;
	unsigned char *serial = new unsigned char[byteCount];
	unsigned char *striped = new unsigned char[byteCount];
	rts::JobSystem &system = rts::JobSystem::instance();
	const unsigned workerCounts[] = { 1, 2, 4, 8, 16 };
	unsigned rangeCount;
	unsigned index;
	unsigned workerIndex;
	int result = 0;

	memset(serial, 0, byteCount);
	convertFullImage(source, serial);
	system.shutdown();
	for (workerIndex = 0;
		workerIndex < sizeof(workerCounts) / sizeof(workerCounts[0]);
		++workerIndex)
	{
		const unsigned effectiveWorkerCount =
			rts_test::ResolveActualWorkerCount(workerCounts[workerIndex],
			localCapacity);
		rts_test::PrintWorkerCountSubstitution(testName,
			workerCounts[workerIndex], effectiveWorkerCount, localCapacity);
		memset(striped, 0xCD, byteCount);
		rts::JobSystemConfig config;
		config.workerCount = effectiveWorkerCount;
		config.queueCapacity = 16;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		if (!system.start(config))
		{
			fprintf(stderr, "%s: failed to start %u-worker runtime\n",
				testName, effectiveWorkerCount);
			result = 1;
			continue;
		}
		const unsigned actualWorkerCount = system.workerCount();
		const unsigned observedTarget = actualWorkerCount > 3 ?
			3 : actualWorkerCount;
		WorkerGate gate(observedTarget);
		result |= check(actualWorkerCount != 0 &&
			actualWorkerCount <= effectiveWorkerCount, testName,
			"screenshot worker count respects the requested upper bound");
		if (actualWorkerCount == 1)
		{
			/* The VC6 oracle and forced-reference mode execute submissions
			 * synchronously, so their single range must not wait on its caller. */
			gate.open();
		}
		rangeCount = BuildScreenshotRowRanges(source.height, actualWorkerCount,
			ranges, sizeof(ranges) / sizeof(ranges[0]));
		rts::JobGroup group = system.createGroup();
		for (index = 0; index < rangeCount; ++index)
		{
			tasks[index] = new ConvertRangeTask(source, ranges[index], striped, &gate);
			submissions[index].job = tasks[index];
			submissions[index].priority = rts::JOB_PRIORITY_BACKGROUND;
		}
		if (!group.isValid() || !system.trySubmitBatch(submissions, rangeCount,
			group, handles))
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
			const bool reachedConfiguredConcurrency =
				gate.waitForTargetEntries(TWO_WORKER_GATE_TIMEOUT_MS);
			gate.open();
			const bool waited = system.wait(group);
			bool handlesSucceeded = true;
			for (index = 0; index < rangeCount; ++index)
			{
				handlesSucceeded = handlesSucceeded && handles[index].isValid() &&
					handles[index].succeeded();
			}
			result |= check(waited && !group.failed() && !group.wasCancelled() &&
				handlesSucceeded, testName,
				"conversion scheduler group and handles complete successfully");
			result |= check(reachedConfiguredConcurrency, testName,
				"conversion tasks use the configured worker-count matrix");
			result |= check(system.metrics().maximumActiveWorkers >= observedTarget,
				testName,
				"screenshot telemetry observes configured concurrency");
			result |= checkBytes(striped, serial, byteCount, testName);
		}
		system.shutdown();
	}

	delete[] serial;
	delete[] striped;
	return result;
}

static int testArgb32ParallelConversionMatchesSerial(bool localCapacity)
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
	return checkParallelConversion(source, testName, localCapacity);
}

static int testRgb565ParallelConversionMatchesSerial(bool localCapacity)
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
	return checkParallelConversion(source, testName, localCapacity);
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

int main(int argc, char **argv)
{
	bool localCapacity = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity))
	{
		fprintf(stderr, "Usage: core_screenshot_codec_tests [--local-capacity]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	int result = 0;
	result |= testRowRangePlanning();
	result |= testArgb32SerialConversionUsesPitch();
	result |= testRgb565SerialConversionUsesPitch();
	result |= testRowRangeLeavesOtherRowsUntouched();
	result |= testArgb32ParallelConversionMatchesSerial(localCapacity);
	result |= testRgb565ParallelConversionMatchesSerial(localCapacity);
	return result;
}
