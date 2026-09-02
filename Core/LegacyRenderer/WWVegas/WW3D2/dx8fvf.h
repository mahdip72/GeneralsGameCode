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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8fvf.h                               $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 5:06p                                             $*
 *                                                                                             *
 *                    $Revision:: 7                                                          $*
 *                                                                                             *
 * 06/26/02 KM VB Vertex format update for shaders                                       *
 * 07/17/02 KM VB Vertex format update for displacement mapping                               *
 * 08/01/02 KM VB Vertex format update for cube mapping                               *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#if defined(_WIN64)
#include "Renderer/LegacyFvfLayout.h"
#else
#include <d3d8.h>
#endif
#ifdef WWDEBUG
#include "WWDebug/wwdebug.h"
#endif

class StringClass;

#if defined(_WIN64)
// Serialized WW3D streams always expose eight texture-coordinate stages. The
// native path uses this neutral limit instead of importing the D3D8 SDK.
enum { DX8_MAX_TEXCOORD = rts::render::LEGACY_TEXTURE_STAGE_COUNT };
#else
// Preserve the original D3D8/VC6 contract on the 32-bit oracle lane.
enum { DX8_MAX_TEXCOORD = D3DDP_MAXTEXCOORD };
#endif

enum {
	#if !defined(_WIN64)
	// Keep the complete D3D8 alias surface on the 32-bit/VC6 oracle lane.
	DX8_FVF_POSITION_MASK		= D3DFVF_POSITION_MASK,
	DX8_FVF_XYZRHW			= D3DFVF_XYZRHW,
	DX8_FVF_XYZB1			= D3DFVF_XYZB1,
	DX8_FVF_XYZB2			= D3DFVF_XYZB2,
	DX8_FVF_XYZB3			= D3DFVF_XYZB3,
	DX8_FVF_XYZB4			= D3DFVF_XYZB4,
	DX8_FVF_XYZB5			= D3DFVF_XYZB5,
	DX8_FVF_NORMAL			= D3DFVF_NORMAL,
	DX8_FVF_PSIZE			= D3DFVF_PSIZE,
	DX8_FVF_DIFFUSE			= D3DFVF_DIFFUSE,
	DX8_FVF_SPECULAR		= D3DFVF_SPECULAR,
	DX8_FVF_TEX1			= D3DFVF_TEX1,
	DX8_FVF_TEX2			= D3DFVF_TEX2,
	DX8_FVF_TEX3			= D3DFVF_TEX3,
	DX8_FVF_TEX4			= D3DFVF_TEX4,
	DX8_FVF_TEX5			= D3DFVF_TEX5,
	DX8_FVF_TEX6			= D3DFVF_TEX6,
	DX8_FVF_TEX7			= D3DFVF_TEX7,
	DX8_FVF_TEX8			= D3DFVF_TEX8,
	DX8_FVF_TEXCOUNT_MASK	= D3DFVF_TEXCOUNT_MASK,
	DX8_FVF_TEXCOUNT_SHIFT	= D3DFVF_TEXCOUNT_SHIFT,
	DX8_FVF_LASTBETA_UBYTE4	= D3DFVF_LASTBETA_UBYTE4,
	DX8_FVF_LASTBETA_D3DCOLOR = 0x00008000u,
	DX8_FVF_XYZ				= D3DFVF_XYZ,
	DX8_FVF_XYZN			= D3DFVF_XYZ|D3DFVF_NORMAL,
	DX8_FVF_XYZNUV1		= D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1,
	DX8_FVF_XYZNUV2		= D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2,
	DX8_FVF_XYZNDUV1		= D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1|D3DFVF_DIFFUSE,
	DX8_FVF_XYZNDUV2		= D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2|D3DFVF_DIFFUSE,
	DX8_FVF_XYZDUV1		= D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_DIFFUSE,
	DX8_FVF_XYZDUV2		= D3DFVF_XYZ|D3DFVF_TEX2|D3DFVF_DIFFUSE,
	DX8_FVF_XYZUV1			= D3DFVF_XYZ|D3DFVF_TEX1,
	DX8_FVF_XYZUV2			= D3DFVF_XYZ|D3DFVF_TEX2,
	DX8_FVF_XYZNDUV1TG3	= (D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX4|D3DFVF_TEXCOORDSIZE2(0)|D3DFVF_TEXCOORDSIZE3(1)|D3DFVF_TEXCOORDSIZE3(2)|D3DFVF_TEXCOORDSIZE3(3)),
	DX8_FVF_XYZNUV2DMAP	= (D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE2(2) ),
	DX8_FVF_XYZNDCUBEMAP	= D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE //|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0)
	#else
	DX8_FVF_XYZ				= rts::render::LEGACY_FVF_XYZ,
	DX8_FVF_XYZN			= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_NORMAL,
	DX8_FVF_XYZNUV1		= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_NORMAL|rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_XYZNUV2		= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_NORMAL|rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_XYZNDUV1		= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_NORMAL|rts::render::LEGACY_FVF_TEX1|rts::render::LEGACY_FVF_DIFFUSE,
	DX8_FVF_XYZNDUV2		= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_NORMAL|rts::render::LEGACY_FVF_TEX2|rts::render::LEGACY_FVF_DIFFUSE,
	DX8_FVF_XYZDUV1		= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_TEX1|rts::render::LEGACY_FVF_DIFFUSE,
	DX8_FVF_XYZDUV2		= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_TEX2|rts::render::LEGACY_FVF_DIFFUSE,
	DX8_FVF_XYZUV1			= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_XYZUV2			= rts::render::LEGACY_FVF_XYZ|rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_XYZNDUV1TG3	= rts::render::LEGACY_FVF_XYZNDUV1TG3,
	DX8_FVF_XYZNUV2DMAP	= rts::render::LEGACY_FVF_XYZNUV2DMAP,
	DX8_FVF_XYZNDCUBEMAP	= rts::render::LEGACY_FVF_XYZNDCUBEMAP,
	DX8_FVF_POSITION_MASK		= rts::render::LEGACY_FVF_POSITION_MASK,
	DX8_FVF_XYZRHW			= rts::render::LEGACY_FVF_XYZRHW,
	DX8_FVF_XYZB1			= rts::render::LEGACY_FVF_XYZB1,
	DX8_FVF_XYZB2			= rts::render::LEGACY_FVF_XYZB2,
	DX8_FVF_XYZB3			= rts::render::LEGACY_FVF_XYZB3,
	DX8_FVF_XYZB4			= rts::render::LEGACY_FVF_XYZB4,
	DX8_FVF_XYZB5			= rts::render::LEGACY_FVF_XYZB5,
	DX8_FVF_NORMAL			= rts::render::LEGACY_FVF_NORMAL,
	DX8_FVF_PSIZE			= rts::render::LEGACY_FVF_PSIZE,
	DX8_FVF_DIFFUSE			= rts::render::LEGACY_FVF_DIFFUSE,
	DX8_FVF_SPECULAR		= rts::render::LEGACY_FVF_SPECULAR,
	DX8_FVF_TEX1			= rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_TEX2			= rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_TEX3			= rts::render::LEGACY_FVF_TEX3,
	DX8_FVF_TEX4			= rts::render::LEGACY_FVF_TEX4,
	DX8_FVF_TEX5			= rts::render::LEGACY_FVF_TEX5,
	DX8_FVF_TEX6			= rts::render::LEGACY_FVF_TEX6,
	DX8_FVF_TEX7			= rts::render::LEGACY_FVF_TEX7,
	DX8_FVF_TEX8			= rts::render::LEGACY_FVF_TEX8,
	DX8_FVF_TEXCOUNT_MASK	= rts::render::LEGACY_FVF_TEXCOUNT_MASK,
	DX8_FVF_TEXCOUNT_SHIFT	= rts::render::LEGACY_FVF_TEXCOUNT_SHIFT,
	DX8_FVF_LASTBETA_UBYTE4	= rts::render::LEGACY_FVF_LASTBETA_UBYTE4,
	DX8_FVF_LASTBETA_D3DCOLOR = rts::render::LEGACY_FVF_LASTBETA_PACKED_COLOR
	#endif
};

