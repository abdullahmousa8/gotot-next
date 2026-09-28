# GNE-018-rev R1 Evidence (unit closure)

**Date:** 2026-09-28. **Gate:** `tools/gt_018a_rev.bat` (standalone, two runs).

**Canonical signature (d1 == d2, byte-identical):**
`v18-rev|lc=20|cc=3091|dc=13|ot=9|dp=2558|d1`

**Scene:** `demo/gpu_smoke/main_018_rev` (parity with main_018: 4 cubes + 20 lights +
camera; the OFF numbers reproduce the frozen v18 evidence exactly: touched=2841,
assignments=11348, overflows=0).

## Criterion results

- R1 (scoped): 2558 differing pixels, ALL inside cull-affected clusters (outside=0)
  and ALL carrying the KI-011 signature (unexplained=0) => no over-cull, no
  unexplained visual change. Corrected-base readings: off=11348, slab=12363,
  on=12350, dc=13.
- R7: cone source age = 1 at use (<= 1 required) - engine-exposed counter.
- R3 DET: d1 == d2 byte-identical sig.
- R5 hygiene: rc 0/0, zero ERROR lines, zero RID leaks (after closing this unit's
  teardown gaps for the new cone objects).

## Defects found and fixed during this unit (both caught by the R1 self-check)

1. **Back-face anchor over-cull:** the first 2c integration used the AABB min corner
   as the light-direction anchor (lab pattern); with our 120 px tiles that anchor
   sits up to ~330 world units off the true surface -> direction error ~25 degrees
   flipped verdicts for tight cones (measured: 9 lights culled on one cluster, 5 of
   them front-facing -> 2682 unexplained pixels). Fixed with `gne_backface_dot_box`
   (direction from the AABB point CLOSEST to the light - the most light-favorable
   anchor, strictly more conservative). After the fix: unexplained=0.
2. **Teardown gaps:** light_cone_{shader,pipeline,sampler,set} + selftest objects
   were not freed (KI-009 pattern); closed in _destroy_mesh.

## Open decision items (Architect)

- **dc on this scene = 13 assignments (~0.1% of the corrected base):** the SPEC's R2
  was provisional (>=10% initial, measured-at). The 20-light scene simply offers few
  back-facing clusters. Options: (a) re-scope the threshold to the measured reality,
  (b) add a dense-light stress sub-scene, (c) accept and document.
- **ot=9 on the ON path** (the slab fix pushes 9 clusters past the 16 cap on this
  scene; overflow is the loud by-design marker; the OFF path stays ot=0).