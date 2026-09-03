/*
** Render-owner capability portion of the native GameEngineDevice seam.
**
** The full command implementation is title-owned and is added at the native
** owner handoff.  This translation unit is intentionally limited to the
** target-kind publication/query contract so the native query cannot infer
** render-to-texture from a requested target or backend selection.
*/

#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/RenderTexturePublication.h"

#include <atomic>
#include <mutex>

// Native surfaces publish their retained CPU image during Unlock. There is no
// compatibility cache to invalidate after that transaction; republishing here
// would duplicate each procedural upload and could overwrite GPU-owned data.
void Notify_Render_Texture_Changed(TextureClass *texture)
{
	(void)texture;
}

bool Is_Render_D3D11_Backend_Active()
{
	return rts::render::IsNativeGameRendererActive();
}

bool Is_Render_Texture_Publication_Operational()
{
	return rts::render::IsNativeGameRendererActive();
}

namespace
{
rts::render::GameDebugRenderStats g_debug_render_stats;
std::atomic<rts::render::IGameRenderClientNativeOwner *> g_native_owner_atomic(0);
std::recursive_mutex g_native_owner_mutex;
thread_local unsigned int g_native_owner_pin_depth = 0;
thread_local unsigned int g_native_owner_lifecycle_depth = 0;

// Map only logical formats with a proven native upload representation.  The
// result is still checked by the owner against the active device/resource
// implementation; this table is not a replacement for that query.  Formats
// whose native loader path remains explicitly legacy (palette/luminance-bump
// layouts) fail closed instead of being accepted because a conversion helper
// happens to exist elsewhere.
bool TryMapGameTextureFormat(WW3DFormat logicalFormat,
	rts::render::RenderFormat *nativeFormat)
{
	if (nativeFormat == 0)
	{
		return false;
	}
	*nativeFormat = rts::render::RENDER_FORMAT_UNKNOWN;
	switch (logicalFormat)
	{
	case WW3D_FORMAT_R8G8B8:
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R5G6B5:
	case WW3D_FORMAT_X1R5G5B5:
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_R3G3B2:
	case WW3D_FORMAT_A8:
	case WW3D_FORMAT_A8R3G3B2:
	case WW3D_FORMAT_X4R4G4B4:
	case WW3D_FORMAT_L8:
	case WW3D_FORMAT_A8L8:
	case WW3D_FORMAT_A4L4:
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		*nativeFormat = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		return true;
	case WW3D_FORMAT_U8V8:
		*nativeFormat = rts::render::RENDER_FORMAT_R8G8_SNORM;
		return true;
	default:
		return false;
	}
}
}

namespace rts
{
namespace render
{

IGameRenderClientNativeOwner *GetGameRenderClientNativeOwner()
{
	return g_native_owner_atomic.load(std::memory_order_acquire);
}

bool IsNativeGameRenderOwnerPinnedByCurrentThread()
{
	return g_native_owner_pin_depth != 0 ||
		g_native_owner_lifecycle_depth != 0;
}

void SetGameRenderClientNativeOwner(IGameRenderClientNativeOwner *owner)
{
	NativeGameRenderOwnerLifecycleScope scope;
	scope.Publish(owner);
}

NativeGameRenderOwnerScope::NativeGameRenderOwnerScope() :
	m_owner(0), m_locked(false)
{
	g_native_owner_mutex.lock();
	m_owner = g_native_owner_atomic.load(std::memory_order_acquire);
	++g_native_owner_pin_depth;
	m_locked = true;
}

NativeGameRenderOwnerScope::~NativeGameRenderOwnerScope()
{
	if (m_locked)
	{
		m_locked = false;
		--g_native_owner_pin_depth;
		g_native_owner_mutex.unlock();
	}
}

IGameRenderClientNativeOwner *NativeGameRenderOwnerScope::Get() const
{
	return m_owner;
}

NativeGameRenderOwnerLifecycleScope::NativeGameRenderOwnerLifecycleScope() :
	m_locked(false)
{
	// A recursive command query is safe, but a recursive lifecycle transition
	// could destroy the aggregate that the outer command is still using.
	if (IsNativeGameRenderOwnerPinnedByCurrentThread())
		return;
	g_native_owner_mutex.lock();
	++g_native_owner_lifecycle_depth;
	m_locked = true;
}

NativeGameRenderOwnerLifecycleScope::~NativeGameRenderOwnerLifecycleScope()
{
	if (m_locked)
	{
		m_locked = false;
		--g_native_owner_lifecycle_depth;
		g_native_owner_mutex.unlock();
	}
}

bool NativeGameRenderOwnerLifecycleScope::IsAcquired() const
{
	return m_locked;
}

IGameRenderClientNativeOwner *NativeGameRenderOwnerLifecycleScope::Get() const
{
	return m_locked ? g_native_owner_atomic.load(std::memory_order_acquire) : 0;
}

void NativeGameRenderOwnerLifecycleScope::Publish(
	IGameRenderClientNativeOwner *owner)
{
	if (m_locked && g_native_owner_pin_depth == 0)
	{
		g_native_owner_atomic.store(owner, std::memory_order_release);
	}
}

bool IsGameRenderingToTexture()
{
	NativeGameRenderOwnerScope scope;
	return IsGameRenderingToTextureForOwner(scope.Get());
}

bool IsNativeGameRendererActive()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsInitialized() && owner->IsOperational();
}

bool IsGameRendererInitialized()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsInitialized();
}

