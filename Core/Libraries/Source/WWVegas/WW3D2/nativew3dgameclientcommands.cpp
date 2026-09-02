/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Native GameEngineDevice command facade.  The title-facing WW3D2 API is
** kept in this translation unit while the published owner receives only
** synchronous renderer-neutral commands.  Every call pins the owner for the
** entire virtual dispatch; no title object or caller-owned command envelope
** is retained by the native owner.
*/

#include "Utility/CppMacros.h"
#include "Renderer/LegacyColorPacking.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/NativeW3DResources.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "nativew3d2.h"
#include "surfaceclass.h"
#include "texture.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"
#include "WWMath/vector4.h"

#include <float.h>
#include <limits.h>
#include <string.h>
#include <atomic>

#if defined(_WIN64)

namespace
{

using namespace rts::render;

// Texture bit depth is a legacy logical policy value.  It is intentionally
// independent from the native D3D11 resource format and therefore must not be
// sent through the aggregate command path.  Stage4 defaulted this value to
// 16 and accepted only the historical 16/32 choices.
std::atomic<int> g_native_texture_bit_depth(16);

bool IsOperationalOwner(IGameRenderClientNativeOwner *owner)
{
	return owner != 0 && owner->IsInitialized() && owner->IsOperational();
}

// Shader handles are logical title resources.  During a device/resource
// rebuild the owner intentionally closes frame/draw admission, but cleanup
// callbacks still need to delete old handles and recreate their logical
// shader entries.  The concrete owner separately verifies that its backend
// is ready before creating anything; this outer predicate only opens the
// narrow lifecycle seam and never admits ordinary rendering commands.
bool IsShaderAdmissionOwner(IGameRenderClientNativeOwner *owner)
{
	return owner != 0 && owner->IsInitialized() &&
		(owner->IsOperational() || owner->IsRebuildingResources());
}

void InitializeCommand(GameRenderCommand *command,
	GameRenderCommandType type)
{
	memset(command, 0, sizeof(*command));
	command->type = type;
}

RenderResult SubmitCommand(IGameRenderClientNativeOwner *owner,
	const GameRenderCommand &command)
{
	if (!IsOperationalOwner(owner))
	{
		if (owner != 0)
			owner->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = owner->ExecuteGameRenderCommand(command);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

RenderResult DispatchCommand(const GameRenderCommand &command)
{
	NativeGameRenderOwnerScope scope;
	return SubmitCommand(scope.Get(), command);
}

GameRenderHandle ToGameHandle(const GpuHandle &handle)
{
	GameRenderHandle result = { 0, 0 };
	if (handle.isValid())
	{
		result.index = handle.index();
		result.generation = handle.generation();
	}
	return result;
}

GameRenderHandle ToGameHandle(const NativeW3DTextureHandle &handle)
{
	return ToGameHandle(handle.resource);
}

bool AcquireTextureHandle(TextureBaseClass *texture,
	GameRenderHandle *handle)
{
	if (handle == 0)
		return false;
	*handle = GameRenderHandle();
	if (texture == 0)
		return true;
	NativeW3DTextureHandle nativeHandle;
	if (!texture->Acquire_Native_Texture(&nativeHandle) ||
		!nativeHandle.isValid())
		return false;
	*handle = ToGameHandle(nativeHandle);
	return handle->index != 0 || handle->generation != 0;
}

bool AcquireTextureResource(TextureBaseClass *texture, GpuHandle *resource)
{
	if (resource == 0)
		return false;
	*resource = GpuHandle();
	if (texture == 0)
		return true;
	NativeW3DTextureHandle nativeHandle;
	if (!texture->Acquire_Native_Texture(&nativeHandle) ||
		!nativeHandle.isValid())
		return false;
	*resource = nativeHandle.resource;
	return resource->isValid();
}

void CopyMatrix(const Matrix3D &source, RenderMatrix4 *destination)
{
	for (unsigned int row = 0; row < 3; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
			destination->values[row * 4 + column] = source[row][column];
	}
	destination->values[12] = 0.0f;
	destination->values[13] = 0.0f;
	destination->values[14] = 0.0f;
	destination->values[15] = 1.0f;
}

void CopyMatrix(const Matrix4x4 &source, RenderMatrix4 *destination)
{
	for (unsigned int row = 0; row < 4; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
			destination->values[row * 4 + column] = source[row][column];
	}
}

void CopyMatrixFromLegacy(const void *source, RenderMatrix4 *destination)
{
	memcpy(destination->values, source, sizeof(destination->values));
}

bool IsFiniteFloat(float value)
{
	return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

bool IsValidColor(const GameRenderColor &color)
{
	return IsFiniteFloat(color.red) && IsFiniteFloat(color.green) &&
		IsFiniteFloat(color.blue) && IsFiniteFloat(color.alpha);
}

void SetClearCommandFields(GameRenderCommand *command, bool clear,
	bool clearz, const GameRenderColor &color, float destinationAlpha)
{
	command->value0 = (clear ? RENDER_CLEAR_COLOR : 0U) |
		(clearz ? RENDER_CLEAR_DEPTH : 0U);
	command->value1 = 0;
	command->float0 = color.red;
	command->float1 = color.green;
	command->float2 = color.blue;
	command->float3 = destinationAlpha;
	command->float4 = 1.0f;
}

bool SetStaticVertexBufferCommand(GameRenderCommand *command,
	const VertexBufferClass *buffer, unsigned int stream)
{
	if (command == 0 || stream != 0U)
		return false;
	if (buffer == 0)
		return true;
	if (buffer->Type() != BUFFER_TYPE_DX8 || buffer->Get_Vertex_Count() == 0)
		return false;
	const DX8VertexBufferClass *nativeBuffer =
		static_cast<const DX8VertexBufferClass *>(buffer);
	GpuHandle handle;
	const unsigned int stride = buffer->FVF_Info().Get_FVF_Size();
	if (stride == 0 || !nativeBuffer->Acquire_Native_Vertex_Buffer(stride,
		0, 0, buffer->Get_Vertex_Count(), &handle))
		return false;
	command->resource0 = ToGameHandle(handle);
	command->value0 = buffer->FVF_Info().Get_FVF();
	command->value1 = stride;
	command->value2 = 0;
	command->value3 = stream;
	return true;
}

bool SetStaticIndexBufferCommand(GameRenderCommand *command,
	const IndexBufferClass *buffer, unsigned short indexBaseOffset)
{
	if (command == 0)
		return false;
	command->value0 = RENDER_FORMAT_R16_UINT;
	command->signedValue0 = indexBaseOffset;
	if (buffer == 0)
		return true;
	if (buffer->Type() != BUFFER_TYPE_DX8 || buffer->Get_Index_Count() == 0)
		return false;
	const DX8IndexBufferClass *nativeBuffer =
		static_cast<const DX8IndexBufferClass *>(buffer);
	GpuHandle handle;
	if (!nativeBuffer->Acquire_Native_Index_Buffer(0, 0,
		buffer->Get_Index_Count(), &handle))
		return false;
	command->resource0 = ToGameHandle(handle);
	return true;
}

}

namespace rts
{
namespace render
{

RenderResult ApplyGameShaderBits(unsigned int shaderBits)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result = owner->ApplyGameShaderBits(shaderBits);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

void SetGameTexture(unsigned int stage, TextureBaseClass *texture)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_TEXTURE);
	command.value0 = stage;
	if (!AcquireTextureHandle(texture, &command.resource0))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	(void)DispatchCommand(command);
}

void SetGameTransform(GameRenderTransformSlot slot, const Matrix3D &matrix)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_TRANSFORM);
	RenderMatrix4 converted;
	CopyMatrix(matrix, &converted);
	command.value0 = static_cast<unsigned int>(slot);
	command.input = &converted;
	command.inputBytes = sizeof(converted);
	(void)DispatchCommand(command);
}

void SetGameTransform(GameRenderTransformSlot slot, const Matrix4x4 &matrix)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_TRANSFORM);
	RenderMatrix4 converted;
	CopyMatrix(matrix, &converted);
	command.value0 = static_cast<unsigned int>(slot);
	command.input = &converted;
	command.inputBytes = sizeof(converted);
	(void)DispatchCommand(command);
}

