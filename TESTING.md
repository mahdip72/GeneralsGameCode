# Test Replays

The GeneralsReplays folder contains replays and the required maps that are tested in CI to ensure that the game is retail compatible.

## Modernization Stage 5 deterministic simulation

The deterministic-runtime gate is documented separately in
`docs/modernization-stage5-deterministic-simulation.md`. It covers the exact
installed native runtime across serial-1, parallel-1/2/4/8/16, and automatic
workers; two full replay passes with repeated 2-vs-6 stress; the exact repeated
4-vs-3 and 4-vs-2 live-AI cross-product across at least three distinct seeds;
one additional installed 16-worker 4-vs-2 `simulationMode=shadow` comparison
with positive collision jobs, `collision_shadow_compared_candidates > 0` for
successful legacy insertions covered by the exact collision order/orientation
oracle, and positive matching physics
shadow prefix/range/jobs; reset-aware per-replay collision/physics manifests;
zero mismatch/unexpected-fallback telemetry; scheduler and consumer faults; and
aggregate Stage 5 large-match throughput. It deliberately does not claim final
Stage 5 acceptance. Qualifying live stress separately requires physical-worker
path authority and physics-specific authoritative batches/prefixes/jobs; AI,
collision, and global scheduler traffic cannot proxy either family. Synchronous
direct-path watchdog timeouts and validation failures fail the
deterministic-runtime gate; late worker drains remain diagnostic because they
may occur after the completion manifest. The 2x
replay threshold is aggregate Stage 5 throughput,
not a collision-lane speedup claim; collision phases and qualifying live
collision authority are reported and gated separately.
Serial evidence must report every collision-lane work counter as zero. The
forced parallel-1 lane may report its expected owner fallback, but cannot report
collision preparation, jobs, authority, shadow comparison, stale publication,
or unexpected fallback.

Final acceptance is a separate fail-closed aggregation. It requires independently
hashed deterministic-runtime, replay, fresh-AI, performance, mixed-worker
multiplayer, combined Stage 4 plus Stage 5 installed-runtime, premium-review,
and user manual evidence for the same commit and artifact-set manifest. The
combined lane must use `pipelineMode=parallel`, `simulationMode=parallel`,
automatic workers, D3D11, and the dedicated render thread for both titles; it
does not replace the serial-pipeline isolation matrix. Run the aggregator only
after the user's real final manual approval:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1 `
  -AcceptanceManifestPath <final-acceptance-request.json> `
  -OutputPath <fresh-final-acceptance-report.json>
```

`FinalAcceptanceManifest.schema.json`, `FinalAcceptanceArtifactSet.schema.json`,
and `FinalAcceptanceEvidence.schema.json` define the envelopes. The PowerShell
aggregator additionally enforces exact kind-specific metrics, attachment roles,
cross-manifest hashes, and combined-policy semantics. Missing manual evidence
is expected to fail until the user has actually approved the candidate.

The multiplayer attachment is not a free-form soak summary. It must match
`Net3LoopbackEvidence.schema.json`: exactly 16 canonically ordered installed
NET3 matches (both titles, four supported topologies, seeds 23063 and 49374)
and exactly 40 nested peers (20 per title). Every multicore peer must prove all
six diagnostic kernel bits with balanced physical-plus-owner execution,
consistent physical-worker masks, and peak concurrent execution above one;
forced-one peers must report zero scheduler work. These v1 bits are diagnostic
only and do not authorize live multiplayer workers. Collision evidence is emitted
by the actual parallel collision-candidate kernel over a qualifying batch and
binds that kernel's own submitted, physical, owner-help, mask, and peak fields.
The strict parser independently binds source commit,
artifact-set hash, both executable hashes, NET3 readiness, exact roster and
policy mask, equal peer CRC/frame, exit zero, and clean shutdown. Each peer
also names a runner-produced raw output file whose SHA-256 and independently
observed process executable/artifact hashes are verified. That evidence creates
an external `MultiplayerSimulationRuntimeProof.txt` for diagnostics, but a
mutable sibling file cannot grant authority to an ordinary build. Product
builds embed no v1 kernel authority. The trusted cap is always zero. The
executable hashes itself and rechecks title, source revision, build/content
CRCs, epochs, schema, and mask; runtime evidence can only remain diagnostic.
`RTS_BUILD_STAGE5_PROMOTED_MULTIPLAYER_AUTHORITY=ON` fails configuration until
a separately reviewed lockstep-v2 authority contract exists.

