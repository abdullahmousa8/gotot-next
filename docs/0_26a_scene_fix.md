# GNE-0.26a - Scene Fix Report (2026-10-01)

Scope: SPEC 0.26a. **No C++ changed, no lighting value changed, no ambient
change, provenance untouched.** Scene data only.

## Summary
One real defect found, and it was not the one the SPEC predicted: the whole
house was shaded with the WRONG MATERIALS (an off-by-one between the mesh id
the shader reads and the instance index the scene wrote). Two "suspected
defects" were reclassified with evidence rather than "fixed".

---

## D1 - Black roof apex: ROOT CAUSE FOUND (and it was a symptom)

**Root cause (named): MATERIAL INDEX OFF-BY-ONE, not roof geometry.**

Evidence chain:
1. Full-frame colour census of the pre-fix frame: the ground area (392,934 px -
   far larger than the 160x700 path quad could ever project to) carried the
   PATH's shaded colour. The path carried the BODY's colour. The chimney read
   `(6,6,6)`, i.e. black.
2. Ratio test: `path_colour / ground_albedo` was constant across R, G and B,
   so this was NOT a lighting or exposure effect.
3. Source: the fragment shader indexes the material SSBO by **mesh id**
   (`uint m = v_mesh_id * 4u`, gne_render_server.cpp:967), but the scene
   called `gpu_material_set_albedo(i, ...)` with the **instance index**, and
   `gpu_mesh_create_from_arrays` returns ids starting at **1**. Every part was
   therefore shaded with its neighbour's material, and the last mesh read a
   slot that is never written (black).
   The API documents the index as a "slot id", not an instance id - the scene
   misread it.

**Fix (scene data, 3 lines):** address the material table by `mid` (the mesh
id returned by `gpu_mesh_create_from_arrays`) instead of `i`.

**Evidence after the fix** (probes, server VP):
| part | before | after |
|---|---|---|
| ground | (175,152,105) = path albedo | **(58,117,47)** = own albedo x 0.91 |
| path | (199,175,140) = body albedo | **(175,152,105)** = own albedo x 0.91 |
| body | (71,16,13) = roof albedo | **(110,97,78)** = own albedo x 0.51 (vertical face, lower ndl) |
| chimney | (6,6,6) black | **(65,26,19)** = own albedo x 0.51 |
| back wall | (110,97,78)/(104,91,71) = neighbours | unchanged - genuinely occluded (see D2) |

The roof-apex "black" was the unwritten material slot, not a gap at the ridge.
A genuine 1-2 row seam at the ridge cannot be excluded from this evidence; it
is now, after the fix, indistinguishable from the background and needs its own
targeted probe before it can be called a defect.

**Negative control:** reverting the three lines restores the pre-fix numbers
exactly - contrast 1.857549, variance 0.002933922, edge 0.000654 - and
re-applying restores contrast 1.849952, variance 0.003122088, edge 0.000652.
The fix is what changed the pixels.

## D2 - Back wall: ACCEPTED, no fix (reclassified)
Already established in Task 1 and now corroborated: the wall (x -250..250,
z -460..-420) sits entirely behind the body and under a roof that is WIDER
(x -260..260), so from the fixed camera it has no visible silhouette. It is
drawn, culled by nothing, and hidden by nearer geometry - **geometric
occlusion by design**, not a bug. Owner decision: Option A (accept).

## D3 - Path: NOT A DEFECT (reclassified)
The path quad is at y=0.6 over ground at y=0.0 - not coplanar, so no z-fight is
possible. After the material fix the path carries its own albedo
(175,152,105 = x0.91). Closed.

---

## Metric delta (reported, NOT gated - per SPEC 0.26a criterion 4)

| metric | 0.26-pre baseline | after D1 fix | delta |
|---|---|---|---|
| contrast | 1.857549 | 1.849952 | -0.41% |
| variance | 0.002933922 | 0.003122088 | **+6.41%** |
| edge | 0.000654 | 0.000652 | -0.31% |
| determinism | det=1 | det=1 | - |
| exit | 0 | 0 | - |

Colour variance RISES 6.4% because nine distinct surface colours now appear
correctly instead of one colour being smeared across several parts. The
0.26b directional targets (contrast +10%, variance +15%, edge <=1.2x) are
therefore measured against a baseline that contained a real defect; the
baseline is re-measured here and the decision to re-baseline is the owner's.

## Provenance
No C++ edit in this unit; `module_digest` unchanged; the binary was not
relinked. `det=1` in-process preserved.

## Reusable output
`tools/scene_defect_probe.gd` + `docs/playbook_scene_defect_diagnosis.md` -
the general-purpose diagnosis kit and method, usable by any AI agent on any
scene.
---

# FINAL (2026-10-01, after owner approval)

## Closure status
| Item | State |
|---|---|
| D1 - black roof apex | **FIXED** - root cause = material indexed by mesh id while the scene wrote instance index (off-by-one). 3 lines of scene data. |
| D2 - back wall | **ACCEPTED** - geometric occlusion by design (owner Option A), no fix |
| D3 - path | **CLOSED** - not coplanar, z-fight impossible, carries its own albedo |
| Negative control (D1) | **PROVEN** - revert restores 1.857549/0.002933922/0.000654 exactly; fix gives 1.849952/0.003122088/0.000652 |
| Provenance | clean - no C++ touched in this unit, module digest unchanged, binary not relinked |
| Determinism | det=1, exit 0, byte-identical across two fresh processes |

## Baseline re-registration (owner decision 2026-10-01)
The 0.26-pre baseline was captured on an integrity-broken scene and is
**WITHDRAWN**:
`contrast 1.857549 / variance 0.002933922 / edge 0.000654`

**Authoritative baseline (integrity-verified, post-D1, stable d1/d2):**
`contrast 1.849952 | variance 0.003122088 | edge 0.000652 | det=1`

0.26b gates derived from it (SPEC 0.26 section 11.2 + contract_026_visual.md):
- contrast >= **2.034947** (x1.10)
- variance >= **0.003590401** (x1.15)
- edge <= **0.000782** (x1.20)

Lesson 10 registered: integrity census BEFORE baseline capture. The
integrity proof is `tools/scene_defect_probe.gd` - every declared part must
carry a colour equal to its own albedo x k.

## What 0.26a did NOT do
- No C++ change, no relink, no lighting change, no ambient change (ADR-001),
  no new API.
- A genuine 1-2 row seam at the roof ridge cannot be excluded from the
  current evidence and is NOT closed here; it needs a targeted probe and is
  left as an open item rather than claimed fixed.
