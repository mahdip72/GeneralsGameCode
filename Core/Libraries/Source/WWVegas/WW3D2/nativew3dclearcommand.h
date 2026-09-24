#ifndef RTS_NATIVEW3DCLEARCOMMAND_H
#define RTS_NATIVEW3DCLEARCOMMAND_H

#include "Renderer/RendererDevice.h"

namespace rts
{
namespace render
{

// The legacy clear-z contract resets both depth and stencil. Volume shadows
// rely on a fresh stencil mask each frame even when their geometry persists.
inline unsigned int NativeGameClearFlags(bool clearColor, bool clearDepth)
{
	return (clearColor ? RENDER_CLEAR_COLOR : 0U) |
		(clearDepth ? (RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL) : 0U);
}

}
}

#endif
