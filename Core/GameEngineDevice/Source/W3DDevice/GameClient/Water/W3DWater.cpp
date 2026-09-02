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

// FILE: W3DWater.cpp /////////////////////////////////////////////////////////////////////////////
// Created:   Mark Wilczynski, June 2001
// Desc:      Draw reflective water surface.  Also handles drawing of waves/ripples
//			  on the surface.
///////////////////////////////////////////////////////////////////////////////////////////////////

#define SCROLL_UV

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////

#include "W3DDevice/GameClient/W3DWater.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/W3DWaterTracks.h"
#include "W3DDevice/GameClient/W3DAssetManager.h"
#include "WW3D2/texture.h"
#include "WW3D2/assetmgr.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/scene.h"
#include "WW3D2/texturemipbuffer.h"
#if defined(_WIN64)
#include "WW3D2/nativew3dsampledtexture.h"
#endif
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderTexturePublication.h"
#include "Renderer/RenderMatrixMath.h"
#include "WW3D2/nativew3dbuffercompat.h"
#include "WW3D2/light.h"
#include "WWLib/simplevec.h"
#include "WW3D2/mesh.h"
#include "WW3D2/matinfo.h"

#include "Common/FramePacer.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/Xfer.h"
#include "Common/GameLOD.h"

#include "GameClient/Color.h"
#include "GameClient/Water.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/ScriptEngine.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/Common/EffectPrepare.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"
#include "Lib/WaterPolygonKernel.h"
#include "W3DDevice/GameClient/W3DDisplay.h"
#include "W3DDevice/GameClient/W3DPoly.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "W3DDevice/GameClient/W3DCustomScene.h"

#include "WWMath/wwmath.h"

#include <new>
#include <stdlib.h>
#include <string.h>
#include <mmsystem.h>

using namespace rts::render;



#define MIPMAP_BUMP_TEXTURE

// DEFINES ////////////////////////////////////////////////////////////////////////////////////////
#define SKYPLANE_SIZE	(384.0f*MAP_XY_FACTOR)
#define SKYPLANE_HEIGHT	(30.0f)

#define SKYBODY_TEXTURE	"TSMoonLarg.tga"
#define SKYBODY_SIZE	45.0f		//extent or radius of sky body

#define SKYBODY_X	150.0f	//location of skybody
#define SKYBODY_Y	550.0f	//location of skybody

/* in the bay
#define SKYBODY_X	120.0f			//location of skybody
#define SKYBODY_Y	75.0f			//location of skybody
*/

#define SKYBODY_HEIGHT	SKYPLANE_HEIGHT	//altitude of sky body (z-buffer disabled, so can equal sky height).

//GeForce3 water system defines
#define PATCH_SIZE 15		//number of vertices on patch edge.  Large patches may waste vertices off edge of screen.
#define PATCH_UV_TILES	42	//number of times the bump map texture is tiled across patch (must be integer!).
#define PATCH_SCALE (4.0f * MAP_XY_FACTOR)	//horizontal scale factor. Adjust this and size to get desired vertex density.
#define SEA_REFLECTION_SIZE 256		//dimensions of reflection texture

#define SEA_BUMP_SCALE		(0.06f)		//scales the du/dv offsets stored in bump map (~ amount to perturb)
#define BUMP_SIZE WATER_MESH_BUMP_SIZE
#define REFLECTION_FACTOR 0.1f

#define PATCH_WIDTH (PATCH_SIZE-1)	//internal defines
#define PATCH_UV_SCALE	((Real)PATCH_UV_TILES/(Real)PATCH_WIDTH)
#define SEA_PATCH_FVF	(rts::render::GAME_VERTEX_XYZDUV1)

//3D Grid Mesh Water defines.
#define WATER_MESH_OPACITY		0.5f
#define WATER_MESH_X_VERTICES	128
#define WATER_MESH_Y_VERTICES	128
#define WATER_MESH_SPACING	MAP_XY_FACTOR	//same as terrain

#ifdef USE_MESH_NORMALS
#define WATER_MESH_FVF	rts::render::GAME_VERTEX_XYZNDUV2
typedef VertexFormatXYZNDUV2 MaterMeshVertexFormat;
#else
#define WATER_MESH_FVF	rts::render::GAME_VERTEX_XYZDUV2
typedef VertexFormatXYZDUV2 MaterMeshVertexFormat;
#endif

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#define RTS_WATER_POLYGON_MODERN 1
#endif

#if defined(RTS_WATER_POLYGON_MODERN)
namespace
{
enum
{
	WATER_TRAPEZOID_MAX_JOBS = 256,
	WATER_TRAPEZOID_MIN_ITEMS_PER_JOB = 256
};

/* One render owner reuses this bounded CRT storage across trapezoid draws.
 * Workers only see the immutable snapshot and disjoint arrays; renderer buffers
 * remain locked and published by the owner after the join. */
class WaterTrapezoidScratch
{
public:
	WaterTrapezoidScratch()
		: m_vertices(0), m_indices(0), m_vertexCapacity(0), m_indexCapacity(0) {}
	~WaterTrapezoidScratch()
	{
		free(m_indices);
		free(m_vertices);
	}

	bool reserve(unsigned vertexCount, unsigned indexCount)
	{
		if (vertexCount > WATER_POLYGON_MAX_VERTICES ||
			indexCount > WATER_POLYGON_MAX_INDICES)
			return false;
		WaterPolygonVertex *vertices = m_vertices;
		unsigned short *indices = m_indices;
		if (vertexCount > m_vertexCapacity)
			vertices = static_cast<WaterPolygonVertex *>(
				malloc(vertexCount * sizeof(WaterPolygonVertex)));
		if (indexCount > m_indexCapacity)
			indices = static_cast<unsigned short *>(
				malloc(indexCount * sizeof(unsigned short)));
		if ((vertexCount > m_vertexCapacity && vertices == 0) ||
			(indexCount > m_indexCapacity && indices == 0))
		{
			if (vertices != m_vertices) free(vertices);
			if (indices != m_indices) free(indices);
			return false;
		}
		if (vertices != m_vertices)
		{
			free(m_vertices);
			m_vertices = vertices;
			m_vertexCapacity = vertexCount;
		}
		if (indices != m_indices)
		{
			free(m_indices);
			m_indices = indices;
			m_indexCapacity = indexCount;
		}
		return true;
	}

	WaterPolygonVertex *vertices() { return m_vertices; }
	unsigned short *indices() { return m_indices; }
	Real *sinTable() { return m_sinTable; }
	void captureSinTable(const Real *source)
	{
		memcpy(m_sinTable, source,
			WATER_POLYGON_FAST_SIN_TABLE_SIZE * sizeof(Real));
	}

private:
	WaterTrapezoidScratch(const WaterTrapezoidScratch &);
	WaterTrapezoidScratch &operator=(const WaterTrapezoidScratch &);
	WaterPolygonVertex *m_vertices;
	unsigned short *m_indices;
	unsigned m_vertexCapacity;
	unsigned m_indexCapacity;
	Real m_sinTable[WATER_POLYGON_FAST_SIN_TABLE_SIZE];
};

static WaterTrapezoidScratch s_waterTrapezoidScratch;

class WaterTrapezoidJob : public rts::Job
{
public:
	static void *operator new(size_t bytes, const std::nothrow_t &) throw()
	{
		return malloc(bytes);
	}
	static void operator delete(void *memory) throw() { free(memory); }
	static void operator delete(void *memory, const std::nothrow_t &) throw()
	{
		free(memory);
	}

	WaterTrapezoidJob(const WaterPolygonSnapshot &snapshot,
		WaterPolygonVertex *vertices, unsigned short *indices,
		unsigned begin, unsigned end, bool indexJob)
		: m_snapshot(snapshot), m_vertices(vertices), m_indices(indices),
		  m_begin(begin), m_end(end), m_indexJob(indexJob),
		  m_floatingPointState() {}

	virtual void execute(rts::JobContext &context)
	{
		rts::JobFloatingPointScope floatingPointScope(m_floatingPointState);
		if (context.isCancellationRequested()) return;
		const bool completed = m_indexJob ?
			PrepareWaterPolygonIndices(m_snapshot, m_indices, m_begin, m_end) :
			PrepareWaterPolygonVertices(m_snapshot, m_vertices, m_begin, m_end);
		if (!completed) context.fail();
	}

private:
	const WaterPolygonSnapshot m_snapshot;
	WaterPolygonVertex *m_vertices;
	unsigned short *m_indices;
	unsigned m_begin;
	unsigned m_end;
	bool m_indexJob;
	const rts::JobFloatingPointState m_floatingPointState;
};

static bool prepareWaterTrapezoidParallel(const WaterPolygonSnapshot &snapshot,
	WaterTrapezoidScratch &scratch, bool &attempted)
{
	attempted = false;
	rts::JobSystem &system = rts::JobSystem::instance();
	if (!rts::UseParallelPipelines() ||
		snapshot.rectangleCount < WATER_POLYGON_MIN_PARALLEL_CELLS)
		return false;
	attempted = true;
	if (system.isWorkerThread())
		return false;
	if (!system.ensureStarted() || system.workerCount() < 2)
		return false;
	const unsigned vertexCount = snapshot.uCount * snapshot.vCount;
	const unsigned indexCount = snapshot.rectangleCount * 6;
	if (!scratch.reserve(vertexCount, indexCount))
		return false;
	const unsigned vertexJobCount = rts::JobSystem::chooseRangeCount(
		vertexCount, WATER_TRAPEZOID_MIN_ITEMS_PER_JOB, system.workerCount());
	const unsigned indexJobCount = rts::JobSystem::chooseRangeCount(
		snapshot.rectangleCount, WATER_TRAPEZOID_MIN_ITEMS_PER_JOB,
		system.workerCount());
	const unsigned jobCount = vertexJobCount + indexJobCount;
	if (jobCount == 0 || jobCount > WATER_TRAPEZOID_MAX_JOBS)
		return false;

	rts::JobSubmission submissions[WATER_TRAPEZOID_MAX_JOBS];
	rts::JobHandle handles[WATER_TRAPEZOID_MAX_JOBS];
	const rts::JobGroup group = system.createGroup();
	unsigned created = 0;
	if (group.isValid())
	{
		for (; created < vertexJobCount; ++created)
		{
			rts::JobRange range;
			if (!rts::JobSystem::rangeForIndex(vertexCount,
				vertexJobCount, created, range))
				break;
			WaterTrapezoidJob *job = new (std::nothrow) WaterTrapezoidJob(
				snapshot, scratch.vertices(), 0, range.begin, range.end, false);
			if (job == 0) break;
			submissions[created].job = job;
			submissions[created].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
		}
		if (created == vertexJobCount)
		{
			for (unsigned index = 0; index < indexJobCount; ++index, ++created)
			{
				rts::JobRange range;
				if (!rts::JobSystem::rangeForIndex(snapshot.rectangleCount,
					indexJobCount, index, range))
					break;
				WaterTrapezoidJob *job = new (std::nothrow) WaterTrapezoidJob(
					snapshot, 0, scratch.indices(), range.begin, range.end, true);
				if (job == 0) break;
				submissions[created].job = job;
				submissions[created].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
			}
		}
	}
	if (created != jobCount ||
		!system.trySubmitBatch(submissions, jobCount, group, handles))
	{
		for (unsigned index = 0; index < created; ++index)
			delete submissions[index].job;
		return false;
	}
	if (!system.wait(group) || group.failed() || group.wasCancelled())
		return false;
	return true;
}
}
#endif

#define DRAW_WATER_WAKES
/// @todo: Fix clipping of objects that intersect the mirror surface
//#define CLIP_GEOMETRY_TO_PLANE	// this enables clipping of objects that intersect the mirror surfaces

// The regular water pass uses the same alpha/depth shader description on both
// renderer lanes.  Keep the description in the game-client layer; the seam
// translates it to the selected backend representation.
#define SC_ZFILL_BLEND3 ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE,\
	ShaderClass::SRCBLEND_SRC_ALPHA, ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, \
	ShaderClass::TEXTURING_ENABLE, ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_DISABLE, ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass zFillAlphaShader(SC_ZFILL_BLEND3);

WaterRenderObjClass *TheWaterRenderObj=nullptr; ///<global water rendering object

static unsigned int GetWaterRestoreCullMode()
{
	LegacyLogicalState tracked;
	if (!rts::render::GetTrackedLegacyLogicalState(&tracked))
		return GAME_RENDER_CULL_COUNTER_CLOCKWISE;
	if (tracked.pipeline.rasterizer.cullMode == RENDER_CULL_NONE)
		return GAME_RENDER_CULL_NONE;
	// The game seam expresses winding directly.  The tracked state retains the
	// same orientation bit used by both compatibility and native rasterizers.
	return tracked.pipeline.rasterizer.frontCounterClockwise ?
		GAME_RENDER_CULL_CLOCKWISE : GAME_RENDER_CULL_COUNTER_CLOCKWISE;
}

static Int getRiverVertexDiffuse(W3DShroud *shroud, Real x, Real y, Real shadeR, Real shadeG, Real shadeB, Int diffuse)
{
	if (!shroud)
		return diffuse;

	Int cellX = (Int)(x / shroud->getCellWidth());
	Int cellY = (Int)(y / shroud->getCellHeight());
	W3DShroudLevel level = shroud->getShroudLevel(cellX, cellY);
	Real shroudScale = (Real)level / 255.0f;
	return GameMakeColor(
		(Int)(shadeR * shroudScale),
		(Int)(shadeG * shroudScale),
		(Int)(shadeB * shroudScale),
		((diffuse >> 24) & 0xff) * shroudScale);
}

void doSkyBoxSet(Bool startDraw)
{
	if (TheWritableGlobalData)
		TheWritableGlobalData->m_drawSkyBox = startDraw;
}


#define DONUT_SIDES	90
#define INNER_RADIUS 200.0f
#define OUTER_RADIUS 250.0f
#define TEXTURE_REPEAT_COUNT 16
#define DONUT_HEIGHT	15.0f
//#define DO_FLAT_DONUT
#define AMP_SCALE	(30.0f/120.0f)
#define WAVE_FREQ	0.3f
#define AMP_SCALE2	(10.0f/120.0f)
#define NOISE_FREQ	(2.0f*PI/WAVE_FREQ)

#define NOISE_REPEAT_FACTOR ((float)(1.0f/(16.0f)))


static Bool wireframeForDebug = 0;

void WaterRenderObjClass::setupJbaWaterShader()
{
	if (!TheWaterTransparency->m_additiveBlend)
		rts::render::SetGameShader(ShaderClass::_PresetAlphaShader);
	else
		rts::render::SetGameShader(ShaderClass::_PresetAdditiveShader);

	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);
	m_riverTexture->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	m_riverTexture->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	m_riverTexture->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_BEST);


