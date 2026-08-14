/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "texturemipbuffer.h"
#include <string.h>

static bool checkedMultiply(size_t left, size_t right, size_t& result)
{
	if (left != 0 && right > (size_t)-1 / left)
	{
		return false;
	}

	result = left * right;
	return true;
}

static unsigned bytesPerPixel(WW3DFormat format)
{
	switch (format)
	{
		case WW3D_FORMAT_R8G8B8:
			return 3;

		case WW3D_FORMAT_A8R8G8B8:
		case WW3D_FORMAT_X8R8G8B8:
		case WW3D_FORMAT_X8L8V8U8:
			return 4;

		case WW3D_FORMAT_R5G6B5:
		case WW3D_FORMAT_X1R5G5B5:
		case WW3D_FORMAT_A1R5G5B5:
		case WW3D_FORMAT_A4R4G4B4:
		case WW3D_FORMAT_A8R3G3B2:
		case WW3D_FORMAT_X4R4G4B4:
		case WW3D_FORMAT_A8P8:
		case WW3D_FORMAT_A8L8:
		case WW3D_FORMAT_U8V8:
		case WW3D_FORMAT_L6V5U5:
			return 2;

		case WW3D_FORMAT_R3G3B2:
		case WW3D_FORMAT_A8:
		case WW3D_FORMAT_P8:
		case WW3D_FORMAT_L8:
		case WW3D_FORMAT_A4L4:
			return 1;

		default:
			return 0;
	}
}

bool CalculateTextureMipLayout(WW3DFormat format, unsigned width, unsigned height,
	unsigned depth, TextureMipLayout& layout)
{
	size_t rowPitch;
	size_t rowCount;
	size_t slicePitch;
	size_t dataSize;

	if (width == 0 || height == 0 || depth == 0)
	{
		return false;
	}

	if (format >= WW3D_FORMAT_DXT1 && format <= WW3D_FORMAT_DXT5)
	{
		const size_t blockSize = format == WW3D_FORMAT_DXT1 ? 8 : 16;
		const size_t blockWidth = ((size_t)width + 3) / 4;

		rowCount = ((size_t)height + 3) / 4;
		if (!checkedMultiply(blockWidth, blockSize, rowPitch))
		{
			return false;
		}
	}
	else
	{
		const unsigned pixelSize = bytesPerPixel(format);
		if (pixelSize == 0 || !checkedMultiply((size_t)width, pixelSize, rowPitch))
		{
			return false;
		}
		rowCount = height;
	}

	if (!checkedMultiply(rowPitch, rowCount, slicePitch) ||
		!checkedMultiply(slicePitch, depth, dataSize))
	{
		return false;
	}

	layout.rowPitch = rowPitch;
	layout.rowCount = rowCount;
	layout.slicePitch = slicePitch;
	layout.dataSize = dataSize;
	return true;
}

bool CopyTextureMipData(const unsigned char* source, const TextureMipLayout& sourceLayout,
	unsigned char* destination, const TextureMipLayout& destinationLayout, unsigned depth)
{
	size_t minimumSourceSlicePitch;
	size_t minimumDestinationSlicePitch;
	size_t requiredSourceSize;
	size_t requiredDestinationSize;
	unsigned slice;

	if (source == 0 || destination == 0 || depth == 0 || sourceLayout.rowPitch == 0 ||
		sourceLayout.rowCount == 0 ||
		!checkedMultiply(sourceLayout.rowPitch, sourceLayout.rowCount, minimumSourceSlicePitch) ||
		!checkedMultiply(destinationLayout.rowPitch, sourceLayout.rowCount, minimumDestinationSlicePitch) ||
		sourceLayout.slicePitch < minimumSourceSlicePitch ||
		destinationLayout.rowPitch < sourceLayout.rowPitch ||
		destinationLayout.rowCount < sourceLayout.rowCount ||
		destinationLayout.slicePitch < minimumDestinationSlicePitch ||
		!checkedMultiply(sourceLayout.slicePitch, depth, requiredSourceSize) ||
		!checkedMultiply(destinationLayout.slicePitch, depth, requiredDestinationSize) ||
		sourceLayout.dataSize < requiredSourceSize || destinationLayout.dataSize < requiredDestinationSize)
	{
		return false;
	}

	for (slice = 0; slice < depth; ++slice)
	{
		const unsigned char* sourceRow = source + sourceLayout.slicePitch * slice;
		unsigned char* destinationRow = destination + destinationLayout.slicePitch * slice;
		size_t row;

		for (row = 0; row < sourceLayout.rowCount; ++row)
		{
			memcpy(destinationRow, sourceRow, sourceLayout.rowPitch);
			sourceRow += sourceLayout.rowPitch;
			destinationRow += destinationLayout.rowPitch;
		}
	}

	return true;
}

TextureMipBuffer::TextureMipBuffer()
	: m_data(0)
{
	m_layout.rowPitch = 0;
	m_layout.rowCount = 0;
	m_layout.slicePitch = 0;
	m_layout.dataSize = 0;
}

TextureMipBuffer::~TextureMipBuffer()
{
	reset();
}

bool TextureMipBuffer::allocate(WW3DFormat format, unsigned width, unsigned height, unsigned depth)
{
	TextureMipLayout layout;
	unsigned char* data;

	if (!CalculateTextureMipLayout(format, width, height, depth, layout))
	{
		return false;
	}

	try
	{
		data = new unsigned char[layout.dataSize];
	}
	catch (...)
	{
		data = 0;
	}

	if (data == 0)
	{
		return false;
	}

	reset();
	m_data = data;
	m_layout = layout;
	return true;
}

bool TextureMipBuffer::copyFrom(const unsigned char* source,
	const TextureMipLayout& sourceLayout, unsigned depth)
{
	return CopyTextureMipData(source, sourceLayout, m_data, m_layout, depth);
}

void TextureMipBuffer::reset()
{
	delete[] m_data;
	m_data = 0;
	m_layout.rowPitch = 0;
	m_layout.rowCount = 0;
	m_layout.slicePitch = 0;
	m_layout.dataSize = 0;
}
