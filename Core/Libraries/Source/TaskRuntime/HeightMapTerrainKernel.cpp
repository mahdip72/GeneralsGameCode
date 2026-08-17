#include "Lib/HeightMapTerrainKernel.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>

struct HeightMapTerrainVector3
{
	Real x;
	Real y;
	Real z;
};

/* Admission bounds keep every finite accepted snapshot inside the narrow
 * production terrain/light domain.  In particular, normal and point-light
 * distance squares stay far below float overflow, while the depth-fade
 * multiplier remains safely inside the REAL_TO_INT input range. */
static const Real kHeightMapTerrainMinMapXYFactor = 0.001f;
static const Real kHeightMapTerrainMaxMapXYFactor = 4096.0f;
static const Real kHeightMapTerrainMaxHeightScale = 4096.0f;
static const Real kHeightMapTerrainMaxSpatialMagnitude = 1048576.0f;
static const Real kHeightMapTerrainMaxLightComponent = 1048576.0f;
static const Real kHeightMapTerrainMaxColorMagnitude = 16.0f;
static const Real kHeightMapTerrainMaxUvMagnitude = 1048576.0f;
static const double kHeightMapTerrainMaxLightRange = 1048576.0;
static const double kHeightMapTerrainMinAttenuationSpan = 0.001;
static const Real kHeightMapTerrainMinWaterHeight = 0.01f;

struct HeightMapTerrainVertexAlignmentProbe
{
	char prefix;
	HeightMapTerrainVertex value;
};

struct HeightMapTerrainCellAlignmentProbe
{
	char prefix;
	HeightMapTerrainCellInput value;
};

struct HeightMapTerrainGlobalLightAlignmentProbe
{
	char prefix;
	HeightMapTerrainGlobalLight value;
};

struct HeightMapTerrainSceneLightAlignmentProbe
{
	char prefix;
	HeightMapTerrainSceneLight value;
};

static size_t heightMapTerrainNaturalAlignment(size_t probeAlignment)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	/* MSVC x86 permits naturally aligned double members at four-byte object
	 * addresses; the char+T probe's member padding is not an object-address
	 * requirement on this ABI. */
	(void)probeAlignment;
	return sizeof(void *);
#else
	return probeAlignment;
#endif
}

static size_t heightMapTerrainVertexAlignment()
{
	return heightMapTerrainNaturalAlignment(
		offsetof(HeightMapTerrainVertexAlignmentProbe, value));
}

static size_t heightMapTerrainCellAlignment()
{
	return heightMapTerrainNaturalAlignment(
		offsetof(HeightMapTerrainCellAlignmentProbe, value));
}

static size_t heightMapTerrainGlobalLightAlignment()
{
	return heightMapTerrainNaturalAlignment(
		offsetof(HeightMapTerrainGlobalLightAlignmentProbe, value));
}

static size_t heightMapTerrainSceneLightAlignment()
{
	return heightMapTerrainNaturalAlignment(
		offsetof(HeightMapTerrainSceneLightAlignmentProbe, value));
}

/* Keep the legacy WWMath x86/x87 operation instead of replacing it with a
 * compiler-selected reciprocal square root. */
static Real heightMapTerrainInvSqrt(Real value)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	Real result;
	__asm {
		mov		eax, 0be6eb508h
		mov		DWORD PTR [esp-12],03fc00000h
		sub		eax, DWORD PTR [value]
		sub		DWORD PTR [value], 800000h
		shr		eax, 1
		mov		DWORD PTR [esp-8], eax

		fld		DWORD PTR [esp-8]
		fmul	st, st
		fld		DWORD PTR [esp-8]
		fxch	st(1)
		fmul	DWORD PTR [value]
		fld		DWORD PTR [esp-12]
		fld		st(0)
		fsub	st,st(2)

		fld		st(1)
		fxch	st(1)
		fmul	st(3),st
		fmul	st(3),st
		fmulp	st(4),st
		fsub	st,st(2)

		fmul	st(2),st
		fmul	st(3),st
		fmulp	st(2),st
		fxch	st(1)
		fsubp	st(1),st
		fmulp	st(1), st

		fstp	result
	}
	return result;
