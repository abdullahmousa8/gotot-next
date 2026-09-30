# GNE-0.26 — Visual Slice + Scene Fix (SPEC v1.0)

**Status:** DRAFT v1.0 (2026-10-01). No implementation before Architect approval.

## 1. Objective
- Fix Country House scene bugs (M0.26a).
- Visual quality slice (M0.26b).
- KI-019 resolution (M0.26c).

## 2. Background
- M0.25 closed as logical PASS, formal verdict env-pending (row 25 wired +
  pinned; owner-shell belt outstanding).
- Visual baseline 5.9/10 and target 7.0/10 are STATED REQUIREMENTS, not
  measurements - no visual scoring instrument exists in the repo. Criterion 6
  gates at >= 6.5/10; the scoring instrument itself is TBD (Architect).
- "Country House" has NO record in demo/ or docs/ - scene identity, location
  and current bugs are TBD input (Architect). §3.1 items below presuppose it.

## 3. Design

### 3.1 M0.26a — Scene Fix (scene TBD - see §2)
- Roof separation: mesh / transform / z-fight.
- Back wall: mesh missing / culling.
- Path: alpha / texture bake.

### 3.2 M0.26b — Visual (all APIs PROPOSED - none exist; only the KI-017 HDR
attachment exists today)
- Sky + atmosphere: procedural gradient.
- Bloom: threshold + intensity.
- Tone mapping: ACES or Reinhard (D11-1).
- Contact shadows: PCF + bias.

### 3.3 M0.26c — KI-019 (opened with this SPEC - see known_issues.md)
- Option A: filter user:// noise at the harness layer, OR
- Option B: clean-environment protocol (owner shell as reference).
- Decision: Architect (affects every future gate's ERROR scan).

## 4. Acceptance Criteria (8)
1. Scene bugs fixed (against the TBD scene inventory).
2. Sky visible.
3. Bloom active.
4. Tone mapping applied.
5. Contact shadows visible.
6. Visual >= 6.5/10 (INSTRUMENT TBD - criterion is not operational until the
   scoring method is defined and registered).
7. DET stable (v26|... literal pinned).
8. Regressions 0.20-0.25 PASS (full belt green in owner shell, incl. row 25).

## 5. APIs (PROPOSED - additive, scene-driven; no engine commitment until approval)
- gpu_sky_set(params).
- gpu_post_process_set(bloom, tonemap).
- gpu_shadow_set_contact(params).

## 6. Out of Scope
- GI changes.
- RT (KI-015).
- Render Graph.
- Raster depth HZB (deferred).

## 7. Deferred
- Full visual overhaul.
- AA / TAA.

## 8. Deliverables
- C++ module changes (only after approval + provenance re-stamp).
- Demo (main_026).
- Harness (gt_026a).
- Contract docs x5 (shipped with this SPEC as DRAFT companions).

## 9. D11 Decisions Required (Architect - implementation blocked until answered)
- D11-1: Tone map (ACES vs Reinhard).
- D11-2: Sky model (gradient vs procedural).
- D11-3: Bloom threshold.
- D11-4: Contact shadows params.
- D11-5: Signature format.
- D11-6 (added by executor): visual scoring instrument for criterion 6.
- D11-7 (added by executor): Country House scene identity + bug inventory.

## 10. Risks
- Provenance (any C++ change re-stamps; perf020 basis re-measured).
- Visual quality subjectivity (compounded by the missing instrument - D11-6).
- Regression risk (25-row belt must stay green; row 25 itself still awaits its
  first owner-shell verdict).
