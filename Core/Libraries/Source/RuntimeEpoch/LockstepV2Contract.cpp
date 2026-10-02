#include "Lib/LockstepV2Contract.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace rts
{
namespace lockstep_v2
{
namespace
{

constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

std::uint64_t FnvUpdate(std::uint64_t hash,
	const runtime_epoch::Byte *bytes,
	std::size_t byteCount)
{
	if (bytes == nullptr && byteCount != 0U)
		return 0U;
	for (std::size_t index = 0U; index < byteCount; ++index)
	{
		hash ^= static_cast<std::uint64_t>(bytes[index]);
		hash *= kFnvPrime;
	}
	return hash;
}

template <typename Unsigned>
std::uint64_t FnvUnsigned(std::uint64_t hash, Unsigned value)
{
	runtime_epoch::Byte bytes[sizeof(Unsigned)] = {};
	for (std::size_t index = 0U; index < sizeof(Unsigned); ++index)
	{
		bytes[index] = static_cast<runtime_epoch::Byte>(value &
			static_cast<Unsigned>(0xffU));
		value >>= 8U;
	}
	return FnvUpdate(hash, bytes, sizeof(bytes));
}

bool IsKnownOriginMode(CommandOriginMode mode)
{
	return mode == CommandOriginMode::DirectAuthenticated ||
		mode == CommandOriginMode::TrustedRouter;
}

bool IsBitSet(std::uint32_t mask, std::uint32_t slot)
{
	return slot < 32U && (mask & (1U << slot)) != 0U;
}

std::uint32_t CountBits(std::uint32_t value)
{
	std::uint32_t count = 0U;
	while (value != 0U)
	{
		value &= value - 1U;
		++count;
	}
	return count;
}

std::uint32_t CountBits64(std::uint64_t value)
{
	std::uint32_t count = 0U;
	while (value != 0U)
	{
		value &= value - 1U;
		++count;
	}
	return count;
}

bool IsWorkerTelemetryValid(const WorkerTelemetry &telemetry,
	std::uint32_t expectedAuthorityMask)
{
	const std::uint32_t knownKernelMask =
		(1U << kKernelCount) - 1U;
	if (!telemetry.executableOrigin || expectedAuthorityMask == 0U ||
		(expectedAuthorityMask & ~knownKernelMask) != 0U ||
		telemetry.authorityMask != expectedAuthorityMask)
	{
		return false;
	}
	for (std::uint32_t kernel = 0U; kernel < kKernelCount; ++kernel)
	{
		const KernelWorkerTelemetry &evidence = telemetry.kernels[kernel];
		const bool claimed = (expectedAuthorityMask & (1U << kernel)) != 0U;
		if (claimed)
		{
			if (!evidence.physicalWorkerMaskComplete ||
				evidence.physicalWorkerMask == 0U ||
				evidence.physicalWorkerJobs == 0U ||
				CountBits64(evidence.physicalWorkerMask) < 2U ||
				evidence.distinctPhysicalWorkers < 2U ||
				evidence.peakConcurrentPhysicalWorkers < 2U)
			{
				return false;
			}
		}
		else if (evidence.physicalWorkerMask != 0U ||
			evidence.physicalWorkerJobs != 0U ||
			evidence.distinctPhysicalWorkers != 0U ||
			evidence.peakConcurrentPhysicalWorkers != 0U ||
			evidence.physicalWorkerMaskComplete)
		{
			return false;
		}
	}
	return true;
}

std::uint64_t AppendAIPlanningDigestValue(std::uint64_t hash,
	std::uint64_t value, unsigned bytes)
{
	runtime_epoch::Byte encoded[sizeof(std::uint64_t)] = {};
	for (unsigned index = 0U; index < bytes; ++index)
	{
		encoded[index] = static_cast<runtime_epoch::Byte>(value & 0xffU);
		value >>= 8U;
	}
	return FnvUpdate(hash, encoded, bytes);
}

std::uint64_t ComputeAIPlanningDigestInternal(std::uint32_t simulationRosterMask,
	std::uint32_t aiRosterMask, const AIPlanningTelemetry &telemetry)
{
	if (aiRosterMask == 0U)
		return 0U;
	std::uint64_t hash = kFnvOffset;
	hash = AppendAIPlanningDigestValue(hash, simulationRosterMask, 4U);
	hash = AppendAIPlanningDigestValue(hash, aiRosterMask, 4U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.capturedSnapshots, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.capturedCandidates, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.requestedBatches, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.submittedJobs, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.completedJobs, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.serialFallbacks, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.shadowMatches, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.shadowMismatches, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.validationFailures, 8U);
	hash = AppendAIPlanningDigestValue(hash,
		telemetry.canonicalValidationInvocations, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.committedBatches, 8U);
	hash = AppendAIPlanningDigestValue(hash,
		telemetry.parallelAuthoritativeCommits, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.rejectedCommits, 8U);
	hash = AppendAIPlanningDigestValue(hash, telemetry.ownerHelpedExecutions, 8U);
	// Physical worker observations are validated as executable-origin telemetry
	// but deliberately do not enter the cross-peer planning digest: the
	// qualification lane uses mixed worker profiles, so scheduler placement and
	// worker counts may differ without changing the deterministic AI answer.
	return hash;
}

bool IsZeroAIPlanningTelemetry(const AIPlanningTelemetry &telemetry)
{
	return telemetry.capturedSnapshots == 0U &&
		telemetry.capturedCandidates == 0U &&
		telemetry.requestedBatches == 0U && telemetry.submittedJobs == 0U &&
		telemetry.completedJobs == 0U && telemetry.serialFallbacks == 0U &&
		telemetry.shadowMatches == 0U && telemetry.shadowMismatches == 0U &&
		telemetry.validationFailures == 0U &&
		telemetry.canonicalValidationInvocations == 0U &&
		telemetry.committedBatches == 0U &&
		telemetry.parallelAuthoritativeCommits == 0U &&
		telemetry.rejectedCommits == 0U &&
		telemetry.physicalWorkerExecutions == 0U &&
		telemetry.ownerHelpedExecutions == 0U &&
		telemetry.observedPhysicalWorkerMask == 0U &&
		telemetry.maximumDistinctPhysicalWorkers == 0U &&
		telemetry.maximumConcurrentPhysicalWorkers == 0U &&
		telemetry.planningDigest == 0U;
}

std::uint64_t ComputeReceiptCommandDigest(const Receipt &receipt)
{
	std::uint64_t hash = kFnvOffset;
	for (std::uint32_t slot = 0U; slot < kMaxPeerCount; ++slot)
	{
		const PeerCommandContribution &contribution = receipt.contributions[slot];
		hash = FnvUnsigned(hash, slot);
		hash = FnvUnsigned(hash, contribution.commandCount);
		hash = FnvUnsigned(hash, contribution.firstCommandFrame);
		hash = FnvUnsigned(hash, contribution.lastCommandFrame);
		hash = FnvUnsigned(hash, contribution.lastCommandId);
		hash = FnvUnsigned(hash, contribution.hasLastCommandId ? 1U : 0U);
		hash = FnvUnsigned(hash, contribution.lastCommandDigest);
		hash = FnvUnsigned(hash, contribution.commandDigest);
	}
	return hash;
}

bool EqualFixedText(const char *left, const char *right, std::size_t capacity)
{
	if (left == nullptr || right == nullptr)
		return false;
	for (std::size_t index = 0U; index < capacity; ++index)
	{
		if (left[index] != right[index])
			return false;
		if (left[index] == '\0')
			return true;
	}
	return false;
}

bool CopyFixedText(const char *input, std::size_t inputSize,
	char *output, std::size_t outputCapacity, std::size_t requiredChars)
{
	if (input == nullptr || output == nullptr || requiredChars + 1U > outputCapacity ||
		inputSize != requiredChars)
		return false;
	std::memcpy(output, input, inputSize);
	output[inputSize] = '\0';
	return true;
}

bool AppendFormat(char *output, std::size_t capacity, std::size_t &offset,
	const char *format, ...)
{
	if (output == nullptr || format == nullptr || offset >= capacity)
		return false;
	va_list arguments;
	va_start(arguments, format);
	const int written = std::vsnprintf(output + offset, capacity - offset,
		format, arguments);
	va_end(arguments);
	if (written < 0 || static_cast<std::size_t>(written) >= capacity - offset)
		return false;
	offset += static_cast<std::size_t>(written);
	return true;
}

bool AppendTextLine(char *output, std::size_t capacity, std::size_t &offset,
	const char *key, const char *value)
{
	if (key == nullptr || value == nullptr)
		return false;
	return AppendFormat(output, capacity, offset, "%s=%s\n", key, value);
}

bool AppendUnsignedLine(char *output, std::size_t capacity, std::size_t &offset,
	const char *key, std::uint64_t value)
{
	return AppendFormat(output, capacity, offset, "%s=%llu\n", key,
		static_cast<unsigned long long>(value));
}

bool AppendBoolLine(char *output, std::size_t capacity, std::size_t &offset,
	const char *key, bool value)
{
	return AppendUnsignedLine(output, capacity, offset, key, value ? 1U : 0U);
}

bool ReadLine(const char *input, std::size_t inputSize, std::size_t &cursor,
	char *line, std::size_t lineCapacity)
{
	if (input == nullptr || line == nullptr || lineCapacity < 2U || cursor >= inputSize)
		return false;
	std::size_t lineEnd = cursor;
	while (lineEnd < inputSize && input[lineEnd] != '\n')
		++lineEnd;
	if (lineEnd >= inputSize || lineEnd - cursor + 1U > lineCapacity)
		return false;
	if (lineEnd > cursor && input[lineEnd - 1U] == '\r')
		return false;
	const std::size_t lineLength = lineEnd - cursor;
	std::memcpy(line, input + cursor, lineLength);
	line[lineLength] = '\0';
	cursor = lineEnd + 1U;
	return true;
}

bool SplitLine(const char *line, const char *key, const char **value)
{
	if (line == nullptr || key == nullptr || value == nullptr)
		return false;
	const std::size_t keyLength = std::strlen(key);
	if (std::strncmp(line, key, keyLength) != 0 || line[keyLength] != '=')
		return false;
	*value = line + keyLength + 1U;
	return true;
}

bool ParseUnsignedValue(const char *value, std::uint64_t maximum,
	std::uint64_t *parsed)
{
	if (value == nullptr || parsed == nullptr || *value == '\0')
		return false;
	std::uint64_t number = 0U;
	for (const char *cursor = value; *cursor != '\0'; ++cursor)
	{
		if (*cursor < '0' || *cursor > '9')
			return false;
		const std::uint64_t digit = static_cast<std::uint64_t>(*cursor - '0');
		if (digit > maximum || number > (maximum - digit) / 10U)
			return false;
		number = number * 10U + digit;
	}
	*parsed = number;
	return true;
}

bool ReadUnsignedLine(const char *input, std::size_t inputSize,
	std::size_t &cursor, const char *key, std::uint64_t maximum,
	std::uint64_t *value)
{
	char line[256] = {};
	const char *text = nullptr;
	return ReadLine(input, inputSize, cursor, line, sizeof(line)) &&
		SplitLine(line, key, &text) && ParseUnsignedValue(text, maximum, value);
}

bool ReadBoolLine(const char *input, std::size_t inputSize,
	std::size_t &cursor, const char *key, bool *value)
{
	std::uint64_t parsed = 0U;
	if (!ReadUnsignedLine(input, inputSize, cursor, key, 1U, &parsed) || value == nullptr)
		return false;
	*value = parsed != 0U;
	return true;
}

bool ReadTextLine(const char *input, std::size_t inputSize,
	std::size_t &cursor, const char *key, char *value,
	std::size_t valueCapacity, std::size_t requiredChars)
{
	char line[256] = {};
	const char *text = nullptr;
	if (!ReadLine(input, inputSize, cursor, line, sizeof(line)) ||
		!SplitLine(line, key, &text))
		return false;
	return CopyFixedText(text, std::strlen(text), value, valueCapacity, requiredChars);
}

bool ReadExactLine(const char *input, std::size_t inputSize,
	std::size_t &cursor, const char *expected)
{
	char line[256] = {};
	return ReadLine(input, inputSize, cursor, line, sizeof(line)) &&
		std::strcmp(line, expected) == 0;
}

bool AppendAIPlanningTelemetry(char *output, std::size_t capacity,
	std::size_t &offset, const AIPlanningTelemetry &telemetry)
{
	return AppendUnsignedLine(output, capacity, offset,
		"ai_planning_captured_snapshots", telemetry.capturedSnapshots) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_captured_candidates", telemetry.capturedCandidates) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_requested_batches", telemetry.requestedBatches) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_submitted_jobs", telemetry.submittedJobs) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_completed_jobs", telemetry.completedJobs) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_serial_fallbacks", telemetry.serialFallbacks) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_shadow_matches", telemetry.shadowMatches) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_shadow_mismatches", telemetry.shadowMismatches) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_validation_failures", telemetry.validationFailures) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_canonical_validation_invocations",
			telemetry.canonicalValidationInvocations) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_committed_batches", telemetry.committedBatches) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_parallel_authoritative_commits",
			telemetry.parallelAuthoritativeCommits) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_rejected_commits", telemetry.rejectedCommits) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_physical_worker_executions",
			telemetry.physicalWorkerExecutions) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_owner_helped_executions",
			telemetry.ownerHelpedExecutions) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_observed_physical_worker_mask",
			telemetry.observedPhysicalWorkerMask) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_maximum_distinct_physical_workers",
			telemetry.maximumDistinctPhysicalWorkers) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_maximum_concurrent_physical_workers",
			telemetry.maximumConcurrentPhysicalWorkers) &&
		AppendUnsignedLine(output, capacity, offset,
			"ai_planning_digest", telemetry.planningDigest);
}

