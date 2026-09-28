# Contract 016 — Buffers

**GNE-016 — DRAFT.** All sizes exact; any deviation ⇒ build/test FAIL.

| Buffer | Size | Contents |
|---|---|---|
| `material_buffer` | 4096 B (64 × 64) | `GneMaterial` records (§data) |
| (existing, untouched) `mesh_color_buffer` | 1024 B (64 × 16) | flat colors — 016 never writes here |

- 64 slots max (`GNE_MESH_TABLE_SIZE` parity); slot ≥ 64 ⇒ `print_error` + reject.
- New `*_mat_*` pipelines/uniform-sets bind `material_buffer`; legacy sets are never rebound to it.
- CPU mirror `GneMaterial[64]`; every write = `buffer_update` + mirror; readback (criterion 1) compares GPU bytes to mirror.
- No allocation during a frame; growth policy: none in 016 (fixed 64, same as mesh table).
