# 0.26a Task 1 - D11-9 Investigation: back wall root cause (2026-10-01)

## Question
Is the Country House back wall (a) an authoring duplicate, (b) a back-face /
front-face double-draw, or (c) a culling problem?

## Investigation (all measured, 2026-10-01)

### (a) Authoring duplicate? NO - exactly ONE wall part
`demo/gpu_smoke/main_026_house.gd:97`:
`[[["box", Vector3(-250, 0, -460), Vector3(250, 300, -420)]], Color(0.5,0.5,0.52), 0.9]`
- grep for the wall's unique coordinates (-460 / -420) in the scene file
  returns exactly ONE line (97). No duplicate part, no second instance.
- The scene asserts `gpu_cull_get_visible_count() == 9` every frame and
  passes, i.e. all 9 parts (including the wall) survive frustum culling and
  are submitted to the draw.
=> The wall IS drawn. Not missing, not duplicated.

### (b) Back-face / double draw? NO - no wall pixels at all
Full-frame triple census of the 0.26-pre unlit capture (every distinct
8-bit RGB triple, 2,073,600 px):
- (19,17,11) 392,934 | (14,3,2) 46,580 | (22,19,15) 45,924 | (20,18,14) 10,724
  | (139,30,25) 960 | (203,177,139) 960 | (13,5,4) 128 | (0,0,0) 1,575,390
The wall's own shaded value under this light is `0.1 x albedo =
(0.050, 0.050, 0.052)` -> byte ~(13,13,13). A search for ANY triple within
3 LSB of (13,13,13) returns **0 pixels**. There is no partial, doubled, or
back-face-only rendering of the wall - it contributes nothing to the frame.

### (c) Culling problem? NO - no culling control exists, and none is set
- grep for `cull_mode` / `front_face` / `CULL_` across the whole module
  returns no rasterization-state setting: every pipeline is created with a
  default-constructed `RD::PipelineRasterizationState` and no
  `cull_mode` assignment (e.g. the material pipeline at
  gne_render_server.cpp:7134-7141).
- Therefore back-face culling is whatever the Godot RD default is, applied
  uniformly to all 9 parts. A per-part culling flag does not exist, so
  "culling disabled on the wall" is not a representable state.

## Root cause (named)
**D2 = OCCLUSION, not culling and not authoring.**
Geometry: the wall spans x -250..250, y 0..300, z -460..-420. The body spans
x -210..210, y 0..260, z -270..70 and the roof slopes span x -260..260, y
250..380, z -290..90. The camera is fixed at (0,300,1500) looking down -Z.
Every part of the wall is therefore BEHIND the body+roof in view order, and
the roof is WIDER (x -260..260) than the wall (x -250..250), so the wall has
no silhouette that pokes out. The wall is fully occluded: drawn, in the
draw call, culled by nothing, hidden by nearer geometry.

Probe evidence (scene `--probe`, using the server's own VP so no hand-rolled
projection):
- `backwall`, `backwall_rimL`, `backwall_rimR` at z=-420 all read
  (71,16,13) = the ROOF's shaded colour, at both candidate y conventions.
- `backwall_top` reads (104,91,71) = the GABLE's shaded colour.
The wall's own colour appears nowhere.

## Classification for the fix
D2 is a SCENE-PLACEMENT defect (the wall is authored behind the house and is
invisible from the registered camera), NOT an engine defect. The fix must be
scene data only: move/rotate the wall so it is visible from the fixed camera
(or accept that a detached back wall is not visible from the front and fold
it into the body). No C++ change is justified by this evidence.

## D1 / D3 status in the same run (probe, not yet classified)
- `roof_ridge` at (0,380,90) reads (71,16,13) under one y convention and
  (0,0,0) under the other -> the ridge pixel is BLACK, i.e. there IS a
  defect at the ridge (expected: a gap/light-leak or a back-face sliver),
  because both roof slopes meet exactly on that edge.
- `path` at (0,0.6,600) reads (199,175,140) = the BODY's shaded colour, and
  `ground` at (600,0,400) reads (175,152,105) = also body-like: the path and
  ground probes are being occluded by nearer geometry too, so the D3 "path"
  artifact cannot be confirmed or refuted from these probes yet.
Classification of D1 and D3 continues in Task 2.

## Provenance / constraints
No C++ touched, no lighting values changed (D11-8: LIGHT_CANON/light setup
untouched - the probe ran under the existing 0.26-pre configuration). The
only tree change is the diagnostic `--probe` block in the scene.