#include "Utility/CppMacros.h"
#include "Renderer/RendererDevice.h"
#include "Renderer/RenderTexturePublication.h"
#include "W3DDevice/GameClient/W3DVideoBuffer.h"
#if defined(RTS_RENDERER_HAS_D3D11) && !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
#include "d3d11legacybridge.h"
#endif
#include "Renderer/LegacyBridgeValidation.h"
#include "Renderer/LegacyRenderState.h"
#if !defined(RTS_RENDERER_HAS_D3D11)
#include "Renderer/NativeW3DRenderer.h"
#endif

#include <stdio.h>
#include <algorithm>
#include <math.h>
#include <string>
#include <string.h>
#include <vector>

#if defined(RTS_RENDERER_HAS_D3D11) && !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
#include <type_traits>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

// The seed operation is added by the renderer implementation below. Keep the
// red test buildable before that implementation exists.
namespace rts { namespace render {
void SeedTrackedLegacyPipelineState();
} }

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

bool nearlyEqual(float left, float right, float epsilon = 0.00001f)
{
	return fabs(left - right) <= epsilon;
}

bool ExpectedNativeVideoPublicationSelection(
	rts::render::RenderBackend backend, bool backendSupported, bool ownerActive)
{
#if defined(_WIN64)
	return backend == rts::render::RENDER_BACKEND_D3D11 &&
		backendSupported && ownerActive;
#else
	(void)backend;
	(void)backendSupported;
	(void)ownerActive;
	return false;
#endif
}

bool ReadSourceText(const char *relativePath, std::string *contents)
{
	if (relativePath == 0 || contents == 0)
	{
		return false;
	}
	std::string path = RTS_SOURCE_ROOT;
	path += "/";
	path += relativePath;
	FILE *input = fopen(path.c_str(), "rb");
	if (input == 0 || fseek(input, 0, SEEK_END) != 0)
	{
		if (input != 0)
		{
			fclose(input);
		}
		return false;
	}
	const long byteCount = ftell(input);
	if (byteCount <= 0 || fseek(input, 0, SEEK_SET) != 0)
	{
		fclose(input);
		return false;
	}
	std::vector<char> bytes(static_cast<size_t>(byteCount));
	const size_t bytesRead = fread(&bytes[0], 1,
		static_cast<size_t>(byteCount), input);
	fclose(input);
	if (bytesRead != static_cast<size_t>(byteCount))
	{
		return false;
	}
	contents->assign(&bytes[0], bytesRead);
	return true;
}

unsigned CountSourceOccurrences(const std::string &source, const char *needle)
{
	if (needle == 0 || *needle == '\0')
	{
		return 0;
	}
	unsigned count = 0;
	for (std::string::size_type position = source.find(needle);
		position != std::string::npos;
		position = source.find(needle, position + 1))
	{
		++count;
	}
	return count;
}

int testBackendNames()
{
	int result = 0;
	result |= check(rts::render::RequestedRenderBackend() ==
		rts::render::DefaultRenderBackend(),
		"the requested renderer starts at the platform default");
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	result |= check(rts::render::DefaultRenderBackend() ==
		rts::render::RENDER_BACKEND_D3D11 &&
		rts::render::IsRenderBackendSupported(
			rts::render::RENDER_BACKEND_D3D11) &&
		!rts::render::IsRenderBackendSupported(
			rts::render::RENDER_BACKEND_DX8),
		"native x64 defaults to D3D11 and rejects the DX8 command-line backend");
#else
	result |= check(rts::render::DefaultRenderBackend() ==
		rts::render::RENDER_BACKEND_DX8 &&
		rts::render::IsRenderBackendSupported(
			rts::render::RENDER_BACKEND_DX8),
		"legacy Win32 retains the DX8 renderer default");
#endif
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
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	result |= check(rts::render::RequestedRenderBackend() ==
		rts::render::RENDER_BACKEND_DX8 &&
		!rts::render::IsRenderBackendSupported(
			rts::render::RequestedRenderBackend()),
		"unsupported native command-line selection remains observable for fail-closed startup");
#endif
	rts::render::SetRequestedRenderBackend(rts::render::DefaultRenderBackend());
	return result;
}

int testRenderTexturePublicationOperationalStates()
{
	int result = 0;
	result |= check(
		rts::render::IsRenderTexturePublicationOperationalState(
			true, false, false, false),
		"legacy texture publication is operational before a scene bracket");
	result |= check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			true, true, false, false),
		"legacy texture publication is suppressed while lost or resetting");
	result |= check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			false, false, false, false),
		"texture publication is suppressed after renderer shutdown");
	result |= check(
		rts::render::IsRenderTexturePublicationOperationalState(
			true, false, true, true),
		"native texture publication is operational after bridge recovery");
	result |= check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			true, false, true, false),
		"native texture publication is suppressed while the bridge is lost");
	return result;
}

#if !defined(RTS_RENDERER_HAS_D3D11)
int testNativeRendererRejectsUnavailableBackend()
{
	rts::render::NativeW3DRenderer renderer;
	rts::render::NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	return check(renderer.Initialize(&renderer, descriptor) ==
		rts::render::RENDER_RESULT_UNSUPPORTED,
		"native renderer reports unsupported when its backend is unavailable");
}
#endif

int testRendererTextureLifecycleContracts()
{
	int result = 0;
	std::string surfaceHeader;
	std::string surfaceSource;
	std::string legacySurfaceSource;
	std::string textureSource;
	std::string legacyTextureSource;
	std::string waterSource;
	std::string videoHeader;
	std::string videoSource;
	std::string videoPlayerHeader;
	std::string ffmpegSource;
	std::string generalsShroudSource;
	std::string generalsMDShroudSource;
	result |= check(ReadSourceText(
		"Core/Libraries/Source/WWVegas/WW3D2/surfaceclass.h",
		&surfaceHeader), "surface lock contract source is available");
	result |= check(ReadSourceText(
		"Core/Libraries/Source/WWVegas/WW3D2/surfaceclass.cpp",
		&surfaceSource), "surface lock implementation source is available");
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	result |= check(ReadSourceText(
		"Core/LegacyRenderer/WWVegas/WW3D2/surfaceclass_legacy.cpp",
		&legacySurfaceSource), "legacy surface lock implementation source is available");
#endif
	result |= check(ReadSourceText(
		"Core/Libraries/Source/WWVegas/WW3D2/texture.cpp",
		&textureSource), "texture construction source is available");
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	result |= check(ReadSourceText(
		"Core/LegacyRenderer/WWVegas/WW3D2/texture_legacy.cpp",
		&legacyTextureSource), "legacy texture construction source is available");
#endif
	result |= check(ReadSourceText(
		"Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp",
		&waterSource), "water upload source is available");
	result |= check(ReadSourceText(
		"Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DVideoBuffer.h",
		&videoHeader), "video buffer state header is available");
	result |= check(ReadSourceText(
		"Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DVideoBuffer.cpp",
		&videoSource), "video buffer publication source is available");
	result |= check(ReadSourceText(
		"Core/GameEngine/Include/GameClient/VideoPlayer.h",
		&videoPlayerHeader), "video player interface source is available");
	result |= check(ReadSourceText(
		"Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp",
		&ffmpegSource), "FFmpeg video source is available");
	result |= check(ReadSourceText(
		"Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp",
		&generalsShroudSource), "Generals shroud source is available");
	result |= check(ReadSourceText(
		"GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp",
		&generalsMDShroudSource), "Zero Hour shroud source is available");

	const std::string::size_type bumpMapMethod = waterSource.find(
		"HRESULT WaterRenderObjClass::initBumpMap");
	const std::string::size_type bumpMapNativeGuard = waterSource.rfind(
		"#if defined(_WIN64)", bumpMapMethod);
	const std::string::size_type bumpMapLegacyBranch = waterSource.find(
		"#else", bumpMapNativeGuard);
	const std::string::size_type bumpMapEnd = waterSource.find(
		"HRESULT WaterRenderObjClass::generateVertexBuffer",
		bumpMapLegacyBranch);
	result |= check(bumpMapMethod != std::string::npos &&
		bumpMapNativeGuard != std::string::npos &&
		bumpMapLegacyBranch != std::string::npos &&
		bumpMapLegacyBranch > bumpMapNativeGuard &&
		bumpMapEnd != std::string::npos && bumpMapEnd > bumpMapLegacyBranch,
		"water bump upload keeps native and compatibility branches bounded");
	std::string nativeBumpMapSource;
	std::string legacyBumpMapSource;
	if (bumpMapNativeGuard != std::string::npos &&
		bumpMapLegacyBranch != std::string::npos &&
		bumpMapLegacyBranch > bumpMapNativeGuard)
	{
		nativeBumpMapSource = waterSource.substr(bumpMapNativeGuard,
			bumpMapLegacyBranch - bumpMapNativeGuard);
	}
	if (bumpMapLegacyBranch != std::string::npos &&
		bumpMapEnd != std::string::npos && bumpMapEnd > bumpMapLegacyBranch)
	{
		legacyBumpMapSource = waterSource.substr(bumpMapLegacyBranch,
			bumpMapEnd - bumpMapLegacyBranch);
	}

#if defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	const std::string::size_type readOnlyDeclaration =
		surfaceHeader.find("void Unlock_Read_Only();");
	const std::string::size_type readOnlyMethod = surfaceSource.find(
		"void SurfaceClass::Unlock_Read_Only()");
	const std::string::size_type readOnlyMethodEnd = surfaceSource.find(
		"bool SurfaceClass::Acquire_Native_Surface", readOnlyMethod);
	result |= check(readOnlyDeclaration != std::string::npos &&
		readOnlyMethod != std::string::npos &&
		readOnlyMethodEnd != std::string::npos &&
		readOnlyMethodEnd > readOnlyMethod,
		"surface exposes an explicit read-only lock completion boundary");
	if (readOnlyMethod != std::string::npos &&
		readOnlyMethodEnd != std::string::npos &&
		readOnlyMethodEnd > readOnlyMethod)
	{
		const std::string readOnlyBody = surfaceSource.substr(readOnlyMethod,
			readOnlyMethodEnd - readOnlyMethod);
		result |= check(readOnlyBody.find("NativeSurface->locked = false") !=
			std::string::npos && readOnlyBody.find("Publish_Native_Surface") ==
			std::string::npos,
			"native read-only completion releases the lock without republishing");
	}

	result |= check(textureSource.find(
		"TextureClass *TextureClass::Create_Native_From_Prepared") !=
			std::string::npos &&
		nativeBumpMapSource.find("TextureClass::Create_Native_From_Prepared") !=
			std::string::npos,
		"water publishes prepared mips through the direct native texture factory");
	result |= check(nativeBumpMapSource.find("Unlock_Read_Only()") !=
		std::string::npos && nativeBumpMapSource.find("NEW_REF(TextureClass") ==
			std::string::npos,
		"water source reads do not republish or allocate an empty native texture");
#else
	const std::string::size_type legacyReadOnlyMethod =
		legacySurfaceSource.find("void SurfaceClass::Unlock_Read_Only()");
	const std::string::size_type legacyReadOnlyMethodEnd =
		legacySurfaceSource.find("bool SurfaceClass::Acquire_Native_Surface",
			legacyReadOnlyMethod);
	result |= check(legacyReadOnlyMethod != std::string::npos &&
		legacyReadOnlyMethodEnd != std::string::npos &&
		legacyReadOnlyMethodEnd > legacyReadOnlyMethod,
		"legacy surface exposes a bounded read-only lock completion boundary");
	if (legacyReadOnlyMethod != std::string::npos &&
		legacyReadOnlyMethodEnd != std::string::npos &&
		legacyReadOnlyMethodEnd > legacyReadOnlyMethod)
	{
		const std::string legacyReadOnlyBody = legacySurfaceSource.substr(
			legacyReadOnlyMethod, legacyReadOnlyMethodEnd - legacyReadOnlyMethod);
		result |= check(legacyReadOnlyBody.find("Unlock();") !=
			std::string::npos,
			"legacy read-only completion closes the compatibility surface lock");
	}

	result |= check(legacyBumpMapSource.find(
		"TextureClass::Create_Native_From_Prepared") == std::string::npos &&
		legacyBumpMapSource.find("destinationSurface->Unlock();") !=
			std::string::npos,
		"legacy water publishes destination mips through the compatibility surface path");
	result |= check(legacyBumpMapSource.find("Unlock_Read_Only()") !=
		std::string::npos && legacyBumpMapSource.find("NEW_REF(TextureClass") !=
			std::string::npos,
		"legacy water reads source mips while owning a compatibility destination texture");
#endif
	const std::string::size_type whiteUpdate = waterSource.find(
		"Bool WaterRenderObjClass::updateWhiteTexture()");
	const std::string::size_type whiteUpdateEnd = waterSource.find(
		"void WaterRenderObjClass::ReAcquireResources()", whiteUpdate);
	result |= check(whiteUpdate != std::string::npos &&
		whiteUpdateEnd > whiteUpdate,
		"water white-texture writes use one bounded completion helper");
	if (whiteUpdate != std::string::npos && whiteUpdateEnd > whiteUpdate)
	{
		const std::string whiteUpdateBody = waterSource.substr(whiteUpdate,
			whiteUpdateEnd - whiteUpdate);
		result |= check(whiteUpdateBody.find(
			"surface->Unlock_Native_Surface()") != std::string::npos &&
			whiteUpdateBody.find("m_whiteTexturePublishPending=TRUE") !=
			std::string::npos && whiteUpdateBody.find(
			"m_whiteTexturePublishPending=publicationSucceeded ? FALSE : TRUE") !=
			std::string::npos,
			"native white-texture publication retains a retryable failure state");
#if defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
		// The native branch is checked above; the source also retains the
		// compatibility branch for the shared game-client implementation.
#else
		result |= check(whiteUpdateBody.find("surface->Unlock();") !=
			std::string::npos && whiteUpdateBody.find(
			"rts::render::NotifyTextureChanged(m_whiteTexture)") !=
			std::string::npos,
			"legacy white-texture publication keeps Unlock and notification behavior");
#endif
	}
	unsigned whiteUpdateCallCount = 0;
	for (std::string::size_type call = waterSource.find(
		"updateWhiteTexture();"); call != std::string::npos; call =
		waterSource.find("updateWhiteTexture();", call + 1))
	{
		++whiteUpdateCallCount;
	}
	result |= check(whiteUpdateCallCount == 3,
		"water reset, initialization, and shader paths retry white publication");

	// The Win32/VC6 procedural constructors must report the result of the
	// actual compatibility allocation.  Keep this as a source contract because
	// the device-free renderer test cannot create a real legacy device, while
	// the VC6 oracle compile proves the guarded implementation remains valid.
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	const char *legacyCreationMarkers[] = {
		"Poke_Texture(DX8Wrapper::_Create_DX8_Texture\n",
		"Poke_Texture(DX8Wrapper::_Create_DX8_Texture(\n\t\tsurface->",
		"Poke_Texture(DX8Wrapper::_Create_DX8_ZTexture\n",
		"Poke_Texture(DX8Wrapper::_Create_DX8_Cube_Texture\n",
		"Poke_Texture(DX8Wrapper::_Create_DX8_Volume_Texture\n"
	};
	const unsigned legacyCreationMarkerCount =
		static_cast<unsigned>(sizeof(legacyCreationMarkers) /
			sizeof(legacyCreationMarkers[0]));
	for (unsigned i = 0; i != legacyCreationMarkerCount; ++i)
	{
		const std::string::size_type creation = legacyTextureSource.find(
			legacyCreationMarkers[i]);
		const std::string::size_type branchEnd = legacyTextureSource.find(
			"#endif", creation);
		bool derivesInitialization = false;
		if (creation != std::string::npos && branchEnd > creation)
		{
			const std::string legacyBranch = legacyTextureSource.substr(
				creation, branchEnd - creation);
			derivesInitialization = legacyBranch.find(
				"Initialized = Peek_D3D_Base_Texture() != nullptr;") !=
				std::string::npos;
		}
		result |= check(derivesInitialization,
			"legacy procedural allocation failure leaves initialization false");
	}
#endif

#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	std::string wrapperSource;
	result |= check(ReadSourceText(
		"Core/LegacyRenderer/WWVegas/WW3D2/dx8wrapper.cpp",
		&wrapperSource), "renderer wrapper source is available");
	const std::string::size_type init = wrapperSource.find(
		"bool DX8Wrapper::Init(void * hwnd, bool lite)");
	const std::string::size_type shutdown = wrapperSource.find(
		"void DX8Wrapper::Shutdown()", init);
	const std::string::size_type reset = wrapperSource.find(
		"bool DX8Wrapper::Reset_Device(", shutdown);
	const std::string::size_type resetEnd = wrapperSource.find(
		"void DX8Wrapper::Release_DX8_Buffer_Bindings()", reset);
	result |= check(init != std::string::npos && shutdown > init &&
		reset > shutdown && resetEnd > reset,
		"renderer lifecycle methods remain source-addressable");
	if (init != std::string::npos && shutdown > init &&
		reset > shutdown && resetEnd > reset)
	{
		result |= check(wrapperSource.find("IsDeviceLost = false;", init) < shutdown,
			"renderer initialization clears stale lost state");
		result |= check(wrapperSource.find("IsDeviceLost = false;", shutdown) < reset,
			"renderer shutdown clears stale lost state");
		result |= check(wrapperSource.find("IsDeviceLost = false;", reset) < resetEnd,
			"successful legacy reset clears stale lost state");
	}

	const std::string::size_type setRenderDevice = wrapperSource.find(
		"bool DX8Wrapper::Set_Render_Device(int dev", resetEnd);
	const std::string::size_type setRenderDeviceEnd = wrapperSource.find(
		"bool DX8Wrapper::Set_Device_Resolution(", setRenderDevice);
	const std::string::size_type setDeviceResolution = setRenderDeviceEnd;
	const std::string::size_type setDeviceResolutionEnd = wrapperSource.find(
		"void DX8Wrapper::Get_Device_Resolution(", setDeviceResolution);
	const std::string::size_type toggleWindowed = wrapperSource.find(
		"bool DX8Wrapper::Toggle_Windowed()", setRenderDevice);
	const std::string::size_type toggleWindowedEnd = wrapperSource.find(
		"void DX8Wrapper::Set_Swap_Interval(", toggleWindowed);
	const std::string::size_type shutdownEnd = wrapperSource.find(
		"bool DX8Wrapper::Do_Onetime_Device_Dependent_Inits()", shutdown);
	result |= check(init != std::string::npos && shutdown > init &&
		wrapperSource.find("rts::render::GameRenderer_IsWindowed = false;", init) <
			shutdown && wrapperSource.find(
			"IsWindowed = rts::render::GameRenderer_IsWindowed;", init) <
			shutdown,
		"legacy initialization synchronizes neutral window state and ABI mirror");
	if (setRenderDevice != std::string::npos &&
		setRenderDeviceEnd > setRenderDevice)
	{
		const std::string setRenderBody = wrapperSource.substr(setRenderDevice,
			setRenderDeviceEnd - setRenderDevice);
		result |= check(setRenderBody.find(
			"rts::render::GameRenderer_IsWindowed = (windowed != 0);") !=
			std::string::npos && setRenderBody.find(
			"IsWindowed = rts::render::GameRenderer_IsWindowed;") !=
			std::string::npos && setRenderBody.find(
			"rts::render::GameRenderer_IsWindowed = previous_windowed;") !=
			std::string::npos,
			"legacy device selection updates and rolls back one canonical window state");
	}
	else
	{
		result |= check(false,
			"legacy device-selection synchronization boundary is source-addressable");
	}
	if (setDeviceResolution != std::string::npos &&
		setDeviceResolutionEnd > setDeviceResolution)
	{
		const std::string setResolutionBody = wrapperSource.substr(
			setDeviceResolution, setDeviceResolutionEnd - setDeviceResolution);
		result |= check(setResolutionBody.find(
			"const bool previous_windowed = rts::render::GameRenderer_IsWindowed;") !=
			std::string::npos && setResolutionBody.find(
			"const bool requested_windowed = windowed == -1 ?") !=
			std::string::npos && setResolutionBody.find(
			"rts::render::GameRenderer_IsWindowed = windowed != 0;") !=
			std::string::npos && setResolutionBody.find(
			"IsWindowed = rts::render::GameRenderer_IsWindowed;") !=
			std::string::npos && setResolutionBody.find(
			"rts::render::GameRenderer_IsWindowed = previous_windowed;") !=
			std::string::npos,
			"legacy resolution changes and rollback use the canonical window state");
	}
	else
	{
		result |= check(false,
			"legacy resolution synchronization boundary is source-addressable");
	}
	if (toggleWindowed != std::string::npos &&
		toggleWindowedEnd > toggleWindowed)
	{
		const std::string toggleBody = wrapperSource.substr(toggleWindowed,
			toggleWindowedEnd - toggleWindowed);
		result |= check(toggleBody.find(
			"!rts::render::GameRenderer_IsWindowed") != std::string::npos,
			"legacy window toggle reads the canonical neutral state");
	}
	else
	{
		result |= check(false,
			"legacy window-toggle synchronization boundary is source-addressable");
	}
	result |= check(shutdownEnd > shutdown && wrapperSource.find(
		"IsWindowed = rts::render::GameRenderer_IsWindowed;", shutdown) <
			shutdownEnd,
		"legacy shutdown reconciles the retained ABI window-state mirror");
#endif

	const std::string::size_type updateBegin = textureSource.find(
		"bool TextureBaseClass::Update_Native_Subresource_Data");
	const std::string::size_type updateEnd = textureSource.find(
		"size_t TextureBaseClass::Get_Native_Texture_Byte_Count", updateBegin);
	result |= check(updateBegin != std::string::npos && updateEnd > updateBegin,
		"native texture update source is bounded for the in-place contract");
	if (updateBegin != std::string::npos && updateEnd > updateBegin)
	{
		const std::string updateBody = textureSource.substr(updateBegin,
			updateEnd - updateBegin);
		result |= check(updateBody.find("RefreshCpuContent") !=
			std::string::npos && updateBody.find("Apply_Native_Texture") ==
			std::string::npos,
			"steady-state texture updates refresh the existing native resource");
	}
	result |= check(surfaceHeader.find("bool Unlock_Native_Surface();") !=
		std::string::npos && surfaceSource.find(
		"bool SurfaceClass::Unlock_Native_Surface()") != std::string::npos,
		"native surface publication exposes a status-returning unlock beside legacy ABI");
	result |= check(surfaceHeader.find("bool Copy_Native_No_Publish(") !=
		std::string::npos && surfaceHeader.find(
		"bool Publish_Native_Changes();") != std::string::npos &&
		surfaceSource.find("bool SurfaceClass::Copy_Native_No_Publish(") !=
		std::string::npos && surfaceSource.find(
		"bool SurfaceClass::Publish_Native_Changes()") != std::string::npos,
		"native surfaces expose a separate batch-copy and publication boundary");
	const std::string::size_type noPublishBegin = surfaceSource.find(
		"bool SurfaceClass::Copy_Native_No_Publish(");
	const std::string::size_type publishChangesBegin = surfaceSource.find(
		"bool SurfaceClass::Publish_Native_Changes()", noPublishBegin);
	if (noPublishBegin != std::string::npos &&
		publishChangesBegin > noPublishBegin)
	{
		const std::string noPublishBody = surfaceSource.substr(noPublishBegin,
			publishChangesBegin - noPublishBegin);
		result |= check(noPublishBody.find("Publish_Native_Surface") ==
			std::string::npos,
			"native batch copies mutate the CPU shadow without publishing");
	}
	if (publishChangesBegin != std::string::npos)
	{
		const std::string publishBody = surfaceSource.substr(publishChangesBegin);
		result |= check(publishBody.find("Publish_Native_Surface") !=
			std::string::npos,
			"native batch completion retains a retryable publication boundary");
	}
	result |= check(videoHeader.find("m_nativePublicationPending") !=
		std::string::npos && videoSource.find("Unlock_Native_Surface()") !=
		std::string::npos && videoSource.find(
		"m_nativePublicationPending = publicationSucceeded ? FALSE : TRUE") !=
		std::string::npos && videoSource.find(
		"!m_nativePublicationPending") != std::string::npos,
		"video retains and visibly reports failed native publication for retry");
	result |= check(videoPlayerHeader.find("publishLockedFrame") ==
		std::string::npos && ffmpegSource.find("publishLockedFrame") ==
		std::string::npos,
		"FFmpeg uses the single VideoBuffer unlock publication boundary");
	const char *shroudSources[] = {
		generalsShroudSource.c_str(), generalsMDShroudSource.c_str()
	};
	for (unsigned int shroudIndex = 0; shroudIndex < 2; ++shroudIndex)
	{
		const std::string shroud(shroudSources[shroudIndex]);
		const std::string::size_type borderBegin = shroud.find(
			"Bool W3DShroud::fillBorderShroudData(");
		const std::string::size_type borderEnd = shroud.find(
			"/**Set the shroud color within the border area", borderBegin);
		const std::string::size_type syncUnlock = shroud.find(
			"m_pSrcTexture->Unlock_Native_Surface()");
		const std::string::size_type syncDirty = shroud.find(
			"m_srcTextureDirty=FALSE;", syncUnlock);
		const std::string::size_type borderCall = shroud.find(
			"fillBorderShroudData(m_boderShroudLevel, pDestSurface)");
		const std::string::size_type borderClear = shroud.find(
			"m_clearDstTexture=FALSE;", borderCall);
		result |= check(syncUnlock != std::string::npos &&
			syncDirty > syncUnlock && shroud.find("Copy_Native") !=
			std::string::npos && borderCall != std::string::npos &&
			borderClear > borderCall && shroud.find(
			"m_clearDstTexture = TRUE;") != std::string::npos,
			"both shroud copies retain dirty state until native publication succeeds");
		if (borderBegin != std::string::npos && borderEnd > borderBegin)
		{
			const std::string borderBody = shroud.substr(borderBegin,
				borderEnd - borderBegin);
			const std::string::size_type rowLoop = borderBody.find(
				"for (y=0; y<m_dstTextureHeight; y++)");
			const std::string::size_type lastBatchCopy = borderBody.rfind(
				"Copy_Native_No_Publish(");
			const std::string::size_type batchPublish = borderBody.find(
				"Publish_Native_Changes()");
			result |= check(rowLoop != std::string::npos &&
				lastBatchCopy != std::string::npos && batchPublish > lastBatchCopy &&
				CountSourceOccurrences(borderBody, "Publish_Native_Changes()") == 1,
				"both shroud border fills publish once after all native row mutations");
		}
		const std::string::size_type borderFailure = shroud.rfind(
			"if (!fillBorderShroudData", borderCall);
		const std::string::size_type borderFailureReturn = shroud.find(
			"return;", borderFailure);
		result |= check(borderFailure != std::string::npos &&
			borderFailureReturn > borderFailure && borderFailureReturn < borderClear,
			"both shroud border publication failures retain the retry latch");
	}
	return result;
}

