extends Node

# GNE-018-rev R0 check scene (tooling, not a gate): numeric spot-check of the
# new normal attachment across the five non-018 raster pipelines.
# Expected: raster / mesh / mesh_batch / group write the all-zero "never cull"
# sentinel; mat_batch writes a real unit normal. Prints R0CHK| lines.

const COUNT := 8
const RASTER_W := 1920
const RASTER_H := 1080

const PTS: Array[Vector3] = [
	Vector3(0, 0, -700),
	Vector3(-350, 150, -850),
	Vector3(350, -150, -900),
	Vector3(0, 320, -800),
	Vector3(-300, -250, -750),
	Vector3(300, 250, -950),
	Vector3(0, -120, -650),
	Vector3(-150, 120, -1000),
]
const SCALES: Array[float] = [50.0, 40.0, 45.0, 35.0, 40.0, 55.0, 35.0, 40.0]

const OCTA_VERTS: Array[Vector3] = [
	Vector3(0, 0, 1),
	Vector3(1, 0, 0),
	Vector3(0, 1, 0),
	Vector3(-1, 0, 0),
	Vector3(0, -1, 0),
	Vector3(0, 0, -1),
]
const OCTA_INDICES: Array[int] = [
	0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4,
	5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 1,
]

var server: GneRenderServer
var camera: Camera3D
var frame := 0
var done := false

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		print("R0CHK: FAIL server null")
		get_tree().quit(90)
		return
	if not server.ensure_gpu_device():
		print("R0CHK: FAIL no device")
		get_tree().quit(91)
		return
	camera = $Camera

func _process(_delta: float) -> void:
	if done:
		return
	frame += 1
	if frame < 2:
		return
	done = true
	_run()
	server.gpu_scene_destroy()
	print("R0CHK: DONE")
	get_tree().quit(0)

func _fail(code: int, msg: String) -> void:
	print("R0CHK: FAIL ", code, " ", msg)
	server.gpu_scene_destroy()
	get_tree().quit(code)

func _set_cam() -> void:
	camera.global_position = Vector3(-850, -550, 2100)
	camera.look_at(Vector3(0, 0, -800), Vector3.UP)
	server.gpu_scene_set_viewport(RASTER_W, RASTER_H)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

func _probe_px() -> Vector2i:
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		return Vector2i(-1, -1)
	var p: Vector3 = PTS[0]
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return Vector2i(-1, -1)
	var ndx := cx / cw
	var ndy := cy / cw
	return Vector2i(int((ndx * 0.5 + 0.5) * RASTER_W), int((ndy * 0.5 + 0.5) * RASTER_H))

func _scan_colors(limit: int) -> Array:
	var px := server.gpu_raster_read_pixels()
	if px.size() != RASTER_W * RASTER_H * 4:
		print("R0CHK: scan pixel buffer size ", px.size())
		return []
	var found := []
	var i := 0
	while i < RASTER_W * RASTER_H and found.size() < limit:
		var o := i * 4
		var r: int = px[o]
		var g: int = px[o + 1]
		var b: int = px[o + 2]
		if r > 60 or g > 60 or b > 60:
			found.append([i % RASTER_W, i / RASTER_W, r, g, b])
		i += 16
	return found
func _report(tag: String, px: Vector2i) -> void:
	if px.x < 0 or px.x >= RASTER_W or px.y < 0 or px.y >= RASTER_H:
		print("R0CHK|", tag, "|px=oob|vz=0|n=[]")
		return
	var vz := server.gpu_raster_read_viewz(px.x, px.y)
	var n := server.gpu_raster_read_normal(px.x, px.y)
	print("R0CHK|", tag, "|px=", px.x, ",", px.y, "|vz=", vz, "|n=", n)

func _report_pair(tag: String, base: Vector2i) -> void:
	_report(tag, base)
	_report(tag + "-flipy", Vector2i(base.x, RASTER_H - 1 - base.y))

