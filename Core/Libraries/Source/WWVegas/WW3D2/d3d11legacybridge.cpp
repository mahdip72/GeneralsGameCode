#include "d3d11legacybridge.h"

#if defined(RTS_RENDERER_HAS_D3D11)

#include "dx8fvf.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "Renderer/LegacyRenderState.h"
#include "Renderer/RendererDevice.h"

#include <d3dx8tex.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
const unsigned int BUFFER_CACHE_CAPACITY = 1024;
const unsigned int TEXTURE_CACHE_CAPACITY = 1024;
const unsigned int CACHE_STALE_FRAME_COUNT = 600;

using rts::render::BufferDescriptor;
using rts::render::GpuHandle;
using rts::render::IRenderContext;
using rts::render::IRenderDevice;
using rts::render::LegacyLogicalState;
using rts::render::LegacyVertexDataFormat;
using rts::render::LegacyVertexElement;
using rts::render::LegacyVertexLayout;
using rts::render::RenderResult;
using rts::render::TextureDescriptor;
using rts::render::TextureSubresourceData;

bool Append_Element(LegacyVertexLayout *layout,
	rts::render::LegacyVertexSemantic semantic, unsigned int semantic_index,
	LegacyVertexDataFormat format, unsigned int byte_offset)
{
	if (layout->elementCount >= LegacyVertexLayout::MAX_ELEMENT_COUNT)
	{
		return false;
	}
	LegacyVertexElement &element = layout->elements[layout->elementCount++];
	element.semantic = semantic;
	element.semanticIndex = semantic_index;
	element.format = format;
	element.byteOffset = byte_offset;
	return true;
}

LegacyVertexDataFormat Texture_Format_From_Bytes(unsigned int byte_count)
{
	switch (byte_count)
	{
	case 4: return rts::render::RENDER_VERTEX_DATA_FLOAT1;
	case 8: return rts::render::RENDER_VERTEX_DATA_FLOAT2;
	case 12: return rts::render::RENDER_VERTEX_DATA_FLOAT3;
	default: return rts::render::RENDER_VERTEX_DATA_FLOAT4;
	}
}

bool Build_Vertex_Layout(const FVFInfoClass &fvf_info,
	LegacyVertexLayout *layout)
{
	if (layout == 0)
	{
		return false;
	}
	*layout = LegacyVertexLayout();
	layout->stride = fvf_info.Get_FVF_Size();
	const unsigned int fvf = fvf_info.Get_FVF();
	if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ ||
		!Append_Element(layout, rts::render::RENDER_VERTEX_SEMANTIC_POSITION,
			0, rts::render::RENDER_VERTEX_DATA_FLOAT3,
			fvf_info.Get_Location_Offset()))
	{
		return false;
	}
	if ((fvf & D3DFVF_NORMAL) != 0 &&
		!Append_Element(layout, rts::render::RENDER_VERTEX_SEMANTIC_NORMAL,
			0, rts::render::RENDER_VERTEX_DATA_FLOAT3,
			fvf_info.Get_Normal_Offset()))
	{
		return false;
	}
	if ((fvf & D3DFVF_DIFFUSE) != 0 &&
		!Append_Element(layout, rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE,
			0, rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8,
			fvf_info.Get_Diffuse_Offset()))
	{
		return false;
	}
	if ((fvf & D3DFVF_SPECULAR) != 0 &&
		!Append_Element(layout, rts::render::RENDER_VERTEX_SEMANTIC_SPECULAR,
			0, rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8,
			fvf_info.Get_Specular_Offset()))
	{
		return false;
	}
	const unsigned int texture_count =
		(fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (texture_count > rts::render::LEGACY_TEXTURE_STAGE_COUNT)
	{
		return false;
	}
	for (unsigned int index = 0; index < texture_count; ++index)
	{
		const unsigned int offset = fvf_info.Get_Tex_Offset(index);
		const unsigned int next_offset = index + 1 < texture_count ?
			fvf_info.Get_Tex_Offset(index + 1) : fvf_info.Get_FVF_Size();
		if (next_offset <= offset || next_offset - offset > 16 ||
			!Append_Element(layout,
				rts::render::RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE,
				index, Texture_Format_From_Bytes(next_offset - offset), offset))
		{
			return false;
		}
	}
	return true;
}

