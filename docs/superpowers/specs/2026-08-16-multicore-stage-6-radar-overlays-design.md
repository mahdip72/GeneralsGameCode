# Stage 6: Radar Object and Shroud Overlay Preparation Design

**Date:** 2026-08-16  
**Status:** Design for implementation  
**Scope:** Radar object-overlay and batched shroud-overlay CPU preparation,
stacked on the completed Stage 5 terrain preparation boundary

## Goal

Move the CPU work that prepares the radar object and batched shroud overlays
into the bounded, synchronous render-preparation fork/join introduced by
Stage 5.  The owner thread continues to own all live game, radar, shroud,
Direct3D, and texture state.  It captures immutable scalar/POD commands,
workers process disjoint output rows, and the owner performs every D3D upload.

The output must be byte-for-byte equivalent to the existing implementation.
In particular, object commands must retain the exact `m_objectList` followed by
`m_localObjectList` order, each object's four-pixel footprint must retain its
existing clipping and write order, and later writes must continue to overwrite
earlier writes at the same pixel.  Batched shroud rectangles must likewise be
applied in their original call order.  The direct, non-batched shroud path
remains synchronous and owner-only.

This is render preparation only.  It must not change simulation, AI,
pathfinding, network, replay, save/load, RNG, frame timing, texture dimensions,
or the visual scheduling decisions that select when the radar is refreshed.

## Non-goals and hard constraints

- Do not parallelize `Radar::update`, `Radar::newMap`, `Radar::refreshTerrain`,
  `W3DRadar::draw`, view-box reconstruction, game logic, or shroud logic.
- Do not move `RadarObject`, `Object`, `Player`, `W3DShroud`, `SurfaceClass`,
  `TextureClass`, `The*` globals, or any other live engine pointer to a worker.
- Workers receive only an owner-owned immutable command snapshot and a pointer
  to an owner-owned output buffer.  They do not call D3D, `The*` globals,
  `GameMakeColor`, `ARGB_Color_To_WW3D_Color`, logging, allocation, waits, RNG,
  replay/save/network code, or title code.
- Each worker writes only rows in its assigned half-open range.  It may scan
  the complete ordered command array, but it may not write outside its rows or
  mutate command state.
- Keep the implementation C++98 and compatible with the VC6/Win32 lane.  Use
  checked size arithmetic and owner-side `try`/`catch (...)` allocation helpers;
  do not depend on newer standard-library containers or array allocation
  overloads.
- Use the Stage 5 render-preparation runtime and exclusive lease.  Do not add a
  second worker pool, a background queue, a callback, an unbounded queue, or an
  overlapping consumer batch.  Object and shroud batches must acquire and
  release the lease independently and synchronously.
- Preserve the existing `SurfaceClass::Clear`, lock, pitch, unlock, release,
  and texture lifetime semantics.  A worker never holds a D3D lock and never
  sees a surface pointer.
- Preserve the established configured profile and replay compatibility.  No
  stage-specific local machine paths, build paths, profile paths, or generated
  logs belong in source, documentation, commits, or the PR.

## Existing output contracts

### Object overlay

`W3DRadar::updateObjectTexture` currently clears the overlay surface and then
calls `renderObjectList` twice:

1. traverse `m_objectList` from head to tail;
2. traverse `m_localObjectList` from head to tail.

`renderObjectList` evaluates visibility on the owner, computes a radar point
from the object's position and `m_mapExtent`, obtains the object's color, and
adjusts alpha for stealth using the current logic frame.  It converts the final
ARGB color to the selected surface format and writes this footprint in order:

```text
(x, y), (x, y + 1), (x + 1, y + 1), (x + 1, y)
```

Each write is accepted only when `legalRadarPoint` accepts the coordinate.  A
later command overwrites an earlier command if their footprints overlap.  The
new implementation must preserve all of these facts, including integer
conversion behavior, frame-based stealth alpha, format packing, clipping, and
the list-order boundary between the two lists.

The owner therefore creates one flattened command array in exactly that order.
The command contains the final packed pixel value and the base x/y; workers do
not recompute visibility, color, alpha, coordinate conversion, or format
packing.

