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

// FILE: TerrainTex.cpp ////////////////////////////////////////////////
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
// File name: TerrainTex.cpp
//
// Created:   John Ahlquist, April 2001
//
// Desc:      TextureClass overrides to perform custom texturing for the terrain.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------
#include <stdlib.h>

#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/TileData.h"
#include "W3DDevice/GameClient/RenderTextureOperations.h"
#include "Common/GlobalData.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderTexturePublication.h"

// Keep the source-level contract explicit without importing the renderer namespace.
using rts::render::GAME_RENDER_STATE_ALPHA_BLEND_ENABLE;
using rts::render::GAME_RENDER_STATE_DESTINATION_BLEND;
using rts::render::GAME_RENDER_STATE_SOURCE_BLEND;
using rts::render::GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE;
using rts::render::GAME_TEXTURE_ARGUMENT_COMPLEMENT;
using rts::render::GAME_TEXTURE_ARGUMENT_CURRENT;
using rts::render::GAME_TEXTURE_ARGUMENT_DIFFUSE;
using rts::render::GAME_TEXTURE_ARGUMENT_FACTOR;
using rts::render::GAME_TEXTURE_ARGUMENT_TEXTURE;
using rts::render::GAME_TEXTURE_COORDINATE_CAMERA_POSITION;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_U;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_V;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_OPERATION;
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
using rts::render::GAME_TRANSFORM_VIEW;
using rts::render::RENDER_BLEND_DESTINATION_COLOR;
using rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
using rts::render::RENDER_BLEND_ONE;
using rts::render::RENDER_BLEND_SOURCE_ALPHA;
using rts::render::RENDER_BLEND_ZERO;
using rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
using rts::render::RENDER_TEXTURE_ADDRESS_WRAP;
using rts::render::RENDER_TEXTURE_FILTER_LINEAR;
using rts::render::RENDER_TEXTURE_FILTER_POINT;
using rts::render::RENDER_TEXTURE_OP_ADD;
using rts::render::RENDER_TEXTURE_OP_DISABLE;
using rts::render::RENDER_TEXTURE_OP_MODULATE;
using rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
using rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;

#include "Renderer/RenderMatrixMath.h"
#include "WW3D2/surfaceclass.h"

namespace
{
class ProceduralTextureSurfaceLock
{
public:
	ProceduralTextureSurfaceLock() : Bits(nullptr), Pitch(0), Width(0),
		Height(0), Format(WW3D_FORMAT_UNKNOWN), m_locked(false),
		m_surface(nullptr) {}
	~ProceduralTextureSurfaceLock() { Close(); }

	bool Open(TextureClass *texture)
	{
		if (texture == nullptr || m_surface != nullptr) return false;
		m_surface = texture->Get_Surface_Level(0);
		if (m_surface == nullptr) return false;
		SurfaceClass::SurfaceDescription description;
		m_surface->Get_Description(description);
		Width = description.Width;
		Height = description.Height;
		Format = description.Format;
		Bits = static_cast<UnsignedByte *>(m_surface->Lock(&Pitch));
		m_locked = Bits != nullptr && Pitch > 0 && Width > 0 && Height > 0;
		return m_locked;
	}

	bool Finish(TextureClass *texture)
	{
		if (!m_locked || m_surface == nullptr || texture == nullptr) return false;
		m_surface->Unlock();
		m_locked = false;
		m_surface->Release_Ref();
		m_surface = nullptr;
		Bits = nullptr;
		return Generate_Render_Texture_Mip_Levels(texture);
	}

	UnsignedByte *Bits;
	Int Pitch;
	UnsignedInt Width;
	UnsignedInt Height;
	WW3DFormat Format;

private:
	void Close()
	{
		if (m_surface == nullptr) return;
		if (m_locked) m_surface->Unlock();
		m_surface->Release_Ref();
		m_surface = nullptr;
		m_locked = false;
		Bits = nullptr;
	}

	bool m_locked;
	SurfaceClass *m_surface;
};
}

