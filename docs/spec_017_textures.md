# GNE-017 — Textures (SPEC DRAFT)

**الحالة:** SPEC 017 v0.1 DRAFT — بانتظار مراجعة المعماري وتثبيت النهائي.
**المرجع:** docs/progress_report.md (§32=016) + docs/spec_016_materials.md (نمط الصياغة) + modules/gne_render/gne_render_server.* (مبنى 016 عند `3e158f4`).
**المرحلة السابقة:** 016 مغلق (Full PASS 8/8) ومدفوع إلى `origin/main` عند `3e158f4`، `GT_REGRESS: PASS`.

---

## 1. الهدف

منح مواد 016 **صورًا حقيقية** بدل ألوان albedo المسطّحة، عبر KTX2 + Basis Universal — **offline فقط** — مع بقاء كل التوقيعات حرفية:

1. **KTX2 container** (Khronos) — حاوية mipmaps القياسية.
2. **Basis Universal compression** (ETC1S/UASTC) — تُفك **دون اتصال** وتُخبز أهدافًا جاهزة للرفع.
3. **ربط بالمادة** — كل مادة تشير إلى 5 slots (albedo/normal/metal-rough/AO/emissive) في جدول منفصل (سجلّ المادة 64B **لا يُمسّ**).
4. **Sampling حتمي** — trilinear + mipmaps في ممر `*_mat_*` نفسه (لا ممر جديد).
5. **أدلة بكسل** — الهيستوغرام يُظهر الـtexture فعليًا + DET ثابت (`v17|…`).

## 2. الخلفية والقيود الموروثة

### 2.1 الحالة اليوم: مواد بلا صور
- 016 يقيّم `albedo` لونًا مسطّحًا من السجلّ؛ لا UV attribute ولا sampler ولا صورة في أي ممر (cpp 3721 ما زال صادقًا على الهندسة).
- صيغة الـvertex (12B, position فقط) **تبقى** في 017 — الإحداثيات تُشتق إجرائيًا (triplanar/box-project من `world_pos`) حتى لا تُكسر هندسة 008A/010/011. الـUV attribute الحقيقي لاحق (مع glTF في خط الأصول).

### 2.2 ما قيس في المحرك (2026-09-28، لا تخمين)
| البند | الواقع المقيس |
|---|---|
| Basis Universal | `thirdparty/basis_universal` + `modules/basis_universal` **موجودان** في هذا الفرع |
| KTX | `thirdparty/libktx` + `modules/ktx` **موجودان** |
| Descriptor indexing / bindless في RD | **غير موجود** (`rendering_device.h` بلا أي أثر) ⇒ المسار الوحيد: مصفوفة samplers ثابتة + فهرس dynamically-uniform (قانوني في Vulkan بلا extensions) |
| سابقة العمل دون اتصال | `tools/meshlet_import` (يبني `.gomlet` بـMSVC، والـruntime يقرأ فقط) — نفس النمط يُعاد هنا |

### 2.3 فجوة الـ Audit (مسجّلة بأمانة)
- `docs/open_source_acceleration_audit.md` **لا يحتوي أي سطر** عن Basis/KTX/textures (فُحص 2026-09-28).
- ⇒ ادعاء «Audit v1.0 يوصي بالتكامل» **غير قابل للتحقق من الوثيقة**؛ المضي هنا بسلطة قرار المالك لا بادعاء توافق موثّق. **التوصية:** ملحق audit يغطي الأصول (textures + glTF) قبل FINAL 017.

### 2.4 القيود الملزِمة الموروثة
- **انزياح التوقيع ممنوع**: `f3106528256`، `v14|…|d1`، `v15-pc9-…-t18616`، `v15-pr1`، `v12-rev levels=10 p1=6 p2=0 ctrl=6`، `v16|mc=8|…|d1` تبقى حرفية.
- سجلّ المادة 64B **مجمّد** (عقد 016) — روابط الـtextures في جدول منفصل.
- كل تغيير سلوكي ⇒ **بناء + 013/014/015/015.5/012-rev/016 + `gt_harness` كاملاً + الضابط `main_012 → XFAIL`**.
- Test-only أولاً؛ لا دمج في `RenderingServer` ولا RHI.
- 016 مغلق ولا يُمسّ (قراءة فقط)؛ 018 لا يُستبق.

## 3. التصميم المقترح (مفتوح للتحسين في المراجعة)

