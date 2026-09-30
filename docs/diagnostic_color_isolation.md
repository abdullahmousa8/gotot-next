# Diagnostic - Color Isolation (A/B vs Godot reference)

## Task 1: LIGHT_CANON Audit (measured 2026-10-01 - raw values from source)

- Direction: mat_light_dir (gne_render_server.h:293), default =
  normalized(-0.5,-1.0,-0.5) = (-0.4082,-0.8165,-0.4082); set_light stores the
  normalized input (cpp:7075). L points DOWN-back-left.
- Color: light_rgb = (1.0, 1.0, 1.0) WHITE, intensity 1.0 (cpp:7156-7159).
  THE LIGHT IS NOT WARM - any warm bias comes from elsewhere.
- Ambient: AMB = 0.1 x albedo (cpp:7151), added unconditionally:
  out = 0.1*alb + alb*ndl*white + specular + emissive.
- Shader use (cpp:1003-1012): ndl = max(dot(N,L),0); backfaces
  (dot(N,Vv)<0) get emissive/backface ONLY, zero diffuse.
- CONFIRMED: for outdoor scenes L lights DOWN-facing surfaces; up-facing
  geometry (ground, roofs, wall tops) sits at 0.1xALBEDO. The 0.26-pre house
  under LIGHT_CANON: mean 0.0146, 0.09% pixels > 0.1; under an above-front
  sun: mean 0.127, 24% > 0.1 (same geometry/draw, light only).
- Godot comparison (structural, no run needed for the audit): Godot
  DirectionalLight3D = white light + SKY/hemisphere ambient fill (cool) +
  energy; GNE = white dir-light + 0.1xALBEDO ambient, no sky. Predicted
  deltas: (1) missing cool sky fill, (2) wrong-face directional for outdoor
  top surfaces, (3) ambient carries the albedo hue (warm albedos -> warm
  ambient) where Godot ambient is neutral/sky. The +12 R/G with +2 B FITS
  (3)+(1): warm albedo x 0.1 with no cool counterpart - NOT a warm light.
- Correction to the warm-light hypothesis: do NOT "fix" light color (it is
  already white); the isolation must test ambient-vs-directional (Steps 2-3).

## Task 2: A/B Isolation plan (5 steps - NOT started; Godot-reference method TBD)

- Step 1 Unlit (albedo only; MAE<2 match / >5 color-space issue).
- Step 2 Direct-only (white 1,1,1; ambient+GI off).
- Step 3 Environment-only (sky+ambient; LIGHT_CANON+GI off).
- Step 4 GI-only. Step 5 Full cumulative.
- OPEN METHOD QUESTION for the owner: how is the Godot reference produced
  (vanilla-Godot twin scene with StandardMaterial3D + DirectionalLight?
  which sky? which tonemap?) - no Godot-side renders captured until defined.
  Stop-after-each-step applies from Step 1.
