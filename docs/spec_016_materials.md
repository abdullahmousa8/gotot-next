# GNE-016 — Materials (SPEC DRAFT)

**الحالة:** SPEC 016 v1.0 FINAL — معتمد من المعماري (2026-09-28).

**Resolved (D4-1..D4-4):**
- D4-1: L = normalize(-0.5, -1.0, -0.5); ambient = 0.1
- D4-2: signature = v16|mc|Lx|Ly|Lz|amb|hp|hr|d
- D4-3: histogram threshold = hr >= 0.3
- D4-4: backface emission = NONE (default)
**المرجع:** docs/progress_report.md (§30=015.5، §31=012-revised) + docs/spec_015_5_resource_pool.md (نمط الصياغة) + modules/gne_render/gne_render_server.* (مبنى الوحدة عند `d499c53`).
**المرحلة السابقة:** إعادة التسمية Godot-Next-engine/GNE مدفوعة إلى `origin/main` عند `d499c53`، `GT_REGRESS: PASS`.

---

## 1. الهدف

تحويل **اللون المسطّح لكل mesh** إلى **سجلّ مادة حقيقي** يُقيَّم على الـGPU، دون كسر أي مسار قائم:

1. **Material records حقيقية** — لكل mesh slot (≤64): albedo + roughness + metallic + emissive + flags، في مخزن GPU يُقرأ ويُكتب ويُتحقق.
2. **Flat-shading normals** — لا يوجد normal attribute في صيغة الـvertex (stride 12 بايت، position فقط: cpp 3756)؛ تُشتق normals الوجوه في الـfragment عبر `dFdx/dFdy` — صيغة الهندسة **لا تُمسّ**.
3. **استجابة للضوء** — ثابت اتجاهي واحد + ambient، يُقيَّم في مسار المادة فقط (إثبات أن المواد تتفاعل مع الضوء؛ **نظام** الإضاءة الكامل هو 018).
4. **مسار مضاف بحت** — ممر raster جديد للمادة؛ ممرات الألوان المسطّحة (cpp 420/510/853) تبقى بايتًا ببايت.
5. **حتمية كاملة** — نفس المدخلات ⇒ نفس البكسلات؛ توقيع content-only جديد.

## 2. الخلفية والقيود الموروثة

### 2.1 الحالة اليوم: لون مسطّح بلا مادة
- `mesh_color_buffer` = `vec4[64]` (h 133؛ cpp 3979) يُملأ من `gne_mesh_palette()` (cpp 530) عند تسجيل كل mesh.
- الـfrag sélectionne اللون حسب `v_mesh_id` فقط (cpp 853): `out_color = mesh_colors.colors[v_mesh_id]` — بلا إضاءة، بلا خشونة، بلا انبعاث.
- التعليقان المرجعيّان: «No lighting, no materials, no textures» (cpp 501) و«No normals, no UVs, no materials, no textures» (cpp 3721).

### 2.2 حدود الخارطة: 016 ليس 017 ولا 018
| Milestone | يملك |
|---|---|
| **016 (هذا)** | سجلّات المادة + ربطها + تقييم أحادي-الضوء (إثبات) |
| 017 | Textures (UV attribute جديد + sampling) — خارج النطاق هنا |
| 018 | نظام الإضاءة (ظلال، أضواء متعددة، إدارة الضوء) — خارج النطاق هنا |

أي ذكر لـtextures أو shadow maps أو multi-light في 016 = خروج نطاق يُرفض في المراجعة.

### 2.3 القيود الملزِمة الموروثة
- **انزياح التوقيع ممنوع**: `f3106528256`، `v14|c18616|…|d1`، `v15-pc9-…-t18616`، `v15-pr1`، `v12-rev levels=10 p1=6 p2=0 ctrl=6` تبقى حرفية.
- **صيغة الـvertex لا تُمسّ**: stride 12 بايت يبقى؛ أي normal جديد يعني كسر هندسة 008A/010/011 ⇒ مرفوض.
- كل تغيير سلوكي ⇒ **بناء + 013/014/015/015.5 + `gt_harness` كاملاً + الضابط `main_012 → XFAIL`**.
- Test-only أولاً؛ لا دمج في `RenderingServer` ولا RHI.
- 012-revised مغلق ولا يُمسّ؛ 017/018 لا تُستبق.

## 3. التصميم المقترح (مفتوح للتحسين في المراجعة)

### 3.1 سجلّ المادة (64 بايت = 4×vec4 لكل slot، 64 slot = 4096 B)
| vec4 | x | y | z | w |
|---|---|---|---|---|
| m+0 | albedo.r | albedo.g | albedo.b | roughness ∈ [0,1] |
| m+1 | emissive.r | emissive.g | emissive.b | metallic ∈ [0,1] |
| m+2 | specular.r | specular.g | specular.b | shininess (>0) |
| m+3 | flags (bit0=emissive_on) | emissive_strength | emission_backface | pad |

