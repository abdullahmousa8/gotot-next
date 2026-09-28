# Contract 019 — Determinism

**GNE-019 — DRAFT.** 019 is DET-gated like every milestone since 013.
No PASS without a byte-identical d1/d2 signature.

## Owned by 019 (D7-6/D7-7/D7-8 FINAL)

- The `v19|lc|sm|cs|rd|hr|d` signature (D7-7): sm = bound shadow lights,
  cs = total casters, rd = real-depth flag 1/0, hr = histogram range (2 decimals),
  d = DET flag. Canonical PASS target: `v19|lc=20|sm=5|cs=4|rd=1|hr=0.88|d1`.
- Bias constants frozen (D7-6): 0.001 + 0.005×tanθ + normal offset 0.02 —
  part of DET (same binary, same constants, same pixels).
- The 8 thresholds (§4, D7-8) are gate constants, not tunables.
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
