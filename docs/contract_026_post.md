# Contract 026 — Post-Process: Bloom + Tone Mapping (DRAFT)

## Scope
- Bloom (threshold + intensity, pending D11-3) and tone mapping (ACES vs
  Reinhard, pending D11-1) for main_026.
- Reads the KI-017 HDR attachment (pre-tonemap); writes the displayed chain.

## Requirements (proposed)
- Bloom active and threshold-gated (criterion 3).
- Exactly one tone map applied (criterion 4); the choice is recorded in the
  signature (pending D11-5).
- Deterministic across d1/d2; FVD-style HDR ratios must not drift vs the
  field series (Convergence Delta reporting stays available).

## API (proposed)
- gpu_post_process_set(bloom, tonemap).

## Non-goals
- AA/TAA (deferred); full post chain; HDR display output.
