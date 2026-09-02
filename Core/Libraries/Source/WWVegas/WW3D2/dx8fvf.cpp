/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "dx8fvf.h"
#include "WWLib/wwstring.h"

#if defined(_WIN64)
namespace
{
// FVF values are serialized WW3D stream metadata. The neutral decoder is the
// authority for both size validation and native input-layout construction.
static unsigned Get_FVF_Vertex_Size(unsigned FVF)
{
	return rts::render::LegacyFvfVertexSize(FVF);
}
}

FVFInfoClass::FVFInfoClass(unsigned FVF_)
	:
	FVF(FVF_),
	fvf_size(Get_FVF_Vertex_Size(FVF_))
{
	location_offset = 0;
	normal_offset = 0;
	blend_offset = 0;
	blend_weight_offset = 0;
	blend_index_offset = 0;
	diffuse_offset = 0;
	specular_offset = 0;
	for (unsigned int i = 0; i < DX8_MAX_TEXCOORD; ++i)
	{
		texcoord_offset[i] = 0;
	}

	if (fvf_size == 0)
	{
		return;
	}

	// Keep the byte offsets for every legal legacy position code, including
	// weighted XYZB streams. The neutral layout exposes the blend fields
	// separately while blend_offset denotes the first byte after the complete
	// position/blend prefix.
	const unsigned int position = FVF & rts::render::LEGACY_FVF_POSITION_MASK;
	const unsigned int blend_field_count =
		rts::render::LegacyFvfBlendFieldCount(FVF);
	const unsigned int blendPositionOffset = 3U * sizeof(float);
	switch (position)
	{
	case rts::render::LEGACY_FVF_XYZ:
		blend_offset = 3U * sizeof(float);
		break;
	case rts::render::LEGACY_FVF_XYZRHW:
		blend_offset = 4U * sizeof(float);
		break;
	case rts::render::LEGACY_FVF_XYZB1:
	case rts::render::LEGACY_FVF_XYZB2:
	case rts::render::LEGACY_FVF_XYZB3:
	case rts::render::LEGACY_FVF_XYZB4:
	case rts::render::LEGACY_FVF_XYZB5:
		// LASTBETA replaces the final blend field with indices.  XYZB1
		// therefore has no floating-point weight field in that form; retain
		// the zero sentinel used by the other absent-field offsets.
		if (!rts::render::LegacyFvfHasLastBeta(FVF) || blend_field_count > 1U)
		{
			blend_weight_offset = blendPositionOffset;
		}
		blend_offset = blendPositionOffset +
			blend_field_count * sizeof(float);
		if (rts::render::LegacyFvfHasLastBeta(FVF))
		{
			blend_index_offset = blendPositionOffset +
				(blend_field_count - 1U) * sizeof(float);
		}
		break;
	default:
		return;
	}

	normal_offset = blend_offset;
	diffuse_offset = normal_offset;
	if ((FVF & rts::render::LEGACY_FVF_NORMAL) != 0U)
	{
		diffuse_offset += 3U * sizeof(float);
	}
	if ((FVF & rts::render::LEGACY_FVF_PSIZE) != 0U)
	{
		diffuse_offset += sizeof(float);
	}
	specular_offset = diffuse_offset;
	if ((FVF & rts::render::LEGACY_FVF_DIFFUSE) != 0U)
	{
		specular_offset += sizeof(unsigned int);
	}
	unsigned int offset = specular_offset;
	if ((FVF & rts::render::LEGACY_FVF_SPECULAR) != 0U)
	{
		offset += sizeof(unsigned int);
	}
	const unsigned int texture_count =
		(FVF & rts::render::LEGACY_FVF_TEXCOUNT_MASK) >>
		rts::render::LEGACY_FVF_TEXCOUNT_SHIFT;
	for (unsigned int stage = 0; stage < texture_count &&
		stage < DX8_MAX_TEXCOORD; ++stage)
	{
		texcoord_offset[stage] = offset;
		const unsigned int encoding =
			(FVF >> (16U + stage * 2U)) & 3U;
		const unsigned int component_count = encoding == 0U ? 2U :
			(encoding == 1U ? 3U : (encoding == 2U ? 4U : 1U));
		offset += component_count * sizeof(float);
	}
}

bool FVFInfoClass::Get_Native_Layout(
	rts::render::RenderVertexLayout *layout) const
{
	return layout != nullptr && fvf_size != 0 &&
		rts::render::DecodeLegacyFvfVertexLayout(FVF, fvf_size, layout);
}

void FVFInfoClass::Get_FVF_Name(StringClass& fvfname) const
{
	// Keep diagnostics descriptive without making the native path depend on
	// D3D8 token declarations.
	switch (Get_FVF()) {
	case DX8_FVF_XYZ: fvfname="LegacyFvf_XYZ"; break;
	case DX8_FVF_XYZN: fvfname="LegacyFvf_XYZ|LegacyFvf_NORMAL"; break;
	case DX8_FVF_XYZNUV1: fvfname="LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX1"; break;
	case DX8_FVF_XYZNUV2: fvfname="LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX2"; break;
	case DX8_FVF_XYZNDUV1: fvfname="LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX1|LegacyFvf_DIFFUSE"; break;
	case DX8_FVF_XYZNDUV2: fvfname="LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX2|LegacyFvf_DIFFUSE"; break;
	case DX8_FVF_XYZDUV1: fvfname="LegacyFvf_XYZ|LegacyFvf_TEX1|LegacyFvf_DIFFUSE"; break;
	case DX8_FVF_XYZDUV2: fvfname="LegacyFvf_XYZ|LegacyFvf_TEX2|LegacyFvf_DIFFUSE"; break;
	case DX8_FVF_XYZUV1: fvfname="LegacyFvf_XYZ|LegacyFvf_TEX1"; break;
	case DX8_FVF_XYZUV2: fvfname="LegacyFvf_XYZ|LegacyFvf_TEX2"; break;
	case DX8_FVF_XYZNDUV1TG3: fvfname="LegacyFvf_XYZNDUV1TG3"; break;
	case DX8_FVF_XYZNUV2DMAP: fvfname="LegacyFvf_XYZNUV2DMAP"; break;
	case DX8_FVF_XYZNDCUBEMAP: fvfname="LegacyFvf_XYZNDCUBEMAP"; break;
	default: fvfname="Unknown!"; break;
	}
}
#else

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
	fvf_size(Get_FVF_Vertex_Size(FVF_))
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
#endif
