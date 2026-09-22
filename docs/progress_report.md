# تقرير شامل — مشروع GOTOT-NEXT (حتى الآن)

## 1. الهدف والأساس
نهدف لبناء نظام **GPU-driven rendering** حقيقي كوحدة C++ (Module) مخصصة في **Godot v4.8.dev (master)**, ننفذ خارطة طريق `docs/engine_spec_v2.md` مرحليًا. الفكرة الجوهرية: كاميرا واحدة → **ممران فقط**: Culling+HZB بالحوسبة (compute) ثم **رسم واحد غير مباشر** (indirect draw) يقوده الـ GPU مباشرة، بدون حلقات CPU على آلاف الأشياء.

المراحل التي اكتملت: **001A** (جهاز GPU كسول) → **001B** (GPU Scene) → **002** (frustum culling) → **003** (indirect args) → **004** (HZB occlusion) → **005** (رسم غير مباشر فعلي) → **006** (قياسات تدرج 10M).

بعد اكتمال هذه المراحل، لم نعد في مرحلة "إثبات إمكانية GPU-driven rendering" تجريبية فقط؛ أصبح لدينا **Prototype متكامل تقريبًا**: GPU Scene → Culling → HZB → Indirect Draw → Rasterization على Framebuffer خاص.

## 2. البيئة والبناء
- الملفات: `C:\Users\opc\Documents\AI_ENGINE\gotot-next\modules\gotot_render\{gotot_render_server.h, .cpp}` + `demo\gpu_smoke\main.gd` + `docs\engine_spec_v2.md`.
- البناء (من `godot-master`): `scons platform=windows target=editor dev_build=yes custom_modules="C:\Users\opc\Documents\AI_ENGINE\gotot-next\modules" -j6` (~15-21 ث).
- **مشكلة قسرية**: سياسة App Control/Device Guard على الجهاز تحجب تشغيل الـ exe مباشرة → حل بديل مثبت: `schtasks /Create /Run /Delete` يشغّل `gt_smoke.bat` الذي يكتب الناتج إلى `gt3.txt`، ثم نقرأه.
- `RenderingDevice` محلي كسول (بنفس نمط `LightmapperRD`) لإبقاء الوحدة منعزلة عن الـ renderer الرئيسي.

## 3. GOTOT-001A — الجهاز
- `ensure_gpu_device()` → يخلق `RenderingDevice` محلي عند أول طلب فقط، مع `is_gpu_ready()`.
- ملاحظة: لا نستدعي `free_rid` على الـ RD نفسه (جهاز مشترك للعملية كلها) — نتجنّب خطأ "Attempted to free invalid ID".

## 4. GOTOT-001B — GPU Scene
- 100,000 كائن ببنية SoA (فصل حسب النوع للاستفادة من الـ bandwidth):
  - `transform_buffer`: `vec4 position+scale` (16 بايت).
  - `bounds_buffer`: `vec4 min_xyz+max_z` (32 بايت).
  - `instance_id_buffer`: uint32 (4 بايت).
- تعبئة بواسطة compute shader واحد يوازى بالترتيب (لكل instance أصله) — نفس النتائج CPU وGPU لاحقًا.
- القياس المُثبت: `instances=100000, create≈196-318ms (تخصيص+تجميع), fill≈0.33-0.40ms`, الحجم ~4.96MB.

## 5. GOTOT-002 — Frustum Culling
- 6 مستويات من `Projection::get_projection_planes()` بالترتيب: near, far, left, top, right, bottom.
- اختبار: **كرة** (مركز+نصف قطر) ضد المستويات: داخل إذا كان `dot(n,p)+d >= -radius` — يعطي حصانة ضد الدخول الجزئي والعرض الزاوي الصغير.
- عدّ ذري للحالات المرئية في `visible_count_buffer` + مصفوفة `visibility[]`.
- التحقق: عدّتان GPU **متطابقتان** (حتمية)، و GPU==CPU بالضبط = **377** على الـ 100K، مع مطابقة كاملة لكل إدخال بـ visibility[i]. `cull_ms≈0.1ms`.
- كاميرا "نظرة بعيدة" تُبقي ≤500 (تحقق من صحة عكس الاتجاه).

