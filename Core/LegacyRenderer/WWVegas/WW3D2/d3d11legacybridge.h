#pragma once

#include <d3d8.h>
#include "Renderer/RendererDevice.h"

// Translate only the primitive types that the neutral renderer can represent.
// Keeping this helper inline makes the fail-closed contract available to the
// bridge and to small portability/contract tests without pulling the bridge's
// device implementation into a test binary.
inline bool Try_Translate_D3D8_Primitive_Topology(
	unsigned int primitive_type,
	rts::render::RenderPrimitiveTopology *topology)
{
	if (topology == 0)
	{
		return false;
	}
	switch (primitive_type)
	{
	case D3DPT_TRIANGLELIST:
		*topology = rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST;
		return true;
	case D3DPT_TRIANGLESTRIP:
		*topology = rts::render::RENDER_PRIMITIVE_TRIANGLE_STRIP;
		return true;
	case D3DPT_LINELIST:
		*topology = rts::render::RENDER_PRIMITIVE_LINE_LIST;
		return true;
	case D3DPT_LINESTRIP:
		*topology = rts::render::RENDER_PRIMITIVE_LINE_STRIP;
		return true;
	default:
		return false;
	}
}

// A rejected logical state update must make the next visible D3D11 draw fail
// closed; reusing the previous stage state would produce an apparently valid
// but incorrect frame.
inline bool Can_Submit_D3D11_Legacy_Draw()
{
	return !rts::render::HasLegacyStatePublicationFailure();
}

struct D3D11LegacyBridgeCacheStats
{
	unsigned int bufferLookups;
	unsigned int bufferHits;
	unsigned int textureLookups;
	unsigned int textureHits;
	unsigned int bufferUploads;
	unsigned int textureRefreshes;
};

class IndexBufferClass;
class VertexBufferClass;
class TextureBaseClass;

class D3D11LegacyBridge
{
public:
	D3D11LegacyBridge();
	~D3D11LegacyBridge();

	// Native x64 passes no legacy device. The optional pointer remains only for
	// the 32-bit differential bridge diagnostics.
	bool Initialize(HWND window, IDirect3DDevice8 *legacy_device,
		unsigned int width, unsigned int height, bool enable_vsync);
	void Shutdown();
	// Keeps the historical void ABI while giving native lifecycle owners a
	// truthful completion result.  A failed teardown retains ownership so the
	// owner thread can retry without discarding the bridge state.
	rts::render::RenderResult Shutdown_Result();
	bool Prepare_Legacy_Device_Reset();
	bool Is_Active() const;
	void Begin_Display_Iteration();
	bool Begin_Frame();
	void Request_Frame_Capture();
	rts::render::RenderResult Get_Back_Buffer_Info(
		rts::render::RenderBackBufferInfo *info) const;
	rts::render::RenderResult Queue_Back_Buffer_Capture(
		const rts::render::RenderCaptureRequestDescriptor &descriptor,
		rts::render::RenderCaptureHandle *handle);
	unsigned int Cancel_Back_Buffer_Captures(void *consumer,
		rts::render::RenderResult reason);
	rts::render::RenderResult End_Frame(bool present_frame);
	rts::render::RenderResult End_Frame(bool present_frame,
		rts::render::RenderFrameOutcome *outcome);
	void Clear(bool clear_color, bool clear_depth_stencil,
		float red, float green, float blue, float alpha,
		float depth, unsigned int stencil);
	void Set_Viewport(const D3DVIEWPORT8 &viewport);
	// True only while the bridge has a valid target mapping for the current
	// legacy target.  Callers use this to avoid submitting to the wrong output
	// after an unsupported or failed target transition.
	bool Is_Render_Target_Operational() const;
	// Records a visible-submission failure when the bridge is still active but
	// has no valid target. Callers must not submit that work to the hidden
	// legacy swap chain because D3D11 still owns presentation.
	void Record_Visible_Submission_Failure();
	rts::render::RenderResult Set_Render_Target_Surfaces(
		IDirect3DSurface8 *color_surface,
		IDirect3DSurface8 *depth_surface, bool use_default_depth);
	rts::render::RenderResult Set_Render_Target_Default();
#if defined(_WIN64)
	rts::render::RenderResult Set_Render_Target_Textures(
		TextureBaseClass *color_texture, TextureBaseClass *depth_texture,
		bool use_default_depth);
#endif
	// Copies the current D3D11 color target into a legacy texture's neutral
	// renderer allocation. The legacy D3D8 path remains untouched.
	rts::render::RenderResult Copy_Active_Color_Target_To_Texture(
		IDirect3DBaseTexture8 *destination);
	// Acquires the GPU-produced contents written by the copy operation for the
	// current frame. Cache eviction, device recovery, or a later D3D8-side write
	// invalidates that content and makes the owner regenerate it.
	bool Acquire_Copied_Texture_Content(IDirect3DBaseTexture8 *texture);
	void Invalidate_Buffer(IUnknown *buffer);
	void Invalidate_Buffer_Range(IUnknown *buffer, unsigned int binding,
		size_t destination_offset, size_t byte_count,
		rts::render::RenderBufferUpdateMode mode);
	bool Publish_Buffer_Change(IUnknown *buffer, unsigned int binding,
		const void *data, size_t byte_count, size_t destination_offset,
		rts::render::RenderBufferUpdateMode mode,
		unsigned int source_generation);
	// Publishes a complete opaque BGRA8 level directly into the neutral renderer
	// mirror. This owner-thread operation is valid before Begin_Frame because
	// movie decoding runs during Display::update.
	bool Publish_Texture_BGRA8_Change(IDirect3DBaseTexture8 *texture,
		const void *data, size_t row_pitch, size_t slice_pitch);
	void Invalidate_Texture(IDirect3DBaseTexture8 *texture);
	bool Draw(VertexBufferClass *vertex_buffer,
		IndexBufferClass *index_buffer, unsigned int primitive_type,
		unsigned int min_vertex_index, unsigned int vertex_count,
		unsigned int start_index, unsigned int primitive_count,
		unsigned int base_vertex);
	bool Draw(IDirect3DVertexBuffer8 *vertex_buffer, unsigned int fvf,
		unsigned int vertex_stride, IDirect3DIndexBuffer8 *index_buffer,
		unsigned int primitive_type, unsigned int min_vertex_index,
		unsigned int vertex_count, unsigned int start_index,
		unsigned int primitive_count, unsigned int base_vertex);
	unsigned int Get_Raw_Indexed_Draw_Count() const;
	void Get_Cache_Stats(D3D11LegacyBridgeCacheStats *stats) const;
	rts::render::RenderResult Draw_Primitive_UP(
		unsigned int fvf, unsigned int primitive_type,
		unsigned int primitive_count, const void *vertex_data,
		unsigned int vertex_stride);
	rts::render::RenderResult Resize(unsigned int width, unsigned int height);

private:
	D3D11LegacyBridge(const D3D11LegacyBridge &);
	D3D11LegacyBridge &operator=(const D3D11LegacyBridge &);

	struct Impl;
	Impl *m_impl;
};
