/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "PreRTS.h"

#include "GameLogic/ImmutableSpatialQueryRuntime.h"

Bool ShouldCaptureLiveImmutableSpatialArena(Bool alreadyCaptured,
	Bool dueNormalModule)
{
	return !alreadyCaptured && dueNormalModule;
}

void CommitLiveImmutableSpatialObjectSequence(Object *const *objects,
	UnsignedInt count, LiveImmutableSpatialObjectCommitCallback callback,
	void *context)
{
	if (objects == nullptr || callback == nullptr)
		return;
	for (UnsignedInt index = 0; index != count; ++index)
		callback(objects[index], context);
}

#if defined(_WIN64)

#include "Common/Geometry.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/MultiplayerSimulationRuntimePolicy.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/SimulationExecutionPolicy.h"

#include <algorithm>
#include <limits.h>
#include <new>
#include <string.h>

namespace
{
enum LiveImmutableSpatialCollectionState
{
	LIVE_SPATIAL_COLLECTION_IDLE = 0,
	LIVE_SPATIAL_COLLECTION_COLLECTING,
	LIVE_SPATIAL_COLLECTION_POLICY_FALLBACK,
	LIVE_SPATIAL_COLLECTION_FAILED,
	LIVE_SPATIAL_COLLECTION_READY
};

enum
{
	LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES =
		rts::ImmutableSpatialCollectionCompletion::MAXIMUM_QUERIES,
	LIVE_IMMUTABLE_SPATIAL_MAXIMUM_RESULT_SLOTS = 1024 * 1024
};

struct LiveImmutableSpatialPreparedOwner
{
	UpdateModule *owner;
	ObjectID objectID;
	LiveImmutableSpatialConsumer consumer;
	UnsignedInt batchIndex;
	UnsignedInt wakePriority;
};

struct LiveImmutableSpatialOwnerIndexEntry
{
	UnsignedInt objectID;
	UnsignedInt batchIndex;
};

class SpatialPerformanceInterval
{
public:
	SpatialPerformanceInterval(
		rts::performance::KernelPerformanceLedger *ledger,
		const rts::performance::KernelPerformanceBatch &batch,
		rts::performance::KernelPerformanceStage stage)
		: m_ledger(ledger), m_interval()
	{
		if (m_ledger != nullptr && batch.valid())
			m_interval = m_ledger->beginInterval(batch, stage);
	}

	~SpatialPerformanceInterval() noexcept
	{
		end();
	}

	void end()
	{
		if (m_ledger != nullptr && m_interval.valid())
		{
			m_ledger->endInterval(m_interval);
			m_interval = rts::performance::KernelPerformanceInterval();
		}
	}

private:
	rts::performance::KernelPerformanceLedger *m_ledger;
	rts::performance::KernelPerformanceInterval m_interval;
};

class SpatialPerformanceBatchGuard
{
public:
	SpatialPerformanceBatchGuard(
		rts::performance::KernelPerformanceLedger *ledger,
		const rts::performance::KernelPerformanceBatch &batch,
		rts::performance::KernelPerformanceInterval *captureInterval,
		rts::performance::KernelPerformanceInterval *commitInterval,
		Bool *active)
		: m_ledger(ledger), m_batch(batch),
		  m_captureInterval(captureInterval), m_commitInterval(commitInterval),
		  m_active(active), m_armed(TRUE)
	{
	}

	~SpatialPerformanceBatchGuard() noexcept
	{
		if (!m_armed)
			return;
		endInterval(m_commitInterval);
		endInterval(m_captureInterval);
		if (m_ledger != nullptr && m_batch.valid())
			m_ledger->endBatch(m_batch,
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		if (m_active != nullptr)
			*m_active = FALSE;
	}

	void release()
	{
		m_armed = FALSE;
	}

private:
	void endInterval(rts::performance::KernelPerformanceInterval *interval)
	{
		if (m_ledger != nullptr && interval != nullptr && interval->valid())
		{
			m_ledger->endInterval(*interval);
			*interval = rts::performance::KernelPerformanceInterval();
		}
	}

	rts::performance::KernelPerformanceLedger *m_ledger;
	rts::performance::KernelPerformanceBatch m_batch;
	rts::performance::KernelPerformanceInterval *m_captureInterval;
	rts::performance::KernelPerformanceInterval *m_commitInterval;
	Bool *m_active;
	Bool m_armed;
};
}

class ImmutableSpatialQueryRuntime
{
public:
	ImmutableSpatialQueryRuntime()
		: m_storage(nullptr), m_storageCapacity(0), m_cachedObjects(nullptr),
		  m_objectScratch(nullptr),
		  m_cachedCells(nullptr), m_cachedMembers(nullptr), m_cachedRadii(nullptr),
		  m_cachedOffsets(nullptr), m_cachedMemberPrefix(nullptr),
		  m_cachedCellCount(0), m_cachedMemberCount(0),
		  m_cachedRadiusCount(0), m_cachedOffsetCount(0), m_arena(nullptr),
		  m_arenaBytes(0), m_captureFrame(0), m_objectCount(0),
		  m_results(nullptr), m_resultSpans(nullptr), m_outputCount(0),
		  m_firstIDs(nullptr), m_secondIDs(nullptr), m_commitObjects(nullptr),
		  m_commitCapacity(0), m_queryStorage(nullptr),
		  m_queryStorageCapacity(0), m_workStorage(nullptr),
		  m_workStorageCapacity(0), m_queries(nullptr), m_owners(nullptr),
		  m_ownerIndex(nullptr), m_queryCapacity(0), m_queryCount(0),
		  m_resultCapacity(0), m_captureManager(nullptr),
		  m_cachedManager(nullptr), m_cacheValid(FALSE),
		  m_collectionState(LIVE_SPATIAL_COLLECTION_IDLE), m_batchEpoch(1),
		  m_batchFailureStale(FALSE), m_failedMetricsClaimed(FALSE),
		  m_ready(FALSE)
	{
		m_generation.lifecycle = 1;
		m_generation.topology = 1;
		m_generation.facts = 1;
		m_cachedGeneration.lifecycle = 0;
		m_cachedGeneration.topology = 0;
		m_cachedGeneration.facts = 0;
		m_consumerDisabled[0] = FALSE;
		m_consumerDisabled[1] = FALSE;
		m_consumerMetricsClaimed[0] = FALSE;
		m_consumerMetricsClaimed[1] = FALSE;
		m_performanceLedger = &rts::performance::KernelPerformanceLedger::instance();
		m_referenceLedger = &rts::performance::KernelPerformanceReferenceLedger::instance();
		m_performanceBatch = rts::performance::KernelPerformanceBatch();
		m_performanceCaptureInterval =
			rts::performance::KernelPerformanceInterval();
		m_performanceCommitInterval =
			rts::performance::KernelPerformanceInterval();
		m_performanceOrdinal = 0;
		m_performanceCompletion.reset(m_batchEpoch);
		m_performanceBatchActive = FALSE;
	}

	~ImmutableSpatialQueryRuntime()
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		delete[] m_storage;
		delete[] m_commitObjects;
		delete[] m_queryStorage;
		delete[] m_workStorage;
	}

	void reset()
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		advance(m_generation.lifecycle);
		advance(m_generation.topology);
		advance(m_generation.facts);
		m_ready = FALSE;
		invalidateCache();
		clearCollection();
		m_consumerDisabled[0] = FALSE;
		m_consumerDisabled[1] = FALSE;
		rts::ResetImmutableSpatialRuntimeMetrics();
	}

	void invalidateLifecycle()
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		advance(m_generation.lifecycle);
		advance(m_generation.topology);
		advance(m_generation.facts);
		m_ready = FALSE;
		invalidateCache();
		clearCollection();
	}

	void invalidateTopology()
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		advance(m_generation.topology);
		m_ready = FALSE;
		clearCollection();
	}

	void invalidateFacts()
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		advance(m_generation.facts);
		m_ready = FALSE;
		clearCollection();
	}

	Bool capture(PartitionManager *manager, UnsignedInt frame)
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		m_ready = FALSE;
		clearCollection();
		if (manager == nullptr || manager->m_cells == nullptr ||
			manager->m_immutableSpatialCellMemberCounts == nullptr ||
			manager->m_cellCountX <= 0 || manager->m_cellCountY <= 0 ||
			manager->m_cellSize <= 0.0f)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}
		beginPerformanceBatch(frame);
		SpatialPerformanceBatchGuard performanceGuard(m_performanceLedger,
			m_performanceBatch, &m_performanceCaptureInterval,
			&m_performanceCommitInterval, &m_performanceBatchActive);
		beginPerformanceCapture();
		if (m_cacheValid && manager == m_cachedManager &&
			m_cachedGeneration.lifecycle == m_generation.lifecycle &&
			m_cachedGeneration.topology == m_generation.topology)
		{
			if (m_cachedGeneration.facts != m_generation.facts &&
				!refreshObjectFacts(manager))
			{
				rts::RecordImmutableSpatialArenaCapture(false);
				return FALSE;
			}
			m_captureFrame = frame;
			m_captureManager = manager;
			m_ready = TRUE;
			rts::RecordImmutableSpatialArenaCapture(true);
			performanceGuard.release();
			return TRUE;
		}
		invalidateCache();

		const UnsignedInt objectCount = manager->m_immutableSpatialObjectCount;
		const UnsignedInt memberCount = manager->m_immutableSpatialMemberCount;

#ifdef FASTER_GCO
		const UnsignedInt radiusCount = static_cast<UnsignedInt>(
			manager->m_radiusVec.size());
		const unsigned __int64 offsetWidth =
			static_cast<unsigned __int64>(manager->m_cellCountX) * 2 - 1;
		const unsigned __int64 offsetHeight =
			static_cast<unsigned __int64>(manager->m_cellCountY) * 2 - 1;
		if (offsetWidth != 0 && offsetHeight > UINT_MAX / offsetWidth)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}
		const UnsignedInt offsetCount = static_cast<UnsignedInt>(
			offsetWidth * offsetHeight);
#else
		rts::RecordImmutableSpatialArenaCapture(false);
		return FALSE;
