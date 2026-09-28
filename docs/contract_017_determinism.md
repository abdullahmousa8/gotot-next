# Contract 017 — Determinism

**GNE-017 — DRAFT.** DET + guards; methods here, numbers frozen at FINAL.

## Determinism rules

- Baked bytes deterministic (same PNG + preset ⇒ same hash).
- Sampled pixels deterministic (same texture + UV ⇒ same texel; trilinear, no anisotropic).
- `sig=` content-only, no timings (015.5 lesson); harness compares byte-for-byte (`gt_015a` pattern).

## 8 criteria evidence (D5-7)

| # | Criterion | Method (numbers at FINAL) |
|---|---|---|
| T1 | KTX2 loads | magic + mip sizes match header |
| T2 | Basis correct | first baked texel == uploaded texel, byte-identical |
| T3 | Binds | slot readback equals written binding |
| T4 | Sampled | pixels carry texture fingerprint, not flat albedo |
| T5 | Mipmaps | different render resolution ⇒ identical signature (MIP stability) |
| T6 | Histogram | hr(texture) exceeds flat-albedo hr past the frozen threshold |
| T7 | DET | 2 runs, identical `sig=` (`d1`), zero `ERROR:` |
| T8 | Regressions | 001A–016 PASS + `main_012 → XFAIL` |

## Regression delta guard

- 016 centers + histograms + DET re-check reproduced on the post-017 binary with textures **unbound** before any 017 claim is accepted.
- `main_017.gd` never prints timings inside `sig=`.

## Bugs fixed during bring-up (regression for future texture code)

- (to be filled during implementation — 014-tests precedent.)
