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
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8vertexbuffer.h                      $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 5:06p                                             $*
 *                                                                                             *
 *                    $Revision:: 26                                                          $*
 *                                                                                             *
 * 06/26/02 KM VB Vertex format size update for shaders                                       *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "WWDebug/wwdebug.h"
#include "dx8fvf.h"

// Dynamic sorting streams use the serialized neutral FVF contract. The
// Win32/VC6 oracle still receives the equivalent D3D8 value through dx8fvf.h.
const unsigned dynamic_fvf_type=DX8_FVF_XYZNDUV2;

class DX8Wrapper;
class SortingRendererClass;
class Vector2;
class Vector3;
class Vector4;
class StringClass;
class DX8VertexBufferClass;
class FVFInfoClass;
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
namespace rts { namespace render { class GpuHandle; class NativeW3DBufferOwner; } }
#else
struct IDirect3DVertexBuffer8;
#endif
class VertexBufferClass;
struct VertexFormatXYZNDUV2;

class VertexBufferLockClass
{
protected:
	VertexBufferClass* VertexBuffer;
	void* Vertices;
	bool Locked;

	// This class can't be used directly, so constructor as to be protected
	VertexBufferLockClass(VertexBufferClass* vertex_buffer_) :
		VertexBuffer(vertex_buffer_), Vertices(nullptr), Locked(false) {}
public:
	void* Get_Vertex_Array() { return Vertices; }
	bool Is_Locked() const { return Locked; }
};

/**
** DX8VertexBufferClass
** This class wraps a DX8 vertex buffer.  Use the lock objects to modify or append to the vertex buffer.
*/
class VertexBufferClass : public RefCountClass
{
protected:
	VertexBufferClass(unsigned type, unsigned FVF, unsigned short VertexCount);
	virtual ~VertexBufferClass() override;
public:

	const FVFInfoClass& FVF_Info() const { return *fvf_info; }
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
		WriteLockClass(VertexBufferClass* vertex_buffer, int flags=0);
		~WriteLockClass();
		bool Commit();
	};

	class AppendLockClass : public VertexBufferLockClass
	{
	public:
		AppendLockClass(VertexBufferClass* vertex_buffer,unsigned start_index, unsigned index_range);
		~AppendLockClass();
		bool Commit();
	};

	static unsigned Get_Total_Buffer_Count();
	static unsigned Get_Total_Allocated_Vertices();
	static unsigned Get_Total_Allocated_Memory();

protected:
	unsigned							type;
	unsigned short					VertexCount;
	mutable int						engine_refs;
	FVFInfoClass*					fvf_info;
	unsigned int					generation;
	unsigned int					change_base_generation;
	unsigned int					change_offset;
	unsigned int					change_count;
	unsigned int					change_flags;
};



/**
** Dynamic vertex buffer access is a wrapper to a single cycled dynamic vertex
** buffer.
** DynamicVBAccess gains an access to the dynamic vertex buffer and only
** only of these are allowed at any one time.
**
** The dynamic fvf buffers are always of the same type.
**
** NOTE: Dynamic vertex buffers accessors should only be used locally!
**
*/

class DynamicVBAccessClass
{
	friend DX8Wrapper;
	friend SortingRendererClass;

	const FVFInfoClass& FVFInfo;
	unsigned Type;
	unsigned short VertexCount;
	unsigned short VertexBufferOffset;
	VertexBufferClass* VertexBuffer;
//	static VertexFormatXYZNDUV2* _Get_Sorting_Vertex_Array();

	void Allocate_Sorting_Dynamic_Buffer();
	void Allocate_DX8_Dynamic_Buffer();
public:
	// Type parameter can be either BUFFER_TYPE_DYNAMIC_DX8 or BUFFER_TYPE_DYNAMIC_SORTING.

	// Note: Even though the constructor takes fvf as a parameter, currently the
	// only acceptable parameter is "dynamic_fvf_type". Any other type will
	// result to an assert.
	DynamicVBAccessClass(unsigned type,unsigned fvf,unsigned short vertex_count);
	~DynamicVBAccessClass();

	// Access fvf
	const FVFInfoClass& FVF_Info() const { return FVFInfo; }
	unsigned Get_Type() const { return Type; }
	unsigned short Get_Vertex_Count() const { return VertexCount; }
	bool Is_Valid() const;
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	// Validates the exact dynamic range against the neutral resource owner.
	// Sorting buffers remain CPU-side and are exposed through the explicit data
	// accessor below so the native owner can copy them into a deferred buffer.
	bool Acquire_Native_Vertex_Buffer(
		rts::render::GpuHandle *validated) const;
	unsigned int Get_Vertex_Buffer_Offset() const;
	unsigned int Get_Vertex_Stride() const;
	const void *Get_Sorted_Vertex_Data() const;
#endif

