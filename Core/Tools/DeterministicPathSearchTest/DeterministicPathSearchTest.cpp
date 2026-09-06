#include "Lib/DeterministicPathBatch.h"
#include "Lib/BoundedFreeCounter.h"
#include "Lib/JobSystem.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#include "../TestSupport/NativeKernelSourceConsumerTest.h"
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#if defined(NDEBUG)
#error DeterministicPathSearchTest must run with assertions enabled
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

extern "C" void rts_direct_path_set_test_pause_mask(unsigned pauseMask);
extern "C" bool rts_direct_path_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds);
extern "C" bool rts_direct_path_wait_for_test_pause_count(
	unsigned pausePoint, unsigned requiredCount, unsigned timeoutMilliseconds);
extern "C" void rts_direct_path_release_test_pause(unsigned pausePoint);
extern "C" void rts_direct_path_set_test_fault_mask(unsigned faultMask);
#if defined(_WIN64)
extern "C" void rts_job_system_set_test_pause_mask(unsigned pauseMask);
extern "C" bool rts_job_system_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds);
extern "C" void rts_job_system_release_test_pause(unsigned pausePoint);
#endif

namespace
{

constexpr std::uint8_t VALID_FACT_FLAGS =
	rts::DIRECT_PATH_FACT_CLEAR_GROUND |
	rts::DIRECT_PATH_FACT_HIERARCHY_PASSABLE |
	rts::DIRECT_PATH_FACT_INSIDE_LOGICAL_EXTENT |
	rts::DIRECT_PATH_FACT_FOOTPRINT_CLEAR |
	rts::DIRECT_PATH_FACT_NO_FOREIGN_OCCUPANCY |
	rts::DIRECT_PATH_FACT_NO_LAYER_CONNECTION |
	rts::DIRECT_PATH_FACT_NOT_PINCHED |
	rts::DIRECT_PATH_FACT_METADATA_CLEAN;

rts::DirectPathCellFact MakeFact(std::int32_t x, std::int32_t y)
{
	rts::DirectPathCellFact fact = {};
	fact.x = x;
	fact.y = y;
	fact.zone = 7;
	fact.flags = VALID_FACT_FLAGS;
	return fact;
}

struct DirectFixture
{
	DirectFixture(std::int32_t startX, std::int32_t startY,
		std::int32_t goalX, std::int32_t goalY)
	{
		std::array<rts::DeterministicPathPoint,
			rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS> points = {};
		std::size_t count = 0;
		assert(rts::BuildLegacySupercoverCallbacks(startX, startY, goalX, goalY,
			points.data(), points.size(), count));
		callbacks.reserve(count);
		for (std::size_t i = 0; i < count; ++i)
			callbacks.push_back(MakeFact(points[i].x, points[i].y));
		static constexpr std::int32_t deltaX[8] = {1, 0, -1, 0, 1, -1, -1, 1};
		static constexpr std::int32_t deltaY[8] = {0, 1, 0, -1, 1, 1, -1, -1};
		for (std::size_t i = 0; i < neighbors.size(); ++i)
			neighbors[i] = MakeFact(startX + deltaX[i], startY + deltaY[i]);
		snapshot = {};
		snapshot.callbacks = callbacks.data();
		snapshot.callbackCount = callbacks.size();
		snapshot.startNeighbors = neighbors.data();
		snapshot.startNeighborCount = neighbors.size();
		snapshot.topologyOccupancyGeneration = 41;
		snapshot.requestToken = 73;
		snapshot.objectId = 99;
		snapshot.availableCellInfoCount = 4096;
		snapshot.startX = startX;
		snapshot.startY = startY;
		snapshot.goalX = goalX;
		snapshot.goalY = goalY;
		snapshot.requiredZone = 7;
		snapshot.expectedLayer = rts::DETERMINISTIC_PATH_LAYER_GROUND;
	}

	std::vector<rts::DirectPathCellFact> callbacks;
	std::array<rts::DirectPathCellFact, 8> neighbors;
	rts::DirectPathSnapshot snapshot;
};

rts::DirectPathSearchResult Search(DirectFixture &fixture,
	std::vector<rts::DeterministicPathPoint> &raw)
{
	raw.assign(fixture.callbacks.size(), {});
	rts::DirectPathSearchResult result = {};
	result.rawPoints = raw.data();
	result.rawPointCapacity = raw.size();
	rts::FindDeterministicDirectPath(fixture.snapshot, result);
	return result;
}

void TestBoundedFreeCounterInvariants()
{
	unsigned freeCount = 2;
	assert(rts::IsBoundedFreeCountValid(2, freeCount));
	assert(rts::TryConsumeBoundedFreeCount(2, freeCount) && freeCount == 1);
	assert(rts::TryConsumeBoundedFreeCount(2, freeCount) && freeCount == 0);
	assert(!rts::TryConsumeBoundedFreeCount(2, freeCount) && freeCount == 0);
	assert(rts::TryRestoreBoundedFreeCount(2, freeCount) && freeCount == 1);
	assert(rts::TryRestoreBoundedFreeCount(2, freeCount) && freeCount == 2);
	assert(!rts::TryRestoreBoundedFreeCount(2, freeCount) && freeCount == 2);
	assert(!rts::IsBoundedFreeCountValid(1, freeCount));

	freeCount = ~0u;
	assert(rts::IsBoundedFreeCountValid(~0u, freeCount));
	assert(!rts::TryRestoreBoundedFreeCount(~0u, freeCount));
	assert(freeCount == ~0u);
	assert(rts::TryConsumeBoundedFreeCount(~0u, freeCount));
	assert(freeCount == ~0u - 1u);
	freeCount = 0;
	assert(!rts::TryConsumeBoundedFreeCount(~0u, freeCount));
	assert(freeCount == 0);
}

void TestConcurrentMultiWorkerAuthorityCorrelation()
{
	rts::DeterministicDirectPathBatchExecutionSnapshot execution = {};
	execution.distinctPhysicalWorkerCount = 2;
	execution.peakActiveWorkers = 1;
	assert(!rts::IsDeterministicDirectPathConcurrentMultiWorkerBatch(execution));
	execution.peakActiveWorkers = 2;
	assert(rts::IsDeterministicDirectPathConcurrentMultiWorkerBatch(execution));
	execution.distinctPhysicalWorkerCount = 1;
	assert(!rts::IsDeterministicDirectPathConcurrentMultiWorkerBatch(execution));

	rts::DeterministicOrdinaryPathBatchExecutionSnapshot ordinary = {};
	ordinary.physicalWorkerMask = ~std::uint64_t(0);
	ordinary.distinctPhysicalWorkerCount = 65;
	ordinary.physicalWorkerMaskComplete = false;
	ordinary.peakActiveWorkers = 2;
	assert(rts::IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(ordinary));
	ordinary.distinctPhysicalWorkerCount = 1;
	assert(!rts::IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(ordinary));
}

void AssertCallbacks(std::int32_t startX, std::int32_t startY,
	std::int32_t goalX, std::int32_t goalY,
	std::initializer_list<std::array<std::int32_t, 2>> expected)
{
	std::array<rts::DeterministicPathPoint,
		rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS> points = {};
	std::size_t count = 0;
	assert(rts::BuildLegacySupercoverCallbacks(startX, startY, goalX, goalY,
		points.data(), points.size(), count));
	assert(count == expected.size());
	std::size_t index = 0;
	for (const auto &point : expected)
	{
		assert(points[index].x == point[0]);
		assert(points[index].y == point[1]);
		assert(points[index].layer == rts::DETERMINISTIC_PATH_LAYER_GROUND);
		++index;
	}
}

void TestExactSupercoverDirectionsAndPostGoal()
{
	AssertCallbacks(1, 2, 5, 2,
		{{1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}});
	AssertCallbacks(5, 2, 1, 2,
		{{5, 2}, {4, 2}, {3, 2}, {2, 2}, {1, 2}});
	AssertCallbacks(3, 1, 3, 5,
		{{3, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}});
	AssertCallbacks(3, 5, 3, 1,
		{{3, 5}, {3, 4}, {3, 3}, {3, 2}, {3, 1}});
	AssertCallbacks(1, 1, 5, 3,
		{{1, 1}, {1, 2}, {2, 2}, {3, 2}, {3, 3}, {4, 3}, {5, 3}, {5, 4}});
	AssertCallbacks(1, 1, 3, 5,
		{{1, 1}, {2, 1}, {2, 2}, {2, 3}, {3, 3}, {3, 4}, {3, 5}, {4, 5}});
	AssertCallbacks(5, 1, 1, 3,
		{{5, 1}, {5, 2}, {4, 2}, {3, 2}, {3, 3}, {2, 3}, {1, 3}, {1, 4}});
	AssertCallbacks(5, 1, 3, 5,
		{{5, 1}, {4, 1}, {4, 2}, {4, 3}, {3, 3}, {3, 4}, {3, 5}, {2, 5}});
	AssertCallbacks(1, 5, 5, 3,
		{{1, 5}, {1, 4}, {2, 4}, {3, 4}, {3, 3}, {4, 3}, {5, 3}, {5, 2}});
	AssertCallbacks(1, 5, 3, 1,
		{{1, 5}, {2, 5}, {2, 4}, {2, 3}, {3, 3}, {3, 2}, {3, 1}, {4, 1}});
	AssertCallbacks(5, 5, 1, 3,
		{{5, 5}, {5, 4}, {4, 4}, {3, 4}, {3, 3}, {2, 3}, {1, 3}, {1, 2}});
	AssertCallbacks(5, 5, 3, 1,
		{{5, 5}, {4, 5}, {4, 4}, {4, 3}, {3, 3}, {3, 2}, {3, 1}, {2, 1}});
	AssertCallbacks(1, 1, 5, 5,
		{{1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}, {3, 4},
		 {4, 4}, {4, 5}, {5, 5}, {5, 6}});
	AssertCallbacks(3, 3, 3, 3, {{3, 3}, {3, 4}});
}

std::vector<std::array<std::int32_t, 2>> BuildLegacyReferenceCallbacks(
	std::int32_t startX, std::int32_t startY,
	std::int32_t goalX, std::int32_t goalY)
{
	const std::int32_t deltaX = goalX >= startX ? goalX - startX : startX - goalX;
	const std::int32_t deltaY = goalY >= startY ? goalY - startY : startY - goalY;
	std::int32_t x = startX;
	std::int32_t y = startY;
	std::int32_t xinc1 = goalX >= startX ? 1 : -1;
	std::int32_t xinc2 = xinc1;
	std::int32_t yinc1 = goalY >= startY ? 1 : -1;
	std::int32_t yinc2 = yinc1;
	std::int32_t denominator = 0;
	std::int32_t numerator = 0;
	std::int32_t numeratorAdd = 0;
	std::int32_t pixelCount = 0;
	if (deltaX >= deltaY)
	{
		xinc1 = 0;
		yinc2 = 0;
		denominator = deltaX;
		numerator = deltaX / 2;
		numeratorAdd = deltaY;
		pixelCount = deltaX;
	}
	else
	{
		xinc2 = 0;
		yinc1 = 0;
		denominator = deltaY;
		numerator = deltaY / 2;
		numeratorAdd = deltaX;
		pixelCount = deltaY;
	}
	std::vector<std::array<std::int32_t, 2>> callbacks;
	for (std::int32_t pixel = 0; pixel <= pixelCount; ++pixel)
	{
		callbacks.push_back({x, y});
		numerator += numeratorAdd;
		if (numerator >= denominator)
		{
			numerator -= denominator;
			x += xinc1;
			y += yinc1;
			callbacks.push_back({x, y});
		}
		x += xinc2;
		y += yinc2;
	}
	return callbacks;
}

void TestLegacyIteratorExhaustiveParity()
{
	for (std::int32_t startX = -3; startX <= 3; ++startX)
	{
		for (std::int32_t startY = -3; startY <= 3; ++startY)
		{
			for (std::int32_t goalX = -3; goalX <= 3; ++goalX)
			{
				for (std::int32_t goalY = -3; goalY <= 3; ++goalY)
				{
					const auto reference = BuildLegacyReferenceCallbacks(
						startX, startY, goalX, goalY);
					std::array<rts::DeterministicPathPoint,
						rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS> actual = {};
					std::size_t actualCount = 0;
					assert(rts::BuildLegacySupercoverCallbacks(startX, startY,
						goalX, goalY, actual.data(), actual.size(), actualCount));
					assert(actualCount == reference.size());
					for (std::size_t i = 0; i < actualCount; ++i)
					{
						assert(actual[i].x == reference[i][0]);
						assert(actual[i].y == reference[i][1]);
					}
				}
			}
		}
	}
}

void TestExactRawChainAndPoolAccounting()
{
	DirectFixture fixture(4, 4, 8, 6);
	std::vector<rts::DeterministicPathPoint> raw;
	const rts::DirectPathSearchResult result = Search(fixture, raw);
	assert(result.status == rts::DIRECT_PATH_FOUND);
	assert(result.callbackCount == 8);
	assert(result.rawPointCount == 7);
	assert(raw.front().x == 4 && raw.front().y == 4);
	assert(raw[6].x == 8 && raw[6].y == 6);
	for (std::size_t i = 0; i < result.rawPointCount; ++i)
	{
		assert(raw[i].x == fixture.callbacks[i].x);
		assert(raw[i].y == fixture.callbacks[i].y);
	}
	assert(fixture.callbacks.back().x == 8 && fixture.callbacks.back().y == 7);
	assert(result.requiredCellInfoCount == 14);
	assert(result.startNeighborAllocationCount == 6);
	assert(result.openCellCountAfterGoal == 12);
	assert(result.cumulativeCellCount == 13);
	assert(rts::IsDirectPathMaterializationPlanValid(fixture.snapshot, result,
		4096));

	fixture.callbacks.front().hasPathfindInfo = 1;
	fixture.callbacks[6].hasPathfindInfo = 1;
	const rts::DirectPathSearchResult existing = Search(fixture, raw);
	assert(existing.status == rts::DIRECT_PATH_FOUND);
	assert(existing.requiredCellInfoCount == 12);
	assert(existing.openCellCountAfterGoal == 12);
	assert(existing.cumulativeCellCount == 13);

	DirectFixture reusable(4, 4, 8, 6);
	reusable.callbacks[3].hasPathfindInfo = 1;
	reusable.neighbors[0].hasPathfindInfo = 1;
	const rts::DirectPathSearchResult reused = Search(reusable, raw);
	assert(reused.status == rts::DIRECT_PATH_FOUND);
	assert(reused.requiredCellInfoCount == 12);
	assert(reused.startNeighborAllocationCount == 5);
	assert(reused.openCellCountAfterGoal == 12);
	assert(reused.cumulativeCellCount == 13);
}

void TestMaxLengthCallbackUniquenessAndAccountingParity()
{
	DirectFixture fixture(0, 0,
		static_cast<std::int32_t>(
			rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS - 1), 0);
	assert(fixture.callbacks.size() ==
		rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS);
	for (std::size_t i = 0; i < fixture.callbacks.size(); ++i)
	{
		assert(fixture.callbacks[i].x == static_cast<std::int32_t>(i));
		assert(fixture.callbacks[i].y == 0);
		if (i != 0)
		{
			assert(fixture.callbacks[i].x != fixture.callbacks[i - 1].x ||
				fixture.callbacks[i].y != fixture.callbacks[i - 1].y);
		}
	}

	std::vector<rts::DeterministicPathPoint> raw;
	const rts::DirectPathSearchResult result = Search(fixture, raw);
	assert(result.status == rts::DIRECT_PATH_FOUND);
	assert(result.callbackCount == fixture.callbacks.size());
	assert(result.rawPointCount == fixture.callbacks.size());
	assert(result.startNeighborAllocationCount == 7);
	assert(result.requiredCellInfoCount ==
		rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS + 7);
	assert(result.openCellCountAfterGoal ==
		rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS + 5);
	assert(result.cumulativeCellCount ==
		rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS + 6);
	assert(rts::IsDirectPathMaterializationPlanValid(fixture.snapshot, result,
		fixture.snapshot.availableCellInfoCount));
}

void TestSameCellAccounting()
{
	DirectFixture fixture(7, 7, 7, 7);
	std::vector<rts::DeterministicPathPoint> raw;
	rts::DirectPathSearchResult result = Search(fixture, raw);
	assert(result.status == rts::DIRECT_PATH_FOUND);
	assert(result.rawPointCount == 1);
	assert(result.requiredCellInfoCount == 1);
	assert(result.openCellCountAfterGoal == 0);
	assert(result.cumulativeCellCount == 0);
	fixture.callbacks[0].hasPathfindInfo = 1;
	result = Search(fixture, raw);
	assert(result.requiredCellInfoCount == 0);
}

void TestPoolShortageFallsBackBeforeMutation()
{
	DirectFixture fixture(4, 4, 8, 6);
	std::vector<rts::DeterministicPathPoint> raw;
	fixture.snapshot.availableCellInfoCount = 13;
	const rts::DirectPathSearchResult result = Search(fixture, raw);
	assert(result.status == rts::DIRECT_PATH_CELL_INFO_SHORTAGE);
	assert(result.requiredCellInfoCount == 14);
	assert(result.rawPointCount == 7);
	assert(!rts::IsDirectPathMaterializationPlanValid(fixture.snapshot, result,
		4096));
}

void TestAdvisoryFallbackStatusClassification()
{
	assert(rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_UNSUPPORTED_SUBSET));
	assert(rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_NO_PATH));
	assert(rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_CELL_INFO_SHORTAGE));
	assert(!rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_INVALID_INPUT));
	assert(!rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_MALFORMED_SNAPSHOT));
	assert(!rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_OUTPUT_TOO_SMALL));
	assert(!rts::IsDirectPathAdvisoryFallbackStatus(
		rts::DIRECT_PATH_FOUND));
}

void TestBridgeElsewhereAndConnectedCorridor()
{
	// A compact snapshot intentionally contains no global bridge inventory: a
	// bridge elsewhere cannot reject this all-ground corridor.
	DirectFixture fixture(2, 2, 6, 2);
	std::vector<rts::DeterministicPathPoint> raw;
	assert(Search(fixture, raw).status == rts::DIRECT_PATH_FOUND);
	fixture.callbacks[2].flags &= ~rts::DIRECT_PATH_FACT_NO_LAYER_CONNECTION;
	assert(Search(fixture, raw).status == rts::DIRECT_PATH_UNSUPPORTED_SUBSET);
}

void TestMalformedAndGenerationStaleResults()
{
	DirectFixture fixture(2, 2, 6, 3);
	std::vector<rts::DeterministicPathPoint> raw;
	rts::DirectPathSearchResult result = Search(fixture, raw);
	assert(result.status == rts::DIRECT_PATH_FOUND);
	assert(rts::IsDirectPathResultCurrent(result, 41, 73, 99));
	assert(!rts::IsDirectPathResultCurrent(result, 42, 73, 99));
	assert(!rts::IsDirectPathResultCurrent(result, 41, 74, 99));
	assert(!rts::IsDirectPathResultCurrent(result, 41, 73, 100));
	fixture.callbacks[1].x += 1;
	result = Search(fixture, raw);
	assert(result.status == rts::DIRECT_PATH_MALFORMED_SNAPSHOT);
}

struct LegacyMutationProbe
{
	std::size_t freeCellInfos;
	std::size_t openCells;
	std::size_t closedCells;
	std::size_t cumulativeCells;
	std::uint32_t debugPathToken;
	std::uint32_t fifoBudget;
};

bool ProbeMaterializationBoundary(const rts::DirectPathSnapshot &snapshot,
	const rts::DirectPathSearchResult &result, LegacyMutationProbe &state)
{
	if (!rts::IsDirectPathMaterializationPlanValid(snapshot, result,
		state.freeCellInfos))
	{
		return false;
	}
	state.freeCellInfos -= result.requiredCellInfoCount;
	state.openCells = result.openCellCountAfterGoal;
	state.closedCells = 1;
	state.cumulativeCells += result.cumulativeCellCount;
	++state.debugPathToken;
	state.fifoBudget -= static_cast<std::uint32_t>(result.cumulativeCellCount);
	return true;
}

void AssertProbeUnchanged(const LegacyMutationProbe &expected,
	const LegacyMutationProbe &actual)
{
	assert(std::memcmp(&expected, &actual, sizeof(expected)) == 0);
}

void TestMismatchFallbacksArePreMutation()
{
	DirectFixture fixture(4, 4, 8, 6);
	std::vector<rts::DeterministicPathPoint> raw;
	const rts::DirectPathSearchResult valid = Search(fixture, raw);
	assert(rts::IsDirectPathMaterializationPlanValid(fixture.snapshot, valid,
		4096));
	const LegacyMutationProbe baseline = {4096, 0, 0, 37, 11, 5000};

	auto assertRejectedWithoutMutation = [&](const rts::DirectPathSearchResult &candidate,
		std::size_t available)
	{
		LegacyMutationProbe state = baseline;
		state.freeCellInfos = available;
		const LegacyMutationProbe expected = state;
		assert(!ProbeMaterializationBoundary(fixture.snapshot, candidate, state));
		AssertProbeUnchanged(expected, state);
	};

	rts::DirectPathSearchResult mismatch = valid;
	++mismatch.startNeighborAllocationCount;
	assertRejectedWithoutMutation(mismatch, 4096);
	mismatch = valid;
	++mismatch.cumulativeCellCount;
	assertRejectedWithoutMutation(mismatch, 4096);
	mismatch = valid;
	++mismatch.openCellCountAfterGoal;
	assertRejectedWithoutMutation(mismatch, 4096);
	mismatch = valid;
	++mismatch.requiredCellInfoCount;
	assertRejectedWithoutMutation(mismatch, 4096);
	mismatch = valid;
	++mismatch.topologyOccupancyGeneration;
	assertRejectedWithoutMutation(mismatch, 4096);
	mismatch = valid;
	++mismatch.objectId;
	assertRejectedWithoutMutation(mismatch, 4096);
	mismatch = valid;
	mismatch.status = rts::DIRECT_PATH_NO_PATH;
	assertRejectedWithoutMutation(mismatch, 4096);
	const std::int32_t savedGoalX = raw[valid.rawPointCount - 1].x;
	++raw[valid.rawPointCount - 1].x;
	assertRejectedWithoutMutation(valid, 4096);
	raw[valid.rawPointCount - 1].x = savedGoalX;
	assertRejectedWithoutMutation(valid, valid.requiredCellInfoCount - 1);
}

