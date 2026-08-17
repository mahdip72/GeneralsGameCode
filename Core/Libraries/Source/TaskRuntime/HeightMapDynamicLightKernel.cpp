#include "Lib/HeightMapDynamicLightKernel.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

namespace
{

static bool checkedMultiply(unsigned left, unsigned right, unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
		return false;
	*result = left * right;
	return true;
}

static bool checkedAdd(unsigned left, unsigned right, unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
		return false;
	*result = left + right;
	return true;
}

static bool finiteReal(Real value)
{
	return value == value && fabs(static_cast<double>(value)) <= 100000000.0;
}

static bool finiteDouble(double value)
{
	return value == value && fabs(value) <= 100000000.0;
}

static bool finiteColor(Real value)
{
	return finiteReal(value) && value >= -16.0f && value <= 16.0f;
}

/* Match WWMath::Sqrt on the legacy Win32 lane.  The owner-side serial
 * implementation uses the x87 single-precision fsqrt path; using a double
 * sqrt here can move a value across the final RGB truncation boundary. */
static Real dynamicLightSqrt(Real value)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	Real result;
	__asm {
		fld [value]
		fsqrt
		fstp [result]
	}
	return result;
#else
	return static_cast<Real>(sqrt(static_cast<double>(value)));
#endif
}

static bool checkedRange(const void *address, unsigned bytes,
	size_t *begin, size_t *end)
{
	const size_t maximum = static_cast<size_t>(-1);
	const size_t start = reinterpret_cast<size_t>(address);
	if (address == 0 || bytes == 0 || start > maximum - bytes)
		return false;
	*begin = start;
	*end = start + bytes;
	return true;
}

static bool alignedForReal(const void *address)
{
	return address != 0 &&
		reinterpret_cast<size_t>(address) % sizeof(Real) == 0;
}

static bool alignedForLight(const void *address)
{
	/* The shared Win32/VC6 ABI aligns this POD on the four-byte Real boundary;
	 * double members are valid at that alignment on the target x86 lane. */
	return address != 0 &&
		reinterpret_cast<size_t>(address) % sizeof(Real) == 0;
}

static bool rangesOverlap(size_t firstBegin, size_t firstEnd,
	size_t secondBegin, size_t secondEnd)
{
	return firstBegin < secondEnd && secondBegin < firstEnd;
}

static bool validLight(const HeightMapDynamicLightSceneLight &light)
{
	if (light.type != HEIGHTMAP_DYNAMIC_LIGHT_POINT &&
		light.type != HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL &&
		light.type != HEIGHTMAP_DYNAMIC_LIGHT_SPOT)
		return false;
	if (!finiteReal(light.positionX) || !finiteReal(light.positionY) ||
		!finiteReal(light.positionZ) || !finiteReal(light.directionX) ||
		!finiteReal(light.directionY) || !finiteReal(light.directionZ) ||
		!finiteColor(light.diffuseRed) || !finiteColor(light.diffuseGreen) ||
		!finiteColor(light.diffuseBlue) || !finiteColor(light.ambientRed) ||
		!finiteColor(light.ambientGreen) || !finiteColor(light.ambientBlue))
		return false;
	if (!finiteDouble(light.range) || !finiteDouble(light.midRange) ||
		light.range < 0.0 || light.midRange < 0.0)
		return false;
	if ((light.type == HEIGHTMAP_DYNAMIC_LIGHT_POINT ||
		light.type == HEIGHTMAP_DYNAMIC_LIGHT_SPOT) &&
		light.range > 0.0 && light.midRange >= 0.1 &&
		light.range <= light.midRange)
		return false;
	return true;
}

static bool validVertex(const HeightMapDynamicLightVertex &vertex)
{
	return finiteReal(vertex.x) && finiteReal(vertex.y) &&
		finiteReal(vertex.z) && vertex.applyLighting <= 1 &&
		finiteReal(vertex.normalX) &&
		finiteReal(vertex.normalY) && finiteReal(vertex.normalZ);
}

