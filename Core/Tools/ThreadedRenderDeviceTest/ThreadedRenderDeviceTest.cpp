#include "Renderer/ThreadedRenderDevice.h"
#include "Lib/JobSystem.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "Lib/FrameTimingDiagnostics.h"
#include "RenderPipelineStallTrace.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
	CAPTURE, INFO, FILTER_CAPS, RESIZE, RECOVER, DEBUG_COUNT, REPORT, SWAP_SET, SWAP_GET,
	GAMMA_SET, GAMMA_GET, FAULT_CONFIG, RESOURCE_STATS, SHUTDOWN, DELETED
};

struct Fixture
{
	Fixture() : busyDraw(false), busyEntered(false), busyRelease(false),
		gateEvent(-1), gateEntered(false), gateReleased(false),
		wrongThread(false), failCreate(false), failDraw(false), failEnd(false),
		failPresent(false), failCapture(false), failInitialize(false), failUpdate(false),
		failTopology(false),
		createFailureResult(RENDER_RESULT_OUT_OF_MEMORY), updateFailureResult(RENDER_RESULT_DEVICE_REMOVED),
		factoryCalls(0), draws(0), presents(0), infos(0),
		textureFilterCapabilityCalls(0), reportedMaxAnisotropy(16),
		destroys(0), failEndFrames(0),
		stateValue(0), layoutStride(0), layoutOffset(0), window(0), proxy(0),
		swapIntervalSetCalls(0), swapIntervalGetCalls(0),
		gammaSetCalls(0), gammaGetCalls(0), faultConfigCalls(0),
		resourceStatisticsCalls(0), lastFaultPoint(RENDER_RESOURCE_FAULT_NONE),
		lastFaultInvocation(0), lastFaultResult(RENDER_RESULT_FAILED),
		sentMessages(0), postedMessages(0), reentrantRejected(true)
	{ events.reserve(4096); }
	std::mutex mutex;
	std::condition_variable changed;
	std::thread::id owner;
	// CPU-only benchmark context, inactive in ordinary contract tests.
	bool busyDraw;
	std::atomic<bool> busyEntered, busyRelease;
	int gateEvent;
	bool gateEntered, gateReleased, wrongThread;
	bool failCreate, failDraw, failEnd, failPresent, failCapture, failInitialize,
		failUpdate, failTopology;
	RenderResult createFailureResult, updateFailureResult;
	unsigned int factoryCalls, draws, presents, infos, textureFilterCapabilityCalls;
	unsigned int reportedMaxAnisotropy, destroys;
	unsigned int failEndFrames;
	float stateValue;
	unsigned int layoutStride, layoutOffset;
	LegacyLogicalState layoutState;
	LegacyVertexLayout layoutValue;
	unsigned int swapIntervalSetCalls, swapIntervalGetCalls;
	unsigned int gammaSetCalls, gammaGetCalls;
	unsigned int faultConfigCalls, resourceStatisticsCalls;
	RenderResourceFaultPoint lastFaultPoint;
	unsigned int lastFaultInvocation;
	RenderResult lastFaultResult;
	RenderTargetBinding targets;
	void *window;
	IRenderDevice *proxy;
	unsigned int sentMessages, postedMessages;
	bool reentrantRejected;
	std::vector<int> events;
	std::vector<unsigned char> bufferBytes, updateBytes, textureBytes, refreshBytes;
	std::vector<RenderPrimitiveTopology> topologies;
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
	explicit FakeBackend(Fixture &fixture) : f(fixture), handles(64), operational(false), open(false),
		gammaValue(1.0f), brightnessValue(0.0f), contrastValue(1.0f),
		calibrateValue(false), useLimitValue(true),
		faultPointValue(RENDER_RESOURCE_FAULT_NONE),
		faultFailOnInvocationValue(0), faultResultValue(RENDER_RESULT_FAILED),
		statisticsValue()
	{
		f.owner = std::this_thread::get_id();
		++f.factoryCalls; f.event(CREATED);
		info.width = info.height = 4; info.format = RENDER_FORMAT_B8G8R8A8_UNORM;
		statisticsValue.liveHandles = 11;
		statisticsValue.bufferCount = 3;
		statisticsValue.textureCount = 5;
		statisticsValue.nativeResourceCount = 7;
		statisticsValue.shaderResourceViewCount = 9;
		statisticsValue.renderTargetViewCount = 13;
		statisticsValue.depthStencilViewCount = 15;
		statisticsValue.recoveryShadowBytes = 17;
	}
	~FakeBackend() override { f.event(DELETED); ++f.destroys; }
	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return operational; }
	RenderResult initialize(const RenderDeviceParameters &parameters) override
	{ f.event(INITIALIZED); f.sendWindowMessage(); swapInterval = parameters.enableVsync ? 1 : 0; operational = !f.failInitialize; return operational ? RENDER_RESULT_OK : RENDER_RESULT_FAILED; }
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
	RenderResult getTextureFilterCapabilities(
		RenderTextureFilterCapabilities *output) const override
	{
		f.event(FILTER_CAPS);
		CHECK(!open && output != 0);
		++f.textureFilterCapabilityCalls;
		output->supportsPoint = true;
		output->supportsLinear = true;
		output->supportsAnisotropic = true;
		output->maxAnisotropy = f.reportedMaxAnisotropy;
		return RENDER_RESULT_OK;
	}
	RenderResult setSwapInterval(unsigned int interval) override
	{
		f.event(SWAP_SET);
		CHECK(!open && interval <= RENDER_SWAP_INTERVAL_MAX);
		++f.swapIntervalSetCalls;
		swapInterval = interval;
		return RENDER_RESULT_OK;
	}
	RenderResult getSwapInterval(unsigned int *interval) const override
	{
		f.event(SWAP_GET);
		CHECK(!open && interval != 0);
		++f.swapIntervalGetCalls;
		*interval = swapInterval;
		return RENDER_RESULT_OK;
	}
	RenderResult setGamma(float gamma, float brightness, float contrast,
		bool calibrate, bool useLimit) override
	{
		f.event(GAMMA_SET);
		CHECK(!open && gamma >= 0.6f && gamma <= 6.0f &&
			brightness >= -0.5f && brightness <= 0.5f &&
			contrast >= 0.5f && contrast <= 2.0f);
		++f.gammaSetCalls;
		gammaValue = gamma; brightnessValue = brightness;
		contrastValue = contrast; calibrateValue = calibrate;
		useLimitValue = useLimit;
		return RENDER_RESULT_OK;
	}
	RenderResult getGamma(float *gamma, float *brightness, float *contrast,
		bool *calibrate, bool *useLimit) const override
	{
		f.event(GAMMA_GET);
		CHECK(!open && gamma != 0 && brightness != 0 && contrast != 0 &&
			calibrate != 0 && useLimit != 0);
		++f.gammaGetCalls;
		*gamma = gammaValue; *brightness = brightnessValue;
		*contrast = contrastValue; *calibrate = calibrateValue;
		*useLimit = useLimitValue;
		return RENDER_RESULT_OK;
	}
	RenderResult configureResourceFaultInjection(
		RenderResourceFaultPoint point, unsigned int failOnInvocation,
		RenderResult result) override
	{
		f.event(FAULT_CONFIG);
		CHECK(!open);
		++f.faultConfigCalls;
		f.lastFaultPoint = point;
		f.lastFaultInvocation = failOnInvocation;
		f.lastFaultResult = result;
		faultPointValue = point;
		faultFailOnInvocationValue = failOnInvocation;
		faultResultValue = result;
		return RENDER_RESULT_OK;
	}
	RenderResult getDebugResourceStatistics(
		RenderResourceStatistics *statistics) const override
	{
		f.event(RESOURCE_STATS);
		CHECK(statistics != 0);
		++f.resourceStatisticsCalls;
		*statistics = statisticsValue;
		return RENDER_RESULT_OK;
	}
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
		f.layoutState = state; f.layoutValue = layout;
		f.layoutStride = layout.stride; f.layoutOffset = layout.elements[0].byteOffset; return RENDER_RESULT_OK;
	}
	RenderResult setVertexBuffer(GpuHandle handle, unsigned int, unsigned int) override
	{ f.event(VERTEX); CHECK(!handle.isValid() || handles.isLive(handle)); return RENDER_RESULT_OK; }
	RenderResult setIndexBuffer(GpuHandle handle, RenderFormat, unsigned int) override
	{ f.event(INDEX); CHECK(!handle.isValid() || handles.isLive(handle)); return RENDER_RESULT_OK; }
	RenderResult setTexture(unsigned int, GpuHandle handle) override
	{ f.event(BIND_TEXTURE); CHECK(!handle.isValid() || handles.isLive(handle)); return RENDER_RESULT_OK; }
	RenderResult setPrimitiveTopology(RenderPrimitiveTopology topology) override
	{
		f.event(TOPOLOGY);
		f.topologies.push_back(topology);
		switch (topology)
		{
		case RENDER_PRIMITIVE_TRIANGLE_LIST:
		case RENDER_PRIMITIVE_TRIANGLE_STRIP:
		case RENDER_PRIMITIVE_LINE_LIST:
		case RENDER_PRIMITIVE_LINE_STRIP:
			return f.failTopology ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
		default:
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
	}
	RenderResult draw(unsigned int, unsigned int) override
	{
		rts::frame_timing::Scope timing(rts::frame_timing::RendererDrawSubmit);
		f.event(DRAW); CHECK(open); ++f.draws;
		if (f.busyDraw)
		{
			f.busyEntered.store(true);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!f.busyRelease.load()) CHECK(std::chrono::steady_clock::now() < deadline);
		}
		return f.failDraw ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
	}
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
	unsigned int swapInterval;
	float gammaValue, brightnessValue, contrastValue;
	bool calibrateValue, useLimitValue;
	RenderResourceFaultPoint faultPointValue;
	unsigned int faultFailOnInvocationValue;
	RenderResult faultResultValue;
	RenderResourceStatistics statisticsValue;
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

