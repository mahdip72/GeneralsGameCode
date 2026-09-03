/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** x86-only DDS compatibility adapter.  The title DDS reader is a CPU-side
** implementation and remains in the product source tree.  This separately
** compiled bridge and the legacy DDS callback registration cannot enter the
** native x64 renderer graph.
*/

#if !defined(_WIN64)

#include "DDSFileLegacy.h"
#include "../../../../GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ddsfile.h"
#include "dx8wrapper.h"
#include "legacytexturecompat.h"

#include <d3d8.h>

WW3DFormat D3DFormat_To_WW3DFormat(D3DFORMAT d3d_format);

#include <string.h>

namespace
{
static bool Legacy_DDS_Name_Is_Compatible(const char *filename)
{
	const size_t length = filename != 0 ? strlen(filename) : 0;
	return length >= 4 && filename[length - 4] == '.';
}

static bool Legacy_DDS_Format_To_Neutral(
	WW3DFormat format, LegacyTextureDDSFormat *neutralFormat)
{
	if (neutralFormat == 0) return false;
	switch (format)
	{
	case WW3D_FORMAT_DXT1: *neutralFormat = LEGACY_TEXTURE_DDS_DXT1; return true;
	case WW3D_FORMAT_DXT2: *neutralFormat = LEGACY_TEXTURE_DDS_DXT2; return true;
	case WW3D_FORMAT_DXT3: *neutralFormat = LEGACY_TEXTURE_DDS_DXT3; return true;
	case WW3D_FORMAT_DXT4: *neutralFormat = LEGACY_TEXTURE_DDS_DXT4; return true;
	case WW3D_FORMAT_DXT5: *neutralFormat = LEGACY_TEXTURE_DDS_DXT5; return true;
	default: return false;
	}
}

static const unsigned char *Legacy_DDS_Get_Level_Data(void *context,
	UINT level)
{
	DDSFileClass *dds = static_cast<DDSFileClass *>(context);
	if (dds == 0 || level >= dds->Get_Mip_Level_Count()) return 0;
	return dds->Get_Memory_Pointer(level);
}

static UINT Legacy_DDS_Get_Level_Size(void *context, UINT level)
{
	DDSFileClass *dds = static_cast<DDSFileClass *>(context);
	if (dds == 0 || level >= dds->Get_Mip_Level_Count()) return 0;
	return dds->Get_Level_Size(level);
}

static void Legacy_DDS_Release(void *context)
{
	delete static_cast<DDSFileClass *>(context);
}

static bool Legacy_DDS_Decode(const char *filename,
	LegacyTextureDDSImage *image)
{
	DDSFileClass *dds;
	LegacyTextureDDSFormat neutralFormat;
	if (image == 0) return false;
	memset(image, 0, sizeof(*image));
	if (!Legacy_DDS_Name_Is_Compatible(filename)) return false;
	dds = new DDSFileClass(filename, 0);
	if (dds == 0 || !dds->Is_Available() || dds->Get_Type() != DDS_TEXTURE ||
		!dds->Load())
	{
		delete dds;
		return false;
	}
	if (dds->Get_Full_Width() == 0 || dds->Get_Full_Height() == 0 ||
		dds->Get_Mip_Level_Count() == 0 ||
		!Legacy_DDS_Format_To_Neutral(dds->Get_Format(), &neutralFormat))
	{
		delete dds;
		return false;
	}
	image->context = dds;
	image->width = dds->Get_Full_Width();
	image->height = dds->Get_Full_Height();
	image->mipLevels = dds->Get_Mip_Level_Count();
	image->format = neutralFormat;
	image->getLevelData = &Legacy_DDS_Get_Level_Data;
	image->getLevelSize = &Legacy_DDS_Get_Level_Size;
	image->release = &Legacy_DDS_Release;
	return true;
}

struct Legacy_DDS_Compat_Registration
{
	Legacy_DDS_Compat_Registration()
	{
		LegacyTextureCreation_Register_DDS_Decode_Callback(&Legacy_DDS_Decode);
	}
};

Legacy_DDS_Compat_Registration g_legacyDDSCompatRegistration;
}

void Legacy_DDS_Copy_Level_To_Surface(DDSFileClass &dds_file,
	unsigned level,
	IDirect3DSurface8 *d3d_surface, const Vector3 &hsv_shift)
{
	WWASSERT(d3d_surface);
	// Verify that the destination surface size matches the source surface size.
	D3DSURFACE_DESC surface_desc;
	DX8_ErrorCode(d3d_surface->GetDesc(&surface_desc));

	// First lock the surface.
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(d3d_surface->LockRect(&locked_rect, nullptr, 0));

	dds_file.Copy_Level_To_Surface(
		level,
		D3DFormat_To_WW3DFormat(surface_desc.Format),
		surface_desc.Width,
		surface_desc.Height,
		reinterpret_cast<unsigned char *>(locked_rect.pBits),
		locked_rect.Pitch,
		hsv_shift);

	// Finally, unlock the surface.
	DX8_ErrorCode(d3d_surface->UnlockRect());
}

#endif
