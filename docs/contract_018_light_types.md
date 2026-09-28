# Contract 018 — Light Types

**GNE-018 — FINAL (D6-1 resolved: Point + Spot + Directional, max 1024).**

## Types (D6-1 FINAL)

| Type | Parameters | Notes |
|---|---|---|
| Directional (016) | `L` (frozen), ambient | Unchanged, always evaluated |
| Point | pos.xyz, range, color.rgb, intensity | Omni falloff (D6-6 equation) |
| Spot | pos.xyz, range, dir.xyz, inner/outer cone, color.rgb, intensity | Cone falloff (D6-6 equation) |

## Rules

- Type set is closed in 018 (point + spot + inherited directional). Area/volumetric ⇒ rejected (019+/020+).
- `range <= 0` or negative intensity ⇒ `print_error` + reject (no silent clamp).
- Cone angles outside (0, π/2] ⇒ reject.
- A disabled/destroyed light contributes exactly zero (verified by toggling in tests).
