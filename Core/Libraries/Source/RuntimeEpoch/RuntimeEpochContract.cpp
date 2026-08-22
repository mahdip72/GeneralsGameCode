#include "Lib/RuntimeEpochContract.h"

#include <limits>

namespace rts
{
namespace runtime_epoch
{
namespace
{

template <typename Unsigned>
void WriteLittleEndian(Byte *destination, Unsigned value)
{
	for (std::size_t index = 0; index < sizeof(Unsigned); ++index)
	{
		destination[index] = static_cast<Byte>(value & static_cast<Unsigned>(0xffU));
		value >>= 8U;
	}
}

template <typename Unsigned>
Unsigned ReadLittleEndian(const Byte *source)
{
	Unsigned value = 0U;
	for (std::size_t index = 0; index < sizeof(Unsigned); ++index)
	{
		value |= static_cast<Unsigned>(source[index]) << (index * 8U);
	}
	return value;
}

bool HasMagic(const Byte *input, const std::array<Byte, 4> &magic)
{
	for (std::size_t index = 0; index < magic.size(); ++index)
	{
		if (input[index] != magic[index])
		{
			return false;
		}
	}
	return true;
}

template <typename Header>
ValidationResult ValidateFields(const Header &header, const ValidationOptions &options);

template <typename Header>
ValidationResult ValidatePayloadFields(const Header &header,
	const Byte *payload,
	std::size_t payloadSize);

template <typename Header>
bool EncodeFields(const std::array<Byte, 4> &magic,
	const Header &header,
	Byte *output,
	std::size_t outputSize)
{
	if (output == nullptr || outputSize < kHeaderSize)
	{
		return false;
	}

	for (std::size_t index = 0; index < magic.size(); ++index)
	{
		output[index] = magic[index];
	}
	WriteLittleEndian<std::uint32_t>(output + 4U, header.schemaVersion);
	WriteLittleEndian<std::uint32_t>(output + 8U, header.engineEpoch);
	WriteLittleEndian<std::uint64_t>(output + 12U, header.buildCompatibilityId);
	WriteLittleEndian<std::uint64_t>(output + 20U, header.contentHash);
	WriteLittleEndian<std::uint64_t>(output + 28U, header.payloadByteCount);
	WriteLittleEndian<std::uint32_t>(output + 36U, header.payloadChecksum);
	return true;
}

template <typename Header>
bool DecodeFields(const std::array<Byte, 4> &magic,
	const Byte *input,
	std::size_t inputSize,
	Header *header)
{
	if (input == nullptr || header == nullptr || inputSize != kHeaderSize ||
		!HasMagic(input, magic))
	{
		return false;
	}

	header->schemaVersion = ReadLittleEndian<std::uint32_t>(input + 4U);
	header->engineEpoch = ReadLittleEndian<std::uint32_t>(input + 8U);
	header->buildCompatibilityId = ReadLittleEndian<std::uint64_t>(input + 12U);
	header->contentHash = ReadLittleEndian<std::uint64_t>(input + 20U);
	header->payloadByteCount = ReadLittleEndian<std::uint64_t>(input + 28U);
	header->payloadChecksum = ReadLittleEndian<std::uint32_t>(input + 36U);
	return true;
}

template <typename Header>
ValidationResult DecodeAndValidateFields(const std::array<Byte, 4> &magic,
	const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	Header *header)
{
	if (input == nullptr || header == nullptr)
	{
		return {ValidationError::NullInput};
	}
	if (inputSize != kHeaderSize)
	{
		return {ValidationError::InvalidSize};
	}
	if (!HasMagic(input, magic))
	{
		return {ValidationError::InvalidMagic};
	}
	if (!DecodeFields(magic, input, inputSize, header))
	{
		return {ValidationError::InvalidSize};
	}

	const ValidationResult headerResult = ValidateFields(*header, options);
	if (!headerResult.ok())
	{
		return headerResult;
	}
	return ValidatePayloadFields(*header, payload, payloadSize);
}

template <typename Header>
ValidationResult ValidateFields(const Header &header, const ValidationOptions &options)
{
	if (header.schemaVersion != options.expectedSchemaVersion)
	{
		return {ValidationError::UnsupportedSchemaVersion};
	}
	if (header.engineEpoch != options.expectedEngineEpoch)
	{
		return {ValidationError::UnsupportedEngineEpoch};
	}
	if (options.requireBuildCompatibilityMatch &&
		header.buildCompatibilityId != options.expectedBuildCompatibilityId)
	{
		return {ValidationError::BuildCompatibilityMismatch};
	}
	if (options.requireContentHashMatch && header.contentHash != options.expectedContentHash)
	{
		return {ValidationError::ContentHashMismatch};
	}
	if (header.payloadByteCount > options.maxPayloadByteCount)
	{
		return {ValidationError::PayloadTooLarge};
	}
	return {};
}

template <typename Header>
ValidationResult ValidatePayloadFields(const Header &header,
	const Byte *payload,
	std::size_t payloadSize)
{
	if (header.payloadByteCount >
		static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
	{
		return {ValidationError::PayloadTooLarge};
	}
	if (payloadSize != static_cast<std::size_t>(header.payloadByteCount))
	{
		return {ValidationError::PayloadSizeMismatch};
	}
	if (payload == nullptr && payloadSize != 0U)
	{
		return {ValidationError::NullInput};
	}
	if (CalculatePayloadChecksum(payload, payloadSize) != header.payloadChecksum)
	{
		return {ValidationError::PayloadChecksumMismatch};
	}
	return {};
}

} // namespace

std::uint32_t CalculatePayloadChecksum(const Byte *payload, std::size_t payloadSize)
{
	if (payload == nullptr && payloadSize != 0U)
	{
		return 0U;
	}

	std::uint32_t checksum = 0xffffffffU;
	for (std::size_t index = 0; index < payloadSize; ++index)
	{
		checksum ^= payload[index];
		for (unsigned bit = 0U; bit < 8U; ++bit)
		{
			const std::uint32_t mask = 0U - (checksum & 1U);
			checksum = (checksum >> 1U) ^ (0xedb88320U & mask);
		}
	}
	return ~checksum;
}

ValidationResult Validate(const SaveHeader &header, const ValidationOptions &options)
{
	return ValidateFields(header, options);
}

ValidationResult Validate(const ReplayHeader &header, const ValidationOptions &options)
{
	return ValidateFields(header, options);
}

ValidationResult Validate(const NetworkHello &header, const ValidationOptions &options)
{
	return ValidateFields(header, options);
}

ValidationResult ValidatePayload(const SaveHeader &header, const Byte *payload, std::size_t payloadSize)
{
	return ValidatePayloadFields(header, payload, payloadSize);
}

ValidationResult ValidatePayload(const ReplayHeader &header, const Byte *payload, std::size_t payloadSize)
{
	return ValidatePayloadFields(header, payload, payloadSize);
}

ValidationResult ValidatePayload(const NetworkHello &header, const Byte *payload, std::size_t payloadSize)
{
	return ValidatePayloadFields(header, payload, payloadSize);
}

bool Encode(const SaveHeader &header, Byte *output, std::size_t outputSize)
{
	return EncodeFields(kSaveMagic, header, output, outputSize);
}

bool Encode(const ReplayHeader &header, Byte *output, std::size_t outputSize)
{
	return EncodeFields(kReplayMagic, header, output, outputSize);
}

bool Encode(const NetworkHello &header, Byte *output, std::size_t outputSize)
{
	return EncodeFields(kNetworkMagic, header, output, outputSize);
}

bool Decode(const Byte *input, std::size_t inputSize, SaveHeader *header)
{
	return DecodeFields(kSaveMagic, input, inputSize, header);
}

bool Decode(const Byte *input, std::size_t inputSize, ReplayHeader *header)
{
	return DecodeFields(kReplayMagic, input, inputSize, header);
}

bool Decode(const Byte *input, std::size_t inputSize, NetworkHello *header)
{
	return DecodeFields(kNetworkMagic, input, inputSize, header);
}

ValidationResult DecodeAndValidate(const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	SaveHeader *header)
{
	return DecodeAndValidateFields(kSaveMagic, input, inputSize, payload, payloadSize, options, header);
}

ValidationResult DecodeAndValidate(const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	ReplayHeader *header)
{
	return DecodeAndValidateFields(kReplayMagic, input, inputSize, payload, payloadSize, options, header);
}

ValidationResult DecodeAndValidate(const Byte *input,
	std::size_t inputSize,
	const Byte *payload,
	std::size_t payloadSize,
	const ValidationOptions &options,
	NetworkHello *header)
{
	return DecodeAndValidateFields(kNetworkMagic, input, inputSize, payload, payloadSize, options, header);
}

} // namespace runtime_epoch
} // namespace rts