void TestGeneralsRecordedPathAuthorityPolicy()
{
	// The fixed semantic epoch does not depend on scheduler topology. Recording
	// needs the paired path/AI epoch; ordinary playback remains owner-thread only.
	for (unsigned mode = 0; mode < 3; ++mode)
	{
		for (unsigned session = 0; session < 3; ++session)
		{
			for (unsigned marked = 0; marked < 2; ++marked)
			{
				rts::FixedPathfindingSemanticsPolicy fixed = {};
				fixed.nativeRuntime = true;
				fixed.runtimeUsesCurrentGeneralsEpoch = true;
				fixed.recordingGame = session == 1;
				fixed.replayGame = session == 2;
				fixed.replayUsesCurrentPathEpoch = marked != 0;
				rts::DirectPathAuthorityPolicy direct = {};
				direct.parallelExecutionMode = mode == 2;
				direct.shadowExecutionMode = mode == 1;
				direct.runtimeUsesCurrentGeneralsEpoch = true;
				direct.recordingGame = fixed.recordingGame;
				direct.replayGame = fixed.replayGame;
				direct.replayUsesCurrentPathEpoch = fixed.replayUsesCurrentPathEpoch;
				// Unrecorded native sessions also cover same-build save restoration.
				const bool expectedFixed = session == 0 || marked != 0;
				const bool expectedDirect = expectedFixed && mode == 2;
				assert(rts::IsFixedPathfindingSemanticsAllowed(fixed) == expectedFixed);
				assert(rts::IsDirectPathAuthorityAllowed(direct) == expectedDirect);
				assert(rts::IsOrdinaryPathAuthorityAllowed(direct, expectedFixed) ==
					(expectedDirect && session != 2));
				assert(!rts::IsOrdinaryPathAuthorityAllowed(direct, false));
				fixed.nativeRuntime = false;
				assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
				fixed.nativeRuntime = true;
				fixed.runtimeUsesCurrentGeneralsEpoch = false;
				direct.runtimeUsesCurrentGeneralsEpoch = false;
				assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
				assert(!rts::IsDirectPathAuthorityAllowed(direct));
			}
		}
	}

	// Keep the pre-existing negotiated direct-network lane separate from the
	// new local recording epoch; it never authorizes ordinary worker paths.
	rts::DirectPathAuthorityPolicy network = {};
	network.parallelExecutionMode = true;
	network.runtimeUsesCurrentGeneralsEpoch = true;
	network.networkGame = true;
	network.multiplayerGame = true;
	assert(!rts::IsDirectPathAuthorityAllowed(network));
	network.multiplayerPolicyEnabled = true;
	assert(rts::IsDirectPathAuthorityAllowed(network));
	assert(!rts::IsOrdinaryPathAuthorityAllowed(network, true));
	network.networkGame = false;
	assert(!rts::IsDirectPathAuthorityAllowed(network));
	network.networkGame = true;
	for (unsigned session = 1; session < 3; ++session)
	{
		network.recordingGame = session == 1;
		network.replayGame = session == 2;
		network.replayUsesCurrentPathEpoch = false;
		assert(!rts::IsDirectPathAuthorityAllowed(network));
		network.replayUsesCurrentPathEpoch = true;
		assert(!rts::IsDirectPathAuthorityAllowed(network));
	}
}

void TestAuthorityPolicyAndRawStartAdmission()
{
	rts::DirectPathAuthorityPolicy policy = {};
	policy.parallelExecutionMode = true;
	policy.zeroHourTitle = true;
	assert(rts::IsDirectPathAuthorityAllowed(policy));
	policy.shadowExecutionMode = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.shadowExecutionMode = false;
	policy.parallelExecutionMode = false;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.parallelExecutionMode = true;
	policy.networkGame = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.multiplayerPolicyEnabled = true;
	assert(rts::IsDirectPathAuthorityAllowed(policy));
	policy.multiplayerPolicyEnabled = false;
	policy.networkGame = false;
	policy.multiplayerGame = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.multiplayerPolicyEnabled = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.multiplayerPolicyEnabled = false;
	policy.multiplayerGame = false;
	policy.replayGame = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.replayUsesCurrentPathEpoch = true;
	assert(rts::IsDirectPathAuthorityAllowed(policy));
	assert(!rts::IsOrdinaryPathAuthorityAllowed(policy, true));
	policy.replayGame = false;
	policy.replayUsesCurrentPathEpoch = false;
	assert(rts::IsOrdinaryPathAuthorityAllowed(policy, true));
	assert(!rts::IsOrdinaryPathAuthorityAllowed(policy, false));
	policy.networkGame = true;
	policy.multiplayerPolicyEnabled = true;
	assert(rts::IsDirectPathAuthorityAllowed(policy));
	assert(!rts::IsOrdinaryPathAuthorityAllowed(policy, true));
	policy.networkGame = false;
	policy.multiplayerPolicyEnabled = false;

	policy = {};
	policy.parallelExecutionMode = true;
	policy.runtimeUsesCurrentGeneralsEpoch = true;
	assert(rts::IsDirectPathAuthorityAllowed(policy));
	policy.recordingGame = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.recordingGame = false;
	policy.replayGame = true;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));
	policy.replayGame = false;
	policy.runtimeUsesCurrentGeneralsEpoch = false;
	assert(!rts::IsDirectPathAuthorityAllowed(policy));

	rts::FixedPathfindingSemanticsPolicy fixed = {};
	fixed.nativeRuntime = true;
	fixed.zeroHourTitle = true;
	assert(rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.networkGame = true;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.networkGame = false;
	fixed.multiplayerGame = true;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.multiplayerGame = false;
	fixed.replayGame = true;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.replayUsesCurrentPathEpoch = true;
	assert(rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.nativeRuntime = false;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));

	fixed = {};
	fixed.nativeRuntime = true;
	fixed.runtimeUsesCurrentGeneralsEpoch = true;
	assert(rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.recordingGame = true;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.recordingGame = false;
	fixed.replayGame = true;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));
	fixed.replayGame = false;
	fixed.runtimeUsesCurrentGeneralsEpoch = false;
	assert(!rts::IsFixedPathfindingSemanticsAllowed(fixed));

	assert(!rts::ShouldCaptureOrdinaryNavigationSnapshot(0, false, true, 4));
	assert(!rts::ShouldCaptureOrdinaryNavigationSnapshot(1, false, true, 4));
	assert(!rts::ShouldCaptureOrdinaryNavigationSnapshot(2, false, true, 1));
	assert(!rts::ShouldCaptureOrdinaryNavigationSnapshot(2, false, false, 4));
	assert(rts::ShouldCaptureOrdinaryNavigationSnapshot(2, false, true, 2));
	assert(!rts::ShouldCaptureOrdinaryNavigationSnapshot(0, true, false, 1));
	assert(!rts::ShouldCaptureOrdinaryNavigationSnapshot(1, true, false, 0));
	assert(rts::ShouldCaptureOrdinaryNavigationSnapshot(1, true, false, 1));

	assert(rts::IsDirectPathRawStartEligible(1.0f, 2.0f, 3.0f,
		1.0f, 2.0f, 3.0f, true));
	assert(!rts::IsDirectPathRawStartEligible(1.0f, 2.0f, 3.0f,
		1.5f, 2.0f, 3.0f, true));
	assert(!rts::IsDirectPathRawStartEligible(1.0f, 2.0f, 3.0f,
		1.0f, 2.0f, 3.0f, false));
}

class BlockingJob final : public rts::Job
{
public:
	BlockingJob(std::atomic<unsigned> &entered, std::atomic<bool> &release) :
		m_entered(entered), m_release(release) {}

	void execute(rts::JobContext &) override
	{
		m_entered.fetch_add(1, std::memory_order_release);
		while (!m_release.load(std::memory_order_acquire))
			std::this_thread::yield();
	}

private:
	std::atomic<unsigned> &m_entered;
	std::atomic<bool> &m_release;
};

void AssertBatchResultMatchesSerial(DirectFixture &fixture,
	const rts::DeterministicDirectPathBatch &batch, std::size_t requestIndex)
{
	std::vector<rts::DeterministicPathPoint> serialRaw;
	const rts::DirectPathSearchResult serial = Search(fixture, serialRaw);
	const rts::DirectPathSearchResult &actual = batch.result(requestIndex);
	assert(actual.status == serial.status);
	assert(actual.rawPointCount == serial.rawPointCount);
	assert(actual.callbackCount == serial.callbackCount);
	assert(actual.requiredCellInfoCount == serial.requiredCellInfoCount);
	assert(actual.startNeighborAllocationCount ==
		serial.startNeighborAllocationCount);
	assert(actual.openCellCountAfterGoal == serial.openCellCountAfterGoal);
	assert(actual.cumulativeCellCount == serial.cumulativeCellCount);
	assert(actual.topologyOccupancyGeneration ==
		serial.topologyOccupancyGeneration);
	assert(actual.requestToken == serial.requestToken);
	assert(actual.objectId == serial.objectId);
	for (std::size_t i = 0; i < actual.rawPointCount; ++i)
	{
		assert(actual.rawPoints[i].x == serialRaw[i].x);
		assert(actual.rawPoints[i].y == serialRaw[i].y);
		assert(actual.rawPoints[i].layer == serialRaw[i].layer);
	}
}

void TestPhysicalWorkerCountsAndOneWorkerParity()
{
	static constexpr unsigned workerCounts[] = {1, 2, 4};
	rts::JobSystem &jobs = rts::JobSystem::instance();
	for (const unsigned workerCount : workerCounts)
	{
		jobs.shutdown();
		rts::JobSystemConfig config;
		config.workerCount = workerCount;
		config.queueCapacity = 32;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		assert(jobs.start(config));
		assert(jobs.workerCount() == workerCount);
		assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
		DirectFixture first(3, 4, 11, 7);
		DirectFixture second(17, 9, 10, 14);
		second.snapshot.requestToken = 74;
		second.snapshot.objectId = 100;
		const std::array<rts::DirectPathSnapshot, 2> snapshots =
			{first.snapshot, second.snapshot};
		const rts::JobMetricCounter ownerHelpBefore =
			jobs.metrics().ownerHelpCount;
		rts::DeterministicDirectPathBatch batch;
		assert(batch.executeSynchronously(jobs, snapshots.data(),
			snapshots.size(), 1000));
		const rts::DeterministicDirectPathBatchExecutionSnapshot execution =
			batch.executionSnapshot();
		assert(execution.completed && !execution.timedOut);
		assert(execution.requestCount == snapshots.size());
		assert(execution.submittedJobCount == snapshots.size());
		assert(execution.workerExecutedJobCount == snapshots.size());
		assert(execution.ownerExecutedJobCount == 0);
		assert(execution.failedJobCount == 0);
		assert(execution.distinctPhysicalWorkerCount >= 1);
		assert(execution.distinctPhysicalWorkerCount <= workerCount);
		assert(execution.peakActiveWorkers >= 1);
		assert(jobs.metrics().ownerHelpCount == ownerHelpBefore);
		for (std::size_t i = 0; i < snapshots.size(); ++i)
		{
			const rts::DeterministicDirectPathExecutionSnapshot request =
				batch.requestExecutionSnapshot(i);
			assert(request.submitted && request.succeeded);
			assert(request.state == rts::DIRECT_PATH_EXECUTION_WORKER);
		}
		AssertBatchResultMatchesSerial(first, batch, 0);
		AssertBatchResultMatchesSerial(second, batch, 1);
		assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
		jobs.shutdown();
	}
}

#if defined(_WIN64)
struct PathPerformanceTestClock
{
	PathPerformanceTestClock() : value(0), reads(0) {}

	static rts::JobMetricCounter read(void *context)
	{
		PathPerformanceTestClock &clock =
			*static_cast<PathPerformanceTestClock *>(context);
		++clock.reads;
		return ++clock.value;
	}

	rts::JobMetricCounter value;
	unsigned reads;
};

void TestDirectPathBatchPerformanceLedgerStages()
{
	using namespace rts::performance;
	PathPerformanceTestClock clock;
	KernelPerformanceLedger &ledger = KernelPerformanceLedger::instance();
	assert(ledger.beginRun(true, PathPerformanceTestClock::read, &clock));
	KernelPerformanceBatch ledgerBatch = ledger.beginBatch(
		KERNEL_PERFORMANCE_PATH, 1, 42, 1);
	assert(ledgerBatch.valid());
	const KernelPerformanceInterval capture = ledger.beginInterval(ledgerBatch,
		KERNEL_PERFORMANCE_CAPTURE);
	assert(capture.valid());
	assert(ledger.endInterval(capture));

	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	DirectFixture first(3, 4, 11, 7);
	DirectFixture second(17, 9, 10, 14);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const std::array<rts::DirectPathSnapshot, 2> snapshots =
		{first.snapshot, second.snapshot};
	rts::DeterministicDirectPathBatch batch;
	assert(batch.executeSynchronously(jobs, snapshots.data(), snapshots.size(),
		1000, &ledgerBatch));
	const KernelPerformanceInterval validate = ledger.beginInterval(ledgerBatch,
		KERNEL_PERFORMANCE_VALIDATE);
	assert(validate.valid());
	assert(ledger.endInterval(validate));
	const KernelPerformanceInterval commit = ledger.beginInterval(ledgerBatch,
		KERNEL_PERFORMANCE_COMMIT);
	assert(commit.valid());
	assert(ledger.endInterval(commit));
	assert(ledger.endBatch(ledgerBatch, KERNEL_PERFORMANCE_COMMITTED));
	const KernelPerformanceSnapshot snapshot = ledger.freeze();
	assert(snapshot.complete);
	assert(snapshot.streamCount == 1);
	const KernelPerformanceStream &stream = snapshot.streams[0];
	assert(stream.kernel == KERNEL_PERFORMANCE_PATH);
	assert(stream.subtype == 1);
	assert(stream.firstFrame == 42 && stream.lastFrame == 42);
	for (unsigned stage = 0; stage < KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		assert(stream.stageSamples[stage] >= 1);
	assert(stream.attemptedBatches == 1);
	assert(stream.admittedBatches == 1);
	assert(stream.committedBatches == 1);
	assert(stream.abortedBatches == 0);
	assert(stream.activePipelineNanoseconds != 0);
	assert(clock.reads != 0);
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
}

struct PathReferenceTestRun
{
	rts::performance::KernelPerformanceSnapshot timing;
	rts::performance::KernelPerformanceReferenceSnapshot reference;
	unsigned referenceClockReads;
};

PathReferenceTestRun RunDirectPathReferenceBatch(
	rts::performance::KernelPerformanceReferenceMode mode, bool committed)
{
	using namespace rts::performance;
	PathPerformanceTestClock timingClock;
	PathPerformanceTestClock referenceClock;
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	KernelPerformanceReferenceLedger reference;
	assert(timing.beginRun(true, PathPerformanceTestClock::read, &timingClock));
	assert(reference.beginRun(mode, PathPerformanceTestClock::read,
		&referenceClock));
	assert(reference.mode() == mode);
	KernelPerformanceBatch timingBatch = timing.beginBatch(
		KERNEL_PERFORMANCE_PATH, 1, 142, 9);
	assert(timingBatch.valid());

	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	DirectFixture first(3, 4, 11, 7);
	DirectFixture second(17, 9, 10, 14);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const std::array<rts::DirectPathSnapshot, 2> snapshots =
		{first.snapshot, second.snapshot};
	rts::DeterministicDirectPathBatch batch;
	rts::performance::KernelPerformanceReferenceBatch referenceBatch;
	assert(batch.executeSynchronously(jobs, snapshots.data(), snapshots.size(),
		1000, &timingBatch, &reference, &referenceBatch));
	const rts::DeterministicDirectPathBatchExecutionSnapshot execution =
		batch.executionSnapshot();
	assert(execution.completed && execution.workerExecutedJobCount == 2);
	// RED until the native path batch emits one reference token from its real
	// post-validation output while the timing VALIDATE scope is still open.
	assert(referenceBatch.valid());

	const KernelPerformanceInterval commit = timing.beginInterval(timingBatch,
		KERNEL_PERFORMANCE_COMMIT);
	assert(commit.valid());
	assert(timing.endInterval(commit));
	assert(reference.finishBatch(referenceBatch, committed));
	assert(timing.endBatch(timingBatch, committed ?
		KERNEL_PERFORMANCE_COMMITTED :
		KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION));
	const KernelPerformanceSnapshot timingSnapshot = timing.freeze();
	const KernelPerformanceReferenceSnapshot referenceSnapshot =
		reference.freeze();
	assert(timingSnapshot.complete);
	assert(referenceSnapshot.complete);
	assert(referenceSnapshot.streamCount == 1);
	const KernelPerformanceReferenceStream &stream =
		referenceSnapshot.streams[0];
	assert(stream.kernel == KERNEL_PERFORMANCE_PATH && stream.subtype == 1);
	assert(stream.firstFrame == 142 && stream.lastFrame == 142);
	assert(stream.validatedBatchCount == 1);
	assert(stream.validatedOperationCount == snapshots.size());
	if (committed)
	{
		assert(stream.committedBatchCount == 1);
		assert(stream.committedOperationCount == snapshots.size());
	}
	else
	{
		assert(stream.abortedBatchCount == 1);
		assert(stream.committedBatchCount == 0);
		assert(stream.committedOperationCount == 0);
	}
	if (mode == KERNEL_REFERENCE_SERIAL_ORACLE)
	{
		assert(committed ? stream.serialSampleCount == 1 :
			stream.serialSampleCount == 0);
		if (committed)
			assert(stream.serialNanoseconds != 0);
	}
	else
	{
		assert(stream.serialSampleCount == 0);
		assert(stream.serialNanoseconds == 0);
		assert(referenceClock.reads == 0);
	}
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
	PathReferenceTestRun result = {};
	result.timing = timingSnapshot;
	result.reference = referenceSnapshot;
	result.referenceClockReads = referenceClock.reads;
	return result;
}

void TestDirectPathReferenceModesAndCommitBoundary()
{
	const PathReferenceTestRun throughput = RunDirectPathReferenceBatch(
		rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING, true);
	const PathReferenceTestRun serial = RunDirectPathReferenceBatch(
		rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE, true);
	assert(throughput.reference.streams[0].inputDigest.equals(
		serial.reference.streams[0].inputDigest));
	assert(throughput.reference.streams[0].outputDigest.equals(
		serial.reference.streams[0].outputDigest));
	assert(throughput.reference.streams[0].commitDigest.equals(
		serial.reference.streams[0].commitDigest));
	const PathReferenceTestRun aborted = RunDirectPathReferenceBatch(
		rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE, false);
	assert(aborted.reference.streams[0].abortedBatchCount == 1);
	assert(aborted.reference.streams[0].serialSampleCount == 0);
}
#endif

void TestBoundedBatchUsesMultiplePhysicalWorkers()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 32;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	DirectFixture first(3, 4, 11, 7);
	DirectFixture second(17, 9, 10, 14);
	DirectFixture third(20, 20, 28, 23);
	DirectFixture fourth(31, 7, 24, 12);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	third.snapshot.requestToken = 75;
	third.snapshot.objectId = 101;
	fourth.snapshot.requestToken = 76;
	fourth.snapshot.objectId = 102;
	const std::array<rts::DirectPathSnapshot, 4> snapshots =
		{first.snapshot, second.snapshot, third.snapshot, fourth.snapshot};
	rts_direct_path_set_test_pause_mask(2);
	std::atomic<bool> bothWorkersReached(false);
	std::thread releaseWorkers([&]() {
		bothWorkersReached.store(rts_direct_path_wait_for_test_pause_count(
			2, 2, 15000), std::memory_order_release);
		rts_direct_path_release_test_pause(2);
	});
	const rts::JobMetricCounter ownerHelpBefore = jobs.metrics().ownerHelpCount;
	rts::DeterministicDirectPathBatch batch;
	assert(batch.executeSynchronously(jobs, snapshots.data(), snapshots.size(),
		1000));
	releaseWorkers.join();
	rts_direct_path_set_test_pause_mask(0);
	assert(bothWorkersReached.load(std::memory_order_acquire));
	const rts::DeterministicDirectPathBatchExecutionSnapshot execution =
		batch.executionSnapshot();
	assert(execution.completed && !execution.timedOut);
	assert(execution.workerExecutedJobCount == snapshots.size());
	assert(execution.ownerExecutedJobCount == 0);
	assert(execution.distinctPhysicalWorkerCount >= 2);
	assert(execution.peakActiveWorkers >= 2);
	assert(jobs.metrics().ownerHelpCount == ownerHelpBefore);
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
}

