#include "WWSaveLoad/pointertoken.h"

#include <array>
#include <cstdint>
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

int TestPointerTokens()
{
	int result = 0;
	std::uintptr_t token = 0U;
	const std::array<std::uint8_t, 4> legacy = {{0x78U, 0x56U, 0x34U, 0x12U}};
	result |= Check(Decode_Persist_Pointer_Token(legacy.data(), legacy.size(), &token),
		"four-byte legacy token decodes");
	result |= Check(token == static_cast<std::uintptr_t>(UINT32_C(0x12345678)),
		"legacy token is zero extended");

	const std::array<std::uint8_t, 8> high_address_token = {{
		0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U}};
	if constexpr (sizeof(std::uintptr_t) == PERSIST_POINTER_TOKEN_CURRENT_SIZE)
	{
		result |= Check(Decode_Persist_Pointer_Token(high_address_token.data(),
			high_address_token.size(), &token), "eight-byte current token decodes");
		result |= Check(token == static_cast<std::uintptr_t>(UINT64_C(0x1122334455667788)),
			"current token preserves high address bits when representable");
	}
	else
	{
		result |= Check(!Decode_Persist_Pointer_Token(high_address_token.data(),
			high_address_token.size(), &token),
			"nonrepresentable eight-byte token is rejected on 32-bit");
	}

	const std::array<std::uint8_t, 8> representable_current_token = {{
		0x78U, 0x56U, 0x34U, 0x12U, 0x00U, 0x00U, 0x00U, 0x00U}};
	result |= Check(Decode_Persist_Pointer_Token(representable_current_token.data(),
		representable_current_token.size(), &token),
		"representable eight-byte current token decodes");
	result |= Check(token == static_cast<std::uintptr_t>(UINT32_C(0x12345678)),
		"representable current token preserves its value");

	std::array<std::uint8_t, 8> encoded;
	encoded.fill(0xccU);
	std::uintptr_t value_to_encode = 0U;
	if constexpr (sizeof(std::uintptr_t) == PERSIST_POINTER_TOKEN_CURRENT_SIZE)
		value_to_encode = static_cast<std::uintptr_t>(UINT64_C(0x1122334455667788));
	else
		value_to_encode = static_cast<std::uintptr_t>(UINT32_C(0x12345678));
	result |= Check(Encode_Persist_Pointer_Token(
		value_to_encode, encoded.data(), encoded.size()), "current token encodes");
	if constexpr (sizeof(std::uintptr_t) == PERSIST_POINTER_TOKEN_CURRENT_SIZE)
	{
		result |= Check(encoded == high_address_token, "current token uses little-endian bytes");
	}
	else
	{
		result |= Check(encoded[0] == 0x78U && encoded[1] == 0x56U &&
			encoded[2] == 0x34U && encoded[3] == 0x12U && encoded[4] == 0xccU &&
			encoded[5] == 0xccU && encoded[6] == 0xccU && encoded[7] == 0xccU,
			"legacy token uses four little-endian bytes without overwriting the tail");
	}
	result |= Check(!Encode_Persist_Pointer_Token(token, nullptr, encoded.size()),
		"null token destination is rejected");
	result |= Check(!Encode_Persist_Pointer_Token(token, encoded.data(),
		Persist_Pointer_Token_Size() - 1U),
		"short token destination is rejected");

	const std::array<std::uint8_t, 6> malformed = {{0U, 1U, 2U, 3U, 4U, 5U}};
	result |= Check(!Decode_Persist_Pointer_Token(malformed.data(), malformed.size(), &token),
		"unexpected token length is rejected");
	result |= Check(!Decode_Persist_Pointer_Token(nullptr, legacy.size(), &token),
		"null token input is rejected");
	result |= Check(!Decode_Persist_Pointer_Token(legacy.data(), legacy.size(), nullptr),
		"null token output is rejected");
	return result;
}

} // namespace

int main()
{
	return TestPointerTokens();
}
