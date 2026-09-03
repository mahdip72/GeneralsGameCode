/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "legacytexturecompat.h"

#include "surfaceblit.h"
#include "WWLib/TARGA.h"

#include <limits>
#include <new>
#include <string.h>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <wincodec.h>
#define LEGACY_TEXTURE_HAS_WIC 1
#endif
#endif

namespace
{
const size_t LEGACY_TEXTURE_MAX_BYTES = 256U * 1024U * 1024U;

// Keep the registration storage function-local so title-owned static
// registrars cannot race the Core translation unit's dynamic initialization.
static LegacyTextureDDSDecodeCallback &DDS_Decode_Callback()
{
	static LegacyTextureDDSDecodeCallback callback = 0;
	return callback;
}

static bool Is_Power_Of_Two(UINT value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

static UINT Full_Mip_Level_Count(UINT width, UINT height, UINT depth)
{
	UINT largest = width;
	UINT levels = 1;
	if (height > largest) largest = height;
	if (depth > largest) largest = depth;
	while (largest > 1)
	{
		largest /= 2U;
		++levels;
	}
	return levels;
}

static bool Format_Is_Characterized(D3DFORMAT format)
{
	switch (format)
	{
	case D3DFMT_R8G8B8:
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_X4R4G4B4:
	case D3DFMT_R3G3B2:
	case D3DFMT_A8:
	case D3DFMT_A8R3G3B2:
	case D3DFMT_L8:
	case D3DFMT_A8L8:
	case D3DFMT_A4L4:
		return true;
	default:
		return false;
	}
}

static bool Descriptor_Dimensions_Are_Valid(
	const LegacyTextureCreationDescriptor &descriptor,
	const D3DCAPS8 &caps)
{
	UINT maxDimension;
	UINT minDimension;
	UINT fullMipLevels;
	if (descriptor.format == D3DFMT_UNKNOWN || descriptor.width == 0 ||
		descriptor.height == 0 || descriptor.depth == 0)
	{
		return false;
	}
	if (descriptor.kind == LEGACY_TEXTURE_CREATION_CUBE)
	{
		if ((caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) == 0 ||
			caps.MaxTextureWidth == 0 || caps.MaxTextureHeight == 0 ||
			descriptor.width != descriptor.height ||
			descriptor.width > caps.MaxTextureWidth ||
			descriptor.height > caps.MaxTextureHeight)
		{
			return false;
		}
	}
	else if (descriptor.kind == LEGACY_TEXTURE_CREATION_VOLUME)
	{
		if ((caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) == 0 ||
			caps.MaxVolumeExtent == 0 || descriptor.width > caps.MaxVolumeExtent ||
			descriptor.height > caps.MaxVolumeExtent ||
			descriptor.depth > caps.MaxVolumeExtent)
		{
			return false;
		}
	}
	else
	{
		return false;
	}
	// NONPOW2CONDITIONAL is not an unconditional POW2 requirement.  The
	// conditional restrictions depend on the resource operation and are
	// ultimately checked by Create*Texture/CheckDeviceFormat.  Rejecting every
	// non-power-of-two cube or volume here changed the legacy device contract.
	if ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0 &&
		(!Is_Power_Of_Two(descriptor.width) ||
		 !Is_Power_Of_Two(descriptor.height) ||
		 !Is_Power_Of_Two(descriptor.depth)))
	{
		return false;
	}
	// A cube's depth field is a descriptor sentinel, not a third sampled
	// dimension.  Its face aspect ratio is already guaranteed to be 1:1.
	if (descriptor.kind == LEGACY_TEXTURE_CREATION_VOLUME)
	{
		maxDimension = descriptor.width;
		if (descriptor.height > maxDimension) maxDimension = descriptor.height;
		if (descriptor.depth > maxDimension) maxDimension = descriptor.depth;
		minDimension = descriptor.width;
		if (descriptor.height < minDimension) minDimension = descriptor.height;
		if (descriptor.depth < minDimension) minDimension = descriptor.depth;
		if (caps.MaxTextureAspectRatio != 0 && minDimension != 0 &&
			maxDimension / minDimension > caps.MaxTextureAspectRatio)
		{
			return false;
		}
	}
	fullMipLevels = Full_Mip_Level_Count(descriptor.width,
		descriptor.height, descriptor.depth);
	return descriptor.mipLevels == 0 || descriptor.mipLevels <= fullMipLevels;
}

static HRESULT Validate_Device_Format(IDirect3DDevice8 *device,
	const LegacyTextureCreationDescriptor &descriptor, D3DRESOURCETYPE type)
{
	D3DDEVICE_CREATION_PARAMETERS parameters;
	D3DDISPLAYMODE displayMode;
	IDirect3D8 *direct3D = 0;
	HRESULT result;
	if (device == 0) return D3DERR_INVALIDCALL;
	result = device->GetCreationParameters(&parameters);
	if (FAILED(result)) return result;
	result = device->GetDirect3D(&direct3D);
	if (FAILED(result) || direct3D == 0)
	{
		if (direct3D != 0) direct3D->Release();
		return FAILED(result) ? result : D3DERR_INVALIDCALL;
	}
	result = direct3D->GetAdapterDisplayMode(parameters.AdapterOrdinal,
		&displayMode);
	if (SUCCEEDED(result))
	{
		result = direct3D->CheckDeviceFormat(parameters.AdapterOrdinal,
			parameters.DeviceType, displayMode.Format, descriptor.usage, type,
			descriptor.format);
	}
	direct3D->Release();
	return result;
}

static bool Decode_Targa_Pixel(const unsigned char *image,
	const unsigned char *palette, unsigned paletteStart, unsigned paletteLength,
	unsigned paletteBytes, unsigned sourceBytes,
	unsigned char *pixel)
{
	if (image == 0 || pixel == 0) return false;
	if (palette != 0)
	{
		const unsigned paletteIndex = (unsigned)image[0];
		if (sourceBytes != 1 || (paletteBytes != 3 && paletteBytes != 4) ||
			paletteIndex < paletteStart ||
			paletteIndex - paletteStart >= paletteLength)
		{
			return false;
		}
		// Targa::Load stores the map at its declared index in the caller's
		// 256-entry buffer, so retain the original image index here.
		const unsigned char *entry = palette + paletteIndex * paletteBytes;
		pixel[0] = entry[0];
		pixel[1] = entry[1];
		pixel[2] = entry[2];
		pixel[3] = paletteBytes == 4 ? entry[3] : 0xff;
		return true;
	}
	if (sourceBytes == 4 || sourceBytes == 3)
	{
		pixel[0] = image[0];
		pixel[1] = image[1];
		pixel[2] = image[2];
		pixel[3] = sourceBytes == 4 ? image[3] : 0xff;
		return true;
	}
	if (sourceBytes == 2)
	{
		const unsigned short value = (unsigned short)(image[0] |
			((unsigned short)image[1] << 8));
		pixel[0] = (unsigned char)(((value & 0x1f) << 3) |
			((value & 0x1f) >> 2));
		pixel[1] = (unsigned char)((((value >> 5) & 0x1f) << 3) |
			(((value >> 5) & 0x1f) >> 2));
		pixel[2] = (unsigned char)((((value >> 10) & 0x1f) << 3) |
			(((value >> 10) & 0x1f) >> 2));
		pixel[3] = (value & 0x8000) != 0 ? 0xff : 0;
		return true;
	}
	if (sourceBytes == 1)
	{
		pixel[0] = pixel[1] = pixel[2] = image[0];
		pixel[3] = 0xff;
		return true;
	}
	return false;
}

static bool Checked_Multiply(size_t left, size_t right, size_t *result);

static bool Decode_Targa(const char *filename, std::vector<unsigned char> *pixels,
	unsigned *width, unsigned *height, PALETTEENTRY *palette)
{
	Targa targa;
	char targaPalette[256 * 4];
	unsigned sourceBytes;
	unsigned paletteStart = 0;
	unsigned paletteLength = 0;
	unsigned paletteBytes = 0;
	unsigned sourceWidth;
	unsigned sourceHeight;
	unsigned char *image;
	const unsigned char *sourcePalette = 0;
	size_t pixelCount;
	size_t totalBytes;
	if (filename == 0 || pixels == 0 || width == 0 || height == 0 ||
		targa.Open(filename, TGA_READMODE) != 0)
	{
		return false;
	}
	targa.Header.ImageDescriptor ^= TGAIDF_YORIGIN;
	sourceWidth = targa.Header.Width > 0 ? (unsigned)targa.Header.Width : 0;
	sourceHeight = targa.Header.Height > 0 ? (unsigned)targa.Header.Height : 0;
	sourceBytes = ((unsigned char)targa.Header.PixelDepth + 7U) / 8U;
	if (sourceWidth == 0 || sourceHeight == 0 ||
		(sourceBytes != 1 && sourceBytes != 2 && sourceBytes != 3 && sourceBytes != 4))
	{
		return false;
	}
	if (targa.Header.ColorMapType != 0)
	{
		if (targa.Header.CMapStart < 0 || targa.Header.CMapLength <= 0)
		{
			return false;
		}
		paletteStart = (unsigned)targa.Header.CMapStart;
		paletteLength = (unsigned)targa.Header.CMapLength;
		paletteBytes = ((unsigned char)targa.Header.CMapDepth + 7U) / 8U;
		if ((paletteBytes != 3 && paletteBytes != 4) || paletteStart > 256U ||
			paletteLength > 256U - paletteStart || sourceBytes != 1)
		{
			return false;
		}
		memset(targaPalette, 0, sizeof(targaPalette));
		targa.SetPalette(targaPalette);
	}
	if (targa.Load(filename, TGAF_IMAGE, false) != 0 || targa.GetImage() == 0)
	{
		return false;
	}
	image = (unsigned char *)targa.GetImage();
	sourcePalette = paletteBytes != 0 ? (const unsigned char *)targa.GetPalette() : 0;
	if (palette != 0 && sourcePalette != 0)
	{
		unsigned index;
		for (index = 0; index < 256; ++index)
		{
			const unsigned char *entry = sourcePalette + index * paletteBytes;
			palette[index].peBlue = entry[0];
			palette[index].peGreen = entry[1];
			palette[index].peRed = entry[2];
			palette[index].peFlags = paletteBytes == 4 ? entry[3] : 0xff;
		}
	}
	if ((size_t)sourceWidth > (std::numeric_limits<size_t>::max)() /
		sourceHeight || !Checked_Multiply(sourceWidth * (size_t)sourceHeight,
		4U, &totalBytes) || totalBytes > 256U * 1024U * 1024U)
	{
		return false;
	}
	pixelCount = (size_t)sourceWidth * sourceHeight;
	try { pixels->resize(totalBytes); }
	catch (...) { return false; }
	for (unsigned index = 0; index < pixelCount; ++index)
	{
		if (!Decode_Targa_Pixel(image + (size_t)index * sourceBytes,
			sourcePalette, paletteStart, paletteLength, paletteBytes, sourceBytes,
			&(*pixels)[index * 4U]))
		{
			pixels->clear();
			return false;
		}
	}
	*width = sourceWidth;
	*height = sourceHeight;
	return true;
}

#if defined(LEGACY_TEXTURE_HAS_WIC)
static bool Decode_WIC(const char *filename, std::vector<unsigned char> *pixels,
	unsigned *width, unsigned *height, PALETTEENTRY *palette)
{
	IWICImagingFactory *factory = 0;
	IWICBitmapDecoder *decoder = 0;
	IWICBitmapFrameDecode *frame = 0;
	IWICFormatConverter *converter = 0;
	std::vector<wchar_t> wideFilename;
	UINT sourceWidth = 0;
	UINT sourceHeight = 0;
	size_t pixelBytes = 0;
	int wideLength;
	HRESULT initializeResult;
	HRESULT result;
	bool shouldUninitialize = false;
	bool decoded = false;
	if (filename == 0 || pixels == 0 || width == 0 || height == 0) return false;
	wideLength = MultiByteToWideChar(CP_ACP, 0, filename, -1, 0, 0);
	if (wideLength <= 1) return false;
	try { wideFilename.resize((size_t)wideLength); }
	catch (...) { return false; }
	if (MultiByteToWideChar(CP_ACP, 0, filename, -1, &wideFilename[0],
		wideLength) != wideLength) return false;
	initializeResult = CoInitializeEx(0, COINIT_MULTITHREADED);
	shouldUninitialize = SUCCEEDED(initializeResult);
	if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE)
		return false;
	result = CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER,
		IID_IWICImagingFactory, (void **)&factory);
	if (SUCCEEDED(result)) result = factory->CreateDecoderFromFilename(
		&wideFilename[0], 0, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
	if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
	if (SUCCEEDED(result)) result = frame->GetSize(&sourceWidth, &sourceHeight);
	if (SUCCEEDED(result) && (sourceWidth == 0 || sourceHeight == 0 ||
		sourceWidth > (std::numeric_limits<UINT>::max)() / 4U ||
		!Checked_Multiply((size_t)sourceWidth, sourceHeight, &pixelBytes) ||
		!Checked_Multiply(pixelBytes, 4U, &pixelBytes) ||
		pixelBytes > LEGACY_TEXTURE_MAX_BYTES)) result = E_FAIL;
	if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
	if (SUCCEEDED(result)) result = converter->Initialize(frame,
		GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, 0, 0.0,
		WICBitmapPaletteTypeCustom);
	if (SUCCEEDED(result))
	{
		try { pixels->resize(pixelBytes); }
		catch (...) { result = E_OUTOFMEMORY; }
	}
	if (SUCCEEDED(result)) result = converter->CopyPixels(0,
		sourceWidth * 4U, (UINT)pixelBytes, &(*pixels)[0]);
	if (SUCCEEDED(result))
	{
		if (palette != 0) memset(palette, 0, 256U * sizeof(*palette));
		*width = sourceWidth;
		*height = sourceHeight;
		decoded = true;
	}
	if (converter != 0) converter->Release();
	if (frame != 0) frame->Release();
	if (decoder != 0) decoder->Release();
	if (factory != 0) factory->Release();
	if (shouldUninitialize) CoUninitialize();
	if (!decoded) pixels->clear();
	return decoded;
}
#else
static bool Decode_WIC(const char *, std::vector<unsigned char> *, unsigned *,
	unsigned *, PALETTEENTRY *)
{
	return false;
}
#endif

