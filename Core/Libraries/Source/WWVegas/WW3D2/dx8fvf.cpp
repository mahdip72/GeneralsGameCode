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

#include "dx8fvf.h"
#include "WWLib/wwstring.h"

namespace
{
/* Newer SDKs name these legacy-compatible bit values; keep the calculator
 * usable with the minimal headers used by the VC6 lane. */
const unsigned kD3DFVF_PSIZE = 0x00000020u;
const unsigned kD3DFVF_LASTBETA_D3DCOLOR = 0x00008000u;
const unsigned kD3DFVF_TEXCOORD_MASK = 0xffff0000u;
const unsigned kD3DFVF_MAX_TEXCOORD = 8u;

static unsigned Get_FVF_Vertex_Size(unsigned FVF)
{
	const unsigned position = FVF & D3DFVF_POSITION_MASK;
	const unsigned lastBetaMask = D3DFVF_LASTBETA_UBYTE4 |
		kD3DFVF_LASTBETA_D3DCOLOR;
	const unsigned knownMask = D3DFVF_POSITION_MASK |
		D3DFVF_NORMAL | kD3DFVF_PSIZE | D3DFVF_DIFFUSE |
		D3DFVF_SPECULAR | D3DFVF_TEXCOUNT_MASK | lastBetaMask |
		kD3DFVF_TEXCOORD_MASK;
	unsigned vertexSize;
	unsigned blendFieldCount;
	unsigned textureCount;
	unsigned stage;

	/* Reject bits which have no legacy FVF meaning before decoding fields. */
	if ((FVF & ~knownMask) != 0u)
		return 0;

	switch (position)
	{
	case D3DFVF_XYZ:
		vertexSize = 3 * sizeof(float);
		blendFieldCount = 0;
		break;
	case D3DFVF_XYZRHW:
		vertexSize = 4 * sizeof(float);
		blendFieldCount = 0;
		break;
	case D3DFVF_XYZB1:
		vertexSize = 4 * sizeof(float);
		blendFieldCount = 1;
		break;
	case D3DFVF_XYZB2:
		vertexSize = 5 * sizeof(float);
		blendFieldCount = 2;
		break;
	case D3DFVF_XYZB3:
		vertexSize = 6 * sizeof(float);
		blendFieldCount = 3;
		break;
	case D3DFVF_XYZB4:
		vertexSize = 7 * sizeof(float);
		blendFieldCount = 4;
		break;
	case D3DFVF_XYZB5:
		vertexSize = 8 * sizeof(float);
		blendFieldCount = 5;
		break;
	default:
		return 0;
	}

	/* RHW vertices cannot carry a normal; LASTBETA requires blend fields. */
	if ((position == D3DFVF_XYZRHW && (FVF & D3DFVF_NORMAL) != 0u) ||
		(blendFieldCount == 0u && (FVF & lastBetaMask) != 0u) ||
		((FVF & lastBetaMask) == lastBetaMask))
		return 0;

	textureCount = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (textureCount > kD3DFVF_MAX_TEXCOORD)
		return 0;

	if ((FVF & D3DFVF_NORMAL) != 0u)
		vertexSize += 3 * sizeof(float);
	if ((FVF & kD3DFVF_PSIZE) != 0u)
		vertexSize += sizeof(float);
	if ((FVF & D3DFVF_DIFFUSE) != 0u)
		vertexSize += sizeof(DWORD);
	if ((FVF & D3DFVF_SPECULAR) != 0u)
		vertexSize += sizeof(DWORD);

	for (stage = 0; stage < kD3DFVF_MAX_TEXCOORD; ++stage)
	{
		const unsigned format = (FVF >> (16 + stage * 2)) & 3u;
		unsigned componentCount;

		/* A non-default dimension on an unused stage is not a legal FVF. */
		if (stage >= textureCount)
		{
			if (format != 0u)
				return 0;
			continue;
		}

		/* D3DFVF_TEXTUREFORMAT2 is encoded as zero. */
		switch (format)
		{
		case 0u: componentCount = 2u; break;
		case 1u: componentCount = 3u; break;
		case 2u: componentCount = 4u; break;
		case 3u: componentCount = 1u; break;
		default: return 0;
		}
		vertexSize += componentCount * sizeof(float);
	}

	return vertexSize;
}
}

