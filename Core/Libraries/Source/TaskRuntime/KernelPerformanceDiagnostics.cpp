#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <string.h>
#include <windows.h>

namespace rts { namespace performance {

KernelPerformanceTimingRunOptions::KernelPerformanceTimingRunOptions() : enabled(false),
	role(KERNEL_PERFORMANCE_PIPELINE), clock(0), clockContext(0) {}
KernelPerformanceFrame::KernelPerformanceFrame() : generation(0), serial(0),
	sampleOrdinal(0), ownerFrameAtEntry(0) {}
bool KernelPerformanceFrame::valid() const { return generation != 0 && serial != 0; }
KernelPerformanceSchedulerBoundary::KernelPerformanceSchedulerBoundary() : submittedJobs(0),
	executedJobs(0), ownerHelpJobs(0), outstandingJobs(0), pendingJobs(0) {}
KernelPerformancePhaseAccountingRow::KernelPerformancePhaseAccountingRow() : totalNanoseconds(0),
	serialNanoseconds(0), pureNanoseconds(0), samples(0), maximumNanoseconds(0) {}
KernelPerformancePhaseAccountingSnapshot::KernelPerformancePhaseAccountingSnapshot() : requested(false),
	frozen(false), complete(false), errors(0), completedFrameCount(0), firstCompletedFrame(0),
	lastCompletedFrame(0), frameNanoseconds(0), maximumFrameNanoseconds(0),
	unscopedSerialNanoseconds(0), completionSerialNanoseconds(0), completionSampleCount(0),
	schedulerClosureKnown(false) {}

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
	complete(false), errors(0), streamCount(0), generation(0), runRole(KERNEL_PERFORMANCE_PIPELINE) {}
KernelPerformanceLedger::PhaseState::PhaseState() : phaseOpen(false), closureSealed(false),
	boundaryKnown(false), nextPhase(0), nextFrame(0), lastSampleOrdinal(0), frameStart(0),
	phaseStart(0), lastClock(0), unscoped(0), completionStart(0), completionElapsed(0) {}
KernelPerformanceLedger::KernelPerformanceLedger() : m_owner(GetCurrentThreadId()),
	m_foreignCall(false), m_runRole(KERNEL_PERFORMANCE_PIPELINE),
	m_started(false), m_enabled(false), m_frozen(false),
	m_admissionsSealed(false), m_errors(0),
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

bool KernelPerformanceLedger::beginRun(const KernelPerformanceTimingRunOptions &options)
{
	if (!owner()) return false;
	if (m_started && !m_frozen) return fail(KERNEL_PERFORMANCE_ERROR_STATE);
	if (static_cast<unsigned>(options.role) > KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE)
		return fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (m_generation == ~static_cast<JobMetricCounter>(0))
		return fail(KERNEL_PERFORMANCE_ERROR_OVERFLOW);
	++m_generation;
	m_started = true;
	m_enabled = options.enabled;
	m_frozen = false;
	m_admissionsSealed = false;
	m_errors = m_openBatches = m_depth = m_streamCount = 0;
	m_nextBatch = m_nextInterval = m_lastClock = 0;
	m_clock = options.clock != 0 ? options.clock : LiveSimulationPhaseClockNowNanoseconds;
	m_clockContext = options.clockContext;
	m_foreignCall.store(false, std::memory_order_release);
	memset(m_batches, 0, sizeof(m_batches));
	memset(m_intervals, 0, sizeof(m_intervals));
	memset(m_identities, 0, sizeof(m_identities));
	for (unsigned index = 0; index != KERNEL_PERFORMANCE_MAXIMUM_STREAMS; ++index)
		m_streams[index] = KernelPerformanceStream();
	m_phase = PhaseState();
	m_phase.snapshot.requested = options.enabled && options.role == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	m_snapshot = KernelPerformanceSnapshot();
	m_runRole.store(options.role, std::memory_order_release);
	return true;
}
KernelPerformanceRunRole KernelPerformanceLedger::runRole() const noexcept
{
	return m_runRole.load(std::memory_order_acquire);
}
bool KernelPerformanceLedger::phaseWritable()
{
	return writable() && m_phase.snapshot.requested;
}
bool KernelPerformanceLedger::frameMatches(KernelPerformanceFrame frame)
{
	if (!m_phase.frame.valid() || frame.generation != m_phase.frame.generation ||
		frame.serial != m_phase.frame.serial || frame.sampleOrdinal != m_phase.frame.sampleOrdinal ||
		frame.ownerFrameAtEntry != m_phase.frame.ownerFrameAtEntry)
		return fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	return true;
}
bool KernelPerformanceLedger::checkSchedulerBoundary(const KernelPerformanceSchedulerBoundary &actual)
{
	// Preserve actual observations, even on failure. Historical nonzero counts
	// are valid; new work, resets, and unfinished work inside this extent are not.
	m_phase.snapshot.schedulerEnd = actual;
	if (!m_phase.boundaryKnown)
	{
		m_phase.snapshot.schedulerBegin = actual;
		m_phase.boundaryKnown = true;
	}
	const KernelPerformanceSchedulerBoundary &first = m_phase.snapshot.schedulerBegin;
	if (actual.outstandingJobs != 0 || actual.pendingJobs != 0 ||
		actual.submittedJobs != first.submittedJobs || actual.executedJobs != first.executedJobs ||
		actual.ownerHelpJobs != first.ownerHelpJobs)
		return fail(KERNEL_PERFORMANCE_ERROR_STATE);
	return true;
}
KernelPerformanceFrame KernelPerformanceLedger::beginFrame(JobMetricCounter sampleOrdinal,
	unsigned ownerFrameAtEntry, const KernelPerformanceSchedulerBoundary &actual)
{
	KernelPerformanceFrame token;
	if (!phaseWritable() || m_admissionsSealed) return token;
	if (m_phase.frame.valid() || m_phase.completion.valid() || m_phase.closureSealed || m_depth != 0)
	{
		fail(KERNEL_PERFORMANCE_ERROR_ORDER);
		return token;
	}
	if (m_phase.snapshot.completedFrameCount != 0 && sampleOrdinal <= m_phase.lastSampleOrdinal)
	{
		fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
		return token;
	}
	JobMetricCounter timestamp = 0;
	if (!checkSchedulerBoundary(actual) || !now(timestamp) || !add(m_phase.nextFrame, 1)) return token;
	token.generation = m_generation;
	token.serial = m_phase.nextFrame;
	token.sampleOrdinal = sampleOrdinal;
	token.ownerFrameAtEntry = ownerFrameAtEntry;
	m_phase.frame = token;
	m_phase.frameStart = m_phase.lastClock = timestamp;
	m_phase.unscoped = 0;
	m_phase.nextPhase = 0;
	m_phase.phaseOpen = false;
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
		m_phase.phases[index] = KernelPerformancePhaseAccountingRow();
	return token;
}
bool KernelPerformanceLedger::beginPhase(KernelPerformanceFrame frame, KernelPerformancePhase phase)
{
	if (!phaseWritable() || !frameMatches(frame)) return false;
	if (static_cast<unsigned>(phase) >= KERNEL_PHASE_COUNT)
		return fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (m_phase.phaseOpen || static_cast<unsigned>(phase) != m_phase.nextPhase || m_depth != 0)
		return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	JobMetricCounter timestamp = 0;
	if (!now(timestamp)) return false;
	m_phase.phaseStart = timestamp;
	m_phase.phaseOpen = true;
	return true;
}
bool KernelPerformanceLedger::endPhase(KernelPerformanceFrame frame, KernelPerformancePhase phase)
{
	if (!phaseWritable() || !frameMatches(frame)) return false;
	if (static_cast<unsigned>(phase) >= KERNEL_PHASE_COUNT)
		return fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	if (!m_phase.phaseOpen || static_cast<unsigned>(phase) != m_phase.nextPhase || m_depth != 0)
		return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	JobMetricCounter timestamp = 0;
	if (!now(timestamp)) return false;
	KernelPerformancePhaseAccountingRow &row = m_phase.phases[phase];
	const JobMetricCounter measured = timestamp - m_phase.phaseStart;
	JobMetricCounter partition = row.serialNanoseconds;
	if (!add(partition, row.pureNanoseconds)) return false;
	if (partition != measured) return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	row.totalNanoseconds = row.maximumNanoseconds = measured;
	row.samples = 1;
	m_phase.phaseOpen = false;
	++m_phase.nextPhase;
	return true;
}
bool KernelPerformanceLedger::endFrame(KernelPerformanceFrame frame, unsigned completedFrame,
	const KernelPerformanceSchedulerBoundary &actual)
{
	if (!phaseWritable() || !frameMatches(frame)) return false;
	if (m_phase.phaseOpen || m_phase.nextPhase != KERNEL_PHASE_COUNT || m_depth != 0)
		return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (m_phase.snapshot.completedFrameCount != 0 && completedFrame <= m_phase.snapshot.lastCompletedFrame)
		return fail(KERNEL_PERFORMANCE_ERROR_IDENTITY);
	JobMetricCounter timestamp = 0;
	if (!checkSchedulerBoundary(actual) || !now(timestamp)) return false;
	const JobMetricCounter measured = timestamp - m_phase.frameStart;
	JobMetricCounter partition = m_phase.unscoped;
	KernelPerformancePhaseAccountingSnapshot updated = m_phase.snapshot;
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &sample = m_phase.phases[index];
		KernelPerformancePhaseAccountingRow &total = updated.phases[index];
		if (!add(partition, sample.totalNanoseconds) ||
			!add(total.totalNanoseconds, sample.totalNanoseconds) ||
			!add(total.serialNanoseconds, sample.serialNanoseconds) ||
			!add(total.pureNanoseconds, sample.pureNanoseconds) || !add(total.samples, sample.samples)) return false;
		if (sample.maximumNanoseconds > total.maximumNanoseconds) total.maximumNanoseconds = sample.maximumNanoseconds;
	}
	if (partition != measured) return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (!add(updated.frameNanoseconds, measured) || !add(updated.unscopedSerialNanoseconds, m_phase.unscoped)) return false;
	if (updated.completedFrameCount == 0) updated.firstCompletedFrame = completedFrame;
	updated.lastCompletedFrame = completedFrame;
	if (measured > updated.maximumFrameNanoseconds) updated.maximumFrameNanoseconds = measured;
	if (!add(updated.completedFrameCount, 1)) return false;
	m_phase.snapshot = updated;
	m_phase.lastSampleOrdinal = frame.sampleOrdinal;
	m_phase.frame = KernelPerformanceFrame();
	return true;
}
KernelPerformanceInterval KernelPerformanceLedger::beginCompletionSerial()
{
	KernelPerformanceInterval token;
	if (!phaseWritable()) return token;
	if (!m_admissionsSealed || m_phase.snapshot.completedFrameCount == 0 || m_phase.frame.valid() ||
		m_phase.completion.valid() || m_phase.closureSealed || m_depth != 0)
	{
		fail(KERNEL_PERFORMANCE_ERROR_ORDER);
		return token;
	}
	JobMetricCounter timestamp = 0;
	if (!now(timestamp) || !add(m_nextInterval, 1)) return token;
	token.generation = m_generation;
	token.serial = m_nextInterval;
	m_phase.completion = token;
	m_phase.completionStart = m_phase.lastClock = timestamp;
	m_phase.completionElapsed = 0;
	return token;
}
bool KernelPerformanceLedger::endCompletionSerial(KernelPerformanceInterval token)
{
	if (!phaseWritable()) return false;
	if (!m_phase.completion.valid() || token.generation != m_phase.completion.generation ||
		token.serial != m_phase.completion.serial || m_depth != 0)
		return fail(KERNEL_PERFORMANCE_ERROR_ORDER);
	JobMetricCounter timestamp = 0;
	if (!now(timestamp)) return false;
	const JobMetricCounter measured = timestamp - m_phase.completionStart;
	if (measured != m_phase.completionElapsed) return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (!add(m_phase.snapshot.completionSerialNanoseconds, measured) ||
		!add(m_phase.snapshot.completionSampleCount, 1)) return false;
	m_phase.completion = KernelPerformanceInterval();
	return true;
}
bool KernelPerformanceLedger::sealExecutionClosure(const KernelPerformanceSchedulerBoundary &actual)
{
	if (!phaseWritable()) return false;
	if (!m_admissionsSealed || m_phase.snapshot.completedFrameCount == 0 || m_phase.frame.valid() ||
		m_phase.completion.valid() || m_depth != 0 || m_openBatches != 0)
		return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	if (!checkSchedulerBoundary(actual) || !checkPhaseTotals()) return false;
	m_phase.closureSealed = true;
	m_phase.snapshot.schedulerClosureKnown = true;
	return true;
}

bool KernelPerformanceLedger::settlePhaseAccounting(JobMetricCounter timestamp)
{
	if (!m_phase.snapshot.requested || (!m_phase.frame.valid() && !m_phase.completion.valid())) return true;
	if (timestamp < m_phase.lastClock) return fail(KERNEL_PERFORMANCE_ERROR_CLOCK);
	const JobMetricCounter elapsed = timestamp - m_phase.lastClock;
	m_phase.lastClock = timestamp;
	if (m_phase.completion.valid()) return add(m_phase.completionElapsed, elapsed);
	if (m_phase.phaseOpen) return add(m_phase.phases[m_phase.nextPhase].serialNanoseconds, elapsed);
	return add(m_phase.unscoped, elapsed);
}

bool KernelPerformanceLedger::checkPhaseTotals()
{
	const KernelPerformancePhaseAccountingSnapshot &a = m_phase.snapshot;
	JobMetricCounter framePartition = a.unscopedSerialNanoseconds;
	JobMetricCounter serial = a.unscopedSerialNanoseconds, pure = 0;
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &row = a.phases[index];
		JobMetricCounter phasePartition = row.serialNanoseconds;
		if (!add(phasePartition, row.pureNanoseconds) || !add(framePartition, row.totalNanoseconds) ||
			!add(serial, row.serialNanoseconds) || !add(pure, row.pureNanoseconds)) return false;
		if (phasePartition != row.totalNanoseconds || row.samples != a.completedFrameCount)
			return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	}
	if (framePartition != a.frameNanoseconds) return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	JobMetricCounter accounted = a.frameNanoseconds;
	if (!add(accounted, a.completionSerialNanoseconds) || !add(serial, a.completionSerialNanoseconds) ||
		!add(serial, pure)) return false;
	if (accounted != serial) return fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
	return true;
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
	// The independently measured phase/frame boundaries and every pipeline
	// timestamp settle one owner-time partition. Pipeline nesting cannot add or
	// subtract time from it; unscoped owner work is serial by default.
	return settlePhaseAccounting(value);
}

