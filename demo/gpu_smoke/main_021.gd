extends Node
# GNE-021 benchmark city - CONSTRUCTION unit v1 (smoke run; not the frozen
# measurement). Night street grid, 256 lights (192 point + 64 spot), camera at
# street level inside the grid (spec_021 section 1.2 bounds).
# Prints: light stats flag-off vs flag-on (dc smoke), cluster occupancy sample.

const INSTANCE_COUNT := 56

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 1200.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(402, "gpu_scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "gpu_scene_dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(404, "gpu_mesh_create")
		return
	camera = $Camera
	camera.global_position = Vector3(0, 170, 2400)
	camera.rotation = Vector3.ZERO
	camera.near = 200.0
	camera.far = 5000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	# ---------------- geometry ----------------
	var idx := 0
	# ground slab: unit cube scaled 6000, top face at y = 0
	server.gpu_scene_set_instance_transform(idx, Vector3(0, -3000, -900), 6000.0); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	# main corridor buildings: two rows at x = +-480, z 900 .. -2520 step 380
	for side in [1.0, -1.0]:
		for r in range(10):
			var z := 900.0 - r * 380.0
			var s := 240.0 + float((r * 37) % 5) * 40.0
			server.gpu_scene_set_instance_transform(idx, Vector3(side * 480.0, s * 0.5, z), s); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	# cross-street blocks: z = -900 and -1700, x = +-900 / +-1350
	for zc in [-900.0, -1700.0]:
		for xx in [900.0, 1350.0, -900.0, -1350.0]:
			var s2 := 300.0 + float(int(absf(zc)) % 80)
			server.gpu_scene_set_instance_transform(idx, Vector3(xx, s2 * 0.5, zc), s2); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	# far skyline silhouette
	for r2 in range(12):
		var s3 := 420.0
		server.gpu_scene_set_instance_transform(idx, Vector3(-1500.0 + r2 * 280.0, s3 * 0.5, -2700.0 + float((r2 % 3) * 70)), s3); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	# park remaining slots far offscreen (deterministic)
	while idx < INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(idx, Vector3(0, -9000, 0), 1.0); server.gpu_scene_set_instance_mesh(idx, 0); idx += 1
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(405, "set_batch_strategy")
		return
	server.gpu_material_create()
	server.gpu_material_set_albedo(0, Color(0.55, 0.57, 0.60))
	server.gpu_material_set_params(0, 0.6, 0.0)
	server.gpu_material_set_specular(0, Color(0.2, 0.2, 0.2), 24.0)
	# ---------------- lights: 64 spots ----------------
	var want := 0
	# main street lamps: 2 columns x 20 = 40 (spot aims across the street)
	for side2 in [1.0, -1.0]:
		for k in range(20):
			var z2 := 820.0 - k * 180.0
			var p := Vector3(side2 * 300.0, 150.0, z2)
			var t := Vector3(-side2 * 300.0, 0.0, z2 - 70.0)
			if server.gpu_light_create(_mk_spot(p, t, 0.38, 0.66, Color(1.0, 0.92, 0.78), 3.0)) != want:
				_fail(410, "spot id " + str(want)); return
			want += 1
	# cross-street lamps: 2 rows x 12 = 24
	for zrow in [-900.0, -1700.0]:
		for k2 in range(12):
			var x2 := -1650.0 + k2 * 300.0
			if server.gpu_light_create(_mk_spot(Vector3(x2, 150.0, zrow), Vector3(x2, 0.0, zrow + 130.0), 0.38, 0.66, Color(0.95, 0.9, 0.85), 2.6)) != want:
				_fail(411, "spot id " + str(want)); return
			want += 1
	# ---------------- lights: 192 points ----------------
	# main building window lights: 20 buildings x 4 = 80
	for side3 in [1.0, -1.0]:
		for r3 in range(10):
			var z3 := 900.0 - r3 * 380.0
			var s4 := 240.0 + float((r3 * 37) % 5) * 40.0
			var fx: float = side3 * (480.0 - s4 * 0.5 + 6.0)
			for wy in [90.0, 175.0, 260.0, 340.0]:
				if server.gpu_light_create(_mk_point(Vector3(fx, wy, z3 + float(((int(wy) + r3) % 3) * 90 - 90)), 300.0, Color(1.0, 0.85, 0.6), 2.2)) != want:
					_fail(412, "point id " + str(want)); return
				want += 1
	# cross-block window lights: 12 blocks x 2 = 24
	for zc2 in [-900.0, -1700.0]:
		for xx2 in [900.0, 1350.0, -900.0, -1350.0]:
			for wy2 in [120.0, 250.0, 330.0]:
				if server.gpu_light_create(_mk_point(Vector3(xx2 + (60.0 if wy2 > 200.0 else -60.0), wy2, zc2 + 170.0), 280.0, Color(0.85, 0.9, 1.0), 2.0)) != want:
					_fail(413, "point id " + str(want)); return
				want += 1
	# skyline glow: 12
	for r4 in range(12):
		if server.gpu_light_create(_mk_point(Vector3(-1500.0 + r4 * 280.0, 200.0, -2600.0), 420.0, Color(0.9, 0.75, 0.95), 1.8)) != want:
			_fail(414, "point id " + str(want)); return
		want += 1
	# scattered street-level points to reach 192: 76 along corridors
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
	print("GNE 021: lights created=", want)
	# ---------------- smoke measurement ----------------
	server.gpu_light_set_normal_cone(false)
	if not _redraw():
		return
	var off_st: Dictionary = server.gpu_light_get_stats()
	print("GNE 021: off count=", off_st["count"], " touched=", off_st["clusters_touched"], " assign=", off_st["assignments"], " ovf=", off_st["overflows"])
	print("GNE 021: off_full=", off_st)
	server.gpu_light_set_normal_cone(true)
	server.gpu_light_cones_clear()
	if not _redraw():
		return
	var slab_st: Dictionary = server.gpu_light_get_stats()
	print("GNE 021: slab count=", slab_st["count"], " touched=", slab_st["clusters_touched"], " assign=", slab_st["assignments"], " ovf=", slab_st["overflows"])
	if not _redraw():
		return
	var on_st: Dictionary = server.gpu_light_get_stats()
	print("GNE 021: on  count=", on_st["count"], " touched=", on_st["clusters_touched"], " assign=", on_st["assignments"], " ovf=", on_st["overflows"])
	var a0 := int(off_st["assignments"]); var a1 := int(slab_st["assignments"]); var a2 := int(on_st["assignments"])
	if a0 > 0:
		print("GNE 021: slab_gain_pct=", "%.2f" % (100.0 * float(a1 - a0) / float(a0)), " cone_saved_pct=", "%.2f" % (100.0 * float(a1 - a2) / float(a1)), " dc_net_pct=", "%.2f" % (100.0 * float(a0 - a2) / float(a0)))
	# occupancy sample over near/mid clusters (tx step 2, ty step 2, tz 14..20)
	var sampled := 0; var nz := 0; var ge8 := 0; var ge12 := 0; var ge16 := 0; var mx := 0
	var cone_ok := 0
	var cone_samples := PackedFloat32Array()
	for tz in range(14, 21):
		for ty in range(0, 9, 2):
			for tx in range(0, 16, 2):
				var ids: PackedInt32Array = server.gpu_light_debug_cluster(tx, ty, tz)
				var cr := server.gpu_light_cone_read(tz * 144 + ty * 16 + tx)
				if cr.size() == 4 and cr[3] > 0.1:
					cone_ok += 1
					if cone_samples.size() < 12:
						cone_samples.append_array(cr)
				sampled += 1
				var c := ids.size()
				if c > 0: nz += 1
				if c >= 8: ge8 += 1
				if c >= 12: ge12 += 1
				if c >= 16: ge16 += 1
				if c > mx: mx = c
	print("GNE 021: occupancy sampled=", sampled, " nonzero=", nz, " ge8=", ge8, " ge12=", ge12, " ge16=", ge16, " max=", mx)
	print("GNE 021: cone_valid=", cone_ok, " of ", sampled)
	print("GNE 021: cone_sample=", cone_samples)
	server.gpu_scene_destroy()
	print("GNE 021: SMOKE DONE")
	get_tree().quit(0)

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(420, "gpu_cull_dispatch"); return false
	if not server.gpu_visibility_dispatch():
		_fail(421, "gpu_visibility_dispatch"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(422, "gpu_mesh_batch_dispatch"); return false
	if not server.gpu_material_draw_lights():
		_fail(423, "gpu_material_draw_lights"); return false
	return true

func _fail(code: int, msg: String) -> void:
	print("GNE 021: FAIL code=", code, " ", msg)
	get_tree().quit(code)