#else
	return 1.0f / (Real)sqrt(value);
#endif
}

static Real heightMapTerrainSqrt(Real value)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	Real result;
	__asm {
		fld		DWORD PTR [value]
		fsqrt
		fstp	result
	}
	return result;
#else
	return (Real)sqrt(value);
#endif
}

static void heightMapTerrainNormalize(HeightMapTerrainVector3 *vector)
{
	const Real lengthSquared = vector->x * vector->x +
		vector->y * vector->y + vector->z * vector->z;
	if (lengthSquared != 0.0f)
	{
		const Real inverseLength = heightMapTerrainInvSqrt(lengthSquared);
		vector->x *= inverseLength;
		vector->y *= inverseLength;
		vector->z *= inverseLength;
	}
}

static void heightMapTerrainNormalizedCross(
	const HeightMapTerrainVector3 &a, const HeightMapTerrainVector3 &b,
	HeightMapTerrainVector3 *result)
{
	result->x = a.y * b.z - a.z * b.y;
	result->y = a.z * b.x - a.x * b.z;
	result->z = a.x * b.y - a.y * b.x;
	heightMapTerrainNormalize(result);
}

static Real heightMapTerrainDot(const HeightMapTerrainVector3 &a,
	const HeightMapTerrainVector3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Real heightMapTerrainLength(const HeightMapTerrainVector3 &vector)
{
	return heightMapTerrainSqrt(vector.x * vector.x +
		vector.y * vector.y + vector.z * vector.z);
}

static Real heightMapTerrainClamp(Real value, Real minimum, Real maximum)
{
	if (value < minimum)
	{
		return minimum;
	}
	if (value > maximum)
	{
		return maximum;
	}
	return value;
}

/* This is deliberately the legacy REAL_TO_INT truncation semantics. */
static Int heightMapTerrainRealToInt(Real value)
{
	return static_cast<Int>(value);
}

static bool heightMapTerrainFiniteReal(Real value)
{
	const double promoted = static_cast<double>(value);
	return promoted == promoted && promoted <= DBL_MAX &&
		promoted >= -DBL_MAX;
}

static bool heightMapTerrainFiniteDouble(double value)
{
	return value == value && value <= DBL_MAX && value >= -DBL_MAX;
}

static bool heightMapTerrainWithinReal(Real value, Real magnitude)
{
	return heightMapTerrainFiniteReal(value) &&
		value <= magnitude && value >= -magnitude;
}

static bool heightMapTerrainWithinRgb(const HeightMapTerrainRgb &rgb,
	Real magnitude)
{
	return heightMapTerrainWithinReal(rgb.red, magnitude) &&
		heightMapTerrainWithinReal(rgb.green, magnitude) &&
		heightMapTerrainWithinReal(rgb.blue, magnitude);
}

static bool heightMapTerrainWithinDouble(double value, double magnitude)
{
	return heightMapTerrainFiniteDouble(value) &&
		value <= magnitude && value >= -magnitude;
}

static double heightMapTerrainAbsDouble(double value)
{
	return value < 0.0 ? -value : value;
}

static bool heightMapTerrainDepthFadeEndpointSafe(double z,
	double waterHeight, Real depthFade)
{
	const double multiplier = 1.0 -
		(1.4 - z) / waterHeight *
		(1.0 - static_cast<double>(depthFade));

	return heightMapTerrainFiniteDouble(multiplier) &&
		heightMapTerrainAbsDouble(multiplier) * 255.0 <=
		static_cast<double>(INT_MAX);
}

static bool heightMapTerrainDepthFadeBoundSafe(
	const HeightMapTerrainSnapshot &snapshot)
{
	const double maximumZ = 255.0 *
		static_cast<double>(snapshot.mapHeightScale);
	const double waterHeight = static_cast<double>(snapshot.waterHeight);

	if (!snapshot.useDepthFade)
	{
		return true;
	}

	return heightMapTerrainDepthFadeEndpointSafe(0.0, waterHeight,
			snapshot.depthFadeR) &&
		heightMapTerrainDepthFadeEndpointSafe(maximumZ, waterHeight,
			snapshot.depthFadeR) &&
		heightMapTerrainDepthFadeEndpointSafe(0.0, waterHeight,
			snapshot.depthFadeG) &&
		heightMapTerrainDepthFadeEndpointSafe(maximumZ, waterHeight,
			snapshot.depthFadeG) &&
		heightMapTerrainDepthFadeEndpointSafe(0.0, waterHeight,
			snapshot.depthFadeB) &&
		heightMapTerrainDepthFadeEndpointSafe(maximumZ, waterHeight,
			snapshot.depthFadeB);
}

static bool heightMapTerrainAligned(const void *address, size_t alignment)
{
	return address != 0 && alignment != 0 &&
		reinterpret_cast<size_t>(address) % alignment == 0;
}

static bool heightMapTerrainCheckedAddressRange(const void *address,
	unsigned bytes, size_t *begin, size_t *end)
{
	const size_t maximum = static_cast<size_t>(-1);
	const size_t start = reinterpret_cast<size_t>(address);

	if (address == 0 || bytes == 0 || start > maximum - bytes)
	{
		return false;
	}

	*begin = start;
	*end = start + bytes;
	return true;
}

static bool heightMapTerrainRangesOverlap(size_t firstBegin,
	size_t firstEnd, size_t secondBegin, size_t secondEnd)
{
	return firstBegin < secondEnd && secondBegin < firstEnd;
}

static bool heightMapTerrainCheckedMultiply(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}

	*result = left * right;
	return true;
}

