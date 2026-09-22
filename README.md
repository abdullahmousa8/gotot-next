<div align="center">

# GOTOT-NEXT

**A GPU-driven rendering prototype built as a custom C++ module for Godot 4.8.dev.**

*One camera → one culling pass → one indirect draw. No per-object CPU loops.*

[![Godot](https://img.shields.io/badge/Godot-4.8.dev-478CBF?logo=godot-engine&logoColor=white)](https://godotengine.org)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D4?logo=windows&logoColor=white)](#-build-from-source)
[![Renderer](https://img.shields.io/badge/renderer-Vulkan-AC162C?logo=vulkan&logoColor=white)](#-architecture)
[![Language](https://img.shields.io/badge/language-C%2B%2B%20%7C%20GDScript-00599C?logo=cplusplus&logoColor=white)](#-repository-layout)
[![Status](https://img.shields.io/badge/status-research%20prototype-orange)](#-project-status)
[![Milestones](https://img.shields.io/badge/milestones-15%20passed-brightgreen)](#-milestones)

[English](#-english) · [العربية](#-نظرة-عامة-بالعربية) · [Progress report](docs/progress_report.md) · [Engine spec](docs/engine_spec_v2.md)

</div>

---

## English

### Overview

**GOTOT-NEXT** is an experimental, GPU-driven rendering pipeline implemented as a standalone C++ module (`modules/gotot_render`) for **Godot v4.8.dev**. It uses a local `RenderingDevice` (Vulkan) — isolated from Godot's main renderer in the same style as `LightmapperRD` — to prove a fully GPU-resident scene flow:

> **GPU Scene → Frustum Culling → HZB Occlusion → Compaction → Indirect Arguments → GPU-driven Rasterization → Real Geometry → Presentation**

The design goal is the classic "one draw" architecture: keep instance data in SoA GPU buffers, cull and compact entirely on the GPU, and issue a **single indexed indirect draw** whose arguments are produced by a compute pass. There is no CPU loop over instances anywhere in the hot path.

This repository contains the module source, the Godot demo project used for verification, the architecture specification, and a full progress report.

### Highlights

- **Fully GPU-resident scene** — 100,000 instances stored as Structure-of-Arrays buffers, filled by a single parallel compute shader.
- **Deterministic frustum culling** — sphere-vs-plane tests with atomic counting; GPU result matches a CPU reference **exactly**.
- **Hierarchical Z occlusion (HZB)** — a 10-level `R32UI` depth pyramid built and consumed entirely on the GPU.
- **GPU-produced indirect arguments** — a compute pass writes a `VkDrawIndexedIndirectCommand` layout directly.
- **Real geometry path** — real vertex buffer, real index buffer, real vertex format, and an **indexed indirect draw** (GOTOT-008A).
- **Viewport integration bridge** — the rendered framebuffer is displayed live inside a Godot window through a CPU readback → `ImageTexture` → `TextureRect` bridge (GOTOT-007A).
- **Honest, measured numbers** — every milestone reports actual measurements, not targets.

### Architecture

```
                         GODOT
                           │
                    Camera / Viewport
                           │
                           ▼
        ┌──────────────────────────────────┐
        │            GOTOT-NEXT            │
        │                                  │
        │   GPU Scene (SoA instance data)  │
        │              │                   │
        │              ▼                   │
        │      Frustum Culling             │
        │              │                   │
        │              ▼                   │
        │        HZB Occlusion             │
        │              │                   │
        │              ▼                   │
        │   Visibility / Compaction        │
        │              │                   │
        │              ▼                   │
        │    Indirect Draw Arguments       │
        │         │             │          │
        │         ▼             ▼          │
        │  Billboard Path   Real Mesh Path │
        │   (GOTOT-005)      (GOTOT-008A)  │
        │         │             │          │
        │         └──────┬──────┘          │
        │                ▼                 │
        │      GPU-driven Rasterization    │
        └────────────────┬─────────────────┘
                         │
                         ▼
             Offscreen Framebuffer (1920×1080)
                         │
                         ▼
          CPU Readback → ImageTexture → TextureRect
                         │
                         ▼
                      SCREEN
```

The **billboard path** (GOTOT-005 / 007A) and the **real mesh path** (GOTOT-008A) are independent and additive. The mesh path never replaces the billboard path.

### Milestones

| # | Milestone | Status | Key evidence |
|---|-----------|:------:|--------------|
| 001A | GPU Device | ✅ PASS | Lazy local `RenderingDevice`, `is_gpu_ready()` |
| 001B | GPU Scene | ✅ PASS | 100K SoA instances, fill ≈ 0.36 ms, 4.96 MB |
| 002 | Frustum Culling | ✅ PASS | GPU == CPU == **377** (deterministic) |
| 003 | Indirect Arguments + Compaction | ✅ PASS | `args = [6, 377, 0, 0, 0]` |
| 004 | HZB Occlusion | ✅ PASS | 377 → **375**, no-occluder control = 377 |
| 005 | Actual Indirect Draw | ✅ PASS | 521 colored px, 112 projected centers matched |
| 006 | 100K / 1M / 10M Scaling | ✅ PASS | 36,744 visible @ 10M |
| 007A | Viewport Integration Bridge | ✅ PASS | visible 356..384, changed 232/239 frames |
| 008A | Real Geometry Proof | ✅ PASS | 8-vertex / 36-index cube, `args = [36, N, 0, 0, 0]`, green 271..621 px, magenta 0 |
| 008B | Multi-Instance Mesh Proof | ✅ PASS | 10,000 instances; GPU==CPU visible; drawargs + full-frame determinism; `sig=v3905\|36\|3905\|g25765\|c0\|5004\|9997\|h176\|m0\|3905\|f150` |
| 009 | Real Depth Buffer | ✅ PASS | `D32_SFLOAT` depth test/write; 009A fg==green (458/458), 009B masked_eq 458/458 bad=0; DET stable (009A/009B) |
| 010 | Batch Instance Rendering | ✅ PASS | 3 meshes (cube/tetra/octa) × 2 instances; batch_count=3; args deterministic `[36,2,0,0,0][12,2,36,8,2][24,2,48,12,4]`; depth `dC<dT<dO`; DET stable |
| 011 | Multi-Draw / Multi-Batch | ✅ PASS | 64 meshes / 128 instances; PER_MESH→64 draws, GROUPED/REORDERED→**5** groups/draw_calls/indirect; `batch_order==[0..63]`; dynamic GPU-written draw count; DET stable; sigs `st0/1/2` all pixel-identical |
| 013 | Meshlets / LOD / Cluster Culling | ✅ PASS | 1M-tri LOD0 (1,048,576), 18,613 meshlets; instance LODs `[0,1,1,2,2,0]`; 3-pass software rasterizer with **coherent 64-bit winner** `covered==winner==76,685`; per-LOD px `[5853,1265,559566]`; DET stable (`d1`) |
| 014 | GPU Scene Manager | ✅ PASS | SSBO SoA scene DB, 64-B records, unified ID space; **1,048,576 instances** (`active=1048576`, ssbo=121.6 MB); 16-MB ring + compute apply (20,000 deltas, zero critical-path readback); 013 ordinal hand-off (fnv=3106528256 preserved); 4096 instances/8 meshes → **5 draw calls**; DET stable (`sig=v14`, `d1`) |

See [`docs/progress_report.md`](docs/progress_report.md) for the full technical report (Arabic).

### Repository layout

```
gotot-next/
├── modules/gotot_render/          # The C++ RenderingDevice module
│   ├── gotot_render_server.h/.cpp #   Core: GPU scene, culling, HZB, raster, mesh
│   ├── gotot_render.h/.cpp        #   Module entry object
│   ├── register_types.h/.cpp      #   Godot module registration
│   ├── config.py / SCsub          #   SCons build glue
├── demo/gpu_smoke/                # Godot demo / verification project
│   ├── project.godot
│   ├── main.gd / main.tscn        #   001A–006 regression smoke test
│   ├── main_007.gd / main_007.tscn#   007A viewport bridge (billboards)
│   ├── main_008.gd / main_008.tscn#   008A real geometry (cubes)
│   ├── main_008b.gd / main_008b.tscn# 008B multi-instance mesh
│   ├── main_009.gd / main_009.tscn#   009 real depth buffer (009A/009B)
│   ├── main_010.gd / main_010.tscn#   010 batch instance rendering
│   ├── main_011.gd / main_011.tscn#   011 multi-draw / multi-batch (a/b/c strategies)
│   ├── main_013.gd / main_013.tscn#   013 meshlets / LOD / cluster culling
│   ├── main_014.gd / main_014.tscn#   014 GPU scene manager (SSBO scene DB)
│   ├── mesh_013_torus.gomlet        #   013 offline-built meshlet asset (25 MB)
│   ├── main_demo.gd / main_demo.tscn# 011 interactive demo
│   └── camera_controller.gd / hud.gd# demo FPS camera + HUD
├── tools/
│   └── meshlet_import/              # Offline .gomlet builder (build-time only)
│       ├── main.cpp / build.ps1     #   CLI: rebuilds demo/gpu_smoke/mesh_013_torus.gomlet
│       └── third_party/meshoptimizer#   vendored meshoptimizer v1.2 (MIT, offline)
├── docs/
│   ├── engine_spec_v2.md          # Architecture specification (v2.0)
│   ├── progress_report.md         # Full progress report (Arabic)
│   ├── open_source_system_strategy_v1.md # Dependency + license strategy (v1)
│   ├── dependency_register.md     # Dependency register (strategy §21.5)
│   ├── legal_reviews/             # Legal review records (strategy §08.5)
│   └── sbom/                      # SBOM artifacts (strategy §17)
└── README.md
```

### Build from source

**Prerequisites**

- Windows 10/11 with the **MSVC** toolchain (Visual Studio 2022)
- **Python 3** and **SCons** (`pip install scons`)
- **Vulkan** drivers
- A **Godot 4.8.dev source tree** (this module is compiled into the engine)

**Command** (run from your Godot source root, e.g. `godot-master/`):

```powershell
scons platform=windows target=editor dev_build=yes `
      custom_modules="C:\path\to\gotot-next\modules" -j6
```

The build produces:

```
bin\godot.windows.editor.dev.x86_64.exe
bin\godot.windows.editor.dev.x86_64.console.exe
```

> **Note:** On machines with App Control / Device Guard policies that block direct
> execution of freshly built binaries, the demos can be launched through a
> scheduled task (`schtasks /Create /Run /Delete`) that runs a `.bat` wrapper and
> redirects output to a log file.

### Running the demos

The demo project lives in `demo/gpu_smoke/`. Launch a specific scene explicitly:

```powershell
# 001A–006 regression smoke test
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke

# GOTOT-007A — viewport bridge, billboard path
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_007.tscn

# GOTOT-008A — real geometry, indexed indirect draw
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_008.tscn

# GOTOT-008B — multi-instance mesh rendering
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_008b.tscn

# GOTOT-009A — real depth buffer, front-only reference run
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_009.tscn -- --front-only

# GOTOT-009B — real depth buffer, full scene (A+B+C+D)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_009.tscn

# GOTOT-010 — batch instance rendering (3 meshes, multi-draw)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_010.tscn

# GOTOT-011 — multi-draw / multi-batch (--strategy: 0=per_mesh 1=grouped 2=reordered)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_011.tscn -- --strategy=2

# GOTOT-011 interactive demo (WASD + mouse, R switches strategy live, F12 screenshot)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_demo.tscn

# GOTOT-011 interactive demo — self-evidence run (scripted sweep + screenshot, exits 0)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_demo.tscn -- --test

# GOTOT-013 — meshlets + LOD + cluster culling (uses mesh_013_torus.gomlet)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_013.tscn

# GOTOT-014 — GPU scene manager (SSBO scene DB, ≥1M instances, GPU-driven updates)
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_014.tscn
```

Rebuild the offline meshlet asset (MSVC required; runtime does not use meshoptimizer):

```powershell
powershell -ExecutionPolicy Bypass -File <repo>\tools\meshlet_import\build.ps1
```

Each demo prints its own objective evidence and finishes with `PASS` or `FAIL code=...`.

### Results

**GOTOT-006 — scaling (100K / 1M / 10M instances):**

| count | fill_ms | cull_ms | finalize_ms | visibility_ms | raster_ms | visible | buffers |
|------:|--------:|--------:|------------:|--------------:|----------:|--------:|--------:|
| 100K | 0.38 | 0.10 | 0.12 | 0.53 | 0.16 | 364 | 4.96 MB |
| 1M | 3.33 | 0.15 | 0.12 | 0.60 | 0.16 | 3,670 | 49.6 MB |
| 10M | 1.48 | 0.50 | 0.12 | 0.92 | 0.18 | 36,744 | 495.9 MB |

**GOTOT-008A — real geometry proof:**

- Mesh: 8 vertices (`R32G32B32_SFLOAT`), 36 `UINT32` indices.
- Indirect args observed: `[36, N, 0, 0, 0]` where `N == visible` every frame.
- Exact final-frame pixel scan: **green = 305**, **magenta = 0** (proves real geometry, not the old billboard).
- Camera orbit changes geometry: `changed_frames = 236 / 239`.

### Roadmap

Completed through **014**. Planned direction:

```
008  Real GPU Mesh Buffer        ✅ (008A proof + 008B multi-instance)
009  Real Depth Buffer           ✅ (009A/009B proof)
010  Batch Instance Rendering    ✅ (010 proof — 3 meshes, multi-draw)
011  Multi-Draw / Multi-Batch    ✅ (12.8x reduction)
012  Production HZB              ⏸️ DEFERRED (projection fix needed)
013  Meshlets / LOD              ✅ (013 proof — meshlets + LOD + cluster culling)
014  GPU Scene Manager           ✅ (014 proof — SSBO scene DB, ≥1M instances)
015  Render Graph                ⏳
016+ Materials / Lighting / Shadows
─────────────────────────────────────────
     GototMesh → Meshlets → Meshlet Culling → LOD
              → Virtual Shadows → Hybrid GI
```

### Performance notes & honest caveats

The numbers above are from a **Benchmark Prototype v0.1**. In this specific test the
culling / finalize / raster stages stay close to flat across 100K–10M because the
prototype is GPU-parallel and **does not yet contain real mesh-complexity draw cost**.

This is **not** a claim that GOTOT-NEXT renders 10M objects in a game at 60 FPS. The
current prototype has: simplified resources, a single quad/cube, simple shaders, a
dedicated offscreen framebuffer, and readback for verification. It has **no**
production material system, **no** real mesh/vertex streams at scale, **no**
production depth buffer, and **no** full render graph or synchronization.

### Notes & limitations

- **CPU readback is a temporary bridge**, not the final presentation path. It costs
  ~2.2–2.8 ms/frame and ~8 MB/frame at 1920×1080. The final path will use a
  zero-copy / engine-integrated presentation.
- The raster target is a **fixed 1920×1080**; the demo window is 1152×648 (same 16:9 aspect).
- The mesh path reuses the existing indirect argument buffer, so the billboard
  `gpu_drawargs_finalize()` and the mesh path must not be interleaved in one run.
- Depth buffer was added in GOTOT-009 (`D32_SFLOAT`, `LESS_OR_EQUAL`, `clear=1.0`).
  The billboard path (005 / 007A) still runs without depth testing. The mesh path
  (008A+) now has per-fragment depth ordering.
- The module deliberately avoids `RenderingServer` integration, RHI changes,
  zero-copy, Vulkan interop, mesh shaders, a material system, and a render graph —
  these are reserved for later milestones.

### License

GOTOT-NEXT source is licensed under the **MIT License** — see [`LICENSE`](LICENSE).

Dependency/third-party licensing (permissive-only runtime policy, legal review process, dependency register, SBOM) is governed by [`docs/open_source_system_strategy_v1.md`](docs/open_source_system_strategy_v1.md).

**Offline tooling:** `tools/meshlet_import` vendors **meshoptimizer v1.2** (MIT, `9d9890c7…e95448`) — build-time only; the runtime module is dependency-free. See [`docs/dependency_register.md`](docs/dependency_register.md).

### Author

**abdullahmousa8** — [github.com/abdullahmousa8](https://github.com/abdullahmousa8)

---

## نظرة عامة بالعربية

**GOTOT-NEXT** نموذج أولي لنظام **GPU-driven rendering** مبني كوحدة C++ مخصّصة (`modules/gotot_render`) داخل **Godot v4.8.dev**، يستخدم `RenderingDevice` محليًا (Vulkan) معزولًا عن renderer المحرك بنفس أسلوب `LightmapperRD`.

**الفكرة:** كاميرا واحدة → ممران فقط: culling + HZB بالحوسبة، ثم **رسم واحد غير مباشر** (indexed indirect draw) تُنتج وسائطه (arguments) على الـ GPU مباشرة. لا توجد حلقات CPU على آلاف الكائنات في المسار الحرج.

**ما تم إنجازه (كلها PASS):**

| # | المرحلة | الحالة |
|---|---------|:------:|
| 001A | جهاز GPU | ✅ |
| 001B | GPU Scene (100K) | ✅ |
| 002 | Frustum Culling | ✅ |
| 003 | Indirect Args + Compaction | ✅ |
| 004 | HZB Occlusion | ✅ |
| 005 | الرسم غير المباشر الفعلي | ✅ |
| 006 | قياسات 100K/1M/10M | ✅ |
| 007A | جسر العرض داخل نافذة Godot | ✅ |
| 008A | إثبات الهندسة الحقيقية (mesh) | ✅ |
| 008B | إثبات الرسم المتعدد (10K mesh) | ✅ |
| 009 | مخزن العمق الحقيقي (`D32_SFLOAT`) | ✅ |
| 010 | رسم الدفعات (batch instance، 3 أشكال) | ✅ |
| 011 | Multi-Draw / Multi-Batch (≤5 دفعات مجمّعة + reorder) | ✅ |
| 011 | Demo تفاعلي (main_demo + كاميرا FPS + HUD) | ✅ |
| 013 | Meshlets + LOD + Cluster Culling | ✅ |
| 014 | GPU Scene Manager (SSBO Scene DB) | ✅ |

**008A تحديدًا** يثبت أن المشروع يستطيع رسم **هندسة 3D حقيقية** (vertex buffer + index buffer + vertex format + indexed indirect draw) لمكعب 8 رؤوس/36 فهرسًا، معتمِدًا على نفس GPU Scene ونفس قائمة الـ compact ونفس نظام الوسائط غير المباشرة، وبقي مسار الـ billboard القديم كما هو دون أي استبدال.
ثم **008B** يمدّ ذلك إلى **10,000 instance** بنفس mesh، و**009** يضيف **مخزن العمق الحقيقي `D32_SFLOAT`** (عمق كتابةً واختبارًا) ويُثبت ترتيب العمق بين المكعبات (fg==green، rim ضمن الحدود، DET ثابت).
ثم **010** يوسّع المسار إلى **3 meshes مختلفة** (cube/tetra/octa) × **مثيلان لكل mesh** عبر **جدول meshes** على GPU + **batch assembly** (count pass + prefix-sum) + **multi-draw** لكل batch، مع الحفاظ على ترتيب العمق من 009 (dC < dT < dO).
ثم **011** يرتقي إلى **64 meshes / 128 instances** مع **parallel prefix-sum** على مستوى workgroup، وتجميع الدفعات إلى **≤5 draw_calls** (استراتيجيات PER_MESH/GROUPED/REORDERED بالتبديل الحي)، **draw count ديناميكي GPU-written**، و**reorder** تصاعدي حسب mesh_id — مع ثبات البكسلات حرفيًا بين الاستراتيجيات (g=178 b=102 o=282 k=11525) وDET ثابت. يليه **demo تفاعلي** (main_demo + camera_controller + hud) بتبديل الاستراتيجية مباشرة من الكيبورد.
ثم **013** يثبت مسار **meshlets + LOD + cluster culling** كاملًا: توليد بيانات `.gomlet` دون اتصال (meshoptimizer v1.2 الأدوات فقط، وقت التشغيل مستقل)، LOD تلقائي يتبدل مع المسافة `[0,1,1,2,2,0]`، culling للمجموعات (frustum + مخروط + LOD)، وراسم شاشة برمجي بثلاثة ممرات مع **فائز متماسك 64-bit** — دليل `covered == winner == 76,685` بكسلًا، وتغطية لكل LOD، وDET ثابت (`d1`). المشهد: 1M مثلث في LOD0 و18,613 meshlet.
ثم **014** يضيف **GPU Scene Manager**: قاعدة بيانات مشهد على الـ GPU (SSBO SoA، سجلات 64 بايت، مساحة IDs موحدة) تتحمل **1,048,576 instance**، مع تحديثات مدفوعة بالـ GPU عبر **ring buffer 16 MB** و compute apply (add/remove/move) دون قراءة رجوعية في المسار الحرج، وتسليم draw-records إلى مسار الـ meshlets (013) مع الحفاظ على التوقيع (fnv=3106528256)، وتوافق 011 (4096 instances عبر 8 meshes → **5 draw calls**)، وثبات DET (`sig=v14` ثابت عبر تشغيلين).

**البناء** (من مجلد مصدر Godot):

```powershell
scons platform=windows target=editor dev_build=yes `
      custom_modules="C:\path\to\gotot-next\modules" -j6
```

**التشغيل:**

```powershell
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_009.tscn -- --front-only
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_009.tscn
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_013.tscn
godot.windows.editor.dev.x86_64.console.exe --path <repo>\demo\gpu_smoke res://main_014.tscn
```

**ملاحظات مهمة:**
- الأرقام المذكورة هي **Benchmark Prototype v0.1** وليست ادّعاءً بأداء إنتاجي (60FPS على 10M).
- الـ CPU readback **جسر مؤقت** وليس مسار العرض النهائي.
- الهدف الحالي هو **الصحّة (correctness) أولًا**، والتحسين لاحقًا.

التفاصيل الكاملة في [`docs/progress_report.md`](docs/progress_report.md).
