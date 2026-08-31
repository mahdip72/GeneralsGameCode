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
#include <climits>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

namespace rts
{
namespace render
{
ThreadedRenderOptions::ThreadedRenderOptions() : serial(false),
	maxFramesInFlight(3), maxPacketBytes(256 * 1024 * 1024 + 64 * 1024),
	maxPacketCommands(65536), resourceCapacity(65536) {}
ThreadedRenderFrameCompletion::ThreadedRenderFrameCompletion() : sequence(0),
	result(RENDER_RESULT_OK), resourceFailure(false), presented(false), operational(false) {}
ThreadedRenderMetrics::ThreadedRenderMetrics() : submittedFrames(0),
	completedFrames(0), failedFrames(0), producerOverlapFrames(0),
	backpressureWaits(0), producerWaitNanoseconds(0), ownerExecutionNanoseconds(0),
	rejectedPackets(0), pendingPackets(0), peakPendingPackets(0), peakPacketBytes(0) {}

namespace
{
typedef std::chrono::steady_clock Clock;
uint64_t Nanoseconds(Clock::duration duration)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(duration).count());
}

void DispatchSentWindowMessages()
{
#ifdef _WIN32
	// DXGI may SendMessage to the window's producer while executing on the
	// render owner. Do not DispatchMessage/GetMessage here: posted game/input
	// events would re-enter the engine inside a render transaction. PeekMessage
	// services nonqueued sent messages even though no posted message is removed.
	MSG message;
	PeekMessageW(&message, 0, WM_NULL, WM_NULL, PM_NOREMOVE | PM_QS_SENDMESSAGE);
#endif
}

RenderResult FirstFailure(RenderResult first, RenderResult next)
{
	if (next == RENDER_RESULT_DEVICE_REMOVED) return next;
	return first == RENDER_RESULT_OK ? next : first;
}

template <typename Call> RenderResult BackendCall(Call call)
{
	try { return call(); }
	catch (const std::bad_alloc &) { return RENDER_RESULT_OUT_OF_MEMORY; }
	catch (...) { return RENDER_RESULT_FAILED; }
}

bool Multiply(size_t a, size_t b, size_t *value)
{
	if (a && b > (std::numeric_limits<size_t>::max)() / a) return false;
	*value = a * b;
	return true;
}

unsigned int PixelBytes(RenderFormat format)
{
	switch (format)
	{
	case RENDER_FORMAT_R8G8_SNORM: case RENDER_FORMAT_R16_UINT: return 2;
	case RENDER_FORMAT_R8G8B8A8_UNORM: case RENDER_FORMAT_B8G8R8A8_UNORM:
	case RENDER_FORMAT_D24_UNORM_S8_UINT: case RENDER_FORMAT_R32_UINT: return 4;
	default: return 0;
	}
}

bool SameTexture(const TextureDescriptor &a, const TextureDescriptor &b)
{
	return a.width == b.width && a.height == b.height && a.mipCount == b.mipCount &&
		a.arrayCount == b.arrayCount && a.dimension == b.dimension && a.format == b.format &&
		a.binding == b.binding && a.usage == b.usage;
}

enum Operation
{
	OP_BEGIN, OP_CREATE_BUFFER, OP_CREATE_TEXTURE, OP_REFRESH_TEXTURE,
	OP_DESTROY, OP_UPDATE_BUFFER, OP_CLEAR, OP_TARGETS, OP_VIEWPORT,
	OP_LEGACY_STATE, OP_LEGACY_LAYOUT, OP_VERTEX_BUFFER, OP_INDEX_BUFFER,
	OP_TEXTURE, OP_TOPOLOGY, OP_DRAW, OP_DRAW_INDEXED, OP_COPY_COLOR
};
enum Control { CONTROL_NONE, CONTROL_FENCE, CONTROL_CAPTURE,
	CONTROL_RESIZE, CONTROL_RECOVER, CONTROL_DEBUG_COUNT, CONTROL_REPORT };

struct Command
{
	explicit Command(Operation value) : operation(value), handle(),
		payloadOffset(0), dataOffset(0), dataBytes(0), destinationOffset(0),
		signedValue(0)
	{
		std::memset(integers, 0, sizeof(integers));
		std::memset(floats, 0, sizeof(floats));
	}
	Operation operation;
	GpuHandle handle;
	size_t payloadOffset, dataOffset, dataBytes, destinationOffset;
	unsigned int integers[4];
	float floats[6];
	int signedValue;
};
struct TextureSpan { size_t offset, rowPitch, slicePitch; };
struct LayoutState { LegacyLogicalState state; LegacyVertexLayout layout; };

struct Reply
{
	Reply() : done(false), result(RENDER_RESULT_OK), format(RENDER_FORMAT_UNKNOWN),
		rowPitch(0), count(0), width(0), height(0) {}
	bool done;
	RenderResult result;
	RenderFormat format;
	size_t rowPitch;
	unsigned int count, width, height;
	std::vector<unsigned char> pixels;
};

struct Packet
{
	Packet() { reset(); }
	void reset()
	{
		commands.clear(); bytes.clear(); sequence = 0; closeFrame = false;
		finalFrame = false; present = false; failure = RENDER_RESULT_OK;
		control = CONTROL_NONE; reply.reset();
	}
	std::vector<Command> commands;
	std::vector<unsigned char> bytes;
	uint64_t sequence;
	bool closeFrame, finalFrame, present;
	RenderResult failure;
	Control control;
	std::shared_ptr<Reply> reply;
};

struct ProducerResource
{
	ProducerResource() : texture(false) {}
	bool texture;
	BufferDescriptor buffer;
	TextureDescriptor descriptor;
};
struct OwnerResource
{
	OwnerResource() : generation(0), backend(), texture(false), contentValid(false),
		recoverySourceValid(false), gpuAuthoritative(false), byteCount(0), writtenSequence(0) {}
	unsigned int generation;
	GpuHandle backend;
	bool texture;
	bool contentValid;
	// A successful CPU buffer create/update leaves a durable source that the
	// native backend can use to recreate the buffer.  This is independent of
	// whether the current native resource contents are still valid.
	bool recoverySourceValid;
	bool gpuAuthoritative;
	size_t byteCount;
	uint64_t writtenSequence;
};

class ThreadedRenderDevice final : public IRenderDevice, public IRenderContext
{
public:
	ThreadedRenderDevice(ThreadedRenderBackendFactory factory, void *factoryContext,
		const ThreadedRenderOptions &options) : m_factory(factory),
		m_factoryContext(factoryContext), m_options(options),
		m_producer(std::this_thread::get_id()), m_waiting(false), m_initialized(false),
		m_started(false), m_stopping(false), m_operational(false),
		m_ownerExecuting(false),
		m_initialResult(RENDER_RESULT_FAILED), m_infoResult(RENDER_RESULT_FAILED),
		m_current(0), m_queueRead(0), m_queueCount(0), m_pending(0),
		m_completionRead(0), m_completionCount(0), m_reservedCompletions(0),
		m_completedSequence(0), m_completedResult(RENDER_RESULT_OK),
		m_recording(false), m_ended(false), m_nextSequence(1), m_sequence(0),
		m_lastSequence(0), m_producerFailure(RENDER_RESULT_OK),
		m_backend(0), m_context(0), m_ownerFrameOpen(false), m_ownerFrameActive(false), m_ownerDeviceRemoved(false),
		m_ownerResourceFailure(false), m_outsideResourceFailure(false), m_ownerSequence(0),
		m_ownerFrameResult(RENDER_RESULT_OK), m_outsideFailure(RENDER_RESULT_OK),
		m_drainFailure(RENDER_RESULT_OK)
	{
		m_handles.reset(new GpuHandleAllocator(options.resourceCapacity));
		if (m_handles->capacity() != options.resourceCapacity) throw std::bad_alloc();
		m_producerResources.resize(options.resourceCapacity);
		m_ownerResources.resize(options.resourceCapacity);
		m_packets.reserve(options.maxFramesInFlight);
		m_free.reserve(options.maxFramesInFlight);
		m_queue.resize(options.maxFramesInFlight);
		for (unsigned int i = 0; i < options.maxFramesInFlight; ++i)
		{
			m_packets.emplace_back(new Packet);
			m_free.push_back(m_packets.back().get());
		}
	}
	~ThreadedRenderDevice() override { shutdown(); }
	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_operational;
	}
	RenderResult initialize(const RenderDeviceParameters &parameters) override;
	void shutdown() override;
	IRenderContext *immediateContext() override { return producer() && m_initialized ? this : 0; }
	RenderResult createBuffer(const BufferDescriptor &, const void *, size_t, GpuHandle *) override;
	RenderResult createTexture(const TextureDescriptor &, const TextureSubresourceData *,
		unsigned int, GpuHandle *) override;
	RenderResult refreshTexture(GpuHandle, const TextureDescriptor &,
		const TextureSubresourceData *, unsigned int) override;
	RenderResult copyActiveColorTargetToTexture(GpuHandle) override;
	bool destroyResource(GpuHandle) override;
	RenderResult recoverDevice() override { return lifecycle(CONTROL_RECOVER, 0, 0); }
	RenderResult resize(unsigned int w, unsigned int h) override { return lifecycle(CONTROL_RESIZE, w, h); }
	RenderResult present() override { return submitFrame(true); }
	RenderResult getBackBufferInfo(RenderBackBufferInfo *info) const override
	{
		if (!info) return RENDER_RESULT_INVALID_ARGUMENT;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_infoResult == RENDER_RESULT_OK) *info = m_info;
		return m_infoResult;
	}
	RenderResult captureBackBuffer(void *, size_t, size_t, RenderFormat *) override;
	RenderResult getDebugValidationErrorCount(unsigned int *) const override;
	RenderResult reportDebugLiveObjects() override { return lifecycle(CONTROL_REPORT, 0, 0); }
	RenderResult beginFrame() override;
	RenderResult endFrame() override;
	RenderResult updateBuffer(GpuHandle, const void *, size_t, size_t, RenderBufferUpdateMode) override;
	RenderResult clear(const RenderFloat4 &c, float d, unsigned int s) override
	{ return clearTargets(RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL, c, d, s); }
	RenderResult clearTargets(unsigned int, const RenderFloat4 &, float, unsigned int) override;
	RenderResult setRenderTargets(const RenderTargetBinding &) override;
	RenderResult setRenderTargets(GpuHandle, GpuHandle) override;
	RenderResult setViewport(float, float, float, float, float, float) override;
	RenderResult setLegacyState(const LegacyLogicalState &, LegacyVertexFormat, unsigned int) override;
	RenderResult setLegacyStateForLayout(const LegacyLogicalState &, const LegacyVertexLayout &, unsigned int) override;
	RenderResult setVertexBuffer(GpuHandle, unsigned int, unsigned int) override;
	RenderResult setIndexBuffer(GpuHandle, RenderFormat, unsigned int) override;
	RenderResult setTexture(unsigned int, GpuHandle) override;
	RenderResult setPrimitiveTopology(RenderPrimitiveTopology) override;
	RenderResult draw(unsigned int, unsigned int) override;
	RenderResult drawIndexed(unsigned int, unsigned int, int) override;
	RenderResult submitFrame(bool);
	RenderResult cancelFrame(RenderResult);
	RenderResult drain();
	bool poll(ThreadedRenderFrameCompletion *);
	uint64_t lastSequence() const { return producer() ? m_lastSequence : 0; }
	bool metrics(ThreadedRenderMetrics *) const;

