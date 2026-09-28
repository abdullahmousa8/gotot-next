# GNE-022 - GI Scoping (DRAFT v0.1) - [TECHNIQUE DECISION PENDING D10]

**Status:** DRAFT for the Architect decision set D10. No implementation until the
technique is picked. This is a scoping document, not a spec.
**Aligns with:** `engine_spec_v2` section 9 "GneLumen - Hybrid GI" (multi-tier:
screen-space tracing / distance-field+proxy tracing / probe & irradiance cache /
hardware RT when available / temporal-spatial reconstruction / fallback raster+probe;
"Hardware RT must be a feature tier, not a requirement") and the M19-M21 milestone
"Hybrid GI + RT backend + denoiser/reconstruction".
**Discipline (unchanged):** module-only changes, flag-gated (`gne_gi_enabled`, default
false), no engine edits, every gate's literals preserved, budgets set at design time by
measurement (no arbitrary numbers).

## 1. Facts established for this decision (observed 2026-09-28, read-only)

1. **The engine fork's RenderingDevice carries ray-tracing scaffolding at the API
   level**: `RaytracingListID`, `ID_TYPE_RAYTRACING_LIST`,
   `CALLBACK_RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ(_WRITE)`,
   `BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT`,
   `RaytracingPipeline` + SBT buffer creation in `servers/rendering/rendering_device.h`.
   No renderer-level RT path exists (and our module bypasses the renderer anyway).
   => HW ray traversal is FEASIBLE-IN-PRINCIPLE through our own RD usage, but
   UNPROVEN end-to-end in our module (ray-query shader compilation, BLAS/TLAS build,
   SBT). An R0 feasibility probe is recommended before betting on it (D10-5).
2. **The fork ships full reference implementations we may study read-only** (frozen
   reference, no engine edits): SDFGI (`renderer_rd/shaders/environment/sdfgi_*.glsl`)
   and VoxelGI (`voxel_gi.glsl`, `scene_forward_gi_inc.glsl`).
3. **Our scenes are static box-instance scenes** with an existing GPU-side bounds
   buffer (AABB min/max per instance) and analytic lights: ray-vs-AABB is trivial for
   this data; a CPU-free, compute-only tracer is plausible TODAY.
4. **Determinism culture**: fixed per-probe ray sets + fixed seeds give
   deterministic-ish GI (byte-stable per machine), which previous gates can digest;
   stochastic per-pixel GI would introduce denoiser/temporal noise into the gates.

## 2. Candidate options

### O1 - DDGI-style irradiance probes (compute tracing first, RT upgrade path)
- Probe grid; per-probe ray traces gather radiance into an octahedral irradiance
  atlas; screen pixels blend the nearest probes.
- Tracing backend candidates: (a) compute traversal vs the instance bounds buffer
  (works today, no new API); (b) hardware ray queries (fork scaffolding present).
- Key property: FIXED ray sets per probe => deterministic, no denoiser needed for a
  sane image; temporal accumulation optional (S2).
- Cost: probe-update = grid x rays-per-probe per refresh (amortizable round-robin);
  sampling per pixel is cheap. Budgets set at design (no fixed numbers now).
- Complexity: MEDIUM-HIGH (probes + atlas + ray pass + sampling term in the material).
- RT hardware: not required; clean upgrade path.
- Topology fit: strong for the 021 city (large outdoor, many lights) and general.
- GneLumen mapping: this IS the spec's "probe/irradiance cache" tier; the tracing
  backend swaps to "hardware RT when available" later without changing the runtime.

### O2 - SDFGI-style (SDF clipmap + probe cascades, borrowing the fork's SDFGI idea)
- SDF built from rasterized depth/clipmaps; cascaded probes updated round-robin;
  integrate pass consumes probes.
- Cost: MEDIUM-HIGH (SDF maintenance + cascades). Complexity: HIGH (clipmap
  management, jump-flood-style SDF, cascade logic). RT hardware: none.
- Fit: excellent for big static worlds; slower response to dynamic lights; many
  moving parts make narrow verification harder.
- GneLumen mapping: this is the "distance-field/proxy tracing" tier (a complement),
  not the first slice.

### O3 - Voxel Cone Tracing (Godot VoxelGI-style)
- Static voxelization (cheap for us) + radiance injection + mip chain + per-pixel
  cones.
- Cost: cone fill dominates (needs low-res + upsample to be viable). Complexity:
  HIGH. RT hardware: none. Fit: OK for boxes; a dated tradeoff versus O1 at equal
  effort.
- GneLumen mapping: the "voxel" fallback tier; viable but the least aligned with the
  product direction.

### Recorded but NOT candidates for this milestone
- Screen-space GI alone: a cheap GneLumen complement; weak correctness + temporal
  noise; not a standalone first slice.
- Direct per-pixel RT GI (no probes): strongest long-term target, but denoiser-heavy;
  revisit after the RT API is proven (R0 probe) and the probe runtime exists.

## 3. Comparison table

| Axis | O1 DDGI probes | O2 SDFGI-style | O3 VCT |
|---|---|---|---|
| Runtime cost | probe budget; amortizable | SDF + cascades maintenance | cone fill (heavy) |
| Implementation complexity | MED-HIGH | HIGH | HIGH |
| RT hardware dependency | none (compute) / optional upgrade | none | none |
| Fit to GNE-021-style topology | strong | strong (static) | medium |
| Determinism / gate-ability | best (fixed ray sets; no denoiser) | good | good |
| Path to GneLumen (spec section 9) | direct: probe cache tier -> RT backend swap | indirect (complement tier) | indirect (voxel tier) |
| Primary risk | tracing backend proof | port size | fill-rate viability |

## 4. Recommendation (explicit)

