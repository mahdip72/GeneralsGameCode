// C++98-compatible interval selection shared by both raw-animation titles.
#pragma once

#include "WWMath/wwmath.h"
#include <limits.h>

namespace rts
{
namespace animation
{

inline int SelectRawAnimationFrame0(float frame)
{
#if defined(_WIN64)
	// Native Float_To_Long truncates, unlike Win32's FISTP. Subtracting a
	// half frame there selects the previous interval and extrapolates. Valid
	// native phases need the enclosing interval. The bound also excludes
	// NaN/infinity and unsafe integer casts; unsupported and negative phases
	// retain the historical expression below, without new validation policy.
	if (frame >= 0.0f && frame < static_cast<float>(INT_MAX))
		return static_cast<int>(WWMath::Floor(frame));
#endif
	return static_cast<int>(WWMath::Float_To_Long(frame - 0.499999f));
}

}
}
