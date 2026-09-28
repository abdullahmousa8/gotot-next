# GNE-015.5 — Resource Pool (SPEC DRAFT)

**الحالة:** SPEC 015.5 v0.1 DRAFT — بانتظار مراجعة المعماري وتثبيت النهائي.
**المرجع:** docs/progress_report.md (§26=015، §27=014) + docs/spec_014_gpu_scene_manager.md (نمط الصياغة) + modules/gne_render/gne_render_server.* (مبنى 015 عند `56849fa`).
**قرار المالك:** 2026-09-27 — 015.5 = Resource Pool **APPROVED** (أولوية قصوى). VMA Strategy = **Resource Pool** (بناء داخلي، لا استيراد).
**المرحلة السابقة:** 6 commits مدفوعة إلى `origin/main` عند `56849fa`، `GT_REGRESS: PASS`.

---

## 1. الهدف

تحويل **محاسبة الـpool** في 015 إلى **ذاكرة حقيقية**، وإزالة جسر الـreadback من المسار الساخن:

1. **Transient resource pool** — ذاكرة cursory تُعاد بين الإطارات مع aliasing فعلي (لا محاكاة).
2. **Persistent resource pool** — ذاكرة طويلة العمر (meshes, draw records, HZB, scene DB) بسقف VRAM معلوم.
3. **Dynamic buffer allocation** — نمو/انكماش مُقاس (grow داخل الإطار، إعادة بناء بين إطارات).
4. **Frame-time drift resolution** — قياس ثم إصلاح (لا ادّعاء قبل قياس).
5. **Zero-copy presentation** — **مشروط بقرار (§9)**؛ المسار الحالي معروف بأنه جسر.

## 2. الخلفية والقيود الموروثة

### 2.1 اكتشاف حاسم: 015 pool = محاسبة فقط، لا ذاكرة

`rg_pool_bytes` / `rg_res_bytes` / `rg_alias_saved` **أعداد CPU فقط**، و`rg_pool` ناقص `Vector<RgPoolSlot>` بلا أي `buffer_create`؛ و`gpu_rg_execute()` (سطر 6483) **لا ينفّذ أي pass body** — يزيد عدّادات ويطبع. أي أن:
- `pool=8,753,152` و`aliased_saved=32,768` في توقيع 015 **أرقام محسوبة، لا ذاكرة مُوزَّعة**؛
- ممرات الـrender اليوم تعمل بـ**موارد مُنشأة يدوياً** خارج الـDAG؛
- ⇒ **مهمّة 015.5 الأساسية: جعل الأرقام حقيقية**، وأي رقم VRAM/performance يُنسب ل Ignacio 015 قبل ذلك **غير صالح** كدليل.

### 2.2 المسار الساخن الحالي (readback)

`gpu_raster_read_pixels()` (سطر 4585) و`gpu_raster_read_depth()` (4597) و`gpu_raster_read_viewz()` (4617) تستخدم `texture_get_data()` ← نسخ كامل 1920×1080 إلى CPU، ثم `Image.create_from_data` + `ImageTexture.update` في GDScript. التكلفة المُقاسة سابقاً ~8MB لكل من pixels/depth لكل إطار (جسر تحقق مُعلَن، لا مسار إنتاجي).

### 2.3 الموارد الحقيقية القائمة اليوم (للمرجع)

| مورد | الحجم | الموقع |
|---|---|---|
| `raster_color_texture` 1920×1080 `R8G8B8A8_UNORM` | 8,294,400 B | cpp 3471–3478 |
| `raster_depth_texture` 1920×1080 `D32_SFLOAT` | 8,294,400 B | cpp 3488–3495 |
| `raster_viewz_texture` 1920×1080 `R32_SFLOAT` | 8,294,400 B | cpp 3506–3513 |
| `hzb_prod_array` 2048×2048×12 `R32_UINT` | 201,326,592 B | cpp 2735–2746 |
| `gms_record_buffer` @1M | 67,108,864 B | contract_014_buffers |
| `gms_snapshot_buffer` @1M | 33,554,432 B | contract_014_buffers |
| `gms_ring_buffer` | 16,777,216 B | contract_014_buffers |