static bool validateInput(const HeightMapDynamicLightSnapshot &snapshot,
	const HeightMapDynamicLightVertex *output, unsigned yBegin,
	unsigned yEnd)
{
	unsigned cellCount;
	unsigned requiredVertices;
	unsigned inputBytes;
	unsigned outputBytes;
	unsigned lightBytes;
	unsigned requiredBatchBytes;
	size_t inputBegin;
	size_t inputEnd;
	size_t outputBegin;
	size_t outputEnd;
	size_t lightBegin;
	size_t lightEnd;
	size_t snapshotBegin;
	size_t snapshotEnd;
	unsigned row;
	unsigned column;

	if (snapshot.width == 0 || snapshot.height == 0 || yBegin > yEnd ||
		yEnd > snapshot.height || snapshot.lightCount >
			HEIGHTMAP_DYNAMIC_LIGHT_MAX_LIGHTS)
		return false;

	if (!checkedMultiply(snapshot.width, snapshot.height, &cellCount) ||
		!checkedMultiply(cellCount, HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT,
			&requiredVertices) || snapshot.vertexCount != requiredVertices ||
		snapshot.vertices == 0 || output == 0 ||
		!checkedMultiply(snapshot.width,
		HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT *
			static_cast<unsigned>(sizeof(HeightMapDynamicLightVertex)),
		&requiredVertices) || snapshot.inputStrideBytes < requiredVertices ||
		snapshot.outputStrideBytes < requiredVertices ||
		snapshot.inputStrideBytes % sizeof(HeightMapDynamicLightVertex) != 0 ||
		snapshot.outputStrideBytes % sizeof(HeightMapDynamicLightVertex) != 0 ||
		!checkedMultiply(snapshot.inputStrideBytes, snapshot.height,
			&inputBytes) || !checkedMultiply(snapshot.outputStrideBytes,
			snapshot.height, &outputBytes) || snapshot.outputCapacityBytes <
			outputBytes || !checkedMultiply(snapshot.lightCount,
			static_cast<unsigned>(sizeof(HeightMapDynamicLightSceneLight)),
			&lightBytes) || !checkedAdd(inputBytes, outputBytes,
			&requiredBatchBytes) || !checkedAdd(requiredBatchBytes, lightBytes,
			&requiredBatchBytes) || snapshot.batchBytes < requiredBatchBytes ||
		snapshot.batchBytes > HEIGHTMAP_DYNAMIC_LIGHT_MAX_BATCH_BYTES ||
		!checkedRange(&snapshot, static_cast<unsigned>(sizeof(snapshot)),
			&snapshotBegin, &snapshotEnd) ||
		!alignedForReal(snapshot.vertices) || !alignedForReal(output) ||
		!checkedRange(snapshot.vertices, inputBytes, &inputBegin, &inputEnd) ||
		!checkedRange(output, outputBytes, &outputBegin, &outputEnd))
		return false;

	if (rangesOverlap(inputBegin, inputEnd, outputBegin, outputEnd) ||
		rangesOverlap(snapshotBegin, snapshotEnd, inputBegin, inputEnd) ||
		rangesOverlap(snapshotBegin, snapshotEnd, outputBegin, outputEnd))
		return false;

	if (snapshot.lightCount != 0)
	{
		if (snapshot.lights == 0 || !alignedForLight(snapshot.lights) ||
			!checkedRange(snapshot.lights, lightBytes,
			&lightBegin, &lightEnd) ||
			rangesOverlap(inputBegin, inputEnd, lightBegin, lightEnd) ||
			rangesOverlap(outputBegin, outputEnd, lightBegin, lightEnd) ||
			rangesOverlap(snapshotBegin, snapshotEnd, lightBegin, lightEnd))
			return false;
		for (column = 0; column < snapshot.lightCount; ++column)
			if (!validLight(snapshot.lights[column]))
				return false;
	}

	for (row = yBegin; row < yEnd; ++row)
	{
		const unsigned char *inputRow = reinterpret_cast<const unsigned char *>(
			snapshot.vertices) + row * snapshot.inputStrideBytes;
		for (column = 0; column < snapshot.width *
			HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT; ++column)
		{
			const HeightMapDynamicLightVertex &vertex =
				reinterpret_cast<const HeightMapDynamicLightVertex *>(inputRow)[column];
			if (!validVertex(vertex))
				return false;
		}
	}
	return true;
}

