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
};

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
			used += _snprintf(header + used, sizeof(header) - used, ",texture_invalid\n");
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
			used += _snprintf(row + used, sizeof(row) - used, "\n");
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
}

class Scope
{
public:
	Scope(Phase phase, bool frameTimingActive) : m_phase(phase), m_previous(0),
		m_active(frameTimingActive && Enabled() && Output().isOpen())
	{
		if (m_active) { m_previous = Current(); Current() = &m_counts; }
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
class Scope { public: Scope(Phase, bool) {} };
inline void RenderState(unsigned int) {}
inline void TextureState(unsigned int) {}
inline void AcceptedDraw(unsigned int, unsigned int) {}
inline void UnlockAttempt(unsigned long) {}
inline void UnlockSuccess(unsigned long) {}
} }
#endif
#endif
