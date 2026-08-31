/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"

namespace rts
{
// The renderer owns these views and keeps the corresponding sorting-buffer
// references alive until the synchronous preparation call has fenced.
struct SortingTriangleIndices
{
	unsigned short i;
	unsigned short j;
	unsigned short k;
};

struct SortingTriangleOutput
{
	SortingTriangleIndices tri;
	unsigned short idx;
	float z;
};

struct SortingTriangleDescriptor
{
	// Points at the x member of the first source vertex. The source format is
	// intentionally not named here so TaskRuntime never depends on WW3D types.
	const unsigned char *vertices;
	unsigned vertexStrideBytes;
	const unsigned short *indices;
	unsigned short minVertexIndex;
	unsigned short vertexCount;
	unsigned short polygonCount;
	unsigned vertexOffset;
	unsigned outputOffset;
	unsigned nodeIndex;
	float zX;
	float zY;
	float zZ;
	float zTranslation;
	unsigned commonZ;
};

enum
{
	SORTING_TRIANGLE_MAX_DESCRIPTORS = 4096,
	SORTING_TRIANGLE_MIN_PARALLEL_POLYGONS = 1024,
	SORTING_TRIANGLE_DEFAULT_MINIMUM_GRAIN = 256,
	SORTING_TRIANGLE_DEFAULT_MAXIMUM_SCRATCH_BYTES = 8 * 1024 * 1024,
	SORTING_TRIANGLE_MAXIMUM_SCRATCH_BYTES = 16 * 1024 * 1024
};

enum SortingTriangleResult
{
	SORTING_TRIANGLE_SERIAL,
	SORTING_TRIANGLE_PARALLEL,
	SORTING_TRIANGLE_SERIAL_FALLBACK,
	SORTING_TRIANGLE_CANCELLED,
	SORTING_TRIANGLE_INVALID_INPUT
};

struct SortingTriangleOptions
{
	SortingTriangleOptions();
	bool parallel;
	unsigned minimumGrain;
	unsigned maximumScratchBytes;
	// This token is observed by the owner before/after the synchronous call.
	// Worker cancellation uses its own JobGroup through JobContext, so workers
	// never dereference a live external group object.
	const JobGroup *cancellationGroup;
};

struct SortingTriangleMetrics
{
	SortingTriangleMetrics();
	unsigned submittedJobs;
	unsigned completedJobs;
	unsigned workerThreadsUsed;
	unsigned serialFallbacks;
	JobMetricCounter waitNanoseconds;
};

// One owner-thread lease is reused across calls. Storage is a bounded CRT
// arena; it never crosses an asynchronous frame or a worker thread.
class SortingTriangleScratchLease
{
public:
	SortingTriangleScratchLease();
	~SortingTriangleScratchLease();

	bool prepare(unsigned descriptorCount, unsigned outputCount,
		unsigned maximumBytes = SORTING_TRIANGLE_DEFAULT_MAXIMUM_SCRATCH_BYTES);
	SortingTriangleDescriptor *descriptors();
	SortingTriangleOutput *outputs();
	unsigned descriptorCount() const;
	unsigned outputCount() const;
	unsigned capacityBytes() const;
	unsigned allocationCount() const;

private:
	SortingTriangleScratchLease(const SortingTriangleScratchLease &);
	SortingTriangleScratchLease &operator=(const SortingTriangleScratchLease &);
	void *m_storage;
	unsigned m_capacityBytes;
	unsigned m_descriptorOffset;
	unsigned m_outputOffset;
	unsigned m_descriptorCount;
	unsigned m_outputCount;
	unsigned m_allocationCount;
};

bool SortingTriangleCompleted(SortingTriangleResult result);

// Inputs, output and scratch storage must remain valid for this synchronous
// call. Output and scratch must be disjoint. Every successful result writes all
// output polygon slots only after the worker fence. Invalid or cancelled calls
// leave output untouched; a serial-fallback result asks the owner to run its
// original reference loop after the accepted jobs have drained.
SortingTriangleResult PrepareSortingTriangles(
	const SortingTriangleDescriptor *descriptors,
	unsigned descriptorCount,
	unsigned polygonCount,
	SortingTriangleOutput *output,
	SortingTriangleOutput *scratchOutput,
	const SortingTriangleOptions &options,
	SortingTriangleMetrics *metrics = 0);
}