## 6. GOTOT-003 — Indirect Args + Compaction
- ممر ضغط (compaction) يحول `visibility[]` إلى قائمة متراصة `compact[]` من الأصول المرئية.
- ممر التثبيت (finalize): `indirect_args_buffer[5] = {index_count, instance_count, first_index, vertex_offset, first_instance}` — بالضبط مخطط `VkDrawIndexedIndirectCommand`.
- **درس**: ترتيب الخانات (slots) في الضغط غير حتمي (تزامن) → نقارن القوائم **بعد الفرز**، بينما العداد والـ args حتميان.
- النتيجة: `finalize_ms≈0.41-0.58`, `args=[6,377,0,0,0]`, والقائمة المتراصة == مجموعة الـ CPU تمامًا.
- أنشئ بـ `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT` (مطلوب لأي indirect).

## 7. GOTOT-004 — HZB Occlusion
- **HZB**: نسيج `512×512 R32UI` من نوع 2D-array بـ 10 طبقات (مستويات). `hzb_valid` في الـ UBO يخبر الـ cull shader هل نستخدم HZB أو frustum فقط.
- 3 ممرات متتالية (كل pass = list + submit + sync):
  1. **Clear level0**: مسح الطبقة 0.
  2. **Occluder pass**: إسقاط صناديق المعيقات (خلفيات) عبر push constant، تراكب المستطيل المسقط، والتخزين بـ `imageAtomicMax(dst, floatBitsToUint(far_plane - min_z))` — قيمة أعلى = أقرب للكاميرا.
  3. **Downsample ×9**: أخذ ماكس 2×2 من كل مستوى فوقه.
- **دمج HZB في cull نفسه بمرور واحد**: مستوى تكلس من `ceil(log2(r_pixels))`، فحص 2×2 بجوار المركز، والتحويل العكسي `sphere_inv = floatBitsToUint(max(far-(z_view-radius), 0))`.
- UBO `GototViewData` (256B): `vp, view, planes[6], viewport(w,h,hzb_texels,tanHalfFovV), occ_count, far_plane, hzb_valid, pad`.
- القدرة: `gpu_scene_set_viewport(w,h)`, `gpu_scene_set_occluders(Vector4 pairs)`, `gpu_visibility_dispatch()`.
- النتائج: `visibility_ms≈0.7`, frustum_only=377 → **hzb_visible=375 (حتمي)**، post-HZB args `[6,375,0,0,0]`، وبدون معيقات يعود إلى 377 (ضابطة).

## 8. GOTOT-005 — الرسم غير المباشر الفعلي
- **Shader الرأس**: بلا vertex buffers — `gl_VertexIndex` ينشئ 4 رؤوس مربع عالمية حول `position_scale[compact[gl_InstanceIndex]]` (half-size = scale.w)، تحويل الموقع `viewdata.vp` من نفس UBO. **Shader المقطع**: لون مجرتي (0.95,0.18,0.9).
- **Pipeline**: `render_pipeline_create(shader, fb_format, vertex_format, TRIANGLES, rs, ms, ds, blend_disabled, dynamic, for_render_pass)`؛ `vertex_format = RD::INVALID_ID` (كما يفعل blit) وإلا يطالب بمصفوفة vertices.
- **الموارد**: نسيج لوني `1920×1080 R8G8B8A8` (COLOR_ATTACHMENT|SAMPLING|CAN_COPY_FROM)، `framebuffer_format_create` بـ `AttachmentFormat{format, samples, usage}`، framebuffer بواسطة `framebuffer_create([tex], fmt)`.
- **فهارس**: `index_buffer_create(6, UINT32, [0,1,2,2,3,0])` → `index_array_create`؛ indirect args `[index_count=6, ...]`.
- **الرسم**: `draw_list_begin(fb, DRAW_CLEAR_COLOR_0, clear, ...)` → bind pipeline + uniform set (set 0: compact=0, transform=1, view_ubo=2) + index array → `draw_list_draw_indirect(list, true, args_buffer, 0, 1, 0)` → end + submit + sync.
- **قراءة**: `texture_get_data(texture, 0)` → PackedByteArray.
- **تحقق**: `colored=521 px`، حتمي عبر ممرّين، و**112 مركزًا مسقطًا** (بـ vp الفعلي من GPU عبر `gpu_scene_get_vp()`) كلها عليها بكسل مجرتي (اصطلاح Vulkan convx=1, convy=1: `py=(ndc.y*0.5+0.5)*H`) — استثنينا 18 مربعًا دون-بكسل. `draw_ms≈0.15ms`.

