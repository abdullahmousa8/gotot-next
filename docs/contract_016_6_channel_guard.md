# contract_016_6_channel_guard — the redundant `tia < 8` guards

Status: **ACTIVE (2026-09-30). Option (a) executed.** The three always-true guards
`if (tia < 8)` / `if (tir < 8)` / `if (tin < 8)` are removed from
`gne_render_server.cpp`. The adopted bound **`0..4` is unchanged** — it lives in
the outer range test, which was never touched.

## 1. What is actually in the code, and what is not in it

Three material channels each read the texture array in a nested pair of guards
(`cpp:2055/2062/2069`, previously cited as `1842/1849/1856` — stale, the lines
moved):

```glsl
if (m2a.x >= 0.0 && m2a.x <= 4.0) {   // outer: the adopted bound
    int tia = int(m2a.x + 0.5);
    if (tia < 8) {                      // inner: always true
        vec3 ca = texture(tex_arr[tia], ...);
        alb = albedo * ca;
    }
}
```

The inner guard is **provably always true**. For `tia >= 8` we would need
`m2a.x + 0.5 >= 8`, i.e. `m2a.x >= 7.5`, while the outer guard requires
`m2a.x <= 4.0`. Contradiction — so `tia >= 8` is unreachable. The same holds for
`tir < 8` and `tin < 8`.

**Therefore the inner guard is not what excludes channels 5..7 — the outer
`<= 4.0` range test is, and it says so in the source, in plain sight.**

## 2. This is not a new finding, and the mis-framing has a traceable history

`contract_016_5_channel_bounds.md` already got this right. Its §"Dead logic" says:

> `tia < 8` / `tir < 8` / `tin < 8` are **unreachable-false**: the outer
> `m2a.X >= 0.0 && m2a.X <= 4.0` already forces the index into {0..4}, so the
> inner bound is always true when reached. Note this is the **opposite** of a
> guard that permits 8 positions — it never admits five.

An independent re-derivation during this review reached the identical conclusion,
which is corroboration rather than discovery. **What was wrong was the
classification, not the analysis** — and the wrong classification is what
propagated:

| Where | It says |
|---|---|
| `contract_016_5` line 4 | "Dead `tia < 8` remains **DEFERRED technical debt**" |
| `contract_016_5` lines 82-83 | "stays registered as **deferred technical debt**" |
| summaries since | "channels 5..7 are loaded and measured but silently skipped" |

That last phrasing is **false in substance**, not merely loose: nothing is
skipped silently, nothing is loaded-and-discarded, and the bound that excludes
them is the visible, deliberate `<= 4.0`. The register and the summaries carried it
into Gemini's direction, and Gemini then re-asserted it back to me. It was wrong
in the register before I ever saw it.

## 3. So what is the actual problem, stated minimally

1. A branch that can never be false, in three places.
2. A name (`tia < 8`) that suggests an 8-wide intent the code does not have —
   and `016_5` already notes this is *suggestive, not a decision*.
3. A bookkeeping error: dead logic filed as *debt* invites someone to "fix" it by
   widening the bound, which would be a feature change wearing a fix's clothes.

**There is no runtime defect. There is no memory-safety issue. There is no
correctness risk.** The cost is a redundant branch and a misleading name.

## 4. Options, and why only one of them is a fix

| | change | verdict |
|---|---|---|
| **(a)** | delete the three inner guards | **the only genuine cleanup.** Provably zero behaviour change — the branch is always true, so removing it cannot alter any result. |
| **(b)** | widen the bound to `0..7` | **not a fix, a feature.** It changes how every material renders and needs its own numeric and visual contract. Filing it as debt cleanup is how a behaviour change sneaks in unverified. |
| **(c)** | leave it | leaves a misleading name in the source. |

**Recommended: (a).**

## 5. Acceptance for the implementation (to be met, not asserted)

Because the deleted branch is always true, the change is *provably* inert. The
verification therefore has to be able to fail, or it proves nothing:

1. **All existing sigs byte-identical** — `gt_016a`, `gt_017a`, `gt_018a`,
   `gt_018a_rev`, `gt_019a`, `gt_020a`, `gt_021a`, `mat_v2_x1`,
   `mat_channel_cluster`, `frustum_cull`, `cluster_overflow`, `gi_shade`.
2. **`main_022_shade` cluster-id dumps byte-identical** — spot and point columns,
   all 24 slices, cumulative against the pre-split baseline. This is now the
   project's standard for "no visible behaviour change".
3. **`no_errors` PASS** and **`provenance` PASS, `module_dirty == 0`**.
4. **A negative control**, because a change proven only by "nothing changed"
   cannot detect its own no-op. The same control that caught the perf020 basis
   bug applies: verify the *gate* can still fail, not only that it passes.
5. `contract_016_5` corrected in the same commit: drop "deferred technical debt"
   from lines 4 and 82-83, fix the stale `1842/1849/1856` citations, and point
   here.

## 6. Standing note

`GNE_TEX_ARRAY = 8` is the real array size and is unrelated to the bound. The
adopted contract is `0..4` (`contract_016_5`). Nothing here reopens that
decision; option (b) would, and is explicitly out of scope.
