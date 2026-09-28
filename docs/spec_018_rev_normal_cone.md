# GNE-018-rev - Normal-Cone Back-Face Cluster Culling (SPEC v1.0)

**Status:** FINAL v1.0 - D8-rev decisions approved (Architect, 2026-09-28); amendments A1/A2 applied.
Lab reference frozen at ClusteredLightingLab `28d5af0`. Implemented + R1 PASS (2026-09-28); canonical rev signature `v18-rev|lc=20|cc=3091|dc=13|ot=9|dp=2558|d1` (d1 == d2); closure note in section 11.
**Depends:** 018 (Lighting, FINAL).
**Borrows (math + safety rules, NOT code):** isolated reference lab `ClusteredLightingLab`
(GL 4.3, no shared code); frozen reference revision `28d5af0` (2026-09-28) containing the
reviewed cone code (`Shaders.h` kBuildConesComp + cull test; `ClusterMath.cpp` BuildNormalCone)
plus its research notes. Prior commits of interest: `1707003` (cutoff is sin(halfAngle), not
mindp), `c5fd6ae` (GPU normal-cone reduction + value-level self-tests), `13f283a`
(empty-cluster skip).
**Scope guard:** strictly additive and flag-gated. With the flag OFF the 018 path must stay
byte-identical (every legacy literal preserved).

## 1. Objective

Add a per-cluster normal-cone back-face stage: skip lights that cannot contribute to any
surface in a cluster (every normal in the cluster faces away from the light). Target:
fewer light-cluster assignments and less shading work, with zero legal pixel change on lit
surfaces.

## 2. Background and borrowed rules (as reviewed in the lab source)

1. Cone record = `vec4(axis, cosHalfAngle)`. `axis` is constructed from the actual normal
   set; `mindp = min dot(axis, n)` is then computed EXACTLY over the actual set (exactness
   keeps the cone conservative even if the axis is not perfectly tight).
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

