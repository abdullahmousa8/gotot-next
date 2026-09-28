extends Node

# GNE-022 section 10 step-2 - RATIO GATE scene (gate 2 evidence).
# City construction identical to S1b (main_022.gd); rendered THROUGH the
# material pipeline with the GI fragment term. Black albedo: the ON-OFF
# per-pixel delta equals the sampled field contribution directly.
# Uniform light-intensity scale s=0.03 (ratio-invariant by construction, the
# spec_022 section 10 statement: "the ratio gate is scale-invariant by
# construction") keeps contributions inside [0,1] for 8-bit readback so the
# measurement is not compressed by framebuffer saturation.
# Witnesses: spare instances 54/55 repositioned AFTER the trace to the S1b
# near/far probe cells (7,1,11)/(0,1,11); their +Z face windows measure the
# near/far of the normalized contribution as rendered.

const INSTANCE_COUNT := 56
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 2400)
const ISCALE := 0.03
const NEAR_PROBE := 7 + 1 * 16 + 11 * 128
const FAR_PROBE := 0 + 1 * 16 + 11 * 128
const NEAR_C := Vector3(-125.0, 128.125, 18.75)
const FAR_C := Vector3(-1875.0, 128.125, 18.75)

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity * ISCALE}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 1600.0, "color": color, "intensity": intensity * ISCALE, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	_setup()

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server"); return
	if not server.ensure_gpu_device():
		_fail(401, "no device"); return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(402, "scene_create"); return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "dispatch"); return
	if not server.gpu_mesh_create():
		_fail(404, "mesh_create"); return
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
	print("SHADE: lights=", want)
	if not server.gpu_material_create():
		_fail(405, "material_create"); return
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(406, "set_batch_strategy(PER_MESH)"); return
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, Color(0, 0, 0)):
			_fail(407, "set_albedo " + str(i)); return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(407, "set_params " + str(i)); return
		if not server.gpu_material_set_specular(i, Color(0, 0, 0), 32.0):
			_fail(407, "set_specular " + str(i)); return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 6200.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	var cfg := {"min": Vector3(-2000, -50, -3000), "max": Vector3(2000, 900, 1200), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(430, "gi_create"); return
	print("SHADE: info=", server.gpu_gi_info())
	if not server.gpu_gi_trace():
		_fail(431, "trace"); return
	var near_f: PackedFloat32Array = server.gpu_gi_read_avg(NEAR_PROBE)
	var far_f: PackedFloat32Array = server.gpu_gi_read_avg(FAR_PROBE)
	print("SHADE: field near(", NEAR_PROBE, ")=", near_f)
	print("SHADE: field far(", FAR_PROBE, ")=", far_f)
	if near_f.size() < 3 or far_f.size() < 3:
		_fail(432, "readback short"); return
	server.gpu_scene_set_instance_transform(54, NEAR_C - Vector3(0, 0, 20.0), 40.0)
	server.gpu_scene_set_instance_transform(55, FAR_C - Vector3(0, 0, 20.0), 40.0)
	# --- draw sanity pre-pass: white albedo, count rendered pixels ---
	for i in INSTANCE_COUNT:
		server.gpu_material_set_albedo(i, Color(1, 1, 1))
	if not _redraw():
		return
	var pix_w := server.gpu_raster_read_pixels()
	var nz := 0
	for i in RASTER_W * RASTER_H:
		var o := i * 4
		if pix_w[o] > 4 or pix_w[o + 1] > 4 or pix_w[o + 2] > 4:
			nz += 1
	print("SHADE: white-pass nonzero=", nz)
	print("SHADE: batches=", server.gpu_mesh_get_batch_count(), " drawcounts=", server.gpu_mesh_get_draw_counts())
	for i in INSTANCE_COUNT:
		server.gpu_material_set_albedo(i, Color(0, 0, 0))
	if not _redraw():
		return
	# OFF draws (timed), readback of the last off state
	var off_t: Array = []
	for k5 in range(6):
		var t0 := Time.get_ticks_usec()
		if not _redraw():
			return
		off_t.append(Time.get_ticks_usec() - t0)
	var pix_off := server.gpu_raster_read_pixels()
	if pix_off.size() != RASTER_W * RASTER_H * 4:
		_fail(435, "pix_off size"); return
	# ON draws (timed)
	server.gpu_gi_enabled_set(true)
	var on_t: Array = []
	for k6 in range(6):
		var t1 := Time.get_ticks_usec()
		if not _redraw():
			return
		on_t.append(Time.get_ticks_usec() - t1)
	var pix_on := server.gpu_raster_read_pixels()
	if pix_on.size() != RASTER_W * RASTER_H * 4:
		_fail(436, "pix_on size"); return
	_show(pix_on)
	off_t.sort(); on_t.sort()
	print("SHADE: redraw_us off=", off_t, " p50=", int(off_t[3]))
	print("SHADE: redraw_us  on=", on_t, " p50=", int(on_t[3]))
	_analyze(pix_off, pix_on)
	server.gpu_scene_destroy()
	get_tree().quit(0)

func _analyze(pix_off: PackedByteArray, pix_on: PackedByteArray) -> void:
	var dmax := -2.0
	var dmax_p := Vector2.ZERO
	var pos := 0
	var clip := 0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var d := _lum(pix_on, o) - _lum(pix_off, o)
		if d > dmax:
			dmax = d
			dmax_p = Vector2(float(i % RASTER_W), float(i / RASTER_W))
		if d > 2.0 / 255.0:
			pos += 1
			if pix_on[o] == 255 or pix_on[o + 1] == 255 or pix_on[o + 2] == 255:
				clip += 1
	print("SHADE: frame dmax=", dmax, " at ", dmax_p, " pos=", pos, " clip=", clip)
	var vp := server.gpu_scene_get_vp()
	var np_px := _first_in_bounds(_proj(NEAR_C, vp))
	var fp_px := _first_in_bounds(_proj(FAR_C, vp))
	if np_px.x < 0 or fp_px.x < 0:
		_fail(437, "witness projection out of bounds"); return
	var nr := _window_delta(pix_on, pix_off, np_px, 2)
	var fr := _window_delta(pix_on, pix_off, fp_px, 2)
	var rt := nr / maxf(fr, 1e-6)
	print("SHADE: witness near_px=", np_px, " near=", nr, " | far_px=", fp_px, " far=", fr, " | ratio=", rt)
	var sig := "v22shade|nr=%.4f|fr=%.5f|rt=%.2f|s=0.03|clip=%d|d1" % [nr, fr, rt, clip]
	print("SHADE: sig=" + sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(438, "sig file"); return
		f.store_string(sig + "\n")
	if pos < 1000 or nr <= 0.0 or fr <= 0.0:
		_fail(439, "analysis invalid pos=%d nr=%f fr=%f" % [pos, nr, fr]); return
	if rt >= 16.0:
		print("SHADE: GATE2 PASS ratio=", rt)
	else:
		print("SHADE: GATE2 FAIL ratio=", rt, " (threshold 16.0)")

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(440, "recull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(441, "revisibility"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(442, "rebatch"); return false
	if not server.gpu_material_draw_lights():
		_fail(443, "redraw_lights"); return false
	return true

func _lum(pixels: PackedByteArray, o: int) -> float:
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _window_delta(pix_on: PackedByteArray, pix_off: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var x := int(c.x) + dx
			var y := int(c.y) + dy
			if x < 0 or y < 0 or x >= RASTER_W or y >= RASTER_H:
				continue
			var o := (y * RASTER_W + x) * 4
			s += _lum(pix_on, o) - _lum(pix_off, o)
			n += 1
	return s / float(maxi(n, 1))

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

func _first_in_bounds(cands: Array) -> Vector2:
	for cand in cands:
		var p: Vector2 = cand
		if p.x >= 4 and p.y >= 4 and p.x < RASTER_W - 4 and p.y < RASTER_H - 4:
			return p
	return Vector2(-1, -1)

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

func _fail(code: int, msg: String) -> void:
	print("SHADE: FAIL code=", code, " ", msg)
	get_tree().quit(code)