//	Setting *setting=&m_settings[m_tod];


	rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices
	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_ADD );
	if (!m_riverAlphaEdge->Is_Initialized())
		m_riverAlphaEdge->Init();
	rts::render::SetGameTexture(3, m_riverAlphaEdge);
	rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);
	rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);
	rts::render::SetGameTextureStageState(3,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1);

	Bool doSparkles = true;

	if (m_riverWaterPixelShader && doSparkles) {
		if (!m_waterSparklesTexture->Is_Initialized())
			m_waterSparklesTexture->Init();
		rts::render::SetGameTexture(1, m_waterSparklesTexture);

		if (!m_waterNoiseTexture->Is_Initialized())
			m_waterNoiseTexture->Init();
		rts::render::SetGameTexture(2, m_waterNoiseTexture);

		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		// Two output coordinates are used.
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

		RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);
		RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);
		RenderMatrix4x4 scale;
		RenderMatrixScaling(&scale, NOISE_REPEAT_FACTOR, NOISE_REPEAT_FACTOR,1);
		RenderMatrix4x4 destMatrix;
		RenderMatrixMultiply(&destMatrix, &inv, &scale);
		RenderMatrixTranslation(&scale, m_riverVOrigin, m_riverVOrigin,0);
		RenderMatrixMultiply(&destMatrix, &destMatrix, &scale);
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE2, &destMatrix);

	}
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(3, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(3, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	if (m_riverWaterPixelShader){
		Vector4 reflectionFactor(REFLECTION_FACTOR, REFLECTION_FACTOR,
			REFLECTION_FACTOR, 1.0f);
		rts::render::SetGamePixelShaderConstant(0, &reflectionFactor, 1);
		rts::render::SetGamePixelShader(m_riverWaterPixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER);
	}
	else
	{
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	}
}




//-------------------------------------------------------------------------------------------------
/** Destructor. Releases w3d assets. */
//-------------------------------------------------------------------------------------------------
WaterRenderObjClass::~WaterRenderObjClass()
{
	REF_PTR_RELEASE(m_meshVertexMaterialClass);
	REF_PTR_RELEASE(m_vertexMaterialClass);
	REF_PTR_RELEASE(m_meshLight);
	REF_PTR_RELEASE(m_alphaClippingTexture);
	REF_PTR_RELEASE (m_skyBox);

	REF_PTR_RELEASE (m_riverTexture);
	REF_PTR_RELEASE (m_whiteTexture);
	REF_PTR_RELEASE (m_waterNoiseTexture);
	REF_PTR_RELEASE (m_riverAlphaEdge);
	REF_PTR_RELEASE (m_waterSparklesTexture);

	Int i;

	for(i=0; i<TIME_OF_DAY_COUNT; i++)
	{	REF_PTR_RELEASE(m_settings[i].skyTexture);
		REF_PTR_RELEASE(m_settings[i].waterTexture);
	}

	i=NUM_BUMP_FRAMES;
	while (i--)
	{
		REF_PTR_RELEASE(m_pBumpTexture[i]);
		REF_PTR_RELEASE(m_pBumpTexture2[i]);
	}

	delete [] m_meshData;
	m_meshData = nullptr;
	m_meshDataSize = 0;

	//Release strings allocated inside global water settings.
	for  (i=0; i<TIME_OF_DAY_COUNT; i++)
	{	WaterSettings[i].m_skyTextureFile.clear();
		WaterSettings[i].m_waterTextureFile.clear();
	}
	deleteInstance((WaterTransparencySetting*)TheWaterTransparency.getNonOverloadedPointer());
	TheWaterTransparency = nullptr;
	ReleaseResources();

	delete m_waterTrackSystem;
}

//-------------------------------------------------------------------------------------------------
/** Constructor. Just nulls out some variables. */
//-------------------------------------------------------------------------------------------------
WaterRenderObjClass::WaterRenderObjClass()
{
	memset( &m_settings, 0, sizeof( m_settings ) );
	m_dx=0;
	m_dy=0;
	m_indexBuffer=nullptr;
	m_waterTrackSystem = nullptr;
	m_doWaterGrid = FALSE;
	m_meshVertexMaterialClass=nullptr;
	m_meshLight=nullptr;
	m_vertexMaterialClass=nullptr;
	m_alphaClippingTexture=nullptr;
	m_useCloudLayer=true;
	m_waterType = WATER_TYPE_0_TRANSLUCENT;
	m_tod=TIME_OF_DAY_AFTERNOON;
	m_pReflectionTexture=nullptr;
	m_pReflectionDepthTexture=nullptr;
	m_skyBox=nullptr;
	m_pDev=nullptr;
	m_vertexBuffer=nullptr;
	m_waterIndexBuffer=nullptr;
	m_vertexBufferOffset=0;

	m_wavePixelShader=0;
	m_waveVertexShader=0;
	m_meshData=nullptr;
	m_meshDataSize = 0;
	m_meshInMotion = FALSE;
	m_gridOrigin=Vector2(0,0);
	m_gridDirectionX=Vector2(1.0f,0.0f);
	m_gridDirectionY=Vector2(1.0f,0.0f);

	m_gridCellSize=WATER_MESH_SPACING;
	m_gridCellsX=WATER_MESH_X_VERTICES;
	m_gridCellsY=WATER_MESH_Y_VERTICES;
	m_gridWidth = m_gridCellsX * m_gridCellSize;
	m_gridHeight = m_gridCellsY * m_gridCellSize;

	Int i=NUM_BUMP_FRAMES;
	while (i--)
	{
		m_pBumpTexture[i]=nullptr;
		m_pBumpTexture2[i]=nullptr;
	}

	m_riverVOrigin=0;
	m_riverTexture=nullptr;
	m_whiteTexture=nullptr;
#if defined(_WIN64)
	m_whiteTexturePublishPending=FALSE;
#endif
	m_waterNoiseTexture=nullptr;
	m_riverAlphaEdge=nullptr;
	m_waterPixelShader=0;		///<logical water pixel program.
	m_riverWaterPixelShader=0;		///<logical river pixel program.
	m_trapezoidWaterPixelShader=0;		///<logical trapezoid pixel program.
	m_waterSparklesTexture=nullptr;
	m_riverXOffset=0;
	m_riverYOffset=0;
}

//-------------------------------------------------------------------------------------------------
/** WW3D method that returns object bounding sphere used in frustum culling*/
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::Get_Obj_Space_Bounding_Sphere(SphereClass & sphere) const
{
	//Since this object is more of a system (containing lots of water pieces),
	//let's disable culling by making bounds huge.  Let each piece do it's own cull.
	Vector3	ObjSpaceCenter(0,0,0);
//	Vector3	ObjSpaceRadius(m_dx,m_dy,0);
	Vector3	ObjSpaceRadius(50000,50000,0);

	sphere.Init(ObjSpaceCenter,ObjSpaceRadius.Length());
}

//-------------------------------------------------------------------------------------------------
/** WW3D method that returns object bounding box used in collision detection*/
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::Get_Obj_Space_Bounding_Box(AABoxClass & box) const
{
	//Since this object is more of a system (containing lots of water pieces),
	//let's disable culling by making bounds huge.  Let each piece do it's own cull.

	Vector3	ObjSpaceCenter(0,0,0);
	Vector3	ObjSpaceExtents(50000,50000,0.001f*m_dy);	//since mirror is a plane, it has no thickness. Set to m_dy/1000.

	box.Init(ObjSpaceCenter,ObjSpaceExtents);
}

//-------------------------------------------------------------------------------------------------
/** returns the class id, so the scene can tell what kind of render object it has. */
//-------------------------------------------------------------------------------------------------
Int WaterRenderObjClass::Class_ID() const
{
	return RenderObjClass::CLASSID_UNKNOWN;
}

//-------------------------------------------------------------------------------------------------
/** Not used, but required virtual method. */
//-------------------------------------------------------------------------------------------------
RenderObjClass *	 WaterRenderObjClass::Clone() const
{
	assert(false);
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
/** Convert a grayscale source image into signed two-channel bump data.  The
 * texture and surface classes keep the platform resource opaque while retaining
 * the historical edge-gradient algorithm and complete mip chain. */
#if defined(_WIN64)
HRESULT WaterRenderObjClass::initBumpMap(TextureClass **pTex,
	TextureClass *pBumpSource)
{
	if (pTex == nullptr)
		return E_POINTER;
	*pTex = nullptr;
	if (pBumpSource == nullptr)
		return S_OK;

	SurfaceClass::SurfaceDescription sourceDescription;
	pBumpSource->Get_Level_Description(sourceDescription);
	if (sourceDescription.Width == 0 || sourceDescription.Height == 0 ||
		Get_Bytes_Per_Pixel(sourceDescription.Format) != 4)
	{
		// Match the compatibility loader's tolerant handling of compressed or
		// missing source images while leaving the native destination unbound.
		return S_OK;
	}

	const unsigned int sourceLevelCount = pBumpSource->Get_Mip_Level_Count();
	if (sourceLevelCount == 0 || sourceLevelCount > MIP_LEVELS_MAX)
		return S_OK;

	TextureMipBuffer bumpMips[MIP_LEVELS_MAX];
	rts::render::NativeW3DSampledTextureMipView views[MIP_LEVELS_MAX];
	unsigned int width = sourceDescription.Width;
	unsigned int height = sourceDescription.Height;

	for (unsigned int level = 0; level < sourceLevelCount; ++level)
	{
		SurfaceClass *sourceSurface = pBumpSource->Get_Surface_Level(level);
		if (sourceSurface == nullptr)
			return S_OK;

		SurfaceClass::SurfaceDescription levelDescription;
		sourceSurface->Get_Description(levelDescription);
		if (levelDescription.Width != width ||
			levelDescription.Height != height ||
			Get_Bytes_Per_Pixel(levelDescription.Format) != 4)
		{
			REF_PTR_RELEASE(sourceSurface);
			return S_OK;
		}

		int sourcePitch = 0;
		unsigned char *source = static_cast<unsigned char *>(
			sourceSurface->Lock(&sourcePitch));
		if (source == nullptr || sourcePitch <= 0 ||
			static_cast<unsigned int>(sourcePitch) < width * 4U)
		{
			REF_PTR_RELEASE(sourceSurface);
			return S_OK;
		}

		if (!bumpMips[level].allocate(WW3D_FORMAT_U8V8, width, height, 1))
		{
			sourceSurface->Unlock_Read_Only();
			REF_PTR_RELEASE(sourceSurface);
			return E_OUTOFMEMORY;
		}

		const TextureMipLayout &destinationLayout = bumpMips[level].layout();
		unsigned char *destination = bumpMips[level].data();
		for (unsigned int y = 0; y < height; ++y)
		{
			const unsigned char *row = source +
				static_cast<size_t>(y) * static_cast<size_t>(sourcePitch);
			const unsigned char *rowBelow = y + 1 < height ?
				row + sourcePitch : row;
			const unsigned char *rowAbove = y != 0 ?
				row - sourcePitch : row;
			unsigned char *destinationRow = destination +
				static_cast<size_t>(y) * destinationLayout.rowPitch;
			for (unsigned int x = 0; x < width; ++x)
			{
				const unsigned char *pixel = row + x * 4U;
				const unsigned char *pixelRight = x + 1 < width ?
					pixel + 4 : pixel;
				const unsigned char *pixelLeft = x != 0 ?
					pixel - 4 : pixel;
				const long v00 = 256 - pixel[0];
				const long v01 = 256 - pixelRight[0];
				const long vM1 = 256 - pixelLeft[0];
				const long v10 = 256 - rowBelow[x * 4U];
				const long v1M = 256 - rowAbove[x * 4U];

				long iDu = vM1 - v01;
				const long iDv = v1M - v10;
				if (v00 < vM1 && v00 < v01)
				{
					iDu = vM1 - v00;
					if (iDu < v00 - v01)
						iDu = v00 - v01;
				}

				destinationRow[x * 2U] = static_cast<unsigned char>(iDu);
				destinationRow[x * 2U + 1U] = static_cast<unsigned char>(iDv);
			}
		}

		sourceSurface->Unlock_Read_Only();
		REF_PTR_RELEASE(sourceSurface);
		views[level].data = bumpMips[level].data();
		views[level].dataSize = destinationLayout.dataSize;
		views[level].rowPitch = destinationLayout.rowPitch;
		ReduceTextureMipDimensions(width, height);
	}

	rts::render::NativeW3DSampledTextureUpload upload;
	if (!upload.Prepare(WW3D_FORMAT_U8V8, sourceDescription.Width,
		sourceDescription.Height, sourceLevelCount, 1, views,
		sourceLevelCount))
		return E_FAIL;

	TextureClass *nativeTexture = TextureClass::Create_Native_From_Prepared(
		upload.Descriptor(), upload.Subresources(),
		upload.SubresourceCount(), WW3D_FORMAT_U8V8);
	if (nativeTexture == nullptr)
		return E_OUTOFMEMORY;

	*pTex = nativeTexture;
	return S_OK;
}
#else
HRESULT WaterRenderObjClass::initBumpMap(TextureClass **pTex,
	TextureClass *pBumpSource)
{
	if (pTex == nullptr)
		return E_POINTER;
	*pTex = nullptr;
	if (pBumpSource == nullptr)
		return S_OK;

	SurfaceClass::SurfaceDescription sourceDescription;
	pBumpSource->Get_Level_Description(sourceDescription);
	if (sourceDescription.Width == 0 || sourceDescription.Height == 0 ||
		Get_Bytes_Per_Pixel(sourceDescription.Format) != 4)
		return S_OK;

	const unsigned int sourceLevelCount = pBumpSource->Get_Mip_Level_Count();
	if (sourceLevelCount == 0 || sourceLevelCount > MIP_LEVELS_MAX)
		return S_OK;

	TextureClass *bumpTexture = NEW_REF(TextureClass,
		(sourceDescription.Width, sourceDescription.Height, WW3D_FORMAT_U8V8,
		MIP_LEVELS_ALL, TextureBaseClass::POOL_MANAGED, false, false));
	if (bumpTexture == nullptr || !bumpTexture->Is_Initialized())
	{
		REF_PTR_RELEASE(bumpTexture);
		return E_FAIL;
	}

	unsigned int width = sourceDescription.Width;
	unsigned int height = sourceDescription.Height;
	for (unsigned int level = 0; level < sourceLevelCount; ++level)
	{
		SurfaceClass *sourceSurface = pBumpSource->Get_Surface_Level(level);
		SurfaceClass *destinationSurface = bumpTexture->Get_Surface_Level(level);
		if (sourceSurface == nullptr || destinationSurface == nullptr)
		{
			REF_PTR_RELEASE(sourceSurface);
			REF_PTR_RELEASE(destinationSurface);
			REF_PTR_RELEASE(bumpTexture);
			return E_FAIL;
		}

		SurfaceClass::SurfaceDescription levelDescription;
		sourceSurface->Get_Description(levelDescription);
		SurfaceClass::SurfaceDescription destinationDescription;
		destinationSurface->Get_Description(destinationDescription);
		if (levelDescription.Width != width ||
			levelDescription.Height != height ||
			destinationDescription.Width != width ||
			destinationDescription.Height != height ||
			Get_Bytes_Per_Pixel(levelDescription.Format) != 4 ||
			Get_Bytes_Per_Pixel(destinationDescription.Format) != 2)
		{
			REF_PTR_RELEASE(sourceSurface);
			REF_PTR_RELEASE(destinationSurface);
			REF_PTR_RELEASE(bumpTexture);
			return E_FAIL;
		}

		int sourcePitch = 0;
		int destinationPitch = 0;
		unsigned char *source = static_cast<unsigned char *>(
			sourceSurface->Lock(&sourcePitch));
		unsigned char *destination = static_cast<unsigned char *>(
			destinationSurface->Lock(&destinationPitch));
		if (source == nullptr || destination == nullptr || sourcePitch <= 0 ||
			destinationPitch <= 0 ||
			static_cast<unsigned int>(sourcePitch) < width * 4U ||
			static_cast<unsigned int>(destinationPitch) < width * 2U)
		{
			if (source != nullptr)
				sourceSurface->Unlock_Read_Only();
			if (destination != nullptr)
				destinationSurface->Unlock();
			REF_PTR_RELEASE(sourceSurface);
			REF_PTR_RELEASE(destinationSurface);
			REF_PTR_RELEASE(bumpTexture);
			return E_FAIL;
		}

		for (unsigned int y = 0; y < height; ++y)
		{
			const unsigned char *row = source +
				static_cast<size_t>(y) * static_cast<size_t>(sourcePitch);
			const unsigned char *rowBelow = y + 1 < height ?
				row + sourcePitch : row;
			const unsigned char *rowAbove = y != 0 ?
				row - sourcePitch : row;
			unsigned char *destinationRow = destination +
				static_cast<size_t>(y) * static_cast<size_t>(destinationPitch);
			for (unsigned int x = 0; x < width; ++x)
			{
				const unsigned char *pixel = row + x * 4U;
				const unsigned char *pixelRight = x + 1 < width ?
					pixel + 4 : pixel;
				const unsigned char *pixelLeft = x != 0 ?
					pixel - 4 : pixel;
				const long v00 = 256 - pixel[0];
				const long v01 = 256 - pixelRight[0];
				const long vM1 = 256 - pixelLeft[0];
				const long v10 = 256 - rowBelow[x * 4U];
				const long v1M = 256 - rowAbove[x * 4U];

				long iDu = vM1 - v01;
				const long iDv = v1M - v10;
				if (v00 < vM1 && v00 < v01)
				{
					iDu = vM1 - v00;
					if (iDu < v00 - v01)
						iDu = v00 - v01;
				}
				destinationRow[x * 2U] = static_cast<unsigned char>(iDu);
				destinationRow[x * 2U + 1U] = static_cast<unsigned char>(iDv);
			}
		}

		sourceSurface->Unlock_Read_Only();
		bool destinationPublished = true;
#if defined(_WIN64)
		destinationPublished = destinationSurface->Unlock_Native_Surface();
#else
		destinationSurface->Unlock();
#endif
		if (!destinationPublished)
		{
			REF_PTR_RELEASE(sourceSurface);
			REF_PTR_RELEASE(destinationSurface);
			REF_PTR_RELEASE(bumpTexture);
			return E_FAIL;
		}
		REF_PTR_RELEASE(sourceSurface);
		REF_PTR_RELEASE(destinationSurface);
		ReduceTextureMipDimensions(width, height);
	}

	*pTex = bumpTexture;
	return S_OK;
}
#endif

//-------------------------------------------------------------------------------------------------
/** Create and fill a renderer vertex buffer with water surface vertices */
//-------------------------------------------------------------------------------------------------
HRESULT WaterRenderObjClass::generateVertexBuffer(Int sizeX, Int sizeY,
	Int vertexSize, Bool doStatic)
{
	m_numVertices = sizeX * sizeY;
	if (m_numVertices <= 0 || m_numVertices > 0xffff)
		return E_INVALIDARG;

	const Int expectedVertexSize = doStatic ?
		static_cast<Int>(sizeof(SEA_PATCH_VERTEX)) :
		static_cast<Int>(sizeof(MaterMeshVertexFormat));
	if (vertexSize != expectedVertexSize)
		return E_INVALIDARG;

	if (m_vertexBuffer == nullptr)
	{
		const unsigned fvf = doStatic ? SEA_PATCH_FVF : WATER_MESH_FVF;
		const DX8VertexBufferClass::UsageType usage = doStatic ?
			DX8VertexBufferClass::USAGE_DEFAULT :
			DX8VertexBufferClass::USAGE_DYNAMIC;
		m_vertexBuffer = NEW_REF(DX8VertexBufferClass,
			(fvf, static_cast<unsigned short>(m_numVertices), usage));
		if (m_vertexBuffer == nullptr || !m_vertexBuffer->Is_Valid())
		{
			REF_PTR_RELEASE(m_vertexBuffer);
			return E_FAIL;
		}
	}

	m_vertexBufferOffset = 0;
	if (!doStatic)
		return S_OK;

	unsigned char *data = nullptr;
	if (m_vertexBuffer->Lock(0,
		static_cast<unsigned int>(m_numVertices * vertexSize), &data, 0) != 0 ||
		data == nullptr)
	{
		REF_PTR_RELEASE(m_vertexBuffer);
		return E_FAIL;
	}

	SEA_PATCH_VERTEX *vertices = reinterpret_cast<SEA_PATCH_VERTEX *>(data);
	Setting *setting = &m_settings[m_tod];
	for (Int z = 0; z < sizeY; ++z)
	{
		for (Int x = 0; x < sizeX; ++x)
		{
			vertices->x = static_cast<float>(x);
			vertices->y = m_level;
			vertices->z = static_cast<float>(z);
			vertices->tu = static_cast<float>(x) * PATCH_UV_SCALE;
			vertices->tv = static_cast<float>(z) * PATCH_UV_SCALE;
			vertices->c = setting->transparentWaterDiffuse;
			++vertices;
		}
	}

	return m_vertexBuffer->Unlock() == 0 ? S_OK : E_FAIL;
}

//-------------------------------------------------------------------------------------------------
/** Create and fill a renderer index buffer with water surface strip indices */
//-------------------------------------------------------------------------------------------------
HRESULT WaterRenderObjClass::generateIndexBuffer(Int sizeX, Int sizeY)
{
	m_numIndices = (sizeY - 1) * (sizeX * 2 + 2) - 2;
	if (m_numIndices <= 0 || m_numIndices + 2 > 0xffff)
		return E_INVALIDARG;

	REF_PTR_RELEASE(m_waterIndexBuffer);
	m_waterIndexBuffer = NEW_REF(DX8IndexBufferClass,
		(static_cast<unsigned short>(m_numIndices + 2),
		DX8IndexBufferClass::USAGE_DEFAULT));
	if (m_waterIndexBuffer == nullptr || !m_waterIndexBuffer->Is_Valid())
	{
		REF_PTR_RELEASE(m_waterIndexBuffer);
		return E_FAIL;
	}

	unsigned char *data = nullptr;
	if (m_waterIndexBuffer->Lock(0,
		static_cast<unsigned int>(m_numIndices * sizeof(UnsignedShort)),
		&data, 0) != 0 || data == nullptr)
	{
		REF_PTR_RELEASE(m_waterIndexBuffer);
		return E_FAIL;
	}

	UnsignedShort *indices = reinterpret_cast<UnsignedShort *>(data);
	Int i = 0;
	Int j = 0;
	Int k = 0;
	while (i < m_numIndices)
	{
		for (; k < sizeX * (j + 1); k += 1, i += 2)
		{
			indices[i] = static_cast<UnsignedShort>(k + sizeX);
			indices[i + 1] = static_cast<UnsignedShort>(k);
		}
		if (i < m_numIndices)
		{
			indices[i] = static_cast<UnsignedShort>(k - 1);
			indices[i + 1] = static_cast<UnsignedShort>(k + sizeX);
			i += 2;
		}
		++j;
	}

	return m_waterIndexBuffer->Unlock() == 0 ? S_OK : E_FAIL;
}

//-------------------------------------------------------------------------------------------------
/** Releases all w3d assets
//-------------------------------------------------------------------------------------------------
/** Releases all w3d assets, to prepare for Reset device call. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::ReleaseResources()
{

	REF_PTR_RELEASE(m_indexBuffer);

	REF_PTR_RELEASE(m_pReflectionTexture);
	REF_PTR_RELEASE(m_pReflectionDepthTexture);
	REF_PTR_RELEASE(m_vertexBuffer);
	REF_PTR_RELEASE(m_waterIndexBuffer);

	if (m_waterTrackSystem)
		m_waterTrackSystem->ReleaseResources();

	if (m_wavePixelShader)
		(void)rts::render::DeleteGameShader(false, m_wavePixelShader);
	if (m_waveVertexShader)
		(void)rts::render::DeleteGameShader(true, m_waveVertexShader);
	if (m_waterPixelShader)
		(void)rts::render::DeleteGameShader(false, m_waterPixelShader);
	if (m_trapezoidWaterPixelShader)
		(void)rts::render::DeleteGameShader(false, m_trapezoidWaterPixelShader);
	if (m_riverWaterPixelShader)
		(void)rts::render::DeleteGameShader(false, m_riverWaterPixelShader);

	m_wavePixelShader=0;
	m_waveVertexShader=0;
	m_waterPixelShader = 0;
	m_trapezoidWaterPixelShader=0;
	m_riverWaterPixelShader=0;
}

//-------------------------------------------------------------------------------------------------
/** Publish the white pixel used when the water shader has no shroud texture.

    The legacy path retains its original Unlock/notification ABI. Native x64
    surfaces have a CPU shadow and a status-returning completion boundary; a
    failed publication leaves that shadow available and marks the logical
    pixel dirty so the next water setup/reacquire retries it. */
//-------------------------------------------------------------------------------------------------
Bool WaterRenderObjClass::updateWhiteTexture()
{
	if (m_whiteTexture == nullptr)
	{
#if defined(_WIN64)
		m_whiteTexturePublishPending=TRUE;
#endif
		return FALSE;
	}

	if (!m_whiteTexture->Is_Initialized())
		m_whiteTexture->Init();

	SurfaceClass *surface=m_whiteTexture->Get_Surface_Level();
	if (surface == nullptr)
	{
#if defined(_WIN64)
		m_whiteTexturePublishPending=TRUE;
#endif
		return FALSE;
	}

	int pitch=0;
	void *pBits = surface->Lock(&pitch);
	const unsigned int bytesPerPixel = surface->Get_Bytes_Per_Pixel();
	if (pBits == nullptr || pitch <= 0 || bytesPerPixel == 0)
	{
		// A successful Lock must still be completed before releasing the wrapper.
		// On x64 this completion is status-bearing; the failed logical write stays
		// pending even if the unlock itself can only release the CPU lock.
#if defined(_WIN64)
		if (pBits != nullptr)
			(void)surface->Unlock_Native_Surface();
		m_whiteTexturePublishPending=TRUE;
#else
		if (pBits != nullptr)
			surface->Unlock();
#endif
		REF_PTR_RELEASE(surface);
		return FALSE;
	}

	surface->Draw_Pixel(0, 0, 0xffffffff, bytesPerPixel, pBits, pitch);
	Bool publicationSucceeded=FALSE;
#if defined(_WIN64)
	publicationSucceeded=surface->Unlock_Native_Surface() ? TRUE : FALSE;
	m_whiteTexturePublishPending=publicationSucceeded ? FALSE : TRUE;
#else
	surface->Unlock();
	publicationSucceeded=TRUE;
	rts::render::NotifyTextureChanged(m_whiteTexture);
#endif
	REF_PTR_RELEASE(surface);
	return publicationSucceeded;
}

//-------------------------------------------------------------------------------------------------
/** (Re)allocates all W3D assets after a reset.. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::ReAcquireResources()
{
	HRESULT hr;

	// Reacquisition may be requested more than once by a device-reset owner.
	// Release the previous quad buffer before replacing it so a failed
	// allocation cannot orphan the old resource or leave a stale lock target.
	REF_PTR_RELEASE(m_indexBuffer);
	m_indexBuffer=NEW_REF(DX8IndexBufferClass,(6));
	if (m_indexBuffer == nullptr || !m_indexBuffer->Is_Valid())
	{
		REF_PTR_RELEASE(m_indexBuffer);
		return;
	}
	// Fill up the IB
	{
		DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexBuffer);
		UnsignedShort *ib=lockIdxBuffer.Get_Index_Array();
		if (!lockIdxBuffer.Is_Locked() || ib == nullptr)
		{
			REF_PTR_RELEASE(m_indexBuffer);
			return;
		}
		//quad of 2 triangles:
		//	3-----2
		//  |    /|
		//  |  /  |
		//	|/    |
		//  0-----1
		ib[0]=3;
		ib[1]=0;
		ib[2]=2;
		ib[3]=2;
		ib[4]=0;
		ib[5]=1;
	}

	//We're using the same grid for either 3D Water Mesh or Pixel/Vertex shader.  Just
	//allocate the right size depending on usage
	if (m_meshData)
	{
		//Create new grid data
		if (FAILED(generateIndexBuffer(m_gridCellsX+1,m_gridCellsY+1)))
			return;
		if (FAILED(generateVertexBuffer(m_gridCellsX+1,m_gridCellsY+1,sizeof(MaterMeshVertexFormat),false)))
			return;
	}
	else
	if (m_waterType == WATER_TYPE_2_PVSHADER)
	{	//pixel/vertex shader based water assets.
		if (FAILED(hr=generateIndexBuffer(PATCH_SIZE,PATCH_SIZE)))
			return;

		if (FAILED(hr=generateVertexBuffer(PATCH_SIZE,PATCH_SIZE,sizeof(SEA_PATCH_VERTEX),true)))
			return;

		// The shader seam supplies the backend-specific declaration from the
		// asset.  The x86 compatibility owner synthesizes the historical wave
		// vertex layout when the caller leaves this boundary unspecified.

		if (rts::render::CreateGameShaderFromAsset("shaders\\wave.pso", false,
			0, 0, 0, &m_wavePixelShader) != rts::render::RENDER_RESULT_OK)
			return;

		if (rts::render::CreateGameShaderFromAsset("shaders\\wave.vso", true,
			0, 0, 0, &m_waveVertexShader) != rts::render::RENDER_RESULT_OK)
		{
			// Keep reacquisition atomic: do not retain a pixel program whose
			// matching vertex program could not be created.
			if (m_wavePixelShader)
				(void)rts::render::DeleteGameShader(false, m_wavePixelShader);
			m_wavePixelShader = 0;
			return;
		}

		REF_PTR_RELEASE(m_pReflectionTexture);
		REF_PTR_RELEASE(m_pReflectionDepthTexture);
		if (rts::render::CreateGameRenderTargetPair(
			SEA_REFLECTION_SIZE, SEA_REFLECTION_SIZE,
			WW3D_FORMAT_A8R8G8B8, WW3D_ZFORMAT_D24S8,
			&m_pReflectionTexture, &m_pReflectionDepthTexture) !=
			rts::render::RENDER_RESULT_OK)
		{
			REF_PTR_RELEASE(m_pReflectionTexture);
			REF_PTR_RELEASE(m_pReflectionDepthTexture);
			if (m_waveVertexShader)
				(void)rts::render::DeleteGameShader(true, m_waveVertexShader);
			if (m_wavePixelShader)
				(void)rts::render::DeleteGameShader(false, m_wavePixelShader);
			m_waveVertexShader = 0;
			m_wavePixelShader = 0;
			return;
		}
	}

	if (m_waterTrackSystem)
		m_waterTrackSystem->ReAcquireResources();

	if (W3DShaderManager::getChipset() >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		(void)rts::render::CreateGameShaderFromAsset(
			"builtin/water/river", false, nullptr, 0, 0,
			&m_riverWaterPixelShader);
		(void)rts::render::CreateGameShaderFromAsset(
			"builtin/water/reflection", false, nullptr, 0, 0,
			&m_waterPixelShader);
		(void)rts::render::CreateGameShaderFromAsset(
			"builtin/water/trapezoid", false, nullptr, 0, 0,
			&m_trapezoidWaterPixelShader);
	}

	//W3D Invalidate textures after losing the device and since we peek at the textures directly, it won't
	//know to reinit them for us.  Do it here manually:
	if (m_riverTexture && !m_riverTexture->Is_Initialized())
		m_riverTexture->Init();
	if (m_waterNoiseTexture && !m_waterNoiseTexture->Is_Initialized())
		m_waterNoiseTexture->Init();
	if (m_riverAlphaEdge && !m_riverAlphaEdge->Is_Initialized())
		m_riverAlphaEdge->Init();
	if (m_waterSparklesTexture && !m_waterSparklesTexture->Is_Initialized())
		m_waterSparklesTexture->Init();
	if (m_whiteTexture && (!m_whiteTexture->Is_Initialized()
#if defined(_WIN64)
		|| m_whiteTexturePublishPending
#endif
		))
		updateWhiteTexture();
}

void WaterRenderObjClass::load()
{
	if (m_waterTrackSystem)
		m_waterTrackSystem->loadTracks();
}

//-------------------------------------------------------------------------------------------------
/** Initializes water with dimensions and parent scene.
	* During rendering, we will render a water surface of given dimensions
	* and reflect the parent scene in its surface.  For now, waters are
	* forced to be rectangles. */
//-------------------------------------------------------------------------------------------------
Int WaterRenderObjClass::init(Real waterLevel, Real dx, Real dy, SceneClass *parentScene, WaterType type)
{

	m_fBumpFrame=0;
	m_fBumpScale=SEA_BUMP_SCALE;

	m_dx=dx;
	m_dy=dy;
	m_level=waterLevel;

	m_LastUpdateTime=timeGetTime();
	m_uScrollPerMs=0.001f;
	m_vScrollPerMs=0.001f;
	m_uOffset=0;
	m_vOffset=0;

	m_parentScene=parentScene;
	m_waterType = type;

	/// Hack for now
	//m_waterType = WATER_TYPE_0_TRANSLUCENT;

	///@todo: calculate a real normal/distance for arbitrary planes.
	m_planeNormal=Vector3(0,0,1);		//water plane normal
	m_planeDistance=m_level;	//water plane distance(always at zero for now)

	m_meshLight=NEW_REF(LightClass,(LightClass::DIRECTIONAL));
	m_meshLight->Set_Ambient(Vector3(0.1f,0.1f,0.1f));
	m_meshLight->Set_Diffuse(Vector3(1.0f,1.0f,1.0f));
	m_meshLight->Set_Specular(Vector3(1.0f,1.0f,1.0f));
	m_meshLight->Set_Position(Vector3(1000,1000,1000));
	//testLight->Set_Spot_Direction(Vector3(TheGlobalData->m_terrainLightX,TheGlobalData->m_terrainLightY,TheGlobalData->m_terrainLightZ));
	m_meshLight->Set_Spot_Direction(Vector3(-0.57f,-0.57f,-0.57f));

	//Setup material for 3D Mesh water.
	m_meshVertexMaterialClass=NEW_REF(VertexMaterialClass,());
	m_meshVertexMaterialClass->Set_Shininess(20.0);
	m_meshVertexMaterialClass->Set_Ambient(1.0f,1.0f,1.0f);
	m_meshVertexMaterialClass->Set_Diffuse(1.0f,1.0f,1.0f);
	m_meshVertexMaterialClass->Set_Specular(0.5,0.5,0.5);
	m_meshVertexMaterialClass->Set_Opacity(WATER_MESH_OPACITY);
	m_meshVertexMaterialClass->Set_Lighting(true);

	//
	// assign the data from the WaterSettings[] global to the data for this
	// render object (we at present only have one water plane)
	//
	loadSetting( &m_settings[ TIME_OF_DAY_MORNING ], TIME_OF_DAY_MORNING );
	loadSetting( &m_settings[ TIME_OF_DAY_AFTERNOON ], TIME_OF_DAY_AFTERNOON );
	loadSetting( &m_settings[ TIME_OF_DAY_EVENING ], TIME_OF_DAY_EVENING );
	loadSetting( &m_settings[ TIME_OF_DAY_NIGHT ], TIME_OF_DAY_NIGHT );

	Set_Sort_Level(2);	//force water to be drawn after all other non translucent objects in scene.
	Set_Force_Visible(TRUE);	//water is always visible since it's a composite object made of multiple planes all over the map.

	ReAcquireResources();
	if (type == WATER_TYPE_2_PVSHADER)
	{	//high-detail water requires its bump assets
		//save previous thumbnail mode
		bool thumbnails_enabled = WW3D::Get_Thumbnail_Enabled();
		WW3D::Set_Thumbnail_Enabled(false);

		//load bump map textures off disk
		TextureClass *pBumpSource;	//temporary textures in a format W3D understands
		Int i;
		i=NUM_BUMP_FRAMES;
		while (i--)
		{
			char bump_name[128];

			sprintf(bump_name,"caust%.2d.tga",i);
			pBumpSource=WW3DAssetManager::Get_Instance()->Get_Texture(bump_name);
			if (pBumpSource != nullptr)
			{
				initBumpMap(m_pBumpTexture+i, pBumpSource);
				WW3DAssetManager::Get_Instance()->Release_Texture(pBumpSource);
				REF_PTR_RELEASE(pBumpSource);
			}
		}
		//restore previous thumpnail mode
		WW3D::Set_Thumbnail_Enabled(thumbnails_enabled);
	}

	//Setup material for regular water
	m_vertexMaterialClass=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);



	m_shaderClass = zFillAlphaShader;//ShaderClass::_PresetAlphaShader;ShaderClass::_PresetOpaqueShader;//detailOpaqueShader;
	m_shaderClass.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);	//water should be visible from both sides

	//Assets used for all types of water
	m_alphaClippingTexture=WW3DAssetManager::Get_Instance()->Get_Texture(SKYBODY_TEXTURE);

#ifdef CLIP_GEOMETRY_TO_PLANE
	m_alphaClippingTexture=WW3DAssetManager::Get_Instance()->Get_Texture("alphaclip.tga");
#endif

	m_skyBox = ((W3DAssetManager*)W3DAssetManager::Get_Instance())->Create_Render_Obj( "new_skybox", TheGlobalData->m_skyBoxScale, 0);

	//Enable clamping on all textures used by the skybox (to reduce corner seams).
	if (m_skyBox && m_skyBox->Class_ID() == RenderObjClass::CLASSID_MESH)
	{
		MeshClass *mesh=(MeshClass*) m_skyBox;
		MaterialInfoClass	*material = mesh->Get_Material_Info();

		for (Int i=0; i<material->Texture_Count(); i++)
		{
			if (material->Peek_Texture(i))
			{
				material->Peek_Texture(i)->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
				material->Peek_Texture(i)->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
			}
		}

		REF_PTR_RELEASE(material);
	}

	m_riverTexture=WW3DAssetManager::Get_Instance()->Get_Texture(TheWaterTransparency->m_standingWaterTexture.str());

	//For some reason setting a null texture does not result in 0xffffffff for pixel shaders so using explicit "white" texture.
	m_whiteTexture=MSGNEW("TextureClass") TextureClass(1,1,WW3D_FORMAT_A4R4G4B4,MIP_LEVELS_1);
	updateWhiteTexture();

	m_waterNoiseTexture=WW3DAssetManager::Get_Instance()->Get_Texture("Noise0000.tga");
	m_riverAlphaEdge=WW3DAssetManager::Get_Instance()->Get_Texture("TWAlphaEdge.tga");
	m_waterSparklesTexture=WW3DAssetManager::Get_Instance()->Get_Texture("WaterSurfaceBubbles.tga");
#ifdef DRAW_WATER_WAKES
	m_waterTrackSystem = NEW WaterTracksRenderSystem;
	m_waterTrackSystem->init();
#endif

	return 0;
}

