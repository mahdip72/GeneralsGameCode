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
 *                     $Archive:: /Commando/Code/ww3d2/dx8vertexbuffer.cpp                    $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 5:06p                                             $*
 *                                                                                             *
 *                    $Revision:: 39                                                          $*
 *                                                                                             *
 * 06/26/02 KM VB Vertex format size update for shaders                                       *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define VERTEX_BUFFER_LOG

#include "dx8vertexbuffer.h"
#include "dx8wrapper.h"
#include "dx8fvf.h"
#include "dx8caps.h"
#include "WWLib/thread.h"
#include "WWDebug/wwmemlog.h"
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
#include "nativew3dbufferowner.h"
#endif

static bool Use_Vertex_Range_Lock(unsigned int first_vertex,
	unsigned int count, unsigned int vertex_count)
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return first_vertex != 0 || count != vertex_count;
#else
	return first_vertex != 0;
#endif
}

#define DEFAULT_VB_SIZE 5000

static bool _DynamicSortingVertexArrayInUse=false;
//static VertexFormatXYZNDUV2* _DynamicSortingVertexArray=nullptr;
static SortingVertexBufferClass* _DynamicSortingVertexArray=nullptr;
static unsigned short _DynamicSortingVertexArraySize=0;
static unsigned short _DynamicSortingVertexArrayOffset=0;

static bool _DynamicDX8VertexBufferInUse=false;
static DX8VertexBufferClass* _DynamicDX8VertexBuffer=nullptr;
static unsigned short _DynamicDX8VertexBufferSize=DEFAULT_VB_SIZE;
static unsigned short _DynamicDX8VertexBufferOffset=0;

static const FVFInfoClass _DynamicFVFInfo(dynamic_fvf_type);

static int _DX8VertexBufferCount=0;

static int _VertexBufferCount;
static int _VertexBufferTotalVertices;
static int _VertexBufferTotalSize;

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
static bool Get_Native_Buffer_Update_Mode(int flags,
	rts::render::RenderBufferUpdateMode *mode)
{
	const unsigned int supported_flags = D3DLOCK_DISCARD |
		D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK;
	if (mode == nullptr ||
		(static_cast<unsigned int>(flags) & ~supported_flags) != 0 ||
		((flags & D3DLOCK_DISCARD) != 0 &&
		 (flags & D3DLOCK_NOOVERWRITE) != 0))
	{
		return false;
	}
	if ((flags & D3DLOCK_DISCARD) != 0)
	{
		*mode = rts::render::RENDER_BUFFER_UPDATE_DISCARD;
	}
	else if ((flags & D3DLOCK_NOOVERWRITE) != 0)
	{
		*mode = rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE;
	}
	else
	{
		*mode = rts::render::RENDER_BUFFER_UPDATE_PRESERVE;
	}
	return true;
}
#endif

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::VertexBufferClass(unsigned type_, unsigned FVF, unsigned short vertex_count_)
	:
	VertexCount(vertex_count_),
	type(type_),
	engine_refs(0),
	generation(1),
	change_base_generation(0),
	change_offset(0),
	change_count(vertex_count_),
	change_flags(0)
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(VertexCount);
	WWASSERT(type==BUFFER_TYPE_DX8 || type==BUFFER_TYPE_SORTING);
	WWASSERT(FVF != 0);
	fvf_info=W3DNEW FVFInfoClass(FVF);

	_VertexBufferCount++;
	_VertexBufferTotalVertices+=VertexCount;
	_VertexBufferTotalSize+=VertexCount*fvf_info->Get_FVF_Size();
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("New VB, %d vertices, size %d bytes",VertexCount,VertexCount*fvf_info->Get_FVF_Size()));
	WWDEBUG_SAY(("Total VB count: %d, total %d vertices, total size %d bytes",
		_VertexBufferCount,
		_VertexBufferTotalVertices,
		_VertexBufferTotalSize));
#endif
}

// ----------------------------------------------------------------------------