static bool Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right >
		(std::numeric_limits<size_t>::max)() / left)) return false;
	*result = left * right;
	return true;
}

static SurfaceBlitFilter To_Surface_Filter(LegacyTextureFilter filter)
{
	switch (filter)
	{
	case LEGACY_TEXTURE_FILTER_TRIANGLE: return SURFACE_BLIT_FILTER_TRIANGLE;
	case LEGACY_TEXTURE_FILTER_BOX: return SURFACE_BLIT_FILTER_BOX;
	case LEGACY_TEXTURE_FILTER_NONE: return SURFACE_BLIT_FILTER_NONE;
	default: return SURFACE_BLIT_FILTER_NONE;
	}
}

static HRESULT Generate_File_Mips(IDirect3DTexture8 *texture,
	SurfaceBlitFilter filter)
{
	const UINT levelCount = texture != 0 ? texture->GetLevelCount() : 0;
	for (UINT level = 1; level < levelCount; ++level)
	{
		IDirect3DSurface8 *sourceSurface = 0;
		IDirect3DSurface8 *destinationSurface = 0;
		D3DSURFACE_DESC sourceDescription;
		D3DSURFACE_DESC destinationDescription;
		D3DLOCKED_RECT sourceLocked;
		D3DLOCKED_RECT destinationLocked;
		std::vector<unsigned char> sourcePixels;
		std::vector<unsigned char> destinationPixels;
		size_t destinationBytes = 0;
		bool sourceIsLocked = false;
		bool destinationIsLocked = false;
		HRESULT result = texture->GetSurfaceLevel(level - 1, &sourceSurface);
		if (SUCCEEDED(result)) result = texture->GetSurfaceLevel(level,
			&destinationSurface);
		if (SUCCEEDED(result)) result = sourceSurface->GetDesc(&sourceDescription);
		if (SUCCEEDED(result)) result = destinationSurface->GetDesc(&destinationDescription);
		if (SUCCEEDED(result) && (destinationDescription.Width !=
			(sourceDescription.Width > 1 ? sourceDescription.Width / 2 : 1) ||
			destinationDescription.Height !=
			(sourceDescription.Height > 1 ? sourceDescription.Height / 2 : 1)))
			result = D3DERR_NOTAVAILABLE;
		if (SUCCEEDED(result)) result = sourceSurface->LockRect(&sourceLocked, 0,
			D3DLOCK_READONLY);
		if (SUCCEEDED(result)) sourceIsLocked = true;
		if (SUCCEEDED(result) && !SurfaceBlit_Convert_To_A8R8G8B8(
			(const unsigned char *)sourceLocked.pBits, sourceLocked.Pitch,
			sourceDescription.Width, sourceDescription.Height,
			sourceDescription.Format, &sourcePixels)) result = D3DERR_NOTAVAILABLE;
		if (sourceIsLocked)
		{
			const HRESULT unlockResult = sourceSurface->UnlockRect();
			sourceIsLocked = false;
			if (SUCCEEDED(result) && FAILED(unlockResult)) result = unlockResult;
		}
		if (SUCCEEDED(result))
		{
			if (!Checked_Multiply((size_t)destinationDescription.Width,
				destinationDescription.Height, &destinationBytes) ||
				!Checked_Multiply(destinationBytes, 4U, &destinationBytes) ||
				destinationBytes > LEGACY_TEXTURE_MAX_BYTES)
			{
				result = D3DERR_NOTAVAILABLE;
			}
			else
			{
				try { destinationPixels.resize(destinationBytes); }
				catch (...) { result = D3DERR_OUTOFVIDEOMEMORY; }
			}
			if (SUCCEEDED(result) && !SurfaceBlit_Resample_A8R8G8B8(
				&sourcePixels[0], sourceDescription.Width, sourceDescription.Height,
				&destinationPixels[0], destinationDescription.Width,
				destinationDescription.Height, filter))
				result = D3DERR_NOTAVAILABLE;
		}
		if (SUCCEEDED(result)) result = destinationSurface->LockRect(
			&destinationLocked, 0, 0);
		if (SUCCEEDED(result)) destinationIsLocked = true;
		if (SUCCEEDED(result) && !SurfaceBlit_Write_A8R8G8B8(
			&destinationPixels[0], destinationDescription.Width,
			destinationDescription.Height, (unsigned char *)destinationLocked.pBits,
			destinationLocked.Pitch, destinationDescription.Format))
			result = D3DERR_NOTAVAILABLE;
		if (destinationIsLocked)
		{
			const HRESULT unlockResult = destinationSurface->UnlockRect();
			destinationIsLocked = false;
			if (SUCCEEDED(result) && FAILED(unlockResult)) result = unlockResult;
		}
		if (destinationSurface != 0) destinationSurface->Release();
		if (sourceSurface != 0) sourceSurface->Release();
		if (FAILED(result)) return result;
	}
	return D3D_OK;
}

