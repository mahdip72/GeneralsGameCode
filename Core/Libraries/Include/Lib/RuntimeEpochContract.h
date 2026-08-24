#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rts
{
namespace runtime_epoch
{

using Byte = std::uint8_t;

// These values are part of the new x64 runtime epoch.  They are intentionally
// independent from legacy in-memory headers and may be changed only as part
// of a deliberate format/compatibility revision.
constexpr std::uint32_t kCurrentSchemaVersion = 1U;
constexpr std::uint32_t kCurrentEngineEpoch = 1U;
constexpr std::size_t kHeaderSize = 40U;

constexpr std::array<Byte, 4> kSaveMagic = {{'S', 'A', 'V', '3'}};
constexpr std::array<Byte, 4> kReplayMagic = {{'R', 'P', 'L', '3'}};
constexpr std::array<Byte, 4> kNetworkMagic = {{'N', 'E', 'T', '3'}};

// The product still exposes its executable and important-INI identities as
// explicit 32-bit CRCs.  The new wire contract stores those values in
// pointer-independent 64-bit slots so the container layout is fixed on every
// supported build.
constexpr std::uint64_t BuildCompatibilityIdFromExecutableCrc(std::uint32_t executableCrc)
{
	return static_cast<std::uint64_t>(executableCrc);
}

constexpr std::uint64_t ContentHashFromIniCrc(std::uint32_t iniCrc)
{
	return static_cast<std::uint64_t>(iniCrc);
}

struct SaveHeader
{
	std::uint32_t schemaVersion = kCurrentSchemaVersion;
	std::uint32_t engineEpoch = kCurrentEngineEpoch;
	std::uint64_t buildCompatibilityId = 0U;
	std::uint64_t contentHash = 0U;
	std::uint64_t payloadByteCount = 0U;
	std::uint32_t payloadChecksum = 0U;
};

struct ReplayHeader
{
	std::uint32_t schemaVersion = kCurrentSchemaVersion;
	std::uint32_t engineEpoch = kCurrentEngineEpoch;
	std::uint64_t buildCompatibilityId = 0U;
	std::uint64_t contentHash = 0U;
	std::uint64_t payloadByteCount = 0U;
	std::uint32_t payloadChecksum = 0U;
};

struct NetworkHello
{
	std::uint32_t schemaVersion = kCurrentSchemaVersion;
	std::uint32_t engineEpoch = kCurrentEngineEpoch;
	std::uint64_t buildCompatibilityId = 0U;
	std::uint64_t contentHash = 0U;
	std::uint64_t payloadByteCount = 0U;
	std::uint32_t payloadChecksum = 0U;
};

struct ValidationOptions
{
	std::uint32_t expectedSchemaVersion = kCurrentSchemaVersion;
	std::uint32_t expectedEngineEpoch = kCurrentEngineEpoch;
	std::uint64_t expectedBuildCompatibilityId = 0U;
	std::uint64_t expectedContentHash = 0U;
	std::uint64_t maxPayloadByteCount = UINT64_MAX;
	bool requireBuildCompatibilityMatch = false;
	bool requireContentHashMatch = false;
};

enum class ValidationError : std::uint8_t
{
	None = 0,
	NullInput,
	InvalidSize,
	InvalidMagic,
	UnsupportedSchemaVersion,
	UnsupportedEngineEpoch,
	BuildCompatibilityMismatch,
	ContentHashMismatch,
	PayloadSizeMismatch,
	PayloadTooLarge,
	PayloadChecksumMismatch,
	NetworkIdentityMismatch,
	NetworkKindMismatch,
	NetworkSessionMismatch
};

struct ValidationResult
{
	ValidationError error = ValidationError::None;

	constexpr bool ok() const
	{
		return error == ValidationError::None;
	}
};

// The checksum is CRC-32/ISO-HDLC over the payload bytes.  It is deliberately
// defined here rather than relying on a compiler, platform, or object layout.
class PayloadChecksumAccumulator
{
public:
	PayloadChecksumAccumulator();

	void update(const Byte *payload, std::size_t payloadSize);
	std::uint32_t finish() const;
	std::uint64_t byteCount() const { return m_byteCount; }

private:
	std::uint32_t m_crc;
	std::uint64_t m_byteCount;
};

std::uint32_t CalculatePayloadChecksum(const Byte *payload, std::size_t payloadSize);

ValidationResult Validate(const SaveHeader &header, const ValidationOptions &options);
ValidationResult Validate(const ReplayHeader &header, const ValidationOptions &options);
ValidationResult Validate(const NetworkHello &header, const ValidationOptions &options);

ValidationResult ValidatePayload(const SaveHeader &header, const Byte *payload, std::size_t payloadSize);
ValidationResult ValidatePayload(const ReplayHeader &header, const Byte *payload, std::size_t payloadSize);
ValidationResult ValidatePayload(const NetworkHello &header, const Byte *payload, std::size_t payloadSize);

bool Encode(const SaveHeader &header, Byte *output, std::size_t outputSize);
bool Encode(const ReplayHeader &header, Byte *output, std::size_t outputSize);
bool Encode(const NetworkHello &header, Byte *output, std::size_t outputSize);

bool Decode(const Byte *input, std::size_t inputSize, SaveHeader *header);
bool Decode(const Byte *input, std::size_t inputSize, ReplayHeader *header);
bool Decode(const Byte *input, std::size_t inputSize, NetworkHello *header);

// Decode and validate a complete record in one operation.  The result
// includes malformed-wire errors as well as schema, epoch, compatibility,
// size, and checksum failures so callers cannot accidentally use a decoded
// header without applying the full contract.
ValidationResult DecodeAndValidate(const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	SaveHeader *header);
ValidationResult DecodeAndValidate(const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	ReplayHeader *header);
ValidationResult DecodeAndValidate(const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	NetworkHello *header);

template <typename Header>
std::array<Byte, kHeaderSize> Encode(const Header &header)
{
	std::array<Byte, kHeaderSize> bytes = {{}};
	Encode(header, bytes.data(), bytes.size());
	return bytes;
}

} // namespace runtime_epoch
} // namespace rts