void SetGameTransform(GameRenderTransformSlot slot, const void *matrix)
{
	if (matrix == 0)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_TRANSFORM);
	RenderMatrix4 converted;
	CopyMatrixFromLegacy(matrix, &converted);
	command.value0 = static_cast<unsigned int>(slot);
	command.input = &converted;
	command.inputBytes = sizeof(converted);
	(void)DispatchCommand(command);
}

void GetGameTransform(GameRenderTransformSlot slot, void *matrix)
{
	if (matrix == 0)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_GET_TRANSFORM);
	RenderMatrix4 converted;
	command.value0 = static_cast<unsigned int>(slot);
	command.output = &converted;
	command.outputBytes = sizeof(converted);
	NativeGameRenderOwnerScope scope;
	if (SubmitCommand(scope.Get(), command) == RENDER_RESULT_OK)
		memcpy(matrix, converted.values, sizeof(converted.values));
}

void SetGameViewport(unsigned int x, unsigned int y, unsigned int width,
	unsigned int height, float minimumDepth, float maximumDepth)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	const RenderViewport viewport(static_cast<float>(x), static_cast<float>(y),
		static_cast<float>(width), static_cast<float>(height), minimumDepth,
		maximumDepth);
	const RenderResult result = owner->SetGameViewport(viewport);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
}

