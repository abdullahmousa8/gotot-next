extends Node

# GOTOT-008A - Real Geometry Proof (independent mesh path)
# TEMPORARY CPU READBACK BRIDGE - NOT FINAL RENDERING PATH
#
# Proves the existing GOTOT GPU scene can render REAL 3D GEOMETRY (a real cube
# mesh with a real vertex buffer, real index buffer, real vertex format and an
# indexed INDIRECT draw) instead of the GOTOT-005 billboard quads.
#
# This is an ADDITIVE path. It does not replace the billboard path, the quad
# index buffer, the existing raster pipeline or any existing 001A-007A API.
#
# Pipeline: GPU Scene -> existing cull -> existing compact buffer ->
#           existing instance indexing -> REAL MESH PATH -> indexed indirect draw
#           -> existing framebuffer -> CPU readback -> ImageTexture -> TextureRect

const INSTANCE_COUNT := 100000
const SPREAD := 12000.0
const FRAME_LIMIT := 240
const PRINT_EVERY := 30
const RASTER_W := 1920  # Fixed GOTOT raster target width (documented limitation).
const RASTER_H := 1080  # Fixed GOTOT raster target height (documented limitation).
const ORBIT_RADIUS := 800.0
const ORBIT_HEIGHT := 400.0

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var angle := 0.0
var want_shot := false
var shot_done := false

var visible_min := -1
var visible_max := -1
var prev_pixels := PackedByteArray()
var changed_frames := 0
var compared_frames := 0

var green_min := -1
var green_max := -1
var green_exact := -1
var magenta_exact := -1
var last_args := PackedInt32Array()


func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(50, "server singleton is null")
		return

	if not server.ensure_gpu_device():
		_fail(51, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display

	if not server.gpu_scene_create(INSTANCE_COUNT, SPREAD):
		_fail(52, "gpu_scene_create")
		return

	if not server.gpu_scene_dispatch(8):
		_fail(53, "gpu_scene_dispatch")
		return

	# ---- GOTOT-008A: independent real-mesh path ----
	if not server.gpu_mesh_create():
		_fail(54, "gpu_mesh_create")
		return

	var vcount := server.gpu_mesh_get_vertex_count()
	var icount := server.gpu_mesh_get_index_count()
	print("GOTOT-NEXT 008A: mesh created vertices=", vcount, " indices=", icount)
	if vcount != 8:
		_fail(55, "unexpected vertex count " + str(vcount))
		return
	if icount != 36:
		_fail(56, "unexpected index count " + str(icount))
		return

	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT 008A: scene ready instances=", INSTANCE_COUNT)


func _process(delta: float) -> void:
	frame += 1
	if frame > FRAME_LIMIT:
		return
	if shot_done:
		return

	# ---- Real moving Camera3D (deterministic orbit), same as 007A ----
	angle += delta * (TAU / 30.0)
	camera.global_position = Vector3(cos(angle) * ORBIT_RADIUS, ORBIT_HEIGHT, sin(angle) * ORBIT_RADIUS)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	# ---- Consume real viewport dimensions + real camera ----
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	# ---- Existing GPU pipeline (unchanged) ----
	if not server.gpu_cull_dispatch():
		_fail(57, "gpu_cull_dispatch")
		return
	var visible := server.gpu_cull_get_visible_count()

	if not server.gpu_visibility_dispatch():
		_fail(58, "gpu_visibility_dispatch")
		return

	# ---- REAL MESH PATH ----
	if not server.gpu_mesh_drawargs_finalize():
		_fail(59, "gpu_mesh_drawargs_finalize")
		return

	# Objective evidence: instance_count must come from the existing visible count.
	last_args = server.gpu_drawargs_read()
	if last_args.size() != 5 or last_args[0] != 36 or last_args[1] != visible:
		_fail(60, "mesh indirect args mismatch " + str(last_args) + " visible=" + str(visible))
		return

	if not server.gpu_mesh_indirect_draw():
		_fail(61, "gpu_mesh_indirect_draw")
		return

	# ---- Bridge: CPU readback (same as 007A) ----
	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(62, "raster readback size " + str(pixels.size()))
		return

	# ---- Objective evidence: exact green/magenta counts are taken on the
	# periodic report frames and on the final frame. A full 2M-pixel scan every
	# frame would dominate frame time in GDScript, so it is not done per frame.

	# ---- Display: pixels -> Godot Image -> ImageTexture -> TextureRect ----
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

	# ---- Dynamic visibility + image tracking ----
	if visible_min == -1 or visible < visible_min:
		visible_min = visible
	if visible_max == -1 or visible > visible_max:
		visible_max = visible

	if prev_pixels.size() == pixels.size():
		var diff := 0
		var j := 0
		while j < pixels.size():
			if pixels[j] != prev_pixels[j]:
				diff += 1
			j += 4 * 64  # sample every 64th pixel
		compared_frames += 1
		if diff > 0:
			changed_frames += 1
	prev_pixels = pixels

	if frame % PRINT_EVERY == 0:
		var counted := _count_exact(pixels)
		var green: int = counted[0]
		var magenta: int = counted[1]
		if green_min == -1 or green < green_min:
			green_min = green
		if green_max == -1 or green > green_max:
			green_max = green
		print("GOTOT-NEXT 008A")
		print("Instances: ", INSTANCE_COUNT)
		print("Visible: ", visible)
		print("Mesh: vertices=", server.gpu_mesh_get_vertex_count(), " indices=", server.gpu_mesh_get_index_count())
		print("Indirect args: ", last_args)
		print("Green pixels: ", green, " Magenta pixels: ", magenta)
		print("Viewport: ", int(vp_size.x), " x ", int(vp_size.y))

	if frame == FRAME_LIMIT:
		var exact := _count_exact(pixels)
		green_exact = exact[0]
		magenta_exact = exact[1]
		print("GOTOT-NEXT 008A: visible range ", visible_min, "..", visible_max)
		print("GOTOT-NEXT 008A: green range ", green_min, "..", green_max)
		print("GOTOT-NEXT 008A: EXACT green pixels=", green_exact, " EXACT magenta pixels=", magenta_exact)
		print("GOTOT-NEXT 008A: changed_frames=", changed_frames, " compared_frames=", compared_frames)
		if green_exact == 0:
			_fail(63, "no real mesh pixels rendered")
			return
		if magenta_exact > 0:
			_fail(64, "old billboard pixels present (magenta=" + str(magenta_exact) + ")")
			return
		want_shot = true


func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false

	# Evidence: capture the real game window (containing the TextureRect display).
	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GOTOT-NEXT 008A: window screenshot NOT EXECUTED (empty image)")
	else:
		var err := shot.save_png("C:/Users/opc/AppData/Local/Temp/opencode/gt_008_window.png")
		print("GOTOT-NEXT 008A: window screenshot saved=", err == OK)

	server.gpu_scene_destroy()
	print("GOTOT-NEXT 008A: PASS")
	get_tree().quit(0)


# Exact full-image scan, run once on the final frame. Returns [green, magenta].
# Green = real mesh pixels; magenta = the old GOTOT-005 billboard (must be 0).
func _count_exact(pixels: PackedByteArray) -> Array:
	var green := 0
	var magenta := 0
	var total := RASTER_W * RASTER_H
	for i in total:
		var r: int = pixels[i * 4]
		var g: int = pixels[i * 4 + 1]
		var b: int = pixels[i * 4 + 2]
		if g > 150 and r < 120 and b < 150:
			green += 1
		if r > 200 and b > 200:
			magenta += 1
	return [green, magenta]


func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT 008A: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)
