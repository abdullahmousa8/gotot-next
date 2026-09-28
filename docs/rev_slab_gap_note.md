# GNE-018-rev: Cluster Slab Coverage Gap (measured; decision pending)

**Date:** 2026-09-28. **Status:** FIXED behind the 018-rev flag (option (a), Architect-approved).
F6 analytic proof and the dc-impact measurement are recorded in KI-014 (known_issues.md).

## What was measured

Unit-2b mandatory cross-check (cull-side AABB vs fragment-side slice assignment on
identical inputs; r0chk selftest mode 3): **10 of 14** corner/edge cases lie OUTSIDE the
cull AABB of the very cluster the fragment assigns to them. Gap (z0 - z_linear) up to
**707 world units** at far slices; center-tile cases are inside.

## Mechanism

- The fragment assigns pixels to depth slices by EUCLIDEAN camera distance
  (`z_view = length(cam - world)`, shared helper `gne_cluster_index`).
- The cull builds cluster AABBs with a LINEAR-depth z slab [z0, z1]
  (`gpu_light_cull_glsl`, the `znear * lratio^(tz/24)` construction).
- For off-axis pixels linear < euclidean (factor cos(theta)); at screen corners
  cos(theta_min) ~ 0.648 for fov 60 / 16:9. A pixel assigned to slice k can sit below
  the slab's z0 by up to (1 - cos) * z0 (~35% of z0 at corners).
- Consequence: a light whose sphere touches such a corner pixel but misses the AABB is
  EXCLUDED from the cluster list - contradicting the shader's "may over-include, never
  wrongly exclude" design claim. Latent in 018's own scenes (their probes avoided the
  exposed corners); real for corner-lit geometry.

## Proposed conservative fix (one line; scope decision required)

In the cull AABB: `bmin.z = z0 * cos_theta_max` with
`cos_theta_max = 1 / sqrt(1 + tanv^2 * (1 + aspect^2))`.
x/y extents and z1 are already conservative (linear <= euclidean <= z1), so this single
term makes the AABB cover every pixel the fragment can assign to the cluster; strictly
monotone: more lights included, never fewer.

## Scope question (frozen signatures)

The change alters the base cull lists, so any scene running it changes (`v18` literal
`v18|lc=20|cc=2841|ot=0|hr=0.94|d1` would move). Options:

- (a) apply only when the rev flag is enabled - flag-off keeps 018 byte-identical; the
  rev path gets the corrected slab and `v18-rev` carries its own signature;
- (b) ratify a separate 018 amendment milestone with re-baselined literals;
- (c) accept the latent gap (no action) and document.

Recommendation: (a) - preserves every frozen artifact and fixes exactly the domain
018-rev operates in.

## Evidence

- `temp\opencode\b008\r0chk_2b_run2.log` (F3 case rows: cid/z0/z1/in_box/gap).
- Repro: run `demo/gpu_smoke/main_r0chk.tscn`; section F mode-3 table.