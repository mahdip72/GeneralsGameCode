# Stage 6 Radar Object and Shroud Overlay Preparation Implementation Plan

> **For agentic workers:** Use strict test-driven development for each task.
> Keep all edits isolated to the Stage 6 worktree and use the completed Stage 5
> branch as the base.  Do not launch the interactive game for this intermediate
> stage.

**Goal:** Prepare radar object and batched shroud overlay pixels with bounded,
synchronous row fork/join while preserving exact legacy bytes and owner-thread
D3D/lifecycle ownership.

**Architecture:** The owner flattens the two object lists and batched shroud
updates into immutable ordered POD commands.  Two row workers scan the complete
command sequence and write only their disjoint rows.  The owner captures the
initial surface bytes, waits for the private Stage 5 render-preparation lease,
uploads the finished rows, and releases the lease.  Allocation, startup,
admission, or submission failure falls back to the complete serial oracle or
the unchanged owner-only path.

## Global constraints

- Preserve `m_objectList` then `m_localObjectList` order, linked-list order,
  four-pixel object write order, clipping, packed color, and last-writer-wins.
- Preserve shroud rectangle inclusivity, alpha selection, corner conversion,
  update call order, and the direct/non-batched synchronous path.
- Keep all D3D, texture, surface, game-object, player, shroud, and `The*` reads
  on the owner.  Workers see only immutable POD and owner-owned output bytes.
- Reuse the Stage 5 bounded runtime/lease.  There is one active render-
  preparation batch, at most two workers, and no background or unbounded queue.
- Use checked C++98/VC6-safe allocation and size arithmetic.  Workers allocate
  nothing and cannot outlive the owner batch or display shutdown.
- Keep simulation, AI, pathfinding, network, replay, save/load, RNG, and Xfer
  behavior unchanged.
- Keep all documentation, tests, commits, and PR text free of machine-specific
  paths and personal profile/build details.

## Task 1: Establish the baseline and red focused tests

**Files:**

- Add `Core/Tools/RadarOverlayPrepareTest/RadarOverlayPrepareTest.cpp`.
- Add/register its CMake target in the existing tools/test CMake files.
- Add only narrowly scoped shared test declarations if the existing Stage 5
  service needs an injectable row-runner seam.

**Steps:**

1. Read the current `W3DRadar.cpp`, `W3DRadar.h`, `Radar.h`, both title display
   implementations, and `PartitionManager.cpp` batching call sites. Record the
   current list, footprint, rectangle, lock, and clear contracts in test names.
2. Write failing D3D-free tests for an object command sequence containing
   overlapping commands from both list partitions. Assert exact four-write
   order, edge clipping, final packed bytes, untouched clear bytes, and
   last-writer-wins.
3. Write failing shroud tests for overlapping inclusive rectangles, negative or
   out-of-range bounds, different alpha values, empty commands, and preserved
   initial bytes. Assert command-order behavior.
4. Compare a serial full-height oracle with two disjoint row ranges, including
   guard bytes before/after each row and every supported overlay format.
5. Add invalid snapshot/range/stride/format tests that prove no output write.
6. Add runtime/fallback seams for lease denial, two-worker start failure, task
   allocation failure, queue rejection, and shutdown. Require serial output
   equality and no surviving task.

**Exit criteria:** The new target is registered and the intentionally incomplete
implementation is red for the behavior tests, with no production source change
yet.

## Task 2: Add the D3D-free ordered command kernels

**Files:**

- Add `Core/Libraries/Include/Lib/RadarOverlayKernel.h`.
- Add `Core/Libraries/Source/TaskRuntime/RadarOverlayKernel.cpp`.
- Update the library/test CMake source lists.

**Steps:**

1. Define C++98 PODs for object commands, shroud commands, and snapshots.  Make
   ownership explicit: the owner owns commands/output; kernels borrow them only
   during a joined call.
2. Implement checked validation and a D3D-free little-endian pixel write helper
   for the supported overlay formats.  Do not infer format ordinals from D3D.
3. Implement the object row kernel.  For every assigned row, scan all commands
   in original order and apply `(x,y)`, `(x,y+1)`, `(x+1,y+1)`, `(x+1,y)` with
   the legacy legal-point predicate and no writes outside the assigned rows.
4. Implement the shroud row kernel.  For every assigned row, scan all commands
   in original order and apply inclusive rectangle coverage to that row, with
   the same clipping behavior as the owner path.
5. Keep untouched bytes unchanged; do not clear inside worker code.  The owner
   provides the initial clear/current surface bytes.
6. Run the tests from Task 1 and confirm they turn green before integration.

**Exit criteria:** Serial and two-range outputs are byte-identical across all
synthetic object/shroud cases and all supported formats; the kernels have no
D3D/global/live-pointer/allocation/wait access.

## Task 3: Implement bounded owner batch storage and runtime reuse

**Files:**

