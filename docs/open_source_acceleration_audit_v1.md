# GNE — Open-Source Acceleration Audit v1

**Document:** docs/open_source_acceleration_audit_v1.md
**Version:** 1.0
**Date:** 2026-09-27
**Status:** ARCHITECT APPROVED — EXECUTION DIRECTIVE
**Owner:** GNE Architecture
**Repository:** godot-next-engine
**License:** MIT

---

## 01. Executive Decision

GNE will use mature open-source technology to accelerate development, but external projects must not replace the architectural identity of GNE.

The governing rule is:

> **BUILD what defines GNE.**
> **BORROW what accelerates GNE.**
> **INTEGRATE only what passes architecture, performance, licensing, security, and maintenance review.**
> **REJECT anything that compromises ownership or unnecessarily duplicates existing systems.**

The project currently owns and has demonstrated:

- GPU Scene foundations
- GPU-driven visibility
- frustum culling
- indirect rendering
- batch rendering
- multi-draw
- meshlets
- LOD
- cluster culling
- GPU-oriented scene representation
- production HZB
- Render Graph (scheduler)
- Resource Pool
- PBR Materials

Therefore, external renderer architectures must not replace these systems.

The purpose of this audit is to accelerate the systems that are expensive to reproduce but are not themselves the unique architectural identity of GNE.

## 02. Current Project Baseline

At the time of this audit:

| # | Milestone | Status |
|---|---|---|
| 001A–011 | GPU Core | PASS |
| 012-revised | Production HZB | PASS |
| 013 | Meshlets + LOD | PASS |
| 014 | GPU Scene Manager | PASS |
| 015 | Render Graph | PASS |
| 015.5 | Resource Pool | PARTIAL (5/8) |
| 016 | Materials | PASS |
| 017 | Textures | SPEC DRAFT |

The project must therefore avoid introducing external systems that invalidate the current architecture.

## 03. Strategic Objective
The objective is:


GNE-owned architecture
        +
mature external technology
        =
faster engine development

External dependencies are categorized by architectural ownership.
## 04. Decision Vocabulary
BUILD
GNE owns the implementation.

Use BUILD when the system:

defines the engine's identity;

directly depends on GPU Scene architecture;

determines renderer scheduling;

determines GPU-driven execution;

provides an important architectural advantage;

would be expensive to replace later;

requires deep integration with GNE-specific systems.

Examples: GPU Scene, Render Graph, GPU-driven culling, meshlet orchestration, draw generation, runtime resource graph, engine-level material binding architecture.

BORROW
Use external projects as:

references;

algorithms;

design studies;

documentation;

architecture inspiration;

benchmark references.

No runtime dependency is introduced.

This is preferred when the knowledge is more valuable than the dependency.

INTEGRATE
Use the external project directly.

Integration requires:

license verification;

pinned version/commit;

dependency-register entry;

architecture review;

benchmark;

security review;

maintenance owner;

failure/removal plan;

regression tests.

REJECT
Do not use the technology.

Reasons may include:

architectural conflict;

unacceptable dependency cost;

redundant functionality;

unsuitable language/runtime;

performance risk;

license incompatibility;

excessive maintenance burden;

replacement of GNE-owned architecture;

insufficient benefit.

REJECT does not mean the project is technically bad.
It means it is not appropriate for GNE's current architecture.

## 05. Master Decision Matrix
Technology	Domain	Decision	Runtime	Offline	Priority
Vulkan	Graphics API	BUILD/KEEP	Yes	No	P0
OpenGL	Graphics API	KEEP compatibility	Yes	No	P3
Diligent Engine	Graphics abstraction	BORROW	No	No	P1
wgpu	Graphics abstraction	BORROW	No	No	P2
bgfx	Graphics abstraction	BORROW	No	No	P2
Filament	PBR/materials/lighting	BORROW → selective INTEGRATE	Later	Yes	P0
glTF	Asset interchange	INTEGRATE	Import	Yes	P0
OpenUSD	Scene interchange	BORROW → future INTEGRATE	Later	Yes	P2
Assimp	Asset importer	INTEGRATE selectively	No	Yes	P1
Basis Universal	Texture compression	INTEGRATE	Tool/runtime support	Yes	P0
KTX2/KTX-Software	Texture container	INTEGRATE	Yes	Yes	P0
Jolt	Physics	INTEGRATE candidate	Yes	No	P1
PhysX	Physics	INTEGRATE candidate	Yes	No	P1
Bullet	Physics	BORROW / fallback candidate	Later	No	P2
meshoptimizer	Geometry processing	INTEGRATE offline	No	Yes	P0
VMA	GPU memory	DEFER	Later	No	P1
AMD FidelityFX	Rendering algorithms	BORROW/AUDIT	Later	Yes	P2
## 06. Graphics Abstraction Strategy
### 6.1 Vulkan
Decision: BUILD/KEEP — PRIMARY GRAPHICS TARGET

