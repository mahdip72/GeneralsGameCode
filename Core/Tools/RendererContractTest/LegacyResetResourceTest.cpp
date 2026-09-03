#include "Utility/CppMacros.h"
#include <stdio.h>
#include <string.h>

// Only the API/environment is doubled. The reset, buffer tracking and texture
// publication definitions below are extracted unchanged from production.
namespace
{
typedef int HRESULT;
typedef unsigned int UINT;
typedef unsigned long DWORD;
const HRESULT D3D_OK = 0;
const HRESULT D3DERR_INVALIDCALL = static_cast<HRESULT>(0x8876086cU);
const HRESULT D3DERR_DEVICELOST = static_cast<HRESULT>(0x88760868U);
#define FAILED(result) ((result) < 0)
#define SUCCEEDED(result) ((result) >= 0)
#define WWDEBUG_SAY(message) ((void)0)
#define WWASSERT(condition) ((void)0)
#define DX8_THREAD_ASSERT() ((void)0)
#define DX8_RECORD_VERTEX_BUFFER_CHANGE() ((void)0)
#define DX8_RECORD_INDEX_BUFFER_CHANGE() ((void)0)
#define DX8CALL(call) DX8Wrapper::_Get_D3D_Device8()->call
#define DX8CALL_HRES(call, result) result = DX8Wrapper::_Get_D3D_Device8()->call;
#define SHD_SHUTDOWN_SHADERS ((void)0)
#define SHD_INIT_SHADERS ((void)0)
#define ZeroMemory(address, length) memset(address, 0, length)

void DX8_Assert() {}
void DX8_ErrorCode(unsigned) {}
void Non_Fatal_Log_DX8_ErrorCode(unsigned, const char *, int) {}

int check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: legacy reset resource: %s\n", message);
	return 1;
}

struct Resource
{
	int references;
	Resource() : references(1) {}
	void AddRef() { ++references; }
	void Release() { --references; }
};
typedef Resource IDirect3DVertexBuffer8;
typedef Resource IDirect3DIndexBuffer8;

struct FakeDevice
{
	Resource *vertex;
	Resource *index;
	HRESULT cooperativeResult;
	HRESULT streamResult;
	HRESULT indicesResult;
	HRESULT resetResult;
	int streamCalls;
	int indicesCalls;
	int resetCalls;
	FakeDevice(Resource *vb, Resource *ib) : vertex(vb), index(ib),
		cooperativeResult(D3D_OK), streamResult(D3D_OK), indicesResult(D3D_OK),
		resetResult(D3D_OK), streamCalls(0), indicesCalls(0), resetCalls(0) {}
	HRESULT TestCooperativeLevel() { return cooperativeResult; }
	HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer8 *, UINT)
	{
		++streamCalls;
		return streamResult;
	}
	HRESULT SetIndices(IDirect3DIndexBuffer8 *, UINT)
	{
		++indicesCalls;
		return indicesResult;
	}
	HRESULT Reset(void *)
	{
		++resetCalls;
		// DEFAULT-pool application references must be gone before Reset. Device
		// bindings do not substitute for releasing the wrapper's own AddRefs.
		if (vertex->references != 0 || index->references != 0)
			return D3DERR_INVALIDCALL;
		return resetResult;
	}
};

struct CleanupHook
{
	Resource *vertex;
	Resource *index;
	int releaseCalls;
	int acquireCalls;
	CleanupHook(Resource *vb, Resource *ib) : vertex(vb), index(ib),
		releaseCalls(0), acquireCalls(0) {}
	void ReleaseResources()
	{
		++releaseCalls;
		if (vertex != nullptr) { vertex->Release(); vertex = nullptr; }
		if (index != nullptr) { index->Release(); index = nullptr; }
	}
	void ReAcquireResources() { ++acquireCalls; }
};

