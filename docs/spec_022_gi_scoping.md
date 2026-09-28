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
