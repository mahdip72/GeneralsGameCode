#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameTexturePass.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/RenderTexturePublication.h"
#include "Renderer/RenderMatrixMath.h"
#include "camera.h"
#include "dx8renderer.h"
#include "vertmaterial.h"
#include "shader.h"
#include "texture.h"
#include "texturefilter.h"
#include "nativew3d2.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <windows.h>

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
		return 0;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

const wchar_t *kWindowClassName = L"GeneralsGameCodeNativeTitleCameraAdapterTest";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateHiddenWindow()
{
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = WindowProcedure;
	windowClass.hInstance = GetModuleHandleW(0);
	windowClass.lpszClassName = kWindowClassName;
	RegisterClassExW(&windowClass);
	return CreateWindowExW(0, kWindowClassName, L"Native title camera adapter",
		WS_OVERLAPPEDWINDOW, 0, 0, 101, 79, 0, 0, windowClass.hInstance, 0);
}

// This sink observes the actual paired title adapter's complete output, not a
// copy of its conversion algorithm. Keeping malformed RED outputs here also
// prevents an invalid viewport from reaching the real GPU. The bootstrap still
// supplies the actual target-resolution query used by production.
class CameraOutputSink : public rts::render::IGameRenderClientNativeOwner
{
public:
	CameraOutputSink() : snapshots(0), viewportCount(0), commandCount(0), failures(0),
		lastFailure(rts::render::RENDER_RESULT_OK), operational(true),
		targetInfo()
	{
		targetInfo.width = 101;
		targetInfo.height = 79;
		targetInfo.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		targetInfo.multisampleCount = 1;
	}
	bool IsInitialized() const override { return true; }
	bool IsOperational() const override { return operational; }
	rts::render::RenderResult GetGameRenderTargetInfo(
		rts::render::RenderBackBufferInfo *info) const override
	{
		if (info == 0)
			return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		*info = targetInfo;
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::RenderResult SetGameShaderCullInverted(bool) override
	{
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::RenderResult ApplyGameShaderBits(unsigned int bits) override
	{
		rts::render::TrackLegacyShaderBits(bits);
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::GameRenderTargetKind ActiveRenderTargetKind() const override
	{
		return rts::render::GAME_RENDER_TARGET_BACK_BUFFER;
	}
	rts::render::RenderResult SetGameRenderCameraSnapshot(
		const rts::render::GameCameraSnapshot &value) override
	{
		++snapshots;
		snapshot = value;
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::RenderResult SetGameViewport(
		const rts::render::RenderViewport &value) override
	{
		++viewportCount;
		viewport = value;
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::RenderResult ExecuteGameRenderCommand(
		const rts::render::GameRenderCommand &command) override
	{
		// Observe the actual Apply command path without forwarding RED payloads
		// to the GPU or retaining pointers to its synchronous matrix payloads.
		if (commandCount < 2)
		{
			commandTypes[commandCount] = command.type;
			commandSlots[commandCount] = command.value0;
			commandMatrixPresent[commandCount] =
				(command.type == rts::render::GAME_RENDER_COMMAND_SET_TRANSFORM ||
				 command.type == rts::render::GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS) &&
				command.input != 0 && command.inputBytes == sizeof(rts::render::RenderMatrix4);
			if (commandMatrixPresent[commandCount])
				commandMatrices[commandCount] =
					*static_cast<const rts::render::RenderMatrix4 *>(command.input);
		}
		++commandCount;
		if (command.type == rts::render::GAME_RENDER_COMMAND_GET_TRANSFORM &&
			command.output != 0 && command.outputBytes == sizeof(transformReply))
			*static_cast<rts::render::RenderMatrix4 *>(command.output) = transformReply;
		if (command.type == rts::render::GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE &&
			command.value1 == rts::render::GAME_TEXTURE_STAGE_MAX_ANISOTROPY)
		{
			++anisotropyCount;
			anisotropyValid = anisotropyValid && command.value2 == 1U;
		}
		return rts::render::RENDER_RESULT_OK;
	}
	void RecordGameFailure(rts::render::RenderResult value) override
	{
		++failures;
		lastFailure = value;
	}
	void ResetObservations()
	{
		snapshots = 0;
		viewportCount = 0;
		commandCount = 0;
		failures = 0;
		lastFailure = rts::render::RENDER_RESULT_OK;
	}
	unsigned int snapshots;
	unsigned int anisotropyCount = 0;
	bool anisotropyValid = true;
	unsigned int viewportCount;
	unsigned int commandCount;
	unsigned int failures;
	rts::render::RenderResult lastFailure;
	bool operational;
	rts::render::RenderBackBufferInfo targetInfo;
	rts::render::GameCameraSnapshot snapshot;
	rts::render::RenderViewport viewport;
	rts::render::GameRenderCommandType commandTypes[2];
	unsigned int commandSlots[2];
	bool commandMatrixPresent[2];
	rts::render::RenderMatrix4 commandMatrices[2];
	rts::render::RenderMatrix4 transformReply;
};

int TestTransformConvention(CameraOutputSink *sink)
{
	using namespace rts::render;
	int result = 0;
	Matrix3D affine(true);
	affine[0][0] = 0.0f;
	affine[0][1] = -1.0f;
	affine[1][0] = 1.0f;
	affine[1][1] = 0.0f;
	affine.Set_Translation(Vector3(7.0f, 11.0f, 13.0f));
	sink->ResetObservations();
	SetGameTransform(GAME_TRANSFORM_WORLD, affine);
	const RenderMatrix4 &world = sink->commandMatrices[0];
	result |= Check(world.values[12] == 7.0f && world.values[13] == 11.0f &&
		world.values[14] == 13.0f && world.values[3] == 0.0f &&
		world.values[7] == 0.0f && world.values[11] == 0.0f && world.values[15] == 1.0f,
		"affine translation reaches D3D row-vector constants, preserving homogeneous w");
	// A 90-degree Z rotation followed by translation maps (2,3,5) to
	// (4,13,18). Evaluate the row-vector operation consumed by the shader.
	const float point[4] = { 2.0f, 3.0f, 5.0f, 1.0f };
	const float expected[4] = { 4.0f, 13.0f, 18.0f, 1.0f };
	for (unsigned int column = 0; column < 4; ++column)
	{
		float transformed = 0.0f;
		for (unsigned int row = 0; row < 4; ++row)
			transformed += point[row] * world.values[row * 4 + column];
		result |= Check(transformed == expected[column],
			"shader row-vector transform rotates and translates the known point");
	}
	Matrix4x4 matrix;
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
			matrix[row][column] = static_cast<float>(row * 4 + column + 1);
	sink->ResetObservations();
	SetGameTransform(GAME_TRANSFORM_PROJECTION, matrix);
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
			result |= Check(sink->commandMatrices[0].values[column * 4 + row] == matrix[row][column],
				"typed projection transposes WWMath to D3D");
	sink->transformReply = sink->commandMatrices[0];
	float raw[16];
	GetGameTransform(GAME_TRANSFORM_PROJECTION, raw);
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
			result |= Check(raw[column * 4 + row] == matrix[row][column],
				"opaque GetGameTransform returns renderer row-vector bytes");
	Matrix4x4 typedReply;
	GetGameTransform(GAME_TRANSFORM_PROJECTION, &typedReply);
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
			result |= Check(typedReply[row][column] == matrix[row][column],
				"typed WWMath GetGameTransform preserves column-vector layout");
	sink->ResetObservations();
	SetGameTransform(GAME_TRANSFORM_TEXTURE0, static_cast<const void *>(raw));
	for (unsigned int index = 0; index < 16; ++index)
		result |= Check(sink->commandMatrices[0].values[index] == sink->transformReply.values[index],
			"opaque facade transform roundtrip preserves D3D constants");
	RenderMatrix4x4 rawProjective;
	RenderMatrixTranslation(&rawProjective, 7.0f, 11.0f, 13.0f);
	rawProjective.m[0][3] = 0.125f;
	rawProjective.m[1][2] = 0.75f;
	sink->ResetObservations();
	SetGameTransform(GAME_TRANSFORM_TEXTURE0, &rawProjective);
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
			result |= Check(sink->commandMatrices[0].values[row * 4 + column] ==
				rawProjective.m[row][column],
				"opaque projective transform preserves asymmetric row-vector fields");

	// Terrain noise coordinates use this row-vector Get/inverse/scale/translate
	// chain. A paired transpose can pass a roundtrip while reversing this order.
	RenderMatrix4x4 rawView;
	RenderMatrixTranslation(&rawView, 4.0f, 8.0f, 12.0f);
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
			sink->transformReply.values[row * 4 + column] = rawView.m[row][column];
	RenderMatrix4x4 readView, inverseView, scale, offset, noise;
	GetGameTransform(GAME_TRANSFORM_VIEW, &readView);
	float determinant = 0.0f;
	result |= Check(RenderMatrixInverse(&inverseView, &determinant, &readView),
		"raw view matrix remains invertible for terrain texture coordinates");
	RenderMatrixScaling(&scale, 2.0f, 3.0f, 4.0f);
	RenderMatrixTranslation(&offset, 0.25f, 0.5f, 0.75f);
	RenderMatrixMultiply(&noise, &inverseView, &scale);
	RenderMatrixMultiply(&noise, &noise, &offset);
	sink->ResetObservations();
	SetGameTransform(GAME_TRANSFORM_TEXTURE2, &noise);
	const float cameraPoint[4] = { 4.0f, 8.0f, 12.0f, 1.0f };
	const float expectedNoise[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	for (unsigned int column = 0; column < 4; ++column)
	{
		float transformed = 0.0f;
		for (unsigned int row = 0; row < 4; ++row)
			transformed += cameraPoint[row] *
				sink->commandMatrices[0].values[row * 4 + column];
		result |= Check(transformed == expectedNoise[column],
			"terrain raw matrix arithmetic preserves inverse-scale-translation order");
	}
	CameraClass camera;
	camera.Set_Transform(affine);
	Matrix3D view;
	Matrix4x4 projection;
	camera.Get_View_Matrix(&view);
	camera.Get_D3D_Projection_Matrix(&projection);
	sink->ResetObservations();
	SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1, "translated camera publishes a snapshot");
	for (unsigned int row = 0; row < 4; ++row)
		for (unsigned int column = 0; column < 4; ++column)
		{
			const float expectedView = row < 3 ? view[row][column] :
				(column == 3 ? 1.0f : 0.0f);
			result |= Check(sink->snapshot.view.values[column * 4 + row] == expectedView &&
				sink->snapshot.projection.values[column * 4 + row] == projection[row][column],
				"paired title camera snapshots follow the D3D matrix convention");
		}
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}

class MaterialMapperProbe : public TextureMapperClass
{
public:
	explicit MaterialMapperProbe(CameraOutputSink *sink) :
		TextureMapperClass(0), sink(sink), calls(0), uvSource(-1),
		materialWasPublished(false) {}
	TextureMapperClass *Clone() const override { return 0; }
#if !defined(RTS_GENERALS)
	void Calculate_Texture_Matrix(Matrix4x4 &) override {}
#endif
	void Apply(int source) override
	{
		++calls;
		uvSource = source;
		materialWasPublished = sink->commandCount == 1 &&
			sink->commandTypes[0] == rts::render::GAME_RENDER_COMMAND_SET_MATERIAL;
	}
	CameraOutputSink *sink;
	unsigned int calls;
	int uvSource;
	bool materialWasPublished;
};

int TestMaterialMapperSubmission(CameraOutputSink *sink)
{
	VertexMaterialClass material;
	MaterialMapperProbe *mapper = new MaterialMapperProbe(sink);
	material.Set_Mapper(mapper, 0);
	material.Set_UV_Source(0, 1);
	int result = 0;
	sink->ResetObservations();
	rts::render::SetGameMaterial(&material);
	result |= Check(mapper->calls == 1 && mapper->uvSource == 1 &&
		mapper->materialWasPublished,
		"direct native mesh material publishes its mapper after material state");
	sink->ResetObservations();
	material.Apply();
	result |= Check(mapper->calls == 2 && mapper->materialWasPublished,
		"native VertexMaterial Apply runs its mapper exactly once");
	mapper->Release_Ref();
	return result;
}

class TextureApplyProbe : public TextureClass
{
public:
	TextureApplyProbe(bool publishTexture) :
		TextureClass(1, 1, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
			TextureBaseClass::POOL_MANAGED, false, true, false),
		m_publishTexture(publishTexture), m_applyCalls(0) {}

	virtual void Apply(unsigned int stage) override
	{
		++m_applyCalls;
		rts::render::PublishTextureStage(stage,
			m_publishTexture ? static_cast<TextureBaseClass *>(this) : nullptr);
	}

	bool m_publishTexture;
	unsigned int m_applyCalls;
};

int TestNativeTextureApplyBoundary(CameraOutputSink *sink)
{
	int result = 0;
	TextureApplyProbe guardedTexture(false);
	sink->operational = false;
	sink->ResetObservations();
	rts::render::SetGameTexture(0, &guardedTexture);
	result |= Check(guardedTexture.m_applyCalls == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"native texture binding rejects an unpublished owner before lazy Apply");
	sink->operational = true;

	TextureApplyProbe nullTexture(false);
	sink->ResetObservations();
	rts::render::SetGameTexture(0, &nullTexture);
	result |= Check(nullTexture.m_applyCalls == 1 && sink->failures == 0 &&
		sink->commandCount == 1 &&
		sink->commandTypes[0] ==
		rts::render::GAME_RENDER_COMMAND_SET_TEXTURE,
		"native texture binding applies the texture before honoring a published null texture");

	TextureApplyProbe unavailableTexture(true);
	sink->ResetObservations();
	rts::render::SetGameTexture(1, &unavailableTexture);
	result |= Check(unavailableTexture.m_applyCalls == 1 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		sink->commandCount == 0,
		"native texture binding preserves legitimate acquisition failures after Apply");
	return result;
}

int TestShaderCacheAndDisabledAnisotropy(CameraOutputSink *sink)
{
	using namespace rts::render;
	LegacyPipelineState saved;
	GetTrackedLegacyPipelineState(&saved);
	ShaderClass shader;
	shader.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);
	shader.Set_Src_Blend_Func(ShaderClass::SRCBLEND_SRC_ALPHA);
	shader.Set_Dst_Blend_Func(ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA);
	shader.Apply();
	SeedTrackedLegacyPipelineState();
	shader.Apply();
	LegacyPipelineState restored;
	int result = Check(GetTrackedLegacyPipelineState(&restored) &&
		restored.blend.blendEnable &&
		restored.depthStencil.depthFunction == RENDER_COMPARE_ALWAYS,
		"same native UI shader restores its state after scene cache invalidation");
	TrackLegacyPipelineState(saved);
	ShaderClass::Invalidate();
	TextureFilterClass::_Set_Max_Anisotropy(
		static_cast<TextureFilterClass::AnisotropicFilterMode>(0));
	result |= Check(sink->anisotropyCount == LEGACY_TEXTURE_STAGE_COUNT &&
		sink->anisotropyValid,
		"disabled anisotropy publishes valid 1x sampler state for every stage");
	return result;
}

struct CameraInputs
{
	CameraInputs() : minX(0.125f), minY(0.25f), maxX(0.875f), maxY(0.75f),
		minimumDepth(0.25f), maximumDepth(0.75f), zNear(1.0f), zFar(3.0f),
		projectionType(CameraClass::PERSPECTIVE) {}
	float minX;
	float minY;
	float maxX;
	float maxY;
	float minimumDepth;
	float maximumDepth;
	float zNear;
	float zFar;
	CameraClass::ProjectionType projectionType;
};

void ConfigureCamera(CameraClass *camera, const CameraInputs &input)
{
	camera->Set_Projection_Type(input.projectionType);
	camera->Set_Viewport(Vector2(input.minX, input.minY), Vector2(input.maxX, input.maxY));
	camera->Set_Depth_Range(input.minimumDepth, input.maximumDepth);
	camera->Set_Clip_Planes(input.zNear, input.zFar);
}

int CheckRejectedCamera(CameraOutputSink *sink, CameraClass *priorCamera,
	const CameraInputs &input, const char *message)
{
	CameraClass camera;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	rts::render::SetGameRenderCamera(&camera);
	const int unsafeArithmetic = std::fetestexcept(FE_INVALID | FE_DIVBYZERO);
	return Check(sink->snapshots == 0 && sink->viewportCount == 0 &&
		sink->commandCount == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		TheDX8MeshRenderer.Peek_Camera() == priorCamera && unsafeArithmetic == 0,
		message);
}

int TestCameraOutputs(CameraOutputSink *sink)
{
	int result = 0;
	CameraClass priorCamera;
	CameraClass camera;
	CameraInputs input;
	ConfigureCamera(&camera, input);
	sink->ResetObservations();
	rts::render::SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1 && sink->failures == 0 &&
		sink->snapshot.viewport.x == 12.0f && sink->snapshot.viewport.y == 19.0f &&
		sink->snapshot.viewport.width == 75.0f && sink->snapshot.viewport.height == 39.0f &&
		sink->snapshot.viewport.minimumDepth == 0.25f &&
		sink->snapshot.viewport.maximumDepth == 0.75f &&
		sink->snapshot.zNear == 1.0f && sink->snapshot.zFar == 3.0f &&
		TheDX8MeshRenderer.Peek_Camera() == &camera,
		"real title camera adapter preserves fractional pixel truncation and camera association");

	input.minX = input.minY = 0.0f;
	input.maxX = input.maxY = 1.0f;
	input.minimumDepth = input.maximumDepth = 0.5f;
	ConfigureCamera(&camera, input);
	sink->ResetObservations();
	rts::render::SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1 && sink->failures == 0 &&
		sink->snapshot.viewport.x == 0.0f && sink->snapshot.viewport.y == 0.0f &&
		sink->snapshot.viewport.width == 101.0f && sink->snapshot.viewport.height == 79.0f &&
		sink->snapshot.viewport.minimumDepth == 0.5f &&
		sink->snapshot.viewport.maximumDepth == 0.5f,
		"real title camera adapter accepts normalized boundaries and equal depth endpoints");

	// Physical clipping planes feed divisions inside CameraClass. Unlike the
	// viewport depth interval above, they cannot have equal endpoints.
	input = CameraInputs();
	input.zNear = input.zFar = 3.0f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera adapter rejects equal perspective clip planes before arithmetic or output");

	const float invalidNormalizedValues[] = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), -0.001f, 1.001f
	};
	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 5; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minX, &input.minY, &input.maxX, &input.maxY };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCamera(sink, &priorCamera, input,
				"invalid normalized camera coordinates reject before conversion and snapshot publication");
		}
	}
	const float invalidRectangles[][4] = {
		{ 0.875f, 0.25f, 0.125f, 0.75f },
		{ 0.125f, 0.75f, 0.875f, 0.25f },
		{ 0.5f, 0.25f, 0.5f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5f },
		{ 0.5f, 0.25f, 0.5001f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5001f }
	};
	for (unsigned int rectangle = 0; rectangle != 6; ++rectangle)
	{
		input = CameraInputs();
		input.minX = invalidRectangles[rectangle][0];
		input.minY = invalidRectangles[rectangle][1];
		input.maxX = invalidRectangles[rectangle][2];
		input.maxY = invalidRectangles[rectangle][3];
		result |= CheckRejectedCamera(sink, &priorCamera, input,
			"reversed or zero-pixel-area camera rectangles reject without replacing the prior camera");
	}

	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 4; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minimumDepth, &input.maximumDepth,
				&input.zNear, &input.zFar };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCamera(sink, &priorCamera, input,
				"invalid camera depth metadata rejects before arithmetic and snapshot publication");
		}
	}
	input = CameraInputs();
	input.minimumDepth = 1.001f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera minimum depth above one rejects before snapshot publication");
	input = CameraInputs();
	input.maximumDepth = 1.001f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera maximum depth above one rejects before snapshot publication");
	input = CameraInputs();
	input.minimumDepth = 0.75f;
	input.maximumDepth = 0.25f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"reversed camera depth range rejects before snapshot publication");
	input = CameraInputs();
	input.zNear = 4.0f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"reversed camera clip planes reject before snapshot publication");
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}