static bool heightMapTerrainCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
	{
		return false;
	}

	*result = left + right;
	return true;
}

static bool heightMapTerrainValidate(
	const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex *output, unsigned yBegin, unsigned yEnd)
{
	unsigned requiredCellCount;
	unsigned requiredCellRowBytes;
	unsigned cellStorageBytes;
	unsigned requiredOutputRowBytes;
	unsigned outputStorageBytes;
	unsigned minimumBatchBytes;
	unsigned globalBytes;
	unsigned sceneBytes;
	size_t cellsBegin;
	size_t cellsEnd;
	size_t outputBegin;
	size_t outputEnd;
	size_t snapshotBegin;
	size_t snapshotEnd;
	size_t globalBegin;
	size_t globalEnd;
	size_t sceneBegin;
	size_t sceneEnd;
	unsigned row;
	unsigned column;
	unsigned lightIndex;
	const size_t cellAlignment = heightMapTerrainCellAlignment();
	const size_t vertexAlignment = heightMapTerrainVertexAlignment();
	const size_t globalLightAlignment = heightMapTerrainGlobalLightAlignment();
	const size_t sceneLightAlignment = heightMapTerrainSceneLightAlignment();

	if (output == 0 || snapshot.cells == 0 ||
		snapshot.width == 0 || snapshot.height == 0 ||
		yBegin >= yEnd || yEnd > snapshot.height)
	{
		return false;
	}

	if (!heightMapTerrainCheckedMultiply(snapshot.width, snapshot.height,
		&requiredCellCount) || snapshot.cellCount != requiredCellCount ||
		!heightMapTerrainCheckedMultiply(snapshot.width,
			static_cast<unsigned>(sizeof(HeightMapTerrainCellInput)),
			&requiredCellRowBytes) ||
		snapshot.cellRowStrideBytes < requiredCellRowBytes ||
		snapshot.cellRowStrideBytes %
			static_cast<unsigned>(cellAlignment) != 0 ||
		!heightMapTerrainCheckedMultiply(snapshot.cellRowStrideBytes,
			snapshot.height, &cellStorageBytes))
	{
		return false;
	}

	if (!heightMapTerrainCheckedMultiply(snapshot.width,
		static_cast<unsigned>(HEIGHTMAP_TERRAIN_VERTEX_COUNT) *
			static_cast<unsigned>(sizeof(HeightMapTerrainVertex)),
		&requiredOutputRowBytes) ||
		snapshot.outputStrideBytes < requiredOutputRowBytes ||
		snapshot.outputStrideBytes %
			static_cast<unsigned>(vertexAlignment) != 0 ||
		!heightMapTerrainCheckedMultiply(snapshot.outputStrideBytes,
			snapshot.height, &outputStorageBytes) ||
		snapshot.outputCapacityBytes < outputStorageBytes)
	{
		return false;
	}

	if (!heightMapTerrainCheckedMultiply(snapshot.globalLightCount,
			static_cast<unsigned>(sizeof(HeightMapTerrainGlobalLight)),
			&globalBytes) ||
		!heightMapTerrainCheckedMultiply(snapshot.sceneLightCount,
			static_cast<unsigned>(sizeof(HeightMapTerrainSceneLight)),
			&sceneBytes) ||
		!heightMapTerrainCheckedAdd(cellStorageBytes, globalBytes,
			&minimumBatchBytes) ||
		!heightMapTerrainCheckedAdd(minimumBatchBytes, sceneBytes,
			&minimumBatchBytes) ||
		!heightMapTerrainCheckedAdd(minimumBatchBytes,
			snapshot.outputCapacityBytes, &minimumBatchBytes) ||
		snapshot.batchBytes < minimumBatchBytes ||
		snapshot.batchBytes > HEIGHTMAP_TERRAIN_MAX_BATCH_BYTES)
	{
		return false;
	}

	if (snapshot.globalLightCount > HEIGHTMAP_TERRAIN_MAX_GLOBAL_LIGHTS ||
		snapshot.sceneLightCount >
			HEIGHTMAP_TERRAIN_MAX_BATCH_BYTES /
			static_cast<unsigned>(sizeof(HeightMapTerrainSceneLight)) ||
		(snapshot.globalLightCount != 0 && snapshot.globalLights == 0) ||
		(snapshot.sceneLightCount != 0 && snapshot.sceneLights == 0))
	{
		return false;
	}

	if (snapshot.useDepthFade &&
		(snapshot.waterHeight < kHeightMapTerrainMinWaterHeight ||
		 snapshot.waterHeight > kHeightMapTerrainMaxSpatialMagnitude))
	{
		return false;
	}

	if (!heightMapTerrainAligned(snapshot.cells,
			cellAlignment) ||
		!heightMapTerrainAligned(output, vertexAlignment) ||
		(snapshot.globalLightCount != 0 &&
			!heightMapTerrainAligned(snapshot.globalLights,
				globalLightAlignment)) ||
		(snapshot.sceneLightCount != 0 &&
			!heightMapTerrainAligned(snapshot.sceneLights,
				sceneLightAlignment)) ||
		!heightMapTerrainCheckedAddressRange(snapshot.cells, cellStorageBytes,
			&cellsBegin, &cellsEnd) ||
		!heightMapTerrainCheckedAddressRange(output, outputStorageBytes,
			&outputBegin, &outputEnd) ||
		!heightMapTerrainCheckedAddressRange(&snapshot, sizeof(snapshot),
			&snapshotBegin, &snapshotEnd))
	{
		return false;
	}

	if (snapshot.globalLightCount != 0 &&
		!heightMapTerrainCheckedAddressRange(snapshot.globalLights, globalBytes,
			&globalBegin, &globalEnd))
	{
		return false;
	}
	if (snapshot.sceneLightCount != 0 &&
		!heightMapTerrainCheckedAddressRange(snapshot.sceneLights, sceneBytes,
			&sceneBegin, &sceneEnd))
	{
		return false;
	}

	if (heightMapTerrainRangesOverlap(outputBegin, outputEnd,
		cellsBegin, cellsEnd) ||
		heightMapTerrainRangesOverlap(outputBegin, outputEnd,
			snapshotBegin, snapshotEnd) ||
		(snapshot.globalLightCount != 0 &&
			heightMapTerrainRangesOverlap(outputBegin, outputEnd,
				globalBegin, globalEnd)) ||
		(snapshot.sceneLightCount != 0 &&
			heightMapTerrainRangesOverlap(outputBegin, outputEnd,
				sceneBegin, sceneEnd)))
	{
		return false;
	}

	if (!heightMapTerrainFiniteReal(snapshot.mapXYFactor) ||
		snapshot.mapXYFactor < kHeightMapTerrainMinMapXYFactor ||
		snapshot.mapXYFactor > kHeightMapTerrainMaxMapXYFactor ||
		!heightMapTerrainFiniteReal(snapshot.mapHeightScale) ||
		snapshot.mapHeightScale < 0.0f ||
		snapshot.mapHeightScale > kHeightMapTerrainMaxHeightScale ||
		!heightMapTerrainWithinRgb(snapshot.terrainAmbient,
			kHeightMapTerrainMaxColorMagnitude) ||
		!heightMapTerrainWithinReal(snapshot.depthFadeR, 1.0f) ||
		snapshot.depthFadeR < 0.0f ||
		!heightMapTerrainWithinReal(snapshot.depthFadeG, 1.0f) ||
		snapshot.depthFadeG < 0.0f ||
		!heightMapTerrainWithinReal(snapshot.depthFadeB, 1.0f) ||
		snapshot.depthFadeB < 0.0f ||
		!heightMapTerrainFiniteReal(snapshot.waterHeight) ||
		!heightMapTerrainWithinReal(snapshot.waterHeight,
			kHeightMapTerrainMaxSpatialMagnitude) ||
		!heightMapTerrainDepthFadeBoundSafe(snapshot))
	{
		return false;
	}

	for (row = 0; row < snapshot.height; ++row)
	{
		const unsigned char *cellRow =
			reinterpret_cast<const unsigned char *>(snapshot.cells) +
			row * snapshot.cellRowStrideBytes;
		for (column = 0; column < snapshot.width; ++column)
		{
			const HeightMapTerrainCellInput &cell =
				reinterpret_cast<const HeightMapTerrainCellInput *>(cellRow)[column];
			unsigned corner;
			for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
			{
				if (!heightMapTerrainWithinReal(cell.x[corner],
						kHeightMapTerrainMaxSpatialMagnitude) ||
					!heightMapTerrainWithinReal(cell.y[corner],
						kHeightMapTerrainMaxSpatialMagnitude) ||
					!heightMapTerrainWithinReal(cell.u1[corner],
						kHeightMapTerrainMaxUvMagnitude) ||
					!heightMapTerrainWithinReal(cell.v1[corner],
						kHeightMapTerrainMaxUvMagnitude) ||
					!heightMapTerrainWithinReal(cell.u2[corner],
						kHeightMapTerrainMaxUvMagnitude) ||
					!heightMapTerrainWithinReal(cell.v2[corner],
						kHeightMapTerrainMaxUvMagnitude) ||
					cell.leftRightHeightDelta[corner] < -255 ||
					cell.leftRightHeightDelta[corner] > 255 ||
					cell.backForwardHeightDelta[corner] < -255 ||
					cell.backForwardHeightDelta[corner] > 255)
				{
					return false;
				}
			}
		}
	}

	for (lightIndex = 0; lightIndex < snapshot.globalLightCount;
		++lightIndex)
	{
		const HeightMapTerrainGlobalLight &light =
			snapshot.globalLights[lightIndex];
		if (!heightMapTerrainWithinReal(light.rayX,
				kHeightMapTerrainMaxLightComponent) ||
			!heightMapTerrainWithinReal(light.rayY,
				kHeightMapTerrainMaxLightComponent) ||
			!heightMapTerrainWithinReal(light.rayZ,
				kHeightMapTerrainMaxLightComponent) ||
			!heightMapTerrainWithinRgb(light.diffuse,
				kHeightMapTerrainMaxColorMagnitude))
		{
			return false;
		}
	}

	if (snapshot.sceneLights != 0)
	{
		for (lightIndex = 0; lightIndex < snapshot.sceneLightCount;
			++lightIndex)
		{
			const HeightMapTerrainSceneLight &light =
				snapshot.sceneLights[lightIndex];
			const unsigned type = light.type;
			if (type != HEIGHTMAP_TERRAIN_LIGHT_POINT &&
				type != HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL &&
				type != HEIGHTMAP_TERRAIN_LIGHT_SPOT)
			{
				return false;
			}
			if (!heightMapTerrainWithinReal(light.positionX,
					kHeightMapTerrainMaxSpatialMagnitude) ||
				!heightMapTerrainWithinReal(light.positionY,
					kHeightMapTerrainMaxSpatialMagnitude) ||
				!heightMapTerrainWithinReal(light.positionZ,
					kHeightMapTerrainMaxSpatialMagnitude) ||
				!heightMapTerrainWithinReal(light.directionX,
					kHeightMapTerrainMaxLightComponent) ||
				!heightMapTerrainWithinReal(light.directionY,
					kHeightMapTerrainMaxLightComponent) ||
				!heightMapTerrainWithinReal(light.directionZ,
					kHeightMapTerrainMaxLightComponent) ||
				!heightMapTerrainWithinDouble(light.range,
					kHeightMapTerrainMaxLightRange) ||
				!heightMapTerrainWithinDouble(light.midRange,
					kHeightMapTerrainMaxLightRange) ||
				light.range < 0.0 || light.midRange < 0.0 ||
				!heightMapTerrainWithinRgb(light.diffuse,
					kHeightMapTerrainMaxColorMagnitude) ||
				!heightMapTerrainWithinRgb(light.ambient,
					kHeightMapTerrainMaxColorMagnitude) ||
				((type == HEIGHTMAP_TERRAIN_LIGHT_POINT ||
					type == HEIGHTMAP_TERRAIN_LIGHT_SPOT) &&
				 light.midRange >= 0.1 && light.range > 0.0 &&
				 light.range - light.midRange <
					kHeightMapTerrainMinAttenuationSpan))
			{
				return false;
			}
		}
	}

	return true;
}

