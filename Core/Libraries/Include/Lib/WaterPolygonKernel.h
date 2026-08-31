/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/BaseTypeCore.h"

/* The legacy trapezoid path clamps each edge to 50 cells.  Keep the limits in
 * the leaf kernel so a malformed owner snapshot cannot turn a frame into an
 * unbounded allocation or an index-buffer overflow. */
enum
{
	WATER_POLYGON_MAX_EDGE_CELLS = 50,
	WATER_POLYGON_MAX_VERTICES =
		(WATER_POLYGON_MAX_EDGE_CELLS + 1) * (WATER_POLYGON_MAX_EDGE_CELLS + 1),
	WATER_POLYGON_MAX_INDICES = WATER_POLYGON_MAX_EDGE_CELLS *
		WATER_POLYGON_MAX_EDGE_CELLS * 6,
	WATER_POLYGON_MIN_PARALLEL_CELLS = 512,
	WATER_POLYGON_FAST_SIN_TABLE_SIZE = 1024
};

struct WaterPolygonPoint
{
	Real x, y, z;
};

/* Field-for-field output for dynamic_fvf_type / VertexFormatXYZNDUV2.  The
 * owner copies this POD after all worker jobs have joined. */
struct WaterPolygonVertex
{
	Real x, y, z;
	Real nx, ny, nz;
	unsigned diffuse;
	Real u1, v1, u2, v2;
};

/*
 * All values are captured by the render owner before admission.  The sine
 * table is an immutable owner-captured table (normally WWMath's initialized
 * table); workers never resolve a game/global object or mutate it.
 */
struct WaterPolygonSnapshot
{
	WaterPolygonPoint origin;
	WaterPolygonPoint uVector;
	WaterPolygonPoint vVector;
	WaterPolygonPoint bilinear;
	unsigned uCount;
	unsigned vCount;
	unsigned rectangleCount;
	unsigned diffuse;
	unsigned featherAlpha;
	bool wavy;
	Real waterFactor;
	Real bumpSize;
	Real phaseBase;
	Real mapCoeff;
	Real amplitude;
	Real wobbleU;
	Real wobbleV;
	double wavyWobbleU;
	double wavyWobbleV;
	Real flatUScale;
	Real flatVScale;
	Real flatPhaseBase;
	Real flatMapCoeff;
	Real flatRowScale;
	Real flatColumnScale;
	Real flatSinScale;
	const Real *flatSinTable;
};

/* Ranges are half-open. Vertex ranges are in [0, uCount*vCount); index
 * ranges are in [0, rectangleCount), with six indices per rectangle. */
bool PrepareWaterPolygonVertices(const WaterPolygonSnapshot &snapshot,
	WaterPolygonVertex *output, unsigned begin, unsigned end);
bool PrepareWaterPolygonIndices(const WaterPolygonSnapshot &snapshot,
	unsigned short *output, unsigned begin, unsigned end);
