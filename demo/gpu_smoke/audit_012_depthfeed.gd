extends Node

# AUDIT-ONLY (not a milestone, not registered in any harness/CVS).
# CLOSED 2026-09-30 (progress section 61): the hypothesis below is CONFIRMED by
# measurement, not by source reading. Measured arms (6 frames, arms on 3/4/5):
#   A depth_feed -> prod_dispatch, no occluders : p1=3 p2=3 TARGET VISIBLE,
#                                                pyr_nz_l0=0, depthfeed_pcount=11890
#   B AABB control, no feed                    : p1=3 p2=2 TARGET OCCLUDED,
#                                                pyr_nz_l0=47560, depthfeed_pcount=0
#   C prod_dispatch -> depth_feed -> prod      : p1=3 p2=3 TARGET VISIBLE, pyr_nz_l0=0
# So gpu_visibility_prod_dispatch() clears hzb_pyramid_data_buffer and rebuilds it
# from the AABB occluders, erasing the fed depth before phase-2 in BOTH orderings:
# in 012-revised the AABB list is the only effective occlusion producer and
# gpu_hzb_depth_feed is unreachable by construction. The scene is kept because the
# question recurs whenever scene-depth occlusion is proposed; re-run it before
# claiming a fix, and keep this header in sync with the result.
# Purpose: decide whether the 012 raster-depth feed (gpu_hzb_depth_feed, which
# writes hzb_pyramid_data_buffer from raster_viewz_texture) can reach the
# phase-2 occlusion decision at all, or whether the AABB-occluder path is the
# only effective producer. Source reading says gpu_visibility_prod_dispatch()
# clears hzb_pyramid_data_buffer and rebuilds it from occluders; this scene
# measures whether that makes depth-fed occlusion unreachable in every ordering.
#
# Scene: 3 instances, PER_MESH, single mesh.
#   0 BLOCKER  (0,0,-800)   scale 120  - large, directly in front
#   1 TARGET   (0,0,-1000)  scale 30   - small, dead behind BLOCKER
#   2 CONTROL  (600,0,-1000) scale 30  - beside, must stay visible
# One registered AABB occluder spans only BLOCKER's screen rect.
#
# Arms (each after the same settled draw order: cull -> batch -> build -> draw):
#   A  occluders=[]            depth_feed -> prod_dispatch   (depth-only)
#   B  occluders=[AABB]        (no depth_feed) prod_dispatch (positive control)
#   C  occluders=[]            prod_dispatch -> depth_feed -> prod_dispatch (order probe)
#
# Witnesses per arm: p1/p2, phase-2 membership of 0/1/2, pyramid nonzero count
# at level 0, depth-feed probe pcount, and a raster view-Z sample at TARGET's
# and CONTROL's projected pixels (raster validity).

const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)
const FRAMES := 6

const PTS: Array[Vector3] = [
	Vector3(0, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(600, 0, -1000),
]
const SCALES: Array[float] = [120.0, 30.0, 30.0]
const OCC_MIN := Vector4(-200.0, -200.0, -950.0, 0.0)
const OCC_MAX := Vector4(200.0, 200.0, -750.0, 0.0)

var server: GneRenderServer
var camera: Camera3D
var frame := 0

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(900, "server singleton null")
		return
	if not server.ensure_gpu_device():
		_fail(901, "no device")
		return
	if not server.gpu_scene_create(3, 1.0):
		_fail(902, "scene_create")
		return
	if not server.gpu_scene_dispatch(7):
		_fail(903, "scene_dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(904, "mesh_create")
		return
	for i in 3:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(905, "batch_strategy")
		return
	camera = $Camera
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	var vps := get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vps.x, vps.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	if not server.gpu_hzb_prod_create():
		_fail(906, "hzb_prod_create")
		return
	while frame < FRAMES:
		_frame()
		frame += 1
	print("AUDIT012|done frames=", frame)
	get_tree().quit(0)

func _frame() -> void:
	if not server.gpu_cull_dispatch():
		_fail(910, "cull")
		return
	if not server.gpu_visibility_dispatch():
		_fail(911, "visibility")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(912, "batch_dispatch")
		return
	if frame > 1 and not server.gpu_hzb_build():
		_fail(913, "hzb_build")
		return
	if not server.gpu_mesh_batch_draw():
		_fail(914, "batch_draw")
		return
	# arms on settled frames only
	if frame == 2:
		_raster_witness()  # after >=1 completed draw: is raster depth populated?
	if frame == 3:
		_arm("A_depth_first", false, true)
	elif frame == 4:
		_arm("B_aabb_control", true, false)
	elif frame == 5:
		_arm("C_depth_after", false, true, true)

func _arm(p_name: String, p_occluders: bool, p_depth_first: bool, p_depth_after := false) -> void:
	if p_occluders:
		server.gpu_hzb_set_occluders(PackedVector4Array([OCC_MIN, OCC_MAX]))
	else:
		server.gpu_hzb_set_occluders(PackedVector4Array())
	if p_depth_first:
		if not server.gpu_hzb_depth_feed():
			_fail(915, "depth_feed")
			return
	if not server.gpu_visibility_prod_dispatch():
		_fail(916, "prod_dispatch")
		return
	if p_depth_after:
		if not server.gpu_hzb_depth_feed():
			_fail(917, "depth_feed_after")
			return
		if not server.gpu_visibility_prod_dispatch():
			_fail(918, "prod_dispatch_after")
			return
	var counts := server.gpu_hzb_get_phase_counts()
	var p1 := counts[0]
	var p2 := counts[1]
	var cmp := server.gpu_compact_read()
	var n := mini(p2, cmp.size())
	var has0 := false
	var has1 := false
	var has2 := false
	for k in range(n):
		if cmp[k] == 0: has0 = true
		if cmp[k] == 1: has1 = true
		if cmp[k] == 2: has2 = true
	var scan := server.gpu_hzb_dbg_scan_buffer(0)
	var nz := scan[3] if scan.size() == 4 else -1
	var pb := server.gpu_hzb_dbg_probe()
	var pcount := pb[0] if pb.size() == 4 else -1
	print("AUDIT012|arm=", p_name, " p1=", p1, " p2=", p2,
		" blocker=", has0, " target=", has1, " control=", has2,
		" pyr_nz_l0=", nz, " depthfeed_pcount=", pcount, " coherent=", server.gpu_hzb_get_coherent())

func _raster_witness() -> void:
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		_fail(920, "vp")
		return
	var t := _proj_px(PTS[1], vp)
	var c := _proj_px(PTS[2], vp)
	var zt := server.gpu_raster_read_viewz(int(t.x), int(t.y))
	var zc := server.gpu_raster_read_viewz(int(c.x), int(c.y))
	print("AUDIT012|raster target_px=", int(t.x), ",", int(t.y), " viewz=", zt,
		" control_px=", int(c.x), ",", int(c.y), " viewz=", zc,
		" far_plane_sample=", server.gpu_raster_read_viewz(4, 4))

func _proj_px(p: Vector3, vp: PackedFloat32Array) -> Vector2:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return Vector2(-1, -1)
	return Vector2((cx / cw * 0.5 + 0.5) * float(RASTER_W), (0.5 - cy / cw * 0.5) * float(RASTER_H))

func _fail(code: int, msg: String) -> void:
	print("AUDIT012|FAIL code=", code, " ", msg)
	get_tree().quit(code)