void SetGameProjectionTransformWithZBias(const Matrix4x4 &projection,
	float zNear, float zFar)
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS);
	RenderMatrix4 converted;
	CopyMatrix(projection, &converted);
	command.input = &converted;
	command.inputBytes = sizeof(converted);
	command.float0 = zNear;
	command.float1 = zFar;
	(void)DispatchCommand(command);
}

void SetGameTextureBumpEnvironment(unsigned int stage, float matrix00,
	float matrix01, float matrix10, float matrix11)
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_SET_TEXTURE_BUMP_ENVIRONMENT);
	command.value0 = stage;
	command.float0 = matrix00;
	command.float1 = matrix01;
	command.float2 = matrix10;
	command.float3 = matrix11;
	(void)DispatchCommand(command);
}

void SetGameTextureStageState(unsigned int stage, GameTextureStageState state,
	unsigned int value)
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE);
	command.value0 = stage;
	command.value1 = static_cast<unsigned int>(state);
	command.value2 = value;
	(void)DispatchCommand(command);
}

void SetGameRenderState(GameRenderState state, unsigned int value)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	const RenderResult result = owner->SetGameRenderState(
		static_cast<unsigned int>(state), value);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
}

void ApplyGameRenderStateChanges()
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_APPLY_RENDER_STATE_CHANGES);
	(void)DispatchCommand(command);
}

void InvalidateGameRenderStateCache()
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_INVALIDATE_RENDER_STATE_CACHE);
	(void)DispatchCommand(command);
}

void SetGameAmbientColor(const GameRenderColor &color)
{
	if (!IsValidColor(color))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_AMBIENT_COLOR);
	command.float0 = color.red;
	command.float1 = color.green;
	command.float2 = color.blue;
	command.float3 = color.alpha;
	(void)DispatchCommand(command);
}

void SetGameVertexShader(unsigned int shaderOrFormat)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	owner->SetGameVertexShader(shaderOrFormat);
}

void SetGamePixelShader(unsigned int shader)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	owner->SetGamePixelShader(shader);
}

void SetGameLegacyVertexProgram(RenderLegacyVertexProgram program)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	owner->SetGameLegacyVertexProgram(program);
}

void SetGameLegacyPixelProgram(RenderLegacyPixelProgram program)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return;
	owner->SetGameLegacyPixelProgram(program);
}

