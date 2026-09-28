# Contract 019 — Real Depth HZB (KI-007 Closure)

**GNE-019 — DRAFT.** How 019 closes KI-007: the HZB pyramid is fed by real
scene depth instead of AABB occluders.

## Owned by 019 (D7-3/D7-4/D7-9 FINAL)

- Activating `gpu_hzb_depth_source`.
- Feeding the pyramid from `raster_viewz_texture` (009 output), R32_SFLOAT (D7-3).
- Keeping the AABB occluder path as fallback only, gated by flag
  `hzb_real_depth_enabled` (default true; D7-4).
- KI-007 closure evidence (D7-9): 012-rev gate green + ≥95% occlusion
  agreement real-depth vs AABB on the proof scene.
- KI-007 closure evidence: before/after occlusion agreement + DET.

## Forbidden

- Removing the AABB fallback before Architect sign-off.
- Changing the HZB format contract (`contract_012_hzb_format.md` stays literal).
- Feeding HZB from any source other than `raster_viewz_texture` without D7 approval.
- Regressing 012-rev behaviour: `v12-rev levels=10 p1=6 p2=0 ctrl=6` must stay byte-identical.

## Hand-off rule

- KI-007 is closed only when: real-depth HZB passes the 012-rev gate AND
  the 019 gate with zero legacy signature drift. Otherwise the fallback stays.
