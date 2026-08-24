#pragma once

#include "Lib/RuntimeEpochContract.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rts
{
namespace network_epoch
{

// The gameplay transport carries this record as an opaque byte sequence.  It
// must never be replaced with sizeof(NetworkHello): NetworkHello is a logical
// contract, while the wire representation is the fixed NET3 encoding owned by
// this boundary.  A four-byte record kind makes the exchange explicitly
// bidirectional: each peer sends a Hello and acknowledges the peer's Hello.
// The final eight bytes identify the connection's stable sender/recipient
// slots. Endpoint addresses are deliberately not part of the identity because
// NAT may rewrite the source port.
constexpr std::size_t kNetworkHelloKindSize = sizeof(std::uint32_t);
constexpr std::size_t kNetworkHelloIdentitySize = 8U;
constexpr std::size_t kNetworkHelloWireSize =
	runtime_epoch::kHeaderSize + kNetworkHelloKindSize + kNetworkHelloIdentitySize;
constexpr std::size_t kNetworkHelloKindOffset = runtime_epoch::kHeaderSize;
constexpr std::size_t kNetworkHelloSenderSlotOffset =
	kNetworkHelloKindOffset + kNetworkHelloKindSize;
constexpr std::size_t kNetworkHelloRecipientSlotOffset =
	kNetworkHelloSenderSlotOffset + sizeof(std::uint32_t);

enum class NetworkHelloKind : std::uint32_t
{
	Hello = 1U,
	Ack = 2U
};

// Retry policy is intentionally fixed so a missing peer cannot stall the
// pregame/map-transfer path indefinitely.  A round includes the initial send;
// subsequent rounds are spaced by the retry interval and the overall timeout
// remains the final bound if the timer is delayed.
constexpr std::uint32_t kNetworkHelloRetryIntervalMs = 1000U;
constexpr std::uint32_t kNetworkHelloMaxAttempts = 5U;
constexpr std::uint32_t kNetworkHelloTimeoutMs = 10000U;

struct NetworkHelloIdentity
{
	std::uint32_t senderSlot;
	std::uint32_t recipientSlot;
};

inline bool IsNetworkHelloRetryDue(std::uint32_t now, std::uint32_t lastSend)
{
	return static_cast<std::uint32_t>(now - lastSend) >= kNetworkHelloRetryIntervalMs;
}

inline bool IsNetworkHelloTimedOut(std::uint32_t now, std::uint32_t startTime)
{
	return static_cast<std::uint32_t>(now - startTime) >= kNetworkHelloTimeoutMs;
}

inline bool IsNetworkHelloAttemptLimitReached(std::uint32_t attempts)
{
	return attempts >= kNetworkHelloMaxAttempts;
}

inline bool IsNetworkHelloComplete(std::uint32_t expectedPeerMask,
	std::uint32_t validatedPeerMask,
	std::uint32_t acknowledgedPeerMask)
{
	return (validatedPeerMask & expectedPeerMask) == expectedPeerMask &&
		(acknowledgedPeerMask & expectedPeerMask) == expectedPeerMask;
}

inline bool IsNetworkHelloGateReady(bool exchangeRequired, bool exchangeFailed)
{
	return !exchangeRequired && !exchangeFailed;
}

inline void WriteLittleEndian32(runtime_epoch::Byte *output, std::uint32_t value)
{
	for (std::size_t index = 0; index < sizeof(value); ++index)
	{
		output[index] = static_cast<runtime_epoch::Byte>(value & 0xffU);
		value >>= 8U;
	}
}

inline std::uint32_t ReadLittleEndian32(const runtime_epoch::Byte *input)
{
	std::uint32_t value = 0U;
	for (std::size_t index = 0; index < sizeof(value); ++index)
		value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
	return value;
}

inline bool HasNetworkHelloMagic(const runtime_epoch::Byte *input, std::size_t inputSize);

inline NetworkHelloKind ReadNetworkHelloKind(const runtime_epoch::Byte *input)
{
	return static_cast<NetworkHelloKind>(ReadLittleEndian32(input + kNetworkHelloKindOffset));
}

inline bool IsKnownNetworkHelloKind(NetworkHelloKind kind)
{
	return kind == NetworkHelloKind::Hello || kind == NetworkHelloKind::Ack;
}

inline bool DecodeNetworkHelloRecord(const runtime_epoch::Byte *input,
	std::size_t inputSize,
	NetworkHelloKind *kind,
	NetworkHelloIdentity *identity);

inline bool DecodeNetworkHelloIdentity(const runtime_epoch::Byte *input,
	std::size_t inputSize,
	NetworkHelloIdentity *identity)
{
	if (identity == nullptr || !HasNetworkHelloMagic(input, inputSize))
		return false;
	identity->senderSlot = ReadLittleEndian32(input + kNetworkHelloSenderSlotOffset);
	identity->recipientSlot = ReadLittleEndian32(input + kNetworkHelloRecipientSlotOffset);
	return true;
}

inline runtime_epoch::NetworkHello MakeNetworkHello(std::uint32_t executableCrc,
	std::uint32_t iniCrc)
{
	runtime_epoch::NetworkHello hello;
	hello.buildCompatibilityId = runtime_epoch::BuildCompatibilityIdFromExecutableCrc(executableCrc);
	hello.contentHash = runtime_epoch::ContentHashFromIniCrc(iniCrc);
	hello.payloadByteCount = 0U;
	hello.payloadChecksum = runtime_epoch::CalculatePayloadChecksum(nullptr, 0U);
	return hello;
}

inline runtime_epoch::ValidationOptions MakeNetworkHelloValidationOptions(
	std::uint32_t executableCrc,
	std::uint32_t iniCrc)
{
	runtime_epoch::ValidationOptions options;
	options.expectedBuildCompatibilityId =
		runtime_epoch::BuildCompatibilityIdFromExecutableCrc(executableCrc);
	options.expectedContentHash = runtime_epoch::ContentHashFromIniCrc(iniCrc);
	options.maxPayloadByteCount = 0U;
	options.requireBuildCompatibilityMatch = true;
	options.requireContentHashMatch = true;
	return options;
}

inline std::array<runtime_epoch::Byte, kNetworkHelloWireSize> EncodeNetworkHello(
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	std::uint32_t senderSlot = 0U,
	std::uint32_t recipientSlot = 0U,
	NetworkHelloKind kind = NetworkHelloKind::Hello)
{
	const std::array<runtime_epoch::Byte, runtime_epoch::kHeaderSize> header =
		runtime_epoch::Encode(MakeNetworkHello(executableCrc, iniCrc));
	std::array<runtime_epoch::Byte, kNetworkHelloWireSize> output = {{}};
	for (std::size_t index = 0; index < header.size(); ++index)
		output[index] = header[index];
	WriteLittleEndian32(output.data() + kNetworkHelloKindOffset,
		static_cast<std::uint32_t>(kind));
	WriteLittleEndian32(output.data() + kNetworkHelloSenderSlotOffset, senderSlot);
	WriteLittleEndian32(output.data() + kNetworkHelloRecipientSlotOffset, recipientSlot);
	return output;
}

inline runtime_epoch::ValidationResult DecodeAndValidateNetworkHello(
	const runtime_epoch::Byte *input,
	std::size_t inputSize,
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	std::uint32_t expectedSenderSlot,
	std::uint32_t expectedRecipientSlot,
	NetworkHelloKind expectedKind,
	runtime_epoch::NetworkHello *hello,
	NetworkHelloIdentity *identity = nullptr)
{
	if (input == nullptr || inputSize != kNetworkHelloWireSize || hello == nullptr)
	{
		return {runtime_epoch::ValidationError::InvalidSize};
	}
	if (!HasNetworkHelloMagic(input, inputSize))
	{
		return {runtime_epoch::ValidationError::InvalidMagic};
	}
	const NetworkHelloKind actualKind = ReadNetworkHelloKind(input);
	if (!IsKnownNetworkHelloKind(actualKind) || actualKind != expectedKind)
	{
		return {runtime_epoch::ValidationError::NetworkKindMismatch};
	}

	const runtime_epoch::ValidationOptions options =
		MakeNetworkHelloValidationOptions(executableCrc, iniCrc);
	const runtime_epoch::ValidationResult headerResult =
		runtime_epoch::DecodeAndValidate(input, runtime_epoch::kHeaderSize,
		nullptr, 0U, options, hello);
	if (!headerResult.ok())
		return headerResult;

	NetworkHelloIdentity decodedIdentity;
	decodedIdentity.senderSlot =
		ReadLittleEndian32(input + kNetworkHelloSenderSlotOffset);
	decodedIdentity.recipientSlot =
		ReadLittleEndian32(input + kNetworkHelloRecipientSlotOffset);
	if (decodedIdentity.senderSlot != expectedSenderSlot ||
		decodedIdentity.recipientSlot != expectedRecipientSlot)
	{
		return {runtime_epoch::ValidationError::NetworkIdentityMismatch};
	}
	if (identity != nullptr)
		*identity = decodedIdentity;
	return {};
}

// Preserve the original Hello-only call shape for focused contract tests and
// other boundary users while making the record kind explicit on the wire.
inline runtime_epoch::ValidationResult DecodeAndValidateNetworkHello(
	const runtime_epoch::Byte *input,
	std::size_t inputSize,
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	std::uint32_t expectedSenderSlot,
	std::uint32_t expectedRecipientSlot,
	runtime_epoch::NetworkHello *hello,
	NetworkHelloIdentity *identity = nullptr)
{
	return DecodeAndValidateNetworkHello(input, inputSize, executableCrc, iniCrc,
		expectedSenderSlot, expectedRecipientSlot, NetworkHelloKind::Hello,
		hello, identity);
}

// A transport payload is considered a NET3 candidate only when its fixed
// magic/size and slot identity point at a real connection in the current
// game. Checking the identity before invoking the epoch decoder keeps an
// unrelated fixed-size payload beginning with "NET3" out of the legacy packet
// parser while still allowing a peer with an incompatible build/content hash
// to reach DecodeAndValidateNetworkHello and be rejected by the handshake.
inline bool IsPlausibleNetworkHelloIdentity(const runtime_epoch::Byte *input,
	std::size_t inputSize,
	std::uint32_t localSlot,
	std::uint32_t maxSlots)
{
	NetworkHelloIdentity identity;
	if (!DecodeNetworkHelloIdentity(input, inputSize, &identity))
		return false;

	return identity.recipientSlot == localSlot &&
		identity.senderSlot < maxSlots &&
		identity.recipientSlot < maxSlots &&
		identity.senderSlot != localSlot;
}

inline bool DecodeNetworkHelloRecord(const runtime_epoch::Byte *input,
	std::size_t inputSize,
	NetworkHelloKind *kind,
	NetworkHelloIdentity *identity)
{
	if (kind == nullptr || identity == nullptr ||
		!HasNetworkHelloMagic(input, inputSize))
	{
		return false;
	}

	*kind = ReadNetworkHelloKind(input);
	if (!IsKnownNetworkHelloKind(*kind))
		return false;
	identity->senderSlot = ReadLittleEndian32(input + kNetworkHelloSenderSlotOffset);
	identity->recipientSlot = ReadLittleEndian32(input + kNetworkHelloRecipientSlotOffset);
	return true;
}

inline bool HasNetworkHelloMagic(const runtime_epoch::Byte *input, std::size_t inputSize)
{
	if (input == nullptr || inputSize != kNetworkHelloWireSize)
	{
		return false;
	}

	for (std::size_t index = 0; index < runtime_epoch::kNetworkMagic.size(); ++index)
	{
		if (input[index] != runtime_epoch::kNetworkMagic[index])
		{
			return false;
		}
	}
	return true;
}

} // namespace network_epoch
} // namespace rts
