#ifndef RTS_WW3D2_NATIVEW3DSAMPLEDTEXTURE_H
#define RTS_WW3D2_NATIVEW3DSAMPLEDTEXTURE_H

// The sampled-texture upload is an x64 migration boundary. Keep neutral
// renderer containers and types out of VC6 while the legacy product continues
// to compile the surrounding loader.
#if !defined(_MSC_VER) || _MSC_VER >= 1900

#include "Utility/CppMacros.h"
#include "Renderer/RendererDevice.h"
#include "ww3dformat.h"

#include <stddef.h>
#include <vector>

namespace rts
{
namespace render
{

// A non-owning view of one Stage4-prepared texture mip. Views are ordered by
// array slice, then mip level, matching the neutral renderer subresource order.
struct NativeW3DSampledTextureMipView
{
	NativeW3DSampledTextureMipView();

	const unsigned char *data;
	size_t dataSize;
	size_t rowPitch;
};

// Owns the converted CPU bytes for one immutable sampled 2D or cube texture.
// The returned TextureSubresourceData pointers remain valid until Reset or the
// next Prepare call. No legacy surface or COM object is retained or exposed.
// DXT2/DXT4 inputs retain their stored premultiplied RGB values when expanded.
class NativeW3DSampledTextureUpload
{
public:
	NativeW3DSampledTextureUpload();

	static bool SupportsSourceFormat(WW3DFormat sourceFormat);

	bool Prepare(WW3DFormat sourceFormat, unsigned int width,
		unsigned int height, unsigned int mipCount, unsigned int arrayCount,
		const NativeW3DSampledTextureMipView *sourceMips,
		unsigned int sourceMipCount);
	void Reset();

	const TextureDescriptor &Descriptor() const;
	const TextureSubresourceData *Subresources() const;
	unsigned int SubresourceCount() const;

private:
	NativeW3DSampledTextureUpload(const NativeW3DSampledTextureUpload &);
	NativeW3DSampledTextureUpload &operator=(
		const NativeW3DSampledTextureUpload &);

	TextureDescriptor m_descriptor;
	std::vector<std::vector<unsigned char> > m_pixels;
	std::vector<TextureSubresourceData> m_subresources;
};

}
}

#endif

#endif