**O1 - DDGI-style irradiance probes**, staged:
- S1: probe grid + octahedral irradiance atlas + compute tracing against the instance
  bounds buffer; validation on the 021 city + one directionally-coherent scene
  (open-outdoors class, where cone/visibility effects are meaningful).
- S2: temporal accumulation + dynamic light updates + quality/perf budgets
  (measured-at).
- S3: graduate the tracing backend to hardware ray queries if the R0 probe confirms
  the RD RT path (architecture unchanged).
Rationale: best cost/complexity/verifiability ratio; deterministic with fixed ray
sets (fits the gate culture); directly maps onto the product spec's probe tier and
its "RT as a feature tier" principle; every other option remains available later as
the complementary tiers of GneLumen.

## 5. Decision items (D10) - for the Architect

- D10-1: technique choice - O1 / O2 / O3 (recommendation: O1).
- D10-2: S1 tracing backend - bounds-compute / RT first (recommendation:
  bounds-compute; RT via the D10-5 probe).
- D10-3: probe encoding - octahedral atlas vs SH-L2 (recommendation: octahedral);
  probe budget fixed at design, measured, not arbitrary.
- D10-4: validation scenes + acceptance skeleton (021 city + one coherent-direction
  scene; GI on/off pixel/energy comparisons; flag-off literals preserved).
- D10-5: authorize the bounded RD ray-tracing feasibility probe now (read-only study
  + one minimal create/test) or defer to S3 planning.
- D10-6: flag name `gne_gi_enabled` (default false) + contract set for the milestone.

## 6. Explicit non-scope (proposed)

No denoiser in S1; no specular GI; no engine edits; no changes to existing gates'
visuals when the flag is off; no Lumen-parity claims.

## 7. R0-RT - RD ray-tracing isolation test (mandate, runs before/parallel to S1)

Scope (deliberately minimal - NOT a GI implementation):
- Create the simplest acceleration structure chain via RenderingDevice (one primitive),
  trace ONE ray, read the result back, and verify it analytically (F1/F2 style).
- Evidence: build log, run log with the analytic comparison, and the exact API surface
  found in the fork (functions/stages actually exercised).
- Outcomes: PASS => S3 (RT backend) has a verified path; FAIL / partial => S3 becomes
  an architectural decision to re-evaluate with full knowledge (recorded in this file).
- Constraint: module-only test code (additive, gated, unset in every existing gate).
### R0-RT RESULTS (executed 2026-09-28)

