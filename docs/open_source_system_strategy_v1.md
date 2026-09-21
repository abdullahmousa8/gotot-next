# GOTOT-NEXT Open-Source System Strategy v1

**Status:** Draft for Architect review
**Milestone baseline:** GOTOT-008A (HEAD `dbd2735`)
**Scope:** POLICY-ONLY. Documents strategy. Integrates nothing.

---

## Core Principle

> **Open-source dependencies must support GOTOT-NEXT architecture; they must never define GOTOT-NEXT architecture.**

GOTOT-NEXT is defined by its own architecture (renderer, GPU Scene, visibility system, render graph, mesh runtime abstraction). External open-source components are accepted only when they fit inside GOTOT-NEXT's boundaries as replaceable, contained utilities. No external component decides how GOTOT-NEXT is structured, scheduled, or owned.

---

## 01 — Strategic Principle

1. GOTOT-NEXT owns the architecture. Dependencies are borrowed capability, never architectural commitments.
2. Every external dependency must sit behind a GOTOT-owned abstraction boundary that isolates GOTOT-NEXT from the dependency's API, behavior, and lifecycle.
3. A dependency is acceptable only if it can be removed or replaced without architectural change.
4. "Listed in this document" does not mean "approved." Every candidate requires an explicit approval decision before any integration.
5. Preference order: **own simple implementation → external permissive dependency → accredited platform/ecosystem component.** External dependencies are a cost, not a benefit.

## 02 — Build vs Buy vs Borrow

| Choice | Meaning | Default stance |
|---|---|---|
| **Build** | GOTOT-NEXT implements and owns it | Preferred for anything architectural: rendering, GPU Scene, visibility, render graph, mesh runtime abstraction |
| **Buy** | Accredited SDK/plugin under approved license | Only when the cost of building exceeds the cost of owning, and no architectural compromise results |
| **Borrow** | Reference code studied but not linked | Always allowed for insight; never linked without approval (e.g., Khronos / NVIDIA / AMD sample code) |

Rules:
- Build when the component defines interface or data flow (architecture).
- Buy/Borrow when the component is leaf utility (geometry math, format parsing, memory allocation).
- Never Buy/Borrow something that would define a data structure GOTOT-NEXT must preserve.

## 03 — GOTOT-NEXT Ownership Boundary

The following are **GOTOT-NEXT-owned** and must never be delegated to or redefined by a dependency:

