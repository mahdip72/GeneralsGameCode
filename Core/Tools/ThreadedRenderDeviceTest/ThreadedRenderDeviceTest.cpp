#include "Renderer/ThreadedRenderDevice.h"
#include "Lib/JobSystem.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace rts::render;
namespace
{
#define CHECK(condition) do { if (!(condition)) { \
	std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
	throw std::runtime_error(#condition); } } while (false)

enum Event
{
	CREATED, INITIALIZED, CONTEXT, BEGIN, BUFFER, TEXTURE, REFRESH, COPY,
	DESTROY_RESOURCE, UPDATE, CLEAR, TARGETS, VIEWPORT, STATE, LAYOUT,
	VERTEX, INDEX, BIND_TEXTURE, TOPOLOGY, DRAW, DRAW_INDEXED, END, PRESENT,
	CAPTURE, INFO, RESIZE, RECOVER, DEBUG_COUNT, REPORT, SHUTDOWN, DELETED
};

struct Fixture
{
	Fixture() : gateEvent(-1), gateEntered(false), gateReleased(false),
		wrongThread(false), failCreate(false), failDraw(false), failEnd(false),
		failPresent(false), failCapture(false), failInitialize(false), failUpdate(false),
		createFailureResult(RENDER_RESULT_OUT_OF_MEMORY), updateFailureResult(RENDER_RESULT_DEVICE_REMOVED),
		factoryCalls(0), draws(0), presents(0), infos(0), destroys(0), failEndFrames(0),
		stateValue(0), layoutStride(0), layoutOffset(0), window(0), proxy(0),
		sentMessages(0), postedMessages(0), reentrantRejected(true)
	{ events.reserve(4096); }
	std::mutex mutex;
	std::condition_variable changed;
	std::thread::id owner;
	int gateEvent;
	bool gateEntered, gateReleased, wrongThread;
	bool failCreate, failDraw, failEnd, failPresent, failCapture, failInitialize, failUpdate;
	RenderResult createFailureResult, updateFailureResult;
	unsigned int factoryCalls, draws, presents, infos, destroys;
	unsigned int failEndFrames;
	float stateValue;
	unsigned int layoutStride, layoutOffset;
	RenderTargetBinding targets;
	void *window;
	IRenderDevice *proxy;
	unsigned int sentMessages, postedMessages;
	bool reentrantRejected;
	std::vector<int> events;
	std::vector<unsigned char> bufferBytes, updateBytes, textureBytes, refreshBytes;
	std::vector<GpuHandle> createdHandles, destroyedHandles;

	void event(int event)
	{
		std::unique_lock<std::mutex> lock(mutex);
		wrongThread = wrongThread || owner != std::this_thread::get_id();
		events.push_back(event);
		if (event == gateEvent && !gateEntered)
		{
			gateEntered = true; changed.notify_all();
			CHECK(changed.wait_for(lock, std::chrono::seconds(5), [this] { return gateReleased; }));
		}
	}
	void waitForGate()
	{
		std::unique_lock<std::mutex> lock(mutex);
		CHECK(changed.wait_for(lock, std::chrono::seconds(5), [this] { return gateEntered; }));
	}
	void release()
	{
		std::lock_guard<std::mutex> lock(mutex);
		gateReleased = true; changed.notify_all();
	}
	void sendWindowMessage()
	{
#ifdef _WIN32
		if (window) SendMessageW(static_cast<HWND>(window), WM_APP + 1, 0, 0);
#endif
	}
};

struct ReleaseGate
{
	explicit ReleaseGate(Fixture &fixture) : state(fixture) {}
	~ReleaseGate() { state.release(); }
	Fixture &state;
};

class FakeBackend final : public IRenderDevice, public IRenderContext
{
public:
	explicit FakeBackend(Fixture &fixture) : f(fixture), handles(64), operational(false), open(false)
	{
		f.owner = std::this_thread::get_id();
		++f.factoryCalls; f.event(CREATED);
		info.width = info.height = 4; info.format = RENDER_FORMAT_B8G8R8A8_UNORM;
	}
	~FakeBackend() override { f.event(DELETED); ++f.destroys; }
	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
	{ f.event(INITIALIZED); f.sendWindowMessage(); operational = !f.failInitialize; return operational ? RENDER_RESULT_OK : RENDER_RESULT_FAILED; }
	void shutdown() override { f.event(SHUTDOWN); f.sendWindowMessage(); operational = false; }
	IRenderContext *immediateContext() override { f.event(CONTEXT); return this; }
	RenderResult createBuffer(const BufferDescriptor &, const void *data, size_t bytes, GpuHandle *out) override
	{
		f.event(BUFFER);
		if (f.failCreate) return f.createFailureResult;
		f.bufferBytes.clear();
		if (bytes) f.bufferBytes.assign(static_cast<const unsigned char *>(data), static_cast<const unsigned char *>(data) + bytes);
		*out = handles.allocate(); f.createdHandles.push_back(*out); return RENDER_RESULT_OK;
	}
	void copyTexture(const TextureDescriptor &descriptor, const TextureSubresourceData *data,
		unsigned int count, std::vector<unsigned char> &output)
	{
		output.clear();
		for (unsigned int i = 0; i < count; ++i)
		{
			const unsigned int mip = i % descriptor.mipCount;
			const size_t height = (std::max)(1u, descriptor.height >> mip);
			const size_t bytes = (std::max)(data[i].slicePitch, data[i].rowPitch * height);
			const unsigned char *begin = static_cast<const unsigned char *>(data[i].data);
			output.insert(output.end(), begin, begin + bytes);
		}
	}
	RenderResult createTexture(const TextureDescriptor &descriptor, const TextureSubresourceData *data,
		unsigned int count, GpuHandle *out) override
	{
		f.event(TEXTURE); copyTexture(descriptor, data, count, f.textureBytes);
		*out = handles.allocate(); f.createdHandles.push_back(*out); return RENDER_RESULT_OK;
	}
	RenderResult refreshTexture(GpuHandle handle, const TextureDescriptor &descriptor,
		const TextureSubresourceData *data, unsigned int count) override
	{
		f.event(REFRESH); CHECK(handles.isLive(handle));
		copyTexture(descriptor, data, count, f.refreshBytes); return RENDER_RESULT_OK;
	}
	RenderResult copyActiveColorTargetToTexture(GpuHandle handle) override
	{ f.event(COPY); CHECK(open && handles.isLive(handle)); return RENDER_RESULT_OK; }
	bool destroyResource(GpuHandle handle) override
	{ f.event(DESTROY_RESOURCE); f.destroyedHandles.push_back(handle); return handles.release(handle); }
	RenderResult recoverDevice() override { f.event(RECOVER); operational = true; return RENDER_RESULT_OK; }
	RenderResult resize(unsigned int width, unsigned int height) override
	{
		f.event(RESIZE); f.sendWindowMessage(); CHECK(!open);
		if (width && height) { info.width = width; info.height = height; }
		return RENDER_RESULT_OK;
	}
	RenderResult present() override
	{
		f.event(PRESENT); f.sendWindowMessage(); CHECK(!open);
		if (f.failPresent) return RENDER_RESULT_DEVICE_REMOVED;
		++f.presents; return RENDER_RESULT_OK;
	}
	RenderResult getBackBufferInfo(RenderBackBufferInfo *output) const override
	{ f.event(INFO); ++f.infos; *output = info; return RENDER_RESULT_OK; }
	RenderResult captureBackBuffer(void *destination, size_t bytes, size_t rowPitch, RenderFormat *format) override
	{
		f.event(CAPTURE); CHECK(!open && bytes >= rowPitch * info.height);
		if (f.failCapture) return RENDER_RESULT_FAILED;
		std::memset(destination, 77, bytes); *format = info.format; return RENDER_RESULT_OK;
	}
	RenderResult getDebugValidationErrorCount(unsigned int *count) const override
	{ f.event(DEBUG_COUNT); *count = 7; return RENDER_RESULT_OK; }
	RenderResult reportDebugLiveObjects() override { f.event(REPORT); return RENDER_RESULT_OK; }
	RenderResult beginFrame() override { f.event(BEGIN); CHECK(!open); open = true; return RENDER_RESULT_OK; }
	RenderResult updateBuffer(GpuHandle handle, const void *data, size_t bytes, size_t, RenderBufferUpdateMode) override
	{
		f.event(UPDATE); CHECK(open && handles.isLive(handle));
		if (f.failUpdate) return f.updateFailureResult;
		f.updateBytes.assign(static_cast<const unsigned char *>(data), static_cast<const unsigned char *>(data) + bytes);
		return RENDER_RESULT_OK;
	}
	RenderResult clear(const RenderFloat4 &color, float depth, unsigned int stencil) override
	{ return clearTargets(7, color, depth, stencil); }
	RenderResult clearTargets(unsigned int, const RenderFloat4 &, float, unsigned int) override
	{ f.event(CLEAR); CHECK(open); return RENDER_RESULT_OK; }
	RenderResult setRenderTargets(const RenderTargetBinding &binding) override
	{
		f.event(TARGETS); CHECK(open);
		CHECK(!(binding.hasColor && binding.useBackBufferColor));
		CHECK(!(binding.hasDepth && binding.useBackBufferDepth));
		if (binding.hasColor) CHECK(handles.isLive(binding.color.resource));
		if (binding.hasDepth) CHECK(handles.isLive(binding.depth.resource));
		f.targets = binding; return RENDER_RESULT_OK;
	}
	RenderResult setRenderTargets(GpuHandle, GpuHandle) override { throw std::runtime_error("untranslated target overload"); }
	RenderResult setViewport(float, float, float, float, float, float) override
	{ f.event(VIEWPORT); CHECK(open); return RENDER_RESULT_OK; }
	RenderResult setLegacyState(const LegacyLogicalState &state, LegacyVertexFormat, unsigned int) override
	{ f.event(STATE); f.stateValue = state.constants.world.values[0]; return RENDER_RESULT_OK; }
	RenderResult setLegacyStateForLayout(const LegacyLogicalState &state, const LegacyVertexLayout &layout, unsigned int) override
	{
		f.event(LAYOUT); f.stateValue = state.constants.world.values[0];
		f.layoutStride = layout.stride; f.layoutOffset = layout.elements[0].byteOffset; return RENDER_RESULT_OK;
	}
	RenderResult setVertexBuffer(GpuHandle handle, unsigned int, unsigned int) override
	{ f.event(VERTEX); CHECK(!handle.isValid() || handles.isLive(handle)); return RENDER_RESULT_OK; }
	RenderResult setIndexBuffer(GpuHandle handle, RenderFormat, unsigned int) override
	{ f.event(INDEX); CHECK(!handle.isValid() || handles.isLive(handle)); return RENDER_RESULT_OK; }
	RenderResult setTexture(unsigned int, GpuHandle handle) override
	{ f.event(BIND_TEXTURE); CHECK(!handle.isValid() || handles.isLive(handle)); return RENDER_RESULT_OK; }
	RenderResult setPrimitiveTopology(RenderPrimitiveTopology) override
	{ f.event(TOPOLOGY); return RENDER_RESULT_OK; }
	RenderResult draw(unsigned int, unsigned int) override
	{ f.event(DRAW); CHECK(open); ++f.draws; return f.failDraw ? RENDER_RESULT_FAILED : RENDER_RESULT_OK; }
	RenderResult drawIndexed(unsigned int, unsigned int, int) override
	{ f.event(DRAW_INDEXED); CHECK(open); return RENDER_RESULT_OK; }
	RenderResult endFrame() override
	{
		f.event(END); CHECK(open); open = false;
		if (f.failEndFrames) { --f.failEndFrames; return RENDER_RESULT_FAILED; }
		return f.failEnd ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
	}
private:
	Fixture &f;
	GpuHandleAllocator handles;
	bool operational, open;
	RenderBackBufferInfo info;
};

IRenderDevice *Factory(void *state) { return new FakeBackend(*static_cast<Fixture *>(state)); }
std::unique_ptr<IRenderDevice> Device(Fixture &fixture, const ThreadedRenderOptions &options = ThreadedRenderOptions())
{
	std::unique_ptr<IRenderDevice> device(CreateThreadedRenderDevice(Factory, &fixture, options));
	CHECK(device.get() != 0 && IsThreadedRenderDevice(device.get()));
	RenderDeviceParameters parameters; parameters.backend = RENDER_BACKEND_D3D11;
	CHECK(device->initialize(parameters) == RENDER_RESULT_OK);
	return device;
}
ThreadedRenderFrameCompletion Complete(IRenderDevice *device, RenderResult expected = RENDER_RESULT_OK)
{
	CHECK(DrainThreadedRenderDevice(device) == expected);
	ThreadedRenderFrameCompletion completion;
	CHECK(PollThreadedRenderCompletion(device, &completion));
	CHECK(completion.result == expected);
	CHECK(!PollThreadedRenderCompletion(device, &completion));
	return completion;
}
void EmptyFrame(IRenderDevice *device, bool visible = true)
{
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(SubmitThreadedRenderFrame(device, visible) == RENDER_RESULT_OK);
}

void OwnershipAndDeepCopy()
{
	Fixture f;
	auto device = Device(f);
	ReleaseGate release(f);
	f.gateEvent = BEGIN;
	IRenderContext *context = device->immediateContext();
	RenderResult offOwner = RENDER_RESULT_OK;
	std::thread rejectedProducer([&] { offOwner = context->beginFrame(); });
	rejectedProducer.join();
	CHECK(offOwner == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	unsigned char bytes[16]; std::memset(bytes, 11, sizeof(bytes));
	BufferDescriptor buffer; buffer.byteCount = sizeof(bytes); buffer.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle vertex, index, texture;
	CHECK(device->createBuffer(buffer, bytes, sizeof(bytes), &vertex) == RENDER_RESULT_OK);
	buffer.binding = RENDER_BUFFER_INDEX;
	CHECK(device->createBuffer(buffer, bytes, sizeof(bytes), &index) == RENDER_RESULT_OK);
	std::memset(bytes, 22, sizeof(bytes));
	CHECK(context->updateBuffer(vertex, bytes, sizeof(bytes), 0, RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK);
	unsigned char top[16], mip[4]; std::memset(top, 33, sizeof(top)); std::memset(mip, 44, sizeof(mip));
	TextureDescriptor descriptor; descriptor.width = descriptor.height = 2; descriptor.mipCount = 2;
	descriptor.format = RENDER_FORMAT_B8G8R8A8_UNORM; descriptor.usage = RENDER_USAGE_DEFAULT;
	TextureSubresourceData subresources[2];
	subresources[0].data = top; subresources[0].rowPitch = 8; subresources[0].slicePitch = 16;
	subresources[1].data = mip; subresources[1].rowPitch = 4; subresources[1].slicePitch = 4;
	CHECK(device->createTexture(descriptor, subresources, 2, &texture) == RENDER_RESULT_OK);
	std::memset(top, 55, sizeof(top)); std::memset(mip, 66, sizeof(mip));
	CHECK(device->refreshTexture(texture, descriptor, subresources, 2) == RENDER_RESULT_OK);
	CHECK(context->clear(RenderFloat4(1, 2, 3, 4), 1, 0) == RENDER_RESULT_OK);
	CHECK(context->clearTargets(RENDER_CLEAR_COLOR, RenderFloat4(), 1, 0) == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(texture, GpuHandle()) == RENDER_RESULT_OK);
	CHECK(context->setViewport(0, 0, 4, 4, 0, 1) == RENDER_RESULT_OK);
	LegacyLogicalState state; state.constants.world.values[0] = 9;
	CHECK(context->setLegacyState(state, RENDER_VERTEX_POSITION3_COLOR, 0) == RENDER_RESULT_OK);
	LegacyVertexLayout layout; layout.stride = 24; layout.elementCount = 1; layout.elements[0].byteOffset = 12;
	CHECK(context->setLegacyStateForLayout(state, layout, 0) == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(vertex, sizeof(unsigned int), 0) == RENDER_RESULT_OK);
	CHECK(context->setIndexBuffer(index, RENDER_FORMAT_R16_UINT, 0) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, texture) == RENDER_RESULT_OK);
	CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK);
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->drawIndexed(3, 0, -1) == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK);
	CHECK(device->copyActiveColorTargetToTexture(texture) == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(texture, GpuHandle()) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	f.waitForGate();
	std::memset(bytes, 99, sizeof(bytes)); std::memset(top, 99, sizeof(top)); std::memset(mip, 99, sizeof(mip));
	std::memset(subresources, 0, sizeof(subresources));
	state.constants.world.values[0] = 99; layout.stride = 99; layout.elements[0].byteOffset = 99;
	f.release();
	const ThreadedRenderFrameCompletion completion = Complete(device.get());
	CHECK(completion.presented && completion.outcome.wasPresented() && completion.outcome.frameEnded());
	CHECK(f.owner != std::this_thread::get_id() && !f.wrongThread);
	CHECK(f.bufferBytes == std::vector<unsigned char>(16, 11));
	CHECK(f.updateBytes == std::vector<unsigned char>(16, 22));
	CHECK(f.textureBytes.size() == 20 && f.textureBytes.front() == 33 && f.textureBytes.back() == 44);
	CHECK(f.refreshBytes.size() == 20 && f.refreshBytes.front() == 55 && f.refreshBytes.back() == 66);
	CHECK(f.stateValue == 9 && f.layoutStride == 24 && f.layoutOffset == 12);
	CHECK(f.targets.hasColor && !f.targets.useBackBufferColor && !f.targets.hasDepth && !f.targets.useBackBufferDepth);
	unsigned int count = 0;
	CHECK(device->getDebugValidationErrorCount(&count) == RENDER_RESULT_OK && count == 7);
	CHECK(device->reportDebugLiveObjects() == RENDER_RESULT_OK);
	const unsigned int infoCalls = f.infos;
	RenderBackBufferInfo info;
	for (unsigned int i = 0; i < 20; ++i)
		CHECK(device->isOperational() && device->getBackBufferInfo(&info) == RENDER_RESULT_OK);
	CHECK(f.infos == infoCalls);
	device.reset();
	CHECK(!f.wrongThread && f.destroys == 1 && f.events.back() == DELETED);
}

void SynchronousProducerRejectionsDoNotPoisonNextFrame()
{
	{
		Fixture f;
		auto device = Device(f);
		BufferDescriptor invalid;
		GpuHandle rejected;
		CHECK(device->createBuffer(invalid, 0, 0, &rejected) ==
			RENDER_RESULT_INVALID_ARGUMENT);
		CHECK(!rejected.isValid());

		BufferDescriptor descriptor;
		descriptor.byteCount = 4; descriptor.usage = RENDER_USAGE_DEFAULT;
		unsigned int value = 0x12345678;
		GpuHandle valid;
		CHECK(device->createBuffer(descriptor, &value, sizeof(value), &valid) ==
			RENDER_RESULT_OK);
		EmptyFrame(device.get());
		CHECK(Complete(device.get()).presented);
	}
	{
		Fixture f;
		ThreadedRenderOptions options; options.resourceCapacity = 1;
		auto device = Device(f, options);
		BufferDescriptor descriptor;
		descriptor.byteCount = 4; descriptor.usage = RENDER_USAGE_DEFAULT;
		unsigned int value = 0x12345678;
		GpuHandle first, rejected, retry;
		CHECK(device->createBuffer(descriptor, &value, sizeof(value), &first) ==
			RENDER_RESULT_OK);
		CHECK(device->createBuffer(descriptor, &value, sizeof(value), &rejected) ==
			RENDER_RESULT_OUT_OF_MEMORY);
		CHECK(!rejected.isValid());
		CHECK(device->destroyResource(first));
		CHECK(device->createBuffer(descriptor, &value, sizeof(value), &retry) ==
			RENDER_RESULT_OK);
		EmptyFrame(device.get());
		CHECK(Complete(device.get()).presented);
	}
}

void GenerationsAndResourceFailure()
{
	Fixture f;
	ThreadedRenderOptions options; options.resourceCapacity = 1;
	auto device = Device(f, options);
	BufferDescriptor descriptor; descriptor.byteCount = 4; descriptor.usage = RENDER_USAGE_DEFAULT;
	unsigned int value = 0x12345678;
	GpuHandle first, second;
	CHECK(device->createBuffer(descriptor, &value, sizeof(value), &first) == RENDER_RESULT_OK);
	CHECK(device->destroyResource(first));
	CHECK(device->createBuffer(descriptor, &value, sizeof(value), &second) == RENDER_RESULT_OK);
	CHECK(first.index() == second.index() && first.generation() != second.generation());
	CHECK(!device->destroyResource(first));
	EmptyFrame(device.get(), false);
	CHECK(!Complete(device.get()).presented);
	CHECK(f.createdHandles.size() == 2 && f.destroyedHandles.size() == 1);
	CHECK(f.createdHandles[0] == f.destroyedHandles[0]);
	CHECK(device->immediateContext()->beginFrame() == RENDER_RESULT_OK);
	CHECK(device->immediateContext()->setVertexBuffer(first, 4, 0) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->immediateContext()->endFrame() == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get(), RENDER_RESULT_INVALID_ARGUMENT).outcome.hasCommandFailure());
	CHECK(device->destroyResource(second));
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
	f.failCreate = true;
	CHECK(device->createBuffer(descriptor, &value, sizeof(value), &first) == RENDER_RESULT_OK);
	EmptyFrame(device.get());
	const ThreadedRenderFrameCompletion failed = Complete(device.get(), RENDER_RESULT_OUT_OF_MEMORY);
	CHECK(failed.outcome.hasCommandFailure() && failed.resourceFailure);
	CHECK(f.presents == 0);
}

void FailurePublicationAndRecovery()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	f.failDraw = true;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	ThreadedRenderFrameCompletion completion = Complete(device.get(), RENDER_RESULT_FAILED);
	CHECK(completion.outcome.hasCommandFailure() && !completion.presented && f.draws == 1);
	f.failDraw = false; EmptyFrame(device.get()); CHECK(Complete(device.get()).presented);
	f.failEnd = true; EmptyFrame(device.get());
	completion = Complete(device.get(), RENDER_RESULT_FAILED);
	CHECK(!completion.outcome.hasCommandFailure() && completion.outcome.endFrameResult() == RENDER_RESULT_FAILED);
	CHECK(!completion.presented);
	f.failEnd = false; f.failPresent = true; EmptyFrame(device.get());
	completion = Complete(device.get(), RENDER_RESULT_DEVICE_REMOVED);
	CHECK(completion.outcome.hasDeviceRemoval() && completion.outcome.presentationResult() == RENDER_RESULT_DEVICE_REMOVED);
	CHECK(!completion.operational && !device->isOperational());
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
	CHECK(!device->isOperational()); // A benign fence must not hide device loss.
	f.failPresent = false;
	CHECK(device->recoverDevice() == RENDER_RESULT_OK && device->isOperational());
	EmptyFrame(device.get()); CHECK(Complete(device.get()).presented);
}