VertexBufferClass::~VertexBufferClass()
{
	_VertexBufferCount--;
	_VertexBufferTotalVertices-=VertexCount;
	_VertexBufferTotalSize-=VertexCount*fvf_info->Get_FVF_Size();

#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("Delete VB, %d vertices, size %d bytes",VertexCount,VertexCount*fvf_info->Get_FVF_Size()));
	WWDEBUG_SAY(("Total VB count: %d, total %d vertices, total size %d bytes",
		_VertexBufferCount,
		_VertexBufferTotalVertices,
		_VertexBufferTotalSize));
#endif
	delete fvf_info;
}

unsigned VertexBufferClass::Get_Total_Buffer_Count()
{
	return _VertexBufferCount;
}

unsigned VertexBufferClass::Get_Total_Allocated_Vertices()
{
	return _VertexBufferTotalVertices;
}

unsigned VertexBufferClass::Get_Total_Allocated_Memory()
{
	return _VertexBufferTotalSize;
}


// ----------------------------------------------------------------------------

void VertexBufferClass::Add_Engine_Ref() const
{
	engine_refs++;
}

// ----------------------------------------------------------------------------

void VertexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs>=0);
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
	{
		++generation;
	}
	change_offset = offset;
	change_count = count;
	change_flags = flags;
}

