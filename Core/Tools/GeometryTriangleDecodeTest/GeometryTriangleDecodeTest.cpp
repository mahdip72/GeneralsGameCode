/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/GeometryTriangleDecode.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#define GEOMETRY_TRIANGLE_TEST_MODERN 1
#include <atomic>
#include <chrono>
#include <thread>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_geometry_triangle_set_test_fault(unsigned fault, unsigned occurrence);
extern "C" void rts_geometry_triangle_set_test_observer(void (*observer)());
#if defined(GEOMETRY_TRIANGLE_TEST_MODERN)
extern "C" void rts_job_system_set_test_fault(unsigned fault, unsigned occurrence);
extern "C" void rts_job_system_set_test_pause_mask(unsigned mask);
extern "C" bool rts_job_system_wait_for_test_pause(unsigned point, unsigned timeoutMilliseconds);
extern "C" void rts_job_system_release_test_pause(unsigned point);
#endif
#endif

namespace
{
using namespace rts;
unsigned failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), #condition); } } while (0)

// Independent W3dTriStruct fixture, deliberately not a kernel input type.
struct LegacyTriangle
{
	unsigned indices[3];
	unsigned attributes;
	float normal[3];
	float distance;
};
typedef char FixtureMustMatchW3dRecord[sizeof(LegacyTriangle) == 32 ? 1 : -1];

void setFloatBits(float &value, unsigned bits) { memcpy(&value, &bits, sizeof(value)); }

void makeFixture(std::vector<LegacyTriangle> &records, unsigned count)
{
	records.resize(count);
	const unsigned indices[] = { 0u, 1u, 65535u, 65536u, 0x80000000u, 0xffffffffu };
	const unsigned floats[] = { 0u, 0x80000000u, 0x3f800000u, 0xbf000000u,
		0x7fc12345u, 0xffc12345u, 0x7f800000u, 0xff800000u };
	for (unsigned i = 0; i != count; ++i)
	{
		LegacyTriangle &record = records[i];
		for (unsigned j = 0; j != 3; ++j)
		{
			record.indices[j] = indices[(i + j) % 6];
			setFloatBits(record.normal[j], floats[(i + j) % 8]);
		}
		record.attributes = i % 5 == 0 ? 0xffffffffu : i % 257;
		setFloatBits(record.distance, floats[(i + 3) % 8]);
	}
}

void capture(const std::vector<LegacyTriangle> &records, GeometryTriangleDecodeScratch &scratch,
	unsigned indexWidth = GEOMETRY_TRIANGLE_INDEX32_BYTES)
{
	CHECK(scratch.prepare(static_cast<unsigned>(records.size()), indexWidth));
	if (!records.empty() && scratch.records() != 0)
		memcpy(scratch.records(), &records[0], records.size() * sizeof(LegacyTriangle));
}

