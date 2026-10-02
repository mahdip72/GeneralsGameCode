/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "surfaceblit.h"

#include <math.h>
#include <limits.h>
#include <limits>
#include <new>
#include <string.h>

namespace
{
const unsigned int SURFACE_BLIT_MAX_BYTES = 256U * 1024U * 1024U;

static bool Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right >
		(std::numeric_limits<size_t>::max)() / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool Checked_A8R8G8B8_Byte_Size(unsigned int width,
	unsigned int height, size_t *result)
{
	size_t pixel_count;
	return Checked_Multiply((size_t)width, (size_t)height, &pixel_count) &&
		Checked_Multiply(pixel_count, 4U, result) &&
		*result <= SURFACE_BLIT_MAX_BYTES;
}

static bool Is_Dxt(WW3DFormat format)
{
	return format == WW3D_FORMAT_DXT1 || format == WW3D_FORMAT_DXT2 ||
		format == WW3D_FORMAT_DXT3 || format == WW3D_FORMAT_DXT4 ||
		format == WW3D_FORMAT_DXT5;
}

static unsigned Dxt_Block_Bytes(WW3DFormat format)
{
	return format == WW3D_FORMAT_DXT1 ? 8U : (Is_Dxt(format) ? 16U : 0U);
}

static bool Bytes_Per_Pixel(WW3DFormat format, unsigned int *bytes)
{
	if (bytes == 0)
	{
		return false;
	}
	switch (format)
	{
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
		*bytes = 4U;
		return true;
	case WW3D_FORMAT_R8G8B8:
		*bytes = 3U;
		return true;
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_X1R5G5B5:
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_X4R4G4B4:
	case WW3D_FORMAT_R5G6B5:
	case WW3D_FORMAT_A8R3G3B2:
	case WW3D_FORMAT_A8L8:
		*bytes = 2U;
		return true;
	case WW3D_FORMAT_R3G3B2:
	case WW3D_FORMAT_A8:
	case WW3D_FORMAT_L8:
	case WW3D_FORMAT_A4L4:
		*bytes = 1U;
		return true;
	default:
		return false;
	}
}

static bool Is_Cpu_Color_Format(WW3DFormat format)
{
	unsigned bytes;
	return Bytes_Per_Pixel(format, &bytes) || Is_Dxt(format);
}

static unsigned short Read_Word(const unsigned char *source)
{
	return (unsigned short)(source[0] | ((unsigned short)source[1] << 8));
}

static void Write_Word(unsigned char *destination, unsigned short value)
{
	destination[0] = (unsigned char)(value & 0xff);
	destination[1] = (unsigned char)(value >> 8);
}

static unsigned Clamp_Byte(float value)
{
	if (value <= 0.0f) return 0U;
	if (value >= 255.0f) return 255U;
	return (unsigned)(value + 0.5f);
}

static unsigned Expand_5(unsigned value)
{
	return (value << 3) | (value >> 2);
}

static unsigned Expand_6(unsigned value)
{
	return (value << 2) | (value >> 4);
}

static unsigned Expand_4(unsigned value)
{
	return (value << 4) | value;
}

static unsigned Expand_3(unsigned value)
{
	return (value << 5) | (value << 2) | (value >> 1);
}

static unsigned Expand_2(unsigned value)
{
	return (value << 6) | (value << 4) | (value << 2) | value;
}

static void Read_565(unsigned short value, unsigned char *pixel)
{
	pixel[0] = (unsigned char)Expand_5(value & 0x1f);
	pixel[1] = (unsigned char)Expand_6((value >> 5) & 0x3f);
	pixel[2] = (unsigned char)Expand_5((value >> 11) & 0x1f);
	pixel[3] = 0xff;
}

static void Read_Dxt_Color(unsigned index, unsigned short color0,
	unsigned short color1, bool force_four_color, unsigned char *pixel)
{
	unsigned char first[4];
	unsigned char second[4];
	Read_565(color0, first);
	Read_565(color1, second);
	if (index == 0)
	{
		memcpy(pixel, first, 4);
	}
	else if (index == 1)
	{
		memcpy(pixel, second, 4);
	}
	else if (index == 2)
	{
		pixel[0] = (unsigned char)((2U * first[0] + second[0] + 1U) / 3U);
		pixel[1] = (unsigned char)((2U * first[1] + second[1] + 1U) / 3U);
		pixel[2] = (unsigned char)((2U * first[2] + second[2] + 1U) / 3U);
		pixel[3] = 0xff;
	}
	else if (color0 > color1 || force_four_color)
	{
		pixel[0] = (unsigned char)((first[0] + 2U * second[0] + 1U) / 3U);
		pixel[1] = (unsigned char)((first[1] + 2U * second[1] + 1U) / 3U);
		pixel[2] = (unsigned char)((first[2] + 2U * second[2] + 1U) / 3U);
		pixel[3] = 0xff;
	}
	else
	{
		pixel[0] = pixel[1] = pixel[2] = 0;
		pixel[3] = 0;
	}
}

static unsigned Dxt_Alpha(const unsigned char *block, WW3DFormat format,
	unsigned pixelIndex)
{
	if (format == WW3D_FORMAT_DXT1)
	{
		return 255U;
	}
	if (format == WW3D_FORMAT_DXT2 || format == WW3D_FORMAT_DXT3)
	{
		const unsigned short alpha = Read_Word(block + (pixelIndex / 4U) * 2U);
		return (unsigned)((alpha >> ((pixelIndex & 3U) * 4U)) & 0xfU) * 17U;
	}

	const unsigned char alpha0 = block[0];
	const unsigned char alpha1 = block[1];
	unsigned char alphaTable[8];
	unsigned index;
	alphaTable[0] = alpha0;
	alphaTable[1] = alpha1;
	if (alpha0 > alpha1)
	{
		for (index = 1; index < 7; ++index)
		{
			alphaTable[index + 1] = (unsigned char)(
				((7U - index) * alpha0 + index * alpha1 + 3U) / 7U);
		}
	}
	else
	{
		for (index = 1; index < 5; ++index)
		{
			alphaTable[index + 1] = (unsigned char)(
				((5U - index) * alpha0 + index * alpha1 + 2U) / 5U);
		}
		alphaTable[6] = 0;
		alphaTable[7] = 255;
	}
	{
		const unsigned bitOffset = 3U * pixelIndex;
		const unsigned byteOffset = bitOffset / 8U;
		const unsigned shift = bitOffset & 7U;
		// A BC3 alpha-index payload is exactly six bytes.  Three index bits can
		// straddle at most two bytes; avoid reading into the following color
		// block for the final index in the payload.
		unsigned packed = (unsigned)block[2U + byteOffset];
		if (byteOffset + 1U < 6U)
		{
			packed |= (unsigned)block[2U + byteOffset + 1U] << 8;
		}
		return alphaTable[(packed >> shift) & 7U];
	}
}

static bool Read_Dxt_Pixel(const unsigned char *source, int source_pitch,
	unsigned width, unsigned height, WW3DFormat format, unsigned x, unsigned y,
	unsigned char *pixel)
{
	const unsigned blockBytes = Dxt_Block_Bytes(format);
	const unsigned blockWidth = (width + 3U) / 4U;
	const unsigned blockHeight = (height + 3U) / 4U;
	const unsigned blockX = x / 4U;
	const unsigned blockY = y / 4U;
	const unsigned localIndex = (y & 3U) * 4U + (x & 3U);
	const unsigned char *block;
	const unsigned char *colorBlock;
	unsigned long colorIndexes;
	unsigned short color0;
	unsigned short color1;
	if (source == 0 || pixel == 0 || blockBytes == 0 || blockX >= blockWidth ||
		blockY >= blockHeight || source_pitch < (int)(blockWidth * blockBytes))
	{
		return false;
	}
	block = source + (size_t)blockY * (size_t)source_pitch +
		(size_t)blockX * blockBytes;
	// BC2/BC3 (D3D8 DXT2-5) store alpha in the first eight bytes and the
	// BC1-compatible color payload in the second eight.  DXT1 consists only
	// of that color payload.
	colorBlock = format == WW3D_FORMAT_DXT1 ? block : block + 8;
	color0 = Read_Word(colorBlock);
	color1 = Read_Word(colorBlock + 2);
	colorIndexes = (unsigned long)colorBlock[4] |
		((unsigned long)colorBlock[5] << 8) |
		((unsigned long)colorBlock[6] << 16) |
		((unsigned long)colorBlock[7] << 24);
	Read_Dxt_Color((colorIndexes >> (2U * localIndex)) & 3U,
		color0, color1, format != WW3D_FORMAT_DXT1, pixel);
	if (format == WW3D_FORMAT_DXT1 && color0 <= color1 &&
		((colorIndexes >> (2U * localIndex)) & 3U) == 3U)
	{
		pixel[3] = 0;
	}
	else
	{
		pixel[3] = (unsigned char)Dxt_Alpha(block, format, localIndex);
	}
	return x < width && y < height;
}

static bool Read_Pixel(const unsigned char *source, int source_pitch,
	unsigned width, unsigned height, WW3DFormat format, unsigned x, unsigned y,
	unsigned char *pixel)
{
	unsigned bytes;
	const unsigned char *source_pixel;
	if (source == 0 || pixel == 0 || x >= width || y >= height)
	{
		return false;
	}
	if (Is_Dxt(format))
	{
		return Read_Dxt_Pixel(source, source_pitch, width, height, format,
			x, y, pixel);
	}
	if (!Bytes_Per_Pixel(format, &bytes) || source_pitch < (int)(width * bytes))
	{
		return false;
	}
	source_pixel = source + (size_t)y * (size_t)source_pitch +
		(size_t)x * bytes;
	switch (format)
	{
	case WW3D_FORMAT_A8R8G8B8:
		memcpy(pixel, source_pixel, 4);
		return true;
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R8G8B8:
		memcpy(pixel, source_pixel, 3);
		pixel[3] = 0xff;
		return true;
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_X1R5G5B5:
	{
		const unsigned short value = Read_Word(source_pixel);
		pixel[0] = (unsigned char)Expand_5(value & 0x1f);
		pixel[1] = (unsigned char)Expand_5((value >> 5) & 0x1f);
		pixel[2] = (unsigned char)Expand_5((value >> 10) & 0x1f);
		pixel[3] = format == WW3D_FORMAT_A1R5G5B5 && (value & 0x8000) ? 0xff :
			(format == WW3D_FORMAT_X1R5G5B5 ? 0xff : 0);
		return true;
	}
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_X4R4G4B4:
	{
		const unsigned short value = Read_Word(source_pixel);
		pixel[0] = (unsigned char)Expand_4(value & 0x000f);
		pixel[1] = (unsigned char)Expand_4((value >> 4) & 0x000f);
		pixel[2] = (unsigned char)Expand_4((value >> 8) & 0x000f);
		pixel[3] = format == WW3D_FORMAT_A4R4G4B4 ?
			(unsigned char)Expand_4((value >> 12) & 0x000f) : 0xff;
		return true;
	}
	case WW3D_FORMAT_R5G6B5:
		Read_565(Read_Word(source_pixel), pixel);
		return true;
	case WW3D_FORMAT_R3G3B2:
		pixel[0] = (unsigned char)Expand_2(source_pixel[0] & 3U);
		pixel[1] = (unsigned char)Expand_3((source_pixel[0] >> 2) & 7U);
		pixel[2] = (unsigned char)Expand_3((source_pixel[0] >> 5) & 7U);
		pixel[3] = 0xff;
		return true;
	case WW3D_FORMAT_A8R3G3B2:
		pixel[0] = (unsigned char)Expand_2(source_pixel[0] & 3U);
		pixel[1] = (unsigned char)Expand_3((source_pixel[0] >> 2) & 7U);
		pixel[2] = (unsigned char)Expand_3((source_pixel[0] >> 5) & 7U);
		pixel[3] = source_pixel[1];
		return true;
	case WW3D_FORMAT_L8:
		pixel[0] = pixel[1] = pixel[2] = source_pixel[0];
		pixel[3] = 0xff;
		return true;
	case WW3D_FORMAT_A8:
		pixel[0] = pixel[1] = pixel[2] = 0;
		pixel[3] = source_pixel[0];
		return true;
	case WW3D_FORMAT_A8L8:
		pixel[0] = pixel[1] = pixel[2] = source_pixel[0];
		pixel[3] = source_pixel[1];
		return true;
	case WW3D_FORMAT_A4L4:
		pixel[0] = pixel[1] = pixel[2] = (unsigned char)Expand_4(source_pixel[0] & 0xf);
		pixel[3] = (unsigned char)Expand_4(source_pixel[0] >> 4);
		return true;
	default:
		return false;
	}
}

static bool Write_Pixel(unsigned char *destination, WW3DFormat format,
	const unsigned char *pixel)
{
	if (destination == 0 || pixel == 0)
	{
		return false;
	}
	switch (format)
	{
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
		destination[0] = pixel[0];
		destination[1] = pixel[1];
		destination[2] = pixel[2];
		destination[3] = format == WW3D_FORMAT_X8R8G8B8 ? 0xff : pixel[3];
		return true;
	case WW3D_FORMAT_R8G8B8:
		memcpy(destination, pixel, 3);
		return true;
	case WW3D_FORMAT_A1R5G5B5:
	{
		const unsigned short value = (unsigned short)
			(((pixel[0] & 0xf8) >> 3) |
			 ((pixel[1] & 0xf8) << 2) |
			 ((pixel[2] & 0xf8) << 7) |
			 (pixel[3] >= 128 ? 0x8000 : 0));
		Write_Word(destination, value);
		return true;
	}
	case WW3D_FORMAT_X1R5G5B5:
	{
		const unsigned short value = (unsigned short)
			(0x8000 | ((pixel[0] & 0xf8) >> 3) |
			 ((pixel[1] & 0xf8) << 2) | ((pixel[2] & 0xf8) << 7));
		Write_Word(destination, value);
		return true;
	}
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_X4R4G4B4:
	{
		const unsigned short value = (unsigned short)
			(((pixel[0] & 0xf0) >> 4) |
			 (pixel[1] & 0xf0) | ((pixel[2] & 0xf0) << 4) |
			 (format == WW3D_FORMAT_A4R4G4B4 ? (pixel[3] & 0xf0) << 8 : 0xf000));
		Write_Word(destination, value);
		return true;
	}
	case WW3D_FORMAT_R5G6B5:
		Write_Word(destination, (unsigned short)(((pixel[0] & 0xf8) >> 3) |
			((pixel[1] & 0xfc) << 3) | ((pixel[2] & 0xf8) << 8)));
		return true;
	case WW3D_FORMAT_R3G3B2:
		destination[0] = (unsigned char)(((pixel[0] & 0xc0) >> 6) |
			((pixel[1] & 0xe0) >> 3) | (pixel[2] & 0xe0));
		return true;
	case WW3D_FORMAT_A8R3G3B2:
		destination[0] = (unsigned char)((pixel[2] & 0xe0) |
			((pixel[1] & 0xe0) >> 3) | ((pixel[0] & 0xc0) >> 6));
		destination[1] = pixel[3];
		return true;
	case WW3D_FORMAT_L8:
		destination[0] = (unsigned char)(((unsigned)pixel[0] * 0x1275U +
			(unsigned)pixel[1] * 0xb725U + (unsigned)pixel[2] * 0x3666U) >> 16);
		return true;
	case WW3D_FORMAT_A8:
		destination[0] = pixel[3];
		return true;
	case WW3D_FORMAT_A8L8:
		destination[0] = (unsigned char)(((unsigned)pixel[0] * 0x1275U +
			(unsigned)pixel[1] * 0xb725U + (unsigned)pixel[2] * 0x3666U) >> 16);
		destination[1] = pixel[3];
		return true;
	case WW3D_FORMAT_A4L4:
	{
		const unsigned luminance = ((unsigned)pixel[0] * 0x1275U +
			(unsigned)pixel[1] * 0xb725U + (unsigned)pixel[2] * 0x3666U) >> 16;
		destination[0] = (unsigned char)((pixel[3] & 0xf0) | (luminance >> 4));
		return true;
	}
	default:
		return false;
	}
}

static bool Rect_Is_Valid(const SurfaceBlitRectangle &rect,
	const SurfaceBlitImageDescription &description)
{
	return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left &&
		rect.bottom > rect.top && (unsigned int)rect.right <= description.width &&
		(unsigned int)rect.bottom <= description.height;
}

#if defined(BUILD_WITH_D3D8)

static bool Legacy_D3D8_Format_To_WW3D(D3DFORMAT format,
	WW3DFormat *neutral_format)
{
	if (neutral_format == 0) return false;
	switch (format)
	{
	case D3DFMT_R8G8B8: *neutral_format = WW3D_FORMAT_R8G8B8; return true;
	case D3DFMT_A8R8G8B8: *neutral_format = WW3D_FORMAT_A8R8G8B8; return true;
	case D3DFMT_X8R8G8B8: *neutral_format = WW3D_FORMAT_X8R8G8B8; return true;
	case D3DFMT_R5G6B5: *neutral_format = WW3D_FORMAT_R5G6B5; return true;
	case D3DFMT_X1R5G5B5: *neutral_format = WW3D_FORMAT_X1R5G5B5; return true;
	case D3DFMT_A1R5G5B5: *neutral_format = WW3D_FORMAT_A1R5G5B5; return true;
	case D3DFMT_A4R4G4B4: *neutral_format = WW3D_FORMAT_A4R4G4B4; return true;
	case D3DFMT_R3G3B2: *neutral_format = WW3D_FORMAT_R3G3B2; return true;
	case D3DFMT_A8: *neutral_format = WW3D_FORMAT_A8; return true;
	case D3DFMT_A8R3G3B2: *neutral_format = WW3D_FORMAT_A8R3G3B2; return true;
	case D3DFMT_X4R4G4B4: *neutral_format = WW3D_FORMAT_X4R4G4B4; return true;
	case D3DFMT_L8: *neutral_format = WW3D_FORMAT_L8; return true;
	case D3DFMT_A8L8: *neutral_format = WW3D_FORMAT_A8L8; return true;
	case D3DFMT_A4L4: *neutral_format = WW3D_FORMAT_A4L4; return true;
	case D3DFMT_DXT1: *neutral_format = WW3D_FORMAT_DXT1; return true;
	case D3DFMT_DXT2: *neutral_format = WW3D_FORMAT_DXT2; return true;
	case D3DFMT_DXT3: *neutral_format = WW3D_FORMAT_DXT3; return true;
	case D3DFMT_DXT4: *neutral_format = WW3D_FORMAT_DXT4; return true;
	case D3DFMT_DXT5: *neutral_format = WW3D_FORMAT_DXT5; return true;
	default: *neutral_format = WW3D_FORMAT_UNKNOWN; return false;
	}
}

static SurfaceBlitImageDescription Legacy_D3D8_Description(
	const D3DSURFACE_DESC &description, WW3DFormat format)
{
	SurfaceBlitImageDescription neutral;
	neutral.width = description.Width;
	neutral.height = description.Height;
	neutral.format = format;
	return neutral;
}

static SurfaceBlitRectangle Legacy_D3D8_Rect(const RECT &rect)
{
	SurfaceBlitRectangle neutral;
	neutral.left = rect.left;
	neutral.top = rect.top;
	neutral.right = rect.right;
	neutral.bottom = rect.bottom;
	return neutral;
}

static void Full_Rect(const D3DSURFACE_DESC &description, RECT *rect)
{
	rect->left = 0;
	rect->top = 0;
	rect->right = (LONG)description.Width;
	rect->bottom = (LONG)description.Height;
}

static HRESULT Create_Staging_Surface(IDirect3DDevice8 *device, unsigned width,
	unsigned height, D3DFORMAT format, IDirect3DSurface8 **surface,
	IDirect3DTexture8 **textureOwner)
{
	HRESULT result;
	if (surface == 0 || textureOwner == 0 || device == 0 || width == 0 ||
		height == 0) return D3DERR_INVALIDCALL;
	*surface = 0;
	*textureOwner = 0;
	result = device->CreateImageSurface(width, height, format, surface);
	WW3DFormat neutral_format;
	if (SUCCEEDED(result) || !Legacy_D3D8_Format_To_WW3D(format,
		&neutral_format) || !Is_Dxt(neutral_format)) return result;
	result = device->CreateTexture(width, height, 1, 0, format,
		D3DPOOL_SYSTEMMEM, textureOwner);
	if (SUCCEEDED(result)) result = (*textureOwner)->GetSurfaceLevel(0, surface);
	if (FAILED(result))
	{
		if (*surface != 0) { (*surface)->Release(); *surface = 0; }
		if (*textureOwner != 0) { (*textureOwner)->Release(); *textureOwner = 0; }
	}
	return result;
}

#endif

static bool Resample_Area(const unsigned char *source, unsigned source_width,
	unsigned source_height, unsigned char *destination, unsigned destination_width,
	unsigned destination_height)
{
	unsigned y;
	for (y = 0; y < destination_height; ++y)
	{
		const float top = (float)y * source_height / destination_height;
		const float bottom = (float)(y + 1U) * source_height / destination_height;
		const int firstY = (int)floor(top);
		const int lastY = (int)ceil(bottom) - 1;
		unsigned x;
		for (x = 0; x < destination_width; ++x)
		{
			const float left = (float)x * source_width / destination_width;
			const float right = (float)(x + 1U) * source_width / destination_width;
			const int firstX = (int)floor(left);
			const int lastX = (int)ceil(right) - 1;
			float sum[4] = { 0, 0, 0, 0 };
			float total = 0.0f;
			int sy;
			for (sy = firstY; sy <= lastY; ++sy)
			{
				const float wy = (float)((bottom < sy + 1 ? bottom : (float)(sy + 1)) -
					(top > sy ? top : (float)sy));
				int sx;
				if (wy <= 0.0f) continue;
				for (sx = firstX; sx <= lastX; ++sx)
				{
					const float wx = (float)((right < sx + 1 ? right : (float)(sx + 1)) -
						(left > sx ? left : (float)sx));
					const float weight = wx * wy;
					const unsigned char *pixel;
					unsigned component;
					if (wx <= 0.0f) continue;
					pixel = source + ((size_t)sy * source_width + (size_t)sx) * 4U;
					for (component = 0; component < 4; ++component)
						sum[component] += pixel[component] * weight;
					total += weight;
				}
			}
			if (total <= 0.0f) return false;
			for (unsigned component = 0; component < 4; ++component)
				destination[((size_t)y * destination_width + x) * 4U + component] =
					(unsigned char)Clamp_Byte(sum[component] / total);
		}
	}
	return true;
}

static bool Resample_Bilinear(const unsigned char *source, unsigned source_width,
	unsigned source_height, unsigned char *destination, unsigned destination_width,
	unsigned destination_height)
{
	for (unsigned y = 0; y < destination_height; ++y)
	{
		const float sourceY = ((float)y + 0.5f) * source_height /
			destination_height - 0.5f;
		const int y0 = sourceY <= 0.0f ? 0 : (int)floor(sourceY);
		const int y1 = y0 + 1 < (int)source_height ? y0 + 1 : y0;
		const float fy = sourceY <= 0.0f ? 0.0f :
			(sourceY >= source_height - 1 ? 1.0f : sourceY - floor(sourceY));
		for (unsigned x = 0; x < destination_width; ++x)
		{
			const float sourceX = ((float)x + 0.5f) * source_width /
				destination_width - 0.5f;
			const int x0 = sourceX <= 0.0f ? 0 : (int)floor(sourceX);
			const int x1 = x0 + 1 < (int)source_width ? x0 + 1 : x0;
			const float fx = sourceX <= 0.0f ? 0.0f :
				(sourceX >= source_width - 1 ? 1.0f : sourceX - floor(sourceX));
			const unsigned char *p00 = source + ((size_t)y0 * source_width + x0) * 4U;
			const unsigned char *p10 = source + ((size_t)y0 * source_width + x1) * 4U;
			const unsigned char *p01 = source + ((size_t)y1 * source_width + x0) * 4U;
			const unsigned char *p11 = source + ((size_t)y1 * source_width + x1) * 4U;
			unsigned char *out = destination + ((size_t)y * destination_width + x) * 4U;
			for (unsigned component = 0; component < 4; ++component)
			{
				const float top = p00[component] +
					(p10[component] - p00[component]) * fx;
				const float bottom = p01[component] +
					(p11[component] - p01[component]) * fx;
				out[component] = (unsigned char)Clamp_Byte(top + (bottom - top) * fy);
			}
		}
	}
	return true;
}
}

