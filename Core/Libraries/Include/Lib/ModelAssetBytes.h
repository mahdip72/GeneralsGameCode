/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/ResourceIoPipeline.h"

namespace rts
{
// Only byte envelopes are checked here. ChunkLoadClass, pooled asset objects,
// material resolution and prototype publication remain on the game owner.
class ModelAssetBytes : public ResourceDecodeOperation
{
public:
	enum { MAXIMUM_BYTES = 8 * 1024 * 1024 };
	ModelAssetBytes();
	~ModelAssetBytes();
	bool prepare(const unsigned char *bytes, size_t size, size_t &workspaceBytes);
	bool decode(const unsigned char *bytes, size_t size,
		const ResourceCancellation &cancellation);
	unsigned char *data() const { return m_bytes; }
	size_t size() const { return m_size; }
private:
	ModelAssetBytes(const ModelAssetBytes &);
	ModelAssetBytes &operator=(const ModelAssetBytes &);
	unsigned char *m_bytes;
	size_t m_size;
};

// This is owner bookkeeping, not another I/O owner or a compute queue. Taking
// completed tickets promptly releases the shared pipeline's budgets even when
// texture loading waits before the model's FIFO owner-publication point.
class ModelAssetReadQueue
{
public:
	enum { MAXIMUM_REQUESTS = 16, RETAINED_BYTE_BUDGET = 16 * 1024 * 1024 };
	ModelAssetReadQueue();
	~ModelAssetReadQueue();
	// The source transfers to the shared pipeline only on success.
	bool submit(ResourceIoPipeline &pipeline, ResourceIoSource *source,
		ResourceIoTicket &ticket);
	void pump(ResourceIoPipeline &pipeline);
	bool contains(const ResourceIoTicket &ticket) const;
	// Consume/delete the result before admitting further model requests. Its
	// copy remains valid after the shared pipeline has destroyed input bytes.
	bool take(const ResourceIoTicket &ticket, ResourceIoStatus &status,
		ModelAssetBytes *&bytes);
	void discard(ResourceIoPipeline &pipeline);
	size_t retainedBytes() const { return m_reservedBytes; }
	unsigned pendingRequests() const { return m_count; }
private:
	struct Entry
	{
		Entry() : reserved(0), status(RESOURCE_IO_PENDING), bytes(0) {}
		ResourceIoTicket ticket;
		size_t reserved;
		ResourceIoStatus status;
		ModelAssetBytes *bytes;
	};
	ModelAssetReadQueue(const ModelAssetReadQueue &);
	ModelAssetReadQueue &operator=(const ModelAssetReadQueue &);
	Entry m_entries[MAXIMUM_REQUESTS];
	size_t m_reservedBytes;
	unsigned m_count;
};
}