bool KernelPerformanceLedger::beginRun(bool enabled,
	KernelPerformanceClock clock, void *context)
{
	KernelPerformanceTimingRunOptions options;
	options.enabled = enabled;
	options.clock = clock;
	options.clockContext = context;
	return beginRun(options);
}

bool KernelPerformanceLedger::sealAdmissions()
{
	if (!writable()) return false;
	m_admissionsSealed = true;
	return true;
}

KernelPerformanceBatch KernelPerformanceLedger::beginBatch(KernelPerformanceKernel kernel,
	unsigned subtype, unsigned frame, JobMetricCounter ordinal)
{
	KernelPerformanceBatch token;
	if (!writable()) return token;
	if (m_admissionsSealed) return token;
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
	// Post-terminal adapters may still construct an empty scope after a new
	// batch was declined. Only that canonical empty token is harmless; retained
	// tokens keep their normal validation and can finish before final freeze.
	if (m_admissionsSealed && token.generation == 0 && token.serial == 0 &&
		token.slot == KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES &&
		static_cast<unsigned>(stage) < KERNEL_PERFORMANCE_STAGE_COUNT)
		return result;
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
	if (m_phase.snapshot.requested)
	{
		if (!m_admissionsSealed || !m_phase.closureSealed || !m_phase.boundaryKnown ||
			m_phase.snapshot.completedFrameCount == 0 || m_phase.frame.valid() ||
			m_phase.phaseOpen || m_phase.completion.valid()) fail(KERNEL_PERFORMANCE_ERROR_INCOMPLETE);
		if (m_errors == 0) checkPhaseTotals();
		m_phase.snapshot.errors = m_errors;
		m_phase.snapshot.complete = m_errors == 0;
		m_phase.snapshot.schedulerClosureKnown = m_phase.closureSealed && m_errors == 0;
	}
	m_phase.snapshot.frozen = true;
	m_frozen = true;
	m_snapshot.enabled = m_enabled;
	m_snapshot.frozen = true;
	m_snapshot.complete = m_started && m_enabled && m_errors == 0 && m_streamCount != 0;
	m_snapshot.errors = m_errors;
	m_snapshot.generation = m_generation;
	m_snapshot.streamCount = m_streamCount;
	m_snapshot.runRole = runRole();
	m_snapshot.phaseAccounting = m_phase.snapshot;
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
