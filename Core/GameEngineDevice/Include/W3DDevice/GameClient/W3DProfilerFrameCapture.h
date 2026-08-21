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

#pragma once

#ifdef PROFILER_ENABLED

#include "Lib/BaseType.h"
#include "Renderer/RendererDevice.h"
#include <vector>

class W3DProfilerFrameCapture
{
public:
	W3DProfilerFrameCapture();
	~W3DProfilerFrameCapture();

	void Capture(UnsignedInt displayWidth, UnsignedInt displayHeight);

private:
	bool ShouldReuseLastCapture(UnsignedInt currentTimeMs) const;
	static void Complete_D3D11_Capture(void *consumer,
		const rts::render::RenderCaptureHandle *handle, unsigned int width,
		unsigned int height, size_t rowPitch,
		rts::render::RenderFormat format, const void *pixels,
		size_t pixelBytes);
	static void Cancel_D3D11_Capture(void *consumer,
		const rts::render::RenderCaptureHandle *handle,
		rts::render::RenderResult reason);

	DWORD m_swizzleShader = 0;
	UnsignedInt m_lastCaptureTimeMs = 0;
	UnsignedInt m_lastCaptureHeight = 0;
	std::vector<UnsignedByte> m_lastCapturePixels;
	bool m_d3d11CapturePending = false;
	UnsignedInt m_d3d11PendingTimeMs = 0;
};

#endif // PROFILER_ENABLED