bool ReadAIPlanningTelemetry(const char *input, std::size_t inputSize,
	std::size_t &cursor, AIPlanningTelemetry &telemetry)
{
	std::uint64_t *values[] =
	{
		&telemetry.capturedSnapshots, &telemetry.capturedCandidates,
		&telemetry.requestedBatches, &telemetry.submittedJobs,
		&telemetry.completedJobs, &telemetry.serialFallbacks,
		&telemetry.shadowMatches, &telemetry.shadowMismatches,
		&telemetry.validationFailures,
		&telemetry.canonicalValidationInvocations,
		&telemetry.committedBatches,
		&telemetry.parallelAuthoritativeCommits, &telemetry.rejectedCommits,
		&telemetry.physicalWorkerExecutions, &telemetry.ownerHelpedExecutions,
		&telemetry.observedPhysicalWorkerMask,
		&telemetry.maximumDistinctPhysicalWorkers,
		&telemetry.maximumConcurrentPhysicalWorkers, &telemetry.planningDigest
	};
	const char *const keys[] =
	{
		"ai_planning_captured_snapshots", "ai_planning_captured_candidates",
		"ai_planning_requested_batches", "ai_planning_submitted_jobs",
		"ai_planning_completed_jobs", "ai_planning_serial_fallbacks",
		"ai_planning_shadow_matches", "ai_planning_shadow_mismatches",
		"ai_planning_validation_failures",
		"ai_planning_canonical_validation_invocations",
		"ai_planning_committed_batches",
		"ai_planning_parallel_authoritative_commits",
		"ai_planning_rejected_commits",
		"ai_planning_physical_worker_executions",
		"ai_planning_owner_helped_executions",
		"ai_planning_observed_physical_worker_mask",
		"ai_planning_maximum_distinct_physical_workers",
		"ai_planning_maximum_concurrent_physical_workers",
		"ai_planning_digest"
	};
	for (unsigned index = 0U; index < sizeof(values) / sizeof(values[0]); ++index)
	{
		if (!ReadUnsignedLine(input, inputSize, cursor, keys[index],
			std::numeric_limits<std::uint64_t>::max(), values[index]))
			return false;
	}
	return true;
}

