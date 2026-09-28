# Contract 018 — Determinism

**GNE-018 — FINAL (D6-7/D6-8 resolved).** DET + guards.

## Determinism rules

- Sorted light ids per cluster (order-independent of GPU scheduling).
- Fixed grid, fixed light set per run ⇒ identical cluster assignment.
- `sig=` content-only, no timings (015.5 lesson); harness compares byte-for-byte.

## 8 criteria evidence (D6-8 FINAL — initial thresholds, frozen at FINAL after measurement)

| # | Criterion | Method |
|---|---|---|
| L1 | Point light visible | ON vs OFF ⇒ changed pixels > 1000 AND probe brightens > 0.1 |
| L2 | Spot light visible | inside-cone vs outside-cone mean ratio > 2 |
| L3 | Multiple lights (>10) | 16 lights stable, counters exact (16/16), no overflow, exit 0 |
| L4 | Clustered Forward+ active | non-empty clusters > 0 AND assignments match CPU-predicted count |
| L5 | GPU-driven culling | zero-cluster light ⇒ pixels identical with/without it |
| L6 | Histogram | differs from 016-baseline reproduction; hr ≥ 0.3 |
| L7 | DET | 2 runs, identical `sig=` (`v18\|lc\|cc\|ot\|hr\|d`, `d1`), zero `ERROR:` |
| L8 | Regressions | 001A–017 PASS + `main_012 → XFAIL` |

## Regression delta guard

- 016 centers + histograms + DET re-check reproduced with ZERO extra
  lights bound before any 018 claim is accepted (baseline equivalence).
- `main_018.gd` never prints timings inside `sig=`.

## Bugs fixed during bring-up

- (to be filled during implementation — 014-tests precedent.)
