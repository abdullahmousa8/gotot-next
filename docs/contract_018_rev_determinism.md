# Contract 018-rev - Determinism

**GNE-018-rev - DRAFT.** DET-gated like every milestone since 013.

## Rules

- Per-frame cone rewrite; no stale reads within a frame; deterministic inputs only.
- Skipped-light decisions are a pure function of (cone, light set, camera) - no timers,
  no hash order, no uninitialized memory.
- Signature `v18-rev|lc|cc|dc|ot|hr|d` (fields per SPEC 3.5; `dc` = dropped assignments).
- Harness: two runs, byte-identical `sig=`, zero `ERROR:`, zero leak lines (KI-010 checks).
- No timings inside the signature (015.5 lesson).

## Hand-off rule

- `SIG1 != SIG2` => FAIL, no discussion. Determinism is proven by the gate, never by
  reasoning about the code.