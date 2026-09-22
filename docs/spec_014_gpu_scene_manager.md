# GOTOT-014 — GPU Scene Manager (SPEC DRAFT)

**الحالة:** SPEC 014 v0.1 DRAFT — بانتظار مراجعة المعماري وتثبيت النهائي.
**المرجع:** docs/progress_report.md (قسم 24 — 013) + docs/spec_012_production_hzb.md (نمط صياغة SPEC) + modules/gotot_render/gotot_render_server.* (مبنى 013).
**المرحلة السابقة:** GOTOT-013 — Meshlets + LOD + Cluster Culling (FINAL PASS، commit `7958742` على GitHub).
**قرار المالك:** 2026-09-23 — SPICE 014 = **A: GPU Scene Manager** (توصية Architect).

---

## 1. الهدف

نقل **إدارة المشهد** من CPU إلى GPU بالكامل: مصفوفات مثيلات (instance data) ساكنة في GPU (transforms، mesh/meshlet مراجع، إعداد LOD، حدود/bounds)، مسار تحديث GPU-driven بدون تدخل CPU في المسار الحرج، وقدرة استعلام/تجميع على GPU تدعم **ملايين المثيلات**. الناتج يغذّي خط 013 (meshlets + LOD) مباشرة ويُعِدّ البنية لـ **Render Graph (015)**.

> المسار الفعلي بعد 014: GPU Scene (SSBO) → GPU cull/dispatch (مبنى 013) → Batch Assembly (011 pattern) → Software Rasterizer (013) → Evidence/DET.

## 2. الخلفية والقيود الموروثة

- **013 (المبنى الحالي):** بيانات المشهد (meshlets، مراجع LOD لكل مثيل، transforms) تُحمَّل من الـ demo عبر API وعبرها تُبنى خُزن بيانات GPU. مسار 3-pass سوف-راستر مع coherent 64-bit winner يعمل ويثبت الإخراج (covered==winner==76,685؛ per-LOD px `[5853,1265,559566]`).
- **011 (نمط التجميع):** `gpu_mesh_batch_dispatch` يستهلك `visibility[]` ويُنتج ≤5 draw calls — التجميع على المجموعة النهائية بعد أي culling.
- **القيود الحالية:** مرور بيانات كل مثيل يمر عبر CPU upload في إعداد المشهد؛ لا يوجد جدول مشاهد GPU موحد (scene DB)؛ السعة الحالية محدودة بحجم الـ upload لا بتصميم إدارة GPU.
- **012 (HZB):** مؤجّل (fix إسقاط مطلوب) — **خارج نطاق 014**؛ تسلسل HZB يُعاد النظر فيه لاحقًا (لا يعتمد عليه 014).
- قيد حماية الـ regressions: أي مسار لا يلمس signatures 013 (لا-مشهد-خارجي) يبقى حرفيًا ثابتًا («signatures literal»).

## 3. التصميم المقترح (مفتوح للتحسين في المراجعة)

### 3.1 تخزين المشهد على GPU (Scene DB)

- **SSBO الموحّدة** لمصفوفات المشهد في مساحة ID موحّدة (مثيلات إلى mesh/meshlet ranges وإعداد LOD). المثيل يمثلها سجل ثابت الحجم (أي: transform مختصر، refs، bounds، flags).
- نموذج **sructure-of-arrays (SoA)** للصفات الساخنة (transform / LOD / bounds) لتغذية الـ cull و dispatch في 013 بكفاءة.
- قدرة مستهدفة ≥ **1M مثيل** (تختبرها أداة تخصيص توثق السعة الفعلية للجهاز).

### 3.2 مسار التحديث GPU-driven

- **Upload queue + dirty marks**: التعديلات تُصفّ في حلقات CPU خارج المسار الحرج؛ والـ GPU يطبّقها (copy/compaction compute pass) دون توقف.
- لا readback GPU→CPU في المسار الحرج (نفس قاعدة 007A/009/013 — القراءة للتحقق فقط).
- إعادة استخدام/إعادة allocation تدريجية (لتجنب إعادة بناء كاملة لكل إطار).

### 3.3 التكامل مع 013 (meshlets + LOD)

- جدول المشهد يزوّد مسار 013: مراجع meshlet range لكل مثيل + LOD config (نفس أرقام 013 عبر `state.data[10+ic]` نمط) بحيث تُحسب cluster culling و التغطية على المشهد المُدار على GPU.
- يحافظ على إخراج 013 المتطابق عند مشهد مطابق (لا تغيير في القيم المرصودة).

