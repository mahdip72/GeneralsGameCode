#include "Lib/NetworkWireContract.h"
#include "Lib/NetworkEpochHandshake.h"

#include <array>
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

int TestNetworkHelloContract()
{
	using namespace rts::network_epoch;
	using namespace rts::runtime_epoch;

	constexpr std::uint32_t executableCrc = 0x11223344U;
	constexpr std::uint32_t iniCrc = 0xaabbccddU;
	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> encoded =
		EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U);

	int result = 0;
	result |= Check(kNetworkHelloWireSize == 52U, "NET3 hello uses the fixed 52-byte wire size");
	result |= Check(HasNetworkHelloMagic(encoded.data(), encoded.size()),
		"NET3 hello carries its independent wire magic");
	result |= Check(!HasNetworkHelloMagic(encoded.data(), encoded.size() - 1U),
		"NET3 classification requires the exact wire size");
	result |= Check(!HasNetworkHelloMagic(encoded.data(), 4U),
		"a normal payload prefix is not classified as NET3");
	result |= Check(encoded[4] == 0x01U && encoded[5] == 0x00U &&
		encoded[6] == 0x00U && encoded[7] == 0x00U,
		"NET3 schema version is little endian");
	result |= Check(encoded[12] == 0x44U && encoded[13] == 0x33U &&
		encoded[14] == 0x22U && encoded[15] == 0x11U &&
		encoded[20] == 0xddU && encoded[21] == 0xccU &&
		encoded[22] == 0xbbU && encoded[23] == 0xaaU,
		"NET3 compatibility fields are fixed-width little endian");
	result |= Check(encoded[kNetworkHelloKindOffset] == 0x01U &&
		encoded[kNetworkHelloKindOffset + 1U] == 0x00U &&
		encoded[kNetworkHelloKindOffset + 2U] == 0x00U &&
		encoded[kNetworkHelloKindOffset + 3U] == 0x00U,
		"NET3 Hello kind is fixed-width little endian");
	result |= Check(ReadNetworkHelloKind(encoded.data()) == NetworkHelloKind::Hello,
		"NET3 Hello record kind decodes explicitly");

	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> ackEncoded =
		EncodeNetworkHello(executableCrc, iniCrc, 5U, 2U, NetworkHelloKind::Ack);
	rts::runtime_epoch::NetworkHello decoded;
	result |= Check(ReadNetworkHelloKind(ackEncoded.data()) == NetworkHelloKind::Ack,
		"NET3 Ack record kind decodes explicitly");
	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> unknownKind = encoded;
	WriteLittleEndian32(unknownKind.data() + kNetworkHelloKindOffset, 99U);
	NetworkHelloKind decodedUnknownKind = NetworkHelloKind::Hello;
	NetworkHelloIdentity unknownKindIdentity;
	result |= Check(!DecodeNetworkHelloRecord(unknownKind.data(), unknownKind.size(),
		&decodedUnknownKind, &unknownKindIdentity),
		"NET3 unknown record kinds are rejected before dispatch");
	result |= Check(DecodeAndValidateNetworkHello(unknownKind.data(), unknownKind.size(),
		executableCrc, iniCrc, 2U, 5U, static_cast<NetworkHelloKind>(99U), &decoded).error ==
		rts::runtime_epoch::ValidationError::NetworkKindMismatch,
		"NET3 unknown record kinds fail validation");
	NetworkHelloKind decodedKind = NetworkHelloKind::Hello;
	NetworkHelloIdentity decodedRecordIdentity;
	result |= Check(DecodeNetworkHelloRecord(ackEncoded.data(), ackEncoded.size(),
		&decodedKind, &decodedRecordIdentity) && decodedKind == NetworkHelloKind::Ack &&
		decodedRecordIdentity.senderSlot == 5U && decodedRecordIdentity.recipientSlot == 2U,
		"NET3 Ack carries the stable sender and recipient identity");

	rts::network_epoch::NetworkHelloIdentity identity;
	result |= Check(DecodeAndValidateNetworkHello(encoded.data(), encoded.size(),
		executableCrc, iniCrc, 2U, 5U, &decoded, &identity).ok(),
		"matching NET3 hello validates");
	result |= Check(identity.senderSlot == 2U && identity.recipientSlot == 5U,
		"NET3 carries stable sender and recipient slot identity");
	result |= Check(IsPlausibleNetworkHelloIdentity(encoded.data(), encoded.size(), 5U, 8U),
		"NET3 candidate identity names the local slot and an active slot range");
	result |= Check(decoded.payloadByteCount == 0U && decoded.payloadChecksum == 0U,
		"NET3 hello has no implicit object-layout payload");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> malformed = encoded;
	malformed[0] = 'X';
	result |= Check(DecodeAndValidateNetworkHello(malformed.data(), malformed.size(),
		executableCrc, iniCrc, 2U, 5U, &decoded).error == rts::runtime_epoch::ValidationError::InvalidMagic,
		"NET3 wrong magic is rejected");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> wrongBuild = encoded;
	wrongBuild[12] ^= 0x01U;
	result |= Check(DecodeAndValidateNetworkHello(wrongBuild.data(), wrongBuild.size(),
		executableCrc, iniCrc, 2U, 5U, &decoded).error == rts::runtime_epoch::ValidationError::BuildCompatibilityMismatch,
		"NET3 incompatible build identity is rejected");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> wrongContent = encoded;
	wrongContent[20] ^= 0x01U;
	result |= Check(DecodeAndValidateNetworkHello(wrongContent.data(), wrongContent.size(),
		executableCrc, iniCrc, 2U, 5U, &decoded).error == rts::runtime_epoch::ValidationError::ContentHashMismatch,
		"NET3 incompatible content identity is rejected");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> wrongIdentity = encoded;
	wrongIdentity[kNetworkHelloSenderSlotOffset] ^= 0x01U;
	result |= Check(DecodeAndValidateNetworkHello(wrongIdentity.data(), wrongIdentity.size(),
		executableCrc, iniCrc, 2U, 5U, &decoded).error == rts::runtime_epoch::ValidationError::NetworkIdentityMismatch,
		"NET3 sender identity is validated independently of endpoint address");

	result |= Check(DecodeAndValidateNetworkHello(ackEncoded.data(), ackEncoded.size(),
		executableCrc, iniCrc, 5U, 2U, NetworkHelloKind::Ack, &decoded).ok(),
		"matching NET3 Ack validates");
	result |= Check(DecodeAndValidateNetworkHello(ackEncoded.data(), ackEncoded.size(),
		executableCrc, iniCrc, 5U, 2U, NetworkHelloKind::Hello, &decoded).error ==
		rts::runtime_epoch::ValidationError::NetworkKindMismatch,
		"NET3 Hello and Ack kinds cannot be interchanged");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> unrelatedPrefix = encoded;
	WriteLittleEndian32(unrelatedPrefix.data() + kNetworkHelloSenderSlotOffset, 99U);
	result |= Check(!IsPlausibleNetworkHelloIdentity(unrelatedPrefix.data(), unrelatedPrefix.size(), 5U, 8U),
		"NET3-sized unrelated payloads are not handshake candidates");

	result |= Check(DecodeAndValidateNetworkHello(encoded.data(), encoded.size() - 1U,
		executableCrc, iniCrc, 2U, 5U, &decoded).error == rts::runtime_epoch::ValidationError::InvalidSize,
		"NET3 truncated records are rejected");
	result |= Check(IsNetworkHelloRetryDue(kNetworkHelloRetryIntervalMs, 0U),
		"NET3 retry policy has a deterministic retry boundary");
	result |= Check(!IsNetworkHelloRetryDue(kNetworkHelloRetryIntervalMs - 1U, 0U),
		"NET3 retry policy does not retry early");
	result |= Check(IsNetworkHelloTimedOut(kNetworkHelloTimeoutMs, 0U),
		"NET3 retry policy has a deterministic timeout boundary");
	result |= Check(IsNetworkHelloAttemptLimitReached(kNetworkHelloMaxAttempts),
		"NET3 retry policy has a deterministic attempt limit");
	result |= Check(!IsNetworkHelloComplete(0x06U, 0x02U, 0x06U),
		"NET3 remains pending until every expected peer Hello validates");
	result |= Check(!IsNetworkHelloComplete(0x06U, 0x06U, 0x02U),
		"NET3 remains pending until every expected peer Ack arrives");
	result |= Check(IsNetworkHelloComplete(0x06U, 0x06U, 0x06U),
		"NET3 completes when every expected peer validates and acknowledges");
	result |= Check(IsNetworkHelloComplete(0U, 0U, 0U),
		"NET3 with no expected remote peers is already complete");
	result |= Check(IsNetworkHelloGateReady(false, false),
		"NET3 gate is ready when no exchange is required and no failure exists");
	result |= Check(!IsNetworkHelloGateReady(true, false),
		"NET3 gate remains closed until the owner clears the required state");
	result |= Check(!IsNetworkHelloGateReady(false, true),
		"NET3 gate remains closed after a handshake failure");
	return result;
}

} // namespace

int main()
{
	return TestFixedSizes() | TestSizeConversion() | TestWrapperCapacity() |
		TestNetworkHelloContract();
}
