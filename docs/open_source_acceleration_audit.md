# GOTOT-NEXT Open-Source Acceleration Audit

**Date:** 2026-09-22
**Author:** Big Pickle (executing Architect Dual-Track decision)
**Status:** STUDY ONLY — no integration. Awaiting Architect decision.
**Companion docs:** `docs/open_source_system_strategy_v1.md` (governance), `docs/dependency_register.md` (register — empty, no external dependency integrated to date), `docs/progress_report.md` (milestone history).

> Scope note: this audit is read-only. It classifies candidates (`Approved / Candidate / Deferred / Rejected / Reference-only`), estimates effort, and proposes a roadmap. No dependency is integrated, no architecture is changed, and no new milestone starts as a result of this audit. Every integration must follow the Dependency Approval Workflow (strategy doc Section 20) and license review (Section 08/08.5) with an Architect decision.

---

## Section 1 — Current GPU Core (delivered through GOTOT-011)

What has been proven working end-to-end on the gotot-render RDG fork (Godot 4 module, custom Vulkan RDG path):

- **GPU Scene (SoA):** GOTOT-owned struct-of-arrays GPU scene — transforms (position_scale), bounds, instance ids, mesh ids (GOTOT-001B).
- **Frustum Culling:** GPU two-phase visibility using 6-plane frustum test against instance bounds (GOTOT-002), correct deterministic counts.
- **HZB (prototype):** GPU hierarchical z-buffer build + occlusion check path (GOTOT-004) — prototype proven; its production successor is GOTOT-012 (below).
- **Compaction + Indirect Args:** GPU compaction of the visible set + computed indirect dispatch/draw arguments (GOTOT-003).
- **Real Geometry:** Real mesh path — actual vertex/index buffers rendered through the custom path, not just particles/stubs (GOTOT-008A).
- **Multi-Instance:** 512-instance (8×8×8) multi-instance rendering proof (GOTOT-008B).
- **Depth Buffer:** Real D32_SFLOAT depth attachment written by the custom path (GOTOT-009A/009B).
- **Batch Rendering:** Batch instance rendering — 3 meshes multi-draw, grouped batches (GOTOT-010).
- **Multi-Draw:** Dynamic draw count, parallel prefix-sum grouping, ≤5 grouped/reordered draws (GOTOT-011) + interactive demo (main_demo + camera_controller + hud, strategy live-switch).
- **Measured reduction:** 64 meshes → 5 draw calls = **12.8×** draw-call reduction (GOTOT-011 evidence).

## Section 2 — Remaining GPU Core (still required)

- **Production HZB (012):** current milestone — 12-level 2048-base occlusion pyramid feeding the *same* visibility array consumed by batch grouping. Status: FAIL code=123 on Attempt #4; root cause = instance→texel projection collapse (every instance samples pyramid texel (0,0)); offset layout itself verified identical in writer/reader. Stopped per decision tree.
- **GPU Scene Manager (013):** unified scene management (add/update/remove, sparse upload, stable ids) on top of the SoA buffers.
- **Meshlets / Cluster Generation:** mesh subdivision into GPU-friendly clusters with culling/LOD metadata.
- **LOD:** multi-LOD selection per cluster/instance (with meshoptimizer — see Section 5).
- **Production Occlusion:** robust occlusion (post-012), two-phase hierarchy, hysteresis/frame-coherence hardening.
- **Multi-Draw (advanced):** indirect indexed multi-draw with per-mesh first/vertex offsets, possibly VK_EXT_mesh_shader evaluation.
- **Memory management:** RHI/memory architecture decision (VMA stays deferred until then — strategy Section 15).
- **Shader tooling:** offline GLSL→SPIR-V compile/validate/reflect pipeline (glslang/shaderc ecosystem — candidate, offline).

## Section 3 — Build / Borrow / Integrate / Defer

