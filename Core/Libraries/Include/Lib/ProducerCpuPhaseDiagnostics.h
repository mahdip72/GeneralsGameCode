#pragma once

// Development attribution only: CPU time belongs to the game-producer thread.
// Included in a few coarse-phase TUs, never in FrameTimingDiagnostics.h.
namespace rts { namespace producer_cpu {
enum Phase {
    ProducerFrame, LogicOwnerIntake, LogicMutableIsland, LogicSpatial,
    LogicOwnerTail, LogicVerificationPublication, Sorting, Particles,
    SceneFlush, SortingGeometryPack, SortingKernel, SortingSink,
    Audio, Client, ClientStep, Messages, MainWait, Radar, PhaseCount
};
} }

#if defined(_WIN64)
#include <windows.h>
#include <stdio.h>
#include <share.h>

namespace rts { namespace producer_cpu {
enum { Capacity = 32768 };
enum Flags {
    BeginQpcValid = 1, EndQpcValid = 2, BeginCpuValid = 4, EndCpuValid = 8,
    QpcOrdered = 16, CpuOrdered = 32
};
enum ExportStatus { Disabled, Active, ExportSucceeded, OpenFailed, WriteFailed,
    InvalidDirectory, FrequencyFailed, NestedSessionRejected };
struct Record {
    unsigned __int64 sequence;
    unsigned int frame, phase, flags;
    __int64 beginQpc, endQpc;
    unsigned __int64 user100ns, kernel100ns;
};
struct State {
    Record records[Capacity];
    char directory[MAX_PATH];
    __int64 frequency;
    DWORD owner;
    unsigned int frame, count, next;
    unsigned __int64 sequence, overwrites, cpuQueryFailures, qpcQueryFailures;
    ExportStatus exportStatus;
};
// Only Session accesses Storage. No constructor, allocation, or bulk zeroing.
// Scope reads just the TLS pointer, which is null on all unbound threads.
inline State& Storage() { static State state; return state; }
inline State*& Selected() { static thread_local State* state = NULL; return state; }
inline unsigned __int64 FileTimeValue(const FILETIME& value) {
    return (static_cast<unsigned __int64>(value.dwHighDateTime) << 32) |
        value.dwLowDateTime;
}
inline bool ReadCpu(State& state, unsigned __int64& user, unsigned __int64& kernel) {
    FILETIME create, exit, system, application;
    if (!GetThreadTimes(GetCurrentThread(), &create, &exit, &system, &application)) {
        ++state.cpuQueryFailures;
        return false;
    }
    user = FileTimeValue(application);
    kernel = FileTimeValue(system);
    return true;
}
inline bool ReadQpc(State& state, __int64& value) {
    LARGE_INTEGER qpc;
    if (!QueryPerformanceCounter(&qpc)) {
        ++state.qpcQueryFailures;
        return false;
    }
    value = qpc.QuadPart;
    return true;
}
inline const char* PhaseName(unsigned int phase) {
    static const char* const names[] = {
        "producer_frame", "logic_owner_intake", "logic_mutable_island",
        "logic_spatial", "logic_owner_tail", "logic_verification_publication",
        "sorting", "particles", "scene_flush", "sorting_geometry_pack",
        "sorting_kernel", "sorting_sink",
        "audio", "client", "client_step", "messages", "main_wait", "radar"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == PhaseCount,
        "Every producer CPU phase must retain its CSV name");
    return phase < PhaseCount ? names[phase] : "invalid";
}
inline ExportStatus Export(State& state) {
    char path[MAX_PATH + 96];
    _snprintf(path, sizeof(path), "%s\\producer-cpu-phases-%lu-%lu-%lu.csv",
        state.directory, GetCurrentProcessId(), state.owner, GetTickCount());
    path[sizeof(path) - 1] = '\0';
    // Ordinary execute() unwind only. Exclusive creation never clobbers evidence.
    FILE* file = _fsopen(path, "w+x", _SH_DENYWR);
    if (file == NULL) return state.exportStatus = OpenFailed;
    bool ok = fprintf(file,
        "# schema=producer-cpu-phases.v1,qpc_frequency=%lld,cpu_unit=100ns,owner_tid=%lu,capacity=%u\n"
        "# flags=begin_qpc:1|end_qpc:2|begin_cpu:4|end_cpu:8|qpc_ordered:16|cpu_ordered:32\n"
        "sequence,frame,phase,phase_name,begin_qpc,end_qpc,user_100ns,kernel_100ns,flags\n",
        state.frequency, state.owner, Capacity) >= 0;
    const unsigned int first = state.count == Capacity ? state.next : 0;
    for (unsigned int i = 0; i < state.count && ok; ++i) {
        const Record& row = state.records[(first + i) % Capacity];
        ok = fprintf(file, "%llu,%u,%u,%s,%lld,%lld,%llu,%llu,%u\n",
            row.sequence, row.frame, row.phase, PhaseName(row.phase), row.beginQpc,
            row.endQpc, row.user100ns, row.kernel100ns, row.flags) >= 0;
    }
    if (ok) ok = fprintf(file,
        "# end,total_completed=%llu,retained=%u,ring_overwrites=%llu,cpu_query_failures=%llu,qpc_query_failures=%llu\n",
        state.sequence, state.count, state.overwrites, state.cpuQueryFailures,
        state.qpcQueryFailures) >= 0;
    if (fflush(file) != 0 || ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    return state.exportStatus = ok ? ExportSucceeded : WriteFailed;
}
class Session {
public:
    Session() : m_state(NULL), m_status(Disabled) {
        if (Selected() != NULL) { m_status = NestedSessionRejected; return; }
        char directory[MAX_PATH];
        const DWORD length = GetEnvironmentVariableA("RTS_FRAME_TIMING_DIR",
            directory, sizeof(directory));
        if (length == 0) return;
        if (length >= sizeof(directory)) { m_status = InvalidDirectory; OutputDebugStringA("producer-cpu-phases: directory too long.\n"); return; }
        const DWORD attributes = GetFileAttributesA(directory);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            m_status = InvalidDirectory; OutputDebugStringA("producer-cpu-phases: invalid opt-in directory.\n"); return;
        }
        LARGE_INTEGER frequency;
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            m_status = FrequencyFailed; OutputDebugStringA("producer-cpu-phases: QPC frequency unavailable.\n"); return;
        }
        State& state = Storage();
        for (DWORD i = 0; i <= length; ++i) state.directory[i] = directory[i];
        state.frequency = frequency.QuadPart;
        state.owner = GetCurrentThreadId();
        state.frame = state.count = state.next = 0;
        state.sequence = state.overwrites = state.cpuQueryFailures = state.qpcQueryFailures = 0;
        state.exportStatus = Active;
        m_state = Selected() = &state;
        m_status = Active;
    }
    ~Session() { finish(); }
    ExportStatus status() const { return m_status; }
    void finish() {
        if (m_state == NULL || m_status != Active) return;
        // Unbind before exporting: no record callbacks or worker interaction.
        Selected() = NULL;
        m_status = Export(*m_state);
        if (m_status != ExportSucceeded)
            OutputDebugStringA("producer-cpu-phases: teardown export failed; inspect debugger and directory.\n");
    }
private:
    State* m_state;
    ExportStatus m_status;
    Session(const Session&);
    Session& operator=(const Session&);
};
inline void BeginFrame(unsigned int frame) {
    State* state = Selected();
    if (state != NULL) state->frame = frame;
}
class Scope {
public:
    explicit Scope(Phase phase) : m_state(Selected()), m_user(0), m_kernel(0) {
        if (m_state == NULL) return;
        m_record.sequence = 0;
        m_record.frame = m_state->frame;
        m_record.phase = phase;
        m_record.flags = 0;
        m_record.beginQpc = m_record.endQpc = 0;
        m_record.user100ns = m_record.kernel100ns = 0;
        // QPC brackets CPU queries; overhead is inclusive and must be calibrated.
        if (ReadQpc(*m_state, m_record.beginQpc)) m_record.flags |= BeginQpcValid;
        if (ReadCpu(*m_state, m_user, m_kernel)) m_record.flags |= BeginCpuValid;
    }
    ~Scope() { finish(); }
    void finish() {
        if (m_state == NULL) return;
        State* state = m_state;
        m_state = NULL; // Explicit finish plus destructor never duplicates a row.
        if (Selected() != state) return; // Session already ended: do not touch capture.
        unsigned __int64 user = 0, kernel = 0;
        if (ReadCpu(*state, user, kernel)) m_record.flags |= EndCpuValid;
        if (ReadQpc(*state, m_record.endQpc)) m_record.flags |= EndQpcValid;
        if ((m_record.flags & (BeginQpcValid | EndQpcValid)) == (BeginQpcValid | EndQpcValid) &&
            m_record.endQpc >= m_record.beginQpc) m_record.flags |= QpcOrdered;
        if ((m_record.flags & (BeginCpuValid | EndCpuValid)) == (BeginCpuValid | EndCpuValid) &&
            user >= m_user && kernel >= m_kernel) {
            m_record.flags |= CpuOrdered;
            m_record.user100ns = user - m_user;
            m_record.kernel100ns = kernel - m_kernel;
        }
        m_record.sequence = ++state->sequence;
        state->records[state->next] = m_record;
        state->next = (state->next + 1) % Capacity;
        if (state->count < Capacity) ++state->count;
        else ++state->overwrites;
    }
private:
    State* m_state;
    Record m_record;
    unsigned __int64 m_user, m_kernel;
    Scope(const Scope&);
    Scope& operator=(const Scope&);
};
} }
#else
namespace rts { namespace producer_cpu {
class Session { public: Session() {} void finish() {} };
class Scope { public: explicit Scope(Phase) {} void finish() {} };
inline void BeginFrame(unsigned int) {}
} }
#endif