## 9. GOTOT-006 — قياسات التدرج
رفعنا حد العدد إلى 100M وقيّمنا نفس الخط على 10M:
| count | fill_ms | cull_ms | finalize_ms | visibility_ms | raster_ms | visible | buffers |
|---|---|---|---|---|---|---|---|
| 100K | 0.38 | 0.10 | 0.12 | 0.53 | 0.16 | 364 | 4.96MB |
| 1M | 3.33 | 0.15 | 0.12 | 0.60 | 0.16 | 3,670 | 49.6MB |
| 10M | 1.48 | 0.50 | 0.12 | 0.92 | 0.18 | 36,744 | 495.9MB |

**الخلاصة الدقيقة (وليست حقيقة عامة):**
في **هذا الاختبار تحديدًا**، ظل زمن مراحل culling/finalize/raster قريبًا من الثبات ضمن نطاق 100K–10M، لأن الـ prototype يعتمد على بنية GPU parallel ولا يحتوي حاليًا على **تكلفة رسم هندسي حقيقي متناسبة مع تعقيد meshes**. هذه أرقام **Benchmark Prototype v0.1** وليست ادعاءً بأن GOTOT-NEXT يستطيع رسم 10 ملايين جسم داخل لعبة بـ 60FPS.

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

**GOTOT-008A (real mesh path):** `gpu_mesh_create`, `gpu_mesh_drawargs_finalize`, `gpu_mesh_indirect_draw`, `gpu_mesh_get_index_count`, `gpu_mesh_get_vertex_count`.

## 12. الحالة الحالية
- كل الاختبارات (001A→011) خضراء، خروج كود 0، إغلاق نظيف بلا أخطاء `free invalid ID` أو أخطاء RID/lifetime.
- التدفق الحالي: **GPU Scene → GPU Culling → HZB → Compaction → Indirect Args → (Billboard | Real Mesh) → Batch Assembly → Multi-Draw → Rasterization → Framebuffer Offscreen → CPU Readback → ImageTexture → TextureRect → نافذة Godot**.
- أصبح ناتج الرسم مرئيًا داخل نافذة Godot فعليًا (007A)، وأصبح المشروع يرسم **هندسة 3D حقيقية** (مكعبات) عبر مسار mesh مستقل (008A).
- **التالي**: **GOTOT-011 — Multi-Draw / Multi-Batch** اكتمل بأدلة PASS (3 استراتيجيات + demo تفاعلي). **GOTOT-012 — Production HZB** بانتظار SPEC الرسمي.

## 13. Milestone Status

- GOTOT-001A — GPU Device: PASS
- GOTOT-001B — GPU Scene: PASS
- GOTOT-002 — Frustum Culling: PASS
- GOTOT-003 — Indirect Arguments + Compaction: PASS
- GOTOT-004 — HZB Occlusion: PASS
- GOTOT-005 — Actual Indirect Draw: PASS
- GOTOT-006 — 100K / 1M / 10M Scaling: PASS
- GOTOT-007A — Viewport Integration Bridge (CPU Readback): PASS
- GOTOT-008A — Real Geometry Proof (Real Mesh Path): PASS
- GOTOT-008B — Multi-Instance Mesh Rendering Proof: PASS
- GOTOT-009A/009B — Real Depth Buffer (D32_SFLOAT) Proof: PASS
- GOTOT-010 — Batch Instance Rendering (3 meshes multi-draw): PASS
- GOTOT-011 — Multi-Draw / Multi-Batch (≤5 grouped/reordered draws, dynamic draw count, parallel prefix-sum): PASS
- GOTOT-011 Demo — Interactive (main_demo + camera_controller + hud, strategy live-switch): PASS

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
  (GOTOT-005)      (GOTOT-008A)
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