Vulkan remains the primary low-level graphics reference for GNE.

The project should continue to use Godot RenderingDevice while the renderer architecture matures.

Do not introduce a second RHI simply because other engines have one.

GNE ownership:

GPU resource lifetime

render scheduling

GPU Scene integration

command organization

Render Graph

descriptor/resource policy

GPU-driven execution model

Vulkan remains the underlying API.

Do not do:

text
GNE
 ↓
New GNE RHI
 ↓
Godot RenderingDevice
 ↓
Vulkan
unless a future architecture decision proves that the extra layer is necessary.

## 07. OpenGL
Decision: KEEP AS COMPATIBILITY TARGET

OpenGL is not a strategic renderer-development target.

Requirements:

maintain compatibility where Godot requires it;

avoid OpenGL-specific architecture;

do not design GPU Scene around OpenGL limitations;

do not optimize new GPU-driven systems for OpenGL first.

## 08. Diligent Engine
Decision: BORROW

Diligent provides a cross-platform graphics abstraction covering Vulkan, Direct3D, Metal and older OpenGL/GLES targets.

Its architecture is useful for studying:

backend abstraction;

resource abstraction;

pipeline abstraction;

shader handling;

cross-backend constraints.

However, replacing RenderingDevice with Diligent at this stage would introduce a second abstraction layer and significant integration cost.

Audit targets:

Study: API abstraction, Resource abstraction, Pipeline abstraction, Shader abstraction, Backend implementation, Synchronization, Descriptor/resource handling, Cross-platform constraints.

Acceptance criteria for possible future integration:

Integration may only be reconsidered if:

≥2 non-Vulkan production backends become a roadmap requirement;

RenderingDevice becomes a demonstrated architectural bottleneck;

benchmark shows measurable benefit;

integration does not require rewriting GPU Scene;

integration does not replace Render Graph ownership.

Until then: BORROW.

## 09. wgpu
Decision: BORROW

wgpu is a cross-platform Rust graphics API based around WebGPU concepts and supports Vulkan, Metal, D3D12 and OpenGL/GLES/WebGPU paths.

Its main value for GNE is architectural research.

Study: WebGPU resource model, validation, lifetime semantics, bind groups, pipeline model, backend portability, shader abstraction, native/WebGPU boundary.

Do not introduce Rust/wgpu into the current C++ renderer merely for abstraction purposes.

Reconsider only if GNE later requires:

first-class WebGPU;

browser deployment;

Rust-native engine subsystems;

a multi-backend architecture where WebGPU is a strategic target.

## 10. bgfx
Decision: BORROW

bgfx is useful as a reference for lightweight cross-platform renderer organization.

Study: backend separation, command abstraction, portability, shader workflow, platform abstraction.

Do not import bgfx as the GNE renderer.

Reason: GNE already has a GPU-driven architecture whose main value would be diluted if another renderer became the execution owner.

## 11. Filament

**Decision:** P0 — HIGH PRIORITY BORROW
**Potential future selective integration.**

Filament is one of the highest-value external references for the Materials/Lighting stage.

**Study:**

- PBR
- BRDF
- material model
- material parameters
- physical units
- lighting
- IBL
- exposure
- normal mapping
- clear coat
- anisotropy
- material compilation
- shader generation
- material packages
- Vulkan backend
- frame graph concepts

**Critical rule:**

Do NOT import the complete Filament renderer.

**Target architecture:**

```
GNE GPU Scene
       ↓
GNE Render Graph
       ↓
GNE Material System
       ↓
Filament-derived PBR knowledge
       ↓
GNE Lighting
```