### 2.4 القيود الملزِمة الموروثة
- **انزياح التوقيع ممنوع**: `sig=v13|…|f3106528256`، `v14|c18616|…|d1`، `v15-pc9-…-t18616` تبقى حرفية.
- كل تغيير سلوكي ⇒ **بناء + إعادة تشغيل 013/014/015 + `gt_harness` كاملاً + الضابط السالب `main_012 → rc=123`**.
- Test-only أولاً؛ لا دمج في `RenderingServer` ولا RHI.
- 012 مؤجَّل ولا يُلمس.

## 3. التصميم المقترح (مفتوح للتحسين في المراجعة)

### 3.1 Transient pool (cursor allocator)
- كتلة `pool_buffer` واحدة + **bump cursor** + **free-list** للكتل المحرَّرة.
- تخصيص/تحرير على أساس **عمر المورد داخل الإطار**: `alloc(bytes, first_pass, last_pass)`؛ موارد الأعمار المتقاطعة لا تتشارك.
- **Aliasing حقيقي**: موردان بعمرين متقاطعين ⇒ نفس الإزاحة الفيزيائية، و`aliased_saved` يُحسب من التخطيط لا من جمع أرقام.
- المعيار: **حجم البليت المُعلن = الحجم المُخصَّص فعلاً** (مُقاس، لا محسوب).

### 3.2 Persistent pool
- سقف VRAM معلن (يُختار في Phase 1 قياساً) + سجل تخصيص دائم.
- **لا تخصيص دائم أثناء الإطار**؛ النمو بين الإطارات فقط.
- تقرير: `persistent_bytes` / `headroom_bytes`؛ طلب متعذّر ⇒ `print_error` صريح (لا تجاوز صامت).

### 3.3 Dynamic allocation
- **grow-only** داخل الإطار؛ تقليص/إعادة بناء **بين** الإطارات عند sync معروف.
- كل نمو = `buffer_create` جديد + نسخ + `free_rid` بعد حاجز ⇒ عدّاد `pool_rebuilds` + `bytes_copied` (قابل للتدقيق).

### 3.4 Frame-time drift
- **لا يُصلَح قبل يُقاس**: harness يقيس زمن الإطار على N إطارات ويطبع وسيط/p95/أقصى + `drift_ms = p95 − p50`.
- خط الأساس غير مُفسَّر (مذكور في التقرير: 007A frame ≈ 11.36 → 14.16 ms).
- الإصلاح يُختار بعد القياس — **ليس قراراً يُتخمَّن**.

### 3.5 Zero-copy presentation (**مشروط**)
- المطلوب: منع `texture_get_data` من المسار الساخن.
- **القيد المرصود**: لا مسار مشاركة `RD::Texture` مع `RenderingServer` بلا نسخ في 4.8.dev؛ و`texture_get_native_handle()` يعطي معرّفاً أصلياً لا يُربط بدورة حياة `ImageTexture` بأمان.
- ⇒ الخيارات في §9. **لا ادّعاء zero-copy قبل قياس بايتات فعلية.**
### 3.6 Presentation Path (D3-D1) — **قرار: Async Readback، وليس zero-copy**

- **القرار:** Async Readback. **التسمية الإلزامية في كل وثيقة/سجل/رسالة commit: «async readback, not zero-copy».**
- **السبب (مرصود في §3.5):** المسار الحقيقي يحتاج تكاملاً مع `RenderingServer` (تغيير أعمق) ⇒ مؤجَّل إلى milestone 020+، فلا يُنشر ادّعاء zero-copy في 015.5.
- **التصميم:** فence-based، بلا انتظار حاجز داخل الإطار؛ `pool_staging` متعدد الإطارات مع دوران `frame_index % N`.
- **سلامة الدوران:** لا كتابة على مخزن staging قبل `submit()+sync()` الخاص به؛ المخالفة ⇒ `_fail` (لا صمت).
- **تحسين متوقَّع 30–50% من عبء الـreadback — فرضية تُقاس، لا ادّعاء**: تُسجَّل كـ`baseline (Phase 1)` مقابل `result (Phase 5)`، وأي رقم يُنشر بلا قياس ⇒ يُرفض.