#endif

		const UnsignedInt cellCount = static_cast<UnsignedInt>(
			manager->m_totalCellCount);
		const unsigned __int64 prefixWidth =
			static_cast<unsigned __int64>(manager->m_cellCountX) + 1;
		const unsigned __int64 prefixHeight =
			static_cast<unsigned __int64>(manager->m_cellCountY) + 1;
		if (prefixWidth != 0 && prefixHeight > UINT_MAX / prefixWidth)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}
		const UnsignedInt prefixCount = static_cast<UnsignedInt>(
			prefixWidth * prefixHeight);
		size_t arenaBytes = sizeof(rts::ImmutableSpatialArenaHeader);
		if (!addArrayBytes(arenaBytes, objectCount,
			sizeof(rts::ImmutableSpatialObjectRecord)) ||
			!addArrayBytes(arenaBytes, cellCount,
				sizeof(rts::ImmutableSpatialCellRecord)) ||
			!addArrayBytes(arenaBytes, memberCount,
				sizeof(rts::ImmutableSpatialMemberRecord)) ||
			!addArrayBytes(arenaBytes, radiusCount,
				sizeof(rts::ImmutableSpatialRadiusRecord)) ||
			!addArrayBytes(arenaBytes, offsetCount,
				sizeof(rts::ImmutableSpatialOffsetRecord)) ||
			arenaBytes > UINT_MAX)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}

		size_t totalBytes = 0;
		if (!addArrayBytes(totalBytes, objectCount,
			sizeof(rts::ImmutableSpatialObjectRecord)) ||
			!addArrayBytes(totalBytes, objectCount,
				sizeof(rts::ImmutableSpatialObjectRecord)) ||
			!addArrayBytes(totalBytes, cellCount,
				sizeof(rts::ImmutableSpatialCellRecord)) ||
			!addArrayBytes(totalBytes, memberCount,
				sizeof(rts::ImmutableSpatialMemberRecord)) ||
			!addArrayBytes(totalBytes, radiusCount,
				sizeof(rts::ImmutableSpatialRadiusRecord)) ||
			!addArrayBytes(totalBytes, offsetCount,
				sizeof(rts::ImmutableSpatialOffsetRecord)) ||
			!addArrayBytes(totalBytes, prefixCount,
				sizeof(rts::ImmutableSpatialUInt32)) ||
			!addArrayBytes(totalBytes, static_cast<UnsignedInt>(arenaBytes), 1) ||
			!addArrayBytes(totalBytes, objectCount, sizeof(ObjectID)) ||
			!addArrayBytes(totalBytes, objectCount, sizeof(ObjectID)) ||
			totalBytes > UINT_MAX || !ensureCommitStorage(objectCount) ||
			!ensureStorage(
				static_cast<UnsignedInt>(totalBytes)))
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}

		unsigned char *cursor = m_storage;
		m_cachedObjects = take<
			rts::ImmutableSpatialObjectRecord>(cursor, objectCount);
		m_objectScratch = take<rts::ImmutableSpatialObjectRecord>(cursor,
			objectCount);
		m_cachedCells = take<
			rts::ImmutableSpatialCellRecord>(cursor, cellCount);
		m_cachedMembers = take<
			rts::ImmutableSpatialMemberRecord>(cursor, memberCount);
		m_cachedRadii = take<
			rts::ImmutableSpatialRadiusRecord>(cursor, radiusCount);
		m_cachedOffsets = take<
			rts::ImmutableSpatialOffsetRecord>(cursor, offsetCount);
		m_cachedMemberPrefix = take<rts::ImmutableSpatialUInt32>(cursor,
			prefixCount);
		m_arena = cursor;
		cursor += arenaBytes;
		m_firstIDs = take<ObjectID>(cursor, objectCount);
		m_secondIDs = take<ObjectID>(cursor, objectCount);

		UnsignedInt objectIndex = 0;
		for (PartitionData *module = manager->m_moduleList; module != nullptr;
			module = module->getNext())
		{
			Object *object = module->getObject();
			if (object == nullptr)
				continue;
			rts::ImmutableSpatialObjectRecord &record =
				m_cachedObjects[objectIndex++];
			record.objectID = static_cast<UnsignedInt>(object->getID());
			record.generation = m_generation;
			record.admissionMask = 0;
			record.buildCost = 0;
			const Coord3D *position = object->getPosition();
			const GeometryInfo &geometry = object->getGeometryInfo();
			record.positionX = position->x;
			record.positionY = position->y;
			record.positionZ = position->z;
			record.boundingCircleRadius = geometry.getBoundingCircleRadius();
			record.boundingSphereRadius = geometry.getBoundingSphereRadius();
			record.zCenterOffset = geometry.getZDeltaToCenterPosition();
		}
		if (objectIndex != objectCount)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}
		if (objectCount != 0)
			std::sort(m_cachedObjects, m_cachedObjects + objectCount,
				objectRecordLess);
		for (UnsignedInt duplicateIndex = 1; duplicateIndex != objectCount;
			++duplicateIndex)
		{
			if (m_cachedObjects[duplicateIndex - 1].objectID ==
				m_cachedObjects[duplicateIndex].objectID)
			{
				rts::RecordImmutableSpatialArenaCapture(false);
				return FALSE;
			}
		}

		UnsignedInt memberIndex = 0;
		for (UnsignedInt cellIndex = 0; cellIndex != cellCount; ++cellIndex)
		{
			m_cachedCells[cellIndex].memberBegin = memberIndex;
			for (CellAndObjectIntersection *coi =
				manager->m_cells[cellIndex].getFirstCoiInCell(); coi != nullptr;
				coi = coi->getNextCoi())
			{
				if (memberIndex == memberCount)
				{
					rts::RecordImmutableSpatialArenaCapture(false);
					return FALSE;
				}
				rts::ImmutableSpatialMemberRecord &member =
					m_cachedMembers[memberIndex++];
				member.objectIndex = rts::IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX;
				PartitionData *module = coi->getModule();
				Object *object = module != nullptr ? module->getObject() : nullptr;
				if (object != nullptr)
				{
					member.objectIndex = findObjectIndex(m_cachedObjects, objectCount,
						static_cast<UnsignedInt>(object->getID()));
					if (member.objectIndex ==
						rts::IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX)
					{
						rts::RecordImmutableSpatialArenaCapture(false);
						return FALSE;
					}
				}
			}
			m_cachedCells[cellIndex].memberCount = memberIndex -
				m_cachedCells[cellIndex].memberBegin;
			if (m_cachedCells[cellIndex].memberCount !=
				manager->m_immutableSpatialCellMemberCounts[cellIndex])
			{
				rts::RecordImmutableSpatialArenaCapture(false);
				return FALSE;
			}
		}
		if (memberIndex != memberCount)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}

		// Cache a summed-area table for deterministic O(1) admission estimates.
		// It preserves dense-cell skew without walking live COI lists before the
		// collection has passed policy admission.
		memset(m_cachedMemberPrefix, 0,
			static_cast<size_t>(prefixCount) *
				sizeof(rts::ImmutableSpatialUInt32));
		const UnsignedInt prefixStride = static_cast<UnsignedInt>(prefixWidth);
		for (UnsignedInt cellY = 0;
			cellY != static_cast<UnsignedInt>(manager->m_cellCountY); ++cellY)
		{
			unsigned __int64 rowMembers = 0;
			for (UnsignedInt cellX = 0;
				cellX != static_cast<UnsignedInt>(manager->m_cellCountX); ++cellX)
			{
				const UnsignedInt cellIndex = cellY *
					static_cast<UnsignedInt>(manager->m_cellCountX) + cellX;
				rowMembers += m_cachedCells[cellIndex].memberCount;
				const unsigned __int64 prefixMembers = rowMembers +
					m_cachedMemberPrefix[cellY * prefixStride + cellX + 1];
				if (prefixMembers > UINT_MAX)
				{
					rts::RecordImmutableSpatialArenaCapture(false);
					return FALSE;
				}
				m_cachedMemberPrefix[(cellY + 1) * prefixStride + cellX + 1] =
					static_cast<UnsignedInt>(prefixMembers);
			}
		}

		UnsignedInt offsetIndex = 0;
#ifdef FASTER_GCO
		UnsignedInt radiusIndex = 0;
		for (PartitionManager::RadiusVec::const_iterator radius =
			manager->m_radiusVec.begin(); radius != manager->m_radiusVec.end();
			++radius, ++radiusIndex)
		{
			m_cachedRadii[radiusIndex].offsetBegin = offsetIndex;
			for (PartitionManager::OffsetVec::const_iterator offset = radius->begin();
				offset != radius->end(); ++offset)
			{
				if (offsetIndex == offsetCount)
				{
					rts::RecordImmutableSpatialArenaCapture(false);
					return FALSE;
				}
				m_cachedOffsets[offsetIndex].x = offset->x;
				m_cachedOffsets[offsetIndex].y = offset->y;
				++offsetIndex;
			}
			m_cachedRadii[radiusIndex].offsetCount =
				offsetIndex - m_cachedRadii[radiusIndex].offsetBegin;
		}