namespace rts { namespace render {
enum RenderResult { RENDER_RESULT_OK };
void ResetTrackedLegacyState() {}
void SeedTrackedLegacyPipelineState() {}
} }
struct FakeBridge
{
	bool allowPrepare;
	int resizeCalls;
	FakeBridge() : allowPrepare(true), resizeCalls(0) {}
	bool Prepare_Legacy_Device_Reset() { return allowPrepare; }
	bool Is_Active() { return true; }
	rts::render::RenderResult Resize(int, int)
	{
		++resizeCalls;
		return rts::render::RENDER_RESULT_OK;
	}
};
FakeBridge _D3D11Bridge;
bool _UseD3D11Backend = false;
struct WW3D
{
	static void _Invalidate_Textures() {}
	static unsigned Get_Sync_Time() { return 42; }
};
struct DynamicVBAccessClass { static void _Deinit() {} };
struct DynamicIBAccessClass { static void _Deinit() {} };
struct DX8TextureManagerClass
{
	static void Release_Textures() {}
	static void Recreate_Textures() {}
};
struct Vector4 { float values[4]; };
const int MAX_VERTEX_STREAMS = 1;
const int MAX_VERTEX_SHADER_CONSTANTS = 1;
const int MAX_PIXEL_SHADER_CONSTANTS = 1;

class DX8Wrapper
{
public:
	static bool IsInitted;
	static bool IsDeviceLost;
	static FakeDevice *D3DDevice;
	static CleanupHook *m_pCleanupHook;
	static IDirect3DVertexBuffer8 *RawVertexBuffer;
	static IDirect3DIndexBuffer8 *RawIndexBuffer;
	static UINT RawVertexStride;
	static DWORD RawVertexFVF;
	static UINT RawIndexBaseVertex;
	static int FrameCount;
	static int _PresentParameters;
	static int ResolutionWidth;
	static int ResolutionHeight;
	static Vector4 Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
	static Vector4 Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];
	static FakeDevice *_Get_D3D_Device8() { return D3DDevice; }
	static void Increment_DX8_CallCount() {}
	// Logical state setters deliberately do not release raw COM references.
	static void Set_Vertex_Buffer(void *, unsigned) {}
	static void Set_Index_Buffer(void *, unsigned) {}
	static void Invalidate_Cached_Render_States() {}
	static void Set_Default_Global_Render_States() {}
	static bool Reset_Device(bool reload_assets, bool *reset_requires_reacquire);
	static void Release_DX8_Buffer_Bindings();
	static void Track_DX8_Vertex_Buffer(IDirect3DVertexBuffer8 *, UINT, DWORD);
	static HRESULT Set_DX8_Vertex_Buffer(IDirect3DVertexBuffer8 *, UINT, DWORD);
	static HRESULT Set_DX8_Index_Buffer(IDirect3DIndexBuffer8 *, UINT);
};
bool DX8Wrapper::IsInitted = true;
bool DX8Wrapper::IsDeviceLost = false;
FakeDevice *DX8Wrapper::D3DDevice = nullptr;
CleanupHook *DX8Wrapper::m_pCleanupHook = nullptr;
IDirect3DVertexBuffer8 *DX8Wrapper::RawVertexBuffer = nullptr;
IDirect3DIndexBuffer8 *DX8Wrapper::RawIndexBuffer = nullptr;
UINT DX8Wrapper::RawVertexStride = 0;
DWORD DX8Wrapper::RawVertexFVF = 0;
UINT DX8Wrapper::RawIndexBaseVertex = 0;
int DX8Wrapper::FrameCount = 0;
int DX8Wrapper::_PresentParameters = 0;
int DX8Wrapper::ResolutionWidth = 2048;
int DX8Wrapper::ResolutionHeight = 1536;
Vector4 DX8Wrapper::Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
Vector4 DX8Wrapper::Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];