bool AppendSession(char *output, std::size_t capacity, std::size_t &offset,
	const SessionContract &session)
{
	return AppendUnsignedLine(output, capacity, offset, "schema", session.schemaVersion) &&
		AppendUnsignedLine(output, capacity, offset, "protocol_epoch", session.protocolEpoch) &&
		AppendUnsignedLine(output, capacity, offset, "local_slot", session.localSlot) &&
		AppendUnsignedLine(output, capacity, offset, "peer_count", session.peerCount) &&
		AppendUnsignedLine(output, capacity, offset, "roster_mask", session.rosterMask) &&
		AppendUnsignedLine(output, capacity, offset, "simulation_roster_mask",
			session.simulationRosterMask) &&
		AppendUnsignedLine(output, capacity, offset, "ai_roster_mask",
			session.aiRosterMask) &&
		AppendUnsignedLine(output, capacity, offset, "build_compatibility_crc", session.buildCompatibilityCrc) &&
		AppendUnsignedLine(output, capacity, offset, "content_crc", session.contentCrc) &&
		AppendUnsignedLine(output, capacity, offset, "map_crc", session.mapCrc) &&
		AppendUnsignedLine(output, capacity, offset, "common_stop_frame", session.commonStopFrame) &&
		AppendUnsignedLine(output, capacity, offset, "proven_kernel_mask", session.provenKernelMask) &&
		AppendUnsignedLine(output, capacity, offset, "packet_router_slot", session.packetRouterSlot) &&
		AppendUnsignedLine(output, capacity, offset, "origin_mode",
			static_cast<std::uint32_t>(session.originMode)) &&
		AppendTextLine(output, capacity, offset, "run_nonce", session.runNonce.data()) &&
		AppendTextLine(output, capacity, offset, "session_nonce", session.sessionNonce.data()) &&
		AppendTextLine(output, capacity, offset, "executable_sha256", session.executableSha256.data()) &&
		AppendTextLine(output, capacity, offset, "source_revision", session.sourceRevision.data());
}

