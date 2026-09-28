extends Node

# GNE-018 — clustered forward+ closure scene (criteria L1-L8, SPEC 018 FINAL).
#
# 4 cubes (main_010 proven recipe: camera (0,0,1700) identity, all visible).
# 20 lights: 16 point + 4 spot (D6-1). Canonical directional L from 016 stays
# frozen; new lights add. PER_MESH strategy; draw via gpu_material_draw_lights.
# L5 uses light MOVE (not destroy) so ids never churn and counts stay exact.

const INSTANCE_COUNT := 4
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)

const PTS: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(-700, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(320, 0, -700),
]
const SCALES: Array[float] = [29.0, 52.0, 140.0, 36.0]

# 16 point lights: 0 front-center (L1 probe), 1 red, 2 green, 3 blue,
# 4-13 white shell, 14-15 far (L5 zero-cluster).
const POINTS: Array = [
	[Vector3(0, 0, -200), 800.0, Color(1, 1, 1), 0.45],
	[Vector3(320, 0, -400), 380.0, Color(1, 0, 0), 0.6],
	[Vector3(-150, 100, -700), 380.0, Color(0, 1, 0), 0.9],
	[Vector3(0, -400, -700), 380.0, Color(0, 0, 1), 0.25],
	[Vector3(0, 500, -800), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(-400, -300, -400), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(400, -300, -400), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(-400, 300, -1000), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(400, 300, -1000), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(0, 0, -300), 350.0, Color(1, 1, 1), 0.11],
	[Vector3(-200, 400, -600), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(200, -400, -600), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(0, 200, -1200), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(0, -200, -1200), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(5000, 5000, 5000), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(-5000, -5000, 5000), 420.0, Color(1, 1, 1), 0.11],
]
# 4 spots from the front-top, aimed at instances (L2 uses spot 0 -> inst 1).
const SPOTS: Array = [
	[Vector3(0, 800, 500), Vector3(-700, 0, -800), 0.2, 0.5, Color(1, 1, 1), 1.1],
	[Vector3(-600, 800, 500), Vector3(-350, 150, -850), 0.2, 0.5, Color(1, 1, 1), 0.18],
	[Vector3(600, 800, 500), Vector3(350, -150, -900), 0.2, 0.5, Color(1, 1, 1), 0.18],
	[Vector3(0, 800, 500), Vector3(320, 0, -700), 0.15, 0.4, Color(1, 0.9, 0.8), 0.18],
]

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""
var shot_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
		elif a.begins_with("--shot="):
			shot_file = a.split("=")[1]
	_setup()

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 1800.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(1000, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(1001, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(1002, "gpu_scene_create(4)")
		return
	if not server.gpu_scene_dispatch(4):
		_fail(1003, "gpu_scene_dispatch(4)")
		return
	if not server.gpu_mesh_create():
		_fail(1004, "gpu_mesh_create")
		return
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(1005, "set_batch_strategy(PER_MESH)")
		return
	if not server.gpu_material_create():
		_fail(1006, "gpu_material_create")
		return
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, Color(1, 1, 1)):
			_fail(1007, "set_albedo " + str(i))
			return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(1007, "set_params " + str(i))
			return
		if not server.gpu_material_set_specular(i, Color(0.25, 0.25, 0.25), 32.0):
			_fail(1007, "set_specular " + str(i))
			return
	# 16 points (ids 0-15) + 4 spots (ids 16-19) = 20 lights.
	for i in POINTS.size():
		var q: Array = POINTS[i]
		if server.gpu_light_create(_mk_point(q[0], q[1], q[2], q[3])) != i:
			_fail(1008, "point id != " + str(i))
			return
	for i in SPOTS.size():
		var s: Array = SPOTS[i]
		if server.gpu_light_create(_mk_spot(s[0], s[1], s[2], s[3], s[4], s[5])) != 16 + i:
			_fail(1008, "spot id != " + str(16 + i))
			return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_measure()

func _measure() -> void:
	if not server.gpu_cull_dispatch():
		_fail(1010, "gpu_cull_dispatch")
		return
	if server.gpu_cull_get_visible_count() != INSTANCE_COUNT:
		_fail(1011, "frustum must keep all 4")
		return
	if not server.gpu_visibility_dispatch():
		_fail(1012, "gpu_visibility_dispatch")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(1013, "gpu_mesh_batch_dispatch")
		return
	print("GNE 018: DBG batch_count=", server.gpu_mesh_get_batch_count(), " args0=", server.gpu_mesh_get_batch_args(0), " compact=", server.gpu_compact_read(), " drawcounts=", server.gpu_mesh_get_draw_counts())
	if not server.gpu_material_draw_lights():
		_fail(1014, "gpu_material_draw_lights")
		return
	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(1015, "pixels size")
		return
	_show(pixels)
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		_fail(1016, "vp size")
		return
	for _ii in INSTANCE_COUNT:
		var _cc := _proj(PTS[_ii], vp)
		if not _cc.is_empty():
			print("GNE 018: DBG inst", _ii, " center brightness=", _brightness(pixels, _cc[0]), " / ", _brightness(pixels, _cc[1]))
	var st: Dictionary = server.gpu_light_get_stats()
	# --- L3: 20 lights, zero overflow ---
	if int(st["count"]) != 20 or int(st["overflows"]) != 0:
		_fail(1017, "stats wrong " + str(st))
		return
	print("GNE 018: L3 stats OK ", st)
	# --- L4: clusters active + spot check under point 0 ---
	var touched := int(st["clusters_touched"])
	var assigns := int(st["assignments"])
	print("GNE 018: L4 touched=", touched, " assignments=", assigns)
	if touched <= 0 or assigns <= 0:
		_fail(1018, "no cluster assignments")
		return
	var probe := _proj(PTS[0] + Vector3(0, 0, SCALES[0] * 0.5), vp)
	if probe.is_empty():
		_fail(1019, "probe projection failed")
		return
	var pc: Vector2 = _brighter(probe)
	var cl := _cluster_of(pc, vp)
	var dbg: PackedInt32Array = server.gpu_light_debug_cluster(cl[0], cl[1], cl[2])
	print("GNE 018: L4 cluster=", cl, " dbg=", dbg)
	if dbg.is_empty() or dbg[0] <= 0:
		_fail(1018, "probe cluster empty")
		return
	var found := false
	for k in range(1, dbg.size()):
		if dbg[k] == 0:
			found = true
	if not found:
		_fail(1018, "light 0 not in its cluster")
		return
	print("GNE 018: L4 cluster evidence OK")
	# --- L1: point 0 ON vs intensity-0 ---
	var b1 := _brightness(pixels, pc)
	var p0: Array = POINTS[0]
	if not server.gpu_light_update(0, _mk_point(p0[0], p0[1], p0[2], 0.0)):
		_fail(1020, "point0 off")
		return
	if not _redraw():
		return
	var pixels_off := server.gpu_raster_read_pixels()
	var b2 := _brightness(pixels_off, pc)
	var changed := _changed_count(pixels, pixels_off, 0.02)
	print("GNE 018: L1 probe ", b1, " -> ", b2, " changed=", changed)
	if b1 - b2 < 0.1:
		_fail(1021, "point light had no effect on probe")
		return
	if changed < 1000:
		_fail(1021, "too few changed pixels")
		return
	if not server.gpu_light_update(0, _mk_point(p0[0], p0[1], p0[2], p0[3])):
		_fail(1020, "point0 restore")
		return
	print("GNE 018: L1 point response OK")
	# --- L2: spot 0 ON vs OFF (inside vs outside cone) ---
	if not _redraw():
		return
	var pin := _window_mean(server.gpu_raster_read_pixels(), _anchor_inst(vp, 1), 2)
	var pout := _window_mean(server.gpu_raster_read_pixels(), _anchor_inst(vp, 3), 2)
	var s0: Array = SPOTS[0]
	if not server.gpu_light_update(16, _mk_spot(s0[0], s0[1], s0[2], s0[3], s0[4], 0.0)):
		_fail(1022, "spot0 off")
		return
	if not _redraw():
		return
	var qin := _window_mean(server.gpu_raster_read_pixels(), _anchor_inst(vp, 1), 2)
	var qout := _window_mean(server.gpu_raster_read_pixels(), _anchor_inst(vp, 3), 2)
	print("GNE 018: L2 inside ", pin, " -> ", qin, " outside ", pout, " -> ", qout)
	if pin / maxf(qin, 1e-3) < 2.0:
		_fail(1023, "spot cone falloff not measurable")
		return
	if qout / maxf(pout, 1e-3) > 1.5 and pout / maxf(qout, 1e-3) > 1.5:
		_fail(1023, "control window moved too much")
		return
	if not server.gpu_light_update(16, _mk_spot(s0[0], s0[1], s0[2], s0[3], s0[4], s0[5])):
		_fail(1022, "spot0 restore")
		return
	print("GNE 018: L2 spot cone OK")
	# --- L5: far lights contribute zero clusters (move, never destroy) ---
	var A_t := int(server.gpu_light_get_stats()["assignments"])
	var p14: Array = POINTS[14]
	var p15: Array = POINTS[15]
	if not server.gpu_light_update(14, _mk_point(Vector3(9000, 9000, 9000), p14[1], p14[2], p14[3])):
		_fail(1024, "move far 14")
		return
	if not server.gpu_light_update(15, _mk_point(Vector3(-9000, -9000, 9000), p15[1], p15[2], p15[3])):
		_fail(1024, "move far 15")
		return
	if not _redraw():
		return
	var B_t := int(server.gpu_light_get_stats()["assignments"])
	print("GNE 018: L5 assignments far=", A_t, " farther=", B_t)
	if A_t != B_t:
		_fail(1025, "zero-cluster lights changed assignments")
		return
	if not server.gpu_light_update(14, _mk_point(p14[0], p14[1], p14[2], p14[3])):
		_fail(1024, "restore far 14")
		return
	if not server.gpu_light_update(15, _mk_point(p15[0], p15[1], p15[2], p15[3])):
		_fail(1024, "restore far 15")
		return
	print("GNE 018: L5 zero-cluster culling OK")
	# --- L6 + signature (canonical full state) ---
	if not _redraw():
		return
	var fin := server.gpu_raster_read_pixels()
	_show(fin)
	var h := _histogram(fin)
	var chroma := _chromatic_count(fin)
	print("GNE 018: L6 hr=", h[1], " chromatic=", chroma)
	if h[1] < 0.3:
		_fail(1026, "histogram range too narrow")
		return
	if chroma < 500:
		_fail(1026, "no chromatic multi-light mixing")
		return
	var st2: Dictionary = server.gpu_light_get_stats()
	var sig := "v18|lc=%d|cc=%d|ot=%d|hr=%.2f|d1" % [int(st2["count"]), int(st2["clusters_touched"]), int(st2["overflows"]), h[1]]
	print("GNE 018: sig=" + sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(1027, "sig file write failed")
			return
		f.store_string(sig + "\n")
	print("GNE 018: PASS")
	get_tree().quit(0)

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(1040, "recull")
		return false
	if not server.gpu_visibility_dispatch():
		_fail(1041, "revisibility")
		return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(1042, "rebatch")
		return false
	if not server.gpu_material_draw_lights():
		_fail(1043, "redraw_lights")
		return false
	return true

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex
	if shot_file != "":
		img.save_png(shot_file)
		print("GNE 018: shot saved to ", shot_file)

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _proj(p: Vector3, vp: PackedFloat32Array) -> Array:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return []
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * RASTER_W
	return [Vector2(px, (ny * 0.5 + 0.5) * RASTER_H), Vector2(px, (0.5 - ny * 0.5) * RASTER_H)]

func _brighter(cands: Array) -> Vector2:
	var best := Vector2(-1, -1)
	var best_b := -1.0
	for cand in cands:
		var p: Vector2 = cand
		if p.x < 4 or p.y < 4 or p.x >= RASTER_W - 4 or p.y >= RASTER_H - 4:
			continue
		best = p
		break
	return best

func _anchor_inst(vp: PackedFloat32Array, inst: int) -> Vector2:
	# Instance +Z face center (faces the identity camera); first y-flip
	# candidate in-bounds wins (deterministic, no brightness chase).
	var c := PTS[inst] + Vector3(0, 0, SCALES[inst] * 0.5)
	return _brighter(_proj(c, vp))

func _cluster_of(p: Vector2, vp: PackedFloat32Array) -> Array:
	# Tile from framebuffer pixels + depth slice from view depth (matches the
	# C++ slice math exactly: same constants, same formula). Y flipped to the
	# cull convention: framebuffer row 0 (top) is cull row 8.
	var tx := clampi(int(p.x / RASTER_W * 16.0), 0, 15)
	var ty := clampi(8 - int(p.y / RASTER_H * 9.0), 0, 8)
	var v: Vector3 = camera.global_transform.affine_inverse() * PTS[0]
	var z := -v.z
	var tz := clampi(int(floor(24.0 * log(z / 300.0) / log(4000.0 / 300.0))), 0, 23)
	return [tx, ty, tz]

func _window_mean(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			s += _brightness(pixels, c + Vector2(dx, dy))
			n += 1
	return s / float(n)

func _changed_count(a: PackedByteArray, b: PackedByteArray, tol: float) -> int:
	var n := 0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var ba := (float(a[o]) + float(a[o + 1]) + float(a[o + 2])) / 765.0
		var bb := (float(b[o]) + float(b[o + 1]) + float(b[o + 2])) / 765.0
		if absf(ba - bb) > tol:
			n += 1
	return n

func _chromatic_count(pixels: PackedByteArray) -> int:
	var n := 0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var r := float(pixels[o])
		var g := float(pixels[o + 1])
		var b := float(pixels[o + 2])
		if maxf(r, maxf(g, b)) - minf(r, minf(g, b)) > 0.3 * 255.0:
			n += 1
	return n

func _histogram(pixels: PackedByteArray) -> Array:
	var bins := PackedInt32Array()
	bins.resize(16)
	var mn := 1.0
	var mx := 0.0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var b := (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0
		if b < mn:
			mn = b
		if b > mx:
			mx = b
		var bin := mini(15, int(b * 16.0))
		bins[bin] += 1
	var peak := 0
	for v in bins:
		peak = maxi(peak, v)
	return [0, mx - mn, peak]

func _fail(code: int, msg: String) -> void:
	print("GNE 018: FAIL code=", code, " ", msg)
	get_tree().quit(code)
