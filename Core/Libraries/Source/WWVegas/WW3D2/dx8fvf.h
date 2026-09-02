#ifndef RTS_WW3D2_NEUTRAL_FVF_H
#define RTS_WW3D2_NEUTRAL_FVF_H

#include "WWLib/always.h"
#include "Renderer/LegacyFvfLayout.h"

class StringClass;

// These names are retained at the WW3D source boundary because serialized
// meshes still carry the original FVF bit layout.  Their values come from the
// neutral renderer contract; no graphics SDK declaration is needed here.
enum
{
	DX8_MAX_TEXCOORD = rts::render::LEGACY_TEXTURE_STAGE_COUNT,
	DX8_FVF_POSITION_MASK = rts::render::LEGACY_FVF_POSITION_MASK,
	DX8_FVF_XYZRHW = rts::render::LEGACY_FVF_XYZRHW,
	DX8_FVF_XYZB1 = rts::render::LEGACY_FVF_XYZB1,
	DX8_FVF_XYZB2 = rts::render::LEGACY_FVF_XYZB2,
	DX8_FVF_XYZB3 = rts::render::LEGACY_FVF_XYZB3,
	DX8_FVF_XYZB4 = rts::render::LEGACY_FVF_XYZB4,
	DX8_FVF_XYZB5 = rts::render::LEGACY_FVF_XYZB5,
	DX8_FVF_NORMAL = rts::render::LEGACY_FVF_NORMAL,
	DX8_FVF_PSIZE = rts::render::LEGACY_FVF_PSIZE,
	DX8_FVF_DIFFUSE = rts::render::LEGACY_FVF_DIFFUSE,
	DX8_FVF_SPECULAR = rts::render::LEGACY_FVF_SPECULAR,
	DX8_FVF_TEX1 = rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_TEX2 = rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_TEX3 = rts::render::LEGACY_FVF_TEX3,
	DX8_FVF_TEX4 = rts::render::LEGACY_FVF_TEX4,
	DX8_FVF_TEX5 = rts::render::LEGACY_FVF_TEX5,
	DX8_FVF_TEX6 = rts::render::LEGACY_FVF_TEX6,
	DX8_FVF_TEX7 = rts::render::LEGACY_FVF_TEX7,
	DX8_FVF_TEX8 = rts::render::LEGACY_FVF_TEX8,
	DX8_FVF_TEXCOUNT_MASK = rts::render::LEGACY_FVF_TEXCOUNT_MASK,
	DX8_FVF_TEXCOUNT_SHIFT = rts::render::LEGACY_FVF_TEXCOUNT_SHIFT,
	DX8_FVF_LASTBETA_UBYTE4 = rts::render::LEGACY_FVF_LASTBETA_UBYTE4,
	DX8_FVF_LASTBETA_PACKED_COLOR = rts::render::LEGACY_FVF_LASTBETA_PACKED_COLOR,
	DX8_FVF_XYZ = rts::render::LEGACY_FVF_XYZ,
	DX8_FVF_XYZN = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_NORMAL,
	DX8_FVF_XYZNUV1 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_XYZNUV2 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_XYZNDUV1 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_XYZNDUV2 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_NORMAL | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_XYZDUV1 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_XYZDUV2 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_DIFFUSE | rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_XYZUV1 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_TEX1,
	DX8_FVF_XYZUV2 = rts::render::LEGACY_FVF_XYZ | rts::render::LEGACY_FVF_TEX2,
	DX8_FVF_XYZNDUV1TG3 = rts::render::LEGACY_FVF_XYZNDUV1TG3,
	DX8_FVF_XYZNUV2DMAP = rts::render::LEGACY_FVF_XYZNUV2DMAP,
	DX8_FVF_XYZNDCUBEMAP = rts::render::LEGACY_FVF_XYZNDCUBEMAP
};

struct VertexFormatXYZ { float x; float y; float z; };
struct VertexFormatXYZNUV1
{
	float x; float y; float z; float nx; float ny; float nz; float u1; float v1;
};
struct VertexFormatXYZNUV2
{
	float x; float y; float z; float nx; float ny; float nz;
	float u1; float v1; float u2; float v2;
};
struct VertexFormatXYZN
{
	float x; float y; float z; float nx; float ny; float nz;
};
struct VertexFormatXYZNDUV1
{
	float x; float y; float z; float nx; float ny; float nz;
	unsigned diffuse; float u1; float v1;
};
struct VertexFormatXYZNDUV2
{
	float x; float y; float z; float nx; float ny; float nz;
	unsigned diffuse; float u1; float v1; float u2; float v2;
};
struct VertexFormatXYZDUV1
{
	float x; float y; float z; unsigned diffuse; float u1; float v1;
};
struct VertexFormatXYZDUV2
{
	float x; float y; float z; unsigned diffuse;
	float u1; float v1; float u2; float v2;
};
struct VertexFormatXYZUV1
{
	float x; float y; float z; float u1; float v1;
};
struct VertexFormatXYZUV2
{
	float x; float y; float z; float u1; float v1; float u2; float v2;
};
struct VertexFormatXYZNDUV1TG3
{
	float x; float y; float z; float nx; float ny; float nz; unsigned diffuse;
	float u1; float v1; float Sx; float Sy; float Sz; float Tx; float Ty; float Tz;
	float SxTx; float SxTy; float SxTz;
};
struct VertexFormatXYZNUV2DMAP
{
	float x; float y; float z; float nx; float ny; float nz;
	float T1x; float T1y; float T1z; float T1w; float T2x; float T2y;
};
struct VertexFormatXYZNDCUBEMAP
{
	float x; float y; float z; float nx; float ny; float nz; unsigned diffuse;
};

class FVFInfoClass
{
	W3DMPO_CODE(FVFInfoClass)

	mutable unsigned FVF;
	mutable unsigned fvf_size;
	unsigned location_offset;
	unsigned normal_offset;
	unsigned blend_offset;
#if defined(_WIN64)
	unsigned blend_weight_offset;
	unsigned blend_index_offset;
#endif
	unsigned texcoord_offset[DX8_MAX_TEXCOORD];
	unsigned diffuse_offset;
	unsigned specular_offset;

public:
	FVFInfoClass(unsigned FVF);
	unsigned Get_Location_Offset() const { return location_offset; }
	unsigned Get_Normal_Offset() const { return normal_offset; }
	unsigned Get_Tex_Offset(unsigned int n) const { return texcoord_offset[n]; }
	unsigned Get_Diffuse_Offset() const { return diffuse_offset; }
	unsigned Get_Specular_Offset() const { return specular_offset; }
#if defined(_WIN64)
	unsigned Get_Blend_Weight_Offset() const { return blend_weight_offset; }
	unsigned Get_Blend_Index_Offset() const { return blend_index_offset; }
	bool Get_Native_Layout(rts::render::RenderVertexLayout *layout) const;
#endif
	unsigned Get_FVF() const { return FVF; }
	unsigned Get_FVF_Size() const { return fvf_size; }
	void Get_FVF_Name(StringClass &fvfname) const;
	void Set_FVF(unsigned fvf) const { FVF = fvf; }
	void Set_FVF_Size(unsigned size) const { fvf_size = size; }
};

#endif
