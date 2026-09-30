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

## 6. Standing rules this change established

- A contract — an observable that can fail — is written **before** the code it
  judges, never after.
- Refactor claims are stated as **static properties** (bytes fetched, tests
  performed) when no timing instrument exists. Never as speedups.
- A performance basis is re-measured and folded into **the same commit** as the
  change that invalidates it.
