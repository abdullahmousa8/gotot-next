# GNE-019 — Shadows + Real Depth HZB (SPEC v1.0)

**Status:** FINAL v1.0 (D7 applied 2026-09-28). Ready for implementation.
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

### 3.1 Shadow Types (D7-1 FINAL)
- Directional: CSM (cascaded shadow maps).
- Point: cube shadow map (6 faces).
- Spot: single shadow map.
- Max 64 shadow-map bindings (D7-1). `sm` in the signature counts bound
  shadow-casting lights (demo: dir + 2 point + 2 spot = 5).

### 3.2 Shadow Resolution (D7-2 FINAL)
- Directional: 4 cascades × 2048×2048.
- Point: 1024×1024 per face.
- Spot: 2048×2048.
- CSM split: hybrid log/uniform, λ = 0.5 (D7-2).

### 3.3 Real Depth HZB (KI-007) (D7-3/D7-4 FINAL)
- Activate `gpu_hzb_depth_source`.
- Feed from `raster_viewz_texture` (009), format R32_SFLOAT (D7-3).
- Deprecate AABB occluder path (fallback only), gated by flag
  `hzb_real_depth_enabled` (default true once proven; D7-4).

### 3.4 Shadow Culling (D7-5 FINAL)
- Compute pass.
- Per-light: cull casters.
- GPU-driven.
- Caster grid with sorted ids; empty caster list on a bound shadow = FAIL
  (no silent fallback; D7-5).

### 3.5 Shadow Bias (D7-6 FINAL)
- Constant 0.001 + slope-scaled 0.005×tanθ.
- Normal offset 0.02 (world units).

## 4. Acceptance Criteria (8) — D7-8 FINAL thresholds
1. Directional shadow visible: dir-bound probe Δ ≥ 0.15 with CSM on vs off,
   changed pixels ≥ 500.
2. Point shadow visible: point-bound probe Δ ≥ 0.15, changed ≥ 500.
3. Spot shadow visible: spot-bound probe Δ ≥ 0.15, changed ≥ 500.
4. CSM transitions smooth: max cascade-seam step ≤ 0.10 brightness across
   the seam sample line.
5. KI-007 closed (real depth): `rd=1`, 012-rev gate green, occlusion agreement
   vs AABB path ≥ 95% on the proof scene (D7-9 evidence).
6. Histogram evidence: hr ≥ 0.3 on the canonical frame.
7. DET stable: `v19|lc|sm|cs|rd|hr|d` byte-identical d1/d2.
8. Regressions 001A–018 PASS (011 + 012 XFAIL stand).

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

## 9. D7 Decisions — FINAL (Architect, 2026-09-28)
- D7-1: CSM(4) + Cube(6×1024) + Spot(2048); max 64 bindings; `sm` = bound shadow lights.
- D7-2: CSM 4×2048², hybrid split λ=0.5.
- D7-3: Real depth from `raster_viewz_texture`, R32_SFLOAT.
- D7-4: AABB = fallback, flag `hzb_real_depth_enabled` (default true).
- D7-5: Caster grid, sorted ids, no silent fallback.
- D7-6: Bias 0.001 + 0.005×tanθ + normal offset 0.02.
- D7-7: sig = `v19|lc|sm|cs|rd|hr|d` (sm = shadow lights, cs = total casters,
  rd = real-depth flag, hr = histogram range, d = DET).
- D7-8: 8 thresholds (§4).
- D7-9: KI-007 closure evidence (012-rev green + ≥95% occlusion agreement).

## 10. Risks
- Cascades pop-in.
- Bias tuning.
- Real depth HZB (KI-007) regression.
- Performance.
