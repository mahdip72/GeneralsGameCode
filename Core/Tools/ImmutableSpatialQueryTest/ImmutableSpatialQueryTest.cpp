/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ImmutableSpatialQuery.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace
{
using namespace rts;

int g_failures = 0;

void expect(bool condition, const char *message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++g_failures;
	}
}

ImmutableSpatialGeneration generation(ImmutableSpatialUInt32 lifecycle,
	ImmutableSpatialUInt32 topology, ImmutableSpatialUInt32 facts)
{
	ImmutableSpatialGeneration value = { lifecycle, topology, facts };
	return value;
}

ImmutableSpatialObjectRecord objectRecord(ImmutableSpatialUInt32 objectID,
	float x, float y, float z, ImmutableSpatialInt32 cost,
	ImmutableSpatialUInt32 admissionMask)
{
	ImmutableSpatialObjectRecord object = {};
	object.objectID = objectID;
	object.generation = generation(objectID + 1, objectID + 2, objectID + 3);
	object.admissionMask = admissionMask;
	object.buildCost = cost;
	object.positionX = x;
	object.positionY = y;
	object.positionZ = z;
	object.boundingCircleRadius = 0.5f;
	object.boundingSphereRadius = 0.5f;
	object.zCenterOffset = 0.0f;
	return object;
}

ImmutableSpatialUInt32 legacyRadiusForFixture(ImmutableSpatialInt32 offsetX,
	ImmutableSpatialInt32 offsetY, float cellSize)
{
	const float halfCell = cellSize * 0.5f;
	const float centerX[4] = { -halfCell, halfCell, -halfCell, halfCell };
	const float centerY[4] = { -halfCell, -halfCell, halfCell, halfCell };
	const float x = static_cast<float>(offsetX) * cellSize;
	const float y = static_cast<float>(offsetY) * cellSize;
	const float otherX[4] = {
		x - halfCell, x + halfCell, x - halfCell, x + halfCell
	};
	const float otherY[4] = {
		y - halfCell, y - halfCell, y + halfCell, y + halfCell
	};
	double minimumDistanceSquared = 1e12;
	for (unsigned first = 0; first < 4; ++first)
	{
		for (unsigned second = 0; second < 4; ++second)
		{
			const double dx = centerX[first] - otherX[second];
			const double dy = centerY[first] - otherY[second];
			minimumDistanceSquared = std::min(minimumDistanceSquared,
				dx * dx + dy * dy);
		}
	}
	const double distance = std::sqrt(static_cast<float>(minimumDistanceSquared));
	return static_cast<ImmutableSpatialUInt32>(std::ceil(
		static_cast<float>(distance / cellSize)));
}

void buildLegacyRadiusTopology(ImmutableSpatialUInt32 width,
	ImmutableSpatialUInt32 height, float cellSize,
	std::vector<ImmutableSpatialRadiusRecord> &radii,
	std::vector<ImmutableSpatialOffsetRecord> &offsets)
{
	const double dx = static_cast<double>(width) * cellSize;
	const double dy = static_cast<double>(height) * cellSize;
	const ImmutableSpatialUInt32 maximumRadius =
		static_cast<ImmutableSpatialUInt32>(std::ceil(static_cast<float>(
			std::sqrt(dx * dx + dy * dy) / cellSize)));
	std::vector<std::vector<ImmutableSpatialOffsetRecord>> buckets(
		maximumRadius + 1);
	for (ImmutableSpatialInt32 y =
		-static_cast<ImmutableSpatialInt32>(height) + 1;
		y < static_cast<ImmutableSpatialInt32>(height); ++y)
	{
		for (ImmutableSpatialInt32 x =
			-static_cast<ImmutableSpatialInt32>(width) + 1;
			x < static_cast<ImmutableSpatialInt32>(width); ++x)
			buckets[legacyRadiusForFixture(x, y, cellSize)].push_back({ x, y });
	}
	for (const auto &bucket : buckets)
	{
		ImmutableSpatialRadiusRecord radius = {};
		radius.offsetBegin = static_cast<ImmutableSpatialUInt32>(offsets.size());
		radius.offsetCount = static_cast<ImmutableSpatialUInt32>(bucket.size());
		radii.push_back(radius);
		offsets.insert(offsets.end(), bucket.begin(), bucket.end());
	}
}

struct Fixture
{
	std::vector<ImmutableSpatialObjectRecord> objects;
	std::vector<ImmutableSpatialCellRecord> cells;
	std::vector<ImmutableSpatialMemberRecord> members;
	std::vector<ImmutableSpatialRadiusRecord> radii;
	std::vector<ImmutableSpatialOffsetRecord> offsets;
	std::vector<ImmutableSpatialUInt32> arena;
	ImmutableSpatialUInt32 arenaBytes = 0;
	ImmutableSpatialGeneration arenaGeneration = generation(7, 11, 13);
	float cellSize = 10.0f;

	Fixture()
	{
		objects.push_back(objectRecord(100, 0.0f, 0.0f, 0.0f, 0, 1));
		objects.push_back(objectRecord(101, 1.0f, 0.0f, 0.0f, 10, 1));
		objects.push_back(objectRecord(102, -1.0f, 0.0f, 3.0f, 10, 1));
		objects.push_back(objectRecord(103, 2.0f, 0.0f, 0.0f, 5, 1));
		objects.push_back(objectRecord(104, 0.0f, 0.0f, 0.0f, 100, 2));

		std::vector<std::vector<ImmutableSpatialUInt32>> perCell(9);
		perCell[3] = { 3, 1 }; // Left cell: first object 103, then duplicate 101.
		perCell[4] = { 0,
			static_cast<ImmutableSpatialUInt32>(
				IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX), 1, 2, 4 };
		perCell[5] = { 3 }; // Duplicate object 103 in another cell.
		for (const auto &cellMembers : perCell)
		{
			ImmutableSpatialCellRecord cell = {};
			cell.memberBegin = static_cast<ImmutableSpatialUInt32>(members.size());
			cell.memberCount = static_cast<ImmutableSpatialUInt32>(cellMembers.size());
			cells.push_back(cell);
			for (ImmutableSpatialUInt32 objectIndex : cellMembers)
				members.push_back({ objectIndex });
		}

		buildLegacyRadiusTopology(3, 3, cellSize, radii, offsets);
		build();
	}

