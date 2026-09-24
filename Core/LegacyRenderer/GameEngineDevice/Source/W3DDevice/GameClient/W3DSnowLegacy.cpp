// Legacy x86/VC6 snow renderer.
//
// The product W3DSnow.cpp owns the backend-neutral/native quad path.  This
// translation unit is selected only by the x86 compatibility build so the
// original point-sprite implementation remains available on devices that
// expose that feature.  Keep the old renderer calls physically outside the
// product source prefix.

#include "W3DDevice/GameClient/W3DSnow.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "GameClient/View.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/assetmgr.h"

#define D3DFVF_POINTVERTEX (D3DFVF_XYZ)
#define SNOW_BUFFER_SIZE 4096
#define SNOW_BATCH_SIZE 2048

struct LegacySnowPointVertex
{
	Vector3 v;
};

W3DSnowManager::W3DSnowManager()
{
	m_indexBuffer = nullptr;
	m_snowTexture = nullptr;
	m_VertexBufferOpaque = nullptr;
}

W3DSnowManager::~W3DSnowManager()
{
	ReleaseResources();
}

void W3DSnowManager::init()
{
	SnowManager::init();
	ReAcquireResources();
}

void W3DSnowManager::ReleaseResources()
{
	REF_PTR_RELEASE(m_snowTexture);

	LPDIRECT3DVERTEXBUFFER8 vertexBuffer =
		reinterpret_cast<LPDIRECT3DVERTEXBUFFER8>(m_VertexBufferOpaque);
	if (vertexBuffer)
		vertexBuffer->Release();
	m_VertexBufferOpaque = nullptr;

	REF_PTR_RELEASE(m_indexBuffer);
}

Bool W3DSnowManager::ReAcquireResources()
{
	ReleaseResources();

	if (!TheWeatherSetting->m_snowEnabled)
		return TRUE;

	const bool usePointSprites = TheWeatherSetting->m_usePointSprites &&
		DX8Wrapper::Get_Current_Caps()->Support_PointSprites();
	if (usePointSprites)
	{
		LPDIRECT3DDEVICE8 device = DX8Wrapper::_Get_D3D_Device8();
		DEBUG_ASSERTCRASH(device,
			("Trying to ReAcquireResources on W3DSnowManager without device"));
		if (!device)
			return FALSE;

		LPDIRECT3DVERTEXBUFFER8 vertexBuffer = nullptr;
		if (FAILED(device->CreateVertexBuffer(
			SNOW_BUFFER_SIZE * sizeof(LegacySnowPointVertex),
			D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC | D3DUSAGE_POINTS,
			D3DFVF_POINTVERTEX, D3DPOOL_DEFAULT, &vertexBuffer)))
			return FALSE;
		m_VertexBufferOpaque = vertexBuffer;
	}
	else
	{
		m_indexBuffer = NEW_REF(DX8IndexBufferClass, (SNOW_BATCH_SIZE * 6));
		if (!m_indexBuffer || !m_indexBuffer->Is_Valid())
		{
			REF_PTR_RELEASE(m_indexBuffer);
			return FALSE;
		}

		DX8IndexBufferClass::WriteLockClass lockIndexBuffer(m_indexBuffer);
		UnsignedShort *indices = lockIndexBuffer.Get_Index_Array();
		if (!lockIndexBuffer.Is_Locked() || !indices)
		{
			REF_PTR_RELEASE(m_indexBuffer);
			return FALSE;
		}
		Int vertexBase = 0;
		for (Int i = 0; i < SNOW_BATCH_SIZE; ++i)
		{
			indices[0] = vertexBase + 3;
			indices[1] = vertexBase;
			indices[2] = vertexBase + 2;
			indices[3] = vertexBase + 2;
			indices[4] = vertexBase;
			indices[5] = vertexBase + 1;
			vertexBase += 4;
			indices += 6;
		}
	}

	m_snowTexture = WW3DAssetManager::Get_Instance()->Get_Texture(
		TheWeatherSetting->m_snowTexture.str());
	m_dwBase = SNOW_BUFFER_SIZE;
	m_dwDiscard = SNOW_BUFFER_SIZE;
	m_dwFlush = SNOW_BATCH_SIZE;
	return TRUE;
}

