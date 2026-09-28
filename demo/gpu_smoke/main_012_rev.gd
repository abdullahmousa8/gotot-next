extends Node
# GNE 012-revised — closure scene: criterion 2 (p2 < p1) + negative control.
#
# MEASUREMENT PATH (production): p1/p2 come from ONE gpu_visibility_prod_dispatch
# via gpu_hzb_get_phase_counts (phase1 = frustum only, phase2 = frustum + HZB).
# Both numbers come from the same dispatch, so there is no cross-dispatch
# counter to pollute (root D bypassed by construction). The old 004 path is
# NOT used for the criterion (it builds a different pyramid).
#
# GEOMETRY (main_010's proven recipe): camera (0,0,1700), near 300 / far 4000,
# 6 instances at positions main_010 proved frustum-visible (all 6 survive).
# One wall sits BETWEEN the camera and the instances, covering the center of
# the screen, so the HZB must remove the instances behind it. A farther wall
# can never occlude a nearer instance under max-inv semantics, so the wall
# must be nearer than every instance it is expected to cull.

const CAM_POS := Vector3(0, 0, 1700)
const WARMUP := 4
const RASTER_W := 1920
const RASTER_H := 1080

# Proven-visible positions/scales from main_010 (all 6 survive frustum cull).
const PTS: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(0, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(320, 0, -700),
	Vector3(-380, 0, -840),
	Vector3(0, -420, -940),
]
const SCALES: Array[float] = [29.0, 45.0, 86.0, 36.0, 46.0, 64.0]
# One wall between camera (z=1700) and instances: nearest face z=-450
# (distance 2150, inv=1850) beats every instance's nearest surface, and its
# screen rect covers the cluster center.
const WALL_MIN := Vector4(-400.0, -400.0, -550.0, 0.0)
const WALL_MAX := Vector4(400.0, 400.0, -450.0, 0.0)

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var frame := 0
var done := false
var p1 := -1
var p2 := -1
var p1_ctrl := -1
var p2_ctrl := -1
var p1_off := -1
var p2_off := -1
var hzb_levels := 0
var diag_valid := -1
var diag_coherent := false
var scan_prod: PackedInt32Array = PackedInt32Array()