	ImmutableSpatialArenaInput input() const
	{
		ImmutableSpatialArenaInput input = {};
		input.generation = arenaGeneration;
		input.gridWidth = 3;
		input.gridHeight = 3;
		input.cellSize = cellSize;
		input.objects = objects.empty() ? nullptr : objects.data();
		input.objectCount = static_cast<ImmutableSpatialUInt32>(objects.size());
		input.cells = cells.data();
		input.cellCount = static_cast<ImmutableSpatialUInt32>(cells.size());
		input.members = members.empty() ? nullptr : members.data();
		input.memberCount = static_cast<ImmutableSpatialUInt32>(members.size());
		input.radii = radii.data();
		input.radiusCount = static_cast<ImmutableSpatialUInt32>(radii.size());
		input.offsets = offsets.empty() ? nullptr : offsets.data();
		input.offsetCount = static_cast<ImmutableSpatialUInt32>(offsets.size());
		return input;
	}

	void build()
	{
		ImmutableSpatialUInt32 required = 0;
		expect(MeasureImmutableSpatialArena(input(), &required) ==
			IMMUTABLE_SPATIAL_SUCCESS, "fixture arena measures");
		arena.assign((required + 3) / 4, 0);
		expect(BuildImmutableSpatialArena(input(), arena.data(), required,
			&arenaBytes) == IMMUTABLE_SPATIAL_SUCCESS,
			"fixture arena builds");
		expect(arenaBytes == required, "fixture build publishes measured bytes");
		expect(ValidateImmutableSpatialArena(arena.data(), arenaBytes),
			"fixture arena validates");
	}
};

ImmutableSpatialQuery baseQuery(const Fixture &fixture)
{
	ImmutableSpatialQuery query = {};
	query.expectedArenaGeneration = fixture.arenaGeneration;
	query.selfObjectIndex = 0;
	query.centerCellX = 1;
	query.centerCellY = 1;
	query.maximumRadius = 0;
	query.positionX = 0.0f;
	query.positionY = 0.0f;
	query.positionZ = 0.0f;
	query.boundingCircleRadius = 0.5f;
	query.boundingSphereRadius = 0.5f;
	query.zCenterOffset = 0.0f;
	query.maximumDistance = 10.0f;
	query.requiredAdmissionMask = 1;
	query.rejectedAdmissionMask = 0;
	query.distanceType = IMMUTABLE_SPATIAL_FROM_CENTER_2D;
	query.iteratorOrder = IMMUTABLE_SPATIAL_ITER_FASTEST;
	return query;
}

float legacyDistanceSquaredReference(const ImmutableSpatialQuery &query,
	const ImmutableSpatialObjectRecord &object)
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
		return actualDistanceSquared;
	const float actualDistance = std::sqrt(actualDistanceSquared);
	const float shrunkenDistance = actualDistance - totalRadius;
	return shrunkenDistance <= 0.0f ? 0.0f :
		shrunkenDistance * shrunkenDistance;
}

struct DispatchContext
{
	bool parallel = false;
	bool reverse = false;
	ImmutableSpatialUInt32 failAfter = std::numeric_limits<ImmutableSpatialUInt32>::max();
	ImmutableSpatialUInt32 failOnDispatchCall = 0;
	ImmutableSpatialUInt32 dispatchCalls = 0;
	ImmutableSpatialUInt32 lastRangeCount = 0;
	std::atomic<bool> *cancelFlag = nullptr;
	ImmutableSpatialUInt32 cancelOnDispatchCall = 0;
	ImmutableSpatialUInt32 cancelAfterRanges = 0;
};

bool dispatchRanges(void *opaque, ImmutableSpatialUInt32 rangeCount,
	ImmutableSpatialRangeFunction function, void *rangeContext)
{
	auto &context = *static_cast<DispatchContext *>(opaque);
	++context.dispatchCalls;
	context.lastRangeCount = rangeCount;
	if (context.cancelFlag != nullptr &&
		context.cancelOnDispatchCall == context.dispatchCalls &&
		context.cancelAfterRanges == 0)
		context.cancelFlag->store(true, std::memory_order_relaxed);
	if (context.failAfter != std::numeric_limits<ImmutableSpatialUInt32>::max() &&
		(context.failOnDispatchCall == 0 ||
			context.failOnDispatchCall == context.dispatchCalls))
	{
		const ImmutableSpatialUInt32 stop = std::min(rangeCount,
			context.failAfter);
		for (ImmutableSpatialUInt32 index = 0; index < stop; ++index)
			if (!function(rangeContext, index))
				return false;
		return false;
	}
	if (!context.parallel)
	{
		for (ImmutableSpatialUInt32 ordinal = 0; ordinal < rangeCount; ++ordinal)
		{
			if (context.cancelFlag != nullptr &&
				context.cancelOnDispatchCall == context.dispatchCalls &&
				context.cancelAfterRanges == ordinal)
				context.cancelFlag->store(true, std::memory_order_relaxed);
			const ImmutableSpatialUInt32 index = context.reverse ?
				rangeCount - ordinal - 1 : ordinal;
			if (!function(rangeContext, index))
				return false;
		}
		return true;
	}
	std::vector<int> succeeded(rangeCount, 0);
	std::vector<std::thread> threads;
	threads.reserve(rangeCount);
	for (ImmutableSpatialUInt32 index = 0; index < rangeCount; ++index)
	{
		threads.emplace_back([=, &succeeded]() {
			succeeded[index] = function(rangeContext, index) ? 1 : 0;
		});
	}
	for (auto &thread : threads)
		thread.join();
	return std::all_of(succeeded.begin(), succeeded.end(),
		[](int value) { return value != 0; });
}

bool cancellation(void *opaque)
{
	return static_cast<std::atomic<bool> *>(opaque)->load(
		std::memory_order_relaxed);
}

struct GenerationContext
{
	ImmutableSpatialGeneration arenaGeneration = {};
	ImmutableSpatialUInt32 arenaCalls = 0;
	ImmutableSpatialUInt32 failArenaCall = 0;
	ImmutableSpatialUInt32 staleObjectID = 0;
};

bool resolveArena(const ImmutableSpatialGeneration *expected, void *opaque)
{
	auto &context = *static_cast<GenerationContext *>(opaque);
	++context.arenaCalls;
	if (context.failArenaCall == context.arenaCalls)
		return false;
	return expected->lifecycle == context.arenaGeneration.lifecycle &&
		expected->topology == context.arenaGeneration.topology &&
		expected->facts == context.arenaGeneration.facts;
}

