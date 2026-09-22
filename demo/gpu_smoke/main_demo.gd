extends Node

# GOTOT-011 INTERACTIVE DEMO
#
# Interactive: WASD + mouse fly-through of a volume with 64 distinct GPU meshes
# and 512 instances (8x8x8 cells, mesh_id = i % 64). R cycles the batch
# strategy live (PER_MESH -> GROUPED -> REORDERED). F12 saves a screenshot.
#
# Evidence mode: `res://main_demo.tscn -- --test` runs a scripted camera sweep
# (160 frames), then verifies batch grouping evidence, takes a window
# screenshot (demo_window.png) and exits 0.

const MESH_COUNT := 64
const INSTANCE_COUNT := 512
const RASTER_W := 1920
const RASTER_H := 1080
const STRATEGY_PER_MESH := 0
const STRATEGY_GROUPED := 1
const STRATEGY_REORDERED := 2
const STRATEGY_NAMES := ["PER_MESH", "GROUPED", "REORDERED"]
var _shot_path := "C:/Users/opc/AppData/Local/Temp/opencode/demo_window.png"

const SCALE := 24.0
var STEP_X := 130.0
var STEP_Y := 85.0
var STEP_Z := 170.0
var GRID_CX := -455.0        # +0*STEP_X => -455
var GRID_CY := -297.5
var GRID_CZ := -700.0        # back shell of the volume

var server: GototRenderServer
var camera: Camera3D
var controller: Node
var display: TextureRect
var hud: Label
var image_tex: ImageTexture

var strategy := STRATEGY_REORDERED
var test_mode := false
var frame := 0
var last_draw_counts := PackedInt32Array()
var dispatch_us := 0
var draw_us := 0
var want_shot := false
var shot_done := false

const TETRA_VERTS: Array[Vector3] = [
	Vector3(0.0, 0.8, 0.0),
	Vector3(-0.6, -0.4, 0.5),
	Vector3(0.6, -0.4, 0.5),
	Vector3(0.0, -0.4, -0.5),
]
const TETRA_INDICES: Array[int] = [0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2]
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

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a == "--test":
			test_mode = true
		elif a.begins_with("--png="):
			_shot_path = a.split("=")[1]
	_setup()

func _setup() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null or not server.ensure_gpu_device():
		_fail(200, "server/GPU unavailable")
		return

	camera = $Camera
	controller = $Camera
	display = $Overlay/Display
	hud = $Overlay/HUD
	camera.near = 300.0
	camera.far = 5000.0
	camera.global_position = Vector3(0, 180, 1200)
	camera.look_at(Vector3(0, 0, 0), Vector3.UP)
	controller.move_speed = 1100.0

	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(201, "gpu_scene_create")
		return
	if not server.gpu_scene_dispatch(21):
		_fail(202, "gpu_scene_dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(203, "gpu_mesh_create")
		return

	var tetra_verts := PackedVector3Array(TETRA_VERTS)
	var tetra_idx := PackedInt32Array(TETRA_INDICES)
	var octa_verts := PackedVector3Array(OCTA_VERTS)
	var octa_idx := PackedInt32Array(OCTA_INDICES)
	for m in range(1, MESH_COUNT):
		var mid := -1
		if m % 2 == 1:
			mid = server.gpu_mesh_create_from_arrays(tetra_verts, tetra_idx)
		else:
			mid = server.gpu_mesh_create_from_arrays(octa_verts, octa_idx)
		if mid != m:
			_fail(204, "mesh table entry " + str(m))
			return

	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, _pos(i), SCALE)
		server.gpu_scene_set_instance_mesh(i, i % MESH_COUNT)

	if not server.gpu_mesh_set_batch_strategy(strategy):
		_fail(205, "set_batch_strategy")
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT DEMO: ready strategy=", STRATEGY_NAMES[strategy],
			" meshes=", MESH_COUNT, " instances=", INSTANCE_COUNT, " test=", test_mode)

func _pos(i: int) -> Vector3:
	var x: int = i % 8
	var y: int = (i / 8) % 8
	var z: int = i / 64
	return Vector3(GRID_CX + x * STEP_X, GRID_CY + y * STEP_Y, GRID_CZ + z * STEP_Z)

func _process(delta: float) -> void:
	if shot_done or server == null:
		return
	frame += 1
	if test_mode and frame > 170:
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	if test_mode:
		_scripted_camera(delta)

	if not server.gpu_cull_dispatch():
		_fail(210, "gpu_cull_dispatch")
		return
	var visible := server.gpu_cull_get_visible_count()
	if not server.gpu_visibility_dispatch():
		_fail(211, "gpu_visibility_dispatch")
		return

	var t0 := Time.get_ticks_usec()
	if not server.gpu_mesh_batch_dispatch():
		_fail(212, "gpu_mesh_batch_dispatch")
		return
	dispatch_us = Time.get_ticks_usec() - t0
	last_draw_counts = server.gpu_mesh_get_draw_counts()

	var t1 := Time.get_ticks_usec()
	if not server.gpu_mesh_batch_draw():
		_fail(213, "gpu_mesh_batch_draw")
		return
	draw_us = Time.get_ticks_usec() - t1

	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(214, "pixels")
		return
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels))
	else:
		image_tex.update(Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels))
	display.texture = image_tex

	var batches := 0
	for c in last_draw_counts:
		if c > 0:
			batches += 1
	hud.data = {
		visible = visible,
		total = INSTANCE_COUNT,
		meshes = MESH_COUNT,
		batches = batches,
		groups = server.gpu_mesh_get_batch_group_count(),
		draw_calls = server.gpu_mesh_get_draw_call_count(),
		strategy_name = STRATEGY_NAMES[strategy],
		dispatch_ms = dispatch_us / 1000.0,
		draw_ms = draw_us / 1000.0,
	}

	_handle_keys()

	if test_mode and frame == 160:
		_finalize(pixels, visible, batches)