bool SurfaceBlit_Can_Copy_Direct(
	const SurfaceBlitImageDescription &destination,
	const SurfaceBlitRectangle &destination_rect,
	const SurfaceBlitImageDescription &source,
	const SurfaceBlitRectangle &source_rect,
	SurfaceBlitFilter filter)
{
	return filter == SURFACE_BLIT_FILTER_NONE &&
		destination.format == source.format &&
		Rect_Is_Valid(destination_rect, destination) &&
		Rect_Is_Valid(source_rect, source) &&
		destination_rect.right - destination_rect.left ==
			source_rect.right - source_rect.left &&
		destination_rect.bottom - destination_rect.top ==
			source_rect.bottom - source_rect.top;
}

SurfaceBlitFilter SurfaceBlit_Filter_For_Full_Copy(
	const SurfaceBlitImageDescription &destination,
	const SurfaceBlitImageDescription &source)
{
	return destination.format == source.format &&
		destination.width == source.width && destination.height == source.height ?
		SURFACE_BLIT_FILTER_NONE : SURFACE_BLIT_FILTER_BOX;
}

#if defined(BUILD_WITH_D3D8)

bool SurfaceBlit_Can_Use_CopyRects(
	const D3DSURFACE_DESC &destination,
	const RECT &destination_rect,
	const D3DSURFACE_DESC &source,
	const RECT &source_rect,
	SurfaceBlitFilter filter)
{
	WW3DFormat destination_format;
	WW3DFormat source_format;
	return Legacy_D3D8_Format_To_WW3D(destination.Format,
		&destination_format) &&
		Legacy_D3D8_Format_To_WW3D(source.Format, &source_format) &&
		SurfaceBlit_Can_Copy_Direct(
			Legacy_D3D8_Description(destination, destination_format),
			Legacy_D3D8_Rect(destination_rect),
			Legacy_D3D8_Description(source, source_format),
			Legacy_D3D8_Rect(source_rect), filter);
}

