#include "XAudio2AudioDevice/XAudio2AudioServiceOwner.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <objbase.h>

#if defined(RTS_XAUDIO2_HAS_JOB_OWNERSHIP)
#include "Lib/JobSystem.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <thread>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t COMPLETIONS_PER_VOICE = 32;
constexpr std::size_t MAX_CHUNK_BYTES = 48000 * 4;
constexpr std::size_t COMMANDS_PER_PASS = 64;

enum class Operation
{
	OPEN, SHUTDOWN, RELEASE, CREATE, DESTROY, RESET, SUBMIT,
	VOLUME, FREQUENCY, SPATIAL, PAUSE, RESUME, STOP, SERVICE, SERVICE_ALL, FENCE
};

struct Reply
{
	bool ready = false;
	bool result = false;
	XAudio2PcmVoiceHandle handle;
};

struct Command
{
	Operation operation = Operation::FENCE;
	XAudio2PcmVoiceHandle handle;
	std::uint64_t generation = 0;
	float value = 0;
	XAudio2SpatializationPose listener;
	XAudio2SpatializationPose emitter;
	AudioPcmChunk pcm;
	Reply *reply = nullptr; // A lifecycle fence retains its stack reply until ready.
};

struct VoiceStatus
{
	XAudio2PcmVoiceHandle handle;
	std::uint64_t requestedGeneration = 0;
	float maxFrequencyRatio = 2.0f;
	bool open = false;
	bool failed = false;
	bool forcedFailure = false;
	bool volumeSupported = true;
	bool destroyPending = false;
	bool drained = true;
	bool accepting = true;
	bool armed = false;
	HRESULT error = S_OK;
	std::int64_t sample = -1;
	std::size_t nativeBuffers = 0;
	std::size_t nativeBytes = 0;
	std::size_t queuedBuffers = 0;
	std::size_t queuedBytes = 0;
	std::size_t executingBuffers = 0;
	std::size_t executingBytes = 0;
	std::size_t pendingResets = 0;
	std::array<XAudio2AudioCompletion, COMPLETIONS_PER_VOICE> completions;
	std::size_t completionRead = 0;
	std::size_t completionCount = 0;

	std::size_t buffers() const noexcept { return nativeBuffers + queuedBuffers + executingBuffers; }
	std::size_t bytes() const noexcept { return nativeBytes + queuedBytes + executingBytes; }
};

bool isPoseFinite(const XAudio2SpatializationPose &pose) noexcept
{
	for (float value : pose.position) if (!std::isfinite(value)) return false;
	for (float value : pose.front) if (!std::isfinite(value)) return false;
	for (float value : pose.top) if (!std::isfinite(value)) return false;
	return true;
}

bool validChunk(const AudioPcmChunk &chunk) noexcept
{
	return chunk.sampleRate == 48000 && chunk.channels == 2
		&& chunk.format == AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
		&& chunk.frameCount != 0 && chunk.frameCount <= 48000 && chunk.startSample >= 0
		&& chunk.startSample <= (std::numeric_limits<std::int64_t>::max)() - chunk.frameCount
		&& chunk.data.size() == static_cast<std::size_t>(chunk.frameCount) * 4;
}

bool isCoalescible(Operation operation) noexcept
{
	return operation == Operation::VOLUME || operation == Operation::SPATIAL
		|| operation == Operation::FREQUENCY || operation == Operation::SERVICE_ALL;
}

class ProcessAudioOwner
{
public:
	static ProcessAudioOwner &instance()
	{
#if defined(RTS_XAUDIO2_HAS_JOB_OWNERSHIP)
		// Construct the registry first so it outlives this owner's exit join.
		(void)rts::JobSystem::instance();
#endif
		static ProcessAudioOwner owner;
		return owner;
	}
	~ProcessAudioOwner();
	bool attach(const std::shared_ptr<XAudio2AudioOwnerState> &state) noexcept;
	void detach(const std::shared_ptr<XAudio2AudioOwnerState> &state) noexcept;
	void wake() noexcept { m_wake.notify_one(); }

private:
	void run() noexcept;
	std::mutex m_registryMutex;
	std::condition_variable m_wake;
	std::array<std::shared_ptr<XAudio2AudioOwnerState>, XAudio2AudioServiceOwner::MAX_SERVICES> m_services;
	std::thread m_thread;
	bool m_stop = false;
};
}