// Compare the original per-record statements to the three owner bulk copies.
// A canary precedes and follows every destination array.
// Keep Index in the signature: VC6 otherwise emits one symbol for both
// specializations when the template argument appears only in the body.
template<class Index> void checkOwnerPublication(const std::vector<LegacyTriangle> &records,
	const GeometryTriangleDecodeScratch &scratch, Index *)
{
	std::vector<Index> expectedIndices(records.size() * 3 + 2), actualIndices(records.size() * 3 + 2);
	std::vector<float> expectedPlanes(records.size() * 4 + 2), actualPlanes(records.size() * 4 + 2);
	std::vector<unsigned char> expectedSurfaces(records.size() + 2), actualSurfaces(records.size() + 2);
	memset(&expectedIndices[0], 0xa5, expectedIndices.size() * sizeof(Index));
	memset(&actualIndices[0], 0xa5, actualIndices.size() * sizeof(Index));
	memset(&expectedPlanes[0], 0xa5, expectedPlanes.size() * sizeof(float));
	memset(&actualPlanes[0], 0xa5, actualPlanes.size() * sizeof(float));
	memset(&expectedSurfaces[0], 0xa5, expectedSurfaces.size());
	memset(&actualSurfaces[0], 0xa5, actualSurfaces.size());
	bool attributesInRange = true;
	for (unsigned i = 0; i != records.size(); ++i)
	{
		for (unsigned j = 0; j != 3; ++j)
		{
			expectedIndices[i * 3 + j + 1] = static_cast<Index>(records[i].indices[j]);
			expectedPlanes[i * 4 + j + 1] = records[i].normal[j];
		}
		expectedPlanes[i * 4 + 4] = -records[i].distance;
		expectedSurfaces[i + 1] = static_cast<unsigned char>(records[i].attributes);
		if (records[i].attributes >= 256) attributesInRange = false;
	}
	memcpy(&actualIndices[1], scratch.indices(), records.size() * 3 * sizeof(Index));
	memcpy(&actualPlanes[1], scratch.planes(), records.size() * 4 * sizeof(float));
	memcpy(&actualSurfaces[1], scratch.surfaces(), records.size());
	if (memcmp(&expectedIndices[0], &actualIndices[0], expectedIndices.size() * sizeof(Index)) != 0)
	{
		for (unsigned slot = 0; slot != expectedIndices.size(); ++slot)
		{
			if (memcmp(&expectedIndices[slot], &actualIndices[slot], sizeof(Index)) == 0) continue;
			unsigned expectedBits = 0, actualBits = 0;
			memcpy(&expectedBits, &expectedIndices[slot], sizeof(Index));
			memcpy(&actualBits, &actualIndices[slot], sizeof(Index));
			const unsigned source = slot > 0 && slot <= records.size() * 3 ?
				records[(slot - 1) / 3].indices[(slot - 1) % 3] : 0xa5a5a5a5u;
			const unsigned short scalar16 = static_cast<unsigned short>(source);
			const int scalar32 = static_cast<int>(source);
			fprintf(stderr, "index mismatch: width=%u count=%u slot=%u expected=%08x actual=%08x source=%08x scalar16=%08x scalar32=%08x\n",
				static_cast<unsigned>(sizeof(Index)), static_cast<unsigned>(records.size()), slot,
				expectedBits, actualBits, source, static_cast<unsigned>(scalar16), static_cast<unsigned>(scalar32));
			break;
		}
	}
	CHECK(memcmp(&expectedIndices[0], &actualIndices[0], expectedIndices.size() * sizeof(Index)) == 0);
	CHECK(memcmp(&expectedPlanes[0], &actualPlanes[0], expectedPlanes.size() * sizeof(float)) == 0);
	CHECK(memcmp(&expectedSurfaces[0], &actualSurfaces[0], expectedSurfaces.size()) == 0);
	CHECK(scratch.attributesInRange() == attributesInRange);
}

void checkDecoded(const std::vector<LegacyTriangle> &records, const GeometryTriangleDecodeScratch &scratch)
{
	CHECK(scratch.indices() != 0 && scratch.planes() != 0 && scratch.surfaces() != 0);
	if (scratch.indices() == 0 || scratch.planes() == 0 || scratch.surfaces() == 0) return;
	CHECK(reinterpret_cast<size_t>(scratch.planes()) % sizeof(float) == 0);
	if (scratch.indexWidthBytes() == GEOMETRY_TRIANGLE_INDEX32_BYTES)
		checkOwnerPublication<int>(records, scratch, static_cast<int *>(0));
	else checkOwnerPublication<unsigned short>(records, scratch, static_cast<unsigned short *>(0));
}