int TestCameraUsesSelectedTarget(CameraOutputSink *sink)
{
	using namespace rts::render;
	const RenderBackBufferInfo saved = sink->targetInfo;
	sink->targetInfo.width = 256;
	sink->targetInfo.height = 256;
	CameraClass camera;
	CameraInputs input;
	input.minX = input.minY = 0.0f;
	input.maxX = input.maxY = 1.0f;
	ConfigureCamera(&camera, input);
	sink->ResetObservations();
	SetGameRenderCamera(&camera);
	int result = Check(sink->snapshots == 1 && sink->failures == 0 &&
		sink->snapshot.viewport.x == 0.0f &&
		sink->snapshot.viewport.y == 0.0f &&
		sink->snapshot.viewport.width == 256.0f &&
		sink->snapshot.viewport.height == 256.0f,
		"title camera adapter uses the selected offscreen target extent");
	sink->targetInfo = saved;
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}

int CheckRejectedCameraApply(CameraOutputSink *sink, CameraClass *priorCamera,
	const CameraInputs &input, const char *message)
{
	CameraClass camera;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	camera.Apply();
	const int unsafeArithmetic = std::fetestexcept(FE_INVALID | FE_DIVBYZERO);
	int result = Check(sink->viewportCount == 0 && sink->commandCount == 0 &&
		sink->snapshots == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		TheDX8MeshRenderer.Peek_Camera() == priorCamera && unsafeArithmetic == 0,
		message);

	// Ordinary WW3D::Render calls this adapter after Apply. Check Apply above
	// first so this later, already-guarded boundary cannot mask earlier writes.
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	rts::render::SetGameRenderCamera(&camera);
	result |= Check(sink->viewportCount == 0 && sink->commandCount == 0 &&
		sink->snapshots == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		TheDX8MeshRenderer.Peek_Camera() == priorCamera &&
		std::fetestexcept(FE_INVALID | FE_DIVBYZERO) == 0,
		"guarded camera adapter still rejects after the actual Apply call");
	return result;
}