private:
	bool producer() const { return std::this_thread::get_id() == m_producer && !m_waiting; }
	bool usable() const { return producer() && m_initialized; }
	RenderResult fail(RenderResult result)
	{
		// Synchronous producer validation/capacity failures outside a frame are
		// returned to the caller directly.  Only failures while a frame is being
		// recorded belong in that frame's packet; accepted asynchronous resource
		// commands still report owner-side failures from execute().
		if (producer() && m_recording)
			m_producerFailure = FirstFailure(m_producerFailure, result);
		return result;
	}
	bool valid(GpuHandle h, bool texture, bool nullable = false) const
	{
		return (!h.isValid() && nullable) || (m_handles->isLive(h) &&
			m_producerResources[h.index()].texture == texture);
	}
	template <typename Predicate> void wait(std::unique_lock<std::mutex> &lock, Predicate ready)
	{
		const Clock::time_point start = Clock::now();
		m_waiting = true;
		while (!ready())
		{
			lock.unlock();
			DispatchSentWindowMessages();
			lock.lock();
			if (!ready()) m_changed.wait_for(lock, std::chrono::milliseconds(1));
		}
		m_waiting = false;
		m_metrics.producerWaitNanoseconds += Nanoseconds(Clock::now() - start);
	}
	bool acquire();
	bool reservePayload(size_t bytes, size_t commands = 1);
	size_t copyPayload(const void *data, size_t bytes);
	RenderResult append(Command command, const void *payload = 0, size_t bytes = 0,
		bool requireFrame = true);
	RenderResult textureCommand(Operation, GpuHandle, const TextureDescriptor &,
		const TextureSubresourceData *, unsigned int);
	RenderResult flush(Control, const std::shared_ptr<Reply> &, bool finalFrame, bool visible);
	RenderResult sync(Control, const std::shared_ptr<Reply> &);
	RenderResult lifecycle(Control, unsigned int, unsigned int);
	void run(RenderDeviceParameters);
	void execute(Packet &);
	RenderResult executeCommand(const Packet &, const Command &);
	GpuHandle resolve(GpuHandle handle) const;
	OwnerResource *ownerResource(GpuHandle handle);
	void writeTarget(GpuHandle handle, bool establishesContents);
	template <typename T> T read(const Packet &packet, size_t offset) const
	{
		static_assert(std::is_trivially_copyable<T>::value, "Packet payload must be a value type");
		T value;
		std::memcpy(&value, packet.bytes.data() + offset, sizeof(value));
		return value;
	}
	void publishMetadata(RenderResult, bool refreshInfo = false);

	ThreadedRenderBackendFactory m_factory;
	void *m_factoryContext;
	ThreadedRenderOptions m_options;
	std::thread::id m_producer;
	bool m_waiting, m_initialized, m_started, m_stopping, m_operational;
	bool m_ownerExecuting;
	RenderResult m_initialResult, m_infoResult;
	RenderBackBufferInfo m_info;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_thread;
	std::vector<std::unique_ptr<Packet> > m_packets;
	std::vector<Packet *> m_free, m_queue;
	Packet *m_current;
	size_t m_queueRead, m_queueCount, m_pending;
	enum { COMPLETION_CAPACITY = 64 };
	ThreadedRenderFrameCompletion m_completions[COMPLETION_CAPACITY];
	size_t m_completionRead, m_completionCount, m_reservedCompletions;
	uint64_t m_completedSequence;
	RenderResult m_completedResult;
	ThreadedRenderMetrics m_metrics;
	std::unique_ptr<GpuHandleAllocator> m_handles;
	std::vector<ProducerResource> m_producerResources;
	std::vector<OwnerResource> m_ownerResources;
	RenderTargetBinding m_targets;
	bool m_recording, m_ended;
	uint64_t m_nextSequence, m_sequence, m_lastSequence;
	RenderResult m_producerFailure;
	// Everything below this line belongs exclusively to the render owner.
	IRenderDevice *m_backend;
	IRenderContext *m_context;
	bool m_ownerFrameOpen, m_ownerFrameActive, m_ownerDeviceRemoved;
	bool m_ownerResourceFailure, m_outsideResourceFailure;
	uint64_t m_ownerSequence;
	GpuHandle m_ownerColorTarget, m_ownerDepthTarget;
	RenderFrameOutcome m_ownerOutcome;
	RenderResult m_ownerFrameResult, m_outsideFailure, m_drainFailure;
};

