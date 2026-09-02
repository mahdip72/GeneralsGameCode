/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// 08/06/02 KM Added cube map and volume texture support

#include "ddsfile.h"
#include "WWLib/ffactory.h"
#include "WWLib/bufffile.h"

#include <string.h>

namespace
{
// These values are part of the DDS file format's legacy CAPS2 field.  They
// are deliberately kept local instead of importing DirectDraw headers into
// the asset loader.
const unsigned DDS_FILE_CAPS2_CUBEMAP = 0x00000200;
const unsigned DDS_FILE_CAPS2_VOLUME = 0x00200000;

const unsigned DDS_FILE_FOURCC_DXT1 = 0x31545844;
const unsigned DDS_FILE_FOURCC_DXT2 = 0x32545844;
const unsigned DDS_FILE_FOURCC_DXT3 = 0x33545844;
const unsigned DDS_FILE_FOURCC_DXT4 = 0x34545844;
const unsigned DDS_FILE_FOURCC_DXT5 = 0x35545844;

static WW3DFormat Legacy_DDS_FourCC_To_WW3DFormat(unsigned fourCC)
{
	switch (fourCC)
	{
	case DDS_FILE_FOURCC_DXT1: return WW3D_FORMAT_DXT1;
	case DDS_FILE_FOURCC_DXT2: return WW3D_FORMAT_DXT2;
	case DDS_FILE_FOURCC_DXT3: return WW3D_FORMAT_DXT3;
	case DDS_FILE_FOURCC_DXT4: return WW3D_FORMAT_DXT4;
	case DDS_FILE_FOURCC_DXT5: return WW3D_FORMAT_DXT5;
	default: return WW3D_FORMAT_UNKNOWN;
	}
}

bool Is_Supported_DDS_Format(WW3DFormat format)
{
	switch (format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		return true;
	default:
		return false;
	}
}

static unsigned Legacy_DDS_Level_Dimension(unsigned dimension, unsigned level)
{
	while (level != 0)
	{
		dimension = dimension > 1 ? dimension / 2 : 1;
		--level;
	}
	return dimension == 0 ? 1 : dimension;
}

static unsigned Legacy_DDS_Max_Mip_Levels
(
	unsigned width,
	unsigned height,
	unsigned depth,
	DDSType type
)
{
	unsigned maximum_dimension=width>height ? width : height;
	if (type==DDS_VOLUME && depth>maximum_dimension) maximum_dimension=depth;

	unsigned maximum_levels=1;
	while (maximum_dimension>1)
	{
		maximum_dimension/=2;
		++maximum_levels;
	}
	return maximum_levels;
}

}

// ----------------------------------------------------------------------------

DDSFileClass::DDSFileClass(const char* name,unsigned reduction_factor)
	:
	DDSMemory(nullptr),
	DDSMemorySize(0),
	Width(0),
	Height(0),
	Depth(0),
	FullWidth(0),
	FullHeight(0),
	FullDepth(0),
	LevelSizes(nullptr),
	LevelOffsets(nullptr),
	MipLevels(0),
	ReductionFactor(reduction_factor),
	Format(WW3D_FORMAT_UNKNOWN),
	Type(DDS_TEXTURE),
	DateTime(0),
	CubeFaceSize(0),
	CubeFaceDataOffset(0)
{
	Name[0] = 0;
	// A null name constructs a private memory-only decoder. No factory call.
	if (name == nullptr) return;
	strlcpy(Name,name,sizeof(Name));
	// The name could be given in .tga or .dds format, so ensure we're opening .dds...
	int len=strlen(Name);
	Name[len-3]='d';
	Name[len-2]='d';
	Name[len-1]='s';

	file_auto_ptr file(_TheFileFactory,Name);
	if (!file->Is_Available())
	{
		return;
	}

	int result=file->Open();
	if (!result)
	{
		WWASSERT("File would not open");
		return;
	}

	DateTime=file->Get_Date_Time();
	char header[4];

	unsigned read_bytes=file->Read(header,4);
	if (read_bytes!=4 || header[0]!='D' || header[1]!='D' || header[2]!='S' || header[3]!=' ')
	{
		WWASSERT_PRINT(0,"File loading failed: invalid DDS magic");
		return;
	}
	// Now, we read DDSURFACEDESC2 defining the compressed data
	read_bytes=file->Read(&SurfaceDesc,sizeof(LegacyDDSURFACEDESC2));
	// Verify the structure size matches the read size
	if (read_bytes!=sizeof(LegacyDDSURFACEDESC2) ||
		SurfaceDesc.Size!=sizeof(LegacyDDSURFACEDESC2))
	{
		StringClass tmp(0,true);
		tmp.Format("File %s loading failed.\nTried to read %d bytes, got %d. (SurfDesc.size=%d)",name,sizeof(LegacyDDSURFACEDESC2),read_bytes,SurfaceDesc.Size);
		WWASSERT_PRINT(0,tmp.str());
		return;
	}

	file->Close();
	Initialize_Header();
}

void DDSFileClass::Initialize_Header()
{
	Format=Legacy_DDS_FourCC_To_WW3DFormat(SurfaceDesc.PixelFormat.FourCC);
	if (!Is_Supported_DDS_Format(Format))
	{
		WWASSERT_PRINT(0,"File loading failed: unsupported DDS texture format");
		return;
	}

	// Check texture type, normal, cube or volume.  A zero width or height is
	// never a valid compressed surface.  Depth is optional for ordinary DDS
	// textures, but a volume texture must carry a real slice count.
	if (SurfaceDesc.Caps.Caps2&DDS_FILE_CAPS2_CUBEMAP)
	{
		Type=DDS_CUBEMAP;
	}
	else if (SurfaceDesc.Caps.Caps2&DDS_FILE_CAPS2_VOLUME)
	{
		Type=DDS_VOLUME;
	}
	if (SurfaceDesc.Width==0 || SurfaceDesc.Height==0 ||
		(Type==DDS_VOLUME && SurfaceDesc.Depth==0))
	{
		return;
	}

	unsigned source_mip_levels=SurfaceDesc.MipMapCount;
	if (source_mip_levels==0) source_mip_levels=1;
	const unsigned maximum_mip_levels=Legacy_DDS_Max_Mip_Levels(
		SurfaceDesc.Width,SurfaceDesc.Height,SurfaceDesc.Depth,Type);
	if (source_mip_levels>maximum_mip_levels)
		source_mip_levels=maximum_mip_levels;
	if (ReductionFactor>=source_mip_levels)
		ReductionFactor=source_mip_levels-1;

	const unsigned available_mip_levels=source_mip_levels-ReductionFactor;
	MipLevels=available_mip_levels>2 ? available_mip_levels-2 : 1;

	// Drop the two lowest miplevels!
	FullWidth=SurfaceDesc.Width;
	FullHeight=SurfaceDesc.Height;
	FullDepth=SurfaceDesc.Depth;
	Width=Legacy_DDS_Level_Dimension(SurfaceDesc.Width,ReductionFactor);
	Height=Legacy_DDS_Level_Dimension(SurfaceDesc.Height,ReductionFactor);
	Depth=Type==DDS_VOLUME ?
		Legacy_DDS_Level_Dimension(SurfaceDesc.Depth,ReductionFactor) :
		SurfaceDesc.Depth;

	unsigned level_offset=0;

	LevelSizes=new unsigned[MipLevels];
	LevelOffsets=new unsigned[MipLevels];
	for (unsigned level=0;level<MipLevels;++level)
	{
		const unsigned sourceLevel=ReductionFactor+level;
		const unsigned levelWidth=Legacy_DDS_Level_Dimension(
			SurfaceDesc.Width,sourceLevel);
		const unsigned levelHeight=Legacy_DDS_Level_Dimension(
			SurfaceDesc.Height,sourceLevel);
		const unsigned levelDepth=Legacy_DDS_Level_Dimension(
			SurfaceDesc.Depth,sourceLevel);
		unsigned level_size=Calculate_DXTC_Surface_Size(
			levelWidth,levelHeight,Format);
		if (level_size==0 ||
			(Type==DDS_VOLUME &&
			(levelDepth==0 || level_size>((unsigned)-1)/levelDepth)))
		{
			delete[] LevelSizes;
			delete[] LevelOffsets;
			LevelSizes=0;
			LevelOffsets=0;
			MipLevels=0;
			return;
		}
		if (Type==DDS_VOLUME) level_size*=levelDepth;
		if (level_offset>((unsigned)-1)-level_size)
		{
			delete[] LevelSizes;
			delete[] LevelOffsets;
			LevelSizes=0;
			LevelOffsets=0;
			MipLevels=0;
			return;
		}
		LevelSizes[level]=level_size;
		LevelOffsets[level]=level_offset;
		level_offset+=level_size;
	}

	if (Type==DDS_CUBEMAP)
	{
		// DDS cube data is face-major.  Keep the complete source-face stride
		// and separately record the bytes skipped before the retained first
		// mip; otherwise reduction would make face N point into face N-1.
		for (unsigned source_level=0; source_level<source_mip_levels; ++source_level)
		{
			const unsigned levelWidth=Legacy_DDS_Level_Dimension(
				SurfaceDesc.Width,source_level);
			const unsigned levelHeight=Legacy_DDS_Level_Dimension(
				SurfaceDesc.Height,source_level);
			const unsigned source_level_size=Calculate_DXTC_Surface_Size(
				levelWidth,levelHeight,Format);
			if (source_level_size==0 ||
				CubeFaceSize>((unsigned)-1)-source_level_size)
			{
				delete[] LevelSizes;
				delete[] LevelOffsets;
				LevelSizes=0;
				LevelOffsets=0;
				MipLevels=0;
				return;
			}
			CubeFaceSize+=source_level_size;
			if (source_level<ReductionFactor)
			{
				if (CubeFaceDataOffset>((unsigned)-1)-source_level_size)
				{
					delete[] LevelSizes;
					delete[] LevelOffsets;
					LevelSizes=0;
					LevelOffsets=0;
					MipLevels=0;
					return;
				}
				CubeFaceDataOffset+=source_level_size;
			}
		}
	}
}

