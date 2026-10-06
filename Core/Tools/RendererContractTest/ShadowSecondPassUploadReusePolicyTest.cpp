/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Device-free C++98 checks for generation- and range-safe shadow upload reuse.
#include "Renderer/ShadowSecondPassUploadReusePolicy.h"
#include <stdio.h>

static int failures = 0;

static void Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static rts::render::ShadowSecondPassUploadReuseQuery Query(
	unsigned int epoch,
	unsigned int vertexGeneration,
	unsigned int indexGeneration,
	const void *geometry,
	const float *transform,
	const void *vertexBuffer,
	const void *indexBuffer)
{
	rts::render::ShadowSecondPassUploadReuseQuery query;
	query.geometry = geometry;
	for (unsigned int i=0; i<12; ++i) query.transform[i]=transform[i];
	query.geometryRevision=1;
	query.secondPassFrozen=true;
	query.task=geometry;
	query.vertexBuffer = vertexBuffer;
	query.indexBuffer = indexBuffer;
	query.renderEpoch = epoch;
	query.vertexDiscardGeneration = vertexGeneration;
	query.indexDiscardGeneration = indexGeneration;
	query.vertexCount = 128;
	query.indexCount = 384;
	query.vertexCapacity = 4096;
	query.indexCapacity = 8192;
	query.vertexCursor = 512;
	query.indexCursor = 1536;
	return query;
}

static void Record(rts::render::ShadowSecondPassUploadReuseRecord *record,
	const rts::render::ShadowSecondPassUploadReuseQuery &query)
{
	rts::render::RecordShadowSecondPassUpload(record, query, 256, 768);
}

static rts::render::ShadowSecondPassUploadReuseQuery PageQuery(
	unsigned int epoch,
	const void *geometry,
	const float *transform,
	const rts::render::ShadowStreamPageState &vertexPage,
	const rts::render::ShadowStreamPageState &indexPage,
	unsigned int capacity)
{
	rts::render::ShadowSecondPassUploadReuseQuery query = Query(epoch,
		vertexPage.discardGeneration, indexPage.discardGeneration, geometry,
		transform, vertexPage.owner, indexPage.owner);
	query.vertexCount = 2;
	query.indexCount = 2;
	query.vertexCapacity = capacity;
	query.indexCapacity = capacity;
	query.vertexCursor = vertexPage.cursor;
	query.indexCursor = indexPage.cursor;
	return query;
}

static bool SameShadowPageStateFields(
	const rts::render::ShadowStreamPageState &left,
	const rts::render::ShadowStreamPageState &right)
{
	return left.owner == right.owner &&
		left.renderEpoch == right.renderEpoch &&
		left.discardGeneration == right.discardGeneration &&
		left.cursor == right.cursor &&
		left.lastUseSequence == right.lastUseSequence &&
		left.usedThisEpoch == right.usedThisEpoch;
}

static bool SameShadowPageArrayFields(
	const rts::render::ShadowStreamPageState *left,
	const rts::render::ShadowStreamPageState *right,
	unsigned int pageCount)
{
	for (unsigned int i=0; i<pageCount; ++i)
		if (!SameShadowPageStateFields(left[i],right[i]))
			return false;
	return true;
}