void SetGameVertexShaderConstant(int reg, const void *data, int count)
{
	if (reg < 0 || count <= 0 || data == 0 ||
		static_cast<unsigned int>(reg) >= LEGACY_VERTEX_CONSTANT_COUNT ||
		static_cast<unsigned int>(count) > LEGACY_VERTEX_CONSTANT_COUNT -
			static_cast<unsigned int>(reg))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_SET_VERTEX_SHADER_CONSTANTS);
	command.value0 = static_cast<unsigned int>(reg);
	command.value1 = static_cast<unsigned int>(count);
	command.input = data;
	command.inputBytes = static_cast<size_t>(count) * sizeof(RenderFloat4);
	(void)DispatchCommand(command);
}

void SetGamePixelShaderConstant(int reg, const void *data, int count)
{
	if (reg < 0 || count <= 0 || data == 0 ||
		static_cast<unsigned int>(reg) >= LEGACY_PIXEL_CONSTANT_COUNT ||
		static_cast<unsigned int>(count) > LEGACY_PIXEL_CONSTANT_COUNT -
			static_cast<unsigned int>(reg))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_SET_PIXEL_SHADER_CONSTANTS);
	command.value0 = static_cast<unsigned int>(reg);
	command.value1 = static_cast<unsigned int>(count);
	command.input = data;
	command.inputBytes = static_cast<size_t>(count) * sizeof(RenderFloat4);
	(void)DispatchCommand(command);
}

void SetGameVertexBuffer(const VertexBufferClass *buffer, unsigned int stream)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_VERTEX_BUFFER);
	if (!SetStaticVertexBufferCommand(&command, buffer, stream))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(stream != 0U ?
				RENDER_RESULT_UNSUPPORTED : RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	(void)DispatchCommand(command);
}

bool SetGameVertexBuffer(const DynamicVBAccessClass &buffer)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_VERTEX_BUFFER);
	const unsigned int type = buffer.Get_Type();
	const unsigned int stride = buffer.Get_Vertex_Stride();
	const unsigned int count = buffer.Get_Vertex_Count();
	const unsigned int fvf = buffer.FVF_Info().Get_FVF();
	if (stride == 0U || count == 0U || fvf == 0U || !buffer.Is_Valid())
		goto invalid_buffer;
	if (type == BUFFER_TYPE_DYNAMIC_DX8)
	{
		GpuHandle handle;
		if (!buffer.Acquire_Native_Vertex_Buffer(&handle) || !handle.isValid())
			goto invalid_buffer;
		command.resource0 = ToGameHandle(handle);
		command.value0 = fvf;
		command.value1 = stride;
		command.value2 = buffer.Get_Vertex_Buffer_Offset();
		command.value3 = 0;
	}
	else if (type == BUFFER_TYPE_DYNAMIC_SORTING)
	{
		const void *data = buffer.Get_Sorted_Vertex_Data();
		const unsigned int byteOffset = buffer.Get_Vertex_Buffer_Offset();
		if (data == 0 || byteOffset % stride != 0U)
			goto invalid_buffer;
		command.value0 = fvf;
		command.value1 = stride;
		// The native command stores the absolute source vertex minimum and
		// byte offset separately.  The pointer already addresses that minimum.
		command.value2 = byteOffset / stride;
		command.value3 = count;
		command.value4 = byteOffset;
		command.input = data;
		command.inputBytes = static_cast<size_t>(count) * stride;
	}
	else
		goto invalid_buffer;
	return DispatchCommand(command) == RENDER_RESULT_OK;

invalid_buffer:
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
	}
	return false;
}

void SetGameIndexBuffer(const IndexBufferClass *buffer,
	unsigned short indexBaseOffset)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_INDEX_BUFFER);
	if (!SetStaticIndexBufferCommand(&command, buffer, indexBaseOffset))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	(void)DispatchCommand(command);
}

