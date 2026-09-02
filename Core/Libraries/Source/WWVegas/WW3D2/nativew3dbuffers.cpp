#include "Utility/CppMacros.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"

#include "nativew3dbuffercompat.h"
#include "nativew3dbufferowner.h"
#include "Renderer/LegacyColorPacking.h"
#include "WWDebug/wwmemlog.h"
#include "WWLib/thread.h"
#include "WWMath/vector2.h"
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"

#include <new>

namespace
{
static const unsigned DEFAULT_VB_SIZE = 5000;
static const unsigned DEFAULT_IB_SIZE = 5000;

static bool g_dynamicSortingVertexArrayInUse = false;
static SortingVertexBufferClass *g_dynamicSortingVertexArray = nullptr;
static unsigned short g_dynamicSortingVertexArraySize = 0;
static unsigned short g_dynamicSortingVertexArrayOffset = 0;

static bool g_dynamicVertexBufferInUse = false;
static DX8VertexBufferClass *g_dynamicVertexBuffer = nullptr;
static unsigned short g_dynamicVertexBufferSize = DEFAULT_VB_SIZE;
static unsigned short g_dynamicVertexBufferOffset = 0;

static bool g_dynamicSortingIndexArrayInUse = false;
static SortingIndexBufferClass *g_dynamicSortingIndexArray = nullptr;
static unsigned short g_dynamicSortingIndexArraySize = 0;
static unsigned short g_dynamicSortingIndexArrayOffset = 0;

static bool g_dynamicIndexBufferInUse = false;
static DX8IndexBufferClass *g_dynamicIndexBuffer = nullptr;
static unsigned short g_dynamicIndexBufferSize = DEFAULT_IB_SIZE;
static unsigned short g_dynamicIndexBufferOffset = 0;

static const FVFInfoClass g_dynamicFvfInfo(dynamic_fvf_type);
static int g_vertexBufferCount = 0;
static int g_vertexBufferTotalVertices = 0;
static int g_vertexBufferTotalBytes = 0;
static int g_indexBufferCount = 0;
static int g_indexBufferTotalIndices = 0;
static int g_indexBufferTotalBytes = 0;

bool Use_Vertex_Range_Lock(unsigned int firstVertex, unsigned int count,
	unsigned int vertexCount)
{
	return firstVertex != 0 || count != vertexCount;
}

bool Use_Index_Range_Lock(unsigned int firstIndex, unsigned int count,
	unsigned int indexCount)
{
	return firstIndex != 0 || count != indexCount;
}

bool Get_Native_Buffer_Update_Mode(int flags,
	rts::render::RenderBufferUpdateMode *mode)
{
	return Decode_Native_Buffer_Update_Mode(static_cast<unsigned int>(flags),
		mode);
}

unsigned Pack_Vertex_Color(const Vector4 &color)
{
	return rts::render::PackLegacyARGB(color[0], color[1], color[2], color[3]);
}
}

VertexBufferClass::VertexBufferClass(unsigned type_, unsigned fvf,
	unsigned short vertexCount_)
	:
	VertexCount(vertexCount_), type(type_), engine_refs(0), generation(1),
	change_base_generation(0), change_offset(0), change_count(vertexCount_),
	change_flags(0)
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(VertexCount != 0);
	WWASSERT(type == BUFFER_TYPE_DX8 || type == BUFFER_TYPE_SORTING);
	WWASSERT(fvf != 0);
	fvf_info = W3DNEW FVFInfoClass(fvf);
	g_vertexBufferCount++;
	g_vertexBufferTotalVertices += VertexCount;
	g_vertexBufferTotalBytes += VertexCount * fvf_info->Get_FVF_Size();
}

VertexBufferClass::~VertexBufferClass()
{
	g_vertexBufferCount--;
	g_vertexBufferTotalVertices -= VertexCount;
	g_vertexBufferTotalBytes -= VertexCount * fvf_info->Get_FVF_Size();
	delete fvf_info;
}

unsigned VertexBufferClass::Get_Total_Buffer_Count()
{
	return g_vertexBufferCount;
}

unsigned VertexBufferClass::Get_Total_Allocated_Vertices()
{
	return g_vertexBufferTotalVertices;
}

unsigned VertexBufferClass::Get_Total_Allocated_Memory()
{
	return g_vertexBufferTotalBytes;
}

void VertexBufferClass::Add_Engine_Ref() const
{
	engine_refs++;
}

void VertexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs >= 0);
}

void VertexBufferClass::Mark_Changed()
{
	Mark_Changed_Range(0, VertexCount, 0);
}

void VertexBufferClass::Mark_Changed_Range(unsigned int offset,
	unsigned int count, unsigned int flags)
{
	change_base_generation = generation;
	++generation;
	if (generation == 0)
		++generation;
	change_offset = offset;
	change_count = count;
	change_flags = flags;
}

bool VertexBufferClass::Get_Change_Since(unsigned int uploadedGeneration,
	unsigned int *offset, unsigned int *count, unsigned int *flags) const
{
	if (offset == nullptr || count == nullptr || flags == nullptr ||
		change_base_generation != uploadedGeneration || change_count == 0)
		return false;
	*offset = change_offset;
	*count = change_count;
	*flags = change_flags;
	return true;
}

VertexBufferClass::WriteLockClass::WriteLockClass(
	VertexBufferClass *vertexBuffer, int flags)
	: VertexBufferLockClass(vertexBuffer)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	WWASSERT(vertexBuffer != nullptr);
	WWASSERT(!vertexBuffer->Engine_Refs());
	vertexBuffer->Add_Ref();
	switch (vertexBuffer->Type())
	{
	case BUFFER_TYPE_DX8:
		Locked = static_cast<DX8VertexBufferClass *>(vertexBuffer)->
			Lock_Native_Buffer(0, 0, flags, &Vertices);
		break;
	case BUFFER_TYPE_SORTING:
		Vertices = static_cast<SortingVertexBufferClass *>(vertexBuffer)->
			VertexBuffer;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

VertexBufferClass::WriteLockClass::~WriteLockClass()
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	Commit();
	VertexBuffer->Release_Ref();
}