### 3.1 صيغة الـTexture (D5-1 مطلوب: preset النهائي)
- حاوية **KTX2** (subset مقيّد: 2D فقط، بلا cubemap/array في 017).
- حمولة **Basis**: ETC1S (نسبة) أو UASTC (جودة) — يُحسم في D5-1.
- تُخبز **دون اتصال** إلى: RGBA8 mipmapped (مسار أول) أو block-compressed (BC7/ETC2/ASTC حسب المنصة، لاحق) — يُحسم في D5-1.
- الـmipmaps جزء من الملف المخبوز (لا توليد runtime في 017).

### 3.2 استراتيجية الربط (D5-2: array ثابت — لا bindless متاح)
- مصفوفة **ثابتة** من `SAMPLER_WITH_TEXTURE` (المرشّح: 8) في مجموعة ممر `*_mat_*`؛ الفهرس من push constant (dynamically-uniform ⇒ قانوني).
- فهرس خارج `[0,N)` ⇒ الـshader يسقط إلى albedo المسطّح + عدّاد `tex_oob` (لا صمت، لا انهيار).
- كل مادة: 5 slots (albedo, normal, metal-rough, AO, emissive) في جدول `mat_tex[64][5]` منفصل (64×5×4B = 1280B).

### 3.3 التكامل مع 016
- سجلّ المادة **لا يتغيّر** (64B مجمّد)؛ جدول الروابط منفصل يُقرأ بنفس `mesh_id`.
- الـfragment الحالي يُمدَّد: `albedo *= texture(sampler_arr[tex[0]], uv_box)` (وأخواتها) — نفس الممر، نفس الـpipeline layout الموسّع بحذر (مجموعة جديدة، القديمة تبقى؟ — يُحسم: مجموعة `*_mat_*` تُعاد إنشاؤها مع bind إضافي؛ ممرات 010/011 لا تُمسّ).
- إحداثيات `uv_box`: إسقاط triplanar مبسّط من `world_pos` (المحور المهيمن من الـnormal المشتق) — حتمي، بلا attributes.

### 3.4 الأداة دون اتصال (D5-1 مطلوب: preset)
- `tools/tex_import/` (نمط `meshlet_import`): يقرأ PNG → Basis → يخبز KTX2+mips → ملف `.gtex`.
- **offline فقط**: لا ربط basisu/libktx في الوحدة؛ الـruntime يقرأ حاوية KTX2 ويرفع الـmips خامًا.
- magic خاص `GNETEX11`؟ — يُحسم في D5-1 (مع درس GOTOML11: الـmagic قرار دائم).

### 3.5 Why .gtex (GNE-owned format)?

**Reasons:**

1. **Runtime format independence:**
   GNE runtime does not parse KTX2.
   KTX2 is used only by offline `tex_import`.

2. **Custom mip-chain metadata:**
   GNE-specific fields: LOD bias, streaming hints, residency flags.
   KTX2 does not support arbitrary metadata.

3. **Direct GPU upload:**
   .gtex is designed for GNE's `gpu_texture_*` upload path.
   No KTX2 re-parse at runtime.

4. **Baked data:**
   Material slots pre-resolved (5 slots per material).
   KTX2 has no notion of "material slot".

**Alternative considered:** Use KTX2 directly at runtime.

**Why rejected:**
- Would require runtime KTX2 parser (larger dep).
- No GNE-specific metadata support.
- Cleaner offline → runtime boundary.
- Audit v1.0: KTX2 = INTEGRATE, but at asset level not runtime.

**Trade-off accepted:**
- Maintenance cost (parser/writer).
- Mitigated: small, stable, versioned format.
- Documented in `contract_017_formats.md`.

## 4. معايير القبول (PASS — 8 مقترحة)
1. ملف KTX2 يُقرأ (magic + mips سليمة، أحجام مطابقة للرأس).
2. حمولة Basis تظهر مفكوكة صحيحة (أول texel = القيمة المخبوزة — دليل الـoffline tool).
3. ترتبط بمادة (slot يُقرأ راجعًا كما كُتب).
4. تُعايَن في الـshader (بكسلات تحمل بصمة الصورة، لا albedo مسطّح).
5. الـmipmaps مطبَّقة (نفس المشهد بدقة عرض مختلفة ⇒ نفس التوقيع — ثبات MIP).
6. الهيستوغرام يُظهر الـtexture (hr أعلى من نسخة albedo-فقط بعتبة تُثبَّت).
7. DET ثابت (`v17|…` متطابق d1/d2).
8. انحدارات 001A–016 PASS + `main_012 → XFAIL`.

## 5. تغييرات الواجهات المقترحة (Test-only في البداية)

