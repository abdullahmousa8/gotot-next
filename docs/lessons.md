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