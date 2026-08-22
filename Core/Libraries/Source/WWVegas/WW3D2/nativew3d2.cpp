#include "nativew3d2.h"

NativeW3D2::NativeW3D2() : m_resources(4096)
{
}

NativeW3D2::~NativeW3D2()
{
	Shutdown();
}

rts::render::RenderResult NativeW3D2::Initialize(void *window,
	const rts::render::NativeW3DRendererDescriptor &descriptor)
{
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
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::Shutdown()
{
	const rts::render::RenderResult resourcesResult = m_resources.Shutdown();
	if (resourcesResult != rts::render::RENDER_RESULT_OK)
	{
		return resourcesResult;
	}
	return m_renderer.Shutdown();
}

rts::render::NativeW3DRenderer &NativeW3D2::Renderer()
{
	return m_renderer;
}

rts::render::NativeW3DResources &NativeW3D2::Resources()
{
	return m_resources;
}
