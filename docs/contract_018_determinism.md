# Contract 018 — Determinism

**GNE-018 — DRAFT.** DET + guards; methods here, numbers frozen at FINAL.

## Determinism rules

- Sorted light ids per cluster (order-independent of GPU scheduling).
- Fixed grid, fixed light set per run ⇒ identical cluster assignment.
- `sig=` content-only, no timings (015.5 lesson); harness compares byte-for-byte.

## 8 criteria evidence (D6-8)

| # | Criterion | Method (thresholds at FINAL) |
|---|---|---|
| L1 | Point light visible | single point ⇒ predicted-region pixels change |
| L2 | Spot light visible | inside-cone vs outside-cone measurable falloff |
| L3 | Multiple lights (>10) | stable frame, counters exact, no overflow |
| L4 | Clustered Forward+ active | cluster buffer non-empty, index counts match assignment |
| L5 | GPU-driven culling | zero-cluster light ⇒ zero shading contribution (measured) |
| L6 | Histogram | multi-light vs 016 single-light differs in predicted direction |
| L7 | DET | 2 runs, identical `sig=` (`d1`), zero `ERROR:` |
| L8 | Regressions | 001A–017 PASS + `main_012 → XFAIL` |

## Regression delta guard

- 016 centers + histograms + DET re-check reproduced with ZERO extra
  lights bound before any 018 claim is accepted (baseline equivalence).
- `main_018.gd` never prints timings inside `sig=`.

## Bugs fixed during bring-up

- (to be filled during implementation — 014-tests precedent.)
