/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#if defined(_MSC_VER) && _MSC_VER < 1600
namespace rts
{
typedef unsigned __int32 ImmutableSpatialUInt32;
typedef signed __int32 ImmutableSpatialInt32;
}
#else
#include <stdint.h>
namespace rts
{
typedef uint32_t ImmutableSpatialUInt32;
typedef int32_t ImmutableSpatialInt32;
}
#endif

namespace rts
{
enum
{
	IMMUTABLE_SPATIAL_ARENA_MAGIC = 0x51505349u, // "ISPQ"
	IMMUTABLE_SPATIAL_ARENA_VERSION = 1u,
	IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX = 0xffffffffu
};

// Every arena and object record carries the independently advancing title
// generations needed by an owner to reject stale worker observations.
struct ImmutableSpatialGeneration
{
	ImmutableSpatialUInt32 lifecycle;
	ImmutableSpatialUInt32 topology;
	ImmutableSpatialUInt32 facts;
};

// The blob contains only fixed-width values and arena-relative byte offsets.
// A valid arena is canonical and contiguous in this order: header, objects,
// cells, members, radius buckets, offsets. It can be copied to any aligned
// address without rewriting its contents.
struct ImmutableSpatialArenaHeader
{
	ImmutableSpatialUInt32 magic;
	ImmutableSpatialUInt32 version;
	ImmutableSpatialUInt32 headerBytes;
	ImmutableSpatialUInt32 arenaBytes;
	float cellSize;
	ImmutableSpatialUInt32 validationToken;
	ImmutableSpatialGeneration generation;
	ImmutableSpatialUInt32 gridWidth;
	ImmutableSpatialUInt32 gridHeight;
	ImmutableSpatialUInt32 objectCount;
	ImmutableSpatialUInt32 objectOffset;
	ImmutableSpatialUInt32 cellCount;
	ImmutableSpatialUInt32 cellOffset;
	ImmutableSpatialUInt32 memberCount;
	ImmutableSpatialUInt32 memberOffset;
	ImmutableSpatialUInt32 radiusCount;
	ImmutableSpatialUInt32 radiusOffset;
	ImmutableSpatialUInt32 offsetCount;
	ImmutableSpatialUInt32 offsetOffset;
};

// Geometry is the copied legacy distance-query representation. zCenterOffset
// is used only for bounding-sphere 3D distance. admissionMask and buildCost
// are query facts copied by the title adapter before worker execution.
struct ImmutableSpatialObjectRecord
{
	ImmutableSpatialUInt32 objectID;
	ImmutableSpatialGeneration generation;
	ImmutableSpatialUInt32 admissionMask;
	ImmutableSpatialInt32 buildCost;
	float positionX;
	float positionY;
	float positionZ;
	float boundingCircleRadius;
	float boundingSphereRadius;
	float zCenterOffset;
};

struct ImmutableSpatialCellRecord
{
	ImmutableSpatialUInt32 memberBegin;
	ImmutableSpatialUInt32 memberCount;
};

// INVALID_OBJECT_INDEX represents a copied null COI/module/object and is
// skipped without consuming a discovery ordinal.
struct ImmutableSpatialMemberRecord
{
	ImmutableSpatialUInt32 objectIndex;
};

struct ImmutableSpatialRadiusRecord
{
	ImmutableSpatialUInt32 offsetBegin;
	ImmutableSpatialUInt32 offsetCount;
};

struct ImmutableSpatialOffsetRecord
{
	ImmutableSpatialInt32 x;
	ImmutableSpatialInt32 y;
};

// This descriptor is transient owner input and is not stored in the blob.
// Objects must be sorted by strictly increasing objectID. Cell/member spans
// must be dense and preserve linked COI order. Radius/offset spans must contain
// the complete (2*width-1)*(2*height-1) legacy calcRadiusVec topology: every
// offset exactly once, assigned to its calcMinRadius bucket, and retaining the
// legacy y-major/x-minor insertion order inside each bucket. cellSize is copied
// so validation uses the same floating-point radius assignment as the title.
struct ImmutableSpatialArenaInput
{
	ImmutableSpatialGeneration generation;
	ImmutableSpatialUInt32 gridWidth;
	ImmutableSpatialUInt32 gridHeight;
	float cellSize;
	const ImmutableSpatialObjectRecord *objects;
	ImmutableSpatialUInt32 objectCount;
	const ImmutableSpatialCellRecord *cells;
	ImmutableSpatialUInt32 cellCount;
	const ImmutableSpatialMemberRecord *members;
	ImmutableSpatialUInt32 memberCount;
	const ImmutableSpatialRadiusRecord *radii;
	ImmutableSpatialUInt32 radiusCount;
	const ImmutableSpatialOffsetRecord *offsets;
	ImmutableSpatialUInt32 offsetCount;
};

enum ImmutableSpatialDistanceType
{
	IMMUTABLE_SPATIAL_FROM_CENTER_2D = 0,
	IMMUTABLE_SPATIAL_FROM_CENTER_3D,
	IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_2D,
	IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_3D
};

enum ImmutableSpatialIteratorOrder
{
	IMMUTABLE_SPATIAL_ITER_FASTEST = 0,
	IMMUTABLE_SPATIAL_ITER_NEAR_TO_FAR,
	IMMUTABLE_SPATIAL_ITER_FAR_TO_NEAR,
	IMMUTABLE_SPATIAL_ITER_CHEAP_TO_EXPENSIVE,
	IMMUTABLE_SPATIAL_ITER_EXPENSIVE_TO_CHEAP
};

struct ImmutableSpatialQuery
{
	ImmutableSpatialGeneration expectedArenaGeneration;
	ImmutableSpatialUInt32 selfObjectIndex;
	ImmutableSpatialInt32 centerCellX;
	ImmutableSpatialInt32 centerCellY;
	ImmutableSpatialUInt32 maximumRadius;
	float positionX;
	float positionY;
	float positionZ;
	float boundingCircleRadius;
	float boundingSphereRadius;
	float zCenterOffset;
	float maximumDistance;
	ImmutableSpatialUInt32 requiredAdmissionMask;
	ImmutableSpatialUInt32 rejectedAdmissionMask;
	ImmutableSpatialUInt32 distanceType;
	ImmutableSpatialUInt32 iteratorOrder;
};

// discoveryOrdinal is the first unique non-null, non-self observation in the
// legacy traversal. Results are ordered exactly like SimpleObjectIterator:
// ITER_FASTEST reverses discovery, and every sorted order keeps that reverse
// discovery order for ties.
struct ImmutableSpatialResult
{
	ImmutableSpatialUInt32 objectIndex;
	ImmutableSpatialUInt32 objectID;
	ImmutableSpatialGeneration generation;
	ImmutableSpatialUInt32 discoveryOrdinal;
	ImmutableSpatialInt32 buildCost;
	float distanceSquared;
};

struct ImmutableSpatialResultSpan
{
	ImmutableSpatialUInt32 begin;
	ImmutableSpatialUInt32 count;
};

typedef bool (*ImmutableSpatialRangeFunction)(void *context,
	ImmutableSpatialUInt32 rangeIndex);

// The dispatcher must invoke every range index [0, rangeCount) exactly once,
// possibly concurrently, and must not return until all invoked ranges finish.
// Returning false is a transactional batch failure.
typedef bool (*ImmutableSpatialDispatchFunction)(void *context,
	ImmutableSpatialUInt32 rangeCount,
	ImmutableSpatialRangeFunction rangeFunction,
	void *rangeContext);

typedef bool (*ImmutableSpatialCancellationFunction)(void *context);
typedef bool (*ImmutableSpatialArenaGenerationResolver)(
	const ImmutableSpatialGeneration *expected, void *context);
typedef bool (*ImmutableSpatialObjectGenerationResolver)(
	ImmutableSpatialUInt32 objectID,
	const ImmutableSpatialGeneration *expected, void *context);

struct ImmutableSpatialExecutionOptions
{
	ImmutableSpatialExecutionOptions();

