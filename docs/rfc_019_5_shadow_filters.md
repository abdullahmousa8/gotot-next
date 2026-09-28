# RFC 019.5 - Shadow Filtering (VSM-class soft shadows; slice-0 = ESM prototype) - [DRAFT v0.1, 2026-09-29]

**Why now:** 019 ships deterministic PCF-4 (`gne_pcf_arr`: 4 taps + `gne_shadow_bias`)
- hard-edged at a fixed kernel. The register defers "VSM / RT / volumetric shadows"
to a future shadow milestone; RT is blocked (KI-015). This RFC opens that milestone
with a bounded first slice.

## Technique options (survey)
1. **VSM** (variance moments, depth + depth^2): Chebyshev upper bound; needs a
   separable BLUR pass for softness; light-bleeding artifacts (mitigable via EVSM
   warping). Storage: R32G32F per map (new storage class for all three map types).
2. **ESM** (exponential): store exp(k*z); HARDWARE-linear filtering yields soft
   shadows with ZERO extra passes; single channel -> reuses the EXISTING R32 map
   storage; artifacts: light bleeding / over-darkening (controlled by k + clamp).
3. **EVSM** (exponential variance): exp(k*z) + exp(-k*z) moments; best quality;
   two channels + blur. The end-state candidate if slice-0 shows ESM insufficient.

## Slice-0 (bounded prototype; the first deliverable)
**ESM on directional CSM only** - smallest delta, highest information:
- Reuse the R32 CSM storage; fill = exp-encoded depth variant of the depth pass;
  sample = the existing filter path with a LINEAR filter state + the standard bias
  transform; flag `gne_shadow_esm` default OFF (all current literals preserved;
  CVS baseline untouched).
- New scene `main_019_5` (based on the 019 directional-shadow scene); OFF vs ON
  in one process.
- Pre-registered acceptance (frozen before the ON/OFF measurement run):
  E1 softness: penumbra transition width >= 2x PCF-4 at a documented edge sample
  series; E2 no-acne: lit-region flatness within tolerance; E3 cost: map-fill and
  light-pass deltas measured and reported (evidence); E4 determinism: in-process
  byte-equal; E5 CVS green + zero ERROR/leak.
- Decision after evidence: keep ESM / advance to VSM/EVSM / stop (negative result
  is a result).

## Invariants
019 API surface unchanged (map creation/culling/teardown untouched); flag-gated;
storage upgrades (R32 -> R32G32) only after slice-0 evidence with a recorded
decision. Not in scope: cube/spot VSM in slice-0; volumetric; RT (KI-015); any
temporal trick.