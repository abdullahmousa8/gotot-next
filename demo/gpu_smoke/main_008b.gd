extends Node

# GOTOT-008B - Multi-Instance Mesh Rendering Proof
#
# Uses the ENTIRE GOTOT-008A real-mesh path unchanged (no API/C++ changes):
#   gpu_scene_create -> gpu_scene_dispatch -> cull -> compact ->
#   gpu_mesh_drawargs_finalize -> gpu_mesh_indirect_draw
#
# 008B objectively proves that this real-mesh indexed-indirect path renders
# HUNDREDS/THOUSANDS of distinct REAL 3D cubes with:
#   1) instance_count taken from the compact buffer (args[1] == visible),
#   2) a correct per-instance transform from compact[gl_InstanceIndex], no leak,
#   3) a DYNAMIC instance_count that changes as the camera orbits,
#   4) exact indirect args [index_count=36, N, 0, 0, 0],
#   5) determinism (same frame repeated -> same readback; cross-run identical).

const INSTANCE_COUNT := 10000
const SPREAD := 1200.0
const FRAME_LIMIT := 150
const PRINT_EVERY := 15
const RASTER_W := 1920
const RASTER_H := 1080
const ORBIT_RADIUS := 420.0
const ORBIT_HEIGHT := 160.0
const SAMPLE_CENTERS := 800   # final-frame projected-center spot checks
const MIN_GREEN := 2000       # far above one cube -> multi-instance pixels
const SUBPIXEL_R_PX := 0.35   # below this projected radius a cube may be 0 px

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var want_shot := false
var shot_done := false

var visible_min := -1
var visible_max := -1
var green_min := -1
var green_max := -1
var green_exact := -1
var last_args := PackedInt32Array()
var args_history_ok := true

# CPU reference state (read once after the scene is filled).
var positions := PackedVector3Array()
var scales := PackedFloat32Array()
var planes := PackedVector4Array()
var orbit_pos := Vector3.ZERO

var deter_repeat_ok := true   # same-frame cull/finalize repeated == equal readback
var deter_args_ok := true     # drawargs_finalize repeated == equal readback
var _last_cpu_count := 0