#if defined(_WIN64)
typedef rts::render::detail::RenderPipelineStallTrace PipelineTrace;

struct PipelineTraceEnvironment
{
	PipelineTraceEnvironment()
	{
		for (unsigned int i = 0; i < 4; ++i)
		{
			wchar_t value[MAX_PATH];
			const DWORD length = GetEnvironmentVariableW(names[i], value, MAX_PATH);
			CHECK(length < MAX_PATH);
			previous[i] = length ? value : L"";
			CHECK(SetEnvironmentVariableW(names[i], nullptr));
		}
	}
	~PipelineTraceEnvironment()
	{
		for (unsigned int i = 0; i < 4; ++i)
			SetEnvironmentVariableW(names[i], previous[i].empty() ? nullptr : previous[i].c_str());
	}
	const wchar_t *names[4] = { L"RTS_RENDER_PIPELINE_TRACE_DIR", L"RTS_FRAME_TIMING_DIR",
		L"RTS_RENDER_OWNER_TIMING_DIR", L"RTS_GPU_FRAME_TIMING_DIR" };
	std::wstring previous[4];
};

std::wstring PipelineTestDirectory(const wchar_t *suffix)
{
	wchar_t relative[128], absolute[MAX_PATH], temporary[MAX_PATH];
	const DWORD temporaryLength = GetTempPathW(MAX_PATH, temporary);
	CHECK(temporaryLength && temporaryLength < MAX_PATH);
	CHECK(swprintf_s(relative, L"ThreadedPipelineTrace-%lu-%llu-%ls",
		GetCurrentProcessId(), GetTickCount64(), suffix) >= 0);
	const std::wstring ownedLeaf = std::wstring(temporary) + relative;
	const DWORD length = GetFullPathNameW(ownedLeaf.c_str(), MAX_PATH, absolute, nullptr);
	CHECK(length && length < MAX_PATH && CreateDirectoryW(absolute, nullptr));
	return absolute;
}

std::vector<std::wstring> PipelineOutputs(const std::wstring &directory)
{
	std::vector<std::wstring> outputs;
	WIN32_FIND_DATAW entry;
	HANDLE search = FindFirstFileW((directory + L"\\pipeline-stall-*").c_str(), &entry);
	if (search != INVALID_HANDLE_VALUE)
	{
		do { CHECK(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY); outputs.push_back(directory + L"\\" + entry.cFileName); }
		while (FindNextFileW(search, &entry));
		CHECK(GetLastError() == ERROR_NO_MORE_FILES); FindClose(search);
	}
	return outputs;
}

struct PipelineRow
{
	std::string stream, event;
	unsigned long long ordinal, sequence, packet;
	long long tick;
	unsigned int thread, queued, free, pending, detail;
};

std::vector<PipelineRow> ReadPipelineRows(const std::wstring &directory)
{
	FILE *file = _wfopen((directory + L"\\events.csv").c_str(), L"rt"); CHECK(file);
	char line[512]; CHECK(fgets(line, sizeof(line), file));
	CHECK(strcmp(line, "stream,ordinal,event,qpc,thread_id,sequence,packet_id,queue_count,free_count,pending_count,detail\n") == 0);
	std::vector<PipelineRow> rows;
	while (fgets(line, sizeof(line), file))
	{
		PipelineRow row; char stream[16], event[32];
		CHECK(sscanf(line, "%15[^,],%llu,%31[^,],%lld,%u,%llu,%llu,%u,%u,%u,%u",
			stream, &row.ordinal, event, &row.tick, &row.thread, &row.sequence, &row.packet,
			&row.queued, &row.free, &row.pending, &row.detail) == 11);
		row.stream = stream; row.event = event; rows.push_back(row);
	}
	CHECK(ferror(file) == 0 && fclose(file) == 0);
	return rows;
}

void RemovePipelineOutput(const std::wstring &directory)
{
	CHECK(DeleteFileW((directory + L"\\events.csv").c_str()));
	CHECK(DeleteFileW((directory + L"\\summary.csv").c_str()));
	CHECK(RemoveDirectoryW(directory.c_str()));
}

void PipelineTraceOffCapOwnershipAndExport()
{
	PipelineTraceEnvironment environment;
	std::thread absentOwner;
	PipelineTrace off;
	CHECK(!off.enabled() && off.status() == PipelineTrace::Off);
	off.record(PipelineTrace::Producer, PipelineTrace::Publish, 1, 1);
	CHECK(off.count(PipelineTrace::Producer) == 0 && !off.exportAfterJoin(absentOwner));
	CHECK(SetEnvironmentVariableW(environment.names[0], L"relative-directory"));
	PipelineTrace relative; CHECK(!relative.enabled() && relative.status() == PipelineTrace::InvalidConfiguration);
	const std::wstring directory = PipelineTestDirectory(L"ring");
	CHECK(SetEnvironmentVariableW(environment.names[0], directory.c_str()));
	CHECK(SetEnvironmentVariableW(environment.names[1], directory.c_str()));
	PipelineTrace collision; CHECK(!collision.enabled());
	auto rejectOverlap = [&](const std::wstring &traceDirectory, const std::wstring &timingDirectory)
	{
		CHECK(SetEnvironmentVariableW(environment.names[0], traceDirectory.c_str()));
		CHECK(SetEnvironmentVariableW(environment.names[1], timingDirectory.c_str()));
		PipelineTrace rejected;
		CHECK(!rejected.enabled() && rejected.status() == PipelineTrace::InvalidConfiguration &&
			!rejected.exportAfterJoin(absentOwner) && rejected.outputDirectory()[0] == L'\0');
	};
	if (directory.size() > 3 && directory[1] == L':' && directory[2] == L'\\')
	{
		const std::wstring driveRoot = directory.substr(0, 3);
		rejectOverlap(driveRoot, directory);
		rejectOverlap(directory, driveRoot);
	}
	const std::wstring nested = directory + L"\\nested", sibling = directory + L"\\nested-extra";
	CHECK(CreateDirectoryW(nested.c_str(), nullptr) && CreateDirectoryW(sibling.c_str(), nullptr));
	rejectOverlap(directory, nested);
	rejectOverlap(nested, directory);
	{
		CHECK(SetEnvironmentVariableW(environment.names[0], nested.c_str()));
		CHECK(SetEnvironmentVariableW(environment.names[1], sibling.c_str()));
		PipelineTrace siblings; CHECK(siblings.enabled() && siblings.status() == PipelineTrace::Configured);
		CHECK(PipelineOutputs(nested).empty() && PipelineOutputs(sibling).empty());
	}
	CHECK(RemoveDirectoryW(nested.c_str()) && RemoveDirectoryW(sibling.c_str()));
	CHECK(SetEnvironmentVariableW(environment.names[0], directory.c_str()));
	CHECK(SetEnvironmentVariableW(environment.names[1], nullptr));
	PipelineTrace trace; CHECK(trace.enabled());
	for (unsigned int i = 0; i < PipelineTrace::Capacity + 17U; ++i)
		trace.record(PipelineTrace::Producer, PipelineTrace::Publish, 0, i + 1, 1, 2, 1);
	CHECK(trace.count(PipelineTrace::Producer) == PipelineTrace::Capacity &&
		trace.overwritten(PipelineTrace::Producer) == 17);
	std::atomic<bool> ready(false), release(false);
	std::thread owner([&]
	{
		trace.bindOwner();
		trace.record(PipelineTrace::Owner, PipelineTrace::ExecuteBegin, 0, 9);
		trace.record(PipelineTrace::Producer, PipelineTrace::Publish, 9, 9); // Rejected, never touches the producer ring.
		ready.store(true);
		while (!release.load()) std::this_thread::yield();
		trace.record(PipelineTrace::Owner, PipelineTrace::ExecuteEnd, 0, 9);
	});
	while (!ready.load()) std::this_thread::yield();
	CHECK(!trace.exportAfterJoin(owner) && PipelineOutputs(directory).empty());
	release.store(true); owner.join();
	trace.record(PipelineTrace::Owner, PipelineTrace::PoolReturn, 0, 9); // Wrong owner after join.
	CHECK(trace.rejectedThread(PipelineTrace::Producer) == 1 && trace.rejectedThread(PipelineTrace::Owner) == 1);
	CHECK(trace.exportAfterJoin(owner) && trace.status() == PipelineTrace::ExportSucceeded);
	CHECK(!trace.exportAfterJoin(owner)); // Never overwrites/reexports a finished capture.
	const auto rows = ReadPipelineRows(trace.outputDirectory());
	CHECK(rows.size() == PipelineTrace::Capacity + 2U && rows.front().ordinal == 18 &&
		rows[PipelineTrace::Capacity - 1].ordinal == PipelineTrace::Capacity + 17U);
	for (size_t i = 1; i < PipelineTrace::Capacity; ++i)
		CHECK(rows[i].tick >= rows[i - 1].tick && rows[i].thread == GetCurrentThreadId());
	CHECK(rows.back().tick >= rows[rows.size() - 2].tick && rows.back().thread != GetCurrentThreadId());
	FILE *summary = _wfopen((std::wstring(trace.outputDirectory()) + L"\\summary.csv").c_str(), L"rt"); CHECK(summary);
	char summaryLine[512]; CHECK(fgets(summaryLine, sizeof(summaryLine), summary));
	CHECK(strcmp(summaryLine, "stream,capacity,retained,overwritten,rejected_thread,clock_failures,qpc_frequency,thread_id,serial_policy\n") == 0);
	for (unsigned int stream = 0; stream < 2; ++stream)
	{
		char name[16]; unsigned int capacity = 0, retained = 0, rejected = 0, failures = 0, thread = 0, serial = 0;
		unsigned long long overwritten = 0; long long frequency = 0;
		CHECK(fgets(summaryLine, sizeof(summaryLine), summary));
		CHECK(sscanf(summaryLine, "%15[^,],%u,%u,%llu,%u,%u,%lld,%u,%u", name, &capacity,
			&retained, &overwritten, &rejected, &failures, &frequency, &thread, &serial) == 9);
		CHECK(strcmp(name, stream == 0 ? "producer" : "owner") == 0 && capacity == PipelineTrace::Capacity &&
			retained == (stream == 0 ? PipelineTrace::Capacity : 2U) && overwritten == (stream == 0 ? 17U : 0U) &&
			rejected == 1 && failures == 0 && frequency > 0 && thread != 0 && serial == 0);
	}
	CHECK(!fgets(summaryLine, sizeof(summaryLine), summary) && fclose(summary) == 0);
	RemovePipelineOutput(trace.outputDirectory()); CHECK(RemoveDirectoryW(directory.c_str()));
	const std::wstring missing = PipelineTestDirectory(L"removed");
	CHECK(SetEnvironmentVariableW(environment.names[0], missing.c_str()));
	PipelineTrace failed; CHECK(failed.enabled());
	CHECK(RemoveDirectoryW(missing.c_str()));
	CHECK(!failed.exportAfterJoin(absentOwner) && failed.status() == PipelineTrace::ExportFailed);
	CHECK(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES); // Export does not recreate parents.
}

