/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "texturemipgenerator.h"
#include "d3d8.h"

static unsigned minUnsigned(unsigned left, unsigned right)
{
	return left < right ? left : right;
}

static unsigned averageFour(unsigned first, unsigned second,
	unsigned third, unsigned fourth)
{
	return (first + second + third + fourth + 2) / 4;
}

static bool getBytesPerPixel(WW3DFormat format, unsigned& bytes)
{
	switch (format)
	{
		case WW3D_FORMAT_A8R8G8B8:
		case WW3D_FORMAT_X8R8G8B8:
			bytes = 4;
			return true;

		case WW3D_FORMAT_A1R5G5B5:
			bytes = 2;
			return true;

		default:
			bytes = 0;
			return false;
	}
}

static unsigned short readWord(const unsigned char* pixel)
{
	return (unsigned short)(pixel[0] | ((unsigned short)pixel[1] << 8));
}

static void writeWord(unsigned char* pixel, unsigned short value)
{
	pixel[0] = (unsigned char)(value & 0xff);
	pixel[1] = (unsigned char)(value >> 8);
}

static void average32BitPixel(
	const unsigned char* first,
	const unsigned char* second,
	const unsigned char* third,
	const unsigned char* fourth,
	unsigned char* destination)
{
	destination[0] = (unsigned char)averageFour(
		first[0], second[0], third[0], fourth[0]);
	destination[1] = (unsigned char)averageFour(
		first[1], second[1], third[1], fourth[1]);
	destination[2] = (unsigned char)averageFour(
		first[2], second[2], third[2], fourth[2]);
	destination[3] = (unsigned char)averageFour(
		first[3], second[3], third[3], fourth[3]);
}

static void averageA1R5G5B5Pixel(
	const unsigned char* first,
	const unsigned char* second,
	const unsigned char* third,
	const unsigned char* fourth,
	unsigned char* destination)
{
	const unsigned short firstPixel = readWord(first);
	const unsigned short secondPixel = readWord(second);
	const unsigned short thirdPixel = readWord(third);
	const unsigned short fourthPixel = readWord(fourth);
	const unsigned blue = averageFour(
		firstPixel & 0x1f, secondPixel & 0x1f,
		thirdPixel & 0x1f, fourthPixel & 0x1f);
	const unsigned green = averageFour(
		(firstPixel >> 5) & 0x1f, (secondPixel >> 5) & 0x1f,
		(thirdPixel >> 5) & 0x1f, (fourthPixel >> 5) & 0x1f);
	const unsigned red = averageFour(
		(firstPixel >> 10) & 0x1f, (secondPixel >> 10) & 0x1f,
		(thirdPixel >> 10) & 0x1f, (fourthPixel >> 10) & 0x1f);
	const unsigned alpha = averageFour(
		(firstPixel >> 15) & 1, (secondPixel >> 15) & 1,
		(thirdPixel >> 15) & 1, (fourthPixel >> 15) & 1);
	const unsigned short result = (unsigned short)(
		(blue & 0x1f) |
		((green & 0x1f) << 5) |
		((red & 0x1f) << 10) |
		(alpha != 0 ? 0x8000 : 0));

	writeWord(destination, result);
}

bool Generate_Texture_Mip_Level_Box(
	const unsigned char* source,
	unsigned source_pitch,
	unsigned source_width,
	unsigned source_height,
	unsigned char* destination,
	unsigned destination_pitch,
	WW3DFormat format)
{
	unsigned bytesPerPixel;
	unsigned destinationWidth;
	unsigned destinationHeight;
	unsigned sourceRowBytes;
	unsigned destinationRowBytes;
	unsigned y;

	if (source == 0 || destination == 0 || source_width == 0 ||
		source_height == 0 || !getBytesPerPixel(format, bytesPerPixel) ||
		(source_width > (unsigned)-1 / bytesPerPixel) ||
		(source_pitch < (sourceRowBytes = source_width * bytesPerPixel)))
	{
		return false;
	}

	destinationWidth = source_width > 1 ? source_width / 2 : 1;
	destinationHeight = source_height > 1 ? source_height / 2 : 1;
	if (destinationWidth > (unsigned)-1 / bytesPerPixel ||
		destination_pitch < (destinationRowBytes = destinationWidth * bytesPerPixel))
	{
		return false;
	}
	if ((source_height > 1 && source_pitch > (unsigned)-1 / (source_height - 1)) ||
		(destinationHeight > 1 && destination_pitch > (unsigned)-1 / (destinationHeight - 1)))
	{
		return false;
	}

	for (y = 0; y < destinationHeight; ++y)
	{
		const unsigned sourceY = y * 2;
		const unsigned sourceY1 = minUnsigned(sourceY + 1, source_height - 1);
		const unsigned char* firstRow = source + sourceY * source_pitch;
		const unsigned char* secondRow = source + sourceY1 * source_pitch;
		unsigned x;

		for (x = 0; x < destinationWidth; ++x)
		{
			const unsigned sourceX = x * 2;
			const unsigned sourceX1 = minUnsigned(sourceX + 1, source_width - 1);
			const unsigned char* first = firstRow + sourceX * bytesPerPixel;
			const unsigned char* second = firstRow + sourceX1 * bytesPerPixel;
			const unsigned char* third = secondRow + sourceX * bytesPerPixel;
			const unsigned char* fourth = secondRow + sourceX1 * bytesPerPixel;
			unsigned char* output = destination + y * destination_pitch +
				x * bytesPerPixel;

			if (format == WW3D_FORMAT_A1R5G5B5)
			{
				averageA1R5G5B5Pixel(first, second, third, fourth, output);
			}
			else
			{
				average32BitPixel(first, second, third, fourth, output);
			}
		}
	}

	return true;
}

