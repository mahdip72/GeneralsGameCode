#include "Renderer/NativeW3DRenderer.h"
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
	rts::render::NativeW3DRenderer renderer;
	rts::render::NativeW3DResources resources;
	rts::render::NativeW3DRendererDescriptor descriptor;
	rts::render::NativeDrawPacket packet;
	rts::render::LegacyLogicalState state;

	result |= Check(renderer.BeginFrame() == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot begin a native frame before initialization");
	result |= Check(renderer.EndFrame(false) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot end a native frame before initialization");
	result |= Check(renderer.Submit(resources, state, packet) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot submit a native draw before initialization");
	descriptor.width = 640;
	descriptor.height = 480;
	result |= Check(renderer.Initialize(0, descriptor) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"a native facade rejects a null window without creating a legacy device");
	result |= Check(!renderer.IsInitialized() && !renderer.IsFrameOpen(),
		"a failed initialization leaves no native renderer state behind");
	result |= Check(renderer.RecoverDevice() == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		renderer.Resize(640, 480) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"recovery and resize reject an uninitialized native facade");
	result |= Check(renderer.Shutdown() == rts::render::RENDER_RESULT_OK,
		"shutdown is idempotent before native initialization");
	result |= Check(resources.Bind(0) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!resources.Destroy(packet.vertexBuffer),
		"native resource tables reject an unbound renderer and stale handles");
	return result;
}
