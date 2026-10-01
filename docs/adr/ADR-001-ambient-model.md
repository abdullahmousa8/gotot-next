# ADR-001: GNE Ambient Model

**Date:** 2026-10-01
**Status:** Accepted
**Reference:** docs/diagnostic_ab_step2.md, docs/diagnostic_ab_step3.md

## Context
GNE uses tinted ambient (`AMB * albedo`, `AMB = 0.1` hardcoded in the
material push constant, gne_render_server.cpp:7151 and :1012).
Godot uses a flat/white ambient term (`ambient_light_color` x energy).

A/B isolation against a vanilla Godot twin of the same scene proved:
- **Direct path matches: MAE 0.280** (both sides sRGB-encoded, 1920x1080) -
  the BRDF, light direction, colour and exposure of the direct term are equal.
- **Ambient differs: MAE 6.87**, channel-dependent (GNE/twin R 0.80, G 0.25,
  B 0.27).
- GNE ambient = exactly `0.1 x albedo`, confirmed to 1 LSB on four
  independent albedos across ~500k pixels.
- No tonemap and no sRGB encode exist in any GNE shader (Step 4 N/A).

Conclusion: not a bug, not an energy error, not a BRDF error - a different
ambient model.

## Decision
Accept the GNE tinted ambient as the design. No engine change.

## Rationale
- Tinted ambient is physically motivated (it stands in for indirect colour
  bleed, which is albedo-tinted by construction).
- The direct path is proven identical (MAE 0.28), so nothing else is wrong.
- A C++ change would break the provenance stamp and force a relink for no
  measured benefit.
- Reversible later: the term is a single float in a push constant.

## Consequences
- Scenes rendered with GNE appear warmer than the Godot default. This is
  expected, not a regression.
- Any Godot-twin comparison must match the ambient model before the numbers
  mean anything (Phase 1/3 numbers above are the exception: they are the
  measurement OF the difference).
- The 0.26-pre metric baseline was captured under the tinted model and stays
  valid - no re-baseline.
- Revisit during visual QA (0.26b) if the look is judged wrong.