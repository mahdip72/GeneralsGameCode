# Stage 5 large-battle FPS follow-up

Status: performance investigation and implementation in progress. PR #24 is
still a draft. No large-battle FPS acceptance result is claimed.

## Accepted baseline

PR #19 was merged at `531ed2fe94d2f2468d2eb3ade8d8f795c8d4d2a1`.
Its source tree matches the manually accepted candidate built from
`3d612ca63080147198a296278198b61605b9eb47`.

Manual feedback reports good general operation and high frame rates away from
large battles, including an eight-player match. Moving the camera over a large
conflict still causes substantial rendering slowdown. Earlier feedback reported
multi-second visual stalls while audio continued. Their absence in later manual
play is useful feedback, not proof that every stall has been eliminated.

## Scope for authorized continuation

- Improve frame rate and frame pacing when the camera covers large battles.
- Preserve the accepted visual appearance, effects, quality settings, and native
  Direct3D 11 rendering behavior.
- Preserve the stable merged AI behavior and deterministic simulation contracts.
- Treat menu performance as a supporting workload, not a substitute for battles.

## Evidence and acceptance

Establish comparable battle captures at
fixed resolution, graphics settings, camera position, and game state. Retain
build identity and distinguish simulation time, CPU render work, GPU work, and
presentation waits before selecting optimizations.

The user-created replay named `performance testing` may provide a useful scene.
Verify its format and compatibility before relying on it. Do not call a legacy
replay a native qualification fixture.

Require before/after timing evidence and temporal visual inspection of the same
workload. Preserve fidelity and verify relevant source tests, replay contracts,
and representative gameplay. Keep local testing within six physical cores and
twelve logical threads. External physical-core qualification remains deferred
and must not be reported as passed.

## Current boundary

The first instrumented menu capture separates main-thread CPU phases from
render-owner packet execution. Owner packet segments are not unique frames.
Scopes are inclusive and may overlap. These CPU measurements do not establish
GPU time, a visually verified large-battle workload, or normal Release FPS.
The game CSV retains its existing `RTS_FRAME_TIMING_DIR` opt-in and fifteen-column
format. Owner sidecars require a separate `RTS_RENDER_OWNER_TIMING_DIR` opt-in
and directory, so strict single-CSV scaling receipts remain unchanged.

The capture points toward client scene preparation, sorted submissions,
particle rendering, and shadow-manager work. Additional subphases separate
command enqueue, shadow preparation and submission, decal queue and flush, and
particle selection and submission. Startup and partial tail buckets are not
steady-performance evidence.

The dynamic buffer change removes redundant zero stores from the DISCARD
prefix immediately overwritten by uploaded bytes. It retains the complete
CPU mirror, initialized ranges, content versions, upload modes, and failure
behavior. Native pixel and lifecycle coverage uses a real windowed D3D11
fixture. Supplementary recording doubles check exact private mirror bytes and
failure branches but do not replace the real backend or game tests.
The producer and threaded owner also avoid copying old initialized ranges that
DISCARD immediately replaces. Their staged publication and failure branches
remain intact. Layout-state commands copy-initialize the supplied complete
state instead of constructing defaults and then overwriting them.

Sorted geometry can now coalesce adjacent source submissions only when their
complete logical state, exact float representations, texture generations,
geometry declarations, and captured pass agree. It preserves ordered triangle
vertices and source acknowledgement boundaries. The fixture's reduced draw
count is a structural assertion, not a measured frame-rate improvement.

The native replay wrapper verifies executable compatibility. A replay recorded
by the accepted executable cannot be assumed compatible with a new renderer
build. The guard remains intact. The supported fixed-seed rendered practical
scenario can provide matched map, player, and faction inputs, but its camera
and frame interval still need to be recorded for a performance comparison.

The first Computer Use attempt could identify the installed game window, but
window activation failed. A subsequent read-only capture succeeded without
input injection and sampled twenty animated showcase frames over about ten
seconds. Unusual transient palette changes still need reference comparison;
that baseline observation does not establish candidate fidelity or battle FPS.
Control was released and each exact test game and launcher exited normally.

A bounded inspection of the combined candidate also reached the visible menu.
Its physical client was 1920 by 1080 on a 3840 by 2160 primary display at
144 DPI. Twenty successive screenshots sampled about ten seconds of animation.
Menu text fit the controls. This observation does not establish continuous-video
parity, other-resolution coverage, or a matched performance gain. Colored light
pulses around moving vehicles and nearby trees still need a comparable reference.
This was the first, pre-commit optimization batch, not the subsequent
effect/submission batch. Its inspected game SHA-256 was
`EF9C2B06B473291388B00DA6545D33A4DD7B66F92524F37EA75A99B79DB2FA9B`.
Both normal Release products built and twelve focused renderer/diagnostic suites
passed for that batch only. The inspected game and launcher exited with code
zero. The first batch was subsequently committed at
`b59123c0946ee51b4940d43c6630a68096a2a614`.

The subsequent effect/submission batch adds a guarded billboard packing path,
pipeline-validity queries, owned-value draw snapshots, and pinned completion
polling. Its tested source is that commit plus eleven source/test changes,
identified by frozen source-manifest SHA-256
`A3DA993EB526ACCBD44AF26BC33DA95EC00178FAB034E77F866B381C57087771`.
Both normal Release products built and all nineteen selected suites passed
(`effects-submission-build-04` and `effects-submission-tests-02`). This includes
the corrected completion-poll fixture and serial/parallel recovery tests.
The isolated game's SHA-256 is
`57A8DFE6412FEE7266D02E7AA3D79D222231835017B01A614F8B132F5E33EFB9`.
These results do not establish battle FPS or visual fidelity. The source pin
predates this documentation clarification; no compiled source changed with it.

The accepted stable installation and live profile remain unchanged. Matched
before/after battle timings, successful temporal visual inspection, independent
review gates, relevant simulations and replay validation remain pending.
External physical-core qualification is deferred, not passed.
