#ifndef RTS_WW3D2_NEUTRAL_VERTEX_BUFFER_H
#define RTS_WW3D2_NEUTRAL_VERTEX_BUFFER_H

#include "WWLib/always.h"
#include "WWLib/refcount.h"
#include "WWDebug/wwdebug.h"
#include "dx8fvf.h"

namespace rts { namespace render { class GpuHandle; class NativeW3DBufferOwner; } }

#ifndef RTS_WW3D2_BUFFER_TYPES_DEFINED
#define RTS_WW3D2_BUFFER_TYPES_DEFINED
enum
{
	BUFFER_TYPE_DX8,
	BUFFER_TYPE_SORTING,
	BUFFER_TYPE_DYNAMIC_DX8,
	BUFFER_TYPE_DYNAMIC_SORTING,
	BUFFER_TYPE_INVALID
};
#endif

class Vector2;
class Vector3;
class Vector4;
class StringClass;
class SortingRendererClass;
class VertexBufferClass;
class DX8VertexBufferClass;
struct VertexFormatXYZNDUV2;

const unsigned dynamic_fvf_type = DX8_FVF_XYZNDUV2;

class VertexBufferLockClass
{
protected:
	VertexBufferClass *VertexBuffer;
	void *Vertices;
	bool Locked;
	VertexBufferLockClass(VertexBufferClass *vertex_buffer) :
		VertexBuffer(vertex_buffer), Vertices(nullptr), Locked(false) {}
public:
	void *Get_Vertex_Array() { return Vertices; }
	bool Is_Locked() const { return Locked; }
};

class VertexBufferClass : public RefCountClass
{
protected:
	VertexBufferClass(unsigned type, unsigned FVF, unsigned short VertexCount);
	virtual ~VertexBufferClass() override;
public:
	const FVFInfoClass &FVF_Info() const { return *fvf_info; }
	unsigned short Get_Vertex_Count() const { return VertexCount; }
	unsigned Type() const { return type; }
	unsigned int Get_Generation() const { return generation; }
	void Mark_Changed();
	void Mark_Changed_Range(unsigned int offset, unsigned int count,
		unsigned int flags);
	bool Get_Change_Since(unsigned int uploaded_generation,
		unsigned int *offset, unsigned int *count, unsigned int *flags) const;
	void Add_Engine_Ref() const;
	void Release_Engine_Ref() const;
	unsigned Engine_Refs() const { return engine_refs; }

	class WriteLockClass : public VertexBufferLockClass
	{
	public:
		WriteLockClass(VertexBufferClass *vertex_buffer, int flags = 0);
		~WriteLockClass();
		bool Commit();
	};

	class AppendLockClass : public VertexBufferLockClass
	{
	public:
		AppendLockClass(VertexBufferClass *vertex_buffer,
			unsigned start_index, unsigned index_range);
		~AppendLockClass();
		bool Commit();
	};

	static unsigned Get_Total_Buffer_Count();
	static unsigned Get_Total_Allocated_Vertices();
	static unsigned Get_Total_Allocated_Memory();

protected:
	unsigned type;
	unsigned short VertexCount;
	mutable int engine_refs;
	FVFInfoClass *fvf_info;
	unsigned int generation;
	unsigned int change_base_generation;
	unsigned int change_offset;
	unsigned int change_count;
	unsigned int change_flags;
};

class DynamicVBAccessClass
{
	friend class SortingRendererClass;
	const FVFInfoClass &FVFInfo;
	unsigned Type;
	unsigned short VertexCount;
	unsigned short VertexBufferOffset;
	VertexBufferClass *VertexBuffer;
	void Allocate_Sorting_Dynamic_Buffer();
	void Allocate_DX8_Dynamic_Buffer();
public:
	DynamicVBAccessClass(unsigned type, unsigned fvf, unsigned short vertex_count);
	~DynamicVBAccessClass();
	const FVFInfoClass &FVF_Info() const { return FVFInfo; }
	unsigned Get_Type() const { return Type; }
	unsigned short Get_Vertex_Count() const { return VertexCount; }
	bool Is_Valid() const;
#if defined(_WIN64)
	bool Acquire_Native_Vertex_Buffer(rts::render::GpuHandle *validated) const;
	unsigned int Get_Vertex_Buffer_Offset() const;
	unsigned int Get_Vertex_Stride() const;
	const void *Get_Sorted_Vertex_Data() const;
#endif
	static void _Deinit();
	static void _Reset(bool frame_changed);
	static unsigned short Get_Default_Vertex_Count();

