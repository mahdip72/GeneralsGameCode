#include "d3d11legacybridge.h"

#if defined(_WIN64) && defined(RTS_RENDERER_HAS_D3D11)

#include "dx8wrapper.h"
#include "dx8fvf.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "nativew3d2.h"
#if defined(_WIN64)
#include "nativew3dbufferowner.h"
#include "nativew3dtextureowner.h"
#include "texture.h"
#endif
#include "surfaceblit.h"
#include "Renderer/LegacyBridgeCache.h"
#include "Renderer/LegacyBridgeValidation.h"
#include "Renderer/LegacyFvfLayout.h"
#include "Renderer/LegacyRenderState.h"
#include "Renderer/RenderTexturePublication.h"
#include "Renderer/RendererDevice.h"
#if defined(_WIN64)
#include "Renderer/ThreadedRenderDevice.h"
#include "Renderer/LegacyAsyncFramePolicy.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/ResourceIoPipeline.h"
#include "textureloader.h"
#endif
#include "Lib/FrameTimingDiagnostics.h"

#include <limits>
#include <math.h>
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
const size_t PRIMITIVE_UP_BUFFER_CAPACITY = 256 * 1024;
const unsigned int MAX_TEXTURE_REFRESH_SUBRESOURCES = 4096;
const size_t MAX_TEXTURE_REFRESH_BYTES = 256U * 1024U * 1024U;
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
using rts::render::LegacyVertexElement;
using rts::render::LegacyVertexLayout;
using rts::render::RenderVertexLayout;
using rts::render::RenderResult;
using rts::render::RenderBufferUpdateMode;
using rts::render::RenderTargetBinding;
using rts::render::TextureDescriptor;
using rts::render::TextureSubresourceData;

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

bool Build_Vertex_Layout(unsigned int fvf, unsigned int vertex_stride,
	LegacyVertexLayout *layout)
{
	if (layout == 0)
	{
		return false;
	}
	RenderVertexLayout native_layout;
	if (!rts::render::DecodeLegacyFvfVertexLayout(fvf, vertex_stride,
		&native_layout))
	{
		return false;
	}
	*layout = LegacyVertexLayout();
	layout->stride = native_layout.stride;
	layout->preTransformed = native_layout.preTransformed;
	layout->elementCount = native_layout.elementCount;
	for (unsigned int index = 0; index < native_layout.elementCount; ++index)
	{
		layout->elements[index].semantic = native_layout.elements[index].semantic;
		layout->elements[index].semanticIndex =
			native_layout.elements[index].semanticIndex;
		layout->elements[index].format = native_layout.elements[index].format;
		layout->elements[index].byteOffset = native_layout.elements[index].byteOffset;
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
	int written = snprintf(message + *used, capacity - *used,
		" pretransformed=%u layout=[", layout.preTransformed ? 1U : 0U);
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

bool Primitive_Index_Count(unsigned int primitive_type,
	unsigned int primitive_count, unsigned int *index_count)
{
	return rts::render::Checked_Legacy_Primitive_Index_Count(primitive_type,
		primitive_count, index_count);
}

bool Primitive_Up_Vertex_Count(unsigned int primitive_type,
	unsigned int primitive_count, size_t *vertex_count)
{
	if (vertex_count == 0)
	{
		return false;
	}
	switch (primitive_type)
	{
	case D3DPT_TRIANGLELIST:
		return Checked_Multiply(static_cast<size_t>(primitive_count), 3,
			vertex_count);
	case D3DPT_TRIANGLESTRIP:
		if (primitive_count > std::numeric_limits<size_t>::max() - 2)
		{
			return false;
		}
		*vertex_count = static_cast<size_t>(primitive_count) + 2;
		return true;
	case D3DPT_LINESTRIP:
		if (primitive_count > std::numeric_limits<size_t>::max() - 1)
		{
			return false;
		}
		*vertex_count = static_cast<size_t>(primitive_count) + 1;
		return true;
	case D3DPT_LINELIST:
		return Checked_Multiply(static_cast<size_t>(primitive_count), 2,
			vertex_count);
	default:
		return false;
	}
}

}

struct D3D11LegacyBridge::Impl
{
	struct BufferEntry
	{
		BufferEntry() : source(0), byte_count(0), source_generation(0),
			source_dirty(true), raw_range_valid(false), raw_range_offset(0),
			raw_range_bytes(0), raw_range_mode(
				rts::render::RENDER_BUFFER_UPDATE_PRESERVE),
			last_used_frame(0), handle() {}
		IUnknown *source;
		size_t byte_count;
		unsigned int source_generation;
		bool source_dirty;
		bool raw_range_valid;
		size_t raw_range_offset;
		size_t raw_range_bytes;
		rts::render::RenderBufferUpdateMode raw_range_mode;
		unsigned int last_used_frame;
		GpuHandle handle;
	};

	struct TextureEntry
	{
		TextureEntry() : source(0), render_target(false), depth_stencil(false),
			d3d11_authority(false), d3d8_dirty(false), gpu_copy_valid(false),
			gpu_copy_frame(0), gpu_copy_lease_epoch(0), last_used_frame(0),
			handle() {}
		IDirect3DBaseTexture8 *source;
		bool render_target;
		bool depth_stencil;
		bool d3d11_authority;
		bool d3d8_dirty;
		bool gpu_copy_valid;
		unsigned int gpu_copy_frame;
		unsigned int gpu_copy_lease_epoch;
		unsigned int last_used_frame;
		GpuHandle handle;
	};

	Impl() : native_w3d(0), native_buffer_resources_bound(false),
		native_texture_resources_bound(false), device(0),
		context(0), legacy_device(0), frame_open(false),
		log_file(0), frame_id(0), display_epoch(1), draw_count(0),
		draw_failure_count(0),
		raw_indexed_draw_count(0),
		counter_clockwise_draw_count(0), unculled_draw_count(0),
		color_write_draw_count(0), depth_never_draw_count(0),
		clear_count(0), viewport_count(0), frame_draw_count(0),
		dynamic_texture_refresh_count(0), dynamic_texture_in_place_count(0),
		dynamic_texture_recreate_count(0), capture_draw_logged(false),
		width(0), height(0), owner_thread_id(0), shutdown_pending(false),
		frame_outcome(), capture_queue(8),
		pending_viewport(false), pending_clear(false),
		pending_clear_color(false), pending_clear_depth_stencil(false),
		pending_red(0.0f), pending_green(0.0f), pending_blue(0.0f),
		pending_alpha(0.0f), pending_depth(1.0f), pending_stencil(0),
		active_target(), pending_target(), pending_target_change(false),
		target_transition_failed(false), primitive_up_vertex_buffer(),
		primitive_up_vertex_capacity(0), draw_texture_pinning(false),
		draw_texture_pins(), vertex_buffer_index(),
		index_buffer_index(), texture_index(), cache_counters()
	{
#if defined(_WIN64)
		for (unsigned int i = 0; i < ASYNC_FRAME_CAPACITY; ++i)
		{
			async_frames[i].sequence = 0;
			async_frames[i].frame = 0;
		}
		deferred_failure_sequence = 0;
		recovered_failure_sequence = 0;
		async_resource_failure = false;
#endif
	}

	NativeW3D2 *native_w3d;
	bool native_buffer_resources_bound;
	bool native_texture_resources_bound;
	IRenderDevice *device;
	IRenderContext *context;
	IDirect3DDevice8 *legacy_device;
	bool frame_open;
	FILE *log_file;
	unsigned int frame_id;
	unsigned int display_epoch;
	unsigned int draw_count;
	unsigned int draw_failure_count;
	unsigned int raw_indexed_draw_count;
	unsigned int counter_clockwise_draw_count;
	unsigned int unculled_draw_count;
	unsigned int color_write_draw_count;
	unsigned int depth_never_draw_count;
	unsigned int clear_count;
	unsigned int viewport_count;
	unsigned int frame_draw_count;
	unsigned int dynamic_texture_refresh_count;
	unsigned int dynamic_texture_in_place_count;
	unsigned int dynamic_texture_recreate_count;
	bool capture_draw_logged;
	unsigned int width;
	unsigned int height;
	unsigned long owner_thread_id;
	// A failed teardown leaves the bridge-owned device/resource graph intact
	// for an owner-thread retry, but it must not remain renderable in the
	// interim.
	bool shutdown_pending;
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
	RenderTargetBinding active_target;
	RenderTargetBinding pending_target;
	bool pending_target_change;
	bool target_transition_failed;
	GpuHandle primitive_up_vertex_buffer;
	size_t primitive_up_vertex_capacity;
	std::vector<BufferEntry> vertex_buffers;
	std::vector<BufferEntry> index_buffers;
	std::vector<TextureEntry> textures;
	bool draw_texture_pinning;
	rts::render::LegacyBridgeDrawTexturePins<
		rts::render::LEGACY_TEXTURE_STAGE_COUNT> draw_texture_pins;
	rts::render::LegacyBridgePointerIndex vertex_buffer_index;
	rts::render::LegacyBridgePointerIndex index_buffer_index;
	rts::render::LegacyBridgePointerIndex texture_index;
	rts::render::LegacyBridgeCacheCounters cache_counters;
#if defined(_WIN64)
	enum { ASYNC_FRAME_CAPACITY = 64 };
	struct AsyncFrame { uint64_t sequence; unsigned int frame; };
	AsyncFrame async_frames[ASYNC_FRAME_CAPACITY];
	rts::render::RenderFrameOutcome deferred_failure;
	uint64_t deferred_failure_sequence;
	uint64_t recovered_failure_sequence;
	bool async_resource_failure;

	bool Is_Threaded() const
	{
		return rts::render::IsThreadedRenderDevice(device);
	}

	// All publication is on the game owner. A sequence identifies the original
	// producer frame even when several newer frames have already been built.
	void Poll_Render_Completions(uint64_t wanted = 0,
		rts::render::ThreadedRenderFrameCompletion *matched = 0)
	{
		rts::render::ThreadedRenderFrameCompletion completed;
		while (rts::render::PollThreadedRenderCompletion(device, &completed))
		{
			if (native_w3d != 0 && native_w3d->IsAttachedToBorrowedBackend())
			{
				const RenderResult publication =
					native_w3d->PublishThreadedCompletion(completed.sequence,
						completed.resourceFailure);
				if (publication != rts::render::RENDER_RESULT_OK)
				{
					async_resource_failure = true;
					Log_Result("D3D11 native resource completion publication failed",
						publication);
				}
			}
			async_resource_failure = async_resource_failure || completed.resourceFailure;
			for (unsigned int i = 0; i < ASYNC_FRAME_CAPACITY; ++i)
			{
				if (async_frames[i].sequence == completed.sequence)
				{
					Complete_GPU_Copy_Frame(completed.result ==
						rts::render::RENDER_RESULT_OK, async_frames[i].frame);
					async_frames[i].sequence = 0;
					break;
				}
			}
			if (matched != 0 && wanted == completed.sequence) *matched = completed;
			if (completed.result != rts::render::RENDER_RESULT_OK)
			{
				Log_Result("D3D11 render-owner frame completion failed", completed.result);
				if (deferred_failure_sequence == 0 ||
					rts::render::ShouldReplaceLegacyAsyncFrameFailure(deferred_failure, completed.outcome,
						recovered_failure_sequence == deferred_failure_sequence))
				{
					deferred_failure = completed.outcome;
					deferred_failure_sequence = completed.sequence;
				}
			}
		}
	}

	bool Remember_Submission(uint64_t previous)
	{
		const uint64_t sequence = rts::render::LastThreadedRenderFrameSequence(device);
		if (sequence == previous || sequence == 0) return false;
		for (unsigned int i = 0; i < ASYNC_FRAME_CAPACITY; ++i)
		{
			if (async_frames[i].sequence == 0)
			{
				async_frames[i].sequence = sequence;
				async_frames[i].frame = frame_id;
				return true;
			}
		}
		// The proxy reserves one completion slot at BeginFrame. Exhaustion here
		// is an integration ownership error, not an allocation fallback.
		Log("D3D11 bridge exhausted reserved frame-completion records");
		abort();
		return false;
	}

	void Cancel_Threaded_Frame(RenderResult reason)
	{
		if (!Is_Threaded()) return;
		const uint64_t previous = rts::render::LastThreadedRenderFrameSequence(device);
		rts::render::CancelThreadedRenderFrame(device, reason);
		Remember_Submission(previous);
	}

	RenderResult Fence_Render()
	{
		if (!Is_Threaded()) return rts::render::RENDER_RESULT_OK;
		const RenderResult result = rts::render::DrainThreadedRenderDevice(device);
		Poll_Render_Completions();
		return result;
	}

	IDirect3DSurface8 *Retain_Target_Surface(const rts::render::RenderTargetSubresource &target)
	{
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			TextureEntry &entry = textures[index];
			if (entry.handle != target.resource || entry.source == 0 ||
				entry.source->GetType() != D3DRTYPE_TEXTURE) continue;
			IDirect3DSurface8 *surface = 0;
			IDirect3DTexture8 *texture = static_cast<IDirect3DTexture8 *>(entry.source);
			if (FAILED(texture->GetSurfaceLevel(target.mip, &surface)))
			{
				if (surface != 0) surface->Release();
				return 0;
			}
			return surface;
		}
		return 0;
	}

	void Rebuild_Resource_Caches()
	{
		// A target may have been requested after the failed frame was submitted
		// but before Begin_Frame polls its completion. Keep its owner-side source
		// surfaces alive while replacing optimistic handles; never redirect that
		// already accepted off-screen pass to the visible backbuffer.
		RenderTargetBinding binding = pending_target_change ? pending_target : active_target;
		const bool custom_color = binding.hasColor && !binding.useBackBufferColor;
		const bool custom_depth = binding.hasDepth && !binding.useBackBufferDepth;
		IDirect3DSurface8 *color = custom_color ? Retain_Target_Surface(binding.color) : 0;
		IDirect3DSurface8 *depth = custom_depth ? Retain_Target_Surface(binding.depth) : 0;
		Invalidate_GPU_Copy_Content();
		Release_Caches();
		Fence_Render();
		active_target = RenderTargetBinding();
		bool restored = true;
		RenderResult result = rts::render::RENDER_RESULT_FAILED;
		if (custom_color)
		{
			binding.color.resource = GpuHandle();
			if (color == 0 || !Ensure_Render_Target(color, &binding.color.resource, &result))
			{
				binding.color.resource = GpuHandle();
				restored = false;
			}
		}
		if (custom_depth)
		{
			binding.depth.resource = GpuHandle();
			if (depth == 0 || !Ensure_Depth_Target(depth, &binding.depth.resource, &result))
			{
				binding.depth.resource = GpuHandle();
				restored = false;
			}
		}
		if (color != 0) color->Release();
		if (depth != 0) depth->Release();
		pending_target = binding;
		pending_target_change = true;
		// A failed restore deliberately retains an invalid custom binding. The
		// next Begin_Frame then fails closed until the caller requests a new one.
		target_transition_failed = !restored;
	}

	void Service_Render_Completions()
	{
		if (!Is_Threaded()) return;
		Require_Owner_Thread("render completion publication");
		Poll_Render_Completions();
		if (!device->isOperational() && (deferred_failure_sequence == 0 ||
			(recovered_failure_sequence == deferred_failure_sequence &&
				deferred_failure.recoveryResult() == rts::render::RENDER_RESULT_OK)))
		{
			// A full upload packet may execute before the first Begin_Frame. Its
			// failure has no frame completion, so read the owner's retained error
			// before Is_Active would otherwise suppress every future frame.
			const RenderResult stalled_result = Fence_Render();
			if (stalled_result != rts::render::RENDER_RESULT_OK)
			{
				rts::render::RenderFrameOutcome stalled_outcome;
				stalled_outcome.recordCommandFailure(stalled_result);
				stalled_outcome.setOperational(false);
				if (deferred_failure_sequence == 0 ||
					rts::render::ShouldReplaceLegacyAsyncFrameFailure(deferred_failure, stalled_outcome,
						recovered_failure_sequence == deferred_failure_sequence))
				{
					deferred_failure = stalled_outcome;
					// Reserved identity for a non-frame error. Never enter this value
					// in async_frames or use it for GPU-copy/capture publication.
					deferred_failure_sequence = ~static_cast<uint64_t>(0);
					recovered_failure_sequence = 0;
					async_resource_failure = true;
				}
			}
		}
		// The game owner may already have prepared another frame when removal
		// occurs. Seal/cancel it before the lifecycle fence and never abandon it.
		if (deferred_failure_sequence != 0 &&
			(deferred_failure.hasDeviceRemoval() || !device->isOperational()) &&
			recovered_failure_sequence != deferred_failure_sequence)
		{
			if (frame_open)
			{
				Cancel_Threaded_Frame(rts::render::RENDER_RESULT_DEVICE_REMOVED);
				frame_open = false;
			}
			Fence_Render();
			const RenderResult result = Recover_Device();
			deferred_failure.recordRecovery(result);
			deferred_failure.setOperational(device->isOperational());
			Log_Result("D3D11 asynchronous device recovery", result);
			// Keep the recovered failure until End_Frame reports it. Begin_Frame
			// resets its current outcome and must not erase delayed failures.
			recovered_failure_sequence = deferred_failure_sequence;
		}
		if (async_resource_failure && device->isOperational())
		{
			if (frame_open)
			{
				Cancel_Threaded_Frame(rts::render::RENDER_RESULT_FAILED);
				frame_open = false;
			}
			Fence_Render();
			// Failure can arrive during the fence itself. Keep cache ownership
			// until the next recovery attempt instead of dropping live handles.
			if (!device->isOperational()) return;
			// Admission updated CPU revision caches optimistically. Failed native
			// allocation/upload requires new handles, not endless reuse of a
			// virtual handle whose native creation failed. FIFO draining also
			// prevents the new generation from racing already accepted draws.
			capture_queue.cancelCurrent(rts::render::RENDER_RESULT_FAILED);
			Rebuild_Resource_Caches();
			async_resource_failure = false;
			Log("D3D11 asynchronous upload failure invalidated bridge resource caches");
		}
	}

	RenderResult End_Threaded_Frame(bool visible,
		rts::render::RenderFrameOutcome *outcome)
	{
		Require_Owner_Thread("threaded frame submission");
		const RenderResult end_result = context->endFrame();
		frame_open = false;
		frame_outcome.recordEndFrame(end_result);
		frame_outcome.markFrameEnded();
		rts::render::RenderBackBufferInfo info;
		std::vector<unsigned char> pixels;
		bool capture_ready = false;
		const bool requested_capture = visible && capture_queue.pendingCount() != 0;
		if (requested_capture && frame_outcome.result() == rts::render::RENDER_RESULT_OK)
		{
			RenderResult result = device->getBackBufferInfo(&info);
			size_t row_pitch = 0, bytes = 0;
			if (result == rts::render::RENDER_RESULT_OK)
			{
				if (info.format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
					!Checked_Multiply(info.width, 4, &row_pitch) ||
					!Checked_Multiply(row_pitch, info.height, &bytes) || bytes == 0)
					result = rts::render::RENDER_RESULT_INVALID_ARGUMENT;
				else
				{
					try
					{
						pixels.resize(bytes);
						result = device->captureBackBuffer(&pixels[0], bytes, row_pitch, &info.format);
					}
					catch (...) { result = rts::render::RENDER_RESULT_OUT_OF_MEMORY; }
				}
			}
			frame_outcome.recordCapture(result);
			capture_ready = result == rts::render::RENDER_RESULT_OK;
			if (!capture_ready) capture_queue.cancelCurrent(result);
		}
		const uint64_t previous = rts::render::LastThreadedRenderFrameSequence(device);
		RenderResult submitted;
		if (frame_outcome.hasCommandFailure() || end_result != rts::render::RENDER_RESULT_OK ||
			frame_outcome.hasDeviceRemoval())
		{
			submitted = rts::render::CancelThreadedRenderFrame(device, frame_outcome.result());
			capture_queue.cancelCurrent(frame_outcome.result());
			Complete_GPU_Copy_Frame(false, frame_id);
		}
		else submitted = rts::render::SubmitThreadedRenderFrame(device, visible);
		const bool admitted = Remember_Submission(previous);
		const uint64_t sequence = rts::render::LastThreadedRenderFrameSequence(device);
		if (admitted) frame_outcome.markSubmitted();
		else
		{
			frame_outcome.recordPresentation(submitted == rts::render::RENDER_RESULT_OK ?
				rts::render::RENDER_RESULT_FAILED : submitted);
			Cancel_Threaded_Frame(frame_outcome.result());
			capture_queue.cancelCurrent(frame_outcome.result());
			Complete_GPU_Copy_Frame(false, frame_id);
		}
		// Ordinary frames remain asynchronous. Captures are rare explicit fences:
		// pixels were read before flip, and callbacks require actual presentation.
		if (admitted && requested_capture) rts::render::DrainThreadedRenderDevice(device);
		rts::render::ThreadedRenderFrameCompletion completed;
		Poll_Render_Completions(admitted ? sequence : 0, &completed);
		if (admitted && completed.sequence == sequence)
		{
			const RenderResult capture_result = frame_outcome.captureResult();
			frame_outcome = completed.outcome;
			frame_outcome.recordCapture(capture_result);
		}
		if (requested_capture)
		{
			if (capture_ready && frame_outcome.wasPresented())
			{
				const RenderResult result = capture_queue.completeVisible(info.width, info.height,
					static_cast<size_t>(info.width) * 4, info.format, &pixels[0], pixels.size());
				if (result != rts::render::RENDER_RESULT_OK) capture_queue.cancelCurrent(result);
			}
			else capture_queue.cancelCurrent(frame_outcome.result() == rts::render::RENDER_RESULT_OK ?
				rts::render::RENDER_RESULT_FAILED : frame_outcome.result());
		}
		// Report a prior frame's failure once, rather than relabelling queue
		// admission as success. Do not clear a newer failure with a capture result.
		if (deferred_failure_sequence != 0)
		{
			frame_outcome = deferred_failure;
			if (!deferred_failure.hasDeviceRemoval() ||
				recovered_failure_sequence == deferred_failure_sequence)
			{
				deferred_failure = rts::render::RenderFrameOutcome();
				deferred_failure_sequence = 0;
			}
		}
		frame_outcome.setOperational(device->isOperational());
		if (outcome != 0) *outcome = frame_outcome;
		return frame_outcome.result();
	}
#endif

	struct Draw_Texture_Scope
	{
		explicit Draw_Texture_Scope(Impl &owner) : impl(owner)
		{
			impl.draw_texture_pins.Clear();
			impl.draw_texture_pinning = true;
		}
		~Draw_Texture_Scope()
		{
			impl.draw_texture_pinning = false;
			impl.draw_texture_pins.Clear();
		}
	private:
		Draw_Texture_Scope(const Draw_Texture_Scope &);
		Draw_Texture_Scope &operator=(const Draw_Texture_Scope &);
		Impl &impl;
	};

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

	void Invalidate_GPU_Copy_Content()
	{
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			textures[index].gpu_copy_valid = false;
			textures[index].gpu_copy_frame = 0;
			textures[index].gpu_copy_lease_epoch = 0;
		}
	}

	RenderResult Recover_Device()
	{
#if defined(_WIN64)
		if (Is_Threaded())
		{
			Cancel_Threaded_Frame(rts::render::RENDER_RESULT_DEVICE_REMOVED);
			frame_open = false;
			Fence_Render();
		}
#endif
		// A target transition requested while no frame was open is deliberately
		// deferred until Begin_Frame. Preserve that newer request across device
		// recovery instead of falling back to the previously active target.
		const bool restore_pending_target = pending_target_change;
		const RenderTargetBinding target_to_restore = restore_pending_target ?
			pending_target : active_target;
		capture_queue.advanceGeneration();
		capture_queue.cancelStale(rts::render::RENDER_RESULT_DEVICE_REMOVED);
		const RenderResult recovery_result = device->recoverDevice();
		if (recovery_result != rts::render::RENDER_RESULT_OK)
		{
			target_transition_failed = true;
			if (device->isOperational())
			{
				device->shutdown();
			}
			context = 0;
			return recovery_result;
		}
		// GPU-only render-to-texture contents are not reconstructible by the
		// render device. Their owners must regenerate them after recovery.
		Invalidate_GPU_Copy_Content();
		context = device->immediateContext();
		if (context == 0)
		{
			target_transition_failed = true;
			device->shutdown();
			return rts::render::RENDER_RESULT_FAILED;
		}
		if (native_w3d != 0 && native_w3d->IsAttachedToBorrowedBackend())
		{
			const RenderResult replace_result =
				native_w3d->ReplaceBackendContext(context);
			if (replace_result != rts::render::RENDER_RESULT_OK)
			{
				target_transition_failed = true;
				device->shutdown();
				context = 0;
				return replace_result;
			}
			const RenderResult buffer_result = native_w3d->Resources().
				RestoreStaticBuffersAfterRecovery();
			if (buffer_result != rts::render::RENDER_RESULT_OK)
			{
				target_transition_failed = true;
				device->shutdown();
				context = 0;
				return buffer_result;
			}
		}
		// Recovery recreates the swap-chain target.  Reapply the target that was
		// active before recovery at the next Begin_Frame; logical GPU handles are
		// recreated by the render device in place.
		active_target = RenderTargetBinding();
		pending_target = target_to_restore;
		pending_target_change = true;
		target_transition_failed = false;
		return rts::render::RENDER_RESULT_OK;
	}

	BufferEntry *Find_Buffer(std::vector<BufferEntry> &entries,
		rts::render::LegacyBridgePointerIndex &cache_index,
		IUnknown *source, bool touch = true)
	{
		if (source == 0)
		{
			return 0;
		}
		unsigned int found_index = 0;
		bool hit = cache_index.Find(source, &found_index) &&
			found_index < entries.size() &&
			entries[found_index].source == source;
		if (!hit)
		{
			// The index is maintained at every vector insertion/erase.  Keep a
			// cold repair path so a stale cache cannot create a duplicate COM
			// entry after an exceptional allocation or future recovery change.
			cache_index.Erase(source);
			for (unsigned int index = 0; index < entries.size(); ++index)
			{
				if (entries[index].source == source)
				{
					found_index = index;
					cache_index.Insert(source, index);
					hit = true;
					break;
				}
			}
		}
		cache_counters.RecordBufferLookup(hit);
		if (!hit)
		{
			return 0;
		}
		if (touch)
		{
			entries[found_index].last_used_frame = frame_id;
		}
		return &entries[found_index];
	}

	void Remove_Buffer_Entry(std::vector<BufferEntry> &entries,
		rts::render::LegacyBridgePointerIndex &cache_index,
		unsigned int index)
	{
		if (index >= entries.size())
		{
			return;
		}
		IUnknown *source = entries[index].source;
		device->destroyResource(entries[index].handle);
		cache_index.EraseAt(source, index);
		if (source != 0)
		{
			source->Release();
		}
		entries.erase(entries.begin() + index);
	}

	void Evict_Oldest_Buffer(std::vector<BufferEntry> &entries,
		rts::render::LegacyBridgePointerIndex &cache_index)
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
		Remove_Buffer_Entry(entries, cache_index, oldest);
	}

	bool Is_Target_Handle_Pinned(const GpuHandle &handle,
		const RenderTargetBinding &binding) const
	{
		if (!handle.isValid())
		{
			return false;
		}
		return (binding.hasColor && binding.color.resource == handle) ||
			(binding.hasDepth && binding.depth.resource == handle);
	}

	bool Is_Texture_Entry_Pinned(const TextureEntry &entry) const
	{
		// A target binding owns the corresponding D3D11 resource until the
		// context transitions away from it.  Producer authority is metadata, not
		// a permanent residency lease: an unbound render target can be recreated
		// deterministically from its D3D8 source when the cache needs space.
		const bool active_target_pinned =
			Is_Target_Handle_Pinned(entry.handle, active_target);
		const bool pending_target_pinned = pending_target_change &&
			Is_Target_Handle_Pinned(entry.handle, pending_target);
		// GPU-produced content acquired by its owner must remain resident through
		// the current frame. Otherwise an unrelated cache insertion between the
		// owner's update and draw could recreate the texture from stale D3D8 data.
		const bool copied_content_acquired =
			rts::render::Is_D3D11_GPU_Copy_Lease_Active(
				entry.gpu_copy_valid, entry.gpu_copy_lease_epoch,
				display_epoch);
		return active_target_pinned || pending_target_pinned ||
			copied_content_acquired || draw_texture_pins.Contains(entry.source);
	}

	void Complete_GPU_Copy_Frame(bool frame_succeeded, unsigned int completed_frame)
	{
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			TextureEntry &entry = textures[index];
			if (rts::render::Should_Invalidate_D3D11_GPU_Copy(
					entry.gpu_copy_valid, entry.gpu_copy_frame, completed_frame,
					frame_succeeded))
			{
				entry.gpu_copy_valid = false;
				entry.gpu_copy_lease_epoch = 0;
			}
		}
	}

	void Release_Unbound_Texture_Authority(const RenderTargetBinding &binding)
	{
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			TextureEntry &entry = textures[index];
			if (entry.d3d11_authority &&
				!Is_Target_Handle_Pinned(entry.handle, binding))
			{
				entry.d3d11_authority = false;
			}
		}
	}

	bool Remove_Texture_Entry(unsigned int index)
	{
		if (index >= textures.size() ||
			Is_Texture_Entry_Pinned(textures[index]))
		{
			return false;
		}
		TextureEntry &entry = textures[index];
		if (entry.handle.isValid() && !device->destroyResource(entry.handle))
		{
			return false;
		}
		IDirect3DBaseTexture8 *source = entry.source;
		texture_index.EraseAt(source, index);
		if (source != 0)
		{
			source->Release();
		}
		textures.erase(textures.begin() + index);
		return true;
	}

	bool Evict_Oldest_Texture()
	{
		if (textures.empty())
		{
			return false;
		}
		unsigned int oldest = static_cast<unsigned int>(textures.size());
		for (unsigned int index = 0; index < textures.size(); ++index)
		{
			if (!Is_Texture_Entry_Pinned(textures[index]) &&
				(oldest == textures.size() ||
					textures[index].last_used_frame <
						textures[oldest].last_used_frame))
			{
				oldest = index;
			}
		}
		return oldest != textures.size() && Remove_Texture_Entry(oldest);
	}

	void Prune_Stale_Caches()
	{
		rts::frame_timing::Scope timing(rts::frame_timing::RendererTexturePrune);
		for (unsigned int index = 0; index < vertex_buffers.size();)
		{
			if (vertex_buffers[index].last_used_frame + CACHE_STALE_FRAME_COUNT <
				frame_id)
			{
				Remove_Buffer_Entry(vertex_buffers, vertex_buffer_index, index);
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
				Remove_Buffer_Entry(index_buffers, index_buffer_index, index);
			}
			else
			{
				++index;
			}
		}
	for (unsigned int index = 0; index < textures.size();)
	{
		if (Is_Texture_Entry_Pinned(textures[index]))
		{
			++index;
			continue;
		}
		// A dirty source is deliberately refreshed at Bind_Texture(), not by
		// periodic eviction.  Cache maintenance otherwise performs a complete
		// D3D8 LockRect/readback for textures that may never be sampled again,
		// creating regular menu hitches and flicker under animated UI churn.
		// Stale entries can be dropped safely: the next real use creates a
		// current D3D11 copy from the D3D8 source.
		if (textures[index].last_used_frame + CACHE_STALE_FRAME_COUNT < frame_id)
			{
				if (!Remove_Texture_Entry(index))
				{
					++index;
				}
			}
			else
			{
				++index;
			}
		}
	}

	bool Acquire_Vertex_Buffer(VertexBufferClass *vertex_buffer,
		unsigned int stride, unsigned int offset, unsigned int start_vertex,
		unsigned int vertex_count, GpuHandle *handle)
	{
#if defined(_WIN64)
		if (handle != 0)
		{
			*handle = GpuHandle();
		}
		if (vertex_buffer == 0 || handle == 0 ||
			(vertex_buffer->Type() != BUFFER_TYPE_DX8 &&
			 vertex_buffer->Type() != BUFFER_TYPE_DYNAMIC_DX8))
		{
			return Fail("draw failure: unsupported native vertex buffer");
		}
		if (!static_cast<DX8VertexBufferClass *>(vertex_buffer)->
			Acquire_Native_Vertex_Buffer(stride, offset, start_vertex,
				vertex_count, handle))
		{
			return Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"draw failure: native vertex range is unavailable");
		}
		return true;
