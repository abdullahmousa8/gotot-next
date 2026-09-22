# Contract 014 — Tests

**GOTOT-014 — PASS (2026-09-23).** Harness: `gt_014a.bat` → `res://main_014.tscn`.

## 8 criteria evidence

| # | Criterion | Evidence |
|---|-----------|----------|
| C1 | GPU Scene DB ≥1M | `alloc(1,048,576)` → active=1048576, snap=1048576, ssbo=121,635,140 B |
| C2 | GPU-driven update | 16-MiB ring; 20,000 deltas (5000 adds/5000 removes/10000 moves); one apply; ring consumed 1,600,000 B then reset; 4-B-only readbacks |
| C3 | 013 hand-off | ordinal ∈ [b_l, b_l+mc_l) per instance; 013 fnv=3106528256 preserved with manager alive |
| C4 | 011 compat | snap=4096, distinct=8, groups=min(8,5)=5 |
| C5 | DET | in-binary double-dispatch identical (ords + stats); two full runs byte-identical `sig=v14|...|d1` |
| C6 | Regressions | 001B smoke, 007, 008A, 008B, 009, 010, 011, 013 — all exit 0 (013 literal sig preserved) |
| C7 | Benchmarks | active/applied/dispatch_seq/ring_bytes/ssbo measured and printed |
| C8 | Zero RID errors | no `ERROR`/FAIL in run output; shader/pass/uniform-set guarded |

## Signature

`sig=v14|c18616|a0|u5000/5000/10000|sn4096|dm8|g5|s121635140|dd365221617|d1`

## Regression delta guard

013 reference must stay byte-identical (single source of truth for the meshlet path):
`v13|t1048576|m18613|l0m10905|l0,1,1,2,2,0|a5532/5853/1265|cc37226/12650|pn5853/1265/559566|cov76685|sp1092857|w76685|f3106528256|d1`.

## Bugs fixed during bring-up (regression for future GPU indexing code)

1. Record base was `id*16` vec4-units instead of `id*4` → wrong offsets (active=258048).
2. Snapshot base `gi*8` instead of `gi*2` (uvec4 units).
3. `buffer_clear` + `submit()+sync()` for deterministic counter/record resets.

Rules: max 3 attempts per problem, then STOP + full report + await Architect decision.