/******************************************************************************
						TerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 16 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
TerrainTextureClass::TerrainTextureClass(int height) :
	TextureClass(TEXTURE_WIDTH, height,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_3 )
{
}

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 16 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
TerrainTextureClass::TerrainTextureClass(int height, int width) :
	TextureClass(width, height,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_ALL )
{
}


//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
int TerrainTextureClass::update(WorldHeightMap *htMap)
{
	ProceduralTextureSurfaceLock surface;
	if (!surface.Open(this)) return 0;
	if (surface.Width < TEXTURE_WIDTH) {
		return 0;
	}

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
	Int tilesPerRow = surface.Width/(2*TILE_PIXEL_EXTENT+TILE_OFFSET);
	tilesPerRow *= 2;
//	Int numRows = surface_desc.Height/(tilePixelExtent+TILE_OFFSET);
#ifdef RTS_DEBUG
	//DEBUG_ASSERTCRASH(tilesPerRow*numRows >= htMap->m_numBitmapTiles, ("Too many tiles."));
	DEBUG_ASSERTCRASH((Int)surface.Width >= tilePixelExtent*tilesPerRow, ("Bitmap too small."));
#endif
	if (surface.Format == WW3D_FORMAT_A1R5G5B5 ||
		surface.Format == WW3D_FORMAT_A8R8G8B8) {
#if 0
		UnsignedInt cellX, cellY;
		for (cellX = 0; cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*2;
				*((Short*)pBGR) = (((255-2*cellY)>>3)<<10) + ((4*cellX)>>4);
			}
		}
#endif
		Int tileNdx;
		const bool canonicalNative =
			surface.Format == WW3D_FORMAT_A8R8G8B8;
		Int pixelBytes = canonicalNative ? 4 : 2;
		for (tileNdx=0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
			TileData *pTile = htMap->getSourceTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real tile offsets start at 2.  jba.

			Int i,j;
			for (j=0; j<tilePixelExtent; j++) {
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				Int row = position.y+j;
				UnsignedByte *pBGRX = surface.Bits + row * surface.Pitch;

				Int column = position.x;
				pBGRX += column*pixelBytes;
				for (i=0; i<tilePixelExtent; i++) {
					if (canonicalNative)
						*((UnsignedInt*)pBGRX) = 0xff000000 |
							(pBGR[2] << 16) | (pBGR[1] << 8) | pBGR[0];
					else
						*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) +
							((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
					pBGRX +=pixelBytes;
					pBGR +=TILE_BYTES_PER_PIXEL;
				}
			}
		}
		// Now draw the 4 pixel border around each tile class.
		Int texClass;
		for (texClass=0; texClass<htMap->m_numTextureClasses; texClass++) {
			Int width = htMap->m_textureClasses[texClass].width;
			ICoord2D origin = htMap->m_textureClasses[texClass].positionInTexture;
			if (origin.x<=0) continue;
			width *= TILE_PIXEL_EXTENT;
			// Duplicate 4 columns of pixels before and after.
			Int j;
			for (j=0; j<width; j++) {
				Int row = origin.y+j;
				UnsignedByte *pBGRX = surface.Bits + row * surface.Pitch;

				Int column = origin.x;
				pBGRX += column*pixelBytes;
				// copy before
				memcpy(pBGRX-(4)*pixelBytes, pBGRX+(width-4)*pixelBytes, 4*pixelBytes);
				// copy after
				memcpy(pBGRX+(width*pixelBytes), pBGRX, 4*pixelBytes);
			}

			// Duplicate 4 rows of pixels before and after.
			for (j=0; j<4; j++) {
				// copy before.
				Int row = origin.y-j-1;
				UnsignedByte *pBGRX = surface.Bits + row * surface.Pitch;
				UnsignedByte *target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target, target+width*surface.Pitch, (width+8)*pixelBytes);
				// copy after.
				row = origin.y+j;
				pBGRX = surface.Bits + row * surface.Pitch;
				target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target+width*surface.Pitch, target, (width+8)*pixelBytes);
			}

		}

	}
	if (!surface.Finish(this)) return 0;
	rts::render::NotifyTextureChanged(this);
	return(surface.Height);
}

//=============================================================================
// TerrainTextureClass::setLOD
//=============================================================================
/** Sets the lod of the texture to be loaded into the video card.  */
//=============================================================================
void TerrainTextureClass::setLOD(Int LOD)
{
	Set_Render_Texture_LOD(this, LOD);
}
//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
Bool TerrainTextureClass::updateFlat(WorldHeightMap *htMap, Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell)
{
	ProceduralTextureSurfaceLock surface;
	if (!surface.Open(this)) return false;
	DEBUG_ASSERTCRASH((Int)surface.Width == cellWidth*pixelsPerCell, ("Bitmap too small."));
	DEBUG_ASSERTCRASH((Int)surface.Height == cellWidth*pixelsPerCell, ("Bitmap too small."));
	if (surface.Width != static_cast<UnsignedInt>(cellWidth*pixelsPerCell)) {
		return false;
	}

	if (surface.Format == WW3D_FORMAT_A1R5G5B5 ||
		surface.Format == WW3D_FORMAT_A8R8G8B8) {

		const bool canonicalNative =
			surface.Format == WW3D_FORMAT_A8R8G8B8;
		Int pixelBytes = canonicalNative ? 4 : 2;
		Int cellX, cellY;
#if 0
		UnsignedInt X, Y;
		for (X = 0; X < surface_desc.Width; X++) {
			for (Y = 0; Y < surface_desc.Height; Y++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(Y*surface_desc.Width+X)*pixelBytes;
				*((Short*)pBGR) = (((255-2*Y)>>3)<<10) + ((2*X)>>4);
			}
		}
#endif
		for (cellX = 0; cellX < cellWidth; cellX++) {
			for (cellY = 0; cellY < cellWidth; cellY++) {
				UnsignedByte *pBGRX_data = surface.Bits;
				UnsignedByte *pBGR = htMap->getPointerToTileData(xCell+cellX, yCell+cellY, pixelsPerCell);
				if (pBGR == nullptr) continue; // past end of defined terrain. [3/24/2003]
				Int k, l;
				for (k=pixelsPerCell-1; k>=0; k--) {
					UnsignedByte *pBGRX = pBGRX_data +
						(pixelsPerCell*(cellWidth-cellY-1)+k)*surface.Pitch +
						cellX*pixelsPerCell*pixelBytes;
					for (l=0; l<pixelsPerCell; l++) {
						if (canonicalNative)
							*((UnsignedInt*)pBGRX) = 0xff000000 |
								(pBGR[2] << 16) | (pBGR[1] << 8) | pBGR[0];
						else
							*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) +
								((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
						pBGRX +=pixelBytes;
						pBGR +=TILE_BYTES_PER_PIXEL;
					}
				}
			}
		}
	}

	if (!surface.Finish(this)) return false;
	rts::render::NotifyTextureChanged(this);
	return(surface.Height);
}

//=============================================================================
// TerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup
(standard D3D setup, but beyond the scope of W3D).  */
//=============================================================================
void TerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	// Now setup the texture pipeline.
	if (stage==0) {
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,false);

	}
