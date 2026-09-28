extends Node
# GNE-020 - Presentation Overhaul: baseline / before-after measurement scene.
#
# Purpose: measure the PRESENTATION readback path as-is, so the 020 work
# (reduced-resolution present target + staging) has a recorded BEFORE and the
# SAME scene can produce the AFTER. This scene MEASURES; it does not optimise
# and claims no improvement.
#
# Path measured (each frame):
#   raster indirect draw -> presentation pixels readback -> Image ->
#   ImageTexture -> TextureRect.
# The depth readback is NOT part of the present path (it belonged to the
# 015.5 Phase-4 drift instruments) and is excluded on purpose.
#
# Timings here are wall-clock microsecond deltas from the module frame
# instrumentation (KI-001: no GPU timestamps on this tree). They are EVIDENCE
# ONLY - never a gate, never inside the signature (015.5 rule).
#
# The presentation size is feature-detected: on a build that ships the 020
# low-res present path, the scene reads through `gpu_present_read_pixels()`
# and asks `gpu_present_info()` for the active size; on older builds it runs
# the full-raster path unchanged. GNE_PRESENT_LOWRES=1 requests the low-res
# path when the module supports it.

const WARMUP_FRAMES := 10
const MEASURE_FRAMES := 60
const P_SCENE := 0
const P_CULL := 1
const P_CLUSTER := 2
const P_BATCH := 3
const P_RASTER := 4
const P_OUTPUT := 5
const RASTER_W := 1920
const RASTER_H := 1080

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""

var frame := 0
var done := false
var wall_series: Array = []
var run_t0 := 0
var reads := 0
var present_w := RASTER_W
var present_h := RASTER_H
var present_lowres := false
var present_api := false


func _ready() -> void:
	run_t0 = Time.get_ticks_msec()
	# Measurement mode: the frame loop is work-bound, so vsync must not
	# clamp the wall intervals at the refresh period (60 Hz = 16.6 ms would
	# hide any per-frame cost change). Identical for before/after runs.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(401, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(6, 1.0):
		_fail(402, "gpu_scene_create(6)")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "gpu_scene_dispatch(8) - scene fill pass")
		return
	if not server.gpu_mesh_create():
		_fail(404, "gpu_mesh_create")
		return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = Vector3(0, 0, 2000)
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	server.gpu_frame_set_warmup(WARMUP_FRAMES)
	# --- 020 present size selection (feature-detected; full path today) ---
	present_api = server.has_method("gpu_present_read_pixels")
	if OS.get_environment("GNE_PRESENT_LOWRES") == "1":
		if server.has_method("gpu_present_lowres_set"):
			server.call("gpu_present_lowres_set", true)
			present_lowres = true
		else:
			print("GNE 020: GNE_PRESENT_LOWRES=1 but the module has no low-res present path - running the full raster path")
	if present_lowres:
		if server.has_method("gpu_present_info"):
			var info: Dictionary = server.call("gpu_present_info")
			present_w = int(info["w"])
			present_h = int(info["h"])
		else:
			present_w = 960
			present_h = 540
	print("GNE 020: start tw=", present_w, " th=", present_h, " lowres=", present_lowres, " present_api=", present_api, " warmup=", WARMUP_FRAMES, " measure=", MEASURE_FRAMES)


func _process(_delta: float) -> void:
	if done:
		return
	frame += 1
	if frame > WARMUP_FRAMES + MEASURE_FRAMES:
		_finish()
		return

	server.gpu_frame_begin()
	server.gpu_frame_mark(P_SCENE)

	if not server.gpu_cull_dispatch():
		_fail(410, "gpu_cull_dispatch")
		return
	if not server.gpu_visibility_dispatch():
		_fail(411, "gpu_visibility_dispatch")
		return
	server.gpu_frame_mark(P_CULL)
	server.gpu_frame_mark(P_CLUSTER)

	if not server.gpu_mesh_batch_dispatch():
		_fail(413, "gpu_mesh_batch_dispatch")
		return
	server.gpu_frame_mark(P_BATCH)

	if not server.gpu_mesh_drawargs_finalize():
		_fail(414, "gpu_mesh_drawargs_finalize")
		return
	if not server.gpu_mesh_indirect_draw():
		_fail(415, "gpu_mesh_indirect_draw")
		return

	# Presentation readback - the 020 target read. Same call site for
	# before/after; the active target depends on the build + env flag only.
	var pixels: PackedByteArray = server.gpu_raster_read_pixels()
	if present_api:
		pixels = server.call("gpu_present_read_pixels")
	reads += 1
	server.gpu_frame_mark(P_RASTER)

	if pixels.size() == present_w * present_h * 4:
		var img := Image.create_from_data(present_w, present_h, false, Image.FORMAT_RGBA8, pixels)
		if image_tex == null:
			image_tex = ImageTexture.create_from_image(img)
			display.texture = image_tex
		else:
			image_tex.update(img)
	else:
		_fail(420, "present readback size " + str(pixels.size()) + " expected " + str(present_w * present_h * 4))
		return
	server.gpu_frame_mark(P_OUTPUT)
	server.gpu_frame_end()

	var st: Dictionary = server.gpu_frame_stats()
	wall_series.append(int(st["wall_last_us"]))


