# Contract 018-rev - Cull Rules

**GNE-018-rev - FINAL.** The back-face prefilter inside `gpu_light_cull_glsl`.

## Test (exact)

- `toLight = lightPos - clusterAnchor; skip the light if
  dot(normalize(toLight), coneAxis) < -sqrt(1 - w*w)` with `w > 0.1` (rule: operand
  order matters; swapped operands cull the lit side - silent wrong output).
- Placement: prefilter BEFORE the existing `sphere_vs_aabb` hit test (lab-validated order).
- Skipped lights are simply not appended; the append path (atomic slot, sorted order,
  overflow counter) is untouched.

## Forbidden

- Culling when `w <= 0.1` or when the cone buffer is absent/invalid (no silent fallback).
- Changing append order or overflow semantics; any reordering of surviving ids.
- CPU-side culling in the hot path (GPU-driven only).