#include "Renderer/NativeW3DRenderer.h"

#include <limits.h>
#include <windows.h>

namespace rts
{
namespace render
{
NativeW3DRendererDescriptor::NativeW3DRendererDescriptor() :
	width(0), height(0), adapterIndex(UINT_MAX), enableDebugLayer(false),
	enableVsync(true), allowSoftwareFallback(true)
{
}

NativeDrawPacket::NativeDrawPacket() :
	vertexStride(0), vertexOffset(0), indexOffset(0),
	indexFormat(RENDER_FORMAT_R16_UINT),
	vertexFormat(RENDER_VERTEX_POSITION3_COLOR),
	topology(RENDER_PRIMITIVE_TRIANGLE_LIST), texturePresenceMask(0),
	vertexCount(0), startVertex(0), indexCount(0), startIndex(0),
	baseVertex(0), indexed(false)
{
}

NativeW3DRenderer::NativeW3DRenderer() :
	m_device(0), m_context(0), m_ownerThread(0), m_frameOpen(false)
{
}

NativeW3DRenderer::~NativeW3DRenderer()
{
	if (m_device != 0 && IsOwnerThread())
	{
		Shutdown();
	}
}

RenderResult NativeW3DRenderer::Initialize(void *window,
	const NativeW3DRendererDescriptor &descriptor)
{
	if (m_device != 0 || window == 0 || descriptor.width == 0 ||
		descriptor.height == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	IRenderDevice *device = CreateD3D11RenderDevice();
	if (device == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = descriptor.width;
	parameters.height = descriptor.height;
	parameters.adapterIndex = descriptor.adapterIndex;
	parameters.enableDebugLayer = descriptor.enableDebugLayer;
	parameters.enableVsync = descriptor.enableVsync;
	parameters.allowSoftwareFallback = descriptor.allowSoftwareFallback;
	const RenderResult result = device->initialize(parameters);
	if (result != RENDER_RESULT_OK)
	{
		delete device;
		return result;
	}
	IRenderContext *context = device->immediateContext();
	if (context == 0)
	{
		device->shutdown();
		delete device;
		return RENDER_RESULT_FAILED;
	}
	m_device = device;
	m_context = context;
	m_ownerThread = GetCurrentThreadId();
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderer::Shutdown()
{
	if (m_device == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (!IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_device != 0)
	{
		m_device->shutdown();
		delete m_device;
	}
	m_device = 0;
	m_context = 0;
	m_ownerThread = 0;
	m_frameOpen = false;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderer::BeginFrame()
{
	if (m_context == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = m_context->beginFrame();
	if (result == RENDER_RESULT_OK)
	{
		m_frameOpen = true;
	}
	return result;
}

RenderResult NativeW3DRenderer::Submit(const LegacyLogicalState &state,
	const NativeDrawPacket &packet)
{
	if (m_context == 0 || !m_frameOpen || !IsOwnerThread() ||
		!packet.vertexBuffer.isValid() ||
		packet.vertexStride == 0 || packet.vertexCount == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (packet.indexed && (!packet.indexBuffer.isValid() ||
		packet.indexCount == 0))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult result = m_context->setLegacyState(state,
		packet.vertexFormat, packet.texturePresenceMask);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	result = m_context->setVertexBuffer(packet.vertexBuffer,
		packet.vertexStride, packet.vertexOffset);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		result = m_context->setTexture(stage, packet.textures[stage]);
		if (result != RENDER_RESULT_OK)
		{
			return result;
		}
	}
	result = m_context->setPrimitiveTopology(packet.topology);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (!packet.indexed)
	{
		return m_context->draw(packet.vertexCount, packet.startVertex);
	}
	result = m_context->setIndexBuffer(packet.indexBuffer, packet.indexFormat,
		packet.indexOffset);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	return m_context->drawIndexed(packet.indexCount, packet.startIndex,
		packet.baseVertex);
}

RenderResult NativeW3DRenderer::EndFrame(bool present)
{
	if (m_context == 0 || !m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_frameOpen = false;
	const RenderResult endResult = m_context->endFrame();
	if (endResult != RENDER_RESULT_OK || !present)
	{
		return endResult;
	}
	return m_device->present();
}

RenderResult NativeW3DRenderer::RecoverDevice()
{
	if (m_device == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = m_device->recoverDevice();
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	m_context = m_device->immediateContext();
	return m_context == 0 ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderer::Resize(unsigned int width, unsigned int height)
{
	if (m_device == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return m_device->resize(width, height);
}

bool NativeW3DRenderer::IsInitialized() const
{
	return m_device != 0 && m_device->isOperational();
}

bool NativeW3DRenderer::IsFrameOpen() const
{
	return m_frameOpen;
}

bool NativeW3DRenderer::IsOwnerThread() const
{
	return m_ownerThread != 0 && m_ownerThread == GetCurrentThreadId();
}
}
}