bool SetGameIndexBuffer(const DynamicIBAccessClass &buffer,
	unsigned short indexBaseOffset)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_INDEX_BUFFER);
	const unsigned int type = buffer.Get_Type();
	const unsigned int count = buffer.Get_Index_Count();
	if (count == 0U || !buffer.Is_Valid())
		goto invalid_buffer;
	command.value0 = RENDER_FORMAT_R16_UINT;
	command.signedValue0 = indexBaseOffset;
	if (type == BUFFER_TYPE_DYNAMIC_DX8)
	{
		GpuHandle handle;
		if (!buffer.Acquire_Native_Index_Buffer(&handle) || !handle.isValid())
			goto invalid_buffer;
		command.resource0 = ToGameHandle(handle);
		command.value1 = buffer.Get_Index_Buffer_Offset();
	}
	else if (type == BUFFER_TYPE_DYNAMIC_SORTING)
	{
		const unsigned short *data = buffer.Get_Sorted_Index_Data();
		const unsigned int byteOffset = buffer.Get_Index_Buffer_Offset();
		if (data == 0 || (byteOffset % sizeof(unsigned short)) != 0U)
			goto invalid_buffer;
		// The command's source index is the byte offset expressed in R16
		// elements.  The pointer already addresses that selected element.
		command.value1 = byteOffset / sizeof(unsigned short);
		command.value2 = count;
		command.input = data;
		command.inputBytes = static_cast<size_t>(count) *
			sizeof(unsigned short);
	}
	else
		goto invalid_buffer;
	return DispatchCommand(command) == RENDER_RESULT_OK;

invalid_buffer:
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
	}
	return false;
}

void SetGameIndexBufferOffset(unsigned int offset)
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_SET_INDEX_BUFFER_OFFSET);
	command.value0 = offset;
	(void)DispatchCommand(command);
}

bool DeleteGameShader(bool vertexShader, unsigned int handle)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsShaderAdmissionOwner(owner) || handle == 0U)
	{
		if (owner != 0)
			owner->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return false;
	}
	const bool deleted = owner->DeleteGameShader(vertexShader, handle);
	if (!deleted)
		owner->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
	return deleted;
}

bool DeleteGameVertexShader(unsigned int shader)
{
	return DeleteGameShader(true, shader);
}

bool DeleteGamePixelShader(unsigned int shader)
{
	return DeleteGameShader(false, shader);
}

RenderResult CreateGameShaderFromAsset(const char *assetPath,
	bool vertexShader, const void *declarationWords,
	unsigned int declarationWordCount, unsigned int usage,
	unsigned int *handle)
{
	if (assetPath == 0 || assetPath[0] == '\0' || handle == 0)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsShaderAdmissionOwner(owner))
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result = owner->CreateGameShaderFromAsset(assetPath,
		vertexShader, declarationWords, declarationWordCount, usage, handle);
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

unsigned int ConvertGameColorClamp(const Vector4 &color)
{
	return PackLegacyARGB(color[0], color[1], color[2], color[3]);
}

void DrawGameTriangles(unsigned short startIndex, unsigned short polygonCount,
	unsigned short minVertexIndex, unsigned short vertexCount)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_DRAW_TRIANGLES);
	command.value0 = startIndex;
	command.value1 = polygonCount;
	command.value2 = minVertexIndex;
	command.value3 = vertexCount;
	(void)DispatchCommand(command);
}

void DrawGameStrip(unsigned short startIndex, unsigned short indexCount,
	unsigned short minVertexIndex, unsigned short vertexCount)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_DRAW_STRIP);
	command.value0 = startIndex;
	command.value1 = indexCount;
	command.value2 = minVertexIndex;
	command.value3 = vertexCount;
	(void)DispatchCommand(command);
}

void DrawGameSortedTriangles(unsigned short startIndex,
	unsigned short polygonCount, unsigned short minVertexIndex,
	unsigned short vertexCount)
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_DRAW_SORTED_TRIANGLES);
	command.value0 = startIndex;
	command.value1 = polygonCount;
	command.value2 = minVertexIndex;
	command.value3 = vertexCount;
	(void)DispatchCommand(command);
}

