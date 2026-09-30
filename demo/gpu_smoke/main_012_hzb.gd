extends Node
# GNE 012-HZB - clean HZB culling gate (Tier 2, path A: scene-only, no C++).
#
# What it measures, production path only (the 004 path is not used):
#   p1 = frustum survivors, p2 = frustum+HZB survivors, from ONE
#   gpu_visibility_prod_dispatch via gpu_hzb_get_phase_counts.
# Geometry: main_010's proven-visible 6 instances + one nearer wall occluder
# (same recipe as the frozen 012-rev closure scene, which is NOT touched).
# Steps: OFF sanity [6,6] -> wall ON (p1 must stay 6, p2 must drop) ->
# in-process repeat of the ON dispatch (byte-equal determinism) -> temporal
# OFF re-measure (REPORTED only, never gated) -> OFF control [6,6].
# Wall-clock around build+dispatch is evidence-only (KI-001: no GPU timers).
# Sig is integer-only so d1/d2 compare byte-exact.

const CAM_POS := Vector3(0, 0, 1700)
const WARMUP := 4
const PTS: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(0, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(320, 0, -700),
	Vector3(-380, 0, -840),
	Vector3(0, -420, -940),
]
const SCALES: Array[float] = [29.0, 45.0, 86.0, 36.0, 46.0, 64.0]
const WALL_MIN := Vector4(-400.0, -400.0, -550.0, 0.0)
const WALL_MAX := Vector4(400.0, 400.0, -450.0, 0.0)

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var frame := 0
var done := false
var sig_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(730, "server singleton is null"); return
	if not server.ensure_gpu_device():
		_fail(731, "no device"); return
	if not server.gpu_scene_create(6, 1.0):
		_fail(732, "gpu_scene_create(6)"); return
	if not server.gpu_scene_dispatch(6):
		_fail(733, "gpu_scene_dispatch(6)"); return
	if not server.gpu_mesh_create():
		_fail(734, "gpu_mesh_create"); return
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
	print("GNE 012-HZB: scene ready instances=6")

func _process(_delta: float) -> void:
	if done:
		return
	frame += 1
	if frame > WARMUP:
		_measure()
		return
	if not server.gpu_cull_dispatch():
		_fail(740, "warmup cull"); return
	if not server.gpu_mesh_batch_dispatch():
		_fail(741, "warmup batch"); return
	if not server.gpu_mesh_drawargs_finalize():
		_fail(742, "warmup finalize"); return
	if not server.gpu_mesh_indirect_draw():
		_fail(743, "warmup draw"); return

func _dispatch_counts() -> PackedInt32Array:
	if not server.gpu_visibility_prod_dispatch():
		return PackedInt32Array()
	return server.gpu_hzb_get_phase_counts()

func _measure() -> void:
	done = true
	if not server.gpu_hzb_prod_create():
		_fail(735, "gpu_hzb_prod_create"); return
	if not server.gpu_hzb_build():
		_fail(736, "arm build 1"); return
	if not server.gpu_hzb_build():
		_fail(737, "arm build 2"); return
	# STEP 0: occluders OFF -> empty pyramid -> [6,6].
	server.gpu_hzb_set_occluders([])
	if not server.gpu_hzb_build():
		_fail(738, "build off"); return
	var ph_off := _dispatch_counts()
	if ph_off.size() != 2:
		_fail(739, "counts unreadable (off)"); return
	print("GNE 012-HZB: off=[", ph_off[0], ",", ph_off[1], "]")
	if ph_off[0] != 6 or ph_off[1] != 6:
		_fail(750, "off sanity: expected [6,6], got [" + str(ph_off[0]) + "," + str(ph_off[1]) + "]"); return
	# STEP 1: wall ON, timed (wall-clock evidence only).
	server.gpu_hzb_set_occluders([WALL_MIN, WALL_MAX])
	var t0 := Time.get_ticks_usec()
	if not server.gpu_hzb_build():
		_fail(744, "build wall"); return
	var ph_on := _dispatch_counts()
	var t1 := Time.get_ticks_usec()
	if ph_on.size() != 2:
		_fail(745, "counts unreadable (wall)"); return
	var p1 := ph_on[0]
	var p2 := ph_on[1]
	print("GNE 012-HZB: wall p1=", p1, " p2=", p2, " build_dispatch_us=", t1 - t0)
	if p1 != 6:
		_fail(751, "geometry: frustum must keep 6, p1=" + str(p1)); return
	if p2 < 0 or p2 >= p1:
		_fail(752, "criterion: expected p2 < p1, got [" + str(p1) + "," + str(p2) + "]"); return
	var culled := p1 - p2
	var cullpm := culled * 1000 / p1
	print("GNE 012-HZB: culled=", culled, "/6 ratio=", float(culled) / float(p1), " per_mille=", cullpm)
	# STEP 2: in-process determinism - rebuild + re-dispatch, byte-equal.
	if not server.gpu_hzb_build():
		_fail(746, "build repeat"); return
	var ph_rep := _dispatch_counts()
	if ph_rep.size() != 2:
		_fail(747, "counts unreadable (repeat)"); return
	var det := ph_rep[0] == p1 and ph_rep[1] == p2
	print("GNE 012-HZB: repeat=[", ph_rep[0], ",", ph_rep[1], "] det=", det)
	if not det:
		_fail(753, "in-process determinism"); return
	# STEP 3: temporal OFF re-measure - REPORTED only, never gated.
	server.gpu_hzb_enable_temporal(false)
	if not server.gpu_hzb_build():
		_fail(748, "build temporal-off"); return
	var ph_toff := _dispatch_counts()
	server.gpu_hzb_enable_temporal(true)
	if ph_toff.size() != 2:
		_fail(749, "counts unreadable (temporal-off)"); return
	print("GNE 012-HZB: temporal_off=[", ph_toff[0], ",", ph_toff[1], "] (report only)")
	# STEP 4: negative control - occluders OFF again -> [6,6].
	server.gpu_hzb_set_occluders([])
	if not server.gpu_hzb_build():
		_fail(754, "build control"); return
	var ph_ctrl := _dispatch_counts()
	if ph_ctrl.size() != 2:
		_fail(755, "counts unreadable (control)"); return
	print("GNE 012-HZB: control=[", ph_ctrl[0], ",", ph_ctrl[1], "]")
	if ph_ctrl[0] != 6 or ph_ctrl[1] != 6:
		_fail(756, "control: expected [6,6]"); return
	var sig := "v12hzb|p1=%d|p2=%d|cullpm=%d|det=1|d1" % [p1, p2, cullpm]
	print("GNE 012-HZB: sig=", sig)
	print("sig d1: \"" + sig + "\"")
	print("sighzb: " + sig.replace("|", ";"))
	print("GNE 012-HZB: PASS")
	if sig_file != "":
		var fl := FileAccess.open(sig_file, FileAccess.WRITE)
		if fl != null:
			fl.store_string(sig + "\n")
	server.gpu_scene_destroy()
	get_tree().quit(0)

func _fail(code: int, msg: String) -> void:
	print("GNE 012-HZB: FAIL code=", code, " ", msg)
	get_tree().quit(code)
