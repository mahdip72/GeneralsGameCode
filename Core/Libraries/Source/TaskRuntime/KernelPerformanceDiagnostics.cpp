#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <string.h>
#include <windows.h>

namespace rts { namespace performance {

KernelPerformanceBatch::KernelPerformanceBatch() : generation(0), serial(0),
	slot(KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES) {}
bool KernelPerformanceBatch::valid() const { return generation != 0 && serial != 0; }
KernelPerformanceInterval::KernelPerformanceInterval() : generation(0), serial(0) {}
bool KernelPerformanceInterval::valid() const { return generation != 0 && serial != 0; }
KernelPerformanceBatchIdentity::KernelPerformanceBatchIdentity() : kernel(KERNEL_PERFORMANCE_PHYSICS),
	subtype(0), frame(0), ordinal(0) {}
KernelPerformanceStream::KernelPerformanceStream() : kernel(KERNEL_PERFORMANCE_PHYSICS),
	subtype(0), attemptedBatches(0), admittedBatches(0), committedBatches(0), abortedBatches(0),
	activePipelineNanoseconds(0), inclusiveBatchNanoseconds(0), maximumBatchNanoseconds(0),
	firstFrame(0), lastFrame(0)
{
	memset(stageNanoseconds, 0, sizeof(stageNanoseconds));
	memset(stageSamples, 0, sizeof(stageSamples));
}
KernelPerformanceSnapshot::KernelPerformanceSnapshot() : enabled(false), frozen(false),
	complete(false), errors(0), streamCount(0), generation(0) {}
KernelPerformanceLedger::KernelPerformanceLedger() : m_owner(GetCurrentThreadId()),
	m_foreignCall(false), m_started(false), m_enabled(false), m_frozen(false), m_errors(0),
	m_openBatches(0), m_depth(0), m_streamCount(0), m_generation(0), m_nextBatch(0),
	m_nextInterval(0), m_lastClock(0), m_clock(0), m_clockContext(0)
{
	memset(m_batches, 0, sizeof(m_batches));
	memset(m_intervals, 0, sizeof(m_intervals));
	memset(m_identities, 0, sizeof(m_identities));
}
KernelPerformanceLedger &KernelPerformanceLedger::instance()
{
	static KernelPerformanceLedger ledger;
	return ledger;
}

bool KernelPerformanceLedger::owner()
{
	if (m_owner.load(std::memory_order_acquire) == GetCurrentThreadId())
		return true;
	// A foreign caller touches only atomics, never the owner's mutable arrays.
	m_foreignCall.store(true, std::memory_order_release);
	return false;
}

bool KernelPerformanceLedger::fail(unsigned error)
{
	m_errors |= error;
	return false;
}

bool KernelPerformanceLedger::writable()
{
	if (!owner()) return false;
	if (!m_started || !m_enabled || m_frozen) return false;
	if (m_foreignCall.load(std::memory_order_acquire))
		return fail(KERNEL_PERFORMANCE_ERROR_OWNER);
	return m_errors == 0;
}

bool KernelPerformanceLedger::add(JobMetricCounter &total, JobMetricCounter amount)
{
	if (amount > ~static_cast<JobMetricCounter>(0) - total)
		return fail(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	total += amount;
	return true;
}

bool KernelPerformanceLedger::now(JobMetricCounter &value)
{
	value = m_clock != 0 ? m_clock(m_clockContext) : 0;
	// The production conversion saturates at the maximum on overflow. Such a
	// timestamp is not an exact clock observation and must not qualify.
	if (value == 0 || value == ~static_cast<JobMetricCounter>(0) || value < m_lastClock)
		return fail(KERNEL_PERFORMANCE_ERROR_CLOCK);
	m_lastClock = value;
	return true;
}

bool KernelPerformanceLedger::beginRun(bool enabled,
	KernelPerformanceClock clock, void *context)
{
	if (!owner()) return false;
	if (m_started && !m_frozen) return fail(KERNEL_PERFORMANCE_ERROR_STATE);
	if (m_generation == ~static_cast<JobMetricCounter>(0))
		return fail(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	++m_generation;
	m_started = true;
	m_enabled = enabled;
	m_frozen = false;
	m_errors = m_openBatches = m_depth = m_streamCount = 0;
	m_nextBatch = m_nextInterval = m_lastClock = 0;
	m_clock = clock != 0 ? clock : LiveSimulationPhaseClockNowNanoseconds;
	m_clockContext = context;
	m_foreignCall.store(false, std::memory_order_release);
	memset(m_batches, 0, sizeof(m_batches));
	memset(m_intervals, 0, sizeof(m_intervals));
	memset(m_identities, 0, sizeof(m_identities));
	for (unsigned index = 0; index != KERNEL_PERFORMANCE_MAXIMUM_STREAMS; ++index)
		m_streams[index] = KernelPerformanceStream();
	m_snapshot = KernelPerformanceSnapshot();
	return true;
}

KernelPerformanceBatch KernelPerformanceLedger::beginBatch(KernelPerformanceKernel kernel,
	unsigned subtype, unsigned frame, JobMetricCounter ordinal)
{
	KernelPerformanceBatch token;
	if (!writable()) return token;
	if (static_cast<unsigned>(kernel) >= KERNEL_PERFORMANCE_KERNEL_COUNT)
	{
		fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		return token;
	}
	unsigned stream = 0;
	for (; stream != m_streamCount; ++stream)
		if (m_streams[stream].kernel == kernel && m_streams[stream].subtype == subtype)
			break;
	if (stream != m_streamCount && (frame < m_identities[stream].frame ||
		(frame == m_identities[stream].frame && ordinal <= m_identities[stream].ordinal)))
	{
		fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		return token;
	}
	if (stream == KERNEL_PERFORMANCE_MAXIMUM_STREAMS ||
		m_openBatches == KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES)
	{
		fail(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		return token;
	}
	JobMetricCounter timestamp = 0;
	if (!now(timestamp) || !add(m_nextBatch, 1)) return token;
	unsigned slot = 0;
	for (; slot != KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES; ++slot)
		if (!m_batches[slot].active) break;
	if (slot == KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES)
	{
		fail(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		return token;
	}
	if (stream == m_streamCount)
	{
		m_streams[stream].kernel = kernel;
		m_streams[stream].subtype = subtype;
		++m_streamCount;
	}
	m_identities[stream].frame = frame;
	m_identities[stream].ordinal = ordinal;
	BatchState &batch = m_batches[slot];
	memset(&batch, 0, sizeof(batch));
	batch.active = true;
	batch.stream = stream;
	batch.frame = frame;
	batch.ordinal = ordinal;
	batch.serial = m_nextBatch;
	batch.start = timestamp;
	++m_openBatches;
	token.generation = m_generation;
	token.serial = batch.serial;
	token.slot = slot;
	return token;
}

bool KernelPerformanceLedger::describeBatch(KernelPerformanceBatch token,
	KernelPerformanceBatchIdentity &identity) const noexcept
{
	// Do not inspect owner-owned state, or mark an ownership violation, for a
	// foreign read-only query. This is an instrumentation gate, not an action.
	if (m_owner.load(std::memory_order_acquire) != GetCurrentThreadId()) return false;
	if (!m_started || !m_enabled || m_frozen || m_errors != 0 ||
		m_foreignCall.load(std::memory_order_acquire) || !token.valid() ||
		token.generation != m_generation || token.slot >= KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES)
		return false;
	const BatchState &batch = m_batches[token.slot];
	if (!batch.active || batch.serial != token.serial || batch.stream >= m_streamCount) return false;
	identity.kernel = m_streams[batch.stream].kernel;
	identity.subtype = m_streams[batch.stream].subtype;
	identity.frame = batch.frame;
	identity.ordinal = batch.ordinal;
	return true;
}

KernelPerformanceLedger::BatchState *KernelPerformanceLedger::resolve(KernelPerformanceBatch token)
{
	if (token.generation != m_generation || token.slot >= KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES ||
		!m_batches[token.slot].active || m_batches[token.slot].serial != token.serial)
	{
		fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		return 0;
	}
	return &m_batches[token.slot];
}

bool KernelPerformanceLedger::settle(JobMetricCounter timestamp)
{
	if (m_depth == 0) return true;
	IntervalState &interval = m_intervals[m_depth - 1];
	BatchState &batch = m_batches[interval.batch];
	if (!batch.active || timestamp < interval.start)
		return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (!add(batch.elapsed[interval.stage], timestamp - interval.start)) return false;
	interval.start = timestamp;
	return true;
}

KernelPerformanceInterval KernelPerformanceLedger::beginInterval(KernelPerformanceBatch token,
	KernelPerformanceStage stage)
{
	KernelPerformanceInterval result;
	if (!writable()) return result;
	BatchState *batch = resolve(token);
	if (batch == 0) return result;
	if (static_cast<unsigned>(stage) >= KERNEL_PERFORMANCE_STAGE_COUNT)
	{
		fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		return result;
	}
	if (m_depth == KERNEL_PERFORMANCE_MAXIMUM_INTERVAL_DEPTH)
	{
		fail(KERNEL_PERFORMANCE_ERROR_CAPACITY);
		return result;
	}
	JobMetricCounter timestamp = 0;
	if (!now(timestamp) || !settle(timestamp) || !add(m_nextInterval, 1) ||
		!add(batch->samples[stage], 1)) return result;
	batch->stageMask |= 1u << stage;
	IntervalState &interval = m_intervals[m_depth++];
	interval.batch = token.slot;
	interval.stage = stage;
	interval.serial = m_nextInterval;
	interval.start = timestamp;
	result.generation = m_generation;
	result.serial = interval.serial;
	return result;
}

bool KernelPerformanceLedger::endInterval(KernelPerformanceInterval token)
{
	if (!writable()) return false;
	if (token.generation != m_generation || m_depth == 0 ||
		m_intervals[m_depth - 1].serial != token.serial)
		return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	JobMetricCounter timestamp = 0;
	if (!now(timestamp) || !settle(timestamp)) return false;
	--m_depth;
	if (m_depth != 0) m_intervals[m_depth - 1].start = timestamp;
	return true;
}

bool KernelPerformanceLedger::endBatch(KernelPerformanceBatch token,
	KernelPerformanceDisposition disposition)
{
	if (!writable()) return false;
	BatchState *batch = resolve(token);
	if (batch == 0) return false;
	if (static_cast<unsigned>(disposition) > KERNEL_PERFORMANCE_COMMITTED)
		return fail(KERNEL_PERFORMANCE_ERROR_STATE);
	for (unsigned depth = 0; depth != m_depth; ++depth)
		if (m_intervals[depth].batch == token.slot)
			return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	const unsigned allStages = (1u << KERNEL_PERFORMANCE_STAGE_COUNT) - 1;
	if (disposition == KERNEL_PERFORMANCE_COMMITTED && batch->stageMask != allStages)
		return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	JobMetricCounter timestamp = 0;
	if (!now(timestamp)) return false;
	const JobMetricCounter inclusive = timestamp - batch->start;
	KernelPerformanceStream updated = m_streams[batch->stream];
	JobMetricCounter active = 0;
	for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
	{
		if (!add(active, batch->elapsed[stage]) ||
			!add(updated.stageNanoseconds[stage], batch->elapsed[stage]) ||
			!add(updated.stageSamples[stage], batch->samples[stage])) return false;
	}
	if (active > inclusive) return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	if (!add(updated.activePipelineNanoseconds, active) ||
		!add(updated.inclusiveBatchNanoseconds, inclusive)) return false;
	if (updated.attemptedBatches == 0 || batch->frame < updated.firstFrame)
		updated.firstFrame = batch->frame;
	if (batch->frame > updated.lastFrame) updated.lastFrame = batch->frame;
	if (inclusive > updated.maximumBatchNanoseconds) updated.maximumBatchNanoseconds = inclusive;
	if (!add(updated.attemptedBatches, 1)) return false;
	if (disposition != KERNEL_PERFORMANCE_NOT_ADMITTED && !add(updated.admittedBatches, 1)) return false;
	if (disposition == KERNEL_PERFORMANCE_COMMITTED && !add(updated.committedBatches, 1)) return false;
	if (disposition == KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION && !add(updated.abortedBatches, 1)) return false;
	m_streams[batch->stream] = updated;
	batch->active = false;
	--m_openBatches;
	return true;
}

KernelPerformanceSnapshot KernelPerformanceLedger::freeze()
{
	if (!owner()) return KernelPerformanceSnapshot();
	if (m_frozen) return m_snapshot;
	if (!m_started) fail(KERNEL_PERFORMANCE_ERROR_STATE);
	if (m_foreignCall.load(std::memory_order_acquire)) fail(KERNEL_PERFORMANCE_ERROR_OWNER);
	if (m_openBatches != 0 || m_depth != 0) fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	m_frozen = true;
	m_snapshot.enabled = m_enabled;
	m_snapshot.frozen = true;
	m_snapshot.complete = m_started && m_enabled && m_errors == 0 && m_streamCount != 0;
	m_snapshot.errors = m_errors;
	m_snapshot.generation = m_generation;
	m_snapshot.streamCount = m_streamCount;
	for (unsigned index = 0; index != m_streamCount; ++index)
		m_snapshot.streams[index] = m_streams[index];
	return m_snapshot;
}

KernelPerformanceScope::KernelPerformanceScope(KernelPerformanceLedger *ledger,
	KernelPerformanceBatch batch, KernelPerformanceStage stage) : m_ledger(ledger)
{
	if (ledger != 0) m_interval = ledger->beginInterval(batch, stage);
}
KernelPerformanceScope::~KernelPerformanceScope()
{
	if (m_ledger != 0 && m_interval.valid()) m_ledger->endInterval(m_interval);
}

} }