void DrawGameSortedTriangles(const GameBoundingSphere &boundingSphere,
	unsigned short startIndex, unsigned short polygonCount,
	unsigned short minVertexIndex, unsigned short vertexCount)
{
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_DRAW_SORTED_TRIANGLES);
	command.value0 = startIndex;
	command.value1 = polygonCount;
	command.value2 = minVertexIndex;
	command.value3 = vertexCount;
	command.boundingSphere = &boundingSphere;
	(void)DispatchCommand(command);
}

RenderResult FlushGameSortedTriangles()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult result = owner->FlushGameSortedTriangles();
	if (result != RENDER_RESULT_OK)
		owner->RecordGameFailure(result);
	return result;
}

RenderResult DrawGamePrimitiveUP(GamePrimitiveTopology topology,
	unsigned int primitiveCount, const void *vertices, unsigned int stride,
	unsigned int vertexFormat)
{
	if (vertices == 0 || primitiveCount == 0U || stride == 0U ||
		vertexFormat == 0U)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	unsigned int vertexCount = 0;
	switch (topology)
	{
	case GAME_PRIMITIVE_TRIANGLE_LIST:
		if (primitiveCount > UINT_MAX / 3U) return RENDER_RESULT_INVALID_ARGUMENT;
		vertexCount = primitiveCount * 3U;
		break;
	case GAME_PRIMITIVE_TRIANGLE_STRIP:
		if (primitiveCount > UINT_MAX - 2U) return RENDER_RESULT_INVALID_ARGUMENT;
		vertexCount = primitiveCount + 2U;
		break;
	case GAME_PRIMITIVE_LINE_LIST:
		if (primitiveCount > UINT_MAX / 2U) return RENDER_RESULT_INVALID_ARGUMENT;
		vertexCount = primitiveCount * 2U;
		break;
	case GAME_PRIMITIVE_LINE_STRIP:
		if (primitiveCount > UINT_MAX - 1U) return RENDER_RESULT_INVALID_ARGUMENT;
		vertexCount = primitiveCount + 1U;
		break;
	case GAME_PRIMITIVE_POINT_LIST:
	default:
		{
			NativeGameRenderOwnerScope scope;
			if (scope.Get() != 0)
				scope.Get()->RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
		}
		return RENDER_RESULT_UNSUPPORTED;
	}
	if (vertexCount == 0U || static_cast<size_t>(vertexCount) >
		static_cast<size_t>(-1) / stride)
		return RENDER_RESULT_INVALID_ARGUMENT;
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_DRAW_PRIMITIVE_UP);
	command.value0 = static_cast<unsigned int>(topology);
	command.value1 = primitiveCount;
	command.value2 = stride;
	command.value3 = vertexFormat;
	command.input = vertices;
	command.inputBytes = static_cast<size_t>(vertexCount) * stride;
	return DispatchCommand(command);
}

void SetGameRenderTarget(TextureClass *colorTexture,
	ZTextureClass *depthTexture, bool useDefaultDepth)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_RENDER_TARGET);
	RenderTargetBinding binding;
	if (colorTexture == 0)
	{
		// A null color target is the historical restore-default operation.  The
		// neutral binding leaves both default attachments selected.
		if (depthTexture != 0)
		{
			NativeGameRenderOwnerScope scope;
			if (scope.Get() != 0)
				scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return;
		}
	}
	else
	{
		if (!AcquireTextureResource(colorTexture, &binding.color.resource))
		{
			NativeGameRenderOwnerScope scope;
			if (scope.Get() != 0)
				scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return;
		}
		binding.useBackBufferColor = false;
		binding.hasColor = true;
		if (depthTexture != 0)
		{
			if (!AcquireTextureResource(depthTexture, &binding.depth.resource))
			{
				NativeGameRenderOwnerScope scope;
				if (scope.Get() != 0)
					scope.Get()->RecordGameFailure(
						RENDER_RESULT_INVALID_ARGUMENT);
				return;
			}
			binding.useBackBufferDepth = false;
			binding.hasDepth = true;
		}
		else if (!useDefaultDepth)
		{
			binding.useBackBufferDepth = false;
			binding.hasDepth = false;
		}
	}
	command.input = &binding;
	command.inputBytes = sizeof(binding);
	(void)DispatchCommand(command);
}

