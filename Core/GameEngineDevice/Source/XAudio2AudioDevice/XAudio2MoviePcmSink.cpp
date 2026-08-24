#include "XAudio2AudioDevice/XAudio2MoviePcmSink.h"

XAudio2MoviePcmSink::XAudio2MoviePcmSink(XAudio2AudioService &service) noexcept :
	m_service(&service),
	m_handle(service.createVoice()),
	m_closed(false),
	m_endOfStream(false)
{
}

XAudio2MoviePcmSink::~XAudio2MoviePcmSink()
{
	close();
}

bool XAudio2MoviePcmSink::isReady() const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return !m_closed && m_service != nullptr && m_handle.isValid()
		&& m_service->isVoiceOpen(m_handle) && !m_service->isVoiceFailed(m_handle);
}

AudioPcmSubmitResult XAudio2MoviePcmSink::submit(AudioPcmChunk &&chunk)
{
	std::lock_guard<std::mutex> lock(m_mutex);
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
	std::lock_guard<std::mutex> lock(m_mutex);
	m_endOfStream = false;
	if (!m_closed && m_service != nullptr && m_handle.isValid()) {
		m_service->resetVoice(m_handle, generation);
	}
}

void XAudio2MoviePcmSink::endOfStream() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_closed) {
		m_endOfStream = true;
	}
}

bool XAudio2MoviePcmSink::isDrained() const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return !m_closed && m_endOfStream && m_service != nullptr && m_handle.isValid()
		&& !m_service->isVoiceFailed(m_handle)
		&& m_service->isVoiceDrained(m_handle);
}

bool XAudio2MoviePcmSink::getPlayedSample(std::int64_t &sample) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return !m_closed && m_service != nullptr && m_handle.isValid()
		&& m_service->getVoicePlayedSample(m_handle, sample);
}

bool XAudio2MoviePcmSink::service() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_closed || m_service == nullptr || !m_handle.isValid()
		|| !m_service->serviceVoice(m_handle)) {
		return false;
	}
	return !m_service->isVoiceFailed(m_handle);
}

void XAudio2MoviePcmSink::close() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_closed) {
		return;
	}
	m_closed = true;
	m_endOfStream = false;
	if (m_service != nullptr && m_handle.isValid()) {
		m_service->destroyVoice(m_handle);
	}
	m_handle = {};
}
