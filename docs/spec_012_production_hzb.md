# GOTOT-012 — Production HZB (SPEC DRAFT)

**الحالة:** SPEC 012 v0.1 DRAFT — بانتظار مراجعة المعماري وتثبيت النهائي.
**المرجع:** docs/progress_report.md (قسم 19/20 — 011) + docs/spec_010_batch_instance_rendering.md (مبنى 010/011) + قسم GOTOT-004 (HZB prototype).
**المرحلة السابقة:** GOTOT-011 — Multi-Draw / Multi-Batch (PASS، commit `371cb3c` على GitHub).

---

## 1. الهدف

ترقية HZB من **prototype** (GOTOT-004) إلى **HZB إنتاجي** متكامل مع مسار الدفعات (011): هرم عمق متعدد المستويات (≥12 مستوى)، occlusion culling **two-phase**، **temporal coherence**، و **GPU-driven** بالكامل (بدون تدخل CPU في المسار الحرج). الهدف العملي: تقليص عدد المثيلات المرئية خلف المعيقات الواقعية قبل تجميع الدفعات (011) — لا يوكل إلى الـ GPU فقط العدد بل **القائمة نفسها** المستهلكة من `gpu_mesh_batch_dispatch`.

> المسار المطلوب: GPU Scene → (Phase 1) Frustum Culling → (Phase 2) HZB Occlusion → Visibility → Batch Assembly (011) → Multi-Draw → Rasterization (D32_SFLOAT) → أعلى الهرم يغذي إطارًا مقبلًا (temporal).

## 2. الخلفية والقيود الموروثة

- **004 (prototype):** نسيج `512×512 R32UI` 2D-array بـ 10 مستويات؛ ممران: (1) clear + occluder pass + downsample×9، (2) دمج HZB في الـ cull بمرور واحد (فحص 2×2 عبر `imageAtomicMax` وتحويل `sphere_inv`). `visibility_ms≈0.7`، 377 → 375 على 100K بلا معيقات في الخلفية.
- **009/010/011:** عمق `D32_SFLOAT` حقيقي (fallback) موجود بالفعل ومكتوب كل إطار في مسار الـ mesh — يمكن بناؤه للهرم مباشرة من الـ depth buffer نفسه (مصدر أدق من معيقات push-constant المتجاهلة لعمق المشهد).
- **011:** `gpu_mesh_batch_dispatch` يستهلك مصفوفة `visibility[]` (من `gpu_cull_dispatch`/`gpu_visibility_dispatch`). HZB الإنتاجي يجب أن يغذي **نفس** مصفوفة الـ visibility حتى يعمل التجميع/الترتيب (5 draw calls) على المجموعة **النهائية** بعد الإخفاء.
- القيد: مشاهد 010/011 الحالية بلا معيقات → فيها كل المثيلات في الفروستوم مرئية؛ HZB يجب أن يكون **no-op تلقائياً** (علامة `hzb_valid=false` مثل 004) كي تبقى signatures الـ 011 ثابتة حرفيًا.

## 3. التصميم المقترح (مفتوح للتحسين في المراجعة)

### 3.1 الهرم (Multi-level hierarchy ≥ 12)

- المصدر: **عمق الـ D32_SFLOAT الفعلي** (من إطار السابق) بدل معيقات منخفضة الدقة — downsample لكل مستوى بـ max 2×2 مع تحويل العمق إلى `depth_inv = floatBitsToUint(FAR_plane - z_view)` (قيمة أعلى = أقرب، كما في 004) عبر `imageAtomicMax`.
- البنية: نسيج 2D-array `R32UI` قاعدة `2048×2048` → `levels = log2(2048)+1 = 12` (بالضبط الحد الأدنى للموافقة). يُقاس ويُطبع `level_count` كدليل.
- المستوى الأعلى كل مستوى يُبنى من أدنى بالتوازي (ممر downsample واحد يغطي كل المستويات)، لا حلقات CPU على المستويات.

### 3.2 Two-phase occlusion culling

- **Phase 1 — Frustum culling** (كما في 002/004): ينتج القائمة الأولية للمرئيات.
- **Phase 2 — HZB occlusion**: على المجموعة الخارجة من Phase 1 فقط، فحص 2×2 (أو 4×4 لمقاومة الحركة) ضد الهرم وتحويل عكسي `sphere_inv`؛ الناتج = مجموعة مرئية نهائية.
- المراحل منفصلة بحيث يكون العدد قابلاً للقياس **منفصلًا لكل مرحلة** (`phase1_count >= phase2_count`) مع **ضابطة no-occluder** (بدون معيقات: phase2_count == phase1_count تمامًا، كـ 004).

### 3.3 Temporal coherence

- يُعاد استخدام HZB الإطار السابق (مبني من عمق الإطار الأخير) فيما لم يتوفر عمق الإطار الحالي بعد — وهذا جوهر **GPU-driven**: لا ننتظر readback ولا نُجمِّد الإطار.
- للأجسام الثابتة (camera/geometry ساكنة 2+ إطار): النتيجة متطابقة (coherence تُقارن بفحص `hzb_coherent`).
- أي تحديث للمشهد (كاميرا متحركة أو تحويل جديد) يجعل الهرم قديمًا جزئيًا → **توسّع تحفظي** في فحص المركّز (لا يتلاشى بشكل خاطئ): الأجسام في حدود عتبة التوسّع تُحتفظ مرئية.

### 3.4 GPU-driven

