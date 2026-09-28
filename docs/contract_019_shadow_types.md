# Contract 019 — Shadow Types

**GNE-019 — DRAFT.** The three shadow-map types 019 owns, with exact
resolutions. No other type may be added without an Architect decision.

## Owned by 019 (D7-1/D7-2 FINAL)

- Directional: CSM, 4 cascades × 2048×2048, hybrid split λ=0.5.
- Point: cube shadow map, 1024×1024 per face (6 faces).
- Spot: single shadow map, 2048×2048.
- Max 64 shadow-map bindings (D7-1).
- `gpu_shadow_map_create(type, resolution)` + `gpu_shadow_light_bind(light_id, shadow_id)` (TEST-ONLY first).

## Forbidden

- VSM (deferred to a later milestone, 004 prototype stays untouched).
- Ray-traced shadows, volumetric shadows (out of scope, SPEC §6).
- Changing cascade/face counts or resolutions without D7 approval —
  they feed memory budgets and the `v19|…` signature.

## Hand-off rule

- Shadow lookup in the 018 frag must be additive: with zero shadows bound,
  019 pixels must reproduce 018 pixels exactly (same rule as 018/016).
