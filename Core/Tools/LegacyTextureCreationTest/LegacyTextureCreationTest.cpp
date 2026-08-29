#include <stdio.h>
#include <string.h>

#include "WWVegas/WW3D2/legacytexturecompat.h"

static int failures = 0;
int targaOpenCalls = 0;
static int ddsCallbackCalls = 0;

static void expectTrue(bool condition, const char *message)
{
	if (!condition)
	{
		++failures;
		printf("FAIL: %s\n", message);
	}
}

static void testRoutes()
{
	expectTrue(LegacyTextureCreation_Get_Route(
		LEGACY_TEXTURE_CREATION_CUBE) ==
		LEGACY_TEXTURE_ROUTE_NATIVE_REQUIREMENTS,
		"cube creation uses native D3D8 after requirement correction");
	expectTrue(LegacyTextureCreation_Get_Route(
		LEGACY_TEXTURE_CREATION_VOLUME) ==
		LEGACY_TEXTURE_ROUTE_NATIVE_REQUIREMENTS,
		"volume creation uses native D3D8 after requirement correction");
	expectTrue(LegacyTextureCreation_Get_Route(
		LEGACY_TEXTURE_CREATION_FILE) ==
		LEGACY_TEXTURE_ROUTE_PROJECT_DECODER,
		"file decode uses the existing project image decoder");
}

static void testCapabilityValidation()
{
	D3DCAPS8 caps;
	LegacyTextureCreationDescriptor descriptor;
	memset(&caps, 0, sizeof(caps));
	caps.TextureCaps = D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP;
	caps.MaxTextureWidth = 256;
	caps.MaxTextureHeight = 256;
	caps.MaxVolumeExtent = 128;
	caps.MaxTextureAspectRatio = 128;
	expectTrue(LegacyTextureCreation_Build_Cube_Descriptor(
		64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &descriptor),
		"cube descriptor builds before capability validation");
	expectTrue(LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps), "cube descriptor fits the advertised caps");
	caps.MaxTextureAspectRatio = 1;
	expectTrue(LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps),
		"cube validation measures face aspect ratio, not the sentinel depth");
	caps.MaxTextureAspectRatio = 128;
	descriptor.width = descriptor.height = 512;
	expectTrue(!LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps), "oversized cube is rejected without D3DX correction");

	expectTrue(LegacyTextureCreation_Build_Volume_Descriptor(
		64, 32, 16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &descriptor),
		"volume descriptor builds before capability validation");
	expectTrue(LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps), "volume descriptor fits the advertised caps");
	descriptor.depth = 256;
	expectTrue(!LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps), "oversized volume is rejected without D3DX correction");

	descriptor.width = descriptor.height = 5;
	descriptor.depth = 1;
	descriptor.mipLevels = 4;
	expectTrue(!LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps),
		"mip count follows D3D floor-halving levels for non-power-of-two sizes");

	caps.TextureCaps |= D3DPTEXTURECAPS_POW2;
	descriptor.depth = 16;
	descriptor.width = 48;
	expectTrue(!LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps), "non-power-of-two volume is rejected on POW2 hardware");
	caps.TextureCaps &= ~D3DPTEXTURECAPS_POW2;
	caps.TextureCaps |= D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
	expectTrue(LegacyTextureCreation_Validate_Descriptor_For_Caps(
		descriptor, caps),
		"conditional non-power-of-two hardware is not treated as unconditional POW2");
}

static void testFileRequestContract()
{
	expectTrue(LegacyTextureCreation_Is_Default_Dimension(
		LEGACY_TEXTURE_DIMENSION_DEFAULT),
		"the file path has an explicit default-dimension sentinel");
	expectTrue(LegacyTextureCreation_Is_Characterized_File_Format(
		D3DFMT_A8R8G8B8), "A8R8G8B8 is a characterized file destination");
	expectTrue(LegacyTextureCreation_Is_Characterized_File_Format(
		D3DFMT_X8R8G8B8), "X8R8G8B8 is a characterized file destination");
	expectTrue(!LegacyTextureCreation_Is_Characterized_File_Format(
		D3DFMT_V8U8), "bump-map format is not accepted by the color file path");
}

static bool TestDDSCallback(const char *, LegacyTextureDDSImage *image)
{
	++ddsCallbackCalls;
	if (image != 0) memset(image, 0, sizeof(*image));
	return false;
}

