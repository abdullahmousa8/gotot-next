# Contract 026 — Visual Metric (Objective (D11-6))

## Scope
- Objective, framebuffer-derived visual quality metric for the 0.26 line
  (0.26a scene fix, 0.26b visual slice).
- Replaces the subjective 5.9/10 -> 7.0/10 scoring, which is NOT an
  instrument and therefore is not a criterion.

## Definitions (measured on the 1920x1080 readback)
- **contrast** = stddev(luminance) / mean(luminance) over the frame.
- **color variance** = mean over pixels of the per-pixel RGB channel variance.
- **edge density** = fraction of pixels with |d(luminance)/dx| > EDGE_T, with
  **EDGE_T = 0.1** pre-registered before the baseline run.
- luminance = (R+G+B)/3 on normalised 8-bit channels.
- Determinism is part of the metric contract: two consecutive readbacks must
  be byte-equal (`det=1`).

## Baseline (AUTHORITATIVE - re-registered 2026-10-01)
`contrast=1.849952 | variance=0.003122088 | edge=0.000652 | det=1`
Scene: `demo/gpu_smoke/main_026_house.gd` after the D1 material-index fix,
LIGHT_HOUSE instrument sun, 1920x1080.

**Withdrawn baseline (0.26-pre):** contrast 1.857549 / variance 0.002933922 /
edge 0.000654. Captured on an integrity-broken scene (every surface carried
its neighbour's material; the last mesh read an unwritten black slot). Kept
only as history and as the negative-control target for the D1 fix.

## Gates for 0.26b
| metric | gate | value |
|---|---|---|
| contrast | >= baseline x 1.10 | **2.034947** |
| color variance | >= baseline x 1.15 | **0.003590401** |
| edge density | <= baseline x 1.20 | **0.000782** |
| determinism | byte-equal | det = 1 |

## Pre-condition (binding, Lesson 10)
A baseline may only be captured on an **integrity-verified** scene: a
full-frame colour census in which every declared part carries a colour equal
to its own albedo x k (`tools/scene_defect_probe.gd`). Capturing a metric on
an unverified scene is invalid, and any gate computed from it rewards the
defect.

## Non-goals
- Not a performance metric (no GPU timers exist - KI-001).
- Not a perception study; no human rating is admitted as evidence.
- Not a comparison against the Godot twin (different ambient model - ADR-001).