# GNE â€” Known Issues

Ù‚ÙŠÙˆØ¯ Ù…Ø¹Ø±ÙˆÙØ© **Ù…ÙÙ‚ÙŠØ³Ø©** ÙÙŠ Ù‡Ø°Ø§ Ø§Ù„Ø¨Ù†Ø§Ø¡ØŒ Ù…Ø³Ø¬ÙŽÙ‘Ù„Ø© Ø­ØªÙ‰ Ù„Ø§ ÙŠÙØ¹ÙŠØ¯ Ø£Ø­Ø¯ÙŒ Ø§ÙƒØªØ´Ø§ÙÙ‡Ø§ Ù…Ù† Ø§Ù„ØµÙØ±. ÙƒÙ„ Ø¨Ù†Ø¯ ÙŠØ­Ù…Ù„ Ø¯Ù„ÙŠÙ„Ù‡ (file:line Ø£Ùˆ Ù‚ÙŠØ§Ø³ Ø­ÙŠÙ‘)ØŒ ÙˆÙ„Ø§ ÙŠÙØ³Ø¬ÙŽÙ‘Ù„ Ø§ÙØªØ±Ø§Ø¶.

---

> **Open-items consolidation (2026-09-28):** the single current view of everything
> still open lives in `docs/open_items_register.md`.
## KI-001: GPU Timestamps ØºÙŠØ± Ù…ØªØ§Ø­Ø© Ø¹Ù„Ù‰ Vulkan

**Ø§Ù„ØªØ§Ø±ÙŠØ®:** 2026-09-27 Â· **Ø§Ù„Ø­Ø§Ù„Ø©:**Unavailable â€” Ù‚ÙŠØ¯ ÙÙŠ Ù…Ø­Ø±Ù‘Ùƒ Godot (Ø®Ø§Ø±Ø¬ Ù†Ø·Ø§Ù‚ GNE)
**Ø§Ù„Ø£Ø«Ø±:** GNE Ù„Ø§ ÙŠØ³ØªØ·ÙŠØ¹ Ù‚Ø±Ø§Ø¡Ø© Ø²Ù…Ù† GPU Ø¹Ù„Ù‰ Ø§Ù„ÙˆØ§Ø¬Ù‡Ø© Ø§Ù„Ø®Ù„ÙÙŠØ© Vulkan. Ù„Ø°Ù„Ùƒ ÙŠÙ‚ÙŠØ³ Phase 5 Ø¹Ù„Ù‰ **wall-clock** Ø­ØµØ±Ø§Ù‹.
**Ø§Ù„Ø¥ØºÙ„Ø§Ù‚ (015.6):** Ù…Ù‚Ø¨ÙˆÙ„ NA + Ù…ÙˆØ«Ù‘Ù‚ (c) â€” ÙƒÙ„ Ø§Ù„Ø£Ø±Ù‚Ø§Ù… wall-clock Ø­ØµØ±Ù‹Ø§ØŒ ÙˆÙ„Ø§ ÙŠÙÙ†Ø´Ø± Ø£ÙŠ Ø±Ù‚Ù… ÙƒØ²Ù…Ù† GPU. ÙŠÙØ¹Ø§Ø¯ Ø§Ù„Ù†Ø¸Ø± ÙÙ‚Ø· Ø¹Ù†Ø¯ Ø¥ØµÙ„Ø§Ø­ upstream.

### Ù…Ø§ ÙŠØ¹Ù…Ù„ (Ù…ÙÙ‚Ø§Ø³)
- Ø§Ù„Ù€pool Ù…ÙÙØ¹ÙŽÙ‘Ù„ ÙØ¹Ù„ÙŠØ§Ù‹: `ProjectSettings.set_setting("debug/settings/profiler/max_timestamp_query_elements", 512)` **Ù‚Ø¨Ù„** Ø¥Ù†Ø´Ø§Ø¡ Ø§Ù„Ø¬Ù‡Ø§Ø² â‡’ `capture_timestamp()` ØµØ§Ø± ÙŠÙ‚Ø¨Ù„: `get_captured_timestamps_count()` ÙŠÙ†ØªÙ‚Ù„ `0 â†’ 1` Ø¨Ø¹Ø¯ Ø§Ù„Ù†Ø¯Ø§Ø¡.
- ÙƒÙˆØ¯ Ø­Ù„Ù‘ Ø§Ù„Ù†ØªØ§Ø¦Ø¬ ÙÙŠ Ø§Ù„Ù…Ø­Ø±Ùƒ **ØµØ­ÙŠØ­**: `servers/rendering/rendering_device.cpp:8335-8342` ÙŠÙ†Ø§Ø¯ÙŠ `timestamp_query_pool_get_results` Ø«Ù… `SWAP` Ø«Ù… `timestamp_result_count = timestamp_count`.
- Ø³Ø§Ø¦Ù‚ Vulkan ÙŠØ·Ø¨Ù‘Ù‚ Ù…Ø³Ø§Ø± Ø§Ù„Ù‚Ø±Ø§Ø¡Ø© ÙƒØ§Ù…Ù„Ø§Ù‹: `drivers/vulkan/rendering_device_driver_vulkan.cpp:6822` â†’ `vkGetQueryPoolResults(...)`.

### Ø§Ù„Ø³Ø¨Ø¨ Ø§Ù„Ø¬Ø°Ø±ÙŠ
`drivers/vulkan` **Ù„Ø§ ÙŠØ­ØªÙˆÙŠ Ù…Ù„Ù `utilities.cpp`** Ø¥Ø·Ù„Ø§Ù‚Ø§Ù‹. Ø§Ù„Ù…Ù„Ù Ø§Ù„ÙˆØ­ÙŠØ¯ Ø§Ù„Ø°ÙŠ ÙŠØ¶Ø¨Ø·
`timestamp_result_count` ÙÙŠ Ø§Ù„Ù…Ø­Ø±Ùƒ Ù‡Ùˆ `drivers/gles3/storage/utilities.cpp:367`
(`frames[frame].timestamp_result_count = frames[frame].timestamp_count;`).
â‡’ Ø¹Ù„Ù‰ Vulkan ØªÙØ¬Ù„Ø¨ Ø§Ù„Ù‚ÙŠÙ… ÙØ¹Ù„Ø§Ù‹ Ù„ÙƒÙ† **Ø§Ù„Ø¹Ø¯Ù‘Ø§Ø¯ Ù„Ø§ ÙŠÙÙ†Ø´Ø±**ØŒ ÙÙŠÙØ±Ø¬Ø¹ ÙƒÙ„ Ù‚Ø§Ø±Ø¦ `0`.
ÙˆØ§Ù„Ù‚Ø±Ø§Ø¡Ø© ÙÙŠ **Ù†ÙØ³** Ø§Ù„Ø¥Ø·Ø§Ø± Ø§Ù„Ø°ÙŠ Ø§Ù„ØªÙÙ‚Ø·Øª ÙÙŠÙ‡ Ø§Ù„ØªØ±ÙˆÙŠØ³Ø© ØªÙØ±Ø¬Ø¹ Ø§Ø³ØªØ¹Ù„Ø§Ù…Ø§Ù‹ ØºÙŠØ± Ù…Ø­Ù„ÙˆÙ„
(Ù‚ÙÙŠØ³Øª: `1.79e18 ns` â€” Ù‚ÙŠÙ…Ø© Ø¹Ø´ÙˆØ§Ø¦ÙŠØ©).

