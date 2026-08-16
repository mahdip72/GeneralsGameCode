#pragma once

#include "Lib/BaseTypeCore.h"

/*
 * These values intentionally match the legacy WW3DFormat ordinals.  The
 * kernel keeps them as explicit POD values so it does not need a WW3D/D3D
 * header or a live render object on the worker side.
 */
enum RadarOverlayFormatCode
{
	RADAR_OVERLAY_FORMAT_UNKNOWN = 0,
	RADAR_OVERLAY_FORMAT_A8R8G8B8 = 2,
	RADAR_OVERLAY_FORMAT_A4R4G4B4 = 7
};

/* The owner owns this immutable command array for the joined call. */
struct RadarObjectOverlayCommand
{
	Int x;
	Int y;
	unsigned packedColor;
};

/* The output pointer is owner-owned and is written only for assigned rows. */
struct RadarObjectOverlaySnapshot
{
	unsigned width;
	unsigned height;
	unsigned bytesPerPixel;
	unsigned formatCode;
	unsigned rowBytes;
	unsigned commandCount;
	unsigned commandCapacity;
	const RadarObjectOverlayCommand *commands;
	unsigned char *output;
};

/* The owner keeps shroud commands separate from object footprint commands. */
struct RadarShroudOverlayCommand
{
	Int minX;
	Int minY;
	Int maxX;
	Int maxY;
	unsigned packedColor;
};

struct RadarShroudOverlaySnapshot
{
	unsigned width;
	unsigned height;
	unsigned bytesPerPixel;
	unsigned formatCode;
	unsigned rowBytes;
	unsigned commandCount;
	unsigned commandCapacity;
	const RadarShroudOverlayCommand *commands;
	unsigned char *output;
};

/* Returns the byte width for a supported format, or zero when unsupported. */
unsigned RadarOverlayBytesPerPixel(unsigned formatCode);

/* Apply ordered object footprints to rows in [rowBegin, rowEnd). */
bool PackRadarObjectRows(const RadarObjectOverlaySnapshot &snapshot,
	unsigned rowBegin, unsigned rowEnd);

/* Apply ordered inclusive shroud rectangles to rows in [rowBegin, rowEnd). */
bool PackRadarShroudRows(const RadarShroudOverlaySnapshot &snapshot,
	unsigned rowBegin, unsigned rowEnd);
