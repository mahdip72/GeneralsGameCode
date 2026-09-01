/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicPathSearch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace rts
{
namespace
{

const std::uint32_t INVALID_NODE_INDEX = std::numeric_limits<std::uint32_t>::max();
const std::uint8_t NODE_UNSEEN = DETERMINISTIC_PATH_NODE_UNSEEN;
const std::uint8_t NODE_OPEN = DETERMINISTIC_PATH_NODE_OPEN;
const std::uint8_t NODE_CLOSED = DETERMINISTIC_PATH_NODE_CLOSED;
const std::uint8_t NODE_TERMINAL = DETERMINISTIC_PATH_NODE_TERMINAL;

struct StableOpenHeap
{
	DeterministicPathSearchNode *nodes;
	std::uint32_t *indices;
	std::size_t size;
	std::size_t capacity;
};

bool HasHigherPriority(const DeterministicPathSearchNode &left,
	const DeterministicPathSearchNode &right)
{
	if (left.estimatedTotalCost != right.estimatedTotalCost)
		return left.estimatedTotalCost < right.estimatedTotalCost;
	return left.insertionOrdinal < right.insertionOrdinal;
}

void SwapHeapEntries(StableOpenHeap &heap, std::size_t left, std::size_t right)
{
	const std::uint32_t leftNode = heap.indices[left];
	const std::uint32_t rightNode = heap.indices[right];
	heap.indices[left] = rightNode;
	heap.indices[right] = leftNode;
	heap.nodes[leftNode].heapPosition = static_cast<std::uint32_t>(right);
	heap.nodes[rightNode].heapPosition = static_cast<std::uint32_t>(left);
}

void SiftUp(StableOpenHeap &heap, std::size_t position)
{
	while (position > 0)
	{
		const std::size_t parent = (position - 1) / 2;
		if (!HasHigherPriority(heap.nodes[heap.indices[position]], heap.nodes[heap.indices[parent]]))
			break;
		SwapHeapEntries(heap, position, parent);
		position = parent;
	}
}

void SiftDown(StableOpenHeap &heap, std::size_t position)
{
	for (;;)
	{
		const std::size_t left = position * 2 + 1;
		if (left >= heap.size)
			return;
		const std::size_t right = left + 1;
		std::size_t best = left;
		if (right < heap.size &&
			HasHigherPriority(heap.nodes[heap.indices[right]], heap.nodes[heap.indices[left]]))
		{
			best = right;
		}
		if (!HasHigherPriority(heap.nodes[heap.indices[best]], heap.nodes[heap.indices[position]]))
			return;
		SwapHeapEntries(heap, position, best);
		position = best;
	}
}

bool Push(StableOpenHeap &heap, std::uint32_t nodeIndex)
{
	if (heap.size >= heap.capacity)
		return false;
	heap.indices[heap.size] = nodeIndex;
	heap.nodes[nodeIndex].heapPosition = static_cast<std::uint32_t>(heap.size);
	++heap.size;
	SiftUp(heap, heap.size - 1);
	return true;
}

void Remove(StableOpenHeap &heap, std::uint32_t nodeIndex)
{
	const std::size_t position = heap.nodes[nodeIndex].heapPosition;
	if (position >= heap.size || heap.indices[position] != nodeIndex)
		return;
	--heap.size;
	heap.nodes[nodeIndex].heapPosition = INVALID_NODE_INDEX;
	if (position == heap.size)
		return;
	heap.indices[position] = heap.indices[heap.size];
	heap.nodes[heap.indices[position]].heapPosition = static_cast<std::uint32_t>(position);
	if (position > 0)
	{
		const std::size_t parent = (position - 1) / 2;
		if (HasHigherPriority(heap.nodes[heap.indices[position]], heap.nodes[heap.indices[parent]]))
		{
			SiftUp(heap, position);
			return;
		}
	}
	SiftDown(heap, position);
}

std::uint32_t Pop(StableOpenHeap &heap)
{
	const std::uint32_t result = heap.indices[0];
	Remove(heap, result);
	return result;
}

bool IsInside(const ImmutableNavigationGrid &grid, std::int32_t x, std::int32_t y)
{
	return x >= grid.originX && y >= grid.originY &&
		static_cast<std::uint64_t>(static_cast<std::int64_t>(x) - grid.originX) <
			grid.width &&
		static_cast<std::uint64_t>(static_cast<std::int64_t>(y) - grid.originY) <
			grid.height;
}

std::uint32_t CellIndex(const ImmutableNavigationGrid &grid, std::int32_t x, std::int32_t y)
{
	return static_cast<std::uint32_t>(y - grid.originY) * grid.width +
		static_cast<std::uint32_t>(x - grid.originX);
}

std::int32_t CellX(const ImmutableNavigationGrid &grid, std::uint32_t index)
{
	return grid.originX + static_cast<std::int32_t>(index % grid.width);
}

std::int32_t CellY(const ImmutableNavigationGrid &grid, std::uint32_t index)
{
	return grid.originY + static_cast<std::int32_t>(index / grid.width);
}

std::uint32_t Heuristic(const DeterministicPathRequest &request,
	std::int32_t x, std::int32_t y)
{
	const std::uint32_t dx = static_cast<std::uint32_t>(
		x < request.goalX ? request.goalX - x : x - request.goalX);
	const std::uint32_t dy = static_cast<std::uint32_t>(
		y < request.goalY ? request.goalY - y : y - request.goalY);
	const std::uint32_t major = std::max(dx, dy);
	const std::uint32_t minor = std::min(dx, dy);
	// PathfindCell::costToGoal deliberately underestimates a diagonal by using
	// 10 * major + 5 * minor rather than the movement cost of 14.
	const std::uint64_t estimate = static_cast<std::uint64_t>(major) * 10U +
		static_cast<std::uint64_t>(minor) * 5U;
	return estimate > std::numeric_limits<std::uint32_t>::max()
		? std::numeric_limits<std::uint32_t>::max()
		: static_cast<std::uint32_t>(estimate);
}

bool IsTraversable(const DeterministicPathCell &cell, std::uint32_t traversalMask)
{
	return (cell.traversalMask & traversalMask) != 0;
}

DeterministicPathSearchStatus Finish(DeterministicPathSearchResult &result,
	DeterministicPathSearchStatus status);

bool IsOwnedOrEmpty(std::uint32_t value, std::uint32_t objectId)
{
	return value == DETERMINISTIC_PATH_INVALID_OBJECT_ID || value == objectId;
}

enum OrdinaryCellInspection
{
	ORDINARY_CELL_CLEAR = 0,
	ORDINARY_CELL_BLOCKED,
	ORDINARY_CELL_UNSUPPORTED
};

struct HierarchyPassability
{
	const std::uint8_t *blocks;
	std::size_t blockWidth;
	std::size_t blockHeight;
	std::uint8_t blockSize;
	bool allPassable;
	bool prepublished;
};

bool IsHierarchyPassable(const ImmutableNavigationGrid &grid,
	const HierarchyPassability &hierarchy, std::int32_t x, std::int32_t y)
{
	if (hierarchy.prepublished)
		return grid.cells[CellIndex(grid, x, y)].blockPassable != 0;
	if (hierarchy.allPassable)
		return true;
	const std::size_t localX = static_cast<std::size_t>(x - grid.originX);
	const std::size_t localY = static_cast<std::size_t>(y - grid.originY);
	const std::size_t blockX = localX / hierarchy.blockSize;
	const std::size_t blockY = localY / hierarchy.blockSize;
	return blockX < hierarchy.blockWidth && blockY < hierarchy.blockHeight &&
		hierarchy.blocks[blockY * hierarchy.blockWidth + blockX] != 0;
}

OrdinaryCellInspection InspectOrdinaryCell(const ImmutableNavigationGrid &grid,
	const DeterministicPathRequest &request, std::int32_t x, std::int32_t y,
	const HierarchyPassability &hierarchy, bool requireHierarchyPassable,
	bool rejectPinched)
{
	if (!IsInside(grid, x, y))
		return ORDINARY_CELL_BLOCKED;
	const DeterministicPathCell &center = grid.cells[CellIndex(grid, x, y)];
	if (center.layer != request.expectedLayer ||
		center.connectsToLayer != DETERMINISTIC_PATH_LAYER_INVALID ||
		(center.navigationFlags & (DETERMINISTIC_PATH_BLOCKED_BY_ALLY |
			DETERMINISTIC_PATH_METADATA_DIRTY)) != 0)
	{
		return ORDINARY_CELL_UNSUPPORTED;
	}
	if (request.isHuman != 0 &&
		(center.navigationFlags & DETERMINISTIC_PATH_INSIDE_LOGICAL_EXTENT) == 0)
	{
		return ORDINARY_CELL_BLOCKED;
	}
	if (!IsTraversable(center, request.traversalMask) ||
		(requireHierarchyPassable &&
			!IsHierarchyPassable(grid, hierarchy, x, y)) ||
		(rejectPinched && center.pinched != 0))
	{
		return ORDINARY_CELL_BLOCKED;
	}

	const std::int32_t radius = request.footprintRadius;
	const std::int32_t upper = request.centerInCell ? radius + 1 : radius;
	for (std::int32_t cellX = x - radius; cellX < x + upper; ++cellX)
	{
		for (std::int32_t cellY = y - radius; cellY < y + upper; ++cellY)
		{
			if (!IsInside(grid, cellX, cellY))
				return ORDINARY_CELL_BLOCKED;
			const DeterministicPathCell &cell =
				grid.cells[CellIndex(grid, cellX, cellY)];
			if (cell.layer != request.expectedLayer ||
				cell.connectsToLayer != DETERMINISTIC_PATH_LAYER_INVALID ||
				(cell.navigationFlags &
					(DETERMINISTIC_PATH_BLOCKED_BY_ALLY |
						DETERMINISTIC_PATH_METADATA_DIRTY)) != 0)
			{
				return ORDINARY_CELL_UNSUPPORTED;
			}
			if (!IsOwnedOrEmpty(cell.positionObjectId, request.objectId) ||
				!IsOwnedOrEmpty(cell.goalObjectId, request.objectId))
			{
				// Relationships, idle state and crushability are live object facts.
				// The immutable ordinary subset never guesses them.
				return ORDINARY_CELL_UNSUPPORTED;
			}
			if (cell.flags != DETERMINISTIC_PATH_NO_UNITS &&
				cell.positionObjectId != request.objectId &&
				cell.goalObjectId != request.objectId)
			{
				return ORDINARY_CELL_UNSUPPORTED;
			}
		}
	}
	return ORDINARY_CELL_CLEAR;
}

std::uint32_t AddCost(std::uint32_t left, std::uint32_t right)
{
	return left > std::numeric_limits<std::uint32_t>::max() - right
		? std::numeric_limits<std::uint32_t>::max() : left + right;
}

std::uint32_t StoreLegacyPathCost(std::uint32_t cost)
{
	// PathfindCellInfo stores both A* costs in UnsignedShort.  Every legacy
	// setter therefore truncates at the write boundary; later additions and
	// comparisons read the already-wrapped value back from the cell.
	return cost & std::numeric_limits<std::uint16_t>::max();
}

std::uint32_t LegacyNeighborCost(const ImmutableNavigationGrid &grid,
	const DeterministicPathSearchScratch &scratch, std::uint32_t childIndex,
	std::uint32_t parentIndex)
{
	const std::int32_t childX = CellX(grid, childIndex);
	const std::int32_t childY = CellY(grid, childIndex);
	const std::int32_t parentX = CellX(grid, parentIndex);
	const std::int32_t parentY = CellY(grid, parentIndex);
	const std::int32_t previousX = parentX - childX;
	const std::int32_t previousY = parentY - childY;
	std::uint32_t cost = AddCost(scratch.nodes[parentIndex].pathCost,
		previousX == 0 || previousY == 0 ? 10U : 14U);
	if (grid.cells[childIndex].pinched != 0)
		cost = AddCost(cost, 14U);

	const std::uint32_t grandparentIndex =
		scratch.nodes[parentIndex].parentIndex;
	if (grandparentIndex != INVALID_NODE_INDEX)
	{
		const std::int32_t grandparentX = CellX(grid, grandparentIndex);
		const std::int32_t grandparentY = CellY(grid, grandparentIndex);
		const std::int32_t directionX = grandparentX - parentX;
		const std::int32_t directionY = grandparentY - parentY;
		if (directionX != previousX || directionY != previousY)
		{
			const std::int32_t dot = directionX * previousX +
				directionY * previousY;
			cost = AddCost(cost, dot > 0 ? 4U : (dot == 0 ? 8U : 16U));
		}
	}
	return cost;
}

template <typename Visitor>
bool VisitLegacySupercover(std::int32_t startX, std::int32_t startY,
	std::int32_t goalX, std::int32_t goalY, Visitor &visitor)
{
	const std::int32_t deltaX = goalX >= startX ? goalX - startX : startX - goalX;
	const std::int32_t deltaY = goalY >= startY ? goalY - startY : startY - goalY;
	std::int32_t x = startX;
	std::int32_t y = startY;
	std::int32_t xinc1 = goalX >= startX ? 1 : -1;
	std::int32_t xinc2 = xinc1;
	std::int32_t yinc1 = goalY >= startY ? 1 : -1;
	std::int32_t yinc2 = yinc1;
	std::int32_t denominator;
	std::int32_t numerator;
	std::int32_t numeratorAdd;
	std::int32_t pixelCount;
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

	bool first = true;
	for (std::int32_t pixel = 0; pixel <= pixelCount; ++pixel)
	{
		if (!visitor(x, y, first))
			return false;
		first = false;
		numerator += numeratorAdd;
		if (numerator >= denominator)
		{
			numerator -= denominator;
			x += xinc1;
			y += yinc1;
			if (!visitor(x, y, first))
				return false;
			first = false;
		}
		x += xinc2;
		y += yinc2;
	}
	return true;
}

enum HierarchyBuildStatus
{
	HIERARCHY_BUILD_READY = 0,
	HIERARCHY_BUILD_UNSUPPORTED,
	HIERARCHY_BUILD_OUTPUT_TOO_SMALL
};

std::uint32_t HierarchicalDistance(const ImmutableNavigationGrid &grid,
	std::uint32_t leftIndex, std::uint32_t rightIndex)
{
	const double dx = static_cast<double>(CellX(grid, leftIndex)) -
		static_cast<double>(CellX(grid, rightIndex));
	const double dy = static_cast<double>(CellY(grid, leftIndex)) -
		static_cast<double>(CellY(grid, rightIndex));
	const double distance = std::sqrt(dx * dx + dy * dy);
	const double scaled = distance * 10.0 + 0.5;
	return scaled >= static_cast<double>(
		std::numeric_limits<std::uint32_t>::max()) ?
		std::numeric_limits<std::uint32_t>::max() :
		static_cast<std::uint32_t>(scaled);
}

HierarchyBuildStatus BuildHierarchyPassability(
	const ImmutableNavigationGrid &grid, const DeterministicPathRequest &request,
	DeterministicPathSearchScratch &scratch,
	DeterministicPathSearchResult &result, HierarchyPassability &hierarchy)
{
	hierarchy.blocks = nullptr;
	hierarchy.blockWidth = 0;
	hierarchy.blockHeight = 0;
	hierarchy.blockSize = request.hierarchyBlockSize;
	hierarchy.allPassable = false;
	hierarchy.prepublished = request.hierarchyMode ==
		DETERMINISTIC_PATH_HIERARCHY_PREPUBLISHED;
	result.passableBlockCount = 0;
	result.hierarchyAllPassable = 0;
	if (hierarchy.prepublished)
		return HIERARCHY_BUILD_READY;
	if ((request.hierarchyMode != DETERMINISTIC_PATH_HIERARCHY_GENERALS &&
		request.hierarchyMode != DETERMINISTIC_PATH_HIERARCHY_ZERO_HOUR) ||
		request.hierarchyBlockSize == 0 || grid.originX != 0 || grid.originY != 0)
	{
		return HIERARCHY_BUILD_UNSUPPORTED;
	}

	const std::size_t blockSize = request.hierarchyBlockSize;
	const std::size_t blockWidth =
		(static_cast<std::size_t>(grid.width) + blockSize - 1) / blockSize;
	const std::size_t blockHeight =
		(static_cast<std::size_t>(grid.height) + blockSize - 1) / blockSize;
	if (blockWidth == 0 || blockHeight == 0 ||
		blockWidth > std::numeric_limits<std::size_t>::max() / blockHeight)
	{
		return HIERARCHY_BUILD_UNSUPPORTED;
	}
	const std::size_t blockCount = blockWidth * blockHeight;
	if (scratch.hierarchyPassableBlocks == nullptr ||
		scratch.hierarchyPassableBlockCapacity < blockCount)
	{
		return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;
	}
	std::memset(scratch.hierarchyPassableBlocks, 0, blockCount);
	hierarchy.blocks = scratch.hierarchyPassableBlocks;
	hierarchy.blockWidth = blockWidth;
	hierarchy.blockHeight = blockHeight;

	const std::uint32_t startIndex = CellIndex(grid, request.startX, request.startY);
	const std::uint32_t goalIndex = CellIndex(grid, request.goalX, request.goalY);
	const std::size_t startBlockX =
		static_cast<std::size_t>(request.startX) / blockSize;
	const std::size_t startBlockY =
		static_cast<std::size_t>(request.startY) / blockSize;
	const std::size_t goalBlockX =
		static_cast<std::size_t>(request.goalX) / blockSize;
	const std::size_t goalBlockY =
		static_cast<std::size_t>(request.goalY) / blockSize;
	if (startBlockX >= blockWidth || startBlockY >= blockHeight ||
		goalBlockX >= blockWidth || goalBlockY >= blockHeight)
	{
		return HIERARCHY_BUILD_UNSUPPORTED;
	}

	const auto publishPassableBlocks = [&]() -> HierarchyBuildStatus
	{
		for (std::size_t block = 0; block < blockCount; ++block)
		{
			if (scratch.hierarchyPassableBlocks[block] == 0)
				continue;
			if (result.passableBlockCount >= result.passableBlockCapacity ||
				result.passableBlockIndices == nullptr)
			{
				return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;
			}
			result.passableBlockIndices[result.passableBlockCount++] =
				static_cast<std::uint32_t>(block);
		}
		return HIERARCHY_BUILD_READY;
	};
	const auto publishAllPassable = [&]() -> HierarchyBuildStatus
	{
		hierarchy.allPassable = true;
		result.hierarchyAllPassable = 1;
		result.passableBlockCount = 0;
		return HIERARCHY_BUILD_READY;
	};

	// The fixed Zero Hour hierarchy only crosses a block when its boundary cell
	// already owns path metadata. Ordinary queue snapshots normally begin with
	// clean metadata, so cross-block searches take the unchanged all-passable
	// fallback. A rare pre-existing boundary record is rejected later by the
	// owner corridor comparison rather than guessed here.
	if (request.hierarchyMode == DETERMINISTIC_PATH_HIERARCHY_ZERO_HOUR)
	{
		if (startBlockX != goalBlockX || startBlockY != goalBlockY)
			return publishAllPassable();
		const std::size_t minX = startBlockX == 0 ? 0 : startBlockX - 1;
		const std::size_t minY = startBlockY == 0 ? 0 : startBlockY - 1;
		const std::size_t maxX = std::min(blockWidth - 1, startBlockX + 1);
		const std::size_t maxY = std::min(blockHeight - 1, startBlockY + 1);
		for (std::size_t blockY = minY; blockY <= maxY; ++blockY)
		{
			for (std::size_t blockX = minX; blockX <= maxX; ++blockX)
				scratch.hierarchyPassableBlocks[blockY * blockWidth + blockX] = 1;
		}
		return publishPassableBlocks();
	}

	const std::size_t cellCount = static_cast<std::size_t>(grid.width) * grid.height;
	for (std::size_t i = 0; i < cellCount; ++i)
	{
		DeterministicPathSearchNode &node = scratch.nodes[i];
		node.pathCost = std::numeric_limits<std::uint32_t>::max();
		node.estimatedTotalCost = std::numeric_limits<std::uint32_t>::max();
		node.parentIndex = INVALID_NODE_INDEX;
		node.heapPosition = INVALID_NODE_INDEX;
		node.insertionOrdinal = 0;
		node.discoveryOrdinal = (grid.cells[i].navigationFlags &
			DETERMINISTIC_PATH_HAS_CELL_INFO) != 0 ? 1 : 0;
		node.closeOrdinal = 0;
		node.state = NODE_UNSEEN;
	}
	std::uint32_t remainingCellInfos = request.availableCellInfoCount;
	const auto allocateHierarchyInfo = [&](std::uint32_t index)
	{
		DeterministicPathSearchNode &node = scratch.nodes[index];
		if (node.discoveryOrdinal != 0)
			return true;
		if (remainingCellInfos == 0)
			return false;
		--remainingCellInfos;
		node.discoveryOrdinal = 1;
		return true;
	};
	if (!allocateHierarchyInfo(goalIndex) ||
		(startIndex != goalIndex && !allocateHierarchyInfo(startIndex)))
	{
		return publishAllPassable();
	}

	StableOpenHeap open = {scratch.nodes, scratch.heap, 0, scratch.heapCapacity};
	std::uint64_t insertionOrdinal = 0;
	DeterministicPathSearchNode &startNode = scratch.nodes[startIndex];
	startNode.pathCost = 0;
	// internal_findHierarchicalPath initializes through startPathfind(), whose
	// first total uses costToGoal (not costToHierGoal) and writes to 16 bits.
	startNode.estimatedTotalCost = StoreLegacyPathCost(Heuristic(request,
		request.startX, request.startY));
	startNode.insertionOrdinal = insertionOrdinal++;
	startNode.state = NODE_OPEN;
	if (!Push(open, startIndex))
		return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;

	bool reachedGoal = false;
	std::uint32_t goalParentIndex = INVALID_NODE_INDEX;
	while (open.size != 0)
	{
		const std::uint32_t parentIndex = Pop(open);
		DeterministicPathSearchNode &parentNode = scratch.nodes[parentIndex];
		const std::int32_t parentX = CellX(grid, parentIndex);
		const std::int32_t parentY = CellY(grid, parentIndex);
		const std::size_t parentBlockX =
			static_cast<std::size_t>(parentX) / blockSize;
		const std::size_t parentBlockY =
			static_cast<std::size_t>(parentY) / blockSize;
		const DeterministicPathCell &parentCell = grid.cells[parentIndex];
		if (parentCell.blockZone == grid.cells[goalIndex].blockZone &&
			parentBlockX == goalBlockX && parentBlockY == goalBlockY)
		{
			reachedGoal = true;
			goalParentIndex = parentIndex;
			break;
		}
		if ((parentCell.navigationFlags &
			DETERMINISTIC_PATH_BLOCK_INTERACTS_WITH_BRIDGE) != 0)
		{
			return HIERARCHY_BUILD_UNSUPPORTED;
		}
		parentNode.state = NODE_CLOSED;

		const auto scanSide = [&](std::int32_t baseX, std::int32_t baseY,
			std::int32_t stepX, std::int32_t stepY) -> bool
		{
			std::uint16_t examinedZones[256] = {};
			std::size_t examinedZoneCount = 0;
			for (std::size_t scan = 1; scan <= blockSize; ++scan)
			{
				const std::int32_t offset = (scan & 1U) != 0 ?
					-static_cast<std::int32_t>(scan >> 1) :
					static_cast<std::int32_t>(scan >> 1);
				const std::int32_t scanX = baseX + (stepY != 0 ? offset : 0);
				const std::int32_t scanY = baseY + (stepX != 0 ? offset : 0);
				if (!IsInside(grid, scanX, scanY))
					continue;
				const std::uint32_t scanIndex = CellIndex(grid, scanX, scanY);
				DeterministicPathSearchNode &scanNode = scratch.nodes[scanIndex];
				const DeterministicPathCell &scanCell = grid.cells[scanIndex];
				if ((scanNode.state == NODE_OPEN || scanNode.state == NODE_CLOSED) &&
					parentCell.blockZone == scanCell.blockZone)
				{
					break;
				}
				if (request.isHuman != 0 && (scanCell.navigationFlags &
					DETERMINISTIC_PATH_INSIDE_LOGICAL_EXTENT) == 0)
				{
					continue;
				}
				if (parentCell.blockZone != scanCell.blockZone ||
					scanNode.state == NODE_OPEN || scanNode.state == NODE_CLOSED ||
					parentCell.blockZone != scanCell.zone)
				{
					continue;
				}
				const std::int32_t adjacentX = scanX + stepX;
				const std::int32_t adjacentY = scanY + stepY;
				if (!IsInside(grid, adjacentX, adjacentY))
					continue;
				const std::uint32_t adjacentIndex =
					CellIndex(grid, adjacentX, adjacentY);
				DeterministicPathSearchNode &adjacentNode =
					scratch.nodes[adjacentIndex];
				const DeterministicPathCell &adjacentCell =
					grid.cells[adjacentIndex];
				if (adjacentNode.state == NODE_OPEN ||
					adjacentNode.state == NODE_CLOSED ||
					adjacentCell.globalZone != scanCell.globalZone)
				{
					continue;
				}
				bool alreadyExamined = false;
				for (std::size_t zone = 0; zone < examinedZoneCount; ++zone)
				{
					if (examinedZones[zone] == adjacentCell.blockZone)
					{
						alreadyExamined = true;
						break;
					}
				}
				if (alreadyExamined)
					continue;
				const bool scanNeededInfo = scanNode.discoveryOrdinal == 0;
				if (!allocateHierarchyInfo(scanIndex))
					continue;
				if (!allocateHierarchyInfo(adjacentIndex))
				{
					if (scanNeededInfo)
					{
						scanNode.discoveryOrdinal = 0;
						++remainingCellInfos;
					}
					continue;
				}
				if (scanNode.state == NODE_UNSEEN)
					scanNode.state = NODE_CLOSED;
				std::uint32_t pathCost = AddCost(parentNode.pathCost,
					HierarchicalDistance(grid, adjacentIndex, parentIndex));
				if (adjacentCell.pinched != 0 || scanCell.pinched != 0)
					pathCost = AddCost(pathCost, 20U);
				else if (examinedZoneCount < blockSize)
					examinedZones[examinedZoneCount++] = adjacentCell.blockZone;
				adjacentNode.pathCost = StoreLegacyPathCost(pathCost);
				adjacentNode.estimatedTotalCost = StoreLegacyPathCost(AddCost(
					adjacentNode.pathCost,
					HierarchicalDistance(grid, adjacentIndex, goalIndex)));
				adjacentNode.parentIndex = parentIndex;
				adjacentNode.insertionOrdinal = insertionOrdinal++;
				adjacentNode.state = NODE_OPEN;
				if (!Push(open, adjacentIndex))
					return false;
			}
			return true;
		};

		const std::int32_t blockOriginX =
			static_cast<std::int32_t>(parentBlockX * blockSize);
		const std::int32_t blockOriginY =
			static_cast<std::int32_t>(parentBlockY * blockSize);
		const std::int32_t halfBlock =
			static_cast<std::int32_t>(blockSize / 2);
		if (parentBlockX > 0 &&
			!scanSide(blockOriginX, blockOriginY + halfBlock, -1, 0))
			return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;
		if (parentBlockX + 1 < blockWidth &&
			!scanSide(blockOriginX + static_cast<std::int32_t>(blockSize) - 1,
				blockOriginY + halfBlock, 1, 0))
			return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;
		if (parentBlockY > 0 &&
			!scanSide(blockOriginX + halfBlock, blockOriginY, 0, -1))
			return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;
		if (parentBlockY + 1 < blockHeight &&
			!scanSide(blockOriginX + halfBlock,
				blockOriginY + static_cast<std::int32_t>(blockSize) - 1, 0, 1))
			return HIERARCHY_BUILD_OUTPUT_TOO_SMALL;
	}
	if (!reachedGoal)
		return publishAllPassable();

	std::uint32_t pathIndex = goalIndex;
	if (goalParentIndex != goalIndex)
		scratch.nodes[goalIndex].parentIndex = goalParentIndex;
	for (std::size_t guard = 0; guard < cellCount; ++guard)
	{
		const std::size_t blockX =
			static_cast<std::size_t>(CellX(grid, pathIndex)) / blockSize;
		const std::size_t blockY =
			static_cast<std::size_t>(CellY(grid, pathIndex)) / blockSize;
		if (blockX >= blockWidth || blockY >= blockHeight)
			return HIERARCHY_BUILD_UNSUPPORTED;
		scratch.hierarchyPassableBlocks[blockY * blockWidth + blockX] = 1;
		if (pathIndex == startIndex)
			return publishPassableBlocks();
		pathIndex = scratch.nodes[pathIndex].parentIndex;
		if (pathIndex == INVALID_NODE_INDEX)
			return HIERARCHY_BUILD_UNSUPPORTED;
	}
	return HIERARCHY_BUILD_UNSUPPORTED;
}

bool IsLegacyDirectFootprintClear(const ImmutableNavigationGrid &grid,
	const DeterministicPathRequest &request, std::int32_t x, std::int32_t y)
{
	if (!IsInside(grid, x, y))
		return false;
	const DeterministicPathCell &center = grid.cells[CellIndex(grid, x, y)];
	if (!IsTraversable(center, request.traversalMask) ||
		center.type != DETERMINISTIC_PATH_CELL_CLEAR ||
		center.layer != request.expectedLayer ||
		center.connectsToLayer != DETERMINISTIC_PATH_LAYER_INVALID ||
		center.pinched != 0 || center.blockPassable == 0 ||
		center.zone != request.requiredZone ||
		center.obstacleObjectId != DETERMINISTIC_PATH_INVALID_OBJECT_ID)
	{
		return false;
	}

	// Legacy always validates the center terrain/zone above, but its movement
	// footprint loop is intentionally empty for a zero-radius edge-centered
	// object.  Preserve that bound while keeping this subset stricter about
	// foreign occupants in every footprint cell that legacy does inspect.
	const std::int32_t radius = request.footprintRadius;
	const std::int32_t upper = request.centerInCell ? radius + 1 : radius;
	for (std::int32_t cellX = x - radius; cellX < x + upper; ++cellX)
	{
		for (std::int32_t cellY = y - radius; cellY < y + upper; ++cellY)
		{
			if (!IsInside(grid, cellX, cellY))
				return false;
			const DeterministicPathCell &cell = grid.cells[CellIndex(grid, cellX, cellY)];
			if (!IsOwnedOrEmpty(cell.positionObjectId, request.objectId) ||
				!IsOwnedOrEmpty(cell.goalObjectId, request.objectId))
			{
				return false;
			}
			if (cell.flags != DETERMINISTIC_PATH_NO_UNITS &&
				cell.positionObjectId == DETERMINISTIC_PATH_INVALID_OBJECT_ID &&
				cell.goalObjectId == DETERMINISTIC_PATH_INVALID_OBJECT_ID)
			{
				return false;
			}
		}
	}
	return true;
}

DeterministicPathSearchStatus FindLegacyDirectPath(
	const ImmutableNavigationGrid &grid,
	const DeterministicPathRequest &request,
	DeterministicPathSearchResult &result)
{
	if (request.expectedLayer != DETERMINISTIC_PATH_LAYER_GROUND ||
		request.requiredZone == 0 || request.objectId == DETERMINISTIC_PATH_INVALID_OBJECT_ID)
	{
		return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
	}

	const std::int32_t deltaX = request.goalX >= request.startX
		? request.goalX - request.startX : request.startX - request.goalX;
	const std::int32_t deltaY = request.goalY >= request.startY
		? request.goalY - request.startY : request.startY - request.goalY;
	std::int32_t x = request.startX;
	std::int32_t y = request.startY;
	std::int32_t xinc1 = request.goalX >= request.startX ? 1 : -1;
	std::int32_t xinc2 = xinc1;
	std::int32_t yinc1 = request.goalY >= request.startY ? 1 : -1;
	std::int32_t yinc2 = yinc1;
	std::int32_t denominator;
	std::int32_t numerator;
	std::int32_t numeratorAdd;
	std::int32_t pixelCount;
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

	std::size_t required = 0;
	std::uint32_t callbackCount = 0;
	bool reachedGoal = false;
	auto appendPoint = [&](std::int32_t pointX, std::int32_t pointY) -> bool
	{
		if (!IsLegacyDirectFootprintClear(grid, request, pointX, pointY))
			return false;
		++callbackCount;
		if (reachedGoal)
			return true;
		if (required < result.pointCapacity)
		{
			result.points[required].x = pointX;
			result.points[required].y = pointY;
			result.points[required].layer = request.expectedLayer;
			result.points[required].reserved[0] = 0;
			result.points[required].reserved[1] = 0;
			result.points[required].reserved[2] = 0;
		}
		++required;
		if (pointX == request.goalX && pointY == request.goalY)
			reachedGoal = true;
		return true;
	};

	for (std::int32_t pixel = 0; pixel <= pixelCount; ++pixel)
	{
		if (!appendPoint(x, y))
			return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
		// Preserve the legacy zero-length edge case too: 0 >= 0 invokes a
		// second callback one cell in +Y before the loop finishes.
		numerator += numeratorAdd;
		if (numerator >= denominator)
		{
			numerator -= denominator;
			x += xinc1;
			y += yinc1;
			if (!appendPoint(x, y))
				return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
		}
		x += xinc2;
		y += yinc2;
	}

	if (!reachedGoal)
		return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
	result.pointCount = required;
	result.expandedNodeCount = callbackCount;
	if (required > result.pointCapacity)
		return Finish(result, DETERMINISTIC_PATH_OUTPUT_TOO_SMALL);
	return Finish(result, DETERMINISTIC_PATH_FOUND);
}

DeterministicPathSearchStatus Finish(DeterministicPathSearchResult &result,
	DeterministicPathSearchStatus status)
{
	result.status = status;
	return status;
}

} // namespace

DeterministicPathSearchStatus FindDeterministicPath(
	const ImmutableNavigationGrid &grid,
	const DeterministicPathRequest &request,
	DeterministicPathSearchScratch &scratch,
	DeterministicPathSearchResult &result) noexcept
{
	result.pointCount = 0;
	result.snapshotGeneration = grid.snapshotGeneration;
	result.expandedNodeCount = 0;
	result.discoveredNodeCount = 0;
	result.requiredCellInfoCount = 0;
	result.cumulativeCellCount = 0;
	result.passableBlockCount = 0;
	result.hierarchyAllPassable = 0;
	result.status = DETERMINISTIC_PATH_INVALID_INPUT;

	if (grid.cells == nullptr || grid.width == 0 || grid.height == 0 ||
		request.traversalMask == 0 || request.maximumExpandedNodes == 0 ||
		!IsInside(grid, request.startX, request.startY) ||
		!IsInside(grid, request.goalX, request.goalY) ||
		(result.pointCapacity != 0 && result.points == nullptr))
	{
		return result.status;
	}
	if (request.expectedSnapshotGeneration != grid.snapshotGeneration)
		return Finish(result, DETERMINISTIC_PATH_SNAPSHOT_GENERATION_MISMATCH);
	if (request.requireLegacyDirectLine)
		return FindLegacyDirectPath(grid, request, result);
	if (grid.width > std::numeric_limits<std::size_t>::max() / grid.height)
		return result.status;
	const std::size_t cellCount = static_cast<std::size_t>(grid.width) * grid.height;
	if (cellCount > std::numeric_limits<std::uint32_t>::max())
		return result.status;
	if (scratch.nodes == nullptr || scratch.heap == nullptr ||
		scratch.nodeCapacity < cellCount || scratch.heapCapacity < cellCount)
	{
		return Finish(result, DETERMINISTIC_PATH_SCRATCH_TOO_SMALL);
	}

	if (request.objectId == DETERMINISTIC_PATH_INVALID_OBJECT_ID ||
		request.requiredZone == 0 ||
		request.expectedLayer != DETERMINISTIC_PATH_LAYER_GROUND ||
		request.allowDiagonal == 0 || request.allowBlockedStart != 0)
	{
		return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
	}
	HierarchyPassability hierarchy = {};
	const HierarchyBuildStatus hierarchyStatus = BuildHierarchyPassability(grid,
		request, scratch, result, hierarchy);
	if (hierarchyStatus == HIERARCHY_BUILD_UNSUPPORTED)
		return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
	if (hierarchyStatus == HIERARCHY_BUILD_OUTPUT_TOO_SMALL)
		return Finish(result, DETERMINISTIC_PATH_SCRATCH_TOO_SMALL);

	const std::uint32_t startIndex = CellIndex(grid, request.startX, request.startY);
	const std::uint32_t goalIndex = CellIndex(grid, request.goalX, request.goalY);
	const OrdinaryCellInspection startInspection = InspectOrdinaryCell(grid,
		request, request.startX, request.startY, hierarchy, false, false);
	const OrdinaryCellInspection goalInspection = InspectOrdinaryCell(grid,
		request, request.goalX, request.goalY, hierarchy, false, false);
	if (startInspection == ORDINARY_CELL_UNSUPPORTED ||
		goalInspection == ORDINARY_CELL_UNSUPPORTED ||
		grid.cells[startIndex].pinched != 0 || grid.cells[goalIndex].pinched != 0)
	{
		return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
	}
	if (startInspection != ORDINARY_CELL_CLEAR ||
		goalInspection != ORDINARY_CELL_CLEAR ||
		grid.cells[startIndex].zone != request.requiredZone ||
		grid.cells[goalIndex].zone != request.requiredZone)
	{
		return Finish(result, DETERMINISTIC_PATH_NO_PATH);
	}

	if (request.requireObstructedSearch != 0)
	{
		if (startIndex == goalIndex)
			return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
		bool unsupportedLineFact = false;
		bool directLineClear = true;
		auto probe = [&](std::int32_t x, std::int32_t y, bool first)
		{
			if (first)
				return true;
			const OrdinaryCellInspection inspection = InspectOrdinaryCell(grid,
				request, x, y, hierarchy, true, true);
			if (inspection == ORDINARY_CELL_UNSUPPORTED)
				unsupportedLineFact = true;
			if (inspection != ORDINARY_CELL_CLEAR)
				directLineClear = false;
			return inspection == ORDINARY_CELL_CLEAR;
		};
		VisitLegacySupercover(request.startX, request.startY, request.goalX,
			request.goalY, probe);
		if (unsupportedLineFact)
			return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
		if (directLineClear)
			return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
	}

	for (std::size_t i = 0; i < cellCount; ++i)
	{
		scratch.nodes[i].pathCost = std::numeric_limits<std::uint32_t>::max();
		scratch.nodes[i].estimatedTotalCost = std::numeric_limits<std::uint32_t>::max();
		scratch.nodes[i].parentIndex = INVALID_NODE_INDEX;
		scratch.nodes[i].heapPosition = INVALID_NODE_INDEX;
		scratch.nodes[i].insertionOrdinal = 0;
		scratch.nodes[i].discoveryOrdinal = 0;
		scratch.nodes[i].closeOrdinal = 0;
		scratch.nodes[i].state = NODE_UNSEEN;
	}

	std::uint32_t remainingCellInfos = request.availableCellInfoCount;
	auto consumeInitialCellInfo = [&](std::uint32_t index)
	{
		if ((grid.cells[index].navigationFlags &
			DETERMINISTIC_PATH_HAS_CELL_INFO) != 0)
		{
			return true;
		}
		if (remainingCellInfos == 0)
			return false;
		--remainingCellInfos;
		++result.requiredCellInfoCount;
		return true;
	};
	// internalFindPath allocates the goal before the start, including when the
	// goal will only be discovered much later by the search.
	if (!consumeInitialCellInfo(goalIndex) ||
		(startIndex != goalIndex && !consumeInitialCellInfo(startIndex)))
	{
		return Finish(result, DETERMINISTIC_PATH_CELL_INFO_SHORTAGE);
	}

	StableOpenHeap open = {scratch.nodes, scratch.heap, 0, scratch.heapCapacity};
	std::uint64_t nextInsertionOrdinal = 0;
	std::uint64_t nextDiscoveryOrdinal = 0;
	std::uint64_t nextCloseOrdinal = 0;
	DeterministicPathSearchNode &startNode = scratch.nodes[startIndex];
	startNode.pathCost = 0;
	startNode.estimatedTotalCost = StoreLegacyPathCost(Heuristic(request,
		request.startX, request.startY));
	startNode.insertionOrdinal = nextInsertionOrdinal++;
	startNode.discoveryOrdinal = nextDiscoveryOrdinal++;
	startNode.state = NODE_OPEN;
	result.discoveredNodeCount = 1;
	if (!Push(open, startIndex))
		return Finish(result, DETERMINISTIC_PATH_SCRATCH_TOO_SMALL);

	static const std::int32_t deltaX[8] = { 1, 0, -1, 0, 1, -1, -1, 1 };
	static const std::int32_t deltaY[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	bool found = false;
	bool cellInfoShortage = false;
	bool unsupportedSubset = false;

	auto consumeDiscoveredCellInfo = [&](std::uint32_t index)
	{
		if (index == startIndex || index == goalIndex ||
			(grid.cells[index].navigationFlags &
				DETERMINISTIC_PATH_HAS_CELL_INFO) != 0)
		{
			return true;
		}
		if (remainingCellInfos == 0)
		{
			cellInfoShortage = true;
			return false;
		}
		--remainingCellInfos;
		++result.requiredCellInfoCount;
		return true;
	};

	auto insertOrImproveDirectCell = [&](std::uint32_t fromIndex,
		std::uint32_t toIndex)
	{
		DeterministicPathSearchNode &fromNode = scratch.nodes[fromIndex];
		DeterministicPathSearchNode &toNode = scratch.nodes[toIndex];
		if (fromNode.state == NODE_UNSEEN)
		{
			unsupportedSubset = true;
			return false;
		}
		if (toNode.state == NODE_UNSEEN)
		{
			if (!consumeDiscoveredCellInfo(toIndex))
				return false;
			toNode.discoveryOrdinal = nextDiscoveryOrdinal++;
			++result.discoveredNodeCount;
		}
		const std::uint32_t candidateCost = AddCost(fromNode.pathCost, 5U);
		if (toNode.state != NODE_UNSEEN && toNode.pathCost <= candidateCost)
			return true;
		if (toNode.state == NODE_OPEN)
			Remove(open, toIndex);
		toNode.pathCost = StoreLegacyPathCost(candidateCost);
		toNode.estimatedTotalCost = StoreLegacyPathCost(AddCost(toNode.pathCost,
			Heuristic(request, CellX(grid, toIndex), CellY(grid, toIndex))));
		toNode.parentIndex = fromIndex;
		toNode.insertionOrdinal = nextInsertionOrdinal++;
		toNode.state = NODE_OPEN;
		if (!Push(open, toIndex))
		{
			unsupportedSubset = true;
			return false;
		}
		return true;
	};

	while (open.size != 0)
	{
		if (result.expandedNodeCount >= request.maximumExpandedNodes)
			return Finish(result, DETERMINISTIC_PATH_BUDGET_EXHAUSTED);
		const std::uint32_t currentIndex = Pop(open);
		DeterministicPathSearchNode &current = scratch.nodes[currentIndex];
		++result.expandedNodeCount;
		if (currentIndex == goalIndex)
		{
			// The legacy goal is removed from open and is never put on closed.
			current.state = NODE_TERMINAL;
			found = true;
			break;
		}
		current.state = NODE_CLOSED;
		current.closeOrdinal = nextCloseOrdinal++;

		const std::int32_t currentX = CellX(grid, currentIndex);
		const std::int32_t currentY = CellY(grid, currentIndex);

		std::uint32_t previousLineIndex = currentIndex;
		auto injectDirectLine = [&](std::int32_t x, std::int32_t y, bool first)
		{
			if (first)
				return true;
			const OrdinaryCellInspection inspection = InspectOrdinaryCell(grid,
				request, x, y, hierarchy, true, true);
			if (inspection == ORDINARY_CELL_UNSUPPORTED)
			{
				unsupportedSubset = true;
				return false;
			}
			if (inspection != ORDINARY_CELL_CLEAR)
				return false;
			const std::uint32_t toIndex = CellIndex(grid, x, y);
			if (!insertOrImproveDirectCell(previousLineIndex, toIndex))
				return false;
			previousLineIndex = toIndex;
			return true;
		};
		VisitLegacySupercover(currentX, currentY, request.goalX,
			request.goalY, injectDirectLine);
		if (unsupportedSubset)
			return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);

		bool neighborFlags[8] = {false, false, false, false,
			false, false, false, false};
		static const std::size_t adjacent[5] = {0, 1, 2, 3, 0};
		for (std::size_t neighbor = 0; neighbor < 8; ++neighbor)
		{
			const std::int32_t nextX = currentX + deltaX[neighbor];
			const std::int32_t nextY = currentY + deltaY[neighbor];
			if (!IsInside(grid, nextX, nextY))
				continue;
			const std::uint32_t nextIndex = CellIndex(grid, nextX, nextY);
			DeterministicPathSearchNode &next = scratch.nodes[nextIndex];
			// Legacy checks list membership before diagonal side eligibility or
			// movement facts.  A previously discovered orthogonal therefore does
			// not set neighborFlags for a later diagonal in this expansion.
			if (next.state != NODE_UNSEEN)
				continue;
			if (neighbor >= 4)
			{
				if (!neighborFlags[adjacent[neighbor - 4]] &&
					!neighborFlags[adjacent[neighbor - 3]])
					continue;
			}
			const OrdinaryCellInspection inspection = InspectOrdinaryCell(grid,
				request, nextX, nextY, hierarchy, false, false);
			if (inspection == ORDINARY_CELL_UNSUPPORTED)
				return Finish(result, DETERMINISTIC_PATH_UNSUPPORTED_SUBSET);
			if (inspection != ORDINARY_CELL_CLEAR)
				continue;
			neighborFlags[neighbor] = true;
			if (!consumeDiscoveredCellInfo(nextIndex))
				break;
			std::uint32_t candidateCost = LegacyNeighborCost(grid, scratch,
				nextIndex, currentIndex);
			if (grid.cells[nextIndex].pinched != 0)
				candidateCost = AddCost(candidateCost, 10U);
			if (!IsHierarchyPassable(grid, hierarchy, nextX, nextY))
				candidateCost = AddCost(candidateCost, 1000U);
			next.pathCost = StoreLegacyPathCost(candidateCost);
			next.estimatedTotalCost = StoreLegacyPathCost(AddCost(next.pathCost,
				Heuristic(request, nextX, nextY)));
			next.parentIndex = currentIndex;
			next.insertionOrdinal = nextInsertionOrdinal++;
			next.discoveryOrdinal = nextDiscoveryOrdinal++;
			next.state = NODE_OPEN;
			++result.discoveredNodeCount;
			if (!Push(open, nextIndex))
				return Finish(result, DETERMINISTIC_PATH_SCRATCH_TOO_SMALL);
		}
	}

	if (!found)
		return Finish(result, cellInfoShortage ?
			DETERMINISTIC_PATH_CELL_INFO_SHORTAGE : DETERMINISTIC_PATH_NO_PATH);

	std::size_t pointCount = 1;
	for (std::uint32_t index = goalIndex; index != startIndex;)
	{
		index = scratch.nodes[index].parentIndex;
		if (index == INVALID_NODE_INDEX || pointCount >= cellCount)
			return Finish(result, DETERMINISTIC_PATH_NO_PATH);
		++pointCount;
	}
	result.pointCount = pointCount;
	result.cumulativeCellCount = result.discoveredNodeCount == 0 ? 0 :
		result.discoveredNodeCount - 1;
	if (pointCount > result.pointCapacity)
		return Finish(result, DETERMINISTIC_PATH_OUTPUT_TOO_SMALL);

	std::uint32_t index = goalIndex;
	for (std::size_t outputIndex = pointCount; outputIndex > 0; --outputIndex)
	{
		result.points[outputIndex - 1].x = CellX(grid, index);
		result.points[outputIndex - 1].y = CellY(grid, index);
		result.points[outputIndex - 1].layer = request.expectedLayer;
		result.points[outputIndex - 1].reserved[0] = 0;
		result.points[outputIndex - 1].reserved[1] = 0;
		result.points[outputIndex - 1].reserved[2] = 0;
		if (index != startIndex)
			index = scratch.nodes[index].parentIndex;
	}
	return Finish(result, DETERMINISTIC_PATH_FOUND);
}

bool BuildLegacySupercoverCallbacks(std::int32_t startX, std::int32_t startY,
	std::int32_t goalX, std::int32_t goalY,
	DeterministicPathPoint *callbacks, std::size_t callbackCapacity,
	std::size_t &callbackCount) noexcept
{
	callbackCount = 0;
	if (callbackCapacity != 0 && callbacks == nullptr)
		return false;
	const std::int64_t wideDeltaX = goalX >= startX
		? static_cast<std::int64_t>(goalX) - startX
		: static_cast<std::int64_t>(startX) - goalX;
	const std::int64_t wideDeltaY = goalY >= startY
		? static_cast<std::int64_t>(goalY) - startY
		: static_cast<std::int64_t>(startY) - goalY;
	if (wideDeltaX > std::numeric_limits<std::int32_t>::max() ||
		wideDeltaY > std::numeric_limits<std::int32_t>::max())
	{
		return false;
	}

	const std::int32_t deltaX = static_cast<std::int32_t>(wideDeltaX);
	const std::int32_t deltaY = static_cast<std::int32_t>(wideDeltaY);
	std::int32_t x = startX;
	std::int32_t y = startY;
	std::int32_t xinc1 = goalX >= startX ? 1 : -1;
	std::int32_t xinc2 = xinc1;
	std::int32_t yinc1 = goalY >= startY ? 1 : -1;
	std::int32_t yinc2 = yinc1;
	std::int32_t denominator;
	std::int32_t numerator;
	std::int32_t numeratorAdd;
	std::int32_t pixelCount;
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

	const auto append = [&](std::int32_t pointX, std::int32_t pointY)
	{
		if (callbackCount < callbackCapacity)
		{
			callbacks[callbackCount].x = pointX;
			callbacks[callbackCount].y = pointY;
			callbacks[callbackCount].layer = DETERMINISTIC_PATH_LAYER_GROUND;
			callbacks[callbackCount].reserved[0] = 0;
			callbacks[callbackCount].reserved[1] = 0;
			callbacks[callbackCount].reserved[2] = 0;
		}
		++callbackCount;
	};

	for (std::int32_t pixel = 0; pixel <= pixelCount; ++pixel)
	{
		append(x, y);
		// Preserve the legacy zero-length edge case too: 0 >= 0 invokes a
		// second callback one cell in +Y before the loop finishes.
		numerator += numeratorAdd;
		if (numerator >= denominator)
		{
			numerator -= denominator;
			x += xinc1;
			y += yinc1;
			append(x, y);
		}
		x += xinc2;
		y += yinc2;
	}
	return callbackCount <= callbackCapacity;
}

DirectPathSearchStatus FindDeterministicDirectPath(
	const DirectPathSnapshot &snapshot,
	DirectPathSearchResult &result) noexcept
{
	result.rawPointCount = 0;
	result.callbackCount = 0;
	result.requiredCellInfoCount = 0;
	result.startNeighborAllocationCount = 0;
	result.openCellCountAfterGoal = 0;
	result.cumulativeCellCount = 0;
	result.topologyOccupancyGeneration = snapshot.topologyOccupancyGeneration;
	result.requestToken = snapshot.requestToken;
	result.objectId = snapshot.objectId;
	result.status = DIRECT_PATH_INVALID_INPUT;
	if (snapshot.callbacks == nullptr || snapshot.callbackCount == 0 ||
		snapshot.callbackCount > DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS ||
		snapshot.startNeighbors == nullptr ||
		snapshot.startNeighborCount != DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT ||
		snapshot.topologyOccupancyGeneration == 0 || snapshot.requestToken == 0 ||
		snapshot.objectId == DETERMINISTIC_PATH_INVALID_OBJECT_ID ||
		snapshot.requiredZone == 0 ||
		snapshot.expectedLayer != DETERMINISTIC_PATH_LAYER_GROUND ||
		(result.rawPointCapacity != 0 && result.rawPoints == nullptr))
	{
		return result.status;
	}

	DeterministicPathPoint expected[DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS];
	std::size_t expectedCount = 0;
	if (!BuildLegacySupercoverCallbacks(snapshot.startX, snapshot.startY,
		snapshot.goalX, snapshot.goalY, expected,
		DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS, expectedCount) ||
		expectedCount != snapshot.callbackCount)
	{
		result.status = DIRECT_PATH_MALFORMED_SNAPSHOT;
		return result.status;
	}

	const std::uint8_t requiredFlags =
		DIRECT_PATH_FACT_CLEAR_GROUND |
		DIRECT_PATH_FACT_HIERARCHY_PASSABLE |
		DIRECT_PATH_FACT_INSIDE_LOGICAL_EXTENT |
		DIRECT_PATH_FACT_FOOTPRINT_CLEAR |
		DIRECT_PATH_FACT_NO_FOREIGN_OCCUPANCY |
		DIRECT_PATH_FACT_NO_LAYER_CONNECTION |
		DIRECT_PATH_FACT_NOT_PINCHED |
		DIRECT_PATH_FACT_METADATA_CLEAN;
	for (std::size_t i = 0; i < snapshot.callbackCount; ++i)
	{
		const DirectPathCellFact &fact = snapshot.callbacks[i];
		if (fact.x != expected[i].x || fact.y != expected[i].y)
		{
			result.status = DIRECT_PATH_MALFORMED_SNAPSHOT;
			return result.status;
		}
		if (fact.zone != snapshot.requiredZone ||
			(fact.flags & requiredFlags) != requiredFlags)
		{
			result.status = DIRECT_PATH_UNSUPPORTED_SUBSET;
			return result.status;
		}
	}

	static const std::int32_t neighborX[DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT] =
		{1, 0, -1, 0, 1, -1, -1, 1};
	static const std::int32_t neighborY[DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT] =
		{0, 1, 0, -1, 1, 1, -1, -1};
	for (std::size_t i = 0; i < snapshot.startNeighborCount; ++i)
	{
		const DirectPathCellFact &fact = snapshot.startNeighbors[i];
		if (fact.x != snapshot.startX + neighborX[i] ||
			fact.y != snapshot.startY + neighborY[i])
		{
			result.status = DIRECT_PATH_MALFORMED_SNAPSHOT;
			return result.status;
		}
		if (fact.zone != snapshot.requiredZone ||
			(fact.flags & requiredFlags) != requiredFlags)
		{
			result.status = DIRECT_PATH_UNSUPPORTED_SUBSET;
			return result.status;
		}
	}

	const auto samePoint = [](std::int32_t leftX, std::int32_t leftY,
		std::int32_t rightX, std::int32_t rightY)
	{
		return leftX == rightX && leftY == rightY;
	};

	std::size_t goalCallback = snapshot.callbackCount;
	for (std::size_t i = 0; i < snapshot.callbackCount; ++i)
	{
		if (samePoint(snapshot.callbacks[i].x, snapshot.callbacks[i].y,
			snapshot.goalX, snapshot.goalY))
		{
			goalCallback = i;
			break;
		}
	}
	if (goalCallback == snapshot.callbackCount)
	{
		result.status = DIRECT_PATH_NO_PATH;
		return result.status;
	}
	result.callbackCount = snapshot.callbackCount;
	result.rawPointCount = goalCallback + 1;
	if (result.rawPointCount > result.rawPointCapacity)
	{
		result.status = DIRECT_PATH_OUTPUT_TOO_SMALL;
		return result.status;
	}
	for (std::size_t i = 0; i < result.rawPointCount; ++i)
	{
		result.rawPoints[i] = expected[i];
	}

	const bool sameCell = samePoint(snapshot.startX, snapshot.startY,
		snapshot.goalX, snapshot.goalY);
	if (sameCell)
	{
		result.requiredCellInfoCount = snapshot.callbacks[0].hasPathfindInfo ? 0 : 1;
		if (result.requiredCellInfoCount > snapshot.availableCellInfoCount)
			result.status = DIRECT_PATH_CELL_INFO_SHORTAGE;
		else
			result.status = DIRECT_PATH_FOUND;
		return result.status;
	}

	const DirectPathCellFact &startFact = snapshot.callbacks[0];
	const DirectPathCellFact &goalFact = snapshot.callbacks[goalCallback];
	result.requiredCellInfoCount = (goalFact.hasPathfindInfo ? 0 : 1) +
		(startFact.hasPathfindInfo ? 0 : 1);

	for (std::size_t i = 1; i < snapshot.callbackCount; ++i)
	{
		const DirectPathCellFact &fact = snapshot.callbacks[i];
		const bool isStart = samePoint(fact.x, fact.y, snapshot.startX, snapshot.startY);
		const bool isGoal = samePoint(fact.x, fact.y, snapshot.goalX, snapshot.goalY);
		// The validated legacy supercover stream is monotone and contains no
		// repeated coordinates, so every non-endpoint callback is a new cell.
		if (!isStart && !isGoal)
		{
			if (!fact.hasPathfindInfo)
				++result.requiredCellInfoCount;
			++result.openCellCountAfterGoal;
		}
	}

	for (std::size_t i = 0; i < snapshot.startNeighborCount; ++i)
	{
		const DirectPathCellFact &fact = snapshot.startNeighbors[i];
		bool alreadyDiscovered = false;
		for (std::size_t callback = 0;
			!alreadyDiscovered && callback < snapshot.callbackCount; ++callback)
		{
			alreadyDiscovered = samePoint(snapshot.callbacks[callback].x,
				snapshot.callbacks[callback].y, fact.x, fact.y);
		}
		for (std::size_t earlier = 0; !alreadyDiscovered && earlier < i; ++earlier)
		{
			alreadyDiscovered = samePoint(snapshot.startNeighbors[earlier].x,
				snapshot.startNeighbors[earlier].y, fact.x, fact.y);
		}
		if (alreadyDiscovered)
			continue;
		if (!fact.hasPathfindInfo)
		{
			++result.requiredCellInfoCount;
			++result.startNeighborAllocationCount;
		}
		if (!samePoint(fact.x, fact.y, snapshot.startX, snapshot.startY) &&
			!samePoint(fact.x, fact.y, snapshot.goalX, snapshot.goalY))
		{
			++result.openCellCountAfterGoal;
		}
	}

	result.cumulativeCellCount = result.openCellCountAfterGoal + 1;
	if (result.requiredCellInfoCount > snapshot.availableCellInfoCount)
		result.status = DIRECT_PATH_CELL_INFO_SHORTAGE;
	else
		result.status = DIRECT_PATH_FOUND;
	return result.status;
}

bool IsDirectPathMaterializationPlanValid(const DirectPathSnapshot &snapshot,
	const DirectPathSearchResult &result,
	std::size_t currentAvailableCellInfoCount) noexcept
{
	if (result.status != DIRECT_PATH_FOUND ||
		!IsDirectPathResultCurrent(result,
			snapshot.topologyOccupancyGeneration, snapshot.requestToken,
			snapshot.objectId) ||
		snapshot.callbacks == nullptr || snapshot.callbackCount == 0 ||
		snapshot.callbackCount > DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS ||
		snapshot.startNeighbors == nullptr ||
		snapshot.startNeighborCount != DETERMINISTIC_DIRECT_PATH_NEIGHBOR_COUNT ||
		result.callbackCount != snapshot.callbackCount ||
		result.rawPoints == nullptr || result.rawPointCount == 0 ||
		result.rawPointCount > result.rawPointCapacity ||
		result.rawPointCount > snapshot.callbackCount)
	{
		return false;
	}

	DeterministicPathPoint expected[DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS];
	std::size_t expectedCount = 0;
	if (!BuildLegacySupercoverCallbacks(snapshot.startX, snapshot.startY,
		snapshot.goalX, snapshot.goalY, expected,
		DETERMINISTIC_DIRECT_PATH_MAX_CALLBACKS, expectedCount) ||
		expectedCount != snapshot.callbackCount)
	{
		return false;
	}

	const auto samePoint = [](std::int32_t leftX, std::int32_t leftY,
		std::int32_t rightX, std::int32_t rightY)
	{
		return leftX == rightX && leftY == rightY;
	};

	std::size_t goalCallback = snapshot.callbackCount;
	for (std::size_t i = 0; i < snapshot.callbackCount; ++i)
	{
		if (snapshot.callbacks[i].x != expected[i].x ||
			snapshot.callbacks[i].y != expected[i].y)
		{
			return false;
		}
		if (samePoint(expected[i].x, expected[i].y,
			snapshot.goalX, snapshot.goalY) && goalCallback == snapshot.callbackCount)
		{
			goalCallback = i;
		}
	}
	if (goalCallback == snapshot.callbackCount ||
		result.rawPointCount != goalCallback + 1)
	{
		return false;
	}
	for (std::size_t i = 0; i < result.rawPointCount; ++i)
	{
		if (result.rawPoints[i].x != expected[i].x ||
			result.rawPoints[i].y != expected[i].y ||
			result.rawPoints[i].layer != DETERMINISTIC_PATH_LAYER_GROUND)
		{
			return false;
		}
	}

	std::size_t requiredCellInfoCount = 0;
	std::size_t startNeighborAllocationCount = 0;
	std::size_t openCellCountAfterGoal = 0;
	std::size_t cumulativeCellCount = 0;
	const bool sameCell = samePoint(snapshot.startX, snapshot.startY,
		snapshot.goalX, snapshot.goalY);
	if (sameCell)
	{
		requiredCellInfoCount = snapshot.callbacks[0].hasPathfindInfo ? 0 : 1;
	}
	else
	{
		const DirectPathCellFact &startFact = snapshot.callbacks[0];
		const DirectPathCellFact &goalFact = snapshot.callbacks[goalCallback];
		requiredCellInfoCount = (goalFact.hasPathfindInfo ? 0 : 1) +
			(startFact.hasPathfindInfo ? 0 : 1);
		for (std::size_t i = 1; i < snapshot.callbackCount; ++i)
		{
			const DirectPathCellFact &fact = snapshot.callbacks[i];
			const bool isStart = samePoint(fact.x, fact.y,
				snapshot.startX, snapshot.startY);
			const bool isGoal = samePoint(fact.x, fact.y,
				snapshot.goalX, snapshot.goalY);
			// BuildLegacySupercoverCallbacks already proved a monotone,
			// coordinate-unique stream above.
			if (!isStart && !isGoal)
			{
				if (!fact.hasPathfindInfo)
					++requiredCellInfoCount;
				++openCellCountAfterGoal;
			}
		}

		for (std::size_t i = 0; i < snapshot.startNeighborCount; ++i)
		{
			const DirectPathCellFact &fact = snapshot.startNeighbors[i];
			bool alreadyDiscovered = false;
			for (std::size_t callback = 0;
				!alreadyDiscovered && callback < snapshot.callbackCount; ++callback)
			{
				alreadyDiscovered = samePoint(snapshot.callbacks[callback].x,
					snapshot.callbacks[callback].y, fact.x, fact.y);
			}
			for (std::size_t earlier = 0; !alreadyDiscovered && earlier < i; ++earlier)
			{
				alreadyDiscovered = samePoint(snapshot.startNeighbors[earlier].x,
					snapshot.startNeighbors[earlier].y, fact.x, fact.y);
			}
			if (alreadyDiscovered)
				continue;
			if (!fact.hasPathfindInfo)
			{
				++requiredCellInfoCount;
				++startNeighborAllocationCount;
			}
			if (!samePoint(fact.x, fact.y, snapshot.startX, snapshot.startY) &&
				!samePoint(fact.x, fact.y, snapshot.goalX, snapshot.goalY))
			{
				++openCellCountAfterGoal;
			}
		}
		cumulativeCellCount = openCellCountAfterGoal + 1;
	}

	return result.requiredCellInfoCount == requiredCellInfoCount &&
		result.startNeighborAllocationCount == startNeighborAllocationCount &&
		result.openCellCountAfterGoal == openCellCountAfterGoal &&
		result.cumulativeCellCount == cumulativeCellCount &&
		requiredCellInfoCount <= snapshot.availableCellInfoCount &&
		requiredCellInfoCount <= currentAvailableCellInfoCount;
}

bool IsDirectPathResultCurrent(const DirectPathSearchResult &result,
	std::uint32_t topologyOccupancyGeneration,
	std::uint32_t requestToken, std::uint32_t objectId) noexcept
{
	return result.status == DIRECT_PATH_FOUND &&
		result.topologyOccupancyGeneration == topologyOccupancyGeneration &&
		result.requestToken == requestToken && result.objectId == objectId;
}

bool IsDirectPathAdvisoryFallbackStatus(
	DirectPathSearchStatus status) noexcept
{
	return status == DIRECT_PATH_UNSUPPORTED_SUBSET ||
		status == DIRECT_PATH_NO_PATH ||
		status == DIRECT_PATH_CELL_INFO_SHORTAGE;
}

bool IsDirectPathAuthorityAllowed(
	const DirectPathAuthorityPolicy &policy) noexcept
{
	// Shadow mode may execute diagnostics, but only the explicitly selected
	// parallel product mode can authorize owner-side legacy materialization.
	if (!policy.parallelExecutionMode || policy.shadowExecutionMode)
		return false;
	if ((policy.networkGame || policy.multiplayerGame) &&
		(!policy.networkGame || !policy.multiplayerPolicyEnabled))
		return false;
	if (policy.zeroHourTitle)
	{
		return !policy.replayGame || policy.replayUsesCurrentPathEpoch;
	}
	// Generals has no direct-path replay epoch.  Keep both recording and
	// playback on the legacy lane until such an epoch is published.
	return !policy.recordingGame && !policy.replayGame &&
		policy.runtimeUsesCurrentGeneralsEpoch;
}

bool IsFixedPathfindingSemanticsAllowed(
	const FixedPathfindingSemanticsPolicy &policy) noexcept
{
	if (!policy.nativeRuntime || policy.networkGame || policy.multiplayerGame)
		return false;
	if (policy.zeroHourTitle)
	{
		// Current Zero Hour recordings publish the pathfinding-capacity epoch.
		// Marked playback keeps the same fixed serial semantics, but replay is
		// separately excluded from ordinary worker authority by the owner.
		return !policy.replayGame || policy.replayUsesCurrentPathEpoch;
	}
	// Generals has no pathfinding replay marker.  Its current native epoch is
	// therefore live-only; both recording and replay stay on retail semantics.
	return policy.runtimeUsesCurrentGeneralsEpoch && !policy.recordingGame &&
		!policy.replayGame;
}

bool IsOrdinaryPathAuthorityAllowed(
	const DirectPathAuthorityPolicy &policy,
	bool fixedPathfindingSemantics) noexcept
{
	// Ordinary A* has no multiplayer or replay proven bit yet.  Even a marked
	// Zero Hour replay that uses fixed serial semantics cannot accept worker
	// authority; the marker preserves playback compatibility only.
	return fixedPathfindingSemantics && !policy.networkGame &&
		!policy.multiplayerGame && !policy.replayGame &&
		IsDirectPathAuthorityAllowed(policy);
}

bool ShouldCaptureOrdinaryNavigationSnapshot(std::size_t eligibleRequestCount,
	bool shadowMode, bool authorityMode, std::size_t workerCount) noexcept
{
	if (workerCount == 0 || eligibleRequestCount == 0)
		return false;
	if (shadowMode)
		return true;
	return authorityMode && workerCount > 1 && eligibleRequestCount >= 2;
}

bool IsDirectPathRawStartEligible(float rawX, float rawY, float rawZ,
	float clippedX, float clippedY, float clippedZ,
	bool rawMovementPositionValid) noexcept
{
	return rawMovementPositionValid && rawX == clippedX && rawY == clippedY &&
		rawZ == clippedZ;
}

} // namespace rts