int TestCameraApplyOutputs(CameraOutputSink *sink)
{
	using namespace rts::render;
	int result = 0;
	CameraClass priorCamera;
	CameraClass camera;
	CameraInputs input;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(&priorCamera);
	sink->ResetObservations();
	camera.Apply();
#if defined(RTS_GENERALS)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[0] == GAME_TRANSFORM_PROJECTION;
#elif defined(RTS_ZEROHOUR)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS;
#else
#error The actual camera Apply fixture requires a paired title interface.
#endif
	result |= Check(sink->viewportCount == 1 && sink->commandCount == 2 &&
		sink->snapshots == 0 && sink->failures == 0 && projectionCommand &&
		sink->commandTypes[1] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[1] == GAME_TRANSFORM_VIEW &&
		sink->viewport.x == 12.0f && sink->viewport.y == 19.0f &&
		sink->viewport.width == 75.0f && sink->viewport.height == 39.0f &&
		sink->viewport.minimumDepth == 0.25f && sink->viewport.maximumDepth == 0.75f &&
		TheDX8MeshRenderer.Peek_Camera() == &priorCamera,
		"actual CameraClass::Apply preserves literal pixels, title commands and prior mesh camera");

	// This finite counterexample also appears in the full corpus below. Keep
	// it separately labelled: its failure does not depend on NaN conversion.
	input = CameraInputs();
	input.minX = -0.001f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects finite negative normalized X before it truncates to pixel zero");

	// Keep this Apply corpus separate from the previously verified adapter
	// test so failure accounting identifies which actual boundary emitted.
	const float invalidNormalizedValues[] = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), -0.001f, 1.001f
	};
	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 5; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minX, &input.minY, &input.maxX, &input.maxY };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCameraApply(sink, &priorCamera, input,
				"actual Apply rejects invalid normalized coordinates before viewport or transform emission");
		}
	}
	const float invalidRectangles[][4] = {
		{ 0.875f, 0.25f, 0.125f, 0.75f },
		{ 0.125f, 0.75f, 0.875f, 0.25f },
		{ 0.5f, 0.25f, 0.5f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5f },
		{ 0.5f, 0.25f, 0.5001f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5001f }
	};
	for (unsigned int rectangle = 0; rectangle != 6; ++rectangle)
	{
		input = CameraInputs();
		input.minX = invalidRectangles[rectangle][0];
		input.minY = invalidRectangles[rectangle][1];
		input.maxX = invalidRectangles[rectangle][2];
		input.maxY = invalidRectangles[rectangle][3];
		result |= CheckRejectedCameraApply(sink, &priorCamera, input,
			"actual Apply rejects reversed or zero-pixel rectangles before viewport or transform emission");
	}
	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 4; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minimumDepth, &input.maximumDepth,
				&input.zNear, &input.zFar };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCameraApply(sink, &priorCamera, input,
				"actual Apply rejects invalid depth metadata before arithmetic or state emission");
		}
	}
	input = CameraInputs();
	input.minimumDepth = 1.001f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects minimum depth above one before state emission");
	input = CameraInputs();
	input.maximumDepth = 1.001f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects maximum depth above one before state emission");
	input = CameraInputs();
	input.minimumDepth = 0.75f;
	input.maximumDepth = 0.25f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects reversed depth range before state emission");
	input = CameraInputs();
	input.zNear = 4.0f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects reversed clip planes before arithmetic or state emission");
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}

