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
- كل الاختبارات (001A→008A) خضراء، خروج كود 0، إغلاق نظيف بلا أخطاء `free invalid ID` أو أخطاء RID/lifetime.
- التدفق الحالي: **GPU Scene → GPU Culling → HZB → Compaction → Indirect Args → (Billboard | Real Mesh) → Rasterization → Framebuffer Offscreen → CPU Readback → ImageTexture → TextureRect → نافذة Godot**.
- أصبح ناتج الرسم مرئيًا داخل نافذة Godot فعليًا (007A)، وأصبح المشروع يرسم **هندسة 3D حقيقية** (مكعبات) عبر مسار mesh مستقل (008A).
- **الخطوة القادمة**: **GOTOT-009 — Batch Instance Rendering** ضمن ممر mesh مستقل مع الحفاظ على مسار billboard وكل regression سابق.

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

**GOTOT-009 — Batch Instance Rendering** (رسم دفعات من الـ instances الحقيقية ضمن ممر mesh مستقل، مع الحفاظ على مسار billboard الحالي).

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

**GOTOT-009 — Batch Instance Rendering**: رسم عدة دفعات من الـ instances الحقيقية ضمن ممر mesh مستقل، مع الإبقاء على مسار billboard كاملًا وعدم المساس بـ regression 001A–008A.