RenderResult ThreadedRenderDevice::initialize(const RenderDeviceParameters &parameters)
{
	if (!producer() || m_initialized || m_thread.joinable() || m_stopping) return RENDER_RESULT_INVALID_ARGUMENT;
	if (parameters.backend != RENDER_BACKEND_D3D11) return RENDER_RESULT_UNSUPPORTED;
	try { m_thread = std::thread(&ThreadedRenderDevice::run, this, parameters); }
	catch (...) { return RENDER_RESULT_OUT_OF_MEMORY; }
	std::unique_lock<std::mutex> lock(m_mutex);
	wait(lock, [this] { return m_started; });
	m_initialized = m_initialResult == RENDER_RESULT_OK;
	return m_initialResult;
}

bool ThreadedRenderDevice::acquire()
{
	if (m_current) return true;
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_free.empty())
	{
		++m_metrics.backpressureWaits;
		wait(lock, [this] { return !m_free.empty(); });
	}
	m_current = m_free.back();
	m_free.pop_back();
	m_current->sequence = m_recording ? m_sequence : 0;
	return true;
}

bool ThreadedRenderDevice::reservePayload(size_t bytes, size_t commands)
{
	acquire();
	if (bytes > m_options.maxPacketBytes || commands > m_options.maxPacketCommands)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		++m_metrics.rejectedPackets;
		return false;
	}
	if (bytes > m_options.maxPacketBytes - m_current->bytes.size() ||
		commands > m_options.maxPacketCommands - m_current->commands.size())
	{
		// Upload-heavy map loads can exceed one packet without exceeding the
		// bounded pool. Split only at command boundaries; the owner's logical
		// frame and failure latch survive these FIFO immutable segments.
		flush(CONTROL_NONE, std::shared_ptr<Reply>(), false, false);
		acquire();
	}
	try
	{
		const size_t dataRequired = m_current->bytes.size() + bytes;
		if (dataRequired > m_current->bytes.capacity())
			m_current->bytes.reserve((std::min)(m_options.maxPacketBytes,
				(std::max)(dataRequired, (std::max)(size_t(4096), m_current->bytes.capacity() * 2))));
		const size_t commandRequired = m_current->commands.size() + commands;
		if (commandRequired > m_current->commands.capacity())
			m_current->commands.reserve((std::min)(size_t(m_options.maxPacketCommands),
				(std::max)(commandRequired, (std::max)(size_t(64), m_current->commands.capacity() * 2))));
		return true;
	}
	catch (...) { return false; }
}

size_t ThreadedRenderDevice::copyPayload(const void *data, size_t bytes)
{
	const size_t offset = m_current->bytes.size();
	if (bytes)
	{
		const unsigned char *source = static_cast<const unsigned char *>(data);
		m_current->bytes.insert(m_current->bytes.end(), source, source + bytes);
	}
	return offset;
}

RenderResult ThreadedRenderDevice::append(Command command, const void *payload,
	size_t bytes, bool requireFrame)
{
	if (!usable() || (requireFrame && (!m_recording || m_ended)))
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	if (!reservePayload(bytes)) return fail(RENDER_RESULT_OUT_OF_MEMORY);
	command.payloadOffset = copyPayload(payload, bytes);
	m_current->commands.push_back(command);
	return RENDER_RESULT_OK;
}

RenderResult ThreadedRenderDevice::beginFrame()
{
	if (!usable() || m_recording) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_completionCount + m_reservedCompletions == COMPLETION_CAPACITY)
		{
			++m_metrics.rejectedPackets;
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		if (m_ownerExecuting) ++m_metrics.producerOverlapFrames;
		++m_reservedCompletions;
	}
	m_recording = true;
	m_ended = false;
	m_targets = RenderTargetBinding();
	m_sequence = m_nextSequence++;
	acquire();
	m_current->sequence = m_sequence;
	const RenderResult result = append(Command(OP_BEGIN));
	if (result != RENDER_RESULT_OK)
	{
		m_recording = false;
		// OP_BEGIN temporarily marks the producer as recording.  If its append
		// fails, no frame was accepted, so do not carry that abandoned attempt's
		// synchronous failure into a later beginFrame retry.
		m_producerFailure = RENDER_RESULT_OK;
		std::lock_guard<std::mutex> lock(m_mutex);
		--m_reservedCompletions;
	}
	return result;
}

RenderResult ThreadedRenderDevice::endFrame()
{
	if (!usable() || !m_recording || m_ended) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	m_ended = true;
	acquire();
	m_current->closeFrame = true;
	return m_producerFailure;
}

RenderResult ThreadedRenderDevice::flush(Control control,
	const std::shared_ptr<Reply> &reply, bool finalFrame, bool visible)
{
	acquire();
	m_current->control = control;
	m_current->reply = reply;
	m_current->finalFrame = finalFrame;
	m_current->present = visible;
	m_current->failure = m_producerFailure;
	m_current->sequence = m_recording ? m_sequence : 0;
	std::unique_lock<std::mutex> lock(m_mutex);
	m_metrics.peakPacketBytes = (std::max)(m_metrics.peakPacketBytes,
		m_current->bytes.size() + m_current->commands.size() * sizeof(Command));
	m_queue[(m_queueRead + m_queueCount) % m_queue.size()] = m_current;
	++m_queueCount;
	++m_pending;
	m_metrics.pendingPackets = static_cast<unsigned int>(m_pending);
	m_metrics.peakPendingPackets = (std::max)(m_metrics.peakPendingPackets,
		m_metrics.pendingPackets);
	if (finalFrame)
	{
		++m_metrics.submittedFrames;
		m_lastSequence = m_sequence;
		m_recording = false;
		m_ended = false;
		m_producerFailure = RENDER_RESULT_OK;
	}
	m_current = 0;
	m_changed.notify_all();
	if (reply)
	{
		wait(lock, [&reply] { return reply->done; });
		return reply->result;
	}
	return RENDER_RESULT_OK;
}

RenderResult ThreadedRenderDevice::submitFrame(bool visible)
{
	if (!usable() || !m_recording || !m_ended) return RENDER_RESULT_INVALID_ARGUMENT;
	const uint64_t sequence = m_sequence;
	flush(CONTROL_NONE, std::shared_ptr<Reply>(), true, visible);
	if (!m_options.serial) return RENDER_RESULT_OK;
	// Serial reference/cancellation has no heap-allocated reply. In particular,
	// shutdown cannot lose an accepted frame when allocation is exhausted.
	std::unique_lock<std::mutex> lock(m_mutex);
	wait(lock, [this, sequence] { return m_completedSequence >= sequence; });
	return m_completedResult;
}

RenderResult ThreadedRenderDevice::cancelFrame(RenderResult reason)
{
	if (!usable() || !m_recording || reason == RENDER_RESULT_OK)
		return RENDER_RESULT_INVALID_ARGUMENT;
	fail(reason);
	if (!m_ended) endFrame();
	return submitFrame(false);
}

RenderResult ThreadedRenderDevice::sync(Control control, const std::shared_ptr<Reply> &reply)
{
	if (!usable()) return RENDER_RESULT_INVALID_ARGUMENT;
	return flush(control, reply, false, false);
}

RenderResult ThreadedRenderDevice::drain()
{
	if (!usable()) return RENDER_RESULT_INVALID_ARGUMENT;
	try { return sync(CONTROL_FENCE, std::make_shared<Reply>()); }
	catch (...) { return fail(RENDER_RESULT_OUT_OF_MEMORY); }
}

RenderResult ThreadedRenderDevice::lifecycle(Control control, unsigned int width, unsigned int height)
{
	if (!usable() || m_recording) return RENDER_RESULT_INVALID_ARGUMENT;
	try
	{
		std::shared_ptr<Reply> reply = std::make_shared<Reply>();
		reply->width = width; reply->height = height;
		return sync(control, reply);
	}
	catch (...) { return fail(RENDER_RESULT_OUT_OF_MEMORY); }
}

void ThreadedRenderDevice::shutdown()
{
	// Destruction is a producer-owned operation. An off-producer caller is an
	// ownership violation; never detach a live thread retaining this object.
	if (std::this_thread::get_id() != m_producer || m_waiting) std::terminate();
	if (!m_thread.joinable()) return;
	if (m_initialized)
	{
		if (m_recording) cancelFrame(RENDER_RESULT_FAILED);
		else if (m_current && !m_current->commands.empty())
			flush(CONTROL_NONE, std::shared_ptr<Reply>(), false, false);
	}
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_stopping = true;
		m_changed.notify_all();
		wait(lock, [this] { return !m_started; });
	}
	m_thread.join();
	m_initialized = false;
}