int testNativeMeshPrelitStageResetContract()
{
	int result = 0;
	result |= check(rts::render::LEGACY_TEXTURE_STAGE_COUNT == 8,
		"the neutral renderer preserves all eight legacy texture stages");

#if defined(RTS_SOURCE_ROOT)
	std::string meshSource;
	result |= check(ReadSourceText(
		"Core/Libraries/Source/WWVegas/WW3D2/nativew3dmeshrenderer.cpp",
		&meshSource), "native mesh source is available for prelit reset checks");
	if (!meshSource.empty())
	{
		const std::string::size_type multiPass = meshSource.find(
			"case MeshGeometryClass::PRELIT_LIGHTMAP_MULTI_PASS");
		const std::string::size_type allStageReset = meshSource.find(
			"for (i = 0; i < rts::render::LEGACY_TEXTURE_STAGE_COUNT; i++)",
			multiPass);
		const std::string::size_type resetCall = meshSource.find(
			"rts::render::SetGameTexture(i, nullptr);", allStageReset);
		const std::string::size_type nextPrelitCase = meshSource.find(
			"case MeshGeometryClass::PRELIT_LIGHTMAP_MULTI_TEXTURE", multiPass);
		result |= check(multiPass != std::string::npos &&
			allStageReset != std::string::npos &&
			resetCall != std::string::npos &&
			(nextPrelitCase == std::string::npos ||
				allStageReset < nextPrelitCase) &&
			(resetCall < nextPrelitCase),
			"no-texture PRELIT multi-pass reset clears every legacy stage");
	}
#endif
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

int testRendererFrameLifecycleState()
{
	int result = 0;
	rts::render::RenderFrameFailureLatch latch;
	result |= check(!latch.hasFailure() &&
		latch.result() == rts::render::RENDER_RESULT_OK,
		"frame failure latch starts clear");
	result |= check(latch.record(rts::render::RENDER_RESULT_FAILED) &&
		latch.hasFailure() &&
		latch.result() == rts::render::RENDER_RESULT_FAILED,
		"first frame command failure is latched");
	result |= check(latch.record(rts::render::RENDER_RESULT_DEVICE_REMOVED) &&
		latch.result() == rts::render::RENDER_RESULT_DEVICE_REMOVED &&
		latch.commandResult() == rts::render::RENDER_RESULT_FAILED &&
		latch.hasDeviceRemoval(),
		"device removal takes precedence while preserving the command failure");
	latch.reset();
	result |= check(!latch.hasFailure() &&
		latch.result() == rts::render::RENDER_RESULT_OK,
		"frame failure latch resets at the next successful frame boundary");

	rts::render::RenderCaptureRequest capture;
	capture.request();
	result |= check(capture.isRequested() &&
		!capture.shouldAttempt(false) && capture.shouldAttempt(true),
		"capture request waits for a visible frame");
	capture.recordFailure();
	result |= check(capture.isRequested() && capture.failureCount() == 1,
		"capture failure retains a bounded retry request");
	capture.recordFailure();
	capture.recordFailure();
	result |= check(!capture.isRequested() && capture.failureCount() ==
		rts::render::RenderCaptureRequest::MAX_FAILURES,
		"capture retry budget is deterministic and bounded");
	capture.request();
	capture.recordSuccess();
	result |= check(!capture.isRequested() && capture.failureCount() == 0,
		"successful visible capture consumes its request");

	rts::render::RenderFrameOutcome queuedFrame;
	result |= check(!queuedFrame.wasSubmitted() && !queuedFrame.wasPresented(),
		"a new frame has neither queue admission nor presentation");
	queuedFrame.markFrameEnded();
	queuedFrame.markSubmitted();
	result |= check(queuedFrame.wasSubmitted() && !queuedFrame.wasPresented() &&
		queuedFrame.result() == rts::render::RENDER_RESULT_OK,
		"queue admission does not falsely report presentation");
	queuedFrame.recordCommandFailure(rts::render::RENDER_RESULT_FAILED);
	result |= check(queuedFrame.wasSubmitted() && !queuedFrame.wasPresented() &&
		queuedFrame.hasCommandFailure(),
		"execution failure remains observable after queue admission");
	rts::render::RenderFrameOutcome completedFrame;
	completedFrame.markPresented();
	result |= check(completedFrame.wasSubmitted() && completedFrame.wasPresented(),
		"actual presentation also establishes submission");

	rts::render::RenderFrameOutcome rejectedFrame;
	result |= check(rejectedFrame.recordCommandFailure(
		rts::render::RENDER_RESULT_FAILED),
		"rejected-frame command failure is recorded");
	rejectedFrame.recordEndFrame(rts::render::RENDER_RESULT_OK);
	rejectedFrame.markFrameEnded();
	rejectedFrame.setOperational(true);
	result |= check(rejectedFrame.frameEnded() && !rejectedFrame.wasPresented() &&
		rejectedFrame.commandResult() == rts::render::RENDER_RESULT_FAILED &&
		rejectedFrame.presentationResult() == rts::render::RENDER_RESULT_OK &&
		rejectedFrame.result() == rts::render::RENDER_RESULT_FAILED &&
		!rejectedFrame.hasLifecycleFailure() && rejectedFrame.isOperational(),
		"command failure remains observable when the incomplete frame is dropped");

	rts::render::RenderFrameOutcome removedFrame;
	removedFrame.recordCommandFailure(rts::render::RENDER_RESULT_FAILED);
	removedFrame.recordEndFrame(rts::render::RENDER_RESULT_OK);
	removedFrame.recordPresentation(
		rts::render::RENDER_RESULT_DEVICE_REMOVED);
	removedFrame.recordRecovery(rts::render::RENDER_RESULT_OK);
	removedFrame.markFrameEnded();
	removedFrame.setOperational(true);
	result |= check(removedFrame.result() ==
		rts::render::RENDER_RESULT_DEVICE_REMOVED &&
		removedFrame.commandResult() == rts::render::RENDER_RESULT_FAILED &&
		removedFrame.hasDeviceRemoval() && !removedFrame.wasPresented(),
		"device removal dominates an earlier command failure and marks the frame unpresented");
	removedFrame.setOperational(false);
	result |= check(!removedFrame.isOperational(),
		"failed recovery can publish an inactive renderer state");

	rts::render::RenderFrameOutcome captureRemovedFrame;
	captureRemovedFrame.recordEndFrame(rts::render::RENDER_RESULT_OK);
	captureRemovedFrame.recordCapture(
		rts::render::RENDER_RESULT_DEVICE_REMOVED);
	captureRemovedFrame.recordRecovery(rts::render::RENDER_RESULT_OK);
	captureRemovedFrame.markFrameEnded();
	captureRemovedFrame.setOperational(true);
	result |= check(captureRemovedFrame.captureResult() ==
		rts::render::RENDER_RESULT_DEVICE_REMOVED &&
		captureRemovedFrame.result() ==
			rts::render::RENDER_RESULT_DEVICE_REMOVED &&
		captureRemovedFrame.hasDeviceRemoval() &&
		!captureRemovedFrame.wasPresented(),
		"capture readback device removal dominates a later presentation path");
	rts::render::RenderFrameOutcome captureFailedFrame;
	captureFailedFrame.recordEndFrame(rts::render::RENDER_RESULT_OK);
	captureFailedFrame.recordCapture(rts::render::RENDER_RESULT_FAILED);
	captureFailedFrame.recordPresentation(rts::render::RENDER_RESULT_OK);
	captureFailedFrame.markFrameEnded();
	captureFailedFrame.markPresented();
	result |= check(captureFailedFrame.hasLifecycleFailure() &&
		captureFailedFrame.result() == rts::render::RENDER_RESULT_FAILED,
		"capture failure is observable even when presentation succeeds");
	return result;
}

struct CaptureQueueProbe
{
	unsigned int completed[8];
	unsigned int cancelled[8];
	unsigned int completionCount;
	unsigned int cancellationCount;
	unsigned int readbackCount;
	const void *firstPixels;
	size_t firstPixelBytes;
	size_t firstRowPitch;
	bool sharedReadback;
	rts::render::RenderCaptureQueue *reentryQueue;
	bool enqueueDuringCancellation;
	bool enqueueDuringCompletion;
	bool resetDuringCompletion;
	bool cancelCurrentDuringCompletion;
	bool shutdownDuringCancellation;
	bool shutdownReturned;
	bool observedShutdownReturned;
	void *additionalEnqueueDuringCompletion;
	bool *shutdownReturnFlag;
	void *cancelDuringCompletion;
	unsigned int reentrantCancellationCount;
	unsigned int reentrantRequestId;
	rts::render::RenderResult reentrantEnqueueResult;
};

void captureQueueCancelled(void *consumer,
	const rts::render::RenderCaptureHandle *handle,
	rts::render::RenderResult reason);

void captureQueueCompleted(void *consumer,
	const rts::render::RenderCaptureHandle *handle, unsigned int,
	unsigned int, size_t rowPitch, rts::render::RenderFormat,
	const void *pixels, size_t pixelBytes)
{
	CaptureQueueProbe *probe = static_cast<CaptureQueueProbe *>(consumer);
	if (pixels != 0)
	{
		++probe->readbackCount;
		if (probe->firstPixels == 0)
		{
			probe->firstPixels = pixels;
			probe->firstPixelBytes = pixelBytes;
			probe->firstRowPitch = rowPitch;
			probe->sharedReadback = true;
		}
		else if (probe->firstPixels != pixels ||
			probe->firstPixelBytes != pixelBytes ||
			probe->firstRowPitch != rowPitch)
		{
			probe->sharedReadback = false;
		}
	}
	if (probe->completionCount < 8)
	{
		probe->completed[probe->completionCount++] = handle->requestId;
	}
	if (probe->reentryQueue != 0 && probe->cancelDuringCompletion != 0)
	{
		void *consumerToCancel = probe->cancelDuringCompletion;
		probe->cancelDuringCompletion = 0;
		probe->reentrantCancellationCount =
			probe->reentryQueue->cancelConsumer(consumerToCancel,
				rts::render::RENDER_RESULT_FAILED);
	}
	if (probe->reentryQueue != 0 && probe->resetDuringCompletion)
	{
		probe->resetDuringCompletion = false;
		probe->reentryQueue->reset();
	}
	if (probe->reentryQueue != 0 && probe->enqueueDuringCompletion)
	{
		probe->enqueueDuringCompletion = false;
		rts::render::RenderCaptureRequestDescriptor descriptor;
		descriptor.kind = rts::render::RENDER_CAPTURE_VISUAL_SMOKE;
		descriptor.consumer = probe;
		descriptor.completed = captureQueueCompleted;
		descriptor.cancelled = captureQueueCancelled;
		rts::render::RenderCaptureHandle reentryHandle;
		probe->reentrantEnqueueResult =
			probe->reentryQueue->enqueue(descriptor, &reentryHandle);
		probe->reentrantRequestId = reentryHandle.requestId;
	}
	if (probe->reentryQueue != 0 &&
		probe->additionalEnqueueDuringCompletion != 0)
	{
		void *additionalConsumer = probe->additionalEnqueueDuringCompletion;
		probe->additionalEnqueueDuringCompletion = 0;
		rts::render::RenderCaptureRequestDescriptor descriptor;
		descriptor.kind = rts::render::RENDER_CAPTURE_VISUAL_SMOKE;
		descriptor.consumer = additionalConsumer;
		descriptor.completed = captureQueueCompleted;
		descriptor.cancelled = captureQueueCancelled;
		rts::render::RenderCaptureHandle reentryHandle;
		probe->reentryQueue->enqueue(descriptor, &reentryHandle);
	}
	if (probe->reentryQueue != 0 && probe->cancelCurrentDuringCompletion)
	{
		probe->cancelCurrentDuringCompletion = false;
		probe->reentryQueue->cancelCurrent(rts::render::RENDER_RESULT_FAILED);
	}
}

void captureQueueCancelled(void *consumer,
	const rts::render::RenderCaptureHandle *handle,
	rts::render::RenderResult)
{
	CaptureQueueProbe *probe = static_cast<CaptureQueueProbe *>(consumer);
	if (probe->cancellationCount < 8)
	{
		probe->cancelled[probe->cancellationCount++] = handle->requestId;
	}
	if (probe->shutdownReturnFlag != 0 && *probe->shutdownReturnFlag)
	{
		probe->observedShutdownReturned = true;
	}
	if (probe->reentryQueue != 0 && probe->shutdownDuringCancellation)
	{
		probe->shutdownDuringCancellation = false;
		probe->reentryQueue->shutdown(rts::render::RENDER_RESULT_FAILED);
		probe->shutdownReturned = true;
	}
	if (probe->reentryQueue != 0 && probe->cancelDuringCompletion != 0)
	{
		void *consumerToCancel = probe->cancelDuringCompletion;
		probe->cancelDuringCompletion = 0;
		probe->reentrantCancellationCount =
			probe->reentryQueue->cancelConsumer(consumerToCancel,
				rts::render::RENDER_RESULT_FAILED);
	}
	if (probe->reentryQueue != 0 && probe->enqueueDuringCancellation)
	{
		probe->enqueueDuringCancellation = false;
		rts::render::RenderCaptureRequestDescriptor descriptor;
		descriptor.kind = rts::render::RENDER_CAPTURE_VISUAL_SMOKE;
		descriptor.consumer = probe;
		descriptor.completed = captureQueueCompleted;
		descriptor.cancelled = captureQueueCancelled;
		rts::render::RenderCaptureHandle reentryHandle;
		probe->reentrantEnqueueResult =
			probe->reentryQueue->enqueue(descriptor, &reentryHandle);
	}
}

void captureQueueThrowingCompleted(void *,
	const rts::render::RenderCaptureHandle *, unsigned int, unsigned int,
	size_t, rts::render::RenderFormat, const void *, size_t)
{
	throw 1;
}

void captureQueueThrowingCancelled(void *,
	const rts::render::RenderCaptureHandle *, rts::render::RenderResult)
{
	throw 1;
}

struct CaptureQueueThreadProbe
{
	rts::render::RenderCaptureQueue *queue;
	rts::render::RenderCaptureRequestDescriptor descriptor;
	rts::render::RenderCaptureHandle handle;
	rts::render::RenderResult result;
};

DWORD WINAPI captureQueueWrongThread(void *parameter)
{
	CaptureQueueThreadProbe *probe =
		static_cast<CaptureQueueThreadProbe *>(parameter);
	probe->result = probe->queue->enqueue(probe->descriptor, &probe->handle);
	return 0;
}

DWORD WINAPI captureQueueShutdownWrongThread(void *parameter)
{
	CaptureQueueThreadProbe *probe =
		static_cast<CaptureQueueThreadProbe *>(parameter);
	probe->queue->shutdown(rts::render::RENDER_RESULT_FAILED);
	probe->result = rts::render::RENDER_RESULT_OK;
	return 0;
}

DWORD WINAPI captureQueueDeleteWrongThread(void *parameter)
{
	CaptureQueueThreadProbe *probe =
		static_cast<CaptureQueueThreadProbe *>(parameter);
	delete probe->queue;
	probe->queue = 0;
	probe->result = rts::render::RENDER_RESULT_OK;
	return 0;
}

int testRenderCaptureQueue()
{
	int result = 0;
	CaptureQueueProbe probe;
	memset(&probe, 0, sizeof(probe));
	probe.reentrantEnqueueResult = rts::render::RENDER_RESULT_FAILED;
	rts::render::RenderCaptureQueue queue(2);
	rts::render::RenderCaptureRequestDescriptor descriptor;
	descriptor.kind = rts::render::RENDER_CAPTURE_VISUAL_SMOKE;
	descriptor.consumer = &probe;
	descriptor.completed = captureQueueCompleted;
	descriptor.cancelled = captureQueueCancelled;
	rts::render::RenderCaptureHandle unboundHandle;
	result |= check(queue.enqueue(descriptor, &unboundHandle) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"capture queue rejects mutation before an owner thread is bound");
	result |= check(queue.bindOwnerThread(),
		"capture queue binds explicitly before accepting owner-thread work");
	rts::render::RenderCaptureHandle first;
	rts::render::RenderCaptureHandle second;
	result |= check(queue.enqueue(descriptor, &first) ==
		rts::render::RENDER_RESULT_OK && first.requestId != 0 &&
		first.generation == queue.generation(),
		"capture queue assigns stable generation-safe request IDs");
	descriptor.kind = rts::render::RENDER_CAPTURE_MOVIE;
	result |= check(queue.enqueue(descriptor, &second) ==
		rts::render::RENDER_RESULT_OK && second.requestId > first.requestId,
		"capture queue preserves FIFO request IDs across consumer kinds");
	CaptureQueueThreadProbe threadProbe;
	memset(&threadProbe, 0, sizeof(threadProbe));
	threadProbe.queue = &queue;
	threadProbe.descriptor = descriptor;
	threadProbe.result = rts::render::RENDER_RESULT_OK;
	HANDLE wrongThread = CreateThread(0, 0, captureQueueWrongThread,
		&threadProbe, 0, 0);
	if (wrongThread != 0)
	{
		WaitForSingleObject(wrongThread, INFINITE);
		CloseHandle(wrongThread);
	}
	result |= check(wrongThread != 0 && threadProbe.result ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT && queue.pendingCount() == 2,
		"capture queue rejects cross-thread mutation of owner-thread state");
	rts::render::RenderCaptureHandle overflow;
	result |= check(queue.enqueue(descriptor, &overflow) ==
		rts::render::RENDER_RESULT_OUT_OF_MEMORY &&
		probe.cancellationCount == 0,
		"capture queue reports bounded overflow without silently dropping work");
	std::vector<unsigned char> pixels(640 * 480 * 4, 7);
	result |= check(queue.completeVisible(640, 480, 2559,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM, &pixels[0], pixels.size()) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		queue.pendingCount() == 2,
		"capture queue rejects a row pitch smaller than the actual pixel width");
	result |= check(queue.completeVisible(640, 480, 2560,
		rts::render::RENDER_FORMAT_UNKNOWN, &pixels[0], pixels.size()) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		queue.pendingCount() == 2,
		"capture queue rejects an unknown back-buffer format before callbacks");
	result |= check(queue.completeVisible(640, 480, 2560,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM, &pixels[0], pixels.size()) ==
		rts::render::RENDER_RESULT_OK && probe.completionCount == 2 &&
		probe.readbackCount == 2 &&
		probe.sharedReadback &&
		probe.completed[0] == first.requestId &&
		probe.completed[1] == second.requestId,
		"visible capture completion is FIFO and shares one readback batch");
	result |= check(queue.pendingCount() == 0,
		"completed visible captures leave no stale requests");

	CaptureQueueProbe firstConsumer;
	CaptureQueueProbe secondConsumer;
	memset(&firstConsumer, 0, sizeof(firstConsumer));
	memset(&secondConsumer, 0, sizeof(secondConsumer));
	descriptor.kind = rts::render::RENDER_CAPTURE_VISUAL_SMOKE;
	descriptor.consumer = &firstConsumer;
	result |= check(queue.enqueue(descriptor, &first) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts the first reentrant completion consumer");
	descriptor.consumer = &secondConsumer;
	result |= check(queue.enqueue(descriptor, &second) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts the second reentrant completion consumer");
	firstConsumer.reentryQueue = &queue;
	firstConsumer.cancelDuringCompletion = &secondConsumer;
	result |= check(queue.completeVisible(640, 480, 2560,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM, &pixels[0], pixels.size()) ==
		rts::render::RENDER_RESULT_OK &&
		firstConsumer.completionCount == 1 &&
		firstConsumer.reentrantCancellationCount == 1 &&
		secondConsumer.completionCount == 0 &&
		secondConsumer.cancellationCount == 1 && queue.pendingCount() == 0,
		"completion callbacks can synchronously cancel a later consumer safely");
	firstConsumer.reentryQueue = 0;

	CaptureQueueProbe shutdownConsumer;
	CaptureQueueProbe lateConsumer;
	memset(&shutdownConsumer, 0, sizeof(shutdownConsumer));
	memset(&lateConsumer, 0, sizeof(lateConsumer));
	descriptor.consumer = &shutdownConsumer;
	result |= check(queue.enqueue(descriptor, &first) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts a completion consumer before nested shutdown");
	descriptor.consumer = &lateConsumer;
	result |= check(queue.enqueue(descriptor, &second) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts a later completion consumer before nested shutdown");
	shutdownConsumer.reentryQueue = &queue;
	shutdownConsumer.enqueueDuringCompletion = true;
	shutdownConsumer.additionalEnqueueDuringCompletion = &lateConsumer;
	shutdownConsumer.cancelCurrentDuringCompletion = true;
	shutdownConsumer.shutdownDuringCancellation = true;
	lateConsumer.reentryQueue = &queue;
	lateConsumer.shutdownReturnFlag = &shutdownConsumer.shutdownReturned;
	result |= check(queue.completeVisible(640, 480, 2560,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM, &pixels[0], pixels.size()) ==
		rts::render::RENDER_RESULT_OK && shutdownConsumer.shutdownReturned &&
		lateConsumer.cancellationCount >= 1 &&
		!lateConsumer.observedShutdownReturned && queue.pendingCount() == 0,
		"nested shutdown drains the active cancellation batch before returning");
	shutdownConsumer.reentryQueue = 0;
	lateConsumer.reentryQueue = 0;
	result |= check(queue.bindOwnerThread(),
		"capture queue can rebind after nested shutdown");
	queue.reset();
	descriptor.consumer = &firstConsumer;
	result |= check(queue.enqueue(descriptor, &first) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts the first reentrant cancellation consumer");
	descriptor.consumer = &secondConsumer;
	result |= check(queue.enqueue(descriptor, &second) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts the second reentrant cancellation consumer");
	firstConsumer.reentryQueue = &queue;
	firstConsumer.cancelDuringCompletion = &secondConsumer;
	firstConsumer.reentrantCancellationCount = 0;
	const unsigned int priorSecondCancellations =
		secondConsumer.cancellationCount;
	result |= check(queue.cancelCurrent(rts::render::RENDER_RESULT_FAILED) == 2 &&
		firstConsumer.reentrantCancellationCount == 1 &&
		secondConsumer.cancellationCount == priorSecondCancellations + 1 &&
		queue.pendingCount() == 0,
		"cancellation callbacks can synchronously cancel a later consumer safely");
	firstConsumer.reentryQueue = 0;
	descriptor.consumer = &firstConsumer;
	result |= check(queue.enqueue(descriptor, &first) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts the first reset-during-completion consumer");
	descriptor.consumer = &secondConsumer;
	result |= check(queue.enqueue(descriptor, &second) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts the second reset-during-completion consumer");
	firstConsumer.reentryQueue = &queue;
	firstConsumer.resetDuringCompletion = true;
	firstConsumer.enqueueDuringCompletion = true;
	secondConsumer.reentryQueue = &queue;
	secondConsumer.shutdownDuringCancellation = true;
	firstConsumer.reentrantEnqueueResult = rts::render::RENDER_RESULT_FAILED;
	firstConsumer.reentrantRequestId = 0;
	const unsigned int priorResetSecondCompletions =
		secondConsumer.completionCount;
	const unsigned int priorResetSecondCancellations =
		secondConsumer.cancellationCount;
	result |= check(queue.completeVisible(640, 480, 2560,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM, &pixels[0], pixels.size()) ==
		rts::render::RENDER_RESULT_OK &&
		firstConsumer.reentrantEnqueueResult ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		firstConsumer.reentrantRequestId == 0 &&
		secondConsumer.completionCount == priorResetSecondCompletions &&
		secondConsumer.cancellationCount == priorResetSecondCancellations + 1 &&
		queue.pendingCount() == 0,
		"nested shutdown wins over reset and rejects reentrant work");
	firstConsumer.reentryQueue = 0;
	secondConsumer.reentryQueue = 0;
	descriptor.consumer = &firstConsumer;
	result |= check(queue.enqueue(descriptor, &first) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"nested shutdown leaves the capture queue inactive after reset returns");
	result |= check(queue.bindOwnerThread(),
		"capture queue can rebind after shutdown wins over reset");
	queue.reset();

	rts::render::RenderCaptureHandle stale;
	descriptor.consumer = &probe;
	result |= check(queue.enqueue(descriptor, &stale) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts a request after completion");
	const unsigned int oldGeneration = queue.generation();
	probe.reentryQueue = &queue;
	probe.enqueueDuringCancellation = true;
	queue.advanceGeneration();
	result |= check(queue.generation() != oldGeneration &&
		queue.cancelStale(rts::render::RENDER_RESULT_FAILED) == 1 &&
		probe.cancellationCount == 1 && queue.pendingCount() == 1,
		"resize or recovery generation cancellation is deterministic");
	probe.reentryQueue = 0;

	CaptureQueueThreadProbe deleteProbe;
	memset(&deleteProbe, 0, sizeof(deleteProbe));
	deleteProbe.queue = new rts::render::RenderCaptureQueue(1);
	deleteProbe.descriptor = descriptor;
	deleteProbe.descriptor.consumer = &probe;
	result |= check(deleteProbe.queue != 0 &&
		deleteProbe.queue->bindOwnerThread() &&
		deleteProbe.queue->enqueue(deleteProbe.descriptor, &deleteProbe.handle) ==
			rts::render::RENDER_RESULT_OK,
		"capture queue accepts work before off-owner destruction fallback");
	HANDLE wrongThreadDelete = CreateThread(0, 0,
		captureQueueDeleteWrongThread, &deleteProbe, 0, 0);
	WaitForSingleObject(wrongThreadDelete, INFINITE);
	CloseHandle(wrongThreadDelete);
	result |= check(deleteProbe.result == rts::render::RENDER_RESULT_OK &&
		deleteProbe.queue == 0,
		"off-owner destruction fails closed without terminating the process");

	queue.shutdown(rts::render::RENDER_RESULT_FAILED);
	CaptureQueueThreadProbe shutdownProbe;
	memset(&shutdownProbe, 0, sizeof(shutdownProbe));
	shutdownProbe.queue = &queue;
	shutdownProbe.result = rts::render::RENDER_RESULT_FAILED;
	HANDLE repeatedShutdown = CreateThread(0, 0,
		captureQueueShutdownWrongThread, &shutdownProbe, 0, 0);
	if (repeatedShutdown != 0)
	{
		WaitForSingleObject(repeatedShutdown, INFINITE);
		CloseHandle(repeatedShutdown);
	}
	result |= check(queue.pendingCount() == 0 && probe.cancellationCount == 2 &&
		repeatedShutdown != 0 && queue.bindOwnerThread(),
		"capture queue shutdown drains once and repeated cross-thread shutdown is inert");
	queue.reset();
	result |= check(queue.generation() == 1 && queue.pendingCount() == 0,
		"capture queue reset reactivates a deliberately shut-down owner queue");
	result |= check(queue.enqueue(descriptor, &stale) ==
		rts::render::RENDER_RESULT_OK,
		"capture queue accepts work before an active reset");
	probe.reentryQueue = &queue;
	probe.enqueueDuringCancellation = true;
	probe.reentrantEnqueueResult = rts::render::RENDER_RESULT_OK;
	queue.reset();
	result |= check(probe.reentrantEnqueueResult ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		queue.pendingCount() == 0,
		"capture queue rejects reentrant enqueue while reset drains consumers");
	probe.reentryQueue = 0;

	rts::render::RenderCaptureQueue throwingQueue(1);
	rts::render::RenderCaptureRequestDescriptor throwingDescriptor;
	throwingDescriptor.kind = rts::render::RENDER_CAPTURE_PROFILER;
	throwingDescriptor.consumer = &probe;
	throwingDescriptor.completed = captureQueueThrowingCompleted;
	throwingDescriptor.cancelled = captureQueueThrowingCancelled;
	result |= check(throwingQueue.bindOwnerThread() &&
		throwingQueue.enqueue(throwingDescriptor, &first) ==
			rts::render::RENDER_RESULT_OK &&
		throwingQueue.completeVisible(1, 1, 4,
			rts::render::RENDER_FORMAT_B8G8R8A8_UNORM, &pixels[0], 4) ==
		rts::render::RENDER_RESULT_OK && throwingQueue.pendingCount() == 0,
		"capture queue contains exceptions from completion callbacks");
	result |= check(throwingQueue.enqueue(throwingDescriptor, &first) ==
		rts::render::RENDER_RESULT_OK &&
		throwingQueue.cancelCurrent(rts::render::RENDER_RESULT_FAILED) == 1 &&
		throwingQueue.pendingCount() == 0,
		"capture queue contains exceptions from cancellation callbacks");
	throwingQueue.shutdown(rts::render::RENDER_RESULT_FAILED);
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
	result |= check(state.pipeline.ambientMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_MATERIAL &&
		state.pipeline.diffuseMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_COLOR1 &&
		state.pipeline.emissiveMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_MATERIAL &&
		state.pipeline.specularMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_COLOR2,
		"legacy material-source state uses D3D8 defaults (COLOR1 diffuse/COLOR2 specular, material ambient/emissive)");
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

int testLegacyResetSeedAndAmbientUpdate()
{
	int result = 0;
	rts::render::LegacyPipelineState pipeline;
	rts::render::LegacyLogicalState logical;
	rts::render::ResetTrackedLegacyState();
	result |= check(!rts::render::GetTrackedLegacyPipelineState(&pipeline),
		"legacy reset invalidates the tracked pipeline before reseeding");
	rts::render::SeedTrackedLegacyPipelineState();
	result |= check(rts::render::GetTrackedLegacyPipelineState(&pipeline) &&
		pipeline.depthStencil.depthEnable && pipeline.depthStencil.depthWrite &&
		pipeline.depthStencil.depthFunction ==
			rts::render::RENDER_COMPARE_LESS_EQUAL &&
		pipeline.rasterizer.fillMode == rts::render::RENDER_FILL_SOLID,
		"explicit neutral pipeline seed restores valid deterministic defaults");
	const rts::render::RenderFloat4 rawAmbient =
		rts::render::DecodeLegacyAmbientColor(0x80402010U);
	result |= check(rawAmbient.x == (64.0f / 255.0f) &&
		rawAmbient.y == (32.0f / 255.0f) &&
		rawAmbient.z == (16.0f / 255.0f) && rawAmbient.w == 1.0f,
		"raw D3D8 ambient color decodes to neutral RGB channels");
	rts::render::TrackLegacyGlobalAmbient(
		rts::render::RenderFloat4(0.1f, 0.2f, 0.3f, 1.0f));
	rts::render::TrackLegacyGlobalAmbient(
		rts::render::RenderFloat4(0.7f, 0.6f, 0.5f, 1.0f));
	result |= check(rts::render::GetTrackedLegacyLogicalState(&logical) &&
		logical.constants.globalAmbient.x == 0.7f &&
		logical.constants.globalAmbient.y == 0.6f &&
		logical.constants.globalAmbient.z == 0.5f,
		"tracked light-environment ambient replaces the prior scene ambient");
	rts::render::ResetTrackedLegacyState();
	return result;
}

int testLegacyWaterPixelProgram()
{
	int result = 0;
	rts::render::LegacyPipelineState state;
	result |= check(state.pixelProgram ==
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION,
		"legacy pipeline defaults to fixed-function pixel evaluation");
	const rts::render::LegacyShaderKey fixedKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	state.pixelProgram = rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT;
	const rts::render::LegacyShaderKey flatKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	state.pixelProgram = rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER;
	const rts::render::LegacyShaderKey riverKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	state.pixelProgram = rts::render::RENDER_LEGACY_PIXEL_TERRAIN_BASE;
	const rts::render::LegacyShaderKey terrainKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	result |= check(fixedKey != flatKey && flatKey != riverKey &&
		riverKey != terrainKey,
		"legacy water pixel programs produce distinct shader keys");

	state.textureStages[2].cameraSpacePosition = true;
	state.textureStages[2].textureTransformEnable = true;
	const rts::render::LegacyShaderKey cameraKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	result |= check(cameraKey != terrainKey,
		"legacy shader keys distinguish camera-space transformed water stages");
	state.textureStages[2].cameraSpacePosition = false;
	state.textureStages[2].cameraSpaceNormal = true;
	const rts::render::LegacyShaderKey cameraNormalKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	state.textureStages[2].cameraSpaceNormal = false;
	state.textureStages[2].cameraSpaceReflectionVector = true;
	const rts::render::LegacyShaderKey cameraReflectionKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x0fU);
	result |= check(cameraNormalKey != cameraKey &&
		cameraReflectionKey != cameraNormalKey && cameraReflectionKey != cameraKey,
		"legacy shader keys distinguish all generated camera-space coordinates");

	rts::render::TrackLegacyShaderBits(0);
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT);
	rts::render::TrackLegacyShaderBits(0);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.pixelProgram == rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT,
		"shader publication preserves an explicitly selected water pixel program");
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.pixelProgram == rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER,
		"neutral state switches from flat to river water program");
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.pixelProgram ==
			rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2,
		"neutral state preserves the full terrain pixel-program range");
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.pixelProgram ==
			rts::render::RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE,
		"neutral state publishes the profiler swizzle program");
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
	return result;
}

int testLegacyTextureTransformAndNormalMath()
{
	int result = 0;
	rts::render::LegacyTextureStageState stage;
	result |= check(stage.textureTransformCount == 0 &&
		!stage.projectedCoordinates,
		"legacy texture transform defaults to disabled COUNT0");
	for (unsigned int count = 0; count <= 4; ++count)
	{
		result |= check(rts::render::IsLegacyTextureTransformCountValid(count),
			"COUNT0 through COUNT4 are valid neutral transform counts");
	}
	result |= check(!rts::render::IsLegacyTextureTransformCountValid(5),
		"transform counts above COUNT4 are rejected");
	result |= check(!rts::render::IsLegacyProjectedTextureTransformValid(0, true) &&
		!rts::render::IsLegacyProjectedTextureTransformValid(1, true) &&
		rts::render::IsLegacyProjectedTextureTransformValid(2, true) &&
		rts::render::IsLegacyProjectedTextureTransformValid(3, true) &&
		rts::render::IsLegacyProjectedTextureTransformValid(4, true),
		"projected coordinates require COUNT2, COUNT3, or COUNT4");
	const rts::render::RenderFloat4 coordinate(10.0f, 20.0f, 30.0f,
		40.0f);
	result |= check(nearlyEqual(
		rts::render::LegacyProjectedTextureDenominator(0, coordinate), 1.0f) &&
		nearlyEqual(rts::render::LegacyProjectedTextureDenominator(1, coordinate),
			1.0f) &&
		nearlyEqual(rts::render::LegacyProjectedTextureDenominator(2, coordinate),
			20.0f) &&
		nearlyEqual(rts::render::LegacyProjectedTextureDenominator(3, coordinate),
			30.0f) &&
		nearlyEqual(rts::render::LegacyProjectedTextureDenominator(4, coordinate),
			40.0f),
		"projected denominator follows D3D8 COUNT2/3/4 component semantics");

	rts::render::LegacyPipelineState pipeline;
	const rts::render::LegacyShaderKey count1Key =
		rts::render::BuildLegacyShaderKey(pipeline, 0x55U, 1U);
	pipeline.textureStages[0].textureTransformCount = 2;
	const rts::render::LegacyShaderKey count2Key =
		rts::render::BuildLegacyShaderKey(pipeline, 0x55U, 1U);
	pipeline.textureStages[0].textureTransformCount = 4;
	const rts::render::LegacyShaderKey count4Key =
		rts::render::BuildLegacyShaderKey(pipeline, 0x55U, 1U);
	result |= check(count1Key != count2Key && count2Key != count4Key,
		"texture transform component count participates in shader identity");

	rts::render::RenderMatrix4 nonUniformScale;
	for (unsigned int index = 0; index < 16; ++index)
	{
		nonUniformScale.values[index] = 0.0f;
	}
	nonUniformScale.values[0] = 2.0f;
	nonUniformScale.values[5] = 3.0f;
	nonUniformScale.values[10] = 4.0f;
	nonUniformScale.values[15] = 1.0f;
	float uploadedNormalMatrix[12];
	const bool builtNormalMatrix =
		rts::render::BuildLegacyInverseTransposeNormalMatrix(
			nonUniformScale, uploadedNormalMatrix);
	result |= check(builtNormalMatrix &&
		nearlyEqual(uploadedNormalMatrix[0], 0.5f) &&
		nearlyEqual(uploadedNormalMatrix[1], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[2], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[4], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[5], 1.0f / 3.0f) &&
		nearlyEqual(uploadedNormalMatrix[6], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[8], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[9], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[10], 0.25f) &&
		nearlyEqual(uploadedNormalMatrix[3], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[7], 0.0f) &&
		nearlyEqual(uploadedNormalMatrix[11], 0.0f),
		"uploaded normal matrix contains the inverse-transpose rows and padding");
	rts::render::RenderMatrix4 shear;
	shear.setIdentity();
	shear.values[1] = 1.0f;
	float shearNormalMatrix[12];
	const bool builtShearNormalMatrix =
		rts::render::BuildLegacyInverseTransposeNormalMatrix(
			shear, shearNormalMatrix);
	result |= check(builtShearNormalMatrix &&
		nearlyEqual(shearNormalMatrix[0], 1.0f) &&
		nearlyEqual(shearNormalMatrix[1], 0.0f) &&
		nearlyEqual(shearNormalMatrix[4], -1.0f) &&
		nearlyEqual(shearNormalMatrix[5], 1.0f) &&
		nearlyEqual(shearNormalMatrix[10], 1.0f),
		"uploaded normal matrix transposes the inverse for a shear transform");
	rts::render::RenderMatrix4 singular;
	singular.setIdentity();
	singular.values[5] = 0.0f;
	float singularNormalMatrix[12];
	result |= check(!rts::render::BuildLegacyInverseTransposeNormalMatrix(
			singular, singularNormalMatrix) &&
		nearlyEqual(singularNormalMatrix[0], 0.0f) &&
		nearlyEqual(singularNormalMatrix[5], 0.0f) &&
		nearlyEqual(singularNormalMatrix[10], 0.0f),
		"singular normal-matrix uploads are zero-filled for shader fallback");
	const rts::render::RenderFloat4 sourceNormal(1.0f, 1.0f, 1.0f, 0.0f);
	const rts::render::RenderFloat4 transformed =
		rts::render::TransformLegacyCameraNormal(nonUniformScale,
			sourceNormal, true, false, false);
	result |= check(nearlyEqual(transformed.x, 0.5f) &&
		nearlyEqual(transformed.y, 1.0f / 3.0f) &&
		nearlyEqual(transformed.z, 0.25f),
		"camera-space normal uses inverse-transpose under non-uniform scale");
	const rts::render::RenderFloat4 normalized =
		rts::render::TransformLegacyCameraNormal(nonUniformScale,
			sourceNormal, true, false, true);
	const float normalizedLength = static_cast<float>(sqrt(
		normalized.x * normalized.x + normalized.y * normalized.y +
		normalized.z * normalized.z));
	result |= check(nearlyEqual(normalizedLength, 1.0f),
		"NORMALIZENORMALS normalizes the inverse-transpose result");
	const rts::render::RenderFloat4 missing =
		rts::render::TransformLegacyCameraNormal(nonUniformScale,
		sourceNormal, false, false, false);
	const rts::render::RenderFloat4 preTransformed =
		rts::render::TransformLegacyCameraNormal(nonUniformScale,
		sourceNormal, true, true, false);
	result |= check(nearlyEqual(missing.x, 0.0f) &&
		nearlyEqual(missing.y, 0.0f) && nearlyEqual(missing.z, 1.0f) &&
		nearlyEqual(preTransformed.x, 0.0f) &&
		nearlyEqual(preTransformed.y, 0.0f) &&
		nearlyEqual(preTransformed.z, 1.0f),
		"missing and pre-transformed normals use a safe camera-space default");
	const rts::render::RenderFloat4 singularNormal =
		rts::render::TransformLegacyCameraNormal(singular, sourceNormal,
			true, false, false);
	result |= check(nearlyEqual(singularNormal.x, 0.0f) &&
		nearlyEqual(singularNormal.y, 0.0f) &&
		nearlyEqual(singularNormal.z, 1.0f),
		"singular camera-space normal transforms use a safe default");
	return result;
}

int testLegacySeaWaveProgram()
{
	int result = 0;
	rts::render::LegacyPipelineState state;
	const rts::render::LegacyShaderKey riverPixelKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x03U);
	state.pixelProgram = rts::render::RENDER_LEGACY_PIXEL_WATER_SEA;
	const rts::render::LegacyShaderKey seaPixelKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x03U);
	state.vertexProgram = rts::render::RENDER_LEGACY_VERTEX_TREES;
	const rts::render::LegacyShaderKey treeVertexKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x03U);
	state.vertexProgram = rts::render::RENDER_LEGACY_VERTEX_WATER_SEA;
	const rts::render::LegacyShaderKey seaVertexKey =
		rts::render::BuildLegacyShaderKey(state, 0x11223344U, 0x03U);
	result |= check(riverPixelKey != seaPixelKey &&
		seaPixelKey != treeVertexKey && treeVertexKey != seaVertexKey,
		"sea wave pixel and vertex programs have distinct shader keys");
	result |= check(rts::render::RENDER_FORMAT_R8G8_SNORM !=
		rts::render::RENDER_FORMAT_UNKNOWN &&
		rts::render::RENDER_FORMAT_R8G8_SNORM !=
		rts::render::RENDER_FORMAT_R8G8B8A8_UNORM,
		"sea wave bump format is a distinct signed two-channel format");
	rts::render::LegacyVertexLayout seaLayout;
	seaLayout.stride = 24;
	seaLayout.elementCount = 3;
	seaLayout.elements[0].semantic =
		rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
	seaLayout.elements[0].format = rts::render::RENDER_VERTEX_DATA_FLOAT3;
	seaLayout.elements[0].byteOffset = 0;
	seaLayout.elements[1].semantic =
		rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
	seaLayout.elements[1].format = rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
	seaLayout.elements[1].byteOffset = 12;
	seaLayout.elements[2].semantic =
		rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	seaLayout.elements[2].format = rts::render::RENDER_VERTEX_DATA_FLOAT2;
	seaLayout.elements[2].byteOffset = 16;
	result |= check(!seaLayout.preTransformed &&
		seaLayout.elements[0].semanticIndex == 0 &&
		seaLayout.elements[1].semanticIndex == 0 &&
		seaLayout.elements[2].semanticIndex == 0,
		"sea wave declaration preserves the 24-byte position/color/UV layout");

	rts::render::TrackLegacyShaderBits(0);
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_WATER_SEA);
	rts::render::TrackLegacyVertexProgram(
		rts::render::RENDER_LEGACY_VERTEX_WATER_SEA);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.pixelProgram == rts::render::RENDER_LEGACY_PIXEL_WATER_SEA &&
		state.vertexProgram == rts::render::RENDER_LEGACY_VERTEX_WATER_SEA,
		"sea wave programs publish through the neutral state tracker");
	rts::render::TrackLegacyPixelProgram(
		static_cast<rts::render::RenderLegacyPixelProgram>(999));
	rts::render::TrackLegacyVertexProgram(
		static_cast<rts::render::RenderLegacyVertexProgram>(999));
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.pixelProgram == rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION &&
		state.vertexProgram == rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION,
		"invalid sea program publication resets to safe fixed-function programs");

	// R8G8_SNORM samples are already signed.  This is the CPU equivalent of
	// the PSSeaWave equation: signed bump dot stage-1 matrix, apply the legacy
	// luminance scale/offset, sample reflection, and multiply every channel
	// (including alpha) by the diffuse color.
	const float signedBumpX = -1.0f;
	const float signedBumpY = 0.5f;
	const float luminanceScale = 0.5f;
	const float luminanceOffset = 0.125f;
	const float offsetX =
		(signedBumpX * 0.25f + signedBumpY * 0.0f) * luminanceScale +
		luminanceOffset;
	const float offsetY =
		(signedBumpX * 0.0f + signedBumpY * 0.5f) * luminanceScale +
		luminanceOffset;
	const float reflectionU = 0.75f + offsetX;
	const float reflectionV = 0.25f + offsetY;
	const float outputRed = 0.4f * 0.5f;
	const float outputGreen = 0.6f * 0.25f;
	const float outputBlue = 0.8f * 0.75f;
	const float outputAlpha = 0.5f * 0.5f;
	result |= check(reflectionU > 0.749f && reflectionU < 0.751f &&
		reflectionV > 0.499f && reflectionV < 0.501f &&
		outputRed > 0.199f && outputRed < 0.201f &&
		outputGreen > 0.149f && outputGreen < 0.151f &&
		outputBlue > 0.599f && outputBlue < 0.601f &&
		outputAlpha > 0.249f && outputAlpha < 0.251f,
		"sea wave signed bump offset and diffuse alpha equation are stable");
	rts::render::ResetTrackedLegacyState();
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
		state.blend.sourceColor == rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		state.alphaReference == 0x9fU &&
		state.alphaFunction == rts::render::RENDER_COMPARE_LESS_EQUAL &&
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
	rts::render::TrackLegacyCullState(true, true);
	rts::render::GetTrackedLegacyPipelineState(&state);
	state.rangeFogEnable = true;
	state.lightingEnable = true;
	state.normalizeNormals = true;
	state.textureFactor = 0x80402010U;
	state.clipPlaneEnableMask = 0x5U;
	state.ambientMaterialSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR1;
	state.diffuseMaterialSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR2;
	state.emissiveMaterialSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR1;
	state.specularMaterialSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR2;
	state.depthStencil.stencilEnable = true;
	state.depthStencil.stencilReference = 7U;
	state.rasterizer.fillMode = rts::render::RENDER_FILL_WIREFRAME;
	state.rasterizer.depthBias = 3;
	state.blend.sourceColor = rts::render::RENDER_BLEND_SOURCE_ALPHA;
	state.blend.destinationColor =
		rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
	state.blend.sourceAlpha = rts::render::RENDER_BLEND_SOURCE_ALPHA;
	state.blend.destinationAlpha =
		rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
	state.blend.colorOperation =
		rts::render::RENDER_BLEND_REVERSE_SUBTRACT;
	state.blend.alphaOperation = rts::render::RENDER_BLEND_SUBTRACT;
	state.blend.colorWriteMask = 0x5U;
	state.alphaFunction = rts::render::RENDER_COMPARE_NOT_EQUAL;
	state.alphaReference = 37U;
	rts::render::TrackLegacyPipelineState(state);
	// Re-selecting an already-applied shader must not erase the effective
	// D3D8 winding or independent range-fog state when their cached render-state
	// calls are suppressed.
	rts::render::TrackLegacyShaderBits(opaque);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.shaderBits == opaque &&
		state.rasterizer.cullMode == rts::render::RENDER_CULL_BACK &&
		state.rasterizer.frontCounterClockwise &&
		state.rangeFogEnable &&
		state.lightingEnable && state.normalizeNormals &&
		state.textureFactor == 0x80402010U &&
		state.clipPlaneEnableMask == 0x5U &&
		state.ambientMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_COLOR1 &&
		state.diffuseMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_COLOR2 &&
		state.emissiveMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_COLOR1 &&
		state.specularMaterialSource ==
			rts::render::RENDER_MATERIAL_SOURCE_COLOR2 &&
		state.depthStencil.stencilEnable &&
		state.depthStencil.stencilReference == 7U &&
		state.rasterizer.fillMode == rts::render::RENDER_FILL_WIREFRAME &&
		state.rasterizer.depthBias == 3 &&
		!state.blend.blendEnable &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_SOURCE_ALPHA &&
		state.blend.destinationColor ==
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		state.blend.colorOperation ==
			rts::render::RENDER_BLEND_REVERSE_SUBTRACT &&
		state.blend.alphaOperation == rts::render::RENDER_BLEND_SUBTRACT &&
		state.blend.colorWriteMask == 0x5U &&
		!state.alphaTestEnable &&
		state.alphaFunction == rts::render::RENDER_COMPARE_NOT_EQUAL &&
		state.alphaReference == 37U &&
		state.textureStages[0].colorOperation == rts::render::RENDER_TEXTURE_OP_MODULATE,
		"legacy shader publication preserves all independent pipeline state");
	rts::render::TrackLegacyShaderBits(alphaTested);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.shaderBits == alphaTested && state.rangeFogEnable &&
		state.lightingEnable && state.normalizeNormals &&
		state.textureFactor == 0x80402010U &&
		state.clipPlaneEnableMask == 0x5U &&
		state.depthStencil.stencilEnable &&
		state.rasterizer.fillMode == rts::render::RENDER_FILL_WIREFRAME &&
		state.blend.blendEnable &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_SOURCE_ALPHA &&
		state.blend.destinationColor ==
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		state.blend.colorOperation ==
			rts::render::RENDER_BLEND_REVERSE_SUBTRACT &&
		state.blend.alphaOperation == rts::render::RENDER_BLEND_SUBTRACT &&
		state.blend.colorWriteMask == 0x5U &&
		state.alphaTestEnable && state.alphaReference == 0x60U &&
		state.alphaFunction == rts::render::RENDER_COMPARE_GREATER_EQUAL,
		"legacy shader changes preserve independent lighting and raster state");
	rts::render::TrackLegacyShaderBits(opaque);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		!state.blend.blendEnable &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_SOURCE_ALPHA &&
		state.blend.destinationColor ==
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		!state.alphaTestEnable && state.alphaReference == 0x60U &&
		state.alphaFunction == rts::render::RENDER_COMPARE_GREATER_EQUAL,
		"opaque shader selection retains inactive blend and alpha-test parameters");
	rts::render::GetTrackedLegacyPipelineState(&state);
	rts::render::LegacyPipelineState effectiveState = state;
	effectiveState.blend.blendEnable = true;
	effectiveState.blend.sourceColor = rts::render::RENDER_BLEND_SOURCE_ALPHA;
	effectiveState.blend.destinationColor =
		rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
	effectiveState.depthStencil.depthWrite = false;
	effectiveState.depthStencil.depthFunction = rts::render::RENDER_COMPARE_EQUAL;
	effectiveState.rasterizer.fillMode = rts::render::RENDER_FILL_WIREFRAME;
	effectiveState.alphaTestEnable = true;
	effectiveState.alphaReference = 96;
	effectiveState.lightingEnable = true;
	effectiveState.textureFactor = 0x80402010U;
	rts::render::TrackLegacyPipelineState(effectiveState);
	result |= check(rts::render::GetTrackedLegacyPipelineState(&state) &&
		state.blend.blendEnable &&
		state.blend.sourceColor == rts::render::RENDER_BLEND_SOURCE_ALPHA &&
		state.blend.destinationColor ==
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		!state.depthStencil.depthWrite &&
		state.depthStencil.depthFunction == rts::render::RENDER_COMPARE_EQUAL &&
		state.rasterizer.fillMode == rts::render::RENDER_FILL_WIREFRAME &&
		state.alphaTestEnable && state.alphaReference == 96 &&
		state.lightingEnable && state.textureFactor == 0x80402010U,
		"legacy bridge publishes effective low-level pipeline changes without device readback");
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
	const float monochromeConstants[12] = {
		0.30f, 0.59f, 0.11f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		0.5f, 0.5f, 0.5f, 1.0f
	};
	rts::render::TrackLegacyPixelProgram(
		rts::render::RENDER_LEGACY_PIXEL_MONOCHROME);
	result |= check(rts::render::TrackLegacyPixelShaderConstants(0,
		monochromeConstants, 3) &&
		!rts::render::TrackLegacyPixelShaderConstants(
			rts::render::LEGACY_PIXEL_CONSTANT_COUNT, monochromeConstants, 1) &&
		rts::render::GetTrackedLegacyLogicalState(&logicalState) &&
		logicalState.pipeline.pixelProgram ==
			rts::render::RENDER_LEGACY_PIXEL_MONOCHROME &&
		logicalState.constants.pixelShaderConstants[0].y == 0.59f &&
		logicalState.constants.pixelShaderConstants[2].x == 0.5f,
		"legacy bridge publishes monochrome program constants without raw shader state");
	rts::render::ResetTrackedLegacyState();
	result |= check(!rts::render::GetTrackedLegacyLogicalState(&logicalState),
		"legacy state invalidation prevents stale neutral state from reaching a draw");
	rts::render::TrackLegacyShaderBits(opaque);
	result |= check(rts::render::GetTrackedLegacyLogicalState(&logicalState) &&
		logicalState.pipeline.pixelProgram ==
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION &&
		logicalState.pipeline.vertexProgram ==
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION &&
		logicalState.texturePresenceMask == 0,
		"legacy state invalidation rebuilds deterministic neutral defaults");
	return result;
}

