/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "W3DDevice/Common/LegacyPixelShaderBytecode.h"

namespace
{
// These streams were generated from the corresponding legacy shader source
// with the legacy assembler.  The comment token is intentionally retained
// so the stream remains auditable with the original compiler output.
const unsigned int g_water_river[] =
{
	0xffff0101, 0x0009fffe, 0x58443344, 0x68532038,
	0x72656461, 0x73734120, 0x6c626d65, 0x56207265,
	0x69737265, 0x30206e6f, 0x0031392e, 0x00000042,
	0xb00f0000, 0x00000042, 0xb00f0001, 0x00000042,
	0xb00f0002, 0x00000042, 0xb00f0003, 0x00000005,
	0x80070000, 0x90e40000, 0xb0e40000, 0x00000001,
	0x80080000, 0xb0e40000, 0x00000005, 0x800f0001,
	0xb0e40001, 0xb0e40002, 0x00000002, 0x80070001,
	0x80e40001, 0xb0e40003, 0x00000005, 0x80070001,
	0x80e40001, 0x90ff0000, 0x40000005, 0x80080000,
	0x80e40000, 0xb0e40003, 0x00000002, 0x80070000,
	0x80e40000, 0x80e40001, 0x0000ffff
};

const unsigned int g_water_reflection[] =
{
	0xffff0101, 0x0009fffe, 0x58443344, 0x68532038,
	0x72656461, 0x73734120, 0x6c626d65, 0x56207265,
	0x69737265, 0x30206e6f, 0x0031392e, 0x00000042,
	0xb00f0000, 0x00000042, 0xb00f0001, 0x00000042,
	0xb00f0002, 0xb0e40001, 0x00000005, 0x800f0000,
	0x90e40000, 0xb0e40000, 0x00000005, 0x80070001,
	0xb0e40002, 0xa0e40000, 0x00000002, 0x80070000,
	0x80e40000, 0x80e40001, 0x0000ffff
};

const unsigned int g_water_trapezoid[] =
{
	0xffff0101, 0x0009fffe, 0x58443344, 0x68532038,
	0x72656461, 0x73734120, 0x6c626d65, 0x56207265,
	0x69737265, 0x30206e6f, 0x0031392e, 0x00000042,
	0xb00f0000, 0x00000042, 0xb00f0001, 0x00000042,
	0xb00f0002, 0x00000042, 0xb00f0003, 0x00000005,
	0x800f0000, 0x90e40000, 0xb0e40000, 0x00000004,
	0x80070000, 0xb0e40001, 0xb0e40002, 0x80e40000,
	0x00000005, 0x80070000, 0x80e40000, 0xb0e40003,
	0x0000ffff
};

const unsigned int g_profiler_swizzle[] =
{
	0xffff0104, 0x0009fffe, 0x58443344, 0x68532038,
	0x72656461, 0x73734120, 0x6c626d65, 0x56207265,
	0x69737265, 0x30206e6f, 0x0031392e, 0x00000042,
	0x800f0000, 0xb0e40000, 0x00000001, 0x80080001,
	0x80000000, 0x00000001, 0x80080002, 0x80550000,
	0x00000001, 0x80080003, 0x80aa0000, 0x00000005,
	0x80070000, 0x80ff0003, 0xa0e40000, 0x00000004,
	0x80070000, 0x80ff0002, 0xa0e40001, 0x80e40000,
	0x00000004, 0x80070000, 0x80ff0001, 0xa0e40002,
	0x80e40000, 0x0000ffff
};

const LegacyPixelShaderBytecode::Stream g_streams[] =
{
	{g_water_river, sizeof(g_water_river) / sizeof(g_water_river[0])},
	{g_water_reflection, sizeof(g_water_reflection) / sizeof(g_water_reflection[0])},
	{g_water_trapezoid, sizeof(g_water_trapezoid) / sizeof(g_water_trapezoid[0])},
	{g_profiler_swizzle, sizeof(g_profiler_swizzle) / sizeof(g_profiler_swizzle[0])}
};
}

namespace LegacyPixelShaderBytecode
{
const Stream &Get(Program program)
{
	static const Stream empty = {0, 0};
	if (program < WATER_RIVER || program >= PROGRAM_COUNT)
		return empty;
	return g_streams[program];
}

bool IsValid(const Stream &stream)
{
	if (stream.words == 0 || stream.word_count < 2)
		return false;

	const unsigned int version = stream.words[0];
	if ((version & 0xffff0000) != 0xffff0000 ||
		(version & 0x0000ffff) < 0x0101 ||
		(version & 0x0000ffff) > 0x0104)
		return false;

	return stream.words[stream.word_count - 1] == 0x0000ffff;
}
}
