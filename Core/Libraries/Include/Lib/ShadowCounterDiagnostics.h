#ifndef RTS_LIB_SHADOWCOUNTERDIAGNOSTICS_H
#define RTS_LIB_SHADOWCOUNTERDIAGNOSTICS_H

// TEMPORARY Stage 5 diagnostics. Remove before a final release.
#if defined(_WIN64)
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace rts { namespace shadow_counters {

enum Phase { Projected, Volume };
enum DurationCategory
{
	BufferLockDuration,
	BufferUnlockDuration,
	ExecuteDrawDuration,
	ExecuteOtherDuration,
	RenderStateSetterDuration,
	GamePacketSubmitDuration,
	DurationCategoryCount
};
enum { RenderStateSlots = 40, TextureStateSlots = 24, MaxRows = 10000 };

inline void Add(unsigned long long *value, unsigned long long amount)
{
	*value = ULLONG_MAX - *value < amount ? ULLONG_MAX : *value + amount;
}

struct Counts
{
	Counts() { memset(this, 0, sizeof(*this)); }
	unsigned long long renderState[RenderStateSlots + 1];
	unsigned long long textureState[TextureStateSlots + 1];
	unsigned long long renderCalls, textureCalls, draws, indices, vertices;
	unsigned long long unlockAttempts, unlockRequestedBytes, unlockSuccessBytes;
	unsigned long long qpcFrequencyHz;
	unsigned long long durationCalls[DurationCategoryCount];
	unsigned long long durationTicks[DurationCategoryCount];
	unsigned long long qpcFailures;
	unsigned long long resourceFindCalls, resourceFindLinearIterations;
	unsigned long long resourceFindMisses;
	unsigned long long threadedMetricsSnapshotAttempted;
	unsigned long long threadedMetricsSnapshotValid;
	unsigned long long threadedMetricsOwnerExecutionNanoseconds;
	unsigned long long threadedMetricsCompletedFrames;
	unsigned long long threadedMetricsSubmittedFrames;
	unsigned long long threadedMetricsProducerWaitNanoseconds;
	unsigned long long threadedMetricsBackpressureWaits;
};

inline const char *DurationName(unsigned int category)
{
	switch (category)
	{
	case BufferLockDuration: return "buffer_lock";
	case BufferUnlockDuration: return "buffer_unlock";
	case ExecuteDrawDuration: return "execute_draw";
	case ExecuteOtherDuration: return "execute_other";
	case RenderStateSetterDuration: return "render_state_setter";
	case GamePacketSubmitDuration: return "packet_submit";
	default: return "duration_invalid";
	}
}

class Writer
{
public:
	explicit Writer(unsigned int limit = MaxRows) : m_file(INVALID_HANDLE_VALUE),
		m_used(0), m_rows(0), m_limit(limit), m_truncated(false) {}
	~Writer() { close(); }
	bool open(const char *directory, const char *name)
	{
		const DWORD attributes = directory != 0 ?
			GetFileAttributesA(directory) : INVALID_FILE_ATTRIBUTES;
		if (m_file != INVALID_HANDLE_VALUE || directory == 0 || name == 0 ||
			attributes == INVALID_FILE_ATTRIBUTES ||
			!(attributes & FILE_ATTRIBUTE_DIRECTORY))
			return false;
		char path[MAX_PATH];
		const int length = _snprintf(path, sizeof(path), "%s\\%s", directory, name);
		if (length <= 0 || length >= static_cast<int>(sizeof(path))) return false;
		m_file = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, 0);
		if (m_file == INVALID_HANDLE_VALUE) return false;
		char header[2048];
		int used = _snprintf(header, sizeof(header), "scope,phase,render_calls,texture_calls,draws,indices,vertices,unlock_attempts,unlock_requested_bytes,unlock_success_bytes");
		for (unsigned int i = 0; i < RenderStateSlots && used > 0 && used < static_cast<int>(sizeof(header)); ++i)
			used += _snprintf(header + used, sizeof(header) - used, ",render_%u", i);
		if (used > 0 && used < static_cast<int>(sizeof(header)))
			used += _snprintf(header + used, sizeof(header) - used, ",render_invalid");
		for (unsigned int i = 0; i < TextureStateSlots && used > 0 && used < static_cast<int>(sizeof(header)); ++i)
			used += _snprintf(header + used, sizeof(header) - used, ",texture_%u", i);
		if (used > 0 && used < static_cast<int>(sizeof(header)))
			used += _snprintf(header + used, sizeof(header) - used, ",texture_invalid,qpc_frequency_hz");
		for (unsigned int i = 0; i < DurationCategoryCount && used > 0 && used < static_cast<int>(sizeof(header)); ++i)
			used += _snprintf(header + used, sizeof(header) - used, ",%s_calls,%s_qpc_ticks",
				DurationName(i), DurationName(i));
		if (used > 0 && used < static_cast<int>(sizeof(header)))
			used += _snprintf(header + used, sizeof(header) - used,
				",qpc_failures,resource_find_calls,resource_find_linear_iterations,resource_find_misses"
				",threaded_metrics_snapshot_attempted,threaded_metrics_snapshot_valid"
				",threaded_metrics_owner_execution_nanoseconds,threaded_metrics_completed_frames"
				",threaded_metrics_submitted_frames,threaded_metrics_producer_wait_nanoseconds"
				",threaded_metrics_backpressure_waits\n");
		if (used <= 0 || used >= static_cast<int>(sizeof(header)) ||
			!buffer(header, static_cast<unsigned int>(used))) { close(); return false; }
		return true;
	}
	bool append(unsigned long long ordinal, Phase phase, const Counts &counts)
	{
		if (m_file == INVALID_HANDLE_VALUE) return false;
		if (m_rows >= m_limit) { m_truncated = true; return false; }
		char row[4096];
		int used = _snprintf(row, sizeof(row), "%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
			ordinal, phase == Projected ? "projected" : "volume", counts.renderCalls,
			counts.textureCalls, counts.draws, counts.indices, counts.vertices,
			counts.unlockAttempts, counts.unlockRequestedBytes, counts.unlockSuccessBytes);
		for (unsigned int i = 0; i <= RenderStateSlots && used > 0 && used < static_cast<int>(sizeof(row)); ++i)
			used += _snprintf(row + used, sizeof(row) - used, ",%llu", counts.renderState[i]);
		for (unsigned int i = 0; i <= TextureStateSlots && used > 0 && used < static_cast<int>(sizeof(row)); ++i)
			used += _snprintf(row + used, sizeof(row) - used, ",%llu", counts.textureState[i]);
		if (used > 0 && used < static_cast<int>(sizeof(row)))
			used += _snprintf(row + used, sizeof(row) - used, ",%llu",
				counts.qpcFrequencyHz);
		for (unsigned int i = 0; i < DurationCategoryCount && used > 0 && used < static_cast<int>(sizeof(row)); ++i)
			used += _snprintf(row + used, sizeof(row) - used, ",%llu,%llu",
				counts.durationCalls[i], counts.durationTicks[i]);
		if (used > 0 && used < static_cast<int>(sizeof(row)))
			used += _snprintf(row + used, sizeof(row) - used,
				",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
				counts.qpcFailures,
				counts.resourceFindCalls, counts.resourceFindLinearIterations,
				counts.resourceFindMisses,
				counts.threadedMetricsSnapshotAttempted,
				counts.threadedMetricsSnapshotValid,
				counts.threadedMetricsOwnerExecutionNanoseconds,
				counts.threadedMetricsCompletedFrames,
				counts.threadedMetricsSubmittedFrames,
				counts.threadedMetricsProducerWaitNanoseconds,
				counts.threadedMetricsBackpressureWaits);
		if (used <= 0 || used >= static_cast<int>(sizeof(row)) ||
			!buffer(row, static_cast<unsigned int>(used))) { close(); return false; }
		++m_rows;
		return true;
	}
	void close()
	{
		if (m_file == INVALID_HANDLE_VALUE) return;
		flush();
		CloseHandle(m_file);
		m_file = INVALID_HANDLE_VALUE;
	}
	bool isOpen() const { return m_file != INVALID_HANDLE_VALUE; }
	unsigned int rows() const { return m_rows; }
	bool truncated() const { return m_truncated; }
private:
	bool flush()
	{
		if (m_file == INVALID_HANDLE_VALUE) return false;
		if (m_used == 0) return true;
		DWORD written = 0;
		const bool ok = WriteFile(m_file, m_buffer, m_used, &written, 0) != 0 && written == m_used;
		m_used = 0;
		return ok;
	}
	bool buffer(const char *text, unsigned int length)
	{
		if (length > sizeof(m_buffer)) return false;
		if (length > sizeof(m_buffer) - m_used && !flush()) return false;
		memcpy(m_buffer + m_used, text, length);
		m_used += length;
		return true;
	}
	HANDLE m_file;
	char m_buffer[16384];
	unsigned int m_used, m_rows, m_limit;
	bool m_truncated;
	Writer(const Writer&);
	Writer& operator=(const Writer&);
};

inline Counts *&Current()
{
	static thread_local Counts *current = 0;
	return current;
}

inline Writer &Output()
{
	static Writer writer;
	static bool initialized = false;
	if (!initialized)
	{
		initialized = true;
		char directory[MAX_PATH];
		const DWORD length = GetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory, sizeof(directory));
		if (length > 0 && length < sizeof(directory))
		{
			char name[96];
			_snprintf(name, sizeof(name), "shadow-counters-%lu-%lu.csv",
				GetCurrentProcessId(), GetTickCount());
			writer.open(directory, name);
		}
	}
	return writer;
}

