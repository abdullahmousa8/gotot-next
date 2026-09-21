# GOTOT-010 — Batch Instance Rendering (SPEC DRAFT)

**الحالة:** SPEC 010 v0.2 FINAL — القرارات 7.1/7.2 محسومة — بانتظار push وموافقة Owner على بدء 010.
**المرجع:** docs/progress_report.md (قسم 12/13/16) + ملاحظات التأجيل المؤجلة إلى 010.
**المرحلة السابقة:** GOTOT-009 — Real Depth Buffer (PASS، commit `495bad7`).

---

## 1. الهدف

رسم **عدة meshes مختلفة في دفعة واحدة** دون أي حلقة CPU فوق المثيلات، مع بقاء ترتيب العمق (من 009) صحيحاً. بدلاً من "كل المثيلات نفس المكعب" (008A/008B)، يجب أن يشير كل مثيل إلى **mesh_id** من جدول meshes على الـ GPU، وتُنجز الرسمة عبر **multi-draw / indirect draw per-batch**.

> المسار المطلوب: GPU Scene (مع per-instance mesh_id) → culling → compaction → **multi-batch indirect draw** → rasterization بدقة العمق (D32_SFLOAT).

## 2. الخلفية والقيود الموروثة

- 008A: رسم real geometry لمكعب واحد، draw واحد `args=[36, N, 0, 0, 0]`.
- 008B: 10,000 مثيل بنفس المكعب، determinism كامل (drawargs + full-frame repeat).
- 009: D32_SFLOAT مع depth test/write (LESS_OR_EQUAL، clear=1.0) — العمق موجود الآن لكل المثيلات.
- القيد القديم المعروف: مسار الـ mesh يعيد استخدام مخزن indirect argument نفسه؛ ولهذا يجب ألا يُداخل مسارا الـ billboard والـ mesh في نفس التشغيل (009 بقي على هذا).

## 3. التصميم المقترح (مفتوح للتحسين في المراجعة)

### 3.1 Mesh Table Schema

```cpp
struct GototMeshDesc {
    uint32_t index_buffer_slot;    // slot في bindless table (مستقبلاً) أو RID
    uint32_t vertex_buffer_slot;   // slot
    uint32_t index_count;          // عدد الفهارس
    uint32_t vertex_count;         // عدد الرؤوس
    uint32_t first_index;          // offset في index buffer
    int32_t  vertex_offset;        // offset في vertex buffer
    uint32_t reserved[2];          // padding
};
```

- **Builder:** CPU (GOTOT-owned) → GPU buffer عبر `buffer_update`.
- **Size:** ثابت ابتداءً (مثل 64 mesh كحد أقصى في 010A).
- **Extension:** ديناميكي لاحقاً (011+).

### 3.2 Batch Assembly (Compute Pass)

بعد compaction، يعمل compute pass جديد:

**Input:**
- `compact[]` (visible instance origin IDs)
- `mesh_id[]` (per-instance mesh ID)
- `mesh_table[]` (per-mesh descriptors)

**Output:**
- `batch_args[]` (per-batch `VkDrawIndexedIndirectCommand`)
- `batch_count` (atomic counter)

**Logic:**
```
for each visible instance i:
    mesh = mesh_id[compact[i]]
    atomicAdd(batch_count[mesh], 1)
    append instance to batch[mesh] list
for each mesh with count > 0:
    batch_args[mesh] = {
        index_count     = mesh_table[mesh].index_count,
        instance_count  = batch_count[mesh],
        first_index     = mesh_table[mesh].first_index,
        vertex_offset   = mesh_table[mesh].vertex_offset,
        first_instance  = batch_offset[mesh]
    }
```

- **Batch offset:** يُحسب بـ **prefix sum** على `batch_count`.
- **Instance list:** `batch_instances[]` (concatenated compact reordered).

### 3.3 Multi-draw per-batch

بعد الـ compaction، تُرسل **رسمة غير مباشرة لكل mesh_id له مرئيون** (مثل `vkCmdDrawIndexedIndirect` بمصفوفة args per batch، أو `MultiDrawIndirect` إن توفر) — عدد الدفعات مرتبط بعدد الـ meshes الفريدة، **ليس** بعدد المثيلات المرئية.

- البديل المعياري عند الحاجة: `DrawIndexedIndirectCount` بتعداد تحسبه compute pass.

### 3.4 العمق

تستمر ربطات التصفية والكتابة من 009؛ اختبارات 010 تضمن أن ترتيب العمق يبقى سليماً بين meshes مختلفة (وليس فقط بين مثيلات نفس المكعب).

### 3.5 التمييز البصري للأدلة

تمرير لون per-instance (أو per-mesh) لتحديد "أي هندسة رُسمت" عبر فحص البكسلات (مثل نمط `green==fg` في 009 لكن مع مركّز لكل mesh).

## 4. معايير القبول (PASS — أدلة موضوعية قابلة للقياس)