bool VertexBufferClass::WriteLockClass::Commit()
{
	if (!Locked)
		return false;
	bool changed = Locked;
	switch (VertexBuffer->Type())
	{
	case BUFFER_TYPE_DX8:
		changed = Locked && static_cast<DX8VertexBufferClass *>(VertexBuffer)->
			Unlock_Native_Buffer();
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
		VertexBuffer->Mark_Changed();
	Locked = false;
	Vertices = nullptr;
	return changed;
}

VertexBufferClass::AppendLockClass::AppendLockClass(
	VertexBufferClass *vertexBuffer, unsigned startIndex, unsigned indexRange)
	: VertexBufferLockClass(vertexBuffer)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	WWASSERT(vertexBuffer != nullptr);
	WWASSERT(!vertexBuffer->Engine_Refs());
	WWASSERT(startIndex + indexRange <= vertexBuffer->Get_Vertex_Count());
	vertexBuffer->Add_Ref();
	switch (vertexBuffer->Type())
	{
	case BUFFER_TYPE_DX8:
		Locked = static_cast<DX8VertexBufferClass *>(vertexBuffer)->
			Lock_Native_Buffer(
				static_cast<size_t>(startIndex) * vertexBuffer->FVF_Info().Get_FVF_Size(),
				static_cast<size_t>(indexRange) * vertexBuffer->FVF_Info().Get_FVF_Size(),
				0, &Vertices);
		break;
	case BUFFER_TYPE_SORTING:
		Vertices = static_cast<SortingVertexBufferClass *>(vertexBuffer)->
			VertexBuffer + startIndex;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

VertexBufferClass::AppendLockClass::~AppendLockClass()
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	Commit();
	VertexBuffer->Release_Ref();
}

bool VertexBufferClass::AppendLockClass::Commit()
{
	if (!Locked)
		return false;
	bool changed = Locked;
	switch (VertexBuffer->Type())
	{
	case BUFFER_TYPE_DX8:
		changed = Locked && static_cast<DX8VertexBufferClass *>(VertexBuffer)->
			Unlock_Native_Buffer();
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
		VertexBuffer->Mark_Changed();
	Locked = false;
	Vertices = nullptr;
	return changed;
}

SortingVertexBufferClass::SortingVertexBufferClass(unsigned short vertexCount)
	: VertexBufferClass(BUFFER_TYPE_SORTING, dynamic_fvf_type, vertexCount),
	VertexBuffer(W3DNEWARRAY VertexFormatXYZNDUV2[vertexCount])
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(vertexCount != 0);
}

SortingVertexBufferClass::~SortingVertexBufferClass()
{
	delete[] VertexBuffer;
}

DX8VertexBufferClass::DX8VertexBufferClass(unsigned fvf,
	unsigned short vertexCount, UsageType usage)
	: VertexBufferClass(BUFFER_TYPE_DX8, fvf, vertexCount), NativeBuffer(nullptr)
{
	Create_Vertex_Buffer(usage);
}

DX8VertexBufferClass::DX8VertexBufferClass(const Vector3 *vertices,
	const Vector3 *normals, const Vector2 *texCoords, unsigned short vertexCount,
	UsageType usage)
	: VertexBufferClass(BUFFER_TYPE_DX8, DX8_FVF_XYZNUV1, vertexCount),
	NativeBuffer(nullptr)
{
	WWASSERT(vertices != nullptr);
	WWASSERT(normals != nullptr);
	WWASSERT(texCoords != nullptr);
	Create_Vertex_Buffer(usage);
	if (!Copy(vertices, normals, texCoords, 0, vertexCount))
	{
		delete NativeBuffer;
		NativeBuffer = nullptr;
	}
}

DX8VertexBufferClass::DX8VertexBufferClass(const Vector3 *vertices,
	const Vector3 *normals, const Vector4 *diffuse, const Vector2 *texCoords,
	unsigned short vertexCount, UsageType usage)
	: VertexBufferClass(BUFFER_TYPE_DX8, DX8_FVF_XYZNDUV1, vertexCount),
	NativeBuffer(nullptr)
{
	WWASSERT(vertices != nullptr);
	WWASSERT(normals != nullptr);
	WWASSERT(diffuse != nullptr);
	WWASSERT(texCoords != nullptr);
	Create_Vertex_Buffer(usage);
	if (!Copy(vertices, normals, texCoords, diffuse, 0, vertexCount))
	{
		delete NativeBuffer;
		NativeBuffer = nullptr;
	}
}

DX8VertexBufferClass::DX8VertexBufferClass(const Vector3 *vertices,
	const Vector4 *diffuse, const Vector2 *texCoords, unsigned short vertexCount,
	UsageType usage)
	: VertexBufferClass(BUFFER_TYPE_DX8, DX8_FVF_XYZDUV1, vertexCount),
	NativeBuffer(nullptr)
{
	WWASSERT(vertices != nullptr);
	WWASSERT(diffuse != nullptr);
	WWASSERT(texCoords != nullptr);
	Create_Vertex_Buffer(usage);
	if (!Copy(vertices, texCoords, diffuse, 0, vertexCount))
	{
		delete NativeBuffer;
		NativeBuffer = nullptr;
	}
}

DX8VertexBufferClass::DX8VertexBufferClass(const Vector3 *vertices,
	const Vector2 *texCoords, unsigned short vertexCount, UsageType usage)
	: VertexBufferClass(BUFFER_TYPE_DX8, DX8_FVF_XYZUV1, vertexCount),
	NativeBuffer(nullptr)
{
	WWASSERT(vertices != nullptr);
	WWASSERT(texCoords != nullptr);
	Create_Vertex_Buffer(usage);
	if (!Copy(vertices, texCoords, 0, vertexCount))
	{
		delete NativeBuffer;
		NativeBuffer = nullptr;
	}
}

DX8VertexBufferClass::~DX8VertexBufferClass()
{
	delete NativeBuffer;
	NativeBuffer = nullptr;
}

bool DX8VertexBufferClass::Is_Valid() const
{
	return NativeBuffer != nullptr && !NativeBuffer->HasFailedMutation();
}

bool DX8VertexBufferClass::Lock_Buffer(size_t byteOffset, size_t byteCount,
	int flags, void **data)
{
	if (data == nullptr)
		return false;
	*data = nullptr;
	return Lock_Native_Buffer(byteOffset, byteCount, flags, data);
}

bool DX8VertexBufferClass::Unlock_Buffer()
{
	const bool changed = Unlock_Native_Buffer();
	if (changed)
		Mark_Changed();
	return changed;
}

long DX8VertexBufferClass::Lock(unsigned int byteOffset,
	unsigned int byteCount, unsigned char **data, unsigned long flags)
{
	return Lock_Buffer(byteOffset, byteCount, static_cast<int>(flags),
		reinterpret_cast<void **>(data)) ? 0L : -1L;
}

long DX8VertexBufferClass::Unlock()
{
	return Unlock_Buffer() ? 0L : -1L;
}

void DX8VertexBufferClass::Create_Vertex_Buffer(UsageType usage)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	WWASSERT(NativeBuffer == nullptr);
	NativeBuffer = W3DNEW rts::render::NativeW3DBufferOwner;
	if (NativeBuffer == nullptr)
		return;
	rts::render::BufferDescriptor descriptor;
	descriptor.byteCount = static_cast<size_t>(FVF_Info().Get_FVF_Size()) *
		VertexCount;
	descriptor.stride = FVF_Info().Get_FVF_Size();
	descriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
	descriptor.usage = (usage & USAGE_DYNAMIC) != 0 ?
		rts::render::RENDER_USAGE_DYNAMIC : rts::render::RENDER_USAGE_DEFAULT;
	if (NativeBuffer->Create(descriptor) != rts::render::RENDER_RESULT_OK)
	{
		delete NativeBuffer;
		NativeBuffer = nullptr;
	}
}

bool DX8VertexBufferClass::Acquire_Native_Vertex_Buffer(unsigned int stride,
	unsigned int offset, unsigned int startVertex, unsigned int vertexCount,
	rts::render::GpuHandle *validated) const
{
	if (validated == nullptr)
		return false;
	*validated = rts::render::GpuHandle();
	return NativeBuffer != nullptr &&
		NativeBuffer->AcquireVertexRange(stride, offset, startVertex, vertexCount,
			validated) == rts::render::RENDER_RESULT_OK;
}

bool DX8VertexBufferClass::Lock_Native_Buffer(size_t offset, size_t byteCount,
	int flags, void **data)
{
	if (data == nullptr)
		return false;
	*data = nullptr;
	rts::render::RenderBufferUpdateMode mode;
	return NativeBuffer != nullptr && Get_Native_Buffer_Update_Mode(flags, &mode) &&
		NativeBuffer->Lock(offset, byteCount, mode, data) ==
			rts::render::RENDER_RESULT_OK;
}

bool DX8VertexBufferClass::Unlock_Native_Buffer()
{
	return NativeBuffer != nullptr &&
		NativeBuffer->Unlock() == rts::render::RENDER_RESULT_OK;
}