SurfaceBlitFilter SurfaceBlit_Filter_For_Full_Copy(
	const D3DSURFACE_DESC &destination,
	const D3DSURFACE_DESC &source)
{
	WW3DFormat destination_format;
	WW3DFormat source_format;
	if (!Legacy_D3D8_Format_To_WW3D(destination.Format,
		&destination_format) ||
		!Legacy_D3D8_Format_To_WW3D(source.Format, &source_format))
	{
		return SURFACE_BLIT_FILTER_BOX;
	}
	return SurfaceBlit_Filter_For_Full_Copy(
		Legacy_D3D8_Description(destination, destination_format),
		Legacy_D3D8_Description(source, source_format));
}

#endif

bool SurfaceBlit_Convert_To_A8R8G8B8(
	const unsigned char *source,
	int source_pitch,
	unsigned int width,
	unsigned int height,
	WW3DFormat source_format,
	std::vector<unsigned char> *pixels)
{
	size_t total_bytes;
	unsigned int y;
	if (source == 0 || pixels == 0 || width == 0 || height == 0 ||
		source_pitch <= 0 || !Is_Cpu_Color_Format(source_format) ||
		!Checked_Multiply((size_t)width, 4U, &total_bytes) ||
		!Checked_Multiply(total_bytes, height, &total_bytes) ||
		total_bytes > SURFACE_BLIT_MAX_BYTES)
	{
		return false;
	}
	if (!Is_Dxt(source_format))
	{
		unsigned bytes;
		if (!Bytes_Per_Pixel(source_format, &bytes) ||
			source_pitch < (int)((size_t)width * bytes)) return false;
	}
	else if (source_pitch < (int)(((width + 3U) / 4U) * Dxt_Block_Bytes(source_format)))
	{
		return false;
	}
	try
	{
		pixels->resize(total_bytes);
	}
	catch (...)
	{
		return false;
	}
	for (y = 0; y < height; ++y)
	{
		unsigned x;
		for (x = 0; x < width; ++x)
		{
			if (!Read_Pixel(source, source_pitch, width, height, source_format,
				x, y, &(*pixels)[((size_t)y * width + x) * 4U]))
			{
				pixels->clear();
				return false;
			}
		}
	}
	return true;
}

