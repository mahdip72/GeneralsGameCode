/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "W3DDevice/Common/HeightMapTerrainPrepare.h"

#include <limits.h>
#include <string.h>

namespace
{

static bool heightMapTerrainPrepareCheckedMultiply(unsigned left,
	unsigned right, unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool heightMapTerrainPrepareCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
	{
		return false;
	}
	*result = left + right;
	return true;
}

static bool heightMapTerrainPrepareCheckedAddressRange(const void *pointer,
	unsigned bytes, size_t *begin, size_t *end)
{
	const size_t address = reinterpret_cast<size_t>(pointer);
	if (pointer == 0 || begin == 0 || end == 0 ||
		static_cast<size_t>(bytes) > static_cast<size_t>(-1) - address)
	{
		return false;
	}
	*begin = address;
	*end = address + static_cast<size_t>(bytes);
	return true;
}

static bool heightMapTerrainPrepareRangesOverlap(size_t firstBegin,
	size_t firstEnd, size_t secondBegin, size_t secondEnd)
{
	return firstBegin < secondEnd && secondBegin < firstEnd;
}

static bool heightMapTerrainPrepareDestinationOverlapsInput(
	const HeightMapTerrainSnapshot &snapshot, size_t destinationBegin,
	size_t destinationEnd)
{
	unsigned inputBytes;
	size_t inputBegin;
	size_t inputEnd;

	if (!heightMapTerrainPrepareCheckedAddressRange(&snapshot,
		static_cast<unsigned>(sizeof(snapshot)), &inputBegin, &inputEnd) ||
		heightMapTerrainPrepareRangesOverlap(destinationBegin, destinationEnd,
			inputBegin, inputEnd) ||
		!heightMapTerrainPrepareCheckedMultiply(snapshot.cellRowStrideBytes,
			snapshot.height, &inputBytes) ||
		!heightMapTerrainPrepareCheckedAddressRange(snapshot.cells, inputBytes,
			&inputBegin, &inputEnd) ||
		heightMapTerrainPrepareRangesOverlap(destinationBegin, destinationEnd,
			inputBegin, inputEnd))
	{
		return true;
	}

	if (snapshot.globalLightCount != 0)
	{
		if (!heightMapTerrainPrepareCheckedMultiply(snapshot.globalLightCount,
				static_cast<unsigned>(sizeof(HeightMapTerrainGlobalLight)),
				&inputBytes) ||
			!heightMapTerrainPrepareCheckedAddressRange(snapshot.globalLights,
				inputBytes, &inputBegin, &inputEnd) ||
			heightMapTerrainPrepareRangesOverlap(destinationBegin,
				destinationEnd, inputBegin, inputEnd))
		{
			return true;
		}
	}

	if (snapshot.sceneLightCount != 0)
	{
		if (!heightMapTerrainPrepareCheckedMultiply(snapshot.sceneLightCount,
				static_cast<unsigned>(sizeof(HeightMapTerrainSceneLight)),
				&inputBytes) ||
			!heightMapTerrainPrepareCheckedAddressRange(snapshot.sceneLights,
				inputBytes, &inputBegin, &inputEnd) ||
			heightMapTerrainPrepareRangesOverlap(destinationBegin,
				destinationEnd, inputBegin, inputEnd))
		{
			return true;
		}
	}

	return false;
}

static HeightMapTerrainCellInput *heightMapTerrainPrepareAllocateCells(
	unsigned count)
{
	HeightMapTerrainCellInput *value = 0;
	try
	{
		value = new HeightMapTerrainCellInput[count];
	}
	catch (...)
	{
		value = 0;
	}
	return value;
}

static HeightMapTerrainGlobalLight *heightMapTerrainPrepareAllocateGlobalLights(
	unsigned count)
{
	HeightMapTerrainGlobalLight *value = 0;
	if (count == 0)
		return 0;
	try
	{
		value = new HeightMapTerrainGlobalLight[count];
	}
	catch (...)
	{
		value = 0;
	}
	return value;
}

static HeightMapTerrainSceneLight *heightMapTerrainPrepareAllocateSceneLights(
	unsigned count)
{
	HeightMapTerrainSceneLight *value = 0;
	if (count == 0)
		return 0;
	try
	{
		value = new HeightMapTerrainSceneLight[count];
	}
	catch (...)
	{
		value = 0;
	}
	return value;
}

static HeightMapTerrainVertex *heightMapTerrainPrepareAllocateOutput(
	unsigned count)
{
	HeightMapTerrainVertex *value = 0;
	try
	{
		value = new HeightMapTerrainVertex[count];
	}
	catch (...)
	{
		value = 0;
	}
	return value;
}

static bool heightMapTerrainPrepareEligible(
	const HeightMapTerrainSnapshot &snapshot)
{
	return snapshot.height >= 2 && snapshot.cellCount >= 512;
}

} // namespace