### Batched shroud overlay

`W3DRadar::setShroudLevel` computes a map-cell rectangle, converts its corners
with `worldToRadar`, selects alpha from the shroud status, and writes an
inclusive rectangle.  The existing batched API is:

```text
beginSetShroudLevel()
setShroudLevel(...)         // one or more calls, in game-thread order
endSetShroudLevel()
```

The non-batched path (`m_shroudSurface == nullptr`) locks, writes, unlocks, and
releases synchronously for each call.  It remains unchanged except for narrow
owner-side helpers shared with the batched path.

For the batched path, the owner must capture the current visible shroud bytes
after the existing clear/initialization state and record every rectangle and
its already-packed pixel in call order.  Workers apply all commands to their
own disjoint rows in command order.  This preserves the existing last-command-
wins behavior for overlapping rectangles while avoiding concurrent writes to a
shared row.  The owner locks and uploads the completed rows only after the
fork/join has completed.

If capturing the current bytes is not possible without changing the existing
surface semantics, implementation must retain the legacy owner-only batched
path for that failure; it must not assume that a format's clear value is a
particular byte pattern.  A D3D surface must never be used as a worker output
buffer.

## Ownership model

### Owner phase

The owner performs all of the following before admitting a task:

1. Assert the game/render-thread boundary and validate texture dimensions,
   format, bytes per pixel, pitch, row size, command count, and the fixed
   staging budget.
2. For object preparation, traverse `m_objectList` and then
   `m_localObjectList`, preserving linked-list order.  Call
   `canRenderObject`, read object/player/status/team/frame data, calculate the
   radar coordinate, stealth alpha, and packed surface color, then append a
   plain command.  No live pointer is retained.
3. For batched shroud preparation, compute the inclusive radar rectangle and
   packed color for each call, copy the current visible surface rows into an
   owner buffer, and append commands in the order received.  Direct shroud
   calls bypass this batch and remain synchronous.
4. Acquire the Stage 5 render-preparation lease with a distinct consumer ID.
   If the lease cannot be acquired, use the owner serial reference path.
5. Submit exactly two disjoint row tasks through the existing all-or-none
   bounded runtime and wait for that batch only.  No task may survive the
   owner's return, upload, release, reset, or shutdown.
6. Lock the D3D surface only on the owner, copy tight staged rows while
   honoring the actual pitch, unlock, release, and then release the lease.

The object and shroud snapshots, command arrays, and output buffers are owned
by the active owner batch.  They are reclaimed only after tasks have joined.

### Worker phase

An object row task scans the complete immutable ordered object-command array
and applies the four fixed offsets only when an offset belongs to its assigned
rows and the radar coordinate is legal.  A shroud row task scans the complete
immutable ordered rectangle-command array and applies each inclusive rectangle
to its assigned rows.  Both tasks write only to their output row range and
return without logging, allocation, synchronization, or engine access.

The apparent repeated scan is intentional: it preserves ordered overwrite
semantics while giving each row exclusive write ownership.  Reordering commands
or partitioning commands between workers would change pixels where footprints
or rectangles overlap.

## Immutable POD snapshots

The final names may follow repository conventions, but the data contract must
be equivalent to the following C++98-safe shapes:

```cpp
struct RadarObjectOverlayCommand {
    Int x;
    Int y;
    unsigned packedColor;
};

struct RadarObjectOverlaySnapshot {
    unsigned width;
    unsigned height;
    unsigned bytesPerPixel;
    unsigned formatCode;
    unsigned rowBytes;
    unsigned commandCount;
    const RadarObjectOverlayCommand *commands;
    unsigned char *output;
};

struct RadarShroudOverlayCommand {
    Int minX;
    Int minY;
    Int maxX;
    Int maxY;
    unsigned packedColor;
};

struct RadarShroudOverlaySnapshot {
    unsigned width;
    unsigned height;
    unsigned bytesPerPixel;
    unsigned formatCode;
    unsigned rowBytes;
    unsigned commandCount;
    const RadarShroudOverlayCommand *commands;
    unsigned char *output;
};
```