static Real clampReal(Real value, Real low, Real high)
{
	if (value < low)
		return low;
	if (value > high)
		return high;
	return value;
}

static unsigned computeDiffuse(const HeightMapDynamicLightVertex &vertex,
	const HeightMapDynamicLightSceneLight *lights, unsigned lightCount)
{
	if (!vertex.applyLighting)
		return vertex.diffuse;

	const Real oo255 = 1.0f / 255.0f;
	Real shadeR = static_cast<Real>((vertex.diffuse >> 16) & 0xFF) * oo255;
	Real shadeG = static_cast<Real>((vertex.diffuse >> 8) & 0xFF) * oo255;
	Real shadeB = static_cast<Real>(vertex.diffuse & 0xFF) * oo255;
	const unsigned alpha = (vertex.diffuse >> 24) & 0xFF;
	unsigned lightIndex;

	for (lightIndex = 0; lightIndex < lightCount; ++lightIndex)
	{
		const HeightMapDynamicLightSceneLight &light = lights[lightIndex];
		Real directionX;
		Real directionY;
		Real directionZ;
		Real factor = 1.0f;
		if (!light.enabled)
			continue;

		if (light.type == HEIGHTMAP_DYNAMIC_LIGHT_POINT ||
			light.type == HEIGHTMAP_DYNAMIC_LIGHT_SPOT)
		{
			directionX = vertex.x - light.positionX;
			directionY = vertex.y - light.positionY;
			directionZ = vertex.z - light.positionZ;
			const Real distanceSquared = directionX * directionX +
				directionY * directionY + directionZ * directionZ;
			const Real distance = dynamicLightSqrt(distanceSquared);
			if (distance >= light.range || light.midRange < 0.1)
				continue;
			factor = static_cast<Real>(1.0 -
				(static_cast<double>(distance) - light.midRange) /
				(light.range - light.midRange));
			factor = clampReal(factor, 0.0f, 1.0f);
			/* The legacy divide-by-zero path produces NaN/undefined packed
			 * colors. Keep the worker finite and deterministic at coincidence;
			 * ambient contribution still follows the legacy factor order. */
			if (distance > 0.0f)
			{
				const Real inverseDistance = 1.0f / distance;
				directionX *= inverseDistance;
				directionY *= inverseDistance;
				directionZ *= inverseDistance;
			}
		}
		else
		{
			directionX = light.directionX;
			directionY = light.directionY;
			directionZ = light.directionZ;
		}

		{
			const Real lightRayX = -directionX;
			const Real lightRayY = -directionY;
			const Real lightRayZ = -directionZ;
			Real shade = lightRayX * vertex.normalX +
				lightRayY * vertex.normalY + lightRayZ * vertex.normalZ;
			shade *= factor;
			shade = clampReal(shade, 0.0f, 1.0f);
			shadeR += shade * light.diffuseRed;
			shadeG += shade * light.diffuseGreen;
			shadeB += shade * light.diffuseBlue;
			shadeR += factor * light.ambientRed;
			shadeG += factor * light.ambientGreen;
			shadeB += factor * light.ambientBlue;
		}
	}

	shadeR = clampReal(shadeR, 0.0f, 1.0f) * 255.0f;
	shadeG = clampReal(shadeG, 0.0f, 1.0f) * 255.0f;
	shadeB = clampReal(shadeB, 0.0f, 1.0f) * 255.0f;
	return (static_cast<unsigned>(shadeB) & 0xFFu) |
		((static_cast<unsigned>(shadeG) & 0xFFu) << 8) |
		((static_cast<unsigned>(shadeR) & 0xFFu) << 16) |
		(alpha << 24);
}

} // namespace

