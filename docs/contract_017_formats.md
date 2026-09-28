# Contract 017 — Formats (KTX2 + Basis)

**GNE-017 — DRAFT (SPEC `spec_017_textures.md`).** Additive TEST-ONLY evidence bridge in `modules/gne_render`.

## KTX2 subset (017 supports exactly this, nothing more)

- Container **KTX2**, 2D textures only (no cubemap/array/volume in 017).
- Basis payloads: **ETC1S** (ratio-first) or **UASTC** (quality-first) — preset frozen at FINAL (D5-1).
- Baked offline to: RGBA8 + full mip chain (first path) or block-compressed (later) — frozen at FINAL.
- Magic: undecided in 017 (D5-1; GOTOML11 lesson: magic is a permanent decision — propose `GNETEX11`, freeze before any asset ships).

## Offline transcode (the only place Basis is touched)

- `tools/tex_import/` (MSVC host binary, `meshlet_import` pattern): PNG → Basis → baked `.gtex` (KTX2 + mips).
- The module **never links basisu/libktx**; the runtime parses the KTX2 header and uploads raw mip levels.
- First-texel proof (criterion 2): baked texel value == runtime-uploaded texel value, byte-identical.

## Loader validation (reject, never silent-fallback)

- Bad magic / truncated mips / size mismatch ⇒ `print_error` + return -1.
- Mip count or dimensions outside the baked header ⇒ reject (no resampling in runtime).
