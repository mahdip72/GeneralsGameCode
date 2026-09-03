/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/SortingTriangleKernel.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <limits>
#include <vector>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <thread>
#if defined(_WIN32)
#include <float.h>
#include <xmmintrin.h>
#endif
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_sorting_triangle_set_test_fault(unsigned fault,
	unsigned occurrence);
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
extern "C" void rts_job_system_set_test_pause_mask(unsigned pauseMask);
extern "C" bool rts_job_system_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds);
extern "C" void rts_job_system_release_test_pause(unsigned pausePoint);
#endif

namespace
{
using namespace rts;
unsigned failures = 0;

#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), #condition); } } while (0)

// The first three fields intentionally match the source prefix consumed by
// SortingTriangleDescriptor. The remaining fields make the stride realistic
// for VertexFormatXYZNDUV2 without coupling this fixture to WW3D headers.
struct FixtureVertex
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	unsigned diffuse;
	float u1;
	float v1;
	float u2;
	float v2;
};

struct Fixture
{
	std::vector<FixtureVertex> vertices[3];
	std::vector<unsigned short> indices[3];
	std::vector<SortingTriangleDescriptor> descriptors;
	unsigned polygonCount;
};

void setVertex(FixtureVertex &vertex, float x, float y, float z)
{
	vertex.x = x;
	vertex.y = y;
	vertex.z = z;
	vertex.nx = vertex.ny = vertex.nz = 0.0f;
	vertex.diffuse = 0;
	vertex.u1 = vertex.v1 = vertex.u2 = vertex.v2 = 0.0f;
}

void addNode(Fixture &fixture, unsigned node, unsigned short minIndex,
	unsigned short vertexCount, unsigned short polygonCount,
	unsigned vertexOffset, unsigned nodeIndex, bool commonZ)
{
	std::vector<FixtureVertex> &vertices = fixture.vertices[node];
	std::vector<unsigned short> &indices = fixture.indices[node];
	vertices.resize(static_cast<unsigned>(minIndex) + vertexCount + 2);
	for (unsigned index = 0; index != vertices.size(); ++index)
	{
		const float z = commonZ ? 1.25f : static_cast<float>(index + node * 3);
		setVertex(vertices[index], static_cast<float>(index + node),
			static_cast<float>(index * 2 + node), z);
	}
	indices.resize(static_cast<unsigned>(polygonCount) * 3);
	for (unsigned polygon = 0; polygon != polygonCount; ++polygon)
	{
		const unsigned first = polygon % vertexCount;
		indices[polygon * 3] = static_cast<unsigned short>(minIndex + first);
		indices[polygon * 3 + 1] = static_cast<unsigned short>(
			minIndex + ((first + 1) % vertexCount));
		indices[polygon * 3 + 2] = static_cast<unsigned short>(
			minIndex + ((first + 2) % vertexCount));
	}

	SortingTriangleDescriptor descriptor;
	descriptor.vertices = reinterpret_cast<const unsigned char *>(
		&vertices[minIndex]);
	descriptor.vertexStrideBytes = sizeof(FixtureVertex);
	descriptor.indices = indices.empty() ? 0 : &indices[0];
	descriptor.minVertexIndex = minIndex;
	descriptor.vertexCount = vertexCount;
	descriptor.polygonCount = polygonCount;
	descriptor.vertexOffset = vertexOffset;
	descriptor.outputOffset = fixture.polygonCount;
	descriptor.nodeIndex = nodeIndex;
	descriptor.zX = commonZ ? 0.0f : 0.25f;
	descriptor.zY = commonZ ? 0.0f : -0.5f;
	descriptor.zZ = commonZ ? 1.0f : 0.75f;
	descriptor.zTranslation = commonZ ? 0.0f : 3.0f;
	descriptor.commonZ = commonZ ? 1u : 0u;
	fixture.descriptors.push_back(descriptor);
	fixture.polygonCount += polygonCount;
}

void makeFixture(Fixture &fixture)
{
	fixture.polygonCount = 0;
	fixture.descriptors.clear();
	addNode(fixture, 0, 2, 11, 1536, 7, 0, true);
	addNode(fixture, 1, 1, 13, 1536, 65530, 1, false);
	addNode(fixture, 2, 3, 9, 1536, 65534, 2, true);
	// Exercise signed zero and nonfinite values without changing the index
	// layout or the owner-provided offsets.
	fixture.vertices[0][2].z = -0.0f;
	fixture.vertices[1][1].x = -0.0f;
	fixture.vertices[1][2].y = std::numeric_limits<float>::infinity();
	fixture.vertices[2][3].z = std::numeric_limits<float>::quiet_NaN();
}

