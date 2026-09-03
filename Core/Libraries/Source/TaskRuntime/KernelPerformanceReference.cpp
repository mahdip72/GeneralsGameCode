#include "Lib/KernelPerformanceReference.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <string.h>
#include <new>
#include <windows.h>
#include <bcrypt.h>

namespace rts { namespace performance {
KernelPerformanceDigest::KernelPerformanceDigest() : valid(false) { memset(bytes, 0, sizeof(bytes)); }
bool KernelPerformanceDigest::equals(const KernelPerformanceDigest &other) const
{ return valid && other.valid && memcmp(bytes, other.bytes, sizeof(bytes)) == 0; }

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
	residentAttemptCapacity(0), residentRangeCapacity(0), append(0), context(0) {}
KernelPerformanceReferenceRunOptions::KernelPerformanceReferenceRunOptions() : mode(KERNEL_REFERENCE_DISABLED),
	clock(0), clockContext(0), trace() {}
KernelPerformanceAttempt::KernelPerformanceAttempt() : m_owner(0), m_generation(0), m_serial(0), m_slot(0) {}
bool KernelPerformanceAttempt::valid() const { return m_owner != 0 && m_generation != 0 && m_serial != 0; }
KernelPerformanceTraceSnapshot::KernelPerformanceTraceSnapshot() : mode(KERNEL_TRACE_DISABLED), requested(false),
	frozen(false), complete(false), observationSealed(false), executionSealed(false), errors(0), binding(), limits(),
	residentAttemptCapacity(0), residentRangeCapacity(0), residentAttemptCount(0), residentAttemptHighWater(0),
	attemptCount(0), admittedAttemptCount(0), notAdmittedAttemptCount(0), abortedAfterAdmissionAttemptCount(0),
	reapCount(0), capturedAttemptCount(0), capturedOperationCount(0), dispatchCount(0), rangeCount(0), releasedRangeCount(0),
	residentRangeCount(0), residentRangeHighWater(0), recordCount(0), logicalEventCount(0),
	coalescedSpanCount(0), coalescedAttemptCount(0), byteCount(0) {}

struct KernelPerformanceCanonicalWriter::State
{
	enum { BufferCapacity = 65536 };
	State() : algorithm(0), hash(0), buffer(0), pending(0), buffered(false),
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
KernelPerformanceReferenceBatch::KernelPerformanceReferenceBatch() : generation(0), serial(0), slot(KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES) {}
bool KernelPerformanceReferenceBatch::valid() const { return generation != 0 && serial != 0; }
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
	return releasedCheckpointProgressValid(checkpoint) && progress.publication >= KERNEL_PUBLICATION_NOT_APPLICABLE &&
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
		Pending() : active(false), stream(0), frame(0), serial(0), ordinal(0), operations(0), serialTime(0) {}
		bool active;
		unsigned stream, frame;
		JobMetricCounter serial, ordinal, operations, serialTime;
	};
	struct Attempt
	{
		Attempt() : active(false), decisionSeen(false), finished(false), captured(false), admitted(false),
			dispatchSeen(false), serial(0), decisionOrdinal(0), capturedOperations(0), inputSchema(0),
			plannedRanges(0), releasedRanges(0), identity(), dispatch() {}
		bool active, decisionSeen, finished, captured, admitted, dispatchSeen;
		JobMetricCounter serial, decisionOrdinal, capturedOperations;
		unsigned inputSchema, plannedRanges, releasedRanges;
		KernelPerformanceAttemptIdentity identity;
		KernelPerformanceDigest inputDigest;
		KernelPerformanceDispatchPlan dispatch;
	};
	struct Range
	{
		Range() : active(false), attemptSerial(0), plan() {}
		bool active;
		JobMetricCounter attemptSerial;
		KernelPerformanceRangePlan plan;
	};
	struct AttemptOrder
	{
		AttemptOrder() : seen(false), sample(0), ordinal(0) {}
		bool seen;
		JobMetricCounter sample, ordinal;
	};
	State() : streamCount(0), openCount(0), nextSerial(0), lastClock(0), busy(false), clock(0), context(0),
		traceOwner(0), traceAppend(0), traceContext(0), attempts(0), ranges(0), attemptCapacity(0), rangeCapacity(0), nextAttemptSerial(0) {}
	~State() { delete[] ranges; delete[] attempts; }
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
		try { accepted = state.traceAppend(state.traceContext, bytes, count); }
		catch (...) { return ledger.failTrace(KERNEL_REFERENCE_ERROR_TRACE_IO); }
		if (!accepted) return ledger.failTrace(KERNEL_REFERENCE_ERROR_TRACE_IO);
		trace.byteCount += count;
		if (ledger.m_foreignCall.load(std::memory_order_acquire)) return ledger.failTrace(KERNEL_PERFORMANCE_ERROR_OWNER);
		return ledger.m_snapshot.errors == 0 && trace.errors == 0;
	}
	bool traceRecord(unsigned kind)
	{
		KernelPerformanceTraceSnapshot &trace = traceOwner->m_snapshot.trace;
		if (trace.recordCount >= trace.limits.maximumRecords || trace.logicalEventCount >= trace.limits.maximumLogicalEvents)
			return traceOwner->failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		++trace.recordCount;
		++trace.logicalEventCount;
		return traceWriter.u32(1, kind) && traceWriter.u64(2, trace.recordCount);
	}
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
};

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
	// Consumption and successful linkage are separate later slices. A
	// rejected configuration must not replace a previously frozen receipt.
	if (options.trace.mode != KERNEL_TRACE_RECORD || options.mode != KERNEL_REFERENCE_THROUGHPUT_BINDING)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	const KernelPerformanceTraceOptions &requested = options.trace;
	if (requested.append == 0 || !requested.binding.nativeRunIdentity.valid || !requested.binding.executable.valid ||
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
	const bool started = beginRun(options.mode, options.clock, options.clockContext);
	if (m_snapshot.generation == previousGeneration) return false;
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	trace.requested = true;
	trace.mode = requested.mode;
	trace.binding = requested.binding;
	trace.limits = requested.limits;
	trace.residentAttemptCapacity = requested.residentAttemptCapacity;
	trace.residentRangeCapacity = requested.residentRangeCapacity;
	if (!started) return failTrace(m_snapshot.errors);
	m_state->traceOwner = this;
	m_state->traceAppend = requested.append;
	m_state->traceContext = requested.context;
	m_state->attemptCapacity = static_cast<unsigned>(requested.residentAttemptCapacity);
	m_state->attempts = new (std::nothrow) State::Attempt[m_state->attemptCapacity];
	if (m_state->attempts == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	m_state->rangeCapacity = static_cast<unsigned>(requested.residentRangeCapacity);
	m_state->ranges = new (std::nothrow) State::Range[m_state->rangeCapacity];
	if (m_state->ranges == 0) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	// These reusable tables follow live bounds, never total event history.
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!writer.begin(0x5001, State::appendTrace, m_state) || !m_state->traceRecord(1) ||
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
	if (!m_state->traceRecord(3) || !writer.u64(3, serial) || !writer.u32(4, identity.workKind) ||
		!writer.u32(5, identity.subtype) || !writer.u64(6, identity.sampleOrdinal) ||
		!writer.u32(7, static_cast<unsigned>(identity.phase)) || !writer.u32(8, identity.ownerFrame) ||
		!writer.u64(9, identity.attemptOrdinal))
	{ failTrace(KERNEL_REFERENCE_ERROR_HASH); return token; }
	State::Attempt &attempt = m_state->attempts[slot];
	attempt = State::Attempt();
	attempt.active = true;
	attempt.serial = serial;
	attempt.identity = identity;
	m_state->nextAttemptSerial = serial;
	order.seen = true; order.sample = identity.sampleOrdinal; order.ordinal = identity.attemptOrdinal;
	++trace.attemptCount;
	++trace.residentAttemptCount;
	if (trace.residentAttemptCount > trace.residentAttemptHighWater) trace.residentAttemptHighWater = trace.residentAttemptCount;
	token.m_owner = this; token.m_generation = m_snapshot.generation; token.m_serial = serial; token.m_slot = slot;
	return token;
}
bool KernelPerformanceReferenceLedger::observeDecision(KernelPerformanceAttempt token,
	const KernelPerformanceAttemptDecision &decision) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (attempt.finished || (decision.admission == KERNEL_ADMISSION_ACCEPTED &&
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
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(4) || !writer.u64(3, attempt.serial) || !writer.u64(4, decision.decisionOrdinal) ||
		!writer.u32(5, decision.site) || !writer.u32(6, decision.reasonSchema) || !writer.u32(7, decision.reason) ||
		!writer.boolean(8, decision.deterministicEligible) || !writer.u32(9, static_cast<unsigned>(decision.admission)) ||
		!writer.u32(10, decision.sourceConfiguredWorkers) || !writer.u32(11, decision.dynamicFactsKnownMask) ||
		!writer.u64(12, decision.pendingJobs) || !writer.u64(13, decision.outstandingJobs) || !writer.u64(14, decision.activeSlots) ||
		!appendDigest(writer, decision.deterministicFacts, 15))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
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
	State::Attempt &attempt = m_state->attempts[slot];
	if (attempt.finished || attempt.captured) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (fieldSchema == 0 || operationCount == 0 || writeInput == 0 || immutableInput == 0)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter capturedCount = m_snapshot.trace.capturedAttemptCount;
	JobMetricCounter capturedOperations = m_snapshot.trace.capturedOperationCount;
	if (!checkedAdd(capturedCount, 1) || !checkedAdd(capturedOperations, operationCount))
		return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	try
	{
		CallbackGuard guard(m_state->busy);
		KernelPerformanceCanonicalWriter inputWriter;
		if (!inputWriter.begin(fieldSchema)) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		if (!writeInput(inputWriter, immutableInput)) return failTrace(KERNEL_REFERENCE_ERROR_CALLBACK);
		const KernelPerformanceDigest input = inputWriter.finish();
		if (!input.valid) return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		if (m_foreignCall.load(std::memory_order_acquire)) return failTrace(KERNEL_PERFORMANCE_ERROR_OWNER);
		if (m_snapshot.errors != 0) return failTrace(m_snapshot.errors);
		KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
		if (!m_state->traceRecord(5) || !writer.u64(3, attempt.serial) || !writer.u32(4, fieldSchema) ||
			!writer.u64(5, operationCount) || !appendDigest(writer, input, 6))
			return failTrace(KERNEL_REFERENCE_ERROR_HASH);
		attempt.inputDigest = input;
	}
	catch (...) { return failTrace(KERNEL_REFERENCE_ERROR_CALLBACK); }
	// Retain the canonical digest and identity only, never the native pointer.
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
	if (trace.observationSealed || attempt.finished || !attempt.captured ||
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
	if (!m_state->traceRecord(6) || !writer.u64(3, attempt.serial) || !writer.u64(4, dispatch.dispatchOrdinal) ||
		!writer.u32(5, dispatch.bodySchema) || !writer.u32(6, dispatch.checkpointSchema) || !writer.u32(7, dispatch.rangeCount) ||
		!writer.u64(8, dispatch.operationCount) || !writer.u64(9, dispatch.sourceGrain) || !writer.u64(10, dispatch.sourceLimit))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	attempt.dispatchSeen = true;
	attempt.dispatch = dispatch;
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
	// Plans enumerate declared range ordinals once. Released progress may be
	// imported in any owner-observed order, without retaining past dispatches.
	if (range.rangeOrdinal != attempt.plannedRanges) return failTrace(KERNEL_PERFORMANCE_ERROR_ORDER);
	KernelPerformanceTraceSnapshot &trace = m_snapshot.trace;
	if (trace.rangeCount >= trace.limits.maximumRanges) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	unsigned rangeSlot = 0;
	while (rangeSlot != m_state->rangeCapacity && m_state->ranges[rangeSlot].active) ++rangeSlot;
	if (rangeSlot == m_state->rangeCapacity) return failTrace(KERNEL_PERFORMANCE_ERROR_CAPACITY);
	CallbackGuard guard(m_state->busy);
	if (!m_state->traceRecord(7) || !appendRangePlan(m_state->traceWriter, attempt.serial, range))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
	State::Range &retained = m_state->ranges[rangeSlot];
	retained.active = true;
	retained.attemptSerial = attempt.serial;
	retained.plan = range;
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
	++attempt.releasedRanges;
	m_snapshot.trace.releasedRangeCount = releasedCount;
	--m_snapshot.trace.residentRangeCount;
	return true;
}
bool KernelPerformanceReferenceLedger::finishAttempt(KernelPerformanceAttempt token,
	const KernelPerformanceAttemptFinish &finish) noexcept
{
	if (!traceReady()) return false;
	const unsigned slot = traceAttemptSlot(token);
	if (slot == ~0U) return false;
	State::Attempt &attempt = m_state->attempts[slot];
	if (!attempt.decisionSeen || attempt.finished ||
		(finish.disposition != KERNEL_PERFORMANCE_NOT_ADMITTED && finish.disposition != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION) ||
		(attempt.admitted != (finish.disposition == KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION)))
		return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
	if (finish.reasonSchema == 0 || finish.fallbackEntered != finish.fallbackCompleted ||
		finish.validatedBatch.generation != 0 || finish.validatedBatch.serial != 0 ||
		finish.validatedBatch.slot != KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES)
		return failTrace(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter &dispositionCount = attempt.admitted ? m_snapshot.trace.abortedAfterAdmissionAttemptCount :
		m_snapshot.trace.notAdmittedAttemptCount;
	JobMetricCounter count = dispositionCount;
	if (!checkedAdd(count, 1)) return failTrace(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	CallbackGuard guard(m_state->busy);
	KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
	if (!m_state->traceRecord(9) || !writer.u64(3, attempt.serial) || !writer.u32(4, static_cast<unsigned>(finish.disposition)) ||
		!writer.u32(5, finish.reasonSchema) || !writer.u32(6, finish.reason) ||
		!writer.boolean(7, finish.fallbackEntered) || !writer.boolean(8, finish.fallbackCompleted) || !writer.boolean(9, false))
		return failTrace(KERNEL_REFERENCE_ERROR_HASH);
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
	m_snapshot.trace.reapCount = count;
	--m_snapshot.trace.residentAttemptCount;
	return true;
}
bool KernelPerformanceReferenceLedger::sealObservationWindow() noexcept
{
	if (!traceReady()) return false;
	if (m_snapshot.trace.observationSealed) return failTrace(KERNEL_PERFORMANCE_ERROR_STATE);
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
		m_snapshot.trace.residentAttemptCount != 0 || m_snapshot.trace.residentRangeCount != 0)
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
	// Successful traced batches require authenticated attempt linkage, which
	// this source-abort slice cannot yet establish. Keep the old untraced path.
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
	token.generation = m_snapshot.generation; token.serial = pending.serial; token.slot = slot;
	return token;
}
bool KernelPerformanceReferenceLedger::finishBatch(KernelPerformanceReferenceBatch batch, bool committed) noexcept
{
	if (!owner()) return false;
	if (m_snapshot.frozen || m_snapshot.mode == KERNEL_REFERENCE_DISABLED || m_snapshot.generation == 0) return false;
	if (m_foreignCall.load(std::memory_order_acquire)) m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OWNER;
	if (m_state == 0 || m_snapshot.errors != 0) return false;
	if (m_state->busy) { m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE; return false; }
	if (batch.generation != m_snapshot.generation || !batch.valid() ||
		batch.slot >= KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES || !m_state->pending[batch.slot].active ||
		m_state->pending[batch.slot].serial != batch.serial)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY; return false; }
	State::Pending &pending = m_state->pending[batch.slot];
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
			trace.residentAttemptCount != 0 || trace.residentRangeCount != 0)
			failTrace(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
		if (trace.errors == 0)
		{
			CallbackGuard guard(m_state->busy);
			KernelPerformanceCanonicalWriter &writer = m_state->traceWriter;
			if (!m_state->traceRecord(11) || !writer.u64(3, trace.recordCount) || !writer.u64(4, trace.logicalEventCount) ||
				!writer.u64(5, trace.attemptCount) || !writer.u64(6, trace.admittedAttemptCount) ||
				!writer.u64(7, trace.notAdmittedAttemptCount) || !writer.u64(8, trace.abortedAfterAdmissionAttemptCount) ||
				!writer.u64(9, trace.reapCount) || !writer.u64(10, trace.residentAttemptCount) ||
				!writer.u64(11, trace.residentAttemptHighWater) || !writer.u64(12, trace.coalescedSpanCount) ||
				!writer.u64(13, trace.coalescedAttemptCount) || !writer.boolean(14, trace.observationSealed) ||
				!writer.boolean(15, trace.executionSealed) || !writer.u64(16, trace.capturedAttemptCount) ||
				!writer.u64(17, trace.capturedOperationCount) || !writer.u64(18, trace.dispatchCount) ||
				!writer.u64(19, trace.rangeCount) || !writer.u64(20, trace.releasedRangeCount) ||
				!writer.u64(21, trace.residentRangeCount) || !writer.u64(22, trace.residentRangeHighWater))
				failTrace(KERNEL_REFERENCE_ERROR_HASH);
			else
			{
				trace.digest = writer.finish();
				if (!trace.digest.valid) failTrace(KERNEL_REFERENCE_ERROR_HASH);
			}
		}
		trace.frozen = true;
		trace.complete = trace.errors == 0 && trace.digest.valid && trace.observationSealed &&
			trace.executionSealed && trace.residentAttemptCount == 0 && trace.residentRangeCount == 0;
	}
	m_snapshot.frozen = true;
	m_snapshot.complete = m_snapshot.generation != 0 && m_snapshot.mode != KERNEL_REFERENCE_DISABLED &&
		m_snapshot.errors == 0 && m_snapshot.streamCount != 0;
	return m_snapshot;
}
} }
