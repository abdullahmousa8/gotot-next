# Contract 018-rev - Tests

**GNE-018-rev - DRAFT.** Acceptance evidence (SPEC section 4).

| # | Criterion | Method |
|---|---|---|
| R1 | No visual change | flag off vs on, static scene: pixel-identical byte compare |
| R2 | Savings | dc >= 10% of baseline assignments (initial; measured-at) |
| R3 | DET | d1/d2 byte-identical `v18-rev|...` |
| R4 | Regressions | full sweep green; every legacy literal byte-intact |
| R5 | Hygiene | zero `ERROR:`, zero leak lines; exit rc 0/0 |
| R6 | Evidence | before/after assignment counts in the report; no timings in sigs |

## Notes

- R1 is the primary gate; a specular-leak delta (see SPEC Risk 1) must be reported with
  numbers and decided by the Architect - never silently tolerated.
- The rev harness (`gt_018a_rev`) mirrors `gt_018a` conventions (d1/d2, sig compare,
  ERROR/leak scans).