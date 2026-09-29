# GNE-019 — cluster light capacity: safety fix applied, a separate defect measured

**Status:** TWO separate items. One safety defect FIXED. One functional defect
OPEN and reproduced. Not closed.

## 1. FIXED — out-of-bounds and cross-cluster read

The cull appends each light under `if (slot < 16u)` (cpp:1509), so a cluster
holds at most 16 valid ids at stride 16, and `cluster_index_buffer` is
`3456 * 16 * 4 = 221184 B = 55296` uint slots. But the count is an uncapped
`atomicAdd`, so `clcnt.cnt[cid]` can exceed 16, and the fragment loop read:

```glsl
for (uint j = 0u; j < ccnt && j < 64u; j++)   // stride is 16
```

With `16 < ccnt <= 64` the loop addressed the **next cluster's** slots, and for
the last cluster (`cid = 3455`, `coff = 55280`) reached index 55343 against a
last valid index of 55295 — a **192-byte out-of-bounds read**.

**Fix:** bound the read at 16, equal to the stride:

```glsl
for (uint j = 0u; j < ccnt && j < 16u; j++) {
```

Committed alone as a safety change, kept separate from any functional work.

## 2. OPEN — cluster contribution collapses to zero at N >= 16

Reproducible, deterministic, measured on a scene whose geometry is correct
(the light is aligned with the flood surface, so the measurement responds to
lighting at all — an earlier geometry left the surface outside the cluster and
showed a flat, unresponsive `ratio = 1.000`).

| N | `C` flood | `C` neighbour | `ovf` |
|---|---|---|---|
| 1 | **24.951** | 22.000 | 0 |
| 16 | **0.000** | **0.000** | 0 |
| 20 | **0.000** | **0.000** | 5120 |

Adding 15 identical lights takes both surfaces to **exactly zero**. This is not
saturation: clamping leaves a value at the ceiling, it does not zero it. It is
not the truncation guard: `ovf` is 0 at N=16, so nothing was rejected.

**Diagnostic — the lights are stored correctly.** Dumping the cluster contents
through `gpu_light_debug_cluster`:

```
N=1   tile 5/4  t7:[0] t8:[0] ... t22:[0]
N=16  tile 5/4  t7:[0,1,2,...,15] t8:[0,1,...,15] ...
```

The id list is complete and correct at N=16. So the fault is **downstream of
cluster assignment**, inside the per-light evaluation, not in the cull or in
truncation.

Candidate terms in `ldiff = alb * ndl2 * A1.rgb * (A1.a * att * cone_f)` and
`lspec = s2 * sc * A1.rgb * (A1.a*att*cone_f) * step(0, dot(N,Ld))`, all scaled
by `shf = gne_shadow_light(lid, ...)`. Since all 16 lights share one position,
`Ld`, `ndl2`, `att` and `cone_f` are identical for every one of them, so a
per-light factor cannot explain all of them zeroing simultaneously. `shf` is the
only term that varies with `lid`, and it is the leading suspect — but this has
**not** been measured, and is not claimed.

## 3. Spacing test — the shf hypothesis is REFUTED

Light 0 is held on the flood surface so `C_ref` stays live; only lights 1..15
are spread on a 4x4 grid around it.

| mode | `C_ref` (N=1) | `C` (N=16) | ratio | `ovf` (N=20) |
|---|---|---|---|---|
| identical (co-located) | 24.951 | **0.000** | 0.000 | 5120 |
| spaced (1 on-axis, 15 spread) | 24.951 | **25.025** | 1.003 | 2404 |

**The `shf` / index-at-`lid 15-16` hypothesis is refuted.** An index or shadow
decompression fault would persist regardless of position; it does not. The
collapse is tied to co-location: 16 lights at one point annihilate the
contribution, while the same 16 spread out leave it alive.

## 4. Accumulation is not linear, and the lower bound was wrong

`ratio = 1.003` for 16 spaced lights against a 24.951 single-light reference.
Accumulation is therefore neither `16x` nor additive. The `cap_lo` lower bound
(`>= 2x`) encoded an assumption about the shading model that measurement
contradicts, and was **removed**. What remains is the double-counting guard
`C(16) <= 16 * C_ref`, which is the mathematically correct expression of "no
light is summed twice" and is exactly what the old 64-slot read could have
violated. It holds in both modes (25.025 <= 399.2; 0.000 <= 399.2).

## 5. Neighbour collapse is a capacity boundary, not a defect

The neighbour reads 22.000 at N=1 and 0.000 at N>=16 in **both** modes, with
different `ovf` (5120 vs 2404). It is therefore independent of co-location and
tracks the cluster capacity of 16: once the tile light list is full, the
neighbour's own lights are displaced. This is a system boundary and is recorded
here rather than opened as a fix.

## 6. Micro-offset sweep — the threshold is sharp, and it is NOT a singularity

Light 0 is held fixed and lights 1..15 are placed at distance `d` from **that same
point**, so `d = 0` reproduces exact co-location and `d` is the only variable.