The output buffers contain tight visible rows.  For object preparation, the
owner initializes them from the exact cleared overlay surface state before
workers run.  For batched shroud preparation, the owner copies the exact
current surface bytes before applying the ordered rectangles.  A test-only
serial kernel must accept the same snapshots and produce the reference output.
No command or snapshot field may contain a live object, list, player, surface,
texture, allocator, mutex, task, callback, or global pointer.

The implementation may use one shared overlay snapshot type when its fields are
identical, but the object and shroud command types should remain distinct so a
future change cannot accidentally apply object footprint rules to shroud
rectangles.

## Pure row kernels

Provide D3D-free helpers in a shared library/testable boundary, for example:

```cpp
bool PackRadarObjectRows(const RadarObjectOverlaySnapshot &snapshot,
                         unsigned rowBegin, unsigned rowEnd);
bool PackRadarShroudRows(const RadarShroudOverlaySnapshot &snapshot,
                         unsigned rowBegin, unsigned rowEnd);
```

The kernels must:

- reject invalid dimensions, ranges, formats, strides, command pointers, and
  output pointers without writing;
- preserve the output's initial bytes for pixels untouched by commands;
- scan commands from index 0 through `commandCount - 1` for every assigned row;
- apply object offsets in the exact four-write sequence and use the same legal
  coordinate predicate as the legacy code;
- apply shroud rectangles inclusively and defensively clip malformed synthetic
  bounds to the valid output rectangle.  Production parity still comes from
  the owner-side `worldToRadar` clamp because legacy `Draw_Pixel` itself does
  not perform clipping;
- write exactly `bytesPerPixel` bytes using a D3D-free little-endian helper;
- perform no allocation, wait, lock, log, global read, RNG, or exception path.

The serial oracle invokes each kernel over `[0, height)`.  The two-worker
execution invokes it over the two row ranges.  Every output byte, including
untouched/clear bytes and row guard bytes in tests, must match.

## Bounded runtime and lease reuse

Stage 6 must reuse the Stage 5 `RenderPrepareRuntime`/lease boundary.  If the
Stage 5 implementation still has a terrain-specific adapter, extract only the
generic row-batch operation needed by both consumers and adapt terrain to it;
do not create a second pool.  The shared service remains:

- bounded and preallocated where practical;
- capped at two workers;
- synchronous and private to the active lease;
- all-or-none for the two row tasks;
- `2 workers -> 1 worker -> serial` on startup, admission, allocation, or
  submission failure;
- drained and joined before display/device teardown;
- idempotent on initialization and shutdown.

Suggested consumer IDs are distinct for terrain, object overlay, and shroud
overlay.  The service rejects nested or overlapping leases.  Object and shroud
operations must never wait on each other's buffers or tasks.  If the shared
lease is unavailable, the owner executes the pure serial kernel or the exact
legacy direct path, and then returns synchronously.

## Failure and lifecycle policy

| Failure | Required behavior |
| --- | --- |
| Object/shroud snapshot allocation failure | Do not submit partial data; use the existing owner-only reference path. |
| Current surface capture/lock failure | Unlock any successful lock, release the surface, and use the unchanged owner path. |
| Lease unavailable or runtime stopped | Run the complete serial kernel or direct owner path; do not queue work. |
| Worker/task allocation or all-or-none submission failure | Reclaim caller-owned task wrappers and run the complete serial kernel over the finished snapshot. |
| Two-worker start failure | Retry the same two row ranges with one worker; if that fails, run serial. |
| Worker output validation failure | Do not upload partial rows; use the owner reference path. |
| Reset/new map/display shutdown | Finish or serially complete the active owner batch before release; shutdown then drains and joins the shared runtime. |
| Malformed begin/end sequence | Preserve the existing owner assertion/fallback behavior and never leave a D3D lock or lease active. |

No fallback may upload uninitialized or partially prepared bytes.  Every
exceptional owner path releases surfaces, snapshots, and leases exactly once.

## Proposed implementation boundaries