bool ReadSession(const char *input, std::size_t inputSize, std::size_t &cursor,
	SessionContract &session)
{
	std::uint64_t value = 0U;
	if (!ReadUnsignedLine(input, inputSize, cursor, "schema",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.schemaVersion = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "protocol_epoch",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.protocolEpoch = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "local_slot",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.localSlot = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "peer_count",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.peerCount = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "roster_mask",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.rosterMask = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "simulation_roster_mask",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.simulationRosterMask = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "ai_roster_mask",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.aiRosterMask = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "build_compatibility_crc",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.buildCompatibilityCrc = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "content_crc",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.contentCrc = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "map_crc",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.mapCrc = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "common_stop_frame",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.commonStopFrame = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "proven_kernel_mask",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.provenKernelMask = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "packet_router_slot",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.packetRouterSlot = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "origin_mode",
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	session.originMode = static_cast<CommandOriginMode>(static_cast<std::uint32_t>(value));
	return ReadTextLine(input, inputSize, cursor, "run_nonce",
		session.runNonce.data(), session.runNonce.size(), kNonceHexChars) &&
		ReadTextLine(input, inputSize, cursor, "session_nonce",
			session.sessionNonce.data(), session.sessionNonce.size(), kNonceHexChars) &&
		ReadTextLine(input, inputSize, cursor, "executable_sha256",
			session.executableSha256.data(), session.executableSha256.size(), kSha256HexChars) &&
		ReadTextLine(input, inputSize, cursor, "source_revision",
			session.sourceRevision.data(), session.sourceRevision.size(), kSourceRevisionHexChars);
}

bool AppendReceiptBody(char *output, std::size_t capacity, std::size_t &offset,
	const Receipt &receipt)
{
	if (!AppendSession(output, capacity, offset, receipt.session) ||
		!AppendUnsignedLine(output, capacity, offset, "network_session_token",
			receipt.networkSessionToken) ||
		!AppendUnsignedLine(output, capacity, offset, "final_frame", receipt.finalFrame) ||
		!AppendUnsignedLine(output, capacity, offset, "frame_count", receipt.frameCount) ||
		!AppendUnsignedLine(output, capacity, offset, "contributed_peer_mask",
			receipt.contributedPeerMask) ||
		!AppendUnsignedLine(output, capacity, offset, "checkpoint_count",
			receipt.checkpointCount) ||
		!AppendUnsignedLine(output, capacity, offset, "validation_authority_mask",
			receipt.validationAuthorityMask) ||
		!AppendBoolLine(output, capacity, offset, "executable_origin", receipt.executableOrigin) ||
		!AppendBoolLine(output, capacity, offset, "worker_telemetry_executable_origin",
			receipt.workerTelemetryExecutableOrigin) ||
		!AppendBoolLine(output, capacity, offset, "transport_path_used", receipt.transportPathUsed) ||
		!AppendBoolLine(output, capacity, offset, "handshake_validated", receipt.handshakeValidated) ||
		!AppendBoolLine(output, capacity, offset, "clean_shutdown", receipt.cleanShutdown) ||
		!AppendAIPlanningTelemetry(output, capacity, offset, receipt.aiPlanning))
		return false;

	for (std::uint32_t slot = 0U; slot < kMaxPeerCount; ++slot)
	{
		char key[64] = {};
		const PeerCommandContribution &contribution = receipt.contributions[slot];
		std::snprintf(key, sizeof(key), "peer_%u_command_count", slot);
		if (!AppendUnsignedLine(output, capacity, offset, key, contribution.commandCount)) return false;
		std::snprintf(key, sizeof(key), "peer_%u_first_command_frame", slot);
		if (!AppendUnsignedLine(output, capacity, offset, key, contribution.firstCommandFrame)) return false;
		std::snprintf(key, sizeof(key), "peer_%u_last_command_frame", slot);
		if (!AppendUnsignedLine(output, capacity, offset, key, contribution.lastCommandFrame)) return false;
		std::snprintf(key, sizeof(key), "peer_%u_last_command_id", slot);
		if (!AppendUnsignedLine(output, capacity, offset, key, contribution.lastCommandId)) return false;
		std::snprintf(key, sizeof(key), "peer_%u_has_last_command_id", slot);
		if (!AppendBoolLine(output, capacity, offset, key, contribution.hasLastCommandId)) return false;
		std::snprintf(key, sizeof(key), "peer_%u_last_command_digest", slot);
		if (!AppendUnsignedLine(output, capacity, offset, key, contribution.lastCommandDigest)) return false;
		std::snprintf(key, sizeof(key), "peer_%u_command_digest", slot);
		if (!AppendUnsignedLine(output, capacity, offset, key, contribution.commandDigest)) return false;
	}

	for (std::uint32_t kernel = 0U; kernel < kKernelCount; ++kernel)
	{
		char key[96] = {};
		const KernelWorkerTelemetry &evidence = receipt.workerTelemetry[kernel];
		std::snprintf(key, sizeof(key), "kernel_%u_physical_worker_mask", kernel);
		if (!AppendUnsignedLine(output, capacity, offset, key,
			evidence.physicalWorkerMask)) return false;
		std::snprintf(key, sizeof(key), "kernel_%u_physical_worker_jobs", kernel);
		if (!AppendUnsignedLine(output, capacity, offset, key,
			evidence.physicalWorkerJobs)) return false;
		std::snprintf(key, sizeof(key), "kernel_%u_distinct_physical_workers", kernel);
		if (!AppendUnsignedLine(output, capacity, offset, key,
			evidence.distinctPhysicalWorkers)) return false;
		std::snprintf(key, sizeof(key), "kernel_%u_peak_concurrent_physical_workers", kernel);
		if (!AppendUnsignedLine(output, capacity, offset, key,
			evidence.peakConcurrentPhysicalWorkers)) return false;
		std::snprintf(key, sizeof(key), "kernel_%u_physical_worker_mask_complete", kernel);
		if (!AppendBoolLine(output, capacity, offset, key,
			evidence.physicalWorkerMaskComplete)) return false;
	}

	for (std::uint32_t index = 0U; index < receipt.checkpointCount; ++index)
	{
		char key[64] = {};
		const FrameCheckpoint &checkpoint = receipt.checkpoints[index];
		std::snprintf(key, sizeof(key), "checkpoint_%u_frame", index);
		if (!AppendUnsignedLine(output, capacity, offset, key, checkpoint.frame)) return false;
		std::snprintf(key, sizeof(key), "checkpoint_%u_crc", index);
		if (!AppendUnsignedLine(output, capacity, offset, key, checkpoint.crc)) return false;
		std::snprintf(key, sizeof(key), "checkpoint_%u_command_digest", index);
		if (!AppendUnsignedLine(output, capacity, offset, key, checkpoint.commandDigest)) return false;
	}
	return true;
}

bool ReadPeerContribution(const char *input, std::size_t inputSize,
	std::size_t &cursor, std::uint32_t slot, PeerCommandContribution &contribution)
{
	char key[64] = {};
	std::uint64_t value = 0U;
	std::snprintf(key, sizeof(key), "peer_%u_command_count", slot);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	contribution.commandCount = static_cast<std::uint32_t>(value);
	std::snprintf(key, sizeof(key), "peer_%u_first_command_frame", slot);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	contribution.firstCommandFrame = static_cast<std::uint32_t>(value);
	std::snprintf(key, sizeof(key), "peer_%u_last_command_frame", slot);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	contribution.lastCommandFrame = static_cast<std::uint32_t>(value);
	std::snprintf(key, sizeof(key), "peer_%u_last_command_id", slot);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint16_t>::max(), &value)) return false;
	contribution.lastCommandId = static_cast<std::uint16_t>(value);
	std::snprintf(key, sizeof(key), "peer_%u_has_last_command_id", slot);
	if (!ReadBoolLine(input, inputSize, cursor, key, &contribution.hasLastCommandId)) return false;
	std::snprintf(key, sizeof(key), "peer_%u_last_command_digest", slot);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint64_t>::max(), &value)) return false;
	contribution.lastCommandDigest = value;
	std::snprintf(key, sizeof(key), "peer_%u_command_digest", slot);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint64_t>::max(), &value)) return false;
	contribution.commandDigest = value;
	return true;
}

