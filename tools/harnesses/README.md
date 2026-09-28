# tools/harnesses — reproduction runners

**Status:** ADDITIVE / new files only. Nothing in this folder modifies an
existing file, and nothing here commits or pushes anything (Architect
constraint #1: no commit/push without explicit approval).

## Why this folder exists

An independent review of the repository found a real reproducibility gap:

* `docs/progress_report.md` quotes per-milestone evidence runners —
  `gt_smoke`, `gt_007`, `gt_008`, `gt_008b`, `gt_009`, `gt_010a`,
  `gt_011a|b|c`, `gt_013a`, `gt_014a`.
* The tree only ever contained **two** tool scripts: `tools/gt_015a.bat` and
  `tools/gt_regress.bat` (`git status`: both untracked).

So every historical PASS claim — and, more importantly, every `sig=` DET
signature — was **not reproducible from the repository**. The runners below
re-create that ability without touching the existing scripts.

## Runners

### `gt_harness.bat` — per-scene DET sweep (2 runs per scene)

```
gt_harness.bat                              all scenes (007..015 + 012 as XFAIL)
gt_harness.bat main_014                     one scene
gt_harness.bat main_012 XFAIL               one scene, known WIP (reported, not gated)
gt_harness.bat main_014 - C:\tmp\gv_014 600 one scene, explicit tmpdir + per-run limit
```

Positional overrides: `<scene> [XFAIL|-] [tmpdir] [maxsec]` (prefer these over the
`GT_HARNESS_TMP` / `GT_HARNESS_MAXSEC` env vars — an inline
`set VAR=value && ...` silently appends a **trailing space** to the value, which
produced unreadable log paths during development).

### `run_scene.ps1` — the single-run runner used by the sweep

Runs one scene and returns the engine's **real** exit code, or `91` when the run
was killed after `-MaxSec`, or `92` when the exit code could not be read.

Two things it exists to fix, both found by testing during development:

1. A naive `Start-Process ... -PassThru` + timed `WaitForExit()` **does not
   populate `Process.ExitCode` when output is redirected**. The sweep therefore
   reported `rc=0` for `main_012`, which actually exits `123`. Using
   `System.Diagnostics.Process` directly (with an async output reader so a
   chatty scene cannot deadlock the pipe) fixed it. `/harness` verification:
   `main_012 → rc=123` (known-FAIL control) and `main_009 → rc=0`.
2. Scenes need a hard wall-clock bound — see the `main_011` note below.


For **both** runs it requires: `rc == 0`, a `PASS` marker, no `ERROR:` line,
no `RID allocations of type` line, and the first `sig=` line identical across
the two runs (DET). `main_007` and `main_008` print no `sig=` line, so they
are gated on rc/marker/ERROR only and reported as `det=N/A`.

Exit code: `0` PASS · `1` FAIL · `90` engine exe missing.

Difference from `tools/gt_regress.bat` (kept as-is, not replaced): this runner
additionally (a) runs every scene **twice and compares the signature**, which
is the DET claim the report makes, and (b) audits `ERROR:`/RID-cleanup lines on
**both** runs instead of only the first.

### `gt_det.bat` — N-run determinism runner

```
gt_det.bat main_014 23
```

Runs one scene N times and requires every run to be green **and** to print the
same `sig=` line as run 1. This exists because a single passing run cannot
validate a fix for a *flaky* defect — the GNE-014 add/remove race was
observed to fail roughly a quarter of the time (`0, 0, -4, 0` lost instances),
so repetition — not one lucky run — is the evidence.

## Where the evidence lands

* `%TEMP%\gt_harness\gt_harness_report.txt` — condensed table.
* `%TEMP%\gt_harness\<scene>_{a,b}.log` — full stdout/stderr of both runs.
* `%TEMP%\gt_harness\<scene>_{a,b}.sig` — extracted `sig=` lines used for DET.
* `%TEMP%\gt_det\<scene>_r<N>.log` — full logs of the N-run sweep.

Nothing is written inside the repository by these runners.

## Notes / limitations

* One variant per scene. Scenes with argument variants (e.g. `main_009`
  `-- --front-only` = 009a vs 009b) are exercised in their default form only;
  pass the scene name and run the variant manually if needed.
* **`main_011` is slow, not broken.** Measured 2026-09-27: it runs
  `FRAME_LIMIT = 60` full GPU frames, and at `frame == FRAME_LIMIT` it calls
  `_finalize()` (`demo/gpu_smoke/main_011.gd:255`), which scans the 1920x1080
  pixel and depth readbacks in GDScript (per-group centre evidence, front/back
  depth probes, colour histogram, DET re-check). One run was still inside
  `_finalize()` after ~7 minutes at a full core, with the log stopping at
  `frame=60`. Budget `-MaxSec` accordingly (a 2-run DET of 011 is a
  multi-minute operation); it is NOT a hang — CPU stays pegged and the process
  keeps its dedicated thread busy.
* **Known limitation — `main_011` DET cannot be compared as-is.** Its `sig` line
  ends with the two timing fields `dispatch_us/draw_us` (e.g. the report's
  `§19` baseline ends `... cb=1 dt=1 275/89`), and timings differ run to run.
  A correct build would therefore still be reported as `det=DIFF` by this
  sweep — a false negative, not a real regression. Before 011 can gate on DET
  either (a) the sweep must compare only the part before the trailing
  `NNN/NNN` pair, or (b) `main_011.gd` should print the timings on a separate
  line so `sig=` stays content-only (the pattern used by 013/014/015).
  Scenes whose sig is content-only (008b/009/010/012/013/014/015) are unaffected.
* `main_012` is **DEFERRED / XFAIL** by Architect decision — it exits `123`
  with `FAIL code=123 occlusion never activated (p2 < p1 never seen)`. The
  sweep reports it and never counts it as a gate failure.
* The runners assume the engine binary built from `godot-master` with
  `custom_modules="<repo>/modules"` (see `docs/sac_unblock_procedure.md` for
  the Smart App Control caveat).

## Result of the first full verification run (2026-09-27, current binary)

The current binary was built *after* the last source edit
(`gne_render_server.cpp` 13:55:19 → `godot...console.exe` 13:55:49) and the
015 work plus the 014 race fix are in that working tree. Every signature below
was reprinted **identically on both runs** of the scene:

| scene | rc | DET | signature (first line) |
|---|---|---|---|
| main_007 | 0/0 | n/a (no `sig=`) | — |
| main_008 | 0/0 | n/a (no `sig=`) | — |
| main_008b | 0/0 | OK | `v3905\|36\|3905\|g25765\|c0\|5004\|9997\|h176\|m0\|3905\|f150` |
| main_009 | 0/0 | OK | `v4\|36\|4\|B\|g2814\|f2814\|b2070786\|dF0.87835\|…\|c0\|2` |
| main_010 | 0/0 | OK | `v6\|mc3\|bc3\|dc2/2/2\|g368\|b290\|o2530\|dC0.87835\|dT0.90449\|dO0.91052\|cc1\|c0\|5` |
| main_013 | 0/0 | OK | `v13\|t1048576\|m18613\|l0m10905\|l0,1,1,2,2,0\|…\|cov76685\|sp1092857\|w76685\|f3106528256\|d1` |
| main_014 | 0/0 | OK | `v14\|c18616\|a0\|u5000/5000/10000\|sn4096\|dm8\|g5\|s121635140\|dd365221617\|d1` |
| main_015 | 0/0 | OK | `v15-pc9-p6-e6-b6-po8753152-res9048064-sv32768-x6-6-q1-2-t18616` |
| main_012 | 123/123 | n/a | XFAIL — `FAIL code=123 occlusion never activated` |

008b / 009 / 010 reproduce the signatures quoted in `docs/progress_report.md`
(§15/§17/§18) verbatim on today's binary, which is the point of this folder:
those claims are reproducible again.

