/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "W3DDevice/Common/RadarTerrainPrepare.h"

#include <limits.h>
#include <string.h>

static bool radarTerrainCheckedMultiply(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool radarTerrainCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
	{
		return false;
	}
	*result = left + right;
	return true;
}

static RadarTerrainCellInput *radarTerrainAllocateCells(unsigned count)
{
	RadarTerrainCellInput *cells = 0;
	try
	{
		cells = new RadarTerrainCellInput[count];
	}
	catch (...)
	{
		cells = 0;
	}
	return cells;
}

static unsigned char *radarTerrainAllocateOutput(unsigned count)
{
	unsigned char *output = 0;
	try
	{
		output = new unsigned char[count];
	}
	catch (...)
	{
		output = 0;
	}
	return output;
}

RadarTerrainBatch::RadarTerrainBatch() : m_cells(0), m_output(0),
	m_complete(false)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

RadarTerrainBatch::~RadarTerrainBatch()
{
	reset();
}

bool RadarTerrainBatch::initialize(unsigned width, unsigned height,
	unsigned formatCode)
{
	const unsigned bytesPerPixel = RadarTerrainBytesPerPixel(formatCode);
	unsigned cellCount;
	unsigned cellBytes;
	unsigned rowBytes;
	unsigned outputBytes;
	unsigned totalBytes;

	/* Refuse to replace live owner storage. */
	if (isAllocated() || width == 0 || height == 0 || bytesPerPixel == 0)
	{
		return false;
	}

	if (!radarTerrainCheckedMultiply(width, height, &cellCount) ||
		!radarTerrainCheckedMultiply(cellCount,
			static_cast<unsigned>(sizeof(RadarTerrainCellInput)), &cellBytes) ||
		!radarTerrainCheckedMultiply(width, bytesPerPixel, &rowBytes) ||
		!radarTerrainCheckedMultiply(rowBytes, height, &outputBytes) ||
		!radarTerrainCheckedAdd(cellBytes, outputBytes, &totalBytes) ||
		totalBytes > MAX_BYTES)
	{
		return false;
	}

	m_cells = radarTerrainAllocateCells(cellCount);
	if (m_cells == 0)
	{
		return false;
	}
	memset(m_cells, 0, cellBytes);

	m_output = radarTerrainAllocateOutput(outputBytes);
	if (m_output == 0)
	{
		reset();
		return false;
	}
	memset(m_output, 0, outputBytes);

	memset(&m_snapshot, 0, sizeof(m_snapshot));
	m_snapshot.width = width;
	m_snapshot.height = height;
	m_snapshot.bytesPerPixel = bytesPerPixel;
	m_snapshot.formatCode = formatCode;
	m_snapshot.rowBytes = rowBytes;
	m_snapshot.cells = m_cells;
	m_complete = false;
	return true;
}

void RadarTerrainBatch::reset()
{
	delete [] m_output;
	delete [] m_cells;
	m_output = 0;
	m_cells = 0;
	m_complete = false;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

bool RadarTerrainBatch::isAllocated() const
{
	return m_cells != 0 && m_output != 0;
}

