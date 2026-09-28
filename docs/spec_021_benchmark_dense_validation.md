# GNE-021 - Benchmark City + Dense-Light Validation (SPEC DRAFT v0.1)

**Status:** DRAFT v0.1 (2026-09-28) - scope approved by Architect; the pre-registration
below is FROZEN before the first measurement run (this commit predates any 021 scene).
**Depends:** 018-rev (cone culling, flag OFF default), 019 (shadows), 020 (presentation).
**Closes (planned):** the carried 018-rev `dc >= 10%` scale retest + the default-state
flip decision for `light_cone_enabled` (spec_018_rev section 12) + the "benchmark city"
prototype-checklist item.

## 1. Pre-registered decision items (frozen BEFORE the first measurement run)

### 1.1 Flag-flip criterion (cone culling default)
The default flips from OFF to ON iff, on the benchmark city scene:
- **dc >= 10%**, where dc = CONE savings on the CORRECTED base = (slab assignments -
  cone-on assignments) / slab assignments, measured on the same scene revision with the
  three-state run (off / slab-only / cone-on); AND
- the flag-on frame time does not regress beyond the same-session noise floor
  (median within +5%; same-binary noise-floor method).

Otherwise the flag stays OFF and the result is recorded.

Rationale: 10% is the already-contracted provisional figure (D8-rev-5, "initial;
measured-at"), revived as the pre-registered flip threshold - not a new fitted number.
Consequence: a dc >= 10% run executes a one-line default flip + spec_018_rev section 12
update; a dc < 10% run keeps the flag off and closes the carried item as measured.

### 1.2 Benchmark City reference bounds
**256 dynamic lights** (192 point + 64 spot) = 1/4 of GNE_LIGHT_MAX (1024): substantial
headroom, no cap pressure; the count is NOT maxed, to avoid an artificially favorable
field. Layout: night street grid with the camera at street level inside the grid (three
fixed views: down-street / cross-street / corner). Construction target: near-field
clusters typically carry **8-16 lights** (the occupancy range where back-face culling
can act and the 16-cap saturates).

Any change to these bounds after the first measurement run invalidates the run and
restarts the unit.

## 2. Measurement discipline (frozen)
- No threshold changes after seeing results; no "winning number" edits.
- No cap changes (16/cluster), no engine edits, no cone-code changes during 021.
- Timings are evidence, never signatures (015.5 rule); dc is the decision number.
- Same-binary noise-floor runs; flag-off/flag-on pairs on the same scene revision.

## 3. Deliverables (planned)
- `main_021` scene builder + `tools/gt_021a.bat` (two runs, signature, dc measurement,
  occupancy stats).
- Results note: dc off/on, occupancy achieved, frame-time medians, the flag decision
  executed per 1.1.
- Contracts x5; progress section; closure doc.