void TestBatchFailureTimeoutLateDrainAndStoppedFallback()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	DirectFixture first(3, 4, 11, 7);
	DirectFixture second(17, 9, 10, 14);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const std::array<rts::DirectPathSnapshot, 2> snapshots =
		{first.snapshot, second.snapshot};
	const rts::JobMetricCounter ownerHelpBefore = jobs.metrics().ownerHelpCount;

	rts_direct_path_set_test_fault_mask(1);
	{
		rts::DeterministicDirectPathBatch faultedBatch;
		assert(!faultedBatch.executeSynchronously(jobs, snapshots.data(),
			snapshots.size(), 500));
		const rts::DeterministicDirectPathBatchExecutionSnapshot faulted =
			faultedBatch.executionSnapshot();
		assert(!faulted.completed && !faulted.timedOut);
		assert(faulted.submittedJobCount == snapshots.size());
		assert(faulted.failedJobCount == snapshots.size());
		for (std::size_t i = 0; i < snapshots.size(); ++i)
		{
			const rts::DeterministicDirectPathExecutionSnapshot request =
				faultedBatch.requestExecutionSnapshot(i);
			assert(request.submitted && !request.succeeded);
			assert(request.state == rts::DIRECT_PATH_EXECUTION_FAILURE);
		}
	}
	rts_direct_path_set_test_fault_mask(0);

	// A scheduler stop observed after physical workers finish but before the
	// owner join is still non-authoritative.  The test-only bit forces that exact
	// terminal race without asking a non-owner thread to shut down JobSystem.
	rts_direct_path_set_test_fault_mask(2);
	{
		rts::DeterministicDirectPathBatch stoppedAtJoinBatch;
		assert(!stoppedAtJoinBatch.executeSynchronously(jobs, snapshots.data(),
			snapshots.size(), 500));
		const rts::DeterministicDirectPathBatchExecutionSnapshot stoppedAtJoin =
			stoppedAtJoinBatch.executionSnapshot();
		assert(!stoppedAtJoin.completed && stoppedAtJoin.timedOut);
		assert(stoppedAtJoin.submittedJobCount == snapshots.size());
		assert(stoppedAtJoin.workerExecutedJobCount == snapshots.size());
		assert(stoppedAtJoin.ownerExecutedJobCount == 0);
		for (std::size_t i = 0; i < snapshots.size(); ++i)
		{
			const rts::DeterministicDirectPathExecutionSnapshot request =
				stoppedAtJoinBatch.requestExecutionSnapshot(i);
			assert(request.submitted && !request.succeeded);
			assert(request.state == rts::DIRECT_PATH_EXECUTION_WORKER);
		}
	}
	rts_direct_path_set_test_fault_mask(0);

	std::atomic<unsigned> entered(0);
	std::atomic<bool> release(false);
	rts::JobGroup blockerGroup = jobs.createGroup();
	assert(blockerGroup.isValid());
	assert(jobs.trySubmit(new BlockingJob(entered, release),
		rts::JOB_PRIORITY_NORMAL, blockerGroup).isValid());
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (entered.load(std::memory_order_acquire) != 1 &&
		std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	assert(entered.load(std::memory_order_acquire) == 1);
	{
		rts::DeterministicDirectPathBatch timedOutBatch;
		assert(!timedOutBatch.executeSynchronously(jobs, snapshots.data(),
			snapshots.size(), 1));
		const rts::DeterministicDirectPathBatchExecutionSnapshot timedOut =
			timedOutBatch.executionSnapshot();
		assert(!timedOut.completed && timedOut.timedOut);
		assert(timedOut.submittedJobCount == snapshots.size());
		for (std::size_t i = 0; i < snapshots.size(); ++i)
		{
			const rts::DeterministicDirectPathExecutionSnapshot request =
				timedOutBatch.requestExecutionSnapshot(i);
			assert(request.submitted && !request.succeeded);
			assert(request.state == rts::DIRECT_PATH_EXECUTION_CANCELLED);
		}
	}
	release.store(true, std::memory_order_release);
	const auto blockerCompletionDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (!blockerGroup.isComplete() &&
		std::chrono::steady_clock::now() < blockerCompletionDeadline)
	{
		std::this_thread::yield();
	}
	assert(blockerGroup.isComplete());
	// Joining an already-complete group cannot help unrelated cancelled jobs.
	assert(jobs.wait(blockerGroup));
	const auto drainDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (jobs.outstandingJobCount() != 0 &&
		std::chrono::steady_clock::now() < drainDeadline)
	{
		std::this_thread::yield();
	}
	assert(jobs.outstandingJobCount() == 0);

	const unsigned lateDrainBefore =
		rts::GetDeterministicDirectPathLateDrainExecutionCount();
	rts_direct_path_set_test_pause_mask(1);
	std::atomic<bool> pauseReached(false);
	std::atomic<bool> allowWorkerRelease(false);
	std::atomic<bool> forcedWorkerRelease(false);
	std::thread releasePausedWorker([&]() {
		pauseReached.store(rts_direct_path_wait_for_test_pause(1, 15000),
			std::memory_order_release);
		const auto forcedReleaseDeadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(5);
		while (!allowWorkerRelease.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < forcedReleaseDeadline)
			std::this_thread::yield();
		if (!allowWorkerRelease.load(std::memory_order_acquire))
			forcedWorkerRelease.store(true, std::memory_order_release);
		rts_direct_path_release_test_pause(1);
	});
	{
		rts::DeterministicDirectPathBatch lateStartBatch;
		assert(!lateStartBatch.executeSynchronously(jobs, snapshots.data(),
			snapshots.size(), 1));
		const rts::DeterministicDirectPathBatchExecutionSnapshot lateStart =
			lateStartBatch.executionSnapshot();
		assert(!lateStart.completed && lateStart.timedOut);
		assert(lateStart.submittedJobCount == snapshots.size());
		assert(lateStartBatch.requestExecutionSnapshot(0).state ==
			rts::DIRECT_PATH_EXECUTION_CANCELLED);
		rts::DeterministicDirectPathBatch whileCancellationDrains;
		assert(!whileCancellationDrains.executeSynchronously(jobs,
			snapshots.data(), snapshots.size(), 1));
		assert(whileCancellationDrains.executionSnapshot().submittedJobCount == 0);
		assert(!forcedWorkerRelease.load(std::memory_order_acquire));
		allowWorkerRelease.store(true, std::memory_order_release);
		releasePausedWorker.join();
		assert(pauseReached.load(std::memory_order_acquire));
		rts_direct_path_set_test_pause_mask(0);
		const auto lateDrainDeadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (jobs.outstandingJobCount() != 0 &&
			std::chrono::steady_clock::now() < lateDrainDeadline)
		{
			std::this_thread::yield();
		}
		assert(jobs.outstandingJobCount() == 0);
		assert(rts::GetDeterministicDirectPathLateDrainExecutionCount() >
			lateDrainBefore);
		assert(lateStartBatch.requestExecutionSnapshot(0).state ==
			rts::DIRECT_PATH_EXECUTION_CANCELLED);
	}
	assert(jobs.metrics().ownerHelpCount == ownerHelpBefore);
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
	rts::DeterministicDirectPathBatch stoppedBatch;
	assert(!stoppedBatch.executeSynchronously(jobs, snapshots.data(),
		snapshots.size(), 1));
	const rts::DeterministicDirectPathBatchExecutionSnapshot stopped =
		stoppedBatch.executionSnapshot();
	assert(!stopped.completed && !stopped.timedOut);
	assert(stopped.submittedJobCount == 0);
	assert(stoppedBatch.requestExecutionSnapshot(0).state ==
		rts::DIRECT_PATH_EXECUTION_PENDING);
}

struct OrdinaryFixture
{
	OrdinaryFixture(std::uint32_t widthValue, std::uint32_t heightValue,
		std::uint32_t generationValue) : width(widthValue), height(heightValue),
		generation(generationValue)
	{
		cells.resize(static_cast<std::size_t>(width) * height);
		for (auto &cell : cells)
		{
			cell = {};
			cell.traversalMask = 1;
			cell.obstacleObjectId = rts::DETERMINISTIC_PATH_INVALID_OBJECT_ID;
			cell.positionObjectId = rts::DETERMINISTIC_PATH_INVALID_OBJECT_ID;
			cell.goalObjectId = rts::DETERMINISTIC_PATH_INVALID_OBJECT_ID;
			cell.blockZone = 7;
			cell.globalZone = 7;
			cell.zone = 7;
			cell.type = rts::DETERMINISTIC_PATH_CELL_CLEAR;
			cell.flags = rts::DETERMINISTIC_PATH_NO_UNITS;
			cell.layer = rts::DETERMINISTIC_PATH_LAYER_GROUND;
			cell.connectsToLayer = rts::DETERMINISTIC_PATH_LAYER_INVALID;
			cell.blockPassable = 1;
			cell.navigationFlags = rts::DETERMINISTIC_PATH_INSIDE_LOGICAL_EXTENT;
		}
		const std::uint32_t wallX = width / 2;
		const std::uint32_t gapY = height / 2;
		for (std::uint32_t y = 0; y < height; ++y)
		{
			if (y == gapY)
				continue;
			rts::DeterministicPathCell &wall = cells[y * width + wallX];
			wall.traversalMask = 0;
			wall.type = rts::DETERMINISTIC_PATH_CELL_OBSTACLE;
		}
		grid = {};
		grid.cells = cells.data();
		grid.width = width;
		grid.height = height;
		grid.originX = 0;
		grid.originY = 0;
		grid.snapshotGeneration = generation;
	}

	rts::DeterministicOrdinaryPathBatchRequest makeRequest(
		std::size_t requestIndex) const
	{
		rts::DeterministicOrdinaryPathBatchRequest batchRequest = {};
		rts::DeterministicPathRequest &request = batchRequest.search;
		request.expectedSnapshotGeneration = generation;
		request.objectId = static_cast<std::uint32_t>(1000 + requestIndex);
		request.startX = 2;
		const std::size_t rowSpan = height / 8 == 0 ? 1 : height / 8;
		request.startY = static_cast<std::int32_t>(
			2 + requestIndex % rowSpan);
		request.goalX = static_cast<std::int32_t>(width - 3);
		request.goalY = static_cast<std::int32_t>(
			height / 4 + requestIndex % rowSpan);
		request.traversalMask = 1;
		request.maximumExpandedNodes = width * height * 8;
		request.availableCellInfoCount = width * height;
		request.requiredZone = 7;
		request.footprintRadius = 0;
		request.centerInCell = 1;
		request.allowDiagonal = 1;
		request.allowBlockedStart = 0;
		request.expectedLayer = rts::DETERMINISTIC_PATH_LAYER_GROUND;
		request.requireLegacyDirectLine = 0;
		request.requireObstructedSearch = 1;
		request.isHuman = 1;
		request.hierarchyMode = rts::DETERMINISTIC_PATH_HIERARCHY_PREPUBLISHED;
		request.hierarchyBlockSize = 10;
		batchRequest.ownerToken =
			(static_cast<std::uint64_t>(generation) << 32) |
			static_cast<std::uint64_t>(requestIndex + 1);
		return batchRequest;
	}

	std::uint32_t width;
	std::uint32_t height;
	std::uint32_t generation;
	std::vector<rts::DeterministicPathCell> cells;
	rts::ImmutableNavigationGrid grid;
};

struct OrdinarySerialResult
{
	rts::DeterministicPathSearchStatus status;
	std::vector<rts::DeterministicPathPoint> points;
	std::uint32_t expanded;
	std::uint32_t discovered;
	std::uint32_t requiredCellInfos;
	std::uint32_t cumulative;
	std::vector<std::uint32_t> passableBlocks;
	bool hierarchyAllPassable;
};

struct IndependentLegacyWidthResult
{
	rts::DeterministicPathSearchStatus status;
	std::vector<rts::DeterministicPathPoint> points;
	bool observedWrappedWrite;
};

IndependentLegacyWidthResult RunIndependentLegacyWidthOracle(
	const OrdinaryFixture &fixture,
	const rts::DeterministicPathRequest &request)
{
	// This intentionally does not call FindDeterministicPath.  It is a small,
	// list-scanned model of the legacy fixed ordinary subset used by the wrap
	// regression below.  Reusing the public, exhaustively checked supercover
	// callback builder keeps this oracle focused on legacy cost storage/order.
	assert(request.hierarchyMode == rts::DETERMINISTIC_PATH_HIERARCHY_PREPUBLISHED);
	assert(request.footprintRadius == 0 && request.centerInCell != 0);
	struct Node
	{
		std::uint16_t pathCost;
		std::uint16_t totalCost;
		std::uint32_t parent;
		std::uint64_t insertionOrdinal;
		std::uint8_t state;
	};
	const std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
	const std::uint8_t unseen = 0;
	const std::uint8_t open = 1;
	const std::uint8_t closed = 2;
	const std::size_t cellCount = fixture.cells.size();
	std::vector<Node> nodes(cellCount);
	for (Node &node : nodes)
	{
		node.pathCost = 0;
		node.totalCost = 0;
		node.parent = invalid;
		node.insertionOrdinal = 0;
		node.state = unseen;
	}
	IndependentLegacyWidthResult oracle = {};
	oracle.status = rts::DETERMINISTIC_PATH_NO_PATH;
	const auto inside = [&](std::int32_t x, std::int32_t y)
	{
		return x >= fixture.grid.originX && y >= fixture.grid.originY &&
			static_cast<std::uint32_t>(x - fixture.grid.originX) < fixture.width &&
			static_cast<std::uint32_t>(y - fixture.grid.originY) < fixture.height;
	};
	const auto indexOf = [&](std::int32_t x, std::int32_t y)
	{
		return static_cast<std::uint32_t>(y - fixture.grid.originY) *
			fixture.width + static_cast<std::uint32_t>(x - fixture.grid.originX);
	};
	const auto xOf = [&](std::uint32_t index)
	{
		return fixture.grid.originX +
			static_cast<std::int32_t>(index % fixture.width);
	};
	const auto yOf = [&](std::uint32_t index)
	{
		return fixture.grid.originY +
			static_cast<std::int32_t>(index / fixture.width);
	};
	const auto heuristic = [&](std::int32_t x, std::int32_t y)
	{
		const std::uint32_t dx = static_cast<std::uint32_t>(
			x < request.goalX ? request.goalX - x : x - request.goalX);
		const std::uint32_t dy = static_cast<std::uint32_t>(
			y < request.goalY ? request.goalY - y : y - request.goalY);
		const std::uint32_t major = dx > dy ? dx : dy;
		const std::uint32_t minor = dx > dy ? dy : dx;
		return major * 10U + minor * 5U;
	};
	const auto storeCost = [&](std::uint32_t raw)
	{
		if (raw > std::numeric_limits<std::uint16_t>::max())
			oracle.observedWrappedWrite = true;
		return static_cast<std::uint16_t>(raw);
	};
	const auto passable = [&](std::int32_t x, std::int32_t y,
		bool directLine)
	{
		if (!inside(x, y))
			return false;
		const rts::DeterministicPathCell &cell = fixture.cells[indexOf(x, y)];
		return cell.layer == request.expectedLayer &&
			cell.connectsToLayer == rts::DETERMINISTIC_PATH_LAYER_INVALID &&
			(cell.traversalMask & request.traversalMask) != 0 &&
			(!directLine || (cell.blockPassable != 0 && cell.pinched == 0));
	};

	const std::uint32_t startIndex = indexOf(request.startX, request.startY);
	const std::uint32_t goalIndex = indexOf(request.goalX, request.goalY);
	std::uint64_t nextInsertionOrdinal = 0;
	nodes[startIndex].pathCost = 0;
	nodes[startIndex].totalCost = storeCost(heuristic(request.startX,
		request.startY));
	nodes[startIndex].insertionOrdinal = nextInsertionOrdinal++;
	nodes[startIndex].state = open;

	static const std::int32_t deltaX[8] = {1, 0, -1, 0, 1, -1, -1, 1};
	static const std::int32_t deltaY[8] = {0, 1, 0, -1, 1, 1, -1, -1};
	static const std::size_t adjacent[5] = {0, 1, 2, 3, 0};
	for (std::size_t expanded = 0; expanded < request.maximumExpandedNodes;
		++expanded)
	{
		std::uint32_t currentIndex = invalid;
		for (std::uint32_t index = 0; index < cellCount; ++index)
		{
			if (nodes[index].state != open)
				continue;
			if (currentIndex == invalid ||
				nodes[index].totalCost < nodes[currentIndex].totalCost ||
				(nodes[index].totalCost == nodes[currentIndex].totalCost &&
				 nodes[index].insertionOrdinal <
					nodes[currentIndex].insertionOrdinal))
			{
				currentIndex = index;
			}
		}
		if (currentIndex == invalid)
			break;
		if (currentIndex == goalIndex)
		{
			oracle.status = rts::DETERMINISTIC_PATH_FOUND;
			break;
		}
		nodes[currentIndex].state = closed;
		const std::int32_t currentX = xOf(currentIndex);
		const std::int32_t currentY = yOf(currentIndex);

		std::array<rts::DeterministicPathPoint,
			rts::DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS> callbacks = {};
		std::size_t callbackCount = 0;
		assert(rts::BuildLegacySupercoverCallbacks(currentX, currentY,
			request.goalX, request.goalY, callbacks.data(), callbacks.size(),
			callbackCount));
		std::uint32_t previousLineIndex = currentIndex;
		for (std::size_t callback = 1; callback < callbackCount; ++callback)
		{
			const std::int32_t x = callbacks[callback].x;
			const std::int32_t y = callbacks[callback].y;
			if (!passable(x, y, true))
				break;
			const std::uint32_t toIndex = indexOf(x, y);
			const std::uint32_t candidate =
				static_cast<std::uint32_t>(nodes[previousLineIndex].pathCost) + 5U;
			if (nodes[toIndex].state == unseen ||
				nodes[toIndex].pathCost > candidate)
			{
				nodes[toIndex].pathCost = storeCost(candidate);
				nodes[toIndex].totalCost = storeCost(
					static_cast<std::uint32_t>(nodes[toIndex].pathCost) +
					heuristic(x, y));
				nodes[toIndex].parent = previousLineIndex;
				nodes[toIndex].insertionOrdinal = nextInsertionOrdinal++;
				nodes[toIndex].state = open;
			}
			previousLineIndex = toIndex;
		}

		bool neighborFlags[8] = {false, false, false, false,
			false, false, false, false};
		for (std::size_t neighbor = 0; neighbor < 8; ++neighbor)
		{
			const std::int32_t nextX = currentX + deltaX[neighbor];
			const std::int32_t nextY = currentY + deltaY[neighbor];
			if (!inside(nextX, nextY))
				continue;
			const std::uint32_t nextIndex = indexOf(nextX, nextY);
			if (nodes[nextIndex].state != unseen)
				continue;
			if (neighbor >= 4 &&
				!neighborFlags[adjacent[neighbor - 4]] &&
				!neighborFlags[adjacent[neighbor - 3]])
			{
				continue;
			}
			if (!passable(nextX, nextY, false))
				continue;
			neighborFlags[neighbor] = true;
			const std::int32_t previousX = currentX - nextX;
			const std::int32_t previousY = currentY - nextY;
			std::uint32_t candidate = nodes[currentIndex].pathCost +
				(previousX == 0 || previousY == 0 ? 10U : 14U);
			const rts::DeterministicPathCell &nextCell = fixture.cells[nextIndex];
			if (nextCell.pinched != 0)
				candidate += 14U;
			const std::uint32_t grandparent = nodes[currentIndex].parent;
			if (grandparent != invalid)
			{
				const std::int32_t directionX = xOf(grandparent) - currentX;
				const std::int32_t directionY = yOf(grandparent) - currentY;
				if (directionX != previousX || directionY != previousY)
				{
					const std::int32_t dot = directionX * previousX +
						directionY * previousY;
					candidate += dot > 0 ? 4U : (dot == 0 ? 8U : 16U);
				}
			}
			if (nextCell.pinched != 0)
				candidate += 10U;
			if (nextCell.blockPassable == 0)
				candidate += 1000U;
			nodes[nextIndex].pathCost = storeCost(candidate);
			nodes[nextIndex].totalCost = storeCost(
				static_cast<std::uint32_t>(nodes[nextIndex].pathCost) +
				heuristic(nextX, nextY));
			nodes[nextIndex].parent = currentIndex;
			nodes[nextIndex].insertionOrdinal = nextInsertionOrdinal++;
			nodes[nextIndex].state = open;
		}
	}
	if (oracle.status != rts::DETERMINISTIC_PATH_FOUND)
		return oracle;
	for (std::uint32_t index = goalIndex;; index = nodes[index].parent)
	{
		rts::DeterministicPathPoint point = {};
		point.x = xOf(index);
		point.y = yOf(index);
		point.layer = request.expectedLayer;
		oracle.points.push_back(point);
		if (index == startIndex)
			break;
		assert(nodes[index].parent != invalid);
	}
	std::reverse(oracle.points.begin(), oracle.points.end());
	return oracle;
}

OrdinarySerialResult RunOrdinarySerialOracle(const OrdinaryFixture &fixture,
	const rts::DeterministicOrdinaryPathBatchRequest &batchRequest)
{
	const std::size_t cellCount = fixture.cells.size();
	std::vector<rts::DeterministicPathSearchNode> nodes(cellCount);
	std::vector<std::uint32_t> heap(cellCount);
	std::vector<rts::DeterministicPathPoint> points(cellCount);
	const std::size_t blockWidth = (fixture.width + 9U) / 10U;
	const std::size_t blockHeight = (fixture.height + 9U) / 10U;
	const std::size_t blockCount = blockWidth * blockHeight;
	std::vector<std::uint8_t> hierarchyPassable(blockCount);
	std::vector<std::uint32_t> hierarchyBlocks(blockCount);
	rts::DeterministicPathSearchScratch scratch = {
		nodes.data(), nodes.size(), heap.data(), heap.size(),
		hierarchyPassable.data(), hierarchyPassable.size()
	};
	rts::DeterministicPathSearchResult result = {};
	result.points = points.data();
	result.pointCapacity = points.size();
	result.passableBlockIndices = hierarchyBlocks.data();
	result.passableBlockCapacity = hierarchyBlocks.size();
	rts::FindDeterministicPath(fixture.grid, batchRequest.search, scratch, result);
	OrdinarySerialResult serial = {};
	serial.status = result.status;
	serial.expanded = result.expandedNodeCount;
	serial.discovered = result.discoveredNodeCount;
	serial.requiredCellInfos = result.requiredCellInfoCount;
	serial.cumulative = result.cumulativeCellCount;
	serial.hierarchyAllPassable = result.hierarchyAllPassable != 0;
	if (result.status == rts::DETERMINISTIC_PATH_FOUND)
	{
		serial.points.assign(points.begin(), points.begin() + result.pointCount);
		serial.passableBlocks.assign(hierarchyBlocks.begin(),
			hierarchyBlocks.begin() + result.passableBlockCount);
	}
	return serial;
}

