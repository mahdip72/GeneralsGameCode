#include "Lib/RuntimeEpochContract.h"

#include <array>
#include <cstdio>

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

template <typename Header>
int CheckCommonHeader(const Header &actual, const Header &expected)
{
	int result = 0;
	result |= Check(actual.schemaVersion == expected.schemaVersion, "schema version round-trips");
	result |= Check(actual.engineEpoch == expected.engineEpoch, "engine epoch round-trips");
	result |= Check(actual.buildCompatibilityId == expected.buildCompatibilityId,
		"build compatibility ID round-trips");
	result |= Check(actual.contentHash == expected.contentHash, "content hash round-trips");
	result |= Check(actual.payloadByteCount == expected.payloadByteCount,
		"payload byte count round-trips");
	result |= Check(actual.payloadChecksum == expected.payloadChecksum,
		"payload checksum round-trips");
	return result;
}

int TestSaveHeader()
{
	using namespace rts::runtime_epoch;

	const std::array<Byte, 7> payload = {{0x00U, 0x01U, 0x7fU, 0x80U, 0xfeU, 0xffU, 0x42U}};
	SaveHeader expected;
	expected.buildCompatibilityId = UINT64_C(0x1122334455667788);
	expected.contentHash = UINT64_C(0x8877665544332211);
	expected.payloadByteCount = payload.size();
	expected.payloadChecksum = CalculatePayloadChecksum(payload.data(), payload.size());

	const std::array<Byte, kHeaderSize> encoded = Encode(expected);
	int result = 0;
	result |= Check(encoded[0] == 'S' && encoded[1] == 'A' && encoded[2] == 'V' && encoded[3] == '3',
		"save header has its independent magic");
	result |= Check(encoded[4] == 0x01U && encoded[5] == 0x00U && encoded[6] == 0x00U && encoded[7] == 0x00U,
		"schema version is little endian");
	result |= Check(encoded[12] == 0x88U && encoded[13] == 0x77U && encoded[14] == 0x66U &&
		encoded[15] == 0x55U && encoded[16] == 0x44U && encoded[17] == 0x33U &&
		encoded[18] == 0x22U && encoded[19] == 0x11U,
		"64-bit build ID is little endian");

	SaveHeader actual;
	result |= Check(Decode(encoded.data(), encoded.size(), &actual), "save header decodes");
	result |= CheckCommonHeader(actual, expected);

	ValidationOptions options;
	options.expectedBuildCompatibilityId = expected.buildCompatibilityId;
	options.expectedContentHash = expected.contentHash;
	options.requireBuildCompatibilityMatch = true;
	options.requireContentHashMatch = true;
	options.maxPayloadByteCount = 64U;
	result |= Check(Validate(actual, options).ok(), "save header validates against current contract");
	result |= Check(ValidatePayload(actual, payload.data(), payload.size()).ok(),
		"save payload validates against its declared checksum");

	SaveHeader combined;
	result |= Check(DecodeAndValidate(encoded.data(), encoded.size(), payload.data(), payload.size(),
		options, &combined).ok(), "combined save record validation succeeds");
	result |= CheckCommonHeader(combined, expected);

	std::array<Byte, kHeaderSize> wrongSchema = encoded;
	wrongSchema[4] = 0x02U;
	result |= Check(DecodeAndValidate(wrongSchema.data(), wrongSchema.size(), payload.data(), payload.size(),
		options, &combined).error == ValidationError::UnsupportedSchemaVersion,
		"combined validation rejects an unsupported schema");
	std::array<Byte, kHeaderSize> wrongEpochBytes = encoded;
	wrongEpochBytes[8] = 0x02U;
	result |= Check(DecodeAndValidate(wrongEpochBytes.data(), wrongEpochBytes.size(), payload.data(), payload.size(),
		options, &combined).error == ValidationError::UnsupportedEngineEpoch,
		"combined validation rejects an unsupported engine epoch");
	ValidationOptions wrongContent = options;
	wrongContent.expectedContentHash++;
	result |= Check(DecodeAndValidate(encoded.data(), encoded.size(), payload.data(), payload.size(),
		wrongContent, &combined).error == ValidationError::ContentHashMismatch,
		"combined validation rejects incompatible content");
	ValidationOptions payloadLimit = options;
	payloadLimit.maxPayloadByteCount = payload.size() - 1U;
	result |= Check(DecodeAndValidate(encoded.data(), encoded.size(), payload.data(), payload.size(),
		payloadLimit, &combined).error == ValidationError::PayloadTooLarge,
		"combined validation enforces the payload limit");

	std::array<Byte, 7> changedPayload = payload;
	changedPayload[3] ^= 0x01U;
	result |= Check(ValidatePayload(actual, changedPayload.data(), changedPayload.size()).error ==
		ValidationError::PayloadChecksumMismatch,
		"payload mutation is rejected by checksum validation");
	result |= Check(ValidatePayload(actual, payload.data(), payload.size() - 1U).error ==
		ValidationError::PayloadSizeMismatch,
		"truncated payload is rejected");
	result |= Check(DecodeAndValidate(encoded.data(), encoded.size(), changedPayload.data(), changedPayload.size(),
		options, &combined).error == ValidationError::PayloadChecksumMismatch,
		"combined validation rejects a changed payload");
	result |= Check(DecodeAndValidate(encoded.data(), encoded.size(), payload.data(), payload.size() - 1U,
		options, &combined).error == ValidationError::PayloadSizeMismatch,
		"combined validation rejects a truncated payload");
	result |= Check(Validate(actual, options).ok(), "valid header remains valid after payload checks");

	ValidationOptions wrongBuild = options;
	wrongBuild.expectedBuildCompatibilityId++;
	result |= Check(Validate(actual, wrongBuild).error == ValidationError::BuildCompatibilityMismatch,
		"incompatible build ID is rejected");
	ValidationOptions wrongEpoch = options;
	wrongEpoch.expectedEngineEpoch++;
	result |= Check(Validate(actual, wrongEpoch).error == ValidationError::UnsupportedEngineEpoch,
		"incompatible engine epoch is rejected");

	result |= Check(!Decode(encoded.data(), encoded.size() - 1U, &actual),
		"truncated header is rejected");
	std::array<Byte, kHeaderSize> badMagic = encoded;
	badMagic[0] = 'X';
	result |= Check(!Decode(badMagic.data(), badMagic.size(), &actual),
		"wrong header magic is rejected");
	result |= Check(DecodeAndValidate(badMagic.data(), badMagic.size(), payload.data(), payload.size(),
		options, &combined).error == ValidationError::InvalidMagic,
		"combined validation reports wrong header magic");
	result |= Check(DecodeAndValidate(nullptr, encoded.size(), payload.data(), payload.size(),
		options, &combined).error == ValidationError::NullInput,
		"combined validation reports null header input");
	result |= Check(!Encode(expected, nullptr, kHeaderSize), "null encode destination is rejected");
	result |= Check(!Encode(expected, badMagic.data(), kHeaderSize - 1U),
		"short encode destination is rejected");
	return result;
}