void Append_Layout_Diagnostic(char *message, unsigned int capacity,
	unsigned int *used, const LegacyVertexLayout &layout)
{
	if (message == 0 || used == 0 || capacity == 0 || *used >= capacity)
	{
		return;
	}
	int written = snprintf(message + *used, capacity - *used, " layout=[");
	if (written < 0 || static_cast<unsigned int>(written) >= capacity - *used)
	{
		message[capacity - 1] = '\0';
		*used = capacity - 1;
		return;
	}
	*used += static_cast<unsigned int>(written);
	const unsigned int elementCount = layout.elementCount >
		LegacyVertexLayout::MAX_ELEMENT_COUNT ?
		LegacyVertexLayout::MAX_ELEMENT_COUNT : layout.elementCount;
	for (unsigned int index = 0; index < elementCount; ++index)
	{
		const LegacyVertexElement &element = layout.elements[index];
		written = snprintf(message + *used, capacity - *used,
			"%s%u,%u,%u", index == 0 ? "" : ";",
			static_cast<unsigned int>(element.semantic),
			static_cast<unsigned int>(element.format), element.byteOffset);
		if (written < 0 || static_cast<unsigned int>(written) >= capacity - *used)
		{
			message[capacity - 1] = '\0';
			*used = capacity - 1;
			return;
		}
		*used += static_cast<unsigned int>(written);
	}
	if (*used < capacity - 1)
	{
		message[(*used)++] = ']';
		message[*used] = '\0';
	}
}

unsigned int Primitive_Index_Count(unsigned int primitive_type,
	unsigned int primitive_count)
{
	switch (primitive_type)
	{
	case D3DPT_TRIANGLELIST: return primitive_count * 3;
	case D3DPT_TRIANGLESTRIP: return primitive_count + 2;
	case D3DPT_LINELIST: return primitive_count * 2;
	case D3DPT_LINESTRIP: return primitive_count + 1;
	default: return 0;
	}
}

rts::render::RenderPrimitiveTopology Translate_Topology(
	unsigned int primitive_type)
{
	switch (primitive_type)
	{
	case D3DPT_TRIANGLESTRIP:
		return rts::render::RENDER_PRIMITIVE_TRIANGLE_STRIP;
	case D3DPT_LINELIST:
		return rts::render::RENDER_PRIMITIVE_LINE_LIST;
	case D3DPT_LINESTRIP:
		return rts::render::RENDER_PRIMITIVE_LINE_STRIP;
	default:
		return rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST;
	}
}
}

struct D3D11LegacyBridge::Impl
{
	struct BufferEntry
	{
		BufferEntry() : source(0), byte_count(0), last_used_frame(0), handle() {}
		IUnknown *source;
		size_t byte_count;
		unsigned int last_used_frame;
		GpuHandle handle;
	};

	struct TextureEntry
	{
		TextureEntry() : source(0), render_target(false), last_used_frame(0),
			handle() {}
		IDirect3DBaseTexture8 *source;
		bool render_target;
		unsigned int last_used_frame;
		GpuHandle handle;
	};

	Impl() : device(0), context(0), legacy_device(0), frame_open(false),
		log_file(0), frame_id(0), draw_count(0), draw_failure_count(0) {}

	IRenderDevice *device;
	IRenderContext *context;
	IDirect3DDevice8 *legacy_device;
	bool frame_open;
	FILE *log_file;
	unsigned int frame_id;
	unsigned int draw_count;
	unsigned int draw_failure_count;
	std::vector<BufferEntry> vertex_buffers;
	std::vector<BufferEntry> index_buffers;
	std::vector<TextureEntry> textures;

	void Log(const char *message)
	{
		if (log_file != 0)
		{
			fprintf(log_file, "%s\n", message);
			fflush(log_file);
		}
	}

	bool Fail(const char *message)
	{
		++draw_failure_count;
		if (draw_failure_count <= 64)
		{
			Log(message);
		}
		return false;
	}