- القيم خارج `[0,1]` لـroughness/metallic ⇒ `print_error` + رفض الكتابة (لا clamp صامت).
- الافتراضيات: `shininess=32.0`، `specular=(1,1,1)`، `emission_backface=0` (D4-4).
- مرآة CPU (`GneMaterial[64]`) + `buffer_update` لكل كتابة؛ القراءة الراجعة جزء من المعيار 1.

### 3.2 اشتقاق الـnormals (flat shading)
- ممر المادة الجديد يمرّر `world_pos` (varying) + `mesh_id` (flat).
- الـfragment: `N = normalize(cross(dFdx(wpos), dFdy(wpos)))` مع توحيد الاتجاه نحو الكاميرا (`faceforward`).
- **لا attribute جديد، لا تغيير stride** ⇒ أدلة 008A/010/011 الهندسية untouched بالبناء.

### 3.3 نموذج التقييم (أحادي، حتمي، صادق — Lambert + Blinn-Phong)
- ضوء اتجاهي **ثابت واحد** (push constant، يُثبَّت في المشهد ولا يتغيّر بين تشغيلي DET):
```glsl
const vec3 L = normalize(vec3(-0.5, -1.0, -0.5));
const float AMBIENT = 0.1;
```
`light.color` الافتراضي أبيض `(1,1,1)`.
```glsl
// Diffuse (Lambert)
float NdotL = max(dot(normal, L), 0.0);
vec3 diffuse = material.albedo * NdotL * light.color;

// Specular (Blinn-Phong)
vec3 V = normalize(camera_pos - world_pos);
vec3 H = normalize(L + V);
float NdotH = max(dot(normal, H), 0.0);
float spec = pow(NdotH, material.shininess);
vec3 specular = spec * material.specular_color * light.color;

// Final
vec3 color = AMBIENT * material.albedo + diffuse + specular + material.emission;
```
- ربط roughness/metallic (حتى يبقى المعيار 5 قابلًا للتحقق): `spec *= (1.0 - 0.5 × roughness)`، ولون الـspec الفعّال `mix(specular_color, albedo, metallic)`.
- قاعدة الـbackface (D4-4): `!gl_FrontFacing` ⇒ diffuse/specular = 0، والانبعاث يُطبَّق فقط إذا `emission_backface > 0` (الافتراضي 0 = NONE)؛ مواد 016 opaque فقط (الشفاف → 017+).
- هذا **ليس PBR كاملاً** (لا IBL، لا Fresnel فيزيائي، لا ظلال) — عمدًا: إثبات استجابة المادة بأقل آلية صادقة، والنظام الكامل لـ018.

### 3.4 مسار مضاف (additive pass)
- pipeline/vertex/fragment/shader جديد كليًا باسم `*_mat_*`؛ الـuniform sets الجديدة لا تشارك مجموعات الممرات القديمة.
- ممرات 005/008A/010/011 تبقى على shadersها القديمة حرفيًا ⇒ صورها بايتًا ببايت.

### 3.5 التوقيع (مُثبَّت — D4-2)
- الصيغة: `v16|mc|Lx|Ly|Lz|amb|hp|hr|d` — content-only، بلا timings (درس 015.5 §5.3.3).
- مثال: `v16|mc=8|L-0.41|-0.82|-0.41|amb=0.10|hp=245|hr=0.85|d1`
- المكونات: `v16` الإصدار؛ `mc` عدد المواد؛ `Lx/Ly/Lz` اتجاه الضوء (رقمان عشريان)؛ `amb` الـambient؛ `hp` قمة الهيستوغرام (أعلى bin)؛ `hr` مدى الهيستوغرام (أقصى − أدنى سطوع)؛ `d` علم الحتمية.

## 4. معايير القبول (PASS — 8 معايير مقترحة)

1. **مخزن مادة حقيقي**: كتابة 8 سجلّات متمايزة + قراءة راجعة مطابقة بايتًا (لا تكفي أرقام CPU).
2. **حتمية**: تشغيلان ⇒ نفس `sig=` حرفيًا (`d1`) + صفر `ERROR:`.
3. **normals وجوه متسقة**: كل وجه لون موحّد (تباين داخل الوجه ≈ 0) — يثبت أن `dFdx/dFdy` يعمل لا أن البكسلات عشوائية.
4. **استجابة N·L**: تدوير `L` بزاوية معلومة ⇒ تتغيّر البكسلات في الاتجاه المتوقع (وجوه كانت مظلمة تُضاء والعكس) — لا مجرد «تغيّرت».
5. **خشونة/معدنية قابلة للقياس (D4-3)**: مادتان بنفس الـalbedo وroughness مختلف ⇒ `hr >= 0.3`، وإلا FAIL (استجابة ضعيفة = الضوء لا يعمل أو المواد flat).
6. **انبعاث مستقل عن الضوء**: بكسل في منطقة مظلمة (`N·L=0`) يحمل لون الانبعاث بدقة عند تفعيل العلم، ويختفي عند إطفائه؛ backfaces بلا انبعاث افتراضيًا (D4-4).
7. **ثبات التوقيعات**: كل تواقيع 013/014/015/015.5/012-rev حرفية + `GT_REGRESS: PASS` + الضابط `main_012 → XFAIL`.
8. **حدود صلبة ونظافة**: slot 64 والـid غير الصالح ⇒ `print_error` + رفض (لا fallback صامت) + صفر `RID allocations of type` في كل التشغيلات.

