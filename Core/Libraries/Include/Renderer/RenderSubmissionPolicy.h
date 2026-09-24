#ifndef RTS_RENDERER_RENDERSUBMISSIONPOLICY_H
#define RTS_RENDERER_RENDERSUBMISSIONPOLICY_H

namespace rts
{
namespace render
{

// Visible work normally has exactly one owner.  A D3D11 device can be active
// while its target mapping is temporarily unavailable (for example during a
// render-target transition or recovery).  The legacy backend is only a
// visible owner when D3D11 is inactive.  When D3D11 remains active but its
// target is unavailable, visible work is suppressed and the frame failure is
// recorded; submitting to the hidden legacy swap chain would produce a frame
// that is never presented.
enum RenderSubmissionBackend
{
	RENDER_SUBMISSION_LEGACY,
	RENDER_SUBMISSION_D3D11,
	RENDER_SUBMISSION_UNAVAILABLE
};

struct RenderSubmissionDecision
{
	RenderSubmissionDecision() :
		backend(RENDER_SUBMISSION_LEGACY),
		submitLegacy(true),
		submitD3D11(false),
		d3d11FailureIsTerminal(false)
	{
	}

	RenderSubmissionBackend backend;
	bool submitLegacy;
	bool submitD3D11;
	bool d3d11FailureIsTerminal;
};

inline RenderSubmissionDecision ChooseVisibleSubmissionBackend(
	bool d3d11Active, bool d3d11TargetOperational)
{
	RenderSubmissionDecision decision;
	if (d3d11Active && d3d11TargetOperational)
	{
		decision.backend = RENDER_SUBMISSION_D3D11;
		decision.submitLegacy = false;
		decision.submitD3D11 = true;
		// A failed D3D11 command is recorded by the frame outcome.  Do not hide
		// it by drawing the same visible work through the legacy backend.
		decision.d3d11FailureIsTerminal = true;
	}
	else if (d3d11Active)
	{
		decision.backend = RENDER_SUBMISSION_UNAVAILABLE;
		decision.submitLegacy = false;
		decision.submitD3D11 = false;
		decision.d3d11FailureIsTerminal = true;
	}
	return decision;
}

// Presentation ownership is latched at the beginning of the scene.  If D3D11
// owned that scene, a later bridge shutdown must not make the hidden compatibility
// differential swap chain visible for the same frame.
inline bool ShouldPresentLegacyFrame(bool d3d11FrameStarted,
	bool d3d11BridgeActive)
{
	return !d3d11FrameStarted && !d3d11BridgeActive;
}

// The pre-transformed terrain path submits raw compatibility ProcessVertices work and
// is only valid when the legacy backend owns the visible frame. D3D11 must
// use the normal vertex-buffer path so the bridge can track every binding.
inline bool UseLegacyPreTransformVertexPath(bool d3d11BackendActive)
{
	return !d3d11BackendActive;
}

// A capture request is armed immediately before End_Render so the bridge can
// queue it for that same frame. Completion is acknowledged only after the
// visible frame boundary advances; a failed/dropped frame keeps the request
// pending for the next completed frame.
class RenderCaptureFrameGate
{
public:
	RenderCaptureFrameGate() : m_requested(false), m_armed(false) {}

	void request()
	{
		m_requested = true;
	}

	void clear()
	{
		m_requested = false;
		m_armed = false;
	}

	bool arm(bool d3d11BackendActive)
	{
		if (!d3d11BackendActive || !m_requested || m_armed)
		{
			return false;
		}
		m_armed = true;
		return true;
	}

	bool complete(bool successfullyCompleted)
	{
		if (!m_armed)
		{
			return false;
		}
		m_armed = false;
		if (!successfullyCompleted)
		{
			return false;
		}
		m_requested = false;
		return true;
	}

	bool isRequested() const
	{
		return m_requested;
	}

	bool isArmed() const
	{
		return m_armed;
	}

private:
	bool m_requested;
	bool m_armed;
};

} // namespace render
} // namespace rts

#endif
