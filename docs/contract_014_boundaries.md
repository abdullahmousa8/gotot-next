# Contract 014 — Boundaries

**GOTOT-014 — PASS (2026-09-23).** Scope of the additive TEST-ONLY manager, and what it must NOT touch.

## Owned by 014

- `gpu_scene_manager_*` APIs: `alloc / set_instances / update / dispatch / get_stats /
  get_draw_counts / get_snapshot / get_active_ids / destroy`.
- `_destroy_scene_manager()` (module shutdown hook).
- Files: `modules/gotot_render/gotot_render_server.{h,cpp}`, `demo/gpu_smoke/main_014.{gd,tscn}`
  and this contract set.

## Forbidden (unchanged from Architect constraints)

- 001B scene (`gpu_scene_create/dispatch/destroy`), 005/007A billboard path,
  008A/008B/009/010/011 mesh batch/depth path, 013 meshlet pipeline — do NOT modify.
- 012 (deferred), `project.godot`, protected specs.
- No engine RHI changes; local `RenderingDevice` only.

## Hand-off contracts with 013 / 011

- **013:** snapshot draw-record ordinals must lie inside the 013 meshlet ordinal ranges
  `[b_l, b_l + mc_l)` (l = the instance's LOD), with per-LOD bases `b_0 = 3`,
  `b_l = 3 + Σ_{k<l} mc_k`. Verified per-instance in the harness; full 013 cull+raster
  signature must be reproduced while the manager is alive (`fnv=3106528256`).
- **011:** managed-set grouping = `min(distinct_meshes, 5)` ≤ 5 draw calls (verified: 4096
  instances / 8 mesh refs → 5).

## Zero-readback rule

The critical path never reads GPU back; only the tiny 4-byte active counter and the
verification getters (`get_stats/get_draw_counts/get_snapshot/get_active_ids`) are read
as evidence bridges — same class as the 013 evidence getters.