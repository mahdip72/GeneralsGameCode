/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectComputationIsland.h"

// This translation unit deliberately uses only C++98 syntax. The legacy lane
// compiles it to keep the public capture/command contract VC6-parseable even
// though physical worker execution is native-only.
int main()
{
	rts::SimulationReadObjectRecord object;
	object.objectID = 1;
	object.objectGeneration = 1;
	object.positionX = 0.0f;
	object.positionY = 0.0f;
	object.positionZ = 0.0f;
	object.boundingSphereRadius = 1.0f;
	object.zCenterOffset = 1.0f;

	rts::SimulationReadModuleRecord module;
	module.objectID = 1;
	module.moduleOrdinal = 0;
	module.moduleType = rts::OBJECT_COMPUTATION_MODULE_HORDE;
	module.wakePriority = 2;
	module.queryRadius = 10.0f;
	module.spatialCellIndex = 0;
	module.spatialCellRadius = 0;
	rts::SimulationReadScheduleEntry schedule;
	schedule.moduleIndex = 0;
	schedule.ownerOrder = 0;
	rts::SimulationReadSpatialCellSpan cell;
	cell.cellIndex = 0;
	cell.objectIndexOffset = 0;
	cell.objectIndexCount = 1;
	UnsignedInt spatialObjectIndex = 0;
	rts::SimulationReadView view(0, 1, &object, 1, &module, 1,
		&schedule, 1, 1, 1, &cell, 1, &spatialObjectIndex, 1);
	if (!view.isValid()) return 1;

	// Link through the production implementation as well as parsing its public
	// headers. The one-worker legacy runtime must deterministically retain the
	// serial reference without requiring chrono/thread support.
	rts::ObjectComputationIsland island;
	rts::ObjectComputationOptions options;
	rts::ObjectComputationMetrics metrics;
	if (rts::PreflightObjectComputationIsland(options, &metrics) !=
		rts::OBJECT_COMPUTATION_SERIAL_REFERENCE || metrics.allocatedBytes != 0 ||
		metrics.submittedJobs != 0)
		return 2;
	if (island.prepare(view, options, &metrics) !=
		rts::OBJECT_COMPUTATION_SERIAL_REFERENCE)
		return 3;
	return island.commandCount() == 1 ? 0 : 4;
}