bool VertexBufferClass::Get_Change_Since(unsigned int uploaded_generation,
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

VertexBufferClass::WriteLockClass::WriteLockClass(VertexBufferClass* VertexBuffer, int flags)
	:
	VertexBufferLockClass(VertexBuffer)
{
	DX8_THREAD_ASSERT();
	WWASSERT(VertexBuffer);
	WWASSERT(!VertexBuffer->Engine_Refs());
	VertexBuffer->Add_Ref();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#ifdef VERTEX_BUFFER_LOG
		{
		StringClass fvf_name;
		VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("VertexBuffer->Lock(start_index: 0, index_range: 0(%d), fvf_size: %d, fvf: %s)",
			VertexBuffer->Get_Vertex_Count(),
			VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		Locked = static_cast<DX8VertexBufferClass *>(VertexBuffer)->
			Lock_Native_Buffer(0, 0, flags, &Vertices);
#else
		{
			DX8_Assert();
			const HRESULT result = static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Lock(
				0, 0, (unsigned char**)&Vertices, flags);
			DX8_ErrorCode(result);
			Locked = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(VertexBuffer)->VertexBuffer;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

VertexBufferClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	Commit();
	VertexBuffer->Release_Ref();
}

bool VertexBufferClass::WriteLockClass::Commit()
{
	if (!Locked)
	{
		return false;
	}
	bool changed = Locked;
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#ifdef VERTEX_BUFFER_LOG
		WWDEBUG_SAY(("VertexBuffer->Unlock()"));
#endif
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		changed = Locked && static_cast<DX8VertexBufferClass *>(VertexBuffer)->
			Unlock_Native_Buffer();
#else
		DX8_Assert();
		if (Locked)
		{
			const HRESULT result = static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Unlock();
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
		VertexBuffer->Mark_Changed();
	}
	Locked = false;
	Vertices = nullptr;
	return changed;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::AppendLockClass::AppendLockClass(VertexBufferClass* VertexBuffer,unsigned start_index, unsigned index_range)
	:
	VertexBufferLockClass(VertexBuffer)
{
	DX8_THREAD_ASSERT();
	WWASSERT(VertexBuffer);
	WWASSERT(!VertexBuffer->Engine_Refs());
	WWASSERT(start_index+index_range<=VertexBuffer->Get_Vertex_Count());
	VertexBuffer->Add_Ref();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#ifdef VERTEX_BUFFER_LOG
		{
		StringClass fvf_name;
		VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("VertexBuffer->Lock(start_index: %d, index_range: %d, fvf_size: %d, fvf: %s)",
			start_index,
			index_range,
			VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		Locked = static_cast<DX8VertexBufferClass *>(VertexBuffer)->
			Lock_Native_Buffer(
				static_cast<size_t>(start_index) * VertexBuffer->FVF_Info().Get_FVF_Size(),
				static_cast<size_t>(index_range) * VertexBuffer->FVF_Info().Get_FVF_Size(),
				0, &Vertices);
#else
		{
			DX8_Assert();
			const HRESULT result = static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Lock(
				start_index*VertexBuffer->FVF_Info().Get_FVF_Size(),
				index_range*VertexBuffer->FVF_Info().Get_FVF_Size(),
				(unsigned char**)&Vertices, 0);
			DX8_ErrorCode(result);
			Locked = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(VertexBuffer)->VertexBuffer+start_index;
		Locked = true;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

VertexBufferClass::AppendLockClass::~AppendLockClass()
{
	DX8_THREAD_ASSERT();
	Commit();
	VertexBuffer->Release_Ref();
}

bool VertexBufferClass::AppendLockClass::Commit()
{
	if (!Locked)
	{
		return false;
	}
	bool changed = Locked;
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#ifdef VERTEX_BUFFER_LOG
		WWDEBUG_SAY(("VertexBuffer->Unlock()"));
#endif
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		changed = Locked && static_cast<DX8VertexBufferClass *>(VertexBuffer)->
			Unlock_Native_Buffer();
#else
		DX8_Assert();
		if (Locked)
		{
			const HRESULT result = static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Unlock();
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
		VertexBuffer->Mark_Changed();
	}
	Locked = false;
	Vertices = nullptr;
	return changed;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

SortingVertexBufferClass::SortingVertexBufferClass(unsigned short VertexCount)
	:
	VertexBufferClass(BUFFER_TYPE_SORTING, dynamic_fvf_type, VertexCount)
{
	WWMEMLOG(MEM_RENDERER);
	VertexBuffer=W3DNEWARRAY VertexFormatXYZNDUV2[VertexCount];
}

// ----------------------------------------------------------------------------

SortingVertexBufferClass::~SortingVertexBufferClass()
{
	delete[] VertexBuffer;
}


// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

//	bool dynamic=false,bool softwarevp=false);

DX8VertexBufferClass::DX8VertexBufferClass(unsigned FVF, unsigned short vertex_count_, UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, FVF, vertex_count_),
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	NativeBuffer(nullptr)
#else
	VertexBuffer(nullptr)
#endif
{
	Create_Vertex_Buffer(usage);
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector3* normals,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_NORMAL, VertexCount),
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	NativeBuffer(nullptr)
#else
	VertexBuffer(nullptr)
#endif
{
	WWASSERT(vertices);
	WWASSERT(normals);
	WWASSERT(tex_coords);

	Create_Vertex_Buffer(usage);
	if (!Copy(vertices,normals,tex_coords,0,VertexCount))
	{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		delete NativeBuffer;
		NativeBuffer = nullptr;
#else
		if (VertexBuffer != nullptr) VertexBuffer->Release();
		VertexBuffer = nullptr;
#endif
	}
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector3* normals,
	const Vector4* diffuse,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_NORMAL|D3DFVF_DIFFUSE, VertexCount),
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	NativeBuffer(nullptr)
#else
	VertexBuffer(nullptr)
#endif
{
	WWASSERT(vertices);
	WWASSERT(normals);
	WWASSERT(tex_coords);
	WWASSERT(diffuse);

	Create_Vertex_Buffer(usage);
	if (!Copy(vertices,normals,tex_coords,diffuse,0,VertexCount))
	{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		delete NativeBuffer;
		NativeBuffer = nullptr;
#else
		if (VertexBuffer != nullptr) VertexBuffer->Release();
		VertexBuffer = nullptr;
#endif
	}
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector4* diffuse,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_DIFFUSE, VertexCount),
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	NativeBuffer(nullptr)
#else
	VertexBuffer(nullptr)
#endif
{
	WWASSERT(vertices);
	WWASSERT(tex_coords);
	WWASSERT(diffuse);

	Create_Vertex_Buffer(usage);
	if (!Copy(vertices,tex_coords,diffuse,0,VertexCount))
	{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		delete NativeBuffer;
		NativeBuffer = nullptr;
#else
		if (VertexBuffer != nullptr) VertexBuffer->Release();
		VertexBuffer = nullptr;
#endif
	}
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1, VertexCount),
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	NativeBuffer(nullptr)
#else
	VertexBuffer(nullptr)
#endif
{
	WWASSERT(vertices);
	WWASSERT(tex_coords);

	Create_Vertex_Buffer(usage);
	if (!Copy(vertices,tex_coords,0,VertexCount))
	{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		delete NativeBuffer;
		NativeBuffer = nullptr;
#else
		if (VertexBuffer != nullptr) VertexBuffer->Release();
		VertexBuffer = nullptr;
#endif
	}
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::~DX8VertexBufferClass()
{
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("VertexBuffer->Release()"));
	_DX8VertexBufferCount--;
	WWDEBUG_SAY(("Current vertex buffer count: %d",_DX8VertexBufferCount));
#endif
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	delete NativeBuffer;
	NativeBuffer = nullptr;
#else
	if (VertexBuffer != nullptr)
	{
		VertexBuffer->Release();
	}
#endif
}

bool DX8VertexBufferClass::Is_Valid() const
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	return NativeBuffer != nullptr && !NativeBuffer->HasFailedMutation();
#else
	return VertexBuffer != nullptr;
#endif
}

bool DX8VertexBufferClass::Lock_Buffer(size_t byte_offset,
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
	if (VertexBuffer == nullptr)
	{
		return false;
	}
	DX8_Assert();
	const HRESULT result = VertexBuffer->Lock(
		static_cast<unsigned int>(byte_offset),
		static_cast<unsigned int>(byte_count),
		reinterpret_cast<unsigned char **>(data), flags);
	DX8_ErrorCode(result);
	return SUCCEEDED(result);
#endif
}

bool DX8VertexBufferClass::Unlock_Buffer()
{
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	const bool changed = Unlock_Native_Buffer();
#else
	if (VertexBuffer == nullptr)
	{
		return false;
	}
	DX8_Assert();
	const HRESULT result = VertexBuffer->Unlock();
	DX8_ErrorCode(result);
	const bool changed = SUCCEEDED(result);
#endif
	if (changed)
	{
		Mark_Changed();
	}
	return changed;
}

HRESULT DX8VertexBufferClass::Lock(UINT byte_offset, UINT byte_count,
	unsigned char **data, DWORD flags)
{
	return Lock_Buffer(byte_offset, byte_count, flags,
		reinterpret_cast<void **>(data)) ? D3D_OK : E_FAIL;
}

HRESULT DX8VertexBufferClass::Unlock()
{
	return Unlock_Buffer() ? D3D_OK : E_FAIL;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Create_Vertex_Buffer(UsageType usage)
{
	DX8_THREAD_ASSERT();
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	WWASSERT(!NativeBuffer);
	NativeBuffer = W3DNEW rts::render::NativeW3DBufferOwner;
	if (NativeBuffer == nullptr)
	{
		WWDEBUG_SAY(("Native vertex buffer owner allocation failed"));
		return;
	}
	rts::render::BufferDescriptor descriptor;
	descriptor.byteCount = static_cast<size_t>(FVF_Info().Get_FVF_Size()) *
		VertexCount;
	descriptor.stride = FVF_Info().Get_FVF_Size();
	descriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
	descriptor.usage = (usage & USAGE_DYNAMIC) != 0 ?
		rts::render::RENDER_USAGE_DYNAMIC : rts::render::RENDER_USAGE_DEFAULT;
	const rts::render::RenderResult result = NativeBuffer->Create(descriptor);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		WWDEBUG_SAY(("Native vertex buffer creation failed: %d",
			static_cast<int>(result)));
		delete NativeBuffer;
		NativeBuffer = nullptr;
	}
	return;
#else
	WWASSERT(!VertexBuffer);

#ifdef VERTEX_BUFFER_LOG
	StringClass fvf_name;
	FVF_Info().Get_FVF_Name(fvf_name);
	WWDEBUG_SAY(("CreateVertexBuffer(fvfsize=%d, vertex_count=%d, D3DUSAGE_WRITEONLY|%s|%s, fvf: %s, %s)",
		FVF_Info().Get_FVF_Size(),
		VertexCount,
		(usage&USAGE_DYNAMIC) ? "D3DUSAGE_DYNAMIC" : "-",
		(usage&USAGE_SOFTWAREPROCESSING) ? "D3DUSAGE_SOFTWAREPROCESSING" : "-",
		fvf_name,
		(usage&USAGE_DYNAMIC) ? "D3DPOOL_DEFAULT" : "D3DPOOL_MANAGED"));
	_DX8VertexBufferCount++;
	WWDEBUG_SAY(("Current vertex buffer count: %d",_DX8VertexBufferCount));
#endif

	unsigned usage_flags=
		D3DUSAGE_WRITEONLY|
		((usage&USAGE_DYNAMIC) ? D3DUSAGE_DYNAMIC : 0)|
		((usage&USAGE_NPATCHES) ? D3DUSAGE_NPATCHES : 0)|
		((usage&USAGE_SOFTWAREPROCESSING) ? D3DUSAGE_SOFTWAREPROCESSING : 0);
	// New Code
	if (!DX8Wrapper::Get_Current_Caps()->Support_TnL()) {
		usage_flags|=D3DUSAGE_SOFTWAREPROCESSING;
	}

	HRESULT ret=DX8Wrapper::_Get_D3D_Device8()->CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer);
	if (SUCCEEDED(ret)) {
		return;
	}

	WWDEBUG_SAY(("Vertex buffer creation failed, trying to release assets..."));

	// Vertex buffer creation failed, so try releasing least used textures and flushing the mesh cache.

	// Free all textures that haven't been used in the last 5 seconds
	TextureClass::Invalidate_Old_Unused_Textures(5000);

	// Invalidate the mesh cache
	WW3D::_Invalidate_Mesh_Cache();

	//@todo: Find some way to invalidate the textures too
	ret = DX8Wrapper::_Get_D3D_Device8()->ResourceManagerDiscardBytes(0);

	// Try again...
	ret=DX8Wrapper::_Get_D3D_Device8()->CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer);

	if (SUCCEEDED(ret)) {
		WWDEBUG_SAY(("...Vertex buffer creation successful"));
	}

	// If it still fails it is fatal
	DX8_ErrorCode(ret);

	/* Old Code
	DX8CALL(CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer));
	*/
#endif
}

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
bool DX8VertexBufferClass::Acquire_Native_Vertex_Buffer(unsigned int stride,
	unsigned int offset, unsigned int start_vertex, unsigned int vertex_count,
	rts::render::GpuHandle *validated) const
{
	if (validated == nullptr)
	{
		return false;
	}
	*validated = rts::render::GpuHandle();
	return NativeBuffer != nullptr &&
		NativeBuffer->AcquireVertexRange(stride, offset, start_vertex,
			vertex_count, validated) == rts::render::RENDER_RESULT_OK;
}

bool DX8VertexBufferClass::Lock_Native_Buffer(size_t offset,
	size_t byte_count, int flags, void **data)
{
	if (data == nullptr)
	{
		return false;
	}
	*data = nullptr;
	rts::render::RenderBufferUpdateMode mode;
	return NativeBuffer != nullptr && Get_Native_Buffer_Update_Mode(flags, &mode) &&
		NativeBuffer->Lock(offset, byte_count, mode, data) ==
			rts::render::RENDER_RESULT_OK;
}

bool DX8VertexBufferClass::Unlock_Native_Buffer()
{
	return NativeBuffer != nullptr &&
		NativeBuffer->Unlock() == rts::render::RENDER_RESULT_OK;
}
#endif

// ----------------------------------------------------------------------------

bool DX8VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, unsigned first_vertex,unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(uv);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZNUV1);
	if (loc == nullptr || norm == nullptr || uv == nullptr ||
		first_vertex > VertexCount || count > VertexCount - first_vertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZNUV1)
	{
		return false;
	}
	if (count == 0) return true;

	if (Use_Vertex_Range_Lock(first_vertex, count, Get_Vertex_Count())) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZNUV1* verts=(VertexFormatXYZNUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
		return l.Commit();
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZNUV1* verts=(VertexFormatXYZNUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------

bool DX8VertexBufferClass::Copy(const Vector3* loc, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZ);
	if (loc == nullptr || first_vertex > VertexCount ||
		count > VertexCount - first_vertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZ)
	{
		return false;
	}
	if (count == 0) return true;

	if (Use_Vertex_Range_Lock(first_vertex, count, Get_Vertex_Count())) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZ* verts=(VertexFormatXYZ*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
		}
		return l.Commit();
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZ* verts=(VertexFormatXYZ*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------

bool DX8VertexBufferClass::Copy(const Vector3* loc, const Vector2* uv, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(uv);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZUV1);
	if (loc == nullptr || uv == nullptr || first_vertex > VertexCount ||
		count > VertexCount - first_vertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZUV1)
	{
		return false;
	}
	if (count == 0) return true;

	if (Use_Vertex_Range_Lock(first_vertex, count, Get_Vertex_Count())) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZUV1* verts=(VertexFormatXYZUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
		return l.Commit();
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZUV1* verts=(VertexFormatXYZUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------

bool DX8VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZN);
	if (loc == nullptr || norm == nullptr || first_vertex > VertexCount ||
		count > VertexCount - first_vertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZN)
	{
		return false;
	}
	if (count == 0) return true;

	if (Use_Vertex_Range_Lock(first_vertex, count, Get_Vertex_Count())) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZN* verts=(VertexFormatXYZN*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
		}
		return l.Commit();
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZN* verts=(VertexFormatXYZN*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------

bool DX8VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(uv);
	WWASSERT(diffuse);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZNDUV1);
	if (loc == nullptr || norm == nullptr || uv == nullptr ||
		diffuse == nullptr || first_vertex > VertexCount ||
		count > VertexCount - first_vertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZNDUV1)
	{
		return false;
	}
	if (count == 0) return true;

	if (Use_Vertex_Range_Lock(first_vertex, count, Get_Vertex_Count())) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZNDUV1* verts=(VertexFormatXYZNDUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
		return l.Commit();
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZNDUV1* verts=(VertexFormatXYZNDUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------

bool DX8VertexBufferClass::Copy(const Vector3* loc, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(uv);
	WWASSERT(diffuse);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZDUV1);
	if (loc == nullptr || uv == nullptr || diffuse == nullptr ||
		first_vertex > VertexCount || count > VertexCount - first_vertex ||
		FVF_Info().Get_FVF() != DX8_FVF_XYZDUV1)
	{
		return false;
	}
	if (count == 0) return true;

	if (Use_Vertex_Range_Lock(first_vertex, count, Get_Vertex_Count())) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZDUV1* verts=(VertexFormatXYZDUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
		return l.Commit();
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZDUV1* verts=(VertexFormatXYZDUV1*)l.Get_Vertex_Array();
		if (!l.Is_Locked() || verts == nullptr) return false;
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
		return l.Commit();
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicVBAccessClass::DynamicVBAccessClass(unsigned t,unsigned fvf,unsigned short vertex_count_)
	:
	Type(t),
	FVFInfo(_DynamicFVFInfo),
	VertexCount(vertex_count_),
	VertexBufferOffset(0),
	VertexBuffer(nullptr)
{
	WWASSERT(fvf==dynamic_fvf_type);
	WWASSERT(Type==BUFFER_TYPE_DYNAMIC_DX8 || Type==BUFFER_TYPE_DYNAMIC_SORTING);

	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		Allocate_DX8_Dynamic_Buffer();
	}
	else {
		Allocate_Sorting_Dynamic_Buffer();
	}
}

DynamicVBAccessClass::~DynamicVBAccessClass()
{
	const bool valid = Is_Valid();
	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		_DynamicDX8VertexBufferInUse=false;
		if (valid) _DynamicDX8VertexBufferOffset+=(unsigned) VertexCount;
	}
	else {
		_DynamicSortingVertexArrayInUse=false;
		if (valid) _DynamicSortingVertexArrayOffset+=VertexCount;
	}

	REF_PTR_RELEASE (VertexBuffer);
}

bool DynamicVBAccessClass::Is_Valid() const
{
	if (VertexBuffer == nullptr)
	{
		return false;
	}
	return Type != BUFFER_TYPE_DYNAMIC_DX8 ||
		static_cast<DX8VertexBufferClass *>(VertexBuffer)->Is_Valid();
}

// ----------------------------------------------------------------------------

void DynamicVBAccessClass::_Deinit()
{
	WWASSERT ((_DynamicDX8VertexBuffer == nullptr) || (_DynamicDX8VertexBuffer->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicDX8VertexBuffer);
	_DynamicDX8VertexBufferInUse=false;
	_DynamicDX8VertexBufferSize=DEFAULT_VB_SIZE;
	_DynamicDX8VertexBufferOffset=0;

	WWASSERT ((_DynamicSortingVertexArray == nullptr) || (_DynamicSortingVertexArray->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicSortingVertexArray);
	WWASSERT(!_DynamicSortingVertexArrayInUse);
	_DynamicSortingVertexArrayInUse=false;
	_DynamicSortingVertexArraySize=0;
	_DynamicSortingVertexArrayOffset=0;
}

void DynamicVBAccessClass::Allocate_DX8_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicDX8VertexBufferInUse);
	_DynamicDX8VertexBufferInUse=true;

	// If requesting more vertices than dynamic vertex buffer can fit, delete the vb
	// and adjust the size to the new count.
	if (VertexCount>_DynamicDX8VertexBufferSize) {
		REF_PTR_RELEASE(_DynamicDX8VertexBuffer);
		_DynamicDX8VertexBufferSize=VertexCount;
		if (_DynamicDX8VertexBufferSize<DEFAULT_VB_SIZE) _DynamicDX8VertexBufferSize=DEFAULT_VB_SIZE;
	}

	// Create a new vb if one doesn't exist currently
	if (!_DynamicDX8VertexBuffer) {
		unsigned usage=DX8VertexBufferClass::USAGE_DYNAMIC;
#if !defined(_WIN64) || !defined(RTS_RENDERER_HAS_D3D11)
		if (DX8Wrapper::Get_Current_Caps()->Support_NPatches()) {
			usage|=DX8VertexBufferClass::USAGE_NPATCHES;
		}
#endif

		_DynamicDX8VertexBuffer=NEW_REF(DX8VertexBufferClass,(
			dynamic_fvf_type,
			_DynamicDX8VertexBufferSize,
			(DX8VertexBufferClass::UsageType)usage));
		_DynamicDX8VertexBufferOffset=0;
	}
	if (_DynamicDX8VertexBuffer != nullptr &&
		!_DynamicDX8VertexBuffer->Is_Valid())
	{
		REF_PTR_RELEASE(_DynamicDX8VertexBuffer);
	}

	// Any room at the end of the buffer?
	if (((unsigned)VertexCount+_DynamicDX8VertexBufferOffset)>_DynamicDX8VertexBufferSize) {
		_DynamicDX8VertexBufferOffset=0;
	}

	REF_PTR_SET(VertexBuffer,_DynamicDX8VertexBuffer);
	VertexBufferOffset=_DynamicDX8VertexBufferOffset;
}

void DynamicVBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicSortingVertexArrayInUse);
	_DynamicSortingVertexArrayInUse=true;

	unsigned new_vertex_count=_DynamicSortingVertexArrayOffset+VertexCount;
	WWASSERT(new_vertex_count<65536);
	if (new_vertex_count>_DynamicSortingVertexArraySize) {
		REF_PTR_RELEASE(_DynamicSortingVertexArray);
		_DynamicSortingVertexArraySize=new_vertex_count;
		if (_DynamicSortingVertexArraySize<DEFAULT_VB_SIZE) _DynamicSortingVertexArraySize=DEFAULT_VB_SIZE;
	}

	if (!_DynamicSortingVertexArray) {
		_DynamicSortingVertexArray=NEW_REF(SortingVertexBufferClass,(_DynamicSortingVertexArraySize));
		_DynamicSortingVertexArrayOffset=0;
	}

	REF_PTR_SET(VertexBuffer,_DynamicSortingVertexArray);
	VertexBufferOffset=_DynamicSortingVertexArrayOffset;
}

// ----------------------------------------------------------------------------
static int dx8_lock;
DynamicVBAccessClass::WriteLockClass::WriteLockClass(DynamicVBAccessClass* dynamic_vb_access_)
	:
	DynamicVBAccess(dynamic_vb_access_), Vertices(nullptr), Locked(false)
{
	DX8_THREAD_ASSERT();
	if (DynamicVBAccess == nullptr || !DynamicVBAccess->Is_Valid())
	{
		return;
	}
	switch (DynamicVBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
#ifdef VERTEX_BUFFER_LOG
		{
		WWASSERT(!dx8_lock);
		dx8_lock++;
		StringClass fvf_name;
		DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("DynamicVertexBuffer->Lock(start_index: %d, index_range: %d, fvf_size: %d, fvf: %s)",
			DynamicVBAccess->VertexBufferOffset,
			DynamicVBAccess->Get_Vertex_Count(),
			DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		WWASSERT(_DynamicDX8VertexBuffer);
//		WWASSERT(!_DynamicDX8VertexBuffer->Engine_Refs());

		DX8_Assert();
		// Lock with discard contents if the buffer offset is zero
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		Locked = static_cast<DX8VertexBufferClass *>(
			DynamicVBAccess->VertexBuffer)->Lock_Native_Buffer(
				static_cast<size_t>(DynamicVBAccess->VertexBufferOffset) *
					_DynamicDX8VertexBuffer->FVF_Info().Get_FVF_Size(),
				static_cast<size_t>(DynamicVBAccess->Get_Vertex_Count()) *
					DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
				D3DLOCK_NOSYSLOCK | (!DynamicVBAccess->VertexBufferOffset ?
					D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE),
				reinterpret_cast<void **>(&Vertices));
#else
		{
			const HRESULT result = static_cast<DX8VertexBufferClass*>(DynamicVBAccess->VertexBuffer)->Get_DX8_Vertex_Buffer()->Lock(
				DynamicVBAccess->VertexBufferOffset*_DynamicDX8VertexBuffer->FVF_Info().Get_FVF_Size(),
				DynamicVBAccess->Get_Vertex_Count()*DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
				(unsigned char**)&Vertices,
				D3DLOCK_NOSYSLOCK | (!DynamicVBAccess->VertexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE));
			DX8_ErrorCode(result);
			Locked = SUCCEEDED(result);
		}
#endif
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(DynamicVBAccess->VertexBuffer)->VertexBuffer;
		Vertices+=DynamicVBAccess->VertexBufferOffset;
		Locked = true;
//		vertices=_DynamicSortingVertexArray+_DynamicSortingVertexArrayOffset;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

DynamicVBAccessClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	Commit();
}

bool DynamicVBAccessClass::WriteLockClass::Commit()
{
	if (!Locked || DynamicVBAccess == nullptr ||
		!DynamicVBAccess->Is_Valid())
	{
		return false;
	}
	const unsigned int change_flags =
		!DynamicVBAccess->VertexBufferOffset ?
			D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
	bool changed = Locked;
	switch (DynamicVBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
#ifdef VERTEX_BUFFER_LOG
		dx8_lock--;
		WWASSERT(!dx8_lock);
		WWDEBUG_SAY(("DynamicVertexBuffer->Unlock()"));
#endif
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
		changed = Locked && static_cast<DX8VertexBufferClass *>(
			DynamicVBAccess->VertexBuffer)->Unlock_Native_Buffer();
#else
		DX8_Assert();
		Publish_Render_Buffer_Change(
			static_cast<DX8VertexBufferClass *>(
				DynamicVBAccess->VertexBuffer)->Get_DX8_Vertex_Buffer(),
			rts::render::RENDER_BUFFER_VERTEX, Vertices,
			static_cast<size_t>(DynamicVBAccess->Get_Vertex_Count()) *
				DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
			static_cast<size_t>(DynamicVBAccess->VertexBufferOffset) *
				DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
			!DynamicVBAccess->VertexBufferOffset ?
				rts::render::RENDER_BUFFER_UPDATE_DISCARD :
				rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE,
			DynamicVBAccess->VertexBuffer->Get_Generation());
		if (Locked)
		{
			const HRESULT result = static_cast<DX8VertexBufferClass*>(DynamicVBAccess->VertexBuffer)->Get_DX8_Vertex_Buffer()->Unlock();
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
		DynamicVBAccess->VertexBuffer->Mark_Changed_Range(
			DynamicVBAccess->VertexBufferOffset,
			DynamicVBAccess->Get_Vertex_Count(), change_flags);
	}
	Locked = false;
	Vertices = nullptr;
	return changed;
}

// ----------------------------------------------------------------------------

void DynamicVBAccessClass::_Reset(bool frame_changed)
{
	_DynamicSortingVertexArrayOffset=0;
	if (frame_changed) _DynamicDX8VertexBufferOffset=0;
}

unsigned short DynamicVBAccessClass::Get_Default_Vertex_Count()
{
	return _DynamicDX8VertexBufferSize;
}

