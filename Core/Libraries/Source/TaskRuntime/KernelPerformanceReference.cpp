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

struct KernelPerformanceCanonicalWriter::State
{
	State() : algorithm(0), hash(0), failed(false), finished(false) {}
	~State()
	{
		if (hash != 0) BCryptDestroyHash(hash);
		if (algorithm != 0) BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	bool append(const unsigned char *bytes, unsigned count)
	{
		if (failed || finished || hash == 0) return false;
		if (BCryptHashData(hash, const_cast<PUCHAR>(bytes), count, 0) != 0) failed = true;
		return !failed;
	}
	BCRYPT_ALG_HANDLE algorithm;
	BCRYPT_HASH_HANDLE hash;
	bool failed, finished;
	KernelPerformanceDigest digest;
};

KernelPerformanceCanonicalWriter::KernelPerformanceCanonicalWriter() : m_state(0) {}
KernelPerformanceCanonicalWriter::~KernelPerformanceCanonicalWriter() { delete m_state; }
bool KernelPerformanceCanonicalWriter::begin(unsigned fieldSchema)
{
	if (m_state != 0 && !m_state->finished) { m_state->failed = true; return false; }
	delete m_state;
	m_state = new (std::nothrow) State;
	if (m_state == 0) return false;
	if (fieldSchema == 0 || BCryptOpenAlgorithmProvider(&m_state->algorithm,
		BCRYPT_SHA256_ALGORITHM, 0, 0) != 0 || BCryptCreateHash(m_state->algorithm,
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
	if (m_state == 0 || m_state->failed) return KernelPerformanceDigest();
	if (!m_state->finished)
	{
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
bool appendDigest(KernelPerformanceCanonicalWriter &writer, const KernelPerformanceDigest &digest)
{
	if (!digest.valid || !writer.sequence(13, 4)) return false;
	for (unsigned index = 0; index != 4; ++index)
	{
		JobMetricCounter value = 0;
		for (unsigned byte = 0; byte != 8; ++byte)
			value |= static_cast<JobMetricCounter>(digest.bytes[index * 8 + byte]) << (byte * 8);
		if (!writer.u64(14 + index, value)) return false;
	}
	return true;
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
	State() : streamCount(0), openCount(0), nextSerial(0), lastClock(0), busy(false), clock(0), context(0) {}
	bool now(JobMetricCounter &value)
	{
		value = clock != 0 ? clock(context) : 0;
		if (value == 0 || value == ~static_cast<JobMetricCounter>(0) || value < lastClock) return false;
		lastClock = value;
		return true;
	}
	unsigned streamCount, openCount;
	JobMetricCounter nextSerial, lastClock;
	bool busy;
	KernelPerformanceClock clock;
	void *context;
	Stream streams[KERNEL_PERFORMANCE_MAXIMUM_STREAMS];
	Pending pending[KERNEL_PERFORMANCE_MAXIMUM_OPEN_BATCHES];
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
bool KernelPerformanceReferenceLedger::beginRun(KernelPerformanceReferenceMode mode,
	KernelPerformanceClock clock, void *clockContext) noexcept
{
	if (!owner()) return false;
	if (m_snapshot.generation != 0 && !m_snapshot.frozen)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_STATE; return false; }
	if (mode < KERNEL_REFERENCE_DISABLED || mode > KERNEL_REFERENCE_SERIAL_ORACLE)
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_IDENTITY; return false; }
	if (m_snapshot.generation == ~static_cast<JobMetricCounter>(0))
	{ m_snapshot.errors |= KERNEL_PERFORMANCE_ERROR_OVERFLOW; return false; }
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
	m_snapshot.frozen = true;
	m_snapshot.complete = m_snapshot.generation != 0 && m_snapshot.mode != KERNEL_REFERENCE_DISABLED &&
		m_snapshot.errors == 0 && m_snapshot.streamCount != 0;
	return m_snapshot;
}
} }
