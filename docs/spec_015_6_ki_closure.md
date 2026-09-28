# GNE-015.6 — KI Closure Sprint (SPEC v1.0)

**Status:** DRAFT v1.0.
**Depends:** 015.5 (PARTIAL).
**Unblocks:** 018 (Lighting).

## 1. Objective

Close KI-001, KI-002, 015.5 C6.

## 2. Background

- KI-001: GPU timestamps NA.
- KI-002: Async readback shares staging.
- 015.5 C6: Frame-time drift (81.8% readback).

## 3. Design

### 3.1 KI-001: GPU Timestamps

**Options:**
- (a) Godot engine patch (rejected).
- (b) Custom timestamp via CPU timing + sync.
- (c) Accept NA + document.

**Resolved:** (c) Accept NA + Document.
**Fallback:** (b) CPU timing labeled as "wall-clock", never "GPU".
**Limitation:** All numbers from 015.6 onward are wall-clock only.

### 3.2 KI-002: Async Readback

**Resolved:** Accept + Document.

**Rationale:** staging is engine-owned; cannot isolate.
Reduce-size breaks DET. Future milestone may explore double-buffering.

### 3.3 015.5 C6: Drift

**Resolved:** Accept + Document (double-buffering deferred to 020).

**Rationale:** "reduce size" breaks DET.
Double-buffering needs separate milestone.

## 4. Acceptance Criteria (5)
1. KI-001 status updated.
2. KI-002 status updated.
3. C6 status updated.
4. No signature regression.
5. Documented in known_issues.md.

## 5. APIs
- None (fix-only).

## 6. Out of Scope
- 018 (Lighting).
- New features.

## 7. Deferred
- Real fix for KI-001 (if (c)).
- Real fix for KI-002 (if (c)).

## 8. Deliverables
- Updated known_issues.md.
- §34 in progress_report.
- Verification evidence.

## 9. State
DRAFT v1.0.

## 10. Risks
- Fix may break signatures.
- Fix may not be measurable.
