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
| `wall_avg_us` | `> 31302` | `> 35997` | **yes** |
| `wall_peak_us` | — | — | **no — recorded only** |

The gate evaluates the **worse of the two passes**, not the last one, so a
regression confined to d1 is still caught. `WARN` is reported in the detail
column and does **not** fail the build.

**Basis — re-baselined 2026-09-30 for the KI-001 instrumentation binary (`65D8904D…`):**

| min | max | spread | first-3 avg | last-3 avg |
|---|---|---|---|---|
| 18129 | 23714 | **1.308x** | 18319 | 22958 |

6 samples, each the **worst of its two `main_020` passes** — the exact statistic
the gate computes. `fail_us = 31302` is **1.32x above the measured maximum**
(23714 x 1.32) and `warn_us = 35997` is `fail x 1.15`. All runs exited 0.

**Host drift inside the window, recorded not hidden:** the first three samples
average 18319 and the last three 22958 — the host slowed during the run, the same
contamination the register documents. A quieter window would tighten this basis;
it is deliberately not tightened by hand, because the rule is "1.32x over the
observed maximum". The module change that forced this re-baseline is
timing-neutral for this scene (`main_020` never calls `gpu_frame_*`). Method as
recorded in `tools/perf020_baseline.txt`: two direct engine runs per sample
instead of the `.bat` wrapper, which cannot write its `%TEMP%` signature files in
the measuring shell. The previous basis (`7A65D1EB`, fail 24474) is void because
the exe was relinked.

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

## 5. A hard limit on this metric: the profile is CPU-side only

`wall_avg_us` comes from `gpu_frame_stats()`, whose per-pass numbers are built by
`gpu_frame_mark(p)` (cpp:11965) accumulating the interval since the **previous**
mark. Two consequences, both measured on `main_015_5_phase4` with `--noreadback`
and `--needvisible` probes:

1. **A pass bucket is whatever the script put between two marks.** Not the pass.
   The raster readbacks sit between `mark(P_BATCH)` and `mark(P_RASTER)`, so they
   land in RASTER: 7471.0 -> 270.2 us/frame when skipped. The `output` window
   holds `Image.create_from_data` + `image_tex.update`, an 8.3 MB per-frame
   CPU->GPU upload, gated on the readback having produced pixels: 8645.9 -> 4.7
   us/frame when skipped. `gpu_cull_get_visible_count()` was a synchronous 4-byte
   `buffer_get_data` (cpp:4148) whose result the scene never read: cull
   4277.4 -> 2005.9 us/frame, frame total 22688.9 -> 18105.5 us (commit 58ab58a).

   **Instrumentation was ~83% of that scene's frame. Engine work was under
   0.75 ms of 23 ms.** Any per-pass conclusion drawn from the raw buckets was
   backwards.

2. **CPU time cannot be separated from GPU wait.** `max_timestamp_query_elements`
   is set nowhere in the repo, so `gpu_last_ns` is always 0. `gpu_cull_dispatch`
   and `gpu_visibility_dispatch` contain no readback at all, so the residual
   2005.9 us is not a discarded measurement call - it is most likely driver queue
   back-pressure, i.e. GPU time landing in a CPU bucket. Unconfirmed, and
   unconfirmable until GPU timestamps are enabled.

**Therefore: no per-pass threshold is admissible from this harness, and
`cluster_cull` at 0.03% of frame time must not be gated at all.** This is
recorded in `open_items_register.md` as a known structural limit.

## 6. Verifying the gate rejects

The threshold is one-sided (cost rising), so it is verified by **injection**:
run `gne_verify.ps1` with `gt_020a.log` carrying a synthetic `wall_avg_us`
above `31302` and confirm `perf020: FAIL` and `GNE_VERIFY: FAIL`. The pass path
is confirmed by the live 5-run measurement above, all `GT_020A: PASS`.
