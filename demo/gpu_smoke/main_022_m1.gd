extends Node

# GNE-022 section 11-M1 - FLOAT FIELD CONVERGENCE INSTRUMENT (KI-016).
# Spec: docs/spec_022_gi_scoping.md section 11-M1 (approved 2026-09-30).
#
# MEASUREMENT ONLY. This scene does NOT change the section-11 criterion, does not
# turn its FAIL into a PASS, and does not touch main_022_loop.gd (frozen).
#
# What it measures: the GI FIELD ITSELF at float precision, away from the 8-bit
# image conversion. Source: gpu_gi_read_avg() (float32 since 11-R1) on the same
# near probe the section-11 scene uses, PLUS a fixed probe set for robustness.
# Per frame: accum_step + redraw (the gather's visibility gate consumes the
# raster, so the loop context stays identical to section 11) and one field
# readback. There is NO 8-bit pixel readback anywhere in this scene.
#
# Metrics: per-frame field series f(k), delta series, slope ratios every 10
# frames (the section-11 cadence), endpoint characteristics (frames to
# |delta| < 1e-5 and < 1e-6), in-process double-run determinism (byte-equal).
# Whether the field's own ratios satisfy the section-11 0.6 geometric rule is
# REPORTED, never gated: this scene's PASS means INSTRUMENT INTEGRITY only
# (finite series, complete readbacks, byte-equal determinism, no errors).

const INSTANCE_COUNT := 56
const CAM_POS := Vector3(0, 0, 2400)
const ISCALE := 0.015 # same instrument gain as section 11 (ratio-invariant by construction)
const NEAR_PROBE := 7 + 1 * 16 + 11 * 128
const FAR_PROBE := 0 + 1 * 16 + 11 * 128
const SPREAD_A := 0 # (px,py,pz) = (0,0,0)   - grid corner
const SPREAD_B := 15 + 7 * 16 + 15 * 128 # (15,7,15) - opposite corner
const SPREAD_C := 8 + 4 * 16 + 8 * 128 # (8,4,8)   - centre
const NEAR_C := Vector3(-125.0, 128.125, 18.75)
const FAR_C := Vector3(-1875.0, 128.125, 18.75)
const FRAMES := 160
const SAMPLE_EVERY := 10
const EPS_FINE := 1e-5
const EPS_FINER := 1e-6

var server: GneRenderServer
var camera: Camera3D
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
	print("M1: lights=", want)
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
	print("M1: info=", server.gpu_gi_info())
	# Witness faces +5 in front of the cell centres, exactly as the section-11
	# scene places them: the field must be measured in the same context.
	server.gpu_scene_set_instance_transform(54, NEAR_C + Vector3(0, 0, 5.0) - Vector3(0, 0, 20.0), 40.0)
	server.gpu_scene_set_instance_transform(55, FAR_C + Vector3(0, 0, 5.0) - Vector3(0, 0, 20.0), 40.0)
	if not server.gpu_gi_reset():
		_fail(431, "reset"); return
	server.gpu_gi_enabled_set(true)
	_pre_sequence()
	# ---- pass 1 ----
	var r1 := _run_sequence()
	print("M1: pass1_fseries=", r1[0])
	if not server.gpu_gi_reset():
		_fail(432, "reset2"); return
	_pre_sequence()
	# ---- pass 2 (determinism) ----
	var r2 := _run_sequence()
	print("M1: pass2_fseries=", r2[0])
	var det: bool = (String(r1[0]) == String(r2[0])) and (String(r1[1]) == String(r2[1])) and (String(r1[2]) == String(r2[2]))
	print("M1: determinism_byte_equal=", det)
	_report(r1, r2, det)
	server.gpu_scene_destroy()
	var ok: bool = _final_ok(r1, det)
	print("M1: instrument_integrity=", "PASS" if ok else "FAIL")
	get_tree().quit(0 if ok else 60)

# Instrument integrity, isolated into a hook so a successor unit (11-M2) can add
# its own criterion without touching the instrument's logic. This is the same
# three-condition check the scene always evaluated inline.
func _final_ok(p_r1: Array, p_det: bool) -> bool:
	return p_det and bool(p_r1[5]) and int(p_r1[4]) == FRAMES