bool ScatterPreparedHeightMapTerrainRows(
	const HeightMapTerrainSnapshot &snapshot,
	const HeightMapTerrainVertex *source, unsigned destinationFirstRow,
	unsigned destinationColumnOffsetBytes,
	unsigned destinationRowStrideBytes, void *backupDestination,
	unsigned backupCapacityBytes, void *hardwareDestination,
	unsigned hardwareCapacityBytes)
{
	unsigned rowBytes;
	unsigned rowSpan;
	unsigned lastRow;
	unsigned firstOffset;
	unsigned lastOffset;
	unsigned requiredEnd;
	unsigned sourceBytes;
	size_t sourceBegin;
	size_t sourceEnd;
	size_t backupBegin;
	size_t backupEnd;
	size_t hardwareBegin;
	size_t hardwareEnd;
	unsigned row;

	if (!ValidatePreparedHeightMapTerrainOutput(snapshot, source) ||
		backupDestination == 0 || hardwareDestination == 0 ||
		destinationColumnOffsetBytes %
			static_cast<unsigned>(sizeof(HeightMapTerrainVertex)) != 0 ||
		destinationRowStrideBytes %
			static_cast<unsigned>(sizeof(HeightMapTerrainVertex)) != 0 ||
		!heightMapTerrainPrepareCheckedMultiply(snapshot.width,
			static_cast<unsigned>(HEIGHTMAP_TERRAIN_VERTEX_COUNT) *
				static_cast<unsigned>(sizeof(HeightMapTerrainVertex)),
			&rowBytes) ||
		!heightMapTerrainPrepareCheckedAdd(destinationColumnOffsetBytes,
			rowBytes, &rowSpan) || rowSpan > destinationRowStrideBytes ||
		!heightMapTerrainPrepareCheckedAdd(destinationFirstRow,
			snapshot.height - 1, &lastRow) ||
		!heightMapTerrainPrepareCheckedMultiply(destinationFirstRow,
			destinationRowStrideBytes, &firstOffset) ||
		!heightMapTerrainPrepareCheckedAdd(firstOffset,
			destinationColumnOffsetBytes, &firstOffset) ||
		!heightMapTerrainPrepareCheckedMultiply(lastRow,
			destinationRowStrideBytes, &lastOffset) ||
		!heightMapTerrainPrepareCheckedAdd(lastOffset,
			destinationColumnOffsetBytes, &lastOffset) ||
		!heightMapTerrainPrepareCheckedAdd(lastOffset, rowBytes,
			&requiredEnd) || requiredEnd > backupCapacityBytes ||
		requiredEnd > hardwareCapacityBytes ||
		!heightMapTerrainPrepareCheckedMultiply(snapshot.outputStrideBytes,
			snapshot.height, &sourceBytes) ||
		!heightMapTerrainPrepareCheckedAddressRange(source, sourceBytes,
			&sourceBegin, &sourceEnd) ||
		!heightMapTerrainPrepareCheckedAddressRange(backupDestination,
			backupCapacityBytes, &backupBegin, &backupEnd) ||
		!heightMapTerrainPrepareCheckedAddressRange(hardwareDestination,
			hardwareCapacityBytes, &hardwareBegin, &hardwareEnd) ||
		heightMapTerrainPrepareRangesOverlap(sourceBegin, sourceEnd,
			backupBegin, backupEnd) ||
		heightMapTerrainPrepareRangesOverlap(sourceBegin, sourceEnd,
			hardwareBegin, hardwareEnd) ||
		heightMapTerrainPrepareRangesOverlap(backupBegin, backupEnd,
			hardwareBegin, hardwareEnd) ||
		heightMapTerrainPrepareDestinationOverlapsInput(snapshot, backupBegin,
			backupEnd) ||
		heightMapTerrainPrepareDestinationOverlapsInput(snapshot, hardwareBegin,
			hardwareEnd))
	{
		return false;
	}

	for (row = 0; row < snapshot.height; ++row)
	{
		const unsigned destinationOffset = firstOffset +
			row * destinationRowStrideBytes;
		const unsigned char *sourceRow =
			reinterpret_cast<const unsigned char *>(source) +
			row * snapshot.outputStrideBytes;
		memcpy(reinterpret_cast<unsigned char *>(backupDestination) +
			destinationOffset, sourceRow, rowBytes);
		memcpy(reinterpret_cast<unsigned char *>(hardwareDestination) +
			destinationOffset, sourceRow, rowBytes);
	}

	return true;
}

