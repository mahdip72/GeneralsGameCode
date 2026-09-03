/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** Private x86/VC6 declaration for the legacy missing-texture adapter.  The
** public product header is backend-neutral; this same-directory header keeps
** the adapter's opaque pointer ABI out of the product prefix.
*/

#pragma once

#include "WWLib/always.h"

// These handles intentionally remain opaque at the adapter declaration.  The
// legacy implementation and its callers perform the one local conversion to
// their backend resource types; both functions still return one machine
// pointer, preserving the x86 decorated symbol and calling convention.
typedef void MissingTextureHandle;
typedef void MissingSurfaceHandle;

class MissingTexture
{
public:
	static void _Init();
	static void _Deinit();

	static MissingTextureHandle* _Get_Missing_Texture();
	static MissingSurfaceHandle* _Create_Missing_Surface();
};
