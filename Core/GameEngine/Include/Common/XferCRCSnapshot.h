/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Common/XferCRC.h"
#include "Lib/DeterministicCrcSnapshot.h"

// Optional owner-only growth hook. Fixed-buffer callers omit it and preserve
// the original fail-closed overflow behavior.
typedef bool (*XferCRCSnapshotGrowFunction)(
	rts::DeterministicCrcSnapshotEncoder *encoder,
	size_t requiredStorageCapacity,
	void *context);

// Title-neutral owner-side tee: the unchanged XferCRC calculation remains the
// authoritative serial oracle while one selected canonical partition is copied
// into a pointer-free worker snapshot. Snapshot overflow never changes the
// serial calculation; it only makes endSnapshotPartition fail closed.
class XferCRCSnapshot : public XferCRC
{
public:
	XferCRCSnapshot();

	bool beginSnapshotPartition(
		rts::DeterministicCrcSnapshotEncoder *encoder,
		void *storage,
		size_t storageCapacity);
	bool beginSnapshotPartition(
		rts::DeterministicCrcSnapshotEncoder *encoder,
		void *storage,
		size_t storageCapacity,
		XferCRCSnapshotGrowFunction grow,
		void *growContext);
	bool endSnapshotPartition(
		uint64_t expectedPayloadByteCount,
		const void **immutableInput,
		size_t *immutableInputBytes);
	bool snapshotPartitionActive() const;
	bool snapshotPartitionFailed() const;

protected:
	virtual void xferImplementation(void *data, Int dataSize) override;

private:
	XferCRCSnapshot(const XferCRCSnapshot &);
	XferCRCSnapshot &operator=(const XferCRCSnapshot &);

	rts::DeterministicCrcSnapshotEncoder *m_snapshotEncoder;
	XferCRCSnapshotGrowFunction m_snapshotGrow;
	void *m_snapshotGrowContext;
	bool m_snapshotPartitionFailed;
};