bool HasFiniteMatrix(const rts::render::RenderMatrix4 &matrix)
{
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (!std::isfinite(matrix.values[index]))
			return false;
	}
	return true;
}

int TestCameraClipConstruction(CameraOutputSink *sink)
{
	using namespace rts::render;
	int result = 0;
	CameraClass priorCamera;
	CameraInputs input;
	input.zNear = input.zFar = 3.0f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects equal perspective clip planes before arithmetic or output");
	input.projectionType = CameraClass::ORTHO;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera adapter rejects equal orthographic clip planes before arithmetic or output");
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects equal orthographic clip planes before arithmetic or output");

	input = CameraInputs();
	input.zNear = 0.0f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera adapter rejects zero perspective near plane before arithmetic or output");
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects zero perspective near plane before arithmetic or output");

	// The real orthographic matrix implementation permits near=0 when far>near.
	// Require finite outputs from both real call paths so preserving that valid
	// allowance cannot repeat the former metadata-only equal-clip test mistake.
	input.projectionType = CameraClass::ORTHO;
	CameraClass camera;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(&priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1 && sink->viewportCount == 0 &&
		sink->commandCount == 0 && sink->failures == 0 &&
		sink->snapshot.zNear == 0.0f && sink->snapshot.zFar == 3.0f &&
		HasFiniteMatrix(sink->snapshot.view) && HasFiniteMatrix(sink->snapshot.projection) &&
		sink->snapshot.viewport.x == 12.0f && sink->snapshot.viewport.y == 19.0f &&
		sink->snapshot.viewport.width == 75.0f && sink->snapshot.viewport.height == 39.0f &&
		TheDX8MeshRenderer.Peek_Camera() == &camera &&
		std::fetestexcept(FE_INVALID | FE_DIVBYZERO) == 0,
		"camera adapter accepts zero orthographic near plane with finite projection and no unsafe arithmetic");

	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(&priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	camera.Apply();
#if defined(RTS_GENERALS)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[0] == GAME_TRANSFORM_PROJECTION;
#elif defined(RTS_ZEROHOUR)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS;
#else
#error The actual camera clip fixture requires a paired title interface.
#endif
	result |= Check(sink->viewportCount == 1 && sink->commandCount == 2 &&
		sink->snapshots == 0 && sink->failures == 0 && projectionCommand &&
		sink->commandTypes[1] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[1] == GAME_TRANSFORM_VIEW &&
		sink->commandMatrixPresent[0] && sink->commandMatrixPresent[1] &&
		HasFiniteMatrix(sink->commandMatrices[0]) && HasFiniteMatrix(sink->commandMatrices[1]) &&
		sink->viewport.x == 12.0f && sink->viewport.y == 19.0f &&
		sink->viewport.width == 75.0f && sink->viewport.height == 39.0f &&
		TheDX8MeshRenderer.Peek_Camera() == &priorCamera &&
		std::fetestexcept(FE_INVALID | FE_DIVBYZERO) == 0,
		"actual Apply accepts zero orthographic near plane with finite transforms and unchanged mesh camera");
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}
}

class NativeTargetRecoveryHook : public rts::render::GameRenderCleanupHook
{
public:
	NativeTargetRecoveryHook() : color(0), depth(0), failReacquire(false),
		releaseCount(0), reacquireCount(0), drawGateWasClosed(false),
		factoryFailed(false), createdDuringReacquire(false) {}
	~NativeTargetRecoveryHook()
	{
		REF_PTR_RELEASE(color);
		REF_PTR_RELEASE(depth);
	}

	void ReleaseResources() override
	{
		++releaseCount;
		REF_PTR_RELEASE(color);
		REF_PTR_RELEASE(depth);
	}

	void ReAcquireResources() override
	{
		++reacquireCount;
		rts::render::IGameRenderClientNativeOwner *owner =
			rts::render::GetGameRenderClientNativeOwner();
		drawGateWasClosed = owner != 0 && !owner->IsOperational();
		factoryFailed = false;
		createdDuringReacquire = false;

		TextureClass *newColor = 0;
		ZTextureClass *newDepth = 0;
		const int dimension = failReacquire ? 0 : 256;
		const rts::render::RenderResult result =
			rts::render::CreateGameRenderTargetPair(dimension, dimension,
			WW3D_FORMAT_A8R8G8B8, WW3D_ZFORMAT_D24S8, &newColor, &newDepth);
		if (result != rts::render::RENDER_RESULT_OK || newColor == 0 ||
			newDepth == 0)
		{
			factoryFailed = true;
			REF_PTR_RELEASE(newColor);
			REF_PTR_RELEASE(newDepth);
			return;
		}
		REF_PTR_RELEASE(color);
		REF_PTR_RELEASE(depth);
		color = newColor;
		depth = newDepth;
		createdDuringReacquire = true;
	}

	TextureClass *Color() const { return color; }
	ZTextureClass *Depth() const { return depth; }

	TextureClass *color;
	ZTextureClass *depth;
	bool failReacquire;
	unsigned int releaseCount;
	unsigned int reacquireCount;
	bool drawGateWasClosed;
	bool factoryFailed;
	bool createdDuringReacquire;
};

