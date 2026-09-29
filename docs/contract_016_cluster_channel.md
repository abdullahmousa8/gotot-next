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

## GNE-017.1 — the non-scaling share is the specular term, through `sc`

At `metal = 0` the cluster term splits into a part that tracks `alb` (diffuse)
and a part that does not:

```
C_off = D(alb_full) + sc * K_s
C_on  = D(alb_half) + sc * K_s          (grey channel halves alb)
S     = 2*C_on - C_off = 64 * spec_col
```
where `K_s = 64` falls out of the measurement. Measured sweep:

| step | `spec_col` | `shininess` | `metal` | `C_off` R | `C_on` R | `S_R` |
|---|---|---|---|---|---|---|
| `spec_black` | 0.0 | 32 | 0.0 | +22.0 | +11.0 | **+0.000** |
| `spec_0125` | 0.125 | 32 | 0.0 | +30.0 | +19.0 | +8.000 |
| (baseline) | 0.25 | 32 | 0.0 | +38.0 | +27.0 | +16.000 |
| `spec_0500` | 0.5 | 32 | 0.0 | +54.0 | +43.0 | +32.012 |
| `shine_004` | 0.25 | 4 | 0.0 | +38.0 | +27.0 | +16.000 |
| `shine_128` | 0.25 | 128 | 0.0 | +37.6 | +26.9 | +16.235 |
| `metal_100` | 0.25 | 32 | 1.0 | +38.0 | +19.0 | **+0.000** |
| `metal_050` | 0.25 | 32 | 0.5 | +38.0 | +23.0 | **+8.000** |
| `spec_1000` | 1.0 | 32 | 0.0 | **INADMISSIBLE** | **INADMISSIBLE** | — |

### The measurable limit, measured rather than assumed

`spec_1000` was added on 2026-09-30 to test the top of the law instead of
extrapolating past it. Its raw reads are `T_off = 255.0` and `B_off = 255.0`:
**both clipped**, so `C_off` collapses to `+0.0` and the derived `S = 58.000` is
an artifact, not a deviation from the predicted 64.0.

Every other row was checked against the admissibility rule (`T_off < 250` and
`B_off < 250`): the highest is `T_off = 220.0` at `spec_0500`, so **all seven
rows above are admissible** and none of the residuals (max 0.235) is clipping.

Therefore the proven domain is **`spec_col` in {0, 0.125, 0.25, 0.5}**, and
beyond it the **8-bit channel runs out of headroom before the law runs out of
range**. This is a limit of the measurement, not evidence against the shading
model - the distinction matters, because the structurally identical clipping in
GNE-019 was at first mistaken for a defect and had to be retracted.

Green and blue are `+0.000` in every row: the cluster term is red-only.

`S` is exactly linear in `spec_col` (64 per unit, four points), vanishes at
`spec_col = 0`, and vanishes at `metal = 1` where `C_on = C_off/2` to the digit.

**General form, now measured on three points.** Substituting `sc = (1-m)*spec_col
+ m*alb` into `S = (2*sc_on - sc_off) * K_s` and using `2*alb_half - alb_full = 0`
gives `S = 64 * spec_col * (1 - metal)`. This is algebra from `mix()`, and it is
no longer an extrapolation between two endpoints: the sweep measures
`metal = 0`, `0.5` and `1.0`.

| `metal` | measured `S_R` | predicted `64*spec*(1-metal)` | deviation |
|---|---|---|---|
| 0.0 | +16.000 | 16.000 | 0.000 |
| 0.5 | +8.000 | 8.000 | **0.000** |
| 1.0 | +0.000 | 0.000 | 0.000 |

The slope is -16 per unit of `metal`, i.e. `64 * 0.25` exactly. An internal
consistency check also holds: `C_off` is +38.0 at all three metal values,
because `spec_col` equals the base albedo (0.25), so `sc` is invariant in the
channel-off case while `C_on` alone moves (27 -> 23 -> 19) as `alb` takes over
from `spec_col` in `sc`.

**What step 3 did and did not establish.** Varying `shininess` was intended to
identify `S` as the `s2` term independently of the `sc` coefficient. It did not:
`S` moved from 16.000 to 16.235, i.e. not at all. The cause is geometric - the
surface is lit near head-on, so `dot(N, H2) ~ 1` and `pow(1.0, shiny_eff) ~ 1`
for every exponent. The control was therefore **not sensitive**, which leaves
`S` proven to scale with `spec_col` and vanish with `metal` but **not proven to
be `s2` specifically**. Any other factor inside `sc` would fit equally well.

## Scope

Covers the channel -> cluster-loop coupling for the three channel roles on one
isolated surface, with the cluster term measured absolutely via a
zero-intensity control, and the non-scaling share attributed to the `sc`
coefficient with its `metal` dependence confirmed at three points. Does not
cover: proof that the non-scaling share is the `s2` exponent term (the
`shininess` control was geometry-insensitive), shadows, GI, or multi-light
interference.
