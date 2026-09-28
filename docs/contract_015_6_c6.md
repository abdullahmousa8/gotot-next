# Contract 015.6 — C6 (frame-time drift)

**GNE-015.6 — DRAFT.** Fix-only; no new features.

## Scope

- 015.5 C6: raster+output = 81.8% of frame time (readback staging root).

## Resolved Path

**Chosen:** Accept + Document (double-buffering deferred).

**Rationale:**
- "Reduce readback size" changes pixel content
  ⇒ violates DET preservation.
- Double-buffering is feasible but needs separate milestone
  (touches presentation path).
- Therefore: accept + document + schedule double-buffering for 020.

**Evidence for 015.6:**
- Baseline measurement (median/p95/max) on current scene.
- Bytes-copied-per-frame (current).
- Label as "known limitation", not "fixed".

## Evidence required

- Frame-time report (median/p95/max) before AND after, same scene.
- Bytes-copied-per-frame before AND after.
- known_issues.md C6 status updated; §34 in progress_report.
- Zero signature drift (full gates green before/after).

## Forbidden

- Optimizing without a baseline measurement first.
- Publishing ratios without raw numbers.
- Changing pixel content (DET signatures must stay literal).