	ImmutableSpatialUInt32 workerCount;
	ImmutableSpatialDispatchFunction dispatch;
	void *dispatchContext;
	ImmutableSpatialCancellationFunction isCancelled;
	void *cancellationContext;
	ImmutableSpatialArenaGenerationResolver resolveArenaGeneration;
	ImmutableSpatialObjectGenerationResolver resolveObjectGeneration;
	void *generationContext;
};

// All storage is caller-owned. visitStamps needs rangeCount*objectCount slots,
// where rangeCount=min(workerCount, queryCount). resultScratch and sortScratch
// each need the final result count. The scratch and publication regions must
// be disjoint. Scratch may change on failure; published spans, results, and
// outputCount never do.
struct ImmutableSpatialBatchScratch
{
	ImmutableSpatialUInt32 *counts;
	ImmutableSpatialUInt32 countCapacity;
	ImmutableSpatialUInt32 *states;
	ImmutableSpatialUInt32 stateCapacity;
	ImmutableSpatialUInt32 *visitStamps;
	ImmutableSpatialUInt32 visitStampCapacity;
	ImmutableSpatialResultSpan *spanScratch;
	ImmutableSpatialUInt32 spanScratchCapacity;
	ImmutableSpatialResult *resultScratch;
	ImmutableSpatialUInt32 resultScratchCapacity;
	ImmutableSpatialResult *sortScratch;
	ImmutableSpatialUInt32 sortScratchCapacity;
};

struct ImmutableSpatialExecutionMetrics
{
	ImmutableSpatialExecutionMetrics();

