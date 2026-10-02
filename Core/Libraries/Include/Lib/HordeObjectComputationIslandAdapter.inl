/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Included once by each title's HordeObjectComputationIsland.cpp. Keeping the
// owner adapter shared prevents the Generals and Zero Hour safety contracts
// from drifting while their legacy public headers remain independent.
#include "GameLogic/HordeObjectComputationIsland.h"

#if defined(_WIN64)

#include "Lib/ObjectComputationIsland.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/SimulationExecutionPolicy.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/HordeUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"

#include <algorithm>
#include <limits.h>
#include <new>

namespace
{
const UnsignedInt HORDE_ISLAND_MAX_OBJECTS = 8192;
const UnsignedInt HORDE_ISLAND_MAX_MODULES = 1024;
const UnsignedInt HORDE_ISLAND_MAX_SPATIAL_CELLS = 262144;
const UnsignedInt HORDE_ISLAND_MAX_SPATIAL_MEMBERSHIPS = 262144;
const UnsignedInt HORDE_ISLAND_MINIMUM_DUE_MODULES = 8;

Bool s_hordeObjectComputationCircuitBreaker = FALSE;
UnsignedInt s_hordeObjectComputationGeneration = 1;
UnsignedInt s_hordeObjectComputationMatchEpoch = 1;

struct HordeObjectCapture
{
	rts::SimulationReadObjectRecord record;
	Object *owner;
};

struct HordeModuleCapture
{
	rts::SimulationReadModuleRecord record;
	HordeUpdate *owner;
	HordeUpdate::ObjectComputationOwnerSnapshot snapshot;
	UnsignedInt ownerOrder;
};

struct HordeHeapNode
{
	UpdateModulePtr update;
	UnsignedInt priority;
};

Bool objectCaptureLess(const HordeObjectCapture &left,
	const HordeObjectCapture &right)
{
	return left.record.objectID < right.record.objectID;
}

Bool moduleCaptureLess(const HordeModuleCapture &left,
	const HordeModuleCapture &right)
{
	if (left.record.objectID != right.record.objectID)
		return left.record.objectID < right.record.objectID;
	if (left.record.moduleOrdinal != right.record.moduleOrdinal)
		return left.record.moduleOrdinal < right.record.moduleOrdinal;
	return left.record.moduleType < right.record.moduleType;
}

Bool updateEnabled(UpdateModulePtr update)
{
	if (update == 0 || update->friend_getObject() == 0) return FALSE;
	const DisabledMaskType disabled =
		update->friend_getObject()->getDisabledFlags();
#if RETAIL_COMPATIBLE_CRC
	return !disabled.any() ||
		disabled.anyIntersectionWith(update->getDisabledTypesToProcess());
#else
	return update->getDisabledTypesToProcess().testForAll(disabled);
#endif
}

Bool identifyHordeUpdate(UpdateModulePtr update, HordeUpdate **horde,
	UnsignedInt *moduleOrdinal)
{
	// This is the only allowlisted module. Its worker slice is limited to the
	// expensive bounding-sphere candidate scan. PartitionFilterHordeMember,
	// horde/AI/drawable mutation, scheduling, and all object resolution remain
	// on the owner in the original heap-root order.
	static NameKeyType hordeNameKey = NAMEKEY("HordeUpdate");
	if (horde == 0 || moduleOrdinal == 0 || update == 0 ||
		update->getModuleNameKey() != hordeNameKey)
		return FALSE;
	Object *object = const_cast<Object *>(update->friend_getObject());
	if (object == 0) return FALSE;
	BehaviorModule **modules = object->getBehaviorModules();
	for (UnsignedInt index = 0; modules[index] != 0; ++index)
	{
		if (modules[index]->getUpdate() != update) continue;
		*horde = static_cast<HordeUpdate *>(modules[index]);
		*moduleOrdinal = index;
		return *horde != 0;
	}
	return FALSE;
}

UnsignedInt scheduledPriority(UnsignedInt now, UpdateSleepTime sleep)
{
	if (sleep < UPDATE_SLEEP_NONE) sleep = UPDATE_SLEEP_NONE;
	UnsignedInt frame = now + static_cast<UnsignedInt>(sleep);
	if (frame > UPDATE_SLEEP_FOREVER) frame = UPDATE_SLEEP_FOREVER;
	return (frame << 2) | PHASE_NORMAL;
}

void rebalanceSimulatedRoot(HordeHeapNode *heap, UnsignedInt count)
{
	UnsignedInt index = 0;
	UnsignedInt child = 1;
	while (child < count)
	{
		if (child + 1 < count &&
			heap[child].priority > heap[child + 1].priority)
			++child;
		if (heap[index].priority <= heap[child].priority) break;
		const HordeHeapNode temporary = heap[index];
		heap[index] = heap[child];
		heap[child] = temporary;
		index = child;
		child = index * 2 + 1;
	}
}

struct PreparedHordeState
{
	PreparedHordeState()
		: objectCaptures(0), objects(0), objectOwners(0), modules(0), moduleOwners(0),
		  snapshots(0), schedule(0), cells(0), spatialObjectIndices(0),
		  candidates(0), candidateIDs(0), oracleIDs(0), view(0),
		  objectCapacity(0), moduleCapacity(0), cellCapacity(0),
		  spatialObjectIndexCapacity(0), objectCount(0), moduleCount(0),
		  cellCountX(0), cellCountY(0), cellCount(0),
		  spatialObjectIndexCount(0), matchEpoch(0), next(0),
		  ready(FALSE), shadow(FALSE)
	{
	}

	~PreparedHordeState()
	{
		delete view;
		delete[] oracleIDs;
		delete[] candidateIDs;
		delete[] candidates;
		delete[] schedule;
		delete[] spatialObjectIndices;
		delete[] cells;
		delete[] snapshots;
		delete[] moduleOwners;
		delete[] modules;
		delete[] objectOwners;
		delete[] objects;
		delete[] objectCaptures;
	}