bool DDSFileClass::Set_Memory_Header(const unsigned char *bytes, size_t size)
{
	if (bytes == nullptr || size < 128 || LevelSizes != nullptr || DDSMemory != nullptr ||
		memcmp(bytes, "DDS ", 4) != 0) return false;
	memcpy(&SurfaceDesc, bytes + 4, sizeof(SurfaceDesc));
	if (SurfaceDesc.Size != sizeof(SurfaceDesc)) return false;
	Initialize_Header();
	return Is_Available();
}

bool DDSFileClass::Load_From_Memory(const unsigned char *bytes, size_t byteCount)
{
	if (!Is_Available() || DDSMemory != nullptr || bytes == nullptr || byteCount < 128 ||
		byteCount > 0x7fffffffU || memcmp(bytes, "DDS ", 4) != 0 ||
		memcmp(bytes + 4, &SurfaceDesc, sizeof(SurfaceDesc)) != 0) return false;
	unsigned offset = 128;
	if (Type != DDS_CUBEMAP)
	{
		for (unsigned i = 0; i < ReductionFactor; ++i)
		{
			const unsigned w = Legacy_DDS_Level_Dimension(SurfaceDesc.Width, i);
			const unsigned h = Legacy_DDS_Level_Dimension(SurfaceDesc.Height, i);
			const unsigned d = Type == DDS_VOLUME ? Legacy_DDS_Level_Dimension(SurfaceDesc.Depth, i) : 1;
			const unsigned levelSize = Calculate_DXTC_Surface_Size(w, h, Format);
			if (levelSize == 0 || d == 0 || levelSize > ((unsigned)-1) / d ||
				offset > byteCount || levelSize * d > byteCount - offset) return false;
			offset += levelSize * d;
		}
	}
	if (MipLevels == 0 || LevelOffsets[MipLevels - 1] > ((unsigned)-1) - LevelSizes[MipLevels - 1]) return false;
	unsigned required = LevelOffsets[MipLevels - 1] + LevelSizes[MipLevels - 1];
	if (Type == DDS_CUBEMAP)
	{
		if (CubeFaceSize == 0 || CubeFaceSize > ((unsigned)-1) / 6U) return false;
		required = CubeFaceSize * 6U;
	}
	if (offset > byteCount || required > byteCount - offset) return false;
	const unsigned size = static_cast<unsigned>(byteCount - offset);
	DDSMemory = new unsigned char[size];
	DDSMemorySize = size;
	memcpy(DDSMemory, bytes + offset, size);
	return true;
}

// ----------------------------------------------------------------------------

DDSFileClass::~DDSFileClass()
{
	delete[] DDSMemory;
	delete[] LevelSizes;
	delete[] LevelOffsets;
}

unsigned DDSFileClass::Get_Width(unsigned level) const
{
	WWASSERT(level<MipLevels);
	unsigned width=Legacy_DDS_Level_Dimension(Width,level);
	if (width<4) width=4;
	return width;
}

unsigned DDSFileClass::Get_Height(unsigned level) const
{
	WWASSERT(level<MipLevels);
	unsigned height=Legacy_DDS_Level_Dimension(Height,level);
	if (height<4) height=4;
	return height;
}

unsigned DDSFileClass::Get_Depth(unsigned level) const
{
	WWASSERT(level<MipLevels);
	return Legacy_DDS_Level_Dimension(Depth,level);
}

const unsigned char* DDSFileClass::Get_Memory_Pointer(unsigned level) const
{
	WWASSERT(level<MipLevels);
	if (DDSMemory==nullptr || level>=MipLevels ||
		LevelOffsets[level]>=DDSMemorySize)
		return nullptr;
	return DDSMemory+LevelOffsets[level];
}

unsigned DDSFileClass::Get_Level_Size(unsigned level) const
{
	WWASSERT(level<MipLevels);
	return LevelSizes[level];
}

// For some reason DX-Tex tool doesn't fill the surface size field, so we need to calculate it...
unsigned DDSFileClass::Calculate_DXTC_Surface_Size
(
	unsigned width,
	unsigned height,
	WW3DFormat format
)
{
	const size_t maximum_size=(size_t)-1;
	if ((size_t)width>maximum_size-3U || (size_t)height>maximum_size-3U)
		return 0;
	const size_t block_columns=((size_t)width+3U)/4U;
	const size_t block_rows=((size_t)height+3U)/4U;
	const size_t bytes_per_block=format==WW3D_FORMAT_DXT1 ? 8U : 16U;
	if (block_columns==0 || block_rows==0 ||
		block_columns>maximum_size/block_rows) return 0;
	const size_t block_count=block_columns*block_rows;
	if (block_count>maximum_size/bytes_per_block ||
		block_count*bytes_per_block>(unsigned)-1) return 0;
	unsigned level_size=(unsigned)(block_count*bytes_per_block);
	switch (format)
	{
	case WW3D_FORMAT_DXT1:
		break;
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		break;
	default:
		return 0;
	}

	return level_size;
}

// ----------------------------------------------------------------------------

