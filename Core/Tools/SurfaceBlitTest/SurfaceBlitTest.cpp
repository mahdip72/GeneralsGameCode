#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "WWVegas/WW3D2/surfaceblit.h"

static int failures = 0;

class FakeSurface;

// SurfaceBlit_Copy_Surface_To_A8R8G8B8 is deliberately tested through the
// D3D8 surface contract.  The direct-lock cases need only a surface mock; the
// fallback case also supplies the two device vtable entries that the
// production code calls (CreateImageSurface and CopyRects).  Keeping this
// seam local to the test avoids adding test-only hooks to the renderer.
class FakeDevice
{
public:
	void **vtable;
	void *entries[64];
	ULONG references;
	unsigned createImageSurfaceCalls;
	unsigned copyRectsCalls;

	FakeDevice();

	IDirect3DDevice8 *AsInterface()
	{
		return reinterpret_cast<IDirect3DDevice8 *>(this);
	}

	void AddReference()
	{
		++references;
	}

	void ReleaseReference()
	{
		if (references > 0)
			--references;
	}

	static ULONG STDMETHODCALLTYPE AddRef(IDirect3DDevice8 *device);
	static ULONG STDMETHODCALLTYPE Release(IDirect3DDevice8 *device);
	static HRESULT STDMETHODCALLTYPE CreateImageSurface(
		IDirect3DDevice8 *device, UINT width, UINT height, D3DFORMAT format,
		IDirect3DSurface8 **surface);
	static HRESULT STDMETHODCALLTYPE CopyRects(
		IDirect3DDevice8 *device, IDirect3DSurface8 *source,
		CONST RECT *sourceRects, UINT rectCount,
		IDirect3DSurface8 *destination, CONST POINT *destinationPoints);
};

class FakeSurface : public IDirect3DSurface8
{
public:
	FakeDevice *device;
	D3DSURFACE_DESC description;
	std::vector<unsigned char> bytes;
	int pitch;
	HRESULT lockResult;
	HRESULT unlockResult;
	unsigned lockCalls;
	unsigned unlockCalls;
	ULONG references;
	bool deleteOnRelease;

	FakeSurface(FakeDevice *surfaceDevice, const D3DSURFACE_DESC &surfaceDescription,
		const unsigned char *initialBytes, unsigned byteCount,
		HRESULT surfaceLockResult, HRESULT surfaceUnlockResult,
		bool destroyOnRelease)
		: device(surfaceDevice), description(surfaceDescription),
			bytes(),
			pitch((int)(surfaceDescription.Width * 4U)),
			lockResult(surfaceLockResult), unlockResult(surfaceUnlockResult),
			lockCalls(0), unlockCalls(0), references(1),
			deleteOnRelease(destroyOnRelease)
	{
		if (initialBytes != 0 && byteCount != 0)
			bytes.assign(initialBytes, initialBytes + byteCount);
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override
	{
		if (object != 0)
			*object = 0;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++references;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		if (references > 0)
			--references;
		const ULONG remaining = references;
		if (remaining == 0 && deleteOnRelease)
			delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **result) override
	{
		if (result == 0)
			return D3DERR_INVALIDCALL;
		*result = 0;
		if (device == 0)
			return D3DERR_NOTAVAILABLE;
		device->AddReference();
		*result = device->AsInterface();
		return D3D_OK;
	}

	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void *, DWORD, DWORD) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void **) override
	{
		return E_NOTIMPL;
	}

	HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *result) override
	{
		if (result == 0)
			return D3DERR_INVALIDCALL;
		*result = description;
		return D3D_OK;
	}

	HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *locked,
		CONST RECT *, DWORD) override
	{
		++lockCalls;
		if (locked == 0)
			return D3DERR_INVALIDCALL;
		if (FAILED(lockResult))
			return lockResult;
		locked->Pitch = pitch;
		locked->pBits = bytes.empty() ? 0 : &bytes[0];
		return D3D_OK;
	}

	HRESULT STDMETHODCALLTYPE UnlockRect() override
	{
		++unlockCalls;
		return unlockResult;
	}
};

