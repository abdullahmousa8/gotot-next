# Contract 017 — Lifecycle

**GNE-017 — DRAFT.** Legal call order; any other order ⇒ `print_error` guard.

## States

`absent → baked (offline tool) → loaded (gpu_texture_load) → bound (gpu_texture_bind) → sampled (mat pass) → destroyed with mesh teardown`.

- `bind` before `load` ⇒ error + no-op.
- `load` twice without destroy ⇒ error (non-destructive guard, 014-lifecycle precedent).
- `sample` with unbound slot ⇒ flat channel + `tex_oob` (contract_017_bindless), never a stale texture.
- Safe to call when unallocated (guards per-RID `is_valid()`).

## Dispatch (bake once, sample deterministically)

- Baking is content-deterministic: same PNG + preset ⇒ byte-identical `.gtex` (verified by hash in Phase 1).
- Sampling is frame-deterministic: same texture + same UV ⇒ same texel (no LOD bias drift; trilinear only).
- `main_017.gd` order: load → bind → verify readback → dispatch ×2 (DET) → pixel evidence → destroy.

## Sequence invariant (017 harness)

`gt_017a`: run1 (d1) then run2 (d2); both print `GNE 017: PASS` and identical `sig=`; any `ERROR:` ⇒ FAIL.
