# Contract 016 — Lifecycle

**GNE-016 — DRAFT.** Legal call order; any other order ⇒ `print_error` guard.

## States

`absent → allocated (gpu_material_create) → written (set_*) → dispatched (mat pass) → readback → destroyed with mesh table teardown`.

- `set_*` before `create` ⇒ error + no-op.
- Dispatch before any write ⇒ error (no default-material silent path).
- Non-destructive guard: `create` twice without teardown ⇒ error (mirrors 014 lifecycle rule).
- Safe to call when unallocated (guards per-RID `is_valid()`).

## Dispatch (one call = deterministic frame of the material pass)

- `L` frozen before dispatch; changing `L` mid-frame ⇒ error.
- The mat pass runs in its own synced submission (`submit()+sync()`, 014-waves pattern) — never interleaved inside another pass.
- `main_016.gd` order: create → 6 writes → readback-verify → dispatch ×2 (DET) → pixel evidence → destroy.

## Sequence invariant (016 harness)

`gt_016a`: run1 (d1) then run2 (d2); both print `GNE 016: PASS` and identical `sig=`; any `ERROR:` ⇒ FAIL.