	BufferEntry *Find_Buffer(std::vector<BufferEntry> &entries,
		IUnknown *source)
	{
		for (unsigned int index = 0; index < entries.size(); ++index)
		{
			if (entries[index].source == source)
			{
				entries[index].last_used_frame = frame_id;
				return &entries[index];
			}
		}
		return 0;
	}

	void Evict_Oldest_Buffer(std::vector<BufferEntry> &entries)
	{
		if (entries.empty())
		{
			return;
		}
		unsigned int oldest = 0;
		for (unsigned int index = 1; index < entries.size(); ++index)
		{
			if (entries[index].last_used_frame < entries[oldest].last_used_frame)
			{
				oldest = index;
			}
		}
		device->destroyResource(entries[oldest].handle);
		entries[oldest].source->Release();
		entries.erase(entries.begin() + oldest);
	}

	void Evict_Oldest_Texture()
	{
		if (textures.empty())
		{
			return;
		}
		unsigned int oldest = 0;
		for (unsigned int index = 1; index < textures.size(); ++index)
		{
			if (textures[index].last_used_frame < textures[oldest].last_used_frame)
			{
				oldest = index;
			}
		}
		device->destroyResource(textures[oldest].handle);
		textures[oldest].source->Release();
		textures.erase(textures.begin() + oldest);
	}

	void Prune_Stale_Caches()
	{
		for (unsigned int index = 0; index < vertex_buffers.size();)
		{
			if (vertex_buffers[index].last_used_frame + CACHE_STALE_FRAME_COUNT <
				frame_id)
			{
				device->destroyResource(vertex_buffers[index].handle);
				vertex_buffers[index].source->Release();
				vertex_buffers.erase(vertex_buffers.begin() + index);
			}
			else
			{
				++index;
			}
		}
		for (unsigned int index = 0; index < index_buffers.size();)
		{
			if (index_buffers[index].last_used_frame + CACHE_STALE_FRAME_COUNT <
				frame_id)
			{
				device->destroyResource(index_buffers[index].handle);
				index_buffers[index].source->Release();
				index_buffers.erase(index_buffers.begin() + index);
			}
			else
			{
				++index;
			}
		}
		for (unsigned int index = 0; index < textures.size();)
		{
			if (textures[index].last_used_frame + CACHE_STALE_FRAME_COUNT < frame_id)
			{
				device->destroyResource(textures[index].handle);
				textures[index].source->Release();
				textures.erase(textures.begin() + index);
			}
			else
			{
				++index;
			}
		}
	}