void testSerialAndBudget()
{
	GeometryTriangleDecodeScratch scratch;
	GeometryTriangleDecodeOptions options;
	GeometryTriangleDecodeMetrics metrics;
	CHECK(DecodeGeometryTriangles(scratch, options) == GEOMETRY_TRIANGLE_INVALID_INPUT);
	CHECK(scratch.prepare(0, GEOMETRY_TRIANGLE_INDEX32_BYTES));
	CHECK(DecodeGeometryTriangles(scratch, options) == GEOMETRY_TRIANGLE_SERIAL);
	CHECK(scratch.indices() == 0 && scratch.records() == 0);
	CHECK(scratch.attributesInRange());
	const unsigned counts[] = { 1, 7, 1023, 16384 };
	for (unsigned i = 0; i != sizeof(counts) / sizeof(counts[0]); ++i)
	{
		std::vector<LegacyTriangle> records;
		makeFixture(records, counts[i]);
		for (unsigned width = 2; width <= 4; width += 2)
		{
			capture(records, scratch, width);
			CHECK(scratch.indices() == 0 && scratch.planes() == 0 && scratch.surfaces() == 0);
			CHECK(scratch.capacityBytes() <= GEOMETRY_TRIANGLE_DEFAULT_SCRATCH_BYTES);
			options.parallel = false;
			CHECK(DecodeGeometryTriangles(scratch, options, &metrics) == GEOMETRY_TRIANGLE_SERIAL);
			CHECK(metrics.submittedJobs == 0 && metrics.workerThreadsUsed == 0);
			checkDecoded(records, scratch);
			CHECK(memcmp(scratch.records(), &records[0], scratch.inputBytes()) == 0);
			if (counts[i] < GEOMETRY_TRIANGLE_MIN_PARALLEL_RECORDS)
			{
				options.parallel = true;
				CHECK(DecodeGeometryTriangles(scratch, options, &metrics) == GEOMETRY_TRIANGLE_SERIAL);
				CHECK(metrics.submittedJobs == 0);
				checkDecoded(records, scratch);
			}
		}
	}
	const unsigned width = GEOMETRY_TRIANGLE_INDEX32_BYTES;
	const unsigned perRecord = GEOMETRY_TRIANGLE_RECORD_BYTES + 3 * width + 4 * sizeof(float) + 1;
	CHECK(!scratch.prepare(UINT_MAX, width));
	CHECK(scratch.indices() == 0);
	CHECK(DecodeGeometryTriangles(scratch, options) == GEOMETRY_TRIANGLE_INVALID_INPUT);
	CHECK(!scratch.prepare(1, width, perRecord - 1));
	CHECK(!scratch.prepare(1, width, GEOMETRY_TRIANGLE_MAXIMUM_SCRATCH_BYTES + 1));
	CHECK(!scratch.prepare(1, width, 0));
	CHECK(!scratch.prepare(1, 3));
	CHECK(scratch.prepare(1, width, perRecord));
	CHECK(scratch.capacityBytes() == perRecord);
	CHECK(scratch.inputBytes() == GEOMETRY_TRIANGLE_RECORD_BYTES);
	CHECK(scratch.prepare(GEOMETRY_TRIANGLE_DEFAULT_SCRATCH_BYTES / perRecord, width));
	CHECK(scratch.capacityBytes() <= GEOMETRY_TRIANGLE_DEFAULT_SCRATCH_BYTES);
	CHECK(!scratch.prepare(GEOMETRY_TRIANGLE_DEFAULT_SCRATCH_BYTES / perRecord + 1, width));
	CHECK(!scratch.prepare(1, GEOMETRY_TRIANGLE_INDEX16_BYTES, 55));
	CHECK(scratch.prepare(1, GEOMETRY_TRIANGLE_INDEX16_BYTES, 57));
	CHECK(scratch.capacityBytes() == 57);
}

void startWorkers(unsigned workers)
{
	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = workers;
	config.queueCapacity = 4096;
	config.pinWorkers = false;
	CHECK(jobs.start(config));
}

void testWorkerCount(unsigned workers, unsigned indexWidth)
{
	startWorkers(workers);
	std::vector<LegacyTriangle> records;
	makeFixture(records, 16384);
	GeometryTriangleDecodeScratch scratch;
	capture(records, scratch, indexWidth);
	GeometryTriangleDecodeOptions options;
	GeometryTriangleDecodeMetrics metrics;
	GeometryTriangleDecodeResult result = DecodeGeometryTriangles(scratch, options, &metrics);
	CHECK(GeometryTriangleDecodeCompleted(result));
#if defined(GEOMETRY_TRIANGLE_TEST_MODERN)
	CHECK(result == (workers > 1 ? GEOMETRY_TRIANGLE_PARALLEL : GEOMETRY_TRIANGLE_SERIAL_FALLBACK));
	if (workers > 1)
	{
		CHECK(metrics.submittedJobs > 2);
		CHECK(metrics.completedJobs == metrics.submittedJobs);
	}
#else
	CHECK(result == GEOMETRY_TRIANGLE_SERIAL_FALLBACK);
#endif
	checkDecoded(records, scratch);
	CHECK(memcmp(scratch.records(), &records[0], scratch.inputBytes()) == 0);
	CHECK(JobSystem::instance().outstandingJobCount() == 0);
	JobSystem::instance().shutdown();
}