static bool DDS_Format_To_D3D(
	LegacyTextureDDSFormat format, D3DFORMAT *d3dFormat, unsigned *blockBytes)
{
	if (d3dFormat == 0 || blockBytes == 0) return false;
	*blockBytes = 0;
	switch (format)
	{
	case LEGACY_TEXTURE_DDS_DXT1:
		*d3dFormat = D3DFMT_DXT1;
		*blockBytes = 8;
		return true;
	case LEGACY_TEXTURE_DDS_DXT2:
		*d3dFormat = D3DFMT_DXT2;
		*blockBytes = 16;
		return true;
	case LEGACY_TEXTURE_DDS_DXT3:
		*d3dFormat = D3DFMT_DXT3;
		*blockBytes = 16;
		return true;
	case LEGACY_TEXTURE_DDS_DXT4:
		*d3dFormat = D3DFMT_DXT4;
		*blockBytes = 16;
		return true;
	case LEGACY_TEXTURE_DDS_DXT5:
		*d3dFormat = D3DFMT_DXT5;
		*blockBytes = 16;
		return true;
	default:
		*d3dFormat = D3DFMT_UNKNOWN;
		return false;
	}
}

static UINT DDS_Level_Dimension(UINT dimension, UINT level)
{
	while (level != 0 && dimension > 1)
	{
		dimension >>= 1;
		--level;
	}
	return dimension == 0 ? 1 : dimension;
}

