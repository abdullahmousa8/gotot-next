# GNE — Known Issues Closure Plan

## Active Known Issues

| KI | Title | Target Milestone | Deadline | Status |
|---|---|---|---|---|
| KI-001 | GPU timestamps NA | 015.6 | Before 018 | CLOSED (015.6): NA accepted + documented; wall-clock workaround; upstream-dependent |
| KI-002 | Async readback shares staging | 015.6 | Before 018 | CLOSED (015.6): accepted + documented (engine-owned staging) |
| KI-003 | Presentation optimization no effect | 020 | Before Production | CLOSED via 020: root in engine; mitigation measured (spec_020); zero-copy stays future |
| KI-007 | HZB uses AABB occluders | 019 | Before 020 | CLOSED via 019 (real depth, rd=1) |
| KI-011 | Specular not gated by NdotL (pre-existing) | Unassigned | Owner decision | CLOSED 2026-09-29 (fix executed; literals unchanged; goldens re-baselined) |
| KI-012 | Demo window size externally mutable (pre-existing) | Unassigned | Owner decision | CLOSED 2026-09-30 by decision (accepted and documented; nothing fixed) |
| KI-013 | gt_regress FAILED reset by XFAIL scene (harness) | Unassigned | Owner decision | Resolved (commit 6bf851a) |
| KI-014 | Cluster lists under-cover corner pixels (linear vs euclidean slice) | 018-rev (fixed behind rev flag) | Before R1 | FIXED (gated) + verified in R1 |
| KI-015 | Hardware RT unusable: RT pipeline creation fails (fork-level) | Unassigned | Owner decision | Open - needs engine-level diagnosis |`n| KI-018 | Measuring-shell artifacts (ghost ERRORs, hung runs, no bat timeouts) | Unassigned | Owner decision | Open (2026-10-01) - environment limitation; verdicts from owner shell only |

## Closure Schedule

### 015.6 — KI Closure Sprint

**Target:** After 017, before 018.
**Scope:**
- KI-001 (GPU timestamps) — fix or workaround.
- KI-002 (staging) — fix or accept.
- 015.5 C6 (drift) — resolve.

### 019 — Real Depth HZB

**Target:** After 018.
**Scope:**
- KI-007 (real depth occlusion).

### 020 — Presentation Overhaul

**Target:** After 019.
**Scope:**
- KI-003 (presentation).
- Kickoff: `docs/spec_020_presentation_overhaul.md` (DRAFT v0.1, 2026-09-28; D9 defaults adopted).
- Closed 2026-09-28: units 1-3 + C6 experiment complete - see progress_report section 38.

## Rule

- No KI older than 3 milestones.
- Each KI has a target + deadline.
- If missed → Architect review.
