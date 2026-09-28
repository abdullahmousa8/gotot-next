# Contract 017 — Boundaries

**GNE-017 — DRAFT.** What 017 owns, and what it must never touch.

## Owned by 017

- `.gtex` assets + `tools/tex_import/` (offline host tool, never part of the module build).
- `mat_tex` table (1280 B) + `gpu_texture_*` API + sampler-array set in the `*_mat_*` path.
- `main_017.gd|.tscn`, `gt_017a` harness, `v17|…` signature.

## Forbidden (unchanged from Architect constraints)

- Frozen 64 B material record — texture links live in `mat_tex`, never in the record.
- Flat-color paths, vertex format/stride (12 B), mesh table, `mesh_color_buffer`.
- `gpu_scene_manager_get_stats()` / `gpu_rg_get_stats()` / `gpu_material_stats()` — no texture keys inside (all feed DET readers).
- `RenderingServer`, RHI, engine patches — out of scope, no exceptions.
- Linking basisu/libktx into the module (offline-only rule; the runtime parses + uploads).

## Hand-off contracts with 016 / 010 / 011

- 016 evidence (centers, histograms, DET re-check) must reproduce **before and after** 017 with textures UNBOUND (flat path intact).
- Binding a texture must change 016-scene pixels in the predicted direction (albedo-modulated), never randomly.
- Any 010/011/013/014/015/015.5/012-rev pixel delta ⇒ 017 bug, rollback, no debate.

## Zero-readback rule (inherited)

- Readback allowed only in TEST-ONLY evidence getters, never in the hot dispatch path.