- Renderer architecture and pipeline layout
- GPU Scene construction and layout
- Visibility / culling system
- Render graph
- Mesh runtime abstraction (GOTOT's own mesh format and runtime representation)
- Framebuffer, texture, and pass management
- Draw command generation and GPU Scene mesh-id lookup path
- Realtime rendering behavior and guarantees

Dependencies may contribute **below** this line only as leaf utilities (see Section 05).

## 04 — Open-Source Component Categories

| Category | Definition | Handling |
|---|---|---|
| **Runtime core** | Linked into GOTOT-NEXT at runtime | Highest scrutiny; abstraction boundary mandatory; permissive license only (see 08) |
| **Offline tooling** | Used at build/content time only (importers, processors) | Lower runtime risk, but still analyzed for output compatibility |
| **Reference-only** | Sample/demo source studied for correctness patterns | Never linked; no license implication on GOTOT-NEXT |
| **Deferred** | Interesting but blocked pending an architecture decision | No integration until unblocked decision lands |
| **Rejected** | Evaluated and excluded for stated reasons | Documented to prevent re-proposal without new facts |

## 05 — Candidate Technologies

Candidates are classified exactly one of: **Approved / Candidate / Deferred / Rejected / Reference-only**.
Appearance in this document is **not** approval.

| Technology | Domain | Classification | Notes |
|---|---|---|---|
| **meshoptimizer** | Offline geometry processing (meshoptimize/vertex/index compression) | **Candidate** (offline only) | Permissive (MIT); strong maintenance; approved ONLY if it never affects runtime pipeline; used at import time, baked into GOTOT mesh format |
| **cgltf** | glTF file parsing | **Candidate** (primary, offline import) | Permissive (MIT); single-header; lightweight; chosen as the primary parser over fastgltf for simpler single-header integration and higher API stability; output feeds GOTOT-owned mesh import |
| **fastgltf** | glTF file parsing (fast path) | **Reference-only** | Permissive (MIT); NOT integrated — alternative to cgltf kept as reference for parsing patterns and evaluation evidence only |
| **Vulkan Memory Allocator (VMA)** | GPU memory allocation | **Deferred** | Blocked until the RHI/memory architecture decision is made. GOTOT's current allocation strategy is GOTOT-owned; integrate only if the RHI decision makes VMA a leaf utility |
| **Khronos Vulkan samples** | Rendering patterns/correctness reference | **Reference-only** | Never linked; used to validate ordering, synchronization, and descriptor patterns |
| **NVIDIA Vulkan samples** | Rendering patterns/correctness reference | **Reference-only** | Never linked; used for GOTOT sample-level validation only |
| **AMD FidelityFX SDK** | GPU techniques/optimizations | **Deferred** | Techniques may inform GOTOT-owned implementations; direct dependency only after explicit evaluation and license review |
| **Shader/compiler ecosystem** (glslang, shaderc, SPIRV-Reflect, etc.) | Compiling/validating GLSL to SPIR-V, reflection | **Candidate** | Likely needed for offline shader compile pipeline; must be leaf tooling; permissive licenses; never defines GOTOT shader model |

Classification is provisional and must be confirmed by the Dependency Approval Workflow (Section 20) before integration.

## 06 — Runtime vs Offline Tooling

- **Runtime:** linked into GOTOT-NEXT executable. Strictest policy: GOTOT-owned abstraction boundary, permissive license, pinned version, availability of a removal plan.
- **Offline tooling:** runs at build/content time (import, mesh optimization, shader compilation). Output is baked into formats GOTOT-NEXT owns. Runtime pipeline must have zero knowledge of, and zero dependency on, offline tools.
- A component may never migrate from offline to runtime without a fresh approval using the Section 20 workflow.

## 07 — Abstraction / Wrapper Policy

- Every integrated dependency sits behind a GOTOT-owned wrapper that:
  - Exposes only GOTOT-conforming interfaces.
  - Hides the dependency's types, errors, allocation behavior, and version-dependent APIs from the rest of GOTOT-NEXT.
- The wrapper is the only place that may `#include` or link the dependency.
- The wrapper must be small enough to be rewritten without architecture change.
- Allowed: thin internal adapters (C++ class / function boundary).
- Forbidden: leaking dependency types/pointers across GOTOT-NEXT modules; depending on dependency-internal behavior; #define patches; parallel forks kept by GOTOT-NEXT.

## 08 — License Policy

GOTOT-NEXT must consider the license before any integration. This section distinguishes the concepts; **this is not legal advice** — every final licensing decision requires review by a qualified legal reviewer (see Section 08.5).

| Consideration | Meaning | GOTOT-NEXT stance |
|---|---|---|
| **Permissive licenses** (MIT, Apache-2.0, BSD, Zlib) | Wide freedom to use/modify/redistribute; minimal obligations | Preferred for all GOTOT-NEXT dependencies |
| **Copyleft licenses** (GPL, LGPL) | Derivative works must be released under the same license | **Runtime core:** avoid. **Offline tooling:** not automatic "no," but requires case-by-case legal evaluation of build/import separation |
| **Attribution requirements** | Must credit the author (e.g., BSD 3-Clause, Apache-2.0 notice filing) | Acceptable; must be tracked in NOTICE/attribution registry |
| **NOTICE requirements** | Must reproduce the upstream license/NOTICE text with distribution | Must be complied with; tracked in dependency register |
| **Patent clauses** | License grants/revokes patent rights (e.g., Apache-2.0 §3, MPL-2.0, GPL-v3 patent provisions) | Must be reviewed for implications on GOTOT-NEXT; documented per dependency |
| **Static vs dynamic linking implications** | Copyleft obligations may differ based on how a library is linked | Re-evaluate if a dependency's linking mode changes at any point |
| **Source availability obligations** | Some licenses (e.g., AGPL) trigger source-sharing duties | AGPL-family runtime components are effectively **rejected** for GOTOT-NEXT runtime core |

Process: every candidate completes a **license review** (recorded in Section 20 workflow) — identify type, obligations, NOTICE/attribution, patent clauses, linking mode, and source-availability consequences. **No dependency may be integrated on license terms not yet reviewed.**

## 08.5 — Legal Review Process

- **Responsible:** Owner + external legal counsel (when required).
- **Trigger:** Any dependency with non-permissive license (copyleft, AGPL, patent clauses).
- **Recording:** All legal reviews recorded in `docs/legal_reviews/` (one file per dependency).
- **SLA:** Review completed before dependency enters Section 20 workflow.
- **Revalidation:** On license change, version bump with license change, or legal advisory.

## 09 — Dependency Governance

Before any candidate may be approved, the review must record, for that dependency:

1. **Why the dependency is needed** — problem it solves that GOTOT-owned code does not already solve.
2. **Alternatives considered** — including "build ourselves"; documented comparison.
3. **License review** — per Section 08.
4. **Security review** — known CVEs, attack surface, sandboxing needs (especially for format parsers / offline tools).
5. **Maintenance/activity review** — release cadence, maintainer responsiveness, bus factor, project health.
6. **API stability** — history of breaking changes; churn level.
7. **Performance impact** — cpu/gpu/memory profile, and where it applies (runtime vs offline).
8. **Abstraction boundary** — the wrapper interface and why replacing it is cheap (Section 07).
9. **Removal/replacement plan** — concrete steps to excise the dependency and substitute an alternative or GOTOT-owned code.

A dependency that fails any of 3–9 has a **recorded objection** and is not approved until resolved.

## 10 — Reference Implementation Policy

- Khronos / NVIDIA / AMD / other sample code is for **study and validation only**.
- Reading reference code is always permitted and encouraged for correctness patterns.
- Copying non-trivial reference code into GOTOT-NEXT requires: license check of the exact file, and the normal dependency approval if the code becomes part of a linked component.
- Reference reasoning may inspire GOTOT-owned implementations without imposing external structure.

## 11 — GPU / Rendering Dependencies

- Rendering architecture, render graph, and GPU Scene are GOTOT-owned (Section 03).
- GPU-level dependencies are permitted only as leaf utilities (e.g., allocators) once the RHI/memory architecture is decided — e.g., VMA is **Deferred** until then.
- Vendor SDKs (e.g., AMD FidelityFX SDK) remain **Deferred**; if later integrated, they must be wrapped behind the RHI abstraction and re-approved.

## 12 — Geometry & Mesh Processing

- Mesh runtime format and mesh runtime abstraction = **GOTOT-owned**.
- Offline geometry processing may use **meshoptimizer** (streaming of vertices/indices, mesh optimization, simplification) at import/bake time.
- Output of any external optimizer must round-trip into the GOTOT-owned mesh format so the runtime never sees the external format.
- Tool choices do not define the runtime mesh layout.

## 13 — Asset Formats

- Preferred interchange: **glTF** (external asset format).
- Parsing at import time uses **cgltf** (Candidate, primary); **fastgltf** is Reference-only (alternative studied for parsing patterns). The parsed content is converted into GOTOT-owned scene/mesh data.
- The runtime must never depend on gltf/parser types at draw time.
- Formats other than glTF require a new policy note before support.

## 14 — Shader Compilation

- GOTOT-NEXT owns its shader model and pipeline configuration.
- External compiler tooling (e.g., glslang/shaderc ecosystem, SPIRV-Reflect) is Candidate **offline tooling**: building, validating, and reflecting GOTOT-authored GLSL into SPIR-V at build time.
- Shader sources and pipeline definitions remain GOTOT-authored.
- No external tool may inject a shader stage or define a pipeline feature into GOTOT-NEXT.

## 15 — Memory Management

- GOTOT-NEXT's memory strategy is GOTOT-owned (CPU-side allocation + GPU memory architecture).
- **VMA** is **Deferred**: its case is blocked on the RHI/memory architecture decision. If adopted, VMA counts as a leaf utility behind the RHI abstraction, permissive license, wrapped.
- No dependency may dictate GOTOT-NEXT's allocation policy.

## 16 — Tooling

- Offline tooling (importers, optimizers, codegen, test shaders) is welcome where it lowers risk and is cleanly separated.
- Tooling must not be required at runtime.
- Tooling output formats must be stable and GOTOT-owned wherever that output is consumed by the runtime.
- Example posture: meshoptimizer offline; cgltf offline import (fastgltf reference-only); shader compiler toolchain offline.

## 17 — Security / Supply Chain

- Every dependency is checked at governance time (Section 09, item 4).
- Format parsers and offline processors run in bounded scope (no network, constrained input) where practical.
- Pinned versions and recorded hashes (Section 18) so provenance stays auditable.
- Integration of a dependency with unresolved CVEs or opaque provenance is **rejected** pending evidence.
- Revalidation is triggered by: major version bump, security advisory, or maintainer change.
- **SBOM (Software Bill of Materials):** Maintained for all runtime dependencies. Generated at build time, committed to `docs/sbom/`.
- **Provenance verification:** Dependency hashes verified at build time. Signed releases preferred.

## 18 — Version Pinning

- Runtime dependencies: exact-pin the version and record the source hash in the dependency register.
- Offline tooling: pin at build time; record the version used to produce committed artifacts.
- Version bumps go through a light re-review (license unchanged, API delta, security delta); runtime core bumps require the full Section 20 workflow.
- "Latest" is never acceptable as a declaration; the exact version must always be recorded.

## 19 — Evaluation Process

Stepwise gate for any candidate:
1. **Triage:** category (04), domain (11–16), runtime vs offline (06).
2. **Governance record:** complete Section 09 checklist.
3. **License review:** Section 08 findings recorded.
4. **Proof-of-concept:** wrapper integration behind GOTOT abstraction, with GOTOT-owned fallback still compiling.
5. **Benchmark:** measured impact vs GOTOT-owned alternative on the affected path.
6. **Decision:** Approve / Defer / Reject / Reference-only, with recorded rationale.
7. **Register:** version, hash, wrapper location, removal plan, license obligations.

## 20 — Dependency Approval Workflow

```
Propose → (Section 09 checklist + Section 08 license review)
        → Architect review → Owner decision
        → integrate behind wrapper (Section 07)
        → verify performance/security
        → record in dependency register (version + hash + removal plan)
        → mark Classification in the strategy table
```

- No step may be skipped. "It is open source" is not an argument by itself.
- Approval is per-dependency and per-category; a dependency approved for offline use is not approved for runtime use.
- Every approval accrues a **removal/replacement plan** in the dependency register.

## 21 — Current Approved Dependencies

At this revision: **none integrated.**

- GOTOT-NEXT runtime is dependency-free at the GPU/rendering layer; it links only against the host Godot core.
- All components in Section 03 are GOTOT-owned.
- Status: no external runtime dependency is approved or linked today.

## 21.5 — Dependency Register

The register is the authoritative record. Location: `docs/dependency_register.md`.

For each dependency (if any in future):
- Name + version + hash
- Category (runtime/offline)
- License + obligations
- Wrapper location
- Removal plan
- Approval date + approver
- SBOM entry

## 22 — Deferred Candidates

| Candidate | Reason deferred |
|---|---|
| **VMA** | Decision on RHI / memory architecture required first |
| **AMD FidelityFX SDK** | No immediate need; requires explicit evaluation + license review before use |

## 23 — Restricted / Rejected Categories

- **Restricted (requires heavy justification + legal review):** any copyleft runtime-core dependency; AGPL-family runtime components (effectively rejected); any dependency that changes GOTOT-NEXT's public data flow.
- **Rejected:** a dependency that would own the GPU Scene, visibility, render graph, or mesh runtime abstraction; a dependency granting patent risk that cannot be documented; a parser/tool with unresolved CVEs used without mitigation; any "approved-by-listing" interpretation of this document.

## 24 — Roadmap Integration

- The strategy table is reviewed at each milestone gate.
- Candidates advance only through the Section 20 workflow, synchronized with the roadmap (e.g., meshoptimizer and cgltf (fastgltf reference-only) attach to the future asset-import milestone; VMA waits on the RHI/memory milestone decision).
- No candidate may enter the codebase ahead of its owning milestone's approval.

## 25 — Definition of Done

A candidate is "done-integrated" only when **all** hold:

1. Classification recorded in the strategy table (Approved/Candidate/Deferred/Rejected/Reference-only).
2. Section 09 governance record complete with no unresolved objections.
3. Section 08 license review complete (legal review where required).
4. Wrapper in place behind a GOTOT-owned abstraction boundary (Section 07).
5. Version pinned and hash recorded (Section 18).
6. Security review on record (Section 17).
7. Performance measured on the affected path (Section 19.5).
8. Removal/replacement plan recorded (Section 20).
9. Marked explicitly **offline** or **runtime** and approved for that scope only.
10. Fully recompilable and verifiable with the dependency absent (clean fallback build).

---

## Strategy Table — Domain Responsibilities

| Domain | GOTOT-NEXT-owned responsibility | External/open-source responsibility | Runtime or offline | Candidate technology | Current status | Decision required |
|---|---|---|---|---|---|---|
| **Renderer / pipeline architecture** | Owns renderer structure, pipeline layout, pass design | None | Runtime | — | GOTOT-owned | No external involvement — closed to dependency by policy |
| **GPU Scene** | Owns layout, building, ids, mesh-id lookup path | None | Runtime | — | GOTOT-owned | No external involvement — closed to dependency by policy |
| **Visibility / culling system** | Owns algorithm and scheduling | None | Runtime | — | GOTOT-owned | No external involvement — closed to dependency by policy |
| **Render graph** | Owns graph, ordering, barriers | None | Runtime | — | GOTOT-owned | No external involvement — closed to dependency by policy |
| **Mesh runtime abstraction** | Owns mesh format, runtime access | None | Runtime | — | GOTOT-owned | No external involvement — closed to dependency by policy |
| **Memory allocation (GPU)** | Owns allocation strategy until RHI decision | Leaf allocator under wrapper (permissive) | Runtime | Vulkan Memory Allocator (VMA) | **Deferred** | RHI/memory architecture decision; then full approval workflow (20) |
| **Geometry processing** | Owns runtime mesh output; validates optimized result | Offline optimization/simplification/meshopt utility | Offline | meshoptimizer | **Candidate** | Full evaluation + license review (20) before integration |
| **glTF import** | Owns conversion into GOTOT scene/mesh data | File parsing only (permissive) | Offline | cgltf (primary; fastgltf: Reference-only) | **Candidate** | Evaluation (19) + approval (20) on cgltf; fastgltf retained as reference only |
| **Shader compilation** | Owns shader sources, pipeline config, SPIR-V disposition | Compile/validate/reflect (permissive toolchain) | Offline | shader/compiler ecosystem (glslang/shaderc/SPIRV-Reflect) | **Candidate** | Offline-tool approval (20) |
| **Technique acceleration** | Owns technique implementation | Possible leaf technique utilization if approved | Offline/runtime | AMD FidelityFX SDK | **Deferred** | Not needed now; full workflow (20) if proposed |
| **Correctness reference** | Owns result verification | Provides sample patterns only — never linked | — | Khronos Vulkan samples | **Reference-only** | None — study only |
| **Correctness reference** | Owns result verification | Provides sample patterns only — never linked | — | NVIDIA Vulkan samples | **Reference-only** | None — study only |

**Status legend:** `Approved` = linked/accepted; `Candidate` = under evaluation, not yet integrated; `Deferred` = blocked pending a decision/need; `Rejected` = evaluated and excluded; `Reference-only` = study material, never linked.

**No candidate is approved by appearing in this document.** Each must pass Sections 19–20 (and 08 for license) before any integration.