FVFInfoClass::FVFInfoClass(unsigned FVF_)
	:
	FVF(FVF_),
	fvf_size(Get_FVF_Vertex_Size(FVF))
{
	/*
	 * Keep the offsets derived from the same field order as the FVF stride
	 * calculator above.  The old code only advanced XYZ (not XYZRHW) and
	 * special-cased one XYZB4 form.  That made every pre-transformed colour
	 * or textured vertex point at byte zero for D3D11 input-layout creation,
	 * which is a particularly bad failure mode for the shell/UI path.
	 */
	location_offset = 0;
	blend_offset = 0;
	switch (FVF & D3DFVF_POSITION_MASK)
	{
	case D3DFVF_XYZ:
		blend_offset = 3 * sizeof(float);
		break;
	case D3DFVF_XYZRHW:
	case D3DFVF_XYZB1:
		blend_offset = 4 * sizeof(float);
		break;
	case D3DFVF_XYZB2:
		blend_offset = 5 * sizeof(float);
		break;
	case D3DFVF_XYZB3:
		blend_offset = 6 * sizeof(float);
		break;
	case D3DFVF_XYZB4:
		blend_offset = 7 * sizeof(float);
		break;
	case D3DFVF_XYZB5:
		blend_offset = 8 * sizeof(float);
		break;
	default:
		/* Keep invalid FVFs fail-closed for the D3D11 layout builder. */
		blend_offset = 0;
		break;
	}
	if ((FVF & kD3DFVF_PSIZE) != 0u)
	{
		blend_offset += sizeof(float);
	}
	normal_offset=blend_offset;
	diffuse_offset=normal_offset;

	if ((FVF&D3DFVF_NORMAL)==D3DFVF_NORMAL) diffuse_offset+=3*sizeof(float);
	specular_offset=diffuse_offset;

	if ((FVF&D3DFVF_DIFFUSE)==D3DFVF_DIFFUSE) specular_offset+=sizeof(DWORD);
	texcoord_offset[0]=specular_offset;

	if ((FVF&D3DFVF_SPECULAR)==D3DFVF_SPECULAR) texcoord_offset[0]+=sizeof(DWORD);

	for (unsigned int i=1; i<D3DDP_MAXTEXCOORD; i++)
	{
		texcoord_offset[i]=texcoord_offset[i-1];

		if ((int(FVF)&D3DFVF_TEXCOORDSIZE1(i-1))==D3DFVF_TEXCOORDSIZE1(i-1)) texcoord_offset[i]+=sizeof(float);
		else if ((int(FVF)&D3DFVF_TEXCOORDSIZE2(i-1))==D3DFVF_TEXCOORDSIZE2(i-1)) texcoord_offset[i]+=2*sizeof(float);
		else if ((int(FVF)&D3DFVF_TEXCOORDSIZE3(i-1))==D3DFVF_TEXCOORDSIZE3(i-1)) texcoord_offset[i]+=3*sizeof(float);
		else if ((int(FVF)&D3DFVF_TEXCOORDSIZE4(i-1))==D3DFVF_TEXCOORDSIZE4(i-1)) texcoord_offset[i]+=4*sizeof(float);
	}
}

void FVFInfoClass::Get_FVF_Name(StringClass& fvfname) const
{
	switch (Get_FVF()) {
	case DX8_FVF_XYZ: fvfname="D3DFVF_XYZ"; break;
	case DX8_FVF_XYZN: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL"; break;
	case DX8_FVF_XYZNUV1: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1"; break;
	case DX8_FVF_XYZNUV2: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2"; break;
	case DX8_FVF_XYZNDUV1: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZNDUV2: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZDUV1: fvfname="D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZDUV2: fvfname="D3DFVF_XYZ|D3DFVF_TEX2|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZUV1: fvfname="D3DFVF_XYZ|D3DFVF_TEX1"; break;
	case DX8_FVF_XYZUV2: fvfname="D3DFVF_XYZ|D3DFVF_TEX2"; break;
	case DX8_FVF_XYZNDUV1TG3 : fvfname="(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX4|D3DFVF_TEXCOORDSIZE2(0)|D3DFVF_TEXCOORDSIZE3(1)|D3DFVF_TEXCOORDSIZE3(2)|D3DFVF_TEXCOORDSIZE3(3))"; break;
	case DX8_FVF_XYZNUV2DMAP :	fvfname="(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX3|D3DFVF_TEXCOORDSIZE1(0)|D3DFVF_TEXCOORDSIZE4(1)|D3DFVF_TEXCOORDSIZE2(2))"; break;
	case DX8_FVF_XYZNDCUBEMAP : fvfname="(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1|D3DFVFTEXCOORDSIZE3(0)"; break;
	default: fvfname="Unknown!";
	}
}
