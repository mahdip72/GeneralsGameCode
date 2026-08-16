/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "W3DDevice/Common/RadarOverlayPrepare.h"

#include <limits.h>
#include <new>
#include <string.h>

namespace
{

static bool radarOverlayPrepareCheckedMultiply(unsigned left,
	unsigned right, unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool radarOverlayPrepareCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
	{
		return false;
	}
	*result = left + right;
	return true;
}

static RadarObjectOverlayCommand *radarOverlayPrepareAllocateObjectCommands(
	unsigned count)
{
	RadarObjectOverlayCommand *commands = 0;
	try
	{
		commands = count == 0 ? 0 : new RadarObjectOverlayCommand[count];
	}
	catch (...)
	{
		commands = 0;
	}
	return commands;
}

static RadarShroudOverlayCommand *radarOverlayPrepareAllocateShroudCommands(
	unsigned count)
{
	RadarShroudOverlayCommand *commands = 0;
	try
	{
		commands = count == 0 ? 0 : new RadarShroudOverlayCommand[count];
	}
	catch (...)
	{
		commands = 0;
	}
	return commands;
}

static unsigned char *radarOverlayPrepareAllocateOutput(unsigned count)
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

static bool radarOverlayPrepareCheckLayout(unsigned width, unsigned height,
	unsigned formatCode, unsigned commandCapacity, unsigned commandBytes,
	unsigned *rowBytes, unsigned *outputBytes, unsigned maxBytes)
{
	const unsigned bytesPerPixel = RadarOverlayBytesPerPixel(formatCode);
	unsigned totalBytes;

	if (rowBytes == 0 || outputBytes == 0)
	{
		return false;
	}
	*rowBytes = 0;
	*outputBytes = 0;

	if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX ||
		bytesPerPixel == 0 ||
		!radarOverlayPrepareCheckedMultiply(width, bytesPerPixel, rowBytes) ||
		!radarOverlayPrepareCheckedMultiply(*rowBytes, height, outputBytes) ||
		!radarOverlayPrepareCheckedMultiply(commandCapacity, commandBytes,
			&totalBytes) ||
		!radarOverlayPrepareCheckedAdd(totalBytes, *outputBytes, &totalBytes) ||
		totalBytes > maxBytes)
	{
		return false;
	}

	return true;
}

static bool radarOverlayPrepareGetMaxCommandCapacity(unsigned width,
	unsigned height, unsigned formatCode, unsigned commandBytes,
	unsigned maxBytes, unsigned *commandCapacity)
{
	unsigned rowBytes;
	unsigned outputBytes;

	if (commandCapacity == 0 || commandBytes == 0 ||
		!radarOverlayPrepareCheckLayout(width, height, formatCode, 0,
			commandBytes, &rowBytes, &outputBytes, maxBytes) ||
		outputBytes > maxBytes)
	{
		return false;
	}

	*commandCapacity = (maxBytes - outputBytes) / commandBytes;
	return true;
}

} // namespace

RadarObjectOverlayBatch::RadarObjectOverlayBatch()
	: m_commands(0), m_output(0), m_initialized(false)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

RadarObjectOverlayBatch::~RadarObjectOverlayBatch()
{
	reset();
}

bool RadarObjectOverlayBatch::initialize(unsigned width, unsigned height,
	unsigned formatCode, unsigned commandCapacity)
{
	const unsigned commandBytes =
		static_cast<unsigned>(sizeof(RadarObjectOverlayCommand));
	unsigned rowBytes;
	unsigned outputBytes;

	/* A live batch is never silently replaced while a worker could borrow it. */
	if (m_initialized ||
		!radarOverlayPrepareCheckLayout(width, height, formatCode,
			commandCapacity, commandBytes, &rowBytes, &outputBytes,
			MAX_BYTES))
	{
		return false;
	}

	m_commands = radarOverlayPrepareAllocateObjectCommands(commandCapacity);
	if (commandCapacity != 0 && m_commands == 0)
	{
		return false;
	}

	m_output = radarOverlayPrepareAllocateOutput(outputBytes);
	if (m_output == 0)
	{
		delete [] m_commands;
		m_commands = 0;
		return false;
	}

	memset(m_output, 0, outputBytes);
	memset(&m_snapshot, 0, sizeof(m_snapshot));
	m_snapshot.width = width;
	m_snapshot.height = height;
	m_snapshot.bytesPerPixel = RadarOverlayBytesPerPixel(formatCode);
	m_snapshot.formatCode = formatCode;
	m_snapshot.rowBytes = rowBytes;
	m_snapshot.commandCount = 0;
	m_snapshot.commandCapacity = commandCapacity;
	m_snapshot.commands = m_commands;
	m_snapshot.output = m_output;
	m_initialized = true;
	return true;
}