	void clearWave()
	{
		parallel.reset();
		reference.reset();
		delete view;
		view = 0;
		objectCount = 0;
		moduleCount = 0;
		cellCountX = 0;
		cellCountY = 0;
		cellCount = 0;
		spatialObjectIndexCount = 0;
		matchEpoch = 0;
		next = 0;
		ready = FALSE;
		shadow = FALSE;
		metrics = rts::ObjectComputationMetrics();
	}

	void releaseStorage()
	{
		clearWave();
		delete[] oracleIDs; oracleIDs = 0;
		delete[] candidateIDs; candidateIDs = 0;
		delete[] candidates; candidates = 0;
		delete[] objectOwners; objectOwners = 0;
		delete[] objects; objects = 0;
		delete[] objectCaptures; objectCaptures = 0;
		delete[] schedule; schedule = 0;
		delete[] snapshots; snapshots = 0;
		delete[] moduleOwners; moduleOwners = 0;
		delete[] modules; modules = 0;
		delete[] spatialObjectIndices; spatialObjectIndices = 0;
		delete[] cells; cells = 0;
		objectCapacity = moduleCapacity = cellCapacity = 0;
		spatialObjectIndexCapacity = 0;
	}

	HordeObjectCapture *objectCaptures;
	rts::SimulationReadObjectRecord *objects;
	Object **objectOwners;
	rts::SimulationReadModuleRecord *modules;
	HordeUpdate **moduleOwners;
	HordeUpdate::ObjectComputationOwnerSnapshot *snapshots;
	rts::SimulationReadScheduleEntry *schedule;
	rts::SimulationReadSpatialCellSpan *cells;
	UnsignedInt *spatialObjectIndices;
	Object **candidates;
	ObjectID *candidateIDs;
	ObjectID *oracleIDs;
	rts::SimulationReadView *view;
	rts::ObjectComputationIsland parallel;
	rts::ObjectComputationIsland reference;
	rts::ObjectComputationMetrics metrics;
	UnsignedInt objectCapacity;
	UnsignedInt moduleCapacity;
	UnsignedInt cellCapacity;
	UnsignedInt spatialObjectIndexCapacity;
	UnsignedInt objectCount;
	UnsignedInt moduleCount;
	UnsignedInt cellCountX;
	UnsignedInt cellCountY;
	UnsignedInt cellCount;
	UnsignedInt spatialObjectIndexCount;
	UnsignedInt matchEpoch;
	UnsignedInt next;
	Bool ready;
	Bool shadow;
};

PreparedHordeState s_reusableHordeState;
Bool s_reusableHordeStateLeased = FALSE;
Bool s_releaseHordeStorageOnReturn = FALSE;

Bool captureContiguousHordePrefix(
	const std::vector<UpdateModulePtr> &sleepyUpdates, UnsignedInt now,
	HordeModuleCapture *captures, UnsignedInt captureCapacity,
	UnsignedInt *captureCount)
{
	if (captures == 0 || captureCount == 0 || sleepyUpdates.empty())
		return FALSE;
	*captureCount = 0;
	if (sleepyUpdates.size() > HORDE_ISLAND_MAX_OBJECTS * 4)
		return FALSE;
	HordeHeapNode *heap = new (std::nothrow)
		HordeHeapNode[sleepyUpdates.size()];
	if (heap == 0) return FALSE;
	for (UnsignedInt index = 0; index != sleepyUpdates.size(); ++index)
	{
		heap[index].update = sleepyUpdates[index];
		heap[index].priority = sleepyUpdates[index] != 0 ?
			sleepyUpdates[index]->friend_getPriority() : ~0u;
	}

	while (*captureCount < captureCapacity)
	{
		UpdateModulePtr update = heap[0].update;
		if (update == 0 || (heap[0].priority >> 2) > now ||
			(heap[0].priority & 3) != PHASE_NORMAL || !updateEnabled(update))
			break;
		HordeUpdate *horde = 0;
		UnsignedInt ordinal = 0;
		if (!identifyHordeUpdate(update, &horde, &ordinal) ||
			!horde->isObjectComputationScanDue(now))
			break;

		HordeModuleCapture &capture = captures[*captureCount];
		if (!horde->captureObjectComputationOwnerSnapshot(now, ordinal,
			capture.snapshot))
			break;
		capture.record.objectID = static_cast<UnsignedInt>(
			update->friend_getObject()->getID());
		capture.record.moduleOrdinal = ordinal;
		capture.record.moduleType = rts::OBJECT_COMPUTATION_MODULE_HORDE;
		capture.record.wakePriority = update->friend_getPriority();
		capture.record.queryRadius =
			capture.snapshot.moduleData->m_minDist;
		capture.owner = horde;
		capture.ownerOrder = *captureCount;
		++*captureCount;

		heap[0].priority = scheduledPriority(now,
			horde->objectComputationSleepTime());
		rebalanceSimulatedRoot(heap,
			static_cast<UnsignedInt>(sleepyUpdates.size()));
	}
	delete[] heap;
	return *captureCount >= 2;
}

Bool ensurePreparedStorage(PreparedHordeState &state,
	UnsignedInt objectCount, UnsignedInt moduleCount, UnsignedInt cellCount,
	UnsignedInt spatialObjectIndexCount)
{
	if (objectCount > state.objectCapacity)
	{
		rts::SimulationReadObjectRecord *objects = new (std::nothrow)
			rts::SimulationReadObjectRecord[objectCount];
		HordeObjectCapture *captures = new (std::nothrow)
			HordeObjectCapture[objectCount];
		Object **owners = new (std::nothrow) Object *[objectCount];
		Object **candidates = new (std::nothrow) Object *[objectCount];
		ObjectID *candidateIDs = new (std::nothrow) ObjectID[objectCount];
		ObjectID *oracleIDs = new (std::nothrow) ObjectID[objectCount];
		if (objects == 0 || captures == 0 || owners == 0 || candidates == 0 ||
			candidateIDs == 0 || oracleIDs == 0)
		{
			delete[] oracleIDs; delete[] candidateIDs; delete[] candidates;
			delete[] owners; delete[] captures; delete[] objects;
			return FALSE;
		}
		delete[] state.oracleIDs; delete[] state.candidateIDs;
		delete[] state.candidates; delete[] state.objectOwners;
		delete[] state.objects; delete[] state.objectCaptures;
		state.objects = objects; state.objectOwners = owners;
		state.objectCaptures = captures;
		state.candidates = candidates; state.candidateIDs = candidateIDs;
		state.oracleIDs = oracleIDs; state.objectCapacity = objectCount;
	}
	if (moduleCount > state.moduleCapacity)
	{
		rts::SimulationReadModuleRecord *modules = new (std::nothrow)
			rts::SimulationReadModuleRecord[moduleCount];
		HordeUpdate **owners = new (std::nothrow) HordeUpdate *[moduleCount];
		HordeUpdate::ObjectComputationOwnerSnapshot *snapshots =
			new (std::nothrow)
			HordeUpdate::ObjectComputationOwnerSnapshot[moduleCount];
		rts::SimulationReadScheduleEntry *schedule = new (std::nothrow)
			rts::SimulationReadScheduleEntry[moduleCount];
		if (modules == 0 || owners == 0 || snapshots == 0 || schedule == 0)
		{
			delete[] schedule; delete[] snapshots; delete[] owners;
			delete[] modules;
			return FALSE;
		}
		delete[] state.schedule; delete[] state.snapshots;
		delete[] state.moduleOwners; delete[] state.modules;
		state.modules = modules; state.moduleOwners = owners;
		state.snapshots = snapshots; state.schedule = schedule;
		state.moduleCapacity = moduleCount;
	}
	if (cellCount > state.cellCapacity)
	{
		rts::SimulationReadSpatialCellSpan *cells = new (std::nothrow)
			rts::SimulationReadSpatialCellSpan[cellCount];
		if (cells == 0) return FALSE;
		delete[] state.cells; state.cells = cells; state.cellCapacity = cellCount;
	}
	if (spatialObjectIndexCount > state.spatialObjectIndexCapacity)
	{
		UnsignedInt *indices = new (std::nothrow)
			UnsignedInt[spatialObjectIndexCount];
		if (indices == 0) return FALSE;
		delete[] state.spatialObjectIndices;
		state.spatialObjectIndices = indices;
		state.spatialObjectIndexCapacity = spatialObjectIndexCount;
	}
	return state.objectCaptures != 0 && state.objects != 0 && state.objectOwners != 0 &&
		state.modules != 0 && state.moduleOwners != 0 &&
		state.snapshots != 0 && state.schedule != 0 &&
		state.candidates != 0 && state.candidateIDs != 0 &&
		state.oracleIDs != 0 && state.cells != 0 &&
		state.spatialObjectIndices != 0;
}

Bool countSpatialCapture(GameLogic *logic, UnsignedInt *objectCount,
	UnsignedInt *cellCountX, UnsignedInt *cellCountY,
	UnsignedInt *nonemptyCellCount, UnsignedInt *membershipCount,
	UnsignedInt *captureMemberVisits)
{
	if (logic == 0 || ThePartitionManager == 0 || objectCount == 0 ||
		cellCountX == 0 || cellCountY == 0 || nonemptyCellCount == 0 ||
		membershipCount == 0 || captureMemberVisits == 0)
		return FALSE;
	*objectCount = *nonemptyCellCount = *membershipCount = 0;
	*captureMemberVisits = 0;
	const Int cellsX = ThePartitionManager->getCellCountX();
	const Int cellsY = ThePartitionManager->getCellCountY();
	if (cellsX <= 0 || cellsY <= 0 ||
		static_cast<UnsignedInt>(cellsY) > ~static_cast<UnsignedInt>(0) /
			static_cast<UnsignedInt>(cellsX) ||
		static_cast<UnsignedInt>(cellsX) * static_cast<UnsignedInt>(cellsY) >
			HORDE_ISLAND_MAX_SPATIAL_CELLS)
		return FALSE;
	*cellCountX = static_cast<UnsignedInt>(cellsX);
	*cellCountY = static_cast<UnsignedInt>(cellsY);
	for (Object *object = logic->getFirstObject(); object != 0;
		object = object->getNextObject())
	{
		PartitionData *data = object->friend_getPartitionData();
		if (data == 0 || data->friend_getCoiInUseCount() <= 0) continue;
		if (++*objectCount > HORDE_ISLAND_MAX_OBJECTS) return FALSE;
	}
	for (Int y = 0; y < cellsY; ++y)
	{
		for (Int x = 0; x < cellsX; ++x)
		{
			UnsignedInt realMembers = 0;
			PartitionCell *cell = ThePartitionManager->getCellAt(x, y);
			if (cell == 0) return FALSE;
			for (CellAndObjectIntersection *coi = cell->getFirstCoiInCell();
				coi != 0; coi = coi->getNextCoi())
			{
				if (*captureMemberVisits == ~static_cast<UnsignedInt>(0))
					return FALSE;
				++*captureMemberVisits;
				PartitionData *data = coi->getModule();
				if (data != 0 && data->getObject() != 0)
				{
					if (realMembers == ~static_cast<UnsignedInt>(0)) return FALSE;
					++realMembers;
				}
			}
			if (realMembers == 0) continue;
			if (++*nonemptyCellCount > HORDE_ISLAND_MAX_SPATIAL_CELLS ||
				realMembers > HORDE_ISLAND_MAX_SPATIAL_MEMBERSHIPS -
					*membershipCount)
				return FALSE;
			*membershipCount += realMembers;
		}
	}
	return *objectCount != 0 && *nonemptyCellCount != 0 &&
		*membershipCount != 0;
}

Bool captureObjects(GameLogic *logic, PreparedHordeState &state)
{
	UnsignedInt objectCount = 0;
	for (Object *object = logic->getFirstObject(); object != 0;
		object = object->getNextObject())
	{
		PartitionData *data = object->friend_getPartitionData();
		if (data == 0 || data->friend_getCoiInUseCount() <= 0) continue;
		if (++objectCount > HORDE_ISLAND_MAX_OBJECTS) return FALSE;
	}
	if (objectCount == 0) return FALSE;
	HordeObjectCapture *captures = state.objectCaptures;
	if (captures == 0 || objectCount > state.objectCapacity) return FALSE;
	UnsignedInt captured = 0;
	for (Object *object = logic->getFirstObject(); object != 0;
		object = object->getNextObject())
	{
		PartitionData *data = object->friend_getPartitionData();
		if (data == 0 || data->friend_getCoiInUseCount() <= 0) continue;
		const Coord3D *position = object->getPosition();
		const GeometryInfo &geometry = object->getGeometryInfo();
		HordeObjectCapture &capture = captures[captured++];
		capture.record.objectID = static_cast<UnsignedInt>(object->getID());
		capture.record.objectGeneration = object->getMotionGeneration();
		capture.record.positionX = position->x;
		capture.record.positionY = position->y;
		capture.record.positionZ = position->z;
		capture.record.boundingSphereRadius =
			geometry.getBoundingSphereRadius();
		capture.record.zCenterOffset = geometry.getZDeltaToCenterPosition();
		capture.owner = object;
	}
	std::sort(captures, captures + captured, objectCaptureLess);
	for (UnsignedInt index = 0; index != captured; ++index)
	{
		if (index != 0 && captures[index - 1].record.objectID >=
			captures[index].record.objectID)
		{
			return FALSE;
		}
		state.objects[index] = captures[index].record;
		state.objectOwners[index] = captures[index].owner;
	}
	state.objectCount = captured;
	return TRUE;
}

UnsignedInt findCapturedObjectIndex(const PreparedHordeState &state,
	Object *object)
{
	if (object == 0) return ~static_cast<UnsignedInt>(0);
	const UnsignedInt objectID = static_cast<UnsignedInt>(object->getID());
	UnsignedInt low = 0;
	UnsignedInt high = state.objectCount;
	while (low < high)
	{
		const UnsignedInt middle = low + (high - low) / 2;
		if (state.objects[middle].objectID < objectID) low = middle + 1;
		else high = middle;
	}
	return low < state.objectCount && state.objects[low].objectID == objectID &&
		state.objectOwners[low] == object ? low : ~static_cast<UnsignedInt>(0);
}

Bool captureSpatialCells(PreparedHordeState &state,
	UnsignedInt expectedCellCount, UnsignedInt expectedMembershipCount)
{
	for (UnsignedInt object = 0; object < state.objectCount; ++object)
		state.candidates[object] = 0;
	UnsignedInt capturedCells = 0;
	UnsignedInt capturedMembers = 0;
	for (UnsignedInt y = 0; y < state.cellCountY; ++y)
	{
		for (UnsignedInt x = 0; x < state.cellCountX; ++x)
		{
			PartitionCell *cell = ThePartitionManager->getCellAt(
				static_cast<Int>(x), static_cast<Int>(y));
			if (cell == 0) return FALSE;
			const UnsignedInt begin = capturedMembers;
			for (CellAndObjectIntersection *coi = cell->getFirstCoiInCell();
				coi != 0; coi = coi->getNextCoi())
			{
				PartitionData *data = coi->getModule();
				Object *object = data != 0 ? data->getObject() : 0;
				if (object == 0) continue;
				if (capturedMembers >= expectedMembershipCount) return FALSE;
				const UnsignedInt objectIndex = findCapturedObjectIndex(state,
					object);
				if (objectIndex == ~static_cast<UnsignedInt>(0)) return FALSE;
				state.spatialObjectIndices[capturedMembers++] = objectIndex;
				state.candidates[objectIndex] = object;
			}
			if (capturedMembers == begin) continue;
			if (capturedCells >= expectedCellCount) return FALSE;
			std::sort(state.spatialObjectIndices + begin,
				state.spatialObjectIndices + capturedMembers);
			for (UnsignedInt member = begin + 1; member < capturedMembers;
				++member)
				if (state.spatialObjectIndices[member - 1] >=
					state.spatialObjectIndices[member])
					return FALSE;
			rts::SimulationReadSpatialCellSpan &span =
				state.cells[capturedCells++];
			span.cellIndex = y * state.cellCountX + x;
			span.objectIndexOffset = begin;
			span.objectIndexCount = capturedMembers - begin;
		}
	}
	if (capturedCells != expectedCellCount ||
		capturedMembers != expectedMembershipCount)
		return FALSE;
	for (UnsignedInt object = 0; object < state.objectCount; ++object)
		if (state.candidates[object] != state.objectOwners[object]) return FALSE;
	state.cellCount = capturedCells;
	state.spatialObjectIndexCount = capturedMembers;
	return TRUE;
}

struct HordeSpatialQueryBounds
{
	UnsignedInt minX;
	UnsignedInt maxX;
	UnsignedInt minY;
	UnsignedInt maxY;
	UnsignedInt ownerCellIndex;
	UnsignedInt radius;
	UnsignedInt legacyRadius;
};

Bool calculateSpatialQueryBounds(const HordeModuleCapture &capture,
	UnsignedInt cellCountX, UnsignedInt cellCountY,
	HordeSpatialQueryBounds *bounds)
{
	Object *owner = capture.owner != 0 ?
		const_cast<Object *>(capture.owner->friend_getObject()) : 0;
	if (owner == 0 || bounds == 0 || cellCountX == 0 || cellCountY == 0 ||
		static_cast<UnsignedInt>(owner->getID()) != capture.record.objectID ||
		capture.record.queryRadius != capture.record.queryRadius ||
		capture.record.queryRadius < 0.0f)
		return FALSE;
	Int cellX = 0;
	Int cellY = 0;
	ThePartitionManager->worldToCell(owner->getPosition()->x,
		owner->getPosition()->y, &cellX, &cellY);
	if (cellX < 0 || cellY < 0 ||
		static_cast<UnsignedInt>(cellX) >= cellCountX ||
		static_cast<UnsignedInt>(cellY) >= cellCountY)
		return FALSE;
	const UnsignedInt ownerCellX = static_cast<UnsignedInt>(cellX);
	const UnsignedInt ownerCellY = static_cast<UnsignedInt>(cellY);
	const UnsignedInt farthestX = ownerCellX > cellCountX - 1 - ownerCellX ?
		ownerCellX : cellCountX - 1 - ownerCellX;
	const UnsignedInt farthestY = ownerCellY > cellCountY - 1 - ownerCellY ?
		ownerCellY : cellCountY - 1 - ownerCellY;
	const UnsignedInt farthestRadius = farthestX > farthestY ?
		farthestX : farthestY;
	UnsignedInt radius = farthestRadius;
	UnsignedInt legacyQueryRadius = farthestRadius;
	if (capture.record.queryRadius < HUGE_DIST)
	{
		const Real scaledRadius = capture.record.queryRadius *
			ThePartitionManager->getCellSizeInv();
		if (scaledRadius != scaledRadius || scaledRadius < 0.0f ||
			scaledRadius > static_cast<Real>(INT_MAX - 1))
			return FALSE;
		const Int legacyRadius = ThePartitionManager->worldToCellDist(
			capture.record.queryRadius);
		if (legacyRadius < 0) return FALSE;
		const UnsignedInt conservativeRadius =
			static_cast<UnsignedInt>(legacyRadius) + 1;
		legacyQueryRadius = static_cast<UnsignedInt>(legacyRadius) <
			farthestRadius ? static_cast<UnsignedInt>(legacyRadius) :
			farthestRadius;
		if (conservativeRadius < radius) radius = conservativeRadius;
	}
	bounds->minX = ownerCellX > radius ? ownerCellX - radius : 0;
	bounds->minY = ownerCellY > radius ? ownerCellY - radius : 0;
	bounds->maxX = radius < cellCountX - 1 - ownerCellX ?
		ownerCellX + radius : cellCountX - 1;
	bounds->maxY = radius < cellCountY - 1 - ownerCellY ?
		ownerCellY + radius : cellCountY - 1;
	bounds->ownerCellIndex = ownerCellY * cellCountX + ownerCellX;
	bounds->radius = radius;
	bounds->legacyRadius = legacyQueryRadius;
	return TRUE;
}

Int legacyMinimumRadiusForOffset(Int offsetX, Int offsetY, Real cellSize)
{
	const Real halfCell = cellSize * 0.5f;
	const Real centerX[4] = { -halfCell, halfCell, -halfCell, halfCell };
	const Real centerY[4] = { -halfCell, -halfCell, halfCell, halfCell };
	const Real x = offsetX * cellSize;
	const Real y = offsetY * cellSize;
	const Real otherX[4] = {
		x - halfCell, x + halfCell, x - halfCell, x + halfCell
	};
	const Real otherY[4] = {
		y - halfCell, y - halfCell, y + halfCell, y + halfCell
	};
	double minimumDistanceSquared = 1e12;
	for (Int first = 0; first < 4; ++first)
	{
		for (Int second = 0; second < 4; ++second)
		{
			const double deltaX = centerX[first] - otherX[second];
			const double deltaY = centerY[first] - otherY[second];
			const double distanceSquared = deltaX * deltaX + deltaY * deltaY;
			if (minimumDistanceSquared > distanceSquared)
				minimumDistanceSquared = distanceSquared;
		}
	}
	const double distance = sqrtf(minimumDistanceSquared);
	return REAL_TO_INT_CEIL(distance / cellSize);
}

Bool addSpatialVisit(rts::ImmutableSpatialMetricCounter *value)
{
	if (value == 0 || *value ==
		~static_cast<rts::ImmutableSpatialMetricCounter>(0))
		return FALSE;
	++*value;
	return TRUE;
}

void configureSpatialAdmissionCost(rts::ImmutableSpatialAdmissionCost &cost,
	UnsignedInt captureCount, UnsignedInt objectCount,
	UnsignedInt cellCountX, UnsignedInt cellCountY,
	UnsignedInt membershipCount,
	rts::ImmutableSpatialMetricCounter radiusOffsetCount)
{
	cost.queryCount = captureCount;
	cost.workerCount = rts::JobSystem::instance().workerCount();
	cost.objectCount = objectCount;
	cost.cellCount = static_cast<rts::ImmutableSpatialMetricCounter>(
		cellCountX) * cellCountY;
	cost.memberCount = membershipCount;
	cost.radiusOffsetCount = radiusOffsetCount;
	cost.rebuildTopology = true;
	cost.refreshFacts = false;
}

Bool admitSpatialCapture(const HordeModuleCapture *captures,
	UnsignedInt captureCount, UnsignedInt objectCount,
	UnsignedInt cellCountX, UnsignedInt cellCountY,
	UnsignedInt captureMemberVisits)
{
	if (captures == 0 || captureCount < HORDE_ISLAND_MINIMUM_DUE_MODULES ||
		ThePartitionManager == 0)
		return FALSE;
	rts::ImmutableSpatialMetricCounter workerCellVisits = 0;
	rts::ImmutableSpatialMetricCounter workerMemberVisits = 0;
	rts::ImmutableSpatialMetricCounter legacyCellVisits = 0;
	rts::ImmutableSpatialMetricCounter legacyMemberVisits = 0;
	const Real cellSize = ThePartitionManager->getCellSize();
	if (cellSize != cellSize || cellSize <= 0.0f) return FALSE;
	for (UnsignedInt module = 0; module < captureCount; ++module)
	{
		HordeSpatialQueryBounds bounds;
		if (!calculateSpatialQueryBounds(captures[module], cellCountX,
			cellCountY, &bounds))
			return FALSE;
		const UnsignedInt ownerCellX = bounds.ownerCellIndex % cellCountX;
		const UnsignedInt ownerCellY = bounds.ownerCellIndex / cellCountX;
		for (UnsignedInt y = bounds.minY; y <= bounds.maxY; ++y)
		{
			for (UnsignedInt x = bounds.minX; x <= bounds.maxX; ++x)
			{
				PartitionCell *cell = ThePartitionManager->getCellAt(
					static_cast<Int>(x), static_cast<Int>(y));
				if (cell == 0 || !addSpatialVisit(&workerCellVisits)) return FALSE;
#ifdef FASTER_GCO
				const Int legacyMinimumRadius = legacyMinimumRadiusForOffset(
					static_cast<Int>(x) - static_cast<Int>(ownerCellX),
					static_cast<Int>(y) - static_cast<Int>(ownerCellY), cellSize);
				const Bool legacyVisitsCell = legacyMinimumRadius >= 0 &&
					static_cast<UnsignedInt>(legacyMinimumRadius) <=
						bounds.legacyRadius;
#else
				const Bool legacyVisitsCell =
					cell->getFirstCoiInCell() != 0;
#endif
				if (legacyVisitsCell && !addSpatialVisit(&legacyCellVisits))
					return FALSE;
				for (CellAndObjectIntersection *coi =
					cell->getFirstCoiInCell(); coi != 0;
					coi = coi->getNextCoi())
				{
					if (legacyVisitsCell &&
						!addSpatialVisit(&legacyMemberVisits)) return FALSE;
					PartitionData *data = coi->getModule();
					if (data != 0 && data->getObject() != 0 &&
						!addSpatialVisit(&workerMemberVisits)) return FALSE;
				}
			}
		}
	}
	const rts::ImmutableSpatialMetricCounter width =
		static_cast<rts::ImmutableSpatialMetricCounter>(cellCountX) * 2 - 1;
	const rts::ImmutableSpatialMetricCounter height =
		static_cast<rts::ImmutableSpatialMetricCounter>(cellCountY) * 2 - 1;
	if (width != 0 && height >
		~static_cast<rts::ImmutableSpatialMetricCounter>(0) / width)
		return FALSE;
	rts::ImmutableSpatialAdmissionCost legacyCost;
	rts::ImmutableSpatialAdmissionCost workerCost;
	configureSpatialAdmissionCost(legacyCost, captureCount, objectCount,
		cellCountX, cellCountY, captureMemberVisits, width * height);
	configureSpatialAdmissionCost(workerCost, captureCount, objectCount,
		cellCountX, cellCountY, captureMemberVisits, width * height);
	legacyCost.queryCellVisits = legacyCellVisits;
	legacyCost.queryMemberVisits = legacyMemberVisits;
	workerCost.queryCellVisits = workerCellVisits;
	workerCost.queryMemberVisits = workerMemberVisits;
	rts::ImmutableSpatialMetricCounter legacyTransactionCost = 0;
	rts::ImmutableSpatialMetricCounter parallelTransactionCost = 0;
	rts::ImmutableSpatialMetricCounter unusedCost = 0;
	const rts::ImmutableSpatialAdmissionResult legacyResult =
		rts::EvaluateImmutableSpatialQueryAdmission(legacyCost,
			&legacyTransactionCost, &unusedCost);
	const rts::ImmutableSpatialAdmissionResult workerResult =
		rts::EvaluateImmutableSpatialQueryAdmission(workerCost,
			&unusedCost, &parallelTransactionCost);
	return legacyResult != rts::IMMUTABLE_SPATIAL_ADMISSION_INVALID &&
		workerResult != rts::IMMUTABLE_SPATIAL_ADMISSION_INVALID &&
		parallelTransactionCost < legacyTransactionCost;
}

Bool materializeModules(PreparedHordeState &state,
	HordeModuleCapture *captures, UnsignedInt captureCount)
{
	std::sort(captures, captures + captureCount, moduleCaptureLess);
	for (UnsignedInt index = 0; index != captureCount; ++index)
	{
		if (index != 0 &&
			!moduleCaptureLess(captures[index - 1], captures[index]))
			return FALSE;
		state.modules[index] = captures[index].record;
		HordeSpatialQueryBounds bounds;
		if (!calculateSpatialQueryBounds(captures[index], state.cellCountX,
			state.cellCountY, &bounds))
			return FALSE;
		state.modules[index].spatialCellIndex = bounds.ownerCellIndex;
		// A cell touched only at its boundary can still have zero minimum
		// distance to the owner cell. The extra ring is the legacy conservative
		// broad-phase allowance; the legacy float sphere test supplies exactness.
		state.modules[index].spatialCellRadius =
			bounds.radius;
		state.moduleOwners[index] = captures[index].owner;
		state.snapshots[index] = captures[index].snapshot;
	}
	for (UnsignedInt ownerOrder = 0; ownerOrder != captureCount; ++ownerOrder)
	{
		UnsignedInt moduleIndex = 0;
		while (moduleIndex != captureCount &&
			captures[moduleIndex].ownerOrder != ownerOrder)
			++moduleIndex;
		if (moduleIndex == captureCount) return FALSE;
		state.schedule[ownerOrder].moduleIndex = moduleIndex;
		state.schedule[ownerOrder].ownerOrder = ownerOrder;
	}
	state.moduleCount = captureCount;
	return TRUE;
}

Bool compareCandidateIDs(const ObjectID *left, UnsignedInt leftCount,
	const ObjectID *right, UnsignedInt rightCount)
{
	if (leftCount != rightCount) return FALSE;
	for (UnsignedInt index = 0; index != leftCount; ++index)
		if (left[index] != right[index]) return FALSE;
	return TRUE;
}

PreparedHordeState *acquireReusableState()
{
	if (s_reusableHordeStateLeased) return 0;
	s_reusableHordeStateLeased = TRUE;
	s_reusableHordeState.clearWave();
	return &s_reusableHordeState;
}

void releaseReusableState(PreparedHordeState *state)
{
	if (state != &s_reusableHordeState) return;
	state->clearWave();
	s_reusableHordeStateLeased = FALSE;
	if (s_releaseHordeStorageOnReturn)
	{
		state->releaseStorage();
		s_releaseHordeStorageOnReturn = FALSE;
	}
}
}

