#include "nativew3dsampledtexture.h"
#include "nativew3dtextureowner.h"

#include <cstdio>
#include <cstring>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAILED: %s\n", message);
		return 1;
	}
	return 0;
}

NativeW3DSampledTextureMipView View(const unsigned char *data,
	size_t dataSize, size_t rowPitch)
{
	NativeW3DSampledTextureMipView view;
	view.data = data;
	view.dataSize = dataSize;
	view.rowPitch = rowPitch;
	return view;
}

bool EqualBytes(const TextureSubresourceData &data,
	const unsigned char *expected, size_t expectedSize)
{
	return data.data != 0 && data.slicePitch == expectedSize &&
		std::memcmp(data.data, expected, expectedSize) == 0;
}

int TestDxtAndPackedPixelParity()
{
	int result = 0;
	NativeW3DSampledTextureUpload upload;
	static const unsigned char dxt1Source[8] = {
		0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char dxt1Expected[4] = { 0x00, 0x00, 0xff, 0xff };
	NativeW3DSampledTextureMipView source = View(dxt1Source,
		sizeof(dxt1Source), sizeof(dxt1Source));
	result |= Check(upload.Prepare(WW3D_FORMAT_DXT1, 1, 1, 1, 1,
		&source, 1) &&
		upload.Descriptor().format == RENDER_FORMAT_B8G8R8A8_UNORM &&
		upload.Subresources()[0].rowPitch == 4 &&
		EqualBytes(upload.Subresources()[0], dxt1Expected,
			sizeof(dxt1Expected)),
		"DXT1 decodes to byte-exact BGRA pixels");

	static const unsigned char dxt3Source[16] = {
		0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
		0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char dxt3Expected[4] = { 0x00, 0x00, 0xff, 0x88 };
	source = View(dxt3Source, sizeof(dxt3Source), sizeof(dxt3Source));
	result |= Check(upload.Prepare(WW3D_FORMAT_DXT3, 1, 1, 1, 1,
		&source, 1) && EqualBytes(upload.Subresources()[0], dxt3Expected,
			sizeof(dxt3Expected)),
		"DXT3 decodes straight-alpha BC2 to byte-exact BGRA pixels");

	// DXT2 and DXT4 designate premultiplied-alpha BC2/BC3 payloads. A stored
	// half-alpha red remains half-intensity after expansion; the upload must not
	// silently un-premultiply it into a full-intensity straight-alpha color.
	static const unsigned char dxt2Source[16] = {
		0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
		0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char premultipliedExpected[4] = {
		0x00, 0x00, 0x84, 0x88
	};
	source = View(dxt2Source, sizeof(dxt2Source), sizeof(dxt2Source));
	result |= Check(upload.Prepare(WW3D_FORMAT_DXT2, 1, 1, 1, 1,
		&source, 1) && EqualBytes(upload.Subresources()[0],
			premultipliedExpected, sizeof(premultipliedExpected)),
		"DXT2 preserves premultiplied RGB while decoding BC2 alpha");

	static const unsigned char dxt4Source[16] = {
		0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	source = View(dxt4Source, sizeof(dxt4Source), sizeof(dxt4Source));
	result |= Check(upload.Prepare(WW3D_FORMAT_DXT4, 1, 1, 1, 1,
		&source, 1) && EqualBytes(upload.Subresources()[0],
			premultipliedExpected, sizeof(premultipliedExpected)),
		"DXT4 preserves premultiplied RGB while decoding BC3 alpha");

	static const unsigned char dxt5Source[16] = {
		0xcc, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xe0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char dxt5Expected[4] = { 0x00, 0xff, 0x00, 0xcc };
	source = View(dxt5Source, sizeof(dxt5Source), sizeof(dxt5Source));
	result |= Check(upload.Prepare(WW3D_FORMAT_DXT5, 1, 1, 1, 1,
		&source, 1) && EqualBytes(upload.Subresources()[0], dxt5Expected,
			sizeof(dxt5Expected)),
		"DXT5 decodes to byte-exact BGRA color and alpha");

	static const unsigned char packedSource[2] = { 0x93, 0x75 };
	static const unsigned char packedExpected[4] = { 0x33, 0x99, 0x55, 0x77 };
	source = View(packedSource, sizeof(packedSource), sizeof(packedSource));
	result |= Check(upload.Prepare(WW3D_FORMAT_A4R4G4B4, 1, 1, 1, 1,
		&source, 1) && EqualBytes(upload.Subresources()[0], packedExpected,
			sizeof(packedExpected)),
		"packed A4R4G4B4 expands with legacy channel parity");
	return result;
}

int TestSignedBumpAndCubeLayout()
{
	int result = 0;
	NativeW3DSampledTextureUpload upload;
	static const unsigned char signedBump[12] = {
		0x80, 0x7f, 0x01, 0xff, 0xaa, 0xaa,
		0x11, 0x22, 0x33, 0x44, 0xbb, 0xbb
	};
	static const unsigned char signedExpected[8] = {
		0x80, 0x7f, 0x01, 0xff, 0x11, 0x22, 0x33, 0x44
	};
	NativeW3DSampledTextureMipView source = View(signedBump,
		sizeof(signedBump), 6);
	result |= Check(upload.Prepare(WW3D_FORMAT_U8V8, 2, 2, 1, 1,
		&source, 1) &&
		upload.Descriptor().format == RENDER_FORMAT_R8G8_SNORM &&
		upload.Subresources()[0].rowPitch == 4 &&
		EqualBytes(upload.Subresources()[0], signedExpected,
			sizeof(signedExpected)),
		"U8V8 preserves exact signed component bytes and removes row padding");

	unsigned char cubePixels[6][4];
	NativeW3DSampledTextureMipView cubeViews[6];
	for (unsigned int face = 0; face < 6; ++face)
	{
		cubePixels[face][0] = static_cast<unsigned char>(face + 1);
		cubePixels[face][1] = static_cast<unsigned char>(face + 11);
		cubePixels[face][2] = static_cast<unsigned char>(face + 21);
		cubePixels[face][3] = static_cast<unsigned char>(face + 31);
		cubeViews[face] = View(cubePixels[face], 4, 4);
	}
	result |= Check(upload.Prepare(WW3D_FORMAT_A8R8G8B8, 1, 1, 1, 6,
		cubeViews, 6) &&
		upload.Descriptor().dimension == RENDER_TEXTURE_CUBE &&
		upload.Descriptor().arrayCount == 6 &&
		upload.SubresourceCount() == 6,
		"six array slices produce an immutable shader-resource cube");
	for (unsigned int face = 0; face < 6; ++face)
	{
		result |= Check(EqualBytes(upload.Subresources()[face], cubePixels[face], 4),
			"cube subresources preserve face order and pixels");
	}
	return result;
}

int TestUnsupportedAndMalformedRejection()
{
	int result = 0;
	NativeW3DSampledTextureUpload upload;
	result |= Check(
		NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_DXT1) &&
		NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_DXT5) &&
		NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_DXT2) &&
		NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_DXT3) &&
		NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_DXT4) &&
		NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_U8V8) &&
		!NativeW3DSampledTextureUpload::SupportsSourceFormat(
			WW3D_FORMAT_P8),
		"capability query adopts characterized DXT layouts only");
	unsigned char bytes[16] = { 0 };
	NativeW3DSampledTextureMipView source = View(bytes, sizeof(bytes), 16);
	const WW3DFormat explicitLegacy[] = {
		WW3D_FORMAT_L6V5U5, WW3D_FORMAT_X8L8V8U8
	};
	for (unsigned int index = 0;
		index < sizeof(explicitLegacy) / sizeof(explicitLegacy[0]); ++index)
	{
		result |= Check(!upload.Prepare(explicitLegacy[index], 1, 1, 1, 1,
			&source, 1) && upload.SubresourceCount() == 0,
			"unproven sampled formats remain on the explicit legacy path");
	}

	NativeW3DSampledTextureMipView shortSource = View(bytes, 3, 4);
	result |= Check(!upload.Prepare(WW3D_FORMAT_A8R8G8B8, 1, 1, 1, 1,
		&shortSource, 1) && upload.Subresources() == 0,
		"short source storage fails closed without a partial publication");
	NativeW3DSampledTextureMipView shortDxt = View(bytes, 15, 16);
	result |= Check(!upload.Prepare(WW3D_FORMAT_DXT3, 1, 1, 1, 1,
		&shortDxt, 1) && upload.Subresources() == 0,
		"short BC2 storage fails closed without a partial publication");
	result |= Check(!upload.Prepare(WW3D_FORMAT_A8R8G8B8, 2, 1, 1, 2,
		&source, 1),
		"volume-like array layouts are rejected by the sampled 2D/cube boundary");
	result |= Check(!upload.Prepare(WW3D_FORMAT_A8R8G8B8, 2, 1, 2, 1,
		&source, 1),
		"incomplete mip arrays are rejected before conversion");
	return result;
}

