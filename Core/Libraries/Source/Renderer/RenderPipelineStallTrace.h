#pragma once

// Private native diagnostic. Each stream has one writer and a bounded tail ring.
// Configuration allocates once; recording never allocates or writes files.
// Export is allowed only after the renderer's std::thread has been joined.
#include <cstdint>
#include <thread>
#if defined(_WIN64)
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <new>
#include <share.h>
#endif

namespace rts { namespace render { namespace detail {

class RenderPipelineStallTrace
{
public:
	enum Stream { Producer, Owner };
	enum Event
	{
		AcquireWaitBegin, AcquireWaitEnd, ReplyWaitBegin, ReplyWaitEnd,
		SerialWaitBegin, SerialWaitEnd, Publish, OwnerWaitBegin, OwnerWaitEnd,
		Dequeue, ExecuteBegin, ExecuteEnd, TelemetryBegin, TelemetryEnd,
		PoolLockBegin, PoolLockEnd, ResetBegin, PoolReturn
	};
	enum Status { Off, Configured, InvalidConfiguration, AllocationFailed,
		ExportSucceeded, ExportFailed };
	enum { Capacity = 65536 };
	static const unsigned int UnknownCounts = ~0U;

#if defined(_WIN64)
	RenderPipelineStallTrace() noexcept : m_status(Off), m_frequency(0), m_serial(false)
	{
		m_directory[0] = m_output[0] = L'\0';
		const DWORD length = GetEnvironmentVariableW(L"RTS_RENDER_PIPELINE_TRACE_DIR",
			m_directory, MAX_PATH);
		if (!length) return;
		m_status = InvalidConfiguration;
		if (length >= MAX_PATH || !safeDirectory(m_directory)) return;
		const wchar_t *otherVariables[] = { L"RTS_FRAME_TIMING_DIR",
			L"RTS_RENDER_OWNER_TIMING_DIR", L"RTS_GPU_FRAME_TIMING_DIR" };
		for (const wchar_t *variable : otherVariables)
		{
			wchar_t other[MAX_PATH];
			const DWORD otherLength = GetEnvironmentVariableW(variable, other, MAX_PATH);
			if (otherLength >= MAX_PATH) return;
			if (otherLength)
			{
				wchar_t absolute[MAX_PATH];
				const DWORD absoluteLength = GetFullPathNameW(other, MAX_PATH, absolute, nullptr);
				if (!absoluteLength || absoluteLength >= MAX_PATH) return;
				normalize(absolute);
				if (overlaps(m_directory, absolute)) return;
			}
		}
		LARGE_INTEGER frequency;
		if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
		m_producer.reset(new (std::nothrow) Buffer);
		m_owner.reset(new (std::nothrow) Buffer);
		if (!m_producer || !m_owner)
		{
			m_producer.reset(); m_owner.reset(); m_status = AllocationFailed; return;
		}
		m_frequency = frequency.QuadPart;
		m_producer->thread.store(GetCurrentThreadId(), std::memory_order_release);
		m_status = Configured;
	}
	bool enabled() const noexcept { return m_producer != nullptr; }
	void setSerial(bool serial) noexcept { m_serial = serial; }
	void bindOwner() noexcept
	{
		if (!enabled()) return;
		DWORD expected = 0;
		const DWORD current = GetCurrentThreadId();
		if (!m_owner->thread.compare_exchange_strong(expected, current, std::memory_order_acq_rel) &&
			expected != current) m_owner->rejectedThread.fetch_add(1, std::memory_order_relaxed);
	}
	void record(Stream stream, Event event, uint64_t sequence, uint64_t packet,
		unsigned int queued = UnknownCounts, unsigned int free = UnknownCounts,
		unsigned int pending = UnknownCounts, unsigned int detail = 0) noexcept
	{
		if (!enabled()) return;
		Buffer &buffer = stream == Producer ? *m_producer : *m_owner;
		const DWORD thread = GetCurrentThreadId();
		if (buffer.thread.load(std::memory_order_acquire) != thread)
		{
			buffer.rejectedThread.fetch_add(1, std::memory_order_relaxed); return;
		}
		LARGE_INTEGER tick;
		if (!QueryPerformanceCounter(&tick)) { ++buffer.clockFailures; return; }
		Record &row = buffer.records[buffer.next];
		row = { tick.QuadPart, sequence, packet, ++buffer.ordinal, thread,
			static_cast<unsigned int>(event), queued, free, pending, detail };
		buffer.next = (buffer.next + 1) % Capacity;
		if (buffer.count < Capacity) ++buffer.count;
		else ++buffer.overwritten;
	}
	Status status() const noexcept { return m_status; }
	const wchar_t *outputDirectory() const noexcept { return m_output; }
	// count/overwritten are read only by the writer or after the owner joins.
	unsigned int count(Stream stream) const noexcept
	{ return enabled() ? (stream == Producer ? m_producer : m_owner)->count : 0; }
	uint64_t overwritten(Stream stream) const noexcept
	{ return enabled() ? (stream == Producer ? m_producer : m_owner)->overwritten : 0; }
	unsigned int rejectedThread(Stream stream) const noexcept
	{ return enabled() ? (stream == Producer ? m_producer : m_owner)->rejectedThread.load() : 0; }
	// Cold export operations form a per-call recording seam, not runtime switches.
	struct ExportFileIO
	{
		bool created() noexcept { return true; }
		FILE *open(const wchar_t *path) noexcept { return _wfsopen(path, L"w+x", _SH_DENYWR); }
		bool written(bool ok) noexcept { return ok; }
		bool flush(FILE *file) noexcept { return ferror(file) == 0 && fflush(file) == 0; }
		bool close(FILE *file) noexcept { return fclose(file) == 0; }
		bool publish(const wchar_t *pending, const wchar_t *final) noexcept
		{ return MoveFileW(pending, final) != FALSE; }
	};
	bool exportAfterJoin(const std::thread &owner) noexcept
	{
		ExportFileIO io;
		return exportAfterJoin(owner, io);
	}
	template<class IO> bool exportAfterJoin(const std::thread &owner, IO &io) noexcept
	{
		if (!enabled() || owner.joinable() ||
			GetCurrentThreadId() != m_producer->thread.load() || m_status != Configured)
			return false;
		m_status = ExportFailed;
		// Recheck the configured parent at export; never create missing parents.
		if (!safeDirectory(m_directory)) return false;
		bool created = false;
		wchar_t final[MAX_PATH];
		const unsigned long long tick = GetTickCount64();
		for (unsigned int attempt = 0; attempt < 16; ++attempt)
		{
			if (swprintf_s(final, MAX_PATH, L"%ls\\pipeline-stall-%lu-%lu-%llu-%u",
				m_directory, GetCurrentProcessId(), GetCurrentThreadId(), tick, attempt) < 0 ||
				swprintf_s(m_output, MAX_PATH, L"%ls.pending", final) < 0)
				return false;
			if (GetFileAttributesW(final) != INVALID_FILE_ATTRIBUTES) continue;
			if (CreateDirectoryW(m_output, nullptr)) { created = true; break; }
			if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
		}
		if (!created || !io.created()) return false;
		wchar_t path[MAX_PATH];
		if (swprintf_s(path, MAX_PATH, L"%ls\\events.csv", m_output) < 0) return false;
		FILE *file = io.open(path);
		if (!file) return false;
		bool ok = fprintf(file, "stream,ordinal,event,qpc,thread_id,sequence,packet_id,queue_count,free_count,pending_count,detail\n") >= 0;
		ok = writeBuffer(file, "producer", *m_producer) && ok;
		ok = writeBuffer(file, "owner", *m_owner) && ok;
		ok = io.written(ok);
		const bool eventsFlushed = io.flush(file), eventsClosed = io.close(file);
		ok = ok && eventsFlushed && eventsClosed;
		if (!ok || swprintf_s(path, MAX_PATH, L"%ls\\summary.csv", m_output) < 0) return false;
		file = io.open(path);
		if (!file) return false;
		ok = fprintf(file, "stream,capacity,retained,overwritten,rejected_thread,clock_failures,qpc_frequency,thread_id,serial_policy\n") >= 0;
		for (unsigned int stream = 0; stream < 2; ++stream)
		{
			const Buffer &buffer = stream == 0 ? *m_producer : *m_owner;
			ok = fprintf(file, "%s,%u,%u,%llu,%u,%u,%lld,%lu,%u\n", stream == 0 ? "producer" : "owner",
				Capacity, buffer.count, buffer.overwritten, buffer.rejectedThread.load(),
				buffer.clockFailures, m_frequency, buffer.thread.load(), m_serial ? 1U : 0U) >= 0 && ok;
		}
		ok = io.written(ok);
		const bool summaryFlushed = io.flush(file), summaryClosed = io.close(file);
		ok = ok && summaryFlushed && summaryClosed;
		if (ok) ok = io.publish(m_output, final);
		if (ok) { wcscpy_s(m_output, MAX_PATH, final); m_status = ExportSucceeded; }
		return ok;
	}

private:
	struct Record
	{
		long long tick;
		uint64_t sequence, packet, ordinal;
		unsigned int thread, event, queued, free, pending, detail;
	};
	struct Buffer
	{
		Buffer() noexcept : thread(0), rejectedThread(0), next(0), count(0),
			clockFailures(0), ordinal(0), overwritten(0) {}
		std::atomic<DWORD> thread;
		std::atomic<unsigned int> rejectedThread;
		unsigned int next, count, clockFailures;
		uint64_t ordinal, overwritten;
		Record records[Capacity];
	};
	static void normalize(wchar_t *path) noexcept
	{
		for (wchar_t *p = path; *p; ++p) if (*p == L'/') *p = L'\\';
		size_t length = wcslen(path);
		while (length > 3 && path[length - 1] == L'\\') path[--length] = L'\0';
	}
	static bool safeDirectory(wchar_t *path) noexcept
	{
		const size_t length = wcslen(path);
		if (length < 3 || length > MAX_PATH - 80 ||
			!((path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
			  (path[0] == L'\\' && path[1] == L'\\'))) return false;
		wchar_t absolute[MAX_PATH];
		const DWORD absoluteLength = GetFullPathNameW(path, MAX_PATH, absolute, nullptr);
		if (!absoluteLength || absoluteLength > MAX_PATH - 80) return false;
		normalize(absolute);
		const DWORD attributes = GetFileAttributesW(absolute);
		if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
		wcscpy_s(path, MAX_PATH, absolute);
		return true;
	}
	static bool overlaps(const wchar_t *left, const wchar_t *right) noexcept
	{
		const size_t a = wcslen(left), b = wcslen(right), length = a < b ? a : b;
		if (_wcsnicmp(left, right, length) != 0) return false;
		return a == b || (length != 0 && left[length - 1] == L'\\') ||
			(a > b ? left[length] : right[length]) == L'\\';
	}
	static bool writeBuffer(FILE *file, const char *stream, const Buffer &buffer) noexcept
	{
		static const char *names[] = { "acquire_wait_begin", "acquire_wait_end", "reply_wait_begin",
			"reply_wait_end", "serial_wait_begin", "serial_wait_end", "publish", "owner_wait_begin",
			"owner_wait_end", "dequeue", "execute_begin", "execute_end", "telemetry_begin",
			"telemetry_end", "pool_lock_begin", "pool_lock_end", "reset_begin", "pool_return" };
		const unsigned int first = buffer.count == Capacity ? buffer.next : 0;
		for (unsigned int i = 0; i < buffer.count; ++i)
		{
			const Record &row = buffer.records[(first + i) % Capacity];
			if (fprintf(file, "%s,%llu,%s,%lld,%u,%llu,%llu,%u,%u,%u,%u\n", stream, row.ordinal,
				names[row.event], row.tick, row.thread, row.sequence, row.packet, row.queued,
				row.free, row.pending, row.detail) < 0) return false;
		}
		return true;
	}
	Status m_status;
	long long m_frequency;
	bool m_serial;
	wchar_t m_directory[MAX_PATH], m_output[MAX_PATH];
	std::unique_ptr<Buffer> m_producer, m_owner;
#else
	RenderPipelineStallTrace() noexcept {}
	bool enabled() const noexcept { return false; }
	void setSerial(bool) noexcept {}
	void bindOwner() noexcept {}
	void record(Stream, Event, uint64_t, uint64_t, unsigned int = UnknownCounts,
		unsigned int = UnknownCounts, unsigned int = UnknownCounts, unsigned int = 0) noexcept {}
	bool exportAfterJoin(const std::thread &) noexcept { return false; }
#endif
};

} } }
