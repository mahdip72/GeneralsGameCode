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

// Legacy renderer declarations; native code includes the neutral header.

#include <Utility/CppMacros.h>
#include "ww3dformat.h"

// Generate one legacy box-filter-compatible level from a pitched byte image.
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