#else
		(void)stride;
		(void)offset;
		(void)start_vertex;
		(void)vertex_count;
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
		const unsigned int source_generation = vertex_buffer->Get_Generation();
		BufferEntry *entry = Find_Buffer(vertex_buffers, vertex_buffer_index,
			source);
		const bool created_entry = entry == 0;
		if (entry == 0)
		{
			if (vertex_buffers.size() >= BUFFER_CACHE_CAPACITY)
			{
				Evict_Oldest_Buffer(vertex_buffers, vertex_buffer_index);
			}
			BufferEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.byte_count = byte_count;
			new_entry.source_generation = 0;
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
			if (!vertex_buffer_index.Insert(static_cast<IUnknown *>(source),
				static_cast<unsigned int>(vertex_buffers.size() - 1)))
			{
				device->destroyResource(vertex_buffers.back().handle);
				vertex_buffers.back().source->Release();
				vertex_buffers.pop_back();
				return Fail("draw failure: vertex buffer cache index allocation");
			}
			entry = &vertex_buffers.back();
		}
		else if (entry->byte_count != byte_count)
		{
			return Fail("draw failure: vertex buffer size changed");
		}
		if (!entry->source_dirty &&
			!rts::render::LegacyBridgeTypedBufferNeedsUpload(
			entry->source_generation, source_generation))
		{
			*handle = entry->handle;
			return true;
		}
		size_t upload_offset = 0;
		size_t upload_bytes = byte_count;
		RenderBufferUpdateMode update_mode =
			rts::render::RENDER_BUFFER_UPDATE_PRESERVE;
		if (!created_entry && !entry->source_dirty &&
			vertex_buffer->Type() == BUFFER_TYPE_DYNAMIC_DX8)
		{
			unsigned int change_offset = 0;
			unsigned int change_count = 0;
			unsigned int change_flags = 0;
			if (vertex_buffer->Get_Change_Since(entry->source_generation,
				&change_offset, &change_count, &change_flags) &&
				change_offset <= vertex_buffer->Get_Vertex_Count() &&
				change_count <= vertex_buffer->Get_Vertex_Count() - change_offset)
			{
				upload_offset = static_cast<size_t>(change_offset) *
					vertex_buffer->FVF_Info().Get_FVF_Size();
				upload_bytes = static_cast<size_t>(change_count) *
					vertex_buffer->FVF_Info().Get_FVF_Size();
				if ((change_flags & D3DLOCK_DISCARD) != 0 && upload_offset == 0)
				{
					update_mode = rts::render::RENDER_BUFFER_UPDATE_DISCARD;
				}
				else if ((change_flags & D3DLOCK_NOOVERWRITE) != 0)
				{
					update_mode = rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE;
				}
			}
		}
		unsigned char *data = 0;
		HRESULT result = source->Lock(static_cast<UINT>(upload_offset),
			static_cast<UINT>(upload_bytes),
			&data, D3DLOCK_READONLY);
		if (FAILED(result))
		{
			result = source->Lock(static_cast<UINT>(upload_offset),
				static_cast<UINT>(upload_bytes), &data, 0);
		}
		if (FAILED(result) || data == 0)
		{
			return Fail("draw failure: legacy vertex buffer lock");
		}
		const RenderResult upload_result = context->updateBuffer(entry->handle,
			data, upload_bytes, upload_offset, update_mode);
		source->Unlock();
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(upload_result,
				"draw failure: D3D11 vertex buffer upload");
		}
		cache_counters.RecordBufferUpload();
		entry->source_generation = source_generation;
		entry->source_dirty = false;
		*handle = entry->handle;
		return true;