	class WriteLockClass
	{
		DynamicVBAccessClass *DynamicVBAccess;
		VertexFormatXYZNDUV2 *Vertices;
		bool Locked;
	public:
		WriteLockClass(DynamicVBAccessClass *vb_access);
		~WriteLockClass();
		bool Is_Locked() const { return Locked; }
		bool Commit();
		VertexFormatXYZNDUV2 *Get_Formatted_Vertex_Array();
	};
	friend class WriteLockClass;
};

inline VertexFormatXYZNDUV2 *DynamicVBAccessClass::WriteLockClass::
	Get_Formatted_Vertex_Array()
{
	if (!Locked || DynamicVBAccess == nullptr || !DynamicVBAccess->Is_Valid())
		return nullptr;
	WWASSERT(DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF() ==
		dynamic_fvf_type);
	return Vertices;
}

class DX8VertexBufferClass : public VertexBufferClass
{
	W3DMPO_CODE(DX8VertexBufferClass)
	friend class VertexBufferClass::WriteLockClass;
	friend class VertexBufferClass::AppendLockClass;
	friend class DynamicVBAccessClass::WriteLockClass;
protected:
	virtual ~DX8VertexBufferClass() override;
public:
	enum UsageType
	{
		USAGE_DEFAULT = 0,
		USAGE_DYNAMIC = 1,
		USAGE_SOFTWAREPROCESSING = 2,
		USAGE_NPATCHES = 4
	};

	DX8VertexBufferClass(unsigned FVF, unsigned short VertexCount,
		UsageType usage = USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3 *vertices, const Vector3 *normals,
		const Vector2 *tex_coords, unsigned short VertexCount,
		UsageType usage = USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3 *vertices, const Vector3 *normals,
		const Vector4 *diffuse, const Vector2 *tex_coords,
		unsigned short VertexCount, UsageType usage = USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3 *vertices, const Vector4 *diffuse,
		const Vector2 *tex_coords, unsigned short VertexCount,
		UsageType usage = USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3 *vertices, const Vector2 *tex_coords,
		unsigned short VertexCount, UsageType usage = USAGE_DEFAULT);
	bool Is_Valid() const;
	bool Lock_Buffer(size_t byte_offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Buffer();
	long Lock(unsigned int byte_offset, unsigned int byte_count,
		unsigned char **data, unsigned long flags);
	long Unlock();
#if defined(_WIN64)
	bool Acquire_Native_Vertex_Buffer(unsigned int stride, unsigned int offset,
		unsigned int start_vertex, unsigned int vertex_count,
		rts::render::GpuHandle *validated) const;
#endif
	bool Copy(const Vector3 *loc, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3 *loc, const Vector2 *uv,
		unsigned first_vertex, unsigned count);
	bool Copy(const Vector3 *loc, const Vector3 *norm,
		unsigned first_vertex, unsigned count);
	bool Copy(const Vector3 *loc, const Vector3 *norm, const Vector2 *uv,
		unsigned first_vertex, unsigned count);
	bool Copy(const Vector3 *loc, const Vector3 *norm, const Vector2 *uv,
		const Vector4 *diffuse, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3 *loc, const Vector2 *uv, const Vector4 *diffuse,
		unsigned first_vertex, unsigned count);

protected:
#if defined(_WIN64)
	rts::render::NativeW3DBufferOwner *NativeBuffer;
	bool Lock_Native_Buffer(size_t offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Native_Buffer();
#else
	// Keep the historical one-pointer object layout for the external x86
	// implementation without importing its device type into product code.
	void *LegacyBuffer;
#endif
	void Create_Vertex_Buffer(UsageType usage);
};

class SortingVertexBufferClass : public VertexBufferClass
{
	W3DMPO_CODE(SortingVertexBufferClass)
	friend class DynamicVBAccessClass::WriteLockClass;
	friend class VertexBufferClass::WriteLockClass;
	friend class VertexBufferClass::AppendLockClass;
	VertexFormatXYZNDUV2 *VertexBuffer;
protected:
	virtual ~SortingVertexBufferClass() override;
public:
	SortingVertexBufferClass(unsigned short VertexCount);
	const VertexFormatXYZNDUV2 *Get_Vertex_Data() const { return VertexBuffer; }
};

#endif
