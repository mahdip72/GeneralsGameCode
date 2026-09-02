/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8indexbuffer.cpp                     $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Jani_p                                                      $*
 *                                                                                             *
 *                     $Modtime:: 11/09/01 3:12p                                              $*
 *                                                                                             *
 *                    $Revision:: 26                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define INDEX_BUFFER_LOG

#include "dx8indexbuffer.h"
#include "dx8wrapper.h"
#include "dx8caps.h"
#include "WWMath/sphere.h"
#include "WWLib/thread.h"
#include "WWDebug/wwmemlog.h"
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
#include "nativew3dbufferowner.h"
#include "dx8nativecompat.h"
#endif

#define DEFAULT_IB_SIZE 5000

static bool _DynamicSortingIndexArrayInUse=false;
static SortingIndexBufferClass* _DynamicSortingIndexArray;
static unsigned short _DynamicSortingIndexArraySize=0;
static unsigned short _DynamicSortingIndexArrayOffset=0;

static bool _DynamicDX8IndexBufferInUse=false;
static DX8IndexBufferClass* _DynamicDX8IndexBuffer=nullptr;
static unsigned short _DynamicDX8IndexBufferSize=DEFAULT_IB_SIZE;
static unsigned short _DynamicDX8IndexBufferOffset=0;

static int _IndexBufferCount;
static int _IndexBufferTotalIndices;
static int _IndexBufferTotalSize;

static bool Use_Index_Range_Lock(unsigned int first_index,
	unsigned int count, unsigned int index_count)
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return first_index != 0 || count != index_count;
#else
	return first_index != 0;
#endif
}

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
static bool Get_Native_Buffer_Update_Mode(int flags,
	rts::render::RenderBufferUpdateMode *mode)
{
	return Decode_Native_Buffer_Update_Mode(
		static_cast<unsigned int>(flags), mode);
}
#endif

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

IndexBufferClass::IndexBufferClass(unsigned type_, unsigned short index_count_)
	:
	index_count(index_count_),
	type(type_),
	engine_refs(0),
	generation(1),
	change_base_generation(0),
	change_offset(0),
	change_count(index_count_),
	change_flags(0)
{
	WWASSERT(type==BUFFER_TYPE_DX8 || type==BUFFER_TYPE_SORTING);
	WWASSERT(index_count);

	_IndexBufferCount++;
	_IndexBufferTotalIndices+=index_count;
	_IndexBufferTotalSize+=index_count*sizeof(unsigned short);
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("New IB, %d indices, size %d bytes",index_count,index_count*sizeof(unsigned short)));
	WWDEBUG_SAY(("Total IB count: %d, total %d indices, total size %d bytes",
		_IndexBufferCount,
		_IndexBufferTotalIndices,
		_IndexBufferTotalSize));
#endif
}

IndexBufferClass::~IndexBufferClass()
{
	_IndexBufferCount--;
	_IndexBufferTotalIndices-=index_count;
	_IndexBufferTotalSize-=index_count*sizeof(unsigned short);
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("Delete IB, %d indices, size %d bytes",index_count,index_count*sizeof(unsigned short)));
	WWDEBUG_SAY(("Total IB count: %d, total %d indices, total size %d bytes",
		_IndexBufferCount,
		_IndexBufferTotalIndices,
		_IndexBufferTotalSize));
#endif
}

unsigned IndexBufferClass::Get_Total_Buffer_Count()
{
	return _IndexBufferCount;
}

unsigned IndexBufferClass::Get_Total_Allocated_Indices()
{
	return _IndexBufferTotalIndices;
}

unsigned IndexBufferClass::Get_Total_Allocated_Memory()
{
	return _IndexBufferTotalSize;
}

void IndexBufferClass::Add_Engine_Ref() const
{
	engine_refs++;
}

void IndexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs>=0);
}

void IndexBufferClass::Mark_Changed()
{
	Mark_Changed_Range(0, index_count, 0);
}