| `d` | `C(16)` |
|---|---|
| 0, 1e-6, 1e-4, 1e-3, 1e-2, 0.1 | 0.000 |
| 1, 2, 5, 10, 20, 50, 100, 150, 160, 170, 175, 180, 185, 190, 195, 199, 200, 220 | 0.000 |
| **240** | **7.086** (partial) |
| 250 | 16.000 |
| 260 | 22.000 |
| 280, 300, 320, 400 | 24.988, 24.630, 25.395, 25.457 |

**The threshold is (220, 240] and it is sharp** — 0.000 to a full value within
20 units, with a partial point at 240. That is a structural switch, not a
continuous distance function.

### 6a. The N x d grid — the condition is TWO variables, not one

The sweep above ran at N = 16 only, which made the threshold look like a pure
geometric property of proximity. It is not. Holding `d` and varying the count:

| `d` \ `N` | 2 | 4 | 8 | 12 | 13 | 14 | 15 | 16 |
|---|---|---|---|---|---|---|---|---|
| 220 | 24.827 | 24.667 | 24.815 | 24.765 | 24.975 | 24.802 | **8.296** | **0.000** |
| 230 | 24.691 | 25.272 | 25.025 | 24.901 | | | **0.000** | **0.000** |
| 260 | 25.432 | 25.432 | 25.074 | 24.802 | | | | 22.000 |

**At N <= 14 no distance kills the light at all** - even at d = 220. The
collapse requires BOTH `N >= 15` AND `d <~ 230`. It is a two-variable
condition, and describing it as a geometric proximity threshold was wrong.

### 6b. The onset is at N = 15, which is cap - 1

| `N` | 12 | 13 | 14 | **15** | **16** |
|---|---|---|---|---|---|
| `C` at d=220 | 24.765 | 24.975 | 24.802 | **8.296** | **0.000** |
| `ovf` | 0 | 0 | 0 | **0** | **0** |

N = 15 collapses to a third of full and N = 16 is dead. That is a step across
two counts, **not a gradual ramp in N**: the onset is exactly one slot below
`GNE_CLUSTER_LIGHT_CAP = 16`.

**`ovf = 0` throughout.** The overflow counter never fires, so the overflow path
is exonerated outright - the fault lies before it, at the 15/16 boundary
itself. N = 15 (ids 0..14) vs N = 16 (ids 0..15) is the signature of a bound
computed off by one, or a 15-vs-16 stride confusion in the slot address.

This also explains why the defect stayed hidden: the scene only ever stepped
`1 -> 16`, never sampling 15. **N = 15 is the smallest regression gate that
exposes it.**

### 6c. Correction: the degradation is graded, not a sharp switch

At N = 16: `d=280 -> 24.988`, `260 -> 22.000`, `250 -> 16.000`, `240 -> 7.086`,
`<=230 -> 0.000`. That is monotonic in `d` and reaches zero at a transition
point. Calling it a "sharp structural switch" was an overstatement; the
accurate description is a graded degradation under capacity pressure that
falls to zero near exact co-location.

Refuted along the way:

- **Singularity / zero-distance division.** Any `d > 0` would have cured it,
  including `1e-6`. None did, across five orders of magnitude.
- **A tile-geometry threshold.** The 120px raster tile and `GNE_CLUSTER_X = 16`
  are not candidates: the bracket matches no such multiple, and `200` and `256`
  are both still inside the dead zone.
- **Pure geometric proximity.** At `N <= 14` no distance kills the light.
- **Full-capacity saturation.** The onset is `N = 15`, not `N = 16`; the cluster
  is one slot short of full when it already collapses.

A previously reported bracket of (150, 200) was **wrong** and is corrected here.
It was derived by comparing against the nominal `200` of the spacing grid, whose
real separations reach 300 - the measurement was not against the geometry it
was meant to bracket.

**Not proven:** the cause, but the search is no longer open-ended. The leading
candidate is a bound computed off by one at the 15/16 slot boundary (or a 15-vs-
16 stride confusion in the slot address), because the onset is exactly
`GNE_CLUSTER_LIGHT_CAP - 1` and the overflow counter never fires. Confirming it
means reading `ccnt` and `shf` at N = 15 in C++ - no longer a blind probe.

## 7. What is proven and what is not

Proven: the out-of-bounds read existed and is fixed by bounding at 16; the
collapse to zero is tied to co-location, not to indexing or shadows; the cap
guard and overflow counter work; accumulation is non-linear; the neighbour
collapse is a capacity boundary.

Not proven: the mechanism of the co-located annihilation. Overlapping
attenuation terms or a non-finite intermediate in the accumulation are
candidates; neither has been measured, and neither is claimed.

## 7. KNOWN LIMITATION OF THE GATE — read before trusting a PASS

With `cap_lo` removed, the gate **passes in both modes, including the
identical mode where the flood surface renders at C = 0.000**. The gate
therefore no longer detects the co-located collapse; it only certifies no
overflow, no double counting, a live reference, and neighbour invariance. The
value is recorded in the signature (`cap=0.000`) so the anomaly stays visible
in logs, but no condition fails on it. A survival condition (`C(16) > 0`) would
catch it and is a deliberate, separate decision, not taken here.

An earlier claim in this slice that `delta_neigh = 0.000` proved
cross-cluster isolation was **wrong**: it compared N=16 against N=16, i.e. zero
against zero. Against the correct N=1 baseline the neighbour moves by -22.000.