bool DDSFileClass::Load()
{
	if (DDSMemory) return false;
	if (!LevelSizes || !LevelOffsets) return false;

	file_auto_ptr file(_TheFileFactory,Name);
	if (!file->Is_Available())
	{
		return false;
	}

	file->Open();
	// Data size is file size minus the header and info block.  Validate before
	// subtracting so a truncated/corrupt DDS cannot wrap an unsigned size and
	// turn into a huge allocation.
	const unsigned file_size=file->Size();
	if (file_size<SurfaceDesc.Size+4)
	{
		file->Close();
		return false;
	}
	unsigned size=file_size-SurfaceDesc.Size-4;

	if (!size || size>=0x80000000)
	{
		file->Close();
		return false;
	}

	// Skip mip levels if reduction factor is not zero.  Cube maps are stored
	// face-major, so their reduced data cannot be skipped with one global seek;
	// retain the complete payload and apply CubeFaceDataOffset per face below.
	unsigned skipped_offset=0;
	if (Type!=DDS_CUBEMAP)
	{
		for (unsigned i=0;i<ReductionFactor;++i)
		{
			const unsigned levelWidth=Legacy_DDS_Level_Dimension(
				SurfaceDesc.Width,i);
			const unsigned levelHeight=Legacy_DDS_Level_Dimension(
				SurfaceDesc.Height,i);
			const unsigned levelDepth=Legacy_DDS_Level_Dimension(
				SurfaceDesc.Depth,i);
			unsigned level_size=Calculate_DXTC_Surface_Size(
				levelWidth,levelHeight,Format);
			if (level_size==0 ||
				(Type==DDS_VOLUME &&
				(levelDepth==0 || level_size>((unsigned)-1)/levelDepth)))
			{
				file->Close();
				return false;
			}
			if (Type==DDS_VOLUME) level_size*=levelDepth;
			if (skipped_offset>((unsigned)-1)-level_size)
			{
				file->Close();
				return false;
			}
			if (size<level_size)
			{
				file->Close();
				return false;
			}
			skipped_offset+=level_size;
			size-=level_size;
		}
	}

	// Skip the header and info block and possible unused mip levels
	if (MipLevels==0 ||
		LevelOffsets[MipLevels-1]>((unsigned)-1)-LevelSizes[MipLevels-1])
	{
		file->Close();
		return false;
	}
	unsigned required_size=LevelOffsets[MipLevels-1]+LevelSizes[MipLevels-1];
	if (Type==DDS_CUBEMAP)
	{
		if (CubeFaceSize==0 || CubeFaceSize>((unsigned)-1)/6U)
		{
			file->Close();
			return false;
		}
		required_size=CubeFaceSize*6U;
	}
	if (Type==DDS_CUBEMAP && size<required_size)
	{
		file->Close();
		return false;
	}
	if (Type!=DDS_CUBEMAP &&
		(size<required_size || skipped_offset>((unsigned)-1)-SurfaceDesc.Size-4))
	{
		file->Close();
		return false;
	}
	unsigned seek_size=file->Seek(SurfaceDesc.Size+4+skipped_offset);
	if (seek_size!=SurfaceDesc.Size+4+skipped_offset)
	{
		file->Close();
		return false;
	}

	// Allocate memory for the data excluding the headers.
	DDSMemory=MSGW3DNEWARRAY("DDSMemory") unsigned char[size];
	DDSMemorySize=size;
	// Read data.
	unsigned read_size=file->Read(DDSMemory,size);
	if (read_size!=size)
	{
		delete[] DDSMemory;
		DDSMemory=0;
		DDSMemorySize=0;
		file->Close();
		return false;
	}
	file->Close();
	return true;
}

// ----------------------------------------------------------------------------

WWINLINE static unsigned RGB565_To_ARGB8888(unsigned short rgb)
{
	unsigned rgba=0;
	rgba|=unsigned(rgb&0x001f)<<3;
	rgba|=unsigned(rgb&0x07e0)<<5;
	rgba|=unsigned(rgb&0xf800)<<8;
	return rgba;
}

WWINLINE static unsigned short ARGB8888_To_RGB565(unsigned argb_)
{
	unsigned char* argb=(unsigned char*)&argb_;
	unsigned short rgb;
	rgb=((argb[2])&0xf8)<<8;
	rgb|=((argb[1])&0xfc)<<3;
	rgb|=((argb[0])&0xf8)>>3;
	return rgb;
}

static unsigned Legacy_DDS_Block_Bytes(WW3DFormat format)
{
	if (format==WW3D_FORMAT_DXT1) return 8;
	if (format==WW3D_FORMAT_DXT2 || format==WW3D_FORMAT_DXT3 ||
		format==WW3D_FORMAT_DXT4 || format==WW3D_FORMAT_DXT5) return 16;
	return 0;
}

static bool Legacy_DDS_Destination_Pitch_Is_Valid(
	WW3DFormat format,unsigned width,unsigned pitch)
{
	const unsigned block_bytes=Legacy_DDS_Block_Bytes(format);
	if (block_bytes!=0)
	{
		if (width>((unsigned)-1)-3U) return false;
		const unsigned blocks=(width+3U)/4U;
		return blocks<=((unsigned)-1)/block_bytes &&
			pitch>=blocks*block_bytes;
	}
	const unsigned bytes_per_pixel=Get_Bytes_Per_Pixel(format);
	return bytes_per_pixel!=0 && width<=((unsigned)-1)/bytes_per_pixel &&
		pitch>=width*bytes_per_pixel;
}

static bool Legacy_DDS_Copy_Compressed_Rows(
	const unsigned char *source,unsigned width,unsigned height,
	WW3DFormat format,unsigned char *destination,unsigned destination_pitch)
{
	const unsigned block_bytes=Legacy_DDS_Block_Bytes(format);
	if (source==0 || destination==0 || block_bytes==0 ||
		!Legacy_DDS_Destination_Pitch_Is_Valid(
			format,width,destination_pitch)) return false;
	const unsigned row_bytes=((width+3U)/4U)*block_bytes;
	const unsigned rows=(height+3U)/4U;
	for (unsigned row=0;row<rows;++row)
	{
		memcpy(destination+row*destination_pitch,source+row*row_bytes,row_bytes);
	}
	return true;
}

static void Legacy_DDS_RGB_To_HSV(Vector3 &hsv,const Vector3 &rgb)
{
	float max=WWMath::Max(rgb.X,rgb.Y);
	max=WWMath::Max(max,rgb.Z);
	float min=WWMath::Min(rgb.X,rgb.Y);
	min=WWMath::Min(min,rgb.Z);
	hsv.Z=max;
	hsv.Y=(max!=0.0f)?((max-min)/max):0.0f;
	if (hsv.Y==0.0f) hsv.X=-1.0f;
	else
	{
		const float delta=max-min;
		if (rgb.X==max) hsv.X=(rgb.Y-rgb.Z)/delta;
		else if (rgb.Y==max) hsv.X=2.0f+(rgb.Z-rgb.X)/delta;
		else hsv.X=4.0f+(rgb.X-rgb.Y)/delta;
		hsv.X*=60.0f;
		if (hsv.X<0.0f) hsv.X+=360.0f;
	}
}

static void Legacy_DDS_HSV_To_RGB(Vector3 &rgb,const Vector3 &hsv)
{
	float h=hsv.X;
	const float s=hsv.Y;
	const float v=hsv.Z;
	if (s==0.0f)
	{
		rgb.Set(v,v,v);
		return;
	}
	if (h==360.0f) h=0.0f;
	h/=60.0f;
	const int i=WWMath::Floor(h);
	const float f=h-i;
	const float p=v*(1.0f-s);
	const float q=v*(1.0f-(s*f));
	const float t=v*(1.0f-(s*(1.0f-f)));
	switch (i)
	{
	case 0: rgb.Set(v,t,p); break;
	case 1: rgb.Set(q,v,p); break;
	case 2: rgb.Set(p,v,t); break;
	case 3: rgb.Set(p,q,v); break;
	case 4: rgb.Set(t,p,v); break;
	case 5: rgb.Set(v,p,q); break;
	}
}

static void Recolor(unsigned &rgba,const Vector3 &hsv_shift)
{
	const float alpha=((rgba&0xff000000U)>>24)/255.0f;
	Vector3 rgb(
		((rgba&0x00ff0000U)>>16)/255.0f,
		((rgba&0x0000ff00U)>>8)/255.0f,
		(rgba&0x000000ffU)/255.0f);
	Vector3 hsv;
	Legacy_DDS_RGB_To_HSV(hsv,rgb);
	if (hsv.X<0.0f) hsv+=Vector3(0.0f,0.0f,hsv_shift.Z);
	else hsv+=hsv_shift;
	if (hsv.X<0.0f) hsv.X+=360.0f;
	if (hsv.X>360.0f) hsv.X-=360.0f;
	hsv.Y=WWMath::Clamp(hsv.Y,0.0f,1.0f);
	hsv.Z=WWMath::Clamp(hsv.Z,0.0f,1.0f);
	Legacy_DDS_HSV_To_RGB(rgb,hsv);
	rgba=(unsigned)rgb.Convert_To_ARGB(alpha);
}