static bool getWW3DFormat(D3DFORMAT format, WW3DFormat& result)
{
	switch (format)
	{
		case D3DFMT_A8R8G8B8:
			result = WW3D_FORMAT_A8R8G8B8;
			return true;

		case D3DFMT_X8R8G8B8:
			result = WW3D_FORMAT_X8R8G8B8;
			return true;

		case D3DFMT_A1R5G5B5:
			result = WW3D_FORMAT_A1R5G5B5;
			return true;

		default:
			return false;
	}
}

unsigned Generate_DX8_Texture_Mip_Levels(IDirect3DTexture8* texture)
{
	const UINT levelCount = texture != 0 ? texture->GetLevelCount() : 0;
	unsigned level;

	if (texture == 0)
	{
		return D3DERR_INVALIDCALL;
	}

	for (level = 1; level < levelCount; ++level)
	{
		IDirect3DSurface8* sourceSurface = 0;
		IDirect3DSurface8* destinationSurface = 0;
		D3DSURFACE_DESC sourceDesc;
		D3DSURFACE_DESC destinationDesc;
		D3DLOCKED_RECT sourceLocked;
		D3DLOCKED_RECT destinationLocked;
		bool sourceIsLocked = false;
		bool destinationIsLocked = false;
		WW3DFormat format;
		HRESULT result;

		result = texture->GetSurfaceLevel(level - 1, &sourceSurface);
		if (FAILED(result))
		{
			return result;
		}
		result = texture->GetSurfaceLevel(level, &destinationSurface);
		if (FAILED(result))
		{
			sourceSurface->Release();
			return result;
		}

		result = sourceSurface->GetDesc(&sourceDesc);
		if (FAILED(result))
		{
			destinationSurface->Release();
			sourceSurface->Release();
			return result;
		}
		result = destinationSurface->GetDesc(&destinationDesc);
		if (FAILED(result))
		{
			destinationSurface->Release();
			sourceSurface->Release();
			return result;
		}

		if (!getWW3DFormat(sourceDesc.Format, format) ||
			destinationDesc.Format != sourceDesc.Format ||
			destinationDesc.Width != (sourceDesc.Width > 1 ? sourceDesc.Width / 2 : 1) ||
			destinationDesc.Height != (sourceDesc.Height > 1 ? sourceDesc.Height / 2 : 1))
		{
			destinationSurface->Release();
			sourceSurface->Release();
			return D3DERR_NOTAVAILABLE;
		}

		result = sourceSurface->LockRect(&sourceLocked, 0, D3DLOCK_READONLY);
		if (FAILED(result))
		{
			destinationSurface->Release();
			sourceSurface->Release();
			return result;
		}
		sourceIsLocked = true;
		result = destinationSurface->LockRect(&destinationLocked, 0, 0);
		if (FAILED(result))
		{
			sourceSurface->UnlockRect();
			destinationSurface->Release();
			sourceSurface->Release();
			return result;
		}
		destinationIsLocked = true;

		if (sourceLocked.pBits == 0 || destinationLocked.pBits == 0 ||
			sourceLocked.Pitch <= 0 || destinationLocked.Pitch <= 0 ||
			!Generate_Texture_Mip_Level_Box(
			(const unsigned char*)sourceLocked.pBits,
			(unsigned)sourceLocked.Pitch,
			sourceDesc.Width,
			sourceDesc.Height,
			(unsigned char*)destinationLocked.pBits,
			(unsigned)destinationLocked.Pitch,
			format))
		{
			result = D3DERR_NOTAVAILABLE;
		}
		else
		{
			result = D3D_OK;
		}

		if (destinationIsLocked)
		{
			const HRESULT unlockResult = destinationSurface->UnlockRect();
			destinationIsLocked = false;
			if (SUCCEEDED(result) && FAILED(unlockResult))
			{
				result = unlockResult;
			}
		}
		if (sourceIsLocked)
		{
			const HRESULT unlockResult = sourceSurface->UnlockRect();
			sourceIsLocked = false;
			if (SUCCEEDED(result) && FAILED(unlockResult))
			{
				result = unlockResult;
			}
		}

		destinationSurface->Release();
		sourceSurface->Release();
		if (FAILED(result))
		{
			return result;
		}
	}

	return D3D_OK;
}
