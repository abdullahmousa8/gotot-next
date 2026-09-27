# JEV Pilot — Synthetic Cases (Phase 0)

**الحالة:** 3 حالات اصطناعية جاهزة. **لم يُشغَّل JEV بعد** (Owner يُشغّله في Phase 1).
**الغرض:** قياس قدرة JEV على تشخيص原因 الجذري من **State فقط** (بلا وصول للكود أو السجل).
**قاعدة نزاهة:** كل حالة تحوي **السبب الجذري مخفياً** — لا يُكشف للـJEV ولا يُذكر في أي وثيقة تُعطى له.

---

## How the Owner runs the pilot (Phase 1)

For each case, give JEV **ONLY** the `## State` block (verbatim, nothing else):

1. Case 1 State
2. Case 2 State
3. Case 3 State

Record JEV's answer per case, then send the whole result sheet back. Scoring is in
`docs/jev_pilot_scoring.md` (three-tier criteria) — **evaluate after 5 real problems**,
not after these 3.

---

## Case 1 — "A gate that started lying"

### State
```
Project GNE, Godot 4.8.dev custom module, Windows.
A regression harness runs 10 test scenes. Each scene is executed TWICE and the
printed signature line ("sig=...") of both runs is compared. The harness reports
a scene as FAILED when the two lines differ.

Timeline:
- 2026-09-25: all 10 scenes green, 20 consecutive runs.
- 2026-09-27: one scene (main_008b) reports det=DIFF. Run A prints
  sig=v3966|36|3966|g26389|...   Run B prints
  sig=v3905|36|3905|g25765|...
  The difference is in the counts, not in a hash.
- Re-running the same scene manually, 3 times in a row: all three runs print
  v3905|36|3905|g25765|... (identical). The failure does not reproduce.
- The engine binary was not changed between 09-25 and 09-27.
- The scene under test builds 10,000 instances from a fixed seed, then advances
  a camera on a per-FRAME-INDEXED orbit (frame 1, frame 2, ... frame N). The
  scene code contains no use of frame delta time and no random calls.
- Harness change on 09-26 (one commit, tests only): the harness previously ran
  the two runs SEQUENTIALLY. It now starts the second run while the first
  process may still be finishing its GPU work (the harness waits for process
  exit, not for GPU idle).
```

### Hidden root cause (DO NOT REVEAL)
Not a code defect and not nondeterminism in the scene: the scene IS deterministic
per frame index. The failure is a **harness race** — the second run's engine
instance competes for the GPU with the first instance's still-executing passes,
and the scene's per-frame work is time/throughput dependent (how many frames
complete before the evidence frame is reached), so a slower/faster GPU
contention changes WHICH frame index the evidence lands on ⇒ different counts.
Evidence: the values are not random (v3905 is the historical value); the scene
visits a different evidence frame under contention.

---

## Case 2 — "Performance claim that measures the wrong thing"

### State
```
GNE has a render module. Milestone 015 reports:
  pool=8,753,152  aliased_saved=32,768
and a report section claims: "transient resource pool with aliasing saved
32,768 bytes".

Code inspection requested by the reviewer shows:
- rg_pool_bytes and rg_alias_saved are declared as `static int`.
- No RenderingDevice buffer_create / buffer_update call exists inside the render
  graph code path; rg_pool is a Vector<RgPoolSlot> of plain structs.
- gpu_rg_execute() increments two counters and prints one line. It calls no pass
  body, no compute dispatch, no draw.

The project owner asks: "we saved 32 KB with pool aliasing — is that real VRAM
saving, and can we cite it in the next milestone's benchmark?"
```

### Hidden root cause (DO NOT REVEAL)
The number is **pure CPU arithmetic on a graph model**, not memory. The aliasing
algorithm is correct *as an algorithm* (lifetime-disjoint slots are folded), but
it is never applied to any allocation, so there is no pool and no saving. The
milestone claim is a **model presented as an implementation**. The correct
answer must separate "the layout algorithm is sound" from "zero bytes are
actually saved", and must refuse to cite it as a VRAM benchmark.

---

## Case 3 — "Determinism gate that cannot ever pass"

### State
```
GNE milestone 011 verifies a scene three ways (strategy 0, 1, 2). The scene
prints one signature line per run.

The printed line is:
  GOTOT-NEXT 011-DET sig=v128|st0|m64|gc64|dc64|ic64|cc64|dF0.92076|dB0.95204|cb1|dt1|347/112
                    ^ everything before this is content                ^ these two are timings

The harness compares the whole line between two runs. It reports det=DIFF on a
build that is otherwise correct, and the owner concludes "determinism is broken
and we must investigate the render path".

A reviewer notes: strategy 0 legitimately has different group/draw counts
(64/64) than strategies 1 and 2 (5/5) — that is the FEATURE being tested, not
a defect.
```

### Hidden root cause (DO NOT REVEAL)
The signature **mixes content and timing**. `dispatch_us/draw_us` are wall-clock
microseconds and can never be byte-identical across runs, so a whole-line
comparison is guaranteed to fail on a correct build — a **false negative in the
gate itself**. Secondary trap: comparing across *different strategies* is not a
determinism test at all. Correct fix: emit content-only signature (move timings
to a separate line) and compare per-strategy.
