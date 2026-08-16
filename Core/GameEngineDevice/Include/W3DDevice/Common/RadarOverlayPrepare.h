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

#include "Lib/RadarOverlayKernel.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"

/*
 * Owner-side storage for one complete object-overlay preparation batch.
 *
 * The command array and tight output buffer are owned by this object and are
 * borrowed by the D3D-free serial/row kernels only while the owner keeps the
 * batch alive.  No live engine, surface, texture, or task pointer is stored.
 */
class RadarObjectOverlayBatch
{
public:
	/* Commands plus output are bounded together for one owner batch. */
	enum { MAX_BYTES = 2u * 1024u * 1024u };

	RadarObjectOverlayBatch();
	~RadarObjectOverlayBatch();

	/* Allocate an empty command array and tight output for one format. */
	bool initialize(unsigned width, unsigned height, unsigned formatCode,
		unsigned commandCapacity);
	void reset();

	/* Append only complete POD commands; the array never grows implicitly. */
	bool append(const RadarObjectOverlayCommand &command);
	bool append(Int x, Int y, unsigned packedColor);

	bool isAllocated() const;
	bool isFull() const;
	unsigned commandCount() const;
	unsigned commandCapacity() const;

	const RadarObjectOverlaySnapshot &snapshot() const;
	unsigned char *output();
	const unsigned char *output() const;

private:
	RadarObjectOverlayBatch(const RadarObjectOverlayBatch &);
	RadarObjectOverlayBatch &operator=(const RadarObjectOverlayBatch &);

	RadarObjectOverlaySnapshot m_snapshot;
	RadarObjectOverlayCommand *m_commands;
	unsigned char *m_output;
	bool m_initialized;
};

/* Owner-side storage for one complete ordered shroud-overlay batch. */
class RadarShroudOverlayBatch
{
public:
	enum { MAX_BYTES = 2u * 1024u * 1024u };

	RadarShroudOverlayBatch();
	~RadarShroudOverlayBatch();

	bool initialize(unsigned width, unsigned height, unsigned formatCode,
		unsigned commandCapacity);
	void reset();

	bool append(const RadarShroudOverlayCommand &command);
	bool append(Int minX, Int minY, Int maxX, Int maxY,
		unsigned packedColor);

	bool isAllocated() const;
	bool isFull() const;
	unsigned commandCount() const;
	unsigned commandCapacity() const;

	const RadarShroudOverlaySnapshot &snapshot() const;
	unsigned char *output();
	const unsigned char *output() const;

private:
	RadarShroudOverlayBatch(const RadarShroudOverlayBatch &);
	RadarShroudOverlayBatch &operator=(const RadarShroudOverlayBatch &);

	RadarShroudOverlaySnapshot m_snapshot;
	RadarShroudOverlayCommand *m_commands;
	unsigned char *m_output;
	bool m_initialized;
};

/*
 * Immutable row adapters.  They retain only an owner-owned POD snapshot and
 * route worker execution directly to the corresponding pure kernel.
 */
class RadarObjectOverlayRowWork : public RadarPrepareRowWork
{
public:
	explicit RadarObjectOverlayRowWork(
		const RadarObjectOverlaySnapshot &snapshot);
	virtual ~RadarObjectOverlayRowWork();
	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd);

private:
	RadarObjectOverlayRowWork(const RadarObjectOverlayRowWork &);
	RadarObjectOverlayRowWork &operator=(const RadarObjectOverlayRowWork &);

	const RadarObjectOverlaySnapshot *m_snapshot;
};

class RadarShroudOverlayRowWork : public RadarPrepareRowWork
{
public:
	explicit RadarShroudOverlayRowWork(
		const RadarShroudOverlaySnapshot &snapshot);
	virtual ~RadarShroudOverlayRowWork();
	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd);

private:
	RadarShroudOverlayRowWork(const RadarShroudOverlayRowWork &);
	RadarShroudOverlayRowWork &operator=(const RadarShroudOverlayRowWork &);

	const RadarShroudOverlaySnapshot *m_snapshot;
};

/*
 * Run one owner-owned row batch through an injected existing service.  Lease
 * denial, runtime/task failure, and submission failure all execute the same
 * complete serial row operation before returning to the caller.
 */
bool RunRadarOverlayRows(RadarPrepareRowWork &work,
	RadarTerrainPrepareService &service, unsigned consumerId,
	unsigned rowBegin, unsigned rowEnd);

/* Full-height object and shroud helpers used by the owner integration. */
bool RunRadarObjectOverlayBatch(RadarObjectOverlayBatch &batch,
	RadarTerrainPrepareService &service, unsigned consumerId);

bool RunRadarShroudOverlayBatch(RadarShroudOverlayBatch &batch,
	RadarTerrainPrepareService &service, unsigned consumerId);
