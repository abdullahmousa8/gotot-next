# Contract 017 — Bindless strategy (array fallback)

**GNE-017 — DRAFT.** Measured 2026-09-28: `rendering_device.h` has zero descriptor-indexing symbols ⇒ **no bindless path exists** in this RD fork.

## Fixed sampler array (the only legal route)

- `SAMPLER_WITH_TEXTURE` array of **8** in the `*_mat_*` set (D5-2; count frozen at FINAL).
- Index comes from push-constant `tex_slot` — dynamically uniform ⇒ legal Vulkan without extensions.
- Index outside `[0,8)` ⇒ shader falls back to flat albedo for that pixel + `tex_oob` counter increments (no crash, no silence).

## Per-material slots

- `mat_tex[64][5]` separate table (1280 B): albedo / normal / metal-rough / AO / emissive texture ids (-1 = unbound).
- Unbound slot ⇒ flat path for that channel (material still renders; evidence distinguishes bound vs unbound).
- The frozen 64 B material record is **never extended** for textures.