int testLegacyStatePublicationFailureLatch()
{
	rts::render::ResetTrackedLegacyState();
	int result = 0;
	result |= check(!rts::render::HasLegacyStatePublicationFailure(),
		"legacy state publication failure latch starts clear");
	rts::render::MarkLegacyStatePublicationFailure();
	result |= check(rts::render::HasLegacyStatePublicationFailure(),
		"rejected neutral state publication poisons the current frame");
	rts::render::ResetLegacyStatePublicationFailure();
	result |= check(!rts::render::HasLegacyStatePublicationFailure(),
		"owner reset clears the publication failure for the next frame");
	return result;
}

#if defined(RTS_RENDERER_HAS_D3D11)
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
int testD3D11PrimitiveTopologyTranslation()
{
	int result = 0;
	struct TopologyCase
	{
		unsigned int primitiveType;
		rts::render::RenderPrimitiveTopology expected;
	};
	const TopologyCase supported[] = {
		{ D3DPT_TRIANGLELIST,
			rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST },
		{ D3DPT_TRIANGLESTRIP,
			rts::render::RENDER_PRIMITIVE_TRIANGLE_STRIP },
		{ D3DPT_LINELIST,
			rts::render::RENDER_PRIMITIVE_LINE_LIST },
		{ D3DPT_LINESTRIP,
			rts::render::RENDER_PRIMITIVE_LINE_STRIP }
	};
	for (unsigned int index = 0;
		index < sizeof(supported) / sizeof(supported[0]); ++index)
	{
		rts::render::RenderPrimitiveTopology actual =
			rts::render::RENDER_PRIMITIVE_LINE_LIST;
		result |= check(Try_Translate_D3D8_Primitive_Topology(
			supported[index].primitiveType, &actual) &&
			actual == supported[index].expected,
			"supported D3D8 primitive topology translates exactly");
	}
	const unsigned int unsupported[] = {
		D3DPT_POINTLIST,
		D3DPT_TRIANGLEFAN,
		0xffffffffU
	};
	for (unsigned int index = 0;
		index < sizeof(unsupported) / sizeof(unsupported[0]); ++index)
	{
		rts::render::RenderPrimitiveTopology unchanged =
			rts::render::RENDER_PRIMITIVE_LINE_STRIP;
		result |= check(!Try_Translate_D3D8_Primitive_Topology(
			unsupported[index], &unchanged) &&
			unchanged == rts::render::RENDER_PRIMITIVE_LINE_STRIP,
			unsupported[index] == D3DPT_POINTLIST ?
				"D3D11 snow point-list remains behind the quad fallback" :
				"unsupported D3D8 primitive topology fails closed");
	}
	result |= check(!Try_Translate_D3D8_Primitive_Topology(
		D3DPT_TRIANGLELIST, 0),
		"null primitive topology output is rejected");
	rts::render::ResetLegacyStatePublicationFailure();
	result |= check(Can_Submit_D3D11_Legacy_Draw(),
		"D3D11 legacy draw gate accepts a clean neutral state");
	rts::render::MarkLegacyStatePublicationFailure();
	result |= check(!Can_Submit_D3D11_Legacy_Draw(),
		"rejected texture stage state blocks a D3D11 draw with stale state");
	rts::render::ResetLegacyStatePublicationFailure();
	return result;
}
#endif

