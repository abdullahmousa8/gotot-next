# GNE-018-rev - Normal-Cone Back-Face Cluster Culling (SPEC v1.0)

**Status:** DRAFT v1.0 - awaiting Architect decisions (D8-rev). No implementation before FINAL.
**Depends:** 018 (Lighting, FINAL).
**Borrows (math + safety rules, NOT code):** isolated reference lab `ClusteredLightingLab`
(GL 4.3, no shared code) commits: `1707003` (cutoff is sin(halfAngle), not mindp),
`c5fd6ae` (GPU normal-cone reduction + value-level self-tests), `13f283a` (empty-cluster skip).
**Scope guard:** strictly additive and flag-gated. With the flag OFF the 018 path must stay
byte-identical (every legacy literal preserved).

## 1. Objective

Add a per-cluster normal-cone back-face stage: skip lights that cannot contribute to any
surface in a cluster (every normal in the cluster faces away from the light). Target:
fewer light-cluster assignments and less shading work, with zero legal pixel change on lit
surfaces.

## 2. Background and borrowed rules (as reviewed in the lab source)

1. Cone record = `vec4(axis, cosHalfAngle)`. `axis` seeded from the pair of
   furthest-apart normals + two Ritter growth passes; `mindp = min dot(axis, n)` is then
   computed EXACTLY over the actual set (exactness keeps the cone conservative even if the
   axis is not perfectly tight).
2. **Cutoff rule:** the cull threshold is `sin(half) = sqrt(1 - mindp^2)`, NOT `mindp`.
   The two agree only at 45 degrees; using mindp directly over-culls lit surfaces for any
   wider cone. [lab bug fix `1707003`]
3. **Incoherent/empty rule:** a cluster whose normals span more than ~168 degrees
   (`mindp <= 0.1`) or that has no valid pixels is written as `vec4(0)`, read back as
   `w <= 0.1` and then the back-face test is SKIPPED (light kept). "Never cull" is the
   safe default; covering too little is affordable, culling lit surfaces is not.
4. **Operand order rule:** `toLight = lightPos - clusterAnchor; skip if
   dot(normalize(toLight), coneAxis) < -sinHalf`. Swapped operands cull exactly the side
   that faces the light (silent wrong output). [lab's second cone bug]
5. **Per-frame rewrite rule:** cones must be rewritten every frame; a stale cone must
   never decide culling. [lab test suite]
6. **Not ported:** their SoA database, their GL bootstrap, tile-based light batching,
   shadow-atlas phase-1 scaffolding.

**Reference-state caveat:** the lab working tree currently has 7 uncommitted files and its
newest claims span commits `...1707003/c5fd6ae/13f283a` without GPU performance numbers
(its build host has no interactive desktop). Before implementation the exact reference
revision must be frozen (lab side commits or stashes its deltas).

## 3. Design (proposed - all points are D8-rev decisions)

### 3.1 Normal source (D8-rev-1)
Our raster framebuffer currently carries color + viewz + depth; there is no normal
attachment. Proposal: add a third color attachment `raster_normal_texture` (RGBA16F,
view-space normal in xyz) written by the existing material fragment shader (it already
computes `N`). Additive to the framebuffer; existing attachments' bytes must verify
unchanged across the full DET sweep.

### 3.2 Cone pass and buffer (D8-rev-2)
New compute `gne_build_cones`: one thread per cluster (3456 threads, 54x64 dispatch like
the light cull). Inputs: the last drawn frame's normal + depth attachments and the same
tile->pixel mapping / z-slice arithmetic the lab validated. Output: `cluster_cone_buffer`
(3456 x 16 B = 55,296 B). Rules 2.2-2.3 apply exactly.

### 3.3 Cull integration (D8-rev-3)
`gpu_light_cull_glsl` gains binding 6 (cone buffer) and the back-face prefilter, applied
before `sphere_vs_aabb` exactly per rules 2.4-2.5. The survivor append path (atomic slot +
sorted order + overflow counter) is untouched; skipped lights are simply not appended,
preserving per-cluster sorted determinism.

### 3.4 Staleness policy (D8-rev-4)
Our frame order culls lights before the raster they shade, so cones can only derive from
the previous frame's raster (1-frame lag). Options: (a) accept + document + restrict
identity gates to static scenes; (b) double-buffer runtime edits (future); (c) enable only
while camera and scene are static (flag). Recommendation: (a) for rev-1 scope.

### 3.5 Flag and artifacts
`gpu_light_set_normal_cone(bool)` (default false). New demo `main_018_rev.gd|.tscn`, new
harness `tools/gt_018a_rev.bat`, signature proposal `v18-rev|lc|cc|dc|ot|hr|d`
(dc = dropped assignments, the savings number). The 018 default path is untouched.

## 4. Acceptance criteria (proposed; final thresholds at D8-rev)

| # | Criterion | Method |
|---|---|---|
| R1 | No visual change | flag off vs on, same static scene: PIXEL-IDENTICAL byte compare |
| R2 | Real savings | dc >= 10% of baseline assignments on the rev scene (initial; measured-at) |
| R3 | DET | d1/d2 byte-identical `v18-rev|...` |
| R4 | Regressions | full sweep green; every legacy literal byte-intact |
| R5 | Hygiene | zero `ERROR:`, zero leak lines (KI-010 checks active) |
| R6 | Evidence | before/after assignment counts + touched clusters in the report; no timings inside sigs (015.5 rule) |

## 5. Boundaries

Owns: cone pass + cone buffer + cull prefilter + flag + `main_018_rev` + `gt_018a_rev`.
Never (unchanged Architect constraints): the 018 default path when the flag is off;
016 material record; other milestones; `RenderingServer`/RHI; engine patches.
Hand-off rule: any legacy pixel or signature delta => STOP, rollback, report.

## 6. Out of scope

Shading-model changes (including any `lspec` gating - see Risk 1), cull-list reordering,
tile-based light batching, applying cones to 019 shadow caster lists.

## 7. Risks

1. **Specular leak (primary):** our material fragment's specular term is not `ndl`-gated;
   a fully back-facing light can still leak a grazing specular. R1 may then show LSB
   differences. Policy: measure first, no silent tolerance; if nonzero, return to the
   Architect (options: extra cone margin, rev-path specular gating, or scope cut).
2. Stale cones (3.4) - mitigated by the flag and static identity gates; full
   restructuring deferred to 020+.
3. Normal attachment: must not perturb existing attachment bytes (DET-proven).
4. Cone-pass cost stays out of the signature; savings evidenced by dc.

## 8. Deliverables

Flag-gated module changes; cone buffer + pass + set updates; `main_018_rev` + `gt_018a_rev`;
5 contracts; before/after measurement note.

## 9. Decisions requested (Architect, D8-rev)

- **D8-rev-1** normal source (3.1).
- **D8-rev-2** cone pass + buffer (3.2).
- **D8-rev-3** cull placement and exact formula (3.3).
- **D8-rev-4** staleness policy (3.4) - recommend (a).
- **D8-rev-5** signature fields and dc threshold (3.5 / section 4).
- **D8-rev-6** specular-leak policy (Risk 1) - measure-first with defined fallback.
- **D8-rev-7** harness set (gt_018a_rev + full sweep) and KI handling.