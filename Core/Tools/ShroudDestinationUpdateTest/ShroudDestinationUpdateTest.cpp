#include <stdio.h>

#include "W3DDevice/Common/W3DShroudRenderPolicy.h"

namespace
{
int check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}
}

int main()
{
	int result = 0;

	const rts::render::W3DShroudDestinationUpdateDecision steadyState =
		rts::render::EvaluateW3DShroudDestinationUpdate(false, false, false);
	result |= check(!steadyState.copySource && !steadyState.notifyTexture,
		"steady-state shroud render skips destination copy and notification");

	const rts::render::W3DShroudDestinationUpdateDecision sourceDirty =
		rts::render::EvaluateW3DShroudDestinationUpdate(true, false, false);
	result |= check(sourceDirty.copySource && sourceDirty.notifyTexture,
		"source changes copy and notify the destination texture");

	const rts::render::W3DShroudDestinationUpdateDecision fogInterpolated =
		rts::render::EvaluateW3DShroudDestinationUpdate(false, true, false);
	result |= check(fogInterpolated.copySource && fogInterpolated.notifyTexture,
		"fog interpolation changes copy and notify the destination texture");

	const rts::render::W3DShroudDestinationUpdateDecision borderDirty =
		rts::render::EvaluateW3DShroudDestinationUpdate(false, false, true);
	result |= check(!borderDirty.copySource && borderDirty.notifyTexture,
		"a border reset notifies without repeating the full source copy");

	return result;
}
