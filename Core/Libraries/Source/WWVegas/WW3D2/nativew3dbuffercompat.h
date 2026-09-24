#pragma once

#include "Renderer/RendererDevice.h"
#include <stddef.h>

class DX8VertexBufferClass;

namespace rts
{
namespace render
{

// Streaming shadow buffers force a discard by making the next append cursor
// exceed the available range.  Keep the cursor policy shared between the
// projected and volumetric shadow callers so a failed mutation always retries
// from offset zero instead of reusing a poisoned append range.
static const int NATIVE_W3D_STREAM_DISCARD_CURSOR = 0xffff;

inline bool Native_W3D_Stream_Needs_Discard(int cursor, int capacity,
	int append_count)
{
	return cursor < 0 || capacity < 0 || append_count < 0 ||
		append_count > capacity || cursor > capacity - append_count;
}

inline void Invalidate_Native_W3D_Stream_Cursors(int &vertex_cursor,
	int &index_cursor, int &vertex_start, int &index_start)
{
	vertex_cursor = NATIVE_W3D_STREAM_DISCARD_CURSOR;
	index_cursor = NATIVE_W3D_STREAM_DISCARD_CURSOR;
	vertex_start = 0;
	index_start = 0;
}

// Water-track writes are already uploaded by DX8VertexBufferClass::Unlock_Buffer
// on the native lane. The x86 compatibility adapter supplies the historical
// bridge publication below, keeping product callers independent of the
// retired wrapper declaration.
#if defined(_WIN64)
inline bool Publish_Render_Buffer_Change(DX8VertexBufferClass *buffer,
	unsigned int binding, const void *data, size_t byte_count,
	size_t destination_offset, RenderBufferUpdateMode mode,
	unsigned int source_generation)
{
	(void)binding;
	(void)destination_offset;
	(void)mode;
	(void)source_generation;
	return buffer != 0 && data != 0 && byte_count != 0;
}
#else
bool Publish_Render_Buffer_Change(DX8VertexBufferClass *buffer,
	unsigned int binding, const void *data, size_t byte_count,
	size_t destination_offset, RenderBufferUpdateMode mode,
	unsigned int source_generation);
#endif

}
}

// The native buffer facade accepts the numeric lock bits carried by existing
// callers, but decodes them into the neutral update contract before touching
// NativeW3DBufferOwner. These values are the serialized WW3D lock contract;
// their names deliberately do not import an API-specific device declaration
// into product code.
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