void WaterRenderObjClass::updateMapOverrides()
{
	if (m_riverTexture && TheWaterTransparency->m_standingWaterTexture.compareNoCase(m_riverTexture->Get_Texture_Name()) != 0)
	{
		REF_PTR_RELEASE(m_riverTexture);
		m_riverTexture = WW3DAssetManager::Get_Instance()->Get_Texture(TheWaterTransparency->m_standingWaterTexture.str());
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void WaterRenderObjClass::reset()
{

	// for vertex animated water mesh reset the values
	if( m_meshData)
	{
		Int i, j;
		WaterMeshData *pData;
		Int	mx = m_gridCellsX + 1;
		Int my = m_gridCellsY + 1;

		// go through each mesh point and adjust the height according to the velocity
		for( j = 0, pData = m_meshData; j < (my + 2); j++ )
		{

			for( i = 0; i < (mx + 2); i++ )
			{

				// areset grid values for this cell
				pData->velocity = 0.0f;
				pData->height = 0.0f;
				pData->preferredHeight = 0.0f;
				pData->status = WaterRenderObjClass::AT_REST;

				// on to the next one
				pData++;

			}

		}

		// mesh data is no longer in motion
		m_meshInMotion = FALSE;

	}

	if (m_waterTrackSystem)
		m_waterTrackSystem->reset();
}

void WaterRenderObjClass::enableWaterGrid(Bool state)
{
	m_doWaterGrid = state;

	m_drawingRiver = false;
	m_disableRiver = false;

	if (state && m_meshData == nullptr)
	{	//water type has changed, must allocate necessary assets for new water.
		//contains the current deformed water surface z(height) values.  With 1 vertex invisible border
		//around surface to speed up normal calculations.
		m_meshDataSize = (m_gridCellsX+1+2)*(m_gridCellsY+1+2);
		m_meshData=NEW WaterMeshData[ m_meshDataSize ];
		memset(m_meshData,0,sizeof(WaterMeshData)*(m_gridCellsX+1+2)*(m_gridCellsY+1+2));
		reset();

		//Release existing grid data through the common reference-counted facade.
		REF_PTR_RELEASE(m_vertexBuffer);
		REF_PTR_RELEASE(m_waterIndexBuffer);

		//Create new grid data
		if (FAILED(generateIndexBuffer(m_gridCellsX+1,m_gridCellsY+1)))
			return;
		if (FAILED(generateVertexBuffer(m_gridCellsX+1,m_gridCellsY+1,sizeof(MaterMeshVertexFormat),false)))
			return;
	}
}

// ------------------------------------------------------------------------------------------------
/** Update phase for water if we need it. */
// ------------------------------------------------------------------------------------------------
void WaterRenderObjClass::update()
{
	// TheSuperHackers @tweak The water movement time step is now decoupled from the render update.
	const Real timeScale = TheFramePacer->getActualLogicTimeScaleOverFpsRatio();

	if (m_waterTrackSystem && rts::render::IsNativeGameRendererActive())
	{
		m_waterTrackSystem->update();
	}

	{
		constexpr const Real MagicOffset = 0.0125f * 33 / 5000; ///< the work of top Munkees; do not question it

		m_riverVOrigin += 0.002f * timeScale;
		m_riverXOffset += (Real)(MagicOffset * timeScale);
		m_riverYOffset += (Real)(2 * MagicOffset * timeScale);

		// This moves offsets towards zero when smaller -1.0 or larger 1.0
		m_riverXOffset -= (Int)m_riverXOffset;
		m_riverYOffset -= (Int)m_riverYOffset;

		m_fBumpFrame += timeScale;
		if (m_fBumpFrame >= NUM_BUMP_FRAMES)
			m_fBumpFrame = 0.0f;

		// for vertex animated water we need to update the vector field
		if( m_doWaterGrid && m_meshInMotion == TRUE )
		{
			const Real PREFERRED_HEIGHT_FUDGE = 1.0f;		///< this is close enough to at rest
			const Real AT_REST_VELOCITY_FUDGE = 1.0f;		///< when we're close enough to at rest height and velocity we will stop
			const Real WATER_DAMPENING = 0.93f;					///< use with up force of 15.0
			Int i, j;
			Int	mx = m_gridCellsX+1;
			Int my = m_gridCellsY+1;
			WaterMeshData *pData;

			//
			// we will mark the mesh as clean now ... if any of the fields are still in motion
			// they will continue to mark the mesh as dirty so processing continues next frame
			//
			m_meshInMotion = FALSE;

			// go through each mesh point and adjust the height according to the velocity
			for( j = 0, pData = m_meshData; j < (my + 2); j++ )
			{

				for( i = 0; i < (mx + 2); i++ )
				{

					// only pay attention to mesh points that are in motion
					if( BitIsSet( pData->status, WaterRenderObjClass::IN_MOTION ) )
					{

						// DAMPENING to slow the changes down
						pData->velocity *= WATER_DAMPENING;

						// if the height here is below our preferred height, we want to add upward force to counteract it
						if( pData->height < pData->preferredHeight )
							pData->velocity -= TheGlobalData->m_gravity * 3.0f;
						else
							pData->velocity += TheGlobalData->m_gravity * 3.0f;

						// adjust the height at this grid location according to the current velocity
						pData->height = pData->height + pData->velocity;

						//
						// if we are close enough to our preferred height and our velocity is small enough
						// this will be our resting location
						//
						if( fabs( pData->height - pData->preferredHeight ) < PREFERRED_HEIGHT_FUDGE &&
								fabs( pData->velocity ) < AT_REST_VELOCITY_FUDGE )
						{

							BitClear( pData->status, WaterRenderObjClass::IN_MOTION );
							pData->height = pData->preferredHeight;
							pData->velocity = 0.0f;

						}
						else
						{

							// there is still motion in the mesh, we need to process next frame
							m_meshInMotion = TRUE;

						}

					}

					// on to the next one
					pData++;

				}

			}

		}

	}

}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::replaceSkyboxTexture(const AsciiString& oldTexName, const AsciiString& newTextName)
{
	W3DAssetManager* assetManager = ((W3DAssetManager*)W3DAssetManager::Get_Instance());

	assetManager->replacePrototypeTexture(m_skyBox, oldTexName.str(), newTextName.str());

	//Enable clamping on all textures used by the skybox (to reduce corner seams).
	if (m_skyBox && m_skyBox->Class_ID() == RenderObjClass::CLASSID_MESH)
	{
		MeshClass *mesh=(MeshClass*) m_skyBox;
		MaterialInfoClass	*material = mesh->Get_Material_Info();

		for (Int i=0; i<material->Texture_Count(); i++)
		{
			if (material->Peek_Texture(i))
			{
				material->Peek_Texture(i)->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
				material->Peek_Texture(i)->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
			}
		}
	}

}

//-------------------------------------------------------------------------------------------------
/** Adjusts various water/sky rendering settings that depend on time of day. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::setTimeOfDay(TimeOfDay tod)
{
	m_tod=tod;
	if (m_waterType == WATER_TYPE_2_PVSHADER)
		generateVertexBuffer(PATCH_SIZE,PATCH_SIZE,sizeof(SEA_PATCH_VERTEX),true);	//update the water mesh with new lighting/alpha
}

//-------------------------------------------------------------------------------------------------
/**Copies GDF settings dealing with a particular time of day into our own
	* structures.  Also allocates any required W3D assets (textures). */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::loadSetting( Setting *setting, TimeOfDay timeOfDay )
{
	SurfaceClass::SurfaceDescription surfaceDesc;

	// sanity
	DEBUG_ASSERTCRASH( setting, ("WaterRenderObjClass::loadSetting, null setting") );

	// textures
	setting->skyTexture = WW3DAssetManager::Get_Instance()->Get_Texture( WaterSettings[ timeOfDay ].m_skyTextureFile.str() );
	setting->waterTexture = WW3DAssetManager::Get_Instance()->Get_Texture( WaterSettings[ timeOfDay ].m_waterTextureFile.str() );

	// texelss per unit
	setting->skyTexelsPerUnit = WaterSettings[ timeOfDay ].m_skyTexelsPerUnit;
	setting->waterTexture->Get_Level_Description( surfaceDesc, 0 );
	setting->skyTexelsPerUnit /= (Real)surfaceDesc.Width;

	// water repeat
	setting->waterRepeatCount = WaterSettings[ timeOfDay ].m_waterRepeatCount;

	// U and V scroll per ms
	setting->uScrollPerMs = WaterSettings[ timeOfDay ].m_uScrollPerMs;
	setting->vScrollPerMs = WaterSettings[ timeOfDay ].m_vScrollPerMs;

	//
	// vertex colors
	//
	// bottom left
	setting->vertex00Diffuse = (WaterSettings[ timeOfDay ].m_vertex00Diffuse.red << 16) |
														 (WaterSettings[ timeOfDay ].m_vertex00Diffuse.green << 8) |
														  WaterSettings[ timeOfDay ].m_vertex00Diffuse.blue;
	// top left
	setting->vertex01Diffuse = (WaterSettings[ timeOfDay ].m_vertex01Diffuse.red << 16) |
														 (WaterSettings[ timeOfDay ].m_vertex01Diffuse.green << 8) |
														  WaterSettings[ timeOfDay ].m_vertex01Diffuse.blue;
	// bottom right
	setting->vertex10Diffuse = (WaterSettings[ timeOfDay ].m_vertex10Diffuse.red << 16) |
														 (WaterSettings[ timeOfDay ].m_vertex10Diffuse.green << 8) |
														  WaterSettings[ timeOfDay ].m_vertex10Diffuse.blue;
	// top right
	setting->vertex11Diffuse = (WaterSettings[ timeOfDay ].m_vertex11Diffuse.red << 16) |
														 (WaterSettings[ timeOfDay ].m_vertex11Diffuse.green << 8) |
														  WaterSettings[ timeOfDay ].m_vertex11Diffuse.blue;

	// diffuse water color
	setting->waterDiffuse = (WaterSettings[ timeOfDay ].m_waterDiffuseColor.alpha << 24) |
												  (WaterSettings[ timeOfDay ].m_waterDiffuseColor.red		<< 16) |
													(WaterSettings[ timeOfDay ].m_waterDiffuseColor.green << 8) |
												   WaterSettings[ timeOfDay ].m_waterDiffuseColor.blue;

	// transparent water color
	setting->transparentWaterDiffuse = (WaterSettings[ timeOfDay ].m_transparentWaterDiffuse.alpha << 24) |
																		 (WaterSettings[ timeOfDay ].m_transparentWaterDiffuse.red	 << 16) |
																		 (WaterSettings[ timeOfDay ].m_transparentWaterDiffuse.green << 8) |
																		  WaterSettings[ timeOfDay ].m_transparentWaterDiffuse.blue;

}

//-------------------------------------------------------------------------------------------------
/** Our water may use effects that require run-time rendered textures.  These
	*	textures need to be updated before we start rendering to the main screen
	* render target because the reflection pass uses a separate color target. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::updateRenderTargetTextures(CameraClass *cam)
{
	if (m_waterType == WATER_TYPE_2_PVSHADER && m_pReflectionTexture != nullptr &&
		(!rts::render::IsNativeGameRendererActive() ||
			m_pReflectionDepthTexture != nullptr) &&
		getClippedWaterPlane(cam, nullptr) &&
		TheTerrainRenderObject && TheTerrainRenderObject->getMap())
		renderMirror(cam);	//generate texture containing reflected scene
}

//-------------------------------------------------------------------------------------------------
/** Renders the reflected scene into an offscreen texture. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::renderMirror(CameraClass *cam)
{
	if (cam == nullptr || m_pReflectionTexture == nullptr ||
		(rts::render::IsNativeGameRendererActive() &&
			m_pReflectionDepthTexture == nullptr))
	{
		return;
	}
#ifdef EXTENDED_STATS
	if (rts::render::GetGameDebugRenderStats().disableWater) {
		return;
	}
#endif
	Matrix3D	OldCameraMatrix=cam->Get_Transform();
	Matrix4x4	FullMatrix4(cam->Get_Transform());	//copy 3x4 matrix into a 4x4
	Vector3		WaterNormal(0,0,1);	//normal of plane used for reflection
	Vector4		WaterPlane(WaterNormal.X,WaterNormal.Y,WaterNormal.Z,m_level);
	Vector3		rRight,rUp,rN,rPos;	//orientation and translation vectors of camera

	Matrix4x4	FullMatrix(FullMatrix4.Transpose());	//swap rows/columns

	//reflect camera right vector
	Real axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[0],WaterNormal);
	rRight = (Vector3&)FullMatrix[0] - (2.0f*axis_distance*WaterNormal);

	//reflect camera up vector
	axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[1],WaterNormal);
	rUp = (Vector3&)FullMatrix[1] - (2.0f*axis_distance*WaterNormal);

	//reflect camera n vector
	axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[2],WaterNormal);
	rN = (Vector3&)FullMatrix[2] - (2.0f*axis_distance*WaterNormal);

	//reflect camera position
	axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[3],WaterNormal);	//distance cam to origin
	axis_distance -= WaterPlane.W;	// subtract mirror plane distance to get distance camera to plane
	rPos = (Vector3&)FullMatrix[3] - (2.0f*axis_distance*WaterNormal);

	//generate a new camera matrix from reflected vectors
	Matrix3D reflectedTransform(rRight,rUp,rN,rPos);


	rts::render::SetGameRenderTarget(m_pReflectionTexture,
		m_pReflectionDepthTexture, !rts::render::IsNativeGameRendererActive());
	if (!rts::render::IsGameRenderingToTexture())
	{
		rts::render::SetGameRenderTarget(nullptr, nullptr, true);
		return;
	}

	// Clear the backbuffer
	const rts::render::GameRenderColor clearColor =
		{ 0.0f, 0.0f, 0.0f, 0.0f };
	if (rts::render::ClearGameRenderTargets(false, true, clearColor, 0.0f) !=
		rts::render::RENDER_RESULT_OK)
	{
		rts::render::SetGameRenderTarget(nullptr, nullptr, true);
		return;
	}	//clearing only z-buffer since background always filled with clouds

	cam->Set_Transform( reflectedTransform );

	//Force reflected image to be drawn into full texture size - not a viewport inside texture.
	Vector2 vMin,vMax,vOldMax,vOldMin;
 	cam->Get_Viewport(vOldMin,vOldMax);
 	vMax.X=vMax.Y=1.0f;
	vMin.X=vMin.Y=0.0f;
 	cam->Set_Viewport(vMin,vMax);

	cam->Apply();	//force an update of all the camera dependent parameters like frustum clip planes

	//flip the winding order of polygons to draw the reflected back sides.
	ShaderClass::Invert_Backface_Culling(true);

	// Render the scene
	renderSky();
	if (m_tod == TIME_OF_DAY_NIGHT)
		renderSkyBody(&reflectedTransform);

	WW3D::Render(m_parentScene,cam);

	cam->Set_Transform(OldCameraMatrix);	//restore original non-reflected matrix
 	cam->Set_Viewport(vOldMin,vOldMax);

	cam->Apply();	//force an update of all the camera dependent parameters like frustum clip planes

	ShaderClass::Invert_Backface_Culling(false);

	// Change the rendertarget back to the main backbuffer
	rts::render::SetGameRenderTarget(nullptr, nullptr, true);
}

//-------------------------------------------------------------------------------------------------
/** Renders (draws) the water.
	*	Algorithm:
	*	Draw reflected scene.
	*	Draw reflected sky layer(s) and bodies.
	*	Clear Zbuffer
	*	Fill Zbuffer by drawing water surface (allows proper sorting into regular scene).
	*	Draw non-reflected scene (done in regular app render loop).
	*
	*	This algorithm doesn't apply to translucent water, which is rendered into a
	*   texture and rendered at end of scene. */
//-------------------------------------------------------------------------------------------------
//DECLARE_PERF_TIMER(Water)
void WaterRenderObjClass::Render(RenderInfoClass & rinfo)
{
	//USE_PERF_TIMER(Water)
	if (TheTerrainRenderObject && !TheTerrainRenderObject->getMap())
		return;	//no map has been loaded yet.

	if (((RTS3DScene *)rinfo.Camera.Get_User_Data())->getCustomPassMode() == SCENE_PASS_ALPHA_MASK ||
		((SceneClass *)rinfo.Camera.Get_User_Data())->Get_Extra_Pass_Polygon_Mode() == SceneClass::EXTRA_PASS_CLEAR_LINE)
		return;	//water is not drawn in wireframe or custom scene passes

#ifdef EXTENDED_STATS
	if (rts::render::GetGameDebugRenderStats().disableWater) {
		return;
	}
#endif
	if (ShaderClass::Is_Backface_Culling_Inverted())
		return;	//the water object will not reflect in itself, so don't do anything if rendering a mirror.

	//this water type needs to rendered after the rest of scene, so buffer it up for later

	// If static sort lists are enabled and this mesh has a sort level, put it on the list instead
	// of rendering it.
	unsigned int sort_level = (unsigned int)Get_Sort_Level();

	if (WW3D::Are_Static_Sort_Lists_Enabled() && sort_level != SORT_LEVEL_NONE)
	{
		WW3D::Add_To_Static_Sort_List(this, sort_level);
		return;
	}

	switch(m_waterType)
	{
		case WATER_TYPE_0_TRANSLUCENT:
		case WATER_TYPE_3_GRIDMESH:
			//Draw the water surface as a bunch of alpha blended tiles covering areas where water is visible
			renderWater();
			if (!m_drawingRiver || m_disableRiver) {
				renderWaterMesh();	//Draw water surface as 3D deforming mesh if it's enabled on this map.
			}
			break;

		case WATER_TYPE_2_PVSHADER:
			//Pixel/Vertex Shader based water which uses an off-screen rendered reflection texture
			if (m_pReflectionTexture == nullptr)
			{
				break;
			}
			drawSea(rinfo);	//draw water surface
			break;

		case WATER_TYPE_1_FB_REFLECTION:
			{
				//Normal frame buffer reflection water type. Non translucent.  Legacy code we're not using anymore.
				Matrix3D	OldCameraMatrix=rinfo.Camera.Get_Transform();
				Matrix4x4	FullMatrix4(rinfo.Camera.Get_Transform());	//copy 3x4 matrix into a 4x4
				Vector3		WaterNormal(0,0,1);	//normal of plane used for reflection
				Vector4		WaterPlane(WaterNormal.X,WaterNormal.Y,WaterNormal.Z,m_level);	//assume distance to origin 0
				Vector3		rRight,rUp,rN,rPos;	//orientation and translation vectors of camera

				Matrix4x4	FullMatrix(FullMatrix4.Transpose());	//swap rows/columns

				//reflect camera right vector
				Real axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[0],WaterNormal);
				rRight = (Vector3&)FullMatrix[0] - (2.0f*axis_distance*WaterNormal);

				//reflect camera up vector
				axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[1],WaterNormal);
				rUp = (Vector3&)FullMatrix[1] - (2.0f*axis_distance*WaterNormal);

				//reflect camera n vector
				axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[2],WaterNormal);
				rN = (Vector3&)FullMatrix[2] - (2.0f*axis_distance*WaterNormal);

				//reflect camera position
				axis_distance=Vector3::Dot_Product((Vector3&)FullMatrix[3],WaterNormal);	//distance cam to origin
				axis_distance -= WaterPlane.W;	// subtract mirror plane distance to get distance camera to plane
				rPos = (Vector3&)FullMatrix[3] - (2.0f*axis_distance*WaterNormal);

				//generate a new camera matrix from reflected vectors
				Matrix3D reflectedTransform(rRight,rUp,rN,rPos);

				//flip the winding order of polygons to draw the reflected back sides.
				ShaderClass::Invert_Backface_Culling(true);

			#ifdef CLIP_GEOMETRY_TO_PLANE
			  // Set a clip plane, so that only objects above the water are reflected
				WaterPlane.W *= -1.0f;	//flip sign of plane distance for clipping.


				// Alternate Clipping Method using alpha testing hack!
				/**************************************************************************************/

				//get current view matrix
				RenderMatrix4x4 curView;
				rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);

				//get inverse of view matrix(= view to world matrix)
				RenderMatrix4x4 inv;
				Real det;
				RenderMatrixInverse(&inv, &det, &curView);

				//create clipping matrix by inserting our plane equation into the 1st column
				RenderMatrix4x4 clipMatrix;
				RenderMatrixIdentity(&clipMatrix);
				clipMatrix.m[0][0]=WaterNormal.X;
				clipMatrix.m[1][0]=WaterNormal.Y;
				clipMatrix.m[2][0]=WaterNormal.Z;
				clipMatrix.m[3][0]=WaterPlane.W+0.5f;
				RenderMatrixMultiply(&inv, &inv, &clipMatrix);

				// Change texture wrapping mode to 'clamp' for texture stage 1
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

				// Use CameraSpace vertices as input to matrix and use texture wrap mode from stage 1
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION|1);
				// Two output coordinates are used.
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);

				// Set texture generation matrix for stage 1
				rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE1, &inv);

				// Disable bilinear filtering
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_POINT);

				// Pass stage 0 texture data untouched(by modulating with white)
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );	//stage 1 texture
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );	//previous stage texture
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );	//module with white => does nothing

				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );	//stage 1 texture
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );	//previous stage texture
				rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );	//modulate with clipping texture

				rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_REFERENCE,0x00);
				rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_FUNCTION,RENDER_COMPARE_NOT_EQUAL);	//pass pixels who's alpha is not zero
				rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_TEST_ENABLE, true);	//test pixels if transparent(clipped) before rendering.

				// Set clipping texture
				m_alphaClippingTexture->Set_U_Addr_Mode(TextureClass::TEXTURE_ADDRESS_CLAMP);
				m_alphaClippingTexture->Set_V_Addr_Mode(TextureClass::TEXTURE_ADDRESS_CLAMP);
				m_alphaClippingTexture->Set_Min_Filter(TextureClass::FILTER_TYPE_NONE);
				m_alphaClippingTexture->Set_Mag_Filter(TextureClass::FILTER_TYPE_NONE);
				m_alphaClippingTexture->Set_Mip_Mapping(TextureClass::FILTER_TYPE_NONE);

				rts::render::SetGameTexture(0,m_alphaClippingTexture);

				//TODO: Will have to make sure that the shader system is not resetting my stage 1 setup
				//while rendering the scene

				/*************************************************************************************/
			#endif

			#if 0	// No longer do simple rendering.
				if (TheGlobalData->m_useWaterPlane)
				{
					//@todo : Would it be better to create a new camera or change the transform of the
					//existing one?
					rinfo.Camera.Set_Transform( reflectedTransform );
					rinfo.Camera.Apply();	//force an update of all the camera dependent parameters like frustum clip planes

					if(m_useCloudLayer)
					{
						if (TheGlobalData && TheGlobalData->m_drawEntireTerrain)
							m_skyBox->Render(rinfo);
						else
						{
							renderSky();
							if (m_tod == TIME_OF_DAY_NIGHT)
								renderSkyBody(&reflectedTransform);
						}
					}

					WW3D::Render(m_parentScene,&rinfo.Camera);

					rinfo.Camera.Set_Transform(OldCameraMatrix);	//restore original non-reflected matrix
					rinfo.Camera.Apply();	//force an update of all the camera dependent parameters like frustum clip planes

					//clear the z-buffer to remove changes made by objects inside mirror
					const rts::render::GameRenderColor clearColor =
						{ 0.1f, 0.1f, 0.1f, 0.0f };
					rts::render::ClearGameRenderTargets(false, true, clearColor, 0.0f);
				}
			#endif

			#ifdef CLIP_GEOMETRY_TO_PLANE
				//restore default culling mode

				//disable texture coordinate generation
				rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_DISABLED);
				rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_TEST_ENABLE, false);	//disable alpha testing
			#endif

				ShaderClass::Invert_Backface_Culling(false);	//return culling back to normal

				ShaderClass::Invalidate();	//reset shading system so it forces full state set.

				renderWater();
			}
			break;

		default:
			break;
	}

	if (TheGlobalData && TheGlobalData->m_drawSkyBox)
	{	//center skybox around camera
		Vector3 pos=rinfo.Camera.Get_Position();
		pos.Z = TheGlobalData->m_skyBoxPositionZ;
		m_skyBox->Set_Position(pos);
		m_skyBox->Render(rinfo);
	}

	//Clean up after any pixel shaders.
	//Force the renderer to apply the null texture and release the shroud reference.
	rts::render::ApplyGameRenderStateChanges();
	rts::render::InvalidateGameRenderStateCache();

	if (m_waterTrackSystem)
		m_waterTrackSystem->flush(rinfo);

