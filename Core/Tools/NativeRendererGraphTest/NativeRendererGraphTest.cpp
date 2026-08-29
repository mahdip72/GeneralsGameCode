#include <cstdio>
#include "Renderer/RendererDevice.h"

int main()
{
#if !defined(RTS_RENDERER_HAS_D3D11)
	std::fprintf(stderr, "FAIL: core renderer was built without the D3D11 backend\n");
	return 1;
#else
	rts::render::IRenderDevice *device = rts::render::CreateD3D11RenderDevice();
	if (device == 0)
	{
		std::fprintf(stderr, "FAIL: core renderer cannot expose its D3D11 device factory\n");
		return 1;
	}
	delete device;
	return 0;
#endif
}