void IndexBufferClass::Mark_Changed_Range(unsigned int offset,
	unsigned int count, unsigned int flags)
{
	change_base_generation = generation;
	++generation;
	if (generation == 0)
	{
		++generation;
	}
	change_offset = offset;
	change_count = count;
	change_flags = flags;
}

bool IndexBufferClass::Get_Change_Since(unsigned int uploaded_generation,
	unsigned int *offset, unsigned int *count, unsigned int *flags) const
{
	if (offset == nullptr || count == nullptr || flags == nullptr ||
		change_base_generation != uploaded_generation || change_count == 0)
	{
		return false;
	}
	*offset = change_offset;
	*count = change_count;
	*flags = change_flags;
	return true;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

bool IndexBufferClass::Copy(unsigned int* indices,unsigned first_index,unsigned count)
{
	WWASSERT(indices);
	if (indices == nullptr || first_index > Get_Index_Count() ||
		count > Get_Index_Count() - first_index)
	{
		return false;
	}
	if (count == 0)
	{
		return true;
	}

	if (Use_Index_Range_Lock(first_index, count, Get_Index_Count())) {
		DX8IndexBufferClass::AppendLockClass l(this,first_index,count);
		unsigned short* inds=l.Get_Index_Array();
		if (!l.Is_Locked() || inds == nullptr)
		{
			return false;
		}
		for (unsigned v=0;v<count;++v) {
			*inds++=(unsigned short)(*indices++);
		}
		return l.Commit();
	}
	else {
		DX8IndexBufferClass::WriteLockClass l(this);
		unsigned short* inds=l.Get_Index_Array();
		if (!l.Is_Locked() || inds == nullptr)
		{
			return false;
		}
		for (unsigned v=0;v<count;++v) {
			*inds++=(unsigned short)(*indices++);
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------

bool IndexBufferClass::Copy(unsigned short* indices,unsigned first_index,unsigned count)
{
	WWASSERT(indices);
	if (indices == nullptr || first_index > Get_Index_Count() ||
		count > Get_Index_Count() - first_index)
	{
		return false;
	}
	if (count == 0)
	{
		return true;
	}

	if (Use_Index_Range_Lock(first_index, count, Get_Index_Count())) {
		DX8IndexBufferClass::AppendLockClass l(this,first_index,count);
		unsigned short* inds=l.Get_Index_Array();
		if (!l.Is_Locked() || inds == nullptr)
		{
			return false;
		}
		for (unsigned v=0;v<count;++v) {
			*inds++=*indices++;
		}
		return l.Commit();
	}
	else {
		DX8IndexBufferClass::WriteLockClass l(this);
		unsigned short* inds=l.Get_Index_Array();
		if (!l.Is_Locked() || inds == nullptr)
		{
			return false;
		}
		for (unsigned v=0;v<count;++v) {
			*inds++=*indices++;
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------------


IndexBufferClass::WriteLockClass::WriteLockClass(IndexBufferClass* index_buffer_, int flags) :
	index_buffer(index_buffer_), indices(nullptr), locked(false)
{
	DX8_THREAD_ASSERT();
	WWASSERT(index_buffer);
	WWASSERT(!index_buffer->Engine_Refs());
	index_buffer->Add_Ref();
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		locked = static_cast<DX8IndexBufferClass *>(index_buffer)->
			Lock_Native_Buffer(0,
				static_cast<size_t>(index_buffer->Get_Index_Count()) * sizeof(WORD),
				flags, reinterpret_cast<void **>(&indices));
#else
		{
			DX8_Assert();
			const HRESULT result = static_cast<DX8IndexBufferClass*>(index_buffer)->Get_DX8_Index_Buffer()->Lock(
				0, index_buffer->Get_Index_Count()*sizeof(WORD),
				(unsigned char**)&indices, flags);
			DX8_ErrorCode(result);
			locked = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_SORTING:
		indices=static_cast<SortingIndexBufferClass*>(index_buffer)->index_buffer;
		locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------------

IndexBufferClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	Commit();
	index_buffer->Release_Ref();
}

bool IndexBufferClass::WriteLockClass::Commit()
{
	if (!locked)
	{
		return false;
	}
	bool changed = true;
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		changed = locked && static_cast<DX8IndexBufferClass *>(index_buffer)->
			Unlock_Native_Buffer();
#else
		DX8_Assert();
		if (locked)
		{
			const HRESULT result = static_cast<DX8IndexBufferClass*>(index_buffer)->index_buffer->Unlock();
			DX8_ErrorCode(result);
			changed = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
	{
		index_buffer->Mark_Changed();
	}
	locked = false;
	indices = nullptr;
	return changed;
}

// ----------------------------------------------------------------------------

IndexBufferClass::AppendLockClass::AppendLockClass(IndexBufferClass* index_buffer_,unsigned start_index, unsigned index_range)
	:
	index_buffer(index_buffer_), indices(nullptr), locked(false)
{
	DX8_THREAD_ASSERT();
	WWASSERT(start_index+index_range<=index_buffer->Get_Index_Count());
	WWASSERT(index_buffer);
	WWASSERT(!index_buffer->Engine_Refs());
	index_buffer->Add_Ref();
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		locked = static_cast<DX8IndexBufferClass *>(index_buffer)->
			Lock_Native_Buffer(
				static_cast<size_t>(start_index) * sizeof(unsigned short),
				static_cast<size_t>(index_range) * sizeof(unsigned short), 0,
				reinterpret_cast<void **>(&indices));
#else
		{
			DX8_Assert();
			const HRESULT result = static_cast<DX8IndexBufferClass*>(index_buffer)->index_buffer->Lock(
				start_index*sizeof(unsigned short),
				index_range*sizeof(unsigned short),
				(unsigned char**)&indices, 0);
			DX8_ErrorCode(result);
			locked = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_SORTING:
		indices=static_cast<SortingIndexBufferClass*>(index_buffer)->index_buffer+start_index;
		locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

IndexBufferClass::AppendLockClass::~AppendLockClass()
{
	DX8_THREAD_ASSERT();
	Commit();
	index_buffer->Release_Ref();
}

bool IndexBufferClass::AppendLockClass::Commit()
{
	if (!locked)
	{
		return false;
	}
	bool changed = locked;
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		changed = locked && static_cast<DX8IndexBufferClass *>(index_buffer)->
			Unlock_Native_Buffer();
#else
		DX8_Assert();
		if (locked)
		{
			const HRESULT result = static_cast<DX8IndexBufferClass*>(index_buffer)->index_buffer->Unlock();
			DX8_ErrorCode(result);
			changed = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
	{
		index_buffer->Mark_Changed();
	}
	locked = false;
	indices = nullptr;
	return changed;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DX8IndexBufferClass::DX8IndexBufferClass(unsigned short index_count_,UsageType usage)
	:
	IndexBufferClass(BUFFER_TYPE_DX8,index_count_)
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	,native_buffer(nullptr)
#else
	,index_buffer(nullptr)
#endif
{
	DX8_THREAD_ASSERT();
	WWASSERT(index_count);
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	native_buffer = W3DNEW rts::render::NativeW3DBufferOwner;
	if (native_buffer == nullptr)
	{
		WWDEBUG_SAY(("Native index buffer owner allocation failed"));
		return;
	}
	rts::render::BufferDescriptor descriptor;
	descriptor.byteCount = static_cast<size_t>(index_count) * sizeof(WORD);
	descriptor.stride = sizeof(WORD);
	descriptor.binding = rts::render::RENDER_BUFFER_INDEX;
	descriptor.usage = (usage & USAGE_DYNAMIC) != 0 ?
		rts::render::RENDER_USAGE_DYNAMIC : rts::render::RENDER_USAGE_DEFAULT;
	const rts::render::RenderResult result = native_buffer->Create(descriptor);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		WWDEBUG_SAY(("Native index buffer creation failed: %d",
			static_cast<int>(result)));
		delete native_buffer;
		native_buffer = nullptr;
	}
#else
	unsigned usage_flags=
		D3DUSAGE_WRITEONLY|
		((usage&USAGE_DYNAMIC) ? D3DUSAGE_DYNAMIC : 0)|
		((usage&USAGE_NPATCHES) ? D3DUSAGE_NPATCHES : 0)|
		((usage&USAGE_SOFTWAREPROCESSING) ? D3DUSAGE_SOFTWAREPROCESSING : 0);
	if (!DX8Wrapper::Get_Current_Caps()->Support_TnL()) {
		usage_flags|=D3DUSAGE_SOFTWAREPROCESSING;
	}

	HRESULT ret=DX8Wrapper::_Get_D3D_Device8()->CreateIndexBuffer(
		sizeof(WORD)*index_count,
		usage_flags,
		D3DFMT_INDEX16,
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&index_buffer);

	if (SUCCEEDED(ret)) {
		return;
	}

	WWDEBUG_SAY(("Index buffer creation failed, trying to release assets..."));

	// Index buffer creation failed, so try releasing least used textures and flushing the mesh cache.

	// Free all textures that haven't been used in the last 5 seconds
	TextureClass::Invalidate_Old_Unused_Textures(5000);

	// Invalidate the mesh cache
	WW3D::_Invalidate_Mesh_Cache();

	// Try again...
	ret=DX8Wrapper::_Get_D3D_Device8()->CreateIndexBuffer(
		sizeof(WORD)*index_count,
		usage_flags,
		D3DFMT_INDEX16,
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&index_buffer);

	if (SUCCEEDED(ret)) {
		WWDEBUG_SAY(("...Index buffer creation successful"));
	}

	// If it still fails it is fatal
	DX8_ErrorCode(ret);
#endif
}

// ----------------------------------------------------------------------------

DX8IndexBufferClass::~DX8IndexBufferClass()
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	delete native_buffer;
	native_buffer = nullptr;
#else
	if (index_buffer != nullptr)
	{
		index_buffer->Release();
	}
#endif
}

bool DX8IndexBufferClass::Is_Valid() const
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return native_buffer != nullptr && !native_buffer->HasFailedMutation();
#else
	return index_buffer != nullptr;
#endif
}

bool DX8IndexBufferClass::Lock_Buffer(size_t byte_offset,
	size_t byte_count, int flags, void **data)
{
	if (data == nullptr)
	{
		return false;
	}
	*data = nullptr;
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return Lock_Native_Buffer(byte_offset, byte_count, flags, data);
#else
	if (index_buffer == nullptr)
	{
		return false;
	}
	DX8_Assert();
	const HRESULT result = index_buffer->Lock(
		static_cast<unsigned int>(byte_offset),
		static_cast<unsigned int>(byte_count),
		reinterpret_cast<unsigned char **>(data), flags);
	DX8_ErrorCode(result);
	return SUCCEEDED(result);
#endif
}

bool DX8IndexBufferClass::Unlock_Buffer()
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	const bool changed = Unlock_Native_Buffer();
#else
	if (index_buffer == nullptr)
	{
		return false;
	}
	DX8_Assert();
	const HRESULT result = index_buffer->Unlock();
	DX8_ErrorCode(result);
	const bool changed = SUCCEEDED(result);
#endif
	if (changed)
	{
		Mark_Changed();
	}
	return changed;
}

long DX8IndexBufferClass::Lock(unsigned int byte_offset,
	unsigned int byte_count, unsigned char **data, unsigned long flags)
{
	#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return Lock_Buffer(byte_offset, byte_count, static_cast<int>(flags),
		reinterpret_cast<void **>(data)) ? 0L : -1L;
	#else
	return Lock_Buffer(byte_offset, byte_count, flags,
		reinterpret_cast<void **>(data)) ? D3D_OK : E_FAIL;
	#endif
}

long DX8IndexBufferClass::Unlock()
{
	#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return Unlock_Buffer() ? 0L : -1L;
	#else
	return Unlock_Buffer() ? D3D_OK : E_FAIL;
	#endif
}

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
bool DX8IndexBufferClass::Acquire_Native_Index_Buffer(unsigned int offset,
	unsigned int start_index, unsigned int index_count,
	rts::render::GpuHandle *validated) const
{
	if (validated == nullptr)
	{
		return false;
	}
	*validated = rts::render::GpuHandle();
	return native_buffer != nullptr &&
		native_buffer->AcquireIndexRange(rts::render::RENDER_FORMAT_R16_UINT,
			offset, start_index, index_count, validated) ==
			rts::render::RENDER_RESULT_OK;
}

bool DX8IndexBufferClass::Lock_Native_Buffer(size_t offset,
	size_t byte_count, int flags, void **data)
{
	if (data == nullptr)
	{
		return false;
	}
	*data = nullptr;
	rts::render::RenderBufferUpdateMode mode;
	return native_buffer != nullptr &&
		Get_Native_Buffer_Update_Mode(flags, &mode) &&
		native_buffer->Lock(offset, byte_count, mode, data) ==
			rts::render::RENDER_RESULT_OK;
}

bool DX8IndexBufferClass::Unlock_Native_Buffer()
{
	return native_buffer != nullptr &&
		native_buffer->Unlock() == rts::render::RENDER_RESULT_OK;
}
#endif

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

SortingIndexBufferClass::SortingIndexBufferClass(unsigned short index_count_)
	:
	IndexBufferClass(BUFFER_TYPE_SORTING,index_count_)
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(index_count);

	index_buffer=W3DNEWARRAY unsigned short[index_count];
}

// ----------------------------------------------------------------------------

SortingIndexBufferClass::~SortingIndexBufferClass()
{
	delete[] index_buffer;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicIBAccessClass::DynamicIBAccessClass(unsigned short type_, unsigned short index_count_)
	:
	Type(type_),
	IndexCount(index_count_),
	IndexBufferOffset(0),
	IndexBuffer(nullptr)
{
	WWASSERT(Type==BUFFER_TYPE_DYNAMIC_DX8 || Type==BUFFER_TYPE_DYNAMIC_SORTING);
	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		Allocate_DX8_Dynamic_Buffer();
	}
	else {
		Allocate_Sorting_Dynamic_Buffer();
	}
}

DynamicIBAccessClass::~DynamicIBAccessClass()
{
	const bool valid = Is_Valid();
	REF_PTR_RELEASE(IndexBuffer);
	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		_DynamicDX8IndexBufferInUse=false;
		if (valid) _DynamicDX8IndexBufferOffset+=IndexCount;
	}
	else {
		_DynamicSortingIndexArrayInUse=false;
		if (valid) _DynamicSortingIndexArrayOffset+=IndexCount;
	}
}

bool DynamicIBAccessClass::Is_Valid() const
{
	if (IndexBuffer == nullptr)
	{
		return false;
	}
	return Type != BUFFER_TYPE_DYNAMIC_DX8 ||
		static_cast<DX8IndexBufferClass *>(IndexBuffer)->Is_Valid();
}

void DynamicIBAccessClass::_Deinit()
{
	WWASSERT ((_DynamicDX8IndexBuffer == nullptr) || (_DynamicDX8IndexBuffer->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicDX8IndexBuffer);
	_DynamicDX8IndexBufferInUse=false;
	_DynamicDX8IndexBufferSize=DEFAULT_IB_SIZE;
	_DynamicDX8IndexBufferOffset=0;

	WWASSERT ((_DynamicSortingIndexArray == nullptr) || (_DynamicSortingIndexArray->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicSortingIndexArray);
	_DynamicSortingIndexArrayInUse=false;
	_DynamicSortingIndexArraySize=0;
	_DynamicSortingIndexArrayOffset=0;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicIBAccessClass::WriteLockClass::WriteLockClass(DynamicIBAccessClass* ib_access_)
	:
	DynamicIBAccess(ib_access_), Indices(nullptr), Locked(false), Referenced(false)
{
	DX8_THREAD_ASSERT();
	if (DynamicIBAccess == nullptr)
	{
		return;
	}
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	// A failed publication is recoverable only through a zero-offset discard;
	// defer the validity decision to Lock_Native_Buffer so it can recreate the
	// native generation.
	if (DynamicIBAccess->IndexBuffer == nullptr)
	{
		return;
	}
#else
	if (!DynamicIBAccess->Is_Valid())
	{
		return;
	}
#endif
	DynamicIBAccess->IndexBuffer->Add_Ref();
	Referenced = true;
	switch (DynamicIBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
		WWASSERT(DynamicIBAccess);
//		WWASSERT(!dynamic_dx8_index_buffer->Engine_Refs());
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		Locked = static_cast<DX8IndexBufferClass *>(
			DynamicIBAccess->IndexBuffer)->Lock_Native_Buffer(
				static_cast<size_t>(DynamicIBAccess->IndexBufferOffset) * sizeof(WORD),
				static_cast<size_t>(DynamicIBAccess->Get_Index_Count()) * sizeof(WORD),
				!DynamicIBAccess->IndexBufferOffset ?
					NATIVE_BUFFER_LOCK_DISCARD : NATIVE_BUFFER_LOCK_NO_OVERWRITE,
				reinterpret_cast<void **>(&Indices));
#else
		{
			DX8_Assert();
			const HRESULT result = static_cast<DX8IndexBufferClass*>(DynamicIBAccess->IndexBuffer)->Get_DX8_Index_Buffer()->Lock(
				DynamicIBAccess->IndexBufferOffset*sizeof(WORD),
				DynamicIBAccess->Get_Index_Count()*sizeof(WORD),
				(unsigned char**)&Indices,
				!DynamicIBAccess->IndexBufferOffset ? D3DLOCK_DISCARD :
					D3DLOCK_NOOVERWRITE);
			DX8_ErrorCode(result);
			Locked = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Indices=static_cast<SortingIndexBufferClass*>(DynamicIBAccess->IndexBuffer)->index_buffer;
		Indices+=DynamicIBAccess->IndexBufferOffset;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

DynamicIBAccessClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	Commit();
	if (Referenced && DynamicIBAccess != nullptr &&
		DynamicIBAccess->IndexBuffer != nullptr)
	{
		DynamicIBAccess->IndexBuffer->Release_Ref();
	}
	Referenced = false;
}

bool DynamicIBAccessClass::WriteLockClass::Commit()
{
	if (!Locked || DynamicIBAccess == nullptr || !DynamicIBAccess->Is_Valid())
	{
		return false;
	}
	#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	const unsigned int change_flags =
		!DynamicIBAccess->IndexBufferOffset ?
			NATIVE_BUFFER_LOCK_DISCARD : NATIVE_BUFFER_LOCK_NO_OVERWRITE;
	#else
	const unsigned int change_flags =
		!DynamicIBAccess->IndexBufferOffset ?
			D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
	#endif
	bool changed = Locked;
	switch (DynamicIBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		changed = Locked && static_cast<DX8IndexBufferClass *>(
			DynamicIBAccess->IndexBuffer)->Unlock_Native_Buffer();
#else
		DX8_Assert();
		Publish_Render_Buffer_Change(
			static_cast<DX8IndexBufferClass *>(
				DynamicIBAccess->IndexBuffer)->Get_DX8_Index_Buffer(),
			rts::render::RENDER_BUFFER_INDEX, Indices,
			static_cast<size_t>(DynamicIBAccess->Get_Index_Count()) * sizeof(WORD),
			static_cast<size_t>(DynamicIBAccess->IndexBufferOffset) * sizeof(WORD),
			!DynamicIBAccess->IndexBufferOffset ?
				rts::render::RENDER_BUFFER_UPDATE_DISCARD :
				rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE,
			DynamicIBAccess->IndexBuffer->Get_Generation());
		if (Locked)
		{
			const HRESULT result = static_cast<DX8IndexBufferClass*>(DynamicIBAccess->IndexBuffer)->Get_DX8_Index_Buffer()->Unlock();
			DX8_ErrorCode(result);
			changed = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	if (changed)
	{
		DynamicIBAccess->IndexBuffer->Mark_Changed_Range(
			DynamicIBAccess->IndexBufferOffset,
			DynamicIBAccess->Get_Index_Count(), change_flags);
	}
	Locked = false;
	Indices = nullptr;
	return changed;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DynamicIBAccessClass::Allocate_DX8_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicDX8IndexBufferInUse);
	_DynamicDX8IndexBufferInUse=true;

	// If requesting more indices than dynamic index buffer can fit, delete the ib
	// and adjust the size to the new count.
	if (IndexCount>_DynamicDX8IndexBufferSize) {
		REF_PTR_RELEASE(_DynamicDX8IndexBuffer);
		_DynamicDX8IndexBufferSize=IndexCount;
		if (_DynamicDX8IndexBufferSize<DEFAULT_IB_SIZE) _DynamicDX8IndexBufferSize=DEFAULT_IB_SIZE;
	}

	// Create a new vb if one doesn't exist currently
	if (!_DynamicDX8IndexBuffer) {
		unsigned usage=DX8IndexBufferClass::USAGE_DYNAMIC;
#if !defined(_WIN64) || !defined(RTS_RENDERER_HAS_D3D11)
		if (DX8Wrapper::Get_Current_Caps()->Support_NPatches()) {
			usage|=DX8IndexBufferClass::USAGE_NPATCHES;
		}
#endif

		_DynamicDX8IndexBuffer=NEW_REF(DX8IndexBufferClass,(
			_DynamicDX8IndexBufferSize,
			(DX8IndexBufferClass::UsageType)usage));
		_DynamicDX8IndexBufferOffset=0;
	}
	if (_DynamicDX8IndexBuffer != nullptr &&
		!_DynamicDX8IndexBuffer->Is_Valid())
	{
		REF_PTR_RELEASE(_DynamicDX8IndexBuffer);
	}

	// Any room at the end of the buffer?
	if (((unsigned)IndexCount+_DynamicDX8IndexBufferOffset)>_DynamicDX8IndexBufferSize) {
		_DynamicDX8IndexBufferOffset=0;
	}

	REF_PTR_SET(IndexBuffer,_DynamicDX8IndexBuffer);
	IndexBufferOffset=_DynamicDX8IndexBufferOffset;
}

void DynamicIBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicSortingIndexArrayInUse);
	_DynamicSortingIndexArrayInUse=true;

	unsigned new_index_count=_DynamicSortingIndexArrayOffset+IndexCount;
	WWASSERT(new_index_count<65536);
	if (new_index_count>_DynamicSortingIndexArraySize) {
		REF_PTR_RELEASE(_DynamicSortingIndexArray);
		_DynamicSortingIndexArraySize=new_index_count;
		if (_DynamicSortingIndexArraySize<DEFAULT_IB_SIZE) _DynamicSortingIndexArraySize=DEFAULT_IB_SIZE;
	}

	if (!_DynamicSortingIndexArray) {
		_DynamicSortingIndexArray=NEW_REF(SortingIndexBufferClass,(_DynamicSortingIndexArraySize));
		_DynamicSortingIndexArrayOffset=0;
	}

	REF_PTR_SET(IndexBuffer,_DynamicSortingIndexArray);
	IndexBufferOffset=_DynamicSortingIndexArrayOffset;
}

void DynamicIBAccessClass::_Reset(bool frame_changed)
{
	_DynamicSortingIndexArrayOffset=0;
	if (frame_changed) _DynamicDX8IndexBufferOffset=0;
}

unsigned short DynamicIBAccessClass::Get_Default_Index_Count()
{
	return _DynamicDX8IndexBufferSize;
}