static bool DDS_Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (right != 0 && left > ((size_t)-1) / right))
		return false;
	*result = left * right;
	return true;
}

static HRESULT Create_DDS_Texture(
	IDirect3DDevice8 *device,
	const LegacyTextureDDSImage &image,
	UINT width,
	UINT height,
	UINT mipLevels,
	DWORD usage,
	D3DFORMAT format,
	D3DPOOL pool,
	LegacyTextureFilter filter,
	LegacyTextureFilter mipFilter,
	D3DCOLOR colorKey,
	LegacyTextureImageInfo *sourceInfo,
	IDirect3DTexture8 **texture)
{
	D3DFORMAT ddsFormat = D3DFMT_UNKNOWN;
	unsigned blockBytes = 0;
	UINT destinationLevels;
	IDirect3DTexture8 *createdTexture = 0;
	if (texture != 0) *texture = 0;
	if (device == 0 || texture == 0 || image.width == 0 ||
		image.height == 0 || image.mipLevels == 0 ||
		image.getLevelData == 0 || image.getLevelSize == 0 ||
		(image.context != 0 && image.release == 0))
		return D3DERR_INVALIDCALL;
	if (width != LEGACY_TEXTURE_DIMENSION_DEFAULT ||
		height != LEGACY_TEXTURE_DIMENSION_DEFAULT || colorKey != 0)
		return D3DERR_NOTAVAILABLE;
	if ((filter != LEGACY_TEXTURE_FILTER_NONE &&
		filter != LEGACY_TEXTURE_FILTER_TRIANGLE &&
		filter != LEGACY_TEXTURE_FILTER_BOX) ||
		(mipFilter != LEGACY_TEXTURE_FILTER_NONE &&
		mipFilter != LEGACY_TEXTURE_FILTER_TRIANGLE &&
		mipFilter != LEGACY_TEXTURE_FILTER_BOX))
		return D3DERR_INVALIDCALL;
	if (!DDS_Format_To_D3D(image.format, &ddsFormat, &blockBytes))
		return D3DERR_NOTAVAILABLE;
	if (format != D3DFMT_UNKNOWN && format != ddsFormat)
		return D3DERR_NOTAVAILABLE;
	destinationLevels = image.mipLevels;
	if (mipLevels != 0)
	{
		if (mipLevels > image.mipLevels) return D3DERR_NOTAVAILABLE;
		destinationLevels = mipLevels;
	}
	{
		const HRESULT result = device->CreateTexture(image.width, image.height,
			destinationLevels, usage, ddsFormat, pool, &createdTexture);
		if (FAILED(result) || createdTexture == 0)
			return FAILED(result) ? result : E_FAIL;
	}
	for (UINT level = 0; level < destinationLevels; ++level)
	{
		const UINT levelWidth = DDS_Level_Dimension(image.width, level);
		const UINT levelHeight = DDS_Level_Dimension(image.height, level);
		const size_t blockColumns = ((size_t)levelWidth + 3U) / 4U;
		const size_t blockRows = ((size_t)levelHeight + 3U) / 4U;
		size_t rowBytes = 0;
		size_t requiredBytes = 0;
		const unsigned char *source = image.getLevelData(image.context, level);
		IDirect3DSurface8 *surface = 0;
		D3DLOCKED_RECT locked;
		bool lockedSurface = false;
		HRESULT levelResult = D3D_OK;
		if (!DDS_Checked_Multiply(blockColumns, blockBytes, &rowBytes) ||
			!DDS_Checked_Multiply(rowBytes, blockRows, &requiredBytes) ||
			source == 0 || image.getLevelSize(image.context, level) < requiredBytes)
			levelResult = D3DERR_NOTAVAILABLE;
		if (SUCCEEDED(levelResult))
			levelResult = createdTexture->GetSurfaceLevel(level, &surface);
		if (SUCCEEDED(levelResult) && surface == 0)
			levelResult = E_FAIL;
		if (SUCCEEDED(levelResult))
			levelResult = surface->LockRect(&locked, 0, 0);
		if (SUCCEEDED(levelResult)) lockedSurface = true;
		if (SUCCEEDED(levelResult) &&
			(locked.pBits == 0 || locked.Pitch <= 0 ||
			(size_t)locked.Pitch < rowBytes))
			levelResult = D3DERR_NOTAVAILABLE;
		if (SUCCEEDED(levelResult))
		{
			for (size_t row = 0; row < blockRows; ++row)
			{
				memcpy((unsigned char *)locked.pBits + row * locked.Pitch,
					source + row * rowBytes, rowBytes);
			}
		}
		if (lockedSurface)
		{
			const HRESULT unlockResult = surface->UnlockRect();
			lockedSurface = false;
			if (SUCCEEDED(levelResult) && FAILED(unlockResult))
				levelResult = unlockResult;
		}
		if (surface != 0) surface->Release();
		if (FAILED(levelResult))
		{
			createdTexture->Release();
			return levelResult;
		}
	}
	if (sourceInfo != 0)
	{
		sourceInfo->width = image.width;
		sourceInfo->height = image.height;
		sourceInfo->depth = 1;
		sourceInfo->mipLevels = destinationLevels;
		sourceInfo->format = ddsFormat;
	}
	*texture = createdTexture;
	return D3D_OK;
}
}

