# Contract 022 — b-field GI Convergence

## Scope
- GI convergence metric for the accumulated probe field.
- Replaces legacy displayed-image criterion (b) as the gate (owner, 2026-10-01);
  the 8-bit clause stands as a historical instrument record only.

## Definition (CORRECTED against the draft: b-field is NOT a ratio of ratios)
- `b-field` = criterion-(b) geometry applied to the FP32 FIELD delta series:
  every 10-frame interval ratio `d_n / d_{n-1} <= 0.6` (0.6 inherited verbatim
  from the frozen section-11 criterion b).
- `ratio_f` = interval-decay rate of the float field (`gpu_gi_read_avg` red).
- `ratio_L` = interval-decay rate of the PRE-TONEMAP HDR radiance
  (`gpu_raster_read_hdr` red) - explicitly NOT 8-bit.
- Convergence Delta `|rL - rF|` is the drift-catcher (FVD: mean 0.0007).

## Constants
- ISCALE = 0.015: anti-saturation instrument setting (s=0.03 saturated the
  8-bit chain), documented in spec_022 §11. Gain-neutrality comes from the
  ratio form, not from ISCALE.

## Thresholds (as gated by main_022_m2 - per-interval, never averaged)
- Every 10-frame interval ratio <= 0.6 (14/14 required).
- det = 1 (in-process byte-equal + cross-process literal match) required.
- Reported only: geomean, median, `|delta| < 1e-5` endpoint frame.

## Evidence
- M2: 14/14 <= 0.6, min 0.507853 / max 0.511340 / geomean 0.509385, e5=127,
  det=1; literal `v022m2|frames=160|r10min=0.507853|r10max=0.511340|
  r10gm=0.509385|below=14/14|e5=127|det=1|d1`.
- FVD: displayed HDR converges at the field rate (dmean=0.000723).
- OPEN POINT (reviewer-accepted): the VALUE 0.509 is observed, not derived -
  pure-EMA theory predicts 0.9^10 = 0.349. A synthetic single-light analytic
  reference is future work, not existing evidence.

## Non-goals
- Not a performance metric.
- Not a visual quality metric.
- Not a claim about absolute radiance (field is irradiance-like, pixel is
  post-shading radiance; only rates are compared).
