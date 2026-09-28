extends Node

# GNE-022 section 11 - LIVE LOOP scene (displayed-image convergence).
# Frozen pre-registration: docs/spec_022_gi_scoping.md section 11.
# City + black albedo + witnesses at the section-10 cells (face offset +5 in
# front of the cell center so the probe does not sit on its own witness face
# during accumulation). Each frame: accum_step (update + front flip) then draw
# (reads the new front). M(k) = near-witness window mean luminance, sampled
# every 10 frames; F(k) = read_avg(NEAR_PROBE)[0] as field cross-evidence.

const INSTANCE_COUNT := 56
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 2400)
const ISCALE := 0.015 # instrument gain: s=0.03 saturated the accumulated image (run1 evidence); ratio-invariant by construction (sec.10)
const NEAR_PROBE := 7 + 1 * 16 + 11 * 128
const FAR_PROBE := 0 + 1 * 16 + 11 * 128
const NEAR_C := Vector3(-125.0, 128.125, 18.75)
const FAR_C := Vector3(-1875.0, 128.125, 18.75)
const FRAMES := 160
const SAMPLE_EVERY := 10

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
	print("LOOP: lights=", want)
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
	print("LOOP: info=", server.gpu_gi_info())
	# witness faces +5 in front of the cell centers (off the probe plane)
	server.gpu_scene_set_instance_transform(54, NEAR_C + Vector3(0, 0, 5.0) - Vector3(0, 0, 20.0), 40.0)
	server.gpu_scene_set_instance_transform(55, FAR_C + Vector3(0, 0, 5.0) - Vector3(0, 0, 20.0), 40.0)
	var vp := server.gpu_scene_get_vp()
	var near_px := _first_in_bounds(_proj(NEAR_C + Vector3(0, 0, 5.0), vp))
	if near_px.x < 0:
		_fail(437, "witness projection out of bounds"); return
	if not server.gpu_gi_reset():
		_fail(431, "reset"); return
	server.gpu_gi_enabled_set(true)
	# run 1
	var run1 := _run_sequence(near_px)
	print("LOOP: seq1_m=", run1[0])
	print("LOOP: seq1_f=", run1[1])
	if not server.gpu_gi_reset():
		_fail(432, "reset2"); return
	var run2 := _run_sequence(near_px)
	print("LOOP: seq2_m=", run2[0])
	var seq_equal: bool = (String(run1[0]) == String(run2[0])) and (String(run1[1]) == String(run2[1]))
	print("LOOP: in_process_equal=", seq_equal)
	_evaluate(run1, seq_equal)
	# --- diagnostic: per-frame field lattice around the stall onset (k 60..100) ---
	if not server.gpu_gi_reset():
		_fail(435, "reset3"); return
	var diag: Array = []
	for kd in range(1, 101):
		if not server.gpu_gi_accum_step():
			_fail(436, "diag accum"); return
		if not _redraw():
			return
		if kd >= 60:
			var fd: PackedFloat32Array = server.gpu_gi_read_avg(NEAR_PROBE)
			var pixd := server.gpu_raster_read_pixels()
			var md := _window_mean(pixd, near_px, 4)
			diag.append("k=%d f=%.9f m=%.9f" % [kd, fd[0], md])
	for d in diag:
		print("LOOP: diag ", d)
	server.gpu_scene_destroy()
	get_tree().quit(0)