#endif
}

/******************************************************************************
						AlphaTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// AlphaTerrainTextureClass::AlphaTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to creat a throw away 8x8 texture,
then uses the base texture's D3D texture. This way the base tiles pass, drawn
using TerrainTextureClass shares the same texture with the blended edges pass,
saving lots of texture memory, and preventing seams between blended tiles. */
//=============================================================================
AlphaTerrainTextureClass::AlphaTerrainTextureClass( TextureClass *pBaseTex ):
	TextureClass(8, 8,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_1 )
#if defined(_WIN64)
	, m_baseTexture(pBaseTex)
#endif
{
	#if defined(_WIN64)
	Release_Native_Texture();
	if (m_baseTexture != nullptr) m_baseTexture->Add_Ref();
	#endif
	Bind_Render_Texture_Alias(this, pBaseTex);
}

AlphaTerrainTextureClass::~AlphaTerrainTextureClass()
{
	#if defined(_WIN64)
	REF_PTR_RELEASE(m_baseTexture);
	#endif
}


//=============================================================================
// AlphaTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
This may be applied in either single pass, as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the base texture in stage 0.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void AlphaTerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
#if defined(_WIN64)
	if (m_baseTexture != nullptr) m_baseTexture->Apply(stage);
	else TextureClass::Apply_Null(stage);
#else
	TextureClass::Apply(stage);
#endif

	// Set the bilinear or trilinear filtering.
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	// Since we are using multiple distinct tiles, the textures doesn't wrap, so clamp it.
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {
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
	}	else if (stage==1) {

		if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
		{
			///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
			//This method is a backdoor specific to Nvidia based cards.  It will fail on
			//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
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
		{
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );

			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
			rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
		}
	}
}


/******************************************************************************
						LightMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// LightMapTerrainTextureClass::LightMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
LightMapTerrainTextureClass::LightMapTerrainTextureClass(AsciiString name, MipCountType mipLevelCount) :
TextureClass(name.isEmpty()?"TSNoiseUrb.tga":name.str(),name.isEmpty()?"TSNoiseUrb.tga":name.str(), mipLevelCount )
{
	Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
}

#define STRETCH_FACTOR ((float)(1/(63.0*MAP_XY_FACTOR/2))) /* covers 63/2 tiles */

