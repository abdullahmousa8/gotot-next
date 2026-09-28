# R0 Closure - GNE-018-rev Unit 1 (Normal Attachment, Additive)

**Date:** 2026-09-28. **Scope:** prove the additive normal attachment changed nothing
that existed before it, and that its new data is exactly as designed.

## The three closure conditions

| # | Condition | Result | Evidence |
|---|---|---|---|
| (a) | All DET signatures + deterministic attachments match literally (011/013/014/015 + CSM + every closed gate) | PASS | Baseline battery 17:11:45-17:19:06 vs post battery 17:32:06-17:38:44, all 8 harnesses rc=0 in both. Byte-identical: gt015/016/017/018/019 *_sig.txt, reg_main_011/013/014/015.txt, csm_layer_0..3.bin (16.7 MB each, SHA256), gt019a console + gt019_full.txt (after normalization), gt011 signatures + PNGs. |
| (b) | Every non-018 raster pipeline writes the designed value | PASS | main_r0chk numeric readback (one run, exit 0): raster / mesh / mesh_batch / group = [0.0, 0.0, 0.0, 0.0] exactly (never-cull sentinel); mat_batch = [-0.5771, -0.5771, 0.5771, 0.0] (real unit normal, by design); mat_light = real normals (018 path, exercised later by the rev scene). |
| (c) | Every remaining diff class classified as known-environmental WITH evidence (never assumption) | PASS | Four classes below, each with direct evidence; the window-size class was source-located AND reproduced. |

## (c) Environment classes

| Class | Evidence | DET impact |
|---|---|---|
| VK loader handle numbers (`VK_OBJECT_TYPE_INSTANCE, Handle N`) | pointer values; differ in every process | none |
| wall-clock timings (`finalize_ms`, `*_us/_ns`, `... ms` prints, per-pass `wall(us)`) | house rule (015.5): timings never gate | none |
| OS window size (see KI-012) | Source: no code path sets window size (full audit; no display settings in project.godot; default 1152x648). Reproduced: external maximize of the game window mid-run -> next read 1920x1009, visible counts jumped (370 -> 402). DET independence: forced `--resolution` A/B (1280x720 vs 1920x1009) => byte-identical signatures: main_011 `v128|st0|m64|gc64|dc64|ic64|cc64|dF0.92076|dB0.95204|cb1|dt1`, main_018 `v18|lc=20|cc=2841|ot=0|hr=0.94|d1`. Cross-battery: all sig files matched although the baseline ran with maximized windows and the post battery at default. | only marginal occlusion counts in spread scenes + window screenshots; never gated DET content |
| Window screenshot PNGs of time-driven scenes (007/008) | same-binary reruns differ; static scenes' PNGs (009/010/011) byte-identical across batteries | none |

**Noise-floor method (now standard):** to separate signal from environment, run the SAME
binary twice and diff; classes that move are environmental. Applied to every suspect class
in this closure.

**Also found and documented during (c):** the gt_regress accounting defect (KI-013): the
XFAIL scene (main_012) resets the sweep's FAILED flag, erasing failures from earlier scenes;
main_008b is currently masked by it (fails rc=75 in every observed battery). Pre-existing,
separate ticket.

## V1 (pre-build audit - mandatory item from the directive)

All six raster-framebuffer pipelines declare `create_disabled(3)` (replace/disabled for all
three attachments - not additive defaults): raster(4601) mesh(4683) mesh_batch(5111)
group(5192) mat_batch(5563) mat_light(6341). The only other blend state is the shadow pass
(count 1, different framebuffer; untouched).

## Verdict

**R0 = PASS.** Unit 1 (normal attachment) is additive-safe: all pre-existing deterministic
content byte-identical; new attachment numerically verified per design; every residual diff
class evidence-classified. Related tickets: KI-012 (window-size nondeterminism), KI-013
(harness accounting).

## Artifacts

- baseline:  C:\Users\opc\AppData\Local\Temp\opencode\r0_baseline\
- post:      C:\Users\opc\AppData\Local\Temp\opencode\r0_post2\
- noise:     C:\Users\opc\AppData\Local\Temp\opencode\nf\
- A/B runs:  C:\Users\opc\AppData\Local\Temp\opencode\x\ (expA2, s011_*, s018_*)
- build:     C:\Users\opc\AppData\Local\Temp\opencode\r0_build_2.log (exit 0)