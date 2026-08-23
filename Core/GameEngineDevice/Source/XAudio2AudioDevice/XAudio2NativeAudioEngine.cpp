#include "XAudio2AudioDevice/XAudio2NativeAudioEngine.h"

#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <xaudio2.h>

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <new>

namespace
{
class NativeEngineCallback final : public IXAudio2EngineCallback
{
public:
	using CriticalErrorCallback = IXAudio2AudioEngineBackend::CriticalErrorCallback;

	void setSink(CriticalErrorCallback callback, void *context) noexcept
	{
		m_context.store(context, std::memory_order_release);
		m_sink.store(callback, std::memory_order_release);
		m_enabled.store(true, std::memory_order_release);
	}

	void disableAndWait() noexcept
	{
		m_enabled.store(false, std::memory_order_release);
		m_sink.store(nullptr, std::memory_order_release);
		m_context.store(nullptr, std::memory_order_release);
		std::unique_lock<std::mutex> lock(m_waitMutex);
		m_waitCv.wait(lock, [this]() {
			return m_inFlight.load(std::memory_order_acquire) == 0;
		});
	}

	void STDMETHODCALLTYPE OnProcessingPassStart() override
	{
		if (begin()) {
			end();
		}
	}

	void STDMETHODCALLTYPE OnProcessingPassEnd() override
	{
		if (begin()) {
			end();
		}
	}

	void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override
	{
		if (!begin()) {
			return;
		}
		CriticalErrorCallback sink = m_sink.load(std::memory_order_acquire);
		void *context = m_context.load(std::memory_order_acquire);
		if (sink != nullptr) {
			sink(context, error);
		}
		end();
	}

private:
	bool begin() noexcept
	{
		if (!m_enabled.load(std::memory_order_acquire)) {
			return false;
		}
		m_inFlight.fetch_add(1, std::memory_order_acq_rel);
		if (!m_enabled.load(std::memory_order_acquire)) {
			end();
			return false;
		}
		return true;
	}

	void end() noexcept
	{
		if (m_inFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			m_waitCv.notify_all();
		}
	}

	std::atomic<bool> m_enabled { false };
	std::atomic<std::uint32_t> m_inFlight { 0 };
	std::atomic<CriticalErrorCallback> m_sink { nullptr };
	std::atomic<void *> m_context { nullptr };
	std::mutex m_waitMutex;
	std::condition_variable m_waitCv;
};

class NativeSourceVoiceCallback final : public IXAudio2VoiceCallback
{
public:
	void setSink(IXAudio2VoiceCallback *sink) noexcept
	{
		m_sink.store(sink, std::memory_order_release);
		m_enabled.store(true, std::memory_order_release);
	}

	void disableAndWait() noexcept
	{
		m_enabled.store(false, std::memory_order_release);
		std::unique_lock<std::mutex> lock(m_waitMutex);
		m_waitCv.wait(lock, [this]() {
			return m_inFlight.load(std::memory_order_acquire) == 0;
		});
		m_sink.store(nullptr, std::memory_order_release);
	}

	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnVoiceProcessingPassStart(bytesRequired);
			end();
		}
	}

	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnVoiceProcessingPassEnd();
			end();
		}
	}

	void STDMETHODCALLTYPE OnStreamEnd() override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnStreamEnd();
			end();
		}
	}

	void STDMETHODCALLTYPE OnBufferStart(void *context) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnBufferStart(context);
			end();
		}
	}

	void STDMETHODCALLTYPE OnBufferEnd(void *context) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnBufferEnd(context);
			end();
		}
	}

	void STDMETHODCALLTYPE OnLoopEnd(void *context) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnLoopEnd(context);
			end();
		}
	}

	void STDMETHODCALLTYPE OnVoiceError(void *context, HRESULT error) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		if (begin(sink)) {
			sink->OnVoiceError(context, error);
			end();
		}
	}

private:
	bool begin(IXAudio2VoiceCallback *&sink) noexcept
	{
		if (!m_enabled.load(std::memory_order_acquire)) {
			return false;
		}
		m_inFlight.fetch_add(1, std::memory_order_acq_rel);
		if (!m_enabled.load(std::memory_order_acquire)) {
			end();
			return false;
		}
		sink = m_sink.load(std::memory_order_acquire);
		if (sink == nullptr) {
			end();
			return false;
		}
		return true;
	}

	void end() noexcept
	{
		if (m_inFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			m_waitCv.notify_all();
		}
	}

	std::atomic<bool> m_enabled { false };
	std::atomic<std::uint32_t> m_inFlight { 0 };
	std::atomic<IXAudio2VoiceCallback *> m_sink { nullptr };
	std::mutex m_waitMutex;
	std::condition_variable m_waitCv;
};

