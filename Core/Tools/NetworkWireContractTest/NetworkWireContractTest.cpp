#include "Lib/NetworkWireContract.h"
#include "Lib/NetworkCommandOriginPolicy.h"
#include "Lib/NetworkEpochHandshake.h"
#include "Lib/MultiplayerSimulationRuntimeProof.h"
#include "Lib/NetworkNatPolicy.h"

#include <array>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
#endif

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

int TestWrappedCommandOriginPolicy()
{
	int result = 0;
	result |= Check(rts::IsWrappedNetworkCommandOriginAuthorized(2U, 2U, 8U),
		"a direct wrapper preserves its endpoint-bound claimed origin");
	result |= Check(rts::IsWrappedNetworkCommandOriginAuthorized(5U, 5U, 8U),
		"a trusted-router wrapper preserves its claimed command origin");
	result |= Check(rts::IsWrappedNetworkCommandOriginAuthorized(3U, 3U, 8U),
		"a frame-recovery wrapper preserves its claimed command origin");
	result |= Check(!rts::IsWrappedNetworkCommandOriginAuthorized(2U, 3U, 8U),
		"a decoded wrapper cannot impersonate another player slot");
	result |= Check(!rts::IsWrappedNetworkCommandOriginAuthorized(8U, 8U, 8U) &&
		!rts::IsWrappedNetworkCommandOriginAuthorized(2U, 8U, 8U),
		"wrapper and decoded origins must both name active slot indices");
	return result;
}

int TestExternalRuntimeReleaseProof()
{
	const std::string executableSha(64, 'A');
	const std::string artifactSha(64, 'B');
	const std::string evidenceSha(64, 'C');
	const std::string rawIndexSha(64, 'D');
	const std::string sourceRevision(40, 'a');
	const unsigned trustedPromotedMask = static_cast<unsigned>(
		rts::MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK);
	const std::string document =
		"RTS_MULTIPLAYER_SIMULATION_RUNTIME_PROOF_V1\n"
		"schema=1\n"
		"title=Generals\n"
		"source_revision=" + sourceRevision + "\n" +
		"executable_sha256=" + executableSha + "\n" +
		"artifact_set_sha256=" + artifactSha + "\n" +
		"evidence_manifest_sha256=" + evidenceSha + "\n"
		"raw_evidence_index_sha256=" + rawIndexSha + "\n"
		"policy_schema=1\n"
		"engine_epoch=1\n"
		"determinism_epoch=1\n"
		"build_compatibility_crc=287454020\n"
		"content_crc=2864434397\n"
		"proven_kernel_mask=63\n"
		"match_count=16\n"
		"peer_process_count=40\n"
		"producer=installed-runtime-runner-v1\n"
		"validation_mode=scoped-net3-loopback-release-proof\n"
		"END\n";
	rts::MultiplayerSimulationRuntimeProof proof;
	int result = 0;
	result |= Check(rts::ParseMultiplayerSimulationRuntimeProof(
		document.data(), document.size(), proof),
		"canonical external runtime proof parses without source generation");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"Generals", executableSha.c_str(), 0x11223344U, 0xaabbccddU,
		trustedPromotedMask, trustedPromotedMask, sourceRevision.c_str()) == 0U,
		"InstalledNet3Validation v1 remains diagnostic even with a nonzero caller mask");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"Generals", executableSha.c_str(), 0x11223344U, 0xaabbccddU,
		trustedPromotedMask, 0U, "") == 0U,
		"a forged sibling proof cannot elevate default-zero build authority");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"ZeroHour", executableSha.c_str(), 0x11223344U, 0xaabbccddU,
		trustedPromotedMask, trustedPromotedMask, sourceRevision.c_str()) == 0U,
		"another title cannot reuse an external runtime proof");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"Generals", std::string(64, 'D').c_str(), 0x11223344U, 0xaabbccddU,
		trustedPromotedMask, trustedPromotedMask, sourceRevision.c_str()) == 0U,
		"a rebuilt or substituted executable cannot reuse an external runtime proof");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"Generals", executableSha.c_str(), 0x11223345U, 0xaabbccddU,
		trustedPromotedMask, trustedPromotedMask, sourceRevision.c_str()) == 0U,
		"a different build identity cannot reuse an external runtime proof");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"Generals", executableSha.c_str(), 0x11223344U, 0xaabbccdeU,
		trustedPromotedMask, trustedPromotedMask, sourceRevision.c_str()) == 0U,
		"different installed content cannot reuse an external runtime proof");
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		"Generals", executableSha.c_str(), 0x11223344U, 0xaabbccddU,
		trustedPromotedMask, trustedPromotedMask,
		std::string(40, 'b').c_str()) == 0U,
		"runtime evidence from another source revision cannot confirm build authority");
	rts::MultiplayerSimulationRuntimeProof partialProof = proof;
	partialProof.provenKernelMask &= ~static_cast<unsigned>(
		rts::MULTIPLAYER_SIMULATION_KERNEL_COLLISION);
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(
		partialProof, "Generals", executableSha.c_str(), 0x11223344U,
		0xaabbccddU, trustedPromotedMask, trustedPromotedMask,
		sourceRevision.c_str()) == 0U,
		"a partial proof cannot enable a partially validated product policy");
	rts::MultiplayerSimulationRuntimeProof manuallyAuthored = proof;
	manuallyAuthored.producer = "manual";
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(
		manuallyAuthored, "Generals", executableSha.c_str(), 0x11223344U,
		0xaabbccddU, trustedPromotedMask, trustedPromotedMask,
		sourceRevision.c_str()) == 0U,
		"a manually labeled proof has no release authority");
	manuallyAuthored = proof;
	manuallyAuthored.validationMode = "ordinary-gameplay";
	result |= Check(rts::ResolveMultiplayerSimulationRuntimeProofMask(
		manuallyAuthored, "Generals", executableSha.c_str(), 0x11223344U,
		0xaabbccddU, trustedPromotedMask, trustedPromotedMask,
		sourceRevision.c_str()) == 0U,
		"ordinary gameplay cannot self-certify a multiplayer release proof");
	return result;
}

