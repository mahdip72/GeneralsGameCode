#include "Lib/DeterministicPathBatch.h"
#include "Lib/BoundedFreeCounter.h"
#include "Lib/JobSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#if defined(NDEBUG)
#error DeterministicPathSearchTest must run with assertions enabled
#endif

extern "C" void rts_direct_path_set_test_pause_mask(unsigned pauseMask);
extern "C" bool rts_direct_path_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds);
extern "C" bool rts_direct_path_wait_for_test_pause_count(
	unsigned pausePoint, unsigned requiredCount, unsigned timeoutMilliseconds);
extern "C" void rts_direct_path_release_test_pause(unsigned pausePoint);
extern "C" void rts_direct_path_set_test_fault_mask(unsigned faultMask);

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
	assert(largeExecution.peakActiveWorkers == 4);
	assert(rts::IsDeterministicOrdinaryPathConcurrentMultiWorkerBatch(
		largeExecution));

	rts_direct_path_set_test_fault_mask(4);
	rts::DeterministicOrdinaryPathBatch faulted;
	assert(!faulted.executeSynchronously(jobs, fixture.grid, requests.data(),
		4, 1000));
	assert(faulted.executionSnapshot().failedRangeJobCount != 0);
	rts_direct_path_set_test_fault_mask(0);

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

} // namespace

int main()
{
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
	TestPhysicalWorkerCountsAndOneWorkerParity();
	TestBoundedBatchUsesMultiplePhysicalWorkers();
	TestBatchFailureTimeoutLateDrainAndStoppedFallback();
	TestOrdinaryLegacyWidthWrapAndBridgeFallback();
	TestOrdinaryOneWorkerSerialParity();
	TestOrdinaryAdaptiveLargeObstructedBatchAndFaults();
	std::puts("DeterministicPathSearchTest passed");
	return 0;
}
