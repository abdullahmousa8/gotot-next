# RFC 016.5 - Material System v2 (texture-driven channels + proper triplanar) - [DRAFT v0.1, 2026-09-29]

**Track:** ARCHITECTURE (material contract extension). Per GNE v2.0: RFC -> prototype ->
benchmark -> decision -> implementation -> regression. Target: begin implementation on
presentation (no approval ceremony; ambiguities listed inline).

## Context (as-built)
- 016 materials: per-mesh record = 4 vec4 (albedo+roughness, emissive+metal,
  spec+shiny, flags). API: set_albedo/params/specular/emissive.
- 017 textures: one ACTIVE texture per draw selected via `push.tex_slot_pad.x` over
  a 5-slot table (`mat_tex_buffer`: tex_ids[mesh*5 + slot]); sampling is a fixed
  box-projection triplanar (`fract(vec2(world.z, world.y) * 0.01)`) with no
  per-material scale, no normal-aware blending, and albedo-only usage.

## Problem
Visual richness is capped: no per-material UV scale, no normal mapping, one
texture slot at a time, crude triplanar. This is the next architecture step for
the material subsystem (Track-1 scope example: "Material System overhaul").

## Proposal (bounded, backward compatible by construction)
1. **Channel map records (new buffer, set 0 binding 7 free):** per mesh:
   `{ albedo_slot, rough_slot, normal_slot, pad }` (tex table slot ids; -1 =
   unset) + `uv_scale` (vec2, world-units -> texture repeats; default matches the
   legacy 0.01 by setting scale = 0.01 when acting in legacy mode).
2. **Proper triplanar:** axis weights from |N| (smooth blend), per-material
   `uv_scale`, applied for all map fetches.
3. **Normal mapping (tangent-free):** perturb N from the normal map's derivative
   swizzled per dominant axis; measurable via `gpu_raster_read_normal`.
4. **Legacy invariant (byte-identity):** when all channel slots are unset AND the
   legacy tex_slot_pad path is inactive, the fragment executes the EXACT current
   math (no new fetches, same instructions) -> all historical literals remain
   byte-exact (CVS enforces). The new path is per-material opt-in; no global flag.

## Acceptance (pre-registered)
- M1: legacy literals byte-identical (full CVS + goldens).
- M2: textured scene `main_016_5`: albedo map visible; measured window mean shifts
  vs unset baseline by >= 0.05 in the mapped region; per-material scale changes
  the repeat count (window count of texture-space cycles >= 2x at scale x2).
- M3: normal map affects lighting: `gpu_raster_read_normal` delta >= 0.05 in the
  mapped region; lit-pixel luminance delta measured and reported.
- M4: determinism: in-process repeats byte-equal (readback + golden PNG).
- M5: perf: draw p50 delta <= +0.15 ms (city-class scene, 1080p) - report
  measured; fragment fetch budget: <= 3 map fetches + existing 1.
- M6: CVS 12/12 + goldens (or the deliberate golden update procedure if M2/M3
  scenes are added to the diff set).

## Invariants / non-scope
- Set-0 binding layout extended only at binding 7 (1-6 unchanged); material record
  format unchanged (channel records live in a NEW buffer).
- Not in scope: PBR energy rework, IBL, layered materials, anisotropic specular,
  texture streaming.
- Ambiguities to resolve during implementation (noted, not blockers): mat_tex
  table growth path if >5 slots needed later; normal-map Y convention (chosen by
  test: derivative sign verified on a known ramp); `tex_slot_pad` migration plan
  (deprecated but kept for legacy scenes).

## Immediate first steps (implementation begins on presentation)
1. Buffer + binding plumbing (`gne_mat2`), API: `gpu_material_set_maps(mat,
   albedo_slot, rough_slot, normal_slot, scale)`.
2. Fragment integration for the three channels (with the legacy gate).
3. Scene `main_016_5` + M2/M3 instruments; then M1/M5 runs; CVS.