struct PipelineExportFailureIO : PipelineTrace::ExportFileIO
{
	enum Failure { AfterCreate, EventsOpen, EventsWrite, EventsFlush, EventsClose,
		SummaryOpen, SummaryWrite, SummaryFlush, SummaryClose, Publish, None };
	explicit PipelineExportFailureIO(Failure failure) : failure(failure) {}
	bool created() noexcept { return failure != AfterCreate; }
	FILE *open(const wchar_t *path) noexcept
	{
		++opens;
		if ((opens == 1 && failure == EventsOpen) || (opens == 2 && failure == SummaryOpen)) return nullptr;
		return ExportFileIO::open(path);
	}
	bool written(bool ok) noexcept
	{
		++writes;
		return ok && !((writes == 1 && failure == EventsWrite) || (writes == 2 && failure == SummaryWrite));
	}
	bool flush(FILE *file) noexcept
	{
		++flushes;
		const bool ok = ExportFileIO::flush(file);
		return ok && !((flushes == 1 && failure == EventsFlush) || (flushes == 2 && failure == SummaryFlush));
	}
	bool close(FILE *file) noexcept
	{
		++closes;
		const bool ok = ExportFileIO::close(file); // Actually close even when simulating a close error.
		return ok && !((closes == 1 && failure == EventsClose) || (closes == 2 && failure == SummaryClose));
	}
	bool publish(const wchar_t *pending, const wchar_t *final) noexcept
	{
		++publishes;
		if (failure == Publish)
		{
			// Race a preexisting destination against the actual no-overwrite publication.
			CHECK(CreateDirectoryW(final, nullptr));
			const bool ok = ExportFileIO::publish(pending, final);
			CHECK(!ok && GetFileAttributesW(final) != INVALID_FILE_ATTRIBUTES);
			CHECK(RemoveDirectoryW(final));
			return ok;
		}
		return ExportFileIO::publish(pending, final);
	}
	Failure failure;
	unsigned int opens = 0, writes = 0, flushes = 0, closes = 0, publishes = 0;
};

void PipelineTraceStagesFailedExports()
{
	PipelineTraceEnvironment environment;
	std::thread absentOwner;
	for (unsigned int failure = PipelineExportFailureIO::AfterCreate; failure <= PipelineExportFailureIO::None; ++failure)
	{
		wchar_t suffix[32]; CHECK(swprintf_s(suffix, L"export-%u", failure) >= 0);
		const std::wstring directory = PipelineTestDirectory(suffix);
		CHECK(SetEnvironmentVariableW(environment.names[0], directory.c_str()));
		PipelineTrace trace; CHECK(trace.enabled());
		trace.record(PipelineTrace::Producer, PipelineTrace::Publish, 0, 7, 1, 2, 1);
		PipelineExportFailureIO io(static_cast<PipelineExportFailureIO::Failure>(failure));
		const bool success = failure == PipelineExportFailureIO::None;
		CHECK(trace.exportAfterJoin(absentOwner, io) == success);
		CHECK(trace.status() == (success ? PipelineTrace::ExportSucceeded : PipelineTrace::ExportFailed));
		CHECK(!trace.exportAfterJoin(absentOwner, io));
		const std::wstring output = trace.outputDirectory();
		CHECK(output.size() > 8 && (output.substr(output.size() - 8) == L".pending") == !success);
		const auto outputs = PipelineOutputs(directory);
		CHECK(outputs.size() == 1 && outputs[0] == output);
		CHECK(GetFileAttributesW(output.c_str()) & FILE_ATTRIBUTE_DIRECTORY);
		if (!success)
			CHECK(GetFileAttributesW(output.substr(0, output.size() - 8).c_str()) == INVALID_FILE_ATTRIBUTES);
		CHECK(io.closes == io.flushes && io.closes == io.writes);
		CHECK(io.publishes == (failure >= PipelineExportFailureIO::Publish ? 1U : 0U));
		if (success)
		{
			CHECK(io.opens == 2 && io.closes == 2);
			const auto rows = ReadPipelineRows(output);
			CHECK(rows.size() == 1 && rows[0].sequence == 0 && rows[0].packet == 7 && rows[0].queued == 1);
			RemovePipelineOutput(output);
		}
		else
		{
			// Only the two exact owned artifact names may exist in a failed staging directory.
			for (const wchar_t *name : { L"events.csv", L"summary.csv" })
			{
				const std::wstring path = output + L"\\" + name;
				if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) CHECK(DeleteFileW(path.c_str()));
			}
			CHECK(RemoveDirectoryW(output.c_str()));
		}
		CHECK(RemoveDirectoryW(directory.c_str()));
	}
}