void PrepareNativeOffscreenDrawState()
{
	using namespace rts::render;
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		SetGameTexture(stage, 0);

	LegacyPipelineState pipeline;
	pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	pipeline.depthStencil.depthEnable = false;
	pipeline.depthStencil.depthWrite = false;
	pipeline.lightingEnable = false;
	pipeline.blend.blendEnable = false;
	pipeline.textureStages[0].colorOperation =
		RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
	pipeline.textureStages[0].colorArgument1 = RENDER_TEXTURE_ARG_DIFFUSE;
	pipeline.textureStages[0].alphaOperation =
		RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
	pipeline.textureStages[0].alphaArgument1 = RENDER_TEXTURE_ARG_DIFFUSE;
	TrackLegacyPipelineState(pipeline);

	RenderMatrix4 identity;
	TrackLegacyTransform(LEGACY_TRANSFORM_WORLD, identity.values);
	TrackLegacyTransform(LEGACY_TRANSFORM_VIEW, identity.values);
	TrackLegacyTransform(LEGACY_TRANSFORM_PROJECTION, identity.values);
}

int CheckPublishedGpuTexture(NativeW3D2 *owner, TextureClass *texture,
	unsigned int *authorityEpoch, const char *message)
{
	using namespace rts::render;
	NativeW3DTextureHandle handle;
	NativeW3DGpuContentLease lease;
	NativeW3DTextureDescription description;
	const bool acquired = texture != 0 &&
		texture->Acquire_Native_Texture(&handle, &lease);
	const RenderResult described = acquired ? owner->Resources().DescribeTexture(
		handle.resource, &description) : RENDER_RESULT_INVALID_ARGUMENT;
	if (authorityEpoch != 0)
		*authorityEpoch = described == RENDER_RESULT_OK ?
			description.authorityEpoch : 0;
	return Check(acquired && lease.isValid() &&
		described == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_GPU_RENDER_TARGET &&
		description.authorityEpoch != 0,
		message);
}

int RenderNativeOffscreenPass(NativeW3D2 *owner, TextureClass *color,
	ZTextureClass *depth, const rts::render::GameRenderColor &clearColor,
	unsigned int *authorityEpoch, TextureClass *copyDestination = 0)
{
	using namespace rts::render;
	int result = 0;
	GameTextureRenderPass reflectionPass(color, depth, false);
	result |= Check(reflectionPass.IsReady(),
		"water texture pass selects its target and opens a hidden native frame");
	RenderBackBufferInfo targetInfo;
	result |= Check(owner->ActiveRenderTargetKind() == GAME_RENDER_TARGET_TEXTURE &&
		owner->GetGameRenderTargetInfo(&targetInfo) == RENDER_RESULT_OK &&
		targetInfo.width == 256 && targetInfo.height == 256,
		"water hidden pass uses the exact paired 256x256 output");

	result |= Check(ClearGameRenderTargets(false, true, clearColor, 0.0f) ==
		RENDER_RESULT_OK, "water clears depth immediately after selecting its hidden pass");
	result |= Check(ClearGameRenderTargets(true, false, clearColor, 1.0f) ==
		RENDER_RESULT_OK, "offscreen fixture fills the sky background");
	CameraClass camera;
	CameraInputs cameraInput;
	cameraInput.projectionType = CameraClass::ORTHO;
	cameraInput.minX = cameraInput.minY = 0.0f;
	cameraInput.maxX = cameraInput.maxY = 1.0f;
	cameraInput.zNear = 0.0f;
	cameraInput.zFar = 3.0f;
	ConfigureCamera(&camera, cameraInput);
	SetGameRenderCamera(&camera);
	PrepareNativeOffscreenDrawState();
	VertexFormatXYZNDUV1 vertices[3] = {};
	vertices[0].x = -0.7f;
	vertices[0].y = -0.7f;
	vertices[0].z = 0.5f;
	vertices[0].nz = 1.0f;
	vertices[0].diffuse = 0xffff0000U;
	vertices[1].x = 0.7f;
	vertices[1].y = -0.7f;
	vertices[1].z = 0.5f;
	vertices[1].nz = 1.0f;
	vertices[1].diffuse = 0xffff0000U;
	vertices[2].x = 0.0f;
	vertices[2].y = 0.7f;
	vertices[2].z = 0.5f;
	vertices[2].nz = 1.0f;
	vertices[2].diffuse = 0xffff0000U;
	const RenderResult drawResult = DrawGamePrimitiveUP(
		GAME_PRIMITIVE_TRIANGLE_LIST, 1, vertices, sizeof(vertices[0]),
		GAME_VERTEX_XYZNDUV1);
	result |= Check(drawResult == RENDER_RESULT_OK,
		"native facade renders distinct offscreen color data");
	if (copyDestination != 0)
		result |= Check(CopyGameActiveTargetToTexture(copyDestination) ==
			RENDER_RESULT_OK, "projected shadow copies into an ordinary sampled destination");
	reflectionPass.RestoreTarget();
	SetGameRenderCamera(&camera);
	const RenderResult endResult = reflectionPass.Finish();
	result |= Check(endResult == RENDER_RESULT_OK,
		"native facade ends the non-presented offscreen frame");
	if (copyDestination != 0)
		result |= Check(AcquireGameCopiedTextureContent(copyDestination),
			"projected shadow publishes its copied content after hidden End_Render");

	// The pass restores its output before sealing the non-presented frame.
	SetGameRenderTarget(0, 0, true);
	RenderBackBufferInfo backBufferInfo;
	result |= Check(owner->ActiveRenderTargetKind() == GAME_RENDER_TARGET_BACK_BUFFER &&
		owner->GetGameRenderTargetInfo(&backBufferInfo) == RENDER_RESULT_OK &&
		backBufferInfo.width == 101 && backBufferInfo.height == 79,
		"native facade restores the bootstrap-sized back buffer outside the frame");
	result |= CheckPublishedGpuTexture(owner, color, authorityEpoch,
		"offscreen output publishes GPU authority and a current sampling lease");
	return result;
}

int RenderNativeMainPass(NativeW3D2 *owner)
{
	using namespace rts::render;
	int result = 0;
	RenderBackBufferInfo targetInfo;
	result |= Check(owner->GetGameRenderTargetInfo(&targetInfo) ==
		RENDER_RESULT_OK && targetInfo.width == 101 && targetInfo.height == 79,
		"main target query returns the bootstrap back buffer extent after restore");
	const GameRenderColor clearColor = { 0.02f, 0.03f, 0.04f, 1.0f };
	result |= Check(BeginGameRender(true, true, clearColor, 1.0f) ==
		RENDER_RESULT_OK, "native facade begins the restored main frame");
	CameraClass camera;
	CameraInputs cameraInput;
	cameraInput.minX = cameraInput.minY = 0.0f;
	cameraInput.maxX = cameraInput.maxY = 1.0f;
	cameraInput.zNear = 1.0f;
	cameraInput.zFar = 3.0f;
	ConfigureCamera(&camera, cameraInput);
	SetGameRenderCamera(&camera);
	const RenderResult endResult = EndGameRender(false);
	result |= Check(endResult == RENDER_RESULT_OK,
		"main camera frame ends without a recorded camera failure");
	result |= Check(owner->ActiveRenderTargetKind() == GAME_RENDER_TARGET_BACK_BUFFER,
		"main camera frame keeps the restored back buffer active");
	return result;
}

