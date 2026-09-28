# Contract 019 — Determinism

**GNE-019 — DRAFT.** 019 is DET-gated like every milestone since 013.
No PASS without a byte-identical d1/d2 signature.

## Owned by 019

- The `v19|…` signature shape (fields frozen at SPEC FINAL; values measured).
- `gt_019a.bat` d1/d2 protocol: rc 0/0, PASS markers ×2, zero `ERROR:`,
  zero RID-leak lines (same checks as `gt_016a/017a/018a`).
- CSM blend factors, bias values, cascade splits: fixed constants in the
  gate scene (no wall-clock, no random, no uninitialized memory).

## Forbidden

- Any nondeterministic input to the shadow path (timers, thread ids,
  hash-order iteration, uninitialized padding in shadow records).
- Loosening the gate (fewer runs, fuzzy compare, threshold creep).
- Reusing 018's signature fields with changed semantics.

## Hand-off rule

- `SIG1 != SIG2` ⇒ FAIL, no discussion. Determinism is proven by the gate,
  never by reasoning about the code.