	bool Upload_Vertex_Buffer(VertexBufferClass *vertex_buffer,
		GpuHandle *handle)
	{
		if (vertex_buffer == 0 || handle == 0 ||
			(vertex_buffer->Type() != BUFFER_TYPE_DX8 &&
			 vertex_buffer->Type() != BUFFER_TYPE_DYNAMIC_DX8))
		{
			return Fail("draw failure: unsupported vertex buffer");
		}
		IDirect3DVertexBuffer8 *source = static_cast<DX8VertexBufferClass *>(
			vertex_buffer)->Get_DX8_Vertex_Buffer();
		const size_t byte_count = static_cast<size_t>(
			vertex_buffer->Get_Vertex_Count()) *
			vertex_buffer->FVF_Info().Get_FVF_Size();
		BufferEntry *entry = Find_Buffer(vertex_buffers, source);
		if (entry == 0)
		{
			if (vertex_buffers.size() >= BUFFER_CACHE_CAPACITY)
			{
				Evict_Oldest_Buffer(vertex_buffers);
			}
			BufferEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.byte_count = byte_count;
			new_entry.last_used_frame = frame_id;
			BufferDescriptor descriptor;
			descriptor.byteCount = byte_count;
			descriptor.stride = vertex_buffer->FVF_Info().Get_FVF_Size();
			descriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
			descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
			if (device->createBuffer(descriptor, 0, 0, &new_entry.handle) !=
				rts::render::RENDER_RESULT_OK)
			{
				new_entry.source->Release();
				return Fail("draw failure: D3D11 vertex buffer creation");
			}
			try
			{
				vertex_buffers.push_back(new_entry);
			}
			catch (...)
			{
				device->destroyResource(new_entry.handle);
				new_entry.source->Release();
				return Fail("draw failure: vertex buffer cache allocation");
			}
			entry = &vertex_buffers.back();
		}
		unsigned char *data = 0;
		HRESULT result = source->Lock(0, static_cast<UINT>(byte_count),
			&data, D3DLOCK_READONLY);
		if (FAILED(result))
		{
			result = source->Lock(0, static_cast<UINT>(byte_count), &data, 0);
		}
		if (FAILED(result) || data == 0)
		{
			return Fail("draw failure: legacy vertex buffer lock");
		}
		const RenderResult upload_result = context->updateBuffer(entry->handle,
			data, byte_count, 0);
		source->Unlock();
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail("draw failure: D3D11 vertex buffer upload");
		}
		*handle = entry->handle;
		return true;
	}

	bool Upload_Index_Buffer(IndexBufferClass *index_buffer, GpuHandle *handle)
	{
		if (index_buffer == 0 || handle == 0 ||
			(index_buffer->Type() != BUFFER_TYPE_DX8 &&
			 index_buffer->Type() != BUFFER_TYPE_DYNAMIC_DX8))
		{
			return Fail("draw failure: unsupported index buffer");
		}
		IDirect3DIndexBuffer8 *source = static_cast<DX8IndexBufferClass *>(
			index_buffer)->Get_DX8_Index_Buffer();
		const size_t byte_count = static_cast<size_t>(
			index_buffer->Get_Index_Count()) * sizeof(unsigned short);
		BufferEntry *entry = Find_Buffer(index_buffers, source);
		if (entry == 0)
		{
			if (index_buffers.size() >= BUFFER_CACHE_CAPACITY)
			{
				Evict_Oldest_Buffer(index_buffers);
			}
			BufferEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.byte_count = byte_count;
			new_entry.last_used_frame = frame_id;
			BufferDescriptor descriptor;
			descriptor.byteCount = byte_count;
			descriptor.stride = sizeof(unsigned short);
			descriptor.binding = rts::render::RENDER_BUFFER_INDEX;
			descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
			if (device->createBuffer(descriptor, 0, 0, &new_entry.handle) !=
				rts::render::RENDER_RESULT_OK)
			{
				new_entry.source->Release();
				return Fail("draw failure: D3D11 index buffer creation");
			}
			try
			{
				index_buffers.push_back(new_entry);
			}
			catch (...)
			{
				device->destroyResource(new_entry.handle);
				new_entry.source->Release();
				return Fail("draw failure: index buffer cache allocation");
			}
			entry = &index_buffers.back();
		}
		unsigned char *data = 0;
		HRESULT result = source->Lock(0, static_cast<UINT>(byte_count),
			&data, D3DLOCK_READONLY);
		if (FAILED(result))
		{
			result = source->Lock(0, static_cast<UINT>(byte_count), &data, 0);
		}
		if (FAILED(result) || data == 0)
		{
			return Fail("draw failure: legacy index buffer lock");
		}
		const RenderResult upload_result = context->updateBuffer(entry->handle,
			data, byte_count, 0);
		source->Unlock();
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail("draw failure: D3D11 index buffer upload");
		}
		*handle = entry->handle;
		return true;
	}

	bool Copy_Surface(IDirect3DSurface8 *source, unsigned int width,
		unsigned int height, std::vector<unsigned char> *pixels)
	{
		IDirect3DSurface8 *staging = 0;
		if (FAILED(legacy_device->CreateImageSurface(width, height,
			D3DFMT_A8R8G8B8, &staging)))
		{
			return false;
		}
		HRESULT result = D3DXLoadSurfaceFromSurface(staging, 0, 0, source,
			0, 0, D3DX_FILTER_NONE, 0);
		D3DLOCKED_RECT locked;
		if (SUCCEEDED(result))
		{
			result = staging->LockRect(&locked, 0, D3DLOCK_READONLY);
		}
		if (FAILED(result))
		{
			staging->Release();
			return false;
		}
		try
		{
			pixels->resize(static_cast<size_t>(width) * height * 4);
			for (unsigned int row = 0; row < height; ++row)
			{
				memcpy(&(*pixels)[static_cast<size_t>(row) * width * 4],
					static_cast<const unsigned char *>(locked.pBits) +
						static_cast<size_t>(row) * locked.Pitch,
					static_cast<size_t>(width) * 4);
			}
		}
		catch (...)
		{
			staging->UnlockRect();
			staging->Release();
			return false;
		}
		staging->UnlockRect();
		staging->Release();
		return true;
	}

	bool Create_Texture(IDirect3DBaseTexture8 *source, GpuHandle *handle,
		bool *render_target)
	{
		if (source == 0 || handle == 0 || render_target == 0)
		{
			return false;
		}
		TextureDescriptor descriptor;
		descriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		descriptor.binding = rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
		descriptor.usage = rts::render::RENDER_USAGE_IMMUTABLE;
		std::vector<std::vector<unsigned char> > pixels;
		std::vector<TextureSubresourceData> subresources;
		if (source->GetType() == D3DRTYPE_TEXTURE)
		{
			IDirect3DTexture8 *texture = static_cast<IDirect3DTexture8 *>(source);
			descriptor.mipCount = texture->GetLevelCount();
			D3DSURFACE_DESC level_zero;
			if (FAILED(texture->GetLevelDesc(0, &level_zero)))
			{
				return false;
			}
			descriptor.width = level_zero.Width;
			descriptor.height = level_zero.Height;
			*render_target = (level_zero.Usage & D3DUSAGE_RENDERTARGET) != 0;
			try
			{
				pixels.resize(descriptor.mipCount);
				subresources.resize(descriptor.mipCount);
			}
			catch (...)
			{
				return false;
			}
			for (unsigned int mip = 0; mip < descriptor.mipCount; ++mip)
			{
				D3DSURFACE_DESC level;
				IDirect3DSurface8 *surface = 0;
				if (FAILED(texture->GetLevelDesc(mip, &level)) ||
					FAILED(texture->GetSurfaceLevel(mip, &surface)))
				{
					if (surface != 0) surface->Release();
					return false;
				}
				const bool copied = Copy_Surface(surface, level.Width, level.Height,
					&pixels[mip]);
				surface->Release();
				if (!copied)
				{
					return false;
				}
				subresources[mip].data = &pixels[mip][0];
				subresources[mip].rowPitch = static_cast<size_t>(level.Width) * 4;
				subresources[mip].slicePitch = pixels[mip].size();
			}
		}
		else if (source->GetType() == D3DRTYPE_CUBETEXTURE)
		{
			IDirect3DCubeTexture8 *texture =
				static_cast<IDirect3DCubeTexture8 *>(source);
			descriptor.dimension = rts::render::RENDER_TEXTURE_CUBE;
			descriptor.arrayCount = 6;
			descriptor.mipCount = texture->GetLevelCount();
			D3DSURFACE_DESC level_zero;
			if (FAILED(texture->GetLevelDesc(0, &level_zero)))
			{
				return false;
			}
			descriptor.width = level_zero.Width;
			descriptor.height = level_zero.Height;
			*render_target = false;
			const unsigned int count = descriptor.mipCount * 6;
			try
			{
				pixels.resize(count);
				subresources.resize(count);
			}
			catch (...)
			{
				return false;
			}
			for (unsigned int face = 0; face < 6; ++face)
			{
				for (unsigned int mip = 0; mip < descriptor.mipCount; ++mip)
				{
					const unsigned int index = face * descriptor.mipCount + mip;
					D3DSURFACE_DESC level;
					IDirect3DSurface8 *surface = 0;
					if (FAILED(texture->GetLevelDesc(mip, &level)) ||
						FAILED(texture->GetCubeMapSurface(
							static_cast<D3DCUBEMAP_FACES>(face), mip, &surface)))
					{
						if (surface != 0) surface->Release();
						return false;
					}
					const bool copied = Copy_Surface(surface, level.Width, level.Height,
						&pixels[index]);
					surface->Release();
					if (!copied)
					{
						return false;
					}
					subresources[index].data = &pixels[index][0];
					subresources[index].rowPitch =
						static_cast<size_t>(level.Width) * 4;
					subresources[index].slicePitch = pixels[index].size();
				}
			}
		}
		else
		{
			return false;
		}
		return device->createTexture(descriptor, &subresources[0],
			static_cast<unsigned int>(subresources.size()), handle) ==
			rts::render::RENDER_RESULT_OK;
	}

	bool Bind_Texture(unsigned int stage, IDirect3DBaseTexture8 *source,
		GpuHandle *handle)
	{
		if (source == 0)
		{
			*handle = GpuHandle();
			return context->setTexture(stage, *handle) ==
				rts::render::RENDER_RESULT_OK;
		}
		TextureEntry *entry = 0;
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			if (textures[index].source == source)
			{
				entry = &textures[index];
				entry->last_used_frame = frame_id;
				break;
			}
		}
		if (entry != 0 && entry->render_target)
		{
			device->destroyResource(entry->handle);
			entry->handle = GpuHandle();
			if (!Create_Texture(source, &entry->handle, &entry->render_target))
			{
				return false;
			}
		}
		else if (entry == 0)
		{
			if (textures.size() >= TEXTURE_CACHE_CAPACITY)
			{
				Evict_Oldest_Texture();
			}
			TextureEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.last_used_frame = frame_id;
			if (!Create_Texture(source, &new_entry.handle,
				&new_entry.render_target))
			{
				new_entry.source->Release();
				return false;
			}
			try
			{
				textures.push_back(new_entry);
			}
			catch (...)
			{
				device->destroyResource(new_entry.handle);
				new_entry.source->Release();
				return false;
			}
			entry = &textures.back();
		}
		*handle = entry->handle;
		return context->setTexture(stage, entry->handle) ==
			rts::render::RENDER_RESULT_OK;
	}

	void Release_Caches()
	{
		for (unsigned int index = 0; index < vertex_buffers.size(); ++index)
		{
			device->destroyResource(vertex_buffers[index].handle);
			vertex_buffers[index].source->Release();
		}
		for (unsigned int index = 0; index < index_buffers.size(); ++index)
		{
			device->destroyResource(index_buffers[index].handle);
			index_buffers[index].source->Release();
		}
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			device->destroyResource(textures[index].handle);
			textures[index].source->Release();
		}
		vertex_buffers.clear();
		index_buffers.clear();
		textures.clear();
	}
};