RenderResult CopyGameActiveTargetToTexture(TextureClass *destination)
{
	GameRenderHandle handle;
	if (destination == 0 || !AcquireTextureHandle(destination, &handle) ||
		(handle.index == 0U && handle.generation == 0U))
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE);
	command.resource0 = handle;
	return DispatchCommand(command);
}

bool AcquireGameCopiedTextureContent(TextureClass *destination)
{
	if (destination == 0)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return false;
	}
	GameRenderHandle handle;
	if (!AcquireTextureHandle(destination, &handle) ||
		(handle.index == 0U && handle.generation == 0U))
		return false;
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_ACQUIRE_COPIED_TEXTURE_CONTENT);
	command.resource0 = handle;
	return DispatchCommand(command) == RENDER_RESULT_OK;
}

TextureClass *CreateGameRenderTarget(int width, int height, WW3DFormat format)
{
	if (width <= 0 || height <= 0)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return 0;
	}
	if (format == WW3D_FORMAT_UNKNOWN)
		format = WW3D_FORMAT_X8R8G8B8;
	NativeGameRenderOwnerScope scope;
	if (!IsOperationalOwner(scope.Get()))
		return 0;
	TextureClass *texture = NEW_REF(TextureClass, (width, height, format,
		MIP_LEVELS_1, TextureBaseClass::POOL_DEFAULT, true, false));
	if (texture == 0 || !texture->Is_Initialized())
	{
		REF_PTR_RELEASE(texture);
		scope.Get()->RecordGameFailure(RENDER_RESULT_FAILED);
		return 0;
	}
	NativeW3DSurfaceHandle outputSurface;
	if (!texture->Acquire_Native_Surface(0, 0, true, &outputSurface) ||
		!outputSurface.isValid())
	{
		REF_PTR_RELEASE(texture);
		scope.Get()->RecordGameFailure(RENDER_RESULT_FAILED);
		return 0;
	}
	return texture;
}

RenderResult CreateGameRenderTargetPair(int width, int height,
	WW3DFormat colorFormat, WW3DZFormat depthFormat,
	TextureClass **colorTarget, ZTextureClass **depthTarget)
{
	if (colorTarget != 0)
		*colorTarget = 0;
	if (depthTarget != 0)
		*depthTarget = 0;
	if (colorTarget == 0 || depthTarget == 0 || width <= 0 || height <= 0)
	{
		NativeGameRenderOwnerScope scope;
		if (scope.Get() != 0)
			scope.Get()->RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (colorFormat == WW3D_FORMAT_UNKNOWN)
		colorFormat = WW3D_FORMAT_A8R8G8B8;
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsOperationalOwner(owner))
		return RENDER_RESULT_INVALID_ARGUMENT;
	TextureClass *color = NEW_REF(TextureClass, (width, height, colorFormat,
		MIP_LEVELS_1, TextureBaseClass::POOL_DEFAULT, true, false));
	ZTextureClass *depth = NEW_REF(ZTextureClass, (width, height, depthFormat,
		MIP_LEVELS_1, TextureBaseClass::POOL_DEFAULT));
	NativeW3DSurfaceHandle colorSurface;
	NativeW3DSurfaceHandle depthSurface;
	if (color == 0 || depth == 0 || !color->Is_Initialized() ||
		!depth->Is_Initialized() ||
		!color->Acquire_Native_Surface(0, 0, true, &colorSurface) ||
		!depth->Acquire_Native_Surface(0, 0, true, &depthSurface) ||
		!colorSurface.isValid() || !depthSurface.isValid())
	{
		REF_PTR_RELEASE(color);
		REF_PTR_RELEASE(depth);
		owner->RecordGameFailure(RENDER_RESULT_FAILED);
		return RENDER_RESULT_FAILED;
	}
	*colorTarget = color;
	*depthTarget = depth;
	return RENDER_RESULT_OK;
}

