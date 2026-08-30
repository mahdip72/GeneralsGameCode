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
// Project:   Generals
//
// Module:    Video
//
// File name: W3DDevice/GameClient/W3DVideoBuffer.cpp
//
// Created:   10/23/01 TR
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "Common/GameMemory.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/texture.h"
#include "WW3D2/textureloader.h"
#include "W3DDevice/GameClient/W3DVideoBuffer.h"

//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------


//============================================================================
// W3DVideoBuffer::W3DVideoBuffer
//============================================================================

W3DVideoBuffer::W3DVideoBuffer( VideoBuffer::Type format )
: VideoBuffer(format),
	m_texture(nullptr),
	m_surface(nullptr),
	m_surfaceLocked(FALSE),
	m_lockedMemory(nullptr),
	m_framePublished(FALSE)
{

}


//============================================================================
// W3DVideoBuffer::SetBuffer
//============================================================================

Bool W3DVideoBuffer::allocate( UnsignedInt width, UnsignedInt height )
{
	free();

	m_width = width;
	m_height = height;
	m_textureWidth = width;
	m_textureHeight = height;
	if (DX8Wrapper::Is_D3D11_Backend_Active())
	{
		// Preserve native 4K dimensions without accepting malformed movies that
		// exceed the bridge's bounded full-level publication contract.
		const UnsignedInt max_texture_dimension = 16384;
		const size_t max_texture_bytes = 256U * 1024U * 1024U;
		if (m_textureWidth == 0 || m_textureHeight == 0 ||
			m_textureWidth > max_texture_dimension ||
			m_textureHeight > max_texture_dimension ||
			static_cast<size_t>(m_textureWidth) > max_texture_bytes / 4U /
				static_cast<size_t>(m_textureHeight))
		{
			return FALSE;
		}
	}
	else
	{
		unsigned int temp_depth=1;
		TextureLoader::Validate_Texture_Size( m_textureWidth, m_textureHeight, temp_depth);
	}

	WW3DFormat w3dFormat = TypeToW3DFormat(  m_format );

	if ( w3dFormat == WW3D_FORMAT_UNKNOWN )
	{
		return FALSE;
	}

	m_texture  = MSGNEW("TextureClass") TextureClass ( m_textureWidth, m_textureHeight, w3dFormat, MIP_LEVELS_1 );

	if ( m_texture == nullptr )
	{
		return FALSE;
	}

	if ( lock() == nullptr )
	{
		free();
		return FALSE;
	}

	unlock();


	return TRUE;
}

//============================================================================
// W3DVideoBuffer::~W3DVideoBuffer
//============================================================================

W3DVideoBuffer::~W3DVideoBuffer()
{
	free();
}

//============================================================================
// W3DVideoBuffer::lock
//============================================================================

void*		W3DVideoBuffer::lock()
{
	void *mem = nullptr;

	if ( m_texture == nullptr )
	{
		return nullptr;
	}

	if ( m_surface != nullptr )
	{
		unlock();
	}

	m_surface = m_texture->Get_Surface_Level();

	if ( m_surface )
	{
		mem = m_surface->Lock( (Int*) &m_pitch );
		m_surfaceLocked = mem != nullptr;
		m_lockedMemory = mem;
		m_framePublished = FALSE;
		if (!m_surfaceLocked)
		{
			m_surface->Release_Ref();
			m_surface = nullptr;
			m_lockedMemory = nullptr;
		}
	}

	return mem;
}

//============================================================================
// W3DVideoBuffer::publishLockedFrame
//============================================================================

Bool W3DVideoBuffer::publishLockedFrame()
{
	size_t slice_pitch = 0;
	if (!m_surfaceLocked || m_lockedMemory == nullptr || m_texture == nullptr ||
		m_format != TYPE_X8R8G8B8 || m_xPos != 0 || m_yPos != 0 ||
		m_width != m_textureWidth || m_height != m_textureHeight ||
		!ComputeDirectBGRA8SlicePitch(m_format, m_textureWidth,
			m_textureHeight, m_pitch, &slice_pitch))
	{
		return FALSE;
	}

	const size_t row_pitch = static_cast<size_t>(m_pitch);

	m_framePublished = Publish_Render_Texture_BGRA8_Change(
		m_texture->Peek_D3D_Base_Texture(), m_lockedMemory, row_pitch,
		slice_pitch) ? TRUE : FALSE;
	return m_framePublished;
}

//============================================================================
// W3DVideoBuffer::unlock
//============================================================================

void		W3DVideoBuffer::unlock()
{
	if ( m_surface != nullptr )
	{
		const Bool frame_published = m_framePublished;
		if (m_surfaceLocked)
		{
			m_surface->Unlock();
		}
		m_surface->Release_Ref();
		m_surface = nullptr;
		m_surfaceLocked = FALSE;
		m_lockedMemory = nullptr;
		m_framePublished = FALSE;
		// Decoded frames update the same managed legacy texture in place. Discard
		// the D3D11 conversion unless the locked BGRA8 contents were published
		// directly. Other formats, backends, and publication failures retain the
		// conservative D3D8/Bink path.
		if ( !frame_published && m_texture != nullptr )
		{
			Notify_Render_Texture_Changed(
				m_texture->Peek_D3D_Base_Texture());
		}
	}
}

//============================================================================
// W3DVideoBuffer::valid
//============================================================================

Bool		W3DVideoBuffer::valid()
{
	return m_texture != nullptr;
}

//============================================================================
// W3DVideoBuffer::reset
//============================================================================

void	W3DVideoBuffer::free()
{
	unlock();

	if ( m_texture )
	{
		unlock();
		m_texture->Release_Ref();
		m_texture = nullptr;
	}
	m_surface = nullptr;

	VideoBuffer::free();
}


//============================================================================
// W3DVideoBuffer::TypeToW3DFormat
//============================================================================

WW3DFormat W3DVideoBuffer::TypeToW3DFormat( VideoBuffer::Type format )
{
	WW3DFormat w3dFormat = WW3D_FORMAT_UNKNOWN;
	switch ( format )
	{
		case TYPE_X8R8G8B8:
			w3dFormat = WW3D_FORMAT_X8R8G8B8;
			break;

 		case TYPE_R8G8B8:
			w3dFormat = WW3D_FORMAT_R8G8B8;
			break;

 		case TYPE_R5G6B5:
			w3dFormat = WW3D_FORMAT_R5G6B5;
			break;

 		case TYPE_X1R5G5B5:
			w3dFormat = WW3D_FORMAT_X1R5G5B5;
			break;
	}

	return w3dFormat;
}

//============================================================================
// W3DFormatToType
//============================================================================

VideoBuffer::Type W3DVideoBuffer::W3DFormatToType( WW3DFormat w3dFormat )
{
	Type format = TYPE_UNKNOWN;
	switch ( w3dFormat )
	{
		case WW3D_FORMAT_X8R8G8B8:
				format = VideoBuffer::TYPE_X8R8G8B8;
				break;
		case WW3D_FORMAT_R8G8B8:
				format = VideoBuffer::TYPE_R8G8B8;
				break;
		case WW3D_FORMAT_R5G6B5:
				format = VideoBuffer::TYPE_R5G6B5;
				break;
		case WW3D_FORMAT_X1R5G5B5:
				format = VideoBuffer::TYPE_X1R5G5B5;
				break;
	}

	return format;
}
