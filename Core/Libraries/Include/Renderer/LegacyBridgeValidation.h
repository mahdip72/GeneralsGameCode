#ifndef RTS_RENDERER_LEGACYBRIDGEVALIDATION_H
#define RTS_RENDERER_LEGACYBRIDGEVALIDATION_H

namespace rts
{
namespace render
{

enum LegacyD3DPrimitiveType
{
	LEGACY_D3DPT_LINELIST = 2,
	LEGACY_D3DPT_LINESTRIP = 3,
	LEGACY_D3DPT_TRIANGLELIST = 4,
	LEGACY_D3DPT_TRIANGLESTRIP = 5
};

enum LegacyD3DRenderState
{
	LEGACY_D3DRS_DITHERENABLE = 26,
	LEGACY_D3DRS_FOGENABLE = 28,
	LEGACY_D3DRS_SPECULARENABLE = 29,
	LEGACY_D3DRS_FOGCOLOR = 34,
	LEGACY_D3DRS_FOGTABLEMODE = 35,
	LEGACY_D3DRS_FOGSTART = 36,
	LEGACY_D3DRS_FOGEND = 37,
	LEGACY_D3DRS_FOGDENSITY = 38,
	LEGACY_D3DRS_RANGEFOGENABLE = 48,
	LEGACY_D3DRS_WRAP0 = 128,
	LEGACY_D3DRS_WRAP1 = 129,
	LEGACY_D3DRS_WRAP2 = 130,
	LEGACY_D3DRS_WRAP3 = 131,
	LEGACY_D3DRS_WRAP4 = 132,
	LEGACY_D3DRS_WRAP5 = 133,
	LEGACY_D3DRS_WRAP6 = 134,
	LEGACY_D3DRS_WRAP7 = 135,
	LEGACY_D3DRS_CLIPPING = 136,
	LEGACY_D3DRS_AMBIENT = 139,
	LEGACY_D3DRS_FOGVERTEXMODE = 140,
	LEGACY_D3DRS_COLORVERTEX = 141,
	LEGACY_D3DRS_SOFTWAREVERTEXPROCESSING = 153,
	LEGACY_D3DRS_CULLMODE = 22
};

// The neutral renderer has no fixed-function viewer-location or N-patch
// state. These predicates encode the exact values that are harmless to
// accept at the legacy boundary; callers must reject every other value so a
// visible draw cannot silently reuse stale neutral state.
inline bool Is_D3D11_Local_Viewer_Value_Supported(unsigned int value)
{
	return value == 0U;
}

inline bool Is_D3D11_Patch_Segments_Value_Supported(unsigned int value)
{
	// D3DRS_PATCHSEGMENTS is a float bit pattern. 1.0f is the no-N-patch
	// neutral value used by the legacy shader path.
	return value == 0x3f800000U;
}

inline bool Is_D3D11_Clip_Plane_Mask_Supported(unsigned int value)
{
	// D3D8 exposes six user clip planes. Do not silently discard upper bits.
	return (value & ~0x3fU) == 0U;
}

inline bool Is_D3D11_Shade_Mode_Value_Supported(unsigned int value)
{
	// D3DSHADE_GOURAUD is the interpolated color route used by the neutral
	// shader. D3DSHADE_FLAT needs a provoking-vertex shader/input contract.
	return value == 2U;
}

inline bool Is_D3D11_Default_Render_State_Value_Supported(
	unsigned int state, unsigned int value)
{
	switch (state)
	{
	case LEGACY_D3DRS_COLORVERTEX:
	case LEGACY_D3DRS_CLIPPING:
		return value == 1U;
	case LEGACY_D3DRS_DITHERENABLE:
	case LEGACY_D3DRS_SOFTWAREVERTEXPROCESSING:
		return value == 0U;
	default:
		return false;
	}
}

enum LegacyVolumetricShadowShadeMode
{
	LEGACY_VOLUMETRIC_SHADOW_SHADE_FLAT = 1U,
	LEGACY_VOLUMETRIC_SHADOW_SHADE_GOURAUD = 2U
};

// The volumetric-shadow final quad writes one identical shadow color at every
// vertex, and the normal volume pass suppresses color writes (its release FVF
// is position-only).  Those are the only cases allowed to replace legacy Flat
// shading with the neutral renderer's Gouraud path.  Keep the D3D8 value when
// equivalence is not proven so the legacy backend retains its exact behavior.
inline unsigned int Select_D3D11_Volumetric_Shadow_Shade_Mode(
	bool d3d11Active, bool vertexColorsEquivalent)
{
	return d3d11Active && vertexColorsEquivalent ?
		LEGACY_VOLUMETRIC_SHADOW_SHADE_GOURAUD :
		LEGACY_VOLUMETRIC_SHADOW_SHADE_FLAT;
}

// These are the only legacy primitive types accepted by the neutral indexed
// draw path.  Keep the arithmetic checked before converting to D3D11's
// unsigned draw count.
inline bool Checked_D3D8_Primitive_Index_Count(unsigned int primitive_type,
	unsigned int primitive_count, unsigned int *index_count)
{
	if (index_count == 0)
	{
		return false;
	}
	unsigned long count = 0;
	unsigned long multiplier = 1;
	unsigned long addend = 0;
	switch (primitive_type)
	{
	case LEGACY_D3DPT_TRIANGLELIST:
		multiplier = 3;
		break;
	case LEGACY_D3DPT_TRIANGLESTRIP:
		addend = 2;
		break;
	case LEGACY_D3DPT_LINELIST:
		multiplier = 2;
		break;
	case LEGACY_D3DPT_LINESTRIP:
		addend = 1;
		break;
	default:
		return false;
	}
	if (multiplier != 1)
	{
		if (static_cast<unsigned long>(primitive_count) >
			static_cast<unsigned long>(-1) / multiplier)
		{
			return false;
		}
		count = static_cast<unsigned long>(primitive_count) * multiplier;
	}
	else
	{
		if (static_cast<unsigned long>(primitive_count) >
			static_cast<unsigned long>(-1) - addend)
		{
			return false;
		}
		count = static_cast<unsigned long>(primitive_count) + addend;
	}
	if (count > static_cast<unsigned long>(static_cast<unsigned int>(-1)))
	{
		return false;
	}
	*index_count = static_cast<unsigned int>(count);
	return true;
}

inline bool Is_D3D8_Indexed_Range_Valid(unsigned long index_capacity,
	unsigned int start_index, unsigned int index_count)
{
	const unsigned long start = static_cast<unsigned long>(start_index);
	const unsigned long count = static_cast<unsigned long>(index_count);
	return start <= index_capacity && count <= index_capacity - start;
}

// These states are deliberately not published into the neutral pipeline:
// fog constants and shader-derived fog/specular bits are mirrored by
// TrackLegacyFog/ShaderBits, while active water code publishes sampler wrap
// through texture-stage state.  Ambient is published below because W3DScene
// sets it directly through the legacy render-state API.
// Other unsupported render states remain fatal so a visible draw cannot reuse
// stale neutral state.
inline bool Is_D3D11_Irrelevant_Render_State(unsigned int state)
{
	switch (state)
	{
	case LEGACY_D3DRS_FOGCOLOR:
	case LEGACY_D3DRS_FOGENABLE:
	case LEGACY_D3DRS_FOGSTART:
	case LEGACY_D3DRS_FOGEND:
	case LEGACY_D3DRS_FOGDENSITY:
	case LEGACY_D3DRS_RANGEFOGENABLE:
	case LEGACY_D3DRS_FOGTABLEMODE:
	case LEGACY_D3DRS_FOGVERTEXMODE:
	case LEGACY_D3DRS_SPECULARENABLE:
	case LEGACY_D3DRS_WRAP0:
	case LEGACY_D3DRS_WRAP1:
	case LEGACY_D3DRS_WRAP2:
	case LEGACY_D3DRS_WRAP3:
	case LEGACY_D3DRS_WRAP4:
	case LEGACY_D3DRS_WRAP5:
	case LEGACY_D3DRS_WRAP6:
	case LEGACY_D3DRS_WRAP7:
		return true;
	default:
		return false;
	}
}

inline bool Should_Poison_D3D11_Render_State(unsigned int state,
	bool published)
{
	return !published && !Is_D3D11_Irrelevant_Render_State(state);
}

inline bool Is_Legacy_Render_Target_Binding_Equal(
	const void *current_color, const void *current_depth,
	bool current_uses_default_depth, const void *requested_color,
	const void *requested_depth, bool requested_uses_default_depth)
{
	return current_color == requested_color &&
		current_uses_default_depth == requested_uses_default_depth &&
		(current_uses_default_depth || current_depth == requested_depth);
}

} // namespace render
} // namespace rts

#endif