- لا قراءة GPU→CPU في المسار الحرج. كل بيانات الهرم تُبنى وتُفحص على الـ GPU.
- جسر الـ readback (pixels/depth كامل الإطار) يبقى **للتحقق فقط** (نفس قاعدة 007A/009)، ولا يدخل في حساب الـ culling.
- **الفرق الجوهري عن 004:** الـ prototype كان مقتصرًا على معيقات push-constant منخفضة الدقة وبدون temporal؛ 012 يبني الهرم من عمق `D32_SFLOAT` الفعلي مع temporal coherence.

### 3.5 التكامل مع 011

- الـ HZB يعمل قبل `gpu_mesh_batch_dispatch` — كتابة `visibility[]` نفسها → التجميع (≤5 draw calls) يُرى على المجموعة النهائية.
- **حماية الـ regressions:** بدون occluders → `hzb_valid=false` → الـ Path مطابق لإخراج 011 (signatures حرفية ثابتة). مع occluders → مسار جديد test-only.

### 3.6 الدليل على فعالية الأداء

- مشهد مخصص 012: كثافة مثيلات عالية + معيقات كبيرة (جدران/طائرات) أمام الجزء الأكبر منها → القياس: `phase1_count` مقابل `phase2_count` + `cull/visibility dispatch_us` وكذلك تأثير المجموعة النهائية على draw counts (011) — أرقام فعلية تُسجَّل كـ Benchmark (وليست ادعاءً أداءً).

## 4. معايير القبول (PASS — 7 معايير مقترحة)

1. **Multi-level hierarchy**: `gpu_hzb_get_level_count() >= 12` ويُطبع؛ الهرم مُبنى بالتوازي.
2. **Two-phase occlusion culling**: `phase2_count <= phase1_count`؛ مع المعيقات تقلُّ حقيقيًا (قياس ملموس); بدون معيقات `phase2 == phase1` (ضابطة).
3. **Temporal coherence**: كاميرا ساكنة إطاران+ → النتيجة مستقرة (coherent); كاميرا متحركة → لا انفجارات مرئية ولا فقدان خاطئ (توسّع تحفظي مفعّل).
4. **GPU-driven**: صفر readback في المسار الحرج (يُثبت بأن الحوسبة تعمل كل الإطارات والقراءات محصورة بالإطارات المعلَّمة للتحقق).
5. **Determinism (DET)**: توقيع DET ثابت بين تشغيلين (نمط `sig=` كالسابق) يشمل `levels/p1/p2/coherent` و draw counts 011.
6. **Regressions 001A–011**: `gt_smoke` / 007 / 008 / 008b / 009a / 009b / 010a / 010b / 011a / 011b / 011c — كلها `exit 0` على نفس البنية مع **signatures حرفية ثابتة** لما لا يلمسه HZB (لا-occluder).
7. **Performance measurable**: جدولة أرقام `phase1/phase2 counts`, `dispatch_us` (cull + HZB)، وقياس أثر التجميع (draw calls) مع/بدون HZB في مشهد 012.

## 5. تغييرات الواجهات المقترحة (Test-only في البداية)

| API | النوع | الغرض |
|---|---|---|
| `gpu_hzb_set_occluders(Vector4[] boxes)` | Test-only | تعيين معيقات للمشهد (ملحق/بديل لمسار 004) |
| `gpu_hzb_get_level_count()` | Test-only | عدد مستويات الهرم (الأدلة ≥12) |
| `gpu_hzb_get_phase_counts()` | Test-only | `phase1` و `phase2` للأدلة |
| `gpu_hzb_get_coherent()` | Test-only | فلاغ coherence (temporal) |
| `gpu_hzb_enable_temporal(enabled)` | Test-only | تفعيل/تعطيل temporal (مقارنات الأدلة) |

ملاحظة: البقاء على قاعدة "test-only حتى مراجعة الاستراتيجية" (Sections 19–20).

## 6. نطاق خارجي (Out of scope لـ 012)

Meshlets / LOD، نظام المواد، render graph، bindless/VMA، reversed-Z (مؤجل)، دمج RenderingServer/RHI، إزالة جسر الـ readback. لا تغيير في مسار البيلبورد، ولا تغيير يمس signatures 010/011 (لا-occluder).

## 7. ملاحظات التكامل مع 011 (مُسجّلة — لا تطبيق خارج نطاق التقارير)

- مراقبة الـ deferred القديمة: frustum reading، atomic ordering، reversed-Z، 007A frame-time drift — لا تُعالج في 012 ما لم يقرر المعماري خلاف ذلك.
- تجميع 011 (G = min(active,5)) يعمل على المجموعة **بعد** HZB — تُوثَّق النتيجة الأرقامية في PASS criterion 7.
- مسار المجموعات procedural يُبقي `phase2_count` أدق إدخال لجدولة الدفعات.

## 8. التسليمات

- C++: هرم عمق ≥12 مستوى مبني من `D32_SFLOAT` + two-phase visibility + temporal flag + getters test-only (+ حراسة no-occluder).
- Demo: `demo/gpu_smoke/main_012.gd` + `main_012.tscn` (مشهد كثافة + معيقات كبيرة).
- Harness: `gt_012a` / `gt_012b` (نمط 011) + مخرجات `sig` + لقطة `gt_012_window.png`.
- توثيق: قسم 21 في `docs/progress_report.md` + تحديث README (milestone 012 + roadmap ⏳→✅) بعد PASS.
- Sweep regressions: 001A–011 بالكامل.
- الملفات المحمية: `docs/sac_unblock_procedure.md`, `docs/open_source_system_strategy_v1.md`, `docs/dependency_register.md` — لا تُلمس.

## 9. الحالة

**SPEC 012 v0.1 DRAFT — بانتظار مراجعة المعماري والتثبيت.**

لا تنفيذ، لا build، لا commit، لا push قبل موافقة Owner/المعماري على البدء.