# Contract 016 — Data Semantics (Materials)

**GNE-016 — DRAFT (SPEC `spec_016_materials.md`).** Additive TEST-ONLY evidence bridge in `modules/gne_render`.

## Material record (64 bytes / 4 vec4), per mesh slot, 64 slots

`id` == slot index in the material buffer:

| vec4 slot | offset | field | layout |
|-----------|--------|-------|--------|
| m[i+0]  | 0  | albedo+rough | vec4 (r, g, b, roughness) |
| m[i+1]  | 16 | emissive+metal | vec4 (r, g, b, metallic) |
| m[i+2]  | 32 | spec | vec4 (specular.r, specular.g, specular.b, shininess) |
| m[i+3]  | 48 | flags | vec4 (flags, emissive_strength, emission_backface, pad) |

- `roughness`, `metallic` ∈ [0,1]; out-of-range write ⇒ `print_error` + reject (no silent clamp).
- Defaults: `shininess=32.0`, `specular=(1,1,1)`, `emission_backface=0` (D4-4).
- `flags` bit0 = emissive_on; other bits reserved = 0 (nonzero reserved ⇒ reject).
- Floats exact for values < 2^24; GPU casts `uint()` on flags only.
- Total: 64 × 64 = **4096 B**.

## Fixed light (SPEC §3.3, NOT a lighting system)

- `L = normalize(-0.5, -1.0, -0.5)`, `AMBIENT = 0.1`, `light.color` defaults to white (D4-1).
- Lambert diffuse + Blinn-Phong specular (`shininess` from record, default 32.0).
- Roughness wiring (keeps C5 satisfiable): `spec *= (1.0 - 0.5 × roughness)`; effective spec color `mix(specular_color, albedo, metallic)`.
- Backface rule (D4-4): `!gl_FrontFacing` ⇒ diffuse/specular = 0; emission applies only if `emission_backface > 0` (default 0 = NONE). 016 materials are opaque-only.

## Face normals (no vertex format change)

- `N = faceforward(normalize(cross(dFdx(wpos), dFdy(wpos))), V)` in-fragment.
- Vertex stride stays 12 B (position only); any normal attribute ⇒ contract breach.
