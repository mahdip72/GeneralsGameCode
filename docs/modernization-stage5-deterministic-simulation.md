# Modernization Stage 5: deterministic simulation validation

Stage 5 keeps one authoritative game-thread owner. Workers may read immutable
snapshots and write disjoint private output only. The owner compares, reduces,
and commits results in canonical order. Workers must not mutate live objects,
consume gameplay random streams, publish messages or commands, create or
destroy game objects, or access replay, network, render, audio, UI, or client
state.

## Runtime controls

- `-simulationMode serial|parallel|shadow` selects the simulation policy at
  startup. The executable itself remains serial by default; the installed
  native launcher selects parallel simulation with automatic workers. Passing
  `-simulationMode serial` to the launcher appends an explicit serial override.
- `-pipelineMode serial` remains fixed during the deterministic-runtime matrix
  so the Stage 4 preparation policy does not confound isolated simulation
  comparisons and the request matches replay runtime's enforced serial
  pipeline. This matrix is not the final Stage 5 acceptance gate.
- The separate combined-policy installed-runtime lane requires
  `-pipelineMode parallel -simulationMode parallel -workerPolicy auto`, the
  dedicated D3D11 render thread, and both titles. It proves that Stage 4 and
  Stage 5 operate together after isolated simulation validation.
- `-workerCount 1|2|4|8|16` selects an explicit in-process worker count.
- Automatic worker selection is `-workerPolicy auto` with no `-workerCount`
  argument. `-workerCount auto` and `-workerCount 0` are invalid.
- `-validationExecutableSha256 <64hex>` supplies externally computed candidate
  provenance. The game does not hash its own executable.
- Replay `-jobs` starts independent processes. It is not an in-process worker
  control and is not used by the Stage 5 matrix orchestrator.

Replay worker children must preserve explicit worker count, worker policy,
pipeline mode, and simulation mode. `Audit-ReplayModePropagation.ps1`
enforces that contract while rejecting recursive process-level `-jobs`.

## Pathfinding worker boundary

Stage 5 has two request-local pathfinding worker lanes. The compact direct lane
retains its bounded prefix of at most 16 eligible requests and immutable
`DirectPathSnapshot` facts. The ordinary lane covers successful ground A* paths,
including the Generals and Zero Hour hierarchical block-corridor preflight. It
scans the complete current FIFO queue and has no product cap of 16 requests.
Dequeue order, the 5000-cell frame budget, `doPathfind`, and AI side effects stay
on the game owner and remain in their original order.

Before copying the navigation grid, the owner performs exact request-local
admission against the live map. Parallel authority requires at least two
eligible ordinary requests and more than one compute worker; shadow comparison
requires one eligible request. Zero- and one-request parallel-authority frames
therefore do not scan or copy the full grid. Once the threshold is met, the
owner publishes one immutable generation containing fixed-width ground cell,
zone, occupancy, layer-connection, bridge-interaction, and hierarchy facts. All
requests in that batch reference the same generation. Request-local A* nodes,
heaps, hierarchy masks, output paths, and accounting plans are private worker
storage, with bounded allocation and stable legacy cost/insertion tie-breaking.

One adaptive `JobGroup` partitions the admitted ordinary requests into ranges
based on request count and physical worker count. Workers never wait or touch
live game objects. The owner performs one bounded worker-only join, rejects a
stopped/cancelled/timed-out group before mutation, validates the shared
generation, and visits results in original FIFO/request-ID order. At each real
`findPath` boundary, the unchanged legacy hierarchy preflight runs first. The
owner then revalidates the object, request token, endpoints, zone, complete
hierarchy passability mask, immutable result shape, allocation/cleanup order,
and exact constant-time `PathfindCellInfo` capacity. A valid result is
materialized in legacy allocation, open/closed-list, parent-chain, path-build,
cleanup, centering, height, optimization, and debug order. Once mutation begins
the request is terminal; any earlier unsupported, stale, malformed,
capacity-short, `NO_PATH`, or fault result uses the unchanged serial search.

