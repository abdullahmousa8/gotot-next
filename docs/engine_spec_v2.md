# gotot-next — Engineering Design & Execution Specification

Version 2.0 — GPU-Driven Next-Generation Rendering Architecture

وثيقة هندسية متكاملة لتحويل الفكرة الأولية إلى برنامج محرك قابل للتنفيذ، مع فصل واضح بين ما هو هدف بحثي، وما هو نظام إنتاج، وما هو Benchmark قابل للقياس.

- الحالة: Architecture Baseline / Engineering Planning
- المدة المرجعية: 30 شهراً مع بوابات قرار (ليست وعداً ثابتاً بإصدار 1.0 خلال 24 شهراً)
- الترخيص المقترح: Core مفتوح المصدر، مع فصل الوحدات التجارية اختيارياً

## 1. الملخص التنفيذي

gotot-next هو مشروع محرك ألعاب جيل قادم يهدف إلى بناء طبقة Rendering وRuntime عالية الأداء، مستوحاة من مزايا Godot في سهولة الاستخدام وخفة البنية، لكن مع Render Architecture موجهة بصورة أساسية نحو GPU-Driven Rendering. لا يفترض المشروع أن الوصول إلى مستوى Nanite/Lumen/VSM يتم بمجرد إضافة Mesh Shaders أو Ray Tracing؛ بل يتطلب إعادة تصميم متكاملة لمسار البيانات، الذاكرة، الرؤية، الأصول، الـ shaders والـ frame scheduling.

القرار الهندسي الرئيسي: عدم إعادة كتابة المحرك كله في خطوة واحدة. يبدأ المشروع بنواة RenderCore مستقلة يمكن تشغيلها داخل بيئة Godot، ثم تُختبر على مشاهد معيارية. بعد إثبات الأداء والاستقرار، يتم نقل المسؤوليات تدريجياً من RenderingServer/RenderingDevice إلى طبقة جديدة.

المراجع تؤكد أن Godot يفصل Scene Layer عن Server Layer وDrivers/Platform، وأن RenderingServer يمكن استخدامه دون SceneTree في حالات الأداء الحساسة. لذلك تستفيد الخطة من هذه الحدود بدلاً من هدمها بالكامل منذ اليوم الأول.

## 2. تصحيح الفرضيات التقنية في الدراسة الأصلية

| الموضوع | القرار في v2.0 | التفسير |
|---|---|---|
| Godot ليس GPU-driven بالكامل | صحيح كاتجاه عام، لكن لا يوصف بأنه CPU-only | Godot لديه Servers وRenderingDevice، ويمكن تجاوز SceneTree؛ عنق الزجاجة يجب إثباته بالـ profiling وليس افتراضه. |
| Mesh Shaders | ميزة اختيارية وليست شرطاً لكل الأجهزة | يُبنى المسار الأساسي على compute-driven culling + indirect draws، مع Mesh Shader backend عندما تتوفر القدرات. |
| Software Rasterizer | ليس جزءاً أساسياً من النسخة الأولى | يدخل كتجربة بحثية بعد إثبات أن فائدته تتجاوز كلفته. |
| 10 مليارات مثلث @ 60 FPS | هدف بحثي غير مضمون | يُستبدل بأهداف benchmark مرتبطة بنسبة visible triangles، bandwidth، GPU time، VRAM وCPU frame time. |
| 4K/60 مع full RT GI | هدف جودة/عتاد مشروط | يجب تعريف GPU target، sample count، internal resolution وdenoiser قبل أي ادعاء. |
| VSM 70% VRAM saving | لا يعتمد كرقم ثابت | الفائدة تتغير مع عدد الصفحات، الإضاءة، الحركة، الكاش ومعدل invalidation. |
| Fully Bindless everywhere | هدف معماري مع fallback | دعم descriptor indexing/descriptor buffer حيث تتوفر، مع مسارات بديلة. |
| Dual licensing | قرار تجاري منفصل عن التصميم التقني | يحتاج مراجعة قانونية؛ لا يُخلط مع Core API. |

## 3. الأهداف والنطاق

