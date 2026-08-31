#ifndef RTS_RENDERER_LEGACYASYNCFRAMEPOLICY_H
#define RTS_RENDERER_LEGACYASYNCFRAMEPOLICY_H

#include "Renderer/RendererDevice.h"

namespace rts
{
namespace render
{

// The bridge retains one pending failure, not an unbounded completion history.
// Command-only drops and capture warnings can legitimately map to legacy
// success, so neither may hide an end/present/recovery failure from that caller.
inline unsigned int LegacyAsyncFrameFailurePriority(const RenderFrameOutcome &outcome)
{
	if (outcome.hasDeviceRemoval()) return 4;
	if (outcome.endFrameResult() != RENDER_RESULT_OK ||
		outcome.presentationResult() != RENDER_RESULT_OK ||
		outcome.recoveryResult() != RENDER_RESULT_OK) return 3;
	if (outcome.hasCommandFailure()) return 2;
	if (outcome.captureResult() != RENDER_RESULT_OK) return 1;
	return 0;
}

inline bool ShouldReplaceLegacyAsyncFrameFailure(const RenderFrameOutcome &pending,
	const RenderFrameOutcome &completed, bool pendingRemovalRecovered = false)
{
	// Retain the first equally severe failure, including its recovery-attempt
	// identity. A fresh removal after that recovery must rearm recovery instead
	// of disappearing when the already recovered failure is finally reported.
	if (pendingRemovalRecovered && pending.hasDeviceRemoval() &&
		completed.hasDeviceRemoval()) return true;
	return LegacyAsyncFrameFailurePriority(completed) >
		LegacyAsyncFrameFailurePriority(pending);
}

}
}
#endif