D3D11LegacyBridge::D3D11LegacyBridge() : m_impl(new(std::nothrow) Impl) {}

D3D11LegacyBridge::~D3D11LegacyBridge()
{
	Shutdown();
	delete m_impl;
}

bool D3D11LegacyBridge::Initialize(HWND window,
	IDirect3DDevice8 *legacy_device, unsigned int width, unsigned int height,
	bool enable_vsync)
{
	if (m_impl == 0)
	{
		return false;
	}
	if (m_impl->log_file == 0)
	{
		m_impl->log_file = fopen("D3D11Renderer.log", "wt");
	}
	m_impl->Log("D3D11 legacy bridge initialization requested");
	if (m_impl->device != 0 || window == 0 ||
		legacy_device == 0 || width == 0 || height == 0)
	{
		m_impl->Log("D3D11 legacy bridge rejected invalid initialization state");
		return false;
	}
	m_impl->device = rts::render::CreateD3D11RenderDevice();
	if (m_impl->device == 0)
	{
		m_impl->Log("D3D11 render device allocation failed");
		return false;
	}
	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = width;
	parameters.height = height;
	parameters.enableVsync = enable_vsync;
	parameters.enableDebugLayer = false;
	const RenderResult initialize_result =
		m_impl->device->initialize(parameters);
	if (initialize_result != rts::render::RENDER_RESULT_OK)
	{
		if (m_impl->log_file != 0)
		{
			fprintf(m_impl->log_file, "D3D11 render device initialization failed: %d\n",
				static_cast<int>(initialize_result));
			fflush(m_impl->log_file);
		}
		delete m_impl->device;
		m_impl->device = 0;
		return false;
	}
	m_impl->context = m_impl->device->immediateContext();
	m_impl->legacy_device = legacy_device;
	m_impl->legacy_device->AddRef();
	m_impl->Log("D3D11 legacy bridge initialized");
	if (m_impl->context == 0)
	{
		m_impl->Log("D3D11 immediate context is unavailable");
		Shutdown();
		return false;
	}
	return true;
}

