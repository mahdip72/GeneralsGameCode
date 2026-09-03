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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: W3DShaderManager.cpp ////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: W3DShaderManager.cpp
//
// Created:   Mark Wilczynski, August 2001
//
// Desc:      Perform tests on the currently selected renderer to determine
//			  which of our rendering features are supported.  The system allows
//			  setting up a few custom shaders that are selected based on video
//			  card features.
//
//			  To add a new shader to the system:
//			  0) Add your shader to the ShaderTypes enum
//			  1) Create shader using W3DShaderInterface
//			  2) Repeat step 1 for any alternate shaders
//			  3) Create list of alternate shaders sorted by order of preference.
//				 The first shader which passes hardware validation will be selected.
//			  4) Add list from step 3) to MasterShaderList[].
//
//-----------------------------------------------------------------------------

#include "Renderer/RenderGameClient.h"
#include "WWLib/cpudetect.h"

// Keep the source-level contract explicit without importing the renderer namespace.
using rts::render::GAME_COLOR_WRITE_ALPHA;
using rts::render::GAME_COLOR_WRITE_BLUE;
using rts::render::GAME_COLOR_WRITE_GREEN;
using rts::render::GAME_COLOR_WRITE_RED;
using rts::render::GAME_PRIMITIVE_TRIANGLE_STRIP;
using rts::render::GAME_RENDER_STATE_ALPHA_BLEND_ENABLE;
using rts::render::GAME_RENDER_STATE_COLOR_WRITE_MASK;
using rts::render::GAME_RENDER_STATE_DEPTH_FUNCTION;
using rts::render::GAME_RENDER_STATE_DEPTH_WRITE;
using rts::render::GAME_RENDER_STATE_DESTINATION_BLEND;
using rts::render::GAME_RENDER_STATE_LIGHTING;
using rts::render::GAME_RENDER_STATE_SOURCE_BLEND;
using rts::render::GAME_RENDER_STATE_TEXTURE_FACTOR;
using rts::render::GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE;
using rts::render::GAME_TEXTURE_ARGUMENT_COMPLEMENT;
using rts::render::GAME_TEXTURE_ARGUMENT_CURRENT;
using rts::render::GAME_TEXTURE_ARGUMENT_DIFFUSE;
using rts::render::GAME_TEXTURE_ARGUMENT_FACTOR;
using rts::render::GAME_TEXTURE_ARGUMENT_TEXTURE;
using rts::render::GAME_TEXTURE_COORDINATE_CAMERA_POSITION;
using rts::render::GAME_TEXTURE_COORDINATE_PASSTHROUGH;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_U;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_V;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_W;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_OPERATION;
using rts::render::GAME_TEXTURE_STAGE_COLOR_ARGUMENT0;
using rts::render::GAME_TEXTURE_STAGE_COLOR_ARGUMENT1;
using rts::render::GAME_TEXTURE_STAGE_COLOR_ARGUMENT2;
using rts::render::GAME_TEXTURE_STAGE_COLOR_OPERATION;
using rts::render::GAME_TEXTURE_STAGE_COORDINATE_INDEX;
using rts::render::GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER;
using rts::render::GAME_TEXTURE_STAGE_MINIFICATION_FILTER;
using rts::render::GAME_TEXTURE_STAGE_MIP_FILTER;
using rts::render::GAME_TEXTURE_STAGE_TRANSFORM_FLAGS;
using rts::render::GAME_TEXTURE_TRANSFORM_COUNT2;
using rts::render::GAME_TEXTURE_TRANSFORM_DISABLED;
using rts::render::GAME_TRANSFORM_TEXTURE0;
using rts::render::GAME_TRANSFORM_TEXTURE1;
using rts::render::GAME_TRANSFORM_TEXTURE2;
using rts::render::GAME_TRANSFORM_TEXTURE3;
using rts::render::GAME_TRANSFORM_VIEW;
using rts::render::GAME_VERTEX_DIFFUSE;
using rts::render::GAME_VERTEX_TEX1;
using rts::render::GAME_VERTEX_TEX2;
using rts::render::GAME_VERTEX_XYZRHW;
using rts::render::RENDER_BLEND_DESTINATION_COLOR;
using rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
using rts::render::RENDER_BLEND_ONE;
using rts::render::RENDER_BLEND_SOURCE_ALPHA;
using rts::render::RENDER_BLEND_SOURCE_COLOR;
using rts::render::RENDER_BLEND_ZERO;
using rts::render::RENDER_COMPARE_ALWAYS;
using rts::render::RENDER_COMPARE_EQUAL;
using rts::render::RENDER_COMPARE_LESS_EQUAL;
using rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
using rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE;
using rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE0;
using rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE;
using rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2;
using rts::render::RENDER_LEGACY_PIXEL_MONOCHROME;
using rts::render::RENDER_LEGACY_PIXEL_ROAD_NOISE2;
using rts::render::RENDER_LEGACY_PIXEL_TERRAIN_BASE;
using rts::render::RENDER_LEGACY_PIXEL_TERRAIN_NOISE;
using rts::render::RENDER_LEGACY_PIXEL_TERRAIN_NOISE2;
using rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION;
using rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
using rts::render::RENDER_TEXTURE_ADDRESS_WRAP;
using rts::render::RENDER_TEXTURE_FILTER_LINEAR;
using rts::render::RENDER_TEXTURE_FILTER_NONE;
using rts::render::RENDER_TEXTURE_FILTER_POINT;
using rts::render::RENDER_TEXTURE_OP_ADD;
using rts::render::RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA;
using rts::render::RENDER_TEXTURE_OP_DISABLE;
using rts::render::RENDER_TEXTURE_OP_DOT_PRODUCT_3;
using rts::render::RENDER_TEXTURE_OP_MODULATE;
using rts::render::RENDER_TEXTURE_OP_MULTIPLY_ADD;
using rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
using rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;

#include "Renderer/RenderMatrixMath.h"
#include "WW3D2/assetmgr.h"
#include "Lib/BaseType.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "WW3D2/camera.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DCustomScene.h"
#include "W3DDevice/GameClient/W3DSmudge.h"
#include "GameClient/View.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/Display.h"
#include "GameClient/Water.h"
#include "GameLogic/GameLogic.h"
#include "Common/GlobalData.h"
#include "Common/GameLOD.h"


// Turn this on to turn off pixel shaders. jba[4/3/2003]
#define do_not_DISABLE_PIXEL_SHADERS 1

/** Interface definition for custom shaders we define in our app.  These shaders can perform more complex
	operations than those allowed in the WW3D2 shader system.
*/
class W3DShaderInterface
{
public:
	Int getNumPasses() {return m_numPasses;};	///<return number of passes needed for this shader
	virtual Int set(Int pass) {return TRUE;};		///<setup shader for the specified rendering pass.
	 ///do any custom resetting necessary to bring W3D in sync.
	virtual void reset() {
		ShaderClass::Invalidate();
		rts::render::SetGameTexture(0, nullptr);
		rts::render::SetGameTexture(1, nullptr);};
	virtual Int init() = 0;			///<perform any one time initialization and validation
	virtual Int shutdown() { return TRUE;};			///<release resources used by shader
protected:
	Int m_numPasses;						///<number of passes to complete shader
};

//this table will contain custom versions of each shader tuned for specific video card and user options.
static W3DFilterInterface *W3DFilters[FT_MAX];
static W3DShaderInterface *W3DShaders[W3DShaderManager::ST_MAX];
static Int W3DShadersPassCount[W3DShaderManager::ST_MAX];	//number of passes for each of the above shaders
TextureClass *W3DShaderManager::m_Textures[8];
W3DShaderManager::ShaderTypes W3DShaderManager::m_currentShader;
FilterTypes W3DShaderManager::m_currentFilter=FT_NULL_FILTER; ///< Last filter that was set.
Int W3DShaderManager::m_currentShaderPass;
ChipsetType W3DShaderManager::m_currentChipset;
GraphicsVenderID W3DShaderManager::m_currentVendor;
__int64 W3DShaderManager::m_driverVersion;

Bool W3DShaderManager::m_renderingToTexture = false;
Bool W3DShaderManager::m_renderTargetAvailable = false;	///<native owner published a valid RTT target
TextureClass *W3DShaderManager::m_renderTexture=nullptr;		///<texture into which rendering will be redirected.

// Filter quads are intentionally submitted through the same immediate-mode
// seam used by the rest of GameEngineDevice.  The helper keeps the compact
// quad call sites readable while preserving the failure latch: a rejected
// submission must not be reported as a successfully rendered filter pass.
static Bool DrawGameTriangleStrip(const void *vertices, unsigned int stride,
	unsigned int vertexFormat)
{
	return rts::render::DrawGamePrimitiveUP(
		rts::render::GAME_PRIMITIVE_TRIANGLE_STRIP, 2, vertices, stride,
		vertexFormat) == rts::render::RENDER_RESULT_OK;
}
/*===========================================================================================*/
/*=========      Screen Shaders	=============================================================*/
/*===========================================================================================*/

class ScreenDefaultFilter : public W3DFilterInterface
{
public:
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override {return true;} ///< Called when the filter is started, one time before the first prerender.
protected:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
};

ScreenDefaultFilter screenDefaultFilter;

///Default filter that just renders screen to off-screen texture and then copies it the the screen.
///Useful because we added some full-time unit effects (microwave tank smudge) to Generals MD that need access
///to the background as a texture.  This filter makes that texture always available for these effects.
W3DFilterInterface *ScreenDefaultFilterList[]=
{
	&screenDefaultFilter,
	nullptr
};

Int ScreenDefaultFilter::init()
{
	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return FALSE;
	}

	//Can render to texture, but we don't know if it can read and write to the same texture.
	//Since there is no renderer capability bit to tell you this, we will just hard-code some specific
	//cards that we know should work.

	Int res;

	if ((res=W3DShaderManager::getChipset()) != DC_UNKNOWN)
	{
		if ( res >=	DC_GEFORCE2)
		{
			//Check if their driver is newer than what we tested for this vendor
/*			if (TheGameLODManager)
			{
				if (TheGameLODManager->getTestedDriverVersion(W3DShaderManager::getCurrentVendor()) < W3DShaderManager::getCurrentDriverVersion())
					return FALSE;
			}*/
		}
	}

	W3DFilters[FT_VIEW_DEFAULT]=&screenDefaultFilter;

	return TRUE;
}

Bool ScreenDefaultFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	// TheSuperHackers @bugfix Disable Render To Texture redirection for the default filter
	// When MSAA is forced by Nvidia driver profile depth buffer is multisampled internally.
	// Rendering to non-MSAA texture with this depth buffer corrupts depth testing producing black screen
	// The smudge system has its own Copy path that works without Render To Texture.
	return FALSE;
}

Bool ScreenDefaultFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	TextureClass * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex)
	{
		reset();
		return false;
	}
	if (!set(mode))
	{
		reset();
		return false;
	}

	struct _TRANS_LIT_TEX_VERTEX {
		Vector4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	rts::render::SetGameTexture(0,tex);	//previously rendered frame inside this texture
	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = Vector4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = Vector4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = Vector4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = Vector4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = 0xffffffff;
	v[1].color = 0xffffffff;
	v[2].color = 0xffffffff;
	v[3].color = 0xffffffff;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::SetGameVertexShader(GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1);

	if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1))
	{
		reset();
		return false;
	}

	reset();
	return true;
}

Int ScreenDefaultFilter::set(FilterModes mode)
{
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
	rts::render::SetGameShader(ShaderClass::_PresetOpaqueShader);
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::SetGameTexture(0,nullptr);
	rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_ALWAYS);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);
	rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

	return true;
}

