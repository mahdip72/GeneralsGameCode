/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	at your option any later version.
*/

#pragma once

// Legacy renderer declarations; native code includes the neutral header.

#include <Utility/CppMacros.h>

#include "ww3dformat.h"
#include <vector>

// Keep the legacy filter values independent of any image service.  Native
// CopyRects handles exact copies; the remaining characterized uncompressed
// and block-compressed paths use the CPU conversion below.
enum SurfaceBlitFilter
{
	SURFACE_BLIT_FILTER_NONE = 0,
	SURFACE_BLIT_FILTER_TRIANGLE = 1,
	SURFACE_BLIT_FILTER_BOX = 2
};

struct SurfaceBlitRectangle
{
	int left;
	int top;
	int right;
	int bottom;
};

struct SurfaceBlitImageDescription
{
	unsigned int width;
	unsigned int height;
	WW3DFormat format;
};

// Backend-neutral geometry contract shared by native byte-image users and the
// legacy adapter below.
bool SurfaceBlit_Can_Copy_Direct(
	const SurfaceBlitImageDescription &destination,
	const SurfaceBlitRectangle &destination_rect,
	const SurfaceBlitImageDescription &source,
	const SurfaceBlitRectangle &source_rect,
	SurfaceBlitFilter filter);

SurfaceBlitFilter SurfaceBlit_Filter_For_Full_Copy(
	const SurfaceBlitImageDescription &destination,
	const SurfaceBlitImageDescription &source);

// Convert a pitched byte image into tightly packed A8R8G8B8 bytes.  This
// covers the production uncompressed formats and DXT1-5 block layouts.  A
// format without a documented color interpretation is rejected.
bool SurfaceBlit_Convert_To_A8R8G8B8(
	const unsigned char *source,
	int source_pitch,
	unsigned int width,
	unsigned int height,
	WW3DFormat source_format,
	std::vector<unsigned char> *pixels);

// Resample tightly packed A8R8G8B8 pixels for the characterized filter modes.
// NONE uses point selection; TRIANGLE uses bilinear interpolation; BOX uses
// an area average for reductions and bilinear interpolation otherwise.
bool SurfaceBlit_Resample_A8R8G8B8(
	const unsigned char *source,
	unsigned int source_width,
	unsigned int source_height,
	unsigned char *destination,
	unsigned int destination_width,
	unsigned int destination_height,
	SurfaceBlitFilter filter);

// Encode tightly packed A8R8G8B8 pixels into a pitched byte-image layout.
// Paletted, bump-map, depth, and compressed destinations are rejected.
bool SurfaceBlit_Write_A8R8G8B8(
	const unsigned char *source,
	unsigned int width,
	unsigned int height,
	unsigned char *destination,
	int destination_pitch,
	WW3DFormat destination_format);
