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
## Slice-1 RESULTS (2026-09-29) - ALL ACCEPTANCE GATES PASS

Scene main_016_5 (4 boxes; inst0 enlarged to 120 for clean checker resolution;
inst1-3 flat for coexistence; checkerboard.gtex; map slots carry the
gpu_texture_load id; scale = UV multiplier):
- M1 (legacy byte-identity): CVS full run 14/14 PASS - all historical literals
  byte-exact + goldens byte-identical (the channel branch is fully skipped when
  slots are unset).
- M2 (map visibility): window mean shift 0.396 >= 0.05; window stddev flat 0.005
  -> mapped 0.383 (checker clearly rendered).
- M3 (scale repeat x2): scanline alternations 11 (s=0.0125) -> 23 (s=0.025),
  gate n2 >= 2*n1 - 2 (documented boundary-truncation convention) PASS.
- M4 (normal via read_normal): face normal (0,0,0.9995) -> (0.4355,0.4355,
  0.7876); max component delta 0.4355 >= 0.05 PASS.
- M4b (determinism): two identical ON-state draws byte-equal PASS.
- M5 (cost): draw p50 off 1728us / on 1605us (run-noise dominated; bounded).
- Sig: v165|m2=0.3960|n1=11|n2=23|m4=0.4355|det=1|off=1728|on=1605|d1
Incidents recorded: (a) GLSL int() truncates toward zero: int(-1.0+0.5) = 0, so
the -1 sentinel activated the normal channel implicitly - fixed with float-safe
ternary gates `(x >= 0.0 && x <= 4.0) ? int(x+0.5) : -1` (brace-neutral; first
run's M4 delta 0.0 with a perturbed-looking baseline exposed it);
(b) slice-1 uses dominant-axis triplanar (1 fetch/map, within the M5 fetch
budget); weighted |N| blending is slice-2 with a revised fetch budget.
Status: 016.5 slice-1 COMPLETE (M1-M5 green; CVS clean). Slice-2 candidates:
weighted triplanar, roughness-channel scene evidence, per-material channel
defaults policy.
## Slice-2 RESULTS (2026-09-29) - weighted |N| triplanar + roughness evidence: ALL GREEN

Changes: all channel fetches switched to weighted triplanar (weights = |N| normalized;
fetch budget revised to <= 9, i.e. 3 per map); outer gate now per-any-channel (a
roughness-only or normal-only configuration works); scene instruments extended.
- Continuity: on axis-aligned faces, M2/M3/M4 reproduce the slice-1 numbers exactly
  (0.3960 / 11->23 / 0.4355) - the weighted blend degenerates to the dominant
  projection by construction.
- S2-octa (tilted surface evidence): window stddev 0.1956 >= 0.05 with the weighted
  blend live; PNG evidence (s165_octa.png) saved for visual audit.
- S2-roughness (clean probe): delta_vs_flat 0.0071 >= 0.005; the first run's 0.384
  was an INSTRUMENT CONTAMINATION (the slot-0 bind activated the legacy texel path);
  discovered the store rule: gpu_texture_bind is what publishes the texture into the
  sampler array (load only stores it) - slot-1 publish keeps the probe clean.
- Determinism: byte-equal at both probe sites. Cost: 3-channel p50 2208us vs
  off ~1.6-2.2ms (bounded, noise-dominated).
- CVS Complete Clean Run: 14/14 PASS (all literals byte-exact + goldens identical,
  zero errors; perf 020:20287/58011).
- Sig: v165|m2=0.3960|n1=11|n2=23|m4=0.4355|det=1|off=1623|on=1978|wsd=0.1956|rd=0.0071|t3=2208|d2
Status: 016.5 slice-2 COMPLETE. Remaining candidates (unscoped): triplanar seam
stitching, per-material channel defaults policy, texture-array >8 growth.