//	renderWaterMesh();
//	renderWaterWave();
}

//-------------------------------------------------------------------------------------------------
/** Clips the water plane to the current camera frustum and returns a bounding
	* box enclosing the clipped plane.  Returns false if water plane is not visible. */
//-------------------------------------------------------------------------------------------------
Bool WaterRenderObjClass::getClippedWaterPlane(CameraClass *cam, AABoxClass *box)
{
	const FrustumClass & frustum = cam->Get_Frustum();

	ClipPolyClass	ClippedPoly0;
	ClipPolyClass	ClippedPoly1;

	///@todo: generate proper sized polygon
	ClippedPoly0.Reset();
	ClippedPoly0.Add_Vertex(Vector3(0,0,m_level));
	ClippedPoly0.Add_Vertex(Vector3(0,m_dy,m_level));
	ClippedPoly0.Add_Vertex(Vector3(m_dx,m_dy,m_level));
	ClippedPoly0.Add_Vertex(Vector3(m_dx,0,m_level));

	//clip against all 6 frustum planes
	ClippedPoly0.Clip(frustum.Planes[0],ClippedPoly1);
	ClippedPoly1.Clip(frustum.Planes[1],ClippedPoly0);
	ClippedPoly0.Clip(frustum.Planes[2],ClippedPoly1);
	ClippedPoly1.Clip(frustum.Planes[3],ClippedPoly0);
	ClippedPoly0.Clip(frustum.Planes[4],ClippedPoly1);
	ClippedPoly1.Clip(frustum.Planes[5],ClippedPoly0);

	Int final_vcount = ClippedPoly0.Verts.Count();

	//make sure the polygon is visible
	if (final_vcount >= 3)
	{
		//find axis aligned bounding box around visible polygon
		if (box)
  			box->Init(&(ClippedPoly0.Verts[0]),final_vcount);
		return TRUE;
	}

	return FALSE;	//water plane is not visible
}

