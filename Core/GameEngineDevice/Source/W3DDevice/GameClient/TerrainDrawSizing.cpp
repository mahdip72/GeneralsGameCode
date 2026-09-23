#include "W3DDevice/GameClient/TerrainDrawSizing.h"

#include <math.h>
#include <float.h>

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
		if (!(requiredCells < (float)largerMapExtent))
		{
			width = input.mapWidth;
			height = input.mapHeight;
			return true;
		}

		width = ClampDrawSize(requiredCells, input.minimumWidth, input.tileLength, input.mapWidth);
		height = ClampDrawSize(requiredCells, input.minimumHeight, input.tileLength, input.mapHeight);
		return true;
	}

	bool CalculateTerrainDrawSizeForCameraDirection(const TerrainDrawSizingInput &input,
		float forwardX, float forwardY, int &width, int &height)
	{
		// Retain the conservative result for invalid inputs and near-horizon views.
		if (!CalculateTerrainDrawSize(input, width, height))
			return false;
		if (!(forwardX > -FLT_MAX && forwardX < FLT_MAX &&
			forwardY > -FLT_MAX && forwardY < FLT_MAX))
			return true;

		const float horizontalLength = (float)sqrt(forwardX * forwardX + forwardY * forwardY);
		const float sinPitch = (float)sin(input.pitchRadians);
		const float cosPitch = (float)cos(input.pitchRadians);
		const float halfVerticalTangent = (float)tan(input.verticalFovRadians * 0.5f);
		const float farDown = sinPitch - cosPitch * halfVerticalTangent;
		if (!(horizontalLength > 0.0001f && horizontalLength < FLT_MAX) || farDown <= 0.01f)
			return true;

		forwardX /= horizontalLength;
		forwardY /= horizontalLength;
		const float rightX = forwardY;
		const float rightY = -forwardX;
		const float halfHorizontalTangent = (float)tan(input.horizontalFovRadians * 0.5f);
		const float nearDown = sinPitch + cosPitch * halfVerticalTangent;
		const float forwardDistances[2] = {
			input.cameraHeight * (cosPitch - sinPitch * halfVerticalTangent) / nearDown,
			input.cameraHeight * (cosPitch + sinPitch * halfVerticalTangent) / farDown
		};
		const float halfWidths[2] = {
			input.cameraHeight * halfHorizontalTangent / nearDown,
			input.cameraHeight * halfHorizontalTangent / farDown
		};
		float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
		for (int end = 0; end < 2; ++end)
		{
			for (int side = -1; side <= 1; side += 2)
			{
				const float x = forwardDistances[end] * forwardX + side * halfWidths[end] * rightX;
				const float y = forwardDistances[end] * forwardY + side * halfWidths[end] * rightY;
				if (end == 0 && side == -1)
				{
					minX = maxX = x;
					minY = maxY = y;
				}
				else
				{
					if (x < minX) minX = x;
					if (x > maxX) maxX = x;
					if (y < minY) minY = y;
					if (y > maxY) maxY = y;
				}
			}
		}

		const float requiredWidth = (maxX - minX) / input.worldUnitsPerCell + input.tileLength;
		const float requiredHeight = (maxY - minY) / input.worldUnitsPerCell + input.tileLength;
		width = !(requiredWidth < (float)input.mapWidth) ? input.mapWidth :
			ClampDrawSize(requiredWidth, input.minimumWidth, input.tileLength, input.mapWidth);
		height = !(requiredHeight < (float)input.mapHeight) ? input.mapHeight :
			ClampDrawSize(requiredHeight, input.minimumHeight, input.tileLength, input.mapHeight);
		return true;
	}

	void StabilizeTerrainDrawSizeForMap(int currentWidth, int currentHeight,
		int mapWidth, int mapHeight, int &width, int &height)
	{
		const int square = width > height ? width : height;
		width = square > currentWidth ? square : currentWidth;
		height = square > currentHeight ? square : currentHeight;
		if (width > mapWidth) width = mapWidth;
		if (height > mapHeight) height = mapHeight;
	}
}