// Only the service owner accesses native/pendingBackend. The short mailbox
// mutex protects fixed POD status and moved commands, never a native call.
struct XAudio2AudioOwnerState
{
	explicit XAudio2AudioOwnerState(std::unique_ptr<IXAudio2AudioEngineBackend> backend) :
		pendingBackend(std::move(backend)) {}

	std::mutex mutex;
	std::condition_variable progress;
	std::array<Command, XAudio2AudioServiceOwner::MAX_COMMANDS> commands;
	std::array<VoiceStatus, XAudio2AudioServiceOwner::MAX_VOICES> voices;
	std::size_t read = 0;
	std::size_t count = 0;
	std::size_t queuedPcmBytes = 0;
	std::size_t executingPcmBytes = 0;
	bool registered = false;
	bool admission = false;
	bool failureObserved = false;
	XAudio2AudioServiceState publishedState = XAudio2AudioServiceState::CLOSED;
	HRESULT error = S_OK;
	HRESULT startupError = S_OK;
	HRESULT commandError = S_OK;
	XAudio2AudioOwnerMetrics metrics;
	std::unique_ptr<IXAudio2AudioEngineBackend> pendingBackend;
	std::unique_ptr<XAudio2AudioService> native;

	VoiceStatus *find(XAudio2PcmVoiceHandle handle) noexcept
	{
		return handle.isValid() && handle.index < voices.size()
			&& voices[handle.index].handle == handle ? &voices[handle.index] : nullptr;
	}

	std::size_t bytes() const noexcept
	{
		// Cancelled commands may outlive their old voice record in the FIFO.
		// Account for their storage globally until the owner consumes them.
		std::size_t total = queuedPcmBytes + executingPcmBytes;
		for (const VoiceStatus &voice : voices) total += voice.nativeBytes;
		return total;
	}

	bool available(const VoiceStatus &voice, std::size_t submissions) const noexcept
	{
		return admission && !voice.failed && !voice.destroyPending && voice.open
			&& submissions <= XAudio2PcmVoice::SLOT_COUNT - (std::min)(XAudio2PcmVoice::SLOT_COUNT, voice.buffers())
			&& submissions <= COMPLETIONS_PER_VOICE - (std::min)(COMPLETIONS_PER_VOICE,
				voice.completionCount + voice.buffers());
	}

	void insert(Command &&command) noexcept
	{
		commands[(read + count) % commands.size()] = std::move(command);
		++count;
		metrics.peakQueuedCommands = (std::max)(metrics.peakQueuedCommands, count);
		ProcessAudioOwner::instance().wake();
	}

