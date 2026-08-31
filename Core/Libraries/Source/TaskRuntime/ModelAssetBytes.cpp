/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ModelAssetBytes.h"
#include <cstdlib>
#include <cstring>
#include <new>

namespace rts
{
namespace
{
unsigned readLittleEndian32(const unsigned char *bytes)
{
	return static_cast<unsigned>(bytes[0]) |
		(static_cast<unsigned>(bytes[1]) << 8) |
		(static_cast<unsigned>(bytes[2]) << 16) |
		(static_cast<unsigned>(bytes[3]) << 24);
}

bool validateEnvelopes(const unsigned char *bytes, size_t size,
	const ResourceCancellation &cancellation)
{
	// ChunkIO's high size bit identifies nested chunks. Older unflagged
	// payloads stay opaque: this does not reinterpret model record formats.
	size_t ends[256];
	unsigned depth = 0;
	ends[0] = size;
	size_t offset = 0;
	for (;;)
	{
		if (cancellation.isCancelled()) return false;
		if (offset == ends[depth])
		{
			if (depth == 0) return true;
			--depth;
			continue;
		}
		if (offset > ends[depth] || ends[depth] - offset < 8) return false;
		const unsigned encodedSize = readLittleEndian32(bytes + offset + 4);
		offset += 8;
		const size_t payload = encodedSize & 0x7fffffffu;
		if (payload > ends[depth] - offset) return false;
		if ((encodedSize & 0x80000000u) && payload != 0)
		{
			// Match the legacy parser's bounded chunk stack, including the
			// unrepresented root envelope in this validator.
			if (depth >= 254) return false;
			ends[++depth] = offset + payload;
		}
		else offset += payload;
	}
}

bool sameTicket(const ResourceIoTicket &left, const ResourceIoTicket &right)
{
	return left.isValid() && left.id == right.id && left.generation == right.generation;
}
}

ModelAssetBytes::ModelAssetBytes() : m_bytes(0), m_size(0) {}
ModelAssetBytes::~ModelAssetBytes() { std::free(m_bytes); }

bool ModelAssetBytes::prepare(const unsigned char *bytes, size_t size, size_t &workspaceBytes)
{
	workspaceBytes = 0;
	if (!bytes || size < 8 || size > MAXIMUM_BYTES || m_bytes) return false;
	if ((readLittleEndian32(bytes + 4) & 0x7fffffffu) > size - 8) return false;
	// The operation owns a full copy, not a pointer into pipeline input which
	// take() destroys. Reserve that exact allocation before worker submission.
	workspaceBytes = size;
	return true;
}

bool ModelAssetBytes::decode(const unsigned char *bytes, size_t size,
	const ResourceCancellation &cancellation)
{
	if (!bytes || size < 8 || size > MAXIMUM_BYTES || m_bytes ||
		!validateEnvelopes(bytes, size, cancellation)) return false;
	// Use the private C heap explicitly, never the game's pooled allocator.
	unsigned char *copy = static_cast<unsigned char *>(std::malloc(size));
	if (!copy) return false;
	for (size_t offset = 0; offset < size;)
	{
		if (cancellation.isCancelled()) { std::free(copy); return false; }
		const size_t count = size - offset < 64u * 1024u ? size - offset : 64u * 1024u;
		std::memcpy(copy + offset, bytes + offset, count);
		offset += count;
	}
	if (cancellation.isCancelled()) { std::free(copy); return false; }
	m_bytes = copy;
	m_size = size;
	return true;
}

ModelAssetReadQueue::ModelAssetReadQueue() : m_reservedBytes(0), m_count(0) {}
ModelAssetReadQueue::~ModelAssetReadQueue()
{
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i) delete m_entries[i].bytes;
}

bool ModelAssetReadQueue::submit(ResourceIoPipeline &pipeline, ResourceIoSource *source,
	ResourceIoTicket &ticket)
{
	ticket = ResourceIoTicket();
	if (!source || source->size() < 8 || source->size() > ModelAssetBytes::MAXIMUM_BYTES ||
		source->size() > RETAINED_BYTE_BUDGET - m_reservedBytes || m_count == MAXIMUM_REQUESTS)
		return false;
	ModelAssetBytes *operation = new (std::nothrow) ModelAssetBytes;
	if (!operation) return false;
	if (!pipeline.submit(source, operation, JOB_PRIORITY_STREAMING, ticket))
	{
		delete operation;
		return false;
	}
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i)
	{
		Entry &entry = m_entries[i];
		if (entry.ticket.isValid()) continue;
		entry.ticket = ticket;
		entry.reserved = source->size();
		m_reservedBytes += entry.reserved;
		++m_count;
		break;
	}
	return true;
}

void ModelAssetReadQueue::pump(ResourceIoPipeline &pipeline)
{
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i)
	{
		Entry &entry = m_entries[i];
		if (!entry.ticket.isValid() || entry.status != RESOURCE_IO_PENDING) continue;
		ResourceDecodeOperation *operation = 0;
		if (!pipeline.take(entry.ticket, entry.status, operation)) continue;
		entry.bytes = static_cast<ModelAssetBytes *>(operation);
	}
}

bool ModelAssetReadQueue::contains(const ResourceIoTicket &ticket) const
{
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i)
		if (sameTicket(m_entries[i].ticket, ticket)) return true;
	return false;
}

bool ModelAssetReadQueue::take(const ResourceIoTicket &ticket, ResourceIoStatus &status,
	ModelAssetBytes *&bytes)
{
	bytes = 0;
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i)
	{
		Entry &entry = m_entries[i];
		if (!sameTicket(entry.ticket, ticket) || entry.status == RESOURCE_IO_PENDING) continue;
		status = entry.status;
		bytes = entry.bytes;
		m_reservedBytes -= entry.reserved;
		--m_count;
		entry = Entry();
		return true;
	}
	return false;
}

void ModelAssetReadQueue::discard(ResourceIoPipeline &pipeline)
{
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i)
		if (m_entries[i].ticket.isValid() && m_entries[i].status == RESOURCE_IO_PENDING)
			pipeline.cancel(m_entries[i].ticket);
	for (unsigned i = 0; i < MAXIMUM_REQUESTS; ++i)
	{
		Entry &entry = m_entries[i];
		if (entry.ticket.isValid() && entry.status == RESOURCE_IO_PENDING)
		{
			pipeline.wait(entry.ticket);
			ResourceDecodeOperation *operation = 0;
			pipeline.take(entry.ticket, entry.status, operation);
			delete operation;
		}
		delete entry.bytes;
		entry = Entry();
	}
	m_count = 0;
	m_reservedBytes = 0;
}
}
