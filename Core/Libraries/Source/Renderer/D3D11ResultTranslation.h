#ifndef RTS_RENDERER_D3D11RESULTTRANSLATION_H
#define RTS_RENDERER_D3D11RESULTTRANSLATION_H

#include "Renderer/RendererDevice.h"

#include <windows.h>
#include <dxgi.h>

namespace rts
{
namespace render
{
namespace detail
{
inline RenderResult TranslateD3D11Result(HRESULT result)
{
	if (SUCCEEDED(result))
	{
		return RENDER_RESULT_OK;
	}
	if (result == E_INVALIDARG)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (result == E_OUTOFMEMORY)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (result == DXGI_ERROR_UNSUPPORTED)
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	if (result == DXGI_ERROR_DEVICE_HUNG ||
		result == DXGI_ERROR_DEVICE_REMOVED ||
		result == DXGI_ERROR_DEVICE_RESET ||
		result == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
	{
		return RENDER_RESULT_DEVICE_REMOVED;
	}
	return RENDER_RESULT_FAILED;
}
}
}
}

#endif
