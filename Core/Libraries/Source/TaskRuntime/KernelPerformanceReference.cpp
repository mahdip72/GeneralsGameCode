#include "Lib/KernelPerformanceReference.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <string.h>
#include <new>
#include <windows.h>
#include <bcrypt.h>

namespace rts { namespace performance {
KernelPerformanceDigest::KernelPerformanceDigest() : valid(false) { memset(bytes, 0, sizeof(bytes)); }
bool KernelPerformanceDigest::equals(const KernelPerformanceDigest &other) const
{ return valid && other.valid && memcmp(bytes, other.bytes, sizeof(bytes)) == 0; }

KernelPerformanceRequestBudget::KernelPerformanceRequestBudget() : requestOrdinal(0),
	disposition(KERNEL_REQUEST_BUDGET_NOT_REACHED), grantSite(0), localGrantOrdinal(0),
	requestedBytes(0), grantedBytes(0), refundSite(0), localRefundOrdinal(0),
	refundedBytes(0), consumedBytes(0) {}

namespace {
bool sameCheckpoint(const KernelPerformanceCheckpoint &first, const KernelPerformanceCheckpoint &second)
{
	return first.site == second.site && first.first == second.first && first.second == second.second;
}
bool checkpointTerminalMatchesCut(KernelPerformanceRangeTerminal terminal, bool hasCut)
{
	if (terminal == KERNEL_RANGE_COMPLETED) return !hasCut;
	if (terminal == KERNEL_RANGE_CANCELLED) return hasCut;
	return terminal == KERNEL_RANGE_FAILED;
}
bool releasedCheckpointProgressValid(const KernelPerformanceCheckpointProgress &source)
{
	if (!source.entered || source.errors != 0 || source.finalCheckpoint.site == 0 ||
		!checkpointTerminalMatchesCut(source.terminal, source.firstTruePoll != 0)) return false;
	if (source.firstTruePoll == 0)
		return source.firstTrueCheckpoint.site == 0 && source.firstTrueCheckpoint.first == 0 &&
			source.firstTrueCheckpoint.second == 0;
	// V1 stops at the first true predicate; a later poll is not representable.
	return source.firstTruePoll == source.pollCount && source.firstTrueCheckpoint.site != 0;
}
}

KernelPerformanceCheckpointProbe::KernelPerformanceCheckpointProbe() : m_mode(Disabled), m_finished(false),
	m_progress(), m_source() {}
bool KernelPerformanceCheckpointProbe::fail() noexcept
{
	m_progress.errors |= KERNEL_REFERENCE_ERROR_CHECKPOINT;
	return false;
}
bool KernelPerformanceCheckpointProbe::beginRecord() noexcept
{
	if (m_mode != Disabled) return fail();
	m_mode = Record;
	if (m_progress.errors != 0) return false;
	m_progress.entered = true;
	return true;
}
bool KernelPerformanceCheckpointProbe::beginReplay(const KernelPerformanceCheckpointProgress &source) noexcept
{
	if (m_mode != Disabled) return fail();
	// Latch replay before validation so malformed input cannot restore ordinary
	// cancellation policy or reset into recording after a failed initialization.
	m_mode = Replay;
	if (m_progress.errors != 0 || !releasedCheckpointProgressValid(source)) return fail();
	m_source = source;
	m_progress.entered = true;
	return true;
}
bool KernelPerformanceCheckpointProbe::cancelled(const KernelPerformanceCheckpoint &at, bool actualCancel) noexcept
{
	if (m_mode == Disabled) return actualCancel;
	if (m_finished || m_progress.errors != 0 || at.site == 0 || m_progress.firstTruePoll != 0 ||
		m_progress.pollCount == ~static_cast<JobMetricCounter>(0))
	{
		fail();
		return m_mode == Replay ? true : actualCancel;
	}
	++m_progress.pollCount;
	bool cancelled = actualCancel;
	if (m_mode == Replay)
	{
		if (m_progress.pollCount > m_source.pollCount)
		{ fail(); return true; }
		cancelled = m_source.firstTruePoll != 0 && m_progress.pollCount == m_source.firstTruePoll;
		if (cancelled && !sameCheckpoint(at, m_source.firstTrueCheckpoint))
		{ fail(); return true; }
	}
	if (cancelled)
	{
		m_progress.firstTruePoll = m_progress.pollCount;
		m_progress.firstTrueCheckpoint = at;
	}
	return cancelled;
}
bool KernelPerformanceCheckpointProbe::finish(const KernelPerformanceCheckpoint &at,
	JobMetricCounter completedWorkUnits, KernelPerformanceRangeTerminal terminal) noexcept
{
	if (m_mode == Disabled || m_finished) return fail();
	m_finished = true;
	if (m_progress.errors != 0 || at.site == 0 ||
		!checkpointTerminalMatchesCut(terminal, m_progress.firstTruePoll != 0)) return fail();
	if (m_mode == Replay && (m_progress.pollCount != m_source.pollCount ||
		m_progress.firstTruePoll != m_source.firstTruePoll ||
		!sameCheckpoint(m_progress.firstTrueCheckpoint, m_source.firstTrueCheckpoint) ||
		!sameCheckpoint(at, m_source.finalCheckpoint) ||
		completedWorkUnits != m_source.completedWorkUnits || terminal != m_source.terminal)) return fail();
	m_progress.finalCheckpoint = at;
	m_progress.completedWorkUnits = completedWorkUnits;
	m_progress.terminal = terminal;
	return true;
}
KernelPerformanceCheckpointProgress KernelPerformanceCheckpointProbe::snapshot() const noexcept
{
	return m_progress;
}

KernelPerformanceTraceOptions::KernelPerformanceTraceOptions() : mode(KERNEL_TRACE_DISABLED), binding(), limits(),
	residentAttemptCapacity(0), residentRangeCapacity(0), append(0), context(0), readAt(0), sourceByteCount(0) {}
KernelPerformanceReferenceRunOptions::KernelPerformanceReferenceRunOptions() : mode(KERNEL_REFERENCE_DISABLED),
	clock(0), clockContext(0), trace() {}
KernelPerformanceWindowBoundary::KernelPerformanceWindowBoundary() : kind(KERNEL_WINDOW_INVALID), sampleOrdinal(0),
	phase(KERNEL_PHASE_COUNT), ownerFrameAtEntry(0), authorityFrame(0), actualOwnerFrame(0) {}
KernelPerformanceAttempt::KernelPerformanceAttempt() : m_owner(0), m_generation(0), m_serial(0), m_slot(0) {}
bool KernelPerformanceAttempt::valid() const { return m_owner != 0 && m_generation != 0 && m_serial != 0; }
KernelPerformanceDeterministicBypassProof::KernelPerformanceDeterministicBypassProof() :
	m_owner(0), m_attempt(), m_class(), m_input(0) {}
KernelPerformanceDeterministicBypassProof::KernelPerformanceDeterministicBypassProof(
	KernelPerformanceReferenceLedger &ledger, KernelPerformanceAttempt attempt,
	const NativeCollisionClassFacts &actualClass, const rts::PartitionCollisionReferenceInput &actualInput) :
	m_owner(&ledger), m_attempt(attempt), m_class(actualClass), m_input(&actualInput) {}
KernelPerformanceDeterministicBypass::KernelPerformanceDeterministicBypass() : m_owner(0), m_generation(0), m_serial(0) {}
bool KernelPerformanceDeterministicBypass::valid() const { return m_owner != 0 && m_generation != 0 && m_serial != 0; }
KernelPerformanceInlineBody::KernelPerformanceInlineBody() : m_owner(0), m_generation(0), m_serial(0) {}
bool KernelPerformanceInlineBody::valid() const { return m_owner != 0 && m_generation != 0 && m_serial != 0; }
KernelPerformanceInlineOwnerSerial::KernelPerformanceInlineOwnerSerial() : m_owner(0), m_generation(0), m_serial(0) {}
bool KernelPerformanceInlineOwnerSerial::valid() const { return m_owner != 0 && m_generation != 0 && m_serial != 0; }
KernelPerformanceTraceSnapshot::KernelPerformanceTraceSnapshot() : mode(KERNEL_TRACE_DISABLED), requested(false),
	frozen(false), complete(false), observationSealed(false), executionSealed(false), errors(0), binding(), limits(),
	residentAttemptCapacity(0), residentRangeCapacity(0), residentAttemptCount(0), residentAttemptHighWater(0),
	attemptCount(0), admittedAttemptCount(0), notAdmittedAttemptCount(0), abortedAfterAdmissionAttemptCount(0),
	reapCount(0), capturedAttemptCount(0), capturedOperationCount(0), dispatchCount(0), rangeCount(0), releasedRangeCount(0),
	residentRangeCount(0), residentRangeHighWater(0), windowBoundaryCount(0), completedWindowCount(0), controlWindowCount(0),
	recordCount(0), logicalEventCount(0),
	coalescedSpanCount(0), coalescedAttemptCount(0), byteCount(0) {}

struct KernelPerformanceCanonicalWriter::State
{
	enum { BufferCapacity = 65536 };
	State() : algorithm(0), hash(0), buffer(0), pending(0), byteCount(0), buffered(false),
		failed(false), finished(false), busy(false), transport(0), context(0) {}
	~State()
	{
		// An abandoned stream never implicitly publishes its unsealed suffix.
		delete[] buffer;
		if (hash != 0) BCryptDestroyHash(hash);
		if (algorithm != 0) BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	bool emit(const unsigned char *bytes, unsigned count) noexcept
	{
		if (busy) { failed = true; return false; }
		if (failed || finished || hash == 0) return false;
		if (count == 0) return true;
		busy = true;
		if (BCryptHashData(hash, const_cast<PUCHAR>(bytes), count, 0) != 0) failed = true;
		if (!failed && transport != 0)
		{
			try { if (!transport(context, bytes, count)) failed = true; }
			catch (...) { failed = true; }
		}
		// Reentry may have poisoned this state even if the callback returned
		// success. Never turn that failure into an accepted hash or retry.
		busy = false;
		return !failed;
	}
	bool flush() noexcept
	{
		if (busy) { failed = true; return false; }
		if (failed || hash == 0) return false;
		if (finished || pending == 0) return true;
		if (!emit(buffer, pending)) return false;
		pending = 0;
		return true;
	}
	bool append(const unsigned char *bytes, unsigned count) noexcept
	{
		if (busy) { failed = true; return false; }
		if (failed || finished || hash == 0) return false;
		if (count > ~JobMetricCounter(0) - byteCount) { failed = true; return false; }
		byteCount += count;
		if (!buffered) return emit(bytes, count);
		while (count != 0)
		{
			const unsigned available = BufferCapacity - pending;
			const unsigned copied = count < available ? count : available;
			memcpy(buffer + pending, bytes, copied);
			pending += copied;
			bytes += copied;
			count -= copied;
			if (pending == BufferCapacity && !flush()) return false;
		}
		return true;
	}
	BCRYPT_ALG_HANDLE algorithm;
	BCRYPT_HASH_HANDLE hash;
	unsigned char *buffer;
	unsigned pending;
	JobMetricCounter byteCount;
	bool buffered, failed, finished, busy;
	KernelPerformanceTraceAppend transport;
	void *context;
	KernelPerformanceDigest digest;
};

KernelPerformanceCanonicalWriter::KernelPerformanceCanonicalWriter() : m_state(0) {}
KernelPerformanceCanonicalWriter::~KernelPerformanceCanonicalWriter() { delete m_state; }
bool KernelPerformanceCanonicalWriter::begin(unsigned fieldSchema, KernelPerformanceTraceAppend append,
	void *context, bool buffered) noexcept
{
	if (m_state != 0 && (m_state->busy || m_state->failed || !m_state->finished))
	{ m_state->failed = true; return false; }
	if (m_state == 0) m_state = new (std::nothrow) State;
	if (m_state == 0) return false;
	// Reuse the writer's provider and optional buffer between completed spans.
	// Only an explicitly buffered writer allocates 64 KiB; ordinary per-input
	// canonical writers keep their existing small allocation footprint.
	m_state->finished = false;
	m_state->pending = 0;
	m_state->byteCount = 0;
	m_state->buffered = buffered;
	m_state->transport = append;
	m_state->context = context;
	m_state->digest = KernelPerformanceDigest();
	if (fieldSchema == 0) { m_state->failed = true; return false; }
	if (buffered && m_state->buffer == 0)
	{
		m_state->buffer = new (std::nothrow) unsigned char[State::BufferCapacity];
		if (m_state->buffer == 0) { m_state->failed = true; return false; }
	}
	if (m_state->hash != 0)
	{
		BCryptDestroyHash(m_state->hash);
		m_state->hash = 0;
	}
	if ((m_state->algorithm == 0 && BCryptOpenAlgorithmProvider(&m_state->algorithm,
		BCRYPT_SHA256_ALGORITHM, 0, 0) != 0) || BCryptCreateHash(m_state->algorithm,
		&m_state->hash, 0, 0, 0, 0, 0) != 0)
	{
		m_state->failed = true;
		return false;
	}
	static const unsigned char domain[] = "RTS-KERNEL-FIELDS-v1";
	unsigned char schema[4];
	for (unsigned index = 0; index != 4; ++index) schema[index] = static_cast<unsigned char>(fieldSchema >> (index * 8));
	return m_state->append(domain, sizeof(domain) - 1) && m_state->append(schema, sizeof(schema));
}
bool KernelPerformanceCanonicalWriter::flush() noexcept
{
	return m_state != 0 && m_state->flush();
}
JobMetricCounter KernelPerformanceCanonicalWriter::bytesWritten() const noexcept
{
	return m_state != 0 ? m_state->byteCount : 0;
}
bool KernelPerformanceCanonicalWriter::begin(unsigned fieldSchema)
{
	return begin(fieldSchema, 0, 0, false);
}
bool KernelPerformanceCanonicalWriter::field(unsigned type, unsigned tag, JobMetricCounter value, unsigned width)
{
	if (m_state == 0 || m_state->failed || m_state->finished) return false;
	unsigned char bytes[13];
	bytes[0] = static_cast<unsigned char>(type);
	for (unsigned index = 0; index != 4; ++index) bytes[1 + index] = static_cast<unsigned char>(tag >> (index * 8));
	for (unsigned index = 0; index != width; ++index) bytes[5 + index] = static_cast<unsigned char>(value >> (index * 8));
	return m_state->append(bytes, 5 + width);
}
bool KernelPerformanceCanonicalWriter::u32(unsigned tag, unsigned value) { return field(1, tag, value, 4); }
bool KernelPerformanceCanonicalWriter::i32(unsigned tag, int value) { return field(2, tag, static_cast<unsigned>(value), 4); }
bool KernelPerformanceCanonicalWriter::u64(unsigned tag, JobMetricCounter value) { return field(3, tag, value, 8); }
bool KernelPerformanceCanonicalWriter::f32(unsigned tag, float value)
{
	static_assert(sizeof(float) == sizeof(unsigned) && sizeof(unsigned) == 4, "Canonical float encoding requires binary32 storage");
	unsigned bits;
	memcpy(&bits, &value, sizeof(bits));
	return field(4, tag, bits, 4);
}
bool KernelPerformanceCanonicalWriter::boolean(unsigned tag, bool value) { return field(5, tag, value ? 1 : 0, 1); }
bool KernelPerformanceCanonicalWriter::sequence(unsigned tag, unsigned count) { return field(6, tag, count, 4); }
KernelPerformanceDigest KernelPerformanceCanonicalWriter::finish()
{
	if (m_state == 0) return KernelPerformanceDigest();
	if (m_state->busy) { m_state->failed = true; return KernelPerformanceDigest(); }
	if (m_state->failed) return KernelPerformanceDigest();
	if (!m_state->finished)
	{
		if (!m_state->flush()) return KernelPerformanceDigest();
		m_state->digest.valid = BCryptFinishHash(m_state->hash, m_state->digest.bytes, 32, 0) == 0;
		m_state->failed = !m_state->digest.valid;
		m_state->finished = true;
	}
	return m_state->digest;
}
KernelPerformanceReferenceBatch::KernelPerformanceReferenceBatch() : generation(0), serial(0),
	slot(KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES), m_owner(0) {}
bool KernelPerformanceReferenceBatch::valid() const { return m_owner != 0 && generation != 0 && serial != 0; }
KernelPerformanceReferenceStream::KernelPerformanceReferenceStream() : kernel(KERNEL_PERFORMANCE_PHYSICS),
	subtype(0), fieldSchema(0), firstFrame(0), lastFrame(0), validatedBatchCount(0),
	committedBatchCount(0), abortedBatchCount(0), validatedOperationCount(0), committedOperationCount(0),
	serialSampleCount(0), serialNanoseconds(0), maximumSerialNanoseconds(0) {}
KernelPerformanceReferenceSnapshot::KernelPerformanceReferenceSnapshot() : mode(KERNEL_REFERENCE_DISABLED),
	frozen(false), complete(false), errors(0), streamCount(0), generation(0) {}

namespace {
bool checkedAdd(JobMetricCounter &value, JobMetricCounter amount)
{
	if (amount > ~static_cast<JobMetricCounter>(0) - value) return false;
	value += amount;
	return true;
}
bool beginStream(KernelPerformanceCanonicalWriter &writer, KernelPerformanceKernel kernel,
	unsigned subtype, unsigned fieldSchema, unsigned domain)
{
	return writer.begin(1) && writer.u32(1, static_cast<unsigned>(kernel)) &&
		writer.u32(2, subtype) && writer.u32(3, fieldSchema) && writer.u32(4, domain);
}
bool appendIdentity(KernelPerformanceCanonicalWriter &writer, unsigned frame,
	JobMetricCounter ordinal, JobMetricCounter operations)
{
	return writer.u32(10, frame) && writer.u64(11, ordinal) && writer.u64(12, operations);
}
bool appendDigest(KernelPerformanceCanonicalWriter &writer, const KernelPerformanceDigest &digest, unsigned tag = 13)
{
	if (!digest.valid || !writer.sequence(tag, 4)) return false;
	for (unsigned index = 0; index != 4; ++index)
	{
		JobMetricCounter value = 0;
		for (unsigned byte = 0; byte != 8; ++byte)
			value |= static_cast<JobMetricCounter>(digest.bytes[index * 8 + byte]) << (byte * 8);
		if (!writer.u64(tag + 1 + index, value)) return false;
	}
	return true;
}
bool traceDynamicFactsValid(unsigned known, JobMetricCounter pending, JobMetricCounter outstanding,
	JobMetricCounter activeSlots)
{
	return (known & ~7U) == 0 && ((known & 1U) != 0 || pending == 0) &&
		((known & 2U) != 0 || outstanding == 0) && ((known & 4U) != 0 || activeSlots == 0);
}
bool sameRangePlan(const KernelPerformanceRangePlan &first, const KernelPerformanceRangePlan &second)
{
	return first.dispatchOrdinal == second.dispatchOrdinal && first.rangeOrdinal == second.rangeOrdinal &&
		first.bodyKind == second.bodyKind && first.begin == second.begin && first.end == second.end &&
		first.operationCount == second.operationCount;
}
bool requestBudgetProfile(const KernelPerformanceAttemptIdentity &identity,
	const KernelPerformanceDispatchPlan &dispatch)
{
	return identity.workKind == KERNEL_PERFORMANCE_PATH && identity.subtype == 0 && dispatch.bodySchema == 2;
}
bool requestBudgetValid(const KernelPerformanceRequestBudget &budget)
{
	JobMetricCounter settled = budget.consumedBytes;
	if (!checkedAdd(settled, budget.refundedBytes) || settled != budget.grantedBytes) return false;
	const bool noRefund = budget.refundSite == 0 && budget.localRefundOrdinal == 0 && budget.refundedBytes == 0;
	if (budget.disposition == KERNEL_REQUEST_BUDGET_NOT_REACHED)
		return budget.grantSite == 0 && budget.localGrantOrdinal == 0 && budget.requestedBytes == 0 &&
			budget.grantedBytes == 0 && noRefund && budget.consumedBytes == 0;
	if (budget.grantSite != 1 || budget.localGrantOrdinal != 1) return false;
	if (budget.disposition == KERNEL_REQUEST_BUDGET_REFUSED)
		return budget.grantedBytes == 0 && noRefund && budget.consumedBytes == 0;
	if (budget.grantedBytes != budget.requestedBytes) return false;
	if (budget.disposition == KERNEL_REQUEST_BUDGET_RETAINED)
		return noRefund && budget.consumedBytes == budget.grantedBytes;
	return budget.disposition == KERNEL_REQUEST_BUDGET_REFUNDED && budget.refundSite == 2 &&
		budget.localRefundOrdinal == 2 && budget.refundedBytes == budget.grantedBytes && budget.consumedBytes == 0;
}
bool sameRequestBudget(const KernelPerformanceRequestBudget &first, const KernelPerformanceRequestBudget &second)
{
	return first.requestOrdinal == second.requestOrdinal && first.disposition == second.disposition &&
		first.grantSite == second.grantSite && first.localGrantOrdinal == second.localGrantOrdinal &&
		first.requestedBytes == second.requestedBytes && first.grantedBytes == second.grantedBytes &&
		first.refundSite == second.refundSite && first.localRefundOrdinal == second.localRefundOrdinal &&
		first.refundedBytes == second.refundedBytes && first.consumedBytes == second.consumedBytes;
}
bool sameCheckpointProgress(const KernelPerformanceCheckpointProgress &a, const KernelPerformanceCheckpointProgress &b)
{
	return a.entered == b.entered && a.errors == b.errors && a.pollCount == b.pollCount &&
		a.firstTruePoll == b.firstTruePoll && a.completedWorkUnits == b.completedWorkUnits && a.terminal == b.terminal &&
		sameCheckpoint(a.firstTrueCheckpoint, b.firstTrueCheckpoint) && sameCheckpoint(a.finalCheckpoint, b.finalCheckpoint);
}
bool releasedRangeProgressValid(const KernelPerformanceRangeProgress &progress)
{
	const KernelPerformanceCheckpointProgress &checkpoint = progress.checkpoint;
	if (!checkpoint.entered)
		return checkpoint.errors == 0 && checkpoint.pollCount == 0 && checkpoint.firstTruePoll == 0 &&
			checkpoint.completedWorkUnits == 0 && checkpoint.firstTrueCheckpoint.site == 0 &&
			checkpoint.firstTrueCheckpoint.first == 0 && checkpoint.firstTrueCheckpoint.second == 0 &&
			checkpoint.finalCheckpoint.site == 0 && checkpoint.finalCheckpoint.first == 0 &&
			checkpoint.finalCheckpoint.second == 0 && checkpoint.terminal == KERNEL_RANGE_NEVER_ENTERED &&
			progress.publication == KERNEL_PUBLICATION_NOT_APPLICABLE;
	return releasedCheckpointProgressValid(checkpoint) && progress.publication >= KERNEL_PUBLICATION_PUBLISHED &&
		progress.publication <= KERNEL_PUBLICATION_REJECTED &&
		(progress.publication != KERNEL_PUBLICATION_PUBLISHED || checkpoint.terminal == KERNEL_RANGE_COMPLETED);
}
bool appendRangePlan(KernelPerformanceCanonicalWriter &writer, JobMetricCounter serial,
	const KernelPerformanceRangePlan &range)
{
	return writer.u64(3, serial) && writer.u64(4, range.dispatchOrdinal) && writer.u32(5, range.rangeOrdinal) &&
		writer.u32(6, range.bodyKind) && writer.u64(7, range.begin) && writer.u64(8, range.end) &&
		writer.u64(9, range.operationCount);
}
struct CallbackGuard
{
	explicit CallbackGuard(bool &active) : flag(active) { flag = true; }
	~CallbackGuard() { flag = false; }
	bool &flag;
};
}

struct KernelPerformanceReferenceLedger::State
{
	struct Stream
	{
		Stream() : ordinal(0) {}
		KernelPerformanceReferenceStream metric;
		KernelPerformanceCanonicalWriter input, output, commit;
		JobMetricCounter ordinal;
	};
	struct Pending
	{
		Pending() : active(false), stream(0), frame(0), serial(0), ordinal(0), operations(0), serialTime(0), attemptSerial(0) {}
		bool active;
		unsigned stream, frame;
		JobMetricCounter serial, ordinal, operations, serialTime, attemptSerial;
	};
	struct Attempt
	{
		Attempt() : active(false), decisionSeen(false), finished(false), captured(false), admitted(false),
			dispatchSeen(false), serial(0), decisionOrdinal(0), capturedOperations(0), inputSchema(0),
			plannedRanges(0), releasedRanges(0), identity(), dispatch(), successfulRanges(true),
			validated(false), commitObserved(false), committed(false), validationOrdinal(0),
			finishOffset(0), dispatchOffset(0), planScan(0), sourceStart(0), beginLogical(0),
			nativeStage(0), nativeClass(0), nativeWorkers(0), nativeOwnerID(0), nativeArmed(false), nativeTiming(0), nativeBatch() {}
		bool active, decisionSeen, finished, captured, admitted, dispatchSeen;
		JobMetricCounter serial, decisionOrdinal, capturedOperations;
		unsigned inputSchema, plannedRanges, releasedRanges;
		KernelPerformanceAttemptIdentity identity;
		KernelPerformanceDigest inputDigest;
		KernelPerformanceDispatchPlan dispatch;
		bool successfulRanges, validated, commitObserved, committed;
		JobMetricCounter validationOrdinal, finishOffset, dispatchOffset, planScan, sourceStart;
		KernelPerformanceDigest outputDigest;
		KernelPerformanceReferenceBatch batch;
		JobMetricCounter beginLogical;
		unsigned nativeStage, nativeClass, nativeWorkers, nativeOwnerID;
		bool nativeArmed;
		KernelPerformanceLedger *nativeTiming;
		KernelPerformanceBatch nativeBatch;
	};
	struct Range
	{
		Range() : active(false), cached(false), bodyStarted(false), bodyFinished(false), attemptSerial(0), plan(), actual(),
			budgeted(false), budgetReached(false), budgetBegin(0), budgetEnd(0), budgetGrantOffset(0),
			budgetGrantRequest(0), budgetImportRequest(0) {}
		bool active, cached, bodyStarted, bodyFinished;
		JobMetricCounter attemptSerial;
		KernelPerformanceRangePlan plan;
		KernelPerformanceRangeProgress actual;
		bool budgeted, budgetReached;
		// Only live-range cursors, not an index or copy of its request records.
		JobMetricCounter budgetBegin, budgetEnd, budgetGrantOffset, budgetGrantRequest, budgetImportRequest;
	};
	struct AttemptOrder
	{
		AttemptOrder() : seen(false), sample(0), ordinal(0) {}
		bool seen;
		JobMetricCounter sample, ordinal;
	};
	struct Window
	{
		Window() : seen(false), open(false), phaseOpen(false), sample(0), lastSample(0),
			entry(0), authority(0), actual(0), nextPhase(0), controlState(0), lastCompleted(0) {}
		bool seen, open, phaseOpen;
		JobMetricCounter sample, lastSample;
		unsigned entry, authority, actual, nextPhase, controlState, lastCompleted;
	};
	struct Span
	{
		Span() : active(false), identity(), classification(0), firstLogical(0), lastLogical(0),
			firstAttempt(0), lastAttempt(0), attempts(0) {}
		bool active;
		KernelPerformanceAttemptIdentity identity;
		unsigned classification;
		JobMetricCounter firstLogical, lastLogical, firstAttempt, lastAttempt, attempts;
	};
	State() : streamCount(0), openCount(0), nextSerial(0), lastClock(0), busy(false), clock(0), context(0),
		traceOwner(0), traceAppend(0), traceContext(0), attempts(0), ranges(0), attemptCapacity(0), rangeCapacity(0), nextAttemptSerial(0),
		readAt(0), sourceBytes(0), readBuffer(0), readOffset(0), readCount(0), prevalidating(false),
		bodyTiming(0), bodyProbe(0), bodyRange(0), nextBody(0), nextOwnerSerial(0),
		budgetImportRange(~0U), budgetPending(false), ownerBudgetUsed(false), pendingCollision(~0U),
		prefixCount(0), nativeFallback(0), nativeFallbackSlot(~0U), nextBypass(0), contacts(0), insertedContacts(0),
		fullContactReady(false), restrictedReapSerial(0), preflightCapturedOperations(0) {}
	~State() { delete[] readBuffer; delete[] ranges; delete[] attempts; }
	bool consuming() const { return readAt != 0; }
	bool readBytes(JobMetricCounter offset, unsigned char *out, unsigned count)
	{
		if (offset > sourceBytes || count > sourceBytes - offset)
			return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		while (count != 0)
		{
			if (readCount == 0 || offset < readOffset || offset - readOffset >= readCount)
			{
				readOffset = offset;
				readCount = static_cast<unsigned>((sourceBytes - offset < 65536) ? sourceBytes - offset : 65536);
				const bool previous = busy; busy = true;
				bool accepted = false;
				try { accepted = readAt(traceContext, readOffset, readBuffer, readCount); } catch (...) {}
				busy = previous;
				if (!accepted) { readCount = 0; return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_IO); }
				if (traceOwner->m_foreignCall.load(std::memory_order_acquire))
					return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_OWNER);
				if (traceOwner->m_snapshot.errors != 0) return false;
			}
			const unsigned within = static_cast<unsigned>(offset - readOffset);
			const unsigned copied = count < readCount - within ? count : readCount - within;
			memcpy(out, readBuffer + within, copied);
			out += copied; offset += copied; count -= copied;
		}
		return true;
	}
	bool verifySourceHash()
	{
		BCRYPT_ALG_HANDLE algorithm = 0; BCRYPT_HASH_HANDLE hash = 0;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0) != 0)
			return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_HASH);
		bool valid = BCryptCreateHash(algorithm, &hash, 0, 0, 0, 0, 0) == 0;
		KernelPerformanceDigest digest;
		// Invalidate the read cache: final binding checks must observe the real
		// immutable backend again, not trust bytes cached before a mutation.
		readCount = 0;
		for (JobMetricCounter offset = 0; valid && offset < sourceBytes;)
		{
			unsigned char first;
			valid = readBytes(offset, &first, 1);
			if (valid) valid = BCryptHashData(hash, readBuffer, readCount, 0) == 0;
			offset += readCount;
		}
		if (valid) digest.valid = BCryptFinishHash(hash, digest.bytes, 32, 0) == 0;
		if (hash != 0) BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		readCount = 0;
		return (valid && digest.equals(sourceDigest)) || traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_BINDING);
	}
	struct Event
	{
		Event() : v(), end(0) {}
		JobMetricCounter v[30], end;
		KernelPerformanceDigest digest;
		KernelPerformanceRangePlan range() const
		{ return { v[4], static_cast<unsigned>(v[5]), static_cast<unsigned>(v[6]), v[7], v[8], v[9] }; }
		KernelPerformanceDispatchPlan dispatch() const
		{ return { v[4], static_cast<unsigned>(v[5]), static_cast<unsigned>(v[6]), static_cast<unsigned>(v[7]), v[8], v[9], v[10] }; }
		KernelPerformanceAttemptDecision decision() const
		{ return { v[4], static_cast<unsigned>(v[5]), static_cast<unsigned>(v[6]), static_cast<unsigned>(v[7]), v[8] != 0,
			digest, static_cast<KernelPerformanceAdmission>(v[9]), static_cast<unsigned>(v[10]), static_cast<unsigned>(v[11]), v[12], v[13], v[14] }; }
		KernelPerformanceRangeProgress progress() const
		{ return { { v[10] != 0, static_cast<unsigned>(v[11]), v[12], v[13], v[14],
			{ static_cast<unsigned>(v[15]), v[16], v[17] }, { static_cast<unsigned>(v[18]), v[19], v[20] },
			static_cast<KernelPerformanceRangeTerminal>(v[21]) }, static_cast<KernelPerformancePublication>(v[22]) }; }
		KernelPerformanceRequestBudget budget() const
		{
			KernelPerformanceRequestBudget result;
			result.requestOrdinal = v[6]; result.disposition = static_cast<KernelPerformanceRequestBudgetDisposition>(v[8]);
			result.grantSite = static_cast<unsigned>(v[9]); result.localGrantOrdinal = v[10];
			result.requestedBytes = v[11]; result.grantedBytes = v[12]; result.refundSite = static_cast<unsigned>(v[13]);
			result.localRefundOrdinal = v[14]; result.refundedBytes = v[15]; result.consumedBytes = v[16];
			return result;
		}
	};
	bool number(JobMetricCounter &at, unsigned tag, unsigned type, JobMetricCounter &value)
	{
		unsigned char bytes[13]; const unsigned width = type == 3 ? 8 : (type == 5 ? 1 : 4);
		if (!readBytes(at, bytes, width + 5)) return false;
		if (bytes[0] != type || bytes[1] != tag || bytes[2] != 0 || bytes[3] != 0 || bytes[4] != 0)
			return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		value = 0;
		for (unsigned i = 0; i != width; ++i) value |= static_cast<JobMetricCounter>(bytes[5 + i]) << (8 * i);
		if (type == 5 && value > 1) return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		at += width + 5; return true;
	}
	bool digestAt(JobMetricCounter &at, unsigned tag, KernelPerformanceDigest &digest)
	{
		JobMetricCounter value;
		if (!number(at, tag, 6, value) || value != 4) return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		for (unsigned i = 0; i != 4; ++i)
		{
			if (!number(at, tag + 1 + i, 3, value)) return false;
			for (unsigned b = 0; b != 8; ++b) digest.bytes[8 * i + b] = static_cast<unsigned char>(value >> (8 * b));
		}
		digest.valid = true; return true;
	}
	bool eventAt(JobMetricCounter at, Event &event)
	{
		event = Event();
		if (!number(at, 1, 1, event.v[1]) || !number(at, 2, 3, event.v[2])) return false;
		const unsigned kind = static_cast<unsigned>(event.v[1]);
		if (kind < 2 || kind > 13) return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		if (kind == 12)
		{
			for (unsigned tag = 3; tag <= 16; ++tag)
				if (!number(at, tag, tag == 3 || (tag >= 5 && tag <= 9) ? 1 : 3, event.v[tag])) return false;
			if (!digestAt(at, 17, event.digest)) return false;
			event.end = at; return true;
		}
		if (kind == 2)
		{
			if (!number(at, 3, 1, event.v[3]) || !number(at, 4, 3, event.v[4])) return false;
			if (event.v[3] >= KERNEL_WINDOW_BEGIN && event.v[3] <= KERNEL_WINDOW_DEFERRED_START_CONSUMED)
			{
				for (unsigned tag = 5; tag <= 8; ++tag)
					if (!number(at, tag, 1, event.v[tag])) return false;
			}
			else if (event.v[3] != 1 && event.v[3] != 2)
				return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
			event.end = at; return true;
		}
		unsigned last = kind == 3 ? 9 : kind == 4 ? 14 : kind == 5 ? 5 :
			kind == 6 ? 10 : kind == 7 ? 9 : kind == 8 ? 22 : kind == 9 ? 9 : kind == 10 ? 9 : kind == 13 ? 16 : 29;
		for (unsigned tag = 3; tag <= last; ++tag)
		{
			unsigned type = 3;
			if ((kind == 3 && (tag == 4 || tag == 5 || tag == 7 || tag == 8)) ||
				(kind == 4 && (tag == 5 || tag == 6 || tag == 7 || tag == 9 || tag == 10 || tag == 11)) ||
				(kind == 5 && tag == 4) || (kind == 6 && tag >= 5 && tag <= 7) ||
				((kind == 7 || kind == 8) && (tag == 5 || tag == 6)) ||
				(kind == 8 && (tag == 11 || tag == 15 || tag == 18 || tag == 21 || tag == 22)) ||
				(kind == 9 && tag >= 4 && tag <= 6) || (kind == 10 && tag >= 4 && tag <= 6) ||
				(kind == 13 && (tag == 5 || tag == 7 || tag == 8 || tag == 9 || tag == 13))) type = 1;
			if ((kind == 4 && tag == 8) || (kind == 8 && tag == 10) || (kind == 9 && tag >= 7) ||
				(kind == 11 && (tag == 14 || tag == 15))) type = 5;
			if (!number(at, tag, type, event.v[tag])) return false;
		}
		if (kind == 4 && !digestAt(at, 15, event.digest)) return false;
		if (kind == 5 && !digestAt(at, 6, event.digest)) return false;
		if (kind == 13 && (event.v[7] != 1 || !requestBudgetValid(event.budget())))
			return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		if (kind == 9 && event.v[9] != 0)
		{
			if (!number(at, 10, 1, event.v[10]) || !number(at, 11, 3, event.v[11]) ||
				!number(at, 12, 3, event.v[12]) || !digestAt(at, 13, event.digest) ||
				!number(at, 18, 5, event.v[18])) return false;
		}
		else if (kind == 9 && event.v[5] == 0x5002)
		{
			if ((event.v[6] != 1 && event.v[6] != 2) || !number(at, 19, 3, event.v[19]) ||
				!number(at, 20, 3, event.v[20]) || !digestAt(at, 21, event.digest))
				return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
		}
		event.end = at; return true;
	}
	bool requestBudgetAt(const Range &range, JobMetricCounter at, JobMetricCounter request, Event &event)
	{
		if (!eventAt(at, event)) return false;
		if (event.v[1] != 13 || event.v[3] != range.attemptSerial || event.v[4] != range.plan.dispatchOrdinal ||
			event.v[5] != range.plan.rangeOrdinal || event.v[6] != request)
			return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
		return true;
	}
	bool finishRequestBudgetTraversal(Range &range)
	{
		while (range.budgetGrantOffset < range.budgetEnd)
		{
			Event event;
			if (!requestBudgetAt(range, range.budgetGrantOffset, range.budgetGrantRequest, event)) return false;
			if (event.v[8] != KERNEL_REQUEST_BUDGET_NOT_REACHED)
				return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
			range.budgetGrantOffset = event.end; ++range.budgetGrantRequest;
		}
		return (range.budgetGrantOffset == range.budgetEnd && range.budgetGrantRequest == range.plan.end) ||
			traceOwner->failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
	}
	bool sourceFinish(Attempt &attempt, Event &event)
	{
		if (attempt.finishOffset != 0) return eventAt(attempt.finishOffset, event);
		for (JobMetricCounter at = attempt.sourceStart; at < sourceBytes;)
		{
			if (!eventAt(at, event)) return false;
			if (event.v[1] == 9 && event.v[3] == attempt.serial)
			{ attempt.finishOffset = at; return true; }
			at = event.end;
		}
		return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	}
	bool now(JobMetricCounter &value)
	{
		value = clock != 0 ? clock(context) : 0;
		if (value == 0 || value == ~static_cast<JobMetricCounter>(0) || value < lastClock) return false;
		lastClock = value;
		return true;
	}
	static bool appendTrace(void *context, const unsigned char *bytes, unsigned count)
	{
		State &state = *static_cast<State *>(context);
		KernelPerformanceReferenceLedger &ledger = *state.traceOwner;
		KernelPerformanceTraceSnapshot &trace = ledger.m_snapshot.trace;
		if (trace.byteCount > trace.limits.maximumBytes || count > trace.limits.maximumBytes - trace.byteCount)
			return ledger.failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		bool accepted = false;
		try
		{
			if (state.consuming())
			{
				unsigned char compared[64]; unsigned remaining = count;
				accepted = true;
				while (accepted && remaining != 0)
				{
					const unsigned chunk = remaining < sizeof(compared) ? remaining : sizeof(compared);
					accepted = state.readBytes(trace.byteCount + count - remaining, compared, chunk) &&
						memcmp(compared, bytes + count - remaining, chunk) == 0;
					remaining -= chunk;
				}
				if (!accepted) return ledger.failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
			}
			else accepted = state.traceAppend(state.traceContext, bytes, count);
		}
		catch (...) { return ledger.failTrace(KERNEL_REFERENCE_ERROR_TRACE_IO); }
		if (!accepted) return ledger.failTrace(KERNEL_REFERENCE_ERROR_TRACE_IO);
		trace.byteCount += count;
		if (ledger.m_foreignCall.load(std::memory_order_acquire)) return ledger.failTrace(KERNEL_PERFORMANCE_ERROR_OWNER);
		return ledger.m_snapshot.errors == 0 && trace.errors == 0;
	}
	bool traceRecord(unsigned kind)
	{
		if (budgetImportRange != ~0U && kind != 13 && kind != 8)
			return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
		if (nativeFallback != 0 || (restrictedReapSerial != 0 && kind != 10))
			return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
		if (!materializeCollision() || !flushSpan()) return false;
		return rawRecord(kind, 1);
	}
	bool logical(JobMetricCounter count)
	{
		KernelPerformanceTraceSnapshot &trace = traceOwner->m_snapshot.trace;
		if (trace.logicalEventCount > trace.limits.maximumLogicalEvents ||
			count > trace.limits.maximumLogicalEvents - trace.logicalEventCount)
			return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		trace.logicalEventCount += count;
		return true;
	}
	bool rawRecord(unsigned kind, JobMetricCounter logicalCount)
	{
		KernelPerformanceTraceSnapshot &trace = traceOwner->m_snapshot.trace;
		if (trace.recordCount >= trace.limits.maximumRecords || !logical(logicalCount))
			return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		++trace.recordCount;
		return traceWriter.u32(1, kind) && traceWriter.u64(2, trace.recordCount);
	}
	bool materializeCollision();
	bool flushSpan();
	bool spanRecord(const Event &event);
	bool writeSpan(const Event &event);
	bool decisionRecord(JobMetricCounter serial, const KernelPerformanceAttemptDecision &decision, bool counted);
	void trackNativeDecision(Attempt &attempt, const KernelPerformanceAttemptDecision &decision);
	static bool writeCollisionFacts(KernelPerformanceCanonicalWriter &writer, unsigned site, const void *facts);
	static KernelPerformanceAttemptDecision collisionDecision(unsigned site, const void *facts, unsigned workers);
	const void *prefixFacts(unsigned site) const;
	unsigned streamCount, openCount;
	JobMetricCounter nextSerial, lastClock;
	bool busy;
	KernelPerformanceClock clock;
	void *context;
	Stream streams[KERNEL_PERFORMANCE_MAXIMUM_STREAMS];
	Pending pending[KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES];
	KernelPerformanceReferenceLedger *traceOwner;
	KernelPerformanceTraceAppend traceAppend;
	void *traceContext;
	KernelPerformanceCanonicalWriter traceWriter;
	Attempt *attempts;
	Range *ranges;
	unsigned attemptCapacity, rangeCapacity;
	JobMetricCounter nextAttemptSerial;
	AttemptOrder attemptOrder[KERNEL_PERFORMANCE_KERNEL_COUNT][2];
	Window window;
	KernelPerformanceTraceReadAt readAt;
	JobMetricCounter sourceBytes;
	KernelPerformanceDigest sourceDigest;
	unsigned char *readBuffer;
	JobMetricCounter readOffset;
	unsigned readCount;
	bool prevalidating;
	KernelPerformanceLedger *bodyTiming;
	KernelPerformanceCheckpointProbe *bodyProbe;
	unsigned bodyRange;
	JobMetricCounter nextBody, nextOwnerSerial;
	KernelPerformanceInlineBody body;
	KernelPerformanceInlineOwnerSerial ownerSerial;
	KernelPerformanceInterval timedBody, timedOwnerSerial;
	unsigned budgetImportRange;
	bool budgetPending, ownerBudgetUsed;
	KernelPerformanceRequestBudget pendingBudget;
	unsigned pendingCollision, prefixCount, nativeFallback, nativeFallbackSlot;
	NativeCollisionPolicyFacts policyFacts;
	NativeCollisionCountFacts countFacts;
	NativeCollisionReserveFacts reserveFacts;
	NativeCollisionCaptureFacts captureFacts;
	KernelPerformanceCanonicalWriter semanticWriter;
	Span span;
	KernelPerformanceDeterministicBypass bypass;
	JobMetricCounter nextBypass, contacts, insertedContacts;
	bool fullContactReady;
	KernelPerformanceDigest fullContactDigest;
	JobMetricCounter restrictedReapSerial, preflightCapturedOperations;
};

