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
		const TerrainCameraBasis &basis, int &width, int &height)
	{
		// Retain the conservative result for invalid inputs.
		if (!CalculateTerrainDrawSize(input, width, height))
			return false;
		const float vectors[9] = {
			basis.forwardX, basis.forwardY, basis.forwardZ,
			basis.rightX, basis.rightY, basis.rightZ,
			basis.upX, basis.upY, basis.upZ
		};
		for (int i = 0; i < 9; ++i)
		{
			if (!(vectors[i] > -FLT_MAX && vectors[i] < FLT_MAX))
			{
				width = input.mapWidth;
				height = input.mapHeight;
				return true;
			}
		}

		const float halfHorizontalTangent = (float)tan(input.horizontalFovRadians * 0.5f);
		const float halfVerticalTangent = (float)tan(input.verticalFovRadians * 0.5f);
		if (!(halfHorizontalTangent < FLT_MAX && halfVerticalTangent < FLT_MAX))
		{
			width = input.mapWidth;
			height = input.mapHeight;
			return true;
		}
		float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
		for (int vertical = -1; vertical <= 1; vertical += 2)
		{
			for (int horizontal = -1; horizontal <= 1; horizontal += 2)
			{
				const float x = basis.forwardX + horizontal * halfHorizontalTangent * basis.rightX +
					vertical * halfVerticalTangent * basis.upX;
				const float y = basis.forwardY + horizontal * halfHorizontalTangent * basis.rightY +
					vertical * halfVerticalTangent * basis.upY;
				const float z = basis.forwardZ + horizontal * halfHorizontalTangent * basis.rightZ +
					vertical * halfVerticalTangent * basis.upZ;
				if (!(z < -0.01f && x > -FLT_MAX && x < FLT_MAX &&
					y > -FLT_MAX && y < FLT_MAX))
				{
					width = input.mapWidth;
					height = input.mapHeight;
					return true;
				}
				const float groundX = input.cameraHeight * x / -z;
				const float groundY = input.cameraHeight * y / -z;
				if (vertical == -1 && horizontal == -1)
				{
					minX = maxX = groundX;
					minY = maxY = groundY;
				}
				else
				{
					if (groundX < minX) minX = groundX;
					if (groundX > maxX) maxX = groundX;
					if (groundY < minY) minY = groundY;
					if (groundY > maxY) maxY = groundY;
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
		// Full-map draws can precede shell sizing, or be temporary for a low
		// camera pitch. Do not permanently retain either as the shell floor.
		width = currentWidth < mapWidth && currentWidth > square ? currentWidth : square;
		height = currentHeight < mapHeight && currentHeight > square ? currentHeight : square;
		if (width > mapWidth) width = mapWidth;
		if (height > mapHeight) height = mapHeight;
	}
}
