/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Lib/WaterPolygonKernel.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

namespace
{
bool waterPolygonFinite(Real value)
{
	return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

bool waterPolygonPointFinite(const WaterPolygonPoint &point)
{
	return waterPolygonFinite(point.x) && waterPolygonFinite(point.y) &&
		waterPolygonFinite(point.z);
}

bool waterPolygonDisjoint(const void *input, size_t inputBytes,
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

bool waterPolygonSnapshotValid(const WaterPolygonSnapshot &snapshot)
{
	if (snapshot.uCount < 2 || snapshot.vCount < 2 ||
		snapshot.uCount > WATER_POLYGON_MAX_EDGE_CELLS + 1 ||
		snapshot.vCount > WATER_POLYGON_MAX_EDGE_CELLS + 1)
		return false;
	const unsigned cellsX = snapshot.uCount - 1;
	const unsigned cellsY = snapshot.vCount - 1;
	if (cellsX > WATER_POLYGON_MAX_EDGE_CELLS ||
		cellsY > WATER_POLYGON_MAX_EDGE_CELLS)
		return false;
	const unsigned rectangles = cellsX * cellsY;
	if (snapshot.rectangleCount != rectangles ||
		snapshot.uCount > WATER_POLYGON_MAX_VERTICES / snapshot.vCount ||
		snapshot.rectangleCount > WATER_POLYGON_MAX_INDICES / 6)
		return false;
	if (!waterPolygonPointFinite(snapshot.origin) ||
		!waterPolygonPointFinite(snapshot.uVector) ||
		!waterPolygonPointFinite(snapshot.vVector) ||
		!waterPolygonPointFinite(snapshot.bilinear))
		return false;
	if (!waterPolygonFinite(snapshot.waterFactor) ||
		!waterPolygonFinite(snapshot.bumpSize) ||
		!waterPolygonFinite(snapshot.phaseBase) ||
		!waterPolygonFinite(snapshot.mapCoeff) ||
		!waterPolygonFinite(snapshot.amplitude) ||
		!waterPolygonFinite(snapshot.wobbleU) ||
		!waterPolygonFinite(snapshot.wobbleV))
		return false;
	if (snapshot.wavyWobbleU != snapshot.wavyWobbleU ||
		snapshot.wavyWobbleU < -DBL_MAX || snapshot.wavyWobbleU > DBL_MAX ||
		snapshot.wavyWobbleV != snapshot.wavyWobbleV ||
		snapshot.wavyWobbleV < -DBL_MAX || snapshot.wavyWobbleV > DBL_MAX)
		return false;
	if (snapshot.waterFactor == 0.0f || snapshot.bumpSize == 0.0f ||
		snapshot.featherAlpha > 255)
		return false;
	if (snapshot.wavy)
	{
		return true;
	}
	return waterPolygonFinite(snapshot.flatUScale) &&
		waterPolygonFinite(snapshot.flatVScale) &&
		waterPolygonFinite(snapshot.flatPhaseBase) &&
		waterPolygonFinite(snapshot.flatMapCoeff) &&
		waterPolygonFinite(snapshot.flatRowScale) &&
		waterPolygonFinite(snapshot.flatColumnScale) &&
		waterPolygonFinite(snapshot.flatSinScale) &&
		snapshot.flatSinScale != 0.0f && snapshot.flatSinTable != 0;
}

/* WWMath::Fast_Sin uses a 1024-entry linear interpolation table.  Keep its
 * scalar evaluation order here while taking the table itself from the owner
 * snapshot; this avoids a worker read of WWMath's mutable global state. */
Real waterPolygonFastSin(Real value, const WaterPolygonSnapshot &snapshot)
{
	value *= snapshot.flatSinScale;
	const int index0 = static_cast<int>(floor(value));
	const int index1 = index0 + 1;
	const Real fraction = value - static_cast<Real>(index0);
	const unsigned tableIndex0 = static_cast<unsigned>(index0) &
		(WATER_POLYGON_FAST_SIN_TABLE_SIZE - 1);
	const unsigned tableIndex1 = static_cast<unsigned>(index1) &
		(WATER_POLYGON_FAST_SIN_TABLE_SIZE - 1);
	return (1.0f - fraction) * snapshot.flatSinTable[tableIndex0] +
		fraction * snapshot.flatSinTable[tableIndex1];
}

void waterPolygonPosition(const WaterPolygonSnapshot &snapshot,
	unsigned row, unsigned column, Real &x, Real &y, Real &z)
{
	Real du;
	Real dv;
	if (snapshot.wavy)
	{
		dv = static_cast<Real>(row);
		dv /= (snapshot.vCount - 1);
		du = static_cast<Real>(column);
		du /= (snapshot.uCount - 1);
	}
	else
	{
		dv = static_cast<Real>(row) * snapshot.flatRowScale;
		du = static_cast<Real>(column) * snapshot.flatColumnScale;
	}

	x = snapshot.origin.x;
	y = snapshot.origin.y;
	z = snapshot.origin.z;
	x += snapshot.uVector.x * du;
	y += snapshot.uVector.y * du;
	z += snapshot.uVector.z * du;
	x += snapshot.vVector.x * dv;
	y += snapshot.vVector.y * dv;
	z += snapshot.vVector.z * dv;
	x += (dv) * (du) * snapshot.bilinear.x;
	y += (dv) * (du) * snapshot.bilinear.y;
	z += (dv) * (du) * snapshot.bilinear.z;
}
}

bool PrepareWaterPolygonVertices(const WaterPolygonSnapshot &snapshot,
	WaterPolygonVertex *output, unsigned begin, unsigned end)
{
	if (!waterPolygonSnapshotValid(snapshot))
		return false;
	const unsigned vertexCount = snapshot.uCount * snapshot.vCount;
	if (begin > end || end > vertexCount)
		return false;
	if (begin == end)
		return true;
	if (output == 0)
		return false;
	WaterPolygonVertex *const first = output + begin;
	if (!waterPolygonDisjoint(&snapshot, sizeof(snapshot), first,
		(end - begin) * sizeof(WaterPolygonVertex)))
		return false;
	if (!snapshot.wavy && !waterPolygonDisjoint(snapshot.flatSinTable,
		WATER_POLYGON_FAST_SIN_TABLE_SIZE * sizeof(Real), first,
		(end - begin) * sizeof(WaterPolygonVertex)))
		return false;

	for (unsigned index = begin; index < end; ++index)
	{
		const unsigned row = index / snapshot.uCount;
		const unsigned column = index % snapshot.uCount;
		Real x, y, z;
		waterPolygonPosition(snapshot, row, column, x, y, z);
		WaterPolygonVertex &vertex = output[index];
		vertex.x = x;
		vertex.y = y;
		if (snapshot.wavy)
		{
			const Real phase = snapshot.phaseBase + x * snapshot.mapCoeff;
			const Real wave = (sin(phase) - 1.0f) * snapshot.amplitude;
			vertex.z = z + wave;
			vertex.diffuse = (snapshot.diffuse & 0x00ffffffu) |
				((snapshot.featherAlpha & 0xffu) << 24);
			vertex.u1 = (x / snapshot.waterFactor) +
				snapshot.wavyWobbleU * wave;
			vertex.v1 = (y / snapshot.waterFactor) +
				snapshot.wavyWobbleV * wave;
		}
		else
		{
			vertex.z = z;
			vertex.diffuse = snapshot.diffuse;
			vertex.u1 = x * snapshot.flatUScale + snapshot.wobbleU *
				waterPolygonFastSin(snapshot.flatPhaseBase + x * snapshot.flatMapCoeff,
					snapshot);
			vertex.v1 = y * snapshot.flatVScale + snapshot.wobbleV *
				waterPolygonFastSin(snapshot.flatPhaseBase + y * snapshot.flatMapCoeff,
					snapshot);
		}
		vertex.u2 = x / snapshot.bumpSize;
		if (snapshot.wavy)
			vertex.v2 = y / snapshot.bumpSize + 0.3f * x / snapshot.bumpSize;
		else
			vertex.v2 = (y + 0.3f * x) / snapshot.bumpSize;
		vertex.nx = 0;
		vertex.ny = 0;
		vertex.nz = 1.0f;
	}
	return true;
}

bool PrepareWaterPolygonIndices(const WaterPolygonSnapshot &snapshot,
	unsigned short *output, unsigned begin, unsigned end)
{
	if (!waterPolygonSnapshotValid(snapshot))
		return false;
	if (begin > end || end > snapshot.rectangleCount)
		return false;
	if (begin == end)
		return true;
	if (output == 0)
		return false;
	unsigned short *const first = output + begin * 6;
	if (!waterPolygonDisjoint(&snapshot, sizeof(snapshot), first,
		(end - begin) * 6 * sizeof(unsigned short)))
		return false;

	const unsigned cellsX = snapshot.uCount - 1;
	for (unsigned cell = begin; cell < end; ++cell)
	{
		const unsigned row = cell / cellsX;
		const unsigned column = cell % cellsX;
		const unsigned rowStart = row * snapshot.uCount;
		const unsigned nextRowStart = (row + 1) * snapshot.uCount;
		unsigned short *const indices = output + cell * 6;
		indices[0] = static_cast<unsigned short>(rowStart + column);
		indices[1] = static_cast<unsigned short>(nextRowStart + column + 1);
		indices[2] = static_cast<unsigned short>(nextRowStart + column);
		indices[3] = static_cast<unsigned short>(rowStart + column);
		indices[4] = static_cast<unsigned short>(rowStart + column + 1);
		indices[5] = static_cast<unsigned short>(nextRowStart + column + 1);
	}
	return true;
}
