#include "Lib/NetworkWireContract.h"

#include <cstdio>
#include <limits>

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

int TestFixedSizes()
{
	using namespace rts::network_wire;
	int result = 0;
	result |= Check(kTransportMessageHeaderSize == 6U, "transport header is six bytes");
	result |= Check(kCommandPacketHeaderSize == 6U, "command packet header is six bytes");
	result |= Check(kCommandTypeFieldSize == 2U && kRelayFieldSize == 2U,
		"single-byte command fields retain their two-byte tag/value layout");
	result |= Check(kFrameFieldSize == 5U && kCommandIdFieldSize == 3U,
		"frame and command ID fields retain fixed-width payloads");
	result |= Check(kWrapperCommandBaseSize == 10U && kWrapperCommandFixedDataSize == 22U,
		"wrapper base and fixed metadata sizes match the packet writers");
	result |= Check(kWrapperCommandWireOverhead == 32U,
		"wrapper wire overhead excludes its variable payload");
	return result;
}

int TestSizeConversion()
{
	using namespace rts::network_wire;
	int result = 0;
	UnsignedInt converted = 0U;
	result |= Check(TryConvertSizeToUnsignedInt(0U, converted) && converted == 0U,
		"zero size converts safely");
	result |= Check(TryConvertSizeToUnsignedInt(std::numeric_limits<UnsignedInt>::max(), converted) &&
		converted == std::numeric_limits<UnsignedInt>::max(),
		"largest 32-bit size converts safely");
	if (std::numeric_limits<std::size_t>::max() >
		static_cast<std::size_t>(std::numeric_limits<UnsignedInt>::max()))
	{
		result |= Check(!TryConvertSizeToUnsignedInt(
			static_cast<std::size_t>(std::numeric_limits<UnsignedInt>::max()) + 1U, converted),
			"size_t values above UnsignedInt are rejected");
	}
	return result;
}

int TestWrapperCapacity()
{
	using namespace rts::network_wire;
	int result = 0;
	result |= Check(GetWrapperChunkCapacity(kRetailPacketSize) == 444U,
		"retail wrapper chunk capacity remains 444 bytes");
	result |= Check(GetWrapperChunkCapacity(kNonRetailPacketSize) == 1062U,
		"non-retail wrapper chunk capacity remains 1062 bytes");
	result |= Check(GetWrapperChunkCapacity(kWrapperCommandWireOverhead) == 0U,
		"a packet containing only wrapper overhead has no payload capacity");
	result |= Check(GetWrapperChunkCapacity(kWrapperCommandWireOverhead - 1U) == 0U,
		"an undersized packet has no payload capacity");

	UnsignedInt capacity = 0U;
	result |= Check(TryGetWrapperChunkCapacity(kRetailPacketSize, capacity) && capacity == 444U,
		"retail capacity converts to the legacy 32-bit metadata type");
	result |= Check(!TryGetWrapperChunkCapacity(kWrapperCommandWireOverhead, capacity),
		"zero capacity is rejected for a wrapper chunk");

	UnsignedInt chunks = 0U;
	result |= Check(TryGetWrapperChunkCount(0U, kRetailPacketSize, chunks) && chunks == 0U,
		"empty wrapped data has zero chunks");
	result |= Check(TryGetWrapperChunkCount(444U, kRetailPacketSize, chunks) && chunks == 1U,
		"one full retail chunk has one chunk");
	result |= Check(TryGetWrapperChunkCount(445U, kRetailPacketSize, chunks) && chunks == 2U,
		"data beyond one chunk rounds up safely");
	result |= Check(!TryGetWrapperChunkCount(1U, kWrapperCommandWireOverhead, chunks),
		"chunk count rejects a packet with no payload capacity");
	return result;
}

} // namespace

int main()
{
	return TestFixedSizes() | TestSizeConversion() | TestWrapperCapacity();
}
