#ifndef RTS_LIB_MENUCRITICALPATHDIAGNOSTICS_H
#define RTS_LIB_MENUCRITICALPATHDIAGNOSTICS_H

// TEMP Stage 5 probe. Opt in with RTS_MENU_CRITICAL_PATH=1 and an existing
// RTS_FRAME_TIMING_DIR. No render authority, queue, or presentation decisions.
namespace rts { namespace menu_trace {
enum Reason { Unknown, PriorBufferPublication, OutsideBufferMutation,
 CreateBuffer, CreateTexture, PriorTexturePublication, RefreshTexture,
 CopyColor, Capture, Lifecycle, Drain, ResourceFence, Backpressure,
 SerialFrame, ReasonCount };
enum Kind { ProducerWait, Enqueue, OwnerPacket, OwnerIdle, BackendPresent,
 DxgiPresent, Startup, Shutdown, KindCount };
} }

#if defined(_WIN64)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rts { namespace menu_trace {
typedef unsigned long long Tick;
enum { MaxEvents = 8192 };
inline bool Enabled()
{
 static const bool enabled = []() { char value[4] = {0};
  return GetEnvironmentVariableA("RTS_MENU_CRITICAL_PATH", value,
   sizeof(value)) == 1 && value[0] == '1'; }();
 return enabled;
}
inline Reason &CurrentReason() { static thread_local Reason reason = Unknown; return reason; }
class ReasonScope
{
public:
 explicit ReasonScope(Reason reason, bool fallback = false) : m_active(Enabled()), m_previous(Unknown)
 { if (m_active) { m_previous = CurrentReason();
   if (!fallback || m_previous == Unknown) CurrentReason() = reason; } }
 ~ReasonScope() { if (m_active) CurrentReason() = m_previous; }
private:
 bool m_active; Reason m_previous;
 ReasonScope(const ReasonScope &); ReasonScope &operator=(const ReasonScope &);
};
struct Context
{
 Context() : sequence(0), lastSequence(0), packet(0), commands(0), bytes(0), control(0), recording(0) {}
 Tick sequence, lastSequence, packet, commands, bytes;
 unsigned int control, recording;
};
struct Event { Tick begin, end; Context context; unsigned int kind, reason, depth; int result; };
struct Aggregate { Tick calls, ticks; };
struct Lane
{
 Lane() : used(0), dropped(0), qpcFailures(0), depth(0), threadId(0), limit(MaxEvents)
 { memset(totals, 0, sizeof(totals)); }
 Event events[MaxEvents];
 Aggregate totals[KindCount][ReasonCount];
 Context context;
 Tick dropped, qpcFailures;
 unsigned int used, depth; DWORD threadId; unsigned int limit;
 Tick now() { LARGE_INTEGER value; if (!QueryPerformanceCounter(&value))
  { ++qpcFailures; return 0; } return static_cast<Tick>(value.QuadPart); }
 void record(const Event &event)
 {
  Aggregate &total = totals[event.kind][event.reason]; ++total.calls;
  if (event.begin && event.end >= event.begin) total.ticks += event.end - event.begin;
  if (used < limit) events[used++] = event; else ++dropped;
 }
};
inline Lane *&CurrentLane() { static thread_local Lane *lane = 0; return lane; }
class Bind
{
public:
 explicit Bind(Lane *lane) : m_previous(CurrentLane()) { CurrentLane() = lane; }
 ~Bind() { CurrentLane() = m_previous; }
private: Lane *m_previous; Bind(const Bind &); Bind &operator=(const Bind &);
};
class Scope
{
public:
 explicit Scope(Kind kind, Lane *lane = CurrentLane()) : m_lane(lane)
 {
  if (m_lane) { m_event.context = m_lane->context; m_event.kind = kind;
   m_event.reason = CurrentReason(); m_event.depth = ++m_lane->depth;
   m_event.result = 0; m_event.begin = m_lane->now(); }
 }
 ~Scope() { if (m_lane) { m_event.end = m_lane->now();
  m_lane->record(m_event); --m_lane->depth; } }
 void result(int result) { if (m_lane) m_event.result = result; }
private: Lane *m_lane; Event m_event; Scope(const Scope &); Scope &operator=(const Scope &);
};
inline const char *ReasonName(unsigned int reason)
{
 static const char *names[ReasonCount] = { "unknown", "prior_buffer_publication",
  "outside_buffer_mutation", "create_buffer", "create_texture",
  "prior_texture_publication", "refresh_texture", "copy_color", "capture",
  "lifecycle", "drain", "resource_fence", "backpressure", "serial_frame" };
 return names[reason];
}
inline const char *KindName(unsigned int kind)
{
 static const char *names[KindCount] = { "producer_wait", "enqueue", "owner_packet",
  "owner_idle", "backend_present", "dxgi_present", "startup", "shutdown" };
 return names[kind];
}
struct Anchor
{
 Tick before, utc100ns, after;
 void capture() { LARGE_INTEGER qpc; FILETIME utc;
  before = QueryPerformanceCounter(&qpc) ? static_cast<Tick>(qpc.QuadPart) : 0;
  GetSystemTimeAsFileTime(&utc);
  utc100ns = (static_cast<Tick>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
  after = QueryPerformanceCounter(&qpc) ? static_cast<Tick>(qpc.QuadPart) : 0; }
};
// Construct before worker startup. Producer and owner each exclusively write
// their own lane; dump only after worker join. A session owns all its storage.
class Session
{
public:
 Session() : frequency(0), dumped(false), dumpSucceeded(false)
 {
  LARGE_INTEGER value; if (QueryPerformanceFrequency(&value)) frequency = value.QuadPart;
  start.capture(); producer.threadId = GetCurrentThreadId();
  char cap[16] = {0}; const DWORD length = GetEnvironmentVariableA("RTS_MENU_TRACE_CAP", cap, sizeof(cap));
  if (length > 0 && length < sizeof(cap)) { char *end = 0; unsigned long requested = strtoul(cap, &end, 10);
   if (*end == '\0' && requested > 0 && requested <= MaxEvents)
    producer.limit = owner.limit = static_cast<unsigned int>(requested); }
  directory[0] = '\0'; const DWORD pathLength = GetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory, sizeof(directory));
  if (!pathLength || pathLength >= sizeof(directory)) directory[0] = '\0';
 }
 Lane producer, owner; Tick frequency; Anchor start, finish; bool dumped, dumpSucceeded;
 void dumpAfterJoin()
 {
  if (dumped) return; dumped = true; finish.capture();
  if (!directory[0] || producer.depth || owner.depth) return;
  char path[MAX_PATH]; int length = snprintf(path, sizeof(path), "%s\\menu-critical-path-%lu-%llu.csv",
   directory, GetCurrentProcessId(), start.before);
  if (length <= 0 || length >= sizeof(path)) return;
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
  if (file == INVALID_HANDLE_VALUE) return;
  bool ok = true; char row[1024];
  length = snprintf(row, sizeof(row), "# TEMP inclusive CPU wall ticks; nested scopes overlap; owner execution includes backend, not GPU time\n# frequency=%llu,start_qpc_before=%llu,start_utc_filetime_100ns=%llu,start_qpc_after=%llu,end_qpc_before=%llu,end_utc_filetime_100ns=%llu,end_qpc_after=%llu\n",
   frequency, start.before, start.utc100ns, start.after, finish.before, finish.utc100ns, finish.after);
  write(file, row, length, ok);
  const char *header = "row,lane,kind,reason,begin_qpc,end_qpc,sequence,last_sequence,packet,control,recording,commands,bytes,depth,result,calls,ticks\n";
  write(file, header, static_cast<int>(strlen(header)), ok);
  for (unsigned int laneIndex = 0; laneIndex < 2; ++laneIndex)
  {
   Lane &lane = laneIndex == 0 ? producer : owner;
   const char *name = laneIndex == 0 ? "producer" : "owner";
   length = snprintf(row, sizeof(row), "# lane=%s,thread=%lu,cap=%u,events=%u,dropped=%llu,qpc_failures=%llu,final_depth=%u\n",
    name, lane.threadId, lane.limit, lane.used, lane.dropped, lane.qpcFailures, lane.depth);
   write(file, row, length, ok);
   for (unsigned int i = 0; i < lane.used; ++i)
   {
    const Event &e = lane.events[i]; const Context &c = e.context;
    length = snprintf(row, sizeof(row), "event,%s,%s,%s,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%u,%d,,\n",
     name, KindName(e.kind), ReasonName(e.reason), e.begin, e.end,
     c.sequence, c.lastSequence, c.packet, c.control, c.recording, c.commands, c.bytes, e.depth, e.result);
    write(file, row, length, ok);
   }
   for (unsigned int k = 0; k < KindCount; ++k) for (unsigned int r = 0; r < ReasonCount; ++r)
   {
    const Aggregate &total = lane.totals[k][r]; if (!total.calls) continue;
    length = snprintf(row, sizeof(row), "total,%s,%s,%s,,,,,,,,,,,,%llu,%llu\n",
     name, KindName(k), ReasonName(r), total.calls, total.ticks);
    write(file, row, length, ok);
   }
  }
  if (!CloseHandle(file)) ok = false; dumpSucceeded = ok;
 }
private:
 char directory[MAX_PATH];
 static void write(HANDLE file, const char *row, int length, bool &ok)
 { if (length <= 0 || length >= 1024) { ok = false; return; }
  DWORD written = 0; if (!WriteFile(file, row, length, &written, 0) || written != static_cast<DWORD>(length)) ok = false; }
 Session(const Session &); Session &operator=(const Session &);
};
} }
#else
namespace rts { namespace menu_trace {
inline bool Enabled() { return false; }
#if defined(_MSC_VER)
typedef unsigned __int64 Tick;
#else
typedef unsigned long long Tick;
#endif
struct Context { Context() : sequence(0), lastSequence(0), packet(0), commands(0), bytes(0), control(0), recording(0) {}
 Tick sequence, lastSequence, packet, commands, bytes; unsigned int control, recording; };
struct Lane { Context context; unsigned long threadId; };
class Bind { public: explicit Bind(Lane *) {} };
class Session { public: Lane producer, owner; void dumpAfterJoin() {} };
class ReasonScope { public: explicit ReasonScope(Reason, bool = false) {} };
class Scope { public: explicit Scope(Kind, Lane * = 0) {} void result(int) {} };
} }
#endif
#endif