The compact direct lane keeps its exact legacy supercover callback stream,
including the post-goal callback, eight start-neighbor facts, combined
topology/occupancy generation and token, and transactional pool/list accounting.
Its worker result is authoritative only after the same physical-worker,
generation, owner, shape, capacity, and live-fact validation. Direct and
ordinary batches both own their payloads; a cancelled late worker may update
only a diagnostic and cannot publish accepted execution or authority.

The fixed pathfinding semantics needed for ordinary result parity are selected
explicitly when a map starts. They are enabled only for the current native
compatibility epoch. VC6 and non-native builds, network/multiplayer sessions,
legacy or unmarked playback, and Generals recording/playback keep retail serial
semantics. A marked current Zero Hour replay may use the matching fixed serial
semantics needed to replay its recording, but replay never accepts ordinary
worker authority. `serial` remains worker-free. `shadow` may execute eligible
ordinary searches and compare their raw path/accounting plan with the serial
owner result, but it never commits worker output.

Path metrics are process-local diagnostics outside serialized and CRC state.
Direct metrics retain eligible/submitted/executed identity, owner help,
authoritative and concurrently multi-worker-backed commits, failure/fallback,
callback range, timeout, late-drain, and peak-worker fields. Ordinary metrics
separately report eligible requests, submitted requests and adaptive ranges,
physical-worker-executed requests, authoritative commits, authoritative commits
backed by concurrent execution on more than one physical worker, stale and
validation rejection, serial fallback, shadow comparison/mismatch, owner
timeout, late drains, peak active workers, and maximum batch/range/grain sizes.
Global scheduler or compact-direct counters cannot proxy ordinary A* work.

The installed lifecycle freezes both path metric sets in the active Pathfinder
reset epoch before clear-game teardown. A qualifying parallel 4v2 stress record
must contain positive ordinary physical-worker execution and owner commits,
including a commit correlated to a batch with more than one simultaneously
active physical worker. Stale acceptance, validation failures, timeouts, and
shadow mismatches must be zero. Serial and parallel one-worker records cannot
claim ordinary submission or authority; shadow records may compare but cannot claim
authority. These ordinary gates are required in addition to the compact-direct
gates and do not self-advertise multiplayer PATH support.

## Partition collision candidate prefix

The collision slice parallelizes only pointer-free candidate preparation. The
game owner traverses the live cells, captures object IDs, synthetic snapshot
generations, owner transform/geometry, cell spans, and participant identity,
then submits immutable ranged work. Workers never dereference an `Object`,
`PartitionData`, cell, or contact list. Owner reduction preserves legacy
discovery and prepend orientation, and the owner re-resolves every generation
and re-traverses the exact live cell topology before publishing contacts.
Narrowphase and `onCollide` callbacks remain on the legacy owner path.

Admission uses a fixed 64-slot deterministic reservoir over the complete
eligible encounter stream, rather than the first 64 encounters. Later cells
and duplicate-heavy tails can replace early samples; slices below 256 total
occupants or below 75 percent unique sampled IDs remain legacy serial. Snapshot
storage is reusable owner-owned bounded memory, and any invalid topology,
stale generation, allocation/submission/cancellation failure, multiplayer
policy, stopped scheduler, or unsupported state returns to the exact legacy
path without partial candidate publication.

## Structured live-match evidence

Successful `-runSkirmishAITest` and `-runSkirmishAITest4v2` completion lines
include:

- requested and independently captured loaded seed, scenario, actual AI
  count/team split, executable SHA-256 input, simulation mode, requested worker
  count, maximum observed effective worker count, and worker policy;
- live requested/effective pipeline and simulation policies captured from the
  runtime policy state rather than inferred from command-line text;
- the owner-supplied final canonical digest and wall-clock duration;
- submitted, executed, stolen, owner-helped, waited, rejected, failed,
  cancelled, and fallback job counts;
- direct-path eligible/submitted/executed, physical-worker and owner-helped
  execution, authoritative commit and fallback/rejection counts, forbidden
  authority invariants, bounded multi-worker peak, and callback range;
- ordinary-path eligible/submitted requests and adaptive ranges,
  physical-worker execution, authoritative and concurrently multi-worker-backed
  commits, shadow comparisons, fallback/rejection health, peak workers, and
  maximum batch/range/grain gauges;