bool DX8VertexBufferClass::Copy(const Vector3 *loc, const Vector3 *norm,
	const Vector2 *uv, unsigned firstVertex, unsigned count)
{
	WWASSERT(loc != nullptr);
	WWASSERT(norm != nullptr);
	WWASSERT(uv != nullptr);
	WWASSERT(count <= VertexCount);
	WWASSERT(FVF_Info().Get_FVF() == DX8_FVF_XYZNUV1);
	if (loc == nullptr || norm == nullptr || uv == nullptr ||
		firstVertex > VertexCount || count > VertexCount - firstVertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZNUV1)
		return false;
	if (count == 0)
		return true;
	if (Use_Vertex_Range_Lock(firstVertex, count, Get_Vertex_Count()))
	{
		AppendLockClass lock(this, firstVertex, count);
		VertexFormatXYZNUV1 *vertices =
			static_cast<VertexFormatXYZNUV1 *>(lock.Get_Vertex_Array());
		if (!lock.Is_Locked() || vertices == nullptr)
			return false;
		for (unsigned v = 0; v < count; ++v)
		{
			vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
			vertices[v].z = (*loc++)[2];
			vertices[v].nx = (*norm)[0]; vertices[v].ny = (*norm)[1];
			vertices[v].nz = (*norm++)[2];
			vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
		}
		return lock.Commit();
	}
	WriteLockClass lock(this);
	VertexFormatXYZNUV1 *vertices =
		static_cast<VertexFormatXYZNUV1 *>(lock.Get_Vertex_Array());
	if (!lock.Is_Locked() || vertices == nullptr)
		return false;
	for (unsigned v = 0; v < count; ++v)
	{
		vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
		vertices[v].z = (*loc++)[2];
		vertices[v].nx = (*norm)[0]; vertices[v].ny = (*norm)[1];
		vertices[v].nz = (*norm++)[2];
		vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
	}
	return lock.Commit();
}

bool DX8VertexBufferClass::Copy(const Vector3 *loc, unsigned firstVertex,
	unsigned count)
{
	WWASSERT(loc != nullptr);
	WWASSERT(count <= VertexCount);
	WWASSERT(FVF_Info().Get_FVF() == DX8_FVF_XYZ);
	if (loc == nullptr || firstVertex > VertexCount ||
		count > VertexCount - firstVertex || FVF_Info().Get_FVF() != DX8_FVF_XYZ)
		return false;
	if (count == 0)
		return true;
	if (Use_Vertex_Range_Lock(firstVertex, count, Get_Vertex_Count()))
	{
		AppendLockClass lock(this, firstVertex, count);
		VertexFormatXYZ *vertices =
			static_cast<VertexFormatXYZ *>(lock.Get_Vertex_Array());
		if (!lock.Is_Locked() || vertices == nullptr)
			return false;
		for (unsigned v = 0; v < count; ++v)
		{
			vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
			vertices[v].z = (*loc++)[2];
		}
		return lock.Commit();
	}
	WriteLockClass lock(this);
	VertexFormatXYZ *vertices =
		static_cast<VertexFormatXYZ *>(lock.Get_Vertex_Array());
	if (!lock.Is_Locked() || vertices == nullptr)
		return false;
	for (unsigned v = 0; v < count; ++v)
	{
		vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
		vertices[v].z = (*loc++)[2];
	}
	return lock.Commit();
}

bool DX8VertexBufferClass::Copy(const Vector3 *loc, const Vector2 *uv,
	unsigned firstVertex, unsigned count)
{
	WWASSERT(loc != nullptr);
	WWASSERT(uv != nullptr);
	WWASSERT(count <= VertexCount);
	WWASSERT(FVF_Info().Get_FVF() == DX8_FVF_XYZUV1);
	if (loc == nullptr || uv == nullptr || firstVertex > VertexCount ||
		count > VertexCount - firstVertex || FVF_Info().Get_FVF() != DX8_FVF_XYZUV1)
		return false;
	if (count == 0)
		return true;
	if (Use_Vertex_Range_Lock(firstVertex, count, Get_Vertex_Count()))
	{
		AppendLockClass lock(this, firstVertex, count);
		VertexFormatXYZUV1 *vertices =
			static_cast<VertexFormatXYZUV1 *>(lock.Get_Vertex_Array());
		if (!lock.Is_Locked() || vertices == nullptr)
			return false;
		for (unsigned v = 0; v < count; ++v)
		{
			vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
			vertices[v].z = (*loc++)[2];
			vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
		}
		return lock.Commit();
	}
	WriteLockClass lock(this);
	VertexFormatXYZUV1 *vertices =
		static_cast<VertexFormatXYZUV1 *>(lock.Get_Vertex_Array());
	if (!lock.Is_Locked() || vertices == nullptr)
		return false;
	for (unsigned v = 0; v < count; ++v)
	{
		vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
		vertices[v].z = (*loc++)[2];
		vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
	}
	return lock.Commit();
}

bool DX8VertexBufferClass::Copy(const Vector3 *loc, const Vector3 *norm,
	unsigned firstVertex, unsigned count)
{
	WWASSERT(loc != nullptr);
	WWASSERT(norm != nullptr);
	WWASSERT(count <= VertexCount);
	WWASSERT(FVF_Info().Get_FVF() == DX8_FVF_XYZN);
	if (loc == nullptr || norm == nullptr || firstVertex > VertexCount ||
		count > VertexCount - firstVertex || FVF_Info().Get_FVF() != DX8_FVF_XYZN)
		return false;
	if (count == 0)
		return true;
	if (Use_Vertex_Range_Lock(firstVertex, count, Get_Vertex_Count()))
	{
		AppendLockClass lock(this, firstVertex, count);
		VertexFormatXYZN *vertices =
			static_cast<VertexFormatXYZN *>(lock.Get_Vertex_Array());
		if (!lock.Is_Locked() || vertices == nullptr)
			return false;
		for (unsigned v = 0; v < count; ++v)
		{
			vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
			vertices[v].z = (*loc++)[2];
			vertices[v].nx = (*norm)[0]; vertices[v].ny = (*norm)[1];
			vertices[v].nz = (*norm++)[2];
		}
		return lock.Commit();
	}
	WriteLockClass lock(this);
	VertexFormatXYZN *vertices =
		static_cast<VertexFormatXYZN *>(lock.Get_Vertex_Array());
	if (!lock.Is_Locked() || vertices == nullptr)
		return false;
	for (unsigned v = 0; v < count; ++v)
	{
		vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
		vertices[v].z = (*loc++)[2];
		vertices[v].nx = (*norm)[0]; vertices[v].ny = (*norm)[1];
		vertices[v].nz = (*norm++)[2];
	}
	return lock.Commit();
}