int testD3D11LegacyStateBoundary()
{
	int result = 0;
	result |= check(rts::render::Is_D3D11_Local_Viewer_Value_Supported(FALSE) &&
		!rts::render::Is_D3D11_Local_Viewer_Value_Supported(TRUE),
		"D3D11 accepts only the legacy default non-local viewer mode");
	result |= check(rts::render::Is_D3D11_Patch_Segments_Value_Supported(
		0x3f800000U) &&
		!rts::render::Is_D3D11_Patch_Segments_Value_Supported(0x40000000U),
		"D3D11 accepts only neutral one-segment patch state");
	result |= check(rts::render::Is_D3D11_Clip_Plane_Mask_Supported(0x3fU) &&
		!rts::render::Is_D3D11_Clip_Plane_Mask_Supported(0x40U),
		"D3D11 clip-plane publication preserves the six-plane boundary");
	result |= check(rts::render::Is_D3D11_Shade_Mode_Value_Supported(
		rts::render::LEGACY_VOLUMETRIC_SHADOW_SHADE_GOURAUD) &&
		!rts::render::Is_D3D11_Shade_Mode_Value_Supported(
			rts::render::LEGACY_VOLUMETRIC_SHADOW_SHADE_FLAT),
		"D3D11 keeps Gouraud shading and rejects unrepresented flat shading");
	result |= check(
		rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_COLORVERTEX, TRUE) &&
		!rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_COLORVERTEX, FALSE) &&
		rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_DITHERENABLE, FALSE) &&
		!rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_DITHERENABLE, TRUE) &&
		rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_CLIPPING, TRUE) &&
		!rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_CLIPPING, FALSE) &&
		rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_SOFTWAREVERTEXPROCESSING, FALSE) &&
		!rts::render::Is_D3D11_Default_Render_State_Value_Supported(
			rts::render::LEGACY_D3DRS_SOFTWAREVERTEXPROCESSING, TRUE),
		"D3D11 accepts only the exact harmless legacy default-state values");
	result |= check(
		rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_FOGTABLEMODE) &&
		!rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_COLORVERTEX) &&
		!rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_FOGTABLEMODE, false) &&
		rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_COLORVERTEX, false),
		"D3D8 fog and color-vertex numeric states cannot alias in the allowlist");
	result |= check(
		rts::render::Select_D3D11_Volumetric_Shadow_Shade_Mode(false, true) ==
			rts::render::LEGACY_VOLUMETRIC_SHADOW_SHADE_FLAT &&
		rts::render::Select_D3D11_Volumetric_Shadow_Shade_Mode(true, true) ==
			rts::render::LEGACY_VOLUMETRIC_SHADOW_SHADE_GOURAUD &&
		rts::render::Select_D3D11_Volumetric_Shadow_Shade_Mode(true, false) ==
			rts::render::LEGACY_VOLUMETRIC_SHADOW_SHADE_FLAT,
		"volumetric shadows select Gouraud only for a proven D3D11-equivalent pass");

	rts::render::ResetTrackedLegacyState();
	rts::render::LegacyLogicalState state;
	state.pipeline.clipPlaneEnableMask = 1U;
	rts::render::TrackLegacyPipelineState(state.pipeline);
	const float plane[4] = { 0.0f, 0.0f, 1.0f, -4.0f };
	result |= check(rts::render::TrackLegacyClipPlane(0, plane) &&
		!rts::render::TrackLegacyClipPlane(
			rts::render::LEGACY_CLIP_PLANE_COUNT, plane) &&
		!rts::render::TrackLegacyClipPlane(0, 0),
		"clip-plane routing rejects invalid slots and null equations");
	rts::render::LegacyLogicalState tracked;
	result |= check(rts::render::GetTrackedLegacyLogicalState(&tracked) &&
		tracked.pipeline.clipPlaneEnableMask == 1U &&
		tracked.constants.clipPlanes[0].z == 1.0f &&
		tracked.constants.clipPlanes[0].w == -4.0f,
		"clip-plane routing preserves the exact equation and enable mask");
	rts::render::ResetTrackedLegacyState();
	return result;
}

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
	rts::render::RenderBackBufferInfo backBufferInfo;
	result |= check(device->getBackBufferInfo(&backBufferInfo) ==
		rts::render::RENDER_RESULT_OK && backBufferInfo.width == 64 &&
		backBufferInfo.height == 64 &&
		backBufferInfo.format == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM,
		"D3D11 back-buffer info reports the actual swap-chain texture");

		struct TestVertex
		{
			float x;
			float y;
			float z;
			unsigned int color;
		};
		const TestVertex vertices[3] = {
			{ -0.8f, -0.8f, 0.0f, 0xff0000ffU },
			{ 0.8f, -0.8f, 0.0f, 0xff0000ffU },
			{ 0.0f, 0.8f, 0.0f, 0xff0000ffU }
		};
		rts::render::BufferDescriptor vertexDescriptor;
		vertexDescriptor.byteCount = sizeof(vertices);
		vertexDescriptor.stride = sizeof(TestVertex);
		rts::render::GpuHandle vertexBuffer;
		result |= check(device->createBuffer(vertexDescriptor, vertices,
			sizeof(vertices), &vertexBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates an immutable vertex buffer");
		rts::render::BufferDescriptor resizeRecoveryDescriptor;
		resizeRecoveryDescriptor.byteCount = 16;
		resizeRecoveryDescriptor.stride = 16;
		resizeRecoveryDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
		rts::render::GpuHandle resizeRecoveryBuffer;
		result |= check(device->createBuffer(resizeRecoveryDescriptor, 0, 0,
			&resizeRecoveryBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 resize recovery probe creates a logical dynamic buffer");
		const TestVertex greenVertices[3] = {
			{ -0.8f, -0.8f, 0.0f, 0xff00ff00U },
			{ 0.8f, -0.8f, 0.0f, 0xff00ff00U },
			{ 0.0f, 0.8f, 0.0f, 0xff00ff00U }
		};
		rts::render::GpuHandle greenVertexBuffer;
		result |= check(device->createBuffer(vertexDescriptor, greenVertices,
			sizeof(greenVertices), &greenVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a contrasting green vertex buffer");
		const TestVertex halfAlphaVertices[3] = {
			{ -0.8f, -0.8f, 0.0f, 0x800000ffU },
			{ 0.8f, -0.8f, 0.0f, 0x800000ffU },
			{ 0.0f, 0.8f, 0.0f, 0x800000ffU }
		};
		rts::render::GpuHandle halfAlphaVertexBuffer;
		result |= check(device->createBuffer(vertexDescriptor, halfAlphaVertices,
			sizeof(halfAlphaVertices), &halfAlphaVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a half-alpha vertex buffer");
		const TestVertex redVertices[3] = {
			{ -0.8f, -0.8f, 0.0f, 0xffff0000U },
			{ 0.8f, -0.8f, 0.0f, 0xffff0000U },
			{ 0.0f, 0.8f, 0.0f, 0xffff0000U }
		};
		rts::render::GpuHandle redVertexBuffer;
		result |= check(device->createBuffer(vertexDescriptor, redVertices,
			sizeof(redVertices), &redVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a red vertex buffer");

		rts::render::IRenderContext *context = device->immediateContext();
		rts::render::LegacyLogicalState logicalState;
		rts::render::LegacyLogicalState counterClockwiseState = logicalState;
		counterClockwiseState.pipeline.rasterizer.frontCounterClockwise = true;
		logicalState.pipeline.rasterizer.cullMode = rts::render::RENDER_CULL_NONE;
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
		rts::render::TextureDescriptor copiedColorDescriptor;
		copiedColorDescriptor.width = 64;
		copiedColorDescriptor.height = 64;
		copiedColorDescriptor.format =
			rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		copiedColorDescriptor.binding =
			rts::render::RENDER_TEXTURE_RENDER_TARGET |
			rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
		copiedColorDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		rts::render::GpuHandle copiedColor;
		rts::render::TextureDescriptor wrongSizeColorDescriptor =
			copiedColorDescriptor;
		wrongSizeColorDescriptor.width = 32;
		wrongSizeColorDescriptor.height = 32;
		rts::render::GpuHandle wrongSizeColor;
		rts::render::TextureDescriptor wrongFormatColorDescriptor =
			copiedColorDescriptor;
		wrongFormatColorDescriptor.format =
			rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
		rts::render::GpuHandle wrongFormatColor;
		rts::render::TextureDescriptor offscreenCopyColorDescriptor =
			offscreenColorDescriptor;
		rts::render::GpuHandle offscreenCopyColor;
		rts::render::GpuHandle staleColor;
		result |= check(device->createTexture(offscreenColorDescriptor, 0, 0,
			&offscreenColor) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(offscreenDepthDescriptor, 0, 0,
				&offscreenDepth) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(copiedColorDescriptor, 0, 0,
				&copiedColor) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(wrongSizeColorDescriptor, 0, 0,
				&wrongSizeColor) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(wrongFormatColorDescriptor, 0, 0,
				&wrongFormatColor) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(offscreenCopyColorDescriptor, 0, 0,
				&offscreenCopyColor) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(copiedColorDescriptor, 0, 0,
				&staleColor) == rts::render::RENDER_RESULT_OK &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(offscreenColor, offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(offscreenColor) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setTexture(0, offscreenColor) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->clear(rts::render::RenderFloat4(0.25f, 0.5f, 0.75f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(offscreenCopyColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(rts::render::GpuHandle(), offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(), 1.0f, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(rts::render::GpuHandle(),
				rts::render::GpuHandle()) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, offscreenColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(offscreenColor, offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, offscreenColor) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setRenderTargets(rts::render::GpuHandle(),
				rts::render::GpuHandle()) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline invalidates SRV caches across target hazards");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(wrongSizeColor, offscreenDepth) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setRenderTargets(offscreenColor, offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 rejects mismatched color/depth attachment dimensions before binding");
		rts::render::RenderTargetBinding colorOnlyTarget;
		colorOnlyTarget.useBackBufferColor = false;
		colorOnlyTarget.useBackBufferDepth = false;
		colorOnlyTarget.hasColor = true;
		colorOnlyTarget.hasDepth = false;
		colorOnlyTarget.color.resource = offscreenColor;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(colorOnlyTarget) ==
				rts::render::RENDER_RESULT_OK &&
			context->clearTargets(rts::render::RENDER_CLEAR_COLOR,
				rts::render::RenderFloat4(1.0f, 1.0f, 1.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(rts::render::RenderTargetBinding()) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 accepts a differently sized color-only projected-shadow target");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(copiedColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 copies the active color target into a compatible texture");
		result |= check(device->copyActiveColorTargetToTexture(copiedColor) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"D3D11 active-target copy requires an open render frame");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, copiedColor) ==
				rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(copiedColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, copiedColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 active-target copy clears a destination SRV hazard");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(wrongSizeColor) ==
				rts::render::RENDER_RESULT_UNSUPPORTED &&
			device->copyActiveColorTargetToTexture(wrongFormatColor) ==
				rts::render::RENDER_RESULT_UNSUPPORTED &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 active-target copy rejects incompatible dimensions and formats");
		const rts::render::GpuHandle staleColorHandle = staleColor;
		result |= check(device->destroyResource(staleColor) &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(staleColorHandle) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 active-target copy rejects a stale destination handle");
		std::vector<unsigned char> defaultViewportPixels(64 * 64 * 4);
		rts::render::RenderFormat defaultViewportFormat =
			rts::render::RENDER_FORMAT_UNKNOWN;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(counterClockwiseState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&defaultViewportPixels[0],
				defaultViewportPixels.size(), 64 * 4, &defaultViewportFormat) ==
				rts::render::RENDER_RESULT_OK &&
			defaultViewportPixels[4 * (32 * 64 + 32) + 0] > 240,
			"D3D11 swap-chain initialization publishes a full-target default viewport");
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
		center[0] > 240 && center[2] < 16,
		"captured D3D11 triangle preserves clear and vertex colors");
	logicalState.pipeline.clipPlaneEnableMask = 1U;
	logicalState.constants.clipPlanes[0] =
		rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, -0.2f);
	result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
		context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
		context->setLegacyState(logicalState,
			rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
			rts::render::RENDER_RESULT_OK &&
		context->setVertexBuffer(redVertexBuffer, sizeof(TestVertex), 0) ==
			rts::render::RENDER_RESULT_OK &&
		context->setPrimitiveTopology(
			rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			rts::render::RENDER_RESULT_OK &&
		context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
		context->endFrame() == rts::render::RENDER_RESULT_OK &&
		device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
			&captureFormat) == rts::render::RENDER_RESULT_OK,
		"D3D11 clip-plane state binds and submits a clipped triangle");
	center = &pixels[4 * (32 * 64 + 32)];
	const unsigned char *right = &pixels[4 * (32 * 64 + 40)];
	result |= check(center[0] > 240 && center[2] < 16 &&
		right[2] > 240,
		"D3D11 clip-plane distance removes the rejected side only");
	logicalState.pipeline.clipPlaneEnableMask = 0;
	logicalState.constants.clipPlanes[0] =
		rts::render::RenderFloat4(0.0f, 0.0f, 0.0f, 1.0f);

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
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex),
				sizeof(TestVertex)) == rts::render::RENDER_RESULT_OK &&
			context->draw(2, 0) == rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->draw(0xffffffffU, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->draw(1, 0xffffffffU) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setIndexBuffer(indexBuffer,
				rts::render::RENDER_FORMAT_R16_UINT, sizeof(indices[0])) ==
				rts::render::RENDER_RESULT_OK &&
			context->drawIndexed(2, 0, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->drawIndexed(3, 0, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->drawIndexed(0xffffffffU, 0, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->drawIndexed(1, 0xffffffffU, 0) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setIndexBuffer(indexBuffer,
				rts::render::RENDER_FORMAT_R16_UINT, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 draw submission rejects vertex and index ranges outside bound buffers");
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
		result |= check(center[0] > 240 && center[2] < 16,
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
			shiftedCenter[0] > 240 && shiftedCenter[2] < 16,
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

		// Keep the triangle centered while translating view space sideways. Z fog
		// sees zero depth; range fog must see the non-zero viewer distance.
		logicalState.constants.fog.start = 0.0f;
		logicalState.constants.fog.end = 0.25f;
		logicalState.constants.view.values[12] = 0.5f;
		logicalState.constants.projection.values[12] = -0.5f;
		logicalState.pipeline.rangeFogEnable = false;
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
			"D3D11 z fog preserves zero-depth geometry");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16,
			"z fog uses camera-space z instead of viewer distance");
		logicalState.pipeline.rangeFogEnable = true;
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
			"D3D11 range fog uses viewer distance");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16,
			"range fog reaches the configured color away from the view axis");
		logicalState.pipeline.rangeFogEnable = false;
		logicalState.constants.view.values[12] = 0.0f;
		logicalState.constants.projection.values[12] = 0.0f;
		logicalState.constants.fog.start = -1.0f;
		logicalState.constants.fog.end = 0.0f;
		// Fog blends RGB only.  Preserve a half-alpha source so the blend stage
		// can distinguish the fixed-function fog result from an alpha blend with
		// fog color alpha zero.
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 0.0f);
		logicalState.pipeline.blend.blendEnable = true;
		logicalState.pipeline.blend.sourceColor =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationColor =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		logicalState.pipeline.blend.sourceAlpha =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationAlpha =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(halfAlphaVertexBuffer,
				sizeof(TestVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline preserves source alpha through fog");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 80 && center[1] > 80 && center[2] < 16,
			"fog RGB blending keeps the half-alpha source available to blending");
		logicalState.pipeline.blend = rts::render::LegacyBlendState();
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 1.0f);
		// FOG_SCALE_FRAGMENT is intentionally asymmetric with regular fog: the
		// visibility scalar multiplies the fragment and the fixed-function fog
		// color is black.  A non-black configured fog color catches the common
		// translation error of treating this mode as ordinary fog blending.
		logicalState.constants.view.values[14] = 0.5f;
		logicalState.constants.fog.start = 0.0f;
		logicalState.constants.fog.end = 1.0f;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_SCALE_FRAGMENT;
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
			"D3D11 parity pipeline applies legacy scale-fragment fog");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 100 && center[0] < 160 &&
			center[1] < 16 && center[2] < 16,
			"scale-fragment fog multiplies the fragment without injecting fog color");
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_WHITE;
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
			"D3D11 parity pipeline applies legacy white fog");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 &&
			center[1] > 100 && center[1] < 160 &&
			center[2] > 100 && center[2] < 160,
			"white fog replaces the configured fog color with white");
		logicalState.constants.view.values[14] = 0.0f;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_DISABLED;
		logicalState.constants.fog.enabled = false;

		struct TexturedVertex
		{
			float position[3];
			float normal[3];
			unsigned int color;
			float texture[2];
		};
		rts::render::LegacyVertexLayout texturedLayout;
		texturedLayout.stride = sizeof(TexturedVertex);
		texturedLayout.elementCount = 4;
		texturedLayout.elements[0].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
		texturedLayout.elements[0].semanticIndex = 0;
		texturedLayout.elements[0].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		texturedLayout.elements[0].byteOffset = 0;
		texturedLayout.elements[1].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_NORMAL;
		texturedLayout.elements[1].semanticIndex = 0;
		texturedLayout.elements[1].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		texturedLayout.elements[1].byteOffset = 12;
		texturedLayout.elements[2].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
		texturedLayout.elements[2].semanticIndex = 0;
		texturedLayout.elements[2].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		texturedLayout.elements[2].byteOffset = 24;
		texturedLayout.elements[3].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		texturedLayout.elements[3].semanticIndex = 0;
		texturedLayout.elements[3].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT2;
		texturedLayout.elements[3].byteOffset = 28;
		const TexturedVertex texturedVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 0.0f, 1.0f } },
			{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 0.5f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 1.0f, 1.0f } }
		};
		const TexturedVertex treeVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 1.0f, 0.0f },
				0xffffffffU, { 0.0f, 1.0f } },
			{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 1.0f, 0.0f },
				0xffffffffU, { 0.5f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 1.0f, 0.0f },
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
		rts::render::GpuHandle treeVertexBuffer;
		result |= check(device->createBuffer(texturedVertexDescriptor,
			treeVertices, sizeof(treeVertices), &treeVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a physical tree vertex buffer");
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
		// Texture type masks are packed into the legacy constant buffer.  Keep
		// deliberately contrasting resources here so a first draw can expose a
		// state-before-texture publication, rather than only proving that the SRV
		// binding itself succeeded.
		const unsigned int typeProbeCubePixels[6] = {
			0xff0000ffU, 0xff0000ffU, 0xff0000ffU,
			0xff0000ffU, 0xff0000ffU, 0xff0000ffU
		};
		rts::render::TextureSubresourceData typeProbeCubeData[6];
		for (unsigned int face = 0; face < 6; ++face)
		{
			typeProbeCubeData[face].data = &typeProbeCubePixels[face];
			typeProbeCubeData[face].rowPitch = sizeof(unsigned int);
			typeProbeCubeData[face].slicePitch = sizeof(unsigned int);
		}
		rts::render::TextureDescriptor typeProbeCubeDescriptor = textureDescriptor;
		typeProbeCubeDescriptor.width = 1;
		typeProbeCubeDescriptor.height = 1;
		typeProbeCubeDescriptor.arrayCount = 6;
		typeProbeCubeDescriptor.dimension = rts::render::RENDER_TEXTURE_CUBE;
		rts::render::GpuHandle typeProbeCubeTexture;
		const unsigned int typeProbeGradientPixels[2] = {
			0xff0000ffU, 0xffff0000U
		};
		rts::render::TextureDescriptor typeProbeGradientDescriptor = textureDescriptor;
		typeProbeGradientDescriptor.width = 2;
		typeProbeGradientDescriptor.height = 1;
		rts::render::TextureSubresourceData typeProbeGradientData;
		typeProbeGradientData.data = typeProbeGradientPixels;
		typeProbeGradientData.rowPitch = 2 * sizeof(unsigned int);
		typeProbeGradientData.slicePitch = sizeof(typeProbeGradientPixels);
		rts::render::GpuHandle typeProbeGradientTexture;
		const unsigned char typeProbeSignedPixels[2] = { 0xc0, 0x00 };
		rts::render::TextureDescriptor typeProbeSignedDescriptor = textureDescriptor;
		typeProbeSignedDescriptor.width = 1;
		typeProbeSignedDescriptor.height = 1;
		typeProbeSignedDescriptor.format = rts::render::RENDER_FORMAT_R8G8_SNORM;
		rts::render::TextureSubresourceData typeProbeSignedData;
		typeProbeSignedData.data = typeProbeSignedPixels;
		typeProbeSignedData.rowPitch = 2;
		typeProbeSignedData.slicePitch = sizeof(typeProbeSignedPixels);
		rts::render::GpuHandle typeProbeSignedTexture;
		const unsigned int typeProbeUnsignedPixels[1] = { 0xff000040U };
		rts::render::TextureDescriptor typeProbeUnsignedDescriptor = textureDescriptor;
		typeProbeUnsignedDescriptor.width = 1;
		typeProbeUnsignedDescriptor.height = 1;
		rts::render::TextureSubresourceData typeProbeUnsignedData;
		typeProbeUnsignedData.data = typeProbeUnsignedPixels;
		typeProbeUnsignedData.rowPitch = sizeof(unsigned int);
		typeProbeUnsignedData.slicePitch = sizeof(typeProbeUnsignedPixels);
		rts::render::GpuHandle typeProbeUnsignedTexture;
		const bool typeProbeResourcesCreated =
			device->createTexture(typeProbeCubeDescriptor, typeProbeCubeData, 6,
				&typeProbeCubeTexture) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(typeProbeGradientDescriptor, &typeProbeGradientData,
				1, &typeProbeGradientTexture) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(typeProbeSignedDescriptor, &typeProbeSignedData, 1,
				&typeProbeSignedTexture) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(typeProbeUnsignedDescriptor, &typeProbeUnsignedData,
				1, &typeProbeUnsignedTexture) == rts::render::RENDER_RESULT_OK;
		result |= check(typeProbeResourcesCreated,
			"D3D11 creates contrasting cube, signed, and unsigned texture probes");
		const unsigned int treeShroudPixels[4] = {
			0xff000000U, 0xffffffffU, 0xffffffffU, 0xffffffffU
		};
		rts::render::TextureSubresourceData treeShroudData;
		treeShroudData.data = treeShroudPixels;
		treeShroudData.rowPitch = 2 * sizeof(unsigned int);
		treeShroudData.slicePitch = sizeof(treeShroudPixels);
		rts::render::GpuHandle treeShroudTexture;
		result |= check(device->createTexture(textureDescriptor, &treeShroudData,
			1, &treeShroudTexture) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a tree shroud texture");
		const unsigned int riverEdgePixels[4] = {
			0x80000000U, 0x80000000U, 0x80000000U, 0x80000000U
		};
		rts::render::TextureSubresourceData riverEdgeData;
		riverEdgeData.data = riverEdgePixels;
		riverEdgeData.rowPitch = 2 * sizeof(unsigned int);
		riverEdgeData.slicePitch = sizeof(riverEdgePixels);
		rts::render::GpuHandle riverEdgeTexture;
		result |= check(device->createTexture(textureDescriptor, &riverEdgeData,
			1, &riverEdgeTexture) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a half-alpha river edge texture");
		const TexturedVertex waterVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0x8000ff00U, { 0.0f, 1.0f } },
			{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0x8000ff00U, { 0.5f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0x8000ff00U, { 1.0f, 1.0f } }
		};
		rts::render::BufferDescriptor waterVertexDescriptor;
		waterVertexDescriptor.byteCount = sizeof(waterVertices);
		waterVertexDescriptor.stride = sizeof(TexturedVertex);
		rts::render::GpuHandle waterVertexBuffer;
	result |= check(device->createBuffer(waterVertexDescriptor, waterVertices,
		sizeof(waterVertices), &waterVertexBuffer) ==
		rts::render::RENDER_RESULT_OK,
		"D3D11 parity probe creates a half-alpha water vertex buffer");
	// The compact enum does not carry offsets for COLOR1 or TEXCOORD1..7.
	// It must not resurrect the old inferred layout and read arbitrary bytes.
	const bool ambiguousFrameStarted = context->beginFrame() ==
		rts::render::RENDER_RESULT_OK;
	rts::render::RenderResult ambiguousStateResult =
		rts::render::RENDER_RESULT_FAILED;
	if (ambiguousFrameStarted)
	{
		ambiguousStateResult = context->setLegacyState(logicalState,
			rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1);
		context->endFrame();
	}
	result |= check(ambiguousFrameStarted &&
		ambiguousStateResult == rts::render::RENDER_RESULT_UNSUPPORTED,
		"D3D11 rejects ambiguous compact textured state without a vertex layout");
	logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 0.0f);
		logicalState.constants.fog.start = -1.0f;
		logicalState.constants.fog.end = 0.0f;
		logicalState.pipeline.blend.blendEnable = true;
		logicalState.pipeline.blend.sourceColor =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationColor =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		logicalState.pipeline.blend.sourceAlpha =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationAlpha =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 0x0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(waterVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(2, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(3, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"water pixel programs run through the shared fog and blend path");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 80 && center[1] < 80 && center[2] > 80,
			"water fog preserves RGB-only fogging and source alpha");
		logicalState.pipeline.alphaTestEnable = true;
		logicalState.pipeline.alphaFunction = rts::render::RENDER_COMPARE_GREATER;
		logicalState.pipeline.alphaReference = 255;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 0x0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(waterVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(2, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(3, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"water pixel programs apply the legacy alpha-test state");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"water alpha-test rejection leaves the clear target untouched");
		logicalState.pipeline.alphaTestEnable = false;
		logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f);
		logicalState.constants.fog.start = 0.0f;
		logicalState.constants.fog.end = 1.0f;
		logicalState.constants.view.values[14] = 0.5f;
		logicalState.pipeline.blend.blendEnable = true;
		logicalState.pipeline.blend.sourceColor =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationColor =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		logicalState.pipeline.blend.sourceAlpha =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationAlpha =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 0x0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(waterVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(2, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(3, riverEdgeTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"river water pixel program runs through its runtime fog and alpha path");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 80 && center[1] > 32 && center[1] < 100 &&
			center[2] > 32 && center[2] < 100 && center[3] > 160 &&
			center[3] < 200,
			"river water preserves edge alpha while fogging RGB only");
		logicalState.pipeline.alphaTestEnable = true;
		logicalState.pipeline.alphaFunction = rts::render::RENDER_COMPARE_GREATER;
		logicalState.pipeline.alphaReference = 200;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 0x0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(waterVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(2, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(3, riverEdgeTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"river water pixel program applies the legacy alpha test");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"river water alpha-test rejection leaves the clear target untouched");
		logicalState.pipeline.alphaTestEnable = false;
		logicalState.pipeline.blend = rts::render::LegacyBlendState();
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_DISABLED;
		logicalState.constants.fog.enabled = false;
		logicalState.constants.view.values[14] = 0.0f;
		logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
		struct SeaVertex
		{
			float position[3];
			unsigned int diffuse;
			float bumpCoordinate[2];
		};
		const SeaVertex seaVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, 0x80ffffffU, { 0.0f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, 0x80ffffffU, { 0.0f, 0.0f } },
			{ { 0.0f, 0.8f, 0.0f }, 0x80ffffffU, { 0.0f, 0.0f } }
		};
		rts::render::BufferDescriptor seaVertexDescriptor;
		seaVertexDescriptor.byteCount = sizeof(seaVertices);
		seaVertexDescriptor.stride = sizeof(SeaVertex);
		rts::render::GpuHandle seaVertexBuffer;
		result |= check(device->createBuffer(seaVertexDescriptor, seaVertices,
			sizeof(seaVertices), &seaVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a sea-wave vertex buffer");
		const unsigned char seaBumpPixels[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		rts::render::TextureDescriptor seaBumpDescriptor;
		seaBumpDescriptor.width = 2;
		seaBumpDescriptor.height = 2;
		seaBumpDescriptor.format = rts::render::RENDER_FORMAT_R8G8_SNORM;
		rts::render::TextureSubresourceData seaBumpData;
		seaBumpData.data = seaBumpPixels;
		seaBumpData.rowPitch = 4;
		seaBumpData.slicePitch = sizeof(seaBumpPixels);
		rts::render::GpuHandle seaBumpTexture;
		const unsigned int seaReflectionPixels[4] = {
			0xff0000ffU, 0xff0000ffU, 0xff0000ffU, 0xff0000ffU
		};
		rts::render::TextureSubresourceData seaReflectionData;
		seaReflectionData.data = seaReflectionPixels;
		seaReflectionData.rowPitch = 2 * sizeof(unsigned int);
		seaReflectionData.slicePitch = sizeof(seaReflectionPixels);
		rts::render::GpuHandle seaReflectionTexture;
		result |= check(device->createTexture(seaBumpDescriptor,
			&seaBumpData, 1, &seaBumpTexture) == rts::render::RENDER_RESULT_OK &&
			device->createTexture(textureDescriptor, &seaReflectionData, 1,
				&seaReflectionTexture) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates sea bump and reflection textures");
		rts::render::LegacyVertexLayout seaLayout;
		seaLayout.stride = sizeof(SeaVertex);
		seaLayout.elementCount = 3;
		seaLayout.elements[0].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
		seaLayout.elements[0].format = rts::render::RENDER_VERTEX_DATA_FLOAT3;
		seaLayout.elements[0].byteOffset = 0;
		seaLayout.elements[1].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
		seaLayout.elements[1].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		seaLayout.elements[1].byteOffset = 12;
		seaLayout.elements[2].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		seaLayout.elements[2].semanticIndex = 0;
		seaLayout.elements[2].format = rts::render::RENDER_VERTEX_DATA_FLOAT2;
		seaLayout.elements[2].byteOffset = 16;
		logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_WATER_SEA;
		logicalState.pipeline.vertexProgram =
			rts::render::RENDER_LEGACY_VERTEX_WATER_SEA;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 0.0f);
		logicalState.constants.fog.start = -1.0f;
		logicalState.constants.fog.end = 0.0f;
		logicalState.pipeline.blend.blendEnable = true;
		logicalState.pipeline.blend.sourceColor =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationColor =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		logicalState.pipeline.blend.sourceAlpha =
			rts::render::RENDER_BLEND_SOURCE_ALPHA;
		logicalState.pipeline.blend.destinationAlpha =
			rts::render::RENDER_BLEND_INVERSE_SOURCE_ALPHA;
		logicalState.constants.vertexShaderConstants[2] =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[3] =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[4] =
			rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[5] =
			rts::render::RenderFloat4(0.0f, 0.0f, 0.0f, 1.0f);
		logicalState.constants.vertexShaderConstants[6] =
			rts::render::RenderFloat4(1.0f, 1.0f, 0.0f, 0.0f);
		// c7-c10 carry the per-sea-patch world/view matrix.  Keep the first
		// draw at zero view-space depth, then move only this patch transform so
		// a shader that still reads the global WorldView produces no change.
		logicalState.constants.vertexShaderConstants[7] =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[8] =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[9] =
			rts::render::RenderFloat4(0.0f, 0.0f, 1.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[10] =
			rts::render::RenderFloat4(0.0f, 0.0f, 0.0f, 1.0f);
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, seaLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(seaVertexBuffer, sizeof(SeaVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, seaBumpTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, seaReflectionTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"sea-wave pixel programs run through the shared fog and blend path");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 80 && center[2] < 80 && center[0] > 80,
			"sea-wave fog preserves RGB-only fogging and source alpha");
		logicalState.constants.fog.start = 0.0f;
		logicalState.constants.fog.end = 1.0f;
		logicalState.constants.vertexShaderConstants[9].w = 0.5f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, seaLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(seaVertexBuffer, sizeof(SeaVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, seaBumpTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, seaReflectionTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"sea-wave draw accepts a translated per-patch view transform");
		center = &pixels[4 * (32 * 64 + 32)];
		const unsigned char seaPatchFogPixel[4] = {
			center[0], center[1], center[2], center[3]
		};
		logicalState.constants.vertexShaderConstants[9].w = 0.0f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, seaLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(seaVertexBuffer, sizeof(SeaVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, seaBumpTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, seaReflectionTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"sea-wave draw accepts the unshifted per-patch view transform");
		center = &pixels[4 * (32 * 64 + 32)];
		const int seaPatchFogDifference =
			(static_cast<int>(center[0]) - seaPatchFogPixel[0] < 0 ?
				static_cast<int>(seaPatchFogPixel[0]) - center[0] :
				static_cast<int>(center[0]) - seaPatchFogPixel[0]) +
			(static_cast<int>(center[1]) - seaPatchFogPixel[1] < 0 ?
				static_cast<int>(seaPatchFogPixel[1]) - center[1] :
				static_cast<int>(center[1]) - seaPatchFogPixel[1]) +
			(static_cast<int>(center[2]) - seaPatchFogPixel[2] < 0 ?
				static_cast<int>(seaPatchFogPixel[2]) - center[2] :
				static_cast<int>(center[2]) - seaPatchFogPixel[2]);
		result |= check(seaPatchFogDifference > 40,
			"sea-wave fog depth follows the per-patch view transform");
		logicalState.pipeline.alphaTestEnable = true;
		logicalState.pipeline.alphaFunction = rts::render::RENDER_COMPARE_GREATER;
		logicalState.pipeline.alphaReference = 255;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, seaLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(seaVertexBuffer, sizeof(SeaVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, seaBumpTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, seaReflectionTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"sea-wave pixel programs apply the legacy alpha-test state");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"sea-wave alpha-test rejection leaves the clear target untouched");
		logicalState.pipeline.alphaTestEnable = false;
		logicalState.pipeline.blend = rts::render::LegacyBlendState();
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 1.0f);
		logicalState.constants.fog.start = 0.0f;
		logicalState.constants.fog.end = 0.25f;
		logicalState.constants.vertexShaderConstants[7].w = 0.5f;
		logicalState.pipeline.rangeFogEnable = false;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, seaLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(seaVertexBuffer, sizeof(SeaVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, seaBumpTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, seaReflectionTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"sea-wave z fog preserves off-axis zero-depth color");
		center = &pixels[4 * (32 * 64 + 32)];
		const unsigned char seaZFogGreen = center[1];
		logicalState.pipeline.rangeFogEnable = true;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, seaLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(seaVertexBuffer, sizeof(SeaVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, seaBumpTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, seaReflectionTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"sea-wave range fog uses off-axis per-patch viewer distance");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[1] > seaZFogGreen + 100,
			"sea-wave range fog reaches its color away from the view axis");
		logicalState.pipeline.rangeFogEnable = false;
		logicalState.constants.vertexShaderConstants[7].w = 0.0f;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_DISABLED;
		logicalState.constants.fog.enabled = false;
		logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
		logicalState.pipeline.vertexProgram =
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION;
		std::vector<unsigned int> targetPixels(16 * 16, 0xff00ff00U);
		rts::render::TextureSubresourceData targetData;
		targetData.data = &targetPixels[0];
		targetData.rowPitch = 16 * sizeof(unsigned int);
		targetData.slicePitch = targetPixels.size() * sizeof(unsigned int);
		result |= check(device->refreshTexture(offscreenColor,
			offscreenColorDescriptor, &targetData, 1) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 target resource can be initialized before an alias hazard probe");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_MODULATE;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		logicalState.constants.fog.color =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f);
		logicalState.constants.fog.start = 0.0f;
		logicalState.constants.fog.end = 0.25f;
		logicalState.constants.view.values[12] = 0.5f;
		logicalState.constants.projection.values[12] = -0.5f;
		logicalState.pipeline.rangeFogEnable = false;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			"textured z fog preserves zero-depth sampled color");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16,
			"textured z fog uses camera-space z");
		logicalState.pipeline.rangeFogEnable = true;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			"textured range fog uses viewer distance");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] > 240 && center[1] < 16,
			"textured range fog reaches its color away from the view axis");

		// Trees physically carry only TEXCOORD0. Their legacy vertex program
		// generates TEXCOORD1 from position and c32/c33 for the shroud lookup.
		// A black texel at (0,0) catches any regression that consults only the
		// input-layout coordinate mask and discards that generated coordinate.
		logicalState.pipeline.vertexProgram =
			rts::render::RENDER_LEGACY_VERTEX_TREES;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_DISABLED;
		logicalState.pipeline.rangeFogEnable = false;
		logicalState.constants.fog.enabled = false;
		logicalState.constants.vertexShaderConstants[8] =
			rts::render::RenderFloat4();
		logicalState.constants.vertexShaderConstants[32] =
			rts::render::RenderFloat4(1.0f, 1.0f, 0.0f, 0.0f);
		logicalState.constants.vertexShaderConstants[33] =
			rts::render::RenderFloat4(0.5f, 0.5f, 1.0f, 1.0f);
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_MODULATE;
		logicalState.pipeline.textureStages[0].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[0].colorArgument2 =
			rts::render::RENDER_TEXTURE_ARG_DIFFUSE;
		logicalState.pipeline.textureStages[1].colorOperation =
			rts::render::RENDER_TEXTURE_OP_MODULATE;
		logicalState.pipeline.textureStages[1].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[1].colorArgument2 =
			rts::render::RENDER_TEXTURE_ARG_CURRENT;
		logicalState.pipeline.textureStages[1].alphaOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		logicalState.pipeline.textureStages[1].alphaArgument2 =
			rts::render::RENDER_TEXTURE_ARG_CURRENT;
		logicalState.pipeline.textureStages[1].textureCoordinateIndex = 1;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(treeVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, treeShroudTexture) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"tree program samples its generated shroud coordinate");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 128 && center[0] < 32 && center[2] < 32,
			"generated tree TEXCOORD1 preserves the lit foliage color");
		logicalState.pipeline.textureStages[1] =
			rts::render::LegacyTextureStageState();
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
		logicalState.constants.fog.enabled = true;
		// The tree program derives its diffuse intensity from normal.y. These
		// vertices deliberately produce black before fog, making the same
		// off-axis range-fog transition unambiguous without a separate fixture.
		logicalState.pipeline.vertexProgram =
			rts::render::RENDER_LEGACY_VERTEX_TREES;
		logicalState.constants.vertexShaderConstants[8] =
			rts::render::RenderFloat4();
		logicalState.pipeline.rangeFogEnable = false;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			"tree z fog preserves off-axis zero-depth color");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] < 16,
			"tree z fog uses camera-space z");
		logicalState.pipeline.rangeFogEnable = true;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			"tree range fog uses viewer distance");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] > 240 && center[1] < 16,
			"tree range fog reaches its color away from the view axis");
		logicalState.pipeline.vertexProgram =
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION;
		logicalState.pipeline.rangeFogEnable = false;
		logicalState.pipeline.fogMode = rts::render::RENDER_FOG_DISABLED;
		logicalState.constants.fog.enabled = false;
		logicalState.constants.view.values[12] = 0.0f;
		logicalState.constants.projection.values[12] = 0.0f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, offscreenColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(offscreenColor, offscreenDepth) ==
				rts::render::RENDER_RESULT_OK &&
			context->setRenderTargets(rts::render::GpuHandle(),
				rts::render::GpuHandle()) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, offscreenColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 target transitions preserve the shader-resource binding cache");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"D3D11 target transitions rebind an SRV after implicit output unbinding");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
		rts::render::TextureDescriptor refreshableTextureDescriptor =
			textureDescriptor;
		refreshableTextureDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		rts::render::GpuHandle secondTexture;
		result |= check(device->createTexture(refreshableTextureDescriptor,
			&secondTextureData, 1, &secondTexture) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a second texture stage resource");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		logicalState.pipeline.textureStages[0].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[1] =
			rts::render::LegacyTextureStageState();
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, secondTexture) ==
				rts::render::RENDER_RESULT_OK &&
			device->refreshTexture(secondTexture, refreshableTextureDescriptor,
				&secondTextureData, 1) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 texture refresh preserves unrelated shader-resource stages");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"a later-stage refresh leaves the earlier sampled texture visible");

		rts::render::GpuHandle destroyProbeTexture;
		result |= check(device->createTexture(refreshableTextureDescriptor,
			&secondTextureData, 1, &destroyProbeTexture) ==
			rts::render::RENDER_RESULT_OK &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, destroyProbeTexture) ==
				rts::render::RENDER_RESULT_OK &&
			device->destroyResource(destroyProbeTexture) &&
			context->setTexture(1, destroyProbeTexture) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 texture destruction preserves unrelated shader-resource stages");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"destroying a later-stage texture leaves the earlier sample visible");

		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 3) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(1, copiedColor) ==
				rts::render::RENDER_RESULT_OK &&
			device->copyActiveColorTargetToTexture(copiedColor) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 target copies preserve unrelated shader-resource stages");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"copying a later-stage target leaves the earlier sample visible");
		logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_TERRAIN_BASE;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 3) ==
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
			"D3D11 parity pipeline executes the extracted terrain pixel program");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] > 240 && center[0] < 16 && center[1] < 16,
			"terrain diffuse alpha selects the second base texture exactly");
		logicalState.pipeline.pixelProgram =
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
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
			context->setLegacyStateForLayout(logicalState, texturedLayout, 3) ==
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
			const rts::render::RenderResult repeatedLayoutResult =
				context->setLegacyStateForLayout(logicalState, flexibleLayout, 2);
			flexibleDrawCompleted = context->setVertexBuffer(
					flexibleVertexBuffer, sizeof(FlexibleVertex), 0) ==
						rts::render::RENDER_RESULT_OK &&
					repeatedLayoutResult == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(flexibleVertexBuffer,
						sizeof(FlexibleVertex), 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, uvSelectionTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, uvSelectionTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
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
			"D3D11 parity pipeline suppresses duplicate layout and resource binds");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"texture stage one selects UV1 instead of repeating UV0");
		// A layout that declares only TEXCOORD0 must not let a stage requesting
		// TEXCOORD1 read the position bytes synthesized for the absent semantic.
		// Draw at x=52 where the position-derived alias would address texel 1;
		// the specified missing-coordinate default (0,0,0,1) addresses texel 0.
		const FlexibleVertex missingCoordinateVertices[3] = {
			{ { 0.25f, -0.8f, 0.0f }, 0xffffffffU,
				{ 0.0f, 0.0f }, { 0.75f, 0.0f } },
			{ { 0.95f, -0.8f, 0.0f }, 0xffffffffU,
				{ 0.0f, 0.0f }, { 0.75f, 0.0f } },
			{ { 0.60f, 0.8f, 0.0f }, 0xffffffffU,
				{ 0.0f, 0.0f }, { 0.75f, 0.0f } }
		};
		rts::render::BufferDescriptor missingCoordinateDescriptor;
		missingCoordinateDescriptor.byteCount =
			sizeof(missingCoordinateVertices);
		missingCoordinateDescriptor.stride = sizeof(FlexibleVertex);
		rts::render::GpuHandle missingCoordinateBuffer;
		result |= check(device->createBuffer(missingCoordinateDescriptor,
			missingCoordinateVertices, sizeof(missingCoordinateVertices),
			&missingCoordinateBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a vertex buffer with a missing UV semantic");
		rts::render::LegacyVertexLayout missingCoordinateLayout = flexibleLayout;
		missingCoordinateLayout.elementCount = 3;
		const bool missingCoordinateFrameStarted = context->beginFrame() ==
			rts::render::RENDER_RESULT_OK;
		if (missingCoordinateFrameStarted)
		{
			context->clear(clearColor, 1.0f, 0);
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f);
			context->setLegacyStateForLayout(logicalState,
				missingCoordinateLayout, 2);
			context->setVertexBuffer(missingCoordinateBuffer,
				sizeof(FlexibleVertex), 0);
			context->setTexture(1, uvSelectionTexture);
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST);
			context->draw(3, 0);
			context->endFrame();
		}
		result |= check(missingCoordinateFrameStarted &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline accepts a layout with an absent texture coordinate");
		const unsigned char *missingCoordinateCenter =
			&pixels[4 * (32 * 64 + 52)];
		result |= check(missingCoordinateCenter[2] > 240 &&
			missingCoordinateCenter[0] < 16,
			"absent texture coordinates use the fixed-function zero default");
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

		struct PreTransformedVertex
		{
			float position[4];
			unsigned int color;
		};
		const PreTransformedVertex preTransformedVertices[6] = {
			{ { 15.5f, 15.5f, 0.25f, 1.0f }, 0xff00ff00U },
			{ { 47.5f, 15.5f, 0.25f, 1.0f }, 0xff00ff00U },
			{ { 15.5f, 47.5f, 0.25f, 1.0f }, 0xff00ff00U },
			{ { 15.5f, 47.5f, 0.25f, 1.0f }, 0xff00ff00U },
			{ { 47.5f, 15.5f, 0.25f, 1.0f }, 0xff00ff00U },
			{ { 47.5f, 47.5f, 0.25f, 1.0f }, 0xff00ff00U }
		};
		rts::render::BufferDescriptor preTransformedDescriptor;
		preTransformedDescriptor.byteCount = sizeof(preTransformedVertices);
		preTransformedDescriptor.stride = sizeof(PreTransformedVertex);
		rts::render::GpuHandle preTransformedBuffer;
		result |= check(device->createBuffer(preTransformedDescriptor,
			preTransformedVertices, sizeof(preTransformedVertices),
			&preTransformedBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a pre-transformed vertex buffer");
		rts::render::LegacyVertexLayout preTransformedLayout;
		preTransformedLayout.stride = sizeof(PreTransformedVertex);
		preTransformedLayout.elementCount = 2;
		preTransformedLayout.preTransformed = true;
		preTransformedLayout.elements[0].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
		preTransformedLayout.elements[0].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT4;
		preTransformedLayout.elements[0].byteOffset = 0;
		preTransformedLayout.elements[1].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
		preTransformedLayout.elements[1].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		preTransformedLayout.elements[1].byteOffset = 16;
		const bool preTransformedDraw =
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setViewport(16.0f, 16.0f, 32.0f, 32.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState,
				preTransformedLayout, 0) == rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(preTransformedBuffer,
				sizeof(PreTransformedVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(6, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK;
		result |= check(preTransformedDraw,
			"D3D11 parity pipeline accepts D3D8 XYZRHW viewport-space vertices");
		center = &pixels[4 * (32 * 64 + 32)];
		const unsigned char *outside = &pixels[4 * (8 * 64 + 8)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16 &&
			outside[0] > 240 && outside[1] < 16 && outside[2] < 16,
			"XYZRHW conversion preserves viewport offset, extent, and diffuse color");
		// POSITIONT constants include the viewport transform.  Change the native
		// viewport after publishing the same logical state so draw-boundary refresh
		// must update the cached constants without rebinding the pipeline.
		bool viewportRefreshFrameStarted = context->beginFrame() ==
			rts::render::RENDER_RESULT_OK;
		bool viewportRefreshFrameSucceeded = false;
		if (viewportRefreshFrameStarted)
		{
			viewportRefreshFrameSucceeded =
				context->clear(clearColor, 1.0f, 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
					rts::render::RENDER_RESULT_OK &&
				context->setLegacyStateForLayout(logicalState,
					preTransformedLayout, 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setViewport(16.0f, 16.0f, 32.0f, 32.0f, 0.0f, 1.0f) ==
					rts::render::RENDER_RESULT_OK &&
				context->setVertexBuffer(preTransformedBuffer,
					sizeof(PreTransformedVertex), 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setPrimitiveTopology(
					rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
					rts::render::RENDER_RESULT_OK &&
				context->draw(6, 0) == rts::render::RENDER_RESULT_OK &&
				context->setLegacyStateForLayout(logicalState,
					preTransformedLayout, 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->draw(6, 0) == rts::render::RENDER_RESULT_OK;
			const rts::render::RenderResult viewportRefreshEndResult =
				context->endFrame();
			viewportRefreshFrameSucceeded = viewportRefreshFrameSucceeded &&
				viewportRefreshEndResult == rts::render::RENDER_RESULT_OK;
		}
		const bool viewportRefreshCaptured = viewportRefreshFrameSucceeded &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK;
		const unsigned char *viewportRefreshEdge =
			&pixels[4 * (32 * 64 + 20)];
		result |= check(viewportRefreshCaptured && viewportRefreshEdge[1] > 240 &&
			viewportRefreshEdge[0] < 16 && viewportRefreshEdge[2] < 16,
			"draw refreshes POSITIONT constants after a cached viewport change");
		rts::render::LegacyLogicalState preTransformedClipState = logicalState;
		preTransformedClipState.pipeline.clipPlaneEnableMask = 1U;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(preTransformedClipState,
				preTransformedLayout, 0) ==
				rts::render::RENDER_RESULT_UNSUPPORTED &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 rejects world-space clip planes for POSITIONT layouts");
		struct SpecularVertex
		{
			float position[3];
			float normal[3];
			unsigned int diffuse;
			unsigned int specular;
			float texture[2];
		};
		const SpecularVertex specularVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, 1.0f },
				0xffffffffU, 0x00000000U, { 0.0f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, 1.0f },
				0xffffffffU, 0x00000000U, { 0.0f, 0.0f } },
			{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 0.0f, 1.0f },
				0xffffffffU, 0x00000000U, { 0.0f, 0.0f } }
		};
		rts::render::BufferDescriptor specularDescriptor;
		specularDescriptor.byteCount = sizeof(specularVertices);
		specularDescriptor.stride = sizeof(SpecularVertex);
		rts::render::GpuHandle specularVertexBuffer;
		result |= check(device->createBuffer(specularDescriptor,
			specularVertices, sizeof(specularVertices), &specularVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a zero-vertex-specular buffer");
		rts::render::LegacyVertexLayout specularLayout;
		specularLayout.stride = sizeof(SpecularVertex);
		specularLayout.elementCount = 5;
		specularLayout.elements[0].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
		specularLayout.elements[0].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		specularLayout.elements[0].byteOffset = 0;
		specularLayout.elements[1].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_NORMAL;
		specularLayout.elements[1].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		specularLayout.elements[1].byteOffset = 12;
		specularLayout.elements[2].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
		specularLayout.elements[2].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		specularLayout.elements[2].byteOffset = 24;
		specularLayout.elements[3].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_SPECULAR;
		specularLayout.elements[3].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		specularLayout.elements[3].byteOffset = 28;
		specularLayout.elements[4].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		specularLayout.elements[4].semanticIndex = 0;
		specularLayout.elements[4].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT2;
		specularLayout.elements[4].byteOffset = 32;
		logicalState.pipeline.lightingEnable = true;
		logicalState.pipeline.secondaryGradientEnable = true;
		logicalState.pipeline.blend = rts::render::LegacyBlendState();
		// D3D8's global initialization overrides the neutral COLOR2 default
		// before the legacy specular-material probe.  Keep this probe explicit
		// so it remains about generated material specular, not vertex COLOR2.
		logicalState.pipeline.specularMaterialSource =
			rts::render::RENDER_MATERIAL_SOURCE_MATERIAL;
		logicalState.constants.material.ambient = rts::render::RenderFloat4();
		logicalState.constants.material.emissive = rts::render::RenderFloat4();
		logicalState.constants.material.diffuse =
			rts::render::RenderFloat4(1.0f, 1.0f, 1.0f, 1.0f);
		logicalState.constants.material.specular =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f);
		logicalState.constants.material.specularPower = 8.0f;
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
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
		// D3DMCS_DIFFUSEMATERIALSOURCE must replace the material diffuse color;
		// the shader must not multiply that lighting result by COLOR1 again.
		logicalState.pipeline.secondaryGradientEnable = false;
		logicalState.constants.material.specular = rts::render::RenderFloat4();
		logicalState.constants.material.specularPower = 0.0f;
		logicalState.constants.material.diffuse =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f);
		logicalState.pipeline.diffuseMaterialSource =
			rts::render::RENDER_MATERIAL_SOURCE_MATERIAL;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			"D3D11 parity pipeline accepts the material diffuse source selector");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] < 16,
			"D3DMCS_MATERIAL uses the material diffuse color without COLOR1 modulation");
		logicalState.pipeline.diffuseMaterialSource =
			rts::render::RENDER_MATERIAL_SOURCE_COLOR1;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
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
			"D3D11 parity pipeline evaluates the COLOR1 diffuse source selector");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] > 240 && center[2] < 16,
			"D3DMCS_COLOR1 uses the vertex diffuse color as the diffuse material");
		logicalState.pipeline.diffuseMaterialSource =
			rts::render::RENDER_MATERIAL_SOURCE_MATERIAL;
		logicalState.pipeline.secondaryGradientEnable = true;
		logicalState.constants.material.diffuse = rts::render::RenderFloat4();
		logicalState.constants.material.specular =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f);
		logicalState.constants.material.specularPower = 8.0f;
		logicalState.constants.lights[0].diffuse = rts::render::RenderFloat4();
		logicalState.constants.lights[0].specular =
			rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f);
		logicalState.constants.lights[0].direction =
			rts::render::RenderFloat4(0.0f, 0.0f, -1.0f, 0.0f);
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		logicalState.pipeline.textureStages[0].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_SPECULAR;
		logicalState.pipeline.textureStages[0].alphaOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		logicalState.pipeline.textureStages[0].alphaArgument1 =
			rts::render::RENDER_TEXTURE_ARG_SPECULAR;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, specularLayout, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(specularVertexBuffer,
				sizeof(SpecularVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline evaluates secondary-gradient specular lighting");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] > 240 && center[0] < 16 && center[1] < 16,
			"secondary-gradient lighting generates the configured red specular term");
		result |= check(center[3] < 16,
			"generated specular lighting preserves the input specular alpha");
		// Generated fixed-function specular is valid without an input COLOR2
		// element.  The secondary output must therefore not be gated on the
		// vertex layout's specular flag.
		rts::render::LegacyVertexLayout noInputSpecularLayout = specularLayout;
		noInputSpecularLayout.elementCount = 3;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, noInputSpecularLayout, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(specularVertexBuffer,
				sizeof(SpecularVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline generates specular without a vertex COLOR2");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] > 240 && center[0] < 16 && center[1] < 16,
			"generated material specular reaches D3DTA_SPECULAR without COLOR2");
		// COLOR2 must source the vertex specular color instead of the material
		// specular color.  The probe vertices carry zero COLOR2, so this draw
		// must write zero rather than generate the configured red highlight.
		logicalState.pipeline.specularMaterialSource =
			rts::render::RENDER_MATERIAL_SOURCE_COLOR2;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, specularLayout, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(specularVertexBuffer,
				sizeof(SpecularVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline evaluates the COLOR2 specular source selector");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 &&
			center[2] < 16 && center[3] < 16,
			"D3DMCS_COLOR2 uses the vertex specular color for generated lighting");
		logicalState.pipeline.specularMaterialSource =
			rts::render::RENDER_MATERIAL_SOURCE_MATERIAL;
		// Rotate the object around Y so its +Z normal becomes +X.  The light
		// remains +Z in view space; a specular implementation that transforms
		// this already-world-space light by WorldView will incorrectly turn it
		// into +X and produce a bright highlight.
		logicalState.constants.world.setIdentity();
		logicalState.constants.world.values[0] = 0.0f;
		logicalState.constants.world.values[2] = -1.0f;
		logicalState.constants.world.values[8] = 1.0f;
		logicalState.constants.world.values[10] = 0.0f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, specularLayout, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(specularVertexBuffer,
				sizeof(SpecularVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline evaluates specular lighting under an object rotation");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[2] < 16 && center[0] > 240,
			"specular lighting keeps the light vector in view space after world rotation");
		logicalState.constants.world.setIdentity();
		logicalState.pipeline.secondaryGradientEnable = false;
		logicalState.pipeline.lightingEnable = false;
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 parity boundary accepts legacy bump-environment combiners");
		// A no-color pass must still update depth/stencil and retain vertex
		// clipping. Exercise both fixed-function and specialized pixel programs,
		// then restore an alpha-tested no-color cutout immediately after null PS.
		const unsigned int cutoutPixels[2] = { 0x00ffffffU, 0xffffffffU };
		rts::render::TextureDescriptor cutoutDescriptor = textureDescriptor;
		cutoutDescriptor.height = 1;
		rts::render::TextureSubresourceData cutoutData;
		cutoutData.data = cutoutPixels;
		cutoutData.rowPitch = sizeof(cutoutPixels);
		cutoutData.slicePitch = sizeof(cutoutPixels);
		rts::render::GpuHandle cutoutTexture;
		result |= check(device->createTexture(cutoutDescriptor, &cutoutData, 1,
			&cutoutTexture) == rts::render::RENDER_RESULT_OK,
			"D3D11 depth-only probe creates a transparent and opaque cutout texture");
		for (unsigned int depthOnlyCase = 0; depthOnlyCase < 3; ++depthOnlyCase)
		{
			const bool alphaCutout = depthOnlyCase == 2;
			rts::render::LegacyLogicalState depthOnlyState;
			depthOnlyState.pipeline.pixelProgram = depthOnlyCase == 1 ?
				rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT :
				rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
			depthOnlyState.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_NONE;
			depthOnlyState.pipeline.blend.colorWriteMask = 0;
			depthOnlyState.pipeline.depthStencil.stencilEnable = true;
			depthOnlyState.pipeline.depthStencil.stencilReference = 37U;
			depthOnlyState.pipeline.depthStencil.stencilFunction =
				rts::render::RENDER_COMPARE_ALWAYS;
			depthOnlyState.pipeline.depthStencil.stencilPass =
				rts::render::RENDER_STENCIL_REPLACE;
			depthOnlyState.pipeline.clipPlaneEnableMask = 1U;
			depthOnlyState.constants.clipPlanes[0] = rts::render::RenderFloat4(
				1.0f, 0.0f, 0.0f, alphaCutout ? -2.0f : 0.0f);
			depthOnlyState.pipeline.textureStages[0].colorOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			depthOnlyState.pipeline.textureStages[0].colorArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			depthOnlyState.pipeline.textureStages[0].alphaOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			depthOnlyState.pipeline.textureStages[0].alphaArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			depthOnlyState.pipeline.textureStages[0].sampler.minification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			depthOnlyState.pipeline.textureStages[0].sampler.magnification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			const unsigned int depthOnlyTextureMask = alphaCutout ? 1U : 0U;
			result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
				context->clear(clearColor, 1.0f, 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
					rts::render::RENDER_RESULT_OK &&
				context->setTexture(0, alphaCutout ? cutoutTexture :
					rts::render::GpuHandle()) == rts::render::RENDER_RESULT_OK &&
				context->setLegacyStateForLayout(depthOnlyState, texturedLayout,
					depthOnlyTextureMask) == rts::render::RENDER_RESULT_OK &&
				context->setVertexBuffer(texturedVertexBuffer,
					sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
				context->setPrimitiveTopology(
					rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
					rts::render::RENDER_RESULT_OK &&
				context->draw(3, 0) == rts::render::RENDER_RESULT_OK,
				"D3D11 no-color pass retains vertex clipping and depth/stencil writes");
			if (alphaCutout)
			{
				// The first, entirely clipped draw bound null PS. Restoring alpha
				// testing must execute the texture sample/discard even without color.
				depthOnlyState.pipeline.clipPlaneEnableMask = 0;
				depthOnlyState.pipeline.alphaTestEnable = true;
				depthOnlyState.pipeline.alphaFunction =
					rts::render::RENDER_COMPARE_GREATER;
				depthOnlyState.pipeline.alphaReference = 127U;
				result |= check(context->setLegacyStateForLayout(depthOnlyState,
						texturedLayout, 1) == rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(depthOnlyState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK,
					"D3D11 alpha-tested no-color cutout restores PS and cached state");
			}
			rts::render::LegacyLogicalState revealDepthState;
			revealDepthState.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_NONE;
			revealDepthState.pipeline.depthStencil.depthFunction =
				rts::render::RENDER_COMPARE_LESS;
			result |= check(context->setTexture(0, rts::render::GpuHandle()) ==
					rts::render::RENDER_RESULT_OK &&
				context->setLegacyState(revealDepthState,
					rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setVertexBuffer(greenVertexBuffer, sizeof(TestVertex), 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
				context->endFrame() == rts::render::RENDER_RESULT_OK &&
				device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
					&captureFormat) == rts::render::RENDER_RESULT_OK,
				"D3D11 color pass restores PS after the no-color depth pass");
			const unsigned char *depthLeft = &pixels[4 * (32 * 64 + 24)];
			const unsigned char *depthRight = &pixels[4 * (32 * 64 + 40)];
			result |= check(depthLeft[0] < 16 && depthLeft[1] > 240 &&
				depthLeft[2] < 16 && depthRight[0] > 240 &&
				depthRight[1] < 16 && depthRight[2] < 16,
				"no-color depth writes reject equal depth only on unclipped or opaque pixels");
			revealDepthState.pipeline.depthStencil.depthEnable = false;
			revealDepthState.pipeline.depthStencil.depthWrite = false;
			revealDepthState.pipeline.depthStencil.stencilEnable = true;
			revealDepthState.pipeline.depthStencil.stencilReference = 37U;
			revealDepthState.pipeline.depthStencil.stencilFunction =
				rts::render::RENDER_COMPARE_EQUAL;
			result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
				context->clearTargets(rts::render::RENDER_CLEAR_COLOR, clearColor,
					1.0f, 0) == rts::render::RENDER_RESULT_OK &&
				context->setLegacyState(revealDepthState,
					rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setVertexBuffer(redVertexBuffer, sizeof(TestVertex), 0) ==
					rts::render::RENDER_RESULT_OK &&
				context->setPrimitiveTopology(
					rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
					rts::render::RENDER_RESULT_OK &&
				context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
				context->endFrame() == rts::render::RENDER_RESULT_OK &&
				device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
					&captureFormat) == rts::render::RENDER_RESULT_OK,
				"D3D11 color pass observes the preceding no-color stencil writes");
			const unsigned char *stencilLeft = &pixels[4 * (32 * 64 + 24)];
			const unsigned char *stencilRight = &pixels[4 * (32 * 64 + 40)];
			result |= check(stencilLeft[0] > 240 && stencilLeft[1] < 16 &&
				stencilLeft[2] < 16 && stencilRight[0] < 16 &&
				stencilRight[1] < 16 && stencilRight[2] > 240,
				"no-color stencil writes retain clipping and alpha-tested cutout coverage");
		}
		result |= check(device->destroyResource(cutoutTexture),
			"D3D11 depth-only probe releases its cutout texture");
		// Draw equal-depth red then green triangles while changing LESS_EQUAL to
		// LESS.  The second draw must fail the strict comparison; a reversed or
		// collapsed comparison table would incorrectly replace the red result.
		logicalState = rts::render::LegacyLogicalState();
		logicalState.pipeline.rasterizer.cullMode =
			rts::render::RENDER_CULL_NONE;
		logicalState.pipeline.depthStencil.depthFunction =
			rts::render::RENDER_COMPARE_LESS_EQUAL;
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
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline accepts the first equal-depth probe");
		logicalState.pipeline.depthStencil.depthFunction =
			rts::render::RENDER_COMPARE_LESS;
		result |= check(context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
			rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(greenVertexBuffer, sizeof(TestVertex), 0) ==
			rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline applies strict less-depth comparison");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"strict less-depth comparison rejects the equal-depth green probe");
		// Replace stencil with a non-zero reference on the first draw, then
		// require EQUAL on the second draw.  A NOT_EQUAL probe must be rejected,
		// making stencil compare and pass-op direction observable as well.
		logicalState.pipeline.depthStencil.depthEnable = false;
		logicalState.pipeline.depthStencil.depthWrite = false;
		logicalState.pipeline.depthStencil.stencilEnable = true;
		logicalState.pipeline.depthStencil.stencilReadMask = 0xffU;
		logicalState.pipeline.depthStencil.stencilWriteMask = 0xffU;
		logicalState.pipeline.depthStencil.stencilReference = 37U;
		logicalState.pipeline.depthStencil.stencilFunction =
			rts::render::RENDER_COMPARE_ALWAYS;
		logicalState.pipeline.depthStencil.stencilPass =
			rts::render::RENDER_STENCIL_REPLACE;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clearTargets(rts::render::RENDER_CLEAR_COLOR |
				rts::render::RENDER_CLEAR_STENCIL, clearColor, 1.0f, 0) ==
				rts::render::RENDER_RESULT_OK &&
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
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline replaces the initial stencil value");
		logicalState.pipeline.depthStencil.stencilFunction =
			rts::render::RENDER_COMPARE_EQUAL;
		result |= check(context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
			rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(greenVertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline accepts the matching stencil reference");
		logicalState.pipeline.depthStencil.stencilFunction =
			rts::render::RENDER_COMPARE_NOT_EQUAL;
		result |= check(context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
			rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline rejects the non-matching stencil reference");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] > 240 && center[2] < 16,
			"stencil equal comparison preserves the green pass and rejects NOT_EQUAL");
		// The probe triangle is counter-clockwise in NDC.  D3D8's default CW
		// culling therefore rejects it, while the translated CCW front-face
		// setting accepts it.  This catches an inverted CW/CCW conversion.
		logicalState = rts::render::LegacyLogicalState();
		logicalState.pipeline.rasterizer.cullMode =
			rts::render::RENDER_CULL_BACK;
		logicalState.pipeline.rasterizer.frontCounterClockwise = false;
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
			"D3D11 parity pipeline applies default CW-facing culling");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"default CW-facing culling rejects the counter-clockwise probe");
		logicalState.pipeline.rasterizer.frontCounterClockwise = true;
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
			"D3D11 parity pipeline applies CCW-facing culling");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"CCW-facing culling accepts the counter-clockwise probe");
		// Non-symmetric blend probes keep source/destination reversal and
		// subtract/reverse-subtract mistakes observable in the contract suite.
		logicalState.pipeline.textureStages[0] =
			rts::render::LegacyTextureStageState();
		logicalState.pipeline.blend.blendEnable = true;
		logicalState.pipeline.blend.sourceColor =
			rts::render::RENDER_BLEND_SOURCE_COLOR;
		logicalState.pipeline.blend.destinationColor =
			rts::render::RENDER_BLEND_ZERO;
		logicalState.pipeline.blend.sourceAlpha =
			logicalState.pipeline.blend.sourceColor;
		logicalState.pipeline.blend.destinationAlpha =
			logicalState.pipeline.blend.destinationColor;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(redVertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline executes source-color blending");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] > 240,
			"source-color blend factors preserve the red source over blue destination");
		logicalState.pipeline.blend.sourceColor =
			rts::render::RENDER_BLEND_ZERO;
		logicalState.pipeline.blend.destinationColor =
			rts::render::RENDER_BLEND_SOURCE_COLOR;
		logicalState.pipeline.blend.sourceAlpha =
			logicalState.pipeline.blend.sourceColor;
		logicalState.pipeline.blend.destinationAlpha =
			logicalState.pipeline.blend.destinationColor;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(redVertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline executes destination-color blending");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] < 16,
			"destination-color blend factors do not accidentally reuse the source factor");
		logicalState.pipeline.blend.sourceColor = rts::render::RENDER_BLEND_ONE;
		logicalState.pipeline.blend.destinationColor = rts::render::RENDER_BLEND_ONE;
		logicalState.pipeline.blend.sourceAlpha = rts::render::RENDER_BLEND_ONE;
		logicalState.pipeline.blend.destinationAlpha = rts::render::RENDER_BLEND_ONE;
		logicalState.pipeline.blend.colorOperation =
			rts::render::RENDER_BLEND_SUBTRACT;
		logicalState.pipeline.blend.alphaOperation =
			rts::render::RENDER_BLEND_SUBTRACT;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(redVertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline executes subtract blend operations");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] > 240,
			"subtract blend operation keeps source-minus-destination direction");
		logicalState.pipeline.blend.colorOperation =
			rts::render::RENDER_BLEND_REVERSE_SUBTRACT;
		logicalState.pipeline.blend.alphaOperation =
			rts::render::RENDER_BLEND_REVERSE_SUBTRACT;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(redVertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline executes reverse-subtract blend operations");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] > 240 && center[1] < 16 && center[2] < 16,
			"reverse-subtract blend operation keeps destination-minus-source direction");
		logicalState.pipeline.blend = rts::render::LegacyBlendState();
		logicalState.pipeline.rasterizer.cullMode =
			rts::render::RENDER_CULL_NONE;
		// D3DTEXF_NONE must pin sampling to the base level.  Use deliberately
		// different base/coarse colors and a large LOD bias so a POINT-only
		// translation cannot pass this characterization.
		const unsigned int mipBasePixels[4] = {
			0xff00ff00U, 0xff00ff00U, 0xff00ff00U, 0xff00ff00U
		};
		const unsigned int mipCoarsePixel = 0xff0000ffU;
		rts::render::TextureDescriptor mipDescriptor = textureDescriptor;
		mipDescriptor.mipCount = 2;
		rts::render::TextureSubresourceData mipData[2];
		mipData[0].data = mipBasePixels;
		mipData[0].rowPitch = 2 * sizeof(unsigned int);
		mipData[0].slicePitch = sizeof(mipBasePixels);
		mipData[1].data = &mipCoarsePixel;
		mipData[1].rowPitch = sizeof(mipCoarsePixel);
		mipData[1].slicePitch = sizeof(mipCoarsePixel);
		rts::render::GpuHandle mipTexture;
		result |= check(device->createTexture(mipDescriptor, mipData, 2,
			&mipTexture) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a deliberately contrasting mip chain");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
		logicalState.pipeline.textureStages[0].colorArgument1 =
			rts::render::RENDER_TEXTURE_ARG_TEXTURE;
		logicalState.pipeline.textureStages[0].sampler.minification =
			rts::render::RENDER_TEXTURE_FILTER_LINEAR;
		logicalState.pipeline.textureStages[0].sampler.magnification =
			rts::render::RENDER_TEXTURE_FILTER_LINEAR;
		logicalState.pipeline.textureStages[0].sampler.mipmapping =
			rts::render::RENDER_TEXTURE_FILTER_NONE;
		logicalState.pipeline.textureStages[0].sampler.mipLodBias = 8.0f;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, mipTexture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline executes the no-mipmap sampler path");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[2] < 16 && center[0] < 16,
			"D3DTEXF_NONE pins sampling to the base mip level");
		logicalState.pipeline.textureStages[0].sampler.mipmapping =
			rts::render::RENDER_TEXTURE_FILTER_POINT;
		logicalState.pipeline.textureStages[0].sampler.maximumMipLevel = 1;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, mipTexture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline distinguishes point mip selection");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] > 240,
			"point mip filtering selects the deliberately contrasting coarse level");
		// D3DTSS_MAXMIPLEVEL excludes higher-detail levels.  A negative bias
		// would otherwise select mip zero; MAXMIPLEVEL=1 must clamp it to the
		// deliberately contrasting coarse mip.
		logicalState.pipeline.textureStages[0].sampler.mipLodBias = -8.0f;
		logicalState.pipeline.textureStages[0].sampler.maximumMipLevel = 1;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyStateForLayout(logicalState, texturedLayout, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, mipTexture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline applies MAXMIPLEVEL as a minimum LOD");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[0] < 16 && center[1] < 16 && center[2] > 240,
			"MAXMIPLEVEL excludes the higher-detail base mip");
		logicalState.pipeline.textureStages[0] =
			rts::render::LegacyTextureStageState();
		result |= check(device->destroyResource(mipTexture),
			"D3D11 parity probe drains the contrasting mip chain");
		if (typeProbeResourcesCreated)
		{
			rts::render::LegacyLogicalState typeProbeState;
			typeProbeState.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_NONE;
			typeProbeState.pipeline.textureStages[0].colorOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			typeProbeState.pipeline.textureStages[0].colorArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			typeProbeState.pipeline.textureStages[0].alphaOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			typeProbeState.pipeline.textureStages[0].alphaArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			std::vector<unsigned char> typeProbeCapture(64 * 64 * 4);
			rts::render::RenderFormat typeProbeCaptureFormat =
				rts::render::RENDER_FORMAT_UNKNOWN;

			// The first draw deliberately publishes state before the cube SRV.  A
			// stale cube mask makes the shader read the empty 2D slot instead of the
			// red cube slot.
			bool typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			bool typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, rts::render::GpuHandle()) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(typeProbeState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeCubeTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool typeProbeCubeCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			const unsigned char *typeProbeCenter =
				&typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(typeProbeCubeCaptured && typeProbeCenter[2] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[1] < 16,
				"D3D11 first draw refreshes cube type constants after state publication");

			// Texture-before-state is the control ordering: setLegacyState sees the
			// current 2D mask and must produce the green 2D sample.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, texture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(typeProbeState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool typeProbe2DCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(typeProbe2DCaptured && typeProbeCenter[1] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[2] < 16,
				"D3D11 texture-before-state ordering publishes the 2D type mask");

			// Once a cube state has been bound, replacing its SRV with a 2D SRV
			// without rebinding the logical state must refresh constants at draw time.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeCubeTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(typeProbeState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, texture) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool typeProbeCubeTo2DCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(typeProbeCubeTo2DCaptured && typeProbeCenter[1] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[2] < 16,
				"D3D11 draw-boundary refresh handles cube-to-2D transitions");

			// Repeating the same state and 2D binding must remain a cached fast path;
			// use indexed draws here so both draw entry points share the refresh path.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, texture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(typeProbeState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setIndexBuffer(indexBuffer,
						rts::render::RENDER_FORMAT_R16_UINT, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->drawIndexed(3, 0, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, texture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(typeProbeState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->drawIndexed(3, 0, 0) ==
						rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool typeProbeCachedCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(typeProbeCachedCaptured && typeProbeCenter[1] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[2] < 16,
				"D3D11 unchanged texture state stays on the cached indexed path");

			// The indexed draw also covers the opposite transition.  The first 2D
			// draw establishes constants with no cube bit; the final cube draw must
			// not reuse that stale mask.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, texture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(typeProbeState, texturedLayout,
						1) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setIndexBuffer(indexBuffer,
						rts::render::RENDER_FORMAT_R16_UINT, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->drawIndexed(3, 0, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeCubeTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->drawIndexed(3, 0, 0) ==
						rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool typeProbe2DToCubeCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(typeProbe2DToCubeCaptured && typeProbeCenter[2] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[1] < 16,
				"D3D11 indexed draw-boundary refresh handles 2D-to-cube transitions");

			rts::render::LegacyLogicalState signedBumpState;
			signedBumpState.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_NONE;
			signedBumpState.pipeline.textureStages[0].colorOperation =
				rts::render::RENDER_TEXTURE_OP_BUMP_ENVIRONMENT;
			signedBumpState.pipeline.textureStages[0].bumpEnvironmentMatrix00 =
			1.0f;
			signedBumpState.pipeline.textureStages[1].colorOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			signedBumpState.pipeline.textureStages[1].colorArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			signedBumpState.pipeline.textureStages[1].alphaOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			signedBumpState.pipeline.textureStages[1].alphaArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			signedBumpState.pipeline.textureStages[0].sampler.addressU =
				rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
			signedBumpState.pipeline.textureStages[0].sampler.addressV =
				rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
			signedBumpState.pipeline.textureStages[1].sampler.addressU =
				rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
			signedBumpState.pipeline.textureStages[1].sampler.addressV =
				rts::render::RENDER_TEXTURE_ADDRESS_CLAMP;
			signedBumpState.pipeline.textureStages[0].sampler.minification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			signedBumpState.pipeline.textureStages[0].sampler.magnification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			signedBumpState.pipeline.textureStages[0].sampler.mipmapping =
				rts::render::RENDER_TEXTURE_FILTER_NONE;
			signedBumpState.pipeline.textureStages[1].sampler.minification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			signedBumpState.pipeline.textureStages[1].sampler.magnification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			signedBumpState.pipeline.textureStages[1].sampler.mipmapping =
				rts::render::RENDER_TEXTURE_FILTER_NONE;

			// With the signed R8G8_SNORM probe, the correct bump offset selects the
			// red left texel.  Retain this passing signed-format control alongside
			// the cube regression so the refresh cannot change bump interpretation.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, rts::render::GpuHandle()) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, rts::render::GpuHandle()) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(signedBumpState, texturedLayout,
						3) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeSignedTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, typeProbeGradientTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool signedBumpCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(signedBumpCaptured && typeProbeCenter[2] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[1] < 16,
				"D3D11 first draw preserves signed bump interpretation after state publication");

			// The reverse order remains a valid control and must select the same
			// signed result when setLegacyState sees the already-bound SNORM SRV.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeSignedTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, typeProbeGradientTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(signedBumpState, texturedLayout,
						3) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool signedBumpReverseCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(signedBumpReverseCaptured && typeProbeCenter[2] > 240 &&
				typeProbeCenter[0] < 16 && typeProbeCenter[1] < 16,
				"D3D11 signed bump reverse ordering matches the state-first path");

			// Change only the stage-0 resource from signed to unsigned after the
			// state has been published.  The unsigned 0.25 sample must be remapped
			// to -0.5 and therefore still select the red left texel.
			typeProbeFrameStarted = context->beginFrame() ==
				rts::render::RENDER_RESULT_OK;
			typeProbeFrameSucceeded = false;
			if (typeProbeFrameStarted)
			{
				typeProbeFrameSucceeded =
					context->clear(clearColor, 1.0f, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeSignedTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(1, typeProbeGradientTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(signedBumpState, texturedLayout,
						3) == rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setIndexBuffer(indexBuffer,
						rts::render::RENDER_FORMAT_R16_UINT, 0) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, typeProbeUnsignedTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->drawIndexed(3, 0, 0) ==
						rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult typeProbeEndResult =
					context->endFrame();
				typeProbeFrameSucceeded = typeProbeFrameSucceeded &&
					typeProbeEndResult == rts::render::RENDER_RESULT_OK;
			}
			const bool signedBumpTransitionCaptured = typeProbeFrameSucceeded &&
				device->captureBackBuffer(&typeProbeCapture[0],
					typeProbeCapture.size(), 64 * 4, &typeProbeCaptureFormat) ==
					rts::render::RENDER_RESULT_OK;
			typeProbeCenter = &typeProbeCapture[4 * (32 * 64 + 32)];
			result |= check(signedBumpTransitionCaptured &&
				typeProbeCenter[2] > 240 && typeProbeCenter[0] < 16 &&
				typeProbeCenter[1] < 16,
				"D3D11 indexed draw refreshes signed-mask transitions");
		}
		rts::render::TextureDescriptor movieDescriptor;
		movieDescriptor.width = 16;
		movieDescriptor.height = 9;
		movieDescriptor.mipCount = 1;
		movieDescriptor.arrayCount = 1;
		movieDescriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		movieDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		const size_t movieVisibleRowBytes =
			static_cast<size_t>(movieDescriptor.width) * 4;
		const size_t movieRowPitch = movieVisibleRowBytes + 16;
		const size_t movieSlicePitch = movieRowPitch * movieDescriptor.height + 32;
		std::vector<unsigned char> moviePixels(movieSlicePitch, 0x31);
		rts::render::TextureSubresourceData movieData;
		movieData.data = &moviePixels[0];
		movieData.rowPitch = movieRowPitch;
		movieData.slicePitch = movieSlicePitch;
		rts::render::GpuHandle movieTexture;
		const bool movieCreated = device->createTexture(movieDescriptor, &movieData,
			1, &movieTexture) == rts::render::RENDER_RESULT_OK &&
			movieTexture.isValid();
		result |= check(movieCreated,
			"D3D11 recovery probe creates an exact-size padded BGRA movie texture");
		if (movieCreated)
		{
			const rts::render::GpuHandle originalMovieTexture = movieTexture;
			std::fill(moviePixels.begin(), moviePixels.end(),
				static_cast<unsigned char>(0x72));
			const bool movieFirstRefresh =
				device->refreshTexture(movieTexture, movieDescriptor, &movieData, 1) ==
				rts::render::RENDER_RESULT_OK && movieTexture == originalMovieTexture;
			std::fill(moviePixels.begin(), moviePixels.end(),
				static_cast<unsigned char>(0x17));
			for (unsigned int row = 0; row < movieDescriptor.height; ++row)
			{
				const unsigned char latestByte = row == movieDescriptor.height / 2 ?
					static_cast<unsigned char>(0xa4) :
					static_cast<unsigned char>(0x33);
				std::fill(moviePixels.begin() +
					static_cast<size_t>(row) * movieRowPitch,
					moviePixels.begin() + static_cast<size_t>(row) * movieRowPitch +
					movieVisibleRowBytes, latestByte);
			}
			const bool movieLatestRefresh =
				device->refreshTexture(movieTexture, movieDescriptor, &movieData, 1) ==
				rts::render::RENDER_RESULT_OK && movieTexture == originalMovieTexture;
			const bool movieRecovered = device->recoverDevice() ==
				rts::render::RENDER_RESULT_OK && device->isOperational() &&
				movieTexture == originalMovieTexture;
			rts::render::LegacyLogicalState movieState;
			movieState.pipeline.rasterizer.cullMode = rts::render::RENDER_CULL_NONE;
			movieState.pipeline.textureStages[0].colorOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			movieState.pipeline.textureStages[0].colorArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			movieState.pipeline.textureStages[0].alphaOperation =
				rts::render::RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
			movieState.pipeline.textureStages[0].alphaArgument1 =
				rts::render::RENDER_TEXTURE_ARG_TEXTURE;
			movieState.pipeline.textureStages[0].sampler.minification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			movieState.pipeline.textureStages[0].sampler.magnification =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			movieState.pipeline.textureStages[0].sampler.mipmapping =
				rts::render::RENDER_TEXTURE_FILTER_NONE;
			std::vector<unsigned char> movieCapture(64 * 64 * 4);
			rts::render::RenderFormat movieCaptureFormat =
				rts::render::RENDER_FORMAT_UNKNOWN;
			bool movieFrameSucceeded = false;
			if (movieRecovered && context->beginFrame() ==
				rts::render::RENDER_RESULT_OK)
			{
				movieFrameSucceeded = context->clear(clearColor, 1.0f, 0) ==
					rts::render::RENDER_RESULT_OK &&
					context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
						rts::render::RENDER_RESULT_OK &&
					context->setLegacyStateForLayout(movieState, texturedLayout, 1) ==
						rts::render::RENDER_RESULT_OK &&
					context->setVertexBuffer(texturedVertexBuffer,
						sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
					context->setTexture(0, movieTexture) ==
						rts::render::RENDER_RESULT_OK &&
					context->setPrimitiveTopology(
						rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
						rts::render::RENDER_RESULT_OK &&
					context->draw(3, 0) == rts::render::RENDER_RESULT_OK;
				const rts::render::RenderResult movieEndResult = context->endFrame();
				movieFrameSucceeded = movieFrameSucceeded && movieEndResult ==
					rts::render::RENDER_RESULT_OK;
			}
			const bool movieCaptured = movieFrameSucceeded &&
				device->captureBackBuffer(&movieCapture[0], movieCapture.size(),
					64 * 4, &movieCaptureFormat) == rts::render::RENDER_RESULT_OK;
			const unsigned char *movieCenter =
				&movieCapture[4 * (32 * 64 + 32)];
			const unsigned char expectedMovieByte = 0xa4;
			const bool latestMoviePixelVisible = movieCaptured &&
				movieCaptureFormat == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM &&
				movieCenter[0] >= expectedMovieByte - 1 &&
				movieCenter[0] <= expectedMovieByte + 1 &&
				movieCenter[1] >= expectedMovieByte - 1 &&
				movieCenter[1] <= expectedMovieByte + 1 &&
				movieCenter[2] >= expectedMovieByte - 1 &&
				movieCenter[2] <= expectedMovieByte + 1;
			result |= check(movieFirstRefresh && movieLatestRefresh &&
				movieRecovered && latestMoviePixelVisible,
				"D3D11 recovery readback preserves the latest BGRA movie pixels across padded rows");
		}
		unsigned int resizeRecoveryValue = 0x12345678U;
		rts::render::RenderBackBufferInfo resizeRecoveryInfo;
		result |= check(device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
			device->resize(80, 72) == rts::render::RENDER_RESULT_OK &&
			device->getBackBufferInfo(&resizeRecoveryInfo) ==
				rts::render::RENDER_RESULT_OK &&
			resizeRecoveryInfo.width == 80 && resizeRecoveryInfo.height == 72 &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->updateBuffer(resizeRecoveryBuffer, &resizeRecoveryValue,
				sizeof(resizeRecoveryValue), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->destroyResource(resizeRecoveryBuffer),
			"D3D11 recovery preserves logical handles before applying a requested resize");
		unsigned int debugErrorCount = 0xffffffffU;
		const rts::render::RenderResult debugValidationResult =
			device->getDebugValidationErrorCount(&debugErrorCount);
		result |= check((debugValidationResult ==
			rts::render::RENDER_RESULT_OK && debugErrorCount == 0) ||
			debugValidationResult == rts::render::RENDER_RESULT_UNSUPPORTED,
			"D3D11 debug layer reports no validation errors or is unavailable");
		if (typeProbeCubeTexture.isValid())
		{
			result |= check(device->destroyResource(typeProbeCubeTexture),
				"D3D11 releases the cube type-mask probe");
		}
		if (typeProbeGradientTexture.isValid())
		{
			result |= check(device->destroyResource(typeProbeGradientTexture),
				"D3D11 releases the gradient type-mask probe");
		}
		if (typeProbeSignedTexture.isValid())
		{
			result |= check(device->destroyResource(typeProbeSignedTexture),
				"D3D11 releases the signed type-mask probe");
		}
		if (typeProbeUnsignedTexture.isValid())
		{
			result |= check(device->destroyResource(typeProbeUnsignedTexture),
				"D3D11 releases the unsigned type-mask probe");
		}
		result |= check(device->destroyResource(vertexBuffer) &&
			device->destroyResource(greenVertexBuffer) &&
			device->destroyResource(halfAlphaVertexBuffer) &&
			device->destroyResource(redVertexBuffer) &&
			device->destroyResource(indexBuffer) &&
			device->destroyResource(texturedVertexBuffer) &&
			device->destroyResource(treeVertexBuffer) &&
			device->destroyResource(waterVertexBuffer) &&
			device->destroyResource(seaVertexBuffer) &&
			device->destroyResource(flexibleVertexBuffer) &&
			device->destroyResource(missingCoordinateBuffer) &&
			device->destroyResource(specularVertexBuffer) &&
			device->destroyResource(preTransformedBuffer) &&
			device->destroyResource(texture) &&
			device->destroyResource(treeShroudTexture) &&
			device->destroyResource(riverEdgeTexture) &&
			device->destroyResource(seaBumpTexture) &&
			device->destroyResource(seaReflectionTexture) &&
			device->destroyResource(secondTexture) &&
			device->destroyResource(uvSelectionTexture) &&
			device->destroyResource(offscreenColor) &&
			device->destroyResource(offscreenDepth) &&
			device->destroyResource(offscreenCopyColor) &&
			device->destroyResource(copiedColor) &&
			device->destroyResource(wrongSizeColor) &&
			device->destroyResource(wrongFormatColor) &&
			device->present() == rts::render::RENDER_RESULT_OK &&
			device->resize(96, 80) == rts::render::RENDER_RESULT_OK &&
			device->present() == rts::render::RENDER_RESULT_OK,
			"hidden flip-model swap chain presents and resizes");
		if (movieCreated)
		{
			result |= check(device->destroyResource(movieTexture),
				"D3D11 recovery probe drains the movie texture");
		}
	std::vector<unsigned char> resizedPixels(96 * 80 * 4);
	result |= check(device->captureBackBuffer(&resizedPixels[0],
		resizedPixels.size(), 96 * 4, &captureFormat) ==
			 rts::render::RENDER_RESULT_OK,
		"resized D3D11 swap chain captures at its new dimensions");
		result |= check(device->getBackBufferInfo(&backBufferInfo) ==
			rts::render::RENDER_RESULT_OK && backBufferInfo.width == 96 &&
			backBufferInfo.height == 80 &&
			backBufferInfo.format == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM,
			"D3D11 back-buffer info follows the resized swap-chain texture");
		result |= check(device->resize(0, 0) ==
			rts::render::RENDER_RESULT_OK &&
			device->getBackBufferInfo(&backBufferInfo) ==
				rts::render::RENDER_RESULT_OK && backBufferInfo.width == 96 &&
			backBufferInfo.height == 80,
			"D3D11 minimized resize preserves the last valid swap-chain targets");
		unsigned char captureProbe = 0;
		result |= check(device->captureBackBuffer(&captureProbe,
			static_cast<size_t>(-1), static_cast<size_t>(-1), &captureFormat) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"D3D11 capture rejects overflowing destination size arithmetic");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(1.0f, 0.0f, 0.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&resizedPixels[0], resizedPixels.size(),
				96 * 4, &captureFormat) == rts::render::RENDER_RESULT_OK &&
			resizedPixels[4 * (79 * 96 + 95) + 2] > 240 &&
			resizedPixels[4 * (79 * 96 + 95) + 1] < 16,
			"resized D3D11 swap chain clears and captures its full dimensions");
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			device->resize(96, 80) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			device->captureBackBuffer(&resizedPixels[0], resizedPixels.size(),
				96 * 4, &captureFormat) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 lifecycle rejects resize and capture while a frame is open");
		result |= check(device->recoverDevice() ==
			rts::render::RENDER_RESULT_OK &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 1.0f),
				1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&resizedPixels[0], resizedPixels.size(),
				96 * 4, &captureFormat) == rts::render::RENDER_RESULT_OK &&
			resizedPixels[1] > 240 && resizedPixels[0] < 16,
			"D3D11 recovery preserves resized back-buffer dimensions and pixels");
		const rts::render::RenderResult liveObjectReportResult =
			device->reportDebugLiveObjects();
		result |= check(liveObjectReportResult ==
			rts::render::RENDER_RESULT_OK || liveObjectReportResult ==
			rts::render::RENDER_RESULT_UNSUPPORTED,
			"D3D11 shutdown path can request a live-object report");
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
	result |= check(device->isOperational(),
		"initialized D3D11 device reports an operational state");
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
	rts::render::RenderResourceStatistics bufferStatistics;
	result |= check(device->getDebugResourceStatistics(&bufferStatistics) ==
		rts::render::RENDER_RESULT_OK && bufferStatistics.bufferCount == 1 &&
		bufferStatistics.recoveryShadowBytes == 0,
		"D3D11 buffer ownership retains descriptors without a persistent CPU shadow");
	rts::render::BufferDescriptor immutableRecoveryDescriptor;
	immutableRecoveryDescriptor.byteCount = 16;
	immutableRecoveryDescriptor.stride = 4;
	unsigned int immutableRecoveryBytes[4] = { 7, 8, 9, 10 };
	rts::render::GpuHandle immutableRecoveryBuffer;
	result |= check(device->createBuffer(immutableRecoveryDescriptor,
		immutableRecoveryBytes, sizeof(immutableRecoveryBytes),
		&immutableRecoveryBuffer) == rts::render::RENDER_RESULT_OK &&
		device->getDebugResourceStatistics(&bufferStatistics) ==
			rts::render::RENDER_RESULT_OK && bufferStatistics.bufferCount == 2 &&
		bufferStatistics.recoveryShadowBytes == sizeof(immutableRecoveryBytes),
		"only immutable buffer creation sources contribute recovery bytes");
	result |= check(device->recoverDevice() ==
		rts::render::RENDER_RESULT_OK && device->isOperational(),
		"D3D11 recovery recreates live logical resources without changing handles");
	result |= check(device->immediateContext()->beginFrame() ==
		rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->setVertexBuffer(immutableRecoveryBuffer,
			immutableRecoveryDescriptor.stride, 0) ==
			rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->setVertexBuffer(buffer, descriptor.stride, 0) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK &&
		device->destroyResource(immutableRecoveryBuffer),
		"a recovered descriptor-only buffer fails closed until owner republish");
	rts::render::BufferDescriptor largerMutableDescriptor;
	largerMutableDescriptor.byteCount = 64;
	largerMutableDescriptor.stride = 16;
	largerMutableDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle largerMutableBuffer;
	result |= check(device->createBuffer(largerMutableDescriptor, 0, 0,
		&largerMutableBuffer) == rts::render::RENDER_RESULT_OK &&
		largerMutableBuffer.index() == immutableRecoveryBuffer.index() &&
		largerMutableBuffer.generation() != immutableRecoveryBuffer.generation() &&
		device->getDebugResourceStatistics(&bufferStatistics) ==
			rts::render::RENDER_RESULT_OK && bufferStatistics.bufferCount == 2 &&
		bufferStatistics.recoveryShadowBytes == 0,
		"destroyed immutable recovery bytes do not survive slot reuse");
	result |= check(device->recoverDevice() ==
		rts::render::RENDER_RESULT_OK && device->isOperational() &&
		device->immediateContext()->beginFrame() ==
			rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->setVertexBuffer(largerMutableBuffer,
			largerMutableDescriptor.stride, 0) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK,
		"reused mutable buffer recovers only its descriptor shell");
	unsigned int largerMutableBytes[16] = { 0 };
	result |= check(device->updateBufferResource(largerMutableBuffer,
		largerMutableBytes, sizeof(largerMutableBytes), 0,
		rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->beginFrame() ==
			rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->setVertexBuffer(largerMutableBuffer,
			largerMutableDescriptor.stride, 0) == rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK &&
		device->destroyResource(largerMutableBuffer),
		"reused mutable buffer becomes bindable only after full republish");
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
		rts::render::RENDER_RESULT_OK && device->isOperational() &&
		device->immediateContext()->beginFrame() ==
			rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->updateBuffer(buffer, values,
			sizeof(values), 0) == rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK,
		"recreated dynamic buffers retain their logical handle and update path");
	result |= check(device->getDebugResourceStatistics(&bufferStatistics) ==
		rts::render::RENDER_RESULT_OK && bufferStatistics.bufferCount == 1 &&
		bufferStatistics.recoveryShadowBytes == 0,
		"buffer recovery recreates descriptor shells without allocating CPU bytes");
	unsigned int rangeValues[4] = { 10, 20, 30, 40 };
	rts::render::IRenderContext *rangeContext = device->immediateContext();
	result |= check(rangeContext->updateBuffer(buffer, rangeValues,
		sizeof(rangeValues), 0, rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"dynamic range updates require an open owner frame");
	result |= check(device->updateBufferResource(buffer, rangeValues,
		sizeof(unsigned int) * 2, 0,
		rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_OK,
		"device-owner buffer publication is legal before an owner frame");
	result |= check(rangeContext->beginFrame() ==
		rts::render::RENDER_RESULT_OK &&
		rangeContext->updateBuffer(buffer, rangeValues, sizeof(unsigned int) * 2,
			0, rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_OK &&
		rangeContext->updateBuffer(buffer, rangeValues + 2,
			sizeof(unsigned int) * 2, sizeof(unsigned int) * 2,
			rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE) ==
			rts::render::RENDER_RESULT_OK &&
		rangeContext->updateBuffer(buffer, rangeValues, sizeof(unsigned int),
			sizeof(unsigned int), rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rangeContext->updateBuffer(buffer, rangeValues, 0, 0,
			rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rangeContext->updateBuffer(buffer, rangeValues, sizeof(rangeValues),
			descriptor.byteCount - sizeof(unsigned int),
			rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rangeContext->updateBuffer(buffer, rangeValues, sizeof(rangeValues), 0,
			static_cast<rts::render::RenderBufferUpdateMode>(99)) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rangeContext->endFrame() == rts::render::RENDER_RESULT_OK,
		"dynamic vertex buffers enforce discard and no-overwrite range contracts");

	rts::render::BufferDescriptor indexDescriptor;
	indexDescriptor.byteCount = 16;
	indexDescriptor.stride = 2;
	indexDescriptor.binding = rts::render::RENDER_BUFFER_INDEX;
	indexDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle indexBuffer;
	rts::render::BufferDescriptor constantDescriptor;
	constantDescriptor.byteCount = 64;
	constantDescriptor.stride = 16;
	constantDescriptor.binding = rts::render::RENDER_BUFFER_CONSTANT;
	constantDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle constantBuffer;
	unsigned short rangeIndices[4] = { 0, 1, 2, 3 };
	result |= check(device->createBuffer(indexDescriptor, 0, 0, &indexBuffer) ==
		rts::render::RENDER_RESULT_OK &&
		device->createBuffer(constantDescriptor, 0, 0, &constantBuffer) ==
		rts::render::RENDER_RESULT_OK &&
		rangeContext->beginFrame() == rts::render::RENDER_RESULT_OK &&
		rangeContext->updateBuffer(indexBuffer, rangeIndices,
			sizeof(unsigned short) * 2, 0,
			rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_OK &&
		rangeContext->updateBuffer(indexBuffer, rangeIndices + 2,
			sizeof(unsigned short) * 2, sizeof(unsigned short) * 2,
			rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE) ==
			rts::render::RENDER_RESULT_OK &&
		rangeContext->updateBuffer(constantBuffer, rangeValues,
			sizeof(rangeValues), 0,
			rts::render::RENDER_BUFFER_UPDATE_DISCARD) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rangeContext->endFrame() == rts::render::RENDER_RESULT_OK &&
		device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
		device->isOperational(),
		"dynamic index ranges recover while constant ranges fail closed");
	result |= check(device->destroyResource(indexBuffer) &&
		device->destroyResource(constantBuffer),
		"dynamic range contract resources release cleanly");
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
		&cubeTexture) == rts::render::RENDER_RESULT_OK,
		"D3D11 creates generation-safe cube-map shader resources");
	if (cubeTexture.isValid())
	{
		rts::render::LegacyLogicalState cubeState;
		cubeState.pipeline.textureStages[0].cameraSpaceReflectionVector = true;
		cubeState.pipeline.textureStages[0].textureTransformEnable = true;
		cubeState.pipeline.textureStages[0].projectedCoordinates = true;
		cubeState.pipeline.textureStages[0].textureTransformCount = 4;
		rts::render::LegacyVertexLayout cubeLayout;
		cubeLayout.stride = 36;
		cubeLayout.elementCount = 4;
		cubeLayout.elements[0].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
		cubeLayout.elements[0].semanticIndex = 0;
		cubeLayout.elements[0].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		cubeLayout.elements[0].byteOffset = 0;
		cubeLayout.elements[1].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_NORMAL;
		cubeLayout.elements[1].semanticIndex = 0;
		cubeLayout.elements[1].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT3;
		cubeLayout.elements[1].byteOffset = 12;
		cubeLayout.elements[2].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
		cubeLayout.elements[2].semanticIndex = 0;
		cubeLayout.elements[2].format =
			rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
		cubeLayout.elements[2].byteOffset = 24;
		cubeLayout.elements[3].semantic =
			rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
		cubeLayout.elements[3].semanticIndex = 0;
		cubeLayout.elements[3].format =
			rts::render::RENDER_VERTEX_DATA_FLOAT2;
		cubeLayout.elements[3].byteOffset = 28;
		const bool cubeFrame = device->immediateContext()->beginFrame() ==
			rts::render::RENDER_RESULT_OK;
		result |= check(cubeFrame &&
			device->immediateContext()->setTexture(0, cubeTexture) ==
				rts::render::RENDER_RESULT_OK &&
			device->immediateContext()->setLegacyStateForLayout(cubeState,
				cubeLayout, 1U) ==
				rts::render::RENDER_RESULT_OK &&
			device->immediateContext()->setTexture(0,
				rts::render::GpuHandle()) ==
				rts::render::RENDER_RESULT_OK &&
			device->immediateContext()->endFrame() ==
				rts::render::RENDER_RESULT_OK,
			"D3D11 binds a cube SRV through the disjoint reflection register path");
	}
	result |= check(device->destroyResource(cubeTexture),
		"D3D11 releases the tested cube-map resource");
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
	std::vector<unsigned char> headlessPixels(64 * 64 * 4);
	rts::render::RenderFormat headlessFormat =
		rts::render::RENDER_FORMAT_UNKNOWN;
	result |= check(device->captureBackBuffer(&headlessPixels[0],
		headlessPixels.size(), 64 * 4, &headlessFormat) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"headless D3D11 devices reject back-buffer capture without a swap chain");
	result |= check(device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
		device->isOperational() &&
		device->immediateContext() != 0 &&
		device->present() == rts::render::RENDER_RESULT_OK,
		"headless D3D11 device recreates at an empty frame boundary");
	device->shutdown();
	result |= check(!device->isOperational(),
		"shutdown D3D11 device reports an inactive state");
	device->shutdown();
	delete device;
	return result;
}

#endif

int testW3DVideoBufferDirectPublicationLayout()
{
	int result = 0;
	result |= check(ExpectedNativeVideoPublicationSelection(
		rts::render::RENDER_BACKEND_DX8, true, true) == false,
		"W3D video keeps the legacy publication policy for DX8");
#if defined(_WIN64)
	result |= check(ExpectedNativeVideoPublicationSelection(
		rts::render::RENDER_BACKEND_D3D11, true, true),
		"W3D video selection requires a supported active native owner");
#else
	result |= check(!ExpectedNativeVideoPublicationSelection(
		rts::render::RENDER_BACKEND_D3D11, true, true),
		"W3D video keeps the compatibility publication policy on x86");
#endif
	result |= check(!ExpectedNativeVideoPublicationSelection(
		rts::render::RENDER_BACKEND_D3D11, true, false),
		"W3D video selection rejects a requested but inactive native owner");
	result |= check(!ExpectedNativeVideoPublicationSelection(
		rts::render::RENDER_BACKEND_D3D11, false, true),
		"W3D video selection rejects an unsupported native backend");

	std::string source;
	result |= check(ReadSourceText(
		"Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DVideoBuffer.cpp",
		&source), "W3D video source is available for policy-contract checks");
	if (!source.empty())
	{
		const std::string::size_type method = source.find(
			"Bool W3DVideoBuffer::UsesNativeD3D11PublicationPath()");
		const std::string::size_type methodEnd = source.find(
			"// W3DVideoBuffer::SetBuffer", method);
		const std::string::size_type nativeGuard = source.find(
			"#if defined(_WIN64)", method);
		const std::string::size_type fallbackBranch = source.find(
			"#else", nativeGuard);
		const std::string::size_type fallbackReturn = source.find(
			"return FALSE;", fallbackBranch);
		const std::string::size_type activeOwnerQuery = source.find(
			"IsNativeD3D11PublicationActive()", method);
		result |= check(method != std::string::npos &&
			methodEnd != std::string::npos && nativeGuard > method &&
			nativeGuard < methodEnd && fallbackBranch > nativeGuard &&
			fallbackBranch < methodEnd && fallbackReturn > fallbackBranch &&
			fallbackReturn < methodEnd,
			"W3D video native selection is compile-time disabled outside x64");
		result |= check(activeOwnerQuery > method &&
			activeOwnerQuery < methodEnd,
			"W3D video selection consults the actual active native owner");
		result |= check(source.find("publishLockedFrame") ==
			std::string::npos && source.find(
			"Publish_Render_Texture_BGRA8_Change") == std::string::npos,
			"W3D video has no second pre-unlock native frame publisher");
		result |= check(source.find("m_surface->Unlock()") !=
			std::string::npos,
			"W3D video retains the SurfaceClass unlock publication boundary");
	}

	const unsigned int width = 16;
	const unsigned int height = 9;
	const unsigned int paddedPitch = width * 4 + 16;
	size_t slicePitch = 0;
	result |= check(W3DVideoBuffer::ComputeDirectBGRA8SlicePitch(
		VideoBuffer::TYPE_X8R8G8B8, width, height, paddedPitch, &slicePitch) &&
		slicePitch == static_cast<size_t>(paddedPitch) * height,
		"W3D video publication accepts a padded BGRA8 pitch and computes its full slice");
	result |= check(!W3DVideoBuffer::ComputeDirectBGRA8SlicePitch(
		VideoBuffer::TYPE_X8R8G8B8, width, height, width * 4 - 1, &slicePitch),
		"W3D video publication rejects a short BGRA8 row");
	result |= check(!W3DVideoBuffer::ComputeDirectBGRA8SlicePitch(
		VideoBuffer::TYPE_R8G8B8, width, height, paddedPitch, &slicePitch),
		"W3D video publication rejects non-BGRA8 formats");
	result |= check(!W3DVideoBuffer::ComputeDirectBGRA8SlicePitch(
		VideoBuffer::TYPE_X8R8G8B8, width, height, paddedPitch, 0),
		"W3D video publication rejects a missing slice output");
	return result;
}

#if defined(RTS_RENDERER_HAS_D3D11)
int testD3D11TextureRefreshContract()
{
	int result = 0;
	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0,
		"D3D11 texture refresh contract factory returns a device");
	if (device == 0)
	{
		return result;
	}
	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	result |= check(device->initialize(parameters) ==
		rts::render::RENDER_RESULT_OK,
		"D3D11 texture refresh contract device initializes");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	rts::render::TextureDescriptor descriptor;
	descriptor.width = 4;
	descriptor.height = 4;
	descriptor.mipCount = 2;
	descriptor.arrayCount = 1;
	descriptor.format = rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
	descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	rts::render::TextureDescriptor arrayDescriptor = descriptor;
	arrayDescriptor.arrayCount = 2;
	rts::render::GpuHandle arrayTexture;
	result |= check(device->createTexture(arrayDescriptor, 0, 0, &arrayTexture) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT && !arrayTexture.isValid(),
		"D3D11 rejects shader-resource 2D texture arrays without an array-index contract");
	rts::render::TextureDescriptor cubeArrayDescriptor = descriptor;
	cubeArrayDescriptor.dimension = rts::render::RENDER_TEXTURE_CUBE;
	cubeArrayDescriptor.arrayCount = 12;
	rts::render::GpuHandle cubeArrayTexture;
	result |= check(device->createTexture(cubeArrayDescriptor, 0, 0,
		&cubeArrayTexture) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!cubeArrayTexture.isValid(),
		"D3D11 rejects cube arrays without a shader cube-array index contract");
	std::vector<std::vector<unsigned char> > storage(2);
	rts::render::TextureSubresourceData data[2];
	for (unsigned int index = 0; index < 2; ++index)
	{
		const unsigned int mip = index % 2;
		const unsigned int width = mip == 0 ? 4 : 2;
		const unsigned int height = mip == 0 ? 4 : 2;
		const size_t rowPitch = static_cast<size_t>(width) * 4 + 4;
		const size_t slicePitch = rowPitch * height + 8;
		storage[index].assign(slicePitch, static_cast<unsigned char>(index + 1));
		data[index].data = &storage[index][0];
		data[index].rowPitch = rowPitch;
		data[index].slicePitch = slicePitch;
	}
	rts::render::GpuHandle texture;
	result |= check(device->createTexture(descriptor, data, 2, &texture) ==
		rts::render::RENDER_RESULT_OK && texture.isValid(),
		"D3D11 refresh contract creates an ordinary multi-mip 2D texture");
	const rts::render::GpuHandle originalTexture = texture;
	for (unsigned int index = 0; index < 2; ++index)
	{
		std::fill(storage[index].begin(), storage[index].end(),
			static_cast<unsigned char>(0xa0 + index));
	}
	result |= check(device->refreshTexture(texture, descriptor, data, 2) ==
		rts::render::RENDER_RESULT_OK && texture == originalTexture,
		"compatible refresh updates every subresource without replacing its handle");
	rts::render::TextureSubresourceData invalidData[2];
	for (unsigned int index = 0; index < 2; ++index)
	{
		invalidData[index] = data[index];
	}
	invalidData[1].rowPitch = 1;
	result |= check(device->refreshTexture(texture, descriptor, invalidData, 2) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT && texture == originalTexture,
		"invalid refresh data fails before writing or replacing the existing resource");
	result |= check(device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
		device->isOperational() && texture == originalTexture,
		"a failed refresh leaves the logical texture recoverable with a stable handle");
	for (unsigned int index = 0; index < 2; ++index)
	{
		invalidData[index] = data[index];
	}
	invalidData[1].slicePitch = invalidData[1].rowPitch;
	rts::render::GpuHandle invalidSliceTexture;
	result |= check(device->createTexture(descriptor, invalidData, 2,
		&invalidSliceTexture) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!invalidSliceTexture.isValid() &&
		device->refreshTexture(texture, descriptor, invalidData, 2) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"texture uploads reject a nonzero slice pitch shorter than its rows");

	rts::render::TextureDescriptor changedDescriptor = descriptor;
	changedDescriptor.width = 8;
	result |= check(device->refreshTexture(texture, changedDescriptor, data, 2) ==
		rts::render::RENDER_RESULT_UNSUPPORTED,
		"shape changes request refresh fallback without accepting stale data");
	changedDescriptor = descriptor;
	changedDescriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
	result |= check(device->refreshTexture(texture, changedDescriptor, data, 2) ==
		rts::render::RENDER_RESULT_UNSUPPORTED,
		"format changes request refresh fallback without replacing the handle");

	rts::render::TextureDescriptor immutableDescriptor = descriptor;
	immutableDescriptor.mipCount = 1;
	immutableDescriptor.arrayCount = 1;
	immutableDescriptor.usage = rts::render::RENDER_USAGE_IMMUTABLE;
	std::vector<unsigned char> immutablePixels(4 * 4 * 4, 0x11);
	rts::render::TextureSubresourceData immutableData;
	immutableData.data = &immutablePixels[0];
	immutableData.rowPitch = 4 * 4;
	immutableData.slicePitch = immutablePixels.size();
	rts::render::GpuHandle immutableTexture;
	result |= check(device->createTexture(immutableDescriptor, &immutableData, 1,
		&immutableTexture) == rts::render::RENDER_RESULT_OK &&
		device->refreshTexture(immutableTexture, immutableDescriptor,
			&immutableData, 1) == rts::render::RENDER_RESULT_UNSUPPORTED,
		"immutable textures report a capability fallback for refresh");

	rts::render::TextureDescriptor dynamicDescriptor = immutableDescriptor;
	dynamicDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle dynamicTexture;
	result |= check(device->createTexture(dynamicDescriptor, &immutableData, 1,
		&dynamicTexture) == rts::render::RENDER_RESULT_OK &&
		device->refreshTexture(dynamicTexture, dynamicDescriptor,
			&immutableData, 1) == rts::render::RENDER_RESULT_OK,
		"dynamic textures refresh through the renderer-neutral contract");

	// This is the neutral-renderer shape used by the direct movie bridge.  A
	// padded row and slice make the exact pitch contract observable while the
	// repeated refreshes exercise the persistent recovery-shadow storage.
	rts::render::TextureDescriptor movieDescriptor;
	movieDescriptor.width = 16;
	movieDescriptor.height = 9;
	movieDescriptor.mipCount = 1;
	movieDescriptor.arrayCount = 1;
	movieDescriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
	movieDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	const size_t movieRowPitch = movieDescriptor.width * 4 + 16;
	const size_t movieSlicePitch = movieRowPitch * movieDescriptor.height + 32;
	std::vector<unsigned char> moviePixels(movieSlicePitch, 0x31);
	rts::render::TextureSubresourceData movieData;
	movieData.data = &moviePixels[0];
	movieData.rowPitch = movieRowPitch;
	movieData.slicePitch = movieSlicePitch;
	rts::render::GpuHandle movieTexture;
	const bool movieCreated = device->createTexture(movieDescriptor, &movieData,
		1, &movieTexture) == rts::render::RENDER_RESULT_OK &&
		movieTexture.isValid();
	result |= check(movieCreated,
		"D3D11 direct movie probe creates an exact-size BGRA texture");
	if (movieCreated)
	{
		const rts::render::GpuHandle originalMovieTexture = movieTexture;
		std::fill(moviePixels.begin(), moviePixels.end(),
			static_cast<unsigned char>(0x72));
		result |= check(device->refreshTexture(movieTexture, movieDescriptor,
			&movieData, 1) == rts::render::RENDER_RESULT_OK &&
			movieTexture == originalMovieTexture,
			"D3D11 direct movie probe refreshes BGRA pixels in place");
		std::fill(moviePixels.begin(), moviePixels.end(),
			static_cast<unsigned char>(0xa4));
		result |= check(device->refreshTexture(movieTexture, movieDescriptor,
			&movieData, 1) == rts::render::RENDER_RESULT_OK &&
			movieTexture == originalMovieTexture &&
			device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
			device->isOperational() && movieTexture == originalMovieTexture,
			"D3D11 direct movie probe retains its latest shadow across recovery");
	}

	rts::render::IRenderContext *context = device->immediateContext();
	const bool frameStarted = context->beginFrame() ==
		rts::render::RENDER_RESULT_OK;
	result |= check(frameStarted && context->setTexture(0, texture) ==
		rts::render::RENDER_RESULT_OK &&
		device->refreshTexture(texture, descriptor, data, 2) ==
		rts::render::RENDER_RESULT_OK &&
		context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
		context->endFrame() == rts::render::RENDER_RESULT_OK,
		"refresh unbinds and safely rebinds an active shader resource");

	rts::render::TextureDescriptor targetDescriptor = immutableDescriptor;
	targetDescriptor.binding = rts::render::RENDER_TEXTURE_RENDER_TARGET |
		rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
	targetDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	rts::render::GpuHandle targetTexture;
	rts::render::TextureDescriptor depthDescriptor;
	depthDescriptor.width = 4;
	depthDescriptor.height = 4;
	depthDescriptor.binding = rts::render::RENDER_TEXTURE_DEPTH_STENCIL;
	depthDescriptor.format = rts::render::RENDER_FORMAT_D24_UNORM_S8_UINT;
	depthDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	rts::render::GpuHandle depthTexture;
	result |= check(device->createTexture(targetDescriptor, &immutableData, 1,
		&targetTexture) == rts::render::RENDER_RESULT_OK &&
		device->createTexture(depthDescriptor, 0, 0, &depthTexture) ==
		rts::render::RENDER_RESULT_OK,
		"refresh contract creates compatible render-target resources");
	rts::render::RenderTargetBinding targetBinding;
	targetBinding.useBackBufferColor = false;
	targetBinding.useBackBufferDepth = false;
	targetBinding.hasColor = true;
	targetBinding.hasDepth = true;
	targetBinding.color.resource = targetTexture;
	targetBinding.depth.resource = depthTexture;
	rts::render::RenderTargetBinding noTargets;
	noTargets.useBackBufferColor = false;
	noTargets.useBackBufferDepth = false;
	result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
		context->setRenderTargets(targetBinding) == rts::render::RENDER_RESULT_OK &&
		device->refreshTexture(targetTexture, targetDescriptor,
			&immutableData, 1) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		context->setRenderTargets(noTargets) == rts::render::RENDER_RESULT_OK &&
		device->refreshTexture(targetTexture, targetDescriptor,
			&immutableData, 1) == rts::render::RENDER_RESULT_OK &&
		context->endFrame() == rts::render::RENDER_RESULT_OK,
		"refresh defers active RTV hazards until the target is unbound");
	// Binding custom color/depth targets invalidates their CPU upload shadow for
	// recovery purposes. Exercise the full lifecycle so a device recreation
	// cannot restore a target that was already written by the GPU from stale
	// initial bytes, while preserving the logical handles.
	const rts::render::GpuHandle recoveryTarget = targetTexture;
	const rts::render::GpuHandle recoveryDepth = depthTexture;
	result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
		context->setRenderTargets(targetBinding) ==
			rts::render::RENDER_RESULT_OK &&
		context->clear(rts::render::RenderFloat4(0.0f, 1.0f, 0.0f, 1.0f),
			1.0f, 0) == rts::render::RENDER_RESULT_OK &&
		context->endFrame() == rts::render::RENDER_RESULT_OK &&
		device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
		context->beginFrame() == rts::render::RENDER_RESULT_OK &&
		context->setRenderTargets(recoveryTarget, recoveryDepth) ==
			rts::render::RENDER_RESULT_OK &&
		context->endFrame() == rts::render::RENDER_RESULT_OK,
		"custom color/depth targets survive recovery with stable logical handles");
	result |= check(device->recoverDevice() == rts::render::RENDER_RESULT_OK &&
		device->isOperational(),
		"refresh shadow retains every subresource across device recovery");

	result |= check(device->destroyResource(texture) &&
		device->refreshTexture(texture, descriptor, data, 2) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"refresh rejects a stale generation-safe texture handle");
	result |= check(device->destroyResource(immutableTexture) &&
		device->destroyResource(dynamicTexture) &&
		device->destroyResource(targetTexture) &&
		device->destroyResource(depthTexture),
		"refresh contract drains auxiliary texture resources");
	if (movieCreated)
	{
		result |= check(device->destroyResource(movieTexture),
			"refresh contract drains the direct movie texture");
	}
	device->shutdown();
	delete device;
	return result;
}

int testD3D11StateCacheEvictionContract()
{
	int result = 0;
	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0,
		"D3D11 state-cache contract factory returns a device");
	if (device == 0)
	{
		return result;
	}
	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	result |= check(device->initialize(parameters) ==
		rts::render::RENDER_RESULT_OK,
		"D3D11 state-cache contract device initializes");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	rts::render::IRenderContext *context = device->immediateContext();
	const bool frameStarted = context != 0 && context->beginFrame() ==
		rts::render::RENDER_RESULT_OK;
	result |= check(frameStarted,
		"D3D11 state-cache contract begins a frame");
	if (frameStarted)
	{
		rts::render::LegacyLogicalState state;
		state.pipeline.blend.blendEnable = true;
		bool everyStateBound = true;
		// Exercise more unique blend descriptors than the bounded cache can
		// retain. The currently bound state must remain protected while older
		// entries are evicted deterministically.
		for (unsigned int index = 0; index < 300; ++index)
		{
			state.pipeline.blend.sourceColor =
				static_cast<rts::render::RenderBlendFactor>(index % 10);
			state.pipeline.blend.destinationColor =
				static_cast<rts::render::RenderBlendFactor>((index / 10) % 10);
			state.pipeline.blend.colorOperation =
				static_cast<rts::render::RenderBlendOperation>((index / 100) % 3);
			const rts::render::RenderResult stateResult =
				context->setLegacyState(state,
					rts::render::RENDER_VERTEX_POSITION3_COLOR, 0);
			if (stateResult != rts::render::RENDER_RESULT_OK)
			{
				everyStateBound = false;
				break;
			}
		}
		result |= check(everyStateBound,
			"D3D11 state cache evicts old unbound blend states instead of failing at capacity");
		result |= check(context->endFrame() ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 state-cache contract ends a frame");
	}
	device->shutdown();
	delete device;
	return result;
}

#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
int testD3D11LegacyBridgeLifecycleContract()
{
	int result = 0;
	typedef rts::render::RenderResult (D3D11LegacyBridge::*ExpectedEndFrame)(bool);
	typedef rts::render::RenderResult (D3D11LegacyBridge::*ExpectedEndFrameOutcome)(
		bool, rts::render::RenderFrameOutcome *);
	typedef rts::render::RenderResult (D3D11LegacyBridge::*ExpectedResize)(
		unsigned int, unsigned int);
	typedef rts::render::RenderResult (D3D11LegacyBridge::*ExpectedActiveCopy)(
		IDirect3DBaseTexture8 *);
	typedef bool (D3D11LegacyBridge::*ExpectedCopiedContentAcquire)(
		IDirect3DBaseTexture8 *);
	typedef void (D3D11LegacyBridge::*ExpectedDisplayIteration)();
	typedef void (D3D11LegacyBridge::*ExpectedCaptureRequest)();
	typedef bool (D3D11LegacyBridge::*ExpectedBeginFrame)();
	typedef bool (D3D11LegacyBridge::*ExpectedPrepareReset)();
	typedef void (D3D11LegacyBridge::*ExpectedShutdown)();
	typedef rts::render::RenderResult (D3D11LegacyBridge::*ExpectedShutdownResult)();
	typedef void (D3D11LegacyBridge::*ExpectedInvalidateBufferRange)(IUnknown *,
		unsigned int, size_t, size_t, rts::render::RenderBufferUpdateMode);
	typedef bool (D3D11LegacyBridge::*ExpectedPublishBufferChange)(IUnknown *,
		unsigned int, const void *, size_t, size_t,
		rts::render::RenderBufferUpdateMode, unsigned int);
	typedef bool (D3D11LegacyBridge::*ExpectedPublishTextureBGRA8Change)(
		IDirect3DBaseTexture8 *, const void *, size_t, size_t);
	result |= check(std::is_same<decltype(static_cast<ExpectedEndFrame>(
		&D3D11LegacyBridge::End_Frame)), ExpectedEndFrame>::value &&
		std::is_same<decltype(static_cast<ExpectedEndFrameOutcome>(
		&D3D11LegacyBridge::End_Frame)), ExpectedEndFrameOutcome>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Resize),
		ExpectedResize>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Copy_Active_Color_Target_To_Texture),
		ExpectedActiveCopy>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Acquire_Copied_Texture_Content),
		ExpectedCopiedContentAcquire>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Begin_Display_Iteration),
		ExpectedDisplayIteration>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Request_Frame_Capture),
		ExpectedCaptureRequest>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Begin_Frame),
		ExpectedBeginFrame>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Prepare_Legacy_Device_Reset),
		ExpectedPrepareReset>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Shutdown),
		ExpectedShutdown>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Shutdown_Result),
		ExpectedShutdownResult>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Invalidate_Buffer_Range),
		ExpectedInvalidateBufferRange>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Publish_Buffer_Change),
		ExpectedPublishBufferChange>::value &&
		std::is_same<decltype(&D3D11LegacyBridge::Publish_Texture_BGRA8_Change),
		ExpectedPublishTextureBGRA8Change>::value,
		"D3D11 bridge exposes result-bearing lifecycle and pre-present capture methods");
	return result;
}
#endif

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
					"legacy blend case %s failed to bind with RenderResult %u\n",
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