int TestNetworkHelloContract()
{
	using namespace rts::network_epoch;
	using namespace rts::runtime_epoch;

	constexpr std::uint32_t executableCrc = 0x11223344U;
	constexpr std::uint32_t iniCrc = 0xaabbccddU;
	constexpr std::uint64_t sessionToken = 0x0123456789abcdefULL;
	const unsigned liveIntegratedMask = static_cast<unsigned>(
		rts::MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK);
	const unsigned nonProductTestMask =
		rts::SelectMultiplayerSimulationNonProductTestOverrideMask(
			liveIntegratedMask, liveIntegratedMask);
	const NetworkSimulationPolicyIdentity simulationPolicy =
		MakeNetworkSimulationPolicyIdentity(executableCrc, iniCrc,
			0x10203040U, 0xa5U, nonProductTestMask);
	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize> encoded =
		EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U, sessionToken,
			NetworkHelloKind::Hello, simulationPolicy);

	int result = 0;
	const rts::MultiplayerSimulationGeneratedReleaseProof absentProof =
		{ 0, "", "", "", "", "", liveIntegratedMask };
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			absentProof, liveIntegratedMask) == 0,
		"default product transport advertises no merely implemented kernel");
	result |= Check(nonProductTestMask == liveIntegratedMask,
		"wire fixture uses an explicit non-product release-proof override");
	result |= Check(kNetworkHelloWireSize == 80U, "NET3 policy hello uses the fixed 80-byte wire size");
	result |= Check(HasNetworkHelloMagic(encoded.data(), encoded.size()),
		"NET3 hello carries its independent wire magic");
	result |= Check(!HasNetworkHelloMagic(encoded.data(), encoded.size() - 1U),
		"NET3 classification requires the exact wire size");
	result |= Check(!HasNetworkHelloMagic(encoded.data(), 4U),
		"a normal payload prefix is not classified as NET3");
	result |= Check(HasNetworkHelloPrefix(encoded.data(), 4U),
		"NET3 prefix classification routes unsupported record sizes to drop handling");
	std::array<rts::runtime_epoch::Byte, 52U> obsoleteRecord = {{}};
	for (std::size_t index = 0; index < obsoleteRecord.size(); ++index)
		obsoleteRecord[index] = encoded[index];
	result |= Check(HasNetworkHelloPrefix(obsoleteRecord.data(), obsoleteRecord.size()) &&
		!HasNetworkHelloMagic(obsoleteRecord.data(), obsoleteRecord.size()),
		"obsolete 52-byte NET3 records cannot enter gameplay packet parsing");
	std::array<rts::runtime_epoch::Byte, 60U> prePolicyRecord = {{}};
	for (std::size_t index = 0; index < prePolicyRecord.size(); ++index)
		prePolicyRecord[index] = encoded[index];
	result |= Check(HasNetworkHelloPrefix(prePolicyRecord.data(),
		prePolicyRecord.size()) &&
		!HasNetworkHelloMagic(prePolicyRecord.data(), prePolicyRecord.size()),
		"pre-policy 60-byte NET3 records cannot enter gameplay packet parsing");
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
	result |= Check(ReadLittleEndian32(encoded.data() +
		kNetworkHelloPolicySchemaOffset) ==
		static_cast<std::uint32_t>(
			rts::MULTIPLAYER_SIMULATION_POLICY_SCHEMA) &&
		ReadLittleEndian32(encoded.data() + kNetworkHelloMapCrcOffset) ==
			0x10203040U &&
		ReadLittleEndian32(encoded.data() + kNetworkHelloRosterMaskOffset) ==
			0xa5U,
		"NET3 carries the fixed-width policy schema, map, and exact roster");
	result |= Check(IsNetworkSimulationRosterIdentityValid(0xa5U, 0x85U,
		5U, 8U) &&
		!IsNetworkSimulationRosterIdentityValid(0xa4U, 0x85U, 5U, 8U) &&
		!IsNetworkSimulationRosterIdentityValid(0xa5U, 0xa5U, 5U, 8U),
		"NET3 policy identity is bound to the exact local-plus-remote roster");

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
	NetworkSimulationPolicyIdentity decodedSimulationPolicy;
	result |= Check(DecodeNetworkHelloRecord(ackEncoded.data(), ackEncoded.size(),
		&decodedKind, &decodedRecordIdentity) && decodedKind == NetworkHelloKind::Ack &&
		decodedRecordIdentity.senderSlot == 5U && decodedRecordIdentity.recipientSlot == 2U,
		"NET3 Ack carries the stable sender and recipient identity");
	std::uint64_t decodedSessionToken = 0U;
	result |= Check(DecodeAndValidateNetworkHelloRecord(encoded.data(),
		encoded.size(), executableCrc, iniCrc, &decoded, &decodedKind,
		&decodedRecordIdentity, &decodedSessionToken,
		&decodedSimulationPolicy).ok() &&
		decodedSimulationPolicy.schema ==
			rts::MULTIPLAYER_SIMULATION_POLICY_SCHEMA &&
		decodedSimulationPolicy.engineEpoch ==
			rts::runtime_epoch::kCurrentEngineEpoch &&
		decodedSimulationPolicy.determinismEpoch ==
			rts::MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH &&
		decodedSimulationPolicy.buildCompatibilityCrc == executableCrc &&
		decodedSimulationPolicy.contentCrc == iniCrc &&
		decodedSimulationPolicy.mapCrc == 0x10203040U &&
		decodedSimulationPolicy.rosterMask == 0xa5U &&
		decodedSimulationPolicy.provenKernelMask ==
			nonProductTestMask,
		"NET3 decoder reconstructs the authoritative simulation policy identity");
	const NetworkSimulationPolicyIdentity defaultProductPolicy =
		MakeNetworkSimulationPolicyIdentity(executableCrc, iniCrc,
			0x10203040U, 0xa5U,
			rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
				absentProof, liveIntegratedMask));
	const std::array<rts::runtime_epoch::Byte, kNetworkHelloWireSize>
		defaultProductEncoded = EncodeNetworkHello(executableCrc, iniCrc,
			2U, 5U, sessionToken, NetworkHelloKind::Hello,
			defaultProductPolicy);
	NetworkSimulationPolicyIdentity decodedDefaultProductPolicy;
	result |= Check(DecodeAndValidateNetworkHelloRecord(
		defaultProductEncoded.data(), defaultProductEncoded.size(),
		executableCrc, iniCrc, &decoded, &decodedKind,
		&decodedRecordIdentity, &decodedSessionToken,
		&decodedDefaultProductPolicy).ok() &&
		decodedDefaultProductPolicy.provenKernelMask == 0,
		"compatible default peers negotiate serial without release evidence");

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
		"NET3 checksum covers kind, identity, session token, and policy payload");

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
	result |= Check(EncodeNetworkHello(executableCrc, iniCrc, 2U, 5U,
		sessionToken, NetworkHelloKind::Hello, simulationPolicy) == encoded,
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
		"a malformed NET3-shaped packet from a known peer reaches the drop-only handler");
	result |= Check(ClassifyNetworkIngress(true, false, true, false, false) ==
		NetworkIngressDisposition::Drop,
		"a malformed NET3-shaped packet from an unknown peer is dropped");
	result |= Check(ClassifyNetworkIngress(true, true, true, true, false) ==
		NetworkIngressDisposition::Quarantine,
		"an invalid exact-size NET3 candidate reaches the drop-only handler");
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

int TestNetworkHelloDropPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	const std::uint32_t expectedPeerMask = 0x06U;
	std::uint32_t validatedPeerMask = 0x02U;
	const std::uint32_t acknowledgedPeerMask = 0x06U;

	result |= Check(ClassifyNetworkIngress(true, true, true, true, false) ==
		NetworkIngressDisposition::Quarantine,
		"a known malformed NET3 record is admitted only to the drop-only path");
	result |= Check(!IsNetworkHelloComplete(expectedPeerMask, validatedPeerMask,
			acknowledgedPeerMask),
		"the invalid NET3 policy keeps the expected-peer gate closed until validation is complete");

	// A valid retry updates only the validation mask; the original expected set
	// remains authoritative for completion.
	validatedPeerMask |= 0x04U;
	result |= Check(IsNetworkHelloComplete(expectedPeerMask, validatedPeerMask,
			acknowledgedPeerMask),
		"the network policy permits a valid Hello retry to complete against the expected set");

	result |= Check(IsNetworkHelloDeferredPeerQuotaExceeded(
		8U, 64U, 8U) &&
		!IsNetworkHelloDeferredPeerQuotaExceeded(7U, 64U, 8U),
		"the deferred quota predicate marks only over-limit packets for drop");
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

int TestNetworkDisconnectFrameRecoveryPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	NetworkDisconnectFrameRecovery peers[8];
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
		4U, 8U, 120U, 65U, true, 120U), "no disconnect proof means no relayed recovery");
	result |= Check(TrySetNetworkDisconnectFrameRecovery(peers[2],
		120U, 125U, 0x14U, 120U, 65U), "accepted ahead-peer progress establishes a bounded cache range");
	for (std::uint32_t frame = 120U; frame < 125U; ++frame)
		result |= Check(IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
			4U, 8U, 120U, 65U, true, frame), "every retained frame in the announced range is recoverable");
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[3],
		4U, 8U, 120U, 65U, true, 120U), "a different observed responder cannot reuse a peer's recovery proof");
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
		0U, 8U, 120U, 65U, true, 120U) &&
		!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
			8U, 8U, 120U, 65U, true, 120U), "recovery excludes the local requester and invalid origins");
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
		4U, 8U, 120U, 65U, true, 119U) &&
		!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
			4U, 8U, 120U, 65U, true, 125U), "recovery never admits frames outside accepted peer progress");
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
		4U, 8U, 121U, 65U, true, 120U) &&
		!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
			4U, 8U, 125U, 65U, true, 125U), "executed frames and completed catch-up invalidate recovery");
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
		4U, 8U, 120U, 65U, false, 120U), "file/control/nested-wrapper payloads are not synchronized recovery");
	result |= Check(!TrySetNetworkDisconnectFrameRecovery(peers[3],
		120U, 186U, 0x14U, 120U, 65U) &&
		!TrySetNetworkDisconnectFrameRecovery(peers[3],
			120U, 125U, 0x14U, 121U, 65U) &&
		!TrySetNetworkDisconnectFrameRecovery(peers[3],
			120U, 120U, 0x14U, 120U, 65U) &&
		!TrySetNetworkDisconnectFrameRecovery(peers[3],
			120U, 125U, 0U, 120U, 65U), "stale local announcements, empty origins and excessive cache spans cannot grant recovery");
	result |= Check(TrySetNetworkDisconnectFrameRecovery(peers[3],
		120U, 185U, 0x14U, 120U, 65U), "the full retained-cache boundary is recoverable");
	peers[2].originMask &= ~0x10U;
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[2],
		4U, 8U, 120U, 65U, true, 120U), "removing an origin revokes its outstanding recovery permission");
	peers[3] = {};
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(peers[3],
		4U, 8U, 120U, 65U, true, 120U), "disconnect and session reset revoke all responder recovery");

	result |= Check(IsNetworkRecoveryWrapperBounded(8192U, 64U) &&
		!IsNetworkRecoveryWrapperBounded(8193U, 64U) &&
		!IsNetworkRecoveryWrapperBounded(8192U, 65U) &&
		!IsNetworkRecoveryWrapperBounded(0U, 1U) &&
		!IsNetworkRecoveryWrapperBounded(1U, 0U), "recovery fragments have independent payload and metadata bounds");
	const std::size_t maxCanonicalGameCommandBytes = 15U + 5U + 255U * (2U + 16U);
	UnsignedInt chunks = 0U;
	result |= Check(rts::network_wire::TryGetWrapperChunkCount(maxCanonicalGameCommandBytes,
		rts::network_wire::kRetailPacketSize, chunks) &&
		IsNetworkRecoveryWrapperBounded(static_cast<std::uint32_t>(maxCanonicalGameCommandBytes), chunks),
		"the recovery bound permits the largest canonical game command in retail-size fragments");
	return result;
}

int TestNetworkFrameRecoveryDeliveryPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	NetworkDisconnectFrameRecovery recovery;
	TrySetNetworkDisconnectFrameRecovery(recovery, 120U, 125U, 0x14U, 120U, 65U);
	for (const std::uint32_t origin : {2U, 4U})
	{
		const bool sourceAuthorized = IsNetworkCommandSourceAuthorized(2U, origin, 1U);
		const bool resend = IsNetworkFrameResendResponseAuthorized(
			2U, 2U, origin, 0x14U, 8U, true, false, true, 120U, 120U);
		const bool catchUp = IsNetworkDisconnectFrameRecoveryAuthorized(
			recovery, origin, 8U, 120U, 65U, true, 120U);
		result |= Check(IsNetworkFrameRecoveryDelivery(resend, 0x01U, 0U, 8U) &&
			IsNetworkFrameRecoveryDelivery(catchUp, 0x01U, 0U, 8U),
			"own-origin and third-party direct recovery both retire the responder's wire queue");
		result |= Check(ShouldStageNetworkFrameWrapper(sourceAuthorized, 0x01U, 0U, 8U, 5000U, 12U),
			"own-origin and third-party wrapped recovery both retain responder-specific staging");
	}
	result |= Check(!IsNetworkFrameRecoveryDelivery(true, 0x09U, 0U, 8U),
		"an ordinary relay-bearing command retains router ACK and forwarding ownership despite a matching proof");
	result |= Check(!ShouldStageNetworkFrameWrapper(true, 0x09U, 0U, 8U, 5000U, 12U),
		"ordinary relay-bearing wrappers retain their normal reassembly path");
	result |= Check(!IsNetworkFrameRecoveryDelivery(false, 0x01U, 0U, 8U) &&
		ShouldStageNetworkFrameWrapper(true, 0x01U, 0U, 8U, 5000U, 12U),
		"ordinary local-only wrappers remain staged without a proof and use ordinary handling after decode");
	result |= Check(!ShouldStageNetworkFrameWrapper(true, 0x01U, 0U, 8U, 8193U, 20U) &&
		!ShouldStageNetworkFrameWrapper(true, 0x01U, 0U, 8U, 5000U, 65U),
		"ordinary large file transfers are not discarded by recovery-only payload or metadata bounds");
	result |= Check(!IsNetworkFrameRecoveryDelivery(false, 0x01U, 0U, 8U) &&
		!IsNetworkFrameRecoveryDelivery(true, 0x01U, 8U, 8U),
		"absent proof and invalid recipient cannot acquire recovery ACK ownership");

	result |= Check(IsNetworkCachedFrameRangeValid(0U, 1U, 65U) &&
		IsNetworkCachedFrameRangeValid(0U, 65U, 65U) &&
		IsNetworkCachedFrameRangeValid(1000000U - 65U, 1000000U, 65U),
		"early frames and the inclusive retained-cache boundary produce at most 65 loop iterations");
	result |= Check(!IsNetworkCachedFrameRangeValid(100U, 100U, 65U) &&
		!IsNetworkCachedFrameRangeValid(101U, 100U, 65U) &&
		!IsNetworkCachedFrameRangeValid(0U, 66U, 65U) &&
		!IsNetworkCachedFrameRangeValid(0U, 1000000U, 65U) &&
		!IsNetworkCachedFrameRangeValid(0U, 0xffffffffU, 65U),
		"equal, future and huge stale ranges are rejected before any per-frame iteration");
	return result;
}

