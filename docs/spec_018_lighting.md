# GNE-018 — Lighting Extension (SPEC v1.0)

**Status:** DRAFT v1.0.
**Depends:** 016 (Materials PASS), 017 (Textures PASS), 015.6 (KI Closure PASS).
**Unblocks:** 019 (Shadows + Real Depth HZB).

## 1. Objective

Extend 016 (single directional + ambient) to multiple lights:
- Point lights.
- Spot lights.
- Clustered Forward+ architecture.
- GPU-driven light culling.

## 2. Background

- 016: directional + ambient (PASS) — one frozen `L`, flat `mat` pass.
- 017: textures (PASS) — albedo carries detail; lighting model unchanged.
- 015.6: KI closure (PASS) — all numbers wall-clock from here on (KI-001 rule).
- Audit v1.0: Filament = BORROW (PBR equations research, §11/§43) — 018 may consult Filament lighting math, never import its renderer.

## 3. Design

### 3.1 Light Types (D6-1: final list at FINAL)
- Point lights (position + range, omni falloff).
- Spot lights (position + direction + inner/outer cone).
- Directional from 016 preserved verbatim (same `L`, same ambient).

### 3.2 Clustered Forward+ (D6-2: grid frozen at FINAL)
- Proposed grid: **16×9×24** = 3,456 clusters (1920/16 = 120, 1080/9 = 120 ⇒ 120×120 px tiles × 24 depth slices, exponential split).
- Cluster assignment is screen-space + view-depth: no CPU per-light work.

### 3.3 Light Buffer — SSBO (D6-3: layout frozen at FINAL)
- Proposed record 3×vec4 = 48 B: `[pos.xyz, range] [color.rgb, intensity] [dir.xyz, type/flags]`, + spot cone in a 4th vec4 if needed (64 B variant — decided at FINAL).
- Proposed cap: **1024 lights** (48–64 KB, bounded and counter-guarded).

### 3.4 Cluster-Light Index (D6-4: format frozen at FINAL)
- Proposed: per-cluster offset+count into a flat light-index list (built by compute, prefix-sum pattern from 011 precedent).
- Per-cluster light cap with loud overflow counter (no silent drop).

### 3.5 Culling Pass (D6-5: design frozen at FINAL)
- Compute pass: sphere/cone vs cluster frustum → append light id.
- Zero CPU intervention between upload and shading (014-waves barrier pattern).
- Deterministic ordering (sorted ids per cluster) for DET stability.

### 3.6 Shading Extension (D6-6: exact equations frozen at FINAL)
- Loop over the cluster's light list (uniform-bounded iterations, early-out at count).
- Accumulate: `diffuse + specular` per light with distance/cone attenuation × material response (016 model reused verbatim for the directional term).
- New lights add; directional + ambient + emissive paths unchanged.

## 4. Acceptance Criteria (8)
1. Point light visible (single point light changes pixels in the predicted region).
2. Spot light visible (cone falloff measurable: inside vs outside).
3. Multiple lights (>10) with stable frame (no overflow, counters exact).
4. Clustered Forward+ active (cluster buffer non-empty, index counts match light assignment).
5. GPU-driven light culling (a light affecting zero clusters contributes zero shading cost — measured, not claimed).
6. Histogram evidence (multi-light image differs from 016 single-light in the predicted direction).
7. DET stable (`v18|...` identical d1/d2).
8. Regressions 001A–017 PASS.

## 5. APIs
- gpu_light_create(params).
- gpu_light_update(id, params).
- gpu_light_destroy(id).
- gpu_light_get_stats().

All TEST-ONLY first; stats dict independent (never inside DET-feeding dicts).

## 6. Out of Scope
- Shadows (019).
- GI (020+).
- Area lights.
- Volumetric lighting.

## 7. Deferred
- Shadows.
- GI.
- Area lights.
- Volumetrics.

## 8. Deliverables
- C++ module (additive: light store + cull compute + shading extension).
- Demo (main_018).
- Harness (gt_018a, d1/d2 + sig compare pattern).
- Contract docs ×5.

## 9. D6 Decisions Required (unresolved — do not guess)
- D6-1: Light types (final list).
- D6-2: Cluster grid size.
- D6-3: Light buffer layout (48 B vs 64 B record).
- D6-4: Cluster index format (offset+count vs bitmask).
- D6-5: Culling pass design (sphere/cone tests, ordering).
- D6-6: Shading extension (exact per-light equations).
- D6-7: Signature format (`v18|…` fields).
- D6-8: Acceptance criteria (thresholds for 1–6 above).

## 10. Risks
- Cluster overflow (mitigated: cap + loud counter, D6-4).
- Performance regression (mitigated: culling must reduce shading work — measured per D6-5, wall-clock labeled per KI-001).
- Signature drift (mitigated: full gates; any byte change ⇒ STOP + rollback).
- Memory (light buffer bounded 48–64 KB + index list bounded; counters guard both).

**State:** DRAFT v1.0 — awaiting Architect review + D6-1..D6-8. No implementation, no build, no code commit before approval. (This file = documentation only.)
