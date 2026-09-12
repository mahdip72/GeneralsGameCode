/*
**  Command & Conquer Generals Zero Hour(tm)
**  Neutral WW3D format publication contract.
**
**  The logical WW3D format is deliberately independent from any graphics
**  API.  The legacy API conversion declarations live in the x86-only
**  Core/LegacyRenderer adapter and must not leak into native renderer code.
*/

#pragma once

#include "ww3dformat.h"
#include "Renderer/RendererDevice.h"

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

/* Returns false for UNKNOWN and for values outside the logical format table. */
bool Try_Get_WW3DFormat_Descriptor(WW3DFormat format,
	WW3DFormatDescriptor *descriptor);

/*
** Resolve a logical format to a backend-neutral upload format.  The boolean
** output tells the caller whether source bytes need CPU expansion/decoding.
*/
bool Try_WW3DFormat_To_RenderFormat(WW3DFormat format,
	rts::render::RenderFormat *renderFormat, bool *requiresCpuConversion);

/* Kept as a source-compatible initialization hook; the neutral table is static. */
void Init_D3D_To_WW3_Conversion();