bool ThreadedRenderDevice::poll(ThreadedRenderFrameCompletion *completion)
{
	if (!producer() || !completion) return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_completionCount) return false;
	*completion = m_completions[m_completionRead];
	m_completionRead = (m_completionRead + 1) % COMPLETION_CAPACITY;
	--m_completionCount;
	return true;
}

bool ThreadedRenderDevice::metrics(ThreadedRenderMetrics *metrics) const
{
	if (!metrics) return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	*metrics = m_metrics;
	return true;
}

RenderResult ThreadedRenderDevice::createBuffer(const BufferDescriptor &descriptor,
	const void *data, size_t bytes, GpuHandle *handle)
{
	if (!usable() || !handle) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	*handle = GpuHandle();
	if (!descriptor.byteCount || descriptor.byteCount > UINT_MAX || !descriptor.binding ||
		(!data && bytes) || (data && bytes != descriptor.byteCount) ||
		(!data && descriptor.usage == RENDER_USAGE_IMMUTABLE) ||
		((descriptor.binding & RENDER_BUFFER_CONSTANT) && descriptor.byteCount % 16))
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	if (bytes > m_options.maxPacketBytes || sizeof(descriptor) > m_options.maxPacketBytes - bytes ||
		!reservePayload(sizeof(descriptor) + bytes)) return fail(RENDER_RESULT_OUT_OF_MEMORY);
	GpuHandle logical = m_handles->allocate();
	if (!logical.isValid()) return fail(RENDER_RESULT_OUT_OF_MEMORY);
	Command command(OP_CREATE_BUFFER);
	command.handle = logical;
	command.payloadOffset = copyPayload(&descriptor, sizeof(descriptor));
	command.dataOffset = copyPayload(data, bytes);
	command.dataBytes = bytes;
	m_current->commands.push_back(command);
	m_producerResources[logical.index()].texture = false;
	m_producerResources[logical.index()].buffer = descriptor;
	*handle = logical;
	return RENDER_RESULT_OK;
}

RenderResult ThreadedRenderDevice::textureCommand(Operation operation, GpuHandle handle,
	const TextureDescriptor &descriptor, const TextureSubresourceData *data, unsigned int count)
{
	const unsigned int bpp = PixelBytes(descriptor.format);
	if (!usable() || !descriptor.width || !descriptor.height || !descriptor.mipCount ||
		descriptor.mipCount > 32 ||
		!descriptor.arrayCount || !bpp || !descriptor.binding ||
		descriptor.mipCount > UINT_MAX / descriptor.arrayCount ||
		(descriptor.dimension != RENDER_TEXTURE_2D && descriptor.dimension != RENDER_TEXTURE_CUBE) ||
		(descriptor.dimension == RENDER_TEXTURE_2D && descriptor.arrayCount != 1) ||
		(descriptor.dimension == RENDER_TEXTURE_CUBE && (descriptor.width != descriptor.height ||
			descriptor.arrayCount != 6 || (descriptor.binding & (RENDER_TEXTURE_RENDER_TARGET | RENDER_TEXTURE_DEPTH_STENCIL)))) ||
		(!data && count) || (data && count != descriptor.mipCount * descriptor.arrayCount) ||
		(!data && (descriptor.usage == RENDER_USAGE_IMMUTABLE || operation == OP_REFRESH_TEXTURE)))
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	size_t spanBytes = 0;
	if (!Multiply(count, sizeof(TextureSpan), &spanBytes) ||
		spanBytes > m_options.maxPacketBytes - (std::min)(sizeof(descriptor), m_options.maxPacketBytes))
		return fail(RENDER_RESULT_OUT_OF_MEMORY);
	size_t total = sizeof(descriptor) + spanBytes;
	for (unsigned int i = 0; i < count; ++i)
	{
		const unsigned int mip = i % descriptor.mipCount;
		const size_t width = (std::max)(1u, mip < 32 ? descriptor.width >> mip : 0u);
		const size_t height = (std::max)(1u, mip < 32 ? descriptor.height >> mip : 0u);
		size_t rowBytes = 0, minimum = 0;
		if (!data[i].data || !data[i].rowPitch || data[i].rowPitch > UINT_MAX ||
			data[i].slicePitch > UINT_MAX || !Multiply(width, bpp, &rowBytes) ||
			data[i].rowPitch < rowBytes || !Multiply(height, data[i].rowPitch, &minimum) ||
			(data[i].slicePitch && data[i].slicePitch < minimum))
			return fail(RENDER_RESULT_INVALID_ARGUMENT);
		const size_t bytes = (std::max)(minimum, data[i].slicePitch);
		if (total > m_options.maxPacketBytes || bytes > m_options.maxPacketBytes - total)
			return fail(RENDER_RESULT_OUT_OF_MEMORY);
		total += bytes;
	}
	if (!reservePayload(total)) return fail(RENDER_RESULT_OUT_OF_MEMORY);
	Command command(operation);
	command.handle = handle;
	command.payloadOffset = copyPayload(&descriptor, sizeof(descriptor));
	command.dataOffset = m_current->bytes.size();
	command.integers[0] = count;
	m_current->bytes.resize(m_current->bytes.size() + spanBytes);
	for (unsigned int i = 0; i < count; ++i)
	{
		const unsigned int mip = i % descriptor.mipCount;
		const size_t height = (std::max)(1u, mip < 32 ? descriptor.height >> mip : 0u);
		TextureSpan span;
		span.rowPitch = data[i].rowPitch;
		span.slicePitch = data[i].slicePitch;
		span.offset = copyPayload(data[i].data, (std::max)(span.slicePitch, span.rowPitch * height));
		std::memcpy(m_current->bytes.data() + command.dataOffset + i * sizeof(span), &span, sizeof(span));
	}
	m_current->commands.push_back(command);
	return RENDER_RESULT_OK;
}

RenderResult ThreadedRenderDevice::createTexture(const TextureDescriptor &descriptor,
	const TextureSubresourceData *data, unsigned int count, GpuHandle *handle)
{
	if (!usable() || !handle) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	*handle = GpuHandle();
	GpuHandle logical = m_handles->allocate();
	if (!logical.isValid()) return fail(RENDER_RESULT_OUT_OF_MEMORY);
	const RenderResult result = textureCommand(OP_CREATE_TEXTURE, logical, descriptor, data, count);
	if (result != RENDER_RESULT_OK) { m_handles->release(logical); return result; }
	m_producerResources[logical.index()].texture = true;
	m_producerResources[logical.index()].descriptor = descriptor;
	*handle = logical;
	return RENDER_RESULT_OK;
}

RenderResult ThreadedRenderDevice::refreshTexture(GpuHandle handle, const TextureDescriptor &descriptor,
	const TextureSubresourceData *data, unsigned int count)
{
	if (!usable() || !valid(handle, true)) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	const TextureDescriptor &existing = m_producerResources[handle.index()].descriptor;
	// This capability result must be synchronous: the bridge uses it to choose
	// recreation. It is not a failed frame, unlike a failed accepted upload.
	if (!SameTexture(existing, descriptor) || existing.usage == RENDER_USAGE_IMMUTABLE)
		return RENDER_RESULT_UNSUPPORTED;
	if ((m_targets.hasColor && !m_targets.useBackBufferColor && m_targets.color.resource == handle) ||
		(m_targets.hasDepth && !m_targets.useBackBufferDepth && m_targets.depth.resource == handle))
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	return textureCommand(OP_REFRESH_TEXTURE, handle, descriptor, data, count);
}

bool ThreadedRenderDevice::destroyResource(GpuHandle handle)
{
	if (!usable() || !m_handles->isLive(handle)) return false;
	Command command(OP_DESTROY); command.handle = handle;
	if (append(command, 0, 0, false) != RENDER_RESULT_OK) return false;
	if ((m_targets.hasColor && m_targets.color.resource == handle) ||
		(m_targets.hasDepth && m_targets.depth.resource == handle)) m_targets = RenderTargetBinding();
	return m_handles->release(handle);
}

