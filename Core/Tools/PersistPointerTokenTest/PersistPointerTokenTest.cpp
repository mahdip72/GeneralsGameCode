#include "Utility/CppMacros.h"
#include "WWSaveLoad/pointertoken.h"
#if defined(_WIN32)
#include "WWLib/RAMFILE.h"
#include "WWLib/chunkio.h"
#include "WWSaveLoad/persistfactory.h"
#include "WWSaveLoad/persist.h"
#endif

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

#if defined(_WIN32)

constexpr int PERSIST_FACTORY_TEST_CHUNK_ID = 0x7f000001;

class PersistFactoryRoundTripObject : public PersistClass
{
public:
	explicit PersistFactoryRoundTripObject(std::uint32_t value = 0U) : Value(value) {}

	virtual const PersistFactoryClass &Get_Factory() const override;

	virtual bool Save(ChunkSaveClass &csave) override
	{
		return csave.Write(&Value, static_cast<uint32>(sizeof(Value))) == sizeof(Value);
	}

	virtual bool Load(ChunkLoadClass &cload) override
	{
		return cload.Read(&Value, static_cast<uint32>(sizeof(Value))) == sizeof(Value);
	}

	std::uint32_t Get_Value() const { return Value; }

private:
	std::uint32_t Value;
};

typedef SimplePersistFactoryClass<PersistFactoryRoundTripObject, PERSIST_FACTORY_TEST_CHUNK_ID>
	PersistFactoryRoundTripFactoryClass;
PersistFactoryRoundTripFactoryClass PersistFactoryRoundTripFactory;

const PersistFactoryClass &PersistFactoryRoundTripObject::Get_Factory() const
{
	return PersistFactoryRoundTripFactory;
}

int TestPersistFactoryRoundTrip()
{
	std::array<std::uint8_t, 128> storage = {{}};
	RAMFileClass file(storage.data(), static_cast<int>(storage.size()));
	PersistFactoryRoundTripObject source(UINT32_C(0x1234abcd));
	const PersistFactoryClass &factory = source.Get_Factory();
	int result = 0;

	result |= Check(file.Open(FileClass::WRITE), "persist factory RAM file opens for write");
	ChunkSaveClass save(&file);
	result |= Check(save.Begin_Chunk(factory.Chunk_ID()),
		"production persist factory parent chunk opens");
	factory.Save(save, &source);
	result |= Check(save.End_Chunk(), "production persist factory parent chunk closes");
	file.Close();
	result |= Check(file.Size() > 0, "persist factory writes a production object");

	result |= Check(file.Open(FileClass::READ), "persist factory RAM file reopens for token inspection");
	ChunkLoadClass token_load(&file);
	std::array<std::uint8_t, PERSIST_POINTER_TOKEN_CURRENT_SIZE> encoded_token_bytes = {{}};
	const uint32 token_size = Persist_Pointer_Token_Size();
	result |= Check(token_load.Open_Chunk() && token_load.Cur_Chunk_ID() == factory.Chunk_ID(),
		"production persist factory parent chunk has the expected id");
	result |= Check(token_load.Open_Chunk(), "production pointer chunk opens");
	result |= Check(token_load.Cur_Chunk_ID() ==
		PersistFactoryRoundTripFactoryClass::SIMPLEFACTORY_CHUNKID_OBJPOINTER,
		"production pointer chunk has the expected id");
	result |= Check(token_load.Cur_Chunk_Length() == token_size,
		"production pointer chunk uses the active token width");
	result |= Check(token_load.Read(encoded_token_bytes.data(), token_size) == token_size,
		"production pointer token reads in full");
	result |= Check(token_load.Close_Chunk(), "production pointer chunk closes");
	result |= Check(token_load.Close_Chunk(), "production persist factory parent chunk closes after inspection");
	file.Close();

	std::uintptr_t encoded_token = 0U;
	result |= Check(Decode_Persist_Pointer_Token(encoded_token_bytes.data(), token_size,
		&encoded_token), "production template writes a decodable pointer token");
	result |= Check(encoded_token == reinterpret_cast<std::uintptr_t>(&source),
		"production template preserves the source object identity token");

	result |= Check(file.Open(FileClass::READ), "persist factory RAM file reopens for round trip");
	ChunkLoadClass load(&file);
	result |= Check(load.Open_Chunk() && load.Cur_Chunk_ID() == factory.Chunk_ID(),
		"production persist factory parent chunk opens for round trip");
	PersistClass *loaded_base = factory.Load(load);
	result |= Check(loaded_base != nullptr, "production persist factory loads an object");
	if (loaded_base != nullptr) {
		PersistFactoryRoundTripObject *loaded =
			static_cast<PersistFactoryRoundTripObject *>(loaded_base);
		result |= Check(loaded->Get_Value() == source.Get_Value(),
			"production persist factory preserves object data");
		delete loaded;
	}
	result |= Check(load.Close_Chunk(),
		"production persist factory parent chunk closes after round trip");
	result |= Check(load.Cur_Chunk_Depth() == 0,
		"production persist factory leaves the load stack balanced");
	file.Close();
	return result;
}

#endif // defined(_WIN32)

} // namespace

int main()
{
	int result = TestPointerTokens();
#if defined(_WIN32)
	result |= TestPersistFactoryRoundTrip();
#endif
	return result;
}