int TestTypedPublicationFallbackAndRecovery()
{
	int result = 0;
	IRenderDevice *device = CreateD3D11RenderDevice();
	result |= Check(device != 0,
		"typed sampled publication allocates a real D3D11 backend");
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
		"typed sampled publication initializes headlessly");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	static const unsigned char dxt3Source[16] = {
		0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
		0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	NativeW3DSampledTextureMipView view = View(dxt3Source,
		sizeof(dxt3Source), sizeof(dxt3Source));
	NativeW3DSampledTextureUpload upload;
	result |= Check(upload.Prepare(WW3D_FORMAT_DXT3, 1, 1, 1, 1,
		&view, 1), "real typed publication prepares a BC2 CPU-authoritative upload");

	NativeW3DResourceHost host(16);
	NativeW3DResources resources(16);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK,
		"typed publication binds the exact resource generation");

	NativeW3DTextureOwner sharedMissingOwner;
	NativeW3DTextureCandidate sharedMissingCandidate;
	result |= Check(sharedMissingOwner.CreateCandidate(upload.Descriptor(),
		upload.Subresources(), upload.SubresourceCount(),
		&sharedMissingCandidate) == RENDER_RESULT_OK &&
		sharedMissingOwner.PublishCandidate(&sharedMissingCandidate, 0) ==
			RENDER_RESULT_OK,
		"the missing asset publishes one shared owned typed image");
	NativeW3DTextureHandle sharedMissing;
	result |= Check(sharedMissingOwner.AcquireForSampling(&sharedMissing) ==
		RENDER_RESULT_OK && sharedMissing.isValid(),
		"the shared missing image is CPU-authoritative for sampling");

	NativeW3DTextureOwner productOwner;
	NativeW3DTextureCandidate borrowedMissing;
	result |= Check(productOwner.BorrowCandidate(sharedMissing,
		upload.Descriptor(), &borrowedMissing) == RENDER_RESULT_OK &&
		productOwner.PublishCandidate(&borrowedMissing, 0) == RENDER_RESULT_OK,
		"a missing file deterministically borrows the shared typed image");
	NativeW3DTextureHandle bound;
	const RenderResult initialAcquire = productOwner.AcquireForSampling(&bound);
	result |= Check(initialAcquire == RENDER_RESULT_OK &&
		bound.resource == sharedMissing.resource,
		"the bridge-facing typed handle resolves without a legacy texture pointer");
	result |= Check(initialAcquire == RENDER_RESULT_OK &&
		device->immediateContext()->beginFrame() == RENDER_RESULT_OK &&
		device->immediateContext()->setTexture(0, bound.resource) ==
			RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() == RENDER_RESULT_OK,
		"the bridge-facing typed handle binds through the native context");

	{
		NativeW3DTextureCandidate staleReplacement;
		result |= Check(productOwner.CreateCandidate(upload.Descriptor(),
			upload.Subresources(), upload.SubresourceCount(),
			&staleReplacement) == RENDER_RESULT_OK &&
			productOwner.PublishCandidate(&staleReplacement, 0) ==
				RENDER_RESULT_FAILED && staleReplacement.IsValid(),
			"a stale product publication cannot replace the borrowed fallback");
		bound = NativeW3DTextureHandle();
		result |= Check(productOwner.AcquireForSampling(&bound) ==
			RENDER_RESULT_OK && bound.resource == sharedMissing.resource,
			"stale publication preserves the exact borrowed missing image");
	}

	result |= Check(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_ALLOCATION, 1,
		RENDER_RESULT_FAILED) == RENDER_RESULT_OK,
		"the sampled publication fixture arms one allocation fault");
	NativeW3DTextureCandidate failedCandidate;
	result |= Check(productOwner.CreateCandidate(upload.Descriptor(),
		upload.Subresources(), upload.SubresourceCount(), &failedCandidate) ==
		RENDER_RESULT_FAILED && !failedCandidate.IsValid(),
		"candidate allocation fault produces no partial product token");
	bound = NativeW3DTextureHandle();
	result |= Check(productOwner.AcquireForSampling(&bound) ==
		RENDER_RESULT_OK && bound.resource == sharedMissing.resource,
		"allocation fault leaves deterministic fallback publication intact");

	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK,
		"sampled publication recovery replaces the backend context");
	bound = NativeW3DTextureHandle();
	const RenderResult recoveredAcquire =
		productOwner.AcquireForSampling(&bound);
	result |= Check(recoveredAcquire == RENDER_RESULT_OK &&
		bound.resource == sharedMissing.resource,
		"CPU shadow recovery preserves borrowed typed sampling authority");
	result |= Check(recoveredAcquire == RENDER_RESULT_OK &&
		device->immediateContext()->beginFrame() == RENDER_RESULT_OK &&
		device->immediateContext()->setTexture(0, bound.resource) ==
			RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() == RENDER_RESULT_OK,
		"recovered borrowed sampling authority binds through the native context");

	RenderResourceStatistics borrowedStatistics;
	result |= Check(productOwner.Reset() == RENDER_RESULT_OK &&
		device->getDebugResourceStatistics(&borrowedStatistics) ==
			RENDER_RESULT_OK && borrowedStatistics.textureCount == 1,
		"resetting a borrowed fallback never destroys the shared texture");
	result |= Check(sharedMissingOwner.Reset() == RENDER_RESULT_OK &&
		UnbindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"sampled owners reset before boundary unbind and registry shutdown");
	device->shutdown();
	delete device;
	return result;
}
}

int main()
{
	return TestDxtAndPackedPixelParity() |
		TestSignedBumpAndCubeLayout() |
		TestUnsupportedAndMalformedRejection() |
		TestTypedPublicationFallbackAndRecovery();
}
