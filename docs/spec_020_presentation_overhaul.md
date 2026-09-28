# GNE-020 - Presentation Overhaul (Readback Bytes + Staging) - SPEC DRAFT v0.1

**Status:** DRAFT v0.1 (2026-09-28) - awaiting Architect decision set D9 (safe defaults adopted for unit 1 - section 11). No module changes yet: unit 1 is the baseline measurement scene/harness only.
**Depends:** 015.5 (Resource Pool), 015.6 (KI sprint), 009 (raster readback), 018 / 018-rev / 019 gates.
**Closes (planned):** KI-003 follow-up (presentation cost floor) + 015.5 C6 double-buffering item (deferred here by contract_015_6_c6).
**Scope guard:** presentation / readback path only. No engine edits (KI-001 / KI-002 / KI-003 consistency rule). No pixel-content change in any existing gated path: the reduced-resolution output lands only behind a new opt-in flag + new scene, so every legacy literal (v18, v18-rev, v19, ...) stays byte-exact.

## 1. Objective

Make per-frame presentation readback cheaper and measurable:
(a) reduce `bytes_copied_per_frame` via a lower-resolution presentation path (the route named in KI-003), and
(b) alleviate the per-frame full flush-and-stall behavior via staging reuse / double-buffering at the module level (the C6 item parked here),
then report median/p95/max wall-clock + byte accounting before/after (015.6 evidence format; timings are evidence, never a gate).

## 2. Background

### 2.1 KI-003 in one paragraph (investigation already closed 2026-09-27)
- Phase 5.1 tried three presentation optimizations: (A) half-resolution read; (B) frame skipping; (C) `Image` reuse.
- (A) was impossible without new C++ (`texture_get_data` reads the full 1920x1080 target; no sub-region) and changes presented pixels; (B) and (C) do not touch the copied bytes, which are the real cost.
- Measured root: every readback frame rides `STAGING_REQUIRED_ACTION_FLUSH_AND_STALL_ALL` inside the engine's `download_staging_buffers`; the process is "waiting, not computing" (probe: ~+3 CPU seconds per 28 wall seconds); measurement budgets could not complete, so no numbers and no claims.
- Owner decision (2026-09-27): close Phase 5.1 with no commit; the only effective lever is reducing the copied bytes themselves = a lower-resolution render path = an independent milestone. No engine edits.
- KI-003 "Next": a later independent milestone to reduce `bytes_copied_per_frame` via a lower-resolution render path. [= this spec]

### 2.2 C6 (contract_015_6_c6)

- 015.5 C6: "Accept + Document; double-buffering scheduled for 020" (touches the presentation path; needs its own milestone).
- Evidence format fixed by the contract: frame-time median/p95/max before AND after (same scene) + `bytes_copied_per_frame` before AND after + zero signature drift.
- Forbidden in the contract: optimizing without a baseline first; publishing ratios without raw numbers; changing pixel content (DET signatures stay literal).

### 2.3 Current baseline (from 015.6, for before/after anchoring)

- main_016 whole-process wall-clock, 5 runs (seconds): 8.76 / 7.58 / 8.19 / 7.57 / 7.41; median 7.58, p95 = 8.76, max 8.76, min 7.41 (all exit=0).
- Bytes per run: 7 reads x 8,294,400 = 58,060,800 B (~55.4 MiB; pixels only, no depth).
- Labeled explicitly: wall-clock process time, NOT GPU time (KI-001 rule).

## 3. Design direction (proposed; final at D9)

### 3.1 Lower-resolution presentation path (D9-1)

- Add a module-side presentation target at reduced resolution (size TBD at D9; e.g., 960x540 = 4x fewer bytes, or 1280x720 = 2.25x).
- The new path is opt-in: new flag + new scene. The existing 1920x1080 readback used by the gated scenes stays untouched (`RASTER_TARGET_W/H` is evidence-critical at 9 sites per the KI-003 audit; those constants are not edited).
- New constants get their own names; no existing site changes.

### 3.2 Staging double-buffering (D9-2)

- The C6 parked item: rotate/hold readback targets across frames so consecutive readbacks do not always hit the full stall pattern; measurable effect is what counts (exact instrument choice at D9).
- Must stay engine-edit-free; module-level resources only.

### 3.3 Measurement surface (D9-3)

- Existing instruments: `bytes_copied` (from `gne_pool_bytes_copied` in the stats dict), whole-process wall-clock timings (KI-001 workaround).
- Proposed additions: a dedicated scene + gate for 020 measuring (i) bytes on both paths, (ii) wall-clock distribution over a fixed loop; reported as median/p95/max with same-binary noise-floor runs (standard method).

### 3.4 Signature (D9-4 proposal)

- New gate signature `v20|...` carrying at least: target size, bytes ratio of the two paths, readback count, DET marker. Exact fields at D9.

## 4. Acceptance criteria (skeleton; thresholds at D9)