const void *KernelPerformanceReferenceLedger::State::prefixFacts(unsigned site) const
{
	switch (site) { case 1: return &policyFacts; case 2: return &countFacts; case 3: return &reserveFacts; case 4: return &captureFacts; }
	return 0;
}
bool KernelPerformanceReferenceLedger::State::writeCollisionFacts(KernelPerformanceCanonicalWriter &w,
	unsigned site, const void *facts)
{
	if (site == 1)
	{
		const auto &f = *static_cast<const NativeCollisionPolicyFacts *>(facts);
		return w.boolean(1, f.authoritativeRequested) && w.boolean(2, f.shadowRequested) &&
			w.boolean(3, f.multiplayerPolicyBlocked) && w.boolean(4, f.schedulerReady) && w.u32(5, f.configuredWorkers);
	}
	if (site == 2)
	{
		const auto &f = *static_cast<const NativeCollisionCountFacts *>(facts);
		return w.u32(1, f.admissionOwnerID) && w.u32(2, f.cellCount) && w.u32(3, f.occupantCount) && w.boolean(4, f.countValid);
	}
	if (site == 3)
	{
		const auto &f = *static_cast<const NativeCollisionReserveFacts *>(facts);
		return w.u32(1, f.requiredCells) && w.u32(2, f.requiredOccupants) && w.u32(3, f.beforeCellCapacity) &&
			w.u32(4, f.beforeOccupantCapacity) && w.u32(5, f.afterCellCapacity) && w.u32(6, f.afterOccupantCapacity) && w.u32(7, f.reserveOutcome);
	}
	if (site == 4)
	{
		const auto &f = *static_cast<const NativeCollisionCaptureFacts *>(facts);
		return w.u32(1, f.flatIndex) && w.boolean(2, f.snapshotValid) && w.boolean(3, f.ownerPresent) &&
			w.u32(4, f.capturedOwnerID) && w.boolean(5, f.participantIdentitiesValid) && w.boolean(6, f.ownerIdentityUnchanged);
	}
	if (site == 5)
	{
		const auto &f = *static_cast<const NativeCollisionClassFacts *>(facts);
		return w.u32(1, f.occupantCount) && w.u32(2, f.minimumInputs) && w.u32(3, f.samplerEncounterCount) &&
			w.u32(4, f.samplerSampleCount) && w.boolean(5, f.spreadEvaluated) && w.boolean(6, f.usefulSpread);
	}
	return false;
}
KernelPerformanceAttemptDecision KernelPerformanceReferenceLedger::State::collisionDecision(
	unsigned site, const void *facts, unsigned workers)
{
	KernelPerformanceAttemptDecision d = {};
	d.decisionOrdinal = site - 1; d.site = site; d.reasonSchema = 1; d.reason = 1;
	d.deterministicEligible = true; d.sourceConfiguredWorkers = workers;
	if (site == 1)
	{
		const auto &f = *static_cast<const NativeCollisionPolicyFacts *>(facts);
		d.sourceConfiguredWorkers = f.configuredWorkers;
		d.reason = f.multiplayerPolicyBlocked ? 2 : !f.schedulerReady ? 3 : 1;
		d.deterministicEligible = d.reason == 1;
	}
	if (site == 2) { d.reason = static_cast<const NativeCollisionCountFacts *>(facts)->countValid ? 1 : 2; d.deterministicEligible = d.reason == 1; }
	if (site == 3) { d.reason = static_cast<const NativeCollisionReserveFacts *>(facts)->reserveOutcome; d.deterministicEligible = d.reason == 1 || d.reason == 2; }
	if (site == 4)
	{
		const auto &f = *static_cast<const NativeCollisionCaptureFacts *>(facts);
		d.reason = !f.snapshotValid ? 2 : !f.ownerPresent ? 3 : !f.participantIdentitiesValid || !f.ownerIdentityUnchanged ? 4 : 1;
		d.deterministicEligible = d.reason == 1;
	}
	if (site == 5)
	{
		const auto &f = *static_cast<const NativeCollisionClassFacts *>(facts);
		d.reasonSchema = 0x5002; d.reason = f.occupantCount < 256 ? 1 : !f.usefulSpread ? 2 : 3;
		d.deterministicEligible = d.reason == 3;
	}
	return d;
}
void KernelPerformanceReferenceLedger::State::trackNativeDecision(Attempt &a, const KernelPerformanceAttemptDecision &d)
{
	if (a.identity.workKind != KERNEL_PERFORMANCE_COLLISION || a.identity.subtype != 0 || a.nativeStage > 5) return;
	if (a.nativeStage == 5) { a.nativeStage = 6; return; }
	const unsigned site = a.nativeStage + 1;
	bool valid = d.site == site && d.decisionOrdinal == site - 1 && d.admission == KERNEL_ADMISSION_NOT_REQUESTED &&
		d.dynamicFactsKnownMask == 0 && d.pendingJobs == 0 && d.outstandingJobs == 0 && d.activeSlots == 0;
	if (site == 1) a.nativeWorkers = d.sourceConfiguredWorkers;
	valid = valid && d.sourceConfiguredWorkers == a.nativeWorkers && a.nativeWorkers > 1;
	if (site <= 2) valid = valid && d.reasonSchema == 1 && d.reason == 1 && d.deterministicEligible;
	if (site == 3) valid = valid && d.reasonSchema == 1 && (d.reason == 1 || d.reason == 2) && d.deterministicEligible;
	if (site == 4) valid = valid && d.reasonSchema == 1 && ((d.reason == 1 && d.deterministicEligible) || (d.reason == 4 && !d.deterministicEligible));
	if (site == 5) valid = valid && d.reasonSchema == 0x5002 && d.reason >= 1 && d.reason <= 3 && d.deterministicEligible == (d.reason == 3);
	a.nativeStage = valid ? site : 6;
	if (valid && site == 5) a.nativeClass = d.reason;
}
bool KernelPerformanceReferenceLedger::State::decisionRecord(JobMetricCounter serial,
	const KernelPerformanceAttemptDecision &d, bool counted)
{
	return rawRecord(4, counted ? 0 : 1) && traceWriter.u64(3, serial) && traceWriter.u64(4, d.decisionOrdinal) &&
		traceWriter.u32(5, d.site) && traceWriter.u32(6, d.reasonSchema) && traceWriter.u32(7, d.reason) &&
		traceWriter.boolean(8, d.deterministicEligible) && traceWriter.u32(9, static_cast<unsigned>(d.admission)) &&
		traceWriter.u32(10, d.sourceConfiguredWorkers) && traceWriter.u32(11, d.dynamicFactsKnownMask) &&
		traceWriter.u64(12, d.pendingJobs) && traceWriter.u64(13, d.outstandingJobs) && traceWriter.u64(14, d.activeSlots) &&
		appendDigest(traceWriter, d.deterministicFacts, 15);
}
bool KernelPerformanceReferenceLedger::State::materializeCollision()
{
	if (pendingCollision == ~0U) return true;
	if (nativeFallback != 0 || !flushSpan()) return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	Attempt &a = attempts[pendingCollision]; const unsigned count = prefixCount;
	pendingCollision = ~0U; prefixCount = 0;
	if (!rawRecord(3, 0) || !traceWriter.u64(3, a.serial) || !traceWriter.u32(4, a.identity.workKind) ||
		!traceWriter.u32(5, a.identity.subtype) || !traceWriter.u64(6, a.identity.sampleOrdinal) ||
		!traceWriter.u32(7, a.identity.phase) || !traceWriter.u32(8, a.identity.ownerFrame) || !traceWriter.u64(9, a.identity.attemptOrdinal)) return false;
	if (consuming()) a.sourceStart = traceOwner->m_snapshot.trace.byteCount;
	for (unsigned site = 1; site <= count; ++site)
	{
		const void *facts = prefixFacts(site);
		auto d = collisionDecision(site, facts, a.nativeWorkers);
		if (!semanticWriter.begin(0x5100 + site, 0, 0, true) || !writeCollisionFacts(semanticWriter, site, facts)) return false;
		d.deterministicFacts = semanticWriter.finish();
		if (!d.deterministicFacts.valid || !decisionRecord(a.serial, d, true)) return false;
	}
	return true;
}
bool KernelPerformanceReferenceLedger::State::writeSpan(const Event &event)
{
	if (!rawRecord(12, 0)) return false;
	for (unsigned tag = 3; tag <= 16; ++tag)
	{
		if (tag == 3 || (tag >= 5 && tag <= 9)) { if (!traceWriter.u32(tag, static_cast<unsigned>(event.v[tag]))) return false; }
		else if (!traceWriter.u64(tag, event.v[tag])) return false;
	}
	return appendDigest(traceWriter, event.digest, 17);
}
bool KernelPerformanceReferenceLedger::State::flushSpan()
{
	if (!span.active) return true;
	if (nativeFallback != 0 || span.attempts == 0) return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	Event e; e.v[3] = 0x5002; e.v[4] = span.identity.sampleOrdinal; e.v[5] = span.identity.ownerFrame;
	e.v[6] = span.identity.phase; e.v[7] = KERNEL_PERFORMANCE_COLLISION; e.v[8] = 0; e.v[9] = span.classification;
	e.v[10] = span.firstLogical; e.v[11] = span.lastLogical; e.v[12] = span.firstAttempt; e.v[13] = span.lastAttempt;
	e.v[14] = span.attempts; e.v[15] = span.attempts * 9; e.v[16] = semanticWriter.bytesWritten();
	e.digest = semanticWriter.finish();
	if (!e.digest.valid || !writeSpan(e)) return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_HASH);
	if (!checkedAdd(traceOwner->m_snapshot.trace.coalescedSpanCount, 1)) return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	span = Span(); return true;
}
bool KernelPerformanceReferenceLedger::State::spanRecord(const Event &e)
{
	auto &t = traceOwner->m_snapshot.trace;
	const JobMetricCounter count = e.v[14];
	if (!prevalidating || restrictedReapSerial != 0 || e.v[3] != 0x5002 || e.v[4] == 0 || e.v[6] >= KERNEL_PHASE_COUNT ||
		e.v[7] != KERNEL_PERFORMANCE_COLLISION || e.v[8] != 0 || (e.v[9] != 1 && e.v[9] != 2) || count == 0 || count > 1024 ||
		e.v[15] != count * 9 || t.logicalEventCount == ~JobMetricCounter(0) ||
		e.v[10] != t.logicalEventCount + 1 || e.v[10] > ~JobMetricCounter(0) - (count * 9 - 1) ||
		e.v[11] != e.v[10] + count * 9 - 1 || e.v[13] < e.v[12] || count - 1 > e.v[13] - e.v[12] ||
		e.v[16] < 91 + count * 808 || t.observationSealed)
		return traceOwner->failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
	if (window.seen && (!window.open || !window.phaseOpen || e.v[4] != window.sample || e.v[6] != window.nextPhase))
		return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	AttemptOrder &order = attemptOrder[KERNEL_PERFORMANCE_COLLISION][0];
	if (order.seen && (e.v[4] < order.sample || (e.v[4] == order.sample && e.v[12] <= order.ordinal)))
		return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (t.residentAttemptCount >= attemptCapacity || t.attemptCount > t.limits.maximumAttempts || count > t.limits.maximumAttempts - t.attemptCount ||
		count > ~JobMetricCounter(0) - nextAttemptSerial || !logical(count * 9))
		return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	if (!checkedAdd(t.attemptCount, count) || !checkedAdd(t.notAdmittedAttemptCount, count) || !checkedAdd(t.reapCount, count) ||
		!checkedAdd(t.capturedAttemptCount, count) || !checkedAdd(t.coalescedAttemptCount, count) || !checkedAdd(t.coalescedSpanCount, 1))
		return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	if (t.residentAttemptHighWater < t.residentAttemptCount + 1) t.residentAttemptHighWater = t.residentAttemptCount + 1;
	nextAttemptSerial += count; order.seen = true; order.sample = e.v[4]; order.ordinal = e.v[13];
	return writeSpan(e);
}

