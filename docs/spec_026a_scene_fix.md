# GNE-0.26a - Country House Scene Fix (SPEC v1.0)

**Status:** DRAFT v1.0 (2026-10-01). No implementation before approval.

## 1. Objective
Fix the three scene defects of the 0.26-pre Country House
(`demo/gpu_smoke/main_026_house.gd`) that SPEC 0.26 section 3.1 lists, and
prove each fix by measurement rather than by eye.

## 2. Background
- 0.26-pre built the scene and the objective metric; baseline captured
  (contrast 1.857549, variance 0.002933922, edge 0.000654, det=1).
- The scene was authored specifically so that each defect has a subject: the
  roof slopes meet the body on a shared edge, the back wall is a separate
  box behind the body, and the path is a quad 0.6 units above the ground.
- Important finding already measured (0.26-pre): GNE's fragment normal comes
  from screen-space derivatives of world position, so there is no per-vertex
  normal and no winding requirement; backfaces are detected by
  `dot(N,V) < 0` (cpp:996) and drawn as emissive-only. Backface culling is
  therefore a DIFFERENT issue than winding, and each defect must be
  classified before it is fixed.

## 3. Defects and required diagnosis (each: diagnose -> fix -> measure)
- D1 Roof separation: the two roof slopes touch the body's top edge and each
  other at the ridge. Required: determine whether the visible artifact is
  (a) coplanar z-fighting, (b) a gap/light leak at the ridge, or (c) a normal
  discontinuity. Measure by sampling the ridge pixel band before/after.
- D2 Back wall: a box 40 units behind the body's back face. Required:
  determine whether it is missing, occluded, or shaded as a backface. The
  derivative-normal + backface rule makes "backface" the leading hypothesis.
- D3 Path: a quad at y=0.6 over ground at y=0.0 (no coplanarity). Required:
  determine whether the artifact is z-fighting (it should NOT be), a
  shadowing/lighting discontinuity, or a normal-direction problem.

## 4. Acceptance criteria (ALL required)
1. Every defect has a recorded root cause naming the mechanism (not "looked
   wrong").
2. Every fix is scene-data only - NO C++ change, so provenance is untouched.
3. Determinism preserved: det=1 and the run still exits 0.
4. Metric reported before/after (contrast/variance/edge) - reported, NOT
   gated: a scene fix is not required to raise the visual numbers, and a
   regression here must not be hidden by a moving baseline.
5. No 016/017/018/019 literal moves (25-row belt stays valid).
6. Negative control: with the fix reverted the defect returns (proves the fix
   is what changed the pixels, not noise).

## 5. APIs
None new. Consumed: `gpu_mesh_create_from_arrays`, instance/material setters,
`gpu_raster_read_pixels`, `gpu_raster_read_normal`, `gpu_raster_read_hdr`.

## 6. Out of Scope
- 0.26b visual features (sky/bloom/tonemap/contact shadows).
- Ambient model change (ADR-001: accepted as-is).
- KI-019, C++ changes, new APIs.

## 7. Deferred
- glTF path (procedural proven adequate for these defects).
- Any new engine-side normal/culling work.

## 8. Deliverables
- Fixed `main_026_house.gd` (+ `.tscn` if the node set changes).
- `docs/diagnostic_026a_scene_fix.md`: per-defect cause, before/after
  evidence, negative control.
- progress entry + register update.

## 9. Decisions required before implementation
- D11-8: is the 0.26-pre light (`LIGHT_HOUSE` = above-front sun) the
  instrument light for 0.26a, or should the fix be verified under
  `LIGHT_CANON` (which leaves outdoor tops at ambient and hides D1/D3)?
- D11-9: for D2, is adding a double-sided option acceptable, or must the fix
  stay inside the existing single-quad emission path?

## 10. Risks
- A "fix" that changes pixels without fixing a defect - mitigated by the
  negative control in criterion 6.
- Baseline churn: the metric is reported, not gated, precisely so the baseline
  cannot be tuned to hide a regression.