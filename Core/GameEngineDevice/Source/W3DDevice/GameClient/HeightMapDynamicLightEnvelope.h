#pragma once

#include "Lib/HeightMapDynamicLightKernel.h"

// Coordinates and bounds are map cells, with inclusive light/cell overlap.
struct HeightMapDynamicLightBounds
{
	int minX, minY, maxX, maxY;
	int prevMinX, prevMinY, prevMaxX, prevMaxY;
};

struct HeightMapDynamicLightEnvelope
{
	int x0, y0, x1, y1;
};

inline bool HeightMapDynamicLightAxisHit(int minimum, int maximum, int cell)
{
	return minimum <= cell + 1 && maximum >= cell;
}

inline bool HeightMapDynamicLightCellHit(
	const HeightMapDynamicLightBounds &light, int x, int y)
{
	return (HeightMapDynamicLightAxisHit(light.minX, light.maxX, x) &&
		HeightMapDynamicLightAxisHit(light.minY, light.maxY, y)) ||
		(HeightMapDynamicLightAxisHit(light.prevMinX, light.prevMaxX, x) &&
		HeightMapDynamicLightAxisHit(light.prevMinY, light.prevMaxY, y));
}

// Only current bounds determine whether a point/spot light can contribute to
// this tile. Previous bounds remain in HeightMapDynamicLightCellHit above so
// cells from a moved or disabled light are still rewritten.
inline bool HeightMapDynamicLightCurrentBoundsHitTile(
	const HeightMapDynamicLightBounds &light, const int *xCoords,
	unsigned width, const int *yCoords, unsigned height)
{
	bool hitX = false;
	bool hitY = false;
	if (xCoords == 0 || yCoords == 0 || width == 0 || height == 0)
		return false;
	for (unsigned x = 0; x < width; ++x)
		if (HeightMapDynamicLightAxisHit(light.minX, light.maxX, xCoords[x]))
		{
			hitX = true;
			break;
		}
	if (!hitX)
		return false;
	for (unsigned y = 0; y < height; ++y)
		if (HeightMapDynamicLightAxisHit(light.minY, light.maxY, yCoords[y]))
		{
			hitY = true;
			break;
		}
	return hitY;
}

// Return an order-preserving contributor subsequence. Directional lights
// affect every rewritten cell regardless of their own bounds. Unknown types
// are retained so the normal capture validator can select the legacy
// fallback instead of silently changing that contract.
inline bool HeightMapSelectDynamicLightContributors(
	const HeightMapDynamicLightSceneLight *lights,
	const HeightMapDynamicLightBounds *bounds, unsigned lightCount,
	const int *xCoords, unsigned width, const int *yCoords, unsigned height,
	HeightMapDynamicLightSceneLight *selected, unsigned *selectedCount)
{
	if (selectedCount == 0 || (lightCount != 0 &&
		(lights == 0 || bounds == 0 || selected == 0)) ||
		lightCount > HEIGHTMAP_DYNAMIC_LIGHT_MAX_LIGHTS ||
		xCoords == 0 || yCoords == 0 || width == 0 || height == 0)
		return false;
	*selectedCount = 0;
	for (unsigned index = 0; index < lightCount; ++index)
	{
		const HeightMapDynamicLightSceneLight &light = lights[index];
		const bool keep = light.type == HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL ||
			(light.type != HEIGHTMAP_DYNAMIC_LIGHT_POINT &&
			 light.type != HEIGHTMAP_DYNAMIC_LIGHT_SPOT) ||
			(light.enabled && HeightMapDynamicLightCurrentBoundsHitTile(
				bounds[index], xCoords, width, yCoords, height));
		if (keep)
			selected[(*selectedCount)++] = light;
	}
	return true;
}

// xCoords/yCoords are the exact wrapped cell coordinates for this VB tile.
inline bool HeightMapFindDynamicLightEnvelope(
	const int *xCoords, int width, const int *yCoords, int height,
	const HeightMapDynamicLightBounds *lights, int lightCount,
	HeightMapDynamicLightEnvelope &envelope)
{
	envelope.x0 = width;
	envelope.y0 = height;
	envelope.x1 = 0;
	envelope.y1 = 0;
	for (int lightIndex = 0; lightIndex < lightCount; ++lightIndex)
	{
		const HeightMapDynamicLightBounds &light = lights[lightIndex];
		for (int previous = 0; previous < 2; ++previous)
		{
			const int minX = previous ? light.prevMinX : light.minX;
			const int maxX = previous ? light.prevMaxX : light.maxX;
			const int minY = previous ? light.prevMinY : light.minY;
			const int maxY = previous ? light.prevMaxY : light.maxY;
			int firstX = width, lastX = -1;
			int firstY = height, lastY = -1;
			for (int x = 0; x < width; ++x)
				if (HeightMapDynamicLightAxisHit(minX, maxX, xCoords[x]))
				{
					if (firstX == width) firstX = x;
					lastX = x;
				}
			for (int y = 0; y < height; ++y)
				if (HeightMapDynamicLightAxisHit(minY, maxY, yCoords[y]))
				{
					if (firstY == height) firstY = y;
					lastY = y;
				}
			if (lastX < 0 || lastY < 0) continue;
			if (firstX < envelope.x0) envelope.x0 = firstX;
			if (firstY < envelope.y0) envelope.y0 = firstY;
			if (lastX + 1 > envelope.x1) envelope.x1 = lastX + 1;
			if (lastY + 1 > envelope.y1) envelope.y1 = lastY + 1;
			if (envelope.x0 == 0 && envelope.y0 == 0 &&
				envelope.x1 == width && envelope.y1 == height)
				return true;
		}
	}
	return envelope.x1 > envelope.x0 && envelope.y1 > envelope.y0;
}