bool SurfaceBlit_Resample_A8R8G8B8(
	const unsigned char *source,
	unsigned int source_width,
	unsigned int source_height,
	unsigned char *destination,
	unsigned int destination_width,
	unsigned int destination_height,
	SurfaceBlitFilter filter)
{
	size_t source_bytes;
	size_t destination_bytes;
	if (source == 0 || destination == 0 || source_width == 0 ||
		source_height == 0 || destination_width == 0 || destination_height == 0 ||
		!Checked_A8R8G8B8_Byte_Size(source_width, source_height, &source_bytes) ||
		!Checked_A8R8G8B8_Byte_Size(destination_width, destination_height,
			&destination_bytes))
	{
		return false;
	}
	if (filter != SURFACE_BLIT_FILTER_NONE &&
		filter != SURFACE_BLIT_FILTER_TRIANGLE &&
		filter != SURFACE_BLIT_FILTER_BOX)
	{
		return false;
	}
	if (source_width == destination_width && source_height == destination_height)
	{
		memcpy(destination, source, destination_bytes);
		return true;
	}
	if (filter == SURFACE_BLIT_FILTER_NONE)
	{
		for (unsigned y = 0; y < destination_height; ++y)
		{
			const unsigned source_y = y * source_height / destination_height;
			for (unsigned x = 0; x < destination_width; ++x)
			{
				const unsigned source_x = x * source_width / destination_width;
				memcpy(destination + ((size_t)y * destination_width + x) * 4U,
					source + ((size_t)source_y * source_width + source_x) * 4U, 4U);
			}
		}
		return true;
	}
	if (filter == SURFACE_BLIT_FILTER_TRIANGLE)
	{
		return Resample_Bilinear(source, source_width, source_height,
			destination, destination_width, destination_height);
	}
	return Resample_Area(source, source_width, source_height, destination,
		destination_width, destination_height);
}

