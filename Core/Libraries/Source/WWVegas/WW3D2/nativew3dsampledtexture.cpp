#include "nativew3dsampledtexture.h"

#if !defined(_MSC_VER) || _MSC_VER >= 1900

#include "surfaceblit.h"

#include <limits.h>
#include <string.h>

namespace rts
{
namespace render
{
namespace
{
const size_t MAX_SAMPLED_TEXTURE_UPLOAD_BYTES = 256U * 1024U * 1024U;

bool CheckedMultiply(size_t left, size_t right, size_t *product)
{
	if (product == 0 || (left != 0 && right > static_cast<size_t>(-1) / left))
	{
		return false;
	}
	*product = left * right;
	return true;
}

bool CheckedAdd(size_t left, size_t right, size_t *sum)
{
	if (sum == 0 || right > static_cast<size_t>(-1) - left)
	{
		return false;
	}
	*sum = left + right;
	return true;
}

bool IsDxt(WW3DFormat format)
{
	return format == WW3D_FORMAT_DXT1 || format == WW3D_FORMAT_DXT2 ||
		format == WW3D_FORMAT_DXT3 || format == WW3D_FORMAT_DXT4 ||
		format == WW3D_FORMAT_DXT5;
}

bool IsExplicitLegacyFormat(WW3DFormat format)
{
	// These bump layouts have no proven color interpretation in the neutral
	// renderer and remain on the explicit legacy path.
	return format == WW3D_FORMAT_L6V5U5 || format == WW3D_FORMAT_X8L8V8U8;
}

bool IsSupportedColorFormat(WW3DFormat format)
{
	switch (format)
	{
	case WW3D_FORMAT_R8G8B8:
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R5G6B5:
	case WW3D_FORMAT_X1R5G5B5:
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_R3G3B2:
	case WW3D_FORMAT_A8:
	case WW3D_FORMAT_A8R3G3B2:
	case WW3D_FORMAT_X4R4G4B4:
	case WW3D_FORMAT_L8:
	case WW3D_FORMAT_A8L8:
	case WW3D_FORMAT_A4L4:
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		return true;
	default:
		return false;
	}
}

unsigned int MaximumMipCount(unsigned int width, unsigned int height)
{
	unsigned int dimension = width > height ? width : height;
	unsigned int count = 1;
	while (dimension > 1)
	{
		dimension >>= 1;
		++count;
	}
	return count;
}

bool ValidateSourceView(const NativeW3DSampledTextureMipView &source,
	WW3DFormat format, unsigned int width, unsigned int height)
{
	if (source.data == 0 || source.rowPitch == 0 ||
		source.rowPitch > static_cast<size_t>(INT_MAX))
	{
		return false;
	}
	const size_t rows = IsDxt(format) ?
		static_cast<size_t>((height + 3U) / 4U) : height;
	size_t requiredBytes = 0;
	return CheckedMultiply(source.rowPitch, rows, &requiredBytes) &&
		requiredBytes <= source.dataSize;
}

bool CopySignedBump(const NativeW3DSampledTextureMipView &source,
	unsigned int width, unsigned int height,
	std::vector<unsigned char> *pixels)
{
	size_t rowBytes = 0;
	size_t totalBytes = 0;
	if (pixels == 0 || !CheckedMultiply(width, 2U, &rowBytes) ||
		source.rowPitch < rowBytes ||
		!CheckedMultiply(rowBytes, height, &totalBytes))
	{
		return false;
	}
	try
	{
		pixels->resize(totalBytes);
	}
	catch (...)
	{
		return false;
	}
	for (unsigned int row = 0; row < height; ++row)
	{
		memcpy(&(*pixels)[static_cast<size_t>(row) * rowBytes],
			source.data + static_cast<size_t>(row) * source.rowPitch,
			rowBytes);
	}
	return true;
}
}

NativeW3DSampledTextureMipView::NativeW3DSampledTextureMipView() :
	data(0), dataSize(0), rowPitch(0)
{
}

NativeW3DSampledTextureUpload::NativeW3DSampledTextureUpload() :
	m_descriptor(), m_pixels(), m_subresources()
{
}

bool NativeW3DSampledTextureUpload::SupportsSourceFormat(
	WW3DFormat sourceFormat)
{
	return sourceFormat == WW3D_FORMAT_U8V8 ||
		IsSupportedColorFormat(sourceFormat);
}

bool NativeW3DSampledTextureUpload::Prepare(WW3DFormat sourceFormat,
	unsigned int width, unsigned int height, unsigned int mipCount,
	unsigned int arrayCount,
	const NativeW3DSampledTextureMipView *sourceMips,
	unsigned int sourceMipCount)
{
	Reset();
	if (width == 0 || height == 0 || mipCount == 0 ||
		(arrayCount != 1 && arrayCount != 6) ||
		(arrayCount == 6 && width != height) ||
		mipCount > MaximumMipCount(width, height) ||
		mipCount > UINT_MAX / arrayCount || sourceMips == 0 ||
		sourceMipCount != mipCount * arrayCount ||
		IsExplicitLegacyFormat(sourceFormat) ||
		!SupportsSourceFormat(sourceFormat))
	{
		return false;
	}

	const bool signedBump = sourceFormat == WW3D_FORMAT_U8V8;
	const unsigned int outputBytesPerPixel = signedBump ? 2U : 4U;
	size_t totalBytes = 0;
	for (unsigned int mip = 0; mip < mipCount; ++mip)
	{
		const size_t mipWidth = (width >> mip) == 0 ? 1U : width >> mip;
		const size_t mipHeight = (height >> mip) == 0 ? 1U : height >> mip;
		size_t mipBytes = 0;
		if (!CheckedMultiply(mipWidth, mipHeight, &mipBytes) ||
			!CheckedMultiply(mipBytes, outputBytesPerPixel, &mipBytes) ||
			!CheckedMultiply(mipBytes, arrayCount, &mipBytes) ||
			!CheckedAdd(totalBytes, mipBytes, &totalBytes) ||
			totalBytes > MAX_SAMPLED_TEXTURE_UPLOAD_BYTES)
		{
			return false;
		}
	}

	try
	{
		m_pixels.resize(sourceMipCount);
		m_subresources.resize(sourceMipCount);
	}
	catch (...)
	{
		Reset();
		return false;
	}

	for (unsigned int arraySlice = 0; arraySlice < arrayCount; ++arraySlice)
	{
		for (unsigned int mip = 0; mip < mipCount; ++mip)
		{
			const unsigned int index = arraySlice * mipCount + mip;
			const unsigned int mipWidth = (width >> mip) == 0 ? 1U : width >> mip;
			const unsigned int mipHeight = (height >> mip) == 0 ? 1U : height >> mip;
			const NativeW3DSampledTextureMipView &source = sourceMips[index];
			if (!ValidateSourceView(source, sourceFormat, mipWidth, mipHeight))
			{
				Reset();
				return false;
			}
			const bool converted = signedBump ?
				CopySignedBump(source, mipWidth, mipHeight, &m_pixels[index]) :
				SurfaceBlit_Convert_To_A8R8G8B8(source.data,
					static_cast<int>(source.rowPitch), mipWidth, mipHeight,
					sourceFormat, &m_pixels[index]);
			if (!converted || m_pixels[index].empty())
			{
				Reset();
				return false;
			}
			m_subresources[index].data = &m_pixels[index][0];
			m_subresources[index].rowPitch =
				static_cast<size_t>(mipWidth) * outputBytesPerPixel;
			m_subresources[index].slicePitch = m_pixels[index].size();
		}
	}

	m_descriptor.width = width;
	m_descriptor.height = height;
	m_descriptor.mipCount = mipCount;
	m_descriptor.arrayCount = arrayCount;
	m_descriptor.dimension = arrayCount == 6 ?
		RENDER_TEXTURE_CUBE : RENDER_TEXTURE_2D;
	m_descriptor.format = signedBump ?
		RENDER_FORMAT_R8G8_SNORM : RENDER_FORMAT_B8G8R8A8_UNORM;
	m_descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	m_descriptor.usage = RENDER_USAGE_IMMUTABLE;
	return true;
}

void NativeW3DSampledTextureUpload::Reset()
{
	m_descriptor = TextureDescriptor();
	m_pixels.clear();
	m_subresources.clear();
}

const TextureDescriptor &NativeW3DSampledTextureUpload::Descriptor() const
{
	return m_descriptor;
}

const TextureSubresourceData *NativeW3DSampledTextureUpload::Subresources() const
{
	return m_subresources.empty() ? 0 : &m_subresources[0];
}

unsigned int NativeW3DSampledTextureUpload::SubresourceCount() const
{
	return static_cast<unsigned int>(m_subresources.size());
}

}
}

#endif
