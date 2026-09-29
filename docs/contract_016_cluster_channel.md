# Contract 016.5 — material channels feed the clustered light loop

**Status:** MEASURED and protected by a standing CVS gate (`mat_channel_cluster`).
**Date:** 2026-09-29. **Method:** chromatic differential in an isolation scene
(`demo/gpu_smoke/main_016_cluster_channel.gd`), not inference.

## What is proven

The material channel path feeds the clustered light loop: the loop consumes the
`alb` value the channels write.

```
A dR=-46.000 dG=-35.000 dB=-35.000 neutral_split=0.000 chroma=11.000 body=81
R sd_r=9.785 max_lsb=21 | N nrm_delta=0.435547 max_lsb=85
cond body=true neutral_split=true chroma=true rough_ctl=true normal_ctl=true
```

A red-only excess over the neutral response is not explicable by any other path
in the scene:

1. The global directional term is nulled. `gpu_material_set_light((0,0,1))`
   points the light along +Z, perpendicular to the target face normal (-Z), so
   `ndl = max(dot(N,L),0) = 0` and the `step(0.0, dot(N,L))` gate zeroes the
   global specular in the same move.
2. The only red source in the scene is the single cluster point light
   (`Color(1,0,0)`). The global `light_color` in the push constant is white
   (1,1,1), so the global terms cannot introduce red chroma.
3. The albedo payload is NEUTRAL grey (`assets/neutral_grey.gtex`), so the
   surviving global term `AMB*alb` is chromatically neutral and moves G and B
   identically - measured `|dG - dB| = 0`.

If the cluster loop were absent, culled away, or blind to `alb`, `dR` and `dG`
would coincide and the chroma residual would be zero. It is 11.

## Gate conditions (owner-declared 2026-09-29)

| Condition | Threshold | Measured |
|---|---|---|
| window is surface, not background | `body_px == 81/81` | 81 |
| neutral split (G/B carry only the neutral term) | `\|dG - dB\| <= 2.0` | 0.000 |
| cluster chroma | `\|dR - dG\| >= 2.55` (1% of range) | 11.000 (4.3%) |
| roughness control | `sd_r > 1.0` and `max_lsb > 0` | 9.785 / 21 |
| normal control | `nrm_delta > 0.1` | 0.435547 |

The scene exits 0 only when all five hold, and 61 otherwise. The rejection path
was verified with a temporary probe (a forced-false chroma condition produced
`GATE FAIL` and exit 61, then was reverted), so the gate is fail-closed rather
than print-only. `gne_verify.ps1` runs it as `mat_channel_cluster`, requiring
exit 0 AND the `GATE PASS` marker, and rejecting `GATE FAIL`, `ERROR:`,
`invalid ID` or `SCRIPT ERROR`.

A ratio threshold (`|dR-dG| / |dG| >= 0.5`) was considered and **dropped**. The
neutral term `AMB*alb` does not depend on light intensity, so the ratio's
denominator is fixed and the ratio only improves by cranking intensity until
clipping - result-fitting, not a stronger property. The absolute chroma floor
is the declared criterion.

## What is NOT proven

The **absolute** share of the cluster term is not established, and the gate name
must not be read as claiming otherwise. The "light absent" control cells (B/D)
are **unrenderable**: `gpu_material_draw_lights` hard-fails without a light
store (`cpp:8950`, exit 6013) and `gpu_light_create` is append-only with no
intensity setter. Measured, the cluster contributes roughly 31% of the channel's
total response at this geometry; the remainder is not proven to travel solely
through the cluster path.

Raising that ceiling requires a C++ zero-intensity control
(`gpu_light_set_intensity` or equivalent). Tracked as a deferred item in
`open_items_register.md`.

## Scope

Covers the channel -> cluster-loop coupling for the three channel roles on one
isolated surface. Does not cover: the absolute cluster share (above), shadow
interaction (`shf` in the cluster loop is unexercised - the scene binds no
shadow map), GI interaction, or multi-light interference (the scene is
deliberately single-light; `main_016_5` is crowded and cannot isolate). The
roughness control asserts that the channel changes pixels and creates spatial
variance - not that the cluster loop consumes `rough`.
