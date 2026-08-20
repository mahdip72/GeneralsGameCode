#include "Renderer/RendererDevice.h"
#include "Renderer/LegacyRenderState.h"

#include <stdio.h>
#include <vector>

#if defined(RTS_RENDERER_HAS_D3D11)
#include <windows.h>
#endif

namespace
{
int check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int testBackendNames()
{
	int result = 0;
	rts::render::RenderBackend backend = rts::render::RENDER_BACKEND_D3D11;
	result |= check(rts::render::ParseRenderBackend("dx8", &backend) &&
		backend == rts::render::RENDER_BACKEND_DX8,
		"dx8 backend name parses");
	result |= check(rts::render::ParseRenderBackend("D3D11", &backend) &&
		backend == rts::render::RENDER_BACKEND_D3D11,
		"d3d11 backend name is case insensitive");
	result |= check(!rts::render::ParseRenderBackend("dx12", &backend),
		"unsupported backend name is rejected");
	result |= check(!rts::render::ParseRenderBackend(0, &backend) &&
		!rts::render::ParseRenderBackend("dx8", 0),
		"backend parser rejects null inputs");
	result |= check(rts::render::RenderBackendName(
		rts::render::RENDER_BACKEND_DX8)[0] == 'd' &&
		rts::render::RenderBackendName(
		rts::render::RENDER_BACKEND_D3D11)[3] == '1',
		"backend names are stable command-line values");
	rts::render::SetRequestedRenderBackend(rts::render::RENDER_BACKEND_D3D11);
	result |= check(rts::render::RequestedRenderBackend() ==
		rts::render::RENDER_BACKEND_D3D11,
		"startup command-line selection reaches the renderer boundary");
	rts::render::SetRequestedRenderBackend(rts::render::RENDER_BACKEND_DX8);
	return result;
}

int testGenerationSafeHandles()
{
	int result = 0;
	rts::render::GpuHandleAllocator allocator(2);
	const rts::render::GpuHandle first = allocator.allocate();
	const rts::render::GpuHandle second = allocator.allocate();
	result |= check(first.isValid() && second.isValid() && first != second,
		"bounded allocator returns unique live handles");
	result |= check(allocator.liveCount() == 2 &&
		allocator.allocate() == rts::render::GpuHandle(),
		"allocator rejects capacity overflow with an invalid handle");
	result |= check(allocator.isLive(first) && allocator.release(first) &&
		!allocator.isLive(first),
		"released handles immediately become stale");
	const rts::render::GpuHandle replacement = allocator.allocate();
	result |= check(replacement.isValid() &&
		replacement.index() == first.index() &&
		replacement.generation() != first.generation(),
		"reused slots advance their generation");
	result |= check(!allocator.release(first) && allocator.isLive(replacement),
		"stale release cannot destroy a replacement resource");
	result |= check(allocator.release(second) && allocator.release(replacement) &&
		allocator.liveCount() == 0,
		"every live handle can be drained deterministically");
	return result;
}

int testNeutralDescriptorDefaults()
{
	int result = 0;
	rts::render::RenderDeviceParameters device;
	rts::render::BufferDescriptor buffer;
	rts::render::TextureDescriptor texture;
	result |= check(device.backend == rts::render::RENDER_BACKEND_DX8 &&
		device.width == 0 && device.height == 0 && device.window == 0 &&
		!device.enableDebugLayer && device.enableVsync,
		"device parameters preserve the legacy backend by default");
	result |= check(buffer.byteCount == 0 && buffer.stride == 0 &&
		buffer.binding == rts::render::RENDER_BUFFER_VERTEX &&
		buffer.usage == rts::render::RENDER_USAGE_IMMUTABLE,
		"buffer descriptor has deterministic defaults");
	result |= check(texture.width == 0 && texture.height == 0 &&
		texture.mipCount == 1 && texture.arrayCount == 1 &&
		texture.format == rts::render::RENDER_FORMAT_UNKNOWN &&
		texture.binding == rts::render::RENDER_TEXTURE_SHADER_RESOURCE,
		"texture descriptor has deterministic defaults");
	return result;
}

int testLegacyLogicalState()
{
	int result = 0;
	rts::render::LegacyLogicalState state;
	result |= check(state.pipeline.depthStencil.depthEnable &&
		state.pipeline.depthStencil.depthWrite &&
		state.pipeline.depthStencil.depthFunction ==
			rts::render::RENDER_COMPARE_LESS_EQUAL &&
		!state.pipeline.blend.blendEnable &&
		state.pipeline.rasterizer.cullMode == rts::render::RENDER_CULL_BACK,
		"legacy logical state starts with deterministic depth and raster defaults");
	result |= check(state.constants.world.values[0] == 1.0f &&
		state.constants.world.values[5] == 1.0f &&
		state.constants.world.values[10] == 1.0f &&
		state.constants.world.values[15] == 1.0f &&
		state.constants.material.diffuse.w == 1.0f,
		"legacy transforms and material use identity/opaque defaults");
	result |= check(!state.constants.lights[0].enabled &&
		!state.constants.fog.enabled &&
		state.pipeline.textureStages[0].colorOperation ==
			rts::render::RENDER_TEXTURE_OP_MODULATE &&
		state.pipeline.textureStages[1].colorOperation ==
			rts::render::RENDER_TEXTURE_OP_DISABLE,
		"legacy lights, fog, and texture stages have stable defaults");

	const rts::render::LegacyShaderKey baseline =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	const rts::render::LegacyShaderKey repeated =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	if (baseline != repeated)
	{
		for (unsigned int word = 0;
			word < rts::render::LegacyShaderKey::WORD_COUNT; ++word)
		{
			if (baseline.words[word] != repeated.words[word])
			{
				fprintf(stderr, "Shader key word %u differs: %08x != %08x\n",
					word, baseline.words[word], repeated.words[word]);
			}
		}
	}
	result |= check(baseline == repeated,
		"identical logical states produce identical shader keys");
	state.pipeline.textureStages[0].colorOperation =
		rts::render::RENDER_TEXTURE_OP_ADD_SIGNED_2X;
	const rts::render::LegacyShaderKey combinerKey =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	state.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
	const rts::render::LegacyShaderKey fogKey =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	state.pipeline.textureStages[0].colorArgument1Complement = true;
	const rts::render::LegacyShaderKey modifierKey =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	result |= check(combinerKey != baseline && fogKey != combinerKey &&
		modifierKey != fogKey,
		"shader key distinguishes texture combiner, fog, and argument modifiers");
	return result;
}

unsigned int makeLegacyShaderBits(unsigned int depthCompare,
	unsigned int depthWrite, unsigned int colorWrite,
	unsigned int sourceBlend, unsigned int destinationBlend,
	unsigned int fog, unsigned int primaryGradient,
	unsigned int secondaryGradient, unsigned int texturing,
	unsigned int alphaTest, unsigned int cull,
	unsigned int detailColor, unsigned int detailAlpha)
{
	return (depthCompare << 0) | (depthWrite << 3) |
		(colorWrite << 4) | (destinationBlend << 5) | (fog << 8) |
		(primaryGradient << 10) | (secondaryGradient << 13) |
		(sourceBlend << 14) | (texturing << 16) |
		(alphaTest << 18) | (cull << 19) |
		(detailColor << 20) | (detailAlpha << 24);
}

int testLegacyShaderBitDecoder()
{
	int result = 0;
	rts::render::LegacyPipelineState state;
	result |= check(!rts::render::DecodeLegacyShaderBits(0, 0),
		"legacy shader decoder rejects a null destination");

	const unsigned int opaque = makeLegacyShaderBits(3, 1, 1, 1, 0,
		0, 1, 0, 1, 0, 1, 0, 0);
	result |= check(rts::render::DecodeLegacyShaderBits(opaque, &state) &&
		state.shaderBits == opaque && state.depthStencil.depthEnable &&
		state.depthStencil.depthWrite &&
		state.depthStencil.depthFunction == rts::render::RENDER_COMPARE_LESS_EQUAL &&
		!state.blend.blendEnable && state.blend.colorWriteMask == 0x0fU &&
		state.rasterizer.cullMode == rts::render::RENDER_CULL_BACK &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_MODULATE &&
		state.textureStages[0].colorArgument1 == rts::render::RENDER_TEXTURE_ARG_TEXTURE &&
		state.textureStages[0].colorArgument2 == rts::render::RENDER_TEXTURE_ARG_DIFFUSE &&
		state.textureStages[1].colorOperation == rts::render::RENDER_TEXTURE_OP_DISABLE,
		"opaque preset decodes to the legacy depth, blend, cull, and texture state");

	const unsigned int alphaTested = makeLegacyShaderBits(7, 0, 1, 2, 5,
		0, 0, 0, 1, 1, 0, 0, 0);
	result |= check(rts::render::DecodeLegacyShaderBits(alphaTested, &state) &&
		state.depthStencil.depthEnable && !state.depthStencil.depthWrite &&
		state.blend.blendEnable &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_SOURCE_ALPHA &&
		state.blend.destinationColor == rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		state.alphaTestEnable && state.alphaReference == 0x60U &&
		state.alphaFunction == rts::render::RENDER_COMPARE_GREATER_EQUAL &&
		state.rasterizer.cullMode == rts::render::RENDER_CULL_NONE &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1,
		"alpha-tested 2D preset decodes blending, alpha reference, and disabled depth");

	const unsigned int additive = makeLegacyShaderBits(3, 0, 1, 1, 1,
		1, 1, 1, 1, 0, 1, 4, 2);
	result |= check(rts::render::DecodeLegacyShaderBits(additive, &state) &&
		state.blend.blendEnable &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_ONE &&
		state.blend.destinationColor == rts::render::RENDER_BLEND_ONE &&
		state.fogMode == rts::render::RENDER_FOG_LINEAR &&
		state.secondaryGradientEnable &&
		state.textureStages[1].colorOperation == rts::render::RENDER_TEXTURE_OP_ADD &&
		state.textureStages[1].alphaOperation == rts::render::RENDER_TEXTURE_OP_MODULATE,
		"additive fogged detail preset preserves secondary gradient and combiners");

	const unsigned int multiplicativeNoColor = makeLegacyShaderBits(3, 0, 0,
		0, 2, 2, 3, 0, 1, 0, 1, 0, 0);
	result |= check(rts::render::DecodeLegacyShaderBits(multiplicativeNoColor, &state) &&
		state.blend.blendEnable && state.blend.colorWriteMask == 0 &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_ZERO &&
		state.blend.destinationColor == rts::render::RENDER_BLEND_ONE &&
		state.fogMode == rts::render::RENDER_FOG_SCALE_FRAGMENT &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT &&
		state.textureStages[0].alphaOperation == rts::render::RENDER_TEXTURE_OP_DISABLE,
		"disabled color writes and bump mapping retain the legacy effective state");

	const unsigned int inverseSourceAlpha = makeLegacyShaderBits(3, 0, 1,
		3, 0, 3, 5, 0, 1, 1, 1, 12, 3) | (1U << 17);
	result |= check(rts::render::DecodeLegacyShaderBits(inverseSourceAlpha, &state) &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_DESTINATION_COLOR &&
		state.alphaReference == 0x60U &&
		state.alphaFunction == rts::render::RENDER_COMPARE_GREATER_EQUAL &&
		state.fogMode == rts::render::RENDER_FOG_WHITE && state.nPatchEnable &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_MODULATE_2X &&
		state.textureStages[1].colorOperation == rts::render::RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR &&
		state.textureStages[1].alphaOperation == rts::render::RENDER_TEXTURE_OP_ADD_SMOOTH,
		"unusual legacy blend and detail encodings are decoded without reinterpretation");
	const unsigned int untextured = makeLegacyShaderBits(3, 1, 1,
		1, 0, 0, 1, 0, 0, 0, 1, 0, 0);
	result |= check(rts::render::DecodeLegacyShaderBits(untextured, &state) &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2 &&
		state.textureStages[0].colorArgument2 == rts::render::RENDER_TEXTURE_ARG_DIFFUSE &&
		state.textureStages[1].colorOperation == rts::render::RENDER_TEXTURE_OP_DISABLE,
		"untextured gradient selects vertex diffuse and disables detail stages");
	result |= check(!rts::render::DecodeLegacyShaderBits(
		makeLegacyShaderBits(3, 1, 1, 1, 7, 0, 7, 0, 1, 0, 1, 15, 7),
		&state) && state.shaderBits == 0,
		"reserved legacy encodings fail closed to deterministic defaults");
	rts::render::TrackLegacyShaderBits(opaque);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.shaderBits == opaque &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_MODULATE,
		"legacy bridge publishes the last valid shader state to the neutral boundary");
	rts::render::TrackLegacyShaderBits(makeLegacyShaderBits(3, 1, 1,
		1, 7, 0, 7, 0, 1, 0, 1, 15, 7));
	result |= check(!rts::render::GetTrackedLegacyPipelineState(&state) &&
		!rts::render::GetTrackedLegacyPipelineState(0),
		"legacy bridge never publishes invalid or null state");
	rts::render::TrackLegacyShaderBits(opaque);
	float transform[16];
	for (unsigned int index = 0; index < 16; ++index)
	{
		transform[index] = static_cast<float>(index + 1);
	}
	rts::render::LegacyLogicalState logicalState;
	result |= check(rts::render::TrackLegacyTransform(
		rts::render::LEGACY_TRANSFORM_WORLD, transform) &&
		rts::render::GetTrackedLegacyLogicalState(&logicalState) &&
		logicalState.constants.world.values[0] == 1.0f &&
		logicalState.constants.world.values[15] == 16.0f,
		"legacy bridge publishes fixed-function transforms with shader state");
	result |= check(!rts::render::TrackLegacyTransform(
		rts::render::LEGACY_TRANSFORM_COUNT, transform) &&
		!rts::render::TrackLegacyTransform(
			rts::render::LEGACY_TRANSFORM_WORLD, 0) &&
		!rts::render::GetTrackedLegacyLogicalState(0),
		"legacy transform bridge rejects invalid slots and null storage");
	rts::render::LegacyMaterialState material;
	material.diffuse = rts::render::RenderFloat4(0.25f, 0.5f, 0.75f, 1.0f);
	rts::render::TrackLegacyMaterial(material);
	rts::render::LegacyLightState light;
	light.enabled = true;
	light.type = rts::render::RENDER_LIGHT_POINT;
	light.range = 64.0f;
	rts::render::LegacyFogConstants fog;
	fog.enabled = true;
	fog.end = 512.0f;
	rts::render::TrackLegacyFog(fog);
	rts::render::TrackLegacyGlobalAmbient(
		rts::render::RenderFloat4(0.1f, 0.2f, 0.3f, 1.0f));
	result |= check(rts::render::TrackLegacyLight(0, light) &&
		!rts::render::TrackLegacyLight(rts::render::LEGACY_LIGHT_COUNT, light) &&
		rts::render::GetTrackedLegacyLogicalState(&logicalState) &&
		logicalState.constants.material.diffuse.z == 0.75f &&
		logicalState.constants.lights[0].enabled &&
		logicalState.constants.lights[0].range == 64.0f &&
		logicalState.constants.fog.end == 512.0f &&
		logicalState.constants.globalAmbient.y == 0.2f,
		"legacy bridge publishes material, light, fog, and ambient constants");
	rts::render::LegacyTextureStageState textureStage;
	textureStage.colorOperation = rts::render::RENDER_TEXTURE_OP_ADD;
	textureStage.sampler.addressU = rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
	textureStage.sampler.minification =
		rts::render::RENDER_TEXTURE_FILTER_ANISOTROPIC;
	textureStage.sampler.maximumAnisotropy = 8;
	result |= check(rts::render::TrackLegacyTextureStage(1, textureStage) &&
		!rts::render::TrackLegacyTextureStage(
			rts::render::LEGACY_TEXTURE_STAGE_COUNT, textureStage) &&
		rts::render::GetTrackedLegacyTextureStage(1, &textureStage) &&
		!rts::render::GetTrackedLegacyTextureStage(
			rts::render::LEGACY_TEXTURE_STAGE_COUNT, &textureStage) &&
		!rts::render::GetTrackedLegacyTextureStage(1, 0) &&
		rts::render::TrackLegacyTexturePresence(1, true) &&
		!rts::render::TrackLegacyTexturePresence(
			rts::render::LEGACY_TEXTURE_STAGE_COUNT, true) &&
		rts::render::GetTrackedLegacyLogicalState(&logicalState) &&
		logicalState.pipeline.textureStages[1].colorOperation ==
			rts::render::RENDER_TEXTURE_OP_ADD &&
		logicalState.pipeline.textureStages[1].sampler.addressU ==
			rts::render::RENDER_TEXTURE_ADDRESS_CLAMP &&
		logicalState.pipeline.textureStages[1].sampler.maximumAnisotropy == 8 &&
		logicalState.texturePresenceMask == 2,
		"legacy bridge publishes texture-stage, sampler, and binding state");
	rts::render::TrackLegacyShaderBits(opaque);
	result |= check(rts::render::GetTrackedLegacyLogicalState(&logicalState) &&
		logicalState.pipeline.textureStages[1].colorOperation ==
			rts::render::RENDER_TEXTURE_OP_ADD &&
		logicalState.pipeline.textureStages[1].sampler.addressU ==
			rts::render::RENDER_TEXTURE_ADDRESS_CLAMP &&
		logicalState.pipeline.textureStages[1].sampler.maximumAnisotropy == 8 &&
		logicalState.texturePresenceMask == 2,
		"shader publication preserves independently tracked texture and sampler state");
	rts::render::TrackLegacyTexturePresence(1, false);
	return result;
}