bool SurfaceBlit_Write_A8R8G8B8(
	const unsigned char *source,
	unsigned int width,
	unsigned int height,
	unsigned char *destination,
	int destination_pitch,
	WW3DFormat destination_format)
{
	unsigned bytes;
	if (source == 0 || destination == 0 || width == 0 || height == 0 ||
		destination_pitch <= 0 || !Bytes_Per_Pixel(destination_format, &bytes) ||
		destination_pitch < (int)((size_t)width * bytes)) return false;
	for (unsigned y = 0; y < height; ++y)
	{
		for (unsigned x = 0; x < width; ++x)
		{
			if (!Write_Pixel(destination + (size_t)y * destination_pitch +
				(size_t)x * bytes, destination_format,
				&source[((size_t)y * width + x) * 4U])) return false;
		}
	}
	return true;
}

#if defined(BUILD_WITH_D3D8)

bool SurfaceBlit_Convert_To_A8R8G8B8(
	const unsigned char *source,
	int source_pitch,
	unsigned int width,
	unsigned int height,
	D3DFORMAT source_format,
	std::vector<unsigned char> *pixels)
{
	WW3DFormat neutral_format;
	return Legacy_D3D8_Format_To_WW3D(source_format, &neutral_format) &&
		SurfaceBlit_Convert_To_A8R8G8B8(source, source_pitch, width,
			height, neutral_format, pixels);
}

