/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Semantic render operations consumed by GameEngineDevice.  The product
** layer publishes intent through this header; the renderer compatibility
** owner performs the selected backend translation.
*/

#pragma once

#include "Renderer/LegacyFvfLayout.h"
#include "Renderer/LegacyRenderState.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/RendererDevice.h"
#include "WW3D2/ww3dformat.h"

class DynamicIBAccessClass;
class DynamicVBAccessClass;
class IndexBufferClass;
class LightEnvironmentClass;
class Matrix3D;
class Matrix4x4;
class ShaderClass;
class SurfaceClass;
class TextureBaseClass;
class TextureClass;
class ZTextureClass;
class VertexBufferClass;
class VertexMaterialClass;
class Vector4;

namespace rts
{
namespace render
{

// Backend-neutral copy of the legacy object-space sort volume.  Keeping this
// POD in the public seam avoids making the renderer depend on WWMath's
// SphereClass while retaining the center/radius values used by deferred
// triangle sorting.
struct GameBoundingSphere
{
	GameBoundingSphere() : centerX(0.0f), centerY(0.0f), centerZ(0.0f),
		radius(0.0f) {}
	GameBoundingSphere(float x, float y, float z, float r) : centerX(x),
		centerY(y), centerZ(z), radius(r) {}

	float centerX;
	float centerY;
	float centerZ;
	float radius;
};

// Extended-stat toggles are renderer-neutral process state.  The display,
// scene and water paths all observe this one value object so a native title
// cannot accidentally diverge from the compatibility lane's diagnostics.
struct GameDebugRenderStats
{
	GameDebugRenderStats() : showingStats(false), disableTerrain(false),
		disableWater(false), disableObjects(false), disableOverhead(false),
		disableConsole(false), debugLinesToShow(-1), sleepTime(0) {}