#if defined(RTS_RENDERER_HAS_D3D11)
struct CrossThreadProbe
{
	rts::render::IRenderDevice *device;
	bool contextRejected;
	bool presentRejected;
};

DWORD WINAPI probeD3D11FromWrongThread(void *parameter)
{
	CrossThreadProbe *probe = static_cast<CrossThreadProbe *>(parameter);
	probe->contextRejected = probe->device->immediateContext() == 0;
	probe->presentRejected = probe->device->present() ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	return 0;
}

int testD3D11HiddenSwapChain()
{
	int result = 0;
	const char *className = "GeneralsRendererContractWindow";
	WNDCLASSEXA windowClass;
	ZeroMemory(&windowClass, sizeof(windowClass));
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = DefWindowProcA;
	windowClass.hInstance = GetModuleHandleA(0);
	windowClass.lpszClassName = className;
	const ATOM classAtom = RegisterClassExA(&windowClass);
	result |= check(classAtom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
		"hidden D3D11 test window class registers");
	HWND window = CreateWindowExA(0, className, "", WS_OVERLAPPED,
		0, 0, 64, 64, 0, 0, windowClass.hInstance, 0);
	result |= check(window != 0, "hidden D3D11 test window is created");
	if (window == 0)
	{
		return result;
	}

	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0, "swap-chain D3D11 factory returns a device");
	if (device != 0)
	{
		rts::render::RenderDeviceParameters parameters;
		parameters.backend = rts::render::RENDER_BACKEND_D3D11;
		parameters.window = window;
		parameters.width = 64;
		parameters.height = 64;
		parameters.enableVsync = false;
		parameters.enableDebugLayer = true;
		result |= check(device->initialize(parameters) ==
			rts::render::RENDER_RESULT_OK,
			"flip-model D3D11 swap chain initializes while hidden");

		struct TestVertex
		{
			float x;
			float y;
			float z;
			unsigned int color;
		};
		const TestVertex vertices[3] = {
			{ -0.8f, -0.8f, 0.0f, 0xff0000ffU },
			{ 0.0f, 0.8f, 0.0f, 0xff0000ffU },
			{ 0.8f, -0.8f, 0.0f, 0xff0000ffU }
		};
		rts::render::BufferDescriptor vertexDescriptor;
		vertexDescriptor.byteCount = sizeof(vertices);
		vertexDescriptor.stride = sizeof(TestVertex);
		rts::render::GpuHandle vertexBuffer;
		result |= check(device->createBuffer(vertexDescriptor, vertices,
			sizeof(vertices), &vertexBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates an immutable vertex buffer");

		rts::render::IRenderContext *context = device->immediateContext();
		rts::render::LegacyLogicalState logicalState;
		const rts::render::RenderFloat4 clearColor(0.0f, 0.0f, 1.0f, 1.0f);
		const bool unbindFrameStarted = context->beginFrame() ==
			rts::render::RENDER_RESULT_OK;
		rts::render::RenderResult unbindResult = rts::render::RENDER_RESULT_FAILED;
		if (unbindFrameStarted)
		{
			unbindResult = context->setTexture(0, rts::render::GpuHandle());
			context->endFrame();
		}
		result |= check(unbindFrameStarted &&
			unbindResult == rts::render::RENDER_RESULT_OK,
			"D3D11 texture binding accepts an invalid handle as an explicit unbind");
		const rts::render::RenderFloat4 selectiveColor(1.0f, 0.0f, 0.0f, 1.0f);
		const rts::render::RenderFloat4 ignoredColor(0.0f, 1.0f, 0.0f, 1.0f);
		std::vector<unsigned char> selectivePixels(64 * 64 * 4);
		rts::render::RenderFormat selectiveFormat =
			rts::render::RENDER_FORMAT_UNKNOWN;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clearTargets(rts::render::RENDER_CLEAR_COLOR,
				selectiveColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->clearTargets(rts::render::RENDER_CLEAR_DEPTH |
				rts::render::RENDER_CLEAR_STENCIL, ignoredColor, 0.5f, 7) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&selectivePixels[0], selectivePixels.size(),
				64 * 4, &selectiveFormat) == rts::render::RENDER_RESULT_OK &&
			selectivePixels[2] > 240 && selectivePixels[1] < 16,
			"D3D11 selective depth and stencil clears preserve the color target");
		rts::render::TextureDescriptor offscreenColorDescriptor;
		offscreenColorDescriptor.width = 16;
		offscreenColorDescriptor.height = 16;
		offscreenColorDescriptor.format =
			rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
		offscreenColorDescriptor.binding =
			rts::render::RENDER_TEXTURE_RENDER_TARGET |
			rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
		offscreenColorDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		rts::render::GpuHandle offscreenColor;
		rts::render::TextureDescriptor offscreenDepthDescriptor;
		offscreenDepthDescriptor.width = 16;
		offscreenDepthDescriptor.height = 16;
		offscreenDepthDescriptor.format =
			rts::render::RENDER_FORMAT_D24_UNORM_S8_UINT;
		offscreenDepthDescriptor.binding =
			rts::render::RENDER_TEXTURE_DEPTH_STENCIL;
		offscreenDepthDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		rts::render::GpuHandle offscreenDepth;
		result |= check(device->createTexture(offscreenColorDescriptor, 0, 0,
			&offscreenColor) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(offscreenDepthDescriptor, 0, 0,
				&offscreenDepth) == rts::render::RENDER_RESULT_OK &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(offscreenColor, offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(0.25f, 0.5f, 0.75f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(rts::render::GpuHandle(), offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(), 1.0f, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(rts::render::GpuHandle(),
				rts::render::GpuHandle()) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline binds color/depth and depth-only targets before restoring the swap chain");
		result |= check(context != 0 &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline clears and draws through logical state");

		std::vector<unsigned char> pixels(64 * 64 * 4);
		rts::render::RenderFormat captureFormat =
			rts::render::RENDER_FORMAT_UNKNOWN;
		result |= check(device->captureBackBuffer(&pixels[0], pixels.size(),
			64 * 4, &captureFormat) == rts::render::RENDER_RESULT_OK &&
			captureFormat == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM,
			"D3D11 back buffer is available for deterministic capture");
		const unsigned char *corner = &pixels[4 * (2 * 64 + 2)];
		const unsigned char *center = &pixels[4 * (32 * 64 + 32)];
		result |= check(corner[0] > 240 && corner[2] < 16 &&
			center[2] > 240 && center[0] < 16,
			"captured D3D11 triangle preserves clear and vertex colors");

		const unsigned short indices[3] = { 0, 1, 2 };
		rts::render::BufferDescriptor indexDescriptor;
		indexDescriptor.byteCount = sizeof(indices);
		indexDescriptor.stride = sizeof(indices[0]);
		indexDescriptor.binding = rts::render::RENDER_BUFFER_INDEX;
		rts::render::GpuHandle indexBuffer;
		result |= check(device->createBuffer(indexDescriptor, indices,
			sizeof(indices), &indexBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates an immutable index buffer");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setIndexBuffer(vertexBuffer,
				rts::render::RENDER_FORMAT_R16_UINT, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setIndexBuffer(indexBuffer,
				rts::render::RENDER_FORMAT_UNKNOWN, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setIndexBuffer(indexBuffer,
				rts::render::RENDER_FORMAT_R16_UINT, 1) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->drawIndexed(3, 0, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 indexed drawing rejects wrong bindings, formats, alignment, and state");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setIndexBuffer(indexBuffer,
				rts::render::RENDER_FORMAT_R16_UINT, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->drawIndexed(3, 0, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline binds and draws indexed geometry");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] > 240 && center[0] < 16,
			"captured indexed triangle preserves the indexed draw result");

		logicalState.constants.world.values[0] = 0.4f;
		logicalState.constants.world.values[5] = 0.4f;
		logicalState.constants.world.values[12] = 0.5f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setIndexBuffer(indexBuffer,
				rts::render::RENDER_FORMAT_R16_UINT, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->drawIndexed(3, 0, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline uploads fixed-function transforms");
		center = &pixels[4 * (32 * 64 + 32)];
		const unsigned char *shiftedCenter = &pixels[4 * (32 * 64 + 48)];
		result |= check(center[0] > 240 && center[2] < 16 &&
			shiftedCenter[2] > 240 && shiftedCenter[0] < 16,
			"captured triangle follows the neutral world transform");
		logicalState.constants.world.setIdentity();

		logicalState.pipeline.alphaTestEnable = true;
		logicalState.pipeline.alphaFunction = rts::render::RENDER_COMPARE_GREATER;
		logicalState.pipeline.alphaReference = 255;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline applies legacy alpha-test state");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[2] < 16,
			"alpha-test rejection leaves the clear color untouched");
		logicalState.pipeline.alphaTestEnable = false;

		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 1.0f);
		logicalState.constants.fog.start = -1.0f;
		logicalState.constants.fog.end = 0.0f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline applies legacy fog constants");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"linear fog reaches the configured fog color");
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_DISABLED;
		logicalState.constants.fog.enabled = false;

		struct TexturedVertex
		{
			float position[3];
			float normal[3];
			unsigned int color;
			float texture[2];
		};
		const TexturedVertex texturedVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 0.0f, 1.0f } },
			{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 0.5f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 1.0f, 1.0f } }
		};
		rts::render::BufferDescriptor texturedVertexDescriptor;
		texturedVertexDescriptor.byteCount = sizeof(texturedVertices);
		texturedVertexDescriptor.stride = sizeof(TexturedVertex);
		rts::render::GpuHandle texturedVertexBuffer;
		result |= check(device->createBuffer(texturedVertexDescriptor,
			texturedVertices, sizeof(texturedVertices), &texturedVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a textured vertex buffer");
		const unsigned int greenPixels[4] = {
			0xff00ff00U, 0xff00ff00U, 0xff00ff00U, 0xff00ff00U
		};
		rts::render::TextureDescriptor textureDescriptor;
		textureDescriptor.width = 2;
		textureDescriptor.height = 2;
		textureDescriptor.format = rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
		rts::render::TextureSubresourceData textureData;
		textureData.data = greenPixels;
		textureData.rowPitch = 2 * sizeof(unsigned int);
		textureData.slicePitch = sizeof(greenPixels);
		rts::render::GpuHandle texture;
		result |= check(device->createTexture(textureDescriptor, &textureData, 1,
			&texture) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates an immutable shader texture");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_MODULATE;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 logical texture state draws through a shader-resource handle");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"captured D3D11 textured triangle preserves sampled color");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline applies the logical texture combiner");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] > 240 && center[2] > 240,
			"select-argument-2 uses diffuse color instead of sampled texture");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		logicalState.pipeline.textureStages[0].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[0].colorArgument1Complement = true;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 texture combiners accept legacy argument modifiers");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] > 240,
			"texture complement produces the hand-derived magenta result");
		logicalState.pipeline.textureStages[0].colorArgument1Complement = false;

		const unsigned int redPixels[4] = {
			0xff0000ffU, 0xff0000ffU, 0xff0000ffU, 0xff0000ffU
		};
		rts::render::TextureSubresourceData secondTextureData;
		secondTextureData.data = redPixels;
		secondTextureData.rowPitch = 2 * sizeof(unsigned int);
		secondTextureData.slicePitch = sizeof(redPixels);
		rts::render::GpuHandle secondTexture;
		result |= check(device->createTexture(textureDescriptor,
			&secondTextureData, 1, &secondTexture) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a second texture stage resource");
		logicalState.pipeline.textureStages[1].colorOperation =
			rts::render::RENDER_TEXTURE_OP_ADD;
		logicalState.pipeline.textureStages[1].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[1].colorArgument2 =
			rts::render::RENDER_TEXTURE_ARG_CURRENT;
		logicalState.pipeline.textureStages[1].alphaOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		logicalState.pipeline.textureStages[1].alphaArgument2 =
			rts::render::RENDER_TEXTURE_ARG_CURRENT;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, secondTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline executes two fixed-function texture stages");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] > 240 && center[2] > 240,
			"two-stage add combines green and red into yellow deterministically");
		logicalState.pipeline.textureStages[1] =
			rts::render::LegacyTextureStageState();

		struct FlexibleVertex
		{
			float position[3];
			unsigned int color;
			float texture0[2];
			float texture1[2];
		};
		const FlexibleVertex flexibleVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, 0xffffffffU,
				{ 0.0f, 0.0f }, { 0.75f, 0.0f } },
			{ { 0.0f, 0.8f, 0.0f }, 0xffffffffU,
				{ 0.0f, 0.0f }, { 0.75f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, 0xffffffffU,
				{ 0.0f, 0.0f }, { 0.75f, 0.0f } }
		};
		rts::render::BufferDescriptor flexibleVertexDescriptor;
		flexibleVertexDescriptor.byteCount = sizeof(flexibleVertices);
		flexibleVertexDescriptor.stride = sizeof(FlexibleVertex);
		rts::render::GpuHandle flexibleVertexBuffer;
		result |= check(device->createBuffer(flexibleVertexDescriptor,
			flexibleVertices, sizeof(flexibleVertices), &flexibleVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a flexible legacy vertex buffer");
		const unsigned int uvSelectionPixels[2] = {
			0xff0000ffU, 0xffff0000U
		};
		rts::render::TextureDescriptor uvSelectionDescriptor = textureDescriptor;
		uvSelectionDescriptor.height = 1;
		rts::render::TextureSubresourceData uvSelectionData;
		uvSelectionData.data = uvSelectionPixels;
		uvSelectionData.rowPitch = sizeof(uvSelectionPixels);
		uvSelectionData.slicePitch = sizeof(uvSelectionPixels);
		rts::render::GpuHandle uvSelectionTexture;
		result |= check(device->createTexture(uvSelectionDescriptor,
			&uvSelectionData, 1, &uvSelectionTexture) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a UV-selection texture");
		rts::render::LegacyVertexLayout flexibleLayout;
		flexibleLayout.stride = sizeof(FlexibleVertex);
		flexibleLayout.elementCount = 4;
		flexibleLayout.elements[0].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
		flexibleLayout.elements[0].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		flexibleLayout.elements[0].byteOffset = 0;
		flexibleLayout.elements[1].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
		flexibleLayout.elements[1].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		flexibleLayout.elements[1].byteOffset = 12;
		flexibleLayout.elements[2].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		flexibleLayout.elements[2].semanticIndex = 0;
		flexibleLayout.elements[2].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT2;
		flexibleLayout.elements[2].byteOffset = 16;
		flexibleLayout.elements[3].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		flexibleLayout.elements[3].semanticIndex = 1;
		flexibleLayout.elements[3].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT2;
		flexibleLayout.elements[3].byteOffset = 24;
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		logicalState.pipeline.textureStages[0].colorArgument2 =
			rts::render::RENDER_TEXTURE_ARG_DIFFUSE;
		logicalState.pipeline.textureStages[1].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		logicalState.pipeline.textureStages[1].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[1].textureCoordinateIndex = 1;
		logicalState.pipeline.textureStages[1].sampler.minification =
			rts::render::RENDER_TEXTURE_FILTER_POINT;
		logicalState.pipeline.textureStages[1].sampler.magnification =
			rts::render::RENDER_TEXTURE_FILTER_POINT;
		logicalState.pipeline.textureStages[1].sampler.mipmapping =
			rts::render::RENDER_TEXTURE_FILTER_POINT;
		const bool flexibleFrameStarted = context->beginFrame() ==
			rts::render::RENDER_RESULT_OK;
		rts::render::RenderResult flexibleStateResult =
			rts::render::RENDER_RESULT_FAILED;
		bool flexibleDrawCompleted = false;
		if (flexibleFrameStarted)
		{
			context->clear(clearColor, 1.0f, 0);
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f);
			flexibleStateResult = context->setLegacyStateForLayout(logicalState,
				flexibleLayout, 2);
			if (flexibleStateResult == rts::render::RENDER_RESULT_OK)
			{
				flexibleDrawCompleted = context->setVertexBuffer(
					flexibleVertexBuffer, sizeof(FlexibleVertex), 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, uvSelectionTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
			}
			context->endFrame();
		}
		result |= check(flexibleFrameStarted &&
			flexibleStateResult == rts::render::RENDER_RESULT_OK &&
			flexibleDrawCompleted &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline accepts descriptor-driven legacy vertex layouts");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"texture stage one selects UV1 instead of repeating UV0");
		logicalState.pipeline.textureStages[1] =
			rts::render::LegacyTextureStageState();
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		logicalState.pipeline.textureStages[0].colorArgument2 =
			rts::render::RENDER_TEXTURE_ARG_DIFFUSE;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, flexibleLayout, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(flexibleVertexBuffer,
				sizeof(FlexibleVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline accepts untextured flexible vertex layouts");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] > 240 && center[2] > 240,
			"untextured flexible layouts preserve vertex diffuse color");
		logicalState.pipeline.lightingEnable = true;
		logicalState.constants.material.ambient = rts::render::RenderFloat4();
		logicalState.constants.material.emissive = rts::render::RenderFloat4();
		logicalState.constants.material.diffuse =
			rts::render::RenderFloat4(1.0f, 1.0f, 1.0f, 1.0f);
		logicalState.constants.globalAmbient = rts::render::RenderFloat4();
		logicalState.constants.lights[0].enabled = true;
		logicalState.constants.lights[0].type =
			rts::render::RENDER_LIGHT_DIRECTIONAL;
		logicalState.constants.lights[0].diffuse =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 1.0f);
		logicalState.constants.lights[0].direction =
			rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 0.0f);
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline evaluates fixed-function directional lighting");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"directional lighting modulates diffuse vertex color deterministically");
		logicalState.pipeline.lightingEnable = false;
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 parity boundary accepts legacy bump-environment combiners");
		unsigned int debugErrorCount = 0xffffffffU;
		result |= check(device->getDebugValidationErrorCount(&debugErrorCount) ==
			rts::render::RENDER_RESULT_OK && debugErrorCount == 0,
			"D3D11 debug layer reports no validation errors");
		result |= check(device->destroyResource(vertexBuffer) &&
			device->destroyResource(indexBuffer) &&
			device->destroyResource(texturedVertexBuffer) &&
			device->destroyResource(flexibleVertexBuffer) &&
			device->destroyResource(texture) &&
			device->destroyResource(secondTexture) &&
			device->destroyResource(uvSelectionTexture) &&
			device->destroyResource(offscreenColor) &&
			device->destroyResource(offscreenDepth) &&
			device->present() == rts::render::RENDER_RESULT_OK &&
			device->resize(96, 80) == rts::render::RENDER_RESULT_OK &&
			device->present() == rts::render::RENDER_RESULT_OK,
			"hidden flip-model swap chain presents and resizes");
		device->shutdown();
		delete device;
	}
	DestroyWindow(window);
	return result;
}

int testD3D11HeadlessDevice()
{
	int result = 0;
	rts::render::IRenderDevice *invalidAdapterDevice =
		rts::render::CreateD3D11RenderDevice();
	rts::render::RenderDeviceParameters invalidAdapterParameters;
	invalidAdapterParameters.backend = rts::render::RENDER_BACKEND_D3D11;
	invalidAdapterParameters.width = 64;
	invalidAdapterParameters.height = 64;
	invalidAdapterParameters.adapterIndex = 0xfffffffeU;
	result |= check(invalidAdapterDevice != 0 &&
		invalidAdapterDevice->initialize(invalidAdapterParameters) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"D3D11 explicit adapter selection rejects nonexistent adapters");
	delete invalidAdapterDevice;
	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0, "D3D11 factory returns a device");
	if (device == 0)
	{
		return result;
	}

	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	result |= check(device->backend() == rts::render::RENDER_BACKEND_D3D11 &&
		device->initialize(parameters) == rts::render::RENDER_RESULT_OK,
		"headless D3D11 feature-level-11 device initializes");
	result |= check(device->immediateContext() != 0,
		"initialized D3D11 device exposes its owner context");
	CrossThreadProbe probe = { device, false, false };
	HANDLE thread = CreateThread(0, 0, probeD3D11FromWrongThread, &probe, 0, 0);
	result |= check(thread != 0, "D3D11 ownership probe thread starts");
	if (thread != 0)
	{
		const DWORD waitResult = WaitForSingleObject(thread, 5000);
		CloseHandle(thread);
		result |= check(waitResult == WAIT_OBJECT_0 &&
			probe.contextRejected && probe.presentRejected,
			"D3D11 immediate context rejects non-owner access");
	}

	rts::render::BufferDescriptor descriptor;
	descriptor.byteCount = 64;
	descriptor.stride = 16;
	descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle buffer;
	result |= check(device->createBuffer(descriptor, 0, 0, &buffer) ==
		rts::render::RENDER_RESULT_OK && buffer.isValid(),
		"D3D11 dynamic buffer receives a logical handle");
	result |= check(device->recoverDevice() ==
		rts::render::RENDER_RESULT_OK,
		"D3D11 recovery recreates live logical resources without changing handles");
	unsigned int values[4] = { 1, 2, 3, 4 };
	result |= check(device->immediateContext()->beginFrame() ==
		rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->updateBuffer(buffer, values,
			sizeof(values), 0) == rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK,
		"owner context maps and updates dynamic buffers");
	values[0] = 9;
	result |= check(device->recoverDevice() ==
		rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->beginFrame() ==
			rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->updateBuffer(buffer, values,
			sizeof(values), 0) == rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK,
		"recreated dynamic buffers retain their logical handle and update path");
	result |= check(device->destroyResource(buffer) &&
		!device->destroyResource(buffer),
		"D3D11 resource destruction rejects stale handles");
	unsigned int cubePixels[6] = {
		0xff0000ffU, 0xff00ff00U, 0xffff0000U,
		0xffffff00U, 0xff00ffffU, 0xffff00ffU
	};
	rts::render::TextureSubresourceData cubeData[6];
	for (unsigned int face = 0; face < 6; ++face)
	{
		cubeData[face].data = &cubePixels[face];
		cubeData[face].rowPitch = sizeof(unsigned int);
		cubeData[face].slicePitch = sizeof(unsigned int);
	}
	rts::render::TextureDescriptor cubeDescriptor;
	cubeDescriptor.width = 1;
	cubeDescriptor.height = 1;
	cubeDescriptor.arrayCount = 6;
	cubeDescriptor.dimension = rts::render::RENDER_TEXTURE_CUBE;
	cubeDescriptor.format = rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
	rts::render::GpuHandle cubeTexture;
	result |= check(device->createTexture(cubeDescriptor, cubeData, 6,
		&cubeTexture) == rts::render::RENDER_RESULT_OK &&
		device->destroyResource(cubeTexture),
		"D3D11 creates generation-safe cube-map shader resources");
	cubeDescriptor.width = 2;
	result |= check(device->createTexture(cubeDescriptor, cubeData, 6,
		&cubeTexture) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cube-map descriptors require square faces");
	rts::render::BufferDescriptor immutableDescriptor;
	immutableDescriptor.byteCount = 16;
	rts::render::GpuHandle invalidBuffer;
	result |= check(device->createBuffer(immutableDescriptor, 0, 0,
		&invalidBuffer) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!invalidBuffer.isValid(),
		"immutable D3D11 resources require complete initial data");
	result |= check(device->present() == rts::render::RENDER_RESULT_OK,
		"headless presentation is a successful no-op");
	result |= check(device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
		device->immediateContext() != 0 &&
		device->present() == rts::render::RENDER_RESULT_OK,
		"headless D3D11 device recreates at an empty frame boundary");
	device->shutdown();
	device->shutdown();
	delete device;
	return result;
}

int testD3D11LegacyBlendFactors()
{
	int result = 0;
	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0, "D3D11 blend contract factory returns a device");
	if (device == 0)
	{
		return result;
	}

	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	parameters.enableDebugLayer = true;
	result |= check(device->initialize(parameters) == rts::render::RENDER_RESULT_OK,
		"D3D11 blend contract device initializes");
	if (device->immediateContext() == 0)
	{
		device->shutdown();
		delete device;
		return result | check(false, "D3D11 blend contract exposes its context");
	}

	struct BlendCase
	{
		rts::render::RenderBlendFactor source;
		rts::render::RenderBlendFactor destination;
		unsigned int colorWriteMask;
		const char *name;
	};
	const BlendCase cases[] = {
		{ rts::render::RENDER_BLEND_SOURCE_COLOR,
			rts::render::RENDER_BLEND_ZERO, 0x0fU,
			"SOURCE_COLOR with ZERO" },
		{ rts::render::RENDER_BLEND_ZERO,
			rts::render::RENDER_BLEND_SOURCE_COLOR, 0x0fU,
			"ZERO with SOURCE_COLOR" },
		{ rts::render::RENDER_BLEND_ONE,
			rts::render::RENDER_BLEND_INVERSE_SOURCE_COLOR, 0x0fU,
			"ONE with INVERSE_SOURCE_COLOR" },
		{ rts::render::RENDER_BLEND_ZERO,
			rts::render::RENDER_BLEND_ONE, 0,
			"color-write-disabled ZERO/ONE" }
	};
	const unsigned int caseCount = sizeof(cases) / sizeof(cases[0]);
	rts::render::IRenderContext *context = device->immediateContext();
	const bool frameStarted = context->beginFrame() ==
		rts::render::RENDER_RESULT_OK;
	result |= check(frameStarted, "D3D11 blend contract begins a frame");
	if (frameStarted)
	{
		for (unsigned int index = 0; index < caseCount; ++index)
		{
			rts::render::LegacyLogicalState state;
			state.pipeline.blend.blendEnable = true;
			state.pipeline.blend.sourceColor = cases[index].source;
			state.pipeline.blend.destinationColor = cases[index].destination;
			state.pipeline.blend.sourceAlpha = cases[index].source;
			state.pipeline.blend.destinationAlpha = cases[index].destination;
			state.pipeline.blend.colorWriteMask = cases[index].colorWriteMask;
			const rts::render::RenderResult stateResult =
				context->setLegacyState(state,
					rts::render::RENDER_VERTEX_POSITION3_COLOR, 0);
			if (stateResult != rts::render::RENDER_RESULT_OK)
			{
				fprintf(stderr,
					"legacy blend case %s returned RenderResult %u; "
					"CreateBlendState rejected the alpha factor\n",
					cases[index].name, static_cast<unsigned int>(stateResult));
			}
			char message[128];
			snprintf(message, sizeof(message),
				"legacy blend state remains bindable: %s", cases[index].name);
			result |= check(stateResult == rts::render::RENDER_RESULT_OK,
				message);
		}
		result |= check(context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 blend contract ends a frame");
	}
	device->shutdown();
	delete device;
	return result;
}
#endif
}

int main()
{
	int result = 0;
	result |= testBackendNames();
	result |= testGenerationSafeHandles();
	result |= testNeutralDescriptorDefaults();
	result |= testLegacyLogicalState();
	result |= testLegacyShaderBitDecoder();
#if defined(RTS_RENDERER_HAS_D3D11)
	result |= testD3D11HeadlessDevice();
	result |= testD3D11LegacyBlendFactors();
	result |= testD3D11HiddenSwapChain();
#endif
	if (result == 0)
	{
		printf("Renderer contract tests passed.\n");
	}
	return result;
}