bool ReadCheckpoint(const char *input, std::size_t inputSize,
	std::size_t &cursor, std::uint32_t index, FrameCheckpoint &checkpoint)
{
	char key[64] = {};
	std::uint64_t value = 0U;
	std::snprintf(key, sizeof(key), "checkpoint_%u_frame", index);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	checkpoint.frame = static_cast<std::uint32_t>(value);
	std::snprintf(key, sizeof(key), "checkpoint_%u_crc", index);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint32_t>::max(), &value)) return false;
	checkpoint.crc = static_cast<std::uint32_t>(value);
	std::snprintf(key, sizeof(key), "checkpoint_%u_command_digest", index);
	if (!ReadUnsignedLine(input, inputSize, cursor, key,
		std::numeric_limits<std::uint64_t>::max(), &value)) return false;
	checkpoint.commandDigest = value;
	return true;
}

} // namespace

std::uint64_t ComputeCommandDigest(const runtime_epoch::Byte *bytes,
	std::size_t byteCount)
{
	return FnvUpdate(kFnvOffset, bytes, byteCount);
}

std::uint64_t MixCommandDigest(std::uint64_t priorDigest,
	std::uint32_t frame, std::uint32_t originSlot,
	std::uint16_t commandId, std::uint64_t commandDigest)
{
	std::uint64_t hash = priorDigest == 0U ? kFnvOffset : priorDigest;
	hash = FnvUnsigned(hash, frame);
	hash = FnvUnsigned(hash, originSlot);
	hash = FnvUnsigned(hash, commandId);
	hash = FnvUnsigned(hash, commandDigest);
	return hash;
}

std::uint64_t ComputeAIPlanningDigest(std::uint32_t simulationRosterMask,
	std::uint32_t aiRosterMask, const AIPlanningTelemetry &telemetry)
{
	return ComputeAIPlanningDigestInternal(simulationRosterMask, aiRosterMask,
		telemetry);
}

bool IsCanonicalHex(const char *value, std::size_t hexChars)
{
	if (value == nullptr)
		return false;
	for (std::size_t index = 0U; index < hexChars; ++index)
	{
		const char character = value[index];
		const bool digit = character >= '0' && character <= '9';
		const bool upper = character >= 'A' && character <= 'F';
		const bool lower = character >= 'a' && character <= 'f';
		if (!digit && !upper && !lower)
			return false;
	}
	return value[hexChars] == '\0';
}

bool IsNonZeroCanonicalHex(const char *value, std::size_t hexChars)
{
	if (!IsCanonicalHex(value, hexChars))
		return false;
	for (std::size_t index = 0U; index < hexChars; ++index)
		if (value[index] != '0')
			return true;
	return false;
}

bool IsValidSessionContract(const SessionContract &session)
{
	const std::uint32_t validSlotMask = (1U << kMaxPeerCount) - 1U;
	if (session.schemaVersion != kSchemaVersion ||
		session.protocolEpoch != kProtocolEpoch ||
		session.peerCount < kMinPeerCount ||
		session.peerCount > kMaxPeerCount ||
		(session.rosterMask & ~validSlotMask) != 0U ||
		(session.simulationRosterMask & ~validSlotMask) != 0U ||
		(session.aiRosterMask & ~validSlotMask) != 0U ||
		session.localSlot >= session.peerCount ||
		session.commonStopFrame != kCommonStopFrame ||
		CountBits(session.rosterMask) != session.peerCount ||
		!IsBitSet(session.rosterMask, session.localSlot) ||
		session.simulationRosterMask == 0U ||
		(session.rosterMask & ~session.simulationRosterMask) != 0U ||
		(session.aiRosterMask & ~session.simulationRosterMask) != 0U ||
		(session.rosterMask & session.aiRosterMask) != 0U ||
		(session.rosterMask | session.aiRosterMask) !=
			session.simulationRosterMask ||
		!IsKnownOriginMode(session.originMode) ||
		!IsNonZeroCanonicalHex(session.runNonce.data(), kNonceHexChars) ||
		!IsNonZeroCanonicalHex(session.sessionNonce.data(), kNonceHexChars) ||
		!IsNonZeroCanonicalHex(session.executableSha256.data(), kSha256HexChars) ||
		!IsNonZeroCanonicalHex(session.sourceRevision.data(), kSourceRevisionHexChars))
		return false;
	if (session.originMode == CommandOriginMode::DirectAuthenticated)
		return session.packetRouterSlot == kNoRouterSlot;
	return session.packetRouterSlot < kMaxPeerCount &&
		IsBitSet(session.rosterMask, session.packetRouterSlot);
}

bool IsValidAIPlanningTelemetry(const SessionContract &session,
	const AIPlanningTelemetry &telemetry)
{
	if (!IsValidSessionContract(session))
		return false;
	if (session.aiRosterMask == 0U)
		return IsZeroAIPlanningTelemetry(telemetry);
	if (telemetry.capturedSnapshots < CountBits(session.aiRosterMask) ||
		telemetry.requestedBatches == 0U || telemetry.submittedJobs == 0U ||
		telemetry.completedJobs != telemetry.submittedJobs ||
		telemetry.serialFallbacks != 0U || telemetry.shadowMismatches != 0U ||
		telemetry.validationFailures != 0U || telemetry.committedBatches == 0U ||
		telemetry.parallelAuthoritativeCommits == 0U ||
		telemetry.parallelAuthoritativeCommits > telemetry.committedBatches ||
		telemetry.rejectedCommits != 0U ||
		telemetry.physicalWorkerExecutions == 0U ||
		telemetry.ownerHelpedExecutions != 0U ||
		telemetry.observedPhysicalWorkerMask == 0U ||
		CountBits64(telemetry.observedPhysicalWorkerMask) < 2U ||
		telemetry.maximumDistinctPhysicalWorkers < 2U ||
		telemetry.maximumConcurrentPhysicalWorkers < 2U ||
		telemetry.maximumConcurrentPhysicalWorkers >
			telemetry.maximumDistinctPhysicalWorkers ||
		telemetry.planningDigest == 0U ||
		telemetry.planningDigest != ComputeAIPlanningDigestInternal(
			session.simulationRosterMask, session.aiRosterMask, telemetry))
		return false;
	return true;
}