void D3D11LegacyBridge::Shutdown()
{
	if (m_impl == 0 || m_impl->device == 0)
	{
		return;
	}
	if (m_impl->frame_open)
	{
		m_impl->context->endFrame();
		m_impl->frame_open = false;
	}
	if (m_impl->log_file != 0)
	{
		fprintf(m_impl->log_file,
			"draws=%u failures=%u vertex_buffers=%u index_buffers=%u textures=%u\n",
			m_impl->draw_count, m_impl->draw_failure_count,
			static_cast<unsigned int>(m_impl->vertex_buffers.size()),
			static_cast<unsigned int>(m_impl->index_buffers.size()),
			static_cast<unsigned int>(m_impl->textures.size()));
		fflush(m_impl->log_file);
	}
	m_impl->Release_Caches();
	if (m_impl->log_file != 0)
	{
		fclose(m_impl->log_file);
		m_impl->log_file = 0;
	}
	m_impl->device->shutdown();
	delete m_impl->device;
	m_impl->device = 0;
	m_impl->context = 0;
	if (m_impl->legacy_device != 0)
	{
		m_impl->legacy_device->Release();
		m_impl->legacy_device = 0;
	}
}

bool D3D11LegacyBridge::Is_Active() const
{
	return m_impl != 0 && m_impl->device != 0;
}

