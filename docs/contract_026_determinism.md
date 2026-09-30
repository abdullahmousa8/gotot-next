# Contract 026 — Determinism (DRAFT)

## Scope
- Determinism + regression discipline for everything 0.26 adds.

## Requirements (proposed)
- v26 signature literal pinned (format pending D11-5); d1/d2 byte-identical,
  in-process determinism byte-equal where loops exist.
- Full 25-row belt green in the owner shell AFTER 0.26 lands (criterion 8) -
  including row 25, whose first owner-shell verdict is still outstanding.
- Zero ERROR/RID lines in the owner shell; measuring-shell artifacts stay
  classified under KI-018 and never gate.
- Provenance re-stamped on any C++ change; perf020 basis re-measured if the
  frame path is touched.

## Non-goals
- Redefining existing literals (016/017/018/019 signatures immutable).