bool SurfaceBlit_Write_A8R8G8B8(
	const unsigned char *source,
	unsigned int width,
	unsigned int height,
	unsigned char *destination,
	int destination_pitch,
	D3DFORMAT destination_format)
{
	WW3DFormat neutral_format;
	return Legacy_D3D8_Format_To_WW3D(destination_format, &neutral_format) &&
		SurfaceBlit_Write_A8R8G8B8(source, width, height, destination,
			destination_pitch, neutral_format);
}

HRESULT SurfaceBlit_Copy(
	IDirect3DSurface8 *destination,
	const RECT *destination_rect,
	IDirect3DSurface8 *source,
	const RECT *source_rect,
	SurfaceBlitFilter filter)
{
	D3DSURFACE_DESC destination_description;
	D3DSURFACE_DESC source_description;
	RECT full_destination;
	RECT full_source;
	RECT resolved_destination;
	RECT resolved_source;
	IDirect3DDevice8 *device = 0;
	IDirect3DSurface8 *source_staging = 0;
	IDirect3DSurface8 *destination_staging = 0;
	IDirect3DTexture8 *source_staging_owner = 0;
	IDirect3DTexture8 *destination_staging_owner = 0;
	D3DLOCKED_RECT source_locked;
	D3DLOCKED_RECT destination_locked;
	WW3DFormat destination_format;
	WW3DFormat source_format;
	bool source_is_locked = false;
	bool destination_is_locked = false;
	HRESULT result;
	std::vector<unsigned char> source_pixels;
	std::vector<unsigned char> destination_pixels;
	if (destination == 0 || source == 0 ||
		FAILED(destination->GetDesc(&destination_description)) ||
		FAILED(source->GetDesc(&source_description)) ) return D3DERR_INVALIDCALL;
	if (!Legacy_D3D8_Format_To_WW3D(destination_description.Format,
		&destination_format) ||
		!Legacy_D3D8_Format_To_WW3D(source_description.Format, &source_format))
		return D3DERR_NOTAVAILABLE;
	Full_Rect(destination_description, &full_destination);
	Full_Rect(source_description, &full_source);
	resolved_destination = destination_rect != 0 ? *destination_rect : full_destination;
	resolved_source = source_rect != 0 ? *source_rect : full_source;
	if (!Rect_Is_Valid(Legacy_D3D8_Rect(resolved_destination),
		Legacy_D3D8_Description(destination_description, destination_format)) ||
		!Rect_Is_Valid(Legacy_D3D8_Rect(resolved_source),
		Legacy_D3D8_Description(source_description, source_format)) ||
		!Is_Cpu_Color_Format(source_format) ||
		!Is_Cpu_Color_Format(destination_format)) return D3DERR_NOTAVAILABLE;
	if (SurfaceBlit_Can_Use_CopyRects(destination_description,
		resolved_destination, source_description, resolved_source, filter))
	{
		POINT point;
		point.x = resolved_destination.left;
		point.y = resolved_destination.top;
		result = source->GetDevice(&device);
		if (SUCCEEDED(result) && device != 0)
		{
			result = device->CopyRects(source, &resolved_source, 1, destination,
				&point);
			device->Release();
			device = 0;
			if (SUCCEEDED(result)) return result;
		}
		else if (device != 0)
		{
			device->Release();
		}
	}
	// DXT destinations cannot be written by the CPU fallback.  An exact,
	// same-format copy was already attempted above and remains valid through
	// the native CopyRects path.
	if (Is_Dxt(destination_format)) return D3DERR_NOTAVAILABLE;

	result = source->GetDevice(&device);
	if (FAILED(result) || device == 0) return FAILED(result) ? result : D3DERR_INVALIDCALL;
	result = Create_Staging_Surface(device,
		resolved_source.right - resolved_source.left,
		resolved_source.bottom - resolved_source.top,
		source_description.Format, &source_staging, &source_staging_owner);
	if (SUCCEEDED(result))
	{
		result = Create_Staging_Surface(device,
			resolved_destination.right - resolved_destination.left,
			resolved_destination.bottom - resolved_destination.top,
			destination_description.Format, &destination_staging,
			&destination_staging_owner);
	}
	if (SUCCEEDED(result))
	{
		result = device->CopyRects(source, &resolved_source, 1, source_staging, 0);
	}
	if (SUCCEEDED(result)) result = source_staging->LockRect(&source_locked, 0,
		D3DLOCK_READONLY);
	if (SUCCEEDED(result)) source_is_locked = true;
	if (SUCCEEDED(result)) result = SurfaceBlit_Convert_To_A8R8G8B8(
		(const unsigned char *)source_locked.pBits, source_locked.Pitch,
		resolved_source.right - resolved_source.left,
		resolved_source.bottom - resolved_source.top,
		source_format, &source_pixels) ? D3D_OK : D3DERR_NOTAVAILABLE;
	if (source_is_locked)
	{
		HRESULT unlock_result = source_staging->UnlockRect();
		source_is_locked = false;
		if (SUCCEEDED(result) && FAILED(unlock_result)) result = unlock_result;
	}
	if (SUCCEEDED(result))
	{
		const unsigned destination_width = resolved_destination.right - resolved_destination.left;
		const unsigned destination_height = resolved_destination.bottom - resolved_destination.top;
		size_t destination_bytes;
		if (!Checked_A8R8G8B8_Byte_Size(destination_width, destination_height,
			&destination_bytes)) result = D3DERR_NOTAVAILABLE;
		try { if (SUCCEEDED(result)) destination_pixels.resize(destination_bytes); }
		catch (...) { result = D3DERR_OUTOFVIDEOMEMORY; }
		if (SUCCEEDED(result) && !SurfaceBlit_Resample_A8R8G8B8(
			&source_pixels[0], resolved_source.right - resolved_source.left,
			resolved_source.bottom - resolved_source.top, &destination_pixels[0],
			destination_width, destination_height, filter)) result = D3DERR_NOTAVAILABLE;
	}
	if (SUCCEEDED(result)) result = destination_staging->LockRect(&destination_locked,
		0, 0);
	if (SUCCEEDED(result)) destination_is_locked = true;
	if (SUCCEEDED(result))
	{
		const unsigned destination_width = resolved_destination.right - resolved_destination.left;
		const unsigned destination_height = resolved_destination.bottom - resolved_destination.top;
		unsigned y;
		unsigned bytes;
		if (!Bytes_Per_Pixel(destination_format, &bytes)) result = D3DERR_NOTAVAILABLE;
		for (y = 0; SUCCEEDED(result) && y < destination_height; ++y)
		{
			unsigned x;
			for (x = 0; x < destination_width; ++x)
			{
				if (!Write_Pixel((unsigned char *)destination_locked.pBits +
					(size_t)y * destination_locked.Pitch + (size_t)x * bytes,
					destination_format,
					&destination_pixels[((size_t)y * destination_width + x) * 4U]))
				{
					result = D3DERR_NOTAVAILABLE;
					break;
				}
			}
		}
	}
	if (destination_is_locked)
	{
		HRESULT unlock_result = destination_staging->UnlockRect();
		destination_is_locked = false;
		if (SUCCEEDED(result) && FAILED(unlock_result)) result = unlock_result;
	}
	if (SUCCEEDED(result))
	{
		POINT point;
		point.x = resolved_destination.left;
		point.y = resolved_destination.top;
		result = device->CopyRects(destination_staging, 0, 0, destination, &point);
	}
	if (destination_staging != 0) destination_staging->Release();
	if (source_staging != 0) source_staging->Release();
	if (destination_staging_owner != 0) destination_staging_owner->Release();
	if (source_staging_owner != 0) source_staging_owner->Release();
	device->Release();
	return result;
}