Per-subsystem table (Architect's directive: Big Pickle completes the table; "?" marks items pending study/RHI decision).

| Subsystem | Build | Borrow | Integrate | Defer | Derivation |
|---|---|---|---|---|---|
| Renderer architecture | ✅ | | | | GOTOT-owned; no external renderer replaces it (strategy Section 03) |
| GPU Scene | ✅ | | | | GOTOT-owned SoA; external scene frameworks rejected |
| Frustum Culling | ✅ | | | | GOTOT-owned; implemented and proven |
| HZB | ✅ | ✅ | | | Owned implementation; borrow *reference patterns* only (Khronos/NVIDIA samples study) |
| Compaction | ✅ | | | | Owned; prefix-sum proven |
| Batch Rendering | ✅ | | | | Owned; GOTOT-010 |
| Multi-Draw | ✅ | | | | Owned; GOTOT-011 |
| Meshlet generation | ✅ | | ? | | Owned runtime format (strategy Section 12); meshoptimizer (offline) supplies generation — pending Architect decision |
| LOD | ✅ | | ? | | Owned selection logic; meshoptimizer simplification supplies LOD sources (offline) — pending decision |
| glTF parsing | | | ? | | cgltf (offline import) primary candidate; fastgltf reference-only (strategy Section 13) |
| meshoptimizer | | | ? | | Offline-only candidate (MIT); never at runtime (strategy Section 12) |
| VMA | | | | ✅ | Deferred until RHI/memory decision (strategy Section 15) |
| Render Graph | | ✅ | | ✅ | Borrow *patterns*; own implementation deferred |
| Materials | ✅ | | | ✅ | Material model GOTOT-owned; advanced material milestone deferred |

## Section 4 — Candidates

### P0 — meshoptimizer

```text
Name: meshoptimizer
URL: https://github.com/zeux/meshoptimizer
License: MIT (permissive)
Maturity: Mature (production use in many engines: BG2/Fortnite tooling lineage, widely embedded)
Relevant components: mesh optimization (vertex/index cache), cluster generation (meshlets),
                     simplification (LOD), quantization, vertex/index codec
Integration difficulty: Medium (single-core offline C++; needs a GOTOT-owned bake wrapper)
Expected time saved: High
Architectural risk: Low (offline tooling; output baked into GOTOT-owned mesh format; runtime never links it)
Maintenance risk: Low (active, MIT, single maintainer with strong track record)
```

Recommended classification: **Candidate (offline only)** — consistent with strategy Section 12.

### P1 — cgltf (glTF parser)

```text
Name: cgltf
URL: https://github.com/jkuhlmann/cgltf
License: MIT (permissive)
Maturity: Mature (single-header, widely used: darmstadt/sglm ecosystem, many engines)
Relevant components: glTF JSON/binary parsing, buffers, nodes, meshes, scenes
Integration difficulty: Low (single-header C; wrap for import-only)
Expected time saved: High (replaces writing a spec complete glTF parser)
Architectural risk: Low (offline import only; output converted to GOTOT-owned data)
Maintenance risk: Low
```

Recommended classification: **Candidate (primary)** — strategy Section 13.

**Note (studied, reference-only):** `fastgltf` (MIT, https://github.com/spnda/fastgltf) studied as an alternative parser; kept reference-only per strategy Section 13 (cgltf chosen for single-header simplicity/API stability). No action.

### P1 — glslang / shaderc (shader toolchain)

```text
Name: glslang / shaderc / SPIRV-Reflect
URL: https://github.com/KhronosGroup/glslang ; https://github.com/google/shaderc ; https://github.com/KhronosGroup/SPIRV-Reflect
License: BSD-3-Clause (glslang) / Apache-2.0 (shaderc, SPIRV-Reflect) — permissive
Maturity: Mature (Khronos/Google reference toolchain; Vulkan SDK standard)
Relevant components: offline GLSL→SPIR-V compilation, validation, reflection
Integration difficulty: Medium (large-ish build; must be leaf tooling, offline only)
Expected time saved: Medium-High (correctness + validation vs self-written compiler)
Architectural risk: Low (build-time only; never defines GOTOT shader model — strategy Section 14)
Maintenance risk: Low (Khronos-backed)
```

Recommended classification: **Candidate (offline)** — strategy Section 14.

### P1 — VMA (Vulkan Memory Allocator)

```text
Name: VMA (VulkanMemoryAllocator)
URL: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
License: MIT (permissive)
Maturity: Mature
Relevant components: GPU memory sub-allocation, fragmentation reduction
Integration difficulty: Medium (RHI-layer wrapper)
Expected time saved: Medium-High
Architectural risk: Medium (touches RHI/memory architecture — not yet decided)
Maintenance risk: Low
```

Recommended classification: **Deferred** (strategy Section 15) — blocked on RHI/memory architecture decision.

### P2 — AMD FidelityFX SDK

```text
Name: AMD FidelityFX SDK
URL: https://github.com/GPUOpen-Effects/FidelityFX
License: MIT (permissive)
Maturity: Mature
Relevant components: technique reference (e.g., contrast-adaptive sharpening, FS/RSR concepts, culling patterns)
Integration difficulty: High (runtime technique SDK — architecture impact)
Expected time saved: Low (no current technical need)
Architectural risk: High (would enter runtime pipeline)
Maintenance risk: Medium
```

Recommended classification: **Deferred** — no current need; may inform GOTOT-owned implementations (strategy Section 11/16).

### P2 — Khronos samples (reference)

```text
Name: Khronos Vulkan samples
URL: https://github.com/KhronosGroup/Vulkan-Samples
License: Apache-2.0 / project source licenses (study reference)
Maturity: Mature (Khronos reference)
Relevant components: correct ordering, synchronization, descriptor, visibility patterns
Integration difficulty: N/A (reference only — never linked)
Expected time saved: Indirect (validation of our correctness patterns)
Architectural risk: None
Maintenance risk: None
```

Recommended classification: **Reference-only** (strategy Section 10).

### P2 — NVIDIA samples (reference)

```text
Name: NVIDIA Vulkan samples / GameWorks sample code
URL: https://github.com/NVIDIAGameWorks / https://github.com/nvpro-samples
License: Various (study reference; per-file license check if any code is used)
Maturity: Mature
Relevant components: advanced culling, mesh shader, visibility, rendering patterns
Integration difficulty: N/A (reference only)
Expected time saved: Indirect
Architectural risk: None
Maintenance risk: None
```

Recommended classification: **Reference-only** (strategy Section 10). Non-trivial code reuse requires per-file license check + dependency approval.

## Section 5 — Recommended Integrations

Only what is worth integrating (and only *after* Architect decision + Dependency Approval Workflow + license review).

### P0 — meshoptimizer (offline)

- **لماذا (Why):** Meshlet/cluster generation and LOD simplification are exactly the hard, algorithm-dense GPU-driven pieces remaining (Section 2). Writing production-grade meshlet generation + simplification from scratch is a multi-week effort already solved and battle-tested here. Runtime format stays GOTOT-owned — meshoptimizer output is baked at import time (strategy Section 12).
- **كيف (How):** Add as offline build/import tooling behind a GOTOT-owned bake wrapper (`gotot_mesh_import`), pinned version, MIT license review recorded; output written into the GOTOT mesh format. Zero runtime linkage, zero runtime knowledge of the dependency.
- **متى (When):** Immediately before milestone 014 (Meshlets + LOD). No earlier milestone depends on it.

### P1 — cgltf (offline glTF import)

- **لماذا (Why):** glTF is the chosen interchange (strategy Section 13); a spec-complete parser is a large, error-prone surface. cgltf is single-header MIT and removes that entire risk class for import-time parsing only. Runtime stays free of any gltf/parser types.
- **كيف (How):** Wrap in `gotot_asset_pipeline` (import path only); convert parsed content into GOTOT-owned scene/mesh data; pinned version + license record.
- **متى (When):** Milestone 015 (Asset Pipeline). Not earlier.

### P1 — Shader tooling: glslang/shaderc/SPIRV-Reflect (offline)

- **لماذا (Why):** Current RDG uses Vulkan's `shader_compile_spirv_from_source` (runtime GLSL compile). An offline pipeline gives validation + reflection + reproducible SPIR-V and de-risks shader correctness — without changing the GOTOT shader model (strategy Section 14).
- **كيف (How):** Offline build step for GOTOT-authored GLSL → baked SPIR-V blobs consumed at runtime; leaf tooling only, wrapped, pinned.
- **متى (When):** Optional for 013/014; primary need arrives with more complex shaders (Materials — 016). Can be deferred to 016.

## Section 6 — Rejected / Deferred

- **VMA — Deferred:** blocked on RHI/memory architecture decision (strategy Section 15). GOTOT's current allocation strategy is GOTOT-owned; revisit as a leaf utility once the RHI decision lands.
- **AMD FidelityFX SDK — Deferred:** no current technical need; high runtime-pipeline risk for techniques we can implement GOTOT-owned. Informational only.
- **Render Graph — Deferred:** render graph architecture remains GOTOT-owned; external graphs are studied as patterns (borrow) but not integrated now.
- **Full renderer — Rejected:** no wholesale import of an external renderer (e.g., a full Vulkan engine), and no replacement of the GOTOT renderer. (Strategy: renderer architecture, GPU Scene, visibility = GOTOT-owned.)
- **fastgltf — Reference-only:** studied as alternative parser; not integrated (cgltf is the primary candidate).
- **AGPL/copyleft runtime components — Rejected/automatic-defer:** per license policy, any non-permissive runtime dependency is deferred pending legal review (Section 08.5).

## Section 7 — Estimated Time Savings

Estimates are engineer-day planning figures, not performance claims (Architect note acknowledged).

```text
Meshlet generation:
  Without: 15–20 engineer-days
  With meshoptimizer (offline): 5–7 engineer-days
  Saving: 10–13 engineer-days

LOD simplification:
  Without: 10–15 engineer-days
  With meshoptimizer: 3–5 engineer-days
  Saving: 7–10 engineer-days

glTF parsing:
  Without: 10–15 engineer-days
  With cgltf: 2–3 engineer-days
  Saving: 8–12 engineer-days

Shader toolchain (compile/validate/reflect):
  Without: 6–10 engineer-days
  With glslang/shaderc: 2–4 engineer-days
  Saving: 4–6 engineer-days

GPU memory (VMA — only if integrated later):
  Without: 8–12 engineer-days
  With VMA: 2–4 engineer-days
  Saving: 6–8 engineer-days (pending RHI decision)
```

**Aggregate (selected P0/P1 integrations):**
```text
Without Open Source:  49–72 engineer-days
With selected integrations: 14–23 engineer-days
Estimated saving: 35–49 engineer-days
```
(Sum covers meshlets, LOD, glTF, shader tooling — the five lines above, excluding the deferred VMA line.)

## Section 8 — Proposed Roadmap (GPU-Driven Core, re-ordered per audit)

Unchanged milestone numbering; dependencies aligned so each integration lands in its natural slot.

| Milestone | Scope | External dependency |
|---|---|---|
| **012** | Production HZB (current — blocked on projection fix) | None |
| **013** | GPU Scene Manager (add/update/remove/stable ids) | None |
| **014** | Meshlets + LOD | **meshoptimizer (offline)** — pending decision |
| **015** | Asset Pipeline (glTF import) | **cgltf (offline)** — pending decision |
| **016** | Materials + Lighting | shader tooling (optional here) |
| **017** | Shadows | None |
| **018** | GI | None (reference study only) |
| **019** | Render Graph | None (own implementation; borrow patterns) |
| **020** | Zero-Copy | VMA — if RHI decision lands before then |

---

## STOP — per Architect instruction

- No dependency integrated.
- No architecture change.
- No new milestone started as a result of this audit.
- Reported to Architect. **Architect Decision Required** before any integration.