#pragma once

#include "Lib/BaseTypeCore.h"

/*
 * These values intentionally match the legacy WW3DFormat enum.  Keeping the
 * numbers here lets the worker kernel pack bytes without including WW3D/D3D
 * headers or carrying a live engine object across the owner boundary.
 */
enum RadarTerrainFormatCode
{
	RADAR_TERRAIN_FORMAT_UNKNOWN = 0,
	RADAR_TERRAIN_FORMAT_R8G8B8,
	RADAR_TERRAIN_FORMAT_A8R8G8B8,
	RADAR_TERRAIN_FORMAT_X8R8G8B8,
	RADAR_TERRAIN_FORMAT_R5G6B5,
	RADAR_TERRAIN_FORMAT_X1R5G5B5,
	RADAR_TERRAIN_FORMAT_A1R5G5B5,
	RADAR_TERRAIN_FORMAT_A4R4G4B4,
	RADAR_TERRAIN_FORMAT_R3G3B2,
	RADAR_TERRAIN_FORMAT_A8,
	RADAR_TERRAIN_FORMAT_A8R3G3B2,
	RADAR_TERRAIN_FORMAT_X4R4G4B4,
	RADAR_TERRAIN_FORMAT_A8P8,
	RADAR_TERRAIN_FORMAT_P8,
	RADAR_TERRAIN_FORMAT_L8,
	RADAR_TERRAIN_FORMAT_A8L8,
	RADAR_TERRAIN_FORMAT_A4L4,
	RADAR_TERRAIN_FORMAT_U8V8,
	RADAR_TERRAIN_FORMAT_L6V5U5,
	RADAR_TERRAIN_FORMAT_X8L8V8U8,
	RADAR_TERRAIN_FORMAT_DXT1,
	RADAR_TERRAIN_FORMAT_DXT2,
	RADAR_TERRAIN_FORMAT_DXT3,
	RADAR_TERRAIN_FORMAT_DXT4,
	RADAR_TERRAIN_FORMAT_DXT5,
	RADAR_TERRAIN_FORMAT_COUNT
};

struct RadarTerrainRgb
{
	Real red;
	Real green;
	Real blue;
};

/*
 * One copied coordinate of an owner-built snapshot.  The caller owns the
 * array containing these PODs; the kernel borrows it only while a call is in
 * progress.  No field is a live terrain, bridge, render, or engine pointer.
 */
struct RadarTerrainCellInput
{
	Real worldX;
	Real worldY;
	Real groundZ;
	unsigned char centerUnderwater;
	Real centerWaterSurfaceZ;
	Real centerWaterBottomZ;
	unsigned char neighborUnderwater;
	Real neighborWaterSurfaceZ;
	Real neighborWaterBottomZ;
	unsigned char workingBridge;
	RadarTerrainRgb terrainColor;
	RadarTerrainRgb bridgeColor;
	Real bridgeHeight;
};

/*
 * Immutable scalar/map data for one raster.  The caller owns both cells and
 * the output byte buffer, and the kernel borrows them only during the call.
 * rowBytes is the output stride; production uses width * bytesPerPixel, while
 * focused tests may use a larger stride to place guard bytes around rows.
 */
struct RadarTerrainSnapshot
{
	unsigned width;
	unsigned height;
	unsigned bytesPerPixel;
	unsigned formatCode;
	unsigned rowBytes;
	Real terrainAverageZ;
	Real mapHighZ;
	Real mapLowZ;
	RadarTerrainRgb waterColor;
	const RadarTerrainCellInput *cells;
};

unsigned RadarTerrainBytesPerPixel(unsigned formatCode);

/* Preserve the legacy height argument order: height, hiZ, midZ, loZ. */
void InterpolateRadarColorForHeight(RadarTerrainRgb *color,
	Real height, Real hiZ, Real midZ, Real loZ);

/*
 * Shade one already-clipped 3x3 neighborhood.  The caller owns neighbors and
 * pixelBytes; this helper writes exactly bytesPerPixel bytes when its inputs
 * are valid and performs no allocation, synchronization, or global access.
 */
void ShadeRadarPixel(const RadarTerrainSnapshot &snapshot,
	unsigned x, unsigned y,
	const RadarTerrainCellInput *neighbors[3][3],
	unsigned char *pixelBytes);

/* Shade [yBegin, yEnd), leaving all rows outside that range untouched. */
bool ShadeRadarRows(const RadarTerrainSnapshot &snapshot,
	unsigned char *output, unsigned yBegin, unsigned yEnd);