std::uint64_t HashOrdinaryPoints(
	const std::vector<rts::DeterministicPathPoint> &points)
{
	std::uint64_t hash = UINT64_C(1469598103934665603);
	for (const auto &point : points)
	{
		hash ^= static_cast<std::uint32_t>(point.x);
		hash *= UINT64_C(1099511628211);
		hash ^= static_cast<std::uint32_t>(point.y);
		hash *= UINT64_C(1099511628211);
		hash ^= point.layer;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

void AssertOrdinaryBatchMatchesSerial(
	const rts::DeterministicOrdinaryPathBatch &batch,
	const std::vector<OrdinarySerialResult> &serial,
	std::size_t requestCount)
{
	std::uint64_t serialCrc = 0;
	std::uint64_t workerCrc = 0;
	for (std::size_t i = 0; i < requestCount; ++i)
	{
		const rts::DeterministicDirectPathExecutionSnapshot execution =
			batch.requestExecutionSnapshot(i);
		assert(execution.submitted && execution.succeeded);
		assert(execution.state == rts::DIRECT_PATH_EXECUTION_WORKER);
		const rts::DeterministicOrdinaryPathBatchResult worker = batch.result(i);
		assert(worker.status == serial[i].status);
		assert(worker.expandedNodeCount == serial[i].expanded);
		assert(worker.discoveredNodeCount == serial[i].discovered);
		assert(worker.cumulativeCellCount == serial[i].cumulative);
		assert(worker.pointCount == serial[i].points.size());
		std::vector<rts::DeterministicPathPoint> workerPoints;
		if (worker.pointCount != 0)
			workerPoints.assign(worker.points, worker.points + worker.pointCount);
		assert(workerPoints.size() == serial[i].points.size());
		for (std::size_t point = 0; point < workerPoints.size(); ++point)
		{
			assert(workerPoints[point].x == serial[i].points[point].x);
			assert(workerPoints[point].y == serial[i].points[point].y);
			assert(workerPoints[point].layer == serial[i].points[point].layer);
		}
		assert(worker.allocationCount ==
			static_cast<std::size_t>(serial[i].requiredCellInfos));
		assert(worker.cleanupCount ==
			static_cast<std::size_t>(serial[i].cumulative));
		assert(worker.hierarchyAllPassable == serial[i].hierarchyAllPassable);
		assert(worker.passableBlockCount == serial[i].passableBlocks.size());
		for (std::size_t block = 0; block < worker.passableBlockCount; ++block)
			assert(worker.passableBlockIndices[block] ==
				serial[i].passableBlocks[block]);
		assert(worker.materializationPlanHash ==
			rts::ComputeDeterministicOrdinaryPathPlanHash(worker.points,
				worker.pointCount, worker.allocationOrder, worker.allocationCount,
				worker.cleanupOrder, worker.cleanupCount,
				worker.passableBlockIndices, worker.passableBlockCount,
				worker.hierarchyAllPassable,
				worker.snapshotGeneration, worker.objectId, worker.ownerToken));
		serialCrc ^= HashOrdinaryPoints(serial[i].points) + i;
		workerCrc ^= HashOrdinaryPoints(workerPoints) + i;
	}
	assert(workerCrc == serialCrc);
}

#if defined(_WIN64)
struct OrdinaryPathReferenceTestRun
{
	rts::performance::KernelPerformanceSnapshot timing;
	rts::performance::KernelPerformanceReferenceSnapshot reference;
	unsigned referenceClockReads;
};

OrdinaryPathReferenceTestRun RunOrdinaryPathReferenceBatch(
	rts::performance::KernelPerformanceReferenceMode mode, bool committed)
{
	using namespace rts::performance;
	OrdinaryFixture fixture(32, 32, 601);
	std::array<rts::DeterministicOrdinaryPathBatchRequest, 2> requests =
		{fixture.makeRequest(0), fixture.makeRequest(1)};
	std::vector<OrdinarySerialResult> serial;
	serial.reserve(requests.size());
	for (std::size_t index = 0; index < requests.size(); ++index)
	{
		requests[index].search.hierarchyMode =
			rts::DETERMINISTIC_PATH_HIERARCHY_ZERO_HOUR;
		serial.push_back(RunOrdinarySerialOracle(fixture, requests[index]));
		assert(serial.back().status == rts::DETERMINISTIC_PATH_FOUND);
	}

	PathPerformanceTestClock timingClock;
	PathPerformanceTestClock referenceClock;
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	KernelPerformanceReferenceLedger reference;
	assert(timing.beginRun(true, PathPerformanceTestClock::read,
		&timingClock));
	assert(reference.beginRun(mode, PathPerformanceTestClock::read,
		&referenceClock));
	assert(reference.mode() == mode);
	KernelPerformanceBatch timingBatch = timing.beginBatch(
		KERNEL_PERFORMANCE_PATH, 0, 242, 11);
	assert(timingBatch.valid());

	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	rts::DeterministicOrdinaryPathBatch batch;
	rts::performance::KernelPerformanceReferenceBatch referenceBatch;
	assert(batch.executeSynchronously(jobs, fixture.grid, requests.data(),
		requests.size(), 2000, &timingBatch, &reference,
		&referenceBatch));
	const rts::DeterministicOrdinaryPathBatchExecutionSnapshot execution =
		batch.executionSnapshot();
	assert(execution.completed);
	assert(execution.requestCount == requests.size());
	assert(execution.rangeCount == requests.size());
	AssertOrdinaryBatchMatchesSerial(batch, serial, requests.size());
	if (mode == KERNEL_REFERENCE_DISABLED)
		assert(!referenceBatch.valid());
	else
		assert(referenceBatch.valid());

	const KernelPerformanceInterval commit = timing.beginInterval(timingBatch,
		KERNEL_PERFORMANCE_COMMIT);
	assert(commit.valid());
	assert(timing.endInterval(commit));
	if (mode != KERNEL_REFERENCE_DISABLED)
		assert(reference.finishBatch(referenceBatch, committed));
	assert(timing.endBatch(timingBatch, committed ?
		KERNEL_PERFORMANCE_COMMITTED :
		KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION));
	const KernelPerformanceSnapshot timingSnapshot = timing.freeze();
	const KernelPerformanceReferenceSnapshot referenceSnapshot =
		reference.freeze();
	assert(timingSnapshot.complete);
	assert(timingSnapshot.streamCount == 1);
	const KernelPerformanceStream &timingStream = timingSnapshot.streams[0];
	assert(timingStream.kernel == KERNEL_PERFORMANCE_PATH);
	assert(timingStream.subtype == 0);
	assert(timingStream.firstFrame == 242 && timingStream.lastFrame == 242);
	for (unsigned stage = 0; stage < KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		assert(timingStream.stageSamples[stage] >= 1);
	assert(timingStream.attemptedBatches == 1);
	assert(timingStream.admittedBatches == 1);
	assert(timingStream.committedBatches == (committed ? 1 : 0));
	assert(timingStream.abortedBatches == (committed ? 0 : 1));
	assert(timingStream.activePipelineNanoseconds != 0);
	assert(timingClock.reads != 0);

	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
	OrdinaryPathReferenceTestRun result = {};
	result.timing = timingSnapshot;
	result.reference = referenceSnapshot;
	result.referenceClockReads = referenceClock.reads;
	return result;
}

void TestOrdinaryPathReferenceModesAndCommitBoundary()
{
	const OrdinaryPathReferenceTestRun disabled =
		RunOrdinaryPathReferenceBatch(
			rts::performance::KERNEL_REFERENCE_DISABLED, true);
	assert(disabled.reference.frozen);
	assert(disabled.reference.mode ==
		rts::performance::KERNEL_REFERENCE_DISABLED);
	assert(disabled.reference.errors == 0);
	assert(disabled.reference.streamCount == 0);
	assert(disabled.referenceClockReads == 0);

	const OrdinaryPathReferenceTestRun throughput =
		RunOrdinaryPathReferenceBatch(
			rts::performance::KERNEL_REFERENCE_THROUGHPUT_BINDING, true);
	const OrdinaryPathReferenceTestRun serial =
		RunOrdinaryPathReferenceBatch(
			rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE, true);
	assert(throughput.reference.complete);
	assert(serial.reference.complete);
	assert(throughput.reference.streamCount == 1);
	assert(serial.reference.streamCount == 1);
	const rts::performance::KernelPerformanceReferenceStream &throughputStream =
		throughput.reference.streams[0];
	const rts::performance::KernelPerformanceReferenceStream &serialStream =
		serial.reference.streams[0];
	assert(throughputStream.kernel == rts::performance::KERNEL_PERFORMANCE_PATH);
	assert(throughputStream.subtype == 0);
	assert(serialStream.kernel == rts::performance::KERNEL_PERFORMANCE_PATH);
	assert(serialStream.subtype == 0);
	assert(throughputStream.fieldSchema == 1);
	assert(serialStream.fieldSchema == 1);
	assert(throughputStream.firstFrame == 242 &&
		throughputStream.lastFrame == 242);
	assert(serialStream.firstFrame == 242 && serialStream.lastFrame == 242);
	assert(throughputStream.validatedBatchCount == 1);
	assert(serialStream.validatedBatchCount == 1);
	assert(throughputStream.validatedOperationCount == 2);
	assert(serialStream.validatedOperationCount == 2);
	assert(throughputStream.committedBatchCount == 1);
	assert(serialStream.committedBatchCount == 1);
	assert(throughputStream.committedOperationCount == 2);
	assert(serialStream.committedOperationCount == 2);
	assert(throughputStream.serialSampleCount == 0);
	assert(throughputStream.serialNanoseconds == 0);
	assert(throughputStream.inputDigest.equals(serialStream.inputDigest));
	assert(throughputStream.outputDigest.equals(serialStream.outputDigest));
	assert(throughputStream.commitDigest.equals(serialStream.commitDigest));
	assert(throughput.referenceClockReads == 0);
	assert(serialStream.serialSampleCount == 1);
	assert(serialStream.serialNanoseconds != 0);
	assert(serial.referenceClockReads != 0);

	const OrdinaryPathReferenceTestRun aborted =
		RunOrdinaryPathReferenceBatch(
			rts::performance::KERNEL_REFERENCE_SERIAL_ORACLE, false);
	assert(aborted.reference.complete);
	assert(aborted.reference.streamCount == 1);
	const rts::performance::KernelPerformanceReferenceStream &abortedStream =
		aborted.reference.streams[0];
	assert(abortedStream.validatedBatchCount == 1);
	assert(abortedStream.validatedOperationCount == 2);
	assert(abortedStream.abortedBatchCount == 1);
	assert(abortedStream.committedBatchCount == 0);
	assert(abortedStream.committedOperationCount == 0);
	assert(abortedStream.serialSampleCount == 0);
	assert(abortedStream.serialNanoseconds == 0);
	assert(aborted.referenceClockReads != 0);
}
#endif

void TestOrdinaryLegacyWidthWrapAndBridgeFallback()
{
	OrdinaryFixture fixture(70, 10, 880);
	for (rts::DeterministicPathCell &cell : fixture.cells)
	{
		cell.traversalMask = 0;
		cell.type = rts::DETERMINISTIC_PATH_CELL_OBSTACLE;
		cell.pinched = 0;
		cell.blockPassable = 0;
	}
	const auto openCell = [&](std::int32_t x, std::int32_t y,
		bool hierarchyPassable, bool pinched)
	{
		rts::DeterministicPathCell &cell = fixture.cells[
			static_cast<std::size_t>(y) * fixture.width + x];
		cell.traversalMask = 1;
		cell.type = rts::DETERMINISTIC_PATH_CELL_CLEAR;
		cell.pinched = pinched ? 1 : 0;
		cell.blockPassable = hierarchyPassable ? 1 : 0;
	};
	openCell(1, 5, false, false);
	for (std::int32_t x = 2; x <= 64; ++x)
		openCell(x, 5, false, true);
	// The east write from (64,5) crosses 65535. Legacy wraps it below the
	// already-open southern detour; a 32-bit A* instead takes that detour.
	openCell(65, 5, false, false);
	openCell(66, 5, true, false);
	openCell(67, 5, true, false);
	openCell(68, 5, true, false);
	openCell(65, 6, true, false);
	openCell(65, 7, true, false);
	openCell(66, 7, true, false);
	openCell(67, 7, true, false);
	openCell(68, 7, true, false);
	openCell(68, 6, true, false);

	rts::DeterministicOrdinaryPathBatchRequest batchRequest = fixture.makeRequest(0);
	rts::DeterministicPathRequest &request = batchRequest.search;
	request.startX = 1;
	request.startY = 5;
	request.goalX = 68;
	request.goalY = 5;
	request.hierarchyMode = rts::DETERMINISTIC_PATH_HIERARCHY_PREPUBLISHED;
	const IndependentLegacyWidthResult oracle =
		RunIndependentLegacyWidthOracle(fixture, request);
	assert(oracle.status == rts::DETERMINISTIC_PATH_FOUND);
	assert(oracle.observedWrappedWrite);

	const std::size_t cellCount = fixture.cells.size();
	std::vector<rts::DeterministicPathSearchNode> nodes(cellCount);
	std::vector<std::uint32_t> heap(cellCount);
	std::vector<rts::DeterministicPathPoint> points(cellCount);
	std::vector<std::uint8_t> hierarchyPassable(70);
	std::vector<std::uint32_t> hierarchyBlocks(70);
	rts::DeterministicPathSearchScratch scratch = {
		nodes.data(), nodes.size(), heap.data(), heap.size(),
		hierarchyPassable.data(), hierarchyPassable.size()
	};
	rts::DeterministicPathSearchResult result = {};
	result.points = points.data();
	result.pointCapacity = points.size();
	result.passableBlockIndices = hierarchyBlocks.data();
	result.passableBlockCapacity = hierarchyBlocks.size();
	assert(rts::FindDeterministicPath(fixture.grid, request, scratch, result) ==
		rts::DETERMINISTIC_PATH_FOUND);
	assert(result.pointCount == oracle.points.size());
	for (std::size_t i = 0; i < result.pointCount; ++i)
	{
		assert(result.points[i].x == oracle.points[i].x);
		assert(result.points[i].y == oracle.points[i].y);
		assert(result.points[i].layer == oracle.points[i].layer);
	}
	bool usedWrappedEastBranch = false;
	bool usedUnwrappedDetour = false;
	for (const rts::DeterministicPathPoint &point : oracle.points)
	{
		usedWrappedEastBranch |= point.x == 65 && point.y == 5;
		usedUnwrappedDetour |= point.x == 65 && point.y == 6;
	}
	assert(usedWrappedEastBranch && !usedUnwrappedDetour);

	OrdinaryFixture bridgeFixture(40, 40, 881);
	rts::DeterministicOrdinaryPathBatchRequest bridgeRequest =
		bridgeFixture.makeRequest(0);
	bridgeRequest.search.hierarchyMode =
		rts::DETERMINISTIC_PATH_HIERARCHY_GENERALS;
	bridgeFixture.cells[static_cast<std::size_t>(bridgeRequest.search.startY) *
		bridgeFixture.width + bridgeRequest.search.startX].navigationFlags |=
		rts::DETERMINISTIC_PATH_BLOCK_INTERACTS_WITH_BRIDGE;
	const std::size_t bridgeCellCount = bridgeFixture.cells.size();
	std::vector<rts::DeterministicPathSearchNode> bridgeNodes(bridgeCellCount);
	std::vector<std::uint32_t> bridgeHeap(bridgeCellCount);
	std::vector<rts::DeterministicPathPoint> bridgePoints(bridgeCellCount);
	std::vector<std::uint8_t> bridgePassable(16);
	std::vector<std::uint32_t> bridgeBlocks(16);
	rts::DeterministicPathSearchScratch bridgeScratch = {
		bridgeNodes.data(), bridgeNodes.size(), bridgeHeap.data(), bridgeHeap.size(),
		bridgePassable.data(), bridgePassable.size()
	};
	rts::DeterministicPathSearchResult bridgeResult = {};
	bridgeResult.points = bridgePoints.data();
	bridgeResult.pointCapacity = bridgePoints.size();
	bridgeResult.passableBlockIndices = bridgeBlocks.data();
	bridgeResult.passableBlockCapacity = bridgeBlocks.size();
	assert(rts::FindDeterministicPath(bridgeFixture.grid, bridgeRequest.search,
		bridgeScratch, bridgeResult) == rts::DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
}

void TestOrdinaryOneWorkerSerialParity()
{
	OrdinaryFixture fixture(48, 48, 501);
	std::vector<rts::DeterministicOrdinaryPathBatchRequest> requests;
	std::vector<OrdinarySerialResult> serial;
	for (std::size_t i = 0; i < 2; ++i)
	{
		requests.push_back(fixture.makeRequest(i));
		requests.back().search.hierarchyMode =
			rts::DETERMINISTIC_PATH_HIERARCHY_ZERO_HOUR;
		serial.push_back(RunOrdinarySerialOracle(fixture, requests.back()));
		assert(serial.back().status == rts::DETERMINISTIC_PATH_FOUND);
		assert(serial.back().hierarchyAllPassable);
		assert(serial.back().passableBlocks.empty());
	}

	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	const rts::JobMetricCounter ownerHelpBefore = jobs.metrics().ownerHelpCount;
	rts::DeterministicOrdinaryPathBatch batch;
	assert(batch.executeSynchronously(jobs, fixture.grid, requests.data(),
		requests.size(), 2000));
	AssertOrdinaryBatchMatchesSerial(batch, serial, requests.size());
	const rts::DeterministicOrdinaryPathBatchExecutionSnapshot execution =
		batch.executionSnapshot();
	assert(execution.rangeCount == 1);
	assert(execution.workerExecutedRangeJobCount == 1);
	assert(execution.ownerExecutedRangeJobCount == 0);
	assert(execution.distinctPhysicalWorkerCount == 1);
	assert(execution.physicalWorkerMaskComplete);
	assert(execution.peakActiveWorkers == 1);
	assert(!rts::IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(execution));
	assert(jobs.metrics().ownerHelpCount == ownerHelpBefore);
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
}

void TestOrdinaryAdaptiveLargeObstructedBatchAndFaults()
{
	OrdinaryFixture scaleFixture(40, 40, 776);
	std::vector<rts::DeterministicOrdinaryPathBatchRequest> scaleRequests;
	std::vector<OrdinarySerialResult> scaleSerial;
	scaleRequests.reserve(16);
	scaleSerial.reserve(16);
	for (std::size_t i = 0; i < 16; ++i)
	{
		scaleRequests.push_back(scaleFixture.makeRequest(i));
		scaleRequests.back().search.hierarchyMode =
			rts::DETERMINISTIC_PATH_HIERARCHY_GENERALS;
		scaleSerial.push_back(RunOrdinarySerialOracle(scaleFixture,
			scaleRequests.back()));
		assert(scaleSerial.back().status == rts::DETERMINISTIC_PATH_FOUND);
		assert(!scaleSerial.back().hierarchyAllPassable);
		assert(!scaleSerial.back().passableBlocks.empty());
	}

	OrdinaryFixture fixture(96, 96, 777);
	constexpr std::size_t requestCount = 24;
	std::vector<rts::DeterministicOrdinaryPathBatchRequest> requests;
	std::vector<OrdinarySerialResult> serial;
	requests.reserve(requestCount);
	serial.reserve(requestCount);
	for (std::size_t i = 0; i < requestCount; ++i)
	{
		requests.push_back(fixture.makeRequest(i));
		requests.back().search.hierarchyMode =
			rts::DETERMINISTIC_PATH_HIERARCHY_GENERALS;
		serial.push_back(RunOrdinarySerialOracle(fixture, requests.back()));
		assert(serial.back().status == rts::DETERMINISTIC_PATH_FOUND);
		assert(!serial.back().hierarchyAllPassable);
		assert(!serial.back().passableBlocks.empty());
	}

	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 64;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	const rts::JobMetricCounter ownerHelpBefore = jobs.metrics().ownerHelpCount;

	const std::array<std::size_t, 5> scaleCounts = {1, 2, 4, 8, 16};
	for (const std::size_t scale : scaleCounts)
	{
		rts::DeterministicOrdinaryPathBatch scaleBatch;
		assert(scaleBatch.executeSynchronously(jobs, scaleFixture.grid,
			scaleRequests.data(), scale, 3000));
		AssertOrdinaryBatchMatchesSerial(scaleBatch, scaleSerial, scale);
		assert(scaleBatch.executionSnapshot().requestCount == scale);
	}

	rts_direct_path_set_test_pause_mask(8);
	std::atomic<bool> allWorkersReached(false);
	std::thread releaseWorkers([&]() {
		allWorkersReached.store(rts_direct_path_wait_for_test_pause_count(
			8, 4, 15000), std::memory_order_release);
		rts_direct_path_release_test_pause(8);
	});
	rts::DeterministicOrdinaryPathBatch largeBatch;
	assert(largeBatch.executeSynchronously(jobs, fixture.grid, requests.data(),
		requests.size(), 5000));
	releaseWorkers.join();
	rts_direct_path_set_test_pause_mask(0);
	assert(allWorkersReached.load(std::memory_order_acquire));
	AssertOrdinaryBatchMatchesSerial(largeBatch, serial, requests.size());
	const rts::DeterministicOrdinaryPathBatchExecutionSnapshot largeExecution =
		largeBatch.executionSnapshot();
	assert(largeExecution.requestCount > 16);
	assert(largeExecution.rangeCount == 4);
	assert(largeExecution.grainSize == 6);
	assert(largeExecution.workerExecutedRangeJobCount == 4);
	assert(largeExecution.ownerExecutedRangeJobCount == 0);
	assert(largeExecution.distinctPhysicalWorkerCount == 4);
	assert(largeExecution.physicalWorkerMaskComplete);
	assert(largeExecution.peakActiveWorkers == 4);
	assert(rts::IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(
		largeExecution));

	rts_direct_path_set_test_fault_mask(4);
	rts::DeterministicOrdinaryPathBatch faulted;
	assert(!faulted.executeSynchronously(jobs, fixture.grid, requests.data(),
		4, 1000));
	assert(faulted.executionSnapshot().failedRangeJobCount != 0);
	rts_direct_path_set_test_fault_mask(0);

	const unsigned dispatchVectorAllocationFault = 256U;
	rts::DeterministicOrdinaryPathBatch allocationFaulted;
	bool allocationExceptionEscaped = false;
	bool allocationCompleted = true;
	rts_direct_path_set_test_fault_mask(dispatchVectorAllocationFault);
	try
	{
		allocationCompleted = allocationFaulted.executeSynchronously(jobs,
			fixture.grid, requests.data(), 4, 1000);
	}
	catch (...)
	{
		allocationExceptionEscaped = true;
	}
	rts_direct_path_set_test_fault_mask(0);
	assert(!allocationExceptionEscaped);
	assert(!allocationCompleted);
	const rts::DeterministicOrdinaryPathBatchExecutionSnapshot allocationExecution =
		allocationFaulted.executionSnapshot();
	assert(!allocationExecution.completed && !allocationExecution.timedOut);
	assert(allocationExecution.submittedRangeJobCount == 0);
	rts::DeterministicOrdinaryPathBatch recoveredAfterAllocation;
	assert(recoveredAfterAllocation.executeSynchronously(jobs, fixture.grid,
		requests.data(), 4, 1000));

	std::vector<rts::DeterministicOrdinaryPathBatchRequest> staleRequests(1,
		requests[0]);
	staleRequests[0].search.expectedSnapshotGeneration = fixture.generation + 1;
	rts::DeterministicOrdinaryPathBatch stale;
	assert(stale.executeSynchronously(jobs, fixture.grid, staleRequests.data(),
		staleRequests.size(), 1000));
	assert(stale.result(0).status ==
		rts::DETERMINISTIC_PATH_SNAPSHOT_GENERATION_MISMATCH);

	rts_direct_path_set_test_fault_mask(8);
	rts::DeterministicOrdinaryPathBatch stopped;
	assert(!stopped.executeSynchronously(jobs, fixture.grid, requests.data(),
		4, 1000));
	assert(stopped.executionSnapshot().timedOut);
	rts_direct_path_set_test_fault_mask(0);

	assert(jobs.metrics().ownerHelpCount == ownerHelpBefore);
	assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	jobs.shutdown();
}

#if defined(_WIN64)
struct NativePathWorkerScope
{
	NativePathWorkerScope() : jobs(rts::JobSystem::instance())
	{
		assert(jobs.outstandingJobCount() == 0);
		rts::JobSystemConfig config;
		config.workerCount = 4;
		config.queueCapacity = 64;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		assert(jobs.start(config));
		assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	}

	~NativePathWorkerScope()
	{
		assert(jobs.outstandingJobCount() == 0);
		jobs.shutdown();
		// Native owners must drain and shut down the scheduler while the owner
		// identity is still attached.  Unregister is the final teardown step;
		// doing it first hides late source/reap calls behind a null owner.
		assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
	}

	rts::JobSystem &jobs;
};

struct NativeOrdinaryQuotaFixture
{
	explicit NativeOrdinaryQuotaFixture(std::size_t requestCount) :
		cells(3277), requests(requestCount), grid()
	{
		for (auto &cell : cells)
		{
			cell = {};
			cell.traversalMask = 1;
			cell.obstacleObjectId = rts::DETERMINISTIC_PATH_INVALID_OBJECT_ID;
			cell.positionObjectId = rts::DETERMINISTIC_PATH_INVALID_OBJECT_ID;
			cell.goalObjectId = rts::DETERMINISTIC_PATH_INVALID_OBJECT_ID;
			cell.blockZone = cell.globalZone = cell.zone = 7;
			cell.type = rts::DETERMINISTIC_PATH_CELL_CLEAR;
			cell.flags = rts::DETERMINISTIC_PATH_NO_UNITS;
			cell.layer = rts::DETERMINISTIC_PATH_LAYER_GROUND;
			cell.connectsToLayer = rts::DETERMINISTIC_PATH_LAYER_INVALID;
			cell.blockPassable = 1;
			cell.navigationFlags = rts::DETERMINISTIC_PATH_INSIDE_LOGICAL_EXTENT;
		}
		grid.cells = cells.data();
		grid.width = 3277;
		grid.height = 1;
		grid.snapshotGeneration = 3207;
		for (std::size_t i = 0; i < requestCount; ++i)
		{
			auto &request = requests[i].search;
			request = {};
			request.expectedSnapshotGeneration = grid.snapshotGeneration;
			request.objectId = static_cast<std::uint32_t>(1000 + i);
			request.goalX = 3276;
			request.traversalMask = 1;
			request.maximumExpandedNodes = 3277;
			request.availableCellInfoCount = 3277;
			request.requiredZone = 7;
			request.centerInCell = 1;
			request.allowDiagonal = 1;
			request.expectedLayer = rts::DETERMINISTIC_PATH_LAYER_GROUND;
			request.isHuman = 1;
			request.hierarchyMode = rts::DETERMINISTIC_PATH_HIERARCHY_PREPUBLISHED;
			request.hierarchyBlockSize = 10;
			// This is the native ordinary A* helper's valid clear-line subset,
			// not the title owner's separate obstructed-only admission fixture.
			request.requireLegacyDirectLine = 0;
			request.requireObstructedSearch = 0;
			requests[i].ownerToken = (std::uint64_t(3207) << 32) | (i + 1);
		}
	}

	std::vector<rts::DeterministicPathCell> cells;
	std::vector<rts::DeterministicOrdinaryPathBatchRequest> requests;
	rts::ImmutableNavigationGrid grid;
};

void AssertLiteralNativeOrdinaryQuotaPath(
	const rts::DeterministicOrdinaryPathBatchResult &result,
	const rts::DeterministicOrdinaryPathBatchRequest &request)
{
	assert(result.status == rts::DETERMINISTIC_PATH_FOUND);
	assert(result.pointCount == 3277 && result.allocationCount == 3277);
	assert(result.cleanupCount == 3276 && result.passableBlockCount == 0);
	assert(result.expandedNodeCount == 2 && result.discoveredNodeCount == 3277);
	assert(result.cumulativeCellCount == 3276 && !result.hierarchyAllPassable);
	assert(result.snapshotGeneration == 3207);
	assert(result.objectId == request.search.objectId);
	assert(result.ownerToken == request.ownerToken);
	assert(result.materializationPlanHash != 0);
	// The initial direct injection discovers the complete horizontal chain.
	// Its decreasing estimated totals make the goal the second pop. The goal
	// is terminal rather than open/closed, so cleanup excludes exactly one cell.
	assert(result.pointCount * 12 + result.allocationCount * 4 +
		result.cleanupCount * 4 == 65536);
	for (std::size_t i = 0; i < 3277; ++i)
	{
		assert(result.points[i].x == static_cast<std::int32_t>(i));
		assert(result.points[i].y == 0);
		assert(result.points[i].layer == rts::DETERMINISTIC_PATH_LAYER_GROUND);
		assert(result.points[i].reserved[0] == 0 &&
			result.points[i].reserved[1] == 0 && result.points[i].reserved[2] == 0);
		const std::uint32_t expectedAllocation = i == 0 ? 3276 :
			(i == 1 ? 0 : static_cast<std::uint32_t>(i - 1));
		assert(result.allocationOrder[i] == expectedAllocation);
	}
	for (std::size_t i = 0; i < 3276; ++i)
	{
		const std::uint32_t expectedCleanup = i == 3275 ? 0 :
			static_cast<std::uint32_t>(3275 - i);
		assert(result.cleanupOrder[i] == expectedCleanup);
	}
}

void TestNativeOrdinaryQuotaOneRequestLiteral()
{
	NativePathWorkerScope runtime;
	NativeOrdinaryQuotaFixture input(1);
	const auto before = runtime.jobs.metrics();
	rts::DeterministicOrdinaryPathBatch batch;
	assert(batch.executeSynchronously(runtime.jobs, input.grid,
		input.requests.data(), input.requests.size(), 10000));
	const auto execution = batch.executionSnapshot();
	assert(execution.completed && !execution.timedOut);
	assert(execution.requestCount == 1 && execution.rangeCount == 1);
	assert(execution.grainSize == 1 && execution.submittedRangeJobCount == 1);
	assert(execution.workerExecutedRangeJobCount == 1);
	assert(execution.ownerExecutedRangeJobCount == 0);
	assert(execution.resultStorageBytes == 65536);
	AssertLiteralNativeOrdinaryQuotaPath(batch.result(0), input.requests[0]);
	assert(runtime.jobs.metrics().ownerHelpCount == before.ownerHelpCount);
}

void TestNativeOrdinaryQuotaRetains2048AndRefusesOne()
{
	NativePathWorkerScope runtime;
	NativeOrdinaryQuotaFixture input(2049);
	const auto before = runtime.jobs.metrics();
	rts::DeterministicOrdinaryPathBatch batch;
	assert(batch.executeSynchronously(runtime.jobs, input.grid,
		input.requests.data(), input.requests.size(), 10000));
	const auto execution = batch.executionSnapshot();
	assert(execution.completed && !execution.timedOut);
	assert(execution.requestCount == 2049 && execution.rangeCount == 4);
	assert(execution.grainSize == 513 && execution.submittedRangeJobCount == 4);
	assert(execution.workerExecutedRangeJobCount == 4);
	assert(execution.ownerExecutedRangeJobCount == 0);
	assert(execution.failedRangeJobCount == 0);
	assert(execution.resultStorageBytes == 134217728);
	unsigned found = 0, refused = 0;
	for (std::size_t i = 0; i < input.requests.size(); ++i)
	{
		const auto result = batch.result(i);
		assert(batch.requestExecutionSnapshot(i).state ==
			rts::DIRECT_PATH_EXECUTION_WORKER);
		if (result.status == rts::DETERMINISTIC_PATH_FOUND)
		{
			++found;
			AssertLiteralNativeOrdinaryQuotaPath(result, input.requests[i]);
		}
		else
		{
			++refused;
			assert(result.status == rts::DETERMINISTIC_PATH_BUDGET_EXHAUSTED);
			assert(result.expandedNodeCount == 2 && result.discoveredNodeCount == 3277);
			assert(result.pointCount == 0 && result.allocationCount == 0);
			assert(result.cleanupCount == 0 && result.materializationPlanHash == 0);
		}
	}
	// Which request loses the shared reservation race is source-dynamic. Its
	// identity is not guessed from range order; the real total is exact.
	assert(found == 2048 && refused == 1);
	assert(runtime.jobs.metrics().ownerHelpCount == before.ownerHelpCount);
}

struct NativeOrdinaryRangeObservation
{
	struct Entry
	{
		unsigned rangeIndex;
		std::size_t begin;
		std::size_t end;
	};

	NativeOrdinaryRangeObservation() : entries(), count(0) {}

	std::array<Entry, 4> entries;
	unsigned count;
};

void ObserveNativeOrdinaryRange(void *opaque,
	rts::DeterministicOrdinaryPathTestEvent event, unsigned rangeIndex,
	std::size_t begin, std::size_t end)
{
	assert(event == rts::DETERMINISTIC_ORDINARY_PATH_TEST_RANGE_PLANNED);
	NativeOrdinaryRangeObservation &observation =
		*static_cast<NativeOrdinaryRangeObservation *>(opaque);
	if (observation.count < observation.entries.size())
	{
		NativeOrdinaryRangeObservation::Entry &entry =
			observation.entries[observation.count];
		entry.rangeIndex = rangeIndex;
		entry.begin = begin;
		entry.end = end;
	}
	++observation.count;
}

void TestNativeOrdinaryFiveRequestCanonicalRanges()
{
	NativePathWorkerScope runtime;
	NativeOrdinaryQuotaFixture input(5);
	NativeOrdinaryRangeObservation observation;
	rts::DeterministicOrdinaryPathTestHooks hooks;
	hooks.context = &observation;
	hooks.observe = &ObserveNativeOrdinaryRange;
	const auto before = runtime.jobs.metrics();
	rts::DeterministicOrdinaryPathBatch batch;
	assert(batch.executeSynchronously(runtime.jobs, input.grid,
		input.requests.data(), input.requests.size(), 10000,
		nullptr, nullptr, nullptr, &hooks));

	assert(observation.count == 4);
	const std::size_t expectedBegins[4] = {0, 2, 3, 4};
	const std::size_t expectedEnds[4] = {2, 3, 4, 5};
	for (unsigned i = 0; i < 4; ++i)
	{
		const NativeOrdinaryRangeObservation::Entry &entry =
			observation.entries[i];
		assert(entry.rangeIndex == i);
		assert(entry.begin == expectedBegins[i]);
		assert(entry.end == expectedEnds[i]);
		assert(entry.begin < entry.end);
		assert(i == 0 ? entry.begin == 0 :
			entry.begin == observation.entries[i - 1].end);
		rts::JobRange canonical = {};
		assert(rts::JobSystem::rangeForIndex(5, 4, i, canonical));
		assert(entry.begin == canonical.begin && entry.end == canonical.end);
	}
	assert(observation.entries[3].end == 5);

	const rts::DeterministicOrdinaryPathBatchExecutionSnapshot execution =
		batch.executionSnapshot();
	assert(execution.completed && !execution.timedOut);
	assert(execution.requestCount == 5 && execution.rangeCount == 4);
	assert(execution.grainSize == 2);
	assert(execution.submittedRangeJobCount == 4);
	assert(execution.workerExecutedRangeJobCount == 4);
	assert(execution.ownerExecutedRangeJobCount == 0);
	assert(execution.failedRangeJobCount == 0);
	assert(execution.resultStorageBytes == 327680);

	unsigned found = 0;
	for (std::size_t i = 0; i < input.requests.size(); ++i)
	{
		const rts::DeterministicDirectPathExecutionSnapshot requestExecution =
			batch.requestExecutionSnapshot(i);
		assert(requestExecution.submitted && requestExecution.succeeded);
		assert(requestExecution.state == rts::DIRECT_PATH_EXECUTION_WORKER);
		assert(requestExecution.physicalWorkerIndex !=
			rts::JOB_INVALID_PHYSICAL_WORKER_INDEX);
		const rts::DeterministicOrdinaryPathBatchResult result = batch.result(i);
		AssertLiteralNativeOrdinaryQuotaPath(result, input.requests[i]);
		assert(result.status == rts::DETERMINISTIC_PATH_FOUND);
		++found;
	}
	assert(found == 5);
	assert(runtime.jobs.outstandingJobCount() == 0);
	assert(runtime.jobs.metrics().ownerHelpCount == before.ownerHelpCount);
}

void WaitForNativePathDrain(rts::JobSystem &jobs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (jobs.outstandingJobCount() != 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	assert(jobs.outstandingJobCount() == 0);
}

// Release the already-existing scheduler finalizer pause after one new real
// submission. The deadline always releases it; failure is asserted only after
// joining the controller, so an admission regression cannot strand a worker.
struct NativePathFinalizerRelease
{
	explicit NativePathFinalizerRelease(rts::JobSystem &jobs) :
		observedSubmission(false), controller([this, &jobs,
			submittedBefore = jobs.metrics().submittedJobCount]()
		{
			const auto deadline = std::chrono::steady_clock::now() +
				std::chrono::seconds(5);
			while (jobs.metrics().submittedJobCount == submittedBefore &&
				std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			observedSubmission.store(jobs.metrics().submittedJobCount !=
				submittedBefore, std::memory_order_release);
			rts_job_system_release_test_pause(32768);
		})
	{}

	void join()
	{
		controller.join();
		rts_job_system_set_test_pause_mask(0);
		assert(observedSubmission.load(std::memory_order_acquire));
	}

	std::atomic<bool> observedSubmission;
	std::thread controller;
};

void TestNativeDirectEntryPauseWaitsForBothAdmittedWorkers()
{
	NativePathWorkerScope runtime;
	DirectFixture first(0, 0, 4, 0), second(0, 1, 4, 1);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const rts::DirectPathSnapshot inputs[] = {first.snapshot, second.snapshot};
	const auto before = runtime.jobs.metrics();
	const unsigned lateBefore = rts::GetDeterministicDirectPathLateDrainExecutionCount();
	std::atomic<unsigned> entered(0);
	std::atomic<bool> releaseBlockers(false), batchReturned(false);
	std::atomic<bool> firstReached(false), prematureReturn(false);
	rts::JobGroup blockers = runtime.jobs.createGroup();
	assert(blockers.isValid());
	rts::JobSubmission submissions[3];
	rts::JobHandle handles[3];
	for (unsigned index = 0; index != 3; ++index)
	{
		submissions[index].job = new BlockingJob(entered, releaseBlockers);
		submissions[index].priority = rts::JOB_PRIORITY_NORMAL;
	}
	const bool blockersSubmitted = runtime.jobs.trySubmitBatch(submissions, 3, blockers, handles);
	if (!blockersSubmitted)
	{
		for (unsigned index = 0; index != 3; ++index) delete submissions[index].job;
		assert(blockersSubmitted);
		return;
	}
	const auto entryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (entered.load(std::memory_order_acquire) != 3 &&
		std::chrono::steady_clock::now() < entryDeadline)
		std::this_thread::yield();
	const bool allBlockersEntered = entered.load(std::memory_order_acquire) == 3;
	if (!allBlockersEntered)
	{
		releaseBlockers.store(true, std::memory_order_release);
		WaitForNativePathDrain(runtime.jobs);
		assert(allBlockersEntered);
		return;
	}

	// Exactly one worker is available. The second admitted direct request
	// cannot enter until this controller releases the three real blockers.
	// The 100-ms observation is bounded adverse scheduling evidence, not an
	// ordinary timeout/qualification measurement or an absolute scheduler proof.
	rts_direct_path_set_test_pause_mask(1);
	std::thread controller;
	try
	{
		controller = std::thread([&]()
		{
			const bool reached = rts_direct_path_wait_for_test_pause(1, 5000);
			firstReached.store(reached, std::memory_order_release);
			if (reached)
			{
				const auto observationDeadline = std::chrono::steady_clock::now() +
					std::chrono::milliseconds(100);
				while (!batchReturned.load(std::memory_order_acquire) &&
					std::chrono::steady_clock::now() < observationDeadline)
					std::this_thread::yield();
				prematureReturn.store(batchReturned.load(std::memory_order_acquire),
					std::memory_order_release);
			}
			releaseBlockers.store(true, std::memory_order_release);
		});
	}
	catch (...)
	{
		releaseBlockers.store(true, std::memory_order_release);
		rts_direct_path_set_test_pause_mask(0);
		WaitForNativePathDrain(runtime.jobs);
		assert(false && "direct entry-pause controller could not start");
		return;
	}
	rts::DeterministicDirectPathBatch retained;
	const bool completed = retained.executeSynchronously(runtime.jobs, inputs, 2, 1);
	batchReturned.store(true, std::memory_order_release);
	controller.join();
	const bool bothReached = rts_direct_path_wait_for_test_pause_count(1, 2, 25);
	rts_direct_path_release_test_pause(1);
	rts_direct_path_set_test_pause_mask(0);
	WaitForNativePathDrain(runtime.jobs);

	// Every held job is released and drained before any behavioral assertion.
	assert(firstReached.load(std::memory_order_acquire));
	assert(!prematureReturn.load(std::memory_order_acquire));
	assert(bothReached);
	assert(!completed && retained.executionSnapshot().timedOut);
	assert(retained.executionSnapshot().submittedJobCount == 2);
	assert(blockers.isComplete() && !blockers.failed() && !blockers.wasCancelled());
	for (unsigned index = 0; index != 2; ++index)
	{
		assert(retained.requestExecutionSnapshot(index).state == rts::DIRECT_PATH_EXECUTION_CANCELLED);
		assert(!retained.requestExecutionSnapshot(index).succeeded);
		assert(retained.result(index).status == rts::DIRECT_PATH_INVALID_INPUT);
		assert(retained.result(index).rawPointCount == 0);
	}
	assert(rts::GetDeterministicDirectPathLateDrainExecutionCount() == lateBefore + 2);
	assert(runtime.jobs.metrics().submittedJobCount == before.submittedJobCount + 5);
	assert(runtime.jobs.metrics().executedJobCount == before.executedJobCount + 5);
	assert(runtime.jobs.metrics().ownerHelpCount == before.ownerHelpCount);
}

void TestNativeDirectLateDrainSlotBeforeGroupTerminal()
{
	// Entry pause covers a helper never entered. Active pause is after the
	// original cancellation check: the real direct helper runs on release,
	// but its cancelled request must never regain publication authority.
	const unsigned pausePoints[] = {1, 2};
	for (const unsigned pausePoint : pausePoints)
	{
		NativePathWorkerScope runtime;
		DirectFixture first(0, 0, 4, 0), second(0, 1, 4, 1);
		second.snapshot.requestToken = 74;
		second.snapshot.objectId = 100;
		const rts::DirectPathSnapshot inputs[] = {first.snapshot, second.snapshot};
		const auto before = runtime.jobs.metrics();
		const unsigned lateBefore = rts::GetDeterministicDirectPathLateDrainExecutionCount();
		rts::DeterministicDirectPathBatch retained, busy, later;
		rts_direct_path_set_test_pause_mask(pausePoint);
		assert(!retained.executeSynchronously(runtime.jobs, inputs, 2, 1));
		assert(retained.executionSnapshot().timedOut);
		assert(retained.executionSnapshot().submittedJobCount == 2);
		assert(!busy.executeSynchronously(runtime.jobs, inputs, 2, 1));
		assert(busy.executionSnapshot().submittedJobCount == 0);
		for (unsigned index = 0; index != 2; ++index)
			assert(retained.requestExecutionSnapshot(index).state == rts::DIRECT_PATH_EXECUTION_CANCELLED);

		rts_job_system_set_test_pause_mask(32768);
		rts_direct_path_release_test_pause(pausePoint);
		assert(rts_job_system_wait_for_test_pause(32768, 5000));
		const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (runtime.jobs.metrics().executedJobCount != before.executedJobCount + 2 &&
			std::chrono::steady_clock::now() < releaseDeadline)
			std::this_thread::yield();
		assert(runtime.jobs.metrics().executedJobCount == before.executedJobCount + 2);
		rts_direct_path_set_test_pause_mask(0);
		// The actual executed count advances only after each job destructor.
		// Both retained jobs have freed the path slot, while scheduler finish
		// remains held before terminal/outstanding publication.
		assert(runtime.jobs.outstandingJobCount() == 2);
		assert(rts::GetDeterministicDirectPathLateDrainExecutionCount() == lateBefore + 2);
		for (unsigned index = 0; index != 2; ++index)
			assert(retained.result(index).status == rts::DIRECT_PATH_INVALID_INPUT);
		NativePathFinalizerRelease release(runtime.jobs);
		const bool laterCompleted = later.executeSynchronously(runtime.jobs,
			inputs, 2, 10000);
		release.join();
		assert(laterCompleted && later.executionSnapshot().submittedJobCount == 2);
		WaitForNativePathDrain(runtime.jobs);
		for (unsigned index = 0; index != 2; ++index)
		{
			assert(later.result(index).status == rts::DIRECT_PATH_FOUND);
			assert(retained.requestExecutionSnapshot(index).state == rts::DIRECT_PATH_EXECUTION_CANCELLED);
			assert(!retained.requestExecutionSnapshot(index).succeeded);
			assert(retained.result(index).rawPointCount == 0);
		}
		assert(runtime.jobs.metrics().submittedJobCount == before.submittedJobCount + 4);
		assert(runtime.jobs.metrics().executedJobCount == before.executedJobCount + 4);
		assert(runtime.jobs.metrics().ownerHelpCount == before.ownerHelpCount);
	}
}

void TestNativeOrdinaryLateDrainSlotBeforeGroupTerminal()
{
	// Both entry and active-before-loop cancellation are real existing seams.
	// Neither proves a completed ordinary search/materialization after timeout.
	const unsigned pausePoints[] = {4, 8};
	for (const unsigned pausePoint : pausePoints)
	{
		NativePathWorkerScope runtime;
		OrdinaryFixture input(40, 40, 3210);
		const auto request = input.makeRequest(0);
		const auto before = runtime.jobs.metrics();
		const unsigned lateBefore = rts::GetDeterministicOrdinaryPathLateDrainExecutionCount();
		rts::DeterministicOrdinaryPathBatch retained, busy, later;
		rts_direct_path_set_test_pause_mask(pausePoint);
		assert(!retained.executeSynchronously(runtime.jobs, input.grid, &request, 1, 1));
		assert(retained.executionSnapshot().timedOut);
		assert(retained.executionSnapshot().submittedRangeJobCount == 1);
		assert(!busy.executeSynchronously(runtime.jobs, input.grid, &request, 1, 1));
		assert(busy.executionSnapshot().submittedRangeJobCount == 0);

		rts_job_system_set_test_pause_mask(32768);
		rts_direct_path_release_test_pause(pausePoint);
		assert(rts_job_system_wait_for_test_pause(32768, 5000));
		rts_direct_path_set_test_pause_mask(0);
		assert(runtime.jobs.outstandingJobCount() == 1);
		assert(rts::GetDeterministicOrdinaryPathLateDrainExecutionCount() == lateBefore + 1);
		assert(retained.result(0).status == rts::DETERMINISTIC_PATH_INVALID_INPUT);
		NativePathFinalizerRelease release(runtime.jobs);
		const bool laterCompleted = later.executeSynchronously(runtime.jobs,
			input.grid, &request, 1, 10000);
		release.join();
		assert(laterCompleted && later.executionSnapshot().submittedRangeJobCount == 1);
		assert(later.result(0).status == rts::DETERMINISTIC_PATH_FOUND);
		WaitForNativePathDrain(runtime.jobs);
		assert(!retained.requestExecutionSnapshot(0).succeeded);
		assert(retained.result(0).pointCount == 0);
		assert(runtime.jobs.metrics().submittedJobCount == before.submittedJobCount + 2);
		assert(runtime.jobs.metrics().executedJobCount == before.executedJobCount + 2);
		assert(runtime.jobs.metrics().ownerHelpCount == before.ownerHelpCount);
	}
}

// Actual ordinary native source/consumer controls, without a copied executor or quota.

unsigned g_nativeOrdinaryBudgetFailures = 0;
const char *g_nativeOrdinaryBudgetRole = "unset";
const char *g_nativeOrdinaryBudgetCase = "unset";

bool NativeOrdinaryBudgetExpect(bool value, const char *message)
{
	if (!value)
	{
		++g_nativeOrdinaryBudgetFailures;
		std::printf("FAIL ordinary-budget case=%s role=%s: %s\n",
			g_nativeOrdinaryBudgetCase, g_nativeOrdinaryBudgetRole, message);
	}
	return value;
}

struct NativeOrdinaryBudgetObservations
{
	static constexpr unsigned Maximum = 2050, A = 512, B = 2048, C = 2049;
	std::array<std::atomic<unsigned>, Maximum> searches{}, materializers{}, grants{}, granted{}, refunds{}, imports{}, faults{};
	std::array<rts::performance::KernelPerformanceRequestBudget, Maximum> budgets{};
	std::array<rts::performance::KernelPerformanceRangeProgress, 4> progress{};
	std::array<unsigned, 4> releasedRanges{};
	std::array<std::atomic<unsigned>, rts::DETERMINISTIC_ORDINARY_PATH_TEST_BODY_EXIT + 1> sites{};
	std::atomic<unsigned> physicalReserves{0}, detachedCalls{0};
	std::atomic<bool> failed{false}, middle1Done{false}, middle2Done{false};
	std::atomic<bool> bAtGrant{false}, aGranted{false}, bRefused{false}, aRefunded{false};
	bool baseline, pressure;
	rts_test::NativeKernelClock &clock;
	const std::chrono::steady_clock::time_point deadline;

	NativeOrdinaryBudgetObservations(bool consume, bool large, rts_test::NativeKernelClock &time) :
		baseline(consume), pressure(large), clock(time),
		deadline(std::chrono::steady_clock::now() + std::chrono::seconds(5)) {}
	~NativeOrdinaryBudgetObservations() { releaseAll(); }
	unsigned count() const { return pressure ? Maximum : 1; }
	unsigned rangeCount() const { return pressure ? 4 : 1; }
	void releaseAll() noexcept
	{
		middle1Done.store(true, std::memory_order_release);
		middle2Done.store(true, std::memory_order_release);
		bAtGrant.store(true, std::memory_order_release);
		aGranted.store(true, std::memory_order_release);
		bRefused.store(true, std::memory_order_release);
		aRefunded.store(true, std::memory_order_release);
	}
	void waitFor(std::atomic<bool> &flag) noexcept
	{
		while (!flag.load(std::memory_order_acquire) && !failed.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		if (!flag.load(std::memory_order_acquire))
		{
			failed.store(true, std::memory_order_release);
			releaseAll();
		}
	}
	static void observe(void *opaque, rts::DeterministicOrdinaryPathTestEvent site,
		unsigned range, std::size_t request, std::size_t bytes, bool actualGranted)
	{
		using namespace rts;
		auto &self = *static_cast<NativeOrdinaryBudgetObservations *>(opaque);
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_REFERENCE_SERIAL_ENTER)
		{
			++self.detachedCalls;
			return; // This event has no range/request identity.
		}
		if (site <= DETERMINISTIC_ORDINARY_PATH_TEST_REFERENCE_SERIAL_ENTER ||
			site > DETERMINISTIC_ORDINARY_PATH_TEST_BODY_EXIT || request >= self.count() ||
			range >= self.rangeCount() ||
			JobSystem::instance().isCurrentThread(JOB_OWNER_GAME) != self.baseline)
		{ self.failed = true; self.releaseAll(); return; }
		const unsigned literalRange = !self.pressure || request < 513 ? 0 :
			request < 1026 ? 1 : request < 1538 ? 2 : 3;
		if (range != literalRange) { self.failed = true; self.releaseAll(); return; }
		++self.sites[site];
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_ENTER) ++self.searches[request];
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_ENTER) ++self.materializers[request];
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_PHYSICAL_RESERVE) ++self.physicalReserves;
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_GRANT ||
			site == DETERMINISTIC_ORDINARY_PATH_TEST_PHYSICAL_RESERVE ||
			site == DETERMINISTIC_ORDINARY_PATH_TEST_GRANT_RETURNED ||
			site == DETERMINISTIC_ORDINARY_PATH_TEST_REFUND_COMPLETED)
			if (bytes != 65536) self.failed = true;

		// Source-only actual-site interleaving; no owner waits in the consumer.
		if (self.pressure && !self.baseline && site == DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_GRANT)
		{
			if (request == A)
			{ self.waitFor(self.middle1Done); self.waitFor(self.middle2Done); self.waitFor(self.bAtGrant); }
			if (request == B)
			{ self.bAtGrant.store(true, std::memory_order_release); self.waitFor(self.aGranted); }
			if (request == C) self.waitFor(self.aRefunded);
		}
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_GRANT_RETURNED)
		{
			++self.grants[request];
			if (actualGranted) ++self.granted[request];
			if (actualGranted != (!self.pressure || request != B)) self.failed = true;
			if (self.pressure && request == A) self.aGranted.store(true, std::memory_order_release);
			if (self.pressure && request == B) self.bRefused.store(true, std::memory_order_release);
		}
		if (self.pressure && site == DETERMINISTIC_ORDINARY_PATH_TEST_FIRST_OUTPUT_ALLOCATION && request == A)
		{
			if (!self.baseline) self.waitFor(self.bRefused);
			if (self.faults[request].fetch_add(1) != 0) self.failed = true;
			throw std::bad_alloc(); // Same independently selected actual site in both roles.
		}
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_REFUND_COMPLETED)
		{
			++self.refunds[request];
			if (!self.pressure || request != A || bytes != 65536) self.failed = true;
			if (request == A) self.aRefunded.store(true, std::memory_order_release);
		}
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_EXIT && request == 1025)
			self.middle1Done.store(true, std::memory_order_release);
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_EXIT && request == 1537)
			self.middle2Done.store(true, std::memory_order_release);
		rts::JobMetricCounter ticks = self.pressure ? 1 : 31 + static_cast<unsigned>(site);
		if (!self.pressure && site == DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_ENTER) ticks = 7;
		if (!self.pressure && site == DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_EXIT) ticks = 11;
		if (!self.pressure && site == DETERMINISTIC_ORDINARY_PATH_TEST_BODY_EXIT) ticks = 5;
		self.clock.now.fetch_add(ticks);
	}
	static void importedBudget(void *opaque, const rts::performance::KernelPerformanceRangePlan &range,
		const rts::performance::KernelPerformanceRequestBudget &actual)
	{
		auto &self = *static_cast<NativeOrdinaryBudgetObservations *>(opaque);
		const auto request = actual.requestOrdinal;
		if (!rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) ||
			request >= self.count() || request < range.begin || request >= range.end)
		{ self.failed = true; self.releaseAll(); return; }
		if (self.imports[request].fetch_add(1) != 0) self.failed = true;
		self.budgets[request] = actual; // Observe actual native POD; never construct one.
	}
	static void importedRange(void *opaque, const rts::performance::KernelPerformanceRangePlan &range,
		const rts::performance::KernelPerformanceRangeProgress &actual)
	{
		auto &self = *static_cast<NativeOrdinaryBudgetObservations *>(opaque);
		static constexpr unsigned begins[] = {0, 513, 1026, 1538};
		static constexpr unsigned ends[] = {513, 1026, 1538, 2050};
		const unsigned i = range.rangeOrdinal;
		if (!rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) || i >= self.rangeCount() ||
			range.begin != (self.pressure ? begins[i] : 0) ||
			range.end != (self.pressure ? ends[i] : 1) ||
			range.operationCount != range.end - range.begin)
		{ self.failed = true; self.releaseAll(); return; }
		if (++self.releasedRanges[i] != 1) self.failed = true;
		self.progress[i] = actual;
	}
};

