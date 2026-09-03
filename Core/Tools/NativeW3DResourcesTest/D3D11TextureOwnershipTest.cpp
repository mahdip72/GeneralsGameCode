#include "Renderer/NativeW3DResources.h"
#include "Renderer/RendererDevice.h"

#include <cstdio>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAILED: %s\n", message);
	return 1;
}

TextureDescriptor MakeTextureDescriptor()
{
	TextureDescriptor descriptor;
	descriptor.width = 4;
	descriptor.height = 2;
	descriptor.mipCount = 2;
	descriptor.arrayCount = 1;
	descriptor.dimension = RENDER_TEXTURE_2D;
	descriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	return descriptor;
}

bool IsEmpty(const RenderResourceStatistics &statistics)
{
	return statistics.liveHandles == 0 && statistics.bufferCount == 0 &&
		statistics.textureCount == 0 && statistics.nativeResourceCount == 0 &&
		statistics.shaderResourceViewCount == 0 &&
		statistics.renderTargetViewCount == 0 &&
		statistics.depthStencilViewCount == 0 &&
		statistics.recoveryShadowBytes == 0;
}

bool SameStatistics(const RenderResourceStatistics &left,
	const RenderResourceStatistics &right)
{
	return left.liveHandles == right.liveHandles &&
		left.bufferCount == right.bufferCount &&
		left.textureCount == right.textureCount &&
		left.nativeResourceCount == right.nativeResourceCount &&
		left.shaderResourceViewCount == right.shaderResourceViewCount &&
		left.renderTargetViewCount == right.renderTargetViewCount &&
		left.depthStencilViewCount == right.depthStencilViewCount &&
		left.recoveryShadowBytes == right.recoveryShadowBytes;
}