### 3.4 الاستعداد لـ Render Graph (015)

- Scene DB ينتج **snapshot/سجل draw records** كمدخل موحّد لـ render graph لاحقًا (لا تنفيذ في 014).
- فصل بيانات المشهد عن مسار الرسم الحالي بحيث يكون أسهل إدخالًا في بنية graph.

### 3.5 الدليل على القدرة (benchmark)

- مشهد مخصص 014 بكثافة عالية (يصل إلى حدود الجهاز): قياس **عدد المثيلات الفاعلة/المدارة**، زمن dispatch (uploads + cull + assembly)، draw counts، واستخدام الذاكرة (SSBO bytes) — أرقام فعلية تُسجَّل كـ benchmark.

## 4. معايير القبول (PASS — 8 معايير مقترحة)

1. **GPU Scene DB**: بيانات المثيلات (≥1M) ساكنة في SSBO على GPU؛ عدد المثيلات المُدارة يُقاس ويُطبع.
2. **GPU-driven update**: تطبيق التحديثات (add/remove/move) عبر compute دون CPU في المسار الحرج؛ صفر readback حرج.
3. **تكامل 013**: مشهد مطابق لمشهد 013 → نفس نواتج cluster culling/LOD/coverage (signature حرفية).
4. **توافق 011**: التجميع (نمط ≤5 draw calls) يعمل على المجموعة النهائية من جدول المشهد.
5. **Determinism (DET)**: توقيع DET ثابت بين تشغيلين (نمط `sig=`) يشمل counts/coverage و draw counts.
6. **Regressions 001A–013**: كل المسارات السابقة `exit 0` على نفس البنية مع **signatures حرفية ثابتة** للمسارات غير الملموسة.
7. **Capacity/benchmark**: تسجيل أرقام فعلية (instances، dispatch_us، draw calls، SSBO memory) في المشهد 014.
8. **Zero RID errors**: لا أخطاء RID في كل التشغيلات (نمط 013).

## 5. تغييرات الواجهات المقترحة (Test-only في البداية)

| API | النوع | الغرض |
|---|---|---|
| `gpu_scene_alloc_object(max_instances: int)` | Test-only | تخصيص جدول مشاهد GPU بسعة محددة |
| `gpu_scene_set_instances(instances: PackedFloat32Array)` | Test-only | تعبئة بيانات المثيلات (transforms/refs/LOD/bounds) |
| `gpu_scene_update(deltas: Array)` | Test-only | اختبار مسار التحديث (add/remove/move) |
| `gpu_scene_get_stats()` | Test-only | عدّادات الأدلة: managed instances، dispatch_us، SSBO bytes |
| `gpu_scene_get_draw_counts()` | Test-only | draw counts بعد التجميع (توافق 011) |

ملاحظة: البقاء على قاعدة «test-only حتى مراجعة الاستراتيجية» (كما في 012/013).

## 6. نطاق خارجي (Out of scope لـ 014)

Render Graph (015)، Materials + Lighting (016+)، GI (019+)، Production (022+)، HZB إنتاجي (012 مؤجّل)، bindless/VMA، reversed-Z، دمج RenderingServer/RHI، تغيير أحجام بيانات 013 أو signatures الثابتة لمشاهد مطابقة.

## 7. التسليمات

- C++: GPU Scene Manager (واجهات test-only أعلاه: SSBO alloc، upload queue، compute update، getters counters) + تكامل مع مسار 013/011.
- Demo: `demo/gpu_smoke/main_014.gd` + `main_014.tscn` (مشهد ملايين مثيلات).
- Harness: `gt_014a` (نمط 013) + مخرجات `sig` + لقطة `gt_014_window.png` إن ضبطت القراءة غير الحاسمة.
- توثيق: قسم 25 في `docs/progress_report.md` + تحديث README (milestone 014 + roadmap).
- Sweep regressions: 001A–013 بالكامل.
- الملفات المحمية: `docs/sac_unblock_procedure.md`، `docs/open_source_system_strategy_v1.md`، `docs/dependency_register.md` — لا تُلمس إلا بأمر مؤكد.

## 8. الحالة

**SPEC 014 v0.1 DRAFT — بانتظار مراجعة المعماري والتثبيت.**

لا تنفيذ، لا build، لا commit، لا push قبل موافقة Owner/المعماري على SPEC النهائي والبدء.