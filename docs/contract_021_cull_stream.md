# contract_021_cull_stream — GNE-021 cull-only light stream

Status: **ACTIVE** (2026-09-30, commit `c23e38e`). Structural change, functionally
verified. **No performance-time claim is made and none can be** — see §4.

## 1. What changed and why it is a split, not an SoA conversion

The cluster cull pass runs once per cluster — 3456 of them — and loops over every
light. It used to read all four `vec4`s of the `GneLight` record
(`cpp:1479-1483`, now rewritten), i.e. **64 bytes per light per cluster**.

But the cull only ever needs:

| Field | Used by cull? |
|---|---|
| `pos.xyz`, `range` (`L0`) | yes, unconditionally |
| `color.rgb`, `intensity` (`L1`) | **never** |
| `dir.xyz`, `type` (`L2`) | only when the light is a spot |
| `cone_inner` (`L3.x`) | **never** |
| `cone_outer` (`L3.y`) | only when the light is a spot |

Converting the whole store to Structure-of-Arrays would have been the obvious move
and is the **wrong** one here: the fragment loop (`cpp:1908-1925`) reads all four
`vec4`s for the few lights in its own cluster, so AoS is correct for it. The two
passes want opposite layouts.

So the store is **split**, not converted. `light_buffer` is byte-for-byte unchanged
and still serves the fragment loop. A second buffer serves the cull:

```
light_cull_buffer : 3 vec4 per light, 49152 B total
  [0] = (pos.xyz, range)                read unconditionally
  [1] = (dir.xyz, type)                 read only after a hit, only for spots
  [2] = (cone_inner, cone_outer, 0, 0) read only after a hit, only for spots
```

**Point-light path: 16 B fetched per light per cluster instead of 64 B.** Spot
lights cost 32 B more, but only spot lights.

## 2. A side effect worth knowing

`intensity` is not in the cull stream, so `gpu_light_set_intensity` — which pushes
a single float at `id*64 + offsetof(GneLight, intensity)` — **no longer dirties the
buffer the cull walks for all 3456 clusters**. That API is unchanged, which also
means the four gates depending on it (`mat_v2_x1`, `mat_channel_cluster`,
`cluster_overflow`, and the 016 scene) are structurally unaffected.

Anyone adding a cull-visible field must add it to `_light_pack_cull` and to all
three full-record write sites, and must decide whether the partial intensity write
also needs to call it.

## 3. How it was verified — and the ordering that made it trustworthy

**The contract existed before the code.** The verification was added first, and it
was added because a real gap was found: `main_019_cluster_overflow` was the only
scene that dumped cluster light ids, and it creates **point lights only** — so the
cone branch the split touches had no id-level observable at all. That is the same
shape of blind spot that hid the `N=15` threshold in 019.

`main_022_shade.gd` gained `_dump_cluster_ids()`, printing per-slice id lists for
one **spot** column and one **point** column.

Determinism was checked, not assumed: the cull appends under `atomicAdd`, so id
order is not guaranteed stable. Two runs were compared — **identical** for both
columns. That is what makes a byte-for-byte diff a valid test *on this machine*;
it is not a claim about other hardware or under contention.

Result, before vs after the rebuilt binary:

| | |
|---|---|
| spot column, tile 9/5, 24 slices | **identical**, counts n=4 … n=89 |
| point column, tile 8/4, 24 slices | **identical**, counts n=4 … n=89 |

The dumps also prove the cone branch is genuinely exercised: the spot column
carries ids `66` and `175-180` and mixes point and spot ids inside one cluster.

A first `Compare-Object` reported differences. Those were **my own capture
ordering** (spot-then-point in one run, interleaved in the other), not data. The
comparison was redone on sorted unique lines.

## 4. What is claimed, and what is explicitly not

**Claimed:** 64 → 16 bytes fetched per point light per cluster. This is a **static
property of the data flow**, verifiable by reading the shader.

**Not claimed:** any frame-time or speedup figure. The harness cannot produce one —
`max_timestamp_query_elements` is unset, so `gpu_last_ns` is always 0 and every
number available is CPU-side wall time with driver back-pressure mixed in (see
`open_items_register.md`, the CPU-only per-pass limit entry).

**The re-baselined perf020 number is not evidence either.** The pre-split series
was already falling through this session — 29734, 27168, 21207, 18402, 18282,
18166, 17844 — so the scene had settled to ~17800 µs *before* any edit. Crediting
the drop to this change would repeat the exact error this contract exists to
prevent. The basis file says so in writing.

## 5. Two defects found while doing this — both will recur

1. **The gate was ignoring its own thresholds.** `perf020` printed
   `[FAIL>40000 WARN>46000]` immediately after the basis was rewritten to 24000.
   `fail_us` / `warn_us` were hardcoded in `gne_verify.ps1` while the basis file
   *also* listed them; only `exe_sha256` was ever read from the file. The file
   looked authoritative and was not. Fixed — the gate now reads both values, with
   the old numbers only as a fallback when the keys are absent.

2. **provenance and the stamp deadlock against a measured change.**
   `provenance` refuses a dirty module worktree, so C++ cannot be measured while
   uncommitted; and the stamp is written only on a **real relink**, so an
   already-built clean tree yields `NO_RELINK` and keeps the last `dirty` value it
   saw. The working order is therefore:
   **edit → commit → run (relink, stamp written clean) → measure → update basis →
   amend.**

