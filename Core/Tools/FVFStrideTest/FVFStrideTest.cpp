#include "WW3D2/dx8fvf.h"
#include "Renderer/LegacyFvfLayout.h"

#include <stdio.h>

namespace
{
const unsigned kPsizeFlag = 0x00000020u;
const unsigned kLastBetaD3DColorFlag = 0x00008000u;

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
		D3DFVF_XYZ,
		D3DFVF_XYZRHW,
		D3DFVF_XYZB1,
		D3DFVF_XYZB2,
		D3DFVF_XYZB3,
		D3DFVF_XYZB4,
		D3DFVF_XYZB5
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
			FVFInfoClass(positions[index] | D3DFVF_LASTBETA_UBYTE4).
				Get_FVF_Size() == sizes[index]);
		CHECK("last beta color sizes",
			FVFInfoClass(positions[index] | kLastBetaD3DColorFlag).
				Get_FVF_Size() == sizes[index]);
		CHECK("neutral last beta sizes",
			rts::render::LegacyFvfVertexSize(positions[index] |
				D3DFVF_LASTBETA_UBYTE4) == sizes[index]);
		CHECK("neutral last beta color sizes",
			rts::render::LegacyFvfVertexSize(positions[index] |
				kLastBetaD3DColorFlag) == sizes[index]);
	}
	return 0;
}

int testVertexAttributes()
{
	static const unsigned attributes[] = {
		0,
		D3DFVF_NORMAL,
		kPsizeFlag,
		D3DFVF_DIFFUSE,
		D3DFVF_SPECULAR,
		D3DFVF_NORMAL | kPsizeFlag,
		D3DFVF_NORMAL | D3DFVF_DIFFUSE,
		D3DFVF_NORMAL | D3DFVF_SPECULAR,
		kPsizeFlag | D3DFVF_DIFFUSE,
		kPsizeFlag | D3DFVF_SPECULAR,
		D3DFVF_DIFFUSE | D3DFVF_SPECULAR,
		D3DFVF_NORMAL | kPsizeFlag | D3DFVF_DIFFUSE,
		D3DFVF_NORMAL | kPsizeFlag | D3DFVF_SPECULAR,
		D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR,
		kPsizeFlag | D3DFVF_DIFFUSE | D3DFVF_SPECULAR,
		D3DFVF_NORMAL | kPsizeFlag | D3DFVF_DIFFUSE | D3DFVF_SPECULAR
	};
	unsigned index;

	for (index = 0; index < sizeof(attributes) / sizeof(attributes[0]); ++index)
	{
		FVFInfoClass info(D3DFVF_XYZ | attributes[index]);
		const unsigned attributeCount =
			((attributes[index] & D3DFVF_NORMAL) != 0 ? 12 : 0) +
			((attributes[index] & kPsizeFlag) != 0 ? 4 : 0) +
			((attributes[index] & D3DFVF_DIFFUSE) != 0 ? 4 : 0) +
			((attributes[index] & D3DFVF_SPECULAR) != 0 ? 4 : 0);
		CHECK("vertex attributes", info.Get_FVF_Size() == 12 + attributeCount);
		CHECK("neutral vertex attributes",
			rts::render::LegacyFvfVertexSize(D3DFVF_XYZ | attributes[index]) ==
				12 + attributeCount);
	}

	CHECK("RHW point size",
		FVFInfoClass(D3DFVF_XYZRHW | kPsizeFlag).Get_FVF_Size() == 20);
	CHECK("RHW color attributes",
		FVFInfoClass(D3DFVF_XYZRHW | kPsizeFlag | D3DFVF_DIFFUSE |
			D3DFVF_SPECULAR).Get_FVF_Size() == 28);
	CHECK("water mesh FVF",
		FVFInfoClass(DX8_FVF_XYZDUV2).Get_FVF_Size() == 32);
	CHECK("pretransformed terrain FVF",
		FVFInfoClass(D3DFVF_XYZRHW | D3DFVF_DIFFUSE |
			D3DFVF_TEX2).Get_FVF_Size() == 36);
	CHECK("dynamic UI FVF",
		FVFInfoClass(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
			D3DFVF_TEX2).Get_FVF_Size() == 44);
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
		FVFInfoClass info(D3DFVF_XYZ | (textureCount << D3DFVF_TEXCOUNT_SHIFT));
		CHECK("texture count defaults", info.Get_FVF_Size() == 12 + textureCount * 8);
		CHECK("neutral texture count defaults",
			rts::render::LegacyFvfVertexSize(D3DFVF_XYZ |
				(textureCount << D3DFVF_TEXCOUNT_SHIFT)) ==
				12 + textureCount * 8);
	}

	for (textureCount = 1; textureCount <= 8; ++textureCount)
	{
		for (stage = 0; stage < textureCount; ++stage)
		{
			for (encoding = 0; encoding < 4; ++encoding)
			{
				const unsigned fvf = D3DFVF_XYZ |
					(textureCount << D3DFVF_TEXCOUNT_SHIFT) |
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
		checkAllEightTextureEncodings(0, D3DFVF_XYZ | D3DFVF_TEX8,
			12 + 8 * 8, componentCounts) == 0);
	return 0;
}

int testInvalidFormats()
{
	static const unsigned invalidFormats[] = {
		0,
		D3DFVF_RESERVED0,
		D3DFVF_XYZRHW | D3DFVF_NORMAL,
		D3DFVF_XYZRHW | D3DFVF_LASTBETA_UBYTE4,
		D3DFVF_XYZ | D3DFVF_LASTBETA_UBYTE4,
		D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4 | kLastBetaD3DColorFlag,
		D3DFVF_XYZ | D3DFVF_TEX1 | (1u << 18),
		D3DFVF_XYZ | D3DFVF_TEX1 | (1u << 22),
		D3DFVF_XYZ | (9u << D3DFVF_TEXCOUNT_SHIFT),
		D3DFVF_XYZ | 0x00002000u,
		D3DFVF_XYZ | 0x40000000u
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
	const unsigned fvf = D3DFVF_XYZ | D3DFVF_NORMAL |
		D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2;
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
	CHECK("neutral blended layout rejection",
		!rts::render::DecodeLegacyFvfVertexLayout(D3DFVF_XYZB1,
			rts::render::LegacyFvfVertexSize(D3DFVF_XYZB1), &layout));

	const unsigned rhwFvf = D3DFVF_XYZRHW | kPsizeFlag |
		D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
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
}

int main()
{
	int result = 0;
	result |= testPositionSizes();
	result |= testVertexAttributes();
	result |= testTextureCoordinateSizes();
	result |= testInvalidFormats();
	result |= testNeutralVertexLayout();
	return result;
}
