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
| silence: red excess vanishes with no cluster light | `chroma_B <= 0.5` | 0.000 |
| cluster term is red-only, above the same floor | `dR >= 2.55` and `\|dG\|, \|dB\| <= 0.5` | +27.000 / 0.000 / 0.000 |
| roughness control | `sd_r > 1.0` and `max_lsb > 0` | 9.785 / 21 |
| normal control | `nrm_delta > 0.1` | 0.435547 |

## Absolute cluster share (GNE-017)

`gpu_light_set_intensity` (added in GNE-017) writes only the intensity float, 4
bytes at `id*64 + offsetof(GneLight, intensity)`, leaving position, range,
colour and cluster membership untouched. A zero-intensity light is therefore the
same light that has stopped emitting. That converts the previously impossible
"light absent" cells into a real A-minus-B differential, and lifts the
measurement from an estimate to an absolute number:

```
A-B cluster term  dR=+27.000  dG=+0.000  dB=+0.000
chroma_B = 0.000        (with no cluster light the channel response is perfectly neutral)
```

`dG` and `dB` are exactly zero, so the cluster term is red-only, and
`chroma_B` being exactly zero shows the red excess cannot arise without the
light.

### The cluster red is only partly albedo-driven

This corrects an over-broad reading that the first chroma-only measurement
supported. The cluster light adds **38** red with no channel bound and **27**
red with the grey channel bound (which halves `alb`). Solving:

- alb-dependent (diffuse) part: **22 LSB**
- alb-INDEPENDENT (specular) part: **16 LSB**

The independent part is explained in the shader: `sc = mix(spec_col, alb,
metal)`, and with `metal = 0` that reduces to `spec_col`, so the cluster
specular term is red (via `A1.rgb`) yet independent of the albedo channel.

**Therefore the presence of red proves nothing.** What proves the cluster loop
consumes `alb` is that the red excess *changes* (38 -> 27) when the albedo
channel is bound. Any claim resting on `chroma != 0` alone would be unsound.


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

The cluster loop's **specular** term is not proven to consume `alb`; the
measurement shows it does not (16 LSB of the 27 is `spec_col`-driven and
independent of the channel). The **diffuse** path is proven: 22 LSB of it
tracks `alb`.

The gate name must not be read as "the cluster path is fully explained". What is
now established is the absolute size of the cluster term (27 LSB, red-only) and
its split into channel-dependent and channel-independent parts.

Still unexercised: shadow interaction (`shf` in the cluster loop - the scene
binds no shadow map), GI interaction, and multi-light interference (the scene
is deliberately single-light; `main_016_5` is crowded and cannot isolate). The
roughness control asserts that the channel changes pixels and creates spatial
variance - not that the cluster loop consumes `rough`.

## Scope

Covers the channel -> cluster-loop coupling for the three channel roles on one
isolated surface, with the cluster term measured absolutely via a
zero-intensity control. Does not cover the specular channel dependency, shadows,
GI, or multi-light interference.
