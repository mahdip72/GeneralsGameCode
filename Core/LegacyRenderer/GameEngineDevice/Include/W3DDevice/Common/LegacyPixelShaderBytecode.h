/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

// The legacy ps_1.x instruction set is not accepted by the current
// d3dcompiler/fxc toolchain.  These immutable token streams are the output of
// the historical shader assembler for the small set of legacy shaders that are
// still needed by the differential lane.  Keeping the bytecode in the
// source removes the legacy runtime assembler dependency while retaining the
// original instruction set and device-facing behavior.
namespace LegacyPixelShaderBytecode
{
enum Program
{
	WATER_RIVER = 0,
	WATER_REFLECTION,
	WATER_TRAPEZOID,
	PROFILER_SWIZZLE,
	PROGRAM_COUNT
};

struct Stream
{
	const unsigned int *words;
	unsigned int word_count;
};

// Return an immutable legacy CreatePixelShader token stream.
const Stream &Get(Program program);

// Structural validation used by the focused test and defensive callers.
bool IsValid(const Stream &stream);
}
