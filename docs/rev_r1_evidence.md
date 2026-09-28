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

## Battery state

Full sweep on the final binary: 9/9 green - gt_regress, gt_011, gt_015a, gt_015_5_phase4,
gt_016a, gt_017a, gt_018a, gt_019a, gt_018a_rev (all rc=0; legacy literals v18/v19
byte-intact).

## Decisions (executor, under user authorization 2026-09-28)

Both open items were decided under explicit user delegation ("choose the safest
decision") with this principle: never move a threshold to make a test pass, never
hide a measured defect, keep every gap falsifiable and tracked.

1. **dc / R2 - measured, not redefined:** dc=13 assignments (~0.11% of the corrected
   base). The provisional >=10% target is NOT met on this scene and is NOT claimed;
   per D8-rev-5 "measured-at" the measured value is recorded and pinned byte-exact
   by the signature (dc=13 inside `v18-rev|...`). The quantitative >=10% scale
   target is deferred - still open - to the dense-light scenario (benchmark-city
   work item); the criterion is not redefined to pass here.
2. **ot=9 - accepted, loud, tracked:** the KI-014 fix expands cluster lists, pushing
   9 clusters over the 16 cap on this scene; the overflow count stays visible in
   the rev signature (ot=9) and the OFF path remains ot=0. Overflow semantics / cap
   policy re-examination is recorded in KI-014's R1 closure addendum.
