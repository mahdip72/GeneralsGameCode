# Native D3D11 GPU frame timing

Set `RTS_GPU_FRAME_TIMING_DIR` in the candidate process environment to an existing
directory to enable this private backend diagnostic. With the variable absent,
the collector allocates no record storage or GPU queries and makes no frame
clock or query calls. It does not change render settings or frame outcomes.
The directory must fit within the Windows MAX_PATH environment buffer.

At normal renderer shutdown a unique gpu-frame-timing-*.csv.pending staging
file is opened with CREATE_NEW. Every header, frame and summary write, stream
error flag, flush and close is checked. Only successful completion is renamed
to .csv with MoveFileW, without replacing any existing file. A failed staging
file is retained for analysis; it is not a completed export. Frames and device
recovery perform no file writes. A crash or forced termination may leave no
export. Shutdown resets the collector; another initialization uses a new unique
file. All retained device epochs remain in the same export. Numeric fields use
a private C numeric locale without changing the process locale.

## What a frame row means

Four timestamps bracket backend beginFrame, the work before backbuffer
resolve, the work after resolve, and the boundary after optional presentation
gamma processing immediately before DXGI Present. The corresponding columns
are gpu_total_ms, gpu_scene_ms, gpu_resolve_ms, and gpu_gamma_ms.
cpu_present_ms independently measures the CPU wall time inside DXGI Present.
Skipped resolve or gamma passes still have adjacent query markers; their short
elapsed intervals include marker overhead. Inspect resolve_applied and
gamma_applied when interpreting them.

GPU timestamps measure elapsed time in the GPU command stream, not pure GPU
busy time. CPU starvation between split packets can leave idle time inside an
interval. Pair this output with CPU owner/producer timing and GPU scheduling
evidence before concluding that GPU execution limits performance. This
diagnostic has no per-pass particle or shadow attribution.

device_epoch increments when a native device is attached or recreated.
local_frame_ordinal counts accepted backend beginFrame calls within that
epoch, including non-visible frames. These values are **not** the threaded
accepted sequence, simulation frame, or presentation index. No exact join to
existing owner CSV sequence ranges is available from this diagnostic. Preserve
candidate identity and comparable capture windows when combining evidence.

owner_begin_tick_ms is sampled once, only when a record slot is reserved, from
the owner's GetTickCount-compatible 32-bit system-uptime clock. Refreshing
presentation settings does not replace this begin tick. It supports coarse
wall-window alignment to other GetTickCount markers, not an accepted-sequence
or simulation-frame join. The clock wraps after approximately 49.7 days and
typically has 10–16 ms resolution. For nearby windows use signed modulo-32
differences with separation less than 2^31 ms; do not interpret raw subtraction
across wrap as a large delay. Skipped samples have no tick row, so skip counters
also describe missing wall coverage. See [Microsoft GetTickCount documentation](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount).

The CSV has 29 columns. owner_begin_tick_ms follows gpu_elapsed_not_busy;
present_start_qpc, present_end_qpc, and cpu_qpc_frequency follow that tick;
count is always last. Frame rows leave count empty. Summary rows use status
for the numeric field name and count for its value, leaving frame columns empty.
Epoch zero identifies collector-wide counters. Positive device_epoch identifies
eight once-per-device identity fields: adapter_identity_valid,
adapter_vendor_id, adapter_device_id, adapter_luid_low, adapter_luid_high,
adapter_software, device_feature_level, and device_debug_layer. LUID halves are
unsigned 32-bit bit patterns. These are obtained from the actual attached D3D11
device and its DXGI adapter, not OS display-controller inventory or requested
settings. device_debug_layer reflects the device's actual creation flags.
Require adapter_identity_valid=1 before interpreting hardware versus software;
a missing identity never proves hardware rendering. Keep GPU-enabled captures
separate from Main/Owner/Off telemetry modes unless independently matched.

The three Present clock fields retain the existing CPU clock readings around
DXGI Present and the frequency already used for cpu_present_ms. They add no
clock calls. New retained/unpresented records start with zero fields; a failed clock or
frequency read remains zero, not a valid fast timing. Require present_called=1,
both ticks positive, end >= start, and cpu_qpc_frequency > 0 before interpreting
that interval. Equal positive ticks are a valid zero-duration interval.
cpu_present_ms remains -1 when the clock interval cannot be calculated.
Retained ticks survive GPU query loss and a failed Present result; validate
presentation success independently of timing availability.

For a qualified visible-window workload, present_called=1 and present_hresult=0
count retained S_OK native Present calls, even when GPU query status is not
complete. These are submitted presentations, not scanned-out/displayed frames.
With matching positive frequencies, consecutive valid present_end_qpc values
give exact CPU-side Present-completion cadence; present_start_qpc values give
call-start cadence. Use 1000 * tick_difference / cpu_qpc_frequency for
milliseconds. Windowed combat throughput is the count of qualifying completion
ticks in an externally delimited QPC interval divided by its wall seconds.
Reject nonmonotonic ticks, failed/occluded calls, and invalid windows rather than
presenting them as normal visible throughput. QPC permits wall-time alignment
to a pipeline trace using the same clock, but provides no exact threaded-sequence
or GPU-ordinal association.