bool resolveObject(ImmutableSpatialUInt32 objectID,
	const ImmutableSpatialGeneration *, void *opaque)
{
	return objectID != static_cast<GenerationContext *>(opaque)->staleObjectID;
}

struct RunStorage
{
	std::vector<ImmutableSpatialUInt32> counts;
	std::vector<ImmutableSpatialUInt32> states;
	std::vector<ImmutableSpatialUInt32> visits;
	std::vector<ImmutableSpatialResultSpan> spanScratch;
	std::vector<ImmutableSpatialResult> resultScratch;
	std::vector<ImmutableSpatialResult> sortScratch;
	std::vector<ImmutableSpatialResult> output;
	std::vector<ImmutableSpatialResultSpan> spans;
	ImmutableSpatialUInt32 outputCount = 0xccccccccu;

	RunStorage(ImmutableSpatialUInt32 queryCount,
		ImmutableSpatialUInt32 rangeCount,
		ImmutableSpatialUInt32 objectCount,
		ImmutableSpatialUInt32 resultCapacity)
		: counts(queryCount, 0), states(queryCount, 0),
		  visits(static_cast<std::size_t>(rangeCount) * objectCount, 0),
		  spanScratch(queryCount), resultScratch(resultCapacity),
		  sortScratch(resultCapacity), output(resultCapacity), spans(queryCount)
	{
		if (!output.empty())
			std::memset(output.data(), 0xcc,
				output.size() * sizeof(ImmutableSpatialResult));
		std::memset(spans.data(), 0xcc,
			spans.size() * sizeof(ImmutableSpatialResultSpan));
	}

	ImmutableSpatialBatchScratch scratch()
	{
		ImmutableSpatialBatchScratch value = {};
		value.counts = counts.empty() ? nullptr : counts.data();
		value.countCapacity = static_cast<ImmutableSpatialUInt32>(counts.size());
		value.states = states.empty() ? nullptr : states.data();
		value.stateCapacity = static_cast<ImmutableSpatialUInt32>(states.size());
		value.visitStamps = visits.empty() ? nullptr : visits.data();
		value.visitStampCapacity = static_cast<ImmutableSpatialUInt32>(visits.size());
		value.spanScratch = spanScratch.empty() ? nullptr : spanScratch.data();
		value.spanScratchCapacity = static_cast<ImmutableSpatialUInt32>(spanScratch.size());
		value.resultScratch = resultScratch.empty() ? nullptr : resultScratch.data();
		value.resultScratchCapacity = static_cast<ImmutableSpatialUInt32>(resultScratch.size());
		value.sortScratch = sortScratch.empty() ? nullptr : sortScratch.data();
		value.sortScratchCapacity = static_cast<ImmutableSpatialUInt32>(sortScratch.size());
		return value;
	}
};

ImmutableSpatialStatus execute(const Fixture &fixture,
	const std::vector<ImmutableSpatialQuery> &queries,
	ImmutableSpatialExecutionOptions &options, RunStorage &storage,
	ImmutableSpatialExecutionMetrics *metrics = nullptr)
{
	ImmutableSpatialBatchScratch scratch = storage.scratch();
	return ExecuteImmutableSpatialQueryBatch(fixture.arena.data(),
		fixture.arenaBytes, queries.data(),
		static_cast<ImmutableSpatialUInt32>(queries.size()), options, scratch,
		storage.output.empty() ? nullptr : storage.output.data(),
		static_cast<ImmutableSpatialUInt32>(storage.output.size()),
		storage.spans.empty() ? nullptr : storage.spans.data(),
		static_cast<ImmutableSpatialUInt32>(storage.spans.size()),
		&storage.outputCount, metrics);
}

std::vector<ImmutableSpatialUInt32> ids(const RunStorage &storage,
	ImmutableSpatialUInt32 queryIndex = 0)
{
	std::vector<ImmutableSpatialUInt32> value;
	const ImmutableSpatialResultSpan span = storage.spans[queryIndex];
	for (ImmutableSpatialUInt32 index = 0; index < span.count; ++index)
		value.push_back(storage.output[span.begin + index].objectID);
	return value;
}

void expectIDs(const RunStorage &storage,
	std::initializer_list<ImmutableSpatialUInt32> expected,
	const char *message)
{
	expect(ids(storage) == std::vector<ImmutableSpatialUInt32>(expected), message);
}

bool outputStillSentinel(const RunStorage &storage);