void ScreenDefaultFilter::reset()
{
	rts::render::SetGameTexture(0,nullptr);	//previously rendered frame inside this texture
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::InvalidateGameRenderStateCache();
}

/*=========  ScreenBWFilter	=============================================================*/
///converts viewport to black & white.

Int ScreenBWFilter::m_fadeFrames;
Int ScreenBWFilter::m_curFadeFrame;
Real ScreenBWFilter::m_curFadeValue;
Int ScreenBWFilter::m_fadeDirection;

ScreenBWFilter screenBWFilter;
ScreenBWFilterDOT3 screenBWFilterDOT3;	//slower version for older cards without pixel shaders.

///List of different BW shader implementations in order of preference
W3DFilterInterface *ScreenBWFilterList[]=
{
	&screenBWFilter,
	&screenBWFilterDOT3,	//slower version for older cards without pixel shaders.
	nullptr
};

Int ScreenBWFilter::init()
{
	Int res;

	m_dwBWPixelShader = 0;
	m_curFadeFrame = 0;

	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return false;
	}

	if ((res=W3DShaderManager::getChipset()) != 0)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//Monochrome pixel shader.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\monochrome.pso", false, nullptr, 0, 0,
				&m_dwBWPixelShader) != rts::render::RENDER_RESULT_OK)
				return FALSE;

			W3DFilters[FT_VIEW_BW_FILTER]=&screenBWFilter;

			return TRUE;
		}
	}
	return FALSE;
}

Bool ScreenBWFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = false;
	W3DShaderManager::startRenderToTexture();
	return W3DShaderManager::isRenderingToTexture();
}

Bool ScreenBWFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	TextureClass * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex)
	{
		reset();
		return false;
	}
	if (!set(mode))
	{
		reset();
		return false;
	}

	struct _TRANS_LIT_TEX_VERTEX {
		Vector4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	rts::render::SetGameTexture(0,tex);	//previously rendered frame inside this texture
	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = Vector4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = Vector4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = Vector4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = Vector4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = 0xffffffff;
	v[1].color = 0xffffffff;
	v[2].color = 0xffffffff;
	v[3].color = 0xffffffff;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	rts::render::SetGameVertexShader(GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1);

	if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1))
	{
		reset();
		return false;
	}

	reset();
	return true;
}

Int ScreenBWFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{	//rendering a quad with redirected rendering surface tinted by pixel shader

		if (m_fadeDirection > 0)
		{	//turning effect on
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;

			if (fade<m_fadeFrames)
			{
				m_curFadeValue = (Real)fade/(Real)m_fadeFrames;
			}
			else
			{
				m_curFadeFrame = 0;
				m_curFadeValue = 1.0f;
				m_fadeDirection = 0;
			}
		}
		else
		if (m_fadeDirection < 0)
		{	//turning effect off
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;
			if (fade<m_fadeFrames)
			{
				m_curFadeValue = 1.0f - (Real)fade/(Real)m_fadeFrames;
			}
			else
			{	m_curFadeValue = 0.0f;
				TheTacticalView->setViewFilterMode(FM_NULL_MODE);
				TheTacticalView->setViewFilter(FT_NULL_FILTER);
				m_curFadeFrame = 0;
				m_fadeDirection = 0;
			}
		}

		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		rts::render::SetGameMaterial(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		rts::render::SetGameShader(ShaderClass::_PresetOpaqueShader);
		rts::render::SetGameLegacyVertexProgram(
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
		rts::render::SetGameTexture(0,nullptr);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_ALWAYS);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

		rts::render::SetGamePixelShader(m_dwBWPixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_MONOCHROME);
		Vector4 lumaWeights(0.3f, 0.59f, 0.11f, 1.0f);
		rts::render::SetGamePixelShaderConstant(0, &lumaWeights, 1);

		Vector4	color(1.0f,1.0f,1.0f,1.0f);	//multiply color

		if (mode == FM_VIEW_BW_BLACK_AND_WHITE)
		{	//back & white mode
			color.X=1.0f;
			color.Y=1.0f;
			color.Z=1.0f;
		}
		if (mode == FM_VIEW_BW_RED_AND_WHITE)
		{	//red is on
			color.X = 1.0f;
			color.Y = 0.0f;
			color.Z = 0.0f;
			//inverse red is on
			//red is on
//			color.x = 0.0f;
//			color.y = 1.0f;
//			color.z = 1.0f;
		}
		if (mode == FM_VIEW_BW_GREEN_AND_WHITE)
		{
			color.X = 0.0f;
			color.Y = 1.0f;
			color.Z = 0.0f;
		}

		rts::render::SetGamePixelShaderConstant(1, &color, 1);
		Vector4 fade(m_curFadeValue, m_curFadeValue, m_curFadeValue, 1.0f);
		rts::render::SetGamePixelShaderConstant(2, &fade, 1);
		return true;
	}
	return false;
}

void ScreenBWFilter::reset()
{
	rts::render::SetGameTexture(0,nullptr);	//previously rendered frame inside this texture
	rts::render::SetGamePixelShader(0);
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::InvalidateGameRenderStateCache();
}

Int ScreenBWFilter::shutdown()
{
	if (m_dwBWPixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBWPixelShader);

	m_dwBWPixelShader=0;

	return TRUE;
}

/**Alternate version of the above filter which does not require pixel shaders - good for older cards*/
Int ScreenBWFilterDOT3::init()
{
	Int res;

	m_curFadeFrame = 0;

	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return false;
	}

	if ((res=W3DShaderManager::getChipset()) != 0)
	{
			W3DFilters[FT_VIEW_BW_FILTER]=&screenBWFilterDOT3;
			return TRUE;
	}
	return FALSE;
}

Bool ScreenBWFilterDOT3::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = false;
	W3DShaderManager::startRenderToTexture();
	return W3DShaderManager::isRenderingToTexture();
}

Bool ScreenBWFilterDOT3::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	TextureClass * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex)
	{
		reset();
		return false;
	}
	if (!set(mode))
	{
		reset();
		return false;
	}

	struct _TRANS_LIT_TEX_VERTEX {
		Vector4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = Vector4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = Vector4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = Vector4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = Vector4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();

	DWORD currentFade=(((Int)((1.0f-m_curFadeValue) * 255.0f))<<24) | 0x00ffffff;	//store alpha value

	v[0].color = currentFade;
	v[1].color = currentFade;
	v[2].color = currentFade;
	v[3].color = currentFade;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	rts::render::SetGameVertexShader(GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1);

	//Draw B&W version first
	if (rts::render::GameRendererSupportsDot3())
	{	//Override W3D states with customizations for grayscale
		rts::render::SetGameRenderState(GAME_RENDER_STATE_TEXTURE_FACTOR, 0x80A5CA8E);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT0, GAME_TEXTURE_ARGUMENT_FACTOR | GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR | GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MULTIPLY_ADD);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_CURRENT);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DOT_PRODUCT_3);
	}
	else
	{	//doesn't have DOT3 blend mode so fake it another way.
		rts::render::SetGameRenderState(GAME_RENDER_STATE_TEXTURE_FACTOR, 0x60606060);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
	}

	rts::render::SetGameTexture(0,tex);	//previously rendered frame inside this texture

	if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1))
	{
		reset();
		return false;
	}

	//Draw normal view blended by current fade level
	ShaderClass::Invalidate();	//reset DOT3 blend from above.
	ShaderClass shader=ShaderClass::_PresetAlphaShader;
	shader.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);
	rts::render::SetGameShader(shader);
	rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices
	//replace texture alpha with vertex alpha
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_SELECT_ARGUMENT_2);

	if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1))
	{
		reset();
		return false;
	}

	reset();
	return true;
}

Int ScreenBWFilterDOT3::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{	//rendering a quad with redirected rendering surface tinted by pixel shader

		if (m_fadeDirection > 0)
		{	//turning effect on
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;

			if (fade<m_fadeFrames)
			{
				m_curFadeValue = (Real)fade/(Real)m_fadeFrames;
			}
			else
			{
				m_curFadeFrame = 0;
				m_curFadeValue = 1.0f;
				m_fadeDirection = 0;
			}
		}
		else
		if (m_fadeDirection < 0)
		{	//turning effect off
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;
			if (fade<m_fadeFrames)
			{
				m_curFadeValue = 1.0f - (Real)fade/(Real)m_fadeFrames;
			}
			else
			{	m_curFadeValue = 0.0f;
				TheTacticalView->setViewFilterMode(FM_NULL_MODE);
				TheTacticalView->setViewFilter(FT_NULL_FILTER);
				m_curFadeFrame = 0;
				m_fadeDirection = 0;
			}
		}

		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		rts::render::SetGameMaterial(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		rts::render::SetGameShader(ShaderClass::_PresetOpaqueShader);
		rts::render::SetGameLegacyVertexProgram(
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
		rts::render::SetGameTexture(0,nullptr);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_ALWAYS);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

		return true;
	}
	return false;
}

void ScreenBWFilterDOT3::reset()
{
	rts::render::SetGameTexture(0,nullptr);	//previously rendered frame inside this texture
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::InvalidateGameRenderStateCache();
}

Int ScreenBWFilterDOT3::shutdown()
{
	return TRUE;
}

/*=========  ScreenCrossFadeFilter	=============================================================*/
///Fades screen between 2 different views of the scene with both being visible at once.

Int ScreenCrossFadeFilter::m_fadeFrames;
Int ScreenCrossFadeFilter::m_curFadeFrame;
Real ScreenCrossFadeFilter::m_curFadeValue;
Int ScreenCrossFadeFilter::m_fadeDirection;
TextureClass *ScreenCrossFadeFilter::m_fadePatternTexture=nullptr;
Bool ScreenCrossFadeFilter::m_skipRender = FALSE;

ScreenCrossFadeFilter screenCrossFadeFilter;

///List of different BW shader implementations in order of preference
///@todo: Add a version that doesn't require pixel shader
W3DFilterInterface *ScreenCrossFadeFilterList[]=
{
	&screenCrossFadeFilter,
	nullptr
};

Int ScreenCrossFadeFilter::init()
{
	if (!TheDisplay)
		return FALSE;	//effect is useless without a view so no point initializing for the WB, etc.

	m_curFadeFrame = 0;

	if (!W3DShaderManager::canRenderToTexture())
		// Have to be able to render to texture.
		return FALSE;

	//Load an alpha mask texture that will mix foreground/background views.
	m_fadePatternTexture=WW3DAssetManager::Get_Instance()->Get_Texture("exmask_g.tga");
	if (!m_fadePatternTexture)
		return FALSE;
	m_fadePatternTexture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
	m_fadePatternTexture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
	m_fadePatternTexture->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);

	W3DFilters[FT_VIEW_CROSSFADE]=&screenCrossFadeFilter;

	return TRUE;
}

Bool ScreenCrossFadeFilter::updateFadeLevel()
{
	if (m_fadeDirection > 0)
	{	//turning effect on
		m_curFadeFrame++;
		Int fade = m_curFadeFrame;

		if (fade<m_fadeFrames)
		{
			m_curFadeValue = (Real)fade/(Real)m_fadeFrames;
		}
		else
		{
			m_curFadeFrame = 0;
			m_curFadeValue = 1.0f;
			m_fadeDirection = 0;
			return false;
		}
	}
	else
	if (m_fadeDirection < 0)
	{	//turning effect off
		Int fade = m_curFadeFrame;
		if (fade<m_fadeFrames)
		{
			m_curFadeValue = 1.0f - (Real)fade/(Real)m_fadeFrames;
			m_curFadeFrame++;
		}
		else
		{	m_curFadeValue = 0.0f;
			TheTacticalView->setViewFilterMode(FM_NULL_MODE);
			TheTacticalView->setViewFilter(FT_NULL_FILTER);
			m_curFadeFrame = 0;
			m_fadeDirection = 0;
			return false;
		}
	}
	return true;
}

