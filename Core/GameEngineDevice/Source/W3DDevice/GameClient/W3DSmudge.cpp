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

// W3DSmudge.cpp ////////////////////////////////////////////////////////////////////////////////
// Smudge System implementation
// Author: Mark Wilczynski, June 2003
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "Lib/BaseType.h"
#include "WWLib/always.h"
#include "W3DDevice/GameClient/W3DSmudge.h"
#include "Common/GameMemory.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"
#include "WW3D2/texture.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/vertmaterial.h"
#include "Renderer/RenderGameClient.h"

// Keep the source-level contract explicit without importing the renderer namespace.
using rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_U;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_V;
using rts::render::GAME_TEXTURE_STAGE_ADDRESS_W;
using rts::render::GAME_TEXTURE_STAGE_ALPHA_OPERATION;
using rts::render::GAME_TEXTURE_STAGE_COLOR_OPERATION;
using rts::render::GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER;
using rts::render::GAME_TEXTURE_STAGE_MINIFICATION_FILTER;
using rts::render::GAME_TEXTURE_STAGE_MIP_FILTER;
using rts::render::GAME_TRANSFORM_VIEW;
using rts::render::GAME_TRANSFORM_WORLD;
using rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
using rts::render::RENDER_RESULT_OK;
using rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
using rts::render::RENDER_TEXTURE_FILTER_LINEAR;
using rts::render::RENDER_TEXTURE_FILTER_NONE;
using rts::render::RENDER_TEXTURE_OP_MODULATE;
using rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
using rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;

#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/sortingrenderer.h"


SmudgeManager *TheSmudgeManager=nullptr;

W3DSmudgeManager::W3DSmudgeManager()
{
}

W3DSmudgeManager::~W3DSmudgeManager()
{
	ReleaseResources();
}

void W3DSmudgeManager::init()
{
	SmudgeManager::init();
	ReAcquireResources();
}

void W3DSmudgeManager::reset ()
{
	SmudgeManager::reset();	//base
}

void W3DSmudgeManager::ReleaseResources()
{
	REF_PTR_RELEASE(m_backgroundTexture);
	REF_PTR_RELEASE(m_indexBuffer);
}


#define SMUDGE_DRAW_SIZE	500	//draw at most 50 smudges per call. Tweak value to improve CPU/GPU parallelism.

static_assert(SMUDGE_DRAW_SIZE * 5 < 0x10000, "Vertex index exceeds 16-bit limit");


void W3DSmudgeManager::ReAcquireResources()
{
	ReleaseResources();

	rts::render::RenderBackBufferInfo back_buffer_info;
	if (rts::render::GetGameBackBufferInfo(&back_buffer_info) !=
		rts::render::RENDER_RESULT_OK || back_buffer_info.width == 0 ||
		back_buffer_info.height == 0 ||
		back_buffer_info.format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM)
	{
		return;
	}

	SurfaceClass::SurfaceDescription surface_desc;
	surface_desc.Width = back_buffer_info.width;
	surface_desc.Height = back_buffer_info.height;
	surface_desc.Format = WW3D_FORMAT_A8R8G8B8;
	m_backgroundTexture = MSGNEW("TextureClass") TextureClass(surface_desc.Width,surface_desc.Height,surface_desc.Format,MIP_LEVELS_1,TextureClass::POOL_DEFAULT, true);

	m_backBufferWidth = surface_desc.Width;
	m_backBufferHeight = surface_desc.Height;

	m_indexBuffer=NEW_REF(DX8IndexBufferClass,(SMUDGE_DRAW_SIZE*4*3));	//allocate 4 triangles per smudge, each with 3 indices.

	// Fill up the IB with static vertex indices that will be used for all smudges.
	{
		DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexBuffer);
		UnsignedShort *ib=lockIdxBuffer.Get_Index_Array();
		//quad of 4 triangles:
		//	0-----3
		//  |\   /|
		//  |  4  |
		//	|/   \|
		//  1-----2
		Int vbCount=0;
		for (Int i=0; i<SMUDGE_DRAW_SIZE; i++)
		{
			//Top
			ib[0]=vbCount;
			ib[1]=vbCount+4;
			ib[2]=vbCount+3;
			//Right
			ib[3]=vbCount+3;
			ib[4]=vbCount+4;
			ib[5]=vbCount+2;
			//Bottom
			ib[6]=vbCount+2;
			ib[7]=vbCount+4;
			ib[8]=vbCount+1;
			//Left
			ib[9]=vbCount+1;
			ib[10]=vbCount+4;
			ib[11]=vbCount+0;

			vbCount += 5;
			ib+=12;
		}
	}
}

/*Copies a portion of the current render target into a specified buffer*/
#define UNIQUE_COLOR	(0x12345678)
#define BLOCK_SIZE	(8)

