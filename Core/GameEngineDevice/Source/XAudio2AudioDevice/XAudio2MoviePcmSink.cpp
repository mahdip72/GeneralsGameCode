#include "XAudio2AudioDevice/XAudio2MoviePcmSink.h"

XAudio2MoviePcmSink::XAudio2MoviePcmSink(XAudio2AudioService &service) noexcept :
	m_service(&service),
	m_handle(service.createVoice()),
	m_closed(false)
{
}

XAudio2MoviePcmSink::~XAudio2MoviePcmSink()
{
	close();
}

bool XAudio2MoviePcmSink::isReady() const noexcept
{
	return !m_closed && m_service != nullptr && m_handle.isValid()
		&& m_service->isVoiceOpen(m_handle) && !m_service->isVoiceFailed(m_handle);
}

AudioPcmSubmitResult XAudio2MoviePcmSink::submit(AudioPcmChunk &&chunk)
{
	if (m_closed || m_service == nullptr || !m_handle.isValid()) {
		chunk = {};
		return AudioPcmSubmitResult::DROPPED;
	}
	if (m_service->isVoiceFailed(m_handle)) {
		chunk = {};
		return AudioPcmSubmitResult::FAILED;
	}
	return m_service->submit(m_handle, std::move(chunk));
}

void XAudio2MoviePcmSink::reset(std::uint64_t generation)
{
	if (!m_closed && m_service != nullptr && m_handle.isValid()) {
		m_service->resetVoice(m_handle, generation);
	}
}

bool XAudio2MoviePcmSink::getPlayedSample(std::int64_t &sample) const noexcept
{
	return !m_closed && m_service != nullptr && m_handle.isValid()
		&& m_service->getVoicePlayedSample(m_handle, sample);
}

bool XAudio2MoviePcmSink::service() noexcept
{
	return !m_closed && m_service != nullptr && m_handle.isValid()
		&& m_service->serviceVoice(m_handle);
}

void XAudio2MoviePcmSink::close() noexcept
{
	if (m_closed) {
		return;
	}
	m_closed = true;
	if (m_service != nullptr && m_handle.isValid()) {
		m_service->destroyVoice(m_handle);
	}
	m_handle = {};
}