bool DX8VertexBufferClass::Copy(const Vector3 *loc, const Vector3 *norm,
	const Vector2 *uv, const Vector4 *diffuse, unsigned firstVertex,
	unsigned count)
{
	WWASSERT(loc != nullptr); WWASSERT(norm != nullptr);
	WWASSERT(uv != nullptr); WWASSERT(diffuse != nullptr);
	WWASSERT(count <= VertexCount);
	WWASSERT(FVF_Info().Get_FVF() == DX8_FVF_XYZNDUV1);
	if (loc == nullptr || norm == nullptr || uv == nullptr || diffuse == nullptr ||
		firstVertex > VertexCount || count > VertexCount - firstVertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZNDUV1)
		return false;
	if (count == 0)
		return true;
	if (Use_Vertex_Range_Lock(firstVertex, count, Get_Vertex_Count()))
	{
		AppendLockClass lock(this, firstVertex, count);
		VertexFormatXYZNDUV1 *vertices =
			static_cast<VertexFormatXYZNDUV1 *>(lock.Get_Vertex_Array());
		if (!lock.Is_Locked() || vertices == nullptr)
			return false;
		for (unsigned v = 0; v < count; ++v)
		{
			vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
			vertices[v].z = (*loc++)[2];
			vertices[v].nx = (*norm)[0]; vertices[v].ny = (*norm)[1];
			vertices[v].nz = (*norm++)[2];
			vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
			vertices[v].diffuse = Pack_Vertex_Color(diffuse[v]);
		}
		return lock.Commit();
	}
	WriteLockClass lock(this);
	VertexFormatXYZNDUV1 *vertices =
		static_cast<VertexFormatXYZNDUV1 *>(lock.Get_Vertex_Array());
	if (!lock.Is_Locked() || vertices == nullptr)
		return false;
	for (unsigned v = 0; v < count; ++v)
	{
		vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
		vertices[v].z = (*loc++)[2];
		vertices[v].nx = (*norm)[0]; vertices[v].ny = (*norm)[1];
		vertices[v].nz = (*norm++)[2];
		vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
		vertices[v].diffuse = Pack_Vertex_Color(diffuse[v]);
	}
	return lock.Commit();
}

bool DX8VertexBufferClass::Copy(const Vector3 *loc, const Vector2 *uv,
	const Vector4 *diffuse, unsigned firstVertex, unsigned count)
{
	WWASSERT(loc != nullptr); WWASSERT(uv != nullptr);
	WWASSERT(diffuse != nullptr); WWASSERT(count <= VertexCount);
	WWASSERT(FVF_Info().Get_FVF() == DX8_FVF_XYZDUV1);
	if (loc == nullptr || uv == nullptr || diffuse == nullptr ||
		firstVertex > VertexCount || count > VertexCount - firstVertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZDUV1)
		return false;
	if (count == 0)
		return true;
	if (Use_Vertex_Range_Lock(firstVertex, count, Get_Vertex_Count()))
	{
		AppendLockClass lock(this, firstVertex, count);
		VertexFormatXYZDUV1 *vertices =
			static_cast<VertexFormatXYZDUV1 *>(lock.Get_Vertex_Array());
		if (!lock.Is_Locked() || vertices == nullptr)
			return false;
		for (unsigned v = 0; v < count; ++v)
		{
			vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
			vertices[v].z = (*loc++)[2];
			vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
			vertices[v].diffuse = Pack_Vertex_Color(diffuse[v]);
		}
		return lock.Commit();
	}
	WriteLockClass lock(this);
	VertexFormatXYZDUV1 *vertices =
		static_cast<VertexFormatXYZDUV1 *>(lock.Get_Vertex_Array());
	if (!lock.Is_Locked() || vertices == nullptr)
		return false;
	for (unsigned v = 0; v < count; ++v)
	{
		vertices[v].x = (*loc)[0]; vertices[v].y = (*loc)[1];
		vertices[v].z = (*loc++)[2];
		vertices[v].u1 = (*uv)[0]; vertices[v].v1 = (*uv++)[1];
		vertices[v].diffuse = Pack_Vertex_Color(diffuse[v]);
	}
	return lock.Commit();
}

DynamicVBAccessClass::DynamicVBAccessClass(unsigned type_, unsigned fvf,
	unsigned short vertexCount)
	: Type(type_), FVFInfo(g_dynamicFvfInfo), VertexCount(vertexCount),
	VertexBufferOffset(0), VertexBuffer(nullptr)
{
	WWASSERT(fvf == dynamic_fvf_type);
	WWASSERT(Type == BUFFER_TYPE_DYNAMIC_DX8 ||
		Type == BUFFER_TYPE_DYNAMIC_SORTING);
	if (Type == BUFFER_TYPE_DYNAMIC_DX8)
		Allocate_DX8_Dynamic_Buffer();
	else
		Allocate_Sorting_Dynamic_Buffer();
}

DynamicVBAccessClass::~DynamicVBAccessClass()
{
	const bool valid = Is_Valid();
	if (Type == BUFFER_TYPE_DYNAMIC_DX8)
	{
		g_dynamicVertexBufferInUse = false;
		if (valid)
			g_dynamicVertexBufferOffset += VertexCount;
	}
	else
	{
		g_dynamicSortingVertexArrayInUse = false;
		if (valid)
			g_dynamicSortingVertexArrayOffset += VertexCount;
	}
	REF_PTR_RELEASE(VertexBuffer);
}

bool DynamicVBAccessClass::Is_Valid() const
{
	if (VertexBuffer == nullptr || VertexCount == 0 ||
		VertexBufferOffset > VertexBuffer->Get_Vertex_Count() ||
		static_cast<unsigned int>(VertexCount) >
			static_cast<unsigned int>(VertexBuffer->Get_Vertex_Count()) -
			VertexBufferOffset)
		return false;
	return Type != BUFFER_TYPE_DYNAMIC_DX8 ||
		static_cast<DX8VertexBufferClass *>(VertexBuffer)->Is_Valid();
}

void DynamicVBAccessClass::_Deinit()
{
	WWASSERT(g_dynamicVertexBuffer == nullptr ||
		g_dynamicVertexBuffer->Num_Refs() == 1);
	REF_PTR_RELEASE(g_dynamicVertexBuffer);
	g_dynamicVertexBufferInUse = false;
	g_dynamicVertexBufferSize = DEFAULT_VB_SIZE;
	g_dynamicVertexBufferOffset = 0;
	WWASSERT(g_dynamicSortingVertexArray == nullptr ||
		g_dynamicSortingVertexArray->Num_Refs() == 1);
	REF_PTR_RELEASE(g_dynamicSortingVertexArray);
	g_dynamicSortingVertexArrayInUse = false;
	g_dynamicSortingVertexArraySize = 0;
	g_dynamicSortingVertexArrayOffset = 0;
}

void DynamicVBAccessClass::Allocate_DX8_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	if (!rts::render::IsNativeW3DBufferOwnerThread() || VertexCount == 0 ||
		g_dynamicVertexBufferOffset > 65535U -
			static_cast<unsigned int>(VertexCount))
	{
		VertexBuffer = nullptr;
		VertexBufferOffset = 0;
		g_dynamicVertexBufferInUse = false;
		return;
	}
	WWASSERT(!g_dynamicVertexBufferInUse);
	g_dynamicVertexBufferInUse = true;
	if (VertexCount > g_dynamicVertexBufferSize)
	{
		REF_PTR_RELEASE(g_dynamicVertexBuffer);
		g_dynamicVertexBufferSize = VertexCount;
		if (g_dynamicVertexBufferSize < DEFAULT_VB_SIZE)
			g_dynamicVertexBufferSize = DEFAULT_VB_SIZE;
	}
	if (!g_dynamicVertexBuffer)
	{
		g_dynamicVertexBuffer = NEW_REF(DX8VertexBufferClass,
			(dynamic_fvf_type, g_dynamicVertexBufferSize,
			DX8VertexBufferClass::USAGE_DYNAMIC));
		g_dynamicVertexBufferOffset = 0;
	}
	if (g_dynamicVertexBuffer != nullptr &&
		!g_dynamicVertexBuffer->Is_Valid())
		REF_PTR_RELEASE(g_dynamicVertexBuffer);
	if (static_cast<unsigned>(VertexCount) + g_dynamicVertexBufferOffset >
		g_dynamicVertexBufferSize)
		g_dynamicVertexBufferOffset = 0;
	REF_PTR_SET(VertexBuffer, g_dynamicVertexBuffer);
	VertexBufferOffset = g_dynamicVertexBufferOffset;
}

void DynamicVBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	if (!rts::render::IsNativeW3DBufferOwnerThread() || VertexCount == 0 ||
		g_dynamicSortingVertexArrayOffset > 65535U -
			static_cast<unsigned int>(VertexCount))
	{
		VertexBuffer = nullptr;
		VertexBufferOffset = 0;
		g_dynamicSortingVertexArrayInUse = false;
		return;
	}
	WWASSERT(!g_dynamicSortingVertexArrayInUse);
	g_dynamicSortingVertexArrayInUse = true;
	const unsigned newVertexCount = g_dynamicSortingVertexArrayOffset + VertexCount;
	if (newVertexCount > 65535U)
	{
		VertexBuffer = nullptr;
		VertexBufferOffset = 0;
		g_dynamicSortingVertexArrayInUse = false;
		return;
	}
	if (newVertexCount > g_dynamicSortingVertexArraySize)
	{
		REF_PTR_RELEASE(g_dynamicSortingVertexArray);
		g_dynamicSortingVertexArraySize = static_cast<unsigned short>(newVertexCount);
		if (g_dynamicSortingVertexArraySize < DEFAULT_VB_SIZE)
			g_dynamicSortingVertexArraySize = DEFAULT_VB_SIZE;
	}
	if (!g_dynamicSortingVertexArray)
	{
		g_dynamicSortingVertexArray = NEW_REF(SortingVertexBufferClass,
			(g_dynamicSortingVertexArraySize));
		g_dynamicSortingVertexArrayOffset = 0;
	}
	REF_PTR_SET(VertexBuffer, g_dynamicSortingVertexArray);
	VertexBufferOffset = g_dynamicSortingVertexArrayOffset;
}

bool DynamicVBAccessClass::Acquire_Native_Vertex_Buffer(
	rts::render::GpuHandle *validated) const
{
	if (validated == nullptr)
		return false;
	*validated = rts::render::GpuHandle();
	if (!rts::render::IsNativeW3DBufferOwnerThread() ||
		Type != BUFFER_TYPE_DYNAMIC_DX8 || !Is_Valid() ||
		!static_cast<DX8VertexBufferClass *>(VertexBuffer)->Is_Valid())
		return false;
	const unsigned int stride = VertexBuffer->FVF_Info().Get_FVF_Size();
	return static_cast<DX8VertexBufferClass *>(VertexBuffer)->
		Acquire_Native_Vertex_Buffer(stride,
			static_cast<unsigned int>(VertexBufferOffset) * stride, 0,
			VertexCount, validated);
}

unsigned int DynamicVBAccessClass::Get_Vertex_Buffer_Offset() const
{
	return !rts::render::IsNativeW3DBufferOwnerThread() || !Is_Valid() ? 0U :
		static_cast<unsigned int>(VertexBufferOffset) *
		VertexBuffer->FVF_Info().Get_FVF_Size();
}

unsigned int DynamicVBAccessClass::Get_Vertex_Stride() const
{
	return !rts::render::IsNativeW3DBufferOwnerThread() || !Is_Valid() ? 0U :
		VertexBuffer->FVF_Info().Get_FVF_Size();
}

const void *DynamicVBAccessClass::Get_Sorted_Vertex_Data() const
{
	if (!rts::render::IsNativeW3DBufferOwnerThread() ||
		Type != BUFFER_TYPE_DYNAMIC_SORTING || !Is_Valid())
		return nullptr;
	return static_cast<const SortingVertexBufferClass *>(VertexBuffer)->
		Get_Vertex_Data() + VertexBufferOffset;
}

DynamicVBAccessClass::WriteLockClass::WriteLockClass(
	DynamicVBAccessClass *dynamicAccess)
	: DynamicVBAccess(dynamicAccess), Vertices(nullptr), Locked(false)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	if (DynamicVBAccess == nullptr ||
		(DynamicVBAccess->Get_Type() != BUFFER_TYPE_DYNAMIC_DX8 &&
			DynamicVBAccess->Get_Type() != BUFFER_TYPE_DYNAMIC_SORTING) ||
		DynamicVBAccess->VertexBuffer == nullptr ||
		DynamicVBAccess->VertexCount == 0 ||
		DynamicVBAccess->VertexBufferOffset >
			DynamicVBAccess->VertexBuffer->Get_Vertex_Count() ||
		static_cast<unsigned int>(DynamicVBAccess->VertexCount) >
			static_cast<unsigned int>(DynamicVBAccess->VertexBuffer->
				Get_Vertex_Count()) - DynamicVBAccess->VertexBufferOffset ||
		(DynamicVBAccess->Get_Type() != BUFFER_TYPE_DYNAMIC_DX8 &&
			!DynamicVBAccess->Is_Valid()))
		return;
	switch (DynamicVBAccess->Get_Type())
	{
	case BUFFER_TYPE_DYNAMIC_DX8:
		WWASSERT(g_dynamicVertexBuffer != nullptr);
		Locked = static_cast<DX8VertexBufferClass *>(
			DynamicVBAccess->VertexBuffer)->Lock_Native_Buffer(
				static_cast<size_t>(DynamicVBAccess->VertexBufferOffset) *
					DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
				static_cast<size_t>(DynamicVBAccess->Get_Vertex_Count()) *
					DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
				NATIVE_BUFFER_LOCK_NO_SYSTEM_LOCK |
					(!DynamicVBAccess->VertexBufferOffset ?
					NATIVE_BUFFER_LOCK_DISCARD : NATIVE_BUFFER_LOCK_NO_OVERWRITE),
				reinterpret_cast<void **>(&Vertices));
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Vertices = static_cast<SortingVertexBufferClass *>(
			DynamicVBAccess->VertexBuffer)->VertexBuffer +
			DynamicVBAccess->VertexBufferOffset;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

DynamicVBAccessClass::WriteLockClass::~WriteLockClass()
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	Commit();
}

bool DynamicVBAccessClass::WriteLockClass::Commit()
{
	if (!Locked || DynamicVBAccess == nullptr ||
		!DynamicVBAccess->Is_Valid())
		return false;
	const unsigned int changeFlags = !DynamicVBAccess->VertexBufferOffset ?
		NATIVE_BUFFER_LOCK_DISCARD : NATIVE_BUFFER_LOCK_NO_OVERWRITE;
	bool changed = Locked;
	switch (DynamicVBAccess->Get_Type())
	{
	case BUFFER_TYPE_DYNAMIC_DX8:
		changed = Locked && static_cast<DX8VertexBufferClass *>(
			DynamicVBAccess->VertexBuffer)->Unlock_Native_Buffer();
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
		DynamicVBAccess->VertexBuffer->Mark_Changed_Range(
			DynamicVBAccess->VertexBufferOffset,
			DynamicVBAccess->Get_Vertex_Count(), changeFlags);
	Locked = false;
	Vertices = nullptr;
	return changed;
}

void DynamicVBAccessClass::_Reset(bool frameChanged)
{
	g_dynamicSortingVertexArrayOffset = 0;
	if (frameChanged)
		g_dynamicVertexBufferOffset = 0;
}

unsigned short DynamicVBAccessClass::Get_Default_Vertex_Count()
{
	return g_dynamicVertexBufferSize;
}

IndexBufferClass::IndexBufferClass(unsigned type_, unsigned short indexCount_)
	: engine_refs(0), index_count(indexCount_), type(type_), generation(1),
	change_base_generation(0), change_offset(0), change_count(indexCount_),
	change_flags(0)
{
	WWASSERT(type == BUFFER_TYPE_DX8 || type == BUFFER_TYPE_SORTING);
	WWASSERT(index_count != 0);
	g_indexBufferCount++;
	g_indexBufferTotalIndices += index_count;
	g_indexBufferTotalBytes += index_count * sizeof(unsigned short);
}

IndexBufferClass::~IndexBufferClass()
{
	g_indexBufferCount--;
	g_indexBufferTotalIndices -= index_count;
	g_indexBufferTotalBytes -= index_count * sizeof(unsigned short);
}

unsigned IndexBufferClass::Get_Total_Buffer_Count() { return g_indexBufferCount; }
unsigned IndexBufferClass::Get_Total_Allocated_Indices()
{
	return g_indexBufferTotalIndices;
}
unsigned IndexBufferClass::Get_Total_Allocated_Memory()
{
	return g_indexBufferTotalBytes;
}
void IndexBufferClass::Add_Engine_Ref() const { engine_refs++; }
void IndexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs >= 0);
}
void IndexBufferClass::Mark_Changed() { Mark_Changed_Range(0, index_count, 0); }
void IndexBufferClass::Mark_Changed_Range(unsigned int offset,
	unsigned int count, unsigned int flags)
{
	change_base_generation = generation;
	++generation;
	if (generation == 0)
		++generation;
	change_offset = offset;
	change_count = count;
	change_flags = flags;
}
bool IndexBufferClass::Get_Change_Since(unsigned int uploadedGeneration,
	unsigned int *offset, unsigned int *count, unsigned int *flags) const
{
	if (offset == nullptr || count == nullptr || flags == nullptr ||
		change_base_generation != uploadedGeneration || change_count == 0)
		return false;
	*offset = change_offset;
	*count = change_count;
	*flags = change_flags;
	return true;
}

