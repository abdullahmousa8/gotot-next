# GNE-018 — Lighting Extension (SPEC v1.0)

**Status:** FINAL v1.0 — Architect approved (D6-1..D6-8 resolved).
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

### 3.1 Light Types (D6-1 FINAL)
- Point lights (position + range, omni falloff).
- Spot lights (position + direction + inner/outer cone).
- Directional from 016 preserved verbatim (same `L`, same ambient).
- Max **1024** lights (counter-guarded, no silent overflow).

### 3.2 Clustered Forward+ (D6-2 FINAL: fixed grid)
- Grid: **16×9×24** = 3,456 clusters (1920/16 = 120, 1080/9 = 120 ⇒ 120×120 px tiles × 24 exponential depth slices).
- Cluster assignment is screen-space + view-depth: no CPU per-light work.

### 3.3 Light Buffer — SSBO (D6-3 FINAL: 64 B record)
| vec4 | x | y | z | w |
|---|---|---|---|---|
| l+0 | pos.x | pos.y | pos.z | range (>0) |
| l+1 | color.r | color.g | color.b | intensity (≥0) |
| l+2 | dir.x | dir.y | dir.z | type (0=point, 1=spot) |
| l+3 | cone_inner | cone_outer | pad | pad |
- 1024 × 64 B = **65,536 B**, bounded and counter-guarded.
- `range ≤ 0`, negative intensity, cone outside (0, π/2], or id ≥ 1024 ⇒ `print_error` + reject.

### 3.4 Cluster-Light Index (D6-4 FINAL)
- Per-cluster `(offset, count)` into a flat light-index list (011 prefix-sum precedent).
- Light ids appended in **SORTED** order per cluster (deterministic regardless of GPU scheduling).
- Per-cluster light cap with loud overflow counter (no silent drop).

### 3.5 Culling Pass (D6-5 FINAL)
- Compute dispatch of **3,456 threads** (one per cluster): sphere test for points, sphere+cone test for spots, against the cluster frustum.
- Zero CPU intervention between upload and shading (014-waves barrier pattern: submit+sync per pass).
- Deterministic ordering (sorted ids per cluster) for DET stability.

### 3.6 Shading Extension (D6-6 FINAL)
- Loop over the cluster's light list (uniform-bounded iterations, early-out at count).
- Per light: **Lambert diffuse + Blinn-Phong specular** (016 equations reused verbatim for the directional term) × **attenuation** (smooth distance falloff to `range` + smooth cone falloff between inner/outer for spots).
- Accumulate: `diffuse + specular` per light with distance/cone attenuation × material response (016 model reused verbatim for the directional term).
- New lights add; directional + ambient + emissive paths unchanged.

## 4. Acceptance Criteria (8 — initial thresholds, frozen at FINAL after measurement)
1. Point light visible: ON vs OFF ⇒ changed pixels > 1000 AND probe brightens > 0.1 (C4 pattern).
2. Spot light visible: inside-cone vs outside-cone mean ratio > 2 (measurable falloff).
3. Multiple lights (>10): 16 lights stable, counters exact (16/16 assigned), no overflow, exit 0.
4. Clustered Forward+ active: non-empty clusters > 0 AND total assignments match the CPU-predicted count for the test layout.
5. GPU-driven light culling: a light affecting zero clusters contributes zero shading (pixels identical with/without it).
6. Histogram evidence: multi-light image differs from the 016-baseline reproduction; hr ≥ 0.3 (D4-3 precedent).
7. DET stable (`v18|lc|cc|ot|hr|d` identical d1/d2).
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

## 9. D6 Decisions — resolved (Architect, 2026-09-28)
| # | Decision | Value |
|---|---|---|
| D6-1 | Light types | Point + Spot + Directional (max 1024) |
| D6-2 | Cluster grid | 16×9×24 = 3,456 clusters (fixed) |
| D6-3 | Buffer record | 64 B (4×vec4) with cone row |
| D6-4 | Index format | Offset+count + flat SORTED list |
| D6-5 | Cull pass | Compute, 3456 threads, sphere/cone tests |
| D6-6 | Shading | Lambert + Blinn-Phong + attenuation |
| D6-7 | Signature | `v18\|lc\|cc\|ot\|hr\|d` |
| D6-8 | Thresholds | §4 initial values above (frozen at FINAL after measurement) |

## 10. Risks
- Cluster overflow (mitigated: cap + loud counter, D6-4).
- Performance regression (mitigated: culling must reduce shading work — measured per D6-5, wall-clock labeled per KI-001).
- Signature drift (mitigated: full gates; any byte change ⇒ STOP + rollback).
- Memory (light buffer 64 KB + index list bounded; counters guard both).

**State:** FINAL v1.0 — Architect approved (D6-1..D6-8). Ready for implementation.
