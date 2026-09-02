#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"
#include "Renderer/ThreadedRenderDevice.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <assert.h>
#include <limits.h>

namespace rts
{
namespace render
{
namespace
{
void PopulateLegacyLayout(const RenderVertexLayout &source,
	LegacyVertexLayout &destination)
{
	destination.stride = source.stride;
	destination.elementCount = source.elementCount;
	destination.preTransformed = source.preTransformed;
	for (unsigned int index = 0; index < source.elementCount; ++index)
	{
		destination.elements[index].semantic = source.elements[index].semantic;
		destination.elements[index].semanticIndex =
			source.elements[index].semanticIndex;
		destination.elements[index].format = source.elements[index].format;
		destination.elements[index].byteOffset = source.elements[index].byteOffset;
	}
}

bool IsCompactPositionColorLayout(const RenderVertexLayout &layout,
	unsigned int vertexStride)
{
	return vertexStride == sizeof(float) * 4 &&
		layout.stride == vertexStride && !layout.preTransformed &&
		layout.elementCount == 2 &&
		layout.elements[0].semantic == RENDER_VERTEX_SEMANTIC_POSITION &&
		layout.elements[0].semanticIndex == 0 &&
		layout.elements[0].format == RENDER_VERTEX_DATA_FLOAT3 &&
		layout.elements[0].byteOffset == 0 &&
		layout.elements[1].semantic == RENDER_VERTEX_SEMANTIC_DIFFUSE &&
		layout.elements[1].semanticIndex == 0 &&
		layout.elements[1].format == RENDER_VERTEX_DATA_COLOR_BGRA8 &&
		layout.elements[1].byteOffset == sizeof(float) * 3;
}
}

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
	minimumVertexIndex(0),
	baseVertex(0), indexed(false)
{
}

NativeW3DRenderer::NativeW3DRenderer() :
	m_state(0), m_recoveryResources(0), m_frameOpen(false),
	m_frameFailure(RENDER_RESULT_OK),
	m_ownsBackend(false), m_borrowedMode(false)
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
		if (m_ownsBackend)
		{
			Shutdown();
		}
		else
		{
			m_state->Release();
			m_state = 0;
		}
	}
}

RenderResult NativeW3DRenderer::Initialize(void *window,
	const NativeW3DRendererDescriptor &descriptor)
{
	if (m_state != 0 || m_borrowedMode || window == 0 || descriptor.width == 0 ||
		descriptor.height == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	ThreadedRenderOptions threadedOptions;
	// Native product rendering owns one dedicated backend thread.  Preserve the
	// process-wide serial policy as a mode switch: serial remains a dedicated
	// owner (and therefore keeps the same lifecycle/publication contract), while
	// parallel permits bounded producer/owner overlap.
	threadedOptions.serial = !rts::UseParallelPipelines();
	IRenderDevice *device = CreateThreadedD3D11RenderDevice(threadedOptions);
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
	m_ownsBackend = true;
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
	if (!m_ownsBackend)
	{
		if (m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_state->Release();
		m_state = 0;
		m_frameOpen = false;
		m_frameFailure = RENDER_RESULT_OK;
		return RENDER_RESULT_OK;
	}
	if (m_state->BoundResourceTables() != 0)
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
	m_frameFailure = RENDER_RESULT_OK;
	m_ownsBackend = false;
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
	m_frameFailure = RENDER_RESULT_OK;
	const RenderResult result = context->beginFrame();
	if (result == RENDER_RESULT_OK)
	{
		m_frameOpen = true;
	}
	return result;
}

RenderResult NativeW3DRenderer::SetViewport(const RenderViewport &viewport)
{
	return SetViewportInternal(viewport, true);
}

RenderResult NativeW3DRenderer::SetViewportExternal(
	const RenderViewport &viewport)
{
	return SetViewportInternal(viewport, false);
}

RenderResult NativeW3DRenderer::SetRenderTargetsExternal(
	const RenderTargetBinding &binding)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return context->setRenderTargets(binding);
}

RenderResult NativeW3DRenderer::ClearExternal(unsigned int clearFlags,
	const RenderFloat4 &color, float depth, unsigned int stencil)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return context->clearTargets(clearFlags, color, depth, stencil);
}

