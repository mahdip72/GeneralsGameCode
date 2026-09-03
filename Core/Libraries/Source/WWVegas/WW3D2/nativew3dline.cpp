#include "nativew3dline.h"

#include <atomic>
#include <mutex>
#include <new>
#include <string.h>

namespace rts
{
namespace render
{
namespace
{
std::atomic<NativeLine3DSubmitter *> g_nativeLine3DSubmitter(0);
std::mutex g_nativeLine3DSubmitterMutex;

struct NativeLine3DBufferNode
{
	NativeLine3DBufferNode(NativeLine3DBufferSet *set,
		NativeLine3DBufferNode *nextNode) : buffers(set), next(nextNode)
	{
	}

	NativeLine3DBufferSet *buffers;
	NativeLine3DBufferNode *next;
};

bool RegisterLine3DBufferSet(void *&storage,
	NativeLine3DBufferSet *buffers)
{
	if (buffers == 0)
	{
		return false;
	}
	NativeLine3DBufferNode *node =
		static_cast<NativeLine3DBufferNode *>(storage);
	while (node != 0)
	{
		if (node->buffers == buffers)
		{
			return true;
		}
		node = node->next;
	}
	NativeLine3DBufferNode *newNode =
		new (std::nothrow) NativeLine3DBufferNode(
			buffers, static_cast<NativeLine3DBufferNode *>(storage));
	if (newNode == 0)
	{
		return false;
	}
	storage = newNode;
	return true;
}

void UnregisterLine3DBufferSet(void *&storage,
	NativeLine3DBufferSet *buffers)
{
	NativeLine3DBufferNode **link =
		reinterpret_cast<NativeLine3DBufferNode **>(&storage);
	while (*link != 0)
	{
		if ((*link)->buffers == buffers)
		{
			NativeLine3DBufferNode *removed = *link;
			*link = removed->next;
			delete removed;
			return;
		}
		link = &(*link)->next;
	}
}

void DeleteLine3DBufferRegistry(void *&storage)
{
	NativeLine3DBufferNode *node =
		static_cast<NativeLine3DBufferNode *>(storage);
	while (node != 0)
	{
		NativeLine3DBufferNode *next = node->next;
		delete node;
		node = next;
	}
	storage = 0;
}

bool DestroyNativeLineBuffer(NativeW3DResources *resources,
	GpuHandle *handle)
{
	if (handle == 0 || !handle->isValid())
	{
		return true;
	}
	if (resources == 0 || !resources->IsOwnerThread())
	{
		return false;
	}
	// IsValid also rejects off-owner callers. Such a rejection is not proof
	// that the handle is stale: retain it and the sidecar for the owner retry.
	// A handle that is no longer valid has crossed a terminal owner/rebind
	// boundary.  The resource table owns or has already discarded it, so the
	// local cache can be cleared.  A still-valid handle whose Destroy call is
	// refused remains intact for a later render-thread retry.
	if (!resources->IsValid(*handle))
	{
		*handle = GpuHandle();
		return true;
	}
	if (resources->Destroy(*handle))
	{
		*handle = GpuHandle();
		return true;
	}
	return false;
}

BufferDescriptor MakeLineVertexBufferDescriptor()
{
	BufferDescriptor descriptor;
	descriptor.byteCount =
		sizeof(NativeLine3DVertex) * NATIVE_LINE3D_VERTEX_COUNT;
	descriptor.stride = sizeof(NativeLine3DVertex);
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	return descriptor;
}

BufferDescriptor MakeLineIndexBufferDescriptor()
{
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(unsigned short) * NATIVE_LINE3D_INDEX_COUNT;
	descriptor.stride = sizeof(unsigned short);
	descriptor.binding = RENDER_BUFFER_INDEX;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	return descriptor;
}

bool HasCompleteLineBuffers(const NativeW3DResources *resources,
	const NativeLine3DBufferSet *buffers)
{
	return resources != 0 && buffers != 0 &&
		buffers->vertexBuffer.isValid() && buffers->indexBuffer.isValid() &&
		resources->IsValid(buffers->vertexBuffer) &&
		resources->IsValid(buffers->indexBuffer);
}
}

NativeLine3DBufferSet::NativeLine3DBufferSet() : vertexBuffer(), indexBuffer(),
	lineOwned(false), lineReferenceActive(false),
	contextReferenceActive(false), resourceOwnerTerminal(false)
{
}

NativeLine3DRenderContext::NativeLine3DRenderContext(
	NativeW3DRenderer *renderer, NativeW3DResources *resources) :
	m_renderer(renderer), m_resources(resources), m_bufferSets(0)
{
}

NativeLine3DRenderContext::~NativeLine3DRenderContext()
{
	// NativeW3D2 normally drains this registry under the lifecycle gate before
	// its resource table is shut down.  Destruction is also the final owner
	// retry for a line that was deleted from a worker thread.  Do not merely
	// discard the registry nodes: that would strand both the sidecar and its
	// handles after the Line3D object has gone away.
	NativeLine3DSubmitterLifecycleScope lifecycleScope;
	if (lifecycleScope.Get() == this)
	{
		lifecycleScope.Publish(0);
	}
	DrainLine3D();

	// If this destructor is reached off the resource owner thread, Destroy()
	// cannot be retried here.  The resource table is still the physical owner of
	// every valid handle and its own terminal shutdown/fallback path will finish
	// that work.  Mark the metadata terminal so a live Line3D can reclaim its
	// sidecar later without calling this dead context.
	NativeLine3DBufferNode *node =
		static_cast<NativeLine3DBufferNode *>(m_bufferSets);
	while (node != 0)
	{
		NativeLine3DBufferNode *next = node->next;
		NativeLine3DBufferSet *buffers = node->buffers;
		buffers->contextReferenceActive = false;
		buffers->resourceOwnerTerminal = true;
		if (buffers->lineOwned && !buffers->lineReferenceActive)
		{
			delete buffers;
		}
		delete node;
		node = next;
	}
	m_bufferSets = 0;
}

RenderResult NativeLine3DRenderContext::SubmitLine3D(
	const NativeLine3DGeometry &geometry, const LegacyLogicalState &state,
	NativeLine3DBufferSet *buffers)
{
	RenderResult result = RENDER_RESULT_OK;
	if (buffers == 0 || !RegisterLine3DBufferSet(m_bufferSets, buffers))
	{
		result = buffers == 0 ? RENDER_RESULT_INVALID_ARGUMENT :
			RENDER_RESULT_OUT_OF_MEMORY;
	}
	else
	{
		if (buffers->lineOwned)
		{
			buffers->contextReferenceActive = true;
		}
		result = Submit_Native_Line3D(m_renderer, m_resources,
			&state, &geometry, buffers);
	}
	if (result != RENDER_RESULT_OK && m_renderer != 0)
	{
		m_renderer->RecordFrameFailure(result);
	}
	return result;
}

bool NativeLine3DRenderContext::ReleaseLine3D(NativeLine3DBufferSet *buffers)
{
	const bool released = Release_Native_Line3D_Buffers(m_resources, buffers);
	if (released)
	{
		if (buffers != 0)
		{
			buffers->contextReferenceActive = false;
		}
		UnregisterLine3DBufferSet(m_bufferSets, buffers);
		if (buffers != 0 && buffers->lineOwned &&
			!buffers->lineReferenceActive)
		{
			delete buffers;
		}
	}
	return released;
}

void NativeLine3DRenderContext::DrainLine3D()
{
	NativeLine3DBufferNode *node =
		static_cast<NativeLine3DBufferNode *>(m_bufferSets);
	while (node != 0)
	{
		NativeLine3DBufferNode *next = node->next;
		NativeLine3DBufferSet *buffers = node->buffers;
		if (Release_Native_Line3D_Buffers(m_resources, buffers))
		{
			buffers->contextReferenceActive = false;
			UnregisterLine3DBufferSet(m_bufferSets, buffers);
			if (buffers->lineOwned && !buffers->lineReferenceActive)
			{
				delete buffers;
			}
		}
		node = next;
	}
}

unsigned int NativeLine3DRenderContext::PendingLine3DCount() const
{
	unsigned int count = 0;
	NativeLine3DBufferNode *node =
		static_cast<NativeLine3DBufferNode *>(m_bufferSets);
	while (node != 0)
	{
		++count;
		node = node->next;
	}
	return count;
}

NativeLine3DSubmitter *Get_Native_Line3D_Submitter()
{
	return g_nativeLine3DSubmitter.load(std::memory_order_acquire);
}

void Set_Native_Line3D_Submitter(NativeLine3DSubmitter *submitter)
{
	NativeLine3DSubmitterLifecycleScope scope;
	scope.Publish(submitter);
}

NativeLine3DSubmitterScope::NativeLine3DSubmitterScope() :
	m_submitter(0), m_locked(false)
{
	g_nativeLine3DSubmitterMutex.lock();
	m_submitter = g_nativeLine3DSubmitter.load(std::memory_order_acquire);
	m_locked = true;
}

NativeLine3DSubmitterScope::~NativeLine3DSubmitterScope()
{
	if (m_locked)
	{
		m_locked = false;
		g_nativeLine3DSubmitterMutex.unlock();
	}
}

NativeLine3DSubmitter *NativeLine3DSubmitterScope::Get() const
{
	return m_submitter;
}

NativeLine3DSubmitterLifecycleScope::NativeLine3DSubmitterLifecycleScope() :
	m_locked(false)
{
	g_nativeLine3DSubmitterMutex.lock();
	m_locked = true;
}

NativeLine3DSubmitterLifecycleScope::~NativeLine3DSubmitterLifecycleScope()
{
	if (m_locked)
	{
		m_locked = false;
		g_nativeLine3DSubmitterMutex.unlock();
	}
}

NativeLine3DSubmitter *NativeLine3DSubmitterLifecycleScope::Get() const
{
	return g_nativeLine3DSubmitter.load(std::memory_order_acquire);
}

void NativeLine3DSubmitterLifecycleScope::Publish(
	NativeLine3DSubmitter *submitter)
{
	if (m_locked)
	{
		g_nativeLine3DSubmitter.store(submitter, std::memory_order_release);
	}
}

bool Build_Native_Line3D_State(unsigned int shaderBits,
	const LegacyLogicalState *baseState, const float *worldMatrix,
	LegacyLogicalState *state)
{
	if (worldMatrix == 0 || state == 0)
	{
		return false;
	}
	LegacyPipelineState decoded;
	if (!DecodeLegacyShaderBits(shaderBits, &decoded))
	{
		return false;
	}
	LegacyPipelineState pipeline;
	if (baseState != 0)
	{
		*state = *baseState;
	}
	else
	{
		*state = LegacyLogicalState();
	}
	// ShaderClass::Apply owns only this subset of pipeline state.  Keep the
	// seeded render-owner state for blend operations/color mask, depth enable
	// and stencil, fill/scissor/front winding/depth bias, range fog, material
	// sources, texture-factor, and other non-shader-owned fields.
	pipeline = state->pipeline;
	pipeline.shaderBits = decoded.shaderBits;
	pipeline.blend.blendEnable = decoded.blend.blendEnable;
	if (decoded.blend.blendEnable)
	{
		pipeline.blend.sourceColor = decoded.blend.sourceColor;
		pipeline.blend.destinationColor = decoded.blend.destinationColor;
		pipeline.blend.sourceAlpha = decoded.blend.sourceAlpha;
		pipeline.blend.destinationAlpha = decoded.blend.destinationAlpha;
	}
	pipeline.depthStencil.depthWrite = decoded.depthStencil.depthWrite;
	pipeline.depthStencil.depthFunction = decoded.depthStencil.depthFunction;
	pipeline.rasterizer.cullMode = decoded.rasterizer.cullMode;
	pipeline.fogMode = decoded.fogMode;
	pipeline.secondaryGradientEnable = decoded.secondaryGradientEnable;
	pipeline.nPatchEnable = decoded.nPatchEnable;
	pipeline.alphaTestEnable = decoded.alphaTestEnable;
	if (decoded.alphaTestEnable)
	{
		pipeline.alphaFunction = decoded.alphaFunction;
		pipeline.alphaReference = decoded.alphaReference;
	}
	// The legacy path applies VertexMaterialClass::PRELIT_DIFFUSE immediately
	// after Set_Shader.  Reproduce that explicit material seam rather than
	// inheriting a lit/material-colored state from the previous draw: the line's
	// diffuse vertex color is COLOR1, lighting is disabled, and the material
	// channels are the preset's white diffuse/ambient and zero specular/emissive
	// values.  These are material-owned writes, not a replacement of the rest of
	// the render-owner pipeline.
	pipeline.lightingEnable = false;
	pipeline.ambientMaterialSource = RENDER_MATERIAL_SOURCE_MATERIAL;
	pipeline.diffuseMaterialSource = RENDER_MATERIAL_SOURCE_COLOR1;
	pipeline.emissiveMaterialSource = RENDER_MATERIAL_SOURCE_MATERIAL;
	state->pipeline = pipeline;
	state->constants.material = LegacyMaterialState();
	state->texturePresenceMask = 0;
	memcpy(state->constants.world.values, worldMatrix,
		sizeof(state->constants.world.values));
	return true;
}

RenderResult Upload_Native_Line3D_Geometry(
	NativeW3DResources *resources, const NativeLine3DGeometry *geometry,
	NativeLine3DBufferSet *buffers)
{
	if (resources == 0 || geometry == 0 || buffers == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	if (HasCompleteLineBuffers(resources, buffers))
	{
		RenderResult result = resources->UpdateBuffer(
			buffers->vertexBuffer, geometry->vertices,
			sizeof(geometry->vertices), 0, RENDER_BUFFER_UPDATE_PRESERVE);
		if (result != RENDER_RESULT_OK)
		{
			return result;
		}
		return resources->UpdateBuffer(buffers->indexBuffer, geometry->indices,
			sizeof(geometry->indices), 0, RENDER_BUFFER_UPDATE_PRESERVE);
	}

	// A partially destroyed set must not leave a live handle associated with
	// the next geometry upload.  A backend refusal retains the exact handle so
	// the owner can retry; terminally stale handles are cleared by the release
	// helper because the resource table has already taken ownership of them.
	if (!Release_Native_Line3D_Buffers(resources, buffers))
	{
		return RENDER_RESULT_FAILED;
	}
	const BufferDescriptor vertexDescriptor =
		MakeLineVertexBufferDescriptor();
	RenderResult result = resources->CreateBuffer(vertexDescriptor,
		geometry->vertices, sizeof(geometry->vertices), &buffers->vertexBuffer);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	const BufferDescriptor indexDescriptor = MakeLineIndexBufferDescriptor();
	result = resources->CreateBuffer(indexDescriptor, geometry->indices,
		sizeof(geometry->indices), &buffers->indexBuffer);
	if (result != RENDER_RESULT_OK)
	{
		(void)DestroyNativeLineBuffer(resources, &buffers->vertexBuffer);
		return result;
	}
	return RENDER_RESULT_OK;
}

RenderResult Submit_Native_Line3D(NativeW3DRenderer *renderer,
	NativeW3DResources *resources, const LegacyLogicalState *state,
	const NativeLine3DGeometry *geometry, NativeLine3DBufferSet *buffers)
{
	if (renderer == 0 || resources == 0 || state == 0 || geometry == 0 ||
		buffers == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult uploadResult = Upload_Native_Line3D_Geometry(
		resources, geometry, buffers);
	if (uploadResult != RENDER_RESULT_OK)
	{
		return uploadResult;
	}
	NativeDrawPacket packet;
	packet.vertexBuffer = buffers->vertexBuffer;
	packet.indexBuffer = buffers->indexBuffer;
	packet.vertexStride = sizeof(NativeLine3DVertex);
	packet.vertexOffset = 0;
	packet.indexOffset = 0;
	packet.indexFormat = RENDER_FORMAT_R16_UINT;
	packet.vertexFormat = RENDER_VERTEX_POSITION3_COLOR;
	Build_Native_Line3D_Layout(&packet.vertexLayout);
	packet.topology = RENDER_PRIMITIVE_TRIANGLE_LIST;
	packet.texturePresenceMask = 0;
	packet.vertexCount = NATIVE_LINE3D_VERTEX_COUNT;
	packet.startVertex = 0;
	packet.indexCount = NATIVE_LINE3D_INDEX_COUNT;
	packet.startIndex = 0;
	packet.baseVertex = 0;
	packet.indexed = true;
	return renderer->Submit(*resources, *state, packet);
}

bool Release_Native_Line3D_Buffers(NativeW3DResources *resources,
	NativeLine3DBufferSet *buffers)
{
	if (buffers == 0)
	{
		return false;
	}
	bool released = true;
	if (buffers->vertexBuffer.isValid())
	{
		released = DestroyNativeLineBuffer(resources,
			&buffers->vertexBuffer) && released;
	}
	if (buffers->indexBuffer.isValid())
	{
		released = DestroyNativeLineBuffer(resources,
			&buffers->indexBuffer) && released;
	}
	return released;
}
}
}