	ImmutableSpatialUInt32 rangeCount;
	ImmutableSpatialUInt32 countPassQueries;
	ImmutableSpatialUInt32 fillPassQueries;
	ImmutableSpatialUInt32 resultCount;
};

enum ImmutableSpatialStatus
{
	IMMUTABLE_SPATIAL_SUCCESS = 0,
	IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
	IMMUTABLE_SPATIAL_MALFORMED_ARENA,
	IMMUTABLE_SPATIAL_STALE_GENERATION,
	IMMUTABLE_SPATIAL_CANCELLED,
	IMMUTABLE_SPATIAL_DISPATCH_FAILURE,
	IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY,
	IMMUTABLE_SPATIAL_OVERFLOW,
	IMMUTABLE_SPATIAL_GENERATION_MISMATCH
};

// Both functions perform the complete linear canonical-content validation
// before writing output. On any failure requiredBytes/arenaBytes and
// destination storage remain untouched. Build input and destination regions
// must be disjoint.
ImmutableSpatialStatus MeasureImmutableSpatialArena(
	const ImmutableSpatialArenaInput &input,
	ImmutableSpatialUInt32 *requiredBytes);
ImmutableSpatialStatus BuildImmutableSpatialArena(
	const ImmutableSpatialArenaInput &input,
	void *destination,
	ImmutableSpatialUInt32 destinationCapacity,
	ImmutableSpatialUInt32 *arenaBytes);

// Performs complete linear validation. Query execution subsequently uses the
// immutable build token plus constant-time structural validation; it never
// repeats whole-arena object/offset uniqueness work per batch.
bool ValidateImmutableSpatialArena(const void *arena,
	ImmutableSpatialUInt32 arenaCapacity);

bool ValidateImmutableSpatialResultSpans(
	const ImmutableSpatialResultSpan *spans,
	ImmutableSpatialUInt32 spanCount,
	ImmutableSpatialUInt32 resultCount);

// Checks each published result against the copied record and, when supplied,
// against the title owner's current object generation.
bool ValidateImmutableSpatialResultGenerations(
	const void *arena,
	ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialResult *results,
	ImmutableSpatialUInt32 resultCount,
	ImmutableSpatialObjectGenerationResolver resolver,
	void *resolverContext,
	ImmutableSpatialUInt32 *firstStaleResult = 0);

// The count and fill passes may execute on injected workers, but validation,
// generation re-resolution, span validation, and publication are owner-side.
// A title adapter must supply both generation resolvers before treating a
// successful batch as authoritative against mutable live state.
ImmutableSpatialStatus ExecuteImmutableSpatialQueryBatch(
	const void *arena,
	ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialQuery *queries,
	ImmutableSpatialUInt32 queryCount,
	const ImmutableSpatialExecutionOptions &options,
	const ImmutableSpatialBatchScratch &scratch,
	ImmutableSpatialResult *output,
	ImmutableSpatialUInt32 outputCapacity,
	ImmutableSpatialResultSpan *outputSpans,
	ImmutableSpatialUInt32 outputSpanCapacity,
	ImmutableSpatialUInt32 *outputCount,
	ImmutableSpatialExecutionMetrics *metrics = 0);
}
