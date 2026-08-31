#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"

#include <assert.h>
#include <limits.h>

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
	m_state(0), m_frameOpen(false)
{
}

NativeW3DRenderer::~NativeW3DRenderer()
{
	if (m_state != 0)
	{
		assert(IsOwnerThread());
		if (!IsOwnerThread())
		{
			return;
		}
		Shutdown();
	}
}

RenderResult NativeW3DRenderer::Initialize(void *window,
	const NativeW3DRendererDescriptor &descriptor)
{
	if (m_state != 0 || window == 0 || descriptor.width == 0 ||
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
	NativeW3DRenderState *state = NativeW3DRenderState::Create();
	if (state == 0)
	{
		device->shutdown();
		delete device;
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (state->BindOwner() != RENDER_RESULT_OK ||
		state->AttachBackend(device, context) != RENDER_RESULT_OK)
	{
		state->Release();
		device->shutdown();
		delete device;
		return RENDER_RESULT_FAILED;
	}
	m_state = state;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderer::Shutdown()
{
	if (m_state == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (!IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult admissionResult = m_state->BeginShutdown();
	unsigned int drained = 0;
	const RenderResult cleanupResult = admissionResult == RENDER_RESULT_OK ?
		m_state->DrainCleanup(0, &drained) : admissionResult;
	IRenderDevice *device = m_state->Device();
	if (device != 0)
	{
		device->shutdown();
		delete device;
	}
	m_state->DetachBackend();
	m_state->Release();
	m_state = 0;
	m_frameOpen = false;
	return cleanupResult;
}

RenderResult NativeW3DRenderer::BeginFrame()
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	// Producer-side destruction is bounded so a worker burst cannot consume an
	// entire render frame.  Shutdown drains the remaining accepted work before
	// releasing the backend.
	unsigned int drained = 0;
	const RenderResult cleanupResult = m_state->DrainCleanup(64, &drained);
	if (cleanupResult != RENDER_RESULT_OK)
	{
		return cleanupResult;
	}
	const RenderResult result = context->beginFrame();
	if (result == RENDER_RESULT_OK)
	{
		m_frameOpen = true;
	}
	return result;
}

RenderResult NativeW3DRenderer::Submit(const NativeW3DResources &resources,
	const LegacyLogicalState &state,
	const NativeDrawPacket &packet)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || !m_frameOpen || !IsOwnerThread() || !resources.IsBoundTo(this) ||
		!packet.vertexBuffer.isValid() ||
		packet.vertexStride == 0 || packet.vertexLayout.stride != packet.vertexStride ||
		packet.vertexCount == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!resources.IsVertexRangeValid(packet.vertexBuffer, packet.vertexStride,
		packet.vertexOffset, packet.startVertex, packet.vertexCount) ||
		(packet.indexed && (!resources.IsIndexRangeValid(packet.indexBuffer,
		packet.indexFormat, packet.indexOffset, packet.startIndex,
		packet.indexCount))))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if ((packet.texturePresenceMask & ~((1U << LEGACY_TEXTURE_STAGE_COUNT) - 1U)) != 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		const bool expected = (packet.texturePresenceMask & (1U << stage)) != 0;
		const bool supplied = packet.textures[stage].isValid();
		if (expected != supplied || !resources.IsTextureValidOrEmpty(packet.textures[stage]))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
	}
	RenderResult result = context->setLegacyStateForLayout(state,
		packet.vertexLayout, packet.texturePresenceMask);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	result = context->setVertexBuffer(packet.vertexBuffer,
		packet.vertexStride, packet.vertexOffset);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	for (unsigned int textureStage = 0;
		textureStage < LEGACY_TEXTURE_STAGE_COUNT; ++textureStage)
	{
		result = context->setTexture(textureStage,
			packet.textures[textureStage]);
		if (result != RENDER_RESULT_OK)
		{
			return result;
		}
	}
	result = context->setPrimitiveTopology(packet.topology);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (!packet.indexed)
	{
		return context->draw(packet.vertexCount, packet.startVertex);
	}
	result = context->setIndexBuffer(packet.indexBuffer, packet.indexFormat,
		packet.indexOffset);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	return context->drawIndexed(packet.indexCount, packet.startIndex,
		packet.baseVertex);
}

RenderResult NativeW3DRenderer::EndFrame(bool present)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || !m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_frameOpen = false;
	const RenderResult endResult = context->endFrame();
	if (endResult != RENDER_RESULT_OK || !present)
	{
		return endResult;
	}
	IRenderDevice *device = m_state->Device();
	return device == 0 ? RENDER_RESULT_FAILED : device->present();
}

RenderResult NativeW3DRenderer::DrainFailedRecoveryCleanup(
	NativeW3DRenderState *state, unsigned int *drained)
{
	if (state == 0 || drained == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult closeResult = state->BeginShutdown();
	if (closeResult != RENDER_RESULT_OK)
	{
		return closeResult;
	}
	// Recovery has already made the backend non-operational.  Unpublish it
	// before accepted cleanup runs so callbacks can release their tokens
	// without calling through a stale backend pointer.
	const RenderResult detachResult = state->DetachBackend();
	if (detachResult != RENDER_RESULT_OK)
	{
		return detachResult;
	}
	return state->DrainCleanup(0, drained);
}

RenderResult NativeW3DRenderer::RecoverDevice()
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = device->recoverDevice();
	if (result != RENDER_RESULT_OK)
	{
		// D3D11RenderDevice tears down its native context on failed recovery.
		// The facade must make the same terminal transition before a caller can
		// observe it again; retaining m_context would permit a later frame to
		// call through a released immediate-context object.
		unsigned int drained = 0;
		DrainFailedRecoveryCleanup(m_state, &drained);
		device->shutdown();
		delete device;
		m_state->Release();
		m_state = 0;
		m_frameOpen = false;
		return result;
	}
	IRenderContext *context = device->immediateContext();
	if (context == 0)
	{
		unsigned int drained = 0;
		DrainFailedRecoveryCleanup(m_state, &drained);
		device->shutdown();
		delete device;
		m_state->Release();
		m_state = 0;
		m_frameOpen = false;
		return RENDER_RESULT_FAILED;
	}
	return m_state->ReplaceContext(context);
}

RenderResult NativeW3DRenderer::Resize(unsigned int width, unsigned int height)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return device->resize(width, height);
}

bool NativeW3DRenderer::IsInitialized() const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return m_state != 0 && m_state->IsOperational() && device != 0 &&
		device->isOperational();
}

bool NativeW3DRenderer::IsFrameOpen() const
{
	return m_frameOpen;
}

unsigned int NativeW3DRenderer::PendingCleanup() const
{
	return m_state == 0 ? 0 : m_state->PendingCleanup();
}

bool NativeW3DRenderer::IsOwnerThread() const
{
	return m_state != 0 && m_state->IsOwnerThread();
}
}
}