static void Legacy_DDS_Write_B8G8R8A8(
	unsigned char *destination,WW3DFormat format,const unsigned &argb_value)
{
	const unsigned char *argb=(const unsigned char *)&argb_value;
	switch (format)
	{
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
		*(unsigned *)destination=argb_value;
		break;
	case WW3D_FORMAT_R8G8B8:
		destination[0]=argb[0]; destination[1]=argb[1]; destination[2]=argb[2];
		break;
	case WW3D_FORMAT_A4R4G4B4:
		*(unsigned short *)destination=(unsigned short)(
			((argb[3]&0xf0)<<8)|((argb[2]&0xf0)<<4)|
			(argb[1]&0xf0)|((argb[0]&0xf0)>>4));
		break;
	case WW3D_FORMAT_A1R5G5B5:
		*(unsigned short *)destination=(unsigned short)(
			(argb[3]?0x8000:0)|((argb[2]&0xf8)<<7)|
			((argb[1]&0xf8)<<2)|((argb[0]&0xf8)>>3));
		break;
	case WW3D_FORMAT_R5G6B5:
		*(unsigned short *)destination=(unsigned short)(
			((argb[2]&0xf8)<<8)|((argb[1]&0xfc)<<3)|((argb[0]&0xf8)>>3));
		break;
	case WW3D_FORMAT_R3G3B2:
		destination[0]=(unsigned char)(
			(argb[2]&0xe0)|((argb[1]&0xe0)>>3)|((argb[0]&0xc0)>>6));
		break;
	case WW3D_FORMAT_L8:
		destination[0]=(unsigned char)(((unsigned)argb[0]*0x1275U+
			(unsigned)argb[1]*0xb725U+(unsigned)argb[2]*0x3666U)>>16);
		break;
	case WW3D_FORMAT_A8:
		destination[0]=argb[0];
		break;
	default:
		WWASSERT(0);
		break;
	}
}

bool DDSFileClass::Copy_4x4_Block_From_Memory_Clipped(
	const unsigned char* source_memory,
	unsigned char* dest_ptr,
	unsigned dest_pitch,
	WW3DFormat dest_format,
	unsigned level,
	unsigned source_x,
	unsigned source_y,
	unsigned dest_width,
	unsigned dest_height,
	const Vector3& hsv_shift) const
{
	const unsigned remaining_width=dest_width-source_x;
	const unsigned remaining_height=dest_height-source_y;
	if (remaining_width>=4 && remaining_height>=4)
	{
		return Get_4x4_Block_From_Memory(source_memory,dest_ptr,dest_pitch,
			dest_format,level,source_x,source_y,hsv_shift);
	}

	const unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_format);
	if (dest_bpp==0 || dest_bpp>4)
	{
		WWASSERT_PRINT(0,"Unsupported partial DDS block destination format");
		return false;
	}
	unsigned char temporary_pixels[4*4*4];
	const unsigned temporary_pitch=dest_bpp*4;
	const bool contains_alpha=Get_4x4_Block_From_Memory(
		source_memory,temporary_pixels,temporary_pitch,dest_format,
		level,source_x,source_y,hsv_shift);
	const unsigned copy_width=remaining_width<4 ? remaining_width : 4;
	const unsigned copy_height=remaining_height<4 ? remaining_height : 4;
	for (unsigned row=0;row<copy_height;++row)
	{
		memcpy(dest_ptr+row*dest_pitch,temporary_pixels+row*temporary_pitch,
			copy_width*dest_bpp);
	}
	return contains_alpha;
}


// ----------------------------------------------------------------------------
//
// Copy mipmap level to D3D surface. The copying is performed using another
// Copy_Level_To_Surface function (see below).
//
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//
// Copy one mipmap level of texture to a memory surface. Surface type conversion
// is performed if the destination is of different format. Scaling will be done
// one of these days as well. Conversions between different types of compressed
// surfaces are not performed and scaling of compressed surfaces is also not
// possible.
//
// ----------------------------------------------------------------------------

void DDSFileClass::Copy_Level_To_Surface
(
	unsigned level,
	WW3DFormat dest_format,
	unsigned dest_width,
	unsigned dest_height,
	unsigned char* dest_surface,
	unsigned dest_pitch,
	const Vector3& hsv_shift
)
{
	WWASSERT(DDSMemory);
	WWASSERT(dest_surface);

	if (!DDSMemory || !Get_Memory_Pointer(level))
	{
		WWASSERT_PRINT(DDSMemory,"Surface mip level pointer is missing");
		return;
	}
	if (!Legacy_DDS_Destination_Pitch_Is_Valid(
		dest_format,dest_width,dest_pitch))
	{
		WWASSERT_PRINT(0,"Surface destination pitch is invalid");
		return;
	}

	// If the format and size is a match just copy the contents
	bool has_hsv_shift = hsv_shift[0]!=0.0f || hsv_shift[1]!=0.0f || hsv_shift[2]!=0.0f;
	if (dest_format==Format && dest_width==Get_Width(level) && dest_height==Get_Height(level)) {
		// If hue shift, we can't just copy...
		if (has_hsv_shift) {
			if (Format==WW3D_FORMAT_DXT1) {
				const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_Memory_Pointer(level));
				for (unsigned y=0;y<dest_height;y+=4) {
					const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*2;
					unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
					for (unsigned x=0;x<dest_width;x+=4) {
						unsigned cols=*src_ptr++;		// Bytes 1-4 of color block
						unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
						unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
						col0=ARGB8888_To_RGB565(col0);
						col1=ARGB8888_To_RGB565(col1);
						cols=unsigned(col0)<<16|col1;
						*dest_ptr++=cols;

						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
					}
				}
			}
			else if (Format==WW3D_FORMAT_DXT5) {
				const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_Memory_Pointer(level));
				for (unsigned y=0;y<dest_height;y+=4) {
					const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*4;
					unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
					for (unsigned x=0;x<dest_width;x+=4) {
						*dest_ptr++=*src_ptr++;		// Bytes 1-4 of alpha block
						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of alpha block
						unsigned cols=*src_ptr++;		// Bytes 1-4 of color block
						unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
						unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
						col0=ARGB8888_To_RGB565(col0);
						col1=ARGB8888_To_RGB565(col1);
						cols=unsigned(col0)<<16|col1;
						*dest_ptr++=cols;

						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
					}
				}
			}
			else {
				WWASSERT(0);
			}

		}
		else {
			if (!Legacy_DDS_Copy_Compressed_Rows(Get_Memory_Pointer(level),
				dest_width,dest_height,Format,dest_surface,dest_pitch))
				WWASSERT_PRINT(0,"Surface compressed copy failed");
		}
	}
	else {
		// If size matches, copy each pixel linearly with color space conversion
		if (dest_width==Get_Width(level) && dest_height==Get_Height(level)) {
			// An exception here - if the source format is DXT1 and the destination
			// is DXT2, just copy the contents and create an empty alpha channel.
			// This is needed on NVidia cards that have problems with DXT1 compression.
			if (Format==WW3D_FORMAT_DXT1 && dest_format==WW3D_FORMAT_DXT2) {
				// If hue shift, we can't just copy...
				if (has_hsv_shift) {
					const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_Memory_Pointer(level));
					for (unsigned y=0;y<dest_height;y+=4) {
						const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*2;
						unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
						for (unsigned x=0;x<dest_width;x+=4) {
							*dest_ptr++=0xffffffff;		// Bytes 1-4 of alpha block
							*dest_ptr++=0xffffffff;		// Bytes 5-8 of alpha block
//							*dest_ptr++=*src_ptr++;		// Bytes 1-4 of color block

							unsigned cols=*src_ptr++;	// Bytes 1-4 of color block
							unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
							unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
							Recolor(col0,hsv_shift);
							Recolor(col1,hsv_shift);
							col0=ARGB8888_To_RGB565(col0);
							col1=ARGB8888_To_RGB565(col1);
							cols=unsigned(col0)<<16|col1;
							*dest_ptr++=cols;

							*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
						}
					}
				}
				else {
					const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_Memory_Pointer(level));
					for (unsigned y=0;y<dest_height;y+=4) {
						const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*2;
						unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
						for (unsigned x=0;x<dest_width;x+=4) {
							*dest_ptr++=0xffffffff;		// Bytes 1-4 of alpha block
							*dest_ptr++=0xffffffff;		// Bytes 5-8 of alpha block
							*dest_ptr++=*src_ptr++;		// Bytes 1-4 of color block
							*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
						}
					}
				}
			}
			else {
				unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_format);

				// Copy 4x4 block at a time
				bool contains_alpha=false;
				for (unsigned y=0;y<dest_height;y+=4) {
					unsigned char* dest_ptr=dest_surface;
					dest_ptr+=y*dest_pitch;
					for (unsigned x=0;x<dest_width;x+=4,dest_ptr+=dest_bpp*4) {
						contains_alpha|=Copy_4x4_Block_From_Memory_Clipped(
							Get_Memory_Pointer(level),dest_ptr,dest_pitch,
							dest_format,level,x,y,dest_width,dest_height,hsv_shift);
					}
				}
				if (Format==WW3D_FORMAT_DXT1 && contains_alpha) {
					WWDEBUG_SAY(("Warning: DXT1 format should not contain alpha information - file %s",Name));
				}
			}
		}
	}
}

