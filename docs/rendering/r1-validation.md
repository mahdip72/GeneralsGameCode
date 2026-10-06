# R1 rendering validation: V16-02

**Code base:** `e9bc8ac999cdea97e043118a107c9a4846897408`
**Validation state:** source and native gates passed; the V16 numeric comparison is data-comparable and clears its frozen descriptive thresholds. The user accepted Stage 1 after fullscreen play on 2026-10-05. This record does not claim a bug-free renderer or completion of the deferred qualifications listed below.

## Implementation and source evidence

V16-02 keeps rendering work on the device owner thread and submits compact, value-owned indexed-draw commands. Renderer/cache state is committed only after queue acceptance. Normal-matrix cache keys preserve raw floating-point bits, with explicit FPU-control handling.

The D3D11.1 constant-buffer path checks capability, byte offset, range, and alignment before API use. Its optional 64-KiB arena uses a 3,072-byte slot stride (192 16-byte registers, 21 slots); devices without the required capability retain the fallback path. Static geometry reuse carries residency and generation proof. Both Generals title variants keep matching shadow passes and permit second-pass upload reuse only while the first-pass proof remains valid.

The bounded source review recorded 10 initial, 3 follow-up, and 1 final independent source gates; all passed. This review coverage is bounded evidence, not a guarantee that every defect is absent. No renderer-creation hitch was confirmed as a prewarming target, so no speculative prewarming was added.

Build04 produced the candidate executable with SHA-256 `701B94298A3348B1D9E9C625466CEAB4E89254A9D52243A46D116255C96AA0C2`. Native04 reports 21/21 selected tests passed across 18 native executables. Its five actual GPU arena records were all admitted, covering one-draw-per-DISCARD and 42-draw/two-page images.

The portable analyzer suite passed 118 tests. The receipt records analyzer SHA-256 `A2A6AC0EC70843F5C8C3A8F37FCC7E316C7263E2C4C03B72BAC7FFC491202224`, unchanged before and after that run.

## Frozen six-run numeric comparison

The policy was frozen before any V16 game run. It required all six runs in fixed order **Baseline, Candidate, Candidate, Baseline, Baseline, Candidate**; at least a 10% median run-FPS gain; every candidate run above the highest baseline; candidate median per-run P95, P99, and maximum gaps no worse than baseline; and zero candidate gaps over 500 ms. Pilots were excluded, the original extreme baseline observation was retained, and post-hoc exclusions or replacements were disallowed.

| Order | Run | Role | Present FPS |
|---:|---|---|---:|
| 1 | numeric-01 | Baseline | 15.2561 |
| 2 | numeric-02 | Candidate | 16.7559 |
| 3 | numeric-03 | Candidate | 16.5485 |
| 4 | numeric-04 | Baseline | 15.0028 |
| 5 | numeric-05 | Baseline | 13.2056 |
| 6 | numeric-06 | Candidate | 16.9855 |

Each run used 600 logic frames: 150 warm-up and 450 measured, under the normal 30-Hz simulation configuration. This is the configured rate, not evidence of a sustained 30 logic updates per wall-clock second. The fixture began with 512 units; observed combat population changed from 507 to 364. The profile requested maximum graphics at 1920×1080, 8× MSAA, and 16× anisotropic filtering. Sampled backend frames verified 1920×1080, effective 8× MSAA with resolve, and the recorded hardware adapter; 16× anisotropic filtering was requested but not measured.

The median run FPS increased from 15.0028 to 16.7559, a descriptive 11.69%; the lowest candidate run (16.5485) exceeded the highest baseline run (15.2561), and all 9 cross-group pairs favored the candidate. Median per-run gap summaries were:

| Gap statistic | Baseline | Candidate |
|---|---:|---:|
| P95 | 88.3898 ms | 80.2737 ms |
| P99 | 112.2110 ms | 96.8359 ms |
| Maximum | 159.2089 ms | 151.9431 ms |

No candidate run had a gap over 500 ms. Baseline numeric-05 retained one 1,607.9989-ms gap; it was not removed. With only three runs per group, these are descriptive summaries, not a significance claim. V15 and failed V15 cohorts were not pooled into this comparison.

