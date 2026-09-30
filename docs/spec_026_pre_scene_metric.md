# GNE-0.26-pre — Scene + Metric + Audit (SPEC v1.0)

**Status:** DRAFT v1.0 (2026-10-01, from SPEC 0.26 §11). No 0.26a/b/c work
starts before 0.26-pre approval. No C++ in this phase (scene + GDScript tool
only, provenance untouched).

## 1. Objective
Make 0.26 measurable before it is visual: register the Country House scene,
instrument the objective visual metric, and close the API audit so 0.26b knows
exactly which new APIs (if any) it must justify.

## 2. Background
- SPEC 0.26 DRAFT (6db8fd1) presupposed a scene, a scoring method and reusable
  post APIs. Audit 2026-10-01: the scene does not exist, no scoring instrument
  exists, and no bloom/tonemap API exists. This phase builds the first two and
  records the third.

## 3. Design

### 3.1 Scene: Country House definition
- Procedural construction via gpu_mesh_create_from_arrays (zero external
  assets - offline constraint); glTF import is the fallback ONLY with a
  vendored file (no network at build/run).
- Stored: demo/gpu_smoke/main_026_house.gd + .tscn (paths RESERVED by SPEC
  0.26 §11.1; files land in the implementation unit, not the SPEC).
- Required elements (mirror the 0.26a fix list so each has a subject):
  separated roof volumes (mesh + transform, no shared planes), a back wall as
  real geometry (not a culling artifact), a ground path with baked
  alpha/texture (no runtime alpha blending dependency).
- Camera: fixed, registered transform (determinism precondition).

### 3.2 Metric: objective, framebuffer-side
- Instrument (GDScript, existing readbacks only): per-frame 8-bit readback;
  contrast = stddev(luminance)/mean(luminance) over the frame; color variance
  = mean over pixels of per-pixel channel variance; edge density = fraction
  of pixels whose luminance gradient exceeds a REGISTERED threshold.
- Gates are RELATIVE to a pinned baseline row captured by this phase:
  contrast >= base x 1.10, variance >= base x 1.15, edge <= base x 1.20.
- HDR cross-evidence (gpu_raster_read_hdr) reported, not gated (FVD pattern).

### 3.3 Audit: existing APIs (CLOSED 2026-10-01 - SPEC 0.26 §11.3)
- Scene/metric/fix need NO new API. 0.26b bloom+tonemap CANNOT ride existing
  shaders (no fullscreen/post path); its new-API proposal is a separate
  Architect decision, not part of 0.26-pre.

## 4. Acceptance (ALL required for 0.26-pre approval)
1. main_026_house scene runs headless, exit 0, zero ERROR/RID lines.
2. Metric instrumented in-scene; one full run prints contrast/variance/edge.
3. Baseline row captured twice (d1/d2) byte-identical in the printed values
   that gate (tolerances, if any, pre-registered - not tuned post hoc).
4. Audit table (§3.3) reviewed unchanged.
5. No 016/017/018/019 literal moved (belt still 25-row green in owner shell).

## 5. APIs
- None new. Consumed: mesh_create_from_arrays, scene/instance/material/light
  setters, raster_read_pixels (+hdr cross-evidence).

## 6. Out of Scope
- Any 0.26a fix, any 0.26b visual, KI-019, new engine APIs, AA/TAA.

## 7. Deferred
- glTF path (only if procedural proves inadequate - with evidence).

## 8. Deliverables
- demo/gpu_smoke/main_026_house.gd + .tscn (implementation unit).
- Metric tool (in-scene; no new harness row until its own gate SPEC).
- This SPEC + SPEC 0.26 §11 amendment.

## 9. Decisions Required (before 0.26a starts)
- 0.26-pre approval itself (Architect).
- Gradient threshold value for edge density (pre-register, not post-tune).
- glTF-vs-procedural final call (default: procedural unless evidence).

## 10. Risks
- Procedural house may look too primitive to carry the 0.26b visual targets -
  contained by the glTF fallback + the fact that metric gates are relative.
- Relative gates can be gamed by a dark baseline; mitigated by publishing the
  absolute baseline row alongside every gated run.
