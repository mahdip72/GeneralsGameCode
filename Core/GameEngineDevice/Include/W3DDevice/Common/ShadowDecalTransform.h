/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/BaseType.h"

#include <string.h>

/* Matrix3D is initialized to identity before this helper is called.  The
 * tree-buffer decal uses a positive-zero local angle, so rotating that
 * identity matrix is redundant; avoid its per-shadow sin/cos calls.  Compare
 * representations rather than floating-point equality so negative zero and
 * every nonzero value retain the legacy Rotate_Z path exactly. */
template <typename Transform>
inline bool ApplyShadowDecalLocalAngle(Transform &transform,
	const Real &angle)
{
	const Real positiveZero = 0.0f;
	if (memcmp(&angle, &positiveZero, sizeof(Real)) == 0)
		return false;
	transform.Rotate_Z(angle);
	return true;
}