ValidationResult ValidateReceipt(const Receipt &receipt,
	const SessionContract &expectedSession,
	std::uint64_t expectedNetworkSessionToken,
	std::uint32_t expectedPeerMask,
	bool allowTrustedRouter,
	std::uint32_t actualValidationAuthorityMask)
{
	if (!IsValidSessionContract(expectedSession) ||
		!IsValidSessionContract(receipt.session))
		return {ValidationError::InvalidSchema};
	if (receipt.session.schemaVersion != expectedSession.schemaVersion ||
		receipt.session.protocolEpoch != expectedSession.protocolEpoch ||
		receipt.session.localSlot != expectedSession.localSlot ||
		receipt.session.peerCount != expectedSession.peerCount ||
		receipt.session.rosterMask != expectedSession.rosterMask ||
		receipt.session.simulationRosterMask !=
			expectedSession.simulationRosterMask ||
		receipt.session.aiRosterMask != expectedSession.aiRosterMask ||
		receipt.session.buildCompatibilityCrc != expectedSession.buildCompatibilityCrc ||
		receipt.session.contentCrc != expectedSession.contentCrc ||
		receipt.session.mapCrc != expectedSession.mapCrc ||
		receipt.session.commonStopFrame != expectedSession.commonStopFrame ||
		receipt.session.provenKernelMask != expectedSession.provenKernelMask ||
		receipt.session.packetRouterSlot != expectedSession.packetRouterSlot ||
		receipt.session.originMode != expectedSession.originMode ||
		!EqualFixedText(receipt.session.runNonce.data(), expectedSession.runNonce.data(),
			receipt.session.runNonce.size()) ||
		!EqualFixedText(receipt.session.sessionNonce.data(), expectedSession.sessionNonce.data(),
			receipt.session.sessionNonce.size()) ||
		!EqualFixedText(receipt.session.executableSha256.data(), expectedSession.executableSha256.data(),
			receipt.session.executableSha256.size()) ||
		!EqualFixedText(receipt.session.sourceRevision.data(), expectedSession.sourceRevision.data(),
			receipt.session.sourceRevision.size()))
		return {ValidationError::InvalidSchema};
	if (receipt.networkSessionToken == 0U ||
		(expectedNetworkSessionToken != 0U &&
			receipt.networkSessionToken != expectedNetworkSessionToken))
		return {ValidationError::InvalidNetworkSession};
	if (!receipt.executableOrigin)
		return {ValidationError::MissingExecutableOrigin};
	if (!receipt.workerTelemetryExecutableOrigin ||
		receipt.validationAuthorityMask == 0U ||
		receipt.validationAuthorityMask != expectedSession.provenKernelMask ||
		receipt.validationAuthorityMask != actualValidationAuthorityMask)
		return {ValidationError::AuthorityNotProven};
	WorkerTelemetry workerTelemetry;
	workerTelemetry.authorityMask = receipt.validationAuthorityMask;
	workerTelemetry.executableOrigin = receipt.workerTelemetryExecutableOrigin;
	workerTelemetry.kernels = receipt.workerTelemetry;
	if (!IsWorkerTelemetryValid(workerTelemetry,
		actualValidationAuthorityMask))
		return {ValidationError::AuthorityNotProven};
	if (!IsValidAIPlanningTelemetry(expectedSession, receipt.aiPlanning))
		return {ValidationError::AuthorityNotProven};
	if (!receipt.transportPathUsed)
		return {ValidationError::MissingTransportPath};
	if (!receipt.handshakeValidated)
		return {ValidationError::MissingHandshake};
	if (!receipt.cleanShutdown)
		return {ValidationError::UncleanShutdown};
	if (receipt.session.originMode == CommandOriginMode::TrustedRouter &&
		!allowTrustedRouter)
		return {ValidationError::InvalidMode};
	if (expectedPeerMask == 0U || expectedPeerMask != receipt.session.rosterMask ||
		receipt.contributedPeerMask != expectedPeerMask)
		return {ValidationError::InvalidRoster};
	if (receipt.finalFrame != kCommonStopFrame ||
		receipt.frameCount != kCommonStopFrame)
		return {ValidationError::InvalidFrame};
	for (std::uint32_t slot = 0U; slot < kMaxPeerCount; ++slot)
	{
		if (!IsBitSet(expectedPeerMask, slot))
			continue;
		const PeerCommandContribution &contribution = receipt.contributions[slot];
		if (contribution.commandCount == 0U || contribution.commandDigest == 0U ||
			contribution.firstCommandFrame == 0U ||
			contribution.firstCommandFrame > contribution.lastCommandFrame ||
			contribution.lastCommandFrame > kCommonStopFrame ||
			!contribution.hasLastCommandId || contribution.lastCommandDigest == 0U)
			return {ValidationError::MissingPeerContribution};
	}
	if (receipt.checkpointCount < 2U || receipt.checkpointCount > kMaxCheckpoints)
		return {ValidationError::MissingCheckpoint};
	std::uint32_t previousFrame = 0U;
	for (std::uint32_t index = 0U; index < receipt.checkpointCount; ++index)
	{
		const FrameCheckpoint &checkpoint = receipt.checkpoints[index];
		if (checkpoint.frame == 0U || checkpoint.frame > kCommonStopFrame ||
			checkpoint.frame <= previousFrame)
			return {ValidationError::NonMonotonicCheckpoint};
		previousFrame = checkpoint.frame;
	}
	if (receipt.checkpoints[receipt.checkpointCount - 1U].frame != kCommonStopFrame ||
		receipt.checkpoints[receipt.checkpointCount - 1U].commandDigest == 0U ||
		receipt.checkpoints[receipt.checkpointCount - 1U].commandDigest !=
		ComputeReceiptCommandDigest(receipt))
		return {ValidationError::MissingCheckpoint};
	return {};
}

