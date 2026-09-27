# Contract 012 — Determinism (الحتمية والـregressions)

| القاعدة | التفصيل |
|---|---|
| التوقيع content-only | `sig=` يحتوي **محتوى فقط** (`levels`, `p1`, `p2`, `coherent`, draw counts). **لا أزمنة** (درس `dispatch_us/draw_us` في 011 و`drift_us` في Phase 4 ⇒ التوقيع صار content-only + سطر `timings` منفصل) |
| حتمية العدّ | `p2` عبر `atomicAdd` ⇒ ترتيب غير محدود **لكن الناتج حتمي** (جمع) |
| حتمية الترتيب | `compact[]` تصاعدي المعرّفات ⇒ لا اعتماد على ترتيب الـworkgroups |
| حتمية الهرم | downsampling حتمي (max على 2×2)، والكتابة `max` لا overwrite ⇒ النتيجة لا تعتمد علىRace |
| ضابط سالب | `main_012` المخطط لـ`rc=123` في البوابات؛ إن صار `rc=0` فالـharness كاذب (درس Start-Process/ExitCode) |

## البوابة الإلزامية بعد التنفيذ
1. `gt_harness` كاملاً ⇒ **9 ok / 0 bad / 1 xfail**.
2. `gt_regress` ⇒ **`GT_REGRESS: PASS`**.
3. تواقيع **حرفية**: `013 f3106528256` · `014 v14|…|d1` · `015 v15-pc9-…-t18616` · `v15-pr1` · `v15.5-p4`.
4. **صفر** `ERROR:` و**صفر** `RID allocations of type`.
5. المشهد الجديد: `sig=v12-…` ب.RunStable بين تشغيلين (نمط `gt_011`).

## ما يُبلَّغ إن فشل
أي انزياح توقيع ⇒ **STOP**، يُبلَّغ مع السطرين المتعارضين حرفياً — لا «تصحيح» للتوقيع ولا تعديل قيمة مُقاسة.