#endif
	}

	bool Acquire_Index_Buffer(IndexBufferClass *index_buffer,
		unsigned int offset, unsigned int start_index, unsigned int index_count,
		GpuHandle *handle)
	{
#if defined(_WIN64)
		if (handle != 0)
		{
			*handle = GpuHandle();
		}
		if (index_buffer == 0 || handle == 0 ||
			(index_buffer->Type() != BUFFER_TYPE_DX8 &&
			 index_buffer->Type() != BUFFER_TYPE_DYNAMIC_DX8))
		{
			return Fail("draw failure: unsupported native index buffer");
		}
		if (!static_cast<DX8IndexBufferClass *>(index_buffer)->
			Acquire_Native_Index_Buffer(offset, start_index, index_count, handle))
		{
			return Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"draw failure: native index range is unavailable");
		}
		return true;
#else
		(void)offset;
		(void)start_index;
		(void)index_count;
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
		const unsigned int source_generation = index_buffer->Get_Generation();
		BufferEntry *entry = Find_Buffer(index_buffers, index_buffer_index,
			source);
		const bool created_entry = entry == 0;
		if (entry == 0)
		{
			if (index_buffers.size() >= BUFFER_CACHE_CAPACITY)
			{
				Evict_Oldest_Buffer(index_buffers, index_buffer_index);
			}
			BufferEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.byte_count = byte_count;
			new_entry.source_generation = 0;
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
			if (!index_buffer_index.Insert(static_cast<IUnknown *>(source),
				static_cast<unsigned int>(index_buffers.size() - 1)))
			{
				device->destroyResource(index_buffers.back().handle);
				index_buffers.back().source->Release();
				index_buffers.pop_back();
				return Fail("draw failure: index buffer cache index allocation");
			}
			entry = &index_buffers.back();
		}
		else if (entry->byte_count != byte_count)
		{
			return Fail("draw failure: index buffer size changed");
		}
		if (!entry->source_dirty &&
			!rts::render::LegacyBridgeTypedBufferNeedsUpload(
			entry->source_generation, source_generation))
		{
			*handle = entry->handle;
			return true;
		}
		size_t upload_offset = 0;
		size_t upload_bytes = byte_count;
		RenderBufferUpdateMode update_mode =
			rts::render::RENDER_BUFFER_UPDATE_PRESERVE;
		if (!created_entry && !entry->source_dirty &&
			index_buffer->Type() == BUFFER_TYPE_DYNAMIC_DX8)
		{
			unsigned int change_offset = 0;
			unsigned int change_count = 0;
			unsigned int change_flags = 0;
			if (index_buffer->Get_Change_Since(entry->source_generation,
				&change_offset, &change_count, &change_flags) &&
				change_offset <= index_buffer->Get_Index_Count() &&
				change_count <= index_buffer->Get_Index_Count() - change_offset)
			{
				upload_offset = static_cast<size_t>(change_offset) *
					sizeof(unsigned short);
				upload_bytes = static_cast<size_t>(change_count) *
					sizeof(unsigned short);
				if ((change_flags & D3DLOCK_DISCARD) != 0 && upload_offset == 0)
				{
					update_mode = rts::render::RENDER_BUFFER_UPDATE_DISCARD;
				}
				else if ((change_flags & D3DLOCK_NOOVERWRITE) != 0)
				{
					update_mode = rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE;
				}
			}
		}
		unsigned char *data = 0;
		HRESULT result = source->Lock(static_cast<UINT>(upload_offset),
			static_cast<UINT>(upload_bytes),
			&data, D3DLOCK_READONLY);
		if (FAILED(result))
		{
			result = source->Lock(static_cast<UINT>(upload_offset),
				static_cast<UINT>(upload_bytes), &data, 0);
		}
		if (FAILED(result) || data == 0)
		{
			return Fail("draw failure: legacy index buffer lock");
		}
		const RenderResult upload_result = context->updateBuffer(entry->handle,
			data, upload_bytes, upload_offset, update_mode);
		source->Unlock();
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(upload_result,
				"draw failure: D3D11 index buffer upload");
		}
		cache_counters.RecordBufferUpload();
		entry->source_generation = source_generation;
		entry->source_dirty = false;
		*handle = entry->handle;
		return true;
