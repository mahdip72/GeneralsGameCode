# Offline visual parity analyzer

`visual_parity_analyzer.py` compares two time-aligned frame directories without
launching either game. It is intended for legacy/D3D11 shell and fixed-camera
captures, but has no Generals-specific assumptions.

The tool uses only the Python standard library. It reads non-interlaced 8-bit
PNG, PPM/PGM, and uncompressed 24/32-bit BMP images. Frames are paired in
natural filename order. Alpha is ignored; comparisons use the visible RGB
channels.

## CLI

From the repository root:

```powershell
python Core/Tools/VisualParityAnalyzer/visual_parity_analyzer.py `
  --legacy-dir captures/legacy `
  --d3d11-dir captures/d3d11 `
  --output captures/visual-parity.json `
  --diagnostic-dir captures/visual-parity-diagnostics
```

Optional timestamp files contain one timestamp per line. CSV/TSV lines are
also accepted; the final field is used. JSON may contain either a timestamp
array or `{ "timestamps": [...] }`.

```powershell
python Core/Tools/VisualParityAnalyzer/visual_parity_analyzer.py `
  --legacy-dir captures/legacy `
  --d3d11-dir captures/d3d11 `
  --legacy-timestamps captures/legacy.times `
  --d3d11-timestamps captures/d3d11.times `
  --output captures/visual-parity.json `
  --fail-on-frame-count-mismatch
```

The JSON report contains:

- per-frame MAE, RMSE, maximum error, 95th-percentile pixel error, and the
  changed-pixel ratio;
- percentile summaries for all image-error measures;
- per-transition reference motion, candidate motion, and positive temporal
  flicker residuals restricted to low-motion regions in the legacy sequence,
  including transition timestamps/intervals when supplied;
- exact consecutive duplicate/held-frame runs for both sequences, with held
  frame ratios and timestamp-derived held-duration estimates;
- timestamp cadence, nominal interval, irregular intervals, estimated dropped
  frames, and an FPS summary (observed frame rate, low-tail/minimum FPS, and
  interval percentiles)
  when timestamps are supplied;
- timestamp-nearest, no-reuse frame pairing when both timestamp files are
  supplied. The report records unmatched frames and absolute timestamp deltas;
- optional PPM error/flicker heatmaps for the highest-error frames and
  transitions.

The capture duration for each sequence is also reported from the first to last
timestamp. Timestamp values must be finite. Cadence is compared with the
median positive interval of the legacy sequence when both timestamp files are
present; otherwise each sequence uses its own median positive interval. A
held-duration estimate includes one nominal interval after the final duplicate
sample because the timestamp stream does not include the eventual release time.

Thresholds are normalized to `[0, 1]` where `1` is a full 8-bit channel delta:

```text
--low-motion-threshold       legacy pixels at or below this delta are eligible
                             for temporal flicker analysis (default 0.02)
--flicker-threshold          candidate excess change counted as flicker
                             (default 0.02)
--pixel-error-threshold      per-frame changed-pixel threshold (default 0.02)
--cadence-tolerance           relative timestamp cadence tolerance (default 0.10)
```

The analyzer returns exit code `0` after a valid comparison, `2` for invalid
input, and `1` only when `--fail-on-frame-count-mismatch` is selected and the
sequence lengths differ. A length mismatch is retained as a warning in the
JSON report when that flag is not used.

## Tests

The deterministic tests create tiny PPM sequences and cover one-frame object
flicker, consecutive duplicate frames, and timestamp gaps:

```powershell
python -m unittest discover `
  -s Core/Tools/VisualParityAnalyzer/tests `
  -p 'test_*.py' `
  -v
```