int TestCanonicalUploadValidation()
{
	int result = 0;
	const TextureDescriptor descriptor = MakeTextureDescriptor();
	unsigned char topMip[40] = { 0 };
	unsigned char lowerMip[8] = { 0 };
	TextureSubresourceData data[2];
	data[0].data = topMip;
	data[0].rowPitch = 20;
	data[0].slicePitch = sizeof(topMip);
	data[1].data = lowerMip;
	data[1].rowPitch = 8;
	data[1].slicePitch = sizeof(lowerMip);

	size_t uploadBytes = 123;
	result |= Check(ValidateTextureUpload(descriptor, data, 2, true, 1024,
		&uploadBytes) == RENDER_RESULT_OK && uploadBytes == 48,
		"canonical upload validation retains padded rows and exact mip sizes");

	TextureSubresourceData invalid[2] = { data[0], data[1] };
	invalid[1].rowPitch = 7;
	uploadBytes = 123;
	result |= Check(ValidateTextureUpload(descriptor, invalid, 2, true, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT && uploadBytes == 0,
		"canonical upload validation rejects a short lower-mip row");
	invalid[1] = data[1];
	invalid[0].slicePitch = 39;
	result |= Check(ValidateTextureUpload(descriptor, invalid, 2, true, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT && uploadBytes == 0,
		"canonical upload validation rejects a short slice");
	result |= Check(ValidateTextureUpload(descriptor, data, 1, true, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT && uploadBytes == 0,
		"canonical upload validation requires every declared mip");
	result |= Check(ValidateTextureUpload(descriptor, data, 2, true, 47,
		&uploadBytes) == RENDER_RESULT_OUT_OF_MEMORY && uploadBytes == 0,
		"canonical upload validation applies a bounded allocation budget");
	invalid[0] = data[0];
	invalid[0].rowPitch = static_cast<size_t>(-1);
	result |= Check(ValidateTextureUpload(descriptor, invalid, 2, true, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT && uploadBytes == 0,
		"canonical upload validation rejects an unrepresentable native pitch");

	TextureDescriptor tooManyMips = descriptor;
	tooManyMips.mipCount = 4;
	result |= Check(ValidateTextureUpload(tooManyMips, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"canonical descriptors reject mips below the one-pixel level");
	TextureDescriptor invalidCube = descriptor;
	invalidCube.dimension = RENDER_TEXTURE_CUBE;
	invalidCube.width = 4;
	invalidCube.height = 2;
	invalidCube.arrayCount = 6;
	result |= Check(ValidateTextureUpload(invalidCube, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"canonical descriptors require square six-face cube maps");
	TextureDescriptor immutable = descriptor;
	immutable.usage = RENDER_USAGE_IMMUTABLE;
	result |= Check(ValidateTextureUpload(immutable, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"immutable textures require complete creation data");
	TextureDescriptor dynamicMips = descriptor;
	dynamicMips.usage = RENDER_USAGE_DYNAMIC;
	result |= Check(ValidateTextureUpload(dynamicMips, data, 2, true, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"dynamic textures reject multi-subresource WRITE_DISCARD ownership");
	TextureDescriptor dynamicCube = descriptor;
	dynamicCube.width = 4;
	dynamicCube.height = 4;
	dynamicCube.mipCount = 1;
	dynamicCube.arrayCount = 6;
	dynamicCube.dimension = RENDER_TEXTURE_CUBE;
	dynamicCube.usage = RENDER_USAGE_DYNAMIC;
	result |= Check(ValidateTextureUpload(dynamicCube, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"dynamic cube textures reject multi-face WRITE_DISCARD ownership");
	TextureDescriptor multiMipTarget = descriptor;
	multiMipTarget.binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	result |= Check(ValidateTextureUpload(multiMipTarget, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"render targets reject recovery-ambiguous mip chains");
	TextureDescriptor multiMipDepth = descriptor;
	multiMipDepth.format = RENDER_FORMAT_D24_UNORM_S8_UINT;
	multiMipDepth.binding = RENDER_TEXTURE_DEPTH_STENCIL;
	result |= Check(ValidateTextureUpload(multiMipDepth, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_INVALID_ARGUMENT,
		"depth targets reject recovery-ambiguous mip chains");
	result |= Check(ValidateTextureUpload(descriptor, 0, 0, false, 1024,
		&uploadBytes) == RENDER_RESULT_OK && uploadBytes == 0,
		"default textures may be created empty for render-owner population");
	return result;
}

int TestNativeD3D11Ownership()
{
	int result = 0;
	IRenderDevice *device = CreateD3D11RenderDevice();
	result |= Check(device != 0, "the D3D11 texture owner fixture allocates");
	if (device == 0)
	{
		return result;
	}

	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.width = 8;
	parameters.height = 8;
	parameters.window = 0;
	parameters.enableDebugLayer = false;
	parameters.allowSoftwareFallback = true;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"the D3D11 texture owner fixture initializes headlessly");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"the typed texture owner borrows the process D3D11 backend");

	const TextureDescriptor descriptor = MakeTextureDescriptor();
	unsigned char topMip[40] = { 0 };
	unsigned char lowerMip[8] = { 0 };
	for (unsigned int index = 0; index < sizeof(topMip); ++index)
	{
		topMip[index] = static_cast<unsigned char>(index + 1);
	}
	for (unsigned int index = 0; index < sizeof(lowerMip); ++index)
	{
		lowerMip[index] = static_cast<unsigned char>(0x80 + index);
	}
	TextureSubresourceData data[2];
	data[0].data = topMip;
	data[0].rowPitch = 20;
	data[0].slicePitch = sizeof(topMip);
	data[1].data = lowerMip;
	data[1].rowPitch = 8;
	data[1].slicePitch = sizeof(lowerMip);

	RenderResourceStatistics statistics;
	result |= Check(device->getDebugResourceStatistics(&statistics) ==
		RENDER_RESULT_OK && IsEmpty(statistics),
		"the native backend begins with no logical texture allocation");
	TextureDescriptor rejectedDescriptors[4];
	rejectedDescriptors[0] = descriptor;
	rejectedDescriptors[0].usage = RENDER_USAGE_DYNAMIC;
	rejectedDescriptors[1] = descriptor;
	rejectedDescriptors[1].width = 4;
	rejectedDescriptors[1].height = 4;
	rejectedDescriptors[1].mipCount = 1;
	rejectedDescriptors[1].arrayCount = 6;
	rejectedDescriptors[1].dimension = RENDER_TEXTURE_CUBE;
	rejectedDescriptors[1].usage = RENDER_USAGE_DYNAMIC;
	rejectedDescriptors[2] = descriptor;
	rejectedDescriptors[2].binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	rejectedDescriptors[3] = descriptor;
	rejectedDescriptors[3].format = RENDER_FORMAT_D24_UNORM_S8_UINT;
	rejectedDescriptors[3].binding = RENDER_TEXTURE_DEPTH_STENCIL;
	for (unsigned int index = 0; index < 4; ++index)
	{
		GpuHandle directRejected;
		const RenderResult directCreationResult = device->createTexture(
			rejectedDescriptors[index], 0, 0, &directRejected);
		result |= Check(directCreationResult == RENDER_RESULT_INVALID_ARGUMENT &&
			!directRejected.isValid() &&
			device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
			IsEmpty(statistics),
			"D3D11 rejects unsupported topology before native publication");
		if (directRejected.isValid())
		{
			device->destroyResource(directRejected);
		}
		NativeW3DTextureHandle rejected;
		const RenderResult creationResult = resources.CreateTexture(
			rejectedDescriptors[index], 0, 0, &rejected);
		result |= Check(creationResult == RENDER_RESULT_INVALID_ARGUMENT &&
			!rejected.isValid() &&
			device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
			IsEmpty(statistics),
			"unsupported dynamic/output topology publishes no D3D resource");
		if (rejected.isValid())
		{
			resources.DestroyTexture(rejected);
		}
	}
	const RenderResourceFaultPoint creationFaults[] = {
		RENDER_RESOURCE_FAULT_TEXTURE_ALLOCATION,
		RENDER_RESOURCE_FAULT_TEXTURE_VIEW,
		RENDER_RESOURCE_FAULT_TEXTURE_SHADOW
	};
	for (unsigned int index = 0;
		index < sizeof(creationFaults) / sizeof(creationFaults[0]); ++index)
	{
		NativeW3DTextureHandle rejected;
		result |= Check(device->configureResourceFaultInjection(
			creationFaults[index], 1, RENDER_RESULT_OUT_OF_MEMORY) ==
			RENDER_RESULT_OK && resources.CreateTexture(descriptor, data, 2,
				&rejected) == RENDER_RESULT_OUT_OF_MEMORY &&
			!rejected.isValid() &&
			device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
			IsEmpty(statistics),
			"injected texture creation failure rolls back every native object");
	}

	NativeW3DTextureHandle texture;
	result |= Check(resources.CreateTexture(descriptor, data, 2, &texture) ==
		RENDER_RESULT_OK && texture.isValid() &&
		device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
		statistics.liveHandles == 1 && statistics.textureCount == 1 &&
		statistics.nativeResourceCount == 1 &&
		statistics.shaderResourceViewCount == 1 &&
		statistics.renderTargetViewCount == 0 &&
		statistics.depthStencilViewCount == 0 &&
		statistics.recoveryShadowBytes == 48,
		"transactional creation publishes one complete texture and recovery image");

	NativeW3DTextureDescription description;
	result |= Check(resources.DescribeTexture(texture.resource, &description) ==
		RENDER_RESULT_OK && description.descriptor.width == 4 &&
		description.descriptor.height == 2 &&
		description.descriptor.mipCount == 2 &&
		description.descriptor.arrayCount == 1 &&
		description.descriptor.format == RENDER_FORMAT_R8G8B8A8_UNORM &&
		description.authority == NATIVE_W3D_CONTENT_CPU,
		"the typed owner retains exact dimensions, format, mips, and CPU authority");

	for (unsigned int index = 0; index < sizeof(topMip); ++index)
	{
		topMip[index] ^= 0x5a;
	}
	result |= Check(resources.RefreshTexture(texture, descriptor, data, 2) ==
		RENDER_RESULT_OK && resources.IsValid(texture) &&
		device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
		statistics.liveHandles == 1 && statistics.textureCount == 1 &&
		statistics.recoveryShadowBytes == 48,
		"typed refresh updates the recovery image without replacing ownership");

	for (unsigned int index = 0;
		index < sizeof(creationFaults) / sizeof(creationFaults[0]); ++index)
	{
		NativeW3DTextureDescription beforeFailure;
		RenderResourceStatistics beforeStatistics;
		result |= Check(resources.DescribeTexture(texture.resource,
			&beforeFailure) == RENDER_RESULT_OK &&
			beforeFailure.authority == NATIVE_W3D_CONTENT_CPU &&
			device->getDebugResourceStatistics(&beforeStatistics) ==
				RENDER_RESULT_OK,
			"the existing texture is authoritative before injected removal");

		result |= Check(device->configureResourceFaultInjection(
			creationFaults[index], 1, RENDER_RESULT_DEVICE_REMOVED) ==
			RENDER_RESULT_OK,
			"the real D3D backend accepts a device-removal creation fault");
		NativeW3DTextureHandle rejected;
		const RenderResult creationResult = resources.CreateTexture(descriptor,
			data, 2, &rejected);
		NativeW3DTextureDescription afterFailure;
		RenderResourceStatistics afterStatistics;
		const RenderResult descriptionResult = resources.DescribeTexture(
			texture.resource, &afterFailure);
		const RenderResult statisticsResult =
			device->getDebugResourceStatistics(&afterStatistics);
		result |= Check(creationResult == RENDER_RESULT_DEVICE_REMOVED &&
			!rejected.isValid() && device->isOperational() &&
			resources.IsValid(texture) &&
			descriptionResult == RENDER_RESULT_OK &&
			afterFailure.authority == NATIVE_W3D_CONTENT_INVALID &&
			afterFailure.authorityEpoch != beforeFailure.authorityEpoch &&
			statisticsResult == RENDER_RESULT_OK &&
			SameStatistics(beforeStatistics, afterStatistics),
			"device removal during texture allocation, view, or shadow creation invalidates all existing authority without leaking the rejected texture");
		if (rejected.isValid())
		{
			resources.DestroyTexture(rejected);
		}
		result |= Check(resources.RefreshTexture(texture, descriptor, data, 2) ==
			RENDER_RESULT_OK,
			"explicit republish restores CPU authority after creation removal");
	}

	NativeW3DSurfaceHandle topSurface;
	NativeW3DSurfaceHandle lowerSurface;
	result |= Check(resources.AcquireTextureSurface(texture, 0, 0,
		&topSurface) == RENDER_RESULT_OK && topSurface.isValid() &&
		resources.IsValid(topSurface) &&
		topSurface.width == 4 && topSurface.height == 2 &&
		resources.AcquireTextureSurface(texture, 1, 0, &lowerSurface) ==
		RENDER_RESULT_OK && lowerSurface.isValid() &&
		resources.IsValid(lowerSurface) &&
		lowerSurface.width == 2 && lowerSurface.height == 1 &&
		lowerSurface.format == RENDER_FORMAT_R8G8B8A8_UNORM,
		"typed surface acquisition resolves exact mip extents without a COM pointer");
	NativeW3DSurfaceHandle rejectedSurface = topSurface;
	result |= Check(resources.AcquireTextureSurface(texture, 2, 0,
		&rejectedSurface) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedSurface.isValid(),
		"out-of-range surface acquisition clears its output");

	const NativeW3DSurfaceHandle beforeRecovery = lowerSurface;
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK,
		"CPU-authoritative native textures recreate after device recovery");
	result |= Check(!resources.IsValid(beforeRecovery),
		"a cached native surface token is invalid after backend recovery");
	lowerSurface = beforeRecovery;
	result |= Check(resources.AcquireTextureSurface(texture, 1, 0,
		&lowerSurface) == RENDER_RESULT_INVALID_ARGUMENT &&
		!lowerSurface.isValid(),
		"a cached native surface token fails closed across backend recovery");
	result |= Check(resources.AcquireTextureSurface(texture, 1, 0,
		&lowerSurface) == RENDER_RESULT_OK && lowerSurface.isValid() &&
		lowerSurface.backendEpoch != beforeRecovery.backendEpoch &&
		device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
		statistics.liveHandles == 1 && statistics.nativeResourceCount == 1 &&
		statistics.shaderResourceViewCount == 1 &&
		statistics.recoveryShadowBytes == 48,
		"surface reacquisition observes the recreated backend epoch and complete shadow");

	const NativeW3DTextureHandle staleTexture = texture;
	result |= Check(resources.DestroyTexture(texture) &&
		!resources.IsValid(staleTexture) &&
		device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
		IsEmpty(statistics),
		"typed destruction releases every native texture object and recovery byte");
	NativeW3DTextureHandle replacement;
	result |= Check(resources.CreateTexture(descriptor, data, 2, &replacement) ==
		RENDER_RESULT_OK && replacement.isValid() &&
		replacement.resource != staleTexture.resource,
		"slot reuse advances the neutral texture generation");
	rejectedSurface = NativeW3DSurfaceHandle();
	result |= Check(resources.AcquireTextureSurface(staleTexture, 0, 0,
		&rejectedSurface) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedSurface.isValid(),
		"a stale typed texture generation cannot alias its replacement");

	TextureDescriptor colorTargetDescriptor;
	colorTargetDescriptor.width = 4;
	colorTargetDescriptor.height = 4;
	colorTargetDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	colorTargetDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	colorTargetDescriptor.usage = RENDER_USAGE_DEFAULT;
	TextureDescriptor depthTargetDescriptor;
	depthTargetDescriptor.width = 4;
	depthTargetDescriptor.height = 4;
	depthTargetDescriptor.binding = RENDER_TEXTURE_DEPTH_STENCIL;
	depthTargetDescriptor.format = RENDER_FORMAT_D24_UNORM_S8_UINT;
	depthTargetDescriptor.usage = RENDER_USAGE_DEFAULT;
	NativeW3DTextureHandle colorTarget;
	NativeW3DTextureHandle depthTarget;
	result |= Check(resources.CreateTexture(colorTargetDescriptor, 0, 0,
		&colorTarget) == RENDER_RESULT_OK &&
		resources.CreateTexture(depthTargetDescriptor, 0, 0, &depthTarget) ==
		RENDER_RESULT_OK,
		"single-mip color and depth outputs remain supported");
	NativeW3DSurfaceHandle colorSurface;
	NativeW3DSurfaceHandle depthSurface;
	result |= Check(resources.AcquireTextureSurface(colorTarget, 0, 0,
			&colorSurface) == RENDER_RESULT_OK && colorSurface.isValid() &&
		resources.AcquireTextureSurface(depthTarget, 0, 0, &depthSurface) ==
			RENDER_RESULT_OK && depthSurface.isValid(),
		"render-target and depth-stencil views publish typed surfaces without COM retention");
	NativeW3DTextureDescription depthBeforeRefresh;
	NativeW3DTextureDescription depthAfterRefresh;
	RenderResourceStatistics beforeDepthRefresh;
	RenderResourceStatistics afterDepthRefresh;
	unsigned char depthPixels[64] = { 0 };
	TextureSubresourceData depthData;
	depthData.data = depthPixels;
	depthData.rowPitch = 16;
	depthData.slicePitch = sizeof(depthPixels);
	result |= Check(resources.DescribeTexture(depthTarget.resource,
		&depthBeforeRefresh) == RENDER_RESULT_OK &&
		device->getDebugResourceStatistics(&beforeDepthRefresh) ==
		RENDER_RESULT_OK &&
		device->refreshTexture(depthTarget.resource, depthTargetDescriptor,
			&depthData, 1) == RENDER_RESULT_UNSUPPORTED &&
		resources.RefreshTexture(depthTarget, depthTargetDescriptor, &depthData,
			1) == RENDER_RESULT_UNSUPPORTED &&
		resources.DescribeTexture(depthTarget.resource, &depthAfterRefresh) ==
		RENDER_RESULT_OK &&
		device->getDebugResourceStatistics(&afterDepthRefresh) ==
		RENDER_RESULT_OK &&
		depthAfterRefresh.authority == depthBeforeRefresh.authority &&
		depthAfterRefresh.authorityEpoch == depthBeforeRefresh.authorityEpoch &&
		SameStatistics(beforeDepthRefresh, afterDepthRefresh),
		"depth refresh fails before upload or CPU-authority publication");

	IRenderContext *context = device->immediateContext();
	RenderTargetBinding targetBinding;
	targetBinding.useBackBufferColor = false;
	targetBinding.useBackBufferDepth = false;
	targetBinding.hasColor = true;
	targetBinding.hasDepth = true;
	targetBinding.color.resource = colorTarget.resource;
	targetBinding.depth.resource = depthTarget.resource;
	NativeW3DGpuContentLease colorLease;
	NativeW3DGpuContentLease depthLease;
	result |= Check(context != 0 && context->beginFrame() == RENDER_RESULT_OK &&
		context->setRenderTargets(targetBinding) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(0.25f, 0.5f, 0.75f, 1.0f), 0.5f, 3) ==
			RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		resources.PublishRenderTargetWrite(colorSurface, &colorLease) ==
			RENDER_RESULT_OK && colorLease.isValid() &&
		resources.PublishRenderTargetWrite(depthSurface, &depthLease) ==
			RENDER_RESULT_OK && depthLease.isValid() &&
		resources.AcquireGpuContentLease(colorTarget.resource, &colorLease) ==
			RENDER_RESULT_OK &&
		resources.AcquireGpuContentLease(depthTarget.resource, &depthLease) ==
			RENDER_RESULT_OK,
		"accepted real D3D11 color and depth outputs publish exact GPU authority leases");
	const NativeW3DSurfaceHandle staleColorSurface = colorSurface;
	const NativeW3DSurfaceHandle staleDepthSurface = depthSurface;
	const NativeW3DGpuContentLease staleColorLease = colorLease;
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.IsValid(colorTarget) && resources.IsValid(depthTarget) &&
		!resources.IsValid(staleColorSurface) &&
		!resources.IsValid(staleDepthSurface) &&
		device->getDebugResourceStatistics(&statistics) == RENDER_RESULT_OK &&
		statistics.liveHandles == 3 && statistics.textureCount == 3 &&
		statistics.nativeResourceCount == 3 &&
		statistics.shaderResourceViewCount == 2 &&
		statistics.renderTargetViewCount == 1 &&
		statistics.depthStencilViewCount == 1 &&
		statistics.recoveryShadowBytes == 48,
		"single-mip GPU-authoritative color and depth outputs recover completely");
	colorLease = staleColorLease;
	result |= Check(resources.AcquireGpuContentLease(colorTarget.resource,
			&colorLease) == RENDER_RESULT_INVALID_ARGUMENT &&
		!colorLease.isValid(),
		"real device recovery clears the stale GPU output lease");
	colorSurface = staleColorSurface;
	depthSurface = staleDepthSurface;
	result |= Check(resources.AcquireTextureSurface(colorTarget, 0, 0,
			&colorSurface) == RENDER_RESULT_INVALID_ARGUMENT &&
		!colorSurface.isValid() &&
		resources.AcquireTextureSurface(depthTarget, 0, 0, &depthSurface) ==
			RENDER_RESULT_INVALID_ARGUMENT && !depthSurface.isValid(),
		"cached real-device output surfaces fail closed after recovery");
	result |= Check(resources.AcquireTextureSurface(colorTarget, 0, 0,
			&colorSurface) == RENDER_RESULT_OK && colorSurface.isValid() &&
		resources.AcquireTextureSurface(depthTarget, 0, 0, &depthSurface) ==
			RENDER_RESULT_OK && depthSurface.isValid(),
		"cleared output tokens reacquire the recreated color and depth views");
	context = device->immediateContext();
	targetBinding.color.resource = colorTarget.resource;
	targetBinding.depth.resource = depthTarget.resource;
	NativeW3DGpuContentLease regeneratedColorLease;
	NativeW3DGpuContentLease regeneratedDepthLease;
	result |= Check(context != 0 && context->beginFrame() == RENDER_RESULT_OK &&
		context->setRenderTargets(targetBinding) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, 0) ==
			RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		resources.PublishRenderTargetWrite(colorSurface,
			&regeneratedColorLease) == RENDER_RESULT_OK &&
		resources.PublishRenderTargetWrite(depthSurface,
			&regeneratedDepthLease) == RENDER_RESULT_OK &&
		regeneratedColorLease.isValid() && regeneratedDepthLease.isValid() &&
		regeneratedColorLease.backendEpoch != staleColorLease.backendEpoch,
		"recovered real D3D11 outputs regenerate deterministic content and new leases");
	result |= Check(resources.DestroyTexture(colorTarget) &&
		resources.DestroyTexture(depthTarget),
		"single-mip recovery outputs release through typed ownership");

	result |= Check(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_RECOVERY, 1,
		RENDER_RESULT_OUT_OF_MEMORY) == RENDER_RESULT_OK &&
		device->recoverDevice() == RENDER_RESULT_OUT_OF_MEMORY &&
		!device->isOperational() && !resources.IsValid(replacement.resource) &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"failed texture recovery releases native objects and closes logical ownership");

	device->shutdown();
	delete device;
	return result;
}
}

int main()
{
	return TestCanonicalUploadValidation() | TestNativeD3D11Ownership();
}
