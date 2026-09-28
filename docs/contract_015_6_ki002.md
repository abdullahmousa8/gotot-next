# Contract 015.6 — KI-002 (async readback shares staging)

**GNE-015.6 — DRAFT.** Fix-only; no new features.

## Scope

- KI-002: async readback shares the sync staging buffer, so no speedup.
- Accepted resolution paths (in order): isolate staging buffers per
  frame; reduce bytes-per-frame; accept + document.

## Evidence required

- known_issues.md KI-002 entry updated with closure status.
- Before/after bytes-per-frame numbers if the reduce path is taken.
- Zero signature drift (full gates green before/after).

## Forbidden

- Godot engine patches.
- Claiming async speedup without measured bytes/timings.
- New hot-path allocations without a cap + counter.