std::uint32_t ResolveValidatedKernelMask(const Receipt &receipt,
	const SessionContract &expectedSession,
	std::uint64_t expectedNetworkSessionToken,
	std::uint32_t expectedPeerMask,
	bool allowTrustedRouter,
	std::uint32_t requestedMask,
	std::uint32_t actualValidationAuthorityMask)
{
	const ValidationResult validation = ValidateReceipt(receipt, expectedSession,
		expectedNetworkSessionToken, expectedPeerMask, allowTrustedRouter,
		actualValidationAuthorityMask);
	if (!validation.ok() || requestedMask != expectedSession.provenKernelMask ||
		requestedMask != actualValidationAuthorityMask)
		return 0U;
	return requestedMask;
}

bool EncodeReceipt(const Receipt &receipt, char *output,
	std::size_t outputCapacity, std::size_t *written)
{
	if (output == nullptr || written == nullptr || outputCapacity == 0U ||
		receipt.checkpointCount > kMaxCheckpoints)
		return false;
	std::size_t offset = 0U;
	output[0] = '\0';
	if (!AppendFormat(output, outputCapacity, offset, "%s\n", kReceiptMagic) ||
		!AppendTextLine(output, outputCapacity, offset, "producer", kReceiptProducer) ||
		!AppendTextLine(output, outputCapacity, offset, "mode", kReceiptMode) ||
		!AppendReceiptBody(output, outputCapacity, offset, receipt) ||
		!AppendFormat(output, outputCapacity, offset, "END\n"))
		return false;
	*written = offset;
	return true;
}

ValidationResult DecodeReceipt(const char *input, std::size_t inputSize,
	Receipt *receipt)
{
	if (input == nullptr || receipt == nullptr || inputSize == 0U ||
		inputSize > kReceiptBufferBytes)
		return {ValidationError::NullInput};
	std::size_t cursor = 0U;
	if (!ReadExactLine(input, inputSize, cursor, kReceiptMagic))
		return {ValidationError::InvalidReceiptText};
	char producer[kProducerChars + 1U] = {};
	char mode[kModeChars + 1U] = {};
	if (!ReadTextLine(input, inputSize, cursor, "producer", producer,
		sizeof(producer), sizeof(kReceiptProducer) - 1U) ||
		!ReadTextLine(input, inputSize, cursor, "mode", mode,
		sizeof(mode), sizeof(kReceiptMode) - 1U) ||
		std::strcmp(producer, kReceiptProducer) != 0 ||
		std::strcmp(mode, kReceiptMode) != 0 ||
		!ReadSession(input, inputSize, cursor, receipt->session))
		return {ValidationError::InvalidReceiptText};

	std::uint64_t value = 0U;
	if (!ReadUnsignedLine(input, inputSize, cursor, "network_session_token",
		std::numeric_limits<std::uint64_t>::max(), &receipt->networkSessionToken) ||
		!ReadUnsignedLine(input, inputSize, cursor, "final_frame",
		std::numeric_limits<std::uint32_t>::max(), &value))
		return {ValidationError::InvalidReceiptText};
	receipt->finalFrame = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "frame_count",
		std::numeric_limits<std::uint32_t>::max(), &value)) return {ValidationError::InvalidReceiptText};
	receipt->frameCount = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "contributed_peer_mask",
		std::numeric_limits<std::uint32_t>::max(), &value)) return {ValidationError::InvalidReceiptText};
	receipt->contributedPeerMask = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "checkpoint_count",
		kMaxCheckpoints, &value)) return {ValidationError::InvalidReceiptText};
	receipt->checkpointCount = static_cast<std::uint32_t>(value);
	if (!ReadUnsignedLine(input, inputSize, cursor, "validation_authority_mask",
		std::numeric_limits<std::uint32_t>::max(), &value))
		return {ValidationError::InvalidReceiptText};
	receipt->validationAuthorityMask = static_cast<std::uint32_t>(value);
	if (!ReadBoolLine(input, inputSize, cursor, "executable_origin", &receipt->executableOrigin) ||
		!ReadBoolLine(input, inputSize, cursor, "worker_telemetry_executable_origin",
			&receipt->workerTelemetryExecutableOrigin) ||
		!ReadBoolLine(input, inputSize, cursor, "transport_path_used", &receipt->transportPathUsed) ||
		!ReadBoolLine(input, inputSize, cursor, "handshake_validated", &receipt->handshakeValidated) ||
		!ReadBoolLine(input, inputSize, cursor, "clean_shutdown", &receipt->cleanShutdown) ||
		!ReadAIPlanningTelemetry(input, inputSize, cursor, receipt->aiPlanning))
		return {ValidationError::InvalidReceiptText};

	for (std::uint32_t slot = 0U; slot < kMaxPeerCount; ++slot)
		if (!ReadPeerContribution(input, inputSize, cursor, slot, receipt->contributions[slot]))
			return {ValidationError::InvalidReceiptText};
	for (std::uint32_t kernel = 0U; kernel < kKernelCount; ++kernel)
	{
		char key[96] = {};
		KernelWorkerTelemetry &evidence = receipt->workerTelemetry[kernel];
		std::snprintf(key, sizeof(key), "kernel_%u_physical_worker_mask", kernel);
		if (!ReadUnsignedLine(input, inputSize, cursor, key,
			std::numeric_limits<std::uint64_t>::max(), &value))
			return {ValidationError::InvalidReceiptText};
		evidence.physicalWorkerMask = value;
		std::snprintf(key, sizeof(key), "kernel_%u_physical_worker_jobs", kernel);
		if (!ReadUnsignedLine(input, inputSize, cursor, key,
			std::numeric_limits<std::uint32_t>::max(), &value))
			return {ValidationError::InvalidReceiptText};
		evidence.physicalWorkerJobs = static_cast<std::uint32_t>(value);
		std::snprintf(key, sizeof(key), "kernel_%u_distinct_physical_workers", kernel);
		if (!ReadUnsignedLine(input, inputSize, cursor, key,
			std::numeric_limits<std::uint32_t>::max(), &value))
			return {ValidationError::InvalidReceiptText};
		evidence.distinctPhysicalWorkers = static_cast<std::uint32_t>(value);
		std::snprintf(key, sizeof(key), "kernel_%u_peak_concurrent_physical_workers", kernel);
		if (!ReadUnsignedLine(input, inputSize, cursor, key,
			std::numeric_limits<std::uint32_t>::max(), &value))
			return {ValidationError::InvalidReceiptText};
		evidence.peakConcurrentPhysicalWorkers = static_cast<std::uint32_t>(value);
		std::snprintf(key, sizeof(key), "kernel_%u_physical_worker_mask_complete", kernel);
		if (!ReadBoolLine(input, inputSize, cursor, key,
			&evidence.physicalWorkerMaskComplete))
			return {ValidationError::InvalidReceiptText};
	}
	if (receipt->checkpointCount > kMaxCheckpoints)
		return {ValidationError::InvalidReceiptText};
	for (std::uint32_t index = 0U; index < receipt->checkpointCount; ++index)
		if (!ReadCheckpoint(input, inputSize, cursor, index, receipt->checkpoints[index]))
			return {ValidationError::InvalidReceiptText};
	if (!ReadExactLine(input, inputSize, cursor, "END") || cursor != inputSize)
		return {ValidationError::InvalidReceiptText};
	return {};
}