static void heightMapTerrainNormal(const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainCellInput &cell, unsigned corner,
	HeightMapTerrainVector3 *normal)
{
	HeightMapTerrainVector3 l2r;
	HeightMapTerrainVector3 n2f;
	const Real span = 2.0f * snapshot.mapXYFactor;

	l2r.x = span;
	l2r.y = 0.0f;
	l2r.z = snapshot.mapHeightScale *
		static_cast<Real>(cell.leftRightHeightDelta[corner]);
	n2f.x = 0.0f;
	n2f.y = span;
	n2f.z = snapshot.mapHeightScale *
		static_cast<Real>(cell.backForwardHeightDelta[corner]);
	heightMapTerrainNormalizedCross(l2r, n2f, normal);
}

static void heightMapTerrainAddGlobalLight(const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVector3 &normal,
	Real *shadeR, Real *shadeG, Real *shadeB)
{
	unsigned lightIndex;
	for (lightIndex = 0; lightIndex < snapshot.globalLightCount;
		++lightIndex)
	{
		const HeightMapTerrainGlobalLight &light =
			snapshot.globalLights[lightIndex];
		HeightMapTerrainVector3 lightRay;
		lightRay.x = light.rayX;
		lightRay.y = light.rayY;
		lightRay.z = light.rayZ;
		Real shade = heightMapTerrainDot(lightRay, normal);
		if (shade > 1.0)
		{
			shade = 1.0f;
		}
		if (shade < 0.0f)
		{
			shade = 0.0f;
		}
		*shadeR += shade * light.diffuse.red;
		*shadeG += shade * light.diffuse.green;
		*shadeB += shade * light.diffuse.blue;
	}
}