//-------------------------------------------------------------------------------------------------
/** Draws the water surface using a custom vertex/pixel shader and a
	* reflection texture.  Only tested to work on GeForce3. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::drawSea(RenderInfoClass & rinfo)
{
	const Int bumpFrame = static_cast<Int>(m_fBumpFrame);
	if (m_pReflectionTexture == nullptr ||
		m_vertexBuffer == nullptr || !m_vertexBuffer->Is_Valid() ||
		m_waterIndexBuffer == nullptr || !m_waterIndexBuffer->Is_Valid() ||
		m_waveVertexShader == 0 || m_wavePixelShader == 0 ||
		m_numIndices < 3 || m_numVertices <= 0 || m_numVertices > 0xffff ||
		bumpFrame < 0 || bumpFrame >= NUM_BUMP_FRAMES ||
		m_pBumpTexture[bumpFrame] == nullptr)
		return;

	AABoxClass	seaBox;

	if (!getClippedWaterPlane(&rinfo.Camera,&seaBox))
		return;	//the sea is not visible

	LegacyLogicalState previousState;
	const bool havePreviousState =
		rts::render::GetTrackedLegacyLogicalState(&previousState);
	const RenderTextureAddressMode previousStage0AddressU = havePreviousState ?
		previousState.pipeline.textureStages[0].sampler.addressU :
		RENDER_TEXTURE_ADDRESS_WRAP;
	const RenderTextureAddressMode previousStage0AddressV = havePreviousState ?
		previousState.pipeline.textureStages[0].sampler.addressV :
		RENDER_TEXTURE_ADDRESS_WRAP;
	const RenderTextureAddressMode previousStage1AddressU = havePreviousState ?
		previousState.pipeline.textureStages[1].sampler.addressU :
		RENDER_TEXTURE_ADDRESS_WRAP;
	const RenderTextureAddressMode previousStage1AddressV = havePreviousState ?
		previousState.pipeline.textureStages[1].sampler.addressV :
		RENDER_TEXTURE_ADDRESS_WRAP;

	RenderMatrix4x4 matProj, matView, matWW3D;

	//create a transform which will flip the y and z coordinates to fit our system
	memset(&matWW3D,0,sizeof(RenderMatrix4x4));
	matWW3D.m[0][0]=1.0f;
	matWW3D.m[2][1]=1.0f;
	matWW3D.m[1][2]=1.0f;
	matWW3D.m[3][3]=1.0f;

	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,Transform);	//position the water surface
	rts::render::SetGameTexture(0,nullptr);	//we'll be setting our own textures, so reset W3D
	rts::render::SetGameTexture(1,nullptr);	//we'll be setting our own textures, so reset W3D


	rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

	Vector3 camTran;

	rinfo.Camera.Get_Transform().Get_Translation(&camTran);

	rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &matView);
	rts::render::GetGameTransform(GAME_TRANSFORM_PROJECTION, &matProj);

	//default setup from Kenny's demo
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_DIFFUSE);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);

	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_MODULATE);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COORDINATE_INDEX, 1);

	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
		GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_COORDINATE_INDEX,
		GAME_TEXTURE_COORDINATE_PASSTHROUGH|2);

	rts::render::SetGameTextureStageState(3, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
		GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState(3, GAME_TEXTURE_STAGE_COORDINATE_INDEX,
		GAME_TEXTURE_COORDINATE_PASSTHROUGH|3);

	//end of default setup

	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_CLAMP);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_CLAMP);

	rts::render::SetGameTexture(0, m_pBumpTexture[bumpFrame]);
#ifdef MIPMAP_BUMP_TEXTURE
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MIP_FILTER, RENDER_TEXTURE_FILTER_POINT);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
#endif
	rts::render::SetGameTextureBumpEnvironment(1, m_fBumpScale, 0.0f,
		0.0f, m_fBumpScale);

	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);

	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE, FALSE);

	RenderMatrix4x4 mat;
	memset(&mat,0,sizeof(RenderMatrix4x4));

	mat.m[0][0] = 0.5f; mat.m[0][1] = -0.5f; mat.m[0][2] = 0.5f;   mat.m[0][3]=0.5f;
	mat.m[1][0] = 0.5f; mat.m[1][1] = 0.5f; mat.m[1][2] = 0.0f;   mat.m[1][3]=0.0f;
	mat.m[2][0] = 0.0f; mat.m[2][1] = 0.0f; mat.m[2][2] = 0.0f;   mat.m[2][3]=1.0f;
	mat.m[3][0] = 0.0f; mat.m[3][1] = 0.0f; mat.m[3][2] = 0.0f;   mat.m[3][3]=1.0f;

	rts::render::SetGameVertexShaderConstant(CV_TEXPROJ_0, &mat, 4);

	// Setup constants
	Vector4 zero(0.0f, 0.0f, 0.0f, 0.0f);
	Vector4 one(1.0f, 1.0f, 1.0f, 1.0f);
	rts::render::SetGameVertexShaderConstant(CV_ZERO, &zero, 1);
	rts::render::SetGameVertexShaderConstant(CV_ONE, &one, 1);

	rts::render::SetGameVertexShader(m_waveVertexShader);
	rts::render::SetGamePixelShader(m_wavePixelShader);
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_WATER_SEA);
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_WATER_SEA);

	rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND, RENDER_BLEND_SOURCE_ALPHA);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND, RENDER_BLEND_INVERSE_SOURCE_ALPHA);

	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE, TRUE);
	rts::render::SetGameTexture(1, m_pReflectionTexture);

	Int patchX,patchY,startX,startY;

	RenderMatrix4x4 patchMatrix;
	memset(&patchMatrix,0,sizeof(RenderMatrix4x4));
	patchMatrix.m[0][0]=PATCH_SCALE;
	patchMatrix.m[1][1]=1.0f;
	patchMatrix.m[2][2]=PATCH_SCALE;
	patchMatrix.m[3][3]=1.0f;

	rts::render::SetGameVertexBuffer(m_vertexBuffer);
	rts::render::SetGameIndexBuffer(m_waterIndexBuffer, 0);

	for (startY=patchY=(seaBox.Center.Y-seaBox.Extent.Y)/(PATCH_WIDTH*PATCH_SCALE); (patchY*PATCH_WIDTH*PATCH_SCALE)<(seaBox.Center.Y+seaBox.Extent.Y); patchY++)
	{
		for (startX=patchX=(seaBox.Center.X-seaBox.Extent.X)/(PATCH_WIDTH*PATCH_SCALE); (patchX*PATCH_WIDTH*PATCH_SCALE)<(seaBox.Center.X+seaBox.Extent.X); patchX++)
		{
			RenderMatrix4x4 matWorldViewProj, matWorldView, matTempWorld;
			patchMatrix.m[3][0]=(float)(patchX*PATCH_WIDTH*PATCH_SCALE );
			patchMatrix.m[3][2]=(float)(patchY*PATCH_WIDTH*PATCH_SCALE );
			//convert the default renderer coordinate system into ours
				RenderMatrixMultiply(&matTempWorld, &patchMatrix, &matWW3D);

				RenderMatrixMultiply(&matWorldView, &matTempWorld, &matView);
				RenderMatrixMultiply(&matWorldViewProj, &matWorldView, &matProj);
			// The wave vertex shader receives the per-patch WVP in c2-c5,
			// so its fog depth must use the matching per-patch world/view
			// transform rather than the global logical WorldView.
				RenderMatrixTranspose(&matWorldView, &matWorldView);
			rts::render::SetGameVertexShaderConstant(CV_SEA_WAVE_WORLDVIEW_0,
				&matWorldView, 4);
			//matrices must be transposed before loading into vertex shader registers
				RenderMatrixTranspose(&matWorldViewProj, &matWorldViewProj);
			rts::render::SetGameVertexShaderConstant(CV_WORLDVIEWPROJ_0, &matWorldViewProj, 4);	//pass transform matrix into shader

			rts::render::DrawGameStrip(
				0, static_cast<unsigned short>(m_numIndices - 2), 0,
				static_cast<unsigned short>(m_numVertices));
		}
	}
	rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE, FALSE);
	rts::render::SetGameTexture(0, nullptr);	//release reference to bump texture
	rts::render::SetGameTexture(1, nullptr);	//release reference to reflection texture
	rts::render::SetGameTexture(2, nullptr);	//release reference to reflection texture

	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
		GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_COORDINATE_INDEX,
		GAME_TEXTURE_COORDINATE_PASSTHROUGH|0);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
		GAME_TEXTURE_TRANSFORM_DISABLED);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COORDINATE_INDEX,
		GAME_TEXTURE_COORDINATE_PASSTHROUGH|1);
	rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_WRITE, TRUE);

	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_ADDRESS_U,
		previousStage0AddressU);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_ADDRESS_V,
		previousStage0AddressV);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_ADDRESS_U,
		previousStage1AddressU);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_ADDRESS_V,
		previousStage1AddressV);

	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_COLOR_OPERATION, RENDER_TEXTURE_OP_DISABLE);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_ALPHA_OPERATION, RENDER_TEXTURE_OP_DISABLE);

	//Restore old transforms
	rts::render::SetGameTransform(GAME_TRANSFORM_VIEW, &matView);
	rts::render::SetGameTransform(GAME_TRANSFORM_PROJECTION, &matProj);

	rts::render::SetGamePixelShader(0);	//turn off pixel shader
	rts::render::SetGameVertexShader(rts::render::GAME_VERTEX_XYZDUV1);	//turn off custom vertex shader
	rts::render::SetGameLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	rts::render::SetGameLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);

	rts::render::InvalidateGameRenderStateCache();

	if (TheTerrainRenderObject->getShroud())
	{
		//do second pass to apply the shroud on water plane
		W3DShaderManager::setTexture(0,TheTerrainRenderObject->getShroud()->getShroudTexture());
		W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 0);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
		rts::render::SetGameLegacyVertexProgram(
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
		rts::render::SetGameVertexBuffer(m_vertexBuffer);
		rts::render::SetGameIndexBuffer(m_waterIndexBuffer, 0);
		for (startY=patchY=(seaBox.Center.Y-seaBox.Extent.Y)/(PATCH_WIDTH*PATCH_SCALE); (patchY*PATCH_WIDTH*PATCH_SCALE)<(seaBox.Center.Y+seaBox.Extent.Y); patchY++)
		{
			for (startX=patchX=(seaBox.Center.X-seaBox.Extent.X)/(PATCH_WIDTH*PATCH_SCALE); (patchX*PATCH_WIDTH*PATCH_SCALE)<(seaBox.Center.X+seaBox.Extent.X); patchX++)
			{
				RenderMatrix4x4 matTemp;
				patchMatrix.m[3][0]=(float)(patchX*PATCH_WIDTH*PATCH_SCALE);
				patchMatrix.m[3][2]=(float)(patchY*PATCH_WIDTH*PATCH_SCALE);

				RenderMatrixMultiply(&matTemp, &patchMatrix, &matWW3D);

				rts::render::SetGameTransform(GAME_TRANSFORM_WORLD, &matTemp);

				rts::render::DrawGameStrip(
					0, static_cast<unsigned short>(m_numIndices - 2), 0,
					static_cast<unsigned short>(m_numVertices));
			}
		}
		W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
	}

}


#define FEATHER_LAYER_COUNT (5.0f)
#define FEATHER_THICKNESS   (4.0f)

//-------------------------------------------------------------------------------------------------
/** Renders (draws) the water surface.*/
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::renderWater()
{
	for (PolygonTrigger *pTrig=PolygonTrigger::getFirstPolygonTrigger(); pTrig; pTrig = pTrig->getNext()) {
		if (pTrig->isWaterArea()) {
			if (pTrig->getNumPoints()>2) {
				if (pTrig->isRiver()) {
					drawRiverWater(pTrig);
					continue;
				}
				Int k;
				for (k=1; k<pTrig->getNumPoints()-1; k=k+2) {
					ICoord3D pt3 = *pTrig->getPoint(0);
					ICoord3D pt2 = *pTrig->getPoint(k);
					ICoord3D pt1 = *pTrig->getPoint(k+1);
					ICoord3D pt0 = *pTrig->getPoint(k+1);
					if (k+2<pTrig->getNumPoints()) {
						pt0 = *pTrig->getPoint(k+2);
					}
					Vector3 points[4];
					points[0].Set(pt0.x, pt0.y, pt0.z);
					points[1].Set(pt1.x, pt1.y, pt1.z);
					points[2].Set(pt2.x, pt2.y, pt2.z);
					points[3].Set(pt3.x, pt3.y, pt3.z);

					if ( TheGlobalData->m_featherWater )
					{
						for (int r = 0; r < TheGlobalData->m_featherWater; ++r)
						{
							drawTrapezoidWater(points);
							points[0].Z += (FEATHER_THICKNESS/TheGlobalData->m_featherWater);
						}
					}

					else
						drawTrapezoidWater(points);


				}
			}
		}
	}

}

//-------------------------------------------------------------------------------------------------
/** Renders (draws) the sky plane.  Will apply current time-of-day settings including
	* some simple UV scrolling animation. */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::renderSky()
{
	Int timeNow,timeDiff;
	Real fu,fv;

	Setting *setting=&m_settings[m_tod];

	timeNow=timeGetTime();

	timeDiff=timeNow-m_LastUpdateTime;
	m_LastUpdateTime=timeNow;

	m_uOffset += timeDiff * setting->uScrollPerMs * setting->skyTexelsPerUnit;
	m_vOffset += timeDiff * setting->vScrollPerMs * setting->skyTexelsPerUnit;

	//clamp uv coordinate into 0,1 range
	m_uOffset = m_uOffset - (Real)((Int) m_uOffset);
	m_vOffset = m_vOffset - (Real)((Int) m_vOffset);

	fu= m_uOffset + (SKYPLANE_SIZE * 2) * setting->skyTexelsPerUnit;
	fv= m_vOffset + (SKYPLANE_SIZE * 2) * setting->skyTexelsPerUnit;


	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);

	ShaderClass m_shader2=ShaderClass::_PresetOpaqueShader;
	m_shader2.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
	m_shader2.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);	//no need to check against z-buffer, sky always rendered first.
	m_shader2.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);	//sky is always behind everything so no need to update z-buffer

	rts::render::SetGameShader(m_shader2);

	rts::render::SetGameTexture(0,setting->skyTexture);

	//draw an infinite sky plane
	DynamicVBAccessClass vb_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,dynamic_fvf_type,4);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();
		if(verts)
		{
			verts[0].x=-SKYPLANE_SIZE;
			verts[0].y=SKYPLANE_SIZE;
			verts[0].z=SKYPLANE_HEIGHT;
			verts[0].u1=m_uOffset;
			verts[0].v1=fv;
			verts[0].diffuse=setting->vertex01Diffuse;

			verts[1].x=SKYPLANE_SIZE;
			verts[1].y=SKYPLANE_SIZE;
			verts[1].z=SKYPLANE_HEIGHT;
			verts[1].u1=fu;
			verts[1].v1=fv;
			verts[1].diffuse=setting->vertex11Diffuse;

			verts[2].x=SKYPLANE_SIZE;
			verts[2].y=-SKYPLANE_SIZE;
			verts[2].z=SKYPLANE_HEIGHT;
			verts[2].u1=fu;
			verts[2].v1=m_vOffset;
			verts[2].diffuse=setting->vertex10Diffuse;

			verts[3].x=-SKYPLANE_SIZE;
			verts[3].y=-SKYPLANE_SIZE;
			verts[3].z=SKYPLANE_HEIGHT;
			verts[3].u1=m_uOffset;
			verts[3].v1=m_vOffset;
			verts[3].diffuse=setting->vertex00Diffuse;
		}
	}

	rts::render::SetGameIndexBuffer(m_indexBuffer,0);
	rts::render::SetGameVertexBuffer(vb_access);

	Matrix3D tm(1);
	tm.Set_Translation(Vector3(0,0,0));
	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,tm);

	rts::render::DrawGameTriangles(	0,2, 0,	4);	//draw a quad, 2 triangles, 4 verts
}

//-------------------------------------------------------------------------------------------------
/** Renders (draws) the sky body.  Used for moon and sun.  We rotate the image
	* so that it always faces the camera.  This removes perspective and helps hide that
	* it's a flat image. */
//-------------------------------------------------------------------------------------------------
///	@todo: Add code to render properly sorted sun sky body.
void WaterRenderObjClass::renderSkyBody(Matrix3D *mat)
{
	Vector3 cPos;

	Vector3 pView,pRight,pUp,pPos(SKYBODY_X,SKYBODY_Y,SKYBODY_HEIGHT);

	mat->Get_Translation(&cPos);

	pView=cPos-pPos;	//billboard to camera
	pView.Normalize();	//particle view direction

	Vector3 WorldUp(0,0,-1);	///@todo: hacked so only works for reflections across xy plane

#ifdef ALLOW_TEMPORARIES
	Vector3 rotAxis=Vector3::Cross_Product(WorldUp,pView);	//get axis of rotation.
	rotAxis.Normalize();
#else
	Vector3 rotAxis;
	Vector3::Normalized_Cross_Product(WorldUp, pView, &rotAxis);
#endif

	Real angle=Vector3::Dot_Product(WorldUp,pView);

	angle = acos(angle);


	Matrix3D tm(1);
	tm.Set(rotAxis,angle);
	tm.Adjust_Translation(Vector3(SKYBODY_X,SKYBODY_Y,SKYBODY_HEIGHT));


	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,tm);


	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);

	ShaderClass m_shader2=ShaderClass::_PresetAlphaShader;
	m_shader2.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
	m_shader2.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);	//no need to check against z-buffer, sky always rendered first.
	m_shader2.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);	//sky is always behind everything so no need to update z-buffer

	rts::render::SetGameShader(m_shader2);


