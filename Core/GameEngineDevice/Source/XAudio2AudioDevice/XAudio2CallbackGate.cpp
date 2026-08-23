#include "XAudio2AudioDevice/XAudio2CallbackGate.h"

XAudio2CallbackGate::XAudio2CallbackGate() noexcept :
	m_admission(DISABLED_MASK),
	m_generation(0)
{
}

std::uint64_t XAudio2CallbackGate::enable() noexcept
{
	std::lock_guard<std::mutex> transitionLock(m_transitionMutex);
	std::uint64_t admission = m_admission.load(std::memory_order_acquire);
	if ((admission & DISABLED_MASK) == 0) {
		return m_generation.load(std::memory_order_acquire);
	}

	if ((admission & COUNT_MASK) != 0) {
		for (;;) {
			if ((admission & COUNT_MASK) == 0) {
				break;
			}
			m_admission.wait(admission, std::memory_order_acquire);
			admission = m_admission.load(std::memory_order_acquire);
		}
	}

	std::uint64_t generation = m_generation.load(std::memory_order_relaxed);
	++generation;
	if (generation == 0) {
		generation = 1;
	}
	m_generation.store(generation, std::memory_order_release);
	m_admission.store(0, std::memory_order_release);
	return generation;
}

bool XAudio2CallbackGate::tryEnter(Token &token, std::uint64_t expectedGeneration) noexcept
{
	token = {};
	const std::uint64_t observedGeneration = m_generation.load(std::memory_order_acquire);
	if (observedGeneration == 0
		|| (expectedGeneration != 0 && observedGeneration != expectedGeneration)) {
		return false;
	}

	std::uint64_t admission = m_admission.load(std::memory_order_acquire);
	for (;;) {
		if ((admission & DISABLED_MASK) != 0) {
			return false;
		}
		if (m_generation.load(std::memory_order_acquire) != observedGeneration
			|| (expectedGeneration != 0 && observedGeneration != expectedGeneration)) {
			return false;
		}

		const std::uint64_t count = admission & COUNT_MASK;
		if (count == COUNT_MASK) {
			return false;
		}
		const std::uint64_t admitted = admission + 1;
		if (!m_admission.compare_exchange_weak(admission, admitted,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
			continue;
		}

		// Disable may have won immediately after the CAS.  That is still safe:
		// the owner waits for this admitted callback before clearing its sink.
		// A generation change is never allowed while an admission is held.
		if (m_generation.load(std::memory_order_acquire) != observedGeneration) {
			decrementAdmission();
			return false;
		}
		token.generation = observedGeneration;
		token.admitted = true;
		return true;
	}
}

void XAudio2CallbackGate::leave(Token &token) noexcept
{
	if (!token.admitted) {
		return;
	}
	decrementAdmission();
	token = {};
}

bool XAudio2CallbackGate::decrementAdmission() noexcept
{
	std::uint64_t admission = m_admission.load(std::memory_order_acquire);
	for (;;) {
		const std::uint64_t count = admission & COUNT_MASK;
		if (count == 0) {
			return false;
		}
		const std::uint64_t remaining = admission - 1;
		if (m_admission.compare_exchange_weak(admission, remaining,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
			if (count == 1) {
				m_admission.notify_all();
			}
			return true;
		}
	}
}

void XAudio2CallbackGate::disableAndWait() noexcept
{
	std::lock_guard<std::mutex> transitionLock(m_transitionMutex);
	m_admission.fetch_or(DISABLED_MASK, std::memory_order_acq_rel);
	if ((m_admission.load(std::memory_order_acquire) & COUNT_MASK) == 0) {
		return;
	}

	for (;;) {
		const std::uint64_t admission = m_admission.load(std::memory_order_acquire);
		if ((admission & COUNT_MASK) == 0) {
			return;
		}
		m_admission.wait(admission, std::memory_order_acquire);
	}
}
