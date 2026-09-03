/* Native diagnostics only: canonical immutable batches and a separate serial
** oracle. Neither callbacks nor receipt failures may change authoritative play.
*/
#pragma once
#if !defined(_WIN64)
#error "KernelPerformanceReference is available only in native x64 builds"
#endif
#include "Lib/KernelPerformanceDiagnostics.h"

namespace rts { namespace performance {

enum KernelPerformanceReferenceMode
{
	KERNEL_REFERENCE_DISABLED = 0,
	KERNEL_REFERENCE_THROUGHPUT_BINDING,
	KERNEL_REFERENCE_SERIAL_ORACLE
};

enum
{
	KERNEL_REFERENCE_ERROR_HASH = 256,
	KERNEL_REFERENCE_ERROR_CALLBACK = 512,
	KERNEL_REFERENCE_ERROR_MISMATCH = 1024,
	KERNEL_REFERENCE_ERROR_TRACE_IO = 4096,
	KERNEL_REFERENCE_ERROR_TRACE_BINDING = 8192,
	KERNEL_REFERENCE_ERROR_CHECKPOINT = 16384
};

struct KernelPerformanceCheckpoint
{
	unsigned site;
	JobMetricCounter first, second;
};

enum KernelPerformanceRangeTerminal
{
	KERNEL_RANGE_NEVER_ENTERED = 0,
	KERNEL_RANGE_COMPLETED,
	KERNEL_RANGE_CANCELLED,
	KERNEL_RANGE_FAILED
};

// Range-local body facts, copied by the owner only after the real job release.
// Publication disposition is separate owner-side state, not probe authority.
struct KernelPerformanceCheckpointProgress
{
	bool entered;
	unsigned errors;
	JobMetricCounter pollCount, firstTruePoll, completedWorkUnits;
	KernelPerformanceCheckpoint firstTrueCheckpoint, finalCheckpoint;
	KernelPerformanceRangeTerminal terminal;
};

// No clock, transport, allocation, scheduler, or pure-execution authority.
// One executing body owns mutation; this object is not internally synchronized.
class KernelPerformanceCheckpointProbe
{
public:
	KernelPerformanceCheckpointProbe();
	bool beginRecord() noexcept;
	bool beginReplay(const KernelPerformanceCheckpointProgress &source) noexcept;
	bool cancelled(const KernelPerformanceCheckpoint &at, bool actualCancel) noexcept;
	bool finish(const KernelPerformanceCheckpoint &at, JobMetricCounter completedWorkUnits,
		KernelPerformanceRangeTerminal terminal) noexcept;
	KernelPerformanceCheckpointProgress snapshot() const noexcept;
private:
	enum Mode { Disabled, Record, Replay };
	Mode m_mode;
	bool m_finished;
	KernelPerformanceCheckpointProgress m_progress, m_source;
	bool fail() noexcept;
};

struct KernelPerformanceDigest
{
	KernelPerformanceDigest();
	bool valid;
	unsigned char bytes[32];
	bool equals(const KernelPerformanceDigest &other) const;
};

// The transport must append this exact byte range or fail. A failed/partial
// append invalidates the stream; the writer never retries an accepted prefix.
// The context and bytes are borrowed. Callbacks must not reenter the writer.
typedef bool (*KernelPerformanceTraceAppend)(void *context,
	const unsigned char *bytes, unsigned count);

// No raw object-representation hashing. Each field encodes a type byte,
// little-endian tag, and canonical value. sequence() records collection count;
// callers then emit each element's scalar fields in deterministic order.
// f32 preserves IEEE-754 bits, including signed zero, without FP arithmetic.
class KernelPerformanceCanonicalWriter
{
public:
	KernelPerformanceCanonicalWriter();
	~KernelPerformanceCanonicalWriter();
	bool begin(unsigned fieldSchema);
	// Optional trace transport uses fixed 64-KiB buffering by default. The
	// legacy entry point and explicit unbuffered mode preserve canonical bytes.
	bool begin(unsigned fieldSchema, KernelPerformanceTraceAppend append,
		void *context, bool buffered = true) noexcept;
	// Drain at a measured owner boundary without sealing the canonical stream.
	// Empty/finalized flushes are inert; any transport/hash failure is sticky.
	bool flush() noexcept;
	bool u32(unsigned tag, unsigned value);
	bool i32(unsigned tag, int value);
	bool u64(unsigned tag, JobMetricCounter value);
	bool f32(unsigned tag, float value);
	bool boolean(unsigned tag, bool value);
	bool sequence(unsigned tag, unsigned count);
	KernelPerformanceDigest finish();
private:
	struct State;
	State *m_state;
	bool field(unsigned type, unsigned tag, JobMetricCounter value, unsigned width);
	KernelPerformanceCanonicalWriter(const KernelPerformanceCanonicalWriter &);
	KernelPerformanceCanonicalWriter &operator=(const KernelPerformanceCanonicalWriter &);
};

typedef bool (*KernelPerformanceCanonicalCallback)(KernelPerformanceCanonicalWriter &, const void *);
typedef bool (*KernelPerformanceSerialCallback)(const void *immutableInput, void *detachedOutput);

struct KernelPerformanceReferenceBatch
{
	KernelPerformanceReferenceBatch();
	bool valid() const;
	JobMetricCounter generation, serial;
	unsigned slot;
};

enum KernelPerformanceTraceMode
{
	KERNEL_TRACE_DISABLED = 0,
	KERNEL_TRACE_RECORD,
	KERNEL_TRACE_CONSUME
};

struct KernelPerformanceTraceLimits
{
	JobMetricCounter maximumBytes, maximumRecords, maximumLogicalEvents;
	JobMetricCounter maximumAttempts, maximumRanges;
};

struct KernelPerformanceTraceBinding
{
	KernelPerformanceDigest nativeRunIdentity, executable, fixture, sourcePolicy;
};

struct KernelPerformanceTraceOptions
{
	KernelPerformanceTraceOptions();
	KernelPerformanceTraceMode mode;
	KernelPerformanceTraceBinding binding;
	KernelPerformanceTraceLimits limits;
	// Derived by the native integration from its frozen worker/input policy,
	// independently from historical event-volume limits; core enforces bounds.
	JobMetricCounter residentAttemptCapacity, residentRangeCapacity;
	KernelPerformanceTraceAppend append;
	void *context;
};

struct KernelPerformanceReferenceRunOptions
{
	KernelPerformanceReferenceRunOptions();
	KernelPerformanceReferenceMode mode;
	KernelPerformanceClock clock;
	void *clockContext;
	KernelPerformanceTraceOptions trace;
};

class KernelPerformanceReferenceLedger;
class KernelPerformanceAttempt
{
public:
	KernelPerformanceAttempt();
	bool valid() const;
private:
	friend class KernelPerformanceReferenceLedger;
	const KernelPerformanceReferenceLedger *m_owner;
	JobMetricCounter m_generation, m_serial;
	unsigned m_slot;
};

struct KernelPerformanceAttemptIdentity
{
	unsigned workKind, subtype;
	JobMetricCounter sampleOrdinal, attemptOrdinal;
	KernelPerformancePhase phase;
	unsigned ownerFrame;
};

enum KernelPerformanceAdmission
{
	KERNEL_ADMISSION_NOT_REQUESTED = 0,
	KERNEL_ADMISSION_REFUSED,
	KERNEL_ADMISSION_ACCEPTED
};

struct KernelPerformanceAttemptDecision
{
	JobMetricCounter decisionOrdinal;
	unsigned site, reasonSchema, reason;
	bool deterministicEligible;
	KernelPerformanceDigest deterministicFacts;
	KernelPerformanceAdmission admission;
	unsigned sourceConfiguredWorkers, dynamicFactsKnownMask;
	JobMetricCounter pendingJobs, outstandingJobs, activeSlots;
};

struct KernelPerformanceDispatchPlan
{
	JobMetricCounter dispatchOrdinal;
	unsigned bodySchema, checkpointSchema, rangeCount;
	JobMetricCounter operationCount, sourceGrain, sourceLimit;
};

struct KernelPerformanceRangePlan
{
	JobMetricCounter dispatchOrdinal;
	unsigned rangeOrdinal, bodyKind;
	JobMetricCounter begin, end, operationCount;
};

enum KernelPerformancePublication
{
	KERNEL_PUBLICATION_NOT_APPLICABLE = 0,
	KERNEL_PUBLICATION_PUBLISHED,
	KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL,
	KERNEL_PUBLICATION_REJECTED
};

struct KernelPerformanceRangeProgress
{
	KernelPerformanceCheckpointProgress checkpoint;
	KernelPerformancePublication publication;
};

struct KernelPerformanceAttemptFinish
{
	KernelPerformanceDisposition disposition;
	unsigned reasonSchema, reason;
	bool fallbackEntered, fallbackCompleted;
	KernelPerformanceReferenceBatch validatedBatch;
};

struct KernelPerformanceAttemptReap
{
	unsigned reasonSchema, reason, dynamicFactsKnownMask;
	JobMetricCounter pendingJobs, outstandingJobs, activeSlots;
};

struct KernelPerformanceTraceSnapshot
{
	KernelPerformanceTraceSnapshot();
	KernelPerformanceTraceMode mode;
	bool requested, frozen, complete, observationSealed, executionSealed;
	unsigned errors;
	KernelPerformanceTraceBinding binding;
	KernelPerformanceTraceLimits limits;
	JobMetricCounter residentAttemptCapacity, residentRangeCapacity;
	JobMetricCounter residentAttemptCount, residentAttemptHighWater;
	JobMetricCounter attemptCount, admittedAttemptCount, notAdmittedAttemptCount;
	JobMetricCounter abortedAfterAdmissionAttemptCount, reapCount;
	JobMetricCounter capturedAttemptCount, capturedOperationCount, dispatchCount, rangeCount, releasedRangeCount;
	JobMetricCounter residentRangeCount, residentRangeHighWater;
	JobMetricCounter recordCount, logicalEventCount, coalescedSpanCount, coalescedAttemptCount;
	JobMetricCounter byteCount;
	KernelPerformanceDigest digest;
};

struct KernelPerformanceReferenceStream
{
	KernelPerformanceReferenceStream();
	KernelPerformanceKernel kernel;
	unsigned subtype, fieldSchema, firstFrame, lastFrame;
	JobMetricCounter validatedBatchCount, committedBatchCount, abortedBatchCount;
	JobMetricCounter validatedOperationCount, committedOperationCount;
	JobMetricCounter serialSampleCount, serialNanoseconds, maximumSerialNanoseconds;
	KernelPerformanceDigest inputDigest, outputDigest, commitDigest;
};

struct KernelPerformanceReferenceSnapshot
{
	KernelPerformanceReferenceSnapshot();
	KernelPerformanceReferenceMode mode;
	bool frozen, complete;
	unsigned errors, streamCount;
	JobMetricCounter generation;
	KernelPerformanceTraceSnapshot trace;
	KernelPerformanceReferenceStream streams[KERNEL_PERFORMANCE_MAXIMUM_STREAMS];
};

class KernelPerformanceReferenceLedger
{
public:
	KernelPerformanceReferenceLedger();
	~KernelPerformanceReferenceLedger();
	static KernelPerformanceReferenceLedger &instance();
	// Active diagnostic mode, not historical run metadata. Foreign threads,
	// disabled/unstarted/frozen/failed runs observe Disabled without mutating
	// the ledger or reading owner-owned state. Gate diagnostics allocations only.
	KernelPerformanceReferenceMode mode() const noexcept;
	// Latched identity across diagnostic failure/freeze; does not reactivate
	// collection or grant permission to dispatch work after an error.
	KernelPerformanceReferenceMode runMode() const noexcept;
	bool beginRun(KernelPerformanceReferenceMode mode,
		KernelPerformanceClock clock = 0, void *clockContext = 0) noexcept;
	bool beginRun(const KernelPerformanceReferenceRunOptions &options) noexcept;
	KernelPerformanceAttempt beginAttempt(const KernelPerformanceAttemptIdentity &identity) noexcept;
	bool observeDecision(KernelPerformanceAttempt attempt,
		const KernelPerformanceAttemptDecision &decision) noexcept;
	bool bindCapturedInput(KernelPerformanceAttempt attempt, unsigned fieldSchema,
		JobMetricCounter operationCount, KernelPerformanceCanonicalCallback writeInput,
		const void *immutableInput) noexcept;
	bool observeDispatch(KernelPerformanceAttempt attempt,
		const KernelPerformanceDispatchPlan &dispatch) noexcept;
	bool observeRangePlan(KernelPerformanceAttempt attempt,
		const KernelPerformanceRangePlan &range) noexcept;
	// Owner import after the native release/acquire, never a worker ledger call
	// or a request to release storage. Group-terminal reap remains separate.
	bool observeReleasedRange(KernelPerformanceAttempt attempt,
		const KernelPerformanceRangePlan &range, const KernelPerformanceRangeProgress &progress) noexcept;
	bool finishAttempt(KernelPerformanceAttempt attempt,
		const KernelPerformanceAttemptFinish &finish) noexcept;
	// Records actual owner cleanup after all planned ranges were acknowledged.
	// Native integration separately establishes the real group-terminal boundary;
	// active-slot release alone does not establish that boundary.
	bool reapAttempt(KernelPerformanceAttempt attempt,
		const KernelPerformanceAttemptReap &reap) noexcept;
	bool sealObservationWindow() noexcept;
	// Reference retained-attempt closure only, never a scheduler-idle claim.
	bool sealExecutionClosure() noexcept;
	// One call represents one already-validated batch, possibly containing
	// multiple operations. SerialOracle computes into DETACHED storage only;
	// ThroughputBinding never invokes serialCompute or reads its clock.
	KernelPerformanceReferenceBatch observeValidatedBatch(KernelPerformanceKernel kernel,
		unsigned subtype, unsigned frame, JobMetricCounter ordinal, unsigned fieldSchema,
		JobMetricCounter operationCount, KernelPerformanceCanonicalCallback writeInput,
		const void *immutableInput, KernelPerformanceCanonicalCallback writeOutput,
		const void *productionOutput, KernelPerformanceSerialCallback serialCompute = 0,
		void *detachedSerialOutput = 0) noexcept;
	// Call after the SINGLE authoritative commit (or its failure). The commit
	// identity digest retains frame/ordinal/count/disposition. Open tokens fail
	// freeze. Consumers match committed BATCH count to timing ledger batches;
	// operation counts are a distinct cardinality and never substituted.
	bool finishBatch(KernelPerformanceReferenceBatch batch, bool committed) noexcept;
	KernelPerformanceReferenceSnapshot freeze() noexcept;
private:
	struct State;
	State *m_state;
	std::atomic<unsigned long> m_owner;
	std::atomic<bool> m_foreignCall;
	std::atomic<KernelPerformanceReferenceMode> m_runMode;
	bool owner() noexcept;
	bool failTrace(unsigned error) noexcept;
	bool traceReady() noexcept;
	unsigned traceAttemptSlot(KernelPerformanceAttempt attempt) noexcept;
	KernelPerformanceReferenceSnapshot m_snapshot;
	KernelPerformanceReferenceLedger(const KernelPerformanceReferenceLedger &);
	KernelPerformanceReferenceLedger &operator=(const KernelPerformanceReferenceLedger &);
};

} }
