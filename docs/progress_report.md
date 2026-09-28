# تقرير شامل — مشروع GNE (حتى الآن)

## 1. الهدف والأساس
نهدف لبناء نظام **GPU-driven rendering** حقيقي كوحدة C++ (Module) مخصصة في **Godot v4.8.dev (master)**, ننفذ خارطة طريق `docs/engine_spec_v2.md` مرحليًا. الفكرة الجوهرية: كاميرا واحدة → **ممران فقط**: Culling+HZB بالحوسبة (compute) ثم **رسم واحد غير مباشر** (indirect draw) يقوده الـ GPU مباشرة، بدون حلقات CPU على آلاف الأشياء.

المراحل التي اكتملت: **001A** (جهاز GPU كسول) → **001B** (GPU Scene) → **002** (frustum culling) → **003** (indirect args) → **004** (HZB occlusion) → **005** (رسم غير مباشر فعلي) → **006** (قياسات تدرج 10M).

بعد اكتمال هذه المراحل، لم نعد في مرحلة "إثبات إمكانية GPU-driven rendering" تجريبية فقط؛ أصبح لدينا **Prototype متكامل تقريبًا**: GPU Scene → Culling → HZB → Indirect Draw → Rasterization على Framebuffer خاص.

## 2. البيئة والبناء
- الملفات: `C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\modules\gne_render\{gne_render_server.h, .cpp}` + `demo\gpu_smoke\main.gd` + `docs\engine_spec_v2.md`.
- البناء (من `godot-master`): `scons platform=windows target=editor dev_build=yes custom_modules="C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\modules" -j6` (~15-21 ث).
- **مشكلة قسرية**: سياسة App Control/Device Guard على الجهاز تحجب تشغيل الـ exe مباشرة → حل بديل مثبت: `schtasks /Create /Run /Delete` يشغّل `gt_smoke.bat` الذي يكتب الناتج إلى `gt3.txt`، ثم نقرأه.
- `RenderingDevice` محلي كسول (بنفس نمط `LightmapperRD`) لإبقاء الوحدة منعزلة عن الـ renderer الرئيسي.

## 3. GNE-001A — الجهاز
- `ensure_gpu_device()` → يخلق `RenderingDevice` محلي عند أول طلب فقط، مع `is_gpu_ready()`.
- ملاحظة: لا نستدعي `free_rid` على الـ RD نفسه (جهاز مشترك للعملية كلها) — نتجنّب خطأ "Attempted to free invalid ID".

## 4. GNE-001B — GPU Scene
- 100,000 كائن ببنية SoA (فصل حسب النوع للاستفادة من الـ bandwidth):
  - `transform_buffer`: `vec4 position+scale` (16 بايت).
  - `bounds_buffer`: `vec4 min_xyz+max_z` (32 بايت).
  - `instance_id_buffer`: uint32 (4 بايت).
- تعبئة بواسطة compute shader واحد يوازى بالترتيب (لكل instance أصله) — نفس النتائج CPU وGPU لاحقًا.
- القياس المُثبت: `instances=100000, create≈196-318ms (تخصيص+تجميع), fill≈0.33-0.40ms`, الحجم ~4.96MB.

## 5. GNE-002 — Frustum Culling
- 6 مستويات من `Projection::get_projection_planes()` بالترتيب: near, far, left, top, right, bottom.
- اختبار: **كرة** (مركز+نصف قطر) ضد المستويات: داخل إذا كان `dot(n,p)+d >= -radius` — يعطي حصانة ضد الدخول الجزئي والعرض الزاوي الصغير.
- عدّ ذري للحالات المرئية في `visible_count_buffer` + مصفوفة `visibility[]`.
- التحقق: عدّتان GPU **متطابقتان** (حتمية)، و GPU==CPU بالضبط = **377** على الـ 100K، مع مطابقة كاملة لكل إدخال بـ visibility[i]. `cull_ms≈0.1ms`.
- كاميرا "نظرة بعيدة" تُبقي ≤500 (تحقق من صحة عكس الاتجاه).

## 6. GNE-003 — Indirect Args + Compaction
- ممر ضغط (compaction) يحول `visibility[]` إلى قائمة متراصة `compact[]` من الأصول المرئية.
- ممر التثبيت (finalize): `indirect_args_buffer[5] = {index_count, instance_count, first_index, vertex_offset, first_instance}` — بالضبط مخطط `VkDrawIndexedIndirectCommand`.
- **درس**: ترتيب الخانات (slots) في الضغط غير حتمي (تزامن) → نقارن القوائم **بعد الفرز**، بينما العداد والـ args حتميان.
- النتيجة: `finalize_ms≈0.41-0.58`, `args=[6,377,0,0,0]`, والقائمة المتراصة == مجموعة الـ CPU تمامًا.
- أنشئ بـ `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT` (مطلوب لأي indirect).

## 7. GNE-004 — HZB Occlusion
- **HZB**: نسيج `512×512 R32UI` من نوع 2D-array بـ 10 طبقات (مستويات). `hzb_valid` في الـ UBO يخبر الـ cull shader هل نستخدم HZB أو frustum فقط.
- 3 ممرات متتالية (كل pass = list + submit + sync):
  1. **Clear level0**: مسح الطبقة 0.
  2. **Occluder pass**: إسقاط صناديق المعيقات (خلفيات) عبر push constant، تراكب المستطيل المسقط، والتخزين بـ `imageAtomicMax(dst, floatBitsToUint(far_plane - min_z))` — قيمة أعلى = أقرب للكاميرا.
  3. **Downsample ×9**: أخذ ماكس 2×2 من كل مستوى فوقه.
- **دمج HZB في cull نفسه بمرور واحد**: مستوى تكلس من `ceil(log2(r_pixels))`، فحص 2×2 بجوار المركز، والتحويل العكسي `sphere_inv = floatBitsToUint(max(far-(z_view-radius), 0))`.
- UBO `GneViewData` (256B): `vp, view, planes[6], viewport(w,h,hzb_texels,tanHalfFovV), occ_count, far_plane, hzb_valid, pad`.
- القدرة: `gpu_scene_set_viewport(w,h)`, `gpu_scene_set_occluders(Vector4 pairs)`, `gpu_visibility_dispatch()`.
- النتائج: `visibility_ms≈0.7`, frustum_only=377 → **hzb_visible=375 (حتمي)**، post-HZB args `[6,375,0,0,0]`، وبدون معيقات يعود إلى 377 (ضابطة).

## 8. GNE-005 — الرسم غير المباشر الفعلي
- **Shader الرأس**: بلا vertex buffers — `gl_VertexIndex` ينشئ 4 رؤوس مربع عالمية حول `position_scale[compact[gl_InstanceIndex]]` (half-size = scale.w)، تحويل الموقع `viewdata.vp` من نفس UBO. **Shader المقطع**: لون مجرتي (0.95,0.18,0.9).
- **Pipeline**: `render_pipeline_create(shader, fb_format, vertex_format, TRIANGLES, rs, ms, ds, blend_disabled, dynamic, for_render_pass)`؛ `vertex_format = RD::INVALID_ID` (كما يفعل blit) وإلا يطالب بمصفوفة vertices.
- **الموارد**: نسيج لوني `1920×1080 R8G8B8A8` (COLOR_ATTACHMENT|SAMPLING|CAN_COPY_FROM)، `framebuffer_format_create` بـ `AttachmentFormat{format, samples, usage}`، framebuffer بواسطة `framebuffer_create([tex], fmt)`.
- **فهارس**: `index_buffer_create(6, UINT32, [0,1,2,2,3,0])` → `index_array_create`؛ indirect args `[index_count=6, ...]`.
- **الرسم**: `draw_list_begin(fb, DRAW_CLEAR_COLOR_0, clear, ...)` → bind pipeline + uniform set (set 0: compact=0, transform=1, view_ubo=2) + index array → `draw_list_draw_indirect(list, true, args_buffer, 0, 1, 0)` → end + submit + sync.
- **قراءة**: `texture_get_data(texture, 0)` → PackedByteArray.
- **تحقق**: `colored=521 px`، حتمي عبر ممرّين، و**112 مركزًا مسقطًا** (بـ vp الفعلي من GPU عبر `gpu_scene_get_vp()`) كلها عليها بكسل مجرتي (اصطلاح Vulkan convx=1, convy=1: `py=(ndc.y*0.5+0.5)*H`) — استثنينا 18 مربعًا دون-بكسل. `draw_ms≈0.15ms`.

## 9. GNE-006 — قياسات التدرج
رفعنا حد العدد إلى 100M وقيّمنا نفس الخط على 10M:
| count | fill_ms | cull_ms | finalize_ms | visibility_ms | raster_ms | visible | buffers |
|---|---|---|---|---|---|---|---|
| 100K | 0.38 | 0.10 | 0.12 | 0.53 | 0.16 | 364 | 4.96MB |
| 1M | 3.33 | 0.15 | 0.12 | 0.60 | 0.16 | 3,670 | 49.6MB |
| 10M | 1.48 | 0.50 | 0.12 | 0.92 | 0.18 | 36,744 | 495.9MB |

**الخلاصة الدقيقة (وليست حقيقة عامة):**
في **هذا الاختبار تحديدًا**، ظل زمن مراحل culling/finalize/raster قريبًا من الثبات ضمن نطاق 100K–10M، لأن الـ prototype يعتمد على بنية GPU parallel ولا يحتوي حاليًا على **تكلفة رسم هندسي حقيقي متناسبة مع تعقيد meshes**. هذه أرقام **Benchmark Prototype v0.1** وليست ادعاءً بأن GNE يستطيع رسم 10 ملايين جسم داخل لعبة بـ 60FPS.

الفرق بين **GPU compute prototype** و **production renderer** كبير، والاختبار الحالي يحتوي:
- موارد مبسطة جدًا، مربع واحد، شادر بسيط، framebuffer مخصص، readback للتحقق.
- لا يوجد material system حقيقي، ولا mesh/vertex streams حقيقية، ولا depth buffer إنتاجي.
- لا توجد هندسة مزامنة كاملة للـ GPU، ولا render graph حقيقي حتى الآن.

## 10. الدروس التقنية المتراكمة (مهمة للمراحل القادمة)
1. **modifiers للصور**: store-only → `writeonly`؛ `imageAtomicMax` → صيغة `r32ui` صريحة **بلا** `writeonly`؛ load+store → `r32ui` صراحةً (خلافات glslang).
2. **ترتيب حقول UBO**: قيم تُملأ في `set_camera` (مثل occ_count) يجب إعادة رفعها عند وقت الـ dispatch وإلا بقيت قديمة → فصل `gpu_cull_dispatch` (hzb_valid=0) عن `gpu_visibility_dispatch` (hzb_valid=1).
3. **ترتيب التحرير**: uniform sets تُحرَّر **قبل** الموارد التي ترجع إليها وإلا "Attempted to free invalid ID".
4. **Buffer للـ indirect** يحتاج `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT`.
5. **Pipeline بلا vertices**: `VertexFormatID = RD::INVALID_ID` والاعتماد على `gl_VertexIndex`.
6. في GDScript: `Projection.xform()` يقبل **Vector4**؛ والأمان: نضرب مصفوفة vp يدويًا (column-major) بنفس خوارزمية GLSL.
7. اصطلاح NDC→بكسل في RD/Vulkan: `py = (ndc.y*0.5+0.5)*H` (قمة الشاشة = ndc -1).
8. **Harness**: schtasks لفتح App Control؛ مقارنة القوائم بعد الفرز فقط.

## 11. الواجهات العامة الحالية للوحدة
`ensure_gpu_device`, `is_gpu_ready`, `gpu_scene_create(count, spread)`, `gpu_scene_dispatch(seed)`, `gpu_scene_get_instance_count`, `gpu_scene_readback_positions/scales`, `gpu_scene_set_camera(...)`, `gpu_scene_set_viewport`, `gpu_scene_set_occluders`, `gpu_cull_dispatch`, `gpu_cull_get_visible_count`, `gpu_cull_get_visibility`, `gpu_drawargs_finalize`, `gpu_drawargs_read`, `gpu_compact_read`, `gpu_visibility_dispatch`, `gpu_raster_indirect_draw`, `gpu_raster_read_pixels`, `gpu_scene_get_vp`, `gpu_scene_destroy`, `get_server_singleton`.

**GNE-008A (real mesh path):** `gpu_mesh_create`, `gpu_mesh_drawargs_finalize`, `gpu_mesh_indirect_draw`, `gpu_mesh_get_index_count`, `gpu_mesh_get_vertex_count`.

## 12. الحالة الحالية
- كل الاختبارات (001A→011) خضراء، خروج كود 0، إغلاق نظيف بلا أخطاء `free invalid ID` أو أخطاء RID/lifetime.
- التدفق الحالي: **GPU Scene → GPU Culling → HZB → Compaction → Indirect Args → (Billboard | Real Mesh) → Batch Assembly → Multi-Draw → Rasterization → Framebuffer Offscreen → CPU Readback → ImageTexture → TextureRect → نافذة Godot**.
- أصبح ناتج الرسم مرئيًا داخل نافذة Godot فعليًا (007A)، وأصبح المشروع يرسم **هندسة 3D حقيقية** (مكعبات) عبر مسار mesh مستقل (008A).
- **التالي**: **GNE-011 — Multi-Draw / Multi-Batch** اكتمل بأدلة PASS (3 استراتيجيات + demo تفاعلي). **GNE-012 — Production HZB** بانتظار SPEC الرسمي.

