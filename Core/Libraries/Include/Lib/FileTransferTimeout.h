#pragma once

#include <Utility/stdint_adapter.h>

namespace rts
{
namespace file_transfer
{

inline uint32_t ElapsedMilliseconds(uint32_t now, uint32_t start)
{
	return now - start;
}

inline bool IsTimedOut(uint32_t now, uint32_t start, uint32_t timeout)
{
	return ElapsedMilliseconds(now, start) >= timeout;
}

inline uint32_t RemainingSeconds(uint32_t now, uint32_t start, uint32_t timeout)
{
	const uint32_t elapsed = ElapsedMilliseconds(now, start);
	if (elapsed >= timeout)
		return 0U;
	return (timeout - elapsed) / 1000U;
}

} // namespace file_transfer
} // namespace rts
