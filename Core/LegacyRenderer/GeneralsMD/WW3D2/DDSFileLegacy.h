/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** x86-only bridge for the legacy renderer's surface upload.  The title DDS
** reader exposes only CPU-side WW3D format and byte-buffer APIs; this header
** is kept outside the product source prefix so the old surface type cannot
** enter the native x64 graph.
*/

#pragma once

#include "WWMath/vector3.h"

class DDSFileClass;
#if !defined(_WIN64)
struct IDirect3DSurface8;
void Legacy_DDS_Copy_Level_To_Surface(
	DDSFileClass &dds_file,
	unsigned level,
	IDirect3DSurface8 *d3d_surface,
	const Vector3 &hsv_shift = Vector3(0.0f, 0.0f, 0.0f));
#endif
