# Contract 015.5 — Data

نموذج البيانات للـResource Pool. مصطلحات ومقادير مقيسة من الكود القائم عند `56849fa`.

## 1. الكيانات

| الكيان | الوصف | الحجم/الوحدة | مرجع |
|---|---|---|---|
| `PoolBlock` | كتلة مُخصَّصة: offset + bytes + عمر `[first_pass, last_pass]` + tag + free/used | 32 B (مقترح CPU) | SPEC §3.1 |
| `PoolExtent` | مدى حر داخل البليت قابل لإعادة التخصيص (free-list) | 16 B | SPEC §3.1 |
| `PersistentEntry` | تخصيص دائم (meshes/draw records/HZB/scene DB) | 24 B | SPEC §3.2 |
| `PoolHeader` | جذر واحد CPU: magic/version/capacity/used/peak/policy | 64 B | — |

## 2. عمر المورد (مفتاح correctness الـaliasing)

العمر يُحدَّد **بالـpass index** لا بالزمن: مورد يُنتَج في pass `p` ويُستهلك حتى pass `q` ⇒ عمره `[p, q]`. موردان يتشاركان الإزاحة **إلا إذا** كان `[a1,b1] ∩ [a2,b2] = ∅`. (نفس القاعدة المستخدمة في 015 — لكن 015 لا يطبّقها على ذاكرة، انظر SPEC §2.1.)

## 3. القواعد الملزِمة
- **alignment**: كل تخصيص يُحاذى على 16 B (STD430/vec4) — misalignment = خطأ، لا تصحيح صامت.
- **حتمية التخطيط**: نفس قائمة `(tag, bytes, first, last)` المرتَّبة ⇒ نفس `offset` لكل كتلة. أي فرز أو تكرار يغيّر التخطيط = انزياح توقيع 015.5.
- **لا تخصيص بحجم صفر**؛ ولا تخصيص بأقل من granule (يُختار في Phase 2).
- **الـcapacity ثابتة** بين عمليتين (لا نمو داخل عملية)؛ النمو = عملية جديدة (§3.3).

## 4. ما لا يدخل الـpool (خارج العقد)
`raster_color_texture` و`raster_depth_texture` و`raster_viewz_texture` و`hzb_prod_array`: نُسخ مُهيكلة (attachment/storage) تُدار كـRID مستقل اليوم.纳入 الـpool شرطٌ في Phase 5 (§9/D1) وليس في 015.5.