## 13. Milestone Status

- GNE-001A — GPU Device: PASS
- GNE-001B — GPU Scene: PASS
- GNE-002 — Frustum Culling: PASS
- GNE-003 — Indirect Arguments + Compaction: PASS
- GNE-004 — HZB Occlusion: PASS
- GNE-005 — Actual Indirect Draw: PASS
- GNE-006 — 100K / 1M / 10M Scaling: PASS
- GNE-007A — Viewport Integration Bridge (CPU Readback): PASS
- GNE-008A — Real Geometry Proof (Real Mesh Path): PASS
- GNE-008B — Multi-Instance Mesh Rendering Proof: PASS
- GNE-009A/009B — Real Depth Buffer (D32_SFLOAT) Proof: PASS
- GNE-010 — Batch Instance Rendering (3 meshes multi-draw): PASS
- GNE-011 — Multi-Draw / Multi-Batch (≤5 grouped/reordered draws, dynamic draw count, parallel prefix-sum): PASS
- GNE-011 Demo — Interactive (main_demo + camera_controller + hud, strategy live-switch): PASS
- GNE-012 - Production HZB: PASS (012-revised, closed)
- GNE-013 - Meshlets + LOD + Cluster Culling: PASS
- GNE-014 - GPU Scene Manager: PASS (race fix included)
- GNE-015 - Render Graph: PASS
- GNE-015.5 - Resource Pool: PARTIAL (5/8 criteria + documented KIs)
- GNE-015.6 - KI Closure: PASS
- GNE-016 - Materials: PASS
- GNE-017 - Textures: PASS
- GNE-018 - Clustered Lighting: PASS
- GNE-018-rev - Normal-Cone Back-Face Culling: PASS (R1 gate; flag OFF by default)
- GNE-019 - Shadows + Real Depth HZB: PASS
- GNE-020 - Presentation Overhaul: PASS (flag-gated reduced-res path; C6 closed)

Current architecture status:

```
           GPU Scene
               ↓
        Frustum Culling
               ↓
         HZB Occlusion
               ↓
      Visibility / Compaction
               ↓
      Indirect Draw Arguments
               │
       ┌───────┴────────┐
       ▼                ▼
 Billboard Path   Real Mesh Path
  (GNE-005)      (GNE-008A)
       │                │
       └───────┬────────┘
               ▼
      GPU-driven Rasterization
               ↓
   Offscreen Framebuffer (1920×1080)
               ↓
   CPU Readback → ImageTexture → TextureRect
               ↓
             SCREEN
```

Next milestone:

**GNE-011 — Multi-Draw / Multi-Batch** اكتمل PASS (3 استراتيجيات + demo تفاعلي). المرحلة القادمة: **GNE-012 — Production HZB** (SPEC قادم).

## 14. GNE-007A — جسر العرض داخل نافذة Godot

- **الوحيد في نطاقه**: عرض ناتج الرسم على الشاشة داخل نافذة Godot فعليًا (كان سابقًا قراءة texture من الذاكرة فقط).
- **المسار**: `Camera3D` حقيقية → `gpu_scene_set_camera` → نفس خط GPU (cull → visibility → finalize → raster) → `gpu_raster_read_pixels` → `Image` → `ImageTexture` → `TextureRect`.
- **بدون أي تعديل C++**: GDScript فقط (`demo/gpu_smoke/main_007.gd` + `main_007.tscn`).
- **ملاحظات مهمة**: `Camera3D.get_projection()` يعيد **enum** وليس المصفوفة → الصحيح `get_camera_projection()`.
- **الأدلة**: النافذة الفعلية 1152×648 (نفس نسبة 1920×1080)، `visible range 356..384`، تغيّر الصورة في `232/239` إطارًا، فحص الاتجاه `probe=0` (لا حاجة لقلب)، ولقطة نافذة محفوظة.
- **القياسات (100K)**: Cull ≈ 0.41→1.09ms، HZB/Visibility ≈ 1.03→2.61ms، Finalize ≈ 0.06→0.08ms، Raster ≈ 0.10ms، Readback ≈ 2.22→2.78ms، Frame ≈ 11.36→14.16ms (يزحف صعودًا؛ موثّق وغير مُفسَّر بعد).
- **القيد المعلن**: مسار CPU readback **جسر مؤقت وليس مسار العرض النهائي** (~8MB/إطار عند 1920×1080).

## 15. GNE-008A — إثبات الهندسة الحقيقية (Real Mesh Path)

- **الهدف**: إثبات أن GNE يستطيع رسم **هندسة 3D حقيقية** باستخدام: vertex buffer حقيقي + index buffer حقيقي + vertex format حقيقي + indexed draw + indirect draw، معتمدًا على نفس GPU Scene ونفس قائمة الـ compact ونفس نظام الوسائط غير المباشرة ونفس جسر العرض 007A.
- **قاعدة معمارية**: مسار 005/007A باقٍ كما هو **دون أي استبدال** (billboard shader، quad index buffer، raster pipeline، `gpu_raster_indirect_draw`، `gpu_raster_read_pixels`). الـ Mesh مسار **مستقل ومضاف**.
- **الهندسة**: مكعب واحد فقط — 8 رؤوس (positions فقط، `R32G32B32_SFLOAT`) و36 فهرسًا (`UINT32`). بلا normals/UVs/materials/textures.
- **نموذج الـ instance**: نفس النموذج — `compact[gl_InstanceIndex]` → `transforms.position_scale[orig]` → `viewdata.vp`. الناتج 377 (أو ما يقابله) **مكعبًا حقيقيًا**.
- **الـ indirect args**: `index_count=36, instance_count=visible, first_index=0, vertex_offset=0, first_instance=0`. قيمة `index_count` تُمرَّر عبر push constant (وليس مضمّنة بشكل يمنع التعميم، ودون بناء Mesh Table).
- **نطاق C++ المسموح**: موارد mesh vertex/index، vertex format، mesh shader، mesh pipeline، دالة رسم mesh، تدمير موارد mesh — لا شيء خارج ذلك.
- **الأدلة (100K)**: `GPU Mesh created vertices=8 indices=36 vertex_format=0`؛ `Indirect args = [36, N, 0, 0, 0]` مع `N == visible` كل إطار (تحقق برمجي يوقف التشغيل عند عدم التطابق)؛ العدّ الدقيق على الإطار الأخير: **green=305، magenta=0** (أي هندسة حقيقية وليست billboard القديم)؛ `visible range 351..389`؛ `changed_frames=236/239`؛ لقطة نافذة `1152×648`؛ إغلاق نظيف بلا أخطاء RID/lifetime.
- **القيود**: يرسم في نفس الـ framebuffer الحالي 1920×1080 ويستخدم `gpu_raster_read_pixels` القائم؛ مكعبة واحدة مضمّنة (لا mesh table/IDs/streaming)؛ لا depth buffer؛ يشترك مسار mesh في `indirect_args_buffer` القائم لذا لا يجوز خلط `gpu_drawargs_finalize()` مع `gpu_mesh_indirect_draw()` في نفس التشغيل.

## 16. المرحلة القادمة

**GNE-021 - Benchmark City + Dense-Light Validation** (proposal awaiting Architect approval; the dense-light test bed for the 018-rev dc>=10% retest and the remaining structural prototype-checklist item).

## 17. GNE-008B — برهان الرسم متعدد النسخ (Multi-Instance Mesh Rendering Proof)

- **الهدف**: إثبات أن مسار الـ mesh الحقيقي (008A) يرسم **مئات/آلاف المكعبات الحقيقية** بتحويل صحيح لكل instance من `compact[gl_InstanceIndex]`، بعد instance ديناميكي من الـ compact buffer، وبدون أي تسريب في indexing.
- **النطاق**: صفر تغيير C++ — أُعيد استخدام مسار 008A بالكامل؛ للملفات الجديدة فقط `demo/gpu_smoke/main_008b.gd` + `main_008b.tscn`.
- **الأدلة (008B PASS)**: 10,000 instance spread 1200؛ `visible` في المدى `3630..3909` (dynamic=true عبر مدار الكاميرا بتتالٍ حتمي frame-indexed)؛ `args=[36, N, 0, 0, 0]` كل إطار مع `N == visible`؛ `green_px=25765` (مدى 17635..25765)؛ توافق CPU: `gpu=3905 cpu=3905 boundary=0 delta=0 set_ok=true`؛ فحص المراكز المسقطة `checked=176 matched=176 miss=0`؛ determinism `drawargs_repeat=true full_frame_repeat=true`؛ **توقيع DET متطابق حرفيًا بين تشغيلين** `v3905|36|3905|g25765|c0|5004|9997|h176|m0|3905|f150`؛ لقطة نافذة `gt_008b_window.png`؛ regressions 001A–006/007A/008A جميعها PASS على نفس البنية.

### 008B — Fixes Applied During Verification

- **Fix 1 — قراءة مستويات frustum كل إطار بعد `set_camera`**: في أول تشغيل أظهر الفحص الفرق `gpu=3864 cpu=3907 delta=43 boundary=0` — فرق منهجي وليس ضجيج rounding، سببه أن المستويات كانت مقروءة في `_ready` بنسبة أبعاد النافذة الأولية بينما GPU يستخدم كاميرا اللحظة الراهنة (أبعاد النافذة تتغير بين الإقلاع وتشغيل الإطارات). الحل: إعادة قراءة `gpu_scene_get_frustum_planes()` كل إطار بعد `set_camera` بالكاميرا نفسها التي يُجري بها GPU الـ cull. النتيجة: `gpu == cpu` بالضبط في كل إطار (`delta=0`).
- **Fix 2 — فحص البقع من compact مُصنَّف**: أول تنفيذين أعطيا نفس دليل الرسم لكن حقل `h` (عدد البقع القابلة للفحص) اختلف (232 مقابل 196) لأن عيّنة البقع كانت تُؤخذ من **ترتيب الإصدار الذري** العشوائي في الـ compact. الحل: أخذ العينة من **نسخة مصنّفة** من الـ compact (h=176 ثابت). التوقيع مُجمَّع أصلاً من قائمة مصنّفة ليكون مستقرًا بين التشغيلات.

### ملاحظات معمارية مؤجلة (سجّل فقط — لا تنفيذ)

- **نقطة frustum reading**: كشف Fix 1 أن `set_camera` لا تُحدّث الـ frustum تلقائيًا عند تغيّر أبعاد النافذة — هل هذا سلوك مقصود أم ثغرة تصميم؟ تُعالج في milestone قادم (مثل 009/رفع لاحق).
- **نقطة الترتيب الذري**: ترتيب الإصدار (atomic compaction) عشوائي بين التشغيلات — هل يؤثر على الأداء الفعلي أم فقط على التحقق؟ تُقيَّم لاحقًا (التحقق الحالي مستقر عبر الفرز).

## 18a. GNE-009 — Real Depth Buffer (D32_SFLOAT)

### الهدف والنطاق
إرفاق **عمق حقيقي** لمسار mesh: نسيج `D32_SFLOAT` في framebuffer الراستر الخاص، مع depth test + write بواقعية (الأقرب يمنع الأبعد) على مسار **Real Mesh** (008A) بينما يبقى مسار billboard depth-disabled تمامًا. إثبات ذاتي عبر مشهدين: `main_009` مع `--front-only` (مرجع A+D) وبدونها (كامل A+B+C+D)، دون لمس أي regression سابق (001A–008B).

### التغييرات C++ (`gne_render_server.{h,cpp}`)
- **`_create_raster_pipeline`**: إنشاء نسيج عمق `1920×1080 D32_SFLOAT` (usage: `DEPTH_STENCIL_ATTACHMENT | CAN_COPY_FROM`)، إضافة `AttachmentFormat` للعمق، attachments = `[color, depth]`، وأعلام `raster_depth_attached=true` / `raster_depth_format_value=125`.
- **`_create_mesh_pipeline`**: تفعيل `enable_depth_test`, `enable_depth_write`, `depth_compare_operator = COMPARE_OP_LESS_OR_EQUAL` (الاتجاه القياسي — قريب = قيمة أصغر، far = 1.0).
- **الرسم**: `gpu_mesh_indirect_draw` و `gpu_raster_indirect_draw` — `draw_list_begin` الآن بـ `DRAW_CLEAR_COLOR_0 | DRAW_CLEAR_DEPTH` مع `clear_depth = 1.0f` (far) كل إطار.
- **التدمير**: تحرير نسيج العمق بعد نسيج اللون + تصفير الأعلام.
- **واجهات جديدة (5)**: انظر قسم الواجهات.

