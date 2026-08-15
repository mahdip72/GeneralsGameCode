#include "W3DDevice/GameClient/TerrainDrawSizing.h"

#include <math.h>

namespace
{
	int ClampDrawSize(float requiredCells, int minimumSize, int tileLength, int mapExtent)
	{
		int requestedSize = (int)ceil(requiredCells);
		if (requestedSize < minimumSize)
		{
			requestedSize = minimumSize;
		}

		if (requestedSize < mapExtent)
		{
			const int cellsBeyondFirst = requestedSize - 1;
			requestedSize = 1 + ((cellsBeyondFirst + tileLength - 1) / tileLength) * tileLength;
		}

		return requestedSize < mapExtent ? requestedSize : mapExtent;
	}
}

namespace rts
{
	bool CalculateTerrainDrawSize(const TerrainDrawSizingInput &input, int &width, int &height)
	{
		if (input.cameraHeight <= 0.0f ||
			input.cameraToPivotDistance < 0.0f ||
			input.pitchRadians <= 0.0f ||
			input.horizontalFovRadians <= 0.0f ||
			input.verticalFovRadians <= 0.0f ||
			input.worldUnitsPerCell <= 0.0f ||
			input.mapWidth <= 0 ||
			input.mapHeight <= 0 ||
			input.minimumWidth <= 0 ||
			input.minimumHeight <= 0 ||
			input.tileLength <= 0)
		{
			return false;
		}

		const float halfHorizontalTangent = (float)tan(input.horizontalFovRadians * 0.5f);
		const float halfVerticalTangent = (float)tan(input.verticalFovRadians * 0.5f);
		const float sinPitch = (float)sin(input.pitchRadians);
		const float cosPitch = (float)cos(input.pitchRadians);

		const float farDown = sinPitch - cosPitch * halfVerticalTangent;
		if (farDown <= 0.01f)
		{
			width = input.mapWidth;
			height = input.mapHeight;
			return true;
		}

		const float nearDown = sinPitch + cosPitch * halfVerticalTangent;
		if (nearDown <= 0.01f)
		{
			return false;
		}

		const float farForward = input.cameraHeight *
			(cosPitch + sinPitch * halfVerticalTangent) / farDown;
		const float nearForward = input.cameraHeight *
			(cosPitch - sinPitch * halfVerticalTangent) / nearDown;
		const float farHalfWidth = input.cameraHeight * halfHorizontalTangent / farDown;
		const float nearHalfWidth = input.cameraHeight * halfHorizontalTangent / nearDown;
		const float farFromPivot = farForward - input.cameraToPivotDistance;
		const float nearFromPivot = nearForward - input.cameraToPivotDistance;
		const float farRadius = (float)sqrt(
			farFromPivot * farFromPivot + farHalfWidth * farHalfWidth);
		const float nearRadius = (float)sqrt(
			nearFromPivot * nearFromPivot + nearHalfWidth * nearHalfWidth);

		// Terrain is centered on the camera pivot. A square using twice the most distant
		// frustum-corner radius remains conservative for every camera yaw.
		const float footprintDiameter = 2.0f * (farRadius > nearRadius ? farRadius : nearRadius);
		const float safetyMarginCells = (float)input.tileLength;
		const float requiredCells = footprintDiameter / input.worldUnitsPerCell + safetyMarginCells;
		const int largerMapExtent = input.mapWidth > input.mapHeight ? input.mapWidth : input.mapHeight;
		if (requiredCells >= (float)largerMapExtent)
		{
			width = input.mapWidth;
			height = input.mapHeight;
			return true;
		}

		width = ClampDrawSize(requiredCells, input.minimumWidth, input.tileLength, input.mapWidth);
		height = ClampDrawSize(requiredCells, input.minimumHeight, input.tileLength, input.mapHeight);
		return true;
	}
}
