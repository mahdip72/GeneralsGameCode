#pragma once

#include "Lib/BaseTypeCore.h"

/* Owner-captured values only. Neither a particle nor a renderer is borrowed. */
struct ParticleRenderInput
{
	Real x, y, z, size;
	Real red, green, blue, alpha, angle;
	unsigned personality;
};

struct ParticleRenderOutput
{
	unsigned visible;
	unsigned angle;
};

struct ParticleRenderBounds
{
	Real centerX, centerY, centerZ;
	Real extentX, extentY, extentZ;
};

enum { PARTICLE_RENDER_MAX_COUNT = 65536 };

/* No compaction here: the owner publishes visible results in source order and
 * applies the legacy per-system cap only after the joined preparation. */
bool PrepareParticleRenderRange(const ParticleRenderInput *input,
	ParticleRenderOutput *output, unsigned count,
	const ParticleRenderBounds &bounds, unsigned begin, unsigned end);