static bool heightMapTerrainCaptureHeight(const UnsignedByte *heightData,
	unsigned heightWidth, unsigned heightHeight, Int drawOriginX,
	Int drawOriginY, Int x, Int y, UnsignedByte *value)
{
	Int64 actualX;
	Int64 actualY;
	unsigned rowOffset;
	unsigned index;
	if (heightData == 0 || value == 0 || heightWidth == 0 ||
		heightHeight == 0)
	{
		return false;
	}
	actualX = static_cast<Int64>(x) + drawOriginX;
	actualY = static_cast<Int64>(y) + drawOriginY;
	if (actualX < 0 || actualY < 0 ||
		actualX >= static_cast<Int64>(heightWidth) ||
		actualY >= static_cast<Int64>(heightHeight))
	{
		return false;
	}
	if (!heightMapTerrainPrepareCheckedMultiply(
			static_cast<unsigned>(actualY), heightWidth, &rowOffset) ||
		!heightMapTerrainPrepareCheckedAdd(rowOffset,
			static_cast<unsigned>(actualX), &index))
	{
		return false;
	}
	*value = heightData[index];
	return true;
}

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
	UnsignedByte flip)
{
	UnsignedByte heights[HEIGHTMAP_TERRAIN_VERTEX_COUNT];
	Int leftRight[HEIGHTMAP_TERRAIN_VERTEX_COUNT];
	Int backForward[HEIGHTMAP_TERRAIN_VERTEX_COUNT];
	unsigned corner;
	if (u1 == 0 || v1 == 0 || u2 == 0 || v2 == 0 || alpha == 0 ||
		!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX, mapY, &heights[0]) ||
		!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX + 1, mapY, &heights[1]) ||
		!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX + 1, mapY + 1, &heights[2]) ||
		!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX, mapY + 1, &heights[3]))
	{
		return false;
	}

	/* Re-read the exact samples used by legacy updateVB.  Keeping each
	 * subtraction in promoted Int form prevents unsigned-byte wraparound. */
	{
		UnsignedByte valueA;
		UnsignedByte valueB;
		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX + 1, mapY, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, leftX, mapY, &valueB))
			return false;
		leftRight[0] = static_cast<Int>(valueA) - static_cast<Int>(valueB);
		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX, mapY + 1, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, mapX, topY, &valueB))
			return false;
		backForward[0] = static_cast<Int>(valueA) - static_cast<Int>(valueB);

		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, rightX, mapY, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, mapX, mapY, &valueB))
			return false;
		leftRight[1] = static_cast<Int>(valueA) - static_cast<Int>(valueB);
		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX + 1, mapY + 1, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, mapX + 1, topY, &valueB))
			return false;
		backForward[1] = static_cast<Int>(valueA) - static_cast<Int>(valueB);

		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, rightX, mapY + 1, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, mapX, mapY + 1, &valueB))
			return false;
		leftRight[2] = static_cast<Int>(valueA) - static_cast<Int>(valueB);
		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX + 1, bottomY, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, mapX + 1, mapY, &valueB))
			return false;
		backForward[2] = static_cast<Int>(valueA) - static_cast<Int>(valueB);

		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX + 1, mapY + 1, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, leftX, mapY + 1, &valueB))
			return false;
		leftRight[3] = static_cast<Int>(valueA) - static_cast<Int>(valueB);
		if (!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
			drawOriginX, drawOriginY, mapX, bottomY, &valueA) ||
			!heightMapTerrainCaptureHeight(heightData, heightWidth, heightHeight,
				drawOriginX, drawOriginY, mapX, mapY, &valueB))
			return false;
		backForward[3] = static_cast<Int>(valueA) - static_cast<Int>(valueB);
	}

	for (corner = 0; corner < HEIGHTMAP_TERRAIN_VERTEX_COUNT; ++corner)
	{
		cell.x[corner] = (static_cast<Real>(xCoord +
			(corner == 1 || corner == 2 ? 1 : 0) - borderSize)) * mapXYFactor;
		cell.y[corner] = (static_cast<Real>(yCoord +
			(corner >= 2 ? 1 : 0) - borderSize)) * mapXYFactor;
		cell.u1[corner] = u1[corner];
		cell.v1[corner] = v1[corner];
		cell.u2[corner] = u2[corner];
		cell.v2[corner] = v2[corner];
		cell.alpha[corner] = alpha[corner];
		cell.vertexHeight[corner] = heights[corner];
		cell.leftRightHeightDelta[corner] = leftRight[corner];
		cell.backForwardHeightDelta[corner] = backForward[corner];
	}
	cell.flip = flip;
	return true;
}

