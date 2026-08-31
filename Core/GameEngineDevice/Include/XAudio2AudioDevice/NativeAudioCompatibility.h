#pragma once

#include "Lib/BaseType.h"

#include <cstddef>

namespace NativeAudioCompatibility
{
constexpr std::size_t MAX_BUFFERED_PCM_BYTES = 64U * 1024U * 1024U;

constexpr Bool canBufferPcm(UnsignedInt frameCount, UnsignedShort channels)
{
	const std::size_t bytesPerFrame = static_cast<std::size_t>(channels) * sizeof(Short);
	return bytesPerFrame != 0
		&& frameCount <= MAX_BUFFERED_PCM_BYTES / bytesPerFrame;
}

constexpr Bool canReservePcm(std::size_t bytesInUse, UnsignedInt frameCount,
	UnsignedShort channels)
{
	const std::size_t bytesPerFrame = static_cast<std::size_t>(channels) * sizeof(Short);
	return bytesInUse <= MAX_BUFFERED_PCM_BYTES
		&& canBufferPcm(frameCount, channels)
		&& static_cast<std::size_t>(frameCount) * bytesPerFrame
			<= MAX_BUFFERED_PCM_BYTES - bytesInUse;
}
}
