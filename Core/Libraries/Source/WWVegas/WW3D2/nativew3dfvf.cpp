#include <Utility/CppMacros.h>
#include "dx8fvf.h"
#include "WWLib/wwstring.h"

namespace
{
unsigned Native_FVF_Vertex_Size(unsigned fvf)
{
	return rts::render::LegacyFvfVertexSize(fvf);
}
}

FVFInfoClass::FVFInfoClass(unsigned fvf_)
	:
	FVF(fvf_),
	fvf_size(Native_FVF_Vertex_Size(fvf_)),
	location_offset(0),
	normal_offset(0),
	blend_offset(0),
	blend_weight_offset(0),
	blend_index_offset(0),
	diffuse_offset(0),
	specular_offset(0)
{
	for (unsigned int stage = 0; stage < DX8_MAX_TEXCOORD; ++stage)
		texcoord_offset[stage] = 0;

	if (fvf_size == 0)
		return;

	const unsigned int position = FVF & rts::render::LEGACY_FVF_POSITION_MASK;
	const unsigned int blend_field_count =
		rts::render::LegacyFvfBlendFieldCount(FVF);
	const unsigned int blend_position_offset = 3U * sizeof(float);
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
		if (!rts::render::LegacyFvfHasLastBeta(FVF) ||
			blend_field_count > 1U)
			blend_weight_offset = blend_position_offset;
		blend_offset = blend_position_offset +
			blend_field_count * sizeof(float);
		if (rts::render::LegacyFvfHasLastBeta(FVF))
		{
			blend_index_offset = blend_position_offset +
				(blend_field_count - 1U) * sizeof(float);
		}
		break;
	default:
		return;
	}

	normal_offset = blend_offset;
	diffuse_offset = normal_offset;
	if ((FVF & rts::render::LEGACY_FVF_NORMAL) != 0U)
		diffuse_offset += 3U * sizeof(float);
	if ((FVF & rts::render::LEGACY_FVF_PSIZE) != 0U)
		diffuse_offset += sizeof(float);
	specular_offset = diffuse_offset;
	if ((FVF & rts::render::LEGACY_FVF_DIFFUSE) != 0U)
		specular_offset += sizeof(unsigned int);

	unsigned int offset = specular_offset;
	if ((FVF & rts::render::LEGACY_FVF_SPECULAR) != 0U)
		offset += sizeof(unsigned int);
	const unsigned int texture_count =
		(FVF & rts::render::LEGACY_FVF_TEXCOUNT_MASK) >>
		rts::render::LEGACY_FVF_TEXCOUNT_SHIFT;
	for (unsigned int stage = 0; stage < texture_count &&
		stage < DX8_MAX_TEXCOORD; ++stage)
	{
		texcoord_offset[stage] = offset;
		const unsigned int encoding = (FVF >> (16U + stage * 2U)) & 3U;
		const unsigned int component_count = encoding == 0U ? 2U :
			(encoding == 1U ? 3U : (encoding == 2U ? 4U : 1U));
		offset += component_count * sizeof(float);
	}
}

bool FVFInfoClass::Get_Native_Layout(
	rts::render::RenderVertexLayout *layout) const
{
	return layout != 0 && fvf_size != 0 &&
		rts::render::DecodeLegacyFvfVertexLayout(FVF, fvf_size, layout);
}

void FVFInfoClass::Get_FVF_Name(StringClass &name) const
{
	switch (Get_FVF())
	{
	case DX8_FVF_XYZ: name = "LegacyFvf_XYZ"; break;
	case DX8_FVF_XYZN: name = "LegacyFvf_XYZ|LegacyFvf_NORMAL"; break;
	case DX8_FVF_XYZNUV1:
		name = "LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX1"; break;
	case DX8_FVF_XYZNUV2:
		name = "LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX2"; break;
	case DX8_FVF_XYZNDUV1:
		name = "LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX1|LegacyFvf_DIFFUSE";
		break;
	case DX8_FVF_XYZNDUV2:
		name = "LegacyFvf_XYZ|LegacyFvf_NORMAL|LegacyFvf_TEX2|LegacyFvf_DIFFUSE";
		break;
	case DX8_FVF_XYZDUV1:
		name = "LegacyFvf_XYZ|LegacyFvf_TEX1|LegacyFvf_DIFFUSE"; break;
	case DX8_FVF_XYZDUV2:
		name = "LegacyFvf_XYZ|LegacyFvf_TEX2|LegacyFvf_DIFFUSE"; break;
	case DX8_FVF_XYZUV1:
		name = "LegacyFvf_XYZ|LegacyFvf_TEX1"; break;
	case DX8_FVF_XYZUV2:
		name = "LegacyFvf_XYZ|LegacyFvf_TEX2"; break;
	case DX8_FVF_XYZNDUV1TG3: name = "LegacyFvf_XYZNDUV1TG3"; break;
	case DX8_FVF_XYZNUV2DMAP: name = "LegacyFvf_XYZNUV2DMAP"; break;
	case DX8_FVF_XYZNDCUBEMAP: name = "LegacyFvf_XYZNDCUBEMAP"; break;
	default: name = "Unknown!"; break;
	}
}
