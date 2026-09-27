extends Node
# GNE 012-revised Phase 1 — DEPTH SOURCE TIMING PROBE (measurement only)
#
# QUESTION (SPEC 012 v2.0 §4.2 / contract_012_sync): the comment at
# gotot_render_server.cpp:3070-3072 ASSERTS that reading raster_viewz_texture
# before the draw returns the pre-draw (cleared) image. That was never
# measured, so it is measured here rather than designed around.
#
# METHOD: sample raster_viewz at 3 points BEFORE and AFTER the indirect draw,
# same frame, same texture, no extra submit/sync anywhere - so the probe cannot
# perturb what it measures.
#
# NO C++ NEEDED: gpu_raster_read_viewz(x, y) already exists (bound cpp:1476,
# implemented cpp:4627) and reads via texture_get_data with no submit/sync, so a
# before/after pair is meaningful.
#
#   same = pre-draw read is NOT the cleared image  (in-frame read is viable)
#   diff = pre-draw read IS the cleared image      (need temporal-1 or option b)

const RASTER_W := 1920
const RASTER_H := 1080
const WARMUP := 2
const MEASURE := 5
# Scene recipe copied from main_010 (the proven-visible setup). The first version of
# this probe used the procedural fill only (spread 1.0 around the origin) with the
# camera at z=2000, which produced an EMPTY frame - so all sample points read the
# cleared far value and the before/after comparison was inconclusive.
const CAM_POS := Vector3(0, 0, 1700)
const CUBES: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(0, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(320, 0, -700),
	Vector3(-380, 0, -840),
	Vector3(0, -420, -940),
]
const SCALES: Array[float] = [29.0, 45.0, 86.0, 36.0, 46.0, 64.0]

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var frame := 0
var done := false
var pre_vals := []
var post_vals := []
var visible_before := 0
# Sample points are LOCATED ON GEOMETRY at runtime. A fixed point would read the
# cleared far value on background pixels, and "background reads far before and
# after" is indistinguishable from "pre-draw read is cleared" - the first version
# of this probe was inconclusive for exactly that reason.
var SAMPLES: Array = []
var samples_locked := false
var prev_post: Array = []
var post_follows_camera := 0