inline bool Enabled()
{
	static const bool enabled = []() {
		char value[4] = {0};
		return GetEnvironmentVariableA("RTS_SHADOW_COUNTERS", value,
			sizeof(value)) == 1 && value[0] == '1';
	}();
	return enabled;
}

inline void Merge(Counts *destination, const Counts &source)
{
	for (unsigned int i = 0; i <= RenderStateSlots; ++i)
		Add(&destination->renderState[i], source.renderState[i]);
	for (unsigned int i = 0; i <= TextureStateSlots; ++i)
		Add(&destination->textureState[i], source.textureState[i]);
	Add(&destination->renderCalls, source.renderCalls);
	Add(&destination->textureCalls, source.textureCalls);
	Add(&destination->draws, source.draws);
	Add(&destination->indices, source.indices);
	Add(&destination->vertices, source.vertices);
	Add(&destination->unlockAttempts, source.unlockAttempts);
	Add(&destination->unlockRequestedBytes, source.unlockRequestedBytes);
	Add(&destination->unlockSuccessBytes, source.unlockSuccessBytes);
	if (destination->qpcFrequencyHz == 0)
		destination->qpcFrequencyHz = source.qpcFrequencyHz;
	for (unsigned int i = 0; i < DurationCategoryCount; ++i)
	{
		Add(&destination->durationCalls[i], source.durationCalls[i]);
		Add(&destination->durationTicks[i], source.durationTicks[i]);
	}
	Add(&destination->qpcFailures, source.qpcFailures);
	Add(&destination->resourceFindCalls, source.resourceFindCalls);
	Add(&destination->resourceFindLinearIterations,
		source.resourceFindLinearIterations);
	Add(&destination->resourceFindMisses, source.resourceFindMisses);
	// These are cumulative device snapshots, not additive per-scope counters.
	destination->threadedMetricsSnapshotAttempted =
		destination->threadedMetricsSnapshotAttempted != 0 ||
			source.threadedMetricsSnapshotAttempted != 0;
	if (destination->threadedMetricsSnapshotValid == 0 &&
		source.threadedMetricsSnapshotValid != 0)
	{
		destination->threadedMetricsSnapshotValid = 1;
		destination->threadedMetricsOwnerExecutionNanoseconds =
			source.threadedMetricsOwnerExecutionNanoseconds;
		destination->threadedMetricsCompletedFrames =
			source.threadedMetricsCompletedFrames;
		destination->threadedMetricsSubmittedFrames =
			source.threadedMetricsSubmittedFrames;
		destination->threadedMetricsProducerWaitNanoseconds =
			source.threadedMetricsProducerWaitNanoseconds;
		destination->threadedMetricsBackpressureWaits =
			source.threadedMetricsBackpressureWaits;
	}
}