bool IndexBufferClass::Copy(unsigned int *indices, unsigned firstIndex,
	unsigned count)
{
	WWASSERT(indices != nullptr);
	if (indices == nullptr || firstIndex > Get_Index_Count() ||
		count > Get_Index_Count() - firstIndex)
		return false;
	if (count == 0)
		return true;
	if (Use_Index_Range_Lock(firstIndex, count, Get_Index_Count()))
	{
		DX8IndexBufferClass::AppendLockClass lock(
			static_cast<DX8IndexBufferClass *>(this), firstIndex, count);
		unsigned short *out = lock.Get_Index_Array();
		if (!lock.Is_Locked() || out == nullptr)
			return false;
		for (unsigned i = 0; i < count; ++i)
			*out++ = static_cast<unsigned short>(*indices++);
		return lock.Commit();
	}
	DX8IndexBufferClass::WriteLockClass lock(
		static_cast<DX8IndexBufferClass *>(this));
	unsigned short *out = lock.Get_Index_Array();
	if (!lock.Is_Locked() || out == nullptr)
		return false;
	for (unsigned i = 0; i < count; ++i)
		*out++ = static_cast<unsigned short>(*indices++);
	return lock.Commit();
}

bool IndexBufferClass::Copy(unsigned short *indices, unsigned firstIndex,
	unsigned count)
{
	WWASSERT(indices != nullptr);
	if (indices == nullptr || firstIndex > Get_Index_Count() ||
		count > Get_Index_Count() - firstIndex)
		return false;
	if (count == 0)
		return true;
	if (Use_Index_Range_Lock(firstIndex, count, Get_Index_Count()))
	{
		DX8IndexBufferClass::AppendLockClass lock(
			static_cast<DX8IndexBufferClass *>(this), firstIndex, count);
		unsigned short *out = lock.Get_Index_Array();
		if (!lock.Is_Locked() || out == nullptr)
			return false;
		for (unsigned i = 0; i < count; ++i)
			*out++ = *indices++;
		return lock.Commit();
	}
	DX8IndexBufferClass::WriteLockClass lock(
		static_cast<DX8IndexBufferClass *>(this));
	unsigned short *out = lock.Get_Index_Array();
	if (!lock.Is_Locked() || out == nullptr)
		return false;
	for (unsigned i = 0; i < count; ++i)
		*out++ = *indices++;
	return lock.Commit();
}

IndexBufferClass::WriteLockClass::WriteLockClass(IndexBufferClass *buffer,
	int flags)
	: index_buffer(buffer), indices(nullptr), locked(false)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	WWASSERT(buffer != nullptr);
	WWASSERT(!buffer->Engine_Refs());
	buffer->Add_Ref();
	switch (buffer->Type())
	{
	case BUFFER_TYPE_DX8:
		locked = static_cast<DX8IndexBufferClass *>(buffer)->Lock_Native_Buffer(
			0, static_cast<size_t>(buffer->Get_Index_Count()) * sizeof(unsigned short),
			flags, reinterpret_cast<void **>(&indices));
		break;
	case BUFFER_TYPE_SORTING:
		indices = static_cast<SortingIndexBufferClass *>(buffer)->index_buffer;
		locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

IndexBufferClass::WriteLockClass::~WriteLockClass()
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	Commit();
	index_buffer->Release_Ref();
}