	bool post(Command &&command) noexcept
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!registered) return false;
		if (count == commands.size()) {
			const auto began = Clock::now();
			++metrics.queueWaits;
			progress.wait(lock, [this]() { return count < commands.size() || !registered; });
			metrics.queueWaitNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now() - began).count();
			if (!registered) return false;
		}
		const bool voiceOperation = command.handle.isValid();
		VoiceStatus *voice = voiceOperation ? find(command.handle) : nullptr;
		if (voiceOperation && (voice == nullptr || !voice->open || voice->destroyPending
			|| (command.operation != Operation::DESTROY && (!admission || voice->failed)))) return false;
		if (command.operation == Operation::CREATE && !admission) return false;
		if (command.operation == Operation::VOLUME && (voice == nullptr || !voice->volumeSupported)) return false;
		if (command.operation == Operation::SHUTDOWN || command.operation == Operation::RELEASE) {
			admission = false;
		}
		if (command.operation == Operation::RESET) {
			voice->requestedGeneration = command.generation;
			++voice->pendingResets;
			voice->sample = -1;
		}
		if (command.operation == Operation::DESTROY) voice->destroyPending = true;
		// Coalesce only adjacent replaceable controls. Never cross PCM, reset,
		// pause, service or lifetime barriers; the newest value retains FIFO order.
		if (count != 0 && command.reply == nullptr && isCoalescible(command.operation)) {
			Command &tail = commands[(read + count - 1) % commands.size()];
			if (tail.reply == nullptr && tail.operation == command.operation && tail.handle == command.handle) {
				tail = std::move(command);
				++metrics.coalescedControls;
				return true;
			}
		}
		insert(std::move(command));
		return true;
	}

	bool fence(Command &&command, XAudio2PcmVoiceHandle *handle = nullptr) noexcept
	{
		Reply reply;
		command.reply = &reply;
		if (!post(std::move(command))) return false;
		std::unique_lock<std::mutex> lock(mutex);
		const auto began = Clock::now();
		++metrics.fenceWaits;
		progress.wait(lock, [&reply]() { return reply.ready; });
		metrics.fenceWaitNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - began).count();
		if (handle != nullptr) *handle = reply.handle;
		return reply.result;
	}

	bool pop(Command &command) noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (count == 0) return false;
		command = std::move(commands[read]);
		commands[read] = {};
		read = (read + 1) % commands.size();
		--count;
		++metrics.commands;
		if (command.operation == Operation::SUBMIT) {
			queuedPcmBytes -= command.pcm.data.size();
			executingPcmBytes += command.pcm.data.size();
			if (VoiceStatus *voice = find(command.handle)) {
				--voice->queuedBuffers;
				voice->queuedBytes -= command.pcm.data.size();
				++voice->executingBuffers;
				voice->executingBytes += command.pcm.data.size();
				voice->armed = false;
			}
		}
		progress.notify_all();
		return true;
	}

	void publishVoice(XAudio2PcmVoiceHandle handle, bool finishSubmission = false,
		std::size_t submittedBytes = 0) noexcept
	{
		if (native == nullptr) return;
		const bool open = native->isVoiceOpen(handle);
		const bool failed = native->isVoiceFailed(handle);
		const bool drained = native->isVoiceDrained(handle);
		const bool accepting = native->canVoiceAccept(handle, 0);
		const HRESULT voiceError = native->getVoiceLastError(handle);
		std::size_t buffers = 0, bufferedBytes = 0;
		native->getVoiceBufferedState(handle, buffers, bufferedBytes);
		std::int64_t sample = -1;
		native->getVoicePlayedSample(handle, sample);
		std::lock_guard<std::mutex> lock(mutex);
		if (finishSubmission) executingPcmBytes -= submittedBytes;
		if (VoiceStatus *voice = find(handle)) {
			voice->open = open;
			voice->failed = failed || voice->forcedFailure;
			voice->drained = drained;
			voice->accepting = accepting;
			if (!voice->forcedFailure) voice->error = voiceError;
			voice->nativeBuffers = buffers;
			voice->nativeBytes = bufferedBytes;
			voice->sample = voice->pendingResets == 0 ? sample : -1;
			if (finishSubmission) {
				--voice->executingBuffers;
				voice->executingBytes -= submittedBytes;
			}
		}
	}

	void publishState() noexcept
	{
		if (native == nullptr) return;
		const auto state = native->state();
		const HRESULT lastError = native->getLastError();
		std::lock_guard<std::mutex> lock(mutex);
		publishedState = state;
		error = FAILED(commandError) && SUCCEEDED(lastError) ? commandError : lastError;
		if (state != XAudio2AudioServiceState::RUNNING) admission = false;
	}

	void failVoice(XAudio2PcmVoiceHandle handle) noexcept
	{
		const HRESULT voiceError = native->getVoiceLastError(handle);
		// Accepted work that subsequently fails is terminal and observable; it
		// is never relabeled DROPPED after its producer has advanced the stream.
		native->stopVoice(handle);
		std::lock_guard<std::mutex> lock(mutex);
		if (VoiceStatus *voice = find(handle)) {
			voice->forcedFailure = voice->failed = true;
			voice->error = FAILED(voiceError) ? voiceError : E_FAIL;
			voice->armed = false;
		}
	}

	void pump() noexcept
	{
		if (native == nullptr) return;
		native->processPendingFailure();
		for (std::size_t index = 0; index < voices.size(); ++index) {
			XAudio2PcmVoiceHandle handle;
			bool armed = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				handle = voices[index].handle;
				armed = voices[index].armed && !voices[index].forcedFailure;
			}
			if (!handle.isValid()) continue;
			if (armed) native->serviceVoice(handle);
			// The source callback FIFO has exactly one consumer: this owner.
			// Each logical service AND each voice gets a separate bounded mailbox.
			for (;;) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (voices[index].completionCount == COMPLETIONS_PER_VOICE) break;
				}
				XAudio2AudioCompletion completion;
				if (!native->tryPopCompletion(handle, completion)) break;
				std::lock_guard<std::mutex> lock(mutex);
				VoiceStatus &voice = voices[index];
				voice.completions[(voice.completionRead + voice.completionCount)
					% COMPLETIONS_PER_VOICE] = completion;
				++voice.completionCount;
			}
			publishVoice(handle);
		}
		publishState();
		std::lock_guard<std::mutex> lock(mutex);
		++metrics.servicePasses;
	}

	void execute(Command &command) noexcept
	{
		bool result = false;
		XAudio2PcmVoiceHandle created;
		const std::size_t submittedBytes = command.pcm.data.size();
		switch (command.operation) {
		case Operation::OPEN:
			{
				std::lock_guard<std::mutex> lock(mutex);
				publishedState = XAudio2AudioServiceState::OPENING;
				commandError = S_OK;
			}
			try {
				if (FAILED(startupError)) {
					std::lock_guard<std::mutex> lock(mutex);
					error = startupError;
					publishedState = XAudio2AudioServiceState::CLOSED;
					break;
				}
				if (native == nullptr) native = std::make_unique<XAudio2AudioService>(
					std::move(pendingBackend), XAudio2AudioExecutionMode::SERIAL_REFERENCE);
				result = native->open();
			} catch (...) {
				std::lock_guard<std::mutex> lock(mutex);
				error = E_OUTOFMEMORY;
				commandError = E_OUTOFMEMORY;
				publishedState = XAudio2AudioServiceState::CLOSED;
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				admission = result;
				failureObserved = false;
			}
			break;
		case Operation::SHUTDOWN:
		case Operation::RELEASE:
			if (native != nullptr) native->shutdown();
			publishState();
			{
				std::lock_guard<std::mutex> lock(mutex);
				for (VoiceStatus &voice : voices) voice = {};
				publishedState = XAudio2AudioServiceState::CLOSED;
				admission = false;
			}
			if (command.operation == Operation::RELEASE) {
				native.reset();
				pendingBackend.reset();
			}
			result = true;
			break;
		case Operation::CREATE:
			{
				bool capacity = false;
				{
					std::lock_guard<std::mutex> lock(mutex);
					for (const VoiceStatus &voice : voices) capacity |= !voice.handle.isValid();
				}
				if (native != nullptr && capacity) created = native->createVoice(command.value);
				if (!capacity) commandError = E_OUTOFMEMORY;
				if (created.isValid()) {
					std::lock_guard<std::mutex> lock(mutex);
					VoiceStatus &voice = voices[created.index];
					voice = {};
					voice.handle = created;
					voice.maxFrequencyRatio = command.value;
				}
				result = created.isValid();
				if (created.isValid()) {
					// Volume is an optional backend control. Probe exactly once while
					// the CREATE fence owns the native voice so later gain updates can
					// be admitted without a synchronous native capability call.
					const bool volumeResult = native->setVoiceVolume(created, 1.0f);
					const bool volumeSupported = volumeResult || native->isVoiceFailed(created);
					std::lock_guard<std::mutex> lock(mutex);
					if (VoiceStatus *voice = find(created)) voice->volumeSupported = volumeSupported;
				}
				publishVoice(created);
			}
			break;
		case Operation::DESTROY:
			result = native->destroyVoice(command.handle);
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (VoiceStatus *voice = find(command.handle)) *voice = {};
			}
			break;
		case Operation::RESET:
			result = native->resetVoice(command.handle, command.generation);
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (VoiceStatus *voice = find(command.handle)) --voice->pendingResets;
			}
			break;
		case Operation::SUBMIT:
			result = native->submit(command.handle, std::move(command.pcm)) == AudioPcmSubmitResult::ACCEPTED;
			break;
		case Operation::VOLUME: result = native->setVoiceVolume(command.handle, command.value); break;
		case Operation::FREQUENCY: result = native->setVoiceFrequencyRatio(command.handle, command.value); break;
		case Operation::SPATIAL: result = native->setVoiceSpatialization(command.handle, command.listener, command.emitter); break;
		case Operation::PAUSE: result = native->pauseVoice(command.handle); break;
		case Operation::RESUME: result = native->resumeVoice(command.handle); break;
		case Operation::STOP: result = native->stopVoice(command.handle); break;
		case Operation::SERVICE:
		case Operation::SERVICE_ALL:
			{
				std::lock_guard<std::mutex> lock(mutex);
				for (VoiceStatus &voice : voices) {
					if (command.operation == Operation::SERVICE_ALL || voice.handle == command.handle)
						voice.armed = true;
				}
				result = true;
			}
			break;
		case Operation::FENCE:
			pump();
			result = native != nullptr && native->isOpen();
			break;
		}
		if (command.handle.isValid() && command.operation != Operation::DESTROY) {
			// XAudio2PcmVoice reports E_NOTIMPL as a healthy optional-control
			// miss. CREATE caches that result and rejects future VOLUME commands,
			// but keep this guard for any already-admitted command crossing the
			// capability publication boundary.
			const bool optionalVolumeMiss = command.operation == Operation::VOLUME
				&& !result && !native->isVoiceFailed(command.handle);
			if (!result && !optionalVolumeMiss) failVoice(command.handle);
			publishVoice(command.handle, command.operation == Operation::SUBMIT, submittedBytes);
		}
		publishState();
		if (command.reply != nullptr) {
			std::lock_guard<std::mutex> lock(mutex);
			command.reply->result = result;
			command.reply->handle = created;
			command.reply->ready = true;
			progress.notify_all();
		}
	}
};

