# GNE - Open Items Register (single source of truth)

**Updated:** 2026-09-28 (post-GNE-021 closure; the pre-GI pause review).
**Purpose:** one clear list of everything still OPEN or DEFERRED. If an item is not
listed here, it is not open. Detailed records stay in their home documents (linked).
Update this file whenever a status changes.

## A. Open KIs (owner decision / engine dependency)

| ID | Item | State today | What closes it | Home record |
|---|---|---|---|---|
| KI-011 | Specular term is not NdotL-gated (pre-existing) | OPEN - deliberately kept outside 018-rev | owner decision: gate specular by NdotL (fix) OR accept + document | known_issues.md (KI-011) |
| KI-012 | Demo window size externally mutable (pre-existing) | OPEN | owner decision: fix demo window sizing OR accept + document | known_issues.md (KI-012) |
| KI-001 | GPU timestamps unavailable on this Vulkan fork | OPEN - engine dependency; wall-clock workaround in effect; NA accepted in 015.6 | closes only if the engine/fork gains working timestamp resolution | known_issues.md (KI-001) + progress section 34 |

## B. Deferred items / future work (not numbered KIs)

| Item | State today | Re-open condition | Home record |
|---|---|---|---|
| Zero-copy presentation (RHI-level) | NOT solved by 020 (mitigation only: 4x fewer bytes, faster frames) | a milestone with RHI scope | spec_020 section 13 |
| Cone culling default-ON | DISABLED by decision (dc = 0.48% < 10%; net -6.47%) | only if a directionally-coherent scene use-case appears (open-outdoors class) | spec_021 section 4 + spec_018_rev section 12 |
| GI S3 RT-backend (hardware ray tracing) | R0-RT PARTIAL: scaffold works up to pipeline creation; vkCreateRayTracingPipelinesKHR fails (-3), root cause open | engine-level diagnosis, then re-decide S3 | spec_022 section 7 |
| VSM / RT / volumetric shadows | Deferred since 019 | a future shadow milestone | spec_019 section 7 |
| 015.5 partial checklist gaps | Recorded partial (5/8 + 3 documented items) | per item, as scheduled | progress section 30 |

## C. The arc at a glance (016 -> 021)

016 materials -> 017 textures -> 018 lighting -> 018-rev cone culling (closed; default
OFF) -> 019 shadows + real-depth HZB -> 020 presentation overhaul (readback mitigated,
not solved; C6 closed with evidence) -> 021 benchmark city + dense validation (dc >= 10%
closed as measured: 0.48%; default stays OFF). Every milestone closed with literal
signatures, honest accounting, and its own recorded decision set; lessons 1-6 live in
docs/lessons.md.

**Next:** GI (022+) - scoping draft delivered (`docs/spec_022_gi_scoping.md`); awaiting the Architect decision set D10 before any implementation.
