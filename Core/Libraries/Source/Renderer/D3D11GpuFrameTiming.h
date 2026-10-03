#pragma once

// Private modern-Windows backend diagnostic. The recording-driver template
// lets the bounded collector itself be exercised without creating a GPU.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <fcntl.h>
#include <io.h>

namespace rts { namespace render { namespace detail {

enum GpuTimingRead { GpuTimingReady, GpuTimingNotReady, GpuTimingReadFailed };
enum GpuTimingStatus
{
	GpuTimingOpen, GpuTimingPending, GpuTimingComplete,
	GpuTimingUnpresented, GpuTimingPresentFailed, GpuTimingInvalid,
	GpuTimingDisjoint, GpuTimingReadinessFailed, GpuTimingResizeDropped,
	GpuTimingDeviceReleased, GpuTimingShutdownPending
};

struct GpuTimingFrameInfo
{
	GpuTimingFrameInfo() : width(0), height(0), samples(1), gamma(1.0f),
		brightness(0.0f), contrast(1.0f), gammaLimit(true), gammaApplied(false),
		swapInterval(0), presentFlags(0) {}
	unsigned int width, height, samples;
	float gamma, brightness, contrast;
	bool gammaLimit, gammaApplied;
	unsigned int swapInterval, presentFlags;
};

struct GpuTimingRecord
{
	uint64_t epoch, ordinal;
	uint32_t ownerBeginTickMs;
	GpuTimingStatus status;
	GpuTimingFrameInfo info;
	bool readback, presentCalled;
	long presentResult;
	double cpuPresentMs, totalMs, sceneMs, resolveMs, gammaMs;
};

struct GpuTimingDeviceInfo
{
	GpuTimingDeviceInfo() : epoch(0), vendorId(0), deviceId(0), luidLow(0), luidHigh(0),
		software(false), identityValid(false), featureLevel(0), debugLayer(false) {}
	uint64_t epoch;
	unsigned int vendorId, deviceId, luidLow, luidHigh;
	bool software, identityValid;
	unsigned int featureLevel;
	bool debugLayer;
};

struct GpuTimingCounters
{
	GpuTimingCounters() { memset(this, 0, sizeof(*this)); }
	uint64_t framesBegun, records, complete, skippedFull, skippedCap,
		skippedUnavailable, pending, cancelled, invalid, disjoint,
		allocationFailures, readinessFailures, notReady, budgetExhaustions,
		resizeDropped, deviceDropped, shutdownPending, readbacks, ioFailures,
		deviceMetadataFailures, deviceMetadataDropped;
};

template<class Driver> class GpuTimingCapture
{
public:
	enum { SlotCount = 8, StampCount = 4, PollBudget = 16, RecordCapacity = 8192, DeviceCapacity = 64 };
	typedef typename Driver::Query Query;
	GpuTimingCapture() : m_enabled(false), m_available(false), m_records(0),
		m_deviceCount(0), m_epoch(0), m_ordinal(0), m_current(-1), m_frameRecord(-1), m_presentRecord(-1), m_pollCursor(0) {}
	~GpuTimingCapture() { release(GpuTimingShutdownPending); delete[] m_records; }
	void enable() { m_enabled = true; }
	bool enabled() const { return m_enabled; }
	const GpuTimingCounters& counters() const { return m_counts; }
	const GpuTimingRecord& record(unsigned int index) const { return m_records[index]; }
	unsigned int recordCount() const { return static_cast<unsigned int>(m_counts.records); }
	unsigned int deviceCount() const { return m_deviceCount; }
	const GpuTimingDeviceInfo& deviceInfo(unsigned int index) const { return m_devices[index]; }
	void noteIoFailure() { ++m_counts.ioFailures; }

	void attach(const Driver& driver)
	{
		if (!m_enabled) return;
		release(GpuTimingDeviceReleased);
		m_driver = driver;
		++m_epoch;
		if (m_deviceCount < DeviceCapacity)
		{
			GpuTimingDeviceInfo& info = m_devices[m_deviceCount++];
			info = m_driver.deviceInfo(); info.epoch = m_epoch;
			if (!info.identityValid) ++m_counts.deviceMetadataFailures;
		}
		else ++m_counts.deviceMetadataDropped;
		m_ordinal = 0;
		m_frameRecord = -1;
		if (m_records == 0)
		{
			m_records = new (std::nothrow) GpuTimingRecord[RecordCapacity];
			if (m_records == 0) { ++m_counts.allocationFailures; return; }
		}
		allocateQueries();
	}

	void begin(const GpuTimingFrameInfo& info)
	{
		if (!m_enabled) return;
		cancel(GpuTimingUnpresented);
		poll();
		++m_counts.framesBegun;
		++m_ordinal;
		m_frameRecord = -1;
		m_presentRecord = -1;
		if (!m_available) { ++m_counts.skippedUnavailable; return; }
		if (m_counts.records == RecordCapacity) { ++m_counts.skippedCap; return; }
		unsigned int index = 0;
		while (index < SlotCount && m_slots[index].pending) ++index;
		if (index == SlotCount) { ++m_counts.skippedFull; return; }
		Slot& slot = m_slots[index];
		slot.record = static_cast<unsigned int>(m_counts.records++);
		slot.pending = true;
		slot.nextStamp = 0;
		slot.disjointReady = false;
		slot.hasAllStamps = false;
		slot.finalStatus = GpuTimingComplete;
		++m_counts.pending;
		m_current = static_cast<int>(index);
		m_frameRecord = static_cast<int>(slot.record);
		GpuTimingRecord& row = m_records[slot.record];
		row.epoch = m_epoch; row.ordinal = m_ordinal; row.info = info;
		row.ownerBeginTickMs = m_driver.ownerTick();
		row.status = GpuTimingOpen; row.readback = false; row.presentCalled = false;
		row.presentResult = 0;
		row.cpuPresentMs = row.totalMs = row.sceneMs = row.resolveMs = row.gammaMs = -1.0;
		m_driver.begin(slot.disjoint);
		stamp(0);
	}

	void beforeResolve(const GpuTimingFrameInfo& info)
	{
		if (m_current < 0) return;
		m_records[m_slots[m_current].record].info = info;
		stamp(1);
	}
	void afterResolve() { if (m_current >= 0) stamp(2); }
	void beforePresent()
	{
		m_presentRecord = -1;
		if (m_current < 0) return;
		Slot& slot = m_slots[m_current];
		stamp(3);
		slot.hasAllStamps = true;
		m_driver.end(slot.disjoint);
		m_records[slot.record].status = GpuTimingPending;
		m_presentRecord = static_cast<int>(slot.record);
		m_current = -1;
	}
	uint64_t cpuPresentStart()
	{
		if (!m_enabled || m_presentRecord < 0) return 0;
		m_records[m_presentRecord].presentCalled = true;
		return m_driver.clock();
	}
	void cpuPresentEnd(uint64_t start, long result)
	{
		if (!m_enabled || m_presentRecord < 0) return;
		GpuTimingRecord& row = m_records[m_presentRecord];
		row.presentResult = result;
		const uint64_t stop = m_driver.clock();
		const uint64_t frequency = m_driver.cpuFrequency();
		if (start != 0 && stop >= start && frequency != 0)
			row.cpuPresentMs = static_cast<double>(stop - start) * 1000.0 / frequency;
		if (result < 0) failPresentation(result);
	}
	void failPresentation(long result)
	{
		if (!m_enabled || m_presentRecord < 0) return;
		GpuTimingRecord& row = m_records[m_presentRecord];
		row.presentResult = result;
		for (unsigned int index = 0; index < SlotCount; ++index)
			if (m_slots[index].pending && m_slots[index].record == static_cast<unsigned int>(m_presentRecord))
				m_slots[index].finalStatus = GpuTimingPresentFailed;
		row.status = GpuTimingPresentFailed;
	}
	void markReadback()
	{
		if (!m_enabled) return;
		++m_counts.readbacks;
		if (m_current >= 0 && m_frameRecord >= 0) m_records[m_frameRecord].readback = true;
	}

	// endFrame is not a present boundary. A non-visible or suppressed frame is
	// detected at the next begin or lifecycle boundary without changing the API.
	void cancel(GpuTimingStatus reason)
	{
		if (m_current < 0) return;
		Slot& slot = m_slots[m_current];
		m_driver.end(slot.disjoint);
		slot.finalStatus = reason;
		m_records[slot.record].status = reason;
		++m_counts.cancelled;
		m_current = -1;
	}
	void resize()
	{
		if (!m_enabled) return;
		release(GpuTimingResizeDropped);
		if (m_records != 0) allocateQueries();
	}
	void release(GpuTimingStatus reason)
	{
		if (!m_enabled) return;
		if (m_current >= 0) cancel(reason);
		for (unsigned int index = 0; index < SlotCount; ++index)
		{
			Slot& slot = m_slots[index];
			if (slot.pending)
			{
				GpuTimingRecord& row = m_records[slot.record];
				if (row.status != GpuTimingPresentFailed) row.status = reason;
				if (reason == GpuTimingResizeDropped) ++m_counts.resizeDropped;
				else if (reason == GpuTimingShutdownPending) ++m_counts.shutdownPending;
				else if (reason == GpuTimingReadinessFailed) ++m_counts.invalid;
				else ++m_counts.deviceDropped;
				slot.pending = false;
				--m_counts.pending;
			}
			releaseQuery(slot.disjoint);
			for (unsigned int stampIndex = 0; stampIndex < StampCount; ++stampIndex)
				releaseQuery(slot.stamps[stampIndex]);
		}
		m_available = false;
		m_current = -1;
		m_frameRecord = -1;
		m_presentRecord = -1;
	}
	void reset()
	{
		release(GpuTimingShutdownPending);
		delete[] m_records; m_records = 0;
		m_counts = GpuTimingCounters(); m_driver = Driver();
		m_enabled = false; m_epoch = m_ordinal = 0; m_pollCursor = m_deviceCount = 0;
	}

	void poll()
	{
		if (!m_available) return;
		unsigned int budget = PollBudget;
		for (unsigned int visit = 0; visit < SlotCount && budget != 0; ++visit)
		{
			Slot& slot = m_slots[m_pollCursor];
			m_pollCursor = (m_pollCursor + 1) % SlotCount;
			if (!slot.pending || (m_current >= 0 && &slot == &m_slots[m_current])) continue;
			if (!slot.disjointReady)
			{
				bool disjoint = false;
				--budget;
				const GpuTimingRead result = m_driver.readDisjoint(slot.disjoint, slot.frequency, disjoint);
				if (!acceptRead(result)) { if (!m_available) return; continue; }
				slot.disjointReady = true;
				if (disjoint) { finish(slot, GpuTimingDisjoint); continue; }
				if (slot.frequency == 0) { finish(slot, GpuTimingInvalid); continue; }
				if (!slot.hasAllStamps)
				{ finish(slot, slot.finalStatus); continue; }
			}
			while (slot.nextStamp < StampCount && budget != 0)
			{
				--budget;
				const GpuTimingRead result = m_driver.readTimestamp(slot.stamps[slot.nextStamp], slot.values[slot.nextStamp]);
				if (!acceptRead(result)) { if (!m_available) return; break; }
				++slot.nextStamp;
			}
			if (slot.nextStamp != StampCount) continue;
			if (slot.values[0] > slot.values[1] || slot.values[1] > slot.values[2] || slot.values[2] > slot.values[3])
			{ finish(slot, GpuTimingInvalid); continue; }
			GpuTimingRecord& row = m_records[slot.record];
			const double scale = 1000.0 / slot.frequency;
			row.totalMs = static_cast<double>(slot.values[3] - slot.values[0]) * scale;
			row.sceneMs = static_cast<double>(slot.values[1] - slot.values[0]) * scale;
			row.resolveMs = static_cast<double>(slot.values[2] - slot.values[1]) * scale;
			row.gammaMs = static_cast<double>(slot.values[3] - slot.values[2]) * scale;
			finish(slot, slot.finalStatus);
		}
		if (budget == 0 && m_counts.pending != 0) ++m_counts.budgetExhaustions;
	}

private:
	struct Slot
	{
		Slot() : disjoint(0), pending(false), disjointReady(false), hasAllStamps(false), record(0),
			nextStamp(0), frequency(0), finalStatus(GpuTimingComplete)
		{ memset(stamps, 0, sizeof(stamps)); memset(values, 0, sizeof(values)); }
		Query *disjoint, *stamps[StampCount];
		bool pending, disjointReady, hasAllStamps;
		unsigned int record, nextStamp;
		uint64_t frequency, values[StampCount];
		GpuTimingStatus finalStatus;
	};
	void stamp(unsigned int index) { m_driver.end(m_slots[m_current].stamps[index]); }
	void releaseQuery(Query*& query) { if (query != 0) { m_driver.release(query); query = 0; } }
	void allocateQueries()
	{
		for (unsigned int index = 0; index < SlotCount; ++index)
		{
			Slot& slot = m_slots[index];
			if (!m_driver.create(true, &slot.disjoint)) { allocationFailed(); return; }
			for (unsigned int stampIndex = 0; stampIndex < StampCount; ++stampIndex)
				if (!m_driver.create(false, &slot.stamps[stampIndex])) { allocationFailed(); return; }
		}
		m_available = true;
	}
	void allocationFailed() { ++m_counts.allocationFailures; release(GpuTimingDeviceReleased); }
	bool acceptRead(GpuTimingRead result)
	{
		if (result == GpuTimingReady) return true;
		if (result == GpuTimingNotReady) ++m_counts.notReady;
		else { ++m_counts.readinessFailures; release(GpuTimingReadinessFailed); }
		return false;
	}
	void finish(Slot& slot, GpuTimingStatus status)
	{
		GpuTimingRecord& row = m_records[slot.record];
		if (row.status != GpuTimingPresentFailed) row.status = status;
		if (status == GpuTimingComplete) ++m_counts.complete;
		else { ++m_counts.invalid; if (status == GpuTimingDisjoint) ++m_counts.disjoint; }
		slot.pending = false; --m_counts.pending;
	}
	GpuTimingCapture(const GpuTimingCapture&);
	GpuTimingCapture& operator=(const GpuTimingCapture&);
	bool m_enabled, m_available;
	GpuTimingRecord *m_records;
	GpuTimingCounters m_counts;
	Driver m_driver;
	Slot m_slots[SlotCount];
	GpuTimingDeviceInfo m_devices[DeviceCapacity];
	unsigned int m_deviceCount;
	uint64_t m_epoch, m_ordinal;
	int m_current, m_frameRecord, m_presentRecord;
	unsigned int m_pollCursor;
};

struct D3D11GpuTimingDriver
{
	typedef ID3D11Query Query;
	D3D11GpuTimingDriver(ID3D11Device *device = 0, ID3D11DeviceContext *context = 0) : device(device), context(context) {}
	bool create(bool disjoint, Query **query)
	{
		D3D11_QUERY_DESC descriptor = {};
		descriptor.Query = disjoint ? D3D11_QUERY_TIMESTAMP_DISJOINT : D3D11_QUERY_TIMESTAMP;
		return device != 0 && SUCCEEDED(device->CreateQuery(&descriptor, query)) && *query != 0;
	}
	void release(Query *query) { query->Release(); }
	void begin(Query *query) { context->Begin(query); }
	void end(Query *query) { context->End(query); }
	static GpuTimingRead readResult(HRESULT result)
	{ return result == S_OK ? GpuTimingReady : (result == S_FALSE ? GpuTimingNotReady : GpuTimingReadFailed); }
	GpuTimingRead readDisjoint(Query *query, uint64_t& frequency, bool& disjoint)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data = {};
		const HRESULT result = context->GetData(query, &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		frequency = data.Frequency; disjoint = data.Disjoint != FALSE;
		return readResult(result);
	}
	GpuTimingRead readTimestamp(Query *query, uint64_t& value)
	{ return readResult(context->GetData(query, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH)); }
	uint64_t clock()
	{ LARGE_INTEGER value; return QueryPerformanceCounter(&value) ? static_cast<uint64_t>(value.QuadPart) : 0; }
	uint64_t cpuFrequency()
	{ LARGE_INTEGER value; return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? static_cast<uint64_t>(value.QuadPart) : 0; }
	uint32_t ownerTick() { return GetTickCount(); }
	GpuTimingDeviceInfo deviceInfo()
	{
		GpuTimingDeviceInfo info;
		if (device == 0) return info;
		info.featureLevel = static_cast<unsigned int>(device->GetFeatureLevel());
		info.debugLayer = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG) != 0;
		IDXGIDevice *dxgiDevice = 0;
		IDXGIAdapter *adapter = 0;
		IDXGIAdapter1 *adapter1 = 0;
		if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgiDevice))) && dxgiDevice != 0)
		{
			if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter != 0 &&
				SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&adapter1))) && adapter1 != 0)
			{
				DXGI_ADAPTER_DESC1 descriptor = {};
				if (SUCCEEDED(adapter1->GetDesc1(&descriptor)))
				{
					info.vendorId = descriptor.VendorId; info.deviceId = descriptor.DeviceId;
					info.luidLow = descriptor.AdapterLuid.LowPart;
					info.luidHigh = static_cast<unsigned int>(descriptor.AdapterLuid.HighPart);
					info.software = (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
					info.identityValid = true;
				}
			}
		}
		if (adapter1 != 0) adapter1->Release();
		if (adapter != 0) adapter->Release();
		if (dxgiDevice != 0) dxgiDevice->Release();
		return info;
	}
	ID3D11Device *device;
	ID3D11DeviceContext *context;
};