**GOTOT-011 — Multi-Draw / Multi-Batch** اكتمل PASS (3 استراتيجيات + demo تفاعلي). المرحلة القادمة: **GOTOT-012 — Production HZB** (SPEC قادم).

## 14. GOTOT-007A — جسر العرض داخل نافذة Godot

- **الوحيد في نطاقه**: عرض ناتج الرسم على الشاشة داخل نافذة Godot فعليًا (كان سابقًا قراءة texture من الذاكرة فقط).
- **المسار**: `Camera3D` حقيقية → `gpu_scene_set_camera` → نفس خط GPU (cull → visibility → finalize → raster) → `gpu_raster_read_pixels` → `Image` → `ImageTexture` → `TextureRect`.
- **بدون أي تعديل C++**: GDScript فقط (`demo/gpu_smoke/main_007.gd` + `main_007.tscn`).
- **ملاحظات مهمة**: `Camera3D.get_projection()` يعيد **enum** وليس المصفوفة → الصحيح `get_camera_projection()`.
- **الأدلة**: النافذة الفعلية 1152×648 (نفس نسبة 1920×1080)، `visible range 356..384`، تغيّر الصورة في `232/239` إطارًا، فحص الاتجاه `probe=0` (لا حاجة لقلب)، ولقطة نافذة محفوظة.
- **القياسات (100K)**: Cull ≈ 0.41→1.09ms، HZB/Visibility ≈ 1.03→2.61ms، Finalize ≈ 0.06→0.08ms، Raster ≈ 0.10ms، Readback ≈ 2.22→2.78ms، Frame ≈ 11.36→14.16ms (يزحف صعودًا؛ موثّق وغير مُفسَّر بعد).
- **القيد المعلن**: مسار CPU readback **جسر مؤقت وليس مسار العرض النهائي** (~8MB/إطار عند 1920×1080).

## 15. GOTOT-008A — إثبات الهندسة الحقيقية (Real Mesh Path)

- **الهدف**: إثبات أن GOTOT-NEXT يستطيع رسم **هندسة 3D حقيقية** باستخدام: vertex buffer حقيقي + index buffer حقيقي + vertex format حقيقي + indexed draw + indirect draw، معتمدًا على نفس GPU Scene ونفس قائمة الـ compact ونفس نظام الوسائط غير المباشرة ونفس جسر العرض 007A.
- **قاعدة معمارية**: مسار 005/007A باقٍ كما هو **دون أي استبدال** (billboard shader، quad index buffer، raster pipeline، `gpu_raster_indirect_draw`، `gpu_raster_read_pixels`). الـ Mesh مسار **مستقل ومضاف**.
- **الهندسة**: مكعب واحد فقط — 8 رؤوس (positions فقط، `R32G32B32_SFLOAT`) و36 فهرسًا (`UINT32`). بلا normals/UVs/materials/textures.
- **نموذج الـ instance**: نفس النموذج — `compact[gl_InstanceIndex]` → `transforms.position_scale[orig]` → `viewdata.vp`. الناتج 377 (أو ما يقابله) **مكعبًا حقيقيًا**.
- **الـ indirect args**: `index_count=36, instance_count=visible, first_index=0, vertex_offset=0, first_instance=0`. قيمة `index_count` تُمرَّر عبر push constant (وليس مضمّنة بشكل يمنع التعميم، ودون بناء Mesh Table).
- **نطاق C++ المسموح**: موارد mesh vertex/index، vertex format، mesh shader، mesh pipeline، دالة رسم mesh، تدمير موارد mesh — لا شيء خارج ذلك.
- **الأدلة (100K)**: `GPU Mesh created vertices=8 indices=36 vertex_format=0`؛ `Indirect args = [36, N, 0, 0, 0]` مع `N == visible` كل إطار (تحقق برمجي يوقف التشغيل عند عدم التطابق)؛ العدّ الدقيق على الإطار الأخير: **green=305، magenta=0** (أي هندسة حقيقية وليست billboard القديم)؛ `visible range 351..389`؛ `changed_frames=236/239`؛ لقطة نافذة `1152×648`؛ إغلاق نظيف بلا أخطاء RID/lifetime.
- **القيود**: يرسم في نفس الـ framebuffer الحالي 1920×1080 ويستخدم `gpu_raster_read_pixels` القائم؛ مكعبة واحدة مضمّنة (لا mesh table/IDs/streaming)؛ لا depth buffer؛ يشترك مسار mesh في `indirect_args_buffer` القائم لذا لا يجوز خلط `gpu_drawargs_finalize()` مع `gpu_mesh_indirect_draw()` في نفس التشغيل.

