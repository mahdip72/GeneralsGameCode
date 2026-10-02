/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/SimulationReadView.h"

#include <float.h>

namespace rts
{
namespace
{
bool finiteFloat(float value)
{
	return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

bool objectRecordValid(const SimulationReadObjectRecord &record)
{
	return record.objectID != 0 && record.objectGeneration != 0 &&
		finiteFloat(record.positionX) && finiteFloat(record.positionY) &&
		finiteFloat(record.positionZ) &&
		finiteFloat(record.boundingSphereRadius) &&
		finiteFloat(record.zCenterOffset) &&
		record.boundingSphereRadius >= 0.0f;
}

bool moduleRecordLess(const SimulationReadModuleRecord &left,
	const SimulationReadModuleRecord &right)
{
	if (left.objectID != right.objectID)
		return left.objectID < right.objectID;
	if (left.moduleOrdinal != right.moduleOrdinal)
		return left.moduleOrdinal < right.moduleOrdinal;
	return left.moduleType < right.moduleType;
}
}

SimulationReadView::SimulationReadView()
	: m_frame(0), m_generation(0), m_objects(0), m_objectCount(0),
	  m_modules(0), m_moduleCount(0), m_schedule(0), m_scheduleCount(0),
	  m_cellCountX(0), m_cellCountY(0), m_spatialCellSpans(0),
	  m_spatialCellSpanCount(0), m_spatialObjectIndices(0),
	  m_spatialObjectIndexCount(0)
{
}

SimulationReadView::SimulationReadView(UnsignedInt frame,
	UnsignedInt generation, const SimulationReadObjectRecord *objects,
	UnsignedInt objectCount, const SimulationReadModuleRecord *modules,
	UnsignedInt moduleCount, const SimulationReadScheduleEntry *schedule,
	UnsignedInt scheduleCount)
	: m_frame(frame), m_generation(generation), m_objects(objects),
	  m_objectCount(objectCount), m_modules(modules),
	  m_moduleCount(moduleCount), m_schedule(schedule),
	  m_scheduleCount(scheduleCount), m_cellCountX(0), m_cellCountY(0),
	  m_spatialCellSpans(0), m_spatialCellSpanCount(0),
	  m_spatialObjectIndices(0), m_spatialObjectIndexCount(0)
{
}

SimulationReadView::SimulationReadView(UnsignedInt frame,
	UnsignedInt generation, const SimulationReadObjectRecord *objects,
	UnsignedInt objectCount, const SimulationReadModuleRecord *modules,
	UnsignedInt moduleCount, const SimulationReadScheduleEntry *schedule,
	UnsignedInt scheduleCount, UnsignedInt cellCountX,
	UnsignedInt cellCountY,
	const SimulationReadSpatialCellSpan *spatialCellSpans,
	UnsignedInt spatialCellSpanCount,
	const UnsignedInt *spatialObjectIndices,
	UnsignedInt spatialObjectIndexCount)
	: m_frame(frame), m_generation(generation), m_objects(objects),
	  m_objectCount(objectCount), m_modules(modules),
	  m_moduleCount(moduleCount), m_schedule(schedule),
	  m_scheduleCount(scheduleCount), m_cellCountX(cellCountX),
	  m_cellCountY(cellCountY), m_spatialCellSpans(spatialCellSpans),
	  m_spatialCellSpanCount(spatialCellSpanCount),
	  m_spatialObjectIndices(spatialObjectIndices),
	  m_spatialObjectIndexCount(spatialObjectIndexCount)
{
}

UnsignedInt SimulationReadView::frame() const { return m_frame; }
UnsignedInt SimulationReadView::generation() const { return m_generation; }
UnsignedInt SimulationReadView::objectCount() const { return m_objectCount; }
UnsignedInt SimulationReadView::moduleCount() const { return m_moduleCount; }
UnsignedInt SimulationReadView::scheduleCount() const
{
	return m_scheduleCount;
}

const SimulationReadObjectRecord *SimulationReadView::objectAt(
	UnsignedInt index) const
{
	return index < m_objectCount ? m_objects + index : 0;
}

const SimulationReadModuleRecord *SimulationReadView::moduleAt(
	UnsignedInt index) const
{
	return index < m_moduleCount ? m_modules + index : 0;
}

const SimulationReadScheduleEntry *SimulationReadView::scheduleAt(
	UnsignedInt index) const
{
	return index < m_scheduleCount ? m_schedule + index : 0;
}

bool SimulationReadView::hasSpatialIndex() const
{
	return m_cellCountX != 0 || m_cellCountY != 0 ||
		m_spatialCellSpanCount != 0 || m_spatialObjectIndexCount != 0;
}

UnsignedInt SimulationReadView::cellCountX() const { return m_cellCountX; }
UnsignedInt SimulationReadView::cellCountY() const { return m_cellCountY; }
UnsignedInt SimulationReadView::spatialCellSpanCount() const
{
	return m_spatialCellSpanCount;
}
UnsignedInt SimulationReadView::spatialObjectIndexCount() const
{
	return m_spatialObjectIndexCount;
}

const SimulationReadSpatialCellSpan *SimulationReadView::spatialCellSpanAt(
	UnsignedInt index) const
{
	return index < m_spatialCellSpanCount ? m_spatialCellSpans + index : 0;
}

const SimulationReadSpatialCellSpan *SimulationReadView::findSpatialCellSpan(
	UnsignedInt cellIndex) const
{
	UnsignedInt low = 0;
	UnsignedInt high = m_spatialCellSpanCount;
	while (low < high)
	{
		const UnsignedInt middle = low + (high - low) / 2;
		if (m_spatialCellSpans[middle].cellIndex < cellIndex) low = middle + 1;
		else high = middle;
	}
	return low < m_spatialCellSpanCount &&
		m_spatialCellSpans[low].cellIndex == cellIndex ?
		m_spatialCellSpans + low : 0;
}

UnsignedInt SimulationReadView::spatialObjectIndexAt(UnsignedInt index) const
{
	return index < m_spatialObjectIndexCount ?
		m_spatialObjectIndices[index] : ~static_cast<UnsignedInt>(0);
}

bool SimulationReadView::isValid() const
{
	if (m_generation == 0 || m_objectCount == 0 ||
		m_moduleCount == 0 || m_scheduleCount != m_moduleCount ||
		m_objects == 0 || m_modules == 0 || m_schedule == 0)
		return false;
	const bool spatial = hasSpatialIndex();
	if (spatial && (m_cellCountX == 0 || m_cellCountY == 0 ||
		m_spatialCellSpanCount == 0 || m_spatialObjectIndexCount == 0 ||
		m_spatialCellSpans == 0 || m_spatialObjectIndices == 0 ||
		m_cellCountY > ~static_cast<UnsignedInt>(0) / m_cellCountX))
		return false;
	const UnsignedInt cellCount = spatial ?
		m_cellCountX * m_cellCountY : 0;

	UnsignedInt index;
	for (index = 0; index < m_objectCount; ++index)
	{
		if (!objectRecordValid(m_objects[index]) ||
			(index != 0 && m_objects[index - 1].objectID >=
				m_objects[index].objectID))
			return false;
	}
	for (index = 0; index < m_moduleCount; ++index)
	{
		const SimulationReadModuleRecord &module = m_modules[index];
		if (module.objectID == 0 || module.moduleType == 0 ||
			module.wakePriority == 0 || !finiteFloat(module.queryRadius) ||
			module.queryRadius < 0.0f ||
			(index != 0 && !moduleRecordLess(m_modules[index - 1], module)))
			return false;
		if (spatial && (module.spatialCellIndex >= cellCount ||
			module.spatialCellRadius >= cellCount))
			return false;
		UnsignedInt low = 0;
		UnsignedInt high = m_objectCount;
		while (low < high)
		{
			const UnsignedInt middle = low + (high - low) / 2;
			if (m_objects[middle].objectID < module.objectID)
				low = middle + 1;
			else
				high = middle;
		}
		if (low == m_objectCount ||
			m_objects[low].objectID != module.objectID)
			return false;
	}
	if (spatial)
	{
		UnsignedInt expectedOffset = 0;
		for (index = 0; index < m_spatialCellSpanCount; ++index)
		{
			const SimulationReadSpatialCellSpan &span =
				m_spatialCellSpans[index];
			if (span.cellIndex >= cellCount || span.objectIndexCount == 0 ||
				span.objectIndexOffset != expectedOffset ||
				span.objectIndexCount > m_spatialObjectIndexCount - expectedOffset ||
				(index != 0 && m_spatialCellSpans[index - 1].cellIndex >=
					span.cellIndex))
				return false;
			UnsignedInt priorObjectIndex = 0;
			for (UnsignedInt member = 0; member < span.objectIndexCount; ++member)
			{
				const UnsignedInt objectIndex =
					m_spatialObjectIndices[expectedOffset + member];
				if (objectIndex >= m_objectCount ||
					(member != 0 && priorObjectIndex >= objectIndex))
					return false;
				priorObjectIndex = objectIndex;
			}
			expectedOffset += span.objectIndexCount;
		}
		if (expectedOffset != m_spatialObjectIndexCount) return false;
	}

	for (index = 0; index < m_scheduleCount; ++index)
	{
		const SimulationReadScheduleEntry &entry = m_schedule[index];
		if (entry.ownerOrder != index || entry.moduleIndex >= m_moduleCount)
			return false;
		for (UnsignedInt prior = 0; prior < index; ++prior)
			if (m_schedule[prior].moduleIndex == entry.moduleIndex)
				return false;
	}
	return true;
}
}