### Ù‚ÙŠÙˆØ¯ Ø¥Ø¶Ø§ÙÙŠØ© Ù…Ø±ØªØ¨Ø·Ø© (ÙƒÙ„Ù‡Ø§ Ù…ÙÙ‚Ø§Ø³Ø©)
| Ø§Ù„Ù‚ÙŠØ¯ | Ø§Ù„Ø¯Ù„ÙŠÙ„ |
|---|---|
| Ù„Ø§ ÙŠÙ…ÙƒÙ† ØªÙØ¹ÙŠÙ„Ù‡ Ù…Ù† `project.godot` | Ù…ÙØ³Ø¬ÙŽÙ‘Ù„ Ø¨Ù€`GLOBAL_DEF_RST` = runtime-onlyØŒ Ù„Ø§ ÙŠÙÙ‚Ø±Ø£ Ù…Ù† ÙˆÙ„Ø§ ÙŠÙÙƒØªØ¨ ÙÙŠ Ø§Ù„Ù…Ø´Ø±ÙˆØ¹ â€” `core/config/project_settings.cpp:1811` |
| Ø§Ù„Ù†Ø·Ø§Ù‚ Ù„Ø§ ÙŠØ³Ù…Ø­ Ø¨Ù€0 | `PROPERTY_HINT_RANGE, "256,65535,1"` â€” Ù†ÙØ³ Ø§Ù„Ø³Ø·Ø± |
| ÙŠØ¬Ø¨ Ø§Ù„Ø¶Ø¨Ø· **Ù‚Ø¨Ù„** `ensure_gpu_device()` | ÙŠÙÙ‚Ø±Ø£ Ù…Ø±Ø© ÙˆØ§Ø­Ø¯Ø© Ø¯Ø§Ø®Ù„ `RenderingDevice::initialize()` â€” `rendering_device.cpp:8625`Ø› ÙˆØ¬Ù‡ØªÙ†Ø§ ÙƒØ³ÙˆÙ„Ø©: `create_local_rendering_device` â†’ `create_local_device` â†’ `initialize` |
| `max_timestamp_query_elements` Ø®Ø§Øµ | ØºÙŠØ± Ù‚Ø§Ø¨Ù„ Ù„Ù„Ù‚Ø±Ø§Ø¡Ø© Ù…Ù† Ø®Ø§Ø±Ø¬ `RenderingDevice` (private) â‡’ Ù„Ø§ ÙŠÙ…ÙƒÙ† ØªØ£ÙƒÙŠØ¯ Ø§Ù„Ø­Ø¬Ù… Ø§Ù„Ù…Ø­Ù„ÙˆÙ„ Ø¥Ù„Ø§ Ø¹Ø¨Ø± Ø¹Ø¯Ù‘Ø§Ø¯ Ø§Ù„Ù‚Ø±Ø§Ø¡Ø© Ù†ÙØ³Ù‡ |

### Ø§Ù„Ø­Ù„ Ø§Ù„Ø¹Ù…Ù„ÙŠ
- Ø§Ù„Ù‚ÙŠØ§Ø³ Ø¹Ù„Ù‰ **wall-clock** Ù„ÙƒÙ„ Ù…Ù…Ø± (ÙƒØ§ÙÙ Ù„Ù€Phase 5ØŒ ÙˆÙ‡Ùˆ Ù…Ø§ ÙŠÙ†ØªØ¬ Ø§Ù„Ø£Ø±Ù‚Ø§Ù… Ø§Ù„Ø­Ù‚ÙŠÙ‚ÙŠØ©).
- **Ù„Ø§ patch Ù„Ù„Ù…Ø­Ø±Ùƒ**: Ø®Ø§Ø±Ø¬ Ø§Ù„Ù†Ø·Ø§Ù‚ + Ø®Ø·Ø±. (ØªÙ‚Ø±ÙŠØ± bug Ø¥Ù„Ù‰ Godot upstream Ø®ÙŠØ§Ø± Ù„Ø§Ø­Ù‚ØŒ ØºÙŠØ± Ù…Ø¹Ù„Ù‘Ù‚ Ø¹Ù„ÙŠÙ‡.)
- Ø¹Ù…ÙˆØ¯ GPU ÙÙŠ Ø§Ù„ØªÙ‚Ø§Ø±ÙŠØ± ÙŠÙØ·Ø¨Ø¹ **`NA`** ÙˆÙ„Ø§ ÙŠÙÙ‚Ø±Ø£ Ø£Ø¨Ø¯Ø§Ù‹ ÙƒÙ€Â«ØªÙƒÙ„ÙØ© ØµÙØ±ÙŠØ©Â» â€” ÙˆÙ‡Ø°Ø§ Ù…Ù‚ØµÙˆØ¯.
- ØªÙØ¹ÙŠÙ„ Ø§Ù„Ù€pool (512) ÙŠØ¨Ù‚Ù‰ ÙÙŠ Ø§Ù„Ù…Ø´Ù‡Ø¯: Ø§Ù„Ø§ØªØ¬Ø§Ù‡ ØµØ­ÙŠØ­ ÙˆÙ„Ø§ ÙŠØ¶Ø±Ù‘.

### Ø§Ù„Ø£Ø«Ø± Ø¹Ù„Ù‰ GNE
- Phase 5 ÙŠÙ‚ÙŠØ³ Ù‚Ø¨Ù„/Ø¨Ø¹Ø¯ Ø¹Ù„Ù‰ wall-clock.
- **ÙƒÙ„ Ø§Ù„ØªÙˆØ§Ù‚ÙŠÙ‚ ØºÙŠØ± Ù…ØªØ£Ø«Ø±Ø©**: `013` / `014` / `015` / `v15.5-p4` ÙƒÙ…Ø§ Ù‡ÙŠ.
---

## KI-002: Async Readback Shares The Sync Staging Buffer