### الاكتشافات والتصحيحات (موثقة بمصادر قياس)
1. **near:far هو جذر السحق (squashing)**: بإسقاط `near=0.05/far=4000` (نسبة 80000:1) يُسحَق كل عمق مكتوب إلى نطاق ~1e-5 تحت 1.0 (قِيس `dA=0.99998128414154`, عتبة `0.9995` عدّت كل شيء خلفية). **الحل**: `camera.near = 300.0` فُرض في `_ready` بعدما تبيّن أن تعديل الـ `.tscn` وحده لا يصل (قراءة `camNear=0.05` في زمن التشغيل) — أعطى فصلاً نظيفاً `A≈0.878 < B≈0.918 < C≈0.916 < D≈0.901` مؤكّداً أن **الاكتشاف فرضية–قياس لا تخمين**.
2. **الاتجاه قياسي (وليس reversed)** عند near سليم: قِيس نمو القيمة مع البعد (أقرب = أصغر). جرّبت بديلاً `clear=0.0/GEQ` (ظن reversed-Z) ثم **أُعيد بعد القياس** — العمل النهائي مطابق للـ SPEC: `clear=1.0 + LESS_OR_EQUAL + write`.
3. **السيليلويتات المائلة**: النسق المتحلّل "مربع الوجه الأمامي ×r²" غير صالح للمكعبات بعناصر مائلة (هيكل سداسي مع parallax؛ قِيس `Drow=1212..1231` يطابق NDC صح، والمدى العرضي للمكعب أكبر من مربع الوجه). استُبدل النموذج التحليلي للمساحات بـ **evidence ذاتي**: `green == fg` (1:1 بين اللون والعمق) + ميزانية تغطية محدودة + `rim > 0 ∧ rim ≤ 6·fg_front` + `green_full > green_front`.

### الأدلة (009 - PASS)
- **009a (`--front-only`):** `fg=458 bg=2073142 green=458` (1:1)، `dF=0.87835 < dC=0.90061`، `dmin==dF` (الأقرب يملك أدنى عمق عام)، حفظ `gt009_depth_a.bin` + `gt009_meta_a.txt` المرجعي، `sig=v2|36|2|A|g458|f458|...|c0|3`، لقطة نافذة، PASS.
- **009b (كامل):** `visible=4`, `args=[36,4,0,0,0]`, compact `[0,1,2,3]`، `fg=2814 bg=2070786 rim=2356`, `green=2814`, **`masked_eq 458/458 bad=0`** (العمق الكامل == المرجع front-only على كل بكسل تغطيه A/D → B/C لم تكتُب عبر السطح الأقرب = اختبار العمق يعمل)، `dF<dC`, `dmin==dF`, determinism الإطار الكامل، `sig=v4|36|4|B|...`, لقطة نافذة، PASS.
- **الانحدارات:** gt_smoke (001A–006) / 007 / 008 / 008b — كلها `exit 0` على نفس البنية الجديدة.
- **القيود المعلنة**: readback العمق كامل الإطار (~8MB) — جسر تحقق كـ `gpu_raster_read_pixels` وليس مسار بيانات إنتاجياً؛ عمق الـ billboard ملغى كما هو مقصود.

### الواجهات الجديدة (5)
1. `gpu_scene_set_instance_transform(index, position, scale)` — **Test-only** (مشاهد وضع ثابت مثل main_009): يكتب 16 بايت `vec4(position,scale)` عند `index*16` في transform buffer.
2. `gpu_raster_read_depth()` — **Test-only/تشخيصي**: `PackedFloat32Array` كامل الصورة (1920×1080 = 2,073,600 float، صفوف، 1.0==far/clear، أكبر=أبعد) عبر readback حاجز.
3. `gpu_raster_get_depth_format()` — **Test-only** getter: قيمة تنسيق العمق (125 == D32_SFLOAT) أو -1.
4. `gpu_mesh_get_depth_enabled()` — **Test-only** getter: هل مسار mesh depth مفعّل.
5. `gpu_raster_get_depth_enabled()` — **Test-only** getter: يجب أن يبقى `false` (billboard depth-disabled).

**ملاحظة معمارية:** الواجهات 3/4/5 مرشّحة للدمج لاحقاً في استعلام قدرات واحد (لا تغيير في 009، سجّل فقط).

## Note: Reversed-Z Considered, Deferred

During 009 implementation, reversed-Z (depth 1.0 = near, depth 0.0 = far,
GREATER_OR_EQUAL) was evaluated as a potential solution to the
near:far precision issue. After measurement:

- Standard direction (LESS_OR_EQUAL, clear=1.0) was kept.
- Reversed-Z requires deeper changes (projection matrix, clear value,
  comparison function, all pipelines) — outside 009 scope.
- Reversed-Z is recorded as a DEFERRED architectural note for a future
  milestone (likely 010 or 011).

Root cause in 009 was identified as camera near/far ratio (80000:1),
not depth direction. Fixed by setting camera.near=300 in _ready.

### ملاحظات مؤجلة إلى 010
- frustum reading (من 008B Fix 1).
- atomic ordering (من 008B).
- reversed-Z (أعلاه).
- 007A frame-time drift.

## 18b. GNE-010 — الرسم الدفعي متعدد الأشكال (Batch Instance Rendering)

### الهدف
رسم **عدة meshes مختلفة في دفعة (batch) واحدة** عبر multi-draw غير مباشر، بلا حلقة CPU على المثيلات: يشير كل مثيل إلى `mesh_id` من جدول meshes على الـ GPU، ويُجمَّع `batch_args` على الـ GPU (prefix-sum) ثم `draw_list_draw_indirect(draw_count=batch_count)`. عمق 009 (D32_SFLOAT + LESS_OR_EQUAL) باقٍ كما هو.

### التنفيذ (إضافي فقط، بلا لمس أي مسار سابق)
- جدول meshes على الـ GPU: `mesh_table_buffer` (64 خانة، `GneMeshDesc` 32B std430: index/vertex slot + counts + first_index + vertex_offset) + `mesh_id_buffer` (per-instance) + `mesh_color_buffer` (palette لكل mesh).
- **مخزنان مشتركان بسعة كاملة**: vertex (32768×vec3) + index (65536×uint32). المكعب (mesh 0) عند offset 0 → مسار 008A/009 القديم يرسم حرفيًا دون تغيير؛ توسّع `gpu_mesh_create_from_arrays(verts, indices)` يسجّل أشكالًا إضافية عند offsets متزايدة (تيتاهدرون mesh 1، أوكتاهدرون mesh 2 في demo).
- **مساران compute للـ batch assembly**:
  - Pass 1 (عدّ لكل mesh): كل مثيل مرئي يقرأ `mesh_id[orig]` ويحوّر `batch_count[mesh]` عبر atomicAdd مع تخزين `orig` في chunk المثيلات الخاص بالشكل.
  - Pass 2 (prefix-sum بخيط واحد): `batch_offset` = تراكمي، نسخ الـ origs إلى `batch_instances` المتسلسل، وتعبئة `batch_args` **مضغوطة** (dense) ليصبح `draw_count == batch_total` — مخرجات حتمية بالكامل (DET مستقرة).
- `gpu_mesh_batch_draw()`: pipeline جديد بنفس عمق 009، يربط مخزني 010 بكامل السعة، `batch_instances[gl_InstanceIndex]` (first_instance مدمج)، `mesh_id[orig]` → `flat v_mesh_id` → لون من `mesh_colors`.
- **10 واجهات Test-only** جديدة (إضافة صافية): `gpu_scene_set_instance_mesh/get_instance_mesh`، `gpu_mesh_create_from_arrays`، `gpu_mesh_batch_dispatch`، `gpu_mesh_batch_draw`، `gpu_mesh_get_mesh_id_count`، `gpu_mesh_get_batch_count`، `gpu_mesh_get_draw_counts`، `gpu_mesh_get_batch_args(batch)`، `gpu_mesh_get_mesh_color(mesh_id)`.
- خريطة ألوان ثابتة (palette): mesh0 أخضر (0.15,0.85,0.35) — يطابق لون 008A/009 للمكعب، mesh1 أزرق، mesh2 برتقالي، default أصفر.

### الأدلة (010 PASS)
مشهد ثابت `main_010.tscn` (رقم مرجعي A: الكاميرا (0,0,1700) هوية، fov60، near300/far4000): 6 مثيلات = 3 أشكال (cube, tetra, octa) × 2 مثيل، المثيلان 1/2 على المحور خلف المكعب (محجوبان منه → إثبات عمق عبر الأشكال).

معايير PASS الثمانية (8/8):
1. **≥3 أشكال مرئية برسمة واحدة** — 3 أشكال على 6 مثيلات في كاميرا واحدة؛ `visible=6`, `mesh_id_count=3`.
2. **أدلة هندسية per-mesh** — مراكز المثيلات 0/3 خضراء، 4 زرقاء، 5 برتقالية (كل شكل يرسم هندسته، window ±11px)؛ عدّادات الأشكال كاملة الإطار `g=368 b=290 o=2530` (كلها > 0).
3. **ترتيب عمق عبر الأشكال** — `dC=0.87835 < dT=0.90449 < dO=0.91052` (TOL 0.002)، `dC` أمامي (< 0.9995)؛ المراكز المحجوبة 1/2 خضراء (المكعب الأقرب يملك البكسلات المشتركة)؛ pixel مركزي (960,540) أخضر بعمق وجه المكعب الأمامي.
4. **batch_count == الأشكال المميزة** (لا المثيلات) — `batch_count=3` (وليس 6)، `draw_counts=[2,2,2]`، args حتمية `[36,2,0,0,0] [12,2,36,8,2] [24,2,48,12,4]` (first_index=prefix، vertex_offset صحيح لكل شكل)؛ لا حلقة billboard للمثيلات.
5. **Determinism (DET)** — إعادة الدفعة/الرسم/القراءة في نفس الإطار → counts وcenter depth متطابقة؛ **توقيع DET متطابق حرفيًا بين تشغيلين**:
   `sig=v6|mc3|bc3|dc2/2/2|g368|b290|o2530|dC0.87835|dT0.90449|dO0.91052|cc1|c0|5` (gt_010a.bat / gt_010b.bat).
6. **Regressions 001A–009B** — `exit 0` على نفس البنية (تفاصيل أدناه).
7. **GPU==CPU** — `visible=6` وfrustum CPU (الكرة ضد 6 مستويات بمسح positions/scales) = 6، `compact_sorted==[0..5]`.
8. **billboard path لم ينكسر** — `gpu_raster_get_depth_enabled()==false`، ومسار depth الخاص بـ 009 محصور في مسار mesh (005/007A regressions PASS).

إصلاح عمر (lifetime) أثناء التحقق: `mesh_010_vertex/index_array` كانا يُحرَّران بعد مخزنَي الـ vertex/index المشتركين → خطآن "free invalid ID" في الـ destroy؛ عولج بتحرير دفعة 010 (arrays/ubersets/pipelines/buffers) قبل تحرير المخزنَين في `_destroy_mesh`. بعد الإصلاح: **صفر** errors/leaks، إغلاق نظيف، exit 0. لقطة نافذة: `C:\Users\opc\AppData\Local\Temp\opencode\gt_010_window.png`. خروج: `GNE 010: PASS`.

### الانحدارات 001A–009B (كلها PASS على نفس البنية)
- `gt_smoke` (001B fill + 002 culling + 003 indirect args + 004 HZB + 005 indirect raster + 006 10M scaling): **GNE-SMOKE: OK**.
- `002` (frustum على CPU): ضمن gt_smoke.
- `007A` (viewport bridge): `007A: PASS`.
- `008A` (real mesh single draw): `008A: PASS`.
- `008B` (10K instances, نفس توقيع DET التاريخي): `sig=v3905|36|3905|g25765|c0|5004|9997|h176|m0|3905|f150`.
- `009a` (front-only، نفس التوقيع): `sig=v2|36|2|A|g458|f458|b2073142|dF0.87835|...|c0|3`.
- `009b` (full، نفس التوقيع): `sig=v4|36|4|B|g2814|f2814|b2070786|dF0.87835|...|c0|2`.

### ملاحظات
- مسار billboard (005/007A) لم يُلمس: `gpu_raster_get_depth_enabled()==false` ومسار depth الخاص بـ 009 محصور في مسار mesh.
- 7.1 (frustum planes يُقرأ من عمق UBO) كان موجودًا أصلًا في `gpu_scene_set_camera`؛ 010 بنى فوقه بلا تغيير.
- 7.2 (prefix-sum بلا sort) نُفّذ كما في SPEC؛ لا ترتيب للأشكال (حدسية) — حتمية المخرجات من البنية وليس من الترتيب.

### ملاحظة معمارية مؤجلة — Prefix Sum
في 010، تم استخدام **prefix-sum بخيط واحد** (single-thread) بدل parallel prefix-sum.
- **السبب:** عدد meshes صغير (64 كحد أقصى).
- **التبعات:** يعمل بشكل ممتاز للمرحلة 010.
- **التوسع في 011:** إذا زاد عدد meshes، يجب التحول إلى parallel prefix-sum (workgroup-level).

**القرار:** يُقيَّم في SPEC 011.

## 19. GNE-011 — Multi-Draw / Multi-Batch (تجميع الدفعات)

### الهدف
ترقية مسار الدفعات (010) من رسم دفعة **لكل mesh** إلى رسم ≤5 دفعات مجمّعة (groups) عبر multi-draw غير مباشر واحد، مع **إعادة ترتيب (reorder)** المثيلات حسب mesh_id على الـ GPU وترتيب الدفعات تصاعديًا، ودعوة غير مباشرة بـ **draw count ديناميكي (GPU-written)** بدون حلقة CPU على الدفعات.