HRESULT SurfaceBlit_Copy_Surface_To_A8R8G8B8(
	IDirect3DSurface8 *source,
	unsigned int width,
	unsigned int height,
	std::vector<unsigned char> *pixels)
{
	D3DSURFACE_DESC description;
	IDirect3DDevice8 *device = 0;
	IDirect3DSurface8 *staging = 0;
	IDirect3DTexture8 *staging_owner = 0;
	D3DLOCKED_RECT locked;
	WW3DFormat source_format;
	bool is_locked = false;
	std::vector<unsigned char> source_pixels;
	HRESULT result;
	if (source == 0 || pixels == 0 || width == 0 || height == 0 ||
		FAILED(source->GetDesc(&description)) ||
		!Legacy_D3D8_Format_To_WW3D(description.Format, &source_format) ||
		!Is_Cpu_Color_Format(source_format)) return D3DERR_NOTAVAILABLE;
	// Managed video textures are CPU-lockable.  Reading those surfaces through
	// CopyRects first introduces a synchronous device copy on every animated
	// frame, even though the source pixels are already available to the caller.
	// Prefer the direct lock path and retain the staging path below for default
	// pool/render-target surfaces that cannot be locked directly.
	D3DLOCKED_RECT direct_locked;
	memset(&direct_locked, 0, sizeof(direct_locked));
	result = source->LockRect(&direct_locked, 0, D3DLOCK_READONLY);
	if (SUCCEEDED(result))
	{
		std::vector<unsigned char> source_pixels;
		const bool converted = direct_locked.pBits != 0 &&
			SurfaceBlit_Convert_To_A8R8G8B8(
				(const unsigned char *)direct_locked.pBits, direct_locked.Pitch,
				description.Width, description.Height, source_format,
				&source_pixels);
		const HRESULT unlock_result = source->UnlockRect();
		if (SUCCEEDED(unlock_result) && converted)
		{
			size_t totalBytes;
			if (!Checked_A8R8G8B8_Byte_Size(width, height, &totalBytes))
				return D3DERR_NOTAVAILABLE;
			try { pixels->resize(totalBytes); }
			catch (...) { return D3DERR_OUTOFVIDEOMEMORY; }
			if (SurfaceBlit_Resample_A8R8G8B8(
				&source_pixels[0], description.Width, description.Height,
				&(*pixels)[0], width, height, SURFACE_BLIT_FILTER_BOX))
			{
				return D3D_OK;
			}
			return D3DERR_NOTAVAILABLE;
		}
		// A successful lock with an unusable layout is a real source failure;
		// do not attempt a second read while the same surface is in an unknown
		// state.  Lock failure itself is expected for default-pool targets and
		// falls through to the established staging implementation.
		if (FAILED(unlock_result))
			return unlock_result;
		return D3DERR_NOTAVAILABLE;
	}
	result = source->GetDevice(&device);
	if (FAILED(result) || device == 0) return FAILED(result) ? result : D3DERR_INVALIDCALL;
	result = Create_Staging_Surface(device, description.Width, description.Height,
		description.Format, &staging, &staging_owner);
	if (SUCCEEDED(result)) result = device->CopyRects(source, 0, 0, staging, 0);
	if (SUCCEEDED(result)) result = staging->LockRect(&locked, 0,
		D3DLOCK_READONLY);
	if (SUCCEEDED(result)) is_locked = true;
	if (SUCCEEDED(result) && !SurfaceBlit_Convert_To_A8R8G8B8(
		(const unsigned char *)locked.pBits, locked.Pitch, description.Width,
		description.Height, source_format, &source_pixels))
		result = D3DERR_NOTAVAILABLE;
	if (is_locked)
	{
		HRESULT unlock_result = staging->UnlockRect();
		is_locked = false;
		if (SUCCEEDED(result) && FAILED(unlock_result)) result = unlock_result;
	}
	if (SUCCEEDED(result))
	{
		size_t totalBytes;
		if (!Checked_A8R8G8B8_Byte_Size(width, height, &totalBytes))
			result = D3DERR_NOTAVAILABLE;
		try { if (SUCCEEDED(result)) pixels->resize(totalBytes); }
		catch (...) { result = D3DERR_OUTOFVIDEOMEMORY; }
		if (SUCCEEDED(result) && !SurfaceBlit_Resample_A8R8G8B8(
			&source_pixels[0], description.Width, description.Height, &(*pixels)[0],
			width, height, SURFACE_BLIT_FILTER_BOX)) result = D3DERR_NOTAVAILABLE;
	}
	if (staging != 0) staging->Release();
	if (staging_owner != 0) staging_owner->Release();
	device->Release();
	return result;
}

#endif