void BufferUpdateFailureRecoveryRestoresBinding()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	BufferDescriptor descriptor;
	descriptor.byteCount = 16;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	descriptor.binding = RENDER_BUFFER_VERTEX;
	unsigned char bytes[16]; std::memset(bytes, 31, sizeof(bytes));
	GpuHandle buffer;
	CHECK(device->createBuffer(descriptor, bytes, sizeof(bytes), &buffer) == RENDER_RESULT_OK);
	f.failUpdate = true; f.updateFailureResult = RENDER_RESULT_DEVICE_REMOVED;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, bytes, sizeof(bytes), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
		RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	const ThreadedRenderFrameCompletion failed = Complete(device.get(), RENDER_RESULT_DEVICE_REMOVED);
	// A failed native buffer update invalidates current contents.
	CHECK(failed.resourceFailure && !device->isOperational());
	const std::size_t updates = static_cast<std::size_t>(std::count(f.events.begin(), f.events.end(), UPDATE));
	// Explicit recovery recreates the device after a failed buffer map/update.
	CHECK(device->recoverDevice() == RENDER_RESULT_OK && device->isOperational());
	f.failUpdate = false;
	// Aggregate mutation failure cleared initialized-range authority. Republish
	// bytes before the resource can be bound again after recovery.
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, bytes, sizeof(bytes), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(buffer, 16, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	const ThreadedRenderFrameCompletion recovered = Complete(device.get());
	// The recovered buffer binds only after a replacement upload republishes it.
	CHECK(recovered.result == RENDER_RESULT_OK && recovered.presented
		&& static_cast<std::size_t>(std::count(f.events.begin(), f.events.end(), UPDATE)) == updates + 1);
}

