#include "Lib/ProjectedTerrainGridKernel.h"

#include <float.h>
#include <limits.h>
#include <stddef.h>

namespace
{

bool projectedTerrainCheckedMultiply(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
		return false;
	*result = left * right;
	return true;
}

bool projectedTerrainCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
		return false;
	*result = left + right;
	return true;
}

bool projectedTerrainFinite(Real value)
{
	return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

bool projectedTerrainBounded(Real value, Real bound)
{
	return projectedTerrainFinite(value) && value >= -bound && value <= bound;
}

bool projectedTerrainAddressRange(const void *address, size_t bytes,
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

bool projectedTerrainRangesOverlap(size_t firstBegin, size_t firstEnd,
	size_t secondBegin, size_t secondEnd)
{
	return firstBegin < secondEnd && secondBegin < firstEnd;
}

bool projectedTerrainValidate(const ProjectedTerrainGridSnapshot &snapshot,
	const ProjectedTerrainGridVertex *vertices,
	const UnsignedShort *indices, bool checkInput)
{
	unsigned vertexCount;
	unsigned cellCount;
	unsigned vertexBytes;
	unsigned indexBytes;
	unsigned heightBytes;
	unsigned totalBytes;
	size_t vertexBegin;
	size_t vertexEnd;
	size_t indexBegin;
	size_t indexEnd;
	size_t heightBegin;
	size_t heightEnd;
	size_t flipBegin;
	size_t flipEnd;
	size_t snapshotBegin;
	size_t snapshotEnd;
	unsigned row;
	unsigned column;

	if (snapshot.kind != PROJECTED_TERRAIN_GRID_SHADOW &&
		snapshot.kind != PROJECTED_TERRAIN_GRID_DECAL)
		return false;
	if (snapshot.width == 0 || snapshot.height == 0 ||
		snapshot.cellWidth != snapshot.width - 1 ||
		snapshot.cellHeight != snapshot.height - 1 ||
		vertices == 0 || snapshot.heights == 0)
		return false;
	if (!projectedTerrainCheckedMultiply(snapshot.width, snapshot.height,
		&vertexCount) || vertexCount > PROJECTED_TERRAIN_GRID_MAX_VERTICES ||
		!projectedTerrainCheckedMultiply(snapshot.cellWidth,
			snapshot.cellHeight, &cellCount) ||
		(cellCount != 0 && (snapshot.flips == 0 || indices == 0)) ||
		!projectedTerrainCheckedMultiply(vertexCount,
			static_cast<unsigned>(sizeof(ProjectedTerrainGridVertex)),
			&vertexBytes) ||
		!projectedTerrainCheckedMultiply(cellCount, 6u, &indexBytes) ||
		!projectedTerrainCheckedMultiply(indexBytes,
			static_cast<unsigned>(sizeof(UnsignedShort)), &indexBytes) ||
		!projectedTerrainCheckedMultiply(vertexCount,
			static_cast<unsigned>(sizeof(Real)), &heightBytes) ||
		!projectedTerrainCheckedAdd(cellCount, vertexBytes, &totalBytes) ||
		!projectedTerrainCheckedAdd(totalBytes, indexBytes, &totalBytes) ||
		!projectedTerrainCheckedAdd(totalBytes, heightBytes, &totalBytes) ||
		!projectedTerrainCheckedAdd(totalBytes, cellCount, &totalBytes) ||
		totalBytes > PROJECTED_TERRAIN_GRID_MAX_BYTES)
		return false;

	if (!projectedTerrainAddressRange(vertices, vertexBytes,
		&vertexBegin, &vertexEnd) ||
		!projectedTerrainAddressRange(snapshot.heights, heightBytes,
			&heightBegin, &heightEnd) ||
		!projectedTerrainAddressRange(&snapshot, sizeof(snapshot),
			&snapshotBegin, &snapshotEnd) ||
		(cellCount != 0 &&
			(!projectedTerrainAddressRange(indices, indexBytes,
				&indexBegin, &indexEnd) ||
			 !projectedTerrainAddressRange(snapshot.flips, cellCount,
				&flipBegin, &flipEnd))))
		return false;

	if (projectedTerrainRangesOverlap(vertexBegin, vertexEnd,
		heightBegin, heightEnd) ||
		projectedTerrainRangesOverlap(vertexBegin, vertexEnd,
			snapshotBegin, snapshotEnd) ||
		(cellCount != 0 &&
			(projectedTerrainRangesOverlap(vertexBegin, vertexEnd,
				indexBegin, indexEnd) ||
			 projectedTerrainRangesOverlap(vertexBegin, vertexEnd,
				flipBegin, flipEnd) ||
			 projectedTerrainRangesOverlap(indexBegin, indexEnd,
				flipBegin, flipEnd) ||
			 projectedTerrainRangesOverlap(heightBegin, heightEnd,
				flipBegin, flipEnd) ||
			 projectedTerrainRangesOverlap(indexBegin, indexEnd,
				heightBegin, heightEnd) ||
			 projectedTerrainRangesOverlap(indexBegin, indexEnd,
				snapshotBegin, snapshotEnd) ||
			 projectedTerrainRangesOverlap(flipBegin, flipEnd,
				snapshotBegin, snapshotEnd))))
		return false;

	if (snapshot.clampToLayerHeight > 1 ||
		!projectedTerrainBounded(snapshot.mapXYFactor, 4096.0f) ||
		snapshot.mapXYFactor < 0.001f ||
		!projectedTerrainBounded(snapshot.mapHeightScale, 4096.0f) ||
		snapshot.mapHeightScale < 0.0f ||
		!projectedTerrainBounded(snapshot.layerHeight, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.heightBias, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.uAxisX, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.uAxisY, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.vAxisX, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.vAxisY, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.objectX, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.objectY, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.uOffset, 1048576.0f) ||
		!projectedTerrainBounded(snapshot.vOffset, 1048576.0f))
		return false;

	if (!checkInput)
		return true;

	for (row = 0; row < snapshot.height; ++row)
	{
		for (column = 0; column < snapshot.width; ++column)
		{
			if (!projectedTerrainFinite(snapshot.heights[
				row * snapshot.width + column]))
				return false;
		}
	}
	for (row = 0; row < snapshot.cellHeight; ++row)
	{
		for (column = 0; column < snapshot.cellWidth; ++column)
		{
			if (snapshot.flips[row * snapshot.cellWidth + column] > 1)
				return false;
		}
	}
	return true;
}

bool projectedTerrainValidateRows(
	const ProjectedTerrainGridSnapshot &snapshot, unsigned rowBegin,
	unsigned rowEnd)
{
	unsigned row;
	unsigned column;
	if (rowBegin >= rowEnd || rowEnd > snapshot.height)
		return false;
	for (row = rowBegin; row < rowEnd; ++row)
	{
		for (column = 0; column < snapshot.width; ++column)
		{
			if (!projectedTerrainFinite(snapshot.heights[
				row * snapshot.width + column]))
				return false;
		}
		if (row < snapshot.cellHeight)
		{
			for (column = 0; column < snapshot.cellWidth; ++column)
			{
				if (snapshot.flips[row * snapshot.cellWidth + column] > 1)
					return false;
			}
		}
	}
	return true;
}

void projectedTerrainWriteVertex(const ProjectedTerrainGridSnapshot &snapshot,
	const Real *height, unsigned column, unsigned row,
	ProjectedTerrainGridVertex *vertex)
{
	const Int mapX = snapshot.firstMapX + static_cast<Int>(column) -
		snapshot.coordinateBiasX;
	const Int mapY = snapshot.firstMapY + static_cast<Int>(row) -
		snapshot.coordinateBiasY;
	const Real x = static_cast<Real>(mapX) * snapshot.mapXYFactor;
	const Real y = static_cast<Real>(mapY) * snapshot.mapXYFactor;
	const Real heightValue = *height * snapshot.mapHeightScale;

	vertex->x = x;
	vertex->y = y;
	vertex->z = heightValue;
	vertex->diffuse = snapshot.kind == PROJECTED_TERRAIN_GRID_DECAL ?
		snapshot.diffuse : 0;
	vertex->u = 0.0f;
	vertex->v = 0.0f;

	if (snapshot.kind == PROJECTED_TERRAIN_GRID_DECAL)
	{
		if (snapshot.clampToLayerHeight)
			vertex->z = heightValue > snapshot.layerHeight ? heightValue :
				snapshot.layerHeight;
		else
			vertex->z = heightValue + snapshot.heightBias;

		const Real deltaX = x - snapshot.objectX;
		const Real deltaY = y - snapshot.objectY;
		vertex->u = snapshot.uAxisX * deltaX +
			snapshot.uAxisY * deltaY + snapshot.uOffset;
		vertex->v = snapshot.vAxisX * deltaX +
			snapshot.vAxisY * deltaY + snapshot.vOffset;
	}
}

void projectedTerrainWriteIndices(const ProjectedTerrainGridSnapshot &snapshot,
	unsigned row, unsigned column, UnsignedShort *output)
{
	const unsigned rowStart = row * snapshot.width;
	const unsigned cellStart = (row * snapshot.cellWidth + column) * 6;
	const unsigned vertex = rowStart + column;
	const unsigned nextRow = vertex + snapshot.width;
	const bool flip = snapshot.flips[row * snapshot.cellWidth + column] != 0;

	if (flip)
	{
		output[cellStart + 0] = static_cast<UnsignedShort>(vertex + 1);
		output[cellStart + 1] = static_cast<UnsignedShort>(nextRow);
		output[cellStart + 2] = static_cast<UnsignedShort>(vertex);
		output[cellStart + 3] = static_cast<UnsignedShort>(vertex + 1);
		output[cellStart + 4] = static_cast<UnsignedShort>(nextRow + 1);
		output[cellStart + 5] = static_cast<UnsignedShort>(nextRow);
	}
	else
	{
		output[cellStart + 0] = static_cast<UnsignedShort>(vertex);
		output[cellStart + 1] = static_cast<UnsignedShort>(nextRow + 1);
		output[cellStart + 2] = static_cast<UnsignedShort>(nextRow);
		output[cellStart + 3] = static_cast<UnsignedShort>(vertex);
		output[cellStart + 4] = static_cast<UnsignedShort>(vertex + 1);
		output[cellStart + 5] = static_cast<UnsignedShort>(nextRow + 1);
	}
}

} // namespace

ProjectedTerrainGridScratch::ProjectedTerrainGridScratch()
	: m_width(0), m_height(0), m_heights(0), m_flips(0),
	m_vertices(0), m_indices(0)
{
}

ProjectedTerrainGridScratch::~ProjectedTerrainGridScratch()
{
	reset();
}

bool ProjectedTerrainGridScratch::ensure(unsigned width, unsigned height)
{
	unsigned vertexCount;
	unsigned cellCount;
	unsigned vertexBytes;
	unsigned heightBytes;
	unsigned indexCount;
	unsigned indexBytes;
	unsigned totalBytes;
	Real *heights;
	UnsignedByte *flips;
	ProjectedTerrainGridVertex *vertices;
	UnsignedShort *indices;

	if (width == 0 || height == 0 ||
		!projectedTerrainCheckedMultiply(width, height, &vertexCount) ||
		vertexCount > PROJECTED_TERRAIN_GRID_MAX_VERTICES ||
		!projectedTerrainCheckedMultiply(width - 1, height - 1, &cellCount) ||
		!projectedTerrainCheckedMultiply(vertexCount,
			static_cast<unsigned>(sizeof(Real)), &heightBytes) ||
		!projectedTerrainCheckedMultiply(vertexCount,
			static_cast<unsigned>(sizeof(ProjectedTerrainGridVertex)),
			&vertexBytes) ||
		!projectedTerrainCheckedMultiply(cellCount, 6u, &indexCount) ||
		!projectedTerrainCheckedMultiply(indexCount,
			static_cast<unsigned>(sizeof(UnsignedShort)), &indexBytes) ||
		!projectedTerrainCheckedAdd(heightBytes, cellCount, &totalBytes) ||
		!projectedTerrainCheckedAdd(totalBytes, vertexBytes, &totalBytes) ||
		!projectedTerrainCheckedAdd(totalBytes, indexBytes, &totalBytes) ||
		totalBytes > PROJECTED_TERRAIN_GRID_MAX_BYTES)
		return false;

	if (m_width >= width && m_height >= height && isAllocated())
		return true;

	heights = 0;
	flips = 0;
	vertices = 0;
	indices = 0;
	try
	{
		heights = new Real[vertexCount];
		flips = new UnsignedByte[cellCount == 0 ? 1 : cellCount];
		vertices = new ProjectedTerrainGridVertex[vertexCount];
		indices = new UnsignedShort[indexCount == 0 ? 1 : indexCount];
	}
	catch (...)
	{
		delete [] indices;
		delete [] vertices;
		delete [] flips;
		delete [] heights;
		return false;
	}

	delete [] m_indices;
	delete [] m_vertices;
	delete [] m_flips;
	delete [] m_heights;
	m_width = width;
	m_height = height;
	m_heights = heights;
	m_flips = flips;
	m_vertices = vertices;
	m_indices = indices;
	return true;
}

void ProjectedTerrainGridScratch::reset()
{
	delete [] m_indices;
	delete [] m_vertices;
	delete [] m_flips;
	delete [] m_heights;
	m_width = 0;
	m_height = 0;
	m_heights = 0;
	m_flips = 0;
	m_vertices = 0;
	m_indices = 0;
}

bool ValidateProjectedTerrainGridInput(
	const ProjectedTerrainGridSnapshot &snapshot,
	const ProjectedTerrainGridVertex *vertices,
	const UnsignedShort *indices)
{
	return projectedTerrainValidate(snapshot, vertices, indices, true);
}

bool PrepareProjectedTerrainGridRows(
	const ProjectedTerrainGridSnapshot &snapshot,
	ProjectedTerrainGridVertex *vertices,
	UnsignedShort *indices, unsigned rowBegin, unsigned rowEnd)
{
	unsigned row;
	unsigned column;
	if (!projectedTerrainValidate(snapshot, vertices, indices, false) ||
		!projectedTerrainValidateRows(snapshot, rowBegin, rowEnd))
		return false;

	for (row = rowBegin; row < rowEnd; ++row)
	{
		for (column = 0; column < snapshot.width; ++column)
		{
			projectedTerrainWriteVertex(snapshot,
				&snapshot.heights[row * snapshot.width + column],
				column, row, &vertices[row * snapshot.width + column]);
		}
		if (row < snapshot.cellHeight)
		{
			for (column = 0; column < snapshot.cellWidth; ++column)
				projectedTerrainWriteIndices(snapshot, row, column, indices);
		}
	}
	return true;
}

bool ValidatePreparedProjectedTerrainGridOutput(
	const ProjectedTerrainGridSnapshot &snapshot,
	const ProjectedTerrainGridVertex *vertices,
	const UnsignedShort *indices)
{
	unsigned vertexCount;
	unsigned cellCount;
	unsigned indexCount;
	unsigned index;
	if (!projectedTerrainValidate(snapshot, vertices, indices, true) ||
		!projectedTerrainCheckedMultiply(snapshot.width, snapshot.height,
			&vertexCount) ||
		!projectedTerrainCheckedMultiply(snapshot.cellWidth,
			snapshot.cellHeight, &cellCount) ||
		!projectedTerrainCheckedMultiply(cellCount, 6u, &indexCount))
		return false;

	for (index = 0; index < vertexCount; ++index)
	{
		if (!projectedTerrainFinite(vertices[index].x) ||
			!projectedTerrainFinite(vertices[index].y) ||
			!projectedTerrainFinite(vertices[index].z) ||
			!projectedTerrainFinite(vertices[index].u) ||
			!projectedTerrainFinite(vertices[index].v))
			return false;
	}
	for (index = 0; index < indexCount; ++index)
	{
		if (indices[index] >= vertexCount)
			return false;
	}
	return true;
}