#endif
	}

	bool Upload_Raw_Vertex_Buffer(IDirect3DVertexBuffer8 *source,
		GpuHandle *handle, size_t *byte_count)
	{
		if (source == 0 || handle == 0 || byte_count == 0)
		{
			return Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"raw draw failure: invalid vertex buffer");
		}
		D3DVERTEXBUFFER_DESC descriptor8;
		memset(&descriptor8, 0, sizeof(descriptor8));
		if (FAILED(source->GetDesc(&descriptor8)) || descriptor8.Size == 0)
		{
			return Fail("raw draw failure: vertex buffer description");
		}
		*byte_count = static_cast<size_t>(descriptor8.Size);
		BufferEntry *entry = Find_Buffer(vertex_buffers, vertex_buffer_index,
			source);
		if (entry == 0)
		{
			if (vertex_buffers.size() >= BUFFER_CACHE_CAPACITY)
			{
				Evict_Oldest_Buffer(vertex_buffers, vertex_buffer_index);
			}
			BufferEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.byte_count = *byte_count;
			new_entry.last_used_frame = frame_id;
			BufferDescriptor buffer_descriptor;
			buffer_descriptor.byteCount = *byte_count;
			buffer_descriptor.stride = 1;
			buffer_descriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
			buffer_descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
			const RenderResult create_result = device->createBuffer(
				buffer_descriptor, 0, 0, &new_entry.handle);
			if (create_result != rts::render::RENDER_RESULT_OK)
			{
				new_entry.source->Release();
				return Fail(create_result,
					"raw draw failure: D3D11 vertex buffer creation");
			}
			try
			{
				vertex_buffers.push_back(new_entry);
			}
			catch (...)
			{
				device->destroyResource(new_entry.handle);
				new_entry.source->Release();
				return Fail("raw draw failure: vertex buffer cache allocation");
			}
			if (!vertex_buffer_index.Insert(static_cast<IUnknown *>(source),
				static_cast<unsigned int>(vertex_buffers.size() - 1)))
			{
				device->destroyResource(vertex_buffers.back().handle);
				vertex_buffers.back().source->Release();
				vertex_buffers.pop_back();
				return Fail("raw draw failure: vertex buffer cache index allocation");
			}
			entry = &vertex_buffers.back();
		}
		else if (entry->byte_count != *byte_count)
		{
			return Fail("raw draw failure: vertex buffer size changed");
		}
		if (!rts::render::LegacyBridgeRawBufferNeedsUpload(
			entry->source_dirty))
		{
			*handle = entry->handle;
			return true;
		}

		unsigned char *data = 0;
		size_t upload_offset = 0;
		size_t upload_bytes = descriptor8.Size;
		RenderBufferUpdateMode update_mode =
			rts::render::RENDER_BUFFER_UPDATE_PRESERVE;
		if (entry->raw_range_valid &&
			entry->raw_range_offset <= descriptor8.Size &&
			entry->raw_range_bytes <=
				descriptor8.Size - entry->raw_range_offset)
		{
			upload_offset = entry->raw_range_offset;
			upload_bytes = entry->raw_range_bytes;
			update_mode = entry->raw_range_mode;
		}
		HRESULT result = source->Lock(static_cast<UINT>(upload_offset),
			static_cast<UINT>(upload_bytes), &data,
			D3DLOCK_READONLY);
		if (FAILED(result))
		{
			result = source->Lock(static_cast<UINT>(upload_offset),
				static_cast<UINT>(upload_bytes), &data, 0);
		}
		if (FAILED(result) || data == 0)
		{
			return Fail("raw draw failure: vertex buffer lock");
		}
		const RenderResult upload_result = context->updateBuffer(entry->handle,
			data, upload_bytes, upload_offset, update_mode);
		source->Unlock();
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(upload_result,
				"raw draw failure: D3D11 vertex buffer upload");
		}
		cache_counters.RecordBufferUpload();
		entry->source_dirty = false;
		entry->raw_range_valid = false;
		*handle = entry->handle;
		return true;
	}

	bool Upload_Raw_Index_Buffer(IDirect3DIndexBuffer8 *source,
		GpuHandle *handle, size_t *byte_count)
	{
		if (source == 0 || handle == 0 || byte_count == 0)
		{
			return Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"raw draw failure: invalid index buffer");
		}
		D3DINDEXBUFFER_DESC descriptor8;
		memset(&descriptor8, 0, sizeof(descriptor8));
		if (FAILED(source->GetDesc(&descriptor8)) || descriptor8.Size == 0)
		{
			return Fail("raw draw failure: index buffer description");
		}
		if (descriptor8.Format != D3DFMT_INDEX16)
		{
			return Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
				"raw draw failure: index buffer is not 16-bit");
		}
		*byte_count = static_cast<size_t>(descriptor8.Size);
		BufferEntry *entry = Find_Buffer(index_buffers, index_buffer_index,
			source);
		if (entry == 0)
		{
			if (index_buffers.size() >= BUFFER_CACHE_CAPACITY)
			{
				Evict_Oldest_Buffer(index_buffers, index_buffer_index);
			}
			BufferEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.byte_count = *byte_count;
			new_entry.last_used_frame = frame_id;
			BufferDescriptor buffer_descriptor;
			buffer_descriptor.byteCount = *byte_count;
			buffer_descriptor.stride = sizeof(unsigned short);
			buffer_descriptor.binding = rts::render::RENDER_BUFFER_INDEX;
			buffer_descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
			const RenderResult create_result = device->createBuffer(
				buffer_descriptor, 0, 0, &new_entry.handle);
			if (create_result != rts::render::RENDER_RESULT_OK)
			{
				new_entry.source->Release();
				return Fail(create_result,
					"raw draw failure: D3D11 index buffer creation");
			}
			try
			{
				index_buffers.push_back(new_entry);
			}
			catch (...)
			{
				device->destroyResource(new_entry.handle);
				new_entry.source->Release();
				return Fail("raw draw failure: index buffer cache allocation");
			}
			if (!index_buffer_index.Insert(static_cast<IUnknown *>(source),
				static_cast<unsigned int>(index_buffers.size() - 1)))
			{
				device->destroyResource(index_buffers.back().handle);
				index_buffers.back().source->Release();
				index_buffers.pop_back();
				return Fail("raw draw failure: index buffer cache index allocation");
			}
			entry = &index_buffers.back();
		}
		else if (entry->byte_count != *byte_count)
		{
			return Fail("raw draw failure: index buffer size changed");
		}
		if (!rts::render::LegacyBridgeRawBufferNeedsUpload(
			entry->source_dirty))
		{
			*handle = entry->handle;
			return true;
		}

		unsigned char *data = 0;
		size_t upload_offset = 0;
		size_t upload_bytes = descriptor8.Size;
		RenderBufferUpdateMode update_mode =
			rts::render::RENDER_BUFFER_UPDATE_PRESERVE;
		if (entry->raw_range_valid &&
			entry->raw_range_offset <= descriptor8.Size &&
			entry->raw_range_bytes <=
				descriptor8.Size - entry->raw_range_offset)
		{
			upload_offset = entry->raw_range_offset;
			upload_bytes = entry->raw_range_bytes;
			update_mode = entry->raw_range_mode;
		}
		HRESULT result = source->Lock(static_cast<UINT>(upload_offset),
			static_cast<UINT>(upload_bytes), &data,
			D3DLOCK_READONLY);
		if (FAILED(result))
		{
			result = source->Lock(static_cast<UINT>(upload_offset),
				static_cast<UINT>(upload_bytes), &data, 0);
		}
		if (FAILED(result) || data == 0)
		{
			return Fail("raw draw failure: index buffer lock");
		}
		const RenderResult upload_result = context->updateBuffer(entry->handle,
			data, upload_bytes, upload_offset, update_mode);
		source->Unlock();
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(upload_result,
				"raw draw failure: D3D11 index buffer upload");
		}
		cache_counters.RecordBufferUpload();
		entry->source_dirty = false;
		entry->raw_range_valid = false;
		*handle = entry->handle;
		return true;
	}

	bool Submit_Indexed_Draw(GpuHandle vertex_handle,
		GpuHandle index_handle, const LegacyVertexLayout &layout,
		unsigned int fvf, unsigned int primitive_type,
		unsigned int index_count, unsigned int start_index,
		unsigned int primitive_count, unsigned int base_vertex,
		bool raw_draw)
	{
		if (!Can_Submit_D3D11_Legacy_Draw())
		{
			return Fail(rts::render::RENDER_RESULT_FAILED,
				"draw failure: neutral texture stage state publication was rejected");
		}
		LegacyLogicalState state;
		if (!rts::render::GetTrackedLegacyLogicalState(&state))
		{
			return Fail("draw failure: unavailable legacy logical state");
		}
		if (state.pipeline.rasterizer.frontCounterClockwise)
		{
			++counter_clockwise_draw_count;
		}
		if (state.pipeline.rasterizer.cullMode == rts::render::RENDER_CULL_NONE)
		{
			++unculled_draw_count;
		}
		if (state.pipeline.blend.colorWriteMask != 0)
		{
			++color_write_draw_count;
		}
		if (state.pipeline.depthStencil.depthFunction ==
			rts::render::RENDER_COMPARE_NEVER)
		{
			++depth_never_draw_count;
		}
		if (!capture_draw_logged && capture_queue.pendingCount() != 0)
		{
			D3DMATRIX actual_world;
			D3DMATRIX actual_view;
			D3DMATRIX actual_projection;
			float maximum_difference[3] = { 0.0f, 0.0f, 0.0f };
			if (legacy_device != 0 &&
				SUCCEEDED(legacy_device->GetTransform(D3DTS_WORLD,
					&actual_world)) && SUCCEEDED(legacy_device->GetTransform(
					D3DTS_VIEW, &actual_view)) && SUCCEEDED(
					legacy_device->GetTransform(D3DTS_PROJECTION,
						&actual_projection)))
			{
				const float *actual_matrices[] = { &actual_world.m[0][0],
					&actual_view.m[0][0], &actual_projection.m[0][0] };
				const float *tracked_matrices[] = { state.constants.world.values,
					state.constants.view.values, state.constants.projection.values };
				for (unsigned int matrix = 0; matrix < 3; ++matrix)
				{
					for (unsigned int component = 0; component < 16; ++component)
					{
						const float difference = static_cast<float>(fabs(
							actual_matrices[matrix][component] -
							tracked_matrices[matrix][component]));
						if (difference > maximum_difference[matrix])
						{
							maximum_difference[matrix] = difference;
						}
					}
				}
			}
			D3DVIEWPORT8 actual_viewport;
			memset(&actual_viewport, 0, sizeof(actual_viewport));
			if (legacy_device != 0)
			{
				legacy_device->GetViewport(&actual_viewport);
			}
			char state_message[320];
			snprintf(state_message, sizeof(state_message),
				"capture first draw fvf=0x%08x primitives=%u start=%u base=%u cull=%u ccw=%u color_mask=0x%x depth=%u matrix_diff=%g,%g,%g viewport=%u,%u,%u,%u,%g,%g",
				fvf, primitive_count, start_index, base_vertex,
				static_cast<unsigned int>(state.pipeline.rasterizer.cullMode),
				state.pipeline.rasterizer.frontCounterClockwise ? 1U : 0U,
				state.pipeline.blend.colorWriteMask,
				static_cast<unsigned int>(state.pipeline.depthStencil.depthFunction),
				maximum_difference[0], maximum_difference[1], maximum_difference[2],
				actual_viewport.X, actual_viewport.Y,
				actual_viewport.Width, actual_viewport.Height, actual_viewport.MinZ,
				actual_viewport.MaxZ);
			Log(state_message);
			capture_draw_logged = true;
		}
		Draw_Texture_Scope texture_scope(*this);
		unsigned int texture_mask = 0;
		for (unsigned int stage = 0;
			stage < rts::render::LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		{
			GpuHandle texture_handle;
#if defined(_WIN64)
			TextureBaseClass *source =
				rts::render::GetPublishedTextureStage(stage);
			if (!Bind_Native_Texture(stage, source, &texture_handle))
#else
			IDirect3DBaseTexture8 *source =
				DX8Wrapper::Get_Tracked_DX8_Texture(stage);
			if (!Bind_Texture(stage, source, &texture_handle))
#endif
			{
				return Fail("draw failure: texture conversion or binding");
			}
			if (texture_handle.isValid())
			{
				texture_mask |= 1U << stage;
			}
		}
		const RenderResult state_result = context->setLegacyStateForLayout(
			state, layout, texture_mask);
		if (state_result != rts::render::RENDER_RESULT_OK)
		{
			char message[384];
			unsigned int used = static_cast<unsigned int>(snprintf(message,
				sizeof(message),
				"draw failure: D3D11 state/layout result=%u fvf=0x%08x stride=%u elements=%u textures=0x%02x",
				static_cast<unsigned int>(state_result), fvf, layout.stride,
				layout.elementCount, texture_mask));
			if (used >= sizeof(message))
			{
				used = sizeof(message) - 1;
				message[used] = '\0';
			}
			Append_Layout_Diagnostic(message, sizeof(message), &used, layout);
			return Fail(state_result, message);
		}
		const RenderResult vertex_bind_result = context->setVertexBuffer(
			vertex_handle, layout.stride, 0);
		if (vertex_bind_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(vertex_bind_result,
				"draw failure: D3D11 vertex buffer binding");
		}
		const RenderResult index_bind_result = context->setIndexBuffer(
			index_handle, rts::render::RENDER_FORMAT_R16_UINT, 0);
		if (index_bind_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(index_bind_result,
				"draw failure: D3D11 index buffer binding");
		}
		rts::render::RenderPrimitiveTopology topology;
		if (!Try_Translate_D3D8_Primitive_Topology(primitive_type, &topology))
		{
			return Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
				"draw failure: unsupported indexed primitive topology");
		}
		const RenderResult topology_result = context->setPrimitiveTopology(
			topology);
		if (topology_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(topology_result,
				"draw failure: D3D11 topology binding");
		}
		const RenderResult draw_result = context->drawIndexed(index_count,
			start_index, static_cast<int>(base_vertex));
		if (draw_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(draw_result,
				"draw failure: D3D11 indexed submission");
		}
		++draw_count;
		++frame_draw_count;
		if (raw_draw)
		{
			++raw_indexed_draw_count;
		}
		if (draw_count == 1)
		{
			Log(raw_draw ? "first D3D11 raw indexed draw submitted" :
				"first D3D11 legacy draw submitted");
		}
		return true;
	}

	bool Ensure_Primitive_Up_Buffer(size_t required_bytes,
		GpuHandle *handle)
	{
		if (handle == 0 || required_bytes == 0 ||
			required_bytes > PRIMITIVE_UP_BUFFER_CAPACITY)
		{
			return Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
				"draw failure: DrawPrimitiveUP byte budget exceeded");
		}
		if (primitive_up_vertex_buffer.isValid() &&
			primitive_up_vertex_capacity >= required_bytes)
		{
			*handle = primitive_up_vertex_buffer;
			return true;
		}
		if (primitive_up_vertex_buffer.isValid())
		{
			if (!device->destroyResource(primitive_up_vertex_buffer))
			{
				return Fail("draw failure: DrawPrimitiveUP buffer replacement");
			}
			primitive_up_vertex_buffer = GpuHandle();
			primitive_up_vertex_capacity = 0;
		}
		size_t allocation_bytes = 256;
		while (allocation_bytes < required_bytes &&
			allocation_bytes < PRIMITIVE_UP_BUFFER_CAPACITY)
		{
			allocation_bytes *= 2;
		}
		if (allocation_bytes > PRIMITIVE_UP_BUFFER_CAPACITY)
		{
			allocation_bytes = PRIMITIVE_UP_BUFFER_CAPACITY;
		}
		BufferDescriptor descriptor;
		descriptor.byteCount = allocation_bytes;
		descriptor.stride = 1;
		descriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
		descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
		const RenderResult create_result = device->createBuffer(descriptor,
			0, 0, &primitive_up_vertex_buffer);
		if (create_result != rts::render::RENDER_RESULT_OK)
		{
			primitive_up_vertex_buffer = GpuHandle();
			return Fail(create_result,
				"draw failure: DrawPrimitiveUP buffer creation");
		}
		primitive_up_vertex_capacity = allocation_bytes;
		*handle = primitive_up_vertex_buffer;
		return true;
	}

	bool Upload_Primitive_Up(const void *vertex_data, size_t byte_count,
		GpuHandle *handle)
	{
		if (vertex_data == 0 || handle == 0 || byte_count == 0)
		{
			return Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"draw failure: invalid DrawPrimitiveUP data");
		}
		if (!Ensure_Primitive_Up_Buffer(byte_count, handle))
		{
			return false;
		}
		const RenderResult upload_result = context->updateBuffer(*handle,
			vertex_data, byte_count, 0,
			rts::render::RENDER_BUFFER_UPDATE_DISCARD);
		if (upload_result != rts::render::RENDER_RESULT_OK)
		{
			return Fail(upload_result,
				"draw failure: DrawPrimitiveUP buffer upload");
		}
		cache_counters.RecordBufferUpload();
		return true;
	}

	bool Copy_Surface(IDirect3DSurface8 *source, unsigned int width,
		unsigned int height, std::vector<unsigned char> *pixels)
	{
		return SUCCEEDED(SurfaceBlit_Copy_Surface_To_A8R8G8B8(
			source, width, height, pixels));
	}

	bool Copy_V8U8_Surface(IDirect3DSurface8 *source, unsigned int width,
		unsigned int height, std::vector<unsigned char> *pixels)
	{
		if (source == 0 || pixels == 0 || width == 0 || height == 0)
		{
			return false;
		}
		size_t rowBytes = 0;
		size_t totalBytes = 0;
		if (!Checked_Multiply(static_cast<size_t>(width), 2, &rowBytes) ||
			!Checked_Multiply(rowBytes, static_cast<size_t>(height),
				&totalBytes) || totalBytes > MAX_TEXTURE_REFRESH_BYTES)
		{
			return false;
		}
		D3DLOCKED_RECT locked;
		if (FAILED(source->LockRect(&locked, 0, D3DLOCK_READONLY)))
		{
			return false;
		}
		if (locked.pBits == 0 || locked.Pitch < static_cast<int>(rowBytes))
		{
			source->UnlockRect();
			return false;
		}
		try
		{
			pixels->resize(totalBytes);
			for (unsigned int row = 0; row < height; ++row)
			{
				memcpy(&(*pixels)[static_cast<size_t>(row) * rowBytes],
					static_cast<const unsigned char *>(locked.pBits) +
						static_cast<size_t>(row) * locked.Pitch,
					rowBytes);
			}
		}
		catch (...)
		{
			source->UnlockRect();
			return false;
		}
		return SUCCEEDED(source->UnlockRect());
	}

	bool Collect_Texture_Data(IDirect3DBaseTexture8 *source,
		TextureDescriptor *descriptor_out,
		std::vector<std::vector<unsigned char> > *pixels_out,
		std::vector<TextureSubresourceData> *subresources_out,
		bool for_render_target, RenderResult *failure_result)
	{
		rts::frame_timing::Scope timing(rts::frame_timing::RendererTextureCollect);
		if (failure_result != 0)
		{
			*failure_result = rts::render::RENDER_RESULT_FAILED;
		}
		if (source == 0 || descriptor_out == 0 || pixels_out == 0 ||
			subresources_out == 0)
		{
			return false;
		}
		TextureDescriptor &descriptor = *descriptor_out;
		std::vector<std::vector<unsigned char> > &pixels = *pixels_out;
		std::vector<TextureSubresourceData> &subresources = *subresources_out;
		descriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		descriptor.binding = rts::render::RENDER_TEXTURE_SHADER_RESOURCE |
			(for_render_target ? rts::render::RENDER_TEXTURE_RENDER_TARGET : 0);
		// D3D8 resources can be mutated through LockRect/UpdateTexture even when
		// they were not created with D3DUSAGE_DYNAMIC.  The bridge exposes that
		// mutation through Invalidate_Texture, so its D3D11 mirror must remain
		// refreshable.  Immutable mirrors forced every animated shell texture to
		// be destroyed and recreated on each dirty notification.
		descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		if (source->GetType() == D3DRTYPE_TEXTURE)
		{
			IDirect3DTexture8 *texture = static_cast<IDirect3DTexture8 *>(source);
			descriptor.mipCount = texture->GetLevelCount();
			D3DSURFACE_DESC level_zero;
			if (FAILED(texture->GetLevelDesc(0, &level_zero)))
			{
				return false;
			}
			const bool signed_bump = level_zero.Format == D3DFMT_V8U8;
			if (signed_bump)
			{
				if (for_render_target)
				{
					if (failure_result != 0)
						*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
					return false;
				}
				descriptor.format = rts::render::RENDER_FORMAT_R8G8_SNORM;
			}
			descriptor.width = level_zero.Width;
			descriptor.height = level_zero.Height;
			// Track the capabilities of the D3D11 allocation, not merely the
			// usage flags carried by its D3D8 source. A texture first uploaded for
			// sampling has no RTV and must be recreated when it later becomes a
			// render target.
			if (descriptor.mipCount > MAX_TEXTURE_REFRESH_SUBRESOURCES)
			{
				if (failure_result != 0)
					*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
				return false;
			}
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
				const bool copied = signed_bump ?
					Copy_V8U8_Surface(surface, level.Width, level.Height,
						&pixels[mip]) :
					Copy_Surface(surface, level.Width, level.Height, &pixels[mip]);
				surface->Release();
				if (!copied)
				{
					return false;
				}
				subresources[mip].data = &pixels[mip][0];
				subresources[mip].rowPitch = static_cast<size_t>(level.Width) *
					(signed_bump ? 2 : 4);
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
			if (descriptor.mipCount > MAX_TEXTURE_REFRESH_SUBRESOURCES / 6)
			{
				if (failure_result != 0)
					*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
				return false;
			}
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
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
			return false;
		}
		if (failure_result != 0)
		{
			*failure_result = rts::render::RENDER_RESULT_OK;
		}
		return true;
	}

	bool Create_Texture(IDirect3DBaseTexture8 *source, GpuHandle *handle,
		bool *render_target, RenderResult *failure_result,
		bool for_render_target = false)
	{
		if (failure_result != 0)
		{
			*failure_result = rts::render::RENDER_RESULT_FAILED;
		}
		if (source == 0 || handle == 0 || render_target == 0)
		{
			return false;
		}
		// Preserve D3D8 render-target capability when a texture is first seen as
		// a shader source.  Smudge/background textures are created by the legacy
		// device with D3DUSAGE_RENDERTARGET, and the D3D11 copy path needs an RTV
		// capable destination without forcing a readback through D3D8.
		if (!for_render_target && source->GetType() == D3DRTYPE_TEXTURE)
		{
			D3DSURFACE_DESC level_zero;
			IDirect3DTexture8 *texture = static_cast<IDirect3DTexture8 *>(source);
			if (SUCCEEDED(texture->GetLevelDesc(0, &level_zero)) &&
				(level_zero.Usage & D3DUSAGE_RENDERTARGET) != 0)
			{
				for_render_target = true;
			}
		}
		TextureDescriptor descriptor;
		std::vector<std::vector<unsigned char> > pixels;
		std::vector<TextureSubresourceData> subresources;
		RenderResult collect_result = rts::render::RENDER_RESULT_FAILED;
		if (!Collect_Texture_Data(source, &descriptor, &pixels, &subresources,
			for_render_target, &collect_result))
		{
			if (failure_result != 0)
				*failure_result = collect_result;
			return false;
		}
		*render_target = (descriptor.binding &
			rts::render::RENDER_TEXTURE_RENDER_TARGET) != 0;
		const RenderResult create_result = device->createTexture(descriptor,
			&subresources[0], static_cast<unsigned int>(subresources.size()),
			handle);
		if (failure_result != 0)
		{
			*failure_result = create_result;
		}
		return create_result == rts::render::RENDER_RESULT_OK;
	}

	RenderResult Refresh_Texture(TextureEntry &entry)
	{
		if (entry.source == 0 || entry.depth_stencil ||
			!entry.handle.isValid())
		{
			return rts::render::RENDER_RESULT_UNSUPPORTED;
		}
		TextureDescriptor descriptor;
		std::vector<std::vector<unsigned char> > pixels;
		std::vector<TextureSubresourceData> subresources;
		RenderResult collect_result = rts::render::RENDER_RESULT_FAILED;
		if (!Collect_Texture_Data(entry.source, &descriptor, &pixels,
			&subresources, entry.render_target, &collect_result))
		{
			return collect_result;
		}
		return device->refreshTexture(entry.handle, descriptor,
			&subresources[0], static_cast<unsigned int>(subresources.size()));
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
		unsigned int entry_index = 0;
		TextureEntry *entry = Find_Texture_Entry(source, &entry_index);
		if (entry != 0)
		{
			if (entry->d3d8_dirty)
			{
				const bool target_pinned =
					Is_Target_Handle_Pinned(entry->handle, active_target) ||
					(pending_target_change &&
						Is_Target_Handle_Pinned(entry->handle, pending_target));
				if (target_pinned)
				{
					return Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
						"draw failure: dirty texture is still a render target");
				}
				const RenderResult refresh_result = Refresh_Texture(*entry);
				if (refresh_result == rts::render::RENDER_RESULT_OK)
				{
					entry->d3d8_dirty = false;
					++dynamic_texture_refresh_count;
					++dynamic_texture_in_place_count;
					cache_counters.RecordTextureRefresh();
				}
				else if (refresh_result ==
					rts::render::RENDER_RESULT_UNSUPPORTED)
				{
					if (!Remove_Texture_Entry(entry_index))
					{
						return Fail(rts::render::RENDER_RESULT_FAILED,
							"draw failure: dirty texture refresh");
					}
					++dynamic_texture_refresh_count;
					++dynamic_texture_recreate_count;
					cache_counters.RecordTextureRefresh();
					entry = 0;
				}
				else
				{
					return Fail(refresh_result,
						"draw failure: dirty texture refresh");
				}
			}
		}
		if (entry == 0)
		{
			if (textures.size() >= TEXTURE_CACHE_CAPACITY &&
				!Evict_Oldest_Texture())
			{
				return Fail(rts::render::RENDER_RESULT_OUT_OF_MEMORY,
					"draw failure: texture cache is pinned");
			}
			TextureEntry new_entry;
			new_entry.source = source;
			new_entry.source->AddRef();
			new_entry.d3d11_authority = false;
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
			if (!texture_index.Insert(source,
				static_cast<unsigned int>(textures.size() - 1)))
			{
				device->destroyResource(textures.back().handle);
				textures.back().source->Release();
				textures.pop_back();
				Fail(rts::render::RENDER_RESULT_OUT_OF_MEMORY,
					"draw failure: texture cache index allocation");
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
		else if (draw_texture_pinning && !draw_texture_pins.Pin(source))
		{
			return Fail("draw failure: current draw texture pin capacity");
		}
		return bind_result == rts::render::RENDER_RESULT_OK;
	}

#if defined(_WIN64)
	bool Bind_Native_Texture(unsigned int stage, TextureBaseClass *source,
		GpuHandle *handle)
	{
		if (handle == 0) return false;
		*handle = GpuHandle();
		if (source != 0)
		{
			rts::render::NativeW3DTextureHandle texture;
			if (!source->Acquire_Native_Texture(&texture) || !texture.isValid())
			{
				const RenderResult unbind_result = context->setTexture(stage,
					GpuHandle());
				if (unbind_result != rts::render::RENDER_RESULT_OK)
					Fail(unbind_result, "draw failure: native texture fallback unbind");
				return false;
			}
			*handle = texture.resource;
		}
		const RenderResult bind_result = context->setTexture(stage, *handle);
		if (bind_result != rts::render::RENDER_RESULT_OK)
		{
			Fail(bind_result, "draw failure: native texture binding");
			return false;
		}
		return true;
	}
#endif

	RenderResult Copy_Active_Color_Target_To_Texture(
		IDirect3DBaseTexture8 *destination)
	{
		if (destination == 0 || !frame_open || context == 0)
		{
			return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		}

		// A legacy render-target texture may have been cached earlier as a
		// sampling-only resource. Upgrade it through the normal target path so
		// the neutral destination has an RTV before issuing CopyResource.
		TextureEntry *existing = Find_Texture_Entry(destination);
		if ((existing == 0 || !existing->render_target) &&
			destination->GetType() == D3DRTYPE_TEXTURE)
		{
			IDirect3DSurface8 *surface = 0;
			IDirect3DTexture8 *texture = static_cast<IDirect3DTexture8 *>(destination);
			if (FAILED(texture->GetSurfaceLevel(0, &surface)) || surface == 0)
			{
				if (surface != 0) surface->Release();
				return rts::render::RENDER_RESULT_UNSUPPORTED;
			}
			GpuHandle upgraded_handle;
			RenderResult upgrade_result = rts::render::RENDER_RESULT_FAILED;
			const bool upgraded = Ensure_Render_Target(surface, &upgraded_handle,
				&upgrade_result);
			surface->Release();
			if (!upgraded)
			{
				return upgrade_result;
			}
		}

		GpuHandle handle;
		if (!Bind_Texture(0, destination, &handle))
		{
			const RenderResult bind_result = frame_outcome.hasCommandFailure() ?
				frame_outcome.commandResult() : rts::render::RENDER_RESULT_FAILED;
			if (!frame_outcome.hasCommandFailure())
			{
				Log_Result("D3D11 active color-target texture preparation failed",
					bind_result);
			}
			return bind_result;
		}
		const RenderResult copy_result = device->copyActiveColorTargetToTexture(
			handle);
		// Bind_Texture is used only to obtain/create the logical resource. Leave
		// the stage explicitly unbound so callers' subsequent state setup cannot
		// accidentally sample the destination while it is still being used as a
		// copy target.
		const RenderResult unbind_result = context->setTexture(0, GpuHandle());
		if (copy_result != rts::render::RENDER_RESULT_OK)
		{
			// Smudge capture is an optional visual pass.  A stale or incompatible
			// destination must be observable, but it must not discard the whole
			// frame.  Device removal remains fatal so End_Frame can run recovery.
			if (copy_result == rts::render::RENDER_RESULT_DEVICE_REMOVED)
			{
				Record_Result(copy_result,
					"D3D11 active color-target texture copy failed");
			}
			else
			{
				Log_Result("D3D11 active color-target texture copy failed",
					copy_result);
			}
			return copy_result;
		}
		if (unbind_result != rts::render::RENDER_RESULT_OK)
		{
			if (unbind_result == rts::render::RENDER_RESULT_DEVICE_REMOVED)
			{
				Record_Result(unbind_result,
					"D3D11 active color-target texture unbind failed");
			}
			else
			{
				Log_Result("D3D11 active color-target texture unbind failed",
					unbind_result);
			}
			return unbind_result;
		}
		TextureEntry *entry = Find_Texture_Entry(destination);
		if (entry != 0)
		{
			entry->d3d11_authority = true;
			entry->d3d8_dirty = false;
			entry->gpu_copy_valid = true;
			entry->gpu_copy_frame = frame_id;
			entry->gpu_copy_lease_epoch = display_epoch;
			entry->last_used_frame = frame_id;
		}
		return rts::render::RENDER_RESULT_OK;
	}

	TextureEntry *Find_Texture_Entry(IDirect3DBaseTexture8 *source,
		unsigned int *index_out = 0, bool touch = true)
	{
		if (source == 0)
		{
			return 0;
		}
		unsigned int found_index = 0;
		bool hit = texture_index.Find(source, &found_index) &&
			found_index < textures.size() &&
			textures[found_index].source == source;
		if (!hit)
		{
			// Keep a cold repair path for an index invariant violation.  Normal
			// draws never scan the texture vector.
			texture_index.Erase(source);
			for (unsigned int index = 0; index < textures.size(); ++index)
			{
				if (textures[index].source == source)
				{
					found_index = index;
					texture_index.Insert(source, index);
					hit = true;
					break;
				}
			}
		}
		cache_counters.RecordTextureLookup(hit);
		if (!hit)
		{
			return 0;
		}
		if (index_out != 0)
		{
			*index_out = found_index;
		}
		if (touch)
		{
			textures[found_index].last_used_frame = frame_id;
		}
		return &textures[found_index];
	}

	bool Ensure_Render_Target(IDirect3DSurface8 *surface,
		GpuHandle *handle, RenderResult *failure_result)
	{
		if (failure_result != 0)
		{
			*failure_result = rts::render::RENDER_RESULT_FAILED;
		}
		if (surface == 0 || handle == 0)
		{
			return false;
		}
		IDirect3DTexture8 *parent_texture = 0;
		if (FAILED(surface->GetContainer(IID_IDirect3DTexture8,
			reinterpret_cast<void **>(&parent_texture))) || parent_texture == 0 ||
			parent_texture->GetType() != D3DRTYPE_TEXTURE)
		{
			if (parent_texture != 0) parent_texture->Release();
			if (failure_result != 0)
			{
				*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
			}
			return false;
		}
		IDirect3DBaseTexture8 *parent = parent_texture;
		TextureEntry *entry = Find_Texture_Entry(parent);
		if (entry != 0 && entry->d3d8_dirty)
		{
			const bool target_pinned =
				Is_Target_Handle_Pinned(entry->handle, active_target) ||
				(pending_target_change &&
					Is_Target_Handle_Pinned(entry->handle, pending_target));
			if (target_pinned)
			{
				parent->Release();
				if (failure_result != 0)
					*failure_result = rts::render::RENDER_RESULT_INVALID_ARGUMENT;
				return false;
			}
			// A sampling-only entry cannot gain RTV capability in place.  Let the
			// normal target path recreate it with the required binding shape.
			const RenderResult refresh_result =
				entry->render_target && !entry->depth_stencil ?
				Refresh_Texture(*entry) : rts::render::RENDER_RESULT_UNSUPPORTED;
			if (refresh_result == rts::render::RENDER_RESULT_OK)
			{
				entry->d3d8_dirty = false;
				entry->d3d11_authority = true;
				++dynamic_texture_refresh_count;
				++dynamic_texture_in_place_count;
				cache_counters.RecordTextureRefresh();
			}
			else if (refresh_result != rts::render::RENDER_RESULT_UNSUPPORTED)
			{
				parent->Release();
				if (failure_result != 0)
					*failure_result = refresh_result;
				return false;
			}
			else
			{
				const unsigned int index = static_cast<unsigned int>(entry -
					&textures[0]);
				if (!Remove_Texture_Entry(index))
				{
					parent->Release();
					if (failure_result != 0)
						*failure_result = rts::render::RENDER_RESULT_FAILED;
					return false;
				}
				++dynamic_texture_refresh_count;
				++dynamic_texture_recreate_count;
				cache_counters.RecordTextureRefresh();
				entry = 0;
			}
		}
		if (entry != 0 && entry->render_target && !entry->depth_stencil &&
			entry->handle.isValid())
		{
			entry->d3d11_authority = true;
			*handle = entry->handle;
			parent->Release();
			return true;
		}
		if (entry != 0)
		{
			const unsigned int index = static_cast<unsigned int>(entry -
				&textures[0]);
			if (!Remove_Texture_Entry(index))
			{
				parent->Release();
				if (failure_result != 0)
					*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
				return false;
			}
		}
		if (textures.size() >= TEXTURE_CACHE_CAPACITY &&
			!Evict_Oldest_Texture())
		{
			parent->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
			return false;
		}
		TextureEntry new_entry;
		new_entry.source = parent;
		new_entry.source->AddRef();
		new_entry.render_target = true;
		new_entry.d3d11_authority = true;
		new_entry.last_used_frame = frame_id;
		RenderResult texture_result = rts::render::RENDER_RESULT_FAILED;
		if (!Create_Texture(parent, &new_entry.handle,
			&new_entry.render_target, &texture_result, true))
		{
			new_entry.source->Release();
			parent->Release();
			if (failure_result != 0) *failure_result = texture_result;
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
			parent->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
			return false;
		}
		if (!texture_index.Insert(parent,
			static_cast<unsigned int>(textures.size() - 1)))
		{
			device->destroyResource(textures.back().handle);
			textures.back().source->Release();
			textures.pop_back();
			parent->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
			return false;
		}
		*handle = new_entry.handle;
		parent->Release();
		if (failure_result != 0)
			*failure_result = rts::render::RENDER_RESULT_OK;
		return true;
	}

	bool Ensure_Depth_Target(IDirect3DSurface8 *surface,
		GpuHandle *handle, RenderResult *failure_result)
	{
		if (failure_result != 0)
			*failure_result = rts::render::RENDER_RESULT_FAILED;
		if (surface == 0 || handle == 0)
			return false;
		IDirect3DTexture8 *parent_texture = 0;
		if (FAILED(surface->GetContainer(IID_IDirect3DTexture8,
			reinterpret_cast<void **>(&parent_texture))) || parent_texture == 0 ||
			parent_texture->GetType() != D3DRTYPE_TEXTURE)
		{
			if (parent_texture != 0) parent_texture->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
			return false;
		}
		IDirect3DBaseTexture8 *parent = parent_texture;
		TextureEntry *entry = Find_Texture_Entry(parent);
		if (entry != 0 && entry->depth_stencil && entry->handle.isValid())
		{
			*handle = entry->handle;
			parent->Release();
			return true;
		}
		if (entry != 0)
		{
			const unsigned int index = static_cast<unsigned int>(entry -
				&textures[0]);
			if (!Remove_Texture_Entry(index))
			{
				parent->Release();
				if (failure_result != 0)
					*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
				return false;
			}
		}
		if (textures.size() >= TEXTURE_CACHE_CAPACITY &&
			!Evict_Oldest_Texture())
		{
			parent->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
			return false;
		}
		D3DSURFACE_DESC surface_desc;
		if (FAILED(surface->GetDesc(&surface_desc)))
		{
			parent->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_INVALID_ARGUMENT;
			return false;
		}
		rts::render::RenderFormat format;
		switch (surface_desc.Format)
		{
		case D3DFMT_D24S8:
			format = rts::render::RENDER_FORMAT_D24_UNORM_S8_UINT;
			break;
		default:
			parent->Release();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_UNSUPPORTED;
			return false;
		}
		TextureDescriptor descriptor;
		descriptor.width = surface_desc.Width;
		descriptor.height = surface_desc.Height;
		descriptor.mipCount = 1;
		descriptor.arrayCount = 1;
		descriptor.dimension = rts::render::RENDER_TEXTURE_2D;
		descriptor.format = format;
		descriptor.binding = rts::render::RENDER_TEXTURE_DEPTH_STENCIL;
		descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		const RenderResult create_result = device->createTexture(descriptor,
			0, 0, handle);
		if (create_result != rts::render::RENDER_RESULT_OK)
		{
			parent->Release();
			if (failure_result != 0) *failure_result = create_result;
			return false;
		}
		TextureEntry new_entry;
		new_entry.source = parent;
		new_entry.source->AddRef();
		new_entry.depth_stencil = true;
		new_entry.last_used_frame = frame_id;
		new_entry.handle = *handle;
		try
		{
			textures.push_back(new_entry);
		}
		catch (...)
		{
			device->destroyResource(*handle);
			new_entry.source->Release();
			parent->Release();
			*handle = GpuHandle();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
			return false;
		}
		if (!texture_index.Insert(parent,
			static_cast<unsigned int>(textures.size() - 1)))
		{
			device->destroyResource(textures.back().handle);
			textures.back().source->Release();
			textures.pop_back();
			parent->Release();
			*handle = GpuHandle();
			if (failure_result != 0)
				*failure_result = rts::render::RENDER_RESULT_OUT_OF_MEMORY;
			return false;
		}
		parent->Release();
		if (failure_result != 0)
			*failure_result = rts::render::RENDER_RESULT_OK;
		return true;
	}

	RenderResult Apply_Target(const RenderTargetBinding &binding)
	{
		if (!frame_open)
		{
			// No D3D11 output is bound between frames.  Replacing a deferred
			// transition therefore releases the previous producer lease before
			// the next frame can allocate another render target.
			Release_Unbound_Texture_Authority(binding);
			pending_target = binding;
			pending_target_change = true;
			target_transition_failed = false;
			return rts::render::RENDER_RESULT_OK;
		}
		const RenderResult result = context->setRenderTargets(binding);
		if (result == rts::render::RENDER_RESULT_OK)
		{
			target_transition_failed = false;
			active_target = binding;
			Release_Unbound_Texture_Authority(binding);
		}
		else
		{
			target_transition_failed = true;
			// Ensure_Render_Target may have created a new RTV before the context
			// rejected this transition.  It is not bound and must not pin the
			// cache after a failed transition.
			Release_Unbound_Texture_Authority(active_target);
		}
		return result;
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
		if (primitive_up_vertex_buffer.isValid() && release_native_resources)
		{
			device->destroyResource(primitive_up_vertex_buffer);
		}
		primitive_up_vertex_buffer = GpuHandle();
		primitive_up_vertex_capacity = 0;
		vertex_buffers.clear();
		index_buffers.clear();
		textures.clear();
		vertex_buffer_index.Clear();
		index_buffer_index.Clear();
		texture_index.Clear();
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
	// Destruction is normally reached after the owner has completed shutdown.
	// A transient native-resource failure must not make the first retry the
	// last attempt merely because the legacy ABI exposes a void Shutdown().
	if (Shutdown_Result() != rts::render::RENDER_RESULT_OK && m_impl != 0)
	{
		(void)Shutdown_Result();
	}
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
	if (m_impl->device != 0 || m_impl->shutdown_pending)
	{
		m_impl->Log("D3D11 legacy bridge rejected initialization while active");
		return false;
	}
	if (window == 0 || width == 0 || height == 0
#if !defined(_WIN64)
		|| legacy_device == 0
#endif
		)
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
#if defined(_WIN64)
	rts::render::ThreadedRenderOptions render_options;
	render_options.serial = !rts::UseParallelPipelines();
	m_impl->device = rts::render::CreateThreadedD3D11RenderDevice(render_options);
#else
	m_impl->device = rts::render::CreateD3D11RenderDevice();
#endif
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
	// The installed product must never degrade to WARP silently.  A software
	// device at high resolution can look like a renderer hang while producing
	// only a few frames per second; fail initialization clearly instead.
	parameters.allowSoftwareFallback = false;
#ifdef _DEBUG
	parameters.enableDebugLayer = true;
#else
	parameters.enableDebugLayer = false;
#endif
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
	if (m_impl->legacy_device != 0)
	{
		m_impl->legacy_device->AddRef();
	}
	m_impl->active_target = RenderTargetBinding();
	m_impl->pending_target = RenderTargetBinding();
	m_impl->pending_target_change = false;
	m_impl->target_transition_failed = false;
	m_impl->Log("D3D11 legacy bridge initialized");
	rts::render::RenderBackBufferInfo back_buffer_info;
	if (m_impl->device->getBackBufferInfo(&back_buffer_info) ==
		rts::render::RENDER_RESULT_OK && m_impl->log_file != 0)
	{
		fprintf(m_impl->log_file,
			"D3D11 hardware renderer back buffer: %ux%u format=%u\n",
			back_buffer_info.width, back_buffer_info.height,
			static_cast<unsigned int>(back_buffer_info.format));
		fflush(m_impl->log_file);
	}
	if (m_impl->context == 0)
	{
		m_impl->Log("D3D11 immediate context is unavailable");
		Shutdown();
		return false;
	}
	m_impl->native_w3d = new(std::nothrow) NativeW3D2;
	if (m_impl->native_w3d == 0)
	{
		m_impl->Log("D3D11 native WW3D resource host allocation failed");
		Shutdown();
		return false;
	}
	const RenderResult attach_result = m_impl->native_w3d->AttachBackend(
		m_impl->device, m_impl->context);
	if (attach_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 native WW3D resource host attachment failed",
			attach_result);
		Shutdown();
		return false;
	}
#if defined(_WIN64)
	const RenderResult bind_result = rts::render::BindNativeW3DBufferResources(
		&m_impl->native_w3d->Resources());
	if (bind_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 native buffer resource binding failed",
			bind_result);
		Shutdown();
		return false;
	}
	m_impl->native_buffer_resources_bound = true;
	const RenderResult texture_bind_result =
		rts::render::BindNativeW3DTextureResources(
			&m_impl->native_w3d->Resources());
	if (texture_bind_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 native texture resource binding failed",
			texture_bind_result);
		Shutdown();
		return false;
	}
	m_impl->native_texture_resources_bound = true;