Bool ScreenCrossFadeFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	if (updateFadeLevel())
	{	//if fade has not completed
		W3DShaderManager::startRenderToTexture();
		if (!W3DShaderManager::isRenderingToTexture())
		{
			m_skipRender = false;
			return false;
		}
		scenePassMode=SCENE_PASS_ALPHA_MASK;
		skipRender = false;
		m_skipRender=true;	//tell the postRender function not to draw into framebuffer yet.
		return true;
	}
	//fade must have completed
	return true;
}

Bool ScreenCrossFadeFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	TextureClass * tex;

	if (m_skipRender)
	{
		//don't render anything to frame buffer because we still need to draw the new scene
		//that we're fading into.  Okay to render on the next call.
		m_skipRender = false;
		doExtraRender = TRUE;
		tex =	W3DShaderManager::endRenderToTexture();
		return true;
	}

	tex=W3DShaderManager::getRenderTexture();

	DEBUG_ASSERTCRASH(tex, ("Require last rendered texture."));
	if (!tex)
	{
		reset();
		return false;
	}
	if (!set(mode))
	{
		reset();
		return false;
	}

	struct _TRANS_LIT_TEX_VERTEX {
		Vector4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
		float	u1;
		float	v1;
	} v[4];

	Int xpos, ypos, width, height;
	Real radius = 0.0f;

	rts::render::SetGameTexture(0,tex);	//previously rendered frame inside this texture
	if (mode == FM_VIEW_CROSSFADE_CIRCLE)
	{	rts::render::SetGameTexture(1, m_fadePatternTexture);
		//Use the current fade level to scale the mask texture, for other modes the texture
		//comes pre-scaled so doesn't require uv scaling.
		radius = (1.0f-m_curFadeValue)*2.0f;
		if (radius <= 0)
			radius = 0.01f;
		radius = 0.5f/radius;
	}

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

/*	Real radius = (1.0f-m_curFadeValue);
	if (radius <= 0)
		radius = 0.01f;
	radius = 25.0f-radius*24.75f;
*/
	//bottom right
	v[0].p = Vector4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	v[0].u1 = 0.5f+radius;	v[0].v1 = 0.5f+radius;
	//top right
	v[1].p = Vector4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[1].u1 = 0.5f+radius;	v[1].v1 = 0.5f-radius;
	//bottom left
	v[2].p = Vector4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	v[2].u1 = 0.5f-radius;	v[2].v1 = 0.5f+radius;
	//top left
	v[3].p = Vector4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[3].u1 = 0.5f-radius;	v[3].v1 = 0.5f-radius;

	DWORD diffuse = 0xffffffff;//((Int)((m_curFadeValue) * 255.0f) << 24) | 0x00ffffff;	//store alpha value in vertex diffuse

	v[0].color = diffuse;
	v[1].color = diffuse;
	v[2].color = diffuse;
	v[3].color = diffuse;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	rts::render::SetGameVertexShader(GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX2);

	if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX2))
	{
		reset();
		return false;
	}

	reset();
	return true;
}

Int ScreenCrossFadeFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{	//rendering a quad with redirected rendering surface
		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		rts::render::SetGameMaterial(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		rts::render::SetGameShader(ShaderClass::_PresetAlphaShader);
		rts::render::SetGameLegacyVertexProgram(
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
		rts::render::SetGameTexture(0,nullptr);
		rts::render::SetGameTexture(1,nullptr);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

		if (mode == FM_VIEW_CROSSFADE_CIRCLE)
		{	//cross-fading using circle mask stored in stage 1
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1 );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_NONE);
		}

		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_ALWAYS);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);

		return true;
	}
	return false;
}

void ScreenCrossFadeFilter::reset()
{
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	rts::render::SetGameTexture(0,nullptr);	//previously rendered frame inside this texture
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::InvalidateGameRenderStateCache();
}

Int ScreenCrossFadeFilter::shutdown()
{
	REF_PTR_RELEASE(m_fadePatternTexture);

	return TRUE;
}

/*=========  ScreenMotionBlurFilter	=============================================================*/
///applies motion blur to viewport.

ScreenMotionBlurFilter screenMotionBlurFilter;

Coord3D ScreenMotionBlurFilter::m_zoomToPos;
Bool ScreenMotionBlurFilter::m_zoomToValid = false;

ScreenMotionBlurFilter::ScreenMotionBlurFilter():
m_decrement(false),
m_maxCount(0),
m_lastFrame(0),
m_skipRender(false)
{
}
///List of different motion blur implementations in order of preference
W3DFilterInterface *ScreenMotionBlurFilterList[]=
{
	&screenMotionBlurFilter,
	nullptr
};

Int ScreenMotionBlurFilter::init()
{
	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return false;
	}
	W3DFilters[FT_VIEW_MOTION_BLUR_FILTER]=this;
	return true;
}

Bool ScreenMotionBlurFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = m_skipRender;
	W3DShaderManager::startRenderToTexture();
	if (!W3DShaderManager::isRenderingToTexture())
	{
		skipRender = false;
		return false;
	}
	return true;
}

Bool ScreenMotionBlurFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	TextureClass * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex)
	{
		reset();
		return false;
	}
	if (!set(mode))
	{
		reset();
		return false;
	}

	Bool continueEffect = true;
	struct _TRANS_LIT_TEX_VERTEX {
		Vector4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	rts::render::SetGameTexture(0,tex);	//previously rendered frame inside this texture
	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = Vector4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = Vector4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = Vector4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = Vector4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = 0xffffffff;
	v[1].color = 0xffffffff;
	v[2].color = 0xffffffff;
	v[3].color = 0xffffffff;


	if (m_additive) {
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_ONE);
	} else {
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);
	}
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,false);
	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	rts::render::ApplyGameRenderStateChanges();
	rts::render::SetGameVertexShader(GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1);

	Coord2D center;
	center.x = 0.5f;
	center.y = 0.5f;
	Bool pan = false;
	if (mode>=FM_VIEW_MB_PAN_ALPHA) {
		Real len = sqrt(scrollDelta.x*scrollDelta.x + scrollDelta.y*scrollDelta.y);
		//center.x += 0.5f * (scrollDelta.x/len);
		center.y -= 0.5f; // * (scrollDelta.y/len);
		m_decrement = false;
		m_maxCount = (len*200*m_panFactor/(Real)DEFAULT_PAN_FACTOR);
		if (m_maxCount<m_panFactor/2)
			m_maxCount = m_panFactor/2;
		if (m_maxCount>m_panFactor)
			m_maxCount=m_panFactor;
		pan = true;
		m_priorDelta = scrollDelta;
	} else if (mode == FM_VIEW_MB_END_PAN_ALPHA) {
		Real len = sqrt(m_priorDelta.x*m_priorDelta.x + m_priorDelta.y*m_priorDelta.y);
		if (len > 0.0f)
		{
			center.x += 0.5f * (m_priorDelta.x/len);
			center.y -= 0.5f * (m_priorDelta.y/len);
		}
		m_decrement = false;
		m_maxCount--;
		if (m_maxCount<2) {
			continueEffect = false;
		}
		pan = true;
	}


	m_skipRender = false;
	if (!pan && m_lastFrame != TheGameLogic->getFrame()) {
		if (m_decrement) {
			m_maxCount-=COUNT_STEP;
			if (m_maxCount<1) {
				m_decrement = false;
				continueEffect = false;
			}	else {
				m_skipRender = true;
			}
		} else {
			m_maxCount+=COUNT_STEP;
			if (m_maxCount>=MAX_COUNT) {
				m_decrement = true;
				if (m_doZoomTo && m_zoomToValid) {
					TheTacticalView->lookAt(&m_zoomToPos);
				} else {
					continueEffect = false;
				}
			}	else {
				m_skipRender = true;
			}
		}
	}
	Int	 i, j;
	if (!pan) {
		for (i=0; i<4; i++) {
			Real factor = 1.0f - (m_maxCount/(Real)MAX_COUNT)*0.90f;
			factor = sqrt(factor);
			v[i].u = ((v[i].u-center.x)*factor) + center.x;
			v[i].v = ((v[i].v-center.y)*factor) + center.y;
		}
	}
	rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_CURRENT);
	rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_TEXTURE);
	rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_SELECT_ARGUMENT_1);
	if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1))
	{
		reset();
		return false;
	}
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);

	rts::render::ApplyGameRenderStateChanges();
	{
		Int limit = m_maxCount;
		if (m_maxCount>30) limit = 30;
		for (j=0; j<limit; j++) {
			for (i=0; i<4; i++) {
				Real factor = 0.99f;
				if (m_additive) factor = 0.98f;
				Int alpha = 0x15;
				if (m_additive) {
					alpha = 0x09;
					if (m_maxCount>limit) {
						alpha += (m_maxCount-limit)/5;
					}
					if (m_maxCount==MAX_COUNT) alpha += 60;
				}
				v[i].color = (alpha<<24)|0x00ffffff; //
				if (pan) {
					v[i].u = ((v[i].u-center.x)*(factor+.006)) + center.x;
					v[i].v = ((v[i].v-center.y)*factor) + center.y;
				} else {
					v[i].u = ((v[i].u-center.x)*factor) + center.x;
					v[i].v = ((v[i].v-center.y)*factor) + center.y;
				}
			}
			if (!DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
				GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1))
			{
				reset();
				return false;
			}

		}
	}
	m_lastFrame = TheGameLogic->getFrame();
	if (pan){
		m_skipRender = false;
	}
	reset();
	if (!continueEffect) {
		m_zoomToValid = false;
	}
	return continueEffect;
}

Bool ScreenMotionBlurFilter::setup(FilterModes mode)
{

	m_additive = false;

	if (mode == FM_VIEW_MB_IN_AND_OUT_SATURATE ||
			mode == FM_VIEW_MB_IN_SATURATE ||
			mode == FM_VIEW_MB_OUT_SATURATE) {
		m_additive = true;
	}

	m_doZoomTo = false;
	if (mode == FM_VIEW_MB_IN_AND_OUT_SATURATE ||
			mode == FM_VIEW_MB_IN_AND_OUT_ALPHA ) {
		m_doZoomTo = true;
	}
	if (mode >= FM_VIEW_MB_PAN_ALPHA)	{
		m_panFactor = (int)mode - FM_VIEW_MB_PAN_ALPHA;
		if (m_panFactor<1) m_panFactor = DEFAULT_PAN_FACTOR;
	}
	m_skipRender = false;
	if (mode != FM_VIEW_MB_END_PAN_ALPHA)
		m_maxCount = 0;
	m_decrement = false;
	m_skipRender = false;
	switch (mode) {
		case FM_VIEW_MB_OUT_SATURATE:
		case FM_VIEW_MB_OUT_ALPHA:
			m_maxCount = MAX_COUNT;
			m_decrement = TRUE;
			break;
	}
	return true;
}

Int ScreenMotionBlurFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{	//rendering a quad with redirected rendering surface motion blurred

		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		rts::render::SetGameMaterial(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		rts::render::SetGameShader(ShaderClass::_PresetOpaqueShader);
		rts::render::SetGameLegacyVertexProgram(
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
		rts::render::SetGameTexture(0,nullptr);
		rts::render::SetGameTexture(1,nullptr);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_ALWAYS);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);
		rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices
	}
	return TRUE;
}

void ScreenMotionBlurFilter::reset()
{
	rts::render::SetGameTexture(0,nullptr);	//previously rendered frame inside this texture
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::InvalidateGameRenderStateCache();
}

Int ScreenMotionBlurFilter::shutdown()
{
	return TRUE;
}