// cube map
const unsigned char* DDSFileClass::Get_CubeMap_Memory_Pointer
(
	unsigned int face,
	unsigned int level
) const
{
	if (DDSMemory==nullptr || face>=6 || level>=MipLevels ||
		CubeFaceSize==0 || CubeFaceSize>DDSMemorySize/6U)
		return nullptr;
	const unsigned face_offset=CubeFaceSize*face;
	if (face_offset>DDSMemorySize ||
		CubeFaceDataOffset>DDSMemorySize-face_offset)
		return nullptr;
	const unsigned level_base=face_offset+CubeFaceDataOffset;
	if (LevelOffsets[level]>=DDSMemorySize-level_base)
		return nullptr;
	return DDSMemory+level_base+LevelOffsets[level];
}


void DDSFileClass::Copy_CubeMap_Level_To_Surface
(
	unsigned face,
	unsigned level,
	WW3DFormat dest_format,
	unsigned dest_width,
	unsigned dest_height,
	unsigned char* dest_surface,
	unsigned dest_pitch,
	const Vector3& hsv_shift
)
{
	WWASSERT(DDSMemory);
	WWASSERT(dest_surface);

	if (!DDSMemory || !Get_CubeMap_Memory_Pointer(face,level))
	{
		WWASSERT_PRINT(DDSMemory,"Surface mip level pointer is missing");
		return;
	}
	if (!Legacy_DDS_Destination_Pitch_Is_Valid(
		dest_format,dest_width,dest_pitch))
	{
		WWASSERT_PRINT(0,"Cube surface destination pitch is invalid");
		return;
	}

	// If the format and size is a match just copy the contents
	bool has_hsv_shift = hsv_shift[0]!=0.0f || hsv_shift[1]!=0.0f || hsv_shift[2]!=0.0f;
	if (dest_format==Format && dest_width==Get_Width(level) && dest_height==Get_Height(level))
	{
		// If hue shift, we can't just copy...
		if (has_hsv_shift)
		{
			if (Format==WW3D_FORMAT_DXT1)
			{
				const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_CubeMap_Memory_Pointer(face,level));
				for (unsigned y=0;y<dest_height;y+=4)
				{
					const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*2;
					unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
					for (unsigned x=0;x<dest_width;x+=4)
					{
						unsigned cols=*src_ptr++;		// Bytes 1-4 of color block
						unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
						unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
						col0=ARGB8888_To_RGB565(col0);
						col1=ARGB8888_To_RGB565(col1);
						cols=unsigned(col0)<<16|col1;
						*dest_ptr++=cols;

						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
					}
				}
			}
			else if (Format==WW3D_FORMAT_DXT5)
			{
				const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_CubeMap_Memory_Pointer(face,level));
				for (unsigned y=0;y<dest_height;y+=4)
				{
					const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*4;
					unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
					for (unsigned x=0;x<dest_width;x+=4)
					{
						*dest_ptr++=*src_ptr++;		// Bytes 1-4 of alpha block
						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of alpha block
						unsigned cols=*src_ptr++;		// Bytes 1-4 of color block
						unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
						unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
						col0=ARGB8888_To_RGB565(col0);
						col1=ARGB8888_To_RGB565(col1);
						cols=unsigned(col0)<<16|col1;
						*dest_ptr++=cols;

						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
					}
				}
			}
			else
			{
				WWASSERT(0);
			}

		}
		else
		{
			if (!Legacy_DDS_Copy_Compressed_Rows(
				Get_CubeMap_Memory_Pointer(face,level),dest_width,dest_height,
				Format,dest_surface,dest_pitch))
				WWASSERT_PRINT(0,"Cube surface compressed copy failed");
		}
	}
	else
	{
		// If size matches, copy each pixel linearly with color space conversion
		if (dest_width==Get_Width(level) && dest_height==Get_Height(level))
		{
			// An exception here - if the source format is DXT1 and the destination
			// is DXT2, just copy the contents and create an empty alpha channel.
			// This is needed on NVidia cards that have problems with DXT1 compression.
			if (Format==WW3D_FORMAT_DXT1 && dest_format==WW3D_FORMAT_DXT2)
			{
				// If hue shift, we can't just copy...
				if (has_hsv_shift)
				{
					const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_CubeMap_Memory_Pointer(face,level));
					for (unsigned y=0;y<dest_height;y+=4)
					{
						const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*2;
						unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
						for (unsigned x=0;x<dest_width;x+=4)
						{
							*dest_ptr++=0xffffffff;		// Bytes 1-4 of alpha block
							*dest_ptr++=0xffffffff;		// Bytes 5-8 of alpha block
//							*dest_ptr++=*src_ptr++;		// Bytes 1-4 of color block

							unsigned cols=*src_ptr++;	// Bytes 1-4 of color block
							unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
							unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
							Recolor(col0,hsv_shift);
							Recolor(col1,hsv_shift);
							col0=ARGB8888_To_RGB565(col0);
							col1=ARGB8888_To_RGB565(col1);
							cols=unsigned(col0)<<16|col1;
							*dest_ptr++=cols;

							*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
						}
					}
				}
				else
				{
					const unsigned* source_base=reinterpret_cast<const unsigned*>(Get_CubeMap_Memory_Pointer(face,level));
					for (unsigned y=0;y<dest_height;y+=4)
					{
						const unsigned* src_ptr=source_base+(y/4)*((dest_width+3)/4)*2;
						unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface+(y/4)*dest_pitch);
						for (unsigned x=0;x<dest_width;x+=4)
						{
							*dest_ptr++=0xffffffff;		// Bytes 1-4 of alpha block
							*dest_ptr++=0xffffffff;		// Bytes 5-8 of alpha block
							*dest_ptr++=*src_ptr++;		// Bytes 1-4 of color block
							*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
						}
					}
				}
			}
			else
			{
				unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_format);

				// Copy 4x4 block at a time
				bool contains_alpha=false;
				for (unsigned y=0;y<dest_height;y+=4)
				{
					unsigned char* dest_ptr=dest_surface;
					dest_ptr+=y*dest_pitch;
					for (unsigned x=0;x<dest_width;x+=4,dest_ptr+=dest_bpp*4)
					{
						contains_alpha|=Copy_4x4_Block_From_Memory_Clipped(
							Get_CubeMap_Memory_Pointer(face,level),dest_ptr,
							dest_pitch,dest_format,level,x,y,
							dest_width,dest_height,hsv_shift);
					}
				}
				if (Format==WW3D_FORMAT_DXT1 && contains_alpha)
				{
					WWDEBUG_SAY(("Warning: DXT1 format should not contain alpha information - file %s",Name));
				}
			}
		}
	}
}