#endif
	m_impl->shutdown_pending = false;
	return true;
}

rts::render::RenderResult D3D11LegacyBridge::Shutdown_Result()
{
	if (m_impl == 0)
	{
		return rts::render::RENDER_RESULT_OK;
	}
	// Teardown may span multiple owner-thread calls.  Publish the fail-closed
	// state before the first potentially failing operation and clear it only
	// after every owned resource and callback has been released.
	m_impl->shutdown_pending = true;
	m_impl->active_target = RenderTargetBinding();
	m_impl->pending_target = RenderTargetBinding();
	m_impl->pending_target_change = false;
	m_impl->target_transition_failed = true;
	if (m_impl->native_w3d != 0)
	{
		// A failed bridge teardown must not leave the public native owner
		// renderable while the bridge waits for its owner-thread retry.
		m_impl->native_w3d->SetActiveRenderTargetKind(
			rts::render::GAME_RENDER_TARGET_UNKNOWN);
	}
	if (m_impl->device == 0)
	{
		if (m_impl->owner_thread_id != 0)
		{
			m_impl->Require_Owner_Thread("failed initialization shutdown");
		}
		m_impl->capture_queue.shutdown(rts::render::RENDER_RESULT_FAILED);
		if (m_impl->native_w3d != 0)
		{
#if defined(_WIN64)
			if (m_impl->native_texture_resources_bound)
			{
				const RenderResult unbind_result =
					rts::render::UnbindNativeW3DTextureResources(
						&m_impl->native_w3d->Resources());
				if (unbind_result != rts::render::RENDER_RESULT_OK)
				{
					m_impl->Log_Result(
						"D3D11 native texture resource unbind failed",
						unbind_result);
					return unbind_result;
				}
				m_impl->native_texture_resources_bound = false;
			}
			if (m_impl->native_buffer_resources_bound)
			{
				const RenderResult unbind_result =
					rts::render::UnbindNativeW3DBufferResources(
						&m_impl->native_w3d->Resources());
				if (unbind_result != rts::render::RENDER_RESULT_OK)
				{
					m_impl->Log_Result(
						"D3D11 native buffer resource unbind failed",
						unbind_result);
					return unbind_result;
				}
				m_impl->native_buffer_resources_bound = false;
			}
#endif
			const RenderResult native_shutdown_result =
				m_impl->native_w3d->Shutdown();
			if (native_shutdown_result != rts::render::RENDER_RESULT_OK)
			{
				m_impl->Log_Result(
					"D3D11 native WW3D resources blocked bridge shutdown",
					native_shutdown_result);
				return native_shutdown_result;
			}
			delete m_impl->native_w3d;
			m_impl->native_w3d = 0;
		}
		if (m_impl->log_file != 0)
		{
			fclose(m_impl->log_file);
			m_impl->log_file = 0;
		}
		m_impl->frame_outcome = rts::render::RenderFrameOutcome();
		m_impl->active_target = RenderTargetBinding();
		m_impl->pending_target = RenderTargetBinding();
		m_impl->pending_target_change = false;
		m_impl->target_transition_failed = true;
		m_impl->owner_thread_id = 0;
		m_impl->shutdown_pending = false;
		return rts::render::RENDER_RESULT_OK;
	}
	m_impl->Require_Owner_Thread("shutdown");
#if defined(_WIN64)
	if (m_impl->Is_Threaded())
	{
		m_impl->Cancel_Threaded_Frame(rts::render::RENDER_RESULT_FAILED);
		m_impl->frame_open = false;
		m_impl->Fence_Render();
		rts::render::ThreadedRenderMetrics metrics;
		if (rts::render::GetThreadedRenderMetrics(m_impl->device, &metrics) && m_impl->log_file != 0)
		{
			fprintf(m_impl->log_file,
				"render_owner_submitted=%llu completed=%llu failed=%llu overlap=%llu waits=%llu wait_ns=%llu execute_ns=%llu peak_packets=%u peak_bytes=%zu\n",
				static_cast<unsigned long long>(metrics.submittedFrames),
				static_cast<unsigned long long>(metrics.completedFrames),
				static_cast<unsigned long long>(metrics.failedFrames),
				static_cast<unsigned long long>(metrics.producerOverlapFrames),
				static_cast<unsigned long long>(metrics.backpressureWaits),
				static_cast<unsigned long long>(metrics.producerWaitNanoseconds),
				static_cast<unsigned long long>(metrics.ownerExecutionNanoseconds),
				metrics.peakPendingPackets, metrics.peakPacketBytes);
		}
	}
	if (m_impl->log_file != 0)
	{
		// Owner-side shutdown snapshots add no polling or file writes to the
		// hot path. Resource counters describe the most recent loader session.
		const rts::ResourceIoMetrics resources = TextureLoader::Get_Resource_Load_Metrics();
		fprintf(m_impl->log_file,
			"resource_io_accepted=%llu rejected=%llu reads=%llu decoded=%llu fallback=%llu ownership_failures=%llu io_decode_overlap=%u read_ns=%llu decode_ns=%llu wait_ns=%llu budget_stalls=%llu peak_input_bytes=%zu peak_decode_bytes=%zu\n",
			static_cast<unsigned long long>(resources.accepted),
			static_cast<unsigned long long>(resources.rejected),
			static_cast<unsigned long long>(resources.reads),
			static_cast<unsigned long long>(resources.decoded),
			static_cast<unsigned long long>(resources.serialFallbacks),
			static_cast<unsigned long long>(resources.ownershipFailures),
			resources.maximumOverlappingIoAndDecode,
			static_cast<unsigned long long>(resources.readNanoseconds),
			static_cast<unsigned long long>(resources.decodeNanoseconds),
			static_cast<unsigned long long>(resources.ownerWaitNanoseconds),
			static_cast<unsigned long long>(resources.decodeBudgetStalls),
			resources.inputHighWater, resources.decodeHighWater);
		const rts::JobSystemMetrics jobs = rts::JobSystem::instance().metrics();
		fprintf(m_impl->log_file,
			"job_submitted=%llu executed=%llu steals=%llu owner_help=%llu failures=%llu fallback=%llu affinity_failures=%llu queue_high_water=%u peak_active_workers=%u available_cpus=%u reserved_owner_cpus=%u selected_worker_cpus=%u\n",
			static_cast<unsigned long long>(jobs.submittedJobCount),
			static_cast<unsigned long long>(jobs.executedJobCount),
			static_cast<unsigned long long>(jobs.stealCount),
			static_cast<unsigned long long>(jobs.ownerHelpCount),
			static_cast<unsigned long long>(jobs.failedJobCount),
			static_cast<unsigned long long>(jobs.serialFallbackCount),
			static_cast<unsigned long long>(jobs.affinityFailureCount),
			jobs.injectionHighWater, jobs.maximumActiveWorkers,
			jobs.availableLogicalCpuCount, jobs.reservedOwnerCpuCount, jobs.selectedWorkerCpuCount);
	}
#endif
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
			"draws=%u failures=%u ccw_front=%u unculled=%u color_write=%u depth_never=%u clears=%u viewports=%u buffer_lookups=%u buffer_hits=%u texture_lookups=%u texture_hits=%u buffer_uploads=%u dynamic_texture_refreshes=%u dynamic_texture_in_place=%u dynamic_texture_recreates=%u vertex_buffers=%u index_buffers=%u textures=%u\n",
			m_impl->draw_count, m_impl->draw_failure_count,
			m_impl->counter_clockwise_draw_count, m_impl->unculled_draw_count,
			m_impl->color_write_draw_count, m_impl->depth_never_draw_count,
			m_impl->clear_count, m_impl->viewport_count,
			m_impl->cache_counters.bufferLookups,
			m_impl->cache_counters.bufferHits,
			m_impl->cache_counters.textureLookups,
			m_impl->cache_counters.textureHits,
			m_impl->cache_counters.bufferUploads,
			m_impl->dynamic_texture_refresh_count,
			m_impl->dynamic_texture_in_place_count,
			m_impl->dynamic_texture_recreate_count,
			static_cast<unsigned int>(m_impl->vertex_buffers.size()),
			static_cast<unsigned int>(m_impl->index_buffers.size()),
			static_cast<unsigned int>(m_impl->textures.size()));
		fflush(m_impl->log_file);
	}
	m_impl->Release_Caches();