| API | النوع | الغرض |
|---|---|---|
| `gpu_texture_load(path)` | Test-only | قراءة `.gtex` + رفع الـmips (يرجع id أو -1) |
| `gpu_texture_bind(material_id, slot, texture_id)` | Test-only | ربط في `mat_tex` (يرفض خارج النطاق) |
| `gpu_texture_get_stats()` | Test-only | `{count, bytes, uploads}` — قاموس مستقل (لا يُضاف إلى قارئات DET القائمة) |

قاعدة: لا تُضاف أي قراءة textures إلى `gpu_scene_manager_get_stats` ولا `gpu_rg_get_stats` ولا `gpu_material_stats` (كلها تُغذّي قارئات DET).

## 6. نطاق خارجي (Out of scope لـ 017)
Texture streaming · virtual texturing · anisotropic filtering · cubemap/array textures · ضغط runtime (كل Basis في الـoffline tool) · PBR كامل جديد · تعديل `RenderingServer` · RHI جديد · أي تغيير على مسارات 001A–016 الهندسية أو ممراتها.

## 7. التسليمات
- **SPEC**: هذا الملف + 5 وثائق عقد: `contract_017_formats|bindless|lifecycle|boundaries|determinism.md`.
- **C++**: `gpu_texture_*` + توسيع مجموعة `*_mat_*` + `mat_tex` + harness `gt_017a`.
- **Demo**: `demo/gpu_smoke/main_017.gd|.tscn` + أصل `.gtex` مخبوز (شطرنج/تدرّج حتمي).
- **الدليل**: `sig=v17|…` + قبل/بعد albedo-فقط.
- **الأداة**: `tools/tex_import/` (offline، MSVC) — **لا تُبنى في CI الخاص بالوحدة**.
- **الملفات المحمية**: `sac_unblock_procedure.md`، `open_source_system_strategy_v1.md`، `dependency_register.md` — لا تُلمس إلا بأمر مؤكد.

## 8. ترتيب التنفيذ المقترح
1. **Phase 0** (هذا الـSPEC): مراجعة المعماري + تثبيت + حسم D5.
2. **Phase 1 (أداة)**: `tex_import` + أصل `.gtex` مرجعي + تحقق يدوي من البايتات.
3. **Phase 2 (لودر)**: `gpu_texture_*` + `mat_tex` + readback (بلا sampling بعد).
4. **Phase 3 (معاينة)**: توسيع frag + triplanar UV + أدلة بكسل (معايير 4–6).
5. **Phase 4 (بوابات)**: DET + كل التواقيع + `gt_harness` + `gt_regress`.
6. **Phase 5 (Gate + Push)**: توثيق FINAL + commit.

## 9. قرارات D5 المطلوبة (غير محسومة — لا تُخمَّن)
| # | القرار | المرشّح |
|---|---|---|
| D5-1 | preset الصيغة | KTX2 + ETC1S مخبوز RGBA8+mips (أولًا)؛ UASTC/BC7 لاحقًا |
| D5-2 | bindless أم array؟ | **array ثابت (8)** — لا descriptor indexing في RD (مقيس) |
| D5-3 | تخطيط الـslots | 5 slots: albedo/normal/metal-rough/AO/emissive في `mat_tex` منفصل |
| D5-4 | وضع الـsampling | trilinear + mips مخبوزة؛ anisotropic مؤجّل |
| D5-5 | streaming؟ | مؤجّل (018+) — تحميل كامل فقط في 017 |
| D5-6 | صيغة التوقيع | `v17\|tc\|…` (العدد + الصيغة) — تُجمَّد في FINAL |
| D5-7 | المعايير الثمانية | أعلاه (§4) — تُثبَّت أرقامها في FINAL |
| D5-8 | خارج النطاق | §6 أعلاه |

## 10. المخاطر + الحالة
| الخطر | التخفيف |
|---|---|
| أدوات KTX/Basis على Windows/MSVC | `tex_import` معزول offline؛ يُبنى مرة ويُحفظ أصله |
| غياب bindless يقيّد العدد | سقف 8 معلن + رفض صريح فوقه (لا صمت) |
| انحدار أداء (ذاكرة/عرض نطاق) | قياس `bytes` + `uploads` قبل أي ادعاء |
| ذاكرة الـmips | سقف معلن في Phase 1 (يُقاس ثم يُثبَّت) |

**الحالة:** DRAFT v0.1 — بانتظار مراجعة المعماري وتثبيت النهائي + حسم D5-1..D5-8. لا implementation ولا build ولا commit للكود قبل الموافقة. (إنشاء هذا الملف = توثيق فقط.)
