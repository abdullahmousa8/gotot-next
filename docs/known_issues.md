# GNE — Known Issues

قيود معروفة **مُقيسة** في هذا البناء، مسجَّلة حتى لا يُعيد أحدٌ اكتشافها من الصفر. كل بند يحمل دليله (file:line أو قياس حيّ)، ولا يُسجَّل افتراض.

---

## KI-001: GPU Timestamps غير متاحة على Vulkan

**التاريخ:** 2026-09-27 · **الحالة:**Unavailable — قيد في محرّك Godot (خارج نطاق GNE)
**الأثر:** GNE لا يستطيع قراءة زمن GPU على الواجهة الخلفية Vulkan. لذلك يقيس Phase 5 على **wall-clock** حصراً.
**الإغلاق (015.6):** مقبول NA + موثّق (c) — كل الأرقام wall-clock حصرًا، ولا يُنشر أي رقم كزمن GPU. يُعاد النظر فقط عند إصلاح upstream.

### ما يعمل (مُقاس)
- الـpool مُفعَّل فعلياً: `ProjectSettings.set_setting("debug/settings/profiler/max_timestamp_query_elements", 512)` **قبل** إنشاء الجهاز ⇒ `capture_timestamp()` صار يقبل: `get_captured_timestamps_count()` ينتقل `0 → 1` بعد النداء.
- كود حلّ النتائج في المحرك **صحيح**: `servers/rendering/rendering_device.cpp:8335-8342` ينادي `timestamp_query_pool_get_results` ثم `SWAP` ثم `timestamp_result_count = timestamp_count`.
- سائق Vulkan يطبّق مسار القراءة كاملاً: `drivers/vulkan/rendering_device_driver_vulkan.cpp:6822` → `vkGetQueryPoolResults(...)`.

### السبب الجذري
`drivers/vulkan` **لا يحتوي ملف `utilities.cpp`** إطلاقاً. الملف الوحيد الذي يضبط
`timestamp_result_count` في المحرك هو `drivers/gles3/storage/utilities.cpp:367`
(`frames[frame].timestamp_result_count = frames[frame].timestamp_count;`).
⇒ على Vulkan تُجلب القيم فعلاً لكن **العدّاد لا يُنشر**، فيُرجع كل قارئ `0`.
والقراءة في **نفس** الإطار الذي التُقطت فيه الترويسة تُرجع استعلاماً غير محلول
(قِيست: `1.79e18 ns` — قيمة عشوائية).

### قيود إضافية مرتبطة (كلها مُقاسة)
| القيد | الدليل |
|---|---|
| لا يمكن تفعيله من `project.godot` | مُسجَّل بـ`GLOBAL_DEF_RST` = runtime-only، لا يُقرأ من ولا يُكتب في المشروع — `core/config/project_settings.cpp:1811` |
| النطاق لا يسمح بـ0 | `PROPERTY_HINT_RANGE, "256,65535,1"` — نفس السطر |
| يجب الضبط **قبل** `ensure_gpu_device()` | يُقرأ مرة واحدة داخل `RenderingDevice::initialize()` — `rendering_device.cpp:8625`؛ وجهتنا كسولة: `create_local_rendering_device` → `create_local_device` → `initialize` |
| `max_timestamp_query_elements` خاص | غير قابل للقراءة من خارج `RenderingDevice` (private) ⇒ لا يمكن تأكيد الحجم المحلول إلا عبر عدّاد القراءة نفسه |

### الحل العملي
- القياس على **wall-clock** لكل ممر (كافٍ لـPhase 5، وهو ما ينتج الأرقام الحقيقية).
- **لا patch للمحرك**: خارج النطاق + خطر. (تقرير bug إلى Godot upstream خيار لاحق، غير معلّق عليه.)
- عمود GPU في التقارير يُطبع **`NA`** ولا يُقرأ أبداً كـ«تكلفة صفرية» — وهذا مقصود.
- تفعيل الـpool (512) يبقى في المشهد: الاتجاه صحيح ولا يضرّ.

### الأثر على GNE
- Phase 5 يقيس قبل/بعد على wall-clock.
- **كل التواقيق غير متأثرة**: `013` / `014` / `015` / `v15.5-p4` كما هي.
---

