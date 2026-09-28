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