void CheckNativeOrdinaryBudgetBodyControls(const NativeOrdinaryBudgetObservations &observed)
{
	using namespace rts;
	NativeOrdinaryBudgetExpect(!observed.failed.load(), "actual sites retain owner, request, byte and rendezvous invariants");
	bool once = true, actualGrants = true, actualRefunds = true, actualFaults = true;
	unsigned grantCount = 0, refundCount = 0;
	for (unsigned i = 0; i != observed.count(); ++i)
	{
		once = once && observed.searches[i] == 1 && observed.materializers[i] == 1 && observed.grants[i] == 1;
		actualGrants = actualGrants && observed.granted[i] == (observed.pressure && i == 2048 ? 0U : 1U);
		actualRefunds = actualRefunds && observed.refunds[i] == (observed.pressure && i == 512 ? 1U : 0U);
		actualFaults = actualFaults && observed.faults[i] == (observed.pressure && i == 512 ? 1U : 0U);
		grantCount += observed.granted[i]; refundCount += observed.refunds[i];
	}
	NativeOrdinaryBudgetExpect(once, "each literal request searches, materializes and reaches grant exactly once");
	NativeOrdinaryBudgetExpect(actualGrants && grantCount == (observed.pressure ? 2049U : 1U),
		"actual grant results preserve the literal refusal rather than an empty consumer quota");
	NativeOrdinaryBudgetExpect(actualRefunds && actualFaults && refundCount == (observed.pressure ? 1U : 0U),
		"one selected real allocation exception reaches the actual catch/refund once");
	NativeOrdinaryBudgetExpect(observed.physicalReserves == (observed.baseline ? 0U : observed.count()),
		"only physical source executes the real result-budget CAS helper");
	NativeOrdinaryBudgetExpect(observed.detachedCalls == 0, "no detached ordinary serial oracle is executed");
	if (!observed.pressure)
	{
		bool sitesOnce = true;
		for (unsigned site = DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_ENTER;
			site <= DETERMINISTIC_ORDINARY_PATH_TEST_BODY_EXIT; ++site)
		{
			const unsigned expected = site == DETERMINISTIC_ORDINARY_PATH_TEST_REFUND_COMPLETED ||
				(observed.baseline && site == DETERMINISTIC_ORDINARY_PATH_TEST_PHYSICAL_RESERVE) ? 0 : 1;
			sitesOnce = sitesOnce && observed.sites[site] == expected;
		}
		NativeOrdinaryBudgetExpect(sitesOnce, "all whole-materializer segments and search/body boundaries are observed once");
	}
	std::printf("ordinary-budget case=%s role=%s actual requests=%u grants=%u refunds=%u physicalReserves=%u\n",
		g_nativeOrdinaryBudgetCase, g_nativeOrdinaryBudgetRole, observed.count(), grantCount, refundCount,
		observed.physicalReserves.load());
}

