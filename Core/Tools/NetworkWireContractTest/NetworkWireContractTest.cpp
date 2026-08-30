#include "Lib/NetworkWireContract.h"
#include "Lib/NetworkEpochHandshake.h"
#include "Lib/NetworkNatPolicy.h"

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
	constexpr std::uint64_t sessionToken = 0x0123456789abcdefULL;
	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> encoded =
		EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U, sessionToken);

	int result = 0;
	result |= Check(kNetworkHelloWireSize == 60U, "NET3 hello uses the fixed 60-byte wire size");
	result |= Check(HasNetworkHelloMagic(encoded.data(), encoded.size()),
		"NET3 hello carries its independent wire magic");
	result |= Check(!HasNetworkHelloMagic(encoded.data(), encoded.size() - 1U),
		"NET3 classification requires the exact wire size");
	result |= Check(!HasNetworkHelloMagic(encoded.data(), 4U),
		"a normal payload prefix is not classified as NET3");
	result |= Check(HasNetworkHelloPrefix(encoded.data(), 4U),
		"NET3 prefix classification quarantines unsupported record sizes");
	std::array<rts::runtime_epoch::Byte, 52U> obsoleteRecord = {{}};
	for (std::size_t index = 0; index < obsoleteRecord.size(); ++index)
		obsoleteRecord[index] = encoded[index];
	result |= Check(HasNetworkHelloPrefix(obsoleteRecord.data(), obsoleteRecord.size()) &&
		!HasNetworkHelloMagic(obsoleteRecord.data(), obsoleteRecord.size()),
		"obsolete 52-byte NET3 records cannot enter gameplay packet parsing");
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
	result |= Check(ReadNetworkHelloSessionToken(encoded.data()) == sessionToken,
		"NET3 session token is fixed-width little endian");

	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> ackEncoded =
		EncodeNetworkHello(executableCrc, iniCrc, 5U, 2U, sessionToken, NetworkHelloKind::Ack);
	rts::runtime_epoch::NetworkHello decoded;
	result |= Check(ReadNetworkHelloKind(ackEncoded.data()) == NetworkHelloKind::Ack,
		"NET3 Ack record kind decodes explicitly");
	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> unknownKind =
		EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U, sessionToken,
			static_cast<NetworkHelloKind>(99U));
	NetworkHelloKind decodedUnknownKind = NetworkHelloKind::Hello;
	NetworkHelloIdentity unknownKindIdentity;
	result |= Check(!DecodeNetworkHelloRecord(unknownKind.data(), unknownKind.size(),
		&decodedUnknownKind, &unknownKindIdentity),
		"NET3 unknown record kinds are rejected before dispatch");
	result |= Check(DecodeAndValidateNetworkHello(unknownKind.data(), unknownKind.size(),
		executableCrc, iniCrc, 2U, 5U, static_cast<NetworkHelloKind>(99U), sessionToken, &decoded).error ==
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
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded, &identity).ok(),
		"matching NET3 hello validates");
	result |= Check(DecodeAndValidateNetworkHello(encoded.data(), encoded.size(),
		executableCrc, iniCrc, 2U, 5U, kAnyNetworkHelloSessionToken, &decoded).ok(),
		"a first Hello accepts any nonzero fresh session token");
	result |= Check(identity.senderSlot == 2U && identity.recipientSlot == 5U,
		"NET3 carries stable sender and recipient slot identity");
	result |= Check(IsPlausibleNetworkHelloIdentity(encoded.data(), encoded.size(), 5U, 8U),
		"NET3 candidate identity names the local slot and an active slot range");
	result |= Check(IsMatchingNetworkPeerEndpoint(0x0a000102U, 8088U, 0x0a000102U, 8088U),
		"NET3 accepts the exact post-NAT peer endpoint");
	result |= Check(!IsMatchingNetworkPeerEndpoint(0x0a000103U, 8088U, 0x0a000102U, 8088U),
		"NET3 rejects a foreign source address");
	result |= Check(!IsMatchingNetworkPeerEndpoint(0x0a000102U, 8089U, 0x0a000102U, 8088U),
		"NET3 rejects a foreign source port behind the same address");
	result |= Check(IsNetworkCommandSourceAuthorized(2U, 2U, 0U),
		"ordinary gameplay packets must name their observed peer");
	result |= Check(IsNetworkCommandSourceAuthorized(0U, 5U, 0U),
		"the packet router may relay another player's claimed origin");
	result |= Check(!IsNetworkCommandSourceAuthorized(2U, 3U, 0U),
		"ordinary gameplay packets cannot claim another player's origin");
	result |= Check(decoded.payloadByteCount == kNetworkHelloPayloadSize &&
		decoded.payloadChecksum == CalculatePayloadChecksum(
			encoded.data() + kNetworkHelloKindOffset, kNetworkHelloPayloadSize),
		"NET3 checksum covers kind, identity, and session token payload");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> malformed = encoded;
	malformed[0] = 'X';
	result |= Check(DecodeAndValidateNetworkHello(malformed.data(), malformed.size(),
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded).error == rts::runtime_epoch::ValidationError::InvalidMagic,
		"NET3 wrong magic is rejected");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> wrongBuild = encoded;
	wrongBuild[12] ^= 0x01U;
	result |= Check(DecodeAndValidateNetworkHello(wrongBuild.data(), wrongBuild.size(),
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded).error == rts::runtime_epoch::ValidationError::BuildCompatibilityMismatch,
		"NET3 incompatible build identity is rejected");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> wrongContent = encoded;
	wrongContent[20] ^= 0x01U;
	result |= Check(DecodeAndValidateNetworkHello(wrongContent.data(), wrongContent.size(),
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded).error == rts::runtime_epoch::ValidationError::ContentHashMismatch,
		"NET3 incompatible content identity is rejected");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> wrongIdentity = encoded;
	wrongIdentity[kNetworkHelloSenderSlotOffset] ^= 0x01U;
	result |= Check(DecodeAndValidateNetworkHello(wrongIdentity.data(), wrongIdentity.size(),
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded).error ==
		rts::runtime_epoch::ValidationError::PayloadChecksumMismatch,
		"NET3 rejects identity mutation before trusting the claimed slot");

	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> validWrongIdentity =
		EncodeNetworkHello(executableCrc, iniCrc, 3U, 5U, sessionToken);
	result |= Check(DecodeAndValidateNetworkHello(validWrongIdentity.data(), validWrongIdentity.size(),
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded).error ==
		rts::runtime_epoch::ValidationError::NetworkIdentityMismatch,
		"NET3 sender identity is validated independently of endpoint address");

	result |= Check(DecodeAndValidateNetworkHello(ackEncoded.data(), ackEncoded.size(),
		executableCrc, iniCrc, 5U, 2U, NetworkHelloKind::Ack, sessionToken, &decoded).ok(),
		"matching NET3 Ack validates");
	result |= Check(DecodeAndValidateNetworkHello(ackEncoded.data(), ackEncoded.size(),
		executableCrc, iniCrc, 5U, 2U, NetworkHelloKind::Hello, sessionToken, &decoded).error ==
		rts::runtime_epoch::ValidationError::NetworkKindMismatch,
		"NET3 Hello and Ack kinds cannot be interchanged");
	result |= Check(DecodeAndValidateNetworkHello(ackEncoded.data(), ackEncoded.size(),
		executableCrc, iniCrc, 5U, 2U, NetworkHelloKind::Ack, sessionToken + 1U, &decoded).error ==
		rts::runtime_epoch::ValidationError::NetworkSessionMismatch,
		"NET3 rejects an Ack replayed from a stale session");
	result |= Check(IsNetworkHelloSessionTokenAccepted(NetworkHelloKind::Hello,
		sessionToken, sessionToken + 1U),
		"a current Hello can replace a stale remembered peer token");
	result |= Check(IsNetworkHelloSessionTokenAccepted(NetworkHelloKind::Ack,
		sessionToken, sessionToken) &&
		!IsNetworkHelloSessionTokenAccepted(NetworkHelloKind::Ack,
			sessionToken, sessionToken + 1U),
		"Ack acceptance requires the current local session token");
	result |= Check(EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U, sessionToken) == encoded,
		"Hello retries preserve the same session challenge");

	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> zeroToken =
		EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U, 0U);
	result |= Check(DecodeAndValidateNetworkHello(zeroToken.data(), zeroToken.size(),
		executableCrc, iniCrc, 2U, 5U, kAnyNetworkHelloSessionToken, &decoded).error ==
		rts::runtime_epoch::ValidationError::NetworkSessionMismatch,
		"NET3 rejects an absent session challenge");

	std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> unrelatedPrefix = encoded;
	WriteLittleEndian32(unrelatedPrefix.data() + kNetworkHelloSenderSlotOffset, 99U);
	result |= Check(!IsPlausibleNetworkHelloIdentity(unrelatedPrefix.data(), unrelatedPrefix.size(), 5U, 8U),
		"NET3-sized unrelated payloads are not handshake candidates");

	result |= Check(DecodeAndValidateNetworkHello(encoded.data(), encoded.size() - 1U,
		executableCrc, iniCrc, 2U, 5U, sessionToken, &decoded).error == rts::runtime_epoch::ValidationError::InvalidSize,
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

int TestNetworkFramePublicationGate()
{
	using namespace rts::network_epoch;
	int result = 0;
	result |= Check(IsNetworkFramePublicationAllowed(false, false),
		"network frames remain publishable when NET3 has not failed");
	result |= Check(!IsNetworkFramePublicationAllowed(true, false),
		"a handled NET3 failure blocks same-update frame publication");
	result |= Check(!IsNetworkFramePublicationAllowed(false, true),
		"an observed NET3 failure blocks same-update frame publication");
	return result;
}

int TestNetworkHelloFailureHandlingPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	result |= Check(!ShouldHandleNetworkHelloFailure(false, false),
		"NET3 failure handling stays idle while no failure exists");
	result |= Check(ShouldHandleNetworkHelloFailure(false, true),
		"an unhandled NET3 failure is handled in every network state");
	result |= Check(!ShouldHandleNetworkHelloFailure(true, true),
		"a NET3 failure is handled only once");
	return result;
}

int TestNetworkIngressPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	result |= Check(ClassifyNetworkIngress(true, false, true, true, false) ==
		NetworkIngressDisposition::Quarantine,
		"a malformed NET3-shaped packet from a known peer quarantines only that peer");
	result |= Check(ClassifyNetworkIngress(true, false, true, false, false) ==
		NetworkIngressDisposition::Drop,
		"a malformed NET3-shaped packet from an unknown peer is dropped");
	result |= Check(ClassifyNetworkIngress(true, true, true, true, false) ==
		NetworkIngressDisposition::Quarantine,
		"an invalid exact-size NET3 candidate quarantines its known source");
	result |= Check(ClassifyNetworkIngress(false, false, true, true, false) ==
		NetworkIngressDisposition::Defer,
		"known gameplay traffic is deferred while NET3 is required");
	result |= Check(ClassifyNetworkIngress(false, false, true, false, false) ==
		NetworkIngressDisposition::Drop,
		"unknown gameplay traffic cannot enter the deferred queue");
	result |= Check(ClassifyNetworkIngress(false, false, false, true, false) ==
		NetworkIngressDisposition::Process,
		"known gameplay traffic is admitted after NET3");
	result |= Check(ClassifyNetworkIngress(false, false, false, false, false) ==
		NetworkIngressDisposition::Drop,
		"post-NET3 gameplay traffic still requires a known endpoint");
	result |= Check(ClassifyNetworkIngress(true, true, true, true, true) ==
		NetworkIngressDisposition::Process,
		"a valid NET3 candidate reaches the production handshake path");
	result |= Check(IsNetworkHelloDeferredPeerQuotaExceeded(7U, 64U, 8U) == false &&
		IsNetworkHelloDeferredPeerQuotaExceeded(8U, 64U, 8U),
		"the per-peer deferred quota has a deterministic boundary");
	result |= Check(IsNetworkPacketRouterEligible(2U, 0U, 8U, true, false) &&
		IsNetworkPacketRouterEligible(0U, 0U, 8U, false, false) &&
		!IsNetworkPacketRouterEligible(2U, 0U, 8U, true, true) &&
		!IsNetworkPacketRouterEligible(8U, 0U, 8U, true, false),
		"router replacement admits only local or active connected peers");
	return result;
}

int TestNetworkFrameResendPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	result |= Check(IsNetworkFrameResendResponseAuthorized(
		2U, 2U, 4U, 0x10U, 8U, true, false, true, 120U, 120U),
		"a direct requested responder may resend cached commands from another player for the requested frame");
	result |= Check(IsNetworkFrameResendResponseAuthorized(
		2U, 2U, 5U, 0x20U, 8U, true, false, true, 120U, 120U),
		"a direct requested responder may resend the matching multi-origin frame-info record");
	result |= Check(!IsNetworkFrameResendResponseAuthorized(
		3U, 2U, 4U, 0x10U, 8U, true, false, true, 120U, 120U),
		"a cached resend from a different responder remains rejected");
	result |= Check(!IsNetworkFrameResendResponseAuthorized(
		2U, 2U, 4U, 0x10U, 8U, true, false, true, 121U, 120U),
		"a cached resend for a different frame remains rejected");
	result |= Check(!IsNetworkFrameResendResponseAuthorized(
		2U, 2U, 4U, 0x10U, 8U, true, true, true, 120U, 120U) &&
		!IsNetworkFrameResendResponseAuthorized(
			2U, 2U, 4U, 0x10U, 8U, false, false, true, 120U, 120U),
		"expired or absent resend requests cannot authorize cached provenance");
	result |= Check(!IsNetworkFrameResendResponseAuthorized(
		2U, 2U, 4U, 0x10U, 8U, true, false, false, 120U, 120U) &&
		!IsNetworkFrameResendResponseAuthorized(
			2U, 2U, 8U, 0x100U, 8U, true, false, true, 120U, 120U),
		"unrelated command types and out-of-range claimed origins remain rejected");
	result |= Check(IsNetworkFrameResendResponseAuthorized(
		1U, 1U, 1U, 0x02U, 2U, true, false, true, 300U, 300U),
		"two-player requester zero/responder one accepts the responder's expected origin");
	result |= Check(!IsNetworkFrameResendResponseAuthorized(
		1U, 1U, 0U, 0x02U, 2U, true, false, true, 300U, 300U),
		"two-player resend rejects an origin outside the expected responder mask");
	result |= Check(!IsNetworkFrameResendResponseComplete(0x06U, 0x06U, 0x02U),
		"frame-info arriving before every cached command keeps resend provenance active");
	result |= Check(IsNetworkFrameResendResponseComplete(0x06U, 0x06U, 0x06U),
		"resend provenance clears only after all expected frame data is ready");
	result |= Check(!IsNetworkFrameResendResponseComplete(0x06U, 0x02U, 0x06U),
		"missing frame-info keeps resend provenance active even when commands are ready");
	result |= Check(IsNetworkFrameResendResponseComplete(0U, 0U, 0U),
		"a resend with no expected origins is complete");
	return result;
}