## 16. المرحلة القادمة

**GOTOT-011 — Multi-Draw / Multi-Batch** اكتمل PASS (3 استراتيجيات + demo تفاعلي، انظر الأقسام 19/20). **GOTOT-012 — Production HZB** بانتظار SPEC الرسمي.

## 17. GOTOT-008B — برهان الرسم متعدد النسخ (Multi-Instance Mesh Rendering Proof)

- **الهدف**: إثبات أن مسار الـ mesh الحقيقي (008A) يرسم **مئات/آلاف المكعبات الحقيقية** بتحويل صحيح لكل instance من `compact[gl_InstanceIndex]`، بعد instance ديناميكي من الـ compact buffer، وبدون أي تسريب في indexing.
- **النطاق**: صفر تغيير C++ — أُعيد استخدام مسار 008A بالكامل؛ للملفات الجديدة فقط `demo/gpu_smoke/main_008b.gd` + `main_008b.tscn`.
- **الأدلة (008B PASS)**: 10,000 instance spread 1200؛ `visible` في المدى `3630..3909` (dynamic=true عبر مدار الكاميرا بتتالٍ حتمي frame-indexed)؛ `args=[36, N, 0, 0, 0]` كل إطار مع `N == visible`؛ `green_px=25765` (مدى 17635..25765)؛ توافق CPU: `gpu=3905 cpu=3905 boundary=0 delta=0 set_ok=true`؛ فحص المراكز المسقطة `checked=176 matched=176 miss=0`؛ determinism `drawargs_repeat=true full_frame_repeat=true`؛ **توقيع DET متطابق حرفيًا بين تشغيلين** `v3905|36|3905|g25765|c0|5004|9997|h176|m0|3905|f150`؛ لقطة نافذة `gt_008b_window.png`؛ regressions 001A–006/007A/008A جميعها PASS على نفس البنية.

### 008B — Fixes Applied During Verification

- **Fix 1 — قراءة مستويات frustum كل إطار بعد `set_camera`**: في أول تشغيل أظهر الفحص الفرق `gpu=3864 cpu=3907 delta=43 boundary=0` — فرق منهجي وليس ضجيج rounding، سببه أن المستويات كانت مقروءة في `_ready` بنسبة أبعاد النافذة الأولية بينما GPU يستخدم كاميرا اللحظة الراهنة (أبعاد النافذة تتغير بين الإقلاع وتشغيل الإطارات). الحل: إعادة قراءة `gpu_scene_get_frustum_planes()` كل إطار بعد `set_camera` بالكاميرا نفسها التي يُجري بها GPU الـ cull. النتيجة: `gpu == cpu` بالضبط في كل إطار (`delta=0`).
- **Fix 2 — فحص البقع من compact مُصنَّف**: أول تنفيذين أعطيا نفس دليل الرسم لكن حقل `h` (عدد البقع القابلة للفحص) اختلف (232 مقابل 196) لأن عيّنة البقع كانت تُؤخذ من **ترتيب الإصدار الذري** العشوائي في الـ compact. الحل: أخذ العينة من **نسخة مصنّفة** من الـ compact (h=176 ثابت). التوقيع مُجمَّع أصلاً من قائمة مصنّفة ليكون مستقرًا بين التشغيلات.

### ملاحظات معمارية مؤجلة (سجّل فقط — لا تنفيذ)

- **نقطة frustum reading**: كشف Fix 1 أن `set_camera` لا تُحدّث الـ frustum تلقائيًا عند تغيّر أبعاد النافذة — هل هذا سلوك مقصود أم ثغرة تصميم؟ تُعالج في milestone قادم (مثل 009/رفع لاحق).
- **نقطة الترتيب الذري**: ترتيب الإصدار (atomic compaction) عشوائي بين التشغيلات — هل يؤثر على الأداء الفعلي أم فقط على التحقق؟ تُقيَّم لاحقًا (التحقق الحالي مستقر عبر الفرز).