func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(700, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(701, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(6, 1.0):
		_fail(702, "gpu_scene_create(6)")
		return
	if not server.gpu_scene_dispatch(6):
		_fail(703, "gpu_scene_dispatch(6)")
		return
	if not server.gpu_mesh_create():
		_fail(704, "gpu_mesh_create")
		return
	for i in 6:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	hzb_levels = server.gpu_hzb_get_level_count()
	print("GNE 012 rev: scene ready instances=6 levels=", hzb_levels, " warmup=", WARMUP)


func _process(_delta: float) -> void:
	if done:
		return
	frame += 1
	if frame > WARMUP:
		_measure()
		_finish()
		return
	# Warm-up: keep the raster path alive so the 004 pyramid has real depth.
	if not server.gpu_cull_dispatch():
		_fail(710, "gpu_cull_dispatch")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(712, "gpu_mesh_batch_dispatch")
		return
	if not server.gpu_mesh_drawargs_finalize():
		_fail(714, "gpu_mesh_drawargs_finalize")
		return
	if not server.gpu_mesh_indirect_draw():
		_fail(715, "gpu_mesh_indirect_draw")
		return


# The measurement, production path, with the occlusion setting as the only
# variable. One gpu_visibility_prod_dispatch per step; p1/p2 are the phase
# counts from THAT dispatch, so occluders ON/OFF is the only difference.
func _measure() -> void:
	if not server.gpu_hzb_prod_create():
		_fail(705, "gpu_hzb_prod_create")
		return
	# Arm the temporal chain: 1st build is conservative (valid=0 by design),
	# 2nd+ builds set valid=1 (the C++ fix under test).
	if not server.gpu_hzb_build():
		_fail(706, "gpu_hzb_build (arm 1)")
		return
	if not server.gpu_hzb_build():
		_fail(707, "gpu_hzb_build (arm 2)")
		return

	# --- STEP 0: occluders OFF -> pyramid empty -> phase2 must equal phase1 ---
	server.gpu_hzb_set_occluders([])
	if not server.gpu_hzb_build():
		_fail(708, "gpu_hzb_build (off)")
		return
	if not server.gpu_visibility_prod_dispatch():
		_fail(710, "gpu_visibility_prod_dispatch (off)")
		return
	var ph_off: PackedInt32Array = server.gpu_hzb_get_phase_counts()
	if ph_off.size() != 2:
		_fail(719, "phase counts unreadable (off)")
		return
	p1_off = ph_off[0]
	p2_off = ph_off[1]

	# --- STEP 1: wall ON -> pyramid holds the wall -> must cull ---
	server.gpu_hzb_set_occluders([WALL_MIN, WALL_MAX])
	if not server.gpu_hzb_build():
		_fail(711, "gpu_hzb_build (wall)")
		return
	if not server.gpu_visibility_prod_dispatch():
		_fail(712, "gpu_visibility_prod_dispatch (wall)")
		return
	var ph_on: PackedInt32Array = server.gpu_hzb_get_phase_counts()
	if ph_on.size() != 2:
		_fail(719, "phase counts unreadable (wall)")
		return
	p1 = ph_on[0]
	p2 = ph_on[1]
	# Direct writer evidence: the flat pyramid buffer the phase-2 reader eats.
	scan_prod = server.gpu_hzb_dbg_scan_buffer(0)

	# --- STEP 2: negative control, occluders OFF again ---
	server.gpu_hzb_set_occluders([])
	if not server.gpu_hzb_build():
		_fail(713, "gpu_hzb_build (control)")
		return
	if not server.gpu_visibility_prod_dispatch():
		_fail(714, "gpu_visibility_prod_dispatch (control)")
		return
	var ph_ctrl: PackedInt32Array = server.gpu_hzb_get_phase_counts()
	if ph_ctrl.size() != 2:
		_fail(719, "phase counts unreadable (control)")
		return
	p1_ctrl = ph_ctrl[0]
	p2_ctrl = ph_ctrl[1]
	# The UBO flag the phase-2 test actually sees + the coherence signal.
	diag_valid = server.gpu_hzb_dbg_valid()
	diag_coherent = server.gpu_hzb_get_coherent()
	# CPU simulation of the phase-2 math for probe instance 0 (same UBO):
	# [j, level, tx, ty, max_inv, sphere_inv, vis, r_px].
	var sim: PackedInt32Array = server.gpu_hzb_dbg_sim2()
	print("GNE 012 rev: DIAG sim2_j0=", sim.slice(0, 8))


func _finish() -> void:
	done = true
	print("GNE 012 rev: levels=", hzb_levels, " off=[", p1_off, ",", p2_off, "] wall p1(frustum)=", p1, " p2(occluders)=", p2, " ctrl=[", p1_ctrl, ",", p2_ctrl, "] instances=6")
	print("GNE 012 rev: DIAG hzb_valid=", diag_valid, " coherent=", diag_coherent, " scan_prod=", scan_prod)
	if p1_off != 6 or p2_off != 6:
		_fail(720, "empty-pyramid sanity FAILED: expected [6,6], got [" + str(p1_off) + "," + str(p2_off) + "]")
		return
	if p1 != 6:
		_fail(721, "geometry invalid: frustum must keep all 6 (p1=" + str(p1) + ")")
		return
	# CRITERION 2: occlusion must strictly reduce the visible set.
	if p2 < 0 or p2 >= p1:
		_fail(722, "criterion 2 FAILED: expected p2 < p1, got p1=" + str(p1) + " p2=" + str(p2))
		return
	print("GNE 012 rev: C2 criterion OK p2<p1 (p1=", p1, " p2=", p2, ")")
	# NEGATIVE CONTROL: with no occluders the HZB must cull nothing.
	if p1_ctrl != 6 or p2_ctrl != 6:
		_fail(723, "negative control FAILED: expected [6,6], got [" + str(p1_ctrl) + "," + str(p2_ctrl) + "]")
		return
	print("GNE 012 rev: C2 control OK [6,6]")
	print("GNE 012 rev: sig=v12-rev levels=", hzb_levels, " p1=", p1, " p2=", p2, " ctrl=", p1_ctrl)
	print("GNE 012 rev: PASS")
	get_tree().quit(0)


func _fail(code: int, msg: String) -> void:
	print("GNE 012 rev: FAIL code=", code, " ", msg)
	get_tree().quit(code)

