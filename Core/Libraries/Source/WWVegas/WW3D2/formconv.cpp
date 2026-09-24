/*
**  Command & Conquer Generals Zero Hour(tm)
**  Neutral logical-to-render format descriptors.
*/

#include "formconv.h"

/*
** This table is the single native upload contract.  Packed and compressed
** source data is decoded by the CPU upload path into BGRA8; U8V8 remains a
** signed-normalized native upload.  Keeping this decision explicit prevents
** the loader from passing an unsupported source layout to the device.
*/
static const WW3DFormatDescriptor WW3DFormatDescriptorTable[WW3D_FORMAT_COUNT] =
{
	{ WW3D_FORMAT_UNKNOWN, 0, 0, 0, 0,
		WW3D_FORMAT_DESCRIPTOR_NONE, rts::render::RENDER_FORMAT_UNKNOWN },
	{ WW3D_FORMAT_R8G8B8, 3, 1, 1, 3,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A8R8G8B8, 4, 1, 1, 4,
		WW3D_FORMAT_DESCRIPTOR_RENDER_NATIVE,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_X8R8G8B8, 4, 1, 1, 4,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_R5G6B5, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_X1R5G5B5, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A1R5G5B5, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A4R4G4B4, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_R3G3B2, 1, 1, 1, 1,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A8, 1, 1, 1, 1,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A8R3G3B2, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_X4R4G4B4, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A8P8, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_P8, 1, 1, 1, 1,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_L8, 1, 1, 1, 1,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A8L8, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_A4L4, 1, 1, 1, 1,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_U8V8, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_SIGNED_NORMALIZED |
		WW3D_FORMAT_DESCRIPTOR_RENDER_NATIVE,
		rts::render::RENDER_FORMAT_R8G8_SNORM },
	{ WW3D_FORMAT_L6V5U5, 2, 1, 1, 2,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_X8L8V8U8, 4, 1, 1, 4,
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_DXT1, 0, 4, 4, 8,
		WW3D_FORMAT_DESCRIPTOR_COMPRESSED |
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_DXT2, 0, 4, 4, 16,
		WW3D_FORMAT_DESCRIPTOR_COMPRESSED |
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_DXT3, 0, 4, 4, 16,
		WW3D_FORMAT_DESCRIPTOR_COMPRESSED |
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_DXT4, 0, 4, 4, 16,
		WW3D_FORMAT_DESCRIPTOR_COMPRESSED |
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM },
	{ WW3D_FORMAT_DXT5, 0, 4, 4, 16,
		WW3D_FORMAT_DESCRIPTOR_COMPRESSED |
		WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION,
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM }
};

bool Try_Get_WW3DFormat_Descriptor(WW3DFormat format,
	WW3DFormatDescriptor *descriptor)
{
	if (descriptor == 0)
	{
		return false;
	}
	*descriptor = WW3DFormatDescriptorTable[0];
	if (format <= WW3D_FORMAT_UNKNOWN || format >= WW3D_FORMAT_COUNT)
	{
		return false;
	}
	*descriptor = WW3DFormatDescriptorTable[(unsigned int)format];
	return true;
}

bool Try_WW3DFormat_To_RenderFormat(WW3DFormat format,
	rts::render::RenderFormat *renderFormat, bool *requiresCpuConversion)
{
	WW3DFormatDescriptor descriptor;
	if (renderFormat == 0 ||
		!Try_Get_WW3DFormat_Descriptor(format, &descriptor))
	{
		if (renderFormat != 0)
		{
			*renderFormat = rts::render::RENDER_FORMAT_UNKNOWN;
		}
		if (requiresCpuConversion != 0)
		{
			*requiresCpuConversion = false;
		}
		return false;
	}
	*renderFormat = descriptor.renderFormat;
	if (requiresCpuConversion != 0)
	{
		*requiresCpuConversion =
			(descriptor.flags & WW3D_FORMAT_DESCRIPTOR_REQUIRES_CPU_CONVERSION) != 0;
	}
	return descriptor.renderFormat != rts::render::RENDER_FORMAT_UNKNOWN;
}

void Init_D3D_To_WW3_Conversion()
{
	/* The descriptor table is immutable and needs no device initialization. */
}