struct D3DSURFACE_DESC
{
	unsigned Format;
	unsigned Width;
	unsigned Height;
};
struct IDirect3DSurface8 : Resource
{
	HRESULT descResult;
	int descCalls;
	IDirect3DSurface8() : descResult(D3D_OK), descCalls(0) {}
	HRESULT GetDesc(D3DSURFACE_DESC *desc)
	{
		++descCalls;
		desc->Format = 21;
		desc->Width = 256;
		desc->Height = 128;
		return descResult;
	}
};
struct IDirect3DBaseTexture8 : Resource {};
struct IDirect3DTexture8 : IDirect3DBaseTexture8
{
	IDirect3DSurface8 surface;
	HRESULT surfaceResult;
	bool nullSurface;
	bool surfaceOnFailure;
	IDirect3DTexture8() : surfaceResult(D3D_OK), nullSurface(false),
		surfaceOnFailure(false) {}
	HRESULT GetSurfaceLevel(unsigned, IDirect3DSurface8 **output)
	{
		if (!nullSurface && (SUCCEEDED(surfaceResult) || surfaceOnFailure))
		{
			*output = &surface;
			surface.AddRef();
		}
		return surfaceResult;
	}
};
unsigned D3DFormat_To_WW3DFormat(unsigned format) { return format; }
class TextureBaseClass
{
public:
	IDirect3DBaseTexture8 *D3DTexture;
	unsigned LastAccessed;
	bool Initialized;
	bool NativeTexturePresent;
	bool NativeMissingResult;
	bool InitializedAtNativeRelease;
	bool InitializedAtNativeMissing;
	int NativeLifecycleSequence;
	int NativeReleaseOrder;
	int NativeMissingOrder;
	int NativeReleaseCalls;
	int NativeMissingCalls;
	TextureBaseClass() : D3DTexture(nullptr), LastAccessed(7),
		Initialized(false), NativeTexturePresent(false),
		NativeMissingResult(true), InitializedAtNativeRelease(false),
		InitializedAtNativeMissing(false), NativeLifecycleSequence(0),
		NativeReleaseOrder(0), NativeMissingOrder(0), NativeReleaseCalls(0),
		NativeMissingCalls(0) {}
	void Release_D3D_Texture();
	void Set_D3D_Base_Texture(IDirect3DBaseTexture8 *);
	void Release_Native_Texture()
	{
		++NativeReleaseCalls;
		NativeReleaseOrder = ++NativeLifecycleSequence;
		InitializedAtNativeRelease = Initialized;
		NativeTexturePresent = false;
	}
	bool Apply_Native_Missing_Texture()
	{
		++NativeMissingCalls;
		NativeMissingOrder = ++NativeLifecycleSequence;
		InitializedAtNativeMissing = Initialized;
		if (NativeMissingResult)
		{
			NativeTexturePresent = true;
			Initialized = true;
		}
		return NativeMissingResult;
	}
};
class TextureClass : public TextureBaseClass
{
public:
	unsigned InactivationTime;
	unsigned TextureFormat;
	unsigned Width;
	unsigned Height;
	TextureClass() : InactivationTime(100),
		TextureFormat(0), Width(8), Height(4) {}
	IDirect3DTexture8 *Peek_D3D_Texture()
	{
		return static_cast<IDirect3DTexture8 *>(D3DTexture);
	}
	void Apply_New_Surface(IDirect3DBaseTexture8 *, bool, bool);
};

#include "LegacyResetMethods.inc"

int testReset(bool nativeBackend, bool failStream, bool failIndices,
	bool failReset, bool reloadAssets)
{
	int result = 0;
	Resource vertex;
	Resource index;
	FakeDevice device(&vertex, &index);
	CleanupHook cleanup(&vertex, &index);
	DX8Wrapper::D3DDevice = &device;
	DX8Wrapper::m_pCleanupHook = &cleanup;
	_UseD3D11Backend = nativeBackend;
	DX8Wrapper::IsDeviceLost = true;
	_D3D11Bridge = FakeBridge();
	DX8Wrapper::Set_DX8_Vertex_Buffer(&vertex, 12, 2);
	DX8Wrapper::Set_DX8_Index_Buffer(&index, 2788);
	result |= check(vertex.references == 2 && index.references == 2,
		"production binders retain their own buffer references");
	device.streamResult = failStream ? D3DERR_INVALIDCALL : D3D_OK;
	device.indicesResult = failIndices ? D3DERR_INVALIDCALL : D3D_OK;
	device.resetResult = failReset ? D3DERR_INVALIDCALL : D3D_OK;
	bool requiresReacquire = false;
	const bool reset = DX8Wrapper::Reset_Device(reloadAssets, &requiresReacquire);
	result |= check(reset == !failReset && requiresReacquire,
		"reset reaches the device with all DEFAULT-pool application references released");
	result |= check(DX8Wrapper::IsDeviceLost == !reset,
		"successful legacy reset clears the device-lost publication latch");
	result |= check(device.resetCalls == 1 && vertex.references == 0 && index.references == 0,
		"both raw references are released before Reset, including rejected unbinds");
	result |= check(device.streamCalls == 2 && device.indicesCalls == 2,
		"reset also attempts to unbind both device buffers");
	result |= check(DX8Wrapper::RawVertexBuffer == nullptr &&
		DX8Wrapper::RawIndexBuffer == nullptr && DX8Wrapper::RawVertexStride == 0 &&
		DX8Wrapper::RawVertexFVF == 0 && DX8Wrapper::RawIndexBaseVertex == 0,
		"reset clears raw pointers and addressing metadata together");
	result |= check(cleanup.acquireCalls == ((!failReset && reloadAssets) ? 1 : 0) &&
		_D3D11Bridge.resizeCalls == ((!failReset && nativeBackend) ? 1 : 0),
		"resource reacquisition and bridge resize retain their success-path semantics");
	if (failReset)
	{
		device.resetResult = D3D_OK;
		result |= check(DX8Wrapper::Reset_Device(reloadAssets, &requiresReacquire),
			"a failed reset can be retried without stale raw references");
		result |= check(!DX8Wrapper::IsDeviceLost,
			"a successful reset retry also clears the device-lost publication latch");
	}
	DX8Wrapper::Release_DX8_Buffer_Bindings();
	result |= check(vertex.references == 0 && index.references == 0,
		"repeated cleanup does not double-release buffers");
	DX8Wrapper::D3DDevice = nullptr;
	DX8Wrapper::m_pCleanupHook = nullptr;
	return result;
}