PreparedHordeObjectComputationIsland::PreparedHordeObjectComputationIsland()
	: m_state(0)
{
}

PreparedHordeObjectComputationIsland::~PreparedHordeObjectComputationIsland()
{
	releaseReusableState(static_cast<PreparedHordeState *>(m_state));
	m_state = 0;
}

void ResetHordeObjectComputationIslandForMatch()
{
	s_hordeObjectComputationCircuitBreaker = FALSE;
	++s_hordeObjectComputationMatchEpoch;
	if (s_hordeObjectComputationMatchEpoch == 0)
		s_hordeObjectComputationMatchEpoch = 1;
	if (s_reusableHordeStateLeased)
		s_releaseHordeStorageOnReturn = TRUE;
	else
		s_reusableHordeState.releaseStorage();
}

Bool PrepareHordeObjectComputationIsland(GameLogic *logic,
	const std::vector<UpdateModulePtr> &sleepyUpdates, UnsignedInt now,
	PreparedHordeObjectComputationIsland &prepared)
{
	releaseReusableState(static_cast<PreparedHordeState *>(prepared.m_state));
	prepared.m_state = 0;
	if (logic == 0 || s_hordeObjectComputationCircuitBreaker ||
		logic->getFrame() != now || sleepyUpdates.empty())
		return FALSE;
	rts::ObjectComputationOptions options;
	options.parallel = rts::PrepareSimulationCommandsOffThread();
	rts::ObjectComputationMetrics preflightMetrics;
	if (rts::PreflightObjectComputationIsland(options, &preflightMetrics) !=
		rts::OBJECT_COMPUTATION_PARALLEL)
		return FALSE;

	HordeModuleCapture *captures = new (std::nothrow)
		HordeModuleCapture[HORDE_ISLAND_MAX_MODULES];
	if (captures == 0) return FALSE;
	UnsignedInt moduleCount = 0;
	if (!captureContiguousHordePrefix(sleepyUpdates, now, captures,
		HORDE_ISLAND_MAX_MODULES, &moduleCount))
	{
		delete[] captures;
		return FALSE;
	}
	if (moduleCount < HORDE_ISLAND_MINIMUM_DUE_MODULES)
	{
		delete[] captures;
		return FALSE;
	}

	UnsignedInt objectCount = 0;
	UnsignedInt cellCountX = 0;
	UnsignedInt cellCountY = 0;
	UnsignedInt nonemptyCellCount = 0;
	UnsignedInt membershipCount = 0;
	UnsignedInt captureMemberVisits = 0;
	if (!countSpatialCapture(logic, &objectCount, &cellCountX, &cellCountY,
		&nonemptyCellCount, &membershipCount, &captureMemberVisits) ||
		!admitSpatialCapture(captures, moduleCount, objectCount, cellCountX,
			cellCountY, captureMemberVisits))
	{
		delete[] captures;
		return FALSE;
	}

	PreparedHordeState *state = acquireReusableState();
	if (state == 0)
	{
		delete[] captures;
		return FALSE;
	}
	state->cellCountX = cellCountX;
	state->cellCountY = cellCountY;
	if (!ensurePreparedStorage(*state, objectCount, moduleCount,
		nonemptyCellCount, membershipCount) ||
		!captureObjects(logic, *state) ||
		state->objectCount != objectCount ||
		!captureSpatialCells(*state, nonemptyCellCount, membershipCount) ||
		!materializeModules(*state, captures, moduleCount))
	{
		releaseReusableState(state);
		delete[] captures;
		return FALSE;
	}
	delete[] captures;

	UnsignedInt generation = s_hordeObjectComputationGeneration++;
	if (generation == 0)
	{
		generation = s_hordeObjectComputationGeneration++;
		if (generation == 0) generation = 1;
	}
	state->matchEpoch = s_hordeObjectComputationMatchEpoch;
	state->view = new (std::nothrow) rts::SimulationReadView(now, generation,
		state->objects, state->objectCount, state->modules, state->moduleCount,
		state->schedule, state->moduleCount, state->cellCountX,
		state->cellCountY, state->cells, state->cellCount,
		state->spatialObjectIndices, state->spatialObjectIndexCount);
	if (state->view == 0 || !state->view->isValid())
	{
		releaseReusableState(state);
		return FALSE;
	}

	const rts::ObjectComputationResult result = state->parallel.prepare(
		*state->view, options, &state->metrics);
	if (result != rts::OBJECT_COMPUTATION_PARALLEL)
	{
		releaseReusableState(state);
		return FALSE;
	}

	state->shadow = rts::UseSimulationShadowOracle() ? TRUE : FALSE;
	if (state->shadow)
	{
		rts::ObjectComputationOptions referenceOptions;
		UnsignedInt difference = 0;
		if (state->reference.prepare(*state->view, referenceOptions, 0) !=
			rts::OBJECT_COMPUTATION_SERIAL_REFERENCE ||
			!rts::ObjectComputationCommandsEqual(*state->view, state->parallel,
				state->reference, &difference))
		{
			s_hordeObjectComputationCircuitBreaker = TRUE;
			releaseReusableState(state);
			return FALSE;
		}
	}
	state->ready = state->shadow || rts::UseParallelSimulation();
	if (!state->ready)
	{
		releaseReusableState(state);
		return FALSE;
	}
	prepared.m_state = state;
	return TRUE;
}

