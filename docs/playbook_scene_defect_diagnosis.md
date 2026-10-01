# Scene-Defect Diagnostic Playbook (reusable, model-agnostic)

Written after the GNE 0.26-pre/0.26a Country House investigation. The point of
this document is that the **method** works on any renderer and any AI agent —
you do not need this repository, this engine, or these coordinates.

---

## 0. The one-paragraph version

A part that "renders wrong" is never diagnosed by looking at it. It is
diagnosed by **asking the frame what colour it thinks that part is**, then
checking whether that colour can only come from somewhere else. Nine times out
of ten the answer is a *data* bug (wrong index, wrong order, occluded geometry)
and not a *shading* bug — and the single highest-yield check is a **full-frame
colour census**: does every surface carry a colour derived from **its own**
albedo? If one does not, you have found your bug in a single pass.

---

## 1. Golden rules (learned the hard way)

1. **Never assume a coordinate convention.** Screen row order, colour space,
   and depth direction differ between pipelines. Probe both and *measure*.
   (Cost of learning: a "99% mismatch" that was really a vertical flip.)
2. **Full-frame census before point probes.** A point probe can land on the
   wrong surface and send you chasing a non-bug. A census cannot lie.
3. **Ask "whose colour is this?" not "why is it black?"** Black is a symptom.
   The neighbour's colour is the evidence.
4. **Match by ratio, not by absolute value.** Find the shade factor `k` such
   that `framebuffer ≈ albedo × k`. A uniform `k` = lighting. No `k` at all =
   the material is not being read.
5. **Suspect the index before the maths.** Off-by-one between "the id you
   registered" and "the id the shader reads" is the single most common defect
   in this class of bug, and it silently corrupts *everything* at once.
6. **Fix data, not code.** If the renderer is behaving as specified, changing
   it is the bug.
7. **Never gate on a number you did not measure before the fix.** Capture the
   baseline first; a moving baseline hides regressions.

---

## 2. The loop (works for any scene, any engine)

```
STEP 1  DECLARE     list every part: name, geometry, albedo, roughness
STEP 2  CENSUS      dump every distinct RGB triple + pixel count (full frame)
STEP 3  MATCH       for each part, find k with framebuffer == albedo * k
                    -> parts with NO match are your suspects
STEP 4  ORIENTATION resolve row/colour convention by measurement, not docs
STEP 5  PROBE       project each part's own point through the engine's OWN
                    matrix; report the pixel at both row conventions
STEP 6  CLASSIFY    for each suspect, name the mechanism:
                      (a) wrong material index  (b) occluded by geometry
                      (c) back-face / winding  (d) depth/z-fight
                      (e) off-screen / behind camera
STEP 7  FIX (data)  change ONLY scene data, then re-run steps 2-5
STEP 8  NEGATIVE    revert the fix, confirm the symptom returns byte-exactly
STEP 9  RECORD      root cause + the measurement that proved it
```

---

## 3. The three questions that find ~all bugs

| Question | If YES |
|---|---|
| Does part P carry a colour that is `albedo_P × k`? | **NO** → material/index bug. Stop looking at lighting. |
| Does part P's pixel carry part Q's colour? | **YES** → Q occludes P, or P reads Q's material. Compare against the render order. |
| Does part P read pure black (0,0,0) in a frame whose background is also black? | **YES** → P is either absent or shaded as a back-face. Distinguish by checking whether *any* pixel in the frame has P's colour. |

---

## 4. Reusable tool

`tools/scene_defect_probe.gd` (this repo) implements steps 2–5 for GNE. The
same ~150 lines port to any engine that can hand you: a framebuffer, a
view-projection matrix, and your declared part list. It returns a dictionary;
it never writes files and never asserts — reporting is the caller's job.

---

## 5. Worked example (what actually happened)

**Symptom:** "the back wall is missing and the roof apex is black."

- Census: the wall's own colour appeared in **0** pixels. But the *path* colour
  appeared on **392,934** pixels — a region far too large for a 160×700 path
  quad. That size mismatch was the tell.
- Probe: the ground's own point read the path's colour; the chimney read
  `(6,6,6)` ≈ black.
- Ratio check: `path_colour / ground_albedo` was constant across all three
  channels ⇒ not lighting.
- Root cause: the material table is indexed by **mesh id**, the scene wrote it
  by **instance index**, and mesh ids start at 1. Every part was shaded with
  its neighbour's material; the last mesh read an unwritten (black) slot.
- Fix: address materials by mesh id. One line. No engine change.
- Negative control: reverting the one line restored the original census
  exactly.

---

## 6. Handing this to another AI agent

Paste this document plus `tools/scene_defect_probe.gd`, and say:

> Diagnose why part X renders wrong in this scene. Follow the playbook.
> Declare the parts, run the census, match by ratio, probe with the engine's
> own matrix, and name the mechanism. Do not modify engine code. Report the
> measurement for every claim, and finish with a negative control.

That is the entire prompt. The method is the deliverable.