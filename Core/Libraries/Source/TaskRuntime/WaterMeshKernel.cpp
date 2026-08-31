#include "Lib/WaterMeshKernel.h"

#include <float.h>
#include <stddef.h>

static bool waterMeshFinite(Real value)
{
	return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

static bool waterMeshDisjoint(const void *input, size_t inputBytes,
	const void *output, size_t outputBytes)
{
	const size_t first = reinterpret_cast<size_t>(input);
	const size_t second = reinterpret_cast<size_t>(output);
	const size_t maximum = static_cast<size_t>(-1);
	return input != 0 && output != 0 && first % sizeof(Real) == 0 &&
		second % sizeof(Real) == 0 && first <= maximum-inputBytes &&
		second <= maximum-outputBytes &&
		!(first < second+outputBytes && second < first+inputBytes);
}

bool PrepareWaterMeshRows(const WaterMeshSnapshot &snapshot,
	WaterMeshVertex *output, unsigned begin, unsigned end)
{
	if (begin > end || end > snapshot.height || snapshot.width == 0 ||
		snapshot.height == 0 || snapshot.width > WATER_MESH_MAX_VERTICES / snapshot.height)
		return false;
	const unsigned count = snapshot.width * snapshot.height;
	const size_t outputBytes = count * sizeof(WaterMeshVertex);
	if (!waterMeshDisjoint(snapshot.heights, count*sizeof(Real), output, outputBytes) ||
		!waterMeshDisjoint(snapshot.rows, snapshot.height*sizeof(WaterMeshRowInput), output, outputBytes) ||
		!waterMeshDisjoint(&snapshot, sizeof(snapshot), output, outputBytes) ||
		!waterMeshFinite(snapshot.cellSizeX) || !waterMeshFinite(snapshot.uScale) ||
		snapshot.cellSizeX < -1048576.0f || snapshot.cellSizeX > 1048576.0f ||
		snapshot.uScale < -1048576.0f || snapshot.uScale > 1048576.0f)
		return false;
	unsigned row;
	for (row = begin; row < end; ++row)
	{
		const WaterMeshRowInput &rowInput = snapshot.rows[row];
		if (!waterMeshFinite(rowInput.y) || !waterMeshFinite(rowInput.v1) ||
			!waterMeshFinite(rowInput.v2))
			return false;
		for (unsigned column = 0; column < snapshot.width; ++column)
			if (!waterMeshFinite(snapshot.heights[row*snapshot.width+column]))
				return false;
	}
	for (row = begin; row < end; ++row)
	{
		const WaterMeshRowInput &rowInput = snapshot.rows[row];
		for (unsigned column = 0; column < snapshot.width; ++column)
		{
			WaterMeshVertex &vertex = output[row*snapshot.width+column];
			vertex.x = static_cast<Real>(column) * snapshot.cellSizeX;
			vertex.y = rowInput.y;
			vertex.z = snapshot.heights[row*snapshot.width+column];
			vertex.diffuse = snapshot.diffuse;
			vertex.u1 = static_cast<Real>(column) * snapshot.uScale;
			vertex.v1 = rowInput.v1;
			vertex.u2 = static_cast<Real>(column) * snapshot.cellSizeX / WATER_MESH_BUMP_SIZE;
			vertex.v2 = rowInput.v2;
		}
	}
	return true;
}