void RadarObjectOverlayBatch::reset()
{
	delete [] m_output;
	delete [] m_commands;
	m_output = 0;
	m_commands = 0;
	m_initialized = false;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

bool RadarObjectOverlayBatch::append(
	const RadarObjectOverlayCommand &command)
{
	if (!m_initialized || m_snapshot.commandCount >=
		m_snapshot.commandCapacity || m_commands == 0)
	{
		return false;
	}
	m_commands[m_snapshot.commandCount] = command;
	++m_snapshot.commandCount;
	return true;
}

bool RadarObjectOverlayBatch::append(Int x, Int y, unsigned packedColor)
{
	RadarObjectOverlayCommand command;
	command.x = x;
	command.y = y;
	command.packedColor = packedColor;
	return append(command);
}

bool RadarObjectOverlayBatch::isAllocated() const
{
	return m_initialized && m_output != 0;
}

bool RadarObjectOverlayBatch::isFull() const
{
	return m_initialized && m_snapshot.commandCount ==
		m_snapshot.commandCapacity;
}

unsigned RadarObjectOverlayBatch::commandCount() const
{
	return m_snapshot.commandCount;
}

unsigned RadarObjectOverlayBatch::commandCapacity() const
{
	return m_snapshot.commandCapacity;
}

const RadarObjectOverlaySnapshot &RadarObjectOverlayBatch::snapshot() const
{
	return m_snapshot;
}

unsigned char *RadarObjectOverlayBatch::output()
{
	return m_output;
}

const unsigned char *RadarObjectOverlayBatch::output() const
{
	return m_output;
}

RadarShroudOverlayBatch::RadarShroudOverlayBatch()
	: m_commands(0), m_output(0), m_initialized(false)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

RadarShroudOverlayBatch::~RadarShroudOverlayBatch()
{
	reset();
}

bool RadarShroudOverlayBatch::initialize(unsigned width, unsigned height,
	unsigned formatCode)
{
	unsigned commandCapacity;
	if (!radarOverlayPrepareGetMaxCommandCapacity(width, height, formatCode,
		static_cast<unsigned>(sizeof(RadarShroudOverlayCommand)), MAX_BYTES,
		&commandCapacity))
	{
		return false;
	}

	return initialize(width, height, formatCode, commandCapacity);
}

bool RadarShroudOverlayBatch::initialize(unsigned width, unsigned height,
	unsigned formatCode, unsigned commandCapacity)
{
	const unsigned commandBytes =
		static_cast<unsigned>(sizeof(RadarShroudOverlayCommand));
	unsigned rowBytes;
	unsigned outputBytes;

	if (m_initialized ||
		!radarOverlayPrepareCheckLayout(width, height, formatCode,
			commandCapacity, commandBytes, &rowBytes, &outputBytes,
			MAX_BYTES))
	{
		return false;
	}

	m_commands = radarOverlayPrepareAllocateShroudCommands(commandCapacity);
	if (commandCapacity != 0 && m_commands == 0)
	{
		return false;
	}

	m_output = radarOverlayPrepareAllocateOutput(outputBytes);
	if (m_output == 0)
	{
		delete [] m_commands;
		m_commands = 0;
		return false;
	}

	memset(m_output, 0, outputBytes);
	memset(&m_snapshot, 0, sizeof(m_snapshot));
	m_snapshot.width = width;
	m_snapshot.height = height;
	m_snapshot.bytesPerPixel = RadarOverlayBytesPerPixel(formatCode);
	m_snapshot.formatCode = formatCode;
	m_snapshot.rowBytes = rowBytes;
	m_snapshot.commandCount = 0;
	m_snapshot.commandCapacity = commandCapacity;
	m_snapshot.commands = m_commands;
	m_snapshot.output = m_output;
	m_initialized = true;
	return true;
}

void RadarShroudOverlayBatch::reset()
{
	delete [] m_output;
	delete [] m_commands;
	m_output = 0;
	m_commands = 0;
	m_initialized = false;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

bool RadarShroudOverlayBatch::append(
	const RadarShroudOverlayCommand &command)
{
	if (!m_initialized || m_snapshot.commandCount >=
		m_snapshot.commandCapacity || m_commands == 0)
	{
		return false;
	}
	m_commands[m_snapshot.commandCount] = command;
	++m_snapshot.commandCount;
	return true;
}

bool RadarShroudOverlayBatch::append(Int minX, Int minY, Int maxX, Int maxY,
	unsigned packedColor)
{
	RadarShroudOverlayCommand command;
	command.minX = minX;
	command.minY = minY;
	command.maxX = maxX;
	command.maxY = maxY;
	command.packedColor = packedColor;
	return append(command);
}

void RadarShroudOverlayBatch::clearCommands()
{
	if (m_initialized)
	{
		m_snapshot.commandCount = 0;
	}
}

bool RadarShroudOverlayBatch::isAllocated() const
{
	return m_initialized && m_output != 0;
}

bool RadarShroudOverlayBatch::isFull() const
{
	return m_initialized && m_snapshot.commandCount ==
		m_snapshot.commandCapacity;
}

unsigned RadarShroudOverlayBatch::commandCount() const
{
	return m_snapshot.commandCount;
}

unsigned RadarShroudOverlayBatch::commandCapacity() const
{
	return m_snapshot.commandCapacity;
}

const RadarShroudOverlaySnapshot &RadarShroudOverlayBatch::snapshot() const
{
	return m_snapshot;
}

unsigned char *RadarShroudOverlayBatch::output()
{
	return m_output;
}

const unsigned char *RadarShroudOverlayBatch::output() const
{
	return m_output;
}

RadarObjectOverlayRowWork::RadarObjectOverlayRowWork(
	const RadarObjectOverlaySnapshot &snapshot)
	: m_snapshot(&snapshot)
{
}

RadarObjectOverlayRowWork::~RadarObjectOverlayRowWork()
{
}

bool RadarObjectOverlayRowWork::executeRows(unsigned rowBegin,
	unsigned rowEnd)
{
	return m_snapshot != 0 &&
		PackRadarObjectRows(*m_snapshot, rowBegin, rowEnd);
}

RadarShroudOverlayRowWork::RadarShroudOverlayRowWork(
	const RadarShroudOverlaySnapshot &snapshot)
	: m_snapshot(&snapshot)
{
}

RadarShroudOverlayRowWork::~RadarShroudOverlayRowWork()
{
}

bool RadarShroudOverlayRowWork::executeRows(unsigned rowBegin,
	unsigned rowEnd)
{
	return m_snapshot != 0 &&
		PackRadarShroudRows(*m_snapshot, rowBegin, rowEnd);
}

RadarOverlayPrepareLease::RadarOverlayPrepareLease(
	RadarTerrainPrepareService &service, unsigned consumerId)
	: m_service(&service), m_consumerId(consumerId), m_active(false)
{
}

RadarOverlayPrepareLease::~RadarOverlayPrepareLease()
{
	release();
}

bool RadarOverlayPrepareLease::runRows(RadarPrepareRowWork &work,
	unsigned rowBegin, unsigned rowEnd)
{
	if (!m_active)
	{
		m_active = m_service->tryAcquire(m_consumerId);
	}

	if (m_active && m_service->runRows(&work, rowBegin, rowEnd))
	{
		return true;
	}

	/* Do not retain a failed/denied lease while executing the serial oracle. */
	release();
	return work.executeRows(rowBegin, rowEnd);
}

void RadarOverlayPrepareLease::release()
{
	if (m_active)
	{
		m_service->release(m_consumerId);
		m_active = false;
	}
}

bool RunRadarObjectOverlayBatch(RadarObjectOverlayBatch &batch,
	RadarOverlayPrepareLease &lease)
{
	if (!batch.isAllocated() || batch.snapshot().height == 0)
	{
		return false;
	}

	RadarObjectOverlayRowWork work(batch.snapshot());
	return lease.runRows(work, 0, batch.snapshot().height);
}

bool RunRadarShroudOverlayBatch(RadarShroudOverlayBatch &batch,
	RadarOverlayPrepareLease &lease)
{
	if (!batch.isAllocated() || batch.snapshot().height == 0)
	{
		return false;
	}

	RadarShroudOverlayRowWork work(batch.snapshot());
	return lease.runRows(work, 0, batch.snapshot().height);
}