RenderResult ThreadedRenderDevice::updateBuffer(GpuHandle handle, const void *data,
	size_t bytes, size_t offset, RenderBufferUpdateMode mode)
{
	if (!usable() || !valid(handle, false) || !data || !bytes ||
		offset > m_producerResources[handle.index()].buffer.byteCount ||
		bytes > m_producerResources[handle.index()].buffer.byteCount - offset)
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	const BufferDescriptor &descriptor = m_producerResources[handle.index()].buffer;
	if (descriptor.usage == RENDER_USAGE_IMMUTABLE) return RENDER_RESULT_UNSUPPORTED;
	if (mode != RENDER_BUFFER_UPDATE_PRESERVE && mode != RENDER_BUFFER_UPDATE_DISCARD &&
		mode != RENDER_BUFFER_UPDATE_NO_OVERWRITE) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	if (mode != RENDER_BUFFER_UPDATE_PRESERVE && (descriptor.usage != RENDER_USAGE_DYNAMIC ||
		(descriptor.binding != RENDER_BUFFER_VERTEX && descriptor.binding != RENDER_BUFFER_INDEX) ||
		(mode == RENDER_BUFFER_UPDATE_DISCARD && offset))) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	Command command(OP_UPDATE_BUFFER); command.handle = handle;
	command.dataBytes = bytes; command.destinationOffset = offset; command.integers[0] = mode;
	return append(command, data, bytes);
}

RenderResult ThreadedRenderDevice::copyActiveColorTargetToTexture(GpuHandle handle)
{
	if (!usable() || !valid(handle, true)) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	Command command(OP_COPY_COLOR); command.handle = handle;
	return append(command);
}

RenderResult ThreadedRenderDevice::clearTargets(unsigned int flags, const RenderFloat4 &color,
	float depth, unsigned int stencil)
{
	Command command(OP_CLEAR); command.integers[0] = flags; command.integers[1] = stencil;
	command.floats[0] = color.x; command.floats[1] = color.y;
	command.floats[2] = color.z; command.floats[3] = color.w; command.floats[4] = depth;
	return append(command);
}

RenderResult ThreadedRenderDevice::setRenderTargets(const RenderTargetBinding &binding)
{
	if (!usable() || (binding.hasColor && !binding.useBackBufferColor && !valid(binding.color.resource, true)) ||
		(binding.hasDepth && !binding.useBackBufferDepth && !valid(binding.depth.resource, true)) ||
		(binding.hasColor && binding.useBackBufferColor) || (binding.hasDepth && binding.useBackBufferDepth))
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	if ((binding.hasColor && (binding.color.mip || binding.color.arraySlice)) ||
		(binding.hasDepth && (binding.depth.mip || binding.depth.arraySlice))) return RENDER_RESULT_UNSUPPORTED;
	const RenderResult result = append(Command(OP_TARGETS), &binding, sizeof(binding));
	if (result == RENDER_RESULT_OK) m_targets = binding;
	return result;
}

RenderResult ThreadedRenderDevice::setRenderTargets(GpuHandle color, GpuHandle depth)
{
	RenderTargetBinding binding;
	if (!color.isValid() && !depth.isValid()) return setRenderTargets(binding);
	binding.useBackBufferColor = false; binding.useBackBufferDepth = false;
	binding.hasColor = color.isValid(); binding.hasDepth = depth.isValid();
	binding.color.resource = color; binding.depth.resource = depth;
	return setRenderTargets(binding);
}

RenderResult ThreadedRenderDevice::setViewport(float x, float y, float w, float h, float lo, float hi)
{
	Command command(OP_VIEWPORT);
	command.floats[0] = x; command.floats[1] = y; command.floats[2] = w;
	command.floats[3] = h; command.floats[4] = lo; command.floats[5] = hi;
	return append(command);
}

RenderResult ThreadedRenderDevice::setLegacyState(const LegacyLogicalState &state,
	LegacyVertexFormat format, unsigned int mask)
{
	Command command(OP_LEGACY_STATE); command.integers[0] = format; command.integers[1] = mask;
	return append(command, &state, sizeof(state));
}

RenderResult ThreadedRenderDevice::setLegacyStateForLayout(const LegacyLogicalState &state,
	const LegacyVertexLayout &layout, unsigned int mask)
{
	if (layout.elementCount > LegacyVertexLayout::MAX_ELEMENT_COUNT)
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	LayoutState payload; payload.state = state; payload.layout = layout;
	Command command(OP_LEGACY_LAYOUT); command.integers[0] = mask;
	return append(command, &payload, sizeof(payload));
}

RenderResult ThreadedRenderDevice::setVertexBuffer(GpuHandle handle, unsigned int stride, unsigned int offset)
{
	if (!usable() || !valid(handle, false, true)) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	Command command(OP_VERTEX_BUFFER); command.handle = handle;
	command.integers[0] = stride; command.integers[1] = offset; return append(command);
}
RenderResult ThreadedRenderDevice::setIndexBuffer(GpuHandle handle, RenderFormat format, unsigned int offset)
{
	if (!usable() || !valid(handle, false, true)) return fail(RENDER_RESULT_INVALID_ARGUMENT);
	Command command(OP_INDEX_BUFFER); command.handle = handle;
	command.integers[0] = format; command.integers[1] = offset; return append(command);
}
RenderResult ThreadedRenderDevice::setTexture(unsigned int stage, GpuHandle handle)
{
	if (!usable() || !valid(handle, true, true) || stage >= LEGACY_TEXTURE_STAGE_COUNT)
		return fail(RENDER_RESULT_INVALID_ARGUMENT);
	Command command(OP_TEXTURE); command.handle = handle; command.integers[0] = stage; return append(command);
}
RenderResult ThreadedRenderDevice::setPrimitiveTopology(RenderPrimitiveTopology topology)
{
	Command command(OP_TOPOLOGY); command.integers[0] = topology; return append(command);
}
RenderResult ThreadedRenderDevice::draw(unsigned int count, unsigned int first)
{
	Command command(OP_DRAW); command.integers[0] = count; command.integers[1] = first; return append(command);
}
RenderResult ThreadedRenderDevice::drawIndexed(unsigned int count, unsigned int first, int base)
{
	Command command(OP_DRAW_INDEXED); command.integers[0] = count; command.integers[1] = first;
	command.signedValue = base; return append(command);
}

RenderResult ThreadedRenderDevice::captureBackBuffer(void *destination, size_t bytes,
	size_t rowPitch, RenderFormat *format)
{
	if (!usable() || (m_recording && !m_ended) || !destination || !format)
		return RENDER_RESULT_INVALID_ARGUMENT;
	RenderBackBufferInfo info;
	RenderResult result = getBackBufferInfo(&info);
	if (result != RENDER_RESULT_OK) return result;
	size_t required = 0, rowBytes = 0;
	if (!Multiply(info.width, 4, &rowBytes) || rowPitch < rowBytes ||
		!Multiply(info.height, rowPitch, &required) || bytes < required)
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (required > m_options.maxPacketBytes) return RENDER_RESULT_OUT_OF_MEMORY;
	try
	{
		std::shared_ptr<Reply> reply = std::make_shared<Reply>();
		reply->pixels.resize(required); reply->rowPitch = rowPitch;
		result = sync(CONTROL_CAPTURE, reply);
		if (result == RENDER_RESULT_OK)
		{
			// The owner never retains or writes through a caller output pointer.
			std::memcpy(destination, reply->pixels.data(), required);
			*format = reply->format;
		}
		return result;
	}
	catch (...) { return RENDER_RESULT_OUT_OF_MEMORY; }
}

RenderResult ThreadedRenderDevice::getDebugValidationErrorCount(unsigned int *count) const
{
	if (!count || !usable()) return RENDER_RESULT_INVALID_ARGUMENT;
	try
	{
		std::shared_ptr<Reply> reply = std::make_shared<Reply>();
		const RenderResult result = const_cast<ThreadedRenderDevice *>(this)->sync(CONTROL_DEBUG_COUNT, reply);
		if (result == RENDER_RESULT_OK) *count = reply->count;
		return result;
	}
	catch (...) { return RENDER_RESULT_OUT_OF_MEMORY; }
}

