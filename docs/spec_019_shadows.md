# GNE-019 — Shadows + Real Depth HZB (SPEC v1.0)

**Status:** DRAFT v1.0.
**Depends:** 018 (Lighting).
**Closes:** KI-007 (Real Depth HZB).
**Unblocks:** 020 (GI).

## 1. Objective

- Shadow maps (directional + point + spot).
- Real depth HZB feed (KI-007 closure).
- GPU-driven shadow culling.

## 2. Background

- 018: clustered forward+ lighting.
- KI-007: HZB uses AABB occluders, not scene depth.
- 004: VSM prototype (deferred).

## 3. Design

### 3.1 Shadow Types
- Directional: CSM (cascaded shadow maps).
- Point: cube shadow map (6 faces).
- Spot: single shadow map.

### 3.2 Shadow Resolution
- Directional: 4 cascades × 2048×2048.
- Point: 1024×1024 per face.
- Spot: 2048×2048.

### 3.3 Real Depth HZB (KI-007)
- Activate `gpu_hzb_depth_source`.
- Feed from `raster_viewz_texture` (009).
- Deprecate AABB occluder path (fallback only).

### 3.4 Shadow Culling
- Compute pass.
- Per-light: cull casters.
- GPU-driven.

### 3.5 Shadow Bias
- Constant + slope-scaled.
- Normal offset.

## 4. Acceptance Criteria (8)
1. Directional shadow visible.
2. Point shadow visible.
3. Spot shadow visible.
4. CSM transitions smooth.
5. KI-007 closed (real depth).
6. Histogram evidence.
7. DET stable (v19|...).
8. Regressions 001A-018 PASS.

## 5. APIs
- gpu_shadow_map_create(type, resolution).
- gpu_shadow_light_bind(light_id, shadow_id).
- gpu_shadow_cull_dispatch().
- gpu_shadow_get_stats().

## 6. Out of Scope
- GI (020+).
- Ray-traced shadows.
- VSM (deferred).
- Volumetric shadows.

## 7. Deferred
- VSM.
- RT shadows.
- Volumetric.

## 8. Deliverables
- C++ module.
- Demo (main_019).
- Harness (gt_019a).
- Contract docs ×5.

## 9. D7 Decisions Required
- D7-1..D7-9.

## 10. Risks
- Cascades pop-in.
- Bias tuning.
- Real depth HZB (KI-007) regression.
- Performance.