#endif
		if (offsetIndex != offsetCount)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}

		rts::ImmutableSpatialArenaInput input;
		input.generation = m_generation;
		input.gridWidth = static_cast<UnsignedInt>(manager->m_cellCountX);
		input.gridHeight = static_cast<UnsignedInt>(manager->m_cellCountY);
		input.cellSize = manager->m_cellSize;
		input.objects = m_cachedObjects;
		input.objectCount = objectCount;
		input.cells = m_cachedCells;
		input.cellCount = cellCount;
		input.members = m_cachedMembers;
		input.memberCount = memberCount;
		input.radii = m_cachedRadii;
		input.radiusCount = radiusCount;
		input.offsets = m_cachedOffsets;
		input.offsetCount = offsetCount;
		rts::ImmutableSpatialUInt32 measuredBytes = 0;
		rts::ImmutableSpatialUInt32 builtBytes = 0;
		if (rts::MeasureImmutableSpatialArena(input, &measuredBytes) !=
			rts::IMMUTABLE_SPATIAL_SUCCESS || measuredBytes != arenaBytes ||
			rts::BuildImmutableSpatialArena(input, m_arena,
				static_cast<UnsignedInt>(arenaBytes), &builtBytes) !=
				rts::IMMUTABLE_SPATIAL_SUCCESS || builtBytes != arenaBytes)
		{
			rts::RecordImmutableSpatialArenaCapture(false);
			return FALSE;
		}

		m_arenaBytes = builtBytes;
		m_captureFrame = frame;
		m_objectCount = objectCount;
		m_captureManager = manager;
		m_cachedManager = manager;
		m_cachedGeneration = m_generation;
		m_cachedCellCount = cellCount;
		m_cachedMemberCount = memberCount;
		m_cachedRadiusCount = radiusCount;
		m_cachedOffsetCount = offsetCount;
		m_cacheValid = TRUE;
		m_ready = TRUE;
		rts::RecordImmutableSpatialArenaCapture(true);
		performanceGuard.release();
		return TRUE;
	}

	Bool beginCollection(UnsignedInt maximumQueryCount)
	{
		clearCollection();
		if (!m_ready || maximumQueryCount == 0 ||
			maximumQueryCount > LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES ||
			!ensureQueryStorage(maximumQueryCount))
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return FALSE;
		}

		unsigned char *cursor = m_queryStorage;
		m_owners = take<LiveImmutableSpatialPreparedOwner>(cursor,
			maximumQueryCount);
		m_queries = take<rts::ImmutableSpatialQuery>(cursor, maximumQueryCount);
		m_ownerIndex = take<LiveImmutableSpatialOwnerIndexEntry>(cursor,
			maximumQueryCount);
		m_counts = take<rts::ImmutableSpatialUInt32>(cursor, maximumQueryCount);
		m_states = take<rts::ImmutableSpatialUInt32>(cursor, maximumQueryCount);
		m_spanScratch = take<rts::ImmutableSpatialResultSpan>(cursor,
			maximumQueryCount);
		m_resultSpans = take<rts::ImmutableSpatialResultSpan>(cursor,
			maximumQueryCount);
		m_queryCapacity = maximumQueryCount;
		m_collectionState = LIVE_SPATIAL_COLLECTION_COLLECTING;
		return TRUE;
	}

	void endCollection()
	{
		if (m_performanceBatchActive || m_referenceBatch.valid())
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
	}

	Bool canQueueConsumer(LiveImmutableSpatialConsumer consumer) const
	{
		const Int ordinal = consumerIndex(consumer);
		return ordinal >= 0 && !m_consumerDisabled[ordinal];
	}

	Bool measureQueryCost(PartitionManager *manager, const Coord3D *position,
		Real maximumDistance, UnsignedInt *cellVisits,
		UnsignedInt *memberVisits) const
	{
		if (manager == nullptr || position == nullptr || cellVisits == nullptr ||
			memberVisits == nullptr || manager->m_cells == nullptr ||
			manager->m_cellCountX <= 0 || manager->m_cellCountY <= 0)
			return FALSE;
		*cellVisits = 0;
		*memberVisits = 0;
#ifdef FASTER_GCO
		Int maximumRadius = manager->worldToCellDist(maximumDistance);
		if (maximumRadius < 0)
			maximumRadius = 0;
		if (maximumRadius > manager->m_maxGcoRadius)
			maximumRadius = manager->m_maxGcoRadius;
		unsigned __int64 cells = 0;
		for (Int radius = 0; radius <= maximumRadius; ++radius)
		{
			const PartitionManager::OffsetVec &offsets = manager->m_radiusVec[radius];
			cells += offsets.size();
			if (cells > UINT_MAX)
				return FALSE;
		}
		*cellVisits = static_cast<UnsignedInt>(cells);
		// A valid persistent topology cache gives the exact number of cell
		// memberships in the query's bounding square. This is a conservative
		// upper bound for the radius offsets and retains actual occupancy skew.
		if (m_cacheValid && manager == m_cachedManager &&
			m_cachedMemberPrefix != nullptr &&
			m_cachedGeneration.lifecycle == m_generation.lifecycle &&
			m_cachedGeneration.topology == m_generation.topology)
		{
			Int centerX = 0;
			Int centerY = 0;
			manager->worldToCell(position->x, position->y, &centerX, &centerY);
			Int minimumX = centerX - maximumRadius;
			Int minimumY = centerY - maximumRadius;
			Int maximumX = centerX + maximumRadius;
			Int maximumY = centerY + maximumRadius;
			if (minimumX < 0)
				minimumX = 0;
			if (minimumY < 0)
				minimumY = 0;
			if (maximumX >= manager->m_cellCountX)
				maximumX = manager->m_cellCountX - 1;
			if (maximumY >= manager->m_cellCountY)
				maximumY = manager->m_cellCountY - 1;
			if (minimumX <= maximumX && minimumY <= maximumY)
			{
				const UnsignedInt stride = static_cast<UnsignedInt>(
					manager->m_cellCountX + 1);
				const UnsignedInt left = static_cast<UnsignedInt>(minimumX);
				const UnsignedInt top = static_cast<UnsignedInt>(minimumY);
				const UnsignedInt right = static_cast<UnsignedInt>(maximumX + 1);
				const UnsignedInt bottom = static_cast<UnsignedInt>(maximumY + 1);
				const unsigned __int64 bottomRight =
					m_cachedMemberPrefix[bottom * stride + right];
				const unsigned __int64 topRight =
					m_cachedMemberPrefix[top * stride + right];
				const unsigned __int64 bottomLeft =
					m_cachedMemberPrefix[bottom * stride + left];
				const unsigned __int64 topLeft =
					m_cachedMemberPrefix[top * stride + left];
				const unsigned __int64 members = bottomRight + topLeft -
					topRight - bottomLeft;
				if (members > UINT_MAX)
					return FALSE;
				*memberVisits = static_cast<UnsignedInt>(members);
			}
		}
		else
		{
			// Cold admission reads only the owner-maintained per-cell counters.
			// This is an exact local membership cost without capturing/sorting the
			// arena or walking any live COI list. Missing counter storage fails the
			// collection closed before capture.
			if (manager->m_immutableSpatialCellMemberCounts == nullptr)
				return FALSE;
			Int centerX = 0;
			Int centerY = 0;
			manager->worldToCell(position->x, position->y, &centerX, &centerY);
			unsigned __int64 members = 0;
			for (Int radius = 0; radius <= maximumRadius; ++radius)
			{
				const PartitionManager::OffsetVec &offsets =
					manager->m_radiusVec[radius];
				for (PartitionManager::OffsetVec::const_iterator offset =
					offsets.begin(); offset != offsets.end(); ++offset)
				{
					const __int64 cellX = static_cast<__int64>(centerX) +
						offset->x;
					const __int64 cellY = static_cast<__int64>(centerY) +
						offset->y;
					if (cellX < 0 || cellY < 0 ||
						cellX >= manager->m_cellCountX ||
						cellY >= manager->m_cellCountY)
						continue;
					const UnsignedInt cellIndex = static_cast<UnsignedInt>(
						cellY * manager->m_cellCountX + cellX);
					members += manager->
						m_immutableSpatialCellMemberCounts[cellIndex];
					if (members > UINT_MAX)
						return FALSE;
				}
			}
			*memberVisits = static_cast<UnsignedInt>(members);
		}
		return TRUE;
#else
		return FALSE;
#endif
	}

	LiveImmutableSpatialCollectionPreflightResult preflightCost(
		PartitionManager *manager, UnsignedInt queryCount,
		UnsignedInt queryCellVisits, UnsignedInt queryMemberVisits,
		UnsignedInt maximumRangeCost, UnsignedInt ownerScanCount,
		UnsignedInt ownerSortComparisons,
		UnsignedInt ownerLookupComparisons) const
	{
		if (manager == nullptr || manager->m_cellCountX <= 0 ||
			manager->m_cellCountY <= 0 || manager->m_totalCellCount <= 0)
			return LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED;
		if (manager->m_immutableSpatialObjectCount != 0 &&
			queryCount > LIVE_IMMUTABLE_SPATIAL_MAXIMUM_RESULT_SLOTS /
				manager->m_immutableSpatialObjectCount)
			return LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED;
		const unsigned __int64 offsetWidth =
			static_cast<unsigned __int64>(manager->m_cellCountX) * 2 - 1;
		const unsigned __int64 offsetHeight =
			static_cast<unsigned __int64>(manager->m_cellCountY) * 2 - 1;
		if (offsetWidth != 0 && offsetHeight > UINT_MAX / offsetWidth)
			return LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED;

		rts::ImmutableSpatialAdmissionCost cost;
		cost.queryCount = queryCount;
		cost.workerCount = rts::JobSystem::instance().workerCount();
		cost.queryCellVisits = queryCellVisits;
		cost.queryMemberVisits = queryMemberVisits;
		cost.objectCount = manager->m_immutableSpatialObjectCount;
		cost.cellCount = static_cast<UnsignedInt>(manager->m_totalCellCount);
		cost.memberCount = manager->m_immutableSpatialMemberCount;
		cost.radiusOffsetCount = offsetWidth * offsetHeight;
		cost.maximumRangeCost = maximumRangeCost;
		cost.ownerScanCount = ownerScanCount;
		cost.ownerSortComparisons = ownerSortComparisons;
		cost.ownerLookupComparisons = ownerLookupComparisons;
		cost.rebuildTopology = !m_cacheValid || manager != m_cachedManager ||
			m_cachedGeneration.lifecycle != m_generation.lifecycle ||
			m_cachedGeneration.topology != m_generation.topology;
		cost.refreshFacts = !cost.rebuildTopology &&
			m_cachedGeneration.facts != m_generation.facts;
		const rts::ImmutableSpatialAdmissionResult result =
			rts::EvaluateImmutableSpatialQueryAdmission(cost);
		if (result == rts::IMMUTABLE_SPATIAL_ADMISSION_ELIGIBLE)
			return LIVE_IMMUTABLE_SPATIAL_COLLECTION_ELIGIBLE;
		return result == rts::IMMUTABLE_SPATIAL_ADMISSION_POLICY_INELIGIBLE ?
			LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK :
			LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED;
	}

	void publishNoCaptureState(
		LiveImmutableSpatialCollectionPreflightResult result,
		PartitionManager *manager, UnsignedInt frame)
	{
		finishPerformanceBatch(
			rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		m_ready = FALSE;
		clearCollection();
		m_captureManager = manager;
		m_captureFrame = frame;
		m_collectionState = result ==
			LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK ?
			LIVE_SPATIAL_COLLECTION_POLICY_FALLBACK :
			LIVE_SPATIAL_COLLECTION_FAILED;
	}

	Bool queue(UpdateModule *owner, PartitionManager *manager,
		const Coord3D *position, Real maximumDistance, UnsignedInt frame,
		LiveImmutableSpatialConsumer consumer)
	{
		const Int consumerOrdinal = consumerIndex(consumer);
		if (m_collectionState != LIVE_SPATIAL_COLLECTION_COLLECTING ||
			owner == nullptr || manager == nullptr || position == nullptr ||
			consumerOrdinal < 0 || manager != m_captureManager ||
			frame != m_captureFrame || m_queryCount >= m_queryCapacity)
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return FALSE;
		}
		Object *object = const_cast<Object *>(owner->friend_getObject());
		if (object == nullptr || object->getID() == INVALID_ID ||
			TheGameLogic == nullptr || TheGameLogic->findObjectByID(object->getID()) !=
				object)
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return FALSE;
		}

		const UnsignedInt batchIndex = m_queryCount;
		rts::ImmutableSpatialQuery &spatialQuery = m_queries[batchIndex];
		memset(&spatialQuery, 0, sizeof(spatialQuery));
		spatialQuery.expectedArenaGeneration = m_generation;
		// Both legacy consumers use the Coord3D overload. Its traversal includes
		// the caller, so self remains an ordinary candidate.
		spatialQuery.selfObjectIndex =
			rts::IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX;
		manager->worldToCell(position->x, position->y,
			&spatialQuery.centerCellX, &spatialQuery.centerCellY);
		Int maximumRadius = manager->worldToCellDist(maximumDistance);
		if (maximumRadius < 0)
			maximumRadius = 0;
		if (maximumRadius > manager->m_maxGcoRadius)
			maximumRadius = manager->m_maxGcoRadius;
		spatialQuery.maximumRadius = static_cast<UnsignedInt>(maximumRadius);
		spatialQuery.positionX = position->x;
		spatialQuery.positionY = position->y;
		spatialQuery.positionZ = position->z;
		spatialQuery.maximumDistance = maximumDistance;
		spatialQuery.distanceType = rts::IMMUTABLE_SPATIAL_FROM_CENTER_2D;
		spatialQuery.iteratorOrder = rts::IMMUTABLE_SPATIAL_ITER_FASTEST;

		LiveImmutableSpatialPreparedOwner &prepared = m_owners[batchIndex];
		prepared.owner = owner;
		prepared.objectID = object->getID();
		prepared.consumer = consumer;
		prepared.batchIndex = batchIndex;
		prepared.wakePriority = owner->friend_getPriority();
		++m_queryCount;
		if (m_performanceBatchActive)
			++m_performanceCompletion.expectedConsumers;
		return TRUE;
	}

	void executeCollection()
	{
		if (m_collectionState != LIVE_SPATIAL_COLLECTION_COLLECTING)
			return;
		if (m_queryCount < 2)
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_POLICY_FALLBACK;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}

		rts::JobSystem &jobs = rts::JobSystem::instance();
		if (!rts::ShouldPrepareLiveSimulationKernelOffThread(
				rts::MULTIPLAYER_SIMULATION_KERNEL_SPATIAL) ||
			!jobs.isRunning() ||
			!rts::ShouldDispatchImmutableSpatialQueryCollection(m_queryCount,
				jobs.workerCount()) ||
			jobs.isWorkerThread() ||
			!jobs.isCurrentThread(rts::JOB_OWNER_GAME))
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_POLICY_FALLBACK;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}

		if (m_objectCount != 0 &&
			m_queryCount > LIVE_IMMUTABLE_SPATIAL_MAXIMUM_RESULT_SLOTS /
				m_objectCount)
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}
		m_resultCapacity = m_queryCount * m_objectCount;
		const UnsignedInt rangeCount = std::min<UnsignedInt>(
			jobs.workerCount(), m_queryCount);
		if (!ensureWorkStorage(rangeCount, m_resultCapacity))
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}

		unsigned char *cursor = m_workStorage;
		m_visitStamps = take<rts::ImmutableSpatialUInt32>(cursor,
			rangeCount * m_objectCount);
		m_resultScratch = take<rts::ImmutableSpatialResult>(cursor,
			m_resultCapacity);
		m_sortScratch = take<rts::ImmutableSpatialResult>(cursor,
			m_resultCapacity);
		m_results = take<rts::ImmutableSpatialResult>(cursor, m_resultCapacity);
		for (UnsignedInt ownerIndex = 0; ownerIndex != m_queryCount; ++ownerIndex)
		{
			m_ownerIndex[ownerIndex].objectID = static_cast<UnsignedInt>(
				m_owners[ownerIndex].objectID);
			m_ownerIndex[ownerIndex].batchIndex = ownerIndex;
		}
		std::sort(m_ownerIndex, m_ownerIndex + m_queryCount, ownerIndexLess);

		rts::ImmutableSpatialBatchScratch scratch;
		scratch.counts = m_counts;
		scratch.countCapacity = m_queryCount;
		scratch.states = m_states;
		scratch.stateCapacity = m_queryCount;
		scratch.visitStamps = m_visitStamps;
		scratch.visitStampCapacity = rangeCount * m_objectCount;
		scratch.spanScratch = m_spanScratch;
		scratch.spanScratchCapacity = m_queryCount;
		scratch.resultScratch = m_resultScratch;
		scratch.resultScratchCapacity = m_resultCapacity;
		scratch.sortScratch = m_sortScratch;
		scratch.sortScratchCapacity = m_resultCapacity;
		m_outputCount = 0;
		m_lastJobMetrics = rts::ImmutableSpatialJobSystemMetrics();
		rts::ImmutableSpatialExecutionMetrics executionMetrics;
		rts::ImmutableSpatialJobSystemOptions options;
		options.performanceLedger = m_performanceLedger;
		options.performanceBatch = m_performanceBatch;
		rts::ImmutableSpatialQueryOwnerIdentity referenceOwners[
			rts::ImmutableSpatialCollectionCompletion::MAXIMUM_QUERIES];
		if (m_referenceLedger->mode() != rts::performance::KERNEL_REFERENCE_DISABLED)
		{
			for (UnsignedInt index = 0; index != m_queryCount; ++index)
			{
				referenceOwners[index].objectID = static_cast<UnsignedInt>(m_owners[index].objectID);
				referenceOwners[index].consumer = mapConsumer(m_owners[index].consumer);
				referenceOwners[index].wakePriority = m_owners[index].wakePriority;
			}
			options.referenceLedger = m_referenceLedger;
			options.referenceBatch = &m_referenceBatch;
			options.queryOwners = referenceOwners;
			options.queryOwnerCount = m_queryCount;
		}
		rts::ImmutableSpatialStatus kernelStatus =
			rts::IMMUTABLE_SPATIAL_INVALID_ARGUMENT;
		// The capture interval covers arena construction, cached fact refresh,
		// query preparation, and owner sorting. The shared executor owns only
		// schedule/wait intervals after this point.
		endPerformanceCapture();
		const rts::ImmutableSpatialJobSystemResult result =
			rts::ExecuteImmutableSpatialQueryBatchOnJobSystem(m_arena,
				m_arenaBytes, m_queries, m_queryCount, resolveArenaGeneration,
				resolveObjectGeneration, this, scratch, m_results, m_resultCapacity,
				m_resultSpans, m_queryCount, &m_outputCount, options,
				&m_lastJobMetrics, &executionMetrics, &kernelStatus);
		if (result == rts::IMMUTABLE_SPATIAL_JOB_SYSTEM_INELIGIBLE)
		{
			m_collectionState = LIVE_SPATIAL_COLLECTION_POLICY_FALLBACK;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}
		if (result != rts::IMMUTABLE_SPATIAL_JOB_SYSTEM_SUCCESS)
		{
			m_batchFailureStale =
				kernelStatus == rts::IMMUTABLE_SPATIAL_STALE_GENERATION ||
				kernelStatus == rts::IMMUTABLE_SPATIAL_GENERATION_MISMATCH;
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}
		Bool validSpans = FALSE;
		{
			SpatialPerformanceInterval validationInterval(m_performanceLedger,
				m_performanceBatch, rts::performance::KERNEL_PERFORMANCE_VALIDATE);
			validSpans = rts::ValidateImmutableSpatialResultSpans(m_resultSpans,
				m_queryCount, m_outputCount);
		}
		if (!validSpans)
		{
			m_batchFailureStale = FALSE;
			m_collectionState = LIVE_SPATIAL_COLLECTION_FAILED;
			finishPerformanceBatch(
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
			return;
		}

		computeConsumerJobMetrics(executionMetrics.rangeCount);
		rts::RecordImmutableSpatialSuccessfulCollection(m_queryCount,
			executionMetrics.rangeCount, m_lastJobMetrics);
		m_collectionState = LIVE_SPATIAL_COLLECTION_READY;
	}

	LiveImmutableSpatialQueryResult query(UpdateModule *owner,
		PartitionManager *manager, const Coord3D *position, Real maximumDistance,
		UnsignedInt frame, LiveImmutableSpatialConsumer consumer,
		LiveImmutableSpatialResultView *view)
	{
		if (view != nullptr)
			*view = LiveImmutableSpatialResultView();
		const Int consumerOrdinal = consumerIndex(consumer);
		if (consumerOrdinal < 0)
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		const rts::ImmutableSpatialConsumer runtimeConsumer = mapConsumer(consumer);
		rts::RecordImmutableSpatialEligibleQuery(runtimeConsumer);
		if (owner == nullptr || manager == nullptr || position == nullptr ||
			view == nullptr || m_consumerDisabled[consumerOrdinal])
		{
			recordUnexpected(consumer, FALSE, TRUE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}

		rts::JobSystem &jobs = rts::JobSystem::instance();
		if (!rts::ShouldPrepareLiveSimulationKernelOffThread(
				rts::MULTIPLAYER_SIMULATION_KERNEL_SPATIAL) ||
			!jobs.isRunning() || jobs.workerCount() <= 1 ||
			jobs.isWorkerThread() ||
			!jobs.isCurrentThread(rts::JOB_OWNER_GAME) ||
			m_collectionState == LIVE_SPATIAL_COLLECTION_POLICY_FALLBACK)
		{
			rts::RecordImmutableSpatialExpectedFallback(runtimeConsumer);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_POLICY_FALLBACK;
		}
		if (m_collectionState == LIVE_SPATIAL_COLLECTION_FAILED)
		{
			recordUnexpected(consumer, m_batchFailureStale,
				m_batchFailureStale == FALSE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}
		if (!m_ready || m_collectionState != LIVE_SPATIAL_COLLECTION_READY ||
			m_captureFrame != frame || manager != m_captureManager ||
			!generationEqual(m_generation, arenaHeader()->generation))
		{
			recordUnexpected(consumer, TRUE, FALSE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}

		const Object *object = owner->friend_getObject();
		if (object == nullptr)
		{
			recordUnexpected(consumer, TRUE, FALSE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}
		UnsignedInt batchIndex = 0;
		if (!findOwner(owner, object->getID(), consumer, &batchIndex))
		{
			recordUnexpected(consumer, TRUE, FALSE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}
		const LiveImmutableSpatialPreparedOwner &prepared = m_owners[batchIndex];
		const rts::ImmutableSpatialQuery &spatialQuery = m_queries[batchIndex];
		Object *resolved = TheGameLogic != nullptr ?
			TheGameLogic->findObjectByID(prepared.objectID) : nullptr;
		if (resolved != object || prepared.owner != owner ||
			prepared.consumer != consumer ||
			owner->friend_getIndexInLogic() != 0 ||
			owner->friend_getPriority() != prepared.wakePriority ||
			owner->friend_getNextCallFrame() != frame ||
			owner->friend_getNextCallPhase() != PHASE_NORMAL ||
			TheGameLogic->getFrame() != frame ||
			!sameFloat(spatialQuery.positionX, position->x) ||
			!sameFloat(spatialQuery.positionY, position->y) ||
			!sameFloat(spatialQuery.positionZ, position->z) ||
			!sameFloat(spatialQuery.maximumDistance, maximumDistance))
		{
			recordUnexpected(consumer, TRUE, FALSE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}

		const rts::ImmutableSpatialResultSpan &span = m_resultSpans[batchIndex];
		if (span.begin > m_outputCount || span.count > m_outputCount - span.begin)
		{
			recordUnexpected(consumer, FALSE, TRUE);
			return LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK;
		}
		view->results = span.count != 0 ? m_results + span.begin : nullptr;
		view->count = span.count;
		view->queryOrdinal = batchIndex;
		view->batchEpoch = m_batchEpoch;
		return LIVE_IMMUTABLE_SPATIAL_QUERY_SUCCESS;
	}

	Object *resolve(const rts::ImmutableSpatialResult &result)
	{
		if (!m_ready || !generationEqual(result.generation, m_generation) ||
			TheGameLogic == nullptr)
			return nullptr;
		Object *object = TheGameLogic->findObjectByID(
			static_cast<ObjectID>(result.objectID));
		if (object == nullptr || object->getID() !=
			static_cast<ObjectID>(result.objectID) ||
			object->friend_getPartitionData() == nullptr)
			return nullptr;
		return object;
	}

	Object *resolveID(ObjectID objectID)
	{
		if (!m_ready || TheGameLogic == nullptr ||
			!generationEqual(arenaHeader()->generation, m_generation))
			return nullptr;
		Object *object = TheGameLogic->findObjectByID(objectID);
		return object != nullptr && object->getID() == objectID &&
			object->friend_getPartitionData() != nullptr ? object : nullptr;
	}

	Bool validate(const LiveImmutableSpatialResultView &view)
	{
		SpatialPerformanceInterval validationInterval(m_performanceLedger,
			m_performanceBatch, rts::performance::KERNEL_PERFORMANCE_VALIDATE);
		if (!m_ready || m_collectionState != LIVE_SPATIAL_COLLECTION_READY ||
			view.batchEpoch != m_batchEpoch ||
			view.queryOrdinal >= m_queryCount || view.count > m_objectCount ||
			(view.count != 0 && view.results == nullptr) ||
			!generationEqual(arenaHeader()->generation, m_generation))
			return FALSE;
		const rts::ImmutableSpatialResultSpan &span =
			m_resultSpans[view.queryOrdinal];
		if (span.begin > m_outputCount || span.count > m_outputCount - span.begin ||
			view.count != span.count ||
			(view.count != 0 && view.results != m_results + span.begin))
			return FALSE;
		for (UnsignedInt index = 0; index != view.count; ++index)
		{
			if (resolve(view.results[index]) == nullptr)
				return FALSE;
		}
		return TRUE;
	}

	Bool buffers(ObjectID **first, ObjectID **second, UnsignedInt *capacity)
	{
		if (!m_ready || first == nullptr || second == nullptr || capacity == nullptr)
			return FALSE;
		*first = m_firstIDs;
		*second = m_secondIDs;
		*capacity = m_objectCount;
		return TRUE;
	}

	Bool commitBuffer(Object ***objects, UnsignedInt *capacity)
	{
		if (!m_ready || objects == nullptr || capacity == nullptr)
			return FALSE;
		*objects = m_commitObjects;
		*capacity = m_objectCount;
		return TRUE;
	}

	rts::ImmutableSpatialConsumerCompletionToken captureCompletion(
		UpdateModule *owner, LiveImmutableSpatialConsumer consumer)
	{
		rts::ImmutableSpatialConsumerCompletionToken token;
		if (!m_performanceBatchActive || owner == nullptr || consumerIndex(consumer) < 0 ||
			m_collectionState != LIVE_SPATIAL_COLLECTION_READY)
			return token;
		const Object *object = owner->friend_getObject();
		UnsignedInt ordinal = 0;
		if (object != nullptr && findOwner(owner, object->getID(), consumer, &ordinal))
		{
			token.batchEpoch = m_batchEpoch;
			token.queryOrdinal = ordinal;
		}
		return token;
	}

	Bool pendingCompletion(LiveImmutableSpatialConsumer consumer,
		const rts::ImmutableSpatialConsumerCompletionToken &token) const
	{
		return m_performanceBatchActive && m_performanceCompletion.pending(token) &&
			token.queryOrdinal < m_queryCount && m_owners[token.queryOrdinal].consumer == consumer;
	}

	void beginCommit(LiveImmutableSpatialConsumer consumer,
		const rts::ImmutableSpatialConsumerCompletionToken &token)
	{
		if (m_performanceCommitInterval.valid() ||
			!pendingCompletion(consumer, token))
			return;
		m_performanceCommitInterval = m_performanceLedger->beginInterval(
			m_performanceBatch, rts::performance::KERNEL_PERFORMANCE_COMMIT);
		m_performanceCommitOwner = token;
	}

	void endCommit(LiveImmutableSpatialConsumer consumer,
		const rts::ImmutableSpatialConsumerCompletionToken &token)
	{
		if (pendingCompletion(consumer, token) &&
			token.batchEpoch == m_performanceCommitOwner.batchEpoch &&
			token.queryOrdinal == m_performanceCommitOwner.queryOrdinal)
			endPerformanceCommit();
	}

	void completeConsumer(LiveImmutableSpatialConsumer consumer,
		const rts::ImmutableSpatialConsumerCompletionToken &token, Bool committed)
	{
		if (!pendingCompletion(consumer, token))
			return;
		if (!m_performanceCompletion.complete(mapConsumer(consumer), token, committed != FALSE))
			return;
		if (m_performanceCompletion.finished())
		{
			finishPerformanceBatch(m_performanceCompletion.allConsumersCommitted ?
				rts::performance::KERNEL_PERFORMANCE_COMMITTED :
				rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		}
	}

	void recordAuthoritative(LiveImmutableSpatialConsumer consumer,
		UnsignedInt candidateCount)
	{
		const rts::ImmutableSpatialJobSystemMetrics metrics = claimMetrics(consumer);
		rts::RecordImmutableSpatialAuthoritativeQuery(mapConsumer(consumer),
			candidateCount, metrics);
	}

	void recordShadow(LiveImmutableSpatialConsumer consumer, Bool matched)
	{
		const rts::ImmutableSpatialJobSystemMetrics metrics = claimMetrics(consumer);
		rts::RecordImmutableSpatialShadowQuery(mapConsumer(consumer),
			matched != FALSE, metrics);
	}

	void disable(LiveImmutableSpatialConsumer consumer)
	{
		const Int index = consumerIndex(consumer);
		if (index >= 0 && !m_consumerDisabled[index])
		{
			m_consumerDisabled[index] = TRUE;
			rts::RecordImmutableSpatialCircuitBreakerTrip(mapConsumer(consumer));
		}
	}

	void recordUnexpected(LiveImmutableSpatialConsumer consumer, Bool stale,
		Bool validationFailure)
	{
		const rts::ImmutableSpatialJobSystemMetrics metrics = claimMetrics(consumer);
		rts::RecordImmutableSpatialUnexpectedFallback(mapConsumer(consumer),
			stale != FALSE, validationFailure != FALSE, &metrics);
	}

private:
	void beginPerformanceBatch(UnsignedInt frame)
	{
		m_performanceLedger =
			&rts::performance::KernelPerformanceLedger::instance();
		m_performanceBatch = rts::performance::KernelPerformanceBatch();
		m_performanceCaptureInterval =
			rts::performance::KernelPerformanceInterval();
		m_performanceCommitInterval =
			rts::performance::KernelPerformanceInterval();
		m_performanceBatchActive = FALSE;
		m_performanceCompletion.reset(m_batchEpoch);
		if (m_performanceOrdinal ==
			~static_cast<rts::JobMetricCounter>(0))
			return;
		++m_performanceOrdinal;
		m_performanceBatch = m_performanceLedger->beginBatch(
			rts::performance::KERNEL_PERFORMANCE_SPATIAL, 0, frame,
			m_performanceOrdinal);
		m_performanceBatchActive = m_performanceBatch.valid();
	}

	void beginPerformanceCapture()
	{
		if (m_performanceBatchActive &&
			!m_performanceCaptureInterval.valid())
			m_performanceCaptureInterval = m_performanceLedger->beginInterval(
				m_performanceBatch, rts::performance::KERNEL_PERFORMANCE_CAPTURE);
	}

	void endPerformanceCapture()
	{
		if (m_performanceLedger != nullptr &&
			m_performanceCaptureInterval.valid())
		{
			m_performanceLedger->endInterval(m_performanceCaptureInterval);
			m_performanceCaptureInterval =
				rts::performance::KernelPerformanceInterval();
		}
	}

	void endPerformanceCommit()
	{
		if (m_performanceLedger != nullptr &&
			m_performanceCommitInterval.valid())
		{
			m_performanceLedger->endInterval(m_performanceCommitInterval);
			m_performanceCommitInterval =
				rts::performance::KernelPerformanceInterval();
		}
		m_performanceCommitOwner = rts::ImmutableSpatialConsumerCompletionToken();
	}

	void finishPerformanceBatch(
		rts::performance::KernelPerformanceDisposition disposition)
	{
		endPerformanceCommit();
		endPerformanceCapture();
		if (m_performanceLedger != nullptr && m_performanceBatchActive &&
			m_performanceBatch.valid())
		{
			const bool closed = m_performanceLedger->endBatch(m_performanceBatch, disposition);
			if (m_referenceBatch.valid())
				m_referenceLedger->finishBatch(m_referenceBatch, closed &&
					disposition == rts::performance::KERNEL_PERFORMANCE_COMMITTED);
		}
		else if (m_referenceBatch.valid())
			m_referenceLedger->finishBatch(m_referenceBatch, false);
		m_referenceBatch = rts::performance::KernelPerformanceReferenceBatch();
		m_performanceBatch = rts::performance::KernelPerformanceBatch();
		m_performanceBatchActive = FALSE;
	}

	void invalidateCache()
	{
		m_cacheValid = FALSE;
		m_cachedManager = nullptr;
		m_cachedObjects = nullptr;
		m_objectScratch = nullptr;
		m_cachedCells = nullptr;
		m_cachedMembers = nullptr;
		m_cachedRadii = nullptr;
		m_cachedOffsets = nullptr;
		m_cachedMemberPrefix = nullptr;
		m_cachedCellCount = 0;
		m_cachedMemberCount = 0;
		m_cachedRadiusCount = 0;
		m_cachedOffsetCount = 0;
		m_arena = nullptr;
		m_arenaBytes = 0;
		m_objectCount = 0;
		m_cachedGeneration.lifecycle = 0;
		m_cachedGeneration.topology = 0;
		m_cachedGeneration.facts = 0;
	}

	static void copyObjectFacts(rts::ImmutableSpatialObjectRecord &record,
		Object *object, const rts::ImmutableSpatialGeneration &generation)
	{
		record.objectID = static_cast<UnsignedInt>(object->getID());
		record.generation = generation;
		record.admissionMask = 0;
		record.buildCost = 0;
		const Coord3D *position = object->getPosition();
		const GeometryInfo &geometry = object->getGeometryInfo();
		record.positionX = position->x;
		record.positionY = position->y;
		record.positionZ = position->z;
		record.boundingCircleRadius = geometry.getBoundingCircleRadius();
		record.boundingSphereRadius = geometry.getBoundingSphereRadius();
		record.zCenterOffset = geometry.getZDeltaToCenterPosition();
	}

	Bool refreshObjectFacts(PartitionManager *manager)
	{
		if (!m_cacheValid || manager == nullptr || manager != m_cachedManager ||
			m_objectScratch == nullptr ||
			manager->m_immutableSpatialObjectCount != m_objectCount)
			return FALSE;
		UnsignedInt observed = 0;
		for (PartitionData *module = manager->m_moduleList; module != nullptr;
			module = module->getNext())
		{
			Object *object = module->getObject();
			if (object == nullptr)
				continue;
			const UnsignedInt objectID = static_cast<UnsignedInt>(object->getID());
			const UnsignedInt objectIndex = findObjectIndex(m_cachedObjects,
				m_objectCount, objectID);
			if (objectIndex == rts::IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX)
				return FALSE;
			copyObjectFacts(m_objectScratch[objectIndex], object, m_generation);
			++observed;
		}
		if (observed != m_objectCount ||
			rts::RefreshImmutableSpatialArenaObjects(m_arena, m_arenaBytes,
				m_generation, m_objectScratch, m_objectCount) !=
				rts::IMMUTABLE_SPATIAL_SUCCESS)
			return FALSE;
		rts::ImmutableSpatialObjectRecord *previous = m_cachedObjects;
		m_cachedObjects = m_objectScratch;
		m_objectScratch = previous;
		m_cachedGeneration = m_generation;
		return TRUE;
	}

	void clearCollection()
	{
		if (m_performanceCompletion.expectedConsumers != 0 || m_referenceBatch.valid())
			finishPerformanceBatch(rts::performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
		advance(m_batchEpoch);
		m_collectionState = LIVE_SPATIAL_COLLECTION_IDLE;
		m_queryCount = 0;
		m_resultCapacity = 0;
		m_outputCount = 0;
		m_batchFailureStale = FALSE;
		m_failedMetricsClaimed = FALSE;
		m_lastJobMetrics = rts::ImmutableSpatialJobSystemMetrics();
		m_consumerJobMetrics[0] = rts::ImmutableSpatialJobSystemMetrics();
		m_consumerJobMetrics[1] = rts::ImmutableSpatialJobSystemMetrics();
		m_consumerMetricsClaimed[0] = FALSE;
		m_consumerMetricsClaimed[1] = FALSE;
		// A fresh collection starts with no queued consumer completions. The
		// shared performance batch/capture interval deliberately remains alive
		// across this bookkeeping reset.
		m_performanceCompletion.reset(m_batchEpoch);
	}

	static Bool sameFloat(float left, float right)
	{
		return memcmp(&left, &right, sizeof(left)) == 0;
	}

	static Bool ownerIndexLess(
		const LiveImmutableSpatialOwnerIndexEntry &left,
		const LiveImmutableSpatialOwnerIndexEntry &right)
	{
		return left.objectID < right.objectID ||
			(left.objectID == right.objectID &&
				left.batchIndex < right.batchIndex);
	}

	Bool findOwner(UpdateModule *owner, ObjectID objectID,
		LiveImmutableSpatialConsumer consumer, UnsignedInt *batchIndex) const
	{
		if (owner == nullptr || batchIndex == nullptr || m_ownerIndex == nullptr)
			return FALSE;
		const UnsignedInt numericID = static_cast<UnsignedInt>(objectID);
		UnsignedInt low = 0;
		UnsignedInt high = m_queryCount;
		while (low < high)
		{
			const UnsignedInt middle = low + (high - low) / 2;
			if (m_ownerIndex[middle].objectID < numericID)
				low = middle + 1;
			else
				high = middle;
		}
		for (UnsignedInt entry = low; entry != m_queryCount &&
			m_ownerIndex[entry].objectID == numericID; ++entry)
		{
			const UnsignedInt candidate = m_ownerIndex[entry].batchIndex;
			if (candidate < m_queryCount && m_owners[candidate].owner == owner &&
				m_owners[candidate].consumer == consumer &&
				m_owners[candidate].objectID == objectID)
			{
				*batchIndex = candidate;
				return TRUE;
			}
		}
		return FALSE;
	}

	void computeConsumerJobMetrics(UnsignedInt rangeCount)
	{
		m_consumerJobMetrics[0] = rts::ImmutableSpatialJobSystemMetrics();
		m_consumerJobMetrics[1] = rts::ImmutableSpatialJobSystemMetrics();
		if (rangeCount == 0 || m_lastJobMetrics.ranges % rangeCount != 0)
			return;
		const UnsignedInt jobsPerRange = m_lastJobMetrics.ranges / rangeCount;
		const Bool allPhysical = m_lastJobMetrics.physicalWorkerJobs ==
			m_lastJobMetrics.ranges;
		const Bool allOwnerHelp = m_lastJobMetrics.ownerHelpedJobs ==
			m_lastJobMetrics.ranges;
		const UnsignedInt quotient = m_queryCount / rangeCount;
		const UnsignedInt remainder = m_queryCount % rangeCount;
		for (UnsignedInt rangeIndex = 0; rangeIndex != rangeCount; ++rangeIndex)
		{
			const UnsignedInt begin = rangeIndex * quotient +
				(rangeIndex < remainder ? rangeIndex : remainder);
			const UnsignedInt count = quotient +
				(rangeIndex < remainder ? 1u : 0u);
			Bool observed[2] = { FALSE, FALSE };
			for (UnsignedInt queryIndex = begin; queryIndex != begin + count;
				++queryIndex)
			{
				const Int ordinal = consumerIndex(m_owners[queryIndex].consumer);
				if (ordinal >= 0)
					observed[ordinal] = TRUE;
			}
			for (UnsignedInt consumerOrdinal = 0; consumerOrdinal != 2;
				++consumerOrdinal)
			{
				if (!observed[consumerOrdinal])
					continue;
				rts::ImmutableSpatialJobSystemMetrics &metrics =
					m_consumerJobMetrics[consumerOrdinal];
				metrics.dispatches = m_lastJobMetrics.dispatches;
				metrics.ranges += jobsPerRange;
				metrics.submittedJobs += jobsPerRange;
				metrics.completedJobs += jobsPerRange;
				if (allPhysical)
					metrics.physicalWorkerJobs += jobsPerRange;
				if (allOwnerHelp)
					metrics.ownerHelpedJobs += jobsPerRange;
			}
		}
	}

	rts::ImmutableSpatialJobSystemMetrics claimMetrics(
		LiveImmutableSpatialConsumer consumer)
	{
		const Int ordinal = consumerIndex(consumer);
		if (ordinal >= 0 &&
			m_collectionState == LIVE_SPATIAL_COLLECTION_READY &&
			!m_consumerMetricsClaimed[ordinal])
		{
			m_consumerMetricsClaimed[ordinal] = TRUE;
			return m_consumerJobMetrics[ordinal];
		}
		if (m_collectionState == LIVE_SPATIAL_COLLECTION_FAILED &&
			!m_failedMetricsClaimed)
		{
			m_failedMetricsClaimed = TRUE;
			return m_lastJobMetrics;
		}
		return rts::ImmutableSpatialJobSystemMetrics();
	}

	static void advance(rts::ImmutableSpatialUInt32 &generation)
	{
		if (++generation == 0)
			generation = 1;
	}

	static Bool generationEqual(const rts::ImmutableSpatialGeneration &left,
		const rts::ImmutableSpatialGeneration &right)
	{
		return left.lifecycle == right.lifecycle &&
			left.topology == right.topology && left.facts == right.facts;
	}

	static Bool addArrayBytes(size_t &bytes, UnsignedInt count,
		size_t elementBytes)
	{
		if (elementBytes != 0 && count > (static_cast<size_t>(-1) - bytes) /
			elementBytes)
			return FALSE;
		bytes += static_cast<size_t>(count) * elementBytes;
		return TRUE;
	}

	template <typename T>
	static T *take(unsigned char *&cursor, UnsignedInt count)
	{
		T *value = reinterpret_cast<T *>(cursor);
		cursor += static_cast<size_t>(count) * sizeof(T);
		return value;
	}

	Bool ensureStorage(UnsignedInt bytes)
	{
		if (bytes <= m_storageCapacity)
			return TRUE;
		unsigned char *storage = new (std::nothrow) unsigned char[bytes];
		if (storage == nullptr)
			return FALSE;
		delete[] m_storage;
		m_storage = storage;
		m_storageCapacity = bytes;
		return TRUE;
	}

	Bool ensureQueryStorage(UnsignedInt count)
	{
		size_t bytes = 0;
		if (!addArrayBytes(bytes, count, sizeof(rts::ImmutableSpatialQuery)) ||
			!addArrayBytes(bytes, count,
				sizeof(LiveImmutableSpatialPreparedOwner)) ||
			!addArrayBytes(bytes, count,
				sizeof(LiveImmutableSpatialOwnerIndexEntry)) ||
			!addArrayBytes(bytes, count,
				2 * sizeof(rts::ImmutableSpatialUInt32)) ||
			!addArrayBytes(bytes, count,
				2 * sizeof(rts::ImmutableSpatialResultSpan)) ||
			bytes > UINT_MAX)
			return FALSE;
		if (bytes <= m_queryStorageCapacity)
			return TRUE;
		unsigned char *storage = new (std::nothrow) unsigned char[bytes];
		if (storage == nullptr)
			return FALSE;
		delete[] m_queryStorage;
		m_queryStorage = storage;
		m_queryStorageCapacity = static_cast<UnsignedInt>(bytes);
		return TRUE;
	}

	Bool ensureWorkStorage(UnsignedInt rangeCount, UnsignedInt resultCapacity)
	{
		if (m_objectCount != 0 && rangeCount > UINT_MAX / m_objectCount)
			return FALSE;
		const UnsignedInt visitCapacity = rangeCount * m_objectCount;
		size_t bytes = 0;
		if (!addArrayBytes(bytes, visitCapacity,
			sizeof(rts::ImmutableSpatialUInt32)) ||
			!addArrayBytes(bytes, resultCapacity,
				3 * sizeof(rts::ImmutableSpatialResult)) ||
			bytes > UINT_MAX)
			return FALSE;
		if (bytes <= m_workStorageCapacity)
			return TRUE;
		unsigned char *storage = new (std::nothrow) unsigned char[bytes];
		if (storage == nullptr)
			return FALSE;
		delete[] m_workStorage;
		m_workStorage = storage;
		m_workStorageCapacity = static_cast<UnsignedInt>(bytes);
		return TRUE;
	}

	Bool ensureCommitStorage(UnsignedInt count)
	{
		if (count <= m_commitCapacity)
			return TRUE;
		Object **objects = new (std::nothrow) Object *[count];
		if (objects == nullptr)
			return FALSE;
		delete[] m_commitObjects;
		m_commitObjects = objects;
		m_commitCapacity = count;
		return TRUE;
	}

	static Bool objectRecordLess(const rts::ImmutableSpatialObjectRecord &left,
		const rts::ImmutableSpatialObjectRecord &right)
	{
		return left.objectID < right.objectID;
	}

	static UnsignedInt findObjectIndex(
		const rts::ImmutableSpatialObjectRecord *objects, UnsignedInt count,
		UnsignedInt objectID)
	{
		UnsignedInt low = 0;
		UnsignedInt high = count;
		while (low < high)
		{
			const UnsignedInt middle = low + (high - low) / 2;
			if (objects[middle].objectID < objectID)
				low = middle + 1;
			else
				high = middle;
		}
		return low < count && objects[low].objectID == objectID ? low :
			rts::IMMUTABLE_SPATIAL_INVALID_OBJECT_INDEX;
	}

	static Int consumerIndex(LiveImmutableSpatialConsumer consumer)
	{
		if (consumer == LIVE_IMMUTABLE_SPATIAL_HEALING)
			return 0;
		if (consumer == LIVE_IMMUTABLE_SPATIAL_POINT_DEFENSE_LASER)
			return 1;
		return -1;
	}

	static rts::ImmutableSpatialConsumer mapConsumer(
		LiveImmutableSpatialConsumer consumer)
	{
		return consumer == LIVE_IMMUTABLE_SPATIAL_POINT_DEFENSE_LASER ?
			rts::IMMUTABLE_SPATIAL_CONSUMER_POINT_DEFENSE_LASER :
			rts::IMMUTABLE_SPATIAL_CONSUMER_HEALING;
	}

	const rts::ImmutableSpatialArenaHeader *arenaHeader() const
	{
		return reinterpret_cast<const rts::ImmutableSpatialArenaHeader *>(m_arena);
	}

	static bool resolveArenaGeneration(
		const rts::ImmutableSpatialGeneration *expected, void *context)
	{
		ImmutableSpatialQueryRuntime *runtime =
			static_cast<ImmutableSpatialQueryRuntime *>(context);
		return runtime != nullptr && expected != nullptr && runtime->m_ready &&
			generationEqual(*expected, runtime->m_generation);
	}

	static bool resolveObjectGeneration(UnsignedInt objectID,
		const rts::ImmutableSpatialGeneration *expected, void *context)
	{
		ImmutableSpatialQueryRuntime *runtime =
			static_cast<ImmutableSpatialQueryRuntime *>(context);
		if (runtime == nullptr || expected == nullptr || !runtime->m_ready ||
			!generationEqual(*expected, runtime->m_generation) ||
			TheGameLogic == nullptr)
			return false;
		Object *object = TheGameLogic->findObjectByID(static_cast<ObjectID>(objectID));
		return object != nullptr && object->friend_getPartitionData() != nullptr &&
			static_cast<UnsignedInt>(object->getID()) == objectID;
	}

	unsigned char *m_storage;
	UnsignedInt m_storageCapacity;
	rts::ImmutableSpatialObjectRecord *m_cachedObjects;
	rts::ImmutableSpatialObjectRecord *m_objectScratch;
	rts::ImmutableSpatialCellRecord *m_cachedCells;
	rts::ImmutableSpatialMemberRecord *m_cachedMembers;
	rts::ImmutableSpatialRadiusRecord *m_cachedRadii;
	rts::ImmutableSpatialOffsetRecord *m_cachedOffsets;
	rts::ImmutableSpatialUInt32 *m_cachedMemberPrefix;
	UnsignedInt m_cachedCellCount;
	UnsignedInt m_cachedMemberCount;
	UnsignedInt m_cachedRadiusCount;
	UnsignedInt m_cachedOffsetCount;
	void *m_arena;
	UnsignedInt m_arenaBytes;
	UnsignedInt m_captureFrame;
	UnsignedInt m_objectCount;
	rts::ImmutableSpatialGeneration m_generation;
	rts::ImmutableSpatialUInt32 *m_counts;
	rts::ImmutableSpatialUInt32 *m_states;
	rts::ImmutableSpatialUInt32 *m_visitStamps;
	rts::ImmutableSpatialResultSpan *m_spanScratch;
	rts::ImmutableSpatialResult *m_resultScratch;
	rts::ImmutableSpatialResult *m_sortScratch;
	rts::ImmutableSpatialResult *m_results;
	rts::ImmutableSpatialResultSpan *m_resultSpans;
	UnsignedInt m_outputCount;
	ObjectID *m_firstIDs;
	ObjectID *m_secondIDs;
	Object **m_commitObjects;
	UnsignedInt m_commitCapacity;
	unsigned char *m_queryStorage;
	UnsignedInt m_queryStorageCapacity;
	unsigned char *m_workStorage;
	UnsignedInt m_workStorageCapacity;
	rts::ImmutableSpatialQuery *m_queries;
	LiveImmutableSpatialPreparedOwner *m_owners;
	LiveImmutableSpatialOwnerIndexEntry *m_ownerIndex;
	UnsignedInt m_queryCapacity;
	UnsignedInt m_queryCount;
	UnsignedInt m_resultCapacity;
	PartitionManager *m_captureManager;
	PartitionManager *m_cachedManager;
	rts::ImmutableSpatialGeneration m_cachedGeneration;
	Bool m_cacheValid;
	LiveImmutableSpatialCollectionState m_collectionState;
	UnsignedInt m_batchEpoch;
	Bool m_batchFailureStale;
	Bool m_failedMetricsClaimed;
	rts::ImmutableSpatialJobSystemMetrics m_lastJobMetrics;
	rts::ImmutableSpatialJobSystemMetrics m_consumerJobMetrics[2];
	Bool m_consumerMetricsClaimed[2];
	Bool m_ready;
	Bool m_consumerDisabled[2];
	rts::performance::KernelPerformanceLedger *m_performanceLedger;
	rts::performance::KernelPerformanceBatch m_performanceBatch;
	rts::performance::KernelPerformanceInterval m_performanceCaptureInterval;
	rts::performance::KernelPerformanceInterval m_performanceCommitInterval;
	rts::JobMetricCounter m_performanceOrdinal;
	rts::ImmutableSpatialCollectionCompletion m_performanceCompletion;
	rts::ImmutableSpatialConsumerCompletionToken m_performanceCommitOwner;
	rts::performance::KernelPerformanceReferenceLedger *m_referenceLedger;
	rts::performance::KernelPerformanceReferenceBatch m_referenceBatch;
	Bool m_performanceBatchActive;
};

namespace
{
ImmutableSpatialQueryRuntime &liveSpatialRuntime()
{
	static ImmutableSpatialQueryRuntime runtime;
	return runtime;
}
}

Bool CaptureLiveImmutableSpatialArena(PartitionManager *manager,
	UnsignedInt frame)
{
	return liveSpatialRuntime().capture(manager, frame);
}

LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryScheduler()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	if (!rts::ShouldPrepareLiveSimulationKernelOffThread(
			rts::MULTIPLAYER_SIMULATION_KERNEL_SPATIAL) ||
		!jobs.isRunning() || jobs.workerCount() < 2 || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(rts::JOB_OWNER_GAME))
		return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;
	return LIVE_IMMUTABLE_SPATIAL_COLLECTION_ELIGIBLE;
}

LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryCollection(
	UnsignedInt queueableQueryCount)
{
	const LiveImmutableSpatialCollectionPreflightResult scheduler =
		PreflightLiveImmutableSpatialQueryScheduler();
	if (scheduler != LIVE_IMMUTABLE_SPATIAL_COLLECTION_ELIGIBLE)
		return scheduler;
	if (queueableQueryCount > LIVE_IMMUTABLE_SPATIAL_MAXIMUM_QUERIES)
		return LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED;
	if (queueableQueryCount < 2)
		return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	return rts::ShouldDispatchImmutableSpatialQueryCollection(
		queueableQueryCount, jobs.workerCount()) ?
		LIVE_IMMUTABLE_SPATIAL_COLLECTION_ELIGIBLE :
		LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED;
}

Bool MeasureLiveImmutableSpatialQueryCost(PartitionManager *manager,
	const Coord3D *position, Real maximumDistance, UnsignedInt *cellVisits,
	UnsignedInt *memberVisits)
{
	return liveSpatialRuntime().measureQueryCost(manager, position,
		maximumDistance, cellVisits, memberVisits);
}

LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryCollectionCost(PartitionManager *manager,
	UnsignedInt queueableQueryCount, UnsignedInt queryCellVisits,
	UnsignedInt queryMemberVisits, UnsignedInt maximumRangeCost,
	UnsignedInt ownerScanCount, UnsignedInt ownerSortComparisons,
	UnsignedInt ownerLookupComparisons)
{
	return liveSpatialRuntime().preflightCost(manager, queueableQueryCount,
		queryCellVisits, queryMemberVisits, maximumRangeCost, ownerScanCount,
		ownerSortComparisons, ownerLookupComparisons);
}

void PublishLiveImmutableSpatialNoCaptureState(
	LiveImmutableSpatialCollectionPreflightResult result,
	PartitionManager *manager, UnsignedInt frame)
{
	liveSpatialRuntime().publishNoCaptureState(result, manager, frame);
}

Bool IsLiveImmutableSpatialConsumerQueueable(
	LiveImmutableSpatialConsumer consumer)
{
	return liveSpatialRuntime().canQueueConsumer(consumer);
}

Bool BeginLiveImmutableSpatialQueryCollection(UnsignedInt maximumQueryCount)
{
	return liveSpatialRuntime().beginCollection(maximumQueryCount);
}

Bool QueueLiveImmutableSpatialQuery(UpdateModule *owner,
	PartitionManager *manager, const Coord3D *position, Real maximumDistance,
	UnsignedInt frame, LiveImmutableSpatialConsumer consumer)
{
	return liveSpatialRuntime().queue(owner, manager, position, maximumDistance,
		frame, consumer);
}

void ExecuteLiveImmutableSpatialQueryCollection()
{
	liveSpatialRuntime().executeCollection();
}

void EndLiveImmutableSpatialQueryCollection()
{
	liveSpatialRuntime().endCollection();
}

void ResetLiveImmutableSpatialRuntime()
{
	liveSpatialRuntime().reset();
}

void InvalidateLiveImmutableSpatialLifecycle()
{
	liveSpatialRuntime().invalidateLifecycle();
}

void InvalidateLiveImmutableSpatialTopology()
{
	liveSpatialRuntime().invalidateTopology();
}

void InvalidateLiveImmutableSpatialFacts()
{
	liveSpatialRuntime().invalidateFacts();
}

LiveImmutableSpatialQueryResult QueryLiveImmutableSpatialCandidates(
	UpdateModule *owner, PartitionManager *manager, const Coord3D *position,
	Real maximumDistance, UnsignedInt frame,
	LiveImmutableSpatialConsumer consumer,
	LiveImmutableSpatialResultView *view)
{
	return liveSpatialRuntime().query(owner, manager, position, maximumDistance,
		frame, consumer, view);
}

Object *ResolveLiveImmutableSpatialResult(
	const rts::ImmutableSpatialResult &result)
{
	return liveSpatialRuntime().resolve(result);
}

Object *ResolveLiveImmutableSpatialObjectID(ObjectID objectID)
{
	return liveSpatialRuntime().resolveID(objectID);
}

Bool ValidateLiveImmutableSpatialResultView(
	const LiveImmutableSpatialResultView &view)
{
	return liveSpatialRuntime().validate(view);
}

Bool GetLiveImmutableSpatialIDBuffers(ObjectID **first, ObjectID **second,
	UnsignedInt *capacity)
{
	return liveSpatialRuntime().buffers(first, second, capacity);
}

Bool GetLiveImmutableSpatialCommitBuffer(Object ***objects,
	UnsignedInt *capacity)
{
	return liveSpatialRuntime().commitBuffer(objects, capacity);
}

rts::ImmutableSpatialConsumerCompletionToken CaptureLiveImmutableSpatialCompletion(
	UpdateModule *owner, LiveImmutableSpatialConsumer consumer)
{
	return liveSpatialRuntime().captureCompletion(owner, consumer);
}

void BeginLiveImmutableSpatialCommit(LiveImmutableSpatialConsumer consumer,
	const rts::ImmutableSpatialConsumerCompletionToken &token)
{
	liveSpatialRuntime().beginCommit(consumer, token);
}

void EndLiveImmutableSpatialCommit(LiveImmutableSpatialConsumer consumer,
	const rts::ImmutableSpatialConsumerCompletionToken &token)
{
	liveSpatialRuntime().endCommit(consumer, token);
}

void CompleteLiveImmutableSpatialConsumer(
	LiveImmutableSpatialConsumer consumer,
	const rts::ImmutableSpatialConsumerCompletionToken &token, Bool committed)
{
	liveSpatialRuntime().completeConsumer(consumer, token, committed);
}

void RecordLiveImmutableSpatialAuthoritativeQuery(
	LiveImmutableSpatialConsumer consumer, UnsignedInt candidateCount)
{
	liveSpatialRuntime().recordAuthoritative(consumer, candidateCount);
}

void RecordLiveImmutableSpatialShadowQuery(
	LiveImmutableSpatialConsumer consumer, Bool matched)
{
	liveSpatialRuntime().recordShadow(consumer, matched);
}

void DisableLiveImmutableSpatialConsumer(
	LiveImmutableSpatialConsumer consumer)
{
	liveSpatialRuntime().disable(consumer);
}

void RecordLiveImmutableSpatialUnexpectedFallback(
	LiveImmutableSpatialConsumer consumer, Bool stale, Bool validationFailure)
{
	liveSpatialRuntime().recordUnexpected(consumer, stale, validationFailure);
}

#else

Bool CaptureLiveImmutableSpatialArena(PartitionManager *, UnsignedInt)
{
	return FALSE;
}
LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryScheduler()
{
	return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;
}
LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryCollection(UnsignedInt)
{
	return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;
}
Bool MeasureLiveImmutableSpatialQueryCost(PartitionManager *, const Coord3D *,
	Real, UnsignedInt *, UnsignedInt *)
{
	return FALSE;
}
LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryCollectionCost(PartitionManager *,
	UnsignedInt, UnsignedInt, UnsignedInt, UnsignedInt, UnsignedInt,
	UnsignedInt, UnsignedInt)
{
	return LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK;
}
void PublishLiveImmutableSpatialNoCaptureState(
	LiveImmutableSpatialCollectionPreflightResult, PartitionManager *, UnsignedInt) { }
Bool IsLiveImmutableSpatialConsumerQueueable(LiveImmutableSpatialConsumer)
{
	return FALSE;
}
Bool BeginLiveImmutableSpatialQueryCollection(UnsignedInt) { return FALSE; }
Bool QueueLiveImmutableSpatialQuery(UpdateModule *, PartitionManager *,
	const Coord3D *, Real, UnsignedInt, LiveImmutableSpatialConsumer)
{
	return FALSE;
}
void ExecuteLiveImmutableSpatialQueryCollection() { }
void EndLiveImmutableSpatialQueryCollection() { }
void ResetLiveImmutableSpatialRuntime() { }
void InvalidateLiveImmutableSpatialLifecycle() { }
void InvalidateLiveImmutableSpatialTopology() { }
void InvalidateLiveImmutableSpatialFacts() { }
LiveImmutableSpatialQueryResult QueryLiveImmutableSpatialCandidates(
	UpdateModule *, PartitionManager *, const Coord3D *, Real, UnsignedInt,
	LiveImmutableSpatialConsumer, LiveImmutableSpatialResultView *)
{
	return LIVE_IMMUTABLE_SPATIAL_QUERY_POLICY_FALLBACK;
}
Object *ResolveLiveImmutableSpatialResult(
	const rts::ImmutableSpatialResult &) { return nullptr; }
Object *ResolveLiveImmutableSpatialObjectID(ObjectID) { return nullptr; }
Bool ValidateLiveImmutableSpatialResultView(
	const LiveImmutableSpatialResultView &) { return FALSE; }
Bool GetLiveImmutableSpatialIDBuffers(ObjectID **, ObjectID **,
	UnsignedInt *) { return FALSE; }
Bool GetLiveImmutableSpatialCommitBuffer(Object ***, UnsignedInt *)
{
	return FALSE;
}
void RecordLiveImmutableSpatialAuthoritativeQuery(
	LiveImmutableSpatialConsumer, UnsignedInt) { }
void RecordLiveImmutableSpatialShadowQuery(
	LiveImmutableSpatialConsumer, Bool) { }
void DisableLiveImmutableSpatialConsumer(LiveImmutableSpatialConsumer) { }
void RecordLiveImmutableSpatialUnexpectedFallback(
	LiveImmutableSpatialConsumer, Bool, Bool) { }

#endif
