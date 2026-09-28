# GNE — External Sources Registry

Every external source used in architectural decisions must be registered:
URL + hash/commit + date + purpose.

## Active Sources

| Source | URL | Hash/Commit | Date | Used For |
|---|---|---|---|---|
| vkguide.dev | https://vkguide.dev/docs/... | (hash) | 2026-09-27 | HZB culling, Y-flip |
| Godot PR #100907 | github.com/godotengine/godot/pull/100907 | (sha) | 2026-09-27 | view depth semantics |
| Godot PR #103798 | github.com/godotengine/godot/pull/103798 | (sha) | 2026-09-27 | HZB |
| miketuritzin.com | https://miketuritzin.com/post/... | (hash) | 2026-09-27 | Hi-Z reference |
| salivity.github.io | https://salivity.github.io/... | (hash) | 2026-09-27 | GLSL HZB |

## Rules

1. Every source = URL + hash/commit + date.
2. No "from memory" sources.
3. Sources reviewed before architectural decisions.
4. Reference code ≠ copied code.
