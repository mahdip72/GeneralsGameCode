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

#include <d3d8.h>

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

// Copy a source rectangle into a destination rectangle.  Equal-format,
// equal-sized rectangles use IDirect3DDevice8::CopyRects.  Other characterized
// combinations are staged and converted/scaled on the CPU.
HRESULT SurfaceBlit_Copy(
	IDirect3DSurface8 *destination,
	const RECT *destination_rect,
	IDirect3DSurface8 *source,
	const RECT *source_rect,
	SurfaceBlitFilter filter);

// Convert a locked D3D8 surface into tightly packed A8R8G8B8 bytes.  This
// covers the production uncompressed formats and DXT1-5 block layouts.  A
// format without a documented color interpretation is rejected.
bool SurfaceBlit_Convert_To_A8R8G8B8(
	const unsigned char *source,
	int source_pitch,
	unsigned int width,
	unsigned int height,
	D3DFORMAT source_format,
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

// Encode tightly packed A8R8G8B8 pixels into a lockable D3D8 surface layout.
// Paletted, bump-map, depth, and compressed destinations are rejected.
bool SurfaceBlit_Write_A8R8G8B8(
	const unsigned char *source,
	unsigned int width,
	unsigned int height,
	unsigned char *destination,
	int destination_pitch,
	D3DFORMAT destination_format);

// Copy and convert a surface to tightly packed A8R8G8B8 bytes.  The source is
// first copied to a lockable system-memory surface with CopyRects, so this is
// valid for both managed/system-memory and default-pool render-target sources.
// Unsupported formats return D3DERR_NOTAVAILABLE.
HRESULT SurfaceBlit_Copy_Surface_To_A8R8G8B8(
	IDirect3DSurface8 *source,
	unsigned int width,
	unsigned int height,
	std::vector<unsigned char> *pixels);

// Pure geometry predicate used by tests and by SurfaceBlit_Copy.  It records
// the exact CopyRects acceptance rule without creating a D3D device.
bool SurfaceBlit_Can_Use_CopyRects(
	const D3DSURFACE_DESC &destination,
	const RECT &destination_rect,
	const D3DSURFACE_DESC &source,
	const RECT &source_rect,
	SurfaceBlitFilter filter);

// Select the full-surface operation used by the legacy texture-from-surface
// path.  Exact format/size matches are byte copies; all other cases retain
// the characterized BOX conversion boundary.
SurfaceBlitFilter SurfaceBlit_Filter_For_Full_Copy(
	const D3DSURFACE_DESC &destination,
	const D3DSURFACE_DESC &source);
