#pragma once

#include <d3d8.h>
#include "Renderer/RendererDevice.h"

class IndexBufferClass;
class VertexBufferClass;

class D3D11LegacyBridge
{
public:
	D3D11LegacyBridge();
	~D3D11LegacyBridge();

	bool Initialize(HWND window, IDirect3DDevice8 *legacy_device,
		unsigned int width, unsigned int height, bool enable_vsync);
	void Shutdown();
	bool Is_Active() const;
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
	bool Draw(VertexBufferClass *vertex_buffer,
		IndexBufferClass *index_buffer, unsigned int primitive_type,
		unsigned int start_index, unsigned int primitive_count,
		unsigned int base_vertex);
	rts::render::RenderResult Resize(unsigned int width, unsigned int height);
	rts::render::RenderResult Capture_Back_Buffer(void *destination,
		size_t destination_bytes, size_t destination_row_pitch,
		rts::render::RenderFormat *format);

private:
	D3D11LegacyBridge(const D3D11LegacyBridge &);
	D3D11LegacyBridge &operator=(const D3D11LegacyBridge &);

	struct Impl;
	Impl *m_impl;
};
