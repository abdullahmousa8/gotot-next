# GNE — Known Issues Closure Plan

## Active Known Issues

| KI | Title | Target Milestone | Deadline | Status |
|---|---|---|---|---|
| KI-001 | GPU timestamps NA | 015.6 | Before 018 | Scheduled |
| KI-002 | Async readback shares staging | 015.6 | Before 018 | Scheduled |
| KI-003 | Presentation optimization no effect | 020 | Before Production | Deferred |
| KI-007 | HZB uses AABB occluders | 019 | Before 020 | Scheduled |
| KI-011 | Specular not gated by NdotL (pre-existing) | Unassigned | Owner decision | Open |

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

## Rule

- No KI older than 3 milestones.
- Each KI has a target + deadline.
- If missed → Architect review.
