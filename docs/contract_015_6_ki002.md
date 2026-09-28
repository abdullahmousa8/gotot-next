# Contract 015.6 — KI-002 (async readback shares staging)

**GNE-015.6 — DRAFT.** Fix-only; no new features.

## Scope

- KI-002: async readback shares the sync staging buffer, so no speedup.
- Staging buffers owned by Godot RenderingDevice (engine-level).
- Cannot be isolated from GNE side.

## Resolved Path

**Chosen:** Accept + Document.

**Rationale:**
- Staging buffer management is Godot-internal.
- "Isolate buffers" would require engine patch (forbidden).
- "Reduce bytes" would change pixel content (violates DET).
- Therefore: accept + document + workaround in future milestone.

**Workaround:**
- Wall-clock measurement clearly labeled.
- No claims of async speedup without bytes/timing proof.
- Future milestone may explore double-buffering.

## Evidence required

- known_issues.md KI-002 entry updated with closure status.
- Before/after bytes-per-frame numbers if the reduce path is taken.
- Zero signature drift (full gates green before/after).

## Forbidden

- Godot engine patches.
- Claiming async speedup without measured bytes/timings.
- New hot-path allocations without a cap + counter.