//	rts::render::SetGameShader(ShaderClass::/*_PresetAdditiveShader*//*_PresetOpaqueShader*/_PresetAlphaShader);
//	rts::render::SetGameTexture(0,setting->skyBodyTexture);

	rts::render::SetGameTexture(0,m_alphaClippingTexture);

	//draw an infinite sky plane
	DynamicVBAccessClass vb_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,dynamic_fvf_type,4);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();
		if(verts)
		{
			verts[0].x=-SKYBODY_SIZE;
			verts[0].y=SKYBODY_SIZE;
			verts[0].z=0;
			verts[0].u2=0;
			verts[0].v2=1;
			verts[0].diffuse=0xffffffff;

			verts[1].x=SKYBODY_SIZE;
			verts[1].y=SKYBODY_SIZE;
			verts[1].z=0;
			verts[1].u2=1;
			verts[1].v2=1;
			verts[1].diffuse=0xffffffff;

			verts[2].x=SKYBODY_SIZE;
			verts[2].y=-SKYBODY_SIZE;
			verts[2].z=0;
			verts[2].u2=1;
			verts[2].v2=0;
			verts[2].diffuse=0xffffffff;

			verts[3].x=-SKYBODY_SIZE;
			verts[3].y=-SKYBODY_SIZE;
			verts[3].z=0;
			verts[3].u2=0;
			verts[3].v2=0;
			verts[3].diffuse=0xffffffff;
		}
	}

	rts::render::SetGameIndexBuffer(m_indexBuffer,0);
	rts::render::SetGameVertexBuffer(vb_access);

	rts::render::DrawGameTriangles(	0,2, 0,	4);	//draw a quad, 2 triangles, 4 verts
}

//Defines for procedural water animation.
#define WATER_FREQ	(2.0*3.2831/4.0)	//2pi (full cycle) cover 4 units
#define WATER_AMP	(1.0f)
#define	WATER_OFFSET (0.1f)

//-------------------------------------------------------------------------------------------------
/** Renders (draws) the water surface mesh geometry.
	*	This is a work-in-progress!  Do not use this code! */
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::renderWaterMesh()
{

	if (!m_doWaterGrid)
		return;	//the water grid is disabled.

	//Some drivers require a fresh dynamic vertex-buffer page each frame, so we
	//force a DISCARD by overflowing the counter.
	m_vertexBufferOffset = 0xffff;

	Setting *setting=&m_settings[m_tod];

	WaterMeshData *pData;
	Int	mx=m_gridCellsX+1;
	Int my=m_gridCellsY+1;
	Int i,j;

	Real cellSizeX=m_gridCellSize;
	Real cellSizeY=m_gridCellSize;
//	Real	uScale2=5.0f*setting->waterRepeatCount/(128.0f)*cellSizeX/10.0f;
//	Real	vScale2=5.0f*setting->waterRepeatCount/(128.0f)*cellSizeY/10.0f;

	//Old waterRepeatCount settings in INI were based on 128x128 water grid of cellsize=10
	//Scale values to correct size.
	Real	uScale=setting->waterRepeatCount/(128.0f)*cellSizeX/10.0f*0.2f;
	Real	vScale=setting->waterRepeatCount/(128.0f)*cellSizeY/10.0f*0.2f;

	Vector3	nx(cellSizeX*2.0f,0,0);
	Vector3 ny(0,cellSizeY*2.0f,0);
	Vector3 C;

#ifdef DO_WATER_SIMULATION		//Debug code used to create a dummy water animation
	//
	// Mark: If you re-enable this water simulation, you might want to consider moving
	// this code to the update() method of the water render object (Colin)
	//

	static Real PhasePerFrameX=0.1f;
	static Real PhasePerFrameY=0.1f;

	//update the mesh heights for this frame (update buffer is 2 samples wider/taller due to border)
	for (j=0,pData=m_meshData; j<(my+2); j++)
	{
		for (i=0; i<(mx+2); i++)
		{
			//*pData = WATER_AMP * sin(WATER_FREQ*(0.7f*i + 0.7f*j) - PhasePerFrame);

			pData->height=WATER_OFFSET+WATER_AMP*(sin((float)i*WATER_FREQ*0.4+PhasePerFrameX*0.5)+sin((float)i*WATER_FREQ*0.6+PhasePerFrameX*0.2)+sin((float)j*WATER_FREQ+PhasePerFrameX)+sin((float)j*WATER_FREQ*0.7+PhasePerFrameX*0.3));
//			*pData=WATER_OFFSET+WATER_AMP*(sin((float)i*WATER_FREQ*0.4+PhasePerFrameX*0.5)+sin((float)i*WATER_FREQ*0.6+PhasePerFrameX*0.2)+sin((float)j*WATER_FREQ+PhasePerFrameX)+sin((float)j*WATER_FREQ*0.7+PhasePerFrameX*0.3));
			pData++;
		}
	}

	PhasePerFrameX -= 0.08f;
	PhasePerFrameY -= 0.1f;
#endif

	Int diffuse;
	diffuse = setting->waterDiffuse&0x00ffffff;
	Int alpha = (setting->waterDiffuse & 0xff000000)>>24;
	// Reduce alpha for wave mesh
	alpha -= 0x20;
	diffuse |= alpha<<24;

	//I pulled some of these constants out of the loops for speed:
	Real uvCosScale=0.02*cos(3*m_riverVOrigin);
	Real sinOffset=25*m_riverVOrigin;
	Real originScale=m_riverVOrigin/vScale;
	Real bumpSizeDiv=cellSizeY/BUMP_SIZE;
	Real bumpSizeDiv2=0.3f*cellSizeY/BUMP_SIZE;

	// Snapshot the live mesh and global sine-table samples before worker admission.
	// No GPU buffer is locked until all CPU preparation has joined.
	WaterMeshBatch &preparedMesh = m_preparedMesh;
	bool meshPrepared = false;
#ifndef USE_MESH_NORMALS
	if (rts::UseParallelPipelines() && mx > 0 && my > 0 && preparedMesh.initialize(mx, my))
	{
		WaterMeshSnapshot &snapshot = preparedMesh.snapshot();
		snapshot.cellSizeX = cellSizeX;
		snapshot.uScale = uScale;
		snapshot.diffuse = diffuse;
		for (j=0,pData=m_meshData+mx+2+1; j<my; j++,pData+=2)
		{
			const Real y=(float)j*cellSizeY;
			WaterMeshRowInput &row = preparedMesh.rows()[j];
			row.y = y;
#ifdef SCROLL_UV
			row.v1 = m_riverVOrigin+(float)j*vScale + uvCosScale*WWMath::Fast_Sin(sinOffset+y*PI/(8*MAP_XY_FACTOR));
#else
			row.v1 = (float)j*vScale;
#endif
			row.v2 = ((float)j+originScale)*bumpSizeDiv + (float)j*bumpSizeDiv2;
			for (i=0; i<mx; ++i,++pData)
				preparedMesh.heights()[j*mx+i] = pData->height;
		}
		meshPrepared = preparedMesh.run();
	}
#endif

	MaterMeshVertexFormat *vb = nullptr;
	const bool appendToBuffer = m_vertexBufferOffset < m_numVertices;
	const size_t lockOffset = appendToBuffer ?
		static_cast<size_t>(m_vertexBufferOffset) * sizeof(MaterMeshVertexFormat) : 0;
	const int lockFlags = appendToBuffer ?
		NATIVE_BUFFER_LOCK_NO_OVERWRITE : NATIVE_BUFFER_LOCK_DISCARD;
	void *lockedData = nullptr;
	if (!m_vertexBuffer->Lock_Buffer(lockOffset,
		static_cast<size_t>(mx) * my * sizeof(MaterMeshVertexFormat),
		lockFlags, &lockedData) || lockedData == nullptr)
		return;
	vb = static_cast<MaterMeshVertexFormat *>(lockedData);
	if (!appendToBuffer)
		m_vertexBufferOffset = 0;
	if (meshPrepared)
	{
#ifndef USE_MESH_NORMALS
		typedef char WaterMeshVertexLayoutMustMatch[
			sizeof(WaterMeshVertex) == sizeof(MaterMeshVertexFormat) ? 1 : -1];
		memcpy(vb, preparedMesh.output(), static_cast<size_t>(mx)*my*sizeof(MaterMeshVertexFormat));
#endif
	}
	else
	{
	//Data has a 1 vertex padding all around it so we don't need to special-case edges.  Improves performance
	for (j=0,pData=m_meshData+mx+2+1; j<my; j++,pData+=2)	//skip 2 horizontal border samples after each row
	{
		Real y=(float)j*cellSizeY;
		Real v1Offset=m_riverVOrigin+(float)j*vScale + uvCosScale*WWMath::Fast_Sin(sinOffset+y*PI/(8*MAP_XY_FACTOR));
		Real v2Offset=((float)j+originScale)*bumpSizeDiv + (float)j*bumpSizeDiv2;

		for (i=0; i<mx; i++)
		{
			//compute normal by looking at 4 vertex neightbors
#ifdef USE_MESH_NORMALS
			nx.Z=(pData+1)->height - (pData-1)->height;
			ny.Z=(pData+mx+2)->height - (pData-mx-2)->height;
//			nx.Z=*(pData+1)-*(pData-1);
//			ny.Z=*(pData+mx+2)-*(pData-mx-2);
			Vector3::Cross_Product(nx,ny,&C);
			C.Normalize();
			vb->nx = C.X;
			vb->ny = C.X;
			vb->nz = C.X;
#endif
			Real x = (float)i*cellSizeX;
			vb->x=	x;
			vb->y=	y;
			vb->z=  pData->height;//WATER_OFFSET+WATER_AMP*(sin((float)i*WATER_FREQ+PhasePerFrame)+cos((float)j*WATER_FREQ+PhasePerFrame));

			vb->diffuse = diffuse;
#ifdef SCROLL_UV
//			vb->diffuse=0x80ffffff;
			vb->u1=(float)i*uScale;
			vb->v1=v1Offset;

			//old slow version
			//vb->v1=m_riverVOrigin+(float)j*vScale + 0.02*cos(3*m_riverVOrigin)*sin(25*m_riverVOrigin+y*PI/(8*MAP_XY_FACTOR));

//			vb->u2=m_initialGridU2+(float)i*uScale2;
//			vb->v2=m_initialGridV2+(float)j*vScale2;
#else
			vb->u1=(float)i*uScale;
			vb->v1=(float)j*vScale;
#endif
			vb->u2=(float)(i)*cellSizeX/BUMP_SIZE;
			vb->v2=v2Offset;
			//old slow code
			//vb->v2=(float)(j+m_riverVOrigin/vScale )*cellSizeY/BUMP_SIZE+ 0.3f*(float)j*cellSizeY/BUMP_SIZE;
			vb++;
			pData++;
		}
	}
	}

	if (!m_vertexBuffer->Unlock_Buffer())
		return;

	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,Transform);	//position the water surface
	rts::render::SetGameMaterial(m_meshVertexMaterialClass);

	ShaderClass::CullModeType oldCullMode=m_shaderClass.Get_Cull_Mode();

	ShaderClass::DepthMaskType oldDepthMask=m_shaderClass.Get_Depth_Mask();
	m_shaderClass.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);	//disable writing to z-buffer to prevent particle clipping.

	m_shaderClass.Set_Cull_Mode(ShaderClass::CULL_MODE_ENABLE);	//water should be visible from both sides

	rts::render::SetGameShader(m_shaderClass);
	setupFlatWaterShader();

	rts::render::SetGameIndexBuffer(m_waterIndexBuffer,
		static_cast<unsigned short>(m_vertexBufferOffset));
	rts::render::SetGameVertexBuffer(m_vertexBuffer);
	rts::render::SetGameVertexShader(WATER_MESH_FVF);


	if (TheTerrainRenderObject->getShroud() && !m_trapezoidWaterPixelShader)
	{	//we have a shroud to apply and can't do it inside the pixel shader.
		//so do it in stage1
		W3DShaderManager::setTexture(0,TheTerrainRenderObject->getShroud()->getShroudTexture());
		W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 1);

		//modulate with shroud texture
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT1, GAME_TEXTURE_ARGUMENT_TEXTURE );	//stage 1 texture
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_ARGUMENT2, GAME_TEXTURE_ARGUMENT_CURRENT );	//previous stage texture
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_COLOR_OPERATION,   RENDER_TEXTURE_OP_MODULATE );
		rts::render::SetGameTextureStageState( 1, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_MODULATE );

		//Shroud shader uses z-compare of EQUAL which wouldn't work on water because it doesn't
		//write to the zbuffer.  Change to LESSEQUAL.
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION, RENDER_COMPARE_LESS_EQUAL);
		rts::render::DrawGameStrip(
			0, static_cast<unsigned short>(m_numIndices - 2), 0,
			static_cast<unsigned short>(mx * my));
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION, RENDER_COMPARE_EQUAL);
		W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
	}
	else
		rts::render::DrawGameStrip(
			0, static_cast<unsigned short>(m_numIndices - 2), 0,
			static_cast<unsigned short>(mx * my));

	if (m_trapezoidWaterPixelShader)
	{
		rts::render::SetGamePixelShader(0);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	}

	m_vertexBufferOffset += mx*my;	//advance past vertices already in buffer

	rts::render::SetGameTexture(0,nullptr);
	rts::render::SetGameTexture(1,nullptr);
	ShaderClass::Invalidate();
	m_shaderClass.Set_Cull_Mode(oldCullMode);	//water should be visible from both sides

	// restore shader to old mask
	m_shaderClass.Set_Depth_Mask(oldDepthMask);

	//W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);

}

inline void WaterRenderObjClass::setGridVertexHeight(Int x, Int y, Real value)
{
	DEBUG_ASSERTCRASH( x < (m_gridCellsX+1) && y < (m_gridCellsY+1), ("Invalid Water Mesh Coordinates") );

	if (m_meshData)
	{
		m_meshData[(y+1)*(m_gridCellsX+1+2)+x+1].height = value;
	}
}

void WaterRenderObjClass::setGridHeightClamps(Real minz, Real maxz)
{
	m_minGridHeight = minz;
	m_maxGridHeight = maxz;
}

void WaterRenderObjClass::addVelocity( Real worldX, Real worldY,
																			 Real zVelocity, Real preferredHeight )
{

	if( m_doWaterGrid)
	{
		Real gx,gy;
		Real minX,maxX,minY,maxY;
		Int x,y;
		WaterMeshData *meshPoint;
		m_disableRiver = true;

		//check if center falls within grid bounds
		if (worldToGridSpace(worldX, worldY, gx, gy))
		{

			//find extents of influence
			minX = floorf(gx - m_gridChangeMaxRange);
			if (minX < 0 )
				minX = 0;	//clamp extent to fall within box
			maxX = ceilf(gx + m_gridChangeMaxRange);
			if (maxX > m_gridCellsX)
				maxX = m_gridCellsX;	//clamp extent to fall within box

			minY = floorf(gy - m_gridChangeMaxRange);
			if (minY < 0 )
				minY = 0;	//clamp extent to fall within box
			maxY = ceilf(gy + m_gridChangeMaxRange);
			if (maxY > m_gridCellsY)
				maxY = m_gridCellsY;	//clamp extent to fall within box

			for (y=minY; y<=maxY; y++)
			{
				for (x=minX; x<=maxX; x++)
				{

					// get the mesh point that we're concerned with
					meshPoint = &m_meshData[ (y + 1) * (m_gridCellsX + 1 + 2) + x + 1 ];

					// we now have a new preferred height
					meshPoint->preferredHeight = preferredHeight;

					//
					// set the velocity of this point based on the distance from the center of the
					// "core" point for this call
					//
					meshPoint->velocity = meshPoint->velocity + zVelocity;

					// this point is now "in motion"
					BitSet( meshPoint->status, WaterRenderObjClass::IN_MOTION );

				}
			}

			//
			// the mesh data is now dirty, we need to pass through the velocity field
			// during an update phase to update the positions
			//
			m_meshInMotion = TRUE;

		}

	}

}

void WaterRenderObjClass::changeGridHeight(Real wx, Real wy, Real delta)
{
	Real gx,gy;
	Real *oldData;
	Real newData;
	Real distance;
	Real minX,maxX,minY,maxY;
	Int x,y;

	//check if center falls within grid bounds
	if (worldToGridSpace(wx, wy, gx, gy))
	{	//find extents of influence
		minX = floorf(gx - m_gridChangeMaxRange);
		if (minX < 0 )
			minX = 0;	//clamp extent to fall within box
		maxX = ceilf(gx + m_gridChangeMaxRange);
		if (maxX > m_gridCellsX)
			maxX = m_gridCellsX;	//clamp extent to fall within box

		minY = floorf(gy - m_gridChangeMaxRange);
		if (minY < 0 )
			minY = 0;	//clamp extent to fall within box
		maxY = ceilf(gy + m_gridChangeMaxRange);
		if (maxY > m_gridCellsY)
			maxY = m_gridCellsY;	//clamp extent to fall within box

		for (y=minY; y<=maxY; y++)
		{
			for (x=minX; x<=maxX; x++)
			{	oldData = &m_meshData[(y+1)*(m_gridCellsX+1+2)+x+1].height;
				distance = (gx - (Real)x)*(gx - (Real)x) + (gy - (Real)y)*(gy - (Real)y);
				distance = sqrt(distance);
				newData = *oldData + 1.0f/(m_gridChangeAtt0+m_gridChangeAtt1*distance+distance*distance*m_gridChangeAtt2)*delta;
				//Clamp to min/max values
				if (newData < m_minGridHeight)
					newData = m_minGridHeight;
				if (newData > m_maxGridHeight)
					newData = m_maxGridHeight;
				*oldData = newData;
			}
		}
	}
}

void WaterRenderObjClass::setGridChangeAttenuationFactors(Real a, Real b, Real c, Real range)
{
	m_gridChangeAtt0 = a;
	m_gridChangeAtt1 = b;
	m_gridChangeAtt2 = c;
	m_gridChangeMaxRange = range/m_gridCellSize;	//convert range to grid space
}

void WaterRenderObjClass::setGridTransform(Real angle, Real x, Real y, Real z)
{
	m_gridDirectionX = Vector2(1.0f,0.0f);

	m_gridOrigin.X = x;
	m_gridOrigin.Y = y;

	Matrix3D xform(1);
	xform.Rotate_Z(angle);

	m_gridDirectionX.X = xform.Get_X_Vector().X;
	m_gridDirectionX.Y = xform.Get_X_Vector().Y;

	m_gridDirectionY.X = xform.Get_Y_Vector().X;
	m_gridDirectionY.Y = xform.Get_Y_Vector().Y;

	xform.Set_Translation(Vector3(x,y,z));
	Set_Transform(xform);
}

