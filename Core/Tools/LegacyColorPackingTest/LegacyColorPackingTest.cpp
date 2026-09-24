#include "Renderer/LegacyColorPacking.h"
#include "Renderer/PointGroupColorPacking.h"

#include <math.h>
#include <stdio.h>

#if defined(_MSC_VER)
#include <float.h>
#endif

namespace
{
	struct PointColor
	{
		float channels[4];

		float operator[](int index) const
		{
			return channels[index];
		}
	};

	bool Near(float first, float second)
	{
		return fabs(first - second) <= 1.0e-6f;
	}

	bool Check(bool condition, const char *description)
	{
		if (!condition) {
			fprintf(stderr, "LegacyColorPackingTest failed: %s\n", description);
		}
		return condition;
	}

	// Use explicit IEEE-754 bit patterns instead of division by zero so this
	// policy test remains deterministic and warning-free under VC6 as well as
	// current MSVC.  These values exercise sanitization policy only; they are
	// not inputs for a legacy bit-parity claim.
	float FloatFromBits(unsigned int bits)
	{
		union FloatBits
		{
			unsigned int bits;
			float value;
		} raw;
		raw.bits = bits;
		return raw.value;
	}

	bool TestPointGroupColorPacking()
	{
		enum { POINT_COUNT = 700, MAX_VERTICES = POINT_COUNT * 4,
			MAX_VB_SIZE = 2048 };
		PointColor pointColors[POINT_COUNT];
		PointColor expandedColors[MAX_VERTICES];
		unsigned int reference[MAX_VERTICES];
		int point;
		for (point = 0; point < POINT_COUNT; ++point)
		{
			pointColors[point].channels[0] =
				static_cast<float>(point % 17) / 16.0f;
			pointColors[point].channels[1] =
				static_cast<float>((point + 3) % 13) / 12.0f;
			pointColors[point].channels[2] =
				static_cast<float>((point + 5) % 11) / 10.0f;
			pointColors[point].channels[3] =
				static_cast<float>((point + 7) % 9) / 8.0f;
		}

		bool passed = true;
		const int verticesPerPointValues[] = {3, 4};
		int mode;
		for (mode = 0; mode < 2; ++mode)
		{
			const int verticesPerPoint = verticesPerPointValues[mode];
			const int vertexCount = POINT_COUNT * verticesPerPoint;
			for (int vertex = 0; vertex < vertexCount; ++vertex)
			{
				expandedColors[vertex] = pointColors[vertex / verticesPerPoint];
				const PointColor &color = expandedColors[vertex];
				reference[vertex] = rts::render::PackLegacyARGB(
					color[0], color[1], color[2], color[3]);
			}

			int packedPointCount = 0;
			int expectedPackedPointCount = 0;
			bool pointMappingMatches = true;
			for (int chunkStart = 0; chunkStart < vertexCount;
				chunkStart += MAX_VB_SIZE)
			{
				const int chunkEnd =
					(chunkStart + MAX_VB_SIZE < vertexCount) ?
					chunkStart + MAX_VB_SIZE : vertexCount;
				int cachedPointIndex = -1;
				int previousPointIndex = -1;
				unsigned int packedColor = 0U;
				for (int vertex = chunkStart; vertex < chunkEnd; ++vertex)
				{
					const int pointIndex = vertex / verticesPerPoint;
					const bool repacked =
						rts::render::PackPointGroupColorForVertex(pointColors,
							vertex, verticesPerPoint, cachedPointIndex, packedColor);
					if (pointIndex != previousPointIndex)
						++expectedPackedPointCount;
					if (repacked)
						++packedPointCount;
					if (repacked != (pointIndex != previousPointIndex) ||
						packedColor != reference[vertex])
						pointMappingMatches = false;
					previousPointIndex = pointIndex;
				}
			}
			passed = Check(pointMappingMatches,
				"cached point colors match reference bytes across split VB chunks") && passed;
			passed = Check(packedPointCount == expectedPackedPointCount &&
				packedPointCount < vertexCount,
				"point color packs once per touched primitive per VB chunk") && passed;
		}
		return passed;
	}
}

