#ifndef RTS_RENDERER_LEGACY_COLOR_PACKING_H
#define RTS_RENDERER_LEGACY_COLOR_PACKING_H

#include <math.h>

namespace rts
{
namespace render
{

// Backend-neutral channel values used by the ARGB file and vertex-color
// contracts.  This deliberately has no WW3D or device dependency so it can
// be shared by native and legacy title code.
struct LegacyPackedColor
{
	LegacyPackedColor() : red(0.0f), green(0.0f), blue(0.0f), alpha(0.0f) {}
	LegacyPackedColor(float red_value, float green_value, float blue_value, float alpha_value) :
		red(red_value), green(green_value), blue(blue_value), alpha(alpha_value) {}

	float red;
	float green;
	float blue;
	float alpha;
};

// Clamp all non-finite and out-of-range values deterministically.  NaN and
// negative infinity are treated as zero; positive infinity saturates to one.
// This is the malformed-input policy at the backend-neutral boundary, not a
// claim of bit-for-bit legacy parity for non-finite source values.
inline float ClampLegacyColorUnit(float value)
{
	if (!(value > 0.0f)) {
		return 0.0f;
	}
	if (value >= 1.0f) {
		return 1.0f;
	}
	return value;
}

// Convert a unit channel to an 8-bit value using truncation toward zero.
// Promote the stored float before scaling in double, then floor the scaled
// value explicitly so the result is independent of the ambient x87 rounding
// mode.  The promotion does not make the original float value exact.
inline unsigned int LegacyColorByteFromUnit(float value)
{
	const float clamped = ClampLegacyColorUnit(value);
	if (clamped >= 1.0f) {
		return 255U;
	}

	const double scaled = static_cast<double>(clamped) * 255.0;
	return static_cast<unsigned int>(::floor(scaled));
}

inline unsigned int PackLegacyARGB(float red, float green, float blue, float alpha)
{
	return (LegacyColorByteFromUnit(alpha) << 24) |
		(LegacyColorByteFromUnit(red) << 16) |
		(LegacyColorByteFromUnit(green) << 8) |
		LegacyColorByteFromUnit(blue);
}

inline unsigned int PackLegacyARGB(const LegacyPackedColor &color)
{
	return PackLegacyARGB(color.red, color.green, color.blue, color.alpha);
}

inline LegacyPackedColor UnpackLegacyARGB(unsigned int color)
{
	return LegacyPackedColor(
		static_cast<float>((color >> 16) & 0xffU) / 255.0f,
		static_cast<float>((color >> 8) & 0xffU) / 255.0f,
		static_cast<float>(color & 0xffU) / 255.0f,
		static_cast<float>((color >> 24) & 0xffU) / 255.0f);
}

} // namespace render
} // namespace rts

#endif // RTS_RENDERER_LEGACY_COLOR_PACKING_H
