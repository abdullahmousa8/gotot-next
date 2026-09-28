# Contract 018-rev - Cone Data

**GNE-018-rev - FINAL.** Cone record format and the safety rules (borrowed, reviewed).

## Record (16 B)

- `vec4 axisCutoff`: `xyz` = cone axis (unit), `w` = `cos(halfAngle) = mindp`.
- `vec4(0)` = "never cull" sentinel (empty or incoherent cluster).

## Rules

- Cutoff = `sin(half) = sqrt(1 - mindp^2)`; NEVER `mindp` itself (agreement only at 45deg).
- `mindp <= 0.1` (normals spanning > ~168deg) => write `vec4(0)`; read side treats
  `w <= 0.1` as "skip the back-face test, keep the light".
- Exact-set guarantee: `mindp` is the exact min over the actual normal set. Sub-sampling
  that could under-cover the set is forbidden; a bounded reducer that cannot hold the set
  must fall back to the never-cull sentinel.
- Deterministic construction: fixed-order reduction for the axis; the min pass is
  order-independent (no timers, no hash order, no uninitialized memory).
- Cones are rewritten every frame (stale cones must never decide). Source age <= 1 frame
  is gated (R7).
- Buffer: 3456 x 16 B = 55,296 B, keyed by the standard cluster index.