void ResourcePreambleRemovalRemainsObservable()
{
	Fixture f;
	auto device = Device(f);
	f.failCreate = true; f.createFailureResult = RENDER_RESULT_DEVICE_REMOVED;
	BufferDescriptor descriptor; descriptor.byteCount = 4; descriptor.usage = RENDER_USAGE_DEFAULT;
	unsigned int bytes = 17; GpuHandle handle;
	CHECK(device->createBuffer(descriptor, &bytes, sizeof(bytes), &handle) == RENDER_RESULT_OK);
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_DEVICE_REMOVED);
	CHECK(!device->isOperational());
	ThreadedRenderFrameCompletion completion;
	CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_DEVICE_REMOVED);
	CHECK(device->recoverDevice() == RENDER_RESULT_OK && device->isOperational());
	f.failCreate = false;
	CHECK(device->destroyResource(handle));
	EmptyFrame(device.get());
	CHECK(Complete(device.get()).presented);
}

void RecoveryClearsPreambleResourceFailure()
{
	Fixture f;
	auto device = Device(f);
	f.failCreate = true; f.createFailureResult = RENDER_RESULT_DEVICE_REMOVED;
	BufferDescriptor descriptor; descriptor.byteCount = 4; descriptor.usage = RENDER_USAGE_DEFAULT;
	unsigned int bytes = 17; GpuHandle handle;
	CHECK(device->createBuffer(descriptor, &bytes, sizeof(bytes), &handle) == RENDER_RESULT_OK);
	// Explicit recovery can follow a failed resource preamble without a prior drain.
	CHECK(device->recoverDevice() == RENDER_RESULT_OK && device->isOperational());
	f.failCreate = false;
	EmptyFrame(device.get());
	const ThreadedRenderFrameCompletion completion = Complete(device.get());
	// Successful recovery consumes the failed preamble resource latch; a later
	// successful frame must not invalidate newly republished resources.
	CHECK(completion.result == RENDER_RESULT_OK && completion.presented &&
		!completion.resourceFailure);
	CHECK(device->destroyResource(handle));
	// Reclaim the failed preamble handle after its resource failure is observed.
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
}