func _run() -> void:
	if not server.gpu_scene_create(COUNT, 1.0):
		_fail(92, "scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(93, "scene_dispatch")
		return
	for i in COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
	var posdbg := server.gpu_scene_readback_positions(0, COUNT)
	print("R0CHK|posdbg=", posdbg[0], " | ", posdbg[1])
	if not server.gpu_mesh_create():
		_fail(94, "mesh_create")
		return
	_set_cam()

	# A: raster (billboard) pipeline
	if not server.gpu_cull_dispatch():
		_fail(95, "A cull")
		return
	print("R0CHK|A|vis=", server.gpu_cull_get_visible_count())
	if not server.gpu_visibility_dispatch():
		_fail(96, "A vis")
		return
	if not server.gpu_drawargs_finalize():
		_fail(97, "A finalize")
		return
	if not server.gpu_raster_indirect_draw():
		_fail(98, "A raster")
		return
	_report_pair("A-raster", _probe_px())
	var vpdbg := server.gpu_scene_get_vp()
	print("R0CHK|vp=", vpdbg)
	print("R0CHK|A-scan=", _scan_colors(5))

	# B: mesh pipeline
	if not server.gpu_cull_dispatch():
		_fail(99, "B cull")
		return
	print("R0CHK|B|vis=", server.gpu_cull_get_visible_count())
	if not server.gpu_visibility_dispatch():
		_fail(100, "B vis")
		return
	if not server.gpu_mesh_drawargs_finalize():
		_fail(101, "B finalize")
		return
	if not server.gpu_mesh_indirect_draw():
		_fail(102, "B mesh")
		return
	_report_pair("B-mesh", _probe_px())

	# C: mesh_batch pipeline (strategy 0)
	var mid := server.gpu_mesh_create_from_arrays(OCTA_VERTS, OCTA_INDICES)
	if mid < 0:
		_fail(103, "mesh_from_arrays")
		return
	for i in COUNT:
		server.gpu_scene_set_instance_mesh(i, mid)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(104, "strategy0")
		return
	if not server.gpu_cull_dispatch():
		_fail(105, "C cull")
		return
	print("R0CHK|C|vis=", server.gpu_cull_get_visible_count())
	if not server.gpu_visibility_dispatch():
		_fail(106, "C vis")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(107, "C batch_dispatch")
		return
	if not server.gpu_mesh_batch_draw():
		_fail(108, "C batch_draw")
		return
	_report_pair("C-mesh_batch", _probe_px())

	# G: group batch pipeline (strategy 2)
	if not server.gpu_mesh_set_batch_strategy(2):
		_fail(109, "strategy2")
		return
	if not server.gpu_cull_dispatch():
		_fail(110, "G cull")
		return
	print("R0CHK|G|vis=", server.gpu_cull_get_visible_count())
	if not server.gpu_visibility_dispatch():
		_fail(111, "G vis")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(112, "G batch_dispatch")
		return
	if not server.gpu_mesh_batch_draw():
		_fail(113, "G batch_draw")
		return
	_report_pair("G-group", _probe_px())

	# D: mat_batch pipeline (materials)
	if not server.gpu_material_create():
		_fail(114, "material_create")
		return
	for i in COUNT:
		server.gpu_material_set_albedo(i, Color(1, 0.5, 0.2))
		server.gpu_material_set_params(i, 0.5, 0.0)
		server.gpu_material_set_specular(i, Color(1, 1, 1), 32.0)
	if not server.gpu_material_set_light(Vector3(-0.5, -1.0, -0.5)):
		_fail(115, "material_light")
		return
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(116, "strategy0b")
		return
	if not server.gpu_cull_dispatch():
		_fail(117, "D cull")
		return
	print("R0CHK|D|vis=", server.gpu_cull_get_visible_count())
	if not server.gpu_visibility_dispatch():
		_fail(118, "D vis")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(119, "D batch_dispatch")
		return
	if not server.gpu_material_draw():
		_fail(120, "D material_draw")
		return
	_report_pair("D-mat_batch", _probe_px())

	# E: cone buffer smoke (GNE-018-rev unit 2a) - zero records until the pass lands
	var lid := server.gpu_light_create({"type": 0, "pos": Vector3(0, 0, -600), "range": 500.0, "color": Color(1, 1, 1), "intensity": 1.0})
	print("R0CHK|E|light_id=", lid)
	var c0 := server.gpu_light_cone_read(0)
	var c3455 := server.gpu_light_cone_read(3455)
	var cbad := server.gpu_light_cone_read(3456)
	print("R0CHK|E|cone0=", c0, "|cone3455=", c3455, "|cone3456_size=", cbad.size())

	# F: unit-2b isolation tests + cone build evidence
	_f_run_mode(1)
	_f_run_mode(2)
	_f_run_mode(3)
	_f_run_mode(4)
	# F7: live-path cone prefilter proof (unit 2c) - three-point decomposition.
	var bait := server.gpu_light_create({"type": 0, "pos": Vector3(0, 0, -3000), "range": 150.0, "color": Color(1, 1, 1), "intensity": 0.2})
	print("R0CHK|F7|bait light id=", bait)
	server.gpu_light_set_normal_cone(false)
	if not server.gpu_material_draw_lights():
		print("R0CHK|F7|draw(off) FAILED")
	else:
		var a0 := int(server.gpu_light_get_stats()["assignments"])
		server.gpu_light_set_normal_cone(true)
		server.gpu_light_cones_clear()
		if not server.gpu_material_draw_lights():
			print("R0CHK|F7|draw(slab) FAILED")
		else:
			var a_slab := int(server.gpu_light_get_stats()["assignments"])
			if not server.gpu_material_draw_lights():
				print("R0CHK|F7|draw(cones) FAILED")
			else:
				var a_cone := int(server.gpu_light_get_stats()["assignments"])
				var ep7 := server.gpu_light_cone_epochs()
				print("R0CHK|F7|off=", a0, " slab_only=", a_slab, " with_cones=", a_cone, " cone_saved=", a_slab - a_cone, " slab_gain=", a_slab - a0, " ep=", ep7)
		server.gpu_light_set_normal_cone(false)

	if not server.gpu_light_cones_build():
		print("R0CHK|F4|cones_build FAILED")
	else:
		var nonzero := 0
		var bad := 0
		var first := PackedFloat32Array()
		var c := 0
		while c < 3456:
			var rec := server.gpu_light_cone_read(c)
			if rec.size() == 4:
				var w: float = rec[3]
				if w != 0.0:
					nonzero += 1
					var axl := sqrt(rec[0] * rec[0] + rec[1] * rec[1] + rec[2] * rec[2])
					if w < 0.1 or w > 1.0 or absf(axl - 1.0) > 0.01:
						bad += 1
					if first.is_empty():
						first = rec
					if nonzero <= 4:
						print("R0CHK|F4|cone[", c, "]=", rec)
			c += 1
		print("R0CHK|F4|cones full_scan nonzero=", nonzero, " bad=", bad, " first=", first)
		var vzp := server.gpu_raster_read_viewz(968, 545)
		var lin := -vzp
		var ndx := 968.5 / 1920.0 * 2.0 - 1.0
		var ndy := 1.0 - 545.5 / 1080.0 * 2.0
		var tanv := tan(60.0 * PI / 360.0)
		var aspect := 1920.0 / 1080.0
		var dxv := ndx * aspect * tanv
		var dyv := ndy * tanv
		var edist := lin * sqrt(dxv * dxv + dyv * dyv + 1.0)
		var pcid := _f_mode1_expected(968.5, 545.5, 1920.0, 1080.0, 1.0, 6000.0, edist)
		var prc := server.gpu_light_cone_read(pcid)
		print("R0CHK|F5|probe vz=", vzp, " edist=", edist, " pcid=", pcid, " cone=", prc)
		var ep := server.gpu_light_cone_epochs()
		print("R0CHK|F4|epochs raster=", ep[0], " cone_src=", ep[1])
		server.gpu_material_draw()
		server.gpu_light_cones_build()
		var ep2 := server.gpu_light_cone_epochs()
		print("R0CHK|F4|epochs2 raster=", ep2[0], " cone_src=", ep2[1], " age_at_build=", ep2[0] - ep2[1])
func _f_mv(view16: PackedFloat32Array, v: Vector3, is_point: bool) -> Vector3:
	var w := 1.0 if is_point else 0.0
	var x: float = view16[0] * v.x + view16[4] * v.y + view16[8] * v.z + view16[12] * w
	var y: float = view16[1] * v.x + view16[5] * v.y + view16[9] * v.z + view16[13] * w
	var z: float = view16[2] * v.x + view16[6] * v.y + view16[10] * v.z + view16[14] * w
	return Vector3(x, y, z)

func _f_flip(v: Vector3) -> Vector3:
	return Vector3(v.x, v.y, -v.z)

func _f_mode1_expected(fx: float, fy: float, W: float, H: float, nr: float, far: float, z: float) -> int:
	var tx := clampf(floor(fx / W * 16.0), 0.0, 15.0)
	var ty := clampf(8.0 - floor(fy / H * 9.0), 0.0, 8.0)
	var lratio := far / nr
	var tz := clampf(floor(24.0 * log(z / nr) / log(lratio)), 0.0, 23.0)
	return int(tz) * 144 + int(ty) * 16 + int(tx)

func _f_run_mode(md: int) -> void:
	var res := server.gpu_light_cones_selftest(md)
	if res.size() < 3:
		print("R0CHK|F|mode=", md, " EMPTY (size=", res.size(), ")")
		return
	var cnt := int(res[0])
	var ins := int(res[1])
	var outs := int(res[2])
	var n_in := cnt * ins
	if md == 1:
		var mismatch := 0
		for i in cnt:
			var b := 3 + i * ins
			var ob := 3 + n_in + i * outs
			var helper := int(res[ob])
			var literal := int(res[ob + 1])
			var expected := _f_mode1_expected(res[b], res[b + 1], res[b + 2], res[b + 3], res[b + 4], res[b + 5], res[b + 6])
			if helper != literal or helper != expected:
				mismatch += 1
				print("R0CHK|F1|MISMATCH case=", i, " fx=", res[b], " fy=", res[b + 1], " z=", res[b + 6], " helper=", helper, " literal=", literal, " expected=", expected)
		print("R0CHK|F1|slice_crosscheck cases=", cnt, " mismatches=", mismatch)
	elif md == 2:
		var maxerr := 0.0
		for i in cnt:
			var b := 3 + i * ins
			var v16 := PackedFloat32Array()
			for j in 16:
				v16.append(res[b + j])
			var n := Vector3(res[b + 16], res[b + 17], res[b + 18])
			var l := Vector3(res[b + 20], res[b + 21], res[b + 22])
			var a := Vector3(res[b + 24], res[b + 25], res[b + 26])
			var axis := _f_flip(_f_mv(v16, n, false))
			var to_l := _f_flip(_f_mv(v16, l, true)) - _f_flip(_f_mv(v16, a, true))
			var dl := to_l.length()
			var expected := 1.0
			if dl >= 1e-5:
				expected = (to_l / dl).dot(axis)
			var ob := 3 + n_in + i * outs
			var got := res[ob]
			var err2 := absf(got - expected)
			if err2 > maxerr:
				maxerr = err2
			if err2 > 0.0005:
				print("R0CHK|F2|ERR case=", i, " got=", got, " expected=", expected)
		print("R0CHK|F2|backface cases=", cnt, " max_err=", maxerr)
	elif md == 3:
		var not_in := 0
		for i in cnt:
			var ob := 3 + n_in + i * outs
			if res[ob + 3] < 0.5:
				not_in += 1
			if i < 6:
				print("R0CHK|F3|case=", i, " cid=", int(res[ob]), " z0=", res[ob + 1], " z1=", res[ob + 2], " in_box=", int(res[ob + 3]), " gap=", res[ob + 4])
		print("R0CHK|F3|cull_vs_frag cases=", cnt, " not_in_box=", not_in)
	else:
		var worst := 0.0
		var min_slack_min := 1e9
		var all_ok := true
		for i in cnt:
			var b := 3 + i * ins
			var ob := 3 + n_in + i * outs
			var tanv := res[b]
			var aspect := res[b + 1]
			var nr := res[b + 2]
			var fr := res[b + 3]
			var tzf := res[b + 4]
			var z0 := nr * pow(fr / nr, tzf / 24.0)
			var expected_bound := z0 / sqrt(1.0 + tanv * tanv * (1.0 + aspect * aspect))
			var bound := res[ob]
			var min_slack := res[ob + 1]
			var viol_old := res[ob + 2]
			var rel := absf(bound - expected_bound) / z0
			var ok := rel < 0.0001 and min_slack >= -0.001 * z0 and viol_old >= 1.0
			if not ok:
				all_ok = false
			worst = maxf(worst, rel)
			min_slack_min = minf(min_slack_min, min_slack / z0)
			print("R0CHK|F6|case=", i, " tanv=", tanv, " aspect=", aspect, " rel_err=", rel, " slack_norm=", min_slack / z0, " viol_old=", viol_old, " ok=", ok)
		print("R0CHK|F6|slab_bound cases=", cnt, " worst_rel_err=", worst, " min_slack_norm=", min_slack_min, " all_ok=", all_ok)
