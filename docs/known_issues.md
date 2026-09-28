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

## KI-011: Specular Not Gated By NdotL (Pre-Existing)

**Date:** 2026-09-28
**Status:** Open - pre-existing gap; deliberately NOT part of 018-rev (Architect directive)
**Severity:** Low-Medium (grazing/backlit specular energy; no DET/buffer impact)
**Owner:** GNE Architecture

**Details:**
- The material fragment light loop does not gate the specular term by NdotL; the diffuse term uses max(dot(N,L), 0) but the specular add is unconditional.
- A light with NdotL <= 0 for a surface can still contribute specular energy in grazing configurations (H midway between L and V).
- The gap predates 018-rev. It surfaced because 018-rev R1 is the first byte-exact pixel comparison across a light-list change; without separation it would be misattributed to the new cone culling.

**Impact:**
- After a strictly back-facing light is culled, small differences (sub-LSB to small) may appear versus not culling; these belong here, not to 018-rev.
- No effect on determinism, buffers, or any gated milestone output; the source path is unchanged.

**Scope separation (Architect directive, 2026-09-28):**
- Must NOT be fixed inside the 018-rev batch.
- 018-rev R1 (amended): differences flag-off/on must be confined to clusters whose light lists changed; every difference must carry the NdotL <= 0 leakage signature; anything else fails as an over-cull defect.

**Fix path (future milestone; Owner decision required):**
- Option A: gate the specular term by the same NdotL factor (shading change; own SPEC + before/after pixel/DET evidence).
- Option B: keep and document (if an impact study proves magnitudes negligible).
- Impact study first: which configurations show it; measured magnitudes.

**Evidence:**
- Source review 2026-09-28: material fragment light-accumulate loop in gne_render_server.cpp.
- Recorded during 018-rev SPEC review; ticket opened per Architect directive (this file).

## KI-012: Demo Window Size Is Externally Mutable (Pre-Existing)

**Date:** 2026-09-28
**Status:** Open - pre-existing; unrelated to 018-rev
**Severity:** Low (gated DET content proven insensitive; affects only printed counts and screenshots of spread scenes)
**Owner:** GNE Architecture

**Details:**
- Demos obtain their "viewport" from `get_viewport().get_visible_rect().size` (the OS window size) and feed it to `gpu_scene_set_viewport`. That value lands in `viewdata.viewport` and feeds the HZB occlusion pixel-radius math.
- No demo or module code sets the window size or mode (full audit: zero DisplayServer/window-size calls; demo project.godot has no display settings); the window runs at the Godot default 1152x648 unless changed externally.
- Source located and REPRODUCED: an external maximize of the game window mid-run flips the very next read to 1920x1009 (observed: visible counts jumped 370 -> 402 in the same run). The R0 baseline battery (2026-09-28 17:11) shows the same signature - a mid-run transition that persisted across that battery's processes (an active-session external interaction; not reproducible by any code path).

**Impact (evidence):**
- Affected: marginal occlusion decisions in spread scenes (007/008/008B) -> Visible/Indirect-args/Green-pixels printouts and window screenshot content vary run-to-run (noise-floor: two runs of the same binary differ in exactly these values).
- NOT affected (proven): gated DET signatures. Forced-resolution A/B on the current binary gives byte-identical signatures - main_011 `v128|st0|m64|gc64|dc64|ic64|cc64|dF0.92076|dB0.95204|cb1|dt1` and main_018 `v18|lc=20|cc=2841|ot=0|hr=0.94|d1` at 1280x720 vs 1920x1009. All closed-milestone signature files and CSM dumps matched across the two R0 batteries although the baseline ran with maximized windows and the post battery at default size.

**Recommendations:**
- For future cross-run pixel comparisons on spread scenes: pin the window size (e.g., `--resolution`) or compare fixed-target readbacks only.
- The 018-rev metrics ride the fixed 1920x1080 cluster grid (see main_018 A/B above) and are window-independent; the R7 staleness counter is frame-based and unaffected.

**Evidence:**
- temp\opencode\x\expA2.log (maximize reproduction), x\s011_*.txt, x\s018_*.txt (forced-resolution A/B), temp\opencode\nf (noise floor).

## KI-013: gt_regress FAILED Flag Reset By The XFAIL Scene (Harness, Pre-Existing)

**Date:** 2026-09-28
**Status:** Open - pre-existing harness accounting defect; unrelated to 018-rev
**Severity:** Medium (masked failures - any scene failing BEFORE the main_012 XFAIL call is forgiven)
**Owner:** GNE Architecture

**Details:**
- `tools/gt_regress.bat` line 66 (`if "%BAD%"=="1" if "%XFAIL%"=="XFAIL" set FAILED=0`) resets the accumulated FAILED flag when the XFAIL scene (main_012) is processed, erasing failures recorded by any earlier scene.
- Reproduced deterministically with an isolated copy of the harness: only `main_008b` -> FAILED=1 -> `GT_REGRESS: FAIL` (exit 1); `main_008b` + `main_012 XFAIL` -> `GT_REGRESS: PASS` (exit 0). Debug trace shows failed=1 after 008b and a final PASS because the XFAIL scene cleared the flag.
- Masked consequence today: `main_008b` fails its own spot-check (rc=75, misses 20..32 on a dynamic scene) in every observed battery while the sweep reports PASS. The JEV pilot log (2026-09-27) already records a main_008b det=DIFF anomaly; progress_report recorded an earlier 008B PASS.

**Impact:**
- Scenes ordered before main_012 (currently 007..011) cannot gate the sweep even if they fail.
- Gate-trust defect in the harness; no engine correctness risk.

**Fix path (Owner decision):**
- Make XFAIL handling not touch other scenes' failures (XFAIL should skip only its own failure), then decide main_008b's disposition (fix its spot-check, mark it XFAIL deliberately, or re-scope) and re-run the full battery as evidence.

**Evidence:**
- temp\opencode\r0_dbg_regress.bat / r0_dbg_mask.bat runs (repro), r0_baseline\r0_verdict_summary.txt.

## 015.5 C6 status (closed in 015.6)

**الحالة:** مقبول + موثّق (double-buffering مؤجّل إلى 020) — "تقليل الحجم" يكسر DET.
**خط الأساس المقاس (2026-09-28، main_016، wall-clock عملية كاملة تشمل الإقلاع):**
- 5 تشغيلات (ثوانٍ): 8.76، 7.58، 8.19، 7.57، 7.41 — كلها exit=0.
- median 7.58 · p95 ≈ 8.76 · max 8.76 · min 7.41.
- البايتات/التشغيل: 7 قراءات × 8,294,400 = **58,060,800 B** (~55.4 MiB، بكسل فقط بلا عمق).
- مُوسم صراحةً: wall-clock عملية، **ليس** زمن GPU (قاعدة KI-001).