void CaptureResizeAndNonVisibleOrdering()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->clear(RenderFloat4(), 1, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	unsigned char pixels[64]; std::memset(pixels, 0, sizeof(pixels));
	RenderFormat format = RENDER_FORMAT_UNKNOWN;
	CHECK(device->captureBackBuffer(pixels, sizeof(pixels), 16, &format) == RENDER_RESULT_OK);
	CHECK(pixels[0] == 77 && pixels[63] == 77 && format == RENDER_FORMAT_B8G8R8A8_UNORM);
	CHECK(f.presents == 0);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
	const auto end = std::find(f.events.begin(), f.events.end(), END);
	const auto capture = std::find(f.events.begin(), f.events.end(), CAPTURE);
	const auto present = std::find(f.events.begin(), f.events.end(), PRESENT);
	CHECK(end < capture && capture < present);
	CHECK(device->resize(8, 6) == RENDER_RESULT_OK);
	RenderBackBufferInfo info;
	CHECK(device->getBackBufferInfo(&info) == RENDER_RESULT_OK && info.width == 8 && info.height == 6);
	CHECK(device->resize(0, 0) == RENDER_RESULT_OK);
	CHECK(device->getBackBufferInfo(&info) == RENDER_RESULT_OK && info.width == 8 && info.height == 6);
	CHECK(device->resize(4, 4) == RENDER_RESULT_OK);
	f.failCapture = true;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	std::memset(pixels, 19, sizeof(pixels)); format = RENDER_FORMAT_UNKNOWN;
	CHECK(device->captureBackBuffer(pixels, sizeof(pixels), 16, &format) == RENDER_RESULT_FAILED);
	CHECK(pixels[0] == 19 && format == RENDER_FORMAT_UNKNOWN);
	CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
	const ThreadedRenderFrameCompletion completion = Complete(device.get(), RENDER_RESULT_FAILED);
	CHECK(!completion.presented && completion.outcome.captureResult() == RENDER_RESULT_FAILED);
}