void testArenaBuildRelocationAndMalformedInput()
{
	Fixture fixture;
	expect(fixture.offsets.size() == 25 && fixture.radii.size() == 6,
		"3x3 legacy radius topology contains all 25 offsets and six buckets");
	expect(fixture.radii[0].offsetCount == 9 &&
		fixture.offsets[fixture.radii[0].offsetBegin].x == -1 &&
		fixture.offsets[fixture.radii[0].offsetBegin].y == -1 &&
		fixture.offsets[fixture.radii[0].offsetBegin + 8].x == 1 &&
		fixture.offsets[fixture.radii[0].offsetBegin + 8].y == 1,
		"radius zero preserves legacy y-major/x-minor adjacent-cell order");
	expect(fixture.radii[1].offsetCount == 12 &&
		fixture.radii[2].offsetCount == 4 &&
		fixture.radii[3].offsetCount == 0 &&
		fixture.radii[4].offsetCount == 0 &&
		fixture.radii[5].offsetCount == 0,
		"legacy radius assignment includes edge, corner, and trailing empty buckets");
	const ImmutableSpatialOffsetRecord exactLegacyOffsets[] = {
		{ -1, -1 }, { 0, -1 }, { 1, -1 },
		{ -1, 0 }, { 0, 0 }, { 1, 0 },
		{ -1, 1 }, { 0, 1 }, { 1, 1 },
		{ -1, -2 }, { 0, -2 }, { 1, -2 },
		{ -2, -1 }, { 2, -1 }, { -2, 0 }, { 2, 0 },
		{ -2, 1 }, { 2, 1 },
		{ -1, 2 }, { 0, 2 }, { 1, 2 },
		{ -2, -2 }, { 2, -2 }, { -2, 2 }, { 2, 2 }
	};
	bool exactLegacyOrder = fixture.offsets.size() ==
		sizeof(exactLegacyOffsets) / sizeof(exactLegacyOffsets[0]);
	for (std::size_t index = 0; exactLegacyOrder &&
		index < fixture.offsets.size(); ++index)
	{
		exactLegacyOrder = fixture.offsets[index].x ==
			exactLegacyOffsets[index].x && fixture.offsets[index].y ==
			exactLegacyOffsets[index].y;
	}
	expect(exactLegacyOrder,
		"3x3 fixture exactly matches calcRadiusVec bucket and insertion order");

	auto omittedOffsets = fixture.offsets;
	omittedOffsets.pop_back();
	ImmutableSpatialArenaInput omitted = fixture.input();
	omitted.offsets = omittedOffsets.data();
	omitted.offsetCount = static_cast<ImmutableSpatialUInt32>(omittedOffsets.size());
	ImmutableSpatialUInt32 rejectedBytes = 0xababababu;
	expect(MeasureImmutableSpatialArena(omitted, &rejectedBytes) ==
		IMMUTABLE_SPATIAL_INVALID_ARGUMENT && rejectedBytes == 0xababababu,
		"omitted supported offset coverage is rejected transactionally");

	auto wrongBucketOffsets = fixture.offsets;
	std::swap(wrongBucketOffsets[fixture.radii[0].offsetBegin],
		wrongBucketOffsets[fixture.radii[1].offsetBegin]);
	ImmutableSpatialArenaInput wrongBucket = fixture.input();
	wrongBucket.offsets = wrongBucketOffsets.data();
	expect(MeasureImmutableSpatialArena(wrongBucket, &rejectedBytes) ==
		IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"offset assigned to the wrong legacy radius bucket is rejected");

	auto wrongOrderOffsets = fixture.offsets;
	std::swap(wrongOrderOffsets[fixture.radii[0].offsetBegin],
		wrongOrderOffsets[fixture.radii[0].offsetBegin + 1]);
	ImmutableSpatialArenaInput wrongOrder = fixture.input();
	wrongOrder.offsets = wrongOrderOffsets.data();
	expect(MeasureImmutableSpatialArena(wrongOrder, &rejectedBytes) ==
		IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"non-canonical offset insertion order is rejected");

	auto unsortedObjects = fixture.objects;
	std::swap(unsortedObjects[1], unsortedObjects[2]);
	ImmutableSpatialArenaInput unsorted = fixture.input();
	unsorted.objects = unsortedObjects.data();
	expect(MeasureImmutableSpatialArena(unsorted, &rejectedBytes) ==
		IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"object IDs must be strictly sorted for linear uniqueness validation");

	std::vector<ImmutableSpatialUInt32> relocated = fixture.arena;
	expect(ValidateImmutableSpatialArena(relocated.data(), fixture.arenaBytes),
		"relocated arena validates at a different address");

	std::vector<ImmutableSpatialUInt32> malformed = relocated;
	auto *header = reinterpret_cast<ImmutableSpatialArenaHeader *>(malformed.data());
	++header->memberOffset;
	expect(!ValidateImmutableSpatialArena(malformed.data(), fixture.arenaBytes),
		"non-canonical section offset is rejected");

	malformed = relocated;
	header = reinterpret_cast<ImmutableSpatialArenaHeader *>(malformed.data());
	auto *members = reinterpret_cast<ImmutableSpatialMemberRecord *>(
		reinterpret_cast<unsigned char *>(malformed.data()) + header->memberOffset);
	members[0].objectIndex = header->objectCount;
	expect(!ValidateImmutableSpatialArena(malformed.data(), fixture.arenaBytes),
		"out-of-range member reference is rejected");

	ImmutableSpatialArenaInput invalid = fixture.input();
	std::vector<ImmutableSpatialObjectRecord> invalidObjects = fixture.objects;
	invalidObjects[1].objectID = 0;
	invalid.objects = invalidObjects.data();
	std::vector<ImmutableSpatialUInt32> destination(fixture.arena.size(),
		0xa5a5a5a5u);
	const std::vector<ImmutableSpatialUInt32> before = destination;
	ImmutableSpatialUInt32 publishedBytes = 0xddddddddu;
	expect(BuildImmutableSpatialArena(invalid, destination.data(),
		fixture.arenaBytes, &publishedBytes) == IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"invalid arena input fails before fill");
	expect(destination == before && publishedBytes == 0xddddddddu,
		"invalid build leaves destination and byte publication untouched");

	publishedBytes = 0xeeeeeeeeu;
	expect(BuildImmutableSpatialArena(fixture.input(), destination.data(),
		fixture.arenaBytes - 1, &publishedBytes) ==
		IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY,
		"short arena allocation is rejected");
	expect(destination == before && publishedBytes == 0xeeeeeeeeu,
		"short allocation has no partial arena publication");

	auto aliasedObjects = fixture.objects;
	const auto aliasedObjectsBefore = aliasedObjects;
	ImmutableSpatialArenaInput aliasedInput = fixture.input();
	aliasedInput.objects = aliasedObjects.data();
	publishedBytes = 0xcdcdcdcdu;
	expect(BuildImmutableSpatialArena(aliasedInput, aliasedObjects.data(),
		fixture.arenaBytes, &publishedBytes) ==
		IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"arena destination overlapping source records is rejected");
	expect(std::memcmp(aliasedObjects.data(), aliasedObjectsBefore.data(),
		aliasedObjects.size() * sizeof(ImmutableSpatialObjectRecord)) == 0 &&
		publishedBytes == 0xcdcdcdcdu,
		"build alias rejection occurs before any destination write");

	ImmutableSpatialArenaInput overflow = {};
	overflow.gridWidth = std::numeric_limits<ImmutableSpatialUInt32>::max();
	overflow.gridHeight = 2;
	overflow.cellSize = 1.0f;
	overflow.radiusCount = 1;
	ImmutableSpatialRadiusRecord emptyRadius = {};
	overflow.radii = &emptyRadius;
	ImmutableSpatialUInt32 required = 0xfefefefeu;
	expect(MeasureImmutableSpatialArena(overflow, &required) ==
		IMMUTABLE_SPATIAL_OVERFLOW, "grid product overflow fails closed");
	expect(required == 0xfefefefeu,
		"overflow does not publish a partial required size");
}