namespace
{
ProcessAudioOwner::~ProcessAudioOwner()
{
	{
		std::lock_guard<std::mutex> lock(m_registryMutex);
		m_stop = true;
	}
	m_wake.notify_one();
	if (m_thread.joinable()) m_thread.join();
}

bool ProcessAudioOwner::attach(const std::shared_ptr<XAudio2AudioOwnerState> &state) noexcept
{
	std::lock_guard<std::mutex> lock(m_registryMutex);
	for (auto &entry : m_services) {
		if (entry != nullptr) continue;
		try {
			if (!m_thread.joinable()) m_thread = std::thread([this]() { run(); });
		} catch (...) { return false; }
		entry = state;
		m_wake.notify_one();
		return true;
	}
	return false;
}

void ProcessAudioOwner::detach(const std::shared_ptr<XAudio2AudioOwnerState> &state) noexcept
{
	std::lock_guard<std::mutex> lock(m_registryMutex);
	for (auto &entry : m_services) if (entry == state) entry.reset();
}

void ProcessAudioOwner::run() noexcept
{
	const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	HRESULT startupError = FAILED(apartmentResult) ? apartmentResult : S_OK;
#if defined(RTS_XAUDIO2_HAS_JOB_OWNERSHIP)
	const bool registeredOwner = rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_AUDIO);
	if (!registeredOwner && SUCCEEDED(startupError)) startupError = E_UNEXPECTED;
#endif
	for (;;) {
		std::array<std::shared_ptr<XAudio2AudioOwnerState>, XAudio2AudioServiceOwner::MAX_SERVICES> services;
		{
			std::unique_lock<std::mutex> lock(m_registryMutex);
			if (m_stop) break;
			services = m_services;
		}
		bool pending = false;
		for (const auto &service : services) {
			if (service == nullptr) continue;
			service->startupError = startupError;
			Command command;
			for (std::size_t index = 0; index < COMMANDS_PER_PASS && service->pop(command); ++index) {
				service->execute(command);
				command = {};
			}
			service->pump();
			std::lock_guard<std::mutex> lock(service->mutex);
			pending |= service->count != 0;
		}
		if (!pending) {
			std::unique_lock<std::mutex> lock(m_registryMutex);
			if (!m_stop) m_wake.wait_for(lock, std::chrono::milliseconds(2));
		}
	}
#if defined(RTS_XAUDIO2_HAS_JOB_OWNERSHIP)
	if (registeredOwner) rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_AUDIO);