bool D3D11LegacyBridge::Begin_Frame()
{
	if (!Is_Active() || m_impl->frame_open)
	{
		return false;
	}
	++m_impl->frame_id;
	if (m_impl->frame_id % 120 == 0)
	{
		m_impl->Prune_Stale_Caches();
	}
	m_impl->frame_open = m_impl->context->beginFrame() ==
		rts::render::RENDER_RESULT_OK;
	return m_impl->frame_open;
}

void D3D11LegacyBridge::End_Frame(bool present_frame)
{
	if (!Is_Active() || !m_impl->frame_open)
	{
		return;
	}
	m_impl->context->endFrame();
	m_impl->frame_open = false;
	if (present_frame)
	{
		const rts::render::RenderResult result = m_impl->device->present();
		if (result == rts::render::RENDER_RESULT_DEVICE_REMOVED)
		{
			m_impl->device->recoverDevice();
		}
	}
}

void D3D11LegacyBridge::Clear(bool clear_color, bool clear_depth_stencil,
	float red, float green, float blue, float alpha, float depth,
	unsigned int stencil)
{
	if (!Is_Active() || !m_impl->frame_open ||
		(!clear_color && !clear_depth_stencil))
	{
		return;
	}
	unsigned int clear_flags = 0;
	if (clear_color)
	{
		clear_flags |= rts::render::RENDER_CLEAR_COLOR;
	}
	if (clear_depth_stencil)
	{
		clear_flags |= rts::render::RENDER_CLEAR_DEPTH |
			rts::render::RENDER_CLEAR_STENCIL;
	}
	m_impl->context->clearTargets(clear_flags,
		rts::render::RenderFloat4(red, green, blue, alpha), depth, stencil);
}

void D3D11LegacyBridge::Set_Viewport(const D3DVIEWPORT8 &viewport)
{
	if (Is_Active() && m_impl->frame_open)
	{
		m_impl->context->setViewport(static_cast<float>(viewport.X),
			static_cast<float>(viewport.Y), static_cast<float>(viewport.Width),
			static_cast<float>(viewport.Height), viewport.MinZ, viewport.MaxZ);
	}
}

