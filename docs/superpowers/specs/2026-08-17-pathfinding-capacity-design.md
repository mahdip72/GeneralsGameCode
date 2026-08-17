# Pathfinding Capacity and Replay Compatibility Design

## Problem

Large Zero Hour skirmishes can leave ground units waiting for a path while
air units continue to move. The path request ring remains below its 511-entry
usable limit in the observed eight-player stress replay, while the shared
`PathfindCellInfo` pool reaches its 30,000-record limit and refuses transient
A* allocations. A refused allocation leaves a ground request waiting for a
later retry; persistent pressure therefore appears as frozen movement.

## Scope

This change increases only the Zero Hour cell-info capacity used by live games
and by recordings that explicitly carry the new pathfinding epoch marker:

- legacy/unmarked replay playback: 30,000 records;
- live Zero Hour games and `[PathfindQueueEpoch=1]` recordings: 150,000 records;
- Generals remains on its existing 30,000-record behavior.

The path request ring, `PATHFIND_CELLS_PER_FRAME`, queue order, A* algorithm,
simulation state, replay commands, CRC/Xfer data, save format, network traffic,
RNG, and AI update order are unchanged. The larger pool is a bounded 32-bit
allocation (approximately 7.2 MiB of record payload) selected at map
activation. The AI subsystem is constructed before the recorder, so startup
keeps the legacy pool and `Pathfinder::newMap()` applies the selected policy
after replay metadata is known.

## Replay marker

New Zero Hour recordings append `[PathfindQueueEpoch=1]` to the existing
variable-length replay build-time field before the existing Skirmish-AI marker.
The parser accepts exactly one current path marker and rejects unknown,
duplicate, or malformed markers. The existing Skirmish marker remains the final
suffix so its parser and old recordings remain compatible. Unmarked recordings
continue to use the legacy allocation policy.

## Ownership and failure behavior

The pool is simulation-owner state and is never accessed from a worker. A pool
replacement prepares and initializes a new array before releasing the old
free-list, so an allocator failure leaves the old pool intact. Replacement is
allowed only before a map owns path cells; an active-map resize is rejected.
Retail failover clears every record, including the terminal free-list entry.
Diagnostics are opt-in counters/logs only and do not bypass replay version or
CRC checks.

## Validation

The Zero Hour regression test covers live/replay policy selection, path-only
and combined marker parsing, duplicate/unknown markers, idempotent writing,
and the existing Skirmish epoch combinations. Modern x86 profile builds cover
both title targets and the registered Core suites. The release-compatible VC6
artifact must pass the repository's ten-fixture/twelve-execution replay gate,
including three byte-identical 2v6 stress CRC traces, before manual testing.
