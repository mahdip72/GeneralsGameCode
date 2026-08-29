/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
*/

#include "W3DDevice/Common/LegacyPixelShaderBytecode.h"

#include <stdio.h>

namespace
{
unsigned int Fnv1a(const LegacyPixelShaderBytecode::Stream &stream)
{
	unsigned int hash = 2166136261u;
	for (unsigned int i = 0; i < stream.word_count; ++i)
	{
		const unsigned int word = stream.words[i];
		for (unsigned int shift = 0; shift < 32; shift += 8)
		{
			hash ^= (word >> shift) & 0xffu;
			hash *= 16777619u;
		}
	}
	return hash;
}

bool Expect(LegacyPixelShaderBytecode::Program program,
	unsigned int expected_count, unsigned int expected_hash,
	unsigned int expected_version)
{
	const LegacyPixelShaderBytecode::Stream &stream =
		LegacyPixelShaderBytecode::Get(program);
	if (!LegacyPixelShaderBytecode::IsValid(stream) ||
		stream.word_count != expected_count ||
		stream.words[0] != expected_version ||
		Fnv1a(stream) != expected_hash)
	{
		printf("legacy shader %d failed validation (count=%u version=0x%08x hash=0x%08x)\n",
			static_cast<int>(program), stream.word_count,
			stream.words == 0 ? 0u : stream.words[0],
			Fnv1a(stream));
		return false;
	}
	return true;
}
}

int main()
{
	bool passed = true;
	passed = Expect(LegacyPixelShaderBytecode::WATER_RIVER,
		47, 0x3b780501u, 0xffff0101u) && passed;
	passed = Expect(LegacyPixelShaderBytecode::WATER_REFLECTION,
		31, 0x8ce97c93u, 0xffff0101u) && passed;
	passed = Expect(LegacyPixelShaderBytecode::WATER_TRAPEZOID,
		33, 0xdbd17704u, 0xffff0101u) && passed;
	passed = Expect(LegacyPixelShaderBytecode::PROFILER_SWIZZLE,
		38, 0x261ec30fu, 0xffff0104u) && passed;

	const LegacyPixelShaderBytecode::Stream &invalid =
		LegacyPixelShaderBytecode::Get(LegacyPixelShaderBytecode::PROGRAM_COUNT);
	passed = (!LegacyPixelShaderBytecode::IsValid(invalid)) && passed;
	return passed ? 0 : 1;
}
