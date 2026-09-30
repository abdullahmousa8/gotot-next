# Contract 026 — Sky (DRAFT companion to spec_026)

## Scope
- Sky + atmosphere for the 0.26 visual slice (main_026 scene).
- Procedural gradient model pending D11-2.

## Requirements (proposed)
- Sky visible in-frame under the 0.26 camera (criterion 2).
- Deterministic: byte-identical pixels across d1/d2 (feeds criterion 7).
- No interaction with GI field or HZB pyramid state.

## API (proposed)
- gpu_sky_set(params) - params defined at approval time.

## Non-goals
- Physically-based atmosphere; day/night cycle; weather.
