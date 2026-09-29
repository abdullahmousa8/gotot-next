# GNE Workflow v2 - three tracks (adopted 2026-09-29, owner decision)

**Rules**
1. SPECs protect architecture. Tests protect code. Benchmarks protect performance.
   Reports record evidence. - not "every change needs a new SPEC".
2. A change that does not alter architecture, a public contract, or an acceptance
   contract needs no RFC/SPEC; a short design note is enough.
3. If a check can be an automated regression, it must not stay a repeated human
   ceremony.

**Tracks**
- ARCHITECTURE (RFC/SPEC -> prototype -> benchmark -> decision -> implementation ->
  regression): new subsystems, contracts, acceptance changes.
- FEATURE (issue -> design note -> implement -> targeted tests -> GT_REGRESS ->
  benchmark if relevant -> merge). Example: 11-R1 (FP16 -> FP32).
- BUGFIX (reproduce -> root cause -> minimal fix -> regression test -> affected
  suite -> merge): no full SPEC unless the bug exposes an architecture problem.

**Kept from v1 (values unchanged):** evidence > claims; tests > claims;
measurements > estimates; deterministic signatures; historical regression;
scope firewall for architecture milestones; red-team for sensitive changes;
research lab for hypothesis-driven work.

**GNE Continuous Verification System (CVS, v1):**
`tools/gne_verify.ps1` - one command: build (optional) -> gt_regress -> the 7
literal-signature gates (016/017/018/018-rev/019/020/021) -> GI checks
(022 gate2 + shade) -> error scan -> summary + history row. Baseline literals:
`tools/verify_baseline.txt` (change only with a recorded decision). History:
`tools/verify_history.tsv`. Exit code 0/1.
Deliberately excluded (research/frozen artifacts): main_022_loop (section-11
official state is FAIL under the frozen criterion - not a CVS gate).
## Golden-Update Protocol (added 2026-09-29, Architect directive; first precedent: KI-011)

A deliberate golden-baseline update (tools/golden/*.png) is permitted ONLY when ALL
THREE conditions hold:

1. **Known root cause, pre-documented** - a closed KI or a complete diagnosis exists
   in the record BEFORE the update is proposed (KI entry or equivalent diagnosis doc).
2. **Exact signature match** - the observed pixel difference (direction AND magnitude
   AND location) matches the predicted defect signature precisely - not "some
   difference". Example (KI-011): 5 px per scene, max 1 LSB, all darker = the exact
   backfacing-specular-removal signature.
3. **Deterministic reproduction** - re-running the scene produces the NEW golden
   byte-identically (the fix itself is deterministic; no luck involved).

The update is then recorded in the commit + progress doc with the evidence. Without
this written rule, a golden update can become cover for any unexplained deviation -
the difference between an excellent record and that risk is the rule being written
in advance, not discipline assumed to repeat by luck every time.

First precedent: KI-011 closure (commit 0ae1343): 12/12 signature literals unchanged;
goldens re-baselined with 5px/1LSB/darker evidence per scene; recheck byte-identical.
The BUGFIX/FEATURE fast path is approved for wider use on similar cases (one real-case
trial, not theory).