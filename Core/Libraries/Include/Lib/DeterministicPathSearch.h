/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace rts
{

enum DeterministicPathSearchStatus
{
	DETERMINISTIC_PATH_INVALID_INPUT = 0,
	DETERMINISTIC_PATH_SNAPSHOT_GENERATION_MISMATCH,
	DETERMINISTIC_PATH_UNSUPPORTED_SUBSET,
	DETERMINISTIC_PATH_SCRATCH_TOO_SMALL,
	DETERMINISTIC_PATH_BUDGET_EXHAUSTED,
	DETERMINISTIC_PATH_CELL_INFO_SHORTAGE,
	DETERMINISTIC_PATH_NO_PATH,
	DETERMINISTIC_PATH_OUTPUT_TOO_SMALL,
	DETERMINISTIC_PATH_FOUND
};

struct DeterministicPathCell
{
	std::uint32_t traversalMask;
	std::uint32_t obstacleObjectId;
	std::uint32_t positionObjectId;
	std::uint32_t goalObjectId;
	std::uint16_t blockZone;
	std::uint16_t globalZone;
	std::uint16_t zone;
	std::uint8_t type;
	std::uint8_t flags;
	std::uint8_t layer;
	std::uint8_t connectsToLayer;
	std::uint8_t pinched;
	std::uint8_t blockPassable;
	std::uint8_t navigationFlags;
	std::uint8_t reserved;
};

enum DeterministicPathCellSemantic
{
	DETERMINISTIC_PATH_CELL_CLEAR = 0,
	DETERMINISTIC_PATH_CELL_OBSTACLE = 4,
	DETERMINISTIC_PATH_NO_UNITS = 0,
	DETERMINISTIC_PATH_LAYER_INVALID = 0,
	DETERMINISTIC_PATH_LAYER_GROUND = 1
};

enum DeterministicPathNavigationFlag
{
	DETERMINISTIC_PATH_INSIDE_LOGICAL_EXTENT = 1 << 0,
	DETERMINISTIC_PATH_HAS_CELL_INFO = 1 << 1,
	DETERMINISTIC_PATH_BLOCKED_BY_ALLY = 1 << 2,
	DETERMINISTIC_PATH_METADATA_DIRTY = 1 << 3,
	DETERMINISTIC_PATH_BLOCK_INTERACTS_WITH_BRIDGE = 1 << 4
};

enum DeterministicPathHierarchyMode
{
	DETERMINISTIC_PATH_HIERARCHY_PREPUBLISHED = 0,
	DETERMINISTIC_PATH_HIERARCHY_GENERALS = 1,
	DETERMINISTIC_PATH_HIERARCHY_ZERO_HOUR = 2
};

const std::uint32_t DETERMINISTIC_PATH_INVALID_OBJECT_ID = 0xffffffffU;

struct ImmutableNavigationGrid
{
	// Backing storage must be completed before publication, remain immutable
	// for every reader, and outlive each search.  A future worker adapter must
	// publish this whole snapshot through its task queue synchronization; this
	// must never alias Pathfinder's mutable cells.
	const DeterministicPathCell *cells;
	std::uint32_t width;
	std::uint32_t height;
	std::int32_t originX;
	std::int32_t originY;
	std::uint32_t snapshotGeneration;
};

struct DeterministicPathRequest
{
	std::uint32_t expectedSnapshotGeneration;
	std::uint32_t objectId;
	std::int32_t startX;
	std::int32_t startY;
	std::int32_t goalX;
	std::int32_t goalY;
	std::uint32_t traversalMask;
	std::uint32_t maximumExpandedNodes;
	std::uint32_t availableCellInfoCount;
	std::uint16_t requiredZone;
	std::uint8_t footprintRadius;
	std::uint8_t centerInCell;
	std::uint8_t allowDiagonal;
	std::uint8_t allowBlockedStart;
	std::uint8_t expectedLayer;
	std::uint8_t requireLegacyDirectLine;
	std::uint8_t requireObstructedSearch;
	std::uint8_t isHuman;
	std::uint8_t hierarchyMode;
	std::uint8_t hierarchyBlockSize;
};

struct DeterministicPathPoint
{
	std::int32_t x;
	std::int32_t y;
	std::uint8_t layer;
	std::uint8_t reserved[3];
};

enum
{
	DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT = 8,
	DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS = 2048
};

enum DirectPathFactFlag
{
	DIRECT_PATH_FACT_CLEAR_GROUND = 1 << 0,
	DIRECT_PATH_FACT_HIERARCHY_PASSABLE = 1 << 1,
	DIRECT_PATH_FACT_INSIDE_LOGICAL_EXTENT = 1 << 2,
	DIRECT_PATH_FACT_FOOTPRINT_CLEAR = 1 << 3,
	DIRECT_PATH_FACT_NO_FOREIGN_OCCUPANCY = 1 << 4,
	DIRECT_PATH_FACT_NO_LAYER_CONNECTION = 1 << 5,
	DIRECT_PATH_FACT_NOT_PINCHED = 1 << 6,
	DIRECT_PATH_FACT_METADATA_CLEAN = 1 << 7
};

struct DirectPathCellFact
{
	std::int32_t x;
	std::int32_t y;
	std::uint16_t zone;
	std::uint8_t flags;
	std::uint8_t hasPathfindInfo;
};

// Owner-published, immutable input for one bounded direct-path request.  The
// callback array is the exact legacy supercover stream, including its initial
// start callback and any callback after the goal.  Only the eight start-ring
// facts are published in addition; no mutable grid or full-grid copy is shared.
struct DirectPathSnapshot
{
	const DirectPathCellFact *callbacks;
	std::size_t callbackCount;
	const DirectPathCellFact *startNeighbors;
	std::size_t startNeighborCount;
	std::uint32_t topologyOccupancyGeneration;
	std::uint32_t requestToken;
	std::uint32_t objectId;
	std::uint32_t availableCellInfoCount;
	std::int32_t startX;
	std::int32_t startY;
	std::int32_t goalX;
	std::int32_t goalY;
	std::uint16_t requiredZone;
	std::uint8_t expectedLayer;
	std::uint8_t reserved;
};

enum DirectPathSearchStatus
{
	DIRECT_PATH_INVALID_INPUT = 0,
	DIRECT_PATH_MALFORMED_SNAPSHOT,
	DIRECT_PATH_UNSUPPORTED_SUBSET,
	DIRECT_PATH_NO_PATH,
	DIRECT_PATH_OUTPUT_TOO_SMALL,
	DIRECT_PATH_CELL_INFO_SHORTAGE,
	DIRECT_PATH_FOUND
};

// The direct-path result is authoritative only in product/runtime epochs that
// explicitly support it.  This policy is kept pointer-free so focused tests can
// prove the multiplayer, recording and replay compatibility matrix.
struct DirectPathAuthorityPolicy
{
	bool parallelExecutionMode;
	bool shadowExecutionMode;
	bool zeroHourTitle;
	bool networkGame;
	bool multiplayerGame;
	bool multiplayerPolicyEnabled;
	bool recordingGame;
	bool replayGame;
	bool replayUsesCurrentPathEpoch;
	bool runtimeUsesCurrentGeneralsEpoch;
};

// Fixed pathfinding semantics are a native compatibility epoch, independent of
// whether request-local work is executed in parallel.  Keeping this policy
// pointer-free lets focused tests prove that legacy, network and unmarked
// replay sessions retain the original retail pathfinder.
struct FixedPathfindingSemanticsPolicy
{
	bool nativeRuntime;
	bool zeroHourTitle;
	bool networkGame;
	bool multiplayerGame;
	bool recordingGame;
	bool replayGame;
	bool replayUsesCurrentPathEpoch;
	bool runtimeUsesCurrentGeneralsEpoch;
};

struct DirectPathSearchResult
{
	DeterministicPathPoint *rawPoints;
	std::size_t rawPointCapacity;
	std::size_t rawPointCount;
	std::size_t callbackCount;
	std::size_t requiredCellInfoCount;
	std::size_t startNeighborAllocationCount;
	std::size_t openCellCountAfterGoal;
	std::size_t cumulativeCellCount;
	std::uint32_t topologyOccupancyGeneration;
	std::uint32_t requestToken;
	std::uint32_t objectId;
	DirectPathSearchStatus status;
};

static_assert(sizeof(DeterministicPathCell) == 32,
	"navigation snapshot cells must remain fixed-width POD");
static_assert(sizeof(DeterministicPathPoint) == 12,
	"worker path points must remain fixed-width POD");
static_assert(sizeof(DirectPathCellFact) == 12,
	"direct-path facts must remain compact fixed-width POD");

enum DeterministicPathSearchNodeState
{
	DETERMINISTIC_PATH_NODE_UNSEEN = 0,
	DETERMINISTIC_PATH_NODE_OPEN = 1,
	DETERMINISTIC_PATH_NODE_CLOSED = 2,
	DETERMINISTIC_PATH_NODE_TERMINAL = 3
};

struct DeterministicPathSearchNode
{
	std::uint32_t pathCost;
	std::uint32_t estimatedTotalCost;
	std::uint32_t parentIndex;
	std::uint32_t heapPosition;
	std::uint64_t insertionOrdinal;
	std::uint64_t discoveryOrdinal;
	std::uint64_t closeOrdinal;
	std::uint8_t state;
};

struct DeterministicPathSearchScratch
{
	DeterministicPathSearchNode *nodes;
	std::size_t nodeCapacity;
	std::uint32_t *heap;
	std::size_t heapCapacity;
	std::uint8_t *hierarchyPassableBlocks;
	std::size_t hierarchyPassableBlockCapacity;
};

struct DeterministicPathSearchResult
{
	DeterministicPathPoint *points;
	std::size_t pointCapacity;
	std::size_t pointCount;
	std::uint32_t snapshotGeneration;
	std::uint32_t expandedNodeCount;
	std::uint32_t discoveredNodeCount;
	std::uint32_t requiredCellInfoCount;
	std::uint32_t cumulativeCellCount;
	std::uint32_t *passableBlockIndices;
	std::size_t passableBlockCapacity;
	std::size_t passableBlockCount;
	std::uint8_t hierarchyAllPassable;
	DeterministicPathSearchStatus status;
};

// Allocation-free search over immutable POD. requireLegacyDirectLine selects
// the compact line-only lane. Otherwise the search reproduces the ordinary
// legacy ground A* subset, including direct-line injection at each expansion,
// stable equal-cost ordering, turn/pinched costs, hierarchy penalties and
// first-discovery neighbor behavior. Foreign live occupancy, layer transitions
// and other request-specific gameplay semantics are advisory unsupported
// outcomes so the owner can run the unchanged serial path before mutation.
DeterministicPathSearchStatus FindDeterministicPath(
	const ImmutableNavigationGrid &grid,
	const DeterministicPathRequest &request,
	DeterministicPathSearchScratch &scratch,
	DeterministicPathSearchResult &result) noexcept;

// Produces the exact callback coordinates used by Pathfinder's legacy line
// iterator.  The stream deliberately continues through the final loop
// iteration, so a shallow/steep line can contain one post-goal callback.
bool BuildLegacySupercoverCallbacks(std::int32_t startX, std::int32_t startY,
	std::int32_t goalX, std::int32_t goalY,
	DeterministicPathPoint *callbacks, std::size_t callbackCapacity,
	std::size_t &callbackCount) noexcept;

// Computes the bounded raw parent chain and exact legacy pool/list accounting
// plan without reading game state.  DIRECT_PATH_NO_PATH is advisory only: the
// owner must fall back to its unchanged serial search for every non-FOUND
// status.
DirectPathSearchStatus FindDeterministicDirectPath(
	const DirectPathSnapshot &snapshot,
	DirectPathSearchResult &result) noexcept;

// These outcomes are valid worker decisions for the conservative direct-path
// subset.  They select the unchanged serial fallback and are not evidence that
// snapshot/result validation failed.
bool IsDirectPathAdvisoryFallbackStatus(
	DirectPathSearchStatus status) noexcept;

bool IsDirectPathResultCurrent(const DirectPathSearchResult &result,
	std::uint32_t topologyOccupancyGeneration,
	std::uint32_t requestToken, std::uint32_t objectId) noexcept;

// Recomputes every raw-chain and pool/list accounting field from the immutable
// owner snapshot.  The owner must call this before allocating or linking any
// PathfindCellInfo; a false result is therefore a transaction-safe serial
// fallback rather than a partially materialized search.
bool IsDirectPathMaterializationPlanValid(const DirectPathSnapshot &snapshot,
	const DirectPathSearchResult &result,
	std::size_t currentAvailableCellInfoCount) noexcept;

bool IsDirectPathAuthorityAllowed(
	const DirectPathAuthorityPolicy &policy) noexcept;

bool IsFixedPathfindingSemanticsAllowed(
	const FixedPathfindingSemanticsPolicy &policy) noexcept;

bool IsOrdinaryPathAuthorityAllowed(
	const DirectPathAuthorityPolicy &policy,
	bool fixedPathfindingSemantics) noexcept;

// The owner performs exact request-local admission before copying the full
// navigation grid.  Parallel authority needs a real multi-request/multi-worker
// batch; shadow comparison needs one worker request.
bool ShouldCaptureOrdinaryNavigationSnapshot(std::size_t eligibleRequestCount,
	bool shadowMode, bool authorityMode, std::size_t workerCount) noexcept;

// Legacy clips a copy for cell lookup but validates the raw start when deciding
// whether tunneling/A* is required.  The bounded direct lane therefore accepts
// only an unchanged, raw-valid start.
bool IsDirectPathRawStartEligible(float rawX, float rawY, float rawZ,
	float clippedX, float clippedY, float clippedZ,
	bool rawMovementPositionValid) noexcept;

} // namespace rts