	bool showingStats;
	bool disableTerrain;
	bool disableWater;
	bool disableObjects;
	bool disableOverhead;
	bool disableConsole;
	int debugLinesToShow;
	int sleepTime;
};

// The public WW3D device query is intentionally represented by fixed-layout
// PODs.  The owner never receives RenderDeviceDescClass, WW3D math objects,
// or a title-owned container; it only fills these caller-owned buffers.  Keep
// the string capacity stable because this contract is shared by the x86/VC6
// and native x64 adapters.
enum
{
	GAME_RENDER_DEVICE_STRING_CAPACITY = 128
};

struct GameRenderColor
{
	float red;
	float green;
	float blue;
	float alpha;
};

// Device-reset cleanup is a retained, neutral contract.  Do not add a
// virtual destructor: the x86 compatibility adapter has to preserve the
// historical two-slot vtable layout while the native owner retains only this
// interface pointer between reset/recovery callbacks.
struct GameRenderCleanupHook
{
	virtual void ReleaseResources() = 0;
	virtual void ReAcquireResources() = 0;
};

struct GameRenderResolutionDesc
{
	int width;
	int height;
	int bitDepth;
	int refreshRate;
};

struct GameRenderDeviceDesc
{
	char deviceName[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char deviceVendor[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char devicePlatform[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char driverName[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char driverVendor[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char driverVersion[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char hardwareName[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char hardwareVendor[GAME_RENDER_DEVICE_STRING_CAPACITY];
	char hardwareChipset[GAME_RENDER_DEVICE_STRING_CAPACITY];
	unsigned int adapterIndex;
};

enum GameRenderTransformSlot
{
	GAME_TRANSFORM_WORLD,
	GAME_TRANSFORM_VIEW,
	GAME_TRANSFORM_PROJECTION,
	GAME_TRANSFORM_TEXTURE0,
	GAME_TRANSFORM_TEXTURE1,
	GAME_TRANSFORM_TEXTURE2,
	GAME_TRANSFORM_TEXTURE3,
	GAME_TRANSFORM_TEXTURE4,
	GAME_TRANSFORM_TEXTURE5,
	GAME_TRANSFORM_TEXTURE6,
	GAME_TRANSFORM_TEXTURE7
};

enum GameRenderState
{
	GAME_RENDER_STATE_ALPHA_BLEND_ENABLE,
	GAME_RENDER_STATE_SOURCE_BLEND,
	GAME_RENDER_STATE_DESTINATION_BLEND,
	GAME_RENDER_STATE_COLOR_WRITE_MASK,
	GAME_RENDER_STATE_DEPTH_ENABLE,
	GAME_RENDER_STATE_DEPTH_WRITE,
	GAME_RENDER_STATE_DEPTH_FUNCTION,
	GAME_RENDER_STATE_ALPHA_TEST_ENABLE,
	GAME_RENDER_STATE_ALPHA_FUNCTION,
	GAME_RENDER_STATE_ALPHA_REFERENCE,
	GAME_RENDER_STATE_TEXTURE_FACTOR,
	GAME_RENDER_STATE_LIGHTING,
	GAME_RENDER_STATE_Z_BIAS,
	GAME_RENDER_STATE_POINT_SPRITE_ENABLE,
	GAME_RENDER_STATE_POINT_SCALE_ENABLE,
	GAME_RENDER_STATE_POINT_SIZE,
	GAME_RENDER_STATE_POINT_SIZE_MIN,
	GAME_RENDER_STATE_POINT_SIZE_MAX,
	GAME_RENDER_STATE_POINT_SCALE_A,
	GAME_RENDER_STATE_POINT_SCALE_B,
	GAME_RENDER_STATE_POINT_SCALE_C,
	// Appended to keep every pre-existing state value stable for x86/VC6
	// callers while exposing the exact fixed-function color blend operation.
	GAME_RENDER_STATE_BLEND_OPERATION,
	// Shadow/stencil states are appended so the existing x86/VC6 ABI values
	// remain unchanged while projected and volumetric shadows publish their
	// complete fixed-function intent through the neutral seam.
	GAME_RENDER_STATE_STENCIL_ENABLE,
	GAME_RENDER_STATE_STENCIL_FUNCTION,
	GAME_RENDER_STATE_STENCIL_REFERENCE,
	GAME_RENDER_STATE_STENCIL_READ_MASK,
	GAME_RENDER_STATE_STENCIL_WRITE_MASK,
	GAME_RENDER_STATE_STENCIL_FAIL_OPERATION,
	GAME_RENDER_STATE_STENCIL_DEPTH_FAIL_OPERATION,
	GAME_RENDER_STATE_STENCIL_PASS_OPERATION,
	GAME_RENDER_STATE_CULL_MODE,
	GAME_RENDER_STATE_SHADE_MODE,
	GAME_RENDER_STATE_FOG_ENABLE,
	GAME_RENDER_STATE_FILL_MODE,
	GAME_RENDER_STATE_AMBIENT_COLOR,
	// Appended state for the scaled-mesh lighting bracket.  Do not renumber
	// the historical state values consumed by VC6 callers.
	GAME_RENDER_STATE_NORMALIZE_NORMALS
};

enum GameRenderFillMode
{
	GAME_RENDER_FILL_POINT,
	GAME_RENDER_FILL_WIREFRAME,
	GAME_RENDER_FILL_SOLID
};

enum GameRenderMultiSampleMode
{
	GAME_RENDER_MULTISAMPLE_NONE = 0,
	GAME_RENDER_MULTISAMPLE_2X = 2,
	GAME_RENDER_MULTISAMPLE_4X = 4,
	GAME_RENDER_MULTISAMPLE_8X = 8
};

enum GameTextureStageState
{
	GAME_TEXTURE_STAGE_COLOR_ARGUMENT0,
	GAME_TEXTURE_STAGE_COLOR_ARGUMENT1,
	GAME_TEXTURE_STAGE_COLOR_ARGUMENT2,
	GAME_TEXTURE_STAGE_COLOR_OPERATION,
	GAME_TEXTURE_STAGE_ALPHA_ARGUMENT0,
	GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1,
	GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2,
	GAME_TEXTURE_STAGE_ALPHA_OPERATION,
	GAME_TEXTURE_STAGE_ADDRESS_U,
	GAME_TEXTURE_STAGE_ADDRESS_V,
	GAME_TEXTURE_STAGE_ADDRESS_W,
	GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER,
	GAME_TEXTURE_STAGE_MINIFICATION_FILTER,
	GAME_TEXTURE_STAGE_MIP_FILTER,
	GAME_TEXTURE_STAGE_COORDINATE_INDEX,
	GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
	// Appended to preserve all existing stage-state ordinals for VC6 callers.
	GAME_TEXTURE_STAGE_MAX_ANISOTROPY
};

// Texture arguments use the low byte for the semantic argument.  Modifiers
// deliberately live in a separate high-byte range so callers can combine
// them without importing a backend token or relying on its bit layout.
enum GameTextureArgument
{
	GAME_TEXTURE_ARGUMENT_CURRENT = 0,
	GAME_TEXTURE_ARGUMENT_DIFFUSE = 1,
	GAME_TEXTURE_ARGUMENT_TEXTURE = 2,
	GAME_TEXTURE_ARGUMENT_FACTOR = 3,
	GAME_TEXTURE_ARGUMENT_SPECULAR = 4,
	GAME_TEXTURE_ARGUMENT_TEMPORARY = 5,
	GAME_TEXTURE_ARGUMENT_COMPLEMENT = 0x00010000U,
	GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE = 0x00020000U
};

enum GameTextureCoordinateGeneration
{
	GAME_TEXTURE_COORDINATE_PASSTHROUGH = 0x00000000U,
	GAME_TEXTURE_COORDINATE_CAMERA_NORMAL = 0x00010000U,
	GAME_TEXTURE_COORDINATE_CAMERA_POSITION = 0x00020000U,
	GAME_TEXTURE_COORDINATE_CAMERA_REFLECTION = 0x00030000U
};

enum GameTextureTransformFlags
{
	GAME_TEXTURE_TRANSFORM_DISABLED = 0,
	GAME_TEXTURE_TRANSFORM_COUNT1 = 1,
	GAME_TEXTURE_TRANSFORM_COUNT2 = 2,
	GAME_TEXTURE_TRANSFORM_COUNT3 = 3,
	GAME_TEXTURE_TRANSFORM_COUNT4 = 4,
	GAME_TEXTURE_TRANSFORM_PROJECTED = 0x00000100U
};

enum GameColorWriteMask
{
	GAME_COLOR_WRITE_RED = 0x1U,
	GAME_COLOR_WRITE_GREEN = 0x2U,
	GAME_COLOR_WRITE_BLUE = 0x4U,
	GAME_COLOR_WRITE_ALPHA = 0x8U
};

enum GamePrimitiveTopology
{
	GAME_PRIMITIVE_TRIANGLE_LIST,
	GAME_PRIMITIVE_TRIANGLE_STRIP,
	GAME_PRIMITIVE_LINE_LIST,
	GAME_PRIMITIVE_LINE_STRIP,
	GAME_PRIMITIVE_POINT_LIST
};

// Fixed-function rasterization values used by shadow-volume passes.  These
// are intentionally renderer-neutral and retain the original winding intent;
// the compatibility/native owner translates them to its rasterizer contract.
enum GameRenderCullMode
{
	GAME_RENDER_CULL_NONE,
	GAME_RENDER_CULL_CLOCKWISE,
	GAME_RENDER_CULL_COUNTER_CLOCKWISE
};

enum GameRenderShadeMode
{
	GAME_RENDER_SHADE_FLAT,
	GAME_RENDER_SHADE_GOURAUD
};

// Dynamic buffer selection is part of the product render contract.  The
// Numeric values intentionally match the historical buffer-kind wire format
// consumed by the compatibility bridge, while callers no longer include a
// backend declaration just to select an immediate-mode stream.
enum GameBufferType
{
	GAME_BUFFER_TYPE_IMMEDIATE = 0,
	GAME_BUFFER_TYPE_SORTED = 1,
	GAME_BUFFER_TYPE_DYNAMIC_IMMEDIATE = 2,
	GAME_BUFFER_TYPE_DYNAMIC_SORTED = 3
};

enum GameVertexFormatFlag
{
	GAME_VERTEX_XYZ = LEGACY_FVF_XYZ,
	GAME_VERTEX_XYZRHW = LEGACY_FVF_XYZRHW,
	GAME_VERTEX_NORMAL = LEGACY_FVF_NORMAL,
	GAME_VERTEX_DIFFUSE = LEGACY_FVF_DIFFUSE,
	GAME_VERTEX_TEX1 = LEGACY_FVF_TEX1,
	GAME_VERTEX_TEX2 = LEGACY_FVF_TEX2,
	GAME_VERTEX_XYZNUV1 = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_TEX1,
	GAME_VERTEX_XYZNUV2 = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_TEX2,
	GAME_VERTEX_XYZDUV1 = LEGACY_FVF_XYZ | LEGACY_FVF_DIFFUSE |
		LEGACY_FVF_TEX1,
	GAME_VERTEX_XYZDUV2 = LEGACY_FVF_XYZ | LEGACY_FVF_DIFFUSE |
		LEGACY_FVF_TEX2,
	GAME_VERTEX_XYZNDUV1 = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_DIFFUSE | LEGACY_FVF_TEX1,
	GAME_VERTEX_XYZNDUV2 = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL |
		LEGACY_FVF_DIFFUSE | LEGACY_FVF_TEX2
};

bool IsNativeGameRendererActive();
bool IsGameRendererInitialized();
bool IsGameRenderTargetOperational();
bool IsGameRenderingToTexture();
bool GameRendererSupportsPointSprites();
bool GameRendererSupportsDot3();
bool GameRendererSupportsZBias();
bool GameRendererSupportsStencil();
bool GameRendererSupportsNPatches();
// Checks the real device/resource capability for the logical format.  The
// native implementation first resolves the logical format to its translated
// resource format and probes that owner; an unpublished owner returns false.
bool IsGameTextureFormatSupported(WW3DFormat format);
bool IsGameTerrainRenderingDisabled();
bool IsGameObjectRenderingDisabled();
bool IsGameDebugConsoleDisabled();
const GameDebugRenderStats &GetGameDebugRenderStats();
GameDebugRenderStats &GetMutableGameDebugRenderStats();
void SetGameDebugRenderStats(const GameDebugRenderStats &stats);
RenderResult ResetGameRenderFrameResources(bool frameChanged);
void BeginGameDisplayIteration();
RenderResult GetGameTextureFilterCapabilities(
	GameTextureFilterCapabilities *capabilities);
unsigned int GetGameMaxTexturesPerPass();
WW3DFormat GetGameBackBufferFormat();
void SetGameCleanupHook(GameRenderCleanupHook *hook);
void ReleaseGameSnowVertexBuffer(void *opaque);
void SetGameCursorVisible(bool visible);
void SetGameCursorProperties(int hotspotX, int hotspotY, SurfaceClass *surface);
void SetGameCursorPosition(int x, int y);

void SetGameShader(const ShaderClass &shader);
// Applies the serialized shader bitfield as one atomic logical pipeline
// update.  The native owner decodes the complete state; the compatibility
// adapter owns the historical fixed-function application algorithm.
RenderResult ApplyGameShaderBits(unsigned int shaderBits);
RenderResult SetGameShaderCullInverted(bool inverted);
void SetGameTexture(unsigned int stage, TextureBaseClass *texture);
void SetGameMaterial(const VertexMaterialClass *material);
void SetGameLightEnvironment(LightEnvironmentClass *lightEnvironment);
RenderResult SetGameFogState(const LegacyFogConstants &fog);
RenderResult SetGameLightState(unsigned int index,
	const LegacyLightState &light);
void SetGameTransform(GameRenderTransformSlot slot, const Matrix3D &matrix);
void SetGameTransform(GameRenderTransformSlot slot, const Matrix4x4 &matrix);
void SetGameTransform(GameRenderTransformSlot slot, const void *matrix);
void GetGameTransform(GameRenderTransformSlot slot, void *matrix);
void SetGameViewport(unsigned int x, unsigned int y, unsigned int width,
	unsigned int height, float minimumDepth, float maximumDepth);
void SetGameProjectionTransformWithZBias(const Matrix4x4 &projection,
	float zNear, float zFar);
void SetGameTextureBumpEnvironment(unsigned int stage, float matrix00,
	float matrix01, float matrix10, float matrix11);

void SetGameVertexBuffer(const VertexBufferClass *buffer, unsigned int stream = 0);
bool SetGameVertexBuffer(const DynamicVBAccessClass &buffer);
void SetGameIndexBuffer(const IndexBufferClass *buffer,
	unsigned short indexBaseOffset);
bool SetGameIndexBuffer(const DynamicIBAccessClass &buffer,
	unsigned short indexBaseOffset);
void SetGameIndexBufferOffset(unsigned int offset);

void SetGameRenderState(GameRenderState state, unsigned int value);
void SetGameTextureStageState(unsigned int stage, GameTextureStageState state,
	unsigned int value);
void ApplyGameRenderStateChanges();
void InvalidateGameRenderStateCache();
void InvalidateGameMeshRendererCache();

void SetGameVertexShader(unsigned int shaderOrFormat);
void SetGamePixelShader(unsigned int shader);
void SetGameLegacyVertexProgram(RenderLegacyVertexProgram program);
void SetGameLegacyPixelProgram(RenderLegacyPixelProgram program);
void SetGameVertexShaderConstant(int reg, const void *data, int count);
void SetGamePixelShaderConstant(int reg, const void *data, int count);
bool DeleteGameVertexShader(unsigned int shader);
bool DeleteGamePixelShader(unsigned int shader);
unsigned int ConvertGameColorClamp(const Vector4 &color);

void DrawGameTriangles(unsigned short startIndex, unsigned short polygonCount,
	unsigned short minVertexIndex, unsigned short vertexCount);
// Sorted submissions are deferred until the native owner flushes its ordering
// queue.  Keep this separate from immediate triangles so a sorted buffer cannot
// accidentally bypass depth/order semantics on the native path.
void DrawGameSortedTriangles(unsigned short startIndex,
	unsigned short polygonCount, unsigned short minVertexIndex,
	unsigned short vertexCount);
void DrawGameSortedTriangles(const GameBoundingSphere &boundingSphere,
	unsigned short startIndex, unsigned short polygonCount,
	unsigned short minVertexIndex, unsigned short vertexCount);
RenderResult FlushGameSortedTriangles();
void DrawGameStrip(unsigned short startIndex, unsigned short indexCount,
	unsigned short minVertexIndex, unsigned short vertexCount);
rts::render::RenderResult DrawGamePrimitiveUP(GamePrimitiveTopology topology,
	unsigned int primitiveCount, const void *vertices, unsigned int stride,
	unsigned int vertexFormat);

void SetGameRenderTarget(TextureClass *colorTexture,
	ZTextureClass *depthTexture, bool useDefaultDepth);

RenderResult GetGameBackBufferInfo(RenderBackBufferInfo *info);
RenderResult QueueGameBackBufferCapture(
	const RenderCaptureRequestDescriptor &descriptor,
	RenderCaptureHandle *handle);
unsigned int CancelGameBackBufferCaptures(void *consumer, RenderResult reason);
void RequestGameBackBufferCapture();

// Legacy-only output helpers are kept behind the same source-level contract;
// native callers use TextureClass publication and never receive a raw object.
TextureClass *CreateGameRenderTarget(int width, int height,
	WW3DFormat format);
RenderResult CreateGameRenderTargetPair(int width, int height,
	WW3DFormat colorFormat, WW3DZFormat depthFormat,
	TextureClass **colorTarget, ZTextureClass **depthTarget);
RenderResult CopyGameActiveTargetToTexture(TextureClass *destination);
// Publishes/obtains the permanent texture contents produced by the native
// projection pass.  The legacy lane returns false because its surface copy is
// still performed by the caller; native owners may keep the copy GPU-local.
bool AcquireGameCopiedTextureContent(TextureClass *destination);

// Shader assets are represented by opaque, generation-safe logical handles.
// The native owner accepts only assets for which a translated program exists;
// it never consumes the caller's backend declaration words.  The x86 adapter
// forwards the same words to its compatibility implementation.  A failed
// create leaves *handle unchanged, and deleting an unknown/stale handle is
// reported as false rather than silently succeeding.
RenderResult CreateGameShaderFromAsset(const char *assetPath,
	bool vertexShader, const void *declarationWords,
	unsigned int declarationWordCount, unsigned int usage,
	unsigned int *handle);
bool DeleteGameShader(bool vertexShader, unsigned int handle);

// WW3D lifecycle/device contract.  These calls are the only renderer
// lifecycle surface used by ww3d.cpp.  RenderResult is deliberately retained
// through the adapter so a lost, unsupported, or invalid operation cannot be
// converted into an accidental success.
RenderResult InitializeGameRenderer(void *window, unsigned int width,
	unsigned int height, bool lite, bool enableVsync);
RenderResult ShutdownGameRenderer();
RenderResult SetGameRenderDeviceByName(const char *name, int width,
	int height, int bitDepth, int windowed, bool resizeWindow);
RenderResult SetGameRenderDeviceByIndex(int device, int width, int height,
	int bitDepth, int windowed, bool resizeWindow, bool resetDevice,
	bool restoreAssets);
RenderResult SetAnyGameRenderDevice();
RenderResult SetNextGameRenderDevice();
RenderResult ToggleGameRendererWindowed();
int GetGameRenderDeviceIndex();
RenderResult GetGameRenderDeviceName(int device, char *name,
	unsigned int nameCapacity);
int GetGameRenderDeviceCount();
RenderResult GetGameRenderDeviceDesc(int device, GameRenderDeviceDesc *desc,
	GameRenderResolutionDesc *resolutions, unsigned int resolutionCapacity,
	unsigned int *resolutionCount);
RenderResult GetGameRendererResolution(int *width, int *height,
	int *bitDepth, bool *windowed);
RenderResult GetGameRendererTargetResolution(int *width, int *height,
	int *bitDepth, bool *windowed);
RenderResult SetGameRendererResolution(int width, int height, int bitDepth,
	int windowed, bool resizeWindow);
RenderResult BeginGameRender(bool clear, bool clearz,
	const GameRenderColor &color, float destinationAlpha);
RenderResult ClearGameRenderTargets(bool clear, bool clearz,
	const GameRenderColor &color, float destinationAlpha);
void SetGameAmbientColor(const GameRenderColor &color);
RenderResult EndGameRender(bool present);
RenderResult FlipGameRenderer();
RenderResult SyncGameRenderer(bool step);
RenderResult SetGameRendererSwapInterval(long interval);
long GetGameRendererSwapInterval();
RenderResult SetGameTextureBitdepth(int bitDepth);
int GetGameTextureBitdepth();
RenderResult SetGameMSAAMode(unsigned int mode);
unsigned int GetGameMSAAMode();
RenderResult SetGameGamma(float gamma, float brightness, float contrast,
	bool calibrate);

// Mesh/sort lifecycle calls keep the common WW3D adapter free of backend
// class names.  CameraClass remains opaque at this boundary; the legacy
// adapter may recover it, while the native owner only observes the command
// ordering it needs.
void SetGameRenderCamera(void *camera);
void FlushGameRenderMeshes();
void ClearGameRenderMeshPendingDeletes();
void InvalidateGameMeshCache();
unsigned int GetGameLastFramePolygonCount();
unsigned int GetGameLastFrameVertexCount();

// Process-level presentation knobs are part of the renderer seam.  The
// native implementation and the x86 compatibility adapter each define this
// same namespace-scoped storage for their respective architecture.
extern bool GameRenderer_IsWindowed;
extern int GameRenderer_PreserveFPU;

}
}