## KI-002: Async Readback Shares The Sync Staging Buffer

**التاريخ:** 2026-09-27 · **الحالة:** لا أسرع من المتزامن — قيد في المحرك
**الخطورة:** منخفضة (موثّقة، لا أثر على الإنتاج) · **النطاق:** كل قياس إطار incurs the sync cost
**الإغلاق (015.6):** مقبول + موثّق — الـstaging مملوك للمحرك ولا يُعزل من GNE؛ لا ادعاء تسريع async بلا دليل بايتات/زمن.

### ما هو متاح فعلاً
- `RenderingDevice::texture_get_data_async` **موجودة** (`servers/rendering/rendering_device.h:475`)، وتُسلِّم `PackedByteArray` عبر `request.callback.call(packed_byte_array)` (`rendering_device.cpp:8543`).
- **لكنها تستعمل نفس `download_staging_buffers`** المستخدَم في `texture_get_data` — `rendering_device.cpp:1432` و `:2916`.
- وكلا المسارين يقع في `STAGING_REQUIRED_ACTION_FLUSH_AND_STALL_ALL` عند نضوب المخزن ⇒ `1080: _flush_and_stall_for_all_frames()`.
- **الجذر (مُقاس في Phase 4):** 8 MB (pixels) + 8 MB (depth) لكل إطار تتجاوز سعة المخزن المؤقت الافتراضية، فالتجميد يقع **كل إطار** ⇒ `raster+output = 81.8%` من زمن الإطار.

### ما جرى فعلاً (حاولة موثّقة، لا ادّعاء)
- نُفِّذت واجهة async كاملة في C++ (7 دوال + `bind_method`s) + مشهد `main_015_5_async.gd` بثلاثة أذرع قياس (`sync` / `async` / `no_readback`).
- البناء **نظيف** (صفر أخطاء؛ أُصلحت 3 أخطاء حقيقية: مسار `callable_mp` = `core/object/` لا `core/variant/`، عدد وسائط `texture_get_data_async` = 3، و`Vector<RID>` غير قابل للتكليف بـ`int`).
- **النتيجة: لا تحسّن مثبت.** القياس لم يكتمل: العملية كانت **منتظرة لا حاسبة** (‎+3 ثوانِ معالج لكل 28 ثانية زمن) — سلوك `stall` لا حساب.
- ⇒ طُلب من المالك قرار، وأُرجِع الـAPI بالكامل.

### القرار (Owner، 2026-09-27)
- **استرجاع** واجهة async: لا دليل على تحسّن ⇒ لا كود ميت في المستودع.
- **لا تعديل للمحرك** (خارج النطاق، اتساقاً مع KI-001).
- **الانتقال إلى Phase 5.1**: تحسين مسار **العرض** فقط (دقة نصفية / تخطي إطار / إعادة استخدام `Image`) — وهو ما يمكن قياسه فعلاً.

### الأثر على GNE
- المسار المتزامن **لم يُمَسّ** طوال المحاولة (تحقّق: `git diff` لا يلمس `gpu_raster_read_pixels/depth`).
- **كل التواقيق محفوظة**: `013` / `014` / `015` / `v15.5-p4` كما هي.
- لم يُعمل أي commit للـasync API (قيد: «لا commit بلا قياس ناجح»).

---

## KI-003: Presentation Optimization Has No Measurable Effect

**التاريخ:** 2026-09-27 · **الحالة:** مُغلق (الجذر في المحرك، لا في GNE)
**الخطورة:** منخفضة (موثّقة، لا أثر على الإنتاج) · **القرار:** إغلاق Phase 5.1 بلا commit