### 3.7 Persistent Resources (D3-D2) — **قرار: سقف 256 مُدخلاً**

- يكفي لـ: Scene DB + Mesh Tables + HZB + Render Targets.
- **`persistent_count > 256` ⇒ `print_error` + رفض التخصيص (معيار 4)**؛ لا تجاوز صامت ولا توسيع تلقائي فوق السقف.

### 3.8 Growth Policy (D3-D3) — **قرار: double-up-to-cap**

- ابتداء: **64 مُدخلاً**. النمو: **64 → 128 → 256** ثم **قف**.
- إعادة البناء بين الإطارات فقط؛ كل عملية = `pool_rebuilds++` و`bytes_copied += old_size` (معيار 5، حتمي).

### 3.9 Signature Naming (D3-D4) — **قرار: داخل `v15` مع راية `pr`**

- التوقيع الجديد: **`v15-…-pr1`** حيث `pr1` = `pool_real=1` (ذاكرة حقيقية، لا محاسبة).
- **متوافق للخلف:** التوقيعات القديمة بلا `pr` تبقى صالحة ⇒ لا كسر للعقد.
- **لا `v15.5`** (تفادياً لكسر التوقيع). ⇒ يُحدَّث `contract_015_5_tests.md §4` ليطابق.

## 4. معايير القبول (PASS — 8 معايير مقترحة)

1. **Pool حقيقي**: `pool_buffer` فعلي + bump/free-list؛ إثبات بتخزين/استرجاع قيمة عبر عمليتين منفصلتين (لا تكفي أرقام المحاسبة).
2. **Aliasing فعلي**: موردان بعمرين متقاطعين ⇒ نفس الإزاحة الفيزيائية، و`aliased_saved` من التخطيط لا من جمع أرقام.
3. **ثبات التوقيعات**: `013` و`014` و`015` مطابقة حرفياً + **صفر** `ERROR:` + الضابط السالب `012 → rc=123`.
4. **Persistent pool بسقف معلن**: `persistent_bytes ≤ ceiling`؛ طلب متعذّر ⇒ `_fail` (لا تجاوز صامت).
5. **Dynamic allocation قابل للتدقيق**: `pool_rebuilds` و`bytes_copied` مطبوعان؛ البناء حتمي (نفس المدخلات ⇒ نفس الأرقام).
6. **قياس drift قبل أي إصلاح**: تقرير `frame_ms` (وسيط/p95/أقصى) على ≥3×N إطارات، **قبل** و**بعد** أي تغيير، بنفس المشهد.
7. **تقليص readback من المسار الساخن** (أو إثبات تعذّره بدليل): مسار بلا `texture_get_data`، أو تقرير لماذا + توثيقه قيداً.
8. **صفر انحدار 001A–011**: `gt_harness` كاملاً (9 ok / 0 bad / 1 xfail) + `gt_regress → GT_REGRESS: PASS`.

## 5. تغييرات الواجهات المقترحة (Test-only في البداية)

| API | النوع | الغرض |
|---|---|---|
| `gpu_pool_create(bytes)` | Test-only | إنشاء الـpool بحجم معلن |
| `gpu_pool_alloc(bytes, first, last, tag)` | Test-only | تخصيص مع عمر (يرجع index أو -1) |
| `gpu_pool_free(index)` | Test-only | تحرير |
| `gpu_pool_stats()` | Test-only | `{pool_bytes, used_bytes, free_bytes, alias_saved, rebuilds, bytes_copied, persistent_bytes}` |
| `gpu_pool_verify(tag)` | Test-only | كتابة/قراءة/تحقق قيمة عبر عمليتين (معيار 1) |
| `gpu_frame_stats(n)` | Test-only | `frame_ms` وسيط/p95/أقصى + `drift_ms` |