- physics integration authoritative batches/prefixes/ranges and exact
  submitted/completed jobs, allocation/storage and capture/prepare/wait/commit
  costs, plus independent shadow, fallback, stale, and circuit-breaker health;
- total and maximum queue latency, sleeps, wakes, affinity failures, queue
  high-water mark, peak active workers, and CPU-selection counts.

Missing executable provenance, mode, or final digest remains visibly
`unavailable` or `unknown`. The installed-runtime validator rejects incomplete
manifests instead of inferring those values.

## Explicit fixture manifest

`Core/Tools/DeterministicSimulationValidation/ReplayFixtureManifest.schema.json`
defines the manifest. All executable, replay, and custom-map sources have an
explicit SHA-256. Fixture paths are relative to the manifest and cannot escape
its directory. Custom-map destinations must stay below the isolated profile's
`Maps` directory. The loader enforces the schema's exact object properties and
native JSON types before conversion; strings are never coerced to booleans or
integers.

For a candidate built during the current CI run, the controller computes the
installed executable hash and supplies it through
`-ExpectedExecutableSha256`; this overrides only the manifest's candidate hash,
not any fixture or map hash. The selected source (`manifest` or `argument`) is
recorded in `validation-plan.json`, and the executable is rehashed before every
launch and after the matrix. The game receives that externally computed value
through `-validationExecutableSha256` and never hashes itself.

The standard candidate manifest contains exactly ten replay fixtures and marks
exactly one 2-vs-6 Hard-AI fixture as stress. `ReplayFixtureManifest.example.json`
is structural only: replace its zero hashes and example paths with reviewed
fixtures before use. `-AllowNonStandardCorpus` is restricted to plan-only or
explicit diagnostic work; it cannot execute a passing deterministic-runtime gate.

## Synchronous installed-runtime matrix

`Run-DeterministicSimulationValidation.ps1` accepts an installed Generals or
Zero Hour runtime containing `launcher.exe`, `launcher.lcf`, and the exact
candidate executable. The manifest title, executable, and evidence are bound
to the selected title; it never launches a build-tree executable and never
overlaps an existing game process.

The script:

1. verifies the candidate, replay, and optional map hashes;
   fixture IDs and derived replay paths must also be unique under Windows
   ordinal-ignore-case comparison;
2. checks free-space reserves and requires a fresh evidence directory;
3. creates a unique isolated Documents profile and stages only manifest files;
   every staged replay/map is rehashed after copy and the complete staged
   corpus is rehashed again before registry mutation or execution;
4. saves and sets both 32-bit and 64-bit HKCU registry views;
5. launches one installed executable synchronously per case with a timeout;
6. requires exactly one frame-timing CSV per execution, validates its exact
   header, rows, finite numeric fields, headless mode, frame range, and frame/logic phases, then
   records its SHA-256 with stdout, stderr, wall time, and exit status;
7. rejects CRC, ownership, assertion, replay-read, missing-map, timeout, crash,
   and structured-manifest failures;
8. requires and parses one `SIMULATION_JOB_METRICS` and one
   `SIMULATION_REPLAY_RESULT` record plus exactly one reset-aware collision and
   physics manifest per replay before scope teardown, then compares the authoritative
   recalculated final `GameLogic` CRC and final frame for the same source replay
   across every repeat and worker configuration;
9. compares `final_digest`, `end_frame`, and `winner_team` for each live-AI
   scenario/seed across every repeat and worker configuration, and binds the
   timing final frame to the completion manifest;
10. records AI-planning, collision, direct-path, ordinary-path, and physics authority counters
    separately from global scheduler activity; requires a 4v2 stress execution
    with AI-specific submitted/completed owner commits, collision-specific work,
    a direct-path authoritative commit backed by physical-worker execution, and
    an ordinary-path authoritative commit backed by a multi-request batch with
    concurrently active physical workers, and
    physics-specific authoritative batches/prefixes/ranges/submitted/completed
    jobs;
    adds one installed 16-worker 4v2 `simulationMode=shadow` execution requiring
    positive collision shadow executions, positive successful legacy insertions
    covered by the exact order/orientation comparison, prepared candidates, and
    balanced submitted/completed jobs; positive matching physics shadow
    prefixes/ranges/submitted/completed jobs; zero collision/physics mismatches
    and unexpected fallbacks; and all collision compare timing phases; rejects
    unsupported, shadow, stale, or malformed path authority; and
