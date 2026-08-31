/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"
#include <stddef.h>

namespace rts
{
// A source is created/resolved on the game owner. Only its private read cursor
// is used by the I/O owner; never implement this with a game File/ArchiveFile.
class ResourceIoSource
{
public:
	virtual ~ResourceIoSource();
	virtual size_t size() const = 0;
	virtual int read(size_t offset, void *destination, unsigned bytes) = 0;
	static ResourceIoSource *openFileRange(const char *path,
		JobMetricCounter offset, JobMetricCounter bytes);
};

class ResourceCancellation
{
public:
	bool isCancelled() const;
private:
	friend class ResourceIoPipeline;
	explicit ResourceCancellation(const void *flag) : m_flag(flag) {}
	const void *m_flag;
};

// prepare runs on the game owner after I/O. It must only inspect headers and
// calculate a checked upper bound, not allocate its decoded output. decode
// uses immutable input and private heap storage. Destruction returns to owner.
class ResourceDecodeOperation
{
public:
	virtual ~ResourceDecodeOperation();
	virtual bool prepare(const unsigned char *bytes, size_t size,
		size_t &workspaceBytes) = 0;
	virtual bool decode(const unsigned char *bytes, size_t size,
		const ResourceCancellation &cancellation) = 0;
};

struct ResourceIoTicket
{
	ResourceIoTicket() : id(0), generation(0) {}
	bool isValid() const { return id != 0; }
	JobMetricCounter id;
	JobMetricCounter generation;
};

enum ResourceIoStatus
{
	RESOURCE_IO_PENDING,
	RESOURCE_IO_SUCCEEDED,
	RESOURCE_IO_READ_FAILED,
	RESOURCE_IO_DECODE_FAILED,
	RESOURCE_IO_CANCELLED,
	RESOURCE_IO_STALE
};

struct ResourceIoConfig
{
	ResourceIoConfig();
	unsigned maximumRequests;
	size_t inputByteBudget;
	size_t decodeByteBudget;
	unsigned readChunkBytes;
};

struct ResourceIoMetrics
{
	ResourceIoMetrics();
	JobMetricCounter accepted, rejected, reads, readFailures, decoded;
	JobMetricCounter cancelled, stale, serialFallbacks, bytesRead;
	JobMetricCounter ownershipFailures;
	JobMetricCounter decodeBudgetStalls;
	JobMetricCounter readNanoseconds, decodeNanoseconds, ownerWaitNanoseconds;
	unsigned pendingRequests, requestHighWater, activeIo, activeDecode;
	unsigned maximumOverlappingIoAndDecode;
	size_t inputBytes, decodeBytes, inputHighWater, decodeHighWater;
};

class ResourceIoPipeline
{
public:
	ResourceIoPipeline();
	~ResourceIoPipeline();
	bool start(const ResourceIoConfig &config, const JobGroup &decodeGroup);
	// Ownership transfers only on successful admission. Admission never waits.
	bool submit(ResourceIoSource *source, ResourceDecodeOperation *operation,
		JobPriority priority, ResourceIoTicket &ticket);
	// Only the creator may pump, wait, take, change generation, or shut down.
	void pump();
	bool promote(const ResourceIoTicket &ticket);
	bool cancel(const ResourceIoTicket &ticket);
	// False means completed outputs must be taken to release their budget;
	// waiting never deadlocks behind an owner-retained result.
	bool wait(const ResourceIoTicket &ticket);
	void cancelAndDrain();
	void advanceGeneration();
	void shutdown();
	bool isComplete(const ResourceIoTicket &ticket) const;
	bool empty() const;
	// Normal streaming publication is submission-ordered. Explicit foreground
	// requests may take their own ticket, preserving the legacy priority rule.
	bool takeNext(ResourceIoTicket &ticket, ResourceIoStatus &status,
		ResourceDecodeOperation *&operation);
	bool take(const ResourceIoTicket &ticket, ResourceIoStatus &status,
		ResourceDecodeOperation *&operation);
	JobMetricCounter generation() const;
	ResourceIoMetrics metrics() const;

private:
	ResourceIoPipeline(const ResourceIoPipeline &);
	ResourceIoPipeline &operator=(const ResourceIoPipeline &);
	struct State;
	State *m_state;
};
}