#endif
	if (SUCCEEDED(apartmentResult)) CoUninitialize();
}

Command operation(Operation operation, XAudio2PcmVoiceHandle handle = {}, float value = 0) noexcept
{
	Command command;
	command.operation = operation;
	command.handle = handle;
	command.value = value;
	return command;
}

bool popCompletion(VoiceStatus &voice, XAudio2AudioCompletion &completion) noexcept
{
	if (voice.completionCount == 0) return false;
	completion = voice.completions[voice.completionRead];
	voice.completionRead = (voice.completionRead + 1) % COMPLETIONS_PER_VOICE;
	--voice.completionCount;
	return true;
}
}

XAudio2AudioServiceOwner::XAudio2AudioServiceOwner(std::unique_ptr<IXAudio2AudioEngineBackend> backend) :
	m_state(std::make_shared<XAudio2AudioOwnerState>(std::move(backend)))
{
	const bool registered = ProcessAudioOwner::instance().attach(m_state);
	std::lock_guard<std::mutex> lock(m_state->mutex);
	m_state->registered = registered;
	// A failed registry admission never joined the shared owner. Keep the
	// metric truthful so callers can distinguish that terminal state from a
	// live process-wide native owner without implying a serial fallback.
	m_state->metrics.sharedOwner = registered;
	if (!registered) m_state->error = E_OUTOFMEMORY;
}

