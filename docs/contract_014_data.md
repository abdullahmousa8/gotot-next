# Contract 014 — Data Semantics (GPU Scene Manager)

**GOTOT-014 — PASS (2026-09-23).** Additive TEST-ONLY evidence bridge in `modules/gotot_render`.

## Instance record (64 bytes / 16 floats / 4 vec4), unified ID space

`id` == record slot index in `gms_record_buffer` (SSBO AoS):

| vec4 slot | offset | field | layout |
|-----------|--------|-------|--------|
| rec[r+0]  | 0  | transform | vec4 (pos.xyz, scale) |
| rec[r+1]  | 16 | bounds    | vec4 (center.xyz, radius) |
| rec[r+2]  | 32 | refs      | vec4 (ordinal, mesh_ref, flags, pad) |
| rec[r+3]  | 48 | lodcfg    | vec4 (lod_t0 bits, lod_t1 bits, pad, pad) |

- `refs.z` doubles as the **active flag**: 1.0 for add path, -1.0 clears on remove.
- Ordinal/mesh/flags/op/id travel as **floats** (exact for values < 2^24); the GPU casts
  `uint()` on use. CPU `set_instances` uploads raw floats.
- `bounds` / `transform` are float32 world data; `lodcfg` holds the 013 LOD thresholds
  (2200 / 3200) ready for the meshlet path hand-off.

## Ring delta (80 bytes / 20 floats)

`[op, id, seq, flags, transform(vec4), bounds(vec4), refs(vec4), lodcfg(vec4)]`

- `op`: 1 = add (all fields), 2 = remove (clear active), 3 = move (transform+bounds only).
- `seq` reserved for ordering evidence.

## Draw record (32 bytes / 2 uvec4), snapshot per active instance (ascending id)

- `uvec4(ordinal, mesh_ref, flags, floatBitsToUint(lod_t0))` + `uvec4 = floatBitsToUint(bounds vec4)`.

## Counters (stats, uint[16])

0=active, 1=adds, 2=removes, 3=moves, 4=reserved, 5=ring bytes consumed,
6=snap records, 7=distinct meshes. Mesh histogram: `gms_mesh_count_buffer` (64 uint slots).