int main()
{
	bool passed = true;

	passed = TestPointGroupColorPacking() && passed;

	passed = Check(rts::render::ClampLegacyColorUnit(0.0f) == 0.0f,
		"zero remains zero") && passed;
	passed = Check(rts::render::ClampLegacyColorUnit(0.5f) == 0.5f,
		"fractional unit value remains unchanged") && passed;
	passed = Check(rts::render::ClampLegacyColorUnit(1.0f) == 1.0f,
		"one remains one") && passed;
	passed = Check(rts::render::ClampLegacyColorUnit(-0.25f) == 0.0f &&
		rts::render::ClampLegacyColorUnit(1.25f) == 1.0f,
		"out-of-range values clamp to the unit interval") && passed;

	// Malformed channels have an explicit deterministic policy: NaN and
	// negative infinity become zero, positive infinity becomes one, and either
	// signed zero packs as byte zero.  This validates sanitization rather than
	// asserting bit parity with undefined/non-finite legacy inputs.
	const float nan_value = FloatFromBits(0x7fc00000U);
	const float positive_infinity = FloatFromBits(0x7f800000U);
	const float negative_infinity = FloatFromBits(0xff800000U);
	const float negative_zero = FloatFromBits(0x80000000U);
	passed = Check(rts::render::ClampLegacyColorUnit(nan_value) == 0.0f &&
		rts::render::ClampLegacyColorUnit(positive_infinity) == 1.0f &&
		rts::render::ClampLegacyColorUnit(negative_infinity) == 0.0f &&
		rts::render::ClampLegacyColorUnit(negative_zero) == 0.0f,
		"non-finite and signed-zero channels follow the documented policy") && passed;
	passed = Check(rts::render::LegacyColorByteFromUnit(nan_value) == 0U &&
		rts::render::LegacyColorByteFromUnit(positive_infinity) == 255U &&
		rts::render::LegacyColorByteFromUnit(negative_infinity) == 0U &&
		rts::render::LegacyColorByteFromUnit(negative_zero) == 0U,
		"malformed channels sanitize to deterministic byte endpoints") && passed;
	passed = Check(rts::render::PackLegacyARGB(nan_value, positive_infinity,
		negative_infinity, negative_zero) == 0x0000ff00U &&
		rts::render::PackLegacyARGB(negative_zero, negative_zero,
		negative_zero, negative_zero) ==
		rts::render::PackLegacyARGB(0.0f, 0.0f, 0.0f, 0.0f),
		"malformed and signed-zero ARGB packing is deterministic") && passed;

	// The conversion contract is truncation, not round-to-nearest: 0.5 * 255
	// must produce 127 in every channel.
	passed = Check(rts::render::PackLegacyARGB(0.0f, 0.0f, 0.0f, 0.0f) == 0x00000000U,
		"zero packs to transparent black") && passed;
	passed = Check(rts::render::PackLegacyARGB(0.5f, 0.5f, 0.5f, 0.5f) == 0x7f7f7f7fU,
		"half packs with truncation in all channels") && passed;
	passed = Check(rts::render::PackLegacyARGB(1.0f, 1.0f, 1.0f, 1.0f) == 0xffffffffU,
		"one packs to opaque white") && passed;
	passed = Check(rts::render::PackLegacyARGB(-0.25f, 1.25f, 0.5f, 1.25f) == 0xff00ff7fU,
		"clamped and fractional channels preserve ARGB order") && passed;

	const rts::render::LegacyPackedColor fractional(0.5f, 0.25f, 0.75f, 0.125f);
	passed = Check(rts::render::PackLegacyARGB(fractional) == 0x1f7f3fbfU &&
		rts::render::PackLegacyARGB(fractional.red, fractional.green, fractional.blue, fractional.alpha) ==
		0x1f7f3fbfU,
		"channel-struct and scalar packing agree") && passed;

	const unsigned int encoded = 0x80407fbfU;
	const rts::render::LegacyPackedColor unpacked = rts::render::UnpackLegacyARGB(encoded);
	passed = Check(Near(unpacked.red, static_cast<float>(64) / 255.0f) &&
		Near(unpacked.green, static_cast<float>(127) / 255.0f) &&
		Near(unpacked.blue, static_cast<float>(191) / 255.0f) &&
		Near(unpacked.alpha, static_cast<float>(128) / 255.0f),
		"unpack exposes ARGB channels in renderer order") && passed;
	passed = Check(rts::render::PackLegacyARGB(unpacked) == encoded,
		"unpack and pack preserve representable file colors") && passed;

	// Every representable byte value must survive a decode/encode cycle in
	// every ARGB channel.  Vary one byte at a time so this remains a focused
	// exhaustive test rather than an impractical 32-bit Cartesian product.
	const unsigned int channel_shifts[] = {24U, 16U, 8U, 0U};
	const unsigned int channel_seed = 0xa1b2c3d4U;
	bool all_byte_round_trips_passed = true;
	int channel;
	for (channel = 0; channel < 4; ++channel) {
		int byte_value;
		for (byte_value = 0; byte_value <= 255; ++byte_value) {
			const unsigned int mask = 0xffU << channel_shifts[channel];
			const unsigned int expected =
				(channel_seed & ~mask) |
				(static_cast<unsigned int>(byte_value) << channel_shifts[channel]);
			if (rts::render::PackLegacyARGB(rts::render::UnpackLegacyARGB(expected)) != expected) {
				all_byte_round_trips_passed = false;
			}
		}
	}
	passed = Check(all_byte_round_trips_passed,
		"all ARGB byte values round-trip exactly") && passed;

	// Check both sides of each integer threshold with a margin larger than a
	// float ulp.  This documents truncation around n/255 without depending on
	// whether the approximate stored float for n/255 is just below or above n.
	bool all_boundary_checks_passed = true;
	for (channel = 1; channel < 255; ++channel) {
		const float lower = (static_cast<float>(channel) - 0.25f) / 255.0f;
		const float upper = (static_cast<float>(channel) + 0.25f) / 255.0f;
		if (rts::render::LegacyColorByteFromUnit(lower) != static_cast<unsigned int>(channel - 1) ||
			rts::render::LegacyColorByteFromUnit(upper) != static_cast<unsigned int>(channel)) {
			all_boundary_checks_passed = false;
		}
	}
	passed = Check(all_boundary_checks_passed,
		"fractional values on either side of n/255 truncate deterministically") && passed;

#if defined(_MSC_VER)
	// Exercise the conversion under each supported rounding mode.  Explicit
	// double scaling plus floor must keep the packed value unchanged.
	const unsigned int saved_control = _controlfp(0, 0);
	const unsigned int rounding_modes[] = { _RC_DOWN, _RC_UP, _RC_CHOP, _RC_NEAR };
	const int rounding_mode_count = sizeof(rounding_modes) / sizeof(rounding_modes[0]);
	int mode;
	for (mode = 0; mode < rounding_mode_count; ++mode) {
		_controlfp(rounding_modes[mode], _MCW_RC);
		passed = Check(rts::render::PackLegacyARGB(0.5f, 0.25f, 0.75f, 0.125f) == 0x1f7f3fbfU,
			"packing is independent of ambient floating-point rounding") && passed;
	}
	_controlfp(saved_control, _MCW_RC);
#endif

	return passed ? 0 : 1;
}
