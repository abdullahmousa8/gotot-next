# Contract 018-rev - Boundaries

**GNE-018-rev - DRAFT.** What the revision owns, and what it must never touch.

## Owned by 018-rev

- Normal-cone reduction pass + cone buffer + back-face cull prefilter (flag-gated).
- `gpu_light_set_normal_cone(bool)` (default false).
- `main_018_rev.gd|.tscn`, `gt_018a_rev` harness, `v18-rev|...` signature.

## Forbidden

- Any change to the 018 default path while the flag is OFF - it must stay byte-identical.
- 016 material record (64 B frozen); light model / intensities of prior milestones.
- Cull-list order, overflow behavior, or append semantics of the 018 cull.
- `RenderingServer`, RHI, engine patches - out of scope, no exceptions.
- Touching 001A-018 scenes, shaders, buffers, or signatures (additive only).

## Hand-off rule

- If a change alters even one legacy pixel or signature byte => STOP, rollback, report.
- With the flag ON, the ONLY acceptable pixel delta on a static scene is zero;
  anything else returns to the Architect with numbers (no silent tolerance).