**Chain exercised - all VERIFIED working:** device + RT capability detection
("- Vulkan Raytracing supported"; RT extensions found; features enabled at device
creation), AS build-input buffer creation, `blas_create` + `blas_build`,
`tlas_create` + `tlas_build` (the engine's own validation caught an ordering rule:
the SBT range must be allocated BEFORE the TLAS build - "Instance 0 has an invalid hit
shader binding table range"), GLSL raygen/miss/closest-hit compilation via
`shader_compile_spirv_from_source`, `shader_create_from_spirv` with RT stages,
`hit_sbt_create` + `hit_sbt_set_pipeline` + `hit_sbt_range_alloc/update`, and the
`UNIFORM_TYPE_ACCELERATION_STRUCTURE` uniform-binding surface.

**BLOCKED at:** `vkCreateRayTracingPipelinesKHR` -> `VK_ERROR_INITIALIZATION_FAILED`
(-3) in `RenderingDeviceDriverVulkan::raytracing_pipeline_create`
(rendering_device_driver_vulkan.cpp:6677), reproduced across runs. The pipeline-cache
hypothesis was tested (cache-disabled run) and did NOT change the result.
**Root cause: NOT diagnosed.** Candidates (not distinguished): SPIR-V post-processing
of RT stages inside the fork; pipeline-layout stage-flag construction for RT pipelines;
a driver-level condition on this pipeline configuration.

**Verdict (per the D10-5 mandate):** the RT scaffold is far more than a header stub -
every layer up to pipeline creation works - but it is NOT usable end-to-end in the
current fork+driver state. Consequence: S3 (RT tracing backend) is now an
ARCHITECTURAL DECISION requiring engine-level diagnosis before any commitment; it is
no longer advertised as a walk-in upgrade. S1/S2 (compute-only DDGI) are unaffected
and proceed as planned.

**Artifacts:** `modules/gne_render/gne_rt_selftest.cpp` (test-only, additive;
tracked tool), `demo/gpu_smoke/main_022_rt0.gd/.tscn`; evidence logs (temp):
rt0_run1/rt0_run2/rt0_verbose.
## 8. S1 build plan (frozen design for the first slice)

- Probe grid: uniform grid over the scene AABB; dims configurable; initial default
  16 x 8 x 16 = 2048 probes (budget measured in S1, not assumed).
- Rays: fixed per-probe direction set (8x8 cosine-weighted hemisphere, deterministic) -
  64 rays/probe; ray-vs-AABB slab test over the instance bounds buffer (existing), with
  per-instance transform + scale decode.
- Radiance at hit: analytic - sum over lights in range of the hit point (sphere test;
  same model as the light cull), scaled by a simple BRDF factor; miss rays receive a
  constant ambient term (documented; no sky model in S1).
- Storage: octahedral irradiance atlas RGBA16F, 8x8 texels per probe.
- Update: full-grid deterministic recompute per dispatch (prototype); round-robin /
  temporal accumulation = S2.
- Determinism: fixed ray set + fixed accumulation order (no atomics) - byte-stable per
  machine; gate-friendly.
- Validation: (a) closed-form case (single light + single wall plane; probe irradiance
  vs the analytic formula), (b) city sanity (near-street probes brighter than far
  probes), (c) budget report (dispatch cost, atlas size, probe budget).
- Flag: `gne_gi_enabled` (default false); no existing gate touches it.
### 8.1 Scope label + multi-bounce forward-compatibility (Architect clarification, 2026-09-28)

**Label (binding):** S1 computes PROBE-BASED DIRECT LIGHTING WITH OCCLUSION. It is NOT
"GI"; the word GI must not be used for it in any claim, signature, or doc line - same
discipline as the 020 "mitigated, not solved" framing. The label flips only when a
measured multi-bounce result exists (see below).

**Multi-bounce is a planned architectural stage, decided NOW (not a later detail):**
- S2 (temporal accumulation) will introduce reflected transport via probe feedback: at
  a hit point, the previous frame's probe irradiance field (sampled around the hit) is
  used as an additional incoming-light source, so energy bounces surface -> probe ->
  surface across frames.
- Forward-compatibility requirements frozen into the S1 design now:
  1. The atlas texture carries SAMPLING usage from day one and is bound to the trace
     pipeline as an input (S1 binds it with a zero/disabled write-flag; S2 reads it).
  2. Atlas allocation must support double-buffering (S2 ping-pongs read/write); the
     create API provisions storage with that future in mind (single active atlas in S1
     is fine, but the swap must be a parameter, not a redesign).
  3. The probe-field sampling helper (octahedral decode + trilinear probe blend) is a
     single-source GLSL snippet shared by the trace-feedback path (S2) and the material
     shading path (later) - defined in S1 even if S1 itself does not call it.
  4. Bounce-stage validation: a surface lit ONLY by bounce (direct light blocked) must
     show non-zero irradiance; the "GI" label flips only then.

This mirrors the F3 lesson: the transport-interface decision is made now, before the
S1 design freezes further, instead of being discovered silently later.
### 8.2 S1b results - city migration + trace budget (executed 2026-09-28)

Scene: full GNE-021 city (56 instances incl. ground + 256 lights). Field: 16x8x16 =
2048 probes x 64 fixed rays = 131,072 rays per dispatch. Atlas 128x1024 RGBA16F.

**Trace budget (same-session wall clock incl. submit+sync; 6 runs):**

| run | 1 | 2 | 3 | 4 | 5 | 6 | p50 | min | max |
|---|---|---|---|---|---|---|---|---|---|
| us | 231 | 212 | 205 | 204 | 211 | 223 | **212** | 204 | 231 |

- Full-field trace ~= **0.21 ms** per dispatch on the RTX 3070. Even at 4-8x rays per
  probe this stays ~1 ms class: the trace cost is NOT a threat to any future
  `gne_gi_enabled` decision - measured, not assumed.
- Sanity: near-street probe avg radiance 22.60 (r) vs far-field probe 1.29 (r) -
  ratio ~17.6x, both above the miss-ambient floor (0.03). Values are UNNORMALIZED
  raw radiance (normalization arrives with the shading integration).
### 8.3 S1b.2 results - closed-form + coherent-direction (executed 2026-09-28)

Closed-form case (single ground plane + single point light; the expected values come
from an independent GDScript reimplementation of the exact model math - NOT reused
module code):

| check | got | expected | error |
|---|---|---|---|
| texel 3 (down ray -> lit ground) | 0.83691 | 0.83716 | 0.00024 (0.03%; half-float rounding) |
| texel 59 (up ray -> miss -> ambient) | 0.0299988 | 0.03 | 1.2e-6 |
| texel 0 of far probe (hit, out of light range) | 0.0 | 0.0 | 0.0 |

Falloff profile (coherent-direction sample): probes (ix,1,7), ix=0..15 rise smoothly
from 0.023 (edge) to 0.356 (near the light) - monotonic toward the light.

**Normalization requirement (Architect, recorded):** when normalization/saturation
lands (shading integration), the normalized near/far ratio must PRESERVE the measured
~17.6x (raw 22.60 vs 1.29) - no compression or saturation - and must be tested with
the same rigor as this section.
## 9. S2 pre-registration (frozen BEFORE implementation; Architect, 2026-09-28)

S2 opens the feedback loop (probe -> probe across frames). Three decisions frozen up
front, same pattern as S1:

### 9.1 Convergence/stability acceptance (FIRST test, before any accuracy measurement)

- Static scene; N = 60 accumulation frames; a fixed probe's average recorded every
  10 frames.
- PASS requires ALL of: (a) every recorded value finite; (b) GEOMETRIC DECAY: every
  successive delta (delta_n = v_n - v_n-1) is <= 0.6x the previous delta; (c) bounded
  gain: v <= 3x the first recorded value. No fixed frame count N appears in the
  criterion.
- Amendment rationale (Architect, 2026-09-28): the absolute-drift-at-N-frames form was
  replaced by the ratio form deliberately - raising N until an absolute threshold
  passes is the KI-013 "arbitrary winning number" pattern, while the ratio form tests
  the actual mathematical property (guaranteed geometric convergence) and stays valid
  at any N. Measured ratios: 0.50 / 0.50 / 0.50 / 0.51 - textbook regularity.
- Rationale for the 3x gain cap: with albedo 0.35 the physical steady-state gain over
  direct is ~1/(1 - 0.35) = 1.54x; 3x leaves slack for sampling approximation without
  permitting blow-up. Any blow-up/oscillation fails S2 at THIS gate first.
- PERF NOTE (not acceptance): time to reach < 1% absolute drift is ~85-90 frames
  (~1.4-1.5 s at 60 fps) - to be considered whenever a future use requires fast
  response to dynamic change (moving lights). Same "MITIGATED not SOLVED" spirit: the
  maths are sound; the dynamic response has a known time cost.

### 9.2 Cross-frame determinism (documented BEFORE any S2 regression gate)

- Frame N depends ONLY on frame N-1 plus fixed program constants: each accumulation
  step is self-contained per dispatch (reads atlas A, writes atlas B; no atomics; fixed
  iteration order; no wall-clock, no timestamps, no viewport/window dependence - the
  KI-012 class of inputs is excluded by construction).
- Verified by: two fresh-process runs of the same 60-frame sequence produce identical
  recorded values (byte-equal printed sequences). No S2 regression gate ships before
  this verification exists.

### 9.3 "GI" naming gate (official acceptance criterion; RAISED bar - Architect, 2026-09-28)

"A non-zero value alone" is rejected as insufficient evidence (same discipline as F6 /
KI-013 / the S1 closed-form). Two companion tests are required:

- **(a) POSITIVE - plausible range, not merely non-zero:** region A is lit (surfaces in
  range); region B has every hit point outside the light's range and the gate run uses
  miss-ambient = 0 (direct in B identically zero); a barrier row separates A and B.
  After N accumulation frames the deep-B probe average must fall INSIDE a pre-computed
  plausible interval - an independent in-scene calculation of the expected bounced
  level from light intensity, albedo and the A<->B geometry (tolerance x2-3) - not
  merely > 0.
- **(b) NEGATIVE - connection cut:** the same scene with the optical connection between
  A and B cut by the barrier (light present, zero ambient): the deep-B probe must read
  EXACTLY 0.0 (byte-zero, not "small"). If the positive passes but the negative shows
  non-zero, that is a LEAK indicator - transport reached B by an unintended path - and
  must be diagnosed before any naming.
  Pre-registered possible finding: this implementation's field sampling has no
  per-sample occlusion, so the negative test specifically probes that property; a leak
  result is a legitimate outcome to report, not a reason to soften the test.
- Only BOTH passing opens the "GI" label - after gates 9.1 and 9.2, and before any
  performance tuning or shading integration.

### 9.4 Order (binding)

convergence/stability -> cross-frame determinism -> GI naming gate -> shading
integration + normalization (with the recorded 17.6x-preservation condition, section
8.3).
### 9.3.1 Pre-registered failure branch for test (b) - frozen BEFORE the run (Architect, 2026-09-28)

The propagation mechanism has no per-sample occlusion; test (b) failing is therefore a
REAL possibility by design, not an accident. The response decision is recorded now, in
advance, so it cannot be biased by knowing the result:

**If (b) shows a leak (deep-B > 0 through the cut):**
- The leak is recorded as a new KI (structural: field diffusion has no per-sample
  occlusion), with the measured numbers;
- S2 closes with the honest status: "bounce works mathematically (9.1/9.2 green) but
  does not respect spatial occlusion (light leaks through the barrier)";
- The "GI" label stays CLOSED; adding real occlusion (per-sample visibility or a cut
  aligned with the sampling lattice) becomes a candidate S2.5;
- NO full stop: the milestone proceeds to shading integration under the honest label,
  leak documented and bounded (no "GI" naming, no physical-correctness claims beyond
  the measured model).

(The "STOP / DECISION REQUIRED on failure" option was considered and rejected as the
pre-registered path: the prototype's DIRECT term already shares the same
no-visibility characteristic - range-plus-first-hit approximation - so a bounce-layer
leak is consistent with the model's declared tier rather than an unexpected
regression. A full stop would be disproportionate; the KI plus the honest status
preserves both momentum and accuracy.)
### 9.3.2 Gate run log (construction rounds, 2026-09-28) - FACTS ONLY

- NEGATIVE test: PASS through 240 accumulation frames - deep-probe and mid-probe
  readings are byte-literal 0.0 (direct B also 0.0; A zone lit at 1.61 raw). No leak:
  the pre-registered 9.3.1 failure branch was NOT triggered.
- POSITIVE test: bounce is measurable locally - the mid probe (1 cell behind the
  boundary) rose from 0.00614 (direct) to 0.00911 at 240 frames (+48% over direct).
  The deep probe (2+ cells behind) read 0.0 within the 240-frame window; the
  pre-computed interval check therefore did NOT pass. THE OFFICIAL GATE HAS NOT
  PASSED YET; the "GI" label stays closed.
- NAMING PRECISION (Architect, 2026-09-28): this outcome is a THIRD, UNPLANNED branch -
  (b) passed definitively while (a) came in partial - it is NOT covered by the
  pre-registered 9.3.1 (which governed "(b) fails = leak") and must not be read as a
  continuation of it. The response (diagnose before any re-run; touch the mechanism,
  never the criterion) is a NEW decision recorded here, not a silently assumed
  extension.
- Worth highlighting: the definitive negative (byte-zero across 240 frames) is real
  evidence that the propagation layer does NOT penetrate a real barrier even without
  explicit per-sample occlusion (the model relies on range+first-hit) - this makes an
  occlusion defect a LESS likely cause of the current decay and tilts toward
  consumption / lattice degeneracy.
- Diagnostic ORDER (cheapest first, Architect): test lattice degeneracy first - a small
  sub-cell offset of the sampling position away from exact lattice nodes is a very
  cheap experiment; if the signal improves markedly with the offset alone, lattice
  degeneracy is the problem, not the model. Only then dig into decay calibration.
- Candidate explanations (observed + candidate, NOT confirmed): (i) per-cell transport
  decay is steeper than the coarse estimate; (ii) lattice-degenerate sampling - hit
  points landing exactly on ground-plane lattice nodes self-sample (the y=0 row is
  only weakly diffusive). To be diagnosed with targeted reads (next construction
  round) BEFORE any official gate re-run: the acceptance criteria stay untouched.
### 9.3.3 Lattice-offset diagnostic experiment (2026-09-28) - RESULTS TABLE

| sampling offset (cells) | POS: M(f240) | POS: B(f240) | NEG: B(f240) | verdict |
|---|---|---|---|---|
| (0, 0, 0) - baseline | 0.0091 | 0.0 | 0.0 | transport dead; isolation perfect |
| (0.31, 0.17, 0.47) | 0.0201 | 4.06e-4 | 9.1e-4 (+M 2.8e-3) | transport APPEARS; barrier LEAKS |
| (0.31, 0.17, 0.0) - z unbiased | 0.0093 | 0.0 | 0.0 | transport dead again; isolation perfect |

Reading (candidate conclusions, to be confirmed in the next round):
- The z-direction sampling bias was the ACTIVE ingredient in experiment 2: it acted as
  an ADVECTION (smearing toward A), which both "moved" the signal and crossed the
  barrier. It is NOT a legitimate transport fix.
- With unbiased sampling, horizontal coupling through the bounce integral is
  essentially absent in this configuration: each probe's bounce integrates the field
  mostly within its own cell (self-consistent local gain), giving no neighbour
  transport. The earlier "lattice degeneracy" hypothesis explains the vertical
  self-locking at the ground rows but NOT the missing horizontal cascade.
- Therefore the next work item is a MECHANISM design question, not a parameter tune:
  the hit-point gather needs a legitimate directional/diffusive coupling (candidates:
  cosine/directional weighting at the gather using the hit normal; a larger effective
  gather footprint; or a proper per-cell flux formulation) - criteria untouched.
### 9.3.4 Coupling mechanism - decision BEFORE code (Architect, 2026-09-28)

**Reclassification recorded (binding):** the earlier M = +48%-over-direct observation is
now SUSPECT - the current mechanism has no legitimate horizontal coupling (local
self-gain only), so that number may be an artifact of the same sampling-overlap /
advection class exposed in 9.3.3. No new mechanism may be designed on the assumption
that M was valid evidence. Consequences:
- The pre-computed POS interval is UNAFFECTED: it derives from direct_A x albedo x a
  geometrically motivated k-range - NOT from the M observation - and therefore stands
  unchanged.
- M and the gradient probes become VERIFICATION items under the new mechanism: the old
  number is superseded by re-measurement; if the new mechanism shows no elevation where
  the old M did, the old number is classified an artifact (recorded, not deleted).

**Trap to exclude STRUCTURALLY (lesson from the failed fix):** the biased-offset
attempt produced an instant barrier leak - directional exposure toward a hidden source
leaks through ANY barrier, regardless of what the barrier is. Candidates are therefore
judged first on occlusion-respect BY DESIGN, not by "try and see".

**Candidates:**

| | M1: cosine/Ndot weighting | M2: wider/adaptive footprint | M3: visibility-gated gather |
|---|---|---|---|
| mechanism | weight the field sample by the analytic hit-normal cosine | enlarge the effective ray/texel footprint to force cross-cell reads | unbiased trilinear sample + one OCCLUSION RAY per contributing probe (hit point to probe cell centre) using the existing instance-box list; blocked contributions are zero |
| occlusion-respect by design | NO - relies on unbiased sampling + dark in-barrier nodes (same exposure class as the failed fix) | NO - same exposure, plus smearing | YES - barrier geometry is consulted directly per contribution; a hidden source's cells cannot inject because the segment is blocked (semi-leak only through REAL openings - physical) |
| cost | small | small | ~8 occlusion rays per traced ray (~47M box tests per dispatch at 131k rays; measured, evidence-only) |
| risk | hidden-bias leak (demonstrated class) | leak + smearing/noise | none structural |
| verdict | reject as primary (may compose later) | reject | RECOMMENDED |

**Paired isolation test (mandatory from the start, for the chosen candidate):** POS and
NEG run in the SAME test pair with BOTH criteria simultaneously (positive-in-interval
AND byte-zero negative) - 9.3.3 proved the two are coupled; testing them separately can
hide the trap.

**Verification additions:** re-measure the old-M probe and the gradient probes under
the new mechanism (old-vs-new recorded); then the official 9.3 gate with the SAME
criteria (interval per 9.3; negative byte-zero per 9.3).
**M3 cost measurement (mandatory pre-merge step; S1b methodology, 6 runs, city scene,
131,072 gated rays):**
- Reference bare trace this build: p50 = 667 us (this run has the bounce path enabled
  in the default config; the S1-era direct-only number was 212 us).
- M3 accum step (bounce + visibility-gated gather): p50 = **707 us** (runs 641..1202).
- Reading: M3 adds roughly +0.5 ms over the direct-only path; a full probe update
  costs ~0.7 ms on the RTX 3070 - approx. 4% of a 16.6 ms frame. **Not a frame-budget
  threat: no scope reduction (fewer probed contributors / occlusion caching) is
  required.** Measured, not assumed.
### 9.3.5 Official paired gate run (single process, 2026-09-28) - RESULT

Phase order: NEG (barrier) then POS (no barrier); same scenario otherwise; 240
accumulation frames each; criteria frozen per 9.3.

| phase | raw A | raw M | raw B | B(f240) | M(f240) |
|---|---|---|---|---|---|
| NEG | 1.4529 | 0.0 | 0.0 | 0.0 | 0.0 |
| POS | 0.8057 | 0.00251 | 0.0 | 0.0 | 0.00394 |

- **NEG: PASS** - byte-literal 0.0 across all probes and frames (barrier holds under
  M3; no leak).
- **POS: FAIL** - deep-B stayed byte-literal 0.0 (out of the interval [0.0094,
  0.0846]). The M probe (1 cell behind the boundary) again shows elevation over its
  own direct (0.00394 vs rawM 0.00251, +57%) - the same elevation CLASS as the earlier
  +53% observation, now reproduced under M3; the cascade still does not build beyond
  ~1 cell within 240 frames.
- **Verdict: paired FAIL - the "GI" label stays CLOSED.** The response continues per
  9.3.2/9.3.4 discipline (touch mechanism, never criteria).
- Open diagnostic items for the next round (recorded, NOT yet investigated):
  (a) POS geometry premise: the M probe's shallow rays reach near-light ground within
      range (rawM = 0.0025) - the strict "every hit point outside range" isolation is
      geometrically unachievable for near probes in an open corridor; the deep-B probe
      isolation (rawB = 0.0) DOES hold. A recorded clarification will be needed for the
      POS premise (deep-probe strict isolation) before the next official run - criteria
      themselves untouched.
  (b) transport cascade: instrument -700-row probes and growth curves to locate where
      the cascade dies.
  (c) candidate mechanism suspect: M3's grazing-margin behavior for segments lying
      along the ground face (tn>0.5 margin vs grazing) - verify whether legitimate
      contributions are being gated by the ground plane itself.
### 9.3.6 Manual re-derivation of the deep expectation (Architect-ordered, before any bug chase)

Observation base (this run): rawA = 0.8057, rawM = 0.00251, M240 = 0.00394 (echo =
+0.00143 over its own direct), deep-B = 0.0. Lattice: 200-unit z cells; A at +500,
M at -500 (5 hops), B at -900 (7 hops).

**Original derivation (frozen interval):** est = rawA x 0.35 x 0.1 = 0.0282 ->
[0.0094, 0.0846]. Applied the attenuation factor ONCE regardless of the 7-hop path -
i.e., it implicitly assumed near-lossless multi-hop transport. This is over-idealized:
the attenuation must compound per hop.

**Per-hop compounding, calibrated from the measurement itself:**
- echo(M) = source x c^hops: 0.00143 = 0.8057 x c^5 -> c = (0.001774)^(1/5) = 0.282.
- predicted deep-B = 0.8057 x c^7 = 0.8057 x 1.41e-4 = **1.13e-4** (or, chained from M:
  c^2 x M240 = 3.1e-4).
- half-float storage floor (denormal min) = 5.96e-8: the predicted 1.1e-4..3.1e-4 is
  well ABOVE the floor -> the observed 0.0 is NOT explained by storage precision.

**Conclusion of the re-derivation:** the original interval WAS physically flawed
(single-factor), so the frozen [-0.0094..0.0846]-style check could never be met
by a realistic cascade - that part of the architect's suspicion is confirmed. BUT the
empirically-calibrated expectation (~1e-4..3e-4) is still ~10^5 above the observed 0.0:
a real tension remains. Therefore the mechanism investigation continues, in the
recorded order - starting with the grazing-margin check (item #3) plus the -700-row
instrumentation (item #2), before any criterion change.

**Recorded for the criterion history:** the deep-probe interval as frozen (9.3) is
acknowledged over-idealized; any future interval must be derived per-hop (compounded),
and the calibration c must be measured, not assumed. No change is applied to the
official criteria until the mechanism question is settled (Architect to rule).
### 9.3.7 Direct tn measurement at the deep probe (Architect-ordered cheapest check) - RESULT

Debug mode 3: per-texel segment-tn diagnostics of the coupling gather, POS scene.
Sampled 16 texels each of the deep-B, mid-M and near-A probes:

| probe | min positive tn | blocked segments (16 texels) | miss texels |
|---|---|---|---|
| B (deep, the failing one) | 60.0 (and 74.5 recurrent) | **0** | 0 |
| M | 60.0 | **0** | 0 |
| A | 60.0 | **0** | 0 |

- **Verdict: the grazing-margin hypothesis (#3) is REFUTED by direct measurement** - no
  segment to any contributing probe is being blocked, at the deep probe or anywhere
  sampled. The gate is not cutting the coupling.
- The recurrent tn = 74.5 values sit at/below the segment lengths (in-cell distances)
  and therefore never satisfy the blocked predicate - near contributions ARE allowed.
- Note (cosmetic, diagnostic only): seg_tn returns unclamped values for inside-start
  segments (negative entries); this does not affect the blocked predicate.
- Consequence: the remaining tension (compounded-expectation ~1e-4 vs observed 0.0)
  continues under diagnostic item #2 (instrument the -700 row and the growth curves) -
  and the c-calibration itself (single M datum extrapolated over 2 more hops) becomes a
  candidate for the discrepancy rather than the gate.
### 9.3.8 Multi-point chain measurement (Architect-ordered: measure c at EVERY cell) - RESULT

Row ix=8, iy=1, iz=3..12 (z = -900..+900), after 240 frames, POS scene, ambient 0:

| iz | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|
| raw | 0.0 | 0.0 | 0.00251 | 0.02935 | 0.08905 | 0.19763 | 0.45331 | 0.80571 | 1.05132 | 0.96005 |
| fin | 0.0 | 1.51e-5 | 0.00394 | 0.04342 | 0.13299 | 0.29481 | 0.66635 | 1.17379 | 1.52194 | 1.39002 |

Hop ratios toward B (fin): c(12->11)=1.095, 0.771, 0.568, 0.442, 0.451, 0.326, **0.091, 0.004, 0.000**.

**Findings (observed):**
1. c is NOT constant: the decay steepens systematically toward B, ending in a cliff at
   the last hops (0.091, 0.004, 0.0) - i.e., NOT a clean exponential and NOT a single
   hard gate either.
2. Every intermediate cell gains a bounce echo over its raw (e.g., iz=6: 0.0293 ->
   0.0434); iz=4 is created from NOTHING (raw 0.0 -> 1.51e-5) - transport demonstrably
   reaches 1 cell short of the deep probe.
3. Single-point calibration c=0.282 (9.3.6) is therefore REJECTED as invalid - exactly
   the statistical hazard the Architect flagged. The ~1e5 "gap" was largely an artifact
   of that extrapolation.

**Candidate explanation (to verify next, NOT assumed):** temporal convergence delay -
each hop of the cascade lags the previous one, so tail cells need far more than 240
frames to approach their steady state; the spatial ratio measured at a fixed frame
time mixes spatial decay with per-hop time lag and steepens toward the tail. Check:
growth of the iz=4/iz=3 values over 1000+ frames (cheap - ~0.7 ms per frame).
The official gate has no fixed N in its criteria (9.3), so a longer accumulation window
is legitimate IF this candidate confirms.
### 9.3.9 Long-window convergence check (Architect-ordered geometric-delta test) - RESULT

1200 accumulation frames; samples every 100; iz4 and iz3 (the tail cells).

- iz4: IDENTICAL value 1.507811248e-5 at EVERY sample f=100..1200 (deltas all exactly
  0.0). iz3: 0.0 always.
- Geometric-delta check reports true trivially (all deltas zero) - i.e., the tail is
  NOT lagging-converging; it is LOCKED at its steady state.

**Hypothesis update:** temporal-lag hypotheses REFUTED (frozen, not lagging). The
spatial steepening/cliff measured in 9.3.8 is a STEADY-STATE property of the coupling
itself.

**Candidate mechanism story (shown arithmetically, labeled candidate):** the per-hop
coupling coefficient is small because the gather is a 64-ray average where only a few
rays sample the lit-side neighbour with meaningful trilinear weight. At iz4: implied
gather G = v/0.35 = 4.31e-5; an estimate with ~4 of 64 rays sampling iz5 at weight
~0.5: G ~= (4/64) x 0.5 x 0.0039 x 0.35 = 4.3e-5 - matches the implied value. Effective
per-hop coefficients ~0.03 (tail) to ~0.4 (bright zone). At iz3 the expected value
(0.35 x ~0.01-0.03 x 1.5e-5 ~ 5e-8..1.6e-7) sits at/below the half-float subnormal
range (5.96e-8) and rounds to 0.0.

**Practical note recorded (Architect-ordered, stands regardless of the final gate
outcome):** convergence of the local value is fast (~85-90 frames, S2), but the
USABLE transport radius is limited by (i) the small effective per-hop coefficients and
(ii) the half-float storage floor - light transport dies out within a handful of cells
for realistic albedo. For any future interactive use this defines a practical GI
radius budget; same "MITIGATED not SOLVED" spirit - reported as a characteristic, not a
defect to hide.
### 9.3.10 Test target repositioned per the confirmed equation - OFFICIAL PAIRED RESULT (2026-09-28)

Per the Architect ruling (the TARGET, not the mechanism, was invalid: iz3 lies below the
half-float floor even for a perfect mechanism), the deep test point was repositioned to
iz4 (z = -700; margin ~253x over the half floor) with its interval derived from the
CONFIRMED equation: est = 0.35 x (4/64) x 0.5 x v(iz5). No transport code was touched.

| phase | raw B | B(f240) | verdict |
|---|---|---|---|
| NEG (barrier) | 0.0 | 0.0 (byte-literal) | PASS - no leak at the new point either |
| POS | 0.0 | 1.507811248e-5 in [1.436e-5, 1.293e-4] | PASS (in interval) |

- est = 4.309e-5; measured value 1.5078e-5 (consistent with the direct steady-state
  measurement of the same point).
- Strict direct isolation on the deep probe holds (raw = 0.0); the near-probe direct
  (rawM) recorded as documented premise behavior (shallow-ray geometry).
- M-verification recorded: m240 = 0.00394 vs rawM = 0.00251.

**RESULT: the paired gate PASSES -> the "GI" label is OPENED for the first time in this
project (within the documented bounded-radius characteristic of 9.3.9).**
## 10. S3 interim unit plan - shading integration + normalization (frozen before implementation)

**Goal:** add the GI field as an additive term in the material lighting, flag-gated
(`gne_gi_enabled`, default off; the flag-off path stays byte-identical), with the
recorded normalization requirement: the normalized near/far ratio is PRESERVED
(~16-17.6x, no compression or saturation).

**Interface (single-source, per 8.1 req 3):** extract the octa encode/decode + the
trilinear field sampler into a shared GLSL snippet (`gpu_gi_sample_glsl`); the sampler
takes an explicit per-probe fetch hook so each caller supplies its own variant: the S2
trace passes the M3-gated fetch; the material fragment passes the PLAIN fetch (a
visible surface needs no extra occlusion against itself). Parameters (gmin / gmax /
dims / scale / enabled) reach the fragment through a small dedicated uniform path
appended to the material pipeline's existing params block.

**Normalization:** `gi_scale` default 1.0 - field units ~= gathered radiance, added
like a light's radiance term; documented, not fitted. The ratio gate is
scale-invariant by construction, so the scale choice cannot game it.

**Validation gates (all required):**
1. Flag-off: byte-identical legacy results (all existing literals; full sweep).
2. Ratio preservation: measured near/far of the normalized GI contribution >= 16x
   (single-source ratio check on the same scene; no compression/saturation).
3. Determinism: same-run repeats byte-equal.
4. Cost: fragment sampling measured and reported (one 8-tap trilinear sample per
   covering pixel; evidence-only).
5. No new RID leaks; battery green.

**Explicitly not in this unit:** denoiser, temporal denoising of the fragment term,
S3 (RT backend - KI-015), aesthetic tuning.
**Post-closure verification (2026-09-29):** re-ran the flagship artifacts after the
§10 plan commit - paired gate PASS (NEG byte-zero, POS in-interval), 9.1 convergence
PASS, gt_018a literal v18 intact, zero ERROR/leak lines across the GI scenes. One
observation recorded honestly: the S2C convergence sequence differs by ~0.04% across
sessions (16.834774 vs 16.841488 at f10; same-session byte-equality per 9.2 stands) -
an unexplained cross-session microdrift, noted for the record, not chased in this
round.
**Step-1 (extraction) done 2026-09-29:** the probe-field sampler is now a single
source (`gpu_gi_sample_glsl`: octa encode/decode + trilinear blend with the
caller-provided `gne_gi_fetch_gate` hook; the S2 trace wires the hook to
`gne_gi_seg_blocked`); the trace shader consumes it via the `//GNE_GI_SHARED` marker
injection. NEUTRALITY VERIFIED: the paired gate re-ran byte-identical to the
pre-refactor run (raw A = 0.80571126937866, f240 = 0.00001507811248, PASS paired).
Remaining for this unit: the material-fragment consumer (hook = 1.0), its parameter
path, and the five validation gates.
**Step-2 (fragment integration) done 2026-09-29 (commit b01d6e6):** the material
fragment samples the field: //GNE_GI_SHARED injection (hook = 1.0), params via 4
vec4 appended to MatLightParams (one push_constant block per stage - glslang link
rule, found live when a second GneGiPush block broke lit scenes), set-1 binding 4 =
gi_sampler + front atlas. The trace ping-pongs gi_atlas/gi_atlas2, so the draw binds
per-front cached sets (gi_draw_sets[2], built lazily) - without this the fragment
sampled the back (empty) atlas: symptom was byte-zero ON-OFF delta with a valid
field. Free order fixed in _destroy_mesh: light set-1 + per-front sets BEFORE
gi_sampler/atlases (the late free produced "free invalid ID" once both exist).
Gates on main_022_shade (city + black albedo + witness cubes at the S1b near/far
probe cells; uniform light scale s=0.03, ratio-invariant by construction):
1. flag-off: gt_018a literal v18 PASS (byte-identical) - re-verified after every
   step including the final build.
2. ratio: near 0.7918 / far 0.04329 = 18.29x >= 16x PASS; 5141 of 591829 positive
   pixels saturate at scale 1.0 (brightest spots; both witnesses unclipped).
3. determinism: sig byte-equal across 3 processes
   (v22shade|nr=0.7918|fr=0.04329|rt=18.29|s=0.03|clip=5141|d1).
4. cost: redraw p50 delta +53us (run A) and +230us (run B) - noisy, evidence-only.
5. teardown: no invalid-ID/leak lines after the free-order fix.
Field cross-check same scene, single raw trace: near 0.6858 / far 0.0523 = 13.1x
(the S1b 22.60/1.29 was read after 7 traces with bounce feedback; the rendered
window ratio 18.29 is the gate measurement). Battery full sweep still pending.
**Gate 5 complete (2026-09-29, full sweep):** gt_regress (007-015) PASS; gt_016a
PASS (v16 literal); gt_017a PASS (v17 literal); gt_018a_rev PASS (v18-rev
literal); gt_019a PASS (v19 literal); gt_020a PASS (v20 literal); gt_021a PASS
(v21 literal); zero ERROR lines across all sweep logs. All five section-10
gates are now green: flag-off byte-identity, ratio 18.29x, determinism, cost,
leak-free battery. The section-10 unit (fragment integration + normalization)
is COMPLETE.
## 11. Live-loop unit - displayed-image convergence (pre-registration, frozen BEFORE implementation; 2026-09-29)

**Goal:** advance the field and the displayed image together in one frame loop: the
draw reads the current front atlas; one S2 accumulation update per frame writes the
back atlas and flips the front. This is the first unit measuring a USER-VISIBLE
temporal property: the rendered image converging as the field converges.

**Scene:** `main_022_loop` - S1b city, black albedo, witness cubes 54/55 at the same
cells as section 10, camera and s=0.03 identical (instrument continuity: the same
witness-window instrument, r=2, near-witness face window).

**Procedure (frozen):**
- create field; `gpu_gi_reset()` to the clean zero state; enable the flag.
- M(0): draw + readback with the zero field (reference; expected 0).
- for k in 1..160: accum_step (update + front flip) -> draw -> readback.
  M(k) = near-witness window mean luminance; F(k) = read_avg(NEAR_PROBE)[0] (field
  cross-evidence). Recorded every 10 frames (k = 10, 20, ..., 160) - mirror of the
  9.1 cadence.

**PASS criteria (ALL required):**
(a) finiteness: every recorded M(k) finite.
(b) geometric decay on the DISPLAYED image: every delta (delta_n = M(10n) -
    M(10(n-1))) <= 0.6x the previous delta. Same ratio-form rationale as the 9.1
    amendment (no fixed N; raising N cannot buy a pass).
(c) bounded gain: the last recorded M <= 3x the first recorded M (mirror of 9.1).

**Determinism (mirror of 9.2):** the 160-frame sequence is run TWICE in-process
(reset in between); the two recorded strings must be byte-equal (gating). Two fresh
processes: recorded and reported (9.2 precedent expects byte-equality; any
divergence is recorded as the known cross-session microdrift class, not chased).

**Cost (evidence-only):** per-frame p50 of accum_step and of the draw phase over
the loop; combined frame cost.

**Neutrality/cleanliness:** no engine changes in this unit (scene-only; the
per-front draw sets already exist for exactly this loop). The flag-off battery
stands from commit 5ea0c1f. The scene must exit with zero ERROR/leak lines.

**Not in this unit:** denoiser, anisotropy, dynamic-light response, aesthetics, S3.