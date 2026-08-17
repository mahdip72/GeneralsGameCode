/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/HeightMapTerrainKernel.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"

/*
 * Capture one owner-resolved cell from the map's immutable height storage.
 * The caller supplies all origin-adjusted/clamped neighbor indices and the
 * already-resolved UV/alpha data.  This seam deliberately has no WorldHeightMap,
 * D3D, light, or render-object dependency so its wrap/clamp and delta contract
 * can be tested independently of the game executable.
 */
bool CaptureHeightMapTerrainCellInput(HeightMapTerrainCellInput &cell,
	const UnsignedByte *heightData, unsigned heightWidth, unsigned heightHeight,
	Int drawOriginX, Int drawOriginY, Int mapX, Int mapY,
	Int leftX, Int rightX, Int topY, Int bottomY,
	Int xCoord, Int yCoord, Int borderSize, Real mapXYFactor,
	const Real u1[HEIGHTMAP_TERRAIN_VERTEX_COUNT],
	const Real v1[HEIGHTMAP_TERRAIN_VERTEX_COUNT],
	const Real u2[HEIGHTMAP_TERRAIN_VERTEX_COUNT],
	const Real v2[HEIGHTMAP_TERRAIN_VERTEX_COUNT],
	const UnsignedByte alpha[HEIGHTMAP_TERRAIN_VERTEX_COUNT],
	UnsignedByte flip);

/* Copy compact prepared rows into both renderer destinations only after all
 * source and destination ranges have been checked.  Offsets are expressed in
 * destination rows and bytes so this seam has no D3D vertex dependency.
 * Column offsets and row strides must be whole terrain-vertex multiples, and
 * the two destination ranges must be disjoint. */
bool ScatterPreparedHeightMapTerrainRows(
	const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex *source, unsigned destinationFirstRow,
	unsigned destinationColumnOffsetBytes,
	unsigned destinationRowStrideBytes, void *backupDestination,
	unsigned backupCapacityBytes, void *hardwareDestination,
	unsigned hardwareCapacityBytes);

/*
 * Owner-owned storage for one complete terrain tile.  Every pointer in the
 * kernel snapshot refers to one of these allocations and remains valid until
 * the synchronous row service call returns and the owner consumes output().
 */
class HeightMapTerrainBatch
{
public:
	enum { MAX_BYTES = HEIGHTMAP_TERRAIN_MAX_BATCH_BYTES };

	HeightMapTerrainBatch();
	~HeightMapTerrainBatch();

	/* Allocate all cells, lights, and output or leave the batch unallocated. */
	bool initialize(unsigned width, unsigned height,
		unsigned globalLightCount, unsigned sceneLightCount);
	void reset();
	/* Owner calls this only after all captured POD fields are populated. */
	bool markComplete();
	/* Set semantic lighting/capture state without exposing storage metadata. */
	bool setParameters(Real mapXYFactor, Real mapHeightScale,
		const HeightMapTerrainRgb &terrainAmbient, UnsignedByte useDepthFade,
		Real depthFadeR, Real depthFadeG, Real depthFadeB,
		Real waterHeight);

	bool isAllocated() const;
	bool isComplete() const;
	unsigned allocatedBytes() const { return m_allocatedBytes; }

	const HeightMapTerrainSnapshot &snapshot() const { return m_snapshot; }

	HeightMapTerrainCellInput *cells() { return m_cells; }
	const HeightMapTerrainCellInput *cells() const { return m_cells; }
	HeightMapTerrainGlobalLight *globalLights() { return m_globalLights; }
	const HeightMapTerrainGlobalLight *globalLights() const
	{
		return m_globalLights;
	}
	HeightMapTerrainSceneLight *sceneLights() { return m_sceneLights; }
	const HeightMapTerrainSceneLight *sceneLights() const
	{
		return m_sceneLights;
	}
	HeightMapTerrainVertex *output() { return m_output; }
	const HeightMapTerrainVertex *output() const { return m_output; }

private:
	HeightMapTerrainBatch(const HeightMapTerrainBatch &);
	HeightMapTerrainBatch &operator=(const HeightMapTerrainBatch &);

	HeightMapTerrainSnapshot m_snapshot;
	HeightMapTerrainCellInput *m_cells;
	HeightMapTerrainGlobalLight *m_globalLights;
	HeightMapTerrainSceneLight *m_sceneLights;
	HeightMapTerrainVertex *m_output;
	unsigned m_allocatedBytes;
	unsigned m_width;
	unsigned m_height;
	unsigned m_cellRowStrideBytes;
	unsigned m_cellCount;
	unsigned m_globalLightCount;
	unsigned m_sceneLightCount;
	unsigned m_outputStrideBytes;
	unsigned m_outputCapacityBytes;
	bool m_complete;

	bool storageMetadataMatches() const;
};

/* Synchronous worker adapter for the pure row kernel. */
class HeightMapTerrainRowWork : public RadarPrepareRowWork
{
public:
	explicit HeightMapTerrainRowWork(HeightMapTerrainBatch &batch);
	HeightMapTerrainRowWork(const HeightMapTerrainSnapshot &snapshot,
		HeightMapTerrainVertex *output);
	virtual ~HeightMapTerrainRowWork();
	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd);

private:
	HeightMapTerrainRowWork(const HeightMapTerrainRowWork &);
	HeightMapTerrainRowWork &operator=(const HeightMapTerrainRowWork &);

	const HeightMapTerrainSnapshot *m_snapshot;
	HeightMapTerrainVertex *m_output;
};

/*
 * Run one complete tile through consumer 4 of the existing shared service.
 * Small tiles, unavailable/denied services, and every failed worker attempt
 * use the complete serial kernel result.
 */
bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service);
/* Use this overload when the owner already holds consumer 4 across tiles. */
bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service, bool leaseAlreadyHeld);

/* The optional result distinguishes a successful worker run from the
 * complete serial-kernel recovery performed after worker admission fails.
 * The owner uses that distinction to retain the legacy updateVB fallback as
 * the authoritative runtime failure path. */
bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service, bool leaseAlreadyHeld,
	bool *ranParallel);
bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch);

/* Descriptive alias for owner call sites that name the operation explicitly. */
bool PrepareHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service);
bool PrepareHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service, bool leaseAlreadyHeld);
bool PrepareHeightMapTerrainBatch(HeightMapTerrainBatch &batch);