void testLegacyTraversalDuplicateNullSelfAndOrders()
{
	Fixture fixture;
	const ImmutableSpatialUInt32 expectedByOrder[][3] = {
		{ 102, 101, 103 },
		{ 102, 101, 103 },
		{ 103, 102, 101 },
		{ 103, 102, 101 },
		{ 102, 101, 103 }
	};
	for (ImmutableSpatialUInt32 order = 0; order < 5; ++order)
	{
		ImmutableSpatialQuery query = baseQuery(fixture);
		query.iteratorOrder = order;
		std::vector<ImmutableSpatialQuery> queries(1, query);
		ImmutableSpatialExecutionOptions options;
		RunStorage storage(1, 1, 5, 8);
		expect(execute(fixture, queries, options, storage) ==
			IMMUTABLE_SPATIAL_SUCCESS, "legacy iterator order query succeeds");
		expect(storage.outputCount == 3 && storage.spans[0].count == 3,
			"duplicate multicell objects, self, null, and rejected fact are omitted");
		std::vector<ImmutableSpatialUInt32> expected(
			expectedByOrder[order], expectedByOrder[order] + 3);
		expect(ids(storage) == expected,
			"iterator ordering and reverse-discovery tie rules match legacy");
		if (order == IMMUTABLE_SPATIAL_ITER_FASTEST)
		{
			expect(storage.output[0].discoveryOrdinal == 2 &&
				storage.output[1].discoveryOrdinal == 1 &&
				storage.output[2].discoveryOrdinal == 0,
				"first-unique discovery ordinals include filtered observations");
		}
	}
}

void testStrictDistanceBoundariesAndDistanceKinds()
{
	Fixture fixture;
	ImmutableSpatialExecutionOptions options;
	auto run = [&](ImmutableSpatialQuery query) {
		std::vector<ImmutableSpatialQuery> queries(1, query);
		RunStorage storage(1, 1, 5, 8);
		expect(execute(fixture, queries, options, storage) ==
			IMMUTABLE_SPATIAL_SUCCESS, "distance query succeeds");
		return storage;
	};

	ImmutableSpatialQuery query = baseQuery(fixture);
	query.maximumDistance = 1.0f;
	RunStorage exactCenter = run(query);
	expect(exactCenter.outputCount == 0,
		"center distance equal to maximum is strictly excluded");
	query.maximumDistance = 1.01f;
	RunStorage insideCenter = run(query);
	expectIDs(insideCenter, { 102, 101 },
		"center 2D includes values strictly inside boundary with tie reversal");

	query.distanceType = IMMUTABLE_SPATIAL_FROM_CENTER_3D;
	RunStorage center3D = run(query);
	expectIDs(center3D, { 101 }, "center 3D consumes copied z distance");

	query.distanceType = IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_2D;
	query.maximumDistance = 0.0f;
	RunStorage zeroBoundary = run(query);
	expect(zeroBoundary.outputCount == 0,
		"overlapping boundaries at zero are still strictly excluded");
	query.maximumDistance = 0.01f;
	RunStorage boundary2D = run(query);
	expectIDs(boundary2D, { 102, 101 },
		"bounding-circle 2D applies copied radii");

	query.distanceType = IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_3D;
	query.maximumDistance = std::sqrt(10.0f) - 1.0f;
	RunStorage exact3D = run(query);
	expectIDs(exact3D, { 101, 103 },
		"bounding-sphere 3D excludes the exact shrunken boundary");
	query.maximumDistance += 0.001f;
	RunStorage inside3D = run(query);
	expectIDs(inside3D, { 102, 101, 103 },
		"bounding-sphere 3D includes the value just inside");

	query = baseQuery(fixture);
	query.requiredAdmissionMask = 0x80000000u;
	std::vector<ImmutableSpatialQuery> emptyQueries(1, query);
	RunStorage nullResultStorage(1, 1, 5, 0);
	expect(execute(fixture, emptyQueries, options, nullResultStorage) ==
		IMMUTABLE_SPATIAL_SUCCESS,
		"zero-result query permits null result, sort, and output storage");
	expect(nullResultStorage.outputCount == 0 &&
		nullResultStorage.spans[0].begin == 0 &&
		nullResultStorage.spans[0].count == 0 &&
		nullResultStorage.resultScratch.empty() &&
		nullResultStorage.sortScratch.empty() &&
		nullResultStorage.output.empty(),
		"zero-result fill publishes only an empty span without pointer formation");
}

void testNearBoundaryFloatingPointDifferential()
{
	Fixture fixture;
	for (auto &object : fixture.objects)
		object.admissionMask = 0;
	auto &target = fixture.objects[1];
	target.admissionMask = 1;
	target.positionX = 1000.125f;
	target.positionY = -17.03125f;
	target.positionZ = 3.0078125f;
	target.boundingCircleRadius = 0.375f;
	target.boundingSphereRadius = 0.625f;
	target.zCenterOffset = 0.25f;
	fixture.build();

	for (ImmutableSpatialUInt32 distanceType =
		IMMUTABLE_SPATIAL_FROM_CENTER_2D;
		distanceType <= IMMUTABLE_SPATIAL_FROM_BOUNDING_SPHERE_3D;
		++distanceType)
	{
		ImmutableSpatialQuery query = baseQuery(fixture);
		query.selfObjectIndex = static_cast<ImmutableSpatialUInt32>(
			IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX);
		query.positionX = -0.25f;
		query.positionY = 0.5f;
		query.positionZ = -1.0f;
		query.boundingCircleRadius = 0.2f;
		query.boundingSphereRadius = 0.4f;
		query.zCenterOffset = 0.1f;
		query.distanceType = distanceType;
		const float referenceDistanceSquared =
			legacyDistanceSquaredReference(query, target);
		const float boundary = std::sqrt(referenceDistanceSquared);
		const float limits[] = {
			std::nextafter(boundary,
				-std::numeric_limits<float>::infinity()),
			boundary,
			std::nextafter(boundary,
				std::numeric_limits<float>::infinity())
		};
		for (float limit : limits)
		{
			query.maximumDistance = limit;
			const bool expected = referenceDistanceSquared < limit * limit;
			std::vector<ImmutableSpatialQuery> queries(1, query);
			ImmutableSpatialExecutionOptions options;
			RunStorage storage(1, 1, 5, 2);
			expect(execute(fixture, queries, options, storage) ==
				IMMUTABLE_SPATIAL_SUCCESS,
				"near-boundary differential query succeeds");
			expect((storage.outputCount == 1) == expected,
				"kernel strict comparison matches legacy float operation order");
			if (expected)
			{
				expect(storage.output[0].objectID == target.objectID &&
					std::memcmp(&storage.output[0].distanceSquared,
						&referenceDistanceSquared, sizeof(float)) == 0,
					"published distance is bit-identical to legacy reference");
			}
		}
	}
}