/*===========================================================================================*/
/*=========      Shroud Shaders	=============================================================*/
/*===========================================================================================*/

///Shroud layer rendering shader
class ShroudTextureShader : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Int m_stageOfSet;
} shroudTextureShader;

///List of different shroud shader implementations in order of preference
W3DShaderInterface *ShroudShaderList[]=
{
	&shroudTextureShader,
	nullptr
};

//#define SHROUD_STRETCH_FACTOR	(1.0f/MAP_XY_FACTOR)	//1 texel per heightmap cell width

Int ShroudTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_SHROUD_TEXTURE]=&shroudTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_SHROUD_TEXTURE]=1;

	return TRUE;
}

//Setup a texture projection in the given stage that applies our shroud.
Int ShroudTextureShader::set(Int stage)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
	rts::render::SetGameTexture(stage, W3DShaderManager::getShaderTexture(0));	//shroud always stored in texture 0

	if (stage == 0)
	{
#if defined(RTS_DEBUG)
	if (TheGlobalData && TheGlobalData->m_fogOfWarOn)
		rts::render::SetGameShader(ShaderClass::_PresetAlphaSpriteShader);
	else
		rts::render::SetGameShader(ShaderClass::_PresetMultiplicativeSpriteShader);
#else
	rts::render::SetGameShader(ShaderClass::_PresetMultiplicativeSpriteShader);
#endif
	}
	rts::render::ApplyGameRenderStateChanges();

	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_EQUAL);

	//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
	//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
	W3DShroud *shroud;
	if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr)
	{	///@todo: All this code really only need to be done once per camera/view.  Find a way to optimize it out.
		RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		RenderMatrix4x4 scale,offset;

		//We need to make all world coordinates be relative to the heightmap data origin since that
		//is where the shroud begins.

		float xoffset = 0;
		float yoffset = 0;
		Real width=shroud->getCellWidth();
		Real height=shroud->getCellHeight();

		if (TheTerrainRenderObject->getMap())
		{	//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
		}

		RenderMatrixTranslation(&offset, xoffset, yoffset,0);

		width = 1.0f/(width*shroud->getTextureWidth());
		height = 1.0f/(height*shroud->getTextureHeight());
		RenderMatrixScaling(&scale, width, height, 1);
		RenderMatrixMultiply(&curView, &inv, &offset);
		RenderMatrixMultiply(&curView, &curView, &scale);
		rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+stage), &curView);
	}
	m_stageOfSet=stage;
	return TRUE;
}

void ShroudTextureShader::reset()
{
	rts::render::SetGameTexture(m_stageOfSet,nullptr);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_LESS_EQUAL);
	rts::render::SetGameTextureStageState(m_stageOfSet,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, m_stageOfSet);
	rts::render::SetGameTextureStageState(m_stageOfSet,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
}

///Shroud layer rendering shader
class FlatShroudTextureShader : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Int m_stageOfSet;
} flatShroudTextureShader;

///List of different shroud shader implementations in order of preference
W3DShaderInterface *FlatShroudShaderList[]=
{
	&flatShroudTextureShader,
	nullptr
};

//#define SHROUD_STRETCH_FACTOR	(1.0f/MAP_XY_FACTOR)	//1 texel per heightmap cell width

Int FlatShroudTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_FLAT_SHROUD_TEXTURE]=&flatShroudTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_SHROUD_TEXTURE]=1;

	return TRUE;
}

//Setup a texture projection in the given stage that applies our shroud.
Int FlatShroudTextureShader::set(Int stage)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	if (stage < 2)
		rts::render::SetGameTexture(stage, W3DShaderManager::getShaderTexture(stage));
	else
		rts::render::SetGameTexture(stage, W3DShaderManager::getShaderTexture(stage));

	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	//rts::render::ApplyGameRenderStateChanges();

	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

	//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
	//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
	W3DShroud *shroud;
	if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr)
	{	///@todo: All this code really only need to be done once per camera/view.  Find a way to optimize it out.
RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		RenderMatrix4x4 scale,offset;

		//We need to make all world coordinates be relative to the heightmap data origin since that
		//is where the shroud begins.

		float xoffset = 0;
		float yoffset = 0;
		Real width=shroud->getCellWidth();
		Real height=shroud->getCellHeight();

		if (TheTerrainRenderObject->getMap())
		{	//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
		}

		RenderMatrixTranslation(&offset, xoffset, yoffset,0);

		width = 1.0f/(width*shroud->getTextureWidth());
		height = 1.0f/(height*shroud->getTextureHeight());
		RenderMatrixScaling(&scale, width, height, 1);
		RenderMatrixMultiply(&curView, &inv, &offset);
		RenderMatrixMultiply(&curView, &curView, &scale);
		rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+stage), &curView);
	}
	m_stageOfSet=stage;
	return TRUE;
}

void FlatShroudTextureShader::reset()
{
	if (m_stageOfSet < static_cast<Int>(rts::render::GetGameMaxTexturesPerPass()))
		rts::render::SetGameTexture(m_stageOfSet,nullptr);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_LESS_EQUAL);
	rts::render::SetGameTextureStageState(m_stageOfSet,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, m_stageOfSet);
	rts::render::SetGameTextureStageState(m_stageOfSet,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
}

///Mask layer rendering shader
class MaskTextureShader : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
} maskTextureShader;

///List of different shroud shader implementations in order of preference
W3DShaderInterface *MaskShaderList[]=
{
	&maskTextureShader,
	nullptr
};

Int MaskTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_MASK_TEXTURE]=&maskTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_MASK_TEXTURE]=1;

	return TRUE;
}

Int MaskTextureShader::set(Int pass)
{
	Real fadeLevel=ScreenCrossFadeFilter::getCurrentFadeValue();

	//Use the current fade level to scale the mask texture
	Real radius = (1.0f-fadeLevel)*2.0f;
	if (radius <= 0)
		radius = 0.01f;
	radius = 0.5f/radius;

	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.

	//For now we're always going to project the texture coming from the crossfade effect
	rts::render::SetGameTexture(0, ScreenCrossFadeFilter::getCurrentMaskTexture());
	ShaderClass shader=ShaderClass::_PresetOpaqueShader;
	shader.Set_Primary_Gradient(ShaderClass::GRADIENT_DISABLE);
	rts::render::SetGameShader(shader);
	rts::render::ApplyGameRenderStateChanges();

RenderMatrix4x4 curView;
						rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

RenderMatrix4x4 inv;
	float det;

	//Get inverse view matrix so we can transform camera space points back to world space
	RenderMatrixInverse(&inv, &det, &curView);

	RenderMatrix4x4 scale,offset,offsetTextureCenter;
	Coord3D centerPos;
	centerPos.zero();

	//Find center of projection (this should be returned from some other filter, etc. but
	//for now assume terrain location at center of screen.
	if (TheTacticalView)
	{	Int xpos,ypos;

		TheTacticalView->getOrigin(&xpos,&ypos);

		ICoord2D screenPos;
		screenPos.x=(Real)TheTacticalView->getWidth()*0.5f;
		screenPos.y=(Real)TheTacticalView->getHeight()*0.5f;
		TheTacticalView->screenToTerrain(&screenPos,&centerPos);
	}

	RenderMatrixTranslation(&offset, -centerPos.x, -centerPos.y,0);

	RenderMatrixTranslation(&offsetTextureCenter, 0.5f, 0.5f, 0);	//shift coordinates so center of projection falls at uv 0.5,0.5

	Real worldTexelWidth=(1.0f-fadeLevel)*25.0f;	//9 worked well for circle but weird shape requires more stretch to cover.
	Real worldTexelHeight=(1.0f-fadeLevel)*25.0f;

	///@todo: Fix this to work with non 128x128 textures.
	if (worldTexelWidth != 0 && worldTexelHeight != 0)
	{
		Real widthScale = 1.0f/(worldTexelWidth*128.0f);
		Real heightScale = 1.0f/(worldTexelHeight*128.0f);
		RenderMatrixScaling(&scale, widthScale, heightScale, 1);
		RenderMatrixMultiply(&curView, &inv, &offset);
		RenderMatrixMultiply(&curView, &curView, &scale);
		RenderMatrixMultiply(&curView, &curView, &offsetTextureCenter);
	}
	else
	{
		RenderMatrixScaling(&scale, 0, 0, 1);	//scaling by 0 will set uv coordinates to 0,0
		RenderMatrixMultiply(&curView, &inv, &offset);
		RenderMatrixMultiply(&curView, &curView, &scale);
	}

	rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);

	return TRUE;
}

void MaskTextureShader::reset()
{
	rts::render::SetGameTexture(0,nullptr);
	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);
	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
}

/*===========================================================================================*/
/*=========      Terrain Shaders	=========================================================*/
/*===========================================================================================*/

///regular terrain shader that should work on all multi-texture video cards (slowest version)
class TerrainShader2Stage : public W3DShaderInterface
{
public:
	float m_xSlidePerSecond ;	 ///< How far the clouds move per second.
	float m_ySlidePerSecond ;	 ///< How far the clouds move per second.
	float m_xOffset;
	float m_yOffset;

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.

	void updateCloud();
void updateNoise1 (RenderMatrix4x4 *destMatrix,RenderMatrix4x4 *curViewInverse, Bool doUpdate=true);	///<generate the uv coordinates for Noise1 (i.e clouds)
void updateNoise2 (RenderMatrix4x4 *destMatrix,RenderMatrix4x4 *curViewInverse, Bool doUpdate=true);	///<generate the uv coordinates for Noise2 (i.e lightmap)
} terrainShader2Stage;

///regular terrain shader that should work on all multi-texture video cards (slowest version)
class FlatTerrainShader2Stage : public W3DShaderInterface
{
public:
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
} flatTerrainShader2Stage;

///regular terrain shader that should work on all multi-texture video cards (slowest version)
class FlatTerrainShaderPixelShader : public W3DShaderInterface
{
public:
	unsigned int				m_dwBasePixelShader;	///<opaque handle to terrain pixel program
	unsigned int				m_dwBaseNoise1PixelShader;	///<opaque handle to terrain/single noise program
	unsigned int				m_dwBaseNoise2PixelShader;	///<opaque handle to terrain/double noise program
	unsigned int				m_dwBase0PixelShader;	///<handle to terrain only pixel shader
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int shutdown() override;			///<release resources used by shader
} flatTerrainShaderPixelShader;

///8 stage terrain shader which only works on certain Nvidia cards.
class TerrainShader8Stage : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int init() override;			///<perform any one time initialization and validation
} terrainShader8Stage;

//Offsets into constant register pool used by vertex shader
#define CV_WORLDVIEWPROJ_0	0	//4 vectors for transform of world->clip space.

///Pixel shader based terrain shader - fastest method for the newest cards.
class TerrainShaderPixelShader : public W3DShaderInterface
{
	unsigned int				m_dwBasePixelShader;	///<opaque handle to terrain pixel program
	unsigned int				m_dwBaseNoise1PixelShader;	///<opaque handle to terrain/single noise program
	unsigned int				m_dwBaseNoise2PixelShader;	///<opaque handle to terrain/double noise program

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;			///<release resources used by shader
} terrainShaderPixelShader;

///List of different terrain shader implementations in order of preference
W3DShaderInterface *TerrainShaderList[]=
{
	&terrainShaderPixelShader,
	&terrainShader8Stage,
	&terrainShader2Stage,
	nullptr
};

///List of different terrain shader implementations in order of preference
W3DShaderInterface *FlatTerrainShaderList[]=
{
	&flatTerrainShaderPixelShader,
	&flatTerrainShader2Stage,
	nullptr
};

