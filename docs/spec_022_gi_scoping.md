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