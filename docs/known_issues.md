# GNE — Known Issues

قيود معروفة **مُقيسة** في هذا البناء، مسجَّلة حتى لا يُعيد أحدٌ اكتشافها من الصفر. كل بند يحمل دليله (file:line أو قياس حيّ)، ولا يُسجَّل افتراض.

---

> **Open-items consolidation (2026-09-28):** the single current view of everything
> still open lives in `docs/open_items_register.md`.
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

## KI-001 update (2026-09-30): the pool was never the limit - publication is

**Measured on binary `65D8904D`, 300 frames (`main_015_5_phase4`):** `gpu_capture_count=2100`
(every capture accepted - 1 frame marker + 6 pass marks per frame) and `gpu_result_count=0`
(not one result ever published to a reader); `gpu_last_ns=0` at frames 1/50/100/300; the scene
reports GPU `UNAVAILABLE` with `NA`, never zero cost.

**Two corrections to the record above:**

1. `max_timestamp_query_elements` is `GLOBAL_DEF_RST(..., "256,65535,1"), 256`
   (`core/config/project_settings.cpp:1811`) - its DEFAULT IS 256, so the query pool has always
   existed and always accepted captures. "The pool is 0 unless the project enables it" is not true
   for this tree. It is still runtime-only and read once in `RenderingDevice::initialize()`
   (`rendering_device.cpp:8625`), so a module must set it before creating a device if it wants a
   different size; GNE now merely prints the effective value and guards a sub-minimum.
2. The single blocker is publication: `timestamp_result_count` is published only in
   `RenderingDevice::_begin_frame()` (`rendering_device.cpp:8342`). It is reachable in principle from
   a readback (the staging path at 1081 calls `_flush_and_stall_for_all_frames()` with the default
   `p_begin_frame = true`, `rendering_device.h:1902`), so "a local device never reaches
   `_begin_frame()`" is NOT proven and is withdrawn. Measured instead: over 300 frames with 2100
   captures, **no frame ever saw a result** (`gpu_result_count=0`, `gpu_result_count_max=0`). Candidate
   mechanism, with lines: the pool prefers growing the download staging buffer (1052-1054) or
   `STAGING_REQUIRED_ACTION_STALL_PREVIOUS` (1062) over the publishing
   `STAGING_REQUIRED_ACTION_FLUSH_AND_STALL_ALL` (1037). The earlier note that `drivers/vulkan`
   "has no `utilities.cpp`" pointed at the wrong file: the gles3 driver keeps its own copy of that
   logic; the Vulkan path publishes through the core `rendering_device.cpp`.

**Evidence surface:** `gpu_frame_stats()` now returns `gpu_capture_count`, `gpu_result_count` and (in the phase4 scene report) the run maximum `gpu_result_count_max`,
and `ensure_gpu_device()` prints the effective pool size at device creation, so this limit is
checked by any run instead of being asserted here.

**Closure condition (unchanged in kind, now precise):** one engine-side change - (a) force the publishing staging action or expose/trigger
`_begin_frame()` for local devices, (b) resolve the pool in `submit()`/`sync()` for local devices,
or (c) drive the local device through the engine frame flow. All are edits inside `godot-master`
and require an explicit owner decision; until then GPU columns stay `NA`.

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
**Status:** CLOSED 2026-09-29 (owner decision: Option A fix executed - specular gated by NdotL in all three sites: mat dir-light, light dir-light, light cluster loop). Before/after: literals unchanged (v16/v18/v19 identical); goldens re-baselined deliberately (main_018: 5 px, max 1 LSB, all darker; main_019: 5 px, max 1 LSB, all darker - the exact backfacing-specular signature).
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
**Status:** ACCEPTED & DOCUMENTED (owner, 2026-09-30) - closed by decision, NOT fixed. Pre-existing external condition, unchanged by any engine work. **Reopen condition:** any criterion that must gate on a raw visible-count across differing window sizes - the real fix there is to make the scene read a fixed viewport, not to tune thresholds.
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
**Status:** Resolved 2026-09-28 - masking defect fixed; 008b criterion recalibrated (0.35 -> 1.0, physical rationale in-scene); battery green with honest accounting. Onset note (pre-019 HZB candidate) left informational.
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