### 3.1 أهداف أساسية
- GPU-driven visibility pipeline يقلل العمل المتكرر على CPU.
- Render Graph صريح لإدارة الموارد والمزامنة.
- GPU Scene موحد قابل للقراءة من compute/graphics/ray tracing.
- Virtualized Geometry بصيغة Meshlet/Cluster مع streaming تدريجي.
- Indirect rendering وresource indexing واسع.
- Hybrid GI: screen-space + distance/voxel/probe fallback + hardware RT.
- Virtual Shadow Maps قابلة للتدرج.
- Asset streaming asynchronous مع cache وإدارة budget.
- Shader compilation pipeline مع cache وprewarming.
- Profiler ومقاييس أداء مدمجة منذ اليوم الأول.
- Fallback renderer يحافظ على قابلية التشغيل على عتاد أقل.

### 3.2 خارج النطاق في البداية
- استبدال Physics/Audio/Animation/Editor بالكامل في أول 12 شهراً.
- دعم كل منصة console من اليوم الأول.
- ضمان 10 مليارات مثلث على بطاقة متوسطة.
- إزالة كل CPU work؛ الهدف إزالة العمل غير الضروري من critical path فقط.
- اعتماد تقنية واحدة لكل الأجهزة.

## 4. المعمارية العليا

الهيكل يفصل بين Engine Runtime وRender Runtime وGraphics Backends. Scene/ECS ينتج تغييرات state؛ GPU Scene يحولها إلى بيانات مستقرة؛ Render Graph يبني frame؛ Backend ينفذ على Vulkan/D3D12/Metal.

| الطبقة | المكونات |
|---|---|
| Application | Game, Tools, Editor, Scripting |
| Gameplay Runtime | Scene/ECS, Animation, Physics integration, Asset references |
| Render Frontend | World Extraction, GPU Scene Builder, View System, Material System |
| Render Core | Render Graph, Frame Scheduler, Visibility, Lighting, Shadows, PostFX |
| GPU Data Layer | Bindless/Descriptors, GPU Buffers, Meshlets, Texture Residency |
| RHI / Gfx Abstraction | Resource, Pipeline, Command, Synchronization, Barriers, Queries |
| Backends | Vulkan, D3D12, Metal |
| Platform | Windowing, filesystem, threads, memory, input, OS services |

## 5. RenderCore

### 5.1 Render Graph
كل frame يُبنى كـ DAG من passes وresources. كل pass يعلن reads/writes، والنظام يحلل lifetimes ويعيد استخدام الذاكرة ويولد barriers/transitions. لا يسمح للـ pass بالوصول العشوائي إلى الموارد.

- Resource lifetime analysis
- Transient resource allocator
- Automatic barriers
- Async compute scheduling
- Pass merging
- Debug graph export
- GPU timestamp instrumentation

### 5.2 Frame model
1. CPU/Game: تحديث العالم والمنطق.
2. World Extraction: استخراج renderable state إلى buffers مستقرة.
3. GPU Scene Update: رفع التغييرات فقط.
4. Visibility: frustum + backface + HZB occlusion.
5. LOD/Cluster selection: اختيار clusters حسب projected error.
6. Command generation: بناء indirect command buffers.
7. Depth/Visibility prepass.
8. Material/shading passes.
9. Lighting/GI/shadows.
10. Transparency/particles.
11. Post-processing/upscaling.
12. Present + telemetry.

## 6. GPU Scene

GPU Scene هو قلب التصميم. بدلاً من إرسال مجموعة أوامر لكل object في كل frame، يخزن المحرك transforms, bounds, mesh IDs, material IDs, flags, LOD state وvisibility metadata في بنية SoA/packed buffers.

| Buffer | بيانات رئيسية | التحديث |
|---|---|---|
| InstanceBuffer | Transform, bounds, mesh handle, material handle | Dirty ranges |
| MeshTable | Meshlet ranges, vertex/index offsets, bounds | عند import |
| MaterialTable | Parameters, texture IDs, shader permutation | عند التغيير |
| LightBuffer | Type, transform, radiance, shadow flags | Dirty ranges |
| ViewBuffer | Camera matrices, jitter, exposure | كل frame |
| VisibilityBuffer | Visible cluster IDs / flags | GPU generated |
| IndirectArgs | Draw parameters | GPU generated |

## 7. GototMesh — Virtualized Geometry

### 7.1 Import pipeline
استيراد mesh عالي الدقة ← تنظيف topology ← تقسيم geometry إلى clusters/meshlets ← حساب bounding sphere/cone وerror metric ← ضغط vertices/indices/attributes ← بناء hierarchy متعددة المستويات ← تخزين chunks قابلة للـ streaming ← توليد metadata للـ shadow/GI/RT.