RenderResult NativeW3DRenderer::CaptureBackBuffer(void *destination,
	size_t destinationBytes, size_t destinationRowPitch, RenderFormat *format)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || !IsOwnerThread() || destination == 0 ||
		destinationBytes == 0 || destinationRowPitch == 0 || format == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return device->captureBackBuffer(destination, destinationBytes,
		destinationRowPitch, format);
}

RenderResult NativeW3DRenderer::SetViewportInternal(
	const RenderViewport &viewport, bool requireFacadeFrame)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || (requireFacadeFrame && !m_frameOpen) ||
		!IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return context->setViewport(viewport.x, viewport.y, viewport.width,
		viewport.height, viewport.minimumDepth, viewport.maximumDepth);
}

RenderResult NativeW3DRenderer::Submit(const NativeW3DResources &resources,
	const LegacyLogicalState &state,
	const NativeDrawPacket &packet)
{
	return SubmitInternal(resources, state, packet, true);
}

RenderResult NativeW3DRenderer::SubmitExternal(
	const NativeW3DResources &resources, const LegacyLogicalState &state,
	const NativeDrawPacket &packet)
{
	return SubmitInternal(resources, state, packet, false);
}

RenderResult NativeW3DRenderer::SubmitInternal(
	const NativeW3DResources &resources, const LegacyLogicalState &state,
	const NativeDrawPacket &packet, bool requireFacadeFrame)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || (requireFacadeFrame && !m_frameOpen) ||
		!IsOwnerThread() || !resources.IsBoundTo(this) ||
		!packet.vertexBuffer.isValid() ||
		packet.vertexStride == 0 || packet.vertexLayout.stride != packet.vertexStride ||
		packet.vertexCount == 0 ||
		packet.vertexLayout.elementCount > RenderVertexLayout::MAX_ELEMENT_COUNT)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!resources.IsVertexRangeValidForSubmission(packet.vertexBuffer,
		packet.vertexStride,
		packet.vertexOffset, packet.startVertex, packet.vertexCount) ||
		(packet.indexed && (!resources.IsIndexRangeValidForSubmission(
		packet.indexBuffer,
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
	// The compact native position/color stream has a real untextured shader and
	// canonical input layout in D3D11.  Route that exact declaration through the
	// explicit enum path; setLegacyStateForLayout historically receives the
	// textured compatibility enum and would bind POSITION3_NORMAL_COLOR_TEX1 to
	// this four-float stream.  All other packets retain the declaration-aware
	// compatibility path.
	RenderResult result = RENDER_RESULT_OK;
	if (packet.texturePresenceMask == 0 &&
		packet.vertexFormat == RENDER_VERTEX_POSITION3_COLOR &&
		IsCompactPositionColorLayout(packet.vertexLayout, packet.vertexStride))
	{
		result = context->setLegacyState(state,
			RENDER_VERTEX_POSITION3_COLOR, 0);
	}
	else
	{
		LegacyVertexLayout legacyLayout;
		PopulateLegacyLayout(packet.vertexLayout, legacyLayout);
		result = context->setLegacyStateForLayout(state,
			legacyLayout, packet.texturePresenceMask);
	}
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

RenderResult NativeW3DRenderer::GetBackBufferInfo(
	RenderBackBufferInfo *info) const
{
	if (info == 0 || m_state == 0 || m_state->Device() == 0 ||
		!IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return m_state->Device()->getBackBufferInfo(info);
}

RenderResult NativeW3DRenderer::GetTextureFilterCapabilities(
	RenderTextureFilterCapabilities *capabilities) const
{
	if (capabilities == 0 || m_state == 0 || m_state->Device() == 0 ||
		!IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return m_state->Device()->getTextureFilterCapabilities(capabilities);
}

RenderResult NativeW3DRenderer::EndFrame(bool present)
{
	IRenderContext *context = m_state == 0 ? 0 : m_state->Context();
	if (context == 0 || !m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult frameFailure = m_frameFailure;
	m_frameFailure = RENDER_RESULT_OK;
	const RenderResult endResult = context->endFrame();
	m_frameOpen = false;
	if (endResult != RENDER_RESULT_OK)
	{
		// ThreadedRenderDevice::endFrame marks the producer packet ended before
		// returning a producer-side command failure. A visible direct caller still
		// needs a sealed packet; the aggregate's non-present path finalizes it
		// explicitly after any readback/cancellation work.
		if (IsThreadedRenderDevice(m_state->Device()))
			FinalizeEndedFrame(false);
		return endResult;
	}
	if (frameFailure != RENDER_RESULT_OK)
	{
		if (IsThreadedRenderDevice(m_state->Device()))
			FinalizeEndedFrame(false);
		return frameFailure;
	}
	if (!present)
		return RENDER_RESULT_OK;
	return FinalizeEndedFrame(present);
}

RenderResult NativeW3DRenderer::FinalizeEndedFrame(bool present)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (IsThreadedRenderDevice(device))
		return SubmitThreadedRenderFrame(device, present);
	if (!present)
		return RENDER_RESULT_OK;
	return device->present();
}

RenderResult NativeW3DRenderer::Present()
{
	return FinalizeEndedFrame(true);
}

RenderResult NativeW3DRenderer::DrainFailedRecoveryCleanup(
	NativeW3DRenderState *state, unsigned int *drained)
{
	if (state == 0 || drained == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	// A failed recovery has already discarded the backend allocation table.
	// Publish that terminal authority before queue Close races any producer.
	state->MarkBackendTerminal();
	const RenderResult closeResult = state->BeginShutdown();
	if (closeResult != RENDER_RESULT_OK)
	{
		return closeResult;
	}
	// Recovery has already made the backend non-operational.  Unpublish it
	// before accepted cleanup runs so callbacks can release their tokens
	// without calling through a stale backend pointer.
	const RenderResult detachResult = state->DetachBackend(true);
	if (detachResult != RENDER_RESULT_OK)
	{
		return detachResult;
	}
	return state->DrainCleanup(0, drained);
}

RenderResult NativeW3DRenderer::RecoverDevice()
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread() || !m_ownsBackend)
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
		m_frameFailure = RENDER_RESULT_OK;
		m_ownsBackend = false;
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
		m_frameFailure = RENDER_RESULT_OK;
		m_ownsBackend = false;
		return RENDER_RESULT_FAILED;
	}
	const unsigned int expectedResourceTables = m_recoveryResources == 0 ? 0U : 1U;
	RenderResult publicationResult =
		m_state->BoundResourceTables() == expectedResourceTables &&
		(m_recoveryResources == 0 ||
			m_recoveryResources->IsBoundTo(this)) ?
			m_state->ReplaceContext(context) : RENDER_RESULT_INVALID_ARGUMENT;
	if (publicationResult == RENDER_RESULT_OK && m_recoveryResources != 0)
	{
		publicationResult =
			m_recoveryResources->RestoreStaticBuffersAfterRecovery();
	}
	if (publicationResult != RENDER_RESULT_OK)
	{
		// Context publication and buffer restoration are one transaction. A
		// partially restored facade must become terminal before any later frame
		// can observe stale mutable bytes or only a subset of static geometry.
		unsigned int drained = 0;
		DrainFailedRecoveryCleanup(m_state, &drained);
		device->shutdown();
		delete device;
		m_state->Release();
		m_state = 0;
		m_frameOpen = false;
		m_frameFailure = RENDER_RESULT_OK;
		m_ownsBackend = false;
		return publicationResult;
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderer::Resize(unsigned int width, unsigned int height)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread() || !m_ownsBackend)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const unsigned int expectedResourceTables = m_recoveryResources == 0 ? 0U : 1U;
	if (m_state->BoundResourceTables() != expectedResourceTables ||
		(m_recoveryResources != 0 &&
			!m_recoveryResources->IsBoundTo(this)))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult result = device->resize(width, height);
	if (result == RENDER_RESULT_OK && width != 0 && height != 0 &&
		m_recoveryResources != 0)
	{
		result = m_recoveryResources->RepublishStaticBuffersAfterResize();
	}
	if (result != RENDER_RESULT_OK)
	{
		unsigned int drained = 0;
		DrainFailedRecoveryCleanup(m_state, &drained);
		device->shutdown();
		delete device;
		m_state->Release();
		m_state = 0;
		m_frameOpen = false;
		m_frameFailure = RENDER_RESULT_OK;
		m_ownsBackend = false;
	}
	return result;
}

RenderResult NativeW3DRenderer::SetSwapInterval(unsigned int interval)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return device->setSwapInterval(interval);
}

RenderResult NativeW3DRenderer::GetSwapInterval(unsigned int *interval) const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread() || interval == 0)
		return RENDER_RESULT_INVALID_ARGUMENT;
	return device->getSwapInterval(interval);
}

