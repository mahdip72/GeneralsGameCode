/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef PROFILER_ENABLED

#include "../../../Include/W3DDevice/GameClient/W3DProfilerFrameCapture.h"

#include "Renderer/RenderGameClient.h"

// Keep the source-level contract explicit without importing the renderer namespace.
using rts::render::RENDER_CAPTURE_PROFILER;
using rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
using rts::render::RENDER_RESULT_FAILED;
using rts::render::RENDER_RESULT_OK;

#include "WW3D2/ww3d.h"
#include "WWDebug/wwdebug.h"
#include <limits>

namespace
{
bool Checked_Profiler_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right >
		std::numeric_limits<size_t>::max() / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}
}

W3DProfilerFrameCapture::W3DProfilerFrameCapture()
{
}

W3DProfilerFrameCapture::~W3DProfilerFrameCapture()
{
	WWASSERT(!m_d3d11CapturePending);
}

bool W3DProfilerFrameCapture::ShouldReuseLastCapture(UnsignedInt currentTimeMs) const
{
	return PROFILER_FRAME_IMAGE_INTERVAL_MS > 0
		&& currentTimeMs - m_lastCaptureTimeMs < PROFILER_FRAME_IMAGE_INTERVAL_MS
		&& !m_lastCapturePixels.empty();
}

bool W3DProfilerFrameCapture::Shutdown_D3D11_Capture()
{
	if (!m_d3d11CapturePending)
	{
		return true;
	}
	const unsigned int cancelled =
		rts::render::CancelGameBackBufferCaptures(this,
			rts::render::RENDER_RESULT_FAILED);
	if (cancelled == 0 && !rts::render::IsNativeGameRendererActive())
	{
		// Once the bridge is inactive there is no queue left that can invoke
		// this callback target. Clear a stale pending bit so deferred teardown
		// can reclaim the consumer safely.
		m_d3d11CapturePending = false;
		return true;
	}
	return cancelled != 0 && !m_d3d11CapturePending;
}

void W3DProfilerFrameCapture::Complete_D3D11_Capture(void *consumer,
	const rts::render::RenderCaptureHandle *, unsigned int width,
	unsigned int height, size_t rowPitch, rts::render::RenderFormat format,
	const void *pixels, size_t pixelBytes)
{
	W3DProfilerFrameCapture *capture =
		static_cast<W3DProfilerFrameCapture *>(consumer);
	if (capture == 0)
	{
		return;
	}
	capture->m_d3d11CapturePending = false;
	const bool sourceIsRGBA =
		format == rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
	const bool sourceIsBGRA =
		format == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
	if (pixels == 0 || width == 0 || height == 0 ||
		(!sourceIsRGBA && !sourceIsBGRA) ||
		PROFILER_FRAME_IMAGE_SIZE == 0)
	{
		WWDEBUG_SAY(("D3D11 profiler capture returned invalid frame data"));
		return;
	}
	size_t sourceRowBytes = 0;
	size_t sourceBytes = 0;
	if (!Checked_Profiler_Multiply(static_cast<size_t>(width), 4,
		&sourceRowBytes) || rowPitch < sourceRowBytes ||
		!Checked_Profiler_Multiply(rowPitch, static_cast<size_t>(height),
		&sourceBytes) || pixelBytes < sourceBytes)
	{
		WWDEBUG_SAY(("D3D11 profiler capture returned invalid row bounds"));
		return;
	}
	const double scaledHeight = static_cast<double>(PROFILER_FRAME_IMAGE_SIZE) *
		static_cast<double>(height) / static_cast<double>(width);
	unsigned int outputHeight = 1;
	if (scaledHeight >= static_cast<double>(PROFILER_FRAME_IMAGE_SIZE))
	{
		outputHeight = PROFILER_FRAME_IMAGE_SIZE;
	}
	else if (scaledHeight > 1.0)
	{
		outputHeight = static_cast<unsigned int>(scaledHeight + 0.5);
	}
	if (outputHeight == 0)
	{
		outputHeight = 1;
	}
	if (outputHeight > PROFILER_FRAME_IMAGE_SIZE)
	{
		outputHeight = PROFILER_FRAME_IMAGE_SIZE;
	}
	size_t outputRowBytes = 0;
	size_t outputBytes = 0;
	if (!Checked_Profiler_Multiply(PROFILER_FRAME_IMAGE_SIZE, 4,
		&outputRowBytes) || !Checked_Profiler_Multiply(outputRowBytes,
		static_cast<size_t>(outputHeight), &outputBytes))
	{
		WWDEBUG_SAY(("D3D11 profiler capture output dimensions overflowed"));
		return;
	}
	try
	{
		capture->m_lastCapturePixels.resize(outputBytes);
	}
	catch (...)
	{
		WWDEBUG_SAY(("D3D11 profiler capture allocation failed"));
		return;
	}
	const unsigned char *source = static_cast<const unsigned char *>(pixels);
	UnsignedByte *destination = capture->m_lastCapturePixels.data();
	for (unsigned int y = 0; y < outputHeight; ++y)
	{
		const unsigned int sourceY = static_cast<unsigned int>(
			static_cast<size_t>(y) * height / outputHeight);
		const unsigned char *sourceRow = source +
			static_cast<size_t>(sourceY) * rowPitch;
		for (unsigned int x = 0; x < PROFILER_FRAME_IMAGE_SIZE; ++x)
		{
			const unsigned int sourceX = static_cast<unsigned int>(
				static_cast<size_t>(x) * width / PROFILER_FRAME_IMAGE_SIZE);
			const unsigned char *sourcePixel = sourceRow +
				static_cast<size_t>(sourceX) * 4;
			UnsignedByte *destinationPixel = destination +
				static_cast<size_t>(y) * outputRowBytes +
				static_cast<size_t>(x) * 4;
			if (sourceIsBGRA)
			{
				destinationPixel[0] = sourcePixel[2];
				destinationPixel[1] = sourcePixel[1];
				destinationPixel[2] = sourcePixel[0];
			}
			else
			{
				destinationPixel[0] = sourcePixel[0];
				destinationPixel[1] = sourcePixel[1];
				destinationPixel[2] = sourcePixel[2];
			}
			destinationPixel[3] = sourcePixel[3];
		}
	}
	capture->m_lastCaptureHeight = outputHeight;
	capture->m_lastCaptureTimeMs = capture->m_d3d11PendingTimeMs;
	PROFILER_FRAME_IMAGE(capture->m_lastCapturePixels.data(),
		PROFILER_FRAME_IMAGE_SIZE, capture->m_lastCaptureHeight, 0, false);
}