inline const char *GpuTimingStatusName(GpuTimingStatus status)
{
	switch (status)
	{
	case GpuTimingOpen: return "open";
	case GpuTimingPending: return "pending";
	case GpuTimingComplete: return "complete";
	case GpuTimingUnpresented: return "unpresented";
	case GpuTimingPresentFailed: return "present_failed";
	case GpuTimingInvalid: return "invalid";
	case GpuTimingDisjoint: return "disjoint";
	case GpuTimingReadinessFailed: return "readiness_failed";
	case GpuTimingResizeDropped: return "resize_dropped";
	case GpuTimingDeviceReleased: return "device_released";
	case GpuTimingShutdownPending: return "shutdown_pending";
	default: return "unknown";
	}
}

enum { GpuTimingCsvColumns = 26 };
inline const char *GpuTimingCsvHeader()
{
	return "row_type,device_epoch,local_frame_ordinal,status,width,height,samples,gamma,brightness,contrast,gamma_limit,gamma_applied,resolve_applied,swap_interval,present_flags,present_called,present_hresult,cpu_present_ms,readback_contaminated,gpu_total_ms,gpu_scene_ms,gpu_resolve_ms,gpu_gamma_ms,gpu_elapsed_not_busy,owner_begin_tick_ms,count\n";
}