func _handle_keys() -> void:
	if test_mode:
		return
	if Input.is_key_pressed(KEY_R):
		if not _r_pressed:
			_r_pressed = true
			strategy = (strategy + 1) % 3
			server.gpu_mesh_set_batch_strategy(strategy)
			print("GOTOT-NEXT DEMO: strategy -> ", STRATEGY_NAMES[strategy])
	elif _r_pressed:
		_r_pressed = false
	if Input.is_key_pressed(KEY_F12):
		if not _f12_pressed:
			_f12_pressed = true
			get_viewport().get_texture().get_image().save_png(_shot_path)
	else:
		_f12_pressed = false
	if Input.is_key_pressed(KEY_P):
		if not _p_pressed:
			_p_pressed = true
			print("GOTOT-NEXT DEMO: visible=", server.gpu_cull_get_visible_count(),
					" batches=", _distinct(), " groups=", server.gpu_mesh_get_batch_group_count(),
					" draw_calls=", server.gpu_mesh_get_draw_call_count())
	else:
		_p_pressed = false

var _r_pressed := false
var _f12_pressed := false
var _p_pressed := false

func _distinct() -> int:
	var n := 0
	for c in last_draw_counts:
		if c > 0:
			n += 1
	return n

var _sweep := 0.0
func _scripted_camera(delta: float) -> void:
	_sweep += delta
	var t := clampf(_sweep / 6.0, 0.0, 1.0)
	var yaw := lerpf(-0.9, 0.9, t)
	var r := 1500.0
	camera.global_position = Vector3(sin(yaw) * r * 0.6, 180.0 + cos(yaw * 2.0) * 120.0, cos(yaw) * r * 0.8)
	camera.look_at(Vector3(0, 0, -100), Vector3.UP)

func _finalize(pixels: PackedByteArray, visible: int, batches: int) -> void:
	var groups := server.gpu_mesh_get_batch_group_count()
	var dc := server.gpu_mesh_get_draw_call_count()
	var ic := server.gpu_mesh_get_indirect_count()
	var ok := true
	if visible <= 0:
		print("GOTOT-NEXT DEMO: FAIL visible=", visible)
		ok = false
	if batches < 10:
		print("GOTOT-NEXT DEMO: FAIL batches=", batches)
		ok = false
	if strategy >= STRATEGY_GROUPED and dc > 5:
		print("GOTOT-NEXT DEMO: FAIL draw_calls=", dc)
		ok = false
	if dc != ic:
		print("GOTOT-NEXT DEMO: FAIL draw_calls != indirect ", dc, "/", ic)
		ok = false
	if groups != (5 if strategy >= STRATEGY_GROUPED else MESH_COUNT):
		print("GOTOT-NEXT DEMO: FAIL groups=", groups)
		ok = false
	var order := server.gpu_mesh_get_batch_order()
	for i in order.size():
		if order[i] != i:
			print("GOTOT-NEXT DEMO: FAIL order broken at ", i)
			ok = false
			break

	# DET: freeze the camera and compare two full passes separated by a warm-up,
# so the temporal depth-feedback cull has converged before comparison.
	camera.global_position = Vector3(0, 180, 1000)
	camera.look_at(Vector3(0, 0, -350), Vector3.UP)
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	var warm := 2
	for w in warm:
		if not _run_pass():
			print("GOTOT-NEXT DEMO: FAIL DET warmup pass ", w)
			ok = false
	var base := server.gpu_raster_read_pixels()
	if not _run_pass():
		print("GOTOT-NEXT DEMO: FAIL DET compare pass")
		ok = false
	var pixels2 := server.gpu_raster_read_pixels()
	if base.size() != pixels.size() or pixels2 != base:
		print("GOTOT-NEXT DEMO: FAIL DET pixel mismatch base=", base.size(),
				" det=", pixels2.size())
		ok = false

	print("GOTOT-NEXT DEMO: evidence visible=", visible, " meshes=", MESH_COUNT,
			" batches=", batches, " groups=", groups, " draw_calls=", dc,
			" indirect=", ic, " strategy=", STRATEGY_NAMES[strategy])
	print("GOTOT-NEXT DEMO: dispatch_us=", dispatch_us, " draw_us=", draw_us,
			" fps=", Engine.get_frames_per_second())
	if ok:
		print("GOTOT-NEXT DEMO: EVIDENCE OK")
		want_shot = true
	else:
		_fail(220, "test evidence failed")

func _run_pass() -> bool:
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	if not server.gpu_cull_dispatch():
		return false
	if not server.gpu_visibility_dispatch():
		return false
	if not server.gpu_mesh_batch_dispatch():
		return false
	return server.gpu_mesh_batch_draw()

func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false
	var shot := get_viewport().get_texture().get_image()
	var err := shot.save_png(_shot_path)
	print("GOTOT-NEXT DEMO: window screenshot saved=", err == OK, " ", _shot_path)
	var srv := server
	server = null
	srv.gpu_scene_destroy()
	if test_mode:
		print("GOTOT-NEXT DEMO: PASS")
		get_tree().quit(0)

func _exit_tree() -> void:
	if server != null:
		server.gpu_scene_destroy()

func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT DEMO: FAIL code=", code, " ", msg)
	if server != null:
		var srv := server
		server = null
		srv.gpu_scene_destroy()
	if test_mode:
		get_tree().quit(code)
	else:
		set_process(false)