int testResetPreflight()
{
	int result = 0;
	Resource vertex;
	Resource index;
	FakeDevice device(&vertex, &index);
	CleanupHook cleanup(&vertex, &index);
	DX8Wrapper::D3DDevice = &device;
	DX8Wrapper::m_pCleanupHook = &cleanup;
	_UseD3D11Backend = true;
	_D3D11Bridge = FakeBridge();
	DX8Wrapper::Set_DX8_Vertex_Buffer(&vertex, 12, 2);
	DX8Wrapper::Set_DX8_Index_Buffer(&index, 2788);
	bool requiresReacquire = true;
	device.cooperativeResult = D3DERR_DEVICELOST;
	result |= check(!DX8Wrapper::Reset_Device(true, &requiresReacquire) &&
		!requiresReacquire && vertex.references == 2 && index.references == 2,
		"not-yet-resettable device preserves resources");
	device.cooperativeResult = D3D_OK;
	_D3D11Bridge.allowPrepare = false;
	result |= check(!DX8Wrapper::Reset_Device(true, &requiresReacquire) &&
		!requiresReacquire && vertex.references == 2 && index.references == 2 &&
		device.resetCalls == 0 && cleanup.releaseCalls == 0,
		"bridge preparation failure does not prematurely destroy resources");
	DX8Wrapper::Release_DX8_Buffer_Bindings();
	cleanup.ReleaseResources();
	DX8Wrapper::D3DDevice = nullptr;
	DX8Wrapper::m_pCleanupHook = nullptr;
	return result;
}

int testTextureFailure(int failure, bool existing)
{
	int result = 0;
	TextureClass texture;
	IDirect3DTexture8 oldTexture;
	IDirect3DTexture8 incoming;
	if (existing)
	{
		texture.Set_D3D_Base_Texture(&oldTexture);
		texture.Initialized = true;
	}
	const unsigned previousAccess = texture.LastAccessed;
	if (failure == 0) incoming.surfaceResult = D3DERR_INVALIDCALL;
	if (failure == 1) incoming.nullSurface = true;
	if (failure == 2) incoming.surface.descResult = D3DERR_INVALIDCALL;
	if (failure == 3)
	{
		incoming.surfaceResult = D3DERR_INVALIDCALL;
		incoming.surfaceOnFailure = true;
	}
	texture.Apply_New_Surface(&incoming, true, true);
	result |= check(texture.D3DTexture == (existing ? &oldTexture : nullptr) &&
		texture.Initialized == existing && texture.LastAccessed == previousAccess,
		"failed publication preserves the old texture or leaves a new texture retryable");
	result |= check(texture.Width == 8 && texture.Height == 4 &&
		texture.TextureFormat == 0 && texture.InactivationTime == 100,
		"failed publication does not publish metadata or disable invalidation");
	result |= check(incoming.references == 1 && incoming.surface.references == 1 &&
		oldTexture.references == (existing ? 2 : 1),
		"failed texture queries balance references without acquiring the candidate");
	result |= check(incoming.surface.descCalls == (failure == 2 ? 1 : 0),
		"invalid or null surface is never queried for its descriptor");
	texture.Release_D3D_Texture();
	return result;
}