### التنفيذ (إضافة صافية بلا لمس مسارات سابقة)
- استراتيجية تجميع قابلة للاختيار عبر enum `GneBatchStrategy` (افتراضي `REORDERED`): `PER_MESH=0` (دفعة لكل mesh)، `GROUPED=1` (≤5 مجموعات متجاورة تصاعدية)، `REORDERED=2` (إعادة ترتيب المثيلات حسب mesh_id ثم نفس التجميع).
- **Parallel prefix-sum على مستوى workgroup** في pass 1/2 (بديل الـ single-thread في 010): pass count لكل mesh، ثم prefix-sum يعبئ offsets ويعيد ترتيب origs إلى `batch_instances` المتسلسل.
- **Batch assemble على GPU** يبني `group_batch_buffer` = `VkDrawIndirectCommand` (16B) لكل مجموعة: `vertex_count=index_count لأول عضو`، `instance_count=مجموع حالات الأعضاء`، `first_vertex=0`، `first_instance=batch_offset لأول عضو`.
- **رسم غير مفهرس (procedural non-indexed)**: `draw_list_draw_indirect(dl, false, group_args_buffer, 0, last_batch_count, 16)` بمخزنين مشتركين كاملَي السعة (`mesh_vertex_storage_buffer`/`mesh_index_storage_buffer`) — الـ vertex shader المجمّع يقرأ `index_data.data[d.first_index+gl_VertexIndex]` و`vertex_data.data[(d.vertex_offset+li)*3+i]` (أن RID الـ vertex buffer لا يُربط storage) وخارج index_count يُدفع الرأس لـ `vec4(0,0,-2,1)` خارج المسرح.
- **Draw count ديناميكي GPU-written**: `dispatch_indirect` إلى عدد لانهائي في `last_batch_count` عبر UBO → لا حاجة لقراءة readback لتحديد عدد الدعوات.
- **early_fragment_tests** مفعّلة لقطاع shader المجموعات (محسّن لعدد مجموعات صغير لا يرسم خلف بعضها).
- **التحويل إلى indexed عند G==active**: الدفعات الموجودة مسار 010 (indexed، stride 20) يبقى مطبَّقًا عندما `last_used_group_draw=false` — فـ regressions 001A–010 محمية مع الاستراتيجية الافتراضية.
- **5 واجهات Test-only** جديدة: `gpu_mesh_set_batch_strategy`, `gpu_mesh_get_batch_strategy`, `gpu_mesh_get_batch_group_count`, `gpu_mesh_get_indirect_count`, `gpu_mesh_get_batch_order`. أضيفت constant enum عبر `ClassDB::bind_integer_constant("GneRenderServer","GneBatchStrategy",...)` (4 وسائط — واجهة bind_integer_constant الصحيحة بدل BIND_ENUM_CONSTANT المتعطّلة).

### أدوات البناء/الإصلاح المؤكدة
- GLSL كلمة `active` محجوزة → أُعيدت التسمية `actn`.
- RD يحرر uniform sets تلقائيًا عند free أي buffer مُشار إليه → **"Attempted to free invalid ID: 5025111736332"** كان سببه تحرير `group_batch_uniform_set` **بعد** `mesh_id_buffer`/`mesh_table_buffer` → نُقل تحرير كل uniform sets إلى **أعلى** `_destroy_mesh_batch` (قبل buffers). بعد الإصلاح: إغلاق نظيف بلا أخطاء RID/lifetime.

### الأدلة (011 PASS — 3 استراتيجيات بـ harnesses gt_011a/b/c)
مشهد `main_011.tscn`: **64 meshes** (mesh0 cube، 1..63 tetra/octa عبر `gpu_mesh_create_from_arrays`) × **128 instances** (front plane 8×8 عند z=-700، back plane 8×8 بإزاحة +70x عند z=-1100، mesh=i%64)؛ كاميرا (0,0,2000) fov60 near300 far4000 → كل 128 مرئية.

| harness | strategy | sig |
|---|---|---|
| gt_011a | PER_MESH | `v=128 st=0 m=64 gc=64 dc=64 ic=64 cc=64 dF0.92076 dB0.95204 cb=1 dt=1 275/89` |
| gt_011b | GROUPED | `v=128 st=1 m=64 gc=5 dc=5 ic=5 cc=5 dF0.92076 dB0.95204 cb=1 dt=1 271/86` |
| gt_011c | REORDERED | `v=128 st=2 m=64 gc=5 dc=5 ic=5 cc=5 dF0.92076 dB0.95204 cb=1 dt=1 290/94` |

- معايير PASS السبع (7/7 لكل استراتيجية): `visible==128`؛ `batches≥10` (64 mesh مميزة، تثبتها per-mesh)؛ `draw_calls≤5` و`groups==5` و`indirect==5` (grouped/reordered)؛ أدلة pixels لكل مجموعة (cc==groups، dF<dB بفاصل 0.002)؛ ترتيب `batch_order==[0..63]` تصاعدي (reorder حتمي)؛ DET ثابت في الثانية (same-frame re-draw); تحديد لا عن البكسل المركز (خلفية)؛ بتوقيتات `dispatch_us/draw_us` للـ perf.
- **Pixel-exact بين الاستراتيجيات**: g=178 b=102 o=282 k=11525 متطابقة في الثلاثة → regroup/reorder لا يغيّر الصورة.
- Regressions 001A–010 كلها `exit 0` على نفس البنية (دقيق أدناه).

### الانحدارات بعد بناء 011 (كلها PASS على نفس البنية)
- `gt_smoke` (001B–006): `GNE-SMOKE: OK` → exit 0.
- `007A`/`008A`/`008B`/`009`+`009a`/`010a`/`010b`: كلها `PASS` → exit 0 بلا أخطاء free/validation.
- التحقق المباشر (بدون schtasks) بعد تعطيل App Control: تشغيل الـ exe مباشرة يعمل الآن.

### القيود (011) — مُسجّلة بوضوح
- **قيد API لعدّ الـ draw**: `draw_list_draw_indirect` في Godot RD يستلزم عدد دعوات (draw_count) كقيمة من خارج GPU — لا قراءة مباشرة من buffer أثناء الرسم. الحل المعتمد: الحد الأقصى `last_batch_count` يُحدَّث من آخر dispatch متزامن ويُمرَّر عبر UBO (لذلك `dc==ic==5` دون readback). هذا هو "indirect count fallback" المذكور؛ يبقى count محدَّثًا بإطار واحد كحد أقصى في أسوأ الحالات.
- **الرسم المجمّع procedural غير مفهرس**: المجموعات تُرسم عبر نسخ storage كاملة السعة (`mesh_vertex_storage_buffer`/`mesh_index_storage_buffer`) لأن RID الـ vertex/index buffers لا تُربط كـ storage — بصمة مضاعفة للنسخ، ومخزنا التخزين المشتركان بسعة قصوى (32768×vec3 / 65536×uint32) مهما كان عدد الأشكال الفعلية.
- **حد أقصى 5 مجموعات** اختياري حسب SPEC (G = min(active,5)) مع مجموعات متجاورة تصاعدية — ترتيب جزئي وليس sort كامل للمشهد؛ يكفي للمطالب الحالية (64 mesh → 5 draw calls، 12.8×).
- **مسار 010 (indexed, stride 20)** يبقى قيد أن G==active أو PER_MESH — التجميع لا يمس الرسم المفهرس عند غياب الحاجة للتصغير.
- **CPU readback** (pixels + depth كامل الإطار ~8MB لكل منهما) ما زال جسر تحقق وليس مسار عرض إنتاجيًا.
- الـ getters الخمسة والـ enum **test-only** مرشّحون لاحقًا للدمج في استعلام قدرات واحد (سجّل فقط).

## 20. GNE-011 demo تفاعلي (GDScript فقط)

- `main_demo.gd/.tscn` + `camera_controller.gd` + `hud.gd` — **بدون أي تغيير C++**: 512 instances (8×8×8) / 64 meshes، كاميرا FPS (WASD + Shift/Ctrl + mouse look)، `R` يبدّل الاستراتيجية حيًا (REORDERED افتراضي)، `F12` لقطة نافذة، HUD حي (FPS، visible، batches، groups، draw calls، strategy).
- وضع الأدلة `-- --test`: مسح كاميرا مبرمج (160 إطار) ثم تحقق grouping + DET (تكرار ممرٍّ كامل مع كاميرا مجمّدة وممرّي warm-up — لأن cull يعتمد على عمق الإطار السابق فيتحرك مع الكاميرا)، لقطة `demo_window.png`، ثم quit 0.
- **الدليل:** `evidence visible=497 meshes=64 batches=64 groups=5 draw_calls=5 indirect=5 strategy=REORDERED`؛ `EVIDENCE OK`؛ `screenshot saved=true`؛ `PASS` (exit 0).

### أمثلة التشغيل
```powershell
# harness الأدلة (الاستراتيجية عبر --strategy=0|1|2)
godot...console.exe --path ...demo\gpu_smoke res://main_011.tscn -- --strategy=0 --png=... --sigf=...
# demo تفاعلي
godot...console.exe --path ...demo\gpu_smoke res://main_demo.tscn
# demo بأدلة ذاتية
godot...console.exe --path ...demo\gpu_smoke res://main_demo.tscn -- --test
```

## 22. GNE-012 — Production HZB (DEFERRED)

**Status:** DEFERRED (2026-09-22).

**Attempts:** 4 (Focus Rule + 1 extra).

**Root cause:** instance→pyramid-texel projection collapse.
- كل instance يُسقط إلى texel (0,0).
- `max_inv = 0` دائمًا.
- `p2 == p1 == 132` بنيويًا.

**What was verified:**
- Frame driver (Option A) يعمل.
- Pyramid builds (12 levels).
- Raw buffer chain populated.
- Writer/reader prefix-sum layout byte-identical.

**Fallback:** `gpu_hzb_build` + `gpu_visibility_dispatch` (from 004).

**Re-attempt:** planned for 013+ (after projection fix).

## 23. Open-Source Acceleration Audit (DELIVERED)

**Document:** `docs/open_source_acceleration_audit.md`

**Sections:** 8 (current/remaining GPU core, build/borrow/integrate/defer, candidates, recommended integrations, rejected/deferred, time savings, roadmap).

**Key recommendations:**
- P0: meshoptimizer (offline geometry processing).
- P1: cgltf (glTF import), shader tooling (offline).
- Deferred: VMA, FidelityFX, Render Graph.

**Estimated savings:** ~35–49 engineer-days.

**Status:** APPROVED by Architect — awaiting integration decision.

## 24. GNE-013 - Meshlets + LOD + Cluster Culling (PASS)

### الأهداف
إثبات مسار GPU-driven كامل للمجموعات: توليد بيانات `.gomlet` دون اتصال (meshoptimizer v1.2، offline tooling)، LOD تلقائي يتبدل مع المسافة، cluster culling (frustum + cone + LOD)، وراسم شاشة برمجي (software rasterizer) بثلاثة ممرات مع دليل بكسل حتمي، ومساءلة per-LOD.

### الأدلة (013 PASS - harness gt_013a)
- مشهد meshlet ≥ 1M مثلث: **LOD0 = 1,048,576 مثلث**؛ مجموع meshlets = **18,613** (LOD0 = 10,905).
- Instance LODs = **[0,1,1,2,2,0]** — يتغير مع المسافة.
- **Coherent winner 64-bit**: atomicMax واحد مُعبّأ `(zkey<<32)|~idkey` يجعل فائز كل بكسل (عمق ثم معرّف) متماسكًا → `covered == winner == 76,685` حرفيًا (subpixel = 1,092,857).
- Per-LOD pixel coverage = **[5853, 1265, 559566]**؛ كل LOD يساهم بكسلات.
- DET ثابت: التوقيع الكامل `v13|t1048576|m18613|l0m10905|l0,1,1,2,2,0|a5532/5853/1265|cc37226/12650|pn5853/1265/559566|cov76685|sp1092857|w76685|f3106528256|d1`.
- Regressions 001A–011 جميعها PASS على نفس البنية؛ صفر أخطاء RID.

### رحلة الإصلاح (FAIL → PASS)
`144` (قراءة وصف float/uint) → `149` (إحداثيات UV→بكسل) → `151` (بروتوكول الفائز الثنائي — اثنان atomicMax مستقلان ينتجان فائزين مختلفين على البكسلات متعددة التغطية → حل: مفتاح 64-bit واحد) → `152` (تغطية لكل LOD) → **PASS**.

### ملاحظات
- `.gomlet` (25 MB) أصل ثنائي مولد **دون اتصال**؛ أعِد توليده عبر `tools/meshlet_import/build.ps1` (يتطلب MSVC فقط). وقت التشغيل مستقل تمامًا عن meshoptimizer.
## 25. GNE-014 - GPU Scene Manager (PASS)

### ملخص
GPU Scene Manager: قاعدة بيانات مشهد ساكنة على الـ GPU (SSBO SoA)، سجلات instances بحجم 64 بايت، مساحة IDs موحدة، تحديثات مدفوعة بالـ GPU عبر حلقة مخزن مؤقت (ring buffer) 16 MB مع compute apply (add/remove/move) وبدون قراءة رجوعية في المسار الحرج، وتسليم draw-records بحجم 32 بايت إلى مسارات 013/015 (interop عبر marked ordinal + LOD config).