Create that evidence only from a fresh installed-runtime directory and the
already-generated six-artifact manifest. The runner starts the exact two x64
product executables in the explicit, hidden, local-only
`-installedNet3Validation` process mode; ordinary game and matchmaking startup
cannot enter this mode. It independently binds every live process ID to its
on-disk executable and re-hashes the complete artifact set for every peer before
allowing the peer to exchange fixed-width NET3 records. It then enforces the
canonical 16-match/40-peer matrix. Every peer completes both directions of the
NET3 Hello/Ack challenge using its own session token before it may publish a
ready record. The first runner pass creates title-specific external proof
bundles for the default-zero candidate without rebuilding either executable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Core/Tools/DeterministicSimulationValidation/Invoke-InstalledNet3LoopbackValidation.ps1 `
  -GeneralsExecutable <installed-generals.exe> `
  -ZeroHourExecutable <installed-generalszh.exe> `
  -ArtifactSetManifestPath <Stage5ArtifactSet.json> `
  -SourceCommit <exact-lowercase-40-hex-commit> `
  -OutputDirectory <fresh-task-owned-evidence-directory>
```

Do not configure a promoted Stage 5 product build yet.
`RTS_BUILD_STAGE5_PROMOTED_MULTIPLAYER_AUTHORITY=ON` is a reserved switch and
CMake rejects it with a clear lockstep-v2 prerequisite error. Generate proof
bundles only as diagnostic artifacts from the default-zero candidate; copy
`ProofBundles/Generals` beside the exact Generals executable and
`ProofBundles/ZeroHour` beside the exact Zero Hour executable only when
diagnostic inspection requires it. Keep the bundle files and `Net3Raw` tree
together. Any changed executable, source revision, artifact manifest, evidence
manifest, raw index, or raw peer output remains non-authorizing. The scoped
runner mode is bounded and never joins a lobby or opens an ordinary gameplay
network session.

The performance attachment must match
`PerformanceScalingEvidence.schema.json`. Final acceptance recomputes exact
physical-core-mask populations for forced-one, 8-core, and 16-core lanes;
one-worker phase totals and Amdahl limits; per-kernel capture/schedule/wait/
validate/commit totals versus the exact serial operation; and the measured
Stage 3 regression, 8-core throughput, and 8-to-16 scaling ratios for canonical
1k, 4k, 8k, and dense eight-player fixtures. Logical worker counts without
distinct physical-core evidence fail closed.

The report must hash an adjacent `PerformanceScalingRawSamples.schema.json`
manifest containing canonical per-process repeat rows bound to the source,
artifact set, Stage 3 baseline, exact installed executable SHA-256, and exact
supported `-headless -noFPSLimit -pipelineMode serial -simulationMode parallel
-workerPolicy auto -validationExecutableSha256 ... -workerCount ... -replay ...`
installed-runtime command line. That manifest must hash a parsed
`PerformanceScalingTopologyReceipt.schema.json` CPU-set receipt. The validator
derives the physical-core lanes and recomputes every reported median and ratio;
the topology receipt must correlate to the first current one-worker run, and
summary-only or internally consistent forged reports fail.

