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
// The payload identifies the record kind, stable sender/recipient slots, and a
// per-exchange session challenge.  All payload fields are covered by the epoch
// checksum. The challenge rejects passive stale-record replay; it is not a
// keyed peer authenticator. Endpoint addresses are deliberately not part of
// the identity because NAT may rewrite the source port.
//
// This tokenized 60-byte record is mandatory for the clean Stage 3 runtime
// epoch. Earlier 52-byte development records and pre-epoch peers are
// intentionally unsupported and are quarantined by their NET3 prefix.
constexpr std::size_t kNetworkHelloKindSize = sizeof(std::uint32_t);
constexpr std::size_t kNetworkHelloIdentitySize = 8U;
constexpr std::size_t kNetworkHelloSessionTokenSize = sizeof(std::uint64_t);
constexpr std::size_t kNetworkHelloPayloadSize =
	kNetworkHelloKindSize + kNetworkHelloIdentitySize + kNetworkHelloSessionTokenSize;
constexpr std::size_t kNetworkHelloWireSize =
	runtime_epoch::kHeaderSize + kNetworkHelloPayloadSize;
constexpr std::size_t kNetworkHelloKindOffset = runtime_epoch::kHeaderSize;
constexpr std::size_t kNetworkHelloSenderSlotOffset =
	kNetworkHelloKindOffset + kNetworkHelloKindSize;
constexpr std::size_t kNetworkHelloRecipientSlotOffset =
	kNetworkHelloSenderSlotOffset + sizeof(std::uint32_t);
constexpr std::size_t kNetworkHelloSessionTokenOffset =
	kNetworkHelloRecipientSlotOffset + sizeof(std::uint32_t);
constexpr std::uint64_t kAnyNetworkHelloSessionToken = 0U;
static_assert(kNetworkHelloSessionTokenOffset + kNetworkHelloSessionTokenSize ==
	kNetworkHelloWireSize);

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

inline bool IsNetworkFramePublicationAllowed(bool failureHandled, bool exchangeFailed)
{
	return !failureHandled && !exchangeFailed;
}

