#include "Renderer/NativeW3DRenderState.h"

#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rts
{
namespace render
{
namespace
{
long IncrementReference(volatile long *references)
{
#ifdef _WIN32
	return InterlockedIncrement(references);
#else
	return __sync_add_and_fetch(references, 1);
#endif
}

long DecrementReference(volatile long *references)
{
#ifdef _WIN32
	return InterlockedDecrement(references);
#else
	return __sync_sub_and_fetch(references, 1);
#endif
}
}

NativeW3DRenderState::NativeW3DRenderState(unsigned int cleanupCapacity) :
	m_references(1), m_cleanup(cleanupCapacity), m_device(0), m_context(0),
	m_generation(1)
{
}

NativeW3DRenderState::~NativeW3DRenderState()
{
}

NativeW3DRenderState *NativeW3DRenderState::Create(unsigned int cleanupCapacity)
{
	if (cleanupCapacity == 0)
	{
		return 0;
	}
	return new (std::nothrow) NativeW3DRenderState(cleanupCapacity);
}

void NativeW3DRenderState::AddRef()
{
	IncrementReference(&m_references);
}

void NativeW3DRenderState::Release()
{
	if (DecrementReference(&m_references) == 0)
	{
		delete this;
	}
}

RenderResult NativeW3DRenderState::BindOwner()
{
	return m_cleanup.BindOwner();
}

RenderResult NativeW3DRenderState::AttachBackend(IRenderDevice *device,
	IRenderContext *context)
{
	if (device == 0 || context == 0 || !IsOwnerThread() ||
		m_device != 0 || m_context != 0 || !IsAcceptingCleanup())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_device = device;
	m_context = context;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderState::ReplaceContext(IRenderContext *context)
{
	if (context == 0 || !IsOwnerThread() || m_device == 0 ||
		!IsAcceptingCleanup())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_context = context;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderState::DetachBackend()
{
	if (!IsOwnerThread() || IsAcceptingCleanup())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_context = 0;
	m_device = 0;
	++m_generation;
	if (m_generation == 0)
	{
		m_generation = 1;
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DRenderState::BeginShutdown()
{
	return m_cleanup.Close();
}

RenderResult NativeW3DRenderState::EnqueueCleanup(NativeW3DOwnerCommand command,
	NativeW3DOwnerToken *token)
{
	return m_cleanup.Enqueue(command, token);
}

RenderResult NativeW3DRenderState::DrainCleanup(unsigned int maxCommands,
	unsigned int *drained)
{
	return m_cleanup.Drain(maxCommands, drained);
}

bool NativeW3DRenderState::IsOwnerThread() const
{
	return m_cleanup.IsOwnerThread();
}

bool NativeW3DRenderState::IsAcceptingCleanup() const
{
	return m_cleanup.IsAccepting();
}

unsigned int NativeW3DRenderState::PendingCleanup() const
{
	return m_cleanup.Size();
}

bool NativeW3DRenderState::IsOperational() const
{
	return IsOwnerThread() && IsAcceptingCleanup() && m_device != 0 &&
		m_context != 0;
}

unsigned int NativeW3DRenderState::Generation() const
{
	return IsOwnerThread() ? m_generation : 0;
}

IRenderDevice *NativeW3DRenderState::Device() const
{
	return IsOwnerThread() ? m_device : 0;
}

IRenderContext *NativeW3DRenderState::Context() const
{
	return IsOwnerThread() ? m_context : 0;
}
}
}