## 6. Step 2 — coarse screen-space bounds pass (DONE, two items OPEN)

The cull tested every light against every cluster: measured, exactly
`clusters × lights` per frame. A new bounds pass, one thread per light, writes a
**conservative** screen tile rect by projecting the 8 corners of the light's
view-space AABB; perspective projection of a convex polytope is bounded by its
projected vertices, so the rect is a superset of the light's true footprint. The
cull skips any light whose rect misses the cluster tile.

Two fail-safes, because this pass can only ever drop lights: a corner at or behind
the near plane gives the light a full-screen rect, and a rect empty after clamping
degrades to one tile, never to zero. The cull's NDC convention is inverted from its
own cluster math rather than re-derived. Depth is deliberately **not** part of the
test — the slices are exponential and agreeing with `gne_cluster_index` is a
separate, larger change.

### Measured, not claimed

| scene | tests before | tests after | ratio |
|---|---|---|---|
| `022_shade` | 884736 | **359208** | 40.6% |
| `019` (spacing) | 55296 | **53136** | 96.1% |

and the number that proves nothing was lost: `assignments` 37180 → 37180 and
`clusters_touched` 2926 → 2926 on 022_shade, 15456 → 15456 and 1479 → 1479 on 019.

019 barely moves because all 16 of its lights are **coincident** and so share one
tile rect — a spatial prune cannot separate lights that occupy the same place. The
022_shade figure is the representative one.

`tests_performed` is a new counter (`ovf[1]`), so both the before and the after
come from the same instrument. No timing is claimed; GPU timestamps remain
unavailable.

### RESOLVED — the one-tile margin is geometrically necessary

**Closed by derivation, not by experiment.** The cluster box's x/y extents are
evaluated at `z1`, the slab's **far** plane (`cpp:1623-1624`), while the box's near
face sits at `bz0 <= z0 < z1`. Inverting the box to NDC at a depth `z` inside the
slab:

```
ndc_x(z) = bmin.x / (z * tanv * aspect) = nx0 * (z1 / z)
```

At `z = z1` that is exactly `nx0` — the tile edge. At `z = bz0` it is
`nx0 * (z1/bz0)`, whose magnitude is **larger**. So the box covers strictly more NDC
at its near face than the tile it was built from.

**Consequence:** the bounds rect is built from the light's true screen projection,
which maps to the **tile**, so it cannot cover that extra region. A light just
outside tile `tx` but inside the box's near-face extension passes `sphere_vs_aabb`
and would be skipped — measured as exactly 15 lost assignments in one cluster in
`gt_021a`. Hence the one-tile margin.

**The margin is not removable.** The construction is a superset only with respect
to the *tile*, not the *box*. The pre-existing comment at `cpp:1618` did say
"conservative: far-z extents" — the intent was written down and was read past.

**The required margin is position-dependent**, and the uniform one tile is a blunt
approximation of it:

```
margin_tiles ~ |ndc_edge| * (z1/bz0 - 1) * (grid/2)
```

With exponential slices `z1/z0 = (zfar/znear)^(1/24)`; for `zfar/znear = 1000` that
is ~1.333, so the requirement approaches 0 at the screen centre and ~2.7 tiles in x
at the edge. One tile covers every case the current scenes exercise — which is why
the cluster-id checks pass, and exactly why it must not be narrowed by inspection.

The **per-cluster derivation is the known better answer and is deliberately
deferred**: it changes load-bearing cull geometry and would need the same four
gates re-proven, for a benefit this harness cannot measure.

### RESOLVED — the perf020 spread is host-side, not an engine defect

Originally recorded as "bimodal, cause unknown". That was an over-reading of 16
points; the samples are a continuum, not two clusters.

Tested directly, with no new code: `main_020.gd` prints `present_api` and the
present size on every run (line 105). Across 8 runs `present_api = true` and
`tw/th = 1920/1080` **every time**, while wall times still spanned 18628–23040
(24%). Every configurable input is constant, so the variance is in the **host** —
CPU frequency and thermal state, machine load, or scheduler behaviour. This is the
same contamination that produced the 110977 µs sample earlier in this project.

There is no engine-side defect here and nothing to fix. The loose ceiling
(30000/34000) remains correct, now justified on evidence rather than on a guess.

### Three defects the contract caught that review did not

1. The bounds pass was dispatched **after** the cull while its comment said
   "before" — the cull read the previous frame's rect. Caught by
   `frustum_cull` (`1=1` → `1=0`), `cluster_overflow` (`ovf=2404` → `2`) and
   `mat_v2_x1` (`diff_px` 0 → 243). A comment asserting an ordering the code does
   not implement is itself the defect.
2. The uniform set supplied `view_ubo` at array index 2 while the shader declares
   `ViewBlock` at **binding 5**. Godot requires the declared binding. The cull's
   set agrees between index and binding by coincidence, so copying its shape
   produced the bug.
3. The bounds pass leaked three RIDs — shader, pipeline, uniform set — caught by
   `gt_023a`'s RID-cleanup check.

Two of the three were found by pre-existing gates, not by review. That is the
argument for writing the contract before the code, demonstrated rather than
asserted.

## 7. Standing rules this change established

- A contract — an observable that can fail — is written **before** the code it
  judges, never after.
- Refactor claims are stated as **static properties** (bytes fetched, tests
  performed) when no timing instrument exists. Never as speedups.
- A performance basis is re-measured and folded into **the same commit** as the
  change that invalidates it.
