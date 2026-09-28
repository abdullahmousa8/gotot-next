# Contract 019 — Boundaries

**GNE-019 — DRAFT.** What 019 owns, and what it must never touch.

## Owned by 019

- Shadow maps (CSM/cube/single) + shadow cull compute + real-depth HZB feed.
- `gpu_shadow_*` API (TEST-ONLY first).
- `main_019.gd|.tscn`, `gt_019a` harness, `v19|…` signature.
- Shadow bias (constant + slope-scaled + normal offset) tuning within 019.

## Forbidden (unchanged from Architect constraints)

- GI (020+), VSM (deferred), ray-traced / volumetric shadows.
- 016 material record (64 B frozen), 016/018 light model and intensities.
- 018 cluster geometry (16×9×24), buffers, cull shader, frag light loop.
- Flat-color paths, vertex format/stride (12 B), mesh table, textures path.
- `*_get_stats()` contents of every prior milestone (all feed DET readers).
- `RenderingServer`, RHI, engine patches — out of scope, no exceptions.
- Touching 001A–018 scenes, shaders, buffers, or signatures (additive only).

## Hand-off rule

- If a change alters even one legacy pixel or signature byte ⇒ STOP, rollback,
  report. 013–018 signatures stay literal, no exceptions.