void PipelineTraceFollowsProductionWaitsAndShutdown()
{
	PipelineTraceEnvironment environment;
	LARGE_INTEGER frequency; CHECK(QueryPerformanceFrequency(&frequency));
	for (unsigned int mode = 0; mode < 4; ++mode)
	{
		const bool enabled = mode != 0, serial = mode >= 2, exportFailure = mode == 3;
		const std::wstring parent = PipelineTestDirectory(serial ? L"serial" : enabled ? L"enabled" : L"off");
		const std::wstring directory = parent + L"\\trace", timing = parent + L"\\timing";
		CHECK(CreateDirectoryW(directory.c_str(), nullptr) && CreateDirectoryW(timing.c_str(), nullptr));
		CHECK(SetEnvironmentVariableW(environment.names[0], enabled ? directory.c_str() : nullptr));
		CHECK(SetEnvironmentVariableW(environment.names[2], timing.c_str()));
		Fixture fixture;
		ThreadedRenderOptions options; options.maxFramesInFlight = 2; options.maxPacketCommands = 2; options.serial = serial;
		auto device = Device(fixture, options);
		BufferDescriptor descriptor; descriptor.byteCount = 4; descriptor.usage = RENDER_USAGE_DEFAULT;
		unsigned int bytes = 7; GpuHandle buffer;
		CHECK(device->createBuffer(descriptor, &bytes, sizeof(bytes), &buffer) == RENDER_RESULT_OK);
		CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK); // Sequence-zero resource packet.
		if (serial)
		{
			EmptyFrame(device.get()); CHECK(Complete(device.get()).presented);
		}
		else
		{
			ReleaseGate release(fixture); fixture.gateEvent = BEGIN;
			EmptyFrame(device.get()); fixture.waitForGate(); EmptyFrame(device.get());
			std::thread unblock([&]
			{
				ThreadedRenderMetrics metrics;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				do { CHECK(GetThreadedRenderMetrics(device.get(), &metrics)); std::this_thread::yield(); }
				while (!metrics.backpressureWaits && std::chrono::steady_clock::now() < deadline);
				std::this_thread::sleep_for(std::chrono::milliseconds(10)); fixture.release();
			});
			const RenderResult begun = device->immediateContext()->beginFrame(); unblock.join();
			CHECK(begun == RENDER_RESULT_OK && CancelThreadedRenderFrame(device.get()) == RENDER_RESULT_OK);
			CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_FAILED);
		}
		CHECK(PipelineOutputs(directory).empty()); // No trace file/directory creation during any render/wait.
		if (exportFailure) CHECK(RemoveDirectoryW(directory.c_str()));
		device->shutdown(); CHECK(!fixture.wrongThread && fixture.presents == (serial ? 1U : 2U));
		const auto outputs = PipelineOutputs(directory);
		CHECK(outputs.size() == (enabled && !exportFailure ? 1U : 0U));
		if (enabled && !exportFailure)
		{
			const auto rows = ReadPipelineRows(outputs[0]);
			bool zero = false, reply = false, telemetry = false, returned = false, waited = false;
			long long waitBegin = 0; uint64_t telemetryPacket = 0;
			for (const auto &row : rows)
			{
				CHECK(row.tick > 0);
				CHECK((row.stream == "producer") == (row.thread == GetCurrentThreadId()));
				if (row.event == "dequeue") { zero |= row.sequence == 0 && row.packet != 0; CHECK(row.pending <= 2); }
				if (row.event == "reply_wait_begin") reply = true;
				if (row.event == "acquire_wait_begin") { waitBegin = row.tick; CHECK(row.free == 0); }
				if (row.event == "acquire_wait_end") { CHECK(waitBegin && row.tick - waitBegin >= frequency.QuadPart / 200); waited = true; }
				if (row.event == "serial_wait_end") waited = true;
				if (row.event == "telemetry_begin") telemetryPacket = row.packet;
				if (row.event == "telemetry_end") { CHECK(row.packet == telemetryPacket); telemetry = true; }
				if (row.event == "pool_return") { CHECK(row.free > 0 && row.pending <= 2); returned = true; }
			}
			CHECK(zero && reply && telemetry && returned && waited);
			RemovePipelineOutput(outputs[0]);
		}
		// Existing owner timing remains in its own namespace and retains its CSV schema.
		WIN32_FIND_DATAW entry;
		HANDLE search = FindFirstFileW((timing + L"\\render-owner-timing-*.csv").c_str(), &entry);
		CHECK(search != INVALID_HANDLE_VALUE);
		const std::wstring timingFile = timing + L"\\" + entry.cFileName;
		CHECK(!FindNextFileW(search, &entry)); FindClose(search);
		FILE *ownerFile = _wfopen(timingFile.c_str(), L"rt"); CHECK(ownerFile);
		char header[512]; CHECK(fgets(header, sizeof(header), ownerFile));
		CHECK(strcmp(header, "session,mode,sequence_begin,sequence_end,executed_packets,wall_ms,phase,samples,total_ms,avg_ms,p95_upper_ms,p99_upper_ms,max_ms,over_33ms,over_100ms\n") == 0);
		CHECK(fclose(ownerFile) == 0);
		CHECK(DeleteFileW(timingFile.c_str()));
		CHECK(RemoveDirectoryW(timing.c_str()) && (exportFailure || RemoveDirectoryW(directory.c_str())) && RemoveDirectoryW(parent.c_str()));
	}
}

void RenderOwnerDiagnosticsFollowActualPacketExecution()
{
	CHECK(SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL));
	char relative[96], absolute[MAX_PATH];
	_snprintf(relative, sizeof(relative), "ThreadedRenderTiming-%lu-%lu", GetCurrentProcessId(), GetTickCount());
	const DWORD length = GetFullPathNameA(relative, sizeof(absolute), absolute, NULL);
	CHECK(length && length < sizeof(absolute) && CreateDirectoryA(absolute, NULL));
	for (unsigned int enabled = 0; enabled != 2; ++enabled)
	{
		const std::string directory = std::string(absolute) + (enabled ? "\\enabled" : "\\disabled");
		CHECK(CreateDirectoryA(directory.c_str(), NULL));
		CHECK(SetEnvironmentVariableA("RTS_RENDER_OWNER_TIMING_DIR", enabled ? directory.c_str() : NULL));
		Fixture fixture;
		ThreadedRenderOptions options; options.maxPacketCommands = 2;
		auto device = Device(fixture, options);
		IRenderContext *context = device->immediateContext();
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		for (unsigned int draw = 0; draw != 6; ++draw)
			CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), true) == RENDER_RESULT_OK);
		CHECK(Complete(device.get()).presented);
		fixture.failDraw = true;
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		for (unsigned int draw = 0; draw != 3; ++draw)
			CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
		CHECK(Complete(device.get(), RENDER_RESULT_FAILED).outcome.hasCommandFailure());
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(CancelThreadedRenderFrame(device.get(), RENDER_RESULT_FAILED) == RENDER_RESULT_OK);
		CHECK(Complete(device.get(), RENDER_RESULT_FAILED).outcome.hasCommandFailure());
		ThreadedRenderMetrics metrics;
		CHECK(GetThreadedRenderMetrics(device.get(), &metrics) && metrics.completedFrames == 3);
		device->shutdown();
		CHECK(!fixture.wrongThread && fixture.draws == 7);
		WIN32_FIND_DATAA entry;
		HANDLE search = FindFirstFileA((directory + "\\frame-timing-*.csv").c_str(), &entry);
		CHECK(search == INVALID_HANDLE_VALUE); // Main receipt namespace remains untouched.
		search = FindFirstFileA((directory + "\\render-owner-timing-*.csv").c_str(), &entry);
		if (!enabled)
			CHECK(search == INVALID_HANDLE_VALUE);
		else
		{
			CHECK(search != INVALID_HANDLE_VALUE);
			const std::string path = directory + "\\" + entry.cFileName;
			CHECK(!FindNextFileA(search, &entry)); FindClose(search);
			FILE *file = fopen(path.c_str(), "rb"); CHECK(file != NULL);
			char line[1024]; CHECK(fgets(line, sizeof(line), file));
			CHECK(strstr(line, "session,mode,sequence_begin,sequence_end,executed_packets,") == line);
			unsigned int rows = 0;
			while (fgets(line, sizeof(line), file))
			{
				unsigned int session = 0, samples = 0;
				unsigned __int64 first = 0, last = 0, packets = 0;
				char mode[32], phase[32]; double wall = 0;
				CHECK(sscanf(line, "%u,%31[^,],%llu,%llu,%llu,%lf,%31[^,],%u",
					&session, mode, &first, &last, &packets, &wall, phase, &samples) == 8);
				CHECK(strcmp(mode, "render_owner") == 0 && first == 1 && last == 3 && packets > metrics.completedFrames);
				if (strcmp(phase, "execution_packet") == 0) CHECK(samples == packets);
				else CHECK(strcmp(phase, "renderer_draw_submit") == 0 && samples == fixture.draws);
				++rows;
			}
			CHECK(rows == 2); fclose(file); CHECK(DeleteFileA(path.c_str()));
		}
		CHECK(RemoveDirectoryA(directory.c_str()));
	}
	CHECK(SetEnvironmentVariableA("RTS_RENDER_OWNER_TIMING_DIR", NULL));
	CHECK(RemoveDirectoryA(absolute));
}
#endif