void WaterRenderObjClass::setGridTransform(const Matrix3D *transform )
{

	if( transform )
		Set_Transform( *transform );

}

void WaterRenderObjClass::getGridTransform(Matrix3D *transform )
{

	if( transform )
		*transform = Get_Transform();

}

void WaterRenderObjClass::setGridResolution(Real gridCellsX, Real gridCellsY, Real cellSize)
{
	m_gridCellSize=cellSize;

	if (m_gridCellsX != gridCellsX || m_gridCellsY != gridCellsY)
	{	//resolution has changed
		m_gridCellsX=gridCellsX;
		m_gridCellsY=gridCellsY;

		if (m_meshData)
		{

			delete [] m_meshData;//free previously allocated grid and allocate new size
			m_meshData = nullptr;	 // must set to null so that we properly re-allocate
			m_meshDataSize = 0;

			Bool enable = m_doWaterGrid;
			enableWaterGrid(true);	// allocates buffers.
			m_doWaterGrid = enable;

		}
	}
}

void WaterRenderObjClass::getGridResolution( Real *gridCellsX, Real *gridCellsY, Real *cellSize )
{

	if( gridCellsX )
		*gridCellsX = m_gridCellsX;
	if( gridCellsY )
		*gridCellsY = m_gridCellsY;
	if( cellSize )
		*cellSize = m_gridCellSize;

}

static Real wobble(Real baseV, Real offset, Bool wobble)
{
	if (!wobble) return 0;
	offset = sin(2*PI*baseV - 3*offset);
	return offset/22;
}

/**Utility function used to query water heights in a manner that works in both RTS and WB.*/
Real WaterRenderObjClass::getWaterHeight(Real x, Real y)
{
	const WaterHandle *waterHandle = nullptr;
	Real waterZ = 0.0f;
	ICoord3D iLoc;

	iLoc.x = REAL_TO_INT_FLOOR( x + 0.5f );
	iLoc.y = REAL_TO_INT_FLOOR( y + 0.5f );
	iLoc.z = 0;

	for( PolygonTrigger *pTrig = PolygonTrigger::getFirstPolygonTrigger(); pTrig; pTrig = pTrig->getNext() )
	{

		if( !pTrig->isWaterArea() )
			continue;

		// See if point is in a water area
		if( pTrig->pointInTrigger( iLoc ) )
		{

			if( pTrig->getPoint( 0 )->z >= waterZ )
			{

				waterZ = pTrig->getPoint( 0 )->z;
				waterHandle = pTrig->getWaterHandle();

			}

		}

	}

	if (waterHandle)
		return waterHandle->m_polygon->getPoint( 0 )->z;
	return INVALID_WATER_HEIGHT;	//point not underwater
}

//-------------------------------------------------------------------------------------------------
//Draw a many sided river polygon.
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::drawRiverWater(PolygonTrigger *pTrig)
{
	rts::render::InvalidateGameRenderStateCache();	///@todo: Figure out why rivers don't draw without reset of all states.

	if (pTrig == nullptr || pTrig->getNumPoints() < 4)
		return;
	Int rectangleCount = pTrig->getNumPoints()/2;
	rectangleCount--;

	Real bumpFactor = 5;
	static Bool doWobble = true;

	if (m_disableRiver) return;
	if (rectangleCount <= 0)
		return;

	//allocate 2 triangles per side with 3 indices per triangle
	DynamicIBAccessClass ib_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,(rectangleCount+1)*2*3);
	if (!ib_access.Is_Valid())
		return;
	{
		DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
 		UnsignedShort *curIb = lockib.Get_Index_Array();
		if (!lockib.Is_Locked() || curIb == nullptr)
			return;
		for (Int i=0; i<rectangleCount; i++)
		{
			//triangle 1
			curIb[0] = i*2;
			curIb[1] = i*2+1;
			curIb[2] = i*2+3;

			//triangle 2
			curIb[3] = i*2;
			curIb[4] = i*2+3;
			curIb[5] = i*2+2;

			curIb += 6;	//skip the 6 indices we just added.
		}
	}


	Real shadeR=TheWaterTransparency->m_standingWaterColor.red;
	Real shadeG=TheWaterTransparency->m_standingWaterColor.green;
	Real shadeB=TheWaterTransparency->m_standingWaterColor.blue;

	//If the water color is not overridden, use legacy lighting code.
	if ( shadeR==1.0f && shadeG==1.0f && shadeB==1.0f)
	{
		shadeR = TheGlobalData->m_terrainAmbient[0].red;
		shadeG = TheGlobalData->m_terrainAmbient[0].green;
		shadeB = TheGlobalData->m_terrainAmbient[0].blue;

		//Add in diffuse lighting from each terrain light
		for (Int lightIndex=0; lightIndex < TheGlobalData->m_numGlobalLights; lightIndex++)
		{
			if (-TheGlobalData->m_terrainLightPos[lightIndex].z > 0)
			{	shadeR += -TheGlobalData->m_terrainLightPos[lightIndex].z * TheGlobalData->m_terrainDiffuse[lightIndex].red;
				shadeG += -TheGlobalData->m_terrainLightPos[lightIndex].z * TheGlobalData->m_terrainDiffuse[lightIndex].green;
				shadeB += -TheGlobalData->m_terrainLightPos[lightIndex].z * TheGlobalData->m_terrainDiffuse[lightIndex].blue;
			}
		}

		//Get water material colors
		Real waterShadeR = (m_settings[m_tod].waterDiffuse & 0xff) / 255.0f;
		Real waterShadeG = ((m_settings[m_tod].waterDiffuse >> 8) & 0xff) / 255.0f;
		Real waterShadeB = ((m_settings[m_tod].waterDiffuse >> 16) & 0xff) / 255.0f;

		shadeR=shadeR*waterShadeR*255.0f;
		shadeG=shadeG*waterShadeG*255.0f;
		shadeB=shadeB*waterShadeB*255.0f;
	}
	else
	{
		shadeR=shadeR*255.0f;
		shadeG=shadeG*255.0f;
		shadeB=shadeB*255.0f;

		if (shadeR == 0 && shadeG == 0 && shadeB == 0)
		{	//special case where we disable lighting
			shadeR=255;
			shadeG=255;
			shadeB=255;
		}
	}

	Int diffuse=REAL_TO_INT(shadeB) | (REAL_TO_INT(shadeG) << 8) | (REAL_TO_INT(shadeR) << 16);

	//Keep diffuse from lighting calculations but substitute custom alpha
	diffuse |= m_settings[m_tod].waterDiffuse & 0xff000000;	//copy alpha/opacity from ini setting

	Int innerNdx = pTrig->getRiverStart();
	Int outerNdx = innerNdx+1;

	Real endLen=0;
	Real totalLen=0;
	Int i;
	for (i=0; i<pTrig->getNumPoints()-1; i++) {
		ICoord3D innerPt = *pTrig->getPoint(i);
		ICoord3D outerPt = *pTrig->getPoint(i+1);
		Real dx = innerPt.x-outerPt.x;
		Real dy = innerPt.y-outerPt.y;
		Real curLen = sqrt(dx*dx+dy*dy);
		totalLen += curLen;
		if ( i==innerNdx) {
			endLen = curLen;
		}
	}
	if (endLen <= 0.0f || totalLen <= 0.0f)
		return;
	bumpFactor = endLen/BUMP_SIZE;

	Real lengthOfRiver = (totalLen/2)-endLen;
	Real repeatCount = lengthOfRiver / (endLen);

	Real vScale=(Real)repeatCount/(Real)rectangleCount;

#define HEIGHT_TO_USE (0.5f)
	if (innerNdx >= pTrig->getNumPoints()-1) return;
	//allocate 2 vertices per side
	DynamicVBAccessClass vb_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,dynamic_fvf_type,(rectangleCount+1)*2);
	if (!vb_access.Is_Valid())
		return;
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* vb=lock.Get_Formatted_Vertex_Array();
		if (!lock.Is_Locked() || vb == nullptr)
			return;
		m_drawingRiver = true;

		Real constA=3*m_riverVOrigin;

		// TheSuperHackers @bugfix afc-afc0 14/04/2026 Apply shroud per-vertex to avoid double-darkening
		// at river borders.
		W3DShroud *shroud = TheTerrainRenderObject ? TheTerrainRenderObject->getShroud() : nullptr;

		for (i=0; i<(pTrig->getNumPoints()/2); i++)
		{
			Real x,y;
			ICoord3D innerPt = *pTrig->getPoint(outerNdx);
			ICoord3D outerPt = *pTrig->getPoint(innerNdx);
			outerNdx++;
			innerNdx--;
			if (innerNdx<0) {
				innerNdx = pTrig->getNumPoints()-1;
			}
			if (outerNdx >= pTrig->getNumPoints()) {
				outerNdx = 0;
			}
			x=innerPt.x;
			y=innerPt.y;

			vb->x=x;
			vb->y=y;

			vb->z=innerPt.z;

			vb->diffuse = getRiverVertexDiffuse(shroud, x, y, shadeR, shadeG, shadeB, diffuse);

			Real wobbleConst=-m_riverVOrigin+vScale*(Real)i + WWMath::Fast_Sin(2*PI*(vScale*(Real)i) - constA)/22.0f;
 			//old slower version
			//vb->v1=-m_riverVOrigin+vScale*(Real)i + wobble(vScale*i, m_riverVOrigin, doWobble);
			vb->v1=wobbleConst;
			vb->u1=HEIGHT_TO_USE ;
			//old slower version
			//vb->v2 = -m_riverVOrigin+vScale*(Real)i + wobble(vScale*i, m_riverVOrigin, doWobble);
			vb->v2=wobbleConst;
			vb->u2 = 1.0f;
			vb->nx = 0;
			vb->ny = 0;
			vb->nz = 1.0f;
			vb++;

			x=outerPt.x;
			y=outerPt.y;

			vb->x=x;
			vb->y=y;
			vb->z=outerPt.z;

			vb->diffuse = getRiverVertexDiffuse(shroud, x, y, shadeR, shadeG, shadeB, diffuse);
 			//old slower version
			//vb->v1=-m_riverVOrigin+vScale*(Real)i + wobble(vScale*i, m_riverVOrigin, doWobble);
			vb->v1=wobbleConst;
			vb->u1=0;
			//old slower version
 			//vb->v2 = -m_riverVOrigin+vScale*(Real)i + wobble(vScale*i, m_riverVOrigin, doWobble);
			vb->v2 =wobbleConst;
			vb->u2 = 0;
			vb->nx = 0;
			vb->ny = 0;
			vb->nz = 1.0f;
			vb++;

		}
	}

	Matrix3D tm(1);

	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,tm);	//position the water surface
	rts::render::SetGameIndexBuffer(ib_access,0);
	rts::render::SetGameVertexBuffer(vb_access);
	rts::render::SetGameTexture(0,m_riverTexture);	//set to blue

	setupJbaWaterShader();

	//In additive blending we need to use the alpha at the edges of river to darken
	//rgb instead.
	if (TheWaterTransparency->m_additiveBlend)
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND, RENDER_BLEND_SOURCE_ALPHA );

	if (m_riverWaterPixelShader)
	{
		rts::render::SetGamePixelShader(m_riverWaterPixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER);
	}
	const unsigned int cull = GetWaterRestoreCullMode();
	rts::render::SetGameRenderState(GAME_RENDER_STATE_CULL_MODE, GAME_RENDER_CULL_NONE);



	if (wireframeForDebug) {
		rts::render::SetGameRenderState(GAME_RENDER_STATE_FILL_MODE, GAME_RENDER_FILL_WIREFRAME);
	}
	rts::render::DrawGameTriangles(	0,rectangleCount*2, 0,	(rectangleCount+1)*2);
	if (wireframeForDebug) {
		rts::render::SetGameRenderState(GAME_RENDER_STATE_FILL_MODE, GAME_RENDER_FILL_SOLID);
	}

	if (m_riverWaterPixelShader)
	{
		rts::render::SetGamePixelShader(0);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	}

	//restore blend mode to what W3D expects.
	if (TheWaterTransparency->m_additiveBlend)
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND, RENDER_BLEND_ONE );

	rts::render::SetGameRenderState(GAME_RENDER_STATE_CULL_MODE, cull);


}

void WaterRenderObjClass::setupFlatWaterShader()
{

	rts::render::SetGameTexture(0,m_riverTexture);
	if (!TheWaterTransparency->m_additiveBlend)
		rts::render::SetGameShader(ShaderClass::_PresetAlphaShader);
	else
		rts::render::SetGameShader(ShaderClass::_PresetAdditiveShader);

	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);
	m_riverTexture->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	m_riverTexture->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	m_riverTexture->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_BEST);

	rts::render::ApplyGameRenderStateChanges();	//force update of view and projection matrices

	//Setup shroud to render in same pass as water
	if (m_trapezoidWaterPixelShader)
	{	if (TheTerrainRenderObject->getShroud())
		{
			W3DShaderManager::setTexture(0,TheTerrainRenderObject->getShroud()->getShroudTexture());
			//Use stage 3 to apply the shroud
			W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 3);
			//Shroud shader uses z-compare of EQUAL which wouldn't work on water because it doesn't
			//write to the zbuffer.  Change to LESSEQUAL.
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION, RENDER_COMPARE_LESS_EQUAL);
		}
		else
		{	//Assume no shroud, so stage 3 will be null texture but using actual white because
			//pixel shader on GF4 generates random colors with SetTexture(3,nullptr).
			if (m_whiteTexture != nullptr && (!m_whiteTexture->Is_Initialized()
#if defined(_WIN64)
				|| m_whiteTexturePublishPending
#endif
				))
				updateWhiteTexture();
			rts::render::SetGameTexture(3, m_whiteTexture);
		}
	}

	rts::render::SetGameTextureStageState( 0, GAME_TEXTURE_STAGE_ALPHA_OPERATION,   RENDER_TEXTURE_OP_ADD );
	rts::render::SetGameTextureStageState(0,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);
	rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, 0);

	Bool doSparkles = true;

	if (m_trapezoidWaterPixelShader && doSparkles) {

		if (!m_waterSparklesTexture->Is_Initialized())
			m_waterSparklesTexture->Init();

		rts::render::SetGameTexture(1, m_waterSparklesTexture);

		if (!m_waterNoiseTexture->Is_Initialized())
			m_waterNoiseTexture->Init();

		rts::render::SetGameTexture(2, m_waterNoiseTexture);

		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(1,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_COORDINATE_INDEX, GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
		// Two output coordinates are used.
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_TRANSFORM_FLAGS, GAME_TEXTURE_TRANSFORM_COUNT2);
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_U, RENDER_TEXTURE_ADDRESS_WRAP);
		rts::render::SetGameTextureStageState(2,  GAME_TEXTURE_STAGE_ADDRESS_V, RENDER_TEXTURE_ADDRESS_WRAP);

		RenderMatrix4x4 curView;
		rts::render::GetGameTransform(GAME_TRANSFORM_VIEW, &curView);
		RenderMatrix4x4 inv;
		float det;
		RenderMatrixInverse(&inv, &det, &curView);
		RenderMatrix4x4 scale;
		RenderMatrixScaling(&scale, NOISE_REPEAT_FACTOR, NOISE_REPEAT_FACTOR,1);
		RenderMatrix4x4 destMatrix;
		RenderMatrixMultiply(&destMatrix, &inv, &scale);
		RenderMatrixTranslation(&scale, m_riverVOrigin, m_riverVOrigin,0);
		RenderMatrixMultiply(&destMatrix, &destMatrix, &scale);
		rts::render::SetGameTransform(GAME_TRANSFORM_TEXTURE2, &destMatrix);

	}
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(0, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(1, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MINIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	rts::render::SetGameTextureStageState(2, GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER, RENDER_TEXTURE_FILTER_LINEAR);
	if (m_trapezoidWaterPixelShader){
		Vector4 reflectionFactor(REFLECTION_FACTOR, REFLECTION_FACTOR,
			REFLECTION_FACTOR, 1.0f);
		rts::render::SetGamePixelShaderConstant(0, &reflectionFactor, 1);
		rts::render::SetGamePixelShader(m_trapezoidWaterPixelShader);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT);
	}
	else
	{
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	}
}

//-------------------------------------------------------------------------------------------------
//Draw a 4 sided flat water area.
//-------------------------------------------------------------------------------------------------
void WaterRenderObjClass::drawTrapezoidWater(Vector3 points[4])
{
	Vector3 origin(points[0]);
	Vector3 uVec1(points[1]);
	Vector3 vVec1(points[3]);
	Vector3 uVec2(points[2]);
	Vector3 vVec2(points[2]);
	uVec2 -= vVec1;
	vVec2	-= uVec1;
	uVec1 -= origin;
	vVec1 -= origin;
	Int uCount = (uVec1.Length()+uVec2.Length()) / (8*MAP_XY_FACTOR);
	if (uCount<1) uCount = 1;
	Int vCount = (vVec1.Length()+vVec2.Length()) / (8*MAP_XY_FACTOR);
	if (vCount<1) vCount = 1;

	if (uCount>50) uCount = 50;
	if (vCount>50) vCount = 50;

	static Bool doWobble = true;

	Int rectangleCount = uCount*vCount;

	uCount++;
	vCount++;

	Int i, j;
	DynamicIBAccessClass ib_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,(rectangleCount+1)*2*3);
	if (!ib_access.Is_Valid())
		return;

	Real	waterFactor=150;
	Real shadeR=TheWaterTransparency->m_standingWaterColor.red;
	Real shadeG=TheWaterTransparency->m_standingWaterColor.green;
	Real shadeB=TheWaterTransparency->m_standingWaterColor.blue;

	//If the water color is not overridden, use legacy lighting code.
	if ( shadeR==1.0f && shadeG==1.0f && shadeB==1.0f)
	{
		shadeR = TheGlobalData->m_terrainAmbient[0].red;
		shadeG = TheGlobalData->m_terrainAmbient[0].green;
		shadeB = TheGlobalData->m_terrainAmbient[0].blue;

		//Add in diffuse lighting from each terrain light
		for (Int lightIndex=0; lightIndex < TheGlobalData->m_numGlobalLights; lightIndex++)
		{
			if (-TheGlobalData->m_terrainLightPos[lightIndex].z > 0)
			{	shadeR += -TheGlobalData->m_terrainLightPos[lightIndex].z * TheGlobalData->m_terrainDiffuse[lightIndex].red;
				shadeG += -TheGlobalData->m_terrainLightPos[lightIndex].z * TheGlobalData->m_terrainDiffuse[lightIndex].green;
				shadeB += -TheGlobalData->m_terrainLightPos[lightIndex].z * TheGlobalData->m_terrainDiffuse[lightIndex].blue;
			}
		}

		//Get water material colors
		Real waterShadeR = (m_settings[m_tod].waterDiffuse & 0xff) / 255.0f;
		Real waterShadeG = ((m_settings[m_tod].waterDiffuse >> 8) & 0xff) / 255.0f;
		Real waterShadeB = ((m_settings[m_tod].waterDiffuse >> 16) & 0xff) / 255.0f;

		shadeR=shadeR*waterShadeR*255.0f;
		shadeG=shadeG*waterShadeG*255.0f;
		shadeB=shadeB*waterShadeB*255.0f;
	}
	else
	{
		shadeR=shadeR*255.0f;
		shadeG=shadeG*255.0f;
		shadeB=shadeB*255.0f;

		if (shadeR == 0 && shadeG == 0 && shadeB == 0)
		{	//special case where we disable lighting
			shadeR=255;
			shadeG=255;
			shadeB=255;
		}
	}

	Int diffuse=REAL_TO_INT(shadeB) | (REAL_TO_INT(shadeG) << 8) | (REAL_TO_INT(shadeR) << 16);

	//Keep diffuse from lighting calculations but substitute custom alpha
	diffuse |= m_settings[m_tod].waterDiffuse & 0xff000000;	//copy alpha/opacity from ini setting

	DynamicVBAccessClass vb_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,dynamic_fvf_type,(rectangleCount+1)*2);
	if (!vb_access.Is_Valid())
		return;
