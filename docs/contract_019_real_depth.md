# Contract 019 — Real Depth HZB (KI-007 Closure)

**GNE-019 — DRAFT.** How 019 closes KI-007: the HZB pyramid is fed by real
scene depth instead of AABB occluders.

## Owned by 019

- Activating `gpu_hzb_depth_source`.
- Feeding the pyramid from `raster_viewz_texture` (009 output).
- Keeping the AABB occluder path as fallback only (flag-guarded, default off
  once real depth is proven).
- KI-007 closure evidence: before/after occlusion agreement + DET.

## Forbidden

- Removing the AABB fallback before Architect sign-off.
- Changing the HZB format contract (`contract_012_hzb_format.md` stays literal).
- Feeding HZB from any source other than `raster_viewz_texture` without D7 approval.
- Regressing 012-rev behaviour: `v12-rev levels=10 p1=6 p2=0 ctrl=6` must stay byte-identical.

## Hand-off rule

- KI-007 is closed only when: real-depth HZB passes the 012-rev gate AND
  the 019 gate with zero legacy signature drift. Otherwise the fallback stays.