#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
int TestLegacyResetResources();
int TestLegacyAsyncFramePolicy();
int TestLegacyAsyncBridgeCompletion();
#if defined(RTS_RENDERER_WAVE_SHADER_CONTRACT_TEST)
int RunLegacyWaveShaderContractTests();
#endif
#if defined(RTS_RENDERER_SHADER_ASSET_CONTRACT_TEST)
int RunLegacyShaderAssetContractTests();
#endif
#endif

int main()
{
	int result = 0;
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	result |= TestLegacyResetResources();
	result |= TestLegacyAsyncFramePolicy();
	result |= TestLegacyAsyncBridgeCompletion();
#if defined(RTS_RENDERER_WAVE_SHADER_CONTRACT_TEST)
	result |= RunLegacyWaveShaderContractTests();
#endif
#if defined(RTS_RENDERER_SHADER_ASSET_CONTRACT_TEST)
	result |= RunLegacyShaderAssetContractTests();
#endif
#endif
	result |= testBackendNames();
	result |= testRenderTexturePublicationOperationalStates();
#if !defined(RTS_RENDERER_HAS_D3D11)
	result |= testNativeRendererRejectsUnavailableBackend();
#endif
	result |= testRendererTextureLifecycleContracts();
	result |= testNativeMeshPrelitStageResetContract();
	result |= testGenerationSafeHandles();
	result |= testNeutralDescriptorDefaults();
	result |= testLegacyLogicalState();
	result |= testLegacyResetSeedAndAmbientUpdate();
	result |= testLegacyStatePublicationFailureLatch();
	result |= testLegacyWaterPixelProgram();
	result |= testLegacyTextureTransformAndNormalMath();
	result |= testLegacySeaWaveProgram();
	result |= testLegacyShaderBitDecoder();
	result |= testRendererFrameLifecycleState();
	result |= testRenderCaptureQueue();
	result |= testW3DVideoBufferDirectPublicationLayout();
#if defined(RTS_RENDERER_HAS_D3D11)
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	result |= testD3D11PrimitiveTopologyTranslation();
#endif
	result |= testD3D11LegacyStateBoundary();
	result |= testD3D11HeadlessDevice();
	result |= testD3D11TextureRefreshContract();
	result |= testD3D11StateCacheEvictionContract();
#if !defined(RTS_RENDERER_NATIVE_CONTRACT_ONLY)
	result |= testD3D11LegacyBridgeLifecycleContract();
#endif
	result |= testD3D11LegacyBlendFactors();
	result |= testD3D11HiddenSwapChain();
#endif
	if (result == 0)
	{
		printf("Renderer contract tests passed.\n");
	}
	return result;
}
