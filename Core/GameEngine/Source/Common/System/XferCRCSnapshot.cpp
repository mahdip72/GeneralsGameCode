/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "PreRTS.h" // This must go first in EVERY cpp file in the GameEngine

#include "Common/XferCRCSnapshot.h"

XferCRCSnapshot::XferCRCSnapshot()
	: m_snapshotEncoder(0),
	  m_snapshotGrow(0),
	  m_snapshotGrowContext(0),
	  m_snapshotPartitionFailed(false)
{
}

bool XferCRCSnapshot::beginSnapshotPartition(
	rts::DeterministicCrcSnapshotEncoder *encoder,
	void *storage,
	size_t storageCapacity)
{
	return beginSnapshotPartition(encoder, storage, storageCapacity, 0, 0);
}

bool XferCRCSnapshot::beginSnapshotPartition(
	rts::DeterministicCrcSnapshotEncoder *encoder,
	void *storage,
	size_t storageCapacity,
	XferCRCSnapshotGrowFunction grow,
	void *growContext)
{
	if (m_snapshotEncoder != 0 || encoder == 0 ||
		!encoder->begin(storage, storageCapacity))
	{
		return false;
	}
	m_snapshotEncoder = encoder;
	m_snapshotGrow = grow;
	m_snapshotGrowContext = growContext;
	m_snapshotPartitionFailed = false;
	return true;
}

bool XferCRCSnapshot::endSnapshotPartition(
	uint64_t expectedPayloadByteCount,
	const void **immutableInput,
	size_t *immutableInputBytes)
{
	if (immutableInput != 0) *immutableInput = 0;
	if (immutableInputBytes != 0) *immutableInputBytes = 0U;
	if (m_snapshotEncoder == 0 || immutableInput == 0 ||
		immutableInputBytes == 0)
	{
		return false;
	}

	rts::DeterministicCrcSnapshotEncoder *encoder = m_snapshotEncoder;
	const bool snapshotSucceeded = !m_snapshotPartitionFailed &&
		encoder->seal(expectedPayloadByteCount, immutableInput,
			immutableInputBytes);
	m_snapshotEncoder = 0;
	m_snapshotGrow = 0;
	m_snapshotGrowContext = 0;
	m_snapshotPartitionFailed = !snapshotSucceeded;
	return snapshotSucceeded;
}

bool XferCRCSnapshot::snapshotPartitionActive() const
{
	return m_snapshotEncoder != 0;
}

bool XferCRCSnapshot::snapshotPartitionFailed() const
{
	return m_snapshotPartitionFailed;
}

void XferCRCSnapshot::xferImplementation(void *data, Int dataSize)
{
	XferCRC::xferImplementation(data, dataSize);
	if (m_snapshotEncoder == 0 || m_snapshotPartitionFailed) return;

	// XferCRC treats a null input as a zero-byte event. Preserve that effective
	// event rather than copying the caller's nominal byte count from a null.
	const size_t byteCount = data != 0 && dataSize > 0 ?
		static_cast<size_t>(dataSize) : 0U;
	size_t requiredStorageCapacity = 0U;
	if (!m_snapshotEncoder->requiredStorageCapacityForXferEvent(byteCount,
			&requiredStorageCapacity) ||
		(requiredStorageCapacity > m_snapshotEncoder->storageCapacity() &&
		 (m_snapshotGrow == 0 ||
		  !m_snapshotGrow(m_snapshotEncoder, requiredStorageCapacity,
			m_snapshotGrowContext) ||
		  requiredStorageCapacity > m_snapshotEncoder->storageCapacity())))
	{
		m_snapshotPartitionFailed = true;
		return;
	}
	if (!m_snapshotEncoder->appendXferEvent(data, byteCount))
		m_snapshotPartitionFailed = true;
}