GpuHandle ThreadedRenderDevice::resolve(GpuHandle handle) const
{
	if (!handle.isValid() || handle.index() >= m_ownerResources.size()) return GpuHandle();
	const OwnerResource &slot = m_ownerResources[handle.index()];
	return slot.generation == handle.generation() ? slot.backend : GpuHandle();
}

OwnerResource *ThreadedRenderDevice::ownerResource(GpuHandle handle)
{
	if (!handle.isValid() || handle.index() >= m_ownerResources.size()) return 0;
	OwnerResource &slot = m_ownerResources[handle.index()];
	return slot.generation == handle.generation() ? &slot : 0;
}

void ThreadedRenderDevice::writeTarget(GpuHandle handle, bool establishesContents)
{
	OwnerResource *slot = ownerResource(handle);
	if (!slot) return;
	slot->gpuAuthoritative = true;
	if (establishesContents) slot->contentValid = true;
	slot->writtenSequence = m_ownerSequence;
}

RenderResult ThreadedRenderDevice::executeCommand(const Packet &packet, const Command &command)
{
	const GpuHandle handle = resolve(command.handle);
	const unsigned int *u = command.integers;
	const float *f = command.floats;
	if (!m_context && command.operation != OP_BEGIN)
		return RENDER_RESULT_FAILED;
	if (command.handle.isValid() && !handle.isValid() &&
		command.operation != OP_CREATE_BUFFER && command.operation != OP_CREATE_TEXTURE &&
		command.operation != OP_DESTROY) return RENDER_RESULT_INVALID_ARGUMENT;
	if (command.handle.isValid() && (command.operation == OP_TEXTURE ||
		command.operation == OP_VERTEX_BUFFER || command.operation == OP_INDEX_BUFFER) &&
		!m_ownerResources[command.handle.index()].contentValid) return RENDER_RESULT_FAILED;
	switch (command.operation)
	{
	case OP_BEGIN:
		m_ownerFrameResult = m_outsideFailure; m_outsideFailure = RENDER_RESULT_OK;
		m_ownerFrameResult = FirstFailure(m_ownerFrameResult, packet.failure);
		if (m_ownerDeviceRemoved) m_ownerFrameResult = RENDER_RESULT_DEVICE_REMOVED;
		m_ownerOutcome = RenderFrameOutcome();
		m_ownerOutcome.recordCommandFailure(m_ownerFrameResult);
		m_ownerFrameActive = true;
		m_ownerResourceFailure = m_outsideResourceFailure; m_outsideResourceFailure = false;
		m_ownerSequence = packet.sequence;
		m_ownerColorTarget = m_ownerDepthTarget = GpuHandle();
		{
			const RenderResult result = m_ownerDeviceRemoved ? RENDER_RESULT_DEVICE_REMOVED :
				(m_context ? m_context->beginFrame() : RENDER_RESULT_FAILED);
			m_ownerFrameOpen = result == RENDER_RESULT_OK;
			return result;
		}
	case OP_CREATE_BUFFER:
	{
		OwnerResource &slot = m_ownerResources[command.handle.index()];
		if (slot.backend.isValid()) return RENDER_RESULT_FAILED;
		slot.generation = command.handle.generation(); slot.backend = GpuHandle();
		slot.texture = false; slot.contentValid = false;
		slot.recoverySourceValid = false; slot.gpuAuthoritative = false; slot.writtenSequence = 0;
		const BufferDescriptor descriptor = read<BufferDescriptor>(packet, command.payloadOffset);
		slot.byteCount = descriptor.byteCount;
		const RenderResult result = m_backend->createBuffer(descriptor,
			command.dataBytes ? packet.bytes.data() + command.dataOffset : 0,
			command.dataBytes, &slot.backend);
		slot.contentValid = result == RENDER_RESULT_OK && slot.backend.isValid();
		slot.recoverySourceValid = slot.contentValid;
		return result == RENDER_RESULT_OK && !slot.contentValid ? RENDER_RESULT_FAILED : result;
	}
	case OP_CREATE_TEXTURE: case OP_REFRESH_TEXTURE:
	{
		// Texture2D/cube descriptors admit at most 32 mips x 6 faces. No per-
		// upload owner allocation (and hence no game allocator lock) is needed.
		TextureSubresourceData data[32 * 6];
		for (unsigned int i = 0; i < u[0]; ++i)
		{
			const TextureSpan span = read<TextureSpan>(packet, command.dataOffset + i * sizeof(TextureSpan));
			data[i].data = packet.bytes.data() + span.offset;
			data[i].rowPitch = span.rowPitch; data[i].slicePitch = span.slicePitch;
		}
		const TextureDescriptor descriptor = read<TextureDescriptor>(packet, command.payloadOffset);
		OwnerResource &slot = m_ownerResources[command.handle.index()];
		if (command.operation == OP_REFRESH_TEXTURE)
		{
			slot.contentValid = false;
			const RenderResult result = m_backend->refreshTexture(handle, descriptor, data, u[0]);
			if (result == RENDER_RESULT_OK)
			{
				slot.contentValid = true; slot.gpuAuthoritative = false; slot.writtenSequence = 0;
			}
			return result;
		}
		if (slot.backend.isValid()) return RENDER_RESULT_FAILED;
		slot.generation = command.handle.generation(); slot.backend = GpuHandle();
		slot.texture = true; slot.contentValid = false;
		slot.recoverySourceValid = false; slot.gpuAuthoritative = false; slot.writtenSequence = 0;
		const RenderResult result = m_backend->createTexture(descriptor, u[0] ? data : 0, u[0], &slot.backend);
		slot.contentValid = result == RENDER_RESULT_OK && slot.backend.isValid();
		return result == RENDER_RESULT_OK && !slot.contentValid ? RENDER_RESULT_FAILED : result;
	}
	case OP_DESTROY:
		if (handle.isValid() && !m_backend->destroyResource(handle)) return RENDER_RESULT_FAILED;
		if (command.handle == m_ownerColorTarget || command.handle == m_ownerDepthTarget)
			m_ownerColorTarget = m_ownerDepthTarget = GpuHandle();
		if (m_ownerResources[command.handle.index()].generation == command.handle.generation())
		{
			OwnerResource &slot = m_ownerResources[command.handle.index()];
			slot.backend = GpuHandle(); slot.texture = false; slot.contentValid = false;
			slot.recoverySourceValid = false; slot.writtenSequence = 0;
		}
		return RENDER_RESULT_OK;
	case OP_UPDATE_BUFFER:
	{
		OwnerResource &slot = m_ownerResources[command.handle.index()];
		const bool previouslyValid = slot.contentValid;
		slot.contentValid = false;
		const RenderResult result = m_context->updateBuffer(handle, packet.bytes.data() + command.payloadOffset,
			command.dataBytes, command.destinationOffset, static_cast<RenderBufferUpdateMode>(u[0]));
		if (result == RENDER_RESULT_OK)
		{
			slot.contentValid = previouslyValid || (!command.destinationOffset && command.dataBytes == slot.byteCount);
			slot.recoverySourceValid = slot.recoverySourceValid
				|| (!command.destinationOffset && command.dataBytes == slot.byteCount);
		}
		return result;
	}
	case OP_CLEAR:
	{
		if (u[0] & RENDER_CLEAR_COLOR) writeTarget(m_ownerColorTarget, false);
		if (u[0] & (RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL)) writeTarget(m_ownerDepthTarget, false);
		const RenderResult result = m_context->clearTargets(u[0], RenderFloat4(f[0], f[1], f[2], f[3]), f[4], u[1]);
		if (u[0] & RENDER_CLEAR_COLOR) writeTarget(m_ownerColorTarget, result == RENDER_RESULT_OK);
		if (u[0] & (RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL))
			writeTarget(m_ownerDepthTarget, result == RENDER_RESULT_OK && (u[0] & RENDER_CLEAR_DEPTH));
		return result;
	}
	case OP_TARGETS:
	{
		RenderTargetBinding binding = read<RenderTargetBinding>(packet, command.payloadOffset);
		const GpuHandle color = binding.hasColor ? binding.color.resource : GpuHandle();
		const GpuHandle depth = binding.hasDepth ? binding.depth.resource : GpuHandle();
		if (binding.hasColor && !binding.useBackBufferColor)
		{
			binding.color.resource = resolve(binding.color.resource);
			if (!binding.color.resource.isValid()) return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (binding.hasDepth && !binding.useBackBufferDepth)
		{
			binding.depth.resource = resolve(binding.depth.resource);
			if (!binding.depth.resource.isValid()) return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const RenderResult result = m_context->setRenderTargets(binding);
		if (result == RENDER_RESULT_OK)
		{
			m_ownerColorTarget = color; m_ownerDepthTarget = depth;
			// The native backend discards CPU recovery shadows as soon as a
			// texture is bound for output, even if no draw follows that binding.
			if (OwnerResource *slot = ownerResource(color)) slot->gpuAuthoritative = true;
			if (OwnerResource *slot = ownerResource(depth)) slot->gpuAuthoritative = true;
		}
		return result;
	}
	case OP_VIEWPORT: return m_context->setViewport(f[0], f[1], f[2], f[3], f[4], f[5]);
	case OP_LEGACY_STATE: return m_context->setLegacyState(read<LegacyLogicalState>(packet, command.payloadOffset),
		static_cast<LegacyVertexFormat>(u[0]), u[1]);
	case OP_LEGACY_LAYOUT:
	{
		const LayoutState state = read<LayoutState>(packet, command.payloadOffset);
		return m_context->setLegacyStateForLayout(state.state, state.layout, u[0]);
	}
	case OP_VERTEX_BUFFER: return m_context->setVertexBuffer(handle, u[0], u[1]);
	case OP_INDEX_BUFFER: return m_context->setIndexBuffer(handle, static_cast<RenderFormat>(u[0]), u[1]);
	case OP_TEXTURE: return m_context->setTexture(u[0], handle);
	case OP_TOPOLOGY: return m_context->setPrimitiveTopology(static_cast<RenderPrimitiveTopology>(u[0]));
	case OP_DRAW: case OP_DRAW_INDEXED:
		writeTarget(m_ownerColorTarget, false); writeTarget(m_ownerDepthTarget, false);
		return command.operation == OP_DRAW ? m_context->draw(u[0], u[1]) :
			m_context->drawIndexed(u[0], u[1], command.signedValue);
	case OP_COPY_COLOR:
	{
		OwnerResource &slot = m_ownerResources[command.handle.index()];
		slot.contentValid = false; slot.recoverySourceValid = false;
		slot.gpuAuthoritative = true; slot.writtenSequence = m_ownerSequence;
		const OwnerResource *source = ownerResource(m_ownerColorTarget);
		if (source && !source->contentValid) return RENDER_RESULT_FAILED;
		const RenderResult result = m_backend->copyActiveColorTargetToTexture(handle);
		if (result == RENDER_RESULT_OK) slot.contentValid = true;
		return result;
	}
	}
	return RENDER_RESULT_UNSUPPORTED;
}

void ThreadedRenderDevice::publishMetadata(RenderResult result, bool refreshInfo)
{
	RenderBackBufferInfo info;
	bool operational = false;
	if (result == RENDER_RESULT_DEVICE_REMOVED) m_ownerDeviceRemoved = true;
	try { operational = m_backend && m_backend->isOperational() && !m_ownerDeviceRemoved; }
	catch (...) {}
	const RenderResult infoResult = operational && refreshInfo ?
		BackendCall([&] { return m_backend->getBackBufferInfo(&info); }) : RENDER_RESULT_FAILED;
	if (infoResult == RENDER_RESULT_DEVICE_REMOVED) { m_ownerDeviceRemoved = true; operational = false; }
	std::lock_guard<std::mutex> lock(m_mutex);
	m_operational = operational;
	if (refreshInfo || !operational)
	{
		m_infoResult = infoResult;
		if (infoResult == RENDER_RESULT_OK) m_info = info;
	}
}

void ThreadedRenderDevice::execute(Packet &packet)
{
	RenderResult packetResult = packet.failure;
	if (m_ownerFrameActive)
	{
		m_ownerFrameResult = FirstFailure(m_ownerFrameResult, packet.failure);
		m_ownerOutcome.recordCommandFailure(packet.failure);
	}
	for (const Command &command : packet.commands)
	{
		// Resource lifetime/update operations survive failed frames. State, draws,
		// and GPU copies do not: continuing them could publish corrupt pixels.
		const bool mandatory = command.operation <= OP_UPDATE_BUFFER;
		if (!mandatory && m_ownerFrameResult != RENDER_RESULT_OK)
		{
			if (command.operation == OP_COPY_COLOR)
			{
				OwnerResource *slot = ownerResource(command.handle);
				if (slot) { slot->contentValid = false; slot->writtenSequence = m_ownerSequence; }
				m_ownerResourceFailure = true;
			}
			continue;
		}
		RenderResult result = RENDER_RESULT_FAILED;
		try { result = executeCommand(packet, command); }
		catch (const std::bad_alloc &) { result = RENDER_RESULT_OUT_OF_MEMORY; }
		catch (...) { result = RENDER_RESULT_FAILED; }
		packetResult = FirstFailure(packetResult, result);
		if (m_ownerFrameActive)
		{
			m_ownerFrameResult = FirstFailure(m_ownerFrameResult, result);
			m_ownerOutcome.recordCommandFailure(result);
		}
		else m_outsideFailure = FirstFailure(m_outsideFailure, result);
		if (result != RENDER_RESULT_OK && (command.handle.isValid() || command.operation == OP_TARGETS))
		{
			if (m_ownerFrameActive) m_ownerResourceFailure = true;
			else m_outsideResourceFailure = true;
		}
	}
	if (packet.closeFrame && m_ownerFrameOpen)
	{
		const RenderResult result = BackendCall([&] { return m_context->endFrame(); });
		m_ownerFrameOpen = false;
		m_ownerOutcome.recordEndFrame(result);
		m_ownerOutcome.markFrameEnded();
		m_ownerFrameResult = FirstFailure(m_ownerFrameResult, result);
		packetResult = FirstFailure(packetResult, result);
	}
	RenderResult frameResult = m_ownerFrameActive ? m_ownerFrameResult : packetResult;
	if (packet.control != CONTROL_NONE)
	{
		RenderResult result = RENDER_RESULT_OK;
		switch (packet.control)
		{
		case CONTROL_CAPTURE:
			result = frameResult != RENDER_RESULT_OK ? frameResult : BackendCall([&] { return m_backend->captureBackBuffer(
				packet.reply->pixels.data(), packet.reply->pixels.size(), packet.reply->rowPitch, &packet.reply->format); });
			if (packet.sequence) m_ownerOutcome.recordCapture(result);
			if (result == RENDER_RESULT_DEVICE_REMOVED)
			{
				m_ownerDeviceRemoved = true;
				m_ownerFrameResult = frameResult = RENDER_RESULT_DEVICE_REMOVED;
			}
			break;
		case CONTROL_RESIZE:
			result = BackendCall([&] { return m_backend->resize(packet.reply->width, packet.reply->height); });
			if (result == RENDER_RESULT_OK && packet.reply->width && packet.reply->height)
			{
				m_ownerDeviceRemoved = false;
				// resize may have recovered the native device internally. GPU-only
				// output pixels are not a CPU recovery source and must be redrawn.
				for (OwnerResource &slot : m_ownerResources)
					if (slot.gpuAuthoritative) slot.contentValid = false;
			}
			m_context = m_backend->immediateContext();
			break;
		case CONTROL_RECOVER:
			result = BackendCall([&] { return m_backend->recoverDevice(); });
			m_context = m_backend->immediateContext();
			if (result == RENDER_RESULT_OK && !m_context) result = RENDER_RESULT_FAILED;
			if (result == RENDER_RESULT_OK)
			{
				m_ownerDeviceRemoved = false;
				for (OwnerResource &slot : m_ownerResources)
				{
					if (slot.gpuAuthoritative) slot.contentValid = false;
					else if (!slot.texture && slot.recoverySourceValid) slot.contentValid = true;
				}
				m_drainFailure = m_outsideFailure = m_ownerFrameResult = RENDER_RESULT_OK;
				frameResult = packetResult = RENDER_RESULT_OK;
			}
			break;
		case CONTROL_DEBUG_COUNT: result = BackendCall([&] { return m_backend->getDebugValidationErrorCount(&packet.reply->count); }); break;
		case CONTROL_REPORT: result = BackendCall([&] { return m_backend->reportDebugLiveObjects(); }); break;
		case CONTROL_FENCE:
			// Resource-only packets have no frame-completion record. Keep their
			// failure observable across fences until Begin inherits it or recovery
			// repairs the device; otherwise a second lifecycle fence hides removal.
			result = FirstFailure(m_drainFailure, FirstFailure(frameResult, m_outsideFailure));
			m_drainFailure = RENDER_RESULT_OK;
			break;
		default: break;
		}
		// Capture/debug failures are not failed rendering commands. Preserve the
		// established caller-owned retry policy, while publishing the sync error.
		packet.reply->result = result;
		if (packet.control == CONTROL_RESIZE || packet.control == CONTROL_RECOVER)
			frameResult = FirstFailure(frameResult, result);
	}
	bool presented = false;
	if (packet.finalFrame && packet.present && frameResult == RENDER_RESULT_OK)
	{
		frameResult = BackendCall([&] { return m_backend->present(); });
		m_ownerOutcome.recordPresentation(frameResult);
		presented = frameResult == RENDER_RESULT_OK;
		if (presented) m_ownerOutcome.markPresented();
	}
	if (packet.finalFrame) frameResult = FirstFailure(frameResult, m_ownerOutcome.result());
	if (packet.finalFrame && frameResult != RENDER_RESULT_OK)
	{
		// Later CPU frames may already be queued with these handles. Invalidate
		// the failed producer's GPU results before executing any dependent frame,
		// independently of when the game owner polls completion/cache revisions.
		for (OwnerResource &slot : m_ownerResources)
		{
			if (slot.writtenSequence == packet.sequence)
			{
				slot.contentValid = false;
				m_ownerResourceFailure = true;
			}
		}
	}
	if (packet.control != CONTROL_FENCE)
		m_drainFailure = FirstFailure(m_drainFailure, frameResult);
	publishMetadata(frameResult, packet.control == CONTROL_RESIZE || packet.control == CONTROL_RECOVER);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (packet.finalFrame)
	{
		m_ownerOutcome.markSubmitted();
		m_ownerOutcome.setOperational(m_operational);
		ThreadedRenderFrameCompletion &completion = m_completions[
			(m_completionRead + m_completionCount) % COMPLETION_CAPACITY];
		completion.sequence = packet.sequence; completion.result = frameResult;
		completion.outcome = m_ownerOutcome;
		completion.resourceFailure = m_ownerResourceFailure;
		completion.presented = presented; completion.operational = m_operational;
		++m_completionCount; --m_reservedCompletions;
		m_completedSequence = packet.sequence; m_completedResult = frameResult;
		++m_metrics.completedFrames;
		if (frameResult != RENDER_RESULT_OK) ++m_metrics.failedFrames;
		m_ownerFrameActive = false;
	}
	if (packet.reply)
	{
		if (packet.control == CONTROL_NONE) packet.reply->result = frameResult;
		packet.reply->done = true;
	}
}

void ThreadedRenderDevice::run(RenderDeviceParameters parameters)
{
	RenderResult initial = RENDER_RESULT_FAILED;
	const bool registered = rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_RENDER);
	try
	{
		m_backend = registered ? m_factory(m_factoryContext) : 0;
		initial = m_backend ? (m_backend->backend() == RENDER_BACKEND_D3D11 ?
			m_backend->initialize(parameters) : RENDER_RESULT_UNSUPPORTED) :
			(registered ? RENDER_RESULT_OUT_OF_MEMORY : RENDER_RESULT_FAILED);
		if (initial == RENDER_RESULT_OK)
		{
			m_context = m_backend->immediateContext();
			if (!m_context) initial = RENDER_RESULT_FAILED;
		}
		publishMetadata(initial, true);
	}
	catch (...) { initial = RENDER_RESULT_FAILED; }
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_initialResult = initial; m_started = true;
		if (initial != RENDER_RESULT_OK) m_operational = false;
		m_changed.notify_all();
	}
	for (;;)
	{
		Packet *packet = 0;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_changed.wait(lock, [this] { return m_queueCount || m_stopping; });
			if (!m_queueCount && m_stopping) break;
			packet = m_queue[m_queueRead];
			m_queueRead = (m_queueRead + 1) % m_queue.size(); --m_queueCount;
			m_ownerExecuting = true;
		}
		const Clock::time_point start = Clock::now();
		execute(*packet);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_metrics.ownerExecutionNanoseconds += Nanoseconds(Clock::now() - start);
		packet->reset();
		m_free.push_back(packet); --m_pending;
		m_ownerExecuting = false;
		m_metrics.pendingPackets = static_cast<unsigned int>(m_pending);
		m_changed.notify_all();
	}
	if (m_backend)
	{
		if (m_ownerFrameOpen && m_context) m_context->endFrame();
		m_backend->shutdown();
		delete m_backend;
		m_backend = 0; m_context = 0;
	}
	if (registered) rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_RENDER);
	std::lock_guard<std::mutex> lock(m_mutex);
	m_operational = false; m_started = false;
	m_infoResult = RENDER_RESULT_FAILED;
	m_changed.notify_all();
}

