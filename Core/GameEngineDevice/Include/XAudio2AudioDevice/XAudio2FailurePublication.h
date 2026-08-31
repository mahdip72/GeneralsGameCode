#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

// A lock-free publication/fence used by the service lifecycle owner and its
// native callback.  A failure publication and an owner commit compete on the
// same atomic word: whichever CAS wins is the linearization point.  The low
// 32 bits carry the first normalized HRESULT when the failure bit is set; the
// upper epoch bits advance on successful owner commits.
class XAudio2FailurePublication final
{
public:
	XAudio2FailurePublication() noexcept = default;
	XAudio2FailurePublication(const XAudio2FailurePublication &) = delete;
	XAudio2FailurePublication &operator=(const XAudio2FailurePublication &) = delete;

	static HRESULT normalize(HRESULT error) noexcept;

	std::uint64_t snapshot() const noexcept;
	bool hasFailure() const noexcept;
	HRESULT failure() const noexcept;

	// The first successful publisher wins.  The CAS also fences an owner that
	// is trying to commit an open/create operation.
	bool publish(HRESULT error) noexcept;

	// Commit only if the caller observed the same failure-free publication.  A
	// successful CAS is the operation's linearization point.
	bool tryCommit(std::uint64_t expected, std::uint64_t &committed) noexcept;

	// Clear a consumed failure while retaining a monotonically advancing epoch.
	void clear() noexcept;

private:
	static constexpr std::uint64_t FAILURE_MASK = (std::uint64_t(1) << 63);
	static constexpr std::uint64_t ERROR_MASK = 0xffffffffull;
	static constexpr std::uint64_t EPOCH_MASK = 0x7fffffff00000000ull;
	static constexpr std::uint64_t EPOCH_INCREMENT = 0x0000000100000000ull;

	static std::uint64_t nextEpoch(std::uint64_t publication) noexcept;

	std::atomic<std::uint64_t> m_publication { 0 };
};
