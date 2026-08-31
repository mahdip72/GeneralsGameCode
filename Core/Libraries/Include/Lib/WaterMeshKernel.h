#pragma once

#include "Lib/BaseTypeCore.h"

#define WATER_MESH_BUMP_SIZE 50.0f

struct WaterMeshRowInput
{
	Real y, v1, v2;
};

/* Field-for-field XYZDUV2 output; no GPU/WW3D dependency. */
struct WaterMeshVertex
{
	Real x, y, z;
	unsigned diffuse;
	Real u1, v1, u2, v2;
};

struct WaterMeshSnapshot
{
	unsigned width, height;
	const Real *heights;
	const WaterMeshRowInput *rows;
	Real cellSizeX, uScale;
	unsigned diffuse;
};

enum { WATER_MESH_MAX_VERTICES = 131072 };

/* Row sine-table samples are captured by the owner. The kernel reads only
 * private scalar arrays and writes the exclusive range [begin,end). */
bool PrepareWaterMeshRows(const WaterMeshSnapshot &snapshot,
	WaterMeshVertex *output, unsigned begin, unsigned end);