class Scope
{
public:
	Scope(Phase phase, bool frameTimingActive) : m_phase(phase), m_previous(0),
		m_active(false)
	{
		if (!frameTimingActive || !Enabled() || !Output().isOpen())
			return;
		m_previous = Current();
		if (m_previous != 0)
			m_counts.qpcFrequencyHz = m_previous->qpcFrequencyHz;
		else
		{
			LARGE_INTEGER frequency;
			if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
				m_counts.qpcFrequencyHz =
					static_cast<unsigned long long>(frequency.QuadPart);
		}
		m_active = true;
		Current() = &m_counts;
	}
	~Scope()
	{
		if (!m_active) return;
		Current() = m_previous;
		if (m_previous != 0) { Merge(m_previous, m_counts); return; }
		static unsigned long long ordinal = 0;
		if (ordinal != ULLONG_MAX) ++ordinal;
		Output().append(ordinal, m_phase, m_counts);
	}
private:
	Phase m_phase;
	Counts m_counts;
	Counts *m_previous;
	bool m_active;
	Scope(const Scope&);
	Scope& operator=(const Scope&);
};

// Captures the active row at entry. The enclosing Scope owns that Counts for
// the timer's lexical lifetime; do not re-read Current() on destruction, since
// nested scopes temporarily replace it. Work is skipped without an active row.
class DurationScope
{
public:
	explicit DurationScope(DurationCategory category) : m_counts(Current()),
		m_category(category), m_startTicks(0), m_started(false)
	{
		const unsigned int categoryIndex =
			static_cast<unsigned int>(category);
		if (m_counts == 0 || categoryIndex >= DurationCategoryCount)
		{
			m_counts = 0;
			return;
		}
		if (m_counts->qpcFrequencyHz == 0)
			return;
		LARGE_INTEGER start;
		if (QueryPerformanceCounter(&start) && start.QuadPart >= 0)
		{
			m_startTicks = start.QuadPart;
			m_started = true;
		}
	}
	~DurationScope()
	{
		if (m_counts == 0)
			return;
		Add(&m_counts->durationCalls[m_category], 1);
		if (!m_started)
		{
			Add(&m_counts->qpcFailures, 1);
			return;
		}
		LARGE_INTEGER finish;
		if (!QueryPerformanceCounter(&finish) || finish.QuadPart < m_startTicks)
		{
			Add(&m_counts->qpcFailures, 1);
			return;
		}
		Add(&m_counts->durationTicks[m_category],
			static_cast<unsigned long long>(finish.QuadPart - m_startTicks));
	}
private:
	Counts *m_counts;
	DurationCategory m_category;
	LONGLONG m_startTicks;
	bool m_started;
	DurationScope(const DurationScope&);
	DurationScope& operator=(const DurationScope&);
};