**Ø§Ù„ØªØ§Ø±ÙŠØ®:** 2026-09-27 Â· **Ø§Ù„Ø­Ø§Ù„Ø©:** Ù„Ø§ Ø£Ø³Ø±Ø¹ Ù…Ù† Ø§Ù„Ù…ØªØ²Ø§Ù…Ù† â€” Ù‚ÙŠØ¯ ÙÙŠ Ø§Ù„Ù…Ø­Ø±Ùƒ
**Ø§Ù„Ø®Ø·ÙˆØ±Ø©:** Ù…Ù†Ø®ÙØ¶Ø© (Ù…ÙˆØ«Ù‘Ù‚Ø©ØŒ Ù„Ø§ Ø£Ø«Ø± Ø¹Ù„Ù‰ Ø§Ù„Ø¥Ù†ØªØ§Ø¬) Â· **Ø§Ù„Ù†Ø·Ø§Ù‚:** ÙƒÙ„ Ù‚ÙŠØ§Ø³ Ø¥Ø·Ø§Ø± incurs the sync cost
**Ø§Ù„Ø¥ØºÙ„Ø§Ù‚ (015.6):** Ù…Ù‚Ø¨ÙˆÙ„ + Ù…ÙˆØ«Ù‘Ù‚ â€” Ø§Ù„Ù€staging Ù…Ù…Ù„ÙˆÙƒ Ù„Ù„Ù…Ø­Ø±Ùƒ ÙˆÙ„Ø§ ÙŠÙØ¹Ø²Ù„ Ù…Ù† GNEØ› Ù„Ø§ Ø§Ø¯Ø¹Ø§Ø¡ ØªØ³Ø±ÙŠØ¹ async Ø¨Ù„Ø§ Ø¯Ù„ÙŠÙ„ Ø¨Ø§ÙŠØªØ§Øª/Ø²Ù…Ù†.

### Ù…Ø§ Ù‡Ùˆ Ù…ØªØ§Ø­ ÙØ¹Ù„Ø§Ù‹
- `RenderingDevice::texture_get_data_async` **Ù…ÙˆØ¬ÙˆØ¯Ø©** (`servers/rendering/rendering_device.h:475`)ØŒ ÙˆØªÙØ³Ù„ÙÙ‘Ù… `PackedByteArray` Ø¹Ø¨Ø± `request.callback.call(packed_byte_array)` (`rendering_device.cpp:8543`).
- **Ù„ÙƒÙ†Ù‡Ø§ ØªØ³ØªØ¹Ù…Ù„ Ù†ÙØ³ `download_staging_buffers`** Ø§Ù„Ù…Ø³ØªØ®Ø¯ÙŽÙ… ÙÙŠ `texture_get_data` â€” `rendering_device.cpp:1432` Ùˆ `:2916`.
- ÙˆÙƒÙ„Ø§ Ø§Ù„Ù…Ø³Ø§Ø±ÙŠÙ† ÙŠÙ‚Ø¹ ÙÙŠ `STAGING_REQUIRED_ACTION_FLUSH_AND_STALL_ALL` Ø¹Ù†Ø¯ Ù†Ø¶ÙˆØ¨ Ø§Ù„Ù…Ø®Ø²Ù† â‡’ `1080: _flush_and_stall_for_all_frames()`.
- **Ø§Ù„Ø¬Ø°Ø± (Ù…ÙÙ‚Ø§Ø³ ÙÙŠ Phase 4):** 8 MB (pixels) + 8 MB (depth) Ù„ÙƒÙ„ Ø¥Ø·Ø§Ø± ØªØªØ¬Ø§ÙˆØ² Ø³Ø¹Ø© Ø§Ù„Ù…Ø®Ø²Ù† Ø§Ù„Ù…Ø¤Ù‚Øª Ø§Ù„Ø§ÙØªØ±Ø§Ø¶ÙŠØ©ØŒ ÙØ§Ù„ØªØ¬Ù…ÙŠØ¯ ÙŠÙ‚Ø¹ **ÙƒÙ„ Ø¥Ø·Ø§Ø±** â‡’ `raster+output = 81.8%` Ù…Ù† Ø²Ù…Ù† Ø§Ù„Ø¥Ø·Ø§Ø±.

### Ù…Ø§ Ø¬Ø±Ù‰ ÙØ¹Ù„Ø§Ù‹ (Ø­Ø§ÙˆÙ„Ø© Ù…ÙˆØ«Ù‘Ù‚Ø©ØŒ Ù„Ø§ Ø§Ø¯Ù‘Ø¹Ø§Ø¡)
- Ù†ÙÙÙÙ‘Ø°Øª ÙˆØ§Ø¬Ù‡Ø© async ÙƒØ§Ù…Ù„Ø© ÙÙŠ C++ (7 Ø¯ÙˆØ§Ù„ + `bind_method`s) + Ù…Ø´Ù‡Ø¯ `main_015_5_async.gd` Ø¨Ø«Ù„Ø§Ø«Ø© Ø£Ø°Ø±Ø¹ Ù‚ÙŠØ§Ø³ (`sync` / `async` / `no_readback`).
- Ø§Ù„Ø¨Ù†Ø§Ø¡ **Ù†Ø¸ÙŠÙ** (ØµÙØ± Ø£Ø®Ø·Ø§Ø¡Ø› Ø£ÙØµÙ„Ø­Øª 3 Ø£Ø®Ø·Ø§Ø¡ Ø­Ù‚ÙŠÙ‚ÙŠØ©: Ù…Ø³Ø§Ø± `callable_mp` = `core/object/` Ù„Ø§ `core/variant/`ØŒ Ø¹Ø¯Ø¯ ÙˆØ³Ø§Ø¦Ø· `texture_get_data_async` = 3ØŒ Ùˆ`Vector<RID>` ØºÙŠØ± Ù‚Ø§Ø¨Ù„ Ù„Ù„ØªÙƒÙ„ÙŠÙ Ø¨Ù€`int`).
- **Ø§Ù„Ù†ØªÙŠØ¬Ø©: Ù„Ø§ ØªØ­Ø³Ù‘Ù† Ù…Ø«Ø¨Øª.** Ø§Ù„Ù‚ÙŠØ§Ø³ Ù„Ù… ÙŠÙƒØªÙ…Ù„: Ø§Ù„Ø¹Ù…Ù„ÙŠØ© ÙƒØ§Ù†Øª **Ù…Ù†ØªØ¸Ø±Ø© Ù„Ø§ Ø­Ø§Ø³Ø¨Ø©** (â€Ž+3 Ø«ÙˆØ§Ù†Ù Ù…Ø¹Ø§Ù„Ø¬ Ù„ÙƒÙ„ 28 Ø«Ø§Ù†ÙŠØ© Ø²Ù…Ù†) â€” Ø³Ù„ÙˆÙƒ `stall` Ù„Ø§ Ø­Ø³Ø§Ø¨.
- â‡’ Ø·ÙÙ„Ø¨ Ù…Ù† Ø§Ù„Ù…Ø§Ù„Ùƒ Ù‚Ø±Ø§Ø±ØŒ ÙˆØ£ÙØ±Ø¬ÙØ¹ Ø§Ù„Ù€API Ø¨Ø§Ù„ÙƒØ§Ù…Ù„.

