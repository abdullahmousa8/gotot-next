# Contract 016 — Boundaries

**GNE-016 — DRAFT.** What 016 owns, and what it must never touch.

## Owned by 016

- `material_buffer` (3072 B) + CPU mirror + `gpu_material_*` API.
- `*_mat_*` pipelines, shaders, uniform sets (new names only).
- `main_016.gd|.tscn`, `gt_016a` harness, `v16|…` signature.

## Forbidden (unchanged from Architect constraints)

- Flat-color paths (cpp 420/510/853) — byte-identical pixels required.
- Vertex format/stride (12 B) — no normal/UV attributes (017 owns those).
- `mesh_color_buffer` contents — 016 never writes; 010/011 evidence depends on it.
- `gpu_scene_manager_get_stats()` / `gpu_rg_get_stats()` — no material keys inside (DET readers).
- `RenderingServer`, RHI, engine patches — out of scope, no exceptions.

## Hand-off contracts with 010 / 011 / 013

- 010/011 pixel evidence (centers, histograms, DET re-check) must reproduce **before and after** 016 on the same binary.
- 013 meshlet path untouched (no shared buffers with 016).
- Any 010/011 pixel delta ⇒ 016 bug, rollback, no debate.

## Zero-readback rule (inherited)

- Readback allowed only in TEST-ONLY evidence getters (`readback`, stats), never in the hot dispatch path (014-lifecycle precedent).
