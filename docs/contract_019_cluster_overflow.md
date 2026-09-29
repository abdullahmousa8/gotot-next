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

## 3. What is proven and what is not

Proven: the out-of-bounds read existed and is fixed; the collapse to zero is
real, deterministic, and occurs after correct cluster assignment; the cap guard
and the overflow counter work.

Not proven: the cause of the collapse. No fix attempted. The capacity gate
remains failing (`EXIT=72`) and must not be wired into CVS.

## 4. Also recorded

An earlier claim in this slice — that `delta_neigh = 0.000` proved
cross-cluster isolation — was **wrong**: that delta was measured against an
N=16 baseline, i.e. zero against zero. Against the correct N=1 baseline the
neighbour moves by -22.000. The current gate compares N=20 against N=16, which
is again zero against zero and **cannot** detect contamination while both
surfaces are dark. That test needs redesigning once the collapse is fixed.

Signature formatting was also fixed (a `str` was passed to a `%d` field).