### Ø§Ù„Ù‚Ø±Ø§Ø± (OwnerØŒ 2026-09-27)
- **Ø§Ø³ØªØ±Ø¬Ø§Ø¹** ÙˆØ§Ø¬Ù‡Ø© async: Ù„Ø§ Ø¯Ù„ÙŠÙ„ Ø¹Ù„Ù‰ ØªØ­Ø³Ù‘Ù† â‡’ Ù„Ø§ ÙƒÙˆØ¯ Ù…ÙŠØª ÙÙŠ Ø§Ù„Ù…Ø³ØªÙˆØ¯Ø¹.
- **Ù„Ø§ ØªØ¹Ø¯ÙŠÙ„ Ù„Ù„Ù…Ø­Ø±Ùƒ** (Ø®Ø§Ø±Ø¬ Ø§Ù„Ù†Ø·Ø§Ù‚ØŒ Ø§ØªØ³Ø§Ù‚Ø§Ù‹ Ù…Ø¹ KI-001).
- **Ø§Ù„Ø§Ù†ØªÙ‚Ø§Ù„ Ø¥Ù„Ù‰ Phase 5.1**: ØªØ­Ø³ÙŠÙ† Ù…Ø³Ø§Ø± **Ø§Ù„Ø¹Ø±Ø¶** ÙÙ‚Ø· (Ø¯Ù‚Ø© Ù†ØµÙÙŠØ© / ØªØ®Ø·ÙŠ Ø¥Ø·Ø§Ø± / Ø¥Ø¹Ø§Ø¯Ø© Ø§Ø³ØªØ®Ø¯Ø§Ù… `Image`) â€” ÙˆÙ‡Ùˆ Ù…Ø§ ÙŠÙ…ÙƒÙ† Ù‚ÙŠØ§Ø³Ù‡ ÙØ¹Ù„Ø§Ù‹.

### Ø§Ù„Ø£Ø«Ø± Ø¹Ù„Ù‰ GNE
- Ø§Ù„Ù…Ø³Ø§Ø± Ø§Ù„Ù…ØªØ²Ø§Ù…Ù† **Ù„Ù… ÙŠÙÙ…ÙŽØ³Ù‘** Ø·ÙˆØ§Ù„ Ø§Ù„Ù…Ø­Ø§ÙˆÙ„Ø© (ØªØ­Ù‚Ù‘Ù‚: `git diff` Ù„Ø§ ÙŠÙ„Ù…Ø³ `gpu_raster_read_pixels/depth`).
- **ÙƒÙ„ Ø§Ù„ØªÙˆØ§Ù‚ÙŠÙ‚ Ù…Ø­ÙÙˆØ¸Ø©**: `013` / `014` / `015` / `v15.5-p4` ÙƒÙ…Ø§ Ù‡ÙŠ.
- Ù„Ù… ÙŠÙØ¹Ù…Ù„ Ø£ÙŠ commit Ù„Ù„Ù€async API (Ù‚ÙŠØ¯: Â«Ù„Ø§ commit Ø¨Ù„Ø§ Ù‚ÙŠØ§Ø³ Ù†Ø§Ø¬Ø­Â»).

---

## KI-003: Presentation Optimization Has No Measurable Effect

**Ø§Ù„ØªØ§Ø±ÙŠØ®:** 2026-09-27 Â· **Ø§Ù„Ø­Ø§Ù„Ø©:** Ù…ÙØºÙ„Ù‚ (Ø§Ù„Ø¬Ø°Ø± ÙÙŠ Ø§Ù„Ù…Ø­Ø±ÙƒØŒ Ù„Ø§ ÙÙŠ GNE)
**Ø§Ù„Ø®Ø·ÙˆØ±Ø©:** Ù…Ù†Ø®ÙØ¶Ø© (Ù…ÙˆØ«Ù‘Ù‚Ø©ØŒ Ù„Ø§ Ø£Ø«Ø± Ø¹Ù„Ù‰ Ø§Ù„Ø¥Ù†ØªØ§Ø¬) Â· **Ø§Ù„Ù‚Ø±Ø§Ø±:** Ø¥ØºÙ„Ø§Ù‚ Phase 5.1 Ø¨Ù„Ø§ commit

### Ù…Ø§ Ø¬Ø±Ù‰ Investigation
| Ø§Ù„Ø·Ø±ÙŠÙ‚Ø© | Ø§Ù„Ù†ØªÙŠØ¬Ø© | Ø§Ù„Ø³Ø¨Ø¨ (Ù…ÙÙ‚Ø§Ø³) |
|---|---|---|
| **A** Ù‚Ø±Ø§Ø¡Ø© Ù†ØµÙ Ø§Ù„Ø¯Ù‚Ø© (4Ã—) | âŒ **Ù…Ø³ØªØ­ÙŠÙ„Ø© Ø¨Ù„Ø§ C++ Ø¬Ø¯ÙŠØ¯** | `texture_get_data` ÙŠÙ‚Ø±Ø£ Ø§Ù„Ù†Ø³ÙŠØ¬ **ÙƒØ§Ù…Ù„Ø§Ù‹** Ø¨Ù„Ø§ sub-regionØ› ÙˆØ§Ù„Ù‡Ø¯Ù `RASTER_TARGET_W/H = 1920/1080` Ø«Ø§Ø¨Øª `constexpr` (`gne_render_server.h:77-78`) Ù…Ø³ØªØ®Ø¯ÙŽÙ… ÙÙŠ 9 Ù…ÙˆØ§Ø¶Ø¹. ÙˆÙ„ she'd ØªÙØºÙŠÙ‘Ø± Ø§Ù„Ø¨ÙƒØ³Ù„Ø§Øª Ø§Ù„Ù…Ø¹Ø±ÙˆØ¶Ø© â‡’ Ù„Ø§ ÙŠÙ‚ÙŠØ³Ù‡Ø§ Ø£ÙŠ harness ÙŠÙØ­Øµ Ø¨ÙƒØ³Ù„Ø§Øª. |
| **B** ØªØ®Ø·ÙŠ Ø¥Ø·Ø§Ø± (2Ã—) | âš ï¸ Ù…Ù…ÙƒÙ†Ø© Ø¨Ù„Ø§ C++ | **Ù„Ø§ ØªØ¹Ø§Ù„Ø¬ Ø§Ù„Ø¬Ø°Ø±**:ØªÙ‚Ù„ÙŠÙ„ ØªØ®Ø·ÙŠ Ø§Ù„Ø·Ù„Ø¨Ø§Øª Ù„Ø§ ÙŠÙˆØ³Ù‘Ø¹ `download_staging_buffers`ØŒ ÙØ§Ù„ØªØ¬Ù…ÙŠØ¯ ÙŠØ¹ÙˆØ¯ Ø¹Ù†Ø¯ Ø§Ù„Ø§Ù…ØªÙ„Ø§Ø¡. |
| **C** Ø¥Ø¹Ø§Ø¯Ø© Ø§Ø³ØªØ®Ø¯Ø§Ù… `Image` | âš ï¸ Ù…Ù…ÙƒÙ†Ø© Ø¨Ù„Ø§ C++ | **Ù„Ø§ ØªØ¹Ø§Ù„Ø¬ Ø§Ù„Ø¬Ø°Ø±**: `Image.set_data` (`core/io/image.h:410`) ÙŠÙˆÙÙ‘Ø± ØªØ®ØµÙŠØµØ§Ù‹ Ø¹Ù„Ù‰ Ø§Ù„Ù…Ø¹Ø§Ù„Ø¬ØŒ ÙˆØ§Ù„Ø¹Ø¨Ø¡ Ø§Ù„Ø­Ù‚ÙŠÙ‚ÙŠ Ø¹Ù„Ù‰ Ø§Ù„Ø¨Ø§ÙŠØªØ§Øª Ø§Ù„Ù…Ù†Ø³ÙˆØ®Ø©. |

