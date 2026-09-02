#ifndef RTS_WW3D2_NEUTRAL_INDEX_BUFFER_H
#define RTS_WW3D2_NEUTRAL_INDEX_BUFFER_H

#include "WWLib/always.h"
#include "WWLib/refcount.h"
#include "WWDebug/wwdebug.h"
#include "Renderer/RendererDevice.h"

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

class DX8IndexBufferClass;
class SortingIndexBufferClass;
class SortingRendererClass;

class IndexBufferClass : public RefCountClass
{
protected:
	virtual ~IndexBufferClass() override;
public:
	IndexBufferClass(unsigned type, unsigned short index_count);
	bool Copy(unsigned int *indices, unsigned start_index, unsigned index_count);
	bool Copy(unsigned short *indices, unsigned start_index, unsigned index_count);
	unsigned short Get_Index_Count() const { return index_count; }
	unsigned int Get_Generation() const { return generation; }
	void Mark_Changed();
	void Mark_Changed_Range(unsigned int offset, unsigned int count,
		unsigned int flags);
	bool Get_Change_Since(unsigned int uploaded_generation,
		unsigned int *offset, unsigned int *count, unsigned int *flags) const;
	unsigned Type() const { return type; }
	void Add_Engine_Ref() const;
	void Release_Engine_Ref() const;
	unsigned Engine_Refs() const { return engine_refs; }

	class WriteLockClass
	{
		IndexBufferClass *index_buffer;
		unsigned short *indices;
		bool locked;
	public:
		WriteLockClass(IndexBufferClass *index_buffer, int flags = 0);
		~WriteLockClass();
		unsigned short *Get_Index_Array() { return indices; }
		bool Is_Locked() const { return locked; }
		bool Commit();
	};

	class AppendLockClass
	{
		IndexBufferClass *index_buffer;
		unsigned short *indices;
		bool locked;
	public:
		AppendLockClass(IndexBufferClass *index_buffer,
			unsigned start_index, unsigned index_range);
		~AppendLockClass();
		unsigned short *Get_Index_Array() { return indices; }
		bool Is_Locked() const { return locked; }
		bool Commit();
	};

	static unsigned Get_Total_Buffer_Count();
	static unsigned Get_Total_Allocated_Indices();
	static unsigned Get_Total_Allocated_Memory();

protected:
	mutable int engine_refs;
	unsigned short index_count;
	unsigned type;
	unsigned int generation;
	unsigned int change_base_generation;
	unsigned int change_offset;
	unsigned int change_count;
	unsigned int change_flags;
};

class DynamicIBAccessClass
{
	W3DMPO_CODE(DynamicIBAccessClass)
	friend class SortingRendererClass;
	unsigned Type;
	unsigned short IndexCount;
	unsigned short IndexBufferOffset;
	IndexBufferClass *IndexBuffer;
	void Allocate_Sorting_Dynamic_Buffer();
	void Allocate_DX8_Dynamic_Buffer();
public:
	DynamicIBAccessClass(unsigned short type, unsigned short index_count);
	~DynamicIBAccessClass();
	unsigned Get_Type() const { return Type; }
	unsigned short Get_Index_Count() const { return IndexCount; }
	bool Is_Valid() const;
#if defined(_WIN64)
	bool Acquire_Native_Index_Buffer(rts::render::GpuHandle *validated) const;
	unsigned int Get_Index_Buffer_Offset() const;
	const unsigned short *Get_Sorted_Index_Data() const;
#endif
	static void _Deinit();
	static void _Reset(bool frame_changed);
	static unsigned short Get_Default_Index_Count();

	class WriteLockClass
	{
		DynamicIBAccessClass *DynamicIBAccess;
		unsigned short *Indices;
		bool Locked;
		bool Referenced;
	public:
		WriteLockClass(DynamicIBAccessClass *ib_access);
		~WriteLockClass();
		unsigned short *Get_Index_Array() { return Indices; }
		bool Is_Locked() const { return Locked; }
		bool Commit();
	};
	friend class WriteLockClass;
};

class DX8IndexBufferClass : public IndexBufferClass
{
	W3DMPO_CODE(DX8IndexBufferClass)
	friend class IndexBufferClass::WriteLockClass;
	friend class IndexBufferClass::AppendLockClass;
	friend class DynamicIBAccessClass::WriteLockClass;
public:
	enum UsageType
	{
		USAGE_DEFAULT = 0,
		USAGE_DYNAMIC = 1,
		USAGE_SOFTWAREPROCESSING = 2,
		USAGE_NPATCHES = 4
	};

	DX8IndexBufferClass(unsigned short index_count,
		UsageType usage = USAGE_DEFAULT);
	virtual ~DX8IndexBufferClass() override;
	bool Is_Valid() const;
	bool Lock_Buffer(size_t byte_offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Buffer();
	long Lock(unsigned int byte_offset, unsigned int byte_count,
		unsigned char **data, unsigned long flags);
	long Unlock();
#if defined(_WIN64)
	bool Acquire_Native_Index_Buffer(unsigned int offset,
		unsigned int start_index, unsigned int index_count,
		rts::render::GpuHandle *validated) const;
#endif

private:
#if defined(_WIN64)
	rts::render::NativeW3DBufferOwner *native_buffer;
	bool Lock_Native_Buffer(size_t offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Native_Buffer();
#else
	// Keep the historical one-pointer object layout for the external x86
	// implementation without importing its device type into product code.
	void *legacy_buffer;
#endif
};

class SortingIndexBufferClass : public IndexBufferClass
{
	W3DMPO_CODE(SortingIndexBufferClass)
	friend class DynamicIBAccessClass::WriteLockClass;
	friend class IndexBufferClass::WriteLockClass;
	friend class IndexBufferClass::AppendLockClass;
public:
	SortingIndexBufferClass(unsigned short index_count);
	virtual ~SortingIndexBufferClass() override;
	const unsigned short *Get_Index_Data() const { return index_buffer; }

protected:
	unsigned short *index_buffer;
};

#if defined(_WIN64)
// Called by the native renderer lifecycle seam at a frame boundary. It
// resets both dynamic stream allocators and reports an outstanding-use or
// failed-resource condition instead of silently discarding it.
namespace rts
{
namespace render
{
RenderResult Reset_Native_W3D_Buffer_Allocators(
	bool frame_changed);
}
}
#endif

#endif