#if defined(RTS_WATER_POLYGON_MODERN)
	bool waterParallelAttempted = false;
	bool waterParallelPrepared = false;

	WaterPolygonSnapshot waterSnapshot;
	waterSnapshot.origin.x = origin.X;
	waterSnapshot.origin.y = origin.Y;
	waterSnapshot.origin.z = origin.Z;
	waterSnapshot.uVector.x = uVec1.X;
	waterSnapshot.uVector.y = uVec1.Y;
	waterSnapshot.uVector.z = uVec1.Z;
	waterSnapshot.vVector.x = vVec1.X;
	waterSnapshot.vVector.y = vVec1.Y;
	waterSnapshot.vVector.z = vVec1.Z;
	Vector3 bilinear(vVec2);
	bilinear -= vVec1;
	waterSnapshot.bilinear.x = bilinear.X;
	waterSnapshot.bilinear.y = bilinear.Y;
	waterSnapshot.bilinear.z = bilinear.Z;
	waterSnapshot.uCount = static_cast<unsigned>(uCount);
	waterSnapshot.vCount = static_cast<unsigned>(vCount);
	waterSnapshot.rectangleCount = static_cast<unsigned>(rectangleCount);
	waterSnapshot.diffuse = static_cast<unsigned>(diffuse);
	waterSnapshot.featherAlpha = 0;
	if (TheGlobalData->m_featherWater == 5) waterSnapshot.featherAlpha = 80;
	if (TheGlobalData->m_featherWater == 4) waterSnapshot.featherAlpha = 110;
	if (TheGlobalData->m_featherWater == 3) waterSnapshot.featherAlpha = 140;
	if (TheGlobalData->m_featherWater == 2) waterSnapshot.featherAlpha = 200;
	if (TheGlobalData->m_featherWater == 1) waterSnapshot.featherAlpha = 255;
	waterSnapshot.wavy = TheGlobalData->m_featherWater != 0;
	waterSnapshot.waterFactor = waterFactor;
	waterSnapshot.bumpSize = BUMP_SIZE;
	waterSnapshot.phaseBase = 25*m_riverVOrigin;
	waterSnapshot.mapCoeff = PI/(4*MAP_XY_FACTOR);
	waterSnapshot.amplitude = 0.5f;
	waterSnapshot.wobbleU = 0.02*cos(11*m_riverVOrigin);
	waterSnapshot.wobbleV = 0.02*cos(5*m_riverVOrigin);
	waterSnapshot.wavyWobbleU = 0.02*cos(11*m_riverVOrigin);
	waterSnapshot.wavyWobbleV = 0.02*cos(5*m_riverVOrigin);
	waterSnapshot.flatUScale = 1.0f/waterFactor;
	waterSnapshot.flatVScale = 1.0f/waterFactor;
	waterSnapshot.flatPhaseBase = 25*m_riverVOrigin;
	waterSnapshot.flatMapCoeff = PI/(4*MAP_XY_FACTOR);
	waterSnapshot.flatRowScale = 1.0f/(Real)(vCount-1);
	waterSnapshot.flatColumnScale = 1.0f/(Real)(uCount-1);
	waterSnapshot.flatSinScale = static_cast<Real>(SIN_TABLE_SIZE) /
		(2.0f * WWMATH_PI);
	s_waterTrapezoidScratch.captureSinTable(&_FastSinTable[0]);
	waterSnapshot.flatSinTable = s_waterTrapezoidScratch.sinTable();
	waterParallelPrepared = prepareWaterTrapezoidParallel(
		waterSnapshot, s_waterTrapezoidScratch, waterParallelAttempted);
	if (waterParallelAttempted && !waterParallelPrepared)
		rts::JobSystem::instance().recordSerialFallback();
#endif

//#define WAVY_WATER
//#define FEATHER_LAYER_COUNT (3) //LORENZEN
//#define FEATHER_LAYER_THICKNESS (2.5f)
//#define FEATHER_WATER

#if defined(RTS_WATER_POLYGON_MODERN)
if (waterParallelPrepared)
{
	typedef char WaterPolygonVertexLayoutMustMatch[
		sizeof(WaterPolygonVertex) == sizeof(VertexFormatXYZNDUV2) ? 1 : -1];
	{
		DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
		UnsignedShort *curIb = lockib.Get_Index_Array();
		if (!lockib.Is_Locked() || curIb == nullptr ||
			s_waterTrapezoidScratch.indices() == nullptr)
			return;
		memcpy(curIb, s_waterTrapezoidScratch.indices(),
			static_cast<size_t>(rectangleCount) * 6 * sizeof(UnsignedShort));
	}
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2 *vb = lock.Get_Formatted_Vertex_Array();
		if (!lock.Is_Locked() || vb == nullptr ||
			s_waterTrapezoidScratch.vertices() == nullptr)
			return;
		memcpy(vb, s_waterTrapezoidScratch.vertices(),
			static_cast<size_t>(uCount) * vCount * sizeof(VertexFormatXYZNDUV2));
	}
}
else
#endif
	{
	//allocate 2 triangles per side with 3 indices per triangle
	{
	DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
	UnsignedShort *curIb = lockib.Get_Index_Array();
	if (!lockib.Is_Locked() || curIb == nullptr)
		return;
	for (j=0; j<vCount-1; j++)
	{	for (i=0; i<uCount-1; i++)
		{
			//triangle 1
			curIb[0] = (j)*uCount + i;
			curIb[1] = (j+1)*uCount + i+1;
			curIb[2] = (j+1)*uCount + i;

			//triangle 2
			curIb[3] = (j)*uCount + i;
			curIb[4] = (j)*uCount + i+1;
			curIb[5] = (j+1)*uCount + i+1;

			curIb += 6;	//skip the 6 indices we just added.
		}
	}
	}
//#ifdef WAVY_WATER // the NEW WATER a'la LORENZEN
	if ( TheGlobalData->m_featherWater )
	{

		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* vb=lock.Get_Formatted_Vertex_Array();
		if (!lock.Is_Locked() || vb == nullptr)
			return;

		Real phase = 0;
		Real mapCoeff = PI/(4*MAP_XY_FACTOR);
		Real wave = 0;
		Real amplitude = 0.5f;

		//The first (high order) byte is the Alpha value for this patch
		// It needs to be set proportional to the number of feather layers
		// this comes from TheGlobalData->m_featherWater, which is a count of layers


		Int Alpha = 0;
		if ( TheGlobalData->m_featherWater == 5) Alpha = 80;
		if ( TheGlobalData->m_featherWater == 4) Alpha = 110;
		if ( TheGlobalData->m_featherWater == 3) Alpha = 140;
		if ( TheGlobalData->m_featherWater == 2) Alpha = 200;
		if ( TheGlobalData->m_featherWater == 1) Alpha = 255;

		//Keep diffuse from lighting calculations but substitute custom alpha
		Int customDiffuse = (diffuse & 0x00ffffff) | (Alpha<< 24);//(0x80 << 16)|(0x90 << 8)|0xa0;

		for (j=0; j<vCount; j++)
		{
			Real dv = j;
			dv /= (vCount-1);
			for (i=0; i<uCount; i++)
			{
				Real du = i;
				du /= (uCount-1);
				Vector3 vertex = origin;
				vertex += uVec1*du;
				vertex += vVec1*dv;
				vertex += (dv)*(du)*(vVec2-vVec1);

				vb->x=vertex.X;
				vb->y=vertex.Y;

				// common to all the waving effects
				phase = 25 * m_riverVOrigin + vertex.X * mapCoeff;
				wave = (sin(phase) - 1.0f) * amplitude;

				vb->z = (vertex.Z + wave);
				vb->diffuse = customDiffuse;
				vb->u1 = (vertex.X/waterFactor) + 0.02*cos(11*m_riverVOrigin)*wave;
				vb->v1 = (vertex.Y/waterFactor) + 0.02*cos(5*m_riverVOrigin)*wave;
				vb->u2 = vertex.X/BUMP_SIZE;
				vb->v2 = vertex.Y/BUMP_SIZE + 0.3f*vertex.X/BUMP_SIZE;
				vb->nx = 0;
				vb->ny = 0;
				vb->nz = 1.0f;
				vb++;
			}
		}
	}
//#else // STILL THE OLD FLAT WATER
	else

	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* vb=lock.Get_Formatted_Vertex_Array();
		if (!lock.Is_Locked() || vb == nullptr)
			return;

		//Pulling some constants out of the inner loops to improve performance -MW
		Real constA=0.02*cos(11*m_riverVOrigin);
		Real constB=0.02*cos(5*m_riverVOrigin);
		Real constC=25*m_riverVOrigin;
		Real ooWaterFactor = 1.0f/waterFactor;
		const Real constD=PI/(4*MAP_XY_FACTOR);
		Real constE=1.0f/(Real)(vCount-1);
		Real constF=1.0f/(Real)(uCount-1);

		for (j=0; j<vCount; j++)
		{
			Real dv = (Real)j * constE;

			for (i=0; i<uCount; i++)
			{
				Real du = (Real)i * constF;
				Vector3 vertex = origin;
				vertex += uVec1*du;
				vertex += vVec1*dv;
				vertex += (dv)*(du)*(vVec2-vVec1);

				vb->x=vertex.X;
				vb->y=vertex.Y;
				vb->z=vertex.Z;

				vb->diffuse= diffuse;
				//Old slower version
 				//vb->u1=(vertex.X/waterFactor) + 0.02*cos(11*m_riverVOrigin)*sin(25*m_riverVOrigin+vertex.X*PI/(4*MAP_XY_FACTOR));
 				//vb->v1=(vertex.Y/waterFactor) + 0.02*cos(5*m_riverVOrigin)*sin(25*m_riverVOrigin+vertex.Y*PI/(4*MAP_XY_FACTOR));
				vb->u1=vertex.X*ooWaterFactor + constA*WWMath::Fast_Sin(constC+vertex.X*constD);
				vb->v1=vertex.Y*ooWaterFactor + constB*WWMath::Fast_Sin(constC+vertex.Y*constD);
				vb->u2 = vertex.X/BUMP_SIZE;
				//Old slower version
 				//vb->v2 = vertex.Y/BUMP_SIZE + 0.3f*vertex.X/BUMP_SIZE;
				vb->v2 = (vertex.Y+0.3f*vertex.X)/BUMP_SIZE;
				vb->nx = 0;
				vb->ny = 0;
				vb->nz = 1.0f;
				vb++;
			}
		}
	}
	}

//#endif // OLD VS NEW WATER



	Matrix3D tm(1);

	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,tm);	//position the water surface
	rts::render::SetGameIndexBuffer(ib_access,0);
	rts::render::SetGameVertexBuffer(vb_access);

	setupFlatWaterShader();// lorenzen sez use the alpha shader

	//If video card supports it and it's enabled, feather the water edge using destination alpha
	if (rts::render::GetGameBackBufferFormat() == WW3D_FORMAT_A8R8G8B8 && TheGlobalData->m_showSoftWaterEdge && TheWaterTransparency->m_transparentWaterDepth !=0)
	{		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND, RENDER_BLEND_DESTINATION_ALPHA );
			if (!TheWaterTransparency->m_additiveBlend)
				rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND, RENDER_BLEND_INVERSE_DESTINATION_ALPHA );
	}


	const unsigned int cull = GetWaterRestoreCullMode();
	rts::render::SetGameRenderState(GAME_RENDER_STATE_CULL_MODE, GAME_RENDER_CULL_NONE);



//#ifdef FEATHER_WATER // the NEW WATER a'la LORENZEN

//	int layer = 0;//LORENZEN
//	for (layer = 0; layer < FEATHER_LAYER_COUNT; ++layer)//LORENZEN
//#endif // FEATHER_WATER
	{
//#ifdef WAVY_WATER // the NEW WATER a'la LORENZEN

		//increment the depth of the water's surface for every vert in the buffer
//#ifdef  FEATHER_WATER
//		VertexFormatXYZNDUV2 *vertBuf = vertexBufferStart;
//		while (vertBuf < vertexBufferStart + vCount * uCount)
//		{
//			vertBuf->z *= FEATHER_LAYER_THICKNESS;
//			++vertBuf;
//		}
//#endif // FEATHER_WATER
//#endif //WAVY_WATER
		rts::render::DrawGameTriangles(	0,rectangleCount*2, 0,	(rectangleCount+1)*2);//lorenzen thinks this is where to itereate the soft shoreline effect
	}




	if (false) {
		rts::render::SetGameRenderState(GAME_RENDER_STATE_FILL_MODE, GAME_RENDER_FILL_WIREFRAME);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE, false);
		rts::render::DrawGameTriangles(	0,rectangleCount*2, 0,	(rectangleCount+1)*2);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_ALPHA_BLEND_ENABLE, true);
		rts::render::SetGameRenderState(GAME_RENDER_STATE_FILL_MODE, GAME_RENDER_FILL_SOLID);
	}

	if (m_trapezoidWaterPixelShader)
	{
		rts::render::SetGamePixelShader(0);
		rts::render::SetGameLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	}
	//Restore alpha blend to default values since we may have changed them to feather edges.
	if (!TheWaterTransparency->m_additiveBlend)
	{	rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND, RENDER_BLEND_SOURCE_ALPHA );
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND, RENDER_BLEND_INVERSE_SOURCE_ALPHA );
	}
	else
	{
		rts::render::SetGameRenderState(GAME_RENDER_STATE_SOURCE_BLEND, RENDER_BLEND_ONE );
		rts::render::SetGameRenderState(GAME_RENDER_STATE_DESTINATION_BLEND, RENDER_BLEND_ONE );
	}

	if (TheTerrainRenderObject->getShroud())
	{
		if (m_trapezoidWaterPixelShader)
		{	//shroud was applied in stage3 of main pass so just need to restore state here.
			W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
			rts::render::SetGameTexture(3,nullptr);	//free possible reference to shroud texture
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION, RENDER_COMPARE_EQUAL);
		}
		else
		{	//do second pass to apply the shroud on water plane for cards that can't do it in main pass.
			W3DShaderManager::setTexture(0,TheTerrainRenderObject->getShroud()->getShroudTexture());
			W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 0);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_CULL_MODE, GAME_RENDER_CULL_NONE);
			//Shroud shader uses z-compare of EQUAL which wouldn't work on water because it doesn't
			//write to the zbuffer.  Change to LESSEQUAL.
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION, RENDER_COMPARE_LESS_EQUAL);
			rts::render::DrawGameTriangles(	0,rectangleCount*2, 0,	(rectangleCount+1)*2);
			rts::render::SetGameRenderState(GAME_RENDER_STATE_DEPTH_FUNCTION, RENDER_COMPARE_EQUAL);
			W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
		}
	}
	rts::render::SetGameRenderState(GAME_RENDER_STATE_CULL_MODE, cull);
}



//-------------------------------------------------------------------------------------------------
//debug version where moon rotates with the camera	(always upright on screen)
//-------------------------------------------------------------------------------------------------
#if 0
void WaterRenderObjClass::renderSkyBody(Matrix3D *mat)
{
	Vector3 vRight,vUp,V0,V1,V2,V3;

	mat->Get_X_Vector(&vRight);
	mat->Get_Y_Vector(&vUp);

	//calculate offsets from quad center to each of the 4 corners
	//	0-----1
	//  |    /|
	//  |  /  |
	//	|/    |
	//  3-----2
	V0=-vRight+vUp;
	V2=vRight+vUp;
	V2=vRight-vUp;
	V3=-vRight-vUp;

	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	rts::render::SetGameMaterial(vmat);
	REF_PTR_RELEASE(vmat);
	rts::render::SetGameShader(ShaderClass::/*_PresetAdditiveShader*//*_PresetOpaqueShader*/_PresetAlphaShader);
//	rts::render::SetGameTexture(0,setting->skyBodyTexture);

	rts::render::SetGameTexture(0,m_alphaClippingTexture);

	//draw an infinite sky plane
	DynamicVBAccessClass vb_access(rts::render::GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE,4);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();
		if(verts)
		{
			verts[0].x=SKYBODY_SIZE*V0.X;
			verts[0].y=SKYBODY_SIZE*V0.Y;
			verts[0].z=SKYBODY_SIZE*V0.Z;
			verts[0].u2=0;
			verts[0].v2=1;
			verts[0].diffuse=0xffffffff;

			verts[1].x=SKYBODY_SIZE*V1.X;
			verts[1].y=SKYBODY_SIZE*V1.Y;
			verts[1].z=SKYBODY_SIZE*V1.Z;
			verts[1].u2=1;
			verts[1].v2=1;
			verts[1].diffuse=0xffffffff;

			verts[2].x=SKYBODY_SIZE*V2.X;
			verts[2].y=SKYBODY_SIZE*V2.Y;
			verts[2].z=SKYBODY_SIZE*V2.Z;
			verts[2].u2=1;
			verts[2].v2=0;
			verts[2].diffuse=0xffffffff;

			verts[3].x=SKYBODY_SIZE*V3.X;
			verts[3].y=SKYBODY_SIZE*V3.Y;
			verts[3].z=SKYBODY_SIZE*V3.Z;
			verts[3].u2=0;
			verts[3].v2=0;
			verts[3].diffuse=0xffffffff;
		}
	}

	rts::render::SetGameIndexBuffer(m_indexBuffer,0);
	rts::render::SetGameVertexBuffer(vb_access);

	Matrix3D tm(1);
	//set position of skybody in world
//	tm.Set_Translation(Vector3(40,0,0));
	rts::render::SetGameTransform(GAME_TRANSFORM_WORLD,tm);

	rts::render::DrawGameTriangles(	0,2, 0,	4);	//draw a quad, 2 triangles, 4 verts
}
#endif

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void WaterRenderObjClass::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void WaterRenderObjClass::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// grid cells x
	Int cellsX = m_gridCellsX;
	xfer->xferInt( &cellsX );
	if( cellsX != m_gridCellsX )
	{

		DEBUG_CRASH(( "WaterRenderObjClass::xfer - cells X mismatch" ));
		throw SC_INVALID_DATA;

	}

	// grid cells Y
	Int cellsY = m_gridCellsY;
	xfer->xferInt( &cellsY );
	if( cellsY != m_gridCellsY )
	{

		DEBUG_CRASH(( "WaterRenderObjClass::xfer - cells Y mismatch" ));
		throw SC_INVALID_DATA;

	}

	// xfer each of the mesh data points
	for( UnsignedInt i = 0; i < m_meshDataSize; ++i )
	{

		// height
		xfer->xferReal( &m_meshData[ i ].height );

		// velocity
		xfer->xferReal( &m_meshData[ i ].velocity );

		// status
		xfer->xferUnsignedByte( &m_meshData[ i ].status );

		// preferred height
		xfer->xferUnsignedByte( &m_meshData[ i ].preferredHeight );

	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void WaterRenderObjClass::loadPostProcess()
{

}


