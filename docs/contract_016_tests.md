# Contract 016 — Tests

**GNE-016 — DRAFT.** 8 criteria evidence; methods here, numbers frozen at FINAL.

## 8 criteria evidence

| # | Criterion | Method (numbers at FINAL) |
|---|---|---|
| C1 | Real material store | 6 distinct writes + byte-identical readback |
| C2 | Determinism | 2 runs, identical `sig=` (`d1`), zero `ERROR:` |
| C3 | Consistent face normals | per-face color variance ≈ 0 |
| C4 | N·L response | rotate `L` by a known angle ⇒ pixels change in the predicted direction (dark↔lit flip, not just any change) |
| C5 | Rough/metal measurable | same albedo, different roughness ⇒ `hr >= 0.3`, else FAIL (D4-3) |
| C6 | Light-independent emissive | `N·L=0` pixel carries emissive color when flag on, gone when off; backfaces NONE by default (D4-4) |
| C7 | Signature preservation | all legacy sigs literal + `GT_REGRESS: PASS` + `main_012 → XFAIL` |
| C8 | Hard caps + clean | slot/id violations ⇒ `print_error` + reject; zero `RID allocations of type` in all runs |

## Signature

- Format (D4-2): `v16|mc|Lx|Ly|Lz|amb|hp|hr|d` — content-only, no timings (015.5 lesson). Example: `v16|mc=8|L-0.41|-0.82|-0.41|amb=0.10|hp=245|hr=0.85|d1`.
- Frozen at FINAL; the harness compares `sig=` lines byte-for-byte (`gt_015a` pattern).

## Regression delta guard

- 010/011 centers + histograms + DET re-check reproduced on the post-016 binary **before** any 016 claim is accepted.
- `main_016.gd` never prints timings inside `sig=` (timings, if any, on a separate line).

## Bugs fixed during bring-up (regression for future material code)

- (to be filled during implementation — 014-tests precedent: every real bug found gets one line here so the next milestone doesn't rediscover it.)
