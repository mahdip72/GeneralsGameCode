#ifndef RTS_RENDERER_LEGACYFVFLAYOUT_H
#define RTS_RENDERER_LEGACYFVFLAYOUT_H

#include "Renderer/RendererDevice.h"

namespace rts
{
namespace render
{

// The FVF value is part of the serialized WW3D vertex-stream contract.  Keep
// its bit layout in this API-neutral header so native renderer code can decode
// an old stream without importing the retired graphics SDK.  The Win32/VC6 oracle may
// still spell the same values with the SDK macros in its compatibility lane.
enum LegacyFvfFlag
{
	LEGACY_FVF_RESERVED0 = 0x00000001U,
	LEGACY_FVF_XYZ = 0x00000002U,
	LEGACY_FVF_XYZRHW = 0x00000004U,
	LEGACY_FVF_XYZB1 = 0x00000006U,
	LEGACY_FVF_XYZB2 = 0x00000008U,
	LEGACY_FVF_XYZB3 = 0x0000000aU,
	LEGACY_FVF_XYZB4 = 0x0000000cU,
	LEGACY_FVF_XYZB5 = 0x0000000eU,
	LEGACY_FVF_POSITION_MASK = 0x0000400eU,
	LEGACY_FVF_NORMAL = 0x00000010U,
	LEGACY_FVF_PSIZE = 0x00000020U,
	LEGACY_FVF_DIFFUSE = 0x00000040U,
	LEGACY_FVF_SPECULAR = 0x00000080U,
	LEGACY_FVF_TEX1 = 0x00000100U,
	LEGACY_FVF_TEX2 = 0x00000200U,
	LEGACY_FVF_TEX3 = 0x00000300U,
	LEGACY_FVF_TEX4 = 0x00000400U,
	LEGACY_FVF_TEX5 = 0x00000500U,
	LEGACY_FVF_TEX6 = 0x00000600U,
	LEGACY_FVF_TEX7 = 0x00000700U,
	LEGACY_FVF_TEX8 = 0x00000800U,
	LEGACY_FVF_TEXCOUNT_MASK = 0x00000f00U,
	LEGACY_FVF_TEXCOUNT_SHIFT = 8U,
	LEGACY_FVF_LASTBETA_UBYTE4 = 0x00001000U,
	LEGACY_FVF_LASTBETA_PACKED_COLOR = 0x00008000U,
	LEGACY_FVF_TEXCOORD_MASK = 0xffff0000U,