int TestNativeOffscreenFacade(HWND window,
	rts::render::IGameRenderClientNativeOwner *actualOwner,
	NativeTargetRecoveryHook *hook)
{
	using namespace rts::render;
	NativeW3D2 *owner = static_cast<NativeW3D2 *>(actualOwner);
	int result = 0;
	int bootstrapWidth = 0;
	int bootstrapHeight = 0;
	int bootstrapBitDepth = 0;
	bool bootstrapWindowed = false;
	result |= Check(GetGameRendererTargetResolution(&bootstrapWidth,
		&bootstrapHeight, &bootstrapBitDepth, &bootstrapWindowed) ==
		RENDER_RESULT_OK && bootstrapWidth == 101 && bootstrapHeight == 79,
		"offscreen fixture records the 101x79 bootstrap dimensions");

	TextureClass *color = 0;
	ZTextureClass *depth = 0;
	const RenderResult createResult = CreateGameRenderTargetPair(256, 256,
		WW3D_FORMAT_A8R8G8B8, WW3D_ZFORMAT_D24S8, &color, &depth);
	result |= Check(createResult == RENDER_RESULT_OK && color != 0 &&
		depth != 0 && color->Is_Initialized() && depth->Is_Initialized(),
		"offscreen fixture creates an initialized color/depth target pair");
	if (createResult != RENDER_RESULT_OK || color == 0 || depth == 0)
	{
		REF_PTR_RELEASE(color);
		REF_PTR_RELEASE(depth);
		return result;
	}

	const GameRenderColor firstClear = { 0.05f, 0.10f, 0.15f, 1.0f };
	// Match W3DShadowTexture::init: this is a sampled texture, not an RTV.
	TextureClass *shadow = new TextureClass(256, 256,
		WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1);
	NativeW3DTextureHandle shadowHandle;
	NativeW3DTextureDescription shadowDescription;
	result |= Check(shadow->Acquire_Native_Texture(&shadowHandle) &&
		owner->Resources().DescribeTexture(shadowHandle.resource,
			&shadowDescription) == RENDER_RESULT_OK &&
		(shadowDescription.descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) == 0 &&
		shadowDescription.descriptor.usage == RENDER_USAGE_DEFAULT,
		"shadow fixture starts as an ordinary sampled texture without an RTV");
	unsigned int firstEpoch = 0;
	result |= RenderNativeOffscreenPass(owner, color, depth, firstClear,
		&firstEpoch, shadow);
	const GameRenderColor secondClear = { 0.60f, 0.20f, 0.05f, 1.0f };
	unsigned int secondEpoch = 0;
	result |= RenderNativeOffscreenPass(owner, color, depth, secondClear,
		&secondEpoch);
	result |= Check(secondEpoch > firstEpoch,
		"repeated offscreen passes advance the published GPU authority epoch");
	{
		GameTextureRenderPass earlyExit(color, depth, false);
		result |= Check(earlyExit.IsReady(),
			"water early-exit fixture opens its hidden frame");
		// Like renderMirror's clear failure return, leave without explicit Finish.
	}
	result |= Check(owner->ActiveRenderTargetKind() == GAME_RENDER_TARGET_BACK_BUFFER,
		"water early-exit cleanup restores the default target");
	result |= RenderNativeMainPass(owner);

	// Keep GPU-authored resources alive across resize and actual device recovery.
	TextureClass *oldColor = color;
	oldColor->Add_Ref();
	REF_PTR_RELEASE(color);
	REF_PTR_RELEASE(depth);

	SetGameCleanupHook(hook);
	const RenderResult recoveryResult = SetGameRendererResolution(101, 79, 32,
		1, true);
	result |= Check(recoveryResult == RENDER_RESULT_OK &&
		hook->releaseCount == 1 && hook->reacquireCount == 1 &&
		hook->drawGateWasClosed && hook->createdDuringReacquire &&
		hook->Color() != 0 && hook->Depth() != 0 &&
		owner->IsOperational() &&
		owner->ActiveRenderTargetKind() == GAME_RENDER_TARGET_BACK_BUFFER,
		"resize recovery closes the draw gate while recreating a real target pair");
	NativeW3DTextureHandle staleHandle;
	result |= Check(oldColor->Acquire_Native_Texture(&staleHandle) &&
		shadow->Acquire_Native_Texture(&shadowHandle),
		"ordinary resize preserves still-valid GPU content of retained textures");
	result |= Check(hook->Color() != 0 && hook->Color()->Is_Initialized() &&
		hook->Depth() != 0 && hook->Depth()->Is_Initialized(),
		"the fresh cleanup-hook target pair remains usable after recovery");

	const GameRenderColor recoveredClear = { 0.10f, 0.55f, 0.25f, 1.0f };
	unsigned int recoveredEpoch = 0;
	result |= RenderNativeOffscreenPass(owner, hook->Color(), hook->Depth(),
		recoveredClear, &recoveredEpoch);
	result |= Check(recoveredEpoch != 0,
		"the recreated target publishes fresh GPU authority after recovery");

	const RenderResult deviceRecoveryResult = owner->RecoverDevice();
	result |= Check(deviceRecoveryResult == RENDER_RESULT_OK,
		"actual device recovery succeeds outside a publication lifecycle scope");
	result |= Check(owner->IsOperational(),
		"actual device recovery reopens owner admission");
	result |= Check(hook->releaseCount == 2 && hook->reacquireCount == 2 &&
		hook->createdDuringReacquire,
		"actual device recovery invokes both hooks and rebuilds the title target pair");
	result |= Check(!oldColor->Acquire_Native_Texture(&staleHandle) &&
		!shadow->Acquire_Native_Texture(&shadowHandle),
		"actual device recovery invalidates GPU content without CPU-zero fallback");
	result |= RenderNativeOffscreenPass(owner, hook->Color(), hook->Depth(),
		recoveredClear, &recoveredEpoch, shadow);
	result |= Check(shadow->Acquire_Native_Texture(&shadowHandle),
		"a retained ordinary shadow destination is sampleable after GPU regeneration");

	// A void cleanup callback can ignore a failed factory call. The owner must
	// retain that recorded failure and leave all frame/draw admission closed.
	hook->failReacquire = true;
	const RenderResult failedRecovery = SetGameRendererResolution(101, 79, 32,
		1, true);
	result |= Check(failedRecovery != RENDER_RESULT_OK && hook->factoryFailed &&
		hook->releaseCount == 3 && hook->reacquireCount == 3 &&
		hook->drawGateWasClosed &&
		hook->Color() == 0 && hook->Depth() == 0 && !owner->IsOperational(),
		"a failed reacquire factory keeps outputs null and frame admission closed");

	oldColor->Release_Ref();
	shadow->Release_Ref();
	(void)window;
	return result;
}