void OpenFrameReportIsRejectedBeforeExecution()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	const std::size_t reports = static_cast<std::size_t>(std::count(f.events.begin(), f.events.end(), REPORT));
	// Debug live-object reports are rejected while a frame is open.
	CHECK(device->reportDebugLiveObjects() == RENDER_RESULT_INVALID_ARGUMENT);
	// A rejected open-frame report does not execute on the render owner.
	CHECK(static_cast<std::size_t>(std::count(f.events.begin(), f.events.end(), REPORT)) == reports);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
}

void FailedGpuCopyDependencies()
{
	Fixture f;
	auto device = Device(f);
	ReleaseGate release(f); f.gateEvent = BEGIN; f.failEndFrames = 1;
	TextureDescriptor descriptor; descriptor.width = descriptor.height = 4;
	descriptor.format = RENDER_FORMAT_B8G8R8A8_UNORM; descriptor.usage = RENDER_USAGE_DEFAULT;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE | RENDER_TEXTURE_RENDER_TARGET;
	GpuHandle texture;
	CHECK(device->createTexture(descriptor, 0, 0, &texture) == RENDER_RESULT_OK);
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(device->copyActiveColorTargetToTexture(texture) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK); f.waitForGate();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, texture) == RENDER_RESULT_OK);
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	f.release();
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_FAILED);
	ThreadedRenderFrameCompletion first, dependent;
	CHECK(PollThreadedRenderCompletion(device.get(), &first));
	CHECK(PollThreadedRenderCompletion(device.get(), &dependent));
	CHECK(first.outcome.endFrameResult() == RENDER_RESULT_FAILED && first.resourceFailure);
	CHECK(dependent.outcome.hasCommandFailure() && dependent.resourceFailure && !dependent.presented);
	CHECK(std::find(f.events.begin(), f.events.end(), BIND_TEXTURE) == f.events.end());
	CHECK(f.presents == 0);
	unsigned char pixels[64]; std::memset(pixels, 42, sizeof(pixels));
	TextureSubresourceData data; data.data = pixels; data.rowPitch = 16; data.slicePitch = sizeof(pixels);
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(device->refreshTexture(texture, descriptor, &data, 1) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, texture) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented); // complete upload clears poisoned provenance
	f.failDraw = true;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(device->copyActiveColorTargetToTexture(texture) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, texture) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_FAILED);
	CHECK(PollThreadedRenderCompletion(device.get(), &first));
	CHECK(PollThreadedRenderCompletion(device.get(), &dependent));
	CHECK(first.resourceFailure && dependent.resourceFailure && !dependent.presented);
	f.failDraw = false;
	// A full clear, unlike an arbitrary partial draw, also establishes valid
	// contents for a failed render-to-texture output.
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(texture, GpuHandle()) == RENDER_RESULT_OK);
	CHECK(context->clearTargets(RENDER_CLEAR_COLOR, RenderFloat4(), 1, 0) == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, texture) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
}