**Reference freeze:** verified 2026-09-28. The lab tree was not frozen when checked
(8 modified files + research notes uncommitted), so the reference state was sealed as
commit `28d5af0` ("freeze: capture working tree as borrowed-reference state for
GNE-018-rev"); worktree clean afterwards except a debug artifact (`shadowdump.ppm`).
The reviewed cone code is intact in that revision.

## 3. Design (decisions closed - see section 9)

### 3.1 Normal source (D8-rev-1)
Our raster framebuffer currently carries color + viewz + depth; there is no normal
attachment. Proposal approved: add a third color attachment `raster_normal_texture`
(RGBA16F, view-space normal in xyz) written by the existing material fragment shader
(it already computes `N`). Additive to the framebuffer; existing attachments' bytes must
verify unchanged across the full DET sweep.

### 3.2 Cone pass and buffer (D8-rev-2)
New compute `gne_build_cones`: one workgroup per cluster (3456 total, 54x64 dispatch like
the light cull). Inputs: the last drawn frame's normal + depth attachments and the same
tile -> pixel mapping / z-slice arithmetic the lab validated. Output: `cluster_cone_buffer`
(3456 x 16 B = 55,296 B). Rules 2.2-2.3 apply exactly.

Implementation note (rev-1): our tiles are 120x120 px (up to 14,400 px per cluster-slice
vs the lab's <=64), so the axis is computed with an equivalent two-pass exact scheme - a
fixed-order deterministic reduction for the axis, then an exact min-dot pass over the
actual set. Containment guarantee is identical (exact min over the set). Sub-sampling that
could under-cover the normal set is forbidden; a bounded reducer that cannot hold the set
must fall back to the never-cull sentinel.

### 3.3 Cull integration (D8-rev-3)
`gpu_light_cull_glsl` gains binding 6 (cone buffer) and the back-face prefilter, applied
before `sphere_vs_aabb` exactly per rules 2.4-2.5. The survivor append path (atomic slot +
sorted order + overflow counter) is untouched; skipped lights are simply not appended,
preserving per-cluster sorted determinism.

### 3.4 Staleness policy (D8-rev-4, as amended)
Our frame order culls lights before the raster they shade, so cones derive from the
previous frame's raster (1-frame lag). Per the Architect amendment this is enforced by a
regression gate, not documentation: the engine exposes the actual frame delta between the
raster record and the cull use (cone source age); `gt_018a_rev` reads it and FAILS
explicitly if it exceeds one frame at any cull during the run (R7). Restructuring beyond
that is deferred to 020+.

### 3.5 Flag and artifacts
`gpu_light_set_normal_cone(bool)` (default false). New demo `main_018_rev.gd|.tscn`, new
harness `tools/gt_018a_rev.bat`, signature proposal `v18-rev|lc|cc|dc|ot|hr|d`
(dc = dropped assignments, the savings number). The 018 default path is untouched.

## 4. Acceptance criteria (thresholds per section 9)

| # | Criterion | Method |
|---|---|---|
| R1 | No visual change from culling (scoped) | flag off vs on, static scene: every differing pixel must lie in the cull-affected region (clusters whose light lists changed) AND carry the KI-011 leakage signature (all dropped lights NdotL <= 0 at that pixel). Unexplained differences fail as over-cull. [amended A1] |
| R2 | Real savings | dc >= 10% of baseline assignments on the rev scene (initial; measured-at) |
| R3 | DET | d1/d2 byte-identical `v18-rev|...` |
| R4 | Regressions | full sweep green; every legacy literal byte-intact |
| R5 | Hygiene | zero `ERROR:`, zero leak lines (KI-010 checks active) |
| R6 | Evidence | before/after assignment counts + dropped light lists in the report; no timings inside sigs (015.5 rule) |
| R7 | Staleness gate | cone source age <= 1 frame at every cull (engine-exposed); explicit FAIL otherwise. [added A2] |

## 5. Boundaries

Owns: cone pass + cone buffer + cull prefilter + flag + `main_018_rev` + `gt_018a_rev`.
Never (unchanged Architect constraints): the 018 default path when the flag is off;
016 material record; other milestones; `RenderingServer`/RHI; engine patches.
Also NOT owned: the pre-existing specular NdotL leak (KI-011) - separate ticket; must NOT
be fixed inside 018-rev.
Hand-off rule: any legacy pixel or signature delta => STOP, rollback, report.

## 6. Out of scope

Shading-model changes (including any `lspec` gating - KI-011), cull-list reordering,
tile-based light batching, applying cones to 019 shadow caster lists.

## 7. Risks

1. **Specular leak (pre-existing, separated):** our material fragment's specular term is
   not `ndl`-gated (KI-011, ticket opened separately). After culling, strictly back-facing
   lights can leave small specular deltas - expected, attributed, and NOT fixed in this
   batch (Architect directive 2026-09-28). Any delta that does not carry the NdotL <= 0
   signature is an over-cull defect and fails R1.
2. Stale cones (3.4) - now enforced by gate R7; full restructuring deferred to 020+.
3. Normal attachment: must not perturb existing attachment bytes (DET-proven).
4. Cone-pass cost stays out of the signature; savings evidenced by dc.

## 8. Deliverables

Flag-gated module changes; cone buffer + pass + set updates; `main_018_rev` + `gt_018a_rev`;
5 contracts; before/after measurement note.

## 9. Decisions - CLOSED (Architect, 2026-09-28)

- **D8-rev-1** normal source (3.1) - approved as proposed.
- **D8-rev-2** cone pass + buffer (3.2) - approved as proposed (implementation note applies).
- **D8-rev-3** cull placement and exact formula (3.3) - approved as proposed.
- **D8-rev-4** staleness - APPROVED AS AMENDED (A2): documentation -> explicit regression
  gate R7 (engine-exposed cone source age; gt_018a_rev fails if > 1 frame).
- **D8-rev-5** signature fields and dc threshold (3.5 / section 4) - approved as proposed.
- **D8-rev-6** specular leak - APPROVED AS AMENDED (A1): leak separated into KI-011
  (pre-existing; NOT part of 018-rev; no fix in this batch); R1 rescoped to cull-affected
  regions + NdotL signature check.
- **D8-rev-7** harness set (gt_018a_rev + full sweep) and KI handling - approved as proposed.

## 10. Amendment log

- **A1 (2026-09-28, Architect directive):** specular-leak scope separation. KI-011 ticket
  opened as a separate pre-existing record; contract R1 rescoped to the cull-affected
  regions with the NdotL <= 0 signature check; no leak fix inside 018-rev.
- **A2 (2026-09-28, Architect directive):** single-frame staleness converted from a
  documentation note into an explicit regression criterion (R7) tied to gt_018a_rev.

## 11. Closure (2026-09-28)

- Units 2a/2b/2c + R1 complete; full sweep 9/9 green; the flag-off path stays
  byte-identical (v18 literal intact).
- As-built signature fields: `lc, cc, dc, ot, dp, d1` (dp = differing-pixel count;
  evidence per R6 - replaces the proposal's hr field).
- R2 decision (delegated): measured dc=13; the provisional >=10% target is NOT met
  on this scene and not claimed; the quantitative scale target stays open and moves
  to the dense-light scenario (benchmark city). The criterion was not redefined to
  pass.
- ot=9 on the rev path: loud marker kept; overflow semantics / cap policy tracked
  in KI-014's R1 addendum.
- Two integration defects found and fixed by the R1 self-check: closest-point
  back-face anchor (`gne_backface_dot_box`); teardown frees for the cone objects.
- Evidence: docs/rev_r1_evidence.md; progress_report section 37. Commits: 51fbf5c,
  ef0d402, 9fa2de1, a3d5e8e, a696b9f.
## 12. Default-state decision: cone culling ships DISABLED by default (2026-09-28)

**Decision (explicit; recorded so the code state is never read as a claim):**
`light_cone_enabled` stays `false` by default in `gpu_light_create` (header default,
`gne_render_server.h`). Enabling happens only via `gpu_light_set_normal_cone(true)` or
the measurement-only env override `GNE_REV_CONE=1` (read once; unset in every gate).

Rationale (same spirit as the 020 "mitigated, not solved" framing):
- The mechanism is VERIFIED correct and safe: R1 confinement + NdotL signature gates
  (outside=0, unexplained=0), R7 staleness gate (age=1), DET, full sweep green.
- The QUANTITATIVE benefit is NOT yet proven: dc=13 assignments (0.11% of the corrected
  base) on the rev scene; the provisional >=10% target is unmet and not claimed.
- Default-on would ship extra per-frame cost (cone build pass + 55 KB buffer + cull
  prefilter) with unproven net effect. Therefore: OFF until the dense-light scenario
  demonstrates savings > cost (tracked in spec_020 section 13 open items).
- Flipping the default is a one-line change to be made AFTER that evidence lands - at
  which point this section gets an update note. This is an explicit decision, not an
  implicit state.
**Update (2026-09-28, GNE-021 results applied):** the dense-light retest has been
executed (spec_021 section 4): corrected-base cone savings measured at 0.48% on a
256-light street-grid city (far below the 10% threshold); net off-vs-on is negative
(-6.5%). Per the pre-registered criterion the default remains OFF; the retest item is
closed as measured. Flipping the default would additionally require new evidence on a
scene that exercises the cone filter (valid uniform-normal clusters) far more strongly
than the observed 5.4% of clusters.