int TestSortedFacadeAndStrip(HWND window)
{
	using namespace rts::render;
	int result = 0;
	NativeW3D2 owner;
	NativeW3DRendererDescriptor descriptor;
	descriptor.width = descriptor.height = 64;
	descriptor.enableVsync = false;
	if (owner.Initialize(window, descriptor) != RENDER_RESULT_OK)
		return Check(false, "sorting facade fixture initializes");
	VertexFormatXYZNDUV2 vertices[6] = {};
	for (unsigned int i = 0; i < 6; ++i)
	{
		vertices[i].x = i < 3 ? 10.0f : (i == 4 ? 0.5f : -0.5f);
		vertices[i].y = i == 5 ? 0.5f : -0.5f;
		vertices[i].z = 0.5f;
		vertices[i].nz = 1.0f;
		vertices[i].diffuse = 0xff00ff00U;
	}
	const unsigned short indices[3] = { 0, 1, 2 };
	// Keep this first: sorted flushes below retain two transient native buffers.
	BufferDescriptor vbDescriptor;
	vbDescriptor.byteCount = 3 * sizeof(VertexFormatXYZNDUV2);
	vbDescriptor.stride = sizeof(VertexFormatXYZNDUV2);
	vbDescriptor.binding = RENDER_BUFFER_VERTEX;
	BufferDescriptor ibDescriptor;
	ibDescriptor.byteCount = sizeof(indices);
	ibDescriptor.stride = sizeof(unsigned short);
	ibDescriptor.binding = RENDER_BUFFER_INDEX;
	GpuHandle vbHandle, ibHandle;
	result |= Check(owner.Resources().CreateBuffer(vbDescriptor, vertices + 3, vbDescriptor.byteCount, &vbHandle) == RENDER_RESULT_OK &&
		owner.Resources().CreateBuffer(ibDescriptor, indices, sizeof(indices), &ibHandle) == RENDER_RESULT_OK,
		"strip fixture creates exactly three indices");
	result |= Check(vbHandle.index() == 0 && vbHandle.generation() != 0 &&
		owner.Resources().IsValid(vbHandle),
		"strip fixture exercises a live native allocation in slot zero");
	unsigned char referencePixels[64 * 64 * 4] = {};
	bool haveReferencePixels = false;
	auto begin = [&]() {
		result |= Check(owner.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"sorting facade begins a frame");
		owner.Renderer().SetViewport(RenderViewport(0, 0, 64, 64, 0, 1));
		owner.Renderer().ClearExternal(RENDER_CLEAR_COLOR, RenderFloat4(0, 0, 0, 1), 1, 0);
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
			SetGameTexture(stage, 0);
		RenderMatrix4 identity;
		TrackLegacyTransform(LEGACY_TRANSFORM_WORLD, identity.values);
		TrackLegacyTransform(LEGACY_TRANSFORM_VIEW, identity.values);
		TrackLegacyTransform(LEGACY_TRANSFORM_PROJECTION, identity.values);
		LegacyPipelineState pipeline;
		pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
		pipeline.depthStencil.depthEnable = false;
		pipeline.depthStencil.depthWrite = false;
		pipeline.lightingEnable = false;
		pipeline.blend.blendEnable = false;
		pipeline.textureStages[0].colorOperation = RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		pipeline.textureStages[0].colorArgument1 = RENDER_TEXTURE_ARG_DIFFUSE;
		TrackLegacyPipelineState(pipeline);
	};
	auto finish = [&](bool compareSortedPixels) {
		const RenderResult end = owner.Renderer().EndFrame(false);
		result |= Check(end == RENDER_RESULT_OK, "sorting facade frame retains valid commands");
		if (end == RENDER_RESULT_OK)
			result |= Check(owner.Renderer().FinalizeEndedFrame(false) == RENDER_RESULT_OK,
				"sorting facade seals its frame");
		result |= Check(owner.Renderer().DrainThreaded() == RENDER_RESULT_OK,
			"sorting facade executes its frame");
		unsigned char pixels[64 * 64 * 4] = {};
		RenderFormat format;
		const RenderResult capture = owner.Renderer().CaptureBackBuffer(pixels,
			sizeof(pixels), 64 * 4, &format);
		unsigned int green = 0;
		for (unsigned int i = 0; i < sizeof(pixels); i += 4)
			if (pixels[i + 1] > 128 && pixels[i] < 40 && pixels[i + 2] < 40) ++green;
		result |= Check(capture == RENDER_RESULT_OK && green > 16,
			"sorting facade selects the visible base-offset vertices, not the offscreen prefix");
		if (capture == RENDER_RESULT_OK && compareSortedPixels)
		{
			if (!haveReferencePixels)
			{
				std::memcpy(referencePixels, pixels, sizeof(pixels));
				haveReferencePixels = true;
			}
			else
				result |= Check(std::memcmp(referencePixels, pixels,
					sizeof(pixels)) == 0,
					"sorted snapshot rebinds preserve exact rendered pixels");
		}
	};
	SortingVertexBufferClass *staticVB = new SortingVertexBufferClass(6);
	SortingIndexBufferClass *staticIB = new SortingIndexBufferClass(6);
	{
		VertexBufferClass::AppendLockClass lock(staticVB, 0, 6);
		if (lock.Get_Vertex_Array() != 0) std::memcpy(lock.Get_Vertex_Array(), vertices, sizeof(vertices));
		result |= Check(lock.Commit(), "static sorting vertex prefix is initialized");
		IndexBufferClass::AppendLockClass indexLock(staticIB, 0, 3);
		if (indexLock.Get_Index_Array() != 0) std::memcpy(indexLock.Get_Index_Array(), indices, sizeof(indices));
		result |= Check(indexLock.Commit(), "static sorting index prefix is initialized");
	}
	begin();
	SetGameVertexBuffer(staticVB);
	SetGameIndexBuffer(staticIB, 3);
	DrawGameSortedTriangles(0, 1, 0, 3);
	result |= Check(FlushGameSortedTriangles() == RENDER_RESULT_OK,
		"static sorting facade accepts initialized prefixes and a nonzero vertex base");
	finish(true);
	staticVB->Release_Ref();
	staticIB->Release_Ref();
	for (unsigned int allocation = 0; allocation < 2; ++allocation)
	{
		DynamicVBAccessClass vb(BUFFER_TYPE_DYNAMIC_SORTING, GAME_VERTEX_XYZNDUV2, 6);
		DynamicIBAccessClass ib(BUFFER_TYPE_DYNAMIC_SORTING, 3);
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb);
			if (lock.Get_Formatted_Vertex_Array() != 0) std::memcpy(lock.Get_Formatted_Vertex_Array(), vertices, sizeof(vertices));
			result |= Check(lock.Commit(), "dynamic sorting vertices are initialized");
			DynamicIBAccessClass::WriteLockClass indexLock(&ib);
			if (indexLock.Get_Index_Array() != 0) std::memcpy(indexLock.Get_Index_Array(), indices, sizeof(indices));
			result |= Check(indexLock.Commit(), "dynamic sorting indices are initialized");
		}
		if (allocation != 0)
			result |= Check(vb.Get_Vertex_Buffer_Offset() != 0 && ib.Get_Index_Buffer_Offset() != 0,
				"second sorting allocation has nonzero physical pool offsets");
		begin();
		result |= Check(SetGameVertexBuffer(vb) && SetGameIndexBuffer(ib, allocation == 0 ? 3 : 0),
			"dynamic sorting facade binds allocation-relative snapshots");
		if (allocation != 0) SetGameIndexBufferOffset(3);
		DrawGameSortedTriangles(0, 1, 0, 3);
		result |= Check(FlushGameSortedTriangles() == RENDER_RESULT_OK,
			"dynamic sorting draws use relative indices and the current vertex base");
		finish(true);
	}
	const unsigned short repeatedIndices[6] = { 0, 1, 2, 0, 1, 2 };
	for (unsigned int iteration = 0; iteration < 4; ++iteration)
	{
		const unsigned short vertexCount = (iteration & 1U) ? 3 : 6;
		const unsigned short indexCount = (iteration & 1U) ? 3 : 6;
		DynamicVBAccessClass vb(BUFFER_TYPE_DYNAMIC_SORTING,
			GAME_VERTEX_XYZNDUV2, vertexCount);
		DynamicIBAccessClass ib(BUFFER_TYPE_DYNAMIC_SORTING, indexCount);
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb);
			if (lock.Get_Formatted_Vertex_Array() != 0)
				std::memcpy(lock.Get_Formatted_Vertex_Array(),
					vertexCount == 6 ? vertices : vertices + 3,
					vertexCount * sizeof(vertices[0]));
			result |= Check(lock.Commit(), "variable-size sorted vertices are initialized");
			DynamicIBAccessClass::WriteLockClass indexLock(&ib);
			if (indexLock.Get_Index_Array() != 0)
				std::memcpy(indexLock.Get_Index_Array(), repeatedIndices,
					indexCount * sizeof(indices[0]));
			result |= Check(indexLock.Commit(), "variable-size sorted indices are initialized");
		}
		begin();
		result |= Check(SetGameVertexBuffer(vb) &&
			SetGameIndexBuffer(ib, vertexCount == 6 ? 3 : 0),
			"variable-size sorted snapshots bind on repeated frames");
		if (iteration == 3)
		{
			// The owner must retain its own bytes before the caller reuses them.
			std::memset(const_cast<void *>(vb.Get_Sorted_Vertex_Data()), 0,
				vertexCount * sizeof(vertices[0]));
			std::memset(const_cast<unsigned short *>(ib.Get_Sorted_Index_Data()),
				0, indexCount * sizeof(indices[0]));
		}
		DrawGameSortedTriangles(0, 1, 0, 3);
		result |= Check(FlushGameSortedTriangles() == RENDER_RESULT_OK,
			"variable-size sorted snapshots keep draw order and indices");
		finish(true);
	}
	begin();
	GameRenderCommand command = {};
	command.type = GAME_RENDER_COMMAND_SET_VERTEX_BUFFER;
	command.resource0.index = vbHandle.index(); command.resource0.generation = vbHandle.generation();
	command.value0 = GAME_VERTEX_XYZNDUV2; command.value1 = sizeof(VertexFormatXYZNDUV2);
	const RenderResult stripBind = owner.ExecuteGameRenderCommand(command);
	if (stripBind != RENDER_RESULT_OK)
	{
		NativeW3DBufferDescription description;
		const RenderResult described = owner.Resources().DescribeBuffer(vbHandle, &description);
		std::fprintf(stderr, "strip binding result=%u handle=%u:%u valid=%u described=%u stride=%u bytes=%llu fvf=%x\n",
			stripBind, vbHandle.index(), vbHandle.generation(), owner.Resources().IsValid(vbHandle),
			described, description.descriptor.stride,
			static_cast<unsigned long long>(description.descriptor.byteCount), command.value0);
	}
	result |= Check(stripBind == RENDER_RESULT_OK, "strip binds its vertex buffer");
	command = GameRenderCommand();
	command.type = GAME_RENDER_COMMAND_SET_INDEX_BUFFER;
	command.resource0.index = ibHandle.index(); command.resource0.generation = ibHandle.generation();
	command.value0 = RENDER_FORMAT_R16_UINT;
	result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_OK, "strip binds its index buffer");
	DrawGameStrip(0, 1, 0, 3);
	finish(false);
	begin();
	{
		DynamicVBAccessClass vb(BUFFER_TYPE_DYNAMIC_SORTING,
			GAME_VERTEX_XYZNDUV2, 6);
		DynamicIBAccessClass ib(BUFFER_TYPE_DYNAMIC_SORTING, 3);
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb);
			if (lock.Get_Formatted_Vertex_Array() != 0)
				std::memcpy(lock.Get_Formatted_Vertex_Array(), vertices,
					sizeof(vertices));
			result |= Check(lock.Commit(), "retained sorted vertex snapshot is initialized");
			DynamicIBAccessClass::WriteLockClass indexLock(&ib);
			if (indexLock.Get_Index_Array() != 0)
				std::memcpy(indexLock.Get_Index_Array(), indices,
					sizeof(indices));
			result |= Check(indexLock.Commit(), "retained sorted index snapshot is initialized");
		}
		result |= Check(SetGameVertexBuffer(vb) && SetGameIndexBuffer(ib, 3),
			"valid sorted snapshot binds before invalid replacements");
		GameRenderCommand rejected = {};
		rejected.type = GAME_RENDER_COMMAND_SET_VERTEX_BUFFER;
		rejected.value0 = GAME_VERTEX_XYZNDUV2;
		rejected.value1 = sizeof(VertexFormatXYZNDUV2);
		rejected.value3 = 6;
		rejected.input = vb.Get_Sorted_Vertex_Data();
		rejected.inputBytes = 6 * sizeof(VertexFormatXYZNDUV2) - 1;
		result |= Check(owner.ExecuteGameRenderCommand(rejected) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"invalid sorted vertex size rejects replacement");
		rejected = GameRenderCommand();
		rejected.type = GAME_RENDER_COMMAND_SET_INDEX_BUFFER;
		rejected.value0 = RENDER_FORMAT_R16_UINT;
		rejected.value2 = 3;
		rejected.input = ib.Get_Sorted_Index_Data();
		rejected.inputBytes = sizeof(indices) - 1;
		result |= Check(owner.ExecuteGameRenderCommand(rejected) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"invalid sorted index size rejects replacement");
		DrawGameSortedTriangles(0, 1, 0, 3);
		result |= Check(FlushGameSortedTriangles() == RENDER_RESULT_OK,
			"invalid sorted replacements preserve the prior draw binding");
	}
	command = GameRenderCommand();
	command.type = GAME_RENDER_COMMAND_DRAW_STRIP; command.value3 = 3;
	result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_INVALID_ARGUMENT,
		"strip rejects zero primitives");
	command.value1 = UINT_MAX;
	result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_INVALID_ARGUMENT,
		"strip rejects primitive-count overflow");
	command.value1 = 2;
	result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_INVALID_ARGUMENT,
		"strip checks the additional two indices against the bound range");
	command = GameRenderCommand();
	command.type = GAME_RENDER_COMMAND_SET_VERTEX_BUFFER;
	command.resource0.index = 1;
	result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_INVALID_ARGUMENT,
		"a non-null game handle cannot omit its generation");
	command.resource0.index = UINT_MAX; command.resource0.generation = 1;
	result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_INVALID_ARGUMENT,
		"the native invalid-index sentinel cannot become a live game handle");
	result |= Check(owner.Renderer().EndFrame(false) == RENDER_RESULT_INVALID_ARGUMENT,
		"invalid strip commands remain frame failures");
	owner.Renderer().DrainThreaded();
	DynamicVBAccessClass::_Deinit();
	DynamicIBAccessClass::_Deinit();
	owner.Resources().Destroy(vbHandle); owner.Resources().Destroy(ibHandle);
	result |= Check(owner.Shutdown() == RENDER_RESULT_OK, "sorting facade fixture shuts down");
	return result;
}