void W3DSnowManager::updateIniSettings()
{
	SnowManager::updateIniSettings();
	if (m_snowTexture && stricmp(m_snowTexture->Get_Texture_Name(),
		TheWeatherSetting->m_snowTexture.str()) != 0)
	{
		REF_PTR_RELEASE(m_snowTexture);
		m_snowTexture = WW3DAssetManager::Get_Instance()->Get_Texture(
			TheWeatherSetting->m_snowTexture.str());
	}
}

void W3DSnowManager::reset()
{
	SnowManager::reset();
}

void W3DSnowManager::update()
{
	m_time += WW3D::Get_Logic_Frame_Time_Seconds();
	m_time = fmod(m_time, m_fullTimePeriod);
}

#define MAXIMUM_CAMERA_DISTANCE 100000
#define ISPOW2(x) ((x) && ((x) & ((x) - 1)) == 0)
#define MODPOW2(x,y) ((x) & ((y) - 1))

inline DWORD LegacySnowFloatToDword(FLOAT value)
{
	return *((DWORD *)&value);
}

void W3DSnowManager::renderSubBox(RenderInfoClass &rinfo, Int originX,
	Int originY, Int cubeDimX, Int cubeDimY)
{
	Int boxDimX = cubeDimX - originX;
	Int boxDimY = cubeDimY - originY;
	Int halfX = REAL_TO_INT_CEIL(boxDimX * 0.5f);
	Int halfY = REAL_TO_INT_CEIL(boxDimY * 0.5f);
	CameraClass &camera = rinfo.Camera;
	MinMaxAABoxClass box;

	if (boxDimX > m_leafDim)
	{
		if (boxDimY > m_leafDim)
		{
			box.MinCorner.Set(originX * m_emitterSpacing - m_cullOverscan,
				(originY + halfY) * m_emitterSpacing - m_cullOverscan,
				m_snowCeiling - m_boxDimensions);
			box.MaxCorner.Set((originX + halfX) * m_emitterSpacing + m_cullOverscan,
				cubeDimY * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX, originY + halfY, originX + halfX, cubeDimY);

			box.MinCorner.Set((originX + halfX) * m_emitterSpacing - m_cullOverscan,
				(originY + halfY) * m_emitterSpacing - m_cullOverscan,
				m_snowCeiling - m_boxDimensions);
			box.MaxCorner.Set(cubeDimX * m_emitterSpacing + m_cullOverscan,
				cubeDimY * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX + halfX, originY + halfY, cubeDimX, cubeDimY);

			box.MinCorner.Set(originX * m_emitterSpacing - m_cullOverscan,
				originY * m_emitterSpacing - m_cullOverscan,
				m_snowCeiling - m_boxDimensions);
			box.MaxCorner.Set((originX + halfX) * m_emitterSpacing + m_cullOverscan,
				(originY + halfY) * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX, originY, originX + halfX, originY + halfY);

			box.MinCorner.Set((originX + halfX) * m_emitterSpacing - m_cullOverscan,
				originY * m_emitterSpacing - m_cullOverscan,
				m_snowCeiling - m_boxDimensions);
			box.MaxCorner.Set(cubeDimX * m_emitterSpacing + m_cullOverscan,
				(originY + halfY) * m_emitterSpacing + m_cullOverscan,
				m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX + halfX, originY, cubeDimX, originY + halfY);
			return;
		}

		box.MinCorner.Set(originX * m_emitterSpacing - m_cullOverscan,
			originY * m_emitterSpacing - m_cullOverscan,
			m_snowCeiling - m_boxDimensions);
		box.MaxCorner.Set((originX + halfX) * m_emitterSpacing + m_cullOverscan,
			cubeDimY * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
		if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
			renderSubBox(rinfo, originX, originY, originX + halfX, cubeDimY);

		box.MinCorner.Set((originX + halfX) * m_emitterSpacing - m_cullOverscan,
			originY * m_emitterSpacing - m_cullOverscan,
			m_snowCeiling - m_boxDimensions);
		box.MaxCorner.Set(cubeDimX * m_emitterSpacing + m_cullOverscan,
			cubeDimY * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
		if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
			renderSubBox(rinfo, originX + halfX, originY, cubeDimX, cubeDimY);
		return;
	}

	if (boxDimY > m_leafDim)
	{
		box.MinCorner.Set(originX * m_emitterSpacing - m_cullOverscan,
			(originY + halfY) * m_emitterSpacing - m_cullOverscan,
			m_snowCeiling - m_boxDimensions);
		box.MaxCorner.Set(cubeDimX * m_emitterSpacing + m_cullOverscan,
			cubeDimY * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
		if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
			renderSubBox(rinfo, originX, originY + halfY, cubeDimX, cubeDimY);

		box.MinCorner.Set(originX * m_emitterSpacing - m_cullOverscan,
			originY * m_emitterSpacing - m_cullOverscan,
			m_snowCeiling - m_boxDimensions);
		box.MaxCorner.Set(cubeDimX * m_emitterSpacing + m_cullOverscan,
			(originY + halfY) * m_emitterSpacing + m_cullOverscan, m_snowCeiling);
		if (CollisionMath::Overlap_Test(camera.Get_Frustum(), box) != CollisionMath::OUTSIDE)
			renderSubBox(rinfo, originX, originY, cubeDimX, originY + halfY);
		return;
	}

	Int totalPart = (cubeDimY - originY) * (cubeDimX - originX);
	if (!totalPart)
		return;

	Int y = originY;
	Int originXRemainder = originX;
	Vector3 snowCenter;
	LPDIRECT3DVERTEXBUFFER8 vertexBuffer =
		reinterpret_cast<LPDIRECT3DVERTEXBUFFER8>(m_VertexBufferOpaque);
	if (!vertexBuffer)
		return;
	m_totalRendered += totalPart;

	while (totalPart)
	{
		Int batchSize = totalPart > m_dwFlush ? m_dwFlush : totalPart;
		if (m_dwBase + batchSize > m_dwDiscard)
			m_dwBase = 0;

		LegacySnowPointVertex *vertices = nullptr;
		if (vertexBuffer->Lock(m_dwBase * sizeof(LegacySnowPointVertex),
			batchSize * sizeof(LegacySnowPointVertex),
			(unsigned char **)&vertices,
			m_dwBase ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD) != D3D_OK)
			return;

		Int numberInBatch = 0;
		for (; y < cubeDimY; ++y)
		{
			for (Int x = originXRemainder; x < cubeDimX; ++x)
			{
				if (numberInBatch >= batchSize)
				{
					originXRemainder = x;
					goto flush_particles;
				}

				Int noiseOffset = MODPOW2(x + MAXIMUM_CAMERA_DISTANCE, SNOW_NOISE_X) +
					MODPOW2(y + MAXIMUM_CAMERA_DISTANCE, SNOW_NOISE_Y) * SNOW_NOISE_X;
				if (noiseOffset > SNOW_NOISE_X * SNOW_NOISE_Y)
					noiseOffset = 0;
				Real h0 = m_snowCeiling - fmod(
					m_heightTraveled + m_startingHeights[noiseOffset], m_boxDimensions);
				snowCenter.Set(x * m_emitterSpacing, y * m_emitterSpacing, h0);
				snowCenter.X += m_amplitude * WWMath::Fast_Sin(
					h0 * m_frequencyScaleX + (Real)x);
				snowCenter.Y += m_amplitude * WWMath::Fast_Sin(
					h0 * m_frequencyScaleY + (Real)y);
				vertices->v = snowCenter;
				++vertices;
				++numberInBatch;
			}
			originXRemainder = originX;
		}

flush_particles:
		vertexBuffer->Unlock();
		if (numberInBatch)
		{
			Debug_Statistics::Record_DX8_Polys_And_Vertices(
				numberInBatch * 2, numberInBatch * 4,
				ShaderClass::_PresetOpaqueShader);
			DX8Wrapper::_Get_D3D_Device8()->DrawPrimitive(
				D3DPT_POINTLIST, m_dwBase, numberInBatch);
			totalPart -= numberInBatch;
			m_dwBase += numberInBatch;
		}
	}
}

void W3DSnowManager::render(RenderInfoClass &rinfo)
{
	if (!TheWeatherSetting->m_snowEnabled || !m_isVisible)
		return;

	WWASSERT(ISPOW2(SNOW_NOISE_X) && ISPOW2(SNOW_NOISE_Y));
	const Coord3D &cameraPosition = TheTacticalView->get3DCameraPosition();
	Vector3 camPos(cameraPosition.x, cameraPosition.y, cameraPosition.z);
	Int emittersInHalf = (Int)floor(m_boxDimensions / m_emitterSpacing * 0.5f);
	Int cubeCenterX = (Int)floor(camPos.X / m_emitterSpacing);
	Int cubeCenterY = (Int)floor(camPos.Y / m_emitterSpacing);
	Int cubeOriginX = cubeCenterX - emittersInHalf;
	Int cubeOriginY = cubeCenterY - emittersInHalf;
	Int cubeDimX = cubeCenterX + emittersInHalf;
	Int cubeDimY = cubeCenterY + emittersInHalf;

	const FrustumClass &frustum = rinfo.Camera.Get_Frustum();
	AABoxClass bbox;
	TheTerrainRenderObject->getMaximumVisibleBox(frustum, &bbox, TRUE);
	bbox.Extent.X += m_amplitude + m_quadSize;
	bbox.Extent.Y += m_amplitude + m_quadSize;
	if (cubeOriginX * m_emitterSpacing < bbox.Center.X - bbox.Extent.X)
		cubeOriginX = (Int)floor((bbox.Center.X - bbox.Extent.X) / m_emitterSpacing);
	if (cubeOriginY * m_emitterSpacing < bbox.Center.Y - bbox.Extent.Y)
		cubeOriginY = (Int)floor((bbox.Center.Y - bbox.Extent.Y) / m_emitterSpacing);
	if (cubeDimX * m_emitterSpacing > bbox.Center.X + bbox.Extent.X)
		cubeDimX = (Int)floor((bbox.Center.X + bbox.Extent.X) / m_emitterSpacing);
	if (cubeDimY * m_emitterSpacing > bbox.Center.Y + bbox.Extent.Y)
		cubeDimY = (Int)floor((bbox.Center.Y + bbox.Extent.Y) / m_emitterSpacing);
	if (cubeDimY - cubeOriginY < 0 || cubeDimX - cubeOriginX < 0)
		return;

	Int totalPart = (cubeDimY - cubeOriginY) * (cubeDimX - cubeOriginX);
	if (totalPart <= 0)
		return;
	m_snowCeiling = camPos.Z + m_boxDimensions / 2.0f;
	Real cameraOffset = fmod(camPos.Z, m_boxDimensions);
	m_heightTraveled = m_time * m_velocity + cameraOffset;

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, identity);
	DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);
	VertexMaterialClass *material = VertexMaterialClass::Get_Preset(
		VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(material);
	REF_PTR_RELEASE(material);

	const bool usePointSprites = TheWeatherSetting->m_usePointSprites &&
		DX8Wrapper::Get_Current_Caps()->Support_PointSprites();
	if (usePointSprites && !m_VertexBufferOpaque)
		ReAcquireResources();
	if (!usePointSprites &&
		(m_indexBuffer == nullptr || !m_indexBuffer->Is_Valid()))
		ReAcquireResources();
	if (!usePointSprites &&
		(m_indexBuffer == nullptr || !m_indexBuffer->Is_Valid()))
		return;
	DX8Wrapper::Set_Texture(0, m_snowTexture);

	if (!usePointSprites)
	{
		renderAsQuads(rinfo, cubeOriginX, cubeOriginY, cubeDimX, cubeDimY);
		return;
	}

	LPDIRECT3DVERTEXBUFFER8 vertexBuffer =
		reinterpret_cast<LPDIRECT3DVERTEXBUFFER8>(m_VertexBufferOpaque);
	LPDIRECT3DDEVICE8 device = DX8Wrapper::_Get_D3D_Device8();
	if (!vertexBuffer || !device)
		return;
	DX8Wrapper::Apply_Render_State_Changes();
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSPRITEENABLE, TRUE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSCALEENABLE, TRUE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSIZE,
		LegacySnowFloatToDword(m_pointSize));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSIZE_MIN,
		LegacySnowFloatToDword(m_minPointSize));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSIZE_MAX,
		LegacySnowFloatToDword(m_maxPointSize));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSCALE_A,
		LegacySnowFloatToDword(0.0f));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSCALE_B,
		LegacySnowFloatToDword(0.0f));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSCALE_C,
		LegacySnowFloatToDword(1.0f));
	device->SetStreamSource(0, vertexBuffer, sizeof(LegacySnowPointVertex));
	device->SetVertexShader(D3DFVF_POINTVERTEX);
	m_dwBase = SNOW_BUFFER_SIZE;
	m_leafDim = 45;
	m_totalRendered = 0;
	m_cullOverscan = m_amplitude + m_quadSize;
	renderSubBox(rinfo, cubeOriginX, cubeOriginY, cubeDimX, cubeDimY);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSPRITEENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_POINTSCALEENABLE, FALSE);
}

