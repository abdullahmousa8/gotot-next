# Contract 019 — Shadow Culling

**GNE-019 — DRAFT.** GPU-driven caster culling per light, in a compute pass.
Mirrors the 018 cluster-cull discipline (waves, barriers, no RMW races).

## Owned by 019 (D7-5 FINAL)

- `gpu_shadow_cull_dispatch()` compute pass.
- Per-light caster lists (directional per-cascade, point per-face, spot single).
- Caster grid with sorted ids; a bound shadow with an empty caster list = FAIL,
  never a silent skip (D7-5).
- `gpu_shadow_get_stats()` (counts, per-light caster totals, overflows).
- Overflow policy: saturate + count (same as 018 clusters — never wrap, never crash).

## Forbidden

- CPU per-caster loops in the hot path (GPU-driven only).
- Read-modify-write counter patterns of the 014 kind (waves + `submit()+sync()`
  barriers; Lesson: 014 race).
- Culling casters on the raster timeline (compute pass only, before draw).

## Hand-off rule

- Caster lists must be deterministic across d1/d2 (sorted ids, stable order).
  Any nondeterminism ⇒ STOP, same bar as 014/018 DET.
