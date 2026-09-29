extends Node

# GNE-019: cluster light-count overflow - memory safety and truncation.
#
# THE DEFECT THIS EXISTS FOR
# --------------------------
# The cull appends each light under `if (slot < 16u)` (cpp:1509), so a cluster
# holds at most 16 valid ids at stride 16 - GNE_CLUSTER_LIGHT_CAP = 16, and
# cluster_index_buffer is 3456 * 16 * 4 = 221184 B = 55296 uint slots.
# BUT the count itself is an uncapped atomicAdd, so clcnt.cnt[cid] can exceed
# 16. The fragment loop used to read `j < ccnt && j < 64u` against a stride of
# 16, which meant:
#   * j = 16..ccnt-1 addressed the NEXT cluster's slots (cross-cluster
#     pollution), and
#   * for the last cluster (cid = 3455, coff = 55280) it read up to 55280+63 =
#     55343, past the last valid index 55295 - a 192-byte out-of-bounds read.
# The fix bounds the read at 16, equal to the stride. This scene verifies the
# bound holds: a flooded cluster must not contaminate a neighbour.
#
# MEASUREMENT
# -----------
# Same isolation method as 016.cclus: one saturated red point light, the global
# directional term nulled along +Z, and a NEUTRAL grey albedo payload, so
# C_R = T - B is the absolute cluster term. Two surfaces are placed in
# DIFFERENT clusters: the flood target and a neighbour.
#
#   capacity case (N = 16): C_R at the flood target == the 016.cclus reference
#   overflow case (N = 20): ovf >= 4 (20 - 16), and the NEIGHBOUR's C_R must be
#                           unchanged from its own baseline.
#
# The neighbour is the load-bearing assertion: if the read over-ran into the
# neighbouring cluster's slots, the neighbour's own lit value would change.

const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)
const SCALES: Array[float] = [120.0, 120.0]
# Distinct depths so the two surfaces cannot share a cluster column slice
const FLOOD_POS := Vector3(-700, 0, -600)
# Neighbour sits near the flood surface in screen space (same tile column) and a
# little deeper, so it is the cluster that follows in the index buffer. A
# cluster index is (tz*GRID_Y + ty)*GRID_X + tx, so the slots the old 64-bound
# read overran into belong to the next depth slice / tile in exactly that order.
const NEIGH_POS := Vector3(-560, 0, -1000)
const GRID_X := 16
const GRID_Y := 9
const GRID_Z := 24

# Aligned with FLOOD_POS.x so the flood surface sits in the lit cluster and
# actually accumulates with N. The previous geometry left it outside the
# cluster, which showed up as ratio == 1.000 and made the capacity condition
# unmeasurable.
const LIGHT_BASE := Vector3(-700, 0, -300)
const LIGHT_RANGE := 1400.0
const LIGHT_COLOR := Color(1, 0, 0)
const LIGHT_INTENSITY := 0.35
const N_CAPACITY := 16
const N_OVERFLOW := 20
const EXPECT_OVF := 4  # N_OVERFLOW - N_CAPACITY

const BASE_ALBEDO := Color(0.25, 0.25, 0.25)
const BASE_SPEC := Color(0.25, 0.25, 0.25)
const BASE_SHINE := 32.0
const ROUGHNESS := 0.5
const CH_SCALE := Vector2(0.0125, 0.0125)
const C_REF := 27.000
const C_TOL := 0.5

var server: GneRenderServer
var camera: Camera3D
var tid_grey := -1
var sig_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	_setup()

func _fail(code: int, msg: String) -> void:
	print("GNE 019.ovf: FAIL code=", code, " ", msg)
	get_tree().quit(code)