void OverlapAndBoundedBackpressure()
{
	Fixture f;
	ThreadedRenderOptions options; options.maxFramesInFlight = 2;
	auto device = Device(f, options);
	ReleaseGate release(f); f.gateEvent = BEGIN;
	EmptyFrame(device.get()); f.waitForGate();
	// The owner is blocked inside frame N, but N+1 can be completely recorded
	// and submitted. A per-draw/end/present RPC would deadlock this assertion.
	EmptyFrame(device.get());
	ThreadedRenderMetrics metrics;
	CHECK(GetThreadedRenderMetrics(device.get(), &metrics) && metrics.submittedFrames == 2);
	CHECK(metrics.completedFrames == 0 && metrics.pendingPackets == 2 && metrics.producerOverlapFrames >= 1);
	std::thread unblock([&]
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		ThreadedRenderMetrics snapshot;
		while (std::chrono::steady_clock::now() < deadline)
		{
			GetThreadedRenderMetrics(device.get(), &snapshot);
			if (snapshot.backpressureWaits) break;
			std::this_thread::yield();
		}
		f.release();
	});
	const RenderResult beginResult = device->immediateContext()->beginFrame();
	unblock.join();
	CHECK(beginResult == RENDER_RESULT_OK);
	CHECK(CancelThreadedRenderFrame(device.get()) == RENDER_RESULT_OK);
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_FAILED);
	ThreadedRenderFrameCompletion completion;
	uint64_t sequence = 0; unsigned int completed = 0;
	while (PollThreadedRenderCompletion(device.get(), &completion))
	{
		CHECK(completion.sequence > sequence); sequence = completion.sequence; ++completed;
	}
	CHECK(completed == 3);
	CHECK(GetThreadedRenderMetrics(device.get(), &metrics));
	CHECK(metrics.backpressureWaits >= 1 && metrics.peakPendingPackets <= 2 && metrics.producerWaitNanoseconds > 0);
}

