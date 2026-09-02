/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Render-owner hook used by the native GameEngineDevice seam.  This header is
** deliberately backend-neutral: it carries no D3D, WW3D device, or title
** object types.  The owner publishes the active target kind at its existing
** NativeW3D2/renderer lifecycle boundary.
*/

#ifndef RTS_RENDERER_RENDERGAMECLIENTNATIVE_H
#define RTS_RENDERER_RENDERGAMECLIENTNATIVE_H

#include "Renderer/RendererDevice.h"

#include <stddef.h>

namespace rts
{
namespace render
{

struct GameBoundingSphere;
struct GameRenderCleanupHook;
struct NativeDrawPacket;

struct GameTextureFilterCapabilities
{
	GameTextureFilterCapabilities() : supportsPoint(false),
		supportsLinear(false), supportsAnisotropic(false), maxAnisotropy(1) {}

	bool supportsPoint;
	bool supportsLinear;
	bool supportsAnisotropic;
	unsigned int maxAnisotropy;
};

// Camera state crosses the WW3D/title boundary as a value object.  The title
// adapter owns the CameraClass-specific extraction; the native owner copies
// this complete snapshot synchronously and never retains the opaque caller
// pointer.  Matrices are row-major in the same convention as
// LegacyRenderState and viewport coordinates are pixel-space.
struct GameCameraSnapshot
{
	GameCameraSnapshot() : view(), projection(), viewport(), zNear(0.0f),
		zFar(1.0f) {}

