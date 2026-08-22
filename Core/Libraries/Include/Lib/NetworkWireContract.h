#pragma once

#include "Lib/BaseTypeCore.h"

#include <cstddef>
#include <limits>

// This contract describes the byte counts emitted by the current legacy
// network writers.  It is deliberately independent from the in-memory
// message classes so a 64-bit port cannot accidentally use pointer-sized
// layout or sizeof(GameMessage) as a wire-format boundary.
namespace rts
{
namespace network_wire
{

constexpr std::size_t kTransportMessageHeaderSize = sizeof(UnsignedInt) + sizeof(UnsignedShort);
constexpr std::size_t kCommandPacketHeaderSize = sizeof(UnsignedInt) + sizeof(UnsignedShort);

constexpr std::size_t kCommandTypeFieldSize = sizeof(UnsignedByte) + sizeof(UnsignedByte);
constexpr std::size_t kRelayFieldSize = sizeof(UnsignedByte) + sizeof(UnsignedByte);
constexpr std::size_t kFrameFieldSize = sizeof(UnsignedByte) + sizeof(UnsignedInt);
constexpr std::size_t kPlayerIdFieldSize = sizeof(UnsignedByte) + sizeof(UnsignedByte);
constexpr std::size_t kCommandIdFieldSize = sizeof(UnsignedByte) + sizeof(UnsignedShort);
constexpr std::size_t kDataFieldSize = sizeof(UnsignedByte);
constexpr std::size_t kRepeatFieldSize = sizeof(UnsignedByte);

// NetPacketWrapperCommandBase writes command type, relay, player ID, command
// ID, and the data marker.  NetPacketWrapperCommandData then writes its five
// 32-bit metadata fields plus the wrapped-command ID before the variable
// payload.
constexpr std::size_t kWrapperCommandBaseSize =
	kCommandTypeFieldSize + kRelayFieldSize + kPlayerIdFieldSize +
	kCommandIdFieldSize + kDataFieldSize;
constexpr std::size_t kWrapperCommandFixedDataSize =
	sizeof(UnsignedShort) + 5U * sizeof(UnsignedInt);
constexpr std::size_t kWrapperCommandWireOverhead =
	kWrapperCommandBaseSize + kWrapperCommandFixedDataSize;

// These are the two existing MAX_PACKET_SIZE values from NetworkDefs.h.  The
// non-retail value is MAX_UDP_PAYLOAD_SIZE - sizeof(TransportMessageHeader).
// They are kept here as protocol facts for tests and migration code only.
constexpr std::size_t kRetailPacketSize = 476U;
constexpr std::size_t kMaxUdpPayloadSize = 1100U;
constexpr std::size_t kNonRetailPacketSize = kMaxUdpPayloadSize - kTransportMessageHeaderSize;

static_assert(kTransportMessageHeaderSize == 6U, "transport header bytes changed");
static_assert(kCommandPacketHeaderSize == 6U, "command packet header bytes changed");
static_assert(kCommandTypeFieldSize == 2U, "command type field bytes changed");
static_assert(kRelayFieldSize == 2U, "relay field bytes changed");
static_assert(kFrameFieldSize == 5U, "frame field bytes changed");
static_assert(kPlayerIdFieldSize == 2U, "player ID field bytes changed");
static_assert(kCommandIdFieldSize == 3U, "command ID field bytes changed");
static_assert(kDataFieldSize == 1U, "data field bytes changed");
static_assert(kRepeatFieldSize == 1U, "repeat field bytes changed");
static_assert(kWrapperCommandBaseSize == 10U, "wrapper command base bytes changed");
static_assert(kWrapperCommandFixedDataSize == 22U, "wrapper metadata bytes changed");
static_assert(kWrapperCommandWireOverhead == 32U, "wrapper wire overhead changed");

constexpr std::size_t GetWrapperChunkCapacity(std::size_t packetSize,
	std::size_t wrapperWireOverhead = kWrapperCommandWireOverhead)
{
	return packetSize > wrapperWireOverhead ? packetSize - wrapperWireOverhead : 0U;
}

constexpr bool TryConvertSizeToUnsignedInt(std::size_t value, UnsignedInt &converted)
{
	if (value > static_cast<std::size_t>(std::numeric_limits<UnsignedInt>::max()))
	{
		return false;
	}
	converted = static_cast<UnsignedInt>(value);
	return true;
}

constexpr bool TryGetWrapperChunkCapacity(std::size_t packetSize,
	UnsignedInt &capacity,
	std::size_t wrapperWireOverhead = kWrapperCommandWireOverhead)
{
	const std::size_t chunkCapacity = GetWrapperChunkCapacity(packetSize, wrapperWireOverhead);
	if (chunkCapacity == 0U)
	{
		return false;
	}
	return TryConvertSizeToUnsignedInt(chunkCapacity, capacity);
}

constexpr bool TryGetWrapperChunkCount(std::size_t totalDataLength,
	std::size_t packetSize,
	UnsignedInt &chunkCount,
	std::size_t wrapperWireOverhead = kWrapperCommandWireOverhead)
{
	const std::size_t chunkCapacity = GetWrapperChunkCapacity(packetSize, wrapperWireOverhead);
	if (totalDataLength == 0U)
	{
		chunkCount = 0U;
		return true;
	}
	if (chunkCapacity == 0U)
	{
		return false;
	}

	// Avoid totalDataLength + chunkCapacity - 1 overflowing size_t.
	const std::size_t quotient = totalDataLength / chunkCapacity;
	const std::size_t remainder = totalDataLength % chunkCapacity;
	const std::size_t count = quotient + (remainder != 0U ? 1U : 0U);
	return TryConvertSizeToUnsignedInt(count, chunkCount);
}

} // namespace network_wire
} // namespace rts