**Update 2026-09-28 (directive task A completed):** the masking defect is fixed in `tools/gt_regress.bat`: `set FAILED=0` initialization added; the XFAIL branch no longer resets the accumulated flag (`if not "%XFAIL%"=="XFAIL" set FAILED=1`); REM note added. Validated by scenario runs: [008b + 012-XFAIL] -> FAIL (exit 1, no masking), [012-XFAIL only] -> PASS (exit 0), [007 only] -> PASS (exit 0). Post-fix battery re-run shows the true per-scene state; main_008b now surfaces its own rc=75 openly - its disposition is tracked under directive task B.

**Update 2026-09-28 (directive task B completed - 008b severity: ISOLATED):** rc=75 is the scene's own acceptance-criterion threshold, not a shared/critical-path defect.
- Miss set = the entire sampled small-radius band: every miss has projected radius r in [0.41, 0.67] px (positions scattered, no index pattern); the center pixel is background; nearest other green >= 10 px away.
- Counterfactual (diagnostic copy, threshold 0.35 -> 0.75 only): misses 32 -> 1, with 130 low-radius cubes reclassified as subpixel-skips (790 -> 663 checked). Mechanism confirmed: at 1x rasterization a cube below ~1 px can cover zero samples however correct the pipeline is; SUBPIXEL_R_PX=0.35 sits below that limit (true can-vanish radius ~0.9-1.0 px).
- The shared stages pass their own rigor checks concurrently: GPU-vs-CPU cull delta=0, compact set_ok=true, no dropped clearly-visible cubes, drawargs repeat + full-frame repeat deterministic.
- Count variation 20..32 across contexts = the KI-012 window-state class: 1152x648 contexts -> visible 3680 / miss 20; ~1920x1009 contexts -> visible 3778 / miss 32 (windows currently auto-maximize right after open; probe: main_008 with --resolution 1152x648 printed 1152 then 1920x1009). The FAIL outcome is invariant - only the sampled band size moves.
- Onset (rc=0 at the rename sweep, pre-017/018/019 engine): candidates only, not proven - (i) pre-019 AABB-occluder HZB may have (falsely) occluded part of the small far cubes; (ii) run-environment state. Recorded as open.
- Implication for 018-rev: none found - the rev path rides the fixed 1920x1080 cluster grid and the mat_light flow; 008b's criterion does not gate it.
- Disposition (Owner): fix the scene criterion (~1.0 threshold / coverage-aware) or mark 008b deliberately as XFAIL (visible, by design). No engine change proposed.