void prepareLegacyOne(const SortingTriangleDescriptor &descriptor,
	unsigned polygon, SortingTriangleOutput &output)
{
	const unsigned short *indices = descriptor.indices;
	unsigned short idx1 = indices[polygon * 3] - descriptor.minVertexIndex;
	unsigned short idx2 = indices[polygon * 3 + 1] - descriptor.minVertexIndex;
	unsigned short idx3 = indices[polygon * 3 + 2] - descriptor.minVertexIndex;
	const unsigned char *bytes = descriptor.vertices;
	const float *v1 = reinterpret_cast<const float *>(bytes +
		static_cast<size_t>(idx1) * descriptor.vertexStrideBytes);
	const float *v2 = reinterpret_cast<const float *>(bytes +
		static_cast<size_t>(idx2) * descriptor.vertexStrideBytes);
	const float *v3 = reinterpret_cast<const float *>(bytes +
		static_cast<size_t>(idx3) * descriptor.vertexStrideBytes);
	output.tri.i = idx1 + descriptor.vertexOffset;
	output.tri.j = idx2 + descriptor.vertexOffset;
	output.tri.k = idx3 + descriptor.vertexOffset;
	output.idx = descriptor.nodeIndex;
	if (descriptor.commonZ != 0)
		output.z = (v1[2] + v2[2] + v3[2]) / 3.0f;
	else
		output.z = (descriptor.zX * (v1[0] + v2[0] + v3[0]) +
			descriptor.zY * (v1[1] + v2[1] + v3[1]) +
			descriptor.zZ * (v1[2] + v2[2] + v3[2])) / 3.0f +
			descriptor.zTranslation;
}

void prepareLegacy(const std::vector<SortingTriangleDescriptor> &descriptors,
	unsigned polygonCount, std::vector<SortingTriangleOutput> &output)
{
	unsigned polygonOffset = 0;
	for (unsigned node = 0; node != descriptors.size(); ++node)
	{
		for (unsigned polygon = 0; polygon != descriptors[node].polygonCount;
			++polygon)
			prepareLegacyOne(descriptors[node], polygon,
				output[polygonOffset + polygon]);
		polygonOffset += descriptors[node].polygonCount;
	}
	CHECK(polygonOffset == polygonCount);
}

bool sameBytes(const std::vector<SortingTriangleOutput> &left,
	const std::vector<SortingTriangleOutput> &right)
{
	return left.size() == right.size() &&
		(left.empty() || memcmp(&left[0], &right[0],
			sizeof(SortingTriangleOutput) * left.size()) == 0);
}

void copyDescriptors(const Fixture &fixture,
	SortingTriangleScratchLease &lease)
{
	CHECK(lease.descriptorCount() == fixture.descriptors.size());
	for (unsigned index = 0; index != fixture.descriptors.size(); ++index)
		lease.descriptors()[index] = fixture.descriptors[index];
}

void fillSentinel(std::vector<SortingTriangleOutput> &output)
{
	for (unsigned index = 0; index != output.size(); ++index)
	{
		output[index].tri.i = 0xaaaa;
		output[index].tri.j = 0xbbbb;
		output[index].tri.k = 0xcccc;
		output[index].idx = 0xdddd;
		output[index].z = 9876.5f;
	}
}

bool isSentinel(const SortingTriangleOutput &output)
{
	return output.tri.i == 0xaaaa && output.tri.j == 0xbbbb &&
		output.tri.k == 0xcccc && output.idx == 0xdddd &&
		output.z == 9876.5f;
}

bool allSentinel(const std::vector<SortingTriangleOutput> &output)
{
	for (unsigned index = 0; index != output.size(); ++index)
	{
		if (!isSentinel(output[index]))
			return false;
	}
	return true;
}

