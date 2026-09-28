# Contract 015.6 — KI-001 (GPU timestamps NA)

**GNE-015.6 — DRAFT.** Fix-only; no new features.

## Scope

- KI-001: GPU timestamp queries return no data on the Vulkan backend
  (engine-side counter never published; out of GNE scope to patch).
- Accepted resolution path: (c) accept NA + document, with (b) CPU
  timing + sync as fallback evidence where a number is required.

## Critical Limitations

- All performance numbers from 015.6 onward are wall-clock only.
- No claims of "GPU speedup" without GPU timestamps.
- This affects 018 (Lighting) and 019 (Shadows).

## Fallback (b) — Rules

- CPU timing + sync.
- Clearly labeled as "wall-clock", not "GPU".
- Only used when a number is required for regression detection.
- Never published as a GPU performance metric.

## Future

- If Godot publishes timestamps (upstream fix) → revisit.
- Otherwise → accept as permanent limitation.

## Evidence required

- known_issues.md KI-001 entry updated with closure status.
- Any wall-clock fallback measurement labeled as such (never presented
  as GPU time).
- Zero signature drift (full gates green before/after).

## Forbidden

- Godot engine patches.
- Presenting CPU timings as GPU timestamps.
- New buffers/passes in the hot path for this item.