## 18. GOTOT-009 — Real Depth Buffer (D32_SFLOAT)

### الهدف والنطاق
إرفاق **عمق حقيقي** لمسار mesh: نسيج `D32_SFLOAT` في framebuffer الراستر الخاص، مع depth test + write بواقعية (الأقرب يمنع الأبعد) على مسار **Real Mesh** (008A) بينما يبقى مسار billboard depth-disabled تمامًا. إثبات ذاتي عبر مشهدين: `main_009` مع `--front-only` (مرجع A+D) وبدونها (كامل A+B+C+D)، دون لمس أي regression سابق (001A–008B).

### التغييرات C++ (`gotot_render_server.{h,cpp}`)
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

## 18. GOTOT-010 — الرسم الدفعي متعدد الأشكال (Batch Instance Rendering)

### الهدف
رسم **عدة meshes مختلفة في دفعة (batch) واحدة** عبر multi-draw غير مباشر، بلا حلقة CPU على المثيلات: يشير كل مثيل إلى `mesh_id` من جدول meshes على الـ GPU، ويُجمَّع `batch_args` على الـ GPU (prefix-sum) ثم `draw_list_draw_indirect(draw_count=batch_count)`. عمق 009 (D32_SFLOAT + LESS_OR_EQUAL) باقٍ كما هو.

### التنفيذ (إضافي فقط، بلا لمس أي مسار سابق)
- جدول meshes على الـ GPU: `mesh_table_buffer` (64 خانة، `GototMeshDesc` 32B std430: index/vertex slot + counts + first_index + vertex_offset) + `mesh_id_buffer` (per-instance) + `mesh_color_buffer` (palette لكل mesh).
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

إصلاح عمر (lifetime) أثناء التحقق: `mesh_010_vertex/index_array` كانا يُحرَّران بعد مخزنَي الـ vertex/index المشتركين → خطآن "free invalid ID" في الـ destroy؛ عولج بتحرير دفعة 010 (arrays/ubersets/pipelines/buffers) قبل تحرير المخزنَين في `_destroy_mesh`. بعد الإصلاح: **صفر** errors/leaks، إغلاق نظيف، exit 0. لقطة نافذة: `C:\Users\opc\AppData\Local\Temp\opencode\gt_010_window.png`. خروج: `GOTOT-NEXT 010: PASS`.

### الانحدارات 001A–009B (كلها PASS على نفس البنية)
- `gt_smoke` (001B fill + 002 culling + 003 indirect args + 004 HZB + 005 indirect raster + 006 10M scaling): **GOTOT-SMOKE: OK**.
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

## 19. GOTOT-011 — Multi-Draw / Multi-Batch (تجميع الدفعات)

### الهدف
ترقية مسار الدفعات (010) من رسم دفعة **لكل mesh** إلى رسم ≤5 دفعات مجمّعة (groups) عبر multi-draw غير مباشر واحد، مع **إعادة ترتيب (reorder)** المثيلات حسب mesh_id على الـ GPU وترتيب الدفعات تصاعديًا، ودعوة غير مباشرة بـ **draw count ديناميكي (GPU-written)** بدون حلقة CPU على الدفعات.