not:

```
GNE
 ↓
Filament
 ↓
Filament renderer becomes owner
```

**Possible selective integration:**

Candidate areas: material compiler concepts; shader generation techniques; PBR reference implementation; lighting equations; material parameter representation.

**Acceptance criteria:**

A Filament-derived component may enter GNE only if:

1. it solves a defined GNE requirement;
2. it does not replace GPU Scene;
3. it does not replace Render Graph;
4. it has measurable performance;
5. license provenance is documented;
6. source files are individually reviewed before copying;
7. dependency size is justified;
8. regression tests exist.

---

## 12. glTF

**Decision:** P0 — INTEGRATE

glTF should become a primary asset interchange/input format.

It is appropriate because it is explicitly designed as a runtime 3D asset delivery format.

**Important distinction:**

glTF is: **interchange / delivery**

not: **GNE runtime database**

**Pipeline:**

```
Blender / DCC
      ↓
    glTF/GLB
      ↓
GNE Asset Compiler
      ↓
 ┌───────────────┐
 │ Mesh          │
 │ Meshlets      │
 │ LOD           │
 │ Bounds        │
 │ Materials     │
 │ Textures      │
 │ Animation     │
 └───────────────┘
      ↓
GNE Runtime Asset
      ↓
GPU Scene
```

**Acceptance criteria:**

glTF integration must demonstrate:

- GLB loading;
- mesh loading;
- material loading;
- texture references;
- node hierarchy;
- transforms;
- animation metadata;
- validation of malformed assets;
- conversion into GNE-owned runtime structures;
- no runtime dependency on arbitrary source-format logic after compilation.

---

## 13. OpenUSD

**Decision:** P2 — BORROW / FUTURE INTEGRATE

OpenUSD should not replace glTF.

**Use cases:** large scene authoring; DCC interoperability; layered scenes; variants; world composition; large production pipelines.

**Target:**

```
USD
 ↓
GNE World/Asset Compiler
 ↓
GNE Runtime Representation
```

not:

```
USD
 ↓
GPU Scene
```

**Acceptance criteria for future integration:**

Do not integrate until the project has:

- a defined large-world/world-authoring requirement;
- a USD ingestion specification;
- memory/IO benchmarks;
- dependency/license inventory;
- clear separation between authoring and runtime.

---

## 14. Assimp

**Decision:** P1 — SELECTIVE OFFLINE INTEGRATION

Assimp is useful as a compatibility importer for formats that GNE does not want to implement itself.

**Supported role:**

```
FBX
OBJ
DAE
3DS
...
 ↓
Assimp
 ↓
Intermediate Representation
 ↓
GNE Compiler
```

**Forbidden role:**

Assimp must not become: runtime scene database; renderer scene representation; material authority; GPU Scene owner.

**Acceptance criteria:**

Integration requires:

- offline-only operation;
- deterministic conversion;
- malformed-file handling;
- format coverage tests;
- conversion-time benchmark;
- output validation;
- license notice handling.

---

## 17. meshoptimizer

**Decision:** INTEGRATE — OFFLINE

Current project status already identifies meshoptimizer as a pinned offline dependency.

**Current documented dependency:**

- **Version:** v1.2
- **Pinned commit:** `9d9890c73011d75920af614485296d1e03e95448`
- **License:** MIT
- **Role:** Offline geometry/mesh processing
- **Location:** `tools/meshlet_import/third_party/meshoptimizer`

It remains offline tooling.

Do not make the runtime renderer depend on meshoptimizer unless a later requirement explicitly justifies it.

---

## 18. Physics Architecture

GNE must define an internal physics abstraction.

```
             GNE Physics API
                    │
       ┌────────────┼────────────┐
       │            │            │
      Jolt        PhysX        Bullet
```

Gameplay code must depend on: **GNE Physics API**

not: `Jolt::...`, `Px...`, `bt...`

This allows backend replacement.

---

## 19. Jolt Physics

**Decision:** P1 — PRIMARY PHYSICS CANDIDATE

Jolt should receive the first serious prototype evaluation.

**Evaluate:** rigid bodies; broad phase; narrow phase; raycasts; shape casts; character movement; joints; sleeping; multithreading; SIMD; memory; scalability; determinism requirements.