void W3DProfilerFrameCapture::Cancel_D3D11_Capture(void *consumer,
	const rts::render::RenderCaptureHandle *, rts::render::RenderResult reason)
{
	W3DProfilerFrameCapture *capture =
		static_cast<W3DProfilerFrameCapture *>(consumer);
	if (capture != 0)
	{
		capture->m_d3d11CapturePending = false;
		WWDEBUG_SAY(("D3D11 profiler capture cancelled: %d",
			static_cast<int>(reason)));
	}
}

void W3DProfilerFrameCapture::Capture(UnsignedInt displayWidth, UnsignedInt displayHeight)
{
	if (!PROFILER_IS_CONNECTED)
		return;

	// the profiler expects an image every render frame. resend the last capture if we're inside the capture interval.
	const UnsignedInt currentTimeMs = WW3D::Get_Logic_Time_Milliseconds();
	if (ShouldReuseLastCapture(currentTimeMs))
	{
		PROFILER_FRAME_IMAGE(m_lastCapturePixels.data(), PROFILER_FRAME_IMAGE_SIZE, m_lastCaptureHeight, 0, false);
		return;
	}

	if (rts::render::IsNativeGameRendererActive())
	{
		if (m_d3d11CapturePending)
		{
			return;
		}
		rts::render::RenderBackBufferInfo backBufferInfo;
		const rts::render::RenderResult infoResult =
			rts::render::GetGameBackBufferInfo(&backBufferInfo);
		if (infoResult != rts::render::RENDER_RESULT_OK ||
			backBufferInfo.width == 0 || backBufferInfo.height == 0 ||
			(backBufferInfo.format != rts::render::RENDER_FORMAT_R8G8B8A8_UNORM &&
			 backBufferInfo.format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM))
		{
			WWDEBUG_SAY(("D3D11 profiler capture has no supported back buffer: %d",
				static_cast<int>(infoResult)));
			return;
		}
		rts::render::RenderCaptureRequestDescriptor descriptor;
		descriptor.kind = rts::render::RENDER_CAPTURE_PROFILER;
		descriptor.consumer = this;
		descriptor.completed = Complete_D3D11_Capture;
		descriptor.cancelled = Cancel_D3D11_Capture;
		m_d3d11PendingTimeMs = currentTimeMs;
		m_d3d11CapturePending = true;
		rts::render::RenderCaptureHandle handle;
		const rts::render::RenderResult queueResult =
			rts::render::QueueGameBackBufferCapture(descriptor, &handle);
		if (queueResult != rts::render::RENDER_RESULT_OK)
		{
			m_d3d11CapturePending = false;
			WWDEBUG_SAY(("D3D11 profiler capture queue rejected: %d",
				static_cast<int>(queueResult)));
		}
		return;
	}
	// The native capture queue is the only supported frame-capture path.  A
	// legacy immediate readback cannot be made truthful on x64 because it
	// requires backend-owned surfaces and device state.
	(void)displayWidth;
	(void)displayHeight;
}

#endif // PROFILER_ENABLED