static void *fakeFunctionPointer(uintptr_t value)
{
	return reinterpret_cast<void *>(value);
}

FakeDevice::FakeDevice()
	: vtable(entries), references(1), createImageSurfaceCalls(0),
		copyRectsCalls(0)
{
	memset(entries, 0, sizeof(entries));
	entries[1] = fakeFunctionPointer(reinterpret_cast<uintptr_t>(&FakeDevice::AddRef));
	entries[2] = fakeFunctionPointer(reinterpret_cast<uintptr_t>(&FakeDevice::Release));
	entries[27] = fakeFunctionPointer(reinterpret_cast<uintptr_t>(&FakeDevice::CreateImageSurface));
	entries[28] = fakeFunctionPointer(reinterpret_cast<uintptr_t>(&FakeDevice::CopyRects));
}

ULONG STDMETHODCALLTYPE FakeDevice::AddRef(IDirect3DDevice8 *device)
{
	FakeDevice *fake = reinterpret_cast<FakeDevice *>(device);
	fake->AddReference();
	return fake->references;
}

ULONG STDMETHODCALLTYPE FakeDevice::Release(IDirect3DDevice8 *device)
{
	FakeDevice *fake = reinterpret_cast<FakeDevice *>(device);
	fake->ReleaseReference();
	return fake->references;
}

HRESULT STDMETHODCALLTYPE FakeDevice::CreateImageSurface(
	IDirect3DDevice8 *device, UINT width, UINT height, D3DFORMAT format,
	IDirect3DSurface8 **surface)
{
	FakeDevice *fake = reinterpret_cast<FakeDevice *>(device);
	if (surface == 0 || width == 0 || height == 0 ||
		format != D3DFMT_A8R8G8B8)
		return D3DERR_INVALIDCALL;
	*surface = 0;
	D3DSURFACE_DESC description;
	memset(&description, 0, sizeof(description));
	description.Format = format;
	description.Width = width;
	description.Height = height;
	const unsigned byteCount = width * height * 4U;
	try
	{
		FakeSurface *created = new FakeSurface(fake, description, 0, 0,
			D3D_OK, D3D_OK, true);
		created->bytes.resize(byteCount);
		*surface = created;
		++fake->createImageSurfaceCalls;
		return D3D_OK;
	}
	catch (...)
	{
		return E_OUTOFMEMORY;
	}
}

HRESULT STDMETHODCALLTYPE FakeDevice::CopyRects(
	IDirect3DDevice8 *device, IDirect3DSurface8 *source,
	CONST RECT *, UINT rectCount, IDirect3DSurface8 *destination,
	CONST POINT *)
{
	FakeDevice *fake = reinterpret_cast<FakeDevice *>(device);
	if (source == 0 || destination == 0 || rectCount > 1)
		return D3DERR_INVALIDCALL;
	FakeSurface *sourceSurface = static_cast<FakeSurface *>(source);
	FakeSurface *destinationSurface = static_cast<FakeSurface *>(destination);
	const unsigned sourceRowBytes = sourceSurface->description.Width * 4U;
	const unsigned destinationRowBytes = destinationSurface->description.Width * 4U;
	const unsigned rowBytes = sourceRowBytes < destinationRowBytes ?
		sourceRowBytes : destinationRowBytes;
	const unsigned rows = sourceSurface->description.Height <
		destinationSurface->description.Height ? sourceSurface->description.Height :
		destinationSurface->description.Height;
	if (sourceSurface->bytes.size() < (size_t)sourceSurface->pitch * rows ||
		destinationSurface->bytes.size() < (size_t)destinationSurface->pitch * rows)
		return D3DERR_INVALIDCALL;
	for (unsigned y = 0; y < rows; ++y)
		memcpy(&destinationSurface->bytes[(size_t)y * destinationSurface->pitch],
			&sourceSurface->bytes[(size_t)y * sourceSurface->pitch], rowBytes);
	++fake->copyRectsCalls;
	return D3D_OK;
}