#if defined(_WIN64)
	m_impl->Fence_Render();
	if (m_impl->native_texture_resources_bound)
	{
		const RenderResult unbind_result =
			rts::render::UnbindNativeW3DTextureResources(
				&m_impl->native_w3d->Resources());
		if (unbind_result != rts::render::RENDER_RESULT_OK)
		{
			m_impl->Log_Result("D3D11 native texture resource unbind failed",
				unbind_result);
			return unbind_result;
		}
		m_impl->native_texture_resources_bound = false;
	}
	if (m_impl->native_buffer_resources_bound)
	{
		const RenderResult unbind_result =
			rts::render::UnbindNativeW3DBufferResources(
				&m_impl->native_w3d->Resources());
		if (unbind_result != rts::render::RENDER_RESULT_OK)
		{
			m_impl->Log_Result("D3D11 native buffer resource unbind failed",
				unbind_result);
			return unbind_result;
		}
		m_impl->native_buffer_resources_bound = false;
	}
#endif
	const RenderResult native_shutdown_result = m_impl->native_w3d == 0 ?
		rts::render::RENDER_RESULT_OK : m_impl->native_w3d->Shutdown();
	if (native_shutdown_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result(
			"D3D11 native WW3D resources blocked backend shutdown",
			native_shutdown_result);
		return native_shutdown_result;
	}
	delete m_impl->native_w3d;
	m_impl->native_w3d = 0;
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
	m_impl->active_target = RenderTargetBinding();
	m_impl->pending_target = RenderTargetBinding();
	m_impl->pending_target_change = false;
	m_impl->target_transition_failed = true;
	m_impl->owner_thread_id = 0;
#if defined(_WIN64)
	m_impl->deferred_failure = rts::render::RenderFrameOutcome();
	m_impl->deferred_failure_sequence = 0;
	m_impl->recovered_failure_sequence = 0;
	m_impl->async_resource_failure = false;
#endif
	m_impl->shutdown_pending = false;
	return rts::render::RENDER_RESULT_OK;
}

void D3D11LegacyBridge::Shutdown()
{
	(void)Shutdown_Result();
}

bool D3D11LegacyBridge::Prepare_Legacy_Device_Reset()
{
#if defined(_WIN64)
	if (m_impl != 0 && m_impl->device != 0) m_impl->Service_Render_Completions();
#endif
	if (!Is_Active())
	{
		return false;
	}
	m_impl->Require_Owner_Thread("legacy device reset");
	if (m_impl->frame_open)
	{
		rts::render::RenderFrameOutcome outcome;
		End_Frame(false, &outcome);
		if (!Is_Active() || !outcome.isOperational())
		{
			return false;
		}
	}
	m_impl->capture_queue.advanceGeneration();
	m_impl->capture_queue.cancelStale(rts::render::RENDER_RESULT_FAILED);
	m_impl->active_target = RenderTargetBinding();
	m_impl->pending_target = RenderTargetBinding();
	m_impl->pending_target_change = false;
	m_impl->target_transition_failed = false;
	m_impl->pending_viewport = false;
	m_impl->pending_clear = false;
#if defined(_WIN64)
	m_impl->Fence_Render();
#endif
	m_impl->Release_Caches();
#if defined(_WIN64)
	m_impl->Fence_Render();
#endif
	return Is_Active();
}

bool D3D11LegacyBridge::Is_Active() const
{
	if (m_impl == 0 || m_impl->shutdown_pending || m_impl->device == 0 ||
		m_impl->context == 0 ||
		!m_impl->device->isOperational())
	{
		return false;
	}
#if defined(_WIN64)
	return m_impl->native_buffer_resources_bound &&
		m_impl->native_texture_resources_bound &&
		m_impl->native_w3d != 0 &&
		m_impl->native_w3d->IsAttachedToBorrowedBackend();
#else
	return true;
#endif
}

void D3D11LegacyBridge::Begin_Display_Iteration()
{
#if defined(_WIN64)
	if (m_impl != 0 && m_impl->device != 0) m_impl->Service_Render_Completions();
#endif
	if (!Is_Active())
	{
		return;
	}
	m_impl->Require_Owner_Thread("display iteration");
	// A display iteration is the ownership boundary for GPU-copy leases.  It
	// may contain several hidden RTT frames followed by one visible frame, or
	// no visible frame at all when rendering is disabled/lost.  Advancing here
	// expires removed textures in both cases without releasing new RTT content
	// before the visible draw can sample it.
	m_impl->display_epoch = rts::render::Advance_D3D11_Display_Epoch(
		m_impl->display_epoch);
}

bool D3D11LegacyBridge::Begin_Frame()
{
#if defined(_WIN64)
	if (m_impl != 0 && m_impl->device != 0) m_impl->Service_Render_Completions();
#endif
	if (!Is_Active() || m_impl->frame_open)
	{
		return false;
	}
	unsigned int drained_cleanup = 0;
	const RenderResult cleanup_result = m_impl->native_w3d == 0 ?
		rts::render::RENDER_RESULT_INVALID_ARGUMENT :
		m_impl->native_w3d->DrainResourceCleanup(0, &drained_cleanup);
	if (cleanup_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result(
			"D3D11 native WW3D deferred resource cleanup failed",
			cleanup_result);
		return false;
	}
	++m_impl->frame_id;
	m_impl->frame_draw_count = 0;
	m_impl->capture_draw_logged = false;
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
	// D3D11RenderDevice::beginFrame starts from the swap-chain target.  Reapply
	// the last target every frame, using the newer pending transition when one
	// was recorded while the frame was closed.
	const RenderTargetBinding frame_target = m_impl->pending_target_change ?
		m_impl->pending_target : m_impl->active_target;
	const RenderResult target_result = m_impl->context->setRenderTargets(
		frame_target);
	if (!m_impl->Record_Result(target_result,
		"D3D11 legacy bridge render-target transition failed"))
	{
		m_impl->target_transition_failed = true;
		m_impl->context->endFrame();
		m_impl->frame_open = false;
#if defined(_WIN64)
		m_impl->Cancel_Threaded_Frame(target_result);
#endif
		return false;
	}
	m_impl->active_target = frame_target;
	m_impl->pending_target_change = false;
	m_impl->target_transition_failed = false;
	m_impl->Release_Unbound_Texture_Authority(frame_target);
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
#if defined(_WIN64)
			m_impl->Cancel_Threaded_Frame(viewport_result);
#endif
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
#if defined(_WIN64)
			m_impl->Cancel_Threaded_Frame(clear_result);
#endif
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
#if defined(_WIN64)
	// A backend failure may arrive while the game is recording the next frame.
	// Still close that recording; Is_Active must not abandon reserved packets.
	if (m_impl != 0 && m_impl->device != 0 && m_impl->Is_Threaded() && m_impl->frame_open)
		return m_impl->End_Threaded_Frame(present_frame, outcome);
#endif
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
	const bool frame_succeeded = end_result == rts::render::RENDER_RESULT_OK &&
		!m_impl->frame_outcome.hasCommandFailure();
	m_impl->Complete_GPU_Copy_Frame(frame_succeeded, m_impl->frame_id);
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
	if (m_impl->frame_outcome.hasCommandFailure())
	{
		// Keep the last complete visible frame. Presenting the commands that
		// succeeded before a rejected draw/state transition causes one-frame
		// holes and unstable object flicker.
		const RenderResult command_result =
			m_impl->frame_outcome.commandResult();
		m_impl->capture_queue.cancelCurrent(command_result);
		m_impl->Log_Result(
			"D3D11 legacy bridge dropped an incomplete frame after a command failure",
			command_result);
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
		char frame_message[160];
		snprintf(frame_message, sizeof(frame_message),
			"D3D11 visual capture frame=%u draws=%u",
			m_impl->frame_id, m_impl->frame_draw_count);
		m_impl->Log(frame_message);
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
	RenderResult present_result;
	{
		rts::frame_timing::Scope timing(rts::frame_timing::RendererPresent);
		present_result = m_impl->device->present();
	}
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
	++m_impl->clear_count;
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
	++m_impl->viewport_count;
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

bool D3D11LegacyBridge::Is_Render_Target_Operational() const
{
	return Is_Active() && m_impl != 0 &&
		!m_impl->target_transition_failed;
}

void D3D11LegacyBridge::Record_Visible_Submission_Failure()
{
	if (!Is_Active() || m_impl == 0)
	{
		return;
	}
	m_impl->Record_Result(rts::render::RENDER_RESULT_FAILED,
		"D3D11 visible submission suppressed because its target is unavailable");
}

rts::render::RenderResult D3D11LegacyBridge::Set_Render_Target_Default()
{
	if (!Is_Active())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->Require_Owner_Thread("default render-target transition");
	RenderTargetBinding binding;
	const RenderResult result = m_impl->Apply_Target(binding);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 default render-target transition failed", result);
	}
	return result;
}

rts::render::RenderResult D3D11LegacyBridge::Copy_Active_Color_Target_To_Texture(
	IDirect3DBaseTexture8 *destination)
{
	if (!Is_Active() || destination == 0)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->Require_Owner_Thread("active color-target texture copy");
	return m_impl->Copy_Active_Color_Target_To_Texture(destination);
}

bool D3D11LegacyBridge::Acquire_Copied_Texture_Content(
	IDirect3DBaseTexture8 *texture)
{
	if (!Is_Active() || texture == 0)
	{
		return false;
	}
	unsigned int found_index = 0;
	const bool valid = m_impl->texture_index.Find(texture, &found_index) &&
		found_index < m_impl->textures.size() &&
		m_impl->textures[found_index].source == texture &&
		m_impl->textures[found_index].gpu_copy_valid;
	if (valid)
	{
		m_impl->textures[found_index].gpu_copy_lease_epoch =
			m_impl->display_epoch;
		m_impl->textures[found_index].last_used_frame = m_impl->frame_id;
	}
	return valid;
}

rts::render::RenderResult D3D11LegacyBridge::Set_Render_Target_Surfaces(
	IDirect3DSurface8 *color_surface, IDirect3DSurface8 *depth_surface,
	bool use_default_depth)
{
	if (!Is_Active())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->Require_Owner_Thread("render-target transition");
	if (color_surface == 0)
	{
		return Set_Render_Target_Default();
	}
	RenderTargetBinding binding;
	binding.useBackBufferColor = false;
	binding.hasColor = true;
	RenderResult target_result = rts::render::RENDER_RESULT_FAILED;
	if (!m_impl->Ensure_Render_Target(color_surface,
		&binding.color.resource, &target_result))
	{
		m_impl->target_transition_failed = true;
		m_impl->Release_Unbound_Texture_Authority(m_impl->active_target);
		m_impl->Log_Result("D3D11 color render-target creation failed",
			target_result);
		return target_result;
	}
	// DX8Wrapper determines this identity before changing the native binding.
	// Re-querying the device here would only observe the newly requested custom
	// surface and would incorrectly classify every explicit depth target as the
	// swap-chain depth buffer.
	if (use_default_depth)
	{
		binding.useBackBufferDepth = true;
	}
	else if (depth_surface != 0)
	{
		binding.useBackBufferDepth = false;
		binding.hasDepth = true;
		if (!m_impl->Ensure_Depth_Target(depth_surface,
			&binding.depth.resource, &target_result))
		{
			m_impl->target_transition_failed = true;
			m_impl->Release_Unbound_Texture_Authority(m_impl->active_target);
			m_impl->Log_Result("D3D11 depth render-target creation failed",
				target_result);
			return target_result;
		}
	}
	else
	{
		binding.useBackBufferDepth = false;
	}
	const RenderResult result = m_impl->Apply_Target(binding);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Log_Result("D3D11 render-target transition failed", result);
	}
	return result;
}