void testEdgeAndOutOfMapTraversal()
{
	Fixture fixture;
	ImmutableSpatialQuery query = baseQuery(fixture);
	query.selfObjectIndex = static_cast<ImmutableSpatialUInt32>(
		IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX);
	query.centerCellX = 0;
	query.centerCellY = 1;
	query.positionX = 0.0f;
	query.iteratorOrder = IMMUTABLE_SPATIAL_ITER_FASTEST;
	std::vector<ImmutableSpatialQuery> queries(1, query);
	ImmutableSpatialExecutionOptions options;
	RunStorage storage(1, 1, 5, 8);
	expect(execute(fixture, queries, options, storage) ==
		IMMUTABLE_SPATIAL_SUCCESS, "edge query succeeds");
	expectIDs(storage, { 102, 100, 101, 103 },
		"out-of-map offsets are skipped without disturbing surviving COI order");
	query.centerCellX = -1;
	queries[0] = query;
	RunStorage outside(1, 1, 5, 8);
	expect(execute(fixture, queries, options, outside) ==
		IMMUTABLE_SPATIAL_SUCCESS, "off-map query center succeeds");
	expectIDs(outside, { 101, 103 },
		"off-map center visits only valid translated offsets");
}

void testRelocatedExecutionAndSpanValidation()
{
	Fixture fixture;
	ImmutableSpatialQuery query = baseQuery(fixture);
	std::vector<ImmutableSpatialQuery> queries(2, query);
	ImmutableSpatialExecutionOptions options;
	RunStorage original(2, 1, 5, 8);
	expect(execute(fixture, queries, options, original) ==
		IMMUTABLE_SPATIAL_SUCCESS, "original arena executes");

	Fixture relocated = fixture;
	relocated.arena = fixture.arena;
	RunStorage moved(2, 1, 5, 8);
	expect(execute(relocated, queries, options, moved) ==
		IMMUTABLE_SPATIAL_SUCCESS, "relocated arena executes");
	expect(original.outputCount == moved.outputCount &&
		std::memcmp(original.output.data(), moved.output.data(),
			original.outputCount * sizeof(ImmutableSpatialResult)) == 0 &&
		std::memcmp(original.spans.data(), moved.spans.data(),
			queries.size() * sizeof(ImmutableSpatialResultSpan)) == 0,
		"relocation preserves byte-identical query publication");
	expect(ValidateImmutableSpatialResultSpans(moved.spans.data(), 2,
		moved.outputCount), "dense published result spans validate");
	auto badSpans = moved.spans;
	++badSpans[1].begin;
	expect(!ValidateImmutableSpatialResultSpans(badSpans.data(), 2,
		moved.outputCount), "gapped result span is rejected");
}

void testWorkerCountsAndMoreThanSixtyFourRanges()
{
	Fixture fixture;
	const ImmutableSpatialQuery query = baseQuery(fixture);
	std::vector<ImmutableSpatialQuery> queries(130, query);
	std::vector<ImmutableSpatialResult> baselineResults;
	std::vector<ImmutableSpatialResultSpan> baselineSpans;
	const ImmutableSpatialUInt32 workerCounts[] = { 1, 2, 4, 8, 16, 96 };
	for (ImmutableSpatialUInt32 workerCount : workerCounts)
	{
		DispatchContext dispatch;
		dispatch.parallel = workerCount > 1 && workerCount <= 16;
		dispatch.reverse = workerCount > 64;
		ImmutableSpatialExecutionOptions options;
		options.workerCount = workerCount;
		options.dispatch = dispatchRanges;
		options.dispatchContext = &dispatch;
		const ImmutableSpatialUInt32 rangeCount = std::min<ImmutableSpatialUInt32>(
			workerCount, static_cast<ImmutableSpatialUInt32>(queries.size()));
		RunStorage storage(static_cast<ImmutableSpatialUInt32>(queries.size()),
			rangeCount, 5, static_cast<ImmutableSpatialUInt32>(queries.size() * 3));
		ImmutableSpatialExecutionMetrics metrics;
		expect(execute(fixture, queries, options, storage, &metrics) ==
			IMMUTABLE_SPATIAL_SUCCESS, "requested worker-count batch succeeds");
		expect(metrics.rangeCount == rangeCount &&
			dispatch.lastRangeCount == rangeCount && storage.outputCount == 390,
			"worker count maps to unbounded deterministic range count");
		if (baselineResults.empty())
		{
			baselineResults.assign(storage.output.begin(),
				storage.output.begin() + storage.outputCount);
			baselineSpans = storage.spans;
		}
		else
		{
			expect(std::memcmp(baselineResults.data(), storage.output.data(),
				storage.outputCount * sizeof(ImmutableSpatialResult)) == 0 &&
				std::memcmp(baselineSpans.data(), storage.spans.data(),
					storage.spans.size() * sizeof(ImmutableSpatialResultSpan)) == 0,
				"1/2/4/8/16/>64 ranges publish byte-identical results");
		}
	}
}

