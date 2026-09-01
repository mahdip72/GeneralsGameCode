/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ImmutableSpatialQuery.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

namespace rts
{
ImmutableSpatialExecutionOptions::ImmutableSpatialExecutionOptions()
	: workerCount(1), dispatch(0), dispatchContext(0), isCancelled(0),
	  cancellationContext(0), resolveArenaGeneration(0),
	  resolveObjectGeneration(0), generationContext(0)
{
}

ImmutableSpatialExecutionMetrics::ImmutableSpatialExecutionMetrics()
	: rangeCount(0), countPassQueries(0), fillPassQueries(0), resultCount(0)
{
}

namespace
{
#if defined(_MSC_VER)
typedef unsigned __int64 SpatialUInt64;
typedef signed __int64 SpatialInt64;
#else
typedef unsigned long long SpatialUInt64;
typedef long long SpatialInt64;
#endif

typedef char SpatialUInt32MustBeFourBytes[
	(sizeof(ImmutableSpatialUInt32) == 4) ? 1 : -1];
typedef char SpatialInt32MustBeFourBytes[
	(sizeof(ImmutableSpatialInt32) == 4) ? 1 : -1];
typedef char SpatialFloatMustBeFourBytes[(sizeof(float) == 4) ? 1 : -1];
typedef char SpatialGenerationMustBeTwelveBytes[
	(sizeof(ImmutableSpatialGeneration) == 12) ? 1 : -1];
typedef char SpatialHeaderMustBeEightyFourBytes[
	(sizeof(ImmutableSpatialArenaHeader) == 84) ? 1 : -1];
typedef char SpatialObjectMustBeFortyEightBytes[
	(sizeof(ImmutableSpatialObjectRecord) == 48) ? 1 : -1];
typedef char SpatialCellMustBeEightBytes[
	(sizeof(ImmutableSpatialCellRecord) == 8) ? 1 : -1];
typedef char SpatialMemberMustBeFourBytes[
	(sizeof(ImmutableSpatialMemberRecord) == 4) ? 1 : -1];
typedef char SpatialRadiusMustBeEightBytes[
	(sizeof(ImmutableSpatialRadiusRecord) == 8) ? 1 : -1];
typedef char SpatialOffsetMustBeEightBytes[
	(sizeof(ImmutableSpatialOffsetRecord) == 8) ? 1 : -1];
typedef char SpatialResultMustBeThirtyTwoBytes[
	(sizeof(ImmutableSpatialResult) == 32) ? 1 : -1];
typedef char SpatialSpanMustBeEightBytes[
	(sizeof(ImmutableSpatialResultSpan) == 8) ? 1 : -1];

bool generationEqual(const ImmutableSpatialGeneration &left,
	const ImmutableSpatialGeneration &right)
{
	return left.lifecycle == right.lifecycle &&
		left.topology == right.topology && left.facts == right.facts;
}

bool finiteFloat(float value)
{
	return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

bool alignedArena(const void *arena)
{
	return arena != 0 &&
		(reinterpret_cast<size_t>(arena) & (sizeof(ImmutableSpatialUInt32) - 1)) == 0;
}

bool addBytes(ImmutableSpatialUInt32 count, size_t elementBytes,
	ImmutableSpatialUInt32 &running)
{
	const SpatialUInt64 added = static_cast<SpatialUInt64>(count) *
		static_cast<SpatialUInt64>(elementBytes);
	const SpatialUInt64 total = static_cast<SpatialUInt64>(running) + added;
	if (total > 0xffffffffu)
		return false;
	running = static_cast<ImmutableSpatialUInt32>(total);
	return true;
}

bool checkedByteCount(SpatialUInt64 count, size_t elementBytes,
	size_t &bytes)
{
	const SpatialUInt64 elementBytes64 =
		static_cast<SpatialUInt64>(elementBytes);
	if (elementBytes64 != 0 && count >
		(~static_cast<SpatialUInt64>(0)) / elementBytes64)
		return false;
	const SpatialUInt64 total = count * elementBytes64;
	if (total > static_cast<SpatialUInt64>(static_cast<size_t>(-1)))
		return false;
	bytes = static_cast<size_t>(total);
	return true;
}

bool checkedMultiply(SpatialUInt64 left, SpatialUInt64 right,
	SpatialUInt64 &product)
{
	if (left != 0 && right >
		(~static_cast<SpatialUInt64>(0)) / left)
		return false;
	product = left * right;
	return true;
}

struct MemoryRegion
{
	const void *address;
	size_t bytes;
};

bool regionIsRepresentable(const MemoryRegion &region)
{
	if (region.bytes == 0)
		return true;
	if (region.address == 0)
		return false;
	const size_t begin = reinterpret_cast<size_t>(region.address);
	return region.bytes <= static_cast<size_t>(-1) - begin;
}

bool regionsOverlap(const MemoryRegion &left, const MemoryRegion &right)
{
	if (left.bytes == 0 || right.bytes == 0)
		return false;
	const size_t leftBegin = reinterpret_cast<size_t>(left.address);
	const size_t rightBegin = reinterpret_cast<size_t>(right.address);
	return leftBegin < rightBegin + right.bytes &&
		rightBegin < leftBegin + left.bytes;
}

bool regionsAreDisjoint(const MemoryRegion *regions, unsigned regionCount)
{
	unsigned first;
	for (first = 0; first < regionCount; ++first)
	{
		if (!regionIsRepresentable(regions[first]))
			return false;
		unsigned second;
		for (second = 0; second < first; ++second)
		{
			if (regionsOverlap(regions[first], regions[second]))
				return false;
		}
	}
	return true;
}

ImmutableSpatialUInt32 ceilPositiveFloat(float value)
{
	return static_cast<ImmutableSpatialUInt32>(ceilf(value));
}

bool legacyMaximumRadius(ImmutableSpatialUInt32 width,
	ImmutableSpatialUInt32 height, float cellSize,
	ImmutableSpatialUInt32 &maximumRadius)
{
	const double dx = static_cast<double>(width) * cellSize;
	const double dy = static_cast<double>(height) * cellSize;
	const double maximumDistance = sqrt(dx * dx + dy * dy);
	const float cellRadius = static_cast<float>(maximumDistance / cellSize);
	if (!finiteFloat(cellRadius) || cellRadius < 0.0f ||
		cellRadius >= 4294967295.0f)
		return false;
	maximumRadius = ceilPositiveFloat(cellRadius);
	return true;
}

bool legacyRadiusForOffset(ImmutableSpatialInt32 offsetX,
	ImmutableSpatialInt32 offsetY, float cellSize,
	ImmutableSpatialUInt32 &radius)
{
	const float halfCell = cellSize * 0.5f;
	const float centerX[4] = { -halfCell, halfCell, -halfCell, halfCell };
	const float centerY[4] = { -halfCell, -halfCell, halfCell, halfCell };
	const float x = static_cast<float>(offsetX) * cellSize;
	const float y = static_cast<float>(offsetY) * cellSize;
	if (!finiteFloat(halfCell) || !finiteFloat(x) || !finiteFloat(y))
		return false;
	const float otherX[4] = {
		x - halfCell, x + halfCell, x - halfCell, x + halfCell
	};
	const float otherY[4] = {
		y - halfCell, y - halfCell, y + halfCell, y + halfCell
	};
	double minimumDistanceSquared = 1e12;
	unsigned first;
	for (first = 0; first < 4; ++first)
	{
		unsigned second;
		for (second = 0; second < 4; ++second)
		{
			const double differenceX = centerX[first] - otherX[second];
			const double differenceY = centerY[first] - otherY[second];
			const double distanceSquared = differenceX * differenceX +
				differenceY * differenceY;
			if (minimumDistanceSquared > distanceSquared)
				minimumDistanceSquared = distanceSquared;
		}
	}
	const double distance = sqrtf(static_cast<float>(minimumDistanceSquared));
	const float cellRadius = static_cast<float>(distance / cellSize);
	if (!finiteFloat(cellRadius) || cellRadius < 0.0f ||
		cellRadius >= 4294967295.0f)
		return false;
	radius = ceilPositiveFloat(cellRadius);
	return true;
}

ImmutableSpatialStatus validateArenaInput(
	const ImmutableSpatialArenaInput &input,
	ImmutableSpatialUInt32 &requiredBytes)
{
	if (input.gridWidth == 0 || input.gridHeight == 0 ||
		input.radiusCount == 0 || !finiteFloat(input.cellSize) ||
		input.cellSize <= 0.0f)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	const SpatialUInt64 cellProduct =
		static_cast<SpatialUInt64>(input.gridWidth) * input.gridHeight;
	if (cellProduct > 0xffffffffu)
		return IMMUTABLE_SPATIAL_OVERFLOW;
	if (input.cellCount != static_cast<ImmutableSpatialUInt32>(cellProduct))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	const SpatialUInt64 offsetWidth =
		static_cast<SpatialUInt64>(input.gridWidth) * 2 - 1;
	const SpatialUInt64 offsetHeight =
		static_cast<SpatialUInt64>(input.gridHeight) * 2 - 1;
	SpatialUInt64 offsetProduct = 0;
	if (!checkedMultiply(offsetWidth, offsetHeight, offsetProduct))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	if (offsetProduct > 0xffffffffu)
		return IMMUTABLE_SPATIAL_OVERFLOW;
	if (input.offsetCount !=
		static_cast<ImmutableSpatialUInt32>(offsetProduct))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	ImmutableSpatialUInt32 maximumRadius = 0;
	if (!legacyMaximumRadius(input.gridWidth, input.gridHeight,
		input.cellSize, maximumRadius) || maximumRadius == 0xffffffffu)
		return IMMUTABLE_SPATIAL_OVERFLOW;
	if (input.radiusCount != maximumRadius + 1)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	if ((input.objectCount != 0 && input.objects == 0) ||
		(input.cellCount != 0 && input.cells == 0) ||
		(input.memberCount != 0 && input.members == 0) ||
		input.radii == 0 ||
		(input.offsetCount != 0 && input.offsets == 0))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	ImmutableSpatialUInt32 index;
	ImmutableSpatialUInt32 previousObjectID = 0;
	for (index = 0; index < input.objectCount; ++index)
	{
		const ImmutableSpatialObjectRecord &object = input.objects[index];
		if (object.objectID == 0 || object.objectID <= previousObjectID ||
			!finiteFloat(object.positionX) ||
			!finiteFloat(object.positionY) || !finiteFloat(object.positionZ) ||
			!finiteFloat(object.boundingCircleRadius) ||
			!finiteFloat(object.boundingSphereRadius) ||
			!finiteFloat(object.zCenterOffset) ||
			object.boundingCircleRadius < 0.0f ||
			object.boundingSphereRadius < 0.0f)
			return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
		previousObjectID = object.objectID;
	}

	ImmutableSpatialUInt32 expectedBegin = 0;
	for (index = 0; index < input.cellCount; ++index)
	{
		const ImmutableSpatialCellRecord &cell = input.cells[index];
		if (cell.memberBegin != expectedBegin ||
			cell.memberCount > input.memberCount - expectedBegin)
			return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
		expectedBegin += cell.memberCount;
	}
	if (expectedBegin != input.memberCount)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	for (index = 0; index < input.memberCount; ++index)
	{
		const ImmutableSpatialUInt32 objectIndex =
			input.members[index].objectIndex;
		if (objectIndex != IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX &&
			objectIndex >= input.objectCount)
			return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	}

	expectedBegin = 0;
	for (index = 0; index < input.radiusCount; ++index)
	{
		const ImmutableSpatialRadiusRecord &radius = input.radii[index];
		if (radius.offsetBegin != expectedBegin ||
			radius.offsetCount > input.offsetCount - expectedBegin)
			return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
		const ImmutableSpatialUInt32 offsetEnd = expectedBegin +
			radius.offsetCount;
		SpatialUInt64 previousLinear = 0;
		bool hasPreviousLinear = false;
		ImmutableSpatialUInt32 offsetIndex;
		for (offsetIndex = expectedBegin; offsetIndex < offsetEnd;
			++offsetIndex)
		{
			const ImmutableSpatialOffsetRecord &offset = input.offsets[offsetIndex];
			if (static_cast<SpatialInt64>(offset.x) <=
				-static_cast<SpatialInt64>(input.gridWidth) ||
				static_cast<SpatialInt64>(offset.x) >=
				static_cast<SpatialInt64>(input.gridWidth) ||
				static_cast<SpatialInt64>(offset.y) <=
				-static_cast<SpatialInt64>(input.gridHeight) ||
				static_cast<SpatialInt64>(offset.y) >=
				static_cast<SpatialInt64>(input.gridHeight))
				return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
			ImmutableSpatialUInt32 assignedRadius = 0;
			if (!legacyRadiusForOffset(offset.x, offset.y, input.cellSize,
				assignedRadius) || assignedRadius != index)
				return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
			const SpatialUInt64 linear =
				(static_cast<SpatialInt64>(offset.y) + input.gridHeight - 1) *
				offsetWidth +
				(static_cast<SpatialInt64>(offset.x) + input.gridWidth - 1);
			if (hasPreviousLinear && linear <= previousLinear)
				return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
			previousLinear = linear;
			hasPreviousLinear = true;
		}
		expectedBegin = offsetEnd;
	}
	if (expectedBegin != input.offsetCount)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	ImmutableSpatialUInt32 bytes =
		static_cast<ImmutableSpatialUInt32>(sizeof(ImmutableSpatialArenaHeader));
	if (!addBytes(input.objectCount, sizeof(ImmutableSpatialObjectRecord), bytes) ||
		!addBytes(input.cellCount, sizeof(ImmutableSpatialCellRecord), bytes) ||
		!addBytes(input.memberCount, sizeof(ImmutableSpatialMemberRecord), bytes) ||
		!addBytes(input.radiusCount, sizeof(ImmutableSpatialRadiusRecord), bytes) ||
		!addBytes(input.offsetCount, sizeof(ImmutableSpatialOffsetRecord), bytes))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	requiredBytes = bytes;
	return IMMUTABLE_SPATIAL_SUCCESS;
}

struct ArenaView
{
	const ImmutableSpatialArenaHeader *header;
	const ImmutableSpatialObjectRecord *objects;
	const ImmutableSpatialCellRecord *cells;
	const ImmutableSpatialMemberRecord *members;
	const ImmutableSpatialRadiusRecord *radii;
	const ImmutableSpatialOffsetRecord *offsets;
};

ImmutableSpatialUInt32 mixToken(ImmutableSpatialUInt32 hash,
	ImmutableSpatialUInt32 value)
{
	hash ^= value;
	hash *= 16777619u;
	return hash;
}

ImmutableSpatialUInt32 arenaValidationToken(
	const ImmutableSpatialArenaHeader &header)
{
	ImmutableSpatialUInt32 cellSizeBits = 0;
	memcpy(&cellSizeBits, &header.cellSize, sizeof(cellSizeBits));
	ImmutableSpatialUInt32 hash = 2166136261u;
	hash = mixToken(hash, header.magic);
	hash = mixToken(hash, header.version);
	hash = mixToken(hash, header.headerBytes);
	hash = mixToken(hash, header.arenaBytes);
	hash = mixToken(hash, cellSizeBits);
	hash = mixToken(hash, header.generation.lifecycle);
	hash = mixToken(hash, header.generation.topology);
	hash = mixToken(hash, header.generation.facts);
	hash = mixToken(hash, header.gridWidth);
	hash = mixToken(hash, header.gridHeight);
	hash = mixToken(hash, header.objectCount);
	hash = mixToken(hash, header.objectOffset);
	hash = mixToken(hash, header.cellCount);
	hash = mixToken(hash, header.cellOffset);
	hash = mixToken(hash, header.memberCount);
	hash = mixToken(hash, header.memberOffset);
	hash = mixToken(hash, header.radiusCount);
	hash = mixToken(hash, header.radiusOffset);
	hash = mixToken(hash, header.offsetCount);
	hash = mixToken(hash, header.offsetOffset);
	return hash ^ 0x9e3779b9u;
}

bool validateArenaHeader(const void *arena,
	ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialArenaHeader *&header)
{
	if (!alignedArena(arena) ||
		arenaCapacity < sizeof(ImmutableSpatialArenaHeader))
		return false;
	header = static_cast<const ImmutableSpatialArenaHeader *>(arena);
	if (header->magic != IMMUTABLE_SPATIAL_ARENA_MAGIC ||
		header->version != IMMUTABLE_SPATIAL_ARENA_VERSION ||
		header->headerBytes != sizeof(ImmutableSpatialArenaHeader) ||
		header->arenaBytes > arenaCapacity ||
		header->arenaBytes < sizeof(ImmutableSpatialArenaHeader) ||
		!finiteFloat(header->cellSize) || header->cellSize <= 0.0f)
		return false;

	const SpatialUInt64 cellProduct =
		static_cast<SpatialUInt64>(header->gridWidth) * header->gridHeight;
	const SpatialUInt64 offsetWidth =
		static_cast<SpatialUInt64>(header->gridWidth) * 2 - 1;
	const SpatialUInt64 offsetHeight =
		static_cast<SpatialUInt64>(header->gridHeight) * 2 - 1;
	SpatialUInt64 offsetProduct = 0;
	if (!checkedMultiply(offsetWidth, offsetHeight, offsetProduct) ||
		header->gridWidth == 0 || header->gridHeight == 0 ||
		cellProduct > 0xffffffffu || header->cellCount != cellProduct ||
		offsetWidth > 0xffffffffu || offsetHeight > 0xffffffffu ||
		offsetProduct > 0xffffffffu ||
		header->offsetCount != offsetProduct)
		return false;
	ImmutableSpatialUInt32 maximumRadius = 0;
	if (!legacyMaximumRadius(header->gridWidth, header->gridHeight,
		header->cellSize, maximumRadius) || maximumRadius == 0xffffffffu ||
		header->radiusCount != maximumRadius + 1)
		return false;

	ImmutableSpatialUInt32 cursor = sizeof(ImmutableSpatialArenaHeader);
	if (header->objectOffset != cursor ||
		!addBytes(header->objectCount, sizeof(ImmutableSpatialObjectRecord), cursor) ||
		header->cellOffset != cursor ||
		!addBytes(header->cellCount, sizeof(ImmutableSpatialCellRecord), cursor) ||
		header->memberOffset != cursor ||
		!addBytes(header->memberCount, sizeof(ImmutableSpatialMemberRecord), cursor) ||
		header->radiusOffset != cursor ||
		!addBytes(header->radiusCount, sizeof(ImmutableSpatialRadiusRecord), cursor) ||
		header->offsetOffset != cursor ||
		!addBytes(header->offsetCount, sizeof(ImmutableSpatialOffsetRecord), cursor) ||
		cursor != header->arenaBytes ||
		header->validationToken != arenaValidationToken(*header))
		return false;
	return true;
}

bool makeArenaView(const void *arena, ImmutableSpatialUInt32 arenaCapacity,
	ArenaView &view)
{
	const ImmutableSpatialArenaHeader *header = 0;
	if (!validateArenaHeader(arena, arenaCapacity, header))
		return false;
	const unsigned char *base = static_cast<const unsigned char *>(arena);
	view.header = header;
	view.objects = reinterpret_cast<const ImmutableSpatialObjectRecord *>(
		base + view.header->objectOffset);
	view.cells = reinterpret_cast<const ImmutableSpatialCellRecord *>(
		base + view.header->cellOffset);
	view.members = reinterpret_cast<const ImmutableSpatialMemberRecord *>(
		base + view.header->memberOffset);
	view.radii = reinterpret_cast<const ImmutableSpatialRadiusRecord *>(
		base + view.header->radiusOffset);
	view.offsets = reinterpret_cast<const ImmutableSpatialOffsetRecord *>(
		base + view.header->offsetOffset);
	return true;
}

bool cancellationRequested(const ImmutableSpatialExecutionOptions &options)
{
	return options.isCancelled != 0 &&
		options.isCancelled(options.cancellationContext);
}

bool queryIsValid(const ArenaView &view, const ImmutableSpatialQuery &query)
{
	if (!generationEqual(view.header->generation,
		query.expectedArenaGeneration))
		return false;
	if (query.selfObjectIndex != IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX &&
		query.selfObjectIndex >= view.header->objectCount)
		return false;
	if (query.maximumRadius >= view.header->radiusCount ||
		query.distanceType > IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_3D ||
		query.iteratorOrder > IMMUTABLE_SPATIAL_ITER_EXPENSIVE_TO_CHEAP ||
		!finiteFloat(query.positionX) || !finiteFloat(query.positionY) ||
		!finiteFloat(query.positionZ) ||
		!finiteFloat(query.boundingCircleRadius) ||
		!finiteFloat(query.boundingSphereRadius) ||
		!finiteFloat(query.zCenterOffset) ||
		!finiteFloat(query.maximumDistance) ||
		query.boundingCircleRadius < 0.0f ||
		query.boundingSphereRadius < 0.0f || query.maximumDistance < 0.0f)
		return false;
	const float maximumDistanceSquared =
		query.maximumDistance * query.maximumDistance;
	return finiteFloat(maximumDistanceSquared);
}

bool admitted(const ImmutableSpatialObjectRecord &object,
	const ImmutableSpatialQuery &query)
{
	return (object.admissionMask & query.requiredAdmissionMask) ==
		query.requiredAdmissionMask &&
		(object.admissionMask & query.rejectedAdmissionMask) == 0;
}

bool calculateDistanceSquared(const ImmutableSpatialQuery &query,
	const ImmutableSpatialObjectRecord &object, float &distanceSquared)
{
	float x = object.positionX - query.positionX;
	float y = object.positionY - query.positionY;
	float z = 0.0f;
	float totalRadius = 0.0f;
	if (query.distanceType == IMMUTABLE_SPATIAL_FROM_CENTER_3D)
		z = object.positionZ - query.positionZ;
	else if (query.distanceType ==
		IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_2D)
		totalRadius = object.boundingCircleRadius +
			query.boundingCircleRadius;
	else if (query.distanceType ==
		IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_3D)
	{
		z = (object.positionZ + object.zCenterOffset) -
			(query.positionZ + query.zCenterOffset);
		totalRadius = object.boundingSphereRadius +
			query.boundingSphereRadius;
	}

	const float actualDistanceSquared = x * x + y * y + z * z;
	if (query.distanceType == IMMUTABLE_SPATIAL_FROM_CENTER_2D ||
		query.distanceType == IMMUTABLE_SPATIAL_FROM_CENTER_3D ||
		totalRadius <= 0.0f)
	{
		distanceSquared = actualDistanceSquared;
	}
	else
	{
		const float actualDistance = sqrtf(actualDistanceSquared);
		const float shrunkenDistance = actualDistance - totalRadius;
		distanceSquared = shrunkenDistance <= 0.0f ? 0.0f :
			shrunkenDistance * shrunkenDistance;
	}
	const float maximumDistanceSquared =
		query.maximumDistance * query.maximumDistance;
	return distanceSquared < maximumDistanceSquared;
}

int compareResult(const ImmutableSpatialResult &left,
	const ImmutableSpatialResult &right,
	ImmutableSpatialUInt32 order)
{
	if (order == IMMUTABLE_SPATIAL_ITER_NEAR_TO_FAR)
	{
		if (left.distanceSquared < right.distanceSquared)
			return -1;
		if (left.distanceSquared > right.distanceSquared)
			return 1;
	}
	else if (order == IMMUTABLE_SPATIAL_ITER_FAR_TO_NEAR)
	{
		if (left.distanceSquared > right.distanceSquared)
			return -1;
		if (left.distanceSquared < right.distanceSquared)
			return 1;
	}
	else if (order == IMMUTABLE_SPATIAL_ITER_CHEAP_TO_EXPENSIVE)
	{
		if (left.buildCost < right.buildCost)
			return -1;
		if (left.buildCost > right.buildCost)
			return 1;
	}
	else if (order == IMMUTABLE_SPATIAL_ITER_EXPENSIVE_TO_CHEAP)
	{
		if (left.buildCost > right.buildCost)
			return -1;
		if (left.buildCost < right.buildCost)
			return 1;
	}
	if (left.discoveryOrdinal > right.discoveryOrdinal)
		return -1;
	if (left.discoveryOrdinal < right.discoveryOrdinal)
		return 1;
	return 0;
}

bool sortResults(ImmutableSpatialResult *results,
	ImmutableSpatialResult *temporary, ImmutableSpatialUInt32 count,
	ImmutableSpatialUInt32 order)
{
	if (count < 2)
		return true;
	ImmutableSpatialResult *source = results;
	ImmutableSpatialResult *destination = temporary;
	SpatialUInt64 width;
	for (width = 1; width < count; width *= 2)
	{
		SpatialUInt64 begin;
		for (begin = 0; begin < count; begin += width * 2)
		{
			SpatialUInt64 left = begin;
			SpatialUInt64 middle = begin + width;
			SpatialUInt64 right = begin + width * 2;
			if (middle > count)
				middle = count;
			if (right > count)
				right = count;
			SpatialUInt64 first = left;
			SpatialUInt64 second = middle;
			SpatialUInt64 output = left;
			while (first < middle || second < right)
			{
				if (second == right ||
					(first < middle && compareResult(source[first],
						source[second], order) <= 0))
					destination[output++] = source[first++];
				else
					destination[output++] = source[second++];
			}
		}
		ImmutableSpatialResult *swap = source;
		source = destination;
		destination = swap;
	}
	if (source != results)
	{
		size_t resultBytes = 0;
		if (!checkedByteCount(count, sizeof(ImmutableSpatialResult), resultBytes))
			return false;
		memcpy(results, source, resultBytes);
	}
	return true;
}

enum QueryPhase
{
	QUERY_PHASE_COUNT,
	QUERY_PHASE_FILL
};

struct QueryBatchContext
{
	const ArenaView *view;
	const ImmutableSpatialQuery *queries;
	ImmutableSpatialUInt32 queryCount;
	ImmutableSpatialUInt32 rangeCount;
	const ImmutableSpatialExecutionOptions *options;
	const ImmutableSpatialBatchScratch *scratch;
	QueryPhase phase;
};

ImmutableSpatialStatus executeOneQuery(QueryBatchContext &context,
	ImmutableSpatialUInt32 queryIndex,
	ImmutableSpatialUInt32 rangeIndex)
{
	if (cancellationRequested(*context.options))
		return IMMUTABLE_SPATIAL_CANCELLED;
	const ArenaView &view = *context.view;
	const ImmutableSpatialQuery &query = context.queries[queryIndex];
	ImmutableSpatialUInt32 *visited = context.scratch->visitStamps;
	if (view.header->objectCount != 0)
		visited += static_cast<size_t>(rangeIndex) *
			view.header->objectCount;
	const ImmutableSpatialUInt32 stamp = queryIndex + 1;
	ImmutableSpatialUInt32 discoveryOrdinal = 0;
	ImmutableSpatialUInt32 resultCount = 0;
	ImmutableSpatialUInt32 radiusIndex;
	for (radiusIndex = 0; radiusIndex <= query.maximumRadius; ++radiusIndex)
	{
		if (cancellationRequested(*context.options))
			return IMMUTABLE_SPATIAL_CANCELLED;
		const ImmutableSpatialRadiusRecord &radius = view.radii[radiusIndex];
		if (radius.offsetBegin > view.header->offsetCount ||
			radius.offsetCount >
				view.header->offsetCount - radius.offsetBegin)
			return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
		ImmutableSpatialUInt32 offsetEnd = radius.offsetBegin +
			radius.offsetCount;
		ImmutableSpatialUInt32 offsetIndex;
		for (offsetIndex = radius.offsetBegin; offsetIndex < offsetEnd;
			++offsetIndex)
		{
			const ImmutableSpatialOffsetRecord &offset = view.offsets[offsetIndex];
			const SpatialInt64 cellX = static_cast<SpatialInt64>(
				query.centerCellX) + offset.x;
			const SpatialInt64 cellY = static_cast<SpatialInt64>(
				query.centerCellY) + offset.y;
			if (cellX < 0 || cellY < 0 ||
				cellX >= view.header->gridWidth ||
				cellY >= view.header->gridHeight)
				continue;
			const SpatialUInt64 cellIndex64 =
				static_cast<SpatialUInt64>(cellY) * view.header->gridWidth +
				static_cast<SpatialUInt64>(cellX);
			const ImmutableSpatialCellRecord &cell =
				view.cells[static_cast<ImmutableSpatialUInt32>(cellIndex64)];
			if (cell.memberBegin > view.header->memberCount ||
				cell.memberCount >
					view.header->memberCount - cell.memberBegin)
				return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
			const ImmutableSpatialUInt32 memberEnd = cell.memberBegin +
				cell.memberCount;
			ImmutableSpatialUInt32 memberIndex;
			for (memberIndex = cell.memberBegin; memberIndex < memberEnd;
				++memberIndex)
			{
				const ImmutableSpatialUInt32 objectIndex =
					view.members[memberIndex].objectIndex;
				if (objectIndex == IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX ||
					objectIndex == query.selfObjectIndex)
					continue;
				if (objectIndex >= view.header->objectCount)
					return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
				if (visited[objectIndex] == stamp)
					continue;
				visited[objectIndex] = stamp;
				const ImmutableSpatialUInt32 currentDiscovery =
					discoveryOrdinal++;
				const ImmutableSpatialObjectRecord &object =
					view.objects[objectIndex];
				if (object.objectID == 0 || !finiteFloat(object.positionX) ||
					!finiteFloat(object.positionY) ||
					!finiteFloat(object.positionZ) ||
					!finiteFloat(object.boundingCircleRadius) ||
					!finiteFloat(object.boundingSphereRadius) ||
					!finiteFloat(object.zCenterOffset) ||
					object.boundingCircleRadius < 0.0f ||
					object.boundingSphereRadius < 0.0f)
					return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
				float distanceSquared;
				if (!calculateDistanceSquared(query, object,
					distanceSquared) || !admitted(object, query))
					continue;
				if (context.phase == QUERY_PHASE_FILL)
				{
					const ImmutableSpatialResultSpan &span =
						context.scratch->spanScratch[queryIndex];
					if (resultCount >= span.count)
						return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
					ImmutableSpatialResult &result =
						context.scratch->resultScratch[span.begin + resultCount];
					result.objectIndex = objectIndex;
					result.objectID = object.objectID;
					result.generation = object.generation;
					result.discoveryOrdinal = currentDiscovery;
					result.buildCost = object.buildCost;
					result.distanceSquared = distanceSquared;
				}
				++resultCount;
			}
		}
	}

	if (context.phase == QUERY_PHASE_COUNT)
		context.scratch->counts[queryIndex] = resultCount;
	else
	{
		const ImmutableSpatialResultSpan &span =
			context.scratch->spanScratch[queryIndex];
		if (resultCount != span.count)
			return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
		if (span.count >= 2)
		{
			if (!sortResults(context.scratch->resultScratch + span.begin,
				context.scratch->sortScratch + span.begin, span.count,
				query.iteratorOrder))
				return IMMUTABLE_SPATIAL_OVERFLOW;
		}
	}
	return IMMUTABLE_SPATIAL_SUCCESS;
}

bool executeQueryRange(void *opaque, ImmutableSpatialUInt32 rangeIndex)
{
	QueryBatchContext &context = *static_cast<QueryBatchContext *>(opaque);
	if (rangeIndex >= context.rangeCount)
		return false;
	const ImmutableSpatialUInt32 quotient =
		context.queryCount / context.rangeCount;
	const ImmutableSpatialUInt32 remainder =
		context.queryCount % context.rangeCount;
	const ImmutableSpatialUInt32 begin = rangeIndex * quotient +
		(rangeIndex < remainder ? rangeIndex : remainder);
	const ImmutableSpatialUInt32 count = quotient +
		(rangeIndex < remainder ? 1u : 0u);
	const ImmutableSpatialUInt32 end = begin + count;
	ImmutableSpatialUInt32 queryIndex;
	for (queryIndex = begin; queryIndex < end; ++queryIndex)
	{
		const ImmutableSpatialStatus status = executeOneQuery(context,
			queryIndex, rangeIndex);
		context.scratch->states[queryIndex] =
			static_cast<ImmutableSpatialUInt32>(status) + 1;
		if (status != IMMUTABLE_SPATIAL_SUCCESS)
			return false;
	}
	return true;
}

ImmutableSpatialStatus runQueryPhase(QueryBatchContext &context)
{
	size_t stateBytes = 0;
	if (!checkedByteCount(context.queryCount,
		sizeof(ImmutableSpatialUInt32), stateBytes))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	memset(context.scratch->states, 0, stateBytes);
	const SpatialUInt64 visitCount =
		static_cast<SpatialUInt64>(context.rangeCount) *
		context.view->header->objectCount;
	if (visitCount != 0)
	{
		size_t visitBytes = 0;
		if (!checkedByteCount(visitCount,
			sizeof(ImmutableSpatialUInt32), visitBytes))
			return IMMUTABLE_SPATIAL_OVERFLOW;
		memset(context.scratch->visitStamps, 0, visitBytes);
	}

	bool dispatched = true;
	if (context.options->dispatch != 0)
	{
		dispatched = context.options->dispatch(
			context.options->dispatchContext, context.rangeCount,
			executeQueryRange, &context);
	}
	else
	{
		ImmutableSpatialUInt32 rangeIndex;
		for (rangeIndex = 0; rangeIndex < context.rangeCount; ++rangeIndex)
		{
			if (!executeQueryRange(&context, rangeIndex))
			{
				dispatched = false;
				break;
			}
		}
	}

	ImmutableSpatialUInt32 queryIndex;
	for (queryIndex = 0; queryIndex < context.queryCount; ++queryIndex)
	{
		const ImmutableSpatialUInt32 state = context.scratch->states[queryIndex];
		if (state == 0 || state > static_cast<ImmutableSpatialUInt32>(
			IMMUTABLE_SPATIAL_GENERATION_MISMATCH) + 1)
			return IMMUTABLE_SPATIAL_DISPATCH_FAILURE;
		const ImmutableSpatialStatus status =
			static_cast<ImmutableSpatialStatus>(state - 1);
		if (status != IMMUTABLE_SPATIAL_SUCCESS)
			return status;
	}
	return dispatched ? IMMUTABLE_SPATIAL_SUCCESS :
		IMMUTABLE_SPATIAL_DISPATCH_FAILURE;
}
}

ImmutableSpatialStatus MeasureImmutableSpatialArena(
	const ImmutableSpatialArenaInput &input,
	ImmutableSpatialUInt32 *requiredBytes)
{
	if (requiredBytes == 0)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	ImmutableSpatialUInt32 measured = 0;
	const ImmutableSpatialStatus status = validateArenaInput(input, measured);
	if (status == IMMUTABLE_SPATIAL_SUCCESS)
		*requiredBytes = measured;
	return status;
}

ImmutableSpatialStatus BuildImmutableSpatialArena(
	const ImmutableSpatialArenaInput &input, void *destination,
	ImmutableSpatialUInt32 destinationCapacity,
	ImmutableSpatialUInt32 *arenaBytes)
{
	if (arenaBytes == 0)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	ImmutableSpatialUInt32 measured = 0;
	ImmutableSpatialStatus status = validateArenaInput(input, measured);
	if (status != IMMUTABLE_SPATIAL_SUCCESS)
		return status;
	if (!alignedArena(destination))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	if (destinationCapacity < measured)
		return IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY;
	size_t objectBytes = 0;
	size_t cellBytes = 0;
	size_t memberBytes = 0;
	size_t radiusBytes = 0;
	size_t offsetBytes = 0;
	if (!checkedByteCount(input.objectCount,
		sizeof(ImmutableSpatialObjectRecord), objectBytes) ||
		!checkedByteCount(input.cellCount,
			sizeof(ImmutableSpatialCellRecord), cellBytes) ||
		!checkedByteCount(input.memberCount,
			sizeof(ImmutableSpatialMemberRecord), memberBytes) ||
		!checkedByteCount(input.radiusCount,
			sizeof(ImmutableSpatialRadiusRecord), radiusBytes) ||
		!checkedByteCount(input.offsetCount,
			sizeof(ImmutableSpatialOffsetRecord), offsetBytes))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	const MemoryRegion buildRegions[] = {
		{ destination, measured },
		{ &input, sizeof(input) },
		{ input.objects, objectBytes },
		{ input.cells, cellBytes },
		{ input.members, memberBytes },
		{ input.radii, radiusBytes },
		{ input.offsets, offsetBytes },
		{ arenaBytes, sizeof(*arenaBytes) }
	};
	if (!regionsAreDisjoint(buildRegions,
		sizeof(buildRegions) / sizeof(buildRegions[0])))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	ImmutableSpatialArenaHeader header;
	memset(&header, 0, sizeof(header));
	header.magic = IMMUTABLE_SPATIAL_ARENA_MAGIC;
	header.version = IMMUTABLE_SPATIAL_ARENA_VERSION;
	header.headerBytes = sizeof(header);
	header.arenaBytes = measured;
	header.cellSize = input.cellSize;
	header.generation = input.generation;
	header.gridWidth = input.gridWidth;
	header.gridHeight = input.gridHeight;
	header.objectCount = input.objectCount;
	header.cellCount = input.cellCount;
	header.memberCount = input.memberCount;
	header.radiusCount = input.radiusCount;
	header.offsetCount = input.offsetCount;

	ImmutableSpatialUInt32 cursor = sizeof(header);
	header.objectOffset = cursor;
	addBytes(input.objectCount, sizeof(ImmutableSpatialObjectRecord), cursor);
	header.cellOffset = cursor;
	addBytes(input.cellCount, sizeof(ImmutableSpatialCellRecord), cursor);
	header.memberOffset = cursor;
	addBytes(input.memberCount, sizeof(ImmutableSpatialMemberRecord), cursor);
	header.radiusOffset = cursor;
	addBytes(input.radiusCount, sizeof(ImmutableSpatialRadiusRecord), cursor);
	header.offsetOffset = cursor;
	header.validationToken = arenaValidationToken(header);

	unsigned char *base = static_cast<unsigned char *>(destination);
	if (objectBytes != 0)
		memcpy(base + header.objectOffset, input.objects, objectBytes);
	memcpy(base + header.cellOffset, input.cells, cellBytes);
	if (memberBytes != 0)
		memcpy(base + header.memberOffset, input.members, memberBytes);
	memcpy(base + header.radiusOffset, input.radii, radiusBytes);
	if (offsetBytes != 0)
		memcpy(base + header.offsetOffset, input.offsets, offsetBytes);
	memcpy(base, &header, sizeof(header));
	*arenaBytes = measured;
	return IMMUTABLE_SPATIAL_SUCCESS;
}

ImmutableSpatialStatus RefreshImmutableSpatialArenaObjects(
	void *arena, ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialGeneration &generation,
	const ImmutableSpatialObjectRecord *objects,
	ImmutableSpatialUInt32 objectCount)
{
	const ImmutableSpatialArenaHeader *existingHeader = 0;
	if (!validateArenaHeader(arena, arenaCapacity, existingHeader))
		return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
	if (objectCount != existingHeader->objectCount ||
		(objectCount != 0 && objects == 0))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	ImmutableSpatialUInt32 previousObjectID = 0;
	for (ImmutableSpatialUInt32 index = 0; index != objectCount; ++index)
	{
		const ImmutableSpatialObjectRecord &object = objects[index];
		if (object.objectID == 0 || object.objectID <= previousObjectID ||
			!finiteFloat(object.positionX) ||
			!finiteFloat(object.positionY) ||
			!finiteFloat(object.positionZ) ||
			!finiteFloat(object.boundingCircleRadius) ||
			!finiteFloat(object.boundingSphereRadius) ||
			!finiteFloat(object.zCenterOffset) ||
			object.boundingCircleRadius < 0.0f ||
			object.boundingSphereRadius < 0.0f)
			return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
		previousObjectID = object.objectID;
	}

	size_t objectBytes = 0;
	if (!checkedByteCount(objectCount,
		sizeof(ImmutableSpatialObjectRecord), objectBytes))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	const MemoryRegion regions[] = {
		{ arena, existingHeader->arenaBytes },
		{ objects, objectBytes }
	};
	if (!regionsAreDisjoint(regions, sizeof(regions) / sizeof(regions[0])))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	ImmutableSpatialArenaHeader refreshedHeader = *existingHeader;
	refreshedHeader.generation = generation;
	refreshedHeader.validationToken = arenaValidationToken(refreshedHeader);
	unsigned char *base = static_cast<unsigned char *>(arena);
	if (objectBytes != 0)
		memcpy(base + refreshedHeader.objectOffset, objects, objectBytes);
	memcpy(base, &refreshedHeader, sizeof(refreshedHeader));
	return IMMUTABLE_SPATIAL_SUCCESS;
}

bool ValidateImmutableSpatialArena(const void *arena,
	ImmutableSpatialUInt32 arenaCapacity)
{
	const ImmutableSpatialArenaHeader *header = 0;
	if (!validateArenaHeader(arena, arenaCapacity, header))
		return false;

	const unsigned char *base = static_cast<const unsigned char *>(arena);
	ImmutableSpatialArenaInput input;
	input.generation = header->generation;
	input.gridWidth = header->gridWidth;
	input.gridHeight = header->gridHeight;
	input.cellSize = header->cellSize;
	input.objects = reinterpret_cast<const ImmutableSpatialObjectRecord *>(
		base + header->objectOffset);
	input.objectCount = header->objectCount;
	input.cells = reinterpret_cast<const ImmutableSpatialCellRecord *>(
		base + header->cellOffset);
	input.cellCount = header->cellCount;
	input.members = reinterpret_cast<const ImmutableSpatialMemberRecord *>(
		base + header->memberOffset);
	input.memberCount = header->memberCount;
	input.radii = reinterpret_cast<const ImmutableSpatialRadiusRecord *>(
		base + header->radiusOffset);
	input.radiusCount = header->radiusCount;
	input.offsets = reinterpret_cast<const ImmutableSpatialOffsetRecord *>(
		base + header->offsetOffset);
	input.offsetCount = header->offsetCount;
	ImmutableSpatialUInt32 measured = 0;
	return validateArenaInput(input, measured) == IMMUTABLE_SPATIAL_SUCCESS &&
		measured == header->arenaBytes;
}

bool ValidateImmutableSpatialResultSpans(
	const ImmutableSpatialResultSpan *spans,
	ImmutableSpatialUInt32 spanCount,
	ImmutableSpatialUInt32 resultCount)
{
	if (spanCount != 0 && spans == 0)
		return false;
	ImmutableSpatialUInt32 expectedBegin = 0;
	ImmutableSpatialUInt32 index;
	for (index = 0; index < spanCount; ++index)
	{
		if (spans[index].begin != expectedBegin ||
			spans[index].count > resultCount - expectedBegin)
			return false;
		expectedBegin += spans[index].count;
	}
	return expectedBegin == resultCount;
}

bool ValidateImmutableSpatialResultGenerations(
	const void *arena, ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialResult *results,
	ImmutableSpatialUInt32 resultCount,
	ImmutableSpatialObjectGenerationResolver resolver,
	void *resolverContext, ImmutableSpatialUInt32 *firstStaleResult)
{
	ArenaView view;
	if (!makeArenaView(arena, arenaCapacity, view) ||
		(resultCount != 0 && results == 0))
	{
		if (firstStaleResult != 0)
			*firstStaleResult = 0;
		return false;
	}
	ImmutableSpatialUInt32 index;
	for (index = 0; index < resultCount; ++index)
	{
		const ImmutableSpatialResult &result = results[index];
		if (result.objectIndex >= view.header->objectCount)
			break;
		const ImmutableSpatialObjectRecord &object =
			view.objects[result.objectIndex];
		if (object.objectID != result.objectID ||
			!generationEqual(object.generation, result.generation) ||
			(resolver != 0 && !resolver(result.objectID,
				&result.generation, resolverContext)))
			break;
	}
	if (firstStaleResult != 0)
		*firstStaleResult = index;
	return index == resultCount;
}

ImmutableSpatialStatus ExecuteImmutableSpatialQueryBatch(
	const void *arena, ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialQuery *queries, ImmutableSpatialUInt32 queryCount,
	const ImmutableSpatialExecutionOptions &options,
	const ImmutableSpatialBatchScratch &scratch,
	ImmutableSpatialResult *output, ImmutableSpatialUInt32 outputCapacity,
	ImmutableSpatialResultSpan *outputSpans,
	ImmutableSpatialUInt32 outputSpanCapacity,
	ImmutableSpatialUInt32 *outputCount,
	ImmutableSpatialExecutionMetrics *metrics)
{
	ArenaView view;
	if (!makeArenaView(arena, arenaCapacity, view))
		return IMMUTABLE_SPATIAL_MALFORMED_ARENA;
	if (outputCount == 0 || options.workerCount == 0 ||
		(queryCount != 0 && queries == 0) ||
		queryCount == 0xffffffffu)
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	size_t queryBytes = 0;
	size_t countBytes = 0;
	size_t stateBytes = 0;
	size_t visitCapacityBytes = 0;
	size_t spanScratchBytes = 0;
	size_t resultScratchBytes = 0;
	size_t sortScratchBytes = 0;
	size_t outputCapacityBytes = 0;
	size_t outputSpanBytes = 0;
	if (!checkedByteCount(queryCount, sizeof(ImmutableSpatialQuery), queryBytes) ||
		!checkedByteCount(scratch.countCapacity,
			sizeof(ImmutableSpatialUInt32), countBytes) ||
		!checkedByteCount(scratch.stateCapacity,
			sizeof(ImmutableSpatialUInt32), stateBytes) ||
		!checkedByteCount(scratch.visitStampCapacity,
			sizeof(ImmutableSpatialUInt32), visitCapacityBytes) ||
		!checkedByteCount(scratch.spanScratchCapacity,
			sizeof(ImmutableSpatialResultSpan), spanScratchBytes) ||
		!checkedByteCount(scratch.resultScratchCapacity,
			sizeof(ImmutableSpatialResult), resultScratchBytes) ||
		!checkedByteCount(scratch.sortScratchCapacity,
			sizeof(ImmutableSpatialResult), sortScratchBytes) ||
		!checkedByteCount(outputCapacity,
			sizeof(ImmutableSpatialResult), outputCapacityBytes) ||
		!checkedByteCount(outputSpanCapacity,
			sizeof(ImmutableSpatialResultSpan), outputSpanBytes))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	const MemoryRegion executionRegions[] = {
		{ arena, view.header->arenaBytes },
		{ queries, queryBytes },
		{ &options, sizeof(options) },
		{ &scratch, sizeof(scratch) },
		{ scratch.counts, countBytes },
		{ scratch.states, stateBytes },
		{ scratch.visitStamps, visitCapacityBytes },
		{ scratch.spanScratch, spanScratchBytes },
		{ scratch.resultScratch, resultScratchBytes },
		{ scratch.sortScratch, sortScratchBytes },
		{ output, outputCapacityBytes },
		{ outputSpans, outputSpanBytes },
		{ outputCount, sizeof(*outputCount) },
		{ metrics, metrics != 0 ? sizeof(*metrics) : 0 }
	};
	if (!regionsAreDisjoint(executionRegions,
		sizeof(executionRegions) / sizeof(executionRegions[0])))
		return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	if (queryCount == 0)
	{
		if (options.resolveArenaGeneration != 0 &&
			!options.resolveArenaGeneration(&view.header->generation,
				options.generationContext))
			return IMMUTABLE_SPATIAL_GENERATION_MISMATCH;
		*outputCount = 0;
		if (metrics != 0)
			*metrics = ImmutableSpatialExecutionMetrics();
		return IMMUTABLE_SPATIAL_SUCCESS;
	}

	ImmutableSpatialUInt32 queryIndex;
	for (queryIndex = 0; queryIndex < queryCount; ++queryIndex)
	{
		if (!generationEqual(view.header->generation,
			queries[queryIndex].expectedArenaGeneration))
			return IMMUTABLE_SPATIAL_STALE_GENERATION;
		if (!queryIsValid(view, queries[queryIndex]))
			return IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
	}
	if (cancellationRequested(options))
		return IMMUTABLE_SPATIAL_CANCELLED;
	if (options.resolveArenaGeneration != 0 &&
		!options.resolveArenaGeneration(&view.header->generation,
			options.generationContext))
		return IMMUTABLE_SPATIAL_GENERATION_MISMATCH;

	const ImmutableSpatialUInt32 rangeCount =
		options.workerCount < queryCount ? options.workerCount : queryCount;
	const SpatialUInt64 visitRequired64 =
		static_cast<SpatialUInt64>(rangeCount) * view.header->objectCount;
	if (visitRequired64 > 0xffffffffu)
		return IMMUTABLE_SPATIAL_OVERFLOW;
	const ImmutableSpatialUInt32 visitRequired =
		static_cast<ImmutableSpatialUInt32>(visitRequired64);
	if (scratch.counts == 0 || scratch.countCapacity < queryCount ||
		scratch.states == 0 || scratch.stateCapacity < queryCount ||
		scratch.spanScratch == 0 || scratch.spanScratchCapacity < queryCount ||
		(visitRequired != 0 && (scratch.visitStamps == 0 ||
			scratch.visitStampCapacity < visitRequired)) ||
		outputSpans == 0 || outputSpanCapacity < queryCount)
		return IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY;

	QueryBatchContext context;
	context.view = &view;
	context.queries = queries;
	context.queryCount = queryCount;
	context.rangeCount = rangeCount;
	context.options = &options;
	context.scratch = &scratch;
	context.phase = QUERY_PHASE_COUNT;
	ImmutableSpatialStatus status = runQueryPhase(context);
	if (status != IMMUTABLE_SPATIAL_SUCCESS)
		return status;

	SpatialUInt64 total64 = 0;
	for (queryIndex = 0; queryIndex < queryCount; ++queryIndex)
	{
		if (total64 > 0xffffffffu)
			return IMMUTABLE_SPATIAL_OVERFLOW;
		scratch.spanScratch[queryIndex].begin =
			static_cast<ImmutableSpatialUInt32>(total64);
		scratch.spanScratch[queryIndex].count = scratch.counts[queryIndex];
		total64 += scratch.counts[queryIndex];
	}
	if (total64 > 0xffffffffu)
		return IMMUTABLE_SPATIAL_OVERFLOW;
	const ImmutableSpatialUInt32 total =
		static_cast<ImmutableSpatialUInt32>(total64);
	size_t totalResultBytes = 0;
	size_t publishedSpanBytes = 0;
	if (!checkedByteCount(total, sizeof(ImmutableSpatialResult),
		totalResultBytes) ||
		!checkedByteCount(queryCount, sizeof(ImmutableSpatialResultSpan),
			publishedSpanBytes))
		return IMMUTABLE_SPATIAL_OVERFLOW;
	if (total > outputCapacity || total > scratch.resultScratchCapacity ||
		total > scratch.sortScratchCapacity ||
		(total != 0 && (output == 0 || scratch.resultScratch == 0 ||
			scratch.sortScratch == 0)))
		return IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY;

	context.phase = QUERY_PHASE_FILL;
	status = runQueryPhase(context);
	if (status != IMMUTABLE_SPATIAL_SUCCESS)
		return status;
	if (cancellationRequested(options))
		return IMMUTABLE_SPATIAL_CANCELLED;
	if (options.resolveArenaGeneration != 0 &&
		!options.resolveArenaGeneration(&view.header->generation,
			options.generationContext))
		return IMMUTABLE_SPATIAL_GENERATION_MISMATCH;
	if (!ValidateImmutableSpatialResultSpans(scratch.spanScratch,
		queryCount, total) ||
		!ValidateImmutableSpatialResultGenerations(arena, arenaCapacity,
			scratch.resultScratch, total, options.resolveObjectGeneration,
			options.generationContext, 0))
		return IMMUTABLE_SPATIAL_GENERATION_MISMATCH;

	if (total != 0)
		memcpy(output, scratch.resultScratch, totalResultBytes);
	memcpy(outputSpans, scratch.spanScratch, publishedSpanBytes);
	*outputCount = total;
	if (metrics != 0)
	{
		metrics->rangeCount = rangeCount;
		metrics->countPassQueries = queryCount;
		metrics->fillPassQueries = queryCount;
		metrics->resultCount = total;
	}
	return IMMUTABLE_SPATIAL_SUCCESS;
}
}