11. registers each registry snapshot in the outer restore list as part of one
    setup transaction; any create/open/write/registration failure immediately
    restores the original value and reverse-removes every validation-created
    empty ancestor before reporting setup plus rollback errors; and
12. attempts every registered registry restoration independently in reverse
    order, removes validation-created keys only while they remain empty, and
    reports aggregated restoration errors after all keys have been attempted.

It leaves the isolated profile and evidence directory for review. Cleanup is a
separate, explicitly scoped operation.

Use `-PlanOnly` first to validate hashes and inspect `validation-plan.json`
without changing registry/profile state or launching a process:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File Core/Tools/DeterministicSimulationValidation/Run-DeterministicSimulationValidation.ps1 `
  -RuntimeRoot <installed-runtime> `
  -FixtureManifestPath <fixture-manifest.json> `
  -OutputRoot <fresh-evidence-directory> `
  -ValidationSet All `
  -PlanOnly
```

Remove `-PlanOnly` only after reviewing the plan and confirming that no game
process is running. `-DisableFrameTiming` is rejected for the deterministic-runtime
gate. It is available only with `-DiagnosticNonAcceptance`, whose plan and
completion output are focused diagnostic evidence. Only `-ValidationSet All`
with performance enforcement can pass the deterministic-runtime gate;
`Replay` and `AI` remain focused partial gates. None of these modes claims final
Stage 5 acceptance.

### Replay gate

The seven configurations are serial-1, parallel-1/2/4/8/16, and
parallel-auto. For each configuration and matrix pass, run the nine non-stress
fixtures once and the stress fixture three times. Two complete passes produce
168 isolated replay executions. Every execution must exit zero without a CRC
mismatch or fatal marker. The ten fixture IDs must resolve to ten unique replay
source hashes. Every replay run explicitly requests the serial Stage 4 pipeline
while varying only Stage 5 simulation workers. Structured job metrics and the
authoritative `SIMULATION_REPLAY_RESULT` final frame/CRC must agree across all
repeats and worker configurations; timing maximum frame must also match the
structured result. A forced one-worker simulation may report its expected
serial kernel fallback; fallback in another parallel configuration fails.

The existing optimized VC6 replay job remains the retail compatibility oracle.
It does not validate a native x64 candidate; both gates are required.

### Live AI gate

The deterministic-runtime manifest explicitly lists both scenarios (`4v3`, `4v2`), at
least three distinct seeds, and a positive repeat count. Admission and result
reduction both require the exact unique scenario-by-seed-by-configuration-by-repeat
cross-product. The same seven regular worker configurations run every case, followed
by one bounded installed 16-worker shadow execution for the first 4v2 seed.
Each match records
and validates its replay and must emit a complete structured manifest with
zero failed or cancelled jobs. For each scenario/seed, `final_digest`,
`end_frame`, and `winner_team` must be identical across every repeat and worker
configuration. The reported scenario, actual AI count, and actual team split
must match the planned 4v3 or 4v2 topology. Only the forced one-worker lane may
report the expected serial kernel fallback. The deterministic-runtime gate marks 4v2 as the
live-AI stress scenario and requires at least one parallel stress execution
with `authoritative_commits > 0` plus positive AI-specific submitted/completed
job counts, authoritative collision evidence, positive compact-direct
physical-worker/owner-commit evidence, and independently positive ordinary A*
physical-worker/owner-commit evidence. Global JobSystem traffic, AI/collision
counters, compact-direct counters, owner help, or shadow activity cannot proxy
ordinary path authority. Unsupported/shadow direct authority,
stale/malformed direct acceptance, ordinary stale/validation rejection,
ordinary timeouts, and all path shadow mismatches must remain zero.
The same qualifying stress result must independently show positive physics
authoritative batches, committed prefixes, ranges, and balanced submitted and
completed jobs. AI, collision, direct-path, or global scheduler work cannot
proxy physics. Physics shadow mismatches, owner or unexpected fallbacks, stale
rejections, and circuit-breaker trips must remain zero; below-grain serial
ineligibility remains an explicit physics-local counter.
The collision shadow run must report positive collision shadow executions,
`collision_shadow_compared_candidates > 0` for successful legacy insertions
covered by the exact final order/orientation oracle, prepared/unique candidates,
and equal submitted/completed collision jobs, with zero collision shadow
mismatches and unexpected fallbacks. Physics shadow evidence independently
requires positive executions/matches, prefixes, ranges, and equal
submitted/completed shadow jobs while authoritative physics publication remains
zero. Collision authority is valid only in parallel mode and collision shadow
work only in shadow mode; the same mode isolation applies to physics. Shadow
executions, owner fallbacks, validation failures, and rejected commits remain explicit evidence
fields. Collision and physics use process-local reset epochs: the authoritative
new-game reset advances both, shell/pre-match values are ignored, teardown
cannot erase frozen evidence, and a second replay/match starts from zero.
Serial evidence requires every collision-lane work counter to remain zero. The
forced parallel-1 lane may report its expected owner fallback, but must report
zero prepared/unique candidates, submitted/completed collision jobs, authority,
shadow comparison, stale publication, and unexpected fallback.
Focused `AI` runs may report the slice schema as unavailable, but cannot pass
the complete deterministic-runtime gate.