//=============================================================================
// LightMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The LightMapTerrainTextureClass may be applied by itself, or with the
CloudMapTerrainTextureClass.  This may be applied in either single pass,
as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the cloud map texture in stage 0.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void LightMapTerrainTextureClass::Apply(unsigned int stage)
{
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	// Do the base apply.
	/* previous setup */
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

	// Disable 3rd stage just in case.
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

	// Now setup the texture pipeline.
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
	if (stage == 0) {
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
		//Disable second stage
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	} else {
		rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	}
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	// Two output coordinates are used.
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);


	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState( stage, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

	RenderMatrix4x4 curView;
	rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, curView);

	RenderMatrix4x4 inv;
	float det;
	RenderMatrixInverse(&inv, &det, &curView);

	RenderMatrix4x4 scale;
	RenderMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	RenderMatrixMultiply(&inv, &inv, &scale);
	if (stage==0) {
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, inv);
	}	if (stage==1) {
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, inv);
	}


	if (stage==0) {
		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_DESTINATION_COLOR);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_ZERO);
	}
#endif
}









/******************************************************************************
						AlphaEdgeTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

/**
* AlphaEdgeTextureClass - Generates the alpha edge blending for terrain.
*
*/
AlphaEdgeTextureClass::AlphaEdgeTextureClass( int height, MipCountType mipLevelCount) :
//	TextureClass("EdgingTemplate.tga","EdgingTemplate.tga", mipLevelCount )
	TextureClass(TEXTURE_WIDTH, height, WW3D_FORMAT_A8R8G8B8, mipLevelCount )
{

}

int AlphaEdgeTextureClass::update256(WorldHeightMap *htMap)
{
	return 1;
}

int AlphaEdgeTextureClass::update(WorldHeightMap *htMap)
{
	ProceduralTextureSurfaceLock surface;
	if (!surface.Open(this)) return 0;

	Int tilePixelExtent = TILE_PIXEL_EXTENT; // blend tiles are 1/4 tiles.
//	Int tilesPerRow = surface_desc.Width / (tilePixelExtent+8);

//	Int numRows = surface_desc.Height/(tilePixelExtent+8);

	if (surface.Format == WW3D_FORMAT_A8R8G8B8) {
#if 1
#if 1
		Int cellX, cellY;
		for (cellX = 0; (UnsignedInt)cellX < surface.Width; cellX++) {
			for (cellY = 0; (UnsignedInt)cellY < surface.Height; cellY++) {
				UnsignedByte *pBGR = surface.Bits + cellY * surface.Pitch +
					cellX * 4;
				pBGR[2] = 255-cellY/2;
				pBGR[0] = cellX/2;
				pBGR[3] = cellX/2;  // alpha.
				pBGR[3] = 128;  // alpha.
			}
		}
#endif
#if 1
		Int tileNdx;
		Int pixelBytes = 4;
		for (tileNdx=0; tileNdx < htMap->m_numEdgeTiles; tileNdx++) {
			TileData *pTile = htMap->getEdgeTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real edge offsets start at 4.  jba.
			Int i,j;
			Int column = position.x;
			for (j=0; j<tilePixelExtent; j++) {
				Int row = position.y+j;
				UnsignedByte *pBGR = htMap->getEdgeTile(tileNdx)->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				UnsignedByte *pBGRX = surface.Bits + row * surface.Pitch;
				pBGRX += column*pixelBytes;

				for (i=0; i<tilePixelExtent; i++) {
					pBGRX[0] = pBGR[0];  //r
					pBGRX[1] = pBGR[1];	//g
					pBGRX[2] = pBGR[2];	//b
					if (pBGR[0]==0 && pBGR[1]==0 && pBGR[2]==0) {
						pBGRX[3] = 0x80;
					} else if (pBGR[0]==0xff && pBGR[1]==0xff && pBGR[2]==0xff) {
						pBGRX[3] = 0x00;
					}	else {
						pBGRX[3] = 0xff;
					}

					pBGRX += pixelBytes;
					pBGR += TILE_BYTES_PER_PIXEL;
				}
			}
		}
#endif
#endif
	}
	if (!surface.Finish(this)) return 0;
	rts::render::NotifyTextureChanged(this);
	return(surface.Height);
}

void AlphaEdgeTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {

		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1,   GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1 );
		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);

		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

	} else if (stage==1) {
		// Drawing texture through the mask.
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1,   GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );

		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1,   GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2,   GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_2 );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1 );
		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_ONE);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_ZERO);

	}
#endif
}