1. **Mix meshes**: مشهد يضم مكعباً + على الأقل هندستين إضافيتين مختلفتين، وجميعها مرئية في كاميرا واحدة في نفس الرسمة.
2. **كل mesh يُرسم هندسته الصحيحة**: بحث بكسلي لكل لون mesh يحقق `count_mesh_i > 0` لكل i ضمن المساحة المتوقعة (بنفس أسلوب `green==fg` 1:1 لـ 009).
3. **عمق صحيح عبر meshes**: `d(front_mesh) < d(back_mesh)` مع TOL المعتاد (DEPTH_ORDER_TOL=0.002)، و155> dmin ضمن حدود؛ وrim المتوقع > 0.
4. **لا حلقة billboard**: عدد الدفعات == عدد الـ meshes الفريدة المرئية (وليس عدد المثيلات) — يُثبت بطباعة count عبر harvesting وarges.
5. **Determinism**: DET signature ثابتة عبر تشغيلين متتاليين (نمط `sig=...` كالمراحل السابقة) — الصيغة النهائية تُحدد عند التنفيذ.
6. **Regressions**: gt_smoke (001A-006) / 007 / 008 / 008b / 009a / 009b — كلها `exit 0`.
7. **GPU==CPU**: عند الحاجة، مقارنة compute culling بالمرجع على الـ CPU بنفس أسلوب 002/008B.
8. **Billboard path لا ينكسر:** 005 / 007A regressions PASS.
   - السبب: 010 يغيّر `indirect_args_buffer` → قد يؤثر على billboard.
   - يجب تشغيل gt_005 (إن وُجد) أو gt_007 + gt_smoke.

## 5. تغييرات الواجهات المقترحة (Test-only في البداية)

| API | النوع | الغرض |
|---|---|---|
| `gpu_scene_set_instance_mesh(index, mesh_id)` | Test-only | تعيين mesh لكل مثيل (+ قراءة getter للتحقق) |
| `gpu_mesh_create_from_arrays(index_arrays, vertex_arrays)` | Test-only | بناء mesh إضافي من Arrays (اختياري إن رُجي توسيع جدول الـ meshes) |
| `gpu_mesh_get_batch_count()` / `gpu_mesh_get_draw_counts()` | Test-only | الأدلة على multi-draw per-batch |
| `gpu_mesh_get_mesh_id_count()` | Test-only | عدد meshes في الجدول |
| `gpu_mesh_get_batch_args(batch_index)` | Test-only | قراءة args دفعة |
| `gpu_mesh_get_mesh_color(mesh_id)` | Test-only | لون mesh للتحقق البصري |
| Getters دمج/توحيد ملاحظة: 009 كان فيه 3 getters مرشحة للدمج — يُنظر في توحيدها هنا | — | Refactor خفيف داخل النطاق |

ملاحظة: أي API إنتاجي مستقبلاً يمر بمراجعة الاستراتيجية (Sections 19–20) قبل اعتماده.

## 6. نطاق خارجي (Out of scope لـ 010)

Meshlets / LOD، نظام المواد، render graph، bindless/VMA، streamed geometries، reversed-Z (مؤجل). لا تغيير في مسار الـ billboard القائم، ولا دمج لـ RenderingServer/RHI.

## 7. ملاحظات مؤجلة — حسم إلزامي في 010

### 7.1 Frustum reading — RESOLVED

**القرار:** الخيار B — إصلاح `set_camera` ليُحدّث frustum تلقائياً.

**السبب:**
- Fix 1 في 008B كان حلًا مؤقتًا (قراءة per-frame).
- الحل الصحيح: `set_camera` يُحدّث frustum داخليًا.
- الفائدة: يزيل الحاجة لقراءة per-frame، ويجعل API أكثر أمانًا.

**التنفيذ:**
- `gpu_scene_set_camera` يستدعي `Projection::get_projection_planes()` داخليًا.
- يُحدّث `GototViewData.planes[6]` في UBO.
- يُلغي الحاجة إلى قراءة per-frame في demo.

**النطاق:** C++ صغير (تحديث UBO في `set_camera`).
**Regression:** يجب أن يبقى 008B PASS بعد التغيير.

### 7.2 Atomic ordering — RESOLVED

**القرار:** batch assembly يستخدم prefix sum — الترتيب غير مهم.

**السبب:**
- `batch_args[mesh].first_instance` يأتي من `batch_offset[mesh]` (prefix sum).
- `batch_instances[]` قائمة مُجمّعة (concatenated) — ترتيبها الداخلي **لا يؤثر**.
- الترتيب الحالي لـ compact **يُحترم** لكن **ليس مطلوبًا**.

**التنفيذ:**
- compute pass جديد: `build_batch_args()`.
- Input: `compact[]`, `mesh_id[]`, `mesh_table[]`.
- Output: `batch_args[]`, `batch_count`, `batch_instances[]`.
- prefix sum على `batch_count[]` → `batch_offset[]`.

**النتيجة:** الترتيب العشوائي في compact **مقبول** — لا يحتاج sort pass.

**Regression:** 008B/009 DET signatures تبقى مستقرة.

### 7.3 Reversed-Z (يُعلَّم كـ deferred)

**القرار:** يبقى مؤجلاً — لا يُعالج في 010.

### 7.4 007A frame-time drift (يُراقَب)

**القرار:** يُراقَب في regressions — لا يُعالج في 010.

## 8. التسليمات

- C++: جدول meshes + per-instance mesh_id + مسار multi-batch + getters test-only.
- Demo: `demo/gpu_smoke/main_010.gd` + `main_010.tscn`.
- Harness: `gt_010a` / `gt_010b` (مثل نمط 009a/009b) + مخرجات `sig` + لقطة `gt_010_window.png` + bin العمق إن لزم.
- توثيق: قسم 19 في `docs/progress_report.md` + تحديث README (Milestones 010 + Roadmap ⏳→✅) بعد PASS.
- Sweep regressions: 001A-009B بالكامل.
- **`docs/sac_unblock_procedure.md`** — لا يُلمس.
- **`docs/open_source_system_strategy_v1.md`** — لا يُلمس.
- **`docs/dependency_register.md`** — لا يُلمس.

## 9. الحالة

**DRAFT v0.2 — القرارات 7.1/7.2 محسومة. جاهز للـ commit.**

لا تنفيذ، لا push قبل موافقة Owner النهائية على بدء 010.