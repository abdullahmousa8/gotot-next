# Contract 015.6 — Boundaries

**GNE-015.6 — DRAFT.** What the sprint owns, and what it must never touch.

## Owned by 015.6

- known_issues.md KI-001/KI-002/C6 status updates.
- Measurement harnesses (wall-clock only, clearly labeled).
- progress_report §34.
- Documentation-only closures where measurement proves infeasible.

## Forbidden (unchanged from Architect constraints)

- 018 (Lighting) scope: no lights, no shadows, no new shading.
- New features of any kind (fix-only sprint).
- Godot engine patches.
- Any change to 001A–017 scenes, shaders, buffers, or signatures.
- `gpu_scene_manager_get_stats()` / `gpu_rg_get_stats()` / `gpu_material_stats()` contents.
- `RenderingServer`, RHI, engine patches — out of scope, no exceptions.

## Hand-off rule

- If a fix changes even one pixel or one signature byte ⇒ STOP, rollback,
  report. Measurement work must be behavior-preserving by construction.
