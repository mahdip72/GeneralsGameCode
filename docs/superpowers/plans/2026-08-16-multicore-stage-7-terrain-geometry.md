# Stage 7 Static Terrain Geometry Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` to execute this plan task by task,
> with strict test-driven development and a fresh review after every task.

**Goal:** Prepare max-LOD static terrain vertex geometry and static lighting on
the existing bounded two-worker service while preserving exact owner-thread
D3D/lifecycle ownership and legacy output.

**Architecture:** The owner snapshots map, UV, alpha, normal, and lighting
inputs into bounded POD for one vertex-buffer tile. Pure workers process
disjoint cell-row ranges into private staging. The owner synchronously joins,
validates, applies owner-only visualization, and commits to the backup and D3D
vertex buffers. Every failure falls back to the unchanged serial tile path.

**Tech stack:** C++98, VC6/Win32, CMake/CTest, existing `TaskRuntime` and
`RadarTerrainPrepareService`, WWMath/Vector3 parity helpers, DirectX 8 owner
upload, replay CRC validation.

## Global constraints

- Keep D3D, live map/light/global reads, backup publication, resource lifetime,
  reset, and upload on the render owner.
- Workers receive only immutable POD plus private output and perform no
  allocation, wait, lock, logging, RNG, replay/save/network, or engine access.
- Preserve exact `VertexFormatXYZDUV2` field bytes, float evaluation order,
  normal/light ordering, flip, UV, alpha, border, and depth-fade behavior.
- Reuse the existing shared two-worker runtime with consumer ID 4; add no pool,
  callback, background work, unbounded queue, or asynchronous lifetime.
- Use checked C++98/VC6-safe sizes and an 8 MiB per-tile batch budget.
- Keep the complete legacy serial path available and publish no partial output.
- Keep local paths, profile details, artifacts, and personal-system information
  out of repository files, commit messages, and PR text.

## Task 1: Establish exact red kernel tests

**Files:**

- Add `Core/Tools/HeightMapTerrainPrepareTest/HeightMapTerrainPrepareTest.cpp`.
- Add `Core/Tools/HeightMapTerrainPrepareTest/CMakeLists.txt`.
- Register the focused test in the Core tools CMake entrypoint.
- Add only the kernel declarations needed to make the target compile red.

**Steps:**

1. Record the exact legacy `updateVB` vertex order, height-neighbor accesses,
   normal calculations, UV/alpha mapping, flip path, lighting order, channel
   conversion, and depth fade in named test fixtures.
2. Add a layout test proving the D3D-free output POD is exactly 32 bytes with
   the expected field offsets.
3. Add failing exact-byte tests comparing full-range and two disjoint row
   executions for ordinary, flipped, border, non-zero-origin, odd-row, and
   minimum-row inputs with guard bytes around every output row.
4. Add failing lighting tests for directional, point, spot, attenuation,
   ambient, channel clamping, multiple-light order, and depth fade at/around the
   water threshold.
5. Add invalid range, stride, null pointer, count, overflow, and budget tests
   requiring zero writes.

**Exit criteria:** The new test target is registered and behavior tests fail for
the missing implementation before production integration changes exist.

## Task 2: Implement the pure terrain row kernel

**Files:**

- Add `Core/Libraries/Include/Lib/HeightMapTerrainKernel.h`.
- Add `Core/Libraries/Source/TaskRuntime/HeightMapTerrainKernel.cpp`.
- Update `Core/Libraries/Source/TaskRuntime/CMakeLists.txt`.
- Complete the focused tests from Task 1.

**Steps:**

1. Define C++98 PODs for captured cells, scene lights, lighting state, output
   vertices, and a validated row snapshot.
2. Implement checked validation with no output writes on invalid input.
3. Reproduce the four legacy normalized cross products, summation, UV/alpha,
   triangle-flip, static-lighting, attenuation, clamping, and depth-fade logic
   in the same evaluation order and WWMath semantics.
4. Write only the assigned half-open cell rows and allocate nothing.
5. Turn all Task 1 tests green; audit the worker source for forbidden calls.

**Exit criteria:** Serial and partitioned pure-kernel outputs are byte-identical
for every fixture and guard bytes remain untouched.

## Task 3: Add bounded owner batch and row-service adapter

**Files:**

- Add `Core/GameEngineDevice/Include/W3DDevice/Common/HeightMapTerrainPrepare.h`.
- Add `Core/GameEngineDevice/Source/W3DDevice/Common/System/HeightMapTerrainPrepare.cpp`.
- Update `Core/GameEngineDevice/CMakeLists.txt`.
- Extend `HeightMapTerrainPrepareTest.cpp`.

**Steps:**

1. Add checked owner-owned storage for captured cells/halo, captured lights, and
   staged vertices. Reject partial allocation and batches over 8 MiB.
2. Add a `RadarPrepareRowWork` adapter for the pure kernel and acquire consumer
   ID 4 from the existing service.
3. Dispatch exactly two disjoint row ranges with the existing all-or-none
   submission, two-worker to one-worker retry, synchronous join, and serial
   fallback result.
4. Add deterministic tests for unavailable service, lease denial, invalid
   batch, task allocation/submission failure, shutdown, exact-once cleanup, and
   no surviving task.
