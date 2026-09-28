# Contract 018 — Light Buffer

**GNE-018 — FINAL (D6-3/D6-4 resolved: 64 B record, offset+count sorted list).**

## Light store (D6-3 FINAL)

- 4×vec4 = **64 B** per light: `[pos.xyz, range] [color.rgb, intensity] [dir.xyz, type] [cone_inner, cone_outer, pad, pad]`.
- Cap **1024** lights = **65,536 B**, bounded and counter-guarded.
- CPU mirror + `buffer_update` per write; readback verifies bytes (014/016 precedent).

## Cluster index (D6-4 FINAL)

- Flat `uint` index list + per-cluster `(offset, count)` (011 prefix-sum precedent).
- Light ids appended in SORTED order per cluster (DET stability — unsorted
  atomics would make ordering nondeterministic).
- Overflow: counter increments + `print_error`-class loud signal (no silent drop).

## Rules

- Buffer sizes are exact constants; overrun writes rejected before upload.
- Destroyed slots zeroed (no stale ids sampled by any cluster).
- `gpu_light_get_stats()` reports `{count, bytes, clusters_touched, overflows}` —
  independent dict, never inside DET-feeding dicts.
