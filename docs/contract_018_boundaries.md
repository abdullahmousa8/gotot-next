# Contract 018 — Boundaries

**GNE-018 — DRAFT.** What 018 owns, and what it must never touch.

## Owned by 018

- Light store + cluster buffers + cull compute + shading extension.
- `gpu_light_*` API (TEST-ONLY first).
- `main_018.gd|.tscn`, `gt_018a` harness, `v18|…` signature.

## Forbidden (unchanged from Architect constraints)

- Shadows (019), GI (020+), area/volumetric lights.
- 016 material record (64 B frozen), 016 light model for the directional term.
- Flat-color paths, vertex format/stride (12 B), mesh table, textures path.
- `gpu_scene_manager_get_stats()` / `gpu_rg_get_stats()` / `gpu_material_stats()` /
  `gpu_texture_get_stats()` contents (all feed DET readers).
- `RenderingServer`, RHI, engine patches — out of scope, no exceptions.
- Touching 001A–017 scenes, shaders, buffers, or signatures (additive only).

## Hand-off rule

- If a change alters even one legacy pixel or signature byte ⇒ STOP, rollback,
  report. The directional+ambient baseline must reproduce 016 pixels exactly
  with zero extra lights bound.