Int TerrainShader2Stage::init()
{
	//initialize settings for uv animated clouds
	m_xSlidePerSecond = -0.02f;
	m_ySlidePerSecond =  1.50f * m_xSlidePerSecond;
	m_xOffset = 0;
	m_yOffset = 0;

	//no special device validation needed - anything in our min spec should handle this.

	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE]=2;
	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=3;
	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=3;
	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=3;

	return TRUE;
}

void TerrainShader2Stage::reset()
{
	ShaderClass::Invalidate();

	//Free references to textures
	rts::render::SetGameTexture(0, nullptr);
	rts::render::SetGameTexture(1, nullptr);

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);
}

void TerrainShader2Stage::updateCloud()
{
	const float frame_time = WW3D::Get_Logic_Frame_Time_Seconds();
	m_xOffset += m_xSlidePerSecond * frame_time;
	m_yOffset += m_ySlidePerSecond * frame_time;

	// This moves offsets towards zero when smaller -1.0 or larger 1.0
	m_xOffset -= (Int)m_xOffset;
	m_yOffset -= (Int)m_yOffset;
}

void TerrainShader2Stage::updateNoise1(RenderMatrix4x4 *destMatrix,RenderMatrix4x4 *curViewInverse, Bool doUpdate)
{
	#define STRETCH_FACTOR ((float)(1/(63.0*MAP_XY_FACTOR/2))) /* covers 63/2 tiles */

	RenderMatrix4x4 scale;

	RenderMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	RenderMatrixMultiply(destMatrix, curViewInverse, &scale);

	RenderMatrix4x4 offset;
	RenderMatrixTranslation(&offset, m_xOffset, m_yOffset,0);
	RenderMatrixMultiply(destMatrix, destMatrix, &offset);
}

void TerrainShader2Stage::updateNoise2(RenderMatrix4x4 *destMatrix,RenderMatrix4x4 *curViewInverse, Bool doUpdate)
{

	RenderMatrix4x4 scale;

	RenderMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	RenderMatrixMultiply(destMatrix, curViewInverse, &scale);
}

Int TerrainShader2Stage::set(Int pass)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	rts::render::ApplyGameRenderStateChanges();

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	}

	switch (pass)
	{
		case 0:
			rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(0));
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
			rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,false);
			break;
		case 1:
			rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(1));
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1 );
			// Blend the result using the alpha. (came from diffuse mod texture)
			rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);
			// Disable stage 2.
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			break;
		case 2:
			// Noise/cloud pass
RenderMatrix4x4 curView;
			rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

			//these states apply to all noise/cloud combination passes
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
			// Two output coordinates are used.
			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

			//blend into frame buffer
			rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_DESTINATION_COLOR);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_ZERO);

RenderMatrix4x4 inv;
			float det;
			RenderMatrixInverse(&inv, &det, &curView);

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE12)
			{
				//setup cloud pass
				rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(2));

				updateNoise1(&curView,&inv);	//update curView with texture matrix
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);
				//clouds always need bilinear filtering
				rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

				//setup noise pass
				rts::render::SetGameTexture(1, W3DShaderManager::getShaderTexture(3));

				updateNoise2(&curView,&inv);
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, &curView);
				//noise always needs point/linear filtering.  Why point!?
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
				// Two output coordinates are used.
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
			}
			else
			{	//only 1 noise or cloud texture
				// Now setup the texture pipeline.
				if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE1)
				{	//setup cloud pass
					rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(2));
					updateNoise1(&curView,&inv);	//update curView with texture matrix
					rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
					rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				}
				else
				{
					//setup noise pass
					rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(3));
					updateNoise2(&curView,&inv);	//update curView with texture matrix
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				}

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);
			}
			break;
	}

	return TRUE;
}

Int TerrainShader8Stage::init()
{
	ChipsetType res;

	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if (terrainShader2Stage.init() && (res=W3DShaderManager::getChipset()) >= DC_TNT && res <= DC_GEFORCE2)
	{
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE]=1;
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=2;
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=2;
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=2;
		return TRUE;
	}

	return FALSE;
}

Int TerrainShader8Stage::set(Int pass)
{
	if (pass == 0)
	{
		//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
		rts::render::ApplyGameRenderStateChanges();

		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

		if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
			rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		} else {
			rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
			rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		}
		if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
			rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		} else {
			rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		}

		rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(0));
		rts::render::SetGameTexture(1, W3DShaderManager::getShaderTexture(1));

		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);

		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_ADD);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_DIFFUSE | GAME_TEXTURE_ARGUMENT_COMPLEMENT | GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_ADD);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR | GAME_TEXTURE_ARGUMENT_COMPLEMENT);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);

		rts::render::SetGameTexture(2, nullptr);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 2);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_TEXTURE);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);

		rts::render::SetGameTexture(3, nullptr);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_SELECT_ARGUMENT_1);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 3);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_DIFFUSE | 0 | GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);

		rts::render::SetGameTexture(4, nullptr);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 4);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_CURRENT);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_CURRENT);
		rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);

		rts::render::SetGameTexture(5, nullptr);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_ADD);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 5);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_DIFFUSE);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_ADD);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR | GAME_TEXTURE_ARGUMENT_COMPLEMENT);
		rts::render::SetGameTextureStageState( 5, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);

		rts::render::SetGameTexture(6, nullptr);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 6);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 6, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);

		rts::render::SetGameTexture(7, nullptr);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_SELECT_ARGUMENT_1);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 7);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_FACTOR);
		rts::render::SetGameTextureStageState( 7, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_FACTOR);
	}
	else
	{	//setup cloud noise/pass
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
		rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
		rts::render::InvalidateGameRenderStateCache();

		terrainShader2Stage.set(2);
	}
	return TRUE;
}

void TerrainShader8Stage::reset()
{
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);

	rts::render::SetGameTexture(0, nullptr);
	rts::render::SetGameTexture(1, nullptr);
	rts::render::InvalidateGameRenderStateCache();
}

Int TerrainShaderPixelShader::shutdown()
{
	if (m_dwBasePixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBasePixelShader);

	if (m_dwBaseNoise1PixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBaseNoise1PixelShader);

	if (m_dwBaseNoise2PixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBaseNoise2PixelShader);

	m_dwBasePixelShader=0;
	m_dwBaseNoise1PixelShader=0;
	m_dwBaseNoise2PixelShader=0;

	return TRUE;
}

Int TerrainShaderPixelShader::init()
{
	Int res;
#ifdef DISABLE_PIXEL_SHADERS
	return false;
#endif
	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if (terrainShader2Stage.init() && (res=W3DShaderManager::getChipset()) >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//base version which doesn't apply any noise textures.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\terrain.pso", false, nullptr, 0, 0,
				&m_dwBasePixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			//version which blends 1 noise texture.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\terrainnoise.pso", false, nullptr, 0, 0,
				&m_dwBaseNoise1PixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			//version which blends 2 noise textures.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\terrainnoise2.pso", false, nullptr, 0, 0,
				&m_dwBaseNoise2PixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			W3DShaders[W3DShaderManager::ST_TERRAIN_BASE]=&terrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=&terrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=&terrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=&terrainShaderPixelShader;
			W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE]=1;
			W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=1;
			W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=1;
			W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=1;
			return TRUE;
		}
	}
	return FALSE;
}

Int TerrainShaderPixelShader::set(Int pass)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	rts::render::ApplyGameRenderStateChanges();

	//setup base pass
	rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(0));
	rts::render::SetGameTexture(1, W3DShaderManager::getShaderTexture(1));

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

	//tell pixel shader which UV set to use for each stage
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1 );

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	}

	if (W3DShaderManager::getCurrentShader() >= W3DShaderManager::ST_TERRAIN_BASE_NOISE1)
	{
RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		// Two output coordinates are used.
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

		if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE12)
		{	//full shader
			rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
			rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
			rts::render::SetGameTexture(2, W3DShaderManager::getShaderTexture(2));
			rts::render::SetGameTexture(3, W3DShaderManager::getShaderTexture(3));
			rts::render::SetGamePixelShader(m_dwBaseNoise2PixelShader);
			rts::render::SetGameLegacyPixelProgram(
				rts::render::RENDER_LEGACY_PIXEL_TERRAIN_NOISE2);

			rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

			rts::render::SetGameTextureStageState(3, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
			rts::render::SetGameTextureStageState(3, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

			terrainShader2Stage.updateNoise1(&curView,&inv);	//update curView with texture matrix
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE2, &curView);

			terrainShader2Stage.updateNoise2(&curView,&inv);	//update curView with texture matrix
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE3, &curView);

			rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
			// Two output coordinates are used.
			rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
		}
		else
		{	//single noise texture shader
			rts::render::SetGamePixelShader(m_dwBaseNoise1PixelShader);
			rts::render::SetGameLegacyPixelProgram(
				rts::render::RENDER_LEGACY_PIXEL_TERRAIN_NOISE);

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE1)
			{	//cloud map
				rts::render::SetGameTexture(2, W3DShaderManager::getShaderTexture(2));
				terrainShader2Stage.updateNoise1(&curView,&inv);	//update curView with texture matrix
				rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			}
			else
			{	//light map
				rts::render::SetGameTexture(2, W3DShaderManager::getShaderTexture(3));
				terrainShader2Stage.updateNoise2(&curView,&inv);	//update curView with texture matrix
				rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
				rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			}
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE2, &curView);
		}
	}
	else
	{	//just base texturing
		rts::render::SetGamePixelShader(m_dwBasePixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_TERRAIN_BASE);
	}

	return TRUE;
}

void TerrainShaderPixelShader::reset()
{
	rts::render::SetGameTexture(2,nullptr);	//release reference to any texture
	rts::render::SetGameTexture(3,nullptr);	//release reference to any texture

	rts::render::SetGamePixelShader(0);	//turn off pixel shader
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);

	rts::render::SetGameTexture(0, nullptr);
	rts::render::SetGameTexture(1, nullptr);

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);

	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|2);

	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|3);


	rts::render::InvalidateGameRenderStateCache();
}

///Cloud layer rendering shader - used for objects similar to terrain which only need the cloud layer.
class CloudTextureShader : public W3DShaderInterface
{
	virtual Int set(Int stage) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Int m_stageOfSet;
} cloudTextureShader;

///List of different cloud shader implementations in order of preference
W3DShaderInterface *CloudShaderList[]=
{
	&cloudTextureShader,
	nullptr
};

Int CloudTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_CLOUD_TEXTURE]=&cloudTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_CLOUD_TEXTURE]=1;

	return TRUE;
}

/**Setup a certain texture stage to project our cloud texture*/
Int CloudTextureShader::set(Int stage)
{
RenderMatrix4x4 curView;
	rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
	float det;

	RenderMatrixInverse(&inv, &det, &curView);

	//Get a texture matrix that applies the current cloud position
	terrainShader2Stage.updateNoise1(&curView,&inv,false);	//update curView with texture matrix

	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
	rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+stage), &curView);
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );

	rts::render::SetGameTexture(stage, W3DShaderManager::getShaderTexture(stage));

	m_stageOfSet=stage;
	return TRUE;
}

