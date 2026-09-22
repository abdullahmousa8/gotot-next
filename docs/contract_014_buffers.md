# Contract 014 — Buffers

**GOTOT-014 — PASS (2026-09-23).** One shader / one compute pipeline / one uniform set
(set 0, storage buffers, bindings 0..6). All buffers Vulkan SSBO via the local `RenderingDevice`.

| binding | buffer | size (alloc 1,048,576) | role |
|---------|--------|------------------------|------|
| 0 | `gms_record_buffer`   | capacity × 64 B = 67,108,864 B  | instance records (AoS) |
| 1 | `gms_ring_buffer`     | 16 MiB (16,777,216 B)           | delta ring (80 B/op) |
| 2 | `gms_stats_buffer`    | 16 × 4 B                        | per-dispatch counters |
| 3 | `gms_active_buffer`   | capacity × 4 B                  | compacted ascending-id list |
| 4 | `gms_active_count_buffer` | 4 B                         | verify counter (tiny readback bridge) |
| 5 | `gms_snapshot_buffer` | capacity × 32 B = 33,554,432 B  | 32-B draw records |
| 6 | `gms_mesh_count_buffer` | 64 × 4 B                       | per-mesh histogram |

- **SSBO footprint @ 1M:** 121,635,140 bytes (measured `ssbo_bytes`).
- Compute passes: 0 = apply (consume ring), 1 = compact (dense ids), 2 = snapshot
  (draw records + mesh counts). Each pass = one `_run_compute_pass` (submit()+sync()),
  so pass N reads pass N-1 deterministically.
- Reset: `buffer_clear` for records/counters + explicit `submit()+sync()` so compute never
  races the transfer queue (fixed against non-deterministic active counts during bring-up).
- Ring capacity check in `gpu_scene_manager_update`: `tail + n*80 ≤ 16 MiB` else hard error.