void ProducerTextureBindingCachePreservesOrderedInvalidation()
{
	Fixture f;
	auto device = Device(f);
	TextureDescriptor descriptor;
	descriptor.width = descriptor.height = 1;
	descriptor.format = RENDER_FORMAT_B8G8R8A8_UNORM;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	unsigned int pixel = 0xffffffffU;
	TextureSubresourceData data;
	data.data = &pixel; data.rowPitch = data.slicePitch = sizeof(pixel);
	GpuHandle first, second;
	CHECK(device->createTexture(descriptor, &data, 1, &first) == RENDER_RESULT_OK);
	CHECK(device->createTexture(descriptor, &data, 1, &second) == RENDER_RESULT_OK);
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(1, second) == RENDER_RESULT_OK);
	CHECK(context->setTexture(1, second) == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(first, GpuHandle()) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(device->copyActiveColorTargetToTexture(second) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(SubmitThreadedRenderFrame(device.get(), true) == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
	CHECK(std::count(f.events.begin(), f.events.end(), BIND_TEXTURE) == 6);

	CHECK(context->setTexture(0, first) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(device->refreshTexture(first, descriptor, &data, 1) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->setTexture(0, first) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(SubmitThreadedRenderFrame(device.get(), true) == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
	CHECK(std::count(f.events.begin(), f.events.end(), BIND_TEXTURE) == 8);
	CHECK(device->destroyResource(second));
	CHECK(context->setTexture(1, second) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
}

void ProducerTopologyCachePreservesAdmissionAndFailure()
{
	// RenderPrimitiveTopology has no fixed underlying type and values 0..3.
	// An out-of-range enum cannot be passed portably through this public API;
	// invalid raw encodings are verified by production owner-switch source review.
	for (unsigned int serial = 0; serial != 2; ++serial)
	{
		Fixture f;
		ThreadedRenderOptions options;
		options.serial = serial != 0;
		options.maxPacketCommands = 2;
		auto device = Device(f, options);
		IRenderContext *context = device->immediateContext();

		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_INVALID_ARGUMENT);
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK);

		RenderResult offOwner = RENDER_RESULT_OK;
		std::thread rejectedProducer([&]
		{
			offOwner = context->setPrimitiveTopology(
				RENDER_PRIMITIVE_TRIANGLE_LIST);
		});
		rejectedProducer.join();
		CHECK(offOwner == RENDER_RESULT_INVALID_ARGUMENT);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK);

		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
		CHECK(!Complete(device.get()).presented);
		CHECK(f.topologies.size() == 4 &&
			f.topologies[0] == RENDER_PRIMITIVE_TRIANGLE_LIST &&
			f.topologies[1] == RENDER_PRIMITIVE_TRIANGLE_STRIP &&
			f.topologies[2] == RENDER_PRIMITIVE_LINE_LIST &&
			f.topologies[3] == RENDER_PRIMITIVE_LINE_STRIP);

		// A new logical frame must enqueue its first topology even when it
		// matches the last value from the preceding frame.
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
		CHECK(!Complete(device.get()).presented);
		CHECK(f.topologies.size() == 5 &&
			f.topologies[4] == RENDER_PRIMITIVE_LINE_STRIP);

		// A control fence breaks producer knowledge even though the owner keeps
		// the same frame and topology open across the control.
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setRenderTargets(GpuHandle(), GpuHandle()) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
		CHECK(!Complete(device.get()).presented);
		CHECK(f.topologies.size() == 8 &&
			f.topologies[5] == RENDER_PRIMITIVE_LINE_STRIP &&
			f.topologies[6] == RENDER_PRIMITIVE_LINE_STRIP &&
			f.topologies[7] == RENDER_PRIMITIVE_LINE_STRIP);

		// An ended-frame call cannot use the last producer value as a hit. Its
		// synchronous rejection remains attached to the submitted frame.
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_INVALID_ARGUMENT);
		const RenderResult endedSubmit = SubmitThreadedRenderFrame(
			device.get(), false);
		CHECK(endedSubmit == (options.serial ? RENDER_RESULT_INVALID_ARGUMENT :
			RENDER_RESULT_OK));
		CHECK(Complete(device.get(), RENDER_RESULT_INVALID_ARGUMENT).result ==
			RENDER_RESULT_INVALID_ARGUMENT);
		CHECK(f.topologies.size() == 8);

		// An out-of-frame attempt also rejects; the next frame starts clean.
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_INVALID_ARGUMENT);
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_LIST) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
		CHECK(!Complete(device.get()).presented);
		CHECK(f.topologies.size() == 9 &&
			f.topologies[8] == RENDER_PRIMITIVE_LINE_LIST);

		// A backend-side failure remains observable after a repeated setter, and
		// a later frame can submit and complete the same topology successfully.
		f.failTopology = true;
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		const RenderResult failedSubmit = SubmitThreadedRenderFrame(
			device.get(), false);
		CHECK(failedSubmit == (options.serial ? RENDER_RESULT_FAILED :
			RENDER_RESULT_OK));
		CHECK(Complete(device.get(), RENDER_RESULT_FAILED).result ==
			RENDER_RESULT_FAILED);
		CHECK(f.topologies.size() == 10 &&
			f.topologies[9] == RENDER_PRIMITIVE_LINE_STRIP);

		f.failTopology = false;
		CHECK(context->beginFrame() == RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_LINE_STRIP) ==
			RENDER_RESULT_OK);
		CHECK(context->endFrame() == RENDER_RESULT_OK);
		CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
		CHECK(!Complete(device.get()).presented);
		CHECK(f.topologies.size() == 11 &&
			f.topologies[10] == RENDER_PRIMITIVE_LINE_STRIP);
	}
}

