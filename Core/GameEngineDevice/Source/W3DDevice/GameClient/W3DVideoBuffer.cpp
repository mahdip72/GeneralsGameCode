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
#include "Renderer/RendererDevice.h"
#include "WW3D2/texture.h"
#include "WW3D2/textureloader.h"
#include "Renderer/RenderTexturePublication.h"
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
	m_nativePublicationPath(FALSE),
	m_nativePublicationPending(FALSE)
{

}

//============================================================================
// W3DVideoBuffer::UsesNativeD3D11PublicationPath
//============================================================================

Bool W3DVideoBuffer::UsesNativeD3D11PublicationPath()
{
#if defined(_WIN64)
	const rts::render::RenderBackend backend =
		rts::render::RequestedRenderBackend();
	return backend == rts::render::RENDER_BACKEND_D3D11 &&
		rts::render::IsRenderBackendSupported(backend) &&
		rts::render::IsNativeD3D11PublicationActive() ? TRUE : FALSE;
#else
	return FALSE;
#endif
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
	m_nativePublicationPath = UsesNativeD3D11PublicationPath();
	if (m_nativePublicationPath)
	{
		// The native lock surface exposes canonical BGRA8 bytes. Refuse legacy
		// packed decoder layouts instead of allowing them to partially overwrite
		// a four-byte native row; the video player can select its deterministic
		// unsupported-format fallback.
		if (m_format != TYPE_X8R8G8B8)
		{
			return FALSE;
		}
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
		if (!m_surfaceLocked)
		{
			if (m_nativePublicationPath)
			{
				// A surface lock can fail while the device is lost even though
				// Get_Surface_Level returned a wrapper. Keep the frame visibly
				// invalid until a later lock/publication retry succeeds.
				m_nativePublicationPending = TRUE;
			}
			m_surface->Release_Ref();
			m_surface = nullptr;
			m_lockedMemory = nullptr;
		}
	}
	else if (m_nativePublicationPath)
	{
		// A lost/invalid native surface must not be reported as a clean frame;
		// the retained TextureClass CPU shadow will be retried on a later lock.
		m_nativePublicationPending = TRUE;
	}

	return mem;
}

//============================================================================
// W3DVideoBuffer::unlock
//============================================================================

void		W3DVideoBuffer::unlock()
{
	if ( m_surface != nullptr )
	{
		if (m_surfaceLocked)
		{
			Bool publicationSucceeded = TRUE;
#if defined(_WIN64)
			if (m_nativePublicationPath)
			{
				publicationSucceeded = m_surface->Unlock_Native_Surface() ?
					TRUE : FALSE;
			}
			else
#endif
			{
				m_surface->Unlock();
			}
			if (m_nativePublicationPath)
			{
				m_nativePublicationPending = publicationSucceeded ? FALSE : TRUE;
			}
		}
		m_surface->Release_Ref();
		m_surface = nullptr;
		m_surfaceLocked = FALSE;
		m_lockedMemory = nullptr;
		// Native SurfaceClass::Unlock owns the one upload into the native texture.
		// The legacy path still needs its dirty notification after Unlock; on x64,
		// an inactive requested backend retains the previous defensive notification.
		if (!m_nativePublicationPath && m_texture != nullptr)
		{
			rts::render::NotifyTextureChanged(m_texture);
		}
	}
}

//============================================================================
// W3DVideoBuffer::valid
//============================================================================

Bool		W3DVideoBuffer::valid()
{
	return m_texture != nullptr && !m_nativePublicationPending;
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
	m_nativePublicationPath = FALSE;
	m_nativePublicationPending = FALSE;

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