Run the validation-tool self-tests and the real replay child-mode source audit
without building or launching the game:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Core/Tools/DeterministicSimulationValidation/DeterministicSimulationValidation.Tests.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File Core/Tools/DeterministicSimulationValidation/Audit-ReplayModePropagation.ps1 -SelfTest
powershell -NoProfile -ExecutionPolicy Bypass -File Core/Tools/DeterministicSimulationValidation/Audit-ReplayModePropagation.ps1 -SourceRoot .
```

The matrix runner requires a fresh evidence directory, an installed Generals or
Zero Hour runtime, and a reviewed explicit title-matching fixture manifest matching
`ReplayFixtureManifest.schema.json`. Use `-PlanOnly` first. Automatic workers
mean `-workerPolicy auto` with no `-workerCount`; replay `-jobs` is process-level
parallelism and is not used by this matrix. A CI-built candidate may supply its
controller-computed hash with `-ExpectedExecutableSha256`; fixture and map
hashes still come only from the manifest. The existing VC6 replay check below
remains a separate retail-compatibility oracle.

Installed execution additionally requires an existing task-owned directory below
`H:\` through `-TaskRoot`, an evidence `-OutputRoot` below that directory, and
the reviewed `-AllowHeadlessDirectExecution` switch. The runner does not invoke
`launcher.exe`, because its legacy process wrapper does not propagate the child
exit code; it parses `launcher.lcf` and records an equivalence contract for the
target executable, arguments, runtime working directory, child environment, and
title-specific profile path before using the explicit headless exception. Both
title variants redirect the Windows Documents known-folder values temporarily to
a per-run H: tree and restore them in `finally`; TEMP, TMP, cache, logs, and
profiles are task-owned and removed after the run. Keep the evidence directory
for review and cleanup only the generated task scratch directory.

You can also test with these replays locally:
- Copy the replays into a subfolder in your `%USERPROFILE%/Documents/Command and Conquer Generals Zero Hour Data/Replays` folder.
- Copy the maps into `%USERPROFILE%/Documents/Command and Conquer Generals Zero Hour Data/Maps`
- Start the test with this: (copy into a .bat file next to your executable)
```
START /B /W generalszh.exe -jobs 4 -headless -replay subfolder/*.rep > replay_check.log
echo %errorlevel%
PAUSE
```
It will run the game in the background and check that each replay is compatible. You need to use a VC6 build with optimizations and RTS_BUILD_OPTION_DEBUG = OFF, otherwise the game won't be compatible.

Zero Hour records the skirmish-AI behavior epoch as a suffix in the replay header's existing variable-length build-time field. Unmarked retail replays use the legacy AI behavior. Replays marked `[SkirmishAILiveness=1]` use only the PR6 liveness fixes, and new recordings marked `[SkirmishAIEpoch=2]` use the current PR7-PR9 AI behavior as well. Unknown, malformed, mixed, or duplicate markers fall back to legacy behavior. Replays produced by transitional PR7-PR9 builds carried only the older liveness marker despite using later AI behavior; those recordings cannot be identified reliably and are unsupported.

# Stage 0 ownership and profiling checks

Use an optimized VC6 release-log build for the replay and CRC diagnostic:

```
cmake --preset vc6-releaselog -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF
cmake --build --preset vc6-releaselog
START /B /W generalszh.exe -jobs 4 -headless -replay subfolder/*.rep > replay_check.log
echo %errorlevel%
```

Run the last two commands from the directory containing the `vc6-releaselog` Zero Hour executable after copying the replay corpus and maps as described above. A zero exit code and no CRC or ownership crash in `replay_check.log` are required. `-jobs` starts independent replay processes only; it does not create or control in-process game workers.

For Tracy frame-phase capture, build the profile configuration, start Tracy v0.13.1, then launch a game and capture several shell and in-game frames:

```
cmake --preset win32-profile -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF
cmake --build --preset win32-profile
```

The capture must show `Engine.Update` and its `Radar`, `Audio`, `Client`, `MessageStream`, `Network`, `GameLogic`, and `ClientStep` child zones, plus `Platform.Win32Messages` and `Engine.FramePacer`. If Tracy cannot connect, remove `dbghelp.dll` from the game binary directory and retry.

Manually smoke-test Zero Hour first, then Generals: launch a skirmish, play through several updates, pause and resume, save and load, take a screenshot, return to the map or shell, alt-tab/minimize and restore, then quit. In crash-enabled diagnostic builds, no owner-thread assertion is expected during these flows.

# Stage 2 managed screenshot checks

Configure a modern x86 Debug build with both games and the Core test executables, then build the two games and focused tests:

```
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target g_generals z_generals core_task_runtime_tests core_screenshot_codec_tests
ctest --test-dir build/win32-debug -C Debug -R "^core_(task_runtime|screenshot_codec)_tests$" --output-on-failure
```

The two CTest targets must pass. Confirm the screenshot implementation has no unmanaged thread creation and that the worker task contains no live engine, render, or UI references:

```
rg -n "CreateThread|ThreadClass" Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp
rg -U -P "(?ms)^class (ScreenshotBatch|ScreenshotConvertTask).*?^};" Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp | rg -n "The[A-Z]|DX8|WW3D|SurfaceClass|GameText|InGameUI|waitUntilIdle"
```

Both source-audit commands must produce no matches. Manually verify JPEG and PNG captures in each game: issue several rapid captures, confirm successful `GUI:ScreenCapture` messages and files under the user-data `Screenshots` folder, then quit immediately after a capture to exercise draining teardown.

# Stage 3 parallel screenshot conversion checks

Run the Stage 2 focused build and CTest commands again. The screenshot codec test now starts a two-worker task runtime and verifies that striped conversion is byte-identical to serial conversion for padded, odd-sized ARGB32 and RGB565 images. It also covers the 128-row parallel threshold and deterministic full-image range coverage.

In a profile build on a machine with at least two logical processors, capture 720p or larger JPEG and PNG screenshots while connected to Tracy. Each large capture should show multiple `Screenshot.Convert` worker zones followed by exactly one `Screenshot.Encode` zone. Captures shorter than 128 rows and one-worker fallback runs should show one conversion zone. No conversion task may wait for another task.

Repeat the rapid-capture and immediate-quit manual checks for both games. Compare every output image against the displayed frame for missing, duplicated, or corrupted horizontal bands, with particular attention to rows around stripe boundaries. Successful capture messages must still be delivered from the main thread, and shutdown must drain accepted batches without a hang or access violation.

# Stage 4 background texture preparation checks

Configure a modern x86 Debug build with both games and Core extras, then build the games and all multicore-focused tests:

```
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target g_generals z_generals core_task_runtime_tests core_screenshot_codec_tests core_texture_mip_buffer_tests
ctest --test-dir build/win32-debug -C Debug -R "^core_(task_runtime|screenshot_codec|texture_mip_buffer)_tests$" --output-on-failure
```

The three CTest targets must pass. The texture mip-buffer suite covers uncompressed and DXT layouts, odd and sub-block dimensions, rectangular mip counts, padded source/destination pitches, overflow rejection, guarded copies, and exact-limit/rejected retained-memory reservations. Two accepted preparation copies are gated concurrently on distinct worker thread IDs and compared byte-for-byte with serial preparation. A saturated runtime also proves that rejected preparation remains caller-owned, executes synchronously on the caller, and is destroyed exactly once. The production loader additionally caps accumulated asynchronous DDS/TGA source, prepared mip, and TGA conversion memory at 64 MiB; byte-admission rejection uses synchronous owner preparation. The TaskRuntime suite separately covers queued-task ownership recovery, rejection, draining, and joins. Confirm that the legacy detached texture loader and its background queue are gone, and that the bounded worker body only prepares CPU data and publishes completion:

```
rg -n "LoaderThreadClass|_TextureLoadThread|_BackgroundQueue|_BackgroundCriticalSection" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp
rg -U -P "(?ms)^class TexturePrepareRuntimeTask.*?^};" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp | rg -n "DX8|D3D|Texture->|Apply|Lock_Surfaces|Unlock_Surfaces|Create_D3D"
```

Both source-audit commands must produce no matches. Build both games with VC6 as well as modern Win32. Run the optimized `vc6-releaselog` replay command documented in Stage 0 and require a zero exit code with no CRC or ownership failure; the Stage 4 texture-preparation changes must not modify gameplay, replay, network, save, RNG, or serialization state.

For Tracy validation, use a profile build on a machine with at least two logical processors and load texture-heavy maps in both games. The capture should show up to two concurrent `Texture.Prepare` worker zones, with their corresponding `Texture.Upload` zones on the render owner thread. No worker may call Direct3D, dereference a live texture, wait for another worker, or access engine globals.

Manually test Zero Hour and Generals by loading several skirmish maps and armies, moving the camera quickly across previously unseen terrain and units, returning to the shell, loading another map, alt-tabbing during map loading, and quitting during or immediately after a load. Verify that terrain, unit, effect, UI, and cube-map textures have no missing-texture placeholders, corruption, delayed permanent blur, device loss, hang, or access violation. The shipped DDS backends do not expose volume-level memory, so Stage 4 rejects DDS volume preparation through the existing missing-texture path instead of uploading uninitialized data; volume rendering is not claimed as supported. Also repeat screenshot capture and save/load smoke tests to guard the earlier multicore stages.

# Stage 5 radar terrain preparation checks

Stage 5 moves only the CPU preparation of the radar terrain raster into a
synchronous, bounded fork-join.  The owner thread still reads live terrain,
bridge, water, and visual state, owns the immutable POD batch, and performs
all Direct3D surface access and the final row upload.  Worker tasks receive no
engine pointer or global and only shade their assigned rows.  This stage does
not parallelize simulation, AI, pathfinding, replay/save/network state, or
radar map mutation.

The following implementation and test coverage is present in the committed
Stage 5 source.  These checkmarks describe source/test evidence only; they do
not claim that the commands below have run successfully:

- [x] The D3D-free kernel preserves the legacy center-branch order, clipped
  3x3 traversal, interpolation guards and argument order, averaging, and
  format bytes.  Serial and split-row calls are compared byte-for-byte,
  including guarded 24-bit rows and the supported 16/32-bit formats.
- [x] The owner batch uses checked sizes and a fixed staging budget, owns the
  cell/output storage, and rejects incomplete or unsupported batches without
  writing output.
- [x] The service admits one consumer, submits exactly two disjoint row
  ranges, and covers two-worker success, one-worker retry, queue rejection,
  submission rollback, task-allocation failure, serial-oracle fallback,
  release, shutdown, and clean restart cases.
- [x] Source audits cover the worker/kernel prohibition on Direct3D, globals,
  live engine pointers, allocation, waits, RNG, replay/save/network calls.
- [x] Both title display variants initialize the service after their display
  prerequisites and shut it down before render teardown, with game-thread
  assertions.  `W3DRadar` keeps acquisition, join/fallback, surface lock,
  upload, unlock, and release on the owner thread.
- [ ] Focused tests, the modern x86 title builds, and the VC6 compatibility
  build have not yet been executed for the current Stage 5 source.
- [ ] The ten-review-lens code review has not yet been completed.
- [ ] The optimized VC6 replay gate has not yet been executed.
- [ ] Interactive manual acceptance is intentionally deferred until the
  complete Stage 5--8 stack is ready; do not launch this intermediate stage.

## Stage 5 focused validation

Run from a clean, repository-relative build directory.  Do not record local
machine paths, profile paths, build-tree paths, or replay logs in repository
documentation or PR text.

First run the source hygiene check and focused modern build/test targets:

```powershell
git diff --check
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
ctest --test-dir build/win32-debug -C Debug -R "^(radar_terrain_prepare|core_task_runtime|core_texture_mip_buffer)_tests$" --output-on-failure
```

The radar target is expected to exercise serial-versus-two-range byte parity,
edge/clipping and zero-sample behavior, bridge/water/regular branches,
equal-height interpolation guards, supported format packing, row ownership,
bounded owner storage, exclusive leases, two-worker admission, one-worker
retry, queue/backpressure and rollback fallback, shutdown/restart, and the
worker/owner source audits.  A test/build result must be captured before any
checkmark is added to the execution items above.

Then run the legacy compatibility lane using an actual optimized VC6 build;
do not substitute a modern Win32 executable for replay evidence:

```powershell
cmake --preset vc6 -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/vc6 --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
```

If the VC6 toolchain or required game data is unavailable, report that exact
external prerequisite and leave the replay gate pending.

## Stage 5 replay gate

After focused/build validation, use the repository's ten distinct replay
fixtures without modifying the live profile.  The corpus must contain several
2v2 matches, one 2v6 Hard-AI stress match, one 2v2v2 match, one 3v3 match, the
golden/reference replay, and two additional corpus replays.  Run the nine
non-stress replays once and the 2v6 stress replay three times in separate CRC
output directories: twelve replay process executions across ten unique
replays.

Each execution must exit with code 0 and have no CRC mismatch, ownership
failure, assertion, crash, or missing-map error.  The three stress executions
must produce identical CRC file sets and byte-for-byte hashes.  `-jobs 4`
starts independent replay processes; it is not an in-process worker-count
switch.  Record only repository-relative commands and aggregate results, not
machine-specific paths or logs.

# Stage 8 dynamic terrain-light preparation checks

Stage 8 moves only the CPU portion of the existing dynamic terrain-light pass
to the shared bounded preparation service.  The owner still discovers lights,
captures map-derived normals and per-cell current/previous bounds, owns the
immutable backup baseline, locks and updates Direct3D buffers, and runs the
serial updater whenever capture, worker execution, validation, or publication
is not safe.  Optimized-lighting builds remain on their unchanged serial path;
the default non-optimized layout has no shared hardware vertices.

Run the repository-relative focused build and CTest target:

```powershell
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target height_map_dynamic_light_prepare_tests --parallel 2
ctest --test-dir build/win32-debug -C Debug -R "^height_map_dynamic_light_prepare_tests$" --output-on-failure
```

The focused core-extra test covers directional, point, spot, disabled,
attenuated, alpha-preserving, bounds-masked, and tiny-batch serial-cutoff
execution,
NaN/range rejection, overlap and alignment rejection, and unchanged output on
failure.  The existing RadarTerrain service tests cover the shared runtime's
worker identity, rejection ownership, retry, lease, and shutdown behavior.
The owner validates the staged structure without replaying the lighting loop;
the focused kernel checks the complete serial lighting oracle separately.
Interactive display initialization warms the private workers; headless replay
defers worker creation because it never renders terrain. One-row and other
tiny batches use the owner serial cutoff to avoid empty-stripe task overhead.
Point/spot vertices exactly coincident with a light use the documented finite
ambient-only fallback instead of reproducing the legacy divide-by-zero NaN.
The worker source audit must find no D3D, `The*` global, live map/light,
allocation, logging, replay, network, or UI access in the kernel or row task.
The shared runtime contains unexpected worker exceptions after bookkeeping;
texture preparation publishes an owner-visible failure for normal missing-texture
cleanup, and screenshot batches mark conversion/encoding failure before their
owned buffers are released.

The automated Stage 8 gates require both title builds, the complete ten-replay
/ twelve-execution deterministic gate, and all ten review lenses.  These
automated gates are complete for the current exact head.  A live
dynamic-light stress map remains intentionally deferred to manual approval on
the owner-preserving candidate.  Do not promote the candidate or launch the
canonical install before that manual approval.

The ten review lenses for the Stage 8 head are: owner boundary,
complete immutable snapshot, legacy byte parity, disjoint row ownership,
private-runtime wait isolation, deterministic fallback behavior, bounded
memory and lifetime, both-display lifecycle, C++98/VC6 plus replay evidence,
and scoped delivery/privacy hygiene.  All ten review passes and the resulting
fix/retest cycle are complete for the current head.  Repeat them after any
subsequent source change before accepting a new stacked-stage handoff.

# Stage 6 radar overlay preparation checks

Stage 6 keeps radar object/shroud capture and every Direct3D operation on the
owner thread.  Workers receive only immutable ordered commands plus owned CPU
pixels, write disjoint row ranges, and join before owner upload.  Small object
lists stay on the direct owner path; large object lists use command-major row
work.  Shroud overflow folds ordered chunks into the owned image, and failed
worker/upload attempts retain a complete image until an owner commit succeeds
or clear/reset/teardown explicitly supersedes it.

Run the repository-relative hygiene, focused, and both-title checks:

```powershell
git diff --check
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target radar_overlay_prepare_tests radar_terrain_prepare_tests core_task_runtime_tests g_generals z_generals --parallel 2
ctest --test-dir build/win32-debug -C Debug -R "^(radar_overlay_prepare|radar_terrain_prepare|core_task_runtime)_tests$" --output-on-failure
```

The overlay test must cover both supported formats, clipping, inclusive shroud
rectangles, exact object/shroud command order, last-writer-wins, row guards,
serial-versus-split bytes, checked storage limits, lease denial, queue/task
failure fallback, and worker/owner source audits.  Build the same targets with
the optimized VC6 preset before replay validation; a modern executable is not
a substitute for the compatibility lane.

For the Stage 6 replay gate, use the same ten distinct fixtures defined above:
run the nine non-stress replays once and the 2v6 Hard-AI replay three times.
All twelve executions must exit successfully without CRC mismatch, assertion,
crash, missing map, or ownership failure, and the three stress CRC trees must
be byte-identical.  Keep replay state isolated from the playable profile and
record no personal machine paths in commits, documentation, or PR text.

Manual acceptance remains deferred to the user.  When requested, test Stage 5
first and Stage 6 second, checking radar terrain, object markers, stealth
alpha, shroud/fog transitions, radar clears, map return/reload, Alt-Tab/device
reset, dense battles, and clean exit.  Do not start Stage 7 before both manual
test rounds are complete.

# Stage 7 static terrain geometry preparation checks

Stage 7 parallelizes only CPU preparation of max-LOD static terrain vertices
and static lighting.  The owner captures map and light state, owns all Direct3D
and backup publication, joins workers synchronously, validates the complete
output, and falls back to the unchanged serial path on any failure.  Simulation,
AI, pathfinding, networking, replay and save state, RNG, camera behavior,
terrain mutation, lower LODs, and dynamic lighting remain serial.

Run the repository-relative hygiene, focused, and both-title checks from clean,
isolated build trees:

```powershell
git diff --check
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target height_map_terrain_prepare_tests radar_terrain_prepare_tests radar_overlay_prepare_tests height_map_dynamic_light_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests core_screenshot_codec_tests core_miles_audio_completion_tests core_camera_audio_policy_tests g_generals z_generals --parallel 2
ctest --test-dir build/win32-debug -C Debug -R "^(height_map_terrain_prepare|radar_terrain_prepare|radar_overlay_prepare|height_map_dynamic_light_prepare|core_task_runtime|core_texture_mip_buffer|core_screenshot_codec|core_miles_audio_completion|core_camera_audio_policy)_tests$" --output-on-failure
```

Repeat the focused targets and both game targets with `win32-profile` using the
Release configuration.  Build and run the same targets with the VC6 preset;
disable precompiled headers if required by the legacy compiler.  A modern
executable is not a substitute for the VC6 compatibility lane.

The terrain suite must cover exact serial-versus-partitioned byte parity,
normals, flip/UV/alpha, origin and border capture, static-light branches, depth
fade, layout and guard bytes, checked bounds, complete output validation,
worker admission, join/fallback, cleanup, shutdown, and restart.  Source review
must confirm that workers access no D3D/DX8 object, live map/light/global
pointer, allocation, wait, logging, RNG, replay/save/network state, or
title-specific API.  Both titles must retain correct shared-service
initialization and shutdown ordering.

After focused checks, both-title builds, and ten review rounds, run the ten
distinct replay fixtures defined above.  Execute the nine non-stress fixtures
once and the 2v6 Hard-AI stress fixture three times: twelve isolated executions
total.  Every run must exit successfully without crash, assertion, ownership
failure, missing map, CRC mismatch, or desync.  The three stress runs must
produce byte-identical CRC file sets.  Run three fresh AI-vs-AI headless smoke
games when the established runner is available.

Use a VC6-compatible artifact from the exact final commit and an isolated
disposable runtime and profile.  Do not use the live profile or playable
installation.  Record only repository-relative commands and aggregate results;
never commit machine paths, personal profile paths, or local build/log paths.
Do not mark Stage 7 ready until all focused tests, both-title modern and VC6
builds, ten review rounds, and the full replay gate have passed.

# Pathfinding capacity checks

The pathfinding-capacity change addresses the observed eight-player ground-unit
freeze by expanding only the Zero Hour `PathfindCellInfo` pool for live games
and recordings carrying `[PathfindQueueEpoch=1]`. The legacy 30,000-record pool
remains selected for unmarked replay playback, preserving the historical
allocation behavior. The 150,000-record pool is a bounded 32-bit allocation;
it does not change the path request ring, per-frame cell budget, queue order,
simulation serialization, or network commands. The pool is selected again at
map activation after replay metadata has been read, because the AI subsystem is
constructed before the recorder during engine startup.

Run the focused policy and title checks from isolated modern x86 build trees:

```powershell
cmake --preset win32-profile -DRTS_BUILD_GENERALS=OFF -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON -DRTS_BUILD_ZEROHOUR_EXTRAS=ON
cmake --build build/win32-profile --config Release --target z_runtime_regression_tests z_generals
ctest --test-dir build/win32-profile -C Release -R "^z_skirmish_ai_replay_epoch_tests$" --output-on-failure
```

Build the Generals title separately with `RTS_BUILD_GENERALS=ON` and
`RTS_BUILD_ZEROHOUR=OFF`; the shared pathfinding source must compile in both
title configurations. The replay-epoch regression test must cover unmarked,
current, combined Skirmish/pathfinding markers, duplicate/unknown markers,
idempotent writing, and live-versus-replay policy selection.
The test intentionally exercises the pure capacity-policy helper; actual pool
allocation and map-activation behavior remain covered by both title builds and
the isolated replay gate below.

For diagnostic profiling only, define `PATHFIND_DIAGNOSTICS` and enable
`RTS_DEBUG_LOGGING=ON` in an isolated profile build. Review aggregate
`PATHFIND_STATS` records for queue depth,
queue-full events, pool high-water, and allocation failures. Diagnostics must
not bypass replay version/CRC checks and must never be used as the compatibility
replay artifact. The target stress run should show pool failures at the legacy
limit but no failures with the 150,000-record live pool; queue saturation is a
separate signal and must not be inferred from pool pressure.

After the final VC6 optimized build, run the complete replay gate described
above: ten distinct fixtures and twelve executions, with the 2v6 Hard-AI
fixture executed three times and its CRC traces byte-identical. Require zero
exit status and no CRC mismatch, assertion, crash, missing map, or ownership
failure. Keep replay state isolated and do not record machine-specific paths in
commits, documentation, or pull-request text.

# Miles completion callback checks

The Miles EOS callbacks must only publish a fixed-size `{handle, type, generation}` record. They must not enter `TheAudio`, call Miles APIs, allocate, or take the audio-cache mutex. The owner-thread `MilesAudioManager::update()` drains one queue snapshot per frame; reset and shutdown close admission before unregistering callbacks and releasing handles, then clear queued generations. On overflow, the owner drains status-visible stopped handles and uses the compatibility fallback rather than waiting in a callback.

Build and run the bounded queue test with Core extras enabled:

```
cmake --build build/win32-debug --config Debug --target core_miles_audio_completion_tests
ctest --test-dir build/win32-debug -C Debug -R "^core_miles_audio_completion_tests$" --output-on-failure
```

The test covers FIFO delivery, concurrent producers, bounded overflow/recovery, snapshot draining, close/reopen admission, reset generations, and exact-once records. A source audit must show no callback-to-manager call:

```
rg -n "set(Sample|3DSample|Stream)Completed|TheAudio->notifyOfAudioCompletion" Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp
```

The callback declarations/definitions may match, but the callback bodies must contain only `tryPublish`; all `notifyOfAudioCompletion` calls must be on the owner-thread drain/recovery path. During manual gameplay, stress dense combat and rapid sound effects, reset/return to shell, alt-tab, and exit while sounds are active. The game must remain responsive and must not show the previous audio hang; no callback-thread stack may contain `ScopedMutex::Lock` or `AudioFileCache` operations.