	LEGACY_FVF_XYZNDUV1TG3 = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_DIFFUSE | LEGACY_FVF_TEX4 |
		(1U << 18) | (1U << 20) | (1U << 22),
	LEGACY_FVF_XYZNUV2DMAP = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_TEX3 | (3U << 16) | (2U << 18),
	LEGACY_FVF_XYZNDCUBEMAP = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_DIFFUSE
};

inline unsigned int LegacyFvfVertexSize(unsigned int fvf)
{
	const unsigned int KNOWN_MASK = LEGACY_FVF_POSITION_MASK |
		LEGACY_FVF_NORMAL | LEGACY_FVF_PSIZE | LEGACY_FVF_DIFFUSE |
		LEGACY_FVF_SPECULAR | LEGACY_FVF_TEXCOUNT_MASK |
		LEGACY_FVF_LASTBETA_UBYTE4 | LEGACY_FVF_LASTBETA_PACKED_COLOR |
		LEGACY_FVF_TEXCOORD_MASK;

	if ((fvf & ~KNOWN_MASK) != 0U)
	{
		return 0;
	}
	const unsigned int position = fvf & LEGACY_FVF_POSITION_MASK;
	unsigned int vertexSize;
	unsigned int blendFieldCount;
	switch (position)
	{
	case LEGACY_FVF_XYZ: vertexSize = 3U * sizeof(float); blendFieldCount = 0U; break;
	case LEGACY_FVF_XYZRHW: vertexSize = 4U * sizeof(float); blendFieldCount = 0U; break;
	case LEGACY_FVF_XYZB1: vertexSize = 4U * sizeof(float); blendFieldCount = 1U; break;
	case LEGACY_FVF_XYZB2: vertexSize = 5U * sizeof(float); blendFieldCount = 2U; break;
	case LEGACY_FVF_XYZB3: vertexSize = 6U * sizeof(float); blendFieldCount = 3U; break;
	case LEGACY_FVF_XYZB4: vertexSize = 7U * sizeof(float); blendFieldCount = 4U; break;
	case LEGACY_FVF_XYZB5: vertexSize = 8U * sizeof(float); blendFieldCount = 5U; break;
	default: return 0;
	}
	const unsigned int lastBeta = fvf &
		(LEGACY_FVF_LASTBETA_UBYTE4 | LEGACY_FVF_LASTBETA_PACKED_COLOR);
	if ((position == LEGACY_FVF_XYZRHW &&
			(fvf & LEGACY_FVF_NORMAL) != 0U) ||
		(blendFieldCount == 0U && lastBeta != 0U) ||
		lastBeta == (LEGACY_FVF_LASTBETA_UBYTE4 |
			LEGACY_FVF_LASTBETA_PACKED_COLOR))
	{
		return 0;
	}
	const unsigned int textureCount =
		(fvf & LEGACY_FVF_TEXCOUNT_MASK) >> LEGACY_FVF_TEXCOUNT_SHIFT;
	if (textureCount > LEGACY_TEXTURE_STAGE_COUNT)
	{
		return 0;
	}
	if ((fvf & LEGACY_FVF_NORMAL) != 0U) vertexSize += 3U * sizeof(float);
	if ((fvf & LEGACY_FVF_PSIZE) != 0U) vertexSize += sizeof(float);
	if ((fvf & LEGACY_FVF_DIFFUSE) != 0U) vertexSize += sizeof(unsigned int);
	if ((fvf & LEGACY_FVF_SPECULAR) != 0U) vertexSize += sizeof(unsigned int);
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		const unsigned int encoding = (fvf >> (16U + stage * 2U)) & 3U;
		if (stage >= textureCount)
		{
			if (encoding != 0U) return 0;
			continue;
		}
		const unsigned int componentCount = encoding == 0U ? 2U :
			(encoding == 1U ? 3U : (encoding == 2U ? 4U : 1U));
		vertexSize += componentCount * sizeof(float);
	}
	return vertexSize;
}

inline unsigned int LegacyFvfBlendFieldCount(unsigned int fvf)
{
	switch (fvf & LEGACY_FVF_POSITION_MASK)
	{
	case LEGACY_FVF_XYZB1: return 1U;
	case LEGACY_FVF_XYZB2: return 2U;
	case LEGACY_FVF_XYZB3: return 3U;
	case LEGACY_FVF_XYZB4: return 4U;
	case LEGACY_FVF_XYZB5: return 5U;
	default: return 0U;
	}
}

inline bool LegacyFvfHasLastBeta(unsigned int fvf)
{
	return (fvf & (LEGACY_FVF_LASTBETA_UBYTE4 |
		LEGACY_FVF_LASTBETA_PACKED_COLOR)) != 0U;
}

// Decode the serialized legacy FVF bit layout without including a graphics
// API header. The numeric values are part of the vertex packet format consumed
// by old WW3D assets; they are not a native device or import-library dependency.
inline bool DecodeLegacyFvfVertexLayout(unsigned int fvf,
	unsigned int vertexStride, RenderVertexLayout *layout)
{
	const unsigned int KNOWN_MASK = LEGACY_FVF_POSITION_MASK |
		LEGACY_FVF_NORMAL | LEGACY_FVF_PSIZE | LEGACY_FVF_DIFFUSE |
		LEGACY_FVF_SPECULAR | LEGACY_FVF_TEXCOUNT_MASK |
		LEGACY_FVF_LASTBETA_UBYTE4 | LEGACY_FVF_LASTBETA_PACKED_COLOR |
		LEGACY_FVF_TEXCOORD_MASK;

	if (layout == 0 || vertexStride == 0 || (fvf & ~KNOWN_MASK) != 0U)
	{
		return false;
	}
	const unsigned int requiredStride = LegacyFvfVertexSize(fvf);
	if (requiredStride == 0U || vertexStride < requiredStride)
	{
		return false;
	}

	const unsigned int position = fvf & LEGACY_FVF_POSITION_MASK;
	if ((position == LEGACY_FVF_XYZRHW &&
			(fvf & LEGACY_FVF_NORMAL) != 0U) ||
		(LegacyFvfBlendFieldCount(fvf) == 0U && LegacyFvfHasLastBeta(fvf)))
	{
		return false;
	}

	const unsigned int textureCount =
		(fvf & LEGACY_FVF_TEXCOUNT_MASK) >> LEGACY_FVF_TEXCOUNT_SHIFT;
	if (textureCount > LEGACY_TEXTURE_STAGE_COUNT)
	{
		return false;
	}

	unsigned int textureComponents[LEGACY_TEXTURE_STAGE_COUNT];
	unsigned int stage;
	for (stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		const unsigned int encoding = (fvf >> (16U + stage * 2U)) & 3U;
		if (stage >= textureCount)
		{
			if (encoding != 0U)
			{
				return false;
			}
			textureComponents[stage] = 0U;
			continue;
		}
		switch (encoding)
		{
		case 0U: textureComponents[stage] = 2U; break;
		case 1U: textureComponents[stage] = 3U; break;
		case 2U: textureComponents[stage] = 4U; break;
		default: textureComponents[stage] = 1U; break;
		}
	}

	RenderVertexLayout decoded;
	decoded.stride = vertexStride;
	decoded.preTransformed = position == LEGACY_FVF_XYZRHW;
	unsigned int offset = decoded.preTransformed ? 4U * sizeof(float) :
		3U * sizeof(float);
	RenderVertexElement &positionElement =
		decoded.elements[decoded.elementCount++];
	positionElement.semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	positionElement.semanticIndex = 0;
	positionElement.format = decoded.preTransformed ?
		RENDER_VERTEX_DATA_FLOAT4 : RENDER_VERTEX_DATA_FLOAT3;
	positionElement.byteOffset = 0;

	// XYZB<n> stores n four-byte blend fields immediately after XYZ.  In the
	// ordinary form all fields are floating-point weights.  LASTBETA replaces
	// the final field with either UBYTE4 or packed-color indices, while preserving
	// the serialized stride and all subsequent offsets.
	const unsigned int blendFieldCount = LegacyFvfBlendFieldCount(fvf);
	const bool hasLastBeta = LegacyFvfHasLastBeta(fvf);
	const unsigned int blendWeightCount = hasLastBeta ?
		blendFieldCount - 1U : blendFieldCount;
	if (blendWeightCount != 0U)
	{
		if (decoded.elementCount >= RenderVertexLayout::MAX_ELEMENT_COUNT)
		{
			return false;
		}
		RenderVertexElement &element = decoded.elements[decoded.elementCount++];
		element.semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
		element.semanticIndex = 0;
		switch (blendWeightCount > 4U ? 4U : blendWeightCount)
		{
		case 1U: element.format = RENDER_VERTEX_DATA_FLOAT1; break;
		case 2U: element.format = RENDER_VERTEX_DATA_FLOAT2; break;
		case 3U: element.format = RENDER_VERTEX_DATA_FLOAT3; break;
		default: element.format = RENDER_VERTEX_DATA_FLOAT4; break;
		}
		element.byteOffset = offset;
		offset += (blendWeightCount > 4U ? 4U : blendWeightCount) *
			sizeof(float);
		if (blendWeightCount > 4U)
		{
			if (decoded.elementCount >= RenderVertexLayout::MAX_ELEMENT_COUNT)
			{
				return false;
			}
			RenderVertexElement &remaining =
				decoded.elements[decoded.elementCount++];
			remaining.semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
			remaining.semanticIndex = 1;
			remaining.format = RENDER_VERTEX_DATA_FLOAT1;
			remaining.byteOffset = offset;
			offset += sizeof(float);
		}
	}
	if (hasLastBeta)
	{
		if (decoded.elementCount >= RenderVertexLayout::MAX_ELEMENT_COUNT)
		{
			return false;
		}
		RenderVertexElement &element = decoded.elements[decoded.elementCount++];
		element.semantic = RENDER_VERTEX_SEMANTIC_BLEND_INDEX;
		element.semanticIndex = 0;
		element.format = (fvf & LEGACY_FVF_LASTBETA_UBYTE4) != 0U ?
			RENDER_VERTEX_DATA_UBYTE4 : RENDER_VERTEX_DATA_PACKED_COLOR;
		element.byteOffset = offset;
		offset += sizeof(unsigned int);
	}

	if ((fvf & LEGACY_FVF_NORMAL) != 0U)
	{
		RenderVertexElement &element = decoded.elements[decoded.elementCount++];
		element.semantic = RENDER_VERTEX_SEMANTIC_NORMAL;
		element.semanticIndex = 0;
		element.format = RENDER_VERTEX_DATA_FLOAT3;
		element.byteOffset = offset;
		offset += 3U * sizeof(float);
	}
	// FVF stream order is position/blend, normal, point size, diffuse,
	// specular, then texture coordinates.  Point size has no consumer in the
	// current fixed-function shader signature, but its bytes must still be
	// consumed here so every following element retains its serialized offset.
	if ((fvf & LEGACY_FVF_PSIZE) != 0U)
	{
		offset += sizeof(float);
	}
	if ((fvf & LEGACY_FVF_DIFFUSE) != 0U)
	{
		RenderVertexElement &element = decoded.elements[decoded.elementCount++];
		element.semantic = RENDER_VERTEX_SEMANTIC_DIFFUSE;
		element.semanticIndex = 0;
		element.format = RENDER_VERTEX_DATA_COLOR_BGRA8;
		element.byteOffset = offset;
		offset += sizeof(unsigned int);
	}
	if ((fvf & LEGACY_FVF_SPECULAR) != 0U)
	{
		RenderVertexElement &element = decoded.elements[decoded.elementCount++];
		element.semantic = RENDER_VERTEX_SEMANTIC_SPECULAR;
		element.semanticIndex = 0;
		element.format = RENDER_VERTEX_DATA_COLOR_BGRA8;
		element.byteOffset = offset;
		offset += sizeof(unsigned int);
	}

	for (stage = 0; stage < textureCount; ++stage)
	{
		if (decoded.elementCount >= RenderVertexLayout::MAX_ELEMENT_COUNT)
		{
			return false;
		}
		RenderVertexElement &element = decoded.elements[decoded.elementCount++];
		element.semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		element.semanticIndex = stage;
		switch (textureComponents[stage])
		{
		case 1U: element.format = RENDER_VERTEX_DATA_FLOAT1; break;
		case 2U: element.format = RENDER_VERTEX_DATA_FLOAT2; break;
		case 3U: element.format = RENDER_VERTEX_DATA_FLOAT3; break;
		default: element.format = RENDER_VERTEX_DATA_FLOAT4; break;
		}
		element.byteOffset = offset;
		offset += textureComponents[stage] * sizeof(float);
	}

	if (offset != requiredStride)
	{
		return false;
	}
	*layout = decoded;
	return true;
}

}
}

#endif
