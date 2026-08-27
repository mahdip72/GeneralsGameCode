#include "Utility/CppMacros.h"
#include "WWLib/RAMFILE.h"
#include "WWLib/chunkio.h"
#include "WWSaveLoad/persistfactory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

template <typename Value>
int CheckValueRead(const Value &expected, const char *message)
{
	std::array<std::uint8_t, sizeof(ChunkHeader) + sizeof(Value)> bytes = {{}};
	const ChunkHeader header(0x12345678U, static_cast<uint32>(sizeof(Value)));
	std::memcpy(bytes.data(), &header, sizeof(header));
	std::memcpy(bytes.data() + sizeof(header), &expected, sizeof(expected));

	RAMFileClass file(bytes.data(), static_cast<int>(bytes.size()));
	int result = 0;
	result |= Check(file.Open(FileClass::READ), "RAM file opens for read");
	ChunkLoadClass load(&file);
	result |= Check(load.Open_Chunk(), "value chunk opens");
	Value actual = {};
	result |= Check(load.Read(&actual) == sizeof(Value), message);
	result |= Check(std::memcmp(&actual, &expected, sizeof(Value)) == 0,
		"value payload is read in full");
	result |= Check(load.Close_Chunk(), "value chunk closes");
	file.Close();
	return result;
}

int TestValueReads()
{
	int result = 0;
	const IOVector2Struct vector2 = {1.0f, -2.0f};
	const IOVector3Struct vector3 = {3.0f, -4.0f, 5.0f};
	const IOVector4Struct vector4 = {6.0f, -7.0f, 8.0f, -9.0f};
	const IOQuaternionStruct quaternion = {{10.0f, -11.0f, 12.0f, -13.0f}};
	result |= CheckValueRead(vector2, "two-component vector read uses its value size");
	result |= CheckValueRead(vector3, "three-component vector read uses its value size");
	result |= CheckValueRead(vector4, "four-component vector read uses its value size");
	result |= CheckValueRead(quaternion, "quaternion read uses its value size");
	return result;
}

int TestRejectedPersistChunkBalancesDepth()
{
	constexpr uint32 outerId = 0x10203040U;
	constexpr uint32 unexpectedId = 0x50607080U;
	constexpr uint32 expectedId = 0x90a0b0c0U;
	std::array<std::uint8_t, sizeof(ChunkHeader) * 3U> bytes = {{}};
	ChunkHeader outer(outerId, static_cast<uint32>(sizeof(ChunkHeader) * 2U));
	outer.Set_Sub_Chunk_Flag(true);
	const ChunkHeader unexpected(unexpectedId, 0U);
	const ChunkHeader expected(expectedId, 0U);
	std::memcpy(bytes.data(), &outer, sizeof(outer));
	std::memcpy(bytes.data() + sizeof(outer), &unexpected, sizeof(unexpected));
	std::memcpy(bytes.data() + sizeof(outer) + sizeof(unexpected), &expected, sizeof(expected));

	RAMFileClass file(bytes.data(), static_cast<int>(bytes.size()));
	int result = 0;
	result |= Check(file.Open(FileClass::READ), "persist chunk fixture opens for read");
	ChunkLoadClass load(&file);
	result |= Check(load.Open_Chunk() && load.Cur_Chunk_ID() == outerId,
		"persist parent chunk opens");
	result |= Check(!PersistFactoryDetail::Open_Expected_Chunk(load, expectedId),
		"unexpected persist child is rejected");
	result |= Check(load.Cur_Chunk_Depth() == 1,
		"rejected persist child restores its parent depth");
	result |= Check(PersistFactoryDetail::Open_Expected_Chunk(load, expectedId),
		"expected sibling remains readable after malformed child");
	result |= Check(load.Close_Chunk(), "expected persist child closes");
	result |= Check(load.Close_Chunk(), "persist parent chunk closes");
	result |= Check(load.Cur_Chunk_Depth() == 0,
		"persist chunk fixture leaves the stack balanced");
	file.Close();
	return result;
}

} // namespace

int main()
{
	return TestValueReads() | TestRejectedPersistChunkBalancesDepth();
}
