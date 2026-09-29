# Contract 016.5 — channel array bounds

**Status:** behaviour MEASURED. Bound `0..4` **ADOPTED as the official contract**
(owner decision, 2026-09-29). Dead `tia < 8` remains DEFERRED technical debt.
**Date:** 2026-09-29. **Method:** measurement (Y2 payload probe), not inference.

## The effective contract, as measured

Material channels sample array positions **0..4** out of a texture array that is
**8 wide**. Positions **5..7** are reachable, occupiable with real GPU resources,
and are then **never sampled**. The skip is **silent**: the frame is
byte-identical to the all-slots-unset baseline (`diff_px = 0`, `max_lsb = 0`).

## Two independent axes — do not conflate

| Constant | Value | Governs | Enforced at |
|---|---|---|---|
| `GNE_MAT_TEX_SLOTS` | 5 | the **material** tex slot (`p_slot` in `gpu_texture_bind`) | validated, `gne_render_server.cpp:7084` |
| `m2a.X <= 4.0` | 4 | the **array position** a channel samples (`tex_arr[tia]`) | shader guard, `cpp:1842 / 1849 / 1856` |

`gpu_texture_bind` derives the array position as `p_tex % GNE_TEX_ARRAY`
(`GNE_TEX_ARRAY = 8`, `cpp:7089`), so positions 0..7 are all reachable. Passing
`p_slot >= 5` is rejected outright — that is the *material slot* bound, and it is
not what limits channels.

`gpu_material_set_maps` validates **only** `p_mat` (`cpp:6831`); the slot integer
is cast to float verbatim. No clamp, no rejection, no per-channel check. So an
out-of-range channel value is accepted, stored, and silently ignored downstream.

## Measurement (probe Y2, on the pre-existing inst0 in main_016_5)

Each array position carried a solid hue no other position used, removing the
aliasing that made an earlier probe ambiguous. `pay4..pay7` were bound into
material slots 1..4, leaving material slot 0 (the legacy path's slot, read via
`tex_ids[mesh*5 + tex_slot_pad.x]`, `cpp:1814`) unbound. Baseline was captured
**after** binding with all m2 slots unset, so reference and measured draws shared
an identical pipeline state and the only variable was the m2 slot value.

Array occupancy was asserted (`tex_id % 8 == arr`, else `_fail(4052)`):

```
Y2 baseline      sig=r=255.0 g=228.7 b=226.7
Y2 slot=4  yellow   (255,255,0)  diff_px=81  max_lsb=206  sig b: 226.7 -> 25.0
Y2 slot=5  magenta  (255,0,255)  diff_px=0   max_lsb=0    sig unchanged
Y2 slot=6  cyan     (0,255,255)  diff_px=0   max_lsb=0    sig unchanged
Y2 slot=7  orange   (255,128,0)  diff_px=0   max_lsb=0    sig unchanged
```

Slot 4 shows a **signature match**, not merely "pixels differ": the blue channel
is crushed exactly as the yellow payload predicts. Slots 5..7 keep the baseline
signature byte-for-byte; had any been sampled, its own hue would have crushed a
different channel. This is what separates "excluded by the guard" from "nothing
is there".

## Dead logic

`tia < 8` / `tir < 8` / `tin < 8` are **unreachable-false**: the outer
`m2a.X >= 0.0 && m2a.X <= 4.0` already forces the index into {0..4}, so the inner
bound is always true when reached. Note this is the **opposite** of a guard that
permits 8 positions — it never admits five.

## Intent: undetermined (do not read intent into the above)

- No comment, constant, SPEC clause or RFC entry ties `4.0` to a decision.
- `docs/spec_016_materials.md` and `docs/spec_017_textures.md` were not
  established as the source of this bound.
- The dead `tia < 8` is *suggestive* of an 8-wide intent, but a never-reached
  guard is equally consistent with an author who changed the outer bound and left
  the inner one behind. It indicates a probability, not a decision.

## Adoption decision (owner, 2026-09-29)

The measured `0..4` bound is **adopted as the official contract** for the system
in its current stage. Rationale: those five positions are the measured,
byte-guaranteed ones; making 5..7 live would require shader changes, a record
re-layout and a performance bundle re-test, which is unearned risk absent an
explicit requirement for eight channels. Fixing the bound also prevents silent
access to the isolated positions and states the real processing limit for
developers.

**Scope of the decision:** this settles the *bound*, and nothing else. It does
not authorise a shader edit, and it does not resolve the dead `tia < 8`, which
stays registered as deferred technical debt for a future shader review. The
alternatives recorded below (widen to 8) remain reachable only via a new
requirement, and would then need their own measurement — the evidence here
describes the current code, not a claim about what the code ought to be.

## Scope

Covers the channel→array bound only. Does not cover: `p_tex >= 8` (where
`p_tex % 8` silently wraps), seam stitching, or per-material slot capacity
(`GNE_MAT_TEX_SLOTS`).

## Artifacts

The Y2 probe was a measurement instrument, not a permanent gate: it printed and
set no SLICE1 condition. It was reverted after the run, and its generated payload
`.gtex` files were removed. The standing `mat_v2_x1` gate is unaffected.