## Shadow and failure gates

Most shadow validation operates within a real production phase using private
serial and parallel outputs. The partition collision lane uses a stronger
post-mutation legacy oracle because contact-list insertion orientation,
existing-pair filtering, and legacy prepend behavior are adapter semantics:

1. the owner builds one immutable snapshot;
2. workers prepare the pointer-free candidate sequence and the owner validates
   live generations and exact cell topology;
3. the owner filters the expected sequence against contacts that existed before
   this dirty-object encounter;
4. the exact legacy cell/contact traversal commits to the real contact list and
   records only successful insertions;
5. the recorded inserted prefix is reversed to its final prepend orientation;
   and
6. complete structured fields are compared after the real legacy commit.

A collision mismatch identifies frame, `partition_contact_commit` phase,
stable item index, first differing field, expected/actual counts, and structured
expected/actual object IDs. It is fatal validation evidence. The live list is
already the exact legacy result, so no alternate or partial prepared publication
has occurred, and later narrowphase/callback destruction behavior remains
legacy. Collision comparison is not a pre-mutation digest or a second call to
the shared candidate kernel.

Focused extras tests inject snapshot/output/group/job/scratch allocation
failure, queue capacity and every partial-admission ordinal, worker failure,
cancel-before-start, cancel-during-execution, cancel-before-reduction, map
reset, and shutdown. Each failure must leave state unchanged or serially
recomputed, publish no partial output, join accepted work, destroy each payload
once, and leave no outstanding completion.

## Mixed-worker multiplayer gate

Replay playback is not multiplayer evidence. Before parallel simulation can be
enabled for network games, `Net3LoopbackEvidence.schema.json` and the strict
PowerShell parser require exact installed-runtime NET3 loopback peers covering:

The Stage 5 compatibility boundary retains the legacy trusted packet-router
session model. Direct peer traffic is bound to the negotiated endpoint, and
wrapped commands must preserve the claimed player slot through decoding and
recovery, but these unkeyed checks are not cryptographic player
authentication. A hostile packet router can impersonate a relayed origin.
Resistance to that threat requires a separately versioned protocol carrying a
per-origin MAC or signature over the session, epoch, player slot, frame,
sequence, and command bytes. Stage 5 documentation and release evidence must
not describe endpoint, token, CRC, or wrapper equality checks as protection
against an on-path attacker or malicious router.

- two peers at 1 versus 16 workers;
- two peers at 2 versus automatic workers;
- two peers at 4 versus 8 workers; and
- four peers at 1, 2, 8, and automatic workers.

The two fixed nonzero seeds are 23063 (`0x5a17`) and 49374 (`0xc0de`). The
canonical order is title, topology, then seed, producing exactly 16 match
records: two titles times four topologies times two seeds. The topology rosters
produce exactly 40 nested peer records overall, 20 per title. Missing, extra,
duplicate, reordered, or topology-inconsistent match or peer records fail.