int testTextureSuccess()
{
	int result = 0;
	TextureClass texture;
	IDirect3DTexture8 first;
	IDirect3DTexture8 second;
	texture.Apply_New_Surface(&first, false, false);
	result |= check(texture.D3DTexture == &first && !texture.Initialized &&
		texture.Width == 8 && texture.Height == 4 && texture.InactivationTime == 100,
		"thumbnail publication preserves deferred initialization and invalidation");
	texture.Apply_New_Surface(&second, true, true);
	result |= check(texture.D3DTexture == &second && texture.Initialized &&
		texture.Width == 256 && texture.Height == 128 && texture.TextureFormat == 21 &&
		texture.InactivationTime == 0 && texture.LastAccessed == 42,
		"valid publication commits the surface and initialized metadata");
	result |= check(first.references == 1 && second.references == 2 &&
		first.surface.references == 1 && second.surface.references == 1,
		"successful replacement balances texture and temporary surface references");
	texture.Apply_New_Surface(nullptr, true, true);
	result |= check(texture.D3DTexture == nullptr && !texture.Initialized &&
		second.references == 1,
		"null publication retains the existing explicit-clear contract");
	return result;
}

#if defined(_WIN64)
int testNativeTexturePublicationLifecycle()
{
	int result = 0;
	IDirect3DTexture8 incoming;

	TextureClass cleared;
	cleared.NativeTexturePresent = true;
	cleared.Initialized = true;
	cleared.Apply_New_Surface(nullptr, true, true);
	result |= check(cleared.NativeReleaseCalls == 1 &&
		cleared.NativeMissingCalls == 0 && cleared.NativeReleaseOrder == 1 &&
		cleared.InitializedAtNativeRelease &&
		!cleared.NativeTexturePresent && !cleared.Initialized,
		"x64 null publication retires native ownership before clearing initialization");
	result |= check(cleared.D3DTexture == nullptr,
		"x64 null publication never creates or retains a D3D8 texture");

	TextureClass published;
	published.Apply_New_Surface(&incoming, true, true);
	result |= check(published.NativeReleaseCalls == 0 &&
		published.NativeMissingCalls == 1 && published.NativeMissingOrder == 1 &&
		!published.InitializedAtNativeMissing &&
		published.NativeTexturePresent && published.Initialized,
		"x64 residual raw publication deterministically publishes the native missing texture");
	result |= check(published.D3DTexture == nullptr && incoming.references == 1 &&
		incoming.surface.references == 1 && incoming.surface.descCalls == 0 &&
		published.InactivationTime == 100,
		"x64 fallback neither retains nor queries the raw candidate and preserves invalidation policy");

	TextureClass deferred;
	deferred.Apply_New_Surface(&incoming, false, true);
	result |= check(deferred.NativeMissingCalls == 1 &&
		deferred.NativeMissingOrder == 1 &&
		!deferred.InitializedAtNativeMissing && deferred.NativeTexturePresent &&
		!deferred.Initialized,
		"x64 deferred publication installs deterministic fallback content before leaving initialization pending");

	TextureClass failed;
	failed.NativeTexturePresent = true;
	failed.Initialized = true;
	failed.NativeMissingResult = false;
	failed.Apply_New_Surface(&incoming, true, true);
	result |= check(failed.NativeMissingCalls == 1 &&
		failed.NativeReleaseCalls == 0 && failed.InitializedAtNativeMissing &&
		failed.NativeTexturePresent && !failed.Initialized &&
		failed.D3DTexture == nullptr,
		"x64 fallback failure preserves prior native ownership but fails initialization closed");
	return result;
}
#endif
}

int TestLegacyResetResources()
{
	int result = 0;
	for (int backend = 0; backend != 2; ++backend)
	{
		for (int failures = 0; failures != 4; ++failures)
			result |= testReset(backend != 0, (failures & 1) != 0,
				(failures & 2) != 0, false, true);
		result |= testReset(backend != 0, true, true, true, true);
		result |= testReset(backend != 0, false, false, false, false);
	}
	result |= testResetPreflight();
#if defined(_WIN64)
	result |= testNativeTexturePublicationLifecycle();
#else
	for (int failure = 0; failure != 4; ++failure)
	{
		result |= testTextureFailure(failure, false);
		result |= testTextureFailure(failure, true);
	}
	result |= testTextureSuccess();
#endif
	return result;
}