5. Prove small batches remain serial and eligible batches use worker thread IDs
   without relying on sleeps.

**Exit criteria:** The adapter is bounded, synchronous, lifecycle-safe, and all
runtime/fallback tests plus prior Stage 5/6 service tests pass.

## Task 4: Integrate owner capture and legacy fallback

**Files:**

- Update `Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp`.
- Update the corresponding HeightMap header only for narrow private helpers.
- Extend focused tests for owner-calculation seams that remain D3D-free.

**Steps:**

1. Extract/retain an unchanged serial `updateVB` implementation as the
   authoritative fallback.
2. In `updateBlock`, hold one consumer lease across eligible tiles, while
   capturing, joining, and committing each tile before the next.
3. Capture final origin-adjusted coordinates, height halo, UV/alpha/flip data,
   global terrain-light values, scene lights in iterator order, and depth-fade
   values before worker admission. Retain no live pointers.
4. Use the parallel path only for at least two rows and 512 cells. On every
   capture, allocation, budget, lease, runtime, or validation failure, run the
   complete tile through the serial implementation.
5. Keep impassable/cliff diagnostic mutation on the owner unless exact tests
   prove an immutable flag-only worker representation is simpler and identical.

**Exit criteria:** No live engine state is reachable from the worker and every
failure produces the complete legacy tile output exactly once.

## Task 5: Commit backup and D3D vertices on the owner

**Files:**

- Update `HeightMap.cpp` integration from Task 4.
- Extend focused source/lifecycle audits.

**Steps:**

1. Wait for preparation before creating the DX8 write lock.
2. Validate the complete tile output, apply owner-only visualization, copy to
   `m_vertexBufferBackup`, lock the hardware buffer, upload, and unlock.
3. Publish neither backup nor D3D bytes after a failed/partial batch.
4. Verify partial updates, map/resource release, LOD replacement, reset,
   re-acquisition, and display shutdown cannot overlap a surviving task.
5. Preserve both title variants' existing display-owned service initialization
   and shutdown; do not add HeightMap-owned runtime teardown.

**Exit criteria:** D3D and backup writes occur only after synchronous join on
the owner, with no deadlock, UAF, stale task, or partial publication path.

## Task 6: Focused and compatibility validation

**Files:**

- Update `TESTING.md` with repository-relative Stage 7 gates.
- Update CMake only as needed for the focused target.

**Steps:**

1. Run `git diff --check` and focused `HeightMapTerrainPrepare`, task-runtime,
   texture, radar-terrain, and radar-overlay tests.
2. Configure/build modern x86 Debug and Profile targets for the focused tests,
   `g_generals`, and `z_generals`, sequentially in task-owned H: build trees.
3. Build the VC6-compatible focused tests and both title executables from the
   same source commit; do not substitute a modern executable for replay proof.
4. Run source audits for D3D/global/live-pointer/allocation/wait/RNG/replay/
   save/network access and compare both title source registrations.
5. Record commit hashes and executable hashes only in local validation notes,
   never repository documentation or PR text.

**Exit criteria:** All focused/regression tests and both-title modern/VC6 builds
pass from clean source.

## Task 7: Draft PR and ten Luna Max review rounds

1. Commit with Conventional Commit subjects, push the Stage 7 branch, and open
   a draft PR against latest `main`; do not merge it.
2. Review the complete current diff ten times with fresh Luna Max agents and
   these distinct lenses, fixing Critical/Important findings before continuing:
   1. owner/D3D affinity and live-pointer escape;
   2. geometry, normal, flip, UV, alpha, border, and origin parity;
   3. static-lighting order, attenuation, clamping, and depth fade;
   4. row disjointness, layout, stride, guard, and commit atomicity;
   5. lease, submission, fallback, task ownership, and shutdown;
   6. reset, LOD, partial-update, map-mutation, and lifetime interleavings;
   7. checked allocation, budgets, malformed inputs, and cleanup;
   8. C++98/VC6/x87, both titles, CMake, and warning hygiene;
   9. simulation/replay/save/network/RNG isolation and stage scope;
   10. complete diff, tests/build evidence, privacy, and readiness.
3. Rerun focused tests after every fix and re-review the affected lens.

**Exit criteria:** Ten recorded review rounds are clean at the final head and
the unmerged draft PR contains no machine-specific or personal-system details.

## Task 8: Replay, live-AI smoke, and final handoff

1. Confirm no Generals executable is running. Create only isolated disposable
   runtime/profile state under the established H: scratch roots.
2. Run the ten unique replay fixtures as twelve executions: nine ordinary
   fixtures once and the 2v6 Hard-AI stress fixture three times.
3. Require exit zero, no crash/assert/ownership/out-of-sync/CRC mismatch, and
   byte-identical stress CRC trace sets across all three runs.
4. Run three fresh AI-vs-AI headless smoke games through the established runner
   when available and require clean completion.
5. If any gate fails, diagnose the actual log, make only a Stage 7 root-cause
   fix, repeat affected review/build/test gates, and rerun the full replay gate.
6. Clean only this task's obsolete build/replay scratch after evidence is
   recorded. Leave other agents' artifacts and the canonical playable install
   untouched.

**Exit criteria:** The final PR head passes all gates and is ready, unmerged,
for the user's Stage 7 manual test.