int main()
{
	int geometryA = 0, geometryB = 0;
	float transformA[12]={0};
	int vertexBufferA = 0, vertexBufferB = 0;
	int indexBufferA = 0, indexBufferB = 0;
	const rts::render::ShadowSecondPassUploadReuseQuery base = Query(
		7, 3, 5, &geometryA, transformA, &vertexBufferA, &indexBufferA);
	rts::render::ShadowSecondPassUploadReuseRecord record;

	Check(!rts::render::CanReuseShadowSecondPassUpload(record, base),
		"an unrecorded range is never reusable");
	Record(&record, base);
	Check(rts::render::CanReuseShadowSecondPassUpload(record, base),
		"same-render ranges in unchanged generations remain reusable");

	rts::render::ShadowSecondPassUploadReuseQuery changed = base;
	changed.renderEpoch++;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"a new render epoch rejects cross-frame reuse");
	changed = base;
	changed.vertexDiscardGeneration++;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"a vertex DISCARD rejects stale vertex ranges");
	changed = base;
	changed.indexDiscardGeneration++;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"an index DISCARD rejects stale index ranges");
	changed = base;
	changed.geometry = &geometryB;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"changed geometry identity rejects reuse");
	changed = base;
	changed.transform[11] = 1;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"changed transform value at the same address rejects reuse");
	changed = base;
	changed.vertexBuffer = &vertexBufferB;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"changed vertex-buffer identity rejects reuse");
	changed = base;
	changed.indexBuffer = &indexBufferB;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"changed index-buffer identity rejects reuse");
	changed = base;
	changed.vertexCount++;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"changed vertex count rejects reuse");
	changed = base;
	changed.indexCount++;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"changed index count rejects reuse");

	changed = base;
	changed.vertexCapacity = 300;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"a vertex range beyond capacity rejects reuse");
	changed = base;
	changed.indexCapacity = 800;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"an index range beyond capacity rejects reuse");
	changed = base;
	changed.vertexCursor = 300;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"a vertex range beyond the current cursor rejects reuse");
	changed = base;
	changed.indexCursor = 800;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"an index range beyond the current cursor rejects reuse");
	changed = base;
	changed.vertexCursor = 0xffffffffU;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"an invalid vertex cursor rejects reuse");
	changed = base;
	changed.indexCursor = 0xffffffffU;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"an invalid index cursor rejects reuse");

	// NO_OVERWRITE appends at later cursors in the same generation, so the
	// already-recorded range remains valid.
	changed = base;
	changed.vertexCursor = 640;
	changed.indexCursor = 1920;
	Check(rts::render::CanReuseShadowSecondPassUpload(record, changed),
		"NO_OVERWRITE appends preserve earlier recorded ranges");

	// The transform snapshot owns its values; changing the original storage
	// after recording must not silently change the proof.
	transformA[0]=2;
	changed=Query(7,3,5,&geometryA,transformA,&vertexBufferA,&indexBufferA);
	Check(!rts::render::CanReuseShadowSecondPassUpload(record,changed), "same-address matrix mutation rejects reuse");
	changed=base; changed.geometryRevision++;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record,changed), "same-size pose or light-derived geometry mutation rejects reuse");
	changed=base; changed.secondPassFrozen=false;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record,changed), "first-pass or resumed mutation cannot reuse");
	changed=base; changed.task=&geometryB;
	Check(!rts::render::CanReuseShadowSecondPassUpload(record,changed), "another task cannot borrow the proof");

	// Hand-authored allocation history: first global pass visits A1,A2,B1,B2.
	// The second global pass visits B1,B2,A1,A2, with static passes between.
	// Only B2 remains after the last VB wrap. Uploading the B1 miss can wrap
	// again BEFORE B2 is visited. A final-generation suffix is not a hit guarantee.
	rts::render::ShadowSecondPassUploadReuseRecord b2;
	changed=base; changed.vertexDiscardGeneration=9; changed.vertexCount=2;
	changed.indexCount=6; changed.vertexCapacity=4; changed.indexCapacity=12;
	changed.vertexCursor=2; changed.indexCursor=6;
	rts::render::RecordShadowSecondPassUpload(&b2,changed,0,0);
	Check(rts::render::CanReuseShadowSecondPassUpload(b2,changed), "tail survives at the global pass boundary");
	changed.vertexDiscardGeneration=10; changed.vertexCursor=3;
	Check(rts::render::ShadowSecondPassUploadReuseReason(b2,changed)==rts::render::ShadowReuseVertexDiscarded,
		"earlier second-pass fallback DISCARD invalidates the unvisited tail");
	changed.vertexDiscardGeneration=9; changed.indexDiscardGeneration++;
	Check(rts::render::ShadowSecondPassUploadReuseReason(b2,changed)==rts::render::ShadowReuseIndexDiscarded,
		"IB-only wrap independently invalidates the tail");
	changed=base; changed.vertexCount=16384; changed.indexCount=49152;
	changed.vertexCapacity=changed.vertexCursor=16384;
	changed.indexCapacity=changed.indexCursor=49152;
	rts::render::RecordShadowSecondPassUpload(&b2,changed,0,0);
	Check(rts::render::CanReuseShadowSecondPassUpload(b2,changed), "exact bounded arena capacity is reusable");
	changed.vertexCount=16385;
	rts::render::RecordShadowSecondPassUpload(&b2,changed,0,0);
	Check(!rts::render::CanReuseShadowSecondPassUpload(b2,changed), "oversized vertex upload cannot authorize reuse");
	changed=base; changed.indexCount=49153; changed.indexCapacity=49152;
	rts::render::RecordShadowSecondPassUpload(&b2,changed,0,0);
	Check(!rts::render::CanReuseShadowSecondPassUpload(b2,changed), "oversized index upload cannot authorize reuse");

	// Four-page history: a draw recorded on VB page 0 and IB page 2 remains
	// reusable after the writer switches to other pages. The query must bind
	// the actual recorded owners rather than whichever pages are currently active.
	int vertexOwners[4], indexOwners[4];
	rts::render::ShadowStreamPageState vertexPages[4], indexPages[4];
	for (unsigned int i=0; i<4; ++i)
	{
		vertexOwners[i]=static_cast<int>(i);
		indexOwners[i]=static_cast<int>(i+10);
		rts::render::InitializeShadowStreamPageState(&vertexPages[i], &vertexOwners[i]);
		rts::render::InitializeShadowStreamPageState(&indexPages[i], &indexOwners[i]);
	}
	rts::render::ShadowStreamPageReservation reservation;
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,-1,20,4,2,1,&reservation) &&
		reservation.pageIndex==0 && reservation.discard && reservation.start==0,
		"the first append claims the first owner with a DISCARD");
	Check(rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"a successful first upload commits its page range");
	Check(rts::render::ReserveShadowStreamPageAppend(indexPages,4,-1,20,4,4,1,&reservation) &&
		reservation.pageIndex==0 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(indexPages,4,4,reservation),
		"the independent index stream owns its own page cursor");
	Check(rts::render::ReserveShadowStreamPageAppend(indexPages,4,0,20,4,4,2,&reservation) &&
		reservation.pageIndex==1 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(indexPages,4,4,reservation),
		"the index stream switches to an untouched owner before eviction");
	Check(rts::render::ReserveShadowStreamPageAppend(indexPages,4,1,20,4,2,3,&reservation) &&
		reservation.pageIndex==2 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(indexPages,4,4,reservation),
		"independent cursors can place a draw on a different index page");
	rts::render::ShadowSecondPassUploadReuseRecord pageRecord;
	rts::render::ShadowSecondPassUploadReuseQuery pageQuery=PageQuery(20,
		&geometryA,transformA,vertexPages[0],indexPages[2],4);
	rts::render::RecordShadowSecondPassUpload(&pageRecord,pageQuery,0,0);

	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,0,20,4,2,2,&reservation) &&
		!reservation.discard && reservation.pageIndex==0 &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"a page with safe tail space appends without invalidating old ranges");
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,0,20,4,1,3,&reservation) &&
		reservation.pageIndex==1 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"a full writer switches to the next unused vertex page");
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,1,20,4,3,4,&reservation) &&
		!reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"the switched vertex page keeps its independent append cursor");
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,1,20,4,4,5,&reservation) &&
		reservation.pageIndex==2 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"the third vertex page is used before any live page is evicted");
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,2,20,4,4,6,&reservation) &&
		reservation.pageIndex==3 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"the fourth vertex page is used before the pool cycles");
	pageQuery=PageQuery(20,&geometryA,transformA,vertexPages[0],indexPages[2],4);
	Check(rts::render::CanReuseShadowSecondPassUpload(pageRecord,pageQuery),
		"second-pass proof rebinds to its recorded VB and IB owners after page switching");
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,3,20,4,1,7,&reservation) &&
		reservation.pageIndex==0 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"a full pool evicts the deterministic least-recently-used vertex page");
	pageQuery=PageQuery(20,&geometryA,transformA,vertexPages[0],indexPages[2],4);
	Check(rts::render::ShadowSecondPassUploadReuseReason(pageRecord,pageQuery)==
		rts::render::ShadowReuseVertexDiscarded,
		"evicting a recorded VB page invalidates that proof by generation");

	// Index-only eviction must not be hidden by a VB generation change.
	rts::render::ShadowSecondPassUploadReuseRecord indexRecord;
	rts::render::ShadowSecondPassUploadReuseQuery indexQuery=PageQuery(20,
		&geometryA,transformA,vertexPages[0],indexPages[0],4);
	rts::render::RecordShadowSecondPassUpload(&indexRecord,indexQuery,0,0);
	Check(rts::render::ReserveShadowStreamPageAppend(indexPages,4,2,20,4,2,4,&reservation) &&
		!reservation.discard && rts::render::CommitShadowStreamPageAppend(indexPages,4,4,reservation),
		"the index stream can append on its own selected page");
	Check(rts::render::ReserveShadowStreamPageAppend(indexPages,4,2,20,4,4,5,&reservation) &&
		reservation.pageIndex==3 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(indexPages,4,4,reservation),
		"the fourth index page is used before eviction");
	Check(rts::render::ReserveShadowStreamPageAppend(indexPages,4,3,20,4,1,6,&reservation) &&
		reservation.pageIndex==0 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(indexPages,4,4,reservation),
		"index-page eviction advances only the selected index generation");
	indexQuery=PageQuery(20,&geometryA,transformA,vertexPages[0],indexPages[0],4);
	Check(rts::render::ShadowSecondPassUploadReuseReason(indexRecord,indexQuery)==
		rts::render::ShadowReuseIndexDiscarded,
		"an IB-only discard independently invalidates the stored index range");

	// A fail-closed reset invalidates every page proof; the next ordinary
	// upload still obtains a page through the normal discard/fallback route.
	rts::render::InvalidateShadowStreamPageState(&vertexPages[0]);
	pageQuery=PageQuery(20,&geometryA,transformA,vertexPages[0],indexPages[2],4);
	Check(rts::render::ShadowSecondPassUploadReuseReason(pageRecord,pageQuery)==
		rts::render::ShadowReuseVertexDiscarded,
		"failure invalidation suppresses stale hits until recovery uploads");
	Check(rts::render::ReserveShadowStreamPageAppend(vertexPages,4,-1,20,4,1,8,&reservation) &&
		reservation.pageIndex==0 && reservation.discard &&
		rts::render::CommitShadowStreamPageAppend(vertexPages,4,4,reservation),
		"the fallback recovery write starts at offset zero with a DISCARD");
	pageQuery=PageQuery(21,&geometryA,transformA,vertexPages[0],indexPages[2],4);
	Check(rts::render::ShadowSecondPassUploadReuseReason(pageRecord,pageQuery)==
		rts::render::ShadowReuseMissingProof,
		"a new render epoch rejects the prior page lifetime");

	// Capacity and count are validated before reservation; no 16-bit API may
	// be asked to represent a larger page or wrapped append range.
	rts::render::ShadowStreamPageReservation invalidReservation;
	Check(!rts::render::ReserveShadowStreamPageAppend(vertexPages,4,0,20,65536U,1,9,
		&invalidReservation) && !invalidReservation.valid,
		"a page larger than the unsigned-short API range is rejected");
	Check(!rts::render::ReserveShadowStreamPageAppend(vertexPages,4,0,20,4,5,9,
		&invalidReservation) && !invalidReservation.valid,
		"an append larger than one page cannot wrap or reserve a range");
	Check(!rts::render::ReserveShadowStreamPageAppend(vertexPages,5,0,20,4,1,9,
		&invalidReservation) && !invalidReservation.valid,
		"page count is bounded by the fixed owner array");

	// A resource release followed by acquisition changes the owner identity,
	// even if the slot number is reused.
	int replacementVertexOwner=99;
	rts::render::ReleaseShadowStreamPageState(&vertexPages[0]);
	rts::render::InitializeShadowStreamPageState(&vertexPages[0],&replacementVertexOwner);
	pageQuery=PageQuery(20,&geometryA,transformA,vertexPages[0],indexPages[2],4);
	Check(rts::render::ShadowSecondPassUploadReuseReason(pageRecord,pageQuery)==
		rts::render::ShadowReuseMissingProof,
		"reacquired page slots cannot borrow proofs from released buffer owners");

	// Rejected policy operations must leave all page-state fields untouched.
	int rejectionOwners[4];
	rts::render::ShadowStreamPageState rejectionPages[4];
	rts::render::ShadowStreamPageState beforeRejectedOperation[4];
	for (unsigned int i=0; i<4; ++i)
	{
		rejectionOwners[i]=static_cast<int>(i+20);
		rts::render::InitializeShadowStreamPageState(&rejectionPages[i],
			&rejectionOwners[i]);
	}
	rts::render::ShadowStreamPageReservation setupReservation;
	Check(rts::render::ReserveShadowStreamPageAppend(rejectionPages,4,0,31,4,2,1,
		&setupReservation) &&
		rts::render::CommitShadowStreamPageAppend(rejectionPages,4,4,setupReservation),
		"rejected-operation checks start from a committed live page");

	for (unsigned int i=0; i<4; ++i)
		beforeRejectedOperation[i]=rejectionPages[i];
	rts::render::ShadowStreamPageReservation failedReservation;
	Check(!rts::render::ReserveShadowStreamPageAppend(rejectionPages,4,0,31,4,5,2,
		&failedReservation) && !failedReservation.valid,
		"an oversized append reservation is rejected before selecting a page");
	Check(SameShadowPageArrayFields(rejectionPages,beforeRejectedOperation,4),
		"failed reservation preserves every owner's identity, epoch, generation, cursor, LRU stamp and used flag");

	rts::render::ShadowStreamPageReservation validReservation;
	Check(rts::render::ReserveShadowStreamPageAppend(rejectionPages,4,0,31,4,1,3,
		&validReservation) && !validReservation.discard,
		"invalid-commit checks obtain a live tail reservation");
	for (unsigned int i=0; i<4; ++i)
		beforeRejectedOperation[i]=rejectionPages[i];
	rts::render::ShadowStreamPageReservation invalidPageCommit=validReservation;
	invalidPageCommit.pageIndex=rts::render::ShadowStreamPageMaximumCount;
	Check(!rts::render::CommitShadowStreamPageAppend(rejectionPages,4,4,invalidPageCommit),
		"commit rejects a page index outside the owner array");
	Check(SameShadowPageArrayFields(rejectionPages,beforeRejectedOperation,4),
		"out-of-range page commit leaves all page-state fields unchanged");

	rts::render::ShadowStreamPageReservation invalidRangeCommit=validReservation;
	invalidRangeCommit.start=4;
	invalidRangeCommit.appendCount=1;
	Check(!rts::render::CommitShadowStreamPageAppend(rejectionPages,4,4,invalidRangeCommit),
		"commit rejects a range extending beyond page capacity");
	Check(SameShadowPageArrayFields(rejectionPages,beforeRejectedOperation,4),
		"out-of-capacity commit leaves all page-state fields unchanged");

	rts::render::ShadowStreamPageReservation staleCursorCommit;
	rts::render::ShadowStreamPageReservation winningCommit;
	Check(rts::render::ReserveShadowStreamPageAppend(rejectionPages,4,0,31,4,1,4,
		&staleCursorCommit) &&
		rts::render::ReserveShadowStreamPageAppend(rejectionPages,4,0,31,4,1,5,
			&winningCommit) &&
		rts::render::CommitShadowStreamPageAppend(rejectionPages,4,4,winningCommit),
		"a second valid commit advances the page beyond an earlier reservation");
	for (unsigned int i=0; i<4; ++i)
		beforeRejectedOperation[i]=rejectionPages[i];
	Check(!rts::render::CommitShadowStreamPageAppend(rejectionPages,4,4,staleCursorCommit),
		"commit rejects a reservation made stale by a newer cursor commit");
	Check(SameShadowPageArrayFields(rejectionPages,beforeRejectedOperation,4),
		"stale cursor commit preserves the winning page state field by field");

	rts::render::ShadowStreamPageReservation staleEpochCommit;
	Check(rts::render::ReserveShadowStreamPageAppend(rejectionPages,4,0,31,4,1,6,
		&staleEpochCommit),
		"epoch-staleness check obtains a live reservation");
	rts::render::InvalidateShadowStreamPageState(&rejectionPages[0]);
	for (unsigned int i=0; i<4; ++i)
		beforeRejectedOperation[i]=rejectionPages[i];
	Check(!rts::render::CommitShadowStreamPageAppend(rejectionPages,4,4,staleEpochCommit),
		"commit rejects a reservation after page invalidation");
	Check(SameShadowPageArrayFields(rejectionPages,beforeRejectedOperation,4),
		"stale epoch commit preserves the invalidated page and every peer field by field");

	record.invalidate();
	Check(!rts::render::CanReuseShadowSecondPassUpload(record, base),
		"failed uploads invalidate reuse eligibility");

	if (!failures)
		printf("shadow second-pass upload reuse policy contracts passed\n");
	return failures ? 1 : 0;
}
