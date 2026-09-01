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
 *                     $Archive:: /Commando/Code/ww3d2/formconv.h                             $*
 *                                                                                             *
 *              Original Author:: Nathaniel Hoffman                                            *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/27/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 3                                                           $*
 *                                                                                             *
 * 06/27/02 KM Z Format support																						*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "ww3dformat.h"
#include <d3d8.h>
#include "Renderer/RendererDevice.h"

/*
** This file is used for conversions between the legacy DX8 format namespace,
** the WW3D logical format namespace, and the backend-neutral renderer format
** namespace.
**
** The D3DFORMAT functions below are intentionally kept as a small DX8
** compatibility adapter.  New renderer code should consume the descriptor
** and RenderFormat helpers instead of carrying D3DFORMAT values across a
** renderer boundary.  The descriptor explicitly records when a native upload
** needs a CPU expansion; silently treating every legacy format as a native
** texture is unsafe and can produce black or missing textures.
*/

enum WW3DFormatDescriptorFlags
{
	WW3D_FORMAT_DESCRIPTOR_NONE = 0,
	WW3D_FORMAT_DESCRIPTOR_COMPRESSED = 1 << 0,
	WW3D_FORMAT_DESCRIPTOR_SIGNED_NORMALIZED = 1 << 1,
	WW3D_FORMAT_DESCRIPTOR_RENDER_NATIVE = 1 << 2,
	WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION = 1 << 3
};

struct WW3DFormatDescriptor
{
	WW3DFormat format;
	unsigned int bytesPerPixel;
	unsigned int blockWidth;
	unsigned int blockHeight;
	unsigned int blockBytes;
	unsigned int flags;
	rts::render::RenderFormat renderFormat;
};

/* Returns false for WW3D_FORMAT_UNKNOWN or an out-of-range value. */
bool Try_Get_WW3DFormat_Descriptor(WW3DFormat format,
	WW3DFormatDescriptor *descriptor);

/*
** Resolve a logical format to the neutral renderer upload format.
** requires_cpu_conversion is true when the source bytes must be expanded or
** decoded before uploading (for example R5G6B5 or DXT textures); pass null
** when that detail is not needed.  A false return means there is no safe
** upload representation.
*/
bool Try_WW3DFormat_To_RenderFormat(WW3DFormat format,
	rts::render::RenderFormat *renderFormat, bool *requiresCpuConversion);

/* DX8-only adapter for callers that still own a D3DFORMAT value. */
bool Try_D3DFormat_To_RenderFormat(D3DFORMAT format,
	rts::render::RenderFormat *renderFormat, bool *requiresCpuConversion);

D3DFORMAT WW3DFormat_To_D3DFormat(WW3DFormat ww3d_format);
WW3DFormat D3DFormat_To_WW3DFormat(D3DFORMAT d3d_format);

D3DFORMAT WW3DZFormat_To_D3DFormat(WW3DZFormat ww3d_zformat);
WW3DZFormat D3DFormat_To_WW3DZFormat(D3DFORMAT d3d_format);

void Init_D3D_To_WW3_Conversion();
