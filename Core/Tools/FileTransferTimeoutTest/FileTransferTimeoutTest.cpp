#include "Lib/FileTransferTimeout.h"

#include <cstdio>

namespace
{

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int TestWrapBoundaryBeforeTimeout()
{
	const uint32_t start = 0xfffffff0U;
	const uint32_t timeout = 120000U;
	int result = 0;
	result |= Check(!rts::file_transfer::IsTimedOut(start, start, timeout),
		"the transfer is active when its timer starts");
	result |= Check(rts::file_transfer::RemainingSeconds(start, start, timeout) == 120U,
		"the full timeout remains when the transfer starts");
	result |= Check(!rts::file_transfer::IsTimedOut(0x00000020U, start, timeout),
		"a timer that crosses the 32-bit wrap has not timed out after 48 ms");
	result |= Check(rts::file_transfer::RemainingSeconds(0x00000020U, start, timeout) == 119U,
		"remaining transfer time uses the wrapped elapsed interval");
	result |= Check(!rts::file_transfer::IsTimedOut(0x0001d0c8U, start, timeout),
		"the wrapped timer remains active with one second left");
	result |= Check(rts::file_transfer::RemainingSeconds(0x0001d0c8U, start, timeout) == 1U,
		"the wrapped timer reports its final whole second");
	return result;
}

int TestWrapBoundaryAtTimeout()
{
	const uint32_t start = 0xfffffff0U;
	const uint32_t timeout = 120000U;
	int result = 0;
	result |= Check(!rts::file_transfer::IsTimedOut(0x0001d4afU, start, timeout),
		"the millisecond before the wrapped deadline remains active");
	result |= Check(rts::file_transfer::IsTimedOut(0x0001d4b0U, start, timeout),
		"the wrapped deadline expires at exactly the timeout interval");
	result |= Check(rts::file_transfer::RemainingSeconds(0x0001d4b0U, start, timeout) == 0U,
		"remaining transfer time is zero at the wrapped deadline");
	result |= Check(rts::file_transfer::IsTimedOut(0x0001d4b1U, start, timeout),
		"the wrapped timer remains expired after its deadline");
	return result;
}

} // namespace

int main()
{
	return TestWrapBoundaryBeforeTimeout() | TestWrapBoundaryAtTimeout();
}