# Hooks for successor units. Both are no-ops here, so the instrument's own
# behaviour is unchanged; a subclass can prepare state before the sequence and
# take extra reads on the section-11 cadence without duplicating the loop.
func _pre_sequence() -> void:
	pass

func _on_sample(_p_k: int) -> void:
	pass

# Returns [fstr, dstr, rstr, fvals, count, finite, p50_read_us, p50_accum_us, probe_last]
func _run_sequence() -> Array:
	var fvals: Array = [] # one entry per frame: near-probe field value (red channel)
	var fstr := ""
	var rstr := ""
	var reads: Array = []
	var accums: Array = []
	var probe_last := {}
	var finite := true
	for k in range(1, FRAMES + 1):
		var t0 := Time.get_ticks_usec()
		if not server.gpu_gi_accum_step():
			_fail(433, "accum"); return []
		accums.append(Time.get_ticks_usec() - t0)
		if not _redraw():
			return []
		# ONE field readback per frame on the section-11 near probe. Each
		# gpu_gi_read_avg() call fetches the whole atlas, so the probe set below
		# is sampled on the section-11 cadence instead of every frame.
		var t1 := Time.get_ticks_usec()
		var f: PackedFloat32Array = server.gpu_gi_read_avg(NEAR_PROBE)
		reads.append(Time.get_ticks_usec() - t1)
		if f.size() < 1:
			_fail(434, "read_avg short"); return []
		var v := float(f[0])
		if is_nan(v) or is_inf(v):
			finite = false
		fvals.append(v)
		fstr += "%.9f;" % v
		if k % SAMPLE_EVERY == 0:
			probe_last = {
				"far": server.gpu_gi_read_avg(FAR_PROBE),
				"a": server.gpu_gi_read_avg(SPREAD_A),
				"b": server.gpu_gi_read_avg(SPREAD_B),
				"c": server.gpu_gi_read_avg(SPREAD_C),
			}
			_on_sample(k)
	if fvals.size() != FRAMES:
		_fail(435, "series size"); return []
	# slope ratios on the section-11 cadence: deltas between 10-frame samples
	var s := ""
	for j in range(SAMPLE_EVERY, FRAMES + 1, SAMPLE_EVERY):
		s += "%.9f;" % float(fvals[j - 1])
	var d10: Array = []
	for j in range(1, fvals.size() / SAMPLE_EVERY):
		d10.append(float(fvals[(j + 1) * SAMPLE_EVERY - 1]) - float(fvals[j * SAMPLE_EVERY - 1]))
	var ratios: Array = []
	for j in range(1, d10.size()):
		ratios.append(float(d10[j]) / maxf(float(d10[j - 1]), 1e-12))
	for r in ratios:
		rstr += "%.6f;" % float(r)
	return [fstr, rstr, s, fvals, int(fvals.size()), finite, _p50(reads), _p50(accums), probe_last]