void W3DSnowManager::renderAsQuads(RenderInfoClass &rinfo, Int cubeOriginX,
	Int cubeOriginY, Int cubeDimX, Int cubeDimY)
{
	if (m_indexBuffer == nullptr || !m_indexBuffer->Is_Valid())
		return;

	Matrix4x4 projection;
	Matrix3D view;
	Vector3 snowCenter;
	Vector3 snowCenterVS;
	CameraClass &camera = rinfo.Camera;
	camera.Get_View_Matrix(&view);
	camera.Get_Projection_Matrix(&projection);
	Vector3 offsets[4] = {
		Vector3(-0.5f, 0.5f, 0.0f), Vector3(-0.5f, -0.5f, 0.0f),
		Vector3(0.5f, -0.5f, 0.0f), Vector3(0.5f, 0.5f, 0.0f)
	};
	Vector2 uvs[4] = {
		Vector2(0.0f, 0.0f), Vector2(0.0f, 1.0f),
		Vector2(1.0f, 1.0f), Vector2(1.0f, 0.0f)
	};
	for (Int i = 0; i < 4; ++i)
		offsets[i] *= m_quadSize;

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_VIEW, identity);
	DX8Wrapper::Set_Index_Buffer(m_indexBuffer, 0);
	Int y = cubeOriginY;
	Int originXRemainder = cubeOriginX;
	Int totalPart = (cubeDimY - cubeOriginY) * (cubeDimX - cubeOriginX);
	m_totalRendered += totalPart;
	while (totalPart)
	{
		Int batchSize = totalPart > SNOW_BATCH_SIZE ? SNOW_BATCH_SIZE : totalPart;
		Int numberInBatch = 0;
		DynamicVBAccessClass vbAccess(BUFFER_TYPE_DYNAMIC_DX8,
			dynamic_fvf_type, batchSize * 4);
		if (!vbAccess.Is_Valid())
			return;
		{
			DynamicVBAccessClass::WriteLockClass lock(&vbAccess);
			VertexFormatXYZNDUV2 *vertices = lock.Get_Formatted_Vertex_Array();
			if (!lock.Is_Locked() || !vertices)
				return;
			for (; y < cubeDimY; ++y)
			{
				for (Int x = originXRemainder; x < cubeDimX; ++x)
				{
					if (numberInBatch >= batchSize)
					{
						originXRemainder = x;
						goto flush_quad_particles;
					}
					Int noiseOffset = MODPOW2(x + MAXIMUM_CAMERA_DISTANCE, SNOW_NOISE_X) +
						MODPOW2(y + MAXIMUM_CAMERA_DISTANCE, SNOW_NOISE_Y) * SNOW_NOISE_X;
					if (noiseOffset > SNOW_NOISE_X * SNOW_NOISE_Y)
						noiseOffset = 0;
					Real h0 = m_snowCeiling - fmod(
						m_heightTraveled + m_startingHeights[noiseOffset], m_boxDimensions);
					snowCenter.Set(x * m_emitterSpacing, y * m_emitterSpacing, h0);
					Matrix3D::Transform_Vector(view, snowCenter, &snowCenterVS);
					snowCenterVS.X += m_amplitude * WWMath::Fast_Sin(
						h0 * m_frequencyScaleX + (Real)x);
					snowCenterVS.Y += m_amplitude * WWMath::Fast_Sin(
						h0 * m_frequencyScaleY + (Real)y);
					for (Int i = 0; i < 4; ++i)
					{
						*(Vector3 *)vertices = snowCenterVS + offsets[i];
						vertices->nx = vertices->ny = vertices->nz = 0;
						vertices->diffuse = 0xffffffff;
						vertices->u1 = uvs[i].X;
						vertices->v1 = uvs[i].Y;
						vertices->u2 = vertices->v2 = 0;
						++vertices;
					}
					++numberInBatch;
				}
				originXRemainder = cubeOriginX;
			}
flush_quad_particles:
			(void)numberInBatch;
		}
		if (numberInBatch)
		{
			DX8Wrapper::Set_Vertex_Buffer(vbAccess);
			DX8Wrapper::Draw_Triangles(0, numberInBatch * 2, 0,
				numberInBatch * 4);
			totalPart -= numberInBatch;
		}
	}
}