void CheckNativeOrdinaryReleasedBudgets(const NativeOrdinaryBudgetObservations &observed)
{
	using namespace rts::performance;
	bool matched = !observed.failed.load();
	std::uint64_t requested = 0, granted = 0, refunded = 0, consumed = 0;
	unsigned retainedCount = 0, refusedCount = 0, refundedCount = 0;
	for (unsigned i = 0; i != observed.count(); ++i)
	{
		const bool refund = observed.pressure && i == 512, refuse = observed.pressure && i == 2048;
		const auto &b = observed.budgets[i];
		matched = matched && observed.imports[i] == 1 && b.requestOrdinal == i &&
			b.grantSite == 1 && b.localGrantOrdinal == 1 && b.requestedBytes == 65536 &&
			b.grantedBytes == (refuse ? 0U : 65536U) && b.refundSite == (refund ? 2U : 0U) &&
			b.localRefundOrdinal == (refund ? 2U : 0U) && b.refundedBytes == (refund ? 65536U : 0U) &&
			b.consumedBytes == (refund || refuse ? 0U : 65536U) &&
			b.disposition == (refund ? KERNEL_REQUEST_BUDGET_REFUNDED :
				refuse ? KERNEL_REQUEST_BUDGET_REFUSED : KERNEL_REQUEST_BUDGET_RETAINED);
		requested += b.requestedBytes; granted += b.grantedBytes;
		refunded += b.refundedBytes; consumed += b.consumedBytes;
		retainedCount += b.disposition == KERNEL_REQUEST_BUDGET_RETAINED;
		refusedCount += b.disposition == KERNEL_REQUEST_BUDGET_REFUSED;
		refundedCount += b.disposition == KERNEL_REQUEST_BUDGET_REFUNDED;
	}
	NativeOrdinaryBudgetExpect(matched, "every actual released request POD matches literal identity, sites and settlement once");
	NativeOrdinaryBudgetExpect(requested == (observed.pressure ? 134348800ULL : 65536ULL) &&
		granted == (observed.pressure ? 134283264ULL : 65536ULL) &&
		refunded == (observed.pressure ? 65536ULL : 0ULL) &&
		consumed == (observed.pressure ? 134217728ULL : 65536ULL) &&
		retainedCount == (observed.pressure ? 2048U : 1U) &&
		refusedCount == (observed.pressure ? 1U : 0U) && refundedCount == (observed.pressure ? 1U : 0U),
		"actual settlements preserve exact charge/refusal/refund/retained totals");
	static constexpr unsigned completed[] = {512, 513, 512, 512};
	bool progressMatched = true;
	for (unsigned i = 0; i != observed.rangeCount(); ++i)
	{
		const auto &p = observed.progress[i].checkpoint;
		progressMatched = progressMatched && observed.releasedRanges[i] == 1 && p.entered && p.errors == 0 &&
			p.completedWorkUnits == (observed.pressure ? completed[i] : 1U) && p.firstTruePoll == 0 &&
			p.terminal == (observed.pressure && i == 0 ? KERNEL_RANGE_FAILED : KERNEL_RANGE_COMPLETED);
	}
	NativeOrdinaryBudgetExpect(progressMatched, "actual fence progress preserves completed requests and the one failed materialization range");
}

