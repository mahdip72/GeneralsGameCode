# Deterministic AI planning focused test

This directory is registered by the shared `Core/Tools/CMakeLists.txt`. It
exercises the fixed counter-RNG vector, enemy and production POD scoring,
stable ordering, malformed-runner fallback, and the real two-worker JobSystem
runner without linking GameEngine.

The Zero Hour x64 skirmish adapters are integrated directly. The broader
`RTS_ENABLE_STAGE5_AI_PLANNING_ADAPTERS` macro remains undefined; its legacy
base-`AIPlayer` adapters are not part of this slice. Replay epochs 0 through 2
continue through their existing global game-logic RNG and serial AI paths.

Integration contract:

1. On the game thread, walk players by ascending player index. Capture enemy
   facts before the existing `AIPlayer` subphases.
2. For adaptive production, capture candidate eligibility, perform the existing
   reinforcement decision on the game thread, and only when it declines finish
   the immutable economy/composition/route snapshot.
3. Schedule at most one `PlanAIPlayer` call per snapshot. The callback receives
   no GameEngine object, global, or mutable RNG state.
4. In shadow mode, compare the complete serial and parallel result batches. On
   capture overflow, runner failure, mismatch, or failed stable-ID resolution,
   discard every uncommitted parallel result and run the deterministic serial
   snapshot planner.
5. Commit only validated results, ordered by `(frame, playerIndex, subphase,
   sourceOrdinal, emissionOrdinal)`. Target commits precede production commits
   within one player; no worker writes live AI state.
6. Keep LAN/Internet sessions on the serial snapshot planner until the mixed
   worker-topology determinism gate exists.