| # | Criterion | Method sketch |
|---|---|---|
| P1 | New path works | new scene renders and presents through the reduced path; its own checks calibrated before gating |
| P2 | Bytes reduced | `bytes_copied` on the new path vs the full path; ratio >= [D9] (4x expected from a 960x540 target); raw numbers published |
| P3 | Zero drift | full sweep green; every legacy literal byte-intact (v18, v18-rev, v19, ...) |
| P4 | Evidence format | median/p95/max + bytes before/after, same scene; same-binary noise-floor runs |
| P5 | Honest accounting | KI-003 follow-up + C6 status updated: exactly what improved and what did not |
| P6 | Hygiene | rc 0/0, no leaks (KI-009 / KI-010 checks), no timings inside signatures (015.5 rule) |

## 5. Boundaries

- Owns: presentation / readback path in `modules/gne_render`; new demo + harness; contracts; docs for 020.
- Never: engine edits (KI-001 / KI-002 / KI-003 rule); existing gated pixel paths; the `RASTER_TARGET_W/H` contracts of 009 / 010; GI (future milestone, see scope ruling); KI-012 (window size); vsync / present-mode policy; RHI redesign.

## 6. Out of scope

- GI (no design exists; remains a later milestone).
- GPU timestamps (KI-001; engine-blocked; wall-clock stays).
- Production presentation pipeline (this is the prototype-track presentation fix).

## 7. Decision items (D9) - for Architect

- D9-1: reduced target size(s) - 960x540 (4x) vs 1280x720 (2.25x) vs pick-at-runtime.
- D9-2: double-buffering in scope for 020 (C6 item) - depth 2, rotation policy, and which stall metric is accepted as evidence.
- D9-3: measurement surface - fixed frame-loop in the new scene vs whole-process runs; noise-floor run count.
- D9-4: signature fields for the new gate.
- D9-5: acceptance threshold for the byte ratio (e.g., >= 2x on the new path) and whether P2 is a gate or evidence-only.
- D9-6: flag / naming defaults (`gne_present_lowres_enabled`, default false until proven).

## 8. Risks

- The wait may be moved, not removed (stall pattern shifts; p95 may still spike) - honest reporting required.
- Accidental changes to gated scenes - mitigated by the opt-in flag + full sweep.
- Measurement noise on a shared desktop - same-binary noise-floor method (standard).
- The engine-internal floor may cap wins - KI-003 already labels this the only effective lever; results must be reported as measured, not as hoped.

## 9. Scope ruling (recorded 2026-09-28, executor under delegated authority)

- 020 = Presentation Overhaul. Basis: ki_closure_plan section "020 - Presentation Overhaul" (KI-003 target + deadline); contract_015_6_c6 ("double-buffering scheduled for 020"); KI-003 "Next" (independent milestone: reduce bytes via a lower-resolution path).
- "GI (020+)" references in the 018 / 019 boundary contracts are read as "a later milestone": GI has no spec and no design. GI stays future and will need its own spec + decision set.
- If the Architect schedules GI at 020 instead, this draft is parked (no code touched).

## 10. Deliverables (planned)

- Module changes (reduced-res present path; staging rotation) behind flags.
- main_020 + tools/gt_020a (two runs, byte-compare, evidence dump).
- 5 contracts; evidence note (before/after raw numbers); closure doc + progress section.
## 11. Adopted defaults + recorded baseline (executor, delegated authority, 2026-09-28)

Adopted so unit 1 could proceed without blocking. All reversible; subject to Architect review.
- D9-1 = 960x540 primary reduced target (4x fewer bytes - the KI-003 evidenced route).
- D9-2 = double-buffering in scope; depth 2; effect measured through the same loop (stall-sensitive wall distribution).
- D9-3 = fixed loop in main_020: warm-up 10 + measured 60 frames; vsync DISABLED for the loop (measurement mode, documented in the scene); same-binary noise-floor runs (both harness runs).
- D9-4 = gate signature `v20|tw|th|rb|rf|d1` (deterministic fields only; no timings inside).
- D9-5 = P2 gate threshold >= 2x byte reduction on the reduced path (expected 4x); timings stay evidence-only.
- D9-6 = env flag `GNE_PRESENT_LOWRES` + feature-detected `gpu_present_lowres_set()` / `gpu_present_read_pixels()` / `gpu_present_info()`; default off / full raster.

Baseline (unit 1: `main_020` + `tools/gt_020a.bat`, this build; wall-clock, evidence-only):
- sig d1 == d2: `v20|tw=1920|th=1080|rb=8294400|rf=60|d1`; rc 0/0; zero ERROR / RID lines.
- p50 per run: 14644 / 14800 us; wall_avg: 15108 / 15878 us; p95: 18422 / 17866 us; run wall: 1452 / 1565 ms.
- Readback accounting: 70 reads x 8,294,400 B = 580,608,000 B per run (scene-side count; the module pool `bytes_copied` counter is currently never incremented - observed; not used for this measurement).
- Note: vsync clamps frames at ~16.6 ms unless disabled; the scene disables it for the measurement loop.