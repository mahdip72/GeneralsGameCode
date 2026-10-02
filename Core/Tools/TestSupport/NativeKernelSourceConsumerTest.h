// Test-only storage and owner-ledger setup. Native executors, not this helper,
// own decisions, dispatch plans, range bodies, fallback and publication.
#pragma once
#if !defined(_WIN64)
#error "Native source/consumer fixtures require x64"
#endif
#include "Lib/KernelPerformanceReference.h"
#include <atomic>
#include <cstring>
#include <vector>

namespace rts_test
{
struct NativeKernelClock
{
	std::atomic<rts::JobMetricCounter> now{100};
	static rts::JobMetricCounter read(void *context)
	{ return static_cast<NativeKernelClock *>(context)->now.load(); }
};

struct NativeKernelTrace
{
	std::vector<unsigned char> bytes;
	rts::performance::KernelPerformanceReferenceRunOptions options;
	rts::performance::KernelPerformanceReferenceSnapshot source;

	explicit NativeKernelTrace(unsigned fixture)
	{
		using namespace rts::performance;
		options.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
		options.trace.mode = KERNEL_TRACE_RECORD;
		options.trace.append = append;
		options.trace.context = this;
		options.trace.limits = {1048576, 100000, 100000, 10000, 500000};
		options.trace.residentAttemptCapacity = 15;
		options.trace.residentRangeCapacity = 340;
		KernelPerformanceDigest *bindings[] = {
			&options.trace.binding.nativeRunIdentity, &options.trace.binding.executable,
			&options.trace.binding.fixture, &options.trace.binding.sourcePolicy};
		for (unsigned index = 0; index != 4; ++index)
		{
			bindings[index]->valid = true;
			for (unsigned byte = 0; byte != 32; ++byte)
				bindings[index]->bytes[byte] = static_cast<unsigned char>(32 * index + byte);
		}
		options.trace.binding.fixture.bytes[31] = static_cast<unsigned char>(fixture);
	}
	static bool append(void *context, const unsigned char *input, unsigned count)
	{
		auto &value = *static_cast<NativeKernelTrace *>(context);
		if (count > 1048576 - value.bytes.size()) return false;
		value.bytes.insert(value.bytes.end(), input, input + count);
		return true;
	}
	static bool read(void *context, rts::JobMetricCounter offset,
		unsigned char *output, unsigned count)
	{
		const auto &value = *static_cast<const NativeKernelTrace *>(context);
		if (offset > value.bytes.size() || count > value.bytes.size() - offset) return false;
		std::memcpy(output, value.bytes.data() + static_cast<std::size_t>(offset), count);
		return true;
	}
	rts::performance::KernelPerformanceReferenceRunOptions consumerOptions()
	{
		using namespace rts::performance;
		auto result = options;
		result.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
		result.trace.mode = KERNEL_TRACE_CONSUME;
		result.trace.append = 0;
		result.trace.readAt = read;
		result.trace.sourceByteCount = source.trace.byteCount;
		result.trace.sourceTraceDigest = source.trace.digest;
		// An independently selected receipt identity for this core-only fixture.
		// Title/process/immutable-file derivation is covered by the runtime tests.
		result.trace.sourceReceiptDigest = options.trace.binding.nativeRunIdentity;
		result.trace.sourceReceiptDigest.bytes[0] = 0x80;
		return result;
	}
};

inline rts::performance::KernelPerformanceSchedulerBoundary NativeKernelSchedulerBoundary();

struct NativeKernelOwnerRun
{
	NativeKernelClock clock;
	rts::performance::KernelPerformanceReferenceLedger reference;
	rts::performance::KernelPerformanceLedger &timing;
	rts::performance::KernelPerformanceFrame frame;
	rts::performance::KernelPerformancePhase phase;
	rts::performance::KernelPerformanceSnapshot timingSnapshot;
	unsigned ownerFrame;
	bool baseline;
	bool timingStarted;

	NativeKernelOwnerRun() : timing(rts::performance::KernelPerformanceLedger::instance()),
		phase(rts::performance::KERNEL_PHASE_OWNER_INTAKE), ownerFrame(0), baseline(false), timingStarted(false) {}
	~NativeKernelOwnerRun()
	{
		// A failed close remains failed, but must not strand the shared timing
		// singleton and prevent the next independent source case from starting.
		// Only a run actually opened by this fixture may be frozen here.
		if (timingStarted) timingSnapshot = timing.freeze();
	}
	bool begin(NativeKernelTrace &trace, bool consume, unsigned actualFrame,
		rts::performance::KernelPerformancePhase actualPhase)
	{
		using namespace rts::performance;
		baseline = consume; ownerFrame = actualFrame; phase = actualPhase;
		auto options = consume ? trace.consumerOptions() : trace.options;
		options.clock = NativeKernelClock::read; options.clockContext = &clock;
		KernelPerformanceTimingRunOptions timingOptions;
		timingOptions.enabled = true;
		timingOptions.role = consume ? KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE : KERNEL_PERFORMANCE_PIPELINE;
		timingOptions.clock = NativeKernelClock::read; timingOptions.clockContext = &clock;
		if (!timing.beginRun(timingOptions)) return false;
		timingStarted = true;
		if (!reference.beginRun(options)) return false;
		if (!consume) return true;
		frame = timing.beginFrame(1, ownerFrame, NativeKernelSchedulerBoundary());
		if (!frame.valid()) return false;
		for (unsigned index = 0; index <= static_cast<unsigned>(phase); ++index)
		{
			const auto current = static_cast<KernelPerformancePhase>(index);
			++clock.now;
			if (!timing.beginPhase(frame, current)) return false;
			if (current != phase && !timing.endPhase(frame, current)) return false;
		}
		return true;
	}
	rts::performance::KernelPerformanceAttempt beginAttempt(unsigned kind, unsigned subtype)
	{
		const rts::performance::KernelPerformanceAttemptIdentity identity =
			{kind, subtype, 1, 0, phase, ownerFrame};
		return reference.beginAttempt(identity);
	}
	bool closeTiming(const rts::performance::KernelPerformanceSchedulerBoundary &actual)
	{
		using namespace rts::performance;
		if (baseline)
		{
			++clock.now;
			if (!timing.endPhase(frame, phase)) return false;
			for (unsigned index = static_cast<unsigned>(phase) + 1; index != KERNEL_PHASE_COUNT; ++index)
			{
				const auto current = static_cast<KernelPerformancePhase>(index);
				++clock.now;
				if (!timing.beginPhase(frame, current) || !timing.endPhase(frame, current)) return false;
			}
			++clock.now;
			if (!timing.endFrame(frame, ownerFrame, actual) || !timing.sealAdmissions() ||
				!timing.sealExecutionClosure(actual)) return false;
		}
		else if (!timing.sealAdmissions()) return false;
		timingSnapshot = timing.freeze();
		return timingSnapshot.complete;
	}
};

inline rts::performance::KernelPerformanceSchedulerBoundary NativeKernelSchedulerBoundary()
{
	const auto metrics = rts::JobSystem::instance().metrics();
	rts::performance::KernelPerformanceSchedulerBoundary result;
	result.submittedJobs = metrics.submittedJobCount;
	result.executedJobs = metrics.executedJobCount;
	result.ownerHelpJobs = metrics.ownerHelpCount;
	result.outstandingJobs = rts::JobSystem::instance().outstandingJobCount();
	result.pendingJobs = rts::JobSystem::instance().pendingOwnerCompletionCount();
	return result;
}
}