bool PrepareHeightMapDynamicLightRows(
	const HeightMapDynamicLightSnapshot &snapshot,
	HeightMapDynamicLightVertex *output, unsigned yBegin, unsigned yEnd)
{
	unsigned row;
	if (!validateInput(snapshot, output, yBegin, yEnd))
		return false;
	if (yBegin == yEnd)
		/* RadarTerrainPrepareService uses an empty second stripe for one row. */
		return true;

	for (row = yBegin; row < yEnd; ++row)
	{
		const unsigned char *inputRow = reinterpret_cast<const unsigned char *>(
			snapshot.vertices) + row * snapshot.inputStrideBytes;
		unsigned char *outputRow = reinterpret_cast<unsigned char *>(output) +
			row * snapshot.outputStrideBytes;
		unsigned column;
		for (column = 0; column < snapshot.width *
			HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT; ++column)
		{
			const HeightMapDynamicLightVertex &inputVertex =
				reinterpret_cast<const HeightMapDynamicLightVertex *>(inputRow)[column];
			HeightMapDynamicLightVertex &outputVertex =
				reinterpret_cast<HeightMapDynamicLightVertex *>(outputRow)[column];
			outputVertex = inputVertex;
			outputVertex.diffuse = computeDiffuse(inputVertex, snapshot.lights,
				snapshot.lightCount);
		}
	}
	return true;
}

static bool validatePreparedHeightMapDynamicLightStructure(
	const HeightMapDynamicLightSnapshot &snapshot,
	const HeightMapDynamicLightVertex *output)
{
	unsigned row;
	unsigned column;
	if (!validateInput(snapshot, output, 0, snapshot.height))
		return false;
	for (row = 0; row < snapshot.height; ++row)
	{
		const unsigned char *inputRow = reinterpret_cast<const unsigned char *>(
			snapshot.vertices) + row * snapshot.inputStrideBytes;
		const unsigned char *outputRow = reinterpret_cast<const unsigned char *>(
			output) + row * snapshot.outputStrideBytes;
		for (column = 0; column < snapshot.width *
			HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT; ++column)
		{
			const HeightMapDynamicLightVertex &inputVertex =
				reinterpret_cast<const HeightMapDynamicLightVertex *>(inputRow)[column];
			const HeightMapDynamicLightVertex &vertex =
				reinterpret_cast<const HeightMapDynamicLightVertex *>(outputRow)[column];
			if (!validVertex(vertex) || vertex.x != inputVertex.x ||
				vertex.y != inputVertex.y || vertex.z != inputVertex.z ||
				vertex.normalX != inputVertex.normalX ||
				vertex.normalY != inputVertex.normalY ||
				vertex.normalZ != inputVertex.normalZ ||
				vertex.applyLighting != inputVertex.applyLighting ||
				(vertex.diffuse & 0xFF000000u) !=
					(inputVertex.diffuse & 0xFF000000u))
				return false;
		}
	}
	return true;
}

bool ValidatePreparedHeightMapDynamicLightStructure(
	const HeightMapDynamicLightSnapshot &snapshot,
	const HeightMapDynamicLightVertex *output)
{
	return validatePreparedHeightMapDynamicLightStructure(snapshot, output);
}

bool ValidatePreparedHeightMapDynamicLightOutput(
	const HeightMapDynamicLightSnapshot &snapshot,
	const HeightMapDynamicLightVertex *output)
{
	unsigned row;
	unsigned column;
	if (!validatePreparedHeightMapDynamicLightStructure(snapshot, output))
		return false;
	for (row = 0; row < snapshot.height; ++row)
	{
		const unsigned char *inputRow = reinterpret_cast<const unsigned char *>(
			snapshot.vertices) + row * snapshot.inputStrideBytes;
		const unsigned char *outputRow = reinterpret_cast<const unsigned char *>(
			output) + row * snapshot.outputStrideBytes;
		for (column = 0; column < snapshot.width *
			HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT; ++column)
		{
			const HeightMapDynamicLightVertex &inputVertex =
				reinterpret_cast<const HeightMapDynamicLightVertex *>(inputRow)[column];
			const HeightMapDynamicLightVertex &vertex =
				reinterpret_cast<const HeightMapDynamicLightVertex *>(outputRow)[column];
			if (vertex.diffuse != computeDiffuse(inputVertex, snapshot.lights,
				snapshot.lightCount))
				return false;
		}
	}
	return true;
}
