#include "Renderer/NativeW3DResources.h"

#include <cstdio>

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}
}

int main()
{
	int result = 0;
	rts::render::NativeW3DResources resources(2);
	rts::render::GpuHandle stale;
	rts::render::BufferDescriptor descriptor;
	result |= Check(resources.Bind(0) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"resource table refuses a null renderer");
	result |= Check(!resources.IsValid(stale) && !resources.Destroy(stale),
		"default and stale handles are never live resources");
	result |= Check(resources.CreateBuffer(descriptor, 0, 0, 0) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"resource creation validates its output handle before touching a device");
	result |= Check(resources.Shutdown() == rts::render::RENDER_RESULT_OK,
		"an unbound resource table shuts down deterministically");
	return result;
}