**Benchmark scene:**

```
10,000 static bodies
1,000 dynamic bodies
100 moving characters
10,000 raycasts
1,000 shape casts
joint stress test
```

**Record:** physics ms, CPU utilization, memory, broad-phase time, narrow-phase time, raycast time, job scheduling time, frame-time P95/P99.

---

## 20. PhysX

**Decision:** P1 — HIGH-VALUE CANDIDATE

PhysX must be benchmarked against Jolt rather than selected by reputation.

**Evaluate:** rigid bodies; collision; character controller; joints; vehicles; multithreading; memory; scalability; tooling; integration cost.

**Special consideration:**

Because PhysX has strong ecosystem maturity and NVIDIA alignment, it deserves a serious technical evaluation.

However: **"ecosystem strength is not itself an acceptance criterion."**

Only benchmark results and integration requirements determine the final backend.

## 15. Basis Universal

text
Decision: P0 — INTEGRATE

Texture compression is considered infrastructure rather than engine identity.

Target:
Source Texture → Offline Encoder → Basis representation → KTX2
→ GNE Asset Compiler → GPU-native representation

Requirements: offline compression, mip generation, quality presets,
format selection, GPU compatibility detection, streaming, residency,
memory accounting.

Benchmark: source size, compressed size, encode time, decode/transcode
time, upload time, VRAM usage, visual quality, startup time,
streaming bandwidth.
## 16. KTX2 / KTX-Software

text
Decision: P0 — INTEGRATE

KTX2 should become the preferred texture container direction for the
asset pipeline.

Why: The engine needs a production texture path compatible with:
compression, mipmaps, streaming, GPU upload, metadata, asset caching.

Prototype: PNG/TGA/EXR → Texture compiler → KTX2 → GPU upload →
compressed GPU texture.

Measure: package size, load time, CPU work, GPU upload, VRAM,
quality, streaming behavior.

Every KTX dependency and third-party component must be recorded
individually in the dependency register.

## 21. Bullet Physics

**Decision:** P2 — REFERENCE / FALLBACK CANDIDATE

Bullet remains valuable because of: maturity; broad adoption; permissive zlib licensing; extensive physics functionality.

But the first implementation benchmark should focus on Jolt and PhysX.

Bullet can remain: reference, compatibility candidate, fallback, research target — unless benchmark results justify direct integration.

---

## 22. Physics Benchmark Protocol

All candidates must use the same test harness.

**Test A — Static World:** 100,000 static colliders. Measure: initialization; memory; broad phase; query time.

**Test B — Dynamic Bodies:** 1,000 dynamic rigid bodies. Measure: simulation; sleeping; island solving; memory.

**Test C — Raycast:** 10,000 raycasts/frame. Measure: average; P95; P99.

**Test D — Character:** 100 simultaneous characters. Measure: controller update; collision; ground detection.

**Test E — Stress:** 10,000 dynamic objects. Measure: frame pacing; CPU; memory; simulation stability.

---

## 23. Graphics Benchmark Protocol

All renderer candidates must be tested against the existing GNE benchmark.

**Baseline:** 100K instances, 1M instances, 10M instances.

**Metrics:** CPU submission, GPU culling, GPU LOD, meshlet culling, draw generation, indirect submission, GPU memory, VRAM, frame time, P95, P99.

No external renderer may be declared faster based on a different workload.

---

## 24. Materials Benchmark Protocol

The Materials system must eventually test: 1,000 / 10,000 / 100,000 material instances with: metallic/roughness; normal maps; AO; emissive; alpha; clear coat; anisotropy; IBL.

**Measure:** shader permutations, pipeline count, shader compilation time, material upload time, descriptor usage, GPU frame time, CPU frame time, VRAM.

---

## 25. Texture Benchmark Protocol

**Test:** 1K textures, 2K textures, 4K textures, 8K textures.

**Compare:** PNG, JPG, raw GPU texture, KTX2, Basis-compressed KTX2.

**Metrics:** disk size; loading time; CPU decode; GPU upload; VRAM; visual quality; streaming bandwidth.

---

## 26. Asset Benchmark Protocol

**Test:** 10, 100, 1,000, 10,000 assets.