/******************************************************************************
						CloudMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// CloudMapTerrainTextureClass::CloudMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture, and sets
up the "sliding" parameters for the clouds to slide over the terrain. */
//=============================================================================
//@todo - Allow adjustment of the cloud slide rate, and lose the hard coded "cloudmap.tga"
CloudMapTerrainTextureClass::CloudMapTerrainTextureClass(MipCountType mipLevelCount) :
	TextureClass("TSCloudMed.tga","TSCloudMed.tga", mipLevelCount )
{
	Get_Filter().Set_Mip_Mapping( TextureFilterClass::FILTER_TYPE_FAST );
	m_xSlidePerSecond = -0.02f;
	m_ySlidePerSecond =  1.50f * m_xSlidePerSecond;
	m_curTick = 0;
	m_xOffset = 0;
	m_yOffset = 0;

}

//=============================================================================
// CloudMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The CloudMapTerrainTextureClass may be applied by itself, or with the
LightMapTerrainTexture.  This may be applied in either single pass,
as the first texture in the pipe with LightMapTerrainTextureClass as the
second stage of the pape, or multipass.  We setup for stage 0, assuming that
we are the only texture, as LightMapTerrainTexture will adjust for multitexture
if it is applied to stage 1.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void CloudMapTerrainTextureClass::Apply(unsigned int stage)
{


	// Do the base apply.
	TextureClass::Apply(stage);
#if 0   // obsolete
	/* previous setup */
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);

	// Now setup the texture pipeline.
	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	// Two output coordinates are used.
	rts::render::SetGameTextureStageState(stage,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);


	rts::render::SetGameTextureStageState( stage,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState( stage,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

	RenderMatrix4x4 curView;
	rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, curView);

	RenderMatrix4x4 inv;
	float det;
	RenderMatrixInverse(&inv, &det, &curView);

	RenderMatrix4x4 scale;
	RenderMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	RenderMatrixMultiply(&inv, &inv, &scale);
	RenderMatrix4x4 offset;

	Int delta = m_curTick;
	m_curTick = ::GetTickCount();
	delta = m_curTick-delta;
	m_xOffset += m_xSlidePerSecond*delta/1000;
	m_yOffset += m_ySlidePerSecond*delta/1000;

	if (m_xOffset > 1) m_xOffset -= 1;
	if (m_yOffset > 1) m_yOffset -= 1;
	if (m_xOffset < -1) m_xOffset += 1;
	if (m_yOffset < -1) m_yOffset += 1;


	RenderMatrixTranslation(&offset, m_xOffset, m_yOffset,0);

	RenderMatrixMultiply(&inv, &inv, &offset);

	if (stage==0) {
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
		rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE0, inv);

		// Disable 3rd stage just in case.
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		rts::render::SetGameTextureStageState( 2, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_DESTINATION_COLOR);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_ZERO);
	}	else if (stage==1) {
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_CURRENT );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );

		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, inv);
	}
#endif
}

//=============================================================================
// CloudMapTerrainTextureClass::restore
//=============================================================================
/** Cleans up any custom settings to the texturing pipeline that may not be
understood by w3d. */
//=============================================================================
void CloudMapTerrainTextureClass::restore()
{
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,false);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);


	if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
	{
		///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
		//This method is a backdoor specific to Nvidia based cards.  It will fail on
		//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
		Int i;
		for (i=0; i<8; i++) {
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_COORDINATE_INDEX, i);
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE);
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
			rts::render::SetGameTextureStageState( i, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);

			rts::render::SetGameTexture(i, nullptr);
		}
	}
}

/******************************************************************************
						ScorchTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// ScorchTextureClass::ScorchTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
/// @todo - get "EXScorch01.tga" from not hard coded location.
ScorchTextureClass::ScorchTextureClass(MipCountType mipLevelCount) :
	TextureClass("EXScorch01.tga","EXScorch01.tga", mipLevelCount )
// Hack to disable texture reduction.
//	TextureClass("EXScorch01.tga","EXScorch01.tga", mipLevelCount,WW3D_FORMAT_UNKNOWN,true,false)
{
}

//=============================================================================
// ScorchTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The ScorchTextureClass is applied by iteself, as it's mesh is a subset of the
terrain mesh.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void ScorchTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
	// Setup bilinear or trilinear filtering as specified in global data.
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	} else {
		rts::render::SetGameTextureStageState(stage, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	}

	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	// Now setup the texture pipeline.

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_SELECT_ARGUMENT_1 );
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0 );
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,true);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND,RENDER_BLEND_SOURCE_ALPHA);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND,RENDER_BLEND_INVERSE_SOURCE_ALPHA);

	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
	rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_DISABLE );
}


