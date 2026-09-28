# GNE — Rename Verification (Gotot-Next → Godot-Next-engine / GNE)

**Date:** 2026-09-28 · **Method:** full `gt_regress` sweep + per-scene `gt_harness` DET runs on the renamed tree.
**Evidence file:** `gt_regress_after_rename.txt` (repo root, this commit).

## Result: GT_REGRESS: PASS

| Scene | Result |
|---|---|
| main_007 / 008 / 008b / 009 / 010 / 011 / 013 / 014 / 015 | rc=0 marker=1 pass=1 |
| main_012 | XFAIL (rc=124, known WIP, deferred — not gating) |

## Signatures (all literal, verified this run)

- 013: `f3106528256` (full: `v13|t1048576|m18613|l0m10905|l0,1,1,2,2,0|a5532/5853/1265|cc37226/12650|pn5853/1265/559566|cov76685|sp1092857|w76685|f3106528256|d1`)
- 014: `v14|c18616|a0|u5000/5000/10000|sn4096|dm8|g5|s121635140|dd365221617|d1`
- 015: `v15-pc9-p6-e6-b6-po8753152-res9048064-sv32768-x6-6-q1-2-t18616`
- 015.5: `v15-pr1 pool=4194304 blocks=258 alias_saved=112`
- 012-rev: `v12-rev levels=10 p1=6 p2=0 ctrl=6`
- 016: `v16|mc=8|L-0.41|-0.82|-0.41|amb=0.10|hp=2072056|hr=0.95|d1`

## Verdict

✅ **Rename officially complete.** 1314 replacements in 56 files + module/file/folder renames; zero `gotot` remains in tracked content or paths (one deliberate exception: the `GOTOML11` file magic, loader-validated — renaming it would break the 25 MB asset with no user-visible benefit).