### ما جرى Investigation
| الطريقة | النتيجة | السبب (مُقاس) |
|---|---|---|
| **A** قراءة نصف الدقة (4×) | ❌ **مستحيلة بلا C++ جديد** | `texture_get_data` يقرأ النسيج **كاملاً** بلا sub-region؛ والهدف `RASTER_TARGET_W/H = 1920/1080` ثابت `constexpr` (`gne_render_server.h:77-78`) مستخدَم في 9 مواضع. ول she'd تُغيّر البكسلات المعروضة ⇒ لا يقيسها أي harness يفحص بكسلات. |
| **B** تخطي إطار (2×) | ⚠️ ممكنة بلا C++ | **لا تعالج الجذر**:تقليل تخطي الطلبات لا يوسّع `download_staging_buffers`، فالتجميد يعود عند الامتلاء. |
| **C** إعادة استخدام `Image` | ⚠️ ممكنة بلا C++ | **لا تعالج الجذر**: `Image.set_data` (`core/io/image.h:410`) يوفّر تخصيصاً على المعالج، والعبء الحقيقي على البايتات المنسوخة. |

### الجذر (Phase 4 + KI-002)
- كل إطار فيه readback يقع في `STAGING_REQUIRED_ACTION_FLUSH_AND_STALL_ALL` (نضوب `download_staging_buffers`).
- زمن الإطار يصبح **≈2 ثانية** (مقاس: العملية «منتظرة لا حاسبة» — ‎+3 ثوانِ معالج لكل 28 ثانية زمن).
- ميزانية القياس (300 إطار/ذرع، وحتى 60 إطار/ذرع) **لا تكتمل** ⇒ لا رقم ⇒ لا ادّعاء.

### القرار (Owner، 2026-09-27)
- **إغلاق Phase 5.1 بلا commit**، وحذف ملفَّي المشهد ⇒ لا كود ميت في المستودع.
- **لا أرقام أداء** — «لا تراجع صامت» تنطبق على التجارب أيضاً، لا على الكود فقط.
- **لا تعديل محرك** (اتساقاً مع KI-001 / KI-002).

### لماذا B وC لا تُستحقان
تقليل **عدد** الطلبات أو **تخصيص المعالج** لا يمس **الحجم المنسوخ لكل إطار**. البوابة الوحيدة المؤثرة هي تقليل البايتات نفسها، أي مسار رسم بدقة أقل — وهو (أ) ويفرض C++ جديداً وتغيير بكسلات ⇒ **milestone مستقل**، لا Phase 5.1.

### التالي
- **015.5 Final** (إغلاق الـmilestone عند **5/8 معايير** + ثلاث KI موثّقة).
- milestone مستقل لاحق: تقليل `bytes_copied_per_frame` عبر مسار رسم بدقة أقل.


## KI-007: HZB Uses AABB Occluders (Not Scene Depth)

**Date:** 2026-09-28
**Status:** By design (012-revised scope)
**Severity:** Medium (deferred to 019)
**Owner:** GNE Architecture

**Details:**
- 012-revised activates HZB occlusion.
- Pyramid is built from AABB occluders via
  `gpu_hzb_set_occluders`.
- Real scene depth (009 `raster_viewz_texture`)
  is NOT used.
- `gpu_hzb_depth_source` is compiled but never dispatched.

**Impact:**
- Real geometry occlusion (walls, terrain) not covered.
- False-occlusion risk for complex scenes.
- Lighting (018) may need real geometry for tests.

**Deferred To:**
- 019 (Shadows + Real Depth HZB).
- Target: before 020.

**Evidence:**
- 012-revised closure: p1=6, p2=0 with AABB occluders.
- Pyramid non-empty (245,520 texels).

**Resolution Path:**
- 019: activate `gpu_hzb_depth_source` + scene depth feed.
- Requires: geometry pipeline + depth source binding.

## 015.5 C6 status (closed in 015.6)

**الحالة:** مقبول + موثّق (double-buffering مؤجّل إلى 020) — "تقليل الحجم" يكسر DET.
**خط الأساس المقاس (2026-09-28، main_016، wall-clock عملية كاملة تشمل الإقلاع):**
- 5 تشغيلات (ثوانٍ): 8.76، 7.58، 8.19، 7.57، 7.41 — كلها exit=0.
- median 7.58 · p95 ≈ 8.76 · max 8.76 · min 7.41.
- البايتات/التشغيل: 7 قراءات × 8,294,400 = **58,060,800 B** (~55.4 MiB، بكسل فقط بلا عمق).
- مُوسم صراحةً: wall-clock عملية، **ليس** زمن GPU (قاعدة KI-001).
