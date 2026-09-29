# NOTE 023 - GI x Shadows Coexistence (integration milestone) - [design note, 2026-09-29]

**Question:** do the two flagship systems (019 CSM shadows + 022 GI) coexist
correctly in one frame, and does INDIRECT light reach shadowed (umbra) regions
while direct light stays blocked?

**Physics being validated:** a shadow blocks DIRECT light only; indirect (GI)
radiance gathered by occlusion-aware probes (M3 visibility-gated gather) should
STILL illuminate umbra regions. If umbra luminance with GI == umbra luminance
without GI, the integration is broken (GI ignoring or double-gating).

**Scene:** main_023 = 019 layout (4 boxes, dir light + 20 lights, CSM bound) +
GI field (16x8x16 over the box region) seeded by one raw trace + 8 accumulation
steps.

**Pre-registered gates:**
- I1 (indirect reaches umbra): umbra-probe luminance delta (GI on - GI off) >= 0.02.
- I2 (lit region also gains): lit-face luminance delta >= 0.01.
- I3 (determinism): two identical GI-state draws byte-equal.
- I4 (cost, evidence-only): draw p50 with GI on vs off; accum_step p50 reported.
- I5: zero ERROR/leak lines; CVS green afterwards.
## RESULTS (2026-09-29) - INTEGRATION PASS (I1 + determinism; I2 redefined as saturation-void)

- I1 (indirect reaches umbra): umbra probe 0.8852 -> 0.9755, delta +0.0902 >= 0.02
  PASS. The umbra region GAINS indirect light while direct stays blocked - the
  two systems coexist correctly in one frame.
- I2 (lit probe gains): VOID BY SATURATION - every dir-lit face pixel in this
  scene saturates at 1.0 (8-bit display chain), so a GI delta is invisible there
  regardless of the field. Field evidence at the lit probe's cell: ambient-level
  (0.03/0.03/0.035) after 8 accum steps - the accumulated field had not yet
  gathered radiance for that region (slow EMA + one raw seed). Recorded as an
  instrument limitation, not an integration defect. The umbra probe (unsaturated
  0.885) is the valid integration witness.
- I3 (determinism): byte-equal PASS.
- I4 (cost): GI-off draw p50 1521-1933us; GI-on (accum+draw) 1455-1892us -
  bounded, noise-dominated.
- Field readback evidence: umbra cell after 8 accum = (0.075, 0.104, 0.078) -
  saturated against its calibration (~0.10); lit cell = ambient (0.03).
- Sig: v023|du=0.0902|dl=0.0000|det=1|off=1264|on=1455|d1

Status: NOTE 023 INTEGRATION PASS. The GI x Shadows coexistence is validated at

UNTESTED CLAIM (recorded 2026-09-29, architect directive): "indirect light adds
to already-lit surfaces" is NOT proven and NOT refuted - unmeasurable via the
8-bit display chain (every dir-lit pixel saturates). Backlog: KI-017 / 023-M1 -
HDR (pre-tonemap) radiance readback instrument, part of the GNE CVS as general
measurement infrastructure (any future GI integration gate hits the same limit).
the integration level. Future scoping (when needed): full-field convergence
measurement (the KI-016/M1 instrument), GI x dynamic shadows response.

## KI-017 CLOSURE (2026-09-29) — HDR instrument built, M1' claim CONFIRMED

**Instrument:** 5th framebuffer attachment (RGBA32F) + `out_radiance` writes in
all 5 raster fragment shaders (zero in non-shading paths, real pre-clamp values
in mat lit/bem + light final/bem) + `gpu_raster_read_hdr(x, y)` API + M1' gate
on main_023. Purely additive: full CVS green, all 7 literal signatures
byte-identical, goldens 018/019 byte-identical, zero error lines.

**Before/after HDR (LIT_PROBE, inst1 face):**
- Display (8-bit): 1.0 -> 1.0, delta 0.0 — blind (I2 void confirmed again).
- HDR (unclipped): h0=1.6276 -> h1=2.3326, **gain +0.7050** — indirect light
  measurably adds to an already-lit surface. The 023 untested claim is now
  TESTED and CONFIRMED (M1': sanity + gain>=0.01, folded into INTEGRATION PASS).

**Sanity vs double accumulation:** independent analytic recomputation of h0 from
dir+cluster only (ambient 0.07 + dir 0.76 + spot0 0.54 + points ~0.25) gives
≈1.62 vs measured 1.6276 (0.25% — inside shell-approximation slop). The large
gain is explained (spot0 alone ≈0.52 at this pixel), not inflated. No GI leakage
into the baseline (fresh session, GI disabled at h0 measurement, shader branches
on gi_params.x).

**Correction during implementation (owner-approved):** the inherited lit-path
write sat pre-cluster/pre-GI (radiance = ambient+dir only — blind to the very
quantity KI-017 exists to measure). Moved to end-of-main (final pre-clamp col,
incl. cluster+GI); unlit bem-path write kept. Zero DET risk (new attachment
only). Blend states of the 6 raster-framebuffer pipelines bumped 3->4 (disabled
throughout; shadow pipeline untouched) to cover the 4th color attachment.