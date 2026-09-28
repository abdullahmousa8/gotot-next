# Contract 018-rev - Cone Data

**GNE-018-rev - DRAFT.** Cone record format and the four safety rules (borrowed, reviewed).

## Record (16 B)

- `vec4 axisCutoff`: `xyz` = cone axis (unit), `w` = `cos(halfAngle) = mindp`.
- `vec4(0)` = "never cull" sentinel (empty or incoherent cluster).

## Rules

- Cutoff = `sin(half) = sqrt(1 - mindp^2)`; NEVER `mindp` itself (agreement only at 45deg).
- `mindp <= 0.1` (normals spanning > ~168deg) => write `vec4(0)`; read side treats
  `w <= 0.1` as "skip the back-face test, keep the light".
- `axis` via furthest-pair seeding + two Ritter growth passes; `mindp` computed EXACTLY as
  min dot over the actual normal set.
- Cones are rewritten every frame (stale cones must never decide).
- Buffer: 3456 x 16 B = 55,296 B, keyed by the standard cluster index.