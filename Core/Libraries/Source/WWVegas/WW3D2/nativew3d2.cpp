#include "nativew3d2.h"

NativeW3D2::NativeW3D2() : m_resourceHost(256), m_resources(4096),
	m_borrowedBackend(false)
{
}

NativeW3D2::~NativeW3D2()
{
	Shutdown();
}

rts::render::RenderResult NativeW3D2::Initialize(void *window,
	const rts::render::NativeW3DRendererDescriptor &descriptor)
{
	if (m_borrowedBackend)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const rts::render::RenderResult result = m_renderer.Initialize(window,
		descriptor);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		return result;
	}
	const rts::render::RenderResult bindResult = m_resources.Bind(&m_renderer);
	if (bindResult != rts::render::RENDER_RESULT_OK)
	{
		m_renderer.Shutdown();
		return bindResult;
	}
	m_renderer.m_recoveryResources = &m_resources;
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::AttachBackend(
	rts::render::IRenderDevice *device, rts::render::IRenderContext *context)
{
	if (m_borrowedBackend || m_renderer.IsInitialized())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	rts::render::RenderResult result = m_resourceHost.Attach(device, context);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		return result;
	}
	result = m_renderer.AttachBorrowedState(m_resourceHost.State());
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_resourceHost.Detach();
		return result;
	}
	result = m_resources.BindHost(&m_resourceHost);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_renderer.DetachBorrowedState();
		m_resourceHost.Detach();
		return result;
	}
	m_borrowedBackend = true;
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::ReplaceBackendContext(
	rts::render::IRenderContext *context)
{
	return !m_borrowedBackend ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
		m_resourceHost.ReplaceContext(context);
}

rts::render::RenderResult NativeW3D2::DrainResourceCleanup(
	unsigned int maxCommands, unsigned int *drained)
{
	return !m_borrowedBackend ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
		m_resourceHost.DrainCleanup(maxCommands, drained);
}

rts::render::RenderResult NativeW3D2::PublishThreadedCompletion(
	rts::render::NativeW3DSubmissionSequence submissionSequence,
	bool resourceFailure)
{
	return !m_borrowedBackend ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
		m_resources.PublishThreadedCompletion(submissionSequence,
			resourceFailure);
}

rts::render::RenderResult NativeW3D2::RecoverDevice()
{
	return m_borrowedBackend ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
		m_renderer.RecoverDevice();
}

rts::render::RenderResult NativeW3D2::Shutdown()
{
	if (m_borrowedBackend && m_renderer.IsFrameOpen())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const rts::render::RenderResult resourcesResult = m_resources.Shutdown();
	if (resourcesResult != rts::render::RENDER_RESULT_OK)
	{
		return resourcesResult;
	}
	m_renderer.m_recoveryResources = 0;
	if (m_borrowedBackend)
	{
		const rts::render::RenderResult hostResult = m_resourceHost.Detach();
		if (hostResult == rts::render::RENDER_RESULT_OK)
		{
			const rts::render::RenderResult rendererResult =
				m_renderer.DetachBorrowedState();
			if (rendererResult != rts::render::RENDER_RESULT_OK)
			{
				return rendererResult;
			}
			m_borrowedBackend = false;
		}
		return hostResult;
	}
	return m_renderer.Shutdown();
}

bool NativeW3D2::IsAttachedToBorrowedBackend() const
{
	return m_borrowedBackend && m_resourceHost.IsAttached();
}

rts::render::NativeW3DRenderer &NativeW3D2::Renderer()
{
	return m_renderer;
}

rts::render::NativeW3DResources &NativeW3D2::Resources()
{
	return m_resources;
}