class ResourceFindScope
{
public:
	ResourceFindScope() : m_counts(Current()), m_linearIterations(0),
		m_found(false)
	{
		if (m_counts != 0)
			Add(&m_counts->resourceFindCalls, 1);
	}
	~ResourceFindScope()
	{
		if (m_counts == 0)
			return;
		Add(&m_counts->resourceFindLinearIterations, m_linearIterations);
		if (!m_found)
			Add(&m_counts->resourceFindMisses, 1);
	}
	void linearIteration()
	{
		if (m_counts != 0 && m_linearIterations != ULLONG_MAX)
			++m_linearIterations;
	}
	void found() { if (m_counts != 0) m_found = true; }
private:
	Counts *m_counts;
	unsigned long long m_linearIterations;
	bool m_found;
	ResourceFindScope(const ResourceFindScope&);
	ResourceFindScope& operator=(const ResourceFindScope&);
};

inline void RenderState(unsigned int state)
{
	Counts *counts = Current();
	if (!counts) return;
	Add(&counts->renderCalls, 1);
	Add(&counts->renderState[state < RenderStateSlots ? state : RenderStateSlots], 1);
}
inline void TextureState(unsigned int state)
{
	Counts *counts = Current();
	if (!counts) return;
	Add(&counts->textureCalls, 1);
	Add(&counts->textureState[state < TextureStateSlots ? state : TextureStateSlots], 1);
}
inline void AcceptedDraw(unsigned int indices, unsigned int vertices)
{
	Counts *counts = Current();
	if (!counts) return;
	Add(&counts->draws, 1);
	Add(&counts->indices, indices);
	Add(&counts->vertices, vertices);
}
inline void UnlockAttempt(size_t bytes)
{
	Counts *counts = Current();
	if (!counts) return;
	Add(&counts->unlockAttempts, 1);
	Add(&counts->unlockRequestedBytes, static_cast<unsigned long long>(bytes));
}
inline void UnlockSuccess(size_t bytes)
{
	Counts *counts = Current();
	if (counts) Add(&counts->unlockSuccessBytes, static_cast<unsigned long long>(bytes));
}

} }
#else
namespace rts { namespace shadow_counters {
enum Phase { Projected, Volume };
enum DurationCategory
{
	BufferLockDuration,
	BufferUnlockDuration,
	ExecuteDrawDuration,
	ExecuteOtherDuration,
	RenderStateSetterDuration,
	GamePacketSubmitDuration,
	DurationCategoryCount
};
class Scope { public: Scope(Phase, bool) {} };
class DurationScope { public: explicit DurationScope(DurationCategory) {} };
class ResourceFindScope
{
public:
	ResourceFindScope() {}
	void linearIteration() {}
	void found() {}
};
inline void RenderState(unsigned int) {}
inline void TextureState(unsigned int) {}
inline void AcceptedDraw(unsigned int, unsigned int) {}
inline void UnlockAttempt(unsigned long) {}
inline void UnlockSuccess(unsigned long) {}
} }
#endif
#endif
