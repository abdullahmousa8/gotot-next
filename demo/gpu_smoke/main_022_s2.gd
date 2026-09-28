extends Node
# GNE-022 S2 gates 9.1/9.2: convergence/stability (60 accumulation frames,
# fixed probe sampled every 10) + cross-frame determinism (two fresh-process
# runs must print byte-equal sequences). Order per spec_022 section 9.4.

const INSTANCE_COUNT := 56
var server: GneRenderServer

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 1600.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _ready() -> void:
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
	# same city construction as GNE-021 / S1b
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
			if server.gpu_light_create(_mk_spot(Vector3(side2 * 300.0, 150.0, z2), Vector3(-side2 * 300.0, 0.0, z2 - 70.0), 0.38, 0.66, Color(1.0, 0.92, 0.78), 3.0)) != want:
				_fail(410, "spot"); return
			want += 1
	for zrow in [-900.0, -1700.0]:
		for k2 in range(12):
			var x2 := -1650.0 + k2 * 300.0
			if server.gpu_light_create(_mk_spot(Vector3(x2, 150.0, zrow), Vector3(x2, 0.0, zrow + 130.0), 0.38, 0.66, Color(0.95, 0.9, 0.85), 2.6)) != want:
				_fail(411, "spot2"); return
			want += 1
	for side3 in [1.0, -1.0]:
		for r3 in range(10):
			var z3 := 900.0 - r3 * 380.0
			var s4 := 240.0 + float((r3 * 37) % 5) * 40.0
			var fx: float = side3 * (480.0 - s4 * 0.5 + 6.0)
			for wy in [90.0, 175.0, 260.0, 340.0]:
				if server.gpu_light_create(_mk_point(Vector3(fx, wy, z3 + float(((int(wy) + r3) % 3) * 90 - 90)), 300.0, Color(1.0, 0.85, 0.6), 2.2)) != want:
					_fail(412, "point"); return
				want += 1
	for zc2 in [-900.0, -1700.0]:
		for xx2 in [900.0, 1350.0, -900.0, -1350.0]:
			for wy2 in [120.0, 250.0, 330.0]:
				if server.gpu_light_create(_mk_point(Vector3(xx2 + (60.0 if wy2 > 200.0 else -60.0), wy2, zc2 + 170.0), 280.0, Color(0.85, 0.9, 1.0), 2.0)) != want:
					_fail(413, "point2"); return
				want += 1
	for r4 in range(12):
		if server.gpu_light_create(_mk_point(Vector3(-1500.0 + r4 * 280.0, 200.0, -2600.0), 420.0, Color(0.9, 0.75, 0.95), 1.8)) != want:
			_fail(414, "point3"); return
		want += 1
	var scatter := 0
	var zz := 860.0
	while scatter < 76:
		var side4 := 1.0 if (scatter % 2 == 0) else -1.0
		var px: float = side4 * 210.0
		if server.gpu_light_create(_mk_point(Vector3(px, 55.0, zz), 340.0, Color(0.8, 0.88, 1.0), 1.7)) != want:
			_fail(415, "point4"); return
		want += 1
		scatter += 1
		zz -= 60.0
		if zz < -2500.0:
			zz = 860.0
	var cfg := {"min": Vector3(-2000, -50, -3000), "max": Vector3(2000, 900, 1200), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(420, "gi_create")
		return
	# defaults: alpha 0.1, albedo 0.35, bounce on, ambient 0.03
	if not server.gpu_gi_reset():
		_fail(421, "gi_reset")
		return
	var probe := 7 + 1 * 16 + 11 * 128
	var seq: Array = []
	for f in range(1, 61):
		if not server.gpu_gi_accum_step():
			_fail(422, "accum step")
			return
		if f % 10 == 0:
			var v: PackedFloat32Array = server.gpu_gi_read_avg(probe)
			seq.append(float(v[0]))
	print("S2C: seq=", seq)
	var ok := true
	for x in seq:
		if not is_finite(float(x)):
			ok = false
	var v10 := float(seq[0]); var v50 := float(seq[4]); var v60 := float(seq[5])
	# Criterion per spec_022 9.1 (adopted amendment): geometric decay - every delta
	# <= 0.6x the previous; gain cap <= 3x; all finite. No fixed N.
	var deltas: Array = []
	for di in range(1, seq.size()):
		deltas.append(absf(float(seq[di]) - float(seq[di - 1])))
	var ratio_ok := true
	for ri in range(1, deltas.size()):
		if float(deltas[ri]) > 0.6 * float(deltas[ri - 1]):
			ratio_ok = false
	print("S2C: deltas=", deltas)
	if not ratio_ok:
		ok = false
	if v60 > 3.0 * v10:
		ok = false
	if v60 <= 0.0:
		ok = false
	var drift := absf(v60 - v50)
	print("S2C: v10=", v10, " v50=", v50, " v60=", v60, " drift_abs=", drift, " drift_rel=", (drift / v60 if v60 > 0.0 else -1.0), " gain=", (v60 / v10 if v10 > 0.0 else -1.0), " ratio_ok=", ratio_ok)
	var parts := PackedStringArray()
	for x3 in seq:
		parts.append("%.6f" % float(x3))
	print("S2C: seqstr=", "|".join(parts))
	server.gpu_scene_destroy()
	if ok:
		print("S2C: PASS (convergence + stability)")
		get_tree().quit(0)
	else:
		print("S2C: FAIL convergence")
		get_tree().quit(21)

func _fail(code: int, msg: String) -> void:
	print("S2C: FAIL code=", code, " ", msg)
	get_tree().quit(code)
