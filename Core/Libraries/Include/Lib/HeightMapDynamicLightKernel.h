#pragma once

#include "Lib/BaseTypeCore.h"

/*
 * D3D-free input/output for one row range of dynamic terrain lighting.  The
 * render owner copies the baseline vertex and normal fields into this POD;
 * workers never see VERTEX_FORMAT, a map, a light object, or a D3D resource.
 */
struct HeightMapDynamicLightVertex
{
	Real x;
	Real y;
	Real z;
	unsigned diffuse;
	/* One means the legacy current/previous bounds require publication. */
	UnsignedByte applyLighting;
	Real normalX;
	Real normalY;
	Real normalZ;
};

enum HeightMapDynamicLightType
{
	HEIGHTMAP_DYNAMIC_LIGHT_POINT = 0,
	HEIGHTMAP_DYNAMIC_LIGHT_DIRECTIONAL = 1,
	HEIGHTMAP_DYNAMIC_LIGHT_SPOT = 2
};

struct HeightMapDynamicLightSceneLight
{
	unsigned type;
	UnsignedByte enabled;
	Real positionX;
	Real positionY;
	Real positionZ;
	Real directionX;
	Real directionY;
	Real directionZ;
	double range;
	double midRange;
	Real diffuseRed;
	Real diffuseGreen;
	Real diffuseBlue;
	Real ambientRed;
	Real ambientGreen;
	Real ambientBlue;
};

struct HeightMapDynamicLightSnapshot
{
	unsigned width;
	unsigned height;
	unsigned inputStrideBytes;
	unsigned outputStrideBytes;
	unsigned vertexCount;
	const HeightMapDynamicLightVertex *vertices;
	const HeightMapDynamicLightSceneLight *lights;
	unsigned lightCount;
	unsigned outputCapacityBytes;
	unsigned batchBytes;
};

enum
{
	HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT = 4,
	HEIGHTMAP_DYNAMIC_LIGHT_MAX_LIGHTS = 64,
	HEIGHTMAP_DYNAMIC_LIGHT_MAX_BATCH_BYTES = 2u * 1024u * 1024u
};

/* Write only vertices in the half-open row range [yBegin, yEnd). */
bool PrepareHeightMapDynamicLightRows(
	const HeightMapDynamicLightSnapshot &snapshot,
	HeightMapDynamicLightVertex *output, unsigned yBegin, unsigned yEnd);

/* Validate all input and all staged output before owner-side publication. */
bool ValidatePreparedHeightMapDynamicLightOutput(
	const HeightMapDynamicLightSnapshot &snapshot,
	const HeightMapDynamicLightVertex *output);

/* Validate the staged object without replaying the lighting arithmetic. */
bool ValidatePreparedHeightMapDynamicLightStructure(
	const HeightMapDynamicLightSnapshot &snapshot,
	const HeightMapDynamicLightVertex *output);