// ----------------------------------------------------------------------------
//
// Util structures for vertex buffer handling. Cast the void pointer returned
// by the vertex buffer to one of these structures.
//
// ----------------------------------------------------------------------------

struct VertexFormatXYZ
{
	float x;
	float y;
	float z;
};

struct VertexFormatXYZNUV1
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	float u1;
	float v1;
};

struct VertexFormatXYZNUV2
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	float u1;
	float v1;
	float u2;
	float v2;
};

struct VertexFormatXYZN
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
};

struct VertexFormatXYZNDUV1
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	unsigned diffuse;
	float u1;
	float v1;
};

struct VertexFormatXYZNDUV2
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	unsigned diffuse;
	float u1;
	float v1;
	float u2;
	float v2;
};

struct VertexFormatXYZDUV1
{
	float x;
	float y;
	float z;
	unsigned diffuse;
	float u1;
	float v1;
};

struct VertexFormatXYZDUV2
{
	float x;
	float y;
	float z;
	unsigned diffuse;
	float u1;
	float v1;
	float u2;
	float v2;
};

struct VertexFormatXYZUV1
{
	float x;
	float y;
	float z;
	float u1;
	float v1;
};

struct VertexFormatXYZUV2
{
	float x;
	float y;
	float z;
	float u1;
	float v1;
	float u2;
	float v2;
};