bool RunNativeOrdinaryBudgetRole(rts_test::NativeKernelTrace &trace, bool baseline, bool pressure)
{
	using namespace rts::performance;
	g_nativeOrdinaryBudgetRole = baseline ? "consumer" : "source";
	g_nativeOrdinaryBudgetCase = pressure ? "pressure2050" : "one-success";
	const unsigned before = g_nativeOrdinaryBudgetFailures;
	NativePathWorkerScope runtime; // Existing fixed four-worker owner fixture.
	rts_test::NativeKernelOwnerRun run;
	if (!NativeOrdinaryBudgetExpect(run.begin(trace, baseline, 3207, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND),
		"actual scoped source/consumer ledgers start")) return false;
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PATH, 0);
	if (!NativeOrdinaryBudgetExpect(attempt.valid(), "core owner opens authentic ordinary attempt before preflight")) return false;
	auto timing = run.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 3207, 1);
	KernelPerformanceReferenceBatch validated;
	NativeOrdinaryQuotaFixture input(pressure ? 2050 : 1);
	auto observed = std::make_unique<NativeOrdinaryBudgetObservations>(baseline, pressure, run.clock);
	rts::DeterministicOrdinaryPathTestHooks hooks;
	hooks.context = observed.get();
	hooks.observeRequest = NativeOrdinaryBudgetObservations::observe;
	hooks.observeReleasedBudget = NativeOrdinaryBudgetObservations::importedBudget;
	hooks.observeReleasedRange = NativeOrdinaryBudgetObservations::importedRange;
	rts::DeterministicOrdinaryPathBatch batch;
	const bool accepted = batch.executeSynchronously(runtime.jobs, input.grid,
		input.requests.data(), input.requests.size(), 10000, &timing, &run.reference,
		&validated, &hooks, attempt);
	// Cleanup before every native result/protocol assertion, including timeout.
	observed->releaseAll();
	WaitForNativePathDrain(runtime.jobs); // Cleanup only; collector must prove its own real group boundary.
	const unsigned controlsBefore = g_nativeOrdinaryBudgetFailures;
	const auto native = batch.executionSnapshot();
	NativeOrdinaryBudgetExpect(accepted == !pressure && native.completed == accepted && !native.timedOut,
		"literal native outcome is success or allocation-abort, never a timeout/retry");
	NativeOrdinaryBudgetExpect(native.referenceAdmissionAccepted,
		"ordinary path retains authenticated admission for physical and baseline-inline execution");
	NativeOrdinaryBudgetExpect(native.requestCount == input.requests.size() &&
		native.rangeCount == (pressure ? 4U : 1U) && native.grainSize == (pressure ? 513U : 1U),
		"actual native canonical range shape matches the literal fixture");
	CheckNativeOrdinaryBudgetBodyControls(*observed);
	if (!baseline)
	{
		NativeOrdinaryBudgetExpect(native.submittedRangeJobCount == (pressure ? 4U : 1U) &&
			native.workerExecutedRangeJobCount == (pressure ? 3U : 1U) &&
			native.failedRangeJobCount == (pressure ? 1U : 0U) &&
			native.resultStorageBytes == (pressure ? 134217728U : 65536U),
			"real source jobs retain exact physical result charge and one failed range only for pressure");
	}
	else
		NativeOrdinaryBudgetExpect(native.submittedRangeJobCount == 0 && native.workerExecutedRangeJobCount == 0 &&
			native.ownerExecutedRangeJobCount == 0 && native.physicalWorkerMask == 0 && native.resultStorageBytes == 0,
			"baseline has no fabricated worker, owner-help, physical mask or local quota counter");
	unsigned ownerAccepts = 0;
	std::vector<rts::DeterministicPathPoint> ownerPoints;
	if (!pressure && accepted)
	{
		const auto commit = run.timing.beginInterval(timing, KERNEL_PERFORMANCE_COMMIT);
		const auto actual = batch.result(0);
		AssertLiteralNativeOrdinaryQuotaPath(actual, input.requests[0]);
		ownerPoints.assign(actual.points, actual.points + actual.pointCount);
		++ownerAccepts; // Actual core-owner result acceptance, not a gameplay commit claim.
		run.clock.now.fetch_add(17);
		NativeOrdinaryBudgetExpect(run.timing.endInterval(commit), "real owner result copy closes its commit interval");
	}
	if (pressure)
	{
		bool gettersEmpty = true;
		for (unsigned i = 0; i != 2050; ++i)
			gettersEmpty = gettersEmpty && batch.result(i).pointCount == 0 && !batch.requestExecutionSnapshot(i).succeeded;
		NativeOrdinaryBudgetExpect(gettersEmpty && ownerAccepts == 0 && ownerPoints.empty(),
			"aborted batch publishes no discarded result or fabricated core fallback");
	}
	else
		NativeOrdinaryBudgetExpect(ownerAccepts == 1 && ownerPoints.size() == 3277 &&
			ownerPoints.front().x == 0 && ownerPoints.back().x == 3276,
			"core owner accepts the literal actual native path exactly once");
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	NativeOrdinaryBudgetExpect(scheduler.submittedJobs == (baseline ? 0U : pressure ? 4U : 1U) &&
		scheduler.executedJobs == scheduler.submittedJobs && scheduler.ownerHelpJobs == 0 &&
		scheduler.pendingJobs == 0 && scheduler.outstandingJobs == 0,
		"actual scheduler closure is physical only for source and fully drained for both roles");
	const bool controlsValid = g_nativeOrdinaryBudgetFailures == controlsBefore;
	std::printf("ordinary-budget case=%s role=%s native-control-failures=%u\n",
		g_nativeOrdinaryBudgetCase, g_nativeOrdinaryBudgetRole,
		g_nativeOrdinaryBudgetFailures - controlsBefore);

	// FIRST RED starts here: collection is an explicit fail-closed native stub,
	// and the old anonymous validated-batch route cannot link a traced attempt.
	const bool collected = batch.collectPerformanceReference(runtime.jobs);
	NativeOrdinaryBudgetExpect(collected, "native owner collects its genuinely terminal reference work");
	NativeOrdinaryBudgetExpect(validated.valid() == accepted,
		"native output is linked to its authentic attempt only on actual success");
	bool canonicalClosed = !accepted;
	if (validated.valid()) canonicalClosed = run.reference.finishBatch(validated, accepted);
	if (accepted && validated.valid())
		NativeOrdinaryBudgetExpect(canonicalClosed, "actual owner acceptance closes canonical batch before attempt finish");
	const auto disposition = accepted ? KERNEL_PERFORMANCE_COMMITTED : KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	NativeOrdinaryBudgetExpect(run.timing.endBatch(timing, disposition), "timing records actual native owner disposition");
	if (!controlsValid || !collected || (accepted && (!validated.valid() || !canonicalClosed)))
	{
		// Keep first source rows independent without manufacturing an empty trace
		// or weakening a failed assertion. Consumer runs only after genuine source closure.
		run.reference.freeze();
		NativeOrdinaryBudgetExpect(run.closeTiming(scheduler), "failed protocol row still closes its own actual timing run");
		return false;
	}
	CheckNativeOrdinaryReleasedBudgets(*observed);
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = disposition; finish.reasonSchema = 1; finish.reason = accepted ? 1 : 2;
	finish.validatedBatch = validated;
	NativeOrdinaryBudgetExpect(run.reference.finishAttempt(attempt, finish), "actual native outcome closes authentic attempt");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	reap.pendingJobs = scheduler.pendingJobs; reap.outstandingJobs = scheduler.outstandingJobs;
	NativeOrdinaryBudgetExpect(run.reference.reapAttempt(attempt, reap), "reference reap follows native terminal collection");
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.closeTiming(scheduler);
	NativeOrdinaryBudgetExpect(sealed && snapshot.errors == 0 && snapshot.trace.complete &&
		snapshot.complete == accepted && snapshot.streamCount == (accepted ? 1U : 0U),
		"source or consumer closes the actual native trace without a synthetic successful stream");
	NativeOrdinaryBudgetExpect(snapshot.trace.attemptCount == 1 && snapshot.trace.capturedAttemptCount == 1 &&
		snapshot.trace.capturedOperationCount == input.requests.size() && snapshot.trace.dispatchCount == 1 &&
		snapshot.trace.rangeCount == observed->rangeCount() && snapshot.trace.releasedRangeCount == observed->rangeCount() &&
		snapshot.trace.reapCount == 1, "native capture, dispatch, releases and reap have exact cardinalities");
	NativeOrdinaryBudgetExpect(timingClosed, "real owner phase and scheduler boundary reconcile");
	if (baseline && timingClosed)
		NativeOrdinaryBudgetExpect(run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_LEGACY_MUTABLE_ISLAND].pureNanoseconds ==
			(pressure ? 0U : 23U), "whole materialization is serial; successful search alone contributes literal pure ticks");
	const bool complete = g_nativeOrdinaryBudgetFailures == before && snapshot.trace.complete;
	std::printf("ordinary-budget case=%s role=%s failures=%u traceComplete=%u\n",
		g_nativeOrdinaryBudgetCase, g_nativeOrdinaryBudgetRole, g_nativeOrdinaryBudgetFailures - before,
		snapshot.trace.complete ? 1U : 0U);
	if (!baseline && complete) trace.source = snapshot;
	return complete; // Batch/payload dies here BEFORE constructing consumer outputs.
}

bool TestNativeOrdinaryBudgetSourceConsumers()
{
	const unsigned before = g_nativeOrdinaryBudgetFailures;
	for (unsigned variant = 0; variant != 2; ++variant)
	{
		rts_test::NativeKernelTrace trace(70 + variant);
		if (RunNativeOrdinaryBudgetRole(trace, false, variant != 0))
			RunNativeOrdinaryBudgetRole(trace, true, variant != 0);
	}
	return g_nativeOrdinaryBudgetFailures == before;
}

void TestNativeOrdinarySourceCollectionFailureRejectsCompletion()
{
	using namespace rts::performance;
	NativePathWorkerScope runtime;
	rts_test::NativeKernelTrace trace(72);
	rts_test::NativeKernelOwnerRun run;
	assert(run.begin(trace, false, 3208, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND));
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PATH, 0);
	assert(attempt.valid());
	auto timing = run.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 3208, 1);
	assert(timing.valid());
	NativeOrdinaryQuotaFixture input(1);
	rts::DeterministicOrdinaryPathBatch batch;
	rts_direct_path_set_test_fault_mask(16);
	const bool completed = batch.executeSynchronously(runtime.jobs, input.grid,
		input.requests.data(), input.requests.size(), 10000, &timing,
		&run.reference, nullptr, nullptr, attempt);
	rts_direct_path_set_test_fault_mask(0);
	WaitForNativePathDrain(runtime.jobs);
	assert(!completed);
	assert(!batch.executionSnapshot().completed);
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1;
	finish.reason = 2;
	assert(run.reference.finishAttempt(attempt, finish));
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	KernelPerformanceAttemptReap reap = {};
	reap.reasonSchema = 1;
	reap.reason = 1;
	reap.pendingJobs = scheduler.pendingJobs;
	reap.outstandingJobs = scheduler.outstandingJobs;
	assert(run.reference.reapAttempt(attempt, reap));
	assert(run.timing.endBatch(timing,
		KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION));
	assert(run.reference.sealObservationWindow());
	assert(run.reference.sealExecutionClosure());
	const auto snapshot = run.reference.freeze();
	assert(snapshot.errors == 0 && snapshot.trace.complete &&
		snapshot.trace.abortedAfterAdmissionAttemptCount == 1 &&
		snapshot.trace.reapCount == 1);
	assert(run.closeTiming(scheduler));
}

// Traced version of the existing real 2049-request native quota control.
struct NativeCompletedQuotaObservation
{
	static constexpr unsigned Count = 2049, Refused = 2048;
	std::array<std::atomic<unsigned>, Count> searches{}, materializers{}, grants{}, granted{};
	std::array<unsigned, Count> imports{}, views{};
	std::array<rts::performance::KernelPerformanceRequestBudget, Count> budgets{};
	std::array<std::size_t, Count> searchPoints{}, storedPoints{}, canonicalPoints{};
	std::array<bool, Count> canonicalHasPoints{};
	std::array<rts::DeterministicPathSearchStatus, Count> statuses{};
	std::array<unsigned, 4> releases{};
	std::array<rts::performance::KernelPerformanceRangeProgress, 4> progress{};
	std::array<std::atomic<bool>, 3> prefixesDone{};
	std::atomic<unsigned> physicalReserves{0}, refunds{0}, detachedCalls{0};
	std::atomic<bool> failed{false};
	bool baseline, nativeControlsChecked = false;
	rts_test::NativeKernelClock &clock;
	rts::DeterministicOrdinaryPathBatch &batch;
	const std::chrono::steady_clock::time_point deadline;

	NativeCompletedQuotaObservation(bool consume, rts_test::NativeKernelClock &time,
		rts::DeterministicOrdinaryPathBatch &native) : baseline(consume), clock(time), batch(native),
		deadline(std::chrono::steady_clock::now() + std::chrono::seconds(5)) {}
	~NativeCompletedQuotaObservation() { releaseAll(); }
	void releaseAll() noexcept
	{ for (auto &ready : prefixesDone) ready.store(true, std::memory_order_release); }
	void waitFor(std::atomic<bool> &ready) noexcept
	{
		while (!ready.load(std::memory_order_acquire) && !failed.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		if (!ready.load(std::memory_order_acquire)) { failed = true; releaseAll(); }
	}
	static unsigned rangeOf(std::size_t request)
	{ return request < 513 ? 0 : request < 1025 ? 1 : request < 1537 ? 2 : 3; }
	static void observe(void *context, rts::DeterministicOrdinaryPathTestEvent site,
		unsigned range, std::size_t request, std::size_t bytes, bool actualGranted)
	{
		using namespace rts;
		auto &self = *static_cast<NativeCompletedQuotaObservation *>(context);
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_REFERENCE_SERIAL_ENTER)
		{ ++self.detachedCalls; return; }
		if (request >= Count || range != rangeOf(request) ||
			JobSystem::instance().isCurrentThread(JOB_OWNER_GAME) != self.baseline)
		{ self.failed = true; self.releaseAll(); return; }
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_SEARCH_ENTER) ++self.searches[request];
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_ENTER) ++self.materializers[request];
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_PHYSICAL_RESERVE) ++self.physicalReserves;
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_REFUND_COMPLETED) ++self.refunds;
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_BEFORE_GRANT && request == Refused && !self.baseline)
			for (auto &ready : self.prefixesDone) self.waitFor(ready);
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_GRANT_RETURNED)
		{
			++self.grants[request];
			if (actualGranted) ++self.granted[request];
			if (bytes != 65536 || actualGranted != (request != Refused)) self.failed = true;
		}
		if (site == DETERMINISTIC_ORDINARY_PATH_TEST_MATERIALIZATION_EXIT)
		{
			if (request == 512) self.prefixesDone[0].store(true, std::memory_order_release);
			if (request == 1024) self.prefixesDone[1].store(true, std::memory_order_release);
			if (request == 1536) self.prefixesDone[2].store(true, std::memory_order_release);
		}
		++self.clock.now;
	}
	static void importedBudget(void *context, const rts::performance::KernelPerformanceRangePlan &range,
		const rts::performance::KernelPerformanceRequestBudget &actual)
	{
		auto &self = *static_cast<NativeCompletedQuotaObservation *>(context);
		if (!rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) ||
			actual.requestOrdinal >= Count || actual.requestOrdinal < range.begin ||
			actual.requestOrdinal >= range.end)
		{ self.failed = true; return; }
		if (++self.imports[actual.requestOrdinal] != 1) self.failed = true;
		self.budgets[actual.requestOrdinal] = actual;
	}
	static void importedRange(void *context, const rts::performance::KernelPerformanceRangePlan &range,
		const rts::performance::KernelPerformanceRangeProgress &actual)
	{
		auto &self = *static_cast<NativeCompletedQuotaObservation *>(context);
		static constexpr unsigned begins[] = {0, 513, 1025, 1537}, ends[] = {513, 1025, 1537, 2049};
		if (!rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) || range.rangeOrdinal >= 4 ||
			range.begin != begins[range.rangeOrdinal] || range.end != ends[range.rangeOrdinal])
		{ self.failed = true; return; }
		if (++self.releases[range.rangeOrdinal] != 1) self.failed = true;
		self.progress[range.rangeOrdinal] = actual;
	}
	void checkNativeControls()
	{
		using namespace rts;
		using namespace rts::performance;
		const unsigned before = g_nativeOrdinaryBudgetFailures;
		nativeControlsChecked = true;
		const auto native = batch.executionSnapshot();
		NativeOrdinaryBudgetExpect(!failed && native.completed && !native.timedOut &&
			native.requestCount == Count && native.rangeCount == 4 && native.grainSize == 513,
			"completed quota fixture has real native completion and canonical partition");
		NativeOrdinaryBudgetExpect(native.referenceAdmissionAccepted,
			"completed ordinary path retains authenticated admission in both roles");
		NativeOrdinaryBudgetExpect(native.submittedRangeJobCount == (baseline ? 0U : 4U) &&
			native.workerExecutedRangeJobCount == (baseline ? 0U : 4U) &&
			native.ownerExecutedRangeJobCount == 0 && native.failedRangeJobCount == 0 &&
			native.resultStorageBytes == (baseline ? 0U : 134217728U) &&
			(!baseline || native.physicalWorkerMask == 0),
			"completed quota source is physically charged; baseline has no fabricated work or counter");
		unsigned found = 0, refused = 0, actualGrants = 0;
		std::uint64_t actualStoredPoints = 0, actualConsumedBytes = 0;
		bool eachOnce = true, outputs = true, settlements = true, progressValid = true;
		for (unsigned i = 0; i != Count; ++i)
		{
			const auto result = batch.result(i);
			const auto execution = batch.requestExecutionSnapshot(i);
			const bool isRefused = i == Refused;
			eachOnce = eachOnce && searches[i] == 1 && materializers[i] == 1 && grants[i] == 1 &&
				granted[i] == (isRefused ? 0U : 1U);
			actualGrants += granted[i]; actualStoredPoints += result.pointCount;
			outputs = outputs && execution.succeeded && execution.submitted == !baseline &&
				execution.state == (baseline ? DIRECT_PATH_EXECUTION_INLINE : DIRECT_PATH_EXECUTION_WORKER) &&
				(!baseline || execution.physicalWorkerIndex == JOB_INVALID_PHYSICAL_WORKER_INDEX) &&
				result.expandedNodeCount == 2 && result.discoveredNodeCount == 3277 &&
				result.objectId == 1000 + i;
			if (isRefused)
			{
				++refused;
				outputs = outputs && result.status == DETERMINISTIC_PATH_BUDGET_EXHAUSTED &&
					result.points == nullptr && result.pointCount == 0 && result.allocationCount == 0 &&
					result.cleanupCount == 0 && result.passableBlockCount == 0 && result.materializationPlanHash == 0;
			}
			else
			{
				++found;
				outputs = outputs && result.status == DETERMINISTIC_PATH_FOUND && result.points != nullptr &&
					result.pointCount == 3277 && result.allocationCount == 3277 && result.cleanupCount == 3276 &&
					result.passableBlockCount == 0 && result.materializationPlanHash != 0;
				if (result.points != nullptr && result.pointCount == 3277)
					outputs = outputs && result.points[0].x == 0 && result.points[3276].x == 3276;
			}
			const auto &budget = budgets[i];
			settlements = settlements && imports[i] == 1 && budget.requestOrdinal == i &&
				budget.grantSite == 1 && budget.localGrantOrdinal == 1 && budget.requestedBytes == 65536 &&
				budget.grantedBytes == (isRefused ? 0U : 65536U) && budget.refundSite == 0 &&
				budget.localRefundOrdinal == 0 && budget.refundedBytes == 0 &&
				budget.consumedBytes == budget.grantedBytes && budget.disposition ==
				(isRefused ? KERNEL_REQUEST_BUDGET_REFUSED : KERNEL_REQUEST_BUDGET_RETAINED);
			actualConsumedBytes += budget.consumedBytes;
		}
		for (unsigned i = 0; i != 4; ++i)
		{
			const auto &actual = progress[i];
			progressValid = progressValid && releases[i] == 1 && actual.checkpoint.entered &&
				actual.checkpoint.errors == 0 && actual.checkpoint.firstTruePoll == 0 &&
				actual.checkpoint.completedWorkUnits == (i == 0 ? 513U : 512U) &&
				actual.checkpoint.terminal == KERNEL_RANGE_COMPLETED && actual.publication == KERNEL_PUBLICATION_PUBLISHED;
		}
		NativeOrdinaryBudgetExpect(eachOnce && actualGrants == 2048 && refunds == 0 && detachedCalls == 0 &&
			physicalReserves == (baseline ? 0U : Count), "all 2049 actual native bodies execute once, with 2048 grants and no refund");
		NativeOrdinaryBudgetExpect(outputs && found == 2048 && refused == 1 && actualStoredPoints == 6711296ULL,
			"native retained/materialized outputs are truthful even though the refused search had a path");
		NativeOrdinaryBudgetExpect(settlements && actualConsumedBytes == 134217728ULL && progressValid,
			"actual completed ranges and request settlements close without a synthetic range failure");
		std::printf("ordinary-completed-quota role=%s native-control-failures=%u requests=2049 grants=%u refunds=%u storedPoints=%llu consumedBytes=%llu\n",
			g_nativeOrdinaryBudgetRole, g_nativeOrdinaryBudgetFailures - before, actualGrants, refunds.load(),
			static_cast<unsigned long long>(actualStoredPoints), static_cast<unsigned long long>(actualConsumedBytes));
		std::fflush(stdout);
	}
	static void referenceView(void *context, std::size_t request, rts::DeterministicPathSearchStatus status,
		std::size_t searchCount, std::size_t storedCount, std::size_t canonicalCount, bool hasPoints)
	{
		auto &self = *static_cast<NativeCompletedQuotaObservation *>(context);
		if (!rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME) || request >= Count)
		{ self.failed = true; return; }
		if (++self.views[request] != 1) self.failed = true;
		self.searchPoints[request] = searchCount; self.storedPoints[request] = storedCount;
		self.canonicalPoints[request] = canonicalCount; self.canonicalHasPoints[request] = hasPoints;
		self.statuses[request] = status;
		if (request == Refused)
		{
			// The real join and native range validation already completed. This
			// read-only callback returns, allowing the REAL canonical writer next.
			self.checkNativeControls();
			bool matched = true;
			std::uint64_t searchTotal = 0, storedTotal = 0, canonicalTotal = 0;
			for (unsigned i = 0; i != Count; ++i)
			{
				const bool refused = i == Refused;
				matched = matched && self.views[i] == 1 && self.searchPoints[i] == 3277 &&
					self.storedPoints[i] == (refused ? 0U : 3277U) &&
					self.canonicalPoints[i] == self.storedPoints[i] && self.canonicalHasPoints[i] == !refused &&
					self.statuses[i] == (refused ? rts::DETERMINISTIC_PATH_BUDGET_EXHAUSTED : rts::DETERMINISTIC_PATH_FOUND);
				searchTotal += self.searchPoints[i]; storedTotal += self.storedPoints[i]; canonicalTotal += self.canonicalPoints[i];
			}
			NativeOrdinaryBudgetExpect(matched && searchTotal == 6714573ULL && storedTotal == 6711296ULL &&
				canonicalTotal == storedTotal, "actual canonical view uses retained points, not unmaterialized search count");
			std::printf("ordinary-completed-quota role=%s canonical-view request=2048 search=%zu stored=%zu canonical=%zu hasPoints=%u; returning to actual canonical writer\n",
				g_nativeOrdinaryBudgetRole, searchCount, storedCount, canonicalCount, hasPoints ? 1U : 0U);
			std::fflush(stdout);
		}
		++self.clock.now;
	}
};

