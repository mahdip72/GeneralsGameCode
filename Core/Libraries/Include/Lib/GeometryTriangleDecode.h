/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"

namespace rts
{
enum
{
	GEOMETRY_TRIANGLE_RECORD_BYTES = 32,
	GEOMETRY_TRIANGLE_INDEX16_BYTES = 2,
	GEOMETRY_TRIANGLE_INDEX32_BYTES = 4,
	GEOMETRY_TRIANGLE_MIN_PARALLEL_RECORDS = 1024,
	GEOMETRY_TRIANGLE_DEFAULT_SCRATCH_BYTES = 4 * 1024 * 1024,
	GEOMETRY_TRIANGLE_MAXIMUM_SCRATCH_BYTES = 8 * 1024 * 1024
};

enum GeometryTriangleDecodeResult
{
	GEOMETRY_TRIANGLE_SERIAL,
	GEOMETRY_TRIANGLE_PARALLEL,
	GEOMETRY_TRIANGLE_SERIAL_FALLBACK,
	GEOMETRY_TRIANGLE_CANCELLED,
	GEOMETRY_TRIANGLE_INVALID_INPUT
};

struct GeometryTriangleDecodeOptions
{
	GeometryTriangleDecodeOptions();
	bool parallel;
	unsigned minimumGrain;
	// Observed only by the owner. Workers observe their private job group.
	const JobGroup *cancellationGroup;
};

struct GeometryTriangleDecodeMetrics
{
	GeometryTriangleDecodeMetrics();
	unsigned submittedJobs, completedJobs, workerThreadsUsed, serialFallbacks;
	JobMetricCounter waitNanoseconds;
};

class GeometryTriangleDecodeScratch;
GeometryTriangleDecodeResult DecodeGeometryTriangles(
	GeometryTriangleDecodeScratch &scratch,
	const GeometryTriangleDecodeOptions &options,
	GeometryTriangleDecodeMetrics *metrics = 0);

// A single owner-local CRT allocation accounts for BOTH input and output.
// The owner fills records before decoding; inputs then remain immutable until
// the synchronous fence. Output views are null until the whole result succeeds.
// Generals selects 32-bit indices and Zero Hour selects 16-bit indices. Workers
// preserve the original narrowing and produce three packed arrays for owner
// memcpy publication, without exposing any WW3D object or allocator to a job.
// The lease must not be shared across concurrent calls or retained by a job.
class GeometryTriangleDecodeScratch
{
public:
	GeometryTriangleDecodeScratch();
	~GeometryTriangleDecodeScratch();
	bool prepare(unsigned count, unsigned indexWidthBytes,
		unsigned maximumBytes = GEOMETRY_TRIANGLE_DEFAULT_SCRATCH_BYTES);
	unsigned char *records();
	const void *indices() const;
	const float *planes() const;
	const unsigned char *surfaces() const;
	// The owner preserves its existing debug assertion before publication;
	// release output retains the original unsigned-byte attribute narrowing.
	bool attributesInRange() const;
	unsigned count() const;
	unsigned inputBytes() const;
	unsigned indexWidthBytes() const;
	unsigned capacityBytes() const;

private:
	GeometryTriangleDecodeScratch(const GeometryTriangleDecodeScratch &);
	GeometryTriangleDecodeScratch &operator=(const GeometryTriangleDecodeScratch &);
	void *m_storage;
	unsigned m_count, m_inputBytes, m_capacityBytes;
	unsigned m_indexWidthBytes, m_planeOffset, m_surfaceOffset;
	bool m_prepared, m_ready, m_attributesInRange;
	friend GeometryTriangleDecodeResult DecodeGeometryTriangles(
		GeometryTriangleDecodeScratch &, const GeometryTriangleDecodeOptions &,
		GeometryTriangleDecodeMetrics *);
};

// Serial fallback is a completed conversion of the SAME captured bytes after
// every accepted job drained. No caller rewind or second source read is needed.
bool GeometryTriangleDecodeCompleted(GeometryTriangleDecodeResult result);
}
