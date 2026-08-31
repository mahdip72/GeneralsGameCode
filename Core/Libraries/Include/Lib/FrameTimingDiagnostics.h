#pragma once

// Shared opt-in owner-thread diagnostics. Set RTS_FRAME_TIMING_DIR to an existing
// directory; no output or clock queries occur when the variable is absent.
namespace rts { namespace frame_timing {

enum Phase
{
	FrameTotal, Radar, Audio, Client, Messages, Network, Logic, ClientStep, Wait,
	RecorderUpdate, RecorderEncode, RecorderFlush,
	AudioVoiceCreate, AudioVoiceDestroy, AudioDecodeOpen, AudioDecodeRead,
	RendererPresent, RendererTextureCollect, RendererTexturePrune,
	PhaseCount
};

} }

#if defined(_WIN64)
#include <windows.h>
#include <atomic>
#include <stdio.h>
#include <string.h>

namespace rts { namespace frame_timing {

class Capture
{
public:
	Capture() : m_file(NULL), m_owner(0), m_frequency(0), m_active(false),
		m_frameStart(0), m_bucketStart(0), m_rows(0), m_session(0),
		m_frameBegin(0), m_frameEnd(0), m_logicFrames(0), m_mode("interactive")
	{
		memset(m_stats, 0, sizeof(m_stats));
		char directory[MAX_PATH];
		const DWORD length = GetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory, sizeof(directory));
		if (length == 0 || length >= sizeof(directory))
			return;
		const DWORD attributes = GetFileAttributesA(directory);
		LARGE_INTEGER frequency;
		if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
			!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
			return;
		char path[MAX_PATH + 80];
		_snprintf(path, sizeof(path), "%s\\frame-timing-%lu-%lu.csv", directory,
			GetCurrentProcessId(), GetTickCount());
		path[sizeof(path) - 1] = '\0';
		// Exclusive creation never replaces a previous capture.
		m_file = fopen(path, "wx");
		if (!m_file)
			return;
		m_frequency = frequency.QuadPart;
		setvbuf(m_file, NULL, _IOFBF, 16384);
		fprintf(m_file, "session,mode,frame_begin,frame_end,logic_frames,wall_ms,phase,samples,total_ms,avg_ms,p95_upper_ms,p99_upper_ms,max_ms,over_33ms,over_100ms\n");
	}

	~Capture()
	{
		flush();
		if (m_file)
			fclose(m_file);
	}

	static Capture& instance()
	{
		static Capture capture;
		return capture;
	}

	void beginSession(const char* mode)
	{
		if (!m_file)
			return;
		flush();
		++m_session;
		m_mode = mode != NULL && mode[0] == 'h' ? "headless" : "interactive";
		m_owner.store(GetCurrentThreadId(), std::memory_order_release);
	}

	void endSession()
	{
		if (!m_file)
			return;
		m_active = false;
		flush();
	}

	void beginFrame(unsigned int frame)
	{
		const DWORD owner = m_owner.load(std::memory_order_acquire);
		if (owner != GetCurrentThreadId() || !m_file)
			return;
		m_frameStart = clock();
		if (!m_bucketStart)
		{
			m_bucketStart = m_frameStart;
			m_frameBegin = frame;
		}
		m_frameEnd = frame;
		m_active = true;
	}

	void endFrame(unsigned int frame)
	{
		if (!isActive())
			return;
		const __int64 end = clock();
		add(FrameTotal, end - m_frameStart);
		if (frame >= m_frameEnd)
			m_logicFrames += frame - m_frameEnd;
		m_frameEnd = frame;
		m_active = false;
		// Headless buckets are simulation-time based for early/late comparison.
		if ((m_mode[0] == 'h' && m_logicFrames >= 900) ||
			end - m_bucketStart >= m_frequency * 5)
			flush();
	}

	bool isActive() const
	{
		const DWORD owner = m_owner.load(std::memory_order_acquire);
		if (owner != GetCurrentThreadId())
			return false;
		return m_file != NULL && m_active;
	}

	static __int64 clock()
	{
		LARGE_INTEGER time;
		return QueryPerformanceCounter(&time) ? time.QuadPart : 0;
	}