الحجم المقترح الأولي للـ cluster هو 64–128 triangles، لكنه غير ثابت؛ يجب إجراء benchmark على 32/64/128/256 وتحديد أفضل configuration لكل backend/GPU.

### 7.2 Visibility
- Frustum culling
- Backface/cone culling
- Hierarchical Z occlusion
- Screen-space error LOD
- Instance culling
- Light/shadow visibility
- Temporal visibility cache

### 7.3 Mesh Shader strategy
Mesh shaders تُستخدم كمسار متقدم عند توفرها. المسار الأساسي لا يعتمد عليها كي لا يصبح دعم Vulkan/D3D12/Metal مقيداً. عند عدم توفرها: compute culling + indirect indexed draws.

### 7.4 Software rasterization
يُعامل كـ research module. تُقارن software rasterization مقابل hardware rasterization على micro-triangles، وتُقاس occupancy، LDS/shared-memory pressure، bandwidth وoverdraw. لا يدخل shipping path قبل اجتياز benchmark gate.

## 8. GototMaterials

نظام المواد يعتمد Material Graph عالي المستوى يولد shader permutations مدارة مركزياً. كل مادة ترتبط بـ Material ID ثابت، والموارد تشير إلى texture/sampler IDs بدلاً من bind calls متكررة.

- PBR metallic/roughness
- Clear Coat
- Sheen
- Transmission/SSS
- Anisotropy
- Emissive
- Decals
- Virtual textures لاحقاً
- Material LOD/feature levels

## 9. GototLumen — Hybrid GI

الاسم داخلي فقط ولا يعني مطابقة Lumen. النظام متعدد المستويات:
- Screen-space tracing للحلول الرخيصة القريبة.
- Distance-field / proxy tracing للفراغات غير المرئية مباشرة.
- Probe/irradiance cache للمناطق البعيدة أو منخفضة الأهمية.
- Hardware ray tracing عند توفره.
- Temporal/spatial reconstruction.
- Fallback raster/probe mode للأجهزة غير الداعمة.

Hardware RT يجب أن يكون feature tier وليس شرط تشغيل المحرك. Capability System يقرر المسار وقت إنشاء الجهاز.

## 10. GototVSM

التصميم يعتمد virtual pages وpage tables وcache. يبدأ التنفيذ بـ directional light واحد، ثم local lights.

- Virtual address → physical page mapping
- Page request generation
- Feedback buffer
- Page allocator
- Static/dynamic cache
- Dirty/invalidation tracking
- Per-light budgets
- Debug visualization

المرجع المعماري: VSM في Unreal يستخدم صفحات 128×128 داخل مساحة افتراضية 16K×16K ويحدث الصفحات المطلوبة فقط. هذه الفكرة مرجع تصميمي، لكن أرقام Unreal ليست مواصفات تُنسخ حرفياً.

## 11. Lighting & Visibility

### 11.1 Clustered/Forward+
النسخة الأولى تستخدم clustered light lists مع compute generation. كل tile/cluster يحتوي قائمة lights. يُفصل light classification عن shading.

### 11.2 Occlusion
يُولد HZB بعد depth prepass. visibility pass يستخدم hierarchical tests ثم temporal hysteresis لتقليل popping.

## 12. RHI

RHI يجب أن يكون صغيراً ومحدداً. لا يعكس تفاصيل Vulkan أو D3D12 واحداً لواحد.

- Device/Adapter
- Buffer
- Texture
- Sampler
- Shader
- Pipeline
- Descriptor/Bindless table
- CommandList
- Fence/Semaphore
- QueryPool
- AccelerationStructure
- Swapchain
- MemoryAllocator

كل backend يجب أن يمر عبر Capability Matrix. أي feature غير مدعوم يعلن capability بدلاً من crash أو silently degraded behavior.

## 13. Memory Architecture

| منطقة | الاستراتيجية |
|---|---|
| GPU persistent | Buddy/segregated pools حسب الحجم والعمر |
| Transient | Render Graph aliasing |
| Upload | Ring buffers + staging |
| Readback | Dedicated readback heap |
| Textures | Residency budget + streaming |
| Geometry | Chunked virtualized storage |
| Descriptors | Persistent bindless tables + transient tables |
| CPU | Frame allocators + pools |

