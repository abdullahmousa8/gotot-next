# Contract 018 — Cluster Grid

**GNE-018 — FINAL (D6-2 resolved: fixed 16×9×24 grid).**

## Grid (D6-2 FINAL)

- **16 × 9 × 24** = 3,456 clusters over 1920×1080 (120×120 px tiles).
- Depth: 24 exponential slices from near to far (exact split frozen at FINAL).
- Tile ↔ pixel mapping is pure arithmetic (no lookup table, no CPU).

## Rules

- Grid dimensions are compile-time constants for 018 (no dynamic resize).
- A cluster stores offset+count into the flat index list (D6-4); per-cluster
  light cap with a loud overflow counter (D6-4).
- Depth slices derived from the SAME projection used for raster (no second
  camera, no drift source).
- Changing resolution changes tile pixel size, never cluster COUNT
  (DET stability across the fixed 1920×1080 harness).