### Ø§Ù„Ø¬Ø°Ø± (Phase 4 + KI-002)
- ÙƒÙ„ Ø¥Ø·Ø§Ø± ÙÙŠÙ‡ readback ÙŠÙ‚Ø¹ ÙÙŠ `STAGING_REQUIRED_ACTION_FLUSH_AND_STALL_ALL` (Ù†Ø¶ÙˆØ¨ `download_staging_buffers`).
- Ø²Ù…Ù† Ø§Ù„Ø¥Ø·Ø§Ø± ÙŠØµØ¨Ø­ **â‰ˆ2 Ø«Ø§Ù†ÙŠØ©** (Ù…Ù‚Ø§Ø³: Ø§Ù„Ø¹Ù…Ù„ÙŠØ© Â«Ù…Ù†ØªØ¸Ø±Ø© Ù„Ø§ Ø­Ø§Ø³Ø¨Ø©Â» â€” â€Ž+3 Ø«ÙˆØ§Ù†Ù Ù…Ø¹Ø§Ù„Ø¬ Ù„ÙƒÙ„ 28 Ø«Ø§Ù†ÙŠØ© Ø²Ù…Ù†).
- Ù…ÙŠØ²Ø§Ù†ÙŠØ© Ø§Ù„Ù‚ÙŠØ§Ø³ (300 Ø¥Ø·Ø§Ø±/Ø°Ø±Ø¹ØŒ ÙˆØ­ØªÙ‰ 60 Ø¥Ø·Ø§Ø±/Ø°Ø±Ø¹) **Ù„Ø§ ØªÙƒØªÙ…Ù„** â‡’ Ù„Ø§ Ø±Ù‚Ù… â‡’ Ù„Ø§ Ø§Ø¯Ù‘Ø¹Ø§Ø¡.

### Ø§Ù„Ù‚Ø±Ø§Ø± (OwnerØŒ 2026-09-27)
- **Ø¥ØºÙ„Ø§Ù‚ Phase 5.1 Ø¨Ù„Ø§ commit**ØŒ ÙˆØ­Ø°Ù Ù…Ù„ÙÙŽÙ‘ÙŠ Ø§Ù„Ù…Ø´Ù‡Ø¯ â‡’ Ù„Ø§ ÙƒÙˆØ¯ Ù…ÙŠØª ÙÙŠ Ø§Ù„Ù…Ø³ØªÙˆØ¯Ø¹.
- **Ù„Ø§ Ø£Ø±Ù‚Ø§Ù… Ø£Ø¯Ø§Ø¡** â€” Â«Ù„Ø§ ØªØ±Ø§Ø¬Ø¹ ØµØ§Ù…ØªÂ» ØªÙ†Ø·Ø¨Ù‚ Ø¹Ù„Ù‰ Ø§Ù„ØªØ¬Ø§Ø±Ø¨ Ø£ÙŠØ¶Ø§Ù‹ØŒ Ù„Ø§ Ø¹Ù„Ù‰ Ø§Ù„ÙƒÙˆØ¯ ÙÙ‚Ø·.
- **Ù„Ø§ ØªØ¹Ø¯ÙŠÙ„ Ù…Ø­Ø±Ùƒ** (Ø§ØªØ³Ø§Ù‚Ø§Ù‹ Ù…Ø¹ KI-001 / KI-002).

### Ù„Ù…Ø§Ø°Ø§ B ÙˆC Ù„Ø§ ØªÙØ³ØªØ­Ù‚Ø§Ù†
ØªÙ‚Ù„ÙŠÙ„ **Ø¹Ø¯Ø¯** Ø§Ù„Ø·Ù„Ø¨Ø§Øª Ø£Ùˆ **ØªØ®ØµÙŠØµ Ø§Ù„Ù…Ø¹Ø§Ù„Ø¬** Ù„Ø§ ÙŠÙ…Ø³ **Ø§Ù„Ø­Ø¬Ù… Ø§Ù„Ù…Ù†Ø³ÙˆØ® Ù„ÙƒÙ„ Ø¥Ø·Ø§Ø±**. Ø§Ù„Ø¨ÙˆØ§Ø¨Ø© Ø§Ù„ÙˆØ­ÙŠØ¯Ø© Ø§Ù„Ù…Ø¤Ø«Ø±Ø© Ù‡ÙŠ ØªÙ‚Ù„ÙŠÙ„ Ø§Ù„Ø¨Ø§ÙŠØªØ§Øª Ù†ÙØ³Ù‡Ø§ØŒ Ø£ÙŠ Ù…Ø³Ø§Ø± Ø±Ø³Ù… Ø¨Ø¯Ù‚Ø© Ø£Ù‚Ù„ â€” ÙˆÙ‡Ùˆ (Ø£) ÙˆÙŠÙØ±Ø¶ C++ Ø¬Ø¯ÙŠØ¯Ø§Ù‹ ÙˆØªØºÙŠÙŠØ± Ø¨ÙƒØ³Ù„Ø§Øª â‡’ **milestone Ù…Ø³ØªÙ‚Ù„**ØŒ Ù„Ø§ Phase 5.1.

### Ø§Ù„ØªØ§Ù„ÙŠ
- **015.5 Final** (Ø¥ØºÙ„Ø§Ù‚ Ø§Ù„Ù€milestone Ø¹Ù†Ø¯ **5/8 Ù…Ø¹Ø§ÙŠÙŠØ±** + Ø«Ù„Ø§Ø« KI Ù…ÙˆØ«Ù‘Ù‚Ø©).
- milestone Ù…Ø³ØªÙ‚Ù„ Ù„Ø§Ø­Ù‚: ØªÙ‚Ù„ÙŠÙ„ `bytes_copied_per_frame` Ø¹Ø¨Ø± Ù…Ø³Ø§Ø± Ø±Ø³Ù… Ø¨Ø¯Ù‚Ø© Ø£Ù‚Ù„.


## KI-007: HZB Uses AABB Occluders (Not Scene Depth)

**Date:** 2026-09-28
**Status:** By design (012-revised scope)
**Severity:** Medium (deferred to 019)
**Owner:** GNE Architecture

**Details:**
- 012-revised activates HZB occlusion.
- Pyramid is built from AABB occluders via
  `gpu_hzb_set_occluders`.
- Real scene depth (009 `raster_viewz_texture`)
  is NOT used.
- `gpu_hzb_depth_source` is compiled but never dispatched.

**Impact:**
- Real geometry occlusion (walls, terrain) not covered.
- False-occlusion risk for complex scenes.
- Lighting (018) may need real geometry for tests.

**Deferred To:**
- 019 (Shadows + Real Depth HZB).
- Target: before 020.

**Evidence:**
- 012-revised closure: p1=6, p2=0 with AABB occluders.
- Pyramid non-empty (245,520 texels).

**Resolution Path:**
- 019: activate `gpu_hzb_depth_source` + scene depth feed.
- Requires: geometry pipeline + depth source binding.