bool RunNativeCompletedQuotaRole(rts_test::NativeKernelTrace &trace, bool baseline)
{
	using namespace rts::performance;
	g_nativeOrdinaryBudgetRole = baseline ? "consumer" : "source";
	g_nativeOrdinaryBudgetCase = "completed-refusal2049";
	const unsigned before = g_nativeOrdinaryBudgetFailures;
	NativePathWorkerScope runtime;
	rts_test::NativeKernelOwnerRun run;
	if (!NativeOrdinaryBudgetExpect(run.begin(trace, baseline, 3207, KERNEL_PHASE_LEGACY_MUTABLE_ISLAND),
		"completed-refusal actual owner run starts")) return false;
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PATH, 0);
	if (!NativeOrdinaryBudgetExpect(attempt.valid(), "completed-refusal authentic attempt opens")) return false;
	auto timing = run.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 3207, 1);
	KernelPerformanceReferenceBatch validated;
	NativeOrdinaryQuotaFixture input(2049);
	rts::DeterministicOrdinaryPathBatch batch;
	auto observed = std::make_unique<NativeCompletedQuotaObservation>(baseline, run.clock, batch);
	rts::DeterministicOrdinaryPathTestHooks hooks;
	hooks.context = observed.get(); hooks.observeRequest = NativeCompletedQuotaObservation::observe;
	hooks.observeReleasedBudget = NativeCompletedQuotaObservation::importedBudget;
	hooks.observeReleasedRange = NativeCompletedQuotaObservation::importedRange;
	hooks.observeReferenceResult = NativeCompletedQuotaObservation::referenceView;
	std::printf("ordinary-completed-quota role=%s entering actual native execute and canonical validation\n", g_nativeOrdinaryBudgetRole);
	std::fflush(stdout);
	const bool accepted = batch.executeSynchronously(runtime.jobs, input.grid, input.requests.data(), input.requests.size(),
		10000, &timing, &run.reference, &validated, &hooks, attempt);
	observed->releaseAll();
	WaitForNativePathDrain(runtime.jobs);
	NativeOrdinaryBudgetExpect(accepted && observed->nativeControlsChecked && validated.valid(),
		"completed native quota refusal reaches real safe canonical validation and authentic linkage");
	if (!observed->nativeControlsChecked) observed->checkNativeControls();
	NativeOrdinaryBudgetExpect(batch.collectPerformanceReference(runtime.jobs), "completed-refusal real collection is already terminal and once-only");
	std::uint64_t ownerReadout = 0;
	unsigned ownerFound = 0, ownerRefused = 0;
	if (accepted)
	{
		const auto commit = run.timing.beginInterval(timing, KERNEL_PERFORMANCE_COMMIT);
		for (unsigned i = 0; i != 2049; ++i)
		{
			const auto result = batch.result(i);
			ownerReadout += result.objectId + result.pointCount;
			ownerFound += result.status == rts::DETERMINISTIC_PATH_FOUND;
			ownerRefused += result.status == rts::DETERMINISTIC_PATH_BUDGET_EXHAUSTED;
		}
		// Actual core owner consumes every advisory result/status once. This is
		// not gameplay path authority or a made-up fallback for the refused row.
		++run.clock.now;
		NativeOrdinaryBudgetExpect(run.timing.endInterval(commit) && ownerReadout == 10858472ULL &&
			ownerFound == 2048 && ownerRefused == 1,
			"core owner consumes the truthful completed batch once");
	}
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	NativeOrdinaryBudgetExpect(scheduler.submittedJobs == (baseline ? 0U : 4U) &&
		scheduler.executedJobs == scheduler.submittedJobs && scheduler.ownerHelpJobs == 0 &&
		scheduler.pendingJobs == 0 && scheduler.outstandingJobs == 0, "completed-refusal actual scheduler is drained with no baseline jobs");
	const bool canonicalClosed = validated.valid() && run.reference.finishBatch(validated, accepted);
	NativeOrdinaryBudgetExpect(canonicalClosed, "actual core-owner readout closes canonical batch before attempt finish");
	NativeOrdinaryBudgetExpect(run.timing.endBatch(timing, accepted ? KERNEL_PERFORMANCE_COMMITTED :
		KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION), "completed-refusal timing records actual native outcome");
	if (!accepted || !validated.valid() || !canonicalClosed)
	{
		run.reference.freeze();
		NativeOrdinaryBudgetExpect(run.closeTiming(scheduler), "failed canonical row still closes its own timing run");
		return false;
	}
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_COMMITTED; finish.reasonSchema = 1; finish.reason = 1; finish.validatedBatch = validated;
	NativeOrdinaryBudgetExpect(run.reference.finishAttempt(attempt, finish), "truthful completed-refusal attempt commits after actual owner readout");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	reap.pendingJobs = scheduler.pendingJobs; reap.outstandingJobs = scheduler.outstandingJobs;
	NativeOrdinaryBudgetExpect(run.reference.reapAttempt(attempt, reap), "completed-refusal reap follows actual native terminal collection");
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	NativeOrdinaryBudgetExpect(sealed && snapshot.errors == 0 && snapshot.complete && snapshot.trace.complete &&
		snapshot.streamCount == 1 && snapshot.streams[0].validatedOperationCount == 2049 &&
		snapshot.streams[0].committedOperationCount == 2049 && snapshot.trace.capturedOperationCount == 2049 &&
		snapshot.trace.rangeCount == 4 && snapshot.trace.releasedRangeCount == 4 && snapshot.trace.reapCount == 1,
		"completed-refusal real source/consumer trace closes with truthful successful output stream");
	NativeOrdinaryBudgetExpect(run.closeTiming(scheduler), "completed-refusal actual phase and scheduler closure reconcile");
	const bool complete = g_nativeOrdinaryBudgetFailures == before && snapshot.trace.complete;
	std::printf("ordinary-completed-quota role=%s failures=%u traceComplete=%u\n", g_nativeOrdinaryBudgetRole,
		g_nativeOrdinaryBudgetFailures - before, snapshot.trace.complete ? 1U : 0U);
	if (!baseline && complete) trace.source = snapshot;
	return complete; // All source output storage dies before consumer construction.
}

bool TestNativeCompletedQuotaSourceConsumer()
{
	// Same process-local modal-dialog suppression as JobSystemTest. A genuine
	// access violation is still a failing CTest process, never an expected pass.
	struct ErrorModeScope
	{
		UINT prior;
		ErrorModeScope() : prior(SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)) {}
		~ErrorModeScope() { SetErrorMode(prior); }
	} errorMode;
	rts_test::NativeKernelTrace trace(72);
	return RunNativeCompletedQuotaRole(trace, false) && RunNativeCompletedQuotaRole(trace, true);
}

// These are intentionally narrow lifecycle probes.  The title queue owns the
// batch context beyond processPathfindQueue's stack frame, so a timed-out
// source must remain collectible after the synchronous call has returned.
void TestNativeDirectRetainedTitleContext()
{
	using namespace rts::performance;
	NativePathWorkerScope runtime;
	rts_test::NativeKernelTrace trace(73);
	rts_test::NativeKernelOwnerRun run;
	assert(run.begin(trace, false, 3207, KERNEL_PHASE_SPATIAL_WORK));
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PATH, 1);
	assert(attempt.valid());
	auto timing = run.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 1, 3207, 1);
	assert(timing.valid());
	KernelPerformanceReferenceBatch validated;
	DirectFixture first(0, 0, 4, 0), second(0, 1, 4, 1);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const rts::DirectPathSnapshot inputs[] = {first.snapshot, second.snapshot};

	std::unique_ptr<rts::DeterministicDirectPathBatch> retained(
		new rts::DeterministicDirectPathBatch());
	rts_direct_path_set_test_pause_mask(1);
	assert(!retained->executeSynchronously(runtime.jobs, inputs, 2, 1,
		&timing, &run.reference, &validated, attempt));
	assert(retained->executionSnapshot().timedOut);
	assert(retained->executionSnapshot().submittedJobCount == 2);
	assert(retained->executionSnapshot().referenceAdmissionAccepted);
	// The title context stays alive while the workers finish the cancelled
	// group; its copied source record must not be a stack-local dependency.
	rts_direct_path_release_test_pause(1);
	rts_direct_path_set_test_pause_mask(0);
	WaitForNativePathDrain(runtime.jobs);
	assert(retained->collectPerformanceReference(runtime.jobs));
	assert(!validated.valid());
	retained.reset();

	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1;
	finish.reason = 2;
	assert(run.timing.endBatch(timing,
		KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION));
	assert(run.reference.finishAttempt(attempt, finish));
	KernelPerformanceAttemptReap reap = {};
	reap.reasonSchema = 1;
	reap.reason = 1;
	reap.dynamicFactsKnownMask = 7;
	reap.pendingJobs = runtime.jobs.pendingOwnerCompletionCount();
	reap.outstandingJobs = runtime.jobs.outstandingJobCount();
	reap.activeSlots = 0;
	assert(run.reference.reapAttempt(attempt, reap));
	assert(run.reference.sealObservationWindow());
	assert(run.reference.sealExecutionClosure());
	const auto snapshot = run.reference.freeze();
	assert(snapshot.errors == 0 && snapshot.trace.reapCount == 1);
	assert(run.closeTiming(rts_test::NativeKernelSchedulerBoundary()));
}

bool RunNativeDirectReferenceRole(rts_test::NativeKernelTrace &trace,
	bool baseline)
{
	using namespace rts::performance;
	NativePathWorkerScope runtime;
	rts_test::NativeKernelOwnerRun run;
	if (!run.begin(trace, baseline, 3209, KERNEL_PHASE_SPATIAL_WORK))
		return false;
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PATH, 1);
	auto timing = run.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 1, 3209, 1);
	KernelPerformanceReferenceBatch validated;
	DirectFixture first(3, 4, 11, 7), second(17, 9, 10, 14);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const rts::DirectPathSnapshot inputs[] = {first.snapshot, second.snapshot};
	rts::DeterministicDirectPathBatch batch;
	const bool completed = attempt.valid() && timing.valid() &&
		batch.executeSynchronously(runtime.jobs, inputs, 2, 10000,
			&timing, &run.reference, &validated, attempt);
	const auto execution = batch.executionSnapshot();
	assert(completed && execution.completed && !execution.timedOut &&
		execution.referenceAdmissionAccepted);
	assert(execution.submittedJobCount == (baseline ? 0U : 2U));
	assert(baseline ? execution.workerExecutedJobCount == 0U :
		execution.workerExecutedJobCount == 2U);
	assert(validated.valid());
	AssertBatchResultMatchesSerial(first, batch, 0);
	AssertBatchResultMatchesSerial(second, batch, 1);
	const auto commit = run.timing.beginInterval(timing,
		KERNEL_PERFORMANCE_COMMIT);
	run.clock.now.fetch_add(11);
	assert(run.timing.endInterval(commit));
	assert(run.reference.finishBatch(validated, true));
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_COMMITTED;
	finish.reasonSchema = 1;
	finish.reason = 1;
	finish.validatedBatch = validated;
	assert(run.reference.finishAttempt(attempt, finish));
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	KernelPerformanceAttemptReap reap = {};
	reap.reasonSchema = 1;
	reap.reason = 1;
	reap.dynamicFactsKnownMask = 7;
	reap.pendingJobs = scheduler.pendingJobs;
	reap.outstandingJobs = scheduler.outstandingJobs;
	reap.activeSlots = 0;
	assert(run.reference.reapAttempt(attempt, reap));
	assert(run.timing.endBatch(timing, KERNEL_PERFORMANCE_COMMITTED));
	const bool sealed = run.reference.sealObservationWindow() &&
		run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.closeTiming(scheduler);
	const bool complete = sealed && timingClosed && snapshot.complete &&
		snapshot.trace.complete && snapshot.errors == 0 &&
		snapshot.streamCount == 1 &&
		snapshot.streams[0].kernel == KERNEL_PERFORMANCE_PATH &&
		snapshot.streams[0].subtype == 1 &&
		snapshot.trace.admittedAttemptCount == 1 &&
		snapshot.trace.reapCount == 1;
	if (!baseline && complete)
		trace.source = snapshot;
	else if (baseline && complete)
		assert(snapshot.streams[0].inputDigest.equals(
			trace.source.streams[0].inputDigest) &&
			snapshot.streams[0].outputDigest.equals(
				trace.source.streams[0].outputDigest) &&
			snapshot.streams[0].commitDigest.equals(
				trace.source.streams[0].commitDigest));
	return complete;
}

bool TestNativeDirectReferenceSourceConsumer()
{
	rts_test::NativeKernelTrace trace(75);
	return RunNativeDirectReferenceRole(trace, false) &&
		RunNativeDirectReferenceRole(trace, true);
}

void TestNativeDirectReferenceAllocationFailuresFallBack()
{
	using namespace rts::performance;
	static constexpr unsigned allocationFailures[] = {32U, 64U, 128U};
	for (const unsigned faultMask : allocationFailures)
	{
		NativePathWorkerScope runtime;
		rts_test::NativeKernelTrace trace(76 + faultMask);
		rts_test::NativeKernelClock clock;
		trace.options.clock = rts_test::NativeKernelClock::read;
		trace.options.clockContext = &clock;
		KernelPerformanceReferenceLedger reference;
		assert(reference.beginRun(trace.options));
		const KernelPerformanceAttemptIdentity identity = {
			KERNEL_PERFORMANCE_PATH, 1, 1, 0,
			KERNEL_PHASE_SPATIAL_WORK, 3211};
		const KernelPerformanceAttempt attempt = reference.beginAttempt(identity);
		assert(attempt.valid());

		DirectFixture first(3, 4, 11, 7), second(17, 9, 10, 14);
		second.snapshot.requestToken = 74;
		second.snapshot.objectId = 100;
		const rts::DirectPathSnapshot inputs[] = {first.snapshot, second.snapshot};
		rts::DeterministicDirectPathBatch batch;
		bool exceptionEscaped = false;
		bool completed = true;
		rts_direct_path_set_test_fault_mask(faultMask);
		try
		{
			completed = batch.executeSynchronously(runtime.jobs, inputs, 2, 1000,
				nullptr, &reference, nullptr, attempt);
		}
		catch (...)
		{
			exceptionEscaped = true;
		}
		rts_direct_path_set_test_fault_mask(0);
		assert(!exceptionEscaped);
		assert(!completed);
		const rts::DeterministicDirectPathBatchExecutionSnapshot execution =
			batch.executionSnapshot();
		assert(!execution.completed && !execution.timedOut);
		assert(!execution.referenceAdmissionAccepted);
		assert(execution.submittedJobCount == 0);
	}
}

void TestNativeOrdinaryRetainedTitleContext()
{
	using namespace rts::performance;
	NativePathWorkerScope runtime;
	rts_test::NativeKernelTrace trace(74);
	rts_test::NativeKernelOwnerRun run;
	assert(run.begin(trace, false, 3207, KERNEL_PHASE_SPATIAL_WORK));
	const auto attempt = run.beginAttempt(KERNEL_PERFORMANCE_PATH, 0);
	assert(attempt.valid());
	auto timing = run.timing.beginBatch(KERNEL_PERFORMANCE_PATH, 0, 3207, 1);
	assert(timing.valid());
	KernelPerformanceReferenceBatch validated;
	NativeOrdinaryQuotaFixture input(1);
	std::unique_ptr<rts::DeterministicOrdinaryPathBatch> retained(
		new rts::DeterministicOrdinaryPathBatch());
	rts_direct_path_set_test_pause_mask(4);
	assert(!retained->executeSynchronously(runtime.jobs, input.grid,
		input.requests.data(), input.requests.size(), 1, &timing, &run.reference,
		&validated, nullptr, attempt));
	assert(retained->executionSnapshot().timedOut);
	assert(retained->executionSnapshot().submittedRangeJobCount == 1);
	rts_direct_path_release_test_pause(4);
	rts_direct_path_set_test_pause_mask(0);
	WaitForNativePathDrain(runtime.jobs);
	assert(retained->collectPerformanceReference(runtime.jobs));
	assert(!validated.valid());
	retained.reset();

	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION;
	finish.reasonSchema = 1;
	finish.reason = 2;
	assert(run.timing.endBatch(timing,
		KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION));
	assert(run.reference.finishAttempt(attempt, finish));
	KernelPerformanceAttemptReap reap = {};
	reap.reasonSchema = 1;
	reap.reason = 1;
	reap.dynamicFactsKnownMask = 7;
	reap.pendingJobs = runtime.jobs.pendingOwnerCompletionCount();
	reap.outstandingJobs = runtime.jobs.outstandingJobCount();
	reap.activeSlots = 0;
	assert(run.reference.reapAttempt(attempt, reap));
	assert(run.reference.sealObservationWindow());
	assert(run.reference.sealExecutionClosure());
	const auto snapshot = run.reference.freeze();
	assert(snapshot.errors == 0 && snapshot.trace.reapCount == 1);
	assert(run.closeTiming(rts_test::NativeKernelSchedulerBoundary()));
}

void TestNativePathNullOwnerTeardown()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	assert(jobs.start(config));
	assert(jobs.registerCurrentThread(rts::JOB_OWNER_GAME));
	DirectFixture first(0, 0, 4, 0), second(0, 1, 4, 1);
	second.snapshot.requestToken = 74;
	second.snapshot.objectId = 100;
	const rts::DirectPathSnapshot inputs[] = {first.snapshot, second.snapshot};
	{
		std::unique_ptr<rts::DeterministicDirectPathBatch> context(
			new rts::DeterministicDirectPathBatch());
		assert(context->executeSynchronously(jobs, inputs, 2, 1000));
		assert(context->executionSnapshot().completed);
		// Teardown deliberately clears the owner only after scheduler shutdown;
		// destroying the retained title context must then be harmless and must
		// not attempt owner-only collection through a null identity.
		jobs.shutdown();
		assert(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME));
		context.reset();
	}
}

void TestDirectPathOwnerMixedOutcomeReceiptState()
{
	rts::DeterministicPathOwnerCompletion completion;
	completion.reset(2);
	completion.beginOperation();
	completion.finishOperation(true, false);
	completion.beginOperation();
	completion.finishOperation(false, false);
	assert(!completion.committed());
	const bool fallback = completion.beginLegacyFallback();
	assert(fallback && completion.fallbackEntered() &&
		!completion.fallbackCompleted());
	completion.completeLegacyFallback(fallback);
	assert(completion.fallbackCompleted());
}

void TestPathOwnerFullBatchCommitState()
{
	rts::DeterministicPathOwnerCompletion completion;
	completion.reset(2);
	completion.beginOperation();
	completion.finishOperation(true, false);
	completion.beginOperation();
	completion.finishOperation(true, false);
	assert(completion.committed());
	assert(!completion.fallbackEntered() &&
		!completion.fallbackCompleted());
}

void TestDirectPathOwnerNoFallbackReceiptState()
{
	rts::DeterministicPathOwnerCompletion completion;
	completion.reset(2);
	completion.beginOperation();
	completion.finishOperation(true, false);
	completion.beginOperation();
	completion.finishOperation(false, true);
	assert(!completion.committed());
	assert(!completion.beginLegacyFallback());
	assert(!completion.fallbackEntered() &&
		!completion.fallbackCompleted());
}

void TestOrdinaryPathOwnerMixedOutcomeReceiptState()
{
	rts::DeterministicPathOwnerCompletion completion;
	completion.reset(2);
	completion.beginOperation();
	completion.finishOperation(false, false);
	const bool fallback = completion.beginLegacyFallback();
	completion.completeLegacyFallback(fallback);
	completion.beginOperation();
	completion.finishOperation(true, false);
	assert(!completion.committed());
	assert(completion.fallbackEntered() && completion.fallbackCompleted());
}

void TestOrdinaryPathOwnerUnconsumedReceiptStateHasNoFallback()
{
	rts::DeterministicPathOwnerCompletion completion;
	completion.reset(2);
	completion.expectLegacyFallback();
	assert(!completion.committed());
	assert(!completion.fallbackEntered() &&
		!completion.fallbackCompleted());
}
#endif

} // namespace

int main()
{
#if defined(_MSC_VER)
	_set_error_mode(_OUT_TO_STDERR);
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	TestBoundedFreeCounterInvariants();
	TestConcurrentMultiWorkerAuthorityCorrelation();
	TestExactSupercoverDirectionsAndPostGoal();
	TestLegacyIteratorExhaustiveParity();
	TestExactRawChainAndPoolAccounting();
	TestMaxLengthCallbackUniquenessAndAccountingParity();
	TestSameCellAccounting();
	TestPoolShortageFallsBackBeforeMutation();
	TestAdvisoryFallbackStatusClassification();
	TestBridgeElsewhereAndConnectedCorridor();
	TestMalformedAndGenerationStaleResults();
	TestMismatchFallbacksArePreMutation();
	TestAuthorityPolicyAndRawStartAdmission();
	TestGeneralsRecordedPathAuthorityPolicy();
	TestPhysicalWorkerCountsAndOneWorkerParity();
#if defined(_WIN64)
	TestDirectPathBatchPerformanceLedgerStages();
	TestDirectPathReferenceModesAndCommitBoundary();
#endif
	TestBoundedBatchUsesMultiplePhysicalWorkers();
	TestBatchFailureTimeoutLateDrainAndStoppedFallback();
	TestOrdinaryLegacyWidthWrapAndBridgeFallback();
	TestOrdinaryOneWorkerSerialParity();
#if defined(_WIN64)
	TestOrdinaryPathReferenceModesAndCommitBoundary();
#endif
	TestOrdinaryAdaptiveLargeObstructedBatchAndFaults();
#if defined(_WIN64)
	TestNativeOrdinaryQuotaOneRequestLiteral();
	TestNativeOrdinaryQuotaRetains2048AndRefusesOne();
	TestNativeOrdinaryFiveRequestCanonicalRanges();
	TestNativeDirectEntryPauseWaitsForBothAdmittedWorkers();
	TestNativeDirectLateDrainSlotBeforeGroupTerminal();
	TestNativeOrdinaryLateDrainSlotBeforeGroupTerminal();
	if (!TestNativeOrdinaryBudgetSourceConsumers()) return 1;
	TestNativeOrdinarySourceCollectionFailureRejectsCompletion();
	if (!TestNativeCompletedQuotaSourceConsumer()) return 1;
	if (!TestNativeDirectReferenceSourceConsumer()) return 1;
	TestNativeDirectReferenceAllocationFailuresFallBack();
	TestNativeDirectRetainedTitleContext();
	TestNativeOrdinaryRetainedTitleContext();
	TestNativePathNullOwnerTeardown();
	TestDirectPathOwnerMixedOutcomeReceiptState();
	TestPathOwnerFullBatchCommitState();
	TestDirectPathOwnerNoFallbackReceiptState();
	TestOrdinaryPathOwnerMixedOutcomeReceiptState();
	TestOrdinaryPathOwnerUnconsumedReceiptStateHasNoFallback();
#endif
	std::puts("DeterministicPathSearchTest passed");
	return 0;
}