HeightMapTerrainBatch::HeightMapTerrainBatch()
	: m_cells(0), m_globalLights(0), m_sceneLights(0), m_output(0),
	  m_allocatedBytes(0), m_width(0), m_height(0),
	  m_cellRowStrideBytes(0), m_cellCount(0), m_globalLightCount(0),
	  m_sceneLightCount(0), m_outputStrideBytes(0),
	  m_outputCapacityBytes(0), m_complete(false)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

HeightMapTerrainBatch::~HeightMapTerrainBatch()
{
	reset();
}

bool HeightMapTerrainBatch::initialize(unsigned width, unsigned height,
	unsigned globalLightCount, unsigned sceneLightCount)
{
	unsigned cellCount;
	unsigned cellBytes;
	unsigned outputRowBytes;
	unsigned outputBytes;
	unsigned outputVertexCount;
	unsigned globalBytes;
	unsigned sceneBytes;
	unsigned totalBytes;
	HeightMapTerrainCellInput *cells = 0;
	HeightMapTerrainGlobalLight *globalLights = 0;
	HeightMapTerrainSceneLight *sceneLights = 0;
	HeightMapTerrainVertex *output = 0;

	if (isAllocated() || width == 0 || height == 0 ||
		globalLightCount > HEIGHTMAP_TERRAIN_MAX_GLOBAL_LIGHTS ||
		!heightMapTerrainPrepareCheckedMultiply(width, height, &cellCount) ||
		!heightMapTerrainPrepareCheckedMultiply(cellCount,
			static_cast<unsigned>(sizeof(HeightMapTerrainCellInput)),
			&cellBytes) ||
		!heightMapTerrainPrepareCheckedMultiply(width,
			static_cast<unsigned>(HEIGHTMAP_TERRAIN_VERTEX_COUNT) *
				static_cast<unsigned>(sizeof(HeightMapTerrainVertex)),
			&outputRowBytes) ||
		!heightMapTerrainPrepareCheckedMultiply(outputRowBytes, height,
			&outputBytes) ||
		!heightMapTerrainPrepareCheckedMultiply(cellCount,
			static_cast<unsigned>(HEIGHTMAP_TERRAIN_VERTEX_COUNT),
			&outputVertexCount) ||
		!heightMapTerrainPrepareCheckedMultiply(globalLightCount,
			static_cast<unsigned>(sizeof(HeightMapTerrainGlobalLight)),
			&globalBytes) ||
		!heightMapTerrainPrepareCheckedMultiply(sceneLightCount,
			static_cast<unsigned>(sizeof(HeightMapTerrainSceneLight)),
			&sceneBytes) ||
		!heightMapTerrainPrepareCheckedAdd(cellBytes, globalBytes,
			&totalBytes) ||
		!heightMapTerrainPrepareCheckedAdd(totalBytes, sceneBytes,
			&totalBytes) ||
		!heightMapTerrainPrepareCheckedAdd(totalBytes, outputBytes,
			&totalBytes) || totalBytes > MAX_BYTES)
	{
		return false;
	}

	cells = heightMapTerrainPrepareAllocateCells(cellCount);
	if (cells == 0)
		return false;
	globalLights = heightMapTerrainPrepareAllocateGlobalLights(globalLightCount);
	if (globalLightCount != 0 && globalLights == 0)
	{
		delete [] cells;
		return false;
	}
	sceneLights = heightMapTerrainPrepareAllocateSceneLights(sceneLightCount);
	if (sceneLightCount != 0 && sceneLights == 0)
	{
		delete [] sceneLights;
		delete [] globalLights;
		delete [] cells;
		return false;
	}
	output = heightMapTerrainPrepareAllocateOutput(outputVertexCount);
	if (output == 0)
	{
		delete [] sceneLights;
		delete [] globalLights;
		delete [] cells;
		return false;
	}

	memset(cells, 0, cellBytes);
	if (globalBytes != 0)
		memset(globalLights, 0, globalBytes);
	if (sceneBytes != 0)
		memset(sceneLights, 0, sceneBytes);
	/* IEEE all-bits-one floats are NaNs.  The owner validation step therefore
	 * rejects a fresh or partially prepared batch instead of publishing
	 * zero-initialized vertices that merely look finite. */
	memset(output, 0xFF, outputBytes);

	m_cells = cells;
	m_globalLights = globalLights;
	m_sceneLights = sceneLights;
	m_output = output;
	m_allocatedBytes = totalBytes;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
	m_snapshot.width = width;
	m_snapshot.height = height;
	m_snapshot.cellRowStrideBytes = width * sizeof(HeightMapTerrainCellInput);
	m_snapshot.cellCount = cellCount;
	m_snapshot.cells = m_cells;
	m_snapshot.globalLights = m_globalLights;
	m_snapshot.globalLightCount = globalLightCount;
	m_snapshot.sceneLights = m_sceneLights;
	m_snapshot.sceneLightCount = sceneLightCount;
	m_snapshot.mapXYFactor = 1.0f;
	m_snapshot.mapHeightScale = 1.0f;
	m_snapshot.outputStrideBytes = outputRowBytes;
	m_snapshot.outputCapacityBytes = outputBytes;
	m_snapshot.batchBytes = totalBytes;
	m_width = width;
	m_height = height;
	m_cellRowStrideBytes = m_snapshot.cellRowStrideBytes;
	m_cellCount = cellCount;
	m_globalLightCount = globalLightCount;
	m_sceneLightCount = sceneLightCount;
	m_outputStrideBytes = outputRowBytes;
	m_outputCapacityBytes = outputBytes;
	m_complete = false;
	return true;
}

