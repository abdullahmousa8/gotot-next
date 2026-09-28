# Contract 018-rev - Tests

**GNE-018-rev - FINAL.** Acceptance evidence (SPEC section 4).

| # | Criterion | Method |
|---|---|---|
| R1 | No visual change from culling (scoped) | flag off vs on, static scene: every differing pixel must lie in the cull-affected region (clusters whose light lists changed) AND carry the KI-011 leakage signature (all dropped lights NdotL <= 0 at that pixel); unexplained diffs fail as over-cull |
| R2 | Savings | dc >= 10% of baseline assignments (initial; measured-at) |
| R3 | DET | d1/d2 byte-identical `v18-rev|...` |
| R4 | Regressions | full sweep green; every legacy literal byte-intact |
| R5 | Hygiene | zero `ERROR:`, zero leak lines; rc 0/0 |
| R6 | Evidence | before/after assignment counts + dropped lists in the report; no timings in sigs |
| R7 | Staleness gate | cone source age <= 1 frame at every cull (engine-exposed); explicit FAIL otherwise |

## Notes

- R1 is the primary gate, amended per Architect directive A1: the specular leak is a
  pre-existing engine gap (KI-011, separate ticket) - R1 exonerates differences that carry
  its signature and fails everything else. Never silently tolerated.
- R7 is amended per Architect directive A2: staleness is a hard regression criterion, not
  documentation.
- The rev harness (`gt_018a_rev`) mirrors `gt_018a` conventions (d1/d2, sig compare,
  ERROR/leak scans).