void SwapIntervalOwnerTransport()
{
	Fixture f;
	std::unique_ptr<IRenderDevice> device = Device(f);
	unsigned int interval = 0xffffffffU;
	CHECK(device->getSwapInterval(&interval) == RENDER_RESULT_OK && interval == 1);
	CHECK(device->setSwapInterval(0) == RENDER_RESULT_OK);
	CHECK(device->getSwapInterval(&interval) == RENDER_RESULT_OK && interval == 0);
	CHECK(device->setSwapInterval(3) == RENDER_RESULT_OK);
	CHECK(device->getSwapInterval(&interval) == RENDER_RESULT_OK && interval == 3);
	CHECK(device->setSwapInterval(4) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->getSwapInterval(0) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(f.swapIntervalSetCalls == 2 && f.swapIntervalGetCalls == 3);

	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(device->setSwapInterval(2) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->getSwapInterval(&interval) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
	CHECK(!Complete(device.get()).presented);
	CHECK(device->setSwapInterval(2) == RENDER_RESULT_OK);
	CHECK(device->getSwapInterval(&interval) == RENDER_RESULT_OK && interval == 2);

	RenderResult offOwnerSet = RENDER_RESULT_OK;
	RenderResult offOwnerGet = RENDER_RESULT_OK;
	std::thread offOwner([&]
	{
		offOwnerSet = device->setSwapInterval(1);
		offOwnerGet = device->getSwapInterval(&interval);
	});
	offOwner.join();
	CHECK(offOwnerSet == RENDER_RESULT_INVALID_ARGUMENT &&
		offOwnerGet == RENDER_RESULT_INVALID_ARGUMENT && interval == 2);
	CHECK(f.swapIntervalSetCalls == 3 && f.swapIntervalGetCalls == 4);
}

void GammaOwnerTransport()
{
	Fixture f;
	std::unique_ptr<IRenderDevice> device = Device(f);
	float gamma = 0.0f, brightness = 0.0f, contrast = 0.0f;
	bool calibrate = true, useLimit = false;
	CHECK(device->getGamma(&gamma, &brightness, &contrast, &calibrate,
		&useLimit) == RENDER_RESULT_OK && gamma == 1.0f &&
		brightness == 0.0f && contrast == 1.0f && !calibrate && useLimit);
	CHECK(device->setGamma(2.0f, 0.125f, 1.5f, true, false) == RENDER_RESULT_OK);
	CHECK(device->getGamma(&gamma, &brightness, &contrast, &calibrate,
		&useLimit) == RENDER_RESULT_OK && gamma == 2.0f &&
		brightness == 0.125f && contrast == 1.5f && calibrate && !useLimit);
	CHECK(f.gammaSetCalls == 1 && f.gammaGetCalls == 2);

	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(device->setGamma(1.5f, 0.0f, 1.0f, false, true) ==
		RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->getGamma(&gamma, &brightness, &contrast, &calibrate,
		&useLimit) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
	CHECK(!Complete(device.get()).presented);
	CHECK(device->setGamma(1.5f, 0.0f, 1.0f, false, true) == RENDER_RESULT_OK);

	RenderResult offOwnerSet = RENDER_RESULT_OK;
	RenderResult offOwnerGet = RENDER_RESULT_OK;
	std::thread offOwner([&]
	{
		offOwnerSet = device->setGamma(1.0f, 0.0f, 1.0f, false, true);
		offOwnerGet = device->getGamma(&gamma, &brightness, &contrast,
			&calibrate, &useLimit);
	});
	offOwner.join();
	CHECK(offOwnerSet == RENDER_RESULT_INVALID_ARGUMENT &&
		offOwnerGet == RENDER_RESULT_INVALID_ARGUMENT &&
		f.gammaSetCalls == 2 && f.gammaGetCalls == 2);
}

void TextureFilterCapabilitiesArePublishedFromOwner()
{
	Fixture f;
	std::unique_ptr<IRenderDevice> device = Device(f);
	CHECK(f.textureFilterCapabilityCalls == 1);

	RenderTextureFilterCapabilities capabilities;
	CHECK(device->getTextureFilterCapabilities(&capabilities) == RENDER_RESULT_OK);
	CHECK(capabilities.supportsPoint && capabilities.supportsLinear &&
		capabilities.supportsAnisotropic && capabilities.maxAnisotropy == 16);
	CHECK(device->getTextureFilterCapabilities(0) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(f.textureFilterCapabilityCalls == 1);

	CHECK(device->resize(8, 6) == RENDER_RESULT_OK);
	CHECK(f.textureFilterCapabilityCalls == 2);
	CHECK(device->getTextureFilterCapabilities(&capabilities) == RENDER_RESULT_OK &&
		capabilities.maxAnisotropy == 16);

	f.reportedMaxAnisotropy = 8;
	CHECK(device->recoverDevice() == RENDER_RESULT_OK);
	CHECK(f.textureFilterCapabilityCalls == 3);
	CHECK(device->getTextureFilterCapabilities(&capabilities) == RENDER_RESULT_OK &&
		capabilities.maxAnisotropy == 8);
	CHECK(f.textureFilterCapabilityCalls == 3 && !f.wrongThread);
}

void DebugResourceOwnerTransport()
{
	Fixture f;
	std::unique_ptr<IRenderDevice> device = Device(f);
	RenderResourceStatistics statistics;
	CHECK(device->getDebugResourceStatistics(&statistics) ==
		RENDER_RESULT_OK && statistics.liveHandles == 11 &&
		statistics.bufferCount == 3 && statistics.textureCount == 5 &&
		statistics.nativeResourceCount == 7 &&
		statistics.shaderResourceViewCount == 9 &&
		statistics.renderTargetViewCount == 13 &&
		statistics.depthStencilViewCount == 15 &&
		statistics.recoveryShadowBytes == 17);
	CHECK(f.resourceStatisticsCalls == 1);
	CHECK(device->getDebugResourceStatistics(0) ==
		RENDER_RESULT_INVALID_ARGUMENT && f.resourceStatisticsCalls == 1);

	// Match the direct backend's validation: NONE clears a pending fault even
	// with unused invocation/result values, while every real point requires a
	// positive invocation and an injected failure result.
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_ALLOCATION, 0,
		RENDER_RESULT_FAILED) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_ALLOCATION, 1,
		RENDER_RESULT_OK) == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(f.faultConfigCalls == 0);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_NONE, 0, RENDER_RESULT_OK) ==
		RENDER_RESULT_OK);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_ALLOCATION, 2,
		RENDER_RESULT_FAILED) == RENDER_RESULT_OK &&
		f.faultConfigCalls == 2);

	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	// Statistics are read-only owner controls and the direct D3D11 backend
	// permits them while a frame is open; sync must therefore fence the accepted
	// packet without touching a producer-owned backend pointer.
	CHECK(device->getDebugResourceStatistics(&statistics) ==
		RENDER_RESULT_OK && statistics.liveHandles == 11 &&
		f.resourceStatisticsCalls == 2);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_NONE, 0, RENDER_RESULT_OK) ==
		RENDER_RESULT_INVALID_ARGUMENT && f.faultConfigCalls == 2);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
	CHECK(!Complete(device.get()).presented);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_NONE, 0, RENDER_RESULT_OK) ==
		RENDER_RESULT_OK && f.faultConfigCalls == 3);

	RenderResourceStatistics offOwnerStatistics;
	RenderResult offOwnerFault = RENDER_RESULT_OK;
	RenderResult offOwnerStatisticsResult = RENDER_RESULT_OK;
	std::thread offOwner([&]
	{
		offOwnerFault = device->configureResourceFaultInjection(
			RENDER_RESOURCE_FAULT_NONE, 0, RENDER_RESULT_OK);
		offOwnerStatisticsResult = device->getDebugResourceStatistics(
			&offOwnerStatistics);
	});
	offOwner.join();
	CHECK(offOwnerFault == RENDER_RESULT_INVALID_ARGUMENT &&
		offOwnerStatisticsResult == RENDER_RESULT_INVALID_ARGUMENT &&
		f.faultConfigCalls == 3 && f.resourceStatisticsCalls == 2);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_REFRESH_AFTER_UNBIND, 1,
		RENDER_RESULT_FAILED) == RENDER_RESULT_OK &&
		f.faultConfigCalls == 4 &&
		f.lastFaultPoint == RENDER_RESOURCE_FAULT_TEXTURE_REFRESH_AFTER_UNBIND &&
		f.lastFaultInvocation == 1 &&
		f.lastFaultResult == RENDER_RESULT_FAILED);
	CHECK(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_RESIZE_TARGETS_RECOVERY_FAILURE, 1,
		RENDER_RESULT_FAILED) == RENDER_RESULT_OK &&
		f.faultConfigCalls == 5 &&
		f.lastFaultPoint == RENDER_RESOURCE_FAULT_RESIZE_TARGETS_RECOVERY_FAILURE &&
		f.lastFaultInvocation == 1 &&
		f.lastFaultResult == RENDER_RESULT_FAILED);
	CHECK(device->configureResourceFaultInjection(
		static_cast<RenderResourceFaultPoint>(
			RENDER_RESOURCE_FAULT_TEXTURE_COPY_AFTER_ISSUE + 1), 1,
		RENDER_RESULT_FAILED) == RENDER_RESULT_INVALID_ARGUMENT &&
		f.faultConfigCalls == 5);
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
	const unsigned int lastStage = LEGACY_TEXTURE_STAGE_COUNT - 1;
	const unsigned int lastVertexConstant = LEGACY_VERTEX_CONSTANT_COUNT - 1;
	const unsigned int lastPixelConstant = LEGACY_PIXEL_CONSTANT_COUNT - 1;
	state.pipeline.textureStages[lastStage].projectedCoordinates = true;
	state.pipeline.textureStages[lastStage].bumpEnvironmentLuminanceOffset = 17;
	state.constants.textureTransforms[lastStage].values[15] = 23;
	state.constants.vertexShaderConstants[lastVertexConstant] = RenderFloat4(29, 31, 37, 41);
	state.constants.pixelShaderConstants[lastPixelConstant] = RenderFloat4(43, 47, 53, 59);
	const LegacyLogicalState expectedLayoutState = state;
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
	state.pipeline.textureStages[lastStage].projectedCoordinates = false;
	state.pipeline.textureStages[lastStage].bumpEnvironmentLuminanceOffset = 99;
	state.constants.textureTransforms[lastStage].values[15] = 99;
	state.constants.vertexShaderConstants[lastVertexConstant] = RenderFloat4(99, 99, 99, 99);
	state.constants.pixelShaderConstants[lastPixelConstant] = RenderFloat4(99, 99, 99, 99);
	f.release();
	const ThreadedRenderFrameCompletion completion = Complete(device.get());
	CHECK(completion.presented && completion.outcome.wasPresented() && completion.outcome.frameEnded());
	CHECK(f.owner != std::this_thread::get_id() && !f.wrongThread);
	CHECK(f.bufferBytes == std::vector<unsigned char>(16, 11));
	CHECK(f.updateBytes == std::vector<unsigned char>(16, 22));
	CHECK(f.textureBytes.size() == 20 && f.textureBytes.front() == 33 && f.textureBytes.back() == 44);
	CHECK(f.refreshBytes.size() == 20 && f.refreshBytes.front() == 55 && f.refreshBytes.back() == 66);
	CHECK(f.stateValue == 9 && f.layoutStride == 24 && f.layoutOffset == 12);
	CHECK(f.layoutValue.elementCount == 1 &&
		f.layoutState.pipeline.textureStages[lastStage].projectedCoordinates &&
		f.layoutState.pipeline.textureStages[lastStage].bumpEnvironmentLuminanceOffset == 17 &&
		f.layoutState.constants.textureTransforms[lastStage].values[15] == 23);
	CHECK(std::memcmp(&f.layoutState.constants.vertexShaderConstants[lastVertexConstant],
		&expectedLayoutState.constants.vertexShaderConstants[lastVertexConstant], sizeof(RenderFloat4)) == 0);
	CHECK(std::memcmp(&f.layoutState.constants.pixelShaderConstants[lastPixelConstant],
		&expectedLayoutState.constants.pixelShaderConstants[lastPixelConstant], sizeof(RenderFloat4)) == 0);
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

