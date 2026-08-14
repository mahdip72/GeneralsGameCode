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

#include "ww3dformat.h"
#include <stddef.h>

struct TextureMipLayout
{
	size_t rowPitch;
	size_t rowCount;
	size_t slicePitch;
	size_t dataSize;
};

bool CalculateTextureMipLayout(WW3DFormat format, unsigned width, unsigned height,
	unsigned depth, TextureMipLayout& layout);

bool CopyTextureMipData(const unsigned char* source, const TextureMipLayout& sourceLayout,
	unsigned char* destination, const TextureMipLayout& destinationLayout, unsigned depth);