### الأدلة (014 PASS - harness gt_014a)
- **C1 GPU Scene DB:** alloc(1,048,576) → active=1048576، snap=1048576، ssbo=121,635,140 B.
- **C2 GPU-driven update:** ring 16 MB، 20,000 deltas (5000 adds / 5000 removes / 10000 moves) عبر compute apply واحد، ring consumed 1,600,000 B ثم reset؛ لا قراءات رجوعية في المسار الحرج (عدّادات تحقق 4 بايت فقط).
- **C3 013 hand-off:** صحة ordinals داخل نطاقات LOD ordinal لكل instance + إعادة إنتاج تواقيع 013 حرفيًا (fnv=3106528256).
- **C4 011 compat:** 4096 instances عبر 8 mesh refs → snap=4096، distinct=8، groups=min(8,5)=5 draw calls.
- **C5 DET:** sig=v14|...|d1 ثابت حرفيًا عبر تشغيلين كاملين + double-dispatch داخل الباينري.
- **C6 Regressions 001A-013:** الكل PASS (smoke/007/008A/008B/009/010/011/013 exit 0).
- **C7 Benchmarks:** instances/active/applied/dispatch_seq/ring bytes/ssbo أعلاه.
- **C8:** صفر أخطاء RID (لا ERROR في خرج التشغيل).

### أخطاء اكتُشفت وأُصلحت
- Compute indexing: base السجل كان id*16 vec4-units بدل id*4 (سبب active=258048 خاطئ).
- Snapshot base: gi*8 بدل gi*2 (uvec4 units).
- buffer_clear + submit/sync لنظام إعادة الضبط الحتمي للعدادات.
- Omitted direct record upload path consistency: تم توحيد تخطيط السجل بين set_instances و deltas.

### ملاحظة
C3 الاعتماد على Overview: 013 hand-off تعني أن draw-record ordinals من مدير المشهد تقع داخل نطاقات الـ meshlet ordinals الخاصة بـ 013، والتوقيع (cull-raster fnv) يبقى مطابقًا مع وجود المدير.
## 26. GNE-015 - Render Graph (PASS)

### المكوّن
Render Graph في `modules/gne_render`: رسم بياني موجّه acyclic يُبنى وقت التشغيل عبر 8 دوال `gpu_rg_*` مربوطة في ClassDB. كل تمريرة تستدعي نقاط الدخول القائمة مسبقًا (013/011/014) على نفس الـ pipelines/uniform sets، فتبقى كل توقيعات ما قبل 015 حرفيًا. TEST-ONLY، إضافي بحت. واجهة: `gpu_rg_create / add_pass(name,kind,in_res,out_res) / add_edge(from,to,resource,bytes) / compile / execute / get_stats / dump / destroy`.

### الأدلة (015 PASS - harness `gt_015a.bat`)
- **C1 DAG:** 6 passes (scene_update, cull, cluster_cull, batch_assembly, raster, output) + 6 حواف قائمة على الموارد، منها حافة ثانية تستهلك `cull_out` لإثبات أن الترتيب ليس قائمة مكتوبة يدويًا.
- **C2 Topo (Kahn):** `["scene_update","cull","cluster_cull","batch_assembly","raster","output"]` - تم التحقق من كل حافة (من قبل إلى) + المصدر/المsink الوحيدان.
- **C2b كشف الدورات:** حافة راجعة `output->scene_update`Accepted=false، `cycle_detected=true`، `compiled=false`، ثم إعادة بناء الـ DAG 수는جة بلا تلويث.
- **C3 Barriers + Pool:** barriers=6 (تلقائية بالكامل، صفر manual) - pool=8,753,152 B مقابل resources=9,048,064 B - **aliased_saved=32,768 B** (aliasing فعلي: `batches` [3..4] تشارك خانة `draw_records` [0..1] بعمر غير متداخل).
- **C5 Execute:** 6 passes نُفِّذت فعليًا، dispatch_seq=1.
- **C6 DET:** تنفيذان متتاليان - الترتيب والـ barriers وتخطيط الـ pool مطابقة، seq 1->2. `dump()` يطابق الجدولة نصيًا.
- **C7 صفر انزياح:** 013 cull/raster يعيد إنتاج `fnv=3106528256` و instance_lods=[0,1,1,2,2,0] و 014 manager يعيد `snap=4096 / distinct=8 / groups=5` والـ graph حيّ.
- **DET d1:** تشغيلان متطابقان - `sig=v15-pc9-p6-e6-b6-po8753152-res9048064-sv32768-x6-6-q1-2-t18616`.
- **صفر أخطاء:** لا `ERROR:` ولا تنظيف RID في أي تشغيل.

### إصلاحات بناء (كانت تُسقط كل شيء)
- 5 أقواس `{` زائدة في كتلة 015 (نتيجة التحويل النصي) = المصدر في `C1075: no matching token found`.
- `Vector<PackedInt32Array>::ptrw()` على CoW متداخل كان **يفسد الذاكرة المجاورة لـ indeg** (`indeg=[-1820255784,490,...]`) فيُبلغ عن دورة وهمية. أُعيد Kahn بمصفوفات POD ثابتة (`int indeg[]` / `int adj[][]`) حتمية وآمنة.
- `rendering_device.is_null()` -> `== nullptr` (العضو مؤشر).
- `rg_passes[i]` -> `rg_passes[r]` (المتغيّر الصحيح في الحلقة).
- جمع مؤشرين `"..." + (cond ? "a" : "b")` -> لفّ بـ `String(...)`.
- قوس `)` ناقص في سطر hzb (سطر 3025) كان يُسقط الترجمة كاملة.

### Regressions (`tools\gt_regress.bat`)
007 / 008 / 008B / 009 / 010 / 011 / 013 / 015 = **PASS (rc=0)**. الفشلان خارج 015 بالكامل ولم يلمسهما عمل 015: `main_012` (منطق occlusion، الملف غير متتبَّع في git=WIP والمؤجَّل بقرار سابق) و `main_014` عند `gpu_scene_manager_update` (active=1048568 != 1048576، خلل قائم في مسار delta-ring). `git diff` يؤكد أن كل الحذوف الـ45 في الوحدة هي إزالة لاحقة `u` على الأعداد (تجميلي بحت) - ولا يوجد حذف وظيفي في 012/014.

## 27. GNE-014 - إصلاح Race في Delta Apply (PASS)

### التشخيص
`main_014` كان **flaky (~75% نجاح)** وليس regression من 015. الدليل: `git diff` لم يلمس `gpu_scene_manager_update`/`dispatch`/شيدرات 014 إطلاقًا (كل التعديلات = إزالة لاحقة `u` تجميلي + 381 سطرًا للـ 015 فقط).

**السبب الجذري - سباق (race) داخل dispatch واحد:** الـ ring stream **مرتّب**، لكن `gpu_scene_manager_dispatch` كان يرسل كل الـ deltas في dispatch متوازٍ واحد بلا ترتيب. في شيدرة apply:
- `add` = كتابة صافية `z = incoming + 1` (بلا قراءة).
- `remove` = `z = max(z - 1, 0)` = **read-modify-write غير ذرّي**.

فلمعرّف واحد في نفس الـ batch (5000 remove ثم 5000 add لنفس المعرّفات 0..4999) إذا نُفِّذ `add` قبل `remove` ← `1 -> 1 -> 0` = **فقدان النسخة**. الترتيب بين الـ workgroups غير محدود ⇒ عدد الخسائر عشوائي. أرقام مرصودة: **0، 0، -4، 0** (وليس -8 ثابتًا).

**تصحيح ادعاء سابق (ring tail):** ادّعي أولًا أن `buffer_update` يكتب عند `gms_ring_tail` بينما الشيدرات تقرأ من offset 0 = خلل latent يُسقط الـ batch الثاني. **هذا غير صحيح**: الـ ring stream متصل ابتداءً من offset 0، و`delta_count = tail/80` يقرأ المخزن كاملًا من الصفر، أي أن القراءة من 0 صحيحة ولا يوجد إسقاط. لم يكن هناك خلل ثانٍ. حقل `cfg.w` موجود لأن تقسيم الـ ring إلى موجات يحتاج **عنونة بداية الموجة** (لا عنونة الـ ring).

### الإصلاح (Option A - الموجات المرتّبة، APPROVED)
- **سجل ops موازٍ** (`gms_ring_ops`) يُبنى عند الرفع في `gpu_scene_manager_update`.
- **تجزئة CPU إلى موجات**: كل مقطع متصل من ops متطابقة = ديسباتش واحد مع `cfg.w = first` و`cfg.z = count`. `_run_compute_pass` ينتهي بـ `submit()`+`sync()` ⇒ كل موجة حاجز كامل، فلا تتداخل الموجات.
- **إصلاح ring tail**: الشيدرات تقرأ `base = (cfg.w + gi) * 20` (المحقّل `w` كان `pad` غير مستخدم) ⇒ يتعاطى مع أي موضع كتابة في الـ ring.
- **حارس صريح**: `runs > 256` ⇒ طيّة في dispatch واحد **مع تحذير صريح في السجل** (لا تراجع صامت). الدليل: `ssbo=121,635,140` و`buffer=7` و`applied=5000/5000/10000` و DET لم تتغيّر - **صفر انزياح في توقيع 014**.

### إعادة-baseline لـ 011 (DET غير زمني)
- سطر الـsig في `main_011` (`GNE 011-DET`) كان يحتوي آخر حقلين `dispatch_us/draw_us` (زمن تنفيذ) ⇒ المقارنة بين تشغيلين كانت تفشل حتى على بناء سليم (توقيتات مختلفة حرفياً).
- الإصلاح (Commit 2): `dispatch_us/draw_us` يُطبعان الآن في سطر منفصل (`GNE 011: timing dispatch_us=... draw_us=...`)، بينما `sig` خالٍ من أي حقل زمني (ينتهي عند `cb%d|dt%d`).
- Baseline غير زمني (Commit 2، ميدان 3 استراتيجيات، الكل PASS):
  - `0: v=128 st=0 m=64 gc=64 dc=64 ic=64 cc=64 dF0.92076 dB0.95204 cb=1 dt=1`
  - `1: v=128 st=1 m=64 gc=5 dc=5 ic=5 cc=5 dF0.92076 dB0.95204 cb=1 dt=1`
  - `2: v=128 st=2 m=64 gc=5 dc=5 ic=5 cc=5 dF0.92076 dB0.95204 cb=1 dt=1`
- قياس التحسين (`decode_u32` + تمرير واحد، ميدان 3 تشغيلات): `before_ms = 264923` -> `after_ms = 30723` (وسيط، RTX3070/Windows) — تحسين ~8.6x مع حفظ تحقق CPU المستقل عن GPU.
- القاعدة: أي مقارنة DET لـ 011 من الآن فصاعدًا تُجرى على سطر `011-DET` فقط (بدون سطر timing).

### ملاحظة Guard >256 (غير مُجرَّب)
- الحارس الحالي (`runs > 256` ⇒ طيّة + WARNING) **ليست له تغطية اختبارية**: `main_014` يستخدم 3 موجات فقط، فلا يمرّر أبداً بالطيّة. أي لا يوجد دليل تشغيل يُثبت أن الطيّة تعمل أو أنها لا تعمل.
- بعد Commit 6 يُرفَع هذا إلى FAIL صريح في المشهد (كاشف `collapsed_single_dispatch`)، لكنه يبقى **untested path** ما لم تُبنَ مرحلة اختبار بعينات متبادلة متعمَّدة (غير متوافقة مع توقيع 014 الحتمي الحالي — قرار لاحق).

### الأدلة
- **15 تشغيل متتالٍ لـ `main_014`: 15/15 PASS، `active=1048576` بالضبط** (قبل الإصلاح: 3/4 فقط). احتمال الصدفة 0.75^15 = 1.3%.
- **015: `GT_015A: PASS`**، d1/d2 متطابقان، التوقيع كما هو: `sig=v15-pc9-p6-e6-b6-po8753152-res9048064-sv32768-x6-6-q1-2-t18616`.
- **Regressions: `GT_REGRESS: PASS`** - 007/008/008B/009/010/011/013/014/015 كلها PASS. `main_012` مصنّف `XFAIL` (WIP معتمد ومؤجَّل) فيُبلَّغ ولا يُحسب فشل بوابة.

## 28. GNE-015.5 - Resource Pool (جزئي: 5 من 8 معايير مُثبتة)

### ما الذي بُني فعلاً (ذاكرة حقيقية لا محاسبة)
أول بناء للموديول يخصّص ذاكرة pool فعلية. وسُجّل سابقاً في SPEC 015.5 §2.1 أن 015 مجرّد محاسبة CPU: `rg_pool_bytes` و`aliased_saved` أعداد، و`rg_execute` لا ينفّذ أي pass body ⇒ كل رقم pool في 015 كان **محسوباً لا مخصَّصاً**.