// All operations are invoked only by normal shutdown. A failed staging file
// remains .pending; MoveFileW never replaces an existing completed export.
class D3D11GpuTimingOutput
{
public:
	explicit D3D11GpuTimingOutput(const wchar_t *directory) : m_directory(directory), m_file(0), m_locale(0)
	{ m_path[0] = m_pending[0] = 0; }
	~D3D11GpuTimingOutput() { close(); }
	bool open()
	{
		LARGE_INTEGER timestamp = {};
		if (!QueryPerformanceCounter(&timestamp)) return false;
		if (_snwprintf_s(m_path, _countof(m_path), _TRUNCATE, L"%ls\\gpu-frame-timing-%lu-%lu-%llu.csv",
			m_directory, GetCurrentProcessId(), GetCurrentThreadId(), static_cast<unsigned long long>(timestamp.QuadPart)) < 0)
			return false;
		if (_snwprintf_s(m_pending, _countof(m_pending), _TRUNCATE, L"%ls.pending", m_path) < 0) return false;
		m_locale = _create_locale(LC_NUMERIC, "C");
		if (m_locale == 0) return false;
		HANDLE handle = CreateFileW(m_pending, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
		if (handle == INVALID_HANDLE_VALUE) return false;
		const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
		if (descriptor < 0) { CloseHandle(handle); return false; }
		m_file = _fdopen(descriptor, "wb");
		if (m_file == 0) { _close(descriptor); return false; }
		return true;
	}
	bool header() { return fprintf(m_file, "%s", GpuTimingCsvHeader()) >= 0; }
	bool frame(const GpuTimingRecord& row)
	{
		return _fprintf_l(m_file, "frame,%llu,%llu,%s,%u,%u,%u,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%ld,%.9f,%u,%.9f,%.9f,%.9f,%.9f,1,%u,\n", m_locale,
			static_cast<unsigned long long>(row.epoch), static_cast<unsigned long long>(row.ordinal), GpuTimingStatusName(row.status),
			row.info.width, row.info.height, row.info.samples, row.info.gamma, row.info.brightness, row.info.contrast,
			row.info.gammaLimit ? 1U : 0U, row.info.gammaApplied ? 1U : 0U, row.info.samples > 1 ? 1U : 0U,
			row.info.swapInterval, row.info.presentFlags, row.presentCalled ? 1U : 0U, row.presentResult, row.cpuPresentMs,
			row.readback ? 1U : 0U, row.totalMs, row.sceneMs, row.resolveMs, row.gammaMs,
			static_cast<unsigned int>(row.ownerBeginTickMs)) >= 0;
	}
	bool summary(const char *name, uint64_t count, uint64_t epoch = 0)
	{
		if (fprintf(m_file, "summary,%llu,0,%s", static_cast<unsigned long long>(epoch), name) < 0) return false;
		for (unsigned int column = 4; column < GpuTimingCsvColumns - 1; ++column)
			if (fputc(',', m_file) == EOF) return false;
		return fprintf(m_file, ",%llu\n", static_cast<unsigned long long>(count)) >= 0;
	}
	bool healthy() const { return m_file != 0 && ferror(m_file) == 0; }
	bool flush() { return fflush(m_file) == 0 && ferror(m_file) == 0; }
	bool close()
	{
		FILE *file = m_file; m_file = 0;
		const bool closed = file == 0 || fclose(file) == 0;
		if (m_locale != 0) { _free_locale(m_locale); m_locale = 0; }
		return closed;
	}
	bool publish() { return MoveFileW(m_pending, m_path) != FALSE; }
private:
	D3D11GpuTimingOutput(const D3D11GpuTimingOutput&);
	D3D11GpuTimingOutput& operator=(const D3D11GpuTimingOutput&);
	const wchar_t *m_directory;
	wchar_t m_path[MAX_PATH + 128], m_pending[MAX_PATH + 128];
	FILE *m_file;
	_locale_t m_locale;
};

// The recording-output fixture executes these production decisions. Staging
// is always closed once, including body/open failures; publication requires
// every write, stream, flush and close check to succeed.
template<class Driver, class Output> bool ExportGpuTimingCapture(GpuTimingCapture<Driver>& capture, Output& output)
{
	bool ok = output.open();
	if (ok) ok = output.header();
	for (unsigned int index = 0; ok && index < capture.recordCount(); ++index)
		ok = output.frame(capture.record(index));
	for (unsigned int index = 0; ok && index < capture.deviceCount(); ++index)
	{
		const GpuTimingDeviceInfo& info = capture.deviceInfo(index);
		ok = output.summary("adapter_identity_valid", info.identityValid ? 1U : 0U, info.epoch) &&
			output.summary("adapter_vendor_id", info.vendorId, info.epoch) &&
			output.summary("adapter_device_id", info.deviceId, info.epoch) &&
			output.summary("adapter_luid_low", info.luidLow, info.epoch) &&
			output.summary("adapter_luid_high", info.luidHigh, info.epoch) &&
			output.summary("adapter_software", info.software ? 1U : 0U, info.epoch) &&
			output.summary("device_feature_level", info.featureLevel, info.epoch) &&
			output.summary("device_debug_layer", info.debugLayer ? 1U : 0U, info.epoch);
	}
	const GpuTimingCounters& counts = capture.counters();
	if (ok)
	{
		ok = output.summary("frames_begun", counts.framesBegun) &&
			output.summary("records", counts.records) && output.summary("complete", counts.complete) &&
			output.summary("skipped_full", counts.skippedFull) && output.summary("skipped_cap", counts.skippedCap) &&
			output.summary("skipped_unavailable", counts.skippedUnavailable) && output.summary("pending", counts.pending) &&
			output.summary("cancelled", counts.cancelled) && output.summary("invalid", counts.invalid) &&
			output.summary("disjoint", counts.disjoint) && output.summary("allocation_failures", counts.allocationFailures) &&
			output.summary("readiness_failures", counts.readinessFailures) && output.summary("not_ready_reads", counts.notReady) &&
			output.summary("poll_budget_exhaustions", counts.budgetExhaustions) &&
			output.summary("resize_dropped", counts.resizeDropped) && output.summary("device_dropped", counts.deviceDropped) &&
			output.summary("shutdown_pending", counts.shutdownPending) && output.summary("readbacks", counts.readbacks) &&
			output.summary("device_metadata_failures", counts.deviceMetadataFailures) &&
			output.summary("device_metadata_dropped", counts.deviceMetadataDropped) &&
			output.summary("io_failures", counts.ioFailures) && output.summary("export_end", capture.recordCount());
	}
	if (ok) ok = output.healthy();
	if (ok) ok = output.flush();
	const bool closed = output.close();
	ok = ok && closed;
	if (ok) ok = output.publish();
	if (!ok) capture.noteIoFailure();
	return ok;
}

class D3D11GpuFrameTiming
{
public:
	D3D11GpuFrameTiming() : m_configured(false) { m_directory[0] = 0; }
	void attach(ID3D11Device *device, ID3D11DeviceContext *context)
	{
		if (!m_configured)
		{
			m_configured = true;
			const DWORD length = GetEnvironmentVariableW(L"RTS_GPU_FRAME_TIMING_DIR", m_directory, MAX_PATH);
			if (length == 0) return;
			if (length >= MAX_PATH) { m_directory[0] = 0; return; }
			const DWORD attributes = GetFileAttributesW(m_directory);
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{ m_directory[0] = 0; return; }
			m_capture.enable();
		}
		m_capture.attach(D3D11GpuTimingDriver(device, context));
	}
	void begin(const GpuTimingFrameInfo& info) { m_capture.begin(info); }
	bool enabled() const { return m_capture.enabled(); }
	void beforeResolve(const GpuTimingFrameInfo& info) { m_capture.beforeResolve(info); }
	void afterResolve() { m_capture.afterResolve(); }
	void beforePresent() { m_capture.beforePresent(); }
	uint64_t cpuPresentStart() { return m_capture.cpuPresentStart(); }
	void cpuPresentEnd(uint64_t start, HRESULT result) { m_capture.cpuPresentEnd(start, result); }
	void failPresentation(HRESULT result) { m_capture.failPresentation(result); }
	void markReadback() { m_capture.markReadback(); }
	void cancel(GpuTimingStatus reason) { m_capture.cancel(reason); }
	void resize() { m_capture.resize(); }
	void release(GpuTimingStatus reason) { m_capture.release(reason); }

	// No file is opened or written from a frame or a device recovery.
	void writeOnShutdown()
	{
		m_capture.release(GpuTimingShutdownPending);
		if (m_capture.enabled())
		{
			D3D11GpuTimingOutput output(m_directory);
			ExportGpuTimingCapture(m_capture, output);
		}
		m_capture.reset(); m_configured = false; m_directory[0] = 0;
	}
private:
	D3D11GpuFrameTiming(const D3D11GpuFrameTiming&);
	D3D11GpuFrameTiming& operator=(const D3D11GpuFrameTiming&);
	bool m_configured;
	wchar_t m_directory[MAX_PATH];
	GpuTimingCapture<D3D11GpuTimingDriver> m_capture;
};

} } }
