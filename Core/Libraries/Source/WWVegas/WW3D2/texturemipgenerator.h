/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include <Utility/CppMacros.h>
#include "ww3dformat.h"

struct IDirect3DTexture8;

// Generate one legacy box-filter-compatible level from a locked texture level.
// The source and destination pitches are in bytes. Only the format's active
// pixels are written; row padding remains untouched. Each component uses the
// The legacy box conversion's round-to-nearest average is (sum + 2) / 4. The
// unused X8R8G8B8 high byte is averaged as a byte (it is not forced opaque),
// and A1R5G5B5's one-bit alpha rounds on at least two opaque samples.
bool Generate_Texture_Mip_Level_Box(
	const unsigned char* source,
	unsigned source_pitch,
	unsigned source_width,
	unsigned source_height,
	unsigned char* destination,
	unsigned destination_pitch,
	WW3DFormat format);

// Generate all remaining levels of a D3D8 texture from its level zero data.
// Characterized formats use the CPU box helper; unsupported formats/layouts
// return D3DERR_NOTAVAILABLE instead of silently invoking an external image
// service. The returned value is a D3D HRESULT represented without requiring
// D3D8 in callers that only use the CPU filter.
unsigned Generate_DX8_Texture_Mip_Levels(IDirect3DTexture8* texture);