int TestNetworkNatPolicy()
{
	using namespace rts::network_nat;
	int result = 0;
	result |= Check(IsValidNode(1, 3U, 8U) && !IsValidNode(-1, 3U, 8U) &&
		!IsValidNode(3, 3U, 8U) && !IsValidNode(8, 9U, 8U),
		"NAT node validation rejects negative, count, and array-bound values");
	result |= Check(IsValidConnectionPairState(0, 0U, 7U) &&
		!IsValidConnectionPairState(-1, 0U, 7U) &&
		!IsValidConnectionPairState(0, 7U, 7U),
		"NAT connection-pair state validation precedes pair indexing");
	result |= Check(IsValidControlSource(2, 8U) &&
		!IsValidControlSource(-1, 8U) && !IsValidControlSource(8, 8U),
		"room and out-of-range NAT UTM sources are rejected");
	result |= Check(IsValidNatPort(1024U) && IsValidNatPort(65535U) &&
		!IsValidNatPort(1023U),
		"NAT port parsing has a bounded valid range");
	#if defined(_WIN64)
	result |= Check(IsExpectedProbeSource(2, 2, 0xA5A5A5A5U, 0xA5A5A5A5U,
		0x0a000103U, 3U, 8U),
		"a validated target identity and cookie permit learning a remapped public address");
	result |= Check(IsExpectedProbeSource(2, 2, 0xA5A5A5A5U, 0xA5A5A5A5U,
		0x0a000102U, 3U, 8U),
		"a probe from the current expected target address remains valid");
	result |= Check(!IsExpectedProbeSource(2, 2, 0xA5A5A5A5U, 0x5A5A5A5AU,
		0x0a000103U, 3U, 8U) &&
		!IsExpectedProbeSource(2, 2, 0xA5A5A5A5U, 0U, 0x0a000103U, 3U, 8U) &&
		!IsExpectedProbeSource(-1, 2, 0xA5A5A5A5U, 0xA5A5A5A5U,
			0x0a000103U, 3U, 8U) &&
		!IsExpectedProbeSource(2, 1, 0xA5A5A5A5U, 0xA5A5A5A5U,
			0x0a000103U, 3U, 8U) &&
		!IsExpectedProbeSource(2, 3, 0xA5A5A5A5U, 0xA5A5A5A5U,
			0x0a000103U, 3U, 8U) &&
		!IsExpectedProbeSource(8, 8, 0xA5A5A5A5U, 0xA5A5A5A5U,
			0x0a000103U, 9U, 8U) &&
		!IsExpectedProbeSource(2, 2, 0xA5A5A5A5U, 0xA5A5A5A5U,
			0U, 3U, 8U),
		"wrong-cookie, missing-cookie, wrong-node, out-of-range, and malformed NAT probes are rejected");
	result |= Check(IsNewerProbeGeneration(1U, 0U) &&
		IsNewerProbeGeneration(2U, 1U),
		"the first probe and a newer remapped endpoint advance freshness");
	result |= Check(!IsNewerProbeGeneration(1U, 2U) &&
		!IsNewerProbeGeneration(2U, 2U),
		"delayed and duplicate remap probes cannot roll the endpoint backward");
	result |= Check(IsNewerProbeGeneration(1U, 0xffffffffU) &&
		!IsNewerProbeGeneration(0U, 0xffffffffU) &&
		!IsNewerProbeGeneration(0x80000000U, 0U),
		"probe freshness handles wrap-around and reserves zero and the half-range");
	result |= Check(!IsNewProbeEpoch(2, 0xA5A5A5A5U, 2, 0xA5A5A5A5U) &&
		IsNewProbeEpoch(2, 0xA5A5A5A5U, 2, 0x5A5A5A5AU) &&
		IsNewProbeEpoch(-1, 0U, 2, 0xA5A5A5A5U),
		"retries preserve freshness while a new cookie or initial map epoch resets it");
	#endif
	return result;
}

} // namespace

int main()
{
	return TestFixedSizes() | TestSizeConversion() | TestWrapperCapacity() |
		TestNetworkHelloContract() | TestNetworkFramePublicationGate() |
		TestNetworkHelloFailureHandlingPolicy() | TestNetworkIngressPolicy() |
		TestNetworkFrameResendPolicy() | TestNetworkNatPolicy();
}