void CloudTextureShader::reset()
{
	//Free reference to texture
	rts::render::SetGameTexture(m_stageOfSet, nullptr);
	//Turn off texture projection
	rts::render::SetGameTextureStageState( m_stageOfSet, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( m_stageOfSet, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|m_stageOfSet);

	rts::render::SetGameTextureStageState( m_stageOfSet, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	rts::render::SetGameTextureStageState( m_stageOfSet, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
}

/*===========================================================================================*/
/*=========      Road Shaders	=========================================================*/
/*===========================================================================================*/
class RoadShaderPixelShader : public W3DShaderInterface
{
	unsigned int				m_dwBaseNoise2PixelShader;	///<opaque handle to road/double noise program

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;			///<release resources used by shader
} roadShaderPixelShader;

class RoadShader2Stage : public W3DShaderInterface
{	friend class RoadShaderPixelShader;	//pixel shader version uses some of the same features.

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;
} roadShader2Stage;

///List of different terrain shader implementations in order of preference
W3DShaderInterface *RoadShaderList[]=
{
	&roadShaderPixelShader,
	&roadShader2Stage,
	nullptr
};

Int RoadShaderPixelShader::shutdown()
{
	if (m_dwBaseNoise2PixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBaseNoise2PixelShader);

	m_dwBaseNoise2PixelShader=0;

	return TRUE;
}

Int RoadShaderPixelShader::init()
{
	Int res;

	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if (roadShader2Stage.init() && (res=W3DShaderManager::getChipset()) >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//version which blends 2 noise textures.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\roadnoise2.pso", false, nullptr, 0, 0,
				&m_dwBaseNoise2PixelShader) != rts::render::RENDER_RESULT_OK)
				return FALSE;

			//Only set this shader for use in dual noise mode.  The 2Stage shader will take care of
			//all the other modes.
			W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE12]=&roadShaderPixelShader;
			W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE12]=1;
			return TRUE;
		}
	}
	return FALSE;
}

Int RoadShaderPixelShader::set(Int pass)
{
	rts::render::SetGameTexture(0,W3DShaderManager::getShaderTexture(0));
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	rts::render::ApplyGameRenderStateChanges();

	//tell pixel shader which UV set to use for each stage
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );

	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_LESS_EQUAL);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_LIGHTING, FALSE);

	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);	//blend roads into terrain
	rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);

RenderMatrix4x4 curView;
	rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
	float det;
	RenderMatrixInverse(&inv, &det, &curView);

	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex)
	{	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	}
	else
	{	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	// Two output coordinates are used.
	rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

	rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

	rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

	rts::render::SetGameTexture(1,W3DShaderManager::getShaderTexture(1));
	rts::render::SetGameTexture(2,W3DShaderManager::getShaderTexture(2));

	rts::render::SetGamePixelShader(m_dwBaseNoise2PixelShader);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_ROAD_NOISE2);

	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

	terrainShader2Stage.updateNoise1(&curView,&inv, false);	//get texture projection matrix
	rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, &curView);

	terrainShader2Stage.updateNoise2(&curView,&inv, false);	//get texture projection matrix
	rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE2, &curView);

	rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	// Two output coordinates are used.
	rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

	return TRUE;
}

void RoadShaderPixelShader::reset()
{
	rts::render::SetGameTexture(0, nullptr);
	rts::render::SetGameTexture(1, nullptr);
	rts::render::SetGameTexture(2, nullptr);
	rts::render::SetGamePixelShader(0);	//turn off pixel shader
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);

	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|2);

	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|3);


	rts::render::InvalidateGameRenderStateCache();
}

Int RoadShader2Stage::init()
{
	//no special device validation needed - anything in our min spec should handle this.
	W3DShaders[W3DShaderManager::ST_ROAD_BASE]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE]=1;
	W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE1]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE1]=1;
	W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE2]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE2]=1;
	W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE12]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE12]=2;

	return TRUE;
}

Int RoadShader2Stage::set(Int pass)
{
	//First stage always contains base texture.
	rts::render::SetGameTexture(0,W3DShaderManager::getShaderTexture(0));
	//Force system to apply world/view transforms.
	rts::render::ApplyGameRenderStateChanges();

	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION,RENDER_COMPARE_LESS_EQUAL);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE,FALSE);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_LIGHTING, FALSE);

	// Modulate the diffuse color with the texture as lighting comes from diffuse.
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);	//blend roads into terrain

	if (pass == 0)
	{
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);

		if (W3DShaderManager::getCurrentShader() >= W3DShaderManager::ST_ROAD_BASE_NOISE1)
		{	//second texture unit will contain a noise pass
RenderMatrix4x4 curView;
			rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
			float det;
			RenderMatrixInverse(&inv, &det, &curView);

			if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex)
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
			else
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);

			rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
			// Two output coordinates are used.
			rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

			rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
			rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_ROAD_BASE_NOISE12)
			{	//full shader, apply noise 1 in pass 0.
				rts::render::SetGameTexture(1,W3DShaderManager::getShaderTexture(1));
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

				terrainShader2Stage.updateNoise1(&curView, &inv, false);	//get texture projection matrix
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, &curView);
			}
			else
			{	//single noise texture shader
				if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_ROAD_BASE_NOISE1)
				{	//cloud map
					rts::render::SetGameTexture(1,W3DShaderManager::getShaderTexture(1));
					terrainShader2Stage.updateNoise1(&curView, &inv, false);	//update curView with texture matrix
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				}
				else
				{	//light map
					rts::render::SetGameTexture(1,W3DShaderManager::getShaderTexture(2));
					terrainShader2Stage.updateNoise2(&curView,&inv, false);	//update curView with texture matrix
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				}
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, &curView);
			}
		}
		else
		{	//just base texturing
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		}
	}
	else
	{	//pass 1, apply additional noise pass
RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex)
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		else
			rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);

		rts::render::SetGameTexture(1,W3DShaderManager::getShaderTexture(2));

		terrainShader2Stage.updateNoise2(&curView, &inv, false);	//update curView with texture matrix
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		// Two output coordinates are used.
		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

		//Copy alpha channel into stage 1 but mask out color channel by replacing with white.
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		//Force color channel to white by copying the alpha into RGB
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE|GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_2);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );

		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA);
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

		//Modulate into existing roads with clouds applied. - only apply where roads are transparent by
		//using road texture as a mask.
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_ZERO);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_SOURCE_COLOR);

		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);
	}

	return TRUE;
}

void RoadShader2Stage::reset()
{
	ShaderClass::Invalidate();

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);
}

/** List of all custom shader lists - each list in this list contains variations of the same
	shader to allow it to work on different hardware configurations.
*/
W3DShaderInterface **MasterShaderList[]=
{
	TerrainShaderList,
	ShroudShaderList,
	FlatShroudShaderList,
	RoadShaderList,
	MaskShaderList,
	CloudShaderList,
	FlatTerrainShaderList,
	nullptr
};

/** List of all custom filter lists - each list in this list contains variations of the same
	filter to allow it to work on different hardware configurations.
*/
W3DFilterInterface **MasterFilterList[]=
{
	ScreenDefaultFilterList,
	ScreenBWFilterList,
	ScreenMotionBlurFilterList,
	ScreenCrossFadeFilterList,
	nullptr
};

// W3DShaderManager::W3DShaderManager =========================================
/** Constructor - just clears some variables */
//=============================================================================
W3DShaderManager::W3DShaderManager()
{
	m_currentShader = ST_INVALID;
	m_currentFilter = FT_NULL_FILTER;
	m_renderTargetAvailable = false;
	m_renderTexture = nullptr;
	m_renderingToTexture = false;
	Int i;
	for (i=0; i<W3DShaderManager::ST_MAX; i++)
	{	W3DShaders[i]=nullptr;
		W3DShadersPassCount[i]=0;
	}
	for (i=0; i<FT_MAX; i++)
	{	W3DFilters[i]=nullptr;
	}
	for (i=0; i<8; i++)
	{
		m_Textures[i]=nullptr;
	}
	m_currentShader=(W3DShaderManager::ShaderTypes)-1;
}

// W3DShaderManager::init =======================================================
/** Walk through all shaders and find versions suitable for current hardware */
//=============================================================================
void W3DShaderManager::init()
{
	int i,j;
	ChipsetType res=W3DShaderManager::getChipset();
	m_currentChipset = res;
	m_renderTargetAvailable = false;
	m_renderingToTexture = false;
	REF_PTR_RELEASE(m_renderTexture);

	// Effects that need a copy of the frame use the renderer-owned target
	// contract.  The owner rejects multisampled or otherwise incompatible
	// targets; no backend surface is inspected here.
	if (res != DC_UNKNOWN)
	{
		rts::render::RenderBackBufferInfo info;
		if (rts::render::GetGameBackBufferInfo(&info) ==
			rts::render::RENDER_RESULT_OK && info.width > 0 && info.height > 0 &&
			(info.format == rts::render::RENDER_FORMAT_R8G8B8A8_UNORM ||
			 info.format == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM))
		{
			m_renderTexture = rts::render::CreateGameRenderTarget(
				info.width, info.height, WW3D_FORMAT_A8R8G8B8);
			m_renderTargetAvailable = m_renderTexture != nullptr;
		}
	}

	W3DShaderInterface **shaders;

	for (i=0; MasterShaderList[i] != nullptr; i++)
	{
		shaders=MasterShaderList[i];
		for (j=0; shaders[j] != nullptr; j++)
		{
			if (shaders[j]->init())
				break;	//found a working shader
		}
	}
	W3DFilterInterface **filters;

	for (i=0; MasterFilterList[i] != nullptr; i++)
	{
		filters=MasterFilterList[i];
		for (j=0; filters[j] != nullptr; j++)
		{
			if (filters[j]->init())
				break;	//found a working shader
		}
	}
	DEBUG_LOG(("ShaderManager ChipsetID %d", res));
}

// W3DShaderManager::shutdown =======================================================
/** Any shaders which allocate resources will be allowed to free them */
//=============================================================================
void W3DShaderManager::shutdown()
{
	m_renderingToTexture = false;
	m_renderTargetAvailable = false;
	REF_PTR_RELEASE(m_renderTexture);
	m_currentShader = ST_INVALID;
	m_currentFilter = FT_NULL_FILTER;
	//release any assets associated with a shader (vertex/pixel shaders, textures, etc.)
	Int i=0;
	for (; i<W3DShaderManager::ST_MAX; i++) {
		if (W3DShaders[i]) {
			W3DShaders[i]->shutdown();
		}
	}

	for (i=0; i < FT_MAX; i++)
	{
		if (W3DFilters[i])
		{
			W3DFilters[i]->shutdown();
		}
	}
}

//=============================================================================
void W3DShaderManager::updateCloud()
{
	terrainShader2Stage.updateCloud();
}

// W3DShaderManager::getShaderPasses =======================================================
/** Return number of renderig passes required in perform the desired shader on current
	hardware.  App will need to re-render the polygons this many times to complete the
	effect.
 */
//=============================================================================
Int W3DShaderManager::getShaderPasses(ShaderTypes shader)
{
	return W3DShadersPassCount[shader];
}

// W3DShaderManager::setShader =======================================================
/** Must call this method before each rendering pass in order to perform proper renderer
	setup for each shader.
 */
//=============================================================================
Int W3DShaderManager::setShader(ShaderTypes shader, Int pass)
{
	if (shader == m_currentShader && pass == m_currentShaderPass)
		return TRUE;	//shader is already set
	m_currentShader=shader;
	m_currentShaderPass = pass;
	if (W3DShaders[shader])
		return W3DShaders[shader]->set(pass);
	return FALSE;
}

// W3DShaderManager::resetShader =======================================================
/** Must call this method after all polygons and rendering passes have been submitted.
	This method allows the renderer to reset itself to a default state that doesn't conflict
	with the WW3D2 Shader system.
 */
//=============================================================================
void W3DShaderManager::resetShader(ShaderTypes shader)
{
	if (m_currentShader == ST_INVALID)
		return;	//last shader is already reset.
	if (W3DShaders[shader])
		W3DShaders[shader]->reset();
	m_currentShader = ST_INVALID;
}
// W3DShaderManager::filterPreRender =======================================================
/** Call to view filter shaders before rendering starts.
 */