int main()
{
	HWND window = CreateHiddenWindow();
	if (window == 0)
		return Check(false, "camera adapter fixture creates a hidden window");
	const rts::render::RenderResult initialized = rts::render::InitializeGameRenderer(
		window, 101, 79, false, false);
	if (initialized != rts::render::RENDER_RESULT_OK)
	{
		DestroyWindow(window);
		return initialized == rts::render::RENDER_RESULT_UNSUPPORTED ? 77 :
			Check(false, "camera adapter fixture initializes the real native bootstrap");
	}

	int result = 0;
	CameraOutputSink sink;
	NativeTargetRecoveryHook recoveryHook;
	rts::render::IGameRenderClientNativeOwner *actualOwner = 0;
	{
		rts::render::NativeGameRenderOwnerLifecycleScope lifecycle;
		if (lifecycle.IsAcquired())
		{
			actualOwner = lifecycle.Get();
			lifecycle.Publish(&sink);
		}
	}
	if (actualOwner != 0)
	{
		result |= TestCameraOutputs(&sink);
		result |= TestCameraUsesSelectedTarget(&sink);
		result |= TestCameraApplyOutputs(&sink);
		result |= TestCameraClipConstruction(&sink);
		result |= TestMaterialMapperSubmission(&sink);
		result |= TestNativeTextureApplyBoundary(&sink);
		result |= TestTransformConvention(&sink);
		result |= TestShaderCacheAndDisabledAnisotropy(&sink);
		{
			rts::render::NativeGameRenderOwnerLifecycleScope lifecycle;
			result |= Check(lifecycle.IsAcquired(), "camera fixture reacquires its owner publication gate");
			if (lifecycle.IsAcquired())
				lifecycle.Publish(actualOwner);
		}
		result |= TestNativeOffscreenFacade(window, actualOwner, &recoveryHook);
	}
	else
		result |= Check(false, "camera fixture observes the real native owner publication");
	result |= Check(rts::render::ShutdownGameRenderer() == rts::render::RENDER_RESULT_OK,
		"camera fixture restores and shuts down the real native owner");
	result |= TestSortedFacadeAndStrip(window);
	DestroyWindow(window);
	return result;
}