Every match and peer binds the exact lowercase source commit, uppercase
artifact-set SHA-256, title executable SHA-256, successful NET3 readiness,
exact roster SHA-256, and the diagnostic-only integrated policy mask `0x3f`.
This v1 mask describes the kernels exercised by the diagnostic harness; it is
not a product authority claim and cannot enable live multiplayer workers. Each
peer records the same final frame/CRC, exit code zero, and clean shutdown.
Physics, status,
collision, AI planning, immutable-spatial, and path kernels each report their
fixed bit, submitted/completed counts, physical-worker jobs, owner-helped jobs,
physical-worker mask, distinct physical workers, and peak physical-worker
concurrency. Submitted and completed must match the physical-plus-owner
execution accounting. Every advertised kernel on a multicore peer must show
positive concurrent work on more than one distinct physical worker; a
forced-one peer must show exactly zero scheduler work. Collision evidence comes
from the actual parallel collision-candidate kernel over a qualifying batch and
uses that kernel's own scheduler counters, not an outer validation-job label.
An automatic peer must resolve to more than one effective worker.

`New-MultiplayerSimulationReleaseProof.ps1` accepts the evidence only alongside
independently supplied source, artifact-set, both-title executable hashes, and
the selected title's build/content CRCs. The installed runner must identify its
scoped validation mode and provide 40 hashed raw peer-process outputs with an
independently observed executable and artifact-set hash for every peer. Only
after full validation does the command create an external diagnostic
`MultiplayerSimulationRuntimeProof.txt` for the exact prebuilt executable. A
runtime sibling file cannot grant multiplayer authority: ordinary builds embed
a zero trusted mask, and the v1 resolver rejects this diagnostic schema even if
a caller supplies a nonzero mask. `RTS_BUILD_STAGE5_PROMOTED_MULTIPLAYER_AUTHORITY=ON`
is intentionally rejected by CMake with a lockstep-v2 prerequisite error; no
v1 evidence can be promoted into live multiplayer authority. A future
lockstep-v2 contract must define its own schema, trust root, and complete gate
before any product build can advertise worker kernels. Invalid or absent
evidence, or a forged sibling bundle beside a default build, keeps the mask at
zero.

Run the installed peer matrix from a fresh task-owned evidence directory. This
uses a dedicated local-only process mode and the exact already-installed
executables; it does not enter the shell, lobby, normal transport, or gameplay.
Each peer must complete the fixed-width NET3 Hello/Ack challenge with its own
session token before it can publish a ready record:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Core/Tools/DeterministicSimulationValidation/Invoke-InstalledNet3LoopbackValidation.ps1 `
  -GeneralsExecutable <installed-generals.exe> `
  -ZeroHourExecutable <installed-generalszh.exe> `
  -ArtifactSetManifestPath <Stage5ArtifactSet.json> `
  -SourceCommit <exact-lowercase-40-hex-commit> `
  -OutputDirectory <fresh-task-owned-evidence-directory>
```

The runner starts 40 exact product peer processes across the canonical 16
matches, independently binds every PID to its executable and complete artifact
set, and preserves every raw peer record. It creates
`ProofBundles/Generals` and `ProofBundles/ZeroHour` only after the strict parser
accepts the whole matrix. Place the matching bundle contents, including the
`Net3Raw` tree, beside the unchanged title executable. The runtime re-hashes
the executable, manifests, raw index, and all indexed peer records; any missing
or altered item keeps the negotiated product mask at zero.

## Performance gate

Use the exact installed native x64 Release candidate without a debugger or
Tracy. Performance enforcement is opt-in with `-EnforcePerformance` and
requires both `-Stage3PerformanceBaselinePath` and an independently supplied
`-ExpectedStage3ExecutableSha256`. The baseline JSON must identify
`schemaVersion: 1`, `stage: "Stage3"`, `architecture: "x64"`, the exact Stage 3
executable SHA-256, the identical stress-fixture SHA-256,
`configuration: "parallel-1"`, `physicalCoreCount`, `availableCpus`, at least
one `warmupRuns`, and raw positive
`wallMilliseconds` containing at least three measured values after warm-up.
Every floating-point timing and baseline sample must be finite; `NaN` and
positive or negative infinity fail closed.
The baseline JSON is copied into the evidence directory; its own SHA-256, exact
executable hash, topology, warm-up count, raw samples, and measured median are
recorded in the plan and performance report. The report also preserves raw
candidate wall-time samples (including separately identified warm-up and
measured samples) for each thresholded configuration. A hash asserted only by
the baseline file cannot establish provenance.