// volume texture copy
const unsigned char* DDSFileClass::Get_Volume_Memory_Pointer(unsigned int level)  const
{
	return nullptr;//DDSMemory[
}

void DDSFileClass::Copy_Volume_Level_To_Surface
(
	unsigned level,
	unsigned depth,
	WW3DFormat dest_format,
	unsigned dest_width,
	unsigned dest_height,
	unsigned char* dest_surface,
	unsigned row_pitch,
	unsigned slice_pitch,
	const Vector3& hsv_shift
)
{
	WWASSERT(DDSMemory);
	WWASSERT(dest_surface);

	if (!DDSMemory || !Get_Volume_Memory_Pointer(level))
	{
		WWASSERT_PRINT(DDSMemory,"Surface mip level pointer is missing");
		return;
	}

	// get 'dest_surface'


	// If the format and size is a match just copy the contents
	bool has_hsv_shift = hsv_shift[0]!=0.0f || hsv_shift[1]!=0.0f || hsv_shift[2]!=0.0f;
	if (dest_format==Format && dest_width==Get_Width(level) && dest_height==Get_Height(level))
	{
		// If hue shift, we can't just copy...
		if (has_hsv_shift)
		{
			if (Format==WW3D_FORMAT_DXT1)
			{
				const unsigned* src_ptr=reinterpret_cast<const unsigned*>(Get_Volume_Memory_Pointer(level));
				unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface);
				for (unsigned y=0;y<dest_height;y+=4)
				{
					for (unsigned x=0;x<dest_width;x+=4)
					{
						unsigned cols=*src_ptr++;		// Bytes 1-4 of color block
						unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
						unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
						col0=ARGB8888_To_RGB565(col0);
						col1=ARGB8888_To_RGB565(col1);
						cols=unsigned(col0)<<16|col1;
						*dest_ptr++=cols;

						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
					}
				}
			}
			else if (Format==WW3D_FORMAT_DXT5)
			{
				const unsigned* src_ptr=reinterpret_cast<const unsigned*>(Get_Volume_Memory_Pointer(level));
				unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface);
				for (unsigned y=0;y<dest_height;y+=4)
				{
					for (unsigned x=0;x<dest_width;x+=4)
					{
						*dest_ptr++=*src_ptr++;		// Bytes 1-4 of alpha block
						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of alpha block
						unsigned cols=*src_ptr++;		// Bytes 1-4 of color block
						unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
						unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
						col0=ARGB8888_To_RGB565(col0);
						col1=ARGB8888_To_RGB565(col1);
						cols=unsigned(col0)<<16|col1;
						*dest_ptr++=cols;

						*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
					}
				}
			}
			else
			{
				WWASSERT(0);
			}

		}
		else
		{
			unsigned compressed_size=Get_Level_Size(level);
			memcpy(dest_surface,Get_Volume_Memory_Pointer(level),compressed_size);
		}
	}
	else
	{
		// If size matches, copy each pixel linearly with color space conversion
		if (dest_width==Get_Width(level) && dest_height==Get_Height(level))
		{
			// An exception here - if the source format is DXT1 and the destination
			// is DXT2, just copy the contents and create an empty alpha channel.
			// This is needed on NVidia cards that have problems with DXT1 compression.
			if (Format==WW3D_FORMAT_DXT1 && dest_format==WW3D_FORMAT_DXT2)
			{
				// If hue shift, we can't just copy...
				if (has_hsv_shift)
				{
					const unsigned* src_ptr=reinterpret_cast<const unsigned*>(Get_Volume_Memory_Pointer(level));
					unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface);
					for (unsigned y=0;y<dest_height;y+=4)
					{
						for (unsigned x=0;x<dest_width;x+=4)
						{
							*dest_ptr++=0xffffffff;		// Bytes 1-4 of alpha block
							*dest_ptr++=0xffffffff;		// Bytes 5-8 of alpha block
//							*dest_ptr++=*src_ptr++;		// Bytes 1-4 of color block

							unsigned cols=*src_ptr++;	// Bytes 1-4 of color block
							unsigned col0=RGB565_To_ARGB8888((unsigned short)(cols>>16));
							unsigned col1=RGB565_To_ARGB8888((unsigned short)(cols&0xffff));
							Recolor(col0,hsv_shift);
							Recolor(col1,hsv_shift);
							col0=ARGB8888_To_RGB565(col0);
							col1=ARGB8888_To_RGB565(col1);
							cols=unsigned(col0)<<16|col1;
							*dest_ptr++=cols;

							*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
						}
					}
				}
				else
				{
					const unsigned* src_ptr=reinterpret_cast<const unsigned*>(Get_Volume_Memory_Pointer(level));
					unsigned* dest_ptr=reinterpret_cast<unsigned*>(dest_surface);
					for (unsigned y=0;y<dest_height;y+=4)
					{
						for (unsigned x=0;x<dest_width;x+=4)
						{
							*dest_ptr++=0xffffffff;		// Bytes 1-4 of alpha block
							*dest_ptr++=0xffffffff;		// Bytes 5-8 of alpha block
							*dest_ptr++=*src_ptr++;		// Bytes 1-4 of color block
							*dest_ptr++=*src_ptr++;		// Bytes 5-8 of color block
						}
					}
				}
			}
			else
			{
				WWASSERT(0);
			/*	todo
				unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_format);

				// Copy 4x4 block at a time
				bool contains_alpha=false;
				for (unsigned z=0;z<dest_depth;z++)
				{
					for (unsigned y=0;y<dest_height;y+=4)
					{
						unsigned char* dest_ptr=dest_surface;
						row_ptr+=y*row_pitch;
						for (unsigned x=0;x<dest_width;x+=4,dest_ptr+=dest_bpp*4)
						{
							contains_alpha|=Get_4x4_Block(dest_ptr,dest_pitch,dest_format,level,x,y,hsv_shift);
						}
					}
					if (Format==WW3D_FORMAT_DXT1 && contains_alpha)
					{
						WWDEBUG_SAY(("Warning: DXT1 format should not contain alpha information - file %s",Name));
					}
				}*/
			}
		}
	}
}

// ----------------------------------------------------------------------------

WWINLINE static unsigned Combine_Colors(unsigned col1, unsigned col2, unsigned rel)
{
	const unsigned R_B_MASK=0x00ff00ff;
	const unsigned G_MASK=0x0000ff00;

	unsigned rel2=255-rel;

	unsigned r_b_col1=col1&R_B_MASK;
	r_b_col1*=rel;
	unsigned r_b_col2=col2&R_B_MASK;
	r_b_col2*=rel2;
	r_b_col1+=r_b_col2;
	r_b_col1>>=8;
	r_b_col1&=R_B_MASK;


	unsigned g_col1=col1&G_MASK;
	g_col1*=rel;
	unsigned g_col2=col2&G_MASK;
	g_col2*=rel2;
	g_col1+=g_col2;
	g_col1>>=8;
	g_col1&=G_MASK;

	return r_b_col1|g_col1;

/*	float f=float(rel)/256.0f;

	unsigned new_col=0;
	new_col|=int(float(int(col1&0x00ff0000))*f+float(int(col2&0x00ff0000))*(1.0f-f))&0x00ff0000;
	new_col|=int(float(int(col1&0x0000ff00))*f+float(int(col2&0x0000ff00))*(1.0f-f))&0x0000ff00;
	new_col|=int(float(int(col1&0x000000ff))*f+float(int(col2&0x000000ff))*(1.0f-f))&0x000000ff;
	return new_col;
*/
}