- Add `Core/GameEngineDevice/Include/W3DDevice/Common/RadarOverlayPrepare.h`.
- Add `Core/GameEngineDevice/Source/W3DDevice/Common/System/RadarOverlayPrepare.cpp`.
- Update the shared GameEngineDevice CMake lists.
- Update Stage 5 `RadarTerrainPrepare.*` only if the common runtime/lease must
  be extracted to support a typed overlay row batch.

**Steps:**

1. Add owner-owned object and shroud batch storage with checked multiplication,
   bounded command/output budget, and VC6-safe allocation helpers.  Do not
   submit a partial snapshot.
2. Reuse the Stage 5 `RenderPrepareRuntime` and exclusive lease.  If a generic
   adapter is needed, preserve terrain behavior and its tests while adding
   object/shroud row-task dispatch; do not create a second `TaskRuntime`.
3. Create exactly two owner-created row tasks with immutable batch pointer and
   disjoint `[rowBegin,rowEnd)` ranges.  Treat submission as all-or-none and
   use the established two-worker, one-worker, serial fallback sequence.
4. Add deterministic fault hooks for task allocation, queue admission, runtime
   start, lease denial, and shutdown.  Verify every fallback matches the serial
   oracle and no worker survives release.
5. Keep the owner wait limited to the private submitted batch and keep all
   command/output memory alive until join and upload complete.

**Exit criteria:** Focused runtime/failure tests pass, all failure modes are
deterministic, and Stage 5 terrain tests remain green.

## Task 4: Capture and integrate object overlay preparation

**Files:**

- Update `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp`.
- Update `Core/GameEngineDevice/Include/W3DDevice/Common/W3DRadar.h` only for
  narrow capture/batch declarations.
- Extend `Core/Tools/RadarOverlayPrepareTest/RadarOverlayPrepareTest.cpp` for
  owner-side calculation seams that do not require D3D.

**Steps:**

1. Preserve `updateObjectTexture`'s clear timing and capture the exact initial
   overlay bytes on the owner.  Validate surface description, pitch, dimensions,
   and format before allocation.
2. Traverse `m_objectList` to completion, then `m_localObjectList` to
   completion.  Preserve linked-list order and append only visible commands.
   Perform `canRenderObject`, position-to-radar conversion, stealth alpha, and
   `ARGB_Color_To_WW3D_Color` on the owner.  Store only final x/y and packed
   color.
3. Acquire the overlay consumer lease, run the row kernel, wait, and upload
   tight rows using the actual surface pitch.  Release the surface and lease in
   every return path.
4. On any capture/allocation/lease/runtime/upload preparation failure, execute
   the unchanged serial owner path.  Never upload an incomplete staging buffer.
5. Keep `renderObjectList` as the reference during bring-up until byte-parity
   tests and source review prove the staged path covers every legacy branch.

**Exit criteria:** The staged object overlay matches the serial reference for
   empty, single-list, dual-list, overlap, stealth, edge, and format cases;
   production object status and frame reads occur only before worker admission.

## Task 5: Capture and integrate batched shroud preparation

**Files:**

- Update `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp`.
- Update `Core/GameEngineDevice/Include/W3DDevice/Common/W3DRadar.h` for batch
  state only if required.
- Extend `Core/Tools/RadarOverlayPrepareTest/RadarOverlayPrepareTest.cpp`.
- Inspect, but do not alter, the `PartitionManager.cpp` call sites unless a
  source-proven behavior issue requires a narrow owner assertion.

**Steps:**

1. Keep the direct/non-batched `setShroudLevel` path synchronous and owner-only.
   Preserve its existing lock, rectangle, alpha, pixel, unlock, and release
   sequence.
2. In `beginSetShroudLevel`, capture the current visible shroud rows through an
   owner lock/copy/unlock and establish an ordered command buffer.  Do not keep
   a D3D lock open while workers execute.
3. In each batched `setShroudLevel`, compute map/radar bounds and packed alpha
   on the owner and append one command in call order.  No live pointer or D3D
   object enters the command.
4. In `endSetShroudLevel`, acquire the lease, run two disjoint row tasks that
   scan all commands, wait, and upload the final rows on the owner.  Reset the
   batch and release every resource exactly once.
5. If any stage cannot capture or allocate, replay the complete ordered command
   list through the existing owner-only batched path.  Preserve current
   assertions for malformed begin/end calls and avoid partial uploads.
6. Test repeated rectangles, full-map rectangles, edge clipping, status alpha,
   nested/empty batches, direct calls, and begin/end reset behavior.

**Exit criteria:** Batched and direct shroud outputs match the serial reference;
   overlapping updates are last-command-wins; no D3D access is reachable from a
   worker.

## Task 6: Verify both title variants and lifecycle safety

**Files:**

- `Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`.
- `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`.
- Corresponding source lists only if the new shared files are not already
  registered.
- Relevant `GameClient.cpp` destruction-order sources for read-only proof.

**Steps:**

1. Confirm Stage 5 initializes the shared render-preparation runtime before
   radar use and drains it before D3D/WW3D teardown in both title variants.
