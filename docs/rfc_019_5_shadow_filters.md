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
## Slice-0 implementation notes (recon 2026-09-29)
- Fill path: geometry -> 2D target (frag `gpu_shadow_depth_frag_glsl` writes v_ndc_z)
  -> compute pack (`gpu_shadow_pack_glsl`, imageStore r32f into the array layer).
- ESM variant: ENCODE at fill (out_dist = exp(c*ndc) when the fill mode says ESM;
  per-type fill mode - dir CSM only in this slice; cube/spot keep raw depth so their
  PCF compare is untouched) and DECODE at compare: s = saturate(exp(c*ref) /
  filtered_e) with ref from the same clamp(ndc) - bias transform.
- The CSM sampler must filter LINEAR for the ESM read; the existing PCF path uses
  texelFetch (filter-independent), so a LINEAR filter state does not change PCF
  results.
- c chosen structurally (c=40 documented; artifacts reported, not silently tuned).
- Flag `gne_shadow_esm` default OFF; scene `main_019_5` for the OFF/ON comparison;
  CVS baseline untouched (flag off default).
## Slice-0 RESULTS (2026-09-29) - measured; softness hypothesis REFUTED

Scene `main_019_5` (4 boxes, dir light, CSM-only; flag `gne_shadow_esm` default OFF).
- E1 (penumbra width >= 2x): FAIL - quantitative: transition-band counts (band from
  the measured plateaus: umbra 0.631 / lit 0.958, band 0.68-0.92): PCF 3542 vs ESM
  3522 pixels (ratio 0.994); scanline median widths 3 (PCF) vs 2 (ESM). Bilinear
  filtering of the exp-encoded map - the only filter stage ESM gets without extra
  passes - does NOT widen the penumbra at this scene/screen scale; the hardware
  filter acts over ~1 texel, the same class as the PCF-4 staircase.
- E2 (lit-plateau tolerance): PASS - mean |diff| 8.6e-5 over lit plateaus.
- E3 (cost): measured; render_maps+draw reps p50 ~1.35ms (PCF) vs ~1.78ms (ESM) -
  within run noise (sync-heavy route); evidence-only.
- E4 (determinism): PASS - ON-state redraws byte-equal.
- E5 (CVS): full run PASS 12/12 with the flag default OFF - all historical
  literals byte-exact; zero error lines.
- Calibration captured at real scene depths: the umbra NDC depth gap ~0.086;
  c=40 gives factor ~0.03 (partial), c=87 (near the float32 exp ceiling e^88)
  gives ~0 - ESM in raw NDC space occludes correctly; the softness limit is the
  filter width, not the encoding.
- Evidence artifacts: s195_pcf.png / s195_esm.png (operator temp area; visual
  audit), logs s195_run1..4, sig file s195_sig4.

DECISION (owner, recorded): keep PCF-4 as the shipping shadow filter; the ESM
path remains as a default-OFF prototype flag (zero cost when off; CVS green).
A future soft-shadow requirement goes to the VSM/EVSM direction (moments + real
multi-texel blur), per this RFC's decision path - not to hardware-filter-only ESM.
Slice-0 closes as a clean negative on its own hypothesis; the scene, edge/band
instruments and the evidence pipeline are reusable for the VSM/EVSM slice.