int TestNetworkRecoveryLostAckPolicy()
{
	using namespace rts::network_epoch;
	int result = 0;
	// A requested B's frame 10 with router C. After publication advances A to
	// 11, B's lost-ACK retry must retire at B, not at C. Proof is already gone.
	for (const bool sourceAuthorized : {true, false})
	{
		result |= Check(ShouldAckNetworkDirectFrame(sourceAuthorized, true, 1U, 0U, 8U, 10U, 11U),
			"own-origin and third-party stale direct retries ACK the observed responder after catch-up");
	}
	result |= Check(ShouldAckNetworkDirectFrame(true, true, 1U, 0U, 8U, 10U, 10U) &&
		ShouldAckNetworkDirectFrame(true, true, 1U, 0U, 8U, 11U, 10U),
		"ordinary source-authorized local-only sync keeps direct ACK ownership without a recovery proof");
	result |= Check(!ShouldAckNetworkDirectFrame(false, true, 1U, 0U, 8U, 10U, 10U) &&
		!ShouldAckNetworkDirectFrame(false, true, 1U, 0U, 8U, 11U, 10U),
		"fresh third-party records arriving before progress announcement remain unacknowledged for retry");
	result |= Check(!ShouldAckNetworkDirectFrame(true, true, 9U, 0U, 8U, 10U, 11U) &&
		!ShouldAckNetworkDirectFrame(true, false, 1U, 0U, 8U, 10U, 11U),
		"ordinary relays and non-synchronized control records retain normal ACK routing");
	NetworkDisconnectFrameRecovery revoked;
	result |= Check(!IsNetworkDisconnectFrameRecoveryAuthorized(revoked, 4U, 8U, 11U, 65U, true, 10U),
		"ACK-only stale retry handling does not restore publication permission");

	NetworkWrapperAckHistory receipts;
	const std::array<rts::runtime_epoch::Byte, 4> bytes = {{1U, 2U, 3U, 4U}};
	auto altered = bytes;
	altered[3] ^= 1U;
	const auto key = MakeNetworkWrapperAckKey(2U, 4U, 77U);
	result |= Check(!receipts.matches(key, bytes.data(), bytes.size(), 1000U),
		"unproven fresh wrapper chunks are not ACKed before their progress announcement");
	receipts.remember(key, bytes.data(), bytes.size(), 1000U);
	result |= Check(receipts.matches(key, bytes.data(), bytes.size(), 3000U),
		"an exact accepted wrapper retry remains ACKable after proof expiry and catch-up");
	result |= Check(!receipts.matches(MakeNetworkWrapperAckKey(3U, 4U, 77U), bytes.data(), bytes.size(), 3000U) &&
		!receipts.matches(MakeNetworkWrapperAckKey(2U, 3U, 77U), bytes.data(), bytes.size(), 3000U) &&
		!receipts.matches(MakeNetworkWrapperAckKey(2U, 4U, 78U), bytes.data(), bytes.size(), 3000U) &&
		!receipts.matches(key, altered.data(), altered.size(), 3000U) &&
		!receipts.matches(key, bytes.data(), bytes.size() - 1U, 3000U),
		"wrapper receipts bind endpoint, origin, ID, size and every canonical byte");
	result |= Check(receipts.matches(key, bytes.data(), bytes.size(), 30999U) &&
		!receipts.matches(key, bytes.data(), bytes.size(), 31000U),
		"ACK-only receipt lookup never renews the finite receipt lifetime");
	result |= Check(kNetworkWrapperAckLifetimeMs >= kNetworkRecoveryRetryLifetimeMs &&
		!IsNetworkRecoveryRetryExpired(true, 900U, 30899U) &&
		IsNetworkRecoveryRetryExpired(true, 900U, 30900U) &&
		IsNetworkRecoveryRetryExpired(true, 900U, 31000U) &&
		!IsNetworkRecoveryRetryExpired(false, 900U, 31000U) &&
		!IsNetworkRecoveryRetryExpired(false, 0U, 0xffffffffU),
		"only cached-recovery refs expire from initial enqueue; ordinary reliable refs never do");
	const std::uint32_t beforeWrap = 0xfffffff0U;
	receipts.remember(key, bytes.data(), bytes.size(), beforeWrap);
	result |= Check(receipts.matches(key, bytes.data(), bytes.size(), beforeWrap + 29999U) &&
		!receipts.matches(key, bytes.data(), bytes.size(), beforeWrap + 30000U) &&
		!IsNetworkRecoveryRetryExpired(true, beforeWrap, beforeWrap + 29999U) &&
		IsNetworkRecoveryRetryExpired(true, beforeWrap, beforeWrap + 30000U),
		"receipt and recovery-only retry deadlines are safe across the millisecond clock wrap");

	receipts.clear();
	for (std::uint32_t id = 0; id <= kNetworkWrapperAckHistoryLimit; ++id)
		receipts.remember(MakeNetworkWrapperAckKey(2U, 4U, static_cast<std::uint16_t>(id)),
			bytes.data(), bytes.size(), 1000U);
	result |= Check(receipts.size() == kNetworkWrapperAckHistoryLimit &&
		!receipts.matches(MakeNetworkWrapperAckKey(2U, 4U, 0U), bytes.data(), bytes.size(), 1001U) &&
		IsNetworkRecoveryRetryExpired(true, 900U, 30900U),
		"receipt eviction cannot leave a cached-recovery sender ref retrying forever");
	receipts.remember(MakeNetworkWrapperAckKey(3U, 4U, 0U), bytes.data(), bytes.size(), 1000U);
	receipts.removePeer(2U);
	result |= Check(receipts.size() == 1U &&
		receipts.matches(MakeNetworkWrapperAckKey(3U, 4U, 0U), bytes.data(), bytes.size(), 1001U),
		"peer teardown clears only that endpoint's accepted wrapper receipts");
	receipts.clear();
	result |= Check(receipts.size() == 0U, "session reset clears every wrapper ACK receipt");
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
	NativePortMessage parsedPort = {-1, 0U, 0U, 0U};
	result |= Check(TryParseNativePortMessage(
		" 2\t8088 \t0A000102\tA5A5A5A5 \r\n", &parsedPort) &&
		parsedPort.node == 2 && parsedPort.port == 8088U &&
		parsedPort.address == 0x0A000102U &&
		parsedPort.probeCookie == 0xA5A5A5A5U &&
		IsValidNatAddress(parsedPort.address),
		"native NAT PORT fields accept bounded inter-token whitespace");
	result |= Check(!TryParseNativePortMessage(
		"1 1024abcd ff", &parsedPort),
		"native NAT PORT rejects adjacent numeric fields without delimiters");
	result |= Check(TryParseNativePortMessage(
		"2 8088 00000000 A5A5A5A5", &parsedPort) &&
		!IsValidNatAddress(parsedPort.address),
		"native NAT PORT rejects a zero internal address before state mutation");
	result |= Check(!TryParseNativePortMessage(
		"2 4294967296 0A000102 A5A5A5A5", &parsedPort) &&
		!TryParseNativePortMessage(
			"2 8088 100000000 A5A5A5A5", &parsedPort),
		"native NAT PORT rejects decimal and hexadecimal numeric overflow");
	result |= Check(!TryParseNativePortMessage(
		"2 8088 0A000102 A5A5A5A5junk", &parsedPort),
		"native NAT PORT rejects trailing non-whitespace junk");
	result |= Check(TryParseNativePortMessage(
		"2 65536 0A000102 A5A5A5A5", &parsedPort) &&
		!IsValidNatPort(parsedPort.port),
		"native NAT PORT keeps the semantic port range check after parsing");
	std::array<char, kNativePortMessageMaxLength + 1U> overlongPort = {{}};
	for (std::size_t index = 0; index + 1U < overlongPort.size(); ++index)
		overlongPort[index] = '1';
	result |= Check(!TryParseNativePortMessage(overlongPort.data(), &parsedPort),
		"native NAT PORT rejects input beyond its bounded option length");
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

template <std::size_t N>
void FillCanonicalText(std::array<char, N> &value, char fill)
{
	for (std::size_t index = 0U; index + 1U < N; ++index)
		value[index] = fill;
	value[N - 1U] = '\0';
}

int TestLockstepV2ReceiptContract()
{
	using namespace rts::lockstep_v2;
	SessionContract session;
	session.localSlot = 0U;
	session.peerCount = 2U;
	session.rosterMask = 0x3U;
	session.simulationRosterMask = 0x3U;
	session.aiRosterMask = 0U;
	session.buildCompatibilityCrc = 0x11223344U;
	session.contentCrc = 0xaabbccddU;
	session.mapCrc = 0x55667788U;
	session.provenKernelMask = 0x3fU;
	FillCanonicalText(session.runNonce, 'A');
	FillCanonicalText(session.sessionNonce, 'B');
	FillCanonicalText(session.executableSha256, 'C');
	FillCanonicalText(session.sourceRevision, 'D');

	int result = 0;
	result |= Check(IsValidSessionContract(session),
		"lockstep-v2 session requires exact roster, hashes, nonces, and frame bound");
	SessionContract mixedSession = session;
	mixedSession.simulationRosterMask =
		kQualificationSimulationRosterMask;
	mixedSession.aiRosterMask = kQualificationAIRosterMask;
	result |= Check(IsValidSessionContract(mixedSession),
		"lockstep-v2 keeps two network humans distinct from four local AI slots");
	SessionContract overlappingRoles = mixedSession;
	overlappingRoles.aiRosterMask |= 0x1U;
	result |= Check(!IsValidSessionContract(overlappingRoles),
		"lockstep-v2 rejects an AI role that overlaps the network roster");
	SessionContract missingSimulationSlot = mixedSession;
	missingSimulationSlot.simulationRosterMask &= ~0x20U;
	result |= Check(!IsValidSessionContract(missingSimulationSlot),
		"lockstep-v2 rejects a simulation roster that omits an AI role");
	SessionContract zeroIdentity = session;
	FillCanonicalText(zeroIdentity.executableSha256, '0');
	result |= Check(!IsValidSessionContract(zeroIdentity),
		"lockstep-v2 rejects an all-zero executable identity");
	SessionContract invalidRouter = session;
	invalidRouter.originMode = CommandOriginMode::TrustedRouter;
	invalidRouter.packetRouterSlot = 7U;
	result |= Check(!IsValidSessionContract(invalidRouter),
		"trusted-router sessions must bind the router to the active roster");
	result |= Check(!rts::IsLockstepV2CommandSourceAuthorized(
		0U, 1U, 0U, CommandOriginMode::DirectAuthenticated, 8U),
		"direct lockstep-v2 rejects a router claiming another origin");
	result |= Check(rts::IsLockstepV2CommandSourceAuthorized(
		0U, 1U, 0U, CommandOriginMode::TrustedRouter, 8U),
		"trusted-router mode is an explicit opt-in boundary");

	ReceiptRecorder recorder;
	const char *executableSha256 = session.executableSha256.data();
	WorkerTelemetry workerTelemetry;
	workerTelemetry.authorityMask = session.provenKernelMask;
	workerTelemetry.executableOrigin = true;
	for (std::uint32_t kernel = 0U; kernel < kKernelCount; ++kernel)
	{
		workerTelemetry.kernels[kernel].physicalWorkerMask = 0x3U;
		workerTelemetry.kernels[kernel].physicalWorkerJobs = 64U;
		workerTelemetry.kernels[kernel].distinctPhysicalWorkers = 2U;
		workerTelemetry.kernels[kernel].peakConcurrentPhysicalWorkers = 2U;
		workerTelemetry.kernels[kernel].physicalWorkerMaskComplete = true;
	}
	AIPlanningTelemetry aiPlanning;
	aiPlanning.capturedSnapshots = 4U;
	aiPlanning.capturedCandidates = 16U;
	aiPlanning.requestedBatches = 2U;
	aiPlanning.submittedJobs = 8U;
	aiPlanning.completedJobs = 8U;
	aiPlanning.canonicalValidationInvocations = 2U;
	aiPlanning.committedBatches = 2U;
	aiPlanning.parallelAuthoritativeCommits = 2U;
	aiPlanning.physicalWorkerExecutions = 8U;
	aiPlanning.observedPhysicalWorkerMask = 0x3U;
	aiPlanning.maximumDistinctPhysicalWorkers = 2U;
	aiPlanning.maximumConcurrentPhysicalWorkers = 2U;
	aiPlanning.planningDigest = ComputeAIPlanningDigest(
		mixedSession.simulationRosterMask, mixedSession.aiRosterMask, aiPlanning);
	result |= Check(IsValidAIPlanningTelemetry(mixedSession, aiPlanning),
		"lockstep-v2 AI telemetry requires an executable-origin parallel commit digest");
	AIPlanningTelemetry alteredAIPlanning = aiPlanning;
	++alteredAIPlanning.planningDigest;
	result |= Check(!IsValidAIPlanningTelemetry(mixedSession, alteredAIPlanning),
		"lockstep-v2 rejects an altered AI planning digest");
	result |= Check(recorder.begin(session, 0x0102030405060708ULL,
		executableSha256, workerTelemetry),
		"receipt recorder binds executable, session, and worker telemetry");
	const rts::runtime_epoch::Byte localBytes[] = {0x11U, 0x22U, 0x33U};
	const rts::runtime_epoch::Byte remoteBytes[] = {0x44U, 0x55U, 0x66U};
	const std::uint64_t localDigest = ComputeCommandDigest(localBytes, sizeof(localBytes));
	const std::uint64_t remoteDigest = ComputeCommandDigest(remoteBytes, sizeof(remoteBytes));
	result |= Check(recorder.recordCommand(1U, 0U, 10U, localDigest),
		"local production command contributes to the v2 receipt");
	result |= Check(recorder.recordCommand(1U, 1U, 20U, remoteDigest),
		"remote production command contributes to the v2 receipt");
	result |= Check(recorder.recordCommand(1U, 1U, 20U, remoteDigest),
		"an identical retransmit is deduplicated by command ID and digest");
	for (std::uint32_t frame = 1U; frame <= kCommonStopFrame; ++frame)
	{
		if (!recorder.recordFrame(frame, frame ^ 0xa5a5a5a5U, 0U))
		{
			result |= Check(false, "receipt recorder accepts only sequential gameplay frames");
			break;
		}
	}
	result |= Check(recorder.finish(true, true, true),
		"complete v2 receipt requires transport, handshake, and clean shutdown");
	const Receipt &receipt = recorder.receipt();
	result |= Check(ValidateReceipt(receipt, session, 0x0102030405060708ULL,
		0x3U, false, session.provenKernelMask).ok(),
		"complete v2 receipt validates against its executable authority");
	result |= Check(ResolveValidatedKernelMask(receipt, session,
		0x0102030405060708ULL, 0x3U, false, 0x3fU,
		session.provenKernelMask) == 0x3fU,
		"only a validated executable-origin telemetry receipt resolves authority");
	Receipt missingWorkerEvidence = receipt;
	missingWorkerEvidence.workerTelemetry[0].physicalWorkerMask = 0U;
	result |= Check(ValidateReceipt(missingWorkerEvidence, session,
		0x0102030405060708ULL, 0x3U, false,
		session.provenKernelMask).error == ValidationError::AuthorityNotProven,
		"a claimed kernel without physical-worker evidence cannot grant authority");
	WorkerTelemetry mixedWorkerTelemetry = workerTelemetry;
	mixedWorkerTelemetry.aiPlanning = aiPlanning;
	ReceiptRecorder mixedRecorder;
	result |= Check(mixedRecorder.begin(mixedSession, 0x1112131415161718ULL,
		executableSha256, mixedWorkerTelemetry),
		"mixed lockstep-v2 recorder accepts local AI planning telemetry");
	result |= Check(mixedRecorder.recordCommand(1U, 0U, 10U, localDigest) &&
		mixedRecorder.recordCommand(1U, 1U, 20U, remoteDigest),
		"mixed lockstep-v2 keeps commands limited to network-human origins");
	for (std::uint32_t frame = 1U; frame <= kCommonStopFrame; ++frame)
	{
		if (!mixedRecorder.recordFrame(frame, frame ^ 0x5a5a5a5aU, 0U))
		{
			result |= Check(false,
				"mixed lockstep-v2 recorder accepts sequential gameplay frames");
			break;
		}
	}
	result |= Check(mixedRecorder.finish(true, true, true),
		"mixed lockstep-v2 receipt requires a clean AI planning boundary");
	const Receipt &mixedReceipt = mixedRecorder.receipt();
	std::array<char, kReceiptBufferBytes> encoded = {{}};
	result |= Check(ValidateReceipt(mixedReceipt, mixedSession,
		0x1112131415161718ULL, kQualificationNetworkRosterMask, false,
		mixedSession.provenKernelMask).ok() &&
		mixedReceipt.aiPlanning.planningDigest == aiPlanning.planningDigest,
		"mixed lockstep-v2 receipt binds AI telemetry separately from peer commands");
	std::size_t mixedEncodedBytes = 0U;
	result |= Check(EncodeReceipt(mixedReceipt, encoded.data(), encoded.size(),
		&mixedEncodedBytes) && mixedEncodedBytes > 0U,
		"mixed lockstep-v2 receipt has a bounded canonical text encoding");
	Receipt mixedDecoded;
	result |= Check(DecodeReceipt(encoded.data(), mixedEncodedBytes,
		&mixedDecoded).ok() &&
		ValidateReceipt(mixedDecoded, mixedSession,
			0x1112131415161718ULL, kQualificationNetworkRosterMask, false,
			mixedSession.provenKernelMask).ok() &&
		mixedDecoded.session.simulationRosterMask ==
			kQualificationSimulationRosterMask &&
		mixedDecoded.session.aiRosterMask == kQualificationAIRosterMask &&
		mixedDecoded.aiPlanning.planningDigest == aiPlanning.planningDigest,
		"mixed receipt round-trip retains role masks and AI planning digest");

	std::size_t encodedBytes = 0U;
	Receipt oversizedReceipt = receipt;
	oversizedReceipt.checkpointCount = kMaxCheckpoints + 1U;
	result |= Check(!EncodeReceipt(oversizedReceipt, encoded.data(), encoded.size(),
		&encodedBytes), "receipt encoding rejects an out-of-bounds checkpoint count");
	result |= Check(EncodeReceipt(receipt, encoded.data(), encoded.size(),
		&encodedBytes) && encodedBytes > 0U,
		"lockstep-v2 receipt has a bounded canonical text encoding");
	Receipt decoded;
	result |= Check(DecodeReceipt(encoded.data(), encodedBytes, &decoded).ok(),
		"canonical lockstep-v2 receipt decodes");
	result |= Check(ValidateReceipt(decoded, session, 0x0102030405060708ULL,
		0x3U, false, session.provenKernelMask).ok(),
		"decoded receipt retains frame, peer, and telemetry evidence");
	decoded.cleanShutdown = false;
	result |= Check(ResolveValidatedKernelMask(decoded, session,
		0x0102030405060708ULL, 0x3U, false, 0x3fU,
		session.provenKernelMask) == 0U,
		"unclean or altered receipts cannot grant authority");
	return result;
}

} // namespace

int main()
{
#if defined(_MSC_VER)
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	return TestFixedSizes() | TestSizeConversion() | TestWrapperCapacity() |
		TestWrappedCommandOriginPolicy() | TestExternalRuntimeReleaseProof() |
		TestLockstepV2ReceiptContract() |
		TestNetworkHelloContract() | TestNetworkFramePublicationGate() |
		TestNetworkHelloFailureHandlingPolicy() | TestNetworkIngressPolicy() |
		TestNetworkHelloDropPolicy() | TestNetworkFrameResendPolicy() |
		TestNetworkDisconnectFrameRecoveryPolicy() | TestNetworkFrameRecoveryDeliveryPolicy() |
		TestNetworkRecoveryLostAckPolicy() |
		TestNetworkNatPolicy();
}
