#include "WW3D2/dx8fvf.h"
#include "Renderer/LegacyFvfLayout.h"

#include <stdio.h>

namespace
{
const unsigned kPsizeFlag = rts::render::LEGACY_FVF_PSIZE;
const unsigned kLastBetaD3DColorFlag = rts::render::LEGACY_FVF_LASTBETA_D3DCOLOR;

int check(bool condition, const char *testName, const char *expression)
{
	if (!condition)
	{
		fprintf(stderr, "%s: %s\n", testName, expression);
		return 1;
	}
	return 0;
}

#define CHECK(testName, expression) \
	do { if (check((expression), testName, #expression) != 0) return 1; } while (0)

int testPositionSizes()
{
	static const unsigned positions[] = {
		rts::render::LEGACY_FVF_XYZ,
		rts::render::LEGACY_FVF_XYZRHW,
		rts::render::LEGACY_FVF_XYZB1,
		rts::render::LEGACY_FVF_XYZB2,
		rts::render::LEGACY_FVF_XYZB3,
		rts::render::LEGACY_FVF_XYZB4,
		rts::render::LEGACY_FVF_XYZB5
	};
	static const unsigned sizes[] = { 12, 16, 16, 20, 24, 28, 32 };
	unsigned index;

	for (index = 0; index < sizeof(positions) / sizeof(positions[0]); ++index)
	{
		FVFInfoClass info(positions[index]);
		CHECK("position sizes", info.Get_FVF_Size() == sizes[index]);
		CHECK("neutral position sizes",
			rts::render::LegacyFvfVertexSize(positions[index]) == sizes[index]);
	}

	for (index = 2; index < sizeof(positions) / sizeof(positions[0]); ++index)
	{
		CHECK("last beta sizes",
			FVFInfoClass(positions[index] | rts::render::LEGACY_FVF_LASTBETA_UBYTE4).
				Get_FVF_Size() == sizes[index]);
		CHECK("last beta color sizes",
			FVFInfoClass(positions[index] | kLastBetaD3DColorFlag).
				Get_FVF_Size() == sizes[index]);
		CHECK("neutral last beta sizes",
			rts::render::LegacyFvfVertexSize(positions[index] |
				rts::render::LEGACY_FVF_LASTBETA_UBYTE4) == sizes[index]);
		CHECK("neutral last beta color sizes",
			rts::render::LegacyFvfVertexSize(positions[index] |
				kLastBetaD3DColorFlag) == sizes[index]);
	}
	return 0;
}

int testWeightedVertexOffsets()
{
	static const unsigned positions[] = {
		rts::render::LEGACY_FVF_XYZB1,
		rts::render::LEGACY_FVF_XYZB2,
		rts::render::LEGACY_FVF_XYZB3,
		rts::render::LEGACY_FVF_XYZB4,
		rts::render::LEGACY_FVF_XYZB5
	};
	static const unsigned positionBytes[] = { 16, 20, 24, 28, 32 };
	for (unsigned index = 0; index < sizeof(positions) / sizeof(positions[0]);
		++index)
	{
		const unsigned fvf = positions[index] |
			rts::render::LEGACY_FVF_NORMAL |
			rts::render::LEGACY_FVF_DIFFUSE |
			rts::render::LEGACY_FVF_TEX1;
		FVFInfoClass info(fvf);
		CHECK("weighted FVF stride",
			info.Get_FVF_Size() == positionBytes[index] + 24);
		CHECK("weighted location offset", info.Get_Location_Offset() == 0);
#if defined(_WIN64)
		CHECK("weighted first blend offset",
			info.Get_Blend_Weight_Offset() == 12);
		CHECK("weighted blend index absent",
			info.Get_Blend_Index_Offset() == 0);
#endif
		CHECK("weighted normal offset",
			info.Get_Normal_Offset() == positionBytes[index]);
		CHECK("weighted diffuse offset",
			info.Get_Diffuse_Offset() == positionBytes[index] + 12);
		CHECK("weighted texture offset",
			info.Get_Tex_Offset(0) == positionBytes[index] + 16);

		const unsigned ubyte4Fvf = fvf |
			rts::render::LEGACY_FVF_LASTBETA_UBYTE4;
		const unsigned colorFvf = fvf |
			rts::render::LEGACY_FVF_LASTBETA_D3DCOLOR;
		CHECK("weighted LASTBETA_UBYTE4 stride",
			FVFInfoClass(ubyte4Fvf).Get_FVF_Size() == positionBytes[index] + 24);
		CHECK("weighted LASTBETA_UBYTE4 offsets",
			FVFInfoClass(ubyte4Fvf).Get_Normal_Offset() == positionBytes[index] &&
			FVFInfoClass(ubyte4Fvf).Get_Tex_Offset(0) == positionBytes[index] + 16);
#if defined(_WIN64)
		CHECK("weighted LASTBETA_UBYTE4 blend offsets",
			FVFInfoClass(ubyte4Fvf).Get_Blend_Weight_Offset() ==
				(index == 0 ? 0U : 12U) &&
			FVFInfoClass(ubyte4Fvf).Get_Blend_Index_Offset() ==
				12 + index * 4);
#endif
		CHECK("weighted LASTBETA_D3DCOLOR stride",
			FVFInfoClass(colorFvf).Get_FVF_Size() == positionBytes[index] + 24);
		CHECK("weighted LASTBETA_D3DCOLOR offsets",
			FVFInfoClass(colorFvf).Get_Normal_Offset() == positionBytes[index] &&
			FVFInfoClass(colorFvf).Get_Tex_Offset(0) == positionBytes[index] + 16);
#if defined(_WIN64)
		CHECK("weighted LASTBETA_D3DCOLOR blend offsets",
			FVFInfoClass(colorFvf).Get_Blend_Weight_Offset() ==
				(index == 0 ? 0U : 12U) &&
			FVFInfoClass(colorFvf).Get_Blend_Index_Offset() ==
				12 + index * 4);
#endif
	}
	return 0;
}

int testVertexAttributes()
{
	static const unsigned attributes[] = {
		0,
		rts::render::LEGACY_FVF_NORMAL,
		kPsizeFlag,
		rts::render::LEGACY_FVF_DIFFUSE,
		rts::render::LEGACY_FVF_SPECULAR,
		rts::render::LEGACY_FVF_NORMAL | kPsizeFlag,
		rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_DIFFUSE,
		rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_SPECULAR,
		kPsizeFlag | rts::render::LEGACY_FVF_DIFFUSE,
		kPsizeFlag | rts::render::LEGACY_FVF_SPECULAR,
		rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_SPECULAR,
		rts::render::LEGACY_FVF_NORMAL | kPsizeFlag | rts::render::LEGACY_FVF_DIFFUSE,
		rts::render::LEGACY_FVF_NORMAL | kPsizeFlag | rts::render::LEGACY_FVF_SPECULAR,
		rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_SPECULAR,
		kPsizeFlag | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_SPECULAR,
		rts::render::LEGACY_FVF_NORMAL | kPsizeFlag | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_SPECULAR
	};
	unsigned index;

	for (index = 0; index < sizeof(attributes) / sizeof(attributes[0]); ++index)
	{
		FVFInfoClass info(rts::render::LEGACY_FVF_XYZ | attributes[index]);
		const unsigned attributeCount =
			((attributes[index] & rts::render::LEGACY_FVF_NORMAL) != 0 ? 12 : 0) +
			((attributes[index] & kPsizeFlag) != 0 ? 4 : 0) +
			((attributes[index] & rts::render::LEGACY_FVF_DIFFUSE) != 0 ? 4 : 0) +
			((attributes[index] & rts::render::LEGACY_FVF_SPECULAR) != 0 ? 4 : 0);
		CHECK("vertex attributes", info.Get_FVF_Size() == 12 + attributeCount);
		CHECK("neutral vertex attributes",
			rts::render::LegacyFvfVertexSize(rts::render::LEGACY_FVF_XYZ | attributes[index]) ==
				12 + attributeCount);
	}

	CHECK("RHW point size",
		FVFInfoClass(rts::render::LEGACY_FVF_XYZRHW | kPsizeFlag).Get_FVF_Size() == 20);
	CHECK("RHW color attributes",
		FVFInfoClass(rts::render::LEGACY_FVF_XYZRHW | kPsizeFlag |
			rts::render::LEGACY_FVF_DIFFUSE |
			rts::render::LEGACY_FVF_SPECULAR).Get_FVF_Size() == 28);
	CHECK("water mesh FVF",
		FVFInfoClass(DX8_FVF_XYZDUV2).Get_FVF_Size() == 32);
	CHECK("pretransformed terrain FVF",
		FVFInfoClass(rts::render::LEGACY_FVF_XYZRHW |
			rts::render::LEGACY_FVF_DIFFUSE |
			rts::render::LEGACY_FVF_TEX2).Get_FVF_Size() == 36);
	CHECK("dynamic UI FVF",
		FVFInfoClass(rts::render::LEGACY_FVF_XYZ |
			rts::render::LEGACY_FVF_NORMAL |
			rts::render::LEGACY_FVF_DIFFUSE |
			rts::render::LEGACY_FVF_TEX2).Get_FVF_Size() == 44);
	return 0;
}

int checkAllEightTextureEncodings(unsigned stage, unsigned fvf, unsigned expected,
	const unsigned *componentCounts)
{
	unsigned encoding;

	if (stage == 8)
	{
		FVFInfoClass info(fvf);
		return check(info.Get_FVF_Size() == expected &&
			rts::render::LegacyFvfVertexSize(fvf) == expected,
			"all texture coordinate encodings",
			"legacy and neutral FVF sizes equal expected");
	}

	for (encoding = 0; encoding < 4; ++encoding)
	{
		const unsigned field = encoding << (16 + stage * 2);
		const unsigned encodingSize = componentCounts[encoding] * 4;
		const unsigned defaultSize = 2 * 4;
		const int result = checkAllEightTextureEncodings(stage + 1,
			fvf | field, expected + encodingSize - defaultSize, componentCounts);
		if (result != 0)
			return result;
	}
	return 0;
}

int testTextureCoordinateSizes()
{
	static const unsigned componentCounts[] = { 2, 3, 4, 1 };
	unsigned textureCount;
	unsigned stage;
	unsigned encoding;

	for (textureCount = 0; textureCount <= 8; ++textureCount)
	{
		FVFInfoClass info(rts::render::LEGACY_FVF_XYZ |
			(textureCount << rts::render::LEGACY_FVF_TEXCOUNT_SHIFT));
		CHECK("texture count defaults", info.Get_FVF_Size() == 12 + textureCount * 8);
		CHECK("neutral texture count defaults",
			rts::render::LegacyFvfVertexSize(rts::render::LEGACY_FVF_XYZ |
				(textureCount << rts::render::LEGACY_FVF_TEXCOUNT_SHIFT)) ==
				12 + textureCount * 8);
	}

	for (textureCount = 1; textureCount <= 8; ++textureCount)
	{
		for (stage = 0; stage < textureCount; ++stage)
		{
			for (encoding = 0; encoding < 4; ++encoding)
			{
				const unsigned fvf = rts::render::LEGACY_FVF_XYZ |
					(textureCount << rts::render::LEGACY_FVF_TEXCOUNT_SHIFT) |
					(encoding << (16 + stage * 2));
				const unsigned expected = 12 + textureCount * 8 +
					componentCounts[encoding] * 4 - 8;
				FVFInfoClass info(fvf);
				CHECK("texture coordinate sizes", info.Get_FVF_Size() == expected);
				CHECK("neutral texture coordinate sizes",
					rts::render::LegacyFvfVertexSize(fvf) == expected);
			}
		}
	}

	/* Exhaust all 4^8 dimension combinations across stages 0 through 7. */
	CHECK("all texture coordinate encodings",
		checkAllEightTextureEncodings(0, rts::render::LEGACY_FVF_XYZ |
			rts::render::LEGACY_FVF_TEX8,
			12 + 8 * 8, componentCounts) == 0);
	return 0;
}

int testInvalidFormats()
{
	static const unsigned invalidFormats[] = {
		0,
		rts::render::LEGACY_FVF_RESERVED0,
		rts::render::LEGACY_FVF_XYZRHW | rts::render::LEGACY_FVF_NORMAL,
		rts::render::LEGACY_FVF_XYZRHW | rts::render::LEGACY_FVF_LASTBETA_UBYTE4,
		rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_LASTBETA_UBYTE4,
		rts::render::LEGACY_FVF_XYZB5 |
			rts::render::LEGACY_FVF_LASTBETA_UBYTE4 | kLastBetaD3DColorFlag,
		rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_TEX1 | (1u << 18),
		rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_TEX1 | (1u << 22),
		rts::render::LEGACY_FVF_XYZ |
		(9u << rts::render::LEGACY_FVF_TEXCOUNT_SHIFT),
		rts::render::LEGACY_FVF_XYZ | 0x00002000u,
		rts::render::LEGACY_FVF_XYZ | 0x40000000u
	};
	unsigned index;

	for (index = 0; index < sizeof(invalidFormats) / sizeof(invalidFormats[0]); ++index)
	{
		FVFInfoClass info(invalidFormats[index]);
		CHECK("invalid FVF rejection", info.Get_FVF_Size() == 0);
		CHECK("neutral invalid FVF rejection",
			rts::render::LegacyFvfVertexSize(invalidFormats[index]) == 0);
	}
	return 0;
}

int testNeutralVertexLayout()
{
	const unsigned fvf = rts::render::LEGACY_FVF_XYZ |
		rts::render::LEGACY_FVF_NORMAL |
		rts::render::LEGACY_FVF_DIFFUSE |
		rts::render::LEGACY_FVF_SPECULAR | rts::render::LEGACY_FVF_TEX2;
	const unsigned requiredStride = rts::render::LegacyFvfVertexSize(fvf);
	rts::render::RenderVertexLayout layout;
	CHECK("neutral layout decode",
		rts::render::DecodeLegacyFvfVertexLayout(fvf, requiredStride, &layout));
	CHECK("neutral layout stride", layout.stride == 48);
	CHECK("neutral layout element count", layout.elementCount == 6);
	CHECK("neutral layout position",
		layout.elements[0].semantic == rts::render::RENDER_VERTEX_SEMANTIC_POSITION &&
		layout.elements[0].format == rts::render::RENDER_VERTEX_DATA_FLOAT3 &&
		layout.elements[0].byteOffset == 0);
	CHECK("neutral layout normal",
		layout.elements[1].semantic == rts::render::RENDER_VERTEX_SEMANTIC_NORMAL &&
		layout.elements[1].byteOffset == 12);
	CHECK("neutral layout diffuse",
		layout.elements[2].semantic == rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE &&
		layout.elements[2].byteOffset == 24);
	CHECK("neutral layout specular",
		layout.elements[3].semantic == rts::render::RENDER_VERTEX_SEMANTIC_SPECULAR &&
		layout.elements[3].byteOffset == 28);
	CHECK("neutral layout texture offsets",
		layout.elements[4].semantic ==
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE &&
		layout.elements[4].semanticIndex == 0 &&
		layout.elements[4].byteOffset == 32 &&
		layout.elements[5].semanticIndex == 1 &&
		layout.elements[5].byteOffset == 40);

	const unsigned paddedStride = requiredStride + 16;
	CHECK("neutral padded packet stride",
		rts::render::DecodeLegacyFvfVertexLayout(fvf, paddedStride, &layout) &&
		layout.stride == paddedStride);
	CHECK("neutral short packet rejection",
		!rts::render::DecodeLegacyFvfVertexLayout(fvf, requiredStride - 1,
			&layout));
	CHECK("neutral blended layout",
		rts::render::DecodeLegacyFvfVertexLayout(rts::render::LEGACY_FVF_XYZB1,
			rts::render::LegacyFvfVertexSize(rts::render::LEGACY_FVF_XYZB1), &layout) &&
		layout.elementCount == 2 &&
		layout.elements[1].semantic ==
			rts::render::RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT &&
		layout.elements[1].semanticIndex == 0 &&
		layout.elements[1].format == rts::render::RENDER_VERTEX_DATA_FLOAT1 &&
		layout.elements[1].byteOffset == 12);

	const unsigned rhwFvf = rts::render::LEGACY_FVF_XYZRHW | kPsizeFlag |
		rts::render::LEGACY_FVF_DIFFUSE |
		rts::render::LEGACY_FVF_SPECULAR | rts::render::LEGACY_FVF_TEX1;
	CHECK("neutral pretransformed layout",
		rts::render::DecodeLegacyFvfVertexLayout(rhwFvf,
			rts::render::LegacyFvfVertexSize(rhwFvf), &layout) &&
		layout.preTransformed && layout.elementCount == 4 &&
		layout.elements[0].format == rts::render::RENDER_VERTEX_DATA_FLOAT4 &&
		layout.elements[1].byteOffset == 20 &&
		layout.elements[2].byteOffset == 24 &&
		layout.elements[3].byteOffset == 28);
	return 0;
}

const rts::render::RenderVertexElement *FindElement(
	const rts::render::RenderVertexLayout &layout,
	rts::render::RenderVertexSemantic semantic, unsigned int semanticIndex)
{
	for (unsigned int index = 0; index < layout.elementCount; ++index)
	{
		if (layout.elements[index].semantic == semantic &&
			layout.elements[index].semanticIndex == semanticIndex)
		{
			return &layout.elements[index];
		}
	}
	return 0;
}

int testAllWeightedNeutralLayouts()
{
	static const unsigned positions[] = {
		rts::render::LEGACY_FVF_XYZB1,
		rts::render::LEGACY_FVF_XYZB2,
		rts::render::LEGACY_FVF_XYZB3,
		rts::render::LEGACY_FVF_XYZB4,
		rts::render::LEGACY_FVF_XYZB5
	};
	static const unsigned blendFieldCounts[] = { 1, 2, 3, 4, 5 };
	for (unsigned int positionIndex = 0; positionIndex < 5; ++positionIndex)
	{
		for (unsigned int betaKind = 0; betaKind < 3; ++betaKind)
		{
			const unsigned int betaFlag = betaKind == 0 ? 0U :
				(betaKind == 1 ? rts::render::LEGACY_FVF_LASTBETA_UBYTE4 :
				 rts::render::LEGACY_FVF_LASTBETA_D3DCOLOR);
			const unsigned int fvf = positions[positionIndex] | betaFlag |
				rts::render::LEGACY_FVF_NORMAL |
				rts::render::LEGACY_FVF_PSIZE |
				rts::render::LEGACY_FVF_DIFFUSE |
				rts::render::LEGACY_FVF_SPECULAR |
				rts::render::LEGACY_FVF_TEX8;
			const unsigned int stride = rts::render::LegacyFvfVertexSize(fvf);
			rts::render::RenderVertexLayout layout;
			CHECK("weighted neutral layout decode",
				stride != 0 && rts::render::DecodeLegacyFvfVertexLayout(
					fvf, stride, &layout));

			const unsigned int fieldCount = blendFieldCounts[positionIndex];
			const unsigned int weightCount = betaKind == 0 ? fieldCount :
				fieldCount - 1;
			const unsigned int expectedWeightElements = weightCount == 0 ? 0U :
				(weightCount > 4 ? 2U : 1U);
			const unsigned int expectedElements = 1U + expectedWeightElements +
				(betaKind == 0 ? 0U : 1U) + 1U + 1U + 1U + 8U;
			CHECK("weighted neutral element count",
				layout.elementCount == expectedElements &&
				layout.elementCount <=
					rts::render::RenderVertexLayout::MAX_ELEMENT_COUNT);
			const rts::render::RenderVertexElement *weight0 = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT, 0);
			const rts::render::RenderVertexElement *weight1 = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT, 1);
			const rts::render::RenderVertexElement *indices = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_BLEND_INDEX, 0);
			if (weightCount != 0)
			{
				const unsigned int firstWeightCount = weightCount > 4 ? 4 :
					weightCount;
				const rts::render::RenderVertexDataFormat expectedFormat =
					firstWeightCount == 1 ? rts::render::RENDER_VERTEX_DATA_FLOAT1 :
					(firstWeightCount == 2 ? rts::render::RENDER_VERTEX_DATA_FLOAT2 :
					 (firstWeightCount == 3 ? rts::render::RENDER_VERTEX_DATA_FLOAT3 :
					  rts::render::RENDER_VERTEX_DATA_FLOAT4));
				CHECK("weighted neutral first weight",
					weight0 != 0 && weight0->format == expectedFormat &&
					weight0->byteOffset == 12);
				if (weightCount > 4)
				{
					CHECK("weighted neutral fifth weight",
						weight1 != 0 &&
						weight1->format == rts::render::RENDER_VERTEX_DATA_FLOAT1 &&
						weight1->byteOffset == 28);
				}
				else
				{
					CHECK("weighted neutral no second weight", weight1 == 0);
				}
			}
			else
			{
				CHECK("weighted neutral no weight", weight0 == 0 && weight1 == 0);
			}
			if (betaKind != 0)
			{
				CHECK("weighted neutral blend index",
					indices != 0 && indices->byteOffset ==
						12 + (fieldCount - 1) * 4 &&
						indices->format == (betaKind == 1 ?
							rts::render::RENDER_VERTEX_DATA_UBYTE4 :
							rts::render::RENDER_VERTEX_DATA_D3DCOLOR));
			}
			else
			{
				CHECK("weighted neutral no blend index", indices == 0);
			}
			const unsigned int positionBytes = 12U + fieldCount * 4U;
			const rts::render::RenderVertexElement *normal = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_NORMAL, 0);
			const rts::render::RenderVertexElement *diffuse = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE, 0);
			const rts::render::RenderVertexElement *specular = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_SPECULAR, 0);
			const rts::render::RenderVertexElement *texture0 = FindElement(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE, 0);
		CHECK("weighted neutral trailing offsets",
				normal != 0 && normal->byteOffset == positionBytes &&
				diffuse != 0 && diffuse->byteOffset == positionBytes + 16 &&
				specular != 0 && specular->byteOffset == positionBytes + 20 &&
				texture0 != 0 && texture0->byteOffset == positionBytes + 24);
#if defined(_WIN64)
			FVFInfoClass info(fvf);
			CHECK("weighted info first field offset",
				info.Get_Blend_Weight_Offset() ==
					(weightCount == 0 ? 0U : 12U));
			CHECK("weighted info index offset",
				info.Get_Blend_Index_Offset() == (betaKind == 0 ? 0U :
					12U + (fieldCount - 1U) * 4U));
#endif
		}
	}
	return 0;
}
}

int main()
{
	int result = 0;
	result |= testPositionSizes();
	result |= testWeightedVertexOffsets();
	result |= testVertexAttributes();
	result |= testTextureCoordinateSizes();
	result |= testInvalidFormats();
	result |= testNeutralVertexLayout();
	result |= testAllWeightedNeutralLayouts();
	return result;
}