static void expectTrue(bool condition, const char *message)
{
	if (!condition)
	{
		++failures;
		printf("FAIL: %s\n", message);
	}
}

static void expectBytes(const unsigned char *actual,
	const unsigned char *expected, unsigned int count, const char *message)
{
	if (memcmp(actual, expected, count) != 0)
	{
		++failures;
		printf("FAIL: %s\n", message);
	}
}

static D3DSURFACE_DESC description(D3DFORMAT format,
	unsigned int width, unsigned int height)
{
	D3DSURFACE_DESC result;
	memset(&result, 0, sizeof(result));
	result.Format = format;
	result.Width = width;
	result.Height = height;
	return result;
}

static void testCopyRectsAcceptanceMatrix()
{
	const D3DSURFACE_DESC source = description(D3DFMT_A8R8G8B8, 8, 8);
	const D3DSURFACE_DESC destination = description(D3DFMT_A8R8G8B8, 16, 16);
	RECT sourceRect = { 1, 2, 5, 6 };
	RECT destinationRect = { 4, 3, 8, 7 };

	// This is the exact native path characterized for FILTER_NONE: same
	// format and equal-size rectangles, with no resampling.
	expectTrue(SurfaceBlit_Can_Use_CopyRects(destination, destinationRect,
		source, sourceRect, SURFACE_BLIT_FILTER_NONE),
		"equal-size same-format FILTER_NONE uses CopyRects");
	expectTrue(!SurfaceBlit_Can_Use_CopyRects(destination, destinationRect,
		source, sourceRect, SURFACE_BLIT_FILTER_TRIANGLE),
		"triangle filtering never uses CopyRects");

	destinationRect.right = 9;
	expectTrue(!SurfaceBlit_Can_Use_CopyRects(destination, destinationRect,
		source, sourceRect, SURFACE_BLIT_FILTER_NONE),
		"scaled FILTER_NONE remains on the legacy conversion boundary");
	destinationRect.right = 8;
	destinationRect.bottom = 8;
	expectTrue(!SurfaceBlit_Can_Use_CopyRects(destination, destinationRect,
		source, sourceRect, SURFACE_BLIT_FILTER_NONE),
		"different destination height remains on the legacy conversion boundary");

	const D3DSURFACE_DESC differentFormat = description(D3DFMT_X8R8G8B8, 16, 16);
	expectTrue(!SurfaceBlit_Can_Use_CopyRects(differentFormat, destinationRect,
		source, sourceRect, SURFACE_BLIT_FILTER_NONE),
		"format conversion remains on the legacy conversion boundary");

	const D3DSURFACE_DESC compressedSource = description(D3DFMT_DXT1, 8, 8);
	const D3DSURFACE_DESC compressedDestination = description(D3DFMT_DXT1, 8, 8);
	const RECT fullCompressed = { 0, 0, 8, 8 };
	expectTrue(SurfaceBlit_Can_Use_CopyRects(compressedDestination,
		fullCompressed, compressedSource, fullCompressed,
		SURFACE_BLIT_FILTER_NONE),
		"equal-size DXT levels retain the native CopyRects path");

	static const unsigned char sample[] = {
		0, 0, 0, 0xff, 0xff, 0, 0, 0xff
	};
	unsigned char filtered[4];
	expectTrue(!SurfaceBlit_Resample_A8R8G8B8(sample, 2, 1, filtered, 1, 1,
		(SurfaceBlitFilter)99),
		"uncharacterized filter options fail closed");
}

