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
## 4. Results (frozen scene, official runs, 2026-09-28)

Scene revision: `main_021` construction v3; gate: `tools/gt_021a.bat` (two runs).
Signature d1 == d2 (byte-identical): `v21|lc=256|of=36097|sl=38620|on=38434|d1`;
rc 0/0; zero ERROR / RID lines.

| state | touched | assignments | overflows |
|---|---|---|---|
| off (baseline, known under-covering corners) | 3132 | 36097 | 9994 |
| slab (KI-014 corrected base) | 3140 | 38620 | 12447 |
| cone-on | 3140 | 38434 | 12311 |

- dc (corrected-base cone savings) = (38620 - 38434) / 38620 = **0.48%** - 20x below the
  pre-registered 10% threshold.
- Net off-vs-on = **-6.47%** (the slab correctness cost outweighs the cone savings on
  this scene).
- Occupancy achieved: ge8 = 165/280 (59%), ge12 = 142 (51%), ge16 = 116 (41%) of the
  sampled near/mid clusters (median approx. 12-15 = the pre-registered "typically
  8-16" band).
- Cone validity on the raster: 15 of 280 sampled clusters (5.4%) carry a valid cone; the
  rest have mixed normal sets (sentinel / never-cull). This is the structural reason the
  savings are small on a street-grid city: most clusters mix ground/walls/background.
- Cap pressure recorded: overflows 9994 / 12447 / 12311 - the 16-cap saturates heavily
  in the dense field; no cap changes made.

**Decision (section 1.1 applied literally):** dc 0.48% < 10% => the default STAYS OFF;
spec_018_rev section 12 updated; the carried dc >= 10% item closes as measured (not
met). No threshold was changed after results; no re-tuning.

Note: no contracts x5 were added for this milestone - it is a measurement milestone;
the binding artifacts are the frozen pre-registration (section 1) and this results
section. [Flagged for the Architect if contracts are still wanted.]