The implementation should keep pure kernels independent of D3D and keep live
capture/upload logic in the game-engine device layer:

- `Core/Libraries/Include/Lib/RadarOverlayKernel.h` and
  `Core/Libraries/Source/TaskRuntime/RadarOverlayKernel.cpp`: POD command
  definitions, validation, row kernels, clipping, and packed-byte writes.
- `Core/GameEngineDevice/Include/W3DDevice/Common/RadarOverlayPrepare.h` and
  `Core/GameEngineDevice/Source/W3DDevice/Common/System/RadarOverlayPrepare.cpp`:
  owner batch storage, checked budgets, row-task adapters, and overlay service
  glue over the Stage 5 runtime.
- `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp` and
  `Core/GameEngineDevice/Include/W3DDevice/Common/W3DRadar.h`: object list
  capture, overlay upload, `beginSetShroudLevel`/`setShroudLevel`/
  `endSetShroudLevel` ownership, and direct-path preservation.
- `Core/Tools/RadarOverlayPrepareTest/RadarOverlayPrepareTest.cpp` plus its
  CMake registration: D3D-free object/shroud serial-vs-split tests, command
  ordering, clipping, clear-byte preservation, and failure seams.
- The shared GameEngineDevice and tool CMake source lists: register only the
  new files and the existing Stage 5 runtime adapter.
- `Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`
  and the GeneralsMD counterpart only if source inspection proves that the
  shared runtime lifecycle needs a title-specific owner hook.  Do not duplicate
  a runtime or alter unrelated display teardown.
- `TESTING.md`: repository-relative focused build, test, review, replay, and
  deferred manual-test instructions.

## Compatibility and scope audit

Before implementation, verify the exact current code and preserve:

1. `updateObjectTexture` clear timing and the two list traversal boundaries;
2. `canRenderObject` visibility, player relationship, stealth, and local-only
   rules;
3. radar coordinate conversion and legal point behavior at all map edges;
4. the frame-based stealth alpha calculation and final surface format packing;
5. the four object writes and their order;
6. shroud rectangle inclusivity, alpha selection, corner conversion, and
   batched call ordering;
7. `beginSetShroudLevel`/`endSetShroudLevel` lock and reset invariants;
8. both Generals title variants and their destruction order;
9. Stage 5's lease initialization, shutdown, and fallback behavior;
10. unchanged replay/save/network/RNG code and unchanged Xfer formats.

If a source audit finds that `Draw_Pixel` has format- or clipping-specific
behavior not captured by the pure helper, add that behavior to the owner-built
POD contract and focused oracle before writing worker code.  Do not “simplify”
the legacy path based on an assumed pixel format.

## Validation and delivery gates

Stage 6 is ready for user testing only when all of the following are true:

- the red/green focused tests cover object and shroud behavior before and after
  implementation;
- modern Win32 Debug/Release builds pass for both `g_generals` and `z_generals`;
- the VC6/Win32 lane builds both titles and the focused target without using a
  modern executable as replay evidence;
- focused tests, existing task-runtime tests, and existing texture tests pass;
- source audits show no worker D3D/global/live-pointer/allocation/wait/RNG/
  replay/save/network access;
- serial and two-row outputs match for all supported formats, edge clipping,
  overlapping object footprints, overlapping shroud rectangles, and untouched
  clear bytes;
- the complete ten-unique-replay gate passes as twelve executions: nine
  regular corpus replays once each and the 2v6 Hard-AI stress replay three
  times.  Every process exits zero, with no CRC mismatch, out-of-sync,
  ownership error, crash, or stale worker; the three stress CRC traces are
  byte-identical;
- ten distinct review rounds have examined the PR and all material findings
  have been fixed and revalidated;
- the PR contains only repository-relative design, source, test, and validation
  material, targets the intended base branch, and remains unmerged;
- interactive/manual game testing remains deferred until the complete stacked
  Stage 5--8 candidate is ready and the user authorizes the Stage 5 launch.

This design intentionally leaves the canonical playable installation and the
configured user profile untouched.  Promotion and interactive launch are not
part of Stage 6 implementation.