**Measure:** import time; compiler time; runtime load; memory; cache hit rate; GPU upload; total package size.

The test must distinguish: **source import time** from: **runtime load time**.

---

## 27. Graphics Abstraction Acceptance Criteria

A graphics abstraction can only be integrated if:

**Architecture:**

- it does not replace GPU Scene;
- it does not own Render Graph;
- it does not dictate GNE resource lifetime;
- it supports the required GPU-driven path.

**Performance:**

It must not regress the current benchmark beyond an agreed threshold.

**Initial threshold: ≤ 5% regression** for comparable workloads.

Any larger regression requires explicit architecture approval.

**Maintenance:**

- pinned version;
- reproducible build;
- documented update process;
- security monitoring;
- removal plan.

---

## 28. Material Dependency Acceptance Criteria

A material technology is accepted only if:

- PBR correctness is validated;
- reference images exist;
- shader compilation is measurable;
- permutations are controlled;
- GPU cost is measured;
- material parameters map cleanly to GNE;
- Render Graph integration is clean;
- dependency does not force a foreign renderer architecture.

---

## 29. Asset Dependency Acceptance Criteria

Every asset dependency must satisfy:

Deterministic, Reproducible, Offline-capable, Validated, Version-pinned, License-documented, Crash-resistant, Benchmark-tested.

For runtime dependencies additionally: Thread-safe, Memory-bounded, Streaming-compatible, Failure-recoverable, Profilable.

---

## 30. Physics Dependency Acceptance Criteria

A physics backend cannot be selected solely from raw benchmark speed.

Evaluation must include:

| Category | Requirement |
|---|---|
| Performance | Benchmark |
| Stability | Stress test |
| Memory | Measured |
| Multithreading | Measured |
| Character | Required |
| Queries | Required |
| Joints | Required |
| Debugging | Required |
| Integration | Measured |
| Licensing | Verified |
| Maintenance | Evaluated |
| Removal | Possible |

## 31. License Governance
Every external dependency must record:

Project, Version, Commit, License, Copyright, Third-party licenses, Runtime/offline, Static/dynamic, Modified?, Source files copied?, NOTICE requirements, Patent clauses, Security status.

"Open source" is not sufficient evidence for acceptance.

License information must be checked at the exact pinned version/commit used by GNE.

This document is an engineering governance document, not legal advice.

## 32. Dependency Register Schema
Each approved dependency must have:

text
name:
version:
commit:
license:
domain:
runtime: true/false
offline: true/false
source_modified: true/false

purpose:

GNE_owned_boundary:

external_owned_boundary:

integration_points:

benchmark:

acceptance_tests:

security_review:

license_review:

maintenance_owner:

update_policy:

removal_plan:

status:
## 33. Supply Chain Rules
No dependency may be added from an unverified mirror.

Preferred order:

text
Official repository
        ↓
Pinned commit/tag
        ↓
Hash verification
        ↓
License verification
        ↓
Build verification
        ↓
Benchmark
        ↓
Integration
Do not blindly copy random source files from examples.

Do not vendor a dependency before its ownership and license are understood.

## 34. Execution Priority
P0 — Immediate
A1 014 GPU Scene Manager

A2 Filament Audit

A3 glTF Asset Pipeline Audit

A4 KTX2/Basis Audit

A5 Render Graph design

A6 Materials architecture

These systems establish the next architectural layer.

P1 — Next
B1 Assimp offline importer prototype

B2 Jolt benchmark

B3 PhysX benchmark

B4 Diligent architecture audit

B5 GPU memory architecture review / VMA revisit

P2 — Later
C1 OpenUSD

C2 wgpu

C3 bgfx

C4 Bullet

C5 FidelityFX

P3 — Compatibility
D1 OpenGL

No architectural investment unless required by platform support.

## 35. Recommended Implementation Sequence
text
014 GPU Scene Manager
        ↓
015 Render Graph
        ↓
Filament Material Audit
        ↓
GNE Material Architecture
        ↓
glTF Asset Compiler
        ↓
KTX2/Basis Texture Pipeline
        ↓
GPU Resource/Streaming System
        ↓
Lighting
        ↓
Shadows
        ↓
Physics API
        ↓
Jolt/PhysX Benchmark
        ↓