//=============================================================================
Bool W3DShaderManager::filterPreRender(FilterTypes filter, Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	if (W3DFilters[filter])
	{	Bool result=W3DFilters[filter]->preRender(skipRender,scenePassMode);
		if (result)
			m_currentFilter = filter;
		else
			m_currentFilter = FT_NULL_FILTER;
		return result;
	}
	m_currentFilter = FT_NULL_FILTER;
	return FALSE;
}

// W3DShaderManager::filterPostRender =======================================================
/** Call to view filter shaders after rendering is complete.
 */
//=============================================================================
Bool W3DShaderManager::filterPostRender(FilterTypes filter, FilterModes mode, Coord2D &scrollDelta, Bool &doExtraRender)
{
	if (W3DFilters[filter])
	{
		const Bool result = W3DFilters[filter]->postRender(
			mode, scrollDelta, doExtraRender);
		// A filter owns the temporary target only for its post pass.  Clear the
		// active-filter latch even when the pass rejects the target or draw so a
		// later RTT setup cannot inherit motion-blur/crossfade clear semantics.
		m_currentFilter = FT_NULL_FILTER;
		return result;
	}

	m_currentFilter = FT_NULL_FILTER;
	return FALSE;
}

// W3DShaderManager::filterPostRender =======================================================
/** Call to view filter shaders after rendering is complete.
 */
//=============================================================================
	static Bool filterSetup(FilterTypes filter, FilterModes mode);
Bool W3DShaderManager::filterSetup(FilterTypes filter, FilterModes mode)
{
	if (W3DFilters[filter])
		return W3DFilters[filter]->setup(mode);
	return FALSE;
}

/*Draws 2 triangles covering the viewport given the current render states*/
void W3DShaderManager::drawViewport(Int color)
{
	struct _TRANS_LIT_TEX_VERTEX {
		Vector4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = Vector4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = Vector4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = Vector4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = Vector4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = color;
	v[1].color = color;
	v[2].color = color;
	v[3].color = color;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::SetGameVertexShader(GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1);

	(void)DrawGameTriangleStrip(v, sizeof(_TRANS_LIT_TEX_VERTEX),
		GAME_VERTEX_XYZRHW | GAME_VERTEX_DIFFUSE | GAME_VERTEX_TEX1);
}

// W3DShaderManager::startRenderToTexture =======================================================
/** Starts rendering to a texture.
 */
//=============================================================================
void W3DShaderManager::startRenderToTexture()
{
	DEBUG_ASSERTCRASH(!m_renderingToTexture, ("Already rendering to texture - cannot nest calls."));

	if (m_renderingToTexture || !canRenderToTexture()) return;
	rts::render::SetGameRenderTarget(m_renderTexture, nullptr, true);

	// A rejected target is latched off for this device lifetime.  Restore the
	// default target before releasing the publication so an optional filter can
	// never leave the frame pointed at an invalid resource.
	if (!rts::render::IsGameRenderingToTexture())
	{
		rts::render::SetGameRenderTarget(nullptr, nullptr, true);
		m_renderingToTexture = false;
		m_renderTargetAvailable = false;
		REF_PTR_RELEASE(m_renderTexture);
		return;
	}

	m_renderingToTexture = true;
	if (TheGlobalData->m_showSoftWaterEdge)
	{	//Soft water edges use frame buffer destination alpha so we must clear it to a known value.
		if (m_currentFilter == FT_VIEW_MOTION_BLUR_FILTER || m_currentFilter == FT_VIEW_CROSSFADE)
		{	//these filters rely on the previous frame being visible so we must be careful about clearing
			//frame buffer.  Only clear the alpha channel
			rts::render::SetGameRenderState(GAME_RENDER_STATE_COLOR_WRITE_MASK,GAME_COLOR_WRITE_ALPHA);	//only clear alpha
			ShaderClass shader=ShaderClass::_PresetOpaqueSolidShader;
			shader.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);
			shader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
			rts::render::SetGameShader(shader);

			VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
			rts::render::SetGameMaterial(vmat);
			REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.

			drawViewport(0x00ffffff | (((Int)(TheWaterTransparency->m_minWaterOpacity*255.0f)) <<24));
			rts::render::SetGameRenderState(GAME_RENDER_STATE_COLOR_WRITE_MASK,GAME_COLOR_WRITE_RED|GAME_COLOR_WRITE_GREEN|GAME_COLOR_WRITE_BLUE);	//disable writes to alpha
		}
		else	//normal clear that overwrites everything.
		{
			rts::render::GameRenderColor clearColor;
			clearColor.red = 0.0f;
			clearColor.green = 0.0f;
			clearColor.blue = 0.0f;
			clearColor.alpha = TheWaterTransparency->m_minWaterOpacity;
			if (rts::render::ClearGameRenderTargets(
				true, false, clearColor, clearColor.alpha) !=
				rts::render::RENDER_RESULT_OK)
			{
				rts::render::SetGameRenderTarget(nullptr, nullptr, true);
				m_renderingToTexture = false;
				m_renderTargetAvailable = false;
				REF_PTR_RELEASE(m_renderTexture);
			}
		}
	}
}

// W3DShaderManager::startRenderToTexture =======================================================
/** Ends rendering to a texture.
 */
//=============================================================================
TextureClass *W3DShaderManager::endRenderToTexture()
{
	DEBUG_ASSERTCRASH(m_renderingToTexture, ("Not rendering to texture."));
	if (!m_renderingToTexture) return nullptr;
	TextureClass *texture = m_renderTexture;
	rts::render::SetGameRenderTarget(nullptr, nullptr, true);	//restore original render target
	if (!rts::render::IsGameRenderingToTexture())
	{
		//assume render target texture will be in stage 0.  Most hardware has "conditional" support for
		//non-power-of-2 textures so we must force some required states:
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_W, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_NONE);

		m_renderingToTexture = false;
		return texture;
	}

	// Restoring the default target failed.  Do not return a texture that may
	// still be bound as an active target; disable this optional path until the
	// next renderer initialization.
	m_renderingToTexture = false;
	m_renderTargetAvailable = false;
	REF_PTR_RELEASE(m_renderTexture);
	return nullptr;
}

/**Returns texture containing the image that was last rendered using any of the effects requiring render target
textures.  Used mostly for cross-fading effects that need an unmodified version of the view before the effect
was applied.  NOTE: This texture does not survive device reset.. so quit effect on reset!*/
TextureClass *W3DShaderManager::getRenderTexture()
{
	return m_renderTexture;
}

enum GraphicsVenderID CPP_11(: Int)
{
	DC_NVIDIA_VENDOR_ID	= 0x10DE,
	DC_3DFX_VENDOR_ID	= 0x121A,
	DC_ATI_VENDOR_ID	= 0x1002
};

// W3DShaderManager::ChipsetType =======================================================
/** Returns the chipset used by the currently active rendering device.  Can be useful
	for coding around specific driver bugs.
 */
//=============================================================================
ChipsetType W3DShaderManager::getChipset()
{
	//check if globaldata has an override for current chipset
	if (TheGlobalData && TheGlobalData->m_chipSetType != DC_UNKNOWN)
		return (ChipsetType)TheGlobalData->m_chipSetType;

	if (!rts::render::IsGameRendererInitialized() ||
		!rts::render::IsGameRenderTargetOperational())
		return DC_UNKNOWN;

	// The native owner exposes capability level rather than adapter-specific
	// identifiers.  The shader manager only needs the historical tier to
	// select a visual path, so classify from the shared texture/stage and Dot3
	// capabilities without importing backend objects.
	const unsigned int maxTextures = rts::render::GetGameMaxTexturesPerPass();
	if (maxTextures >= 8)
		return DC_GENERIC_PIXEL_SHADER_2_0;
	if (maxTextures >= 4 && rts::render::GameRendererSupportsDot3())
		return DC_GENERIC_PIXEL_SHADER_1_4;
	if (maxTextures >= 4)
		return DC_GENERIC_PIXEL_SHADER_1_1;
	return DC_UNKNOWN;
}

//For the MP test, we're enforcing high min-spec requirements that need to be verified.
#define MIN_INTEL_CPU_FREQ	1300
#define MIN_AMD_CPU_FREQ	1100
#define MIN_ACCEPTED_FREQUENCY	1300
#define MIN_ACCEPTED_MEMORY	(1024*1024*256)	//256 MB
#define MIN_ACCEPTED_TEXTURE_MEMORY	(1024*1024*30)	//30 MB

/**Hack to give gameengine access to this function*/
Bool testMinimumRequirements(ChipsetType *videoChipType, CpuType *cpuType, Int *cpuFreq, MemValueType *numRAM, Real *intBenchIndex, Real *floatBenchIndex, Real *memBenchIndex)
{
	return W3DShaderManager::testMinimumRequirements(videoChipType,cpuType,cpuFreq,numRAM,intBenchIndex,floatBenchIndex,memBenchIndex);
}

Bool W3DShaderManager::testMinimumRequirements(ChipsetType *videoChipType, CpuType *cpuType, Int *cpuFreq, MemValueType *numRAM, Real *intBenchIndex, Real *floatBenchIndex, Real *memBenchIndex)
{
	if (videoChipType)
		*videoChipType = getChipset();

	if (cpuType)
	{
		*cpuType = XX;	//unknown

		//Check if it's an Athlon
		if (CPUDetectClass::Get_Processor_Manufacturer() == CPUDetectClass::MANUFACTURER_AMD &&
				CPUDetectClass::Get_AMD_Processor() >= CPUDetectClass::AMD_PROCESSOR_ATHLON_025)
				*cpuType = K7;

		//Check if it's a P3
		if (CPUDetectClass::Get_Processor_Manufacturer() == CPUDetectClass::MANUFACTURER_INTEL &&
				CPUDetectClass::Get_Intel_Processor() >= CPUDetectClass::INTEL_PROCESSOR_PENTIUM_III_MODEL_7)
				*cpuType = P3;
		//Check if it's a P4
		if (CPUDetectClass::Get_Processor_Manufacturer() == CPUDetectClass::MANUFACTURER_INTEL &&
				CPUDetectClass::Get_Intel_Processor() >= CPUDetectClass::INTEL_PROCESSOR_PENTIUM4)
				*cpuType = P4;
	}

	if (cpuFreq)
		*cpuFreq=CPUDetectClass::Get_Processor_Speed();

	if (numRAM)
		*numRAM=CPUDetectClass::Get_Total_Physical_Memory();

	if (intBenchIndex && floatBenchIndex && memBenchIndex)
	{
		// TheSuperHackers @tweak Aliendroid1 19/06/2025 Legacy benchmarking code was removed.
		// Since modern hardware always meets the minimum requirements, we preset the benchmark "results" to a high value.
		*intBenchIndex = 10.0f;
		*floatBenchIndex = 10.0f;
		*memBenchIndex = 10.0f;
	}

	return TRUE;
}

/**Try to guess how well the video card will handle the game assuming very fast CPU*/
StaticGameLODLevel W3DShaderManager::getGPUPerformanceIndex()
{
	ChipsetType	chipType;
	StaticGameLODLevel detailSetting=STATIC_GAME_LOD_LOW;	//assume lowest settings for now.

	if ((chipType=getChipset()) != DC_UNKNOWN)
	{	//a known video card so we can make some assumptions
		if (chipType >=	DC_GEFORCE2)
			detailSetting=STATIC_GAME_LOD_LOW;	//these cards need multiple terrain passes.
		if (chipType >= DC_GENERIC_PIXEL_SHADER_1_1)	//these cards can do terrain in single pass.
			detailSetting=STATIC_GAME_LOD_VERY_HIGH;
	}

	return detailSetting;
}

/**We need a hardware independent method to compare different CPU's.  For lack of anything better, we'll
use time to calculate PIE using a slow random number algorithm.*/

