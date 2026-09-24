#include "Renderer/NativeProductDeviceLifecycle.h"

#include <stdio.h>

namespace
{
struct FakeDevice
{
	FakeDevice() : initializeCalls(0), prepareCalls(0), resizeCalls(0),
		shutdownCalls(0), shutdownResultCalls(0), failInitialize(false),
		failPrepare(false), failResize(false), failShutdown(false), width(0),
		height(0), vsync(false) {}

	unsigned int initializeCalls;
	unsigned int prepareCalls;
	unsigned int resizeCalls;
	unsigned int shutdownCalls;
	unsigned int shutdownResultCalls;
	bool failInitialize;
	bool failPrepare;
	bool failResize;
	bool failShutdown;
	unsigned int width;
	unsigned int height;
	bool vsync;
};

bool Initialize(void *context, unsigned int width, unsigned int height,
	bool enableVsync)
{
	FakeDevice *device = static_cast<FakeDevice *>(context);
	++device->initializeCalls;
	device->width = width;
	device->height = height;
	device->vsync = enableVsync;
	return !device->failInitialize;
}

bool PrepareResize(void *context)
{
	FakeDevice *device = static_cast<FakeDevice *>(context);
	++device->prepareCalls;
	return !device->failPrepare;
}

bool Resize(void *context, unsigned int width, unsigned int height)
{
	FakeDevice *device = static_cast<FakeDevice *>(context);
	++device->resizeCalls;
	device->width = width;
	device->height = height;
	return !device->failResize;
}

void Shutdown(void *context)
{
	++static_cast<FakeDevice *>(context)->shutdownCalls;
}

bool ShutdownResult(void *context)
{
	FakeDevice *device = static_cast<FakeDevice *>(context);
	++device->shutdownResultCalls;
	return !device->failShutdown;
}

rts::render::NativeProductDeviceOperations Operations(FakeDevice *device)
{
	rts::render::NativeProductDeviceOperations operations;
	operations.context = device;
	operations.initialize = Initialize;
	operations.prepareResize = PrepareResize;
	operations.resize = Resize;
	operations.shutdown = Shutdown;
	return operations;
}

rts::render::NativeProductDeviceOperations ResultOperations(FakeDevice *device)
{
	rts::render::NativeProductDeviceOperations operations = Operations(device);
	operations.shutdownResult = ShutdownResult;
	return operations;
}

bool Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "Native product lifecycle test failed: %s\n", message);
		return false;
	}
	return true;
}
}

int main()
{
	FakeDevice device;
	rts::render::NativeProductDeviceLifecycle lifecycle;
	if (!Check(lifecycle.bind(Operations(&device)), "bind") ||
		!Check(lifecycle.state() ==
			rts::render::NativeProductDeviceLifecycle::READY, "ready state") ||
		!Check(!lifecycle.create(0, 720, true), "reject zero width") ||
		!Check(lifecycle.create(1280, 720, true), "create") ||
		!Check(lifecycle.isActive(), "active after create") ||
		!Check(device.initializeCalls == 1 && device.width == 1280 &&
			device.height == 720 && device.vsync, "create arguments") ||
		!Check(!lifecycle.create(640, 480, false), "reject duplicate create") ||
		!Check(lifecycle.reset(1920, 1080), "reset") ||
		!Check(device.prepareCalls == 1 && device.resizeCalls == 1 &&
			device.width == 1920 && device.height == 1080,
			"reset ordering and arguments"))
	{
		return 1;
	}
	lifecycle.shutdown();
	if (!Check(device.shutdownCalls == 1, "single shutdown") ||
		!Check(lifecycle.state() ==
			rts::render::NativeProductDeviceLifecycle::UNBOUND,
			"unbound after shutdown"))
	{
		return 1;
	}
	lifecycle.shutdown();
	if (!Check(device.shutdownCalls == 1, "idempotent shutdown"))
	{
		return 1;
	}

	FakeDevice failedInitialize;
	failedInitialize.failInitialize = true;
	if (!Check(lifecycle.bind(Operations(&failedInitialize)),
			"bind failed-initialize fixture") ||
		!Check(!lifecycle.create(800, 600, false), "failed initialize") ||
		!Check(lifecycle.state() ==
			rts::render::NativeProductDeviceLifecycle::READY,
			"failed initialize remains ready"))
	{
		return 1;
	}
	lifecycle.shutdown();
	if (!Check(failedInitialize.shutdownCalls == 0,
		"failed initialize does not own shutdown"))
	{
		return 1;
	}

	FakeDevice failedShutdown;
	if (!Check(lifecycle.bind(ResultOperations(&failedShutdown)),
			"bind failed-shutdown fixture") ||
		!Check(lifecycle.create(800, 600, false),
			"create failed-shutdown fixture"))
	{
		return 1;
	}
	failedShutdown.failShutdown = true;
	if (!Check(!lifecycle.shutdown(), "failed shutdown") ||
		!Check(lifecycle.state() ==
			rts::render::NativeProductDeviceLifecycle::ACTIVE,
			"failed shutdown retains active state") ||
		!Check(lifecycle.ownsDeviceResources(),
			"failed shutdown retains device-resource ownership") ||
		!Check(failedShutdown.shutdownResultCalls == 1 &&
			failedShutdown.shutdownCalls == 0,
			"result callback is authoritative"))
	{
		return 1;
	}
	failedShutdown.failShutdown = false;
	if (!Check(lifecycle.shutdown(), "retry shutdown") ||
		!Check(failedShutdown.shutdownResultCalls == 2,
			"retry invokes result callback") ||
		!Check(lifecycle.state() ==
			rts::render::NativeProductDeviceLifecycle::UNBOUND,
			"retry reaches unbound state"))
	{
		return 1;
	}

	FakeDevice failedResize;
	failedResize.failResize = true;
	if (!Check(lifecycle.bind(Operations(&failedResize)),
			"bind failed-resize fixture") ||
		!Check(lifecycle.create(800, 600, false),
			"create failed-resize fixture") ||
		!Check(!lifecycle.reset(1024, 768), "failed resize") ||
		!Check(lifecycle.state() ==
			rts::render::NativeProductDeviceLifecycle::LOST,
			"failed resize becomes lost") ||
		!Check(lifecycle.ownsDeviceResources(),
			"lost lifecycle retains device-resource ownership"))
	{
		return 1;
	}
	lifecycle.shutdown();
	return Check(failedResize.shutdownCalls == 1,
		"lost device shuts down exactly once") ? 0 : 1;
}
