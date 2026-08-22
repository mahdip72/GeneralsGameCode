#include "nativew3d2.h"

#include <cstdio>

int main()
{
	NativeW3D2 w3d;
	rts::render::NativeW3DRendererDescriptor descriptor;
	descriptor.width = 640;
	descriptor.height = 480;
	if (w3d.Initialize(0, descriptor) != rts::render::RENDER_RESULT_INVALID_ARGUMENT)
	{
		std::fprintf(stderr, "FAIL: native WW3D2 accepted an invalid window\n");
		return 1;
	}
	if (w3d.Shutdown() != rts::render::RENDER_RESULT_OK)
	{
		std::fprintf(stderr, "FAIL: native WW3D2 shutdown was not deterministic\n");
		return 1;
	}
	return 0;
}