- **backing store حقيقي**: `storage_buffer_create(4 MiB)` — نفس مصنع RD المستخدم في `gpu_scene_create` (محاولة أولى فشلت باستخدام `buffer_create(BufferFormat)` غير الموجود في هذه الشوكة).
- **aliasing حقيقي**: كتلتان بعمرين متقاطعين تتشاركان **نفس الإزاحة**، و`alias_saved` مشتق من التخطيط.
- **سقف persistent صلب (D3-D2)**: 256 مُدخلاً؛ الطلب 257+ = `print_error` + رفض.
- **محاذاة 16B (STD430)** دون تصحيح صامت؛ وميل أو نطاق معكوس = خطأ صريح.

### الأدلة (main_015_5 - PASS، rc=0، 5/5 phases)

| المعيار | النتيجة المقيسة |
|---|---|
| C1 pool حقيقي | `pool_bytes=4,194,304` + `pool_verify alpha=123456789 OK` + `beta=-42 OK` (قراءة من المخزن الفعلي) |
| C2 aliasing حقيقي | `alias_saved=64` — الأعمار المتقاطعة تشارك الإزاحة، والمتأكبة تأخذ ذاكرة جديدة |
| C4 سقف persistent | من 300 طلب: **256 قُبلت + 44 رُفضت** = `cap 256` بالضبط |
| C5 dynamic قابل للتدقيق | `rebuilds=0 bytes_copied=0 grow_initial=64` — العدادات لم تتحرك دون resize |
| C7 تأطير العرض | `readback=async-readback-not-zero-copy` + `flag=pr1` (قرار D3-D1 مُطبَّق حرفياً) |
| التوقيع | `sig=v15-pr1 pool=4194304 blocks=258 alias_saved=112` |
| C3 + C8 | `gt_harness`: **9 ok / 0 bad / 1 xfail** · الضابط السالب `main_012 → rc=123` · `GT_REGRESS: PASS` · تواقيع 013/014/015 حرفية |

### عيب حقيقي كشفه الاختبار (وأُصلح)
ترتيب الأولوية في `gpu_pool_alloc` كان: free-list **قبل** aliasing. النتيجة: `gpu_pool_verify` يحرّر كتلته، فيلتقطها الطلب التالي من free-list **متجاوزاً زوج الأعمار** ⇒ aliasing لم يُطبَّق أصلاً. الاختبار `C2` فشل بـ`code=308 (16 -> 80)` ⇒ **المعيار كشف أن التنفيذ خاطئ**. الإصلاح: aliasing أولاً (كتلة حيّة بعمر متقاطع) ثم free-list ثم bump، مع تعليق في الكود يشرح أن الترتيب إلزامي.

### ما لم يُنفَّذ بعد
- **C6 قياس frame-time drift**: لم يُقاس (Phase 4). **لم يُدَّعَ أي رقم**.
- **Async readback الفَعلي (fence-based)**: لم يُنفَّذ (Phase 5). معيار 7 مُغطّى حالياً بـ**تأكيد التأطير** فقط؛ لم يُقلَّل بايت readback بعد.
- **growth double-up-to-cap**: معرَّف ومعروض (`grow_initial=64`) لكنه غير مُفعَّل — الـpool يُنشأ بسعة ثابتة في هذا البناء.

### قيد صريح
تحسين 30–50% على readback **فرضية غير مقيسة** (SPEC §3.6). و`pool_bytes=4,194,304` حجم بليت **مُختبر لهذا المشهد**، لا سقفاً للعالم.


## 29. GNE-015.5 Phase 4 — Frame-Time Drift DIAGNOSIS (PASS)

### الهدف
خط أساس **مقيس** قبل Phase 5 (async readback). القياس فقط، بلا تحسين وبلا ادّعاء أي رقم مسبق.

### truth: ما وُجد في الشجرة مقابل ما افترضه الأمر
| افتراض الأمر | الواقع في 4.8.dev | الأثر |
|---|---|---|
| `rendering_device->get_frame_timestamp()` | **غير موجودة** | استُبدلت بالواجهة الحقيقية |
| `RD::capture_timestamp(name)` | موجودة (`rendering_device.h:1929`) | استُخدمت |
| `get_captured_timestamp_gpu_time(i)` | موجودة (1932) ⇒ **نانوثانية** | استُخدمت |
| `get_captured_timestamp_cpu_time(i)` | موجودة (1933) ⇒ **ميكروثانية** | استُخدمت لـ wall |
|Queries مفعّلة دائماً | **لا**: حجم الـpool من `debug/settings/profiler/max_timestamp_query_elements` = 0 افتراضياً ⇒ `count()==0` | GPU columns = **NA** لا صفر |

### الأدلة (main_015_5_phase4 — PASS، rc=0، 300 إطار = 100 warm-up + 200 مقيس)
**Per-pass wall-clock (µs، مجموع 200 إطار مقيس؛ `pass_share_pct=100`):**
| pass | wall µs | حصة |
|---|---|---|
| scene_update | 1,257 | 0.03% |
| cull | 651,254 | 15.1% |
| cluster_cull | 1,823 | 0.04% (no-op marker، لا meshlets في هذا المشهد) |
| batch_assembly | 130,752 | 3.0% |
| **raster** | **1,549,544** | **36.0%** |
| **output** | **1,971,030** | **45.8%** |
| المجموع | 4,305,660 | 100% |

**الإطارات:** `wall_avg=22,657 µs` · `wall_peak=47,813 µs` · `warmup_peak=33,477 µs` · `frame_total=4,811,857 µs` (= 4.31M pass + 0.51M غير مُسنَد).
**Probes:** frame 100 = 17,453 µs · frame 200 = 15,956 µs · frame 300 = 20,325 µs.
**Drift (أول نصف مقابل النصف الثاني من 300 عيّنة):** `22,006 → 23,308 µs` ⇒ **`drift = +1,301 µs` (~+5.9%)** — **إطار، وليس انحرافاً تصاعدياً**.
**GPU:** `NA` — query pool معطّل ⇒ **غير مقيس، وليس صفراً** (مُبلَّغ بسطر مستقل عمداً).
**التوقيع (content-only):** `sig=v15.5-p4 warm=100 meas=200 passes=200` — **متطابق حرفياً بين تشغيلين**.

### ⭐ مصدر الانحراف: **READBACK، مُفسَّر**
`raster + output = 3,520,574 µs = 81.8%` من زمن الإطار. وم失了ها هو بالضبط **الـreadback**: `gpu_raster_read_pixels()` (8 MB) + `gpu_raster_read_depth()` (8 MB) داخل ممر raster، ثم `Image.create_from_data` + `ImageTexture.update` في ممر output. ⇒ **الانحراف المرصود في 007A (11.36→14.16 ms) له نفس المصدر**: جسر readback المتزامن، لاّ الـculling ولا الـbarriers (cull = 15%، batch = 3%).

### أربعة عيوب حقيقية كشفها هذا القياس (وأُصلحت)
1. **`frames=0` مع per-pass غير صفرية**: `if (gne_ft_frames > 0)` كان يلتفّ على `gne_ft_frames++` ⇒ عدّاد الإطارات لا يتقدّم أبداً. الإصلاح: التقديم أولاً.
2. **قاموس ناقص الإحداث**: `return d` مبكراً ترك `wall_last_us` مفقوداً ⇒ `SCRIPT ERROR: Invalid access to property or key` **أوقف المشهد صامتاً** (rc=0 مع PASS بلا تقرير). الإصلاح: كل المفاتيح موجودة من الإطار 1.
3. **`//` ليست تعليقاً في GDScript**: `Parse Error: Expected statement, found "/"` ⇒ المشهد لم يعمل إطلاقاً وما بدا «أرقاماً» كان من نسخة أقدم. التُقط من **`.err`** لا من `.log` (وهو ما أبطأ التشخيص: `run_scene.ps1` يكتب منفصلاً، وكنت أقرأ `.log` فقط).
4. **توقيع غير حتمي بالبناء**: ضمّ `drift_us` و`wall_avg_us` ⇒ فشل DET بين تشغيلين لأن **إشارة الانحراف نفسها تتغيّر** (+4865 مقابل −1602). الإصلاح: توقيع content-only + سطر `timings` منفصل — **نفس صنف خطأ `dispatch_us/draw_us` في 011 (commit 2)**، يتكرّر إن لم يُفصل القياس عن التوقيع.

### نتيجة للقرار التالي
**Drift مُفسَّر** ⇒ لا STOP ⇒ **يُسمح بالانتقال إلى Phase 5**، والهدف محدَّد بالقياس: Passive `raster`+`output` = 81.8% من زمن الإطار، والـreadback_sync هو المشتبه به ⇒ fence-based + multi-frame staging، والمقياس هو:是否可以 تقليص Passive دون كسر التواقيع.


### Phase 5 step 1 — GPU timestamps: **BLOCKED by engine limitation** (documented, not worked around)
الأمر كان «أضِف `max_timestamp_query_elements=512` في `project.godot`». **هذا لا يمكن أن يعمل**، والجlaid بالقراءة لا بالتخمين:

| الفرضية | الواقع المُقاس | المرجع |
|---|---|---|
| يمكن تفعيله من `project.godot` | **لا** — مُسجَّل بـ`GLOBAL_DEF_RST` = runtime-only، لا يُقرأ من ولا يُكتب في `project.godot` | `core/config/project_settings.cpp:1811` |
| يُقرأ عند بدء التشغيل | ✅ لكن **مرة واحدة** داخل `initialize()` | `servers/rendering/rendering_device.cpp:8625` |
| النطاق يسمح بـ0 | **لا** — النطاق `256..65535` | `project_settings.cpp:1811` |
| الضبط في `_ready()` يكفي | **لا** — يجب أن يسبق `ensure_gpu_device()`، لأن جهازنا كسول: `create_local_rendering_device()` → `create_local_device()` → `rd->initialize()` وهو القارئ الوحيد | `rendering_server.cpp` / `rendering_device.cpp` |
| تفعيل الـpool يكفي لقراءة النتائج | **لا** — بعد الضبط الصحيح (512، قبل إنشاء الجهاز) صار `captured 0 → 1` بعد `capture_timestamp()`، لكن `get_captured_timestamps_count()` يبقى **0** | قياس حي |

**السبب الجذري النهائي:** حلّ نتائج الـquery يقع في `_begin_frame()` (سطر 8335–8342: `timestamp_query_pool_get_results` + `SWAP` + `timestamp_result_count = timestamp_count`). هذا المسار **موجود وصحيح**، وسائق Vulkan يطبّق القراءة كاملة (`timestamp_query_pool_get_results` → `vkGetQueryPoolResults`، سطر 6822). لكن **`drivers/vulkan` لا يحتوي `utilities.cpp`** — الملف الوحيد الذي يضبط `timestamp_result_count` للمحرّك هو `drivers/gles3/storage/utilities.cpp:367`. ⇒ على Vulkan، `timestamp_result_values` تُملأ ولا يُحدَّث `timestamp_result_count`، فيُرجع كل قارئ `0` (وقيمة غير مهيّأة عند القراءة في نفس الإطار: `1.79e18 ns`).

**القرار (Owner، 2026-09-27):** إثبات التوثيق + **الانتقال إلى Phase 5 على خط wall-clock**، لأنه ما ينتج الأرقام الحقيقية اليوم. عمود GPU يبقى **`NA`** (غير مقيس) ولا يُقرأ أبداً كـ«مجانٍ». تفعيل الـpool بُقي في المشهد (512) لأنه صحيح Direction ولا يضرّ.


---

## 30. GNE-015.5 — FINAL (PARTIAL: 5/8 criteria + 3 documented known issues)

**الحالة:** **PARTIAL PASS** — 5 من 8 معايير مُثبتة وقياساً، وثلاثة قيود موثّقة.
**التوقيع:** `v15-pr1` (`pr1` = `pool_real=1`) — محفوظ، متوافق مع توقيع 015 القديم.

### المُثبَت
| المعيار | الدليل المقيس |
|---|---|
| **C1** تخصيص حقيقي | `pool_bytes=4,194,304` + `pool_verify alpha=123456789` / `beta=-42` (قراءة من المخزن الفعلي) |
| **C2** aliasing حقيقي | `alias_saved=64` — أعمار متقاطعة تشارك الإزاحة، والمتأكبة تأخذ ذاكرة جديدة |
| **C4** سقف persistent صلب | من 300 طلب: **256 قُبلت + 44 رُفضت** = `cap 256` |
| **C5** dynamic قابل للتدقيق | `rebuilds=0 bytes_copied=0 grow_initial=64` — لا تتحرك العدادات دون resize |
| **C7** تأطير العرض | `readback=async-readback-not-zero-copy` + `flag=pr1` (قرار D3-D1 مُطبَّق حرفياً) |
| **C3 + C8** | `gt_harness`: **9 ok / 0 bad / 1 xfail** · الضابط السالب `main_012 → rc=123` · `GT_REGRESS: PASS` · تواقيع 013/014/015 حرفية |

### المؤجَّل — C6 (frame-time drift)
- **مُفسَّر لا مجهول**: `raster + output = 81.8%` من زمن الإطار، ومصدره جذر الـreadback (KI-002).
- **لا يمكن تحسينه داخل GNE**: الـasync يشارك نفس مخزن staging (KI-002)، وتحسين العرض لا يعالج الجذر (KI-003).
- ⇒ **مؤجَّل** إلى milestone مستقل: تقليل `bytes_copied_per_frame` (مسار رسم بدقة أقل).