	RenderMatrix4 view;
	RenderMatrix4 projection;
	RenderViewport viewport;
	float zNear;
	float zFar;
};

// A synchronous command envelope for the portions of the old render-client
// surface that do not need a title object.  Handles are index/generation
// values rather than backend pointers and all pointer fields below are
// caller-owned for the duration of ExecuteGameRenderCommand only.  The owner
// copies input PODs before returning and never retains input/output/consumer
// pointers.  value* fields have command-specific meanings documented by the
// command enum; unused fields must be zeroed by callers.
struct GameRenderHandle
{
	unsigned int index;
	unsigned int generation;
};

enum GameRenderCommandType
{
	GAME_RENDER_COMMAND_INVALID = 0,
	GAME_RENDER_COMMAND_SET_TEXTURE,
	GAME_RENDER_COMMAND_SET_MATERIAL,
	GAME_RENDER_COMMAND_SET_LIGHT,
	GAME_RENDER_COMMAND_SET_TRANSFORM,
	GAME_RENDER_COMMAND_GET_TRANSFORM,
	GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS,
	GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE,
	GAME_RENDER_COMMAND_SET_TEXTURE_BUMP_ENVIRONMENT,
	GAME_RENDER_COMMAND_SET_VERTEX_BUFFER,
	GAME_RENDER_COMMAND_SET_INDEX_BUFFER,
	GAME_RENDER_COMMAND_SET_INDEX_BUFFER_OFFSET,
	GAME_RENDER_COMMAND_APPLY_RENDER_STATE_CHANGES,
	GAME_RENDER_COMMAND_INVALIDATE_RENDER_STATE_CACHE,
	GAME_RENDER_COMMAND_SET_VERTEX_SHADER_CONSTANTS,
	GAME_RENDER_COMMAND_SET_PIXEL_SHADER_CONSTANTS,
	GAME_RENDER_COMMAND_DRAW_TRIANGLES,
	GAME_RENDER_COMMAND_DRAW_STRIP,
	GAME_RENDER_COMMAND_DRAW_SORTED_TRIANGLES,
	GAME_RENDER_COMMAND_DRAW_PRIMITIVE_UP,
	GAME_RENDER_COMMAND_SET_RENDER_TARGET,
	GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE,
	GAME_RENDER_COMMAND_ACQUIRE_COPIED_TEXTURE_CONTENT,
	GAME_RENDER_COMMAND_BEGIN_RENDER,
	GAME_RENDER_COMMAND_CLEAR_RENDER_TARGETS,
	GAME_RENDER_COMMAND_SET_AMBIENT_COLOR,
	GAME_RENDER_COMMAND_END_RENDER,
	GAME_RENDER_COMMAND_FLIP_RENDERER,
	GAME_RENDER_COMMAND_SYNC_RENDERER,
	GAME_RENDER_COMMAND_SET_SWAP_INTERVAL,
	GAME_RENDER_COMMAND_GET_SWAP_INTERVAL,
	GAME_RENDER_COMMAND_SET_TEXTURE_BITDEPTH,
	GAME_RENDER_COMMAND_GET_TEXTURE_BITDEPTH,
	GAME_RENDER_COMMAND_SET_MSAA_MODE,
	GAME_RENDER_COMMAND_GET_MSAA_MODE,
	GAME_RENDER_COMMAND_SET_GAMMA,
	GAME_RENDER_COMMAND_CAPTURE_BACKBUFFER,
	GAME_RENDER_COMMAND_GET_BACKBUFFER_INFO,
	GAME_RENDER_COMMAND_SET_RESOLUTION,
	GAME_RENDER_COMMAND_SET_DEVICE_BY_NAME,
	GAME_RENDER_COMMAND_SET_DEVICE_BY_INDEX,
	GAME_RENDER_COMMAND_SET_ANY_DEVICE,
	GAME_RENDER_COMMAND_SET_NEXT_DEVICE,
	GAME_RENDER_COMMAND_TOGGLE_WINDOWED,
	GAME_RENDER_COMMAND_GET_DEVICE_INDEX,
	GAME_RENDER_COMMAND_GET_DEVICE_NAME,
	GAME_RENDER_COMMAND_GET_DEVICE_COUNT,
	GAME_RENDER_COMMAND_GET_DEVICE_DESC,
	GAME_RENDER_COMMAND_GET_RESOLUTION,
	GAME_RENDER_COMMAND_GET_TARGET_RESOLUTION
};

// Command payload map (all sizes are exact and all pointer data is
// synchronous).  `resource0/1` are GameRenderHandle values; `value0..5`,
// signedValue*, signedLongValue and float* have no implicit backend encoding.
//
// SET_TEXTURE: value0=stage, resource0=texture (invalid clears the stage).
// SET_MATERIAL: input=LegacyVertexMaterialState, inputBytes=sizeof(...).
// SET_LIGHT: value0=light index, input=LegacyLightState,
// inputBytes=sizeof(...).
// SET_TRANSFORM: value0=LegacyTransformSlot, input=RenderMatrix4,
// inputBytes=sizeof(RenderMatrix4).
// GET_TRANSFORM: value0=LegacyTransformSlot, output=RenderMatrix4,
// outputBytes=sizeof(RenderMatrix4).
// SET_PROJECTION_WITH_Z_BIAS: input=RenderMatrix4,
// inputBytes=sizeof(RenderMatrix4), float0=zNear, float1=zFar. The owner
// applies the no-hardware-bias projection fallback and stores both depths.
// SET_TEXTURE_STAGE_STATE: value0=stage, value1=GameTextureStageState,
// value2=semantic value.  SET_TEXTURE_BUMP_ENVIRONMENT uses value0=stage and
// float0..3=(00,01,10,11).
// SET_VERTEX_BUFFER: resource0=buffer, value0=legacy FVF, value1=stride,
// value2=byte offset, value3=stream (only stream zero is representable).
// A sorted CPU binding instead has an invalid resource0 and copies input:
// value0=legacy FVF, value1=stride, value2=minimum source vertex,
// value3=vertex count, value4=source byte offset, and inputBytes must equal
// value3*value1. The copied bytes begin at the selected minimum vertex.
// SET_INDEX_BUFFER: resource0=buffer, value0=RenderFormat,
// value1=byte offset, signedValue0=base vertex offset.
// A sorted CPU binding instead has an invalid resource0 and copies input:
// value0=RENDER_FORMAT_R16_UINT, value1=source start index, value2=index
// count, and inputBytes must equal value2*sizeof(unsigned short).
// SET_INDEX_BUFFER_OFFSET: value0=base vertex offset.
// APPLY_RENDER_STATE_CHANGES and INVALIDATE_RENDER_STATE_CACHE have no
// payload and complete the corresponding logical cache operation.
// SET_*_SHADER_CONSTANTS: value0=start register, value1=register count,
// input points to count*4 floats and inputBytes is the exact byte count.
// DRAW_TRIANGLES: value0=start index, value1=polygon count,
// value2=min vertex index, value3=vertex count.
// DRAW_STRIP: value0=start index, value1=index count, value2=min vertex index,
// value3=vertex count.
// DRAW_SORTED_TRIANGLES: value0=start index, value1=polygon count,
// value2=min vertex index, value3=vertex count, boundingSphere is copied
// synchronously when non-null. The owner uses the CPU bindings above and
// queues deterministic per-triangle ordering.
// DRAW_PRIMITIVE_UP: value0=RenderPrimitiveTopology, value1=primitive count,
// value2=vertex stride, value3=legacy FVF, input=vertex bytes,
// inputBytes is the exact copied byte count.
// SET_RENDER_TARGET: input=RenderTargetBinding,
// inputBytes=sizeof(RenderTargetBinding); the owner copies it before return.
// COPY_ACTIVE_TARGET_TO_TEXTURE: resource0=destination texture,
// output=NativeW3DGpuContentLease, outputBytes=sizeof(...), or output null
// when no lease is requested.  ACQUIRE_COPIED_TEXTURE_CONTENT uses resource0,
// with the same optional lease output.
// BEGIN_RENDER: value0=RenderClearFlags, float0..3=color RGBA, float4=depth,
// value1=stencil. CLEAR_RENDER_TARGETS has the same payload but does not open
// or close a frame. SET_AMBIENT_COLOR uses float0..3=ambient RGBA.
// END_RENDER: value0=present flag. FLIP_RENDERER has no payload.
// SYNC_RENDERER: value0=step flag. SET_SWAP_INTERVAL uses signedLongValue and
// GET_SWAP_INTERVAL writes a long at output/outputBytes=sizeof(long).
// SET_TEXTURE_BITDEPTH uses signedValue0 and GET_TEXTURE_BITDEPTH writes int.
// SET_MSAA_MODE uses value0 and GET_MSAA_MODE writes unsigned int.
// SET_GAMMA uses float0=gamma, float1=brightness, float2=contrast,
// value0=calibrate flag. These display-policy commands are accepted only by
// an owner that can apply them to the active device.
// CAPTURE_BACKBUFFER: output=pixel destination, outputBytes=capacity,
// value0=row pitch, secondaryOutput=RenderFormat output slot (optional),
// secondaryOutputBytes=sizeof(RenderFormat) when supplied. GET_BACKBUFFER_INFO writes
// RenderBackBufferInfo to output/outputBytes=sizeof(...).
// SET_RESOLUTION: value0=width, value1=height, signedValue0=bit depth,
// value2=windowed flag, value3=resize-window flag.
// Device-selection/query commands are pre-frame policy operations: NAME uses
// input NUL-terminated name and output char buffer (value0=capacity), INDEX
// uses signedValue0=adapter and value0..3=(width,height,bit depth,windowed),
// value4=resize, value5=reset, signedValue1=restore-assets; ANY/NEXT/TOGGLE
// have no payload; GET_DEVICE_INDEX/COUNT write int; GET_DEVICE_NAME uses
// signedValue0=device and output char buffer/value0=capacity; GET_DEVICE_DESC
// uses signedValue0=device, output=GameRenderDeviceDesc/outputBytes exact,
// secondaryOutput=caller-owned GameRenderResolutionDesc array,
// secondaryOutputBytes=its byte capacity, value0=element capacity, and
// outputCount points to the number of descriptors written (zero on failure).
// GET_RESOLUTION and GET_TARGET_RESOLUTION write four scalar values as int[4]
// to output/outputBytes=sizeof(int[4]).

struct GameRenderCommand
{
	GameRenderCommandType type;
	unsigned int value0;
	unsigned int value1;
	unsigned int value2;
	unsigned int value3;
	unsigned int value4;
	unsigned int value5;
	int signedValue0;
	int signedValue1;
	long signedLongValue;
	float float0;
	float float1;
	float float2;
	float float3;
	float float4;
	float float5;
	GameRenderHandle resource0;
	GameRenderHandle resource1;
	const void *input;
	size_t inputBytes;
	void *output;
	size_t outputBytes;
	void *secondaryOutput;
	size_t secondaryOutputBytes;
	unsigned int *outputCount;
	void *consumer;
	const GameBoundingSphere *boundingSphere;
};

enum GameRenderTargetKind
{
	GAME_RENDER_TARGET_UNKNOWN = 0,
	GAME_RENDER_TARGET_BACK_BUFFER,
	GAME_RENDER_TARGET_TEXTURE
};

// The native implementation observes the already-existing render owner
// through this narrow capability contract.  It never constructs a device and
// never keeps a second backend.  Publication and removal are render-owner
// operations: publish only after initialization/target state is valid, clear
// before teardown or backend replacement, and do not mutate the pointer from
// worker threads while a query is in flight.
class IGameRenderClientNativeOwner
{
public:
	virtual ~IGameRenderClientNativeOwner() {}
	virtual bool IsInitialized() const = 0;
	virtual bool IsOperational() const = 0;
	// Owner-thread resource callbacks may release logical objects while the
	// backend is unavailable. Creation still validates backend readiness in
	// the concrete owner; this does not admit frame or draw commands.
	virtual bool IsRebuildingResources() const { return false; }
	virtual GameRenderTargetKind ActiveRenderTargetKind() const = 0;
	// Executes one complete synchronous command.  Commands are never queued
	// behind this boundary: a non-OK result means the requested operation did
	// not become visible and the owner must not retain any envelope pointer.
	virtual RenderResult ExecuteGameRenderCommand(
		const GameRenderCommand &command)
	{
		(void)command;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult SetGameViewport(const RenderViewport &viewport)
	{
		(void)viewport;
		return RENDER_RESULT_UNSUPPORTED;
	}
	// The shader bitfield is the serialized WW3D contract.  Implementations
	// must decode it as one state transaction and return failure instead of
	// leaving a partially updated pipeline visible to the next draw.
	virtual RenderResult ApplyGameShaderBits(unsigned int shaderBits)
	{
		(void)shaderBits;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult SetGameRenderState(unsigned int state,
		unsigned int value)
	{
		(void)state;
		(void)value;
		return RENDER_RESULT_UNSUPPORTED;
	}
	// This is intentionally a WW3D-layer escape hatch.  The caller passes the
	// active CameraClass synchronously; the native owner copies the camera's
	// matrices/viewport and never retains the pointer beyond this call.
	virtual void SetGameRenderCamera(void *camera)
	{
		(void)camera;
	}
	virtual RenderResult SetGameRenderCameraSnapshot(
		const GameCameraSnapshot &snapshot)
	{
		(void)snapshot;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult SetGameShaderCullInverted(bool inverted)
	{
		(void)inverted;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual void SetGameCleanupHook(GameRenderCleanupHook *hook)
	{
		(void)hook;
	}
	virtual RenderResult CreateGameShaderFromAsset(const char *assetPath,
		bool vertexShader, const void *declarationWords,
		unsigned int declarationWordCount, unsigned int usage,
		unsigned int *handle)
	{
		(void)assetPath;
		(void)vertexShader;
		(void)declarationWords;
		(void)declarationWordCount;
		(void)usage;
		if (handle != 0)
			*handle = 0;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual bool DeleteGameShader(bool vertexShader, unsigned int handle)
	{
		(void)vertexShader;
		(void)handle;
		return false;
	}
	virtual void SetGameVertexShader(unsigned int shaderOrFormat)
	{
		(void)shaderOrFormat;
	}
	virtual void SetGamePixelShader(unsigned int shader)
	{
		(void)shader;
	}
	virtual void SetGameLegacyVertexProgram(RenderLegacyVertexProgram program)
	{
		(void)program;
	}
	virtual void SetGameLegacyPixelProgram(RenderLegacyPixelProgram program)
	{
		(void)program;
	}
	virtual RenderResult SubmitGameTriangles(
		const LegacyLogicalState &state, const NativeDrawPacket &)
	{
		(void)state;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult QueueGameSortedTriangles(
		const LegacyLogicalState &state, const NativeDrawPacket &,
		const void *vertexData, size_t vertexBytes, const void *indexData,
		size_t indexBytes, const GameBoundingSphere *boundingSphere)
	{
		(void)state;
		(void)vertexData;
		(void)vertexBytes;
		(void)indexData;
		(void)indexBytes;
		(void)boundingSphere;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult FlushGameSortedTriangles()
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult BeginGameDisplayIteration()
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult ResetGameRenderFrameResources(bool frameChanged)
	{
		(void)frameChanged;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual bool SupportsPointSprites() const { return false; }
	virtual bool SupportsDot3() const { return false; }
	virtual bool SupportsZBias() const { return false; }
	virtual bool SupportsStencil() const { return false; }
	virtual bool SupportsNPatches() const { return false; }
	// Reports support for the actual native resource format selected by the
	// logical WW3D format mapper.  This is deliberately distinct from a source
	// upload predicate: a caller must not infer device support merely because a
	// CPU conversion exists.  Unpublished owners fail closed.
	virtual bool IsGameTextureFormatSupported(RenderFormat format) const
	{
		(void)format;
		return false;
	}
	virtual RenderResult SetGameFogState(const LegacyFogConstants &fog)
	{
		(void)fog;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult SetGameLightState(unsigned int index,
		const LegacyLightState &light)
	{
		(void)index;
		(void)light;
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual bool IsDebugConsoleDisabled() const { return false; }
	virtual bool IsGameRenderingToTexture() const
	{
		return ActiveRenderTargetKind() == GAME_RENDER_TARGET_TEXTURE;
	}
	virtual RenderResult GetTextureFilterCapabilities(
		GameTextureFilterCapabilities *capabilities) const
	{
		if (capabilities != 0)
		{
			*capabilities = GameTextureFilterCapabilities();
		}
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual unsigned int GetMaxTexturesPerPass() const
	{
		return 0;
	}
	virtual RenderResult InvalidateGameMeshRendererCache()
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual RenderResult GetGameBackBufferInfo(
		RenderBackBufferInfo *info) const
	{
		if (info != 0)
			*info = RenderBackBufferInfo();
		return RENDER_RESULT_UNSUPPORTED;
	}
	// Capture admission is asynchronous.  The owner copies this descriptor
	// and retains only the callback/consumer values until completion or
	// cancellation; no caller-owned command envelope is retained.
	virtual RenderResult QueueGameBackBufferCapture(
		const RenderCaptureRequestDescriptor &descriptor,
		RenderCaptureHandle *handle)
	{
		(void)descriptor;
		if (handle != 0)
			*handle = RenderCaptureHandle();
		return RENDER_RESULT_UNSUPPORTED;
	}
	virtual unsigned int CancelGameBackBufferCaptures(
		void *consumer, RenderResult reason)
	{
		(void)consumer;
		(void)reason;
		return 0;
	}
	virtual void RequestGameBackBufferCapture() {}
	virtual void RecordGameFailure(RenderResult result) { (void)result; }
};

// These functions are a lifecycle hook for the existing native owner, not a
// backend selector.  A null owner means that no native target is published.
IGameRenderClientNativeOwner *GetGameRenderClientNativeOwner();
void SetGameRenderClientNativeOwner(IGameRenderClientNativeOwner *owner);

// Lifecycle entry points must reject reentry before mutating resources or
// invoking callbacks. A same-thread command/lifecycle pin cannot be released
// by a nested shutdown, even though queries may safely reenter the facade.
bool IsNativeGameRenderOwnerPinnedByCurrentThread();

// The published owner is an aggregate whose lifetime includes its renderer,
// resource registry, and backend context. A query must pin that aggregate for
// the complete virtual call; loading the pointer and dereferencing it later is
// a use-after-detach race. The implementation keeps the gate type opaque so
// this public contract remains C++98/VC6-compatible.
class NativeGameRenderOwnerScope
{
public:
	NativeGameRenderOwnerScope();
	~NativeGameRenderOwnerScope();

	IGameRenderClientNativeOwner *Get() const;

private:
	NativeGameRenderOwnerScope(const NativeGameRenderOwnerScope &);
	NativeGameRenderOwnerScope &operator=(
		const NativeGameRenderOwnerScope &);

	IGameRenderClientNativeOwner *m_owner;
	bool m_locked;
};

// Lifecycle code holds this same gate across clear/publish and teardown.
// Reentrant acquisition is rejected: callers must check IsAcquired before
// teardown, rather than deleting an aggregate still pinned by a callback.
class NativeGameRenderOwnerLifecycleScope
{
public:
	NativeGameRenderOwnerLifecycleScope();
	~NativeGameRenderOwnerLifecycleScope();

	bool IsAcquired() const;
	IGameRenderClientNativeOwner *Get() const;
	void Publish(IGameRenderClientNativeOwner *owner);

private:
	NativeGameRenderOwnerLifecycleScope(
		const NativeGameRenderOwnerLifecycleScope &);
	NativeGameRenderOwnerLifecycleScope &operator=(
		const NativeGameRenderOwnerLifecycleScope &);

	bool m_locked;
};

// True only for an initialized, operational owner that reports its active
// target as a texture.  Unknown/unpublished state is deliberately false and
// must not be interpreted as proof that the back buffer is active.
inline bool IsGameRenderingToTextureForOwner(
	const IGameRenderClientNativeOwner *owner)
{
	return owner != 0 && owner->IsInitialized() && owner->IsOperational() &&
		owner->ActiveRenderTargetKind() == GAME_RENDER_TARGET_TEXTURE;
}

}
}

#endif