LegacyTextureCreationRoute LegacyTextureCreation_Get_Route(
	LegacyTextureCreationKind kind)
{
	return kind == LEGACY_TEXTURE_CREATION_FILE ?
		LEGACY_TEXTURE_ROUTE_PROJECT_DECODER :
		LEGACY_TEXTURE_ROUTE_NATIVE_REQUIREMENTS;
}

void LegacyTextureCreation_Register_DDS_Decode_Callback(
	LegacyTextureDDSDecodeCallback callback)
{
	DDS_Decode_Callback() = callback;
}

bool LegacyTextureCreation_Has_DDS_Decode_Callback()
{
	return DDS_Decode_Callback() != 0;
}

bool LegacyTextureCreation_Build_Cube_Descriptor(
	UINT size, UINT mipLevels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
	LegacyTextureCreationDescriptor *descriptor)
{
	if (descriptor == 0 || size == 0) return false;
	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->kind = LEGACY_TEXTURE_CREATION_CUBE;
	descriptor->width = descriptor->height = size;
	descriptor->depth = 1;
	descriptor->mipLevels = mipLevels;
	descriptor->usage = usage;
	descriptor->format = format;
	descriptor->pool = pool;
	return true;
}

bool LegacyTextureCreation_Build_Volume_Descriptor(
	UINT width, UINT height, UINT depth, UINT mipLevels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, LegacyTextureCreationDescriptor *descriptor)
{
	if (descriptor == 0 || width == 0 || height == 0 || depth == 0) return false;
	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->kind = LEGACY_TEXTURE_CREATION_VOLUME;
	descriptor->width = width;
	descriptor->height = height;
	descriptor->depth = depth;
	descriptor->mipLevels = mipLevels;
	descriptor->usage = usage;
	descriptor->format = format;
	descriptor->pool = pool;
	return true;
}

