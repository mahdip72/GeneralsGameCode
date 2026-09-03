/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** Native title-owned portion of the renderer-neutral WW3D2 seam.
**
** Camera, material, and light objects are title ABI types, so their
** conversion belongs beside the title WW3D2 sources.  The owner receives
** only synchronous renderer-neutral values and never retains these objects.
*/

#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/LegacyColorPacking.h"
#include "dx8renderer.h"
#include "camera.h"
#include "lightenvironment.h"
#include "shader.h"
#include "statistics.h"
#include "vertmaterial.h"
#include "ww3d.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"

#include <cmath>
#include <math.h>
#include <string.h>

namespace
{

using namespace rts::render;

// The snapshot is a value transfer, not a pointer alias.  Keep this check
// close to the copy code so a future contract change cannot silently change
// the row-major matrix payload used by the native owner.
typedef char GameCameraSnapshotMatrixLayoutCheck[
	sizeof(RenderMatrix4) == sizeof(float) * 16 ? 1 : -1];

bool IsOperationalOwner(IGameRenderClientNativeOwner *owner)
{
	return owner != 0 && owner->IsInitialized() && owner->IsOperational();
}

RenderResult SubmitCommand(IGameRenderClientNativeOwner *owner,
	const GameRenderCommand &command)
{
	const RenderResult result = owner->ExecuteGameRenderCommand(command);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

void CopyMatrix(const Matrix4x4 &source, RenderMatrix4 *destination)
{
	for (unsigned int row = 0; row < 4; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
			destination->values[row * 4 + column] = source[row][column];
	}
}

void CopyMatrix(const Matrix3D &source, RenderMatrix4 *destination)
{
	for (unsigned int row = 0; row < 3; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
			destination->values[row * 4 + column] = source[row][column];
	}
	destination->values[12] = 0.0f;
	destination->values[13] = 0.0f;
	destination->values[14] = 0.0f;
	destination->values[15] = 1.0f;
}

void SetLightCommandFields(GameRenderCommand *command,
	const LegacyLightState &light, unsigned int index)
{
	memset(command, 0, sizeof(*command));
	command->type = GAME_RENDER_COMMAND_SET_LIGHT;
	command->value0 = index;
	command->input = &light;
	command->inputBytes = sizeof(light);
}

}

namespace rts
{
namespace render
{

void SetGameShader(const ShaderClass &shader)
{
	// Apply only reads the instance bits and updates the process-wide shader
	// cache.  Copying keeps this const facade honest while retaining the exact
	// ShaderClass::Apply path used by direct title calls.
	ShaderClass appliedShader(shader);
	appliedShader.Apply();
}

RenderResult SetGameShaderCullInverted(bool inverted)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result = owner->SetGameShaderCullInverted(inverted);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

void SetGameMaterial(const VertexMaterialClass *material)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;

	LegacyVertexMaterialState state;
	if (material != 0)
	{
		// The historical color-source and UV accessors predate const-correctness;
		// they only read the material and do not alter its state.
		VertexMaterialClass *mutableMaterial =
			const_cast<VertexMaterialClass *>(material);
		state.material = mutableMaterial->Get_Renderer_Material_State();
		state.lightingEnable = WW3D::Is_Coloring_Enabled() ? false :
			mutableMaterial->Get_Lighting();
		state.ambientMaterialSource =
			static_cast<RenderMaterialSource>(
				mutableMaterial->Get_Ambient_Color_Source());
		state.diffuseMaterialSource =
			static_cast<RenderMaterialSource>(
				mutableMaterial->Get_Diffuse_Color_Source());
		state.emissiveMaterialSource =
			static_cast<RenderMaterialSource>(
				mutableMaterial->Get_Emissive_Color_Source());
		for (unsigned int stage = 0;
			stage < MeshBuilderClass::MAX_STAGES; ++stage)
		{
			state.textureCoordinateIndex[stage] = static_cast<unsigned int>(
				mutableMaterial->Get_UV_Source(stage));
			if (mutableMaterial->Peek_Mapper(stage) == 0)
				state.textureStageResetMask |= 1U << stage;
		}
	}

