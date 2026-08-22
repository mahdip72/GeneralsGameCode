#include "Utility/CppMacros.h"
#include "WWLib/RAMFILE.h"
#include "WWLib/chunkio.h"

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

} // namespace

int main()
{
	return TestValueReads();
}
