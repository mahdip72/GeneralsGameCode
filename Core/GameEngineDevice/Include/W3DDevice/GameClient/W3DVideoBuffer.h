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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:    Generals
//
// File name:  W3DDevice/GameClient/W3DVideoBuffer.h
//
// Created:    10/23/01
//
//----------------------------------------------------------------------------

#pragma once

#include <stddef.h>

//----------------------------------------------------------------------------
//           Includes
//----------------------------------------------------------------------------

#include "GameClient/VideoPlayer.h"
#include "WW3D2/ww3dformat.h"

//----------------------------------------------------------------------------
//           Forward References
//----------------------------------------------------------------------------

class TextureClass;
class SurfaceClass;

//----------------------------------------------------------------------------
//           Type Defines
//----------------------------------------------------------------------------

//===============================
// W3DVideoBuffer
//===============================
/**
  * Video buffer interface class to a W3D TextureClass
	*/
//===============================


class W3DVideoBuffer : public VideoBuffer
{
	protected:

		TextureClass	*m_texture;
		SurfaceClass	*m_surface;
		Bool m_surfaceLocked;
		void *m_lockedMemory;
		Bool m_nativePublicationPath;
		// A failed native unlock is a visible poisoned frame, but the texture's
		// retained CPU shadow remains available for the next lock/retry.
		Bool m_nativePublicationPending;

	public:

		W3DVideoBuffer( Type format );
		virtual ~W3DVideoBuffer() override;

		// Select the native publication contract once per allocation. This keeps
		// a backend transition from changing the interpretation of a locked
		// legacy surface.
		static Bool UsesNativeD3D11PublicationPath();

		virtual	Bool		allocate( UnsignedInt width, UnsignedInt height) override; ///< Allocates buffer
		virtual void		free() override;					///< Free buffer
		virtual	void*		lock() override;					///< Returns memory pointer to start of buffer
		static Bool		ComputeDirectBGRA8SlicePitch(Type format,
			UnsignedInt width, UnsignedInt height, UnsignedInt pitch,
			size_t *slice_pitch);
		virtual void		unlock() override;				///< Release buffer
		virtual Bool		valid() override;				///< Is the buffer valid to use

		TextureClass		*texture();			///< Returns texture object

		static WW3DFormat TypeToW3DFormat( VideoBuffer::Type format );
		static VideoBuffer::Type W3DFormatToType( WW3DFormat w3dFormat );
};


//----------------------------------------------------------------------------
//           Inlining
//----------------------------------------------------------------------------

inline TextureClass* W3DVideoBuffer::texture() { return m_texture; }

inline Bool W3DVideoBuffer::ComputeDirectBGRA8SlicePitch(
	Type format, UnsignedInt width, UnsignedInt height, UnsignedInt pitch,
	size_t *slice_pitch)
{
	if (slice_pitch == 0 || format != TYPE_X8R8G8B8 || width == 0 ||
		height == 0)
	{
		return FALSE;
	}

	const size_t width_size = static_cast<size_t>(width);
	const size_t row_pitch = static_cast<size_t>(pitch);
	const size_t maximum_size = static_cast<size_t>(-1);
	if (width_size > maximum_size / 4U)
	{
		return FALSE;
	}
	const size_t minimum_row_pitch = width_size * 4U;
	if (row_pitch < minimum_row_pitch ||
		row_pitch > maximum_size / static_cast<size_t>(height))
	{
		return FALSE;
	}

	*slice_pitch = row_pitch * static_cast<size_t>(height);
	return TRUE;
}