int runWorkerConfiguration(unsigned requestedWorkers)
{
	int result = 0;
	Fixture fixture;
	makeFixture(fixture);
	std::vector<SortingTriangleOutput> expected(fixture.polygonCount);
	std::vector<SortingTriangleOutput> actual(fixture.polygonCount);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, expected);

	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = requestedWorkers;
	config.queueCapacity = 8192;
	if (!jobs.start(config))
	{
		CHECK(false);
		return 1;
	}

	SortingTriangleScratchLease lease;
	SortingTriangleOptions options;
	options.parallel = false;
	CHECK(lease.prepare(static_cast<unsigned>(fixture.descriptors.size()),
		fixture.polygonCount, options.maximumScratchBytes));
	copyDescriptors(fixture, lease);
	fillSentinel(actual);
	SortingTriangleMetrics metrics;
	SortingTriangleResult serialResult = PrepareSortingTriangles(
		lease.descriptors(), lease.descriptorCount(), fixture.polygonCount,
		&actual[0], lease.outputs(), options, &metrics);
	CHECK(serialResult == SORTING_TRIANGLE_SERIAL);
	CHECK(sameBytes(expected, actual));
	CHECK(metrics.submittedJobs == 0);

	fillSentinel(actual);
	options.parallel = true;
	metrics = SortingTriangleMetrics();
	SortingTriangleResult parallelResult = PrepareSortingTriangles(
		lease.descriptors(), lease.descriptorCount(), fixture.polygonCount,
		&actual[0], lease.outputs(), options, &metrics);
	if (parallelResult == SORTING_TRIANGLE_PARALLEL)
	{
		CHECK(sameBytes(expected, actual));
		CHECK(metrics.completedJobs == metrics.submittedJobs);
	}
	else
	{
		CHECK(parallelResult == SORTING_TRIANGLE_SERIAL_FALLBACK);
		CHECK(allSentinel(actual));
		prepareLegacy(fixture.descriptors, fixture.polygonCount, actual);
		CHECK(sameBytes(expected, actual));
	}
	CHECK(JobSystem::instance().outstandingJobCount() == 0);
	printf("sorting triangle workers: requested=%u actual=%u result=%d jobs=%u\n",
		requestedWorkers, jobs.workerCount(), static_cast<int>(parallelResult),
		metrics.submittedJobs);
	jobs.shutdown();
	return result;
}

void testTinyAndInvalid()
{
	Fixture fixture;
	makeFixture(fixture);
	fixture.descriptors.resize(1);
	fixture.descriptors[0].polygonCount = 2;
	fixture.descriptors[0].outputOffset = 0;
	fixture.polygonCount = 2;
	std::vector<SortingTriangleOutput> expected(2), actual(2);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, expected);
	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 128;
	CHECK(jobs.start(config));
	SortingTriangleScratchLease lease;
	SortingTriangleOptions options;
	CHECK(lease.prepare(1, 2, options.maximumScratchBytes));
	copyDescriptors(fixture, lease);
	fillSentinel(actual);
	CHECK(PrepareSortingTriangles(lease.descriptors(), 1, 2, &actual[0],
		lease.outputs(), options) == SORTING_TRIANGLE_SERIAL);
	CHECK(sameBytes(expected, actual));

	SortingTriangleDescriptor saved = lease.descriptors()[0];
	lease.descriptors()[0].vertexStrideBytes = sizeof(float);
	fillSentinel(actual);
	CHECK(PrepareSortingTriangles(lease.descriptors(), 1, 2, &actual[0],
		lease.outputs(), options) == SORTING_TRIANGLE_INVALID_INPUT);
	CHECK(allSentinel(actual));
	lease.descriptors()[0] = saved;

	fillSentinel(actual);
	CHECK(PrepareSortingTriangles(lease.descriptors(), 1, 2, &actual[0],
		&actual[1], options) == SORTING_TRIANGLE_INVALID_INPUT);
	CHECK(allSentinel(actual));

	JobGroup cancelled = jobs.createGroup();
	CHECK(jobs.cancel(cancelled));
	options.cancellationGroup = &cancelled;
	fillSentinel(actual);
	CHECK(PrepareSortingTriangles(lease.descriptors(), 1, 2, &actual[0],
		lease.outputs(), options) == SORTING_TRIANGLE_CANCELLED);
	CHECK(allSentinel(actual));
	options.cancellationGroup = 0;
	jobs.shutdown();
}