## KI-011: Specular Not Gated By NdotL (Pre-Existing)

**Date:** 2026-09-28
**Status:** CLOSED 2026-09-29 (owner decision: Option A fix executed - specular gated by NdotL in all three sites: mat dir-light, light dir-light, light cluster loop). Before/after: literals unchanged (v16/v18/v19 identical); goldens re-baselined deliberately (main_018: 5 px, max 1 LSB, all darker; main_019: 5 px, max 1 LSB, all darker - the exact backfacing-specular signature).
**Severity:** Low-Medium (grazing/backlit specular energy; no DET/buffer impact)
**Owner:** GNE Architecture

**Details:**
- The material fragment light loop does not gate the specular term by NdotL; the diffuse term uses max(dot(N,L), 0) but the specular add is unconditional.
- A light with NdotL <= 0 for a surface can still contribute specular energy in grazing configurations (H midway between L and V).
- The gap predates 018-rev. It surfaced because 018-rev R1 is the first byte-exact pixel comparison across a light-list change; without separation it would be misattributed to the new cone culling.

**Impact:**
- After a strictly back-facing light is culled, small differences (sub-LSB to small) may appear versus not culling; these belong here, not to 018-rev.
- No effect on determinism, buffers, or any gated milestone output; the source path is unchanged.

**Scope separation (Architect directive, 2026-09-28):**
- Must NOT be fixed inside the 018-rev batch.
- 018-rev R1 (amended): differences flag-off/on must be confined to clusters whose light lists changed; every difference must carry the NdotL <= 0 leakage signature; anything else fails as an over-cull defect.

**Fix path (future milestone; Owner decision required):**
- Option A: gate the specular term by the same NdotL factor (shading change; own SPEC + before/after pixel/DET evidence).
- Option B: keep and document (if an impact study proves magnitudes negligible).
- Impact study first: which configurations show it; measured magnitudes.

**Evidence:**
- Source review 2026-09-28: material fragment light-accumulate loop in gne_render_server.cpp.
- Recorded during 018-rev SPEC review; ticket opened per Architect directive (this file).

## KI-012: Demo Window Size Is Externally Mutable (Pre-Existing)

**Date:** 2026-09-28
**Status:** ACCEPTED & DOCUMENTED (owner, 2026-09-30) - closed by decision, NOT fixed. Pre-existing external condition, unchanged by any engine work. **Reopen condition:** any criterion that must gate on a raw visible-count across differing window sizes - the real fix there is to make the scene read a fixed viewport, not to tune thresholds.
**Severity:** Low (gated DET content proven insensitive; affects only printed counts and screenshots of spread scenes)
**Owner:** GNE Architecture

**Details:**
- Demos obtain their "viewport" from `get_viewport().get_visible_rect().size` (the OS window size) and feed it to `gpu_scene_set_viewport`. That value lands in `viewdata.viewport` and feeds the HZB occlusion pixel-radius math.
- No demo or module code sets the window size or mode (full audit: zero DisplayServer/window-size calls; demo project.godot has no display settings); the window runs at the Godot default 1152x648 unless changed externally.
- Source located and REPRODUCED: an external maximize of the game window mid-run flips the very next read to 1920x1009 (observed: visible counts jumped 370 -> 402 in the same run). The R0 baseline battery (2026-09-28 17:11) shows the same signature - a mid-run transition that persisted across that battery's processes (an active-session external interaction; not reproducible by any code path).

**Impact (evidence):**
- Affected: marginal occlusion decisions in spread scenes (007/008/008B) -> Visible/Indirect-args/Green-pixels printouts and window screenshot content vary run-to-run (noise-floor: two runs of the same binary differ in exactly these values).
- NOT affected (proven): gated DET signatures. Forced-resolution A/B on the current binary gives byte-identical signatures - main_011 `v128|st0|m64|gc64|dc64|ic64|cc64|dF0.92076|dB0.95204|cb1|dt1` and main_018 `v18|lc=20|cc=2841|ot=0|hr=0.94|d1` at 1280x720 vs 1920x1009. All closed-milestone signature files and CSM dumps matched across the two R0 batteries although the baseline ran with maximized windows and the post battery at default size.

**Recommendations:**
- For future cross-run pixel comparisons on spread scenes: pin the window size (e.g., `--resolution`) or compare fixed-target readbacks only.
- The 018-rev metrics ride the fixed 1920x1080 cluster grid (see main_018 A/B above) and are window-independent; the R7 staleness counter is frame-based and unaffected.

**Evidence:**
- temp\opencode\x\expA2.log (maximize reproduction), x\s011_*.txt, x\s018_*.txt (forced-resolution A/B), temp\opencode\nf (noise floor).

## KI-013: gt_regress FAILED Flag Reset By The XFAIL Scene (Harness, Pre-Existing)

**Date:** 2026-09-28
**Status:** Resolved 2026-09-28 - masking defect fixed; 008b criterion recalibrated (0.35 -> 1.0, physical rationale in-scene); battery green with honest accounting. Onset note (pre-019 HZB candidate) left informational.
**Severity:** Medium (masked failures - any scene failing BEFORE the main_012 XFAIL call is forgiven)
**Owner:** GNE Architecture

**Details:**
- `tools/gt_regress.bat` line 66 (`if "%BAD%"=="1" if "%XFAIL%"=="XFAIL" set FAILED=0`) resets the accumulated FAILED flag when the XFAIL scene (main_012) is processed, erasing failures recorded by any earlier scene.
- Reproduced deterministically with an isolated copy of the harness: only `main_008b` -> FAILED=1 -> `GT_REGRESS: FAIL` (exit 1); `main_008b` + `main_012 XFAIL` -> `GT_REGRESS: PASS` (exit 0). Debug trace shows failed=1 after 008b and a final PASS because the XFAIL scene cleared the flag.
- Masked consequence today: `main_008b` fails its own spot-check (rc=75, misses 20..32 on a dynamic scene) in every observed battery while the sweep reports PASS. The JEV pilot log (2026-09-27) already records a main_008b det=DIFF anomaly; progress_report recorded an earlier 008B PASS.

**Impact:**
- Scenes ordered before main_012 (currently 007..011) cannot gate the sweep even if they fail.
- Gate-trust defect in the harness; no engine correctness risk.

**Fix path (Owner decision):**
- Make XFAIL handling not touch other scenes' failures (XFAIL should skip only its own failure), then decide main_008b's disposition (fix its spot-check, mark it XFAIL deliberately, or re-scope) and re-run the full battery as evidence.

**Evidence:**
- temp\opencode\r0_dbg_regress.bat / r0_dbg_mask.bat runs (repro), r0_baseline\r0_verdict_summary.txt.

**Update 2026-09-28 (directive task A completed):** the masking defect is fixed in `tools/gt_regress.bat`: `set FAILED=0` initialization added; the XFAIL branch no longer resets the accumulated flag (`if not "%XFAIL%"=="XFAIL" set FAILED=1`); REM note added. Validated by scenario runs: [008b + 012-XFAIL] -> FAIL (exit 1, no masking), [012-XFAIL only] -> PASS (exit 0), [007 only] -> PASS (exit 0). Post-fix battery re-run shows the true per-scene state; main_008b now surfaces its own rc=75 openly - its disposition is tracked under directive task B.