يجب تسجيل peak/average allocation، fragmentation، residency، upload bandwidth وevictions في profiler.

## 14. Asset Streaming

الأصول تُقسم إلى chunks مستقلة: geometry clusters، textures، material metadata، animation blocks. Asset manager يستخدم IO threads وpriority queues وGPU upload queues.

- Distance-based priority
- Screen-space priority
- Prefetch
- Cancellation
- LRU/LFU hybrid cache
- VRAM budget
- Disk bandwidth telemetry
- Background decompression
- Failure recovery

DirectStorage يكون backend اختياري على Windows، وليس dependency معمارية للمحرك.

## 15. Shader System

منع shader compilation من الظهور كـ hidden runtime hitch. السلسلة: shader source → preprocessing → reflection → permutation key → compilation → pipeline cache → warmup.

- Offline compiler
- Runtime fallback compiler
- Pipeline cache
- Binary cache
- Permutation database
- Shader dependency graph
- Async compilation
- Warmup scenes
- Cache invalidation by driver/GPU/engine version

## 16. Multithreading

| Thread/Worker | المسؤولية |
|---|---|
| Main | Gameplay / orchestration |
| Render Frontend | World extraction |
| Render Graph | Scheduling/build |
| GPU submission | Command recording |
| IO workers | Asset streaming |
| Shader workers | Compilation |
| Compression workers | Decode/transcode |
| Telemetry | Profiling/logging |

لا يجوز جعل GPU submission thread ينتظر IO أو shader compilation. التواصل عبر lock-free queues أو job system مع ownership واضح.

## 17. ECS / Scene Integration

لا يُفرض ECS كاملاً على مستخدمي Godot. يقدم المحرك Render ECS داخلياً: renderer يرى arrays من renderables بينما يمكن للـ SceneTree أو ECS خارجي إنتاجها. يحافظ هذا على مرونة Godot ويمنع ربط renderer ببنية gameplay واحدة.

## 18. Editor

- Render Graph Visualizer
- GPU Scene Inspector
- Meshlet viewer
- HZB/occlusion visualization
- VSM page visualization
- GI probe/ray visualization
- Material permutation debugger
- Shader cache monitor
- VRAM/residency profiler
- Frame capture/replay

## 19. Profiling & Telemetry

كل نظام يجب أن يكون قابلاً للقياس قبل تحسينه.

| Metric | Target أولي |
|---|---|
| CPU render submission | < 4 ms في benchmark desktop مرجعي |
| Render graph build | < 1 ms بعد warm-up |
| GPU culling | < 1.5 ms في scene benchmark محدد |
| Shader hitch | 0 hitch بعد warm-up في certification scene |
| Frame pacing | P95/P99 ضمن الميزانية المحددة |
| VRAM | لا تجاوز للـ budget دون telemetry وeviction |
| Streaming | عدم حجب game thread |

## 20. Benchmark Suite

- MicroTriangle Stress: ملايين micro-triangles.
- City Dense: آلاف/مئات آلاف instances.
- Open World: streaming + foliage + terrain.
- Indoor GI: غرف متعددة وانعكاسات.
- Shadow Stress: directional + many local lights.
- Material Stress: high permutation count.
- CPU Stress: thousands of dynamic entities.
- VRAM Stress: textures/geometry فوق budget.
- Shader Cold Start: أول تشغيل بعد حذف cache.
- Frame Pacing: camera traversal مع streaming.

## 21. Performance Gates

لا ينتقل النظام إلى المرحلة التالية بمجرد اكتمال الكود.

| Gate | شرط |
|---|---|
| Correctness | Golden image / deterministic validation ضمن tolerance |
| Performance | اجتياز benchmark target |
| Stability | ساعات stress test بدون GPU/device loss غير مبرر |
| Memory | عدم وجود leaks أو unbounded growth |
| Compatibility | اختبار NVIDIA/AMD/Intel |
| Fallback | تشغيل feature tier الأدنى |
| Tooling | إمكانية تشخيص failure دون debugger خارجي فقط |

## 22. Feature Tiers