#if defined(RTS_BUILD_CORE_EXTRAS) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
void checkFallbackPreservesOutput(unsigned fault, unsigned occurrence,
	bool jobSystemFault)
{
	Fixture fixture;
	makeFixture(fixture);
	std::vector<SortingTriangleOutput> expected(fixture.polygonCount);
	std::vector<SortingTriangleOutput> actual(fixture.polygonCount);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, expected);
	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	CHECK(jobs.start(config));
	SortingTriangleScratchLease lease;
	SortingTriangleOptions options;
	CHECK(lease.prepare(static_cast<unsigned>(fixture.descriptors.size()),
		fixture.polygonCount, options.maximumScratchBytes));
	copyDescriptors(fixture, lease);
	fillSentinel(actual);
	if (jobSystemFault)
		rts_job_system_set_test_fault(fault, occurrence);
	else
		rts_sorting_triangle_set_test_fault(fault, occurrence);
	SortingTriangleMetrics metrics;
	SortingTriangleResult result = PrepareSortingTriangles(
		lease.descriptors(), lease.descriptorCount(), fixture.polygonCount,
		&actual[0], lease.outputs(), options, &metrics);
	CHECK(result == SORTING_TRIANGLE_SERIAL_FALLBACK);
	CHECK(allSentinel(actual));
	if (jobSystemFault)
		rts_job_system_set_test_fault(0, 0);
	else
		rts_sorting_triangle_set_test_fault(0, 0);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, actual);
	CHECK(sameBytes(expected, actual));
	CHECK(jobs.outstandingJobCount() == 0);
	jobs.shutdown();
}

void testScratchAllocationFailure()
{
	Fixture fixture;
	makeFixture(fixture);
	std::vector<SortingTriangleOutput> expected(fixture.polygonCount);
	std::vector<SortingTriangleOutput> actual(fixture.polygonCount);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, expected);
	SortingTriangleScratchLease lease;
	rts_sorting_triangle_set_test_fault(1, 1);
	CHECK(!lease.prepare(static_cast<unsigned>(fixture.descriptors.size()),
		fixture.polygonCount, SORTING_TRIANGLE_DEFAULT_MAXIMUM_SCRATCH_BYTES));
	rts_sorting_triangle_set_test_fault(0, 0);
	// The renderer's full-original fallback owns the target when its bounded
	// workspace cannot be acquired. Keep the parity check explicit here.
	fillSentinel(actual);
	CHECK(allSentinel(actual));
	prepareLegacy(fixture.descriptors, fixture.polygonCount, actual);
	CHECK(sameBytes(expected, actual));
}

void testFailurePaths()
{
	// Fault 2 rejects a job allocation; fault 3 fails an accepted job. The
	// runtime faults exercise group creation and queue admission respectively.
	checkFallbackPreservesOutput(2, 1, false);
	checkFallbackPreservesOutput(3, 1, false);
	checkFallbackPreservesOutput(4, 1, true);
	checkFallbackPreservesOutput(5, 1, true);

	Fixture fixture;
	makeFixture(fixture);
	std::vector<SortingTriangleOutput> expected(fixture.polygonCount);
	std::vector<SortingTriangleOutput> actual(fixture.polygonCount);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, expected);
	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	CHECK(jobs.start(config));
	SortingTriangleScratchLease lease;
	SortingTriangleOptions options;
	CHECK(lease.prepare(static_cast<unsigned>(fixture.descriptors.size()),
		fixture.polygonCount, options.maximumScratchBytes));
	copyDescriptors(fixture, lease);
	fillSentinel(actual);
	SortingTriangleMetrics metrics;
	rts_job_system_set_test_fault(6, 1);
	SortingTriangleResult result = PrepareSortingTriangles(
		lease.descriptors(), lease.descriptorCount(), fixture.polygonCount,
		&actual[0], lease.outputs(), options, &metrics);
	CHECK(result == SORTING_TRIANGLE_SERIAL_FALLBACK);
	CHECK(allSentinel(actual));
	rts_job_system_set_test_fault(0, 0);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, actual);
	CHECK(sameBytes(expected, actual));
	jobs.shutdown();
}