Bool W3DSmudgeManager::testHardwareSupport()
{
	if (m_hardwareSupportStatus == SMUDGE_SUPPORT_UNKNOWN)
	{
		m_hardwareSupportStatus = m_backgroundTexture != nullptr &&
			m_backgroundTexture->Is_Initialized() ?
			SMUDGE_SUPPORT_YES : SMUDGE_SUPPORT_NO;
	}
	return SMUDGE_SUPPORT_YES == m_hardwareSupportStatus;
}

void W3DSmudgeManager::render(RenderInfoClass &rinfo)
{
	//Verify that the card supports the effect.
	if (!testHardwareSupport())
		return;

	// Avoid back-buffer work when there are no smudges to render.
	if (m_usedSmudgeSetList.empty() ||
		(m_usedSmudgeSetList.size() == 1 &&
		 m_usedSmudgeSetList.front()->getUsedSmudgeCount() == 0))
	{
		m_smudgeCountLastFrame = 0;
		return;
	}

	rts::render::RenderBackBufferInfo back_buffer_info;
	if (rts::render::GetGameBackBufferInfo(&back_buffer_info) !=
		rts::render::RENDER_RESULT_OK || back_buffer_info.width == 0 ||
		back_buffer_info.height == 0 ||
		back_buffer_info.format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM)
	{
		return;
	}
	SurfaceClass::SurfaceDescription surface_desc;
	surface_desc.Width = back_buffer_info.width;
	surface_desc.Height = back_buffer_info.height;
	surface_desc.Format = WW3D_FORMAT_A8R8G8B8;
	CameraClass &camera=rinfo.Camera;
	Vector3 vsVert;
	Vector4 ssVert;
	Real uvSpanX,uvSpanY;
	Vector3 vertex_offsets[4] = {
		Vector3(-0.5f, 0.5f, 0.0f),
		Vector3(-0.5f, -0.5f, 0.0f),
		Vector3(0.5f, -0.5f, 0.0f),
		Vector3(0.5f, 0.5f, 0.0f)
	};

#define THE_COLOR (0x00ffeedd)

	UnsignedInt vertexDiffuse[5]={THE_COLOR,THE_COLOR,THE_COLOR,THE_COLOR,THE_COLOR};

	Matrix4x4 proj;
	Matrix3D view;

	camera.Get_View_Matrix(&view);
	camera.Get_Projection_Matrix(&proj);

	Real texClampX = (Real)TheTacticalView->getWidth()/(Real)surface_desc.Width;
	Real texClampY = (Real)TheTacticalView->getHeight()/(Real)surface_desc.Height;

	Real texScaleX = texClampX*0.5f;
	Real texScaleY = texClampY*0.5f;

	//Do a first pass over the smudges to determine how many are visible
	//and to fill in their world-space positions and screen uv coordinates.
	//TODO: Optimize out this extra pass!
	//TODO: Find size of screen rectangle that actually needs copying.

	SmudgeSetDeque::iterator setIt=m_usedSmudgeSetList.begin();	//first set that didn't fit into render batch.
	Int count = 0;

	// make sure background particles have finished drawing.
	SortingRendererClass::Flush();	//draw sorted translucent polys like particles.

	for(; setIt != m_usedSmudgeSetList.end(); ++setIt)
	{
		SmudgeSet* set=*setIt;
		SmudgeDeque::iterator smudgeIt=set->getUsedSmudgeList().begin();

		for (; smudgeIt != set->getUsedSmudgeList().end(); ++smudgeIt)
		{
			Smudge* smudge=*smudgeIt;
			if (!smudge->m_draw)
				continue;

			//Get view-space center
			Matrix3D::Transform_Vector(view,smudge->m_pos,&vsVert);

			//Get 5 view-space vertices
			Smudge::smudgeVertex *verts=smudge->m_verts;

			//Do center vertex outside 'for' loop since it's different.
			verts[4].pos = vsVert;

			Vector2 offset = smudge->m_offset;

			for (Int i=0; i<4; i++)
			{
				verts[i].pos = vsVert + vertex_offsets[i] * smudge->m_size;
				//Ge uv coordinates for each vertex
				ssVert = proj * verts[i].pos;
				Real oow = 1.0f/ssVert.W;
				ssVert *= oow;	//returned in camera space which is -1,-1 (bottom-left) to 1,1 (top-right)
				//convert camera space to uv space: 0,0 (top-left), 1,1 (bottom-right)
				verts[i].uv.Set((ssVert.X+1.0f)*texScaleX,(1.0f-ssVert.Y)*texScaleY);

				Vector2 &thisUV=verts[i].uv;

				// Zero coordinates that fall outside valid texel bounds
				if (thisUV.X < 0 || thisUV.X > texClampX)
					offset.X = 0;

				if (thisUV.Y < 0 || thisUV.Y > texClampY)
					offset.Y = 0;
			}

			//Finish center vertex
			//Ge uv coordinates by interpolating corner uv coordinates and applying desired offset.
			uvSpanX=verts[3].uv.X - verts[0].uv.X;
			uvSpanY=verts[1].uv.Y - verts[0].uv.Y;
			verts[4].uv.X=verts[0].uv.X+uvSpanX*(0.5f+offset.X);
			verts[4].uv.Y=verts[0].uv.Y+uvSpanY*(0.5f+offset.Y);

			count++;	//increment visible smudge count.
		}
	}

	m_smudgeCountLastFrame = count;

	if (!count)
	{
		return;	//nothing to render.
	}

	// Copy the visible color target into an alternate buffer through the
	// renderer contract. The backend owns synchronization and resource
	// transition; this call fails closed when the active target is unavailable.
	if (m_backgroundTexture == nullptr || !m_backgroundTexture->Is_Initialized() ||
		rts::render::CopyGameActiveTargetToTexture(m_backgroundTexture) !=
		rts::render::RENDER_RESULT_OK)
	{
		return;
	}


	Matrix4x4 identity(true);
	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,identity);
	rts::render::SetGameTransform(GAME_TRANSFORM_VIEW,identity);

	rts::render::SetGameIndexBuffer(m_indexBuffer,0);
	//rts::render::SetGameShader(ShaderClass::_PresetOpaqueSpriteShader);

	rts::render::SetGameShader(ShaderClass::_PresetAlphaShader);

	rts::render::SetGameTexture(0,m_backgroundTexture);
	//Need these states in case texture is non-power-of-2
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ADDRESS_W, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_NONE);
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);
	rts::render::ApplyGameRenderStateChanges();

	//Disable reading texture alpha since it's undefined.
	//rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_COLOR_OPERATION,RENDER_TEXTURE_OP_SELECT_ARGUMENT_1);
	rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_ALPHA_OPERATION,RENDER_TEXTURE_OP_SELECT_ARGUMENT_2);

	Int smudgesRemaining=count;
	setIt=m_usedSmudgeSetList.begin();	//first smudge set that needs rendering.
	SmudgeDeque::iterator smudgeIt = (*setIt)->getUsedSmudgeList().begin();	//first smudge that needs rendering.

	while (smudgesRemaining)	//keep drawing smudges until we run out.
	{
		//Now that we know how many smudges need rendering, allocate vertex buffer space and copy verts.
		count=smudgesRemaining;

		if (count > SMUDGE_DRAW_SIZE)
			count = SMUDGE_DRAW_SIZE;

		Int smudgesInRenderBatch=0;

		DynamicVBAccessClass vb_access(GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,dynamic_fvf_type,count*5);	//allocate 5 verts per smudge.
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();

			while (setIt != m_usedSmudgeSetList.end())
			{
				SmudgeDeque& smudgeList = (*setIt)->getUsedSmudgeList();

				for(; smudgeIt != smudgeList.end(); ++smudgeIt)
				{
					Smudge* smudge = *smudgeIt;
					if (!smudge->m_draw)
					{
						continue;
					}

					Smudge::smudgeVertex *smVerts = smudge->m_verts;

					//Check if we exceeded maximum number of smudges allowed per draw call.
					if (smudgesInRenderBatch >= count)
					{
						goto flushSmudges;
					}

					//Set center vertex opacity.
					vertexDiffuse[4] = ((Int)(smudge->m_opacity * 255.0f) << 24) | THE_COLOR;

					for (Int i=0; i<5; i++)
					{
						verts->x=smVerts->pos.X;
						verts->y=smVerts->pos.Y;
						verts->z=smVerts->pos.Z;
						verts->nx=0;	//keep AGP write-combining active
						verts->ny=0;
						verts->nz=0;
						verts->diffuse=vertexDiffuse[i];	//set to transparent
						verts->u1=smVerts->uv.X;
						verts->v1=smVerts->uv.Y;
						verts->u2=0;	//keep AGP write-combining active
						verts->v2=0;
						verts++;
						smVerts++;
					}

					smudgesInRenderBatch++;
				}

				++setIt;	//advance to next node.

				if (setIt != m_usedSmudgeSetList.end())	//start next batch at beginning of set.
					smudgeIt = (*setIt)->getUsedSmudgeList().begin();
			}
		}

flushSmudges:
		rts::render::SetGameVertexBuffer(vb_access);

		rts::render::DrawGameTriangles(0,smudgesInRenderBatch*4, 0, smudgesInRenderBatch*5);

		smudgesRemaining -= smudgesInRenderBatch;
	}

	rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_COLOR_OPERATION,RENDER_TEXTURE_OP_MODULATE);
	rts::render::SetGameTextureStageState(0,GAME_TEXTURE_STAGE_ALPHA_OPERATION,RENDER_TEXTURE_OP_MODULATE);

}