bool D3D11LegacyBridge::Draw(VertexBufferClass *vertex_buffer,
	IndexBufferClass *index_buffer, unsigned int primitive_type,
	unsigned int start_index, unsigned int primitive_count,
	unsigned int base_vertex)
{
	if (!Is_Active() || !m_impl->frame_open || vertex_buffer == 0 ||
		index_buffer == 0)
	{
		return Is_Active() ? m_impl->Fail("draw failure: invalid draw state") : false;
	}
	const unsigned int index_count = Primitive_Index_Count(primitive_type,
		primitive_count);
	if (index_count == 0)
	{
		return m_impl->Fail("draw failure: unsupported primitive topology");
	}
	GpuHandle vertex_handle;
	GpuHandle index_handle;
	if (!m_impl->Upload_Vertex_Buffer(vertex_buffer, &vertex_handle) ||
		!m_impl->Upload_Index_Buffer(index_buffer, &index_handle))
	{
		return false;
	}
	LegacyVertexLayout layout;
	if (!Build_Vertex_Layout(vertex_buffer->FVF_Info(), &layout))
	{
		return m_impl->Fail("draw failure: unsupported legacy vertex layout");
	}
	LegacyLogicalState state;
	if (!rts::render::GetTrackedLegacyLogicalState(&state))
	{
		return m_impl->Fail("draw failure: unavailable legacy logical state");
	}
	unsigned int texture_mask = 0;
	for (unsigned int stage = 0;
		stage < rts::render::LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		GpuHandle texture_handle;
		IDirect3DBaseTexture8 *source = 0;
		m_impl->legacy_device->GetTexture(stage, &source);
		const bool bound = m_impl->Bind_Texture(stage, source, &texture_handle);
		if (source != 0)
		{
			source->Release();
		}
		if (!bound)
		{
			return m_impl->Fail("draw failure: texture conversion or binding");
		}
		if (texture_handle.isValid())
		{
			texture_mask |= 1U << stage;
		}
	}
	const RenderResult state_result =
		m_impl->context->setLegacyStateForLayout(state, layout, texture_mask);
	if (state_result != rts::render::RENDER_RESULT_OK)
	{
		char message[384];
		unsigned int used = static_cast<unsigned int>(snprintf(message,
			sizeof(message),
			"draw failure: D3D11 state/layout result=%u fvf=0x%08x stride=%u elements=%u textures=0x%02x",
			static_cast<unsigned int>(state_result),
			vertex_buffer->FVF_Info().Get_FVF(), layout.stride,
			layout.elementCount, texture_mask));
		if (used >= sizeof(message))
		{
			used = sizeof(message) - 1;
			message[used] = '\0';
		}
		Append_Layout_Diagnostic(message, sizeof(message), &used, layout);
		return m_impl->Fail(message);
	}
	if (m_impl->context->setVertexBuffer(vertex_handle, layout.stride, 0) !=
		rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail("draw failure: D3D11 vertex buffer binding");
	}
	if (m_impl->context->setIndexBuffer(index_handle,
		rts::render::RENDER_FORMAT_R16_UINT, 0) !=
		rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail("draw failure: D3D11 index buffer binding");
	}
	if (m_impl->context->setPrimitiveTopology(
		Translate_Topology(primitive_type)) != rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail("draw failure: D3D11 topology binding");
	}
	if (m_impl->context->drawIndexed(index_count, start_index,
		static_cast<int>(base_vertex)) != rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail("draw failure: D3D11 indexed submission");
	}
	++m_impl->draw_count;
	if (m_impl->draw_count == 1)
	{
		m_impl->Log("first D3D11 legacy draw submitted");
	}
	return true;
}

bool D3D11LegacyBridge::Resize(unsigned int width, unsigned int height)
{
	return Is_Active() && m_impl->device->resize(width, height) ==
		rts::render::RENDER_RESULT_OK;
}

#else

struct D3D11LegacyBridge::Impl {};

D3D11LegacyBridge::D3D11LegacyBridge() : m_impl(0) {}
D3D11LegacyBridge::~D3D11LegacyBridge() {}
bool D3D11LegacyBridge::Initialize(HWND, IDirect3DDevice8 *, unsigned int,
	unsigned int, bool) { return false; }
void D3D11LegacyBridge::Shutdown() {}
bool D3D11LegacyBridge::Is_Active() const { return false; }
bool D3D11LegacyBridge::Begin_Frame() { return false; }
void D3D11LegacyBridge::End_Frame(bool) {}
void D3D11LegacyBridge::Clear(bool, bool, float, float, float, float, float,
	unsigned int) {}
void D3D11LegacyBridge::Set_Viewport(const D3DVIEWPORT8 &) {}
bool D3D11LegacyBridge::Draw(VertexBufferClass *, IndexBufferClass *,
	unsigned int, unsigned int, unsigned int, unsigned int) { return false; }
bool D3D11LegacyBridge::Resize(unsigned int, unsigned int) { return false; }

#endif
