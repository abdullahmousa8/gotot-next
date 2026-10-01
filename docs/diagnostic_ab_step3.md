# A/B Isolation - Step 3: Conditional Confirm (2026-10-01)

## Task 1: Ambient mechanism CONFIRMED
Both ambient-only frames were re-read and bucketed by their distinct 8-bit
triples (exact floats, no averaging):

GNE ambient-only frame (5 lit triples):
| n px | GNE byte | linear | equals |
|---|---|---|---|
| 392,934 | (19,17,11) | (0.075,0.067,0.043) | **0.1 x path albedo (0.75,0.65,0.45)** |
| 46,580 | (14,3,2) | (0.055,0.012,0.008) | **0.1 x roof albedo (0.55,0.12,0.10)** |
| 45,924 | (22,19,15) | (0.086,0.075,0.059) | **0.1 x body albedo (0.85,0.75,0.60)** |
| 10,724 | (20,18,14) | (0.078,0.071,0.055) | **0.1 x gable albedo (0.80,0.70,0.55)** |

So GNE ambient = **exactly 0.1 x albedo, per material, per channel** -
confirmed to 1 LSB on four independent albedos. It is albedo-tinted, as
hypothesised, and it is not an approximation: cpp:1012 is literally
`AMB * alb` with AMB = 0.1 (cpp:7151).

Godot twin ambient-only frame, same four surfaces:
| n px | twin byte | linear |
|---|---|---|
| 402,128 | (16,40,11) | (0.002,0.024,0.002) |
| 45,924 | (65,55,35) | (0.055,0.038,0.015) |
| 38,620 | (74,65,50) | (0.070,0.052,0.030) |
| 6,882 | (69,60,45) | (0.061,0.045,0.024) |

Both frames have exactly 45,924 px for the body surface - the geometry is
pixel-identical there, so the difference is purely the ambient model.

Per-channel ratios from Step 2 (R 0.80, G 0.25, B 0.27) are CONFIRMED and now
explained: the twin's ambient is NOT albedo x 0.1 either - it is a whiter,
desaturated term (twin body 0.055/0.038/0.015 vs GNE 0.086/0.075/0.059).
GNE multiplies by albedo (so R/G/B all scale together and the G/B ratio is
preserved), the twin's ambient adds a white-ish component. GNE being darker in
G/B by ~4x is exactly what "albedo-tinted vs whiter ambient" predicts.

## Task 2: Tonemap applicability (grep, GNE shaders)
Searched all GNE shaders for tonemap / ACES / Reinhard / Filmic / AgX / gamma /
pow-based encoding:
- **No tonemap operator exists.** The only `pow()` calls are the specular
  exponent (cpp:1009) and HZB/projection ratio curves (cpp:1160-1161,
  1308) - none is a display transform.
- **No sRGB/gamma encode exists.** The 8-bit target receives the LINEAR value
  directly (`out_color = vec4(AMB*alb + diff + spec + emi, 1.0)`, cpp:1013).
- Therefore Phase 4 (Linear EXR) is **N/A as a correction step**: both sides
  are already linear at the buffer. It remains valid only as a numeric
  confirmation that no hidden transform exists, which Step 2's 0.280 direct
  MAE already demonstrated.

## Task 3: Ambient model recommendation (for the Architect decision - NO fix applied)
Evidence points to ONE mechanism, and it is not an error in either engine:
- GNE: ambient = 0.1 x albedo (albedo-tinted, flat, no sky).
- Godot: ambient = white-ish x energy (sky/colour ambient, partially
  desaturated by its own pipeline).
- The direct path already matches at MAE 0.280, so nothing is wrong with the
  light, the BRDF, or the exposure scale of the direct term.

Options for the owner (not enacted):
1. **Keep GNE as-is** and record the difference as an accepted rendering-model
   difference. Cheapest; the house still reads correctly (0.26-pre baseline
   was captured under exactly this model).
2. **Make GNE ambient neutral** (0.1 x constant, no albedo tint) to match
   Godot's hue behaviour. This is a C++ change (push constant + one shader
   line), which is explicitly forbidden right now, and it would move the
   0.26-pre baseline (contrast/variance/edge) - so it must be a registered
   decision with a re-baseline, not a side effect of A/B.

Recommendation: option 1 now (record and move on), option 2 only if a future
milestone requires Godot-parity ambient. Nothing in the A/B data justifies a
change today.

## Stopped after Step 3 per directive. Steps 4-5 not started. No C++ touched,
no lighting values modified.