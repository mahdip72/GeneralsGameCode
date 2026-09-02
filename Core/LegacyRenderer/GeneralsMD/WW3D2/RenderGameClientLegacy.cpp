/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** Win32/VC6 implementation of the renderer-neutral GameEngineDevice seam.
** This file is deliberately a legacy-only translation unit.  The x64 target
** selects RenderGameClientNative.cpp instead and never includes dx8wrapper.h.
*/

#include "Renderer/RenderGameClient.h"
#include "Lib/BaseType.h"

#if !defined(_WIN64)

#include "dx8wrapper.h"
#include "dx8renderer.h"
#include "rddesc.h"
#include "surfaceclass.h"
#include "texture.h"
#include "sortingrenderer.h"
#include "statistics.h"
#include "ww3d.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/Common/LegacyPixelShaderBytecode.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"
#include "WWMath/sphere.h"
#include "WWMath/vector3.h"

#include <string.h>

namespace
{

using namespace rts::render;

rts::render::RenderFrameFailureLatch g_game_render_failure;
unsigned long g_failure_frame = 0;
rts::render::GameDebugRenderStats g_debug_render_stats;

void ObserveFrame()
{
	const unsigned long frame = DX8Wrapper::Get_FrameCount();
	if (frame != g_failure_frame)
	{
		g_failure_frame = frame;
		g_game_render_failure.reset();
	}
}

rts::render::RenderResult FromHRESULT(HRESULT result)
{
	if (SUCCEEDED(result))
	{
		return rts::render::RENDER_RESULT_OK;
	}
	if (result == D3DERR_DEVICELOST || result == D3DERR_DEVICENOTRESET)
	{
		return rts::render::RENDER_RESULT_DEVICE_REMOVED;
	}
	if (result == E_INVALIDARG)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	return rts::render::RENDER_RESULT_FAILED;
}

void RecordFailure(rts::render::RenderResult result)
{
	if (result != rts::render::RENDER_RESULT_OK)
	{
		g_game_render_failure.record(result);
	}
}

bool CheckDevice()
{
	ObserveFrame();
	if (g_game_render_failure.hasFailure())
	{
		return false;
	}
	if (!DX8Wrapper::Is_Initted())
	{
		RecordFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return false;
	}
	if (DX8Wrapper::Is_Device_Lost())
	{
		RecordFailure(rts::render::RENDER_RESULT_DEVICE_REMOVED);
		return false;
	}
	return true;
}

LegacyPixelShaderBytecode::Program EmbeddedPixelProgram(
	const char *assetPath)
{
	if (assetPath == 0)
	{
		return LegacyPixelShaderBytecode::PROGRAM_COUNT;
	}
	if (strcmp(assetPath, "builtin/water/river") == 0)
	{
		return LegacyPixelShaderBytecode::WATER_RIVER;
	}
	if (strcmp(assetPath, "builtin/water/reflection") == 0)
	{
		return LegacyPixelShaderBytecode::WATER_REFLECTION;
	}
	if (strcmp(assetPath, "builtin/water/trapezoid") == 0)
	{
		return LegacyPixelShaderBytecode::WATER_TRAPEZOID;
	}
	return LegacyPixelShaderBytecode::PROGRAM_COUNT;
}

const DWORD kHistoricalWaveVertexDeclaration[] =
{
	D3DVSD_STREAM(0),
	D3DVSD_REG(0, D3DVSDT_FLOAT3),
	D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
	D3DVSD_REG(2, D3DVSDT_FLOAT2),
	D3DVSD_END()
};

const DWORD kHistoricalTreesVertexDeclaration[] =
{
	D3DVSD_STREAM(0),
	D3DVSD_REG(0, D3DVSDT_FLOAT3),
	D3DVSD_REG(1, D3DVSDT_FLOAT3),
	D3DVSD_REG(2, D3DVSDT_D3DCOLOR),
	D3DVSD_REG(7, D3DVSDT_FLOAT2),
	D3DVSD_END()
};

const DWORD *HistoricalVertexDeclaration(const char *assetPath,
	unsigned int *wordCount)
{
	if (assetPath == 0 || wordCount == 0)
		return 0;
	if (stricmp(assetPath, "shaders\\wave.vso") == 0)
	{
		*wordCount = sizeof(kHistoricalWaveVertexDeclaration) /
			sizeof(kHistoricalWaveVertexDeclaration[0]);
		return kHistoricalWaveVertexDeclaration;
	}
	if (stricmp(assetPath, "shaders\\Trees.vso") == 0)
	{
		*wordCount = sizeof(kHistoricalTreesVertexDeclaration) /
			sizeof(kHistoricalTreesVertexDeclaration[0]);
		return kHistoricalTreesVertexDeclaration;
	}
	return 0;
}

void CheckDeviceAfterVoidCall()
{
	if (!DX8Wrapper::Is_Initted())
	{
		RecordFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
	}
	else if (DX8Wrapper::Is_Device_Lost())
	{
		RecordFailure(rts::render::RENDER_RESULT_DEVICE_REMOVED);
	}
}

bool ToTransform(GameRenderTransformSlot slot,
	D3DTRANSFORMSTATETYPE *transform)
{
	if (transform == 0)
	{
		return false;
	}
	switch (slot)
	{
	case GAME_TRANSFORM_WORLD:
		*transform = D3DTS_WORLD;
		return true;
	case GAME_TRANSFORM_VIEW:
		*transform = D3DTS_VIEW;
		return true;
	case GAME_TRANSFORM_PROJECTION:
		*transform = D3DTS_PROJECTION;
		return true;
	case GAME_TRANSFORM_TEXTURE0:
	case GAME_TRANSFORM_TEXTURE1:
	case GAME_TRANSFORM_TEXTURE2:
	case GAME_TRANSFORM_TEXTURE3:
	case GAME_TRANSFORM_TEXTURE4:
	case GAME_TRANSFORM_TEXTURE5:
	case GAME_TRANSFORM_TEXTURE6:
	case GAME_TRANSFORM_TEXTURE7:
		*transform = static_cast<D3DTRANSFORMSTATETYPE>(
			D3DTS_TEXTURE0 + (slot - GAME_TRANSFORM_TEXTURE0));
		return true;
	default:
		return false;
	}
}

void CopyMatrixToLegacy(const Matrix4x4 &source, void *destination)
{
	float *values = static_cast<float *>(destination);
	for (unsigned int row = 0; row < 4; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
		{
			values[row * 4 + column] = source[row][column];
		}
	}
}

Matrix4x4 CopyMatrixFromLegacy(const void *source)
{
	const float *values = static_cast<const float *>(source);
	return Matrix4x4(
		values[0], values[1], values[2], values[3],
		values[4], values[5], values[6], values[7],
		values[8], values[9], values[10], values[11],
		values[12], values[13], values[14], values[15]);
}

bool ToD3DCompare(unsigned int value, unsigned int *translated)
{
	if (translated == 0 || value > RENDER_COMPARE_ALWAYS)
	{
		return false;
	}
	*translated = value + 1U;
	return true;
}

bool ToD3DBlend(unsigned int value, unsigned int *translated)
{
	if (translated == 0 || value > RENDER_BLEND_INVERSE_DESTINATION_COLOR)
	{
		return false;
	}
	*translated = value + 1U;
	return true;
}

bool ToD3DBlendOperation(unsigned int value, unsigned int *translated)
{
	if (translated == 0)
	{
		return false;
	}
	switch (value)
	{
	case RENDER_BLEND_ADD:
		*translated = D3DBLENDOP_ADD;
		break;
	case RENDER_BLEND_SUBTRACT:
		*translated = D3DBLENDOP_SUBTRACT;
		break;
	case RENDER_BLEND_REVERSE_SUBTRACT:
		*translated = D3DBLENDOP_REVSUBTRACT;
		break;
	case RENDER_BLEND_MINIMUM:
		*translated = D3DBLENDOP_MIN;
		break;
	case RENDER_BLEND_MAXIMUM:
		*translated = D3DBLENDOP_MAX;
		break;
	default:
		return false;
	}
	return true;
}

bool ToD3DStencilOperation(unsigned int value, unsigned int *translated)
{
	if (translated == 0 || value > RENDER_STENCIL_DECREMENT)
	{
		return false;
	}
	*translated = value + 1U;
	return true;
}

bool ToD3DCullMode(unsigned int value, unsigned int *translated)
{
	if (translated == 0)
	{
		return false;
	}
	switch (value)
	{
	case GAME_RENDER_CULL_NONE:
		*translated = D3DCULL_NONE;
		return true;
	case GAME_RENDER_CULL_CLOCKWISE:
		*translated = D3DCULL_CW;
		return true;
	case GAME_RENDER_CULL_COUNTER_CLOCKWISE:
		*translated = D3DCULL_CCW;
		return true;
	default:
		return false;
	}
}

bool ToD3DShadeMode(unsigned int value, unsigned int *translated)
{
	if (translated == 0)
	{
		return false;
	}
	switch (value)
	{
	case GAME_RENDER_SHADE_FLAT:
		*translated = D3DSHADE_FLAT;
		return true;
	case GAME_RENDER_SHADE_GOURAUD:
		*translated = D3DSHADE_GOURAUD;
		return true;
	default:
		return false;
	}
}

bool ToD3DRenderState(GameRenderState state, unsigned int value,
	D3DRENDERSTATETYPE *translatedState, unsigned int *translatedValue)
{
	if (translatedState == 0 || translatedValue == 0)
	{
		return false;
	}
	switch (state)
	{
	case GAME_RENDER_STATE_ALPHA_BLEND_ENABLE:
		*translatedState = D3DRS_ALPHABLENDENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_SOURCE_BLEND:
		*translatedState = D3DRS_SRCBLEND;
		return ToD3DBlend(value, translatedValue);
	case GAME_RENDER_STATE_DESTINATION_BLEND:
		*translatedState = D3DRS_DESTBLEND;
		return ToD3DBlend(value, translatedValue);
	case GAME_RENDER_STATE_BLEND_OPERATION:
		*translatedState = D3DRS_BLENDOP;
		return ToD3DBlendOperation(value, translatedValue);
	case GAME_RENDER_STATE_COLOR_WRITE_MASK:
		if ((value & ~(GAME_COLOR_WRITE_RED | GAME_COLOR_WRITE_GREEN |
			GAME_COLOR_WRITE_BLUE | GAME_COLOR_WRITE_ALPHA)) != 0U)
		{
			return false;
		}
		*translatedState = D3DRS_COLORWRITEENABLE;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_DEPTH_ENABLE:
		*translatedState = D3DRS_ZENABLE;
		*translatedValue = value ? D3DZB_TRUE : D3DZB_FALSE;
		return true;
	case GAME_RENDER_STATE_DEPTH_WRITE:
		*translatedState = D3DRS_ZWRITEENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_DEPTH_FUNCTION:
		*translatedState = D3DRS_ZFUNC;
		return ToD3DCompare(value, translatedValue);
	case GAME_RENDER_STATE_ALPHA_TEST_ENABLE:
		*translatedState = D3DRS_ALPHATESTENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_ALPHA_FUNCTION:
		*translatedState = D3DRS_ALPHAFUNC;
		return ToD3DCompare(value, translatedValue);
	case GAME_RENDER_STATE_ALPHA_REFERENCE:
		*translatedState = D3DRS_ALPHAREF;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_TEXTURE_FACTOR:
		*translatedState = D3DRS_TEXTUREFACTOR;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_LIGHTING:
		*translatedState = D3DRS_LIGHTING;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_Z_BIAS:
		*translatedState = D3DRS_ZBIAS;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_POINT_SPRITE_ENABLE:
		*translatedState = D3DRS_POINTSPRITEENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_POINT_SCALE_ENABLE:
		*translatedState = D3DRS_POINTSCALEENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_POINT_SIZE:
		*translatedState = D3DRS_POINTSIZE;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_POINT_SIZE_MIN:
		*translatedState = D3DRS_POINTSIZE_MIN;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_POINT_SIZE_MAX:
		*translatedState = D3DRS_POINTSIZE_MAX;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_POINT_SCALE_A:
		*translatedState = D3DRS_POINTSCALE_A;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_POINT_SCALE_B:
		*translatedState = D3DRS_POINTSCALE_B;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_POINT_SCALE_C:
		*translatedState = D3DRS_POINTSCALE_C;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_STENCIL_ENABLE:
		*translatedState = D3DRS_STENCILENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_STENCIL_FUNCTION:
		*translatedState = D3DRS_STENCILFUNC;
		return ToD3DCompare(value, translatedValue);
	case GAME_RENDER_STATE_STENCIL_REFERENCE:
		*translatedState = D3DRS_STENCILREF;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_STENCIL_READ_MASK:
		*translatedState = D3DRS_STENCILMASK;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_STENCIL_WRITE_MASK:
		*translatedState = D3DRS_STENCILWRITEMASK;
		*translatedValue = value;
		return true;
	case GAME_RENDER_STATE_STENCIL_FAIL_OPERATION:
		*translatedState = D3DRS_STENCILFAIL;
		return ToD3DStencilOperation(value, translatedValue);
	case GAME_RENDER_STATE_STENCIL_DEPTH_FAIL_OPERATION:
		*translatedState = D3DRS_STENCILZFAIL;
		return ToD3DStencilOperation(value, translatedValue);
	case GAME_RENDER_STATE_STENCIL_PASS_OPERATION:
		*translatedState = D3DRS_STENCILPASS;
		return ToD3DStencilOperation(value, translatedValue);
	case GAME_RENDER_STATE_CULL_MODE:
		*translatedState = D3DRS_CULLMODE;
		return ToD3DCullMode(value, translatedValue);
	case GAME_RENDER_STATE_SHADE_MODE:
		*translatedState = D3DRS_SHADEMODE;
		return ToD3DShadeMode(value, translatedValue);
	case GAME_RENDER_STATE_FOG_ENABLE:
		*translatedState = D3DRS_FOGENABLE;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_NORMALIZE_NORMALS:
		*translatedState = D3DRS_NORMALIZENORMALS;
		*translatedValue = value ? TRUE : FALSE;
		return true;
	case GAME_RENDER_STATE_FILL_MODE:
		*translatedState = D3DRS_FILLMODE;
		switch (value)
		{
		case GAME_RENDER_FILL_POINT:
			*translatedValue = D3DFILL_POINT;
			return true;
		case GAME_RENDER_FILL_WIREFRAME:
			*translatedValue = D3DFILL_WIREFRAME;
			return true;
		case GAME_RENDER_FILL_SOLID:
			*translatedValue = D3DFILL_SOLID;
			return true;
		default:
			return false;
		}
	default:
		return false;
	}
}

bool ToD3DTextureOperation(unsigned int value, unsigned int *translated)
{
	if (translated == 0)
	{
		return false;
	}
	switch (value)
	{
	case RENDER_TEXTURE_OP_DISABLE: *translated = D3DTOP_DISABLE; break;
	case RENDER_TEXTURE_OP_SELECT_ARGUMENT_1: *translated = D3DTOP_SELECTARG1; break;
	case RENDER_TEXTURE_OP_SELECT_ARGUMENT_2: *translated = D3DTOP_SELECTARG2; break;
	case RENDER_TEXTURE_OP_MODULATE: *translated = D3DTOP_MODULATE; break;
	case RENDER_TEXTURE_OP_MODULATE_2X: *translated = D3DTOP_MODULATE2X; break;
	case RENDER_TEXTURE_OP_MODULATE_4X: *translated = D3DTOP_MODULATE4X; break;
	case RENDER_TEXTURE_OP_ADD: *translated = D3DTOP_ADD; break;
	case RENDER_TEXTURE_OP_ADD_SIGNED: *translated = D3DTOP_ADDSIGNED; break;
	case RENDER_TEXTURE_OP_ADD_SIGNED_2X: *translated = D3DTOP_ADDSIGNED2X; break;
	case RENDER_TEXTURE_OP_SUBTRACT: *translated = D3DTOP_SUBTRACT; break;
	case RENDER_TEXTURE_OP_ADD_SMOOTH: *translated = D3DTOP_ADDSMOOTH; break;
	case RENDER_TEXTURE_OP_BLEND_DIFFUSE_ALPHA: *translated = D3DTOP_BLENDDIFFUSEALPHA; break;
	case RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA: *translated = D3DTOP_BLENDTEXTUREALPHA; break;
	case RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA: *translated = D3DTOP_BLENDCURRENTALPHA; break;
	case RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR: *translated = D3DTOP_MODULATEALPHA_ADDCOLOR; break;
	case RENDER_TEXTURE_OP_DOT_PRODUCT_3: *translated = D3DTOP_DOTPRODUCT3; break;
	case RENDER_TEXTURE_OP_BUMP_ENVIRONMENT: *translated = D3DTOP_BUMPENVMAP; break;
	case RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE: *translated = D3DTOP_BUMPENVMAPLUMINANCE; break;
	case RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA_PREMULTIPLIED: *translated = D3DTOP_BLENDTEXTUREALPHAPM; break;
	case RENDER_TEXTURE_OP_BLEND_TEXTURE_FACTOR_ALPHA: *translated = D3DTOP_BLENDFACTORALPHA; break;
	case RENDER_TEXTURE_OP_PREMODULATE: *translated = D3DTOP_PREMODULATE; break;
	case RENDER_TEXTURE_OP_MODULATE_COLOR_ADD_ALPHA: *translated = D3DTOP_MODULATECOLOR_ADDALPHA; break;
	case RENDER_TEXTURE_OP_MODULATE_INVERSE_ALPHA_ADD_COLOR: *translated = D3DTOP_MODULATEINVALPHA_ADDCOLOR; break;
	case RENDER_TEXTURE_OP_MODULATE_INVERSE_COLOR_ADD_ALPHA: *translated = D3DTOP_MODULATEINVCOLOR_ADDALPHA; break;
	case RENDER_TEXTURE_OP_MULTIPLY_ADD: *translated = D3DTOP_MULTIPLYADD; break;
	case RENDER_TEXTURE_OP_LINEAR_INTERPOLATE: *translated = D3DTOP_LERP; break;
	default:
		return false;
	}
	return true;
}

bool ToD3DTextureArgument(unsigned int value, unsigned int *translated)
{
	if (translated == 0 ||
		(value & ~(0xffU | GAME_TEXTURE_ARGUMENT_COMPLEMENT |
		GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE)) != 0U)
	{
		return false;
	}
	const unsigned int argument = value & 0xffU;
	switch (argument)
	{
	case GAME_TEXTURE_ARGUMENT_CURRENT: *translated = D3DTA_CURRENT; break;
	case GAME_TEXTURE_ARGUMENT_DIFFUSE: *translated = D3DTA_DIFFUSE; break;
	case GAME_TEXTURE_ARGUMENT_TEXTURE: *translated = D3DTA_TEXTURE; break;
	case GAME_TEXTURE_ARGUMENT_FACTOR: *translated = D3DTA_TFACTOR; break;
	case GAME_TEXTURE_ARGUMENT_SPECULAR: *translated = D3DTA_SPECULAR; break;
	case GAME_TEXTURE_ARGUMENT_TEMPORARY: *translated = D3DTA_TEMP; break;
	default:
		return false;
	}
	if ((value & GAME_TEXTURE_ARGUMENT_COMPLEMENT) != 0U)
	{
		*translated |= D3DTA_COMPLEMENT;
	}
	if ((value & GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE) != 0U)
	{
		*translated |= D3DTA_ALPHAREPLICATE;
	}
	return true;
}

bool ToD3DTextureAddress(unsigned int value, unsigned int *translated)
{
	if (translated == 0)
	{
		return false;
	}
	switch (value)
	{
	case RENDER_TEXTURE_ADDRESS_WRAP: *translated = D3DTADDRESS_WRAP; break;
	case RENDER_TEXTURE_ADDRESS_MIRROR: *translated = D3DTADDRESS_MIRROR; break;
	case RENDER_TEXTURE_ADDRESS_CLAMP: *translated = D3DTADDRESS_CLAMP; break;
	case RENDER_TEXTURE_ADDRESS_BORDER: *translated = D3DTADDRESS_BORDER; break;
	default: return false;
	}
	return true;
}

bool ToD3DTextureFilter(unsigned int value, unsigned int *translated)
{
	if (translated == 0)
	{
		return false;
	}
	switch (value)
	{
	case RENDER_TEXTURE_FILTER_NONE: *translated = D3DTEXF_NONE; break;
	case RENDER_TEXTURE_FILTER_POINT: *translated = D3DTEXF_POINT; break;
	case RENDER_TEXTURE_FILTER_LINEAR: *translated = D3DTEXF_LINEAR; break;
	case RENDER_TEXTURE_FILTER_ANISOTROPIC: *translated = D3DTEXF_ANISOTROPIC; break;
	default: return false;
	}
	return true;
}

bool ToD3DTextureCoordinateIndex(unsigned int value, unsigned int *translated)
{
	if (translated == 0 || (value & 0xffffU) > 7U)
	{
		return false;
	}
	const unsigned int index = value & 0xffffU;
	switch (value & 0xffff0000U)
	{
	case GAME_TEXTURE_COORDINATE_PASSTHROUGH:
		*translated = index | D3DTSS_TCI_PASSTHRU;
		break;
	case GAME_TEXTURE_COORDINATE_CAMERA_NORMAL:
		*translated = index | D3DTSS_TCI_CAMERASPACENORMAL;
		break;
	case GAME_TEXTURE_COORDINATE_CAMERA_POSITION:
		*translated = index | D3DTSS_TCI_CAMERASPACEPOSITION;
		break;
	case GAME_TEXTURE_COORDINATE_CAMERA_REFLECTION:
		*translated = index | D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR;
		break;
	default:
		return false;
	}
	return true;
}

bool ToD3DTextureTransformFlags(unsigned int value, unsigned int *translated)
{
	if (translated == 0 || (value & ~(0xffU | GAME_TEXTURE_TRANSFORM_PROJECTED)) != 0U)
	{
		return false;
	}
	const unsigned int count = value & 0xffU;
	if (count > GAME_TEXTURE_TRANSFORM_COUNT4)
	{
		return false;
	}
	*translated = count;
	if ((value & GAME_TEXTURE_TRANSFORM_PROJECTED) != 0U)
	{
		*translated |= D3DTTFF_PROJECTED;
	}
	return true;
}

bool ToD3DTextureStageState(unsigned int state, unsigned int value,
	D3DTEXTURESTAGESTATETYPE *translatedState, unsigned int *translatedValue)
{
	if (translatedState == 0 || translatedValue == 0)
	{
		return false;
	}
	switch (state)
	{
	case GAME_TEXTURE_STAGE_COLOR_ARGUMENT0:
		*translatedState = D3DTSS_COLORARG0;
		return ToD3DTextureArgument(value, translatedValue);
	case GAME_TEXTURE_STAGE_COLOR_ARGUMENT1:
		*translatedState = D3DTSS_COLORARG1;
		return ToD3DTextureArgument(value, translatedValue);
	case GAME_TEXTURE_STAGE_COLOR_ARGUMENT2:
		*translatedState = D3DTSS_COLORARG2;
		return ToD3DTextureArgument(value, translatedValue);
	case GAME_TEXTURE_STAGE_COLOR_OPERATION:
		*translatedState = D3DTSS_COLOROP;
		return ToD3DTextureOperation(value, translatedValue);
	case GAME_TEXTURE_STAGE_ALPHA_ARGUMENT0:
		*translatedState = D3DTSS_ALPHAARG0;
		return ToD3DTextureArgument(value, translatedValue);
	case GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1:
		*translatedState = D3DTSS_ALPHAARG1;
		return ToD3DTextureArgument(value, translatedValue);
	case GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2:
		*translatedState = D3DTSS_ALPHAARG2;
		return ToD3DTextureArgument(value, translatedValue);
	case GAME_TEXTURE_STAGE_ALPHA_OPERATION:
		*translatedState = D3DTSS_ALPHAOP;
		return ToD3DTextureOperation(value, translatedValue);
	case GAME_TEXTURE_STAGE_ADDRESS_U:
		*translatedState = D3DTSS_ADDRESSU;
		return ToD3DTextureAddress(value, translatedValue);
	case GAME_TEXTURE_STAGE_ADDRESS_V:
		*translatedState = D3DTSS_ADDRESSV;
		return ToD3DTextureAddress(value, translatedValue);
	case GAME_TEXTURE_STAGE_ADDRESS_W:
		*translatedState = D3DTSS_ADDRESSW;
		return ToD3DTextureAddress(value, translatedValue);
	case GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER:
		*translatedState = D3DTSS_MAGFILTER;
		return ToD3DTextureFilter(value, translatedValue);
	case GAME_TEXTURE_STAGE_MINIFICATION_FILTER:
		*translatedState = D3DTSS_MINFILTER;
		return ToD3DTextureFilter(value, translatedValue);
	case GAME_TEXTURE_STAGE_MIP_FILTER:
		*translatedState = D3DTSS_MIPFILTER;
		return ToD3DTextureFilter(value, translatedValue);
	case GAME_TEXTURE_STAGE_COORDINATE_INDEX:
		*translatedState = D3DTSS_TEXCOORDINDEX;
		return ToD3DTextureCoordinateIndex(value, translatedValue);
	case GAME_TEXTURE_STAGE_TRANSFORM_FLAGS:
		*translatedState = D3DTSS_TEXTURETRANSFORMFLAGS;
		return ToD3DTextureTransformFlags(value, translatedValue);
	default:
		return false;
	}
}

unsigned int FloatBits(float value)
{
	unsigned int bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

bool ToRenderResultFormat(D3DFORMAT format,
	rts::render::RenderFormat *renderFormat)
{
	if (renderFormat == 0)
	{
		return false;
	}
	switch (format)
	{
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:
		*renderFormat = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		return true;
	default:
		*renderFormat = rts::render::RENDER_FORMAT_UNKNOWN;
		return false;
	}
}

// DX8Wrapper keeps the historical device-selection and display-policy entry
// points protected because WW3D used to be their only caller.  The neutral
// renderer seam is now the caller, so expose only the exact operations needed
// by this external x86 adapter through a private derived access class.  This
// avoids routing back through WW3D and keeps the shared product code free of
// the legacy backend types.
class LegacyDX8RendererAccess : public DX8Wrapper
{
public:
	static bool SetRenderDeviceByName(const char *name, int width, int height,
		int bitDepth, int windowed, bool resizeWindow)
	{
		return Set_Render_Device(name, width, height, bitDepth, windowed,
			resizeWindow);
	}

	static bool SetRenderDeviceByIndex(int device, int width, int height,
		int bitDepth, int windowed, bool resizeWindow, bool resetDevice,
		bool restoreAssets)
	{
		return Set_Render_Device(device, width, height, bitDepth, windowed,
			resizeWindow, resetDevice, restoreAssets);
	}

	static bool SetAnyRenderDevice()
	{
		return Set_Any_Render_Device();
	}

	static bool SetNextRenderDevice()
	{
		return Set_Next_Render_Device();
	}

	static int GetRenderDeviceIndex()
	{
		return Get_Render_Device();
	}

	static int GetRenderDeviceCount()
	{
		return Get_Render_Device_Count();
	}

	static const char *GetRenderDeviceName(int device)
	{
		return Get_Render_Device_Name(device);
	}

	static const RenderDeviceDescClass &GetRenderDeviceDesc(int device)
	{
		return Get_Render_Device_Desc(device);
	}

	static bool SetRendererResolution(int width, int height, int bitDepth,
		int windowed, bool resizeWindow)
	{
		return Set_Device_Resolution(width, height, bitDepth, windowed,
			resizeWindow);
	}

	static void GetRendererResolution(int &width, int &height, int &bitDepth,
		bool &windowed)
	{
		Get_Device_Resolution(width, height, bitDepth, windowed);
	}

	static void GetRendererTargetResolution(int &width, int &height,
		int &bitDepth, bool &windowed)
	{
		Get_Render_Target_Resolution(width, height, bitDepth, windowed);
	}

	static void SetTextureBitdepth(int bitDepth)
	{
		Set_Texture_Bitdepth(bitDepth);
	}

	static int GetTextureBitdepth()
	{
		return Get_Texture_Bitdepth();
	}

	static void SetMSAAMode(D3DMULTISAMPLE_TYPE mode)
	{
		Set_MSAA_Mode(mode);
	}

	static D3DMULTISAMPLE_TYPE GetMSAAMode()
	{
		return Get_MSAA_Mode();
	}

	static void SetSwapInterval(int interval)
	{
		Set_Swap_Interval(interval);
	}

	static int GetSwapInterval()
	{
		return Get_Swap_Interval();
	}
};

void CopyLegacyDeviceString(char *destination, const char *source)
{
	if (destination == 0)
		return;
	if (source == 0)
		source = "";
	strncpy(destination, source,
		rts::render::GAME_RENDER_DEVICE_STRING_CAPACITY - 1);
	destination[rts::render::GAME_RENDER_DEVICE_STRING_CAPACITY - 1] = '\0';
}

void CopyLegacyDeviceDescription(const RenderDeviceDescClass &source,
	rts::render::GameRenderDeviceDesc *destination, int adapterIndex)
{
	memset(destination, 0, sizeof(*destination));
	CopyLegacyDeviceString(destination->deviceName,
		source.Get_Device_Name());
	CopyLegacyDeviceString(destination->deviceVendor,
		source.Get_Device_Vendor());
	CopyLegacyDeviceString(destination->devicePlatform,
		source.Get_Device_Platform());
	CopyLegacyDeviceString(destination->driverName,
		source.Get_Driver_Name());
	CopyLegacyDeviceString(destination->driverVendor,
		source.Get_Driver_Vendor());
	CopyLegacyDeviceString(destination->driverVersion,
		source.Get_Driver_Version());
	CopyLegacyDeviceString(destination->hardwareName,
		source.Get_Hardware_Name());
	CopyLegacyDeviceString(destination->hardwareVendor,
		source.Get_Hardware_Vendor());
	CopyLegacyDeviceString(destination->hardwareChipset,
		source.Get_Hardware_Chipset());
	destination->adapterIndex = adapterIndex < 0 ? 0U :
		static_cast<unsigned int>(adapterIndex);
}

bool LegacyRendererIsReady()
{
	ObserveFrame();
	if (!DX8Wrapper::Is_Initted())
	{
		RecordFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return false;
	}
	return true;
}

rts::render::RenderResult CompleteLegacyBooleanOperation(bool success)
{
	if (!success)
	{
		RecordFailure(DX8Wrapper::Is_Device_Lost() ?
			rts::render::RENDER_RESULT_DEVICE_REMOVED :
			rts::render::RENDER_RESULT_FAILED);
	}
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() :
		(success ? rts::render::RENDER_RESULT_OK :
		rts::render::RENDER_RESULT_FAILED);
}

rts::render::RenderResult LegacyFailureResult(
	rts::render::RenderResult fallback)
{
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() : fallback;
}

bool ToggleLegacyWindowedMode()
{
	if (!DX8Wrapper::Is_Initted())
		return false;
	const int deviceCount = LegacyDX8RendererAccess::GetRenderDeviceCount();
	if (deviceCount <= 0)
		return false;

	const RenderDeviceDescClass &device =
		LegacyDX8RendererAccess::GetRenderDeviceDesc(-1);
	const DynamicVectorClass<ResolutionDescClass> &resolutions =
		device.Enumerate_Resolutions();
	int width = 0;
	int height = 0;
	int bitDepth = 0;
	bool windowed = false;
	LegacyDX8RendererAccess::GetRendererResolution(width, height, bitDepth,
		windowed);

	for (int i = 0; i < resolutions.Count(); ++i)
	{
		if (resolutions[i].Width == width &&
			resolutions[i].Height == height &&
			resolutions[i].BitDepth == bitDepth)
		{
			return LegacyDX8RendererAccess::SetRendererResolution(
				-1, -1, -1, windowed ? 0 : 1, true);
		}
	}
	if (resolutions.Count() == 0)
		return false;
	return LegacyDX8RendererAccess::SetRendererResolution(
		resolutions[0].Width, resolutions[0].Height, resolutions[0].BitDepth,
		windowed ? 0 : 1, true);
}

}

namespace rts
{
namespace render
{

bool IsNativeGameRendererActive()
{
	return false;
}

bool IsGameRendererInitialized()
{
	return DX8Wrapper::Is_Initted();
}

bool IsGameRenderTargetOperational()
{
	return DX8Wrapper::Is_Initted() && !DX8Wrapper::Is_Device_Lost();
}

bool IsGameRenderingToTexture()
{
	return DX8Wrapper::Is_Initted() && DX8Wrapper::Is_Render_To_Texture();
}

bool IsGameDebugConsoleDisabled()
{
#ifdef EXTENDED_STATS
	return DX8Wrapper::stats.m_disableConsole ||
		GetGameDebugRenderStats().disableConsole;
#else
	return false;
#endif
}

bool GameRendererSupportsPointSprites()
{
	return DX8Wrapper::Is_Initted() &&
		DX8Wrapper::Get_Current_Caps()->Support_PointSprites();
}

bool GameRendererSupportsDot3()
{
	return DX8Wrapper::Is_Initted() &&
		DX8Wrapper::Get_Current_Caps()->Support_Dot3();
}

bool GameRendererSupportsZBias()
{
	return DX8Wrapper::Is_Initted() &&
		DX8Wrapper::Get_Current_Caps()->Support_ZBias();
}

bool GameRendererSupportsNPatches()
{
	return DX8Wrapper::Is_Initted() &&
		DX8Wrapper::Get_Current_Caps()->Support_NPatches();
}

bool GameRendererSupportsStencil()
{
	return DX8Wrapper::Is_Initted() && DX8Wrapper::Has_Stencil();
}

bool IsGameTextureFormatSupported(WW3DFormat format)
{
	if (!DX8Wrapper::Is_Initted() ||
		DX8Wrapper::Get_Current_Caps() == 0 ||
		format <= WW3D_FORMAT_UNKNOWN || format >= WW3D_FORMAT_COUNT)
	{
		return false;
	}
	// This is the same CheckDeviceFormat-backed table used by the legacy
	// texture constructors.  It intentionally does not use the separate
	// render-target table: W3DRadar asks whether a sampled texture format is
	// supported by the active device.
	return DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(format);
}

const GameDebugRenderStats &GetGameDebugRenderStats()
{
	return g_debug_render_stats;
}

GameDebugRenderStats &GetMutableGameDebugRenderStats()
{
	return g_debug_render_stats;
}

void SetGameDebugRenderStats(const GameDebugRenderStats &stats)
{
	GetMutableGameDebugRenderStats() = stats;
}

void BeginGameDisplayIteration()
{
	// The Win32 compatibility lane keeps GPU-copy lease ownership in the DX8
	// wrapper/bridge.  Its display iteration hook remains the authoritative
	// epoch boundary; the neutral call is intentionally a no-op here.
}

RenderResult ResetGameRenderFrameResources(bool frameChanged)
{
	if (!CheckDevice())
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	DynamicVBAccessClass::_Reset(frameChanged);
	DynamicIBAccessClass::_Reset(frameChanged);
	CheckDeviceAfterVoidCall();
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() : RENDER_RESULT_OK;
}

RenderResult GetGameTextureFilterCapabilities(
	GameTextureFilterCapabilities *capabilities)
{
	if (capabilities == 0)
		return RENDER_RESULT_INVALID_ARGUMENT;
	*capabilities = GameTextureFilterCapabilities();
	if (!DX8Wrapper::Is_Initted())
		return RENDER_RESULT_INVALID_ARGUMENT;
	const DX8Caps *caps = DX8Wrapper::Get_Current_Caps();
	if (caps == 0)
		return RENDER_RESULT_FAILED;
	capabilities->supportsPoint = true;
	capabilities->supportsLinear = true;
	capabilities->supportsAnisotropic =
		caps->Support_Anisotropic_Filtering();
	// DX8Caps exposes the support bit but not MaxAnisotropy.  Keep the
	// advertised neutral limit conservative until that legacy capability is
	// widened; callers must not infer full anisotropic support from the flag.
	capabilities->maxAnisotropy = 1;
	return RENDER_RESULT_OK;
}

unsigned int GetGameMaxTexturesPerPass()
{
	if (!DX8Wrapper::Is_Initted() || DX8Wrapper::Get_Current_Caps() == 0)
		return 0;
	const int reported = DX8Wrapper::Get_Current_Caps()->
		Get_Max_Simultaneous_Textures();
	return reported <= 0 ? 0 : static_cast<unsigned int>(reported) >
		LEGACY_TEXTURE_STAGE_COUNT ? LEGACY_TEXTURE_STAGE_COUNT :
		static_cast<unsigned int>(reported);
}

bool IsGameTerrainRenderingDisabled()
{
	return false;
}

bool IsGameObjectRenderingDisabled()
{
	return false;
}

WW3DFormat GetGameBackBufferFormat()
{
	return DX8Wrapper::Is_Initted() ? DX8Wrapper::getBackBufferFormat() :
		WW3D_FORMAT_UNKNOWN;
}

RenderResult InitializeGameRenderer(void *window, unsigned int width,
	unsigned int height, bool lite, bool enableVsync)
{
	if (window == 0 || width == 0 || height == 0 ||
		DX8Wrapper::Is_Initted())
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	(void)enableVsync;
	g_game_render_failure.reset();
	const bool initialized = DX8Wrapper::Init(window, lite);
	return CompleteLegacyBooleanOperation(initialized);
}

RenderResult ShutdownGameRenderer()
{
	ObserveFrame();
	DX8Wrapper::Shutdown();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult SetGameRenderDeviceByName(const char *name, int width,
	int height, int bitDepth, int windowed, bool resizeWindow)
{
	if (name == 0 || name[0] == '\0' ||
		(windowed < -1 || windowed > 1) || !LegacyRendererIsReady())
	{
		if (name == 0 || name[0] == '\0' || windowed < -1 || windowed > 1)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	unsigned int nameLength = 0;
	while (nameLength < 256U && name[nameLength] != '\0')
		++nameLength;
	if (nameLength == 256U)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	return CompleteLegacyBooleanOperation(
		LegacyDX8RendererAccess::SetRenderDeviceByName(name,
			width > 0 ? width : -1, height > 0 ? height : -1,
			bitDepth > 0 ? bitDepth : -1, windowed, resizeWindow));
}

RenderResult SetGameRenderDeviceByIndex(int device, int width, int height,
	int bitDepth, int windowed, bool resizeWindow, bool resetDevice,
	bool restoreAssets)
{
	if (device < 0 || windowed < -1 || windowed > 1 ||
		!LegacyRendererIsReady())
	{
		if (device < 0 || windowed < -1 || windowed > 1)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	if (device >= LegacyDX8RendererAccess::GetRenderDeviceCount())
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	return CompleteLegacyBooleanOperation(
		LegacyDX8RendererAccess::SetRenderDeviceByIndex(device,
			width > 0 ? width : -1, height > 0 ? height : -1,
			bitDepth > 0 ? bitDepth : -1, windowed, resizeWindow,
			resetDevice, restoreAssets));
}

RenderResult SetAnyGameRenderDevice()
{
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	return CompleteLegacyBooleanOperation(
		LegacyDX8RendererAccess::SetAnyRenderDevice());
}

RenderResult SetNextGameRenderDevice()
{
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	if (LegacyDX8RendererAccess::GetRenderDeviceCount() <= 0)
		return CompleteLegacyBooleanOperation(false);
	return CompleteLegacyBooleanOperation(
		LegacyDX8RendererAccess::SetNextRenderDevice());
}

RenderResult ToggleGameRendererWindowed()
{
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	return CompleteLegacyBooleanOperation(ToggleLegacyWindowedMode());
}

int GetGameRenderDeviceIndex()
{
	ObserveFrame();
	if (!DX8Wrapper::Is_Initted() ||
		LegacyDX8RendererAccess::GetRenderDeviceCount() <= 0)
		return -1;
	return LegacyDX8RendererAccess::GetRenderDeviceIndex();
}

RenderResult GetGameRenderDeviceName(int device, char *name,
	unsigned int nameCapacity)
{
	if (name == 0 || nameCapacity == 0U || device < 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	name[0] = '\0';
	if (!LegacyRendererIsReady() ||
		device >= LegacyDX8RendererAccess::GetRenderDeviceCount())
	{
		if (device >= 0 &&
			LegacyDX8RendererAccess::GetRenderDeviceCount() <= device)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	CopyLegacyDeviceString(name,
		LegacyDX8RendererAccess::GetRenderDeviceName(device));
	return LegacyFailureResult(RENDER_RESULT_OK);
}

int GetGameRenderDeviceCount()
{
	ObserveFrame();
	return DX8Wrapper::Is_Initted() ?
		LegacyDX8RendererAccess::GetRenderDeviceCount() : 0;
}

RenderResult GetGameRenderDeviceDesc(int device, GameRenderDeviceDesc *desc,
	GameRenderResolutionDesc *resolutions, unsigned int resolutionCapacity,
	unsigned int *resolutionCount)
{
	if (resolutionCount != 0)
		*resolutionCount = 0;
	if (desc == 0 || resolutionCount == 0 ||
		(resolutionCapacity != 0U && resolutions == 0) || device < 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	const int deviceCount = LegacyDX8RendererAccess::GetRenderDeviceCount();
	if (device >= deviceCount)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	const RenderDeviceDescClass &source =
		LegacyDX8RendererAccess::GetRenderDeviceDesc(device);
	CopyLegacyDeviceDescription(source, desc, device);
	const DynamicVectorClass<ResolutionDescClass> &available =
		source.Enumerate_Resolutions();
	const unsigned int availableCount =
		static_cast<unsigned int>(available.Count());
	if (resolutionCapacity == 0U)
	{
		*resolutionCount = availableCount;
		return LegacyFailureResult(RENDER_RESULT_OK);
	}
	const unsigned int copiedCount = availableCount < resolutionCapacity ?
		availableCount : resolutionCapacity;
	for (unsigned int i = 0; i < copiedCount; ++i)
	{
		resolutions[i].width = available[i].Width;
		resolutions[i].height = available[i].Height;
		resolutions[i].bitDepth = available[i].BitDepth;
		resolutions[i].refreshRate = available[i].RefreshRate;
	}
	*resolutionCount = copiedCount;
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult GetGameRendererResolution(int *width, int *height,
	int *bitDepth, bool *windowed)
{
	if (width == 0 || height == 0 || bitDepth == 0 || windowed == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	LegacyDX8RendererAccess::GetRendererResolution(*width, *height,
		*bitDepth, *windowed);
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult GetGameRendererTargetResolution(int *width, int *height,
	int *bitDepth, bool *windowed)
{
	if (width == 0 || height == 0 || bitDepth == 0 || windowed == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	LegacyDX8RendererAccess::GetRendererTargetResolution(*width, *height,
		*bitDepth, *windowed);
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult SetGameRendererResolution(int width, int height, int bitDepth,
	int windowed, bool resizeWindow)
{
	if (windowed < -1 || windowed > 1)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	if (!LegacyRendererIsReady())
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	return CompleteLegacyBooleanOperation(
		LegacyDX8RendererAccess::SetRendererResolution(
			width > 0 ? width : -1, height > 0 ? height : -1,
			bitDepth > 0 ? bitDepth : -1, windowed, resizeWindow));
}

RenderResult BeginGameRender(bool clear, bool clearz,
	const GameRenderColor &color, float destinationAlpha)
{
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		if (DX8Wrapper::_Get_D3D_Device8() == 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	if (clear || clearz)
	{
		int width = 0;
		int height = 0;
		int bitDepth = 0;
		bool windowed = false;
		LegacyDX8RendererAccess::GetRendererTargetResolution(width, height,
			bitDepth, windowed);
		if (width <= 0 || height <= 0)
		{
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
		}
		D3DVIEWPORT8 viewport;
		viewport.X = 0;
		viewport.Y = 0;
		viewport.Width = width;
		viewport.Height = height;
		viewport.MinZ = 0.0f;
		viewport.MaxZ = 1.0f;
		DX8Wrapper::Set_Viewport(&viewport);
		DX8Wrapper::Clear(clear, clearz,
			Vector3(color.red, color.green, color.blue), destinationAlpha);
		CheckDeviceAfterVoidCall();
		if (g_game_render_failure.hasFailure())
			return g_game_render_failure.result();
	}
	if (!DX8Wrapper::Begin_Scene())
	{
		RecordFailure(DX8Wrapper::Is_Device_Lost() ?
			RENDER_RESULT_DEVICE_REMOVED : RENDER_RESULT_FAILED);
		return LegacyFailureResult(RENDER_RESULT_FAILED);
	}
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult ClearGameRenderTargets(bool clear, bool clearz,
	const GameRenderColor &color, float destinationAlpha)
{
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		if (DX8Wrapper::_Get_D3D_Device8() == 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	DX8Wrapper::Clear(clear, clearz,
		Vector3(color.red, color.green, color.blue), destinationAlpha);
	CheckDeviceAfterVoidCall();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

void SetGameAmbientColor(const GameRenderColor &color)
{
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
		return;
	DX8Wrapper::Set_Ambient(Vector3(color.red, color.green, color.blue));
	CheckDeviceAfterVoidCall();
}

RenderResult EndGameRender(bool present)
{
	ObserveFrame();
	if (!DX8Wrapper::Is_Initted() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	DX8Wrapper::End_Scene(present);
	CheckDeviceAfterVoidCall();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult FlipGameRenderer()
{
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		if (DX8Wrapper::_Get_D3D_Device8() == 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	DX8Wrapper::Flip_To_Primary();
	CheckDeviceAfterVoidCall();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult SyncGameRenderer(bool step)
{
	(void)step;
	ObserveFrame();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

RenderResult SetGameRendererSwapInterval(long interval)
{
	if (!LegacyRendererIsReady() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		if (DX8Wrapper::_Get_D3D_Device8() == 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	LegacyDX8RendererAccess::SetSwapInterval(static_cast<int>(interval));
	CheckDeviceAfterVoidCall();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

long GetGameRendererSwapInterval()
{
	ObserveFrame();
	return DX8Wrapper::Is_Initted() ?
		static_cast<long>(LegacyDX8RendererAccess::GetSwapInterval()) : 0L;
}

RenderResult SetGameTextureBitdepth(int bitDepth)
{
	if (bitDepth != 16 && bitDepth != 32)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	ObserveFrame();
	LegacyDX8RendererAccess::SetTextureBitdepth(bitDepth);
	return LegacyFailureResult(RENDER_RESULT_OK);
}

int GetGameTextureBitdepth()
{
	return LegacyDX8RendererAccess::GetTextureBitdepth();
}

RenderResult SetGameMSAAMode(unsigned int mode)
{
	D3DMULTISAMPLE_TYPE translated;
	switch (mode)
	{
	case GAME_RENDER_MULTISAMPLE_NONE:
		translated = D3DMULTISAMPLE_NONE;
		break;
	case GAME_RENDER_MULTISAMPLE_2X:
		translated = D3DMULTISAMPLE_2_SAMPLES;
		break;
	case GAME_RENDER_MULTISAMPLE_4X:
		translated = D3DMULTISAMPLE_4_SAMPLES;
		break;
	case GAME_RENDER_MULTISAMPLE_8X:
		translated = D3DMULTISAMPLE_8_SAMPLES;
		break;
	default:
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	ObserveFrame();
	LegacyDX8RendererAccess::SetMSAAMode(translated);
	return LegacyFailureResult(RENDER_RESULT_OK);
}

unsigned int GetGameMSAAMode()
{
	switch (LegacyDX8RendererAccess::GetMSAAMode())
	{
	case D3DMULTISAMPLE_2_SAMPLES:
		return GAME_RENDER_MULTISAMPLE_2X;
	case D3DMULTISAMPLE_4_SAMPLES:
		return GAME_RENDER_MULTISAMPLE_4X;
	case D3DMULTISAMPLE_8_SAMPLES:
		return GAME_RENDER_MULTISAMPLE_8X;
	default:
		return GAME_RENDER_MULTISAMPLE_NONE;
	}
}

RenderResult SetGameGamma(float gamma, float brightness, float contrast,
	bool calibrate)
{
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		if (DX8Wrapper::_Get_D3D_Device8() == 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return LegacyFailureResult(RENDER_RESULT_INVALID_ARGUMENT);
	}
	DX8Wrapper::Set_Gamma(gamma, brightness, contrast, calibrate);
	CheckDeviceAfterVoidCall();
	return LegacyFailureResult(RENDER_RESULT_OK);
}

void SetGameCleanupHook(GameRenderCleanupHook *hook)
{
	DX8Wrapper::SetCleanupHook(hook);
}

void ReleaseGameSnowVertexBuffer(void *opaque)
{
	if (opaque != 0)
		static_cast<IDirect3DVertexBuffer8 *>(opaque)->Release();
}

void SetGameCursorVisible(bool visible)
{
	if (!CheckDevice())
	{
		return;
	}
	DX8Wrapper::Set_Cursor_Visible(visible);
	CheckDeviceAfterVoidCall();
}

void SetGameCursorProperties(int hotspotX, int hotspotY,
	SurfaceClass *surface)
{
	if (!CheckDevice() || surface == 0 ||
		DX8Wrapper::_Get_D3D_Device8() == 0 ||
		surface->Peek_D3D_Surface() == 0)
	{
		if (surface == 0 || DX8Wrapper::_Get_D3D_Device8() == 0)
		{
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		}
		return;
	}
	const HRESULT result = DX8Wrapper::_Get_D3D_Device8()->SetCursorProperties(
		hotspotX, hotspotY, surface->Peek_D3D_Surface());
	RecordFailure(FromHRESULT(result));
}

void SetGameCursorPosition(int x, int y)
{
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		return;
	}
	DX8Wrapper::_Get_D3D_Device8()->SetCursorPosition(x, y,
		D3DCURSOR_IMMEDIATE_UPDATE);
	CheckDeviceAfterVoidCall();
}

void SetGameShader(const ShaderClass &shader)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Shader(shader);
	CheckDeviceAfterVoidCall();
}

RenderResult ApplyGameShaderBits(unsigned int shaderBits)
{
	if (!CheckDevice())
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;

	const DX8Caps *caps = DX8Wrapper::Get_Current_Caps();
	if (caps == 0)
	{
		RecordFailure(RENDER_RESULT_FAILED);
		return g_game_render_failure.result();
	}

	const ShaderClass shader(shaderBits);
	const unsigned int textureOpCaps =
		caps->Get_DX8_Caps().TextureOpCaps;

	// COLOR MASK, BLENDING, AND ALPHA TEST
	D3DBLEND sourceBlend = D3DBLEND_ONE;
	D3DBLEND destinationBlend = D3DBLEND_ZERO;
	if (shader.Get_Color_Mask() != ShaderClass::COLOR_WRITE_ENABLE)
	{
		sourceBlend = D3DBLEND_ZERO;
		destinationBlend = D3DBLEND_ONE;
	}
	else
	{
		switch (shader.Get_Src_Blend_Func())
		{
		case ShaderClass::SRCBLEND_ZERO: sourceBlend = D3DBLEND_ZERO; break;
		case ShaderClass::SRCBLEND_ONE: sourceBlend = D3DBLEND_ONE; break;
		case ShaderClass::SRCBLEND_SRC_ALPHA: sourceBlend = D3DBLEND_SRCALPHA; break;
		case ShaderClass::SRCBLEND_ONE_MINUS_SRC_ALPHA:
			sourceBlend = D3DBLEND_INVSRCALPHA;
			break;
		default:
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return g_game_render_failure.result();
		}

		switch (shader.Get_Dst_Blend_Func())
		{
		case ShaderClass::DSTBLEND_ZERO: destinationBlend = D3DBLEND_ZERO; break;
		case ShaderClass::DSTBLEND_ONE: destinationBlend = D3DBLEND_ONE; break;
		case ShaderClass::DSTBLEND_SRC_COLOR: destinationBlend = D3DBLEND_SRCCOLOR; break;
		case ShaderClass::DSTBLEND_ONE_MINUS_SRC_COLOR:
			destinationBlend = D3DBLEND_INVSRCCOLOR;
			break;
		case ShaderClass::DSTBLEND_SRC_ALPHA: destinationBlend = D3DBLEND_SRCALPHA; break;
		case ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA:
			destinationBlend = D3DBLEND_INVSRCALPHA;
			break;
		default:
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return g_game_render_failure.result();
		}
	}

	BOOL blendEnabled = FALSE;
	if (sourceBlend != D3DBLEND_ONE || destinationBlend != D3DBLEND_ZERO)
	{
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, sourceBlend);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, destinationBlend);
		blendEnabled = TRUE;
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, blendEnabled);

	BOOL alphaTestEnabled = FALSE;
	if (shader.Get_Alpha_Test() == ShaderClass::ALPHATEST_ENABLE)
	{
		const unsigned char alphaReference = 0x60;
		if (sourceBlend == D3DBLEND_INVSRCALPHA)
		{
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHAREF,
				0xff - alphaReference);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHAFUNC,
				D3DCMP_LESSEQUAL);
		}
		else
		{
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHAREF,
				alphaReference);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHAFUNC,
				D3DCMP_GREATEREQUAL);
		}
		alphaTestEnabled = TRUE;
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE,
		alphaTestEnabled);

	// FOG
	if (DX8Wrapper::Get_Fog_Enable())
	{
		BOOL fogEnabled = FALSE;
		D3DCOLOR fogColor = DX8Wrapper::Get_Fog_Color();
		switch (shader.Get_Fog_Func())
		{
		case ShaderClass::FOG_ENABLE:
			fogEnabled = TRUE;
			break;
		case ShaderClass::FOG_SCALE_FRAGMENT:
			fogColor = 0;
			fogEnabled = TRUE;
			break;
		case ShaderClass::FOG_WHITE:
			fogColor = 0xffffff;
			fogEnabled = TRUE;
			break;
		case ShaderClass::FOG_DISABLE:
			break;
		default:
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return g_game_render_failure.result();
		}
		DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, fogEnabled);
		if (fogEnabled)
			DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGCOLOR, fogColor);
	}
	else
	{
		DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	}

	// The fixed-function texture stages intentionally retain the historical
	// two-stage algorithm.  The compatibility lane owns these calls; the
	// native lane decodes the same serialized bits independently.
	D3DTEXTUREOP primaryColorOp = D3DTOP_SELECTARG1;
	DWORD primaryColorArg1 = D3DTA_DIFFUSE;
	DWORD primaryColorArg2 = D3DTA_DIFFUSE;
	D3DTEXTUREOP primaryAlphaOp = D3DTOP_SELECTARG1;
	DWORD primaryAlphaArg1 = D3DTA_DIFFUSE;
	DWORD primaryAlphaArg2 = D3DTA_DIFFUSE;
	D3DTEXTUREOP secondaryColorOp = D3DTOP_DISABLE;
	DWORD secondaryColorArg1 = D3DTA_TEXTURE;
	DWORD secondaryColorArg2 = D3DTA_CURRENT;
	D3DTEXTUREOP secondaryAlphaOp = D3DTOP_DISABLE;
	DWORD secondaryAlphaArg1 = D3DTA_TEXTURE;
	DWORD secondaryAlphaArg2 = D3DTA_CURRENT;

	if (shader.Get_Texturing() == ShaderClass::TEXTURING_ENABLE)
	{
		switch (shader.Get_Primary_Gradient())
		{
		case ShaderClass::GRADIENT_DISABLE:
			primaryColorOp = D3DTOP_SELECTARG1;
			primaryColorArg1 = D3DTA_TEXTURE;
			primaryColorArg2 = D3DTA_CURRENT;
			primaryAlphaOp = D3DTOP_SELECTARG1;
			primaryAlphaArg1 = D3DTA_TEXTURE;
			primaryAlphaArg2 = D3DTA_CURRENT;
			break;
		case ShaderClass::GRADIENT_MODULATE:
			primaryColorOp = D3DTOP_MODULATE;
			primaryColorArg1 = D3DTA_TEXTURE;
			primaryColorArg2 = D3DTA_DIFFUSE;
			primaryAlphaOp = D3DTOP_MODULATE;
			primaryAlphaArg1 = D3DTA_TEXTURE;
			primaryAlphaArg2 = D3DTA_DIFFUSE;
			break;
		case ShaderClass::GRADIENT_ADD:
			primaryColorOp = (textureOpCaps & D3DTEXOPCAPS_ADD) != 0 ?
				D3DTOP_ADD : D3DTOP_MODULATE;
			primaryColorArg1 = D3DTA_TEXTURE;
			primaryColorArg2 = D3DTA_DIFFUSE;
			primaryAlphaOp = D3DTOP_MODULATE;
			primaryAlphaArg1 = D3DTA_TEXTURE;
			primaryAlphaArg2 = D3DTA_DIFFUSE;
			break;
		case ShaderClass::GRADIENT_BUMPENVMAP:
			if (textureOpCaps & D3DTEXOPCAPS_BUMPENVMAP)
			{
				primaryColorOp = D3DTOP_BUMPENVMAP;
				primaryColorArg1 = D3DTA_TEXTURE;
				primaryColorArg2 = D3DTA_DIFFUSE;
				primaryAlphaOp = D3DTOP_DISABLE;
				primaryAlphaArg1 = D3DTA_TEXTURE;
				primaryAlphaArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::GRADIENT_BUMPENVMAPLUMINANCE:
			if (textureOpCaps & D3DTEXOPCAPS_BUMPENVMAPLUMINANCE)
			{
				primaryColorOp = D3DTOP_BUMPENVMAPLUMINANCE;
				primaryColorArg1 = D3DTA_TEXTURE;
				primaryColorArg2 = D3DTA_DIFFUSE;
				primaryAlphaOp = D3DTOP_DISABLE;
				primaryAlphaArg1 = D3DTA_TEXTURE;
				primaryAlphaArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::GRADIENT_MODULATE2X:
			primaryColorOp = (textureOpCaps & D3DTOP_MODULATE2X) != 0 ?
				D3DTOP_MODULATE2X : D3DTOP_MODULATE;
			primaryColorArg1 = D3DTA_TEXTURE;
			primaryColorArg2 = D3DTA_DIFFUSE;
			primaryAlphaOp = D3DTOP_MODULATE;
			primaryAlphaArg1 = D3DTA_TEXTURE;
			primaryAlphaArg2 = D3DTA_DIFFUSE;
			break;
		default:
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return g_game_render_failure.result();
		}
	}
	else
	{
		switch (shader.Get_Primary_Gradient())
		{
		case ShaderClass::GRADIENT_DISABLE:
			primaryColorOp = D3DTOP_DISABLE;
			primaryColorArg1 = D3DTA_TEXTURE;
			primaryColorArg2 = D3DTA_CURRENT;
			primaryAlphaOp = D3DTOP_DISABLE;
			primaryAlphaArg1 = D3DTA_TEXTURE;
			primaryAlphaArg2 = D3DTA_CURRENT;
			break;
		case ShaderClass::GRADIENT_MODULATE:
		case ShaderClass::GRADIENT_ADD:
		default:
			primaryColorOp = D3DTOP_SELECTARG2;
			primaryColorArg1 = D3DTA_TEXTURE;
			primaryColorArg2 = D3DTA_DIFFUSE;
			primaryAlphaOp = D3DTOP_SELECTARG2;
			primaryAlphaArg1 = D3DTA_TEXTURE;
			primaryAlphaArg2 = D3DTA_DIFFUSE;
			break;
		}
	}

	if (WW3D::Is_Coloring_Enabled())
	{
		primaryColorArg2 = D3DTA_TFACTOR;
		primaryAlphaArg2 = D3DTA_TFACTOR;
		primaryColorOp = D3DTOP_SELECTARG2;
		primaryAlphaOp = D3DTOP_SELECTARG2;
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP,
		primaryColorOp);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1,
		primaryColorArg1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2,
		primaryColorArg2);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP,
		primaryAlphaOp);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1,
		primaryAlphaArg1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG2,
		primaryAlphaArg2);

	if (shader.Get_Texturing() == ShaderClass::TEXTURING_ENABLE)
	{
		switch (shader.Get_Post_Detail_Color_Func())
		{
		case ShaderClass::DETAILCOLOR_DETAIL:
			if (textureOpCaps & D3DTEXOPCAPS_MODULATE)
			{
				secondaryColorOp = D3DTOP_SELECTARG1;
				secondaryColorArg1 = D3DTA_TEXTURE;
				secondaryColorArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_SCALE:
			if (textureOpCaps & D3DTEXOPCAPS_MODULATE)
			{
				secondaryColorOp = D3DTOP_MODULATE;
				secondaryColorArg1 = D3DTA_TEXTURE;
				secondaryColorArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_INVSCALE:
			if (textureOpCaps & D3DTEXOPCAPS_ADDSMOOTH)
			{
				secondaryColorOp = D3DTOP_ADDSMOOTH;
				secondaryColorArg1 = D3DTA_TEXTURE;
				secondaryColorArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_ADD:
			if (textureOpCaps & D3DTEXOPCAPS_ADD)
			{
				secondaryColorOp = D3DTOP_ADD;
				secondaryColorArg1 = D3DTA_TEXTURE;
				secondaryColorArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_SUB:
		case ShaderClass::DETAILCOLOR_SUBR:
			if (textureOpCaps & D3DTEXOPCAPS_SUBTRACT)
			{
				secondaryColorOp = D3DTOP_SUBTRACT;
				secondaryColorArg1 = shader.Get_Post_Detail_Color_Func() ==
					ShaderClass::DETAILCOLOR_SUBR ? D3DTA_CURRENT : D3DTA_TEXTURE;
				secondaryColorArg2 = shader.Get_Post_Detail_Color_Func() ==
					ShaderClass::DETAILCOLOR_SUBR ? D3DTA_TEXTURE : D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_BLEND:
			if (textureOpCaps & D3DTEXOPCAPS_BLENDTEXTUREALPHA)
			{
				secondaryColorOp = D3DTOP_BLENDTEXTUREALPHA;
				secondaryColorArg1 = D3DTA_TEXTURE;
				secondaryColorArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_DETAILBLEND:
			if (textureOpCaps & D3DTEXOPCAPS_BLENDCURRENTALPHA)
			{
				secondaryColorOp = D3DTOP_BLENDCURRENTALPHA;
				secondaryColorArg1 = D3DTA_TEXTURE;
				secondaryColorArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILCOLOR_DISABLE:
			break;
		default:
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return g_game_render_failure.result();
		}
	}
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP,
		secondaryColorOp);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG1,
		secondaryColorArg1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG2,
		secondaryColorArg2);

	if (shader.Get_Texturing() == ShaderClass::TEXTURING_ENABLE)
	{
		switch (shader.Get_Post_Detail_Alpha_Func())
		{
		case ShaderClass::DETAILALPHA_DETAIL:
			if (textureOpCaps & D3DTEXOPCAPS_MODULATE)
			{
				secondaryAlphaOp = D3DTOP_SELECTARG1;
				secondaryAlphaArg1 = D3DTA_TEXTURE;
				secondaryAlphaArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILALPHA_SCALE:
			if (textureOpCaps & D3DTEXOPCAPS_MODULATE)
			{
				secondaryAlphaOp = D3DTOP_MODULATE;
				secondaryAlphaArg1 = D3DTA_TEXTURE;
				secondaryAlphaArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILALPHA_INVSCALE:
			if (textureOpCaps & D3DTEXOPCAPS_ADDSMOOTH)
			{
				secondaryAlphaOp = D3DTOP_ADDSMOOTH;
				secondaryAlphaArg1 = D3DTA_TEXTURE;
				secondaryAlphaArg2 = D3DTA_CURRENT;
			}
			break;
		case ShaderClass::DETAILALPHA_DISABLE:
			break;
		default:
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return g_game_render_failure.result();
		}
	}
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP,
		secondaryAlphaOp);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAARG1,
		secondaryAlphaArg1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAARG2,
		secondaryAlphaArg2);

	DX8Wrapper::Set_DX8_Render_State(D3DRS_SPECULARENABLE,
		shader.Get_Secondary_Gradient() !=
		ShaderClass::SECONDARY_GRADIENT_DISABLE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,
		static_cast<unsigned int>(D3DCMPFUNC(
			static_cast<int>(shader.Get_Depth_Compare()) + 1)));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,
		shader.Get_Depth_Mask() == ShaderClass::DEPTH_WRITE_ENABLE);

	const D3DCULL polygonCullMode = shader.Get_Cull_Mode() ==
		ShaderClass::CULL_MODE_ENABLE ?
		(ShaderClass::Is_Backface_Culling_Inverted() ? D3DCULL_CCW : D3DCULL_CW) :
		D3DCULL_NONE;
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, polygonCullMode);

	float patchLevel = 1.0f;
	if (shader.Get_NPatch_Enable())
		patchLevel = static_cast<float>(WW3D::Get_NPatches_Level());
	DWORD patchLevelBits;
	memcpy(&patchLevelBits, &patchLevel, sizeof(patchLevelBits));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_PATCHSEGMENTS,
		patchLevelBits);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE,
		shader.Get_Alpha_Test() == ShaderClass::ALPHATEST_ENABLE);

	CheckDeviceAfterVoidCall();
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() : RENDER_RESULT_OK;
}

HRESULT CreateLegacyD3DShader(const char *assetPath,
	const DWORD *declarationWords, DWORD usage, bool vertexShader,
	DWORD *handle)
{
	if (assetPath == 0 || handle == 0 ||
		(vertexShader && declarationWords == 0) ||
		DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		return E_INVALIDARG;
	}

	File *file = 0;
	try
	{
		if (TheFileSystem == 0)
			return E_FAIL;
		file = TheFileSystem->openFile(assetPath, File::READ | File::BINARY);
		if (file == 0)
			return E_FAIL;

		FileInfo fileInfo;
		if (!TheFileSystem->getFileInfo(AsciiString(assetPath), &fileInfo) ||
			fileInfo.sizeLow <= 0)
		{
			file->close();
			return E_FAIL;
		}
		const DWORD fileSize = static_cast<DWORD>(fileInfo.sizeLow);
		DWORD *shader = reinterpret_cast<DWORD *>(HeapAlloc(
			GetProcessHeap(), HEAP_ZERO_MEMORY, fileSize));
		if (shader == 0)
		{
			file->close();
			return E_OUTOFMEMORY;
		}

		const Int bytesRead = file->read(shader, static_cast<Int>(fileSize));
		file->close();
		file = 0;
		if (bytesRead != static_cast<Int>(fileSize))
		{
			HeapFree(GetProcessHeap(), 0, shader);
			return E_FAIL;
		}

		HRESULT result;
		if (vertexShader)
		{
			result = DX8Wrapper::_Get_D3D_Device8()->CreateVertexShader(
				declarationWords, shader, handle, usage);
		}
		else
		{
			result = DX8Wrapper::_Get_D3D_Device8()->CreatePixelShader(
				shader, handle);
		}
		HeapFree(GetProcessHeap(), 0, shader);
		return result;
	}
	catch (...)
	{
		if (file != 0)
			file->close();
		return E_FAIL;
	}
}

RenderResult SetGameShaderCullInverted(bool inverted)
{
	if (!CheckDevice())
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE,
		inverted ? D3DCULL_CCW : D3DCULL_CW);
	CheckDeviceAfterVoidCall();
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() : RENDER_RESULT_OK;
}

void SetGameTexture(unsigned int stage, TextureBaseClass *texture)
{
	if (stage >= MAX_TEXTURE_STAGES || !CheckDevice())
	{
		if (stage >= MAX_TEXTURE_STAGES) RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	DX8Wrapper::Set_Texture(stage, texture);
	CheckDeviceAfterVoidCall();
}

void SetGameMaterial(const VertexMaterialClass *material)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Material(material);
	CheckDeviceAfterVoidCall();
}

void SetGameLightEnvironment(LightEnvironmentClass *lightEnvironment)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Light_Environment(lightEnvironment);
	CheckDeviceAfterVoidCall();
}

void SetGameRenderCamera(void *camera)
{
	if (!CheckDevice()) return;
	TheDX8MeshRenderer.Set_Camera(static_cast<CameraClass *>(camera));
}

RenderResult SetGameFogState(const LegacyFogConstants &fog)
{
	if (!CheckDevice())
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	const Vector3 color(fog.color.x, fog.color.y, fog.color.z);
	DX8Wrapper::Set_Fog(fog.enabled, color, fog.start, fog.end);
	CheckDeviceAfterVoidCall();
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() : RENDER_RESULT_OK;
}

RenderResult SetGameLightState(unsigned int index, const LegacyLightState &light)
{
	if (index >= LEGACY_LIGHT_COUNT)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!CheckDevice())
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	if (!light.enabled)
	{
		DX8Wrapper::Set_Light(index, static_cast<const D3DLIGHT8 *>(0));
	}
	else
	{
		D3DLIGHT8 d3dLight;
		memset(&d3dLight, 0, sizeof(d3dLight));
		d3dLight.Type = light.type == RENDER_LIGHT_POINT ? D3DLIGHT_POINT :
			(light.type == RENDER_LIGHT_SPOT ? D3DLIGHT_SPOT :
			D3DLIGHT_DIRECTIONAL);
		d3dLight.Diffuse.r = light.diffuse.x;
		d3dLight.Diffuse.g = light.diffuse.y;
		d3dLight.Diffuse.b = light.diffuse.z;
		d3dLight.Diffuse.a = light.diffuse.w;
		d3dLight.Specular.r = light.specular.x;
		d3dLight.Specular.g = light.specular.y;
		d3dLight.Specular.b = light.specular.z;
		d3dLight.Specular.a = light.specular.w;
		d3dLight.Ambient.r = light.ambient.x;
		d3dLight.Ambient.g = light.ambient.y;
		d3dLight.Ambient.b = light.ambient.z;
		d3dLight.Ambient.a = light.ambient.w;
		d3dLight.Position.x = light.position.x;
		d3dLight.Position.y = light.position.y;
		d3dLight.Position.z = light.position.z;
		d3dLight.Direction.x = light.direction.x;
		d3dLight.Direction.y = light.direction.y;
		d3dLight.Direction.z = light.direction.z;
		d3dLight.Range = light.range;
		d3dLight.Falloff = light.falloff;
		d3dLight.Attenuation0 = light.attenuation0;
		d3dLight.Attenuation1 = light.attenuation1;
		d3dLight.Attenuation2 = light.attenuation2;
		d3dLight.Theta = light.theta;
		d3dLight.Phi = light.phi;
		DX8Wrapper::Set_Light(index, &d3dLight);
	}
	CheckDeviceAfterVoidCall();
	return g_game_render_failure.hasFailure() ?
		g_game_render_failure.result() : RENDER_RESULT_OK;
}

void SetGameTransform(GameRenderTransformSlot slot, const Matrix3D &matrix)
{
	D3DTRANSFORMSTATETYPE transform;
	if (!ToTransform(slot, &transform))
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Transform(transform, matrix);
	CheckDeviceAfterVoidCall();
}

void SetGameTransform(GameRenderTransformSlot slot, const Matrix4x4 &matrix)
{
	D3DTRANSFORMSTATETYPE transform;
	if (!ToTransform(slot, &transform))
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Transform(transform, matrix);
	CheckDeviceAfterVoidCall();
}

void SetGameTransform(GameRenderTransformSlot slot, const void *matrix)
{
	if (matrix == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	const Matrix4x4 converted = CopyMatrixFromLegacy(matrix);
	SetGameTransform(slot, converted);
}

void GetGameTransform(GameRenderTransformSlot slot, void *matrix)
{
	D3DTRANSFORMSTATETYPE transform;
	if (matrix == 0 || !ToTransform(slot, &transform))
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	Matrix4x4 converted;
	DX8Wrapper::Get_Transform(transform, converted);
	CopyMatrixToLegacy(converted, matrix);
	CheckDeviceAfterVoidCall();
}

void SetGameViewport(unsigned int x, unsigned int y, unsigned int width,
	unsigned int height, float minimumDepth, float maximumDepth)
{
	if (!CheckDevice()) return;
	D3DVIEWPORT8 viewport;
	viewport.X = x;
	viewport.Y = y;
	viewport.Width = width;
	viewport.Height = height;
	viewport.MinZ = minimumDepth;
	viewport.MaxZ = maximumDepth;
	DX8Wrapper::Set_Viewport(&viewport);
	CheckDeviceAfterVoidCall();
}

void SetGameProjectionTransformWithZBias(const Matrix4x4 &projection,
	float zNear, float zFar)
{
	if (!CheckDevice())
	{
		return;
	}
	DX8Wrapper::Set_Projection_Transform_With_Z_Bias(projection, zNear,
		zFar);
	CheckDeviceAfterVoidCall();
}

void SetGameTextureBumpEnvironment(unsigned int stage, float matrix00,
	float matrix01, float matrix10, float matrix11)
{
	if (stage >= MAX_TEXTURE_STAGES || !CheckDevice())
	{
		if (stage >= MAX_TEXTURE_STAGES) RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_BUMPENVMAT00,
		FloatBits(matrix00));
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_BUMPENVMAT01,
		FloatBits(matrix01));
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_BUMPENVMAT10,
		FloatBits(matrix10));
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_BUMPENVMAT11,
		FloatBits(matrix11));
	CheckDeviceAfterVoidCall();
}

void SetGameVertexBuffer(const VertexBufferClass *buffer, unsigned int stream)
{
	if (stream >= MAX_VERTEX_STREAMS || !CheckDevice())
	{
		if (stream >= MAX_VERTEX_STREAMS) RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	DX8Wrapper::Set_Vertex_Buffer(buffer, stream);
	CheckDeviceAfterVoidCall();
}

bool SetGameVertexBuffer(const DynamicVBAccessClass &buffer)
{
	if (!CheckDevice()) return false;
	const bool result = DX8Wrapper::Set_Vertex_Buffer(buffer);
	if (!result) RecordFailure(RENDER_RESULT_FAILED);
	CheckDeviceAfterVoidCall();
	return result;
}

void SetGameIndexBuffer(const IndexBufferClass *buffer,
	unsigned short indexBaseOffset)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Index_Buffer(buffer, indexBaseOffset);
	CheckDeviceAfterVoidCall();
}

bool SetGameIndexBuffer(const DynamicIBAccessClass &buffer,
	unsigned short indexBaseOffset)
{
	if (!CheckDevice()) return false;
	const bool result = DX8Wrapper::Set_Index_Buffer(buffer, indexBaseOffset);
	if (!result) RecordFailure(RENDER_RESULT_FAILED);
	CheckDeviceAfterVoidCall();
	return result;
}

void SetGameIndexBufferOffset(unsigned int offset)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Index_Buffer_Index_Offset(offset);
	CheckDeviceAfterVoidCall();
}

void SetGameRenderState(GameRenderState state, unsigned int value)
{
	D3DRENDERSTATETYPE translatedState;
	unsigned int translatedValue;
	if (!ToD3DRenderState(state, value, &translatedState, &translatedValue))
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	DX8Wrapper::Set_DX8_Render_State(translatedState, translatedValue);
	CheckDeviceAfterVoidCall();
}

void SetGameTextureStageState(unsigned int stage, GameTextureStageState state,
	unsigned int value)
{
	D3DTEXTURESTAGESTATETYPE translatedState;
	unsigned int translatedValue;
	if (stage >= MAX_TEXTURE_STAGES ||
		!ToD3DTextureStageState(state, value, &translatedState, &translatedValue))
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, translatedState,
		translatedValue);
	CheckDeviceAfterVoidCall();
}

void ApplyGameRenderStateChanges()
{
	if (!CheckDevice()) return;
	DX8Wrapper::Apply_Render_State_Changes();
	CheckDeviceAfterVoidCall();
}

void InvalidateGameRenderStateCache()
{
	ObserveFrame();
	DX8Wrapper::Invalidate_Cached_Render_States();
}

void FlushGameRenderMeshes()
{
	if (!CheckDevice()) return;
	TheDX8MeshRenderer.Flush();
	CheckDeviceAfterVoidCall();
}

void ClearGameRenderMeshPendingDeletes()
{
	if (!CheckDevice()) return;
	TheDX8MeshRenderer.Clear_Pending_Delete_Lists();
	CheckDeviceAfterVoidCall();
}

void InvalidateGameMeshCache()
{
	ObserveFrame();
	TheDX8MeshRenderer.Invalidate();
}

void InvalidateGameMeshRendererCache()
{
	if (!CheckDevice()) return;
	SortingRendererClass::Flush();
	TheDX8MeshRenderer.Invalidate();
	CheckDeviceAfterVoidCall();
}

void SetGameVertexShader(unsigned int shaderOrFormat)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Vertex_Shader(shaderOrFormat);
	CheckDeviceAfterVoidCall();
}

void SetGamePixelShader(unsigned int shader)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Pixel_Shader(shader);
	CheckDeviceAfterVoidCall();
}

void SetGameLegacyVertexProgram(RenderLegacyVertexProgram program)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Legacy_Vertex_Program(program);
	CheckDeviceAfterVoidCall();
}

void SetGameLegacyPixelProgram(RenderLegacyPixelProgram program)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Legacy_Pixel_Program(program);
	CheckDeviceAfterVoidCall();
}

void SetGameVertexShaderConstant(int reg, const void *data, int count)
{
	if (reg < 0 || count <= 0 || data == 0 ||
		static_cast<unsigned int>(reg) + static_cast<unsigned int>(count) >
		MAX_VERTEX_SHADER_CONSTANTS)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Vertex_Shader_Constant(reg, data, count);
	CheckDeviceAfterVoidCall();
}

void SetGamePixelShaderConstant(int reg, const void *data, int count)
{
	if (reg < 0 || count <= 0 || data == 0 ||
		static_cast<unsigned int>(reg) + static_cast<unsigned int>(count) >
		MAX_PIXEL_SHADER_CONSTANTS)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (!CheckDevice()) return;
	DX8Wrapper::Set_Pixel_Shader_Constant(reg, data, count);
	CheckDeviceAfterVoidCall();
}

bool DeleteGameVertexShader(unsigned int shader)
{
	if (shader == 0 || !CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		if (shader != 0 && DX8Wrapper::_Get_D3D_Device8() == 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return shader == 0;
	}
	const HRESULT result = DX8Wrapper::_Get_D3D_Device8()->DeleteVertexShader(shader);
	const RenderResult translated = FromHRESULT(result);
	RecordFailure(translated);
	return translated == RENDER_RESULT_OK;
}

bool DeleteGamePixelShader(unsigned int shader)
{
	if (shader == 0 || !CheckDevice()) return shader == 0;
	const RenderResult translated = FromHRESULT(
		DX8Wrapper::Delete_Pixel_Shader(shader));
	RecordFailure(translated);
	return translated == RENDER_RESULT_OK;
}

unsigned int ConvertGameColorClamp(const Vector4 &color)
{
	return DX8Wrapper::Convert_Color_Clamp(color);
}

void DrawGameTriangles(unsigned short startIndex, unsigned short polygonCount,
	unsigned short minVertexIndex, unsigned short vertexCount)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Draw_Triangles(startIndex, polygonCount, minVertexIndex,
		vertexCount);
	CheckDeviceAfterVoidCall();
}

void DrawGameSortedTriangles(unsigned short startIndex,
	unsigned short polygonCount, unsigned short minVertexIndex,
	unsigned short vertexCount)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Draw_Triangles(BUFFER_TYPE_DYNAMIC_SORTING, startIndex,
		polygonCount, minVertexIndex, vertexCount);
	CheckDeviceAfterVoidCall();
}

void DrawGameSortedTriangles(const GameBoundingSphere &boundingSphere,
	unsigned short startIndex, unsigned short polygonCount,
	unsigned short minVertexIndex, unsigned short vertexCount)
{
	if (!CheckDevice()) return;
	const SphereClass sphere(Vector3(boundingSphere.centerX,
		boundingSphere.centerY, boundingSphere.centerZ), boundingSphere.radius);
	SortingRendererClass::Insert_Triangles(sphere, startIndex, polygonCount,
		minVertexIndex, vertexCount);
	CheckDeviceAfterVoidCall();
}

RenderResult FlushGameSortedTriangles()
{
	if (!CheckDevice())
		return g_game_render_failure.hasFailure() ? g_game_render_failure.result() :
			RENDER_RESULT_INVALID_ARGUMENT;
	SortingRendererClass::Flush();
	CheckDeviceAfterVoidCall();
	return g_game_render_failure.hasFailure() ? g_game_render_failure.result() :
		RENDER_RESULT_OK;
}

unsigned int GetGameLastFramePolygonCount()
{
	return static_cast<unsigned int>(Debug_Statistics::Get_DX8_Polygons());
}

unsigned int GetGameLastFrameVertexCount()
{
	return static_cast<unsigned int>(Debug_Statistics::Get_DX8_Vertices());
}

void DrawGameStrip(unsigned short startIndex, unsigned short indexCount,
	unsigned short minVertexIndex, unsigned short vertexCount)
{
	if (!CheckDevice()) return;
	DX8Wrapper::Draw_Strip(startIndex, indexCount, minVertexIndex, vertexCount);
	CheckDeviceAfterVoidCall();
}

RenderResult DrawGamePrimitiveUP(GamePrimitiveTopology topology,
	unsigned int primitiveCount, const void *vertices, unsigned int stride,
	unsigned int /*vertexFormat*/)
{
	if (vertices == 0 || stride == 0 || !CheckDevice())
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	D3DPRIMITIVETYPE translatedTopology;
	switch (topology)
	{
	case GAME_PRIMITIVE_TRIANGLE_LIST: translatedTopology = D3DPT_TRIANGLELIST; break;
	case GAME_PRIMITIVE_TRIANGLE_STRIP: translatedTopology = D3DPT_TRIANGLESTRIP; break;
	case GAME_PRIMITIVE_LINE_LIST: translatedTopology = D3DPT_LINELIST; break;
	case GAME_PRIMITIVE_LINE_STRIP: translatedTopology = D3DPT_LINESTRIP; break;
	case GAME_PRIMITIVE_POINT_LIST: translatedTopology = D3DPT_POINTLIST; break;
	default:
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = FromHRESULT(DX8Wrapper::Draw_Primitive_UP(
		translatedTopology, primitiveCount, vertices, stride));
	RecordFailure(result);
	return result;
}

void SetGameRenderTarget(TextureClass *colorTexture,
	ZTextureClass *depthTexture, bool useDefaultDepth)
{
	if (!CheckDevice()) return;
	if (colorTexture == 0)
	{
		RecordFailure(FromHRESULT(DX8Wrapper::Restore_Default_Render_Target()));
		return;
	}
	DX8Wrapper::Set_Render_Target_With_Z(colorTexture, depthTexture,
		useDefaultDepth);
	if (!DX8Wrapper::Is_Render_To_Texture())
	{
		RecordFailure(RENDER_RESULT_FAILED);
	}
	CheckDeviceAfterVoidCall();
}

RenderResult GetGameBackBufferInfo(RenderBackBufferInfo *info)
{
	if (info == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0)
	{
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	}
	IDirect3DSurface8 *surface = 0;
	const HRESULT getResult = DX8Wrapper::_Get_D3D_Device8()->GetBackBuffer(
		0, D3DBACKBUFFER_TYPE_MONO, &surface);
	if (FAILED(getResult) || surface == 0)
	{
		const RenderResult result = FromHRESULT(getResult);
		RecordFailure(result);
		return result;
	}
	D3DSURFACE_DESC description;
	const HRESULT descriptionResult = surface->GetDesc(&description);
	surface->Release();
	const RenderResult result = FromHRESULT(descriptionResult);
	if (result != RENDER_RESULT_OK)
	{
		RecordFailure(result);
		return result;
	}
	if (!ToRenderResultFormat(description.Format, &info->format))
	{
		RecordFailure(RENDER_RESULT_UNSUPPORTED);
		return RENDER_RESULT_UNSUPPORTED;
	}
	info->width = description.Width;
	info->height = description.Height;
	return RENDER_RESULT_OK;
}

RenderResult QueueGameBackBufferCapture(
	const RenderCaptureRequestDescriptor &/*descriptor*/,
	RenderCaptureHandle * /*handle*/)
{
	RecordFailure(RENDER_RESULT_UNSUPPORTED);
	return RENDER_RESULT_UNSUPPORTED;
}

unsigned int CancelGameBackBufferCaptures(void * /*consumer*/,
	RenderResult /*reason*/)
{
	RecordFailure(RENDER_RESULT_UNSUPPORTED);
	return 0;
}

void RequestGameBackBufferCapture()
{
	RecordFailure(RENDER_RESULT_UNSUPPORTED);
}

TextureClass *CreateGameRenderTarget(int width, int height, WW3DFormat format)
{
	if (!CheckDevice() || width <= 0 || height <= 0)
	{
		if (width <= 0 || height <= 0)
			RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return 0;
	}
	TextureClass *target = DX8Wrapper::Create_Render_Target(width, height, format);
	if (target == 0)
	{
		RecordFailure(RENDER_RESULT_OUT_OF_MEMORY);
	}
	return target;
}

RenderResult CreateGameRenderTargetPair(int width, int height,
	WW3DFormat colorFormat, WW3DZFormat depthFormat,
	TextureClass **colorTarget, ZTextureClass **depthTarget)
{
	if (colorTarget != 0)
		*colorTarget = 0;
	if (depthTarget != 0)
		*depthTarget = 0;
	if (colorTarget == 0 || depthTarget == 0 || width <= 0 || height <= 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!CheckDevice())
	{
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	}
	DX8Wrapper::Create_Render_Target(width, height, colorFormat, depthFormat,
		colorTarget, depthTarget);
	if (*colorTarget == 0 || *depthTarget == 0 ||
		!(*colorTarget)->Is_Initialized() || !(*depthTarget)->Is_Initialized())
	{
		if (*colorTarget != 0)
		{
			(*colorTarget)->Release_Ref();
			*colorTarget = 0;
		}
		if (*depthTarget != 0)
		{
			(*depthTarget)->Release_Ref();
			*depthTarget = 0;
		}
		RecordFailure(RENDER_RESULT_FAILED);
		return RENDER_RESULT_FAILED;
	}
	return RENDER_RESULT_OK;
}

RenderResult CreateGameShaderFromAsset(const char *assetPath,
	bool vertexShader, const void *declarationWords,
	unsigned int declarationWordCount, unsigned int usage,
	unsigned int *handle)
{
	const DWORD *legacyDeclarationWords =
		static_cast<const DWORD *>(declarationWords);
	if (vertexShader && declarationWords == 0 && declarationWordCount == 0)
	{
		legacyDeclarationWords = HistoricalVertexDeclaration(assetPath,
			&declarationWordCount);
	}
	if (assetPath == 0 || handle == 0 ||
		(vertexShader && (legacyDeclarationWords == 0 ||
			declarationWordCount == 0)))
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!CheckDevice())
	{
		return g_game_render_failure.hasFailure() ?
			g_game_render_failure.result() : RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!vertexShader)
	{
		const LegacyPixelShaderBytecode::Program program =
			EmbeddedPixelProgram(assetPath);
		if (program != LegacyPixelShaderBytecode::PROGRAM_COUNT)
		{
			const LegacyPixelShaderBytecode::Stream &stream =
				LegacyPixelShaderBytecode::Get(program);
			if (!LegacyPixelShaderBytecode::IsValid(stream) ||
				DX8Wrapper::_Get_D3D_Device8() == 0)
			{
				RecordFailure(RENDER_RESULT_FAILED);
				return RENDER_RESULT_FAILED;
			}
			DWORD shader = 0;
			const RenderResult translated = FromHRESULT(
				DX8Wrapper::_Get_D3D_Device8()->CreatePixelShader(
					reinterpret_cast<const DWORD *>(stream.words), &shader));
			RecordFailure(translated);
			if (translated == RENDER_RESULT_OK)
			{
				*handle = shader;
			}
			return translated;
		}
	}
	const HRESULT result = CreateLegacyD3DShader(assetPath,
		legacyDeclarationWords, usage, vertexShader,
		reinterpret_cast<DWORD *>(handle));
	const RenderResult translated = FromHRESULT(result);
	RecordFailure(translated);
	return translated;
}

bool DeleteGameShader(bool vertexShader, unsigned int handle)
{
	if (handle == 0 || !CheckDevice())
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return false;
	}
	HRESULT result = E_FAIL;
	if (vertexShader)
	{
		if (DX8Wrapper::_Get_D3D_Device8() != 0)
			result = DX8Wrapper::_Get_D3D_Device8()->DeleteVertexShader(handle);
	}
	else
	{
		result = DX8Wrapper::Delete_Pixel_Shader(handle);
	}
	const RenderResult translated = FromHRESULT(result);
	RecordFailure(translated);
	return translated == RENDER_RESULT_OK;
}

RenderResult CopyGameActiveTargetToTexture(TextureClass *destination)
{
	if (destination == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!CheckDevice() || DX8Wrapper::_Get_D3D_Device8() == 0 ||
		destination->Peek_D3D_Base_Texture() == 0)
	{
		RecordFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	IDirect3DSurface8 *source = 0;
	IDirect3DSurface8 *target = 0;
	HRESULT result = DX8Wrapper::_Get_D3D_Device8()->GetRenderTarget(&source);
	if (SUCCEEDED(result))
	{
		target = destination->Get_D3D_Surface_Level();
		if (target == 0)
		{
			result = E_FAIL;
		}
		else
		{
			result = DX8Wrapper::_Get_D3D_Device8()->CopyRects(source, 0, 0,
				target, 0);
		}
	}
	if (target != 0) target->Release();
	if (source != 0) source->Release();
	const RenderResult translated = FromHRESULT(result);
	RecordFailure(translated);
	return translated;
}

bool AcquireGameCopiedTextureContent(TextureClass * /*destination*/)
{
	// The x86 compatibility lane performs the surface copy synchronously in
	// W3DProjectedShadow::updateTexture; there is no native copy lease to acquire.
	return false;
}

}
}

#endif