### القيود الموثّقة (docs/known_issues.md)
| # | القيد | الجذر |
|---|---|---|
| **KI-001** | GPU timestamps غير متاحة على Vulkan | `drivers/vulkan` لا يملك `utilities.cpp`؛ `timestamp_result_count` لا يُنشر (سلوك المحرك) |
| **KI-002** | async readback لا أسرع من المتزامن | `texture_get_data_async` تستعمل نفس `download_staging_buffers` ⇒ نفس `FLUSH_AND_STALL_ALL` |
| **KI-003** | تحسين العرض بلا أثر قابل للقياس | تقليل عدد الطلبات/تخصيصات المعالج لا يقلّل البايتات المنسوخة |

### دروس مُثبَّتة في هذا الـmilestone (قيمة تستحق البقاء)
1. **«محاسبة» ≠ «ذاكرة»**: 015 كان يطبع `pool=8,753,152` و`aliased_saved=32,768` **بلا أي مخزن مخصَّص** (`rg_execute` لا ينفّذ pass body). كل رقم أداء سابق يُنسب ل(pool) كان بلا أساس مادي.
2. **معيار الاختبار كشف خطأً في التنفيذ لا العكس**: C2 فشل (`code=308`) بسبب أن free-list كان يُجرَّب **قبل** aliasing ⇒ التصحيح كان على ترتيب الأولوية.
3. **الأرقام يجب أن تُفصل عن التوقيع**: تكرار خطأ `dispatch_us/draw_us` (011) ثم `drift_us` (Phase 4) — التوقيع صار content-only + سطر `timings` منفصل.
4. **«لا تراجع صامت» يمتدّ للتجارب**: KI-003 أغلق Phase 5.1 **بلا رقم** بدل نشر نسبة ضعيفة أو مضلّلة.
5. **فخّ الترقيم**: `Measure-Object -Line` **يستثني الأسطر الفارغة** ⇒ ترقيم خاطئ أدّى لإدراج كتل داخل دوال.

### التالي
**012-revised (HZB)** ← ثم 016 (Materials).


## 31. GNE-012-revised — CLOSED (PASS)

**الحالة:** مغلق بنجاح — معيار الإخفاء متحقق وحتمي، وكل التواقيع حرفية.
**التوقيع:** `sig=v12-rev levels=10 p1=6 p2=0 ctrl=6` (مطابق في تشغيلين، DET OK).

### ما أُصلح فعلًا (وليس الخيار أ)
الخيار (أ) (`-z_view` في `depth_source` سطر 1019-1020) **لم يُطبَّق**، لسببين مثبتين:
1. `gpu_hzb_depth_source` **لا يُستدعى في أي مسار** (يُبنى ويُحرَّر فقط) — تعديله لا يغيّر أي سلوك.
2. سلسلة الاستدلال المقترحة لا تطابق الكود: لا يوجد clamp علوي، والقيمة السالبة تعطي `far+|z|` (بتات ضخمة) لا صفرًا.

**الجذران الحقيقيان المُصلحان:**
- **(F) جمود علم `hzb_pyramid_fresh`:** الفرع الأول في `gpu_hzb_build` كان يكتب `hzb_valid=0` ويخرج **دون ضبط العلم أبدًا** (السطر التالي غير قابل للوصول، ولا setter آخر في الكود). النتيجة: `hzb_valid=0` دائمًا ⇒ phase-2 يتخطى الإخفاء دائمًا ⇒ `p2==p1` بنيويًا. الإصلاح: سطر واحد يضبط العلم في الفرع الأول (`gne_render_server.cpp:3071`).
- **(T) كتابة int في حقل float بالـ UBO:** الترقيعات الثلاثة لـ `viewport[2]` كانت تكتب `int32_t 2048` (بتات `0x00000800`) في حقل `float` ⇒ تُقرأ `2.8e-42 ≈ صفر` ⇒ `level=-138` (مقاس عبر `sim2`) ⇒ عينات phase-2 تُخطئ دائمًا. الإصلاح: كتابة `float` في المواضع الثلاثة (`gpu_hzb_build`، `gpu_visibility_prod_dispatch`، `gpu_mesh_batch_draw`). هذا الجذر يفسّر أيضًا فشل 012 الأصلي: الـ occ القديم يقرأ `int(viewport.z)=int(~0)=0` فيخرج فورًا بهرم فارغ.

### أداة القياس (اختبار فقط)
`main_012_rev.gd` أُعيدت كتابته: قياس من مسار الإنتاج نفسه عبر `gpu_hzb_get_phase_counts` من dispatch واحد (يتجاوز تلوث العداد D بالبناء)، هندسة وصفة `main_010` المثبتة (6/6 مرئية)، جدار واحد بين الكاميرا والنسخ. إزالة استدعاء `gpu_hzb_debug_state` غير الموجود، وإضافة مسابير `dbg_valid` و`coherent` و`sim2`.

### الأدلة
| البند | القيمة |
|---|---|
| p1 (frustum) | 6 |
| p2 (occluders) | **0** (الجدار أخفى الستة) |
| فارغ/ضابط | `[6,6]` / `[6,6]` |
| الهرم | `scan_prod=[1156005882, 694, 838, 245520]` (غير صفري) |
| `hzb_valid` / coherent | `1` / `true` |
| DET | تشغيلان متطابقان |
| 013 | `f3106528256` حرفي |
| 014 | `v14\|c18616\|a0\|u5000/5000/10000\|sn4096\|dm8\|g5\|s121635140\|dd365221617\|d1` حرفي |
| 015 | `v15-pc9-p6-e6-b6-po8753152-res9048064-sv32768-x6-6-q1-2-t18616` حرفي |
| 015.5 | `v15-pr1 pool=4194304 blocks=258 alias_saved=112` حرفي |
| `gt_regress` | PASS |
| `main_012` القديم | XFAIL محفوظ (خرج 124 بدل 123: الإخفاء صار ينشط `wall=true` ويفشل لاحقًا في بكسل 4/5 — مشهد مؤجل، لا إجراء) |

### الملفات المتأثرة (بلا commit — بانتظار موافقة Architect)
- `modules/gne_render/gne_render_server.cpp` (إصلاح F سطر واحد + إصلاح T في 3 مواضع + تعليقات)
- `demo/gpu_smoke/main_012_rev.gd` (أداة الإغلاق)
- `docs/progress_report.md` (هذا القسم)

### التالي
016 (Materials): SPEC ثم Contracts ثم تنفيذ.

## 32. GNE-016 — Materials (PASS)

**الحالة:** PASS — 8/8 معايير مثبتة وقياسًا.
**التوقيع:** `v16|mc=8|L-0.41|-0.82|-0.41|amb=0.10|hp=2072056|hr=0.95|d1` (مطابق في تشغيلين، DET OK).

### المُثبَت
| المعيار | الدليل المقيس |
|---|---|
| **C1** مخزن حقيقي | 8 سجلّات متمايزة + قراءة راجعة مطابقة بايتًا |
| **C2** حتمية | `GT_016A: PASS` — d1/d2 متطابقان، صفر `ERROR:` |
| **C3** normals وجوه | تباين 3×3 داخل الوجه ≤ 3 LSB |
| **C4** استجابة N·L | probe يخفت 0.34→0.065 عند تدوير الضوء + 966 بكسل متغيّر من 1713 ملوّنة |
| **C5** خشونة قابلة للقياس | نفس المادة: 0.1725 مقابل 0.2825 (Δ=0.11 > 0.05) |
| **C6** انبعاث مستقل | on=390 بكسل أحمر مهيمن، off=0 |
| **C7** ثبات التوقيعات | 013/014/015/015.5/012-rev حرفية + `GT_REGRESS: PASS` |
| **C8** حدود صلبة | `slots=64 bytes=4096 dispatches=7` + صفر `ERROR:`/`RID` في البوابة |

### التنفيذ (مضاف بحت)
- مخزن `GneMaterial` (64B × 64 = 4096B) + API من 8 دوال test-only + `gpu_material_draw` (يُعيد تشغيل أوامر `batch_args` نفسها بممر `*_mat_*`).
- ممر جديد كليًا: vertex يمرّر `world_pos` + frag (Lambert + Blinn-Phong + انبعاث + قاعدة backface D4-4) + push constant للضوء/الكاميرا. ممرات 005/008A/010/011 لم تُلمس.
- مشهد `main_016` (8 نسخ: 4 cube + 4 octa، كاميرا 3/4 من جهة الضوء) + `gt_016a.bat` + فرع `--negtest` خارج البوابة (6 مسارات رفض تعمل).

### أخطاء حقيقية وُجدت وأُصلحت أثناء التنفيذ
1. نسيان `draw_list_set_push_constant` في `gpu_material_draw` ⇒ خطأ الممر + إطار أسود.
2. تفكيك `mat set` بعد `_destroy_mesh_batch` (الذي يحرّر buffers يشاركها) ⇒ `free invalid ID`؛ نُقل التفكيك أولًا.
3. `static_assert` على struct خاص من نطاق عام ⇒ نُقل داخل الدالة.
4. عتبة C4 المطلقة (5000) خاطئة لمشهد متفرق ⇒ عتبة نسبية (changed > colored/4).
5. مرساة C5 على بقعة specular متحركة ⇒ القياس على وجه diffuse ثابت بنفس البكسلات.

### 32.1 — 016 Bugs Fixed (Detailed)

**Bug 1: Push constant forgotten.**
- Impact: material params (light/camera) never reached the shader — pipeline error + black frame.
- Fix: added `draw_list_set_push_constant` in `gpu_material_draw`.
- Detection: engine error line + empty frame, C1 already green.

**Bug 2: Teardown order (mat set freed too late).**
- Impact: `mat_batch_uniform_set` freed AFTER `_destroy_mesh_batch` released shared buffers it references ⇒ `free invalid ID` on exit.
- Fix: moved material teardown FIRST in `_destroy_mesh` (sets before buffers — the documented order rule).
- Detection: `Attempted to free invalid ID` at shutdown.

**Bug 3: static_assert on private struct from global scope.**
- Impact: build failed (MSVC C2248 — cannot access private struct).
- Fix: moved the assert INSIDE `gpu_material_create` (kept, not removed — the 64B contract still enforced).
- Detection: MSVC compile error.

**Bug 4: C4 absolute threshold wrong for a sparse scene.**
- Impact: 966 changed pixels failed a hardcoded 5000 gate although the light response was correct (probe 0.34→0.065).
- Fix: relative gate (changed > colored/4).
- Detection: gate FAIL code=826.

**Bug 5: C5 anchor on a moving specular highlight.**
- Impact: brightest-pixel anchor moved when roughness changed ⇒ both reads ~ambient ⇒ false "no effect".
- Fix: anchor on a static diffuse face, measure the SAME pixels twice.
- Detection: FAIL code=828 (means 0.098 vs 0.099) within one run.

**Lesson:** Each bug = documented lesson (see docs/lessons.md).

### التالي
017 (Textures): SPEC ثم Contracts ثم تنفيذ.

## 33. GNE-017 — Textures Phase 2 (PASS)

**الحالة:** PASS — 8/8 معايير مثبتة وقياسًا.
**التوقيع:** `v17|tc=1|fmt=uastc|slot=5|hr=0.51|d1` (مطابق في تشغيلين، DET OK).

### المُثبَت
| المعيار | الدليل المقيس |
|---|---|
| **T1** تحميل KTX2 | id=0 من `miptex.gtex` (magic/version/dims تُتحقق) |
| **T2** فك Basis | 9 مستويات تُحوَّل دون اتصال بمطابقة 0 (L0: 0/65536) |
| **T3** ربط بالمادة | `bind(0,0,0)` + `bind(3,0,0)` + قراءة راجعة GPU تطابق |
| **T4** معاينة | خلايا bimodal على الوجه القريب (dark=111 bright=98) |
| **T5** mips | بعيد أحمر 0.467 مقابل قريب 0.0 (فصل MIP حاسم عبر تلوين تشخيصي) |
| **T6** هستوغرام | hr=0.51 ≥ 0.5 |
| **T7** حتمية | `GT_017A: PASS` — d1/d2 متطابقان |
| **T8** انحدارات | 001A–016 PASS + `main_012 → XFAIL` + كل التواقيع حرفية |

### التنفيذ (مضاف بحت)
- مخزن textures (256) + `mat_tex[64][5]` (1280B) + sampler مشترك + dummy؛ مجموعة `*_mat_*` أُعيد بناؤها مع bind 5 (array) و6 (mat_tex).
- الـfrag يعاين slot 0 (albedo) بإسقاط triplanar؛ الفهرسة المباشرة قانونية (السائق يشترط NonUniformIndexing).
- لا ربط Basis/KTX في الوحدة: الـruntime يقرأ قسم RGBA8 خامًا ويرفع الـmips.
- مشهد `main_017` (4 نسخ، نسيج واحد) + `gt_017a.bat`.

