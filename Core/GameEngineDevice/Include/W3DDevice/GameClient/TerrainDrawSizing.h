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

namespace rts
{
	struct TerrainDrawSizingInput
	{
		float cameraHeight;
		float cameraToPivotDistance;
		float pitchRadians;
		float horizontalFovRadians;
		float verticalFovRadians;
		float worldUnitsPerCell;
		int mapWidth;
		int mapHeight;
		int minimumWidth;
		int minimumHeight;
		int tileLength;
	};

	bool CalculateTerrainDrawSize(const TerrainDrawSizingInput &input, int &width, int &height);
}