void ProducerFailureTraceIsOptInAndRateLimited()
{
#ifdef _WIN32
	struct TraceEnvironment
	{
		TraceEnvironment() : hadPrevious(std::getenv("RTS_RENDER_FAILURE_TRACE") != 0),
			enabled(false)
		{
			path[0] = '\0';
			const char *previous = std::getenv("RTS_RENDER_FAILURE_TRACE");
			if (previous != 0) previousPath = previous;
		}
		~TraceEnvironment()
		{
			if (enabled)
				_putenv_s("RTS_RENDER_FAILURE_TRACE",
					hadPrevious ? previousPath.c_str() : "");
			if (path[0] != '\0') DeleteFileA(path);
		}
		bool configure()
		{
			char directory[MAX_PATH];
			const DWORD length = GetCurrentDirectoryA(MAX_PATH, directory);
			if (length == 0 || length >= MAX_PATH ||
				GetTempFileNameA(directory, "rft", 0, path) == 0)
				return false;
			enabled = _putenv_s("RTS_RENDER_FAILURE_TRACE", path) == 0;
			return enabled;
		}
		bool hadPrevious;
		bool enabled;
		char path[MAX_PATH];
		std::string previousPath;
	} trace;
	CHECK(trace.configure());

	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(LEGACY_TEXTURE_STAGE_COUNT, GpuHandle()) ==
		RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->setTexture(LEGACY_TEXTURE_STAGE_COUNT + 1, GpuHandle()) ==
		RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->endFrame() == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(!Complete(device.get(), RENDER_RESULT_INVALID_ARGUMENT).presented);

	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(LEGACY_TEXTURE_STAGE_COUNT, GpuHandle()) ==
		RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->endFrame() == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(!Complete(device.get(), RENDER_RESULT_INVALID_ARGUMENT).presented);

	EmptyFrame(device.get());
	CHECK(Complete(device.get()).presented);

	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setTexture(LEGACY_TEXTURE_STAGE_COUNT, GpuHandle()) ==
		RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(context->endFrame() == RENDER_RESULT_INVALID_ARGUMENT);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(!Complete(device.get(), RENDER_RESULT_INVALID_ARGUMENT).presented);

	std::ifstream traceFile(trace.path, std::ios::binary);
	const std::string contents((std::istreambuf_iterator<char>(traceFile)),
		std::istreambuf_iterator<char>());
	const std::string marker("renderer_failure source=producer");
	std::size_t markerCount = 0, offset = 0;
	while ((offset = contents.find(marker, offset)) != std::string::npos)
	{
		++markerCount;
		offset += marker.size();
	}
	CHECK(markerCount == 2);
	CHECK(contents.find("op=setTexture") != std::string::npos);
	CHECK(contents.find("arg0=8") != std::string::npos);
	CHECK(contents.find("arg2=8") != std::string::npos);
	CHECK(contents.find("arg3=1") != std::string::npos);
	CHECK(contents.find("frame=1") != std::string::npos);
	CHECK(contents.find("frame=4") != std::string::npos);
#endif
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

void FragmentedBufferDiscardReplacesRanges()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	unsigned int words[4] = { 11, 13, 17, 19 };
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(words);
	descriptor.stride = sizeof(words[0]);
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	descriptor.binding = RENDER_BUFFER_VERTEX;
	GpuHandle buffer;
	CHECK(device->createBuffer(descriptor, 0, 0, &buffer) == RENDER_RESULT_OK);
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, words, sizeof(words[0]), 0,
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, words + 3, sizeof(words[3]), 3 * sizeof(words[0]),
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(buffer, sizeof(words[0]), 0) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 0) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 3) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented && f.draws == 2);
	// DISCARD replaces two separated initialized ranges with only its prefix.
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, words, sizeof(words[0]), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(buffer, sizeof(words[0]), 0) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 0) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 3) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get(), RENDER_RESULT_FAILED).resourceFailure && f.draws == 3);
	// A subsequent PRESERVE adds its range without reviving the old tail or hole.
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, words + 2, sizeof(words[2]), 2 * sizeof(words[0]),
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(buffer, sizeof(words[0]), 0) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 0) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 2) == RENDER_RESULT_OK);
	CHECK(context->draw(1, 1) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get(), RENDER_RESULT_FAILED).resourceFailure && f.draws == 5);
	CHECK(device->destroyResource(buffer));
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
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

void BufferMutationFailureIsIsolated()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	BufferDescriptor failedDescriptor;
	failedDescriptor.byteCount = 16;
	failedDescriptor.stride = 16;
	failedDescriptor.usage = RENDER_USAGE_DYNAMIC;
	failedDescriptor.binding = RENDER_BUFFER_VERTEX;
	BufferDescriptor stableDescriptor = failedDescriptor;
	stableDescriptor.usage = RENDER_USAGE_DEFAULT;
	unsigned char failedBytes[16]; std::memset(failedBytes, 31, sizeof(failedBytes));
	unsigned char stableBytes[16]; std::memset(stableBytes, 47, sizeof(stableBytes));
	GpuHandle failedBuffer, stableBuffer;
	CHECK(device->createBuffer(failedDescriptor, failedBytes,
		sizeof(failedBytes), &failedBuffer) == RENDER_RESULT_OK);
	CHECK(device->createBuffer(stableDescriptor, stableBytes,
		sizeof(stableBytes), &stableBuffer) == RENDER_RESULT_OK);
	f.failUpdate = true;
	f.updateFailureResult = RENDER_RESULT_FAILED;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(failedBuffer, failedBytes,
		sizeof(failedBytes), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
		RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	const ThreadedRenderFrameCompletion failed = Complete(device.get(),
		RENDER_RESULT_FAILED);
	CHECK(failed.resourceFailure && device->isOperational());
	f.failUpdate = false;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(stableBuffer, 16, 0) == RENDER_RESULT_OK);
	CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
		RENDER_RESULT_OK);
	CHECK(context->draw(1, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
	CHECK(device->destroyResource(failedBuffer));
	CHECK(device->destroyResource(stableBuffer));
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
}

void SuccessfulCpuUploadSurvivesUnrelatedFrameFailure()
{
	Fixture f;
	auto device = Device(f);
	IRenderContext *context = device->immediateContext();
	BufferDescriptor descriptor;
	descriptor.byteCount = 16;
	descriptor.stride = 16;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	descriptor.binding = RENDER_BUFFER_VERTEX;
	unsigned char bytes[16]; std::memset(bytes, 61, sizeof(bytes));
	GpuHandle buffer;
	CHECK(device->createBuffer(descriptor, bytes, sizeof(bytes), &buffer) ==
		RENDER_RESULT_OK);
	f.failDraw = true;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->updateBuffer(buffer, bytes, sizeof(bytes), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK);
	// This deliberately failing draw does not consume the uploaded buffer.
	CHECK(context->draw(3, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	const ThreadedRenderFrameCompletion failed = Complete(device.get(),
		RENDER_RESULT_FAILED);
	CHECK(failed.outcome.hasCommandFailure() && !failed.presented &&
		!failed.resourceFailure);
	f.failDraw = false;
	CHECK(context->beginFrame() == RENDER_RESULT_OK);
	CHECK(context->setVertexBuffer(buffer, descriptor.stride, 0) ==
		RENDER_RESULT_OK);
	CHECK(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
		RENDER_RESULT_OK);
	CHECK(context->draw(1, 0) == RENDER_RESULT_OK);
	CHECK(context->endFrame() == RENDER_RESULT_OK);
	CHECK(device->present() == RENDER_RESULT_OK);
	CHECK(Complete(device.get()).presented);
	CHECK(device->destroyResource(buffer));
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
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

void EmptyCompletionPollingPreservesOutputAndAuthority()
{
	Fixture f;
	auto device = Device(f);
	ThreadedRenderFrameCompletion completion;
	completion.sequence = 987; completion.result = RENDER_RESULT_UNSUPPORTED;
	completion.resourceFailure = completion.presented = completion.operational = true;
	CHECK(!PollThreadedRenderCompletion(device.get(), 0));
	CHECK(!PollThreadedRenderCompletion(0, &completion));
	CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(completion.sequence == 987 && completion.result == RENDER_RESULT_UNSUPPORTED &&
		completion.resourceFailure && completion.presented && completion.operational);
	ReleaseGate release(f); f.gateEvent = BEGIN;
	EmptyFrame(device.get()); f.waitForGate();
	// A reserved/pending frame is not a completed record.
	for (unsigned int i = 0; i < 128; ++i)
		CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(completion.sequence == 987 && completion.result == RENDER_RESULT_UNSUPPORTED);
	f.release(); CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
	bool offOwnerResult = true;
	std::thread offOwner([&] { offOwnerResult = PollThreadedRenderCompletion(device.get(), &completion); });
	offOwner.join();
	CHECK(!offOwnerResult && completion.sequence == 987);
	CHECK(!PollThreadedRenderCompletion(device.get(), 0));
	CHECK(PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(completion.sequence == 1 && completion.result == RENDER_RESULT_OK &&
		completion.presented && completion.operational && !completion.resourceFailure);
	CHECK(!PollThreadedRenderCompletion(device.get(), &completion) && completion.sequence == 1);
}

void CompletionMailboxWrapAndRecoveryRetention()
{
	Fixture f;
	auto device = Device(f);
	ThreadedRenderFrameCompletion completion;
	uint64_t expected = 0;
	for (unsigned int round = 0; round < 4; ++round)
	{
		for (unsigned int i = 0; i < 64; ++i) EmptyFrame(device.get(), (i & 1) == 0);
		CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
		CHECK(device->immediateContext()->beginFrame() == RENDER_RESULT_OUT_OF_MEMORY);
		for (unsigned int i = 0; i < 32; ++i)
		{
			CHECK(PollThreadedRenderCompletion(device.get(), &completion));
			CHECK(completion.sequence == ++expected && completion.presented == ((i & 1) == 0));
		}
		// Refill before draining the remaining records, crossing the ring boundary.
		for (unsigned int i = 0; i < 32; ++i) EmptyFrame(device.get(), (i & 1) == 0);
		CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
		CHECK(device->immediateContext()->beginFrame() == RENDER_RESULT_OUT_OF_MEMORY);
		for (unsigned int i = 0; i < 64; ++i)
		{
			CHECK(PollThreadedRenderCompletion(device.get(), &completion));
			CHECK(completion.sequence == ++expected && completion.presented == ((i & 1) == 0));
		}
		CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
	}
	f.failPresent = true; EmptyFrame(device.get());
	CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_DEVICE_REMOVED);
	f.failPresent = false;
	CHECK(device->recoverDevice() == RENDER_RESULT_OK && device->isOperational());
	// Recovery cannot erase the old asynchronous failure record.
	CHECK(PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(completion.sequence == ++expected && completion.result == RENDER_RESULT_DEVICE_REMOVED &&
		completion.outcome.hasDeviceRemoval() && !completion.operational && !completion.presented);
	EmptyFrame(device.get()); device->shutdown();
	CHECK(PollThreadedRenderCompletion(device.get(), &completion));
	CHECK(completion.sequence == ++expected && completion.result == RENDER_RESULT_OK && completion.presented);
	CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
}

void ConcurrentCompletionPollingPreservesFifo()
{
	Fixture f;
	auto device = Device(f);
	ThreadedRenderFrameCompletion completion;
	uint64_t popped = 0;
	for (unsigned int i = 0; i < 512; ++i)
	{
		EmptyFrame(device.get());
		// Race actual owner publication, without assuming a particular interleaving.
		while (PollThreadedRenderCompletion(device.get(), &completion))
			CHECK(completion.sequence == ++popped && completion.result == RENDER_RESULT_OK);
		if ((i & 15) == 15)
		{
			CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
			while (PollThreadedRenderCompletion(device.get(), &completion))
				CHECK(completion.sequence == ++popped && completion.result == RENDER_RESULT_OK);
			CHECK(popped == i + 1);
		}
	}
	CHECK(popped == 512 && !PollThreadedRenderCompletion(device.get(), &completion));
}

void BenchmarkCompletionPolling()
{
	const unsigned int calls = 2000000, warmup = 100000, rounds = 1000;
	std::atomic<unsigned int> ready(0);
	CHECK(ready.is_lock_free());
	std::printf("atomic_size=%zu atomic_lock_free=%d calls=%u warmup=%u nonempty_calls=%u\n",
		sizeof(ready), ready.is_lock_free(), calls, warmup, rounds * 64);
	Fixture f;
	auto device = Device(f);
	ThreadedRenderFrameCompletion completion;
	struct ReleaseBusyDraw
	{
		explicit ReleaseBusyDraw(Fixture &fixture) : state(fixture) {}
		~ReleaseBusyDraw() { state.busyRelease.store(true); }
		Fixture &state;
	} releaseBusy(f);
	for (unsigned int context = 0; context < 2; ++context)
	{
		if (context)
		{
			f.busyDraw = true;
			CHECK(device->immediateContext()->beginFrame() == RENDER_RESULT_OK);
			CHECK(device->immediateContext()->draw(3, 0) == RENDER_RESULT_OK);
			CHECK(device->immediateContext()->endFrame() == RENDER_RESULT_OK);
			CHECK(SubmitThreadedRenderFrame(device.get(), false) == RENDER_RESULT_OK);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!f.busyEntered.load()) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
		}
		for (unsigned int i = 0; i < warmup; ++i) CHECK(!PollThreadedRenderCompletion(device.get(), &completion));
		const auto start = std::chrono::steady_clock::now();
		unsigned int hits = 0;
		for (unsigned int i = 0; i < calls; ++i) hits += PollThreadedRenderCompletion(device.get(), &completion);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		f.busyRelease.store(true); // Release before any assertion can unwind the device.
		CHECK(hits == 0);
		std::printf("context=%s ns_per_call=%.6f hits=%u\n", context ? "owner_cpu_busy_draw" : "owner_idle",
			std::chrono::duration<double, std::nano>(elapsed).count() / calls, hits);
		if (context) { Complete(device.get()); f.busyDraw = false; }
		else f.busyRelease.store(false);
	}
	double nonemptyNanoseconds = 0;
	for (unsigned int round = 0; round < rounds; ++round)
	{
		for (unsigned int i = 0; i < 64; ++i) EmptyFrame(device.get(), false);
		CHECK(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK);
		const auto start = std::chrono::steady_clock::now();
		unsigned int hits = 0;
		for (unsigned int i = 0; i < 64; ++i) hits += PollThreadedRenderCompletion(device.get(), &completion);
		nonemptyNanoseconds += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
		CHECK(hits == 64 && !PollThreadedRenderCompletion(device.get(), &completion));
	}
	std::printf("context=nonempty_64_batch ns_per_call=%.6f\n", nonemptyNanoseconds / (rounds * 64));
	// Timer + loop-floor receipt; not subtracted from API timings.
	volatile unsigned int sink = 0;
	const auto start = std::chrono::steady_clock::now();
	for (unsigned int i = 0; i < calls; ++i) sink = sink + (i & 1);
	std::printf("context=volatile_loop_floor ns_per_call=%.6f sink=%u\n",
		std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls, sink);
}

struct OperationalStateObserver
{
	OperationalStateObserver(IRenderDevice *device, bool expected) :
		started(false), stop(false), matched(false), reader([this, device, expected]
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!stop.load(std::memory_order_acquire))
			{
				const bool operational = device->isOperational();
				started.store(true, std::memory_order_release);
				if (operational == expected)
				{
					matched.store(true, std::memory_order_release);
					return;
				}
				if (std::chrono::steady_clock::now() >= deadline) return;
				std::this_thread::yield();
			}
		}) {}
	~OperationalStateObserver()
	{
		stop.store(true, std::memory_order_release);
		if (reader.joinable()) reader.join();
	}
	void waitForFirstQuery()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!started.load(std::memory_order_acquire))
		{
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::yield();
		}
	}
	void wait()
	{
		reader.join();
		CHECK(matched.load(std::memory_order_acquire));
	}
	std::atomic<bool> started, stop, matched;
	std::thread reader;
};

