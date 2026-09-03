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
	KERNEL_REFERENCE_ERROR_MISMATCH = 1024
};

struct KernelPerformanceDigest
{
	KernelPerformanceDigest();
	bool valid;
	unsigned char bytes[32];
	bool equals(const KernelPerformanceDigest &other) const;
};

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
	KernelPerformanceReferenceSnapshot m_snapshot;
	KernelPerformanceReferenceLedger(const KernelPerformanceReferenceLedger &);
	KernelPerformanceReferenceLedger &operator=(const KernelPerformanceReferenceLedger &);
};

} }
