#include "d3d11legacybridge.h"

#if defined(RTS_RENDERER_HAS_D3D11)

#include "dx8fvf.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "Renderer/LegacyRenderState.h"
#include "Renderer/RendererDevice.h"

#include <d3dx8tex.h>
#include <limits>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
const unsigned int BUFFER_CACHE_CAPACITY = 1024;
const unsigned int TEXTURE_CACHE_CAPACITY = 1024;
const unsigned int CACHE_STALE_FRAME_COUNT = 600;
const unsigned int MAX_TGA_DIMENSION = 65535;
const char *const VISUAL_SMOKE_CAPTURE_FILE = "D3D11RendererCapture.tga";
const char *const VISUAL_SMOKE_CAPTURE_SUCCESS_FILE =
	"D3D11RendererCapture.complete";

unsigned long Current_D3D11_Bridge_Thread_Id()
{
#if defined(_WIN32)
	return static_cast<unsigned long>(GetCurrentThreadId());
#else
	return 1;
#endif
}

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

bool Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right >
		std::numeric_limits<size_t>::max() / left))
	{
		return false;
	}
	*result = left * right;
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
		log_file(0), frame_id(0), draw_count(0), draw_failure_count(0),
		width(0), height(0), owner_thread_id(0), frame_outcome(), capture_queue(8),
		pending_viewport(false), pending_clear(false),
		pending_clear_color(false), pending_clear_depth_stencil(false),
		pending_red(0.0f), pending_green(0.0f), pending_blue(0.0f),
		pending_alpha(0.0f), pending_depth(1.0f), pending_stencil(0) {}

	IRenderDevice *device;
	IRenderContext *context;
	IDirect3DDevice8 *legacy_device;
	bool frame_open;
	FILE *log_file;
	unsigned int frame_id;
	unsigned int draw_count;
	unsigned int draw_failure_count;
	unsigned int width;
	unsigned int height;
	unsigned long owner_thread_id;
	rts::render::RenderFrameOutcome frame_outcome;
	rts::render::RenderCaptureQueue capture_queue;
	bool pending_viewport;
	D3DVIEWPORT8 viewport;
	bool pending_clear;
	bool pending_clear_color;
	bool pending_clear_depth_stencil;
	float pending_red;
	float pending_green;
	float pending_blue;
	float pending_alpha;
	float pending_depth;
	unsigned int pending_stencil;
	std::vector<BufferEntry> vertex_buffers;
	std::vector<BufferEntry> index_buffers;
	std::vector<TextureEntry> textures;

	bool Require_Owner_Thread(const char *operation)
	{
		if (owner_thread_id == 0 || owner_thread_id ==
			Current_D3D11_Bridge_Thread_Id())
		{
			return true;
		}
		Log("D3D11 legacy bridge owner-thread violation");
		(void)operation;
		abort();
		return false;
	}

	void Log(const char *message)
	{
		if (log_file != 0)
		{
			fprintf(log_file, "%s\n", message);
			fflush(log_file);
		}
	}

	void Log_Result(const char *message, RenderResult result)
	{
		char buffer[256];
		snprintf(buffer, sizeof(buffer), "%s (result=%u)", message,
			static_cast<unsigned int>(result));
		Log(buffer);
	}

	bool Record_Result(RenderResult result, const char *message)
	{
		if (result == rts::render::RENDER_RESULT_OK)
		{
			return true;
		}
		if (frame_outcome.recordCommandFailure(result))
		{
			Log_Result(message, result);
		}
		return false;
	}

	bool Fail(RenderResult result, const char *message)
	{
		++draw_failure_count;
		Record_Result(result, message);
		return false;
	}

	bool Fail(const char *message)
	{
		return Fail(rts::render::RENDER_RESULT_FAILED, message);
	}

	RenderResult Recover_Device()
	{
		capture_queue.advanceGeneration();
		capture_queue.cancelStale(rts::render::RENDER_RESULT_DEVICE_REMOVED);
		const RenderResult recovery_result = device->recoverDevice();
		if (recovery_result != rts::render::RENDER_RESULT_OK)
		{
			return recovery_result;
		}
		context = device->immediateContext();
		return context == 0 ? rts::render::RENDER_RESULT_FAILED :
			rts::render::RENDER_RESULT_OK;
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
			const RenderResult create_result = device->createBuffer(descriptor,
				0, 0, &new_entry.handle);
			if (create_result != rts::render::RENDER_RESULT_OK)
			{
				new_entry.source->Release();
				return Fail(create_result,
					"draw failure: D3D11 vertex buffer creation");
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
			return Fail(upload_result,
				"draw failure: D3D11 vertex buffer upload");
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
			const RenderResult create_result = device->createBuffer(descriptor,
				0, 0, &new_entry.handle);
			if (create_result != rts::render::RENDER_RESULT_OK)
			{
				new_entry.source->Release();
				return Fail(create_result,
					"draw failure: D3D11 index buffer creation");
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
			return Fail(upload_result,
				"draw failure: D3D11 index buffer upload");
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
		bool *render_target, RenderResult *failure_result)
	{
		if (failure_result != 0)
		{
			*failure_result = rts::render::RENDER_RESULT_FAILED;
		}
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
		const RenderResult create_result = device->createTexture(descriptor,
			&subresources[0], static_cast<unsigned int>(subresources.size()),
			handle);
		if (failure_result != 0)
		{
			*failure_result = create_result;
		}
		return create_result == rts::render::RENDER_RESULT_OK;
	}

	bool Bind_Texture(unsigned int stage, IDirect3DBaseTexture8 *source,
		GpuHandle *handle)
	{
		if (source == 0)
		{
			*handle = GpuHandle();
			const RenderResult bind_result = context->setTexture(stage, *handle);
			if (bind_result != rts::render::RENDER_RESULT_OK)
			{
				Fail(bind_result, "draw failure: D3D11 texture unbind");
			}
			return bind_result == rts::render::RENDER_RESULT_OK;
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
			RenderResult texture_result = rts::render::RENDER_RESULT_FAILED;
			if (!Create_Texture(source, &entry->handle, &entry->render_target,
				&texture_result))
			{
				Fail(texture_result, "draw failure: D3D11 render-target texture conversion");
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
			RenderResult texture_result = rts::render::RENDER_RESULT_FAILED;
			if (!Create_Texture(source, &new_entry.handle,
				&new_entry.render_target, &texture_result))
			{
				new_entry.source->Release();
				Fail(texture_result, "draw failure: D3D11 texture conversion");
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
				Fail(rts::render::RENDER_RESULT_OUT_OF_MEMORY,
					"draw failure: texture cache allocation");
				return false;
			}
			entry = &textures.back();
		}
		*handle = entry->handle;
		const RenderResult bind_result = context->setTexture(stage, entry->handle);
		if (bind_result != rts::render::RENDER_RESULT_OK)
		{
			Fail(bind_result, "draw failure: D3D11 texture binding");
		}
		return bind_result == rts::render::RENDER_RESULT_OK;
	}

	void Release_Caches()
	{
		const bool release_native_resources = device != 0 &&
			device->isOperational();
		for (unsigned int index = 0; index < vertex_buffers.size(); ++index)
		{
			if (release_native_resources)
			{
				device->destroyResource(vertex_buffers[index].handle);
			}
			vertex_buffers[index].source->Release();
		}
		for (unsigned int index = 0; index < index_buffers.size(); ++index)
		{
			if (release_native_resources)
			{
				device->destroyResource(index_buffers[index].handle);
			}
			index_buffers[index].source->Release();
		}
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			if (release_native_resources)
			{
				device->destroyResource(textures[index].handle);
			}
			textures[index].source->Release();
		}
		vertex_buffers.clear();
		index_buffers.clear();
		textures.clear();
	}

	static void Complete_Visual_Smoke(void *consumer,
		const rts::render::RenderCaptureHandle *, unsigned int capture_width,
		unsigned int capture_height, size_t row_pitch,
		rts::render::RenderFormat format, const void *capture_pixels,
		size_t pixel_bytes)
	{
		Impl *impl = static_cast<Impl *>(consumer);
		remove(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE);
		if (impl == 0 || capture_pixels == 0 || format !=
			rts::render::RENDER_FORMAT_B8G8R8A8_UNORM || capture_width == 0 ||
		capture_height == 0 || capture_width > MAX_TGA_DIMENSION ||
		capture_height > MAX_TGA_DIMENSION)
		{
			remove(VISUAL_SMOKE_CAPTURE_FILE);
			return;
		}
		size_t required_row_bytes = 0;
		size_t required_bytes = 0;
		if (!Checked_Multiply(static_cast<size_t>(capture_width), 4,
			&required_row_bytes) || row_pitch < required_row_bytes ||
			!Checked_Multiply(row_pitch, static_cast<size_t>(capture_height),
			&required_bytes) || pixel_bytes < required_bytes)
		{
			impl->Log("D3D11 visual smoke capture has invalid pixel bounds");
			remove(VISUAL_SMOKE_CAPTURE_FILE);
			return;
		}
		FILE *file = fopen(VISUAL_SMOKE_CAPTURE_FILE, "wb");
		if (file == 0)
		{
			impl->Log("D3D11 visual smoke capture could not open its output");
			remove(VISUAL_SMOKE_CAPTURE_FILE);
			return;
		}
		unsigned char header[18];
		memset(header, 0, sizeof(header));
		header[2] = 2;
		header[12] = static_cast<unsigned char>(capture_width & 0xff);
		header[13] = static_cast<unsigned char>((capture_width >> 8) & 0xff);
		header[14] = static_cast<unsigned char>(capture_height & 0xff);
		header[15] = static_cast<unsigned char>((capture_height >> 8) & 0xff);
		header[16] = 24;
		bool wrote_file = fwrite(header, 1, sizeof(header), file) ==
			sizeof(header);
		const unsigned char *pixels =
			static_cast<const unsigned char *>(capture_pixels);
		for (unsigned int row = capture_height; wrote_file && row != 0; --row)
		{
			const unsigned char *source = pixels +
				static_cast<size_t>(row - 1) * row_pitch;
			for (unsigned int column = 0; column < capture_width; ++column)
			{
				const unsigned char *pixel = source + column * 4;
				const unsigned char bgr[3] = { pixel[0], pixel[1], pixel[2] };
				if (fwrite(bgr, 1, sizeof(bgr), file) != sizeof(bgr))
				{
					wrote_file = false;
					break;
				}
			}
		}
		if (wrote_file && fflush(file) != 0)
		{
			wrote_file = false;
		}
		if (fclose(file) != 0)
		{
			wrote_file = false;
		}
		if (!wrote_file)
		{
			impl->Log("D3D11 visual smoke capture output was truncated");
			remove(VISUAL_SMOKE_CAPTURE_FILE);
			remove(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE);
			return;
		}
		FILE *marker = fopen(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE, "wb");
		if (marker == 0)
		{
			impl->Log("D3D11 visual smoke capture success marker could not be opened");
			remove(VISUAL_SMOKE_CAPTURE_FILE);
			remove(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE);
			return;
		}
		const char marker_text[] = "D3D11RendererCapture=complete\n";
		bool marker_written = fwrite(marker_text, 1,
			sizeof(marker_text) - 1, marker) == sizeof(marker_text) - 1;
		if (marker_written && fflush(marker) != 0)
		{
			marker_written = false;
		}
		if (fclose(marker) != 0)
		{
			marker_written = false;
		}
		if (!marker_written)
		{
			impl->Log("D3D11 visual smoke capture success marker was truncated");
			remove(VISUAL_SMOKE_CAPTURE_FILE);
			remove(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE);
			return;
		}
		impl->Log("D3D11 visual smoke frame captured from the pre-Present back buffer");
	}

	static void Cancel_Visual_Smoke(void *consumer,
		const rts::render::RenderCaptureHandle *, rts::render::RenderResult)
	{
		Impl *impl = static_cast<Impl *>(consumer);
		remove(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE);
		remove(VISUAL_SMOKE_CAPTURE_FILE);
		if (impl != 0)
		{
			impl->Log("D3D11 visual smoke capture was cancelled");
		}
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
	if (m_impl->device != 0)
	{
		m_impl->Log("D3D11 legacy bridge rejected initialization while active");
		return false;
	}
	if (window == 0 || legacy_device == 0 || width == 0 || height == 0)
	{
		m_impl->Log("D3D11 legacy bridge rejected invalid initialization state");
		if (m_impl->log_file != 0)
		{
			fclose(m_impl->log_file);
			m_impl->log_file = 0;
		}
		return false;
	}
	if (!m_impl->capture_queue.bindOwnerThread())
	{
		m_impl->Log("D3D11 legacy bridge rejected initialization from a non-owner thread");
		return false;
	}
	m_impl->owner_thread_id = Current_D3D11_Bridge_Thread_Id();
	m_impl->capture_queue.reset();
	m_impl->device = rts::render::CreateD3D11RenderDevice();
	if (m_impl->device == 0)
	{
		m_impl->Log("D3D11 render device allocation failed");
		Shutdown();
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
		Shutdown();
		return false;
	}
	m_impl->context = m_impl->device->immediateContext();
	m_impl->width = width;
	m_impl->height = height;
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
	if (m_impl == 0)
	{
		return;
	}
	if (m_impl->device == 0)
	{
		if (m_impl->owner_thread_id != 0)
		{
			m_impl->Require_Owner_Thread("failed initialization shutdown");
		}
		m_impl->capture_queue.shutdown(rts::render::RENDER_RESULT_FAILED);
		if (m_impl->log_file != 0)
		{
			fclose(m_impl->log_file);
			m_impl->log_file = 0;
		}
		m_impl->frame_outcome = rts::render::RenderFrameOutcome();
		m_impl->owner_thread_id = 0;
		return;
	}
	m_impl->Require_Owner_Thread("shutdown");
	if (m_impl->frame_open)
	{
		if (m_impl->context != 0 && m_impl->device->isOperational())
		{
			m_impl->context->endFrame();
		}
		m_impl->frame_open = false;
	}
	// Complete cancellation while the render owner, bridge log, and consumer
	// objects are still alive. The queue is owner-thread bound, so this must
	// happen before releasing the device and before closing the log.
	m_impl->capture_queue.shutdown(rts::render::RENDER_RESULT_FAILED);
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
	m_impl->frame_open = false;
	m_impl->width = 0;
	m_impl->height = 0;
	m_impl->frame_outcome = rts::render::RenderFrameOutcome();
	m_impl->pending_viewport = false;
	m_impl->pending_clear = false;
	m_impl->owner_thread_id = 0;
}

bool D3D11LegacyBridge::Is_Active() const
{
	return m_impl != 0 && m_impl->device != 0 &&
		m_impl->context != 0 && m_impl->device->isOperational();
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
	if (!m_impl->frame_open)
	{
		m_impl->Log("D3D11 legacy bridge begin-frame failed");
		return false;
	}
	m_impl->frame_outcome = rts::render::RenderFrameOutcome();
	if (m_impl->pending_viewport)
	{
		const RenderResult viewport_result = m_impl->context->setViewport(
			static_cast<float>(m_impl->viewport.X),
			static_cast<float>(m_impl->viewport.Y),
			static_cast<float>(m_impl->viewport.Width),
			static_cast<float>(m_impl->viewport.Height),
			m_impl->viewport.MinZ, m_impl->viewport.MaxZ);
		if (!m_impl->Record_Result(viewport_result,
			"D3D11 legacy bridge pending viewport failed"))
		{
			m_impl->context->endFrame();
			m_impl->frame_open = false;
			return false;
		}
		m_impl->pending_viewport = false;
	}
	if (m_impl->pending_clear)
	{
		unsigned int clear_flags = 0;
		if (m_impl->pending_clear_color)
		{
			clear_flags |= rts::render::RENDER_CLEAR_COLOR;
		}
		if (m_impl->pending_clear_depth_stencil)
		{
			clear_flags |= rts::render::RENDER_CLEAR_DEPTH |
				rts::render::RENDER_CLEAR_STENCIL;
		}
		const RenderResult clear_result = m_impl->context->clearTargets(
			clear_flags, rts::render::RenderFloat4(m_impl->pending_red,
				m_impl->pending_green, m_impl->pending_blue,
				m_impl->pending_alpha), m_impl->pending_depth,
			m_impl->pending_stencil);
		if (!m_impl->Record_Result(clear_result,
			"D3D11 legacy bridge pending clear failed"))
		{
			m_impl->context->endFrame();
			m_impl->frame_open = false;
			return false;
		}
		m_impl->pending_clear = false;
	}
	return m_impl->frame_open;
}

void D3D11LegacyBridge::Request_Frame_Capture()
{
	if (Is_Active())
	{
		m_impl->Require_Owner_Thread("frame capture request");
		remove(VISUAL_SMOKE_CAPTURE_SUCCESS_FILE);
		remove(VISUAL_SMOKE_CAPTURE_FILE);
		rts::render::RenderCaptureRequestDescriptor descriptor;
		descriptor.kind = rts::render::RENDER_CAPTURE_VISUAL_SMOKE;
		descriptor.consumer = m_impl;
		descriptor.completed = &Impl::Complete_Visual_Smoke;
		descriptor.cancelled = &Impl::Cancel_Visual_Smoke;
		rts::render::RenderCaptureHandle handle;
		const RenderResult result = m_impl->capture_queue.enqueue(descriptor,
			&handle);
		if (result != rts::render::RENDER_RESULT_OK)
		{
			m_impl->Log_Result("D3D11 visual smoke capture queue rejected request",
				result);
		}
	}
}

RenderResult D3D11LegacyBridge::Get_Back_Buffer_Info(
	rts::render::RenderBackBufferInfo *info) const
{
	if (!Is_Active() || info == 0)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	return m_impl->device->getBackBufferInfo(info);
}

RenderResult D3D11LegacyBridge::Queue_Back_Buffer_Capture(
	const rts::render::RenderCaptureRequestDescriptor &descriptor,
	rts::render::RenderCaptureHandle *handle)
{
	if (!Is_Active())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	return m_impl->capture_queue.enqueue(descriptor, handle);
}

unsigned int D3D11LegacyBridge::Cancel_Back_Buffer_Captures(void *consumer,
	rts::render::RenderResult reason)
{
	if (m_impl == 0)
	{
		return 0;
	}
	if (m_impl->owner_thread_id != 0)
	{
		m_impl->Require_Owner_Thread("frame capture cancellation");
	}
	return m_impl->capture_queue.cancelConsumer(consumer, reason);
}

RenderResult D3D11LegacyBridge::End_Frame(bool present_frame)
{
	return End_Frame(present_frame, 0);
}

RenderResult D3D11LegacyBridge::End_Frame(bool present_frame,
	rts::render::RenderFrameOutcome *outcome)
{
	if (!Is_Active() || !m_impl->frame_open)
	{
		if (outcome != 0)
		{
			*outcome = rts::render::RenderFrameOutcome();
			outcome->setOperational(Is_Active());
		}
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult end_result = m_impl->context->endFrame();
	m_impl->frame_open = false;
	m_impl->frame_outcome.recordEndFrame(end_result);
	m_impl->frame_outcome.markFrameEnded();
	if (end_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 legacy bridge end-frame failed", end_result);
	}
	if (m_impl->frame_outcome.hasDeviceRemoval())
	{
		const RenderResult recovery_result = m_impl->Recover_Device();
		m_impl->frame_outcome.recordRecovery(recovery_result);
		if (recovery_result != rts::render::RENDER_RESULT_OK)
		{
			m_impl->Log_Result(
				"D3D11 legacy bridge device recovery failed", recovery_result);
			m_impl->frame_outcome.setOperational(false);
			const RenderResult frame_result = m_impl->frame_outcome.result();
			if (outcome != 0)
			{
				*outcome = m_impl->frame_outcome;
			}
			Shutdown();
			return frame_result;
		}
		m_impl->frame_outcome.setOperational(Is_Active());
		m_impl->Log("D3D11 legacy bridge recovered after a device-removal command");
		if (outcome != 0)
		{
			*outcome = m_impl->frame_outcome;
		}
		return m_impl->frame_outcome.result();
	}
	if (end_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->capture_queue.cancelCurrent(end_result);
		m_impl->frame_outcome.setOperational(Is_Active());
		if (outcome != 0)
		{
			*outcome = m_impl->frame_outcome;
		}
		return m_impl->frame_outcome.result();
	}
	if (!present_frame)
	{
		// A render-to-texture or otherwise non-visible pass must not consume a
		// visible-frame capture request.
		m_impl->frame_outcome.setOperational(Is_Active());
		if (outcome != 0)
		{
			*outcome = m_impl->frame_outcome;
		}
		return m_impl->frame_outcome.result();
	}
	// Flip-discard may expose a different/undefined back buffer after Present.
	// Capture while the completed visible frame is still current, then fulfill
	// all queued consumers after Present on this owner thread.
	rts::render::RenderBackBufferInfo capture_info;
	std::vector<unsigned char> capture_pixels;
	bool capture_ready = false;
	RenderResult info_result = rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	RenderResult capture_result = rts::render::RENDER_RESULT_OK;
	if (present_frame && m_impl->capture_queue.pendingCount() != 0)
	{
		info_result = m_impl->device->getBackBufferInfo(
			&capture_info);
		capture_result = info_result;
		if (info_result == rts::render::RENDER_RESULT_OK &&
			capture_info.format == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM)
		{
			size_t row_pitch = 0;
			size_t pixel_bytes = 0;
			if (Checked_Multiply(static_cast<size_t>(capture_info.width), 4,
				&row_pitch) && Checked_Multiply(row_pitch,
				static_cast<size_t>(capture_info.height), &pixel_bytes) &&
				pixel_bytes != 0)
			{
				try
				{
					capture_pixels.resize(pixel_bytes);
					capture_result = m_impl->device->captureBackBuffer(
							&capture_pixels[0], capture_pixels.size(), row_pitch,
							&capture_info.format);
					capture_ready = capture_result ==
						rts::render::RENDER_RESULT_OK;
					if (!capture_ready)
					{
						m_impl->Log_Result(
							"D3D11 visible capture readback failed", capture_result);
					}
				}
				catch (...)
				{
					capture_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
					m_impl->Log("D3D11 visible capture allocation failed");
				}
			}
			else
			{
				capture_result = rts::render::RENDER_RESULT_INVALID_ARGUMENT;
			}
		}
		else if (info_result == rts::render::RENDER_RESULT_OK)
		{
			capture_result = rts::render::RENDER_RESULT_UNSUPPORTED;
		}
		m_impl->frame_outcome.recordCapture(capture_result);
		if (!capture_ready)
		{
			m_impl->capture_queue.cancelCurrent(capture_result);
		}
		if (capture_result == rts::render::RENDER_RESULT_DEVICE_REMOVED)
		{
			const RenderResult recovery_result = m_impl->Recover_Device();
			m_impl->frame_outcome.recordRecovery(recovery_result);
			if (recovery_result != rts::render::RENDER_RESULT_OK)
			{
				m_impl->Log_Result(
					"D3D11 legacy bridge device recovery failed", recovery_result);
				m_impl->frame_outcome.setOperational(false);
				const RenderResult frame_result = m_impl->frame_outcome.result();
				if (outcome != 0)
				{
					*outcome = m_impl->frame_outcome;
				}
				Shutdown();
				return frame_result;
			}
			m_impl->frame_outcome.setOperational(Is_Active());
			m_impl->Log(
				"D3D11 legacy bridge recovered after capture device removal");
			if (outcome != 0)
			{
				*outcome = m_impl->frame_outcome;
			}
			return m_impl->frame_outcome.result();
		}
	}
	const RenderResult present_result = m_impl->device->present();
	m_impl->frame_outcome.recordPresentation(present_result);
	if (present_result == rts::render::RENDER_RESULT_DEVICE_REMOVED)
	{
		const RenderResult recovery_result = m_impl->Recover_Device();
		m_impl->frame_outcome.recordRecovery(recovery_result);
		if (recovery_result != rts::render::RENDER_RESULT_OK)
		{
			m_impl->Log_Result(
				"D3D11 legacy bridge device recovery failed", recovery_result);
			m_impl->frame_outcome.setOperational(false);
			const RenderResult frame_result = m_impl->frame_outcome.result();
			if (outcome != 0)
			{
				*outcome = m_impl->frame_outcome;
			}
			Shutdown();
			return frame_result;
		}
		m_impl->frame_outcome.setOperational(Is_Active());
		m_impl->Log("D3D11 legacy bridge recovered the device after present");
		if (outcome != 0)
		{
			*outcome = m_impl->frame_outcome;
		}
		return m_impl->frame_outcome.result();
	}
	if (present_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->capture_queue.cancelCurrent(present_result);
		m_impl->Log_Result("D3D11 legacy bridge present failed", present_result);
		m_impl->frame_outcome.setOperational(Is_Active());
		if (outcome != 0)
		{
			*outcome = m_impl->frame_outcome;
		}
		return m_impl->frame_outcome.result();
	}
	m_impl->frame_outcome.markPresented();
	if (capture_ready)
	{
		const size_t row_pitch = static_cast<size_t>(capture_info.width) * 4;
		const RenderResult completion_result = m_impl->capture_queue.completeVisible(
			capture_info.width,
			capture_info.height, row_pitch, capture_info.format,
			&capture_pixels[0], capture_pixels.size());
		if (completion_result != rts::render::RENDER_RESULT_OK)
		{
			m_impl->capture_queue.cancelCurrent(completion_result);
			m_impl->Log_Result("D3D11 visible capture completion rejected",
				completion_result);
		}
	}
	if (m_impl->frame_outcome.hasCommandFailure())
	{
		m_impl->Log_Result(
			"D3D11 legacy bridge presented a partial frame after a command failure",
			m_impl->frame_outcome.commandResult());
	}
	m_impl->frame_outcome.setOperational(Is_Active());
	if (outcome != 0)
	{
		*outcome = m_impl->frame_outcome;
	}
	return m_impl->frame_outcome.result();
}

void D3D11LegacyBridge::Clear(bool clear_color, bool clear_depth_stencil,
	float red, float green, float blue, float alpha, float depth,
	unsigned int stencil)
{
	if (!Is_Active() || (!clear_color && !clear_depth_stencil))
	{
		return;
	}
	if (!m_impl->frame_open)
	{
		m_impl->pending_clear = true;
		m_impl->pending_clear_color = clear_color;
		m_impl->pending_clear_depth_stencil = clear_depth_stencil;
		m_impl->pending_red = red;
		m_impl->pending_green = green;
		m_impl->pending_blue = blue;
		m_impl->pending_alpha = alpha;
		m_impl->pending_depth = depth;
		m_impl->pending_stencil = stencil;
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
	m_impl->Record_Result(m_impl->context->clearTargets(clear_flags,
		rts::render::RenderFloat4(red, green, blue, alpha), depth, stencil),
		"D3D11 legacy bridge clear failed");
}

void D3D11LegacyBridge::Set_Viewport(const D3DVIEWPORT8 &viewport)
{
	if (!Is_Active())
	{
		return;
	}
	if (!m_impl->frame_open)
	{
		m_impl->viewport = viewport;
		m_impl->pending_viewport = true;
		return;
	}
	if (m_impl->frame_open)
	{
		m_impl->Record_Result(m_impl->context->setViewport(
			static_cast<float>(viewport.X),
			static_cast<float>(viewport.Y), static_cast<float>(viewport.Width),
			static_cast<float>(viewport.Height), viewport.MinZ, viewport.MaxZ),
			"D3D11 legacy bridge viewport failed");
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
		return m_impl->Fail(state_result, message);
	}
	const RenderResult vertex_bind_result = m_impl->context->setVertexBuffer(
		vertex_handle, layout.stride, 0);
	if (vertex_bind_result != rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail(vertex_bind_result,
			"draw failure: D3D11 vertex buffer binding");
	}
	const RenderResult index_bind_result = m_impl->context->setIndexBuffer(
		index_handle, rts::render::RENDER_FORMAT_R16_UINT, 0);
	if (index_bind_result != rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail(index_bind_result,
			"draw failure: D3D11 index buffer binding");
	}
	const RenderResult topology_result = m_impl->context->setPrimitiveTopology(
		Translate_Topology(primitive_type));
	if (topology_result != rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail(topology_result,
			"draw failure: D3D11 topology binding");
	}
	const RenderResult draw_result = m_impl->context->drawIndexed(index_count,
		start_index, static_cast<int>(base_vertex));
	if (draw_result != rts::render::RENDER_RESULT_OK)
	{
		return m_impl->Fail(draw_result,
			"draw failure: D3D11 indexed submission");
	}
	++m_impl->draw_count;
	if (m_impl->draw_count == 1)
	{
		m_impl->Log("first D3D11 legacy draw submitted");
	}
	return true;
}

RenderResult D3D11LegacyBridge::Resize(unsigned int width, unsigned int height)
{
	if (!Is_Active())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->capture_queue.advanceGeneration();
	m_impl->capture_queue.cancelStale(rts::render::RENDER_RESULT_FAILED);
	const RenderResult result = m_impl->device->resize(width, height);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 legacy bridge resize failed", result);
		m_impl->context = m_impl->device->immediateContext();
		if (!m_impl->device->isOperational() || m_impl->context == 0)
		{
			m_impl->Log("D3D11 legacy bridge became inactive after resize failure");
			Shutdown();
		}
	}
	else
	{
		m_impl->width = width;
		m_impl->height = height;
	}
	return result;
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
void D3D11LegacyBridge::Request_Frame_Capture() {}
rts::render::RenderResult D3D11LegacyBridge::Get_Back_Buffer_Info(
	rts::render::RenderBackBufferInfo *) const
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
rts::render::RenderResult D3D11LegacyBridge::Queue_Back_Buffer_Capture(
	const rts::render::RenderCaptureRequestDescriptor &,
	rts::render::RenderCaptureHandle *)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
unsigned int D3D11LegacyBridge::Cancel_Back_Buffer_Captures(void *,
	rts::render::RenderResult)
{
	return 0;
}
rts::render::RenderResult D3D11LegacyBridge::End_Frame(bool)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
rts::render::RenderResult D3D11LegacyBridge::End_Frame(bool,
	rts::render::RenderFrameOutcome *outcome)
{
	if (outcome != 0)
	{
		*outcome = rts::render::RenderFrameOutcome();
		outcome->setOperational(false);
	}
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
void D3D11LegacyBridge::Clear(bool, bool, float, float, float, float, float,
	unsigned int) {}
void D3D11LegacyBridge::Set_Viewport(const D3DVIEWPORT8 &) {}
bool D3D11LegacyBridge::Draw(VertexBufferClass *, IndexBufferClass *,
	unsigned int, unsigned int, unsigned int, unsigned int) { return false; }
rts::render::RenderResult D3D11LegacyBridge::Resize(unsigned int, unsigned int)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
#endif
