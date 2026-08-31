#include "Lib/ReplayFieldReader.h"
#include "Lib/RuntimeEpochContract.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{

class MemorySource
{
public:
	MemorySource(const void *data, int size)
		: m_data(static_cast<const unsigned char *>(data)), m_size(size), m_position(0)
	{
	}

	int read(void *destination, int size)
	{
		if (destination == nullptr || size < 0 || size > m_size - m_position)
		{
			return 0;
		}
		std::memcpy(destination, m_data + m_position, static_cast<std::size_t>(size));
		m_position += size;
		return size;
	}

	int readChar()
	{
		if (m_position >= m_size)
		{
			return EOF;
		}
		return m_data[m_position++];
	}

	int readWideChar()
	{
		wchar_t value = 0;
		return read(&value, sizeof(value)) == sizeof(value) ? static_cast<int>(value) : EOF;
	}

private:
	const unsigned char *m_data;
	int m_size;
	int m_position;
};

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int TestExactReads()
{
	const std::array<unsigned char, 4> bytes = {{0x10U, 0x20U, 0x30U, 0x40U}};
	MemorySource complete(bytes.data(), static_cast<int>(bytes.size()));
	std::array<unsigned char, 4> output = {{}};
	int result = Check(rts::replay::ReadExact(complete, output.data(), static_cast<int>(output.size())),
		"an exact fixed-width field reads successfully");
	result |= Check(output == bytes, "an exact fixed-width field preserves every byte");

	MemorySource truncated(bytes.data(), static_cast<int>(bytes.size() - 1U));
	result |= Check(!rts::replay::ReadExact(truncated, output.data(), static_cast<int>(output.size())),
		"a truncated fixed-width field is rejected");
	return result;
}

int TestAsciiStrings()
{
	const std::array<unsigned char, 4> valid = {{'R', 'T', 'S', 0U}};
	char output[4] = {};
	MemorySource validSource(valid.data(), static_cast<int>(valid.size()));
	int result = Check(rts::replay::ReadAsciiString(validSource, output, 4),
		"a terminated ASCII field reads successfully");
	result |= Check(std::strcmp(output, "RTS") == 0, "the ASCII field preserves its value");

	const std::array<unsigned char, 3> missingTerminator = {{'R', 'T', 'S'}};
	MemorySource unterminated(missingTerminator.data(), static_cast<int>(missingTerminator.size()));
	result |= Check(!rts::replay::ReadAsciiString(unterminated, output, 4),
		"EOF before an ASCII terminator is rejected");

	const std::array<unsigned char, 4> overCapacity = {{'R', 'T', 'S', 'X'}};
	MemorySource tooLong(overCapacity.data(), static_cast<int>(overCapacity.size()));
	result |= Check(!rts::replay::ReadAsciiString(tooLong, output, 4),
		"an ASCII field without room for a terminator is rejected");
	return result;
}

int TestWideStrings()
{
	const std::array<wchar_t, 4> valid = {{L'R', L'T', L'S', L'\0'}};
	wchar_t output[4] = {};
	MemorySource validSource(valid.data(), static_cast<int>(sizeof(valid)));
	int result = Check(rts::replay::ReadWideString(validSource, output, 4),
		"a terminated wide field reads successfully");
	result |= Check(std::wcscmp(output, L"RTS") == 0, "the wide field preserves its value");

	MemorySource truncated(valid.data(), static_cast<int>(sizeof(valid) - 1U));
	result |= Check(!rts::replay::ReadWideString(truncated, output, 4),
		"a partial wide character is rejected");

	const std::array<wchar_t, 3> missingTerminator = {{L'R', L'T', L'S'}};
	MemorySource unterminated(missingTerminator.data(), static_cast<int>(sizeof(missingTerminator)));
	result |= Check(!rts::replay::ReadWideString(unterminated, output, 4),
		"EOF before a wide terminator is rejected");
	return result;
}

int TestValidatedOuterContainerRejectsMalformedInnerField()
{
	using namespace rts::runtime_epoch;
	const std::array<Byte, 9> payload = {{'G', 'E', 'N', 'R', 'E', 'P', 'B', 'A', 'D'}};
	ReplayHeader header;
	header.payloadByteCount = payload.size();
	header.payloadChecksum = CalculatePayloadChecksum(payload.data(), payload.size());
	const std::array<Byte, kHeaderSize> encoded = Encode(header);
	ValidationOptions options;
	options.expectedSchemaVersion = kCurrentReplaySchemaVersion;
	ReplayHeader decoded;
	int result = Check(DecodeAndValidate(encoded.data(), encoded.size(), payload.data(), payload.size(),
		options, &decoded).ok(), "the outer replay container accepts its exact checked payload");

	MemorySource inner(payload.data(), static_cast<int>(payload.size()));
	char magic[6] = {};
	char malformed[4] = {};
	result |= Check(rts::replay::ReadExact(inner, magic, sizeof(magic)),
		"the inner GENREP magic remains readable");
	result |= Check(!rts::replay::ReadAsciiString(inner, malformed, 4),
		"a valid outer checksum cannot make an unterminated inner field valid");
	return result;
}

} // namespace

int main()
{
	return TestExactReads() | TestAsciiStrings() | TestWideStrings() |
		TestValidatedOuterContainerRejectsMalformedInnerField();
}
