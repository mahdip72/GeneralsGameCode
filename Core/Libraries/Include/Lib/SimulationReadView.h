/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/BaseTypeCore.h"

namespace rts
{
// Fixed, pointer-free object facts captured by the simulation owner. Worker
// code receives this record by const view and cannot resolve a live Object.
struct SimulationReadObjectRecord
{
	UnsignedInt objectID;
	UnsignedInt objectGeneration;
	float positionX;
	float positionY;
	float positionZ;
	float boundingSphereRadius;
	float zCenterOffset;
};

// A stable module identity is an owner-defined build-stable adapter ID plus
// the module's stable ordinal within its object. Runtime NameKeys and pointers
// are deliberately absent.
struct SimulationReadModuleRecord
{
	UnsignedInt objectID;
	UnsignedInt moduleOrdinal;
	UnsignedInt moduleType;
	UnsignedInt wakePriority;
	float queryRadius;
	// Flattened owner cell and conservative query-cell radius. These values
	// are meaningful only for a view published with a spatial index.
	UnsignedInt spatialCellIndex;
	UnsignedInt spatialCellRadius;
};

// Non-empty cells are stored in increasing row-major cellIndex order. Each
// span names a strictly ObjectID-ordered slice of spatialObjectIndices.
struct SimulationReadSpatialCellSpan
{
	UnsignedInt cellIndex;
	UnsignedInt objectIndexOffset;
	UnsignedInt objectIndexCount;
};

// Schedule entries preserve the exact owner scheduler order independently of
// the ObjectID/module-ordered module table.
struct SimulationReadScheduleEntry
{
	UnsignedInt moduleIndex;
	UnsignedInt ownerOrder;
};

class SimulationReadView
{
public:
	// Publication contract: the owner fully initializes and validates every
	// backing record before submitting work, then keeps those arrays alive and
	// byte-for-byte unchanged until all jobs are fenced and owner commit ends.
	// Records contain values only; a worker cannot resolve or dereference a
	// live engine Object or module through this interface.
	SimulationReadView();
	SimulationReadView(UnsignedInt frame, UnsignedInt generation,
		const SimulationReadObjectRecord *objects, UnsignedInt objectCount,
		const SimulationReadModuleRecord *modules, UnsignedInt moduleCount,
		const SimulationReadScheduleEntry *schedule,
		UnsignedInt scheduleCount);
	SimulationReadView(UnsignedInt frame, UnsignedInt generation,
		const SimulationReadObjectRecord *objects, UnsignedInt objectCount,
		const SimulationReadModuleRecord *modules, UnsignedInt moduleCount,
		const SimulationReadScheduleEntry *schedule,
		UnsignedInt scheduleCount, UnsignedInt cellCountX,
		UnsignedInt cellCountY,
		const SimulationReadSpatialCellSpan *spatialCellSpans,
		UnsignedInt spatialCellSpanCount,
		const UnsignedInt *spatialObjectIndices,
		UnsignedInt spatialObjectIndexCount);

	UnsignedInt frame() const;
	UnsignedInt generation() const;
	UnsignedInt objectCount() const;
	UnsignedInt moduleCount() const;
	UnsignedInt scheduleCount() const;
	const SimulationReadObjectRecord *objectAt(UnsignedInt index) const;
	const SimulationReadModuleRecord *moduleAt(UnsignedInt index) const;
	const SimulationReadScheduleEntry *scheduleAt(UnsignedInt index) const;
	bool hasSpatialIndex() const;
	UnsignedInt cellCountX() const;
	UnsignedInt cellCountY() const;
	UnsignedInt spatialCellSpanCount() const;
	UnsignedInt spatialObjectIndexCount() const;
	const SimulationReadSpatialCellSpan *spatialCellSpanAt(
		UnsignedInt index) const;
	const SimulationReadSpatialCellSpan *findSpatialCellSpan(
		UnsignedInt cellIndex) const;
	UnsignedInt spatialObjectIndexAt(UnsignedInt index) const;
	bool isValid() const;

private:
	UnsignedInt m_frame;
	UnsignedInt m_generation;
	const SimulationReadObjectRecord *m_objects;
	UnsignedInt m_objectCount;
	const SimulationReadModuleRecord *m_modules;
	UnsignedInt m_moduleCount;
	const SimulationReadScheduleEntry *m_schedule;
	UnsignedInt m_scheduleCount;
	UnsignedInt m_cellCountX;
	UnsignedInt m_cellCountY;
	const SimulationReadSpatialCellSpan *m_spatialCellSpans;
	UnsignedInt m_spatialCellSpanCount;
	const UnsignedInt *m_spatialObjectIndices;
	UnsignedInt m_spatialObjectIndexCount;
};
}
