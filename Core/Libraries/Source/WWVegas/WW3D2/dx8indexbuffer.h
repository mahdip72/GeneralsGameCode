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
 *                     $Archive:: /Commando/Code/ww3d2/dx8indexbuffer.h                       $*
 *                                                                                             *
 *              Original Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                      $Author:: Jani_p                                                      $*
 *                                                                                             *
 *                     $Modtime:: 7/10/01 12:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 12                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "WWDebug/wwdebug.h"
#include "WWMath/sphere.h"

class DX8Wrapper;
class SortingRendererClass;
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
namespace rts { namespace render { class GpuHandle; class NativeW3DBufferOwner; } }
#else
struct IDirect3DIndexBuffer8;
#endif
class DX8IndexBufferClass;
class SortingIndexBufferClass;

// ----------------------------------------------------------------------------

class IndexBufferClass : public RefCountClass
{
protected:
	virtual ~IndexBufferClass() override;
public:
	IndexBufferClass(unsigned type, unsigned short index_count);

	bool Copy(unsigned int* indices,unsigned start_index,unsigned index_count);
	bool Copy(unsigned short* indices,unsigned start_index,unsigned index_count);

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
		IndexBufferClass* index_buffer;
		unsigned short* indices;
		bool locked;
	public:
		WriteLockClass(IndexBufferClass* index_buffer, int flags=0);
		~WriteLockClass();

		unsigned short* Get_Index_Array() { return indices; }
		bool Is_Locked() const { return locked; }
		bool Commit();
	};

	class AppendLockClass
	{
		IndexBufferClass* index_buffer;
		unsigned short* indices;
		bool locked;
	public:
		AppendLockClass(IndexBufferClass* index_buffer,unsigned start_index, unsigned index_range);
		~AppendLockClass();

		unsigned short* Get_Index_Array() { return indices; }
		bool Is_Locked() const { return locked; }
		bool Commit();
	};

	static unsigned Get_Total_Buffer_Count();
	static unsigned Get_Total_Allocated_Indices();
	static unsigned Get_Total_Allocated_Memory();

protected:
	mutable int					engine_refs;
	unsigned short				index_count;		// number of indices
	unsigned						type;
	unsigned int					generation;
	unsigned int					change_base_generation;
	unsigned int					change_offset;
	unsigned int					change_count;
	unsigned int					change_flags;
};


// HY 2/14/01
// Created
class DynamicIBAccessClass
{
	W3DMPO_CODE(DynamicIBAccessClass)

	friend DX8Wrapper;
	friend SortingRendererClass;

	unsigned Type;
	unsigned short IndexCount;
	unsigned short IndexBufferOffset;
	IndexBufferClass* IndexBuffer;

	void Allocate_Sorting_Dynamic_Buffer();
	void Allocate_DX8_Dynamic_Buffer();

public:
	DynamicIBAccessClass(unsigned short type, unsigned short index_count);
	~DynamicIBAccessClass();

	unsigned Get_Type() const { return Type; }
	unsigned short Get_Index_Count() const { return IndexCount; }
	bool Is_Valid() const;

	// Call at the end of the execution, or at whatever time you wish to release
	// the recycled dynamic index buffer.
	static void _Deinit();
	static void _Reset(bool frame_changed);
	static unsigned short Get_Default_Index_Count();	///<current size of dynamic index buffer

	// To lock the index buffer, create instance of this write class locally.
	// The buffer is automatically unlocked when you exit the scope.
	class WriteLockClass
	{
		DynamicIBAccessClass* DynamicIBAccess;
		unsigned short* Indices;
		bool Locked;
		bool Referenced;
	public:
		WriteLockClass(DynamicIBAccessClass* ib_access);
		~WriteLockClass();
		unsigned short* Get_Index_Array() { return Indices; }
		bool Is_Locked() const { return Locked; }
		bool Commit();
	};

	friend WriteLockClass;
};


/**
** DX8IndexBufferClass
** This class wraps a DX8 index buffer.
*/
class DX8IndexBufferClass : public IndexBufferClass
{
	W3DMPO_CODE(DX8IndexBufferClass)

	friend IndexBufferClass::WriteLockClass;
	friend IndexBufferClass::AppendLockClass;
	friend DynamicIBAccessClass::WriteLockClass;
public:
	enum UsageType {
		USAGE_DEFAULT=0,
		USAGE_DYNAMIC=1,
		USAGE_SOFTWAREPROCESSING=2,
		USAGE_NPATCHES=4
	};

	DX8IndexBufferClass(unsigned short index_count,UsageType usage=USAGE_DEFAULT);
	virtual ~DX8IndexBufferClass() override;
	bool Is_Valid() const;
	bool Lock_Buffer(size_t byte_offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Buffer();
	long Lock(unsigned int byte_offset, unsigned int byte_count,
		unsigned char **data, unsigned long flags);
	long Unlock();

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	bool Acquire_Native_Index_Buffer(unsigned int offset,
		unsigned int start_index, unsigned int index_count,
		rts::render::GpuHandle *validated) const;
#else
	IDirect3DIndexBuffer8* Get_DX8_Index_Buffer()	{ return index_buffer; }
#endif

private:
#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)
	rts::render::NativeW3DBufferOwner *native_buffer;
	bool Lock_Native_Buffer(size_t offset, size_t byte_count, int flags,
		void **data);
	bool Unlock_Native_Buffer();
#else
	IDirect3DIndexBuffer8*	index_buffer;		// actual dx8 index buffer
#endif
};



class SortingIndexBufferClass : public IndexBufferClass
{
	W3DMPO_CODE(SortingIndexBufferClass)

	friend DX8Wrapper;
	friend SortingRendererClass;
	friend IndexBufferClass::WriteLockClass;
	friend IndexBufferClass::AppendLockClass;
	friend DynamicIBAccessClass::WriteLockClass;
public:
	SortingIndexBufferClass(unsigned short index_count);
	virtual ~SortingIndexBufferClass() override;

protected:
	unsigned short* index_buffer;
};