ReceiptRecorder::ReceiptRecorder()
	: m_receipt(), m_nextFrame(1U), m_active(false), m_failed(false)
{
}

void ReceiptRecorder::reset()
{
	m_receipt = Receipt();
	m_nextFrame = 1U;
	m_active = false;
	m_failed = false;
}

bool ReceiptRecorder::begin(const SessionContract &session,
	std::uint64_t networkSessionToken, const char *actualExecutableSha256,
	const WorkerTelemetry &workerTelemetry)
{
	if (!beginQualification(session, networkSessionToken, actualExecutableSha256))
		return false;
	return publishWorkerTelemetry(workerTelemetry);
}

bool ReceiptRecorder::beginQualification(const SessionContract &session,
	std::uint64_t networkSessionToken, const char *actualExecutableSha256)
{
	reset();
	if (!IsValidSessionContract(session) || networkSessionToken == 0U ||
		actualExecutableSha256 == nullptr ||
		!IsCanonicalHex(actualExecutableSha256, kSha256HexChars) ||
		!EqualFixedText(session.executableSha256.data(), actualExecutableSha256,
			kSha256HexChars + 1U))
	{
		m_failed = true;
		return false;
	}
	m_receipt.session = session;
	m_receipt.networkSessionToken = networkSessionToken;
	m_receipt.executableOrigin = true;
	m_nextFrame = 1U;
	m_active = true;
	return true;
}

bool ReceiptRecorder::publishWorkerTelemetry(
	const WorkerTelemetry &workerTelemetry)
{
	if (!m_active || m_failed || m_receipt.validationAuthorityMask != 0U ||
		!IsWorkerTelemetryValid(workerTelemetry,
			m_receipt.session.provenKernelMask) ||
		!IsValidAIPlanningTelemetry(m_receipt.session,
			workerTelemetry.aiPlanning))
	{
		m_failed = true;
		return false;
	}
	m_receipt.validationAuthorityMask = workerTelemetry.authorityMask;
	m_receipt.workerTelemetryExecutableOrigin = workerTelemetry.executableOrigin;
	m_receipt.workerTelemetry = workerTelemetry.kernels;
	m_receipt.aiPlanning = workerTelemetry.aiPlanning;
	return true;
}

bool ReceiptRecorder::recordCommand(std::uint32_t frame,
	std::uint32_t originSlot, std::uint16_t commandId,
	std::uint64_t commandDigest)
{
	if (!m_active || frame == 0U || frame > m_receipt.session.commonStopFrame ||
		originSlot >= kMaxPeerCount || !IsBitSet(m_receipt.session.rosterMask, originSlot) ||
		commandDigest == 0U)
	{
		m_failed = true;
		return false;
	}
	PeerCommandContribution &contribution = m_receipt.contributions[originSlot];
	if (contribution.hasLastCommandId && contribution.lastCommandId == commandId)
	{
		if (contribution.lastCommandDigest == commandDigest)
			return true;
		m_failed = true;
		return false;
	}
	if (contribution.commandCount == 0U)
		contribution.firstCommandFrame = frame;
	contribution.lastCommandFrame = frame;
	contribution.lastCommandId = commandId;
	contribution.hasLastCommandId = true;
	contribution.lastCommandDigest = commandDigest;
	contribution.commandDigest = MixCommandDigest(contribution.commandDigest,
		frame, originSlot, commandId, commandDigest);
	++contribution.commandCount;
	m_receipt.contributedPeerMask |= 1U << originSlot;
	return true;
}

bool ReceiptRecorder::recordFrame(std::uint32_t frame,
	std::uint32_t crc, std::uint64_t commandDigest)
{
	if (!m_active || frame != m_nextFrame || frame == 0U ||
		frame > m_receipt.session.commonStopFrame)
	{
		m_failed = true;
		return false;
	}
	m_receipt.frameCount = frame;
	m_receipt.finalFrame = frame;
	if (frame == 1U || frame % kCheckpointStride == 0U ||
		frame == m_receipt.session.commonStopFrame)
	{
		if (m_receipt.checkpointCount >= kMaxCheckpoints)
		{
			m_failed = true;
			return false;
		}
		FrameCheckpoint &checkpoint = m_receipt.checkpoints[m_receipt.checkpointCount++];
		checkpoint.frame = frame;
		checkpoint.crc = crc;
		checkpoint.commandDigest = commandDigest == 0U ?
			ComputeReceiptCommandDigest(m_receipt) : commandDigest;
	}
	++m_nextFrame;
	return true;
}

bool ReceiptRecorder::finish(bool cleanShutdown,
	bool transportPathUsed, bool handshakeValidated)
{
	if (!m_active || m_nextFrame != m_receipt.session.commonStopFrame + 1U)
	{
		m_failed = true;
		return false;
	}
	m_receipt.transportPathUsed = transportPathUsed;
	m_receipt.handshakeValidated = handshakeValidated;
	m_receipt.cleanShutdown = cleanShutdown;
	m_active = false;
	const ValidationResult validation = ValidateReceipt(m_receipt,
		m_receipt.session, m_receipt.networkSessionToken,
		m_receipt.session.rosterMask,
		m_receipt.session.originMode == CommandOriginMode::TrustedRouter,
		m_receipt.validationAuthorityMask);
	if (!validation.ok())
	{
		m_failed = true;
		return false;
	}
	return true;
}

} // namespace lockstep_v2
} // namespace rts