void PacketSegmentationAndBudgetFailure()
{
	Fixture f;
	ThreadedRenderOptions options; options.maxPacketBytes = 128; options.maxPacketCommands = 2;
	auto device = Device(f, options);
	BufferDescriptor descriptor; descriptor.byteCount = 64; descriptor.usage = RENDER_USAGE_DEFAULT;
	unsigned char bytes[64]; std::memset(bytes, 17, sizeof(bytes));
	GpuHandle handles[3];
	for (unsigned int i = 0; i < 3; ++i)
		CHECK(device->createBuffer(descriptor, bytes, sizeof(bytes), &handles[i]) == RENDER_RESULT_OK);
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	for (unsigned int i = 0; i < 9; ++i) CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented && f.draws == 9);
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	LegacyLogicalState state;
	CHECK(context->setLegacyState(state, RENDER_VERTEX_POSITION3_COLOR, 0) == RENDER_RESULT_OUT_OF_MEMORY);
	CHECK(CancelThreadedRenderFrame(device.get(), RENDER_RESULT_OUT_OF_MEMORY) == RENDER_RESULT_OK);
	CHECK(Complete(device.get(), RENDER_RESULT_OUT_OF_MEMORY).outcome.hasCommandFailure());
	for (unsigned int i = 0; i < 3; ++i) CHECK(device->destroyResource(handles[i]));
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
	CHECK(f.destroyedHandles.size() == 3);
}

