# A/B Isolation - Step 2: Direct-only (2026-10-01)

## Task 1: Ambient clarification (the 0.45x claim, resolved)
- WHAT IT WAS: the twin's ambient term is ~0.45x of the naive
  albedo x energy, measured by re-scaling energy 0.1 -> 1.0 (roof band raw
  29.6 -> 91.6 for a 10x energy step). It is the TWIN (Godot) that is
  WEAKER, not GNE. GNE's ambient is exactly 0.1 x albedo, float math,
  hardcoded in the push constant (cpp:7151) - no model, no energy.
- WHY GNE LOOKED BRIGHTER despite the weaker twin ambient: the earlier
  unlit MAE was computed in MIXED spaces (GNE raw = linear, twin byte =
  sRGB). Re-computed in a single space (below) the unlit bias REVERSES: GNE
  is DARKER than the twin in ambient (bias -6.2/-10.4/-4.0 sRGB bytes),
  while the direct stage makes GNE brighter. The original +11 luminance
  "GNE brighter" reading was an artifact of the space mismatch, not of
  ambient strength.

## Two measurement conventions that MUST be honoured (KI-021)
1. GNE readback rows are BOTTOM-UP; the Godot viewport capture is top-down.
   vflip one side (99.4% overlap vs 1% straight).
2. GNE writes LINEAR radiance into the 8-bit target (no sRGB encode,
   verified: no tonemap/gpu_gamma in any GNE shader). The Godot viewport is
   sRGB-encoded. So GNE raw byte = linear*255 and twin byte = sRGB(linear).
   Comparisons below encode BOTH sides to sRGB before differencing.

## Task 2: Captures (sequential, same camera/geometry/materials)
- GNE: --direct (sun LIGHT_HOUSE (0.5,1,0.5), ambient floor cancelled by
  differencing against the --unlitlight frame - GNE has no ambient switch).
- Godot twin: --direct (DirectionalLight3D energy 1.0 white at
  normalized(0.5,1,0.5) via an explicit basis so local -Z == light dir;
  WorldEnvironment ambient energy 0; LINEAR; 1920x1080).

## Task 3: Results (both sides sRGB-encoded, lit+dark, 1920x1080)
| Stage | MAE R | MAE G | MAE B | mean |
|---|---|---|---|---|
| Direct-only | 0.331 | 0.287 | 0.223 | **0.280** |
| Ambient (unlit, re-scored in one space) | 6.202 | 10.407 | 4.012 | 6.874 |

Interpretation per the directive: DIRECT MAE <= 2 => the BRDF / direct
light path MATCHES. The +12 R/G is therefore produced by the AMBIENT stage,
not by light direction, colour or BRDF.

On the twin-lit pixels, linear domain:
- GNE ambient R=0.0523 G=0.0122 B=0.0083
- TWIN ambient R=0.0655 G=0.0500 B=0.0303
- GNE/TWIN ratio R=0.80 G=0.25 B=0.27

That ratio is the fingerprint of the mechanism: the two ambients disagree in
a CHANNEL-DEPENDENT way (R nearly equal, G/B GNE ~4x lower). A pure energy or
exposure error would scale all channels equally. The channel-dependent shape
points at the AMBIENT COLOUR SOURCE (GNE's ambient is albedo-tinted 0.1 x
albedo; the twin's white-ambient pipeline is not) rather than at light energy.

## Task 4: Which mechanism produces +12 R/G
AMBIENT COLOUR SOURCE (albedo-tinted ambient in GNE, white/neutral in Godot),
measured - not the directional light (direct path MAE 0.28) and not a colour
space error (darks 0.000, direct stage near-exact once both sides are in one
space). Ambient-stage magnitude: MAE ~6.9 sRGB bytes, biased -6.2/-10.4/-4.0
(GNE darker, G strongest).

## Where the +12 came from (correction to the earlier record)
The unlit MAE of 3.0 reported in 3e2e850 mixed GNE-linear with twin-sRGB; the
"+11 GNE brighter" conclusion drawn from it does not survive re-scoring in a
single space. Corrected statement: GNE direct == Godot direct (0.28); GNE
ambient != Godot ambient (6.87, channel-dependent, GNE darker).

## Stopping here (per directive). Step 3 not started.