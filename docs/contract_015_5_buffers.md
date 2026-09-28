# Contract 015.5 — Buffers

كل مخزن/نسيج يملكه الـpool، بأرقام **مُقيسة** من الكود (`RASTER_TARGET_W/H = 1920/1080` عند `gne_render_server.h:77-78`، `HZB_PROD_TEXEL_COUNT = 2048` و`HZB_PROD_LEVELS = 12` عند 207-208).

## 1. المخازن المقترحة (Phase 2/3)

| buffer | حجم مبدئي | usage | الغرض |
|---|---|---|---|
| `pool_buffer` | يُختار في Phase 1 قياساً | `STORAGE | COPY_SRC | COPY_DST` | البليت المؤقت (bump + free-list) |
| `pool_staging` | مضاعف `pool_buffer` (أو 3 أطر) | `COPY_SRC | COPY_DST` | readback متعدد الإطارات (D1/أ) |
| `persistent_buffer` | سقف معلن (§9/D2) | `STORAGE | COPY_DST` | التخصيص الدائم |

## 2. أحجام مقيسة的资源 القائمة (خط أساس VRAM)

| مورد | الصيغة | البايتات | cpp |
|---|---|---|---|
| `raster_color_texture` | 1920×1080×4 (`R8G8B8A8_UNORM`) | **8,294,400** | 3478 |
| `raster_depth_texture` | 1920×1080×4 (`D32_SFLOAT`) | **8,294,400** | 3495 |
| `raster_viewz_texture` | 1920×1080×4 (`R32_SFLOAT`) | **8,294,400** | 3513 |
| `hzb_prod_array` | 2048×2048×12×4 (`R32_UINT`) | **201,326,592** | 2746 |
| `gms_record_buffer` @1M | capacity×64 | 67,108,864 | contract_014_buffers |
| `gms_snapshot_buffer` @1M | capacity×32 | 33,554,432 | contract_014_buffers |
| `gms_ring_buffer` | 16 MiB | 16,777,216 | contract_014_buffers |
| `gms_active_buffer` @1M | capacity×4 | 4,194,304 | contract_014_buffers |

**إجمالي مرجعي قابل للقياس**: `ssbo=121,635,140` كما يطبعه `gpu_scene_manager_get_stats()` (`ssbo_bytes`) — وهو **مجموع المخازن فقط**، لا يشمل النسيج (نسيج منفصل).

## 3. قواعد الإزاحة (binding)
- كل مورد من الـpool يُربط كـ**مخزن كامل** (`whole`) أو نطاق `offset/size` (إن دعم `RD` ذلك للمنطقة المطلوبة) — **يُتحقَّق في Phase 2**؛ إن لم يُدعم، يُستخدم مخزن كامل per-resource ويُلغى claim الـaliasing.
- `bind set/offset` يُحفظ في جدول resource→(buffer, offset) ولا يُعاد ربطه يدوياً في كل pass.

## 4. إدراج صريح: ما لا يُنقل
- النسيج (attachment/storage) تبقى RID مستقلاً في 015.5.
- `raster_depth_texture` **بلا** `SAMPLING` (ملاحظة 3484-3487: الD32 sampled view مسار معطّل في هذه الشوكة) ⇒ أي تخطيط جديد يقرأ العمق يجب أن يقرأ `raster_viewz_texture` لا الـD32.