static void heightMapTerrainApplySceneLight(
	const HeightMapTerrainSceneLight &light,
	const HeightMapTerrainVertex &vertex,
	const HeightMapTerrainVector3 &normal,
	Real *shadeR, Real *shadeG, Real *shadeB)
{
	HeightMapTerrainVector3 lightDirection;
	lightDirection.x = vertex.x;
	lightDirection.y = vertex.y;
	lightDirection.z = vertex.z;
	Real factor = 1.0f;

	switch (light.type)
	{
	case HEIGHTMAP_TERRAIN_LIGHT_POINT:
	case HEIGHTMAP_TERRAIN_LIGHT_SPOT:
		{
			HeightMapTerrainVector3 lightLocation;
			lightLocation.x = light.positionX;
			lightLocation.y = light.positionY;
			lightLocation.z = light.positionZ;
			const double range = light.range;
			const double midRange = light.midRange;
			lightDirection.x -= lightLocation.x;
			lightDirection.y -= lightLocation.y;
			lightDirection.z -= lightLocation.z;
			if (vertex.x < lightLocation.x - range ||
				vertex.x > lightLocation.x + range ||
				vertex.y < lightLocation.y - range ||
				vertex.y > lightLocation.y + range)
			{
				return;
			}
			const Real distance = heightMapTerrainLength(lightDirection);
			if (distance >= range || midRange < 0.1)
			{
				return;
			}
			factor = 1.0f - (distance - midRange) /
				(range - midRange);
			factor = heightMapTerrainClamp(factor, 0.0f, 1.0f);
		}
		break;
	case HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL:
		lightDirection.x = light.directionX;
		lightDirection.y = light.directionY;
		lightDirection.z = light.directionZ;
		factor = 1.0f;
		break;
	}

	heightMapTerrainNormalize(&lightDirection);
	{
		HeightMapTerrainVector3 lightRay;
		lightRay.x = -lightDirection.x;
		lightRay.y = -lightDirection.y;
		lightRay.z = -lightDirection.z;
		Real shade = heightMapTerrainDot(lightRay, normal);
		shade *= factor;
		if (shade > 1.0)
		{
			shade = 1.0f;
		}
		if (shade < 0.0f)
		{
			shade = 0.0f;
		}
		*shadeR += shade * light.diffuse.red;
		*shadeG += shade * light.diffuse.green;
		*shadeB += shade * light.diffuse.blue;
		*shadeR += factor * light.ambient.red;
		*shadeG += factor * light.ambient.green;
		*shadeB += factor * light.ambient.blue;
	}
}

