#ifndef RTS_RENDERER_NATIVEW3DRENDERER_H
#define RTS_RENDERER_NATIVEW3DRENDERER_H

#include "Renderer/RendererDevice.h"

namespace rts
{
namespace render
{
// Native facade inputs are intentionally logical and handle-based.  Game code
// never receives a backend COM pointer or a legacy-adapter header.
struct NativeW3DRendererDescriptor
{
	NativeW3DRendererDescriptor();

	unsigned int width;
	unsigned int height;
	unsigned int adapterIndex;
	bool enableDebugLayer;
	bool enableVsync;
	bool allowSoftwareFallback;
};

struct NativeDrawPacket
{
	NativeDrawPacket();

	GpuHandle vertexBuffer;
	GpuHandle indexBuffer;
	GpuHandle textures[LEGACY_TEXTURE_STAGE_COUNT];
	unsigned int vertexStride;
	unsigned int vertexOffset;
	unsigned int indexOffset;
	RenderFormat indexFormat;
	LegacyVertexFormat vertexFormat;
	RenderPrimitiveTopology topology;
	unsigned int texturePresenceMask;
	unsigned int vertexCount;
	unsigned int startVertex;
	unsigned int indexCount;
	unsigned int startIndex;
	int baseVertex;
	bool indexed;
};

class NativeW3DRenderer
{
public:
	NativeW3DRenderer();
	~NativeW3DRenderer();

	RenderResult Initialize(void *window,
		const NativeW3DRendererDescriptor &descriptor);
	void Shutdown();
	RenderResult BeginFrame();
	RenderResult Submit(const LegacyLogicalState &state,
		const NativeDrawPacket &packet);
	RenderResult EndFrame(bool present);
	bool IsInitialized() const;
	bool IsFrameOpen() const;

private:
	NativeW3DRenderer(const NativeW3DRenderer &);
	NativeW3DRenderer &operator=(const NativeW3DRenderer &);

	IRenderDevice *m_device;
	IRenderContext *m_context;
	bool m_frameOpen;
};
}
}

#endif
