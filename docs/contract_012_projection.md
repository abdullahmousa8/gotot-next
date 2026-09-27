# Contract 012 — Projection (الإسقاط)

ثوابت إسقاط الـinstance → texel. **كل بند مُثبت بطرفين Writer/Reader** (انظر SPEC §3.4).

| البند | القيمة | المنتج (producer) | المستهلك (consumer) | المدى الصالح | شرط الفشل |
|---|---|---|---|---|---|
| View-space depth | `z_view = -(view * vec4(center,1)).z` | occluder 1101-1102 · Reader 1317-1318 | اكتب 1104 · اقرأ 1335 | `z_view > 0` | `z_view <= 0` ⇒ فشل ⇒ conservatively visible ⇒ conservatively visible |
| Inverted depth | `floatBitsToUint(max(far - z, 0))` | 1104 | 1335 | `[0, far]` سالب ⇒ NaN | قيمة خارج المدى ⇒ `print_error` |
| **تطابق التحويل** | `1104 ≡ 1335` **حرفياً** | — | — | — | **أي انحراف = كسر عقد** |
| NDC → texel | `uv = (ndc*0.5+0.5) * texels` | 1134-1137 (للصندوق) | 1332 | `[0, texels-1]` | خارج المدى ⇒ clamp (لا fallback صامت) |
| **تطابق الفهرسة** | `1145 ≡ 1346` (`x*texels + y`) | occluder | Reader | `uint` | **أي انحراف = كسر عقد** |
| Sphere radius px | `r_px = (radius*0.5*vp_h)/(fov*max(z_view,1e-4))` | — | 1319 | `>= 1` | `< 1` ⇒ تخطي فحص (مُعلَن) |
| Level select | `level = clamp(log2(r_px), 0, log2(base))` | — | 1328-1329 | `[0, 11]` | clamp موثّق |

## قاعدة صارمة
**لا تغيير في الإسقاط ما لم يتغيّر سطران متقابلان معاً.** أي «إصلاح إسقاط» منفرد = كسر عقد (وهو ما أفشل v0.1).