static void testFullCopyRoutingMatrix()
{
	const D3DSURFACE_DESC source = description(D3DFMT_A8R8G8B8, 8, 8);
	const D3DSURFACE_DESC same = description(D3DFMT_A8R8G8B8, 8, 8);
	const D3DSURFACE_DESC scaled = description(D3DFMT_A8R8G8B8, 16, 16);
	const D3DSURFACE_DESC converted = description(D3DFMT_X8R8G8B8, 8, 8);

	expectTrue(SurfaceBlit_Filter_For_Full_Copy(same, source) ==
		SURFACE_BLIT_FILTER_NONE,
		"full equal-format/equal-size copy uses the native path");
	expectTrue(SurfaceBlit_Filter_For_Full_Copy(scaled, source) ==
		SURFACE_BLIT_FILTER_BOX,
		"full scaled copy retains BOX filtering");
	expectTrue(SurfaceBlit_Filter_For_Full_Copy(converted, source) ==
		SURFACE_BLIT_FILTER_BOX,
		"full format conversion retains the legacy boundary");
}

static void testConversionReferenceMatrix()
{
	static const unsigned char a8r8g8b8Source[] = { 3, 8, 12, 64 };
	static const unsigned char x8r8g8b8Expected[] = { 3, 8, 12, 0xff };
	static const unsigned char r8g8b8Source[] = { 3, 8, 12 };
	static const unsigned char r8g8b8Expected[] = { 3, 8, 12, 0xff };
	static const unsigned char sourceWithPadding[] = {
		3, 8, 12, 64, 9, 9, 9, 9,
		20, 24, 28, 128, 7, 7, 7, 7
	};
	unsigned char expected[8];
	std::vector<unsigned char> pixels;

	expectTrue(SurfaceBlit_Convert_To_A8R8G8B8(a8r8g8b8Source, 4, 1, 1,
		D3DFMT_X8R8G8B8, &pixels),
		"X8R8G8B8 conversion is supported");
	expectBytes(&pixels[0], x8r8g8b8Expected, 4,
		"X8R8G8B8 conversion supplies opaque alpha (native D3DX8 reference)");

	expectTrue(SurfaceBlit_Convert_To_A8R8G8B8(r8g8b8Source, 3, 1, 1,
		D3DFMT_R8G8B8, &pixels),
		"R8G8B8 conversion is supported");
	expectBytes(&pixels[0], r8g8b8Expected, 4,
		"R8G8B8 conversion supplies opaque alpha");

	expectTrue(SurfaceBlit_Convert_To_A8R8G8B8(sourceWithPadding, 8, 1, 2,
		D3DFMT_A8R8G8B8, &pixels),
		"conversion accepts a padded source pitch");
	memcpy(expected, sourceWithPadding, 4);
	memcpy(expected + 4, sourceWithPadding + 8, 4);
	expectBytes(&pixels[0], expected, 8,
		"conversion ignores source row padding");

	// These are the uncompressed production formats used by terrain, trees,
	// image loading, and render-target readback.  The conversion must retain
	// the D3D8 byte/channel rules instead of handing the operation to D3DX.
	static const unsigned char a1Source[] = { 0x4b, 0xaa };
	static const unsigned char x1Source[] = { 0x4b, 0x2a };
	static const unsigned char a4Source[] = { 0x93, 0x75 };
	static const unsigned char x4Source[] = { 0x93, 0x05 };
	static const unsigned char r565Source[] = { 0x2b, 0x53 };
	static const unsigned char a8r3g3b2Source[] = { 0x72, 0x5a };
	static const unsigned char a8l8Source[] = { 0x72, 0x5a };
	static const unsigned char a4l4Source[] = { 0x57 };
	static const unsigned char l8Source[] = { 0x63 };
	static const unsigned char a8Source[] = { 0x5a };
	static const unsigned char r3g3b2Source[] = { 0x72 };
	static const unsigned char packedExpected[] = {
		0x5a, 0x94, 0x52, 0xff,
		0x5a, 0x94, 0x52, 0xff,
		0x33, 0x99, 0x55, 0x77,
		0x33, 0x99, 0x55, 0xff,
		0x5a, 0x65, 0x52, 0xff,
		0xaa, 0x92, 0x6d, 0x5a,
		0x72, 0x72, 0x72, 0x5a,
		0x77, 0x77, 0x77, 0x55,
		0x63, 0x63, 0x63, 0xff,
		0x00, 0x00, 0x00, 0x5a,
		0xaa, 0x92, 0x6d, 0xff
	};
	const struct ConversionCase {
		const unsigned char *source;
		unsigned pitch;
		D3DFORMAT format;
		unsigned expectedOffset;
	} cases[] = {
		{ a1Source, 2, D3DFMT_A1R5G5B5, 0 },
		{ x1Source, 2, D3DFMT_X1R5G5B5, 4 },
		{ a4Source, 2, D3DFMT_A4R4G4B4, 8 },
		{ x4Source, 2, D3DFMT_X4R4G4B4, 12 },
		{ r565Source, 2, D3DFMT_R5G6B5, 16 },
		{ a8r3g3b2Source, 2, D3DFMT_A8R3G3B2, 20 },
		{ a8l8Source, 2, D3DFMT_A8L8, 24 },
		{ a4l4Source, 1, D3DFMT_A4L4, 28 },
		{ l8Source, 1, D3DFMT_L8, 32 },
		{ a8Source, 1, D3DFMT_A8, 36 },
		{ r3g3b2Source, 1, D3DFMT_R3G3B2, 40 }
	};
	unsigned i;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
	{
		expectTrue(SurfaceBlit_Convert_To_A8R8G8B8(cases[i].source,
			cases[i].pitch, 1, 1, cases[i].format, &pixels),
			"production packed format has a characterized CPU conversion");
		if (pixels.size() == 4)
		{
			expectBytes(&pixels[0], packedExpected + cases[i].expectedOffset,
				4, "packed format expansion matches the D3D8 channel layout");
		}
	}

	static const unsigned char dxt1Source[8] = {
		0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char dxt1Expected[4] = { 0x00, 0x00, 0xff, 0xff };
	expectTrue(SurfaceBlit_Convert_To_A8R8G8B8(dxt1Source, 8, 1, 1,
		D3DFMT_DXT1, &pixels),
		"DXT1 block decode is supported for D3D11 readback");
	expectBytes(&pixels[0], dxt1Expected,
		4, "DXT1 block decode preserves the source color");

	static const unsigned char unsupported[] = { 0, 0, 0, 0 };
	expectTrue(!SurfaceBlit_Convert_To_A8R8G8B8(unsupported, 4, 1, 1,
		D3DFMT_V8U8, &pixels),
		"bump-map V8U8 conversion fails closed outside the color readback path");
}

static void testCpuResampling()
{
	static const unsigned char source[] = {
		0, 0, 0, 0xff,  0xff, 0, 0, 0xff,
		0, 0xff, 0, 0xff,  0, 0, 0xff, 0xff
	};
	static const unsigned char expected[4] = { 0x40, 0x40, 0x40, 0xff };
	unsigned char destination[4];
	expectTrue(SurfaceBlit_Resample_A8R8G8B8(source, 2, 2, destination, 1, 1,
		SURFACE_BLIT_FILTER_BOX),
		"BOX resampling reduces a full source rectangle on the CPU");
	expectBytes(destination, expected,
		4, "BOX resampling averages all four source pixels");

	static const unsigned char triangleExpected[4] = { 0x80, 0x00, 0x00, 0xff };
	expectTrue(SurfaceBlit_Resample_A8R8G8B8(source, 2, 1, destination, 1, 1,
		SURFACE_BLIT_FILTER_TRIANGLE),
		"TRIANGLE filtering uses the characterized CPU resampler");
	expectBytes(destination, triangleExpected, 4,
		"TRIANGLE filtering preserves the bilinear midpoint");

	static unsigned char smallBuffer[4] = { 0, 0, 0, 0 };
	expectTrue(!SurfaceBlit_Resample_A8R8G8B8(smallBuffer, 65536, 65536,
		smallBuffer, 1, 1, SURFACE_BLIT_FILTER_NONE),
		"oversized source dimensions fail before 32-bit byte-size overflow");
}

static void testSurfaceReadbackLockPaths()
{
	const D3DSURFACE_DESC surfaceDescription = description(
		D3DFMT_A8R8G8B8, 1, 1);
	static const unsigned char sourceBytes[] = { 3, 8, 12, 64 };
	static const unsigned char expectedBytes[] = { 3, 8, 12, 64 };
	std::vector<unsigned char> pixels;

	FakeSurface directSurface(0, surfaceDescription, sourceBytes,
		sizeof(sourceBytes), D3D_OK, D3D_OK, false);
	HRESULT result = SurfaceBlit_Copy_Surface_To_A8R8G8B8(
		&directSurface, 1, 1, &pixels);
	expectTrue(result == D3D_OK,
		"direct-lock readback succeeds for a lockable surface");
	expectTrue(directSurface.lockCalls == 1 && directSurface.unlockCalls == 1,
		"direct-lock readback unlocks exactly once");
	expectTrue(pixels.size() == sizeof(expectedBytes),
		"direct-lock readback returns one tightly packed pixel");
	if (pixels.size() == sizeof(expectedBytes))
		expectBytes(&pixels[0], expectedBytes, sizeof(expectedBytes),
			"direct-lock readback preserves the source pixel");

	FakeSurface unlockFailureSurface(0, surfaceDescription, sourceBytes,
		sizeof(sourceBytes), D3D_OK, D3DERR_NOTAVAILABLE, false);
	pixels.clear();
	result = SurfaceBlit_Copy_Surface_To_A8R8G8B8(
		&unlockFailureSurface, 1, 1, &pixels);
	expectTrue(result == D3DERR_NOTAVAILABLE,
		"direct-lock readback propagates an unlock failure");
	expectTrue(unlockFailureSurface.lockCalls == 1 &&
		unlockFailureSurface.unlockCalls == 1,
		"unlock failure still performs exactly one unlock");
	expectTrue(pixels.empty(),
		"unlock failure does not publish partially read-back pixels");

	FakeDevice device;
	FakeSurface fallbackSurface(&device, surfaceDescription, sourceBytes,
		sizeof(sourceBytes), D3DERR_NOTAVAILABLE, D3D_OK, false);
	pixels.clear();
	result = SurfaceBlit_Copy_Surface_To_A8R8G8B8(
		&fallbackSurface, 1, 1, &pixels);
	expectTrue(result == D3D_OK,
		"lock failure falls back to a system-memory staging surface");
	expectTrue(device.createImageSurfaceCalls == 1 &&
		device.copyRectsCalls == 1,
		"fallback readback creates staging and copies the source once");
	expectTrue(fallbackSurface.lockCalls == 1,
		"fallback readback does not retry a failed direct lock");
	expectTrue(pixels.size() == sizeof(expectedBytes),
		"fallback readback returns one tightly packed pixel");
	if (pixels.size() == sizeof(expectedBytes))
		expectBytes(&pixels[0], expectedBytes, sizeof(expectedBytes),
			"fallback readback preserves the source pixel");
	expectTrue(device.references == 1,
		"fallback readback releases its temporary device reference");
	device.ReleaseReference();
	expectTrue(device.references == 0,
		"test releases the device's original reference");
}

int main()
{
	testCopyRectsAcceptanceMatrix();
	testFullCopyRoutingMatrix();
	testConversionReferenceMatrix();
	testCpuResampling();
	testSurfaceReadbackLockPaths();
	return failures;
}
