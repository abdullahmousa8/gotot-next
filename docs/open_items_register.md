# GNE - Open Items Register (single source of truth)

**Updated:** 2026-09-29 (post-11-R1 closure; workflow v2 adopted; 11-M1 backlogged; CVS v1 added).
**Purpose:** one clear list of everything still OPEN or DEFERRED. If an item is not
listed here, it is not open. Detailed records stay in their home documents (linked).
Update this file whenever a status changes.

## A. Open KIs (owner decision / engine dependency)

| ID | Item | State today | What closes it | Home record |
|---|---|---|---|---|
| KI-011 | Specular term is not NdotL-gated (pre-existing) | CLOSED 2026-09-29 (fix executed; literals unchanged; goldens re-baselined: 5px/1 LSB darker per scene) | - | known_issues.md (KI-011) |
| KI-012 | Demo window size externally mutable (pre-existing) | OPEN | owner decision: fix demo window sizing OR accept + document | known_issues.md (KI-012) |
| KI-001 | GPU timestamps unavailable on this Vulkan fork | OPEN - engine dependency; wall-clock workaround in effect; NA accepted in 015.6 | closes only if the engine/fork gains working timestamp resolution | known_issues.md (KI-001) + progress section 34 |
| KI-015 | Hardware ray tracing unusable: `vkCreateRayTracingPipelinesKHR` fails (-3) (fork-level; root cause open) | OPEN - any future RT feature blocked; compute-only paths unaffected | engine-level diagnosis (outside current boundaries) | known_issues.md (KI-015) + spec_022 section 7 |

## B. Deferred items / future work (not numbered KIs)

| Item | State today | Re-open condition | Home record |
|---|---|---|---|
| Zero-copy presentation (RHI-level) | NOT solved by 020 (mitigation only: 4x fewer bytes, faster frames) | a milestone with RHI scope | spec_020 section 13 |
| Cone culling default-ON | DISABLED by decision (dc = 0.48% < 10%; net -6.47%) | only if a directionally-coherent scene use-case appears (open-outdoors class) | spec_021 section 4 + spec_018_rev section 12 |
| GI S3 RT-backend (hardware ray tracing) | R0-RT PARTIAL: scaffold works up to pipeline creation; vkCreateRayTracingPipelinesKHR fails (-3), root cause open | engine-level diagnosis, then re-decide S3 | spec_022 section 7 |
| VSM / RT / volumetric shadows | 019.5 slice-0 DONE: ESM prototype measured - softness hypothesis refuted (band ratio 0.994); PCF-4 stays; ESM flag stays inert (default OFF). Next soft-shadow step (when a requirement exists): VSM/EVSM per RFC | volumetric + RT parts remain deferred | rfc_019_5_shadow_filters.md |
| KI-017 / 023-M1 | HDR (pre-tonemap) radiance readback instrumentation - measure GI contribution on ALREADY-LIT surfaces | IN VERIFICATION (executed as top Architect directive 2026-09-29): instrument built (5th RGBA32F attachment + `gpu_raster_read_hdr` + M1' gate on main_023); M1' measured: indirect adds +0.705 HDR units on a lit surface where the 8-bit chain saw 0.0 (claim CONFIRMED); full CVS green with all literals byte-identical; pending owner closure approval + closure docs + commit | note_023_gi_shadow_integration.md |
| GI x Shadows integration (023) | DONE: umbra gains indirect +0.0902 with direct blocked; determinism byte-equal; cost bounded; I2 void-by-saturation documented | future: full-field convergence (KI-016/M1 instrument), GI x dynamic shadows | note_023_gi_shadow_integration.md |
| Material System v2 (016.5) | slice-1+2 DONE: channel maps + weighted triplanar + roughness evidence all green; legacy byte-identical; CVS clean. Unscoped candidates: seam stitching, channel-defaults policy, >8 array growth | future scoping | rfc_016_5_material_v2.md |
| 016.5 channel array bound: `m2a.X <= 4.0` vs `GNE_TEX_ARRAY = 8` | MEASURED (Y2, 2026-09-29): channels sample array positions 0..4; positions 5..7 are reachable, occupied with real resources, and silently never sampled - output byte-identical to the all-unset baseline. The inner `tia < 8` is unreachable-false dead logic. **Design intent UNDETERMINED** - no comment/constant/SPEC ties `4.0` to a decision, so this is neither "designed N=5" nor a proven bug. Distinct from `GNE_MAT_TEX_SLOTS = 5`, which bounds the MATERIAL slot, not the array position | owner architecture call: document the 0..4 bound as intended, or widen it and make 5..7 live. Either way the dead `tia < 8` needs a decision | contract_016_5_channel_bounds.md |
| KI-016 / 11-M1 | Float-field convergence instrumentation (measurement infrastructure ONLY; NOT a section-11 criterion change) | BACKLOG - Priority: Medium | when a GI/temporal measurement need arises; build as shared instrumentation | spec_022 section 11-M1 |
| 015.5 partial checklist gaps | Recorded partial (5/8 + 3 documented items) | per item, as scheduled | progress section 30 |
| 018-rev R7 cone-age tooling | DONE 2026-09-28 (unit 2c committed: live prefilter + per-frame rebuild + age counters + F7 proof cone_saved=2, slab_gain=6; battery green) | - | commit a3d5e8e |

## C. The arc at a glance (016 -> 021)

016 materials -> 017 textures -> 018 lighting -> 018-rev cone culling (closed; default
OFF) -> 019 shadows + real-depth HZB -> 020 presentation overhaul (readback mitigated,
not solved; C6 closed with evidence) -> 021 benchmark city + dense validation (dc >= 10%
closed as measured: 0.48%; default stays OFF). Every milestone closed with literal
signatures, honest accounting, and its own recorded decision set; lessons 1-6 live in
docs/lessons.md.

Next: section 11-M1 spec written; awaiting owner approval before implementation. 11-R1 CLOSED (FP32 accepted; section 11 = FAIL (criterion b) unchanged - official wording preserved). 11-M1/KI-016 backlogged, no implementation. Workflow v2 + CVS v1 in place (tools/gne_verify.ps1). Next architectural target: owner pick (see section B).
