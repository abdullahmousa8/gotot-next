extends Node

# GNE-018-rev R1 scene - scoped flag-off/flag-on verification (contract R1/R7).
#
# Scene parity with main_018 (same 4 boxes + 20 lights + camera) so the OFF
# numbers stay comparable to the frozen v18 evidence. Flow:
#   1. OFF frame  -> pixels_off, stats_off (parity: 11348 assigns expected),
#      per-cluster list snapshot OFF.
#   2. cones from the OFF raster (static scene), then ON frame -> pixels_on,
#      stats_on, lists ON (cull consumed the cones; same-frame ordering).
#   3. R1 (scoped): every differing pixel must lie in a cluster whose light
#      list changed AND must carry the KI-011 signature (all dropped lights
#      NdotL <= 0 at that pixel). Unexplained differences fail as over-cull.
#   4. R7: cone source age <= 1 frame (engine-exposed counter).
#   Sig: v18-rev|lc=20|cc=<touched_on>|dc=<slab->cone saved>|ot=<ot_on>|dp=<diff px>|d1

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
var vp_aspect := 16.0 / 9.0
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
		_fail(3000, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(3001, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(3002, "gpu_scene_create(4)")
		return
	if not server.gpu_scene_dispatch(4):
		_fail(3003, "gpu_scene_dispatch(4)")
		return
	if not server.gpu_mesh_create():
		_fail(3004, "gpu_mesh_create")
		return
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(3005, "set_batch_strategy(PER_MESH)")
		return
	if not server.gpu_material_create():
		_fail(3006, "gpu_material_create")
		return
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, Color(1, 1, 1)):
			_fail(3007, "set_albedo " + str(i))
			return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(3007, "set_params " + str(i))
			return
		if not server.gpu_material_set_specular(i, Color(0.25, 0.25, 0.25), 32.0):
			_fail(3007, "set_specular " + str(i))
			return
	for i in POINTS.size():
		var q: Array = POINTS[i]
		if server.gpu_light_create(_mk_point(q[0], q[1], q[2], q[3])) != i:
			_fail(3008, "point id != " + str(i))
			return
	for i in SPOTS.size():
		var s: Array = SPOTS[i]
		if server.gpu_light_create(_mk_spot(s[0], s[1], s[2], s[3], s[4], s[5])) != 16 + i:
			_fail(3008, "spot id != " + str(16 + i))
			return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	vp_aspect = vp_size.x / maxf(vp_size.y, 1.0)
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_run_r1()

func _draw_frame() -> bool:
	if not server.gpu_cull_dispatch():
		return false
	if server.gpu_cull_get_visible_count() != INSTANCE_COUNT:
		return false
	if not server.gpu_visibility_dispatch():
		return false
	if not server.gpu_mesh_batch_dispatch():
		return false
	if not server.gpu_material_draw_lights():
		return false
	return true

func _snapshot_lists() -> PackedInt32Array:
	var out := PackedInt32Array()
	out.resize(3456 * 17)
	var idx := 0
	for tz in 24:
		for ty in 9:
			for tx in 16:
				var dbg: PackedInt32Array = server.gpu_light_debug_cluster(tx, ty, tz)
				var cnt := 0
				if dbg.size() > 0:
					cnt = dbg[0]
					var have := dbg.size() - 1
					var take := mini(cnt, mini(16, have))
					for k in take:
						out[idx + 1 + k] = dbg[1 + k]
				out[idx] = cnt
				idx += 17
	return out

func _run_r1() -> void:
	# --- A) OFF frame ---
	server.gpu_light_set_normal_cone(false)
	if not _draw_frame():
		_fail(3010, "draw frame OFF")
		return
	var px_off := server.gpu_raster_read_pixels()
	if px_off.size() != RASTER_W * RASTER_H * 4:
		_fail(3011, "pixels_off size")
		return
	var st_off: Dictionary = server.gpu_light_get_stats()
	var a_off := int(st_off["assignments"])
	var t_off := int(st_off["clusters_touched"])
	var o_off := int(st_off["overflows"])
	print("GNE 018-REV: OFF stats touched=", t_off, " assignments=", a_off, " overflows=", o_off)
	# Scene parity with the frozen v18 evidence (lists + raster identical path).
	if a_off != 11348 or t_off != 2841 or o_off != 0:
		_fail(3012, "OFF parity mismatch (expected 2841/11348/0) got " + str(t_off) + "/" + str(a_off) + "/" + str(o_off))
		return
	var lists_off := _snapshot_lists()
	# --- B) slab-only reference (flag on, cones cleared = no-op prefilter) ---
	server.gpu_light_set_normal_cone(true)
	server.gpu_light_cones_clear()
	if not _draw_frame():
		_fail(3013, "draw frame SLAB")
		return
	var st_slab: Dictionary = server.gpu_light_get_stats()
	var a_slab := int(st_slab["assignments"])
	print("GNE 018-REV: SLAB stats assignments=", a_slab)
	# --- C) ON frame (real cones from the OFF/SLAB raster) ---
	if not _draw_frame():
		_fail(3014, "draw frame ON")
		return
	var px_on := server.gpu_raster_read_pixels()
	if px_on.size() != RASTER_W * RASTER_H * 4:
		_fail(3015, "pixels_on size")
		return
	var st_on: Dictionary = server.gpu_light_get_stats()
	var a_on := int(st_on["assignments"])
	var t_on := int(st_on["clusters_touched"])
	var o_on := int(st_on["overflows"])
	var dc := a_slab - a_on
	print("GNE 018-REV: ON stats touched=", t_on, " assignments=", a_on, " overflows=", o_on, " dc=", dc)
	var lists_on := _snapshot_lists()
	var ep := server.gpu_light_cone_epochs()
	var age_max := int(ep[3]) if ep.size() >= 4 else -1
	print("GNE 018-REV: epochs=", ep, " age_max=", age_max)
	if age_max != 1:
		_fail(3016, "R7 cone source age " + str(age_max) + " != 1")
		return
	# --- D) cluster diff ---
	var changed := PackedByteArray()
	changed.resize(3456)
	var dropped := {}
	var dropped_total := 0
	for c in 3456:
		var b := c * 17
		var diff := false
		var dlist := PackedInt32Array()
		var cnt_off := lists_off[b]
		var cnt_on := lists_on[b]
		var set_on := {}
		for k in cnt_on:
			set_on[lists_on[b + 1 + k]] = true
		for k in cnt_off:
			var lid := lists_off[b + 1 + k]
			if not set_on.has(lid):
				dlist.append(lid)
		if dlist.size() > 0:
			dropped[c] = dlist
			dropped_total += dlist.size()
		if cnt_off != cnt_on:
			diff = true
		else:
			for k in cnt_off:
				if lists_off[b + 1 + k] != lists_on[b + 1 + k]:
					diff = true
					break
		if diff:
			changed[c] = 1
	print("GNE 018-REV: changed_clusters=", changed.count(1), " dropped_total=", dropped_total)
	# --- E) pixel diff + confinement + signature ---
	var normals := server.gpu_raster_read_normal_all()
	var viewz := server.gpu_raster_read_viewz_all()
	if normals.size() != RASTER_W * RASTER_H * 4 or viewz.size() != RASTER_W * RASTER_H:
		_fail(3017, "full-frame readback sizes")
		return
	var tanv: float = tan(deg_to_rad(camera.fov * 0.5))
	var aspect: float = vp_aspect
	var outside := 0
	var unexplained := 0
	var diff_px := 0
	var max_delta := 0
	var cam_t: Transform3D = camera.global_transform
	var light_positions := PackedVector3Array()
	for q in POINTS:
		light_positions.append(q[0])
	for s in SPOTS:
		light_positions.append(s[0])
	var report_budget := 8
	for y in RASTER_H:
		var row := y * RASTER_W
		for x in RASTER_W:
			var o := (row + x) * 4
			var d := maxi(abs(px_on[o] - px_off[o]), maxi(abs(px_on[o + 1] - px_off[o + 1]), abs(px_on[o + 2] - px_off[o + 2])))
			if d == 0:
				continue
			diff_px += 1
			if d > max_delta:
				max_delta = d
			# cluster of this pixel (fragment semantics)
			var tx := clampi(int((float(x) + 0.5) / RASTER_W * 16.0), 0, 15)
			var ty := clampi(8 - int((float(y) + 0.5) / RASTER_H * 9.0), 0, 8)
			var vz := viewz[row + x]
			var cid := -1
			if vz < 0.0:
				var lin := -vz
				var ndx := (float(x) + 0.5) / RASTER_W * 2.0 - 1.0
				var ndy := 1.0 - (float(y) + 0.5) / RASTER_H * 2.0
				var dxf := ndx * aspect * tanv
				var dyf := ndy * tanv
				var dist := lin * sqrt(dxf * dxf + dyf * dyf + 1.0)
				var tz := clampi(int(floor(24.0 * log(dist / 300.0) / log(4000.0 / 300.0))), 0, 23)
				cid = tz * 144 + ty * 16 + tx
			var cnt_off := lists_off[cid * 17] if cid >= 0 else 0
			var cnt_on := lists_on[cid * 17] if cid >= 0 else 0
			if cid < 0 or changed[cid] == 0:
				outside += 1
				if report_budget > 0:
					report_budget -= 1
					print("GNE 018-REV: DIFF outside changed-region at ", x, ",", y, " d=", d, " cid=", cid, " vz=", vz)
				continue
			# signature: every dropped light must face away at this pixel
			if dropped.has(cid):
				var dworld: Vector3 = Vector3.ZERO
				var vz2 := viewz[row + x]
				var lin2 := -vz2
				var ndx2 := (float(x) + 0.5) / RASTER_W * 2.0 - 1.0
				var ndy2 := 1.0 - (float(y) + 0.5) / RASTER_H * 2.0
				var dxf2 := ndx2 * aspect * tanv
				var dyf2 := ndy2 * tanv
				var nrm2 := sqrt(dxf2 * dxf2 + dyf2 * dyf2 + 1.0)
				var vp3 := Vector3(dxf2 * lin2 / nrm2, dyf2 * lin2 / nrm2, -lin2)
				dworld = cam_t * vp3
				var n3 := Vector3(normals[(row + x) * 4], normals[(row + x) * 4 + 1], normals[(row + x) * 4 + 2])
				var bad := 0
				for lid in dropped[cid]:
					var to_l: Vector3 = light_positions[lid] - dworld
					if to_l.length() < 1e-4:
						continue
					var ndl := n3.dot(to_l.normalized())
					if ndl > 0.03:
						bad += 1
				if bad > 0:
					unexplained += 1
					if report_budget > 0:
						report_budget -= 1
						print("GNE 018-REV: DIFF unexplained at ", x, ",", y, " cid=", cid, " bad=", bad, " cnt_off=", cnt_off, " cnt_on=", cnt_on)
					if report_budget == 0:
						pass
	print("GNE 018-REV: diff_px=", diff_px, " max_delta=", max_delta, " outside=", outside, " unexplained=", unexplained)
	if outside > 0:
		_fail(3018, "diffs outside changed regions: " + str(outside))
		return
	if unexplained > 0:
		_fail(3019, "unexplained diffs (over-cull signature failed): " + str(unexplained))
		return
	# --- F) signature ---
	var sig := "v18-rev|lc=20|cc=%d|dc=%d|ot=%d|dp=%d|d1" % [t_on, dc, o_on, diff_px]
	print("GNE 018-REV: sig=", sig)
	print("GNE 018-REV: parity_off=", a_off, "/", t_off, " slab=", a_slab, " on=", a_on)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(3020, "sig file write failed")
			return
		f.store_line(sig)
		f.close()
	print("GNE 018-REV: PASS")
	get_tree().quit(0)

func _fail(code: int, msg: String) -> void:
	print("GNE 018-REV: FAIL code=", code, " ", msg)
	get_tree().quit(code)