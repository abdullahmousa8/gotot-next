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