/**Used to test function call overhead*/
void add(float *sum,float *addend)
{
	*sum = *sum + *addend;
}

/**Returns seconds needed to run the test*/
Real W3DShaderManager::GetCPUBenchTime()
{
	float ztot, yran, ymult, ymod, x, y, z, pi, prod;
    long int low, ixran, itot, j, iprod;

  	__int64 endTime64,freq64,startTime64;
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
	QueryPerformanceCounter((LARGE_INTEGER *)&startTime64);

    ztot = 0.0;
    low = 1;
    ixran = 1907;
    yran = 5813.0;
    ymult = 1307.0;
    ymod = 5471.0;
    itot = 560000;	//total iterations. This value ends up running at ~30 fps on our P4-2.2Ghz.

    for(j=1; j<=itot; j++)
    {
		iprod = 27611 * ixran;
		ixran = iprod - 74383*(long int)(iprod/74383);
		x = (float)ixran / 74383.0;
		prod = ymult * yran;
		yran = (prod - ymod*(long int)(prod/ymod));
		y = yran / ymod;
		z = x*x + y*y;
		add(&ztot,&z);
		if ( z <= 1.0 )
		{
		  low = low + 1;
		}
	}
	pi = 4.0 * (float)low/(float)itot;

	QueryPerformanceCounter((LARGE_INTEGER *)&endTime64);
	return ((double)(endTime64-startTime64)/(double)(freq64));
}


// W3DShaderManager::setShroudTex =======================================================
/** Puts the shroud texture into a texture stage.
 */
//=============================================================================
Int W3DShaderManager::setShroudTex(Int stage)
{
	//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
	//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
	W3DShroud *shroud;
	if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr)
	{
		rts::render::SetGameTexture(stage, shroud->getShroudTexture());

		rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_2 );

RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		RenderMatrix4x4 scale,offset;

		//We need to make all world coordinates be relative to the heightmap data origin since that
		//is where the shroud begins.

		float xoffset = 0;
		float yoffset = 0;
		Real width=shroud->getCellWidth();
		Real height=shroud->getCellHeight();

		if (TheTerrainRenderObject->getMap())
		{	//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
		}

		RenderMatrixTranslation(&offset, xoffset, yoffset,0);

		width = 1.0f/(width*shroud->getTextureWidth());
		height = 1.0f/(height*shroud->getTextureHeight());
		RenderMatrixScaling(&scale, width, height, 1);
		RenderMatrixMultiply(&curView, &inv, &offset);
		RenderMatrixMultiply(&curView, &curView, &scale);
		rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+stage), &curView);
		return TRUE;
	}
	return FALSE;
}



Int FlatTerrainShader2Stage::init()
{
	//no special device validation needed - anything in our min spec should handle this.

	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=1;
	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=2;
	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=2;
	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=2;

	return TRUE;
}

void FlatTerrainShader2Stage::reset()
{
	ShaderClass::Invalidate();

	//Free references to textures
	rts::render::SetGameTexture(0, nullptr);
	rts::render::SetGameTexture(1, nullptr);

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);
}


Int FlatTerrainShader2Stage::set(Int pass)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	rts::render::ApplyGameRenderStateChanges();

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	switch (pass)
	{
		case 0:

			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
			if (W3DShaderManager::getShaderTexture(0)) {
				rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(0));
				rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
				rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
				rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
				rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

				rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
				rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

				//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
				//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
				W3DShroud *shroud;
				if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr)
				{
RenderMatrix4x4 curView;
	rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
					float det;
					RenderMatrixInverse(&inv, &det, &curView);

					RenderMatrix4x4 scale,offset;

					//We need to make all world coordinates be relative to the heightmap data origin since that
					//is where the shroud begins.

					float xoffset = 0;
					float yoffset = 0;
					Real width=shroud->getCellWidth();
					Real height=shroud->getCellHeight();

					if (TheTerrainRenderObject->getMap())
					{	//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
						xoffset = -(float)shroud->getDrawOriginX() + width;
						yoffset = -(float)shroud->getDrawOriginY() + height;
					}

					RenderMatrixTranslation(&offset, xoffset, yoffset,0);

					width = 1.0f/(width*shroud->getTextureWidth());
					height = 1.0f/(height*shroud->getTextureHeight());
					RenderMatrixScaling(&scale, width, height, 1);
					RenderMatrixMultiply(&curView, &inv, &offset);
					RenderMatrixMultiply(&curView, &curView, &scale);
					rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);
				}
			}	else {
				rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_2 );
				rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
			}
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,false);
			break;
		case 1:
			// Noise/cloud pass
RenderMatrix4x4 curView;
			rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

			//these states apply to all noise/cloud combination passes
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
			// Two output coordinates are used.
			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
			rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

			//blend into frame buffer
			rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_DESTINATION_COLOR);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_ZERO);

RenderMatrix4x4 inv;
			float det;
			RenderMatrixInverse(&inv, &det, &curView);

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12)
			{
				//setup cloud pass

				terrainShader2Stage.updateNoise1(&curView,&inv);	//update curView with texture matrix
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);
				//clouds always need bilinear filtering
				rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(2));

				//setup noise pass

				terrainShader2Stage.updateNoise2(&curView,&inv);
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, &curView);
				//noise always needs point/linear filtering.  Why point!?
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
				// Two output coordinates are used.
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
				rts::render::SetGameTexture(1, W3DShaderManager::getShaderTexture(3));
			}
			else
			{	//only 1 noise or cloud texture
				// Now setup the texture pipeline.
				if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1)
				{	//setup cloud pass
					rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(2));
					terrainShader2Stage.updateNoise1(&curView,&inv);	//update curView with texture matrix
					rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
					rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				}
				else
				{
					//setup noise pass
					rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(3));
					terrainShader2Stage.updateNoise2(&curView,&inv);	//update curView with texture matrix
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
					rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
				}

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, &curView);
			}
			break;
	}

	return TRUE;
}






Int FlatTerrainShaderPixelShader::shutdown()
{
	if (m_dwBasePixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBasePixelShader);

	if (m_dwBase0PixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBase0PixelShader);

	if (m_dwBaseNoise1PixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBaseNoise1PixelShader);

	if (m_dwBaseNoise2PixelShader)
		(void)rts::render::DeleteGameShader(false, m_dwBaseNoise2PixelShader);

	m_dwBasePixelShader=0;
	m_dwBase0PixelShader=0;
	m_dwBaseNoise1PixelShader=0;
	m_dwBaseNoise2PixelShader=0;

	return TRUE;
}

Int FlatTerrainShaderPixelShader::init()
{
	Int res;

#ifdef DISABLE_PIXEL_SHADERS
	return false;
#endif

	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if ((res=W3DShaderManager::getChipset()) >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//base version which doesn't apply any noise textures.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\fterrain.pso", false, nullptr, 0, 0,
				&m_dwBasePixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			//base version which doesn't apply any shroud textures.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\fterrain0.pso", false, nullptr, 0, 0,
				&m_dwBase0PixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			//version which blends 1 noise texture.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\fterrainnoise.pso", false, nullptr, 0, 0,
				&m_dwBaseNoise1PixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			//version which blends 2 noise textures.
			if (rts::render::CreateGameShaderFromAsset(
				"shaders\\fterrainnoise2.pso", false, nullptr, 0, 0,
				&m_dwBaseNoise2PixelShader) != rts::render::RENDER_RESULT_OK)
			{
				shutdown();
				return FALSE;
			}

			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=&flatTerrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=&flatTerrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=&flatTerrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=&flatTerrainShaderPixelShader;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=1;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=1;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=1;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=1;
			return TRUE;
		}
	}
	return FALSE;
}

Int FlatTerrainShaderPixelShader::set(Int pass)
{
	//setup base pass
	Int curStage = 1;
	// setup terrain [3/31/2003]

	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTexture(0, W3DShaderManager::getShaderTexture(2));
	rts::render::SetGameTexture(1, W3DShaderManager::getShaderTexture(2));
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	rts::render::ApplyGameRenderStateChanges();




	rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	//tell pixel shader which UV set to use for each stage
	rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
	rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	curStage = 0;

	W3DShroud *shroud = TheTerrainRenderObject->getShroud();
	if (shroud) {

		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

		//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
		//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
		{
RenderMatrix4x4 curView;
			rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
			float det;
			RenderMatrixInverse(&inv, &det, &curView);

			RenderMatrix4x4 scale,offset;

			//We need to make all world coordinates be relative to the heightmap data origin since that
			//is where the shroud begins.

			float xoffset = 0;
			float yoffset = 0;
			Real width=shroud->getCellWidth();
			Real height=shroud->getCellHeight();

			if (TheTerrainRenderObject->getMap())
			{	//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
				xoffset = -(float)shroud->getDrawOriginX() + width;
				yoffset = -(float)shroud->getDrawOriginY() + height;
			}

			RenderMatrixTranslation(&offset, xoffset, yoffset,0);

			width = 1.0f/(width*shroud->getTextureWidth());
			height = 1.0f/(height*shroud->getTextureHeight());
			RenderMatrixScaling(&scale, width, height, 1);
			RenderMatrixMultiply(&curView, &inv, &offset);
			RenderMatrixMultiply(&curView, &curView, &scale);
		rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+curStage), &curView);
		}
		rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState( curStage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTexture(curStage, shroud->getShroudTexture());
		curStage++;
		if (curStage==1) curStage++;
	}

	Bool doNoise1 = (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1 ||
						W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12);
	if (doNoise1) {	 // Cloud pass.
RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		// Two output coordinates are used.
		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTexture(curStage, W3DShaderManager::getShaderTexture(2));
		terrainShader2Stage.updateNoise1(&curView,&inv);	//update curView with texture matrix
		rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+curStage), &curView);
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

		curStage++;
		if (curStage==1) curStage++;
	}

	Bool doNoise2 = (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2 ||
						W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12);
	if (doNoise2)
	{
RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

		RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);

		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		// Two output coordinates are used.
		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(curStage,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTexture(curStage, W3DShaderManager::getShaderTexture(3));
		terrainShader2Stage.updateNoise2(&curView,&inv);	//update curView with texture matrix
		rts::render::SetGameTransform(static_cast<rts::render::GameRenderTransformSlot>(GAME_TRANSFORM_TEXTURE0+curStage), &curView);
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(curStage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

		curStage++;
		if (curStage==1) curStage++;
	}
	if (curStage<2) {
		rts::render::SetGamePixelShader(m_dwBase0PixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE0);
	}	else if (curStage==2) {
		rts::render::SetGamePixelShader(m_dwBasePixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE);
	}	else if (curStage==3) {
		rts::render::SetGamePixelShader(m_dwBaseNoise1PixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE);
	}else if (curStage==4) {
		rts::render::SetGamePixelShader(m_dwBaseNoise2PixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2);
	}
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE, false);
	rts::render::ApplyGameRenderStateChanges();
	rts::render::SetGameTexture(curStage, W3DShaderManager::getShaderTexture(3));
	return TRUE;
}

void FlatTerrainShaderPixelShader::reset()
{
	rts::render::SetGameTexture(2,nullptr);	//release reference to any texture
	rts::render::SetGameTexture(3,nullptr);	//release reference to any texture
	rts::render::SetGameTexture(4,nullptr);	//curStage can advance past the four shader inputs

	rts::render::SetGamePixelShader(0);	//turn off pixel shader
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);

	rts::render::SetGameTexture(0, nullptr);
	rts::render::SetGameTexture(1, nullptr);

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);

	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|2);

	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 3, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|3);
	rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 4, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_PASSTHROUGH|4);


	rts::render::InvalidateGameRenderStateCache();
}