func _run_sequence(near_px: Vector2) -> Array:
	var mvals: Array = []
	var fvals: Array = []
	var at: Array = []
	var dt: Array = []
	# M(0): zero-field reference
	if not _redraw():
		return []
	var pix0 := server.gpu_raster_read_pixels()
	var m0 := _window_mean(pix0, near_px, 4)
	print("LOOP: M0=", m0)
	var mstr := "%.7f" % m0
	var fstr := ""
	for k in range(1, FRAMES + 1):
		var t0 := Time.get_ticks_usec()
		if not server.gpu_gi_accum_step():
			_fail(433, "accum"); return []
		at.append(Time.get_ticks_usec() - t0)
		var t1 := Time.get_ticks_usec()
		if not _redraw():
			return []
		dt.append(Time.get_ticks_usec() - t1)
		if k % SAMPLE_EVERY == 0:
			var pix := server.gpu_raster_read_pixels()
			var m := _window_mean(pix, near_px, 4)
			var f: PackedFloat32Array = server.gpu_gi_read_avg(NEAR_PROBE)
			mvals.append(m)
			fvals.append(f[0])
			mstr += ";%.7f" % m
			fstr += "%.6f;" % f[0]
	if mvals.size() != FRAMES / SAMPLE_EVERY:
		_fail(434, "sample count"); return []
	var p50a := _p50(at)
	var p50d := _p50(dt)
	print("LOOP: accum_us p50=", p50a, " min=", _minv(at), " max=", _maxv(at))
	print("LOOP: draw_us  p50=", p50d, " min=", _minv(dt), " max=", _maxv(dt))
	return [mstr, fstr, mvals, fvals, p50a, p50d]

func _evaluate(run1: Array, seq_equal: bool) -> void:
	var mvals: Array = run1[2]
	var mstr: String = run1[0]
	var fstr: String = run1[1]
	# (a) finiteness
	var finite := true
	for v in mvals:
		var x: float = v
		if is_nan(x) or is_inf(x):
			finite = false
	print("LOOP: criterion_a_finite=", finite)
	# (b) geometric decay on deltas of the recorded sequence
	var ok_b := true
	var ratios: Array = []
	var prev_delta := -1.0
	for i in range(1, mvals.size()):
		var d: float = mvals[i] - mvals[i - 1]
		if prev_delta >= 0.0:
			var r := d / maxf(prev_delta, 1e-12)
			ratios.append(r)
			if d > 0.6 * prev_delta:
				ok_b = false
		prev_delta = d
	print("LOOP: delta_ratios=", ratios)
	print("LOOP: criterion_b_geometric=", ok_b)
	# (c) bounded gain
	var first_m: float = mvals[0]
	var last_m: float = mvals[mvals.size() - 1]
	var ok_c := last_m <= 3.0 * first_m
	print("LOOP: criterion_c_bound first=", first_m, " last=", last_m, " ratio=", last_m / maxf(first_m, 1e-12), " ok=", ok_c)
	var all_ok := finite and ok_b and ok_c and seq_equal
	print("LOOP: CRITERIA ", "PASS" if all_ok else "FAIL")
	var sig := "v22loop|m=" + mstr + "|f=" + fstr + "|eq=" + ("1" if seq_equal else "0") + "|acc_p50=" + str(run1[4]) + "|draw_p50=" + str(run1[5]) + "|d1"
	print("LOOP: sig=", sig)
	if sig_file != "":
		var fl := FileAccess.open(sig_file, FileAccess.WRITE)
		if fl == null:
			_fail(438, "sig file"); return
		fl.store_string(sig + "\n")

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(440, "recull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(441, "revisibility"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(442, "rebatch"); return false
	if not server.gpu_material_draw_lights():
		_fail(443, "draw"); return false
	return true

func _lum(pixels: PackedByteArray, o: int) -> float:
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _window_mean(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var x := int(c.x) + dx
			var y := int(c.y) + dy
			if x < 0 or y < 0 or x >= RASTER_W or y >= RASTER_H:
				continue
			var o := (y * RASTER_W + x) * 4
			s += _lum(pixels, o)
			n += 1
	return s / float(maxi(n, 1))

func _p50(a: Array) -> int:
	var s := a.duplicate()
	s.sort()
	return int(s[s.size() / 2])

func _minv(a: Array) -> int:
	var m := 1 << 60
	for v in a:
		m = mini(m, int(v))
	return m

func _maxv(a: Array) -> int:
	var m := 0
	for v in a:
		m = maxi(m, int(v))
	return m

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

func _fail(code: int, msg: String) -> void:
	print("LOOP: FAIL code=", code, " ", msg)
	get_tree().quit(code)