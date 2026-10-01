# GNE-0.26 — Visual Slice + Scene Fix (SPEC v1.0)

**Status:** DRAFT v1.0 (2026-10-01). No implementation before Architect approval.

## 1. Objective
- Fix Country House scene bugs (M0.26a).
- Visual quality slice (M0.26b).
- KI-019 resolution (M0.26c).

## 2. Background
- M0.25 closed as logical PASS, formal verdict env-pending (row 25 wired +
  pinned; owner-shell belt outstanding).
- Visual baseline 5.9/10 and target 7.0/10 are STATED REQUIREMENTS, not
  measurements - no visual scoring instrument exists in the repo. Criterion 6
  gates at >= 6.5/10; the scoring instrument itself is TBD (Architect).
- "Country House" has NO record in demo/ or docs/ - scene identity, location
  and current bugs are TBD input (Architect). §3.1 items below presuppose it.

## 3. Design

### 3.1 M0.26a — Scene Fix (scene TBD - see §2)
- Roof separation: mesh / transform / z-fight.
- Back wall: mesh missing / culling.
- Path: alpha / texture bake.

### 3.2 M0.26b — Visual (all APIs PROPOSED - none exist; only the KI-017 HDR
attachment exists today)
- Sky + atmosphere: procedural gradient.
- Bloom: threshold + intensity.
- Tone mapping: ACES or Reinhard (D11-1).
- Contact shadows: PCF + bias.

### 3.3 M0.26c — KI-019 (opened with this SPEC - see known_issues.md)
- Option A: filter user:// noise at the harness layer, OR
- Option B: clean-environment protocol (owner shell as reference).
- Decision: Architect (affects every future gate's ERROR scan).

## 4. Acceptance Criteria (8)
1. Scene bugs fixed (against the TBD scene inventory).
2. Sky visible.
3. Bloom active.
4. Tone mapping applied.
5. Contact shadows visible.
6. Visual >= 6.5/10 (INSTRUMENT TBD - criterion is not operational until the
   scoring method is defined and registered).
7. DET stable (v26|... literal pinned).
8. Regressions 0.20-0.25 PASS (full belt green in owner shell, incl. row 25).

## 5. APIs (PROPOSED - additive, scene-driven; no engine commitment until approval)
- gpu_sky_set(params).
- gpu_post_process_set(bloom, tonemap).
- gpu_shadow_set_contact(params).

## 6. Out of Scope
- GI changes.
- RT (KI-015).
- Render Graph.
- Raster depth HZB (deferred).

## 7. Deferred
- Full visual overhaul.
- AA / TAA.

## 8. Deliverables
- C++ module changes (only after approval + provenance re-stamp).
- Demo (main_026).
- Harness (gt_026a).
- Contract docs x5 (shipped with this SPEC as DRAFT companions).

## 9. D11 Decisions Required (Architect - implementation blocked until answered)
- D11-1: Tone map (ACES vs Reinhard).
- D11-2: Sky model (gradient vs procedural).
- D11-3: Bloom threshold.
- D11-4: Contact shadows params.
- D11-5: Signature format.
- D11-6 (added by executor): visual scoring instrument for criterion 6.
- D11-7 (added by executor): Country House scene identity + bug inventory.

## 10. Risks
- Provenance (any C++ change re-stamps; perf020 basis re-measured).
- Visual quality subjectivity (compounded by the missing instrument - D11-6).
- Regression risk (25-row belt must stay green; row 25 itself still awaits its
  first owner-shell verdict).

## 11. Pre-conditions (REQUIRED - Architect AMEND 2026-10-01)

### 11.1 Scene Definition (0.26-pre)
- Country House scene missing (verified: no record in demo/ or docs/).
- Must be created (procedural or glTF).
- Stored: demo/gpu_smoke/main_026_house.gd + .tscn.
- Must NOT use external assets (offline constraints).

### 11.2 Visual Metric (D11-6 revised - no subjective scoring)
- No subjective 5.9->7.0. Objective metrics from the framebuffer:
  - Contrast ratio >= baseline + 10%.
  - Color variance >= baseline + 15%.
  - Edge density <= baseline x 1.2.
- Measured from framebuffer (8-bit readback; HDR series available as
  cross-evidence, FVD pattern).
- **BASELINE RE-REGISTERED (owner, 2026-10-01).** The 0.26-pre baseline
  (contrast 1.857549 / variance 0.002933922 / edge 0.000654) was captured on
  an integrity-BROKEN scene: the material table was indexed by mesh id while
  the scene wrote it by instance index, so every surface carried its
  neighbour's material and the last mesh read an unwritten (black) slot.
  Those numbers are WITHDRAWN - a target computed against them would reward
  the defect, not the fix.
  **Authoritative baseline (post-D1, integrity-verified):**
  `contrast=1.849952 | variance=0.003122088 | edge=0.000652 | det=1`
  Scene: `demo/gpu_smoke/main_026_house.gd`, LIGHT_HOUSE instrument sun,
  EDGE_T = 0.1, 1920x1080. Gates become contrast >= 2.034947,
  variance >= 0.003590401, edge <= 0.000782.
- **Rule (Lesson 10): verify scene integrity BEFORE capturing a metric
  baseline.** Integrity check = full-frame colour census in which every
  declared part carries a colour equal to its own albedo x k
  (`tools/scene_defect_probe.gd`).

### 11.3 Existing APIs Audit (measured 2026-10-01 - full bind list read)
- Scene/geometry: gpu_scene_create/dispatch, set_instance_transform/mesh,
  gpu_mesh_create, gpu_mesh_create_from_arrays (PROCEDURAL path exists -
  the house can be built with zero external assets), gpu_meshlet_load[_path].
- Shading: gpu_material_create/set_albedo/set_specular/set_params/set_maps/
  set_emissive, gpu_light_create/update/set_intensity (point+spot+cone),
  gpu_texture_load/bind.
- Readback: gpu_raster_read_pixels (8-bit), _hdr, _normal[_all], _depth,
  _viewz[_all] - the metric tool needs NO new API.
- Shadows: gpu_shadow_map_create/render_maps/light_bind, ESM flag; CSM only -
  no screen-space contact-shadow path exists.
- Post: NO bloom / tone-map / fullscreen-pass API exists. Finding: 0.26b
  bloom+tonemap canNOT ride existing shaders without new C++ unless done
  CPU-side (measurement-only, slow) - new-API justification is REQUIRED,
  not optional, for the visual half of 0.26b. The audit therefore PARTIALLY
  REFUTES the "post-process via existing shaders if possible" hope for
  bloom/tonemap (sky-as-geometry and CSM contact darkening remain possible
  without new APIs).
- Verdict: NO new APIs needed for 0.26-pre (scene + metric) and 0.26a (fix);
  0.26b needs a justified new-API proposal (Architect).

### 11.4 Phase Split (binding order)
- 0.26-pre: Scene + metric + audit. NOTHING else starts before its approval.
- 0.26a: Scene fix.
- 0.26b: Visual (needs the new-API justification from �11.3 first).
- 0.26c: KI-019.
- D11-1..D11-5 status: OPEN (recorded, not decided - no Architect answers yet).