void testLargeCanonicalTopologyAndCheapBatchValidation()
{
	const ImmutableSpatialUInt32 width = 128;
	const ImmutableSpatialUInt32 height = 128;
	const float cellSize = 10.0f;
	std::vector<ImmutableSpatialCellRecord> cells(width * height);
	std::vector<ImmutableSpatialRadiusRecord> radii;
	std::vector<ImmutableSpatialOffsetRecord> offsets;
	buildLegacyRadiusTopology(width, height, cellSize, radii, offsets);
	expect(offsets.size() == 65025,
		"128x128 canonical topology contains every supported offset once");

	ImmutableSpatialArenaInput input = {};
	input.generation = generation(31, 37, 41);
	input.gridWidth = width;
	input.gridHeight = height;
	input.cellSize = cellSize;
	input.cells = cells.data();
	input.cellCount = static_cast<ImmutableSpatialUInt32>(cells.size());
	input.radii = radii.data();
	input.radiusCount = static_cast<ImmutableSpatialUInt32>(radii.size());
	input.offsets = offsets.data();
	input.offsetCount = static_cast<ImmutableSpatialUInt32>(offsets.size());
	ImmutableSpatialUInt32 arenaBytes = 0;
	expect(MeasureImmutableSpatialArena(input, &arenaBytes) ==
		IMMUTABLE_SPATIAL_SUCCESS,
		"128x128 full topology validates without quadratic uniqueness work");
	std::vector<ImmutableSpatialUInt32> arena((arenaBytes + 3) / 4);
	ImmutableSpatialUInt32 builtBytes = 0;
	expect(BuildImmutableSpatialArena(input, arena.data(), arenaBytes,
		&builtBytes) == IMMUTABLE_SPATIAL_SUCCESS && builtBytes == arenaBytes,
		"128x128 full topology builds transactionally");
	expect(ValidateImmutableSpatialArena(arena.data(), arenaBytes),
		"128x128 full arena passes explicit linear content validation");

	ImmutableSpatialQuery query = {};
	query.expectedArenaGeneration = input.generation;
	query.selfObjectIndex = static_cast<ImmutableSpatialUInt32>(
		IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX);
	query.centerCellX = 64;
	query.centerCellY = 64;
	query.maximumRadius = 0;
	query.maximumDistance = 1.0f;
	query.distanceType = IMMUTABLE_SPATIAL_FROM_CENTER_2D;
	query.iteratorOrder = IMMUTABLE_SPATIAL_ITER_FASTEST;
	std::vector<ImmutableSpatialQuery> queries(257, query);
	DispatchContext dispatch;
	dispatch.reverse = true;
	ImmutableSpatialExecutionOptions options;
	options.workerCount = 96;
	options.dispatch = dispatchRanges;
	options.dispatchContext = &dispatch;
	RunStorage storage(257, 96, 0, 0);
	ImmutableSpatialBatchScratch scratch = storage.scratch();
	ImmutableSpatialExecutionMetrics metrics;
	expect(ExecuteImmutableSpatialQueryBatch(arena.data(), arenaBytes,
		queries.data(), static_cast<ImmutableSpatialUInt32>(queries.size()),
		options, scratch, nullptr, 0, storage.spans.data(),
		static_cast<ImmutableSpatialUInt32>(storage.spans.size()),
		&storage.outputCount, &metrics) == IMMUTABLE_SPATIAL_SUCCESS,
		"repeated batches use cheap header token validation on the large arena");
	expect(storage.outputCount == 0 && metrics.rangeCount == 96,
		"large empty topology publishes deterministic empty spans");
}

void testAliasingIsRejectedBeforeWork()
{
	Fixture fixture;
	std::vector<ImmutableSpatialQuery> queries(1, baseQuery(fixture));
	ImmutableSpatialExecutionOptions options;

	RunStorage resultAlias(1, 1, 5, 8);
	ImmutableSpatialBatchScratch scratch = resultAlias.scratch();
	expect(ExecuteImmutableSpatialQueryBatch(fixture.arena.data(),
		fixture.arenaBytes, queries.data(), 1, options, scratch,
		scratch.resultScratch, scratch.resultScratchCapacity,
		resultAlias.spans.data(), 1, &resultAlias.outputCount, nullptr) ==
		IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"publication output cannot alias private result scratch");
	expect(resultAlias.outputCount == 0xccccccccu,
		"result alias rejection occurs before count publication");

	RunStorage stateAlias(1, 1, 5, 8);
	scratch = stateAlias.scratch();
	scratch.states = scratch.counts;
	expect(ExecuteImmutableSpatialQueryBatch(fixture.arena.data(),
		fixture.arenaBytes, queries.data(), 1, options, scratch,
		stateAlias.output.data(), 8, stateAlias.spans.data(), 1,
		&stateAlias.outputCount, nullptr) == IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"count and state scratch alias is rejected");
	expect(outputStillSentinel(stateAlias),
		"scratch alias rejection leaves publication untouched");

	RunStorage countAlias(1, 1, 5, 8);
	scratch = countAlias.scratch();
	auto *aliasedCount = reinterpret_cast<ImmutableSpatialUInt32 *>(
		countAlias.spans.data());
	expect(ExecuteImmutableSpatialQueryBatch(fixture.arena.data(),
		fixture.arenaBytes, queries.data(), 1, options, scratch,
		countAlias.output.data(), 8, countAlias.spans.data(), 1,
		aliasedCount, nullptr) == IMMUTABLE_SPATIAL_INVALID_ARGUMENT,
		"output count cannot alias published span storage");
	expect(outputStillSentinel(countAlias),
		"publication alias rejection performs no writes");
}

void testWin32SizeOverflowGuards()
{
#if !defined(_WIN32) || defined(_WIN64)
	return;
#else
	static_assert(sizeof(std::size_t) == 4,
		"Win32 overflow test requires 32-bit size_t");
	Fixture fixture;
	std::vector<ImmutableSpatialQuery> queries(1, baseQuery(fixture));
	ImmutableSpatialExecutionOptions options;
	RunStorage storage(1, 1, 5, 8);
	ImmutableSpatialBatchScratch scratch = storage.scratch();
	ImmutableSpatialResult dummy = {};
	scratch.resultScratch = &dummy;
	scratch.resultScratchCapacity =
		std::numeric_limits<ImmutableSpatialUInt32>::max();
	expect(ExecuteImmutableSpatialQueryBatch(fixture.arena.data(),
		fixture.arenaBytes, queries.data(), 1, options, scratch,
		storage.output.data(), 8, storage.spans.data(), 1,
		&storage.outputCount, nullptr) == IMMUTABLE_SPATIAL_OVERFLOW,
		"Win32 rejects count-times-record-size overflow before memory access");
	expect(outputStillSentinel(storage),
		"Win32 byte-count overflow leaves publication untouched");
#endif
}

bool outputStillSentinel(const RunStorage &storage)
{
	for (const auto &result : storage.output)
	{
		const auto *bytes = reinterpret_cast<const unsigned char *>(&result);
		for (std::size_t index = 0; index < sizeof(result); ++index)
			if (bytes[index] != 0xcc)
				return false;
	}
	for (const auto &span : storage.spans)
	{
		const auto *bytes = reinterpret_cast<const unsigned char *>(&span);
		for (std::size_t index = 0; index < sizeof(span); ++index)
			if (bytes[index] != 0xcc)
				return false;
	}
	return storage.outputCount == 0xccccccccu;
}

