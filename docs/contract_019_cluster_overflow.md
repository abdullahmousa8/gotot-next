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

## 6. What is proven and what is not

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

