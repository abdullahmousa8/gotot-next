# GNE — Lessons Learned

## Lesson 1: Diagnosis Discipline (012-revised)

**Date:** 2026-09-28
**Context:** 4 attempts on 012 spent on wrong hypothesis.

**Lesson:**
Do not diagnose from final symptoms alone (e.g., max_inv=0).
Inspect intermediate state (fresh flags, UBO types) before assuming
a math error.

**Actual root causes:**
- Fix F: hzb_pyramid_fresh deadlock at line 3071.
- Fix T: int32 in float UBO field (3 sites).

**Wrong hypothesis:**
- Projection collapse (spent 4 attempts).

**Signatures preserved:**
- 013/014/015/015.5/012-rev literal.

## Lesson 2: External Search Strategy

**Date:** 2026-09-28
**Context:** 012 was stuck for 4 attempts.

**Lesson:**
External references (vkguide.dev, Godot PRs) provide industry
consensus. Always document URL + hash + date.

**Sources:**
- vkguide.dev (GPU-driven culling).
- Godot PR #100907 (view depth semantics).
- miketuritzin.com (Hi-Z reference).

**Rule:** External source → registry entry → decision.

## Lesson 3: Type Mismatch is Silent

**Date:** 2026-09-28
**Context:** int32 written to float UBO field.

**Lesson:**
int 2048 = 0x00000800 = float 2.8e-42 ≈ 0.
Silent corruption. Only sim2 detected.

**Rule:** UBO field types must match exactly.

## Lesson 4: Frustum Planes Bug (017 phase 2)

**Date:** 2026-09-28
**Context:** `get_projection_planes` produced wrong planes for rotating
cameras. Latent bug — only surfaced with motion.

**Impact:**
- Culling incorrect for rotating cameras.
- Worked by accident with static cameras.

**Root cause:**
- Plane extraction from projection matrix incorrect.

**Fix:**
- Extract planes from VP matrix rows in `set_camera`.
- 4 independent pieces of evidence.

**Verification:**
- 6 gates: signatures literal.
- Zero drift confirmed.

**Lesson:**
Static-camera tests can hide culling bugs.
Always test with rotating cameras.

**Related:** KI-007 (HZB AABB).
Both in set_camera / culling pipeline.

## Lesson 5: Concurrent GPU Runs (Environment)

**Date:** 2026-09-28
**Context:** Isolated project + GNE tested simultaneously.

**Observed:**
- Repeated `nvoglv64.dll` crashes.
- Repeated `nvlddmkm 153` (kernel driver error).
- Multiple GNE runs polluted by driver resets.

**Root cause:**
- Concurrent Vulkan/OpenGL usage on same GPU.
- Driver does not isolate contexts cleanly.

**Rule:**
No concurrent runs. Clean test windows only.

**Enforcement:**
- GNE tests: dedicated time windows.
- Isolated project: separate windows.
- Any run with driver crash = rejected measurement.

**Related:** KI-008 (potential).

## Lesson 6: Never Trust cwd For Git Operations (Tooling, Build Host)

**Date:** 2026-09-28
**Context:** A long build+test script did `Set-Location` into the build host
(godot-master) for scons; a later stray `git add/commit` in the same script ran
there by mistake.

**Observed:**
- The stray commit became the build host repo's FIRST commit (28,656 engine files
  staged); the repo has no remote, so nothing left the machine.
- Caught immediately; restored exactly with `git update-ref -d` (unborn branch).

**Rule:**
- Every git command uses `git -C <absolute repo path>`; never rely on cwd.
- Assert the target with `git rev-parse --show-toplevel` before add/commit/push.
- Build host = build only; its repo state must stay untouched.
## Lesson 7: GI Temporal Accumulation Quantization Lock (LAB/GNE-validated diagnostic)

**Date:** 2026-09-29
**Context:** GNE-022 section 11 live-loop test (EMA alpha=0.1 accumulation into an
RGBA16F double-buffered probe atlas, 16x8x16 field).

**Observed (validated diagnostic, not a bugfix):**
- The accumulating field marches on the half-float lattice (steps of exactly
  2^-11 = 0.00048828125 in [0.5,1)) and then HARD-FREEZES mid-range: the section-11
  run froze at f = 0.739257812 = 1514 x 2^-11 (k=77); the displayed image froze in
  the same frame.
- Mechanism: when the EMA update 0.1 x (g - old) falls below the round-to-nearest
  threshold (~half the storage quantum, i.e. ~2.44e-4/frame in this band), the
  stored bits stop changing; the loop latches permanently. Instruments above the
  storage (8-bit framebuffer window mean) cannot see past the latch.
- Same class as the S2 long-window freeze (9.3.9, iz4); section 11 pinned the
  mid-range threshold and produced direct per-frame lattice evidence.

**Rule for future work (temporal GI / denoising / multi-frame lighting):**
- Any EMA-style temporal accumulation over FP16 storage exhibits a mid/long-term
  quantization lock; the lock point scales with the storage exponent band.
- Budget the accumulation storage precision FIRST when designing temporal GI; an
  FP16-only accumulator silently truncates convergence to its quantum.
- Diagnose with per-frame lattice dumps (exact multiples of 2^-k expose the lock
  and its onset frame immediately).

**Verification update (R1 closure, 2026-09-29):** the counterfactual was run - the
same algorithm, scene, gain and criterion on FP32 accumulation show NO lock (clean
0.509 decay through 160 frames). Removing the FP16 storage removed the symptom: the
quantization-lock mechanism is CONFIRMED (reproduction + counterfactual), and
RGBA32F is the adopted accumulation storage for the GI field (commit 52ab47a).

**Related:** GNE-022 section 11 (spec_022), section 11-R1 (accumulation precision
experiment).
## Lesson 8: Manual Test Is Not a Blocking Gate
**Date:** 2026-10-01
**Context:** X1 (016.5 slice-1) succeeded manually for weeks without CVS registration; made fail-closed (exit 41) + wrapped (gt_016_5.bat) + rejection-tested (mutation: x1=false alone, exit 41) in one unit.
**Lesson:** A test that succeeds by hand is NOT a gate. A gate requires exit-code enforcement AND harness integration AND a proven rejection path (something that fails on demand and is refused).
**Rule:** Every milestone gets its blocking gate before closure; the rejection path is demonstrated, never assumed. (Lesson 6 on git-cwd and the pre-existing Lesson numbering are untouched - this is the next free number.)

## Lesson 9: A Measured Difference Can Be A Design Choice
**Date:** 2026-10-01
**Context:** A/B isolation of the Country House against a vanilla Godot twin.
Direct-only MAE 0.280 (matched) vs ambient-only MAE 6.87 (mismatch). The
mismatch was GNE's tinted ambient (`0.1 x albedo`) against Godot's flat white
ambient.
**Lesson:** a difference is not a defect until the stage that produces it is
isolated. Splitting the render into direct / ambient / GI stages first turned
"the colours are wrong" into "one named term differs by design" - and the
per-channel ratio shape (R .80 / G .25 / B .27) is what identified it as a
COLOUR SOURCE difference rather than an energy or exposure one.
**Rule:** before changing a number, prove which stage produces it, and check
whether the difference is channel-uniform (energy/exposure) or
channel-dependent (colour source). A uniform difference looks like a bug; a
structured one often is the design.
