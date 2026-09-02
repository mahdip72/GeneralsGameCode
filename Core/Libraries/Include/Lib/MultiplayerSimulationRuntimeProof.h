#pragma once

#include "Lib/MultiplayerSimulationPolicy.h"

#include <cstddef>
#include <string>

namespace rts
{

enum
{
	MULTIPLAYER_SIMULATION_RUNTIME_PROOF_SCHEMA = 1,
	MULTIPLAYER_SIMULATION_RUNTIME_PROOF_MATCH_COUNT = 16,
	MULTIPLAYER_SIMULATION_RUNTIME_PROOF_PEER_PROCESS_COUNT = 40
};

// This is a diagnostic-only v1 record.  It deliberately has no product trust
// authority; ResolveMultiplayerSimulationRuntimeProofMask rejects the v1
// runner/mode even when a caller supplies a nonzero mask.  Live authority must
// use a separately reviewed lockstep-v2 contract.
struct MultiplayerSimulationRuntimeProof
{
	unsigned schema;
	std::string title;
	std::string sourceRevision;
	std::string executableSha256;
	std::string artifactSetSha256;
	std::string evidenceManifestSha256;
	std::string rawEvidenceIndexSha256;
	unsigned policySchema;
	unsigned engineEpoch;
	unsigned determinismEpoch;
	unsigned buildCompatibilityCrc;
	unsigned contentCrc;
	unsigned provenKernelMask;
	unsigned matchCount;
	unsigned peerProcessCount;
	std::string producer;
	std::string validationMode;

	MultiplayerSimulationRuntimeProof() :
		schema(0), policySchema(0), engineEpoch(0), determinismEpoch(0),
		buildCompatibilityCrc(0), contentCrc(0), provenKernelMask(0),
		matchCount(0), peerProcessCount(0)
	{
	}
};

inline bool IsCanonicalUpperHexDigest(const std::string &value,
	unsigned expectedLength)
{
	if (value.size() != expectedLength)
		return false;
	bool hasNonZeroDigit = false;
	for (unsigned index = 0; index < expectedLength; ++index)
	{
		const char digit = value[index];
		if (!((digit >= '0' && digit <= '9') ||
			(digit >= 'A' && digit <= 'F')))
		{
			return false;
		}
		if (digit != '0')
			hasNonZeroDigit = true;
	}
	return hasNonZeroDigit;
}

inline bool ParseRuntimeProofUnsigned(const std::string &value,
	unsigned &output)
{
	if (value.empty())
		return false;
	unsigned parsed = 0;
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		if (value[index] < '0' || value[index] > '9')
			return false;
		const unsigned digit = static_cast<unsigned>(value[index] - '0');
		if (parsed > (0xffffffffU - digit) / 10U)
			return false;
		parsed = parsed * 10U + digit;
	}
	output = parsed;
	return true;
}

inline bool ReadRuntimeProofLine(const std::string &document,
	std::size_t &cursor, const char *name, std::string &value)
{
	const std::string prefix = std::string(name) + "=";
	const std::size_t lineEnd = document.find('\n', cursor);
	if (lineEnd == std::string::npos)
		return false;
	const std::size_t contentEnd = lineEnd > cursor &&
		document[lineEnd - 1] == '\r' ? lineEnd - 1 : lineEnd;
	if (contentEnd < cursor + prefix.size() ||
		document.compare(cursor, prefix.size(), prefix) != 0)
	{
		return false;
	}
	value.assign(document, cursor + prefix.size(),
		contentEnd - cursor - prefix.size());
	cursor = lineEnd + 1;
	return !value.empty();
}

