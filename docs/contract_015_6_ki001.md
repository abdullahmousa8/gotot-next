# Contract 015.6 — KI-001 (GPU timestamps NA)

**GNE-015.6 — DRAFT.** Fix-only; no new features.

## Scope

- KI-001: GPU timestamp queries return no data on the Vulkan backend
  (engine-side counter never published; out of GNE scope to patch).
- Accepted resolution path: (c) accept NA + document, with (b) CPU
  timing + sync as fallback evidence where a number is required.

## Evidence required

- known_issues.md KI-001 entry updated with closure status.
- Any wall-clock fallback measurement labeled as such (never presented
  as GPU time).
- Zero signature drift (full gates green before/after).

## Forbidden

- Godot engine patches.
- Presenting CPU timings as GPU timestamps.
- New buffers/passes in the hot path for this item.