KernelPerformanceReferenceLedger::KernelPerformanceReferenceLedger() : m_state(0), m_owner(GetCurrentThreadId()),
	m_foreignCall(false), m_runMode(KERNEL_REFERENCE_DISABLED) {}
KernelPerformanceReferenceLedger::~KernelPerformanceReferenceLedger() { delete m_state; }
KernelPerformanceReferenceLedger &KernelPerformanceReferenceLedger::instance() { static KernelPerformanceReferenceLedger ledger; return ledger; }
KernelPerformanceReferenceMode KernelPerformanceReferenceLedger::mode() const noexcept
{
	if (m_owner.load(std::memory_order_acquire) != GetCurrentThreadId()) return KERNEL_REFERENCE_DISABLED;
	if (m_foreignCall.load(std::memory_order_acquire) || m_snapshot.generation == 0 ||
		m_snapshot.frozen || m_snapshot.errors != 0 || m_state == 0)
		return KERNEL_REFERENCE_DISABLED;
	return m_snapshot.mode;
}
KernelPerformanceReferenceMode KernelPerformanceReferenceLedger::runMode() const noexcept
{
	return m_runMode.load(std::memory_order_acquire);
}
bool KernelPerformanceReferenceLedger::traceRequested() const noexcept
{
	return m_owner.load(std::memory_order_acquire) == GetCurrentThreadId() &&
		m_snapshot.trace.requested;
}
bool KernelPerformanceReferenceLedger::owner() noexcept
{
	if (m_owner.load(std::memory_order_acquire) == GetCurrentThreadId()) return true;
	m_foreignCall.store(true, std::memory_order_release);
	return false;
}
bool KernelPerformanceReferenceLedger::failTrace(unsigned error) noexcept
{
	if (!m_snapshot.frozen)
	{
		m_snapshot.errors |= error;
		if (m_snapshot.trace.requested) m_snapshot.trace.errors |= error;
		if (m_state != 0 && m_state->body.valid() && m_state->bodyTiming != 0)
			m_state->bodyTiming->fail(error);
	}
	return false;
}
bool KernelPerformanceReferenceLedger::traceReady() noexcept
{
	if (!owner()) return false;
	if (m_snapshot.frozen || !m_snapshot.trace.requested) return false;
	if (m_foreignCall.load(std::memory_order_acquire)) return failTrace(KERNEL_PERFORMANCE_ERROR_OWNER);
	if (m_state == 0 || m_snapshot.errors != 0 || m_snapshot.trace.errors != 0) return false;
	if (m_state->busy) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	return true;
}
unsigned KernelPerformanceReferenceLedger::traceAttemptSlot(KernelPerformanceAttempt attempt) noexcept
{
	if (attempt.m_owner != this || attempt.m_generation != m_snapshot.generation || !attempt.valid() ||
		attempt.m_slot >= m_state->attemptCapacity || !m_state->attempts[attempt.m_slot].active ||
		m_state->attempts[attempt.m_slot].serial != attempt.m_serial)
	{
		failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		return ~0U;
	}
	return attempt.m_slot;
}
bool KernelPerformanceReferenceLedger::beginRun(KernelPerformanceReferenceMode mode,
	KernelPerformanceClock clock, void *clockContext) noexcept
{
	if (!owner()) return false;
	if (m_snapshot.generation != 0 && !m_snapshot.frozen)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE; return false; }
	if (mode < KERNEL_REFERENCE_DISABLED || mode > KERNEL_REFERENCE_SERIAL_ORACLE)
	{
		if (!m_snapshot.frozen) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY;
		return false;
	}
	if (m_snapshot.generation == ~static_cast<JobMetricCounter>(0))
	{
		if (!m_snapshot.frozen) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OVERFLOW;
		return false;
	}
	const JobMetricCounter generation = m_snapshot.generation + 1;
	delete m_state;
	m_state = 0;
	m_snapshot = KernelPerformanceReferenceSnapshot();
	m_snapshot.mode = mode;
	m_snapshot.generation = generation;
	m_foreignCall.store(false, std::memory_order_release);
	// Latch before allocation: a failed diagnostic setup must not silently
	// change this run's execution identity. Rejected reconfiguration above
	// leaves the previous identity untouched.
	m_runMode.store(mode, std::memory_order_release);
	if (mode == KERNEL_REFERENCE_DISABLED) return true;
	m_state = new (std::nothrow) State;
	if (m_state == 0) { m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_CAPACITY; return false; }
	m_state->clock = clock != 0 ? clock : LiveSimulationPhaseClockNowNanoseconds;
	m_state->context = clockContext;
	return true;
}
bool KernelPerformanceReferenceLedger::beginRun(const KernelPerformanceReferenceRunOptions &options) noexcept
{
	if (!owner()) return false;
	if (m_snapshot.generation != 0 && !m_snapshot.frozen) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (options.trace.mode == KERNEL_TRACE_DISABLED)
		return beginRun(options.mode, options.clock, options.clockContext);
	const bool consume = options.trace.mode == KERNEL_TRACE_CONSUME;
	if ((!consume && (options.trace.mode != KERNEL_TRACE_RECORD || options.mode != KERNEL_REFERENCE_THROUGHPUT_BINDING)) ||
		(consume && options.mode != KERNEL_REFERENCE_PHASE_BASELINE_BINDING))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	const KernelPerformanceTraceOptions &requested = options.trace;
	if ((!consume && requested.append == 0) || (consume && (requested.append != 0 || requested.readAt == 0 ||
		requested.sourceByteCount == 0 || !requested.sourceTraceDigest.valid || !requested.sourceReceiptDigest.valid)) ||
		!requested.binding.nativeRunIdentity.valid || !requested.binding.executable.valid ||
		!requested.binding.fixture.valid || !requested.binding.sourcePolicy.valid)
		return failTrace(KERNEL_REFERENCE_ERROR_TRACE_BINDING);
	if (requested.limits.maximumBytes == 0 || requested.limits.maximumRecords == 0 ||
		requested.limits.maximumLogicalEvents == 0 || requested.limits.maximumAttempts == 0 ||
		requested.limits.maximumRanges == 0 || requested.residentAttemptCapacity == 0 ||
		requested.residentRangeCapacity == 0)
		return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	if (requested.residentAttemptCapacity > ~0U || requested.residentRangeCapacity > ~0U ||
		requested.residentAttemptCapacity > static_cast<size_t>(-1) / sizeof(State::Attempt) ||
		requested.residentRangeCapacity > static_cast<size_t>(-1) / sizeof(State::Range))
		return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	const JobMetricCounter previousGeneration = m_snapshot.generation;
	const bool started = beginRun(consume ? KERNEL_REFERENCE_THROUGHPUT_BINDING : options.mode, options.clock, options.clockContext);
	if (m_snapshot.generation == previousGeneration) return false;
	// The baseline role is latched before any source callback or allocation.
	m_snapshot.mode = options.mode;
	m_runMode.store(options.mode, std::memory_order_release);
	if (!started) return failTrace(m_snapshot.errors);
	if (!prepareTrace(options)) return false;
	if (!consume) return true;
	m_state->prevalidating = true;
	if (!validateSource()) return false;
	// Complete preflight uses this same bounded ledger/grammar. Discard its
	// temporary evidence, not the selected source; actual observations start
	// with fresh resident tables and the same externally visible generation.
	const JobMetricCounter generation = m_snapshot.generation;
	delete m_state; m_state = new (std::nothrow) State;
	m_snapshot = KernelPerformanceReferenceSnapshot();
	m_snapshot.generation = generation; m_snapshot.mode = options.mode;
	if (m_state == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	return prepareTrace(options);
}
bool KernelPerformanceReferenceLedger::prepareTrace(const KernelPerformanceReferenceRunOptions &options) noexcept
{
	const KernelPerformanceTraceOptions &requested = options.trace;
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	trace.requested = true;
	trace.mode = requested.mode;
	trace.binding = requested.binding;
	trace.limits = requested.limits;
	trace.residentAttemptCapacity = requested.residentAttemptCapacity;
	trace.residentRangeCapacity = requested.residentRangeCapacity;
	trace.sourceReceiptDigest = requested.sourceReceiptDigest;
	m_state->clock = options.clock != 0 ? options.clock : LiveSimulationPhaseClockNowNanoseconds;
	m_state->context = options.clockContext;
	m_state->traceOwner = this;
	m_state->traceAppend = requested.append;
	m_state->traceContext = requested.context;
	if (requested.mode == KERNEL_TRACE_CONSUME)
	{
		if (requested.sourceByteCount > requested.limits.maximumBytes) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		m_state->readAt = requested.readAt;
		m_state->sourceBytes = requested.sourceByteCount;
		m_state->sourceDigest = requested.sourceTraceDigest;
		m_state->readBuffer = new (std::nothrow) unsigned char[65536];
		if (m_state->readBuffer == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	}
	m_state->attemptCapacity = static_cast<unsigned>(requested.residentAttemptCapacity);
	m_state->attempts = new (std::nothrow) State::Attempt[m_state->attemptCapacity];
	if (m_state->attempts == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	m_state->rangeCapacity = static_cast<unsigned>(requested.residentRangeCapacity);
	m_state->ranges = new (std::nothrow) State::Range[m_state->rangeCapacity];
	if (m_state->ranges == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	// These reusable tables follow live bounds, never total event history.
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!writer.begin(0x5001, State::appendTrace, m_state, !m_state->consuming()) || !m_state->traceRecord(1) ||
		!writer.u32(3, 1) || !writer.u64(4, trace.residentAttemptCapacity) || !writer.u64(5, trace.residentRangeCapacity) ||
		!writer.u64(6, trace.limits.maximumBytes) || !writer.u64(7, trace.limits.maximumRecords) ||
		!writer.u64(8, trace.limits.maximumLogicalEvents) || !writer.u64(9, trace.limits.maximumAttempts) ||
		!writer.u64(10, trace.limits.maximumRanges) || !writer.sequence(11, 4) ||
		!appendDigest(writer, trace.binding.nativeRunIdentity) || !appendDigest(writer, trace.binding.executable) ||
		!appendDigest(writer, trace.binding.fixture) || !appendDigest(writer, trace.binding.sourcePolicy))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	return true;
}
KernelPerformanceAttempt KernelPerformanceReferenceLedger::beginAttempt(const KernelPerformanceAttemptIdentity &identity) noexcept
{
	KernelPerformanceAttempt token;
	if (!traceReady()) return token;
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	if (trace.observationSealed) { failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return token; }
	const unsigned maximumSubtype = identity.workKind == KERNEL_PERFORMANCE_AI || identity.workKind == KERNEL_PERFORMANCE_PATH ? 1 : 0;
	if (identity.workKind >= KERNEL_PERFORMANCE_KERNEL_COUNT || identity.subtype > maximumSubtype ||
		identity.sampleOrdinal == 0 || identity.phase < KERNEL_PHASE_OWNER_INTAKE || identity.phase >= KERNEL_PHASE_COUNT)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY); return token; }
	const State::Window &window = m_state->window;
	if (window.seen && (!window.open || !window.phaseOpen || identity.sampleOrdinal != window.sample ||
		static_cast<unsigned>(identity.phase) != window.nextPhase))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_ORDER); return token; }
	State::AttemptOrder &order = m_state->attemptOrder[identity.workKind][identity.subtype];
	if (order.seen && (identity.sampleOrdinal < order.sample ||
		(identity.sampleOrdinal == order.sample && identity.attemptOrdinal <= order.ordinal)))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_ORDER); return token; }
	if (trace.attemptCount >= trace.limits.maximumAttempts)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY); return token; }
	if (m_state->nextAttemptSerial == ~static_cast<JobMetricCounter>(0))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW); return token; }
	unsigned slot = 0;
	while (slot != m_state->attemptCapacity && m_state->attempts[slot].active) ++slot;
	if (slot == m_state->attemptCapacity)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY); return token; }
	const JobMetricCounter serial = m_state->nextAttemptSerial + 1;
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	const bool deferred = !m_state->prevalidating && identity.workKind == KERNEL_PERFORMANCE_COLLISION && identity.subtype == 0;
	if (m_state->budgetImportRange != ~0U || m_state->nativeFallback != 0 || m_state->restrictedReapSerial != 0 || !m_state->materializeCollision())
	{ failTrace(KERNEL_PERFORMANCE_ERROR_ORDER); return token; }
	if (deferred && (trace.recordCount >= trace.limits.maximumRecords || !m_state->logical(1)))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY); return token; }
	if (!deferred && (!m_state->traceRecord(3) || !writer.u64(3, serial) || !writer.u32(4, identity.workKind) ||
		!writer.u32(5, identity.subtype) || !writer.u64(6, identity.sampleOrdinal) ||
		!writer.u32(7, static_cast<unsigned>(identity.phase)) || !writer.u32(8, identity.ownerFrame) ||
		!writer.u64(9, identity.attemptOrdinal)))
	{ failTrace(KERNEL_REFERENCE_ERROR_HASH); return token; }
	State::Attempt &attempt = m_state->attempts[slot];
	attempt = State::Attempt();
	attempt.active = true;
	attempt.serial = serial;
	attempt.identity = identity;
	attempt.beginLogical = trace.logicalEventCount;
	if (deferred) { m_state->pendingCollision = slot; m_state->prefixCount = 0; }
	if (m_state->consuming()) attempt.sourceStart = trace.byteCount;
	m_state->nextAttemptSerial = serial;
	order.seen = true; order.sample = identity.sampleOrdinal; order.ordinal = identity.attemptOrdinal;
	++trace.attemptCount;
	++trace.residentAttemptCount;
	if (trace.residentAttemptCount > trace.residentAttemptHighWater) trace.residentAttemptHighWater = trace.residentAttemptCount;
	token.m_owner = this; token.m_generation = m_snapshot.generation; token.m_serial = serial; token.m_slot = slot;
	return token;
}
KernelPerformanceDeterministicBypass KernelPerformanceReferenceLedger::beginDeterministicBypass(
	KernelPerformanceAttempt token, const KernelPerformanceDeterministicBypassProof &proof) noexcept
{
	KernelPerformanceDeterministicBypass result;
	if (!traceReady()) return result;
	if (proof.m_owner != this || proof.m_input == 0 || proof.m_attempt.m_owner != token.m_owner ||
		proof.m_attempt.m_generation != token.m_generation || proof.m_attempt.m_serial != token.m_serial ||
		proof.m_attempt.m_slot != token.m_slot || m_state->nextBypass == ~JobMetricCounter(0))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY); return result; }
	if (!beginCollisionFallback(token, proof.m_class, *proof.m_input, true)) return result;
	result.m_owner = this; result.m_generation = m_snapshot.generation; result.m_serial = ++m_state->nextBypass;
	m_state->bypass = result; return result;
}
bool KernelPerformanceReferenceLedger::observeBypassFallbackContact(KernelPerformanceDeterministicBypass token,
	unsigned actualOwnerID, unsigned actualParticipantID, bool inserted) noexcept
{
	if (!traceReady()) return false;
	if (token.m_owner != this || token.m_generation != m_snapshot.generation || token.m_serial == 0 ||
		token.m_serial != m_state->bypass.m_serial || m_state->nativeFallback != 1)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	State::Attempt &a = m_state->attempts[m_state->nativeFallbackSlot];
	if (actualOwnerID != a.nativeOwnerID || actualParticipantID == 0 || actualParticipantID == actualOwnerID ||
		m_state->contacts >= a.capturedOperations || !checkedAdd(m_state->contacts, 1) ||
		(inserted && !checkedAdd(m_state->insertedContacts, 1))) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	CallbackGuard guard(m_state->busy);
	auto &w = m_state->semanticWriter;
	return (w.u32(160, 1) && w.u32(161, actualOwnerID) && w.u32(162, actualParticipantID) && w.boolean(163, inserted)) ||
		failTrace(KERNEL_REFERENCE_ERROR_HASH);
}
bool KernelPerformanceReferenceLedger::finishDeterministicBypass(KernelPerformanceDeterministicBypass token,
	JobMetricCounter attempted, JobMetricCounter inserted) noexcept
{
	if (!traceReady()) return false;
	if (token.m_owner != this || token.m_generation != m_snapshot.generation || token.m_serial == 0 ||
		token.m_serial != m_state->bypass.m_serial || m_state->nativeFallback != 1 ||
		attempted != m_state->contacts || inserted != m_state->insertedContacts)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	State::Attempt &a = m_state->attempts[m_state->nativeFallbackSlot];
	KernelPerformanceBatchIdentity timingIdentity;
	if (!a.nativeTiming->describeBatch(a.nativeBatch, timingIdentity)) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	CallbackGuard guard(m_state->busy);
	auto &w = m_state->semanticWriter; auto &t = m_snapshot.trace;
	if (!m_state->logical(1) || !w.u32(170, 2) || !w.u64(171, attempted) || !w.u64(172, inserted) || !w.boolean(173, true) ||
		!w.u32(180, 10) || !w.u64(181, t.logicalEventCount)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	if (!checkedAdd(t.notAdmittedAttemptCount, 1) || !checkedAdd(t.reapCount, 1) || !checkedAdd(t.coalescedAttemptCount, 1))
		return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	++m_state->span.attempts; m_state->span.lastLogical = t.logicalEventCount;
	m_state->span.lastAttempt = a.identity.attemptOrdinal;
	a.finished = true; a.active = false; --t.residentAttemptCount;
	m_state->nativeFallback = 0; m_state->nativeFallbackSlot = ~0U; m_state->bypass = KernelPerformanceDeterministicBypass();
	if (m_state->span.attempts == 1024 || w.bytesWritten() >= 1048576) return m_state->flushSpan();
	return true;
}
bool KernelPerformanceReferenceLedger::armNativeCollisionBegin(KernelPerformanceAttempt token,
	KernelPerformanceLedger &timing, const KernelPerformanceBatch &batch) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &a = m_state->attempts[slot];
	KernelPerformanceBatchIdentity identity;
	if (m_state->pendingCollision != slot || m_state->prefixCount != 0 || a.nativeArmed || a.nativeStage != 0 ||
		!timing.describeBatch(batch, identity) || identity.kernel != KERNEL_PERFORMANCE_COLLISION || identity.subtype != 0 ||
		identity.frame != a.identity.ownerFrame ||
		(timing.runRole() == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE) != m_state->consuming())
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	a.nativeTiming = &timing; a.nativeBatch = batch; a.nativeArmed = true;
	unsigned checkedSlot = 0;
	return nativeCollisionReady(token, 0, checkedSlot);
}
bool KernelPerformanceReferenceLedger::nativeCollisionReady(KernelPerformanceAttempt token, unsigned stage, unsigned &slot) noexcept
{
	if (!traceReady()) return false;
	slot = traceAttemptSlot(token); if (slot == ~0U) return false;
	const State::Attempt &a = m_state->attempts[slot]; KernelPerformanceBatchIdentity identity;
	if (!a.nativeArmed || a.nativeStage != stage || a.finished || a.captured || a.admitted || a.dispatchSeen || a.validated ||
		m_state->nativeFallback != 0 || a.nativeTiming == 0 || !a.nativeTiming->describeBatch(a.nativeBatch, identity) ||
		identity.kernel != KERNEL_PERFORMANCE_COLLISION || identity.subtype != 0 || identity.frame != a.identity.ownerFrame)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	const State::Window &window = m_state->window;
	if (window.seen && (!window.open || !window.phaseOpen || window.sample != a.identity.sampleOrdinal || window.nextPhase != a.identity.phase))
		return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (m_state->consuming() && (!a.nativeTiming->m_phase.phaseOpen ||
		a.nativeTiming->m_phase.frame.sampleOrdinal != a.identity.sampleOrdinal || a.nativeTiming->m_phase.nextPhase != a.identity.phase))
		return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	return true;
}
bool KernelPerformanceReferenceLedger::observeNativeCollisionFacts(KernelPerformanceAttempt token, unsigned site, const void *facts) noexcept
{
	unsigned slot = 0;
	if (site < 1 || site > 4 || !nativeCollisionReady(token, site - 1, slot)) return false;
	State::Attempt &a = m_state->attempts[slot];
	if (m_state->pendingCollision != ~0U && m_state->pendingCollision != slot)
	{
		CallbackGuard guard(m_state->busy);
		if (!m_state->materializeCollision()) return false;
	}
	if (site == 1) m_state->policyFacts = *static_cast<const NativeCollisionPolicyFacts *>(facts);
	if (site == 2) m_state->countFacts = *static_cast<const NativeCollisionCountFacts *>(facts);
	if (site == 3) m_state->reserveFacts = *static_cast<const NativeCollisionReserveFacts *>(facts);
	if (site == 4) m_state->captureFacts = *static_cast<const NativeCollisionCaptureFacts *>(facts);
	auto d = State::collisionDecision(site, facts, a.nativeWorkers);
	if (site == 1 && (!m_state->policyFacts.authoritativeRequested && !m_state->policyFacts.shadowRequested))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (site == 3 && (d.reason < 1 || d.reason > 3)) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (m_state->pendingCollision == slot)
	{
		CallbackGuard guard(m_state->busy);
		if (m_state->prefixCount != site - 1 || !m_state->logical(1)) return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
		++m_state->prefixCount; a.decisionSeen = true; a.decisionOrdinal = site - 1;
		m_state->trackNativeDecision(a, d);
		if (d.reason != 1 || !d.deterministicEligible) return m_state->materializeCollision();
		return true;
	}
	{
		CallbackGuard guard(m_state->busy);
		if (!m_state->flushSpan() || !m_state->semanticWriter.begin(0x5100 + site, 0, 0, true) ||
			!State::writeCollisionFacts(m_state->semanticWriter, site, facts)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		d.deterministicFacts = m_state->semanticWriter.finish();
	}
	return observeDecision(token, d);
}
bool KernelPerformanceReferenceLedger::observeNativeCollisionPolicy(KernelPerformanceAttempt token, const NativeCollisionPolicyFacts &facts) noexcept
{ return observeNativeCollisionFacts(token, 1, &facts); }
bool KernelPerformanceReferenceLedger::observeNativeCollisionCount(KernelPerformanceAttempt token, const NativeCollisionCountFacts &facts) noexcept
{ return observeNativeCollisionFacts(token, 2, &facts); }
bool KernelPerformanceReferenceLedger::observeNativeCollisionReserve(KernelPerformanceAttempt token, const NativeCollisionReserveFacts &facts) noexcept
{ return observeNativeCollisionFacts(token, 3, &facts); }
bool KernelPerformanceReferenceLedger::observeNativeCollisionCapture(KernelPerformanceAttempt token, const NativeCollisionCaptureFacts &facts) noexcept
{ return observeNativeCollisionFacts(token, 4, &facts); }
bool KernelPerformanceReferenceLedger::observeNativeCollisionFullClass(KernelPerformanceAttempt token,
	const NativeCollisionClassFacts &facts) noexcept
{
	unsigned slot = 0;
	if (!nativeCollisionReady(token, 4, slot)) return false;
	if (facts.minimumInputs != 256 || facts.occupantCount < 256 || !facts.spreadEvaluated || !facts.usefulSpread)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	auto d = State::collisionDecision(5, &facts, m_state->attempts[slot].nativeWorkers);
	{
		CallbackGuard guard(m_state->busy);
		if (!m_state->materializeCollision() || !m_state->flushSpan() || !m_state->semanticWriter.begin(0x5105, 0, 0, true) ||
			!State::writeCollisionFacts(m_state->semanticWriter, 5, &facts)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		d.deterministicFacts = m_state->semanticWriter.finish();
	}
	return observeDecision(token, d);
}
bool KernelPerformanceReferenceLedger::beginCollisionFallback(KernelPerformanceAttempt token, const NativeCollisionClassFacts &facts,
	const rts::PartitionCollisionReferenceInput &input, bool compact) noexcept
{
	unsigned slot = 0;
	if (!nativeCollisionReady(token, 4, slot)) return false;
	State::Attempt &a = m_state->attempts[slot];
	const unsigned classification = facts.occupantCount < 256 ? 1 : 2;
	if (facts.minimumInputs != 256 || facts.occupantCount != input.occupantCount || facts.usefulSpread ||
		facts.spreadEvaluated != (facts.occupantCount >= 256) || facts.samplerSampleCount > facts.samplerEncounterCount ||
		(input.cellCount != 0 && input.cells == 0) || (input.occupantCount != 0 && input.occupants == 0) ||
		input.cellCount != m_state->countFacts.cellCount || input.occupantCount != m_state->countFacts.occupantCount ||
		input.owner.objectID != m_state->captureFacts.capturedOwnerID || !m_state->captureFacts.snapshotValid || !m_state->captureFacts.ownerPresent)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (compact && (m_state->pendingCollision != slot || m_state->prefixCount != 4 ||
		m_state->reserveFacts.reserveOutcome != 1 || !m_state->captureFacts.participantIdentitiesValid ||
		!m_state->captureFacts.ownerIdentityUnchanged || input.owner.objectID == 0 ||
		input.owner.objectID != m_state->countFacts.admissionOwnerID ||
		m_state->reserveFacts.requiredCells != input.cellCount || m_state->reserveFacts.requiredOccupants != input.occupantCount ||
		m_state->reserveFacts.beforeCellCapacity != m_state->reserveFacts.afterCellCapacity ||
		m_state->reserveFacts.beforeOccupantCapacity != m_state->reserveFacts.afterOccupantCapacity ||
		m_state->reserveFacts.afterCellCapacity < input.cellCount || m_state->reserveFacts.afterOccupantCapacity < input.occupantCount))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (!compact)
	{
		auto d = State::collisionDecision(5, &facts, a.nativeWorkers); KernelPerformanceDigest inputDigest;
		{
			CallbackGuard guard(m_state->busy);
			if (!m_state->materializeCollision() || !m_state->flushSpan() || !m_state->semanticWriter.begin(0x5105, 0, 0, true) ||
				!State::writeCollisionFacts(m_state->semanticWriter, 5, &facts)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
			d.deterministicFacts = m_state->semanticWriter.finish();
			if (!m_state->semanticWriter.begin(1, 0, 0, true) || !rts::WritePartitionCollisionReferenceInput(m_state->semanticWriter, &input))
				return failTrace(KERNEL_REFERENCE_ERROR_HASH);
			inputDigest = m_state->semanticWriter.finish();
		}
		if (!observeDecision(token, d) || !bindCapturedDigest(token, 0x5100, input.occupantCount, inputDigest)) return false;
		CallbackGuard guard(m_state->busy);
		if (m_snapshot.trace.logicalEventCount == ~JobMetricCounter(0) || !m_state->semanticWriter.begin(0x5106, 0, 0, true) ||
			!m_state->semanticWriter.u32(150, 9) || !m_state->semanticWriter.u64(151, m_snapshot.trace.logicalEventCount + 1) ||
			!m_state->semanticWriter.boolean(152, true)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	}
	else
	{
		CallbackGuard guard(m_state->busy);
		State::Span &span = m_state->span; auto &w = m_state->semanticWriter; auto &t = m_snapshot.trace;
		if (span.active && (span.identity.sampleOrdinal != a.identity.sampleOrdinal || span.identity.ownerFrame != a.identity.ownerFrame ||
			span.identity.phase != a.identity.phase || span.classification != classification) && !m_state->flushSpan()) return false;
		if (!span.active)
		{
			if (!w.begin(0x5002, 0, 0, true) || !w.u32(100, 0x5002) || !w.u64(101, a.identity.sampleOrdinal) ||
				!w.u32(102, a.identity.ownerFrame) || !w.u32(103, a.identity.phase) || !w.u32(104, KERNEL_PERFORMANCE_COLLISION) ||
				!w.u32(105, 0) || !w.u32(106, classification)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
			span.active = true; span.identity = a.identity; span.classification = classification;
			span.firstLogical = a.beginLogical; span.firstAttempt = a.identity.attemptOrdinal;
		}
		if (!m_state->logical(3) || !checkedAdd(t.capturedAttemptCount, 1) || !checkedAdd(t.capturedOperationCount, input.occupantCount))
			return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
		if (!w.u32(110, 3) || !w.u64(111, a.beginLogical) || !w.u64(112, a.serial) ||
			!w.u64(113, a.identity.attemptOrdinal) || !w.u32(114, a.nativeWorkers)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		for (unsigned site = 1; site <= 5; ++site)
		{
			const void *actual = site == 5 ? static_cast<const void *>(&facts) : m_state->prefixFacts(site);
			if (!w.u32(120, 4) || !w.u64(121, a.beginLogical + site) || !w.u64(122, site - 1) ||
				!w.u32(123, site) || !w.u32(124, 0x5100 + site) || !State::writeCollisionFacts(w, site, actual))
				return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		}
		if (!w.u32(140, 5) || !w.u64(141, a.beginLogical + 6) || !w.u32(142, 0x5100) || !w.u32(143, 1) ||
			!w.u64(144, input.occupantCount) || !rts::WritePartitionCollisionReferenceInput(w, &input) ||
			!w.u32(150, 9) || !w.u64(151, a.beginLogical + 7) || !w.boolean(152, true)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		a.captured = true; a.inputSchema = 0x5100; a.capturedOperations = input.occupantCount;
		a.nativeStage = 5; a.nativeClass = classification; a.decisionOrdinal = 4;
		m_state->pendingCollision = ~0U; m_state->prefixCount = 0;
	}
	a.nativeOwnerID = input.owner.objectID;
	m_state->contacts = m_state->insertedContacts = 0;
	m_state->nativeFallback = compact ? 1 : 2; m_state->nativeFallbackSlot = slot;
	return true;
}
bool KernelPerformanceReferenceLedger::beginNativeCollisionFullFallback(KernelPerformanceAttempt token,
	const NativeCollisionClassFacts &facts, const rts::PartitionCollisionReferenceInput &input) noexcept
{ return beginCollisionFallback(token, facts, input, false); }
bool KernelPerformanceReferenceLedger::observeNativeCollisionFullFallbackContact(KernelPerformanceAttempt token,
	unsigned ownerID, unsigned participantID, bool inserted) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token); if (slot == ~0U) return false;
	const State::Attempt &a = m_state->attempts[slot];
	if (m_state->nativeFallback != 2 || m_state->nativeFallbackSlot != slot || ownerID != a.nativeOwnerID ||
		m_state->contacts >= a.capturedOperations || !checkedAdd(m_state->contacts, 1) ||
		(inserted && !checkedAdd(m_state->insertedContacts, 1))) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	CallbackGuard guard(m_state->busy); auto &w = m_state->semanticWriter;
	return (w.u32(160, 1) && w.u32(161, ownerID) && w.u32(162, participantID) && w.boolean(163, inserted)) || failTrace(KERNEL_REFERENCE_ERROR_HASH);
}
bool KernelPerformanceReferenceLedger::finishNativeCollisionFullFallback(KernelPerformanceAttempt token,
	JobMetricCounter attempted, JobMetricCounter inserted) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token); if (slot == ~0U) return false;
	State::Attempt &a = m_state->attempts[slot]; KernelPerformanceBatchIdentity identity;
	if (m_state->nativeFallback != 2 || m_state->nativeFallbackSlot != slot || attempted != m_state->contacts ||
		inserted != m_state->insertedContacts || !a.nativeTiming->describeBatch(a.nativeBatch, identity))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	{
		CallbackGuard guard(m_state->busy); auto &w = m_state->semanticWriter;
		if (!w.u32(170, 2) || !w.u64(171, attempted) || !w.u64(172, inserted) || !w.boolean(173, true))
			return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		m_state->fullContactDigest = w.finish();
	}
	m_state->fullContactReady = true; m_state->nativeFallback = 0; m_state->nativeFallbackSlot = ~0U;
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_NOT_ADMITTED; finish.reasonSchema = 0x5002; finish.reason = a.nativeClass;
	finish.fallbackEntered = finish.fallbackCompleted = true;
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = reap.reason = 1;
	return finishAttempt(token, finish) && reapAttempt(token, reap);
}
bool KernelPerformanceReferenceLedger::observeDecision(KernelPerformanceAttempt token,
	const KernelPerformanceAttemptDecision &decision) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (attempt.finished || attempt.inputSchema == 0x5100 || (decision.admission == KERNEL_ADMISSION_ACCEPTED &&
		(!attempt.captured || m_snapshot.trace.observationSealed)))
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (decision.site == 0 || decision.reasonSchema == 0 || !decision.deterministicFacts.valid ||
		decision.admission < KERNEL_ADMISSION_NOT_REQUESTED || decision.admission > KERNEL_ADMISSION_ACCEPTED ||
		(decision.admission != KERNEL_ADMISSION_NOT_REQUESTED && !decision.deterministicEligible) ||
		!traceDynamicFactsValid(decision.dynamicFactsKnownMask, decision.pendingJobs, decision.outstandingJobs, decision.activeSlots))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (attempt.decisionSeen && decision.decisionOrdinal <= attempt.decisionOrdinal)
		return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	JobMetricCounter admittedCount = m_snapshot.trace.admittedAttemptCount;
	if (!attempt.admitted && decision.admission == KERNEL_ADMISSION_ACCEPTED && !checkedAdd(admittedCount, 1))
		return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	if (m_state->budgetImportRange != ~0U || m_state->nativeFallback != 0 || m_state->restrictedReapSerial != 0 || !m_state->materializeCollision() ||
		!m_state->flushSpan() || !m_state->decisionRecord(attempt.serial, decision, false))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	m_state->trackNativeDecision(attempt, decision);
	attempt.decisionSeen = true;
	attempt.decisionOrdinal = decision.decisionOrdinal;
	// Later source refusal/failure cannot erase an earlier actual acceptance.
	if (decision.admission == KERNEL_ADMISSION_ACCEPTED) attempt.admitted = true;
	m_snapshot.trace.admittedAttemptCount = admittedCount;
	return true;
}
bool KernelPerformanceReferenceLedger::bindCapturedInput(KernelPerformanceAttempt token, unsigned fieldSchema,
	JobMetricCounter operationCount, KernelPerformanceCanonicalCallback writeInput, const void *immutableInput) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	const State::Attempt &attempt = m_state->attempts[slot];
	if (attempt.finished || attempt.captured) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (fieldSchema == 0 || fieldSchema == 0x5100 || operationCount == 0 || writeInput == 0 || immutableInput == 0)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter capturedCount = m_snapshot.trace.capturedAttemptCount;
	JobMetricCounter capturedOperations = m_snapshot.trace.capturedOperationCount;
	if (!checkedAdd(capturedCount, 1) || !checkedAdd(capturedOperations, operationCount))
		return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	KernelPerformanceDigest input;
	try
	{
		CallbackGuard guard(m_state->busy);
		if (!m_state->materializeCollision() || !m_state->flushSpan()) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		KernelPerformanceCanonicalWriter inputWriter;
		if (!inputWriter.begin(fieldSchema)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		if (!writeInput(inputWriter, immutableInput)) return failTrace(KERNEL_REFERENCE_ERROR_CALLBACK);
		input = inputWriter.finish();
		if (!input.valid) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		if (m_foreignCall.load(std::memory_order_acquire)) return failTrace(KERNEL_PERFORMANCE_ERROR_OWNER);
		if (m_snapshot.errors != 0) return failTrace(m_snapshot.errors);
	}
	catch (...) { return failTrace(KERNEL_REFERENCE_ERROR_CALLBACK); }
	return bindCapturedDigest(token, fieldSchema, operationCount, input);
}
bool KernelPerformanceReferenceLedger::bindCapturedDigest(KernelPerformanceAttempt token, unsigned fieldSchema,
	JobMetricCounter operationCount, const KernelPerformanceDigest &input) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (attempt.finished || attempt.captured) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	const bool nativeCapture = fieldSchema == 0x5100;
	if (fieldSchema == 0 || (!nativeCapture && operationCount == 0) || !input.valid)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (nativeCapture && (attempt.identity.workKind != KERNEL_PERFORMANCE_COLLISION || attempt.identity.subtype != 0 ||
		attempt.nativeStage != 5 || attempt.nativeClass < 1 || attempt.nativeClass > 2 || attempt.admitted ||
		attempt.dispatchSeen || attempt.validated || (attempt.nativeClass == 1 ? operationCount >= 256 : operationCount < 256)))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter capturedCount = m_snapshot.trace.capturedAttemptCount;
	JobMetricCounter capturedOperations = m_snapshot.trace.capturedOperationCount;
	if (!checkedAdd(capturedCount, 1) || !checkedAdd(capturedOperations, operationCount))
		return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(5) || !writer.u64(3, attempt.serial) || !writer.u32(4, fieldSchema) ||
		!writer.u64(5, operationCount) || !appendDigest(writer, input, 6)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	// Retain the canonical digest and identity only, never the native pointer.
	attempt.inputDigest = input;
	attempt.captured = true;
	attempt.inputSchema = fieldSchema;
	attempt.capturedOperations = operationCount;
	m_snapshot.trace.capturedAttemptCount = capturedCount;
	m_snapshot.trace.capturedOperationCount = capturedOperations;
	return true;
}
bool KernelPerformanceReferenceLedger::observeDispatch(KernelPerformanceAttempt token,
	const KernelPerformanceDispatchPlan &dispatch) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	if (trace.observationSealed || attempt.finished || !attempt.captured || attempt.inputSchema == 0x5100 ||
		(attempt.dispatchSeen && (attempt.plannedRanges != attempt.dispatch.rangeCount ||
			attempt.releasedRanges != attempt.dispatch.rangeCount)))
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (dispatch.bodySchema == 0 || dispatch.checkpointSchema == 0 || dispatch.rangeCount == 0 || dispatch.operationCount == 0)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (attempt.dispatchSeen && dispatch.dispatchOrdinal <= attempt.dispatch.dispatchOrdinal)
		return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (trace.residentRangeCount > trace.residentRangeCapacity ||
		dispatch.rangeCount > trace.residentRangeCapacity - trace.residentRangeCount ||
		trace.rangeCount > trace.limits.maximumRanges || dispatch.rangeCount > trace.limits.maximumRanges - trace.rangeCount)
		return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	JobMetricCounter dispatchCount = trace.dispatchCount;
	if (!checkedAdd(dispatchCount, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	const bool cachedDispatch = m_state->consuming() && attempt.dispatchOffset == trace.byteCount;
	if (m_state->consuming()) attempt.dispatchOffset = trace.byteCount;
	if (!m_state->traceRecord(6) || !writer.u64(3, attempt.serial) || !writer.u64(4, dispatch.dispatchOrdinal) ||
		!writer.u32(5, dispatch.bodySchema) || !writer.u32(6, dispatch.checkpointSchema) || !writer.u32(7, dispatch.rangeCount) ||
		!writer.u64(8, dispatch.operationCount) || !writer.u64(9, dispatch.sourceGrain) || !writer.u64(10, dispatch.sourceLimit))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	attempt.dispatchSeen = true;
	attempt.dispatch = dispatch;
	if (m_state->consuming() && (!cachedDispatch || attempt.planScan < trace.byteCount)) attempt.planScan = trace.byteCount;
	attempt.plannedRanges = attempt.releasedRanges = 0;
	trace.dispatchCount = dispatchCount;
	return true;
}
bool KernelPerformanceReferenceLedger::observeRangePlan(KernelPerformanceAttempt token,
	const KernelPerformanceRangePlan &range) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (attempt.finished || !attempt.dispatchSeen || attempt.plannedRanges == attempt.dispatch.rangeCount)
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (range.dispatchOrdinal != attempt.dispatch.dispatchOrdinal || range.end < range.begin)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (requestBudgetProfile(attempt.identity, attempt.dispatch) &&
		(range.operationCount != range.end - range.begin || range.end > attempt.capturedOperations))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	// Plans enumerate declared range ordinals once. Released progress may be
	// imported in any owner-observed order, without retaining past dispatches.
	if (range.rangeOrdinal != attempt.plannedRanges) return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	if (trace.rangeCount >= trace.limits.maximumRanges) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	unsigned rangeSlot = 0;
	while (rangeSlot != m_state->rangeCapacity && (!m_state->ranges[rangeSlot].cached ||
		m_state->ranges[rangeSlot].attemptSerial != attempt.serial || !sameRangePlan(m_state->ranges[rangeSlot].plan, range))) ++rangeSlot;
	if (rangeSlot == m_state->rangeCapacity)
	{
		rangeSlot = 0;
		while (rangeSlot != m_state->rangeCapacity && (m_state->ranges[rangeSlot].active || m_state->ranges[rangeSlot].cached)) ++rangeSlot;
	}
	if (rangeSlot == m_state->rangeCapacity) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	CallbackGuard guard(m_state->busy);
	if (!m_state->traceRecord(7) || !appendRangePlan(m_state->traceWriter, attempt.serial, range))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	State::Range &retained = m_state->ranges[rangeSlot];
	retained = State::Range();
	retained.active = true;
	retained.attemptSerial = attempt.serial;
	retained.plan = range;
	retained.budgeted = requestBudgetProfile(attempt.identity, attempt.dispatch);
	retained.budgetImportRequest = range.begin;
	if (m_state->consuming() && attempt.planScan < trace.byteCount) attempt.planScan = trace.byteCount;
	++attempt.plannedRanges;
	++trace.rangeCount;
	++trace.residentRangeCount;
	if (trace.residentRangeCount > trace.residentRangeHighWater) trace.residentRangeHighWater = trace.residentRangeCount;
	return true;
}
bool KernelPerformanceReferenceLedger::observeReleasedRange(KernelPerformanceAttempt token,
	const KernelPerformanceRangePlan &range, const KernelPerformanceRangeProgress &progress) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (!attempt.dispatchSeen || attempt.plannedRanges != attempt.dispatch.rangeCount ||
		(progress.checkpoint.entered && !attempt.admitted))
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (range.dispatchOrdinal != attempt.dispatch.dispatchOrdinal)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	unsigned rangeSlot = 0;
	while (rangeSlot != m_state->rangeCapacity && (!m_state->ranges[rangeSlot].active ||
		m_state->ranges[rangeSlot].attemptSerial != attempt.serial || !sameRangePlan(m_state->ranges[rangeSlot].plan, range))) ++rangeSlot;
	if (rangeSlot == m_state->rangeCapacity) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (!releasedRangeProgressValid(progress)) return failTrace(KERNEL_REFERENCE_ERROR_CHECKPOINT);
	const State::Range &retained = m_state->ranges[rangeSlot];
	if ((m_state->budgetImportRange != ~0U && m_state->budgetImportRange != rangeSlot) ||
		(retained.budgeted && (retained.budgetImportRequest != range.end ||
			(!progress.checkpoint.entered && retained.budgetReached))))
		return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (m_state->consuming() && !m_state->prevalidating &&
		!m_state->ranges[rangeSlot].bodyFinished) return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (m_state->consuming() && !m_state->prevalidating &&
		(!sameCheckpointProgress(progress.checkpoint, m_state->ranges[rangeSlot].actual.checkpoint) ||
			progress.publication != m_state->ranges[rangeSlot].actual.publication)) return failTrace(KERNEL_REFERENCE_ERROR_CHECKPOINT);
	JobMetricCounter releasedCount = m_snapshot.trace.releasedRangeCount;
	if (!checkedAdd(releasedCount, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	const KernelPerformanceCheckpointProgress &checkpoint = progress.checkpoint;
	if (!m_state->traceRecord(8) || !appendRangePlan(writer, attempt.serial, range) ||
		!writer.boolean(10, checkpoint.entered) || !writer.u32(11, checkpoint.errors) ||
		!writer.u64(12, checkpoint.pollCount) || !writer.u64(13, checkpoint.firstTruePoll) || !writer.u64(14, checkpoint.completedWorkUnits) ||
		!writer.u32(15, checkpoint.firstTrueCheckpoint.site) || !writer.u64(16, checkpoint.firstTrueCheckpoint.first) ||
		!writer.u64(17, checkpoint.firstTrueCheckpoint.second) || !writer.u32(18, checkpoint.finalCheckpoint.site) ||
		!writer.u64(19, checkpoint.finalCheckpoint.first) || !writer.u64(20, checkpoint.finalCheckpoint.second) ||
		!writer.u32(21, static_cast<unsigned>(checkpoint.terminal)) || !writer.u32(22, static_cast<unsigned>(progress.publication)))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	// The import acknowledges immutable released POD; it never releases native
	// work. Only this reusable metadata is freed, not the containing attempt.
	m_state->ranges[rangeSlot].active = false;
	m_state->ranges[rangeSlot].cached = false;
	m_state->budgetImportRange = ~0U;
	if (checkpoint.terminal != KERNEL_RANGE_COMPLETED || progress.publication != KERNEL_PUBLICATION_PUBLISHED)
		attempt.successfulRanges = false;
	++attempt.releasedRanges;
	m_snapshot.trace.releasedRangeCount = releasedCount;
	--m_snapshot.trace.residentRangeCount;
	return true;
}
bool KernelPerformanceReferenceLedger::replayDecision(KernelPerformanceAttempt token, unsigned site, bool eligible,
	const KernelPerformanceDigest &facts, KernelPerformanceAttemptDecision &decision) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	if (!m_state->consuming() || m_state->prevalidating) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	{
		CallbackGuard guard(m_state->busy);
		if (!m_state->materializeCollision() || !m_state->flushSpan()) return false;
	}
	State::Event event;
	if (!m_state->eventAt(m_snapshot.trace.byteCount, event)) return false;
	if (event.v[1] != 4 || event.v[3] != m_state->attempts[slot].serial || event.v[5] != site ||
		(event.v[8] != 0) != eligible || !event.digest.equals(facts)) return failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
	const KernelPerformanceAttemptDecision expected = event.decision();
	if (!observeDecision(token, expected)) return false;
	decision = expected; return true;
}
bool KernelPerformanceReferenceLedger::readSourceDispatch(KernelPerformanceAttempt token, JobMetricCounter ordinal,
	KernelPerformanceDispatchPlan &plan) const noexcept
{
	if (mode() != KERNEL_REFERENCE_PHASE_BASELINE_BINDING || m_state->busy || token.m_owner != this ||
		token.m_generation != m_snapshot.generation || token.m_slot >= m_state->attemptCapacity) return false;
	State::Attempt &attempt = m_state->attempts[token.m_slot];
	if (!attempt.active || attempt.serial != token.m_serial) return false;
	if (attempt.dispatchSeen && attempt.dispatch.dispatchOrdinal == ordinal) { plan = attempt.dispatch; return true; }
	State::Event event;
	if (attempt.dispatchOffset != 0)
	{
		if (!m_state->eventAt(attempt.dispatchOffset, event)) return false;
		if (event.v[1] == 6 && event.v[3] == attempt.serial && event.v[4] == ordinal)
		{ plan = event.dispatch(); return true; }
	}
	for (JobMetricCounter at = attempt.dispatchOffset != 0 ? attempt.dispatchOffset : m_snapshot.trace.byteCount;
		at < m_state->sourceBytes;)
	{
		if (!m_state->eventAt(at, event)) return false;
		if (event.v[1] == 6 && event.v[3] == attempt.serial && event.v[4] == ordinal)
		{ attempt.dispatchOffset = at; attempt.planScan = event.end; plan = event.dispatch(); return true; }
		if (event.v[1] == 9 && event.v[3] == attempt.serial) break;
		at = event.end;
	}
	return false;
}
bool KernelPerformanceReferenceLedger::readSourceRange(KernelPerformanceAttempt token, JobMetricCounter ordinal,
	unsigned rangeOrdinal, KernelPerformanceRangePlan &plan) const noexcept
{
	KernelPerformanceDispatchPlan dispatch;
	if (!readSourceDispatch(token, ordinal, dispatch) || rangeOrdinal >= dispatch.rangeCount) return false;
	State::Attempt &attempt = m_state->attempts[token.m_slot];
	for (unsigned i = 0; i != m_state->rangeCapacity; ++i)
	{
		const State::Range &range = m_state->ranges[i];
		if ((range.active || range.cached) && range.attemptSerial == attempt.serial &&
			range.plan.dispatchOrdinal == ordinal && range.plan.rangeOrdinal == rangeOrdinal) { plan = range.plan; return true; }
	}
	State::Event event;
	for (JobMetricCounter at = attempt.planScan; at < m_state->sourceBytes;)
	{
		if (!m_state->eventAt(at, event)) return false;
		attempt.planScan = event.end;
		if (event.v[1] == 7 && event.v[3] == attempt.serial && event.v[4] == ordinal)
		{
			unsigned i = 0;
			const KernelPerformanceRangePlan found = event.range();
			while (i != m_state->rangeCapacity && (!(m_state->ranges[i].active || m_state->ranges[i].cached) ||
				m_state->ranges[i].attemptSerial != attempt.serial || !sameRangePlan(m_state->ranges[i].plan, found))) ++i;
			if (i != m_state->rangeCapacity)
			{
				if (found.rangeOrdinal == rangeOrdinal) { plan = found; return true; }
				at = event.end; continue;
			}
			i = 0;
			while (i != m_state->rangeCapacity && (m_state->ranges[i].active || m_state->ranges[i].cached)) ++i;
			if (i == m_state->rangeCapacity) return m_state->traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
			State::Range &range = m_state->ranges[i]; range = State::Range();
			range.cached = true; range.attemptSerial = attempt.serial; range.plan = event.range();
			if (range.plan.rangeOrdinal == rangeOrdinal) { plan = range.plan; return true; }
		}
		if (event.v[1] == 9 && event.v[3] == attempt.serial) break;
		at = event.end;
	}
	return false;
}
bool KernelPerformanceReferenceLedger::readSourceFinish(KernelPerformanceAttempt token,
	KernelPerformanceAttemptFinish &finish) const noexcept
{
	if (mode() != KERNEL_REFERENCE_PHASE_BASELINE_BINDING || m_state->busy || token.m_owner != this ||
		token.m_generation != m_snapshot.generation || token.m_slot >= m_state->attemptCapacity) return false;
	State::Attempt &attempt = m_state->attempts[token.m_slot];
	if (!attempt.active || attempt.serial != token.m_serial) return false;
	State::Event event;
	if (!m_state->sourceFinish(attempt, event)) return false;
	KernelPerformanceAttemptFinish result = {};
	result.disposition = static_cast<KernelPerformanceDisposition>(event.v[4]);
	result.reasonSchema = static_cast<unsigned>(event.v[5]); result.reason = static_cast<unsigned>(event.v[6]);
	result.fallbackEntered = event.v[7] != 0; result.fallbackCompleted = event.v[8] != 0;
	result.validationObserved = event.v[9] != 0;
	finish = result; return true;
}
KernelPerformanceInlineAction KernelPerformanceReferenceLedger::beginInlineBody(KernelPerformanceAttempt token,
	const KernelPerformanceRangePlan &plan, KernelPerformanceLedger &timing, KernelPerformanceInlineBody &body,
	KernelPerformanceCheckpointProbe &probe) noexcept
{
	body = KernelPerformanceInlineBody();
	if (!traceReady()) return KERNEL_INLINE_INVALID;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return KERNEL_INLINE_INVALID;
	if (!m_state->consuming() || m_state->prevalidating || m_state->body.valid() || m_state->budgetImportRange != ~0U)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return KERNEL_INLINE_INVALID; }
	State::Attempt &attempt = m_state->attempts[slot];
	unsigned i = 0;
	while (i != m_state->rangeCapacity && (!m_state->ranges[i].active || m_state->ranges[i].attemptSerial != attempt.serial ||
		!sameRangePlan(m_state->ranges[i].plan, plan))) ++i;
	if (i == m_state->rangeCapacity || m_state->ranges[i].bodyStarted ||
		attempt.plannedRanges != attempt.dispatch.rangeCount)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY); return KERNEL_INLINE_INVALID; }
	State::Event event, finish;
	if (!m_state->sourceFinish(attempt, finish)) return KERNEL_INLINE_INVALID;
	State::Range &range = m_state->ranges[i];
	JobMetricCounter at = m_snapshot.trace.byteCount;
	bool reached = false;
	if (range.budgeted)
	{
		range.budgetBegin = range.budgetGrantOffset = at;
		range.budgetGrantRequest = plan.begin;
		for (JobMetricCounter request = plan.begin; request != plan.end; ++request)
		{
			if (!m_state->requestBudgetAt(range, at, request, event)) return KERNEL_INLINE_INVALID;
			reached = reached || event.v[8] != KERNEL_REQUEST_BUDGET_NOT_REACHED;
			at = event.end;
		}
		range.budgetEnd = at;
	}
	if (!m_state->eventAt(at, event)) return KERNEL_INLINE_INVALID;
	if (event.v[1] != 8 || event.v[3] != attempt.serial || !sameRangePlan(event.range(), plan))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_ORDER); return KERNEL_INLINE_INVALID; }
	range.actual = event.progress();
	if (!range.actual.checkpoint.entered)
	{
		if (reached) { failTrace(KERNEL_REFERENCE_ERROR_MISMATCH); return KERNEL_INLINE_INVALID; }
		range.budgetGrantOffset = range.budgetEnd; range.budgetGrantRequest = plan.end;
		range.bodyStarted = range.bodyFinished = true;
		return KERNEL_INLINE_SKIP_SOURCE_NEVER_ENTERED;
	}
	if (probe.snapshot().entered || probe.snapshot().errors != 0)
	{ failTrace(KERNEL_REFERENCE_ERROR_CHECKPOINT); return KERNEL_INLINE_INVALID; }
	if (m_state->nextBody == ~static_cast<JobMetricCounter>(0))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW); return KERNEL_INLINE_INVALID; }
	if (!probe.beginReplay(range.actual.checkpoint)) { failTrace(KERNEL_REFERENCE_ERROR_CHECKPOINT); return KERNEL_INLINE_INVALID; }
	const bool pure = finish.v[4] == KERNEL_PERFORMANCE_COMMITTED && range.actual.publication == KERNEL_PUBLICATION_PUBLISHED;
	JobMetricCounter timingSample = attempt.identity.sampleOrdinal;
	KernelPerformancePhase timingPhase = attempt.identity.phase;
	const State::Window &window = m_state->window;
	if (!pure && finish.v[4] == KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION && window.open && window.phaseOpen)
	{
		// Late discarded work is serial in its source-observed owner window;
		// the captured attempt identity and pure entitlement remain unchanged.
		timingSample = window.sample;
		timingPhase = static_cast<KernelPerformancePhase>(window.nextPhase);
	}
	const auto timed = timing.beginAuthenticatedInline(timingSample, timingPhase, pure);
	if (!timed.valid()) { failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return KERNEL_INLINE_INVALID; }
	body.m_owner = this; body.m_generation = m_snapshot.generation; body.m_serial = ++m_state->nextBody;
	m_state->body = body; m_state->bodyRange = i; m_state->bodyTiming = &timing;
	m_state->bodyProbe = &probe; m_state->timedBody = timed; range.bodyStarted = true;
	return KERNEL_INLINE_EXECUTE;
}
bool KernelPerformanceReferenceLedger::finishInlineBody(KernelPerformanceInlineBody body,
	const KernelPerformanceRangeProgress &progress) noexcept
{
	if (!traceReady()) return false;
	if (!body.valid() || body.m_owner != this || body.m_generation != m_snapshot.generation ||
		!m_state->body.valid() || body.m_serial != m_state->body.m_serial || m_state->ownerSerial.valid())
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	State::Range &range = m_state->ranges[m_state->bodyRange];
	if (!releasedRangeProgressValid(progress) || !sameCheckpointProgress(progress.checkpoint, range.actual.checkpoint) ||
		progress.publication != range.actual.publication || m_state->bodyProbe == 0 ||
		!sameCheckpointProgress(progress.checkpoint, m_state->bodyProbe->snapshot()))
		return failTrace(KERNEL_REFERENCE_ERROR_CHECKPOINT);
	if (!m_state->bodyTiming->endAuthenticatedInline(m_state->timedBody)) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	// Remaining NOT_REACHED checks are owner bookkeeping, outside pure body time.
	if (range.budgeted && !m_state->finishRequestBudgetTraversal(range)) return false;
	range.bodyFinished = true;
	m_state->body = KernelPerformanceInlineBody(); m_state->bodyTiming = 0; m_state->bodyProbe = 0;
	return true;
}
KernelPerformanceInlineOwnerSerial KernelPerformanceReferenceLedger::beginInlineOwnerSerial(
	KernelPerformanceInlineBody body, KernelPerformanceInlineOwnerSerialKind kind) noexcept
{
	KernelPerformanceInlineOwnerSerial token;
	if (!traceReady()) return token;
	if (!body.valid() || body.m_owner != this || body.m_generation != m_snapshot.generation ||
		!m_state->body.valid() || body.m_serial != m_state->body.m_serial || m_state->ownerSerial.valid() ||
		kind != KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY); return token; }
	const JobMetricCounter serial = m_state->ranges[m_state->bodyRange].attemptSerial;
	unsigned i = 0;
	while (i != m_state->attemptCapacity && (!m_state->attempts[i].active || m_state->attempts[i].serial != serial)) ++i;
	if (i == m_state->attemptCapacity || m_state->attempts[i].identity.workKind != KERNEL_PERFORMANCE_PATH ||
		m_state->attempts[i].identity.subtype != 0)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY); return token; }
	if (m_state->nextOwnerSerial == ~static_cast<JobMetricCounter>(0))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW); return token; }
	const auto timed = m_state->bodyTiming->beginAuthenticatedOwnerSerial(m_state->timedBody);
	if (!timed.valid()) { failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return token; }
	token.m_owner = this; token.m_generation = m_snapshot.generation; token.m_serial = ++m_state->nextOwnerSerial;
	m_state->ownerSerial = token; m_state->timedOwnerSerial = timed; m_state->ownerBudgetUsed = false; return token;
}
bool KernelPerformanceReferenceLedger::endInlineOwnerSerial(KernelPerformanceInlineOwnerSerial token) noexcept
{
	if (!traceReady()) return false;
	if (!token.valid() || token.m_owner != this || token.m_generation != m_snapshot.generation ||
		!m_state->ownerSerial.valid() || token.m_serial != m_state->ownerSerial.m_serial || !m_state->body.valid())
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (m_state->budgetPending) return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (!m_state->bodyTiming->endAuthenticatedOwnerSerial(m_state->timedOwnerSerial)) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	m_state->ownerSerial = KernelPerformanceInlineOwnerSerial(); return true;
}
bool KernelPerformanceReferenceLedger::observeReleasedRequestBudget(KernelPerformanceAttempt token,
	const KernelPerformanceRangePlan &plan, const KernelPerformanceRequestBudget &actual) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	const State::Attempt &attempt = m_state->attempts[slot];
	if (!attempt.dispatchSeen || attempt.plannedRanges != attempt.dispatch.rangeCount || m_state->body.valid() ||
		!requestBudgetProfile(attempt.identity, attempt.dispatch)) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	unsigned i = 0;
	while (i != m_state->rangeCapacity && (!m_state->ranges[i].active || m_state->ranges[i].attemptSerial != attempt.serial ||
		!sameRangePlan(m_state->ranges[i].plan, plan))) ++i;
	if (i == m_state->rangeCapacity) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	State::Range &range = m_state->ranges[i];
	if ((m_state->budgetImportRange != ~0U && m_state->budgetImportRange != i) ||
		(m_state->consuming() && !m_state->prevalidating && !range.bodyFinished))
		return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (range.budgetImportRequest == plan.end || actual.requestOrdinal != range.budgetImportRequest ||
		!requestBudgetValid(actual) || (!attempt.admitted && actual.disposition != KERNEL_REQUEST_BUDGET_NOT_REACHED))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(13) || !writer.u64(3, attempt.serial) || !writer.u64(4, plan.dispatchOrdinal) ||
		!writer.u32(5, plan.rangeOrdinal) || !writer.u64(6, actual.requestOrdinal) || !writer.u32(7, 1) ||
		!writer.u32(8, static_cast<unsigned>(actual.disposition)) || !writer.u32(9, actual.grantSite) ||
		!writer.u64(10, actual.localGrantOrdinal) || !writer.u64(11, actual.requestedBytes) || !writer.u64(12, actual.grantedBytes) ||
		!writer.u32(13, actual.refundSite) || !writer.u64(14, actual.localRefundOrdinal) ||
		!writer.u64(15, actual.refundedBytes) || !writer.u64(16, actual.consumedBytes)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	++range.budgetImportRequest;
	range.budgetReached = range.budgetReached || actual.disposition != KERNEL_REQUEST_BUDGET_NOT_REACHED;
	m_state->budgetImportRange = i;
	return true;
}
bool KernelPerformanceReferenceLedger::replayRequestBudgetGrant(KernelPerformanceInlineOwnerSerial extent,
	JobMetricCounter requestOrdinal, unsigned grantSite, JobMetricCounter localGrantOrdinal,
	JobMetricCounter actualRequestedBytes, bool &sourceGranted) noexcept
{
	if (!traceReady()) return false;
	if (!m_state->consuming() || m_state->prevalidating || !extent.valid() || extent.m_owner != this ||
		extent.m_generation != m_snapshot.generation || !m_state->ownerSerial.valid() ||
		extent.m_serial != m_state->ownerSerial.m_serial || !m_state->body.valid() ||
		m_state->budgetPending || m_state->ownerBudgetUsed) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	State::Range &range = m_state->ranges[m_state->bodyRange];
	if (!range.budgeted || requestOrdinal < range.budgetGrantRequest || requestOrdinal >= range.plan.end)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	while (range.budgetGrantOffset < range.budgetEnd)
	{
		State::Event event;
		if (!m_state->requestBudgetAt(range, range.budgetGrantOffset, range.budgetGrantRequest, event)) return false;
		const KernelPerformanceRequestBudget expected = event.budget();
		if (expected.requestOrdinal < requestOrdinal)
		{
			if (expected.disposition != KERNEL_REQUEST_BUDGET_NOT_REACHED) return failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
			range.budgetGrantOffset = event.end; ++range.budgetGrantRequest;
			continue;
		}
		if (expected.disposition == KERNEL_REQUEST_BUDGET_NOT_REACHED || expected.grantSite != grantSite ||
			expected.localGrantOrdinal != localGrantOrdinal || expected.requestedBytes != actualRequestedBytes)
			return failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
		m_state->pendingBudget = expected; m_state->budgetPending = m_state->ownerBudgetUsed = true;
		range.budgetGrantOffset = event.end; ++range.budgetGrantRequest;
		sourceGranted = expected.disposition != KERNEL_REQUEST_BUDGET_REFUSED;
		return true;
	}
	return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
}
bool KernelPerformanceReferenceLedger::finishInlineRequestBudget(KernelPerformanceInlineOwnerSerial extent,
	const KernelPerformanceRequestBudget &actual) noexcept
{
	if (!traceReady()) return false;
	if (!m_state->consuming() || m_state->prevalidating || !extent.valid() || extent.m_owner != this ||
		extent.m_generation != m_snapshot.generation || !m_state->ownerSerial.valid() ||
		extent.m_serial != m_state->ownerSerial.m_serial || !m_state->body.valid() ||
		!m_state->budgetPending || !m_state->ownerBudgetUsed) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (!requestBudgetValid(actual) || !sameRequestBudget(actual, m_state->pendingBudget))
		return failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
	m_state->budgetPending = false;
	return true;
}
KernelPerformanceReferenceBatch KernelPerformanceReferenceLedger::observeValidatedAttempt(KernelPerformanceAttempt token,
	KernelPerformanceCanonicalCallback outputCallback, const void *output, KernelPerformanceSerialCallback detached,
	const void *input, void *storage) noexcept
{
	KernelPerformanceReferenceBatch invalid;
	if (!traceReady()) return invalid;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return invalid;
	const State::Attempt &attempt = m_state->attempts[slot];
	if (detached != 0 || input != 0 || storage != 0 || outputCallback == 0 || output == 0 ||
		attempt.validated || attempt.finished || !attempt.captured || !attempt.admitted || !attempt.dispatchSeen ||
		attempt.plannedRanges != attempt.dispatch.rangeCount || attempt.releasedRanges != attempt.dispatch.rangeCount ||
		!attempt.successfulRanges)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return invalid; }
	KernelPerformanceDigest digest;
	try
	{
		CallbackGuard guard(m_state->busy);
		KernelPerformanceCanonicalWriter writer;
		if (!writer.begin(attempt.inputSchema)) { failTrace(KERNEL_REFERENCE_ERROR_HASH); return invalid; }
		if (!outputCallback(writer, output)) { failTrace(KERNEL_REFERENCE_ERROR_CALLBACK); return invalid; }
		digest = writer.finish();
	}
	catch (...) { failTrace(KERNEL_REFERENCE_ERROR_CALLBACK); return invalid; }
	return linkValidatedDigest(token, digest);
}
KernelPerformanceReferenceBatch KernelPerformanceReferenceLedger::linkValidatedDigest(KernelPerformanceAttempt token,
	const KernelPerformanceDigest &output) noexcept
{
	KernelPerformanceReferenceBatch result;
	if (!traceReady()) return result;
	const unsigned attemptSlot = traceAttemptSlot(token);
	if (attemptSlot == ~0U) return result;
	State::Attempt &attempt = m_state->attempts[attemptSlot];
	if (!output.valid || attempt.validated || attempt.finished || !attempt.captured || !attempt.admitted ||
		!attempt.dispatchSeen || !attempt.successfulRanges || attempt.plannedRanges != attempt.dispatch.rangeCount ||
		attempt.releasedRanges != attempt.dispatch.rangeCount)
	{ failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return result; }
	if (m_state->consuming())
	{
		State::Event finish;
		if (!m_state->sourceFinish(attempt, finish)) return result;
		const bool sourceCommitted = finish.v[4] == KERNEL_PERFORMANCE_COMMITTED;
		if ((!sourceCommitted && finish.v[4] != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION) ||
			finish.v[9] != 1 || finish.v[10] != attempt.inputSchema ||
			finish.v[11] != attempt.capturedOperations || !finish.digest.equals(output) ||
			(finish.v[18] != 0) != sourceCommitted)
		{ failTrace(KERNEL_REFERENCE_ERROR_MISMATCH); return result; }
	}
	unsigned slot = 0;
	while (slot != KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES && m_state->pending[slot].active) ++slot;
	if (slot == KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES) { failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY); return result; }
	if (m_state->nextSerial == ~static_cast<JobMetricCounter>(0)) { failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW); return result; }
	const auto kernel = static_cast<KernelPerformanceKernel>(attempt.identity.workKind);
	const unsigned subtype = attempt.identity.subtype, frame = attempt.identity.ownerFrame;
	const JobMetricCounter ordinal = attempt.identity.attemptOrdinal, operations = attempt.capturedOperations;
	unsigned streamIndex = 0;
	while (streamIndex != m_state->streamCount && (m_state->streams[streamIndex].metric.kernel != kernel ||
		m_state->streams[streamIndex].metric.subtype != subtype)) ++streamIndex;
	if (streamIndex == KERNEL_PERFORMANCE_MAXIMUM_STREAMS) { failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY); return result; }
	State::Stream &stream = m_state->streams[streamIndex];
	if (streamIndex == m_state->streamCount)
	{
		stream.metric.kernel = kernel; stream.metric.subtype = subtype; stream.metric.fieldSchema = attempt.inputSchema;
		stream.metric.firstFrame = frame;
		if (!beginStream(stream.input, kernel, subtype, attempt.inputSchema, 1) ||
			!beginStream(stream.output, kernel, subtype, attempt.inputSchema, 2) ||
			!beginStream(stream.commit, kernel, subtype, attempt.inputSchema, 3))
		{ failTrace(KERNEL_REFERENCE_ERROR_HASH); return result; }
		++m_state->streamCount;
	}
	else if (stream.metric.fieldSchema != attempt.inputSchema || frame < stream.metric.lastFrame ||
		(frame == stream.metric.lastFrame && ordinal <= stream.ordinal))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY); return result; }
	KernelPerformanceReferenceStream updated = stream.metric;
	if (!checkedAdd(updated.validatedBatchCount, 1) || !checkedAdd(updated.validatedOperationCount, operations))
	{ failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW); return result; }
	if (!appendIdentity(stream.input, frame, ordinal, operations) || !appendDigest(stream.input, attempt.inputDigest) ||
		!appendIdentity(stream.output, frame, ordinal, operations) || !appendDigest(stream.output, output))
	{ failTrace(KERNEL_REFERENCE_ERROR_HASH); return result; }
	updated.lastFrame = frame; stream.metric = updated; stream.ordinal = ordinal;
	State::Pending &pending = m_state->pending[slot]; pending = State::Pending();
	pending.active = true; pending.stream = streamIndex; pending.frame = frame; pending.serial = ++m_state->nextSerial;
	pending.ordinal = ordinal; pending.operations = operations; pending.attemptSerial = attempt.serial;
	++m_state->openCount;
	result.m_owner = this; result.generation = m_snapshot.generation; result.serial = pending.serial; result.slot = slot;
	attempt.validated = true; attempt.outputDigest = output; attempt.validationOrdinal = pending.serial; attempt.batch = result;
	return result;
}

