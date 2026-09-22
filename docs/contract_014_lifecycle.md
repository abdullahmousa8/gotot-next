# Contract 014 — Lifecycle

**GOTOT-014 — PASS (2026-09-23).** Manager lifecycle mirrors `gpu_scene_create/dispatch/destroy`.

## States

- **Unallocated** — no buffers exist; every API returns `false` / empty with a
  `print_error` guard.
- **Allocated (`gpu_scene_manager_alloc(max)`)** — creates all 7 buffers, compiles the
  shader SPIR-V, creates pipeline + uniform set. Rejects `max <= 0` or `max > GMS_MAX_CAPACITY`
  (4,000,000) **without** tearing down an existing manager (non-destructive guard).
- **Ready** — `set_instances` (direct upload of a full record set, resets whole space),
  `update` (ring deltas), then `dispatch`.

## Dispatch (one call = deterministic frame of the scene DB)

1. Clear stats words 1..8, active counter, mesh histogram (+ submit/sync).
2. **Apply** (if deltas): consume ring, atomic counters, reset `tail` → 0.
3. **Compact**: scan all records, `atomicAdd` the active counter, write id list.
4. Read back the 4-byte active count → stats[0] (evidence bridge, not critical path).
5. **Snapshot**: emit draw records + mesh histogram from the compacted list.

## Teardown

- `gpu_scene_manager_destroy()` → `_destroy_scene_manager()`: frees uniform set, pipeline,
  shader, all 7 buffers; resets validity/capacity/tail/seq. Also called from module
  `shutdown()`. Safe to call when unallocated (guards per-RID `is_valid()`).
- Manager never touches the 001B scene, 008A/010/011 mesh batch path, 009 depth buffer,
  or 013 meshlet pipeline — it is a sibling evidence bridge.

## Sequence invariant (014 harness)

DET: repeat `dispatch` yields identical snapshot ordinals + identical stats; two full
harness runs yield byte-identical `sig=v14|...|d1`.