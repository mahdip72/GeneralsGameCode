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

unsigned CalculateTextureMipLevelCount(unsigned width, unsigned height);
void ReduceTextureMipDimensions(unsigned& width, unsigned& height);

bool CopyTextureMipData(const unsigned char* source, const TextureMipLayout& sourceLayout,
	unsigned char* destination, const TextureMipLayout& destinationLayout, unsigned depth);

class TextureMipBuffer
{
public:
	TextureMipBuffer();
	~TextureMipBuffer();

	bool allocate(WW3DFormat format, unsigned width, unsigned height, unsigned depth);
	bool copyFrom(const unsigned char* source, const TextureMipLayout& sourceLayout, unsigned depth);
	void reset();

	unsigned char* data() { return m_data; }
	const unsigned char* data() const { return m_data; }
	const TextureMipLayout& layout() const { return m_layout; }

private:
	TextureMipBuffer(const TextureMipBuffer&);
	TextureMipBuffer& operator=(const TextureMipBuffer&);

	unsigned char* m_data;
	TextureMipLayout m_layout;
};
