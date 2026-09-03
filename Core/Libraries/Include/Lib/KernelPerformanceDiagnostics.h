/*
** Native, non-authoritative kernel timing evidence. Never use these values
** to select gameplay, schedule work, or validate deterministic output.
*/
#pragma once

#if !defined(_WIN64)
#error "KernelPerformanceDiagnostics is available only in native x64 builds"
#endif

#include "Lib/JobSystem.h"
#include <atomic>

namespace rts { namespace performance {

enum KernelPerformanceKernel
{
	KERNEL_PERFORMANCE_PHYSICS = 0,
	KERNEL_PERFORMANCE_STATUS,
	KERNEL_PERFORMANCE_COLLISION,
	KERNEL_PERFORMANCE_AI,
	KERNEL_PERFORMANCE_SPATIAL,
	KERNEL_PERFORMANCE_PATH,
	KERNEL_PERFORMANCE_KERNEL_COUNT
};

enum KernelPerformanceStage
{
	KERNEL_PERFORMANCE_CAPTURE = 0,
	KERNEL_PERFORMANCE_SCHEDULE,
	KERNEL_PERFORMANCE_WAIT,
	KERNEL_PERFORMANCE_VALIDATE,
	KERNEL_PERFORMANCE_COMMIT,
	KERNEL_PERFORMANCE_STAGE_COUNT
};

enum KernelPerformanceDisposition
{
	KERNEL_PERFORMANCE_NOT_ADMITTED = 0,
	KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION,
	KERNEL_PERFORMANCE_COMMITTED
};

enum
{
	KERNEL_PERFORMANCE_MAXIMUM_STREAMS = 16,
	KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES = 64,
	KERNEL_PERFORMANCE_MAXIMUM_INTERVAL_DEPTH = 64,
	KERNEL_PERFORMANCE_ERROR_OWNER = 1,
	KERNEL_PERFORMANCE_ERROR_STATE = 2,
	KERNEL_PERFORMANCE_ERROR_CAPACITY = 4,
	KERNEL_PERFORMANCE_ERROR_CLOCK = 8,
	KERNEL_PERFORMANCE_ERROR_OVERFLOW = 16,
	KERNEL_PERFORMANCE_ERROR_ORDER = 32,
	KERNEL_PERFORMANCE_ERROR_INCOMPLETE = 64,
	KERNEL_PERFORMANCE_ERROR_IDENTITY = 128
};

typedef JobMetricCounter (*KernelPerformanceClock)(void *context);

struct KernelPerformanceBatch
{
	KernelPerformanceBatch();
	bool valid() const;
	JobMetricCounter generation, serial;
	unsigned slot;
};

struct KernelPerformanceInterval
{
	KernelPerformanceInterval();
	bool valid() const;
	JobMetricCounter generation, serial;
};

struct KernelPerformanceBatchIdentity
{
	KernelPerformanceBatchIdentity();
	KernelPerformanceKernel kernel;
	unsigned subtype, frame;
	JobMetricCounter ordinal;
};

struct KernelPerformanceStream
{
	KernelPerformanceStream();
	KernelPerformanceKernel kernel;
	// Stable operation subtype supplied by the adapter, e.g. enemy/production
	// AI or ordinary/direct path. Ordinals increase within each subtype/frame.
	unsigned subtype;
	JobMetricCounter attemptedBatches, admittedBatches, committedBatches, abortedBatches;
	JobMetricCounter stageNanoseconds[KERNEL_PERFORMANCE_STAGE_COUNT];
	JobMetricCounter stageSamples[KERNEL_PERFORMANCE_STAGE_COUNT];
	// Active intervals are stack-exclusive across ALL streams. Inclusive batch
	// latency may contain unrelated owner work and must not be summed as CPU
	// work, used as a serial portion, or substituted for active pipeline cost.
	JobMetricCounter activePipelineNanoseconds;
	JobMetricCounter inclusiveBatchNanoseconds, maximumBatchNanoseconds;
	unsigned firstFrame, lastFrame;
};

struct KernelPerformanceSnapshot
{
	KernelPerformanceSnapshot();
	bool enabled, frozen, complete;
	unsigned errors, streamCount;
	JobMetricCounter generation;
	KernelPerformanceStream streams[KERNEL_PERFORMANCE_MAXIMUM_STREAMS];
};

// Fixed-storage, owner-thread-only ledger. No clock is read while disabled.
// Completeness proves accounting, not speedup or successful admission. A
// separate same-input serial oracle and phase serial coverage are required
// before any external acceptance result can be derived from these intervals.
class KernelPerformanceLedger
{
public:
	KernelPerformanceLedger();
	static KernelPerformanceLedger &instance();
	bool beginRun(bool enabled, KernelPerformanceClock clock = 0, void *context = 0);
	// Stop admitting new diagnostic batches at the accepted terminal boundary.
	// Existing tokens remain live until their real completion and final freeze.
	bool sealAdmissions();
	KernelPerformanceBatch beginBatch(KernelPerformanceKernel kernel,
		unsigned subtype, unsigned frame, JobMetricCounter ordinal);
	// Owner-only, non-mutating active-token query. Failure preserves identity;
	// no clocks, freeze, error poisoning, or gameplay decisions are involved.
	bool describeBatch(KernelPerformanceBatch batch,
		KernelPerformanceBatchIdentity &identity) const noexcept;
	KernelPerformanceInterval beginInterval(KernelPerformanceBatch batch,
		KernelPerformanceStage stage);
	bool endInterval(KernelPerformanceInterval interval);
	bool endBatch(KernelPerformanceBatch batch, KernelPerformanceDisposition disposition);
	// Idempotent. Open batches/scopes, malformed identities, ownership errors,
	// clock failure, overflow, and omitted committed stages all fail closed.
	KernelPerformanceSnapshot freeze();

private:
	struct BatchState
	{
		bool active;
		unsigned stream, frame, stageMask;
		JobMetricCounter serial, start, ordinal;
		JobMetricCounter elapsed[KERNEL_PERFORMANCE_STAGE_COUNT];
		JobMetricCounter samples[KERNEL_PERFORMANCE_STAGE_COUNT];
	};
	struct IntervalState
	{
		unsigned batch, stage;
		JobMetricCounter serial, start;
	};
	struct StreamIdentity
	{
		unsigned frame;
		JobMetricCounter ordinal;
	};
	bool owner();
	bool writable();
	bool fail(unsigned error);
	bool now(JobMetricCounter &value);
	bool add(JobMetricCounter &total, JobMetricCounter amount);
	bool settle(JobMetricCounter value);
	BatchState *resolve(KernelPerformanceBatch batch);
	std::atomic<unsigned long> m_owner;
	std::atomic<bool> m_foreignCall;
	bool m_started, m_enabled, m_frozen, m_admissionsSealed;
	unsigned m_errors, m_openBatches, m_depth, m_streamCount;
	JobMetricCounter m_generation, m_nextBatch, m_nextInterval, m_lastClock;
	KernelPerformanceClock m_clock;
	void *m_clockContext;
	BatchState m_batches[KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES];
	IntervalState m_intervals[KERNEL_PERFORMANCE_MAXIMUM_INTERVAL_DEPTH];
	StreamIdentity m_identities[KERNEL_PERFORMANCE_MAXIMUM_STREAMS];
	KernelPerformanceStream m_streams[KERNEL_PERFORMANCE_MAXIMUM_STREAMS];
	KernelPerformanceSnapshot m_snapshot;
	KernelPerformanceLedger(const KernelPerformanceLedger &);
	KernelPerformanceLedger &operator=(const KernelPerformanceLedger &);
};

class KernelPerformanceScope
{
public:
	KernelPerformanceScope(KernelPerformanceLedger *ledger,
		KernelPerformanceBatch batch, KernelPerformanceStage stage);
	~KernelPerformanceScope();
private:
	KernelPerformanceLedger *m_ledger;
	KernelPerformanceInterval m_interval;
	KernelPerformanceScope(const KernelPerformanceScope &);
	KernelPerformanceScope &operator=(const KernelPerformanceScope &);
};

} }
