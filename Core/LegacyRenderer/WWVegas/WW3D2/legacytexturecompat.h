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

#include <d3d8.h>

// The old file-loading wrapper used an all-in-one image service. Keep its
// public behavior explicit here: native resources are validated against the
// device caps, while files use the project's Targa decoder first, an optional
// title-owned DDS decoder second, and Windows Imaging Component for the other
// legacy image formats before the same CPU pixel/mip helpers used by the
// renderer.
enum LegacyTextureCreationKind
{
	LEGACY_TEXTURE_CREATION_CUBE,
	LEGACY_TEXTURE_CREATION_VOLUME,
	LEGACY_TEXTURE_CREATION_FILE
};

enum LegacyTextureCreationRoute
{
	LEGACY_TEXTURE_ROUTE_NATIVE_REQUIREMENTS,
	LEGACY_TEXTURE_ROUTE_PROJECT_DECODER
};

enum LegacyTextureFilter
{
	LEGACY_TEXTURE_FILTER_NONE = 0,
	LEGACY_TEXTURE_FILTER_TRIANGLE = 1,
	LEGACY_TEXTURE_FILTER_BOX = 2
};

static const UINT LEGACY_TEXTURE_DIMENSION_DEFAULT = 0xffffffffU;

struct LegacyTextureImageInfo
{
	UINT width;
	UINT height;
	UINT depth;
	UINT mipLevels;
	D3DFORMAT format;
};

// Title-specific builds already own a DDSFileClass implementation because it
// depends on each title's WW3D file factory and format table. Keep that
// decoder behind a neutral callback so the title adapter never owns native
// D3D8 allocation or upload. The Core compatibility boundary consumes the
// image synchronously while the decoder's context is valid.
enum LegacyTextureDDSFormat
{
	LEGACY_TEXTURE_DDS_DXT1 = 1,
	LEGACY_TEXTURE_DDS_DXT2 = 2,
	LEGACY_TEXTURE_DDS_DXT3 = 3,
	LEGACY_TEXTURE_DDS_DXT4 = 4,
	LEGACY_TEXTURE_DDS_DXT5 = 5
};

typedef const unsigned char *(*LegacyTextureDDSGetLevelData)(
	void *context, UINT level);
typedef UINT (*LegacyTextureDDSGetLevelSize)(void *context, UINT level);
typedef void (*LegacyTextureDDSRelease)(void *context);

struct LegacyTextureDDSImage
{
	void *context;
	UINT width;
	UINT height;
	UINT mipLevels;
	LegacyTextureDDSFormat format;
	LegacyTextureDDSGetLevelData getLevelData;
	LegacyTextureDDSGetLevelSize getLevelSize;
	// The Core uploader owns the image only for the duration of the callback;
	// it invokes release after the native texture has been created or declined.
	LegacyTextureDDSRelease release;
};

typedef bool (*LegacyTextureDDSDecodeCallback)(
	const char *filename, LegacyTextureDDSImage *image);

struct LegacyTextureCreationDescriptor
{
	LegacyTextureCreationKind kind;
	UINT width;
	UINT height;
	UINT depth;
	UINT mipLevels;
	DWORD usage;
	D3DFORMAT format;
	D3DPOOL pool;
};

LegacyTextureCreationRoute LegacyTextureCreation_Get_Route(
	LegacyTextureCreationKind kind);

// Register the title-owned DDS/DXTC decoder during title-library startup. A
// null callback restores the Core-only fallback path. Registration is
// intentionally a simple owner-thread/startup contract and is not a per-file
// synchronization point.
void LegacyTextureCreation_Register_DDS_Decode_Callback(
	LegacyTextureDDSDecodeCallback callback);
bool LegacyTextureCreation_Has_DDS_Decode_Callback();

bool LegacyTextureCreation_Build_Cube_Descriptor(
	UINT size,
	UINT mipLevels,
	DWORD usage,
	D3DFORMAT format,
	D3DPOOL pool,
	LegacyTextureCreationDescriptor *descriptor);

bool LegacyTextureCreation_Build_Volume_Descriptor(
	UINT width,
	UINT height,
	UINT depth,
	UINT mipLevels,
	DWORD usage,
	D3DFORMAT format,
	D3DPOOL pool,
	LegacyTextureCreationDescriptor *descriptor);

bool LegacyTextureCreation_Validate_Descriptor_For_Caps(
	const LegacyTextureCreationDescriptor &descriptor,
	const D3DCAPS8 &caps);

bool LegacyTextureCreation_Is_Default_Dimension(UINT dimension);

bool LegacyTextureCreation_Is_Characterized_File_Format(D3DFORMAT format);

HRESULT LegacyTextureCreation_Create_Cube(
	IDirect3DDevice8 *device,
	UINT size,
	UINT mipLevels,
	DWORD usage,
	D3DFORMAT format,
	D3DPOOL pool,
	IDirect3DCubeTexture8 **texture);

HRESULT LegacyTextureCreation_Create_Volume(
	IDirect3DDevice8 *device,
	UINT width,
	UINT height,
	UINT depth,
	UINT mipLevels,
	DWORD usage,
	D3DFORMAT format,
	D3DPOOL pool,
	IDirect3DVolumeTexture8 **texture);

// Decode a Targa, optional title-owned DDS, or Windows Imaging
// Component-supported image file, then create the requested native texture
// and CPU-generated levels.
HRESULT LegacyTextureCreation_Create_File_Texture(
	IDirect3DDevice8 *device,
	const char *filename,
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
	PALETTEENTRY *palette,
	IDirect3DTexture8 **texture);
