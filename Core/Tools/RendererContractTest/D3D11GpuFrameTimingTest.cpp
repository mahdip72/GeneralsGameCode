// Recording-driver proof of the production collector, not GPU timing or FPS.
#include "../../Libraries/Source/Renderer/D3D11GpuFrameTiming.h"
#include <vector>
#include <math.h>

// Replace only the standard nothrow array allocation in this test executable,
// so the real collector's record-allocation failure can be proved deterministically.
static bool failRecordArrayAllocation = false;
void *operator new[](size_t bytes, const std::nothrow_t&) noexcept
{
    if (failRecordArrayAllocation) return 0;
    try { return ::operator new[](bytes); } catch (...) { return 0; }
}
void operator delete[](void *memory, const std::nothrow_t&) noexcept
{ ::operator delete[](memory); }
namespace
{
using namespace rts::render::detail;
int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}
struct RecordingQuery
{
	RecordingQuery(bool disjoint) : disjoint(disjoint), released(false), begun(false), ended(false), value(0), ends(0) {}
	bool disjoint, released, begun, ended;
	uint64_t value;
	unsigned int ends;
};
struct RecordingState
{
	RecordingState() : creates(0), releases(0), begins(0), ends(0), reads(0), clocks(0), failCreate(0), tickCalls(0), ownerTickMs(0xfffffff0U),
		failRead(false), ready(true), disjoint(false),
		frequency(1000), timestamp(100), cpuTick(100), cpuStep(50), cpuFrequencyValue(1000), failClockCall(0), cpuFrequencies(0),
		unsafeUse(false), metadataCalls(0), metadataValid(true) {}
	~RecordingState() { for (size_t index = 0; index < queries.size(); ++index) delete queries[index]; }
	unsigned int creates, releases, begins, ends, reads, clocks, failCreate;
	unsigned int tickCalls;
	uint32_t ownerTickMs;
	bool failRead, ready, disjoint;
	uint64_t frequency, timestamp, cpuTick, cpuStep, cpuFrequencyValue;
	unsigned int failClockCall, cpuFrequencies;
	bool unsafeUse;
	unsigned int metadataCalls;
	bool metadataValid;
	std::vector<RecordingQuery *> queries;
};
struct RecordingDriver
{
	typedef RecordingQuery Query;
	RecordingDriver(RecordingState *state = 0) : state(state) {}
	bool create(bool disjoint, Query **query)
	{
		++state->creates;
		if (state->failCreate == state->creates) { *query = 0; return false; }
		*query = new Query(disjoint); state->queries.push_back(*query); return true;
	}
	void release(Query *query)
	{
		if (query->released) state->unsafeUse = true;
		query->released = true; ++state->releases;
	}
	void begin(Query *query)
	{
		if (query->released || !query->disjoint) state->unsafeUse = true;
		query->begun = true; query->ended = false; ++state->begins;
	}
	void end(Query *query)
	{
		if (query->released) state->unsafeUse = true;
		query->ended = true; query->value = state->timestamp;
		state->timestamp += 100; ++query->ends; ++state->ends;
	}
	GpuTimingRead readDisjoint(Query *query, uint64_t& frequency, bool& disjoint)
	{
		++state->reads;
		if (query->released || !query->ended || !query->disjoint) state->unsafeUse = true;
		frequency = state->frequency; disjoint = state->disjoint;
		return state->failRead ? GpuTimingReadFailed : (state->ready ? GpuTimingReady : GpuTimingNotReady);
	}
	GpuTimingRead readTimestamp(Query *query, uint64_t& value)
	{
		++state->reads;
		if (query->released || !query->ended || query->disjoint) state->unsafeUse = true;
		value = query->value;
		return state->failRead ? GpuTimingReadFailed : (state->ready ? GpuTimingReady : GpuTimingNotReady);
	}
	uint64_t clock() { ++state->clocks; state->cpuTick += state->cpuStep; return state->clocks == state->failClockCall ? 0 : state->cpuTick; }
	uint64_t cpuFrequency() { ++state->cpuFrequencies; return state->cpuFrequencyValue; }
	uint32_t ownerTick() { ++state->tickCalls; return state->ownerTickMs; }
	GpuTimingDeviceInfo deviceInfo()
	{
		++state->metadataCalls;
		GpuTimingDeviceInfo info;
		info.identityValid = state->metadataValid; info.vendorId = 4318; info.deviceId = 1234;
		info.luidLow = 5678; info.luidHigh = 0xfffffff0U;
		info.featureLevel = D3D_FEATURE_LEVEL_11_0; info.debugLayer = true;
		return info;
	}
	RecordingState *state;
	// Deliberately no Flush, fence, wait, or sleep capability.
};
typedef GpuTimingCapture<RecordingDriver> Capture;
GpuTimingFrameInfo Info()
{
	GpuTimingFrameInfo info;
	info.width = 3840; info.height = 2160; info.samples = 4;
	info.gamma = 1.1f; info.brightness = 0.1f; info.contrast = 1.2f;
	info.gammaApplied = true; info.swapInterval = 1;
	return info;
}
void Present(Capture& capture, long result = 0)
{
	capture.beforeResolve(Info()); capture.afterResolve(); capture.beforePresent();
	const uint64_t start = capture.cpuPresentStart(); capture.cpuPresentEnd(start, result);
}
int DisabledAndIntervals()
{
	RecordingState state;
	Capture capture;
	capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture);
	capture.markReadback(); capture.poll(); capture.resize(); capture.release(GpuTimingShutdownPending);
	int result = Check(state.creates == 0 && state.reads == 0 && state.ends == 0 && state.clocks == 0 && state.cpuFrequencies == 0 && state.tickCalls == 0 && state.metadataCalls == 0 && capture.recordCount() == 0,
		"disabled allocates no queries and calls no GPU or clocks");
	capture.enable(); capture.attach(RecordingDriver(&state));
	result |= Check(state.creates == Capture::SlotCount * (Capture::StampCount + 1), "fixed query pool allocated once");
	capture.begin(Info()); state.ownerTickMs = 32; capture.markReadback(); Present(capture); capture.poll();
	const GpuTimingRecord& row = capture.record(0);
	result |= Check(row.status == GpuTimingComplete && row.epoch == 1 && row.ordinal == 1 && row.info.samples == 4 &&
		row.info.width == 3840 && row.info.gammaApplied && row.readback && row.presentCalled,
		"complete row retains settings, epoch ordinal and readback contamination");
	result |= Check(row.ownerBeginTickMs == 0xfffffff0U && state.tickCalls == 1 &&
		static_cast<uint32_t>(state.ownerTickMs - row.ownerBeginTickMs) == 48U,
		"owner begin tick survives metadata refresh and supports modulo32 wrap arithmetic");
	result |= Check(fabs(row.totalMs - 300.0) < 0.001 && fabs(row.sceneMs - 100.0) < 0.001 &&
		fabs(row.resolveMs - 100.0) < 0.001 && fabs(row.gammaMs - 100.0) < 0.001 && fabs(row.cpuPresentMs - 50.0) < 0.001,
		"four timestamps partition GPU elapsed work with separate CPU Present");
	result |= Check(row.presentStartQpc == 150 && row.presentEndQpc == 200 && row.cpuQpcFrequency == 1000 &&
		state.clocks == 2 && state.cpuFrequencies == 1,
		"Present retains existing clock ticks and frequency without additional calls");
	const unsigned int clocks = state.clocks;
	capture.beforePresent(); capture.cpuPresentEnd(capture.cpuPresentStart(), -1);
	result |= Check(state.clocks == clocks && row.status == GpuTimingComplete,
		"Present without sampled begin cannot mutate the previous row");
	return result | Check(!state.unsafeUse, "queries read only when ended and live");
}
int PresentClockValidation()
{
	int result = 0;
	for (unsigned int invalid = 0; invalid < 5; ++invalid)
	{
		RecordingState state;
		if (invalid < 2) state.failClockCall = invalid + 1;
		if (invalid == 2) state.cpuFrequencyValue = 0;
		if (invalid == 4) state.cpuStep = 0;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
		capture.begin(Info());
		result |= Check(capture.record(0).presentStartQpc == 0 && capture.record(0).presentEndQpc == 0 &&
			capture.record(0).cpuQpcFrequency == 0, "unpresented record starts with missing clock fields");
		capture.beforeResolve(Info()); capture.afterResolve(); capture.beforePresent();
		const uint64_t start = capture.cpuPresentStart();
		if (invalid == 3) state.cpuTick = 1;
		capture.cpuPresentEnd(start, 0);
		const GpuTimingRecord& row = capture.record(0);
		result |= Check(row.presentCalled && row.presentStartQpc == start && row.presentEndQpc == (invalid == 1 ? 0 : state.cpuTick) &&
			row.cpuQpcFrequency == state.cpuFrequencyValue && state.clocks == 2 && state.cpuFrequencies == 1,
			"missing or backward clocks retain raw evidence without extra sampling");
		result |= Check(row.cpuPresentMs == (invalid == 4 ? 0.0 : -1.0),
			"valid equal ticks are zero duration; invalid clock evidence is not a fast sample");
	}
	RecordingState state;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	capture.begin(Info()); Present(capture); capture.poll();
	capture.begin(Info()); Present(capture); capture.poll();
	result |= Check(capture.record(1).presentStartQpc - capture.record(0).presentStartQpc == 100 &&
		capture.record(1).presentEndQpc - capture.record(0).presentEndQpc == 100 &&
		state.clocks == 4 && state.cpuFrequencies == 2,
		"successive retained Presents expose exact start and end cadence");
	return result;
}
int ReadbackBoundaryAssociation()
{
	RecordingState state; state.ready = false;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	capture.begin(Info()); capture.beforeResolve(Info()); capture.afterResolve(); capture.beforePresent();
	const unsigned int ends = state.ends, reads = state.reads, clocks = state.clocks;
	capture.markReadback();
	int result = Check(capture.counters().readbacks == 1 && !capture.record(0).readback &&
		capture.record(0).status == GpuTimingPending && state.ends == ends && state.reads == reads && state.clocks == clocks,
		"readback after GPU interval increments counter without contaminating pending row");
	const uint64_t start = capture.cpuPresentStart(); capture.cpuPresentEnd(start, 0);
	result |= Check(capture.record(0).presentCalled && capture.record(0).cpuPresentMs == 50.0,
		"readback boundary guard preserves CPU Present record linkage");
	state.ready = true; capture.poll();
	const double total = capture.record(0).totalMs;
	capture.markReadback();
	result |= Check(capture.counters().readbacks == 2 && !capture.record(0).readback &&
		capture.record(0).status == GpuTimingComplete && capture.record(0).totalMs == total,
		"readback after Present and collection cannot mutate completed row");
	state.ready = false; capture.begin(Info()); capture.cancel(GpuTimingUnpresented);
	capture.markReadback();
	result |= Check(capture.counters().readbacks == 3 && !capture.record(1).readback &&
		capture.record(1).status == GpuTimingUnpresented && capture.counters().pending == 1,
		"readback after cancel counts attempt without contaminating cancelled row");
	for (unsigned int index = 1; index < Capture::SlotCount; ++index)
	{
		capture.begin(Info()); Present(capture);
	}
	capture.begin(Info()); capture.markReadback();
	result |= Check(capture.counters().skippedFull == 1 && capture.counters().readbacks == 4 &&
		capture.recordCount() == Capture::SlotCount + 1 && capture.counters().pending == Capture::SlotCount,
		"readback after unsampled ring-full begin is counter-only");
	for (unsigned int index = 0; index < capture.recordCount(); ++index)
		result |= Check(!capture.record(index).readback, "outside-interval readbacks leave every retained row unmarked");
	return result | Check(!state.unsafeUse, "readback association cases preserve query lifetime and readiness");
}
int FullRingAndBudget()
{
	RecordingState state; state.ready = false;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	int result = 0;
	for (unsigned int index = 0; index < Capture::SlotCount; ++index)
	{
		const unsigned int reads = state.reads;
		capture.begin(Info()); Present(capture);
		result |= Check(state.reads - reads <= Capture::PollBudget, "begin observes fixed read budget");
	}
	const unsigned int creates = state.creates, ends = state.ends, clocks = state.clocks, frequencies = state.cpuFrequencies;
	capture.begin(Info()); Present(capture);
	result |= Check(capture.recordCount() == Capture::SlotCount && capture.counters().skippedFull == 1 &&
		capture.counters().pending == Capture::SlotCount && state.creates == creates && state.ends == ends,
		"full ring skips without allocation, query reuse or wait");
	result |= Check(state.tickCalls == Capture::SlotCount, "ring-full skips do not query owner tick");
	result |= Check(state.clocks == clocks && state.cpuFrequencies == frequencies &&
		capture.counters().framesBegun > capture.recordCount(),
		"unsampled ring-full Present has no clock row and cannot prove complete Present totals");
	for (unsigned int index = 0; index < state.queries.size(); ++index)
		result |= Check(state.queries[index]->ends == 1, "not-ready queries never reissued");
	state.ready = true;
	const unsigned int reads = state.reads;
	capture.poll();
	result |= Check(state.reads - reads == Capture::PollBudget && capture.counters().budgetExhaustions == 1 &&
		capture.counters().pending != 0, "ready backlog stops at fixed polling budget");
	for (unsigned int index = 0; index < Capture::SlotCount; ++index) capture.poll();
	return result | Check(capture.counters().complete == Capture::SlotCount && capture.counters().pending == 0 &&
		capture.counters().notReady != 0 && !state.unsafeUse, "partial reads resume and ready slots collected");
}
int CancelAndLifecycle()
{
	RecordingState state; state.ready = false;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	capture.begin(Info()); capture.begin(Info());
	int result = Check(capture.record(0).status == GpuTimingUnpresented && capture.counters().cancelled == 1 &&
		capture.counters().pending == 2 && state.queries[0]->ends == 1,
		"ordinary cancel closes disjoint query but retains pending slot");
	state.ready = true; capture.poll();
	result |= Check(capture.record(0).status == GpuTimingUnpresented && capture.record(0).totalMs == -1.0 &&
		capture.counters().pending == 1 && !state.unsafeUse, "cancelled slot requires only disjoint readiness");
	const unsigned int oldCreates = state.creates;
	capture.resize();
	result |= Check(capture.record(1).status == GpuTimingResizeDropped && capture.counters().resizeDropped == 1 &&
		state.releases == oldCreates && state.creates == oldCreates * 2, "resize replaces pending queries with fresh pool");
	capture.begin(Info()); Present(capture); capture.attach(RecordingDriver(&state));
	result |= Check(capture.record(2).status == GpuTimingDeviceReleased && capture.counters().deviceDropped == 1,
		"recreation drops pending old-device results");
	capture.begin(Info()); Present(capture); capture.poll();
	result |= Check(capture.record(3).epoch == 2 && capture.record(3).ordinal == 1 && capture.record(3).status == GpuTimingComplete,
		"epoch increments and ordinal restarts with earlier rows retained");
	state.ready = false; capture.begin(Info()); Present(capture);
	const unsigned int reads = state.reads;
	capture.release(GpuTimingShutdownPending);
	result |= Check(capture.record(4).status == GpuTimingShutdownPending && capture.counters().shutdownPending == 1 &&
		capture.counters().pending == 0 && state.reads == reads && !state.unsafeUse,
		"shutdown loss needs no poll or GPU drain");
	capture.reset();
	return result | Check(!capture.enabled() && capture.recordCount() == 0 && capture.counters().framesBegun == 0,
		"reset starts independent collector lifecycle");
}
int InvalidAndFailure()
{
	int result = 0;
	{
		RecordingState state; state.failCreate = 7;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state)); capture.begin(Info());
		result |= Check(capture.counters().allocationFailures == 1 && capture.counters().skippedUnavailable == 1 &&
			state.releases == 6 && capture.recordCount() == 0, "partial allocation releases every query and disables diagnostic");
	}
	{
		RecordingState state;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture);
		state.failRead = true; capture.poll();
		const unsigned int reads = state.reads;
		capture.begin(Info()); capture.poll();
		result |= Check(capture.record(0).status == GpuTimingReadinessFailed && capture.counters().readinessFailures == 1 &&
			capture.counters().skippedUnavailable == 1 && state.reads == reads && state.releases == state.creates,
			"read failure drops rows and disables only diagnostic");
	}
	{
		RecordingState state;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture);
		state.disjoint = true; capture.poll();
		result |= Check(capture.record(0).status == GpuTimingDisjoint && capture.record(0).totalMs == -1.0 &&
			capture.counters().disjoint == 1 && state.reads == 1, "disjoint publishes no duration");
	}
	{
		RecordingState state;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture);
		state.frequency = 0; capture.poll();
		result |= Check(capture.record(0).status == GpuTimingInvalid && capture.record(0).totalMs == -1.0,
			"zero frequency cannot appear as fast valid sample");
	}
	{
		RecordingState state;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture, -1); capture.poll();
		result |= Check(capture.record(0).status == GpuTimingPresentFailed && capture.record(0).presentResult == -1 &&
			capture.counters().complete == 0, "failed Present never becomes complete frame");
	}
	{
		RecordingState state;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state)); capture.begin(Info());
		capture.beforeResolve(Info()); capture.cancel(GpuTimingPresentFailed); capture.poll();
		result |= Check(capture.record(0).status == GpuTimingPresentFailed && capture.record(0).totalMs == -1.0 &&
			!capture.record(0).presentCalled && state.reads == 1 && capture.counters().pending == 0 && !state.unsafeUse,
			"failed resolve never reads timestamps that were not issued");
	}
	return result;
}
int FailedPresentThenLoss()
{
	int result = 0;
	for (unsigned int loss = 0; loss < 4; ++loss)
	{
		RecordingState state; state.ready = false;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
		capture.begin(Info()); Present(capture, -1);
		if (loss == 0) capture.resize();
		else if (loss == 1) capture.attach(RecordingDriver(&state));
		else if (loss == 2) capture.release(GpuTimingShutdownPending);
		else { state.failRead = true; capture.poll(); }
		result |= Check(capture.record(0).status == GpuTimingPresentFailed && capture.record(0).presentResult == -1 &&
			capture.record(0).totalMs == -1.0 && capture.counters().complete == 0 && capture.counters().pending == 0,
			"known Present failure survives immediate unresolved-query loss");
		result |= Check(capture.record(0).presentStartQpc == 150 && capture.record(0).presentEndQpc == 200 &&
			capture.record(0).cpuQpcFrequency == 1000, "Present clock evidence survives unresolved-query loss");
		const GpuTimingCounters& counts = capture.counters();
		result |= Check((loss != 0 || counts.resizeDropped == 1) && (loss != 1 || counts.deviceDropped == 1) &&
			(loss != 2 || counts.shutdownPending == 1) && (loss != 3 || (counts.readinessFailures == 1 && counts.invalid == 1)),
			"actual query-loss reason remains separately accounted");
	}
	return result;
}
struct RecordingOutput
{
	enum Failure { None, Open, Header, Frame, Summary, Stream, Flush, Close, Publish };
	explicit RecordingOutput(Failure failure) : failure(failure), closeCalls(0), publishCalls(0), frameCalls(0),
		pending(false), finalFile(false), footer(false), rowStatus(GpuTimingOpen), rowTick(0), rowStart(0), rowEnd(0), rowFrequency(0),
		metadataRows(0), identityEpoch(0), identityValid(0) {}
	bool open() { pending = failure != Open; return failure != Open; }
	bool header() { return failure != Header; }
	bool frame(const GpuTimingRecord& row)
	{ ++frameCalls; rowStatus = row.status; rowTick = row.ownerBeginTickMs;
		rowStart = row.presentStartQpc; rowEnd = row.presentEndQpc; rowFrequency = row.cpuQpcFrequency; return failure != Frame; }
	bool summary(const char *name, uint64_t value, uint64_t epoch = 0)
	{
		if (failure == Summary) return false;
		if (epoch != 0) ++metadataRows;
		if (strcmp(name, "adapter_identity_valid") == 0) { identityEpoch = epoch; identityValid = value; }
		if (strcmp(name, "export_end") == 0) footer = true;
		return true;
	}
	bool healthy() { return failure != Stream; }
	bool flush() { return failure != Flush; }
	bool close() { ++closeCalls; return failure != Close; }
	bool publish()
	{
		++publishCalls;
		if (failure == Publish) return false;
		pending = false; finalFile = true; return true;
	}
	Failure failure;
	unsigned int closeCalls, publishCalls, frameCalls;
	bool pending, finalFile, footer;
	GpuTimingStatus rowStatus;
	uint32_t rowTick;
	uint64_t rowStart, rowEnd, rowFrequency;
	unsigned int metadataRows;
	uint64_t identityEpoch, identityValid;
};
int CheckedExport()
{
	int result = 0;
	const char *header = GpuTimingCsvHeader();
	unsigned int columns = 1;
	for (const char *cursor = header; *cursor != 0; ++cursor) if (*cursor == ',') ++columns;
	result |= Check(columns == 29 && columns == GpuTimingCsvColumns &&
		strstr(header, ",owner_begin_tick_ms,present_start_qpc,present_end_qpc,cpu_qpc_frequency,count\n") != 0,
		"production 29-column header appends precise Present clocks before final count");
	for (unsigned int failure = RecordingOutput::None; failure <= RecordingOutput::Publish; ++failure)
	{
		RecordingState state;
		Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
		capture.begin(Info()); capture.cancel(GpuTimingUnpresented); capture.poll();
		capture.release(GpuTimingShutdownPending);
		RecordingOutput output(static_cast<RecordingOutput::Failure>(failure));
		const bool exported = ExportGpuTimingCapture(capture, output);
		result |= Check(exported == (failure == RecordingOutput::None) && output.closeCalls == 1 &&
			capture.counters().ioFailures == (failure == RecordingOutput::None ? 0U : 1U),
			"production exporter checks every stage and always closes without rendering failure");
		result |= Check(output.publishCalls == (failure == RecordingOutput::None || failure == RecordingOutput::Publish ? 1U : 0U),
			"publication is attempted only after all writes, stream, flush and close succeed");
		if (failure == RecordingOutput::None)
			result |= Check(output.finalFile && !output.pending && output.footer && output.frameCalls == 1 &&
				output.rowStatus == GpuTimingUnpresented && output.rowTick == 0xfffffff0U &&
				output.rowStart == 0 && output.rowEnd == 0 && output.rowFrequency == 0 &&
				output.metadataRows == 8 && output.identityEpoch == 1 && output.identityValid == 1,
				"export retains cancelled sample status and original owner tick");
		else
			result |= Check(!output.finalFile && (failure == RecordingOutput::Open || output.pending),
				"failed export leaves no completed final file and retains pending staging");
		if (failure == RecordingOutput::Close || failure == RecordingOutput::Publish)
			result |= Check(output.footer && output.pending && !output.finalFile,
				"footer alone does not publish a close- or rename-failed pending export");
	}
	RecordingState state;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	capture.begin(Info()); Present(capture); capture.poll(); capture.release(GpuTimingShutdownPending);
	RecordingOutput output(RecordingOutput::None);
	result |= Check(ExportGpuTimingCapture(capture, output) && output.rowStart == 150 && output.rowEnd == 200 &&
		output.rowFrequency == 1000, "production exporter passes precise Present clock evidence unchanged");
	return result;
}
int DeviceIdentityBounds()
{
	RecordingState state;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	int result = Check(capture.deviceCount() == 1 && state.metadataCalls == 1 &&
		capture.deviceInfo(0).epoch == 1 && capture.deviceInfo(0).identityValid &&
		capture.deviceInfo(0).vendorId == 4318 && capture.deviceInfo(0).luidHigh == 0xfffffff0U &&
		capture.deviceInfo(0).debugLayer && capture.deviceInfo(0).featureLevel == D3D_FEATURE_LEVEL_11_0,
		"once-per-attach device identity retains scalar bits and actual epoch association");
	capture.begin(Info()); Present(capture); capture.poll(); capture.resize();
	result |= Check(state.metadataCalls == 1, "frame and resize do not recollect device identity");
	state.metadataValid = false;
	capture.attach(RecordingDriver(&state));
	result |= Check(capture.deviceInfo(1).epoch == 2 && !capture.deviceInfo(1).identityValid &&
		capture.counters().deviceMetadataFailures == 1, "missing identity remains explicitly invalid");
	for (unsigned int index = 2; index < Capture::DeviceCapacity + 1; ++index) capture.attach(RecordingDriver(&state));
	result |= Check(capture.deviceCount() == Capture::DeviceCapacity && state.metadataCalls == Capture::DeviceCapacity &&
		capture.counters().deviceMetadataDropped == 1, "bounded device metadata overflow is reported without overwrite");
	capture.reset();
	return result | Check(capture.deviceCount() == 0 && !state.unsafeUse, "lifecycle reset clears identity count safely");
}
int RecordCap()
{
	RecordingState state;
	Capture capture; capture.enable(); capture.attach(RecordingDriver(&state));
	for (unsigned int index = 0; index < Capture::RecordCapacity; ++index) { capture.begin(Info()); Present(capture); }
	const unsigned int creates = state.creates, clocks = state.clocks, frequencies = state.cpuFrequencies;
	capture.begin(Info()); Present(capture);
	const unsigned int ends = state.ends;
	capture.begin(Info()); Present(capture);
	return Check(capture.recordCount() == Capture::RecordCapacity && capture.counters().skippedCap == 2 &&
		capture.counters().framesBegun == Capture::RecordCapacity + 2 && capture.counters().complete == Capture::RecordCapacity &&
		state.creates == creates && state.ends == ends && state.clocks == clocks && state.cpuFrequencies == frequencies &&
		state.tickCalls == Capture::RecordCapacity && !state.unsafeUse,
		"record cap retains old evidence and accounts for every dropped frame");
}
int RecordCapacityConfiguration()
{
    int result = Check(Capture::SlotCount == 8 && Capture::PollBudget == 16 && Capture::RecordCapacity == 8192 &&
        Capture::MaximumRecordCapacity == 65536 && sizeof(GpuTimingRecord) * Capture::MaximumRecordCapacity <= 10U * 1024U * 1024U,
        "record budget remains bounded below 10MiB with unchanged query ring and polling budget");
    unsigned int capacity = 0;
    result |= Check(ParseGpuTimingRecordCapacity(0, &capacity) && capacity == 8192,
        "absent request uses historical default");
    const wchar_t *valid[] = {L"8192", L"9000", L"65536"};
    const unsigned int expected[] = {8192, 9000, 65536};
    for (unsigned int i = 0; i < 3; ++i)
        result |= Check(ParseGpuTimingRecordCapacity(valid[i], &capacity) && capacity == expected[i], "strict decimal bounded capacity accepted");
    const wchar_t *invalid[] = {L"", L"0", L"8191", L"65537", L"4294967296", L"9999999999999999999999999",
        L"+65536", L"-8192", L" 65536", L"65536 ", L"65536x", L"8.192", L"0x2000"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        capacity = 123;
        result |= Check(!ParseGpuTimingRecordCapacity(invalid[i], &capacity) && capacity == 123,
            "invalid requested capacity is rejected without a partial/default output");
    }
    result |= Check(!ParseGpuTimingRecordCapacity(L"8192", 0), "capacity parser rejects null output");
    const unsigned int rejected[] = {0, 8191, 65537, 0xffffffffU};
    for (unsigned int i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i)
    {
        RecordingState state; Capture capture;
        result |= Check(!capture.enable(rejected[i]) && !capture.enabled() && capture.recordCapacity() == 8192,
            "invalid collector request leaves capture disabled and default capacity unchanged");
        capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture); capture.poll();
        result |= Check(state.creates == 0 && state.metadataCalls == 0 && state.reads == 0 && capture.recordCount() == 0 &&
            capture.counters().allocationFailures == 0, "configuration rejection never fabricates allocation failure or GPU work");
    }
    RecordingState state; Capture capture;
    result |= Check(capture.enable(65536) && capture.recordCapacity() == 65536 && state.creates == 0,
        "expanded enable selects capacity before attachment without allocation");
    result |= Check(!capture.enable(8192) && capture.recordCapacity() == 65536,
        "enabled configuration cannot be changed even before first attachment");
    capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture); capture.poll();
    const unsigned int creates = state.creates, releases = state.releases;
    result |= Check(!capture.enable(9000) && capture.recordCapacity() == 65536 && capture.recordCount() == 1 &&
        state.creates == creates && state.releases == releases, "active configuration rejection performs no reallocation or query change");
    capture.attach(RecordingDriver(&state));
    result |= Check(capture.recordCapacity() == 65536 && capture.recordCount() == 1 && state.creates == creates * 2 &&
        state.releases == creates && capture.deviceCount() == 2, "reattach preserves record budget and old rows while replacing only query pool");
    capture.reset();
    result |= Check(!capture.enabled() && capture.recordCapacity() == 8192 && capture.recordCount() == 0 &&
        capture.deviceCount() == 0 && capture.counters().allocationFailures == 0, "reset restores independent historical default");
    result |= Check(capture.enable() && capture.recordCapacity() == 8192, "default enable remains source compatible after reset");
    capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture); capture.poll();
    return result | Check(capture.recordCount() == 1 && capture.record(0).epoch == 1 && !state.unsafeUse,
        "reset/default reattach starts fresh capture without stale query use");
}
int ExpandedRecordCapAndAllocationFailure()
{
    int result = 0;
    {
        RecordingState state; Capture capture;
        result |= Check(capture.enable(Capture::MaximumRecordCapacity), "maximum opt-in capacity accepted");
        capture.attach(RecordingDriver(&state));
        for (unsigned int index = 0; index < Capture::MaximumRecordCapacity; ++index)
        {
            const unsigned int reads = state.reads;
            capture.begin(Info()); Present(capture);
            result |= Check(state.reads - reads <= Capture::PollBudget, "expanded capture keeps bounded read budget per begin");
        }
        const unsigned int creates = state.creates, clocks = state.clocks;
        capture.begin(Info()); Present(capture); capture.poll();
        result |= Check(capture.recordCount() == Capture::MaximumRecordCapacity && capture.recordCount() > Capture::RecordCapacity &&
            capture.counters().complete == Capture::MaximumRecordCapacity && capture.counters().skippedCap == 1 &&
            capture.counters().framesBegun == Capture::MaximumRecordCapacity + 1 && capture.counters().pending == 0 &&
            capture.record(0).ordinal == 1 && capture.record(Capture::MaximumRecordCapacity - 1).ordinal == Capture::MaximumRecordCapacity &&
            state.creates == Capture::SlotCount * (Capture::StampCount + 1) && state.creates == creates && state.clocks == clocks &&
            state.tickCalls == Capture::MaximumRecordCapacity && !state.unsafeUse,
            "65536-row capture retains both endpoints and reports cap drops with no query growth or skipped Present clock");
    }
    {
        RecordingState state; Capture capture; capture.enable(65536);
        failRecordArrayAllocation = true;
        capture.attach(RecordingDriver(&state));
        failRecordArrayAllocation = false;
        capture.begin(Info()); Present(capture); capture.poll();
        result |= Check(capture.enabled() && capture.recordCapacity() == 65536 && capture.recordCount() == 0 &&
            capture.counters().allocationFailures == 1 && capture.counters().skippedUnavailable == 1 && state.creates == 0,
            "real record new[] failure remains explicit allocation failure with no query allocation or fabricated row");
        capture.attach(RecordingDriver(&state)); capture.begin(Info()); Present(capture); capture.poll();
        result |= Check(capture.recordCount() == 1 && capture.counters().complete == 1 && capture.counters().allocationFailures == 1 &&
            capture.recordCapacity() == 65536 && state.creates == Capture::SlotCount * (Capture::StampCount + 1) && !state.unsafeUse,
            "later attachment can allocate requested budget but cannot erase earlier allocation failure provenance");
    }
    return result;
}struct RecordingConfigurationOutput
{
    enum Failure { None, Open, Write, Flush, Close };
    explicit RecordingConfigurationOutput(Failure fail) : fail(fail), opens(0), writes(0), flushes(0), closes(0), pid(0), capacity(0), bytes(0) {}
    bool open(unsigned long value) { ++opens; pid = value; return fail != Open; }
    bool write(unsigned long value, unsigned int records, uint64_t recordBytes)
    { ++writes; pid = value; capacity = records; bytes = recordBytes; return fail != Write; }
    bool flush() { ++flushes; return fail != Flush; }
    bool close() { ++closes; return fail != Close; }
    Failure fail; unsigned int opens, writes, flushes, closes;
    unsigned long pid; unsigned int capacity; uint64_t bytes;
};
int CheckedConfigurationProvenance()
{
    int result = 0;
    Capture capture; capture.enable(65536);
    for (unsigned int failure = RecordingConfigurationOutput::None; failure <= RecordingConfigurationOutput::Close; ++failure)
    {
        RecordingConfigurationOutput output(static_cast<RecordingConfigurationOutput::Failure>(failure));
        const bool ok = WriteGpuTimingConfiguration(output, 4321UL, capture.recordCapacity(), sizeof(GpuTimingRecord));
        result |= Check(ok == (failure == RecordingConfigurationOutput::None) && output.opens == 1 && output.closes == 1 &&
            output.writes == (failure == RecordingConfigurationOutput::Open ? 0U : 1U) &&
            output.flushes == (failure == RecordingConfigurationOutput::Open || failure == RecordingConfigurationOutput::Write ? 0U : 1U),
            "once-activation provenance checks open/write/flush/close and cannot succeed on a failed stage");
        if (failure != RecordingConfigurationOutput::Open)
            result |= Check(output.pid == 4321UL && output.capacity == capture.recordCapacity() && output.bytes == sizeof(GpuTimingRecord),
                "sidecar receives actual collector budget, process identity and record ABI size");
    }
    return result;
}}
int main()
{
	int result = DisabledAndIntervals(); result |= PresentClockValidation(); result |= ReadbackBoundaryAssociation();
	result |= FullRingAndBudget(); result |= CancelAndLifecycle();
	result |= InvalidAndFailure(); result |= RecordCap();
	result |= RecordCapacityConfiguration(); result |= ExpandedRecordCapAndAllocationFailure(); result |= CheckedConfigurationProvenance();
	result |= FailedPresentThenLoss(); result |= CheckedExport(); result |= DeviceIdentityBounds();
	if (!result) puts("GPU timing collector contracts passed (recording driver, no GPU)");
	return result;
}
