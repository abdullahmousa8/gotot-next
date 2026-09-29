extends Node

# GNE-018: prove that a light outside the view frustum is removed from the
# cluster path, under a DUAL-CONDITION protocol.
#
# WHY DUAL CONDITION
# ------------------
# A pixel measurement alone cannot prove culling. Move a light far away and C
# tends to 0 simply through attenuation, 1/(1+d^2/r^2). A C of 0 is therefore
# ambiguous between "culled" and "merely far". So every step asserts BOTH:
#   1. MEMBERSHIP: the light id is absent from the target cluster column's light
#      list, read back with gpu_light_debug_cluster.
#   2. CONTRIBUTION: C_R falls to 0.000 +/- 0.5 LSB, matching chroma_B from the
#      016.cclus work.
# Only the pair distinguishes culling from distance decay. The protocol is not
# decorative: on the first run the rear-cull step produced C = 0.000 while
# membership stayed PRESENT, and that combination was correctly rejected
# instead of being read as a pass.
#
# HOW CULLING ACTUALLY HAPPENS HERE
# ---------------------------------
# The cull shader has no explicit frustum-plane test. Its only rejection test is
# sphere_vs_aabb against each cluster's view-space AABB, so "frustum culling"
# is emergent: a light outside the frustum falls outside every cluster box. The
# membership read observes that independently of the pixels.
#
# LIGHT MOVEMENT
# --------------
# gpu_light_update replaces the whole record from a dictionary (pos, color and
# range are required; intensity defaults to 1.0). There is no partial position
# write, so every move MUST re-supply color, range and intensity unchanged or
# the comparison stops being about culling. range is deliberately held at 1400
# in every state per owner decision - which also means a culling move has to
# clear the light's own radius.
#
# SCOPE: frustum only. Depth / Z-buffer culling is out of scope and remains
# blocked; the raster-depth path is unapproved.

const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)
const PTS: Array[Vector3] = [Vector3(0, 0, -600)]
const SCALES: Array[float] = [120.0]
const GRID_X := 16
const GRID_Y := 9
const GRID_Z := 24

const LIGHT_BASE := Vector3(0, 0, -300)
const LIGHT_RANGE := 1400.0
const LIGHT_COLOR := Color(1, 0, 0)
const LIGHT_INTENSITY := 0.35
# Side cull: well outside the horizontal half-extent at the light's depth while
# keeping range fixed. Rear cull: with range HELD at 1400, the displacement must
# EXCEED the range, otherwise the light's own sphere still reaches the front
# clusters. Camera sits at z=1700, so z=3300 is 1600 behind it = range + 200.
const LIGHT_SIDE := Vector3(6000, 0, -300)
const LIGHT_REAR := Vector3(0, 0, 3300)
# Rotation must keep the surface inside the frustum: at ~2240 view distance a
# 0.15 rad yaw swings the view axis ~340 units, past the 120-unit half-extent,
# emptying the window (body=0) and making the comparison vacuous.
const YAW := 0.02

const BASE_ALBEDO := Color(0.25, 0.25, 0.25)
const BASE_SPEC := Color(0.25, 0.25, 0.25)
const BASE_SHINE := 32.0
const ROUGHNESS := 0.5
const CH_SCALE := Vector2(0.0125, 0.0125)
# Reference cluster term from 016.cclus, and the pre-declared tolerance.
const C_REF := 27.000
const C_TOL := 0.5

var server: GneRenderServer
var camera: Camera3D
var tid_grey := -1
var fpx := Vector2.ZERO
var sig_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	_setup()

