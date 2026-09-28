# Contract 018 — Light Buffer

**GNE-018 — DRAFT.** SSBO layouts (frozen at FINAL, D6-3/D6-4).

## Light store (proposed)

- 3×vec4 = 48 B per light (64 B variant with spot cone row — decided at FINAL).
- Proposed cap: **1024 lights** (≤64 KB, bounded).
- CPU mirror + `buffer_update` per write; readback verifies bytes (014/016 precedent).

## Cluster index (proposed)

- Flat `uint` index list + per-cluster `(offset, count)` (011 prefix-sum precedent).
- Light ids appended in SORTED order per cluster (DET stability — unsorted
  atomics would make ordering nondeterministic).
- Overflow: counter increments + `print_error`-class loud signal (no silent drop).

## Rules

- Buffer sizes are exact constants; overrun writes rejected before upload.
- Destroyed slots zeroed (no stale ids sampled by any cluster).
- `gpu_light_get_stats()` reports `{count, bytes, clusters_touched, overflows}` —
  independent dict, never inside DET-feeding dicts.