	GameRenderCommand command;
	memset(&command, 0, sizeof(command));
	command.type = GAME_RENDER_COMMAND_SET_MATERIAL;
	command.input = &state;
	command.inputBytes = sizeof(state);
	SubmitCommand(owner, command);
}

void SetGameLightEnvironment(LightEnvironmentClass *lightEnvironment)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner) || lightEnvironment == 0)
		return;

	const int lightCount = lightEnvironment->Get_Light_Count();
	if (lightCount < 0 || lightCount > static_cast<int>(LEGACY_LIGHT_COUNT))
	{
		owner->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}

	// The old path packed ambient through its ARGB state cache before
	// publishing it.  Decode the packed value so native and compatibility
	// paths see the same quantized channels and historical opaque alpha.
	const Vector3 &equivalentAmbient =
		lightEnvironment->Get_Equivalent_Ambient();
	const unsigned int ambientColor = PackLegacyARGB(equivalentAmbient.X,
		equivalentAmbient.Y, equivalentAmbient.Z, 0.0f);
	const RenderFloat4 ambient = DecodeLegacyAmbientColor(ambientColor);
	GameRenderCommand command;
	memset(&command, 0, sizeof(command));
	command.type = GAME_RENDER_COMMAND_SET_AMBIENT_COLOR;
	command.float0 = ambient.x;
	command.float1 = ambient.y;
	command.float2 = ambient.z;
	command.float3 = ambient.w;
	if (SubmitCommand(owner, command) != RENDER_RESULT_OK)
		return;

	for (int index = 0; index < lightCount; ++index)
	{
		LegacyLightState light;
		light.enabled = true;
		light.type = RENDER_LIGHT_DIRECTIONAL;
		const Vector3 &diffuse =
			lightEnvironment->Get_Light_Diffuse(index);
		light.diffuse = RenderFloat4(diffuse.X, diffuse.Y, diffuse.Z, 0.0f);
		const Vector3 &direction =
			lightEnvironment->Get_Light_Direction(index);
		// WW3D's historical light setup negates the stored direction.
		light.direction = RenderFloat4(-direction.X, -direction.Y,
			-direction.Z, 0.0f);
		if (index == 0)
		{
			light.specular = RenderFloat4(1.0f, 1.0f, 1.0f, 0.0f);
		}

		if (lightEnvironment->isPointLight(index))
		{
			light.type = RENDER_LIGHT_POINT;
			const Vector3 &pointDiffuse =
				lightEnvironment->getPointDiffuse(index);
			const Vector3 &pointAmbient =
				lightEnvironment->getPointAmbient(index);
			const Vector3 &pointCenter =
				lightEnvironment->getPointCenter(index);
			light.diffuse = RenderFloat4(pointDiffuse.X, pointDiffuse.Y,
				pointDiffuse.Z, 0.0f);
			light.ambient = RenderFloat4(pointAmbient.X, pointAmbient.Y,
				pointAmbient.Z, 0.0f);
			light.position = RenderFloat4(pointCenter.X, pointCenter.Y,
				pointCenter.Z, 1.0f);
			const float innerRadius = lightEnvironment->getPointIrad(index);
			const float outerRadius = lightEnvironment->getPointOrad(index);
			light.range = outerRadius;
			if (::fabs(static_cast<double>(innerRadius) -
				static_cast<double>(outerRadius)) < 1e-5)
			{
				light.attenuation1 = 0.0f;
			}
			else
			{
				light.attenuation1 = 0.1f / innerRadius;
			}
			light.attenuation0 = 1.0f;
			light.attenuation2 = 8.0f /
				(outerRadius * outerRadius);
		}

		SetLightCommandFields(&command, light,
			static_cast<unsigned int>(index));
		if (SubmitCommand(owner, command) != RENDER_RESULT_OK)
			return;
	}

	for (unsigned int index = static_cast<unsigned int>(lightCount);
		index < LEGACY_LIGHT_COUNT; ++index)
	{
		LegacyLightState light;
		SetLightCommandFields(&command, light, index);
		if (SubmitCommand(owner, command) != RENDER_RESULT_OK)
			return;
	}
}