RenderResult NativeW3DRenderer::SetGamma(float gamma, float brightness,
	float contrast, bool calibrate, bool useLimit)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || m_frameOpen || !IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return device->setGamma(gamma, brightness, contrast, calibrate, useLimit);
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

bool NativeW3DRenderer::HasBackendState() const
{
	return m_state != 0;
}

bool NativeW3DRenderer::IsThreaded() const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return device != 0 && IsThreadedRenderDevice(device);
}

bool NativeW3DRenderer::IsBackendOperational() const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return m_state != 0 && m_state->IsOperational() && device != 0 &&
		device->isOperational();
}

bool NativeW3DRenderer::CanRecoverDevice() const
{
	return m_state != 0 && m_ownsBackend && !m_frameOpen &&
		IsOwnerThread() && m_state->IsOperational() &&
		m_state->Device() != 0;
}

uint64_t NativeW3DRenderer::LastThreadedSubmissionSequence() const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return device == 0 ? 0 : LastThreadedRenderFrameSequence(device);
}

bool NativeW3DRenderer::PollThreadedCompletion(
	ThreadedRenderFrameCompletion *completion)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return device != 0 && IsThreadedRenderDevice(device) && IsOwnerThread() &&
		PollThreadedRenderCompletion(device, completion);
}