Physics Backend
        ↓
Production Integration
## 36. What Must NOT Happen
Forbidden 1
Do not replace GNE Render Graph with an external renderer merely because it already has a frame graph.

External implementations may be studied.

Forbidden 2
Do not replace GPU Scene with:

Filament scene management;

bgfx scene management;

Diligent scene management;

USD runtime scene representation.

Forbidden 3
Do not add three graphics abstractions simultaneously.

Never create:

text
RenderingDevice
+
Diligent
+
wgpu
+
bgfx
inside the same runtime without an explicit architecture decision.

Forbidden 4
Do not introduce physics directly into gameplay code.

Always:

text
Gameplay
 ↓
GNE Physics API
 ↓
Physics Backend
Forbidden 5
Do not make source formats runtime dependencies unnecessarily.

Prefer:

text
Source
 ↓
Compiler
 ↓
GNE Runtime Asset
## 37. Open-Source Acceleration Work Packages
OSA-001 — Filament
Deliver: docs/open_source/filament_acceleration_audit.md
Status: PENDING

OSA-002 — glTF
Deliver: docs/open_source/gltf_runtime_pipeline.md
Prototype: GLB → GNE intermediate → GPU-ready asset

OSA-003 — Texture
Deliver: docs/open_source/texture_pipeline_audit.md
Prototype: PNG → KTX2/Basis → GPU

OSA-004 — Physics
Deliver: docs/open_source/physics_backend_benchmark.md
Candidates: Jolt, PhysX, Bullet

OSA-005 — Graphics Abstraction
Deliver: docs/open_source/graphics_abstraction_audit.md
Candidates: Diligent, wgpu, bgfx

OSA-006 — Scene Interchange
Deliver: docs/open_source/world_scene_format_audit.md
Candidates: glTF, OpenUSD

## 38. Benchmark Reporting Standard
Every benchmark report must contain:

Environment, GPU, CPU, RAM, Driver, API, Engine commit, Dependency commit, Build configuration, Scene size, Test duration, Warm-up frames, Average, P50, P95, P99, Peak, Memory, VRAM, Correctness result, Regression result.

Example:

text
GPU:       RTX 3070 8GB
CPU:       Ryzen 5 7600
RAM:       16GB
API:       Vulkan
Build:     Godot 4.8.dev custom
Engine:    GNE <commit>

Test:      1,000,000 instances
Warm-up:   300 frames
Measure:   1000 frames
## 39. Correctness Before Performance
A dependency cannot pass because it is fast if it is incorrect.

Required order:

text
Correctness
    ↓
Determinism / Stability
    ↓
Memory Safety
    ↓
Performance
    ↓
Integration Cost
    ↓
Maintenance Cost
## 40. Performance Thresholds
Initial engineering thresholds:

Renderer: ≤ 5% regression relative to equivalent GNE workload.

Asset import: No fixed target until baseline measurements exist.

Texture loading: Must demonstrate measurable improvement in: storage; upload; streaming; residency.

Physics: Backend selection must be based on the complete benchmark matrix rather than a single frame-time number.

## 41. Removal Test

Every integrated dependency must answer:

> Can GNE remove this dependency without rewriting the engine architecture?

If the answer is **no:** REVIEW REQUIRED

If removing it requires rewriting GPU Scene, Render Graph, or engine-level ownership: **DO NOT INTEGRATE** unless there is an explicit Architecture Board decision.

---

## 42. Architecture Ownership Matrix

| System | GNE owns | External may provide |
|---|---|---|
| GPU Scene | YES | Reference |
| Meshlet orchestration | YES | meshoptimizer offline |
| Culling | YES | Algorithms/reference |
| LOD policy | YES | mesh processing tools |
| Draw generation | YES | Reference |
| Render Graph | YES | Reference |
| Materials API | YES | Filament research |
| PBR equations | YES | Filament reference |
| Texture compression | No | Basis/KTX |
| Asset import | Compiler-owned | Assimp/glTF/USD |
| Runtime asset format | YES | External formats as input |
| Physics API | YES | Jolt/PhysX/Bullet |
| Physics implementation | No | Backend |
| Graphics API | Vulkan/Godot | Khronos |
| Shader tools | Shared | External compiler ecosystem |
| Memory allocator | Pending | VMA candidate |
| World authoring | Future | OpenUSD candidate |