قاعدة: الدوال الجديدة **لا تُضاف** إلى `gpu_scene_manager_get_stats()` ولا `gpu_rg_get_stats()` (المستبعدة من قارئات DET في `main_014.gd:459`) — تُصدَّر في قاموس مستقل.

## 6. نطاق خارجي (Out of scope لـ 015.5)
012-revised · Materials (016) · VMA أو مكتبات تخصيص خارجية · RHI/Backend جديد · descriptor indexing/bindless · GI/VSM · تعديل `RenderingServer` · Mesh shaders · Software Rasterizer بديل · أي تغيير على مسارات 001A–011.

## 7. التسليمات
- **SPEC**: هذا الملف + 5 وثائق عقد: `contract_015_5_data|buffers|lifecycle|boundaries|tests.md`.
- **C++ (Phase 2)**: `gpu_pool_*` + جعل `gpu_rg_execute` ينفّذ ممرات حقيقية عبر pool + harness `gt_0155`.
- **Demo**: `demo/gpu_smoke/main_0155.gd|.tscn`.
- **الدليل**: `sig=v15.5-…` + جدول before/after لـ`frame_ms` و`readback bytes` و`pool bytes`.
- **الملفات المحمية**: `sac_unblock_procedure.md`، `open_source_system_strategy_v1.md`، `dependency_register.md` — لا تُلمس إلا بأمر مؤكد.

## 8. ترتيب التنفيذ المقترح
1. **Phase 0** (هذا الـSPEC): مراجعة المعماري + تثبيت.
2. **Phase 1 (قياس)**: `frame_ms` baseline + readback bytes + VRAM الحالي ⇒ أرقام قبل أي تغيير.
3. **Phase 2 (Transient + Aliasing)**: معيارا 1–2.
4. **Phase 3 (Persistent + Dynamic)**: معيارا 4–5.
5. **Phase 4 (drift)**: قياس بعد ⇒ إصلاح ⇒ قياس (معيار 6).
6. **Phase 5 (presentation)**: تنفيذ ما يُقرَّر في §9، أو توثيق التعذّر (معيار 7).
7. **Phase 6 (Gate + Push)**: `gt_harness` + `gt_regress` + push.

## 9. قرارات المالك — **محسومة (D1–D4)**

| # | القرار | النتيجة |
|---|---|---|
| D3-D1 | Presentation | ✅ **Async Readback** (ليس zero-copy) — §3.6 |
| D3-D2 | Persistent cap | ✅ **256 مُدخلاً** — §3.7 |
| D3-D3 | Growth | ✅ **double-up-to-cap** 64→128→256 — §3.8 |
| D3-D4 | Signature | ✅ **داخل `v15` + راية `pr`** ⇒ `v15-…-pr1` — §3.9 |

**قرارات نطاق من Owner (2026-09-27):**
- بادئة السجل `[GNE]` في C++ (164 موضعاً) ⇒ **مؤجَّلة إلى Phase 2** (ليست نصوصاً تعليقية؛ `gt_015a.bat` يعتمد عليها حرفياً).
- الملفات المحمية الثلاثة (SPEC 014 §7) ⇒ **لا تُلمس**.

**ما لم يُحسم بعد (يُقاس في Phase 1 ثم يُثبَّت، ولا يُخمَّن):** سقف الـVRAM بالبايت، وقياس التحسين (تحسين 30–50% فرضية).

## 10. الحالة
**DRAFT v0.2 — D1–D4 محسومة. جاهز للتنفيذ (Phase 1 = قياس، ثم Phase 2+).**
قرارات D3-D1..D4 مثبَّتة في §3.6–§3.9 أعلاه. يبقى التوقيع النهائي بعد التنفيذ: **لا commit للكود قبل قياس خط الأساس (Phase 1)**.