bool LegacyTextureCreation_Validate_Descriptor_For_Caps(
	const LegacyTextureCreationDescriptor &descriptor, const D3DCAPS8 &caps)
{
	return Descriptor_Dimensions_Are_Valid(descriptor, caps);
}

bool LegacyTextureCreation_Is_Default_Dimension(UINT dimension)
{
	return dimension == LEGACY_TEXTURE_DIMENSION_DEFAULT;
}

bool LegacyTextureCreation_Is_Characterized_File_Format(D3DFORMAT format)
{
	return Format_Is_Characterized(format);
}

HRESULT LegacyTextureCreation_Create_Cube(
	IDirect3DDevice8 *device, UINT size, UINT mipLevels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8 **texture)
{
	LegacyTextureCreationDescriptor descriptor;
	D3DCAPS8 caps;
	HRESULT result;
	if (texture != 0) *texture = 0;
	if (device == 0 || texture == 0 ||
		!LegacyTextureCreation_Build_Cube_Descriptor(size, mipLevels, usage,
			format, pool, &descriptor)) return D3DERR_INVALIDCALL;
	result = device->GetDeviceCaps(&caps);
	if (FAILED(result)) return result;
	if (!LegacyTextureCreation_Validate_Descriptor_For_Caps(descriptor, caps))
		return D3DERR_NOTAVAILABLE;
	result = Validate_Device_Format(device, descriptor, D3DRTYPE_CUBETEXTURE);
	if (FAILED(result)) return result;
	return device->CreateCubeTexture(descriptor.width, descriptor.mipLevels,
		descriptor.usage, descriptor.format, descriptor.pool, texture);
}