class NativePcmVoiceBackend final : public IXAudio2PcmVoiceBackend
{
public:
	explicit NativePcmVoiceBackend(IXAudio2 *engine) :
		m_engine(engine)
	{
	}

	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback) noexcept override
	{
		if (m_engine == nullptr || m_voice != nullptr) {
			return E_HANDLE;
		}
		m_callback.setSink(callback);
		HRESULT result = m_engine->CreateSourceVoice(&m_voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
			&m_callback, nullptr, nullptr);
		if (FAILED(result)) {
			m_callback.disableAndWait();
			m_voice = nullptr;
		}
		return result;
	}

	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		return m_voice != nullptr ? m_voice->SubmitSourceBuffer(&buffer, nullptr) : E_HANDLE;
	}

	HRESULT start() noexcept override
	{
		return m_voice != nullptr ? m_voice->Start(0, 0) : E_HANDLE;
	}

	HRESULT stop() noexcept override
	{
		return m_voice != nullptr ? m_voice->Stop(0, 0) : E_HANDLE;
	}

	HRESULT flush() noexcept override
	{
		return m_voice != nullptr ? m_voice->FlushSourceBuffers() : E_HANDLE;
	}

	HRESULT getCriticalError() const noexcept override
	{
		return m_criticalError.load(std::memory_order_acquire);
	}

	void destroy() noexcept override
	{
		IXAudio2SourceVoice *voice = m_voice;
		if (voice == nullptr) {
			m_callback.disableAndWait();
			return;
		}

		// Stop and flush before DestroyVoice.  Delivery is disabled first so a
		// callback cannot enter the project voice while its owner is tearing down.
		m_callback.disableAndWait();
		voice->Stop(0, 0);
		voice->FlushSourceBuffers();
		voice->DestroyVoice();
		m_voice = nullptr;
		m_callback.disableAndWait();
	}

private:
	IXAudio2 *m_engine;
	IXAudio2SourceVoice *m_voice = nullptr;
	NativeSourceVoiceCallback m_callback;
	std::atomic<HRESULT> m_criticalError { S_OK };
};
}

class XAudio2NativeAudioEngine::Impl
{
public:
	~Impl()
	{
		close();
	}

	HRESULT open(CriticalErrorCallback callback, void *context) noexcept
	{
		if (m_engine != nullptr) {
			return S_OK;
		}

		m_callback.setSink(callback, context);
		HRESULT result = XAudio2Create(&m_engine, 0, XAUDIO2_USE_DEFAULT_PROCESSOR);
		if (FAILED(result)) {
			m_callback.disableAndWait();
			return result;
		}

		result = m_engine->RegisterForCallbacks(&m_callback);
		if (FAILED(result)) {
			close();
			return result;
		}
		m_callbacksRegistered = true;

		result = m_engine->CreateMasteringVoice(&m_masteringVoice);
		if (FAILED(result)) {
			close();
			return result;
		}
		m_stopIssued = false;
		return S_OK;
	}

	HRESULT start() noexcept
	{
		if (m_engine == nullptr || m_masteringVoice == nullptr) {
			return E_HANDLE;
		}
		m_stopIssued = false;
		return m_engine->StartEngine();
	}

	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept
	{
		if (m_engine == nullptr || m_masteringVoice == nullptr) {
			return E_HANDLE;
		}
		NativePcmVoiceBackend *nativeVoice = new (std::nothrow) NativePcmVoiceBackend(m_engine);
		if (nativeVoice == nullptr) {
			return E_OUTOFMEMORY;
		}
		voice.reset(nativeVoice);
		return S_OK;
	}

	HRESULT stop() noexcept
	{
		if (m_engine == nullptr || m_stopIssued) {
			return S_OK;
		}
		m_engine->StopEngine();
		m_stopIssued = true;
		return S_OK;
	}

	HRESULT close() noexcept
	{
		if (m_engine == nullptr) {
			m_callback.disableAndWait();
			return S_OK;
		}

		// Disable and drain before unregistering.  This guarantees the service
		// sink cannot be reached after native close returns.
		m_callback.disableAndWait();
		if (m_callbacksRegistered) {
			m_engine->UnregisterForCallbacks(&m_callback);
			m_callbacksRegistered = false;
		}
		if (m_masteringVoice != nullptr) {
			m_masteringVoice->DestroyVoice();
			m_masteringVoice = nullptr;
		}
		m_engine->Release();
		m_engine = nullptr;
		m_stopIssued = true;
		return S_OK;
	}

private:
	IXAudio2 *m_engine = nullptr;
	IXAudio2MasteringVoice *m_masteringVoice = nullptr;
	NativeEngineCallback m_callback;
	bool m_callbacksRegistered = false;
	bool m_stopIssued = true;
};

XAudio2NativeAudioEngine::XAudio2NativeAudioEngine() :
	m_impl(std::make_unique<Impl>())
{
}

XAudio2NativeAudioEngine::~XAudio2NativeAudioEngine() = default;

HRESULT XAudio2NativeAudioEngine::open(CriticalErrorCallback callback, void *context) noexcept
{
	return m_impl->open(callback, context);
}

HRESULT XAudio2NativeAudioEngine::start() noexcept
{
	return m_impl->start();
}

HRESULT XAudio2NativeAudioEngine::createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept
{
	return m_impl->createPcmVoice(voice);
}

HRESULT XAudio2NativeAudioEngine::stop() noexcept
{
	return m_impl->stop();
}

HRESULT XAudio2NativeAudioEngine::close() noexcept
{
	return m_impl->close();
}