	// Call at the end of the execution, or at whatever time you wish to release
	// the recycled dynamic vertex buffer.
	static void _Deinit();
	static void _Reset(bool frame_changed);
	static unsigned short Get_Default_Vertex_Count();	///<current size of dynamic vertex buffer

	// To lock the vertex buffer, create instance of this write class locally.
	// The buffer is automatically unlocked when you exit the scope.
	class WriteLockClass// : public VertexBufferLockClass
	{
		DynamicVBAccessClass* DynamicVBAccess;
		VertexFormatXYZNDUV2 * Vertices;
		bool Locked;
	public:
		WriteLockClass(DynamicVBAccessClass* vb_access);
		~WriteLockClass();
		bool Is_Locked() const { return Locked; }
		bool Commit();

		// Use this function to get a pointer to the first vertex you can write into.
		// If we ever change the format used by DynamicVBAccessClass, then the
		// return type of this function will change and we'll easily find all code
		// using it.
		VertexFormatXYZNDUV2 * Get_Formatted_Vertex_Array();
	};
	friend WriteLockClass;
};

// ----------------------------------------------------------------------------

inline VertexFormatXYZNDUV2 * DynamicVBAccessClass::WriteLockClass::Get_Formatted_Vertex_Array()
{
	if (!Locked || DynamicVBAccess == nullptr || !DynamicVBAccess->Is_Valid())
	{
		return nullptr;
	}
	// assert that the format of the dynamic vertex buffer is still what we think it is.
	WWASSERT(DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF() == dynamic_fvf_type);
	return Vertices;
}

// ----------------------------------------------------------------------------

/**
** DX8VertexBufferClass
** This class wraps a DX8 vertex buffer.  Use the lock objects to modify or append to the vertex buffer.
*/
class DX8VertexBufferClass : public VertexBufferClass
{
	W3DMPO_CODE(DX8VertexBufferClass)
	friend VertexBufferClass::WriteLockClass;
	friend VertexBufferClass::AppendLockClass;
	friend DynamicVBAccessClass::WriteLockClass;
protected:
	virtual ~DX8VertexBufferClass() override;
public:
	enum UsageType {
		USAGE_DEFAULT=0,
		USAGE_DYNAMIC=1,
		USAGE_SOFTWAREPROCESSING=2,
		USAGE_NPATCHES=4
	};

	DX8VertexBufferClass(unsigned FVF, unsigned short VertexCount, UsageType usage=USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3* vertices, const Vector3* normals, const Vector2* tex_coords, unsigned short VertexCount,UsageType usage=USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3* vertices, const Vector3* normals, const Vector4* diffuse, const Vector2* tex_coords, unsigned short VertexCount,UsageType usage=USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3* vertices, const Vector4* diffuse, const Vector2* tex_coords, unsigned short VertexCount,UsageType usage=USAGE_DEFAULT);
	DX8VertexBufferClass(const Vector3* vertices, const Vector2* tex_coords, unsigned short VertexCount,UsageType usage=USAGE_DEFAULT);
	bool Is_Valid() const;
	bool Lock_Buffer(size_t byte_offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Buffer();
	// Compatibility-shaped calls retain the historical error contract while
	// dispatching through the backend-neutral owner on native builds.
	long Lock(unsigned int byte_offset, unsigned int byte_count,
		unsigned char **data, unsigned long flags);
	long Unlock();

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	bool Acquire_Native_Vertex_Buffer(unsigned int stride, unsigned int offset,
		unsigned int start_vertex, unsigned int vertex_count,
		rts::render::GpuHandle *validated) const;
#else
	IDirect3DVertexBuffer8* Get_DX8_Vertex_Buffer() { return VertexBuffer; }
#endif

	bool Copy(const Vector3* loc, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3* loc, const Vector2* uv, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3* loc, const Vector3* norm, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count);
	bool Copy(const Vector3* loc, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count);

protected:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	rts::render::NativeW3DBufferOwner *NativeBuffer;
	bool Lock_Native_Buffer(size_t offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Native_Buffer();
#else
	IDirect3DVertexBuffer8*		VertexBuffer;
#endif

	void Create_Vertex_Buffer(UsageType usage);
};


/**
** SortingVertexBufferClass
** This class acts as a vertex buffer for the vertices that need to be passed to alpha renderer.
*/
class SortingVertexBufferClass : public VertexBufferClass
{
	W3DMPO_CODE(SortingVertexBufferClass)

	friend DX8Wrapper;
	friend SortingRendererClass;
	friend VertexBufferClass::WriteLockClass;
	friend VertexBufferClass::AppendLockClass;
	friend DynamicVBAccessClass::WriteLockClass;

	VertexFormatXYZNDUV2* VertexBuffer;

protected:
	virtual ~SortingVertexBufferClass() override;
public:
	SortingVertexBufferClass(unsigned short VertexCount);
	const VertexFormatXYZNDUV2 *Get_Vertex_Data() const { return VertexBuffer; }
};
