extends Node

# GNE-016.5 slice-1 evidence scene (acceptance M2/M3/M4/M4b/M5; RFC docs/rfc_016_5_material_v2.md).
# Layout: 4 boxes; inst0 enlarged (120) so the checker texture resolves cleanly;
# inst1-3 flat (textured + flat coexistence). Checker: res://checkerboard.gtex,
# map slots carry the gpu_texture_load id (tex_arr index), scale = UV multiplier
# (triplanar fract(world * scale)).
# Instruments (pre-declared):
#  M2: |window mean shift| >= 0.05 at the inst0 face (maps vs flat) + stddev report.
#  M3: checker alternations along the face scanline; scale x2 -> count x2
#      (tolerance -2 edges for boundary truncation, documented convention).
#  M4: |read_normal delta| (max abs component) >= 0.05 with the normal channel set.
#  M4b: two identical ON-state draws byte-equal.
#  M5: draw p50 OFF vs ON (evidence).

const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)

const PTS: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(-700, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(320, 0, -700),
]
const SCALES: Array[float] = [120.0, 52.0, 140.0, 36.0]
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

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	_setup()

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 1800.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(4000, "server"); return
	if not server.ensure_gpu_device():
		_fail(4001, "device"); return
	if not server.gpu_scene_create(4, 1.0):
		_fail(4002, "scene_create"); return
	if not server.gpu_scene_dispatch(4):
		_fail(4003, "dispatch"); return
	if not server.gpu_mesh_create():
		_fail(4004, "mesh_create"); return
	for i in 4:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(4005, "batch"); return
	if not server.gpu_material_create():
		_fail(4006, "material_create"); return
	for i in 4:
		if not server.gpu_material_set_albedo(i, Color(1, 1, 1)):
			_fail(4007, "albedo"); return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(4007, "params"); return
		if not server.gpu_material_set_specular(i, Color(0.25, 0.25, 0.25), 32.0):
			_fail(4007, "specular"); return
	for i in POINTS.size():
		var q: Array = POINTS[i]
		if server.gpu_light_create(_mk_point(q[0], q[1], q[2], q[3])) != i:
			_fail(4008, "point id"); return
	for i in SPOTS.size():
		var s: Array = SPOTS[i]
		if server.gpu_light_create(_mk_spot(s[0], s[1], s[2], s[3], s[4], s[5])) != 16 + i:
			_fail(4008, "spot id"); return
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
		_fail(4010, "cull"); return
	if not server.gpu_visibility_dispatch():
		_fail(4011, "vis"); return
	if not server.gpu_mesh_batch_dispatch():
		_fail(4012, "batch"); return
	if not _redraw():
		return
	var pix_flat := server.gpu_raster_read_pixels()
	var vp := server.gpu_scene_get_vp()
	var fc := PTS[0] + Vector3(0, 0, SCALES[0] * 0.5)
	var fpx := _proj_px(fc, vp)
	var m_flat := _window_mean(pix_flat, fpx, 2)
	var sd_flat := _window_stddev(pix_flat, fpx, 4)
	print("GNE 016.5: flat m=", m_flat, " sd=", sd_flat, " face_px=", fpx)

	# load checker + bind + maps on inst0
	var tid := server.gpu_texture_load("res://checkerboard.gtex")
	print("GNE 016.5: checker id=", tid)
	if tid < 0:
		_fail(4020, "texture load"); return
	if not server.gpu_texture_bind(0, 0, tid):
		_fail(4021, "texture_bind"); return
	if not server.gpu_material_set_maps(0, tid, -1, -1, Vector2(0.0125, 0.0125)):
		_fail(4022, "set_maps"); return
	# M5: OFF-state cost reps (before enabling)
	var t_off: Array = []
	for r in range(3):
		var t0 := Time.get_ticks_usec()
		if not _redraw():
			return
		t_off.append(Time.get_ticks_usec() - t0)
	if not _redraw():
		return
	var pix_map := server.gpu_raster_read_pixels()
	var m_map := _window_mean(pix_map, fpx, 2)
	var sd_map := _window_stddev(pix_map, fpx, 4)
	var m2_shift := absf(m_map - m_flat)
	var m2_ok := m2_shift >= 0.05
	print("GNE 016.5: M2 map m=", m_map, " sd=", sd_map, " shift=", m2_shift, " pass=", m2_ok)

	# M3: alternations at scale s and 2s
	var n1 := _alternations(pix_map, fpx)
	if not server.gpu_material_set_maps(0, tid, -1, -1, Vector2(0.025, 0.025)):
		_fail(4023, "set_maps x2"); return
	if not _redraw():
		return
	var pix_map2 := server.gpu_raster_read_pixels()
	var n2 := _alternations(pix_map2, fpx)
	var m3_ok := (n2 >= 2 * n1 - 2) and n1 >= 2
	print("GNE 016.5: M3 alt s=", n1, " 2s=", n2, " pass=", m3_ok)

	# M4: normal channel via read_normal (checker as normal map; strong perturb)
	var n_flat := server.gpu_raster_read_normal(int(fpx.x), int(fpx.y))
	# reset scale to s for the normal test draw
	if not server.gpu_material_set_maps(0, tid, -1, tid, Vector2(0.0125, 0.0125)):
		_fail(4024, "set_maps normal"); return
	if not _redraw():
		return
	var pix_nm := server.gpu_raster_read_pixels()
	var n_map := server.gpu_raster_read_normal(int(fpx.x), int(fpx.y))
	var m4_delta := 0.0
	if n_flat.size() >= 3 and n_map.size() >= 3:
		m4_delta = maxf(absf(n_map[0] - n_flat[0]), maxf(absf(n_map[1] - n_flat[1]), absf(n_map[2] - n_flat[2])))
	var m4_ok := m4_delta >= 0.05
	print("GNE 016.5: M4 normals flat=", n_flat, " mapped=", n_map, " delta=", m4_delta, " pass=", m4_ok)

	# M4b: determinism (two identical ON-state draws)
	if not _redraw():
		return
	var pix_a := server.gpu_raster_read_pixels()
	if not _redraw():
		return
	var pix_b := server.gpu_raster_read_pixels()
	var det_ok: bool = (pix_a == pix_b)
	print("GNE 016.5: M4b determinism=", det_ok)

	# M5: ON-state cost reps
	var t_on: Array = []
	for r in range(3):
		var t1 := Time.get_ticks_usec()
		if not _redraw():
			return
		t_on.append(Time.get_ticks_usec() - t1)
	print("GNE 016.5: M5 cost off_us=", t_off, " on_us=", t_on)
	_show(pix_nm)
	var sig := "v165|m2=%.4f|n1=%d|n2=%d|m4=%.4f|det=%d|off=%d|on=%d|d1" % [m2_shift, n1, n2, m4_delta, int(det_ok), _p50(t_off), _p50(t_on)]
	print("GNE 016.5: sig=", sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(4025, "sig file"); return
		f.store_string(sig + "\n")
	if m2_ok and m3_ok and m4_ok and det_ok:
		print("GNE 016.5: SLICE1 PASS")
	else:
		print("GNE 016.5: SLICE1 FAIL m2=", m2_ok, " m3=", m3_ok, " m4=", m4_ok, " det=", det_ok)
	server.gpu_scene_destroy()
	get_tree().quit(0)

func _alternations(pixels: PackedByteArray, c: Vector2) -> int:
	# Count luminance direction changes along the face scanline (|step| > 0.03).
	var n := 0
	var last_dir := 0
	var y := int(c.y)
	var x := int(c.x) - 49
	var x_end := int(c.x) + 49
	var prev := _brightness(pixels, Vector2(x, y))
	x += 1
	while x <= x_end:
		var cur := _brightness(pixels, Vector2(x, y))
		var d := cur - prev
		if absf(d) > 0.03:
			var dir := 1 if d > 0.0 else -1
			if last_dir != 0 and dir != last_dir:
				n += 1
			last_dir = dir
		prev = cur
		x += 1
	return n

func _window_mean(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			s += _brightness(pixels, c + Vector2(dx, dy))
			n += 1
	return s / float(n)

func _window_stddev(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var m := _window_mean(pixels, c, r)
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var d := _brightness(pixels, c + Vector2(dx, dy)) - m
			s += d * d
			n += 1
	return sqrt(s / float(n))

func _proj_px(p: Vector3, vp: PackedFloat32Array) -> Vector2:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * RASTER_W
	var c0 := Vector2(px, (ny * 0.5 + 0.5) * RASTER_H)
	var c1 := Vector2(px, (0.5 - ny * 0.5) * RASTER_H)
	if c0.x >= 4 and c0.y >= 4 and c0.x < RASTER_W - 4 and c0.y < RASTER_H - 4:
		return c0
	return c1

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(4030, "recull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(4031, "revis"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(4032, "rebatch"); return false
	if not server.gpu_material_draw_lights():
		_fail(4033, "draw"); return false
	return true

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _p50(a: Array) -> int:
	var s := a.duplicate()
	s.sort()
	return int(s[s.size() / 2])

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

func _fail(code: int, msg: String) -> void:
	print("GNE 016.5: FAIL code=", code, " ", msg)
	get_tree().quit(code)