func _report(r1: Array, _r2: Array, det: bool) -> void:
	var fvals: Array = r1[3]
	var n := fvals.size()
	var deltas: Array = []
	for i in range(1, n):
		deltas.append(float(fvals[i]) - float(fvals[i - 1]))
	var d_first := float(deltas[0])
	var d_last := float(deltas[deltas.size() - 1])
	var d_min := d_first
	var d_max := d_first
	for d in deltas:
		if float(d) < d_min:
			d_min = float(d)
		if float(d) > d_max:
			d_max = float(d)
	# 10-frame sample table: k, f, delta over the interval, ratio vs previous interval
	var d10: Array = []
	for j in range(1, n / SAMPLE_EVERY):
		d10.append(float(fvals[(j + 1) * SAMPLE_EVERY - 1]) - float(fvals[j * SAMPLE_EVERY - 1]))
	var ratios: Array = []
	for j in range(1, d10.size()):
		ratios.append(float(d10[j]) / maxf(float(d10[j - 1]), 1e-12))
	print("M1: --- 10-frame sample table (section-11 cadence) ---")
	var prev_delta := 0.0
	var below06 := 0
	for si in range(0, n / SAMPLE_EVERY):
		var kk := (si + 1) * SAMPLE_EVERY
		var dd := 0.0
		var rr := 0.0
		if si > 0:
			dd = d10[si - 1]
			if si > 1:
				rr = ratios[si - 2]
				if rr <= 0.6:
					below06 += 1
		print("M1: k=%d f=%.9f delta10=%.9f ratio10=%.6f" % [kk, float(fvals[kk - 1]), dd, rr])
	print("M1: --- summary ---")
	print("M1: f_first=%.9f f_last=%.9f" % [float(fvals[0]), float(fvals[n - 1])])
	print("M1: delta_first=%.9f delta_last=%.9f delta_min=%.9f delta_max=%.9f" % [d_first, d_last, d_min, d_max])
	print("M1: delta10_first=%.9f delta10_last=%.9f" % [float(d10[0]), float(d10[d10.size() - 1])])
	# ratio series statistics
	var r_med := _median(ratios)
	var r_gm := 1.0
	for r in ratios:
		r_gm *= maxf(float(r), 1e-12)
	r_gm = pow(r_gm, 1.0 / float(ratios.size()))
	var per10 := pow(maxf(d_last, 1e-12) / maxf(d_first, 1e-12), 1.0 / float(n - 1))
	per10 = pow(per10, float(SAMPLE_EVERY))
	print("M1: ratio10_series=", r1[1])
	print("M1: ratio10_median=%.6f ratio10_geomean=%.6f ratio10_below_0.6=%d/%d" % [r_med, r_gm, below06, ratios.size()])
	print("M1: decay_per_10_frames_geomean=%.6f (11-R1 recorded 0.509)" % r_gm)
	# endpoints on the PER-FRAME delta series
	var e5 := -1
	var e6 := -1
	for i in range(deltas.size()):
		var ad: float = absf(float(deltas[i]))
		if e5 < 0 and ad < EPS_FINE:
			e5 = i + 2
		if e6 < 0 and ad < EPS_FINER:
			e6 = i + 2
	print("M1: endpoint_delta_lt_1e-5_frame=", ("%d" % e5) if e5 > 0 else "NOT-REACHED-WITHIN-%d" % FRAMES)
	print("M1: endpoint_delta_lt_1e-6_frame=", ("%d" % e6) if e6 > 0 else "NOT-REACHED-WITHIN-%d" % FRAMES)
	# probe-set robustness (values at the final sample)
	var pl: Dictionary = r1[8]
	if pl.has("far"):
		print("M1: probe_set_last near=%.9f far=%.9f a=%.9f b=%.9f c=%.9f" % [
			float(fvals[n - 1]), float(pl["far"][0]), float(pl["a"][0]), float(pl["b"][0]), float(pl["c"][0])])
		var allf := true
		for key in ["far", "a", "b", "c"]:
			var arr: PackedFloat32Array = pl[key]
			for x in arr:
				if is_nan(float(x)) or is_inf(float(x)):
					allf = false
		print("M1: probe_set_all_finite=", allf)
	print("M1: cost read_p50_us=", r1[6], " accum_p50_us=", r1[7], " (per frame, whole-atlas readback)")
	print("M1: section11_untouched: main_022_loop.gd not modified; its criterion/state unchanged (official: FAIL, criterion b)")
	print("M1: determinism_byte_equal=", det, " (both passes compared on series + ratios + 10-frame samples)")
	var sig := "v022m1|frames=%d|dfirst=%.9f|dlast=%.9f|r10gm=%.6f|r10med=%.6f|e5=%d|e6=%d|det=%d|d1" % [
		n, d_first, d_last, r_gm, r_med, e5, e6, 1 if det else 0]
	print("M1: sig=", sig)
	if sig_file != "":
		var fl := FileAccess.open(sig_file, FileAccess.WRITE)
		if fl != null:
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

func _median(a: Array) -> float:
	if a.is_empty():
		return 0.0
	var b := a.duplicate()
	b.sort()
	var m := b.size() / 2
	if b.size() % 2 == 1:
		return float(b[m])
	return (float(b[m - 1]) + float(b[m])) / 2.0

func _p50(a: Array) -> int:
	if a.is_empty():
		return 0
	var b := a.duplicate()
	b.sort()
	return int(b[b.size() / 2])

func _fail(code: int, msg: String) -> void:
	print("M1: FAIL code=", code, " ", msg)
	get_tree().quit(code)