#if defined(_WIN64)
rts::render::RenderResult D3D11LegacyBridge::Set_Render_Target_Textures(
	TextureBaseClass *color_texture, TextureBaseClass *depth_texture,
	bool use_default_depth)
{
	if (!Is_Active()) return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	m_impl->Require_Owner_Thread("native render-target transition");
	if (color_texture == 0) return Set_Render_Target_Default();

	rts::render::NativeW3DSurfaceHandle color_surface;
	if (!color_texture->Acquire_Native_Surface(0, 0, true, &color_surface) ||
		!color_surface.isValid())
	{
		m_impl->target_transition_failed = true;
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderTargetBinding binding;
	binding.useBackBufferColor = false;
	binding.hasColor = true;
	binding.color.resource = color_surface.texture.resource;
	binding.color.mip = color_surface.mipLevel;
	binding.color.arraySlice = color_surface.arraySlice;

	rts::render::NativeW3DSurfaceHandle depth_surface;
	if (use_default_depth)
	{
		binding.useBackBufferDepth = true;
	}
	else if (depth_texture != 0)
	{
		if (!depth_texture->Acquire_Native_Surface(0, 0, true, &depth_surface) ||
			!depth_surface.isValid())
		{
			m_impl->target_transition_failed = true;
			return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		}
		binding.useBackBufferDepth = false;
		binding.hasDepth = true;
		binding.depth.resource = depth_surface.texture.resource;
		binding.depth.mip = depth_surface.mipLevel;
		binding.depth.arraySlice = depth_surface.arraySlice;
	}
	else
	{
		binding.useBackBufferDepth = false;
	}

	const RenderResult result = m_impl->Apply_Target(binding);
	if (result != rts::render::RENDER_RESULT_OK)
		return result;
	rts::render::NativeW3DGpuContentLease color_lease;
	if (!color_texture->Publish_Native_Output(color_surface, &color_lease))
	{
		m_impl->target_transition_failed = true;
		return rts::render::RENDER_RESULT_FAILED;
	}
	if (binding.hasDepth)
	{
		rts::render::NativeW3DGpuContentLease depth_lease;
		if (!depth_texture->Publish_Native_Output(depth_surface, &depth_lease))
		{
			m_impl->target_transition_failed = true;
			return rts::render::RENDER_RESULT_FAILED;
		}
	}
	return rts::render::RENDER_RESULT_OK;
}
#endif

void D3D11LegacyBridge::Invalidate_Buffer(IUnknown *buffer)
{
	if (!Is_Active() || buffer == 0)
	{
		return;
	}
	Impl::BufferEntry *vertex_entry = m_impl->Find_Buffer(
		m_impl->vertex_buffers, m_impl->vertex_buffer_index, buffer, false);
	if (vertex_entry != 0)
	{
		vertex_entry->source_dirty = true;
		vertex_entry->raw_range_valid = false;
	}
	Impl::BufferEntry *index_entry = m_impl->Find_Buffer(
		m_impl->index_buffers, m_impl->index_buffer_index, buffer, false);
	if (index_entry != 0)
	{
		index_entry->source_dirty = true;
		index_entry->raw_range_valid = false;
	}
}

void D3D11LegacyBridge::Invalidate_Buffer_Range(IUnknown *buffer,
	unsigned int binding, size_t destination_offset, size_t byte_count,
	rts::render::RenderBufferUpdateMode mode)
{
	if (!Is_Active() || buffer == 0 || byte_count == 0)
	{
		return;
	}
	Impl::BufferEntry *entry = 0;
	if (binding == rts::render::RENDER_BUFFER_VERTEX)
	{
		entry = m_impl->Find_Buffer(m_impl->vertex_buffers,
			m_impl->vertex_buffer_index, buffer, false);
	}
	else if (binding == rts::render::RENDER_BUFFER_INDEX)
	{
		entry = m_impl->Find_Buffer(m_impl->index_buffers,
			m_impl->index_buffer_index, buffer, false);
	}
	if (entry == 0)
	{
		return;
	}
	if (destination_offset > entry->byte_count ||
		byte_count > entry->byte_count - destination_offset ||
		(mode != rts::render::RENDER_BUFFER_UPDATE_DISCARD &&
		 mode != rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE) ||
		(mode == rts::render::RENDER_BUFFER_UPDATE_DISCARD &&
		 destination_offset != 0))
	{
		entry->source_dirty = true;
		entry->raw_range_valid = false;
		return;
	}

	if (mode == rts::render::RENDER_BUFFER_UPDATE_DISCARD)
	{
		entry->source_dirty = true;
		entry->raw_range_valid = true;
		entry->raw_range_offset = 0;
		entry->raw_range_bytes = byte_count;
		entry->raw_range_mode = mode;
		return;
	}

	if (!entry->source_dirty)
	{
		entry->source_dirty = true;
		entry->raw_range_valid = true;
		entry->raw_range_offset = destination_offset;
		entry->raw_range_bytes = byte_count;
		entry->raw_range_mode = mode;
		return;
	}
	if (!entry->raw_range_valid)
	{
		return;
	}
	const size_t previous_end = entry->raw_range_offset +
		entry->raw_range_bytes;
	const size_t changed_end = destination_offset + byte_count;
	if (destination_offset > previous_end ||
		changed_end < entry->raw_range_offset)
	{
		// A gap could contain bytes that the GPU is still consuming.  Preserve
		// the conservative full-copy path instead of rewriting that gap while
		// coalescing unrelated NO_OVERWRITE updates.
		entry->raw_range_valid = false;
		return;
	}
	const size_t combined_start = destination_offset < entry->raw_range_offset ?
		destination_offset : entry->raw_range_offset;
	const size_t combined_end = changed_end > previous_end ?
		changed_end : previous_end;
	entry->raw_range_offset = combined_start;
	entry->raw_range_bytes = combined_end - combined_start;
}

bool D3D11LegacyBridge::Publish_Buffer_Change(IUnknown *buffer,
	unsigned int binding, const void *data, size_t byte_count,
	size_t destination_offset, rts::render::RenderBufferUpdateMode mode,
	unsigned int source_generation)
{
	if (!Is_Active() || !m_impl->frame_open || buffer == 0 || data == 0 ||
		byte_count == 0)
	{
		return false;
	}
	m_impl->Require_Owner_Thread("legacy buffer publication");
	std::vector<Impl::BufferEntry> *entries = 0;
	rts::render::LegacyBridgePointerIndex *cache_index = 0;
	Impl::BufferEntry *entry = 0;
	if (binding == rts::render::RENDER_BUFFER_VERTEX)
	{
		entries = &m_impl->vertex_buffers;
		cache_index = &m_impl->vertex_buffer_index;
	}
	else if (binding == rts::render::RENDER_BUFFER_INDEX)
	{
		entries = &m_impl->index_buffers;
		cache_index = &m_impl->index_buffer_index;
	}
	else
	{
		return false;
	}
	entry = m_impl->Find_Buffer(*entries, *cache_index, buffer, false);
	if (entry == 0)
	{
		size_t source_byte_count = 0;
		unsigned int source_stride = 1;
		if (binding == rts::render::RENDER_BUFFER_VERTEX)
		{
			D3DVERTEXBUFFER_DESC descriptor8;
			memset(&descriptor8, 0, sizeof(descriptor8));
			IDirect3DVertexBuffer8 *source =
				static_cast<IDirect3DVertexBuffer8 *>(buffer);
			if (FAILED(source->GetDesc(&descriptor8)) || descriptor8.Size == 0)
			{
				return false;
			}
			source_byte_count = descriptor8.Size;
		}
		else
		{
			D3DINDEXBUFFER_DESC descriptor8;
			memset(&descriptor8, 0, sizeof(descriptor8));
			IDirect3DIndexBuffer8 *source =
				static_cast<IDirect3DIndexBuffer8 *>(buffer);
			if (FAILED(source->GetDesc(&descriptor8)) || descriptor8.Size == 0 ||
				descriptor8.Format != D3DFMT_INDEX16)
			{
				return false;
			}
			source_byte_count = descriptor8.Size;
			source_stride = sizeof(unsigned short);
		}
		if (destination_offset > source_byte_count ||
			byte_count > source_byte_count - destination_offset ||
			(mode != rts::render::RENDER_BUFFER_UPDATE_PRESERVE &&
			 mode != rts::render::RENDER_BUFFER_UPDATE_DISCARD &&
			 mode != rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE) ||
			(mode == rts::render::RENDER_BUFFER_UPDATE_DISCARD &&
			 destination_offset != 0))
		{
			return false;
		}
		if (entries->size() >= BUFFER_CACHE_CAPACITY)
		{
			m_impl->Evict_Oldest_Buffer(*entries, *cache_index);
		}
		Impl::BufferEntry new_entry;
		new_entry.source = buffer;
		new_entry.source->AddRef();
		new_entry.byte_count = source_byte_count;
		new_entry.last_used_frame = m_impl->frame_id;
		rts::render::BufferDescriptor descriptor;
		descriptor.byteCount = source_byte_count;
		descriptor.stride = source_stride;
		descriptor.binding = binding;
		descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
		const RenderResult create_result = m_impl->device->createBuffer(
			descriptor, 0, 0, &new_entry.handle);
		if (create_result != rts::render::RENDER_RESULT_OK)
		{
			new_entry.source->Release();
			return false;
		}
		try
		{
			entries->push_back(new_entry);
		}
		catch (...)
		{
			m_impl->device->destroyResource(new_entry.handle);
			new_entry.source->Release();
			return false;
		}
		if (!cache_index->Insert(buffer,
			static_cast<unsigned int>(entries->size() - 1)))
		{
			m_impl->device->destroyResource(entries->back().handle);
			entries->back().source->Release();
			entries->pop_back();
			return false;
		}
		entry = &entries->back();
	}
	if (destination_offset > entry->byte_count ||
		byte_count > entry->byte_count - destination_offset)
	{
		return false;
	}
	const bool full_publication = destination_offset == 0 &&
		byte_count == entry->byte_count &&
		mode != rts::render::RENDER_BUFFER_UPDATE_NO_OVERWRITE;
	const bool discard_publication = destination_offset == 0 &&
		mode == rts::render::RENDER_BUFFER_UPDATE_DISCARD;
	if (entry->source_dirty && !full_publication &&
		!discard_publication)
	{
		// A partial patch cannot make an already-dirty cached copy authoritative.
		// Leave it dirty so the next draw performs the conservative full upload.
		return false;
	}
	const RenderResult result = m_impl->context->updateBuffer(entry->handle,
		data, byte_count, destination_offset, mode);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		entry->source_dirty = true;
		return false;
	}
	m_impl->cache_counters.RecordBufferUpload();
	entry->source_dirty = false;
	entry->raw_range_valid = false;
	if (source_generation != 0)
	{
		entry->source_generation = source_generation;
	}
	return true;
}

bool D3D11LegacyBridge::Publish_Texture_BGRA8_Change(
	IDirect3DBaseTexture8 *texture, const void *data, size_t row_pitch,
	size_t slice_pitch)
{
	if (!Is_Active() || texture == 0 || data == 0 || row_pitch == 0 ||
		slice_pitch == 0 || texture->GetType() != D3DRTYPE_TEXTURE)
	{
		return false;
	}
	m_impl->Require_Owner_Thread("legacy BGRA8 texture publication");

	IDirect3DTexture8 *source = static_cast<IDirect3DTexture8 *>(texture);
	D3DSURFACE_DESC source_descriptor;
	memset(&source_descriptor, 0, sizeof(source_descriptor));
	if (source->GetLevelCount() != 1 ||
		FAILED(source->GetLevelDesc(0, &source_descriptor)) ||
		source_descriptor.Width == 0 || source_descriptor.Height == 0 ||
		(source_descriptor.Usage &
			(D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0 ||
		(source_descriptor.Format != D3DFMT_X8R8G8B8 &&
		 source_descriptor.Format != D3DFMT_A8R8G8B8))
	{
		return false;
	}

	size_t minimum_row_pitch = 0;
	size_t minimum_slice_pitch = 0;
	if (!Checked_Multiply(static_cast<size_t>(source_descriptor.Width), 4,
			&minimum_row_pitch) || row_pitch < minimum_row_pitch ||
		!Checked_Multiply(row_pitch,
			static_cast<size_t>(source_descriptor.Height),
			&minimum_slice_pitch) || slice_pitch < minimum_slice_pitch ||
		row_pitch > std::numeric_limits<unsigned int>::max() ||
		slice_pitch > std::numeric_limits<unsigned int>::max() ||
		slice_pitch > MAX_TEXTURE_REFRESH_BYTES)
	{
		return false;
	}

	TextureDescriptor descriptor;
	descriptor.width = source_descriptor.Width;
	descriptor.height = source_descriptor.Height;
	descriptor.mipCount = 1;
	descriptor.arrayCount = 1;
	descriptor.dimension = rts::render::RENDER_TEXTURE_2D;
	descriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
	descriptor.binding = rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
	descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	TextureSubresourceData subresource;
	subresource.data = data;
	subresource.rowPitch = row_pitch;
	subresource.slicePitch = slice_pitch;

	Impl::TextureEntry *entry = m_impl->Find_Texture_Entry(texture, 0, false);
	if (entry == 0)
	{
		if (m_impl->textures.size() >= TEXTURE_CACHE_CAPACITY &&
			!m_impl->Evict_Oldest_Texture())
		{
			return false;
		}
		Impl::TextureEntry new_entry;
		new_entry.source = texture;
		new_entry.source->AddRef();
		new_entry.last_used_frame = m_impl->frame_id;
		const RenderResult create_result = m_impl->device->createTexture(
			descriptor, &subresource, 1, &new_entry.handle);
		if (create_result != rts::render::RENDER_RESULT_OK)
		{
			new_entry.source->Release();
			return false;
		}
		try
		{
			m_impl->textures.push_back(new_entry);
		}
		catch (...)
		{
			m_impl->device->destroyResource(new_entry.handle);
			new_entry.source->Release();
			return false;
		}
		if (!m_impl->texture_index.Insert(texture,
			static_cast<unsigned int>(m_impl->textures.size() - 1)))
		{
			m_impl->device->destroyResource(m_impl->textures.back().handle);
			m_impl->textures.back().source->Release();
			m_impl->textures.pop_back();
			return false;
		}
		entry = &m_impl->textures.back();
	}
	else
	{
		if (entry->render_target || entry->depth_stencil ||
			!entry->handle.isValid())
		{
			entry->d3d11_authority = false;
			entry->d3d8_dirty = true;
			return false;
		}
		const RenderResult refresh_result = m_impl->device->refreshTexture(
			entry->handle, descriptor, &subresource, 1);
		if (refresh_result != rts::render::RENDER_RESULT_OK)
		{
			entry->d3d11_authority = false;
			entry->d3d8_dirty = true;
			entry->gpu_copy_valid = false;
			entry->gpu_copy_lease_epoch = 0;
			return false;
		}
		++m_impl->dynamic_texture_refresh_count;
		++m_impl->dynamic_texture_in_place_count;
		m_impl->cache_counters.RecordTextureRefresh();
	}

	entry->d3d11_authority = false;
	entry->d3d8_dirty = false;
	entry->gpu_copy_valid = false;
	entry->gpu_copy_frame = 0;
	entry->gpu_copy_lease_epoch = 0;
	entry->last_used_frame = m_impl->frame_id;
	return true;
}

void D3D11LegacyBridge::Invalidate_Texture(IDirect3DBaseTexture8 *texture)
{
	if (!Is_Active() || texture == 0)
	{
		return;
	}
	Impl::TextureEntry *found_entry = m_impl->Find_Texture_Entry(
		texture, 0, false);
	if (found_entry != 0)
	{
		Impl::TextureEntry &entry = *found_entry;
		// This notification follows a completed D3D8-side write.  Mark the
		// legacy copy as authoritative, but defer the CPU readback until the
		// texture is actually consumed by a D3D11 draw. Texture loading and
		// terrain/water update paths can
		// publish several notifications in one frame; refreshing here would make
		// every notification perform a full D3D8 surface readback, even when a
		// later notification supersedes it.  The dirty bit is intentionally
		// idempotent, so consecutive notifications coalesce into one refresh.
		entry.d3d11_authority = false;
		entry.d3d8_dirty = true;
		entry.gpu_copy_valid = false;
		entry.gpu_copy_lease_epoch = 0;
		entry.last_used_frame = m_impl->frame_id;
	}
}

bool D3D11LegacyBridge::Draw(VertexBufferClass *vertex_buffer,
	IndexBufferClass *index_buffer, unsigned int primitive_type,
	unsigned int min_vertex_index, unsigned int vertex_count,
	unsigned int start_index, unsigned int primitive_count,
	unsigned int base_vertex)
{
	if (!Is_Active() || !m_impl->frame_open || vertex_buffer == 0 ||
		index_buffer == 0)
	{
		return Is_Active() ? m_impl->Fail("draw failure: invalid draw state") : false;
	}
	if (!Can_Submit_D3D11_Legacy_Draw())
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_FAILED,
			"draw failure: neutral texture stage state publication was rejected");
	}
	unsigned int index_count = 0;
	if (!Primitive_Index_Count(primitive_type, primitive_count, &index_count))
	{
		return m_impl->Fail("draw failure: indexed primitive count overflow or unsupported topology");
	}
	if (!rts::render::Is_Legacy_Indexed_Range_Valid(
		static_cast<unsigned long>(index_buffer->Get_Index_Count()),
		start_index, index_count))
	{
		return m_impl->Fail("draw failure: indexed range exceeds buffer");
	}
	if (!rts::render::Is_D3D11_Base_Vertex_Valid(base_vertex))
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"draw failure: base vertex exceeds D3D11 range");
	}
	if (!rts::render::Is_Legacy_Vertex_Range_Valid(
		static_cast<unsigned long>(vertex_buffer->Get_Vertex_Count()),
		base_vertex, min_vertex_index, vertex_count))
	{
		return m_impl->Fail("draw failure: vertex range exceeds buffer");
	}
	LegacyVertexLayout layout;
	if (!Build_Vertex_Layout(vertex_buffer->FVF_Info().Get_FVF(),
		vertex_buffer->FVF_Info().Get_FVF_Size(), &layout))
	{
		return m_impl->Fail("draw failure: unsupported legacy vertex layout");
	}
	GpuHandle vertex_handle;
	GpuHandle index_handle;
	if (!m_impl->Acquire_Vertex_Buffer(vertex_buffer, layout.stride, 0,
			base_vertex + min_vertex_index, vertex_count, &vertex_handle) ||
		!m_impl->Acquire_Index_Buffer(index_buffer, 0, start_index,
			index_count, &index_handle))
	{
		return false;
	}
	LegacyLogicalState state;
	if (!rts::render::GetTrackedLegacyLogicalState(&state))
	{
		return m_impl->Fail("draw failure: unavailable legacy logical state");
	}
	if (state.pipeline.rasterizer.frontCounterClockwise)
	{
		++m_impl->counter_clockwise_draw_count;
	}
	if (state.pipeline.rasterizer.cullMode == rts::render::RENDER_CULL_NONE)
	{
		++m_impl->unculled_draw_count;
	}
	if (state.pipeline.blend.colorWriteMask != 0)
	{
		++m_impl->color_write_draw_count;
	}
	if (state.pipeline.depthStencil.depthFunction ==
		rts::render::RENDER_COMPARE_NEVER)
	{
		++m_impl->depth_never_draw_count;
	}
	if (!m_impl->capture_draw_logged && m_impl->capture_queue.pendingCount() != 0)
	{
		D3DMATRIX actual_world;
		D3DMATRIX actual_view;
		D3DMATRIX actual_projection;
		float maximum_difference[3] = { 0.0f, 0.0f, 0.0f };
		if (m_impl->legacy_device != 0 &&
			SUCCEEDED(m_impl->legacy_device->GetTransform(D3DTS_WORLD,
				&actual_world)) && SUCCEEDED(m_impl->legacy_device->GetTransform(
				D3DTS_VIEW, &actual_view)) && SUCCEEDED(
				m_impl->legacy_device->GetTransform(D3DTS_PROJECTION,
					&actual_projection)))
		{
			const float *actual_matrices[] = { &actual_world.m[0][0],
				&actual_view.m[0][0], &actual_projection.m[0][0] };
			const float *tracked_matrices[] = { state.constants.world.values,
				state.constants.view.values, state.constants.projection.values };
			for (unsigned int matrix = 0; matrix < 3; ++matrix)
			{
				for (unsigned int component = 0; component < 16; ++component)
				{
					const float difference = static_cast<float>(fabs(
						actual_matrices[matrix][component] -
						tracked_matrices[matrix][component]));
					if (difference > maximum_difference[matrix])
					{
						maximum_difference[matrix] = difference;
					}
				}
			}
		}
		D3DVIEWPORT8 actual_viewport;
		memset(&actual_viewport, 0, sizeof(actual_viewport));
		if (m_impl->legacy_device != 0)
		{
			m_impl->legacy_device->GetViewport(&actual_viewport);
		}
		char state_message[320];
		snprintf(state_message, sizeof(state_message),
			"capture first draw fvf=0x%08x primitives=%u start=%u base=%u cull=%u ccw=%u color_mask=0x%x depth=%u matrix_diff=%g,%g,%g viewport=%u,%u,%u,%u,%g,%g",
			vertex_buffer->FVF_Info().Get_FVF(), primitive_count, start_index,
			base_vertex, static_cast<unsigned int>(
				state.pipeline.rasterizer.cullMode),
			state.pipeline.rasterizer.frontCounterClockwise ? 1U : 0U,
			state.pipeline.blend.colorWriteMask,
			static_cast<unsigned int>(state.pipeline.depthStencil.depthFunction),
			maximum_difference[0], maximum_difference[1], maximum_difference[2],
			actual_viewport.X, actual_viewport.Y,
			actual_viewport.Width, actual_viewport.Height, actual_viewport.MinZ,
			actual_viewport.MaxZ);
		m_impl->Log(state_message);
		m_impl->capture_draw_logged = true;
	}
	Impl::Draw_Texture_Scope texture_scope(*m_impl);
	unsigned int texture_mask = 0;
	for (unsigned int stage = 0;
		stage < rts::render::LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		GpuHandle texture_handle;
#if defined(_WIN64)
		TextureBaseClass *source =
			rts::render::GetPublishedTextureStage(stage);
		const bool bound = m_impl->Bind_Native_Texture(stage, source,
			&texture_handle);
#else
		// DX8Wrapper owns and maintains this shadow.  Reading it avoids the
		// eight per-draw GetTexture/AddRef/Release round trips and keeps the
		// D3D11 bridge from asking the legacy device for mutable state.
		IDirect3DBaseTexture8 *source =
			DX8Wrapper::Get_Tracked_DX8_Texture(stage);
		const bool bound = m_impl->Bind_Texture(stage, source, &texture_handle);
#endif
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
	rts::render::RenderPrimitiveTopology topology;
	if (!Try_Translate_D3D8_Primitive_Topology(primitive_type, &topology))
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"draw failure: unsupported indexed primitive topology");
	}
	const RenderResult topology_result = m_impl->context->setPrimitiveTopology(
		topology);
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
	++m_impl->frame_draw_count;
	if (m_impl->draw_count == 1)
	{
		m_impl->Log("first D3D11 legacy draw submitted");
	}
	return true;
}