void testFailureCancellationCapacityAndNoPublication()
{
	Fixture fixture;
	std::vector<ImmutableSpatialQuery> queries(20, baseQuery(fixture));

	DispatchContext partial;
	partial.failAfter = 3;
	partial.failOnDispatchCall = 2;
	ImmutableSpatialExecutionOptions partialOptions;
	partialOptions.workerCount = 8;
	partialOptions.dispatch = dispatchRanges;
	partialOptions.dispatchContext = &partial;
	RunStorage partialStorage(20, 8, 5, 60);
	expect(execute(fixture, queries, partialOptions, partialStorage) ==
		IMMUTABLE_SPATIAL_DISPATCH_FAILURE,
		"partial range fill failure rejects the whole batch");
	expect(outputStillSentinel(partialStorage),
		"partial admission publishes no spans, results, or count");

	ImmutableSpatialExecutionOptions capacityOptions;
	capacityOptions.workerCount = 4;
	RunStorage shortOutput(20, 4, 5, 59);
	expect(execute(fixture, queries, capacityOptions, shortOutput) ==
		IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY,
		"short result allocation fails after transactional count");
	expect(outputStillSentinel(shortOutput),
		"short allocation has no partial publication");

	RunStorage shortVisits(20, 4, 5, 60);
	shortVisits.visits.pop_back();
	expect(execute(fixture, queries, capacityOptions, shortVisits) ==
		IMMUTABLE_SPATIAL_INSUFFICIENT_CAPACITY,
		"short per-range visited allocation fails before work");
	expect(outputStillSentinel(shortVisits),
		"visited allocation failure has no publication");

	std::atomic<bool> cancelled(false);
	DispatchContext cancelDispatch;
	cancelDispatch.cancelFlag = &cancelled;
	cancelDispatch.cancelOnDispatchCall = 2;
	cancelDispatch.cancelAfterRanges = 2;
	ImmutableSpatialExecutionOptions cancelOptions;
	cancelOptions.workerCount = 4;
	cancelOptions.dispatch = dispatchRanges;
	cancelOptions.dispatchContext = &cancelDispatch;
	cancelOptions.isCancelled = cancellation;
	cancelOptions.cancellationContext = &cancelled;
	RunStorage cancelledStorage(20, 4, 5, 60);
	expect(execute(fixture, queries, cancelOptions, cancelledStorage) ==
		IMMUTABLE_SPATIAL_CANCELLED,
		"cancellation after partial fill fails closed");
	expect(outputStillSentinel(cancelledStorage),
		"cancelled fill has no partial publication");
}

void testGenerationAndMalformedArenaFailClosed()
{
	Fixture fixture;
	std::vector<ImmutableSpatialQuery> queries(2, baseQuery(fixture));
	Fixture malformedMember = fixture;
	auto *memberHeader = reinterpret_cast<ImmutableSpatialArenaHeader *>(
		malformedMember.arena.data());
	auto *malformedMembers = reinterpret_cast<ImmutableSpatialMemberRecord *>(
		reinterpret_cast<unsigned char *>(malformedMember.arena.data()) +
			memberHeader->memberOffset);
	malformedMembers[0].objectIndex = memberHeader->objectCount;
	ImmutableSpatialExecutionOptions malformedMemberOptions;
	RunStorage malformedMemberStorage(2, 1, 5, 8);
	expect(execute(malformedMember, queries, malformedMemberOptions,
		malformedMemberStorage) == IMMUTABLE_SPATIAL_MALFORMED_ARENA,
		"cheap execution validation rejects an encountered malformed member");
	expect(outputStillSentinel(malformedMemberStorage),
		"encountered malformed member publishes nothing");

	queries[0].expectedArenaGeneration.facts++;
	ImmutableSpatialExecutionOptions options;
	RunStorage stale(2, 1, 5, 8);
	expect(execute(fixture, queries, options, stale) ==
		IMMUTABLE_SPATIAL_STALE_GENERATION,
		"query snapshot generation mismatch is rejected");
	expect(outputStillSentinel(stale),
		"stale query has no partial publication");
	queries[0] = baseQuery(fixture);

	GenerationContext changedArena;
	changedArena.arenaGeneration = fixture.arenaGeneration;
	changedArena.failArenaCall = 2;
	options.resolveArenaGeneration = resolveArena;
	options.resolveObjectGeneration = resolveObject;
	options.generationContext = &changedArena;
	RunStorage changed(2, 1, 5, 8);
	expect(execute(fixture, queries, options, changed) ==
		IMMUTABLE_SPATIAL_GENERATION_MISMATCH,
		"arena generation change during workers is rejected before publication");
	expect(outputStillSentinel(changed),
		"changed arena generation publishes nothing");

	GenerationContext staleObject;
	staleObject.arenaGeneration = fixture.arenaGeneration;
	staleObject.staleObjectID = 102;
	options.generationContext = &staleObject;
	RunStorage staleObjectStorage(2, 1, 5, 8);
	expect(execute(fixture, queries, options, staleObjectStorage) ==
		IMMUTABLE_SPATIAL_GENERATION_MISMATCH,
		"object lifecycle/topology/fact generation failure rejects batch");
	expect(outputStillSentinel(staleObjectStorage),
		"object generation rejection publishes nothing");

	Fixture malformed = fixture;
	auto *header = reinterpret_cast<ImmutableSpatialArenaHeader *>(
		malformed.arena.data());
	header->arenaBytes--;
	options.resolveArenaGeneration = nullptr;
	options.resolveObjectGeneration = nullptr;
	options.generationContext = nullptr;
	RunStorage malformedStorage(2, 1, 5, 8);
	expect(execute(malformed, queries, options, malformedStorage) ==
		IMMUTABLE_SPATIAL_MALFORMED_ARENA,
		"malformed relocated offsets fail closed");
	expect(outputStillSentinel(malformedStorage),
		"malformed arena publishes nothing");
}
}

int main()
{
	testArenaBuildRelocationAndMalformedInput();
	testLegacyTraversalDuplicateNullSelfAndOrders();
	testStrictDistanceBoundariesAndDistanceKinds();
	testNearBoundaryFloatingPointDifferential();
	testEdgeAndOutOfMapTraversal();
	testRelocatedExecutionAndSpanValidation();
	testWorkerCountsAndMoreThanSixtyFourRanges();
	testLargeCanonicalTopologyAndCheapBatchValidation();
	testAliasingIsRejectedBeforeWork();
	testWin32SizeOverflowGuards();
	testFailureCancellationCapacityAndNoPublication();
	testGenerationAndMalformedArenaFailClosed();

	if (g_failures != 0)
	{
		std::cerr << g_failures << " immutable spatial query test(s) failed\n";
		return 1;
	}
	std::cout << "Immutable spatial query tests passed\n";
	return 0;
}