bool IndexBufferClass::WriteLockClass::Commit()
{
	if (!locked)
		return false;
	bool changed = locked;
	switch (index_buffer->Type())
	{
	case BUFFER_TYPE_DX8:
		changed = locked && static_cast<DX8IndexBufferClass *>(index_buffer)->
			Unlock_Native_Buffer();
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
		index_buffer->Mark_Changed();
	locked = false;
	indices = nullptr;
	return changed;
}

IndexBufferClass::AppendLockClass::AppendLockClass(IndexBufferClass *buffer,
	unsigned startIndex, unsigned indexRange)
	: index_buffer(buffer), indices(nullptr), locked(false)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	WWASSERT(buffer != nullptr);
	WWASSERT(startIndex + indexRange <= buffer->Get_Index_Count());
	WWASSERT(!buffer->Engine_Refs());
	buffer->Add_Ref();
	switch (buffer->Type())
	{
	case BUFFER_TYPE_DX8:
		locked = static_cast<DX8IndexBufferClass *>(buffer)->Lock_Native_Buffer(
			static_cast<size_t>(startIndex) * sizeof(unsigned short),
			static_cast<size_t>(indexRange) * sizeof(unsigned short), 0,
			reinterpret_cast<void **>(&indices));
		break;
	case BUFFER_TYPE_SORTING:
		indices = static_cast<SortingIndexBufferClass *>(buffer)->index_buffer +
			startIndex;
		locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

IndexBufferClass::AppendLockClass::~AppendLockClass()
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	Commit();
	index_buffer->Release_Ref();
}

bool IndexBufferClass::AppendLockClass::Commit()
{
	if (!locked)
		return false;
	bool changed = locked;
	switch (index_buffer->Type())
	{
	case BUFFER_TYPE_DX8:
		changed = locked && static_cast<DX8IndexBufferClass *>(index_buffer)->
			Unlock_Native_Buffer();
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
		index_buffer->Mark_Changed();
	locked = false;
	indices = nullptr;
	return changed;
}

DX8IndexBufferClass::DX8IndexBufferClass(unsigned short indexCount,
	UsageType usage)
	: IndexBufferClass(BUFFER_TYPE_DX8, indexCount), native_buffer(nullptr)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	WWASSERT(index_count != 0);
	native_buffer = W3DNEW rts::render::NativeW3DBufferOwner;
	if (native_buffer == nullptr)
		return;
	rts::render::BufferDescriptor descriptor;
	descriptor.byteCount = static_cast<size_t>(index_count) * sizeof(unsigned short);
	descriptor.stride = sizeof(unsigned short);
	descriptor.binding = rts::render::RENDER_BUFFER_INDEX;
	descriptor.usage = (usage & USAGE_DYNAMIC) != 0 ?
		rts::render::RENDER_USAGE_DYNAMIC : rts::render::RENDER_USAGE_DEFAULT;
	if (native_buffer->Create(descriptor) != rts::render::RENDER_RESULT_OK)
	{
		delete native_buffer;
		native_buffer = nullptr;
	}
}

DX8IndexBufferClass::~DX8IndexBufferClass()
{
	delete native_buffer;
	native_buffer = nullptr;
}

bool DX8IndexBufferClass::Is_Valid() const
{
	return native_buffer != nullptr && !native_buffer->HasFailedMutation();
}

bool DX8IndexBufferClass::Lock_Buffer(size_t byteOffset, size_t byteCount,
	int flags, void **data)
{
	if (data == nullptr)
		return false;
	*data = nullptr;
	return Lock_Native_Buffer(byteOffset, byteCount, flags, data);
}

bool DX8IndexBufferClass::Unlock_Buffer()
{
	const bool changed = Unlock_Native_Buffer();
	if (changed)
		Mark_Changed();
	return changed;
}

long DX8IndexBufferClass::Lock(unsigned int byteOffset,
	unsigned int byteCount, unsigned char **data, unsigned long flags)
{
	return Lock_Buffer(byteOffset, byteCount, static_cast<int>(flags),
		reinterpret_cast<void **>(data)) ? 0L : -1L;
}

long DX8IndexBufferClass::Unlock()
{
	return Unlock_Buffer() ? 0L : -1L;
}

bool DX8IndexBufferClass::Acquire_Native_Index_Buffer(unsigned int offset,
	unsigned int startIndex, unsigned int indexCount,
	rts::render::GpuHandle *validated) const
{
	if (validated == nullptr)
		return false;
	*validated = rts::render::GpuHandle();
	return native_buffer != nullptr &&
		native_buffer->AcquireIndexRange(rts::render::RENDER_FORMAT_R16_UINT,
			offset, startIndex, indexCount, validated) ==
		rts::render::RENDER_RESULT_OK;
}

bool DX8IndexBufferClass::Lock_Native_Buffer(size_t offset, size_t byteCount,
	int flags, void **data)
{
	if (data == nullptr)
		return false;
	*data = nullptr;
	rts::render::RenderBufferUpdateMode mode;
	return native_buffer != nullptr && Get_Native_Buffer_Update_Mode(flags, &mode) &&
		native_buffer->Lock(offset, byteCount, mode, data) ==
		rts::render::RENDER_RESULT_OK;
}

bool DX8IndexBufferClass::Unlock_Native_Buffer()
{
	return native_buffer != nullptr &&
		native_buffer->Unlock() == rts::render::RENDER_RESULT_OK;
}

SortingIndexBufferClass::SortingIndexBufferClass(unsigned short indexCount)
	: IndexBufferClass(BUFFER_TYPE_SORTING, indexCount),
	index_buffer(W3DNEWARRAY unsigned short[indexCount])
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(indexCount != 0);
}

SortingIndexBufferClass::~SortingIndexBufferClass()
{
	delete[] index_buffer;
}

DynamicIBAccessClass::DynamicIBAccessClass(unsigned short type_,
	unsigned short indexCount)
	: Type(type_), IndexCount(indexCount), IndexBufferOffset(0), IndexBuffer(nullptr)
{
	WWASSERT(Type == BUFFER_TYPE_DYNAMIC_DX8 ||
		Type == BUFFER_TYPE_DYNAMIC_SORTING);
	if (Type == BUFFER_TYPE_DYNAMIC_DX8)
		Allocate_DX8_Dynamic_Buffer();
	else
		Allocate_Sorting_Dynamic_Buffer();
}

DynamicIBAccessClass::~DynamicIBAccessClass()
{
	const bool valid = Is_Valid();
	REF_PTR_RELEASE(IndexBuffer);
	if (Type == BUFFER_TYPE_DYNAMIC_DX8)
	{
		g_dynamicIndexBufferInUse = false;
		if (valid)
			g_dynamicIndexBufferOffset += IndexCount;
	}
	else
	{
		g_dynamicSortingIndexArrayInUse = false;
		if (valid)
			g_dynamicSortingIndexArrayOffset += IndexCount;
	}
}

bool DynamicIBAccessClass::Is_Valid() const
{
	if (IndexBuffer == nullptr || IndexCount == 0 ||
		IndexBufferOffset > IndexBuffer->Get_Index_Count() ||
		static_cast<unsigned int>(IndexCount) >
			static_cast<unsigned int>(IndexBuffer->Get_Index_Count()) -
			IndexBufferOffset)
		return false;
	return Type != BUFFER_TYPE_DYNAMIC_DX8 ||
		static_cast<DX8IndexBufferClass *>(IndexBuffer)->Is_Valid();
}

void DynamicIBAccessClass::_Deinit()
{
	WWASSERT(g_dynamicIndexBuffer == nullptr ||
		g_dynamicIndexBuffer->Num_Refs() == 1);
	REF_PTR_RELEASE(g_dynamicIndexBuffer);
	g_dynamicIndexBufferInUse = false;
	g_dynamicIndexBufferSize = DEFAULT_IB_SIZE;
	g_dynamicIndexBufferOffset = 0;
	WWASSERT(g_dynamicSortingIndexArray == nullptr ||
		g_dynamicSortingIndexArray->Num_Refs() == 1);
	REF_PTR_RELEASE(g_dynamicSortingIndexArray);
	g_dynamicSortingIndexArrayInUse = false;
	g_dynamicSortingIndexArraySize = 0;
	g_dynamicSortingIndexArrayOffset = 0;
}

bool DynamicIBAccessClass::Acquire_Native_Index_Buffer(
	rts::render::GpuHandle *validated) const
{
	if (validated == nullptr)
		return false;
	*validated = rts::render::GpuHandle();
	if (!rts::render::IsNativeW3DBufferOwnerThread() ||
		Type != BUFFER_TYPE_DYNAMIC_DX8 || !Is_Valid() ||
		!static_cast<DX8IndexBufferClass *>(IndexBuffer)->Is_Valid())
		return false;
	return static_cast<DX8IndexBufferClass *>(IndexBuffer)->
		Acquire_Native_Index_Buffer(
			static_cast<unsigned int>(IndexBufferOffset) * sizeof(unsigned short),
			0, IndexCount, validated);
}

unsigned int DynamicIBAccessClass::Get_Index_Buffer_Offset() const
{
	return !rts::render::IsNativeW3DBufferOwnerThread() || !Is_Valid() ? 0U :
		static_cast<unsigned int>(IndexBufferOffset) * sizeof(unsigned short);
}

const unsigned short *DynamicIBAccessClass::Get_Sorted_Index_Data() const
{
	if (!rts::render::IsNativeW3DBufferOwnerThread() ||
		Type != BUFFER_TYPE_DYNAMIC_SORTING || !Is_Valid())
		return nullptr;
	return static_cast<const SortingIndexBufferClass *>(IndexBuffer)->
		Get_Index_Data() + IndexBufferOffset;
}