func _finish() -> void:
	done = true
	var st: Dictionary = server.gpu_frame_stats()
	var measured: Array = wall_series.slice(WARMUP_FRAMES, mini(wall_series.size(), WARMUP_FRAMES + MEASURE_FRAMES))
	var sorted := measured.duplicate()
	sorted.sort()
	var n := sorted.size()
	# nearest-rank percentiles over the measured window (method documented so
	# before/after use the same arithmetic)
	var p50 := 0
	var p95 := 0
	var pmin := 0
	var pmax := 0
	if n > 0:
		p50 = int(sorted[n / 2])
		p95 = int(sorted[mini(n - 1, int(ceil(0.95 * n)) - 1)])
		pmin = int(sorted[0])
		pmax = int(sorted[n - 1])
	var run_wall := Time.get_ticks_msec() - run_t0
	print("GNE 020: frames=", st["frames"], " warmup=", st["warmup"], " measured=", st["measured_frames"])
	print("GNE 020: wall_avg_us=", st["wall_avg_us"], " wall_peak_us=", st["wall_peak_us"], " pass_sum_us=", st["pass_sum_us"], " pass_share_pct=", int(st["pass_share_pct"]))
	print("GNE 020: p50_us=", p50, " p95_us=", p95, " min_us=", pmin, " max_us=", pmax, " samples=", n)
	print("GNE 020: present_tw=", present_w, " present_th=", present_h, " rb_bytes_per_frame=", present_w * present_h * 4, " lowres=", present_lowres)
	print("GNE 020: rb_total_bytes=", reads * present_w * present_h * 4, " reads=", reads)
	print("GNE 020: run_wall_ms=", run_wall)
	# Low-res verification (only runs in lowres mode): the GPU blit must be
	# the exact 2x2 box average of the full raster. Compares GPU output vs a
	# CPU-side box average of a fresh full read; tolerance <= 1 LSB (UNORM
	# round-to-nearest ties). Any pixel off by more than 1 fails the run.
	if present_lowres:
		var full_px: PackedByteArray = server.gpu_raster_read_pixels()
		var low_px: PackedByteArray = server.call("gpu_present_read_pixels")
		var bad := 0
		var maxdiff := 0
		if full_px.size() == RASTER_W * RASTER_H * 4 and low_px.size() == present_w * present_h * 4:
			for y in range(present_h):
				for x in range(present_w):
					var si := (y * 2 * RASTER_W + x * 2) * 4
					var di := (y * present_w + x) * 4
					for ch in range(4):
						var avg: int = int(full_px[si + ch]) + int(full_px[si + 4 + ch]) + int(full_px[si + RASTER_W * 4 + ch]) + int(full_px[si + RASTER_W * 4 + 4 + ch])
						var expect: int = int(round(avg / 4.0))
						var d := absi(expect - int(low_px[di + ch]))
						if d > maxdiff:
							maxdiff = d
						if d > 1:
							bad += 1
		else:
			bad = -1
		print("GNE 020: blit_verify bad_over1=", bad, " maxdiff=", maxdiff)
		if bad != 0:
			_fail(421, "blit verify failed bad=" + str(bad) + " maxdiff=" + str(maxdiff))
			return
	var sig := "v20|tw=%d|th=%d|rb=%d|rf=%d|d1" % [present_w, present_h, present_w * present_h * 4, MEASURE_FRAMES]
	print("GNE 020: sig=" + sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(1027, "sig file write failed")
			return
		f.store_string(sig + "\n")
	server.gpu_scene_destroy()
	print("GNE 020: PASS")
	get_tree().quit(0)


func _fail(code: int, msg: String) -> void:
	print("GNE 020: FAIL code=", code, " ", msg)
	get_tree().quit(code)
