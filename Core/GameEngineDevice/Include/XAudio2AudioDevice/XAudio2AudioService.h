#pragma once

#include <atomic>

#include <xaudio2.h>

class XAudio2AudioService : private IXAudio2EngineCallback
{
public:
	XAudio2AudioService();
	~XAudio2AudioService();

	XAudio2AudioService(const XAudio2AudioService &) = delete;
	XAudio2AudioService &operator=(const XAudio2AudioService &) = delete;

	bool open();
	void shutdown();
	bool isOpen() const;
	HRESULT getLastError() const;

private:
	void STDMETHODCALLTYPE OnProcessingPassStart() override;
	void STDMETHODCALLTYPE OnProcessingPassEnd() override;
	void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override;

	IXAudio2 *m_engine;
	IXAudio2MasteringVoice *m_masteringVoice;
	bool m_callbacksRegistered;
	std::atomic<HRESULT> m_lastError;
};