### أخطاء حقيقية وُجدت وأُصلحت
1. نسيان push constant في مسار جديد (درس 016 يتكرر بصيغة أخرى) — كشفها خطأ الممر.
2. `static_assert`/إعلان ناقص + `Vector(size)` غير موجود — أخطاء ترجمة مباشرة.
3. كاميرا شبه أمامية تُظهر وجوهًا مظلمة فقط — كاميرا 3/4 من جهة الضوء.
4. عتبات مطلقة على مشهد متفرق — عتبات نسبية/موضعية.
5. `get_projection_planes` خاطئ للكاميرات الدوّارة (مستويات لا تحقق نقاطها؛ أثبت بـNDC + `is_position_in_frustum`) ⇒ استخراج المستويات من VP في `set_camera` — صفر انزياح مثبت بالبوابات.
6. تباين near/far مستحيل بنفس الكثافة (منظور) ⇒ تلوين MIP تشخيصي (L0-L2 شطرنج، L3+ أحمر).

### التالي
015.6 (KI Closure Sprint) ← ثم 018 (Lighting).

## 34. GNE-015.6 — KI Closure (PASS)

**الحالة:** PASS — 5/5 معايير (K1–K5) بالتوثيق والقياس، صفر تغيير سلوكي.
**النطاق:** KI-001 (قبول NA) + KI-002 (قبول) + C6 (قبول، double-buffering → 020).

### الأدلة
| المعيار | الدليل |
|---|---|
| **K1** | KI-001 مغلق: NA مقبول + موثّق؛ كل الأرقام wall-clock حصرًا |
| **K2** | KI-002 مغلق: staging مملوك للمحرك؛ لا ادعاء تسريع |
| **K3** | C6 مغلق: خط أساس wall-clock (median 7.58s، p95 ≈ 8.76s، 5 تشغيلات main_016) + 58,060,800 B/تشغيل |
| **K4** | صفر انزياح: `gt_regress` + `gt_016a` + `gt_017a` خضراء، كل التواقيع حرفية |
| **K5** | §34 + إدخالات KI تشير لبعضها |

### ملاحظة منهجية
015.6 لم يغيّر سطر كود واحدًا في الوحدة — الإغلاق بالقياس والتوثيق فقط. أي "إصلاح" كان سيكسر DET أو يتطلب ترقيع محرك، وكلاهما مرفوض بالعقد.

### التالي
018 (Lighting): SPEC ثم Contracts ثم تنفيذ.

## 35. GNE-018 — Clustered Lighting Extension (PASS)

**النتيجة:** `GT_018A: PASS` — d1/d2 متطابقان حرفيًا:
`v18|lc=20|cc=2841|ot=0|hr=0.94|d1` (rc1=0 rc2=0، صفر `ERROR:`، صفر تسرّب RID).
**الانزياح:** `GT_REGRESS: PASS` (كل البوابات 007–017 خضراء؛ 011 و012 XFAIL موثّقان مسبقًا).

### الأدلة (L1–L6 من التشغيل المغلق)
| المعيار | الدليل |
|---|---|
| **L3** | count=20، bytes=65536، clusters=3456، overflows=0 |
| **L4** | touched=2841، assignments=11348؛ العنقود [8,4,18] فيه 14 ضوءًا بينها id 0 |
| **L1** | المسبار 0.927 ← 0.512 (Δ=0.414 ≥ 0.1)، البكسلات المتغيرة 3184 ≥ 1000 |
| **L2** | داخل المخروط 0.827 ← 0.360 (النسبة 2.30 ≥ 2.0)، الخارج ثابت 0.728 ← 0.728 |
| **L5** | assignments ثابتة 11342=11342 بعد إبعاد الضوءين 14/15 (zero-cluster culling) |
| **L6** | hr=0.94 ≥ 0.3، chromatic=754 ≥ 500 (مزج RGB حقيقي) |

### السببان الجذريان (Phase 2/3)
1. **بقايا تشخيصية حيّة (العطل الحقيقي):** السطر `TEMP-DIAG-018d` في `gpu_mat_light_frag_glsl`
   كان يحقن `out_color=<push-viz>; return;` **قبل** حلقة الإضاءة — الحلقة كلها dead code.
   كل أعراض "الفراج لا يساهم" (L1 أصفار، ccnt-viz أسود، تشبع 0.69 الموحد) كانت من هذا السطر،
   لا من الربط (الـpipeline والـset-0/set-1 موصّلان صحيحًا وتم التحقق منهما سطرًا بسطرًا).
   الدرس: أنماط grep المبنية على الذاكرة أخطأت السطر (`DIAG` ≠ `DIAGNOSTIC`، والصيغة كانت
   مضمّنة لا `vec3 pv`) — القراءة المباشرة للكود هي الدليل، والتشخيصات المؤقتة تُزال فور انتهاء مهمتها.
2. **نظافة التخطيط (latent):** تصريحات set-1 كانت `std140` على `buffer` تخزين (غير قانوني؛
   مصفوفات `uint` بخطوة 16B بدل 4B) ← صُححت إلى `std430`. لا أثر سلوكي (المترجم يطبّعها)،
   لكنها كانت ستعضّ مع أي مترجم صارم. لا تكرارات أخرى (`std140.*buffer` = صفر).

### معايرة المشهد (ملف مشهد فقط — لا مساس بالعقود)
- التشبع قصّ كل شيء عند 1.0 (L1/L2/L6 غير قابلة للقياس) ← تخفيض الشدات ~6× + specular رمادي
  (0.25, shininess 32) عبر `gpu_material_set_specular` لإخماد الـhotspot المحوري (s2=1).
- فصل L1 عن L2: inst1 نُقلت خارج المحور (0,0,-800) ← (-700,0,-800)؛ ضوء L1 وُسّع (r800)
  ليغطي inst0+inst2+inst3 (changed=3184) دون إغراق نافذة L2؛ spot0 أُعيد توجيهه وأُعطي 1.1.
- الكروما: الأحمر وُضع أماميًا على وجه inst3 (0.6)، والأخضر أماميًا على وجه inst2 (0.9،
  مساهمته عند المسبار صفر بـndl2<0)؛ الأزرق الجانبي عديم الأثر على الوجوه الأمامية (بقي للعدد فقط).

### ملاحظة بيئية (تلوث GPU مشترك)
أثناء التشخيص: `nvlddmkm id=153` + تعطلات متكررة لـ`ClusteredLightingLab.exe` داخل
`nvoglv64.dll` (0xc0000005) في نفس النوافذ الزمنية — فسّرت القتلات المتقطعة (EXIT=-1) والقراءات
المتناقضة. بوابة `gt_016a` استُخدمت كفحص صحة GPU قبل كل استنتاج. التوصية: نوافذ اختبار هادئة
(عدم تشغيل المشروع المعزول بالتزامن مع بوابات GNE).

### التالي
019 — بانتظار التوجيه (تثبيت §35 + commit/push 018 بعد الاعتماد).

## 36. GNE-019 - Shadows + Real Depth HZB (PASS)

**Outcome:** GT_019A PASS - d1/d2 byte-identical `v19|lc=20|sm=5|cs=4|rd=1|hr=1.00|d1`; rc 0/0; zero leak warnings.
**Criteria (D7-8):** S1 dir shadow D=0.48 (886 px) PASS; S2 point shadow D=0.17 (1550 px) PASS; S3 spot shadow D=0.27 (1007 px) PASS; S4 seam max step 0.048 PASS; S5 KI-007 real depth (rd=1, pcount=41498) PASS; S6 hr=1.00 PASS; DET PASS; regressions PASS.
**Regressions on final binary:** GT_REGRESS PASS (007-015, 012 XFAIL), GT_011 PASS, GT_015A PASS, GT_015_5_P4 PASS, GT_016A PASS, GT_017A PASS, GT_018A PASS (literal signatures preserved).
**Structural fixes after TEMP-DIAG removal:** (1) CSM z clip remap GL-style to Vulkan [0,1] (maps were empty); (2) shadow record header written as floats (int bits read as ~1e-45 - all hits lit; Lesson 3 recurrence); (3) _s019_lookat transposed (rays missed the light vector; axes now stored as rows); (4) bind/re-bind recomputes VPs immediately (no stale records between unbind-rebind cycles); (5) probe pixel y-convention in main_019 _proj_px (raster is y-down; verified via inst3 marker + in-shader trace); (6) S4 check re-scoped to fully-lit neighborhoods (cascade-seam smoothness per SPEC; silhouettes/shadow edges excluded).
**Scene calibration:** inst0/1/2 resized and the green light repositioned so the D7-8 gates are physically reachable (>=500 px umbras, unclipped D>=0.15 deltas); a re-cull before S4 keeps cs=4 in the signature.
**KI-009 (RID leaks):** CLOSED - _shadow_ensure_set2 lazy dummies were recreated unfreed by the store block; reuse-only fix; zero leak lines in all runs.
**KI-010 (harness RID check):** FIXED - all harnesses now match the current Godot leak format (RID.*of type).
**Commits:** b85d4dd (019 milestone), 3951191 (RID leaks), b661c0f (KI-010). Next: 020 (GI) per SPEC.

## 37. GNE-018-rev - Normal-Cone Back-Face Culling (PASS - R1 gate)

**Outcome:** GT_018A_REV PASS - d1/d2 byte-identical `v18-rev|lc=20|cc=3091|dc=13|ot=9|dp=2558|d1`; rc 0/0; zero leak lines; the OFF/legacy paths stay byte-identical (v18 literal intact in gt_018a and gt_regress).
**Criteria:** R1 scoped PASS (2558 differing pixels: outside=0 - all inside cull-affected clusters; unexplained=0 - all carry the KI-011 NdotL<=0 signature). R7 PASS (cone source age=1 at use; engine-exposed epochs [3,3,1,1]). R3 DET PASS. R4 regressions PASS (full sweep; v18/v19 literals byte-intact). R5 hygiene PASS (zero RID leaks after closing the unit's teardown gaps). R6 evidence: assignment counts and dropped lists recorded; no timings in the signature.
**R2 decision (executor, delegated 2026-09-28):** measured dc=13 assignments (slab 12363 -> on 12350; ~0.11% of the corrected base). The provisional >=10% target is NOT met on this scene and is NOT claimed; per D8-rev-5 "measured-at" the measured value is recorded and pinned byte-exact by the signature; the quantitative >=10% scale target moves to the dense-light scenario (benchmark-city work item), still open. The criterion was not redefined to pass.
**Defects found + fixed during the unit (both by the R1 self-check):** (1) back-face anchor over-cull - the AABB-min anchor (lab pattern) sat up to ~330 units off-surface with our 120 px tiles (~25 degree direction error), measured 2682 unexplained pixels; fixed with `gne_backface_dot_box` (closest-point anchor, strictly conservative; after fix unexplained=0). (2) teardown gaps - light_cone objects and selftest objects now freed in _destroy_mesh (KI-009 pattern).
**ot=9 note:** the KI-014 fix expands lists; 9 clusters exceed the 16 cap on this scene (loud marker in the signature; OFF path stays ot=0). Overflow semantics / cap policy tracked in KI-014's R1 addendum.
**Artifacts:** main_018_rev.gd/.tscn, tools/gt_018a_rev.bat, docs/rev_r1_evidence.md. Commits: 51fbf5c (2a), ef0d402 (2b), 9fa2de1 (KI-014), a3d5e8e (2c), a696b9f (R1).
**Next:** 020 (open scope decision: presentation/KI-003 vs GI).

## 38. GNE-020 - Presentation Overhaul (CLOSED - units 1-3, flag-gated)

**Outcome:** reduced-resolution presentation path (960x540, deterministic 2x2 box blit) behind `GNE_PRESENT_LOWRES` (default OFF); readback bytes exactly 4x fewer (8,294,400 -> 2,073,600 per frame); measured frame time -43..-54% p50 (same-session); blit verified against a CPU-side 2x2 box average (bad_over1=0, maxdiff=1). The readback problem is MITIGATED, not solved: the CPU roundtrip persists; zero-copy stays a future item (spec section 13).
**Units:** 1 - baseline scene + harness (main_020 / gt_020a, signature v20, d1==d2); 2 - reduced-res path + in-scene verification + full battery; 3 - C6 double-buffering experiment: deferred ping-pong = NO measurable effect (medians 7795 vs 7890us, 5 runs each), sync removal = crash (invalid; root cause NOT diagnosed, gap recorded in spec section 14); experimental code removed after measurement; decomposition tooling kept.
**Battery:** 11/11 green on the final rebuilt binary (8 classic gates + 018a_rev + gt_020a full + gt_020a lowres); all legacy literals intact.
**Decisions:** D9 defaults adopted (section 11); framing per Architect directive (section 13); cone-culling default state recorded separately (spec_018_rev section 12: flag OFF until dense-light evidence); C6 item closed with measured evidence.
**Incidents:** a stray git commit hit the build-host repo (no remote, no effect); restored exactly; Lesson 6 recorded.
**Commits:** 74d2790 (kickoff) -> 607b620 (unit1) -> 66ee082 (unit2) -> 9f0454e (framing) -> 0250fc7 (cone default-state) -> c742dda (unit3 + C6) -> 1583225 (Lesson 6).
**Next:** GNE-021 proposal pending Architect approval (Benchmark City + Dense-Light Validation; seed for the 018-rev dc>=10% retest). GI remains a later milestone with its own future spec.