**Update 2026-09-28 (disposition executed - RESOLVED):** Owner decision: raise the criterion, not XFAIL. Applied in `main_008b.gd`: `SUBPIXEL_R_PX` 0.35 -> 1.0 with an in-body physical rationale (1x point sampling: a silhouette below the half-diagonal bound sqrt(2)/2 ~= 0.7071 can cover zero samples by phase; the analytic r_px estimate carries orientation/scale scatter - a 0.77 px cube still missed at 0.75 - so the round full-pixel bound 1.0 is used, matching main.gd's "< 1 projected px may legitimately rasterize zero pixels" note). Validation: scene rc=0 / miss=0 (checked=533, subpixel=260); full battery re-run: all EIGHT harnesses rc=0 with honest accounting (GT_REGRESS: PASS; 012 XFAIL reported, not masking); cross-run classification audit: no other scene changed (reg_main_011/013/014/015.txt and gt015/016/017/018/019 sigs byte-identical; other consoles identical after timing/handle normalization). Retained instrument: `demo/gpu_smoke/main_008b_dbg.gd|.tscn` (documented mirror + per-miss coverage diagnostics; keep in sync with main_008b.gd).

**Update 2026-09-30 (disposition executed - ACCEPTED & DOCUMENTED, no code change):** Owner ruling on the register row that had this OPEN pending "fix demo window sizing OR accept + document": accept and document. Rationale as given - the item is a pre-existing external condition that does not touch the gne render path and is unrelated to the cluster-lighting line closed this session, so closing it by documentation ends the pending state without fabricating an unrequested engine fix.

**What this acceptance does and does not mean.** NOTHING WAS FIXED. The behaviour above is still true: demos still read `get_viewport().get_visible_rect().size` and still land it in `viewdata.viewport`. The stated boundary survives into the closure - affected values are printed counts and screenshots of spread scenes, and the FAIL outcome is invariant with only the sampled band size moving (miss 20 at 1152x648 vs 32 at ~1920x1009, 3680 vs 3778 visible). Therefore any harness that gates on a visible-count number must record the window size it ran at, or its criterion is not reproducible. That sentence is the actual deliverable of this closure; without it, "accepted" would read as "harmless".

The earlier 2026-09-28 update on this entry closed a DIFFERENT thing - the `main_008b.gd` `SUBPIXEL_R_PX` criterion (0.35 -> 1.0), a real code change, unaffected by this closure.

## KI-014: Cluster Light Lists Under-Cover Corner Pixels (Linear-vs-Euclidean Slice Mismatch)

**Date:** 2026-09-28
**Status:** FIXED behind the 018-rev flag (2026-09-28); flag-off path byte-identical (v18 literal intact)
**Severity:** Medium (latent lighting loss for corner-adjacent geometry; would silently undermine 018-rev gate validity if unfixed)
**Owner:** GNE Architecture

**FACT:**
- The mat_light fragment assigns pixels to depth slices by EUCLIDEAN camera distance
  (`z_view = length(cam - world)`; shared `gne_cluster_index`), while `gpu_light_cull_glsl`
  builds cluster AABBs with a LINEAR-depth slab [z0, z1] (`znear * lratio^(tz/24)`).
  For off-axis pixels linear < euclidean (factor cos(theta)); at screen corners
  cos(theta_min) ~ 0.648 for fov 60 / 16:9.
- Unit-2b cross-check (r0chk selftest mode 3): 10 of 14 corner/edge cases lie OUTSIDE the
  AABB of the cluster the fragment assigns to them; gaps (z0 - z_linear) up to 707 units.

**OBSERVATION / IMPACT:**
- A light sphere touching such a corner pixel can miss the AABB and be excluded from the
  cluster list - contradicting the cull's "may over-include, never wrongly exclude" claim.
  Latent in 018's own gate scenes (their probes avoided the exposed corners), but it would
  sit UNDER any future cone-culling validity gate (R1, dc): comparing two equally deficient
  results could pass silently.

**ROOT CAUSE:** slice-semantics mismatch - fragment (euclidean distance) vs cull AABB (linear depth).

**FIX (flag-gated, applied):**
- `gpu_light_cull_glsl`: `bmin.z = z0 * cosmax` with
  `cosmax = 1 / sqrt(1 + tanv^2 * (1 + aspect^2))`, active only when the rev flag is set
  (new push-constant field x); flag-off evaluates the exact old z0, keeping 018
  byte-identical (gt_018a/gt_019a PASS with literal v18/v19 signatures).
- Analytic proof (selftest mode 4 / F6): the bound matches an independent out-of-engine
  computation across 6 FOV/aspect configurations (worst relative error 3.2e-8) and is
  TIGHT (min over dense pixel samples of (z_linear - bound) = 0.0; the old bound is
  violated by 1128..3146 samples per case). Not a fitted constant.
- Flag API: `gpu_light_set_normal_cone(bool)` (default false); measurement-only env
  override `GNE_REV_CONE=1` (unset in every gate).

**dc-impact measurement (018 scene, same binary, flag off vs on):**
- assignments 11348 -> 12363 (+8.94%); clusters_touched 2841 -> 3091 (+8.80%);
  overflows (16-cap) 0 -> 13. On 019 the signature and checks are unchanged off/on.
- Consequence for D8-rev-5 (dc >= 10%): cone-filter savings will be measured against the
  CORRECTED base (base grew ~9%); the criterion itself stays as contracted.

**GNE RELEVANCE / scope note (why fixed inside 018-rev):**
- Pre-existing 018 defect (linear z-slicing), fully independent of cone-culling logic,
  discovered by 018-rev rigor. Contrary to the usual separation policy (cf. KI-011), the
  fix ships INSIDE 018-rev scope because it is a precondition for the validity of
  018-rev's own gates - not because it is convenient to merge.

**Evidence:**
- temp\opencode\b008\r0chk_f6_run2.log (F6 rows); m018_off.log / m018_on.log (stats);
  docs/rev_slab_gap_note.md (mechanism derivation).

**R1 closure addendum (2026-09-28):** final canonical numbers on `main_018_rev`:
slab=12363 -> on=12350 assignments; overflows 0 -> 9; dc=13; d1==d2 signature
`v18-rev|lc=20|cc=3091|dc=13|ot=9|dp=2558|d1`. (The earlier env-override
measurement on main_018 read overflows 0 -> 13; both are recorded; the rev-scene
number is canonical for 018-rev.) The over-cap clusters stay loud via the `ot`
field in the rev signature; the OFF path remains ot=0. Overflow semantics / cap
policy re-examination is parked with the dense-light scenario. Status unchanged:
FIXED (gated), verified in R1.

## KI-015: Hardware Ray Tracing Unusable - RT Pipeline Creation Fails (Fork-Level)

**Date:** 2026-09-28
**Status:** Open - fork/engine-level blocker; NOT GI-specific (any future RT feature hits it)
**Severity:** Medium (blocks hardware-RT features: GI S3 backend, future RT reflections; compute-only paths unaffected)
**Owner:** GNE Architecture / engine-fork owner

**FACT:**
- R0-RT isolation test (spec_022 section 7) proved the RD RT stack works up to and including:
  RT capability detection + feature enablement at device creation, BLAS/TLAS create+build,
  RT GLSL raygen/miss/closest-hit compilation, shader creation, SBT create/range handling,
  acceleration-structure uniform binding.
- `vkCreateRayTracingPipelinesKHR` fails with `VK_ERROR_INITIALIZATION_FAILED` (-3) in
  `RenderingDeviceDriverVulkan::raytracing_pipeline_create`
  (godot-master/drivers/vulkan/rendering_device_driver_vulkan.cpp:6677). Reproduced across
  runs; the pipeline-cache hypothesis was tested (cache-disabled run) and eliminated.

**ROOT CAUSE:** NOT diagnosed. Candidates (undistinguished): SPIR-V post-processing of RT
stages inside the fork; pipeline-layout stage-flag construction for RT pipelines; a
driver-level condition on this pipeline configuration.

**IMPACT / GNE RELEVANCE:**
- Blocks any hardware-RT feature on this fork (GI S3, future RT reflections/ray queries).
- Does NOT affect raster/compute pipelines, compute-only GI (S1/S2), or any existing gate.

**NEXT (not scheduled):** engine-level diagnosis requires dedicated engine work outside the
current boundaries. Until then: hardware RT = unavailable; designs must not assume it.

**Evidence:** temp rt0_run1.log / rt0_run2.log / rt0_verbose.log; code references above.
## KI-007 closure update (2026-09-30)

**Status: CLOSED via GNE-019 (real depth HZB, `rd=1`) - the entry above is kept as the design record.**
The 019 gate proves the real-depth path feeds occlusion (`rd=1`, `pcount=41498`, literal
`v19|lc=20|sm=5|cs=4|rd=1|hr=1.00|d1`).

**Measured boundary, added 2026-09-30 (progress section 61):** the separate raster-depth
*feed* path (`gpu_hzb_depth_feed` writing `hzb_pyramid_data_buffer`) still cannot influence the
phase-2 decision, because `gpu_visibility_prod_dispatch()` clears that buffer and rebuilds it
from the registered AABB occluders. With no occluders the fed depth is erased (`pyr_nz_l0=0`) and
the occluded TARGET stays visible (`p2=3`), in both orderings tested; with an AABB occluder and no
feed the TARGET is correctly occluded (`p2=2`, `pyr_nz_l0=47560`). So AABB occluders remain the
only effective producer in the 012-revised path, and 019 is what carries real depth. Audit scene:
`demo/gpu_smoke/audit_012_depthfeed.gd`.

---

## 015.5 C6 status (closed in 015.6)

**الحالة:** مقبول + موثّق (double-buffering مؤجّل إلى 020) — "تقليل الحجم" يكسر DET.
**خط الأساس المقاس (2026-09-28، main_016، wall-clock عملية كاملة تشمل الإقلاع):**
- 5 تشغيلات (ثوانٍ): 8.76، 7.58، 8.19، 7.57، 7.41 — كلها exit=0.
- median 7.58 · p95 ≈ 8.76 · max 8.76 · min 7.41.
- البايتات/التشغيل: 7 قراءات × 8,294,400 = **58,060,800 B** (~55.4 MiB، بكسل فقط بلا عمق).
- مُوسم صراحةً: wall-clock عملية، **ليس** زمن GPU (قاعدة KI-001).

**020 unit-3 addendum (2026-09-28):** the parked double-buffering item was tested at
module level (see spec_020 section 14): deferred ping-pong readback = no measurable
effect (medians 7795 vs 7890 us, 5 runs each); sync removal = crash (invalid in this
fork). The per-call readback floor is engine-side; both experimental paths were removed
after measurement. C6 remains closed-as-accepted; zero-copy (RHI-level) stays a future
item.

---

## KI-017: HDR (pre-tonemap) radiance readback - indirect light invisible on the 8-bit chain

**Date:** 2026-09-29 opened / 2026-09-30 CLOSED (DONE) - **Severity:** Medium (instrument, not a renderer defect) - **Owner:** GNE Architecture

**Observed (FACT):** shading is tonemapped before the raster readback, so the GI
contribution cannot be read on the 8-bit chain: a directly lit surface reports
`1.0 -> 1.0` and an umbra probe reports `0.8 -> 0.8` (delta exactly 0.0). GNE-023
recorded this as "I2 VOID by saturation" instead of claiming a pass on a saturated
number.

**Resolution:** a 5th `R32G32B32A32_SFLOAT` attachment on the raster framebuffer plus
`gpu_raster_read_hdr(x, y)` reads pre-clamp radiance. Measured on `main_023` at the
same pixel where the 8-bit chain saw `1.0 -> 1.0`:
`h0 = 1.62763388951619 -> h1 = 2.33263762791952, gain = +0.70500373840332`.
Gated by `gt_023a` in `tools/gne_verify.ps1` (numeric: `sane=True`,
`|h1-h0-gain| <= 1e-9`, `gain >= 0.01`).

**Second and larger use (GNE-022b, 2026-09-30):** on the 256-light city scene the 8-bit
umbra delta is 0.0 (same saturation) while the HDR delta is `+32.9104452133179` on the
umbra probe and `+55.1367074549198` on the lit face, **byte-identical across two
separate processes**. The 022b gate is built on this instrument.

**Proof the instrument is not reading its own noise (added with the 022b gate):** two
derived floors must be EXACTLY 0.0 - the same-frame re-read (`repeat`) and the same draw
with `gpu_gi_enabled_set(false)` (`neg`; the fragment gates the field on
`params.gi_params.x` at `gpu_mat_light_frag_glsl:2152`, written 0 at cpp:9452). Both
measured 0.0, so `dh_u > max(repeat, neg)` reduces to a separated measurement instead of
a tuned threshold.

**Evidence:** `%TEMP%\opencode\m022b_*.log`, `gt022b_full.txt`;
`docs/note_023_gi_shadow_integration.md`; `tools/gt_023a.bat`; `tools/gt_022b.bat`;
progress sections 53, 54, 59, 60.

## Monitoring note (2026-09-29): transient silent EXIT=-1 on first run of a fresh binary

**Observed:** one silent process death (EXIT=-1, no `ERROR:` line, no FAIL line) mid-run of
`main_018` on the first execution of a freshly linked binary carrying the KI-017
5th-attachment change. Environment was quiet at the time (no neighbor GPU load, no
nvlddmkm/TDR events in the logs). An immediate retry of the identical command passed
fully (`v18|lc=20|cc=2841|ot=0|hr=0.94|d1`, EXIT=0), as did the full CVS afterwards.
**Status:** watch item only, NOT a numbered KI — single occurrence, unreproduced, no root
cause claimed. Pattern resembles the historical first-run flakes (Lesson 5 class:
silent kills with a quiet event log), hence recorded rather than dismissed.
Re-open as a KI only on second occurrence with logs attached.