bool KernelPerformanceReferenceLedger::finishAttempt(KernelPerformanceAttempt token,
	const KernelPerformanceAttemptFinish &finish) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	const bool success = finish.disposition == KERNEL_PERFORMANCE_COMMITTED;
	if (!attempt.decisionSeen || attempt.finished ||
		(finish.disposition != KERNEL_PERFORMANCE_NOT_ADMITTED && finish.disposition != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION && !success) ||
		(attempt.admitted != (finish.disposition != KERNEL_PERFORMANCE_NOT_ADMITTED)))
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (finish.reasonSchema == 0 || finish.fallbackEntered != finish.fallbackCompleted)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	const bool nativeCapture = attempt.inputSchema == 0x5100;
	if (nativeCapture || finish.reasonSchema == 0x5002)
	{
		if (!nativeCapture || finish.reasonSchema != 0x5002 || finish.reason != attempt.nativeClass ||
			finish.disposition != KERNEL_PERFORMANCE_NOT_ADMITTED || !finish.fallbackEntered ||
			attempt.dispatchSeen || !m_state->fullContactReady || !m_state->fullContactDigest.valid ||
			m_state->insertedContacts > m_state->contacts || m_state->contacts > attempt.capturedOperations)
			return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	}
	if (success)
	{
		if (!attempt.validated || !attempt.commitObserved || !attempt.committed || !attempt.successfulRanges ||
			finish.fallbackEntered || finish.validatedBatch.m_owner != this ||
			finish.validatedBatch.generation != attempt.batch.generation || finish.validatedBatch.serial != attempt.batch.serial ||
			finish.validatedBatch.slot != attempt.batch.slot) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	}
	else if (attempt.validated)
	{
		if (finish.disposition != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION ||
			!attempt.commitObserved || attempt.committed || !attempt.successfulRanges ||
			finish.validatedBatch.m_owner != this ||
			finish.validatedBatch.generation != attempt.batch.generation ||
			finish.validatedBatch.serial != attempt.batch.serial ||
			finish.validatedBatch.slot != attempt.batch.slot)
			return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	}
	else if (finish.validatedBatch.m_owner != 0 || finish.validatedBatch.generation != 0 ||
		finish.validatedBatch.serial != 0 || finish.validatedBatch.slot != KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter &dispositionCount = attempt.admitted ? m_snapshot.trace.abortedAfterAdmissionAttemptCount :
		m_snapshot.trace.notAdmittedAttemptCount;
	JobMetricCounter count = dispositionCount;
	if (!success && !checkedAdd(count, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(9) || !writer.u64(3, attempt.serial) || !writer.u32(4, static_cast<unsigned>(finish.disposition)) ||
		!writer.u32(5, finish.reasonSchema) || !writer.u32(6, finish.reason) ||
		!writer.boolean(7, finish.fallbackEntered) || !writer.boolean(8, finish.fallbackCompleted) ||
		!writer.boolean(9, attempt.validated) ||
		(attempt.validated && (!writer.u32(10, attempt.inputSchema) || !writer.u64(11, attempt.capturedOperations) ||
			!writer.u64(12, attempt.validationOrdinal) || !appendDigest(writer, attempt.outputDigest) || !writer.boolean(18, attempt.committed))))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	if (nativeCapture && (!writer.u64(19, m_state->contacts) || !writer.u64(20, m_state->insertedContacts) ||
		!appendDigest(writer, m_state->fullContactDigest, 21))) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	if (nativeCapture) { m_state->restrictedReapSerial = attempt.serial; m_state->fullContactReady = false; }
	attempt.finished = true;
	dispositionCount = count;
	return true;
}
bool KernelPerformanceReferenceLedger::reapAttempt(KernelPerformanceAttempt token,
	const KernelPerformanceAttemptReap &reap) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (!attempt.finished) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (m_state->restrictedReapSerial != 0 && (m_state->restrictedReapSerial != attempt.serial ||
		reap.reasonSchema != 1 || reap.reason != 1 || reap.dynamicFactsKnownMask != 0 ||
		reap.pendingJobs != 0 || reap.outstandingJobs != 0 || reap.activeSlots != 0))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (attempt.dispatchSeen && (attempt.plannedRanges != attempt.dispatch.rangeCount ||
		attempt.releasedRanges != attempt.dispatch.rangeCount))
		return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (reap.reasonSchema == 0 ||
		!traceDynamicFactsValid(reap.dynamicFactsKnownMask, reap.pendingJobs, reap.outstandingJobs, reap.activeSlots))
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter count = m_snapshot.trace.reapCount;
	if (!checkedAdd(count, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(10) || !writer.u64(3, attempt.serial) || !writer.u32(4, reap.reasonSchema) ||
		!writer.u32(5, reap.reason) || !writer.u32(6, reap.dynamicFactsKnownMask) || !writer.u64(7, reap.pendingJobs) ||
		!writer.u64(8, reap.outstandingJobs) || !writer.u64(9, reap.activeSlots))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	// The native owner separately proves group-terminal cleanup. A released
	// range or active-slot observation cannot substitute for that boundary.
	attempt.active = false;
	if (m_state->restrictedReapSerial == attempt.serial) m_state->restrictedReapSerial = 0;
	m_snapshot.trace.reapCount = count;
	--m_snapshot.trace.residentAttemptCount;
	return true;
}
bool KernelPerformanceReferenceLedger::observeWindowBoundary(const KernelPerformanceWindowBoundary &boundary) noexcept
{
	if (!traceReady()) return false;
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	if (trace.observationSealed || trace.executionSealed || m_state->body.valid() || m_state->ownerSerial.valid())
		return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (boundary.kind < KERNEL_WINDOW_BEGIN || boundary.kind > KERNEL_WINDOW_DEFERRED_START_CONSUMED ||
		boundary.sampleOrdinal == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	State::Window window = m_state->window;
	JobMetricCounter boundaries = trace.windowBoundaryCount, completed = trace.completedWindowCount, controls = trace.controlWindowCount;
	if (boundary.kind == KERNEL_WINDOW_BEGIN)
	{
		if (window.open || (!window.seen && trace.attemptCount != 0))
			return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
		if (boundary.phase != KERNEL_PHASE_COUNT || boundary.sampleOrdinal <= window.lastSample ||
			boundary.ownerFrameAtEntry != boundary.authorityFrame || boundary.authorityFrame != boundary.actualOwnerFrame)
			return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		window.seen = window.open = true;
		window.phaseOpen = false; window.nextPhase = 0;
		window.sample = boundary.sampleOrdinal;
		window.entry = boundary.ownerFrameAtEntry;
		window.authority = boundary.authorityFrame; window.actual = boundary.actualOwnerFrame;
	}
	else
	{
		if (!window.open || boundary.sampleOrdinal != window.sample || boundary.ownerFrameAtEntry != window.entry)
			return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		if (boundary.kind == KERNEL_WINDOW_PHASE_BEGIN)
		{
			if (window.phaseOpen || window.nextPhase >= KERNEL_PHASE_COUNT ||
				static_cast<unsigned>(boundary.phase) != window.nextPhase)
				return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
			if (boundary.authorityFrame != window.actual || boundary.actualOwnerFrame != window.actual)
				return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
			window.phaseOpen = true; window.authority = boundary.authorityFrame;
		}
		else if (boundary.kind == KERNEL_WINDOW_PHASE_END)
		{
			if (!window.phaseOpen || static_cast<unsigned>(boundary.phase) != window.nextPhase)
				return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
			if (boundary.authorityFrame != window.authority) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
			// Actual world identity can reset in intake or advance in verification;
			// the borrowed phase authority remains the exact begin observation.
			window.actual = boundary.actualOwnerFrame; window.phaseOpen = false; ++window.nextPhase;
		}
		else if (boundary.kind == KERNEL_WINDOW_DEFERRED_START_DECLARED || boundary.kind == KERNEL_WINDOW_DEFERRED_START_CONSUMED)
		{
			if (!window.phaseOpen || window.nextPhase != KERNEL_PHASE_OWNER_INTAKE || boundary.phase != KERNEL_PHASE_OWNER_INTAKE)
				return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
			if (boundary.authorityFrame != window.authority || boundary.actualOwnerFrame != 0)
				return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
			if (boundary.kind == KERNEL_WINDOW_DEFERRED_START_DECLARED)
			{
				if (window.controlState != 0 || completed != 0) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
				window.controlState = KERNEL_CONTROL_DEFERRED_START_DECLARED;
			}
			else
			{
				if (window.controlState != KERNEL_CONTROL_DEFERRED_START_DECLARED) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
				window.controlState = KERNEL_CONTROL_DEFERRED_START_CONSUMED;
			}
			window.actual = boundary.actualOwnerFrame;
		}
		else
		{
			if (window.phaseOpen || window.nextPhase != KERNEL_PHASE_COUNT)
				return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
			if (boundary.phase != KERNEL_PHASE_COUNT || boundary.authorityFrame != window.entry ||
				boundary.actualOwnerFrame != window.actual) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
			if (boundary.kind == KERNEL_WINDOW_CONTROL_END)
			{
				if (window.controlState != KERNEL_CONTROL_DEFERRED_START_DECLARED || window.actual != 0 || completed != 0)
					return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
				if (!checkedAdd(controls, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
			}
			else
			{
				if (window.controlState == KERNEL_CONTROL_DEFERRED_START_DECLARED || window.actual == 0 ||
					(completed != 0 && window.actual <= window.lastCompleted)) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
				if (!checkedAdd(completed, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
				window.lastCompleted = window.actual;
			}
			window.open = false; window.lastSample = window.sample;
		}
	}
	if (!checkedAdd(boundaries, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(2) || !writer.u32(3, static_cast<unsigned>(boundary.kind)) ||
		!writer.u64(4, boundary.sampleOrdinal) || !writer.u32(5, static_cast<unsigned>(boundary.phase)) ||
		!writer.u32(6, boundary.ownerFrameAtEntry) || !writer.u32(7, boundary.authorityFrame) || !writer.u32(8, boundary.actualOwnerFrame))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	if (boundary.kind == KERNEL_WINDOW_PHASE_END && !writer.flush()) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	m_state->window = window;
	trace.windowBoundaryCount = boundaries; trace.completedWindowCount = completed; trace.controlWindowCount = controls;
	return true;
}
bool KernelPerformanceReferenceLedger::sealObservationWindow() noexcept
{
	if (!traceReady()) return false;
	if (m_snapshot.trace.observationSealed) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (m_state->window.open || m_state->window.controlState == KERNEL_CONTROL_DEFERRED_START_DECLARED ||
		(m_state->window.seen && m_snapshot.trace.completedWindowCount == 0))
		return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	CallbackGuard guard(m_state->busy);
	if (!m_state->traceRecord(2) || !m_state->traceWriter.u32(3, 1) ||
		!m_state->traceWriter.u64(4, m_snapshot.trace.residentAttemptCount) || !m_state->traceWriter.flush())
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	m_snapshot.trace.observationSealed = true;
	return true;
}
bool KernelPerformanceReferenceLedger::sealExecutionClosure() noexcept
{
	if (!traceReady()) return false;
	if (!m_snapshot.trace.observationSealed || m_snapshot.trace.executionSealed ||
		m_snapshot.trace.residentAttemptCount != 0 || m_snapshot.trace.residentRangeCount != 0 ||
		m_state->window.open || m_state->window.controlState == KERNEL_CONTROL_DEFERRED_START_DECLARED)
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	CallbackGuard guard(m_state->busy);
	if (!m_state->traceRecord(2) || !m_state->traceWriter.u32(3, 2) ||
		!m_state->traceWriter.u64(4, m_snapshot.trace.residentAttemptCount) || !m_state->traceWriter.flush())
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	m_snapshot.trace.executionSealed = true;
	return true;
}
KernelPerformanceReferenceBatch KernelPerformanceReferenceLedger::observeValidatedBatch(KernelPerformanceKernel kernel,
	unsigned subtype, unsigned frame, JobMetricCounter ordinal, unsigned fieldSchema, JobMetricCounter operationCount,
	KernelPerformanceCanonicalCallback writeInput, const void *immutableInput,
	KernelPerformanceCanonicalCallback writeOutput, const void *productionOutput,
	KernelPerformanceSerialCallback serialCompute, void *detachedSerialOutput) noexcept
{
	KernelPerformanceReferenceBatch token;
	if (!owner()) return token;
	if (m_snapshot.frozen || m_snapshot.mode == KERNEL_REFERENCE_DISABLED || m_snapshot.generation == 0) return token;
	if (m_foreignCall.load(std::memory_order_acquire)) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OWNER;
	if (m_state == 0 || m_snapshot.errors != 0) return token;
	if (m_state->busy) { m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE; return token; }
	// Traced success must use its captured attempt, never an anonymous second
	// input/output observation. Keep the existing untraced oracle path.
	if (m_snapshot.trace.requested) { failTrace(KERNEL_PERFORMANCE_ERROR_STATE); return token; }
	const unsigned maximumSubtype = kernel == KERNEL_PERFORMANCE_AI || kernel == KERNEL_PERFORMANCE_PATH ? 1 : 0;
	if (kernel < KERNEL_PERFORMANCE_PHYSICS || kernel >= KERNEL_PERFORMANCE_KERNEL_COUNT ||
		subtype > maximumSubtype || fieldSchema == 0 || operationCount == 0 ||
		writeInput == 0 || writeOutput == 0 || immutableInput == 0 || productionOutput == 0)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY; return token; }
	unsigned slot = 0;
	while (slot != KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES && m_state->pending[slot].active) ++slot;
	if (slot == KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_CAPACITY; return token; }
	if (m_state->nextSerial == ~static_cast<JobMetricCounter>(0))
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OVERFLOW; return token; }
	unsigned streamIndex = 0;
	while (streamIndex != m_state->streamCount &&
		(m_state->streams[streamIndex].metric.kernel != kernel || m_state->streams[streamIndex].metric.subtype != subtype)) ++streamIndex;
	if (streamIndex == KERNEL_PERFORMANCE_MAXIMUM_STREAMS)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_CAPACITY; return token; }
	State::Stream &stream = m_state->streams[streamIndex];
	if (streamIndex == m_state->streamCount)
	{
		stream.metric.kernel = kernel;
		stream.metric.subtype = subtype;
		stream.metric.fieldSchema = fieldSchema;
		stream.metric.firstFrame = frame;
		if (!beginStream(stream.input, kernel, subtype, fieldSchema, 1) ||
			!beginStream(stream.output, kernel, subtype, fieldSchema, 2) ||
			!beginStream(stream.commit, kernel, subtype, fieldSchema, 3))
		{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return token; }
		++m_state->streamCount;
	}
	else if (stream.metric.fieldSchema != fieldSchema || frame < stream.metric.lastFrame ||
		(frame == stream.metric.lastFrame && ordinal <= stream.ordinal))
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY; return token; }
	KernelPerformanceReferenceStream updated = stream.metric;
	if (!checkedAdd(updated.validatedBatchCount, 1) || !checkedAdd(updated.validatedOperationCount, operationCount))
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OVERFLOW; return token; }
	JobMetricCounter serialTime = 0;
	try
	{
		CallbackGuard guard(m_state->busy);
		KernelPerformanceCanonicalWriter inputWriter, outputWriter;
		if (!inputWriter.begin(fieldSchema) || !outputWriter.begin(fieldSchema))
		{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return token; }
		if (!writeInput(inputWriter, immutableInput) || !writeOutput(outputWriter, productionOutput))
		{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_CALLBACK; return token; }
		const KernelPerformanceDigest input = inputWriter.finish(), output = outputWriter.finish();
		if (!input.valid || !output.valid)
		{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return token; }
		if (m_snapshot.mode == KERNEL_REFERENCE_SERIAL_ORACLE)
		{
			if (serialCompute == 0 || detachedSerialOutput == 0 ||
				detachedSerialOutput == productionOutput || detachedSerialOutput == immutableInput)
			{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY; return token; }
			JobMetricCounter start = 0, end = 0;
			if (!m_state->now(start)) { m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_CLOCK; return token; }
			if (!serialCompute(immutableInput, detachedSerialOutput))
			{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_CALLBACK; return token; }
			if (!m_state->now(end)) { m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_CLOCK; return token; }
			serialTime = end - start;
			KernelPerformanceCanonicalWriter serialWriter;
			if (!serialWriter.begin(fieldSchema)) { m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return token; }
			if (!writeOutput(serialWriter, detachedSerialOutput))
			{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_CALLBACK; return token; }
			const KernelPerformanceDigest serialOutput = serialWriter.finish();
			if (!serialOutput.valid) { m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return token; }
			if (!serialOutput.equals(output)) { m_snapshot.errors |= KERNEL_REFERENCE_ERROR_MISMATCH; return token; }
		}
		if (m_snapshot.errors != 0 || m_snapshot.frozen ||
			!appendIdentity(stream.input, frame, ordinal, operationCount) || !appendDigest(stream.input, input) ||
			!appendIdentity(stream.output, frame, ordinal, operationCount) || !appendDigest(stream.output, output))
		{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return token; }
	}
	catch (...) { m_snapshot.errors |= KERNEL_REFERENCE_ERROR_CALLBACK; return token; }
	updated.lastFrame = frame;
	stream.metric = updated;
	stream.ordinal = ordinal;
	State::Pending &pending = m_state->pending[slot];
	pending.active = true; pending.stream = streamIndex; pending.frame = frame;
	pending.serial = ++m_state->nextSerial; pending.ordinal = ordinal;
	pending.operations = operationCount; pending.serialTime = serialTime;
	++m_state->openCount;
	token.m_owner = this; token.generation = m_snapshot.generation; token.serial = pending.serial; token.slot = slot;
	return token;
}
bool KernelPerformanceReferenceLedger::finishBatch(KernelPerformanceReferenceBatch batch, bool committed) noexcept
{
	if (!owner()) return false;
	if (m_snapshot.frozen || m_snapshot.mode == KERNEL_REFERENCE_DISABLED || m_snapshot.generation == 0) return false;
	if (m_foreignCall.load(std::memory_order_acquire)) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OWNER;
	if (m_state == 0 || m_snapshot.errors != 0) return false;
	if (m_state->busy) { m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE; return false; }
	if (batch.m_owner != this || batch.generation != m_snapshot.generation || !batch.valid() ||
		batch.slot >= KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES || !m_state->pending[batch.slot].active ||
		m_state->pending[batch.slot].serial != batch.serial)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY; return false; }
	State::Pending &pending = m_state->pending[batch.slot];
	State::Attempt *attempt = 0;
	if (m_snapshot.trace.requested)
	{
		for (unsigned i = 0; i != m_state->attemptCapacity; ++i)
			if (m_state->attempts[i].active && m_state->attempts[i].serial == pending.attemptSerial) attempt = &m_state->attempts[i];
		if (attempt == 0 || !attempt->validated || attempt->commitObserved)
			return failTrace(KERNEL_REFERENCE_ERROR_MISMATCH);
	}
	State::Stream &stream = m_state->streams[pending.stream];
	KernelPerformanceReferenceStream updated = stream.metric;
	if (committed)
	{
		if (!checkedAdd(updated.committedBatchCount, 1) || !checkedAdd(updated.committedOperationCount, pending.operations) ||
			(m_snapshot.mode == KERNEL_REFERENCE_SERIAL_ORACLE &&
				(!checkedAdd(updated.serialSampleCount, 1) || !checkedAdd(updated.serialNanoseconds, pending.serialTime))))
		{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OVERFLOW; return false; }
		if (pending.serialTime > updated.maximumSerialNanoseconds) updated.maximumSerialNanoseconds = pending.serialTime;
	}
	else if (!checkedAdd(updated.abortedBatchCount, 1))
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OVERFLOW; return false; }
	if (!appendIdentity(stream.commit, pending.frame, pending.ordinal, pending.operations) || !stream.commit.boolean(13, committed))
	{ m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH; return false; }
	stream.metric = updated;
	if (attempt != 0) { attempt->commitObserved = true; attempt->committed = committed; }
	pending.active = false;
	--m_state->openCount;
	return true;
}
KernelPerformanceReferenceSnapshot KernelPerformanceReferenceLedger::freeze() noexcept
{
	if (!owner()) return KernelPerformanceReferenceSnapshot();
	if (m_snapshot.frozen) return m_snapshot;
	if (m_state != 0 && m_state->busy)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE; return KernelPerformanceReferenceSnapshot(); }
	if (m_snapshot.generation == 0) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE;
	if (m_foreignCall.load(std::memory_order_acquire)) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OWNER;
	if (m_state != 0)
	{
		if (m_state->body.valid() || m_state->ownerSerial.valid()) failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
		if (m_state->openCount != 0) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_INCOMPLETE;
		m_snapshot.streamCount = m_state->streamCount;
		for (unsigned index = 0; index != m_state->streamCount; ++index)
		{
			State::Stream &stream = m_state->streams[index];
			stream.metric.inputDigest = stream.input.finish();
			stream.metric.outputDigest = stream.output.finish();
			stream.metric.commitDigest = stream.commit.finish();
			if (!stream.metric.inputDigest.valid || !stream.metric.outputDigest.valid || !stream.metric.commitDigest.valid)
				m_snapshot.errors |= KERNEL_REFERENCE_ERROR_HASH;
			m_snapshot.streams[index] = stream.metric;
		}
	}
	if (m_snapshot.trace.requested)
	{
		KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
		trace.errors |= m_snapshot.errors;
		if (m_state == 0 || !trace.observationSealed || !trace.executionSealed ||
			trace.residentAttemptCount != 0 || trace.residentRangeCount != 0 || m_state->window.open ||
			m_state->window.controlState == KERNEL_CONTROL_DEFERRED_START_DECLARED ||
			(m_state->window.seen && trace.completedWindowCount == 0))
			failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
		if (trace.errors == 0)
		{
			JobMetricCounter validated = 0, committed = 0, validatedOperations = 0, committedOperations = 0;
			for (unsigned i = 0; i != m_state->streamCount; ++i)
			{
				const auto &stream = m_state->streams[i].metric;
				if (!checkedAdd(validated, stream.validatedBatchCount) || !checkedAdd(committed, stream.committedBatchCount) ||
					!checkedAdd(validatedOperations, stream.validatedOperationCount) || !checkedAdd(committedOperations, stream.committedOperationCount))
					failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
			}
			CallbackGuard guard(m_state->busy);
			KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
			if (!m_state->traceRecord(11) || !writer.u64(3, trace.recordCount) || !writer.u64(4, trace.logicalEventCount) ||
				!writer.u64(5, trace.attemptCount) || !writer.u64(6, trace.admittedAttemptCount) ||
				!writer.u64(7, trace.notAdmittedAttemptCount) || !writer.u64(8, trace.abortedAfterAdmissionAttemptCount) ||
				!writer.u64(9, trace.reapCount) || !writer.u64(10, trace.residentAttemptCount) ||
				!writer.u64(11, trace.residentAttemptHighWater) || !writer.u64(12, trace.coalescedSpanCount) ||
				!writer.u64(13, trace.coalescedAttemptCount) || !writer.boolean(14, trace.observationSealed) ||
				!writer.boolean(15, trace.executionSealed) || !writer.u64(16, trace.capturedAttemptCount) ||
				!writer.u64(17, m_state->prevalidating && trace.coalescedSpanCount != 0 ?
					m_state->preflightCapturedOperations : trace.capturedOperationCount) || !writer.u64(18, trace.dispatchCount) ||
				!writer.u64(19, trace.rangeCount) || !writer.u64(20, trace.releasedRangeCount) ||
				!writer.u64(21, trace.residentRangeCount) || !writer.u64(22, trace.residentRangeHighWater) ||
				!writer.u64(23, validated) || !writer.u64(24, committed) || !writer.u64(25, validatedOperations) || !writer.u64(26, committedOperations) ||
				!writer.u64(27, trace.windowBoundaryCount) || !writer.u64(28, trace.completedWindowCount) || !writer.u64(29, trace.controlWindowCount))
				failTrace(KERNEL_REFERENCE_ERROR_HASH);
			else
			{
				trace.digest = writer.finish();
				if (!trace.digest.valid) failTrace(KERNEL_REFERENCE_ERROR_HASH);
				if (m_state->consuming() && (trace.byteCount != m_state->sourceBytes || !trace.digest.equals(m_state->sourceDigest)))
					failTrace(KERNEL_REFERENCE_ERROR_TRACE_BINDING);
			}
		}
		if (trace.errors == 0 && m_state->consuming()) m_state->verifySourceHash();
		trace.frozen = true;
		trace.complete = trace.errors == 0 && trace.digest.valid && trace.observationSealed &&
			trace.executionSealed && trace.residentAttemptCount == 0 && trace.residentRangeCount == 0;
	}
	m_snapshot.frozen = true;
	m_snapshot.complete = m_snapshot.generation != 0 && m_snapshot.mode != KERNEL_REFERENCE_DISABLED &&
		m_snapshot.errors == 0 && m_snapshot.streamCount != 0;
	return m_snapshot;
}
bool KernelPerformanceReferenceLedger::validateSource() noexcept
{
	// Preflight uses the same lifecycle checks, resident metadata, counter
	// arithmetic and canonical emission as live observation. It supplies only
	// already encoded digests, never invokes game callbacks or body authority.
	while (m_snapshot.errors == 0 && m_snapshot.trace.byteCount < m_state->sourceBytes)
	{
		State::Event event;
		if (!m_state->eventAt(m_snapshot.trace.byteCount, event)) return false;
		const unsigned kind = static_cast<unsigned>(event.v[1]);
		if (kind == 11)
		{
			if (m_snapshot.trace.coalescedSpanCount != 0)
			{
				if (event.v[17] < m_snapshot.trace.capturedOperationCount) return failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
				m_state->preflightCapturedOperations = event.v[17];
			}
			return freeze().trace.complete;
		}
		if (kind == 12)
		{
			CallbackGuard guard(m_state->busy);
			if (!m_state->spanRecord(event)) return false;
			continue;
		}
		if (kind == 2)
		{
			if (event.v[3] == 1) { if (!sealObservationWindow()) return false; }
			else if (event.v[3] == 2) { if (!sealExecutionClosure()) return false; }
			else
			{
				KernelPerformanceWindowBoundary boundary;
				boundary.kind = static_cast<KernelPerformanceWindowBoundaryKind>(event.v[3]);
				boundary.sampleOrdinal = event.v[4]; boundary.phase = static_cast<KernelPerformancePhase>(event.v[5]);
				boundary.ownerFrameAtEntry = static_cast<unsigned>(event.v[6]);
				boundary.authorityFrame = static_cast<unsigned>(event.v[7]); boundary.actualOwnerFrame = static_cast<unsigned>(event.v[8]);
				if (!observeWindowBoundary(boundary)) return false;
			}
			continue;
		}
		if (kind == 3)
		{
			KernelPerformanceAttemptIdentity identity = { static_cast<unsigned>(event.v[4]), static_cast<unsigned>(event.v[5]),
				event.v[6], event.v[9], static_cast<KernelPerformancePhase>(event.v[7]), static_cast<unsigned>(event.v[8]) };
			if (!beginAttempt(identity).valid()) return false;
			continue;
		}
		unsigned slot = 0;
		while (slot != m_state->attemptCapacity && (!m_state->attempts[slot].active || m_state->attempts[slot].serial != event.v[3])) ++slot;
		if (slot == m_state->attemptCapacity) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		KernelPerformanceAttempt token;
		token.m_owner = this; token.m_generation = m_snapshot.generation; token.m_serial = event.v[3]; token.m_slot = slot;
		if (kind == 4) { if (!observeDecision(token, event.decision())) return false; }
		else if (kind == 5)
		{ if (!bindCapturedDigest(token, static_cast<unsigned>(event.v[4]), event.v[5], event.digest)) return false; }
		else if (kind == 6) { if (!observeDispatch(token, event.dispatch())) return false; }
		else if (kind == 7) { if (!observeRangePlan(token, event.range())) return false; }
		else if (kind == 8) { if (!observeReleasedRange(token, event.range(), event.progress())) return false; }
		else if (kind == 13)
		{
			unsigned rangeSlot = 0;
			while (rangeSlot != m_state->rangeCapacity && (!m_state->ranges[rangeSlot].active ||
				m_state->ranges[rangeSlot].attemptSerial != token.m_serial ||
				m_state->ranges[rangeSlot].plan.dispatchOrdinal != event.v[4] ||
				m_state->ranges[rangeSlot].plan.rangeOrdinal != event.v[5])) ++rangeSlot;
			if (rangeSlot == m_state->rangeCapacity) return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
			if (!observeReleasedRequestBudget(token, m_state->ranges[rangeSlot].plan, event.budget())) return false;
		}
		else if (kind == 9)
		{
			KernelPerformanceAttemptFinish finish = {};
			finish.disposition = static_cast<KernelPerformanceDisposition>(event.v[4]);
			finish.reasonSchema = static_cast<unsigned>(event.v[5]); finish.reason = static_cast<unsigned>(event.v[6]);
			finish.fallbackEntered = event.v[7] != 0; finish.fallbackCompleted = event.v[8] != 0;
			finish.validationObserved = event.v[9] != 0;
			if (event.v[9] == 0 && event.v[5] == 0x5002)
			{
				m_state->contacts = event.v[19]; m_state->insertedContacts = event.v[20];
				m_state->fullContactDigest = event.digest; m_state->fullContactReady = true;
			}
			if (event.v[9] != 0)
			{
				finish.validatedBatch = linkValidatedDigest(token, event.digest);
				if (!finish.validatedBatch.valid() || !finishBatch(finish.validatedBatch, event.v[18] != 0)) return false;
			}
			if (!finishAttempt(token, finish)) return false;
		}
		else if (kind == 10)
		{
			KernelPerformanceAttemptReap reap = { static_cast<unsigned>(event.v[4]), static_cast<unsigned>(event.v[5]),
				static_cast<unsigned>(event.v[6]), event.v[7], event.v[8], event.v[9] };
			if (!reapAttempt(token, reap)) return false;
		}
		else return failTrace(KERNEL_REFERENCE_ERROR_TRACE_ENCODING);
	}
	return failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
}
} }
