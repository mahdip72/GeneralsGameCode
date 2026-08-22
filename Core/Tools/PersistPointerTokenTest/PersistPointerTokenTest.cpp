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

	const std::array<std::uint8_t, 8> native = {{
		0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U}};
	result |= Check(Decode_Persist_Pointer_Token(native.data(), native.size(), &token),
		"eight-byte current token decodes");
	result |= Check(token == static_cast<std::uintptr_t>(UINT64_C(0x1122334455667788)),
		"current token preserves high address bits when representable");

	std::array<std::uint8_t, 8> encoded = {{}};
	result |= Check(Encode_Persist_Pointer_Token(
		static_cast<std::uintptr_t>(UINT64_C(0x1122334455667788)), encoded.data(), encoded.size()),
		"current token encodes");
	result |= Check(encoded == native, "current token uses little-endian bytes");
	result |= Check(!Encode_Persist_Pointer_Token(token, nullptr, encoded.size()),
		"null token destination is rejected");
	result |= Check(!Encode_Persist_Pointer_Token(token, encoded.data(), encoded.size() - 1U),
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