**Update 2026-09-28 (directive task B completed - 008b severity: ISOLATED):** rc=75 is the scene's own acceptance-criterion threshold, not a shared/critical-path defect.
- Miss set = the entire sampled small-radius band: every miss has projected radius r in [0.41, 0.67] px (positions scattered, no index pattern); the center pixel is background; nearest other green >= 10 px away.
- Counterfactual (diagnostic copy, threshold 0.35 -> 0.75 only): misses 32 -> 1, with 130 low-radius cubes reclassified as subpixel-skips (790 -> 663 checked). Mechanism confirmed: at 1x rasterization a cube below ~1 px can cover zero samples however correct the pipeline is; SUBPIXEL_R_PX=0.35 sits below that limit (true can-vanish radius ~0.9-1.0 px).
- The shared stages pass their own rigor checks concurrently: GPU-vs-CPU cull delta=0, compact set_ok=true, no dropped clearly-visible cubes, drawargs repeat + full-frame repeat deterministic.
- Count variation 20..32 across contexts = the KI-012 window-state class: 1152x648 contexts -> visible 3680 / miss 20; ~1920x1009 contexts -> visible 3778 / miss 32 (windows currently auto-maximize right after open; probe: main_008 with --resolution 1152x648 printed 1152 then 1920x1009). The FAIL outcome is invariant - only the sampled band size moves.
- Onset (rc=0 at the rename sweep, pre-017/018/019 engine): candidates only, not proven - (i) pre-019 AABB-occluder HZB may have (falsely) occluded part of the small far cubes; (ii) run-environment state. Recorded as open.
- Implication for 018-rev: none found - the rev path rides the fixed 1920x1080 cluster grid and the mat_light flow; 008b's criterion does not gate it.
- Disposition (Owner): fix the scene criterion (~1.0 threshold / coverage-aware) or mark 008b deliberately as XFAIL (visible, by design). No engine change proposed.