void HeightMapTerrainBatch::reset()
{
	delete [] m_output;
	delete [] m_sceneLights;
	delete [] m_globalLights;
	delete [] m_cells;
	m_output = 0;
	m_sceneLights = 0;
	m_globalLights = 0;
	m_cells = 0;
	m_allocatedBytes = 0;
	m_width = 0;
	m_height = 0;
	m_cellRowStrideBytes = 0;
	m_cellCount = 0;
	m_globalLightCount = 0;
	m_sceneLightCount = 0;
	m_outputStrideBytes = 0;
	m_outputCapacityBytes = 0;
	m_complete = false;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

bool HeightMapTerrainBatch::isAllocated() const
{
	return m_allocatedBytes != 0 && m_cells != 0 && m_output != 0;
}

bool HeightMapTerrainBatch::storageMetadataMatches() const
{
	return m_snapshot.width == m_width &&
		m_snapshot.height == m_height &&
		m_snapshot.cellRowStrideBytes == m_cellRowStrideBytes &&
		m_snapshot.cellCount == m_cellCount &&
		m_snapshot.cells == m_cells &&
		m_snapshot.globalLightCount == m_globalLightCount &&
		m_snapshot.globalLights == m_globalLights &&
		m_snapshot.sceneLightCount == m_sceneLightCount &&
		m_snapshot.sceneLights == m_sceneLights &&
		m_snapshot.outputStrideBytes == m_outputStrideBytes &&
		m_snapshot.outputCapacityBytes == m_outputCapacityBytes &&
		m_snapshot.batchBytes == m_allocatedBytes;
}

bool HeightMapTerrainBatch::isComplete() const
{
	return m_complete && storageMetadataMatches();
}

bool HeightMapTerrainBatch::setParameters(Real mapXYFactor,
	Real mapHeightScale, const HeightMapTerrainRgb &terrainAmbient,
	UnsignedByte useDepthFade, Real depthFadeR, Real depthFadeG,
	Real depthFadeB, Real waterHeight)
{
	if (!isAllocated() || m_complete)
		return false;
	m_snapshot.mapXYFactor = mapXYFactor;
	m_snapshot.mapHeightScale = mapHeightScale;
	m_snapshot.terrainAmbient = terrainAmbient;
	m_snapshot.useDepthFade = useDepthFade;
	m_snapshot.depthFadeR = depthFadeR;
	m_snapshot.depthFadeG = depthFadeG;
	m_snapshot.depthFadeB = depthFadeB;
	m_snapshot.waterHeight = waterHeight;
	return true;
}

bool HeightMapTerrainBatch::markComplete()
{
	if (!isAllocated() || m_complete || !storageMetadataMatches())
		return false;
	m_complete = true;
	return true;
}

HeightMapTerrainRowWork::HeightMapTerrainRowWork(HeightMapTerrainBatch &batch)
	: m_snapshot(&batch.snapshot()), m_output(batch.output()),
	  m_inputValid(ValidateHeightMapTerrainInput(batch.snapshot(), batch.output()))
{
}

HeightMapTerrainRowWork::HeightMapTerrainRowWork(
	const HeightMapTerrainSnapshot &snapshot, HeightMapTerrainVertex *output)
	: m_snapshot(&snapshot), m_output(output),
	  m_inputValid(ValidateHeightMapTerrainInput(snapshot, output))
{
}

HeightMapTerrainRowWork::~HeightMapTerrainRowWork()
{
}

bool HeightMapTerrainRowWork::executeRows(unsigned rowBegin, unsigned rowEnd)
{
	return m_inputValid && m_snapshot != 0 && m_output != 0 &&
		PrepareValidatedHeightMapTerrainRows(*m_snapshot, m_output, rowBegin, rowEnd);
}

bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service, bool leaseAlreadyHeld,
	bool *ranParallel)
{
	const HeightMapTerrainSnapshot &snapshot = batch.snapshot();
	HeightMapTerrainRowWork work(batch);
	bool ranWorker = false;
	bool acquiredLease = false;

	if (ranParallel != 0)
		*ranParallel = false;
	if (!batch.isComplete() || snapshot.height == 0 || snapshot.width == 0)
		return false;

	if (heightMapTerrainPrepareEligible(snapshot) &&
		((leaseAlreadyHeld && service.hasLease() &&
			service.activeConsumer() == 4) ||
			(!leaseAlreadyHeld && service.tryAcquire(4))))
	{
		ranWorker = service.runRows(work, 0, snapshot.height, ranParallel);
		acquiredLease = !leaseAlreadyHeld;
	}
	if (acquiredLease)
		service.release(4);

	if (ranWorker)
	{
		return true;
	}

	/* Worker failures are recoverable only after the complete serial tile is
	 * produced; the owner never publishes a partial worker result. */
	return work.executeRows(0, snapshot.height);
}

bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service, bool leaseAlreadyHeld)
{
	return RunHeightMapTerrainBatch(batch, service, leaseAlreadyHeld, 0);
}

bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service)
{
	return RunHeightMapTerrainBatch(batch, service, false);
}

bool RunHeightMapTerrainBatch(HeightMapTerrainBatch &batch)
{
	return RunHeightMapTerrainBatch(batch, GetRadarTerrainPrepareService());
}

bool PrepareHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service)
{
	return RunHeightMapTerrainBatch(batch, service);
}

bool PrepareHeightMapTerrainBatch(HeightMapTerrainBatch &batch,
	RadarTerrainPrepareService &service, bool leaseAlreadyHeld)
{
	return RunHeightMapTerrainBatch(batch, service, leaseAlreadyHeld);
}

bool PrepareHeightMapTerrainBatch(HeightMapTerrainBatch &batch)
{
	return RunHeightMapTerrainBatch(batch);
}
