#include "Lib/ParticleRenderKernel.h"

#include <float.h>
#include <stddef.h>
#include <string.h>

/* WWMath::Fabs clears the sign bit of its float argument. Preserve that
 * single-precision boundary on x87 too, without legacy type-punning. */
static Real particleRenderAbs(Real value)
{
	unsigned bits;
	memcpy(&bits, &value, sizeof(bits));
	bits &= 0x7fffffffu;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static bool particleRenderFinite(Real value)
{
	return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

bool PrepareParticleRenderRange(const ParticleRenderInput *input,
	ParticleRenderOutput *output, unsigned count,
	const ParticleRenderBounds &bounds, unsigned begin, unsigned end)
{
	if (begin > end || end > count || count > PARTICLE_RENDER_MAX_COUNT)
		return false;
	if (count == 0)
		return true;
	const size_t inputBegin = reinterpret_cast<size_t>(input);
	const size_t outputBegin = reinterpret_cast<size_t>(output);
	const size_t boundsBegin = reinterpret_cast<size_t>(&bounds);
	const size_t inputBytes = count * sizeof(ParticleRenderInput);
	const size_t outputBytes = count * sizeof(ParticleRenderOutput);
	const size_t maximum = static_cast<size_t>(-1);
	if (input == 0 || output == 0 || inputBegin % sizeof(Real) != 0 ||
		outputBegin % sizeof(unsigned) != 0 || inputBegin > maximum-inputBytes ||
		outputBegin > maximum-outputBytes || boundsBegin > maximum-sizeof(bounds) ||
		(inputBegin < outputBegin+outputBytes && outputBegin < inputBegin+inputBytes) ||
		(boundsBegin < outputBegin+outputBytes && outputBegin < boundsBegin+sizeof(bounds)))
		return false;
	if (!particleRenderFinite(bounds.centerX) || !particleRenderFinite(bounds.centerY) ||
		!particleRenderFinite(bounds.centerZ) || !particleRenderFinite(bounds.extentX) ||
		!particleRenderFinite(bounds.extentY) || !particleRenderFinite(bounds.extentZ))
		return false;
	unsigned index;
	/* Validate the entire assigned stripe before writing any part of it. */
	for (index = begin; index < end; ++index)
	{
		const ParticleRenderInput &particle = input[index];
		if (!particleRenderFinite(particle.x) || !particleRenderFinite(particle.y) ||
			!particleRenderFinite(particle.z) || !particleRenderFinite(particle.size) ||
			!particleRenderFinite(particle.angle) || particleRenderAbs(particle.angle) > 1000000.0f)
			return false;
	}
	for (index = begin; index < end; ++index)
	{
		const ParticleRenderInput &particle = input[index];
		ParticleRenderOutput &result = output[index];
		result.visible = 0;
		result.angle = 0;
		if (particleRenderAbs(particle.x-bounds.centerX) > bounds.extentX+particle.size ||
			particleRenderAbs(particle.y-bounds.centerY) > bounds.extentY+particle.size ||
			particleRenderAbs(particle.z-bounds.centerZ) > bounds.extentZ+particle.size)
			continue;
		result.visible = 1;
		/* Keep the legacy operation order and byte truncation, including wrap. */
		result.angle = static_cast<UnsignedByte>(static_cast<Int>(
			particle.angle * 255.0f / (2.0f * PI)));
	}
	return true;
}
