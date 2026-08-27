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
 *                 Project Name : WWSaveLoad                                                   *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/wwsaveload/persistfactory.h                  $*
 *                                                                                             *
 *                       Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                     $Modtime:: 5/04/01 8:42p                                               $*
 *                                                                                             *
 *                    $Revision:: 11                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "WWLib/bittype.h"
#include "WWLib/chunkio.h"
#include "WWDebug/wwdebug.h"
#include "saveload.h"
#include "persist.h"
#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include "pointertoken.h"
#endif

/*
** PersistFactoryClass
** Create a PersistFactoryClass for each concrete derived PersistClass.  These
** factories automatically register with the SaveLoadSystem in their constructors
** and should be accessible through the virtual Get_Factory method of any
** derived PersistClass.
*/

class PersistFactoryClass
{
public:

	PersistFactoryClass();
	virtual ~PersistFactoryClass();

	virtual uint32				Chunk_ID() const												= 0;
	virtual PersistClass *	Load(ChunkLoadClass & cload) const	 						= 0;
	virtual void				Save(ChunkSaveClass & csave,PersistClass * obj)	const	= 0;

private:

	PersistFactoryClass * NextFactory;
	friend class SaveLoadSystemClass;
};




/*
** SimplePersistFactoryClass
** This template automates the creation of a PersistFactory for any type of Persist
** object.  Simply instantiate a single static instance of this template with the
** type and chunkid in the .cpp file of your class.
*/
template <class T,int CHUNKID> class SimplePersistFactoryClass : public PersistFactoryClass
{
public:

	virtual uint32				Chunk_ID() const override { return CHUNKID; }
	virtual PersistClass *	Load(ChunkLoadClass & cload) const override;
	virtual void				Save(ChunkSaveClass & csave,PersistClass * obj) const override;

	/*
	** Internal chunk id's
	*/
	enum
	{
		SIMPLEFACTORY_CHUNKID_OBJPOINTER		=	 0x00100100,
		SIMPLEFACTORY_CHUNKID_OBJDATA
	};
};

#if !defined(_MSC_VER) || _MSC_VER >= 1300
namespace PersistFactoryDetail
{

inline bool Open_Expected_Chunk(ChunkLoadClass &cload, uint32 expected_chunk_id)
{
	if (!cload.Open_Chunk())
	{
		return false;
	}
	if (cload.Cur_Chunk_ID() == expected_chunk_id)
	{
		return true;
	}
	cload.Close_Chunk();
	return false;
}

} // namespace PersistFactoryDetail
#endif


template<class T, int CHUNKID> PersistClass *
SimplePersistFactoryClass<T,CHUNKID>::Load(ChunkLoadClass & cload) const
{
	T * new_obj = W3DNEW T;
	T * old_obj = nullptr;

#if defined(_MSC_VER) && _MSC_VER < 1300
	cload.Open_Chunk();
	WWASSERT(cload.Cur_Chunk_ID() == SIMPLEFACTORY_CHUNKID_OBJPOINTER);
	cload.Read(&old_obj,sizeof(T *));
	cload.Close_Chunk();
#else
	if (!PersistFactoryDetail::Open_Expected_Chunk(
		cload, SIMPLEFACTORY_CHUNKID_OBJPOINTER))
	{
		WWASSERT(0);
		delete new_obj;
		return nullptr;
	}
	const uint32 token_size = cload.Cur_Chunk_Length();
	uint8 token_bytes[PERSIST_POINTER_TOKEN_CURRENT_SIZE] = {};
	std::uintptr_t old_token = 0U;
	if (token_size > sizeof(token_bytes) ||
		cload.Read(token_bytes, token_size) != token_size ||
		!Decode_Persist_Pointer_Token(token_bytes, token_size, &old_token))
	{
		WWASSERT(0);
		cload.Close_Chunk();
		delete new_obj;
		return nullptr;
	}
	old_obj = reinterpret_cast<T *>(old_token);
	cload.Close_Chunk();
#endif

#if defined(_MSC_VER) && _MSC_VER < 1300
	cload.Open_Chunk();
	WWASSERT(cload.Cur_Chunk_ID() == SIMPLEFACTORY_CHUNKID_OBJDATA);
#else
	if (!PersistFactoryDetail::Open_Expected_Chunk(
		cload, SIMPLEFACTORY_CHUNKID_OBJDATA))
	{
		WWASSERT(0);
		delete new_obj;
		return nullptr;
	}
#endif
	new_obj->Load(cload);
	cload.Close_Chunk();

	SaveLoadSystemClass::Register_Pointer(old_obj,new_obj);
	return new_obj;
}


template<class T, int CHUNKID> void
SimplePersistFactoryClass<T,CHUNKID>::Save(ChunkSaveClass & csave,PersistClass * obj) const
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	uint32 objptr = (uint32)obj;
	csave.Begin_Chunk(SIMPLEFACTORY_CHUNKID_OBJPOINTER);
	csave.Write(&objptr,sizeof(uint32));
	csave.End_Chunk();
#else
	uint8 token_bytes[PERSIST_POINTER_TOKEN_CURRENT_SIZE] = {};
	const uint32 token_size = Persist_Pointer_Token_Size();
	WWASSERT(Encode_Persist_Pointer_Token(reinterpret_cast<std::uintptr_t>(obj), token_bytes,
		token_size));
	csave.Begin_Chunk(SIMPLEFACTORY_CHUNKID_OBJPOINTER);
	csave.Write(token_bytes, token_size);
	csave.End_Chunk();
#endif

	csave.Begin_Chunk(SIMPLEFACTORY_CHUNKID_OBJDATA);
	obj->Save(csave);
	csave.End_Chunk();
}
