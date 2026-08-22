/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

// WWSaveLoad used to serialize object-address remap tokens as raw pointers.
// The token is not dereferenced after loading; it is only an identity key for
// SaveLoadSystemClass pointer remapping.  Its width is therefore explicit:
// legacy assets use four bytes and x64-era output uses eight bytes.
constexpr std::uint32_t PERSIST_POINTER_TOKEN_LEGACY_SIZE = 4U;
constexpr std::uint32_t PERSIST_POINTER_TOKEN_CURRENT_SIZE = 8U;

inline bool Decode_Persist_Pointer_Token(const std::uint8_t *input,
	std::size_t input_size, std::uintptr_t *token)
{
	if (input == nullptr || token == nullptr)
	{
		return false;
	}

	if (input_size == PERSIST_POINTER_TOKEN_LEGACY_SIZE)
	{
		std::uint32_t legacy_token = 0U;
		for (std::size_t index = 0U; index < PERSIST_POINTER_TOKEN_LEGACY_SIZE; ++index)
		{
			legacy_token |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
		}
		*token = static_cast<std::uintptr_t>(legacy_token);
		return true;
	}

	if (input_size == PERSIST_POINTER_TOKEN_CURRENT_SIZE)
	{
		std::uint64_t current_token = 0U;
		for (std::size_t index = 0U; index < PERSIST_POINTER_TOKEN_CURRENT_SIZE; ++index)
		{
			current_token |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
		}
		if (current_token > static_cast<std::uint64_t>(
			std::numeric_limits<std::uintptr_t>::max()))
		{
			return false;
		}
		*token = static_cast<std::uintptr_t>(current_token);
		return true;
	}

	return false;
}

inline std::uint32_t Persist_Pointer_Token_Size()
{
	static_assert(sizeof(std::uintptr_t) == PERSIST_POINTER_TOKEN_LEGACY_SIZE ||
		sizeof(std::uintptr_t) == PERSIST_POINTER_TOKEN_CURRENT_SIZE,
		"WWSaveLoad supports only 32-bit and 64-bit pointer token widths");
	return static_cast<std::uint32_t>(sizeof(std::uintptr_t));
}

inline bool Encode_Persist_Pointer_Token(std::uintptr_t token,
	std::uint8_t *output, std::size_t output_size)
{
	if (output == nullptr)
	{
		return false;
	}

	if (Persist_Pointer_Token_Size() == PERSIST_POINTER_TOKEN_LEGACY_SIZE)
	{
		if (output_size < PERSIST_POINTER_TOKEN_LEGACY_SIZE)
		{
			return false;
		}
		const std::uint32_t legacy_token = static_cast<std::uint32_t>(token);
		for (std::size_t index = 0U; index < PERSIST_POINTER_TOKEN_LEGACY_SIZE; ++index)
		{
			output[index] = static_cast<std::uint8_t>(legacy_token >> (index * 8U));
		}
		return true;
	}

	if (output_size < PERSIST_POINTER_TOKEN_CURRENT_SIZE)
	{
		return false;
	}
	const std::uint64_t current_token = static_cast<std::uint64_t>(token);
	for (std::size_t index = 0U; index < PERSIST_POINTER_TOKEN_CURRENT_SIZE; ++index)
	{
		output[index] = static_cast<std::uint8_t>(current_token >> (index * 8U));
	}
	return true;
}
