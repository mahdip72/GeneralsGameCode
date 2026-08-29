#ifndef RTS_RENDERER_NATIVEW3DRESOURCES_H
#define RTS_RENDERER_NATIVEW3DRESOURCES_H

#include "Renderer/NativeW3DRenderer.h"

#include <vector>

namespace rts
{
namespace render
{
class NativeW3DResources
{
public:
	explicit NativeW3DResources(unsigned int capacity = 4096);
	~NativeW3DResources();

	RenderResult Bind(NativeW3DRenderer *renderer);
	RenderResult Shutdown();
	RenderResult CreateBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *handle);
	RenderResult CreateTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData, unsigned int initialDataCount,
		GpuHandle *handle);
	bool Destroy(GpuHandle handle);
	bool IsValid(GpuHandle handle) const;

private:
	friend class NativeW3DRenderer;
	NativeW3DResources(const NativeW3DResources &);
	NativeW3DResources &operator=(const NativeW3DResources &);

	struct Slot;
	Slot *Find(GpuHandle handle);
	const Slot *Find(GpuHandle handle) const;
	bool IsVertexRangeValid(GpuHandle handle, unsigned int stride,
		unsigned int offset, unsigned int startVertex,
		unsigned int vertexCount) const;
	bool IsIndexRangeValid(GpuHandle handle, RenderFormat format,
		unsigned int offset, unsigned int startIndex,
		unsigned int indexCount) const;
	bool IsTextureValidOrEmpty(GpuHandle handle) const;
	bool IsBoundTo(const NativeW3DRenderer *renderer) const;
	static bool EnqueueDeferredResources(NativeW3DRenderState *state,
		const std::vector<GpuHandle> &handles);
	static void DestroyDeferredResources(void *context);

	struct Impl;
	Impl *m_impl;
};
}
}

#endif
