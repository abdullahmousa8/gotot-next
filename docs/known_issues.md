# GNE — Known Issues

قيود معروفة **مُقيسة** في هذا البناء، مسجَّلة حتى لا يُعيد أحدٌ اكتشافها من الصفر. كل بند يحمل دليله (file:line أو قياس حيّ)، ولا يُسجَّل افتراض.

---

## KI-001: GPU Timestamps غير متاحة على Vulkan

**التاريخ:** 2026-09-27 · **الحالة:**Unavailable — قيد في محرّك Godot (خارج نطاق GNE)
**الأثر:** GNE لا يستطيع قراءة زمن GPU على الواجهة الخلفية Vulkan. لذلك يقيس Phase 5 على **wall-clock** حصراً.

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
