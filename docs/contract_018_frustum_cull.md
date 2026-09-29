# Contract 018 — frustum culling of cluster lights

**Status:** MEASURED. Gate is fail-closed but **not yet wired into CVS**.
**Date:** 2026-09-29. **Method:** dual-condition validation (cluster membership
read back from the GPU, plus a pixel measurement).

## What is proven

A light outside the view frustum is removed from the cluster path, and the
removal is attributable to **culling** rather than to distance decay.

```
S1 ref       present=true  tz=[6..22]  C=27.000  body=81
S2a repeat   C=27.000  body=81  delta_vs_ref=0.000
S2b yaw=0.02 present=true  tz=[6..22]  C=25.123  body=81
S3 side      present=false tz=[]       C= 0.000  body=81
S4 rear      present=false tz=[]       C= 0.000  body=81
cond ref=true repeat=true rot_keep=true side=true rear=true
```

## Why the dual condition is mandatory

A pixel measurement alone cannot prove culling. Move a light far away and `C`
tends to zero through attenuation, `1/(1+d^2/r^2)`. A `C` of 0 is ambiguous
between "culled" and "merely far", so every step asserts both:

1. **Membership** — the light id is absent from the target cluster column's
   list, read back with `gpu_light_debug_cluster`.
2. **Contribution** — `C_R` falls to `0.000 +/- 0.5` LSB, matching `chroma_B`
   from the 016.cclus work.

**This was not decorative.** On the first run the rear-cull step produced
`C = 0.000` while membership stayed **PRESENT** (`tz = [0..13]`), and that
combination was rejected. A pixel-only test would have declared it a pass. The
cause was physical: with `range` held at 1400 (owner decision), a light only
200 units behind the camera still has a sphere reaching 1400 units forward,
into the front clusters. The displacement must exceed the light's own radius.
Moved to 1600 units (range + 200), the membership condition then held.

## How frustum culling actually happens

The cull shader has **no explicit frustum-plane test**. Its only rejection test
is:

```glsl
bool hit = sphere_vs_aabb(vc, rr, bmin, bmax);
```

against each cluster's view-space AABB, derived from the NDC tile rect and the
depth slice. Frustum culling is therefore **emergent**: a light outside the
frustum falls outside every cluster box, so it is never appended. The declared
`planes[6]` in the cull push constant is **never read** — a separate
`CullPush` vs `CullParams` field-alignment question is deferred to a dedicated
maintenance slice.

Consequence worth knowing: a light *inside* the frustum may still be absent if
it misses every cluster box. That is correct behaviour, not a defect.

Membership is evaluated by scanning all 24 depth slices at the target tile
column rather than computing the slice, because the slice formula lives in
`gne_cluster_index` (cpp:1022) and a second copy would violate the 018-rev
single-source rule. This makes the predicate conservative in the safe
direction: it can report presence more often than the fragment's single slice
would.

## View-dependent specular: a recorded physical property

An earlier version of the rotation control asserted that `C` is byte-stable
under camera rotation. **That premise was wrong**, and the measurement refuted
it rather than confirming it: a 0.02 rad yaw moved `C` by `-1.877`.

The cause is in the shader (cpp:1915):

```glsl
vec3 H2 = normalize(Ld + V);
float s2 = pow(max(dot(N, H2), 0.0), shiny_eff) * (1.0 - 0.5 * rough);
```

`V` is the view vector, so rotating the camera changes `H2` and therefore
`s2`. The cluster **diffuse** term is view-independent
(`alb * ndl2 * A1.rgb * (A1.a * att * cone_f)`, no `V`); the cluster
**specular** is not. The observed `-1.877` against a 16 LSB specular share is
about 12%, consistent with `pow` moving as `dot(N,H2)` leaves 1.

The control was therefore replaced with the correct two parts:

- **S2a repeatability** — two consecutive reference readings, no state change,
  `ΔC = 0.000` exactly. This is what the instrument must guarantee.
- **S2b non-culling under rotation** — after a 0.02 rad yaw, membership must
  still be `true`, proving the rotation did not drop the light. `C` is expected
  to move and is deliberately **not** gated.

## Threshold provenance

`C_REF = 27.000` is not a fitted number: it is the value already measured and
protected by the `mat_channel_cluster` gate in 016.cclus. S1 reproducing it
exactly is a **mutual cross-validation** between the two scenes.

## Gate conditions and enforcement

| Condition | Threshold | Measured |
|---|---|---|
| S1 reference | `present` and `\|C - 27.000\| <= 0.5` and `body = 81` | present, 27.000, 81 |
| S2a repeatability | `ΔC == 0.000` exactly | 0.000 |
| S2b non-culling | `present == true`, `body = 81` | true, 81 |
| S3 lateral cull | `not present` and `\|C\| <= 0.5` | absent, 0.000 |
| S4 rear cull | `not present` and `\|C\| <= 0.5` | absent, 0.000 |

The scene exits 0 only when all five hold, and 71 otherwise. The rejection path
was verified with a temporary forced-false condition (`GATE FAIL`, exit 71),
then reverted — so the gate is fail-closed rather than print-only. Deterministic
across two runs, byte-identical.

**Not yet in CVS.** Unlike `mat_channel_cluster`, this scene has no row in
`gne_verify.ps1`, so nothing in the belt runs it yet. It must not be described
as protected until that row exists.

## Scope

Frustum culling only, verified on one surface with one point light.
**Depth / Z-buffer culling is out of scope and remains blocked**; the
raster-depth path is unapproved and no measurement here touches it. Does not
cover: multiple simultaneous lights, spot cone culling, the unused
`planes[6]` field, or the `CullPush`/`CullParams` alignment question.
