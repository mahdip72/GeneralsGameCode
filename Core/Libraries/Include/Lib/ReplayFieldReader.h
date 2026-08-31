#pragma once

namespace rts
{
namespace replay
{

template <typename Source>
bool ReadExact(Source &source, void *destination, int byteCount)
{
	if (destination == 0 || byteCount <= 0)
	{
		return false;
	}
	return source.read(destination, byteCount) == byteCount;
}

template <typename Source, typename Character>
bool ReadTerminatedString(Source &source, Character *destination, int capacity)
{
	if (destination == 0 || capacity <= 0)
	{
		return false;
	}

	destination[0] = 0;
	for (int index = 0; index < capacity; ++index)
	{
		Character value = 0;
		if (!ReadExact(source, &value, static_cast<int>(sizeof(value))))
		{
			destination[0] = 0;
			return false;
		}
		destination[index] = value;
		if (value == 0)
		{
			return true;
		}
	}

	destination[capacity - 1] = 0;
	return false;
}

template <typename Source>
bool ReadAsciiString(Source &source, char *destination, int capacity)
{
	return ReadTerminatedString(source, destination, capacity);
}

template <typename Source, typename WideCharacter>
bool ReadWideString(Source &source, WideCharacter *destination, int capacity)
{
	return ReadTerminatedString(source, destination, capacity);
}

} // namespace replay
} // namespace rts