func _fail(code: int, msg: String) -> void:
	print("GNE 018.frus: FAIL code=", code, " ", msg)
	get_tree().quit(code)

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(7000, "server"); return
	if not server.ensure_gpu_device():
		_fail(7001, "device"); return
	if not server.gpu_scene_create(1, 1.0):
		_fail(7002, "scene_create"); return
	if not server.gpu_scene_dispatch(1):
		_fail(7003, "dispatch"); return
	if not server.gpu_mesh_create():
		_fail(7004, "mesh_create"); return
	server.gpu_scene_set_instance_transform(0, PTS[0], SCALES[0])
	server.gpu_scene_set_instance_mesh(0, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(7005, "batch"); return
	if not server.gpu_material_create():
		_fail(7006, "material_create"); return
	if not server.gpu_material_set_albedo(0, BASE_ALBEDO):
		_fail(7007, "albedo"); return
	if not server.gpu_material_set_params(0, ROUGHNESS, 0.0):
		_fail(7007, "params"); return
	if not server.gpu_material_set_specular(0, BASE_SPEC, BASE_SHINE):
		_fail(7007, "specular"); return
	# Null the global directional term: +Z is perpendicular to the face normal.
	if not server.gpu_material_set_light(Vector3(0, 0, 1)):
		_fail(7008, "set_light"); return
	if _spawn_light(LIGHT_BASE) != 0:
		_fail(7009, "light id"); return
	tid_grey = server.gpu_texture_load("res://assets/neutral_grey.gtex")
	if tid_grey < 0:
		_fail(7010, "payload load"); return
	if not server.gpu_texture_bind(0, 1, tid_grey):
		_fail(7011, "bind"); return
	camera = $Camera
	_set_camera(CAM_POS, 0.0)
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	if not _redraw():
		return
	var vp := server.gpu_scene_get_vp()
	var fc := PTS[0] + Vector3(0, 0, SCALES[0] * 0.5)
	fpx = _proj_px(fc, vp)
	_measure()

func _spawn_light(pos: Vector3) -> int:
	return server.gpu_light_create({"type": 0, "pos": pos, "range": LIGHT_RANGE,
		"color": LIGHT_COLOR, "intensity": LIGHT_INTENSITY})

# Move the existing light. The whole record must be re-supplied because
# gpu_light_update replaces it; range/color/intensity are held fixed so the only
# variable is the position.
func _move_light(pos: Vector3) -> bool:
	return server.gpu_light_update(0, {"type": 0, "pos": pos, "range": LIGHT_RANGE,
		"color": LIGHT_COLOR, "intensity": LIGHT_INTENSITY})

func _set_camera(pos: Vector3, yaw: float) -> void:
	camera.global_position = pos
	camera.rotation = Vector3(0.0, yaw, 0.0)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(7012, "cull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(7013, "vis"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(7014, "batch"); return false
	if not server.gpu_material_draw_lights():
		_fail(7015, "draw"); return false
	return true

# Absolute cluster term for the current state: identical configuration, only the
# light intensity differs (0.35 vs 0), so the neutral AMB*alb term cancels.
func _measure_c() -> Dictionary:
	if not server.gpu_material_set_maps(0, tid_grey, -1, -1, CH_SCALE):
		_fail(7016, "maps"); return {"c": 0.0, "body": 0}
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(7017, "int on"); return {"c": 0.0, "body": 0}
	if not _redraw():
		return {"c": 0.0, "body": 0}
	var t := _means(server.gpu_raster_read_pixels())
	if not server.gpu_light_set_intensity(0, 0.0):
		_fail(7017, "int off"); return {"c": 0.0, "body": 0}
	if not _redraw():
		return {"c": 0.0, "body": 0}
	var b := _means(server.gpu_raster_read_pixels())
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(7017, "int restore"); return {"c": 0.0, "body": 0}
	return {"c": t[0] - b[0], "body": b[3]}

# Is light 0 assigned to the cluster column the target pixel lives in? The depth
# slice tz is scanned rather than computed, because the slice formula lives in
# gne_cluster_index (cpp:1022) and duplicating it here would violate the
# 018-rev single-source rule. This makes the predicate "present in this tile
# column at ANY depth slice", which is conservative in the safe direction: it can
# only report presence more often than the fragment's own single slice would.
func _membership() -> Dictionary:
	var tx := int(fpx.x) / (RASTER_W / GRID_X)
	var ty := int(fpx.y) / (RASTER_H / GRID_Y)
	var zs: Array = []
	for tz in GRID_Z:
		var c := server.gpu_light_debug_cluster(tx, ty, tz)
		if c.size() > 1:
			for i in range(1, c.size()):
				if c[i] == 0:
					zs.append(tz)
					break
	return {"present": (zs.size() > 0), "tx": tx, "ty": ty, "zs": zs}

func _means(px: PackedByteArray) -> Array:
	var sum := [0.0, 0.0, 0.0]
	var n := 0
	for dy in range(-4, 5):
		for dx in range(-4, 5):
			var o: int = ((int(fpx.y) + dy) * RASTER_W + (int(fpx.x) + dx)) * 4
			if px[o + 3] > 0:
				n += 1
				sum[0] += px[o]
				sum[1] += px[o + 1]
				sum[2] += px[o + 2]
	if n == 0:
		return [0.0, 0.0, 0.0, 0]
	return [sum[0] / float(n), sum[1] / float(n), sum[2] / float(n), n]

func _measure() -> void:
	# --- step 1: reference, light inside the frustum ---
	var m1 := _membership()
	var r1 := _measure_c()
	print("GNE 018.frus: S1 ref      present=%s tz=%s C=%.3f body=%d" % [str(m1["present"]), str(m1["zs"]), r1["c"], r1["body"]])
	var ok1: bool = m1["present"] and absf(r1["c"] - C_REF) <= C_TOL and r1["body"] == 81

	# --- step 2a: repeatability control, no state change at all ---
	# This is the correct invariance control. The previous version asserted that
	# C is byte-stable under camera ROTATION, which is false: H2 = normalize(Ld+V)
	# makes the cluster specular view-dependent (cpp:1915), so a yaw legitimately
	# moves C. Determinism of the instrument is what must be bit-exact.
	var r1b := _measure_c()
	print("GNE 018.frus: S2a repeat  C=%.3f body=%d delta_vs_ref=%.3f" % [r1b["c"], r1b["body"], r1b["c"] - r1["c"]])
	var ok2a: bool = r1b["body"] == 81 and absf(r1b["c"] - r1["c"]) <= 0.0

	# --- step 2b: non-culling under rotation ---
	# Rotating the camera must NOT drop the light from the clusters. The C value
	# is expected to move (view-dependent specular) and is deliberately NOT gated
	# here; only membership is.
	_set_camera(CAM_POS, YAW)
	if not _redraw():
		return
	var m2 := _membership()
	var r2 := _measure_c()
	print("GNE 018.frus: S2b yaw=%.3f present=%s tz=%s C=%.3f body=%d" % [YAW, str(m2["present"]), str(m2["zs"]), r2["c"], r2["body"]])
	var ok2b: bool = m2["present"] and r2["body"] == 81
	_set_camera(CAM_POS, 0.0)
	if not _redraw():
		return
	var ok2: bool = ok2a and ok2b

	# --- step 3: lateral cull ---
	if not _move_light(LIGHT_SIDE):
		_fail(7018, "move side"); return
	if not _redraw():
		return
	var m3 := _membership()
	var r3 := _measure_c()
	print("GNE 018.frus: S3 side     present=%s tz=%s C=%.3f body=%d" % [str(m3["present"]), str(m3["zs"]), r3["c"], r3["body"]])
	var ok3: bool = (not m3["present"]) and absf(r3["c"]) <= C_TOL and r3["body"] == 81

	# --- step 4: rear cull, displacement exceeds the light's own range ---
	if not _move_light(LIGHT_REAR):
		_fail(7018, "move rear"); return
	if not _redraw():
		return
	var m4 := _membership()
	var r4 := _measure_c()
	print("GNE 018.frus: S4 rear     present=%s tz=%s C=%.3f body=%d" % [str(m4["present"]), str(m4["zs"]), r4["c"], r4["body"]])
	var ok4: bool = (not m4["present"]) and absf(r4["c"]) <= C_TOL and r4["body"] == 81

	# --- verdict ---
	var all_ok: bool = ok1 and ok2 and ok3 and ok4
	print("GNE 018.frus: cond ref=%s repeat=%s rot_keep=%s side=%s rear=%s" % [str(ok1), str(ok2a), str(ok2b), str(ok3), str(ok4)])
	var sig := "v018frus|1=%d,%d,%.3f|2a=%d,%.3f|2b=%d,%d,%.3f|3=%d,%d,%.3f|4=%d,%d,%.3f|%d%d%d%d%d" % [
		int(m1["present"]), r1["body"], r1["c"],
		r1b["body"], r1b["c"] - r1["c"],
		int(m2["present"]), r2["body"], r2["c"],
		int(m3["present"]), r3["body"], r3["c"],
		int(m4["present"]), r4["body"], r4["c"],
		int(ok1), int(ok2a), int(ok2b), int(ok3), int(ok4)]
	print("GNE 018.frus: sig=", sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(7019, "sig file"); return
		f.store_string(sig + "\n")
	server.gpu_scene_destroy()
	if all_ok:
		print("GNE 018.frus: GATE PASS")
		get_tree().quit(0)
	else:
		print("GNE 018.frus: GATE FAIL ref=", ok1, " rot=", ok2, " side=", ok3, " rear=", ok4)
		get_tree().quit(71)

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