**Update 2026-09-28 (disposition executed - RESOLVED):** Owner decision: raise the criterion, not XFAIL. Applied in `main_008b.gd`: `SUBPIXEL_R_PX` 0.35 -> 1.0 with an in-body physical rationale (1x point sampling: a silhouette below the half-diagonal bound sqrt(2)/2 ~= 0.7071 can cover zero samples by phase; the analytic r_px estimate carries orientation/scale scatter - a 0.77 px cube still missed at 0.75 - so the round full-pixel bound 1.0 is used, matching main.gd's "< 1 projected px may legitimately rasterize zero pixels" note). Validation: scene rc=0 / miss=0 (checked=533, subpixel=260); full battery re-run: all EIGHT harnesses rc=0 with honest accounting (GT_REGRESS: PASS; 012 XFAIL reported, not masking); cross-run classification audit: no other scene changed (reg_main_011/013/014/015.txt and gt015/016/017/018/019 sigs byte-identical; other consoles identical after timing/handle normalization). Retained instrument: `demo/gpu_smoke/main_008b_dbg.gd|.tscn` (documented mirror + per-miss coverage diagnostics; keep in sync with main_008b.gd).

**Update 2026-09-30 (disposition executed - ACCEPTED & DOCUMENTED, no code change):** Owner ruling on the register row that had this OPEN pending "fix demo window sizing OR accept + document": accept and document. Rationale as given - the item is a pre-existing external condition that does not touch the gne render path and is unrelated to the cluster-lighting line closed this session, so closing it by writing documentation ends the pending state without fabricating an unrequested engine fix.

**What this acceptance does and does not mean.** NOTHING WAS FIXED. The behaviour described above is still true: demos still read `get_viewport().get_visible_rect().size` and still land it in `viewdata.viewport`. The stated boundary survives into the closure - affected values are printed counts and screenshots of spread scenes, and the FAIL outcome is invariant with only the sampled band size moving (miss 20 at 1152x648 vs 32 at ~1920x1009, 3680 vs 3778 visible). Therefore any harness that gates on a visible-count number must record the window size it ran at, or its criterion is not reproducible. That sentence is the actual deliverable of this closure; without it "accepted" would read as "harmless".

Note the earlier 2026-09-28 update on this entry closed a DIFFERENT thing - the `main_008b.gd` `SUBPIXEL_R_PX` criterion (0.35 -> 1.0), which was a real code change and is unaffected. This closure does not disturb it.
## KI-014: Cluster Light Lists Under-Cover Corner Pixels (Linear-vs-Euclidean Slice Mismatch)

**Date:** 2026-09-28
**Status:** FIXED behind the 018-rev flag (2026-09-28); flag-off path byte-identical (v18 literal intact)
**Severity:** Medium (latent lighting loss for corner-adjacent geometry; would silently undermine 018-rev gate validity if unfixed)
**Owner:** GNE Architecture

**FACT:**
- The mat_light fragment assigns pixels to depth slices by EUCLIDEAN camera distance
  (`z_view = length(cam - world)`; shared `gne_cluster_index`), while `gpu_light_cull_glsl`
  builds cluster AABBs with a LINEAR-depth slab [z0, z1] (`znear * lratio^(tz/24)`).
  For off-axis pixels linear < euclidean (factor cos(theta)); at screen corners
  cos(theta_min) ~ 0.648 for fov 60 / 16:9.
- Unit-2b cross-check (r0chk selftest mode 3): 10 of 14 corner/edge cases lie OUTSIDE the
  AABB of the cluster the fragment assigns to them; gaps (z0 - z_linear) up to 707 units.

**OBSERVATION / IMPACT:**
- A light sphere touching such a corner pixel can miss the AABB and be excluded from the
  cluster list - contradicting the cull's "may over-include, never wrongly exclude" claim.
  Latent in 018's own gate scenes (their probes avoided the exposed corners), but it would
  sit UNDER any future cone-culling validity gate (R1, dc): comparing two equally deficient
  results could pass silently.

**ROOT CAUSE:** slice-semantics mismatch - fragment (euclidean distance) vs cull AABB (linear depth).

**FIX (flag-gated, applied):**
- `gpu_light_cull_glsl`: `bmin.z = z0 * cosmax` with
  `cosmax = 1 / sqrt(1 + tanv^2 * (1 + aspect^2))`, active only when the rev flag is set
  (new push-constant field x); flag-off evaluates the exact old z0, keeping 018
  byte-identical (gt_018a/gt_019a PASS with literal v18/v19 signatures).
- Analytic proof (selftest mode 4 / F6): the bound matches an independent out-of-engine
  computation across 6 FOV/aspect configurations (worst relative error 3.2e-8) and is
  TIGHT (min over dense pixel samples of (z_linear - bound) = 0.0; the old bound is
  violated by 1128..3146 samples per case). Not a fitted constant.
- Flag API: `gpu_light_set_normal_cone(bool)` (default false); measurement-only env
  override `GNE_REV_CONE=1` (unset in every gate).

**dc-impact measurement (018 scene, same binary, flag off vs on):**
- assignments 11348 -> 12363 (+8.94%); clusters_touched 2841 -> 3091 (+8.80%);
  overflows (16-cap) 0 -> 13. On 019 the signature and checks are unchanged off/on.
- Consequence for D8-rev-5 (dc >= 10%): cone-filter savings will be measured against the
  CORRECTED base (base grew ~9%); the criterion itself stays as contracted.

**GNE RELEVANCE / scope note (why fixed inside 018-rev):**
- Pre-existing 018 defect (linear z-slicing), fully independent of cone-culling logic,
  discovered by 018-rev rigor. Contrary to the usual separation policy (cf. KI-011), the
  fix ships INSIDE 018-rev scope because it is a precondition for the validity of
  018-rev's own gates - not because it is convenient to merge.

**Evidence:**
- temp\opencode\b008\r0chk_f6_run2.log (F6 rows); m018_off.log / m018_on.log (stats);
  docs/rev_slab_gap_note.md (mechanism derivation).

**R1 closure addendum (2026-09-28):** final canonical numbers on `main_018_rev`:
slab=12363 -> on=12350 assignments; overflows 0 -> 9; dc=13; d1==d2 signature
`v18-rev|lc=20|cc=3091|dc=13|ot=9|dp=2558|d1`. (The earlier env-override
measurement on main_018 read overflows 0 -> 13; both are recorded; the rev-scene
number is canonical for 018-rev.) The over-cap clusters stay loud via the `ot`
field in the rev signature; the OFF path remains ot=0. Overflow semantics / cap
policy re-examination is parked with the dense-light scenario. Status unchanged:
FIXED (gated), verified in R1.

## KI-015: Hardware Ray Tracing Unusable - RT Pipeline Creation Fails (Fork-Level)

**Date:** 2026-09-28
**Status:** Open - fork/engine-level blocker; NOT GI-specific (any future RT feature hits it)
**Severity:** Medium (blocks hardware-RT features: GI S3 backend, future RT reflections; compute-only paths unaffected)
**Owner:** GNE Architecture / engine-fork owner

**FACT:**
- R0-RT isolation test (spec_022 section 7) proved the RD RT stack works up to and including:
  RT capability detection + feature enablement at device creation, BLAS/TLAS create+build,
  RT GLSL raygen/miss/closest-hit compilation, shader creation, SBT create/range handling,
  acceleration-structure uniform binding.
- `vkCreateRayTracingPipelinesKHR` fails with `VK_ERROR_INITIALIZATION_FAILED` (-3) in
  `RenderingDeviceDriverVulkan::raytracing_pipeline_create`
  (godot-master/drivers/vulkan/rendering_device_driver_vulkan.cpp:6677). Reproduced across
  runs; the pipeline-cache hypothesis was tested (cache-disabled run) and eliminated.

**ROOT CAUSE:** NOT diagnosed. Candidates (undistinguished): SPIR-V post-processing of RT
stages inside the fork; pipeline-layout stage-flag construction for RT pipelines; a
driver-level condition on this pipeline configuration.

**IMPACT / GNE RELEVANCE:**
- Blocks any hardware-RT feature on this fork (GI S3, future RT reflections/ray queries).
- Does NOT affect raster/compute pipelines, compute-only GI (S1/S2), or any existing gate.

**NEXT (not scheduled):** engine-level diagnosis requires dedicated engine work outside the
current boundaries. Until then: hardware RT = unavailable; designs must not assume it.

**Evidence:** temp rt0_run1.log / rt0_run2.log / rt0_verbose.log; code references above.
## 015.5 C6 status (closed in 015.6)

**Ø§Ù„Ø­Ø§Ù„Ø©:** Ù…Ù‚Ø¨ÙˆÙ„ + Ù…ÙˆØ«Ù‘Ù‚ (double-buffering Ù…Ø¤Ø¬Ù‘Ù„ Ø¥Ù„Ù‰ 020) â€” "ØªÙ‚Ù„ÙŠÙ„ Ø§Ù„Ø­Ø¬Ù…" ÙŠÙƒØ³Ø± DET.
**Ø®Ø· Ø§Ù„Ø£Ø³Ø§Ø³ Ø§Ù„Ù…Ù‚Ø§Ø³ (2026-09-28ØŒ main_016ØŒ wall-clock Ø¹Ù…Ù„ÙŠØ© ÙƒØ§Ù…Ù„Ø© ØªØ´Ù…Ù„ Ø§Ù„Ø¥Ù‚Ù„Ø§Ø¹):**
- 5 ØªØ´ØºÙŠÙ„Ø§Øª (Ø«ÙˆØ§Ù†Ù): 8.76ØŒ 7.58ØŒ 8.19ØŒ 7.57ØŒ 7.41 â€” ÙƒÙ„Ù‡Ø§ exit=0.
- median 7.58 Â· p95 â‰ˆ 8.76 Â· max 8.76 Â· min 7.41.
- Ø§Ù„Ø¨Ø§ÙŠØªØ§Øª/Ø§Ù„ØªØ´ØºÙŠÙ„: 7 Ù‚Ø±Ø§Ø¡Ø§Øª Ã— 8,294,400 = **58,060,800 B** (~55.4 MiBØŒ Ø¨ÙƒØ³Ù„ ÙÙ‚Ø· Ø¨Ù„Ø§ Ø¹Ù…Ù‚).
- Ù…ÙÙˆØ³Ù… ØµØ±Ø§Ø­Ø©Ù‹: wall-clock Ø¹Ù…Ù„ÙŠØ©ØŒ **Ù„ÙŠØ³** Ø²Ù…Ù† GPU (Ù‚Ø§Ø¹Ø¯Ø© KI-001).

**020 unit-3 addendum (2026-09-28):** the parked double-buffering item was tested at
module level (see spec_020 section 14): deferred ping-pong readback = no measurable
effect (medians 7795 vs 7890 us, 5 runs each); sync removal = crash (invalid in this
fork). The per-call readback floor is engine-side; both experimental paths were removed
after measurement. C6 remains closed-as-accepted; zero-copy (RHI-level) stays a future
item.

---

## Monitoring note (2026-09-29): transient silent EXIT=-1 on first run of a fresh binary

**Observed:** one silent process death (EXIT=-1, no `ERROR:` line, no FAIL line) mid-run of
`main_018` on the first execution of a freshly linked binary carrying the KI-017
5th-attachment change. Environment was quiet at the time (no neighbor GPU load, no
nvlddmkm/TDR events in the logs). An immediate retry of the identical command passed
fully (`v18|lc=20|cc=2841|ot=0|hr=0.94|d1`, EXIT=0), as did the full CVS afterwards.
**Status:** watch item only, NOT a numbered KI â€” single occurrence, unreproduced, no root
cause claimed. Pattern resembles the historical first-run flakes (Lesson 5 class:
silent kills with a quiet event log), hence recorded rather than dismissed.
Re-open as a KI only on second occurrence with logs attached.