static unsigned heightMapTerrainDiffuse(const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex &vertex,
	const HeightMapTerrainVector3 &normal)
{
	Real shadeR = snapshot.terrainAmbient.red;
	Real shadeG = snapshot.terrainAmbient.green;
	Real shadeB = snapshot.terrainAmbient.blue;
	unsigned lightIndex;

	for (lightIndex = 0; lightIndex < snapshot.sceneLightCount;
		++lightIndex)
	{
		heightMapTerrainApplySceneLight(snapshot.sceneLights[lightIndex],
			vertex, normal, &shadeR, &shadeG, &shadeB);
	}

	heightMapTerrainAddGlobalLight(snapshot, normal, &shadeR,
		&shadeG, &shadeB);

	if (shadeR > 1.0)
	{
		shadeR = 1.0f;
	}
	if (shadeR < 0.0f)
	{
		shadeR = 0.0f;
	}
	if (shadeG > 1.0)
	{
		shadeG = 1.0f;
	}
	if (shadeG < 0.0f)
	{
		shadeG = 0.0f;
	}
	if (shadeB > 1.0)
	{
		shadeB = 1.0f;
	}
	if (shadeB < 0.0f)
	{
		shadeB = 0.0f;
	}

	if (snapshot.useDepthFade && vertex.z <= snapshot.waterHeight)
	{
		const Real depthScale = (1.4f - vertex.z) / snapshot.waterHeight;
		shadeR *= 1.0f - depthScale * (1.0f - snapshot.depthFadeR);
		shadeG *= 1.0f - depthScale * (1.0f - snapshot.depthFadeG);
		shadeB *= 1.0f - depthScale * (1.0f - snapshot.depthFadeB);
	}

	shadeR *= 255.0f;
	shadeG *= 255.0f;
	shadeB *= 255.0f;
	/* Validation proves every post-depth channel *255 is inside Int before
	 * this legacy truncation. */
	return static_cast<unsigned>(heightMapTerrainRealToInt(shadeB)) |
		(static_cast<unsigned>(heightMapTerrainRealToInt(shadeG)) << 8) |
		(static_cast<unsigned>(heightMapTerrainRealToInt(shadeR)) << 16);
}