void testDisabledPolicy()
{
	CHECK(SetPipelineExecutionMode(PIPELINE_EXECUTION_SERIAL));
	startWorkers(4);
	for (unsigned width = 2; width <= 4; width += 2)
	{
		std::vector<LegacyTriangle> records;
		makeFixture(records, 4096);
		GeometryTriangleDecodeScratch scratch;
		capture(records, scratch, width);
		GeometryTriangleDecodeOptions options;
		GeometryTriangleDecodeMetrics metrics;
		CHECK(DecodeGeometryTriangles(scratch, options, &metrics) == GEOMETRY_TRIANGLE_SERIAL_FALLBACK);
		CHECK(metrics.submittedJobs == 0 && metrics.workerThreadsUsed == 0);
		checkDecoded(records, scratch);
	}
	CHECK(JobSystem::instance().outstandingJobCount() == 0);
	JobSystem::instance().shutdown();
}

#if defined(RTS_BUILD_CORE_EXTRAS)
void testAllocationFailure()
{
	GeometryTriangleDecodeScratch scratch;
	rts_geometry_triangle_set_test_fault(1, 1);
	CHECK(!scratch.prepare(2048, GEOMETRY_TRIANGLE_INDEX32_BYTES));
	CHECK(scratch.records() == 0 && scratch.indices() == 0 && scratch.capacityBytes() == 0);
	rts_geometry_triangle_set_test_fault(0, 0);
	CHECK(scratch.prepare(2048, GEOMETRY_TRIANGLE_INDEX32_BYTES));
}
#endif

#if defined(RTS_BUILD_CORE_EXTRAS) && defined(GEOMETRY_TRIANGLE_TEST_MODERN)
void checkFailure(unsigned fault, unsigned occurrence, bool runtime, unsigned indexWidth)
{
	startWorkers(4);
	std::vector<LegacyTriangle> records;
	makeFixture(records, 16384);
	GeometryTriangleDecodeScratch scratch;
	capture(records, scratch, indexWidth);
	GeometryTriangleDecodeOptions options;
	GeometryTriangleDecodeMetrics metrics;
	if (runtime) rts_job_system_set_test_fault(fault, occurrence);
	else rts_geometry_triangle_set_test_fault(fault, occurrence);
	CHECK(DecodeGeometryTriangles(scratch, options, &metrics) == GEOMETRY_TRIANGLE_SERIAL_FALLBACK);
	if (runtime) rts_job_system_set_test_fault(0, 0);
	else rts_geometry_triangle_set_test_fault(0, 0);
	CHECK(metrics.serialFallbacks == 1);
	if (occurrence == 2) CHECK(metrics.submittedJobs >= 1);
	checkDecoded(records, scratch);
	CHECK(memcmp(scratch.records(), &records[0], scratch.inputBytes()) == 0);
	CHECK(JobSystem::instance().outstandingJobCount() == 0);
	JobSystem::instance().shutdown();
}

void testFailurePaths()
{
	for (unsigned width = 2; width <= 4; width += 2)
	{
		checkFailure(2, 1, false, width); // first job allocation
		checkFailure(2, 2, false, width); // allocation after an accepted range
		checkFailure(3, 1, false, width); // accepted worker execution
		checkFailure(4, 1, true, width);  // job group creation
		checkFailure(5, 1, true, width);  // queue admission
		checkFailure(5, 2, true, width);  // queue admission after an accepted range
		checkFailure(6, 1, true, width);  // runtime execution failure
	}
}

