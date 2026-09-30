extends Node
# GNE 015.5 Phase 4 — frame-time drift DIAGNOSIS (measurement only)
#
# Purpose: produce the BASELINE that Phase 5 (async readback) is compared
# against. This scene MEASURES; it does not optimise and claims no improvement.
#
# Method (API verified against this 4.8.dev tree, see the Phase-4 block in
# gne_render_server.cpp): RD::capture_timestamp() marks a point on the GPU
# timeline; get_captured_timestamp_gpu_time() returns NANOSECONDS for the
# PREVIOUS completed frame; get_captured_timestamp_cpu_time() returns
# MICROSECONDS. Per-pass numbers are CPU-side microsecond deltas between markers.
#
# Schedule: 100 warm-up frames (kept for the peak) + 200 measured = 300 total.

const WARMUP_FRAMES := 100
const MEASURE_FRAMES := 200
const P_SCENE := 0
const P_CULL := 1
const P_CLUSTER := 2
const P_BATCH := 3
const P_RASTER := 4
const P_OUTPUT := 5
const RASTER_W := 1920
const RASTER_H := 1080

const PROBE_FRAMES := [1, 100, 200, 300]

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var shot_done := false
var probe_wall := {}
var probe_gpu := {}
var wall_series: Array = []
var pass_count_ok := 0
# Differential probe: skip the raster readbacks to measure their share of the
# raster bucket. Set by --noreadback.
var noreadback := false
# Gate the synchronous visible-count readback (see the cull pass below).
var want_visible := false


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--noreadback":
			noreadback = true
		elif a == "--needvisible":
			want_visible = true
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "server singleton is null")
		return
	# KI-001 (corrected 2026-09-30): "max_timestamp_query_elements" defaults to 256
	# in this tree (GLOBAL_DEF_RST range 256..65535), so the query pool was never
	# disabled - it has always accepted captures. The MODULE now prints the
	# effective value at device creation, so every run states it without any scene
	# cooperating; this scene deliberately no longer sets it. What is measured
	# here: the pool accepts every capture (gpu_capture_count = frames x 7) while a
	# LOCAL RenderingDevice never reaches RenderingDevice::_begin_frame() - the
	# only place that publishes timestamp_result_count (rendering_device.cpp:8342)
	# - so gpu_result_count stays 0 and no GPU value reaches a reader. Columns stay
	# NA, never zero cost; the wall-clock measurement is the load-bearing one.
	print("GNE 015.5 p5: timestamp_query_elements=", int(ProjectSettings.get_setting("debug/settings/profiler/max_timestamp_query_elements")), " (effective value, printed by the module too; GPU results still unavailable - KI-001)")
	if not server.ensure_gpu_device():
		_fail(401, "local RenderingDevice not available")
		return

	if not server.ensure_gpu_device():
		_fail(401, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(6, 1.0):
		_fail(402, "gpu_scene_create(6)")
		return
	# The scene buffers are filled by a GPU dispatch pass (seed 8 = the same
	# seed the other scenes use), NOT by gpu_scene_create alone. Without this
	# the cull has no instances and fails with code=410.
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
	# REQUIRED before any cull: sets frustum_valid. Without it gpu_cull_dispatch
	# fails with code=410 ("no camera. Call gpu_scene_set_camera first").
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	# Set the warm-up prefix explicitly (Phase 4 uses 100); per-pass samples are
	# cleared at that boundary so averages cover the MEASURED window only.
	server.gpu_frame_set_warmup(WARMUP_FRAMES)
	print("GNE 015.5 p4: warmup=", WARMUP_FRAMES, " measure=", MEASURE_FRAMES)


func _process(_delta: float) -> void:
	if shot_done:
		return
	frame += 1
	if frame > WARMUP_FRAMES + MEASURE_FRAMES:
		_finish()
		return

	server.gpu_frame_begin()

	# pass 0 — scene_update: instance transforms are static in this scene, so the
	# slot is a measured marker rather than work.
	server.gpu_frame_mark(P_SCENE)

	# pass 1 — cull: frustum + HZB visibility.
	if not server.gpu_cull_dispatch():
		_fail(410, "gpu_cull_dispatch")
		return
	# gpu_cull_get_visible_count() is a SYNCHRONOUS 4-byte GPU readback
	# (cpp:4148 -> buffer_get_data), so it stalls the whole pipeline. Measured
	# cost here: 4.26 ms/frame, 18% of this scene's frame. Its result was never
	# read by this scene, so by default it is not called at all. Ask for it with
	# --needvisible only when the number itself is the thing being measured.
	if want_visible:
		var visible: int = server.gpu_cull_get_visible_count()
	if not server.gpu_visibility_dispatch():
		_fail(411, "gpu_visibility_dispatch")
		return
	server.gpu_frame_mark(P_CULL)

	# pass 2 — cluster_cull: this scene has no meshlet data, so the slot exists
	# to keep the timeline complete; it is a no-op marker, NOT a culling cost.
	server.gpu_frame_mark(P_CLUSTER)

	# pass 3 — batch_assembly.
	if not server.gpu_mesh_batch_dispatch():
		_fail(413, "gpu_mesh_batch_dispatch")
		return
	server.gpu_frame_mark(P_BATCH)

	# pass 4 — raster: the real indirect draw plus BOTH readbacks (this is the
	# pass Phase 5 will attack, so it must be measured here).
	if not server.gpu_mesh_drawargs_finalize():
		_fail(414, "gpu_mesh_drawargs_finalize")
		return
	if not server.gpu_mesh_indirect_draw():
		_fail(415, "gpu_mesh_indirect_draw")
		return
	# The two readbacks below sit BETWEEN mark(P_BATCH) and mark(P_RASTER), and
	# gpu_frame_mark(p) accumulates the interval since the PREVIOUS mark, so
	# their cost lands in the RASTER bucket - not output. --noreadback skips
	# them so the readback share of raster can be measured instead of assumed.
	var pixels := PackedByteArray()
	var depth := PackedFloat32Array()
	if not noreadback:
		pixels = server.gpu_raster_read_pixels()
		depth = server.gpu_raster_read_depth()
	server.gpu_frame_mark(P_RASTER)

	# pass 5 — output: CPU-side presentation.
	if pixels.size() == RASTER_W * RASTER_H * 4:
		var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
		if image_tex == null:
			image_tex = ImageTexture.create_from_image(img)
			display.texture = image_tex
		else:
			image_tex.update(img)
	server.gpu_frame_mark(P_OUTPUT)
	server.gpu_frame_end()

	# The interval closed by this begin() is the previous frame's wall time.
	var st: Dictionary = server.gpu_frame_stats()
	wall_series.append(int(st["wall_last_us"]))
	if frame in PROBE_FRAMES:
		probe_wall[frame] = int(st["wall_last_us"])
		probe_gpu[frame] = int(st["gpu_last_ns"])
	if frame == 1 or frame == 50 or frame == WARMUP_FRAMES or frame == WARMUP_FRAMES + MEASURE_FRAMES:
		print("GNE 015.5 p4: dbg frame=", frame, " wall_last_us=", st["wall_last_us"], " gpu_last_ns=", st["gpu_last_ns"], " frames=", st["frames"])



func _finish() -> void:
	shot_done = true
	pass_count_ok = int(server.gpu_frame_stats()["measured_frames"])
	_report(server.gpu_frame_stats())
	server.gpu_scene_destroy()
	print("GNE 015.5 p4: PASS")
	get_tree().quit(0)


func _report(st: Dictionary) -> void:
	print("GNE 015.5 p4: frames=", st["frames"], " warmup=", st["warmup"], " measured=", st["measured_frames"])
	var names: PackedStringArray = st["pass_names"]
	var per: PackedFloat64Array = st["pass_cpu_us"]
	var line := "GNE 015.5 p4: per-pass wall(us)"
	for i in range(mini(names.size(), per.size())):
		line += " " + names[i] + "=" + str(int(per[i]))
	print(line)
	print("GNE 015.5 p4: pass_sum_us=", st["pass_sum_us"], " pass_share_pct=", int(st["pass_share_pct"]), " frame_total_wall_us=", st["frame_total_wall_us"])
	print("GNE 015.5 p4: wall_avg_us=", st["wall_avg_us"], " wall_peak_us=", st["wall_peak_us"], " wall_first_us=", st["wall_first_us"], " warmup_peak_us=", st["warmup_wall_peak_us"])
	# GPU nanoseconds are UNAVAILABLE, not zero, when the engine's query pool is
	# disabled (debug/settings/profiler/max_timestamp_query_elements = 0). The
	# flag makes that explicit so no reader mistakes "not measured" for "free".
	if bool(st["gpu_timestamps_available"]):
		print("GNE 015.5 p4: gpu_avg_ns=", st["gpu_avg_ns"], " gpu_avg_us=", st["gpu_avg_us"], " gpu_first_ns=", st["gpu_first_ns"])
	else:
		print("GNE 015.5 p4: gpu UNAVAILABLE (query pool disabled) - gpu_avg_ns=NA gpu_first_ns=NA")
	# KI-001 evidence pair, printed so the limit is reproducible instead of
	# asserted: captures > 0 with results == 0 means the pool exists and accepted
	# every capture, but the device never published a single result. The pool is
	# now enabled by the MODULE (ensure_gpu_device), not by this scene.
	print("GNE 015.5 p4: gpu_capture_count=", st["gpu_capture_count"], " gpu_result_count=", st["gpu_result_count"],
		" (results 0 = local device never reaches RenderingDevice::_begin_frame; engine-side limit, KI-001)")
	for f in PROBE_FRAMES:
		if probe_wall.has(f):
			print("GNE 015.5 p4: probe frame=", f, " wall_us=", probe_wall[f], " gpu_ns=", probe_gpu[f])
	_report_drift()


func _report_drift() -> void:
	var n: int = wall_series.size()
	if n < 10:
		print("GNE 015.5 p4: drift NOT REPORTED (only ", n, " samples; need >= 10)")
		return
	# Compare the first half of the MEASURED window with the second half. The
	# comparison is made on the ordered series, never on two hand-picked frames.
	var half: int = n / 2
	var first_sum := 0.0
	var last_sum := 0.0
	for i in range(0, half):
		first_sum += float(wall_series[i])
	for i in range(half, n):
		last_sum += float(wall_series[i])
	var first_avg: float = first_sum / float(half)
	var last_avg: float = last_sum / float(maxi(1, n - half))
	print("GNE 015.5 p4: drift first_half_avg_us=", int(first_avg), " last_half_avg_us=", int(last_avg), " delta_us=", int(last_avg - first_avg), " samples=", n)
	# DET-SAFE SIGNATURE: contains ONLY content that must be reproducible.
	# Timings (wall_avg_us, drift_us) are MEASUREMENTS, not content, and they
	# vary run to run - including the SIGN of the drift. A previous revision
	# folded them into the signature, which made a 2-run DET comparison fail by
	# construction (this is the same class of bug as the 011 dispatch_us/draw_us
	# pair, fixed in commit 2). So: content-only sig + a separate timing line.
	print("GNE 015.5 p4: timings wall_avg_us=", int(first_avg), " drift_us=", int(last_avg - first_avg), " samples=", n)
	print("GNE 015.5 p4: sig=v15.5-p4 warm=", WARMUP_FRAMES, " meas=", MEASURE_FRAMES, " passes=", pass_count_ok)


func _fail(code: int, msg: String) -> void:
	print("GNE 015.5 p4: FAIL code=", code, " ", msg)
	get_tree().quit(code)