// todo KJM compress
struct VertexFormatXYZNDUV1TG3
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	unsigned diffuse;
	float u1;
	float v1;
	float Sx;
	float Sy;
	float Sz;
	float Tx;
	float Ty;
	float Tz;
	float SxTx;
	float SxTy;
	float SxTz;
};


// displacement mapping format
struct VertexFormatXYZNUV2DMAP
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	float T1x;
	float T1y;
	float T1z;
	float T1w;
	float T2x;
	float T2y;
};

// cube map format (texcoords are normally generated)
struct VertexFormatXYZNDCUBEMAP
{
	float x;
	float y;
	float z;
	float nx;
	float ny;
	float nz;
	unsigned diffuse;
//	float u1;
//	float v1;
//	float w1;
};

// FVF info class can be created for any legal FVF. It constructs information
// of offsets to various elements in the vertex buffer.

class FVFInfoClass
{
	W3DMPO_CODE(FVFInfoClass)

	mutable unsigned						FVF;
	mutable unsigned						fvf_size;

	unsigned							location_offset;
	unsigned							normal_offset;
	unsigned							blend_offset;
#if defined(_WIN64)
	// Neutral-only offsets for the individual weighted fields.  Keep these
	// outside the VC6 layout so the 32-bit oracle retains its original ABI.
	unsigned							blend_weight_offset;
	unsigned							blend_index_offset;
#endif
	unsigned							texcoord_offset[DX8_MAX_TEXCOORD];
	unsigned							diffuse_offset;
	unsigned							specular_offset;
public:
	FVFInfoClass(unsigned FVF);

	unsigned Get_Location_Offset() const { return location_offset; }
	unsigned Get_Normal_Offset() const { return normal_offset; }
#ifdef WWDEBUG
	inline unsigned Get_Tex_Offset(unsigned int n) const { WWASSERT(n<DX8_MAX_TEXCOORD); return texcoord_offset[n]; }
#else
	unsigned Get_Tex_Offset(unsigned int n) const { return texcoord_offset[n]; }
#endif

	unsigned Get_Diffuse_Offset() const { return diffuse_offset; }
	unsigned Get_Specular_Offset() const { return specular_offset; }
#if defined(_WIN64)
	unsigned Get_Blend_Weight_Offset() const { return blend_weight_offset; }
	unsigned Get_Blend_Index_Offset() const { return blend_index_offset; }
#endif
	unsigned Get_FVF() const { return FVF; }
	unsigned Get_FVF_Size() const { return fvf_size; }

#if defined(_WIN64)
	// Convert the serialized stream description into the backend-neutral input
	// layout consumed by the D3D11 authority. No native interface is exposed.
	bool Get_Native_Layout(rts::render::RenderVertexLayout *layout) const;
#endif

	void Get_FVF_Name(StringClass& fvfname) const;	// For debug purposes

	// for enabling vertex shaders
	void Set_FVF(unsigned fvf) const { FVF=fvf; }
	void Set_FVF_Size(unsigned size) const { fvf_size=size; }
};
