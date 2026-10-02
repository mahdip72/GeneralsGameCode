/*
**  x86/VC6-only format adapter.  This header is intentionally outside the
**  native product prefix so no native renderer target can consume API-native
**  format values accidentally.
*/

#pragma once

#include "Core/Libraries/Source/WWVegas/WW3D2/formconv.h"
#include <d3d8.h>

extern D3DFORMAT WW3DFormatToD3DFormatConversionArray[WW3D_FORMAT_COUNT];
extern D3DFORMAT WW3DZFormatToD3DFormatConversionArray[WW3D_ZFORMAT_COUNT];

bool Try_D3DFormat_To_RenderFormat(D3DFORMAT format,
	rts::render::RenderFormat *renderFormat, bool *requiresCpuConversion);

D3DFORMAT WW3DFormat_To_D3DFormat(WW3DFormat ww3d_format);
WW3DFormat D3DFormat_To_WW3DFormat(D3DFORMAT d3d_format);

D3DFORMAT WW3DZFormat_To_D3DFormat(WW3DZFormat ww3d_zformat);
WW3DZFormat D3DFormat_To_WW3DZFormat(D3DFORMAT d3d_format);