void ConcurrentOperationalPublicationAndLifecycle()
{
	Fixture f;
	auto device = Device(f);
	CHECK(device->isOperational());
	{
		ReleaseGate release(f);
		f.gateEvent = PRESENT; f.failPresent = true;
		EmptyFrame(device.get()); f.waitForGate();
		// This gate holds backend execution, not the queue mutex. A concurrent
		// reader observes the last owner publication until the failed Present ends.
		OperationalStateObserver active(device.get(), true);
		active.wait();
		OperationalStateObserver removed(device.get(), false);
		removed.waitForFirstQuery();
		f.release();
		removed.wait(); // Observe asynchronous removal before draining completion.
		const ThreadedRenderFrameCompletion completion =
			Complete(device.get(), RENDER_RESULT_DEVICE_REMOVED);
		CHECK(!completion.operational && !completion.outcome.isOperational());
	}
	f.failPresent = false;
	OperationalStateObserver recovered(device.get(), true);
	recovered.waitForFirstQuery();
	CHECK(device->recoverDevice() == RENDER_RESULT_OK);
	recovered.wait();
	EmptyFrame(device.get());
	CHECK(Complete(device.get()).operational);
	OperationalStateObserver stopped(device.get(), false);
	stopped.waitForFirstQuery();
	device->shutdown();
	stopped.wait();
	CHECK(!device->isOperational());
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
	CHECK(!second->isOperational());
	CHECK(second->initialize(parameters) == RENDER_RESULT_FAILED); // one render owner
	CHECK(!second->isOperational());
	second.reset(); CHECK(rejected.factoryCalls == 0);
	device.reset();
	Fixture failed; failed.failInitialize = true;
	std::unique_ptr<IRenderDevice> bad(CreateThreadedRenderDevice(Factory, &failed));
	CHECK(!bad->isOperational());
	CHECK(bad->initialize(parameters) == RENDER_RESULT_FAILED);
	CHECK(!bad->isOperational());
	bad->shutdown(); CHECK(!bad->isOperational());
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

int main(int argc, char **argv)
{
	try
	{
		CHECK(rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_GAME));
#if defined(_WIN64)
		if (argc == 2 && std::strcmp(argv[1], "--pipeline-stall-trace") == 0)
		{
			PipelineTraceOffCapOwnershipAndExport();
			PipelineTraceStagesFailedExports();
			PipelineTraceFollowsProductionWaitsAndShutdown();
			CHECK(rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_GAME));
			std::puts("Production pipeline stall trace contracts passed");
			return 0;
		}
#endif
		if (argc == 2 && std::strcmp(argv[1], "--benchmark-completion-poll") == 0)
		{
			BenchmarkCompletionPolling();
			CHECK(rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_GAME));
			return 0;
		}
#if defined(_WIN64)
		RenderOwnerDiagnosticsFollowActualPacketExecution();
#endif
		ProducerTextureBindingCachePreservesOrderedInvalidation();
		ProducerTopologyCachePreservesAdmissionAndFailure();
		SwapIntervalOwnerTransport();
		GammaOwnerTransport();
		TextureFilterCapabilitiesArePublishedFromOwner();
		DebugResourceOwnerTransport();
		OwnershipAndDeepCopy();
		SynchronousProducerRejectionsDoNotPoisonNextFrame();
		GenerationsAndResourceFailure();
		ProducerFailureTraceIsOptInAndRateLimited();
		FailurePublicationAndRecovery();
		FragmentedBufferDiscardReplacesRanges();
		BufferUpdateFailureRecoveryRestoresBinding();
		BufferMutationFailureIsIsolated();
		SuccessfulCpuUploadSurvivesUnrelatedFrameFailure();
		ResourcePreambleRemovalRemainsObservable();
		RecoveryClearsPreambleResourceFailure();
		CaptureResizeAndNonVisibleOrdering();
		OpenFrameReportIsRejectedBeforeExecution();
		FailedGpuCopyDependencies();
		OverlapAndBoundedBackpressure();
		PacketSegmentationAndBudgetFailure();
		CompletionAdmissionAndShutdown();
		EmptyCompletionPollingPreservesOutputAndAuthority();
		CompletionMailboxWrapAndRecoveryRetention();
		ConcurrentCompletionPollingPreservesFifo();
		ConcurrentOperationalPublicationAndLifecycle();
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