---

## 43. Final Technology Decisions

### BUILD

GNE GPU Scene, GNE Render Graph, GNE GPU culling, GNE meshlet orchestration, GNE LOD policy, GNE draw generation, GNE runtime asset representation, GNE material API, GNE physics API, GNE resource ownership, GNE renderer scheduling.

### BORROW

Diligent Engine, wgpu, bgfx, Filament architecture/PBR research, OpenUSD concepts, Bullet architecture/reference, FidelityFX concepts.

### INTEGRATE

Vulkan, glTF, meshoptimizer (offline), KTX2, Basis Universal, Assimp (offline/selective).

Potential future: Jolt, PhysX, Filament components, OpenUSD, VMA — subject to benchmarks/audits.

### REJECT FOR CURRENT ARCHITECTURE

Full Filament renderer, Full Diligent renderer abstraction, Full bgfx renderer, Full wgpu renderer, External scene-management replacement, External GPU Scene replacement, External Render Graph replacement.

This category means: rejected as the current architectural owner, not: technically inferior projects.

---

## 44. Audit Completion Criteria

The overall Open-Source Acceleration Audit v1 is complete when:

- every P0 technology has an audit;
- every P1 technology has an initial evaluation;
- every integrated dependency has a dependency-register entry;
- licenses are verified at pinned versions;
- benchmark methodology is standardized;
- no external dependency owns GNE architecture;
- removal paths exist;
- security/supply-chain review exists;
- performance regression criteria are enforced;
- 014 remains unaffected;
- 015 can consume the resulting architecture;
- Materials can consume Filament-derived knowledge without depending on Filament renderer;
- Asset Pipeline can consume glTF/KTX2/Basis;
- Physics can consume a backend through GNE Physics API.

---

## 45. Architect Decision

The project is not becoming a collection of external engines.

It is becoming:

```
GNE
                     │
       ┌─────────────┼─────────────┐
       │             │             │
   GPU Architecture  Runtime      Tooling
       │             │             │
 GPU Scene       Materials      glTF
 Meshlets        Lighting       Assimp
 Culling         Shadows        KTX2
 LOD             GI             Basis
 Draw Gen        Physics API    meshoptimizer
 Render Graph
       │
       └──────────────┐
                      │
              Mature OSS
              Acceleration
                      │
      ┌───────────────┼───────────────┐
      │               │               │
   Filament       Jolt/PhysX       Khronos
   research       physics          formats
```

The architectural principle is therefore:

> **Own the engine. Borrow the knowledge. Integrate the infrastructure. Never outsource the identity.**

---

## 46. Immediate Manager Instructions

The manager must execute in this order:

1. Do NOT modify 014 scope.
2. Execute 014.
3. Create OSA documentation directory.
4. Create Filament audit.
5. Create KTX2/Basis audit.
6. Create graphics abstraction audit.
7. Create physics benchmark specification.
8. Create KTX2/Basis audit.
9. Update dependency register.
10. STOP before broad integration.
11. Present benchmark/audit evidence.
12. Architecture approval.
13. Only then integrate selected dependencies.

No broad dependency integration is authorized merely because this document exists.

The audit authorizes **investigation**.
The benchmark and architecture review authorize **integration**.

---

## 47. Final Status

**Open-Source Acceleration Audit v1**

**STATUS:** APPROVED

| Technology | Status |
|---|---|
| 014 | EXECUTE |
| Filament | AUDIT |
| glTF | INTEGRATE — ASSET INPUT STANDARD |
| KTX2/Basis | INTEGRATE — TEXTURE PIPELINE |
| Assimp | OFFLINE INTEGRATION CANDIDATE |
| Jolt | BENCHMARK |
| PhysX | BENCHMARK |
| Bullet | REFERENCE |
| Diligent | AUDIT / BORROW |
| wgpu | REFERENCE |
| bgfx | REFERENCE |
| OpenUSD | FUTURE AUTHORING PIPELINE |
| VMA | DEFERRED UNTIL MEMORY/RHI REVIEW |

**STOP CONDITION:**

Do not convert this audit into mass dependency integration.
Each dependency must pass its own acceptance gate.