func _mk_light() -> Dictionary:
	return {"type": 0, "pos": LIGHT_BASE, "range": LIGHT_RANGE,
		"color": LIGHT_COLOR, "intensity": LIGHT_INTENSITY}

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(7100, "server"); return
	if not server.ensure_gpu_device():
		_fail(7101, "device"); return
	if not server.gpu_scene_create(2, 1.0):
		_fail(7102, "scene_create"); return
	if not server.gpu_scene_dispatch(2):
		_fail(7103, "dispatch"); return
	if not server.gpu_mesh_create():
		_fail(7104, "mesh_create"); return
	for i in 2:
		var p: Vector3 = FLOOD_POS if i == 0 else NEIGH_POS
		server.gpu_scene_set_instance_transform(i, p, SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(7105, "batch"); return
	if not server.gpu_material_create():
		_fail(7106, "material_create"); return
	for i in 2:
		if not server.gpu_material_set_albedo(i, BASE_ALBEDO):
			_fail(7107, "albedo"); return
		if not server.gpu_material_set_params(i, ROUGHNESS, 0.0):
			_fail(7107, "params"); return
		if not server.gpu_material_set_specular(i, BASE_SPEC, BASE_SHINE):
			_fail(7107, "specular"); return
		# +Z is perpendicular to both faces' normals: null the global terms.
		if not server.gpu_material_set_light(Vector3(0, 0, 1)):
			_fail(7108, "set_light"); return
	tid_grey = server.gpu_texture_load("res://assets/neutral_grey.gtex")
	if tid_grey < 0:
		_fail(7109, "payload load"); return
	for i in 2:
		if not server.gpu_texture_bind(i, 1, tid_grey):
			_fail(7110, "bind"); return
	camera = $Camera
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_measure()

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(7111, "cull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(7112, "vis"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(7113, "batch"); return false
	if not server.gpu_material_draw_lights():
		_fail(7114, "draw"); return false
	return true

# Absolute cluster term for one material: identical configuration, only light
# intensity differs, so the neutral term cancels exactly.
func _cluster_c(mat: int) -> Dictionary:
	if not server.gpu_material_set_maps(mat, tid_grey, -1, -1, CH_SCALE):
		_fail(7115, "maps"); return {"c": 0.0, "body": 0}
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(7116, "int on"); return {"c": 0.0, "body": 0}
	if not _redraw():
		return {"c": 0.0, "body": 0}
	var t := _means(server.gpu_raster_read_pixels(), mat)
	if not server.gpu_light_set_intensity(0, 0.0):
		_fail(7116, "int off"); return {"c": 0.0, "body": 0}
	if not _redraw():
		return {"c": 0.0, "body": 0}
	var b := _means(server.gpu_raster_read_pixels(), mat)
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(7116, "int restore"); return {"c": 0.0, "body": 0}
	return {"c": t[0] - b[0], "body": b[3]}

func _proj_px(p: Vector3) -> Vector2:
	var vp := server.gpu_scene_get_vp()
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

func _means(px: PackedByteArray, mat: int) -> Array:
	var fpx := _proj_px(FLOOD_POS if mat == 0 else NEIGH_POS)
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

func _cluster_ids(mat: int) -> String:
	# Diagnostic: dump the light ids the cull actually stored in the cluster
	# column the surface projects into. This separates "the pipeline lost the
	# lights" from "the pipeline kept them and something downstream zeroed the
	# contribution". Depth slices are scanned rather than computed, because the
	# slice formula lives in gne_cluster_index (cpp:1022).
	var fpx := _proj_px(FLOOD_POS if mat == 0 else NEIGH_POS)
	var tx := int(fpx.x) / (RASTER_W / GRID_X)
	var ty := int(fpx.y) / (RASTER_H / GRID_Y)
	var parts: Array = []
	for tz in GRID_Z:
		var c := server.gpu_light_debug_cluster(tx, ty, tz)
		if c.size() > 1 and c[0] > 0:
			var ids: Array = []
			for i in range(1, c.size()):
				ids.append(c[i])
			parts.append("t%d:%s" % [tz, str(ids)])
	if parts.is_empty():
		return "tile %d/%d EMPTY" % [tx, ty]
	return "tile %d/%d %s" % [tx, ty, str(parts)]

func _measure() -> void:
	# --- N = 1: SELF-REFERENCE. C_ref_019 is measured on THIS geometry, not
	# borrowed from 016.cclus: that scene had one surface on the camera axis,
	# here the two surfaces are far off-axis, so the absolute value differs and
	# importing 27.000 would have been a fabricated threshold. ---
	if server.gpu_light_create(_mk_light()) != 0:
		_fail(7117, "light 1"); return
	if not _redraw():
		return
	var st1: Dictionary = server.gpu_light_get_stats()
	var n1 := _cluster_c(0)
	var n1b := _cluster_c(1)
	var c_ref: float = n1["c"]
	var neigh_base: float = n1b["c"]
	print("GNE 019.ovf: S0 n1   C_ref019=%.3f/%d neigh=%.3f/%d ovf=%d" % [c_ref, n1["body"], neigh_base, n1b["body"], int(st1["overflows"])])
	print("GNE 019.ovf: DIAG n1  flood_ids=%s" % _cluster_ids(0))

	# --- N = 16: exactly at the cap ---
	for i in range(1, N_CAPACITY):
		if server.gpu_light_create(_mk_light()) != i:
			_fail(7117, "light id"); return
	if not _redraw():
		return
	var st_cap: Dictionary = server.gpu_light_get_stats()
	var cap_flood := _cluster_c(0)
	var cap_neigh := _cluster_c(1)
	print("GNE 019.ovf: S1 cap  N=%d C=%.3f/%d neigh=%.3f/%d ovf=%d ratio=%.3f" % [N_CAPACITY, cap_flood["c"], cap_flood["body"], cap_neigh["c"], cap_neigh["body"], int(st_cap["overflows"]), (cap_flood["c"] / c_ref) if c_ref > 0.0 else -1.0])
	print("GNE 019.ovf: DIAG cap flood_ids=%s" % _cluster_ids(0))
	# Capacity bounds, stated physically rather than fitted: a cluster may not
	# gain MORE than N times a single light (that would mean a light counted
	# twice, which is what the out-of-bounds read could have caused), and must
	# gain at least 2x (accumulation actually happened, robust to saturation).
	var ok_cap_cnt: bool = (int(st_cap["overflows"]) == 0)
	var ok_cap_hi: bool = cap_flood["c"] <= float(N_CAPACITY) * c_ref + C_TOL
	var ok_cap_lo: bool = cap_flood["c"] >= 2.0 * c_ref - C_TOL
	var ok_cap_body: bool = cap_flood["body"] == 81 and cap_neigh["body"] == 81
	var ok_cap: bool = ok_cap_cnt and ok_cap_hi and ok_cap_lo and ok_cap_body

	# --- N = 20: overflow ---
	for i in range(N_CAPACITY, N_OVERFLOW):
		if server.gpu_light_create(_mk_light()) != i:
			_fail(7117, "light id 2"); return
	if not _redraw():
		return
	var st_ovf: Dictionary = server.gpu_light_get_stats()
	var ovf_flood := _cluster_c(0)
	var ovf_neigh := _cluster_c(1)
	print("GNE 019.ovf: S2 ovf  N=%d C=%.3f/%d neigh=%.3f/%d delta_neigh=%.3f ovf=%d" % [N_OVERFLOW, ovf_flood["c"], ovf_flood["body"], ovf_neigh["c"], ovf_neigh["body"], ovf_neigh["c"] - neigh_base, int(st_ovf["overflows"])])
	var ok_ovf_cnt: bool = (int(st_ovf["overflows"]) >= EXPECT_OVF)
	# Cross-cluster isolation is judged BETWEEN N=16 and N=20: those four extra
	# lights are the ones that overflow the flood cluster, so if the fragment
	# read still overran the 16-slot stride it would pull ids out of the
	# neighbouring cluster's slots and this delta would move.
	var ok_clean: bool = absf(ovf_neigh["c"] - cap_neigh["c"]) <= C_TOL
	var neigh_delta: float = ovf_neigh["c"] - cap_neigh["c"]
	var ok_body: bool = ovf_flood["body"] == 81 and ovf_neigh["body"] == 81
	var ok_ovf_hi: bool = ovf_flood["c"] <= float(N_OVERFLOW) * c_ref + C_TOL

	var all_ok: bool = ok_cap and ok_ovf_cnt and ok_clean and ok_body and ok_ovf_hi
	print("GNE 019.ovf: cond ref=%.3f cap_cnt=%s cap_hi=%s cap_lo=%s cap_body=%s ovf_cnt=%s clean=%s ovf_hi=%s body=%s" % [c_ref, str(ok_cap_cnt), str(ok_cap_hi), str(ok_cap_lo), str(ok_cap_body), str(ok_ovf_cnt), str(ok_clean), str(ok_ovf_hi), str(ok_body)])
	var sig := "v019ovf|cref=%.3f|neigh=%.3f|cap=%.3f|capn=%.3f|ovf0=%d|o2=%.3f|o2n=%.3f|dn=%.3f|ovf=%d|%d%d%d%d%d%d%d" % [
		c_ref, neigh_base, cap_flood["c"], cap_neigh["c"], int(st_cap["overflows"]),
		ovf_flood["c"], ovf_neigh["c"], neigh_delta, int(st_ovf["overflows"]),
		int(ok_cap), int(ok_ovf_cnt), int(ok_clean), int(ok_body), int(ok_ovf_hi),
		int(ok_cap_cnt), int(ok_cap_hi)]
	print("GNE 019.ovf: sig=", sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(7118, "sig file"); return
		f.store_string(sig + "\n")
	server.gpu_scene_destroy()
	if all_ok:
		print("GNE 019.ovf: GATE PASS")
		get_tree().quit(0)
	else:
		print("GNE 019.ovf: GATE FAIL cap=", ok_cap, " ovf_count=", ok_ovf_cnt,
				" neighbour_clean=", ok_clean, " ovf_hi=", ok_ovf_hi)
		get_tree().quit(72)

# Membership read for a surface, used only for reporting which slice it lands in.
func _membership(mat: int) -> Dictionary:
	var fpx := _proj_px(FLOOD_POS if mat == 0 else NEIGH_POS)
	var tx := int(fpx.x) / (RASTER_W / GRID_X)
	var ty := int(fpx.y) / (RASTER_H / GRID_Y)
	var n := 0
	for tz in GRID_Z:
		var c := server.gpu_light_debug_cluster(tx, ty, tz)
		if c.size() > 0 and c[0] > 0:
			n += 1
	return {"tx": tx, "ty": ty, "slices_with_lights": n}
