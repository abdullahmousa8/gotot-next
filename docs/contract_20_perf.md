# contract_20_perf — GNE-020 performance regression gate

Status: **ACTIVE** (2026-09-30). Enforced by `perf020` in `tools/gne_verify.ps1`.

## 1. What is measured

`tools/gt_020a.bat` runs `demo/gpu_smoke/main_020.tscn` twice (d1, d2) in a
**cold process** each time, and each pass prints:

```
wall_avg_us=<mean wall us over the run>  wall_peak_us=<worst frame us>
```

`wall_avg_us` averages the **whole cold run**, first-frame shader compilation
and pipeline creation included. It is not a steady-state frame time.

## 2. The threshold, and why those two numbers

| Metric | FAIL | WARN | Gated? |
|---|---|---|---|
| `wall_avg_us` | `> 40000` | `> 46000` | **yes** |
| `wall_peak_us` | — | — | **no — recorded only** |

The gate evaluates the **worse of the two passes**, not the last one, so a
regression confined to d1 is still caught. `WARN` is reported in the detail
column and does **not** fail the build.

**Basis — a controlled quiet-host run, 10 samples (5 runs x 2 passes), 2026-09-30:**

| min | median | mean | max | spread |
|---|---|---|---|---|
| 26703 | 28846 | 28873 | 30679 | **1.15x** |

`40000` sits **1.30x above the measured maximum**. That margin clears the
1.70x spread that `wall_peak_us` shows on its own, while staying far below the
`110977 us` a contaminated host recorded.

## 3. What is deliberately NOT the basis

The 31 historical samples in `tools/verify_history.tsv` are **rejected** as a
threshold source:

- they spread **6.9x** (avg min 16125, max 110977; peak max 533645),
- they are left-skewed, so a percentile of that population is meaningless,
- three samples 35 minutes apart on different commits read
  `24747 / 110977 / 40617` — that is **host contamination, not a code change**.

Each historical row is itself an **average of two passes** (d1 and d2), which
is why the file alone never showed the real per-sample spread.

## 4. Known limits

These are facts to keep, not defects:

- **The threshold is relative to this machine and this scene.** Slower
  hardware will read a false alarm. Re-baseline — do **not** widen.
- **A genuine optimisation is indistinguishable from noise** at n=10. A drop to
  `20000` would pass silently and tell us nothing.
- **`wall_peak_us` is ungated** because a single-frame maximum at 1.70x spread
  would fire on jitter alone.
- A per-frame **`p50` gate would be the stronger regression guard**, but it
  needs `p50` stability proven across machines first. Not done. `p50_us` is
  already emitted by the scene for free when that work is taken up.

## 5. Verifying the gate rejects

The threshold is one-sided (cost rising), so it is verified by **injection**:
run `gne_verify.ps1` with `gt_020a.log` carrying a synthetic `wall_avg_us`
above `40000` and confirm `perf020: FAIL` and `GNE_VERIFY: FAIL`. The pass path
is confirmed by the live 5-run measurement above, all `GT_020A: PASS`.
