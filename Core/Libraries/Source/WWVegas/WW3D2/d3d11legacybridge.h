#pragma once

#include <d3d8.h>

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
	void End_Frame(bool present_frame);
	void Clear(bool clear_color, bool clear_depth_stencil,
		float red, float green, float blue, float alpha,
		float depth, unsigned int stencil);
	void Set_Viewport(const D3DVIEWPORT8 &viewport);
	bool Draw(VertexBufferClass *vertex_buffer,
		IndexBufferClass *index_buffer, unsigned int primitive_type,
		unsigned int start_index, unsigned int primitive_count,
		unsigned int base_vertex);
	bool Resize(unsigned int width, unsigned int height);

private:
	D3D11LegacyBridge(const D3D11LegacyBridge &);
	D3D11LegacyBridge &operator=(const D3D11LegacyBridge &);

	struct Impl;
	Impl *m_impl;
};
