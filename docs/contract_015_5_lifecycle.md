# Contract 015.5 — Lifecycle

دورة حياة الـpool: من الإنشاء حتى التحرير، ومتى يُسمح بالتغيير.

## 1. آلة الحالة

```
UNINIT ──create(bytes)──► READY ──begin_frame()──► FRAME
  ▲                        ▲                        │
  │                        │                    alloc/free
  └──── destroy() ─────────┴────────────────────────┤
                                                     ▼
                                    end_frame() ─► (readback/verify) ─► READY
```

## 2. قواعد الإطار (إلزامية)
1. `begin_frame()` **يصفّر** bump cursor ويبني free-list من extentsprevious؛ لا يبقى تخصيص من إطار سابق.
2. `alloc()` **ممنوع خارج `FRAME`** ⇒ `print_error` + return -1 (لا صمت).
3. `end_frame()` يجمّد التخطيط: بعده `alloc` ممنوع حتى `begin_frame` التالي.
4. `destroy()` يحرّر `pool_buffer`/`pool_staging`/`persistent_buffer` **بعد** كل `free_rid` للـuniform sets (درس 010: تحرير الـuniform sets بعد الـbuffers = «free invalid ID» — انظر `progress_report.md` §18).
5. `free(index)` خارج الإطار مسموح فقط للـpersistent (لا للـtransient).

## 3. النمو/إعادة البناء (§3.3)
- `grow-only` داخل الإطار (موصى به): طلب أكبر من المتاح ⇒ `_fail` أو `spill` **حسب D3**، ولا يُوسَّع المخزن بصمت.
- إعادة البناء **بين الإطارات فقط**: `create(new) → copy(old→new) → submit+sync → free_rid(old)`. كل عملية = `pool_rebuilds++` و`bytes_copied += old_size`.

## 4. التفاعل مع حاجز 015
- الـtransient lifetimes مشتقة من `rg_topo` (ترتيب Kahn). المورد-consumer يجب أن يكون في pass **لاحق** للمنتج؛ إن تغيّر الترتيب بتغيّر الحافة ⇒ يتغيّر التخطيط (وتوقيع 015.5).
- `gpu_rg_execute()` اليوم لا ينفّذ ممرات (SPEC §2.1) ⇒ في 015.5 تصبح الممرات نداءات حقيقية على `pool_buffer`؛ **يُحتفظ** بأن `rg_execute` الحالي increment-only هو السلوك历史ي ولا يُكسر.

## 5. الـmulti-frame staging
-(readback متعدد الإطارات (خيار D1/أ) يحتاج 2-3 مخازن staging ودوران بفهرس `frame_index % N`. شرط السلامة: لا كتابة على staging مستخدم قبل اكتمال `submit+sync` الخاص به؛ إن تعذّر الضبط ⇒ توثيقه كقيد (معيار 7).