DynamicIBAccessClass::WriteLockClass::WriteLockClass(
	DynamicIBAccessClass *dynamicAccess)
	: DynamicIBAccess(dynamicAccess), Indices(nullptr), Locked(false),
	Referenced(false)
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	if (DynamicIBAccess == nullptr ||
		(DynamicIBAccess->Get_Type() != BUFFER_TYPE_DYNAMIC_DX8 &&
			DynamicIBAccess->Get_Type() != BUFFER_TYPE_DYNAMIC_SORTING) ||
		DynamicIBAccess->IndexBuffer == nullptr ||
		DynamicIBAccess->IndexCount == 0 ||
		DynamicIBAccess->IndexBufferOffset >
			DynamicIBAccess->IndexBuffer->Get_Index_Count() ||
		static_cast<unsigned int>(DynamicIBAccess->IndexCount) >
			static_cast<unsigned int>(DynamicIBAccess->IndexBuffer->
				Get_Index_Count()) - DynamicIBAccess->IndexBufferOffset ||
		(DynamicIBAccess->Get_Type() != BUFFER_TYPE_DYNAMIC_DX8 &&
			!DynamicIBAccess->Is_Valid()))
		return;
	DynamicIBAccess->IndexBuffer->Add_Ref();
	Referenced = true;
	switch (DynamicIBAccess->Get_Type())
	{
	case BUFFER_TYPE_DYNAMIC_DX8:
		Locked = static_cast<DX8IndexBufferClass *>(
			DynamicIBAccess->IndexBuffer)->Lock_Native_Buffer(
				static_cast<size_t>(DynamicIBAccess->IndexBufferOffset) *
					sizeof(unsigned short),
				static_cast<size_t>(DynamicIBAccess->Get_Index_Count()) *
					sizeof(unsigned short),
				!DynamicIBAccess->IndexBufferOffset ?
					NATIVE_BUFFER_LOCK_DISCARD : NATIVE_BUFFER_LOCK_NO_OVERWRITE,
				reinterpret_cast<void **>(&Indices));
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Indices = static_cast<SortingIndexBufferClass *>(
			DynamicIBAccess->IndexBuffer)->index_buffer +
			DynamicIBAccess->IndexBufferOffset;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

DynamicIBAccessClass::WriteLockClass::~WriteLockClass()
{
	if (!rts::render::IsNativeW3DBufferOwnerThread())
	{
		return;
	}
	Commit();
	if (Referenced && DynamicIBAccess != nullptr &&
		DynamicIBAccess->IndexBuffer != nullptr)
		DynamicIBAccess->IndexBuffer->Release_Ref();
	Referenced = false;
}

bool DynamicIBAccessClass::WriteLockClass::Commit()
{
	if (!Locked || DynamicIBAccess == nullptr ||
		!DynamicIBAccess->Is_Valid())
		return false;
	const unsigned int changeFlags = !DynamicIBAccess->IndexBufferOffset ?
		NATIVE_BUFFER_LOCK_DISCARD : NATIVE_BUFFER_LOCK_NO_OVERWRITE;
	bool changed = Locked;
	switch (DynamicIBAccess->Get_Type())
	{
	case BUFFER_TYPE_DYNAMIC_DX8:
		changed = Locked && static_cast<DX8IndexBufferClass *>(
			DynamicIBAccess->IndexBuffer)->Unlock_Native_Buffer();
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
		DynamicIBAccess->IndexBuffer->Mark_Changed_Range(
			DynamicIBAccess->IndexBufferOffset,
			DynamicIBAccess->Get_Index_Count(), changeFlags);
	Locked = false;
	Indices = nullptr;
	return changed;
}

void DynamicIBAccessClass::Allocate_DX8_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	if (!rts::render::IsNativeW3DBufferOwnerThread() || IndexCount == 0 ||
		g_dynamicIndexBufferOffset > 65535U -
			static_cast<unsigned int>(IndexCount))
	{
		IndexBuffer = nullptr;
		IndexBufferOffset = 0;
		g_dynamicIndexBufferInUse = false;
		return;
	}
	WWASSERT(!g_dynamicIndexBufferInUse);
	g_dynamicIndexBufferInUse = true;
	if (IndexCount > g_dynamicIndexBufferSize)
	{
		REF_PTR_RELEASE(g_dynamicIndexBuffer);
		g_dynamicIndexBufferSize = IndexCount;
		if (g_dynamicIndexBufferSize < DEFAULT_IB_SIZE)
			g_dynamicIndexBufferSize = DEFAULT_IB_SIZE;
	}
	if (!g_dynamicIndexBuffer)
	{
		g_dynamicIndexBuffer = NEW_REF(DX8IndexBufferClass,
			(g_dynamicIndexBufferSize, DX8IndexBufferClass::USAGE_DYNAMIC));
		g_dynamicIndexBufferOffset = 0;
	}
	if (g_dynamicIndexBuffer != nullptr && !g_dynamicIndexBuffer->Is_Valid())
		REF_PTR_RELEASE(g_dynamicIndexBuffer);
	if (static_cast<unsigned>(IndexCount) + g_dynamicIndexBufferOffset >
		g_dynamicIndexBufferSize)
		g_dynamicIndexBufferOffset = 0;
	REF_PTR_SET(IndexBuffer, g_dynamicIndexBuffer);
	IndexBufferOffset = g_dynamicIndexBufferOffset;
}

void DynamicIBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	if (!rts::render::IsNativeW3DBufferOwnerThread() || IndexCount == 0 ||
		g_dynamicSortingIndexArrayOffset > 65535U -
			static_cast<unsigned int>(IndexCount))
	{
		IndexBuffer = nullptr;
		IndexBufferOffset = 0;
		g_dynamicSortingIndexArrayInUse = false;
		return;
	}
	WWASSERT(!g_dynamicSortingIndexArrayInUse);
	g_dynamicSortingIndexArrayInUse = true;
	const unsigned newIndexCount = g_dynamicSortingIndexArrayOffset + IndexCount;
	if (newIndexCount > 65535U)
	{
		IndexBuffer = nullptr;
		IndexBufferOffset = 0;
		g_dynamicSortingIndexArrayInUse = false;
		return;
	}
	if (newIndexCount > g_dynamicSortingIndexArraySize)
	{
		REF_PTR_RELEASE(g_dynamicSortingIndexArray);
		g_dynamicSortingIndexArraySize = static_cast<unsigned short>(newIndexCount);
		if (g_dynamicSortingIndexArraySize < DEFAULT_IB_SIZE)
			g_dynamicSortingIndexArraySize = DEFAULT_IB_SIZE;
	}
	if (!g_dynamicSortingIndexArray)
	{
		g_dynamicSortingIndexArray = NEW_REF(SortingIndexBufferClass,
			(g_dynamicSortingIndexArraySize));
		g_dynamicSortingIndexArrayOffset = 0;
	}
	REF_PTR_SET(IndexBuffer, g_dynamicSortingIndexArray);
	IndexBufferOffset = g_dynamicSortingIndexArrayOffset;
}

void DynamicIBAccessClass::_Reset(bool frameChanged)
{
	g_dynamicSortingIndexArrayOffset = 0;
	if (frameChanged)
		g_dynamicIndexBufferOffset = 0;
}

unsigned short DynamicIBAccessClass::Get_Default_Index_Count()
{
	return g_dynamicIndexBufferSize;
}

namespace rts
{
namespace render
{
RenderResult Reset_Native_W3D_Buffer_Allocators(bool frameChanged)
{
	if (!IsNativeW3DBufferOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (g_dynamicVertexBufferInUse || g_dynamicSortingVertexArrayInUse ||
		g_dynamicIndexBufferInUse || g_dynamicSortingIndexArrayInUse)
		return RENDER_RESULT_INVALID_ARGUMENT;
	if ((g_dynamicVertexBuffer != nullptr &&
			!g_dynamicVertexBuffer->Is_Valid()) ||
		(g_dynamicIndexBuffer != nullptr && !g_dynamicIndexBuffer->Is_Valid()))
		return RENDER_RESULT_FAILED;
	DynamicVBAccessClass::_Reset(frameChanged);
	DynamicIBAccessClass::_Reset(frameChanged);
	return RENDER_RESULT_OK;
}
}
}