// ----------------------------------------------------------------------------
//
// Note that this is NOT an efficient way of extracting pixels from compressed image - we should implement
// faster block-copy method for non-scaled copying.
//
// ----------------------------------------------------------------------------

unsigned DDSFileClass::Get_Pixel(unsigned level,unsigned x,unsigned y) const
{
	WWASSERT(level<MipLevels);
	WWASSERT(x<Get_Width(level));
	WWASSERT(y<Get_Height(level));

	switch (Format) {
	// Note that we don't currently really support alpha on DXT1 - all alpha textures should use DXT5.
	// The reason for this is that when converting from DXT1 to 16 bit uncompressed texture we want
	// to be able to use RGB565 format instead of ARGB4444. As the alpha is encoded in DXT1 per-block
	// basis there isn't really a way to tell if the surface has an alpha or not so either we use alpha
	// or we don't.
	case WW3D_FORMAT_DXT1:
		{
			const unsigned char* block_memory=Get_Memory_Pointer(level)+(x/4)*8+((y/4)*((Get_Width(level)+3)/4))*8;

			unsigned col0=RGB565_To_ARGB8888(*(unsigned short*)&block_memory[0]);
			unsigned col1=RGB565_To_ARGB8888(*(unsigned short*)&block_memory[2]);
			unsigned char line=block_memory[4+(y%4)];
			line>>=(x%4)*2;
			line=(line&3);
			if (col0>col1) {
				switch (line) {
				case 0: return col0|0xff000000;
				case 1: return col1|0xff000000;
				case 2: return Combine_Colors(col1,col0,85)|0xff000000;
				case 3: return Combine_Colors(col0,col1,85)|0xff000000;
				}
			}
			else {
				switch (line) {
				case 0: return col0|0xff000000;
				case 1: return col1|0xff000000;
				case 2: return Combine_Colors(col1,col0,128)|0xff000000;
				case 3: return 0x00000000;
				}
			}
		}
		break;
	case WW3D_FORMAT_DXT2:
		return 0xffffffff;
	case WW3D_FORMAT_DXT3:
		return 0xffffffff;
	case WW3D_FORMAT_DXT4:
		return 0xffffffff;
	case WW3D_FORMAT_DXT5:
		{
			const unsigned char* alpha_block=Get_Memory_Pointer(level)+(x/4)*16+((y/4)*((Get_Width(level)+3)/4))*16;

			unsigned alpha0=alpha_block[0];
			unsigned alpha1=alpha_block[1];

			unsigned bit_idx=((x%4)+4*(y%4))*3;
			unsigned byte_idx=bit_idx/8;
			bit_idx%=8;
			unsigned alpha_index=0;
			for (int i=0;i<3;++i) {
				WWASSERT(byte_idx<6);
				unsigned alpha_bit=(alpha_block[2+byte_idx]>>(bit_idx))&1;
				alpha_index|=alpha_bit<<(i);
				bit_idx++;
				if (bit_idx>=8) {
					bit_idx=0;
					byte_idx++;
				}
			}
			WWASSERT(alpha_index<8);

			// 8-alpha or 6-alpha block?
			unsigned alpha_value=0;
			if (alpha0>alpha1) {
				// 8-alpha block:  derive the other six alphas.
				// Bit code 000 = alpha_0, 001 = alpha_1, others are interpolated.
				switch (alpha_index) {
				case 0: alpha_value=alpha0; break;
				case 1: alpha_value=alpha1; break;
				case 2: alpha_value=(6*alpha0+1*alpha1+3) / 7; break;    // bit code 010
				case 3: alpha_value=(5*alpha0+2*alpha1+3) / 7; break;    // bit code 011
				case 4: alpha_value=(4*alpha0+3*alpha1+3) / 7; break;    // bit code 100
				case 5: alpha_value=(3*alpha0+4*alpha1+3) / 7; break;    // bit code 101
				case 6: alpha_value=(2*alpha0+5*alpha1+3) / 7; break;    // bit code 110
				case 7: alpha_value=(1*alpha0+6*alpha1+3) / 7; break;    // bit code 111
				}
			}
			else {
				// 6-alpha block.
				// Bit code 000 = alpha_0, 001 = alpha_1, others are interpolated.
				switch (alpha_index) {
				case 0: alpha_value=alpha0; break;
				case 1: alpha_value=alpha1; break;
				case 2: alpha_value=(4*alpha0+1*alpha1+2) / 5; break;    // Bit code 010
				case 3: alpha_value=(3*alpha0+2*alpha1+2) / 5; break;    // Bit code 011
				case 4: alpha_value=(2*alpha0+3*alpha1+2) / 5; break;    // Bit code 100
				case 5: alpha_value=(1*alpha0+4*alpha1+2) / 5; break;    // Bit code 101
				case 6: alpha_value=0; break;                            // Bit code 110
				case 7: alpha_value=255; break;                          // Bit code 111
				}
			}
			alpha_value<<=24;

			// Extract color

			const unsigned char* color_block=alpha_block+8;
			unsigned col0=RGB565_To_ARGB8888(*(unsigned short*)&color_block[0]);
			unsigned col1=RGB565_To_ARGB8888(*(unsigned short*)&color_block[2]);
			unsigned char line=color_block[4+(y%4)];
			line>>=(x%4)*2;
			line=(line&3);
			switch (line) {
			case 0: return col0|alpha_value;
			case 1: return col1|alpha_value;
			case 2: return Combine_Colors(col1,col0,85)|alpha_value;
			case 3: return Combine_Colors(col0,col1,85)|alpha_value;
			}
		}
		break;
	}
	return 0xffffffff;
}

// ----------------------------------------------------------------------------
//
// Uncompress one 4x4 block from the compressed image.
//
// Returns: true if block contained alpha, false is not
//
// Note: Destination can't be DXT or paletted surface!
//
// ----------------------------------------------------------------------------