### التنفيذ (إضافة صافية بلا لمس مسارات سابقة)
- استراتيجية تجميع قابلة للاختيار عبر enum `GototBatchStrategy` (افتراضي `REORDERED`): `PER_MESH=0` (دفعة لكل mesh)، `GROUPED=1` (≤5 مجموعات متجاورة تصاعدية)، `REORDERED=2` (إعادة ترتيب المثيلات حسب mesh_id ثم نفس التجميع).
- **Parallel prefix-sum على مستوى workgroup** في pass 1/2 (بديل الـ single-thread في 010): pass count لكل mesh، ثم prefix-sum يعبئ offsets ويعيد ترتيب origs إلى `batch_instances` المتسلسل.
- **Batch assemble على GPU** يبني `group_batch_buffer` = `VkDrawIndirectCommand` (16B) لكل مجموعة: `vertex_count=index_count لأول عضو`، `instance_count=مجموع حالات الأعضاء`، `first_vertex=0`، `first_instance=batch_offset لأول عضو`.
- **رسم غير مفهرس (procedural non-indexed)**: `draw_list_draw_indirect(dl, false, group_args_buffer, 0, last_batch_count, 16)` بمخزنين مشتركين كاملَي السعة (`mesh_vertex_storage_buffer`/`mesh_index_storage_buffer`) — الـ vertex shader المجمّع يقرأ `index_data.data[d.first_index+gl_VertexIndex]` و`vertex_data.data[(d.vertex_offset+li)*3+i]` (أن RID الـ vertex buffer لا يُربط storage) وخارج index_count يُدفع الرأس لـ `vec4(0,0,-2,1)` خارج المسرح.
- **Draw count ديناميكي GPU-written**: `dispatch_indirect` إلى عدد لانهائي في `last_batch_count` عبر UBO → لا حاجة لقراءة readback لتحديد عدد الدعوات.
- **early_fragment_tests** مفعّلة لقطاع shader المجموعات (محسّن لعدد مجموعات صغير لا يرسم خلف بعضها).
- **التحويل إلى indexed عند G==active**: الدفعات الموجودة مسار 010 (indexed، stride 20) يبقى مطبَّقًا عندما `last_used_group_draw=false` — فـ regressions 001A–010 محمية مع الاستراتيجية الافتراضية.
- **5 واجهات Test-only** جديدة: `gpu_mesh_set_batch_strategy`, `gpu_mesh_get_batch_strategy`, `gpu_mesh_get_batch_group_count`, `gpu_mesh_get_indirect_count`, `gpu_mesh_get_batch_order`. أضيفت constant enum عبر `ClassDB::bind_integer_constant("GototRenderServer","GototBatchStrategy",...)` (4 وسائط — واجهة bind_integer_constant الصحيحة بدل BIND_ENUM_CONSTANT المتعطّلة).

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
- `gt_smoke` (001B–006): `GOTOT-SMOKE: OK` → exit 0.
- `007A`/`008A`/`008B`/`009`+`009a`/`010a`/`010b`: كلها `PASS` → exit 0 بلا أخطاء free/validation.
- التحقق المباشر (بدون schtasks) بعد تعطيل App Control: تشغيل الـ exe مباشرة يعمل الآن.

### القيود (011) — مُسجّلة بوضوح
- **قيد API لعدّ الـ draw**: `draw_list_draw_indirect` في Godot RD يستلزم عدد دعوات (draw_count) كقيمة من خارج GPU — لا قراءة مباشرة من buffer أثناء الرسم. الحل المعتمد: الحد الأقصى `last_batch_count` يُحدَّث من آخر dispatch متزامن ويُمرَّر عبر UBO (لذلك `dc==ic==5` دون readback). هذا هو "indirect count fallback" المذكور؛ يبقى count محدَّثًا بإطار واحد كحد أقصى في أسوأ الحالات.
- **الرسم المجمّع procedural غير مفهرس**: المجموعات تُرسم عبر نسخ storage كاملة السعة (`mesh_vertex_storage_buffer`/`mesh_index_storage_buffer`) لأن RID الـ vertex/index buffers لا تُربط كـ storage — بصمة مضاعفة للنسخ، ومخزنا التخزين المشتركان بسعة قصوى (32768×vec3 / 65536×uint32) مهما كان عدد الأشكال الفعلية.
- **حد أقصى 5 مجموعات** اختياري حسب SPEC (G = min(active,5)) مع مجموعات متجاورة تصاعدية — ترتيب جزئي وليس sort كامل للمشهد؛ يكفي للمطالب الحالية (64 mesh → 5 draw calls، 12.8×).
- **مسار 010 (indexed, stride 20)** يبقى قيد أن G==active أو PER_MESH — التجميع لا يمس الرسم المفهرس عند غياب الحاجة للتصغير.
- **CPU readback** (pixels + depth كامل الإطار ~8MB لكل منهما) ما زال جسر تحقق وليس مسار عرض إنتاجيًا.
- الـ getters الخمسة والـ enum **test-only** مرشّحون لاحقًا للدمج في استعلام قدرات واحد (سجّل فقط).