XAudio2AudioServiceOwner::~XAudio2AudioServiceOwner()
{
	m_state->fence(operation(Operation::RELEASE));
	ProcessAudioOwner::instance().detach(m_state);
	std::lock_guard<std::mutex> lock(m_state->mutex);
	m_state->registered = false;
}

bool XAudio2AudioServiceOwner::open() noexcept { return m_state->fence(operation(Operation::OPEN)); }
void XAudio2AudioServiceOwner::shutdown() noexcept { m_state->fence(operation(Operation::SHUTDOWN)); }
bool XAudio2AudioServiceOwner::synchronize() noexcept { return m_state->fence(operation(Operation::FENCE)); }

XAudio2AudioServiceState XAudio2AudioServiceOwner::state() const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (m_state->publishedState == XAudio2AudioServiceState::RUNNING && !m_state->admission)
		return XAudio2AudioServiceState::QUIESCING;
	return m_state->publishedState;
}

HRESULT XAudio2AudioServiceOwner::getLastError() const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->error;
}

XAudio2AudioOwnerMetrics XAudio2AudioServiceOwner::metrics() const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->metrics;
}

bool XAudio2AudioServiceOwner::processPendingFailure() noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (m_state->publishedState != XAudio2AudioServiceState::FAILED || m_state->failureObserved) return false;
	m_state->failureObserved = true;
	return true;
}

XAudio2PcmVoiceHandle XAudio2AudioServiceOwner::createVoice(float maxFrequencyRatio) noexcept
{
	XAudio2PcmVoiceHandle handle;
	if (!std::isfinite(maxFrequencyRatio) || maxFrequencyRatio < 1 || maxFrequencyRatio > XAUDIO2_MAX_FREQ_RATIO)
		return handle;
	m_state->fence(operation(Operation::CREATE, {}, maxFrequencyRatio), &handle);
	return handle;
}

bool XAudio2AudioServiceOwner::destroyVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	return handle.isValid() && m_state->fence(operation(Operation::DESTROY, handle));
}

