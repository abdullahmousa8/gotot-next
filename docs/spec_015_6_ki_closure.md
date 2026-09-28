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

**Decision:** (c) with fallback (b) where possible.

### 3.2 KI-002: Async Readback

**Fix:**
- Isolate staging buffers per frame.
- Or: reduce bytes-per-frame.
- Or: accept + document.

### 3.3 015.5 C6: Drift

**Fix:**
- Reduce readback size.
- Or: mitigate via double-buffering.

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
