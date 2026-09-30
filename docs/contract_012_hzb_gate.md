# Contract 012 — HZB Occlusion Gate

## Scope
- main_012_hzb scene + tools/gt_012hzb.bat (Tier 2 path A, scene-only).
- HZB occlusion validation on the production AABB-occluder path.

## Requirements
- cullpm = 1000 (fully-designed occlusion: 6/6 behind one nearer wall).
- p1 = 6 (frustum keeps all), p2 = 0 (HZB removes all).
- det = 1 (in-process rebuild+re-dispatch byte-equal AND d1/d2 identical).
- Temporal-OFF re-measure must agree (reported; the gate does not rank it).
- Negative control [6,6] with occluders off, before and after.
- No rebuild (provenance preserved); wall-clock evidence-only (KI-001).

## Signature
- v12hzb|p1=6|p2=0|cullpm=1000|det=1|d1 (integer-only, pinned in
  tools/verify_baseline.txt; CVS row 25).

## Path
- AABB occluders via gpu_hzb_set_occluders + gpu_hzb_build +
  gpu_visibility_prod_dispatch (current production path).
- Raster depth feed: NOT active (measured unreachable for phase-2, audit 012;
  KI-007 closed via 019 on its own path).

## Non-goals
- Not production HZB from raster depth.
- Not camera-motion validated (static camera + static geometry only).
- Not a performance gate (no GPU timers exist to gate on).