void SetGameRenderCamera(void *cameraOpaque)
{
	CameraClass *camera = static_cast<CameraClass *>(cameraOpaque);
	if (camera == 0)
		return;

	// Query before pinning the owner: the query itself acquires the owner gate,
	// and taking it twice would deadlock the lifecycle mutex.
	int width = 0;
	int height = 0;
	int bitDepth = 0;
	bool windowed = false;
	const RenderResult resolutionResult = GetGameRendererTargetResolution(
		&width, &height, &bitDepth, &windowed);
	if (resolutionResult != RENDER_RESULT_OK || width <= 0 || height <= 0)
	{
		NativeGameRenderOwnerScope scope;
		IGameRenderClientNativeOwner *owner = scope.Get();
		if (owner != 0)
		{
			owner->RecordGameFailure(resolutionResult != RENDER_RESULT_OK ?
				resolutionResult : RENDER_RESULT_INVALID_ARGUMENT);
		}
		return;
	}

	const ViewportClass &viewport = camera->Get_Viewport();
	float zNear = 0.0f;
	float zFar = 1.0f;
	float minimumDepth = 0.0f;
	float maximumDepth = 1.0f;
	camera->Get_Clip_Planes(zNear, zFar);
	camera->Get_Depth_Range(&minimumDepth, &maximumDepth);
	// Validate raw camera metadata before projection arithmetic or conversion
	// of normalized viewport coordinates to unsigned pixel values.
	if (!std::isfinite(viewport.Min.X) || !std::isfinite(viewport.Min.Y) ||
		!std::isfinite(viewport.Max.X) || !std::isfinite(viewport.Max.Y) ||
		!std::isfinite(minimumDepth) || !std::isfinite(maximumDepth) ||
		!std::isfinite(zNear) || !std::isfinite(zFar) ||
		viewport.Min.X < 0.0f || viewport.Min.Y < 0.0f ||
		viewport.Max.X > 1.0f || viewport.Max.Y > 1.0f ||
		viewport.Min.X > viewport.Max.X || viewport.Min.Y > viewport.Max.Y ||
		minimumDepth < 0.0f || minimumDepth > 1.0f ||
		maximumDepth < 0.0f || maximumDepth > 1.0f ||
		minimumDepth > maximumDepth || zNear < 0.0f || zFar <= zNear ||
		(camera->Get_Projection_Type() == CameraClass::PERSPECTIVE && zNear == 0.0f))
	{
		NativeGameRenderOwnerScope scope;
		IGameRenderClientNativeOwner *owner = scope.Get();
		if (owner != 0)
			owner->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}

	const unsigned int viewportX = static_cast<unsigned int>(
		viewport.Min.X * static_cast<float>(width));
	const unsigned int viewportY = static_cast<unsigned int>(
		viewport.Min.Y * static_cast<float>(height));
	const unsigned int viewportWidth = static_cast<unsigned int>(
		(viewport.Max.X - viewport.Min.X) * static_cast<float>(width));
	const unsigned int viewportHeight = static_cast<unsigned int>(
		(viewport.Max.Y - viewport.Min.Y) * static_cast<float>(height));
	if (viewportWidth == 0 || viewportHeight == 0)
	{
		NativeGameRenderOwnerScope scope;
		IGameRenderClientNativeOwner *owner = scope.Get();
		if (owner != 0)
			owner->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}

	Matrix4x4 projection;
	Matrix3D view;
	camera->Get_D3D_Projection_Matrix(&projection);
	camera->Get_View_Matrix(&view);

	GameCameraSnapshot snapshot;
	CopyMatrix(view, &snapshot.view);
	CopyMatrix(projection, &snapshot.projection);
	snapshot.viewport = RenderViewport(static_cast<float>(viewportX),
		static_cast<float>(viewportY), static_cast<float>(viewportWidth),
		static_cast<float>(viewportHeight), minimumDepth, maximumDepth);
	snapshot.zNear = zNear;
	snapshot.zFar = zFar;

	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	const RenderResult result = owner->SetGameRenderCameraSnapshot(snapshot);
	if (result != RENDER_RESULT_OK)
	{
		owner->RecordGameFailure(result);
		return;
	}

	// Mesh sorting uses the title camera synchronously while flushing.  Keep
	// the historical camera association alongside the neutral snapshot.
	TheDX8MeshRenderer.Set_Camera(camera);
}

void FlushGameRenderMeshes()
{
	TheDX8MeshRenderer.Flush();
}

void ClearGameRenderMeshPendingDeletes()
{
	TheDX8MeshRenderer.Clear_Pending_Delete_Lists();
}

void InvalidateGameMeshCache()
{
	// This is the direct category-cache operation used by WW3D's mesh-cache
	// invalidation hook.  Sorted work belongs to the renderer-cache operation.
	TheDX8MeshRenderer.Invalidate();
}

void InvalidateGameMeshRendererCache()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	const RenderResult result = owner->FlushGameSortedTriangles();
	if (result != RENDER_RESULT_OK)
	{
		owner->RecordGameFailure(result);
		return;
	}
	TheDX8MeshRenderer.Invalidate();
}

unsigned int GetGameLastFramePolygonCount()
{
	return static_cast<unsigned int>(Debug_Statistics::Get_DX8_Polygons());
}

unsigned int GetGameLastFrameVertexCount()
{
	return static_cast<unsigned int>(Debug_Statistics::Get_DX8_Vertices());
}

}
}
