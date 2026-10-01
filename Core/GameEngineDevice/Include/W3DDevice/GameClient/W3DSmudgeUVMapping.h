#ifndef W3DSMUDGEUVMAPPING_H
#define W3DSMUDGEUVMAPPING_H

// The active target is copied at the origin of the display-sized background.
// Camera viewport coordinates are normalized to the active target, not to the
// display. Keep this mapping shared with the production copy regression.
struct W3DSmudgeUVAxis
{
	W3DSmudgeUVAxis(float viewportMin, float viewportMax,
		unsigned int targetSize, unsigned int backgroundSize)
	{
		const float targetScale = (float)targetSize / (float)backgroundSize;
		origin = viewportMin * targetScale;
		span = (viewportMax - viewportMin) * targetScale;
		minimum = 0.5f / (float)backgroundSize;
		maximum = ((float)targetSize - 0.5f) / (float)backgroundSize;
		partial = targetSize < backgroundSize;
	}

	float Project(float clipCoordinate) const
	{
		return origin + (clipCoordinate + 1.0f) * span * 0.5f;
	}

	bool Outside(float coordinate) const
	{
		return coordinate < origin || coordinate > origin + span;
	}

	float Constrain(float coordinate) const
	{
		// The hardware clamps to the full background. A partial copy instead
		// needs texel-center bounds to avoid filtering pixels outside its region.
		if (partial && coordinate < minimum) return minimum;
		if (partial && coordinate > maximum) return maximum;
		return coordinate;
	}

	float origin, span, minimum, maximum;
	bool partial;
};

#endif