RenderResult BeginGameRender(bool clear, bool clearz,
	const GameRenderColor &color, float destinationAlpha)
{
	if (!IsValidColor(color) || !IsFiniteFloat(destinationAlpha))
		return RENDER_RESULT_INVALID_ARGUMENT;
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_BEGIN_RENDER);
	SetClearCommandFields(&command, clear, clearz, color, destinationAlpha);
	return DispatchCommand(command);
}

RenderResult ClearGameRenderTargets(bool clear, bool clearz,
	const GameRenderColor &color, float destinationAlpha)
{
	if (!IsValidColor(color) || !IsFiniteFloat(destinationAlpha))
		return RENDER_RESULT_INVALID_ARGUMENT;
	GameRenderCommand command;
	InitializeCommand(&command,
		GAME_RENDER_COMMAND_CLEAR_RENDER_TARGETS);
	SetClearCommandFields(&command, clear, clearz, color, destinationAlpha);
	return DispatchCommand(command);
}

RenderResult EndGameRender(bool present)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_END_RENDER);
	command.value0 = present ? 1U : 0U;
	return DispatchCommand(command);
}

RenderResult FlipGameRenderer()
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_FLIP_RENDERER);
	return DispatchCommand(command);
}

RenderResult SyncGameRenderer(bool step)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SYNC_RENDERER);
	command.value0 = step ? 1U : 0U;
	return DispatchCommand(command);
}

RenderResult SetGameRendererSwapInterval(long interval)
{
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_SWAP_INTERVAL);
	command.signedLongValue = interval;
	return DispatchCommand(command);
}

long GetGameRendererSwapInterval()
{
	long interval = 0;
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_GET_SWAP_INTERVAL);
	command.output = &interval;
	command.outputBytes = sizeof(interval);
	if (DispatchCommand(command) != RENDER_RESULT_OK)
		return 0;
	return interval;
}

RenderResult SetGameTextureBitdepth(int bitDepth)
{
	if (bitDepth != 16 && bitDepth != 32)
		return RENDER_RESULT_INVALID_ARGUMENT;
	g_native_texture_bit_depth.store(bitDepth, std::memory_order_release);
	return RENDER_RESULT_OK;
}

int GetGameTextureBitdepth()
{
	return g_native_texture_bit_depth.load(std::memory_order_acquire);
}

RenderResult SetGameMSAAMode(unsigned int mode)
{
	if (mode != GAME_RENDER_MULTISAMPLE_NONE &&
		mode != GAME_RENDER_MULTISAMPLE_2X &&
		mode != GAME_RENDER_MULTISAMPLE_4X &&
		mode != GAME_RENDER_MULTISAMPLE_8X)
		return RENDER_RESULT_INVALID_ARGUMENT;
	// The current native D3D11 swap chain is single-sampled.  Match the
	// accepted Stage4 native behavior by accepting the request but exposing the
	// effective mode as NONE until a renderer-owned resolve path exists.
	return RENDER_RESULT_OK;
}

unsigned int GetGameMSAAMode()
{
	return GAME_RENDER_MULTISAMPLE_NONE;
}

RenderResult SetGameGamma(float gamma, float brightness, float contrast,
	bool calibrate)
{
	if (!IsFiniteFloat(gamma) || !IsFiniteFloat(brightness) ||
		!IsFiniteFloat(contrast))
		return RENDER_RESULT_INVALID_ARGUMENT;
	GameRenderCommand command;
	InitializeCommand(&command, GAME_RENDER_COMMAND_SET_GAMMA);
	command.float0 = gamma;
	command.float1 = brightness;
	command.float2 = contrast;
	command.value0 = calibrate ? 1U : 0U;
	return DispatchCommand(command);
}

}
}

#endif // defined(_WIN64)