2. Verify object and shroud consumers acquire only during owner calls and
   release after upload.  No consumer retains a lease across map reset, display
   reset, or destruction.
3. Add guarded lifecycle tests for partial display initialization, repeated
   shutdown, begin/end with no commands, and shutdown after a successful or
   failed overlay batch.
4. Run source audits for worker restrictions, command lifetime, row overlap,
   and unchanged simulation/replay/save/network code.

**Exit criteria:** Both variants use the same drained shared runtime with no
surviving worker/task or surface pointer at display teardown.

## Task 7: Build and focused validation

**Files:**

- `TESTING.md` for generic repository-relative Stage 6 instructions.
- CMake files for the new focused test target.

**Steps:**

1. Run `git diff --check` and the focused overlay target.  Run the existing
   `core_task_runtime_tests`, `core_texture_mip_buffer_tests`, and all Stage 5
   radar terrain tests.
2. Configure/build modern Win32 Debug or Release for
   `radar_overlay_prepare_tests`, `core_task_runtime_tests`,
   `core_texture_mip_buffer_tests`, `g_generals`, and `z_generals`.
3. Configure/build the VC6/Win32 lane for the same focused targets and both
   title executables.  Keep the two lanes distinct; never substitute a modern
   executable for optimized replay evidence.
4. Record source commit, target results, and executable hashes in the local
   handoff only.  Keep machine-specific build and replay paths out of the repo.
5. Verify the deferred manual checklist: map load, radar refresh, object count
   stress, shroud reveal/fog/clear, map reset, save/load, alt-tab, and exit.
   Do not launch the intermediate game.

**Exit criteria:** Modern and VC6 builds, focused tests, existing tests, source
audits, and deterministic serial/split comparisons pass.

## Task 8: Ten review rounds and complete replay gate

Perform ten distinct reviews of the complete Stage 6 diff.  Each round must
inspect the current head, record findings, fix material issues before the next
round, and rerun the focused tests after fixes.  Use these lenses in order:

1. owner-thread/D3D affinity and live-pointer escape;
2. object list order, four-write order, clipping, and last-writer-wins;
3. object visibility, stealth alpha, frame dependence, and format parity;
4. shroud rectangle inclusivity, alpha, command order, and direct-path parity;
5. row disjointness, output initialization, stride/guard safety, and serial
   oracle equivalence;
6. runtime lease, queue capacity, fallback, shutdown, and task lifetime;
7. checked allocation/budget behavior, malformed begin/end, and failure cleanup;
8. C++98/VC6, both title variants, CMake registration, and warning hygiene;
9. replay/save/network/RNG/Xfer isolation and scope against Stages 5, 7, and 8;
10. full diff, tests/build evidence, documentation privacy, and delivery
    readiness.

After the tenth clean review, run the full optimized VC6 replay gate over ten
unique corpus files: nine regular replays once each and the 2v6 Hard-AI stress
replay three times.  Require twelve exits of zero, no CRC mismatch/out-of-sync/
ownership/crash marker, and identical file sets and hashes for all three stress
CRC traces.  Stop and debug any failure; do not report the PR ready on compile
success alone.

## Task 9: Commit, push, and stacked handoff

1. Inspect the final diff and ensure only Stage 6 source, tests, CMake, and
   repository-relative docs are present.  Remove local paths and generated
   artifacts from the PR description and commit text.
2. Use a Conventional Commit subject for the implementation commit.  Push the
   Stage 6 branch and create/update its unmerged PR against the reviewed Stage 5
   head.  Include focused tests, both build lanes, ten review rounds, and the
   12-run replay evidence without personal machine paths.
3. Do not merge or promote the Stage 6 build.  The next stage must branch from
   this fully reviewed/tested Stage 6 head and retain the Stage 5 and Stage 6
   changes as a stack.
4. At the end of the full Stage 5--8 sequence, ask the user for permission to
   launch the Stage 5 candidate for interactive testing.  Until then, keep all
   intermediate builds isolated and do not alter the canonical playable game.

## Review checklist

- [ ] Object command order is exactly `m_objectList` then
      `m_localObjectList`, with linked-list order preserved.
- [ ] Object command stores final packed color/x/y and workers do no live reads.
- [ ] Four object writes, clipping, and overwrite order match the reference.
- [ ] Shroud commands store final rectangle/packed color in call order.
- [ ] Direct/non-batched shroud updates remain synchronous and owner-only.
- [ ] Batched shroud output starts from captured current bytes and preserves
      untouched pixels.
- [ ] Workers write only disjoint rows and scan the full ordered command list.
- [ ] No worker calls D3D, globals, allocation, waits, RNG, replay/save/network,
      or title code.
- [ ] Stage 5 runtime/lease is reused; no second pool or overlapping batch.
- [ ] All fallback, shutdown, reset, and malformed begin/end paths clean up.
- [ ] C++98/VC6 and modern builds pass for both titles and focused tests pass.
- [ ] Ten unique replays pass in twelve executions with identical stress CRCs.
- [ ] Ten review rounds are clean and the PR remains unmerged.