	void add(Phase phase, __int64 ticks)
	{
		if (!isActive() || ticks < 0 || phase < 0 || phase >= PhaseCount)
			return;
		Stats& stats = m_stats[phase];
		const double milliseconds = static_cast<double>(ticks) * 1000.0 / m_frequency;
		++stats.count;
		stats.total += milliseconds;
		if (milliseconds > stats.maximum)
			stats.maximum = milliseconds;
		if (milliseconds > 1000.0 / 30.0)
			++stats.over33;
		if (milliseconds > 100.0)
			++stats.over100;
		unsigned int bucket = 0;
		while (bucket + 1 < BucketCount && milliseconds > bucketUpper(bucket))
			++bucket;
		++stats.histogram[bucket];
	}

private:
	enum { BucketCount = 17, MaxRows = 16384 };
	struct Stats
	{
		unsigned int count, over33, over100;
		double total, maximum;
		unsigned int histogram[BucketCount];
	};

	static double bucketUpper(unsigned int bucket)
	{
		static const double limits[BucketCount] =
			{0.1, 0.25, 0.5, 1, 2, 4, 8, 12, 16.667, 25, 33.334, 50, 100, 200, 500, 1000, 1.0e30};
		return limits[bucket];
	}

	static double percentileUpper(const Stats& stats, unsigned int percent)
	{
		const unsigned int rank = static_cast<unsigned int>(
			(static_cast<unsigned __int64>(stats.count) * percent + 99) / 100);
		unsigned int cumulative = 0;
		for (unsigned int i = 0; i < BucketCount; ++i)
		{
			cumulative += stats.histogram[i];
			if (cumulative >= rank)
				return bucketUpper(i) < stats.maximum ? bucketUpper(i) : stats.maximum;
		}
		return stats.maximum;
	}

	void flush()
	{
		if (!m_file || !m_bucketStart)
			return;
		static const char* names[PhaseCount] =
		{
			"frame", "radar", "audio", "client", "messages", "network", "logic", "client_step", "wait",
			"recorder_update", "recorder_encode", "recorder_flush",
			"audio_voice_create", "audio_voice_destroy", "audio_decode_open", "audio_decode_read",
			"renderer_present", "renderer_texture_collect", "renderer_texture_prune"
		};
		const double wall = static_cast<double>(clock() - m_bucketStart) * 1000.0 / m_frequency;
		for (unsigned int i = 0; i < PhaseCount && m_rows < MaxRows; ++i)
		{
			const Stats& stats = m_stats[i];
			if (!stats.count)
				continue;
			fprintf(m_file, "%u,%s,%u,%u,%u,%.3f,%s,%u,%.3f,%.4f,%.4f,%.4f,%.4f,%u,%u\n",
				m_session, m_mode, m_frameBegin, m_frameEnd, m_logicFrames, wall, names[i], stats.count,
				stats.total, stats.total / stats.count, percentileUpper(stats, 95), percentileUpper(stats, 99),
				stats.maximum, stats.over33, stats.over100);
			++m_rows;
		}
		const bool failed = ferror(m_file) != 0 || fflush(m_file) != 0;
		memset(m_stats, 0, sizeof(m_stats));
		m_bucketStart = 0;
		m_logicFrames = 0;
		if (failed || m_rows >= MaxRows)
		{
			fclose(m_file);
			m_file = NULL;
			m_active = false;
		}
	}

	FILE* m_file;
	std::atomic<DWORD> m_owner;
	__int64 m_frequency;
	bool m_active;
	__int64 m_frameStart, m_bucketStart;
	unsigned int m_rows, m_session, m_frameBegin, m_frameEnd, m_logicFrames;
	const char* m_mode;
	Stats m_stats[PhaseCount];
	Capture(const Capture&);
	Capture& operator=(const Capture&);
};

class Scope
{
public:
	explicit Scope(Phase phase) : m_capture(Capture::instance()), m_phase(phase),
		m_start(m_capture.isActive() ? Capture::clock() : 0) {}
	Scope(Capture& capture, Phase phase) : m_capture(capture), m_phase(phase),
		m_start(m_capture.isActive() ? Capture::clock() : 0) {}
	~Scope() { if (m_start) m_capture.add(m_phase, Capture::clock() - m_start); }
private:
	Capture& m_capture;
	Phase m_phase;
	__int64 m_start;
	Scope(const Scope&);
	Scope& operator=(const Scope&);
};

class Session
{
public:
	explicit Session(const char* mode) { Capture::instance().beginSession(mode); }
	~Session() { Capture::instance().endSession(); }
private:
	Session(const Session&);
	Session& operator=(const Session&);
};

inline void BeginFrame(unsigned int frame) { Capture::instance().beginFrame(frame); }
inline void EndFrame(unsigned int frame) { Capture::instance().endFrame(frame); }

} }
#else
namespace rts { namespace frame_timing {
class Scope { public: explicit Scope(Phase) {} };
class Session { public: explicit Session(const char*) {} };
inline void BeginFrame(unsigned int) {}
inline void EndFrame(unsigned int) {}
} }
#endif