inline bool ParseMultiplayerSimulationRuntimeProof(const char *bytes,
	std::size_t byteCount, MultiplayerSimulationRuntimeProof &proof)
{
	if (bytes == 0 || byteCount == 0 || byteCount > 4096)
		return false;
	const std::string document(bytes, byteCount);
	static const char magic[] =
		"RTS_MULTIPLAYER_SIMULATION_RUNTIME_PROOF_V1\n";
	if (document.compare(0, sizeof(magic) - 1, magic) != 0)
		return false;
	std::size_t cursor = sizeof(magic) - 1;
	std::string value;
#define RTS_READ_RUNTIME_PROOF_STRING(field, name) \
	if (!ReadRuntimeProofLine(document, cursor, name, proof.field)) return false
#define RTS_READ_RUNTIME_PROOF_UNSIGNED(field, name) \
	if (!ReadRuntimeProofLine(document, cursor, name, value) || \
		!ParseRuntimeProofUnsigned(value, proof.field)) return false
	RTS_READ_RUNTIME_PROOF_UNSIGNED(schema, "schema");
	RTS_READ_RUNTIME_PROOF_STRING(title, "title");
	RTS_READ_RUNTIME_PROOF_STRING(sourceRevision, "source_revision");
	RTS_READ_RUNTIME_PROOF_STRING(executableSha256, "executable_sha256");
	RTS_READ_RUNTIME_PROOF_STRING(artifactSetSha256, "artifact_set_sha256");
	RTS_READ_RUNTIME_PROOF_STRING(evidenceManifestSha256, "evidence_manifest_sha256");
	RTS_READ_RUNTIME_PROOF_STRING(rawEvidenceIndexSha256, "raw_evidence_index_sha256");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(policySchema, "policy_schema");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(engineEpoch, "engine_epoch");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(determinismEpoch, "determinism_epoch");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(buildCompatibilityCrc, "build_compatibility_crc");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(contentCrc, "content_crc");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(provenKernelMask, "proven_kernel_mask");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(matchCount, "match_count");
	RTS_READ_RUNTIME_PROOF_UNSIGNED(peerProcessCount, "peer_process_count");
	RTS_READ_RUNTIME_PROOF_STRING(producer, "producer");
	RTS_READ_RUNTIME_PROOF_STRING(validationMode, "validation_mode");
#undef RTS_READ_RUNTIME_PROOF_UNSIGNED
#undef RTS_READ_RUNTIME_PROOF_STRING
	return document.compare(cursor, 4, "END\n") == 0 && cursor + 4 == document.size();
}

inline unsigned ResolveMultiplayerSimulationRuntimeProofMask(
	const MultiplayerSimulationRuntimeProof &proof, const char *expectedTitle,
	const char *actualExecutableSha256, unsigned expectedBuildCompatibilityCrc,
	unsigned expectedContentCrc, unsigned liveIntegratedKernelMask,
	unsigned trustedBuildKernelMask, const char *trustedBuildSourceRevision)
{
	const unsigned knownMask = static_cast<unsigned>(
		MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK);
	// InstalledNet3Validation v1 records useful diagnostic evidence, but it is
	// not a trust root.  Even a caller that supplies a nonzero build mask must
	// not turn a mutable v1 proof bundle into live lockstep authority.  A future
	// lockstep-v2 contract must use a separate schema and resolver with an
	// independently reviewed trust mechanism.
	if (proof.schema == MULTIPLAYER_SIMULATION_RUNTIME_PROOF_SCHEMA &&
		proof.producer == "installed-runtime-runner-v1" &&
		proof.validationMode == "scoped-net3-loopback-release-proof")
	{
		return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	if (trustedBuildKernelMask == 0 || expectedTitle == 0 ||
		actualExecutableSha256 == 0 || trustedBuildSourceRevision == 0 ||
		proof.schema != MULTIPLAYER_SIMULATION_RUNTIME_PROOF_SCHEMA ||
		proof.title != expectedTitle ||
		!IsCanonicalMultiplayerSimulationHexDigest(proof.sourceRevision.c_str(),
			MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH) ||
		!IsCanonicalUpperHexDigest(proof.executableSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalUpperHexDigest(proof.artifactSetSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalUpperHexDigest(proof.evidenceManifestSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalUpperHexDigest(proof.rawEvidenceIndexSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		proof.executableSha256 != actualExecutableSha256 ||
		proof.policySchema != MULTIPLAYER_SIMULATION_POLICY_SCHEMA ||
		proof.engineEpoch != MULTIPLAYER_SIMULATION_ENGINE_EPOCH ||
		proof.determinismEpoch != MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH ||
		proof.buildCompatibilityCrc != expectedBuildCompatibilityCrc ||
		proof.contentCrc != expectedContentCrc ||
		proof.provenKernelMask == 0 || liveIntegratedKernelMask == 0 ||
		(liveIntegratedKernelMask & ~knownMask) != 0 ||
		(trustedBuildKernelMask & ~knownMask) != 0 ||
		trustedBuildKernelMask != liveIntegratedKernelMask ||
		proof.provenKernelMask != trustedBuildKernelMask ||
		!AreExactMultiplayerSimulationHexDigestsEqual(
			proof.sourceRevision.c_str(), trustedBuildSourceRevision,
			MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH) ||
		proof.matchCount != MULTIPLAYER_SIMULATION_RUNTIME_PROOF_MATCH_COUNT ||
		proof.peerProcessCount !=
			MULTIPLAYER_SIMULATION_RUNTIME_PROOF_PEER_PROCESS_COUNT ||
		proof.producer != "installed-runtime-runner-v1" ||
		proof.validationMode != "scoped-net3-loopback-release-proof")
	{
		return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	return trustedBuildKernelMask;
}

} // namespace rts