bool IsGameRenderTargetOperational()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsOperational();
}

bool GameRendererSupportsPointSprites()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsOperational() && owner->SupportsPointSprites();
}

bool GameRendererSupportsDot3()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsOperational() && owner->SupportsDot3();
}

bool GameRendererSupportsZBias()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsOperational() && owner->SupportsZBias();
}

bool GameRendererSupportsStencil()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsOperational() && owner->SupportsStencil();
}

bool GameRendererSupportsNPatches()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsOperational() && owner->SupportsNPatches();
}

bool IsGameTextureFormatSupported(WW3DFormat format)
{
	RenderFormat nativeFormat = RENDER_FORMAT_UNKNOWN;
	if (!TryMapGameTextureFormat(format, &nativeFormat))
	{
		return false;
	}
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return owner != 0 && owner->IsInitialized() && owner->IsOperational() &&
		owner->IsGameTextureFormatSupported(nativeFormat);
}

RenderResult SetGameFogState(const LegacyFogConstants &fog)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (owner == 0 || !owner->IsInitialized() || !owner->IsOperational())
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result = owner->SetGameFogState(fog);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

RenderResult SetGameLightState(unsigned int index,
	const LegacyLightState &light)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (owner == 0 || !owner->IsInitialized() || !owner->IsOperational())
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result = owner->SetGameLightState(index, light);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

bool IsGameTerrainRenderingDisabled()
{
	return g_debug_render_stats.disableTerrain;
}

bool IsGameObjectRenderingDisabled()
{
	return g_debug_render_stats.disableObjects;
}

bool IsGameDebugConsoleDisabled()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return g_debug_render_stats.disableConsole ||
		(owner != 0 && owner->IsDebugConsoleDisabled());
}

RenderResult GetGameBackBufferInfo(RenderBackBufferInfo *info)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (info == 0)
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (owner == 0 || !owner->IsInitialized() || !owner->IsOperational())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return owner->GetGameBackBufferInfo(info);
}

WW3DFormat GetGameBackBufferFormat()
{
	RenderBackBufferInfo info;
	if (GetGameBackBufferInfo(&info) != RENDER_RESULT_OK)
		return WW3D_FORMAT_UNKNOWN;
	switch (info.format)
	{
	case RENDER_FORMAT_R8G8B8A8_UNORM:
	case RENDER_FORMAT_B8G8R8A8_UNORM:
		return WW3D_FORMAT_A8R8G8B8;
	default:
		return WW3D_FORMAT_UNKNOWN;
	}
}

const GameDebugRenderStats &GetGameDebugRenderStats()
{
	return g_debug_render_stats;
}

GameDebugRenderStats &GetMutableGameDebugRenderStats()
{
	return g_debug_render_stats;
}

void SetGameDebugRenderStats(const GameDebugRenderStats &stats)
{
	g_debug_render_stats = stats;
}

void ReleaseGameSnowVertexBuffer(void *opaque)
{
	// Native snow streams are published as renderer-owned resources.  The
	// legacy opaque pointer is never populated on x64; keep this hook harmless
	// if an old caller releases an empty slot during teardown.
	(void)opaque;
}

void BeginGameDisplayIteration()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (owner != 0)
	{
		const RenderResult result = owner->BeginGameDisplayIteration();
		if (result != RENDER_RESULT_OK)
			owner->RecordGameFailure(result);
	}
}

RenderResult ResetGameRenderFrameResources(bool frameChanged)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (owner == 0 || !owner->IsInitialized() || !owner->IsOperational())
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result =
		owner->ResetGameRenderFrameResources(frameChanged);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

RenderResult GetGameTextureFilterCapabilities(
	GameTextureFilterCapabilities *capabilities)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (capabilities == 0)
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (owner == 0 || !owner->IsInitialized() || !owner->IsOperational())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return owner->GetTextureFilterCapabilities(capabilities);
}

unsigned int GetGameMaxTexturesPerPass()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (owner == 0 || !owner->IsInitialized() || !owner->IsOperational())
		return 0;
	const unsigned int reported = owner->GetMaxTexturesPerPass();
	return reported > LEGACY_TEXTURE_STAGE_COUNT ?
		LEGACY_TEXTURE_STAGE_COUNT : reported;
}

}
}