std::atomic<unsigned> observerEntered(0);
std::atomic<bool> observerTimedOut(false);
void workerObserver()
{
	observerEntered.fetch_add(1);
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (observerEntered.load() < 2)
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{ observerTimedOut.store(true); break; }
		std::this_thread::yield();
	}
}

void testRealWorkerExecution()
{
	startWorkers(4);
	std::vector<LegacyTriangle> records;
	makeFixture(records, 16384);
	for (unsigned i = 0; i != records.size(); ++i) records[i].attributes = i % 256;
	GeometryTriangleDecodeScratch scratch;
	capture(records, scratch);
	GeometryTriangleDecodeOptions options;
	GeometryTriangleDecodeMetrics metrics;
	observerEntered.store(0);
	observerTimedOut.store(false);
	rts_geometry_triangle_set_test_observer(workerObserver);
	CHECK(DecodeGeometryTriangles(scratch, options, &metrics) == GEOMETRY_TRIANGLE_PARALLEL);
	rts_geometry_triangle_set_test_observer(0);
	CHECK(!observerTimedOut.load());
	CHECK(metrics.workerThreadsUsed > 0 && metrics.submittedJobs > 2);
	CHECK(scratch.attributesInRange());
	checkDecoded(records, scratch);
	fprintf(stdout, "geometry triangle workers=%u, ranges=%u\n",
		metrics.workerThreadsUsed, metrics.submittedJobs);
	JobSystem::instance().shutdown();
}

void testCancellation()
{
	startWorkers(4);
	JobSystem &jobs = JobSystem::instance();
	std::vector<LegacyTriangle> records;
	makeFixture(records, 16384);
	GeometryTriangleDecodeScratch scratch;
	capture(records, scratch);
	GeometryTriangleDecodeOptions options;
	JobGroup token = jobs.createGroup();
	CHECK(jobs.cancel(token));
	options.cancellationGroup = &token;
	CHECK(DecodeGeometryTriangles(scratch, options) == GEOMETRY_TRIANGLE_CANCELLED);
	CHECK(scratch.indices() == 0 && scratch.planes() == 0 && scratch.surfaces() == 0);

	token = jobs.createGroup();
	GeometryTriangleDecodeResult result = GEOMETRY_TRIANGLE_INVALID_INPUT;
	rts_job_system_set_test_pause_mask(32);
	std::thread producer([&]() { result = DecodeGeometryTriangles(scratch, options); });
	const bool paused = rts_job_system_wait_for_test_pause(32, 5000);
	CHECK(paused);
	CHECK(jobs.cancel(token));
	rts_job_system_release_test_pause(32);
	producer.join();
	rts_job_system_set_test_pause_mask(0);
	CHECK(result == GEOMETRY_TRIANGLE_CANCELLED);
	CHECK(scratch.indices() == 0 && scratch.planes() == 0 && scratch.surfaces() == 0);
	CHECK(memcmp(scratch.records(), &records[0], scratch.inputBytes()) == 0);
	CHECK(jobs.outstandingJobCount() == 0);
	// A subsequent owner call can publish the same captured bytes after the
	// cancelled call's complete fence, without reading the source again.
	options.cancellationGroup = 0;
	CHECK(GeometryTriangleDecodeCompleted(DecodeGeometryTriangles(scratch, options)));
	checkDecoded(records, scratch);
	jobs.shutdown();
}
#endif
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--serial-pipelines") == 0)
	{
		testDisabledPolicy();
		return failures != 0 ? 1 : 0;
	}
	testSerialAndBudget();
	for (unsigned width = 2; width <= 4; width += 2)
	{
		testWorkerCount(1, width);
		testWorkerCount(2, width);
		testWorkerCount(4, width);
	}
#if defined(RTS_BUILD_CORE_EXTRAS)
	testAllocationFailure();
#if defined(GEOMETRY_TRIANGLE_TEST_MODERN)
	testFailurePaths();
	testRealWorkerExecution();
	testCancellation();
#endif
#endif
	fprintf(stdout, "geometry triangle decode failures=%u\n", failures);
	return failures != 0 ? 1 : 0;
}
