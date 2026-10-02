# Stage 5 large-battle FPS follow-up

Status: scope only. Performance implementation has not started.

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

After explicit authorization to resume, establish comparable battle captures at
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

This document opens the separate follow-up without modifying game code,
profiling the accepted release, or starting optimization work. The follow-up
remains paused until the user requests continuation. The merged release retains
the reported large-battle FPS limitation and outstanding broad qualification.