AudioPcmSubmitResult XAudio2AudioServiceOwner::submit(XAudio2PcmVoiceHandle handle, AudioPcmChunk &&chunk) noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	VoiceStatus *voice = m_state->find(handle);
	const bool failed = voice != nullptr && (voice->failed
		|| m_state->publishedState == XAudio2AudioServiceState::FAILED);
	if (failed || voice == nullptr || voice->destroyPending || !validChunk(chunk)
		|| chunk.generation != voice->requestedGeneration
		|| !m_state->available(*voice, 1) || m_state->count == m_state->commands.size()
		|| chunk.data.size() > MAX_PCM_BYTES - (std::min)(MAX_PCM_BYTES, m_state->bytes())) {
		chunk = {};
		++m_state->metrics.rejectedSubmissions;
		return failed ? AudioPcmSubmitResult::FAILED : AudioPcmSubmitResult::DROPPED;
	}
	Command command = operation(Operation::SUBMIT, handle);
	command.pcm = std::move(chunk);
	++voice->queuedBuffers;
	voice->queuedBytes += command.pcm.data.size();
	m_state->queuedPcmBytes += command.pcm.data.size();
	m_state->metrics.peakBufferedBytes = (std::max)(m_state->metrics.peakBufferedBytes, m_state->bytes());
	m_state->insert(std::move(command));
	return AudioPcmSubmitResult::ACCEPTED;
}

AudioPcmSubmitResult XAudio2AudioServiceOwner::submitRetained(
	XAudio2PcmVoiceHandle handle, AudioPcmChunk &chunk) noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	VoiceStatus *voice = m_state->find(handle);
	const bool failed = voice != nullptr && (voice->failed
		|| m_state->publishedState == XAudio2AudioServiceState::FAILED);
	if (failed || voice == nullptr || !m_state->admission || !voice->open
		|| voice->destroyPending || !validChunk(chunk)
		|| chunk.generation != voice->requestedGeneration) {
		// A retry is only valid for bounded admission pressure. Stale handles,
		// generations, malformed chunks and closed/failed owners are terminal.
		chunk = {};
		++m_state->metrics.rejectedSubmissions;
		return AudioPcmSubmitResult::FAILED;
	}
	if (!m_state->available(*voice, 1) || m_state->count == m_state->commands.size()
		|| chunk.data.size() > MAX_PCM_BYTES - (std::min)(MAX_PCM_BYTES, m_state->bytes())) {
		// Keep the producer-owned chunk and its sequence metadata intact. The
		// manager will retry on a later owner pass after queue/slot progress.
		++m_state->metrics.rejectedSubmissions;
		return AudioPcmSubmitResult::DROPPED;
	}
	Command command = operation(Operation::SUBMIT, handle);
	command.pcm = std::move(chunk);
	++voice->queuedBuffers;
	voice->queuedBytes += command.pcm.data.size();
	m_state->queuedPcmBytes += command.pcm.data.size();
	m_state->metrics.peakBufferedBytes = (std::max)(m_state->metrics.peakBufferedBytes, m_state->bytes());
	m_state->insert(std::move(command));
	return AudioPcmSubmitResult::ACCEPTED;
}

bool XAudio2AudioServiceOwner::canVoiceAccept(XAudio2PcmVoiceHandle handle, std::size_t submissions) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	return voice != nullptr && !voice->destroyPending && voice->pendingResets == 0 && voice->accepting
		&& m_state->available(*voice, submissions)
		&& submissions <= m_state->commands.size() - m_state->count
		&& submissions <= (MAX_PCM_BYTES - (std::min)(MAX_PCM_BYTES, m_state->bytes())) / MAX_CHUNK_BYTES;
}

bool XAudio2AudioServiceOwner::resetVoice(XAudio2PcmVoiceHandle handle, std::uint64_t generation) noexcept
{
	Command command = operation(Operation::RESET, handle);
	command.generation = generation;
	return handle.isValid() && m_state->post(std::move(command));
}

bool XAudio2AudioServiceOwner::serviceVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	return handle.isValid() && m_state->post(operation(Operation::SERVICE, handle));
}

void XAudio2AudioServiceOwner::serviceVoices() noexcept { m_state->post(operation(Operation::SERVICE_ALL)); }

