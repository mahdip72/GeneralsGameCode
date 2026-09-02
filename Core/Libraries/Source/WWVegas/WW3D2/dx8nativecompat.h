#pragma once

#include "Renderer/RendererDevice.h"

// The native buffer facade accepts the numeric lock bits carried by existing
// callers, but decodes them into the neutral update contract before touching
// NativeW3DBufferOwner.  These values are the serialized WW3D lock contract;
// their names deliberately do not import D3D8 declarations into x64 code.
enum NativeBufferLockFlag
{
	NATIVE_BUFFER_LOCK_READ_ONLY = 0x0010U,
	NATIVE_BUFFER_LOCK_NO_SYSTEM_LOCK = 0x0800U,
	NATIVE_BUFFER_LOCK_NO_OVERWRITE = 0x1000U,
	NATIVE_BUFFER_LOCK_DISCARD = 0x2000U
};

inline bool Decode_Native_Buffer_Update_Mode(unsigned int flags,
	rts::render::RenderBufferUpdateMode *mode)
{
	const unsigned int supported_flags = NATIVE_BUFFER_LOCK_DISCARD |
		NATIVE_BUFFER_LOCK_NO_OVERWRITE | NATIVE_BUFFER_LOCK_NO_SYSTEM_LOCK;
	if (mode == 0 || (flags & ~supported_flags) != 0U ||
		((flags & NATIVE_BUFFER_LOCK_DISCARD) != 0U &&
			(flags & NATIVE_BUFFER_LOCK_NO_OVERWRITE) != 0U))
	{
		return false;
	}
	if ((flags & NATIVE_BUFFER_LOCK_DISCARD) != 0U)
	{
		*mode = rts::render::RENDER_BUFFER_UPDATE_DISCARD;
	}
	else if ((flags & NATIVE_BUFFER_LOCK_NO_OVERWRITE) != 0U)
	{
		*mode = rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE;
	}
	else
	{
		*mode = rts::render::RENDER_BUFFER_UPDATE_PRESERVE;
	}
	return true;
}