void testActiveCancellation()
{
	Fixture fixture;
	makeFixture(fixture);
	std::vector<SortingTriangleOutput> actual(fixture.polygonCount);
	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	CHECK(jobs.start(config));
	SortingTriangleScratchLease lease;
	SortingTriangleOptions options;
	CHECK(lease.prepare(static_cast<unsigned>(fixture.descriptors.size()),
		fixture.polygonCount, options.maximumScratchBytes));
	copyDescriptors(fixture, lease);
	fillSentinel(actual);
	JobGroup cancellation = jobs.createGroup();
	options.cancellationGroup = &cancellation;
	SortingTriangleResult result = SORTING_TRIANGLE_INVALID_INPUT;
	rts_job_system_set_test_pause_mask(32);
	std::thread producer([&]() {
		SortingTriangleMetrics metrics;
		result = PrepareSortingTriangles(lease.descriptors(),
			lease.descriptorCount(), fixture.polygonCount, &actual[0],
			lease.outputs(), options, &metrics);
	});
	CHECK(rts_job_system_wait_for_test_pause(32, 5000));
	CHECK(jobs.cancel(cancellation));
	rts_job_system_release_test_pause(32);
	producer.join();
	rts_job_system_set_test_pause_mask(0);
	CHECK(result == SORTING_TRIANGLE_CANCELLED);
	CHECK(allSentinel(actual));
	CHECK(jobs.outstandingJobCount() == 0);
	jobs.shutdown();
}

#if defined(_WIN32)
void testFloatingPointParity()
{
	Fixture fixture;
	makeFixture(fixture);
	std::vector<SortingTriangleOutput> expected(fixture.polygonCount), actual(
		fixture.polygonCount);
	JobSystem &jobs = JobSystem::instance();
	jobs.shutdown();
	JobSystemConfig config = jobs.startupConfig();
	config.workerCount = 4;
	config.queueCapacity = 8192;
	CHECK(jobs.start(config));
	SortingTriangleScratchLease lease;
	SortingTriangleOptions options;
	CHECK(lease.prepare(static_cast<unsigned>(fixture.descriptors.size()),
		fixture.polygonCount, options.maximumScratchBytes));
	copyDescriptors(fixture, lease);
	const unsigned savedMxcsr = _mm_getcsr();
#if !defined(_WIN64)
	const unsigned savedControl = _controlfp(0, 0);
	_controlfp(_PC_53 | _RC_DOWN, _MCW_PC | _MCW_RC);
#endif
	const unsigned altered = (savedMxcsr & ~0x6000u) | 0x2000u | 0x8000u;
	_mm_setcsr(altered);
	prepareLegacy(fixture.descriptors, fixture.polygonCount, expected);
	fillSentinel(actual);
	options.minimumGrain = 16;
	SortingTriangleMetrics metrics;
	SortingTriangleResult result = PrepareSortingTriangles(
		lease.descriptors(), lease.descriptorCount(), fixture.polygonCount,
		&actual[0], lease.outputs(), options, &metrics);
	CHECK(result == SORTING_TRIANGLE_PARALLEL ||
		result == SORTING_TRIANGLE_SERIAL_FALLBACK);
	if (result == SORTING_TRIANGLE_SERIAL_FALLBACK)
	{
		CHECK(allSentinel(actual));
		prepareLegacy(fixture.descriptors, fixture.polygonCount, actual);
	}
	CHECK(sameBytes(expected, actual));
	CHECK((_mm_getcsr() & 0xffc0u) == (altered & 0xffc0u));
#if !defined(_WIN64)
	CHECK((_controlfp(0, 0) & (_MCW_PC | _MCW_RC)) ==
		(_PC_53 | _RC_DOWN));
	_controlfp(savedControl, _MCW_PC | _MCW_RC);
#endif
	_mm_setcsr(savedMxcsr);
	jobs.shutdown();
}
#endif
#endif
}

int main(int argc, char **argv)
{
	bool localCapacity = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity))
	{
		fprintf(stderr, "Usage: core_sorting_triangle_kernel_tests [--local-capacity]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	int result = 0;
	const unsigned workers[] = { 1, 2, 4, 8, 16, 0 };
	for (unsigned index = 0; index != sizeof(workers) / sizeof(workers[0]);
		++index)
	{
		const unsigned effectiveWorkerCount =
			rts_test::ResolveActualWorkerCount(workers[index], localCapacity);
		rts_test::PrintWorkerCountSubstitution("sorting triangle",
			workers[index], effectiveWorkerCount, localCapacity);
		result |= runWorkerConfiguration(effectiveWorkerCount);
	}
	testTinyAndInvalid();
#if defined(RTS_BUILD_CORE_EXTRAS) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
	testScratchAllocationFailure();
	testFailurePaths();
	testActiveCancellation();
#if defined(_WIN32)
	testFloatingPointParity();
#endif
#endif
	return result | (failures != 0 ? 1 : 0);
}