void CompletionAdmissionAndShutdown()
{
	Fixture f;
	auto device = Device(f);
	for (unsigned int i = 0; i < 64; ++i) EmptyFrame(device.get());
	CHECK(device->immediateContext()->beginFrame() == RENDER_RESULT_OUT_OF_MEMORY);
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
	ThreadedRenderFrameCompletion completion; unsigned int count = 0;
	while (PollThreadedRenderCompletion(device.get(), &completion))
	{
		CHECK(completion.sequence == ++count && completion.result == RENDER_RESULT_OK);
	}
	CHECK(count == 64);
	CHECK(device->immediateContext()->beginFrame() == RENDER_RESULT_OK);
	CHECK(device->immediateContext()->draw(3, 0) == RENDER_RESULT_OK);
	device->shutdown(); // recording cancellation has a reserved completion slot
	CHECK(PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(completion.result == RENDER_RESULT_FAILED && !completion.presented);
	CHECK(f.destroys == 1 && !f.wrongThread);
	device->shutdown();
	CHECK(f.destroys == 1);
}

void ShutdownWithQueuedFrames()
{
	Fixture f;
	ThreadedRenderOptions options; options.maxFramesInFlight = 2;
	auto device = Device(f, options);
	ReleaseGate release(f); f.gateEvent = BEGIN;
	EmptyFrame(device.get()); f.waitForGate(); EmptyFrame(device.get());
	ThreadedRenderMetrics metrics;
	CHECK(GetThreadedRenderMetrics(device.get(), &metrics) && metrics.pendingPackets == 2);
	std::thread unblock([&] { f.release(); });
	device->shutdown(); unblock.join();
	ThreadedRenderFrameCompletion completion;
	CHECK(PollThreadedRenderCompletion(device.get(), &completion) && completion.sequence == 1 && completion.presented);
	CHECK(PollThreadedRenderCompletion(device.get(), &completion) && completion.sequence == 2 && completion.presented);
	CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(f.destroys == 1 && !f.wrongThread && f.events.back() == DELETED);
}

void SerialAndInitializationFailure()
{
	Fixture f;
	ThreadedRenderOptions options; options.serial = true;
	auto device = Device(f, options);
	EmptyFrame(device.get());
	ThreadedRenderFrameCompletion completion;
	CHECK(PollThreadedRenderCompletion(device.get(), &completion) && completion.presented);
	CHECK(f.owner != std::this_thread::get_id());
	Fixture rejected;
	std::unique_ptr<IRenderDevice> second(CreateThreadedRenderDevice(Factory, &rejected));
	RenderDeviceParameters parameters; parameters.backend = RENDER_BACKEND_D3D11;
	CHECK(second->initialize(parameters) == RENDER_RESULT_FAILED); // one render owner
	second.reset(); CHECK(rejected.factoryCalls == 0);
	device.reset();
	Fixture failed; failed.failInitialize = true;
	std::unique_ptr<IRenderDevice> bad(CreateThreadedRenderDevice(Factory, &failed));
	CHECK(bad->initialize(parameters) == RENDER_RESULT_FAILED);
	bad.reset(); CHECK(failed.destroys == 1 && !failed.wrongThread);
}

#ifdef _WIN32
LRESULT CALLBACK ContractWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	Fixture *fixture = reinterpret_cast<Fixture *>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (fixture && message == WM_APP + 1)
	{
		++fixture->sentMessages;
		fixture->reentrantRejected = fixture->reentrantRejected &&
			fixture->proxy->resize(1, 1) == RENDER_RESULT_INVALID_ARGUMENT;
		return 0;
	}
	if (fixture && message == WM_APP + 2) { ++fixture->postedMessages; return 0; }
	return DefWindowProcW(window, message, wparam, lparam);
}

void SentMessageOnlyLifecycleWaits()
{
	Fixture f;
	const wchar_t *name = L"ThreadedRenderOwnerContractWindow";
	WNDCLASSW windowClass = {};
	windowClass.lpfnWndProc = ContractWindowProcedure;
	windowClass.hInstance = GetModuleHandleW(0);
	windowClass.lpszClassName = name;
	CHECK(RegisterClassW(&windowClass));
	HWND window = CreateWindowExW(0, name, L"", 0, 0, 0, 4, 4, HWND_MESSAGE, 0, windowClass.hInstance, 0);
	CHECK(window != 0);
	SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&f));
	f.window = window;
	ThreadedRenderOptions options; options.serial = true;
	std::unique_ptr<IRenderDevice> device(CreateThreadedRenderDevice(Factory, &f, options));
	CHECK(device.get() != 0); f.proxy = device.get();
	CHECK(PostMessageW(window, WM_APP + 2, 0, 0));
	RenderDeviceParameters parameters; parameters.backend = RENDER_BACKEND_D3D11;
	CHECK(device->initialize(parameters) == RENDER_RESULT_OK);
	EmptyFrame(device.get());
	CHECK(device->resize(8, 8) == RENDER_RESULT_OK);
	device->shutdown();
	CHECK(f.sentMessages == 4 && f.postedMessages == 0 && f.reentrantRejected);
	MSG message;
	CHECK(PeekMessageW(&message, window, WM_APP + 2, WM_APP + 2, PM_REMOVE));
	DispatchMessageW(&message);
	CHECK(f.postedMessages == 1);
	device.reset();
	CHECK(DestroyWindow(window));
	CHECK(UnregisterClassW(name, windowClass.hInstance));
}
#endif
}

int main()
{
	try
	{
		CHECK(rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_GAME));
		OwnershipAndDeepCopy();
		SynchronousProducerRejectionsDoNotPoisonNextFrame();
		GenerationsAndResourceFailure();
		FailurePublicationAndRecovery();
		BufferUpdateFailureRecoveryRestoresBinding();
		ResourcePreambleRemovalRemainsObservable();
		RecoveryClearsPreambleResourceFailure();
		CaptureResizeAndNonVisibleOrdering();
		OpenFrameReportIsRejectedBeforeExecution();
		FailedGpuCopyDependencies();
		OverlapAndBoundedBackpressure();
		PacketSegmentationAndBudgetFailure();
		CompletionAdmissionAndShutdown();
		ShutdownWithQueuedFrames();
		SerialAndInitializationFailure();
#ifdef _WIN32
		SentMessageOnlyLifecycleWaits();
#endif
		CHECK(rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_GAME));
		std::puts("Threaded render-owner contracts passed");
		return 0;
	}
	catch (const std::exception &error)
	{
		std::fprintf(stderr, "Threaded renderer contract failure: %s\n", error.what());
		return 1;
	}
}
