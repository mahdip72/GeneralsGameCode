#ifndef RTS_RENDERER_RENDERTEXTUREPUBLICATION_H
#define RTS_RENDERER_RENDERTEXTUREPUBLICATION_H

#include <stddef.h>

class TextureClass;
class TextureBaseClass;

// Each architecture supplies these entry points through its own texture owner.
// Native surface unlocks already publish their CPU writes, so notification is
// only a compatibility-cache operation on the historical lane.
void Notify_Render_Texture_Changed(TextureClass *texture);
bool Publish_Render_Texture_BGRA8_Change(TextureClass *texture,
	const void *data, size_t row_pitch, size_t slice_pitch);
bool Is_Render_D3D11_Backend_Active();
bool Is_Render_Texture_Publication_Operational();


namespace rts
{
namespace render
{

// Requested backend capability is not proof that the native owner is alive;
// this query reflects the published owner's current operational state.
inline bool IsNativeD3D11PublicationActive()
{
	return ::Is_Render_D3D11_Backend_Active();
}

// This is the state policy used by the bridge-backed implementation.  Keeping
// the state inputs explicit makes the lifecycle contract testable without
// creating a graphics device or mutating the compatibility renderer's protected state.
inline bool IsRenderTexturePublicationOperationalState(
	bool renderer_initialized, bool device_lost,
	bool native_backend_selected, bool native_backend_active)
{
	return renderer_initialized && !device_lost &&
		(!native_backend_selected || native_backend_active);
}

// Resource producers may run before a scene frame is bracketed.  This query
// deliberately reports backend/device operational state rather than the
// requested backend or the transient scene bracket.
inline bool IsRenderTexturePublicationOperational()
{
	return ::Is_Render_Texture_Publication_Operational();
}

// Texture stage publication is renderer state, not a backend device operation.
// The compatibility bridge may mirror this state while the native renderer
// owns the actual resource handle.
void Publish_Render_Texture_Stage(unsigned int stage,
	TextureBaseClass *texture);
TextureBaseClass *Get_Published_Render_Texture_Stage(unsigned int stage);
void Unpublish_Render_Texture(TextureBaseClass *texture);
void Record_Render_Texture_Use(TextureClass *texture);
unsigned int Get_Render_Texture_Use_Count();

inline void NotifyTextureChanged(TextureClass *texture)
{
	::Notify_Render_Texture_Changed(texture);
}

inline bool PublishTextureBGRA8Change(TextureClass *texture,
	const void *data, size_t row_pitch, size_t slice_pitch)
{
	return ::Publish_Render_Texture_BGRA8_Change(texture, data, row_pitch,
		slice_pitch);
}

inline void PublishTextureStage(unsigned int stage, TextureBaseClass *texture)
{
	Publish_Render_Texture_Stage(stage, texture);
}

inline TextureBaseClass *GetPublishedTextureStage(unsigned int stage)
{
	return Get_Published_Render_Texture_Stage(stage);
}

inline void UnpublishTexture(TextureBaseClass *texture)
{
	Unpublish_Render_Texture(texture);
}

inline void RecordTextureUse(TextureClass *texture)
{
	Record_Render_Texture_Use(texture);
}

inline unsigned int GetTextureUseCount()
{
	return Get_Render_Texture_Use_Count();
}

}
}

#endif
