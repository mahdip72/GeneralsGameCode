/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

namespace rts {
namespace render {

// A dynamic shadow upload may be replayed for the second stencil pass only
// while its exact ranges remain in the same stream generations and render.
struct ShadowSecondPassUploadReuseRecord
{
	const void *task;
	const void *geometry;
	float transform[12];
	unsigned int geometryRevision;
	const void *vertexBuffer;
	const void *indexBuffer;
	unsigned int renderEpoch;
	unsigned int vertexDiscardGeneration;
	unsigned int indexDiscardGeneration;
	unsigned int vertexStart;
	unsigned int vertexCount;
	unsigned int indexStart;
	unsigned int indexCount;
	bool valid;

	ShadowSecondPassUploadReuseRecord()
		: task(0), geometry(0), geometryRevision(0), vertexBuffer(0), indexBuffer(0),
		  renderEpoch(0), vertexDiscardGeneration(0), indexDiscardGeneration(0),
		  vertexStart(0), vertexCount(0), indexStart(0), indexCount(0), valid(false)
	{
		for (unsigned int i = 0; i < 12; ++i) transform[i] = 0;
	}

	void invalidate()
	{
		valid = false;
	}
};

struct ShadowSecondPassUploadReuseQuery
{
	const void *task;
	const void *geometry;
	float transform[12];
	unsigned int geometryRevision;
	bool secondPassFrozen;
	const void *vertexBuffer;
	const void *indexBuffer;
	unsigned int renderEpoch;
	unsigned int vertexDiscardGeneration;
	unsigned int indexDiscardGeneration;
	unsigned int vertexCount;
	unsigned int indexCount;
	unsigned int vertexCapacity;
	unsigned int indexCapacity;
	unsigned int vertexCursor;
	unsigned int indexCursor;

	ShadowSecondPassUploadReuseQuery()
		: task(0), geometry(0), geometryRevision(0), secondPassFrozen(false), vertexBuffer(0), indexBuffer(0),
		  renderEpoch(0), vertexDiscardGeneration(0), indexDiscardGeneration(0),
		  vertexCount(0), indexCount(0), vertexCapacity(0), indexCapacity(0),
		  vertexCursor(0), indexCursor(0)
	{
		for (unsigned int i = 0; i < 12; ++i) transform[i] = 0;
	}
};

// Fixed-size native shadow-stream page bookkeeping. Each page belongs to one
// concrete buffer owner; cursors and discard generations remain local so all
// draw offsets can continue using the unsigned-short renderer API.
enum { ShadowStreamPageMaximumCount = 4U, ShadowStreamPageMaximumCapacity = 65535U };

struct ShadowStreamPageState
{
	const void *owner;
	unsigned int renderEpoch;
	unsigned int discardGeneration;
	unsigned int cursor;
	unsigned int lastUseSequence;
	bool usedThisEpoch;

	ShadowStreamPageState()
		: owner(0), renderEpoch(0), discardGeneration(1), cursor(0),
		  lastUseSequence(0), usedThisEpoch(false)
	{
	}
};

struct ShadowStreamPageReservation
{
	unsigned int pageIndex;
	unsigned int start;
	unsigned int appendCount;
	unsigned int renderEpoch;
	unsigned int useSequence;
	bool discard;
	bool valid;

