# Contract 015.6 — C6 (frame-time drift)

**GNE-015.6 — DRAFT.** Fix-only; no new features.

## Scope

- 015.5 C6: raster+output = 81.8% of frame time (readback staging root).
- Accepted resolution paths: reduce readback size; mitigate via
  double-buffering; accept + document with measured numbers.

## Evidence required

- Frame-time report (median/p95/max) before AND after, same scene.
- Bytes-copied-per-frame before AND after.
- known_issues.md C6 status updated; §34 in progress_report.
- Zero signature drift (full gates green before/after).

## Forbidden

- Optimizing without a baseline measurement first.
- Publishing ratios without raw numbers.
- Changing pixel content (DET signatures must stay literal).