| Tier | المحتوى |
|---|---|
| Tier 0 | Raster baseline + classic LOD + conventional shadows |
| Tier 1 | GPU culling + indirect + clustered lighting + bindless/indexed resources |
| Tier 2 | Virtualized geometry + VSM + advanced GI cache |
| Tier 3 | Hardware RT GI/reflections + mesh shaders + advanced reconstruction |
| Research | Software rasterizer, experimental mega-geometry, future features |

## 23. خطة التنفيذ 30 شهراً

| الفترة | المخرجات |
|---|---|
| M0–M3 | Repository, CI, coding standards, RHI prototype, capability system, benchmarks |
| M4–M6 | Render Graph, GPU Scene v0, indirect rendering, profiler |
| M7–M9 | GPU culling, HZB, clustered lighting, bindless resource model |
| M10–M12 | Meshlet importer, hierarchy, streaming prototype |
| M13–M15 | Virtualized geometry production path + editor visualization |
| M16–M18 | VSM prototype + cache + shadow integration |
| M19–M21 | Hybrid GI + RT backend + denoiser/reconstruction |
| M22–M24 | Asset streaming production, shader cache, stability/compatibility |
| M25–M27 | Editor tools, migration layer, sample games, certification |
| M28–M30 | Release candidate, documentation, packaging, 1.0 decision gate |

## 24. الفريق المقترح

| الدور | العدد المبدئي |
|---|---|
| Rendering Architect | 1 |
| Graphics Engineers | 4–6 |
| RHI/Backend Engineers | 2–3 |
| GPU/Compute Engineer | 2 |
| Asset/Streaming Engineer | 2 |
| Shader/Material Engineer | 1–2 |
| Engine/Core Engineer | 2 |
| Tools/Editor Engineer | 2 |
| Performance/QA Engineer | 2 |
| Build/CI/Release | 1 |
| Technical Artist / Samples | 1–2 |

الفريق الأساسي المقترح: ~20–25 شخصاً. يمكن بدء Prototype بفريق أصغر، لكن production-grade multi-backend engine يتطلب تخصصات متوازية.

## 25. استراتيجية Git وRepository

```
engine/core
engine/render
engine/rhi
engine/backends/vulkan
engine/backends/d3d12
engine/backends/metal
engine/gpu_scene
engine/geometry
engine/lighting
engine/shadows
engine/streaming
engine/shaders
tools/rendergraph
tools/profiler
tests
benchmarks
samples
docs
```

## 26. خطة Prototype الأولى

1. إنشاء RHI Vulkan/D3D12 minimal.
2. إنشاء window + swapchain + command submission.
3. إنشاء GPU Scene لـ 100k instances.
4. Compute frustum culling.
5. HZB occlusion.
6. Indirect draw generation.
7. Material/texture indexing.
8. Profiler + GPU timestamps.
9. Benchmark city بسيط.
10. مقارنة CPU draw submission مقابل GPU-driven path.

إذا لم يظهر prototype مكسباً قابلاً للقياس، لا يتم توسيع النظام إلى Meshlets/GI/VSM؛ تتم مراجعة الفرضية أولاً.

## 27. Migration من Godot

Compatibility Layer:
- Godot project importer
- Scene/resource compatibility
- Material translation
- GDExtension ABI strategy
- Rendering fallback
- Migration warnings
- Automated project audit
- Compatibility report

التوافق مع Godot 4 هدف تدريجي، وليس وعداً بأن كل plugin أو renderer extension سيعمل دون تعديل.

## 28. الجودة والاستقرار

- Unit tests
- Render tests
- Golden images
- GPU validation layers in CI
- Device lost recovery tests
- Out-of-memory tests
- Shader compiler fuzzing
- Asset corruption tests
- Long-run soak tests
- Driver matrix

## 29. مخاطر المشروع

| الخطر | الأثر | المعالجة |
|---|---|---|
| اتساع النطاق | عالٍ | Freeze architecture قبل إضافة features |
| Driver variance | عالٍ | Capability matrix + backend tests |
| Shader permutations | عالٍ | Material graph + permutation budgeting |
| VRAM pressure | عالٍ | Residency budgets + eviction |
| GPU synchronization | عالٍ | Render Graph ownership/barrier validation |
| Meshlet overhead | متوسط/عالٍ | Benchmark cluster sizes |
| RT cost | عالٍ | Tiered quality + temporal reconstruction |
| Streaming stalls | عالٍ | Async queues + prefetch |
| Migration complexity | عالٍ | Compatibility layer + staged adoption |
| Team knowledge | عالٍ | Architecture docs + code ownership |