The gate uses the stress replay's outer process wall time, excludes one warm-up
per configuration, and compares medians of at least three measured runs. On a
host exposing at least eight physical cores and eight selected worker CPUs,
parallel-8 must deliver at least 2.0x median throughput (inverse wall time)
versus the same candidate forced to one worker. The candidate's forced
one-worker median wall time may regress by at most 5% against the exact-hash
Stage 3 baseline recorded on matching physical/logical topology. On a host with
at least 16 physical cores and 16 selected
worker CPUs, parallel-16 must continue positive median scaling from parallel-8.
Absent baseline, x64 identity, raw samples, physical-core topology, or effective
worker topology fails or reports the performance gate unsupported; it never
claims final Stage 5 acceptance. Omit `-EnforcePerformance` to run the
functional determinism matrix on a smaller host.

Final acceptance additionally requires the schema-bound
`PerformanceScalingEvidence.schema.json` attachment. It is not satisfied by a
logical worker count. The attachment binds the exact source, artifact set, and
installed Zero Hour executable, plus the independently hashed Stage 3 baseline
attachment used for every regression ratio; hashes the `GetSystemCpuSetInformation`
topology; and records canonical forced-one, physical-8, and physical-16 lanes.
Each lane supplies an exact physical-core mask whose population must equal its
distinct physical-core count (1, 8, or 16 respectively).

The summary also hashes a separate
`PerformanceScalingRawSamples.schema.json` manifest produced by the installed
runtime. Every fixture repeat records its process ID, executable SHA-256, and
exact supported installed-runtime command (`-headless -noFPSLimit`, serial
Stage 4 pipeline, parallel Stage 5 mode, auto worker policy, explicit worker
count, executable-hash binding, and the scaling replay). Phase and kernel rows
must correlate with the same canonical one-worker and physical-8 receipts. The raw manifest
in turn hashes a `PerformanceScalingTopologyReceipt.schema.json` CPU-set
receipt; validation derives its physical cores and selected-lane masks from
the logical-to-physical mapping rather than trusting reported counts. The
topology receipt is also correlated to the exact first current one-worker run.

Seven one-worker phase timings and their measured serial portions are summed by
the validator. The reported Amdahl serial fraction and maximum speedup are
recomputed and must show that the measured workload can theoretically reach
2x. Physics, status, collision, AI planning, immutable-spatial, and path each
record admitted slices plus capture, schedule, wait, validate, and commit time.
Those components must exactly sum to the reported parallel total, which must
show positive net speedup over the matching exact serial operation. All phase,
kernel, fixture, regression, and scaling aggregates are recomputed from the
raw per-repeat rows, so a self-authored summary cannot satisfy the gate.

Four canonically ordered, eight-player fixtures are mandatory: 1,000, 4,000,
and 8,000 units plus a dense eight-player case with at least 8,000 peak units.
Every fixture needs at least three measured repetitions, at least 2.0x actual
headless throughput on eight distinct physical cores, positive physical-8 to
physical-16 scaling, and no more than 5% forced-one regression from its Stage 3
baseline. Missing/tampered timings, a logical-only lane, or a self-asserted
Amdahl fraction fails final acceptance.

This is explicitly an aggregate Stage 5 stress-replay throughput measurement.
Replay samples are not conditioned on positive collision work, so neither the
2.0x threshold nor 8-to-16 scaling is a collision-specific speedup claim. The
performance report schema records
`measurementScope: aggregate-stage5-stress-replay-throughput`, sets
`collisionSpecificSpeedupClaim: false`, and lists collision phase evidence
separately. Collision usefulness is established instead by the qualifying live
4v2 authoritative and shadow executions with positive collision-specific jobs.

