# Stage 8: Dynamic Terrain-Light Preparation Design

## Goal

Stage 8 moves only the CPU calculation of dynamic terrain-light diffuse values
off the render owner thread. The owner still discovers lights, captures all
inputs, locks and unlocks Direct3D vertex buffers, applies diagnostics, and
publishes dynamic values to hardware. The backup vertex array remains the
immutable static-light/geometry baseline. The serial path remains the
authoritative fallback whenever capture, admission, worker execution,
validation, or publication is not safe.

## Scope and invariants

- The change applies symmetrically to Generals and Zero Hour through the shared
  `Core/GameEngineDevice` implementation.
- No worker may dereference `The*` globals, `WorldHeightMap`, `W3DDynamicLight`,
  `DX8VertexBufferClass`, `VERTEX_FORMAT`, scene iterators, allocators,
  diagnostics, replay state, network state, logging, or UI/audio objects.
- The worker receives an immutable owner-created snapshot and writes only a
  disjoint range of an owner-created staging array. A worker never waits for a
  sibling task.
- Lighting arithmetic, truncation, alpha preservation, light order, clipping,
  wrap handling, and duplicate-vertex propagation must match the existing
  `updateVBForLightOptimized` result byte-for-byte. Duplicate vertices are
  propagated deterministically by the owner after all row tasks complete.
- Dynamic-light state (`m_processMe`, previous/current bounds, and prior-enable
  state) is updated only on the owner before capture. Stage 8 does not change
  simulation or replay serialization and does not parallelize A* or path
  queue processing.
- Existing `RadarTerrainPrepareService` is the only worker runtime. Stage 8
  uses a distinct consumer id and its existing lease, bounded queue, two-worker
  split, one-worker retry, join-before-release, and shutdown semantics.
- A small/empty/invalid batch, unavailable service, rejected task, failed
  validation, failed D3D lock, or any runtime fault immediately runs the
  unchanged serial implementation for that tile. No partial staged output is
  published.

## Data flow

`On_Frame_Update` performs the existing owner-side light discovery and tile
intersection checks. For each affected tile, it copies the tile's backup
vertices (position and alpha-bearing diffuse), computes the four corner
normals from owner-owned map reads, and snapshots the enabled dynamic scene
lights into a `HeightMapDynamicLightBatch`. Static/global terrain lighting is
already baked into the immutable backup diffuse and is not sampled again by
this dynamic pass. Each captured vertex also records whether the legacy
current/previous light bounds require a hardware update, preserving serial
per-cell clipping. The batch records row stride, output capacity, and bounded
batch bytes for structural validation.

The pure kernel computes one tile row range. It starts from the captured
backup diffuse value, applies the captured light list in the legacy order, and
writes RGB while preserving the captured alpha. A row task writes only its
own rows. After `runRows` joins all accepted tasks, the owner validates every
staged vertex and only then locks the D3D buffer. It scatters only vertices
whose captured bounds require an update, unlocking before returning. The
default non-optimized vertex layout has no shared hardware vertices; builds
using the separate optimized lighting macro remain on the unchanged serial
path that owns duplicate propagation. Writing dynamic values into the backup
would compound lighting across frames because the legacy implementation reads
that backup as its baseline on every frame.

The owner never exposes live pointers to the worker. Batch storage is stack or
owner-owned and remains alive through the synchronous service call. The
service lease is released only after the tile output has been consumed.

## Interfaces

The pure library kernel defines POD-only input/output types and functions:

- `HeightMapDynamicLightSnapshot` — dimensions, row stride, copied vertices,
  dynamic scene lights, and finite bounds.
- `HeightMapDynamicLightVertex` — copied position, alpha-bearing diffuse,
  precomputed normal, and a legacy bounds-update mask.
- `PrepareHeightMapDynamicLightRows(snapshot, output, yBegin, yEnd)` — pure
  row-range calculation returning `bool`.
- `ValidatePreparedHeightMapDynamicLightOutput(snapshot, output)` — complete
  finite/range/sentinel and lighting-oracle validation for focused tests.
- `ValidatePreparedHeightMapDynamicLightStructure(snapshot, output)` — the
  owner-side publication check; it avoids replaying the lighting arithmetic
  after worker completion.

The common adapter owns allocation/capture helpers and implements
`RadarPrepareRowWork::executeRows`. It must not allocate from a worker. The
render owner calls one `updateVBForLightWithPreparation` helper from
`On_Frame_Update`; the existing `updateVBForLight` remains available as the
serial fallback and as the byte-reference implementation used by tests.

## Failure and lifecycle handling

The service is initialized and shut down with the shared display lifecycle in
both titles. Shutdown first prevents new leases, joins accepted work, then
clears service state before height-map buffers and D3D resources are destroyed.
Reset/reacquire paths use the same owner-side drain before touching vertex
storage. No task may outlive the batch, map, light list, backup buffer, or
runtime lease.

If a two-worker start or submission fails, the service retries once with one
worker; if that also fails, the caller executes the serial tile. If a batch
allocation fails, the tile executes serially without changing existing
buffers. Validation rejects NaN/Inf, impossible dimensions/strides, overflowed
address ranges, malformed light types/ranges, or incomplete sentinel output.
Interactive display initialization warms the private workers before the first
dynamic-light frame; headless replay defers worker creation because it never
renders terrain. One-row and other tiny batches stay serial so the bounded
two-row submission does not pay an empty-stripe/event cost.
At exact point/spot/light coincidence the worker keeps a zero direction and
finite ambient-only result; this deliberately avoids the legacy divide-by-zero
NaN while preserving deterministic replay-safe output.
A D3D lock or hardware publication failure leaves the backup untouched and
immediately executes the unchanged serial path.

## Test strategy

The focused core-extra test compares serial and split execution for point,
spot, directional, enabled/disabled, attenuated, clipped, and masked cases.
It checks alpha and guard bytes, malformed/overlap/alignment/overflow
rejection, and unchanged destinations on failure. A service fixture exercises
the two-row split, including its empty second stripe; the existing RadarTerrain
tests remain the worker-identity, rejection, retry, and shutdown gates.

Both modern x86 titles and the VC6/C++98 lane must compile the shared kernel
and common adapter. The complete replay corpus remains a required determinism
gate; Stage 8 must produce no replay-marker or CRC-policy changes.

## Explicit non-goals

Stage 8 does not change pathfinding capacity, movement scheduling, dynamic
object simulation, rendering submission order, D3D ownership, audio, replay
serialization, or map-cache parsing. Those areas require separate designs.