## 5. تغييرات الواجهات المقترحة (Test-only في البداية)

| API | النوع | الغرض |
|---|---|---|
| `gpu_material_create()` | Test-only | إنشاء مخزن 64 سجلًا + مرآة CPU |
| `gpu_material_set_albedo(id, color)` | Test-only | كتابة albedo (يرفض خارج النطاق) |
| `gpu_material_set_params(id, rough, metal)` | Test-only | خشونة/معدنية ∈ [0,1] (يرفض خارجها) |
| `gpu_material_set_specular(id, color, shininess)` | Test-only | لون الـspec + `shininess` (الافتراضي 32.0) |
| `gpu_material_set_emissive(id, color, strength, on, backface)` | Test-only | انبعاث + علم + `emission_backface` (الافتراضي 0) |
| `gpu_material_readback(id)` | Test-only | السجل كما في GPU (معيار 1) |
| `gpu_material_set_light(dir)` | Test-only | اتجاه `L` الثابت للمشهد |
| `gpu_material_stats()` | Test-only | `{slots_used, bytes, dispatches}` — قاموس مستقل (لا يُضاف إلى `gpu_scene_manager_get_stats` ولا `gpu_rg_get_stats`) |

قاعدة: لا تُضاف أي قراءة مادة إلى قارئات DET القائمة — قاموسا `gpu_scene_manager_get_stats` و`gpu_rg_get_stats` يُغذّيان قارئات DET (`main_014.gd` يستدعي الأول في L210/245/283/426)، فأي مفتاح جديد فيهما انزياح توقيع.

## 6. نطاق خارجي (Out of scope لـ 016)
Textures/UV (017) · نظام إضاءة: ظلال/أضواء متعددة/إدارة ضوء (018) · PBR كامل/IBL · مواد شفافة · تجاوز per-instance (لاحق) · تعديل `RenderingServer` · RHI جديد · أي تغيير على مسارات 001A–015.5 الهندسية أو ممراتها.

## 7. التسليمات
- **SPEC**: هذا الملف + 5 وثائق عقد: `contract_016_data|buffers|lifecycle|boundaries|tests.md`.
- **C++**: `gpu_material_*` + ممر `*_mat_*` + harness `gt_016a` (تشغيلان + مقارنة `sig=`، نمط `gt_015a`).
- **Demo**: `demo/gpu_smoke/main_016.gd|.tscn` (8 نسخ: 4 cube + 4 octahedron، كاميرا 1700، 8 مواد متمايزة red/green/blue/yellow/cyan/magenta/white/gray + تدرج metallic/roughness، عرض 3/4 والضوء من (-0.5,-1.0,-0.5)).
- **الدليل**: `sig=v16|mc|Lx|Ly|Lz|amb|hp|hr|d` + جدول الهيستوغرام + قبل/بعد المسار المسطّح.
- **الملفات المحمية**: `sac_unblock_procedure.md`، `open_source_system_strategy_v1.md`، `dependency_register.md` — لا تُلمس إلا بأمر مؤكد.

## 8. ترتيب التنفيذ المقترح
1. **Phase 0** (هذا الـSPEC): مراجعة المعماري + تثبيت.
2. **Phase 1 (مخزن)**: `gpu_material_*` + readback (معيارا 1، 8 جزئيًا) — بلا ممر رسم بعد.
3. **Phase 2 (ممر)**: `*_mat_*` + normals + تقييم (معايير 3–6).
4. **Phase 3 (بوابات)**: DET + كل التواقيع + `gt_harness` + `gt_regress` (معايير 2، 7، 8).
5. **Phase 4 (Gate + Push)**: توثيق FINAL + commit.

## 9. القرارات — محسومة (D4-1..D4-4)
| # | القرار | القيمة |
|---|---|---|
| D4-D1 | اتجاه `L` الثابت وقيمة الـambient | `L=normalize(-0.5,-1.0,-0.5)`، `AMBIENT=0.1` |
| D4-D2 | صيغة التوقيع النهائية | `v16\|mc\|Lx\|Ly\|Lz\|amb\|hp\|hr\|d` |
| D4-D3 | عتبة فارق الهيستوغرام (معيار 5) | `hr >= 0.3` ⇒ PASS، وإلا FAIL |
| D4-D4 | الانبعاث على backfaces | NONE افتراضيًا (`emission_backface=0`)؛ frontfaces كاملة |

## 10. الحالة
**FINAL v1.0 — معتمد من المعماري (2026-09-28).** لا implementation ولا build ولا commit للكود قبل push authorization. (هذا الـcommit = توثيق SPEC فقط.)
