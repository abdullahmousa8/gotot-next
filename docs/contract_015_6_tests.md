# Contract 015.6 — Tests

**GNE-015.6 — DRAFT.** 5 criteria evidence; methods here, numbers frozen at FINAL.

## 5 criteria evidence

| # | Criterion | Method |
|---|---|---|
| K1 | KI-001 status updated | known_issues.md entry closed with evidence link |
| K2 | KI-002 status updated | known_issues.md entry closed with evidence link |
| K3 | C6 status updated | frame-time + bytes reports before/after, §34 written |
| K4 | No signature regression | full `gt_harness` + `gt_regress` green, all sigs literal |
| K5 | Documented | §34 + updated KI entries reference each other |

## Regression delta guard

- Every legacy signature byte-identical before and after any 015.6 change.
- `main_012 → XFAIL` preserved; 016 gate re-run green.

## Bugs fixed during bring-up

- (to be filled during implementation — 014-tests precedent.)
