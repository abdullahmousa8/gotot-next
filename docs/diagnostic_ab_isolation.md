# A/B Isolation - Consolidated Record (2026-10-01)

## Sequence status
| Phase | State | Evidence |
|---|---|---|
| Phase 0: Godot Reference Scene | DONE (3e2e850) | godot_reference/ (vanilla D: binary); 9 ArrayMesh parts; Sun white, energy 0; ambient COLOR 1.0x0.1, no sky; LINEAR; glow/AA off; camera (0,300,1500)/rot0/fov60/300/6200; 1920x1080 fullscreen |
| Phase 1: Unlit | DONE - INVESTIGATE band | MAE linear mean 3.0 (R 3.86 / G 2.74 / B 2.39); darks 0.000; details below |
| Phase 2: Direct-only | PENDING (needs go) | Requires twin Sun rotation set to the canon dir (currently unset - irrelevant at energy 0) + GNE LIGHT_HOUSE run + diff |
| Phase 3: Environment-only | PENDING | GNE = LIGHT_HOUSE scene as-is is mostly ambient already; twin ambient-only run exists (it IS the Phase-1 twin config) - comparison reframed, not re-run |
| Phase 4: Linear EXR | PENDING | Both sides already linear-out (GNE: no tonemap in-shader; twin: LINEAR). EXR export adds nothing unless HDR values are compared - precondition: gpu_raster_read_hdr series vs twin HDR capture (method TBD) |
| Phase 5: Full cumulative | PENDING | Blocked on 2-4 |

## Mandatory mapping (GNE has no off-switches - literal OFF is impossible)
- "GNE LIGHT_CANON OFF, ambient OFF" EXECUTED AS: sun straight down
  (--unlitlight) + black specular -> frame is EXACTLY 0.1xALBEDO (ambient
  floor). There is no unshaded mode and ambient is hardcoded 0.1. The twin
  side is LITERALLY off (energy 0) + ambient 0.1. Asymmetry documented, not
  hidden: any residual confounds ambient-model, never color pipeline.
- "No tonemap difference": satisfied trivially (neither side tonemaps).

## Phase-1 results (the numbers that gate the rest)
- Framing: 99.4% overlap after vertical flip (GNE readback rows are
  bottom-up vs Godot top-down - convention recorded for all future diffs).
- Darks MAE 0.000: NO color-space issue. A colorspace shift would move darks.
- Lit MAE 3.0, bias +3.7/+2.3/+2.2 (GNE brighter, R strongest).
- Twin ambient-scale experiment: ~0.45x of naive albedo x energy
  (Godot-side model question). GNE side exact by construction.
- LIGHT_CANON (white 1.0) EXONERATED as a color source.

## Which phase introduces +12 R/G: UNKNOWN (that is what Phases 2-5 determine)
- Current hypothesis, ranked: (1) ambient stage - warm albedo x0.1 with no
  cool sky fill on either side, amplified by GNE-side brightness bias;
  (2) direct stage - wrong-face directional (outdoor tops at ambient);
  (3) tonemap stage - expected NIL (both linear).
- Phase 2 is the sharpest next test (white light both sides, no ambient
  ambiguity). Awaiting explicit go per STOP-after-each-phase.
