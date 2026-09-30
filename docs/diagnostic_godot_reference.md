# Task 4 Report - Godot Reference + Unlit Verification (2026-10-01)

## Task 1 (parameters - CLOSED, all matched or documented)
- Camera: pos (0,300,1500), rot ZERO, fov 60, near 300, far 6200 - SAME both.
- Light: dir normalized(-0.5,-1,-0.5), WHITE 1.0, shadows OFF both (GNE house
  creates no shadow maps; twin sun shadow_enabled=false).
- Ambient: COLOR white x 0.1, no sky, both. GNE ambient = 0.1xALBEDO hardcoded
  (cpp:7151); twin WorldEnvironment ambient COLOR/0.1.
- Tonemap: NONE in GNE shaders (linear-out, verified by grep); twin LINEAR.
  Comparison done in linear (twin sRGB output inverted with the exact EOTF).
- Resolution: 1920x1080 both (twin --fullscreen; windowed gives 1920x1061).
  GNE raster viewport is 1152x648 into the 1920x1080 target (same 16:9).
- AA: none both. Glow: none both.
- GNE "unlit" protocol (no off-switch exists): sun straight down + black
  specular -> frame is EXACTLY 0.1xALBEDO (--unlitlight flag, documented).
  Twin unlit: sun energy 0 + ambient 0.1.

## Task 2 (twin scene - BUILT)
- demo/gpu_smoke/godot_reference/: project.godot + main_026_house_ref.tscn
  + main_026_house_ref.gd (9 ArrayMesh parts, numbers duplicated from GNE;
  the MAE itself validates the transcription). Runs on the vanilla D: binary
  (4.8.dev, no GNE strings). Capture via stdout hex chunks (Godot file writes
  fail in this shell; 160px/chunk - the editor binary drops >~4KB lines).

## Task 3 (unlit verification - INVESTIGATE band, root-caused)
- Geometry/framing: 99.4% pixel overlap AFTER vertical flip (495394/498k).
  FINDING: GNE readback rows are bottom-up vs Godot viewport top-down -
  recorded; every future image diff must vflip one side.
- Dark pixels (both < 5): MAE = 0.000 EXACT. No color-space issue (a colorspace
  shift would move the darks too).
- Lit pixels: MAE linear R=3.86 G=2.74 B=2.39, mean 3.00 (gate band 2-5 =
  INVESTIGATE). Bias GNE-twin +3.7/+2.3/+2.2 (GNE brighter, R strongest).
- Ambient-scale experiment (twin energy 0.1 vs 1.0, roof band): 8.4x in
  linear for 10x energy, absolute level ~0.45x of naive 0.1xALBEDO. The twin
  ambient does NOT equal albedo x energy in this build - Godot-side model
  question, NOT a GNE defect. GNE side is exact by construction (0.1 x albedo
  floats, verified in-shader).
- Original question ANSWERED: LIGHT_CANON is white; the +12 R/G mechanism is
  ambient-level/scale + missing cool sky fill, not light color. Do NOT touch
  light color.

## Ready for A/B: CONDITIONAL YES
- Proceed to Steps 2-4 (direct-only next: white light both sides removes the
  ambient-scale ambiguity and is the sharper test). Do NOT gate A/B on the
  <=2 unlit threshold until the Godot ambient-scale model is pinned - the
  residual is characterized, not blocking.
