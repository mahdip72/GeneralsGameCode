#ifndef RTS_WW3D2_NATIVEW3DBUFFEROWNER_H
#define RTS_WW3D2_NATIVEW3DBUFFEROWNER_H

#include "Renderer/NativeW3DResources.h"

namespace rts
{
namespace render
{

// The native x64 compatibility classes borrow the product's one resource
// registry. The bridge owns that registry and must unbind it before backend
// shutdown. No backend object or COM interface crosses this boundary.
RenderResult BindNativeW3DBufferResources(NativeW3DResources *resources);
RenderResult UnbindNativeW3DBufferResources(NativeW3DResources *resources);

class NativeW3DBufferOwner
{
public:
	NativeW3DBufferOwner();
	~NativeW3DBufferOwner();

	RenderResult Create(const BufferDescriptor &descriptor);
	RenderResult Reset();
	RenderResult Lock(size_t destinationOffset, size_t byteCount,
		RenderBufferUpdateMode mode, void **data);
	RenderResult Unlock();
	RenderResult AcquireVertexRange(unsigned int stride, unsigned int offset,
		unsigned int startVertex, unsigned int vertexCount,
		GpuHandle *validated) const;
	RenderResult AcquireIndexRange(RenderFormat format, unsigned int offset,
		unsigned int startIndex, unsigned int indexCount,
		GpuHandle *validated) const;

	bool IsLocked() const;
	bool HasFailedMutation() const;

private:
	NativeW3DBufferOwner(const NativeW3DBufferOwner &);
	NativeW3DBufferOwner &operator=(const NativeW3DBufferOwner &);

	RenderResult RecreateForDiscard();
	NativeW3DResources *ActiveResources() const;
	void ReleaseStaging();

	NativeW3DResources *m_resources;
	unsigned int m_bindingGeneration;
	BufferDescriptor m_descriptor;
	GpuHandle m_handle;
	unsigned char *m_staging;
	size_t m_lockOffset;
	size_t m_lockBytes;
	RenderBufferUpdateMode m_lockMode;
	bool m_locked;
	bool m_failedMutation;
};

}
}

#endif