inline bool ShouldHandleNetworkHelloFailure(bool failureHandled, bool exchangeFailed)
{
	return !failureHandled && exchangeFailed;
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

inline void WriteLittleEndian64(runtime_epoch::Byte *output, std::uint64_t value)
{
	for (std::size_t index = 0; index < sizeof(value); ++index)
	{
		output[index] = static_cast<runtime_epoch::Byte>(value & 0xffU);
		value >>= 8U;
	}
}

inline std::uint64_t ReadLittleEndian64(const runtime_epoch::Byte *input)
{
	std::uint64_t value = 0U;
	for (std::size_t index = 0; index < sizeof(value); ++index)
		value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
	return value;
}

inline bool HasNetworkHelloMagic(const runtime_epoch::Byte *input, std::size_t inputSize);

inline bool HasNetworkHelloPrefix(const runtime_epoch::Byte *input, std::size_t inputSize)
{
	if (input == nullptr || inputSize < runtime_epoch::kNetworkMagic.size())
		return false;
	for (std::size_t index = 0; index < runtime_epoch::kNetworkMagic.size(); ++index)
	{
		if (input[index] != runtime_epoch::kNetworkMagic[index])
			return false;
	}
	return true;
}

inline NetworkHelloKind ReadNetworkHelloKind(const runtime_epoch::Byte *input)
{
	return static_cast<NetworkHelloKind>(ReadLittleEndian32(input + kNetworkHelloKindOffset));
}

inline std::uint64_t ReadNetworkHelloSessionToken(const runtime_epoch::Byte *input)
{
	return ReadLittleEndian64(input + kNetworkHelloSessionTokenOffset);
}

inline bool IsKnownNetworkHelloKind(NetworkHelloKind kind)
{
	return kind == NetworkHelloKind::Hello || kind == NetworkHelloKind::Ack;
}

inline bool IsNetworkHelloSessionTokenAccepted(NetworkHelloKind kind,
	std::uint64_t localSessionToken,
	std::uint64_t receivedSessionToken)
{
	if (receivedSessionToken == 0U)
		return false;
	if (kind == NetworkHelloKind::Hello)
		return true;
	return kind == NetworkHelloKind::Ack && localSessionToken != 0U &&
		receivedSessionToken == localSessionToken;
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
	std::uint32_t iniCrc,
	const runtime_epoch::Byte *payload)
{
	runtime_epoch::NetworkHello hello;
	hello.buildCompatibilityId = runtime_epoch::BuildCompatibilityIdFromExecutableCrc(executableCrc);
	hello.contentHash = runtime_epoch::ContentHashFromIniCrc(iniCrc);
	hello.payloadByteCount = kNetworkHelloPayloadSize;
	hello.payloadChecksum = runtime_epoch::CalculatePayloadChecksum(
		payload, kNetworkHelloPayloadSize);
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
	options.maxPayloadByteCount = kNetworkHelloPayloadSize;
	options.requireBuildCompatibilityMatch = true;
	options.requireContentHashMatch = true;
	return options;
}

inline std::array<runtime_epoch::Byte, kNetworkHelloWireSize> EncodeNetworkHello(
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	std::uint32_t senderSlot,
	std::uint32_t recipientSlot,
	std::uint64_t sessionToken,
	NetworkHelloKind kind = NetworkHelloKind::Hello)
{
	std::array<runtime_epoch::Byte, kNetworkHelloWireSize> output = {{}};
	WriteLittleEndian32(output.data() + kNetworkHelloKindOffset,
		static_cast<std::uint32_t>(kind));
	WriteLittleEndian32(output.data() + kNetworkHelloSenderSlotOffset, senderSlot);
	WriteLittleEndian32(output.data() + kNetworkHelloRecipientSlotOffset, recipientSlot);
	WriteLittleEndian64(output.data() + kNetworkHelloSessionTokenOffset, sessionToken);
	const std::array<runtime_epoch::Byte, runtime_epoch::kHeaderSize> header =
		runtime_epoch::Encode(MakeNetworkHello(executableCrc, iniCrc,
			output.data() + kNetworkHelloKindOffset));
	for (std::size_t index = 0; index < header.size(); ++index)
		output[index] = header[index];
	return output;
}

inline runtime_epoch::ValidationResult DecodeAndValidateNetworkHelloRecord(
	const runtime_epoch::Byte *input,
	std::size_t inputSize,
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	runtime_epoch::NetworkHello *hello,
	NetworkHelloKind *kind,
	NetworkHelloIdentity *identity,
	std::uint64_t *sessionToken)
{
	if (input == nullptr || inputSize != kNetworkHelloWireSize || hello == nullptr ||
		kind == nullptr || identity == nullptr || sessionToken == nullptr)
	{
		return {runtime_epoch::ValidationError::InvalidSize};
	}
	if (!HasNetworkHelloMagic(input, inputSize))
		return {runtime_epoch::ValidationError::InvalidMagic};

	const runtime_epoch::ValidationOptions options =
		MakeNetworkHelloValidationOptions(executableCrc, iniCrc);
	const runtime_epoch::ValidationResult headerResult =
		runtime_epoch::DecodeAndValidate(input, runtime_epoch::kHeaderSize,
			input + kNetworkHelloKindOffset, kNetworkHelloPayloadSize, options, hello);
	if (!headerResult.ok())
		return headerResult;

	const NetworkHelloKind decodedKind = ReadNetworkHelloKind(input);
	if (!IsKnownNetworkHelloKind(decodedKind))
		return {runtime_epoch::ValidationError::NetworkKindMismatch};

	NetworkHelloIdentity decodedIdentity;
	decodedIdentity.senderSlot = ReadLittleEndian32(input + kNetworkHelloSenderSlotOffset);
	decodedIdentity.recipientSlot = ReadLittleEndian32(input + kNetworkHelloRecipientSlotOffset);
	const std::uint64_t decodedSessionToken = ReadNetworkHelloSessionToken(input);
	if (decodedSessionToken == 0U)
		return {runtime_epoch::ValidationError::NetworkSessionMismatch};

	*kind = decodedKind;
	*identity = decodedIdentity;
	*sessionToken = decodedSessionToken;
	return {};
}

inline runtime_epoch::ValidationResult DecodeAndValidateNetworkHello(
	const runtime_epoch::Byte *input,
	std::size_t inputSize,
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	std::uint32_t expectedSenderSlot,
	std::uint32_t expectedRecipientSlot,
	NetworkHelloKind expectedKind,
	std::uint64_t expectedSessionToken,
	runtime_epoch::NetworkHello *hello,
	NetworkHelloIdentity *identity = nullptr)
{
	NetworkHelloKind actualKind;
	NetworkHelloIdentity decodedIdentity;
	std::uint64_t actualSessionToken = 0U;
	const runtime_epoch::ValidationResult recordResult =
		DecodeAndValidateNetworkHelloRecord(input, inputSize, executableCrc, iniCrc,
			hello, &actualKind, &decodedIdentity, &actualSessionToken);
	if (!recordResult.ok())
		return recordResult;
	if (actualKind != expectedKind)
	{
		return {runtime_epoch::ValidationError::NetworkKindMismatch};
	}
	if (decodedIdentity.senderSlot != expectedSenderSlot ||
		decodedIdentity.recipientSlot != expectedRecipientSlot)
	{
		return {runtime_epoch::ValidationError::NetworkIdentityMismatch};
	}
	if (expectedSessionToken != kAnyNetworkHelloSessionToken &&
		actualSessionToken != expectedSessionToken)
	{
		return {runtime_epoch::ValidationError::NetworkSessionMismatch};
	}
	if (identity != nullptr)
		*identity = decodedIdentity;
	return {};
}

// Hello-only overload for focused contract tests and other boundary users.
// Session freshness remains explicit and cannot be omitted by callers.
inline runtime_epoch::ValidationResult DecodeAndValidateNetworkHello(
	const runtime_epoch::Byte *input,
	std::size_t inputSize,
	std::uint32_t executableCrc,
	std::uint32_t iniCrc,
	std::uint32_t expectedSenderSlot,
	std::uint32_t expectedRecipientSlot,
	std::uint64_t expectedSessionToken,
	runtime_epoch::NetworkHello *hello,
	NetworkHelloIdentity *identity = nullptr)
{
	return DecodeAndValidateNetworkHello(input, inputSize, executableCrc, iniCrc,
		expectedSenderSlot, expectedRecipientSlot, NetworkHelloKind::Hello,
		expectedSessionToken, hello, identity);
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
	if (inputSize != kNetworkHelloWireSize)
		return false;
	return HasNetworkHelloPrefix(input, inputSize);
}

} // namespace network_epoch
} // namespace rts
