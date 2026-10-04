#pragma once

// Private owner-only diagnostic. Native Present events do not depend on GPU
// query slots, readiness, or allocation. Recording is bounded and performs no I/O.
#include "D3D11GpuFrameTiming.h"
#include <wchar.h>

namespace rts { namespace render { namespace detail {

struct PresentTimingRecord
{
	uint64_t epoch, frame, ordinal, start, end, frequency;
	long result;
	unsigned int swapInterval, flags, width, height;
};
struct PresentTimingCounters
{
	PresentTimingCounters() { memset(this, 0, sizeof(*this)); }
	uint64_t calls, retained, dropped, succeeded, positive, failed, invalidClocks, ioFailures;
};
struct PresentTimingOwner
{
	PresentTimingOwner() : session(0), start(0), process(0), thread(0) {}
	uint64_t session, start;
	unsigned long process, thread;
};
struct PresentTimingClockDriver
{
	uint64_t clock()
	{ LARGE_INTEGER value; return QueryPerformanceCounter(&value) ? static_cast<uint64_t>(value.QuadPart) : 0; }
	uint64_t cpuFrequency()
	{ LARGE_INTEGER value; return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? static_cast<uint64_t>(value.QuadPart) : 0; }
};

template<class Driver> class PresentTimingCapture
{
public:
	enum { Capacity = 32768 };
	PresentTimingCapture() : m_records(0), m_epoch(0), m_frame(0) {}
	~PresentTimingCapture() { delete[] m_records; }
	bool enable(const Driver& driver, uint64_t session, unsigned long process, unsigned long thread)
	{
		if (enabled()) return false;
		m_records = new (std::nothrow) PresentTimingRecord[Capacity];
		if (m_records == 0) return false;
		m_driver = driver; m_owner.session = session; m_owner.process = process; m_owner.thread = thread;
		m_owner.start = m_driver.clock();
		return true;
	}
	bool enabled() const { return m_records != 0; }
	void attachDevice() { if (enabled()) { ++m_epoch; m_frame = 0; } }
	void beginFrame() { if (enabled()) ++m_frame; }
	uint64_t clock() { return enabled() ? m_driver.clock() : 0; }
	uint64_t cpuFrequency() { return enabled() ? m_driver.cpuFrequency() : 0; }
	void record(uint64_t start, uint64_t end, uint64_t frequency, long result,
		unsigned int swapInterval, unsigned int flags, unsigned int width, unsigned int height)
	{
		if (!enabled()) return;
		++m_counts.calls;
		if (result == 0) ++m_counts.succeeded;
		else if (result > 0) ++m_counts.positive;
		else ++m_counts.failed;
		if (start == 0 || end == 0 || end < start || frequency == 0) ++m_counts.invalidClocks;
		if (m_counts.retained == Capacity) { ++m_counts.dropped; return; }
		PresentTimingRecord& row = m_records[m_counts.retained++];
		row = { m_epoch, m_frame, m_counts.calls, start, end, frequency, result,
			swapInterval, flags, width, height };
	}
	const PresentTimingCounters& counters() const { return m_counts; }
	const PresentTimingOwner& owner() const { return m_owner; }
	const PresentTimingRecord& record(unsigned int index) const { return m_records[index]; }
	void noteIoFailure() { ++m_counts.ioFailures; }
	void reset()
	{
		delete[] m_records; m_records = 0; m_counts = PresentTimingCounters();
		m_owner = PresentTimingOwner(); m_epoch = m_frame = 0; m_driver = Driver();
	}
private:
	PresentTimingCapture(const PresentTimingCapture&);
	PresentTimingCapture& operator=(const PresentTimingCapture&);
	Driver m_driver;
	PresentTimingRecord *m_records;
	PresentTimingCounters m_counts;
	PresentTimingOwner m_owner;
	uint64_t m_epoch, m_frame;
};

// Shared production callsite clock path, also exercised with recording drivers.
// Off/off samples nothing; GPU-only retains its established call path; both
// enabled share exactly two clocks and one frequency for each actual Present.
template<class Gpu, class Present> uint64_t BeginPresentTiming(Gpu& gpu, Present& present)
{
	if (!present.enabled()) return gpu.cpuPresentStart();
	const uint64_t start = present.clock();
	gpu.cpuPresentStart(start);
	return start;
}
template<class Gpu, class Present> void EndPresentTiming(Gpu& gpu, Present& present,
	uint64_t start, long result, unsigned int swapInterval, unsigned int flags,
	unsigned int width, unsigned int height)
{
	if (!present.enabled()) { gpu.cpuPresentEnd(start, result); return; }
	const uint64_t stop = present.clock(), frequency = present.cpuFrequency();
	gpu.cpuPresentEnd(start, result, stop, frequency);
	present.record(start, stop, frequency, result, swapInterval, flags, width, height);
}

enum { PresentTimingCsvColumns = 18 };
inline const char *PresentTimingCsvHeader()
{
	return "row_type,owner_session,process_id,owner_thread_id,owner_start_qpc,device_epoch,backend_frame_ordinal,present_ordinal,present_start_qpc,present_end_qpc,cpu_qpc_frequency,present_hresult,swap_interval,present_flags,width,height,status,count\n";
}
inline const char *PresentTimingResultName(long result)
{
	if (result == 0) return "s_ok";
	if (result == DXGI_STATUS_OCCLUDED) return "occluded";
	return result > 0 ? "positive" : "failed";
}

class PresentTimingOutput
{
public:
	PresentTimingOutput(const wchar_t *directory, const PresentTimingOwner& owner) :
		m_directory(directory), m_owner(owner), m_file(0) { m_path[0] = m_pending[0] = 0; }
	~PresentTimingOutput() { close(); }
	bool open()
	{
		// Owner/session metadata supplies identity; CREATE_NEW resolves collisions.
		for (unsigned int attempt = 0; attempt < 16; ++attempt)
		{
			if (_snwprintf_s(m_path, _countof(m_path), _TRUNCATE,
				L"%ls\\present-frame-timing-%lu-%lu-%llu-%llu-%u.csv", m_directory,
				m_owner.process, m_owner.thread, static_cast<unsigned long long>(m_owner.session),
				static_cast<unsigned long long>(m_owner.start), attempt) < 0 ||
				_snwprintf_s(m_pending, _countof(m_pending), _TRUNCATE, L"%ls.pending", m_path) < 0) return false;
			if (GetFileAttributesW(m_path) != INVALID_FILE_ATTRIBUTES) continue;
			HANDLE handle = CreateFileW(m_pending, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
			if (handle == INVALID_HANDLE_VALUE)
			{
				if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
				return false;
			}
			const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
			if (descriptor < 0) { CloseHandle(handle); return false; }
			m_file = _fdopen(descriptor, "wb");
			if (m_file == 0) _close(descriptor);
			return m_file != 0;
		}
		return false;
	}
	bool header() { return fprintf(m_file, "%s", PresentTimingCsvHeader()) >= 0; }
	bool frame(const PresentTimingRecord& row)
	{
		return fprintf(m_file, "present,%llu,%lu,%lu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%ld,%u,%u,%u,%u,%s,\n",
			static_cast<unsigned long long>(m_owner.session), m_owner.process, m_owner.thread,
			static_cast<unsigned long long>(m_owner.start), static_cast<unsigned long long>(row.epoch),
			static_cast<unsigned long long>(row.frame), static_cast<unsigned long long>(row.ordinal),
			static_cast<unsigned long long>(row.start), static_cast<unsigned long long>(row.end),
			static_cast<unsigned long long>(row.frequency), row.result, row.swapInterval, row.flags,
			row.width, row.height, PresentTimingResultName(row.result)) >= 0;
	}
	bool summary(const char *name, uint64_t value)
	{
		if (fprintf(m_file, "summary,%llu,%lu,%lu,%llu", static_cast<unsigned long long>(m_owner.session),
			m_owner.process, m_owner.thread, static_cast<unsigned long long>(m_owner.start)) < 0) return false;
		for (unsigned int column = 5; column < PresentTimingCsvColumns - 2; ++column)
			if (fputc(',', m_file) == EOF) return false;
		return fprintf(m_file, ",%s,%llu\n", name, static_cast<unsigned long long>(value)) >= 0;
	}
	bool healthy() const { return m_file != 0 && ferror(m_file) == 0; }
	bool flush() { return fflush(m_file) == 0 && ferror(m_file) == 0; }
	bool close() { FILE *file = m_file; m_file = 0; return file == 0 || fclose(file) == 0; }
	bool publish() { return MoveFileW(m_pending, m_path) != FALSE; }
private:
	const wchar_t *m_directory;
	PresentTimingOwner m_owner;
	wchar_t m_path[MAX_PATH + 128], m_pending[MAX_PATH + 128];
	FILE *m_file;
};

template<class Driver, class Output> bool ExportPresentTimingCapture(PresentTimingCapture<Driver>& capture, Output& output)
{
	bool ok = output.open();
	if (ok) ok = output.header();
	for (unsigned int index = 0; ok && index < capture.counters().retained; ++index)
		ok = output.frame(capture.record(index));
	const PresentTimingCounters& counts = capture.counters();
	if (ok) ok = output.summary("capacity", PresentTimingCapture<Driver>::Capacity) &&
		output.summary("calls", counts.calls) && output.summary("retained", counts.retained) &&
		output.summary("dropped", counts.dropped) && output.summary("s_ok", counts.succeeded) &&
		output.summary("positive", counts.positive) && output.summary("failed", counts.failed) &&
		output.summary("invalid_clocks", counts.invalidClocks) && output.summary("io_failures", counts.ioFailures) &&
		output.summary("export_end", counts.retained);
	if (ok) ok = output.healthy();
	if (ok) ok = output.flush();
	const bool closed = output.close();
	ok = ok && closed;
	if (ok) ok = output.publish();
	if (!ok) capture.noteIoFailure();
	return ok;
}

class D3D11PresentFrameTiming
{
public:
	D3D11PresentFrameTiming() : m_configured(false), m_session(0) { m_directory[0] = 0; }
	void attachDevice()
	{
		if (!m_configured)
		{
			m_configured = true;
			const DWORD length = GetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", m_directory, MAX_PATH);
			if (length == 0) return;
			if (length >= MAX_PATH || !validDirectory()) { m_directory[0] = 0; return; }
			m_capture.enable(PresentTimingClockDriver(), ++m_session, GetCurrentProcessId(), GetCurrentThreadId());
		}
		m_capture.attachDevice();
	}
	bool enabled() const { return m_capture.enabled(); }
	void beginFrame() { m_capture.beginFrame(); }
	uint64_t clock() { return m_capture.clock(); }
	uint64_t cpuFrequency() { return m_capture.cpuFrequency(); }
	void record(uint64_t start, uint64_t end, uint64_t frequency, long result,
		unsigned int swapInterval, unsigned int flags, unsigned int width, unsigned int height)
	{ m_capture.record(start, end, frequency, result, swapInterval, flags, width, height); }
	void writeOnShutdown()
	{
		if (enabled() && GetCurrentThreadId() != m_capture.owner().thread) return;
		if (enabled())
		{
			if (validDirectory())
			{
				PresentTimingOutput output(m_directory, m_capture.owner());
				ExportPresentTimingCapture(m_capture, output);
			}
			else m_capture.noteIoFailure();
		}
		m_capture.reset(); m_configured = false; m_directory[0] = 0;
	}
private:
	bool validDirectory()
	{
		if (!((m_directory[0] && m_directory[1] == L':' &&
			(m_directory[2] == L'\\' || m_directory[2] == L'/')) ||
			(m_directory[0] == L'\\' && m_directory[1] == L'\\'))) return false;
		wchar_t absolute[MAX_PATH];
		const DWORD length = GetFullPathNameW(m_directory, MAX_PATH, absolute, 0);
		if (length == 0 || length > MAX_PATH - 80) return false;
		const DWORD attributes = GetFileAttributesW(absolute);
		if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
		wcscpy_s(m_directory, MAX_PATH, absolute);
		return true;
	}
	bool m_configured;
	uint64_t m_session;
	wchar_t m_directory[MAX_PATH];
	PresentTimingCapture<PresentTimingClockDriver> m_capture;
};

} } }