	ShadowStreamPageReservation()
		: pageIndex(0), start(0), appendCount(0), renderEpoch(0),
		  useSequence(0), discard(false), valid(false)
	{
	}
};

inline void AdvanceShadowStreamPageGeneration(unsigned int *generation)
{
	if (generation == 0)
		return;
	++(*generation);
	if (*generation == 0)
		++(*generation);
}

inline void InitializeShadowStreamPageState(ShadowStreamPageState *page,
	const void *owner)
{
	if (page == 0)
		return;
	page->owner = owner;
	page->renderEpoch = 0;
	page->discardGeneration = 1;
	page->cursor = 0;
	page->lastUseSequence = 0;
	page->usedThisEpoch = false;
}

inline void InvalidateShadowStreamPageState(ShadowStreamPageState *page)
{
	if (page == 0)
		return;
	AdvanceShadowStreamPageGeneration(&page->discardGeneration);
	page->renderEpoch = 0;
	page->cursor = 0;
	page->lastUseSequence = 0;
	page->usedThisEpoch = false;
}

inline void ReleaseShadowStreamPageState(ShadowStreamPageState *page)
{
	if (page == 0)
		return;
	InvalidateShadowStreamPageState(page);
	page->owner = 0;
}

inline bool ReserveShadowStreamPageAppend(
	ShadowStreamPageState *pages,
	unsigned int pageCount,
	int preferredPage,
	unsigned int renderEpoch,
	unsigned int capacity,
	unsigned int appendCount,
	unsigned int useSequence,
	ShadowStreamPageReservation *reservation)
{
	if (reservation == 0)
		return false;
	reservation->valid = false;
	if (pages == 0 || pageCount == 0 || pageCount > ShadowStreamPageMaximumCount ||
		capacity == 0 || capacity > ShadowStreamPageMaximumCapacity ||
		appendCount == 0 || appendCount > capacity)
		return false;

	int selected = -1;
	bool discard = true;
	if (preferredPage >= 0 && static_cast<unsigned int>(preferredPage) < pageCount)
	{
		const ShadowStreamPageState &page = pages[preferredPage];
		if (page.owner != 0 && page.usedThisEpoch && page.renderEpoch == renderEpoch &&
			page.cursor <= capacity && appendCount <= capacity - page.cursor)
		{
			selected = preferredPage;
			discard = false;
		}
	}

	// Reuse a previously touched page with safe tail space before discarding a
	// page. Prefer the most recently used fit so successive appends stay local.
	if (selected < 0)
	{
		unsigned int newestUse = 0;
		for (unsigned int i = 0; i < pageCount; ++i)
		{
			const ShadowStreamPageState &page = pages[i];
			if (page.owner != 0 && page.usedThisEpoch && page.renderEpoch == renderEpoch &&
				page.cursor <= capacity && appendCount <= capacity - page.cursor &&
				(selected < 0 || page.lastUseSequence >= newestUse))
			{
				selected = static_cast<int>(i);
				newestUse = page.lastUseSequence;
			}
		}
		if (selected >= 0)
			discard = false;
	}

	// First writes in an epoch use an untouched owner before evicting any live
	// range. The required DISCARD is committed only after the upload succeeds.
	if (selected < 0)
	{
		for (unsigned int i = 0; i < pageCount; ++i)
		{
			if (pages[i].owner != 0 &&
				(!pages[i].usedThisEpoch || pages[i].renderEpoch != renderEpoch))
			{
				selected = static_cast<int>(i);
				break;
			}
		}
	}

	// All pages are live and no tail fits: deterministically evict the least
	// recently used page. Its generation advances when the DISCARD commits.
	if (selected < 0)
	{
		unsigned int oldestUse = 0xffffffffU;
		for (unsigned int i = 0; i < pageCount; ++i)
		{
			const ShadowStreamPageState &page = pages[i];
			if (page.owner != 0 && page.usedThisEpoch && page.renderEpoch == renderEpoch &&
				page.lastUseSequence < oldestUse)
			{
				selected = static_cast<int>(i);
				oldestUse = page.lastUseSequence;
			}
		}
	}
	if (selected < 0)
		return false;

	reservation->pageIndex = static_cast<unsigned int>(selected);
	reservation->start = discard ? 0U : pages[selected].cursor;
	reservation->appendCount = appendCount;
	reservation->renderEpoch = renderEpoch;
	reservation->useSequence = useSequence;
	reservation->discard = discard;
	reservation->valid = true;
	return true;
}

inline bool CommitShadowStreamPageAppend(
	ShadowStreamPageState *pages,
	unsigned int pageCount,
	unsigned int capacity,
	const ShadowStreamPageReservation &reservation)
{
	if (pages == 0 || !reservation.valid || reservation.pageIndex >= pageCount ||
		pageCount > ShadowStreamPageMaximumCount || capacity == 0 ||
		capacity > ShadowStreamPageMaximumCapacity || reservation.appendCount == 0 ||
		reservation.renderEpoch == 0 || reservation.start > capacity ||
		reservation.appendCount > capacity - reservation.start ||
		(reservation.discard && reservation.start != 0))
		return false;

	ShadowStreamPageState &page = pages[reservation.pageIndex];
	if (page.owner == 0)
		return false;
	if (!reservation.discard &&
		(!page.usedThisEpoch || page.renderEpoch != reservation.renderEpoch ||
		 page.cursor != reservation.start))
		return false;
	if (reservation.discard)
		AdvanceShadowStreamPageGeneration(&page.discardGeneration);
	page.cursor = reservation.start + reservation.appendCount;
	page.renderEpoch = reservation.renderEpoch;
	page.lastUseSequence = reservation.useSequence;
	page.usedThisEpoch = true;
	return true;
}

inline bool MarkShadowStreamPageUsed(ShadowStreamPageState *page,
	unsigned int renderEpoch, unsigned int useSequence)
{
	if (page == 0 || page->owner == 0 || !page->usedThisEpoch ||
		page->renderEpoch != renderEpoch)
		return false;
	page->lastUseSequence = useSequence;
	return true;
}

enum ShadowUploadReuseMissReason
{
	ShadowReuseHit, ShadowReuseMissingProof, ShadowReuseGeometryChanged,
	ShadowReuseTransformChanged, ShadowReuseVertexDiscarded,
	ShadowReuseIndexDiscarded, ShadowReuseRangeInvalid
};

inline ShadowUploadReuseMissReason ShadowSecondPassUploadReuseReason(
	const ShadowSecondPassUploadReuseRecord &record,
	const ShadowSecondPassUploadReuseQuery &query)
{
	if (!record.valid || !query.secondPassFrozen || record.task != query.task ||
		record.vertexBuffer != query.vertexBuffer || record.indexBuffer != query.indexBuffer ||
		record.renderEpoch != query.renderEpoch)
		return ShadowReuseMissingProof;
	if (record.geometry != query.geometry || record.geometryRevision != query.geometryRevision ||
		record.vertexCount != query.vertexCount || record.indexCount != query.indexCount)
		return ShadowReuseGeometryChanged;
	for (unsigned int i = 0; i < 12; ++i)
		if (record.transform[i] != query.transform[i]) return ShadowReuseTransformChanged;
	if (record.vertexDiscardGeneration != query.vertexDiscardGeneration) return ShadowReuseVertexDiscarded;
	if (record.indexDiscardGeneration != query.indexDiscardGeneration) return ShadowReuseIndexDiscarded;
	if (query.vertexCount == 0 || query.indexCount == 0 ||
		query.vertexCursor > query.vertexCapacity || query.indexCursor > query.indexCapacity ||
		record.vertexStart > query.vertexCapacity ||
		record.vertexCount > query.vertexCapacity - record.vertexStart ||
		record.vertexStart > query.vertexCursor ||
		record.vertexCount > query.vertexCursor - record.vertexStart ||
		record.indexStart > query.indexCapacity ||
		record.indexCount > query.indexCapacity - record.indexStart ||
		record.indexStart > query.indexCursor ||
		record.indexCount > query.indexCursor - record.indexStart)
		return ShadowReuseRangeInvalid;
	return ShadowReuseHit;
}

inline bool CanReuseShadowSecondPassUpload(
	const ShadowSecondPassUploadReuseRecord &record,
	const ShadowSecondPassUploadReuseQuery &query)
{
	return ShadowSecondPassUploadReuseReason(record, query) == ShadowReuseHit;
}

inline void RecordShadowSecondPassUpload(
	ShadowSecondPassUploadReuseRecord *record,
	const ShadowSecondPassUploadReuseQuery &query,
	unsigned int vertexStart,
	unsigned int indexStart)
{
	if (record == 0)
		return;

	record->invalidate();
	record->task = query.task;
	record->geometry = query.geometry;
	for (unsigned int i = 0; i < 12; ++i) record->transform[i] = query.transform[i];
	record->geometryRevision = query.geometryRevision;
	record->vertexBuffer = query.vertexBuffer;
	record->indexBuffer = query.indexBuffer;
	record->renderEpoch = query.renderEpoch;
	record->vertexDiscardGeneration = query.vertexDiscardGeneration;
	record->indexDiscardGeneration = query.indexDiscardGeneration;
	record->vertexStart = vertexStart;
	record->vertexCount = query.vertexCount;
	record->indexStart = indexStart;
	record->indexCount = query.indexCount;
	record->valid = true;
}

} // namespace render
} // namespace rts