HRESULT LegacyTextureCreation_Create_Volume(
	IDirect3DDevice8 *device, UINT width, UINT height, UINT depth, UINT mipLevels,
	DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture8 **texture)
{
	LegacyTextureCreationDescriptor descriptor;
	D3DCAPS8 caps;
	HRESULT result;
	if (texture != 0) *texture = 0;
	if (device == 0 || texture == 0 ||
		!LegacyTextureCreation_Build_Volume_Descriptor(width, height, depth,
			mipLevels, usage, format, pool, &descriptor)) return D3DERR_INVALIDCALL;
	result = device->GetDeviceCaps(&caps);
	if (FAILED(result)) return result;
	if (!LegacyTextureCreation_Validate_Descriptor_For_Caps(descriptor, caps))
		return D3DERR_NOTAVAILABLE;
	result = Validate_Device_Format(device, descriptor, D3DRTYPE_VOLUMETEXTURE);
	if (FAILED(result)) return result;
	return device->CreateVolumeTexture(descriptor.width, descriptor.height,
		descriptor.depth, descriptor.mipLevels, descriptor.usage, descriptor.format,
		descriptor.pool, texture);
}

HRESULT LegacyTextureCreation_Create_File_Texture(
	IDirect3DDevice8 *device, const char *filename, UINT width, UINT height,
	UINT mipLevels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
	LegacyTextureFilter filter, LegacyTextureFilter mipFilter, D3DCOLOR colorKey,
	LegacyTextureImageInfo *sourceInfo, PALETTEENTRY *palette,
	IDirect3DTexture8 **texture)
{
	std::vector<unsigned char> sourcePixels;
	std::vector<unsigned char> resizedPixels;
	unsigned sourceWidth;
	unsigned sourceHeight;
	unsigned destinationWidth;
	unsigned destinationHeight;
	unsigned destinationLevels;
	D3DFORMAT destinationFormat;
	IDirect3DTexture8 *createdTexture = 0;
	D3DLOCKED_RECT locked;
	bool textureLevelIsLocked = false;
	HRESULT result;
	if (texture != 0) *texture = 0;
	if (device == 0 || filename == 0 || texture == 0) return D3DERR_INVALIDCALL;
	// Preserve the historical decoder order.  Targa remains the first path so
	// indexed/paletted assets retain their palette behavior.  The title-owned
	// DDS callback is consulted only after Targa declines the file; Core-only
	// tools simply leave it unregistered and continue to WIC.
	if (!Decode_Targa(filename, &sourcePixels, &sourceWidth, &sourceHeight,
		palette))
	{
		LegacyTextureDDSDecodeCallback ddsCallback = DDS_Decode_Callback();
		if (ddsCallback != 0)
		{
			LegacyTextureDDSImage image;
			memset(&image, 0, sizeof(image));
			if (ddsCallback(filename, &image))
			{
				result = Create_DDS_Texture(device, image, width, height,
					mipLevels, usage, format, pool, filter, mipFilter, colorKey,
					sourceInfo, texture);
				if (image.release != 0) image.release(image.context);
				if (result != D3DERR_NOTAVAILABLE) return result;
			}
		}
		if (!Decode_WIC(filename, &sourcePixels, &sourceWidth, &sourceHeight,
			palette)) return D3DERR_NOTAVAILABLE;
	}
	destinationWidth = LegacyTextureCreation_Is_Default_Dimension(width) ?
		sourceWidth : width;
	destinationHeight = LegacyTextureCreation_Is_Default_Dimension(height) ?
		sourceHeight : height;
	if (destinationWidth == 0 || destinationHeight == 0 ||
		(!LegacyTextureCreation_Is_Default_Dimension(width) && width == 0) ||
		(!LegacyTextureCreation_Is_Default_Dimension(height) && height == 0))
		return D3DERR_INVALIDCALL;
	if (filter != LEGACY_TEXTURE_FILTER_NONE &&
		filter != LEGACY_TEXTURE_FILTER_TRIANGLE &&
		filter != LEGACY_TEXTURE_FILTER_BOX)
		return D3DERR_INVALIDCALL;
	if (mipFilter != LEGACY_TEXTURE_FILTER_NONE &&
		mipFilter != LEGACY_TEXTURE_FILTER_TRIANGLE &&
		mipFilter != LEGACY_TEXTURE_FILTER_BOX)
		return D3DERR_INVALIDCALL;
	destinationFormat = format == D3DFMT_UNKNOWN ? D3DFMT_A8R8G8B8 : format;
	if (!Format_Is_Characterized(destinationFormat)) return D3DERR_NOTAVAILABLE;
	if (colorKey != 0)
	{
		const unsigned char keyBlue = (unsigned char)(colorKey & 0xff);
		const unsigned char keyGreen = (unsigned char)((colorKey >> 8) & 0xff);
		const unsigned char keyRed = (unsigned char)((colorKey >> 16) & 0xff);
		for (size_t i = 0; i < sourcePixels.size(); i += 4)
		{
			if (sourcePixels[i] == keyBlue && sourcePixels[i + 1] == keyGreen &&
				sourcePixels[i + 2] == keyRed) sourcePixels[i + 3] = 0;
		}
	}
	{
		size_t resizedBytes;
		if (!Checked_Multiply((size_t)destinationWidth, destinationHeight,
			&resizedBytes) || !Checked_Multiply(resizedBytes, 4U, &resizedBytes) ||
			resizedBytes > LEGACY_TEXTURE_MAX_BYTES) return D3DERR_NOTAVAILABLE;
		try { resizedPixels.resize(resizedBytes); }
		catch (...) { return D3DERR_OUTOFVIDEOMEMORY; }
	}
	if (!SurfaceBlit_Resample_A8R8G8B8(&sourcePixels[0], sourceWidth,
		sourceHeight, &resizedPixels[0], destinationWidth, destinationHeight,
		To_Surface_Filter(filter))) return D3DERR_NOTAVAILABLE;
	destinationLevels = mipLevels == 0 ? Full_Mip_Level_Count(destinationWidth,
		destinationHeight, 1) : mipLevels;
	if (destinationLevels == 0 || destinationLevels > Full_Mip_Level_Count(
		destinationWidth, destinationHeight, 1)) return D3DERR_INVALIDCALL;
	result = device->CreateTexture(destinationWidth, destinationHeight,
		destinationLevels, usage, destinationFormat, pool, &createdTexture);
	if (FAILED(result) || createdTexture == 0) return FAILED(result) ? result : E_FAIL;
	result = createdTexture->LockRect(0, &locked, 0, 0);
	if (SUCCEEDED(result)) textureLevelIsLocked = true;
	if (SUCCEEDED(result) && !SurfaceBlit_Write_A8R8G8B8(&resizedPixels[0],
		destinationWidth, destinationHeight, (unsigned char *)locked.pBits,
		locked.Pitch, destinationFormat)) result = D3DERR_NOTAVAILABLE;
	if (textureLevelIsLocked)
	{
		const HRESULT unlockResult = createdTexture->UnlockRect(0);
		textureLevelIsLocked = false;
		if (SUCCEEDED(result) && FAILED(unlockResult)) result = unlockResult;
	}
	if (SUCCEEDED(result) && destinationLevels > 1)
	{
		if (mipFilter != LEGACY_TEXTURE_FILTER_BOX &&
			mipFilter != LEGACY_TEXTURE_FILTER_NONE &&
			mipFilter != LEGACY_TEXTURE_FILTER_TRIANGLE)
			result = D3DERR_INVALIDCALL;
		else
			result = Generate_File_Mips(createdTexture, To_Surface_Filter(mipFilter));
	}
	if (FAILED(result))
	{
		createdTexture->Release();
		return result;
	}
	if (sourceInfo != 0)
	{
		sourceInfo->width = sourceWidth;
		sourceInfo->height = sourceHeight;
		sourceInfo->depth = 1;
		sourceInfo->mipLevels = destinationLevels;
		sourceInfo->format = destinationFormat;
	}
	*texture = createdTexture;
	return D3D_OK;
}
