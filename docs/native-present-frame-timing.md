# Native Present event timing

Set `RTS_PRESENT_FRAME_TIMING_DIR` in the candidate process to an existing
absolute directory to enable this private D3D11 owner diagnostic. It is separate
from `RTS_GPU_FRAME_TIMING_DIR`: GPU-query slot availability, readiness, query
allocation failure, and GPU record capacity cannot suppress native Present events.
The configured directory must fit the bounded Windows path buffer. Missing or
invalid configuration leaves the diagnostic off without changing game results.
No directories, global environment/settings, or game policy are changed.

With both diagnostics off, no records/queries or Present clocks are allocated or
sampled. With only GPU timing enabled, its previous clock path is unchanged.
With Present timing enabled, exactly two QPC calls and one frequency read bracket
each actual swapchain Present call, whether GPU sampling is enabled, skipped, or
unavailable. An available GPU record receives those same values without additional
clock calls. Configuration samples one owner-session start QPC after successful
allocation. Native Present failures and positive statuses still create events.
Begin/end render intervals that never call native Present create none.

## Records and bounds

The owner allocates a fixed prefix of 32768 records once (about 2.25 MiB). Recording
does not allocate, write files, poll/flush queries, wait, or alter native call
ordering. After capacity, earlier records are preserved, `dropped` increments,
and aggregate call/outcome/invalid-clock counters continue. Do not interpret the
retained prefix as complete throughput when events were dropped. Hidden render
passes do not consume Present-event capacity.

The 18-column CSV schema is:

```text
row_type,owner_session,process_id,owner_thread_id,owner_start_qpc,device_epoch,backend_frame_ordinal,present_ordinal,present_start_qpc,present_end_qpc,cpu_qpc_frequency,present_hresult,swap_interval,present_flags,width,height,status,count
```

`row_type=present` is one actual native call. `present_ordinal` counts calls in the
owner session, not logic frames. `device_epoch` advances on successful native
device attachment/recreation. `backend_frame_ordinal` counts accepted backend
begin calls within that epoch, including hidden or unsampled GPU frames, and can
associate an event with a retained GPU frame's epoch/local ordinal when available.
Zero means no associated backend begin. It is not the threaded sequence or
simulation frame. Resize without device recreation does not create an epoch.
An explicit shutdown/reinitialize starts a new owner session, clean counters and
ordinals; device recovery retains earlier events in the same session.

Process/thread IDs and owner session/start QPC identify the writing owner. All
recording/lifecycle calls are private to the existing native owner path; no
producer thread writes this buffer. Foreign-thread export refuses without
discarding retained owner data. A controller manifest must bind the published CSV
hash, these owner/process/session fields, source revision/hash, executable hash,
run configuration, and externally recorded benchmark phase QPCs. This diagnostic
does not claim to discover or verify source/executable hashes itself.

`present_hresult` is the raw native call result, not a later device-removal
check. `status` is s_ok, occluded, positive, or failed. S_OK acceptance does not
prove display scanout, and neither foreground nor a normal process exit proves
continued successful presentation. Keep failure/occlusion and visible-frame gate
qualification independent of timing.

QPC fields preserve raw zero/failure data. Valid call timing requires positive
start/end/frequency and end >= start. Equal positive ticks are valid zero duration.
Summary `invalid_clocks` includes invalid calls even beyond capacity. Matching
frequency and monotonic completion ticks permit exact CPU-side completion cadence:
`1000*(end[i]-end[i-1])/frequency` milliseconds. Use qualifying completion counts
in an externally delimited `[measurement_begin,measurement_stop)` QPC interval
divided by wall seconds for throughput. Report per-second rates and long gaps,
not just an average. CPU call duration is wall time, not CPU utilization; GPU
elapsed timestamps remain elapsed rather than busy time.

## Export and qualification

Only normal owner shutdown opens a unique present-frame-timing-*.csv.pending
file using CREATE_NEW. Header, every event/summary, stream status, flush, close,
and non-overwriting MoveFileW publication are checked. A failure never becomes a
game/render error; it leaves no valid completed export, and a staging file may be
retained. Recording and recovery perform no file I/O. Forced termination may
produce no export. Only published .csv files qualify, not .pending files, even
if a staging footer exists.

Summary rows keep owner metadata, use `status` as counter name and final `count`
as value, and leave event columns empty. Counters are capacity, calls, retained,
dropped, s_ok, positive, failed, invalid_clocks, io_failures, export_end. Require
schema/owner identity and `export_end=retained=parsed event rows`,
`calls=retained+dropped`, and `calls=s_ok+positive+failed`. No missing/invalid
clock or dropped event may silently enter a qualified complete window. GPU skip
counters still qualify GPU-sample coverage but no longer invalidate complete
independent Present-event coverage. Source failures that suppress presentation
are not repaired or hidden by this diagnostic.

`core_native_present_frame_timing_tests` executes the production collector and
shared callsite clock helpers with recording drivers, including off/GPU/Present/
combined budgets, query-full/unavailable/cap independence, unpresented intervals,
raw native outcomes, lifecycle ordinals, bounded overflow, invalid clocks, and
checked export failure injection. It also exercises the production Windows
configuration/export under GetTempPath (controller TEMP/TMP selects the task run),
checking no file during recording, owner-only shutdown, exact physical CSV schema
and metadata, one export, and exact owned-file cleanup. No GPU is required.
These tests are not visual acceptance, GPU performance, or FPS proof; build and
runtime validation must use the exact integrated candidate separately.
