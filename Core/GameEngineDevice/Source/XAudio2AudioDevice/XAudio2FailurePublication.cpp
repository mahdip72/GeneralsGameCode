#include "XAudio2AudioDevice/XAudio2FailurePublication.h"

HRESULT XAudio2FailurePublication::normalize(HRESULT error) noexcept
{
	return FAILED(error) ? error : E_FAIL;
}

std::uint64_t XAudio2FailurePublication::snapshot() const noexcept
{
	return m_publication.load(std::memory_order_acquire);
}

bool XAudio2FailurePublication::hasFailure() const noexcept
{
	return (snapshot() & FAILURE_MASK) != 0;
}

HRESULT XAudio2FailurePublication::failure() const noexcept
{
	const std::uint64_t publication = snapshot();
	if ((publication & FAILURE_MASK) == 0) {
		return S_OK;
	}
	return normalize(static_cast<HRESULT>(static_cast<std::uint32_t>(publication & ERROR_MASK)));
}

bool XAudio2FailurePublication::publish(HRESULT error) noexcept
{
	const std::uint64_t normalized = static_cast<std::uint64_t>(
		static_cast<std::uint32_t>(normalize(error)));
	std::uint64_t expected = m_publication.load(std::memory_order_acquire);
	for (;;) {
		if ((expected & FAILURE_MASK) != 0) {
			return false;
		}
		const std::uint64_t published = (expected & EPOCH_MASK) | FAILURE_MASK | normalized;
		if (m_publication.compare_exchange_weak(expected, published,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
			return true;
		}
	}
}

bool XAudio2FailurePublication::tryCommit(std::uint64_t expected, std::uint64_t &committed) noexcept
{
	committed = 0;
	if ((expected & FAILURE_MASK) != 0) {
		return false;
	}
	const std::uint64_t next = nextEpoch(expected);
	if (m_publication.compare_exchange_strong(expected, next,
			std::memory_order_acq_rel, std::memory_order_acquire)) {
		committed = next;
		return true;
	}
	return false;
}

void XAudio2FailurePublication::clear() noexcept
{
	std::uint64_t expected = m_publication.load(std::memory_order_acquire);
	while ((expected & FAILURE_MASK) != 0) {
		const std::uint64_t next = nextEpoch(expected);
		if (m_publication.compare_exchange_weak(expected, next,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
			return;
		}
	}
}

std::uint64_t XAudio2FailurePublication::nextEpoch(std::uint64_t publication) noexcept
{
	return ((publication & EPOCH_MASK) + EPOCH_INCREMENT) & EPOCH_MASK;
}