IRenderDevice *MakeD3D11(void *) { return CreateD3D11RenderDevice(); }
}

IRenderDevice *CreateThreadedRenderDevice(ThreadedRenderBackendFactory factory,
	void *context, const ThreadedRenderOptions &options)
{
	if (!factory || options.maxFramesInFlight < 2 || options.maxFramesInFlight > 3 ||
		!options.maxPacketBytes || !options.maxPacketCommands || !options.resourceCapacity)
		return 0;
	try { return new ThreadedRenderDevice(factory, context, options); }
	catch (...) { return 0; }
}
IRenderDevice *CreateThreadedD3D11RenderDevice(const ThreadedRenderOptions &options)
{ return CreateThreadedRenderDevice(MakeD3D11, 0, options); }
bool IsThreadedRenderDevice(const IRenderDevice *device)
{ return dynamic_cast<const ThreadedRenderDevice *>(device) != 0; }
RenderResult SubmitThreadedRenderFrame(IRenderDevice *device, bool presentFrame)
{
	ThreadedRenderDevice *threaded = dynamic_cast<ThreadedRenderDevice *>(device);
	return threaded ? threaded->submitFrame(presentFrame) : RENDER_RESULT_UNSUPPORTED;
}
uint64_t LastThreadedRenderFrameSequence(const IRenderDevice *device)
{
	const ThreadedRenderDevice *threaded = dynamic_cast<const ThreadedRenderDevice *>(device);
	return threaded ? threaded->lastSequence() : 0;
}
bool PollThreadedRenderCompletion(IRenderDevice *device, ThreadedRenderFrameCompletion *completion)
{
	ThreadedRenderDevice *threaded = dynamic_cast<ThreadedRenderDevice *>(device);
	return threaded && threaded->poll(completion);
}
RenderResult DrainThreadedRenderDevice(IRenderDevice *device)
{
	ThreadedRenderDevice *threaded = dynamic_cast<ThreadedRenderDevice *>(device);
	return threaded ? threaded->drain() : RENDER_RESULT_UNSUPPORTED;
}
bool GetThreadedRenderMetrics(const IRenderDevice *device, ThreadedRenderMetrics *metrics)
{
	const ThreadedRenderDevice *threaded = dynamic_cast<const ThreadedRenderDevice *>(device);
	return threaded && threaded->metrics(metrics);
}
RenderResult CancelThreadedRenderFrame(IRenderDevice *device, RenderResult reason)
{
	ThreadedRenderDevice *threaded = dynamic_cast<ThreadedRenderDevice *>(device);
	return threaded ? threaded->cancelFrame(reason) : RENDER_RESULT_UNSUPPORTED;
}
}
}
