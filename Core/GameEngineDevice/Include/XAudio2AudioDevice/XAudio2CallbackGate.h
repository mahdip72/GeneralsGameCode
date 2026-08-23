#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

// Admission/drain gate for callbacks owned by the native XAudio2 adapter.
// Callback threads only touch atomics and notify the owner; the owner may wait
// for the admitted count to reach zero before releasing native callback state.
class XAudio2CallbackGate final
{
public:
	struct Token
	{
		std::uint64_t generation = 0;
		bool admitted = false;
	};

	XAudio2CallbackGate() noexcept;
	XAudio2CallbackGate(const XAudio2CallbackGate &) = delete;
	XAudio2CallbackGate &operator=(const XAudio2CallbackGate &) = delete;

	// Enable a new callback generation.  The owner calls this only after any
	// prior generation has been disabled and drained.
	std::uint64_t enable() noexcept;

	// Admit a callback in the currently enabled generation.  An expected
	// generation of zero means "the generation observed by this call".
	bool tryEnter(Token &token, std::uint64_t expectedGeneration = 0) noexcept;
	void leave(Token &token) noexcept;

	// Linearizes callback admission with shutdown: callbacks that win admission
	// before this call are drained, and callbacks that lose the race are rejected.
	void disableAndWait() noexcept;

private:
	static constexpr std::uint64_t DISABLED_MASK = (std::uint64_t(1) << 63);
	static constexpr std::uint64_t COUNT_MASK = ~DISABLED_MASK;

	bool decrementAdmission() noexcept;

	std::atomic<std::uint64_t> m_admission;
	std::atomic<std::uint64_t> m_generation;
	std::mutex m_transitionMutex;
};