bool D3D11LegacyBridge::Draw(IDirect3DVertexBuffer8 *vertex_buffer,
	unsigned int fvf, unsigned int vertex_stride,
	IDirect3DIndexBuffer8 *index_buffer, unsigned int primitive_type,
	unsigned int min_vertex_index, unsigned int vertex_count,
	unsigned int start_index, unsigned int primitive_count,
	unsigned int base_vertex)
{
	if (!Is_Active() || m_impl == 0 || !m_impl->frame_open ||
		vertex_buffer == 0 || index_buffer == 0)
	{
		return Is_Active() ? m_impl->Fail(
			"raw draw failure: invalid draw state") : false;
	}
	if (!Can_Submit_D3D11_Legacy_Draw())
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_FAILED,
			"raw draw failure: neutral texture stage state publication was rejected");
	}
	unsigned int index_count = 0;
	if (!Primitive_Index_Count(primitive_type, primitive_count, &index_count))
	{
		return m_impl->Fail("raw draw failure: indexed primitive count overflow or unsupported topology");
	}
	if (!rts::render::Is_D3D11_Base_Vertex_Valid(base_vertex))
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"raw draw failure: base vertex exceeds D3D11 range");
	}
	D3DVERTEXBUFFER_DESC vertex_desc;
	D3DINDEXBUFFER_DESC index_desc;
	memset(&vertex_desc, 0, sizeof(vertex_desc));
	memset(&index_desc, 0, sizeof(index_desc));
	if (FAILED(vertex_buffer->GetDesc(&vertex_desc)) ||
		vertex_desc.Size == 0)
	{
		return m_impl->Fail("raw draw failure: vertex buffer description");
	}
	if (FAILED(index_buffer->GetDesc(&index_desc)) || index_desc.Size == 0)
	{
		return m_impl->Fail("raw draw failure: index buffer description");
	}
	if (index_desc.Format != D3DFMT_INDEX16)
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"raw draw failure: index buffer is not 16-bit");
	}
	if (!rts::render::Is_Legacy_Indexed_Range_Valid(
		static_cast<unsigned long>(index_desc.Size / sizeof(unsigned short)),
		start_index, index_count))
	{
		return m_impl->Fail("raw draw failure: indexed range exceeds buffer");
	}
	if (vertex_stride == 0)
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"raw draw failure: zero vertex stride");
	}
	const unsigned int required_fvf_stride =
		rts::render::LegacyFvfVertexSize(fvf);
	if (required_fvf_stride == 0 || vertex_stride < required_fvf_stride)
	{
		return m_impl->Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"raw draw failure: vertex stride is smaller than its FVF");
	}
	LegacyVertexLayout layout;
	if (!Build_Vertex_Layout(fvf, vertex_stride, &layout))
	{
		return m_impl->Fail(
			"raw draw failure: unsupported legacy vertex layout");
	}
	const size_t index_start = static_cast<size_t>(start_index) *
		sizeof(unsigned short);
	const size_t index_bytes = static_cast<size_t>(index_count) *
		sizeof(unsigned short);
	if (index_start > static_cast<size_t>(index_desc.Size) ||
		index_bytes > static_cast<size_t>(index_desc.Size) - index_start)
	{
		return m_impl->Fail("raw draw failure: indexed range exceeds buffer");
	}
	const size_t first_vertex = static_cast<size_t>(base_vertex) +
		static_cast<size_t>(min_vertex_index);
	if (first_vertex > static_cast<size_t>(-1) - vertex_count ||
		(first_vertex + vertex_count) >
		(static_cast<size_t>(vertex_desc.Size) / vertex_stride))
	{
		return m_impl->Fail("raw draw failure: vertex range exceeds buffer");
	}
	GpuHandle vertex_handle;
	GpuHandle index_handle;
	size_t uploaded_vertex_bytes = 0;
	size_t uploaded_index_bytes = 0;
	if (!m_impl->Upload_Raw_Vertex_Buffer(vertex_buffer, &vertex_handle,
		&uploaded_vertex_bytes) ||
		!m_impl->Upload_Raw_Index_Buffer(index_buffer, &index_handle,
		&uploaded_index_bytes))
	{
		return false;
	}
	(void)uploaded_vertex_bytes;
	(void)uploaded_index_bytes;
	return m_impl->Submit_Indexed_Draw(vertex_handle, index_handle, layout,
		fvf, primitive_type, index_count, start_index, primitive_count,
		base_vertex, true);
}

unsigned int D3D11LegacyBridge::Get_Raw_Indexed_Draw_Count() const
{
	return m_impl == 0 ? 0 : m_impl->raw_indexed_draw_count;
}

void D3D11LegacyBridge::Get_Cache_Stats(
	D3D11LegacyBridgeCacheStats *stats) const
{
	if (stats == 0)
	{
		return;
	}
	memset(stats, 0, sizeof(*stats));
	if (m_impl == 0)
	{
		return;
	}
	stats->bufferLookups = m_impl->cache_counters.bufferLookups;
	stats->bufferHits = m_impl->cache_counters.bufferHits;
	stats->textureLookups = m_impl->cache_counters.textureLookups;
	stats->textureHits = m_impl->cache_counters.textureHits;
	stats->bufferUploads = m_impl->cache_counters.bufferUploads;
	stats->textureRefreshes = m_impl->cache_counters.textureRefreshes;
}

RenderResult D3D11LegacyBridge::Draw_Primitive_UP(
	unsigned int fvf, unsigned int primitive_type, unsigned int primitive_count,
	const void *vertex_data, unsigned int vertex_stride)
{
	if (!Is_Active() || m_impl == 0 || !m_impl->frame_open)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->Require_Owner_Thread("DrawPrimitiveUP");
	if (!Can_Submit_D3D11_Legacy_Draw())
	{
		m_impl->Fail(rts::render::RENDER_RESULT_FAILED,
			"draw failure: neutral texture stage state publication was rejected");
		return rts::render::RENDER_RESULT_FAILED;
	}
	if (primitive_count == 0)
	{
		return rts::render::RENDER_RESULT_OK;
	}
	if (vertex_data == 0 || vertex_stride == 0)
	{
		m_impl->Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"draw failure: invalid DrawPrimitiveUP arguments");
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const unsigned int required_fvf_stride =
		rts::render::LegacyFvfVertexSize(fvf);
	if (required_fvf_stride == 0 || vertex_stride < required_fvf_stride)
	{
		m_impl->Fail(rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"draw failure: DrawPrimitiveUP stride is smaller than its FVF");
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	size_t vertex_count = 0;
	if (!Primitive_Up_Vertex_Count(primitive_type, primitive_count,
		&vertex_count) || vertex_count > UINT_MAX)
	{
		m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"draw failure: unsupported DrawPrimitiveUP topology or count");
		return rts::render::RENDER_RESULT_UNSUPPORTED;
	}
	size_t byte_count = 0;
	if (!Checked_Multiply(vertex_count, static_cast<size_t>(vertex_stride),
		&byte_count) || byte_count > PRIMITIVE_UP_BUFFER_CAPACITY)
	{
		m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"draw failure: DrawPrimitiveUP byte budget exceeded");
		return rts::render::RENDER_RESULT_UNSUPPORTED;
	}
	LegacyVertexLayout layout;
	if (!Build_Vertex_Layout(fvf, vertex_stride, &layout))
	{
		m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"draw failure: unsupported DrawPrimitiveUP FVF");
		return rts::render::RENDER_RESULT_UNSUPPORTED;
	}
	GpuHandle vertex_handle;
	if (!m_impl->Upload_Primitive_Up(vertex_data, byte_count,
		&vertex_handle))
	{
		return rts::render::RENDER_RESULT_FAILED;
	}
	LegacyLogicalState state;
	if (!rts::render::GetTrackedLegacyLogicalState(&state))
	{
		m_impl->Fail("draw failure: unavailable legacy logical state");
		return rts::render::RENDER_RESULT_FAILED;
	}
	Impl::Draw_Texture_Scope texture_scope(*m_impl);
	unsigned int texture_mask = 0;
	for (unsigned int stage = 0;
		stage < rts::render::LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		GpuHandle texture_handle;
#if defined(_WIN64)
		TextureBaseClass *source =
			rts::render::GetPublishedTextureStage(stage);
		if (!m_impl->Bind_Native_Texture(stage, source, &texture_handle))
#else
		IDirect3DBaseTexture8 *source =
			DX8Wrapper::Get_Tracked_DX8_Texture(stage);
		if (!m_impl->Bind_Texture(stage, source, &texture_handle))
#endif
		{
			m_impl->Fail("draw failure: DrawPrimitiveUP texture binding");
			return rts::render::RENDER_RESULT_FAILED;
		}
		if (texture_handle.isValid())
		{
			texture_mask |= 1U << stage;
		}
	}
	const RenderResult state_result = m_impl->context->setLegacyStateForLayout(
		state, layout, texture_mask);
	if (state_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Fail(state_result,
			"draw failure: DrawPrimitiveUP state/layout binding");
		return state_result;
	}
	const RenderResult vertex_bind_result = m_impl->context->setVertexBuffer(
		vertex_handle, vertex_stride, 0);
	if (vertex_bind_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Fail(vertex_bind_result,
			"draw failure: DrawPrimitiveUP vertex binding");
		return vertex_bind_result;
	}
	rts::render::RenderPrimitiveTopology topology;
	if (!Try_Translate_D3D8_Primitive_Topology(primitive_type, &topology))
	{
		m_impl->Fail(rts::render::RENDER_RESULT_UNSUPPORTED,
			"draw failure: unsupported DrawPrimitiveUP topology");
		return rts::render::RENDER_RESULT_UNSUPPORTED;
	}
	const RenderResult topology_result =
		m_impl->context->setPrimitiveTopology(topology);
	if (topology_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Fail(topology_result,
			"draw failure: DrawPrimitiveUP topology binding");
		return topology_result;
	}
	const RenderResult draw_result = m_impl->context->draw(
		static_cast<unsigned int>(vertex_count), 0);
	if (draw_result != rts::render::RENDER_RESULT_OK)
	{
		m_impl->Fail(draw_result, "draw failure: DrawPrimitiveUP submission");
		return draw_result;
	}
	++m_impl->draw_count;
	++m_impl->frame_draw_count;
	if (m_impl->draw_count == 1)
	{
		m_impl->Log("first D3D11 legacy DrawPrimitiveUP submitted");
	}
	return rts::render::RENDER_RESULT_OK;
}

RenderResult D3D11LegacyBridge::Resize(unsigned int width, unsigned int height)
{
	bool native_buffer_publication_failed = false;
#if defined(_WIN64)
	if (m_impl != 0 && m_impl->device != 0)
	{
		m_impl->Service_Render_Completions();
		if (m_impl->Is_Threaded())
		{
			if (m_impl->frame_open)
			{
				m_impl->Cancel_Threaded_Frame(rts::render::RENDER_RESULT_FAILED);
				m_impl->frame_open = false;
			}
			m_impl->Fence_Render();
		}
	}
#endif
	if (!Is_Active())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->capture_queue.advanceGeneration();
	m_impl->capture_queue.cancelStale(rts::render::RENDER_RESULT_FAILED);
	m_impl->Invalidate_GPU_Copy_Content();
	RenderResult result = m_impl->device->resize(width, height);
	if (result == rts::render::RENDER_RESULT_DEVICE_REMOVED)
	{
		// ResizeBuffers can be the first call to report a removed, reset, or
		// hung device.  The render device normally recovers and retries this
		// operation itself; keep this bridge-level retry for failures surfaced
		// by the second attempt (or by a backend implementation that reports
		// the removal without doing its own recovery).  Recover_Device preserves
		// the bridge's logical target binding and the renderer's generation-safe
		// handles before the requested size is applied again.
		const RenderResult recovery_result = m_impl->Recover_Device();
		if (recovery_result == rts::render::RENDER_RESULT_OK)
		{
			m_impl->context = m_impl->device->immediateContext();
			if (m_impl->context != 0)
			{
				result = m_impl->device->resize(width, height);
			}
			else
			{
				result = rts::render::RENDER_RESULT_FAILED;
			}
		}
		else
		{
			result = recovery_result;
		}
	}
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
	else if (width != 0 && height != 0)
	{
		IRenderContext *resized_context = m_impl->device->immediateContext();
		if (resized_context == 0)
		{
			result = rts::render::RENDER_RESULT_FAILED;
			m_impl->Log("D3D11 immediate context is unavailable after resize");
		}
		else
		{
			m_impl->context = resized_context;
			result = m_impl->native_w3d == 0 ?
				rts::render::RENDER_RESULT_INVALID_ARGUMENT :
				m_impl->native_w3d->ReplaceBackendContext(resized_context);
			if (result == rts::render::RENDER_RESULT_OK)
			{
				result = m_impl->native_w3d->Resources().
					RepublishStaticBuffersAfterResize();
			}
			native_buffer_publication_failed =
				result != rts::render::RENDER_RESULT_OK;
			if (result != rts::render::RENDER_RESULT_OK)
			{
				m_impl->Log_Result(
					"D3D11 native WW3D resize publication failed",
					result);
			}
		}
	}
	if (native_buffer_publication_failed &&
		m_impl != 0 && m_impl->device != 0 &&
		m_impl->context != 0 && m_impl->device->isOperational())
	{
		// Resize may have recovered the native device internally. Once static
		// geometry cannot be republished, no later draw may use that backend.
		Shutdown();
	}
	if (result == rts::render::RENDER_RESULT_OK && width != 0 && height != 0)
	{
		// D3D11RenderDevice deliberately treats a minimized 0x0 resize as a
		// successful no-op so its last valid swap-chain targets survive.  Mirror
		// that contract here instead of replacing the bridge's usable dimensions
		// with zero and making the later restore path appear invalid.
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
rts::render::RenderResult D3D11LegacyBridge::Shutdown_Result()
{
	return rts::render::RENDER_RESULT_OK;
}
void D3D11LegacyBridge::Shutdown() {}
bool D3D11LegacyBridge::Prepare_Legacy_Device_Reset() { return true; }
bool D3D11LegacyBridge::Is_Active() const { return false; }
void D3D11LegacyBridge::Begin_Display_Iteration() {}
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
bool D3D11LegacyBridge::Is_Render_Target_Operational() const { return false; }
void D3D11LegacyBridge::Record_Visible_Submission_Failure() {}
rts::render::RenderResult D3D11LegacyBridge::Set_Render_Target_Surfaces(
	IDirect3DSurface8 *, IDirect3DSurface8 *, bool)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
rts::render::RenderResult D3D11LegacyBridge::Set_Render_Target_Default()
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
#if defined(_WIN64)
rts::render::RenderResult D3D11LegacyBridge::Set_Render_Target_Textures(
	TextureBaseClass *, TextureBaseClass *, bool)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
#endif
rts::render::RenderResult D3D11LegacyBridge::Copy_Active_Color_Target_To_Texture(
	IDirect3DBaseTexture8 *)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
bool D3D11LegacyBridge::Acquire_Copied_Texture_Content(
	IDirect3DBaseTexture8 *) { return false; }
void D3D11LegacyBridge::Invalidate_Buffer(IUnknown *) {}
void D3D11LegacyBridge::Invalidate_Buffer_Range(IUnknown *, unsigned int,
	size_t, size_t, rts::render::RenderBufferUpdateMode) {}
bool D3D11LegacyBridge::Publish_Buffer_Change(IUnknown *, unsigned int,
	const void *, size_t, size_t,
	rts::render::RenderBufferUpdateMode, unsigned int) { return false; }
bool D3D11LegacyBridge::Publish_Texture_BGRA8_Change(
	IDirect3DBaseTexture8 *, const void *, size_t, size_t) { return false; }
void D3D11LegacyBridge::Invalidate_Texture(IDirect3DBaseTexture8 *) {}
bool D3D11LegacyBridge::Draw(VertexBufferClass *, IndexBufferClass *,
	unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
	unsigned int) { return false; }
bool D3D11LegacyBridge::Draw(IDirect3DVertexBuffer8 *, unsigned int,
	unsigned int, IDirect3DIndexBuffer8 *, unsigned int, unsigned int,
	unsigned int, unsigned int, unsigned int, unsigned int) { return false; }
unsigned int D3D11LegacyBridge::Get_Raw_Indexed_Draw_Count() const
{
	return 0;
}
void D3D11LegacyBridge::Get_Cache_Stats(
	D3D11LegacyBridgeCacheStats *stats) const
{
	if (stats != 0)
	{
		stats->bufferLookups = 0;
		stats->bufferHits = 0;
		stats->textureLookups = 0;
		stats->textureHits = 0;
		stats->bufferUploads = 0;
		stats->textureRefreshes = 0;
	}
}
rts::render::RenderResult D3D11LegacyBridge::Draw_Primitive_UP(
	unsigned int, unsigned int, unsigned int, const void *, unsigned int)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
rts::render::RenderResult D3D11LegacyBridge::Resize(unsigned int, unsigned int)
{
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}
#endif