static void heightMapTerrainPrepareCell(
	const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainCellInput &cell,
	HeightMapTerrainVertex vertices[4])
{
	unsigned corner;
	for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
	{
		HeightMapTerrainVector3 normal;

		heightMapTerrainNormal(snapshot, cell, corner, &normal);
		vertices[corner].x = cell.x[corner];
		vertices[corner].y = cell.y[corner];
		vertices[corner].z = static_cast<Real>(cell.vertexHeight[corner]) *
			snapshot.mapHeightScale;
		vertices[corner].u1 = cell.u1[corner];
		vertices[corner].v1 = cell.v1[corner];
		vertices[corner].u2 = cell.u2[corner];
		vertices[corner].v2 = cell.v2[corner];
		vertices[corner].diffuse = (static_cast<unsigned>(cell.alpha[corner]) << 24) |
			heightMapTerrainDiffuse(snapshot, vertices[corner], normal);
	}

	if (cell.flip)
	{
		HeightMapTerrainVertex temporary = vertices[0];
		vertices[0] = vertices[1];
		vertices[1] = vertices[2];
		vertices[2] = vertices[3];
		vertices[3] = temporary;
	}
}

bool PrepareHeightMapTerrainRows(const HeightMapTerrainSnapshot &snapshot,
	HeightMapTerrainVertex *output, unsigned yBegin, unsigned yEnd)
{
	unsigned row;

	if (!heightMapTerrainValidate(snapshot, output, yBegin, yEnd))
	{
		return false;
	}

	for (row = yBegin; row < yEnd; ++row)
	{
		const unsigned char *cellRow =
			reinterpret_cast<const unsigned char *>(snapshot.cells) +
			row * snapshot.cellRowStrideBytes;
		HeightMapTerrainVertex *outputRow =
			reinterpret_cast<HeightMapTerrainVertex *>(
				reinterpret_cast<unsigned char *>(output) +
				row * snapshot.outputStrideBytes);
		unsigned column;

		for (column = 0; column < snapshot.width; ++column)
		{
			const HeightMapTerrainCellInput *cell =
				reinterpret_cast<const HeightMapTerrainCellInput *>(cellRow) +
				column;
			HeightMapTerrainVertex vertices[HEIGHTMAP_TERRAIN_VERTEX_COUNT];
			heightMapTerrainPrepareCell(snapshot, *cell, vertices);
			unsigned corner;

			for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT;
				++corner)
			{
				outputRow[column * HEIGHTMAP_TERRAIN_VERTEX_COUNT + corner] =
					vertices[corner];
			}
		}
	}

	return true;
}

bool ValidatePreparedHeightMapTerrainOutput(
	const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex *output)
{
	unsigned row;

	if (!heightMapTerrainValidate(snapshot, output, 0, snapshot.height))
	{
		return false;
	}

	for (row = 0; row < snapshot.height; ++row)
	{
		const HeightMapTerrainVertex *outputRow =
			reinterpret_cast<const HeightMapTerrainVertex *>(
				reinterpret_cast<const unsigned char *>(output) +
				row * snapshot.outputStrideBytes);
		unsigned column;
		for (column = 0; column < snapshot.width *
			HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++column)
		{
			const HeightMapTerrainVertex &vertex = outputRow[column];
			if (!heightMapTerrainFiniteReal(vertex.x) ||
				!heightMapTerrainFiniteReal(vertex.y) ||
				!heightMapTerrainFiniteReal(vertex.z) ||
				!heightMapTerrainFiniteReal(vertex.u1) ||
				!heightMapTerrainFiniteReal(vertex.v1) ||
				!heightMapTerrainFiniteReal(vertex.u2) ||
				!heightMapTerrainFiniteReal(vertex.v2))
			{
				return false;
			}
		}
	}

	return true;
}
