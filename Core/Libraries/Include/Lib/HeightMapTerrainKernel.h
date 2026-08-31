#pragma once

#include "Lib/BaseTypeCore.h"

/*
 * The kernel-facing vertex deliberately mirrors VertexFormatXYZDUV2 without
 * importing any DirectX or WW3D header.  Keep this POD field-for-field: the
 * owner copies these bytes only after the worker rows have joined.
 */
struct HeightMapTerrainVertex
{
	Real x;
	Real y;
	Real z;
	unsigned diffuse;
	Real u1;
	Real v1;
	Real u2;
	Real v2;
};

enum HeightMapTerrainSceneLightType
{
	HEIGHTMAP_TERRAIN_LIGHT_POINT = 0,
	HEIGHTMAP_TERRAIN_LIGHT_DIRECTIONAL = 1,
	HEIGHTMAP_TERRAIN_LIGHT_SPOT = 2
};

struct HeightMapTerrainRgb
{
	Real red;
	Real green;
	Real blue;
};

/*
 * One owner-captured cell.  The four coordinate/UV/alpha entries use the
 * legacy order top-left, top-right, bottom-right, bottom-left.  `flip` is
 * captured before the complete four-vertex FLIP_TRIANGLES rotation.
 */
struct HeightMapTerrainCellInput
{
	Real x[4];
	Real y[4];
	Real u1[4];
	Real v1[4];
	Real u2[4];
	Real v2[4];
	UnsignedByte alpha[4];
	UnsignedByte flip;
	/* Captured after the owner applies getX/YWithOrigin wrap/clamp. */
	UnsignedByte vertexHeight[4];
	Int leftRightHeightDelta[4];
	Int backForwardHeightDelta[4];
};

/* Global terrain light data is already reduced to the legacy light ray. */
struct HeightMapTerrainGlobalLight
{
	Real rayX;
	Real rayY;
	Real rayZ;
	HeightMapTerrainRgb diffuse;
};

/* Scene light data copied in exact iterator order before worker admission. */
struct HeightMapTerrainSceneLight
{
	unsigned type;
	Real positionX;
	Real positionY;
	Real positionZ;
	Real directionX;
	Real directionY;
	Real directionZ;
	double range;
	double midRange;
	HeightMapTerrainRgb diffuse;
	HeightMapTerrainRgb ambient;
};

/*
 * Immutable POD input for one rectangular cell tile.  The owner captures the
 * four already wrapped/clamped display heights and the four signed promoted
 * left/right and back/forward differences, so no worker-side map indexing or
 * shared halo is required.  The kernel applies mapHeightScale to these raw
 * values, matching the legacy updateVB path.
 * cellRowStrideBytes/outputStrideBytes are explicit so checked arithmetic can
 * reject malformed or overflowing batches before any output write.
 */
struct HeightMapTerrainSnapshot
{
	unsigned width;
	unsigned height;
	unsigned cellRowStrideBytes;
	unsigned cellCount;
	const HeightMapTerrainCellInput *cells;

	Real mapXYFactor;
	Real mapHeightScale;

	HeightMapTerrainRgb terrainAmbient;
	const HeightMapTerrainGlobalLight *globalLights;
	unsigned globalLightCount;
	const HeightMapTerrainSceneLight *sceneLights;
	unsigned sceneLightCount;

	UnsignedByte useDepthFade;
	Real depthFadeR;
	Real depthFadeG;
	Real depthFadeB;
	Real waterHeight;

	unsigned outputStrideBytes;
	unsigned outputCapacityBytes;
	unsigned batchBytes;
};

enum
{
	HEIGHTMAP_TERRAIN_VERTEX_COUNT = 4,
	HEIGHTMAP_TERRAIN_MAX_GLOBAL_LIGHTS = 3,
	HEIGHTMAP_TERRAIN_MAX_BATCH_BYTES = 8u * 1024u * 1024u
};

/* Task 4 seam: owner-only impassable/cliff masks apply after this post-flip
 * kernel result is joined; no diagnostic mutation crosses the worker API. */

/* Write only cells in the half-open row range [yBegin, yEnd). */
bool PrepareHeightMapTerrainRows(const HeightMapTerrainSnapshot &snapshot,
	HeightMapTerrainVertex *output, unsigned yBegin, unsigned yEnd);

/* Owner admission validates the full immutable tile once. The joined worker
 * entry still validates its range, but does not rescan other workers' cells. */
bool ValidateHeightMapTerrainInput(const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex *output);
bool PrepareValidatedHeightMapTerrainRows(const HeightMapTerrainSnapshot &snapshot,
	HeightMapTerrainVertex *output, unsigned yBegin, unsigned yEnd);

/* Validate the complete immutable input and every prepared output vertex
 * before the owner publishes any bytes to renderer-owned storage. */
bool ValidatePreparedHeightMapTerrainOutput(
	const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex *output);