bool DDSFileClass::Get_4x4_Block_From_Memory(
	const unsigned char* source_memory,
	unsigned char* dest_ptr,			// Destination surface pointer
	unsigned dest_pitch,					// Destination surface pitch, in bytes
	WW3DFormat dest_format,				// Destination surface format, A8R8G8B8 is fastest
	unsigned level,						// DDS mipmap level to copy from
	unsigned source_x,					// DDS x offset to copy from, must be aligned by 4!
	unsigned source_y,					// DDS y offset to copy from, must be aligned by 4!
	const Vector3& hsv_shift) const
{
	if (source_memory==nullptr || dest_ptr==nullptr)
		return false;
	// Verify the block alignment
	WWASSERT((source_x&3)==0);
	WWASSERT((source_y&3)==0);
	// Verify level
	WWASSERT(level<MipLevels);
	// Verify coordinate bounds
	WWASSERT(source_x<Get_Width(level));
	WWASSERT(source_y<Get_Height(level));

	unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_format);

	bool has_hsv_shift = hsv_shift[0]!=0.0f || hsv_shift[1]!=0.0f || hsv_shift[2]!=0.0f;
	switch (Format) {
	// Note that we don't currently really support alpha on DXT1 - all alpha textures should use DXT5.
	// The reason for this is that when converting from DXT1 to 16 bit uncompressed texture we want
	// to be able to use RGB565 format instead of ARGB4444. As the alpha is encoded in DXT1 per-block
	// basis there isn't really a way to tell if the surface has an alpha or not so either we use alpha
	// or we don't.
	case WW3D_FORMAT_DXT1:
		{
			const unsigned char* block_memory=source_memory+(source_x/4)*8+((source_y/4)*((Get_Width(level)+3)/4))*8;

			unsigned col0=RGB565_To_ARGB8888(*(unsigned short*)&block_memory[0]);
			unsigned col1=RGB565_To_ARGB8888(*(unsigned short*)&block_memory[2]);
			// Even if we don't support alpha, decompression is different if source has alpha
			unsigned dest_pixel=0;
			if (col0>col1) {
				if (has_hsv_shift) {
					Recolor(col0,hsv_shift);
					Recolor(col1,hsv_shift);
				}

				for (int y=0;y<4;++y) {
					unsigned char* tmp_dest_ptr=dest_ptr;
					dest_ptr+=dest_pitch;
					unsigned char line=block_memory[4+y];
					for (int x=0;x<4;++x) {
						switch (line&3) {
						case 0: dest_pixel=col0|0xff000000; break;
						case 1: dest_pixel=col1|0xff000000; break;
						case 2: dest_pixel=Combine_Colors(col1,col0,85)|0xff000000; break;
						case 3: dest_pixel=Combine_Colors(col0,col1,85)|0xff000000; break;
						}
						line>>=2;

						Legacy_DDS_Write_B8G8R8A8(tmp_dest_ptr,dest_format,dest_pixel);
						tmp_dest_ptr+=dest_bpp;
					}
				}
				return false;	// No alpha found in the block
			}
			else {
				if (has_hsv_shift) {
					Recolor(col0,hsv_shift);
					Recolor(col1,hsv_shift);
				}
				bool contains_alpha=false;
				for (int y=0;y<4;++y) {
					unsigned char* tmp_dest_ptr=dest_ptr;
					dest_ptr+=dest_pitch;
					unsigned char line=block_memory[4+y];
					for (int x=0;x<4;++x) {
						switch (line&3) {
						case 0: dest_pixel=col0|0xff000000; break;
						case 1: dest_pixel=col1|0xff000000; break;
						case 2: dest_pixel=Combine_Colors(col1,col0,128)|0xff000000; break;
						case 3: dest_pixel=0x00000000; contains_alpha=true; break;
						}
						line>>=2;

						Legacy_DDS_Write_B8G8R8A8(tmp_dest_ptr,dest_format,dest_pixel);
						tmp_dest_ptr+=dest_bpp;
					}
				}
				return contains_alpha;	// Alpha block...?
			}
		}
		break;
	case WW3D_FORMAT_DXT2:
		return false;
	case WW3D_FORMAT_DXT3:
		return false;
	case WW3D_FORMAT_DXT4:
		return false;
	case WW3D_FORMAT_DXT5:
		{
			// Init alphas
			const unsigned char* alpha_block=source_memory+(source_x/4)*16+((source_y/4)*((Get_Width(level)+3)/4))*16;

			unsigned alphas[8];
			alphas[0]=alpha_block[0];
			alphas[1]=alpha_block[1];

			// 8-alpha or 6-alpha block?
			if (alphas[0]>alphas[1]) {
				alphas[2]=(6*alphas[0]+1*alphas[1]+3) / 7;   // bit code 010
				alphas[3]=(5*alphas[0]+2*alphas[1]+3) / 7;   // bit code 011
				alphas[4]=(4*alphas[0]+3*alphas[1]+3) / 7;   // bit code 100
				alphas[5]=(3*alphas[0]+4*alphas[1]+3) / 7;   // bit code 101
				alphas[6]=(2*alphas[0]+5*alphas[1]+3) / 7;   // bit code 110
				alphas[7]=(1*alphas[0]+6*alphas[1]+3) / 7;   // bit code 111
			}
			else {
				alphas[2]=(4*alphas[0]+1*alphas[1]+2) / 5;   // Bit code 010
				alphas[3]=(3*alphas[0]+2*alphas[1]+2) / 5;   // Bit code 011
				alphas[4]=(2*alphas[0]+3*alphas[1]+2) / 5;   // Bit code 100
				alphas[5]=(1*alphas[0]+4*alphas[1]+2) / 5;   // Bit code 101
				alphas[6]=0; 										   // Bit code 110
				alphas[7]=255; 									   // Bit code 111
			}

			// Init colors
			const unsigned char* color_block=alpha_block+8;
			unsigned col0=RGB565_To_ARGB8888(*(unsigned short*)&color_block[0]);
			unsigned col1=RGB565_To_ARGB8888(*(unsigned short*)&color_block[2]);
			if (has_hsv_shift) {
				Recolor(col0,hsv_shift);
				Recolor(col1,hsv_shift);
			}

			unsigned dest_pixel=0;
			unsigned bit_idx=0;
			unsigned contains_alpha=0xff;

			unsigned alpha_indices[16];
			unsigned* ai_ptr=alpha_indices;
			for (int a=0;a<2;++a) {
				ai_ptr[0]=alpha_block[2]&0x7;
				ai_ptr[1]=(alpha_block[2]>>3)&0x7;
				ai_ptr[2]=(alpha_block[2]>>6)|((alpha_block[3]&1)<<2);
				ai_ptr[3]=(alpha_block[3]>>1)&0x7;
				ai_ptr[4]=(alpha_block[3]>>4)&0x7;
				ai_ptr[5]=(alpha_block[3]>>7)|((alpha_block[4]&3)<<1);
				ai_ptr[6]=(alpha_block[4]>>2)&0x7;
				ai_ptr[7]=(alpha_block[4]>>5);
				ai_ptr+=8;
				alpha_block+=3;
			}

			unsigned aii=0;
			for (int y=0;y<4;++y) {
				unsigned char* tmp_dest_ptr=dest_ptr;
				dest_ptr+=dest_pitch;
				unsigned char line=color_block[4+y];
				for (int x=0;x<4;++x,bit_idx+=3) {
					unsigned alpha_value=alphas[alpha_indices[aii++]];
					contains_alpha&=alpha_value;
					alpha_value<<=24;

					// Extract color

					switch (line&3) {
					case 0: dest_pixel=col0|alpha_value; break;
					case 1: dest_pixel=col1|alpha_value; break;
					case 2: dest_pixel=Combine_Colors(col1,col0,85)|alpha_value; break;
					case 3: dest_pixel=Combine_Colors(col0,col1,85)|alpha_value; break;
					}
					line>>=2;

					Legacy_DDS_Write_B8G8R8A8(tmp_dest_ptr,dest_format,dest_pixel);
					tmp_dest_ptr+=dest_bpp;
				}
			}




/*
			for (int y=0;y<4;++y) {
				unsigned char* tmp_dest_ptr=dest_ptr;
				dest_ptr+=dest_pitch;
				unsigned char line=color_block[4+y];
				for (int x=0;x<4;++x,bit_idx+=3) {
					unsigned byte_idx=bit_idx/8;
					unsigned tmp_bit_idx=bit_idx&7;
					unsigned alpha_index=0;
					for (int i=0;i<3;++i) {
						WWASSERT(byte_idx<6);
						unsigned alpha_bit=(alpha_block[2+byte_idx]>>(tmp_bit_idx))&1;
						alpha_index|=alpha_bit<<(i);
						tmp_bit_idx++;
						if (tmp_bit_idx>=8) {
							tmp_bit_idx=0;
							byte_idx++;
						}
					}
					WWASSERT(alpha_index<8);
					unsigned alpha_value=alphas[alpha_index];
					contains_alpha&=alpha_value;
					alpha_value<<=24;

					// Extract color

					switch (line&3) {
					case 0: dest_pixel=col0|alpha_value; break;
					case 1: dest_pixel=col1|alpha_value; break;
					case 2: dest_pixel=Combine_Colors(col1,col0,85)|alpha_value; break;
					case 3: dest_pixel=Combine_Colors(col0,col1,85)|alpha_value; break;
					}
					line>>=2;

					Legacy_DDS_Write_B8G8R8A8(tmp_dest_ptr,dest_format,dest_pixel);
					tmp_dest_ptr+=dest_bpp;
				}
			}
*/
			return contains_alpha!=0xff;	// Alpha block... DXT5 should only be used when the image needs alpha
													// but for now check anyway...
		}
	}
	return false;

}

bool DDSFileClass::Get_4x4_Block(
	unsigned char* dest_ptr,
	unsigned dest_pitch,
	WW3DFormat dest_format,
	unsigned level,
	unsigned source_x,
	unsigned source_y,
	const Vector3& hsv_shift) const
{
	return Get_4x4_Block_From_Memory(
		Get_Memory_Pointer(level),dest_ptr,dest_pitch,dest_format,
		level,source_x,source_y,hsv_shift);
}
