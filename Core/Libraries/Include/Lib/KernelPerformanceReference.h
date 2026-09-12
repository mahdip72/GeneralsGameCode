/* Native diagnostics only: canonical immutable batches and a separate serial
** oracle. Neither callbacks nor receipt failures may change authoritative play.
*/
#pragma once
#if !defined(_WIN64)
#error "KernelPerformanceReference is available only in native x64 builds"
#endif
#include "Lib/KernelPerformanceDiagnostics.h"

namespace rts { class PartitionCollisionBypassObserver; struct PartitionCollisionReferenceInput; }

namespace rts { namespace performance {

class KernelPerformanceReferenceLedger;

enum KernelPerformanceReferenceMode
{
	KERNEL_REFERENCE_DISABLED = 0,
	KERNEL_REFERENCE_THROUGHPUT_BINDING,
	KERNEL_REFERENCE_SERIAL_ORACLE,
	KERNEL_REFERENCE_PHASE_BASELINE_BINDING
};

enum
{
	KERNEL_REFERENCE_ERROR_HASH = 256,
	KERNEL_REFERENCE_ERROR_CALLBACK = 512,
	KERNEL_REFERENCE_ERROR_MISMATCH = 1024,
	KERNEL_REFERENCE_ERROR_TRACE_ENCODING = 2048,
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
// Borrowed immutable source transport: exact read or failure. The native
// runtime owns the source handle and selection of its matching receipt.
typedef bool (*KernelPerformanceTraceReadAt)(void *context, JobMetricCounter offset,
	unsigned char *bytes, unsigned count);

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
	friend class KernelPerformanceReferenceLedger;
	JobMetricCounter bytesWritten() const noexcept;
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
private:
	friend class KernelPerformanceReferenceLedger;
	const KernelPerformanceReferenceLedger *m_owner;
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

enum KernelPerformanceWindowBoundaryKind
{
	KERNEL_WINDOW_INVALID = 0,
	KERNEL_WINDOW_BEGIN = 3,
	KERNEL_WINDOW_PHASE_BEGIN,
	KERNEL_WINDOW_PHASE_END,
	KERNEL_WINDOW_WORLD_END,
	KERNEL_WINDOW_CONTROL_END,
	KERNEL_WINDOW_DEFERRED_START_DECLARED,
	KERNEL_WINDOW_DEFERRED_START_CONSUMED
};
struct KernelPerformanceWindowBoundary
{
	KernelPerformanceWindowBoundary();
	KernelPerformanceWindowBoundaryKind kind;
	JobMetricCounter sampleOrdinal;
	KernelPerformancePhase phase;
	unsigned ownerFrameAtEntry, authorityFrame, actualOwnerFrame;
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
	KernelPerformanceTraceReadAt readAt;
	JobMetricCounter sourceByteCount;
	KernelPerformanceDigest sourceTraceDigest, sourceReceiptDigest;
};

struct KernelPerformanceReferenceRunOptions
{
	KernelPerformanceReferenceRunOptions();
	KernelPerformanceReferenceMode mode;
	KernelPerformanceClock clock;
	void *clockContext;
	KernelPerformanceTraceOptions trace;
};

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

// Fixed native collision facts. Only the compiled title observer may submit
// these through the ledger's private bridge; POD values alone grant no proof.
struct NativeCollisionPolicyFacts
{
	bool authoritativeRequested, shadowRequested, multiplayerPolicyBlocked, schedulerReady;
	unsigned configuredWorkers;
};
struct NativeCollisionCountFacts
{
	unsigned admissionOwnerID, cellCount, occupantCount;
	bool countValid;
};
struct NativeCollisionReserveFacts
{
	unsigned requiredCells, requiredOccupants, beforeCellCapacity, beforeOccupantCapacity;
	unsigned afterCellCapacity, afterOccupantCapacity, reserveOutcome;
};
struct NativeCollisionCaptureFacts
{
	unsigned flatIndex;
	bool snapshotValid, ownerPresent;
	unsigned capturedOwnerID;
	bool participantIdentitiesValid, ownerIdentityUnchanged;
};
struct NativeCollisionClassFacts
{
	unsigned occupantCount, minimumInputs, samplerEncounterCount, samplerSampleCount;
	bool spreadEvaluated, usefulSpread;
};
class KernelPerformanceDeterministicBypassProof
{
public:
	KernelPerformanceDeterministicBypassProof();
private:
	friend class rts::PartitionCollisionBypassObserver;
	friend class KernelPerformanceReferenceLedger;
	KernelPerformanceDeterministicBypassProof(KernelPerformanceReferenceLedger &ledger,
		KernelPerformanceAttempt attempt, const NativeCollisionClassFacts &actualClass,
		const rts::PartitionCollisionReferenceInput &actualInput);
	const KernelPerformanceReferenceLedger *m_owner;
	KernelPerformanceAttempt m_attempt;
	NativeCollisionClassFacts m_class;
	const rts::PartitionCollisionReferenceInput *m_input;
};
class KernelPerformanceDeterministicBypass
{
public:
	KernelPerformanceDeterministicBypass();
	bool valid() const;
private:
	friend class KernelPerformanceReferenceLedger;
	const KernelPerformanceReferenceLedger *m_owner;
	JobMetricCounter m_generation, m_serial;
};

enum KernelPerformanceInlineAction
{
	KERNEL_INLINE_INVALID = 0,
	KERNEL_INLINE_SKIP_SOURCE_NEVER_ENTERED,
	KERNEL_INLINE_EXECUTE
};
enum KernelPerformanceInlineOwnerSerialKind
{
	KERNEL_INLINE_SERIAL_ORDINARY_PATH_MATERIALIZATION = 1
};
class KernelPerformanceInlineBody
{
public:
	KernelPerformanceInlineBody();
	bool valid() const;
private:
	friend class KernelPerformanceReferenceLedger;
	const KernelPerformanceReferenceLedger *m_owner;
	JobMetricCounter m_generation, m_serial;
};
class KernelPerformanceInlineOwnerSerial
{
public:
	KernelPerformanceInlineOwnerSerial();
	bool valid() const;
private:
	friend class KernelPerformanceReferenceLedger;
	const KernelPerformanceReferenceLedger *m_owner;
	JobMetricCounter m_generation, m_serial;
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

enum KernelPerformanceRequestBudgetDisposition
{
	KERNEL_REQUEST_BUDGET_NOT_REACHED = 0,
	KERNEL_REQUEST_BUDGET_REFUSED,
	KERNEL_REQUEST_BUDGET_RETAINED,
	KERNEL_REQUEST_BUDGET_REFUNDED
};
struct KernelPerformanceRequestBudget
{
	KernelPerformanceRequestBudget();
	JobMetricCounter requestOrdinal;
	KernelPerformanceRequestBudgetDisposition disposition;
	unsigned grantSite;
	JobMetricCounter localGrantOrdinal, requestedBytes, grantedBytes;
	unsigned refundSite;
	JobMetricCounter localRefundOrdinal, refundedBytes, consumedBytes;
};

struct KernelPerformanceAttemptFinish
{
	KernelPerformanceDisposition disposition;
	unsigned reasonSchema, reason;
	bool fallbackEntered, fallbackCompleted;
	// Source finish metadata: true when the admitted body was fully validated,
	// even if the owner subsequently discarded it instead of committing.
	bool validationObserved;
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
	JobMetricCounter windowBoundaryCount, completedWindowCount, controlWindowCount;
	JobMetricCounter recordCount, logicalEventCount, coalescedSpanCount, coalescedAttemptCount;
	JobMetricCounter byteCount;
	KernelPerformanceDigest digest;
	KernelPerformanceDigest sourceReceiptDigest;
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
	// True only while the owner has an active trace-backed run.  Native
	// integrations use this to fail closed when an authenticated attempt is
	// required, while preserving the legacy untraced oracle path.
	bool traceRequested() const noexcept;
	bool beginRun(KernelPerformanceReferenceMode mode,
		KernelPerformanceClock clock = 0, void *clockContext = 0) noexcept;
	bool beginRun(const KernelPerformanceReferenceRunOptions &options) noexcept;
	KernelPerformanceAttempt beginAttempt(const KernelPerformanceAttemptIdentity &identity) noexcept;
	KernelPerformanceDeterministicBypass beginDeterministicBypass(KernelPerformanceAttempt attempt,
		const KernelPerformanceDeterministicBypassProof &proof) noexcept;
	bool observeBypassFallbackContact(KernelPerformanceDeterministicBypass token,
		unsigned actualOwnerID, unsigned actualParticipantID, bool actualInsertionResult) noexcept;
	bool finishDeterministicBypass(KernelPerformanceDeterministicBypass token,
		JobMetricCounter actualAttemptedContacts, JobMetricCounter actualInsertedContacts) noexcept;
	bool observeDecision(KernelPerformanceAttempt attempt,
		const KernelPerformanceAttemptDecision &decision) noexcept;
	bool replayDecision(KernelPerformanceAttempt attempt, unsigned site,
		bool actualDeterministicEligibility, const KernelPerformanceDigest &actualFacts,
		KernelPerformanceAttemptDecision &sourceDecision) noexcept;
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
	bool observeReleasedRequestBudget(KernelPerformanceAttempt attempt,
		const KernelPerformanceRangePlan &range, const KernelPerformanceRequestBudget &actual) noexcept;
	// Owner-only non-consuming lookahead cannot issue a local validated token.
	bool readSourceDispatch(KernelPerformanceAttempt attempt, JobMetricCounter dispatchOrdinal,
		KernelPerformanceDispatchPlan &plan) const noexcept;
	bool readSourceRange(KernelPerformanceAttempt attempt, JobMetricCounter dispatchOrdinal,
		unsigned rangeOrdinal, KernelPerformanceRangePlan &plan) const noexcept;
	bool readSourceFinish(KernelPerformanceAttempt attempt, KernelPerformanceAttemptFinish &finish) const noexcept;
	KernelPerformanceInlineAction beginInlineBody(KernelPerformanceAttempt attempt,
		const KernelPerformanceRangePlan &range, KernelPerformanceLedger &timing,
		KernelPerformanceInlineBody &body, KernelPerformanceCheckpointProbe &probe) noexcept;
	bool finishInlineBody(KernelPerformanceInlineBody body,
		const KernelPerformanceRangeProgress &actualProgress) noexcept;
	KernelPerformanceInlineOwnerSerial beginInlineOwnerSerial(KernelPerformanceInlineBody body,
		KernelPerformanceInlineOwnerSerialKind kind) noexcept;
	bool endInlineOwnerSerial(KernelPerformanceInlineOwnerSerial extent) noexcept;
	bool replayRequestBudgetGrant(KernelPerformanceInlineOwnerSerial extent,
		JobMetricCounter requestOrdinal, unsigned grantSite,
		JobMetricCounter localGrantOrdinal, JobMetricCounter actualRequestedBytes,
		bool &sourceGranted) noexcept;
	bool finishInlineRequestBudget(KernelPerformanceInlineOwnerSerial extent,
		const KernelPerformanceRequestBudget &actual) noexcept;
	KernelPerformanceReferenceBatch observeValidatedAttempt(KernelPerformanceAttempt attempt,
		KernelPerformanceCanonicalCallback writeOutput, const void *productionOutput,
		KernelPerformanceSerialCallback serialCompute = 0, const void *immutableInput = 0,
		void *detachedSerialOutput = 0) noexcept;
	bool finishAttempt(KernelPerformanceAttempt attempt,
		const KernelPerformanceAttemptFinish &finish) noexcept;
	// Records actual owner cleanup after all planned ranges were acknowledged.
	// Native integration separately establishes the real group-terminal boundary;
	// active-slot release alone does not establish that boundary.
	bool reapAttempt(KernelPerformanceAttempt attempt,
		const KernelPerformanceAttemptReap &reap) noexcept;
	// Preserve actual phase/window observations; borrowed authority is not the
	// mutable world frame, and a boundary does not release retained attempts.
	bool observeWindowBoundary(const KernelPerformanceWindowBoundary &boundary) noexcept;
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
	friend class rts::PartitionCollisionBypassObserver;
	bool armNativeCollisionBegin(KernelPerformanceAttempt attempt,
		KernelPerformanceLedger &actualTiming, const KernelPerformanceBatch &actualTimingBatch) noexcept;
	bool observeNativeCollisionPolicy(KernelPerformanceAttempt attempt, const NativeCollisionPolicyFacts &actual) noexcept;
	bool observeNativeCollisionCount(KernelPerformanceAttempt attempt, const NativeCollisionCountFacts &actual) noexcept;
	bool observeNativeCollisionReserve(KernelPerformanceAttempt attempt, const NativeCollisionReserveFacts &actual) noexcept;
	bool observeNativeCollisionCapture(KernelPerformanceAttempt attempt, const NativeCollisionCaptureFacts &actual) noexcept;
	bool observeNativeCollisionFullClass(KernelPerformanceAttempt attempt, const NativeCollisionClassFacts &actual) noexcept;
	bool beginNativeCollisionFullFallback(KernelPerformanceAttempt attempt, const NativeCollisionClassFacts &actualClass,
		const rts::PartitionCollisionReferenceInput &actualInput) noexcept;
	bool observeNativeCollisionFullFallbackContact(KernelPerformanceAttempt attempt,
		unsigned actualOwnerID, unsigned actualParticipantID, bool actualInsertionResult) noexcept;
	bool finishNativeCollisionFullFallback(KernelPerformanceAttempt attempt,
		JobMetricCounter actualAttemptedContacts, JobMetricCounter actualInsertedContacts) noexcept;
	bool observeNativeCollisionFacts(KernelPerformanceAttempt attempt, unsigned site, const void *actual) noexcept;
	bool nativeCollisionReady(KernelPerformanceAttempt attempt, unsigned stage, unsigned &slot) noexcept;
	bool beginCollisionFallback(KernelPerformanceAttempt attempt, const NativeCollisionClassFacts &actualClass,
		const rts::PartitionCollisionReferenceInput &actualInput, bool compact) noexcept;
	struct State;
	State *m_state;
	std::atomic<unsigned long> m_owner;
	std::atomic<bool> m_foreignCall;
	std::atomic<KernelPerformanceReferenceMode> m_runMode;
	bool owner() noexcept;
	bool failTrace(unsigned error) noexcept;
	bool traceReady() noexcept;
	unsigned traceAttemptSlot(KernelPerformanceAttempt attempt) noexcept;
	bool prepareTrace(const KernelPerformanceReferenceRunOptions &options) noexcept;
	bool validateSource() noexcept;
	bool bindCapturedDigest(KernelPerformanceAttempt attempt, unsigned schema,
		JobMetricCounter operations, const KernelPerformanceDigest &digest) noexcept;
	KernelPerformanceReferenceBatch linkValidatedDigest(KernelPerformanceAttempt attempt,
		const KernelPerformanceDigest &digest) noexcept;
	KernelPerformanceReferenceSnapshot m_snapshot;
	KernelPerformanceReferenceLedger(const KernelPerformanceReferenceLedger &);
	KernelPerformanceReferenceLedger &operator=(const KernelPerformanceReferenceLedger &);
};

} }
