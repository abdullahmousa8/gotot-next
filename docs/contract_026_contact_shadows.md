# Contract 026 — Contact Shadows (DRAFT)

## Scope
- Screen-space contact shadows (PCF + bias, pending D11-4) for main_026.
- Distinct from CSM shadows (019) and from HZB occlusion (012).

## Requirements (proposed)
- Contact shadows visible at geometry contact points (criterion 5).
- Bias bounded so flat lit surfaces stay clean (no acne by construction;
  proven by a flat-surface control cell, pattern of 016-channel work).
- Deterministic across d1/d2.

## API (proposed)
- gpu_shadow_set_contact(params).

## Non-goals
- Replacing 019 CSM; soft-shadow overhaul (see rfc_019_5).