The first comparison invocation omitted required multipage proof inputs and remains rejected. Corrected comparison02 uses the six original prelaunch environment proofs. In the result, `data_comparable: true` means the evidence can be compared descriptively; it does not mean stage approval. The legacy `accepted` field is only an alias for data comparability. The original comparison records `stage_accepted: false` and `stage_status: not_evaluated`; the frozen policy also sets manual acceptance to false. These historical records remain unchanged. The later user acceptance is recorded separately below.

## Fullscreen acceptance and compiler closeout

The user played the exact benchmarked normal Release executable at 3840x2160 fullscreen with normal intro videos and a copy of the established settings, maps, and saves. The executable retained SHA-256 `701B94298A3348B1D9E9C625466CEAB4E89254A9D52243A46D116255C96AA0C2`. The user reported smoother play and accepted Stage 1 on 2026-10-05, while noting that performance was still short of ideal. This is user gameplay acceptance, not an additional controlled FPS measurement.

Three compiler compatibility repairs follow the benchmarked implementation. The generation counter remains accessed only through Interlocked operations, with a declaration accepted by the older Windows SDK. The second texture-slot loop uses a distinct variable name for VC6 scoping. The line-group sorting test includes its frame-timing declarations. These changes preserve runtime behavior by source inspection. A rebuilt executable has a different hash and is not substituted for the executable the user accepted.

The compiler-closeout x64 Release build passed. All 22 selected native tests passed, including the sorting test and five admitted GPU constant-arena images. The original implementation review and benchmark evidence remain applicable, with the three compiler repairs reviewed separately.

## Deferred qualification

Automated visual inspection was waived. Retail replay, full Generals title/runtime qualification, and external qualification remain deferred. Native builds and local six-core testing do not establish retail replay compatibility or external 16-core qualification. The hosted installed replay matrices and external-core checks remain opt-in while their private data service or required runner is unavailable. Their skipped status is not a pass. The final playable package uses the exact user-accepted executable.

## Reproduction

From the repository root, substitute the local run directories and each run’s original environment-proof file. Supply one proof pair for every run in the comparison; the run value must match the corresponding baseline or candidate argument.

```powershell
python -B tools/rendering-benchmark/analyze_r1.py analyze <RUN> --multipage-proof <ORIGINAL_ENVIRONMENT_PROOF>

python -B tools/rendering-benchmark/analyze_r1.py compare --baseline <B1> <B4> <B5> --candidate <C2> <C3> <C6> --multipage-proof <B1> <B1_PROOF> --multipage-proof <C2> <C2_PROOF> --multipage-proof <C3> <C3_PROOF> --multipage-proof <B4> <B4_PROOF> --multipage-proof <B5> <B5_PROOF> --multipage-proof <C6> <C6_PROOF>
```

## Pinned evidence

The following evidence identifiers and hashes are recorded as basenames under the task integration evidence bundle:

- Source base: commit `e9bc8ac999cdea97e043118a107c9a4846897408`; frozen source snapshot SHA-256 `E73FD8787F480C60118DB65F5E43BCC8A0CCDCD5EDC7C48CCDBCDD93BBEADEBD`; 50-file C++ pin SHA-256 `F37D5125ED82F331439F9799148E2B62284B27B1EC6F56928BF0578AC10DE596`.
- `r1-v16-numeric01-comparison02.json` — SHA-256 `518C0C790AD720635CDCADEC62A54DA6740B6E8244626C9C88D20C3DE8890D65`.
- `r1-v16-prospective-performance-policy01.json` — SHA-256 `471185AA9A579BC942824553055124CDCE2BF4FF40B46C6A0BC5D6D190CB2CD9`.
- `r1-production-v16-candidate-build04.result.json` — SHA-256 `766EDD8660CC951AD563D5713AF1AEC8DAF0EEEB0D579E030C372CEBF231C207`.
- `r1-production-v16-candidate-native04.result.json` — SHA-256 `9C660F12D0B68F5D41CC743309538D98C3257BC9042B7DE5D322330496288A87`.
- `portable-analyzer-tests-04-1ef084ed14d043478fb50b15557b0d5c.json` — SHA-256 `75D922CFB04A4F2E1A21F70D1646EF0FD0A5B8F7F1310BB969FDF0DB1285130F`.

This page is a post-test documentation update. Build and test receipts retain the source identities recorded when they were produced; this edit does not rerun or extend those gates.
