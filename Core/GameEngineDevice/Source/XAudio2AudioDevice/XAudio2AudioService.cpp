#include "XAudio2AudioDevice/XAudio2AudioService.h"

XAudio2AudioService::XAudio2AudioService() :
	m_engine(nullptr),
	m_masteringVoice(nullptr),
	m_lastError(S_OK)
{
}

XAudio2AudioService::~XAudio2AudioService()
{
	shutdown();
}

bool XAudio2AudioService::open()
{
	if (m_engine != nullptr) {
		return true;
	}

	HRESULT result = XAudio2Create(&m_engine, 0, XAUDIO2_USE_DEFAULT_PROCESSOR);
	if (FAILED(result)) {
		m_lastError.store(result);
		m_engine = nullptr;
		return false;
	}

	result = m_engine->RegisterForCallbacks(this);
	if (SUCCEEDED(result)) {
		result = m_engine->CreateMasteringVoice(&m_masteringVoice);
	}
	if (FAILED(result)) {
		m_lastError.store(result);
		shutdown();
		return false;
	}

	m_lastError.store(S_OK);
	return true;
}

void XAudio2AudioService::shutdown()
{
	if (m_masteringVoice != nullptr) {
		m_masteringVoice->DestroyVoice();
		m_masteringVoice = nullptr;
	}
	if (m_engine != nullptr) {
		m_engine->UnregisterForCallbacks(this);
		m_engine->Release();
		m_engine = nullptr;
	}
}

bool XAudio2AudioService::isOpen() const
{
	return m_engine != nullptr && m_masteringVoice != nullptr;
}

HRESULT XAudio2AudioService::getLastError() const
{
	return m_lastError.load();
}

void STDMETHODCALLTYPE XAudio2AudioService::OnProcessingPassStart()
{
}

void STDMETHODCALLTYPE XAudio2AudioService::OnProcessingPassEnd()
{
}

void STDMETHODCALLTYPE XAudio2AudioService::OnCriticalError(HRESULT error)
{
	m_lastError.store(error);
}