bool XAudio2AudioServiceOwner::setVoiceVolume(XAudio2PcmVoiceHandle handle, float volume) noexcept
{
	if (!handle.isValid() || !std::isfinite(volume) || volume < 0 || volume > XAUDIO2_MAX_VOLUME_LEVEL)
		return false;
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		const VoiceStatus *voice = m_state->find(handle);
		if (voice == nullptr || !voice->volumeSupported) return false;
	}
	return m_state->post(operation(Operation::VOLUME, handle, volume));
}

bool XAudio2AudioServiceOwner::setVoiceFrequencyRatio(XAudio2PcmVoiceHandle handle, float ratio) noexcept
{
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		const VoiceStatus *voice = m_state->find(handle);
		if (voice == nullptr || !std::isfinite(ratio) || ratio < XAUDIO2_MIN_FREQ_RATIO
			|| ratio > voice->maxFrequencyRatio) return false;
	}
	return m_state->post(operation(Operation::FREQUENCY, handle, ratio));
}

bool XAudio2AudioServiceOwner::setVoiceSpatialization(XAudio2PcmVoiceHandle handle,
	const XAudio2SpatializationPose &listener, const XAudio2SpatializationPose &emitter) noexcept
{
	if (!handle.isValid() || !isPoseFinite(listener) || !isPoseFinite(emitter)) return false;
	Command command = operation(Operation::SPATIAL, handle);
	command.listener = listener;
	command.emitter = emitter;
	return m_state->post(std::move(command));
}

bool XAudio2AudioServiceOwner::pauseVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	return handle.isValid() && m_state->post(operation(Operation::PAUSE, handle));
}
bool XAudio2AudioServiceOwner::resumeVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	return handle.isValid() && m_state->post(operation(Operation::RESUME, handle));
}
bool XAudio2AudioServiceOwner::stopVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	return handle.isValid() && m_state->post(operation(Operation::STOP, handle));
}

bool XAudio2AudioServiceOwner::isVoiceOpen(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	return voice != nullptr && voice->open;
}
bool XAudio2AudioServiceOwner::isVoiceFailed(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	return voice != nullptr && (voice->failed || m_state->publishedState == XAudio2AudioServiceState::FAILED);
}
bool XAudio2AudioServiceOwner::isVoiceDrained(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	return voice != nullptr && !voice->failed && voice->drained
		&& voice->buffers() == 0 && voice->pendingResets == 0;
}
HRESULT XAudio2AudioServiceOwner::getVoiceLastError(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	return voice != nullptr ? voice->error : E_HANDLE;
}
bool XAudio2AudioServiceOwner::getVoicePlayedSample(XAudio2PcmVoiceHandle handle, std::int64_t &sample) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	if (voice == nullptr || voice->pendingResets != 0 || voice->sample < 0) return false;
	sample = voice->sample;
	return true;
}
bool XAudio2AudioServiceOwner::getVoiceBufferedState(XAudio2PcmVoiceHandle handle,
	std::size_t &buffers, std::size_t &bytes) const noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	const VoiceStatus *voice = m_state->find(handle);
	buffers = voice != nullptr ? voice->buffers() : 0;
	bytes = voice != nullptr ? voice->bytes() : 0;
	return voice != nullptr;
}

bool XAudio2AudioServiceOwner::tryPopCompletion(XAudio2AudioCompletion &completion) noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	for (VoiceStatus &voice : m_state->voices) if (popCompletion(voice, completion)) return true;
	return false;
}
bool XAudio2AudioServiceOwner::tryPopCompletion(XAudio2PcmVoiceHandle handle,
	XAudio2AudioCompletion &completion) noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	VoiceStatus *voice = m_state->find(handle);
	return voice != nullptr && popCompletion(*voice, completion);
}
void XAudio2AudioServiceOwner::discardCompletions() noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	for (VoiceStatus &voice : m_state->voices) voice.completionRead = voice.completionCount = 0;
}
void XAudio2AudioServiceOwner::discardCompletions(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (VoiceStatus *voice = m_state->find(handle)) voice->completionRead = voice->completionCount = 0;
}