Frame rows retain actual scene sample count, width/height, gamma, brightness,
contrast, gamma limit setting, whether gamma processing ran, swap interval,
presentation flags, and the presentation result. present_called=0 means the
sample never reached DXGI Present. readback_contaminated=1 marks a valid
backbuffer readback attempt during an active sampled GPU interval; its staging
copy/Map can synchronize the GPU. Exclude these rows from ordinary performance
comparisons. beforePresent closes that interval. Later readbacks, including
those after presentation, do not mark an earlier pending or completed row.
Every enabled valid readback attempt still increments readbacks, including
attempts outside a sampled interval and after cancellation.

Only status=complete, present_called=1, present_hresult=0, and
readback_contaminated=0 rows are ordinary successful clean samples. Durations
that were not collected are -1, never zero. A failed presentation can have
collected GPU durations but remains present_failed. An unpresented row cannot
distinguish an intentional render-to-texture frame from a suppressed/failed
frame whose backend present was never called.
A known present_failed status also survives subsequent unresolved-query loss
at resize, device release, shutdown, or GetData failure; the corresponding
query-loss counter remains separate. Missing durations stay -1.

## Bounds and lifecycle

The collector preallocates eight slots, each containing one disjoint query and
four timestamps, and at most 8192 records. Every backend begin polls with at
most 16 GetData calls. Calls run on the existing owner and always use
D3D11_ASYNC_GETDATA_DONOTFLUSH. Not-ready data is deferred; there is no spin,
wait, sleep, flush, or GPU fence in the diagnostic. Existing renderer lifecycle
flushes remain existing renderer behavior.

At most 64 scalar device-epoch identities are retained. No frame or resize
recollects device identity. device_metadata_failures counts retained identities
whose adapter inspection failed, and device_metadata_dropped counts subsequent
attachments beyond that cap; earlier identities are never overwritten.

A full ring increments skipped_full. Exhausted record storage increments
skipped_cap without overwriting earlier records. Query or record allocation
failure increments allocation_failures and leaves later frames
skipped_unavailable. A query read failure increments readiness_failures,
drops pending rows, and disables collection until a subsequent resize/device
attachment recreates the query pool. These failures never become render errors.
Skipped begins have no Present clock row even when the game subsequently calls
Present. A published CSV with nonzero skipped_full, skipped_cap, or
skipped_unavailable cannot prove complete Present totals. Preserve those counts
and capture-window coverage alongside throughput; the precise clock fields do
not repair missing records.

An ordinary cancellation ends its disjoint query and keeps the slot pending
until its result is ready; in-flight query objects are not immediately reused.
Resize explicitly drops pending results and allocates a fresh query pool.
Device release drops pending results before the context/device is released.
Normal shutdown does not poll or drain for late results and marks them
shutdown_pending. Collected earlier rows remain available across these events.

Summary rows provide begun/retained/complete counts, full/cap/unavailable skips,
not-ready reads, budget exhaustion, cancellations, invalid/disjoint samples,
allocation/readiness failures, resize/device/shutdown losses, readback attempts,
and output errors known before the footer. export_end records the intended row
count; it is necessary but not proof of a successful export. Later flush, close,
or rename failures can leave that footer, and an earlier io_failures=0 summary,
in a failed .pending file. Use only the published .csv, verify schema and frame
row count, and externally preserve its provenance. Publication is a checked CRT
flush/close and no-replacement rename, not a power-loss durability guarantee.
The [Microsoft MoveFileW contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefilew)
requires a non-existing destination. Drop counters and statuses must accompany
statistics so a partial set cannot masquerade as a complete fast workload.

## Focused validation

core_d3d11_gpu_frame_timing_tests executes the same collector with a recording
driver. It covers default-off behavior, interval arithmetic, settings/readback
retention, counter-only readbacks after interval closure, completed and cancelled
rows, unsampled ring-full frames, fixed capacity, readiness deferral and polling budget,
disjoint/invalid data, allocation/readiness failures, cancellation, resize,
device epochs, shutdown loss, and failed presentation followed immediately by
every query-loss lifecycle. It also tests owner-tick retention/wrap arithmetic,
Present clock retention without extra calls, exact start/end cadence, missing
and backward clocks versus valid equal ticks, skipped-frame clock absence,
29-column schema order with count last,
device-identity retention/failure/cap, and the production exporter decisions via
a recording output seam that injects open/header/frame/summary/stream/flush/
close/publication failures. This is not a duplicate exporter algorithm.
It requires no real GPU. These fixtures do not physically exercise native COM
adapter inspection or Windows/CRT filesystem failures such as disk exhaustion,
permissions, close failure, or a rename collision; those require separate
integration validation. No live GPU or native I/O test is implied by the seam.
Passing it does not establish GPU timestamp accuracy, visual acceptance, or an
FPS improvement. Build and runtime validation must use the exact integrated
candidate separately.