RenderResult NativeW3DRenderer::DrainThreaded()
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || !IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	return IsThreadedRenderDevice(device) ? DrainThreadedRenderDevice(device) :
		RENDER_RESULT_UNSUPPORTED;
}

RenderResult NativeW3DRenderer::CancelThreadedFrame(RenderResult reason)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || !IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (!IsThreadedRenderDevice(device))
		return RENDER_RESULT_UNSUPPORTED;
	const RenderResult result = CancelThreadedRenderFrame(device, reason);
	// The low-level cancellation closes the producer frame directly, so keep
	// the facade's frame state in lockstep even when the owner reports the
	// cancellation reason as a failure.
	if (result != RENDER_RESULT_INVALID_ARGUMENT)
	{
		m_frameOpen = false;
		m_frameFailure = RENDER_RESULT_OK;
	}
	return result;
}

bool NativeW3DRenderer::GetThreadedMetrics(ThreadedRenderMetrics *metrics) const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return device != 0 && IsThreadedRenderDevice(device) && IsOwnerThread() &&
		GetThreadedRenderMetrics(device, metrics);
}

void NativeW3DRenderer::RecordFrameFailure(RenderResult result)
{
	if (result != RENDER_RESULT_OK && m_frameOpen && IsOwnerThread() &&
		m_frameFailure == RENDER_RESULT_OK)
	{
		m_frameFailure = result;
	}
}

RenderResult NativeW3DRenderer::AttachBorrowedState(
	NativeW3DRenderState *state)
{
	if (m_state != 0 || m_borrowedMode || state == 0 || !state->IsOperational() ||
		!state->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	state->AddRef();
	m_state = state;
	m_frameOpen = false;
	m_frameFailure = RENDER_RESULT_OK;
	m_ownsBackend = false;
	m_borrowedMode = true;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderer::DetachBorrowedState()
{
	if (m_state == 0)
	{
		m_borrowedMode = false;
		return RENDER_RESULT_OK;
	}
	if (m_ownsBackend || m_frameOpen || !m_state->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_state->Release();
	m_state = 0;
	m_frameFailure = RENDER_RESULT_OK;
	m_borrowedMode = false;
	return RENDER_RESULT_OK;
}
}
}