int TestReplayAndNetworkHeaders()
{
	using namespace rts::runtime_epoch;
	int result = 0;

	ReplayHeader replay;
	replay.schemaVersion = 3U;
	replay.engineEpoch = 7U;
	replay.buildCompatibilityId = UINT64_C(0x0102030405060708);
	replay.contentHash = UINT64_C(0x1020304050607080);
	replay.payloadByteCount = 0U;
	replay.payloadChecksum = CalculatePayloadChecksum(nullptr, 0U);
	const std::array<Byte, kHeaderSize> replayBytes = Encode(replay);
	ReplayHeader replayRoundTrip;
	result |= Check(Decode(replayBytes.data(), replayBytes.size(), &replayRoundTrip),
		"replay header decodes");
	result |= CheckCommonHeader(replayRoundTrip, replay);
	result |= Check(replayBytes[0] == 'R' && replayBytes[1] == 'P' && replayBytes[2] == 'L' &&
		replayBytes[3] == '3', "replay header has its independent magic");
	result |= Check(!Decode(replayBytes.data(), replayBytes.size(),
		static_cast<SaveHeader *>(nullptr)), "null save decode destination is rejected");

	NetworkHello hello;
	hello.buildCompatibilityId = replay.buildCompatibilityId;
	hello.contentHash = replay.contentHash;
	const std::array<Byte, kHeaderSize> helloBytes = Encode(hello);
	NetworkHello helloRoundTrip;
	result |= Check(Decode(helloBytes.data(), helloBytes.size(), &helloRoundTrip),
		"network hello decodes");
	result |= CheckCommonHeader(helloRoundTrip, hello);
	result |= Check(helloBytes[0] == 'N' && helloBytes[1] == 'E' && helloBytes[2] == 'T' &&
		helloBytes[3] == '3', "network hello has its independent magic");
	result |= Check(!Decode(helloBytes.data(), helloBytes.size(), &replayRoundTrip),
		"network hello cannot be decoded as replay header");
	result |= Check(ValidatePayload(helloRoundTrip, nullptr, 0U).ok(),
		"empty network hello payload validates");
	return result;
}

} // namespace

int main()
{
	static_assert(rts::runtime_epoch::kHeaderSize == 40U, "runtime epoch wire size is contractual");
	return TestSaveHeader() | TestReplayAndNetworkHeaders();
}