static void testDDSRegistrationAndOrder()
{
	LegacyTextureImageInfo sourceInfo;
	IDirect3DTexture8 *texture = 0;
	const HRESULT result = LegacyTextureCreation_Create_File_Texture(
		reinterpret_cast<IDirect3DDevice8 *>(1),
		"legacy-texture-compat-test.dds",
		LEGACY_TEXTURE_DIMENSION_DEFAULT,
		LEGACY_TEXTURE_DIMENSION_DEFAULT,
		0, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
		LEGACY_TEXTURE_FILTER_BOX, LEGACY_TEXTURE_FILTER_BOX, 0,
		&sourceInfo, 0, &texture);
	expectTrue(result == D3DERR_NOTAVAILABLE,
		"unregistered DDS path declines a missing test file safely");
	expectTrue(!LegacyTextureCreation_Has_DDS_Decode_Callback(),
		"Core-only texture tests start without a title DDS callback");
	LegacyTextureCreation_Register_DDS_Decode_Callback(&TestDDSCallback);
	expectTrue(LegacyTextureCreation_Has_DDS_Decode_Callback(),
		"title DDS callback registration is observable");
	texture = 0;
	ddsCallbackCalls = 0;
	targaOpenCalls = 0;
	const HRESULT registeredResult = LegacyTextureCreation_Create_File_Texture(
		reinterpret_cast<IDirect3DDevice8 *>(1),
		"legacy-texture-compat-test.dds",
		LEGACY_TEXTURE_DIMENSION_DEFAULT,
		LEGACY_TEXTURE_DIMENSION_DEFAULT,
		0, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
		LEGACY_TEXTURE_FILTER_BOX, LEGACY_TEXTURE_FILTER_BOX, 0,
		&sourceInfo, 0, &texture);
	expectTrue(registeredResult == D3DERR_NOTAVAILABLE,
		"a declining DDS callback preserves the WIC fallback result");
	expectTrue(targaOpenCalls > 0 && ddsCallbackCalls == 1,
		"file decode order is Targa before the external DDS callback");
	LegacyTextureCreation_Register_DDS_Decode_Callback(0);
	expectTrue(!LegacyTextureCreation_Has_DDS_Decode_Callback(),
		"DDS callback can be cleared for Core-only operation");
}

static void testCubeDescriptor()
{
	LegacyTextureCreationDescriptor descriptor;
	const bool built = LegacyTextureCreation_Build_Cube_Descriptor(
		64, 0, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
		&descriptor);
	expectTrue(built, "cube descriptor accepts a non-zero edge length");
	if (built)
	{
		expectTrue(descriptor.kind == LEGACY_TEXTURE_CREATION_CUBE,
			"cube descriptor records its resource kind");
		expectTrue(descriptor.width == 64 && descriptor.height == 64 &&
			descriptor.depth == 1,
			"cube descriptor maps one edge length to square dimensions");
		expectTrue(descriptor.mipLevels == 0 &&
			descriptor.usage == D3DUSAGE_RENDERTARGET &&
			descriptor.format == D3DFMT_A8R8G8B8 &&
			descriptor.pool == D3DPOOL_DEFAULT,
			"cube descriptor preserves mip, usage, format, and pool");
	}
	expectTrue(!LegacyTextureCreation_Build_Cube_Descriptor(
		0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &descriptor),
		"cube descriptor rejects a zero edge length");
}

static void testVolumeDescriptor()
{
	LegacyTextureCreationDescriptor descriptor;
	const bool built = LegacyTextureCreation_Build_Volume_Descriptor(
		32, 16, 8, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &descriptor);
	expectTrue(built, "volume descriptor accepts non-zero dimensions");
	if (built)
	{
		expectTrue(descriptor.kind == LEGACY_TEXTURE_CREATION_VOLUME,
			"volume descriptor records its resource kind");
		expectTrue(descriptor.width == 32 && descriptor.height == 16 &&
			descriptor.depth == 8,
			"volume descriptor preserves all three dimensions");
		expectTrue(descriptor.mipLevels == 3 && descriptor.usage == 0 &&
			descriptor.format == D3DFMT_A8R8G8B8 &&
			descriptor.pool == D3DPOOL_SYSTEMMEM,
			"volume descriptor preserves mip, usage, format, and pool");
	}
	expectTrue(!LegacyTextureCreation_Build_Volume_Descriptor(
		32, 16, 0, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &descriptor),
		"volume descriptor rejects a zero depth");
}

int main()
{
	testRoutes();
	testCubeDescriptor();
	testVolumeDescriptor();
	testCapabilityValidation();
	testFileRequestContract();
	testDDSRegistrationAndOrder();
	return failures;
}
