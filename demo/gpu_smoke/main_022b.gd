extends Node

var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""
# GNE-022 S1b: probe field on the dense GNE-021 city + trace-budget measurement.
# Methodology mirrors GNE-021: raw numbers, same-session runs, no timings in sigs.
# Field: 16x8x16 = 2048 probes x 64 fixed rays = 131,072 rays per dispatch.

const INSTANCE_COUNT := 56
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)
const UMBRA_P := Vector3(620, 5, -880)
const LIT_P := Vector3(100, 5, 700)

var server: GneRenderServer

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 1600.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(402, "scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(404, "mesh_create")
		return
	# ---- geometry: same construction as GNE-021 (city) ----
	var idx := 0
	server.gpu_scene_set_instance_transform(idx, Vector3(0, -3000, -900), 6000.0); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	for side in [1.0, -1.0]:
		for r in range(10):
			var z := 900.0 - r * 380.0
			var s := 240.0 + float((r * 37) % 5) * 40.0
			server.gpu_scene_set_instance_transform(idx, Vector3(side * 480.0, s * 0.5, z), s); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	for zc in [-900.0, -1700.0]:
		for xx in [900.0, 1350.0, -900.0, -1350.0]:
			var s2 := 300.0 + float(int(absf(zc)) % 80)
			server.gpu_scene_set_instance_transform(idx, Vector3(xx, s2 * 0.5, zc), s2); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	for r2 in range(12):
		var s3 := 420.0
		server.gpu_scene_set_instance_transform(idx, Vector3(-1500.0 + r2 * 280.0, s3 * 0.5, -2700.0 + float((r2 % 3) * 70)), s3); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	while idx < INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(idx, Vector3(0, -9000, 0), 1.0); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	# ---- lights: same 256 as GNE-021 ----
	var want := 0
	for side2 in [1.0, -1.0]:
		for k in range(20):
			var z2 := 820.0 - k * 180.0
			var p := Vector3(side2 * 300.0, 150.0, z2)
			var t := Vector3(-side2 * 300.0, 0.0, z2 - 70.0)
			if server.gpu_light_create(_mk_spot(p, t, 0.38, 0.66, Color(1.0, 0.92, 0.78), 3.0)) != want:
				_fail(410, "spot id " + str(want)); return
			want += 1
	for zrow in [-900.0, -1700.0]:
		for k2 in range(12):
			var x2 := -1650.0 + k2 * 300.0
			if server.gpu_light_create(_mk_spot(Vector3(x2, 150.0, zrow), Vector3(x2, 0.0, zrow + 130.0), 0.38, 0.66, Color(0.95, 0.9, 0.85), 2.6)) != want:
				_fail(411, "spot id " + str(want)); return
			want += 1
	for side3 in [1.0, -1.0]:
		for r3 in range(10):
			var z3 := 900.0 - r3 * 380.0
			var s4 := 240.0 + float((r3 * 37) % 5) * 40.0
			var fx: float = side3 * (480.0 - s4 * 0.5 + 6.0)
			for wy in [90.0, 175.0, 260.0, 340.0]:
				if server.gpu_light_create(_mk_point(Vector3(fx, wy, z3 + float(((int(wy) + r3) % 3) * 90 - 90)), 300.0, Color(1.0, 0.85, 0.6), 2.2)) != want:
					_fail(412, "point id " + str(want)); return
				want += 1
	for zc2 in [-900.0, -1700.0]:
		for xx2 in [900.0, 1350.0, -900.0, -1350.0]:
			for wy2 in [120.0, 250.0, 330.0]:
				if server.gpu_light_create(_mk_point(Vector3(xx2 + (60.0 if wy2 > 200.0 else -60.0), wy2, zc2 + 170.0), 280.0, Color(0.85, 0.9, 1.0), 2.0)) != want:
					_fail(413, "point id " + str(want)); return
				want += 1
	for r4 in range(12):
		if server.gpu_light_create(_mk_point(Vector3(-1500.0 + r4 * 280.0, 200.0, -2600.0), 420.0, Color(0.9, 0.75, 0.95), 1.8)) != want:
			_fail(414, "point id " + str(want)); return
		want += 1
	var scatter := 0
	var zz := 860.0
	while scatter < 76:
		var side4 := 1.0 if (scatter % 2 == 0) else -1.0
		var px: float = side4 * 210.0
		if server.gpu_light_create(_mk_point(Vector3(px, 55.0, zz), 340.0, Color(0.8, 0.88, 1.0), 1.7)) != want:
			_fail(415, "point id " + str(want)); return
		want += 1
		scatter += 1
		zz -= 60.0
		if zz < -2500.0:
			zz = 860.0
	print("S1B: lights=", want)
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 5000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	# ---- probe field: 16 x 8 x 16 = 2048 probes over the city ----
	var cfg := {"min": Vector3(-2000, -50, -3000), "max": Vector3(2000, 900, 1200), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(420, "gi_create")
		return
	print("S1B: info=", server.gpu_gi_info())
	# warm-up dispatch
	if not server.gpu_gi_trace():
		_fail(421, "trace warmup")
		return
	# ---- timed runs (same-session wall-clock; submit+sync included) ----
	var times: Array = []
	for k3 in range(6):
		var t0 := Time.get_ticks_usec()
		if not server.gpu_gi_trace():
			_fail(422, "trace timed")
			return
		times.append(Time.get_ticks_usec() - t0)
	var sorted := times.duplicate()
	sorted.sort()
	var n := sorted.size()
	print("S1B: trace_us per run=", times)
	print("S1B: trace_us sorted=", sorted, " p50=", int(sorted[n / 2]), " min=", int(sorted[0]), " max=", int(sorted[n - 1]), " rays=131072")
	# ---- sanity probes: near-street vs far-field ----
	var near_p := 7 + 1 * 16 + 11 * 128
	var far_p := 0 + 1 * 16 + 11 * 128
	var near: PackedFloat32Array = server.gpu_gi_read_avg(near_p)
	var far: PackedFloat32Array = server.gpu_gi_read_avg(far_p)
	print("S1B: near(", near_p, ")=", near)
	print("S1B: far(", far_p, ")=", far)
	if near.size() < 3 or far.size() < 3:
		_fail(423, "readback short")
		return
	# ---- M3 accum cost (bounce + visibility-gated gather), same methodology ----
	var atimes: Array = []
	for k4 in range(6):
		var t1 := Time.get_ticks_usec()
		if not server.gpu_gi_accum_step():
			_fail(424, "accum timed")
			return
		atimes.append(Time.get_ticks_usec() - t1)
	var asorted := atimes.duplicate()
	asorted.sort()
	print("S1B: accum_us per run=", atimes)
	print("S1B: accum_us sorted=", asorted, " p50=", int(asorted[3]), " min=", int(asorted[0]), " max=", int(asorted[5]))

	# ---- 023 integration layer: draw + HDR/8-bit probes on the city ----
	if not server.gpu_material_draw_lights():
		_fail(430, "draw lights pcf"); return
	var pix_pcf := server.gpu_raster_read_pixels()
	if pix_pcf.size() != RASTER_W * RASTER_H * 4:
		_fail(431, "pix size"); return
	var vp := server.gpu_scene_get_vp()
	var upx := _proj_px(UMBRA_P, vp)
	var lpx := _proj_px(LIT_P, vp)
	var u0 := _window_mean(pix_pcf, upx, 2)
	var l0 := _window_mean(pix_pcf, lpx, 2)
	var h0u := server.gpu_raster_read_hdr(int(upx.x), int(upx.y))
	var h0l := server.gpu_raster_read_hdr(int(lpx.x), int(lpx.y))
	print("S22B: pcf umbra=", u0, " lit=", l0, " hdr_u=", h0u, " hdr_l=", h0l)
	server.gpu_gi_enabled_set(true)
	if not server.gpu_gi_trace():
		_fail(432, "trace"); return
	for k5 in range(24):
		if not server.gpu_gi_accum_step():
			_fail(433, "accum"); return
	if not server.gpu_material_draw_lights():
		_fail(434, "draw lights gi"); return
	var pix_gi := server.gpu_raster_read_pixels()
	var u1 := _window_mean(pix_gi, upx, 2)
	var l1 := _window_mean(pix_gi, lpx, 2)
	var h1u := server.gpu_raster_read_hdr(int(upx.x), int(upx.y))
	var h1l := server.gpu_raster_read_hdr(int(lpx.x), int(lpx.y))
	var du := u1 - u0
	var dl := l1 - l0
	var dh_u := h1u[0] - h0u[0]
	var dh_l := h1l[0] - h0l[0]
	print("S22B: GI umbra 8bit ", u0, "->", u1, " delta=", du, " | hdr ", h0u, "->", h1u, " delta=", dh_u)
	print("S22B: GI lit    8bit ", l0, "->", l1, " delta=", dl, " | hdr ", h0l, "->", h1l, " delta=", dh_l)
	if not _redraw():
		return
	var pix_a := server.gpu_raster_read_pixels()
	if not server.gpu_material_draw_lights():
		_fail(435, "draw det"); return
	var pix_b := server.gpu_raster_read_pixels()
	var det_ok: bool = (pix_a == pix_b)
	print("S22B: determinism=", det_ok)
	var i1_ok := du >= 0.02
	var i2_ok := dh_l >= 0.005
	var pass_all: bool = i1_ok and det_ok and (dh_l >= 0.0)
	print("S22B: I1(umbra indirect)=", i1_ok, " I2(hdr lit gain)=", (dh_l >= 0.005), " det=", det_ok, " => ", ("PASS" if pass_all else "FAIL"))
	var sig := "v022b|du=%.4f|dh_u=%.4f|dh_l=%.4f|det=%d|d1" % [du, dh_u, dh_l, int(det_ok)]
	print("S22B: sig=", sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(436, "sig write"); return
		f.store_string(sig + "\n")
	server.gpu_scene_destroy()
	if pass_all:
		print("S22B: PASS")
		get_tree().quit(0)
	else:
		print("S22B: FAIL")
		get_tree().quit(50)
	var ok := true
	if near[0] <= far[0]:
		ok = false
	if near[0] <= 0.031:
		ok = false
	server.gpu_scene_destroy()
	if ok:
		print("S1B: PASS (near r=", near[0], " > far r=", far[0], " > ambient 0.03)")
		get_tree().quit(0)
	else:
		print("S1B: FAIL sanity near=", near[0], " far=", far[0])
		get_tree().quit(21)


func _hdr_probe(p_px: Vector2) -> PackedFloat32Array:
	return server.gpu_raster_read_hdr(int(p_px.x), int(p_px.y))

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _window_mean(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			s += _brightness(pixels, c + Vector2(dx, dy))
			n += 1
	return s / float(n)

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
		_fail(5040, "recull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(5041, "revis"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(5042, "rebatch"); return false
	if not server.gpu_material_draw_lights():
		_fail(5043, "draw"); return false
	return true

func _fail(code: int, msg: String) -> void:
	print("S1B: FAIL code=", code, " ", msg)
	get_tree().quit(code)