Record the machine/CPU context, executable hash, effective workers, logic
frames, wall time, working-set peak, exact or bounded p50/p95/p99 phase times,
queue latency, high-water mark, peak active workers, waits, owner help, steals,
failures, cancellation, and fallback.

`FrameTimingDiagnostics` exposes collision admission traversal, snapshot,
parallel preparation, wait, reduce, live topology/generation validation,
shadow existing-contact filtering, legacy/authoritative commit, shadow reversal
preparation, and structured compare phases. `simulation_parallel` is an
inclusive synchronous call duration and therefore includes its separately
reported wait and owner-reduce subphases. Every other collision phase is an
exclusive owner scope; `collision_existing_filter`,
`collision_commit_prepare`, and `simulation_shadow_compare` occur only in the
shadow oracle, while authoritative existing-contact checks remain honestly
inside `simulation_commit`. Correctness is a hard gate at every count.
Freeze performance thresholds from a paired serial baseline before reviewing
the candidate result; do not tune them after observing the matrix.

## CI and final candidate

Ordinary pull requests retain the bounded VC6 compatibility workflow. A manual
`GenCI` dispatch may provide the Zero Hour-specific `stage5_fixture_manifest`,
`stage5_performance_baseline`, and executable hash, or the corresponding
`stage5_generals_*` inputs for Generals. Each opt-in job downloads its
title-specific native x64 installed artifact and runs the full replay matrix;
title-specific evidence is uploaded even when the matrix fails.

After focused tests, faults, shadow, replay, AI, mixed-peer, combined-policy,
and performance gates pass, manually test the exact installed candidate in both intended
parallel/automatic mode and serial fallback. Cover startup, shell, map loading,
orders, camera, pause/resume, dense combat, long 2-vs-6, save/load, replay
record/playback, audio, movies, screenshots, resize, minimize/restore, Alt-Tab,
device reset, return to shell, a second match, quit during load/combat, shutdown
with work active, and relaunch. Manual acceptance is not automated and cannot
be replaced by headless evidence.

## Final acceptance evidence aggregation

`Run-DeterministicSimulationValidation.ps1` is the deterministic-runtime gate.
Its isolated replay/AI/performance result is necessary but deliberately
insufficient for final acceptance. The final gate is
`Invoke-Stage5FinalAcceptance.ps1`, which consumes a reviewed request matching
`FinalAcceptanceManifest.schema.json` and fails closed unless all of these
independent evidence manifests exist:

1. deterministic-runtime matrix;
2. replay determinism and its fixture manifest;
3. fresh 4v3/4v2 AI games;
4. performance scaling plus the independently identified Stage 3 baseline;
5. mixed-worker multiplayer soak;
6. the combined Stage 4 plus Stage 5 installed-runtime lane;
7. complete-diff premium review with zero open P0, P1, or P2 findings; and
8. the user's final installed-runtime manual acceptance.

The request, artifact-set manifest, evidence envelopes, and attachments use
manifest-relative paths. The aggregator independently computes every SHA-256,
rejects missing or duplicate evidence, and requires every evidence envelope to
name the same lowercase source commit, x64 artifact-set SHA-256, and passed
status. The artifact set itself must contain the independently hashed
executables, launchers, and launcher configurations for both titles. A hash
written only inside the file it purports to identify is never sufficient.

The deterministic-runtime envelope must explicitly set
`finalAcceptanceClaim: false` and bind the independent replay, AI, and
performance evidence file hashes. The combined installed-runtime envelope must
report `pipelineMode=parallel`, `simulationMode=parallel`, `workerPolicy=auto`,
`renderer=d3d11`, and `renderThread=dedicated`, with both titles and the combined
Stage 4/5 execution passing. This lane complements rather than replaces the
serial-pipeline isolation matrix.

Run the aggregator only after the user has actually approved the exact final
candidate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1 `
  -AcceptanceManifestPath <final-acceptance-request.json> `
  -OutputPath <fresh-final-acceptance-report.json>
```

The output path must be new. A missing manual manifest, stale commit, changed
binary, tampered attachment, serial combined-policy lane, unsupported topology,
open premium finding, or any non-passing evidence prevents report creation.