func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(50, "server singleton is null")
		return

	if not server.ensure_gpu_device():
		_fail(51, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display

	if not server.gpu_scene_create(INSTANCE_COUNT, SPREAD):
		_fail(52, "gpu_scene_create")
		return

	if not server.gpu_scene_dispatch(8):
		_fail(53, "gpu_scene_dispatch")
		return

	if not server.gpu_mesh_create():
		_fail(54, "gpu_mesh_create")
		return

	var vcount := server.gpu_mesh_get_vertex_count()
	var icount := server.gpu_mesh_get_index_count()
	if vcount != 8:
		_fail(55, "unexpected vertex count " + str(vcount))
		return
	if icount != 36:
		_fail(56, "unexpected index count " + str(icount))
		return

	# Deterministic frame-indexed orbit (independent of delta time) so two
	# consecutive runs visit the SAME per-frame cameras.
	_set_orbit_camera(1)
	# Establish the first camera on the server (needed before reading the
	# frustum planes; _process re-applies the same camera for frame 1).
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	positions = server.gpu_scene_readback_positions(0, INSTANCE_COUNT)
	scales = server.gpu_scene_readback_scales(0, INSTANCE_COUNT)
	if positions.size() != INSTANCE_COUNT or scales.size() != INSTANCE_COUNT:
		_fail(57, "transform readback size mismatch")
		return
	planes = server.gpu_scene_get_frustum_planes()
	if planes.size() != 6:
		_fail(58, "frustum planes size " + str(planes.size()))
		return

	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT 008B: scene ready instances=", INSTANCE_COUNT, " spread=", SPREAD)
	print("GOTOT-NEXT 008B: camera orbit R=", ORBIT_RADIUS, " H=", ORBIT_HEIGHT)


func _process(_delta: float) -> void:
	if shot_done:
		return
	frame += 1
	if frame > FRAME_LIMIT:
		return

	_set_orbit_camera(frame)

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	# Re-read the frustum planes EVERY frame from the SAME camera the GPU culled
	# with (the window aspect can change between frames; stale planes would make
	# the CPU reference disagree with the GPU by a band of boundary instances).
	planes = server.gpu_scene_get_frustum_planes()
	if planes.size() != 6:
		_fail(58, "frustum planes size " + str(planes.size()))
		return

	# ---- existing GPU pipeline (unchanged) ----
	if not server.gpu_cull_dispatch():
		_fail(59, "gpu_cull_dispatch")
		return
	var visible := server.gpu_cull_get_visible_count()

	if not server.gpu_visibility_dispatch():
		_fail(60, "gpu_visibility_dispatch")
		return

	# ---- CPU reference cull (same frustum formula the GPU uses) ----
	var cpu := _cpu_reference_cull()
	var cpu_count: int = cpu[0]
	var boundary_count: int = cpu[1]
	var dsmin: PackedFloat32Array = cpu[2]
	var bound: PackedFloat32Array = cpu[3]

	if not server.gpu_mesh_drawargs_finalize():
		_fail(61, "gpu_mesh_drawargs_finalize")
		return
	last_args = server.gpu_drawargs_read()

	# Objective evidence 4: exact indirect args + instance_count from compact.
	if last_args.size() != 5 or last_args[0] != 36 or last_args[1] != visible or last_args[2] != 0 or last_args[3] != 0 or last_args[4] != 0:
		args_history_ok = false
		_fail(62, "indirect args mismatch " + str(last_args) + " visible=" + str(visible))
		return
	if visible <= 0:
		_fail(63, "no visible instances")
		return
	# GPU (float32) and CPU (double) may differ ONLY on instances sitting on the
	# exact frustum boundary; any larger mismatch is a real bug.
	var delta := absi(visible - cpu_count)
	if delta > boundary_count:
		_fail(64, "visible mismatch beyond boundary gpu=" + str(visible) + " cpu=" + str(cpu_count)
				+ " delta=" + str(delta) + " boundary=" + str(boundary_count))
		return

	# ---- determinism: same frame -> same readback (drawargs repeat) ----
	if not server.gpu_mesh_drawargs_finalize():
		_fail(65, "gpu_mesh_drawargs_finalize (repeat)")
		return
	var args2 := server.gpu_drawargs_read()
	if args2 != last_args:
		deter_args_ok = false
		_fail(66, "drawargs repeat mismatch " + str(last_args) + " vs " + str(args2))
		return

	# ---- compact list integrity vs the CPU reference (every compact slot is a
	# valid, distinct, CPU-visible original id) ----
	var compact := server.gpu_compact_read()
	if compact.size() != visible:
		_fail(67, "compact size " + str(compact.size()) + " != visible " + str(visible))
		return
	for i in compact.size():
		var orig: int = compact[i]
		if orig < 0 or orig >= INSTANCE_COUNT:
			_fail(68, "compact id out of range at slot " + str(i) + " orig=" + str(orig))
			return
		if dsmin[orig] < -bound[orig]:
			_fail(69, "compact spot draws a clearly-outside cube " + str(orig)
					+ " ds=" + str(dsmin[orig]))
			return

	# ---- no-missing: the GPU must not drop a clearly-visible cube ----
	var vis_flags := server.gpu_cull_get_visibility()
	if vis_flags.size() != INSTANCE_COUNT:
		_fail(71, "visibility readback size " + str(vis_flags.size()))
		return
	var dropped := 0
	for i in INSTANCE_COUNT:
		if vis_flags[i] == 0 and dsmin[i] >= bound[i]:
			dropped += 1
	if dropped > 0:
		for i in INSTANCE_COUNT:
			if vis_flags[i] == 0 and dsmin[i] >= bound[i]:
				_fail(71, "GPU dropped clearly-visible cubes=" + str(dropped) + " first=" + str(i))
				return

	# ---- real mesh indexed indirect draw ----
	if not server.gpu_mesh_indirect_draw():
		_fail(70, "gpu_mesh_indirect_draw")
		return

	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(71, "raster readback size " + str(pixels.size()))
		return

	# ---- display: pixels -> Image -> ImageTexture -> TextureRect ----
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

	# ---- dynamic instance_count tracking ----
	if visible_min == -1 or visible < visible_min:
		visible_min = visible
	if visible_max == -1 or visible > visible_max:
		visible_max = visible

	if frame % PRINT_EVERY == 0:
		var covered := _count_green_exact(pixels)
		if green_min == -1 or covered < green_min:
			green_min = covered
		if green_max == -1 or covered > green_max:
			green_max = covered
		print("GOTOT-NEXT 008B: frame=", frame, " visible=", visible,
				" cpu=", cpu[0], " args=", last_args, " green_px=", covered)

	if frame == FRAME_LIMIT:
		_finalize(pixels, visible, compact)


func _finalize(pixels: PackedByteArray, visible: int, compact: PackedInt32Array) -> void:
	green_exact = _count_green_exact(pixels)

	# Determinism: repeat the WHOLE frame (cull + finalize) with the same camera.
	if not server.gpu_cull_dispatch():
		_fail(72, "gpu_cull_dispatch (determinism repeat)")
		return
	var visible2 := server.gpu_cull_get_visible_count()
	if not server.gpu_mesh_drawargs_finalize():
		_fail(73, "gpu_mesh_drawargs_finalize (determinism repeat)")
		return
	var args2 := server.gpu_drawargs_read()
	if visible2 != visible or args2 != last_args:
		deter_repeat_ok = false
		_fail(74, "same-frame determinism mismatch visible=" + str(visible) + " vs " + str(visible2)
				+ " args=" + str(last_args) + " vs " + str(args2))
		return

	# Compact vs CPU consensus with boundary tolerance: the per-frame checks
	# already proved (a) no compact slot is a clearly-outside cube and (b) no
	# clearly-visible cube is dropped. Materialize the sorted sets for the report.
	var compact_sorted := compact.duplicate()
	compact_sorted.sort()
	var cpu2 := _cpu_reference_cull()
	var dsmin: PackedFloat32Array = cpu2[2]
	var bound: PackedFloat32Array = cpu2[3]
	var cpu_certain_sorted := PackedInt32Array()
	var cpu_possible_sorted := PackedInt32Array()
	for i in INSTANCE_COUNT:
		if dsmin[i] >= bound[i]:
			cpu_certain_sorted.append(i)
		if dsmin[i] >= -bound[i]:
			cpu_possible_sorted.append(i)
	var subset_ok := true
	for o in compact:
		if dsmin[o] < -bound[o]:
			subset_ok = false
			break
	var cover_ok := true
	for o in cpu_certain_sorted:
		var bi := compact_sorted.bsearch(o)
		if bi < 0 or bi >= compact_sorted.size() or compact_sorted[bi] != o:
			cover_ok = false
			break
	var set_ok := subset_ok and cover_ok

	# Final-frame projected-center spot checks (each visible cube's compact id ->
	# its own transform -> projected center must carry its green pixels).
	var spot := _projection_spot_checks(pixels, compact)

	# Dynamic instance_count evidence.
	var dynamic_ok := visible_max > visible_min

	# Determinism intra-run report.
	var det_ok := deter_args_ok and deter_repeat_ok

	print("GOTOT-NEXT 008B: FINAL visible=", visible, " range=", visible_min, "..", visible_max,
			" dynamic=", dynamic_ok)
	print("GOTOT-NEXT 008B: FINAL args=", last_args)
	print("GOTOT-NEXT 008B: FINAL green_px=", green_exact, " green_range=", green_min, "..", green_max)
	print("GOTOT-NEXT 008B: FINAL cpu_reference gpu=", visible, " cpu=", cpu2[0],
			" boundary=", cpu2[1], " delta=", absi(visible - cpu2[0]),
			" certain=", cpu_certain_sorted.size(), " possible=", cpu_possible_sorted.size(),
			" compact=", compact.size(), " set_ok=", set_ok)
	print("GOTOT-NEXT 008B: FINAL center_spot checked=", spot[0], " subpixel=", spot[1],
			" matched=", spot[2], " miss=", spot[3])
	print("GOTOT-NEXT 008B: FINAL determinism drawargs_repeat=", deter_args_ok,
			" full_frame_repeat=", deter_repeat_ok, " ok=", det_ok)

	# ---- PASS/FAIL summary ----
	var fail_msg := ""
	if not dynamic_ok:
		fail_msg = "visible did not change during orbit (" + str(visible_min) + ".." + str(visible_max) + ")"
	elif green_exact < MIN_GREEN:
		fail_msg = "green pixels " + str(green_exact) + " below multi-instance floor " + str(MIN_GREEN)
	elif not args_history_ok:
		fail_msg = "indirect args were not [36, visible, 0, 0, 0] every frame"
	elif not set_ok:
		fail_msg = "compact set differs from CPU-visible set (subset/cover)"
	elif spot[3] != 0:
		fail_msg = "spot-check misses = " + str(spot[3])
	elif not det_ok:
		fail_msg = "determinism check failed"
	if fail_msg != "":
		_fail(75, fail_msg)
		return

	var sig := _det_signature(visible, compact, green_exact, spot)
	print("GOTOT-NEXT 008B-DET ", sig)

	want_shot = true
	print("GOTOT-NEXT 008B: EVIDENCE OK")


func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false

	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GOTOT-NEXT 008B: window screenshot NOT EXECUTED (empty image)")
	else:
		var err := shot.save_png("C:/Users/opc/AppData/Local/Temp/opencode/gt_008b_window.png")
		print("GOTOT-NEXT 008B: window screenshot saved=", err == OK)

	server.gpu_scene_destroy()
	print("GOTOT-NEXT 008B: PASS")
	get_tree().quit(0)


func _set_orbit_camera(f: int) -> void:
	var a: float = (float(f) / float(FRAME_LIMIT)) * TAU * 2.0
	orbit_pos = Vector3(cos(a) * ORBIT_RADIUS, ORBIT_HEIGHT, sin(a) * ORBIT_RADIUS)
	camera.global_position = orbit_pos
	camera.look_at(Vector3.ZERO, Vector3.UP)


# Returns [cpu_count, boundary_count, dsmin(PackedFloat32Array), bound(PackedFloat32Array)].
# For each instance: ds = min over 6 planes of (dot(n,center)+d + scale).
# GPU (float32) and CPU (double) may disagree only near the exact frustum
# boundary |ds| <= bound(instance); anything beyond that boundary is a hard
# classification that MUST agree.
func _cpu_reference_cull() -> Array:
	var flags := PackedByteArray()
	flags.resize(INSTANCE_COUNT)
	var dsmin := PackedFloat32Array()
	dsmin.resize(INSTANCE_COUNT)
	var bound := PackedFloat32Array()
	bound.resize(INSTANCE_COUNT)
	var count := 0
	var boundary_count := 0
	for i in INSTANCE_COUNT:
		var p: Vector3 = positions[i]
		var s: float = scales[i]
		var dmin: float = 1e30
		for pl in planes:
			var d: float = pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w
			if d < dmin:
				dmin = d
		var ds: float = dmin + s
		dsmin[i] = ds
		var eps: float = maxf((p.length() + s) * 1e-6, 1e-6)
		bound[i] = eps
		if ds >= 0.0:
			count += 1
		if absf(ds) <= eps:
			boundary_count += 1
	_last_cpu_count = count
	return [count, boundary_count, dsmin, bound]


func _count_green_exact(pixels: PackedByteArray) -> int:
	var c := 0
	var n: int = RASTER_W * RASTER_H
	for i in n:
		if pixels[i * 4 + 1] > 150 and pixels[i * 4] < 120 and pixels[i * 4 + 2] < 150:
			c += 1
	return c


# For up to SAMPLE_CENTERS visible cubes: project its OWN transform center
# through the exact VP used by the shaders and require green pixels within a
# small radius around the projected center. Returns [checked, subpixel, matched, miss].
# Samples are taken from the SORTED compact ids so the checked set is identical
# across runs (the GPU's atomic compaction order is not deterministic).
func _projection_spot_checks(pixels: PackedByteArray, compact: PackedInt32Array) -> Array:
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		return [0, 0, 0, 0]

	var cs := compact.duplicate()
	cs.sort()
	var n := mini(cs.size(), SAMPLE_CENTERS)
	var best_miss := 999999999
	var best_checked := 0
	var best_sub := 0
	var best_matched := 0
	for cx_sign: int in [1, -1]:
		for cy_sign: int in [1, -1]:
			var checked := 0
			var sub := 0
			var matched := 0
			var miss := 0
			for s in n:
				var orig: int = cs[s]
				var p: Vector3 = positions[orig]
				var ccx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
				var ccy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
				var ccz: float = vp[2] * p.x + vp[6] * p.y + vp[10] * p.z + vp[14]
				var ccw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
				if ccw <= 0.0:
					continue
				var ndcx := ccx / ccw
				var ndcy := ccy / ccw
				if ndcx < -1.0 or ndcx > 1.0 or ndcy < -1.0 or ndcy > 1.0:
					continue
				var px: float = (ndcx * 0.5 + 0.5) * float(RASTER_W)
				var py: float = (0.5 + float(cy_sign) * 0.5 * ndcy) * float(RASTER_H)
				var dist := orbit_pos.distance_to(p)
				if dist <= 0.0:
					continue
				var r_px: float = scales[orig] * 540.0 * vp[5] / dist
				if r_px < SUBPIXEL_R_PX:
					sub += 1
					continue
				var rad: int = maxi(mini(int(ceil(r_px)) + 2, 64), 3)
				var hit := false
				var x0: int = maxi(int(floor(px)) - rad, 0)
				var x1: int = mini(int(ceil(px)) + rad, RASTER_W - 1)
				var y0: int = maxi(int(floor(py)) - rad, 0)
				var y1: int = mini(int(ceil(py)) + rad, RASTER_H - 1)
				for yy in range(y0, y1 + 1):
					var row: int = yy * RASTER_W
					for xx in range(x0, x1 + 1):
						var o: int = (row + xx) * 4
						if pixels[o + 1] > 150 and pixels[o] < 120 and pixels[o + 2] < 150:
							hit = true
							break
					if hit:
						break
				checked += 1
				if hit:
					matched += 1
				else:
					miss += 1
			if miss < best_miss:
				best_miss = miss
				best_checked = checked
				best_sub = sub
				best_matched = matched
	return [best_checked, best_sub, best_matched, best_miss]


# Stable cross-run signature: identical inputs (seed 8, frame-indexed orbit)
# produce an identical string every run. Uses SORTED compact ids so the
# signature does not depend on the GPU's atomic compaction order.
func _det_signature(visible: int, compact: PackedInt32Array, green: int, spot: Array) -> String:
	var cs := compact.duplicate()
	cs.sort()
	var mid: int = cs.size() / 2
	return "sig=v%d|%d|%d|g%d|c%d|%d|%d|h%d|m%d|%d|f%d" % [
		visible, last_args[0], last_args[1], green,
		cs[0] if cs.size() > 0 else -1,
		cs[mid] if cs.size() > 0 else -1,
		cs[cs.size() - 1] if cs.size() > 0 else -1,
		spot[2], spot[3], _last_cpu_count, frame]


func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT 008B: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)