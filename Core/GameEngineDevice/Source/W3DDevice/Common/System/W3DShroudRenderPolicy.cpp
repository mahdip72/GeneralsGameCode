#include "W3DDevice/Common/W3DShroudRenderPolicy.h"

namespace rts
{
	namespace render
	{
		W3DShroudDestinationUpdateDecision EvaluateW3DShroudDestinationUpdate(
			bool sourceDirtyBeforeSync,
			bool sourceDirtyAfterInterpolation,
			bool destinationBorderDirty)
		{
			W3DShroudDestinationUpdateDecision decision;
			decision.copySource = sourceDirtyBeforeSync || sourceDirtyAfterInterpolation;
			decision.notifyTexture = decision.copySource || destinationBorderDirty;
			return decision;
		}
	}
}