Bool ConsumeHordeObjectComputationIsland(GameLogic *logic,
	PreparedHordeObjectComputationIsland &prepared, UpdateModulePtr update,
	UnsignedInt now, UpdateSleepTime &sleepLen)
{
	PreparedHordeState *state =
		static_cast<PreparedHordeState *>(prepared.m_state);
	if (logic == 0 || state == 0 || !state->ready ||
		state->next >= state->moduleCount || update == 0 ||
		state->matchEpoch != s_hordeObjectComputationMatchEpoch ||
		update->friend_getIndexInLogic() != 0 ||
		update->friend_getNextCallPhase() != PHASE_NORMAL)
		return FALSE;

	const rts::SimulationMergedCommand *merged =
		state->parallel.commandAt(state->next);
	rts::ObjectComputationCandidateSetHeader header;
	if (merged == 0 ||
		!rts::DecodeObjectComputationCandidateSet(*state->view, *merged,
			&header) ||
		merged->command()->orderKey().phase() != state->next ||
		header.moduleIndex >= state->moduleCount)
	{
		state->ready = FALSE;
		s_hordeObjectComputationCircuitBreaker = TRUE;
		return FALSE;
	}

	const UnsignedInt moduleIndex = header.moduleIndex;
	HordeUpdate *horde = state->moduleOwners[moduleIndex];
	Object *owner = const_cast<Object *>(update->friend_getObject());
	if (horde == 0 || owner == 0 || horde != update ||
		static_cast<UnsignedInt>(owner->getID()) !=
			state->modules[moduleIndex].objectID ||
		logic->findObjectByID(owner->getID()) != owner ||
		logic->getFrame() != now ||
		!horde->validateObjectComputationOwnerSnapshot(
			state->snapshots[moduleIndex]))
	{
		state->ready = FALSE;
		s_hordeObjectComputationCircuitBreaker = TRUE;
		return FALSE;
	}

	UnsignedInt candidateCount = 0;
	for (UnsignedInt candidateOrdinal = 0;
		candidateOrdinal != header.candidateCount; ++candidateOrdinal)
	{
		UnsignedInt objectIndex = 0;
		if (!rts::ObjectComputationCandidateIndexAt(*state->view, *merged,
			candidateOrdinal, &objectIndex) || objectIndex >= state->objectCount)
		{
			state->ready = FALSE;
			s_hordeObjectComputationCircuitBreaker = TRUE;
			return FALSE;
		}
		Object *candidate = logic->findObjectByID(static_cast<ObjectID>(
			state->objects[objectIndex].objectID));
		PartitionData *partitionData = candidate != 0 ?
			candidate->friend_getPartitionData() : 0;
		if (candidate == 0 || candidate != state->objectOwners[objectIndex] ||
			partitionData == 0 ||
			partitionData->friend_getCoiInUseCount() <= 0 ||
			candidate->getMotionGeneration() !=
				state->objects[objectIndex].objectGeneration)
		{
			state->ready = FALSE;
			s_hordeObjectComputationCircuitBreaker = TRUE;
			return FALSE;
		}
		state->candidates[candidateCount] = candidate;
		state->candidateIDs[candidateCount] = candidate->getID();
		++candidateCount;
	}

	if (state->shadow)
	{
		UnsignedInt oracleCount = 0;
		if (!horde->collectObjectComputationCandidateOracle(state->oracleIDs,
			state->objectCount, &oracleCount) ||
			!compareCandidateIDs(state->candidateIDs, candidateCount,
				state->oracleIDs, oracleCount))
		{
			state->ready = FALSE;
			s_hordeObjectComputationCircuitBreaker = TRUE;
		}
		else
		{
			++state->next;
		}
		return FALSE;
	}

	sleepLen = horde->updateFromObjectComputationCandidates(
		state->candidates, candidateCount);
	++state->next;
	return TRUE;
}

#else

PreparedHordeObjectComputationIsland::PreparedHordeObjectComputationIsland()
	: m_state(0)
{
}

PreparedHordeObjectComputationIsland::~PreparedHordeObjectComputationIsland()
{
}

void ResetHordeObjectComputationIslandForMatch()
{
}

Bool PrepareHordeObjectComputationIsland(GameLogic *,
	const std::vector<UpdateModulePtr> &, UnsignedInt,
	PreparedHordeObjectComputationIsland &)
{
	return FALSE;
}

Bool ConsumeHordeObjectComputationIsland(GameLogic *,
	PreparedHordeObjectComputationIsland &, UpdateModulePtr,
	UnsignedInt, UpdateSleepTime &)
{
	return FALSE;
}

#endif