## 30. قرارات يجب تثبيتها قبل كتابة Production Code

1. الـ coordinate conventions وprecision policy.
2. GPU Scene data layout.
3. Resource handle ABI.
4. RHI ownership/lifetime model.
5. Render Graph API.
6. Capability system.
7. Frame synchronization model.
8. Asset package format.
9. Shader intermediate representation.
10. Error/reporting policy.
11. Benchmark hardware matrix.
12. License/governance policy.

## 31. المقارنة المرجعية

- **Godot الحالي**: ثلاث طرق rendering، Forward+ وMobile فوق RenderingDevice، architecture مقسمة إلى Scene/Server/Driver layers → قاعدة مناسبة للـ prototype والـ migration.
- **Unreal**: مرجع عملي للـ virtualized geometry وVSM وLumen (Nanite: geometry افتراضية مع streaming وautomatic LOD؛ VSM: virtual pages/cache؛ Lumen: عدة طرق ray tracing) → references معمارية لا أهداف نسخ حرفي.
- **Vulkan**: extensions حديثة مثل descriptor buffer وray tracing pipeline، لكن دعمها capability-dependent → abstraction مبنية على capabilities وليس على افتراض أن كل GPU يدعم كل feature.

## 32. المراجع التقنية الأساسية

- Godot Engine — Architecture Overview
- Godot Engine — RenderingServer
- Godot Engine — Rendering methods / RenderingDevice
- Vulkan Documentation — Extensions
- Vulkan Documentation — VK_EXT_descriptor_buffer
- Vulkan Documentation — Ray Tracing
- Epic Games — Nanite Virtualized Geometry
- Epic Games — Virtual Shadow Maps
- Epic Games — Lumen Global Illumination and Reflections
- Unity Documentation — Adaptive Probe Volumes

## 33. الخلاصة الهندسية

gotot-next ليس "نسخة Godot تحتوي Nanite/Lumen/VSM". التعريف الأدق: **Render-centric engine architecture ذات GPU Scene وRender Graph وvirtualized resources**، تسمح بإضافة هذه التقنيات كأنظمة متكاملة.

أكبر نجاح في أول سنة ليس جودة الصورة، بل إثبات أن البيانات تنتقل من العالم إلى GPU بكفاءة، وأن visibility والـ indirect commands والموارد يمكن إدارتها دون CPU overhead غير ضروري، مع profiler يكشف أين تذهب كل ميلي ثانية.

بعد إثبات ذلك، تصبح Meshlets وVSM وHybrid GI مراحل قابلة للتطوير. أما محاولة بناء كل هذه الأنظمة دفعة واحدة فترفع المخاطر وتمنع معرفة أي جزء هو سبب الأداء أو عدم الاستقرار.

## 34. Definition of Done لإصدار 1.0

- Render Graph production-ready.
- GPU Scene production-ready.
- GPU culling + indirect rendering production-ready.
- Virtualized geometry production-ready على Tier 2/3 hardware.
- VSM production-ready مع caching.
- Hybrid GI production-ready مع fallback.
- Shader/pipeline cache يمنع runtime compilation stalls بعد warmup.
- Asset streaming production-ready.
- Vulkan + D3D12 production backends مستقرة؛ Metal حسب الموارد والاختبارات.
- Profiler وRenderDoc-compatible capture workflow.
- نماذج ألعاب كاملة قابلة للبناء والتصدير.
- وثائق API وArchitecture وMigration.
- CI على NVIDIA/AMD/Intel.
- لا توجد known critical correctness/security blockers.
- Performance targets موثقة بالعتاد والسيناريو، وليست أرقاماً عامة.

## 35. أول 12 مهمة يجب تنفيذها الآن

1. تثبيت Git repository وbranch policy.
2. إنشاء Architecture Decision Records (ADR).
3. تعريف RHI interface.
4. تعريف Capability Matrix.
5. بناء Vulkan minimal backend.
6. بناء D3D12 minimal backend.
7. إنشاء Render Graph prototype.
8. إنشاء GPU Scene prototype.
9. إنشاء 100k-instance benchmark.
10. إضافة GPU frustum culling.
11. إضافة HZB occlusion.
12. إضافة indirect draw generation.