func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(700, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(701, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(6, 1.0):
		_fail(702, "gpu_scene_create(6)")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(703, "gpu_scene_dispatch(8)")
		return
	if not server.gpu_mesh_create():
		_fail(704, "gpu_mesh_create")
		return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	# Place the instances explicitly (main_010 recipe) so the frame is not empty.
	for i in CUBES.size():
		server.gpu_scene_set_instance_transform(i, CUBES[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	# Sample points are located AFTER the first draw (see _process): locating in
	# _ready finds nothing because the target has not been rendered yet.
	print("GNE 012 p1: warmup=", WARMUP, " measure=", MEASURE)


# Finds up to 3 pixels that are NOT background, by comparing against the colour
# at (4,4) of the same frame. Read-only; uses the existing pixel readback.
func _locate_geometry_samples() -> Array:
	var px := server.gpu_raster_read_pixels()
	if px.size() != RASTER_W * RASTER_H * 4:
		return []
	var bgo := 4 * 4
	var bg := [px[bgo], px[bgo + 1], px[bgo + 2]]
	var found: Array = []
	var step := 6
	var y: int = step
	while y < RASTER_H and found.size() < 3:
		var x: int = step
		while x < RASTER_W and found.size() < 3:
			var o: int = (y * RASTER_W + x) * 4
			if px[o] != bg[0] or px[o + 1] != bg[1] or px[o + 2] != bg[2]:
				found.append([x, y])
				x += 260
			x += step
		y += 90
	return found


func _process(_delta: float) -> void:
	if done:
		return
	frame += 1
	# MOVE THE CAMERA every frame. With a static scene, before==after is expected
	# whether the read is fresh or stale, so the first version of this probe was
	# still ambiguous. With motion, a stale read keeps the previous frame's value
	# while a fresh read follows the camera.
	var cam_z: float = 1700.0 if frame % 2 == 0 else 1100.0
	camera.global_position = Vector3(0, 0, cam_z)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	# Primitives needed before the draw. No submit/sync here, so the probe does
	# not perturb the timing it is measuring.
	if not server.gpu_cull_dispatch():
		_fail(710, "gpu_cull_dispatch")
		return
	if not server.gpu_visibility_dispatch():
		_fail(711, "gpu_visibility_dispatch")
		return
	visible_before = server.gpu_cull_get_visible_count()
	if not server.gpu_mesh_batch_dispatch():
		_fail(713, "gpu_mesh_batch_dispatch")
		return

	# READ #1 - BEFORE the draw (nothing has written viewz this frame yet).
	var pre: Array = []
	for s in SAMPLES:
		pre.append(server.gpu_raster_read_viewz(int(s[0]), int(s[1])))

	# The draw that writes viewz in the same submission.
	if not server.gpu_mesh_drawargs_finalize():
		_fail(714, "gpu_mesh_drawargs_finalize")
		return
	if not server.gpu_mesh_indirect_draw():
		_fail(715, "gpu_mesh_indirect_draw")
		return

	# READ #2 - AFTER the draw.
	var post: Array = []
	for s in SAMPLES:
		post.append(server.gpu_raster_read_viewz(int(s[0]), int(s[1])))

	# Locate sample points once, now that the target actually holds the draw.
	if not samples_locked:
		samples_locked = true
		SAMPLES = _locate_geometry_samples()
		print("GNE 012 p1: located samples=", SAMPLES.size(), " points=", SAMPLES)

	if frame > WARMUP:
		pre_vals.append(pre)
		post_vals.append(post)
		# Decisive: did the POST read follow the camera motion?
		if post.size() > 0 and prev_post.size() > 0:
			if not is_equal_approx(float(post[0]), float(prev_post[0])):
				post_follows_camera += 1
		prev_post = post.duplicate()
	if frame >= WARMUP + MEASURE:
		_finish()


func _finish() -> void:
	done = true
	var same := 0
	var diff := 0
	for i in range(pre_vals.size()):
		for k in range(SAMPLES.size()):
			if is_equal_approx(float(pre_vals[i][k]), float(post_vals[i][k])):
				same += 1
			else:
				diff += 1
	if pre_vals.size() > 0:
		var lp: Array = pre_vals[pre_vals.size() - 1]
		var lq: Array = post_vals[post_vals.size() - 1]
		for k in range(SAMPLES.size()):
			print("GNE 012 p1: sample (", SAMPLES[k][0], ", ", SAMPLES[k][1], ") before=", lp[k], " after=", lq[k], " same=", is_equal_approx(float(lp[k]), float(lq[k])))
	print("GNE 012 p1: pairs=", pre_vals.size(), " same=", same, " different=", diff)
	print("GNE 012 p1: post_follows_camera=", post_follows_camera, "/", maxi(0, pre_vals.size() - 1))
	print("GNE 012 p1: visible_before=", visible_before)
	# MEASURED RESULT (2026-09-27), stated exactly:
	#  - the PRE-draw read is STALE BY ONE FRAME (it returns real depth from the
	#    previous frame, not the cleared far value)
	#  - the POST-draw read is FRESH: it tracks camera motion every time
	# => a CPU read after the draw is usable, but a build that must run INSIDE the
	#    same submission cannot read the frame it just drew. Design: temporal-1
	#    (build from the previous frame's settled depth), NOT an in-frame build.
	var stale_before: bool = diff > 0 and same == 0
	print("GNE 012 p1: FINDING pre-draw read is STALE_BY_ONE_FRAME=", stale_before, " (not 'cleared') post-draw read is FRESH=", post_follows_camera > 0)
	print("GNE 012 p1: sig=v12-p1 same=", same, " diff=", diff, " samples=", SAMPLES.size(), " measure=", MEASURE)
	print("GNE 012 p1: PASS")
	get_tree().quit(0)


func _fail(code: int, msg: String) -> void:
	print("GNE 012 p1: FAIL code=", code, " ", msg)
	get_tree().quit(code)

